#!/usr/bin/env python3
"""Create the two solid thumbnails used by the dynamic UI fixture."""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

COLORS = {"a.png": (231, 76, 60), "b.png": (52, 152, 219)}
WIDTH, HEIGHT = 104, 60


def main() -> int:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent / "page" / "images"
    out.mkdir(parents=True, exist_ok=True)
    for name, color in COLORS.items():
        image = Image.new("RGB", (WIDTH, HEIGHT), color)
        pixels = image.load()
        for y in range(HEIGHT):
            for x in range(WIDTH):
                r, g, b = color
                if (x // 26) % 2 == 0:
                    pixels[x, y] = (r, g, b)
                else:
                    pixels[x, y] = (max(r - 30, 0), max(g - 30, 0), max(b - 30, 0))
        image.save(out / name)
    print(f"wrote {len(COLORS)} thumbnails to {out} ({WIDTH}x{HEIGHT})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
