#!/usr/bin/env python3
"""AOT back half of exp16: display-list IR + atlas -> baked C header.

Runs at BUILD time only. The generated `scene_data.h` contains everything the
native runtime needs, so `scene_text.exe` has no JSON, atlas file, Python or
font dependency at runtime:

  * kSceneOps[]        rect metadata (reporting / generic op loop)
  * kSceneVertices[]   6 vertices per rect, clip-space pos + unorm8x4 color
  * kGlyphQuads[]      one quad per glyph (runtime_reporting / inspection)
  * kGlyphVertices[]   6 vertices per glyph, pos + uv + unorm8x4 color (20 B)
  * kAtlasPixels[]     the 512x512 R8 atlas, raw (262144 bytes)
  * kRectShaderWGSL    solid pipeline
  * kGlyphShaderWGSL   textured glyph pipeline (texture_2d + sampler)
  * viewport + clear color + counts + strides

Usage (normally driven by run.py):
  emit_c.py --ir display_list.json --atlas atlas.bin --quads glyph_quads.json \
            --out scene_data.h
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

HEX = set("0123456789abcdef")


def parse_color(value: object) -> tuple[int, int, int, int]:
    if not isinstance(value, str):
        raise ValueError(f"fill must be a string, got {type(value).__name__}")
    text = value.strip().lower()
    if not text.startswith("#"):
        raise ValueError(f"fill must start with '#': {value!r}")
    digits = text[1:]
    if len(digits) == 3:
        digits = "".join(ch * 2 for ch in digits)
    elif len(digits) == 6:
        digits += "ff"
    elif len(digits) != 8:
        raise ValueError(f"fill must be #rgb, #rrggbb or #rrggbbaa: {value!r}")
    if any(ch not in HEX for ch in digits):
        raise ValueError(f"fill has non-hex characters: {value!r}")
    r, g, b, a = (int(digits[i : i + 2], 16) for i in range(0, 8, 2))
    return r, g, b, a


def number(raw: object, field: str, index: int) -> float:
    if isinstance(raw, bool) or not isinstance(raw, (int, float)):
        raise ValueError(f"op[{index}].{field} must be a number, got {raw!r}")
    return float(raw)


def load_ir(path: Path) -> tuple[int, int, list[dict], list[dict]]:
    with path.open("r", encoding="utf-8-sig") as handle:
        doc = json.load(handle)

    viewport = doc.get("viewport")
    if not isinstance(viewport, dict):
        raise ValueError("IR must contain a 'viewport' object")
    width = viewport.get("width")
    height = viewport.get("height")
    if not isinstance(width, int) or not isinstance(height, int) or width <= 0 or height <= 0:
        raise ValueError("viewport.width and viewport.height must be positive integers")

    ops = doc.get("ops")
    if not isinstance(ops, list) or not ops:
        raise ValueError("IR must contain a non-empty 'ops' array")

    rects: list[dict] = []
    texts: list[dict] = []
    for index, op in enumerate(ops):
        if not isinstance(op, dict):
            raise ValueError(f"op[{index}] must be an object")
        kind = op.get("type")
        if kind == "rect":
            x = number(op.get("x"), "x", index)
            y = number(op.get("y"), "y", index)
            w = number(op.get("w"), "w", index)
            h = number(op.get("h"), "h", index)
            if w <= 0 or h <= 0:
                raise ValueError(f"op[{index}] must have positive w/h, got {w}x{h}")
            r, g, b, a = parse_color(op.get("fill"))
            rects.append(
                {
                    "x": round(x), "y": round(y), "w": round(w), "h": round(h),
                    "rgba": (r << 24) | (g << 16) | (b << 8) | a,
                    "rgb": (r, g, b, a),
                    "fill": op["fill"],
                }
            )
        elif kind == "text_run":
            color = str(op.get("color", "#000000"))
            r, g, b, a = parse_color(color)
            texts.append(
                {
                    "x": number(op.get("x"), "x", index),
                    "y": number(op.get("y"), "y", index),
                    "text": str(op.get("text", "")),
                    "font": str(op.get("font", "")),
                    "size": number(op.get("size", 16.0), "size", index),
                    "color": color,
                    "rgba": (r, g, b, a),
                    "glyphs": op.get("glyphs", []),
                }
            )
        else:
            raise ValueError(f"op[{index}].type {kind!r} is not supported (rect/text_run)")
    if not rects:
        raise ValueError("IR must contain at least one rect op (clear color comes from op[0])")
    return width, height, rects, texts


def load_atlas(path: Path, quads_path: Path) -> tuple[int, int, bytes, list[dict], dict]:
    meta = json.loads(quads_path.read_text(encoding="utf-8-sig"))
    atlas_meta = meta.get("atlas") or meta
    width = int(atlas_meta.get("width", 0))
    height = int(atlas_meta.get("height", 0))
    if width <= 0 or height <= 0:
        raise ValueError(f"{quads_path}: missing positive atlas width/height")
    data = path.read_bytes()
    if len(data) != width * height:
        raise ValueError(
            f"{path}: {len(data)} bytes != {width}x{height} R8 ({width * height})")
    quads = meta.get("quads")
    if not isinstance(quads, list):
        raise ValueError(f"{quads_path}: missing 'quads' array")
    return width, height, data, quads, meta


def f32(value: float) -> str:
    """Emit the exact float32 the frontend would use (array('f'))."""
    value32 = struct.unpack("<f", struct.pack("<f", float(value)))[0]
    text = repr(value32)
    if "e" in text or "E" in text:
        text = f"{value32:.9g}"
    if "." not in text and "e" not in text and "E" not in text:
        text += ".0"
    return text + "f"


def ndc(x: float, y: float, width: int, height: int) -> tuple[float, float]:
    return x / width * 2.0 - 1.0, 1.0 - y / height * 2.0


def hex_hash(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()[:16]


RECT_SHADER_LINES = [
    "// Generated by emit_c.py (exp16): solid rect pipeline.",
    "// Vertex format: @location(0)=clip-space vec2, @location(1)=unorm8x4 color.",
    "struct VsOut {",
    "    @builtin(position) clip_pos: vec4f,",
    "    @location(0) color: vec4f,",
    "};",
    "",
    "@vertex",
    "fn vs_main(@location(0) pos: vec2f, @location(1) color: vec4f) -> VsOut {",
    "    var o: VsOut;",
    "    o.clip_pos = vec4f(pos, 0.0, 1.0);",
    "    o.color = color;",
    "    return o;",
    "}",
    "",
    "@fragment",
    "fn fs_main(in: VsOut) -> @location(0) vec4f {",
    "    return in.color;",
    "}",
]

GLYPH_SHADER_LINES = [
    "// Generated by emit_c.py (exp16): textured glyph pipeline.",
    "// Vertex format: @location(0)=clip-space vec2, @location(1)=atlas uv vec2,",
    "// @location(2)=unorm8x4 color. The atlas is R8Unorm: coverage in .r.",
    "struct VsOut {",
    "    @builtin(position) clip_pos: vec4f,",
    "    @location(0) uv: vec2f,",
    "    @location(1) color: vec4f,",
    "};",
    "",
    "@vertex",
    "fn vs_main(@location(0) pos: vec2f, @location(1) uv: vec2f,",
    "           @location(2) color: vec4f) -> VsOut {",
    "    var o: VsOut;",
    "    o.clip_pos = vec4f(pos, 0.0, 1.0);",
    "    o.uv = uv;",
    "    o.color = color;",
    "    return o;",
    "}",
    "",
    "@group(0) @binding(0) var atlas_tex: texture_2d<f32>;",
    "@group(0) @binding(1) var atlas_sampler: sampler;",
    "",
    "@fragment",
    "fn fs_main(in: VsOut) -> @location(0) vec4f {",
    "    let coverage = textureSample(atlas_tex, atlas_sampler, in.uv).r;",
    "    return vec4f(in.color.rgb, in.color.a * coverage);",
    "}",
]


def shader_block(lines: list[str]) -> list[str]:
    out = []
    for line in lines:
        escaped = line.replace("\\", "\\\\").replace('"', '\\"')
        out.append(f'    "{escaped}\\n"')
    out.append("    ;")
    return out


def emit_atlas_array(data: bytes, per_line: int = 32) -> list[str]:
    lines = ["static const unsigned char kAtlasPixels[ATLAS_PIXELS] = {"]
    for start in range(0, len(data), per_line):
        chunk = data[start:start + per_line]
        lines.append("    " + ",".join(str(value) for value in chunk) + ",")
    lines.append("};")
    return lines


def render_header(ir_path: Path, digest: str, width: int, height: int,
                  rects: list[dict], texts: list[dict], quads: list[dict],
                  atlas_width: int, atlas_height: int, atlas: bytes) -> str:
    clear = rects[0]["rgb"]
    glyph_count = len(quads)
    lines = [
        "/* AUTO-GENERATED by emit_c.py from display_list.json + atlas.bin. DO NOT EDIT. */",
        f"/* source: {ir_path.name}  sha256: {digest}  atlas sha256: {hex_hash(atlas)} */",
        f"/* scene: {width}x{height}, {len(rects)} rect op(s) + {len(texts)} text run(s), "
        f"painter's order: rects first, then glyphs */",
        f"/* atlas: {atlas_width}x{atlas_height} r8unorm, {glyph_count} glyph quad(s) */",
        "#ifndef SCENE_DATA_H",
        "#define SCENE_DATA_H",
        "",
        "/* Keep these layouts in sync with runtime_text.c. */",
        "typedef struct {",
        "    float x, y;                   /* clip space, -1..1 */",
        "    unsigned char r, g, b, a;     /* normalized unorm8x4 */",
        "} SceneVertex;",
        "",
        "typedef struct {",
        "    float x, y, w, h;             /* device px, top-left origin */",
        "    unsigned int rgba;            /* 0xRRGGBBAA */",
        "} RectOp;",
        "",
        "typedef struct {",
        "    float x, y;                   /* clip space, -1..1 */",
        "    float u, v;                   /* atlas uv */",
        "    unsigned char r, g, b, a;     /* normalized unorm8x4 */",
        "} GlyphVertex;",
        "",
        "typedef struct {",
        "    float x, y, w, h;             /* device px, top-left origin */",
        "    float u0, v0, u1, v1;         /* atlas uv rect */",
        "    unsigned int rgba;            /* 0xRRGGBBAA */",
        "    int run_id;",
        "    int gid;                      /* HarfBuzz glyph id (reporting only) */",
        "} GlyphQuad;",
        "",
        f"#define SCENE_WIDTH  {width}",
        f"#define SCENE_HEIGHT {height}",
        f"#define SCENE_OP_COUNT {len(rects) + len(texts)}",
        f"#define SCENE_RECT_COUNT {len(rects)}",
        f"#define SCENE_TEXT_RUN_COUNT {len(texts)}",
        f"#define SCENE_VERTEX_COUNT {len(rects) * 6}",
        "#define SCENE_VERTEX_STRIDE 12",
        "#define SCENE_GLYPH_QUAD_COUNT %d" % glyph_count,
        "#define SCENE_GLYPH_VERTEX_COUNT %d" % (glyph_count * 6),
        "#define SCENE_GLYPH_VERTEX_STRIDE 20",
        f"#define ATLAS_WIDTH {atlas_width}",
        f"#define ATLAS_HEIGHT {atlas_height}",
        "#define ATLAS_PIXELS (ATLAS_WIDTH * ATLAS_HEIGHT)",
        "#define ATLAS_GLYPH_COUNT %d" % glyph_count,
        f"#define SCENE_CLEAR_R ({clear[0]}.0f / 255.0f)",
        f"#define SCENE_CLEAR_G ({clear[1]}.0f / 255.0f)",
        f"#define SCENE_CLEAR_B ({clear[2]}.0f / 255.0f)",
        f"#define SCENE_CLEAR_A ({clear[3]}.0f / 255.0f)",
        "",
        "static const RectOp kSceneOps[SCENE_RECT_COUNT] = {",
    ]
    for op in rects:
        lines.append(
            "    {{ {x:>7}, {y:>7}, {w:>7}, {h:>7}, 0x{rgba:08X}u }}, /* {fill} */".format(
                x=f32(op["x"]), y=f32(op["y"]), w=f32(op["w"]), h=f32(op["h"]),
                rgba=op["rgba"], fill=op["fill"],
            )
        )
    lines += [
        "};",
        "",
        "/* Two triangles per rect: TL, TR, BR, TL, BR, BL. */",
        "static const SceneVertex kSceneVertices[SCENE_VERTEX_COUNT] = {",
    ]
    for op in rects:
        x, y, w, h = op["x"], op["y"], op["w"], op["h"]
        rgb = op["rgb"]
        lines.append(f"    /* rect {op['fill']} at ({x:g},{y:g}) {w:g}x{h:g} */")
        for vx, vy in ((x, y), (x + w, y), (x + w, y + h), (x, y), (x + w, y + h), (x, y + h)):
            nx, ny = ndc(vx, vy, width, height)
            lines.append(
                f"    {{ {f32(nx):>14}, {f32(ny):>14}, {rgb[0]:3d}, {rgb[1]:3d}, "
                f"{rgb[2]:3d}, {rgb[3]:3d} }},"
            )
    lines += [
        "};",
        "",
        "/* Glyph quads as they came from the atlas (reporting / cross-checks). */",
        "static const GlyphQuad kGlyphQuads[SCENE_GLYPH_QUAD_COUNT] = {",
    ]
    for quad in quads:
        r, g, b, a = parse_color(quad["color"])
        lines.append(
            "    {{ {x:>8}, {y:>6}, {w:>3}, {h:>3}, {u0:>10}, {v0:>10}, {u1:>10}, "
            "{v1:>10}, 0x{rgba:08X}u, {run}, {gid} }},".format(
                x=f32(quad["x"]), y=f32(quad["y"]), w=quad["w"], h=quad["h"],
                u0=f32(quad["u0"]), v0=f32(quad["v0"]),
                u1=f32(quad["u1"]), v1=f32(quad["v1"]),
                rgba=(r << 24) | (g << 16) | (b << 8) | a, run=int(quad["run_id"]),
                gid=int(quad.get("gid", -1)),
            )
        )
    lines += [
        "};",
        "",
        "/* Two triangles per glyph: TL, BL, BR, TL, BR, TR (matches exp09 wgpu). */",
        "static const GlyphVertex kGlyphVertices[SCENE_GLYPH_VERTEX_COUNT] = {",
    ]
    for index, quad in enumerate(quads):
        x, y, w, h = quad["x"], quad["y"], quad["w"], quad["h"]
        u0, v0 = quad["u0"], quad["v0"]
        u1, v1 = quad["u1"], quad["v1"]
        r, g, b, a = parse_color(quad["color"])
        x0, y0 = ndc(x, y, width, height)
        x1, y1 = ndc(x + w, y + h, width, height)
        lines.append(
            f"    /* glyph {index} gid={quad.get('gid', -1)} run={quad['run_id']} "
            f"{quad['color']} at ({x:g},{y:g}) {w}x{h} */"
        )
        for vx, vy, u, v in (
            (x0, y0, u0, v0), (x0, y1, u0, v1), (x1, y1, u1, v1),
            (x0, y0, u0, v0), (x1, y1, u1, v1), (x1, y0, u1, v0),
        ):
            lines.append(
                f"    {{ {f32(vx):>14}, {f32(vy):>14}, {f32(u):>10}, {f32(v):>10}, "
                f"{r:3d}, {g:3d}, {b:3d}, {a:3d} }},"
            )
    lines += ["};", ""]
    lines += emit_atlas_array(atlas)
    lines += ["", "static const char kRectShaderWGSL[] ="]
    lines += shader_block(RECT_SHADER_LINES)
    lines += ["", "static const char kGlyphShaderWGSL[] ="]
    lines += shader_block(GLYPH_SHADER_LINES)
    lines += ["", "#endif /* SCENE_DATA_H */", ""]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Bake display-list IR + glyph atlas into a self-contained C header")
    parser.add_argument("--ir", default="display_list.json", type=Path)
    parser.add_argument("--atlas", default="atlas.bin", type=Path)
    parser.add_argument("--quads", default="glyph_quads.json", type=Path)
    parser.add_argument("--out", default="scene_data.h", type=Path)
    args = parser.parse_args(argv)

    for path, what in ((args.ir, "IR"), (args.atlas, "atlas"), (args.quads, "quads")):
        if not path.exists():
            print(f"error: {what} not found: {path}", file=sys.stderr)
            return 1
    try:
        width, height, rects, texts = load_ir(args.ir)
        atlas_width, atlas_height, atlas, quads, _meta = load_atlas(args.atlas, args.quads)
    except (ValueError, json.JSONDecodeError) as exc:
        print(f"error: invalid input: {exc}", file=sys.stderr)
        return 1

    ir_glyphs = sum(len(text["glyphs"]) for text in texts)
    if len(quads) != ir_glyphs:
        print(f"error: atlas has {len(quads)} quads but IR has {ir_glyphs} glyphs",
              file=sys.stderr)
        return 1

    raw = args.ir.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()[:16]
    header = render_header(args.ir, digest, width, height, rects, texts, quads,
                           atlas_width, atlas_height, atlas)
    args.out.write_text(header, encoding="utf-8", newline="\n")

    print(
        f"emit_c: {args.ir} + {args.atlas.name} -> {args.out} | "
        f"{len(rects)} rect ops, {len(texts)} text runs, {len(quads)} glyph quads, "
        f"{len(rects) * 6} rect verts + {len(quads) * 6} glyph verts, "
        f"atlas {atlas_width}x{atlas_height} ({len(atlas)} B), {width}x{height}, "
        f"sha256:{digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
