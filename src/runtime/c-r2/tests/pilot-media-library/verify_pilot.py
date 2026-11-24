#!/usr/bin/env python3
"""Acceptance for the ui-tree/2 pilot page: grid, scroll/clip and images.

Reads the offscreen renders + layout report + runtime logs produced by
run_pilot.ps1 and asserts:

  1. grid: 6 columns at 1280, 3 columns at 800 (repeat auto-fill, row-major),
  2. images: object-fit fill/cover/contain colors and letterboxing,
  3. clipping: the scroll container stops painting at its content box,
  4. wheel: the runtime applies +clamp and only the content area changes.

Usage: python verify_pilot.py [--dir out]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from PIL import Image, ImageChops

CHECKS: list[dict] = []

RED = (231, 76, 60)
ORANGE = (243, 156, 18)
YELLOW = (241, 196, 15)
CARD = (26, 31, 42)
SCROLL = (16, 20, 27)
ROOT = (11, 14, 20)
HEADER = (18, 24, 36)


def check(group: str, name: str, ok: bool, detail: str) -> None:
    CHECKS.append({"group": group, "name": name, "ok": bool(ok), "detail": detail})
    print(f"[{'ok  ' if ok else 'FAIL'}] {group}: {name} - {detail}")


def rgb(img: Image.Image, x: float, y: float) -> tuple[int, int, int]:
    return img.convert("RGB").getpixel((int(round(x)), int(round(y))))


def node(nodes: list[dict], suffix: str) -> dict:
    for item in nodes:
        if item["name"].endswith(suffix):
            return item
    raise KeyError(suffix)


def parse_log(path: Path) -> dict:
    raw = path.read_bytes()
    if raw.startswith(b"\xff\xfe") or raw.startswith(b"\xfe\xff"):
        text = raw.decode("utf-16", errors="replace")
    else:
        text = raw.decode("utf-8", errors="replace")
    scroll = re.search(r"scroll: node=2 .*?offset=([0-9.]+) max=([0-9.]+)", text)
    render = re.search(r"render: .*?image_vertices=(\d+) layout=([0-9.]+)ms "
                       r"frame=([0-9.]+)ms", text)
    out = {"text": text}
    if scroll:
        out["offset"] = float(scroll.group(1))
        out["max"] = float(scroll.group(2))
    if render:
        out["image_vertices"] = int(render.group(1))
        out["layout_ms"] = float(render.group(2))
        out["frame_ms"] = float(render.group(3))
    return out


def diff(a: Image.Image, b: Image.Image) -> tuple[int, tuple[int, int, int, int] | None]:
    delta = ImageChops.difference(a.convert("RGB"), b.convert("RGB"))
    hist = delta.histogram()
    changed = sum(value for index, value in enumerate(hist) if index % 256 != 0)
    return changed, delta.getbbox()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", default=str(Path(__file__).resolve().parent / "out"))
    args = parser.parse_args()
    root = Path(args.dir).resolve()
    report_path = root / "pilot_layout.json"
    if not report_path.is_file():
        print(f"FAIL: {report_path} is missing")
        return 1
    report = json.loads(report_path.read_text(encoding="utf-8"))
    sizes = {item["width"]: item for item in report["sizes"]}

    images_1280 = root / "render_1280.png"
    images_800 = root / "render_800.png"
    for path in (images_1280, images_800, root / "wheel_800.png",
                 root / "wheel_1280.png", root / "wheel_max.png",
                 root / "wheel_outside.png"):
        if not path.is_file():
            print(f"FAIL: {path} is missing")
            return 1
    img1280 = Image.open(images_1280)
    img800 = Image.open(images_800)

    # ---------------------------------------------------------------- grid
    for width, columns in ((1280, 6), (800, 3)):
        nodes = sizes[width]["nodes"]
        posters = [node(nodes, f"img.poster[{i}]") for i in range(18)]
        group = f"grid@{width}"
        first_row = posters[:columns]
        next_row = posters[columns]
        check(group, f"{columns} columns", all(abs(p["y"] - 80.0) < 0.5 for p in first_row),
              f"y={[p['y'] for p in first_row]}")
        check(group, "row-major wrap", abs(next_row["y"] - 336.0) < 0.5,
              f"poster[{columns}].y={next_row['y']} (expected 336)")
        check(group, "column pitch 180+16",
              all(abs(first_row[i + 1]["x"] - first_row[i]["x"] - 196.0) < 0.5
                  for i in range(columns - 1)),
              f"x={[p['x'] for p in first_row]}")
        check(group, "items are 180x240",
              all(abs(p["w"] - 180.0) < 0.5 and abs(p["h"] - 240.0) < 0.5
                  for p in posters),
              f"w/h={posters[0]['w']}x{posters[0]['h']}")

    nodes = sizes[1280]["nodes"]
    cover = node(nodes, "img.poster[0]")
    fill = node(nodes, "img.poster[1]")
    contain = node(nodes, "img.poster[2]")

    # ------------------------------------------------------------ object-fit
    group = "object-fit@1280"
    cx = int(cover["x"])
    cy = int(cover["y"])
    check(group, "cover fills the box vertically", rgb(img1280, cx + 90, cy + 8) == ORANGE,
          f"top={rgb(img1280, cx + 90, cy + 8)} (source band 2)")
    check(group, "cover crops source horizontally",
          rgb(img1280, cx + 10, cy + 120) == RED and
          rgb(img1280, cx + 50, cy + 120) == ORANGE and
          rgb(img1280, cx + 170, cy + 120) == YELLOW,
          f"x10={rgb(img1280, cx + 10, cy + 120)} x50={rgb(img1280, cx + 50, cy + 120)} "
          f"x170={rgb(img1280, cx + 170, cy + 120)}")
    fx = int(fill["x"])
    fy = int(fill["y"])
    check(group, "fill stretches all three bands",
          rgb(img1280, fx + 10, fy + 120) == RED and
          rgb(img1280, fx + 50, fy + 120) == RED and
          rgb(img1280, fx + 70, fy + 120) == ORANGE and
          rgb(img1280, fx + 130, fy + 120) == YELLOW,
          f"x10={rgb(img1280, fx + 10, fy + 120)} x50={rgb(img1280, fx + 50, fy + 120)} "
          f"x70={rgb(img1280, fx + 70, fy + 120)} x130={rgb(img1280, fx + 130, fy + 120)}")
    tx = int(contain["x"])
    ty = int(contain["y"])
    check(group, "contain letterboxes top/bottom",
          rgb(img1280, tx + 90, ty + 8) == CARD and rgb(img1280, tx + 90, ty + 232) == CARD,
          f"top={rgb(img1280, tx + 90, ty + 8)} bottom={rgb(img1280, tx + 90, ty + 232)}")
    check(group, "contain keeps the whole source width",
          rgb(img1280, tx + 10, ty + 120) == RED and
          rgb(img1280, tx + 90, ty + 120) == ORANGE and
          rgb(img1280, tx + 170, ty + 120) == YELLOW,
          f"x10={rgb(img1280, tx + 10, ty + 120)} x90={rgb(img1280, tx + 90, ty + 120)} "
          f"x170={rgb(img1280, tx + 170, ty + 120)}")

    # -------------------------------------------------------------- clipping
    group = "clip@1280"
    scroll_rect = sizes[1280]["nodes"][2]
    check(group, "scroll container rect",
          abs(scroll_rect["x"] - 24.0) < 0.5 and abs(scroll_rect["w"] - 1232.0) < 0.5,
          f"{scroll_rect['x']},{scroll_rect['y']},{scroll_rect['w']},{scroll_rect['h']}")
    check(group, "footer below the scroll box",
          rgb(img1280, 400, 510) == CARD, f"{rgb(img1280, 400, 510)}")
    check(group, "no image bleed into the gap below the clip",
          rgb(img1280, 400, 470) == SCROLL, f"{rgb(img1280, 400, 470)}")
    check(group, "root background outside the layout",
          rgb(img1280, 400, 580) == ROOT, f"{rgb(img1280, 400, 580)}")
    check(group, "header background", rgb(img1280, 60, 40) == HEADER,
          f"{rgb(img1280, 60, 40)}")

    # ---------------------------------------------------------------- wheel
    log_1280 = parse_log(root / "render_1280.log")
    log_800 = parse_log(root / "render_800.log")
    wheel_1280 = parse_log(root / "wheel_1280.log")
    wheel_800 = parse_log(root / "wheel_800.log")
    wheel_max = parse_log(root / "wheel_max.log")
    wheel_outside = parse_log(root / "wheel_outside.log")
    group = "wheel"
    check(group, "1280 scroll max = 752 - 388",
          abs(log_1280.get("max", -1) - 364.0) < 0.5, f"max={log_1280.get('max')}")
    check(group, "800 scroll max = 1520 - 388",
          abs(log_800.get("max", -1) - 1132.0) < 0.5, f"max={log_800.get('max')}")
    check(group, "wheel applies delta_y at 1280",
          abs(wheel_1280.get("offset", -1) - 200.0) < 0.5,
          f"offset={wheel_1280.get('offset')} max={wheel_1280.get('max')}")
    check(group, "wheel applies delta_y at 800",
          abs(wheel_800.get("offset", -1) - 240.0) < 0.5,
          f"offset={wheel_800.get('offset')} max={wheel_800.get('max')}")
    check(group, "wheel clamps to content_h - view_h",
          abs(wheel_max.get("offset", -1) - 1132.0) < 0.5,
          f"offset={wheel_max.get('offset')} max={wheel_max.get('max')}")
    check(group, "wheel outside a scroll container is ignored",
          "no scrollable container" in wheel_outside.get("text", ""),
          "runtime reported no scrollable container")
    check(group, "image batch count (18 x 6 vertices)",
          log_1280.get("image_vertices") == 108,
          f"{log_1280.get('image_vertices')} vertices")

    for width, base_name, wheel_name, clip_rect in (
        (1280, "render_1280.png", "wheel_1280.png", (40, 80, 1200, 468)),
        (800, "render_800.png", "wheel_800.png", (40, 80, 612, 468)),
    ):
        group = f"wheel@{width}"
        normal = Image.open(root / base_name)
        scrolled = Image.open(root / wheel_name)
        changed, bbox = diff(normal, scrolled)
        check(group, "visible content changed", changed > 10000, f"{changed} px")
        inside = (bbox is not None and bbox[0] >= clip_rect[0] - 1 and
                  bbox[1] >= clip_rect[1] - 1 and bbox[2] <= clip_rect[2] + 1 and
                  bbox[3] <= clip_rect[3] + 1)
        check(group, "changes stay inside the clip rect", inside,
              f"bbox={bbox} expected inside {clip_rect}")
        check(group, "footer untouched by the scroll",
              rgb(scrolled, 400, 510) == CARD, f"{rgb(scrolled, 400, 510)}")
        check(group, "gap below the scroll box untouched",
              rgb(scrolled, 400, 470) == SCROLL, f"{rgb(scrolled, 400, 470)}")

    group = "wheel@max"
    normal = Image.open(root / "render_800.png")
    clamped = Image.open(root / "wheel_max.png")
    changed, bbox = diff(normal, clamped)
    check(group, "max scroll changes content", changed > 10000, f"{changed} px")
    check(group, "max scroll stays inside the clip",
          bbox is not None and bbox[1] >= 79 and bbox[2] <= 613 and bbox[3] <= 469,
          f"bbox={bbox}")
    check(group, "footer untouched at max scroll",
          rgb(clamped, 400, 510) == CARD, f"{rgb(clamped, 400, 510)}")
    check(group, "gap untouched at max scroll",
          rgb(clamped, 400, 470) == SCROLL, f"{rgb(clamped, 400, 470)}")

    group = "wheel@outside"
    outside = Image.open(root / "wheel_outside.png")
    changed, bbox = diff(normal, outside)
    check(group, "render is unchanged", changed == 0, f"{changed} px, bbox={bbox}")

    failed = [item for item in CHECKS if not item["ok"]]
    result = {
        "ok": not failed,
        "total_checks": len(CHECKS),
        "failed_checks": len(failed),
        "checks": CHECKS,
        "scroll": {
            "1280": {"offset": wheel_1280.get("offset"), "max": wheel_1280.get("max")},
            "800": {"offset": wheel_800.get("offset"), "max": wheel_800.get("max")},
        },
        "render": {
            "1280": {"layout_ms": log_1280.get("layout_ms"),
                     "frame_ms": log_1280.get("frame_ms")},
            "800": {"layout_ms": log_800.get("layout_ms"),
                    "frame_ms": log_800.get("frame_ms")},
        },
    }
    (root / "pilot_verify_report.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"\npilot verify: {len(CHECKS) - len(failed)}/{len(CHECKS)} checks passed")
    if failed:
        for item in failed:
            print(f"  FAILED {item['group']}: {item['name']} - {item['detail']}")
        return 1
    print("pilot verify: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
