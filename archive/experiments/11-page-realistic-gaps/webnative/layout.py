"""Layout: flujo de bloque + flex row/column + líneas de texto con wrapping.

Soporta:
  - display: block | flex | none (inline se aplana dentro de su contenedor)
  - width/height en px, auto (fill para bloque, max-content para ítems flex)
  - margin/padding (margin auto solo en el eje principal de flex y en el eje
    horizontal de bloque), gap, justify-content, align-items
  - texto: una caja de texto por elemento con contenido inline, wrapping greedy
    por espacios, text-align left/center/right
No soporta: porcentajes, em/rem/vh, min/max, flex-grow/shrink/basis, wrap,
position, float, border, radios, sombras, clip, inline formatting context real.

Modelo border-box. `forced_height` lo usa el padre flex para stretch.
"""
from dataclasses import dataclass, field

from .cascade import ComputedStyle
from .html_parse import find_body, text_content
from .tree import Node

JUSTIFY = {"flex-start", "start", "flex-end", "end", "center",
           "space-between", "space-around", "space-evenly"}


@dataclass
class Line:
    text: str
    x: float
    baseline: float
    width: float
    face: str
    size: float
    color: str
    weight: int
    line_height: float


@dataclass
class Box:
    node: Node | None
    tag: str
    classes: str
    style: ComputedStyle | None
    x: float
    y: float
    w: float
    h: float
    kind: str
    children: list["Box"] = field(default_factory=list)
    lines: list[Line] = field(default_factory=list)
    flex_info: dict | None = None

    @property
    def right(self) -> float:
        return self.x + self.w

    @property
    def bottom(self) -> float:
        return self.y + self.h

    def walk(self):
        yield self
        for child in self.children:
            yield from child.walk()

    def translate(self, dx: float, dy: float) -> None:
        self.x += dx
        self.y += dy
        for line in self.lines:
            line.x += dx
            line.baseline += dy
        for child in self.children:
            child.translate(dx, dy)

    def class_set(self) -> set[str]:
        return set(self.classes.split())


def _norm(value: str) -> str:
    value = (value or "").strip().lower()
    if value == "start":
        return "flex-start"
    if value == "end":
        return "flex-end"
    if value == "normal":
        return "stretch"
    return value


class LayoutEngine:
    def __init__(self, styles: dict[Node, ComputedStyle], fonts, viewport: tuple[int, int]):
        self.styles = styles
        self.fonts = fonts
        self.viewport = viewport
        self.warnings: list[str] = []

    # ---------------------------------------------------------------- helpers
    def style_of(self, node: Node) -> ComputedStyle:
        style = self.styles.get(node)
        if style is None:
            raise KeyError(f"sin estilo para <{node.tag}>")
        return style

    def _visible_children(self, node: Node) -> list[Node]:
        return [child for child in node.children
                if not child.is_text and self.style_of(child).display != "none"]

    def _has_block_descendant(self, node: Node) -> bool:
        for child in node.children:
            if child.is_text:
                continue
            if self.style_of(child).display in ("block", "flex"):
                return True
            if self._has_block_descendant(child):
                return True
        return False

    def kind(self, node: Node, style: ComputedStyle) -> str:
        if style.display == "none":
            return "none"
        if style.display == "flex":
            return "flex"
        if not self._has_block_descendant(node) and text_content(node).strip():
            return "text"
        return "block"

    def _text(self, node: Node) -> str:
        return " ".join(text_content(node).split())

    # ------------------------------------------------------------- intrinsics
    def intrinsic_width(self, node: Node, style: ComputedStyle) -> float:
        if style.width is not None:
            return style.width
        pad = style.padding
        base = pad[1] + pad[3]
        kind = self.kind(node, style)
        if kind == "text":
            face = self.fonts.face_key(style)
            return base + self.fonts.advance(self._text(node), face, style.font_size)
        children = self._visible_children(node)
        if not children:
            return base
        outer = []
        for child in children:
            cs = self.style_of(child)
            margin = cs.margin
            ml = margin[3] or 0.0
            mr = margin[1] or 0.0
            outer.append(self.intrinsic_width(child, cs) + ml + mr)
        if kind == "flex" and _norm(style.flex_direction) == "row":
            gap = style.column_gap
            return base + sum(outer) + gap * max(len(outer) - 1, 0)
        return base + max(outer, default=0.0)

    # --------------------------------------------------------------- wrapping
    def _measure(self, text: str, face: str, size: float) -> float:
        return self.fonts.advance(text, face, size)

    def _break_word(self, word: str, max_w: float, face: str, size: float) -> list[str]:
        chunks: list[str] = []
        current = ""
        for char in word:
            candidate = current + char
            if current and self._measure(candidate, face, size) > max_w:
                chunks.append(current)
                current = char
            else:
                current = candidate
        if current:
            chunks.append(current)
        return chunks or [word]

    def wrap(self, text: str, max_w: float, face: str, size: float,
             white_space: str = "normal") -> list[str]:
        if not text:
            return []
        if white_space in ("nowrap", "pre") or max_w <= 1.0:
            return [text]
        words = [word for word in text.split(" ") if word]
        if not words:
            return []
        lines: list[str] = []
        current = ""
        for word in words:
            candidate = word if not current else current + " " + word
            if self._measure(candidate, face, size) <= max_w:
                current = candidate
                continue
            if current:
                lines.append(current)
                current = ""
            if self._measure(word, face, size) <= max_w:
                current = word
                continue
            chunks = self._break_word(word, max_w, face, size)
            lines.extend(chunks[:-1])
            current = chunks[-1]
        if current:
            lines.append(current)
        return lines

    def _align_offset(self, align: str, content_w: float, line_w: float) -> float:
        align = (align or "left").strip().lower()
        if align in ("center",):
            return (content_w - line_w) / 2.0
        if align in ("right", "end"):
            return content_w - line_w
        return 0.0  # left/start/justify (justify no implementado: left)

    # ------------------------------------------------------------------ boxes
    def layout(self, node: Node, style: ComputedStyle, x: float, y: float,
               width: float, forced_height: float | None = None,
               depth: int = 0) -> Box:
        width = max(width, 0.0)
        if style.display == "none":
            return Box(node, node.tag, node.attrs.get("class", ""), style,
                       x, y, 0.0, 0.0, "none")
        kind = self.kind(node, style)
        if kind == "text":
            return self._layout_text(node, style, x, y, width, forced_height)
        if kind == "flex":
            return self._layout_flex(node, style, x, y, width, forced_height, depth)
        return self._layout_block(node, style, x, y, width, forced_height, depth)

    def _layout_text(self, node: Node, style: ComputedStyle, x: float, y: float,
                     width: float, forced_height: float | None) -> Box:
        pad = style.padding
        content_w = max(width - pad[1] - pad[3], 0.0)
        text = self._text(node)
        face = self.fonts.face_key(style)
        size = style.font_size
        asc, desc, gap = self.fonts.metrics(face, size)
        line_height = style.line_height or (asc + desc + gap)
        lines_text = self.wrap(text, content_w, face, size, style.white_space)
        content_h = line_height * len(lines_text)
        height = forced_height if forced_height is not None else \
            (style.height if style.height is not None else pad[0] + content_h + pad[2])
        box = Box(node, node.tag, node.attrs.get("class", ""), style,
                  x, y, width, height, "text")
        top = y + pad[0]
        for line_text in lines_text:
            line_w = self._measure(line_text, face, size)
            offset = self._align_offset(style.text_align, content_w, line_w)
            baseline = top + (line_height - (asc + desc)) / 2.0 + asc
            box.lines.append(Line(line_text, x + pad[3] + offset, baseline, line_w,
                                  face, size, style.color, style.font_weight, line_height))
            top += line_height
        return box

    def _layout_block(self, node: Node, style: ComputedStyle, x: float, y: float,
                      width: float, forced_height: float | None, depth: int) -> Box:
        pad = style.padding
        content_x = x + pad[3]
        content_y = y + pad[0]
        content_w = max(width - pad[1] - pad[3], 0.0)
        box = Box(node, node.tag, node.attrs.get("class", ""), style,
                  x, y, width, 0.0, "block")
        cursor = content_y
        for child in node.children:
            if child.is_text:
                continue  # texto mezclado con bloques: no soportado (se ignora)
            cs = self.style_of(child)
            if cs.display == "none":
                continue
            mt = cs.margin[0] or 0.0
            mb = cs.margin[2] or 0.0
            ml_auto, mr_auto = cs.margin[3] is None, cs.margin[1] is None
            ml = cs.margin[3] or 0.0
            mr = cs.margin[1] or 0.0
            if cs.width is not None:
                child_w = cs.width
                rest = content_w - child_w
                if ml_auto and mr_auto:
                    ml = max(rest, 0.0) / 2.0
                    mr = max(rest - ml, 0.0)
                elif ml_auto:
                    ml = max(rest - mr, 0.0)
                elif mr_auto:
                    mr = max(rest - ml, 0.0)
            else:
                child_w = max(content_w - ml - mr, 0.0)
            child_box = self.layout(child, cs, content_x + ml, cursor + mt, child_w,
                                    None, depth + 1)
            box.children.append(child_box)
            cursor = child_box.bottom + mb
        used = cursor - content_y
        height = forced_height if forced_height is not None else \
            (style.height if style.height is not None else pad[0] + used + pad[2])
        box.h = height
        return box

    def _layout_flex(self, node: Node, style: ComputedStyle, x: float, y: float,
                     width: float, forced_height: float | None, depth: int) -> Box:
        pad = style.padding
        content_x, content_y = x + pad[3], y + pad[0]
        content_w = max(width - pad[1] - pad[3], 0.0)
        fixed_h = forced_height if forced_height is not None else style.height
        content_h = (max(fixed_h - pad[0] - pad[2], 0.0)
                     if fixed_h is not None else None)
        box = Box(node, node.tag, node.attrs.get("class", ""), style,
                  x, y, width, 0.0, "flex")
        children = self._visible_children(node)
        direction = _norm(style.flex_direction)
        if direction not in ("row", "column"):
            self.warnings.append(f"flex-direction {direction!r} no soportado; se usa row")
            direction = "row"
        justify = _norm(style.justify_content)
        if justify not in JUSTIFY:
            self.warnings.append(f"justify-content {justify!r} no soportado; flex-start")
            justify = "flex-start"
        align = _norm(style.align_items)
        if align not in ("stretch", "flex-start", "flex-end", "center"):
            self.warnings.append(f"align-items {align!r} no soportado; stretch")
            align = "stretch"
        if not children:
            box.h = fixed_h if fixed_h is not None else pad[0] + pad[2]
            box.flex_info = {"direction": direction, "children": 0}
            return box

        items = []
        for child in children:
            cs = self.style_of(child)
            m = cs.margin
            items.append({
                "node": child, "style": cs,
                "mt": m[0], "mr": m[1], "mb": m[2], "ml": m[3],
                "main": None, "cross": None, "box": None,
            })

        if direction == "row":
            self._flex_row(items, style, content_x, content_y, content_w, content_h,
                           justify, align, box, depth)
        else:
            self._flex_column(items, style, content_x, content_y, content_w, content_h,
                              justify, align, box, depth)
        return box

    def _resolve_auto_margins(self, items, axis: str, free: float) -> float:
        """Reparte free entre márgenes auto del eje principal. Devuelve el resto."""
        slots = []
        for item in items:
            for side in (("ml", "mr") if axis == "row" else ("mt", "mb")):
                if item[side] is None:
                    slots.append((item, side))
        if not slots or free <= 0:
            for item in items:
                for side in ("mt", "mr", "mb", "ml"):
                    if item[side] is None:
                        item[side] = 0.0
            return max(free, 0.0)
        share = free / len(slots)
        for item, side in slots:
            item[side] = share
        return 0.0

    def _justify(self, justify: str, free: float, n: int, gap: float):
        free = max(free, 0.0)
        if justify == "flex-end":
            return free, gap
        if justify == "center":
            return free / 2.0, gap
        if justify == "space-between" and n > 1:
            return 0.0, gap + free / (n - 1)
        if justify == "space-around" and n > 0:
            spacing = gap + free / n
            return spacing / 2.0, spacing
        if justify == "space-evenly" and n > 0:
            spacing = gap + free / (n + 1)
            return spacing, spacing
        return 0.0, gap

    def _flex_row(self, items, style, cx, cy, cw, content_h, justify, align, box, depth):
        gap = style.column_gap
        for item in items:
            cs = item["style"]
            item["main"] = cs.width if cs.width is not None else \
                max(self.intrinsic_width(item["node"], cs), 0.0)
            natural = self.layout(item["node"], cs, 0.0, 0.0, item["main"], cs.height,
                                  depth + 1)
            item["cross"] = natural.h

        cross = content_h if content_h is not None else max(
            (item["cross"] + (item["mt"] or 0.0) + (item["mb"] or 0.0)
             for item in items), default=0.0)
        used = sum(item["main"] + (item["ml"] or 0.0) + (item["mr"] or 0.0)
                   for item in items) + gap * max(len(items) - 1, 0)
        free = cw - used
        remaining = self._resolve_auto_margins(items, "row", free)
        start, spacing = self._justify(justify, remaining, len(items), gap)

        cursor = cx + start
        for item in items:
            ml = item["ml"] or 0.0
            mr = item["mr"] or 0.0
            mt = item["mt"] or 0.0
            mb = item["mb"] or 0.0
            item_h = item["cross"]
            forced = item["style"].height
            if align == "stretch" and forced is None:
                forced = max(cross - mt - mb, 0.0)
            if align == "flex-end":
                item_y = cy + cross - mb - item_h
            elif align == "center":
                item_y = cy + mt + (cross - (item_h + mt + mb)) / 2.0
            else:
                item_y = cy + mt
            item_x = cursor + ml
            item["box"] = self.layout(item["node"], item["style"], item_x, item_y,
                                      item["main"], forced, depth + 1)
            box.children.append(item["box"])
            cursor = item_x + item["main"] + mr + spacing

        if content_h is None:
            used_cross = max((child.bottom + (item["mb"] or 0.0) - cy
                              for child, item in zip(box.children, items)), default=0.0)
            box.h = style.padding[0] + used_cross + style.padding[2]
        else:
            box.h = content_h + style.padding[0] + style.padding[2]
        box.flex_info = {"direction": "row", "content": {"x": cx, "y": cy, "w": cw,
                        "h": content_h}, "used": used, "free": free,
                        "start": start, "spacing": spacing, "cross": cross,
                        "justify": justify, "align": align}

    def _flex_column(self, items, style, cx, cy, cw, content_h, justify, align, box, depth):
        gap = style.row_gap
        for item in items:
            cs = item["style"]
            ml = item["ml"] or 0.0
            mr = item["mr"] or 0.0
            item_w = cs.width if cs.width is not None else (
                max(cw - ml - mr, 0.0) if align == "stretch"
                else max(self.intrinsic_width(item["node"], cs), 0.0))
            item["main"] = item_w  # en columna, "main" guarda el ancho (cross real)
            natural = self.layout(item["node"], cs, 0.0, 0.0, item_w, cs.height,
                                  depth + 1)
            item["cross"] = natural.h

        used = sum(item["cross"] + (item["mt"] or 0.0) + (item["mb"] or 0.0)
                   for item in items) + gap * max(len(items) - 1, 0)
        free = (content_h - used) if content_h is not None else 0.0
        remaining = self._resolve_auto_margins(items, "column", free)
        start, spacing = self._justify(justify, remaining, len(items), gap)

        cursor = cy + start
        for item in items:
            ml = item["ml"] or 0.0
            mr = item["mr"] or 0.0
            mt = item["mt"] or 0.0
            mb = item["mb"] or 0.0
            item_w = item["main"]
            offset = 0.0
            if align == "center":
                offset = (cw - ml - mr - item_w) / 2.0
            elif align == "flex-end":
                offset = cw - ml - mr - item_w
            item_x = cx + ml + offset
            item_y = cursor + mt
            item["box"] = self.layout(item["node"], item["style"], item_x, item_y,
                                      item_w, item["cross"], depth + 1)
            box.children.append(item["box"])
            cursor = item_y + item["cross"] + mb + spacing

        if content_h is None:
            used_cross = max((child.bottom + (item["mb"] or 0.0) - cy
                              for child, item in zip(box.children, items)), default=0.0)
            box.h = style.padding[0] + used_cross + style.padding[2]
        else:
            box.h = content_h + style.padding[0] + style.padding[2]
        box.flex_info = {"direction": "column", "content": {"x": cx, "y": cy, "w": cw,
                        "h": content_h}, "used": used, "free": free,
                        "start": start, "spacing": spacing,
                        "justify": justify, "align": align}


def layout_document(root: Node, styles: dict[Node, ComputedStyle], fonts,
                    viewport: tuple[int, int]) -> tuple[Box, LayoutEngine]:
    engine = LayoutEngine(styles, fonts, viewport)
    body = find_body(root)
    style = styles[body]
    width = style.width if style.width is not None else float(viewport[0])
    forced = style.height if style.height is not None else float(viewport[1])
    box = engine.layout(body, style, 0.0, 0.0, width, forced)
    return box, engine


def find_boxes(root: Box, predicate) -> list[Box]:
    return [box for box in root.walk() if predicate(box)]


def boxes_with_class(root: Box, name: str) -> list[Box]:
    return find_boxes(root, lambda box: name in box.class_set())


def serialize_box(box: Box) -> dict:
    return {
        "tag": box.tag,
        "class": box.classes,
        "kind": box.kind,
        "rect": {"x": round(box.x, 3), "y": round(box.y, 3),
                 "w": round(box.w, 3), "h": round(box.h, 3)},
        "flex": box.flex_info,
        "lines": [{"text": line.text, "x": round(line.x, 3),
                   "baseline": round(line.baseline, 3),
                   "width": round(line.width, 3), "face": line.face,
                   "size": line.size, "color": line.color}
                  for line in box.lines],
        "children": [serialize_box(child) for child in box.children],
    }
