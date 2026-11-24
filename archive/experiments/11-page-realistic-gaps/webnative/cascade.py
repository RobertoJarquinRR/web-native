"""Cascada semántica: cssselect2 -> estilo computado por Node.

cssselect2 resuelve selectores, especificidad y orden. Aquí se añaden
!important, style="", herencia y normalización a valores de layout.

Modelo de caja: **border-box** en todo el motor (width/height incluyen padding).
No hay content-box, border, ni box-sizing. Es una decisión explícita de v0.
"""
import re
from dataclasses import dataclass
from xml.etree import ElementTree as ET

import cssselect2
from tinycss2.color3 import parse_color as _css_color

from .css_parse import Rule, parse_declarations
from .html_parse import NON_RENDERED
from .tree import Node

SIDES = ("top", "right", "bottom", "left")
INHERITED = {"color", "font-family", "font-size", "font-weight", "line-height",
             "text-align", "white-space"}

_BLOCK_TAGS = {
    "html", "body", "div", "section", "article", "header", "footer", "nav",
    "main", "aside", "h1", "h2", "h3", "h4", "h5", "h6", "p", "ul", "ol", "li",
    "figure", "figcaption", "blockquote", "pre", "form", "fieldset", "hr",
    "address", "details", "summary", "table", "thead", "tbody", "tr", "td", "th",
}
_INLINE_TAGS = {
    "a", "abbr", "b", "bdi", "bdo", "br", "cite", "code", "em", "i", "img",
    "label", "q", "s", "small", "span", "strong", "sub", "sup", "u", "var", "wbr",
}
UA_DISPLAY = ({tag: "none" for tag in NON_RENDERED}
              | {tag: "block" for tag in _BLOCK_TAGS}
              | {tag: "inline" for tag in _INLINE_TAGS})

_LEN = re.compile(r"(-?(?:\d+\.?\d*|\.\d+))(px)?")
_NUM = re.compile(r"-?(?:\d+\.?\d*|\.\d+)")


def length(value: str | None) -> float | None:
    """px o número sin unidad. auto/%/em/rem/vh/... -> None (no soportado)."""
    if value is None:
        return None
    v = value.strip().lower()
    if not v:
        return None
    match = _LEN.fullmatch(v)
    return float(match.group(1)) if match else None


def parse_color(value: str | None) -> str | None:
    """#rgb/#rrggbb/rgb()/hsl()/nombres vía tinycss2; alfa 0 = sin color."""
    if not value:
        return None
    color = _css_color(value)
    if color is None or color.alpha <= 0:
        return None
    return "#%02x%02x%02x" % tuple(
        round(c * 255) for c in (color.red, color.green, color.blue))


def parse_weight(value: str | None, inherited: int = 400) -> int:
    if not value:
        return inherited
    v = value.strip().lower()
    if v == "normal":
        return 400
    if v == "bold":
        return 700
    if v == "bolder":
        return min(900, inherited + 300)
    if v == "lighter":
        return max(100, inherited - 300)
    if _NUM.fullmatch(v):
        return max(100, min(900, int(round(float(v)))))
    return inherited


def _edge(raw: dict[str, str], prop: str, side: str) -> float | None:
    """None representa `auto`; no confundir con 0."""
    value = raw.get(f"{prop}-{side}")
    if value is None:
        return 0.0
    if value.strip().lower() == "auto":
        return None
    return max(length(value) or 0.0, 0.0)


def _sides(raw: dict[str, str], prop: str) -> tuple[float | None, ...]:
    return tuple(_edge(raw, prop, side) for side in SIDES)


@dataclass
class ComputedStyle:
    raw: dict[str, str]
    display: str
    margin: tuple[float | None, float | None, float | None, float | None]
    padding: tuple[float, float, float, float]
    width: float | None
    height: float | None
    background: str | None
    color: str
    font_size: float
    font_family: str
    font_weight: int
    line_height: float | None       # px calculados; None = métricas de la fuente
    text_align: str
    white_space: str
    flex_direction: str
    justify_content: str
    align_items: str
    row_gap: float
    column_gap: float
    flex_grow: float
    flex_shrink: float
    flex_basis: float | None


def _resolve(node: Node, raw: dict[str, str]) -> ComputedStyle:
    font_size = length(raw.get("font-size"))
    font_size = 16.0 if font_size is None or font_size <= 0 else font_size
    line_height = raw.get("line-height")
    line_height_px: float | None = None
    if line_height:
        v = line_height.strip().lower()
        if v not in ("normal", ""):
            if _NUM.fullmatch(v):                     # multiplicador sin unidad
                line_height_px = float(v) * font_size
            elif v.endswith("%") and _NUM.fullmatch(v[:-1]):
                line_height_px = float(v[:-1]) / 100.0 * font_size
            else:
                parsed = length(v)
                if parsed is not None and parsed > 0:
                    line_height_px = parsed
    display = (raw.get("display") or UA_DISPLAY.get(node.tag, "block")).strip().lower()
    if display in ("", "initial", "unset"):
        display = UA_DISPLAY.get(node.tag, "block")
    return ComputedStyle(
        raw=raw,
        display=display,
        margin=_sides(raw, "margin"),
        padding=tuple(max(length(raw.get(f"padding-{side}")) or 0.0, 0.0)
                      for side in SIDES),
        width=(max(length(raw["width"]), 0.0)
               if raw.get("width") and length(raw["width"]) is not None else None),
        height=(max(length(raw["height"]), 0.0)
                if raw.get("height") and length(raw["height"]) is not None else None),
        background=(parse_color(raw.get("background-color"))
                    or parse_color(raw.get("background"))),
        color=parse_color(raw.get("color")) or "#000000",
        font_size=font_size,
        font_family=raw.get("font-family", "sans-serif"),
        font_weight=parse_weight(raw.get("font-weight")),
        line_height=line_height_px,
        text_align=(raw.get("text-align") or "left").strip().lower(),
        white_space=(raw.get("white-space") or "normal").strip().lower(),
        flex_direction=(raw.get("flex-direction") or "row").strip().lower(),
        justify_content=(raw.get("justify-content") or "flex-start").strip().lower(),
        align_items=(raw.get("align-items") or "stretch").strip().lower(),
        row_gap=max(length(raw.get("row-gap")) or 0.0, 0.0),
        column_gap=max(length(raw.get("column-gap")) or 0.0, 0.0),
        flex_grow=float(raw.get("flex-grow") or 0.0) if _NUM.fullmatch(
            (raw.get("flex-grow") or "0").strip()) else 0.0,
        flex_shrink=float(raw.get("flex-shrink") or 1.0) if _NUM.fullmatch(
            (raw.get("flex-shrink") or "1").strip()) else 1.0,
        flex_basis=length(raw.get("flex-basis")),
    )


def _mirror(node: Node, mapping: dict[int, Node]) -> ET.Element:
    """cssselect2 exige etree; se replica 1:1 (sin #text) y se indexa por identidad."""
    element = ET.Element(node.tag, dict(node.attrs))
    mapping[id(element)] = node
    for child in node.children:
        if not child.is_text:
            element.append(_mirror(child, mapping))
    return element


def compute_styles(root: Node, rules: list[Rule]) -> dict[Node, ComputedStyle]:
    matcher = cssselect2.Matcher()
    for index, rule in enumerate(rules):
        matcher.add_selector(rule.selector, index)
    mapping: dict[int, Node] = {}
    wrappers = cssselect2.ElementWrapper.from_html_root(_mirror(root, mapping))
    styles: dict[Node, ComputedStyle] = {}
    for wrapper in wrappers.iter_subtree():
        node = mapping[id(wrapper.etree_element)]
        parent = styles.get(node.parent)
        raw = {k: v for k, v in parent.raw.items() if k in INHERITED} if parent else {}
        winners: dict[str, tuple[tuple, str]] = {}

        def offer(name: str, value: str, key: tuple) -> None:
            if name not in winners or key >= winners[name][0]:
                winners[name] = (key, value)

        for specificity, order, _pseudo, index in matcher.match(wrapper):
            for decl_index, (name, value, important) in enumerate(rules[index].declarations):
                offer(name, value, (important, 0, specificity, order, decl_index))
        for decl_index, (name, value, important) in enumerate(
                parse_declarations(node.attrs.get("style", ""))):
            # style="" gana a selectores normales; !important también gana.
            offer(name, value, (important, 1, (0, 0, 0), 0, decl_index))
        raw.update({name: value for name, (_key, value) in winners.items()})
        styles[node] = _resolve(node, raw)
    return styles
