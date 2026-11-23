"""exp04: HTML + CSS -> display list IR -> native GPU render (wgpu)."""
import json, re, sys, time
from array import array
from dataclasses import dataclass, field
from pathlib import Path

import html5lib, tinycss2, wgpu
from PIL import Image

HERE = Path(__file__).resolve().parent
VIEWPORT = (400, 300)

@dataclass
class El:
    tag: str
    attrs: dict
    children: list = field(default_factory=list)
    parent: "El | None" = None
    style: dict = field(default_factory=dict)

def build_el(node, parent=None):
    el = El(str(node.tag), dict(node.attrib), parent=parent)
    for child in node:
        if isinstance(child.tag, str):
            el.children.append(build_el(child, el))
    return el

def parse_html(path):
    tree = html5lib.parse(path.read_text(encoding="utf-8"), treebuilder="etree",
                          namespaceHTMLElements=False)
    return build_el(tree)

def parse_selector(text):
    compounds = []
    for chunk in text.split():
        ids, classes, tags = set(), set(), []
        tokens = re.findall(r"[#.]?[\w-]+|\*", chunk)
        if "".join(tokens) != chunk:
            return None
        for tok in tokens:
            if tok == "*":
                continue
            elif tok[0] == "#":
                ids.add(tok[1:])
            elif tok[0] == ".":
                classes.add(tok[1:])
            else:
                tags.append(tok.lower())
        compounds.append((ids, classes, tags))
    return compounds or None

def load_rules(path):
    sheet = tinycss2.parse_stylesheet(path.read_text(encoding="utf-8"),
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
            compounds = parse_selector(sel_text)
            if compounds is not None:
                spec = tuple(sum(len(c[i]) for c in compounds) for i in range(3))
                rules.append({"sel": compounds, "decls": decls,
                              "spec": spec, "order": order})
                order += 1
    return rules

def compound_matches(el, compound):
    ids, classes, tags = compound
    if ids and el.attrs.get("id") not in ids:
        return False
    if classes and not classes.issubset(set(el.attrs.get("class", "").split())):
        return False
    if tags and el.tag not in tags:
        return False
    return True

def selector_matches(el, compounds):
    idx = len(compounds) - 1
    if not compound_matches(el, compounds[idx]):
        return False
    node, idx = el.parent, idx - 1
    while idx >= 0:  # descendant combinators, right-to-left
        while node is not None and not compound_matches(node, compounds[idx]):
            node = node.parent
        if node is None:
            return False
        node, idx = node.parent, idx - 1
    return True

def cascade(root, rules):
    def walk(el):
        matched = [(r["spec"], r["order"], r["decls"]) for r in rules
                   if selector_matches(el, r["sel"])]
        matched.sort(key=lambda t: (t[0], t[1]))  # specificity, then source order
        for _, _, decls in matched:
            el.style.update(decls)
        for child in el.children:
            walk(child)
    walk(root)

def px(value, default=0.0):
    m = re.fullmatch(r"\s*(-?\d+(?:\.\d+)?)px\s*", value or "")
    return float(m.group(1)) if m else default

def sides(style, prop):
    val = style.get(prop)
    if val:
        v = px(val)
        return (v, v, v, v)
    return tuple(px(style.get(f"{prop}-{s}")) for s in ("top", "right", "bottom", "left"))

def parse_color(value):
    value = (value or "").strip().lower()
    if re.fullmatch(r"#[0-9a-f]{3}", value):
        return tuple(int(c * 2, 16) for c in value[1:])
    if re.fullmatch(r"#[0-9a-f]{6}", value):
        return tuple(int(value[i:i + 2], 16) for i in (1, 3, 5))
    return None

def layout(el, x, y, avail_w, out):
    style = el.style
    w = px(style["width"]) if "width" in style else avail_w
    h = px(style["height"]) if "height" in style else None
    pad = sides(style, "padding")
    bg = parse_color(style.get("background") or style.get("background-color"))
    if bg is not None and h is not None:
        out.append({"type": "rect", "x": round(x), "y": round(y), "w": round(w),
                    "h": round(h), "fill": "#%02x%02x%02x" % bg})
    cursor = y + pad[0]
    for child in el.children:
        if child.tag in ("head", "script", "style", "link", "meta", "title"):
            continue
        m = sides(child.style, "margin")
        child_y = cursor + m[0]
        used = layout(child, x + pad[3] + m[3], child_y, w - pad[1] - pad[3], out)
        cursor = child_y + used + m[2]
    return h if h is not None else pad[0] + (cursor - (y + pad[0])) + pad[2]

WGSL = """
struct VSOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) color: vec4<f32>,
};
@vertex
fn vs_main(@location(0) pos: vec2<f32>, @location(1) color: vec4<f32>) -> VSOut {
    var out: VSOut;
    out.pos = vec4<f32>(pos, 0.0, 1.0);
    out.color = color;
    return out;
}
@fragment
fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
    return in.color;
}
"""

def rect_vertices(rect, width, height):
    x, y, w, h = rect["x"], rect["y"], rect["w"], rect["h"]
    r, g, b = (c / 255.0 for c in parse_color(rect["fill"]))
    x0, x1 = x / width * 2 - 1, (x + w) / width * 2 - 1
    y0, y1 = 1 - y / height * 2, 1 - (y + h) / height * 2
    out = []
    for px_, py_ in ((x0, y0), (x0, y1), (x1, y1), (x0, y0), (x1, y1), (x1, y0)):
        out.extend((px_, py_, r, g, b, 1.0))
    return out

def render_gpu(ops, width, height):
    adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
    info = adapter.info
    print(f"[wgpu] adapter: {info['device']} | backend: {info['backend_type']} | "
          f"type: {info['adapter_type']} | vendor_id: {info['vendor_id']}")
    device = adapter.request_device_sync()

    fmt = wgpu.TextureFormat.rgba8unorm
    texture = device.create_texture(size=(width, height, 1), format=fmt,
                                    usage=wgpu.TextureUsage.RENDER_ATTACHMENT
                                    | wgpu.TextureUsage.COPY_SRC)
    # ops[0] covers the viewport: use it as pass clear color; draw the rest.
    clear = tuple(c / 255.0 for c in parse_color(ops[0]["fill"])) + (1.0,)
    verts = []
    for op in ops[1:]:
        verts.extend(rect_vertices(op, width, height))
    vbuf = device.create_buffer_with_data(data=array("f", verts),
                                          usage=wgpu.BufferUsage.VERTEX)

    shader = device.create_shader_module(code=WGSL)
    pipeline = device.create_render_pipeline(
        layout=wgpu.AutoLayoutMode.auto,
        vertex={
            "module": shader, "entry_point": "vs_main",
            "buffers": [{
                "array_stride": 24, "step_mode": wgpu.VertexStepMode.vertex,
                "attributes": [
                    {"format": wgpu.VertexFormat.float32x2, "offset": 0, "shader_location": 0},
                    {"format": wgpu.VertexFormat.float32x4, "offset": 8, "shader_location": 1},
                ],
            }],
        },
        primitive={"topology": wgpu.PrimitiveTopology.triangle_list},
        fragment={"module": shader, "entry_point": "fs_main",
                  "targets": [{"format": fmt}]},
    )

    encoder = device.create_command_encoder()
    rpass = encoder.begin_render_pass(color_attachments=[{
        "view": texture.create_view(), "clear_value": clear,
        "load_op": wgpu.LoadOp.clear, "store_op": wgpu.StoreOp.store,
    }])
    rpass.set_pipeline(pipeline)
    rpass.set_vertex_buffer(0, vbuf)
    rpass.draw(vertex_count=len(verts) // 6)  # 1 draw call, 2 triangles per rect
    rpass.end()

    bytes_per_row = ((width * 4 + 255) // 256) * 256  # 256-byte row alignment
    readback = device.create_buffer(size=bytes_per_row * height,
                                    usage=wgpu.BufferUsage.COPY_DST
                                    | wgpu.BufferUsage.MAP_READ)
    encoder.copy_texture_to_buffer(
        {"texture": texture, "mip_level": 0, "origin": (0, 0, 0)},
        {"buffer": readback, "offset": 0, "bytes_per_row": bytes_per_row,
         "rows_per_image": height},
        (width, height, 1))
    device.queue.submit([encoder.finish()])
    readback.map_sync(wgpu.MapMode.READ)
    data = bytes(readback.read_mapped())
    readback.unmap()

    rows = [data[i * bytes_per_row:i * bytes_per_row + width * 4] for i in range(height)]
    Image.frombytes("RGBA", (width, height), b"".join(rows)).save(HERE / "render.png")

def verify():
    checks = [((60, 60), (0xe7, 0x4c, 0x3c), "box a"),
              ((60, 156), (0x34, 0x98, 0xdb), "box b"),
              ((200, 250), (0x10, 0x10, 0x14), "root")]
    img = Image.open(HERE / "render.png").convert("RGBA")
    ok = img.size == VIEWPORT
    print(f"[verify] size: {img.size} (expected {VIEWPORT}) -> {'OK' if ok else 'FAIL'}")
    for (x, y), expected, name in checks:
        got = img.getpixel((x, y))[:3]
        diff = max(abs(a - b) for a, b in zip(got, expected))
        good = diff <= 8
        ok = ok and good
        print(f"[verify] ({x},{y}) {name}: got #{got[0]:02x}{got[1]:02x}{got[2]:02x} "
              f"expected #{expected[0]:02x}{expected[1]:02x}{expected[2]:02x} "
              f"maxdiff={diff} -> {'OK' if good else 'FAIL'}")
    return ok

def main():
    t_start = time.perf_counter()
    timings = {}

    t = time.perf_counter()
    root = parse_html(HERE / "input.html")
    timings["html5lib parse"] = (time.perf_counter() - t) * 1000

    t = time.perf_counter()
    rules = load_rules(HERE / "input.css")
    timings["tinycss2 parse"] = (time.perf_counter() - t) * 1000

    t = time.perf_counter()
    cascade(root, rules)
    timings["cascade"] = (time.perf_counter() - t) * 1000
    body = next((el for el in root.children if el.tag == "body"), None)
    print(f"[css] rules: {len(rules)} | body style: {body.style if body else None}")

    t = time.perf_counter()
    ops = []
    layout(root, 0, 0, VIEWPORT[0], ops)
    doc = {"viewport": {"width": VIEWPORT[0], "height": VIEWPORT[1]}, "ops": ops}
    timings["layout + display list"] = (time.perf_counter() - t) * 1000
    (HERE / "display_list.json").write_text(json.dumps(doc, indent=2), encoding="utf-8")
    print("[ir] display_list.json:", json.dumps(doc["ops"]))

    t = time.perf_counter()
    render_gpu(doc["ops"], VIEWPORT[0], VIEWPORT[1])
    timings["wgpu render + readback + png"] = (time.perf_counter() - t) * 1000

    t = time.perf_counter()
    ok = verify()
    timings["verify"] = (time.perf_counter() - t) * 1000
    timings["TOTAL"] = (time.perf_counter() - t_start) * 1000

    print("[timings]")
    for name, ms in timings.items():
        print(f"    {name:<32} {ms:7.1f} ms")
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
