#!/usr/bin/env python3
"""Non-blank check for the offscreen image-cache frame.

The budget math lives in image_cache_test.c; this script only proves the frame
that produced those numbers actually drew the posters.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageChops


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--png", required=True)
    parser.add_argument("--min-content-fraction", type=float, default=0.05)
    args = parser.parse_args()

    path = Path(args.png)
    if not path.is_file():
        print(f"image cache frame missing: {path}")
        return 1
    with Image.open(path) as raw:
        frame = raw.convert("RGBA")
    total = frame.size[0] * frame.size[1]
    clear = frame.getpixel((0, 0))
    blank = Image.new("RGBA", frame.size, clear)
    histogram = ImageChops.difference(frame, blank).convert("L").histogram()
    content = total - sum(histogram[:3])
    fraction = content / total
    colors = frame.convert("RGB").getcolors(maxcolors=1 << 20)
    unique = len(colors) if colors is not None else 1 << 20
    print(f"image cache frame: {frame.size[0]}x{frame.size[1]} content={fraction:.2%} "
          f"colors={unique}")
    if fraction < args.min_content_fraction:
        print(f"image cache frame looks blank (content {fraction:.2%} < "
              f"{args.min_content_fraction:.2%})")
        return 1
    if unique < 4:
        print(f"image cache frame has too few colors ({unique}); posters did not draw")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
