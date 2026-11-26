#!/usr/bin/env python3
"""Deterministic 2x2 quadrant poster for the group-opacity pixel fixture.

The same texture as the CSS suite: four solid texels map to the four quadrants
of the 60x60 image node, so the verifier can assert the 0.5 composite over the
page for each quadrant.
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
    print(f"wrote {out} (2x2 quadrants)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
