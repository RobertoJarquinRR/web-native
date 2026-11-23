"""Cascada semántica: cssselect2 -> estilo computado por Node.

cssselect2 resuelve selectores, especificidad y orden; aquí se añaden
!important, style="", herencia y normalización a valores de layout. v1 añade
las propiedades flex que consume `layout._layout_flex_children` y conserva el
resto en `ComputedStyle.raw` para futuras etapas.
"""
import re
from dataclasses import dataclass
from xml.etree import ElementTree as ET

import cssselect2
from tinycss2.color3 import parse_color as css_color

from .css_parse import Rule, parse_declarations
from .html_parse import NON_RENDERED
from .tree import Node

SIDES = ("top", "right", "bottom", "left")
INHERITED = {"color", "font-family", "font-size", "font-weight", "line-height",
             "text-align", "white-space"}
_INLINE = {"a", "abbr", "b", "bdi", "bdo", "br", "cite", "code", "em", "i", "img",
           "label", "q", "s", "small", "span", "strong", "sub", "sup", "u", "var", "wbr"}
UA_DISPLAY = ({tag: "none" for tag in NON_RENDERED}
              | {tag: "inline" for tag in _INLINE})
_LEN = re.compile(r"(-?(?:\d+\.?\d*|\.\d+))(px)?")


def length(value: str | None) -> float | None:
    """px o número sin unidad; auto/%/otras unidades -> None (auto en v1)."""
    if value is None:
        return None
    v = value.strip().lower()
    if v in ("", "auto", "none", "normal"):
        return None
    match = _LEN.fullmatch(v)
    return float(match.group(1)) if match else None


def parse_color(value: str | None) -> str | None:
    """#rgb/#rrggbb/rgb()/hsl()/nombres vía tinycss2; alfa 0 = sin fondo."""
    color = css_color(value) if value else None
    if color is None or color.alpha <= 0:
        return None
    return "#%02x%02x%02x" % tuple(round(c * 255) for c in
                                   (color.red, color.green, color.blue))


def _keyword(raw: dict[str, str], name: str, default: str) -> str:
    return (raw.get(name) or default).strip().lower()


@dataclass
class ComputedStyle:
    raw: dict[str, str]
    display: str
    margin: tuple[float, float, float, float]   # top, right, bottom, left
    padding: tuple[float, float, float, float]
    width: float | None
    height: float | None
    background: str | None
    color: str
    font_size: float
    font_family: str
    flex_direction: str = "row"          # row | column
    justify_content: str = "flex-start"  # flex-start | center | space-between | flex-end
    align_items: str = "stretch"         # stretch | flex-start | center | flex-end
    gap: float = 0.0
    text: str = ""                       # texto propio; entrada del pase de text_run


def _sides(raw: dict[str, str], prop: str) -> tuple[float, ...]:
    return tuple(length(raw.get(f"{prop}-{side}")) or 0.0 for side in SIDES)


def _resolve(node: Node, raw: dict[str, str]) -> ComputedStyle:
    font_size = length(raw.get("font-size"))
    gap = length(raw.get("gap"))
    if gap is None:
        gap = length(raw.get("row-gap")) or length(raw.get("column-gap")) or 0.0
    display = (raw.get("display") or UA_DISPLAY.get(node.tag, "block")).strip().lower()
    return ComputedStyle(
        raw=raw, display=display,
        margin=_sides(raw, "margin"), padding=_sides(raw, "padding"),
        width=length(raw.get("width")), height=length(raw.get("height")),
        background=parse_color(raw.get("background-color") or raw.get("background")),
        color=parse_color(raw.get("color")) or "#000000",
        font_size=16.0 if font_size is None else font_size,
        font_family=raw.get("font-family", "sans-serif"),
        flex_direction=_keyword(raw, "flex-direction", "row"),
        justify_content=_keyword(raw, "justify-content", "flex-start"),
        align_items=_keyword(raw, "align-items", "stretch"),
        gap=gap, text=node.text)


def _mirror(node: Node, mapping: dict[int, Node]) -> ET.Element:
    """cssselect2 exige etree; se replica 1:1 y se indexa por identidad."""
    element = ET.Element(node.tag, dict(node.attrs))
    mapping[id(element)] = node
    for child in node.children:
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
