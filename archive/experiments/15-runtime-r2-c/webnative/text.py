"""Texto nativo: HarfBuzz shaping + FreeType raster + atlas canónico de glyphs.

Capa opcional: solo se importa cuando el documento contiene texto. Por cada
archivo TTF se cachean una `freetype.Face` y una `uharfbuzz.Font` por tamaño.
`build_text_ops()` convierte los `TextRun` del layout en `TextOp` del IR:

- posiciones absolutas de cada glyph (x,y = esquina superior izquierda),
- avance HarfBuzz (26.6 -> px) y offsets de la fuente,
- UVs de un atlas empaquetado en orden de primera aparición.

El atlas es determinista: `GlyphAtlas` recibe las claves en el mismo orden en
que las recorren los backends al reconstruirlo desde el IR. Aun así, el renderer
wgpu recalcula sus UVs con su propio atlas, para no depender de que el empaquetado
del build y el del render coincidan.
"""
from dataclasses import dataclass
from pathlib import Path

import freetype
import uharfbuzz as hb

from .fonts import first_family, resolve_font_file
from .ir import Glyph, TextOp

ATLAS_SIZE = 512
DEFAULT_SCRIPT = "Latn"
DEFAULT_DIRECTION = "ltr"


@dataclass(frozen=True)
class ShapedGlyph:
    gid: int
    cluster: int
    adv: float
    dx: float
    dy: float


@dataclass(frozen=True)
class GlyphBitmap:
    w: int
    h: int
    left: int
    top: int
    lines: tuple[bytes, ...]


class FontFace:
    """Una fuente del sistema con sus cachés de métricas, shaping y bitmaps."""

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self._ft = freetype.Face(str(self.path))
        self._hb_face = hb.Face(self.path.read_bytes())
        self._hb_fonts: dict[float, hb.Font] = {}
        self._metrics: dict[float, tuple[float, float]] = {}
        self._bitmaps: dict[tuple[int, int], GlyphBitmap] = {}

    @property
    def upem(self) -> int:
        return int(self._hb_face.upem)

    def hb_font(self, size: float) -> hb.Font:
        key = round(float(size), 3)
        if key not in self._hb_fonts:
            font = hb.Font(self._hb_face)
            hb.ot_font_set_funcs(font)
            scale = max(1, int(round(key * 64)))
            font.scale = (scale, scale)
            self._hb_fonts[key] = font
        return self._hb_fonts[key]

    def metrics(self, size: float) -> tuple[float, float]:
        """(ascender, descender) en px con el hinting de `size` redondeado."""
        key = round(float(size), 3)
        if key not in self._metrics:
            pixel_size = max(1, int(round(key)))
            self._ft.set_pixel_sizes(0, pixel_size)
            self._metrics[key] = (self._ft.size.ascender / 64.0,
                                  -self._ft.size.descender / 64.0)
        return self._metrics[key]

    def shape(self, text: str, size: float) -> tuple[ShapedGlyph, ...]:
        buf = hb.Buffer()
        buf.add_str(text)
        buf.direction = DEFAULT_DIRECTION
        buf.script = DEFAULT_SCRIPT
        buf.language = "en"
        hb.shape(self.hb_font(size), buf)
        return tuple(
            ShapedGlyph(info.codepoint, info.cluster,
                        pos.x_advance / 64.0, pos.x_offset / 64.0,
                        pos.y_offset / 64.0)
            for info, pos in zip(buf.glyph_infos, buf.glyph_positions))

    def raster(self, gid: int, size: float) -> GlyphBitmap:
        """Bitmap grayscale de FreeType tal cual lo entrega el hinting."""
        pixel_size = max(1, int(round(float(size))))
        key = (gid, pixel_size)
        if key in self._bitmaps:
            return self._bitmaps[key]
        self._ft.set_pixel_sizes(0, pixel_size)
        flags = freetype.FT_LOAD_RENDER | getattr(freetype, "FT_LOAD_TARGET_NORMAL", 0)
        self._ft.load_glyph(gid, flags)
        glyph, bitmap = self._ft.glyph, self._ft.glyph.bitmap
        w, rows, pitch = bitmap.width, bitmap.rows, bitmap.pitch
        if w == 0 or rows == 0:
            result = GlyphBitmap(0, 0, glyph.bitmap_left, glyph.bitmap_top, ())
        else:
            raw = bytes(bitmap.buffer)
            lines = tuple(raw[row * pitch:row * pitch + w] for row in range(rows))
            result = GlyphBitmap(w, rows, glyph.bitmap_left, glyph.bitmap_top, lines)
        self._bitmaps[key] = result
        return result


class TextEngine:
    """Fachada de fuentes para layout (métricas), IR (shaping) y render (raster)."""

    def __init__(self) -> None:
        self._faces: dict[str, FontFace] = {}

    def face(self, font_family: str, font_file: str | None = None) -> FontFace:
        path: Path | None = None
        if font_file:
            candidate = Path(font_file)
            if candidate.is_file():
                path = candidate
        if path is None:
            path = resolve_font_file(font_family)
        key = str(path)
        if key not in self._faces:
            self._faces[key] = FontFace(path)
        return self._faces[key]

    def face_by_path(self, path: str | Path) -> FontFace:
        key = str(Path(path))
        if key not in self._faces:
            self._faces[key] = FontFace(Path(path))
        return self._faces[key]

    def metrics(self, font_family: str, size: float) -> tuple[float, float]:
        return self.face(font_family).metrics(size)

    def shape(self, text: str, font_family: str, size: float) -> tuple[ShapedGlyph, ...]:
        return self.face(font_family).shape(text, size)

    def raster(self, font_family: str, gid: int, size: float) -> GlyphBitmap:
        return self.face(font_family).raster(gid, size)

    def raster_path(self, path: str | Path, gid: int, size: float) -> GlyphBitmap:
        return self.face_by_path(path).raster(gid, size)


class GlyphAtlas:
    """Empaquetado shelf de bitmaps 8-bit en un cuadrado de `size` px."""

    def __init__(self, size: int = ATLAS_SIZE) -> None:
        self.size = int(size)
        self.data = bytearray(self.size * self.size)
        self.rects: dict[tuple[str, int, int], tuple[int, int, int, int] | None] = {}
        self._shelf_x = self._shelf_y = self._shelf_h = 0

    def add(self, key: tuple[str, int, int], w: int, h: int,
            lines: tuple[bytes, ...] | list[bytes]) -> None:
        if w == 0 or h == 0:
            self.rects[key] = None
            return
        pad_w, pad_h = w + 2, h + 2  # 1 px de guarda evita sangrado del linear sampler
        if self._shelf_x + pad_w > self.size:
            self._shelf_x = 0
            self._shelf_y += self._shelf_h
            self._shelf_h = 0
        if self._shelf_y + pad_h > self.size:
            raise RuntimeError(f"atlas de glyphs desbordado al añadir {key}")
        x, y = self._shelf_x + 1, self._shelf_y + 1
        for row, line in enumerate(lines):
            offset = (y + row) * self.size + x
            self.data[offset:offset + w] = line
        self.rects[key] = (x, y, w, h)
        self._shelf_x += pad_w
        self._shelf_h = max(self._shelf_h, pad_h)

    def uv(self, key: tuple[str, int, int]) -> tuple[float, float, float, float]:
        rect = self.rects.get(key)
        if not rect:
            return 0.0, 0.0, 0.0, 0.0
        x, y, w, h = rect
        scale = float(self.size)
        return x / scale, y / scale, (x + w) / scale, (y + h) / scale

    @property
    def glyph_count(self) -> int:
        return sum(1 for rect in self.rects.values() if rect)


def build_atlas(engine: TextEngine,
                keys: list[tuple[str, int, int]]) -> GlyphAtlas:
    atlas = GlyphAtlas()
    for key in keys:
        path, gid, size = key
        bitmap = engine.raster_path(path, gid, size)
        atlas.add(key, bitmap.w, bitmap.h, bitmap.lines)
    return atlas


def text_op_glyph_keys(ops, engine: TextEngine) -> list[tuple[str, int, int]]:
    """Claves del atlas en orden de primera aparición, leyendo el IR."""
    keys: list[tuple[str, int, int]] = []
    for op in ops:
        if not isinstance(op, TextOp):
            continue
        face = engine.face(op.font, op.font_file or None)
        size_key = int(round(op.size))
        for glyph in op.glyphs:
            key = (str(face.path), glyph.gid, size_key)
            if key not in keys:
                keys.append(key)
    return keys


def build_text_ops(runs, engine: TextEngine) -> list[TextOp]:
    """TextRun (layout) -> TextOp (IR), con atlas y posiciones resueltas."""
    shaped = []
    keys: list[tuple[str, int, int]] = []
    for run in runs:
        face = engine.face(run.font_family)
        glyphs = face.shape(run.text, run.font_size)
        shaped.append((run, face, glyphs))
        size_key = int(round(run.font_size))
        for shaped_glyph in glyphs:
            key = (str(face.path), shaped_glyph.gid, size_key)
            if key not in keys:
                keys.append(key)
    atlas = build_atlas(engine, keys)

    ops: list[TextOp] = []
    for run, face, glyphs in shaped:
        size_key = int(round(run.font_size))
        pen = run.x
        out: list[Glyph] = []
        for shaped_glyph in glyphs:
            key = (str(face.path), shaped_glyph.gid, size_key)
            bitmap = engine.raster_path(str(face.path), shaped_glyph.gid, run.font_size)
            u0, v0, u1, v1 = atlas.uv(key)
            out.append(Glyph(
                gid=shaped_glyph.gid,
                x=round(pen + shaped_glyph.dx + bitmap.left, 3),
                y=round(run.baseline - shaped_glyph.dy - bitmap.top, 3),
                w=bitmap.w, h=bitmap.h, adv=round(shaped_glyph.adv, 4),
                u0=round(u0, 6), v0=round(v0, 6),
                u1=round(u1, 6), v1=round(v1, 6)))
            pen += shaped_glyph.adv
        ops.append(TextOp(
            x=round(run.x, 3), y=round(run.baseline, 3), text=run.text,
            font=first_family(run.font_family), font_file=str(face.path),
            size=round(run.font_size, 3), color=run.color, glyphs=tuple(out)))
    return ops
