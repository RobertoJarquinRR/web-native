"""exp06-text: HTML+CSS -> line layout -> HarfBuzz -> FreeType atlas -> wgpu."""
import json, os, re, sys, time
from array import array
from dataclasses import dataclass, field
from pathlib import Path

import freetype, html5lib, tinycss2, uharfbuzz as hb, wgpu
from PIL import Image

HERE = Path(__file__).resolve().parent
VIEWPORT = (400, 200)
ATLAS_SIZE = 512
SKIP_TAGS = {"head", "script", "style", "link", "meta", "title", "base"}
INHERITED = ("font-family", "font-size", "color")

# ------------------------------------------------------------------ 1. HTML/CSS
@dataclass
class El:
    tag: str
    attrs: dict
    text: str = ""
    children: list = field(default_factory=list)
    parent: "El | None" = None
    style: dict = field(default_factory=dict)
def build_el(node, parent=None):
    el = El(str(node.tag), dict(node.attrib), node.text or "", parent=parent)
    for child in node:
        if isinstance(child.tag, str):
            el.children.append(build_el(child, el))
    return el
def parse_html(path):
    tree = html5lib.parse(path.read_text(encoding="utf-8"), treebuilder="etree",
                          namespaceHTMLElements=False)
    return build_el(tree)
def parse_selector(text):
    out = []
    for chunk in text.split():
        ids, classes, tags = set(), set(), []
        toks = re.findall(r"[#.]?[\w-]+|\*", chunk)
        if "".join(toks) != chunk: return None
        for t in toks:
            if t == "*": continue
            if t[0] == "#": ids.add(t[1:])
            elif t[0] == ".": classes.add(t[1:])
            else: tags.append(t.lower())
        out.append((ids, classes, tags))
    return out or None
def load_rules(path):
    sheet = tinycss2.parse_stylesheet(path.read_text(encoding="utf-8"),
                                      skip_comments=True, skip_whitespace=True)
    rules, order = [], 0
    for rule in sheet:
        if rule.type != "qualified-rule": continue
        decls = {}
        for d in tinycss2.parse_declaration_list(rule.content, skip_comments=True,
                                                 skip_whitespace=True):
            if d.type == "declaration": decls[d.lower_name] = tinycss2.serialize(d.value).strip()
        for sel in tinycss2.serialize(rule.prelude).split(","):
            c = parse_selector(sel)
            if c is None: continue
            rules.append({"sel": c, "decls": decls, "order": order,
                          "spec": tuple(sum(len(x[i]) for x in c) for i in range(3))})
            order += 1
    return rules
def compound_matches(el, compound):
    ids, classes, tags = compound
    if ids and el.attrs.get("id") not in ids: return False
    if classes and not classes.issubset(set(el.attrs.get("class", "").split())): return False
    return not tags or el.tag in tags
def selector_matches(el, compounds):
    idx = len(compounds) - 1
    if not compound_matches(el, compounds[idx]): return False
    node, idx = el.parent, idx - 1
    while idx >= 0:
        while node is not None and not compound_matches(node, compounds[idx]): node = node.parent
        if node is None: return False
        node, idx = node.parent, idx - 1
    return True
def cascade(root, rules):
    def walk(el, inherited):
        style = dict(inherited)
        for r in sorted(rules, key=lambda r: (r["spec"], r["order"])):
            if selector_matches(el, r["sel"]): style.update(r["decls"])
        el.style = style
        inherited = {k: style[k] for k in INHERITED if k in style}
        for child in el.children: walk(child, inherited)
    walk(root, {})
def px(value, default=0.0):
    m = re.fullmatch(r"\s*(-?\d+(?:\.\d+)?)px\s*", value or "")
    return float(m.group(1)) if m else default
def sides(style, prop):
    if style.get(prop):
        v = px(style[prop])
        return (v, v, v, v)
    return tuple(px(style.get(f"{prop}-{s}"))
                 for s in ("top", "right", "bottom", "left"))
def parse_color(value):
    value = (value or "").strip().lower()
    if re.fullmatch(r"#[0-9a-f]{3}", value):
        return tuple(int(c * 2, 16) for c in value[1:])
    if re.fullmatch(r"#[0-9a-f]{6}", value):
        return tuple(int(value[i:i + 2], 16) for i in (1, 3, 5))
    return None
def hex_color(rgb):
    return "#%02x%02x%02x" % rgb

# -------------------------------------------------------------------- 2. Fonts
def pick_font_file():
    fonts = Path(os.environ.get("WINDIR", r"C:\Windows")) / "Fonts"
    preferred, fallback = fonts / "segoeui.ttf", fonts / "arial.ttf"
    if preferred.exists():
        return preferred
    if fallback.exists():
        print(f"[font] WARNING: {preferred} missing, fallback {fallback}")
        return fallback
    raise SystemExit(f"[font] no usable font: tried {preferred} and {fallback}")
class Fonts:
    def __init__(self):
        self.path = pick_font_file()
        self.ft = freetype.Face(str(self.path))
        self.hb_face = hb.Face(self.path.read_bytes())
        self._hb, self._metrics = {}, {}

    def family(self, style):
        raw = style.get("font-family", "Segoe UI")
        return raw.split(",")[0].strip().strip("\"'") or "Segoe UI"

    def size(self, style): return px(style.get("font-size"), 16.0)

    def hb_font(self, size):
        size = round(size, 2)
        if size not in self._hb:
            font = hb.Font(self.hb_face)
            hb.ot_font_set_funcs(font)
            font.scale = (int(round(size * 64)),) * 2
            self._hb[size] = font
        return self._hb[size]

    def metrics(self, size):
        key = round(size, 2)
        if key not in self._metrics:
            self.ft.set_pixel_sizes(0, max(1, int(round(size))))
            self._metrics[key] = (self.ft.size.ascender / 64.0,
                                  -self.ft.size.descender / 64.0)
        return self._metrics[key]

    def shape(self, text, size):
        buf = hb.Buffer()
        buf.add_str(text)
        buf.direction, buf.script, buf.language = "ltr", "Latn", "en"
        hb.shape(self.hb_font(size), buf)
        return [{"gid": i.codepoint, "cluster": i.cluster,
                 "adv": p.x_advance / 64.0, "dx": p.x_offset / 64.0,
                 "dy": p.y_offset / 64.0}
                for i, p in zip(buf.glyph_infos, buf.glyph_positions)]

    def raster(self, gid, size):
        """FreeType grayscale bitmap: (w, rows, left, top, row_bytes)."""
        self.ft.set_pixel_sizes(0, max(1, int(round(size))))
        flags = freetype.FT_LOAD_RENDER | getattr(freetype, "FT_LOAD_TARGET_NORMAL", 0)
        self.ft.load_glyph(gid, flags)
        glyph, bmp = self.ft.glyph, self.ft.glyph.bitmap
        w, rows, pitch = bmp.width, bmp.rows, bmp.pitch
        if w == 0 or rows == 0: return 0, 0, glyph.bitmap_left, glyph.bitmap_top, []
        raw = bytes(bmp.buffer)
        return w, rows, glyph.bitmap_left, glyph.bitmap_top, \
            [raw[r * pitch:r * pitch + w] for r in range(rows)]
class Atlas:
    def __init__(self, size=ATLAS_SIZE):
        self.size, self.data, self.rects = size, bytearray(size * size), {}
        self.shelf_x = self.shelf_y = self.shelf_h = 0

    def add(self, key, w, h, lines):
        if w == 0 or h == 0:
            self.rects[key] = None
            return
        pad_w, pad_h = w + 2, h + 2
        if self.shelf_x + pad_w > self.size:
            self.shelf_x, self.shelf_y = 0, self.shelf_y + self.shelf_h
            self.shelf_h = 0
        if self.shelf_y + pad_h > self.size: raise RuntimeError(f"atlas overflow at {key}")
        x, y = self.shelf_x + 1, self.shelf_y + 1
        for r, line in enumerate(lines):
            off = (y + r) * self.size + x
            self.data[off:off + w] = line
        self.rects[key] = (x, y, w, h)
        self.shelf_x += pad_w
        self.shelf_h = max(self.shelf_h, pad_h)

    def uv(self, key):
        rect = self.rects.get(key)
        if not rect: return (0.0, 0.0, 0.0, 0.0)
        x, y, w, h = rect
        s = float(self.size)
        return (x / s, y / s, (x + w) / s, (y + h) / s)

# ------------------------------------------------------------ 3. Layout + IR
def layout(el, x, y, avail_w, rects, runs, fonts):
    style = el.style
    w = px(style["width"]) if "width" in style else avail_w
    h = px(style["height"]) if "height" in style else None
    pad = sides(style, "padding")
    content_x, content_y = x + pad[3], y + pad[0]
    sub_rects, sub_runs = [], []
    text = " ".join(el.text.split())
    if text and not any(c.tag not in SKIP_TAGS for c in el.children):
        size = fonts.size(style)
        asc, desc = fonts.metrics(size)
        sub_runs.append({"text": text, "size": size,
                         "color": parse_color(style.get("color")) or (0, 0, 0),
                         "family": fonts.family(style), "x": content_x,
                         "y": content_y + asc})
        content_h = asc + desc
    else:
        cursor = content_y
        for child in el.children:
            if child.tag in SKIP_TAGS:
                continue
            m = sides(child.style, "margin")
            child_y = cursor + m[0]
            used = layout(child, content_x + m[3], child_y,
                          max(0.0, w - pad[1] - pad[3]), sub_rects, sub_runs, fonts)
            cursor = child_y + used + m[2]
        content_h = cursor - content_y
    total_h = h if h is not None else pad[0] + content_h + pad[2]
    bg = parse_color(style.get("background") or style.get("background-color"))
    if bg is not None:
        rects.append({"type": "rect", "x": round(x), "y": round(y),
                      "w": round(w), "h": round(total_h), "fill": hex_color(bg)})
    rects.extend(sub_rects)
    runs.extend(sub_runs)
    return total_h
def build_ir(root, fonts, timings):
    t = time.perf_counter()
    rects, runs = [], []
    layout(root, 0.0, 0.0, VIEWPORT[0], rects, runs, fonts)
    timings["layout (block + line boxes)"] = (time.perf_counter() - t) * 1000

    t = time.perf_counter()
    shaped_runs, keys, glyph_cache = [], [], {}
    for run in runs:
        glyphs = fonts.shape(run["text"], run["size"])
        run["glyphs"] = glyphs
        shaped_runs.append({
            "text": run["text"], "font": run["family"], "size": run["size"],
            "color": hex_color(run["color"]), "direction": "ltr", "script": "Latn",
            "hb_scale": int(round(run["size"] * 64)),
            "advance": round(sum(g["adv"] for g in glyphs), 4), "glyphs": glyphs})
        for g in glyphs:
            key = (g["gid"], int(round(run["size"])))
            if key not in keys: keys.append(key)
    timings["shaping (HarfBuzz)"] = (time.perf_counter() - t) * 1000

    t = time.perf_counter()
    atlas = Atlas()
    for key in keys:
        glyph_cache[key] = fonts.raster(*key)
        w, h, _left, _top, lines = glyph_cache[key]
        atlas.add(key, w, h, lines)
    timings["atlas (FreeType raster + pack)"] = (time.perf_counter() - t) * 1000

    t = time.perf_counter()
    ops = list(rects)
    for run in runs:
        out_glyphs, pen = [], run["x"]
        size_key = int(round(run["size"]))
        for g in run["glyphs"]:
            w, h, left, top, _lines = glyph_cache[(g["gid"], size_key)]
            u0, v0, u1, v1 = atlas.uv((g["gid"], size_key))
            out_glyphs.append({"gid": g["gid"], "x": round(pen + g["dx"] + left, 3),
                "y": round(run["y"] - g["dy"] - top, 3), "w": w, "h": h,
                "u0": round(u0, 6), "v0": round(v0, 6), "u1": round(u1, 6),
                "v1": round(v1, 6), "adv": round(g["adv"], 4)})
            pen += g["adv"]
        ops.append({"type": "text_run", "x": round(run["x"], 3),
                    "y": round(run["y"], 3), "font": run["family"],
                    "size": round(run["size"], 3), "color": hex_color(run["color"]),
                    "glyphs": out_glyphs})
    timings["IR assembly"] = (time.perf_counter() - t) * 1000
    return ops, shaped_runs, atlas

# ----------------------------------------------------------------- 4. wgpu
RECT_WGSL = """
struct VSOut { @builtin(position) pos: vec4<f32>, @location(0) color: vec4<f32> };
@vertex fn vs_main(@location(0) pos: vec2<f32>, @location(1) color: vec4<f32>) -> VSOut {
    return VSOut(vec4<f32>(pos, 0.0, 1.0), color); }
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> { return in.color; }
"""
GLYPH_WGSL = """
struct VSOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32>,
               @location(1) color: vec4<f32> };
@vertex fn vs_main(@location(0) pos: vec2<f32>, @location(1) uv: vec2<f32>,
                   @location(2) color: vec4<f32>) -> VSOut {
    return VSOut(vec4<f32>(pos, 0.0, 1.0), uv, color); }
@group(0) @binding(0) var atlas_tex: texture_2d<f32>;
@group(0) @binding(1) var atlas_sampler: sampler;
@fragment fn fs_main(in: VSOut) -> @location(0) vec4<f32> {
    let a = textureSample(atlas_tex, atlas_sampler, in.uv).r;
    return vec4<f32>(in.color.rgb, in.color.a * a); }
"""
def to_ndc(x, y, w, h):
    return (x / VIEWPORT[0] * 2 - 1, 1 - y / VIEWPORT[1] * 2,
            (x + w) / VIEWPORT[0] * 2 - 1, 1 - (y + h) / VIEWPORT[1] * 2)
def rect_vertices(rect):
    x0, y0, x1, y1 = to_ndc(rect["x"], rect["y"], rect["w"], rect["h"])
    r, g, b = (c / 255.0 for c in parse_color(rect["fill"]))
    out = []
    for px_, py_ in ((x0, y0), (x0, y1), (x1, y1),
                     (x0, y0), (x1, y1), (x1, y0)):
        out.extend((px_, py_, r, g, b, 1.0))
    return out
def glyph_vertices(op):
    r, g, b = (c / 255.0 for c in parse_color(op["color"]))
    out = []
    for glyph in op["glyphs"]:
        if glyph["w"] == 0 or glyph["h"] == 0:
            continue
        x0, y0, x1, y1 = to_ndc(glyph["x"], glyph["y"], glyph["w"], glyph["h"])
        u0, v0, u1, v1 = (glyph[k] for k in ("u0", "v0", "u1", "v1"))
        for px_, py_, u, v in ((x0, y0, u0, v0), (x0, y1, u0, v1),
                               (x1, y1, u1, v1), (x0, y0, u0, v0),
                               (x1, y1, u1, v1), (x1, y0, u1, v0)):
            out.extend((px_, py_, u, v, r, g, b, 1.0))
    return out
def render_gpu(ops, atlas, clear_rgb):
    adapter = wgpu.gpu.request_adapter_sync(power_preference="high-performance")
    info = adapter.info
    print(f"[wgpu] adapter: {info['device']} | backend: {info['backend_type']} | "
          f"type: {info['adapter_type']} | vendor_id: {info['vendor_id']}")
    device = adapter.request_device_sync()
    fmt = wgpu.TextureFormat.rgba8unorm
    target = device.create_texture(
        size=(VIEWPORT[0], VIEWPORT[1], 1), format=fmt,
        usage=wgpu.TextureUsage.RENDER_ATTACHMENT | wgpu.TextureUsage.COPY_SRC)
    tex = device.create_texture(
        size=(ATLAS_SIZE, ATLAS_SIZE, 1), format=wgpu.TextureFormat.r8unorm,
        usage=wgpu.TextureUsage.TEXTURE_BINDING | wgpu.TextureUsage.COPY_DST)
    device.queue.write_texture(
        {"texture": tex, "mip_level": 0, "origin": (0, 0, 0)}, bytes(atlas.data),
        {"offset": 0, "bytes_per_row": ATLAS_SIZE, "rows_per_image": ATLAS_SIZE},
        (ATLAS_SIZE, ATLAS_SIZE, 1))
    sampler = device.create_sampler(
        mag_filter=wgpu.FilterMode.linear, min_filter=wgpu.FilterMode.linear,
        address_mode_u=wgpu.AddressMode.clamp_to_edge,
        address_mode_v=wgpu.AddressMode.clamp_to_edge)

    rect_mod = device.create_shader_module(code=RECT_WGSL)
    rect_pipe = device.create_render_pipeline(
        layout=wgpu.AutoLayoutMode.auto,
        vertex={"module": rect_mod, "entry_point": "vs_main", "buffers": [{
            "array_stride": 24, "step_mode": wgpu.VertexStepMode.vertex,
            "attributes": [
                {"format": wgpu.VertexFormat.float32x2, "offset": 0, "shader_location": 0},
                {"format": wgpu.VertexFormat.float32x4, "offset": 8, "shader_location": 1}]}]},
        primitive={"topology": wgpu.PrimitiveTopology.triangle_list},
        fragment={"module": rect_mod, "entry_point": "fs_main",
                  "targets": [{"format": fmt}]})
    def bc(src, dst):
        return {"src_factor": src, "dst_factor": dst,
                "operation": wgpu.BlendOperation.add}
    glyph_mod = device.create_shader_module(code=GLYPH_WGSL)
    blend = {"color": bc(wgpu.BlendFactor.src_alpha, wgpu.BlendFactor.one_minus_src_alpha),
             "alpha": bc(wgpu.BlendFactor.one, wgpu.BlendFactor.one_minus_src_alpha)}
    glyph_pipe = device.create_render_pipeline(
        layout=wgpu.AutoLayoutMode.auto,
        vertex={"module": glyph_mod, "entry_point": "vs_main", "buffers": [{
            "array_stride": 32, "step_mode": wgpu.VertexStepMode.vertex,
            "attributes": [
                {"format": wgpu.VertexFormat.float32x2, "offset": 0, "shader_location": 0},
                {"format": wgpu.VertexFormat.float32x2, "offset": 8, "shader_location": 1},
                {"format": wgpu.VertexFormat.float32x4, "offset": 16, "shader_location": 2}]}]},
        primitive={"topology": wgpu.PrimitiveTopology.triangle_list},
        fragment={"module": glyph_mod, "entry_point": "fs_main",
                  "targets": [{"format": fmt, "blend": blend}]})
    bind_group = device.create_bind_group(
        layout=glyph_pipe.get_bind_group_layout(0),
        entries=[{"binding": 0, "resource": tex.create_view()},
                 {"binding": 1, "resource": sampler}])

    rect_verts, glyph_verts = [], []
    for op in ops:
        if op["type"] == "rect":
            rect_verts.extend(rect_vertices(op))
        elif op["type"] == "text_run":
            glyph_verts.extend(glyph_vertices(op))
    rect_buf = device.create_buffer_with_data(
        data=array("f", rect_verts), usage=wgpu.BufferUsage.VERTEX)
    glyph_buf = device.create_buffer_with_data(
        data=array("f", glyph_verts), usage=wgpu.BufferUsage.VERTEX)
    encoder = device.create_command_encoder()
    rpass = encoder.begin_render_pass(color_attachments=[{
        "view": target.create_view(),
        "clear_value": tuple(c / 255.0 for c in clear_rgb) + (1.0,),
        "load_op": wgpu.LoadOp.clear, "store_op": wgpu.StoreOp.store}])
    if rect_verts:
        rpass.set_pipeline(rect_pipe)
        rpass.set_vertex_buffer(0, rect_buf)
        rpass.draw(vertex_count=len(rect_verts) // 6)
    if glyph_verts:
        rpass.set_pipeline(glyph_pipe)
        rpass.set_bind_group(0, bind_group)
        rpass.set_vertex_buffer(0, glyph_buf)
        rpass.draw(vertex_count=len(glyph_verts) // 8)
    rpass.end()

    bytes_per_row = ((VIEWPORT[0] * 4 + 255) // 256) * 256
    readback = device.create_buffer(
        size=bytes_per_row * VIEWPORT[1],
        usage=wgpu.BufferUsage.COPY_DST | wgpu.BufferUsage.MAP_READ)
    encoder.copy_texture_to_buffer(
        {"texture": target, "mip_level": 0, "origin": (0, 0, 0)},
        {"buffer": readback, "offset": 0, "bytes_per_row": bytes_per_row,
         "rows_per_image": VIEWPORT[1]}, (VIEWPORT[0], VIEWPORT[1], 1))
    device.queue.submit([encoder.finish()])
    readback.map_sync(wgpu.MapMode.READ)
    data = bytes(readback.read_mapped())
    readback.unmap()
    rows = [data[i * bytes_per_row:i * bytes_per_row + VIEWPORT[0] * 4]
            for i in range(VIEWPORT[1])]
    img = Image.frombytes("RGBA", VIEWPORT, b"".join(rows))
    img.save(HERE / "render.png")
    return img

# ----------------------------------------------------------- 5. Verification
def luminance(p):
    return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]
def dist2(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b))
def verify(img, shaped, ops):
    img = img.convert("RGBA")
    checks = [("png size 400x200", img.size == VIEWPORT, str(img.size))]
    bg, blue = (0x10, 0x10, 0x14), (0x34, 0x98, 0xdb)
    title = [img.getpixel((x, y))[:3] for y in range(20, 60)
             for x in range(VIEWPORT[0])]
    lit = sum(1 for p in title if luminance(p) > 40)
    above = sum(1 for p in title if luminance(p) > luminance(bg) + 40)
    checks.append(("title lit px >40 (>=100)", lit >= 100,
                   f"{lit} px (>{luminance(bg) + 40:.0f}: {above})"))
    body = [img.getpixel((x, y))[:3] for y in range(70, 100)
            for x in range(VIEWPORT[0])]
    blue_px = sum(1 for p in body if dist2(p, blue) < dist2(p, bg))
    checks.append(("body blue px (>=50)", blue_px >= 50, f"{blue_px} px"))
    counts = [len(r["glyphs"]) for r in shaped["runs"]]
    checks.append(("shaped glyphs per run > 0",
                   bool(counts) and all(c > 0 for c in counts), f"{counts}"))
    text_ops = [op for op in ops if op["type"] == "text_run"]
    checks.append(("text_run ops == 2", len(text_ops) == 2, f"{len(text_ops)}"))
    ok = True
    for name, good, detail in checks:
        ok = ok and good
        print(f"[verify] {name}: {detail} -> {'OK' if good else 'FAIL'}")
    return ok

# ------------------------------------------------------------------- 6. Main
def main():
    t_start, timings = time.perf_counter(), {}
    t = time.perf_counter()
    root = parse_html(HERE / "input.html")
    rules = load_rules(HERE / "input.css")
    cascade(root, rules)
    timings["html5lib + tinycss2 + cascade"] = (time.perf_counter() - t) * 1000
    body = next((el for el in root.children if el.tag == "body"), None)
    print(f"[css] rules: {len(rules)} | body bg: {body.style.get('background') if body else None}")

    fonts = Fonts()
    print(f"[font] family: Segoe UI | file: {fonts.path} | exists: {fonts.path.exists()} | upem: {fonts.hb_face.upem}")
    ops, shaped, atlas = build_ir(root, fonts, timings)

    doc = {"viewport": {"width": VIEWPORT[0], "height": VIEWPORT[1]},
           "ir_version": 1, "font_file": str(fonts.path),
           "atlas": {"width": ATLAS_SIZE, "height": ATLAS_SIZE, "format": "r8unorm",
                     "bytes": len(atlas.data)}, "ops": ops}
    (HERE / "display_list.json").write_text(json.dumps(doc, indent=2), encoding="utf-8")
    (HERE / "shaped.json").write_text(json.dumps(
        {"font_file": str(fonts.path), "font_family": "Segoe UI",
         "runs": shaped}, indent=2), encoding="utf-8")
    print(f"[ir] ops: {[op['type'] for op in ops]} | atlas unique glyphs: "
          f"{sum(1 for v in atlas.rects.values() if v)}")
    print("[shape] runs:", [(r["text"], r["size"], len(r["glyphs"]),
                             round(r["advance"], 2)) for r in shaped])
    print("[ir] detail:", [(op["type"], op["x"], op["y"], op.get("color"),
                            len(op.get("glyphs", []))) for op in ops])

    clear = (parse_color(body.style.get("background", "")) if body else None) or (0, 0, 0)
    t = time.perf_counter()
    img = render_gpu(ops, atlas, clear)
    timings["wgpu render + readback + png"] = (time.perf_counter() - t) * 1000
    t = time.perf_counter()
    ok = verify(img, {"runs": shaped}, ops)
    img.crop((0, 20, VIEWPORT[0], 70)).resize(
        (VIEWPORT[0] * 2, 100), Image.Resampling.NEAREST).save(HERE / "title_crop.png")
    timings["verify + crop"] = (time.perf_counter() - t) * 1000
    timings["TOTAL"] = (time.perf_counter() - t_start) * 1000
    print("[timings]")
    for name, ms in timings.items(): print(f"    {name:<36} {ms:8.1f} ms")
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
