"""Fallback CPU con Pillow: rects + glifos del atlas (alpha mask).

No es el camino objetivo, pero garantiza que el pipeline produzca PNG aunque
wgpu no tenga adaptador. Respeta el mismo display list.
"""
from PIL import Image, ImageDraw


def _rgb(color: str) -> tuple[int, int, int]:
    return tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))


def render_pillow(ops: list[dict], atlas, width: int, height: int,
                  clear_rgb: tuple[int, int, int]) -> Image.Image:
    image = Image.new("RGB", (width, height), clear_rgb)
    draw = ImageDraw.Draw(image)
    for op in ops:
        if op["type"] == "rect":
            x0, y0 = round(op["x"]), round(op["y"])
            x1, y1 = round(op["x"] + op["w"]) - 1, round(op["y"] + op["h"]) - 1
            if x1 >= x0 and y1 >= y0:
                draw.rectangle([x0, y0, x1, y1], fill=_rgb(op["fill"]))
    for op in ops:
        if op["type"] != "text_run":
            continue
        color = _rgb(op["color"])
        for glyph in op["glyphs"]:
            w, h = glyph["w"], glyph["h"]
            if w == 0 or h == 0:
                continue
            key = (op["face"], int(round(op["size"])), glyph["gid"])
            rows = atlas.rows(key)
            if len(rows) != h or any(len(row) != w for row in rows):
                continue
            mask = Image.frombytes("L", (w, h), b"".join(rows))
            patch = Image.new("RGB", (w, h), color)
            x, y = round(glyph["x"]), round(glyph["y"])
            image.paste(patch, (x, y), mask)
    return image
