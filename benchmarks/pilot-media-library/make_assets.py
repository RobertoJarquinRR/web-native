#!/usr/bin/env python3
"""Generate the placeholder posters for the media-library pilot.

Every poster is a deterministic 180x240 RGB PNG built only from geometric
shapes and a large number: no external or copyrighted images are downloaded
or copied.

Design contract (also used by verify.py):

  * solid `bg` frame around a patterned inset;
  * a large two-digit number in the middle;
  * an accent `rule` band at y=196..204;
  * an accent `footer` band at y=224..240;
  * sample points (4,4) -> bg and (4,232) -> footer.

The accent colours are unique across the 30 posters so the verifier can find a
single poster's bands by colour when checking grid positions and scroll.

Usage:
    python make_assets.py
    python make_assets.py --catalog catalog.json --out posters
"""

from __future__ import annotations

import argparse
import colorsys
import hashlib
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
POSTER_W, POSTER_H = 180, 240
PATTERN_BOX = (16, 16, 164, 196)
RULE_BAND = (196, 204)
FOOTER_BAND = (224, 240)
SAMPLE_POINTS = {"bg": [4, 4], "rule": [4, 200], "footer": [4, 232]}
EXPECTED_ACCENT_COUNT = 30

FONT_CANDIDATES = (
    r"C:\Windows\Fonts\segoeuib.ttf",
    r"C:\Windows\Fonts\arialbd.ttf",
    r"C:\Windows\Fonts\calibrib.ttf",
    r"C:\Windows\Fonts\seguisb.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
)


def hsv_rgb(hue: float, sat: float, val: float) -> tuple[int, int, int]:
    r, g, b = colorsys.hsv_to_rgb(hue / 360.0, sat, val)
    return int(round(r * 255)), int(round(g * 255)), int(round(b * 255))


def mix(a: tuple[int, int, int], b: tuple[int, int, int], t: float) -> tuple[int, int, int]:
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def hex_rgb(rgb: tuple[int, int, int]) -> str:
    return "#{:02x}{:02x}{:02x}".format(*rgb)


def palette(index: int, total: int) -> dict:
    """Golden-angle palette: stable across runs and independent of `total`."""
    hue = (index * 137.508 + 15.0) % 360.0
    value = 0.34 + (index % 3) * 0.025
    bg = hsv_rgb(hue, 0.55, value)
    pattern = mix(bg, (255, 255, 255), 0.18)
    accent = hsv_rgb((hue + 155.0) % 360.0, 0.68, 0.86)
    return {
        "hue": round(hue, 3),
        "bg": bg,
        "pattern": pattern,
        "accent": accent,
        "variant": index % 6,
    }


def load_font(size: int):
    from PIL import ImageFont

    for candidate in FONT_CANDIDATES:
        path = Path(candidate)
        if path.is_file():
            try:
                return ImageFont.truetype(str(path), size)
            except OSError:
                continue
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def draw_pattern(draw, width: int, height: int, color, variant: int) -> None:
    if variant == 0:  # vertical bars
        for x in range(0, width, 20):
            draw.rectangle((x, 0, x + 8, height), fill=color)
    elif variant == 1:  # horizontal bars
        for y in range(8, height, 22):
            draw.rectangle((0, y, width, y + 7), fill=color)
    elif variant == 2:  # dots
        for y in range(12, height, 24):
            for x in range(12, width, 24):
                draw.ellipse((x - 3, y - 3, x + 3, y + 3), fill=color)
    elif variant == 3:  # diagonals
        for x in range(-height, width, 22):
            draw.line((x, height, x + height, 0), fill=color, width=5)
    elif variant == 4:  # checkerboard
        cell = 24
        for y in range(0, height, cell):
            for x in range(0, width, cell):
                if ((x // cell) + (y // cell)) % 2 == 0:
                    draw.rectangle((x, y, x + cell - 1, y + cell - 1), fill=color)
    else:  # concentric frames
        step = 22
        for inset in range(0, min(width, height) // 2, step):
            draw.rectangle(
                (inset, inset, width - 1 - inset, height - 1 - inset),
                outline=color,
                width=4,
            )


def make_poster(index: int, total: int):
    from PIL import Image, ImageDraw

    colors = palette(index, total)
    image = Image.new("RGB", (POSTER_W, POSTER_H), colors["bg"])
    draw = ImageDraw.Draw(image)

    # Patterned inset. It never covers the outer frame where verify.py samples.
    left, top, right, bottom = PATTERN_BOX
    pattern = Image.new("RGB", (right - left, bottom - top), colors["bg"])
    draw_pattern(
        ImageDraw.Draw(pattern),
        right - left,
        bottom - top,
        colors["pattern"],
        colors["variant"],
    )
    image.paste(pattern, (left, top))

    # Large number as the per-poster identifier.
    font = load_font(84)
    draw.text(
        ((left + right) // 2, (top + bottom) // 2),
        f"{index + 1:02d}",
        font=font,
        fill=(244, 247, 252),
        anchor="mm",
    )

    # Accent bands: a thin rule and a solid footer. Both are only place where
    # the accent colour appears, so a whole-image colour scan is meaningful.
    draw.rectangle((0, RULE_BAND[0], POSTER_W - 1, RULE_BAND[1] - 1), fill=colors["accent"])
    draw.rectangle((0, FOOTER_BAND[0], POSTER_W - 1, FOOTER_BAND[1] - 1), fill=colors["accent"])

    return image, colors


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate pilot-media-library poster PNGs")
    parser.add_argument("--catalog", type=Path, default=HERE / "catalog.json")
    parser.add_argument("--out", type=Path, default=HERE, help="pilot root (poster paths are relative to it)")
    args = parser.parse_args()

    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        print(
            "ERROR: Pillow is required.\n"
            "  python -m venv .venv\n"
            "  .venv\\Scripts\\python -m pip install Pillow\n"
            "  .venv\\Scripts\\python make_assets.py",
            file=sys.stderr,
        )
        return 2

    catalog_path = args.catalog.resolve()
    root = args.out.resolve()
    try:
        catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: cannot read {catalog_path}: {exc}", file=sys.stderr)
        return 2

    items = catalog.get("items")
    if not isinstance(items, list) or not (24 <= len(items) <= 36):
        print(f"ERROR: catalog must have 24..36 items, got {len(items) if isinstance(items, list) else 'n/a'}", file=sys.stderr)
        return 2

    manifest = []
    accents = set()
    for index, item in enumerate(items):
        try:
            poster_rel = item["poster"]
            poster_id = item["id"]
        except KeyError as exc:
            print(f"ERROR: item {index} is missing {exc}", file=sys.stderr)
            return 2
        poster_path = (root / poster_rel).resolve()
        try:
            poster_path.relative_to((root / "posters").resolve())
        except ValueError:
            print(f"ERROR: poster path must live under posters/: {poster_rel}", file=sys.stderr)
            return 2
        poster_path.parent.mkdir(parents=True, exist_ok=True)

        image, colors = make_poster(index, len(items))
        if colors["accent"] in accents:
            print(f"ERROR: duplicate accent colour at index {index}", file=sys.stderr)
            return 2
        accents.add(colors["accent"])
        image.save(poster_path, format="PNG", optimize=True)

        manifest.append(
            {
                "index": index,
                "id": poster_id,
                "file": poster_rel,
                "width": POSTER_W,
                "height": POSTER_H,
                "sha256": sha256(poster_path),
                "bg": hex_rgb(colors["bg"]),
                "pattern": hex_rgb(colors["pattern"]),
                "accent": hex_rgb(colors["accent"]),
                "pattern_variant": colors["variant"],
                "number": f"{index + 1:02d}",
            }
        )

    if len(accents) != len(items):
        print("ERROR: accent colours are not unique", file=sys.stderr)
        return 2

    # Remove posters that belonged to a previous, longer catalog.
    keep = {(root / item["poster"]).resolve() for item in items}
    posters_dir = (root / "posters").resolve()
    for stale in posters_dir.glob("vid-*.png"):
        if stale.resolve() not in keep:
            stale.unlink()

    manifest_doc = {
        "schema": "webnative.pilot-media-library.posters/1",
        "generated_by": "make_assets.py (Pillow, geometric placeholders only)",
        "size": [POSTER_W, POSTER_H],
        "sample_points": SAMPLE_POINTS,
        "bands": {"rule": list(RULE_BAND), "footer": list(FOOTER_BAND)},
        "count": len(manifest),
        "posters": manifest,
    }
    manifest_path = posters_dir / "manifest.json"
    manifest_bytes = (json.dumps(manifest_doc, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    if not manifest_path.is_file() or manifest_path.read_bytes() != manifest_bytes:
        manifest_path.write_bytes(manifest_bytes)

    print(f"generated {len(manifest)} posters at {posters_dir}")
    print(f"wrote {manifest_path}")
    print("sample points: bg=(4,4) footer=(4,232); bands rule=196..204 footer=224..240")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
