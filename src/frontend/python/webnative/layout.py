"""Layout de bloque y flex (row/column) con line boxes de texto.

Reglas de v1, deliberadamente pequeñas y explícitas:

- Modelo border-box: `width`/`height` incluyen padding. Sin bordes.
- `display:block` apila hijos en vertical (sin colapso de márgenes).
- `display:flex` despacha a `_layout_flex_children`:
  `flex-direction: row|column`, `gap`, `justify-content: flex-start|center|
  space-between|flex-end`, `align-items: stretch|flex-start|center|flex-end`.
  Sin grow/shrink/wrap; si falta espacio se recorta el hueco (no se encoge).
- El texto propio del nodo se coloca antes de sus hijos, en una línea:
  baseline = content_y + ascender; alto de línea = ascender + descender.
  Las métricas se inyectan (`metrics`) para que layout no dependa de FreeType.

El resultado es un árbol de LayoutBox con rects y `TextRun` en orden de pintado.
"""
from dataclasses import dataclass, field
from typing import Callable

from .cascade import ComputedStyle
from .ir import DisplayList, RectOp
from .tree import Node

MetricsFn = Callable[[str, float], tuple[float, float]]
""" (font-family, size-px) -> (ascender, descender) en px. """


def normalize_text(text: str) -> str:
    """Colapsa whitespace como `white-space: normal` en una sola línea."""
    return " ".join((text or "").split())


def estimate_metrics(family: str, size: float) -> tuple[float, float]:
    """Fallback sin FreeType (0.8em/0.2em). El CLI inyecta métricas reales."""
    del family
    return size * 0.8, size * 0.2


@dataclass
class TextRun:
    text: str
    x: float
    baseline: float
    font_family: str
    font_size: float
    color: str


@dataclass
class LayoutBox:
    node: Node
    style: ComputedStyle
    x: float
    y: float
    width: float
    height: float
    kind: str = "block"          # "block" | "flex"
    children: list["LayoutBox"] = field(default_factory=list)
    margin_bottom: float = 0.0
    text_runs: list[TextRun] = field(default_factory=list)

    @property
    def right(self) -> float:
        return self.x + self.width

    @property
    def bottom(self) -> float:
        return self.y + self.height

    @property
    def text(self) -> str:
        return " ".join(run.text for run in self.text_runs)

    def walk(self):
        yield self
        for child in self.children:
            yield from child.walk()

    def translate(self, dx: float, dy: float) -> None:
        """Mueve el subárbol completo (flex mide en 0,0 y luego coloca)."""
        for box in self.walk():
            box.x += dx
            box.y += dy
            for run in box.text_runs:
                run.x += dx
                run.baseline += dy


def find_body(root: Node) -> Node:
    return next((node for node in root.walk() if node.tag == "body"), root)


def _renderable_children(node: Node, styles: dict[Node, ComputedStyle]) -> list[Node]:
    return [child for child in node.children
            if (style := styles.get(child)) is not None
            and style.display in ("block", "flex")]


def _justify(name: str, free: float, count: int, gap: float) -> tuple[float, float]:
    """(start_offset, spacing) en el eje principal para justify-content."""
    if count <= 1:
        return (free / 2.0 if name in ("center",) else
                free if name in ("flex-end", "end") else 0.0), gap
    if name in ("center",):
        return free / 2.0, gap
    if name in ("flex-end", "end"):
        return free, gap
    if name in ("space-between",):
        return 0.0, gap + free / (count - 1)
    return 0.0, gap  # flex-start/start y valores no soportados


def _layout_flex_children(parent_box: LayoutBox, children: list[Node],
                          styles: dict[Node, ComputedStyle],
                          content_x: float, content_y: float,
                          content_width: float, content_height: float | None,
                          metrics: MetricsFn, parent: ComputedStyle) -> float:
    """Coloca hijos flex y devuelve el tamaño consumido en el eje principal.

    Mide cada hijo en (0,0) con el ancho disponible y luego traslada el subárbol
    a su posición final. Así el tamaño automático de un hijo (p. ej. una línea
    de texto) sale del layout real y no de una tabla de medidas aparte.
    """
    if not children:
        return 0.0
    direction = parent.flex_direction
    gap = max(parent.gap, 0.0)
    measured = [(child, styles[child],
                 _layout_box(child, styles[child], styles, 0.0, 0.0,
                             content_width, metrics))
                for child in children]

    row = direction != "column"  # cualquier valor no-column se trata como row
    if row:
        main_sizes = [box.width for _n, _s, box in measured]
        cross_sizes = [box.height for _n, _s, box in measured]
        extent = content_height if content_height is not None else max(cross_sizes, default=0.0)
        main_available = content_width
    else:
        main_sizes = [box.height for _n, _s, box in measured]
        cross_sizes = [box.width for _n, _s, box in measured]
        extent = content_width
        main_available = content_height if content_height is not None else sum(main_sizes) + gap * (len(measured) - 1)

    used = sum(main_sizes) + gap * (len(measured) - 1)
    free = max(main_available - used, 0.0) if main_available is not None else 0.0
    start, spacing = _justify(parent.justify_content, free, len(measured), gap)

    cursor = 0.0
    for (child, child_style, box), main_size, cross_size in zip(measured, main_sizes, cross_sizes):
        # stretch en el eje cruzado: solo cuando el hijo no fijó esa dimensión.
        if parent.align_items in ("stretch", "normal"):
            if row and child_style.height is None:
                box.height = extent
                cross_size = extent
            elif not row and child_style.width is None:
                box.width = extent
                cross_size = extent
        if parent.align_items in ("center",):
            cross_offset = (extent - cross_size) / 2.0
        elif parent.align_items in ("flex-end", "end"):
            cross_offset = extent - cross_size
        else:  # stretch/flex-start/start
            cross_offset = 0.0

        if row:
            box.translate(content_x + start + cursor, content_y + cross_offset)
        else:
            box.translate(content_x + cross_offset, content_y + start + cursor)
        parent_box.children.append(box)
        cursor += main_size + spacing

    return extent if row else used


def _layout_box(node: Node, style: ComputedStyle, styles: dict[Node, ComputedStyle],
                origin_x: float, origin_y: float, available_width: float,
                metrics: MetricsFn) -> LayoutBox:
    margin, padding = style.margin, style.padding
    width = style.width if style.width is not None else max(
        available_width - margin[1] - margin[3], 0.0)
    x, y = origin_x + margin[3], origin_y + margin[0]
    content_x, content_y = x + padding[3], y + padding[0]
    content_width = max(width - padding[1] - padding[3], 0.0)
    box = LayoutBox(
        node, style, x, y, width, 0.0,
        kind=style.display if style.display in ("block", "flex") else "block",
        margin_bottom=margin[2])

    cursor_y = content_y
    text = normalize_text(node.text)
    if text:
        ascender, descender = metrics(style.font_family, style.font_size)
        box.text_runs.append(TextRun(text, content_x, content_y + ascender,
                                     style.font_family, style.font_size, style.color))
        cursor_y += ascender + descender

    children = _renderable_children(node, styles)
    if style.display == "flex" and children:
        content_height = (None if style.height is None else
                          max(style.height - padding[0] - padding[2], 0.0))
        cursor_y += _layout_flex_children(box, children, styles, content_x, cursor_y,
                                         content_width, content_height, metrics, style)
    else:
        for child in children:
            child_style = styles[child]
            child_box = _layout_box(child, child_style, styles, content_x, cursor_y,
                                    content_width, metrics)
            box.children.append(child_box)
            cursor_y = child_box.bottom + child_box.margin_bottom

    content_used = cursor_y - content_y
    box.height = (style.height if style.height is not None
                  else max(content_used + padding[0] + padding[2], 0.0))
    return box


def layout_tree(root: Node, styles: dict[Node, ComputedStyle],
                viewport: tuple[int, int] = (400, 240),
                metrics: MetricsFn | None = None) -> LayoutBox:
    body = find_body(root)
    return _layout_box(body, styles[body], styles, 0.0, 0.0, float(viewport[0]),
                       metrics or estimate_metrics)


def _paint_rects(box: LayoutBox, ops: list[RectOp]) -> None:
    if box.style.background:
        ops.append(RectOp(box.x, box.y, box.width, box.height, box.style.background))
    for child in box.children:
        _paint_rects(child, ops)


def paint(box: LayoutBox) -> list[RectOp]:
    """Vista solo-rects (compatibilidad v0); el texto va en `to_display_list`."""
    ops: list[RectOp] = []
    _paint_rects(box, ops)
    return ops


def _paint_all(box: LayoutBox, raw: list, runs: list[TextRun]) -> None:
    """Pre-orden: fondo del padre, su texto, y después los hijos."""
    if box.style.background:
        raw.append(RectOp(box.x, box.y, box.width, box.height, box.style.background))
    for run in box.text_runs:
        raw.append(len(runs))  # marcador; se sustituye por el TextOp ya shapeado
        runs.append(run)
    for child in box.children:
        _paint_all(child, raw, runs)


def collect_text_runs(box: LayoutBox) -> list[TextRun]:
    runs: list[TextRun] = []
    _paint_all(box, [], runs)
    return runs


def to_display_list(box: LayoutBox, viewport: tuple[int, int],
                    shaper=None) -> DisplayList:
    """LayoutBox -> DisplayList; `shaper` es un `text.TextEngine` (lazy si falta)."""
    raw: list = []
    runs: list[TextRun] = []
    _paint_all(box, raw, runs)
    ops: list = list(raw)
    if runs:
        if shaper is None:
            from .text import TextEngine
            shaper = TextEngine()
        from .text import build_text_ops
        text_ops = build_text_ops(runs, shaper)
        ops = [text_ops[item] if isinstance(item, int) else item for item in raw]
    return DisplayList(int(viewport[0]), int(viewport[1]), ops)
