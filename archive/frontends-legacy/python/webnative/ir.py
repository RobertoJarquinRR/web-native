"""IR de display list, lenguaje-agnóstico y extensible por registro de ops.

v1 incorpora `TextOp` (type `"text_run"`, formato de exp06): posición de la run
y de cada glyph ya colocada por HarfBuzz, más un nombre de fuente, tamaño y
color. Las UV apuntan a un atlas canónico que cada backend reconstruye a partir
de la propia lista de glyphs; son una pista de empaquetado, no una imagen.

Para añadir otra op: dataclass que herede de Op, `type = "..."`, `to_dict()`,
`from_dict()` y decorarla con `register_op`.
"""
import json
from dataclasses import dataclass, field
from typing import ClassVar


class Op:
    type: ClassVar[str] = ""

    def to_dict(self) -> dict:
        raise NotImplementedError


_OPS: dict[str, type[Op]] = {}


def register_op(cls: type[Op]) -> type[Op]:
    if not cls.type:
        raise ValueError("la op necesita un atributo type")
    _OPS[cls.type] = cls
    return cls


@register_op
@dataclass(frozen=True)
class RectOp(Op):
    x: float
    y: float
    w: float
    h: float
    fill: str
    type: ClassVar[str] = "rect"

    def to_dict(self) -> dict:
        return {"type": self.type, "x": round(self.x), "y": round(self.y),
                "w": round(self.w), "h": round(self.h), "fill": self.fill}

    @classmethod
    def from_dict(cls, data: dict) -> "RectOp":
        return cls(float(data["x"]), float(data["y"]), float(data["w"]),
                   float(data["h"]), str(data["fill"]))


@dataclass(frozen=True)
class Glyph:
    """Un quad de glyph en coordenadas de imagen (x,y = esquina superior izq.)."""
    gid: int
    x: float
    y: float
    w: int
    h: int
    adv: float = 0.0
    u0: float = 0.0
    v0: float = 0.0
    u1: float = 0.0
    v1: float = 0.0

    def to_dict(self) -> dict:
        return {"gid": self.gid, "x": round(self.x, 3), "y": round(self.y, 3),
                "w": self.w, "h": self.h,
                "u0": round(self.u0, 6), "v0": round(self.v0, 6),
                "u1": round(self.u1, 6), "v1": round(self.v1, 6),
                "adv": round(self.adv, 4)}

    @classmethod
    def from_dict(cls, data: dict) -> "Glyph":
        return cls(int(data["gid"]), float(data["x"]), float(data["y"]),
                   int(data["w"]), int(data["h"]), float(data.get("adv", 0.0)),
                   float(data.get("u0", 0.0)), float(data.get("v0", 0.0)),
                   float(data.get("u1", 0.0)), float(data.get("v1", 0.0)))


@register_op
@dataclass(frozen=True)
class TextOp(Op):
    """Text run pre-shapeada. `x`,`y` = origen y baseline de la run.

    `font_file` es una pista absoluta para que otro backend rasterice los
    mismos glyphs; si no existe, se resuelve `font` con `fonts.resolve_font_file`.
    `text` permite un fallback aproximado (CPU) sin invertir el cmap.
    """
    x: float
    y: float
    text: str
    font: str
    size: float
    color: str
    glyphs: tuple[Glyph, ...] = ()
    font_file: str = ""
    type: ClassVar[str] = "text_run"

    def to_dict(self) -> dict:
        return {"type": self.type, "x": round(self.x, 3), "y": round(self.y, 3),
                "text": self.text, "font": self.font, "font_file": self.font_file,
                "size": round(self.size, 3), "color": self.color,
                "glyphs": [glyph.to_dict() for glyph in self.glyphs]}

    @classmethod
    def from_dict(cls, data: dict) -> "TextOp":
        return cls(float(data["x"]), float(data["y"]), str(data.get("text", "")),
                   str(data.get("font", "")), float(data.get("size", 16.0)),
                   str(data.get("color", "#000000")),
                   tuple(Glyph.from_dict(item) for item in data.get("glyphs", [])),
                   str(data.get("font_file", "")))


# Alias descriptivo: el tipo JSON sigue siendo "text_run" (formato exp06).
TextRunOp = TextOp


def op_from_dict(data: dict) -> Op:
    try:
        return _OPS[data.get("type")].from_dict(data)
    except KeyError:
        raise ValueError(f"tipo de op no soportado: {data.get('type')!r}") from None


@dataclass
class DisplayList:
    width: int
    height: int
    ops: list[Op] = field(default_factory=list)

    @property
    def viewport(self) -> dict:
        return {"width": self.width, "height": self.height}

    def to_dict(self) -> dict:
        return {"viewport": self.viewport, "ops": [op.to_dict() for op in self.ops]}

    def to_json(self, indent: int = 2) -> str:
        return json.dumps(self.to_dict(), indent=indent, ensure_ascii=False)

    @classmethod
    def from_dict(cls, data: dict) -> "DisplayList":
        viewport = data["viewport"]
        return cls(int(viewport["width"]), int(viewport["height"]),
                   [op_from_dict(item) for item in data.get("ops", [])])

    @classmethod
    def from_json(cls, text: str) -> "DisplayList":
        return cls.from_dict(json.loads(text))
