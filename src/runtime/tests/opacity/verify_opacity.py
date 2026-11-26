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
  * leaves are groups too: a leaf and the same box with an extra child are
    pixel-identical, and every multi-pass leaf (border+fill, shadow+fill,
    input ring/body/selection, text/image over a background) matches the
    flattened group result instead of fading each pass separately.
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
GREEN = (0, 255, 0)
BLACK = (0, 0, 0)
FOCUS = (0x3B, 0x82, 0xF6)
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


def src_over(top: tuple[int, int, int], alpha: float,
             bottom: tuple[int, int, int]) -> tuple[int, int, int]:
    return tuple(int(round(t * alpha + b * (1.0 - alpha)))
                 for t, b in zip(top, bottom))


def close(actual: tuple[int, int, int], expected: tuple[int, int, int],
          tolerance: int) -> bool:
    return all(abs(a - b) <= tolerance for a, b in zip(actual, expected))


def shadow_alpha() -> float:
    """Accumulated alpha of the runtime's three stacked box-shadow quads."""
    alpha = 0.0
    for layer in (0.50, 0.35, 0.25):
        sample = round(255 * layer) / 255.0
        alpha = alpha + sample * (1.0 - alpha)
    return alpha


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


def darkest_luma(img: Image.Image, rect: tuple[float, float, float, float]):
    x, y, w, h = rect
    darkest = 255.0
    darkest_at = (0, 0)
    darkest_px = (0, 0, 0)
    for py in range(int(round(y)), int(round(y + h))):
        for pxx in range(int(round(x)), int(round(x + w))):
            value = px(img, pxx, py)
            luminance = 0.299 * value[0] + 0.587 * value[1] + 0.114 * value[2]
            if luminance < darkest:
                darkest = luminance
                darkest_at = (pxx, py)
                darkest_px = value
    return darkest, darkest_at, darkest_px


def main() -> int:
    parser = argparse.ArgumentParser(description="group opacity pixel verifier")
    parser.add_argument("--png", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--backend", default="?")
    args = parser.parse_args()

    img = Image.open(args.png).convert("RGB")
    report = json.loads(Path(args.report).read_text(encoding="utf-8"))
    size_key = f"{img.size[0]}x{img.size[1]}"
    sizes = {f"{item['width']}x{item['height']}": item for item in report["sizes"]}
    if size_key not in sizes:
        print(f"FAIL: report has no {size_key} size: {sorted(sizes)}")
        return 1
    nodes = sizes[size_key]["nodes"]
    backend = args.backend
    ok = check("capture", "PNG size", img.size == (400, 500), f"{img.size}")

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

    # ---------------------------------------------------- single-pass leaves
    sx, sy, sw, sh = rect_of(nodes, "#leaf-solid.leaf-solid")
    solid = px(img, sx + sw / 2, sy + sh / 2)
    ok = check(f"leaf/{backend}", "leaf solid goes through a layer at 0.5",
               close(solid, expected_a, 2), f"{solid} == {expected_a}") and ok

    tx, ty, tw, th = rect_of(nodes, "#leaf-text.leaf-text")
    darkest, darkest_at, _ = darkest_luma(img, (tx, ty, tw, th))
    # A 48 px "H" has fully covered stem pixels: black at 0.5 over the page.
    expected_text = 0.5 * 0.0 + 0.5 * 240.0
    ok = check(f"leaf/{backend}", "leaf text glyph alpha 0.5",
               115.0 <= darkest <= 126.0 and abs(darkest - expected_text) <= 6.0,
               f"darkest={darkest:.1f} at {darkest_at} (expected ~{expected_text:.1f})") and ok

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

    # ------------------------------------------- multi-pass leaves (B1)
    bx, by, bw, bh = rect_of(nodes, "#leaf-border.leaf-border")
    px_border = px(img, bx + 5, by + bh / 2)
    px_fill = px(img, bx + bw / 2, by + bh / 2)
    ok = check(f"leaf/{backend}", "bordered leaf border flattens once",
               close(px_border, blend(BLUE, 0.5), 3),
               f"{px_border} == {blend(BLUE, 0.5)}") and ok
    ok = check(f"leaf/{backend}", "bordered leaf fill flattens once",
               close(px_fill, blend(RED, 0.5), 3),
               f"{px_fill} == {blend(RED, 0.5)}") and ok

    pbx, pby, pbw, pbh = rect_of(nodes, "#leaf-border-pair.leaf-border")
    px_pair_border = px(img, pbx + 5, pby + pbh / 2)
    px_pair_fill = px(img, pbx + pbw / 2, pby + pbh / 2)
    ok = check(f"leaf/{backend}", "same DOM with/without a child: border identical",
               close(px_pair_border, px_border, 1),
               f"{px_pair_border} == {px_border}") and ok
    ok = check(f"leaf/{backend}", "same DOM with/without a child: fill identical",
               close(px_pair_fill, px_fill, 1),
               f"{px_pair_fill} == {px_fill}") and ok

    hx, hy, hw, hh = rect_of(nodes, "#leaf-shadow.leaf-shadow")
    shadow = px(img, hx - 5, hy + hh / 2)
    expected_shadow = blend(BLUE, shadow_alpha() * 0.5)
    direct_shadow = blend(BLUE, (1.0 - (1 - 64 / 255) * (1 - 45 / 255) *
                                  (1 - 32 / 255)))
    ok = check(f"leaf/{backend}", "box-shadow leaf stacks then flattens",
               close(shadow, expected_shadow, 4) and
               not close(shadow, direct_shadow, 8),
               f"{shadow} == {expected_shadow} (direct {direct_shadow})") and ok
    center = px(img, hx + hw / 2, hy + hh / 2)
    ok = check(f"leaf/{backend}", "box-shadow leaf fill stays grouped",
               close(center, blend(RED, 0.5), 3),
               f"{center} == {blend(RED, 0.5)}") and ok

    ax, ay, aw, ah = rect_of(nodes, "#leaf-input.leaf-input")
    body = px(img, ax + aw - 20, ay + ah / 2)
    ring = px(img, ax + aw / 2, ay + 1)
    selection = px(img, ax + 14, ay + ah - 10)
    expected_selection = blend(src_over(FOCUS, 102 / 255.0, RED), 0.5)
    ok = check(f"leaf/{backend}", "input leaf body flattens once",
               close(body, blend(RED, 0.5), 3),
               f"{body} == {blend(RED, 0.5)}") and ok
    ok = check(f"leaf/{backend}", "input leaf focus ring flattens once",
               close(ring, blend(FOCUS, 0.5), 4),
               f"{ring} == {blend(FOCUS, 0.5)}") and ok
    ok = check(f"leaf/{backend}", "input leaf selection flattens once",
               close(selection, expected_selection, 6),
               f"{selection} == {expected_selection}") and ok

    tgx, tgy, tgw, tgh = rect_of(nodes, "#leaf-text-bg.leaf-text-bg")
    _, bg_at, bg_px = darkest_luma(img, (tgx, tgy, tgw, tgh))
    spread = max(bg_px) - min(bg_px)
    ok = check(f"leaf/{backend}", "text over background is flattened (neutral stem)",
               abs(bg_px[0] - 120) <= 6 and spread <= 8,
               f"darkest={bg_px} at {bg_at} (spread {spread}, direct would be ~(124,60,60))") and ok

    igx, igy, igw, igh = rect_of(nodes, "#leaf-image-bg.leaf-image-bg")
    # padding 10 -> the image content is inset by 10 on each side.
    image_probes = (
        (igx + 10 + 15, igy + 10 + 10, IMAGE_QUADRANTS[0]),
        (igx + igw - 25, igy + 10 + 10, IMAGE_QUADRANTS[1]),
        (igx + 10 + 15, igy + igh - 20, IMAGE_QUADRANTS[2]),
        (igx + igw - 25, igy + igh - 20, IMAGE_QUADRANTS[3]),
    )
    details = []
    image_ok = True
    for pxx, pyy, color in image_probes:
        actual = px(img, pxx, pyy)
        expected = blend(color, 0.5)
        image_ok = image_ok and close(actual, expected, 4)
        details.append(f"({pxx},{pyy})={actual}~{expected}")
    ok = check(f"leaf/{backend}", "image over background flattens once",
               image_ok, ", ".join(details)) and ok

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
