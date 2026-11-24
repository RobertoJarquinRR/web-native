#!/usr/bin/env python3
"""Generate the pilot poster PNGs (120x90, three vertical bands).

Rows of the grid use distinct palettes so scrolling is visible:
  posters 1-6   warm   (red/orange/yellow)
  posters 7-12  cool   (green/teal/blue)
  posters 13-18 purple (purple/magenta/pink)
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

PALETTES = (
    ((231, 76, 60), (243, 156, 18), (241, 196, 15)),
    ((46, 204, 113), (26, 188, 156), (52, 152, 219)),
    ((155, 89, 182), (142, 68, 173), (255, 105, 180)),
)
WIDTH, HEIGHT = 120, 90


def main() -> int:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent / "posters"
    out.mkdir(parents=True, exist_ok=True)
    for index in range(18):
        palette = PALETTES[min(index // 6, len(PALETTES) - 1)]
        image = Image.new("RGB", (WIDTH, HEIGHT))
        pixels = image.load()
        for y in range(HEIGHT):
            for x in range(WIDTH):
                pixels[x, y] = palette[min(x * 3 // WIDTH, 2)]
        image.save(out / f"poster-{index + 1:02d}.png")
    print(f"wrote 18 posters to {out} ({WIDTH}x{HEIGHT})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
