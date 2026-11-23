"""Layout de bloque (border-box) -> LayoutBox con geometría.

Cajas apiladas, height auto por contenido, sin colapso de márgenes. `kind` y
el despacho por `display` dejan el hueco para layout flex futuro.
"""
from dataclasses import dataclass, field

from .cascade import ComputedStyle
from .ir import DisplayList, RectOp
from .tree import Node


@dataclass
class LayoutBox:
    node: Node
    style: ComputedStyle
    x: float
    y: float
    width: float
    height: float
    kind: str = "block"          # "block" | "flex" (futuro)
    children: list["LayoutBox"] = field(default_factory=list)
    margin_bottom: float = 0.0
    text: str = ""               # futuro TextOp

    @property
    def right(self) -> float:
        return self.x + self.width

    @property
    def bottom(self) -> float:
        return self.y + self.height

    def walk(self):
        yield self
        for child in self.children:
            yield from child.walk()


def find_body(root: Node) -> Node:
    return next((node for node in root.walk() if node.tag == "body"), root)


def _layout_block(node: Node, style: ComputedStyle, styles: dict[Node, ComputedStyle],
                  origin_x: float, origin_y: float, available_width: float) -> LayoutBox:
    margin, padding = style.margin, style.padding
    width = style.width if style.width is not None else max(
        available_width - margin[1] - margin[3], 0.0)
    x, y = origin_x + margin[3], origin_y + margin[0]
    content_x, content_y = x + padding[3], y + padding[0]
    content_width = max(width - padding[1] - padding[3], 0.0)
    box = LayoutBox(node, style, x, y, width, 0.0,
                    kind=style.display if style.display in ("block", "flex") else "block",
                    margin_bottom=margin[2], text=style.text)
    cursor = content_y
    for child in node.children:
        child_style = styles.get(child)
        if child_style is None or child_style.display not in ("block", "flex"):
            continue  # inline/none quedan fuera del layout de bloque v0
        child_box = _layout_block(child, child_style, styles, content_x, cursor, content_width)
        box.children.append(child_box)
        cursor = child_box.bottom + child_box.margin_bottom
    content_used = cursor - content_y
    box.height = (style.height if style.height is not None
                  else max(content_used + padding[0] + padding[2], 0.0))
    return box


def layout_tree(root: Node, styles: dict[Node, ComputedStyle],
                viewport: tuple[int, int] = (400, 300)) -> LayoutBox:
    body = find_body(root)
    return _layout_block(body, styles[body], styles, 0.0, 0.0, float(viewport[0]))


def _paint(box: LayoutBox, ops: list[RectOp]) -> None:
    if box.style.background:
        ops.append(RectOp(box.x, box.y, box.width, box.height, box.style.background))
    for child in box.children:
        _paint(child, ops)


def paint(box: LayoutBox) -> list[RectOp]:
    """Pre-orden = fondo del padre antes que fondos de hijos (painter's order)."""
    ops: list[RectOp] = []
    _paint(box, ops)
    return ops


def to_display_list(box: LayoutBox, viewport: tuple[int, int]) -> DisplayList:
    return DisplayList(int(viewport[0]), int(viewport[1]), paint(box))
