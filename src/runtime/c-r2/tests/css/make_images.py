#!/usr/bin/env python3
"""Deterministic 2x2 quadrant poster for the CSS core v1 pixel test.

The four texels map to the four screen quadrants of the image card, so the
verifier can prove the rounded textured tessellation preserves object-fit UVs
(orientation, no stretch, no flip).
"""
from __future__ import annotations

from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
PAGE = HERE / "page"
QUADRANTS = (
    (204, 68, 51),    # top-left     #cc4433
    (51, 170, 102),   # top-right    #33aa66
    (51, 85, 204),    # bottom-left  #3355cc
    (221, 170, 34),   # bottom-right #ddaa22
)


def main() -> int:
    PAGE.mkdir(parents=True, exist_ok=True)
    image = Image.new("RGB", (2, 2))
    image.putpixel((0, 0), QUADRANTS[0])
    image.putpixel((1, 0), QUADRANTS[1])
    image.putpixel((0, 1), QUADRANTS[2])
    image.putpixel((1, 1), QUADRANTS[3])
    out = PAGE / "poster.png"
    image.save(out)
    print(f"wrote {out} (2x2, q1={QUADRANTS[0]} q2={QUADRANTS[1]} "
          f"q3={QUADRANTS[2]} q4={QUADRANTS[3]})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
