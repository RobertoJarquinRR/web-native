#!/usr/bin/env python3
"""Compare Chromium and native PNGs and write report.json/report.md.

Metrics follow docs/visual-parity.md:
  - SSIM on grayscale (BT.601 luma) with an 8x8 box window.
  - pixel diff: percentage of pixels with max(|dR|,|dG|,|dB|) > 8.
  - MAE: mean absolute channel difference.
  - region metrics for strict/text/background masks; excludes are ignored.
  - side-by-side, amplified diff and red overlay PNGs.

The harness is dev-only. It never touches the runtime, ABI or install size.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image, ImageDraw, ImageFont

import common as c

CANONICAL_COMMAND = (
    "powershell -NoProfile -ExecutionPolicy Bypass -File "
    "benchmarks\\visual-parity\\run_visual.ps1"
)
METHOD = {
    "ssim": (
        "Luma BT.601 (0.299R+0.587G+0.114B); media local box 8x8 con bordes "
        "reflejados; media sobre pixeles validos."
    ),
    "pixel_diff": (
        "Porcentaje de pixeles con max(|dR|,|dG|,|dB|) > 8. "
        "diff_bbox es [x0, y0, x1, y1] inclusivo."
    ),
    "mae": "Media de |d| por canal sobre pixeles validos.",
    "regions": "Cada region se recorta y se evalua con la mascara global de excludes.",
    "excludes": (
        "Los pixeles excluidos no cuentan en diff/MAE ni en las medias por region; "
        "la ventana SSIM 8x8 puede incluir vecinos del borde de una exclusion."
    ),
    "text": "Region text: SSIM/diff con umbral propio + bbox de tinta e IoU como diagnostico.",
}

TRIAGE = [
    "Layout: comparar bounding boxes del IR antes de mirar pixeles. Un offset de 1 px explica casi todo.",
    "Clip/scroll: comprobar si el contenido se sale o se recorta distinto.",
    "Color/fill: color plano, alpha y premultiplicado.",
    "Bordes/radius/sombra: diferencias locales en el borde, no en el relleno.",
    "Texto: forma, baseline y corte de linea; despues antialiasing.",
    "AA/rasterizado: solo si los pasos 1-5 estan limpios y la diferencia es de 1 px en bordes.",
]


# --------------------------------------------------------------------------
# image math


def load_rgb(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        return np.array(image.convert("RGB"), dtype=np.uint8)


def luma(rgb: np.ndarray) -> np.ndarray:
    data = rgb.astype(np.float64)
    return 0.299 * data[..., 0] + 0.587 * data[..., 1] + 0.114 * data[..., 2]


def box_mean(image: np.ndarray, win: int = 8) -> np.ndarray:
    """Box filter with reflected borders, preserving the input shape."""
    height, width = image.shape
    pad = win - 1
    mode = "reflect" if min(height, width) > pad else "edge"
    padded = np.pad(image.astype(np.float64), pad, mode=mode)
    integral = np.zeros((padded.shape[0] + 1, padded.shape[1] + 1), dtype=np.float64)
    integral[1:, 1:] = padded.cumsum(0).cumsum(1)
    total = (
        integral[win:, win:]
        - integral[:-win, win:]
        - integral[win:, :-win]
        + integral[:-win, :-win]
    )
    filtered = total / float(win * win)
    return filtered[pad : pad + height, pad : pad + width]


def ssim_map(gray_a: np.ndarray, gray_b: np.ndarray, win: int = 8) -> np.ndarray:
    c1 = (0.01 * 255.0) ** 2
    c2 = (0.03 * 255.0) ** 2
    mu_a = box_mean(gray_a, win)
    mu_b = box_mean(gray_b, win)
    sigma_a = np.maximum(box_mean(gray_a * gray_a, win) - mu_a * mu_a, 0.0)
    sigma_b = np.maximum(box_mean(gray_b * gray_b, win) - mu_b * mu_b, 0.0)
    sigma_ab = box_mean(gray_a * gray_b, win) - mu_a * mu_b
    numerator = (2.0 * mu_a * mu_b + c1) * (2.0 * sigma_ab + c2)
    denominator = (mu_a * mu_a + mu_b * mu_b + c1) * (sigma_a + sigma_b + c2)
    with np.errstate(divide="ignore", invalid="ignore"):
        result = numerator / denominator
    result = np.nan_to_num(result, nan=0.0, posinf=1.0, neginf=-1.0)
    return np.clip(result, -1.0, 1.0)


def parse_rect(rect: Any, label: str) -> tuple[int, int, int, int]:
    if not isinstance(rect, (list, tuple)) or len(rect) != 4:
        raise ValueError(f"{label}: rect must be [x, y, w, h], got {rect!r}")
    values = [int(round(float(v))) for v in rect]
    if values[2] <= 0 or values[3] <= 0:
        raise ValueError(f"{label}: rect must have positive width/height, got {rect!r}")
    return values[0], values[1], values[2], values[3]


def clip_rect(rect: tuple[int, int, int, int], width: int, height: int) -> tuple[int, int, int, int]:
    x, y, w, h = rect
    x0 = max(0, x)
    y0 = max(0, y)
    x1 = min(width, x + w)
    y1 = min(height, y + h)
    return x0, y0, max(0, x1 - x0), max(0, y1 - y0)


def valid_mask(
    width: int, height: int, excludes: list[dict[str, Any]]
) -> tuple[np.ndarray, list[dict[str, Any]]]:
    mask = np.ones((height, width), dtype=bool)
    applied: list[dict[str, Any]] = []
    for item in excludes:
        reason = str(item.get("reason", "")).strip()
        if not reason:
            raise ValueError(f"exclude {item.get('name')!r} has no reason")
        rect = parse_rect(item.get("rect"), f"exclude {item.get('name')}")
        x, y, w, h = clip_rect(rect, width, height)
        mask[y : y + h, x : x + w] = False
        applied.append(
            {
                "name": str(item.get("name", "exclude")),
                "rect": [x, y, w, h],
                "reason": reason,
            }
        )
    if not mask.any():
        raise ValueError("every pixel is excluded")
    return mask, applied


def region_mask(
    width: int, height: int, rect: tuple[int, int, int, int]
) -> tuple[np.ndarray, tuple[int, int, int, int]]:
    x, y, w, h = clip_rect(rect, width, height)
    mask = np.zeros((height, width), dtype=bool)
    if w > 0 and h > 0:
        mask[y : y + h, x : x + w] = True
    return mask, (x, y, w, h)


def compute_metrics(
    rgb_a: np.ndarray,
    rgb_b: np.ndarray,
    mask: np.ndarray,
    ssim_full: np.ndarray,
    diff_threshold: int,
) -> dict[str, Any]:
    if not mask.any():
        raise ValueError("metrics received an empty mask")
    diff = np.abs(rgb_a.astype(np.int16) - rgb_b.astype(np.int16))
    max_diff = diff.max(axis=2)
    values = diff[mask]
    max_values = max_diff[mask]
    hit_map = (max_diff > diff_threshold) & mask
    positions = np.where(hit_map)
    if positions[0].size:
        bbox = [
            int(positions[1].min()),
            int(positions[0].min()),
            int(positions[1].max()),
            int(positions[0].max()),
        ]
    else:
        bbox = None
    return {
        "ssim": float(ssim_full[mask].mean()),
        "pixel_diff_pct": float(hit_map.sum() / mask.sum() * 100.0),
        "mae": float(values.mean()),
        "mae_per_channel": [float(diff[..., channel][mask].mean()) for channel in range(3)],
        "diff_pixels": int(hit_map.sum()),
        "valid_pixels": int(mask.sum()),
        "diff_threshold": int(diff_threshold),
        "diff_bbox": bbox,
        "p99_channel_diff": float(np.percentile(max_values, 99)),
        "max_channel_diff": int(max_values.max()),
        "mean_rgb_reference": [float(rgb_a[..., channel][mask].mean()) for channel in range(3)],
        "mean_rgb_native": [float(rgb_b[..., channel][mask].mean()) for channel in range(3)],
    }


def _packed_rgb(pixels: np.ndarray) -> np.ndarray:
    data = pixels.astype(np.uint32)
    return (data[:, 0] << 16) | (data[:, 1] << 8) | data[:, 2]


def ink_geometry(rgb: np.ndarray, mask: np.ndarray, tolerance: int = 24) -> dict[str, Any] | None:
    pixels = rgb[mask]
    if pixels.size == 0:
        return None
    packed = _packed_rgb(pixels)
    values, counts = np.unique(packed, return_counts=True)
    background = values[int(counts.argmax())]
    bg = np.array(
        [
            (int(background) >> 16) & 0xFF,
            (int(background) >> 8) & 0xFF,
            int(background) & 0xFF,
        ],
        dtype=np.int16,
    )
    delta = np.abs(rgb.astype(np.int16) - bg).max(axis=2)
    ink = (delta > tolerance) & mask
    ys, xs = np.where(ink)
    if ys.size == 0:
        return {"background_rgb": bg.tolist(), "ink_pixels": 0, "bbox": None}
    return {
        "background_rgb": bg.tolist(),
        "ink_pixels": int(ink.sum()),
        "ink_pct": float(ink.sum() / mask.sum() * 100.0),
        "bbox": [int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())],
    }


def bbox_iou(a: list[int] | None, b: list[int] | None) -> float | None:
    if not a or not b:
        return None
    ax0, ay0, ax1, ay1 = a
    bx0, by0, bx1, by1 = b
    ix0, iy0 = max(ax0, bx0), max(ay0, by0)
    ix1, iy1 = min(ax1, bx1), min(ay1, by1)
    iw, ih = max(0, ix1 - ix0 + 1), max(0, iy1 - iy0 + 1)
    inter = iw * ih
    area_a = (ax1 - ax0 + 1) * (ay1 - ay0 + 1)
    area_b = (bx1 - bx0 + 1) * (by1 - by0 + 1)
    union = area_a + area_b - inter
    return float(inter / union) if union > 0 else 1.0


# --------------------------------------------------------------------------
# thresholds and status


def _threshold_block(config: dict[str, Any], fixture: dict[str, Any], mode: str) -> dict[str, Any]:
    defaults = config.get("defaults", {})
    global_block = defaults.get("global", {"min_ssim": 0.98, "max_diff_pct": 1.0})
    if mode == "background":
        base = dict(global_block)
        source = "defaults.global"
    else:
        base = dict(defaults.get("tolerances", {}).get(mode, {}))
        source = f"defaults.tolerances.{mode}"
    override = dict(fixture.get("tolerances", {}).get(mode, {}))
    merged = {**base, **override}
    if override:
        source += f" + fixture.tolerances.{mode}"
    return {
        "min_ssim": float(merged.get("min_ssim", 0.0)),
        "max_diff_pct": float(merged.get("max_diff_pct", 100.0)),
        "source": source,
    }


def _status_for(metrics: dict[str, Any], thresholds: dict[str, Any] | None) -> str:
    if thresholds is None:
        return "INFO"
    passed = (
        float(metrics["ssim"]) >= float(thresholds["min_ssim"])
        and float(metrics["pixel_diff_pct"]) <= float(thresholds["max_diff_pct"])
    )
    return "PASS" if passed else "FAIL"


def _global_thresholds(config: dict[str, Any]) -> dict[str, Any]:
    block = dict(config.get("defaults", {}).get("global", {"min_ssim": 0.98, "max_diff_pct": 1.0}))
    return {
        "min_ssim": float(block.get("min_ssim", 0.98)),
        "max_diff_pct": float(block.get("max_diff_pct", 1.0)),
        "source": "defaults.global",
    }


# --------------------------------------------------------------------------
# artifacts


def _font(size: int = 14):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # very old Pillow
        return ImageFont.load_default()


def compose_panels(
    panels: list[np.ndarray], labels: list[str], path: Path, header: int = 28, gap: int = 8
) -> None:
    height = max(panel.shape[0] for panel in panels)
    width = sum(panel.shape[1] for panel in panels) + gap * (len(panels) - 1)
    canvas = Image.new("RGB", (width, height + header), (16, 20, 28))
    draw = ImageDraw.Draw(canvas)
    font = _font(14)
    x = 0
    for panel, label in zip(panels, labels):
        draw.text((x + 8, 6), label, fill=(232, 236, 244), font=font)
        canvas.paste(Image.fromarray(panel.astype(np.uint8), mode="RGB"), (x, header))
        x += panel.shape[1] + gap
    path.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(path, format="PNG")


def diff_visuals(
    rgb_a: np.ndarray, rgb_b: np.ndarray, diff_threshold: int
) -> tuple[np.ndarray, np.ndarray]:
    diff = np.abs(rgb_a.astype(np.int16) - rgb_b.astype(np.int16))
    max_diff = diff.max(axis=2)
    amplified = np.clip(max_diff.astype(np.float64) * 4.0, 0, 255).astype(np.uint8)
    diff_rgb = np.dstack(
        [
            amplified,
            (amplified.astype(np.float64) * 0.30).astype(np.uint8),
            (amplified.astype(np.float64) * 0.12).astype(np.uint8),
        ]
    )
    gray = np.clip(luma(rgb_a) * 0.35, 0, 255).astype(np.uint8)
    overlay = np.dstack([gray, gray, gray])
    hit = max_diff > diff_threshold
    overlay[hit] = np.array([255, 64, 64], dtype=np.uint8)
    return diff_rgb, overlay


def _rel(path: Path, base: Path) -> str:
    try:
        return path.resolve().relative_to(base.resolve()).as_posix()
    except ValueError:
        return str(path.resolve())


_REDACTIONS: tuple[tuple[str, str], ...] = tuple(
    (str(prefix.resolve()), token)
    for prefix, token in (
        (c.repo_root(), "<repo>"),
        (Path.home(), "<home>"),
    )
)


def _redact_string(text: str) -> str:
    """Replace machine-specific absolute prefixes so reports can be committed."""
    result = text
    for prefix, token in _REDACTIONS:
        for variant in {prefix, prefix.replace("\\", "/"), prefix.replace("/", "\\")}:
            if not variant:
                continue
            result = re.sub(re.escape(variant), token, result, flags=re.IGNORECASE)
    return result


def _redact(value: Any) -> Any:
    if isinstance(value, dict):
        return {key: _redact(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_redact(item) for item in value]
    if isinstance(value, str):
        return _redact_string(value)
    return value


def _md_artifact(value: str, out: Path, report_path: Path) -> str:
    target = (out / value).resolve()
    relative = os.path.relpath(target, report_path.parent).replace("\\", "/")
    return f"[`{value}`]({relative})"


# --------------------------------------------------------------------------
# report builders


def _fixture_public(fixture: dict[str, Any]) -> dict[str, Any]:
    return {
        "name": fixture["name"],
        "description": fixture.get("description"),
        "html": str(fixture["html_path"]),
        "style": str(fixture["style_path"]) if fixture.get("style_path") else None,
        "viewport": dict(fixture["viewport"]),
        "gate": bool(fixture.get("gate", False)),
        "fonts": list(fixture.get("fonts", [])),
        "native": {
            "mode": fixture.get("native", {}).get("mode"),
            "exe": str(fixture.get("native", {}).get("exe_path", "")),
        },
        "regions": [dict(region) for region in fixture.get("regions", [])],
        "exclude": [dict(item) for item in fixture.get("exclude", [])],
        "tolerances": dict(fixture.get("tolerances", {})),
        "notes": list(fixture.get("notes", [])),
    }


def _read_meta(path: Path) -> dict[str, Any] | None:
    if not path.exists():
        return None
    try:
        return c.load_json(path)
    except (OSError, ValueError):
        return None


def compare_fixture(
    config: dict[str, Any],
    fixture: dict[str, Any],
    reference_path: Path,
    native_path: Path,
    reference_meta_path: Path,
    native_meta_path: Path,
    out: Path,
    environment: dict[str, Any],
    command: str,
) -> dict[str, Any]:
    c.validate_no_reasonless_excludes(fixture)
    diff_dir = c.ensure_dir(out / "diff")
    reports_dir = c.ensure_dir(out / "reports")
    width = int(fixture["viewport"]["width"])
    height = int(fixture["viewport"]["height"])
    threshold = int(
        config.get("defaults", {}).get("pixel_diff_threshold", c.DEFAULT_PIXEL_DIFF_THRESHOLD)
    )
    reference_meta = _read_meta(reference_meta_path)
    native_meta = _read_meta(native_meta_path)

    report: dict[str, Any] = {
        "schema": "visual-parity/1",
        "harness": c.HARNESS_VERSION,
        "generated_at": c.utc_now(),
        "fixture": _fixture_public(fixture),
        "gate": bool(fixture.get("gate", False)),
        "status": "ERROR",
        "would_fail": None,
        "errors": [],
        "warnings": [],
        "dimensions": {
            "expected": {"width": width, "height": height},
            "reference": None,
            "native": None,
            "match": False,
        },
        "reference": {
            "path": _rel(reference_path, out),
            "exists": reference_path.exists(),
            "meta": reference_meta,
        },
        "native": {
            "path": _rel(native_path, out),
            "exists": native_path.exists(),
            "meta": native_meta,
        },
        "global": None,
        "regions": [],
        "failures": [],
        "artifacts": {},
        "method": METHOD,
        "caveats": c.caveats_json(),
        "environment": environment,
        "command": command,
        "canonical_command": CANONICAL_COMMAND,
        "out": str(out),
    }

    if not reference_path.exists():
        report["errors"].append(
            f"reference PNG missing: {reference_path}. Run tools/capture_chromium.py first."
        )
    if not native_path.exists():
        report["errors"].append(
            f"native PNG missing: {native_path}. Run tools/render_native.py first."
        )
    if report["errors"]:
        return report

    try:
        rgb_a = load_rgb(reference_path)
        rgb_b = load_rgb(native_path)
    except Exception as exc:
        report["errors"].append(f"cannot read PNGs: {type(exc).__name__}: {exc}")
        return report

    ref_height, ref_width = rgb_a.shape[0], rgb_a.shape[1]
    nat_height, nat_width = rgb_b.shape[0], rgb_b.shape[1]
    report["dimensions"] = {
        "expected": {"width": width, "height": height},
        "reference": {"width": ref_width, "height": ref_height},
        "native": {"width": nat_width, "height": nat_height},
        "match": (ref_width, ref_height) == (nat_width, nat_height) == (width, height),
    }
    report["reference"].update(
        {
            "width": ref_width,
            "height": ref_height,
            "sha256": c.sha256_file(reference_path),
        }
    )
    report["native"].update(
        {
            "width": nat_width,
            "height": nat_height,
            "sha256": c.sha256_file(native_path),
        }
    )
    if not report["dimensions"]["match"]:
        report["errors"].append(
            "dimension mismatch: expected "
            f"{width}x{height}, reference {ref_width}x{ref_height}, "
            f"native {nat_width}x{nat_height}. Refusing to rescale."
        )
        return report

    try:
        mask, excludes = valid_mask(width, height, list(fixture.get("exclude", [])))
    except ValueError as exc:
        report["errors"].append(str(exc))
        return report

    report["exclude_applied"] = excludes
    ssim_full = ssim_map(luma(rgb_a), luma(rgb_b))

    global_metrics = compute_metrics(rgb_a, rgb_b, mask, ssim_full, threshold)
    global_thresholds = _global_thresholds(config)
    report["global"] = {
        "metrics": global_metrics,
        "thresholds": global_thresholds,
        "status": _status_for(global_metrics, global_thresholds),
    }

    regions: list[dict[str, Any]] = []
    for region in fixture.get("regions", []):
        name = str(region.get("name", f"region-{len(regions) + 1}"))
        mode = str(region.get("mode", "strict")).lower()
        try:
            rect = parse_rect(region.get("rect"), f"region {name}")
            rmask, clipped = region_mask(width, height, rect)
            rvalid = rmask & mask
            if not rvalid.any():
                report["errors"].append(f"region {name!r} has no valid pixels")
                continue
            metrics = compute_metrics(rgb_a, rgb_b, rvalid, ssim_full, threshold)
        except ValueError as exc:
            report["errors"].append(f"region {name!r}: {exc}")
            continue
        thresholds = (
            _global_thresholds(config)
            if mode == "background"
            else _threshold_block(config, fixture, mode)
            if mode in {"strict", "text"}
            else None
        )
        status = _status_for(metrics, thresholds)
        entry: dict[str, Any] = {
            "name": name,
            "mode": mode,
            "rect": list(rect),
            "clipped_rect": list(clipped),
            "metrics": metrics,
            "thresholds": thresholds,
            "status": status,
        }
        if mode == "text":
            ref_ink = ink_geometry(rgb_a, rvalid)
            nat_ink = ink_geometry(rgb_b, rvalid)
            iou = bbox_iou(
                ref_ink.get("bbox") if ref_ink else None,
                nat_ink.get("bbox") if nat_ink else None,
            )
            geometry = {"reference": ref_ink, "native": nat_ink, "bbox_iou": iou}
            entry["geometry"] = geometry
            if iou is not None and iou < 0.90 and status == "PASS":
                entry["status"] = "WARN"
                report["warnings"].append(
                    f"region {name!r}: SSIM/diff pasan pero bbox IoU={iou:.3f} < 0.90"
                )
        regions.append(entry)
        if entry["status"] == "FAIL":
            report["failures"].append(
                {
                    "scope": name,
                    "mode": mode,
                    "ssim": metrics["ssim"],
                    "pixel_diff_pct": metrics["pixel_diff_pct"],
                    "mae": metrics["mae"],
                    "thresholds": thresholds,
                    "rect": list(clipped),
                }
            )

    report["regions"] = regions
    if report["global"]["status"] == "FAIL":
        report["failures"].insert(
            0,
            {
                "scope": "global",
                "mode": "global",
                "ssim": global_metrics["ssim"],
                "pixel_diff_pct": global_metrics["pixel_diff_pct"],
                "mae": global_metrics["mae"],
                "thresholds": global_thresholds,
                "rect": [0, 0, width, height],
            },
        )

    hard_fail = report["global"]["status"] == "FAIL" or any(
        region["status"] == "FAIL" for region in regions
    )
    report["would_fail"] = hard_fail
    if report["errors"]:
        report["status"] = "ERROR"
    elif report["gate"]:
        report["status"] = "FAIL" if hard_fail else "PASS"
    else:
        report["status"] = "INFO"

    # artifacts ---------------------------------------------------------
    prefix = fixture["name"]
    side_path = diff_dir / f"{prefix}.side-by-side.png"
    diff_path = diff_dir / f"{prefix}.diff.png"
    overlay_path = diff_dir / f"{prefix}.overlay.png"
    browser_version = (
        reference_meta.get("browser", {}).get("version") if reference_meta else None
    ) or "Chromium"
    native_mode = (native_meta or {}).get("mode", "native")
    backend = ((native_meta or {}).get("adapter") or {}).get("backend", "native")
    compose_panels(
        [rgb_a, rgb_b],
        [f"Chromium {browser_version}", f"nativo {native_mode} ({backend})"],
        side_path,
    )
    diff_rgb, overlay = diff_visuals(rgb_a, rgb_b, threshold)
    Image.fromarray(diff_rgb, mode="RGB").save(diff_path, format="PNG")
    Image.fromarray(overlay, mode="RGB").save(overlay_path, format="PNG")
    report["artifacts"] = {
        "side_by_side": _rel(side_path, out),
        "diff": _rel(diff_path, out),
        "overlay": _rel(overlay_path, out),
    }

    region_crops: dict[str, str] = {}
    for entry in [r for r in regions if r["status"] in {"FAIL", "WARN"}][:6]:
        x, y, w, h = entry["clipped_rect"]
        if w <= 0 or h <= 0:
            continue
        pad = 12
        x0, y0 = max(0, x - pad), max(0, y - pad)
        x1, y1 = min(width, x + w + pad), min(height, y + h + pad)
        crop_a = rgb_a[y0:y1, x0:x1]
        crop_b = rgb_b[y0:y1, x0:x1]
        crop_overlay = overlay[y0:y1, x0:x1]
        crop_path = diff_dir / f"{prefix}.{entry['name']}.crop.png"
        compose_panels(
            [crop_a, crop_b, crop_overlay],
            ["Chromium", "nativo", f"diff>{threshold}"],
            crop_path,
        )
        region_crops[entry["name"]] = _rel(crop_path, out)
    if region_crops:
        report["artifacts"]["region_crops"] = region_crops

    return report


# --------------------------------------------------------------------------
# markdown


def _caveats_md() -> str:
    lines = ["## Caveats", ""]
    for item in c.CAVEATS:
        lines.append(f"- **{item['title']}.** {item['text']}")
    return "\n".join(lines)


def _environment_md(environment: dict[str, Any], report: dict[str, Any]) -> str:
    browser = ((report.get("reference") or {}).get("meta") or {}).get("browser") or {}
    native_meta = (report.get("native") or {}).get("meta") or {}
    adapter = native_meta.get("adapter") or {}
    rows = [
        ["SO", environment.get("os")],
        ["Python", environment.get("python")],
        ["Pillow / NumPy", f"{environment.get('pillow')} / {environment.get('numpy')}"],
        ["Playwright", environment.get("playwright")],
        ["Chromium", browser.get("version")],
        ["Chromium build", browser.get("playwright_browser_build")],
        ["Chromium SHA-256", browser.get("executable_sha256")],
        ["Chromium pin", browser.get("pin_ok")],
        ["Native modo", native_meta.get("mode")],
        ["Native exe", native_meta.get("exe")],
        ["Native SHA-256", native_meta.get("exe_sha256")],
        ["Build", native_meta.get("build_policy")],
        ["Backend", adapter.get("backend")],
        ["Adaptador", adapter.get("device")],
        ["Software", adapter.get("software")],
        ["DPR / DPI", "1 / 96"],
        ["Perfil de color", "sRGB, 8 bits"],
        ["Git", environment.get("git", {}).get("commit")],
    ]
    return "## Entorno\n\n" + c.markdown_table(["Campo", "Valor"], rows)


def _metrics_table(metrics: dict[str, Any] | None, thresholds: dict[str, Any] | None, status: str) -> str:
    if not metrics:
        return "_Sin metricas._"
    rows = [
        ["SSIM", c.format_float(metrics["ssim"])],
        ["Pixel diff %", c.format_float(metrics["pixel_diff_pct"], 4)],
        ["MAE", c.format_float(metrics["mae"], 4)],
        ["MAE R / G / B", ", ".join(c.format_float(v, 4) for v in metrics["mae_per_channel"])],
        ["Pixeles con diff", metrics["diff_pixels"]],
        ["Pixeles validos", metrics["valid_pixels"]],
        ["BBox diff", metrics["diff_bbox"]],
        ["P99 / max diff", f"{c.format_float(metrics['p99_channel_diff'], 2)} / {metrics['max_channel_diff']}"],
        ["Umbral", (
            f"SSIM >= {c.format_float(thresholds['min_ssim'], 3)}, "
            f"diff <= {c.format_float(thresholds['max_diff_pct'], 2)}%"
            if thresholds
            else "informativo"
        )],
        ["Estado", status],
    ]
    return c.markdown_table(["Metrica", "Valor"], rows)


def write_fixture_markdown(report: dict[str, Any], path: Path, out: Path) -> None:
    fixture = report["fixture"]
    lines = [f"# Paridad visual — {fixture['name']}", ""]
    gate = "gate" if fixture.get("gate") else "informativo, gate=false"
    lines.append(f"- **Estado:** **{report['status']}** ({gate})")
    if report.get("would_fail") is not None:
        lines.append(f"- **Habria fallado el gate:** {report['would_fail']}")
    lines.append(f"- **Fixture:** `{fixture['html']}`")
    lines.append(
        f"- **Viewport:** {fixture['viewport']['width']}×{fixture['viewport']['height']} "
        "(DPR 1, 96 DPI, sRGB)"
    )
    lines.append(f"- **Generado:** {report['generated_at']}")
    lines.append(f"- **Comando canonico:** `{report.get('canonical_command')}`")
    if fixture.get("notes"):
        lines.append("")
        lines.append("**Notas del fixture:**")
        for note in fixture["notes"]:
            lines.append(f"- {note}")
    lines.append("")
    if report["errors"]:
        lines.append("## Errores")
        lines.append("")
        for error in report["errors"]:
            lines.append(f"- {error}")
        lines.append("")
    if report.get("dimensions"):
        dims = report["dimensions"]
        lines.append("## Dimensiones")
        lines.append("")
        lines.append(
            c.markdown_table(
                ["Esperado", "Chromium", "Nativo", "Coinciden"],
                [
                    [
                        f"{dims['expected']['width']}×{dims['expected']['height']}",
                        (
                            f"{dims['reference']['width']}×{dims['reference']['height']}"
                            if dims.get("reference")
                            else "—"
                        ),
                        (
                            f"{dims['native']['width']}×{dims['native']['height']}"
                            if dims.get("native")
                            else "—"
                        ),
                        dims.get("match"),
                    ]
                ],
            )
        )
        lines.append("")
    if report.get("global"):
        lines.append("## Metricas globales")
        lines.append("")
        lines.append(
            _metrics_table(
                report["global"]["metrics"],
                report["global"]["thresholds"],
                report["global"]["status"],
            )
        )
        lines.append("")
    if report.get("regions"):
        lines.append("## Regiones")
        lines.append("")
        rows = []
        for region in report["regions"]:
            metrics = region["metrics"]
            rows.append(
                [
                    region["name"],
                    region["mode"],
                    str(region["clipped_rect"]),
                    c.format_float(metrics["ssim"]),
                    c.format_float(metrics["pixel_diff_pct"], 3),
                    c.format_float(metrics["mae"], 3),
                    (
                        f"SSIM >= {c.format_float(region['thresholds']['min_ssim'], 3)}, "
                        f"diff <= {c.format_float(region['thresholds']['max_diff_pct'], 2)}%"
                        if region.get("thresholds")
                        else "informativo"
                    ),
                    region["status"],
                ]
            )
        lines.append(
            c.markdown_table(
                ["Region", "Modo", "Rect", "SSIM", "Diff %", "MAE", "Umbral", "Estado"],
                rows,
            )
        )
        lines.append("")
        for region in report["regions"]:
            geometry = region.get("geometry")
            if not geometry:
                continue
            ref_box = (geometry.get("reference") or {}).get("bbox")
            nat_box = (geometry.get("native") or {}).get("bbox")
            lines.append(
                f"- **{region['name']} (text):** bbox tinta Chromium={ref_box}, "
                f"nativo={nat_box}, IoU={c.format_float(geometry.get('bbox_iou'), 4)}"
            )
        lines.append("")
    if report.get("failures"):
        lines.append("## Hallazgos")
        lines.append("")
        lines.append(
            c.markdown_table(
                ["Scope", "Modo", "SSIM", "Diff %", "MAE", "Umbral", "Rect"],
                [
                    [
                        item["scope"],
                        item["mode"],
                        c.format_float(item["ssim"]),
                        c.format_float(item["pixel_diff_pct"], 3),
                        c.format_float(item["mae"], 3),
                        (
                            f"{c.format_float(item['thresholds']['min_ssim'], 3)} / "
                            f"{c.format_float(item['thresholds']['max_diff_pct'], 2)}%"
                            if item.get("thresholds")
                            else "—"
                        ),
                        str(item.get("rect")),
                    ]
                    for item in report["failures"]
                ],
            )
        )
        lines.append("")
        lines.append("## Triage sugerido")
        lines.append("")
        for step, text in enumerate(TRIAGE, 1):
            lines.append(f"{step}. {text}")
        lines.append("")
    if report.get("warnings"):
        lines.append("## Avisos")
        lines.append("")
        for warning in report["warnings"]:
            lines.append(f"- {warning}")
        lines.append("")
    artifacts = report.get("artifacts") or {}
    if artifacts:
        lines.append("## Artefactos")
        lines.append("")
        for key, value in artifacts.items():
            if isinstance(value, dict):
                for sub_key, sub_value in value.items():
                    lines.append(f"- {key}/{sub_key}: {_md_artifact(sub_value, out, path)}")
            else:
                lines.append(f"- {key}: {_md_artifact(value, out, path)}")
        lines.append("")
    lines.append(_environment_md(report.get("environment", {}), report))
    lines.append("")
    lines.append("## Metodo")
    lines.append("")
    for key, value in METHOD.items():
        lines.append(f"- **{key}:** {value}")
    lines.append("")
    lines.append(_caveats_md())
    lines.append("")
    c.write_text(path, "\n".join(lines))


def write_combined_markdown(
    report: dict[str, Any], path: Path, out: Path
) -> None:
    lines = ["# Paridad visual Chromium vs nativo", ""]
    lines.append(f"- **Generado:** {report['generated_at']}")
    lines.append(f"- **Comando canonico:** `{CANONICAL_COMMAND}`")
    lines.append(f"- **Harness:** `{report['harness']}`")
    lines.append("")
    summary = report["summary"]
    lines.append("## Resumen")
    lines.append("")
    lines.append(
        c.markdown_table(
            ["Fixtures", "PASS", "FAIL", "INFO", "ERROR", "Gate failures"],
            [
                [
                    summary["total"],
                    summary["passed"],
                    summary["failed"],
                    summary["info"],
                    summary["error"],
                    ", ".join(summary["gate_failures"]) or "—",
                ]
            ],
        )
    )
    lines.append("")
    rows = []
    for item in report["fixtures"]:
        metrics = (item.get("global") or {}).get("metrics") or {}
        browser = (((item.get("reference") or {}).get("meta") or {}).get("browser") or {})
        adapter = (((item.get("native") or {}).get("meta") or {}).get("adapter") or {})
        rows.append(
            [
                item["fixture"]["name"],
                f"{item['fixture']['viewport']['width']}×{item['fixture']['viewport']['height']}",
                browser.get("version") or "—",
                adapter.get("backend") or "—",
                c.format_float(metrics.get("ssim")),
                c.format_float(metrics.get("pixel_diff_pct"), 3),
                c.format_float(metrics.get("mae"), 3),
                "gate" if item.get("gate") else "info",
                item["status"],
                f"[detalle](reports/{item['fixture']['name']}.report.md)",
            ]
        )
    lines.append("## Fixtures")
    lines.append("")
    lines.append(
        c.markdown_table(
            ["Fixture", "Viewport", "Chromium", "Backend", "SSIM", "Diff %", "MAE", "Gate", "Estado", "Reporte"],
            rows,
        )
    )
    lines.append("")
    for item in report["fixtures"]:
        lines.append(f"### {item['fixture']['name']} — {item['status']}")
        lines.append("")
        if item.get("errors"):
            lines.append("Errores: " + "; ".join(item["errors"]))
            lines.append("")
        if item.get("failures"):
            first = item["failures"][0]
            lines.append(
                f"Primer fallo: **{first['scope']}** SSIM={c.format_float(first['ssim'])}, "
                f"diff={c.format_float(first['pixel_diff_pct'], 3)}%, "
                f"MAE={c.format_float(first['mae'], 3)}."
            )
            lines.append("")
        artifacts = item.get("artifacts") or {}
        if artifacts:
            links = []
            for key in ("side_by_side", "diff", "overlay"):
                if artifacts.get(key):
                    links.append(f"[{key}]({artifacts[key]})")
            if links:
                lines.append("Artefactos: " + " · ".join(links))
                lines.append("")
    lines.append("## Entorno")
    lines.append("")
    env = report.get("environment", {})
    lines.append(
        c.markdown_table(
            ["Campo", "Valor"],
            [
                ["SO", env.get("os")],
                ["Python", env.get("python")],
                ["Pillow / NumPy", f"{env.get('pillow')} / {env.get('numpy')}"],
                ["Playwright", env.get("playwright")],
                ["Git", env.get("git", {}).get("commit")],
                ["DPR / DPI", "1 / 96"],
                ["Color", "sRGB, 8 bits"],
                ["Chromium", "ver cada reporte (pinned por Playwright)"],
                ["Backend nativo", "ver cada reporte (WARP/D3D12 en esta maquina)"],
            ],
        )
    )
    lines.append("")
    lines.append("## Metodo")
    lines.append("")
    for key, value in METHOD.items():
        lines.append(f"- **{key}:** {value}")
    lines.append("")
    if report.get("summary", {}).get("would_fail"):
        lines.append("## Fixtures informativos que habrian fallado")
        lines.append("")
        for name in report["summary"]["would_fail"]:
            lines.append(f"- `{name}`")
        lines.append("")
    lines.append(_caveats_md())
    lines.append("")
    c.write_text(path, "\n".join(lines))


# --------------------------------------------------------------------------
# main


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", nargs="*", default=None, help="fixture names (default: all)")
    parser.add_argument("--fixtures-json", default=None)
    parser.add_argument("--out", required=True, help="harness output directory")
    parser.add_argument("--capture-dir", default=None)
    parser.add_argument("--meta-dir", default=None)
    parser.add_argument("--reference", default=None, help="override reference PNG (single fixture)")
    parser.add_argument("--native", default=None, help="override native PNG (single fixture)")
    return parser.parse_args(argv)


def _command_line() -> str:
    return " ".join([Path(sys.argv[0]).name] + sys.argv[1:])


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.fixtures_json is None:
        args.fixtures_json = str(c.harness_dir() / "fixtures.json")
    out = Path(args.out)
    if not out.is_absolute():
        out = (Path.cwd() / out).resolve()
    c.ensure_dir(out)
    try:
        config, fixtures = c.load_fixtures(args.fixtures_json)
    except Exception as exc:
        print(f"COMPARE FAILED: cannot load fixtures: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2

    mapping = c.fixture_map(fixtures)
    names = c.split_names(args.fixtures) if args.fixtures else [fixture["name"] for fixture in fixtures]
    unknown = [name for name in names if name not in mapping]
    if unknown:
        print(f"COMPARE FAILED: unknown fixtures: {', '.join(unknown)}", file=sys.stderr)
        return 2
    if (args.reference or args.native) and len(names) != 1:
        print("COMPARE FAILED: --reference/--native require exactly one fixture", file=sys.stderr)
        return 2

    capture_dir = Path(args.capture_dir) if args.capture_dir else out / "captures"
    meta_dir = Path(args.meta_dir) if args.meta_dir else out / "meta"
    reports_dir = c.ensure_dir(out / "reports")
    environment = c.environment_info()
    command = _command_line()

    fixture_reports: list[dict[str, Any]] = []
    for name in names:
        fixture = mapping[name]
        reference_path = (
            Path(args.reference)
            if args.reference
            else capture_dir / f"{name}.chromium.png"
        )
        native_path = (
            Path(args.native) if args.native else capture_dir / f"{name}.native.png"
        )
        if not reference_path.is_absolute():
            reference_path = (Path.cwd() / reference_path).resolve()
        if not native_path.is_absolute():
            native_path = (Path.cwd() / native_path).resolve()
        try:
            report = compare_fixture(
                config,
                fixture,
                reference_path,
                native_path,
                meta_dir / f"{name}.chromium.json",
                meta_dir / f"{name}.native.json",
                out,
                environment,
                command,
            )
        except Exception as exc:  # never leave the run without a report
            report = {
                "schema": "visual-parity/1",
                "harness": c.HARNESS_VERSION,
                "generated_at": c.utc_now(),
                "fixture": _fixture_public(fixture),
                "gate": bool(fixture.get("gate", False)),
                "status": "ERROR",
                "would_fail": None,
                "errors": [f"compare crashed: {type(exc).__name__}: {exc}"],
                "warnings": [],
                "dimensions": None,
                "reference": {"path": _rel(reference_path, out), "meta": None},
                "native": {"path": _rel(native_path, out), "meta": None},
                "global": None,
                "regions": [],
                "failures": [],
                "artifacts": {},
                "method": METHOD,
                "caveats": c.caveats_json(),
                "environment": environment,
                "command": command,
                "canonical_command": CANONICAL_COMMAND,
            }
        fixture_reports.append(report)
        report = _redact(report)
        fixture_reports[-1] = report
        fixture_json = reports_dir / f"{name}.report.json"
        fixture_md = reports_dir / f"{name}.report.md"
        c.write_json(fixture_json, report)
        write_fixture_markdown(report, fixture_md, out)
        print(
            f"compare: {name} -> {report['status']} "
            f"(SSIM={c.format_float((report.get('global') or {}).get('metrics', {}).get('ssim'))}, "
            f"diff={c.format_float((report.get('global') or {}).get('metrics', {}).get('pixel_diff_pct'), 3)}%)"
        )

    summary = {
        "total": len(fixture_reports),
        "passed": sum(1 for item in fixture_reports if item["status"] == "PASS"),
        "failed": sum(1 for item in fixture_reports if item["status"] == "FAIL"),
        "info": sum(1 for item in fixture_reports if item["status"] == "INFO"),
        "error": sum(1 for item in fixture_reports if item["status"] == "ERROR"),
        "gate_failures": [
            item["fixture"]["name"]
            for item in fixture_reports
            if item["gate"] and item["status"] == "FAIL"
        ],
        "would_fail": [
            item["fixture"]["name"]
            for item in fixture_reports
            if item.get("would_fail") and not item["gate"]
        ],
    }
    combined: dict[str, Any] = {
        "schema": "visual-parity/1",
        "harness": c.HARNESS_VERSION,
        "generated_at": c.utc_now(),
        "command": command,
        "canonical_command": CANONICAL_COMMAND,
        "environment": environment,
        "method": METHOD,
        "summary": summary,
        "fixtures": fixture_reports,
        "caveats": c.caveats_json(),
    }
    c.write_json(out / "report.json", combined)
    markdown_path = out / "report.md"
    write_combined_markdown(combined, markdown_path, out)
    # Committable copies: same content with machine-specific absolute paths
    # collapsed to <repo>/<home>.
    c.write_json(out / "report.redacted.json", _redact(combined))
    if markdown_path.exists():
        c.write_text(out / "report.redacted.md", _redact(markdown_path.read_text(encoding="utf-8")))
    print(
        f"summary: {summary['passed']} PASS, {summary['failed']} FAIL, "
        f"{summary['info']} INFO, {summary['error']} ERROR -> {out / 'report.md'}"
    )
    return 1 if (summary["failed"] or summary["error"]) else 0


if __name__ == "__main__":
    raise SystemExit(main())
