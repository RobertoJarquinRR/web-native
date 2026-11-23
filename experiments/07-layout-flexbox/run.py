"""exp07-flex: HTML + CSS -> cascade -> row flex layout -> display list -> wgpu PNG.

Supported slice: display:flex, flex-direction:row, gap, justify-content
(flex-start | center | space-between), align-items (flex-start | center),
fixed px width/height and padding shorthand. No grow, shrink, wrap, column or
auto sizes; unsupported input raises LayoutError.
"""
import json, re, sys, time
from array import array
from dataclasses import dataclass, field
from importlib.metadata import version
from pathlib import Path

import html5lib, tinycss2, wgpu
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
FIXTURES, OUT, VIEWPORT, EPS = HERE / "fixtures", HERE / "out", (400, 120), 1e-9

class LayoutError(Exception):
    """Input outside the supported flex slice."""

@dataclass(frozen=True)
class Edges:
    top: float = 0.0; right: float = 0.0; bottom: float = 0.0; left: float = 0.0

@dataclass(frozen=True)
class Style:
    display: str = "block"; flex_direction: str = "row"
    justify_content: str = "flex-start"; align_items: str = "flex-start"
    width: float | None = None; height: float | None = None
    padding: Edges = Edges(); gap: float = 0.0; background: str | None = None

@dataclass
class Node:
    tag: str; attrs: dict = field(default_factory=dict)
    children: list = field(default_factory=list); parent: "Node | None" = None
    style: Style = Style()

@dataclass(frozen=True)
class Rect:
    x: float; y: float; width: float; height: float

@dataclass(frozen=True)
class FlexInfo:
    content: Rect; used: float; free: float
    start_offset: float; spacing: float; cross_offsets: tuple = ()

@dataclass(frozen=True)
class LayoutBox:
    tag: str; node_id: str | None; style: Style; rect: Rect
    flex: FlexInfo | None = None; children: tuple = ()

# ------------------------------------------------------------------ html/css

def parse_document(path: Path):
    tree = html5lib.parse(path.read_text(encoding="utf-8-sig"), treebuilder="etree",
                          namespaceHTMLElements=False)
    body = tree.find(".//body")
    if body is None:
        raise LayoutError(f"{path.name}: no <body>")
    links = [el.get("href") for el in tree.iter("link")
             if "stylesheet" in (el.get("rel") or "").lower().split()]

    def convert(el, parent):
        node = Node(str(el.tag).lower(), dict(el.attrib), parent=parent)
        node.children = [convert(c, node) for c in el if isinstance(c.tag, str)]
        return node

    return convert(body, None), links
def parse_selector(text: str):
    """Compound selector only: tag? + #id/.class, no combinators."""
    m = re.fullmatch(r"([a-zA-Z][\w-]*)?((?:[#.][\w-]+)*)", text.strip())
    if not m:
        return None
    tag = m.group(1).lower() if m.group(1) else None
    return (tag, re.findall(r"#([\w-]+)", m.group(2)),
            re.findall(r"\.([\w-]+)", m.group(2)))
def load_rules(path: Path):
    """tinycss2 -> rules with own specificity (ids, classes, tags) + source order."""
    sheet = tinycss2.parse_stylesheet(path.read_text(encoding="utf-8-sig"),
                                      skip_comments=True, skip_whitespace=True)
    rules, order = [], 0
    for rule in sheet:
        if rule.type != "qualified-rule":
            continue
        decls = {}
        for d in tinycss2.parse_declaration_list(rule.content, skip_comments=True,
                                                 skip_whitespace=True):
            if d.type == "declaration":
                decls[d.lower_name] = tinycss2.serialize(d.value).strip()
        for sel_text in tinycss2.serialize(rule.prelude).split(","):
            sel = parse_selector(sel_text)
            if sel:
                spec = (len(sel[1]), len(sel[2]), 1 if sel[0] else 0)
                rules.append({"sel": sel, "decls": decls, "spec": spec, "order": order})
                order += 1
    return rules

def selector_matches(el: Node, sel) -> bool:
    tag, ids, classes = sel
    return ((tag is None or el.tag == tag)
            and all(el.attrs.get("id") == i for i in ids)
            and all(c in set(el.attrs.get("class", "").split()) for c in classes))

def walk_nodes(node: Node):
    yield node
    for child in node.children:
        yield from walk_nodes(child)

def cascade(root: Node, rules: list):
    for el in walk_nodes(root):
        matched = sorted((r for r in rules if selector_matches(el, r["sel"])),
                         key=lambda r: (r["spec"], r["order"]))
        decls = {}
        for rule in matched:
            decls.update(rule["decls"])
        el.style = compute_style(decls)

# ------------------------------------------------------------- computed style
def px(value):
    m = re.fullmatch(r"\s*(-?\d+(?:\.\d+)?)px\s*", value or "")
    return float(m.group(1)) if m else None
def parse_color(value):
    value = (value or "").strip().lower()
    if re.fullmatch(r"#[0-9a-f]{3}", value):
        return "#" + "".join(c * 2 for c in value[1:])
    return value if re.fullmatch(r"#[0-9a-f]{6}", value) else None
def compute_style(decls):
    def keyword(prop, default):
        return (decls.get(prop) or default).strip().lower()

    width, height, gap = decls.get("width"), decls.get("height"), decls.get("gap")
    pad = px(decls["padding"]) if "padding" in decls else 0.0
    if pad is None:
        raise LayoutError(f"unsupported padding: {decls['padding']!r}")
    return Style(
        display=keyword("display", "block"),
        flex_direction=keyword("flex-direction", "row"),
        justify_content=keyword("justify-content", "flex-start"),
        align_items=keyword("align-items", "flex-start"),
        width=px(width) if width is not None else None,
        height=px(height) if height is not None else None,
        padding=Edges(pad, pad, pad, pad), gap=px(gap) if gap is not None else 0.0,
        background=parse_color(decls.get("background") or decls.get("background-color")))

# ----------------------------------------------------------------- flex layout
def content_rect(rect: Rect, padding: Edges) -> Rect:
    return Rect(rect.x + padding.left, rect.y + padding.top,
                rect.width - padding.left - padding.right,
                rect.height - padding.top - padding.bottom)

def flex_layout(content: Rect, children: list, parent: Style):
    """Pure row-axis placement for fixed-size children.

    used = sum(child widths) + gap * (n - 1); free = content.width - used.
    justify-content moves the origin (center) or expands the gaps
    (space-between); align-items offsets every item on the cross axis.
    """
    n, gap = len(children), parent.gap
    widths = [c.width for c in children]
    heights = [c.height for c in children]
    if any(v is None for v in widths + heights):
        raise LayoutError("flex children need fixed px width and height")
    if parent.justify_content not in ("flex-start", "center", "space-between"):
        raise LayoutError(f"unsupported justify-content: {parent.justify_content}")
    if parent.align_items not in ("flex-start", "center"):
        raise LayoutError(f"unsupported align-items: {parent.align_items}")

    used = sum(widths) + gap * max(n - 1, 0)
    free = content.width - used
    if free < -EPS:
        raise LayoutError(f"negative free space ({free}px): flex-shrink not implemented")
    free = max(free, 0.0)
    if parent.justify_content == "flex-start":
        start, spacing = 0.0, gap
    elif parent.justify_content == "center":
        start, spacing = free / 2.0, gap
    else:  # space-between: split the free space between adjacent items
        start, spacing = 0.0, gap + (free / (n - 1) if n > 1 else 0.0)

    rects, cross_offsets, cursor = [], [], content.x + start
    for width, height in zip(widths, heights):
        cross = content.y + ((content.height - height) / 2.0
                             if parent.align_items == "center" else 0.0)
        rects.append(Rect(cursor, cross, width, height))
        cross_offsets.append(cross - content.y)
        cursor += width + spacing
    return rects, FlexInfo(content, used, free, start, spacing, tuple(cross_offsets))

def layout_tree(node: Node, border: Rect) -> LayoutBox:
    """Pure: turn a styled Node subtree into a laid-out LayoutBox tree."""
    style = node.style
    box = LayoutBox(node.tag, node.attrs.get("id"), style, border)
    if not node.children:
        return box
    if style.display != "flex":
        raise LayoutError(f"<{node.tag}> has children but display:{style.display} is not supported")
    if style.flex_direction != "row":
        raise LayoutError(f"unsupported flex-direction: {style.flex_direction}")
    rects, info = flex_layout(content_rect(border, style.padding),
                              [c.style for c in node.children], style)
    kids = tuple(layout_tree(child, rect) for child, rect in zip(node.children, rects))
    return LayoutBox(node.tag, node.attrs.get("id"), style, border, info, kids)
def layout_document(root: Node, viewport=VIEWPORT) -> LayoutBox:
    style = root.style
    width = style.width if style.width is not None else float(viewport[0])
    height = style.height if style.height is not None else float(viewport[1])
    return layout_tree(root, Rect(0.0, 0.0, width, height))
def walk_boxes(box: LayoutBox):
    yield box
    for child in box.children:
        yield from walk_boxes(child)
def find_box(root: LayoutBox, node_id: str) -> LayoutBox:
    for box in walk_boxes(root):
        if box.node_id == node_id:
            return box
    raise LayoutError(f"missing box #{node_id}")

# --------------------------------------------------------------- display list

def px_int(value: float) -> int:
    rounded = int(round(value))
    if abs(value - rounded) > 1e-6:
        raise LayoutError(f"non-integral geometry in IR: {value}")
    return rounded

def build_display_list(root: LayoutBox, viewport=VIEWPORT) -> dict:
    ops = []
    for box in walk_boxes(root):
        if box.style.background:
            ops.append({"type": "rect", "x": px_int(box.rect.x), "y": px_int(box.rect.y),
                        "w": px_int(box.rect.width), "h": px_int(box.rect.height),
                        "fill": box.style.background})
    return {"viewport": {"width": viewport[0], "height": viewport[1]}, "ops": ops}

# ------------------------------------------------------------- wgpu renderer

WGSL = """
struct VSOut { @builtin(position) pos: vec4<f32>, @location(0) color: vec4<f32>, };
@vertex fn vs_main(@location(0) pos: vec2<f32>, @location(1) color: vec4<f32>) -> VSOut {
    return VSOut(vec4<f32>(pos, 0.0, 1.0), color);
}
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> { return in.color; }
"""
def rgb(color: str):
    return tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))
def rect_vertices(op, width, height):
    x, y, w, h = op["x"], op["y"], op["w"], op["h"]
    r, g, b = (c / 255.0 for c in rgb(op["fill"]))
    x0, x1 = x / width * 2 - 1, (x + w) / width * 2 - 1
    y0, y1 = 1 - y / height * 2, 1 - (y + h) / height * 2
    out = []
    for px_, py_ in ((x0, y0), (x0, y1), (x1, y1), (x0, y0), (x1, y1), (x1, y0)):
        out.extend((px_, py_, r, g, b, 1.0))
    return out

class WgpuRenderer:
    """One adapter/device/pipeline reused by every variant."""

    def __init__(self):
        adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
        if adapter is None:
            raise RuntimeError("no wgpu adapter available")
        self.info = {k: str(v) for k, v in dict(adapter.info).items()}
        self.device = adapter.request_device_sync()
        self.format = wgpu.TextureFormat.rgba8unorm
        shader = self.device.create_shader_module(code=WGSL)
        self.pipeline = self.device.create_render_pipeline(
            layout=wgpu.AutoLayoutMode.auto,
            vertex={"module": shader, "entry_point": "vs_main", "buffers": [{
                "array_stride": 24, "step_mode": wgpu.VertexStepMode.vertex,
                "attributes": [{"format": wgpu.VertexFormat.float32x2, "offset": 0, "shader_location": 0},
                               {"format": wgpu.VertexFormat.float32x4, "offset": 8, "shader_location": 1}]}]},
            primitive={"topology": wgpu.PrimitiveTopology.triangle_list},
            fragment={"module": shader, "entry_point": "fs_main", "targets": [{"format": self.format}]})

    def render(self, ops, width, height, png_path: Path):
        device = self.device
        texture = device.create_texture(
            size=(width, height, 1), format=self.format,
            usage=wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.COPY_SRC)
        verts = [v for op in ops for v in rect_vertices(op, width, height)]
        vbuf = device.create_buffer_with_data(data=array("f", verts),
                                             usage=wgpu.BufferUsage.VERTEX)
        encoder = device.create_command_encoder()
        rpass = encoder.begin_render_pass(color_attachments=[{
            "view": texture.create_view(), "clear_value": (0.0, 0.0, 0.0, 1.0),
            "load_op": wgpu.LoadOp.clear, "store_op": wgpu.StoreOp.store}])
        rpass.set_pipeline(self.pipeline)
        rpass.set_vertex_buffer(0, vbuf)
        rpass.draw(vertex_count=len(verts) // 6)
        rpass.end()

        bytes_per_row = ((width * 4 + 255) // 256) * 256  # 256-byte row alignment
        readback = device.create_buffer(size=bytes_per_row * height,
                                        usage=wgpu.BufferUsage.COPY_DST | wgpu.BufferUsage.MAP_READ)
        encoder.copy_texture_to_buffer(
            {"texture": texture, "mip_level": 0, "origin": (0, 0, 0)},
            {"buffer": readback, "offset": 0, "bytes_per_row": bytes_per_row,
             "rows_per_image": height}, (width, height, 1))
        device.queue.submit([encoder.finish()])
        readback.map_sync(wgpu.MapMode.READ)
        data = bytes(readback.read_mapped())
        readback.unmap()
        rows = [data[i * bytes_per_row:i * bytes_per_row + width * 4] for i in range(height)]
        png_path.parent.mkdir(parents=True, exist_ok=True)
        Image.frombytes("RGBA", (width, height), b"".join(rows)).save(png_path)

def render_pillow(ops, width, height, png_path: Path):
    img = Image.new("RGB", (width, height), (0, 0, 0))
    draw = ImageDraw.Draw(img)
    for op in ops:
        draw.rectangle([op["x"], op["y"], op["x"] + op["w"] - 1, op["y"] + op["h"] - 1],
                       fill=rgb(op["fill"]))
    png_path.parent.mkdir(parents=True, exist_ok=True)
    img.save(png_path)

# --------------------------------------------------------------- verification

_EXPECTED = json.loads((FIXTURES / "expected.json").read_text(encoding="utf-8-sig"))
COLORS, EXPECTED = _EXPECTED["colors"], _EXPECTED["variants"]
def num(value):
    value = round(float(value), 6)
    return int(value) if value.is_integer() else value
def rect_json(rect: Rect):
    return {"x": num(rect.x), "y": num(rect.y), "w": num(rect.width), "h": num(rect.height)}

def verify_variant(name, layout_root, display, png_path, backend):
    exp, checks = EXPECTED[name], []

    def add(kind, check, expected, got, point=None, delta=None):
        ok = (delta <= 8) if delta is not None else expected == got
        checks.append({"kind": kind, "check": check, "expected": expected, "got": got,
                       "point": point, "delta": delta, "pass": ok})

    info = find_box(layout_root, "root").flex
    got_flex = {"content": rect_json(info.content), "used": num(info.used),
                "free": num(info.free), "start_offset": num(info.start_offset),
                "spacing": num(info.spacing),
                "cross_offsets": [num(v) for v in info.cross_offsets]}
    exp_flex = {"content": {"x": 20, "y": 20, "w": 360, "h": 80}, "used": 272,
                "free": 88, "start_offset": exp["start"], "spacing": exp["spacing"],
                "cross_offsets": [exp["cross"]] * 3}
    add("flex", "free-space distribution", exp_flex, got_flex)

    for bid in ("a", "b", "c"):
        got = rect_json(find_box(layout_root, bid).rect)
        expected = dict(zip(("x", "y", "w", "h"), exp["boxes"][bid]))
        add("geometry", f"box {bid} rect", expected, got)

    expected_ops = [{"type": "rect", "x": 0, "y": 0, "w": VIEWPORT[0], "h": VIEWPORT[1],
                     "fill": COLORS["root"]}]
    for bid in ("a", "b", "c"):
        x, y, w, h = exp["boxes"][bid]
        expected_ops.append({"type": "rect", "x": x, "y": y, "w": w, "h": h,
                             "fill": COLORS[bid]})
    add("display-list", "ops (root, a, b, c)", expected_ops, display["ops"])

    img = Image.open(png_path).convert("RGB")
    add("pixel", "png size", list(VIEWPORT), list(img.size))
    ax, ay, aw, ah = exp["boxes"]["a"]
    bx = exp["boxes"]["b"][0]
    samples = [(4, 4, COLORS["root"], "root corner"),
               (VIEWPORT[0] // 2, VIEWPORT[1] - 4, COLORS["root"], "root bottom band"),
               (ax + aw + (bx - (ax + aw)) // 2, ay + ah // 2, COLORS["root"], "gap a-b")]
    for bid in ("a", "b", "c"):
        x, y, w, h = exp["boxes"][bid]
        samples.append((x + w // 2, y + h // 2, COLORS[bid], f"box {bid} center"))
    for x, y, want, label in samples:
        got = img.getpixel((x, y))
        delta = max(abs(a - b) for a, b in zip(got, rgb(want)))
        add("pixel", label, want, "#%02x%02x%02x" % got, [x, y], delta)

    return {"name": name, "backend": backend, "justify": exp["justify"],
            "align": exp["align"], "pass": all(c["pass"] for c in checks), "checks": checks}
def print_variant(result, display_path, png_path):
    checks = result["checks"]
    geom = [c for c in checks if c["kind"] == "geometry"]
    pix = [c for c in checks if c["kind"] == "pixel" and c["point"] is not None]
    flex, display_ok = checks[0], next(c["pass"] for c in checks if c["kind"] == "display-list")
    print(f"[{result['name']}] {result['justify']}/{result['align']} "
          f"free={flex['got']['free']} start={flex['got']['start_offset']} "
          f"spacing={flex['got']['spacing']}")
    print("  " + " ".join(f"{c['check'][4]}=({c['got']['x']},{c['got']['y']},"
                          f"{c['got']['w']},{c['got']['h']})" for c in geom))
    print(f"  geometry {sum(c['pass'] for c in geom)}/{len(geom)} exact | "
          f"IR {'OK' if display_ok else 'FAIL'} | "
          f"pixels {sum(c['pass'] for c in pix)}/{len(pix)} | "
          f"{'PASS' if result['pass'] else 'FAIL'}")
    print(f"  out: {display_path.relative_to(HERE)} + {png_path.relative_to(HERE)}")

# ----------------------------------------------------------------------- main

def main() -> int:
    started = time.perf_counter()
    OUT.mkdir(exist_ok=True)
    timings = {k: 0.0 for k in ("parse_html", "parse_css", "cascade", "layout",
                                "display_list", "render", "verify")}
    variants, all_pass = [], True

    t = time.perf_counter()
    renderer, render_error = None, None
    try:
        renderer = WgpuRenderer()
        adapter = {k: renderer.info.get(k) for k in
                   ("device", "backend_type", "adapter_type", "vendor_id")}
        backend = f"wgpu/{adapter['backend_type']}"
        print(f"exp07-flex - 4 row-flex fixtures | backend {backend}")
        print(f"  adapter: {adapter['device']} | type: {adapter['adapter_type']} | "
              f"vendor: {adapter['vendor_id']}")
    except Exception as exc:
        adapter, render_error = None, f"{type(exc).__name__}: {exc}"
        backend = "pillow-fallback"
        print(f"exp07-flex - 4 row-flex fixtures | wgpu unavailable: {render_error}")
        print("  falling back to Pillow rasterization")
    timings["wgpu_init"] = round((time.perf_counter() - t) * 1000, 2)

    for name in ("v1", "v2", "v3", "v4"):
        variant_dir = OUT / name
        display_path, png_path = variant_dir / "display_list.json", variant_dir / "render.png"
        variant_dir.mkdir(parents=True, exist_ok=True)
        try:
            t = time.perf_counter()
            html_path = FIXTURES / f"{name}.html"
            body, links = parse_document(html_path)
            timings["parse_html"] += (time.perf_counter() - t) * 1000
            if not links:
                raise LayoutError(f"{html_path.name}: no stylesheet <link>")

            t = time.perf_counter()
            rules = load_rules(html_path.parent / links[0])
            timings["parse_css"] += (time.perf_counter() - t) * 1000
            t = time.perf_counter()
            cascade(body, rules)
            timings["cascade"] += (time.perf_counter() - t) * 1000

            root_node = body.children[0] if len(body.children) == 1 else body
            t = time.perf_counter()
            layout_root = layout_document(root_node)
            timings["layout"] += (time.perf_counter() - t) * 1000

            t = time.perf_counter()
            display = build_display_list(layout_root)
            display_path.write_text(json.dumps(display, indent=2), encoding="utf-8")
            timings["display_list"] += (time.perf_counter() - t) * 1000

            t = time.perf_counter()
            variant_backend = backend
            if renderer is not None:
                try:
                    renderer.render(display["ops"], *VIEWPORT, png_path)
                except Exception as exc:
                    render_error = f"{type(exc).__name__}: {exc}"
                    renderer, variant_backend = None, "pillow-fallback"
                    render_pillow(display["ops"], *VIEWPORT, png_path)
            else:
                variant_backend = "pillow-fallback"
                render_pillow(display["ops"], *VIEWPORT, png_path)
            timings["render"] += (time.perf_counter() - t) * 1000

            t = time.perf_counter()
            result = verify_variant(name, layout_root, display, png_path, variant_backend)
            timings["verify"] += (time.perf_counter() - t) * 1000
        except Exception as exc:
            all_pass = False
            result = {"name": name, "backend": backend, "pass": False,
                      "error": f"{type(exc).__name__}: {exc}"}
            print(f"[{name}] ERROR {result['error']}")
            variants.append(result)
            continue

        all_pass = all_pass and result["pass"]
        variants.append(result)
        print_variant(result, display_path, png_path)

    timings = {k: round(v, 2) for k, v in timings.items()}
    timings["total"] = round((time.perf_counter() - started) * 1000, 2)
    used_fallback = any(v.get("backend") == "pillow-fallback" for v in variants)
    failed = any(not v["pass"] for v in variants)
    status = "FAILED" if failed else ("PARTIAL" if used_fallback else "SUCCESS")
    report = {
        "status": status, "experiment": "exp07-flex",
        "viewport": {"width": VIEWPORT[0], "height": VIEWPORT[1]},
        "backend": backend, "adapter": adapter, "render_error": render_error,
        "versions": {"python": sys.version.split()[0], "html5lib": version("html5lib"),
                     "tinycss2": version("tinycss2"), "wgpu": version("wgpu"),
                     "pillow": version("Pillow")},
        "timings_ms": timings, "variants": variants,
    }
    (HERE / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("\ntimings (ms): " + ", ".join(f"{k}={v}" for k, v in timings.items()))
    if render_error:
        print(f"render error: {render_error}")
    print(f"STATUS: {status} ({sum(v['pass'] for v in variants)}/4 variants)")
    return 0 if not failed else 1

if __name__ == "__main__":
    sys.exit(main())
