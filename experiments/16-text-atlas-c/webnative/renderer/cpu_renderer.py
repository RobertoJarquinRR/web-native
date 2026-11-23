"""Renderer CPU de referencia: PIL/ImageDraw, sin dependencias nativas de GPU.

Los rects son exactos (misma geometría entera que wgpu). El texto es una
aproximación independiente: se vuelve a maquetar la cadena con `ImageFont`
(FreeType + layout interno de Pillow) en la baseline de la run, en vez de usar
los quads del atlas de HarfBuzz. Para paridad exacta de texto usar wgpu; aquí
se busca una referencia legible y barata, y se documentan las diferencias.
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from ..fonts import resolve_font_file
from ..ir import DisplayList, RectOp, TextOp
from . import rgb255


class CpuRenderer:
    name = "cpu"
    backend_info = "Pillow / ImageDraw (CPU, texto aproximado)"

    def __init__(self) -> None:
        self._fonts: dict[tuple[str, int], ImageFont.FreeTypeFont] = {}

    def _font(self, op: TextOp) -> ImageFont.FreeTypeFont:
        path = Path(op.font_file) if op.font_file else resolve_font_file(op.font)
        if not path.is_file():
            path = resolve_font_file(op.font)
        key = (str(path), int(round(op.size)))
        if key not in self._fonts:
            self._fonts[key] = ImageFont.truetype(str(path), key[1])
        return self._fonts[key]

    def render(self, display_list: DisplayList, width: int | None = None,
               height: int | None = None) -> Image.Image:
        w = display_list.width if width is None else int(width)
        h = display_list.height if height is None else int(height)
        image = Image.new("RGB", (w, h), (0, 0, 0))
        draw = ImageDraw.Draw(image)
        for op in display_list.ops:
            if isinstance(op, RectOp):
                x, y = round(op.x), round(op.y)
                draw.rectangle([x, y, x + round(op.w) - 1, y + round(op.h) - 1],
                               fill=rgb255(op.fill))
            elif isinstance(op, TextOp):
                if not op.text:
                    continue
                # anchor="ls": left + baseline, la misma referencia que TextOp.y.
                draw.text((op.x, op.y), op.text, font=self._font(op),
                          fill=rgb255(op.color), anchor="ls")
            else:
                raise NotImplementedError(f"cpu: op no soportada: {type(op).__name__}")
        return image
