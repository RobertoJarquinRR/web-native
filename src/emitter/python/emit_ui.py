#!/usr/bin/env python3
"""Build-time emitter for the R2 style-tree runtime (`src/runtime/c-r2`).

Two input modes, one output contract:

1. **Python tree (oracle)** — `--html/--css`: parses HTML+CSS with the
   canonical Python frontend (`src/frontend/python/webnative`), computes
   styles with cssselect2 and flattens the styled tree to a pre-order UiNode
   array. HarfBuzz/FreeType shape and raster the text and pack the atlas.
2. **Rust tree** — `--ui-json <ui.json>`: consumes the pre-layout style tree
   emitted by `src/frontend/rust --emit-ui` (plus its `atlas.bin`) and emits
   the same `ui_data.h` / `ui_meta.json`. This mode does not import
   `webnative`; only the color parser (tinycss2) is shared.

`--compare A.h B.h` parses two generated headers and reports semantic
differences (nodes, styles, glyphs, runs, atlas dimensions/bytes).

R2-specific data merged here (the oracle's ComputedStyle deliberately drops
it from its display-list IR):
  - percentage width/height (width_pct / height_pct)
  - flex-wrap
  - data-hover / data-press / data-selected / data-action from HTML attributes
  - text line boxes measured with HarfBuzz (build-time only; the runtime never
    links a font engine)

Usage:
  .venv\\Scripts\\python.exe emit_ui.py
  .venv\\Scripts\\python.exe emit_ui.py --html page/index.html --css page/style.css \
      --out ui_data.h --meta ui_meta.json
  .venv\\Scripts\\python.exe emit_ui.py --ui-json build/ui.json \
      --out build/ui_data.h --meta build/ui_meta.json
  .venv\\Scripts\\python.exe emit_ui.py --compare oracle/ui_data.h build/ui_data.h
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

KIND_BLOCK, KIND_FLEX, KIND_TEXT, KIND_GRID, KIND_IMAGE, KIND_INPUT = 0, 1, 2, 3, 4, 5
JUSTIFY = {"flex-start": 0, "start": 0, "normal": 0,
           "center": 1,
           "flex-end": 2, "end": 2,
           "space-between": 3}
ALIGN = {"flex-start": 0, "start": 0, "normal": 0,
         "center": 1,
         "flex-end": 2, "end": 2,
         "stretch": 3}
ACTION = {"select": 1}
GRID_NONE, GRID_AUTO_FILL = 0, 1
OVERFLOW = {"visible": 0, "hidden": 1, "auto": 2, "scroll": 2}
OBJECT_FIT = {"fill": 0, "cover": 1, "contain": 2}
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
        code = ord(ch)
        if ch in ('"', "\\"):
            out.append("\\" + ch)
        elif ch == "\n":
            out.append("\\n")
        elif 32 <= code < 127:
            out.append(ch)
        else:
            # UTF-8 bytes as three-digit octal escapes: unlike \xNN they can
            # never absorb a following hex digit ("\xd72160" bug).
            for byte in ch.encode("utf-8"):
                out.append(f"\\{byte:03o}")
    out.append('"')
    return "".join(out)


# ------------------------------------------------------- v2 style (grid/image)

def _as_float(value) -> float | None:
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        px, pct = dim(value)
        if px >= 0.0:
            return px
        match = _LEN.fullmatch(value.strip().lower())
        if match:
            return float(match.group(1))
    return None


def grid_columns(value) -> tuple[int, float]:
    """grid-template-columns -> (mode, size_px). Only auto-fill in v2."""
    if value is None:
        return GRID_NONE, 0.0
    if isinstance(value, dict):
        mode = str(value.get("mode", "")).strip().lower()
        size = _as_float(value.get("size_px"))
        if mode == "auto_fill" and size is not None and size > 0:
            return GRID_AUTO_FILL, float(size)
        return GRID_NONE, 0.0
    text = str(value).strip().lower()
    match = re.fullmatch(r"repeat\(\s*auto-fill\s*,\s*([0-9.]+)px\s*\)", text)
    if match:
        return GRID_AUTO_FILL, float(match.group(1))
    return GRID_NONE, 0.0


def auto_rows(value) -> float:
    """grid-auto-rows -> px or -1 for auto. Accepts 320, "320px" or {"px":320}."""
    if value is None:
        return -1.0
    if isinstance(value, dict):
        number = _as_float(value.get("px"))
        return -1.0 if number is None else max(0.0, number)
    if isinstance(value, str):
        text = value.strip().lower()
        if text in ("", "auto"):
            return -1.0
        number = _as_float(text)
        return -1.0 if number is None else number
    number = _as_float(value)
    return -1.0 if number is None else number


def overflow_value(value) -> int:
    if value is None:
        return 0
    return OVERFLOW.get(str(value).strip().lower(), 0)


def object_fit_value(value) -> int:
    if value is None:
        return 0
    return OBJECT_FIT.get(str(value).strip().lower(), 0)


def border_width_value(style: dict) -> float:
    """Uniform border width in px. `border_style:"none"` forces 0.

    Relative units (em/rem/vh/vw) live in their own Rust fields; v1 only
    consumes the resolved px value, so those declarations emit 0.
    """
    if str(style.get("border_style") or "").strip().lower() == "none":
        return 0.0
    width = _as_float(style.get("border_width_px"))
    return 0.0 if width is None else max(0.0, float(width))


def first_box_shadow(style: dict) -> dict:
    """First non-inset `box-shadow`, or {} when the node has none."""
    for shadow in style.get("box_shadow") or []:
        if isinstance(shadow, dict) and not shadow.get("inset"):
            return shadow
    return {}


def css_node_fields(style: dict) -> dict:
    """CSS core v1 fields consumed by R2 (rounded bg, border, first shadow)."""
    shadow = first_box_shadow(style)
    return {
        "border_width": border_width_value(style),
        "border_color": argb(style.get("border_color")),
        "border_radius": max(0.0, _as_float(style.get("border_radius_px")) or 0.0),
        "shadow_x": _as_float(shadow.get("offset_x_px")) or 0.0,
        "shadow_y": _as_float(shadow.get("offset_y_px")) or 0.0,
        "shadow_blur": max(0.0, _as_float(shadow.get("blur_px")) or 0.0),
        "shadow_spread": _as_float(shadow.get("spread_px")) or 0.0,
        "shadow_color": argb(shadow.get("color")),
    }


ZERO_CSS_FIELDS = {
    "border_width": 0.0,
    "border_color": 0,
    "border_radius": 0.0,
    "shadow_x": 0.0,
    "shadow_y": 0.0,
    "shadow_blur": 0.0,
    "shadow_spread": 0.0,
    "shadow_color": 0,
}


def _png_size(data: bytes) -> tuple[int, int] | None:
    if len(data) >= 24 and data[:8] == b"\x89PNG\r\n\x1a\n" and data[12:16] == b"IHDR":
        width = int.from_bytes(data[16:20], "big")
        height = int.from_bytes(data[20:24], "big")
        if width > 0 and height > 0:
            return width, height
    return None


def _jpeg_size(data: bytes) -> tuple[int, int] | None:
    if len(data) < 4 or data[0:2] != b"\xff\xd8":
        return None
    index = 2
    while index + 9 < len(data):
        if data[index] != 0xFF:
            index += 1
            continue
        marker = data[index + 1]
        index += 2
        while index < len(data) and data[index] == 0xFF:
            index += 1
        if index >= len(data) or marker in (0xD8, 0xD9):
            continue
        length = int.from_bytes(data[index:index + 2], "big")
        if length < 2:
            return None
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            if index + 7 > len(data):
                return None
            height = int.from_bytes(data[index + 3:index + 5], "big")
            width = int.from_bytes(data[index + 5:index + 7], "big")
            if width > 0 and height > 0:
                return width, height
            return None
        index += length
    return None


def image_size(path_text: str | None, base_dir: Path | None) -> tuple[float, float]:
    """PNG/JPEG intrinsic size resolved at build time (0,0 if unknown)."""
    if not path_text or base_dir is None:
        return 0.0, 0.0
    path = Path(path_text)
    if not path.is_absolute():
        path = base_dir / path
    try:
        data = path.read_bytes()[:65536]
    except OSError:
        return 0.0, 0.0
    size = _png_size(data) or _jpeg_size(data)
    if size is None:
        return 0.0, 0.0
    return float(size[0]), float(size[1])


def first_gap(raw: dict, specific: str, fallback: float) -> float:
    value = _as_float(raw.get(specific))
    if value is None:
        value = _as_float((raw.get("gap") or None))
    if value is None:
        value = fallback
    return max(0.0, float(value))



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
    def __init__(self, root: Node, styles: dict[Node, object], metrics: TextMetrics | None,
                 base_dir: Path | None = None):
        self.root = root
        self.styles = styles
        self.metrics = metrics
        self.base_dir = base_dir
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
        if any(node.tag == "input" for node in self.root.walk()):
            raise SystemExit(
                "input requires --ui-json (frontend-rust --emit-ui --pilot): "
                "the Python oracle tree does not bake the ASCII input pool")
        body_style = self.styles[body]
        if body_style.display not in ("block", "flex", "grid"):
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
        if node.tag == "img":
            kind = KIND_IMAGE
        elif style.display == "grid":
            kind = KIND_GRID
        elif style.display == "flex":
            kind = KIND_FLEX
        else:
            kind = KIND_BLOCK
        grid_mode, grid_column_px = grid_columns(raw.get("grid-template-columns"))
        grid_rows = auto_rows(raw.get("grid-auto-rows"))
        overflow = overflow_value(raw.get("overflow"))
        computed_gap = float(style.gap or 0.0)
        row_gap = first_gap(raw, "row-gap", computed_gap)
        column_gap = first_gap(raw, "column-gap", computed_gap)
        image_src = node.attrs.get("src") if kind == KIND_IMAGE else None
        image_alt = node.attrs.get("alt") if kind == KIND_IMAGE else None
        object_fit = object_fit_value(raw.get("object-fit"))
        image_w, image_h = image_size(image_src, self.base_dir)
        index = len(self.nodes)
        self.nodes.append({
            "parent": parent,
            "kind": kind,
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
            "gap": computed_gap,
            "font_size": float(style.font_size or DEFAULT_SIZE),
            "text_color": argb(style.color) or 0xFF000000,
            "grid_mode": grid_mode,
            "grid_column_px": grid_column_px,
            "grid_auto_rows": grid_rows,
            "row_gap": row_gap,
            "column_gap": column_gap,
            "overflow": overflow,
            "image_width": image_w,
            "image_height": image_h,
            "image_src": image_src,
            "image_alt": image_alt,
            "object_fit": object_fit,
            # El oraculo Python no modela CSS v3 (bordes/sombras): el arbol
            # Rust (--ui-json) es la unica fuente de estos campos en v1.
            **ZERO_CSS_FIELDS,
            "flex_grow": 0.0,
            "flex_shrink": 1.0,
            "flex_basis": -1.0,
            "box_sizing": 1 if str(style.raw.get("box-sizing") or "").strip().lower()
            == "border-box" else 0,
        })
        self.meta.append({
            "id": index, "name": name, "tag": node.tag,
            "classes": (node.attrs.get("class") or "").split(),
            "element_id": node.attrs.get("id"),
            "kind": "image" if kind == KIND_IMAGE else (
                "grid" if kind == KIND_GRID else ("flex" if kind == KIND_FLEX else "block")),
            "direction": "column" if style.flex_direction == "column" else "row",
            "wrap": bool(wrap),
            "background": f"#{style.background[1:]}" if style.background else None,
            "interactive": bool(action_raw),
            "text": None,
            "grid": {
                "mode": "auto_fill" if grid_mode == GRID_AUTO_FILL else None,
                "column_px": grid_column_px,
                "row_px": grid_rows,
            },
            "overflow": overflow,
            "image": {
                "src": image_src,
                "alt": image_alt,
                "object_fit": object_fit,
                "width": image_w,
                "height": image_h,
            } if kind == KIND_IMAGE else None,
        })

        # Own text first, exactly like exp09's "texto propio antes de hijos".
        text = normalize_text(node.text)
        if text:
            self._emit_text(node, style, index, name, text)

        child_index = 0
        for child in node.children:
            child_style = self.styles.get(child)
            if child_style is None or (child.tag != "img" and
                                       child_style.display not in ("block", "flex", "grid")):
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
            "grid_mode": GRID_NONE,
            "grid_column_px": 0.0,
            "grid_auto_rows": -1.0,
            "row_gap": 0.0,
            "column_gap": 0.0,
            "overflow": 0,
            "image_width": 0.0,
            "image_height": 0.0,
            "image_src": None,
            "image_alt": None,
            "object_fit": 0,
            **ZERO_CSS_FIELDS,
            "flex_grow": 0.0,
            "flex_shrink": 1.0,
            "flex_basis": -1.0,
            "box_sizing": 1 if str(style.raw.get("box-sizing") or "").strip().lower()
            == "border-box" else 0,
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
                 default_w: int, default_h: int, metrics: TextMetrics | None,
                 inputs: list[dict] | None = None,
                 input_glyphs: list[dict] | None = None) -> None:
    inputs = inputs or []
    input_glyphs = input_glyphs or []
    # E1: los campos de text-align/line-height/margin:auto solo se escriben si
    # alguna pagina los usa. Los headers existentes quedan byte a byte iguales.
    extras = any(int(node.get("text_align") or 0) != 0
                 or float(node.get("line_height") or 0.0) > 0.0
                 or int(node.get("margin_auto") or 0) != 0
                 or int(node.get("box_sizing") or 0) != 0
                 or float(node.get("flex_grow") or 0.0) != 0.0
                 or float(node.get("flex_shrink") or 1.0) != 1.0
                 or float(node.get("flex_basis") or -1.0) >= 0.0 for node in nodes)
    lines: list[str] = []
    lines.append("/* Generated by emit_ui.py. Do not edit. */")
    lines.append("#ifndef UI_DATA_H")
    lines.append("#define UI_DATA_H")
    lines.append("")
    node_kinds = "R2_NODE_BLOCK, R2_NODE_FLEX, R2_NODE_TEXT, R2_NODE_GRID, R2_NODE_IMAGE"
    if inputs:
        # ui-tree/3: aditivo, solo presente en paginas con <input>.
        node_kinds += ", R2_NODE_INPUT"
    lines.append(f"typedef enum {{ {node_kinds} }} NodeKind;")
    lines.append("typedef enum { JUSTIFY_START, JUSTIFY_CENTER, JUSTIFY_END, JUSTIFY_SPACE_BETWEEN } Justify;")
    lines.append("typedef enum { ALIGN_START, ALIGN_CENTER, ALIGN_END, ALIGN_STRETCH } Align;")
    lines.append("typedef enum { ACTION_NONE = 0, ACTION_SELECT = 1 } Action;")
    lines.append("typedef enum { R2_GRID_NONE = 0, R2_GRID_AUTO_FILL = 1 } GridMode;")
    lines.append("typedef enum { R2_OVERFLOW_VISIBLE = 0, R2_OVERFLOW_HIDDEN = 1, R2_OVERFLOW_SCROLL = 2 } OverflowMode;")
    lines.append("typedef enum { R2_OBJECT_FIT_FILL = 0, R2_OBJECT_FIT_COVER = 1, R2_OBJECT_FIT_CONTAIN = 2 } ObjectFit;")
    if extras:
        lines.append("")
        lines.append("#define UI_STYLE_EXTRAS 1")
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
    lines.append("    /* ui-tree/2: grid, scroll and images */")
    lines.append("    int grid_mode;              /* R2_GRID_* */")
    lines.append("    float grid_column_px;       /* repeat(auto-fill, <px>) */")
    lines.append("    float grid_auto_rows;       /* px, -1 auto */")
    lines.append("    float row_gap, column_gap;  /* resolved gaps, px */")
    lines.append("    int overflow;               /* R2_OVERFLOW_* */")
    lines.append("    float image_width, image_height; /* intrinsic px, 0 unknown */")
    lines.append("    const char *image_src;      /* local path, NODE_IMAGE only */")
    lines.append("    const char *image_alt;")
    lines.append("    int object_fit;             /* R2_OBJECT_FIT_* */")
    lines.append("    /* CSS core v1: rounded background, uniform border, first shadow. */")
    lines.append("    float border_width;         /* px, 0 none */")
    lines.append("    unsigned int border_color;  /* 0xAARRGGBB, 0 none */")
    lines.append("    float border_radius;        /* uniform px, 0 square */")
    lines.append("    float shadow_x, shadow_y;   /* first non-inset shadow offset, px */")
    lines.append("    float shadow_blur, shadow_spread;")
    lines.append("    unsigned int shadow_color;  /* 0xAARRGGBB, 0 none */")
    if extras:
        lines.append("    /* E1: text alignment, line box, auto margins and box model (only when used). */")
        lines.append("    int text_align;             /* 0 left, 1 center, 2 right */")
        lines.append("    float line_height;          /* px, 0 = font metrics */")
        lines.append("    int margin_auto;            /* bit0 top, 1 right, 2 bottom, 3 left */")
        lines.append("    int box_sizing;             /* 0 content-box (CSS default), 1 border-box */")
        lines.append("    float flex_grow;            /* >= 0 */")
        lines.append("    float flex_shrink;          /* >= 0, default 1 */")
        lines.append("    float flex_basis;           /* px, -1 = auto */")
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
        format_text = (
            "    { %d, %d, %d, %d, %d, %d, %s, %s, %s, %s, "
            "{ %s }, { %s }, 0x%08Xu, %s, 0x%08Xu, 0x%08Xu, 0x%08Xu, %d, %s, %s, 0x%08Xu, "
            "%d, %s, %s, %s, %s, %d, %s, %s, %s, %s, %d, "
            "%s, 0x%08Xu, %s, %s, %s, %s, %s, 0x%08Xu"
        )
        values = [
            node["parent"], node["kind"], node["direction"], node["justify"],
            node["align"], node["wrap"],
            c_float(node["width"]), c_float(node["height"]),
            c_float(node["width_pct"]), c_float(node["height_pct"]),
            margin, padding, node["background"], c_string(node["text"]),
            node["hover_bg"], node["press_bg"], node["selected_bg"],
            node["action"], c_float(node["gap"]), c_float(node["font_size"]),
            node["text_color"],
            node["grid_mode"], c_float(node["grid_column_px"]),
            c_float(node["grid_auto_rows"]), c_float(node["row_gap"]),
            c_float(node["column_gap"]), node["overflow"],
            c_float(node["image_width"]), c_float(node["image_height"]),
            c_string(node["image_src"]), c_string(node["image_alt"]),
            node["object_fit"],
            c_float(node["border_width"]), node["border_color"],
            c_float(node["border_radius"]),
            c_float(node["shadow_x"]), c_float(node["shadow_y"]),
            c_float(node["shadow_blur"]), c_float(node["shadow_spread"]),
            node["shadow_color"],
        ]
        if extras:
            format_text += ", %d, %s, %d, %d, %s, %s, %s"
            values += [
                int(node.get("text_align") or 0),
                c_float(float(node.get("line_height") or 0.0)),
                int(node.get("margin_auto") or 0),
                int(node.get("box_sizing") or 0),
                c_float(float(node.get("flex_grow") or 0.0)),
                c_float(float(node.get("flex_shrink") or 1.0)),
                c_float(float(node.get("flex_basis") if node.get("flex_basis") is not None else -1.0)),
            ]
        format_text += " },"
        lines.append(format_text % tuple(values))
    lines.append("};")
    lines.append("")
    lines.append("static const char *const kUiNodeNames[UI_NODE_COUNT] = {")
    for item in meta:
        lines.append(f"    {c_string(item['name'])},")
    lines.append("};")
    lines.append("")

    if inputs:
        # ui-tree/3: piscina ASCII imprimible por tamano de fuente. El runtime
        # compone texto sumando avances; no hay shaping en runtime.
        lines.append("typedef struct {")
        lines.append("    int cp;                    /* ASCII imprimible 32..126 */")
        lines.append("    int gid;                   /* diagnostico */")
        lines.append("    float advance;             /* avance HarfBuzz aislado, px */")
        lines.append("    float x, y;                /* caja relativa al content box */")
        lines.append("    float w, h;")
        lines.append("    float u0, v0, u1, v1;      /* R8 atlas UVs */")
        lines.append("} UiInputGlyph;")
        lines.append("")
        lines.append("typedef struct {")
        lines.append("    int node;                  /* UiNode id (NODE_INPUT) */")
        lines.append("    const char *value;         /* valor inicial, ASCII */")
        lines.append("    const char *placeholder;   /* ASCII, vacio = sin placeholder */")
        lines.append("    int max_length;            /* 0 = sin limite (tope 255) */")
        lines.append("    int password;              /* reservado, 0 en v1 */")
        lines.append("    float baseline;            /* ascender desde el content box */")
        lines.append("    float line_height;         /* ascender + descender */")
        lines.append("    int glyph_first;           /* primer glyph en kUiInputGlyphs */")
        lines.append("    int glyph_count;")
        lines.append("} UiInput;")
        lines.append("")
        lines.append(f"#define UI_INPUT_COUNT {len(inputs)}")
        lines.append(f"#define UI_INPUT_GLYPH_COUNT {len(input_glyphs)}")
        lines.append("")
        lines.append("static const UiInputGlyph kUiInputGlyphs[UI_INPUT_GLYPH_COUNT > 0 ? UI_INPUT_GLYPH_COUNT : 1] = {")
        if input_glyphs:
            for glyph in input_glyphs:
                lines.append(
                    "    { %d, %d, %s, %s, %s, %s, %s, %s, %s, %s, %s },"
                    % (glyph["cp"], glyph["gid"],
                       c_float(glyph["advance"]),
                       c_float(glyph["x"]), c_float(glyph["y"]),
                       c_float(glyph["w"]), c_float(glyph["h"]),
                       c_float(glyph["u0"]), c_float(glyph["v0"]),
                       c_float(glyph["u1"]), c_float(glyph["v1"])))
        else:
            lines.append("    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },")
        lines.append("};")
        lines.append("")
        lines.append("static const UiInput kUiInputs[UI_INPUT_COUNT > 0 ? UI_INPUT_COUNT : 1] = {")
        for item in inputs:
            lines.append("    { %d, %s, %s, %d, %d, %s, %s, %d, %d }," % (
                item["node"], c_string(item["value"]), c_string(item["placeholder"]),
                item["max_length"], item["password"],
                c_float(item["baseline"]), c_float(item["line_height"]),
                item["glyph_first"], item["glyph_count"]))
        lines.append("};")
        lines.append("")

    glyphs = metrics.glyphs if metrics is not None else []
    runs = metrics.runs if metrics is not None else []
    # With no glyphs, emit a 1x1 atlas so the runtime upload stays valid. La
    # piscina de un input tambien cuenta: una pagina solo-input necesita atlas.
    has_atlas = metrics is not None and bool(glyphs or input_glyphs)
    atlas_size = metrics.packer.size if has_atlas else 1

    lines.append("/* Pre-shaped text (HarfBuzz positions, FreeType R8 bitmaps). */")
    lines.append(f"#define UI_ATLAS_SIZE {atlas_size}")
    lines.append(f"#define UI_GLYPH_COUNT {len(glyphs)}")
    lines.append(f"#define UI_TEXT_RUN_COUNT {len(runs)}")
    lines.append("")
    if has_atlas:
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


# ----------------------------------------------------------- Rust ui.json

class AtlasShim:
    """`GlyphPacker`-compatible view over the Rust `atlas.bin` bytes."""

    def __init__(self, size: int, data: bytes) -> None:
        self.size = int(size)
        self.data = bytearray(data)


class UiJsonMetrics:
    def __init__(self, size: int, data: bytes, glyphs: list[dict], runs: list[dict]) -> None:
        self.packer = AtlasShim(size, data)
        self.glyphs = glyphs
        self.runs = runs


def load_ui_json(path: Path) -> tuple[dict, bytes | None, int]:
    data = json.loads(path.read_text(encoding="utf-8"))
    atlas = data.get("atlas")
    if not atlas:
        return data, None, 0
    wide = int(atlas.get("width", 256))
    high = int(atlas.get("height", wide))
    name = atlas.get("file") or "atlas.bin"
    atlas_path = Path(name)
    if not atlas_path.is_absolute():
        atlas_path = path.parent / atlas_path
    payload = atlas_path.read_bytes()
    if wide != high:
        raise SystemExit(
            f"atlas {atlas_path}: {wide}x{high} is not square (R2 contract)")
    if len(payload) != wide * high:
        raise SystemExit(
            f"atlas {atlas_path}: {len(payload)} bytes, expected {wide * high}")
    return data, payload, wide


def validate_ui_tree(data: dict) -> list[str]:
    """Estructura pre-orden: ids secuenciales y parent/children consistentes."""
    nodes = data.get("nodes") or []
    errors: list[str] = []
    declared = int(data.get("node_count", len(nodes)))
    if declared != len(nodes):
        errors.append(f"node_count={declared} but nodes has {len(nodes)} entries")
    for index, node in enumerate(nodes):
        if int(node.get("id", index)) != index:
            errors.append(f"nodes[{index}].id is {node.get('id')!r}")
        parent = int(node.get("parent", -1))
        children = [int(child) for child in (node.get("children") or [])]
        if index == 0:
            if parent != -1:
                errors.append("root parent must be -1")
        else:
            if not 0 <= parent < index:
                errors.append(f"nodes[{index}].parent={parent} is not a previous node")
            elif index not in [int(child) for child in (nodes[parent].get("children") or [])]:
                errors.append(f"nodes[{index}] is missing from nodes[{parent}].children")
        for child in children:
            if not 0 <= child < len(nodes):
                errors.append(f"nodes[{index}].children has out-of-range {child}")
            elif child <= index:
                errors.append(f"nodes[{index}].children has non-forward {child}")
            elif int(nodes[child].get("parent", -1)) != index:
                errors.append(f"nodes[{child}].parent does not point back at {index}")
    return errors


def emit_from_ui_json(ui_path: Path, out_path: Path, meta_path: Path) -> int:
    data, payload, atlas_size = load_ui_json(ui_path)
    errors = validate_ui_tree(data)
    if errors:
        for item in errors[:20]:
            print(f"error: {item}")
        raise SystemExit(f"{ui_path}: invalid ui.json ({len(errors)} errors)")
    print(f"ui.json: structure ok ({len(data['nodes'])} pre-order nodes, "
          f"parents/children consistent)")
    nodes: list[dict] = []
    meta: list[dict] = []
    glyphs: list[dict] = []
    runs: list[dict] = []
    inputs: list[dict] = []
    input_glyphs: list[dict] = []
    input_blocks: dict[tuple, tuple[int, int]] = {}
    for item in data.get("nodes", []):
        style = item.get("style") or {}
        kind_name = item.get("kind", "block")
        if kind_name == "text":
            kind = KIND_TEXT
        elif kind_name == "grid":
            kind = KIND_GRID
        elif kind_name == "image":
            kind = KIND_IMAGE
        elif kind_name == "input":
            kind = KIND_INPUT
        elif kind_name == "flex":
            kind = KIND_FLEX
        else:
            kind = KIND_BLOCK
        if kind_name not in ("text", "grid", "image", "input", "flex", "block"):
            display = str(style.get("display", "")).strip().lower()
            if display == "grid":
                kind = KIND_GRID
                kind_name = "grid"
            elif display == "image":
                kind = KIND_IMAGE
                kind_name = "image"
        attributes = item.get("data") or {}
        action_raw = (attributes.get("action") or "").strip().lower()
        text = item.get("text")
        content = text["content"] if text else None
        input_spec = item.get("input")
        input_meta = None
        if kind == KIND_INPUT:
            if not isinstance(input_spec, dict):
                raise SystemExit(
                    f"{ui_path}: node {item.get('id')} kind=input has no input object")
            glyph_list = input_spec.get("glyphs") or []
            cps = [int(glyph.get("cp", -1)) for glyph in glyph_list]
            if cps != list(range(32, 127)):
                raise SystemExit(
                    f"{ui_path}: node {item.get('id')} input glyph pool must be "
                    f"ASCII 32..126 in order")
            key = tuple(
                (int(glyph["cp"]), int(glyph["gid"]),
                 round(float(glyph["advance"]), 6),
                 round(float(glyph["x"]), 6), round(float(glyph["y"]), 6),
                 round(float(glyph["w"]), 6), round(float(glyph["h"]), 6),
                 round(float(glyph["u0"]), 9), round(float(glyph["v0"]), 9),
                 round(float(glyph["u1"]), 9), round(float(glyph["v1"]), 9))
                for glyph in glyph_list)
            block = input_blocks.get(key)
            if block is None:
                first = len(input_glyphs)
                for glyph in glyph_list:
                    input_glyphs.append({
                        "cp": int(glyph["cp"]), "gid": int(glyph["gid"]),
                        "advance": float(glyph["advance"]),
                        "x": float(glyph["x"]), "y": float(glyph["y"]),
                        "w": float(glyph["w"]), "h": float(glyph["h"]),
                        "u0": float(glyph["u0"]), "v0": float(glyph["v0"]),
                        "u1": float(glyph["u1"]), "v1": float(glyph["v1"]),
                    })
                block = (first, len(glyph_list))
                input_blocks[key] = block
            max_length = max(0, min(255, int(input_spec.get("max_length", 0) or 0)))
            inputs.append({
                "node": int(item.get("id", len(nodes))),
                "value": str(input_spec.get("value") or ""),
                "placeholder": str(input_spec.get("placeholder") or ""),
                "max_length": max_length,
                "password": 1 if input_spec.get("password") else 0,
                "baseline": float(input_spec.get("baseline", 0.0) or 0.0),
                "line_height": float(input_spec.get("line_height", 0.0) or 0.0),
                "glyph_first": block[0],
                "glyph_count": block[1],
            })
            input_meta = {
                "value": str(input_spec.get("value") or ""),
                "placeholder": str(input_spec.get("placeholder") or ""),
                "max_length": max_length,
                "password": bool(input_spec.get("password")),
                "baseline": float(input_spec.get("baseline", 0.0) or 0.0),
                "line_height": float(input_spec.get("line_height", 0.0) or 0.0),
            }
        elif input_spec:
            raise SystemExit(
                f"{ui_path}: node {item.get('id')} has input data but kind is not input")
        image = item.get("image") or {}
        image_src = image.get("src")
        image_alt = image.get("alt")
        object_fit = object_fit_value(image.get("object_fit", style.get("object_fit")))
        image_w, image_h = image_size(image_src, ui_path.parent)
        grid_mode, grid_column_px = grid_columns(style.get("grid_template_columns"))
        grid_rows = auto_rows(style.get("grid_auto_rows"))
        gap = _as_float(style.get("gap")) or 0.0
        row_gap = _as_float(style.get("row_gap"))
        column_gap = _as_float(style.get("column_gap"))
        if row_gap is None:
            row_gap = gap
        if column_gap is None:
            column_gap = gap
        text_align = {"left": 0, "center": 1, "right": 2}.get(
            str(style.get("text_align") or "left").strip().lower(), 0)
        line_height = max(0.0, _as_float(style.get("line_height")) or 0.0)
        margin_auto = 0
        for index, flag in enumerate(style.get("margin_auto") or []):
            if flag:
                margin_auto |= 1 << index
        box_sizing = 1 if str(style.get("box_sizing") or "").strip().lower() == "border-box" else 0
        flex_grow = max(0.0, _as_float(style.get("flex_grow")) or 0.0)
        flex_shrink = _as_float(style.get("flex_shrink"))
        flex_shrink = 1.0 if flex_shrink is None else max(0.0, flex_shrink)
        flex_basis = _as_float(style.get("flex_basis"))
        flex_basis = -1.0 if flex_basis is None else flex_basis
        nodes.append({
            "parent": int(item.get("parent", -1)),
            "kind": kind,
            "direction": int(style.get("direction", 0)),
            "justify": int(style.get("justify", 0)),
            "align": int(style.get("align", 3)),
            "wrap": int(style.get("wrap", 0)),
            "width": float(style.get("width_px", -1.0)),
            "height": float(style.get("height_px", -1.0)),
            "width_pct": float(style.get("width_pct", 0.0)),
            "height_pct": float(style.get("height_pct", 0.0)),
            "margin": [float(v) for v in style.get("margin", [0, 0, 0, 0])],
            "padding": [float(v) for v in style.get("padding", [0, 0, 0, 0])],
            "background": argb(style.get("background")),
            "text": content,
            "hover_bg": argb(attributes.get("hover")),
            "press_bg": argb(attributes.get("press")),
            "selected_bg": argb(attributes.get("selected")),
            "action": ACTION.get(action_raw, 0),
            "gap": float(gap),
            "font_size": float(style.get("font_size", DEFAULT_SIZE)),
            "text_color": argb(style.get("color")) or 0xFF000000,
            "text_align": text_align,
            "line_height": line_height,
            "margin_auto": margin_auto,
            "box_sizing": box_sizing,
            "flex_grow": flex_grow,
            "flex_shrink": flex_shrink,
            "flex_basis": flex_basis,
            "grid_mode": grid_mode,
            "grid_column_px": grid_column_px,
            "grid_auto_rows": grid_rows,
            "row_gap": float(row_gap),
            "column_gap": float(column_gap),
            "overflow": overflow_value(style.get("overflow")),
            "image_width": image_w,
            "image_height": image_h,
            "image_src": image_src,
            "image_alt": image_alt,
            "object_fit": object_fit,
            # CSS core v1: radio uniforme, primer borde y primera sombra.
            **css_node_fields(style),
        })
        meta.append({
            "id": item.get("id", len(meta)),
            "name": item.get("name", ""),
            "tag": item.get("tag", ""),
            "classes": list(item.get("classes") or []),
            "element_id": item.get("element_id"),
            "kind": kind_name,
            "direction": None if kind == KIND_TEXT else ("column" if int(style.get("direction", 0)) else "row"),
            "wrap": False if kind == KIND_TEXT else bool(int(style.get("wrap", 0))),
            "background": None if kind == KIND_TEXT else style.get("background"),
            "interactive": False if kind == KIND_TEXT else bool(action_raw),
            "text": content,
            "grid": {
                "mode": "auto_fill" if grid_mode == GRID_AUTO_FILL else None,
                "column_px": grid_column_px,
                "row_px": grid_rows,
            },
            "overflow": overflow_value(style.get("overflow")),
            "image": {
                "src": image_src,
                "alt": image_alt,
                "object_fit": object_fit,
                "width": image_w,
                "height": image_h,
            } if kind == KIND_IMAGE else None,
        })
        if input_meta is not None:
            meta[-1]["input"] = input_meta
        if text:
            first = len(glyphs)
            for glyph in text.get("glyphs", []):
                glyphs.append({
                    "x": float(glyph["x"]),
                    "y": float(glyph["y"]),
                    "w": float(glyph["w"]),
                    "h": float(glyph["h"]),
                    "u0": float(glyph["u0"]),
                    "v0": float(glyph["v0"]),
                    "u1": float(glyph["u1"]),
                    "v1": float(glyph["v1"]),
                })
            runs.append({
                "node": int(item.get("id", len(nodes) - 1)),
                "first": first,
                "count": len(text.get("glyphs", [])),
                "baseline": float(text.get("baseline", 0.0)),
            })

    metrics = None
    if payload is not None:
        metrics = UiJsonMetrics(atlas_size, payload, glyphs, runs)
    elif glyphs or input_glyphs:
        raise SystemExit("ui.json has glyphs but no atlas")
    if glyphs and len(glyphs) != int((data.get("text") or {}).get("glyph_count", len(glyphs))):
        print("warning: ui.json glyph_count does not match the node glyphs")

    viewport = data.get("viewport") or {}
    width = int(viewport.get("width", 960))
    height = int(viewport.get("height", 640))
    write_header(nodes, meta, out_path, width, height, metrics, inputs, input_glyphs)

    source = data.get("source") or {}
    html_path = Path(source.get("html") or "")
    css_path = Path(source.get("css") or "")
    meta_text = {
        "atlas_size": metrics.packer.size if metrics is not None else 0,
        "glyph_count": len(glyphs),
        "text_run_count": len(runs),
    }
    if inputs:
        meta_text["input_count"] = len(inputs)
        meta_text["input_glyph_count"] = len(input_glyphs)
    meta_out = {
        "source": {
            "html": display_path(html_path) if str(html_path) else "",
            "css": display_path(css_path) if str(css_path) else "",
        },
        "default_viewport": [width, height],
        "node_count": len(nodes),
        "rule_count": int(data.get("rule_count", 0)),
        "text": meta_text,
        "nodes": meta,
    }
    meta_path.write_text(json.dumps(meta_out, indent=2, ensure_ascii=False) + "\n",
                         encoding="utf-8")
    print(f"emit_ui(ui-json): {len(nodes)} nodes, {len(glyphs)} glyphs, "
          f"{len(runs)} runs, {len(inputs)} inputs ({len(input_glyphs)} input glyphs) "
          f"-> {out_path}, {meta_path}")
    for item, node in zip(meta, nodes):
        print("  [%2d] %-42s kind=%-5s size=%sx%s%s"
              % (item["id"], item["name"], item["kind"], node["width"], node["height"],
                 (" text=" + repr(node["text"])) if node["text"] else ""))
    return 0


# --------------------------------------------------------------- comparison

NODE_FIELDS = ("parent", "kind", "direction", "justify", "align", "wrap",
               "width", "height", "width_pct", "height_pct",
               "margin", "padding", "background", "text",
               "hover_bg", "press_bg", "selected_bg", "action",
               "gap", "font_size", "text_color",
               "grid_mode", "grid_column_px", "grid_auto_rows",
               "row_gap", "column_gap", "overflow",
               "image_width", "image_height", "image_src", "image_alt",
               "object_fit",
               "border_width", "border_color", "border_radius",
               "shadow_x", "shadow_y", "shadow_blur", "shadow_spread",
               "shadow_color",
               "text_align", "line_height", "margin_auto", "box_sizing",
                "flex_grow", "flex_shrink", "flex_basis")

GLYPH_FIELDS = ("x", "y", "w", "h", "u0", "v0", "u1", "v1")
RUN_FIELDS = ("node", "first", "count", "baseline")

INPUT_FIELDS = ("node", "max_length", "password", "baseline", "line_height",
                "glyph_first", "glyph_count")
INPUT_GLYPH_FIELDS = ("cp", "gid", "advance", "x", "y", "w", "h",
                      "u0", "v0", "u1", "v1")

_NODE_RE = re.compile(
    r"^\s*\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*"
    r"(-?[\d.]+)f,\s*(-?[\d.]+)f,\s*(-?[\d.]+)f,\s*(-?[\d.]+)f,\s*"
    r"\{\s*([^}]*)\}\s*,\s*\{\s*([^}]*)\}\s*,\s*(0x[0-9A-Fa-f]+)u,\s*"
    r'(NULL|"(?:[^"\\]|\\.)*"),\s*'
    r"(0x[0-9A-Fa-f]+)u,\s*(0x[0-9A-Fa-f]+)u,\s*(0x[0-9A-Fa-f]+)u,\s*"
    r"(-?\d+),\s*(-?[\d.]+)f,\s*(-?[\d.]+)f,\s*(0x[0-9A-Fa-f]+)u,\s*"
    r"(-?\d+),\s*(-?[\d.]+)f,\s*(-?[\d.]+)f,\s*(-?[\d.]+)f,\s*(-?[\d.]+)f,\s*"
    r"(-?\d+),\s*(-?[\d.]+)f,\s*(-?[\d.]+)f,\s*"
    r'(NULL|"(?:[^"\\]|\\.)*"),\s*(NULL|"(?:[^"\\]|\\.)*"),\s*(-?\d+)'
    # CSS core v1 tail: optional so headers emitted before the extension
    # (the Python oracle and old Rust trees) still parse as square/no shadow.
    r"(?:,\s*(-?[\d.]+)f,\s*(0x[0-9A-Fa-f]+)u,\s*(-?[\d.]+)f,\s*"
    r"(-?[\d.]+)f,\s*(-?[\d.]+)f,\s*(-?[\d.]+)f,\s*(-?[\d.]+)f,\s*"
    r"(0x[0-9A-Fa-f]+)u)?"
    # E1 tail: text-align, line-height, margin-auto bitmask (optional).
    r"(?:,\s*(-?\d+),\s*(-?[\d.]+)f,\s*(-?\d+))?"
    # A tail: box-sizing (optional; absent means content-box).
    r"(?:,\s*(-?\d+))?"
    # Flex tail: grow, shrink, basis (optional).
    r"(?:,\s*(-?[\d.]+)f,\s*(-?[\d.]+)f,\s*(-?[\d.]+)f)?"
    r"\s*\}"
)

_NUM_RE = re.compile(r"-?\d+\.?\d*(?:[eE][-+]?\d+)?")


def _block(text: str, marker: str) -> str:
    if marker not in text:
        raise SystemExit(f"missing {marker!r} in header")
    return text.split(marker, 1)[1].split("};", 1)[0]


def _c_string(token: str) -> str | None:
    if token == "NULL":
        return None
    body = token[1:-1]
    out = bytearray()
    index = 0
    while index < len(body):
        char = body[index]
        if char != "\\":
            out.extend(char.encode("utf-8"))
            index += 1
            continue
        index += 1
        if index >= len(body):
            break
        escape = body[index]
        if escape == "n":
            out.append(10)
            index += 1
        elif escape == "x":
            out.append(int(body[index + 1:index + 3], 16))
            index += 3
        elif escape in "01234567":
            value = 0
            digits = 0
            while index < len(body) and digits < 3 and body[index] in "01234567":
                value = value * 8 + int(body[index])
                index += 1
                digits += 1
            out.append(value & 0xFF)
        else:
            out.extend(escape.encode("utf-8"))
            index += 1
    return bytes(out).decode("utf-8", errors="replace")


def parse_header(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")

    def define(name: str) -> int:
        match = re.search(rf"#define {name} (\d+)", text)
        return int(match.group(1)) if match else 0

    nodes = []
    for line in _block(text, "static const UiNode kUiNodes").splitlines():
        match = _NODE_RE.match(line)
        if not match:
            continue
        values = match.groups()
        nodes.append({
            "parent": int(values[0]), "kind": int(values[1]),
            "direction": int(values[2]), "justify": int(values[3]),
            "align": int(values[4]), "wrap": int(values[5]),
            "width": float(values[6]), "height": float(values[7]),
            "width_pct": float(values[8]), "height_pct": float(values[9]),
            "margin": [float(v) for v in _NUM_RE.findall(values[10])],
            "padding": [float(v) for v in _NUM_RE.findall(values[11])],
            "background": int(values[12], 16),
            "text": _c_string(values[13]),
            "hover_bg": int(values[14], 16), "press_bg": int(values[15], 16),
            "selected_bg": int(values[16], 16), "action": int(values[17]),
            "gap": float(values[18]), "font_size": float(values[19]),
            "text_color": int(values[20], 16),
            "grid_mode": int(values[21]),
            "grid_column_px": float(values[22]),
            "grid_auto_rows": float(values[23]),
            "row_gap": float(values[24]), "column_gap": float(values[25]),
            "overflow": int(values[26]),
            "image_width": float(values[27]), "image_height": float(values[28]),
            "image_src": _c_string(values[29]), "image_alt": _c_string(values[30]),
            "object_fit": int(values[31]),
            "border_width": float(values[32]) if values[32] is not None else 0.0,
            "border_color": int(values[33], 16) if values[33] is not None else 0,
            "border_radius": float(values[34]) if values[34] is not None else 0.0,
            "shadow_x": float(values[35]) if values[35] is not None else 0.0,
            "shadow_y": float(values[36]) if values[36] is not None else 0.0,
            "shadow_blur": float(values[37]) if values[37] is not None else 0.0,
            "shadow_spread": float(values[38]) if values[38] is not None else 0.0,
            "shadow_color": int(values[39], 16) if values[39] is not None else 0,
            "text_align": int(values[40]) if values[40] is not None else 0,
            "line_height": float(values[41]) if values[41] is not None else 0.0,
            "margin_auto": int(values[42]) if values[42] is not None else 0,
            "box_sizing": int(values[43]) if values[43] is not None else 0,
            "flex_grow": float(values[44]) if values[44] is not None else 0.0,
            "flex_shrink": float(values[45]) if values[45] is not None else 1.0,
            "flex_basis": float(values[46]) if values[46] is not None else -1.0,
        })

    glyphs = []
    for line in _block(text, "static const UiGlyph kUiGlyphs").splitlines():
        numbers = [float(v) for v in _NUM_RE.findall(line)]
        if len(numbers) == len(GLYPH_FIELDS):
            glyphs.append(dict(zip(GLYPH_FIELDS, numbers)))

    runs = []
    for line in _block(text, "static const UiTextRun kUiTextRuns").splitlines():
        numbers = [float(v) for v in _NUM_RE.findall(line)]
        if len(numbers) == len(RUN_FIELDS):
            run = dict(zip(RUN_FIELDS, numbers))
            run["node"] = int(run["node"])
            run["first"] = int(run["first"])
            run["count"] = int(run["count"])
            runs.append(run)

    names = []
    name_re = re.compile(r'^\s*("(?:[^"\\]|\\.)*")\s*,\s*$')
    for line in _block(text, "static const char *const kUiNodeNames").splitlines():
        match = name_re.match(line)
        if match:
            names.append(_c_string(match.group(1)))

    atlas = []
    atlas_block = _block(text, "static const unsigned char kUiAtlas")
    initializer = atlas_block.split("=", 1)[1] if "=" in atlas_block else ""
    atlas = [int(v) for v in re.findall(r"\d+", initializer)]

    inputs: list[dict] = []
    input_glyphs: list[dict] = []
    input_count = 0
    input_glyph_count = 0
    if "static const UiInputGlyph kUiInputGlyphs" in text:
        input_count = define("UI_INPUT_COUNT")
        input_glyph_count = define("UI_INPUT_GLYPH_COUNT")
        for line in _block(text, "static const UiInputGlyph kUiInputGlyphs").splitlines():
            numbers = [float(v) for v in _NUM_RE.findall(line)]
            if len(numbers) == len(INPUT_GLYPH_FIELDS):
                glyph = dict(zip(INPUT_GLYPH_FIELDS, numbers))
                glyph["cp"] = int(glyph["cp"])
                glyph["gid"] = int(glyph["gid"])
                input_glyphs.append(glyph)
        input_re = re.compile(
            r'^\s*\{\s*(\d+),\s*(NULL|"(?:[^"\\]|\\.)*"),\s*'
            r'(NULL|"(?:[^"\\]|\\.)*")\s*,\s*(-?\d+),\s*(-?\d+),\s*'
            r'(-?[\d.]+)f,\s*(-?[\d.]+)f,\s*(\d+),\s*(\d+)\s*\}'
        )
        for line in _block(text, "static const UiInput kUiInputs").splitlines():
            match = input_re.match(line)
            if match:
                values = match.groups()
                inputs.append({
                    "node": int(values[0]),
                    "value": _c_string(values[1]),
                    "placeholder": _c_string(values[2]),
                    "max_length": int(values[3]),
                    "password": int(values[4]),
                    "baseline": float(values[5]),
                    "line_height": float(values[6]),
                    "glyph_first": int(values[7]),
                    "glyph_count": int(values[8]),
                })
    return {
        "node_count": define("UI_NODE_COUNT"),
        "glyph_count": define("UI_GLYPH_COUNT"),
        "run_count": define("UI_TEXT_RUN_COUNT"),
        "atlas_size": define("UI_ATLAS_SIZE"),
        "viewport": (define("UI_VIEWPORT_WIDTH"), define("UI_VIEWPORT_HEIGHT")),
        "nodes": nodes,
        "glyphs": glyphs,
        "runs": runs,
        "names": names,
        "atlas": atlas,
        "input_count": input_count,
        "input_glyph_count": input_glyph_count,
        "inputs": inputs,
        "input_glyphs": input_glyphs,
    }


def _same(a, b, eps: float = 1e-6) -> bool:
    if isinstance(a, float) or isinstance(b, float):
        return abs(float(a) - float(b)) <= eps
    return a == b


def _values_differ(name: str, left, right) -> bool:
    if name in ("margin", "padding"):
        return len(left) != len(right) or any(not _same(a, b) for a, b in zip(left, right))
    return not _same(left, right)


def compare_headers(left_path: Path, right_path: Path, strict: bool) -> int:
    left = parse_header(left_path)
    right = parse_header(right_path)
    failures: list[str] = []
    differences: list[str] = []
    bitmap_differences: list[str] = []
    uv_shifted: list[str] = []

    def check(label: str, a, b) -> None:
        if _values_differ(label, a, b):
            differences.append(f"{label}: {a!r} != {b!r}")

    for key in ("node_count", "glyph_count", "run_count", "atlas_size", "viewport"):
        if left[key] != right[key]:
            failures.append(f"{key}: {left[key]!r} != {right[key]!r}")

    if left["node_count"] != len(left["nodes"]) or right["node_count"] != len(right["nodes"]):
        failures.append("node_count does not match the number of parsed nodes")

    for index in range(min(len(left["nodes"]), len(right["nodes"]))):
        for field in NODE_FIELDS:
            check(f"node[{index}].{field}", left["nodes"][index][field],
                  right["nodes"][index][field])

    for index in range(min(len(left["names"]), len(right["names"]))):
        if left["names"][index] != right["names"][index]:
            differences.append(
                f"name[{index}]: {left['names'][index]!r} != {right['names'][index]!r}")
    if len(left["names"]) != len(right["names"]):
        failures.append(f"names: {len(left['names'])} != {len(right['names'])}")
    node_differences = len(differences)

    # El header no lleva gid: la unica evidencia de un bitmap distinto es que
    # cambie w/h. Si un glyph tiene la misma caja pero otras UVs, es
    # consecuencia de que el shelf se desplazo por una caja anterior.
    box_changed = False
    x_exact = 0
    y_exact = 0
    uv_exact = 0
    for index in range(min(len(left["glyphs"]), len(right["glyphs"]))):
        a = left["glyphs"][index]
        b = right["glyphs"][index]
        if abs(a["x"] - b["x"]) > 0.51:
            differences.append(f"glyph[{index}].x: {a['x']} != {b['x']}")
        else:
            x_exact += 1
        width_height_same = (
            abs(a["w"] - b["w"]) <= 1e-6 and abs(a["h"] - b["h"]) <= 1e-6
        )
        if width_height_same and abs(a["y"] - b["y"]) <= 0.51:
            y_exact += 1
        if not width_height_same:
            bitmap_differences.append(
                f"glyph[{index}] bitmap box: y/h {a['y']}/{a['h']} -> "
                f"{b['y']}/{b['h']} (rasterizer hinting)")
            box_changed = True
        elif abs(a["y"] - b["y"]) > 0.51:
            differences.append(
                f"glyph[{index}].y: {a['y']} != {b['y']} (same bitmap box)")
        uv_same = all(abs(a[field] - b[field]) <= 1e-6
                      for field in ("u0", "v0", "u1", "v1"))
        if uv_same:
            uv_exact += 1
        elif (not width_height_same) or box_changed:
            uv_shifted.append(
                f"glyph[{index}]: UV follows a bitmap-box change "
                f"({a['u0']},{a['v0']},{a['u1']},{a['v1']} -> "
                f"{b['u0']},{b['v0']},{b['u1']},{b['v1']})")
        else:
            differences.append(
                f"glyph[{index}].uv: ({a['u0']},{a['v0']},{a['u1']},{a['v1']}) != "
                f"({b['u0']},{b['v0']},{b['u1']},{b['v1']})")

    for index in range(min(len(left["runs"]), len(right["runs"]))):
        for field in RUN_FIELDS:
            check(f"run[{index}].{field}", left["runs"][index][field],
                  right["runs"][index][field])

    # ui-tree/3: solo cuando alguna cabecera tiene inputs (el oraculo Python no
    # los emite; la paridad de paginas con input no se compara contra el).
    if left["input_count"] or right["input_count"]:
        for key in ("input_count", "input_glyph_count"):
            if left[key] != right[key]:
                failures.append(f"{key}: {left[key]!r} != {right[key]!r}")
        for index in range(min(len(left["inputs"]), len(right["inputs"]))):
            for field in ("node", "value", "placeholder", "max_length", "password",
                          "glyph_first", "glyph_count", "baseline", "line_height"):
                check(f"input[{index}].{field}", left["inputs"][index][field],
                      right["inputs"][index][field])
        for index in range(min(len(left["input_glyphs"]), len(right["input_glyphs"]))):
            for field in INPUT_GLYPH_FIELDS:
                check(f"input_glyph[{index}].{field}",
                      left["input_glyphs"][index][field],
                      right["input_glyphs"][index][field])

    atlas_diffs = 0
    if len(left["atlas"]) == len(right["atlas"]):
        atlas_diffs = sum(1 for a, b in zip(left["atlas"], right["atlas"]) if a != b)
    else:
        failures.append(f"atlas bytes: {len(left['atlas'])} != {len(right['atlas'])}")

    glyph_count = len(left["glyphs"])
    print(f"compare: {left_path} vs {right_path}")
    print(f"  nodes  : {left['node_count']} vs {right['node_count']} "
          f"({node_differences} node/style/name diffs)")
    print(f"  glyphs : {left['glyph_count']} vs {right['glyph_count']} "
          f"(x {x_exact}/{glyph_count} exact, y {y_exact}/{glyph_count} within 0.5 px, "
          f"UV {uv_exact}/{glyph_count} exact)")
    print(f"  runs   : {left['run_count']} vs {right['run_count']}")
    print(f"  atlas  : {left['atlas_size']}x{left['atlas_size']} vs "
          f"{right['atlas_size']}x{right['atlas_size']}, "
          f"{atlas_diffs}/{len(left['atlas'])} bytes differ (AA)")
    for item in bitmap_differences:
        print(f"  known  : {item}")
    for item in uv_shifted:
        print(f"  known  : {item}")
    for item in failures:
        print(f"  FAIL {item}")
    if differences:
        print(f"  semantic differences: {len(differences)}")
        for item in differences[:40]:
            print(f"    {item}")
        if len(differences) > 40:
            print(f"    ... {len(differences) - 40} more")
    else:
        print("  semantic differences: 0")
    if strict:
        if atlas_diffs:
            failures.append(f"atlas bytes differ ({atlas_diffs})")
        failures.extend(bitmap_differences)
        failures.extend(uv_shifted)
    ok = not failures and not differences
    print("compare:", "PASS" if ok else "FAIL",
          "(strict)" if strict else "(semantic; bitmap-box/AA differences allowed)")
    return 0 if ok else 1


# --------------------------------------------------------------------- main

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
    parser.add_argument("--ui-json", default=None,
                        help="consume the Rust pre-layout style tree instead of HTML+CSS")
    parser.add_argument("--compare", nargs=2, metavar=("PYTHON_H", "RUST_H"),
                        default=None, help="compare two generated ui_data.h files")
    parser.add_argument("--strict", action="store_true",
                        help="with --compare, fail on bitmap-box, UV-shift or AA differences")
    args = parser.parse_args()

    if args.compare:
        return compare_headers(Path(args.compare[0]), Path(args.compare[1]), args.strict)
    if args.ui_json:
        return emit_from_ui_json(Path(args.ui_json), Path(args.out), Path(args.meta))

    from webnative.cascade import compute_styles
    from webnative.css_parse import parse_stylesheets
    from webnative.html_parse import parse_html_file, stylesheet_texts

    html_path = Path(args.html)
    css_path = Path(args.css)
    root = parse_html_file(html_path)
    texts = [css_path.read_text(encoding="utf-8")]
    # Also honour <style> / <link> order if the fixture grows.
    texts = stylesheet_texts(root, html_path.parent) or texts
    rules = parse_stylesheets(texts)
    styles = compute_styles(root, rules)

    metrics = None if args.no_text_metrics else TextMetrics()
    emitter = Emitter(root, styles, metrics, base_dir=html_path.parent)
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
