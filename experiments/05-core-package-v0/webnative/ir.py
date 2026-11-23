"""IR de display list, lenguaje-agnóstico y extensible por registro de ops.

Para añadir TextOp: dataclass que herede de Op, `type = "text"`, `to_dict()`,
`from_dict()` y decorarla con `register_op`. El JSON la reconocerá; cada
renderer decide si la soporta o lanza NotImplementedError.
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
