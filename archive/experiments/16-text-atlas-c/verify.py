#!/usr/bin/env python3
"""Verificación de aceptación de exp16-text-c (build-time only).

Comprueba, sobre el PNG que produce `scene_text.exe`:

  1. tamaño 400x240,
  2. texto "Hola" visible en la banda del título: >= 100 píxeles con
     luminancia > max(100, fondo+40), bbox dentro de la banda, y >= 10 píxeles
     claros dentro de CADA uno de los 4 quads de glyph,
  3. los tres chips conservan su color exacto y el fondo también,
  4. glyph count del header == glyphs del IR == quads del atlas,
  5. el atlas subido a la GPU coincide con atlas.bin (FNV-1a del stdout),
  6. paridad con el render de exp09 (render_frontend.png): maxdiff y % de
     píxeles cambiados en la imagen completa y en el área de texto.

Uso:
  python verify.py --png scene_text.png --frontend render_frontend.png \
                   --stdout run_stdout.txt --report verify_report.json
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from PIL import Image

CHIPS = [
    ("chip-a", (50, 100), (0xE7, 0x4C, 0x3C)),
    ("chip-b", (118, 100), (0x2E, 0xCC, 0x71)),
    ("chip-c", (186, 100), (0x34, 0x98, 0xDB)),
]
BACKGROUND_POINTS = [(300, 200), (390, 20), (10, 200), (260, 120)]
BACKGROUND = (0x10, 0x10, 0x14)
TITLE_BAND = (0, 10, 400, 70)

CHECKS: list[dict] = []


def check(name: str, ok: bool, detail: str = "") -> bool:
    CHECKS.append({"check": name, "pass": bool(ok), "detail": detail})
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" -> {detail}" if detail else ""))
    return bool(ok)


def luminance(pixel) -> float:
    return 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2]


def fnv1a(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def diff_stats(a: Image.Image, b: Image.Image, box=None) -> dict:
    if box is not None:
        a = a.crop(box)
        b = b.crop(box)
    if a.size != b.size:
        raise ValueError(f"size mismatch: {a.size} vs {b.size}")
    pa, pb = a.tobytes(), b.tobytes()
    changed = 0
    maxdiff = 0
    for i in range(0, len(pa), 3):
        d0 = abs(pa[i] - pb[i])
        d1 = abs(pa[i + 1] - pb[i + 1])
        d2 = abs(pa[i + 2] - pb[i + 2])
        local = max(d0, d1, d2)
        if local:
            changed += 1
            if local > maxdiff:
                maxdiff = local
    pixels = a.width * a.height
    return {
        "region": list(box) if box else [0, 0, a.width, a.height],
        "pixels": pixels,
        "maxdiff": maxdiff,
        "changed_pixels": changed,
        "changed_pct": round(100.0 * changed / pixels, 4),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="exp16 acceptance checks")
    parser.add_argument("--png", type=Path, default=Path("scene_text.png"))
    parser.add_argument("--frontend", type=Path, default=Path("render_frontend.png"))
    parser.add_argument("--header", type=Path, default=Path("scene_data.h"))
    parser.add_argument("--ir", type=Path, default=Path("display_list.json"))
    parser.add_argument("--atlas", type=Path, default=Path("atlas.bin"))
    parser.add_argument("--quads", type=Path, default=Path("glyph_quads.json"))
    parser.add_argument("--stdout", type=Path, default=Path("run_stdout.txt"))
    parser.add_argument("--report", type=Path, default=Path("verify_report.json"))
    parser.add_argument("--no-frontend", action="store_true",
                        help="skip frontend parity checks (no reference PNG)")
    parser.add_argument("--visuals", type=Path, default=None,
                        help="directory for zoomed side-by-side PNG evidence")
    parser.add_argument("--exp09", type=Path,
                        default=Path("..") / "09-core-package-v1" / "render.png",
                        help="exp09's own committed wgpu artifact (read-only)")
    args = parser.parse_args(argv)

    if not args.png.exists():
        print(f"error: PNG not found: {args.png}", file=sys.stderr)
        return 1

    image = Image.open(args.png).convert("RGB")
    header = args.header.read_text(encoding="utf-8")
    ir = json.loads(args.ir.read_text(encoding="utf-8-sig"))
    quads_doc = json.loads(args.quads.read_text(encoding="utf-8-sig"))
    stdout = args.stdout.read_text(encoding="utf-8", errors="replace") \
        if args.stdout.exists() else ""

    result: dict = {"png": str(args.png), "checks": CHECKS, "metrics": {}}

    # 1. size ------------------------------------------------------------
    check("png-size", image.size == (400, 240), f"{image.size[0]}x{image.size[1]}")

    # 2. text band -------------------------------------------------------
    x0, y0, x1, y1 = TITLE_BAND
    background_lum = luminance(BACKGROUND)
    threshold = max(100.0, background_lum + 40.0)
    band_pixels = [image.getpixel((x, y)) for y in range(y0, y1) for x in range(x0, x1)]
    bright = [p for p in band_pixels if luminance(p) > threshold]
    result["metrics"]["title_band"] = {
        "box": list(TITLE_BAND),
        "threshold_luminance": round(threshold, 3),
        "bright_pixels": len(bright),
        "background_luminance": round(background_lum, 3),
    }
    check("text-bright-pixels", len(bright) >= 100,
          f"{len(bright)} px with luminance > {threshold:.1f} (need >= 100)")

    glyph_quads = quads_doc.get("quads", [])
    text_bbox = None
    if glyph_quads:
        gx0 = min(q["x"] for q in glyph_quads)
        gy0 = min(q["y"] for q in glyph_quads)
        gx1 = max(q["x"] + q["w"] for q in glyph_quads)
        gy1 = max(q["y"] + q["h"] for q in glyph_quads)
        text_bbox = (gx0, gy0, gx1, gy1)
        result["metrics"]["text_bbox"] = [round(v, 3) for v in text_bbox]
        inside = [image.getpixel((x, y)) for y in range(int(gy0), int(gy1))
                  for x in range(int(gx0), int(gx1))]
        lit = sum(1 for p in inside if luminance(p) > threshold)
        check("text-bbox-in-band", (gy0 >= y0 and gy1 <= y1 and gx0 >= x0 and gx1 <= x1),
              f"bbox=({gx0:g},{gy0:g})-({gx1:g},{gy1:g}) inside band {TITLE_BAND}")
        check("text-bbox-lit", lit >= 100, f"{lit} bright px inside glyph bbox")

        per_glyph = []
        all_glyphs_lit = True
        for index, quad in enumerate(glyph_quads):
            gx = int(quad["x"])
            gy = int(quad["y"])
            gw = max(1, int(quad["w"]))
            gh = max(1, int(quad["h"]))
            count = sum(1 for yy in range(gy, gy + gh) for xx in range(gx, gx + gw)
                        if luminance(image.getpixel((xx, yy))) > threshold)
            per_glyph.append({"index": index, "gid": quad.get("gid"),
                              "box": [gx, gy, gw, gh], "bright_pixels": count})
            if count < 10:
                all_glyphs_lit = False
        result["metrics"]["per_glyph"] = per_glyph
        check("each-glyph-visible", all_glyphs_lit,
              ", ".join(f"gid{q['gid']}={q['bright_pixels']}px" for q in per_glyph))
        # texto = cobertura clara sobre fondo oscuro, no un rectángulo blanco
        check("text-not-solid-block", len(bright) < 0.75 * (gx1 - gx0) * (gy1 - gy0),
              f"{len(bright)} lit of {int((gx1-gx0)*(gy1-gy0))} bbox px")
    else:
        check("each-glyph-visible", False, "no quads in glyph_quads.json")

    # 3. chips + background ---------------------------------------------
    chip_rects = [(20, 71, 60, 60), (88, 71, 60, 60), (156, 71, 60, 60)]
    for name, (px, py), expected in CHIPS:
        actual = image.getpixel((px, py))
        check(f"{name}-exact", actual == expected,
              f"({px},{py})={actual} expected={expected}")
    for (rx, ry, rw, rh), (_name, _point, expected) in zip(chip_rects, CHIPS):
        wrong = sum(1 for yy in range(ry, ry + rh) for xx in range(rx, rx + rw)
                    if image.getpixel((xx, yy)) != expected)
        check(f"chip-{rx}-uniform", wrong == 0,
              f"rect ({rx},{ry}) {rw}x{rh}: {rw * rh - wrong}/{rw * rh} px exact {expected}")
    for px, py in BACKGROUND_POINTS:
        actual = image.getpixel((px, py))
        check(f"background-{px}x{py}", actual == BACKGROUND,
              f"{actual} expected={BACKGROUND}")

    # 4. counts: header == IR == atlas --------------------------------
    def header_int(name: str) -> int:
        match = re.search(rf"#define {name} (\d+)", header)
        if match is None:
            raise ValueError(f"{name} not found in {args.header}")
        return int(match.group(1))

    ir_glyphs = 0
    for op in ir["ops"]:
        if op.get("type") == "text_run":
            ir_glyphs += len(op.get("glyphs", []))
    header_glyphs = header_int("SCENE_GLYPH_QUAD_COUNT")
    header_verts = header_int("SCENE_GLYPH_VERTEX_COUNT")
    atlas_glyphs = quads_doc.get("glyph_count")
    result["metrics"]["counts"] = {
        "ir_glyphs": ir_glyphs,
        "header_glyph_quads": header_glyphs,
        "header_glyph_vertices": header_verts,
        "atlas_glyph_count": atlas_glyphs,
        "atlas_quads": len(glyph_quads),
    }
    check("glyph-count-ir-header-atlas",
          ir_glyphs == header_glyphs == atlas_glyphs == len(glyph_quads),
          f"IR={ir_glyphs} header={header_glyphs} atlas={atlas_glyphs} quads={len(glyph_quads)}")
    check("glyph-vertex-count", header_verts == header_glyphs * 6,
          f"{header_verts} == {header_glyphs} * 6")

    # 5. atlas upload proof (stdout FNV vs atlas.bin) ------------------
    atlas_bytes = args.atlas.read_bytes()
    expected_fnv = fnv1a(atlas_bytes)
    match = re.search(r"gpu-hash=0x([0-9A-Fa-f]{8}) cpu-hash=0x([0-9A-Fa-f]{8}) (MATCH|MISMATCH)",
                      stdout)
    if match:
        gpu_hash = int(match.group(1), 16)
        cpu_hash = int(match.group(2), 16)
        verdict = match.group(3)
        result["metrics"]["atlas_hash"] = {
            "gpu": f"0x{gpu_hash:08X}", "cpu": f"0x{cpu_hash:08X}",
            "atlas_bin": f"0x{expected_fnv:08X}", "verdict": verdict,
        }
        check("atlas-gpu-upload",
              verdict == "MATCH" and gpu_hash == cpu_hash == expected_fnv,
              f"gpu=0x{gpu_hash:08X} cpu=0x{cpu_hash:08X} atlas.bin=0x{expected_fnv:08X}")
    else:
        check("atlas-gpu-upload", False, "no atlas hash line in exe stdout")

    match = re.search(r"atlas: uploaded (\d+)x(\d+) R8Unorm \((\d+) bytes\)", stdout)
    if match:
        dims = tuple(int(match.group(i)) for i in (1, 2, 3))
        check("atlas-texture-shape", dims == (512, 512, 262144), f"{dims}")
    else:
        check("atlas-texture-shape", False, "no atlas upload line in exe stdout")

    check("exe-draw-calls", "2 draw call(s)" in stdout,
          "rects + glyphs in one pass, 2 draws")

    # 6. frontend parity ------------------------------------------------
    if args.no_frontend:
        result["skipped"] = ["frontend-parity (--no-frontend)"]
        print("[SKIP] frontend parity (--no-frontend)")
    elif args.frontend.exists():
        frontend = Image.open(args.frontend).convert("RGB")
        whole = diff_stats(image, frontend)
        result["metrics"]["frontend_diff"] = {"whole": whole}
        check("frontend-parity-whole", whole["changed_pixels"] == 0,
              f"maxdiff={whole['maxdiff']} changed={whole['changed_pixels']} "
              f"({whole['changed_pct']}%)")
        if text_bbox:
            pad = 2
            box = (max(0, int(text_bbox[0]) - pad), max(0, int(text_bbox[1]) - pad),
                   min(image.width, int(text_bbox[2]) + pad),
                   min(image.height, int(text_bbox[3]) + pad))
            text_diff = diff_stats(image, frontend, box)
            result["metrics"]["frontend_diff"]["text_area"] = text_diff
            check("frontend-parity-text", text_diff["changed_pixels"] == 0,
                  f"box={box} maxdiff={text_diff['maxdiff']} "
                  f"changed={text_diff['changed_pixels']} ({text_diff['changed_pct']}%)")
            chip_box = (20, 71, 216, 131)
            chip_diff = diff_stats(image, frontend, chip_box)
            result["metrics"]["frontend_diff"]["chip_area"] = chip_diff
            check("frontend-parity-chips", chip_diff["changed_pixels"] == 0,
                  f"box={chip_box} maxdiff={chip_diff['maxdiff']}")

            if args.visuals is not None:
                args.visuals.mkdir(parents=True, exist_ok=True)
                zoom = 4
                crop = (0, 20, 130, 60)
                width = (crop[2] - crop[0]) * zoom
                height = (crop[3] - crop[1]) * zoom
                left = image.crop(crop).resize((width, height), Image.NEAREST)
                right = frontend.crop(crop).resize((width, height), Image.NEAREST)
                left.save(args.visuals / "text_zoom_c.png")
                right.save(args.visuals / "text_zoom_frontend.png")
                gap = 8
                combo = Image.new("RGB", (width, height * 2 + gap), (64, 64, 64))
                combo.paste(left, (0, 0))
                combo.paste(right, (0, height + gap))
                combo.save(args.visuals / "text_compare_4x.png")
                # atlas: recorte de la zona usada, 8x, para inspección humana
                atlas_w = int(quads_doc.get("width", 512))
                atlas_h = int(quads_doc.get("height", 512))
                atlas_image = Image.frombytes("L", (atlas_w, atlas_h), atlas_bytes)
                ax0 = max(0, int(min(q["u0"] for q in glyph_quads) * atlas_w) - 1)
                ay0 = max(0, int(min(q["v0"] for q in glyph_quads) * atlas_h) - 1)
                ax1 = min(atlas_w, int(max(q["u1"] for q in glyph_quads) * atlas_w) + 2)
                ay1 = min(atlas_h, int(max(q["v1"] for q in glyph_quads) * atlas_h) + 2)
                atlas_crop = atlas_image.crop((ax0, ay0, ax1, ay1))
                atlas_crop.resize(((ax1 - ax0) * 8, (ay1 - ay0) * 8),
                                  Image.NEAREST).save(args.visuals / "atlas_used_8x.png")
                result["visuals"] = {
                    "text_zoom_c": str(args.visuals / "text_zoom_c.png"),
                    "text_zoom_frontend": str(args.visuals / "text_zoom_frontend.png"),
                    "text_compare_4x": str(args.visuals / "text_compare_4x.png"),
                    "atlas_used_8x": str(args.visuals / "atlas_used_8x.png"),
                }
    else:
        check("frontend-parity-whole", False, f"reference not found: {args.frontend}")

    # 7. parity against exp09's committed artifact (read-only reference) --
    if not args.no_frontend and args.exp09.exists():
        exp09 = Image.open(args.exp09).convert("RGB")
        exp09_diff = diff_stats(image, exp09)
        result["metrics"]["exp09_artifact_diff"] = {
            "png": str(args.exp09), **exp09_diff,
        }
        check("exp09-artifact-parity", exp09_diff["changed_pixels"] == 0,
              f"maxdiff={exp09_diff['maxdiff']} changed={exp09_diff['changed_pixels']} "
              f"({exp09_diff['changed_pct']}%)")
        if args.frontend.exists():
            same_as_frontend = diff_stats(
                Image.open(args.frontend).convert("RGB"), exp09)["changed_pixels"] == 0
            check("frontend-render-reproduces-exp09", same_as_frontend,
                  f"{args.frontend} == {args.exp09}")
    failures = [item for item in CHECKS if not item["pass"]]
    result["passed"] = len(CHECKS) - len(failures)
    result["failed"] = len(failures)
    result["ok"] = not failures
    args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")

    print(f"\nverify: {result['passed']}/{len(CHECKS)} checks passed -> {args.report}")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
