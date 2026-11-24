"""Exportación build-time del atlas de glyphs para el runtime nativo C (exp16).

El IR v1 (`text_run`) lleva posiciones y UVs de cada glyph, pero no los píxeles
del atlas: los backends lo reconstruyen rasterizando la misma fuente. Este
módulo hace exactamente eso, con las mismas claves y el mismo orden que
`renderer/wgpu_renderer.py` (`text_op_glyph_keys` + `build_atlas`), y serializa:

  * `atlas.bin`        - píxeles R8 crudos, 512x512 (orden de filas, sin padding)
  * `atlas.json`       - sidecar: tamaño, formato, glyph_count, sha256
  * `glyph_quads.json` - un quad por glyph: {run_id, color, x, y, w, h, uv...}

Además valida que las UVs/tamaños reconstruidos coinciden con los que el
frontend dejó en el IR (si no, el render nativo no podría ser idéntico).
"""
from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path

from .ir import DisplayList, TextOp
from .text import ATLAS_SIZE, GlyphAtlas, TextEngine, build_atlas, text_op_glyph_keys

UV_TOLERANCE = 1e-5


@dataclass(frozen=True)
class GlyphQuad:
    run_id: int
    color: str
    rgba: tuple[int, int, int, int]
    x: float
    y: float
    w: int
    h: int
    u0: float
    v0: float
    u1: float
    v1: float
    gid: int

    def to_dict(self) -> dict:
        return {
            "run_id": self.run_id,
            "color": self.color,
            "x": round(self.x, 3),
            "y": round(self.y, 3),
            "w": self.w,
            "h": self.h,
            "u0": round(self.u0, 6),
            "v0": round(self.v0, 6),
            "u1": round(self.u1, 6),
            "v1": round(self.v1, 6),
            "gid": self.gid,
        }


def parse_rgba(color: str) -> tuple[int, int, int, int]:
    value = color.strip().lower().lstrip("#")
    if len(value) == 3:
        value = "".join(ch * 2 for ch in value)
    if len(value) == 6:
        value += "ff"
    if len(value) != 8:
        raise ValueError(f"color no válida: {color!r}")
    return tuple(int(value[i : i + 2], 16) for i in range(0, 8, 2))  # type: ignore[return-value]


def build_scene_atlas(ops, engine: TextEngine | None = None
                      ) -> tuple[TextEngine, GlyphAtlas | None, list[tuple[str, int, int]]]:
    """Atlas canónico del documento, con el mismo orden de claves que wgpu."""
    engine = engine or TextEngine()
    keys = text_op_glyph_keys(ops, engine)
    if not keys:
        return engine, None, keys
    atlas = build_atlas(engine, keys)
    return engine, atlas, keys


def collect_quads(ops, engine: TextEngine, atlas: GlyphAtlas) -> list[GlyphQuad]:
    """Quads autoritativos (atlas) y validación contra las UVs del IR."""
    quads: list[GlyphQuad] = []
    run_id = 0
    for op in ops:
        if not isinstance(op, TextOp):
            continue
        face = engine.face(op.font, op.font_file or None)
        size_key = int(round(op.size))
        rgba = parse_rgba(op.color)
        for glyph in op.glyphs:
            key = (str(face.path), glyph.gid, size_key)
            rect = atlas.rects.get(key)
            if not rect:
                raise ValueError(
                    f"run {run_id}: glyph gid={glyph.gid} sin bitmap en el atlas"
                )
            ax, ay, aw, ah = rect
            u0, v0, u1, v1 = atlas.uv(key)
            if (aw, ah) != (glyph.w, glyph.h):
                raise ValueError(
                    f"run {run_id} gid={glyph.gid}: atlas {aw}x{ah} != IR "
                    f"{glyph.w}x{glyph.h}"
                )
            if max(abs(u0 - glyph.u0), abs(v0 - glyph.v0),
                   abs(u1 - glyph.u1), abs(v1 - glyph.v1)) > UV_TOLERANCE:
                raise ValueError(
                    f"run {run_id} gid={glyph.gid}: UV del atlas != UV del IR "
                    f"({u0:.6f},{v0:.6f},{u1:.6f},{v1:.6f} vs "
                    f"{glyph.u0:.6f},{glyph.v0:.6f},{glyph.u1:.6f},{glyph.v1:.6f})"
                )
            quads.append(GlyphQuad(
                run_id=run_id, color=op.color, rgba=rgba,
                x=float(glyph.x), y=float(glyph.y), w=aw, h=ah,
                u0=u0, v0=v0, u1=u1, v1=v1, gid=glyph.gid,
            ))
        run_id += 1
    return quads


def export_document(display: DisplayList, out_dir: str | Path
                    ) -> dict:
    """Escribe atlas.bin, atlas.json, glyph_quads.json y devuelve el resumen."""
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)

    engine, atlas, keys = build_scene_atlas(display.ops)
    if atlas is None:
        raise ValueError("el documento no contiene text_run: no hay atlas que exportar")
    quads = collect_quads(display.ops, engine, atlas)

    atlas_bytes = bytes(atlas.data)
    (out / "atlas.bin").write_bytes(atlas_bytes)

    summary = {
        "width": atlas.size,
        "height": atlas.size,
        "format": "r8unorm",
        "glyph_count": atlas.glyph_count,
        "atlas_size": ATLAS_SIZE,
        "size_bytes": len(atlas_bytes),
        "sha256": hashlib.sha256(atlas_bytes).hexdigest(),
        "pixels_nonzero": sum(1 for value in atlas_bytes if value),
        "runs": [],
        "quads": [quad.to_dict() for quad in quads],
    }
    runs = [op for op in display.ops if isinstance(op, TextOp)]
    for run_id, op in enumerate(runs):
        count = sum(1 for quad in quads if quad.run_id == run_id)
        summary["runs"].append({
            "run_id": run_id,
            "text": op.text,
            "font": op.font,
            "font_file": op.font_file,
            "size": op.size,
            "color": op.color,
            "glyph_count": count,
        })

    (out / "atlas.json").write_text(json.dumps(
        {k: summary[k] for k in
         ("width", "height", "format", "glyph_count", "size_bytes", "sha256",
          "pixels_nonzero")},
        indent=2) + "\n", encoding="utf-8")
    (out / "glyph_quads.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    summary["atlas_path"] = str(out / "atlas.bin")
    summary["quads_path"] = str(out / "glyph_quads.json")
    return summary
