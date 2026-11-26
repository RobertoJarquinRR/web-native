#!/usr/bin/env python3
"""Create the intrinsic-ratio image used by the fidelity ABI v6 test.

The 20x10 PNG is what the emitter reads to bake image_width/image_height. The
ABI v6 test then replaces that intrinsic size at runtime to prove the layout
follows a new aspect ratio.
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

WIDTH, HEIGHT = 20, 10


def main() -> int:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent / "out" / "images"
    out.mkdir(parents=True, exist_ok=True)
    image = Image.new("RGB", (WIDTH, HEIGHT), (40, 120, 200))
    pixels = image.load()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if x < WIDTH // 2:
                pixels[x, y] = (40, 120, 200)
            else:
                pixels[x, y] = (200, 80, 40)
    image.save(out / "ratio.png")
    print(f"wrote ratio.png to {out} ({WIDTH}x{HEIGHT})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
