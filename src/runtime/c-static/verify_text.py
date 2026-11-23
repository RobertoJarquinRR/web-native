#!/usr/bin/env python3
"""Pixel verification for the text-capable webnative C runtime.

Build-time only (Pillow). Two modes:

  text  Compare an offscreen render against the exp16 reference pixel by pixel
        (tolerance 0) and check the text band, glyph coverage, exact chips,
        background pixels, generated header counts and runtime stdout proof.

  rect  Rebuild the expected rect-only image from the display-list IR with
        Pillow and compare the runtime PNG pixel by pixel (tolerance 0). This
        is the regression gate for the original rect-only emitter/runtime.

Exit code 0 only when every check passes.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw


def parse_color(value: str) -> tuple[int, int, int, int]:
    text = value.strip().lower()
    if not text.startswith("#"):
        raise ValueError(f"not a color: {value!r}")
    digits = text[1:]
    if len(digits) == 3:
        digits = "".join(ch * 2 for ch in digits)
    elif len(digits) == 6:
        digits += "ff"
    if len(digits) != 8:
        raise ValueError(f"not a color: {value!r}")
    r, g, b, a = (int(digits[i : i + 2], 16) for i in range(0, 8, 2))
    return r, g, b, a


def load_ir(path: Path) -> dict:
    with path.open("r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def read_text_any(path: Path) -> str:
    """Read UTF-8/UTF-16 text; PowerShell 5 redirection writes UTF-16LE."""
    data = path.read_bytes()
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return data.decode("utf-16")
    return data.decode("utf-8-sig", errors="replace")


def diff_metrics(image: Image.Image, reference: Image.Image) -> dict:
    if image.size != reference.size:
        return {"same_size": False, "maxdiff": 255, "changed_pixels": -1,
                "changed_pct": 100.0}
    diff = ImageChops.difference(image, reference)
    extrema = diff.getextrema()
    maxdiff = max(channel[1] for channel in extrema)
    raw = diff.tobytes()
    changed = sum(
        1 for i in range(0, len(raw), 4)
        if raw[i] or raw[i + 1] or raw[i + 2] or raw[i + 3])
    total = image.size[0] * image.size[1]
    return {
        "same_size": True,
        "maxdiff": int(maxdiff),
        "changed_pixels": changed,
        "changed_pct": round(100.0 * changed / total, 4),
    }


class Checks:
    def __init__(self) -> None:
        self.items: list[dict] = []

    def add(self, name: str, ok: bool, detail: str) -> None:
        self.items.append({"name": name, "ok": bool(ok), "detail": detail})
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")

    @property
    def passed(self) -> int:
        return sum(1 for item in self.items if item["ok"])

    @property
    def failed(self) -> int:
        return len(self.items) - self.passed


def luminance(pixel: tuple[int, int, int, int]) -> float:
    return 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2]


def text_checks(args: argparse.Namespace, checks: Checks) -> dict:
    image = Image.open(args.png).convert("RGBA")
    reference = Image.open(args.reference).convert("RGBA")
    ir = load_ir(args.ir)
    viewport = ir["viewport"]
    width, height = int(viewport["width"]), int(viewport["height"])
    ops = ir["ops"]

    checks.add("png_size", image.size == (width, height),
               f"{image.size[0]}x{image.size[1]} (expected {width}x{height})")
    diff = diff_metrics(image, reference)
    checks.add("reference_parity", diff["same_size"] and diff["maxdiff"] == 0,
               f"maxdiff={diff['maxdiff']} changed={diff['changed_pixels']} "
               f"({diff['changed_pct']}%)")

    rects = [op for op in ops if op.get("type") == "rect"]
    if not rects:
        raise ValueError("IR has no rect ops")
    # The visible background is the LAST full-viewport rect (exp16 clears with
    # op[0]=#0f0f12 but then paints the #101014 card over the whole viewport).
    full_viewport = [
        op for op in rects
        if int(op["x"]) == 0 and int(op["y"]) == 0
        and int(op["w"]) == width and int(op["h"]) == height
    ]
    background = parse_color((full_viewport[-1] if full_viewport else rects[0])["fill"])
    points = [(300, 200), (390, 20), (10, 200), (260, 120)]
    bad_points = []
    for x, y in points:
        if x < width and y < height:
            pixel = image.getpixel((x, y))
            if pixel != background:
                bad_points.append(((x, y), pixel))
    checks.add("background_exact", not bad_points,
               f"{len(points) - len(bad_points)}/{len(points)} points == "
               f"#{background[0]:02x}{background[1]:02x}{background[2]:02x}"
               + (f" bad={bad_points}" if bad_points else ""))

    # Text band: exp16 used (0,10)-(400,70) and luminance > max(100, bg+40).
    band_x0, band_y0, band_x1, band_y1 = 0, 10, min(width, 400), min(height, 70)
    bg_lum = luminance(background)
    threshold = max(100.0, bg_lum + 40.0)
    bright = 0
    for y in range(band_y0, band_y1):
        for x in range(band_x0, band_x1):
            if luminance(image.getpixel((x, y))) > threshold:
                bright += 1
    checks.add("title_band_brightness", bright >= 100,
               f"{bright} bright pixel(s) > {threshold:.1f} in band "
               f"({band_x0},{band_y0})-({band_x1},{band_y1})")

    # Chips: every 60x60 rect must be exactly its declared fill.
    chips = [op for op in rects if int(op["w"]) == 60 and int(op["h"]) == 60]
    chip_ok = True
    chip_details = []
    for op in chips:
        x0, y0 = int(op["x"]), int(op["y"])
        w, h = int(op["w"]), int(op["h"])
        expected = parse_color(op["fill"])
        mismatched = 0
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                if image.getpixel((x, y)) != expected:
                    mismatched += 1
        if mismatched:
            chip_ok = False
        chip_details.append(
            f"({x0},{y0}) {w}x{h} #{op['fill'].lstrip('#')[:6]}: "
            f"{w * h - mismatched}/{w * h} exact")
    checks.add("chips_exact", chip_ok and bool(chips),
               "; ".join(chip_details) if chip_details else "no 60x60 chips found")

    # Each glyph quad from the IR must contain at least one bright pixel.
    glyph_ok = True
    glyph_details = []
    for run in (op for op in ops if op.get("type") == "text_run"):
        for glyph in run.get("glyphs", []):
            x0, y0 = int(round(glyph["x"])), int(round(glyph["y"]))
            w, h = int(glyph["w"]), int(glyph["h"])
            count = 0
            for y in range(max(0, y0), min(height, y0 + h)):
                for x in range(max(0, x0), min(width, x0 + w)):
                    if luminance(image.getpixel((x, y))) > threshold:
                        count += 1
            if count == 0:
                glyph_ok = False
            glyph_details.append(f"gid={glyph.get('gid')}: {count}px")
    checks.add("glyph_coverage", glyph_ok and bool(glyph_details),
               "; ".join(glyph_details) if glyph_details else "no glyphs in IR")

    header_metrics = {}
    if args.header:
        header = Path(args.header).read_text(encoding="utf-8")

        def macro(name: str) -> int | None:
            match = re.search(rf"^#define\s+{name}\s+(\d+)\s*$", header, re.M)
            return int(match.group(1)) if match else None

        glyph_count = sum(len(op.get("glyphs", []))
                          for op in ops if op.get("type") == "text_run")
        expected = {
            "SCENE_WIDTH": width,
            "SCENE_HEIGHT": height,
            "SCENE_RECT_COUNT": len(rects),
            "SCENE_GLYPH_QUAD_COUNT": glyph_count,
            "SCENE_GLYPH_VERTEX_COUNT": glyph_count * 6,
        }
        mismatches = []
        for name, want in expected.items():
            got = macro(name)
            header_metrics[name] = got
            if got != want:
                mismatches.append(f"{name}={got} want {want}")
        has_arrays = ("kSceneVertices" in header
                      and ("kGlyphVertices" in header or glyph_count == 0)
                      and ("kAtlasPixels" in header or glyph_count == 0))
        has_shaders = ("kRectShaderWGSL" in header
                       and ("kGlyphShaderWGSL" in header or glyph_count == 0))
        checks.add("header_counts", not mismatches and has_arrays and has_shaders,
                   "; ".join(mismatches) if mismatches
                   else f"{glyph_count} glyph(s), rects={len(rects)}, arrays+shaders present")
        if glyph_count:
            atlas_w = macro("ATLAS_WIDTH")
            atlas_h = macro("ATLAS_HEIGHT")
            header_metrics["ATLAS_WIDTH"] = atlas_w
            header_metrics["ATLAS_HEIGHT"] = atlas_h
            checks.add("header_atlas", bool(atlas_w and atlas_h),
                       f"atlas {atlas_w}x{atlas_h}")

    stdout_metrics = {}
    if args.stdout:
        stdout = read_text_any(Path(args.stdout))
        atlas_line = next(
            (line for line in stdout.splitlines() if line.startswith("atlas: uploaded")),
            "")
        adapter_line = next(
            (line for line in stdout.splitlines() if line.startswith("adapter: backend=")),
            "")
        hashes = re.search(r"gpu-hash=0x([0-9A-Fa-f]+).*?cpu-hash=0x([0-9A-Fa-f]+)",
                           atlas_line)
        match = bool(hashes) and hashes.group(1).lower() == hashes.group(2).lower() \
            and "MATCH" in atlas_line
        checks.add("stdout_atlas_match", match, atlas_line or "missing atlas line")
        stdout_metrics["atlas_line"] = atlas_line
        checks.add("stdout_adapter_d3d12", "backend=D3D12" in adapter_line,
                   adapter_line or "missing adapter line")
        stdout_metrics["adapter_line"] = adapter_line

    return {
        "diff": diff,
        "bright_pixels": bright,
        "threshold": threshold,
        "glyphs": len(glyph_details),
        "header": header_metrics,
        "stdout": stdout_metrics,
    }


def rect_checks(args: argparse.Namespace, checks: Checks) -> dict:
    ir = load_ir(args.ir)
    viewport = ir["viewport"]
    width, height = int(viewport["width"]), int(viewport["height"])
    ops = [op for op in ir["ops"] if op.get("type") == "rect"]
    if not ops:
        raise ValueError("IR has no rect ops")

    image = Image.open(args.png).convert("RGBA")
    checks.add("png_size", image.size == (width, height),
               f"{image.size[0]}x{image.size[1]} (expected {width}x{height})")

    expected = Image.new("RGBA", (width, height), parse_color(ops[0]["fill"]))
    draw = ImageDraw.Draw(expected)
    for op in ops:
        x = int(round(float(op["x"])))
        y = int(round(float(op["y"])))
        w = int(round(float(op["w"])))
        h = int(round(float(op["h"])))
        draw.rectangle([x, y, x + w - 1, y + h - 1], fill=parse_color(op["fill"]))

    diff = diff_metrics(image, expected)
    checks.add("rect_reference_parity", diff["same_size"] and diff["maxdiff"] == 0,
               f"maxdiff={diff['maxdiff']} changed={diff['changed_pixels']} "
               f"({diff['changed_pct']}%)")

    return {"diff": diff, "rect_ops": len(ops)}


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify webnative C runtime pixels")
    parser.add_argument("--mode", choices=("text", "rect"), required=True)
    parser.add_argument("--png", required=True, type=Path)
    parser.add_argument("--reference", type=Path,
                        help="text mode: exp16 scene_text.png")
    parser.add_argument("--ir", required=True, type=Path)
    parser.add_argument("--header", type=Path)
    parser.add_argument("--stdout", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()

    if not args.png.exists():
        print(f"error: PNG not found: {args.png}", file=sys.stderr)
        return 1
    if args.mode == "text":
        if not args.reference or not args.reference.exists():
            print("error: text mode needs --reference", file=sys.stderr)
            return 1
        print(f"verify_text: text mode {args.png} vs {args.reference}")
        checks = Checks()
        try:
            metrics = text_checks(args, checks)
        except (ValueError, KeyError, OSError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1
    else:
        print(f"verify_text: rect mode {args.png} vs Pillow reference")
        checks = Checks()
        try:
            metrics = rect_checks(args, checks)
        except (ValueError, KeyError, OSError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1

    report = {
        "mode": args.mode,
        "png": str(args.png),
        "passed": checks.passed,
        "failed": checks.failed,
        "checks": checks.items,
        "metrics": metrics,
    }
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"verify_text: {checks.passed}/{checks.passed + checks.failed} checks passed")
    return 0 if checks.failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
