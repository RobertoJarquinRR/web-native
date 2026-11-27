#!/usr/bin/env python3
"""Pixel gate for the E4 video frame path.

Checks four offscreen renders produced by media_frame_test:
  - frame_poster.png: poster + neutral placeholder before any frame;
  - frame_first.png: decoded frame 0 in a fill box (color bars, bit 0, marker);
  - frame_seek.png: the frame at/after 5 s (index 151) with object-fit contain
    and cover geometry;
  - frame_paused.png: identical to the seek frame after pause.

The expected pattern comes from media_test.c: eight vertical bars, ten 10x10
bit blocks at the top and a 24x20 orange marker at the bottom.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image

W, H = 320, 180
FRAMES = 300
DURATION_US = 10_000_000

BARS = [
    (255, 255, 255),
    (255, 255, 0),
    (0, 255, 255),
    (0, 255, 0),
    (255, 0, 255),
    (255, 0, 0),
    (0, 0, 255),
    (0, 0, 0),
]
MARKER = (255, 128, 0)
POSTER = (192, 57, 43)
PLACEHOLDER = (35, 42, 54)
PAGE_BG = (16, 16, 16)

CHECKS: list[dict] = []


def check(group: str, name: str, ok: bool, detail: str) -> bool:
    CHECKS.append({"group": group, "name": name, "ok": bool(ok), "detail": detail})
    print(f"[{'ok  ' if ok else 'FAIL'}] {group}: {name} - {detail}")
    return bool(ok)


def px(img: Image.Image, x: float, y: float) -> tuple[int, int, int]:
    return img.getpixel((int(round(x)), int(round(y))))[:3]


def close(actual: tuple[int, int, int], expected: tuple[int, int, int],
          tolerance: int = 3) -> bool:
    return all(abs(a - b) <= tolerance for a, b in zip(actual, expected))


def frame_pixel(index: int, x: int, y: int) -> tuple[int, int, int]:
    """Mirror of wn_media_test_pixel for a point inside the 320x180 frame."""
    if y < 10:
        bit = x // 12
        if bit < 10 and x - bit * 12 < 10:
            return (255, 255, 255) if (index >> bit) & 1 else (0, 0, 0)
    marker_x = index * (W - 24) // (FRAMES - 1)
    if marker_x <= x < marker_x + 24 and H - 20 <= y < H:
        return MARKER
    return BARS[(x * 8) // W]


def fill_screen_to_frame(x: float, y: float, box: tuple[int, int, int, int]
                          ) -> tuple[float, float] | None:
    bx, by, bw, bh = box
    if bw <= 0 or bh <= 0:
        return None
    return ((x - bx) * W / bw, (y - by) * H / bh)


def expect_fill(img: Image.Image, group: str, index: int,
                box: tuple[int, int, int, int]) -> None:
    """Pixel probes for a `fill` box: the whole 320x180 frame is stretched."""
    bx, by, bw, bh = box
    marker_x = index * (W - 24) // (FRAMES - 1)
    probes = [
        ("bar 0 white", 20, 80),
        ("bar 1 yellow", 60, 80),
        ("bar 5 red", 220, 80),
        ("bit 0 of the index", 5, 5),
        ("marker", marker_x + 8, 170),
    ]
    for label, fx, fy in probes:
        sx = bx + (fx + 0.5) * bw / W
        sy = by + (fy + 0.5) * bh / H
        expected = frame_pixel(index, fx, fy)
        actual = px(img, sx, sy)
        check(group, label, close(actual, expected),
              f"{actual} != {expected} at ({sx:.1f},{sy:.1f})")
    # Index bits: every decoded frame must carry the right number.
    for bit in range(10):
        fx = bit * 12 + 5
        sy = by + 5.5 * bh / H
        sx = bx + (fx + 0.5) * bw / W
        expected = frame_pixel(index, fx, 5)
        actual = px(img, sx, sy)
        check(group, f"index bit {bit}", close(actual, expected),
              f"{actual} != {expected}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", required=True)
    parser.add_argument("--backend", default="d3d11")
    args = parser.parse_args()

    root = Path(args.dir)
    poster = Image.open(root / "frame_poster.png").convert("RGB")
    first = Image.open(root / "frame_first.png").convert("RGB")
    seek = Image.open(root / "frame_seek.png").convert("RGB")
    paused = Image.open(root / "frame_paused.png").convert("RGB")

    check("capture", "all 400x640", all(
        image.size == (400, 640) for image in (poster, first, seek, paused)),
        str(first.size))

    # Before any frame: the poster node keeps its poster and the poster-less
    # node keeps the neutral placeholder.
    check("poster", "poster red", close(px(poster, 150, 75), POSTER),
          str(px(poster, 150, 75)))
    check("poster", "placeholder is neutral",
          close(px(poster, 80, 195), PLACEHOLDER), str(px(poster, 80, 195)))

    # Frame 0 in the fill box (0,150,160,90).
    expect_fill(first, "first-frame", 0, (0, 150, 160, 90))
    check("first-frame", "a decoded frame replaces the placeholder",
          not close(px(first, 80, 195), PLACEHOLDER), str(px(first, 80, 195)))
    check("first-frame", "the untouched poster node still paints its poster",
          close(px(first, 150, 75), POSTER), str(px(first, 150, 75)))

    # Seek to 5 s: the frame exactly at the target is frame 150 (5,000,000 us).
    seek_index = 150
    expect_fill(seek, "seek-frame", seek_index, (0, 150, 160, 90))
    check("seek-frame", "position marker moved",
          close(px(seek, 80, 235), MARKER), str(px(seek, 80, 235)))

    # contain into 100x100: 100x56.25 centered at y=261.875.
    contain_scale = 100 / W
    contain_h = H * contain_scale
    contain_y = 240 + (100 - contain_h) / 2
    fx = 160
    fy = 90
    sx = 0 + fx * contain_scale
    sy = contain_y + fy * contain_scale
    check("contain", "letterboxed frame in the middle",
          close(px(seek, sx, sy), frame_pixel(seek_index, fx, fy)),
          f"{px(seek, sx, sy)} != {frame_pixel(seek_index, fx, fy)}")
    check("contain", "top band is page background",
          close(px(seek, 50, 250), PAGE_BG), str(px(seek, 50, 250)))
    check("contain", "bottom band is page background",
          close(px(seek, 50, 328), PAGE_BG), str(px(seek, 50, 328)))

    # cover into 100x100: scale to height, crop the sides (source x 70..250).
    cover_scale = 100 / H
    vis_w = 100 / cover_scale
    u0 = (W - vis_w) / 2
    cover_sx = 20
    cover_sy = 390
    src_x = u0 + (cover_sx - 0) / cover_scale
    src_y = (cover_sy - 340) / cover_scale
    check("cover", "left edge shows the cropped source",
          close(px(seek, cover_sx, cover_sy),
                frame_pixel(seek_index, int(src_x), int(src_y))),
          f"{px(seek, cover_sx, cover_sy)} != "
          f"{frame_pixel(seek_index, int(src_x), int(src_y))}")
    check("cover", "the whole box is frame (no letterbox)",
          close(px(seek, 50, 345), frame_pixel(seek_index, 160, 9)),
          str(px(seek, 50, 345)))

    # Pause keeps exactly the seek frame.
    same = True
    for y in range(340, 440, 7):
        for x in range(0, 100, 7):
            if px(seek, x, y) != px(paused, x, y):
                same = False
    check("pause", "the paused PNG equals the seek frame", same,
          "cover box sampled")

    passed = sum(1 for item in CHECKS if item["ok"])
    total = len(CHECKS)
    print(f"media frames ({args.backend}): {passed}/{total} passed")
    return 0 if passed == total else 1


if __name__ == "__main__":
    raise SystemExit(main())
