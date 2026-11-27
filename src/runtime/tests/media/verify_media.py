#!/usr/bin/env python3
"""Layout + pixel gate for the media fixture (400x640).

Checks the five declared boxes, the default 300x150 size, object-fit contain
(the poster is letterboxed inside a square box) and cover (it fills the square),
the neutral placeholder for a poster-less video, and the page background around
each box. The fixture is a real HTML/CSS page compiled by frontend-rust.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image

CHECKS: list[dict] = []

PAGE_BG = (16, 16, 16)          # #101010
POSTER = (192, 57, 43)          # #c0392b (poster.png)
PLACEHOLDER = (35, 42, 54)      # #232a36 (WN_VIDEO_PLACEHOLDER_COLOR)


def check(group: str, name: str, ok: bool, detail: str) -> bool:
    CHECKS.append({"group": group, "name": name, "ok": bool(ok), "detail": detail})
    print(f"[{'ok  ' if ok else 'FAIL'}] {group}: {name} - {detail}")
    return bool(ok)


def px(img: Image.Image, x: float, y: float) -> tuple[int, int, int]:
    return img.getpixel((int(round(x)), int(round(y))))[:3]


def close(actual: tuple[int, int, int], expected: tuple[int, int, int],
          tolerance: int = 3) -> bool:
    return all(abs(a - b) <= tolerance for a, b in zip(actual, expected))


def node_named(nodes: list[dict], suffix: str) -> dict | None:
    for node in nodes:
        if str(node.get("name", "")).endswith(suffix):
            return node
    return None


def rect_of(nodes: list[dict], suffix: str) -> tuple[int, int, int, int]:
    node = node_named(nodes, suffix)
    if node is None:
        raise KeyError(suffix)
    return (int(round(node["x"])), int(round(node["y"])),
            int(round(node["w"])), int(round(node["h"])))


EXPECTED_RECTS = {
    "/video#poster": (0, 0, 300, 150),
    "/video#placeholder": (0, 150, 160, 90),
    "/video#contain": (0, 240, 100, 100),
    "/video#cover": (0, 340, 100, 100),
    "/video#auto": (0, 440, 300, 150),
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--png", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--backend", default="d3d11")
    args = parser.parse_args()

    image = Image.open(args.png).convert("RGB")
    report = json.loads(Path(args.report).read_text(encoding="utf-8"))
    nodes = report["sizes"][0]["nodes"]

    check("capture", "size", image.size == (400, 640), str(image.size))

    for suffix, expected in EXPECTED_RECTS.items():
        actual = rect_of(nodes, suffix)
        check("layout", suffix, actual == expected, f"{actual} != {expected}")

    for suffix in EXPECTED_RECTS:
        node = node_named(nodes, suffix)
        kind = node.get("kind") if node else None
        check("dump-kind", suffix, kind == "video", str(kind))

    poster_center = px(image, 150, 75)
    check("poster", "fill center is poster red",
          close(poster_center, POSTER), str(poster_center))
    check("poster", "outside the box is page background",
          close(px(image, 310, 75), PAGE_BG), str(px(image, 310, 75)))

    placeholder_center = px(image, 80, 195)
    check("placeholder", "center is the neutral placeholder",
          close(placeholder_center, PLACEHOLDER), str(placeholder_center))
    check("placeholder", "outside the 160px box is page background",
          close(px(image, 170, 195), PAGE_BG), str(px(image, 170, 195)))

    # contain: 300x150 poster into 100x100 -> 100x50 centered at y=265.
    check("contain", "letterboxed image at the vertical center",
          close(px(image, 50, 290), POSTER), str(px(image, 50, 290)))
    check("contain", "top band keeps the page background",
          close(px(image, 50, 250), PAGE_BG), str(px(image, 50, 250)))
    check("contain", "bottom band keeps the page background",
          close(px(image, 50, 330), PAGE_BG), str(px(image, 50, 330)))

    # cover: the same source fills the whole 100x100 box.
    check("cover", "center is poster",
          close(px(image, 50, 390), POSTER), str(px(image, 50, 390)))
    check("cover", "near the top edge is still poster",
          close(px(image, 50, 345), POSTER), str(px(image, 50, 345)))

    auto_center = px(image, 150, 515)
    check("default-size", "300x150 box paints the placeholder",
          close(auto_center, PLACEHOLDER), str(auto_center))
    check("default-size", "below the box is page background",
          close(px(image, 150, 600), PAGE_BG), str(px(image, 150, 600)))

    passed = sum(1 for item in CHECKS if item["ok"])
    total = len(CHECKS)
    print(f"media verify ({args.backend}): {passed}/{total} passed")
    return 0 if passed == total else 1


if __name__ == "__main__":
    raise SystemExit(main())
