#!/usr/bin/env python3
"""AOT text-capable emitter: display-list IR + R8 glyph atlas -> scene_data.h.

Runs at BUILD time only. The generated header is a superset of the rect-only
header produced by emit_c.py, so both the canonical window runtime and the
offscreen runtimes can consume it without a JSON parser, Python, FreeType or
HarfBuzz at runtime:

  * kSceneOps[]        rect metadata (one entry per rect op)
  * kSceneVertices[]   6 vertices per rect, clip-space pos + unorm8x4 color
  * kGlyphQuads[]      one quad per glyph (x/y/w/h/uv/color/run/gid)
  * kGlyphVertices[]   6 vertices per glyph, pos + uv + unorm8x4 color (20 B)
  * kAtlasPixels[]     the R8 coverage atlas, raw rows (width * height bytes)
  * kRectShaderWGSL    solid rect pipeline
  * kGlyphShaderWGSL   textured glyph pipeline (texture_2d + sampler)
  * viewport, clear color, counts, strides and atlas dimensions

The glyph quads are derived from the `glyphs` arrays already present in the
IR (the same authoritative data exp16 exported to glyph_quads.json). Pass
--quads to cross-check the derivation against an exported glyph_quads.json.

Atlas dimensions are resolved in this order:
  1. --atlas-width/--atlas-height when both are > 0
  2. --atlas-json (default: atlas.json next to --atlas, when it exists)
  3. a square atlas whose side is sqrt(file size)

Usage:
  emit_c_text.py --ir display_list.json --atlas atlas.bin --out scene_data.h
  emit_c_text.py --ir display_list.json --atlas atlas.bin \
                 --atlas-json atlas.json --quads glyph_quads.json \
                 --out scene_data.h
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
from pathlib import Path

HEX = set("0123456789abcdef")


def parse_color(value: object) -> tuple[int, int, int, int]:
    if not isinstance(value, str):
        raise ValueError(f"color must be a string, got {type(value).__name__}")
    text = value.strip().lower()
    if not text.startswith("#"):
        raise ValueError(f"color must start with '#': {value!r}")
    digits = text[1:]
    if len(digits) == 3:
        digits = "".join(ch * 2 for ch in digits)
    elif len(digits) == 6:
        digits += "ff"
    elif len(digits) != 8:
        raise ValueError(f"color must be #rgb, #rrggbb or #rrggbbaa: {value!r}")
    if any(ch not in HEX for ch in digits):
        raise ValueError(f"color has non-hex characters: {value!r}")
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
            glyphs = op.get("glyphs", [])
            if not isinstance(glyphs, list):
                raise ValueError(f"op[{index}].glyphs must be an array")
            texts.append(
                {
                    "x": number(op.get("x"), "x", index),
                    "y": number(op.get("y"), "y", index),
                    "text": str(op.get("text", "")),
                    "font": str(op.get("font", "")),
                    "size": number(op.get("size", 16.0), "size", index),
                    "color": color,
                    "rgba": (r, g, b, a),
                    "glyphs": glyphs,
                }
            )
        else:
            raise ValueError(
                f"op[{index}].type {kind!r} is not supported (rect/text_run)")
    if not rects:
        raise ValueError("IR must contain at least one rect op (clear color comes from op[0])")
    return width, height, rects, texts


def build_quads(texts: list[dict]) -> list[dict]:
    """Flatten the IR text runs into the same flat quad list exp16 exported."""
    quads: list[dict] = []
    for run_id, run in enumerate(texts):
        for glyph_index, glyph in enumerate(run["glyphs"]):
            if not isinstance(glyph, dict):
                raise ValueError(
                    f"text run {run_id} glyph[{glyph_index}] must be an object")
            x = number(glyph.get("x"), "glyphs[].x", run_id)
            y = number(glyph.get("y"), "glyphs[].y", run_id)
            w = number(glyph.get("w"), "glyphs[].w", run_id)
            h = number(glyph.get("h"), "glyphs[].h", run_id)
            if w <= 0 or h <= 0:
                raise ValueError(
                    f"text run {run_id} glyph[{glyph_index}] has non-positive size {w}x{h}")
            if not float(w).is_integer() or not float(h).is_integer():
                raise ValueError(
                    f"text run {run_id} glyph[{glyph_index}] w/h must be integer pixels, "
                    f"got {w}x{h}")
            quads.append(
                {
                    "run_id": run_id,
                    "color": run["color"],
                    "x": x,
                    "y": y,
                    "w": int(w),
                    "h": int(h),
                    "u0": number(glyph.get("u0"), "glyphs[].u0", run_id),
                    "v0": number(glyph.get("v0"), "glyphs[].v0", run_id),
                    "u1": number(glyph.get("u1"), "glyphs[].u1", run_id),
                    "v1": number(glyph.get("v1"), "glyphs[].v1", run_id),
                    "gid": int(glyph.get("gid", -1)),
                }
            )
    return quads


def f32_bits(value: float) -> bytes:
    return struct.pack("<f", float(value))


def load_atlas(path: Path, atlas_json: Path | None, width_arg: int,
               height_arg: int) -> tuple[int, int, bytes, dict]:
    data = path.read_bytes()
    meta: dict = {}
    if atlas_json is not None:
        with atlas_json.open("r", encoding="utf-8-sig") as handle:
            meta = json.load(handle)
        declared = meta.get("size_bytes")
        if isinstance(declared, int) and declared != len(data):
            raise ValueError(
                f"{atlas_json}: size_bytes={declared} but {path} has {len(data)} bytes")
        declared_sha = meta.get("sha256")
        if isinstance(declared_sha, str) and declared_sha:
            actual = hashlib.sha256(data).hexdigest()
            if actual.lower() != declared_sha.lower():
                raise ValueError(
                    f"{path}: sha256 {actual[:16]} != atlas.json {declared_sha[:16]}")
        fmt = str(meta.get("format", "")).lower()
        if fmt and fmt != "r8unorm":
            raise ValueError(f"{atlas_json}: unsupported atlas format {meta['format']!r}")

    if width_arg > 0 and height_arg > 0:
        width, height = width_arg, height_arg
    else:
        width = int(meta.get("width", 0))
        height = int(meta.get("height", 0))
        if width <= 0 or height <= 0:
            side = math.isqrt(len(data))
            if side * side != len(data):
                raise ValueError(
                    f"{path}: {len(data)} bytes is not a square R8 atlas; "
                    f"pass --atlas-width/--atlas-height or --atlas-json")
            width = height = side
    if width <= 0 or height <= 0:
        raise ValueError(f"{path}: could not determine atlas dimensions")
    if len(data) != width * height:
        raise ValueError(
            f"{path}: {len(data)} bytes != {width}x{height} R8 ({width * height})")
    return width, height, data, meta


def cross_check_quads(quads: list[dict], quads_path: Path, atlas_width: int,
                      atlas_height: int) -> None:
    with quads_path.open("r", encoding="utf-8-sig") as handle:
        doc = json.load(handle)
    meta = doc.get("atlas") if isinstance(doc.get("atlas"), dict) else doc
    width = int(meta.get("width", 0))
    height = int(meta.get("height", 0))
    if width and height and (width != atlas_width or height != atlas_height):
        raise ValueError(
            f"{quads_path}: atlas {width}x{height} != atlas.bin {atlas_width}x{atlas_height}")
    exported = doc.get("quads")
    if not isinstance(exported, list):
        raise ValueError(f"{quads_path}: missing 'quads' array")
    if len(exported) != len(quads):
        raise ValueError(
            f"{quads_path}: {len(exported)} quads != {len(quads)} glyphs in the IR")
    for index, (ours, theirs) in enumerate(zip(quads, exported)):
        if int(theirs.get("run_id", -1)) != ours["run_id"]:
            raise ValueError(f"{quads_path}: quad[{index}] run_id mismatch")
        if parse_color(theirs.get("color", "#000000")) != parse_color(ours["color"]):
            raise ValueError(f"{quads_path}: quad[{index}] color mismatch")
        if int(theirs.get("gid", -1)) != ours["gid"]:
            raise ValueError(f"{quads_path}: quad[{index}] gid mismatch")
        if int(round(float(theirs.get("w", 0)))) != ours["w"] or \
           int(round(float(theirs.get("h", 0)))) != ours["h"]:
            raise ValueError(f"{quads_path}: quad[{index}] w/h mismatch")
        for field in ("x", "y", "u0", "v0", "u1", "v1"):
            if f32_bits(float(theirs.get(field, 0.0))) != f32_bits(ours[field]):
                raise ValueError(
                    f"{quads_path}: quad[{index}].{field} mismatch "
                    f"({theirs.get(field)} vs {ours[field]})")
    print(f"emit_c_text: quads cross-checked against {quads_path.name} "
          f"({len(quads)} entries, float32-exact)")


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
    "// Generated by emit_c_text.py: solid rect pipeline.",
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
    "// Generated by emit_c_text.py: textured glyph pipeline.",
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
        "/* AUTO-GENERATED by emit_c_text.py from display_list.json + atlas.bin. DO NOT EDIT. */",
        f"/* source: {ir_path.name}  sha256: {digest}  atlas sha256: {hex_hash(atlas)} */",
        f"/* scene: {width}x{height}, {len(rects)} rect op(s) + {len(texts)} text run(s), "
        f"painter's order: rects first, then glyphs */",
        f"/* atlas: {atlas_width}x{atlas_height} r8unorm, {glyph_count} glyph quad(s) */",
        "#ifndef SCENE_DATA_H",
        "#define SCENE_DATA_H",
        "",
        "/* Keep these layouts in sync with the runtime vertex buffer layouts. */",
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
        f"#define SCENE_RECT_COUNT {len(rects)}",
        f"#define SCENE_TEXT_RUN_COUNT {len(texts)}",
        "/* Legacy: rect-only runtimes iterate kSceneOps/SCENE_OP_COUNT. */",
        "#define SCENE_OP_COUNT SCENE_RECT_COUNT",
        "#define SCENE_TOTAL_OP_COUNT (SCENE_RECT_COUNT + SCENE_TEXT_RUN_COUNT)",
        f"#define SCENE_VERTEX_COUNT {len(rects) * 6}",
        "#define SCENE_VERTEX_STRIDE 12",
        f"#define SCENE_GLYPH_QUAD_COUNT {glyph_count}",
        f"#define SCENE_GLYPH_VERTEX_COUNT {glyph_count * 6}",
        "#define SCENE_GLYPH_VERTEX_STRIDE 20",
        f"#define ATLAS_WIDTH {atlas_width}",
        f"#define ATLAS_HEIGHT {atlas_height}",
        "#define ATLAS_PIXELS (ATLAS_WIDTH * ATLAS_HEIGHT)",
        f"#define ATLAS_GLYPH_COUNT {glyph_count}",
        "/* Marks a header emitted by emit_c_text.py (rect + optional glyphs). */",
        "#define SCENE_TEXT_CAPABLE 1",
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
    lines += ["};", ""]

    if glyph_count > 0:
        lines += [
            "/* Glyph quads as derived from the IR (reporting / cross-checks). */",
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
                    rgba=(r << 24) | (g << 16) | (b << 8) | a, run=quad["run_id"],
                    gid=quad["gid"],
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
                f"    /* glyph {index} gid={quad['gid']} run={quad['run_id']} "
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
        lines += [""]

    lines += ["static const char kRectShaderWGSL[] ="]
    lines += shader_block(RECT_SHADER_LINES)
    if glyph_count > 0:
        lines += ["", "static const char kGlyphShaderWGSL[] ="]
        lines += shader_block(GLYPH_SHADER_LINES)
    lines += [
        "",
        "/* Legacy alias so rect-only runtimes (runtime_gpu.c) build unchanged. */",
        "#define kSceneShaderWGSL kRectShaderWGSL",
        "",
        "#endif /* SCENE_DATA_H */",
        "",
    ]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Bake display-list IR + glyph atlas into a self-contained C header")
    parser.add_argument("--ir", default="display_list.json", type=Path)
    parser.add_argument("--atlas", default="atlas.bin", type=Path)
    parser.add_argument("--atlas-json", default=None, type=Path)
    parser.add_argument("--atlas-width", default=0, type=int)
    parser.add_argument("--atlas-height", default=0, type=int)
    parser.add_argument("--quads", default=None, type=Path,
                        help="optional glyph_quads.json to cross-check the IR")
    parser.add_argument("--out", default="scene_data.h", type=Path)
    args = parser.parse_args(argv)

    if not args.ir.exists():
        print(f"error: IR not found: {args.ir}", file=sys.stderr)
        return 1
    try:
        width, height, rects, texts = load_ir(args.ir)
        quads = build_quads(texts)
    except (ValueError, json.JSONDecodeError) as exc:
        print(f"error: invalid IR: {exc}", file=sys.stderr)
        return 1

    atlas_width = atlas_height = 0
    atlas = b""
    if quads:
        if not args.atlas.exists():
            print(f"error: atlas not found: {args.atlas}", file=sys.stderr)
            return 1
        atlas_json = args.atlas_json
        if atlas_json is None:
            candidate = args.atlas.with_name("atlas.json")
            if candidate.exists():
                atlas_json = candidate
        elif not atlas_json.exists():
            print(f"error: atlas.json not found: {atlas_json}", file=sys.stderr)
            return 1
        try:
            atlas_width, atlas_height, atlas, _meta = load_atlas(
                args.atlas, atlas_json, args.atlas_width, args.atlas_height)
        except (ValueError, json.JSONDecodeError) as exc:
            print(f"error: invalid atlas: {exc}", file=sys.stderr)
            return 1

    if args.quads is not None:
        if not args.quads.exists():
            print(f"error: quads file not found: {args.quads}", file=sys.stderr)
            return 1
        try:
            cross_check_quads(quads, args.quads, atlas_width, atlas_height)
        except (ValueError, json.JSONDecodeError) as exc:
            print(f"error: quads cross-check failed: {exc}", file=sys.stderr)
            return 1

    raw = args.ir.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()[:16]
    header = render_header(args.ir, digest, width, height, rects, texts, quads,
                           atlas_width, atlas_height, atlas)
    args.out.write_text(header, encoding="utf-8", newline="\n")

    atlas_descriptor = (f"{args.atlas.name} {atlas_width}x{atlas_height} ({len(atlas)} B)"
                        if quads else "no atlas (rect-only)")
    print(
        f"emit_c_text: {args.ir} -> {args.out} | "
        f"{len(rects)} rect ops, {len(texts)} text runs, {len(quads)} glyph quads, "
        f"{len(rects) * 6} rect verts + {len(quads) * 6} glyph verts, "
        f"{atlas_descriptor}, {width}x{height}, sha256:{digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
