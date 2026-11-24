"""IR de display list (rect + text_run) y ensamblado desde el layout.

Contrato:
  rect      {x,y,w,h,fill}
  text_run  {x, baseline, text, face, size, weight, color, advance, glyphs[]}
  glyph     {gid, cluster, x, y, w, h, u0,v0,u1,v1, adv}

Coordenadas en píxeles del viewport, origen arriba-izquierda. El backend wgpu
usa quads + UVs; el fallback Pillow usa el atlas directamente.
"""
import json
from dataclasses import dataclass, field

from .layout import Box, Line


@dataclass
class RectOp:
    x: float
    y: float
    w: float
    h: float
    fill: str
    type: str = "rect"

    def to_dict(self) -> dict:
        return {"type": "rect", "x": round(self.x), "y": round(self.y),
                "w": round(self.w), "h": round(self.h), "fill": self.fill}


@dataclass
class TextOp:
    x: float
    baseline: float
    text: str
    face: str
    size: float
    weight: int
    color: str
    advance: float
    glyphs: list[dict] = field(default_factory=list)
    type: str = "text_run"

    def to_dict(self) -> dict:
        return {"type": "text_run", "x": round(self.x, 3),
                "baseline": round(self.baseline, 3), "text": self.text,
                "face": self.face, "size": round(self.size, 3),
                "weight": self.weight, "color": self.color,
                "advance": round(self.advance, 3), "glyphs": self.glyphs}


@dataclass
class DisplayList:
    width: int
    height: int
    ops: list = field(default_factory=list)
    meta: dict = field(default_factory=dict)

    def to_dict(self) -> dict:
        data = {"viewport": {"width": self.width, "height": self.height},
                "ir_version": 1}
        data.update(self.meta)
        data["ops"] = [op.to_dict() for op in self.ops]
        return data

    def to_json(self, indent: int = 2) -> str:
        return json.dumps(self.to_dict(), indent=indent, ensure_ascii=False)

    @classmethod
    def from_json(cls, text: str) -> "DisplayList":
        data = json.loads(text)
        viewport = data["viewport"]
        meta = {k: v for k, v in data.items()
                if k not in ("viewport", "ir_version", "ops")}
        return cls(int(viewport["width"]), int(viewport["height"]),
                   data.get("ops", []), meta)

    def text_ops(self) -> list:
        return [op for op in self.ops if getattr(op, "type", None) == "text_run"]


def _collect_rects(box: Box, out: list[RectOp]) -> None:
    if box.style is not None and box.style.background and box.w > 0 and box.h > 0:
        out.append(RectOp(box.x, box.y, box.w, box.h, box.style.background))
    for child in box.children:
        _collect_rects(child, out)


def _collect_lines(box: Box, out: list[tuple[Line, str]]) -> None:
    """Texto en orden de documento, con la clase del elemento para depurar."""
    for line in box.lines:
        out.append((line, box.classes))
    for child in box.children:
        _collect_lines(child, out)


def build_display_list(root: Box, fonts, atlas, viewport: tuple[int, int]) -> DisplayList:
    rects: list[RectOp] = []
    _collect_rects(root, rects)
    lines: list[tuple[Line, str]] = []
    _collect_lines(root, lines)

    bitmaps: dict[tuple, tuple] = {}
    text_ops: list[TextOp] = []
    for line, _classes in lines:
        glyphs = fonts.shape(line.text, line.face, line.size)
        out_glyphs: list[dict] = []
        pen = line.x
        for glyph in glyphs:
            key = (line.face, int(round(line.size)), glyph["gid"])
            if key not in bitmaps:
                w, h, left, top, rows = fonts.raster(line.face, glyph["gid"], line.size)
                bitmaps[key] = (w, h, left, top)
                atlas.add(key, w, h, rows)
            w, h, left, top = bitmaps[key]
            u0, v0, u1, v1 = atlas.uv(key)
            out_glyphs.append({
                "gid": glyph["gid"], "cluster": glyph["cluster"],
                "x": round(pen + glyph["dx"] + left, 3),
                "y": round(line.baseline - glyph["dy"] - top, 3),
                "w": w, "h": h,
                "u0": round(u0, 6), "v0": round(v0, 6),
                "u1": round(u1, 6), "v1": round(v1, 6),
                "adv": round(glyph["adv"], 4),
            })
            pen += glyph["adv"]
        text_ops.append(TextOp(line.x, line.baseline, line.text, line.face,
                               line.size, line.weight, line.color, pen - line.x,
                               out_glyphs))

    ops = list(rects) + list(text_ops)
    meta = {
        "font_files": fonts.describe(),
        "atlas": atlas.to_json(),
        "stats": {
            "rects": len(rects),
            "text_runs": len(text_ops),
            "glyph_quads": sum(len(op.glyphs) for op in text_ops),
        },
    }
    return DisplayList(viewport[0], viewport[1], ops, meta)
