"""HTML/CSS -> display list.

Native engines: selectolax (Lexbor, C) for HTML, lightningcss (Rust) for CSS.
Cascade, block layout and display-list emission are implemented here.
Scope: tag/id/class compound selectors, single-value margin/padding shorthand,
px lengths, hex backgrounds, block flow with margin-top.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

import lightningcss
from selectolax.lexbor import LexborHTMLParser

VIEWPORT = (400, 300)


@dataclass
class El:
    tag: str
    attrs: dict[str, str]
    children: list["El"] = field(default_factory=list)
    decl: dict[str, str] = field(default_factory=dict)
    rect: tuple[float, float, float, float] | None = None


@dataclass
class Rule:
    selector: str
    spec: tuple[int, int, int]
    order: int
    decl: dict[str, str]


def parse_html(src: str) -> El:
    doc = LexborHTMLParser(src)

    def build(node) -> El:
        el = El(node.tag, {k: v or "" for k, v in node.attributes.items()})
        child = node.child
        while child:
            if child.is_element_node:
                el.children.append(build(child))
            child = child.next
        return el

    return build(doc.body)


def native_css(css_text: str) -> str:
    """Run the Rust parser/transformer; returns minified, normalized CSS."""
    return lightningcss.process_stylesheet(css_text, filename="input.css", minify=True)


_RULE = re.compile(r"([^{}]+)\{([^{}]*)\}")
_TOKEN = re.compile(r"([#.]?)([\w-]+)")


def parse_rules(css_min: str) -> list[Rule]:
    rules: list[Rule] = []
    for order, m in enumerate(_RULE.finditer(css_min)):
        decl = {}
        for part in m.group(2).split(";"):
            if ":" in part:
                k, v = part.split(":", 1)
                decl[k.strip()] = v.strip()
        for sel in m.group(1).split(","):
            sel = sel.strip()
            toks = _TOKEN.findall(sel)
            spec = (sum(p == "#" for p, _ in toks),
                    sum(p == "." for p, _ in toks),
                    sum(p == "" for p, _ in toks))
            rules.append(Rule(sel, spec, order, decl))
    return rules


def matches(el: El, sel: str) -> bool:
    for prefix, name in _TOKEN.findall(sel):
        if prefix == "#" and el.attrs.get("id") != name:
            return False
        if prefix == "." and name not in el.attrs.get("class", "").split():
            return False
        if prefix == "" and el.tag != name:
            return False
    return True


def cascade(root: El, rules: list[Rule]) -> int:
    matched_count = 0
    stack = [root]
    while stack:
        el = stack.pop()
        matched = [r for r in rules if matches(el, r.selector)]
        matched.sort(key=lambda r: (r.spec, r.order))
        matched_count += len(matched)
        for rule in matched:
            el.decl.update(rule.decl)
        stack.extend(el.children)
    return matched_count


def px(value: str | None, default: float = 0.0) -> float:
    if value is None:
        return default
    value = value.strip()
    if value.endswith("px"):
        return float(value[:-2])
    if re.fullmatch(r"-?\d+(\.\d+)?", value):
        return float(value)
    return default


def box_shorthand(decl: dict[str, str], prop: str) -> tuple[float, float, float, float]:
    v = decl.get(prop)
    parts = [px(p) for p in v.split()] if v else []
    if len(parts) == 1:
        t = r = b = l = parts[0]
    elif len(parts) == 2:
        t = b = parts[0]
        r = l = parts[1]
    elif len(parts) == 4:
        t, r, b, l = parts
    else:
        t = r = b = l = 0.0
    sides = [t, r, b, l]
    for i, side in enumerate(("top", "right", "bottom", "left")):
        if f"{prop}-{side}" in decl:
            sides[i] = px(decl[f"{prop}-{side}"])
    return (sides[0], sides[1], sides[2], sides[3])


def color(value: str | None) -> str | None:
    m = re.fullmatch(r"#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})", (value or "").strip())
    if not m:
        return None
    h = m.group(1)
    if len(h) == 3:
        h = "".join(c * 2 for c in h)
    return "#" + h.lower()


def layout(root: El, size: tuple[int, int]) -> list[El]:
    """Block layout. Body is viewport-sized; width/height are border-box."""
    w_view, h_view = size
    order: list[El] = []

    def place(el: El, x: float, y: float, avail_w: float) -> float:
        d = el.decl
        mt, mr, mb, ml = box_shorthand(d, "margin")
        pt, pr, pb, pl = box_shorthand(d, "padding")
        w = px(d.get("width"), avail_w - ml - mr) if "width" in d else avail_w - ml - mr
        h = px(d.get("height"), 0.0) if "height" in d else 0.0
        bx, by = x + ml, y + mt
        el.rect = (bx, by, w, h)
        order.append(el)
        cursor = by + pt
        for child in el.children:
            cursor = place(child, bx + pl, cursor, w - pl - pr)
        return by + h + mb

    root.rect = (0.0, 0.0, float(w_view), float(h_view))
    order.append(root)
    cursor = 0.0
    for child in root.children:
        cursor = place(child, 0.0, cursor, float(w_view))
    return order


def num(v: float) -> int | float:
    return int(v) if float(v).is_integer() else round(float(v), 3)


def display_list(ordered: list[El]) -> dict:
    ops = []
    for el in ordered:
        fill = color(el.decl.get("background"))
        if not el.rect or not fill:
            continue
        x, y, w, h = el.rect
        if w <= 0 or h <= 0:
            continue
        ops.append({"type": "rect", "x": num(x), "y": num(y),
                    "w": num(w), "h": num(h), "fill": fill})
    return {"viewport": {"width": VIEWPORT[0], "height": VIEWPORT[1]}, "ops": ops}
