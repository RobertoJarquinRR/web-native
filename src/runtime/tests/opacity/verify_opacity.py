#!/usr/bin/env python3
"""Pixel acceptance for CSS group opacity (blob v3).

The page is emitted by `frontend-rust --emit-blob` and rendered offscreen by
the runtime with `--document`. The same script runs against D3D11 and wgpu:
the composited values are deterministic because every probe is an axis-aligned
interior pixel.

What it proves:

  * group overlap: a 0.5 group with two overlapping child solids composites the
    result ONCE. The overlap pixel is 0.5*top + 0.5*page, not the naive
    top*0.5 over child*0.5 over page.
  * nested groups: 0.5 inside 0.5 -> 0.25 over the page.
  * leaf solid, text and image: 0.5 applied through the geometry alpha (the
    image through its new per-vertex alpha).
  * opacity: 0 renders nothing.

Usage:
  python verify_opacity.py --png render.png --report layout.json
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image

PAGE_BG = (240, 240, 240)
RED = (255, 0, 0)
BLUE = (0, 0, 255)
IMAGE_QUADRANTS = ((204, 68, 51), (51, 170, 102),
                   (51, 85, 204), (221, 170, 34))
CHECKS: list[dict] = []


def check(group: str, name: str, ok: bool, detail: str) -> bool:
    CHECKS.append({"group": group, "name": name, "ok": bool(ok), "detail": detail})
    print(f"[{'ok  ' if ok else 'FAIL'}] {group}: {name} - {detail}")
    return bool(ok)


def px(img: Image.Image, x: float, y: float) -> tuple[int, int, int]:
    return img.getpixel((int(round(x)), int(round(y))))[:3]


def blend(color: tuple[int, int, int], alpha: float,
          background: tuple[int, int, int] = PAGE_BG) -> tuple[int, int, int]:
    return tuple(int(round(c * alpha + b * (1.0 - alpha)))
                 for c, b in zip(color, background))


def close(actual: tuple[int, int, int], expected: tuple[int, int, int],
          tolerance: int) -> bool:
    return all(abs(a - b) <= tolerance for a, b in zip(actual, expected))


def rect_of(nodes: list[dict], suffix: str) -> tuple[float, float, float, float]:
    for node in nodes:
        if str(node.get("name", "")).endswith(suffix):
            return (node["x"], node["y"], node["w"], node["h"])
    raise KeyError(suffix)


def region_uniform(img: Image.Image, rect: tuple[float, float, float, float],
                   color: tuple[int, int, int]) -> tuple[int, str | None]:
    x, y, w, h = rect
    first = None
    bad = 0
    for py in range(int(round(y)), int(round(y + h))):
        for pxx in range(int(round(x)), int(round(x + w))):
            if px(img, pxx, py) != color:
                bad += 1
                if first is None:
                    first = f"({pxx},{py})={px(img, pxx, py)}"
    return bad, first


def main() -> int:
    parser = argparse.ArgumentParser(description="group opacity pixel verifier")
    parser.add_argument("--png", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--backend", default="?")
    args = parser.parse_args()

    img = Image.open(args.png).convert("RGB")
    report = json.loads(Path(args.report).read_text(encoding="utf-8"))
    sizes = {f"{item['width']}x{item['height']}": item for item in report["sizes"]}
    if "400x300" not in sizes:
        print(f"FAIL: report has no 400x300 size: {sorted(sizes)}")
        return 1
    nodes = sizes["400x300"]["nodes"]
    backend = args.backend
    ok = check("capture", "PNG size", img.size == (400, 300), f"{img.size}")

    # ---------------------------------------------------- group overlap
    gx, gy, gw, gh = rect_of(nodes, "#overlap.group")
    ok = check("layout", "overlap group rect", (gx, gy, gw, gh) == (20, 20, 100, 100),
               f"({gx},{gy},{gw},{gh})") and ok
    only_a = px(img, gx + 10, gy + gh / 2)
    overlap = px(img, gx + 50, gy + gh / 2)
    only_b = px(img, gx + 90, gy + gh / 2)
    expected_a = blend(RED, 0.5)
    expected_b = blend(BLUE, 0.5)
    naive_overlap = blend(BLUE, 0.5, blend(RED, 0.5))
    ok = check(f"group/{backend}", "child A composites once",
               close(only_a, expected_a, 2),
               f"{only_a} == {expected_a}") and ok
    ok = check(f"group/{backend}", "child B composites once",
               close(only_b, expected_b, 2),
               f"{only_b} == {expected_b}") and ok
    ok = check(f"group/{backend}", "overlap is grouped, not double-composited",
               close(overlap, expected_b, 2) and not close(overlap, naive_overlap, 6),
               f"{overlap} == {expected_b} (naive {naive_overlap})") and ok

    # ---------------------------------------------------- nested groups
    fx, fy, fw, fh = rect_of(nodes, "#fill.fill")
    nested = px(img, fx + fw / 2, fy + fh / 2)
    expected_nested = blend(BLUE, 0.25)
    ok = check(f"nested/{backend}", "0.5 inside 0.5 composites to 0.25",
               close(nested, expected_nested, 2),
               f"{nested} == {expected_nested}") and ok

    # ---------------------------------------------------- leaf solid
    sx, sy, sw, sh = rect_of(nodes, "#leaf-solid.leaf-solid")
    solid = px(img, sx + sw / 2, sy + sh / 2)
    ok = check(f"leaf/{backend}", "leaf solid alpha 0.5",
               close(solid, expected_a, 2), f"{solid} == {expected_a}") and ok

    # ---------------------------------------------------- leaf text
    tx, ty, tw, th = rect_of(nodes, "#leaf-text.leaf-text")
    darkest = 255
    darkest_at = (0, 0)
    for py in range(int(round(ty)), int(round(ty + th))):
        for pxx in range(int(round(tx)), int(round(tx + tw))):
            value = px(img, pxx, py)
            luminance = 0.299 * value[0] + 0.587 * value[1] + 0.114 * value[2]
            if luminance < darkest:
                darkest = luminance
                darkest_at = (pxx, py)
    # A 48 px "H" has fully covered stem pixels: black at 0.5 over the page.
    expected_text = 0.5 * 0.0 + 0.5 * 240.0
    ok = check(f"leaf/{backend}", "leaf text glyph alpha 0.5",
               115.0 <= darkest <= 126.0 and abs(darkest - expected_text) <= 6.0,
               f"darkest={darkest:.1f} at {darkest_at} (expected ~{expected_text:.1f})") and ok

    # ---------------------------------------------------- leaf image
    ix, iy, iw, ih = rect_of(nodes, "#leaf-image.leaf-image")
    probes = ((ix + 15, iy + 15, IMAGE_QUADRANTS[0]),
              (ix + iw - 15, iy + 15, IMAGE_QUADRANTS[1]),
              (ix + 15, iy + ih - 15, IMAGE_QUADRANTS[2]),
              (ix + iw - 15, iy + ih - 15, IMAGE_QUADRANTS[3]))
    details = []
    image_ok = True
    for pxx, pyy, color in probes:
        actual = px(img, pxx, pyy)
        expected = blend(color, 0.5)
        image_ok = image_ok and close(actual, expected, 4)
        details.append(f"({pxx},{pyy})={actual}~{expected}")
    ok = check(f"leaf/{backend}", "leaf image vertex alpha 0.5", image_ok,
               ", ".join(details)) and ok

    # ---------------------------------------------------- opacity: 0
    zx, zy, zw, zh = rect_of(nodes, "#zero.group")
    bad, first = region_uniform(img, (zx, zy, zw, zh), PAGE_BG)
    ok = check(f"zero/{backend}", "opacity 0 group paints nothing",
               bad == 0, f"{bad} non-page pixels ({first})") and ok

    failed = [item for item in CHECKS if not item["ok"]]
    result = {
        "ok": not failed,
        "backend": backend,
        "total_checks": len(CHECKS),
        "failed_checks": len(failed),
        "checks": CHECKS,
    }
    Path(args.png).with_suffix(".json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"\nopacity verify ({backend}): {len(CHECKS) - len(failed)}/{len(CHECKS)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
