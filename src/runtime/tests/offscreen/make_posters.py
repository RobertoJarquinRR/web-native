#!/usr/bin/env python3
"""Deterministic 512x512 posters for the image-cache budget test.

Every poster decodes to exactly 1 MiB of RGBA (512 * 512 * 4), so the C test
can reason about the WN_IMAGE_CACHE_MB budget in fixed-size units.
"""
from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
POSTERS = HERE / "cache_page" / "posters"
COUNT = 12
SIZE = 512
COLORS = (
    "#c0392b", "#27ae60", "#2980b9", "#f39c12",
    "#8e44ad", "#16a085", "#d35400", "#2c3e50",
    "#c2185b", "#00838f", "#7cb342", "#5d4037",
)


def main() -> int:
    POSTERS.mkdir(parents=True, exist_ok=True)
    for index in range(COUNT):
        image = Image.new("RGB", (SIZE, SIZE), COLORS[index % len(COLORS)])
        draw = ImageDraw.Draw(image)
        # A light band keeps the frame non-blank even if a solid fill is sampled.
        draw.rectangle((64, 64, SIZE - 64, SIZE - 64),
                       fill="#f5f5f5", outline="#202020", width=8)
        draw.text((96, 96), f"poster {index:02d}", fill="#202020")
        out = POSTERS / f"p{index}.png"
        image.save(out)
    print(f"wrote {COUNT} posters ({SIZE}x{SIZE}, rgba=1.0 MiB each) -> {POSTERS}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
