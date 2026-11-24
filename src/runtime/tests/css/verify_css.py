#!/usr/bin/env python3
"""Pixel + header acceptance for CSS core v1 (rounded bg, border, shadow).

Two pages share the same layout and colours:

  card.json    - card with border-radius 12, 2 px border, box-shadow; an
                 image card whose radius comes from a parent overflow:hidden;
                 a transparent card with a border (v1 skips the ring).
  control.json - identical geometry with no CSS v1 fields at all. Every pixel
                 of the old square path is asserted, and the render must match
                 the pre-CSS-core reference PNG byte-for-byte (same pixels).

Usage:
  python verify_css.py --dir out --page page [--expected expected]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image, ImageChops

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[4]
sys.path.insert(0, str(REPO / "src" / "emitter" / "python"))

from emit_ui import parse_header  # noqa: E402

PAGE_BG = (240, 240, 240)
CARD = (51, 102, 204)
BORDER = (255, 255, 255)
IMAGE_QUADRANTS = ((204, 68, 51), (51, 170, 102),
                   (51, 85, 204), (221, 170, 34))
SHADOW_ALPHA_RGB = 0x66000000
CHECKS: list[dict] = []


def check(group: str, name: str, ok: bool, detail: str) -> bool:
    CHECKS.append({"group": group, "name": name, "ok": bool(ok), "detail": detail})
    print(f"[{'ok  ' if ok else 'FAIL'}] {group}: {name} - {detail}")
    return bool(ok)


def px(img: Image.Image, x: float, y: float) -> tuple[int, int, int]:
    return img.getpixel((int(round(x)), int(round(y))))[:3]


def luminance(rgb: tuple[int, int, int]) -> float:
    return 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]


def rect_of(nodes: list[dict], suffix: str) -> tuple[int, int, int, int]:
    for node in nodes:
        if str(node.get("name", "")).endswith(suffix):
            return (int(round(node["x"])), int(round(node["y"])),
                    int(round(node["w"])), int(round(node["h"])))
    raise KeyError(suffix)


def check_color(actual: tuple[int, int, int], expected: tuple[int, int, int],
                tolerance: int = 0) -> bool:
    return all(abs(a - b) <= tolerance for a, b in zip(actual, expected))


def region_uniform(img: Image.Image, rect: tuple[int, int, int, int],
                   color: tuple[int, int, int]) -> tuple[int, str | None]:
    x0, y0, w, h = rect
    bad = 0
    first = None
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            if img.getpixel((x, y))[:3] != color:
                bad += 1
                if first is None:
                    first = f"({x},{y})={img.getpixel((x, y))[:3]}"
    return bad, first


def header_checks(root: Path) -> bool:
    ok = True
    card_header = parse_header(root / "emit" / "card" / "ui_data.h")
    control_header = parse_header(root / "emit" / "control" / "ui_data.h")
    none_header = parse_header(root / "emit" / "none" / "ui_data.h")
    legacy_header = parse_header(HERE / "fixture" / "legacy_ui_data.h")

    card = card_header["nodes"][1]
    expected = {
        "border_width": 2.0,
        "border_color": 0xFFFFFFFF,
        "border_radius": 12.0,
        "shadow_x": 0.0,
        "shadow_y": 10.0,
        "shadow_blur": 6.0,
        "shadow_spread": 0.0,
        "shadow_color": SHADOW_ALPHA_RGB,
    }
    for key, value in expected.items():
        ok = check("emit/card", key, card[key] == value,
                   f"{card[key]!r} == {value!r}") and ok
    ok = check("emit/card", "node count", card_header["node_count"] == 5,
               f"{card_header['node_count']} nodes") and ok

    control = control_header["nodes"][1]
    zeros = all(control[key] == 0 for key in
                ("border_width", "border_color", "border_radius", "shadow_x",
                 "shadow_y", "shadow_blur", "shadow_spread", "shadow_color"))
    ok = check("emit/control", "missing fields default to 0", zeros,
               f"border_radius={control['border_radius']} shadow=0x{control['shadow_color']:08x}") and ok

    none_node = none_header["nodes"][1]
    ok = check("emit/none", "border_style none forces width 0",
               none_node["border_width"] == 0.0 and none_node["border_radius"] == 0.0,
               f"width={none_node['border_width']} radius={none_node['border_radius']}") and ok

    legacy = legacy_header["nodes"][1]
    legacy_zero = all(legacy[key] == 0 for key in
                      ("border_width", "border_color", "border_radius", "shadow_x",
                       "shadow_y", "shadow_blur", "shadow_spread", "shadow_color"))
    ok = check("emit/legacy", "old header parses with CSS fields = 0",
               legacy_header["node_count"] == 2 and legacy_zero,
               f"{legacy_header['node_count']} nodes, css fields zero={legacy_zero}") and ok
    return ok


def card_pixel_checks(img: Image.Image, nodes: list[dict]) -> bool:
    ok = True
    x0, y0, w, h = rect_of(nodes, "div.card")
    ok = check("layout/card", "border box", (x0, y0, w, h) == (20, 20, 160, 100),
               f"({x0},{y0},{w},{h}) == (20,20,160,100)") and ok

    corners = ((x0, y0), (x0 + w - 1, y0), (22, 22), (x0 + w - 3, 22))
    ok = check("raster/card", "rounded corners are page background",
               all(px(img, x, y) == PAGE_BG for x, y in corners),
               ", ".join(f"{p}={px(img, *p)}" for p in corners)) and ok
    ok = check("raster/card", "center is the card background",
               px(img, 100, 70) == CARD, f"{px(img, 100, 70)}") and ok
    ok = check("raster/card", "inside the rounded corner is background",
               px(img, 26, 26) == CARD, f"{px(img, 26, 26)}") and ok

    border_points = ((x0, y0 + h / 2), (x0 + w - 1, y0 + h / 2),
                     (x0 + w / 2, y0), (x0 + w / 2, y0 + h - 1))
    ok = check("raster/card", "2 px border on all straight edges",
               all(px(img, x, y) == BORDER for x, y in border_points),
               ", ".join(f"{p}={px(img, *p)}" for p in border_points)) and ok
    ok = check("raster/card", "inner background starts after the border",
               px(img, x0 + 3, y0 + h / 2) == CARD,
               f"{px(img, x0 + 3, y0 + h / 2)}") and ok

    all_layers = px(img, 100, 123)
    mid_layer = px(img, 100, 131)
    outer_layer = px(img, 100, 135)
    ok = check("raster/card", "shadow is visible below the card",
               all(value != PAGE_BG for value in
                   (all_layers, mid_layer, outer_layer)),
               f"all={all_layers} mid={mid_layer} outer={outer_layer}") and ok
    ok = check("raster/card", "three translucent layers darken towards the box",
               luminance(all_layers) < luminance(mid_layer) < luminance(outer_layer)
               < luminance(PAGE_BG),
               f"lum all={luminance(all_layers):.1f} mid={luminance(mid_layer):.1f} "
               f"outer={luminance(outer_layer):.1f} page={luminance(PAGE_BG):.1f}") and ok
    ok = check("raster/card", "shadow does not leak past the blur cap",
               px(img, 186, 70) == PAGE_BG and px(img, 100, 142) == PAGE_BG,
               f"{px(img, 186, 70)} / {px(img, 100, 142)}") and ok

    ghost = rect_of(nodes, "div.ghost-card")
    bad, first = region_uniform(img, ghost, PAGE_BG)
    ok = check("raster/card", "transparent background skips the border ring",
               bad == 0, f"{bad} non-page pixels ({first})") and ok
    magenta = 0
    for y in range(img.height):
        for x in range(img.width):
            if img.getpixel((x, y))[:3] == (255, 0, 255):
                magenta += 1
    ok = check("raster/card", "no border color anywhere", magenta == 0,
               f"{magenta} magenta pixels") and ok
    return ok


def image_card_checks(img: Image.Image, nodes: list[dict], rounded: bool) -> bool:
    ok = True
    x0, y0, w, h = rect_of(nodes, "div.image-card")
    group = "raster/image" if rounded else "raster/control-image"
    ok = check("layout/image", "border box", (x0, y0, w, h) == (200, 20, 120, 120),
               f"({x0},{y0},{w},{h}) == (200,20,120,120)") and ok
    tl, tr, bl, br = IMAGE_QUADRANTS
    if rounded:
        corner_points = ((x0, y0), (x0 + w - 1, y0), (x0, y0 + h - 1),
                         (x0 + w - 1, y0 + h - 1), (201, 21), (202, 22))
        ok = check(group, "parent radius rounds the image corners",
                   all(px(img, x, y) == PAGE_BG for x, y in corner_points),
                   ", ".join(f"{p}={px(img, *p)}" for p in corner_points)) and ok
        probes = ((x0 + 7, y0 + 7, tl), (x0 + w - 7, y0 + 7, tr),
                  (x0 + 7, y0 + h - 7, bl), (x0 + w - 7, y0 + h - 7, br))
        ok = check(group, "rounded corners keep object-fit UVs",
                   all(check_color(px(img, x, y), color, 3) for x, y, color in probes),
                   ", ".join(f"({p[0]},{p[1]})={px(img, p[0], p[1])}" for p in probes)) and ok
    else:
        corner_points = ((x0, y0, tl), (x0 + w - 1, y0, tr),
                         (x0, y0 + h - 1, bl), (x0 + w - 1, y0 + h - 1, br))
        ok = check(group, "square image corner colours",
                   all(px(img, x, y) == color for x, y, color in corner_points),
                   ", ".join(f"({x},{y})={px(img, x, y)}" for x, y, _ in corner_points)) and ok

    quadrants = ((x0 + 30, y0 + 30, tl), (x0 + 90, y0 + 30, tr),
                 (x0 + 30, y0 + 90, bl), (x0 + 90, y0 + 90, br))
    ok = check(group, "four texture quadrants at the expected UVs",
               all(check_color(px(img, x, y), color, 3) for x, y, color in quadrants),
               ", ".join(f"({x},{y})={px(img, x, y)}" for x, y, _ in quadrants)) and ok
    return ok


def control_pixel_checks(img: Image.Image, nodes: list[dict],
                         expected_dir: Path) -> bool:
    ok = True
    x0, y0, w, h = rect_of(nodes, "div.card")
    ok = check("layout/control", "border box", (x0, y0, w, h) == (20, 20, 160, 100),
               f"({x0},{y0},{w},{h}) == (20,20,160,100)") and ok
    bad, first = region_uniform(img, (x0, y0, w, h), CARD)
    ok = check("raster/control", "card is the exact old square rectangle",
               bad == 0, f"{bad} non-card pixels ({first})") and ok
    corners = ((x0, y0), (x0 + w - 1, y0), (x0, y0 + h - 1),
               (x0 + w - 1, y0 + h - 1))
    ok = check("raster/control", "square corners are card background",
               all(px(img, x, y) == CARD for x, y in corners),
               ", ".join(f"{p}={px(img, *p)}" for p in corners)) and ok
    no_shadow = ((100, 123), (100, 131), (100, 135), (186, 70))
    ok = check("raster/control", "no shadow pixels",
               all(px(img, x, y) == PAGE_BG for x, y in no_shadow),
               ", ".join(f"{p}={px(img, *p)}" for p in no_shadow)) and ok
    ok = check("raster/control", "page background around the card",
               px(img, 2, 2) == PAGE_BG and px(img, 395, 235) == PAGE_BG,
               f"{px(img, 2, 2)} / {px(img, 395, 235)}") and ok

    ghost = rect_of(nodes, "div.ghost-card")
    bad, first = region_uniform(img, ghost, PAGE_BG)
    ok = check("raster/control", "transparent card paints nothing",
               bad == 0, f"{bad} non-page pixels ({first})") and ok

    reference = expected_dir / "control_legacy.png"
    if not reference.is_file():
        ok = check("raster/control", "pre-CSS reference image", False,
                   f"missing {reference}") and ok
    else:
        legacy = Image.open(reference).convert("RGB")
        diff = ImageChops.difference(img, legacy)
        bbox = diff.getbbox()
        ok = check("raster/control", "pixel-identical to the pre-CSS renderer",
                   bbox is None,
                   "0 pixel differences" if bbox is None else f"diff bbox={bbox}") and ok
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description="CSS core v1 verifier")
    parser.add_argument("--dir", default=str(HERE / "out"))
    parser.add_argument("--page", default=str(HERE / "page"))
    parser.add_argument("--expected", default=str(HERE / "expected"))
    args = parser.parse_args()
    root = Path(args.dir).resolve()
    expected_dir = Path(args.expected).resolve()

    ok = header_checks(root)

    report_path = root / "layout_card.json"
    if not report_path.is_file():
        print(f"FAIL: missing {report_path}")
        return 1
    report = json.loads(report_path.read_text(encoding="utf-8"))
    sizes = {f"{item['width']}x{item['height']}": item for item in report["sizes"]}
    if "400x240" not in sizes:
        print(f"FAIL: layout report has no 400x240 size: {sorted(sizes)}")
        return 1
    nodes = sizes["400x240"]["nodes"]

    card = Image.open(root / "render_card.png").convert("RGB")
    control = Image.open(root / "render_control.png").convert("RGB")
    ok = check("raster", "card PNG size", card.size == (400, 240), f"{card.size}") and ok
    ok = check("raster", "control PNG size", control.size == (400, 240), f"{control.size}") and ok
    if card.size == (400, 240) and control.size == (400, 240):
        ok = card_pixel_checks(card, nodes) and ok
        ok = image_card_checks(card, nodes, rounded=True) and ok
        ok = image_card_checks(control, nodes, rounded=False) and ok
        ok = control_pixel_checks(control, nodes, expected_dir) and ok

        ok = check("raster/card", "page background",
                   px(card, 2, 2) == PAGE_BG and px(card, 190, 10) == PAGE_BG,
                   f"{px(card, 2, 2)} / {px(card, 190, 10)}") and ok
        ok = check("raster/card", "rounded card differs from the square control",
                   px(card, 20, 20) == PAGE_BG and px(control, 20, 20) == CARD
                   and px(card, 100, 70) == px(control, 100, 70),
                   f"corner card={px(card, 20, 20)} control={px(control, 20, 20)}") and ok

    failed = [item for item in CHECKS if not item["ok"]]
    result = {
        "ok": not failed,
        "total_checks": len(CHECKS),
        "failed_checks": len(failed),
        "checks": CHECKS,
    }
    (root / "css_verify_report.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"\ncss verify: {len(CHECKS) - len(failed)}/{len(CHECKS)} checks passed")
    if failed:
        for item in failed:
            print(f"  FAILED {item['group']}: {item['name']} - {item['detail']}")
        return 1
    print("css verify: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
