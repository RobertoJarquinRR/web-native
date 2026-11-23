"""Texto nativo: HarfBuzz (shaping) + FreeType (raster) + atlas R8.

Soporta dos pesos (400 y 700) eligiendo segoeui.ttf / segoeuib.ttf. No hay
cadena de fallback por glifo, italic, variable fonts ni emoji de color.
"""
import os
from dataclasses import dataclass, field
from pathlib import Path

import freetype
import uharfbuzz as hb

FONT_DIR = Path(os.environ.get("WINDIR", r"C:\Windows")) / "Fonts"

# En v0 la familia CSS se ignora a propósito: siempre se carga Segoe UI (con
# fallback Arial si el archivo no existe). No hay resolución de font-family ni
# fallback por glifo; ver GAPS.md §A/§B.


@dataclass
class Face:
    key: str
    path: Path
    ft: freetype.Face
    hb_face: hb.Face
    upem: int
    _hb_fonts: dict = field(default_factory=dict)
    _metrics: dict = field(default_factory=dict)

    def hb_font(self, size: float) -> hb.Font:
        key = round(size, 3)
        if key not in self._hb_fonts:
            font = hb.Font(self.hb_face)
            hb.ot_font_set_funcs(font)
            scale = max(1, int(round(size * 64)))
            font.scale = (scale, scale)
            self._hb_fonts[key] = font
        return self._hb_fonts[key]

    def pixel_size(self, size: float) -> int:
        return max(1, int(round(size)))

    def metrics(self, size: float) -> tuple[float, float, float]:
        """(ascender, descender positivo, line_gap) en px.

        Se usan las extents de HarfBuzz (hhea/OS2 a la escala exacta), no las
        métricas de FreeType a ppem entero: así el line-height normal coincide
        con el del navegador en vez de redondear hacia arriba.
        """
        key = round(size, 3)
        if key not in self._metrics:
            extents = self.hb_font(size).get_font_extents("ltr")
            self._metrics[key] = (extents.ascender / 64.0,
                                  -extents.descender / 64.0,
                                  extents.line_gap / 64.0)
        return self._metrics[key]

    def shape(self, text: str, size: float) -> list[dict]:
        buf = hb.Buffer()
        buf.add_str(text)
        buf.direction, buf.script, buf.language = "ltr", "Latn", "es"
        hb.shape(self.hb_font(size), buf)
        return [{"gid": info.codepoint, "cluster": info.cluster,
                 "adv": pos.x_advance / 64.0, "dx": pos.x_offset / 64.0,
                 "dy": pos.y_offset / 64.0}
                for info, pos in zip(buf.glyph_infos, buf.glyph_positions)]

    def raster(self, gid: int, size: float):
        """(w, h, bitmap_left, bitmap_top, filas) en grayscale 8bpp."""
        self.ft.set_pixel_sizes(0, self.pixel_size(size))
        flags = freetype.FT_LOAD_RENDER | getattr(freetype, "FT_LOAD_TARGET_NORMAL", 0)
        self.ft.load_glyph(gid, flags)
        glyph, bmp = self.ft.glyph, self.ft.glyph.bitmap
        w, rows, pitch = bmp.width, bmp.rows, bmp.pitch
        if w == 0 or rows == 0:
            return 0, 0, glyph.bitmap_left, glyph.bitmap_top, []
        raw = bytes(bmp.buffer)
        return w, rows, glyph.bitmap_left, glyph.bitmap_top, [
            raw[r * pitch:r * pitch + w] for r in range(rows)]


class FontBook:
    def __init__(self):
        self.faces: dict[str, Face] = {}
        self.warnings: list[str] = []
        self._shape_cache: dict[tuple, list[dict]] = {}
        regular = self._load_variant("regular", search=("segoeui.ttf", "arial.ttf",
                                                       "tahoma.ttf", "verdana.ttf"))
        self.faces["regular"] = regular
        bold_path = None
        for name in ("segoeuib.ttf", "arialbd.ttf", "tahomabd.ttf", "verdanab.ttf"):
            candidate = FONT_DIR / name
            if candidate.is_file():
                bold_path = candidate
                break
        if bold_path is None:
            self.warnings.append("no bold font found; font-weight 700 renders regular")
            self.faces["bold"] = regular
        else:
            self.faces["bold"] = self._load(bold_path, "bold")

    def _load(self, path: Path, key: str) -> Face:
        hb_face = hb.Face(path.read_bytes())
        return Face(key, path, freetype.Face(str(path)), hb_face, hb_face.upem)

    def _load_variant(self, key: str, search: tuple[str, ...]) -> Face:
        for name in search:
            path = FONT_DIR / name
            if path.is_file():
                return self._load(path, key)
        raise RuntimeError(f"no usable system font in {FONT_DIR} (tried {search})")

    def face_key(self, style) -> str:
        return "bold" if style.font_weight >= 600 else "regular"

    def metrics(self, face_key: str, size: float) -> tuple[float, float, float]:
        return self.faces[face_key].metrics(size)

    def shape(self, text: str, face_key: str, size: float) -> list[dict]:
        cache_key = (face_key, round(size, 3), text)
        if cache_key not in self._shape_cache:
            self._shape_cache[cache_key] = self.faces[face_key].shape(text, size)
        return self._shape_cache[cache_key]

    def advance(self, text: str, face_key: str, size: float) -> float:
        return sum(g["adv"] for g in self.shape(text, face_key, size))

    def raster(self, face_key: str, gid: int, size: float):
        return self.faces[face_key].raster(gid, size)

    def describe(self) -> dict:
        return {
            "regular": str(self.faces["regular"].path),
            "bold": str(self.faces["bold"].path),
            "regular_upem": self.faces["regular"].upem,
            "bold_upem": self.faces["bold"].upem,
            "warnings": list(self.warnings),
        }


class Atlas:
    """Shelf packing simple. Si se llena, omite glifos (nunca revienta)."""

    def __init__(self, size: int = 1024):
        self.size = size
        self.data = bytearray(size * size)
        self.rects: dict[tuple, tuple[int, int, int, int] | None] = {}
        self._shelf_x = self._shelf_y = self._shelf_h = 0
        self.overflow = 0

    def add(self, key: tuple, w: int, h: int, rows: list[bytes]) -> None:
        if w == 0 or h == 0:
            self.rects[key] = None
            return
        pad_w, pad_h = w + 2, h + 2
        if self._shelf_x + pad_w > self.size:
            self._shelf_x, self._shelf_y = 0, self._shelf_y + self._shelf_h
            self._shelf_h = 0
        if self._shelf_y + pad_h > self.size:
            self.rects[key] = None
            self.overflow += 1
            return
        x, y = self._shelf_x + 1, self._shelf_y + 1
        for r, line in enumerate(rows):
            off = (y + r) * self.size + x
            self.data[off:off + w] = line
        self.rects[key] = (x, y, w, h)
        self._shelf_x += pad_w
        self._shelf_h = max(self._shelf_h, pad_h)

    def uv(self, key: tuple) -> tuple[float, float, float, float]:
        rect = self.rects.get(key)
        if not rect:
            return (0.0, 0.0, 0.0, 0.0)
        x, y, w, h = rect
        s = float(self.size)
        return (x / s, y / s, (x + w) / s, (y + h) / s)

    def rows(self, key: tuple) -> list[bytes]:
        rect = self.rects.get(key)
        if not rect:
            return []
        x, y, w, h = rect
        return [bytes(self.data[(y + r) * self.size + x:
                                (y + r) * self.size + x + w])
                for r in range(h)]

    def unique(self) -> int:
        return sum(1 for rect in self.rects.values() if rect)

    def to_json(self) -> dict:
        return {"width": self.size, "height": self.size, "format": "r8unorm",
                "bytes": len(self.data), "unique_bitmaps": self.unique(),
                "overflow": self.overflow}
