#!/usr/bin/env python3
"""Build-time emitter for the R2 style-tree runtime (`src/runtime/c-r2`).

Parses an HTML + CSS fixture with the canonical Python frontend
(`src/frontend/python/webnative`, the oracle), computes styles with
cssselect2, then flattens the styled tree to a pre-order UiNode array in
ui_data.h plus a debug ui_meta.json. The generated header is consumed by
runtime_r2.c, which computes layout, handles input and renders with
wgpu-native.

The Python package is imported from `src/frontend/python` by adding that
directory to sys.path; it is never copied into the runtime.

R2-specific data merged here (the oracle's ComputedStyle deliberately drops
it from its display-list IR):
  - percentage width/height (width_pct / height_pct)
  - flex-wrap
  - data-hover / data-press / data-selected / data-action from HTML attributes
  - text line boxes measured with HarfBuzz (build-time only; the runtime never
    links a font engine)

NOTE: R2 uses the Python frontend because the canonical Rust frontend emits a
post-layout display list, not a pre-layout style tree. Extending Rust to emit
the style tree is the pending step (see docs/architecture.md).

Usage:
  .venv\\Scripts\\python.exe emit_ui.py
  .venv\\Scripts\\python.exe emit_ui.py --html page/index.html --css page/style.css \
      --out ui_data.h --meta ui_meta.json
"""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path

# src/emitter/python/emit_ui.py -> src/emitter/python -> src/emitter -> src
SRC_ROOT = Path(__file__).resolve().parents[2]
FRONTEND_PYTHON = SRC_ROOT / "frontend" / "python"
if str(FRONTEND_PYTHON) not in sys.path:
    sys.path.insert(0, str(FRONTEND_PYTHON))

from tinycss2.color3 import parse_color as css_color

from webnative.cascade import compute_styles
from webnative.css_parse import parse_stylesheets
from webnative.html_parse import parse_html_file, stylesheet_texts
from webnative.tree import Node

DEFAULT_HTML = SRC_ROOT / "runtime" / "c-r2" / "page" / "index.html"
DEFAULT_CSS = SRC_ROOT / "runtime" / "c-r2" / "page" / "style.css"
REPO_ROOT = SRC_ROOT.parent


def display_path(path: Path) -> str:
    """Repo-relative path for metadata; never bake machine-specific absolutes."""
    try:
        return str(path.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(path)

# ----------------------------------------------------------------- constants

KIND_BLOCK, KIND_FLEX, KIND_TEXT = 0, 1, 2
JUSTIFY = {"flex-start": 0, "start": 0, "normal": 0,
           "center": 1,
           "flex-end": 2, "end": 2,
           "space-between": 3}
ALIGN = {"flex-start": 0, "start": 0, "normal": 0,
         "center": 1,
         "flex-end": 2, "end": 2,
         "stretch": 3}
ACTION = {"select": 1}
DEFAULT_FAMILY = "Segoe UI"
DEFAULT_SIZE = 16.0

_LEN = re.compile(r"(-?(?:\d+\.?\d*|\.\d+))(px)?\s*$")
_PCT = re.compile(r"(-?(?:\d+\.?\d*|\.\d+))%\s*$")


# --------------------------------------------------------------- small utils

def dim(value: str | None) -> tuple[float, float]:
    """CSS length -> (px, pct). auto/none/invalid -> (-1, 0)."""
    if value is None:
        return -1.0, 0.0
    text = value.strip().lower()
    if text in ("", "auto", "none", "normal", "initial", "inherit"):
        return -1.0, 0.0
    match = _PCT.fullmatch(text)
    if match:
        return -1.0, float(match.group(1))
    match = _LEN.fullmatch(text)
    if match:
        return float(match.group(1)), 0.0
    return -1.0, 0.0


def argb(value: str | None) -> int:
    """#rgb / #rrggbb / rgb() / named -> 0xAARRGGBB, or 0 for none."""
    if not value:
        return 0
    color = css_color(value)
    if color is None or color.alpha <= 0:
        return 0
    a = round(color.alpha * 255)
    r = round(color.red * 255)
    g = round(color.green * 255)
    b = round(color.blue * 255)
    return (a << 24) | (r << 16) | (g << 8) | b


def normalize_text(text: str) -> str:
    return " ".join((text or "").split())


def c_float(value: float) -> str:
    if value is None:
        return "-1.0f"
    return f"{float(value):.4f}f".replace("-0.0000f", "0.0000f")


def c_string(value: str | None) -> str:
    if value is None:
        return "NULL"
    out = ['"']
    for ch in value:
        if ch in ('"', "\\"):
            out.append("\\" + ch)
        elif ch == "\n":
            out.append("\\n")
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            out.append(f"\\x{ord(ch):02x}" if ord(ch) < 256 else "?")
    out.append('"')
    return "".join(out)


# --------------------------------------------------------------- text metrics

class GlyphPacker:
    """Deterministic shelf packer for R8 glyph bitmaps with a 1px guard."""

    def __init__(self, size: int = 256) -> None:
        self.size = int(size)
        self.data = bytearray(self.size * self.size)
        self.rects: dict[tuple[str, int, int], tuple[int, int, int, int]] = {}
        self.x = 0
        self.y = 0
        self.row_h = 0

    def add(self, key: tuple[str, int, int], w: int, h: int,
            lines) -> tuple[int, int, int, int]:
        if key in self.rects:
            return self.rects[key]
        pad = 1
        if self.x + w + pad * 2 > self.size:
            self.x = 0
            self.y += self.row_h
            self.row_h = 0
        if self.y + h + pad * 2 > self.size:
            raise RuntimeError(f"glyph atlas overflow at {key}")
        px, py = self.x + pad, self.y + pad
        for row, line in enumerate(lines):
            offset = (py + row) * self.size + px
            self.data[offset:offset + w] = line
        self.rects[key] = (px, py, w, h)
        self.x += w + pad * 2
        self.row_h = max(self.row_h, h + pad * 2)
        return self.rects[key]


class TextMetrics:
    """Build-time text measurement + shaping + atlas baking (never shipped)."""

    def __init__(self) -> None:
        from webnative.text import TextEngine
        self.engine = TextEngine()
        self.packer = GlyphPacker(256)
        self.glyphs: list[dict] = []
        self.runs: list[dict] = []
        self._cache: dict[tuple[str, float, str], tuple[float, float, float]] = {}

    def measure(self, text: str, family: str, size: float) -> tuple[float, float, float]:
        key = (text, float(size), family)
        if key not in self._cache:
            glyphs = self.engine.shape(text, family, size)
            width = sum(g.adv for g in glyphs)
            asc, desc = self.engine.metrics(family, size)
            self._cache[key] = (float(width), float(asc + desc), float(asc))
        return self._cache[key]

    def bake(self, node_id: int, text: str, family: str, size: float) -> None:
        """Shape with HarfBuzz and rasterize the used glyphs into the atlas.

        Positions are stored relative to the text node's top-left, so the
        runtime only adds the final layout rect. Baseline is taken from the
        same FreeType metrics that produced the line box height.
        """
        face = self.engine.face(family)
        asc, _desc = face.metrics(size)
        shaped = face.shape(text, size)
        first = len(self.glyphs)
        pen = 0.0
        for item in shaped:
            bitmap = face.raster(item.gid, size)
            if bitmap.w > 0 and bitmap.h > 0:
                key = (str(face.path), item.gid, int(round(size)))
                x, y, w, h = self.packer.add(key, bitmap.w, bitmap.h, bitmap.lines)
                self.glyphs.append({
                    "x": pen + item.dx + bitmap.left,
                    "y": asc - item.dy - bitmap.top,
                    "w": float(bitmap.w),
                    "h": float(bitmap.h),
                    "u0": x / self.packer.size,
                    "v0": y / self.packer.size,
                    "u1": (x + w) / self.packer.size,
                    "v1": (y + h) / self.packer.size,
                })
            pen += item.adv
        self.runs.append({
            "node": node_id,
            "first": first,
            "count": len(self.glyphs) - first,
            "baseline": float(asc),
        })


# ------------------------------------------------------------------ emitter

class Emitter:
    def __init__(self, root: Node, styles: dict[Node, object], metrics: TextMetrics | None):
        self.root = root
        self.styles = styles
        self.metrics = metrics
        self.nodes: list[dict] = []
        self.meta: list[dict] = []
        self.name_counts: dict[str, int] = {}

    # -- naming ------------------------------------------------------------
    def _base_name(self, node: Node) -> str:
        base = node.tag
        if node.attrs.get("id"):
            base += "#" + node.attrs["id"]
        classes = (node.attrs.get("class") or "").split()
        if classes:
            base += "." + classes[0]
        return base

    def _unique_name(self, base: str) -> str:
        count = self.name_counts.get(base, 0)
        self.name_counts[base] = count + 1
        return base if count == 0 else f"{base}[{count}]"

    # -- emit --------------------------------------------------------------
    def run(self) -> None:
        body = next(node for node in self.root.walk() if node.tag == "body")
        body_style = self.styles[body]
        if body_style.display not in ("block", "flex"):
            raise SystemExit("body is not renderable")
        self._emit_element(body, -1, "body.root", is_root=True)

    def _emit_element(self, node: Node, parent: int, name: str, is_root: bool = False) -> int:
        style = self.styles[node]
        raw = style.raw
        width, width_pct = dim(raw.get("width"))
        height, height_pct = dim(raw.get("height"))
        # Root special case: % sizes resolve against the viewport. Keep them
        # as percentages; the runtime knows the containing block is the window.
        if is_root:
            if width_pct == 0 and width < 0:
                width, width_pct = -1.0, 100.0
            if height_pct == 0 and height < 0:
                height, height_pct = -1.0, 100.0

        wrap_raw = (raw.get("flex-wrap") or "").strip().lower()
        wrap = 1 if wrap_raw in ("wrap", "wrap-reverse") else 0
        action_raw = (node.attrs.get("data-action") or "").strip().lower()
        index = len(self.nodes)
        self.nodes.append({
            "parent": parent,
            "kind": KIND_FLEX if style.display == "flex" else KIND_BLOCK,
            "direction": 1 if style.flex_direction == "column" else 0,
            "justify": JUSTIFY.get(style.justify_content, 0),
            "align": ALIGN.get(style.align_items, 3),
            "wrap": wrap,
            "width": width,
            "height": height,
            "width_pct": width_pct,
            "height_pct": height_pct,
            "margin": [float(v) for v in style.margin],
            "padding": [float(v) for v in style.padding],
            "background": argb(style.background),
            "text": None,
            "hover_bg": argb(node.attrs.get("data-hover")),
            "press_bg": argb(node.attrs.get("data-press")),
            "selected_bg": argb(node.attrs.get("data-selected")),
            "action": ACTION.get(action_raw, 0),
            "gap": float(style.gap or 0.0),
            "font_size": float(style.font_size or DEFAULT_SIZE),
            "text_color": argb(style.color) or 0xFF000000,
        })
        self.meta.append({
            "id": index, "name": name, "tag": node.tag,
            "classes": (node.attrs.get("class") or "").split(),
            "element_id": node.attrs.get("id"),
            "kind": "flex" if style.display == "flex" else "block",
            "direction": "column" if style.flex_direction == "column" else "row",
            "wrap": bool(wrap),
            "background": f"#{style.background[1:]}" if style.background else None,
            "interactive": bool(action_raw),
            "text": None,
        })

        # Own text first, exactly like exp09's "texto propio antes de hijos".
        text = normalize_text(node.text)
        if text:
            self._emit_text(node, style, index, name, text)

        child_index = 0
        for child in node.children:
            child_style = self.styles.get(child)
            if child_style is None or child_style.display not in ("block", "flex"):
                continue
            base = self._base_name(child)
            child_name = self._unique_name(base)
            self._emit_element(child, index, f"{name}/{child_name}")
            child_index += 1
        return index

    def _emit_text(self, node: Node, style, parent: int, parent_name: str, text: str) -> None:
        family = style.font_family or DEFAULT_FAMILY
        size = float(style.font_size or DEFAULT_SIZE)
        width = height = 0.0
        if self.metrics is not None:
            width, height, _asc = self.metrics.measure(text, family, size)
        if width <= 0:
            width = 0.55 * size * len(text)
        if height <= 0:
            height = size * 1.25
        index = len(self.nodes)
        self.nodes.append({
            "parent": parent,
            "kind": KIND_TEXT,
            "direction": 0,
            "justify": 0, "align": 0, "wrap": 0,
            "width": float(math.ceil(width)),
            "height": float(math.ceil(height)),
            "width_pct": 0.0, "height_pct": 0.0,
            "margin": [0.0, 0.0, 0.0, 0.0],
            "padding": [0.0, 0.0, 0.0, 0.0],
            "background": 0,
            "text": text,
            "hover_bg": 0, "press_bg": 0, "selected_bg": 0,
            "action": 0,
            "gap": 0.0,
            "font_size": size,
            "text_color": argb(style.color) or 0xFF000000,
        })
        if self.metrics is not None:
            self.metrics.bake(index, text, family, size)
        text_name = self._unique_name("text")
        self.meta.append({
            "id": index, "name": f"{parent_name}/{text_name}", "tag": "#text",
            "classes": [], "element_id": None, "kind": "text",
            "direction": None, "wrap": False, "background": None,
            "interactive": False, "text": text,
        })


# ------------------------------------------------------------------- output

def write_header(nodes: list[dict], meta: list[dict], out: Path,
                 default_w: int, default_h: int, metrics: TextMetrics | None) -> None:
    lines: list[str] = []
    lines.append("/* Generated by emit_ui.py. Do not edit. */")
    lines.append("#ifndef UI_DATA_H")
    lines.append("#define UI_DATA_H")
    lines.append("")
    lines.append("typedef enum { R2_NODE_BLOCK, R2_NODE_FLEX, R2_NODE_TEXT } NodeKind;")
    lines.append("typedef enum { JUSTIFY_START, JUSTIFY_CENTER, JUSTIFY_END, JUSTIFY_SPACE_BETWEEN } Justify;")
    lines.append("typedef enum { ALIGN_START, ALIGN_CENTER, ALIGN_END, ALIGN_STRETCH } Align;")
    lines.append("typedef enum { ACTION_NONE = 0, ACTION_SELECT = 1 } Action;")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    int parent;                 /* -1 root */")
    lines.append("    int kind;")
    lines.append("    int direction;              /* 0 row, 1 column */")
    lines.append("    int justify, align, wrap;")
    lines.append("    float width, height;        /* px, -1 auto */")
    lines.append("    float width_pct, height_pct;/* 0 none, resolved at runtime */")
    lines.append("    float margin[4], padding[4];/* top,right,bottom,left */")
    lines.append("    unsigned int background;    /* 0xAARRGGBB, 0 none */")
    lines.append("    const char *text;           /* NODE_TEXT only */")
    lines.append("    unsigned int hover_bg, press_bg, selected_bg;")
    lines.append("    int action;")
    lines.append("    /* R2 extensions (documented in README) */")
    lines.append("    float gap;                  /* main-axis gap, px */")
    lines.append("    float font_size;            /* text nodes, build-time metric */")
    lines.append("    unsigned int text_color;    /* text nodes, 0xAARRGGBB */")
    lines.append("} UiNode;")
    lines.append("")
    lines.append(f"#define UI_NODE_COUNT {len(nodes)}")
    lines.append(f"#define UI_VIEWPORT_WIDTH {default_w}")
    lines.append(f"#define UI_VIEWPORT_HEIGHT {default_h}")
    lines.append("")
    lines.append("static const UiNode kUiNodes[UI_NODE_COUNT] = {")
    for node in nodes:
        margin = ", ".join(c_float(v) for v in node["margin"])
        padding = ", ".join(c_float(v) for v in node["padding"])
        lines.append(
            "    { %d, %d, %d, %d, %d, %d, %s, %s, %s, %s, "
            "{ %s }, { %s }, 0x%08Xu, %s, 0x%08Xu, 0x%08Xu, 0x%08Xu, %d, %s, %s, 0x%08Xu },"
            % (node["parent"], node["kind"], node["direction"], node["justify"],
               node["align"], node["wrap"],
               c_float(node["width"]), c_float(node["height"]),
               c_float(node["width_pct"]), c_float(node["height_pct"]),
               margin, padding, node["background"], c_string(node["text"]),
               node["hover_bg"], node["press_bg"], node["selected_bg"],
               node["action"], c_float(node["gap"]), c_float(node["font_size"]),
               node["text_color"]))
    lines.append("};")
    lines.append("")
    lines.append("static const char *const kUiNodeNames[UI_NODE_COUNT] = {")
    for item in meta:
        lines.append(f"    {c_string(item['name'])},")
    lines.append("};")
    lines.append("")

    glyphs = metrics.glyphs if metrics is not None else []
    runs = metrics.runs if metrics is not None else []
    # With no glyphs, emit a 1x1 atlas so the runtime upload stays valid.
    atlas_size = metrics.packer.size if (metrics is not None and glyphs) else 1

    lines.append("/* Pre-shaped text (HarfBuzz positions, FreeType R8 bitmaps). */")
    lines.append(f"#define UI_ATLAS_SIZE {atlas_size}")
    lines.append(f"#define UI_GLYPH_COUNT {len(glyphs)}")
    lines.append(f"#define UI_TEXT_RUN_COUNT {len(runs)}")
    lines.append("")
    if metrics is not None and glyphs:
        lines.append(f"static const unsigned char kUiAtlas[UI_ATLAS_SIZE * UI_ATLAS_SIZE] = {{")
        data = metrics.packer.data
        for offset in range(0, len(data), 24):
            chunk = data[offset:offset + 24]
            lines.append("    " + ", ".join(f"{value:3d}" for value in chunk) + ",")
        lines.append("};")
    else:
        lines.append("static const unsigned char kUiAtlas[1] = {0};")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    float x, y, w, h;          /* relative to the text node top-left */")
    lines.append("    float u0, v0, u1, v1;      /* R8 atlas UVs */")
    lines.append("} UiGlyph;")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    int node;                  /* UiNode id (NODE_TEXT) */")
    lines.append("    int first;                 /* first glyph in kUiGlyphs */")
    lines.append("    int count;")
    lines.append("    float baseline;            /* ascender inside the line box */")
    lines.append("} UiTextRun;")
    lines.append("")
    lines.append("static const UiGlyph kUiGlyphs[UI_GLYPH_COUNT > 0 ? UI_GLYPH_COUNT : 1] = {")
    if glyphs:
        for glyph in glyphs:
            lines.append(
                "    { %s, %s, %s, %s, %s, %s, %s, %s },"
                % tuple(c_float(glyph[key]) for key in
                        ("x", "y", "w", "h", "u0", "v0", "u1", "v1")))
    else:
        lines.append("    { 0, 0, 0, 0, 0, 0, 0, 0 },")
    lines.append("};")
    lines.append("")
    lines.append("static const UiTextRun kUiTextRuns[UI_TEXT_RUN_COUNT > 0 ? UI_TEXT_RUN_COUNT : 1] = {")
    if runs:
        for run in runs:
            lines.append("    { %d, %d, %d, %s }," % (
                run["node"], run["first"], run["count"], c_float(run["baseline"])))
    else:
        lines.append("    { -1, 0, 0, 0.0f },")
    lines.append("};")
    lines.append("")
    lines.append("#endif /* UI_DATA_H */")
    lines.append("")
    out.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--html", default=str(DEFAULT_HTML))
    parser.add_argument("--css", default=str(DEFAULT_CSS))
    parser.add_argument("--out", default="ui_data.h")
    parser.add_argument("--meta", default="ui_meta.json")
    parser.add_argument("--width", type=int, default=960)
    parser.add_argument("--height", type=int, default=640)
    parser.add_argument("--no-text-metrics", action="store_true",
                        help="skip HarfBuzz, use a fixed 0.55em estimate")
    args = parser.parse_args()

    html_path = Path(args.html)
    css_path = Path(args.css)
    root = parse_html_file(html_path)
    texts = [css_path.read_text(encoding="utf-8")]
    # Also honour <style> / <link> order if the fixture grows.
    texts = stylesheet_texts(root, html_path.parent) or texts
    rules = parse_stylesheets(texts)
    styles = compute_styles(root, rules)

    metrics = None if args.no_text_metrics else TextMetrics()
    emitter = Emitter(root, styles, metrics)
    emitter.run()
    write_header(emitter.nodes, emitter.meta, Path(args.out), args.width, args.height, metrics)

    meta = {
        "source": {"html": display_path(html_path), "css": display_path(css_path)},
        "default_viewport": [args.width, args.height],
        "node_count": len(emitter.nodes),
        "rule_count": len(rules),
        "text": {
            "atlas_size": metrics.packer.size if metrics is not None else 0,
            "glyph_count": len(metrics.glyphs) if metrics is not None else 0,
            "text_run_count": len(metrics.runs) if metrics is not None else 0,
        },
        "nodes": emitter.meta,
    }
    Path(args.meta).write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n",
                               encoding="utf-8")
    print(f"emit_ui: {len(emitter.nodes)} nodes, {len(rules)} rules -> {args.out}, {args.meta}")
    for item in emitter.meta:
        node = emitter.nodes[item["id"]]
        print("  [%2d] %-42s kind=%-5s size=%sx%s%s"
              % (item["id"], item["name"], item["kind"], node["width"], node["height"],
                 (" text=" + repr(node["text"])) if node["text"] else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
