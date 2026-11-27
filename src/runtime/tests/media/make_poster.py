#!/usr/bin/env python3
"""Deterministic 300x150 poster for the media suite.

The size matches the documented video default (300x150), so `object-fit:
contain/cover` has an exact expected geometry even in phase A (no poster size
probe yet). The fill is a solid distinctive color with a small corner mark so a
solid-fill regression is still visible if one is ever added.
"""
from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
POSTER = HERE / "page" / "posters" / "poster.png"
COLOR = "#c0392b"
MARK = "#f5f5f5"


def main() -> int:
    POSTER.parent.mkdir(parents=True, exist_ok=True)
    image = Image.new("RGB", (300, 150), COLOR)
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 0, 9, 9), fill=MARK)
    image.save(POSTER)
    print(f"wrote {POSTER} (300x150)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
