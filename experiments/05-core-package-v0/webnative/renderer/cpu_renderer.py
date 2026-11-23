"""Renderer CPU de referencia: PIL/ImageDraw, sin dependencias nativas de GPU."""
from PIL import Image, ImageDraw

from ..ir import DisplayList, RectOp
from . import rgb255


class CpuRenderer:
    name = "cpu"
    backend_info = "Pillow / ImageDraw (CPU)"

    def render(self, display_list: DisplayList, width: int | None = None,
               height: int | None = None) -> Image.Image:
        w = display_list.width if width is None else int(width)
        h = display_list.height if height is None else int(height)
        image = Image.new("RGB", (w, h), (0, 0, 0))
        draw = ImageDraw.Draw(image)
        for op in display_list.ops:
            if not isinstance(op, RectOp):
                raise NotImplementedError(f"cpu: op no soportada: {type(op).__name__}")
            x, y = round(op.x), round(op.y)
            draw.rectangle([x, y, x + round(op.w) - 1, y + round(op.h) - 1],
                           fill=rgb255(op.fill))
        return image
