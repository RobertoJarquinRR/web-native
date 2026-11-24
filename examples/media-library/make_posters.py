"""Generate placeholder posters for the media-library example.

Usage:
    python make_posters.py

Requires Pillow. Writes 12 PNGs (160x214) under app/posters/.
"""
from __future__ import annotations

import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
OUT = HERE / "app" / "posters"
OUT.mkdir(parents=True, exist_ok=True)

PALETTES = [
    ((231, 76, 60), (44, 62, 80)),
    ((52, 152, 219), (12, 24, 48)),
    ((46, 204, 113), (10, 40, 30)),
    ((155, 89, 182), (30, 10, 50)),
    ((241, 196, 15), (50, 30, 0)),
    ((26, 188, 156), (0, 40, 40)),
    ((230, 126, 34), (40, 20, 0)),
    ((149, 165, 166), (20, 25, 30)),
    ((52, 73, 94), (5, 10, 20)),
    ((192, 57, 43), (30, 0, 0)),
    ((211, 84, 0), (35, 15, 0)),
    ((127, 140, 141), (15, 20, 25)),
]

W, H = 160, 214


def font(size: int) -> ImageFont.ImageFont:
    for candidate in ("C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf"):
        try:
            return ImageFont.truetype(candidate, size)
        except OSError:
            continue
    return ImageFont.load_default()


def make(index: int) -> None:
    top, bottom = PALETTES[index % len(PALETTES)]
    image = Image.new("RGB", (W, H), bottom)
    draw = ImageDraw.Draw(image)
    for y in range(H):
        t = y / (H - 1)
        color = tuple(round(top[i] * (1 - t) + bottom[i] * t) for i in range(3))
        draw.line([(0, y), (W, y)], fill=color)

    # subtle diagonal band
    for x in range(-H, W, 18):
        draw.line([(x, H), (x + H, 0)], fill=tuple(min(255, c + 18) for c in top), width=2)

    label = f"{index + 1:02d}"
    f = font(56)
    bbox = draw.textbbox((0, 0), label, font=f)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    draw.text(((W - tw) / 2, (H - th) / 2 - 12), label, font=f, fill=(255, 255, 255))

    f2 = font(14)
    sub = "DEMO"
    bbox2 = draw.textbbox((0, 0), sub, font=f2)
    tw2 = bbox2[2] - bbox2[0]
    draw.text(((W - tw2) / 2, H - 34), sub, font=f2, fill=(255, 255, 255))

    image.save(OUT / f"poster-{index + 1:02d}.png")


if __name__ == "__main__":
    for i in range(12):
        make(i)
    print(f"wrote 12 posters to {OUT}")
