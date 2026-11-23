#!/usr/bin/env python3
"""exp03-semantic-cssselect2.

HTML + CSS -> semantic cascade (html5lib + tinycss2 + cssselect2: the WeasyPrint
frontend trio) -> minimal block layout -> display_list.json -> offscreen moderngl
render -> render.png -> Pillow verification. No browser, WebView, Skia or Electron.
"""
import json, struct, sys, time, traceback
from contextlib import contextmanager
from pathlib import Path

import cssselect2, html5lib, tinycss2
from PIL import Image

BASE = Path(__file__).resolve().parent
VIEWPORT = (400, 300)
TIMINGS = {}
INHERITED = {"color", "font-size", "font-family"}
BLOCK_TAGS = {"html", "body", "div", "p", "section", "article", "header", "footer",
              "main", "aside", "nav", "ul", "ol", "li", "h1", "h2", "h3"}

@contextmanager
def stage(name):
    start = time.perf_counter()
    try:
        yield
    finally:
        TIMINGS[name] = (time.perf_counter() - start) * 1000.0

# ---- 1-2. HTML + CSS parsing ---------------------------------------------
def stylesheet_texts(root, base):
    chunks = [(base / link.get("href")).read_text(encoding="utf-8")
              for link in root.iter("link")
              if link.get("href") and "stylesheet" in (link.get("rel") or "").lower().split()]
    chunks += ["".join(node.itertext()) for node in root.iter("style")]
    if not chunks:
        raise RuntimeError("no stylesheet found")
    return chunks

def parse_css(chunks):
    """tinycss2 -> [(CompiledSelector, [(property, value, important), ...])]."""
    rules = []
    for css in chunks:
        for rule in tinycss2.parse_stylesheet(css, skip_comments=True, skip_whitespace=True):
            if rule.type != "qualified-rule":
                continue
            decls = [(d.lower_name, tinycss2.serialize(d.value).strip(), bool(d.important))
                     for d in tinycss2.parse_declaration_list(
                         rule.content, skip_comments=True, skip_whitespace=True)
                     if d.type == "declaration" and d.name]
            prelude = tinycss2.serialize(rule.prelude).strip()
            for selector in cssselect2.compile_selector_list(prelude) if prelude and decls else ():
                # Pseudo-elements are out of scope for this block-level proof.
                if selector.pseudo_element is None and not selector.never_matches:
                    rules.append((selector, decls))
    return rules

# ---- 3. Semantic cascade: real selector matching + specificity -----------
def cascade(root, rules):
    root_wrapper = cssselect2.ElementWrapper.from_html_root(root)
    matcher = cssselect2.Matcher()
    for selector, decls in rules:
        matcher.add_selector(selector, decls)

    styles, trace = {}, []
    for element in root_wrapper.iter_subtree():
        parent = styles.get(element.parent)
        computed = {k: v for k, v in parent.items() if k in INHERITED} if parent else {}
        winners = {}  # property -> ((important, specificity, order, decl_index), value)
        for specificity, order, _, decls in matcher.match(element):
            for index, (name, value, important) in enumerate(decls):
                key = (important, specificity, order, index)
                if name not in winners or key >= winners[name][0]:
                    winners[name] = (key, value)
        for name, (key, value) in winners.items():
            computed[name] = value
            if element.local_name not in ("html", "head", "link"):
                tag = element.local_name + (f"#{element.id}" if element.id else "")
                tag += "." + ".".join(sorted(element.classes)) if element.classes else ""
                trace.append((tag, name, value, key[1], key[2]))
        styles[element] = computed
    return root_wrapper, styles, trace

# ---- 4. Minimal block layout (border-box for explicit width/height) ------
def px(value):
    if value is None:
        return None
    value = value.strip().lower()
    if value.endswith("px"):
        try:
            return float(value[:-2])
        except ValueError:
            return None
    return 0.0 if value in ("0", "0.0") else None

def sides(style, prefix):
    """Shorthand + longhands -> [top, right, bottom, left]."""
    parts = [px(p) for p in style.get(prefix, "").replace("/", " ").split()]
    if not parts or None in parts:
        parts = [0.0]
    indices = {1: (0, 0, 0, 0), 2: (0, 1, 0, 1), 3: (0, 1, 2, 1)}.get(len(parts), (0, 1, 2, 3))
    values = [parts[i] for i in indices]
    for index, side in enumerate(("top", "right", "bottom", "left")):
        longhand = px(style.get(f"{prefix}-{side}"))
        if longhand is not None:
            values[index] = longhand
    return values

def parse_color(value):
    value = (value or "").strip().lower()
    if value.startswith("#"):
        digits = value[1:]
        if len(digits) == 3:
            digits = "".join(c * 2 for c in digits)
        if len(digits) == 6 and all(c in "0123456789abcdef" for c in digits):
            return "#" + digits
    return None

class Box:
    def __init__(self, wrapper, x, y, w, bg):
        self.x, self.y, self.w, self.h, self.bg = x, y, w, 0.0, bg
        self.margin_bottom, self.children = 0.0, []

def build_layout(wrapper, origin_x, origin_y, available, styles):
    style = styles[wrapper]
    margin, padding = sides(style, "margin"), sides(style, "padding")
    width, height = px(style.get("width")), px(style.get("height"))
    border_w = width if width is not None else max(0.0, available - margin[1] - margin[3])
    x, y = origin_x + margin[3], origin_y + margin[0]
    content_x, content_y = x + padding[3], y + padding[0]
    content_w = max(0.0, border_w - padding[1] - padding[3])
    box = Box(wrapper, x, y, border_w, parse_color(style.get("background")))
    box.margin_bottom = margin[2]

    cursor = content_y
    for child in wrapper.iter_children():
        if child.local_name not in BLOCK_TAGS or styles[child].get("display") == "none":
            continue
        child_box = build_layout(child, content_x, cursor, content_w, styles)
        box.children.append(child_box)
        cursor = child_box.y + child_box.h + child_box.margin_bottom
    box.h = height if height is not None else max(
        0.0, cursor - content_y + padding[0] + padding[2])
    return box

def paint(box, ops):
    if box.bg:
        ops.append({"type": "rect", "x": box.x, "y": box.y,
                    "w": box.w, "h": box.h, "fill": box.bg})
    for child in box.children:
        paint(child, ops)

# ---- 5. Display list ------------------------------------------------------
def emit_display_list(ops, path):
    doc = {"viewport": {"width": VIEWPORT[0], "height": VIEWPORT[1]},
           "ops": [{"type": "rect", "x": round(op["x"]), "y": round(op["y"]),
                    "w": round(op["w"]), "h": round(op["h"]),
                    "fill": op["fill"]} for op in ops]}
    path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    return doc

# ---- 6. Offscreen render: moderngl (GPU) with Pillow fallback ------------
VERTEX_SHADER = """#version 330 core
in vec2 in_pos; in vec3 in_color; uniform vec2 u_viewport; out vec3 v_color;
void main() {  // pixel coords, top-left origin -> clip space
    vec2 p = in_pos / u_viewport;
    gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    v_color = in_color; }"""
FRAGMENT_SHADER = """#version 330 core
in vec3 v_color; out vec4 f_color;
void main() { f_color = vec4(v_color, 1.0); }"""

def hex_rgb(fill):
    return tuple(int(fill[i:i + 2], 16) / 255.0 for i in (1, 3, 5))

def render_moderngl(ops, path):
    import moderngl
    ctx = moderngl.create_standalone_context(require=330)
    fbo = ctx.simple_framebuffer(VIEWPORT)
    fbo.use()
    fbo.clear((0.0, 0.0, 0.0, 1.0))
    prog = ctx.program(vertex_shader=VERTEX_SHADER, fragment_shader=FRAGMENT_SHADER)
    prog["u_viewport"].value = VIEWPORT
    for op in ops:
        x0, y0, x1, y1 = op["x"], op["y"], op["x"] + op["w"], op["y"] + op["h"]
        r, g, b = hex_rgb(op["fill"])
        verts = [c for vx, vy in ((x0, y0), (x1, y0), (x1, y1),
                                  (x0, y0), (x1, y1), (x0, y1)) for c in (vx, vy, r, g, b)]
        vbo = ctx.buffer(struct.pack(f"{len(verts)}f", *verts))
        vao = ctx.vertex_array(prog, [(vbo, "2f 3f", "in_pos", "in_color")])
        vao.render(moderngl.TRIANGLES)
        vao.release()
        vbo.release()
    data = fbo.read(components=3, alignment=1)
    Image.frombytes("RGB", VIEWPORT, data).transpose(
        Image.Transpose.FLIP_TOP_BOTTOM).save(path)
    ctx.release()
    return "moderngl (OpenGL standalone)"

def render_pillow(ops, path):
    from PIL import ImageDraw
    image = Image.new("RGB", VIEWPORT)
    draw = ImageDraw.Draw(image)
    for op in ops:
        draw.rectangle([op["x"], op["y"], op["x"] + op["w"] - 1, op["y"] + op["h"] - 1],
                       fill=tuple(round(c * 255) for c in hex_rgb(op["fill"])))
    image.save(path)
    return "Pillow fallback (CPU)"

# ---- 7. Verification (Pillow encode/sample only) -------------------------
EXPECTED_OPS = [{"type": "rect", "x": x, "y": y, "w": w, "h": h, "fill": f} for x, y, w, h, f in
                [(0, 0, 400, 300, "#101014"), (20, 20, 120, 80, "#e74c3c"),
                 (20, 116, 120, 80, "#3498db")]]
PIXEL_CHECKS = [(60, 60, "#e74c3c", "box a"), (60, 156, "#3498db", "box b"),
                (200, 250, "#101014", "root")]

def verify(doc, png_path):
    print("\n[verify] display list geometry")
    indices = [next((i for i, op in enumerate(doc["ops"]) if op == expected), None)
               for expected in EXPECTED_OPS]
    for expected, index in zip(EXPECTED_OPS, indices):
        print(f"  {'OK  ' if index is not None else 'FAIL'} {expected}")
    ordered = None not in indices and indices == sorted(indices)
    ok = ordered
    print(f"  {'OK  ' if ordered else 'FAIL'} mandatory ops present and in painter's order")

    image = Image.open(png_path).convert("RGB")
    print(f"\n[verify] PNG {png_path.name}: {image.size[0]}x{image.size[1]}")
    if image.size != VIEWPORT:
        print(f"  FAIL expected {VIEWPORT}")
        ok = False
    for x, y, expected_hex, name in PIXEL_CHECKS:
        actual = image.getpixel((x, y))
        target = tuple(int(expected_hex[i:i + 2], 16) for i in (1, 3, 5))
        delta = max(abs(a - b) for a, b in zip(actual, target))
        passed = delta <= 8
        ok = ok and passed
        print(f"  {'OK  ' if passed else 'FAIL'} ({x:3d},{y:3d}) {name:6s} "
              f"got={actual} expected={target} maxdiff={delta}")
    return ok

# ---- Entry point ----------------------------------------------------------
def main():
    started = time.perf_counter()
    print("exp03-semantic-cssselect2 :: html5lib + tinycss2 + cssselect2 + moderngl")
    print(f"python {sys.version.split()[0]} | viewport {VIEWPORT[0]}x{VIEWPORT[1]}\n")
    partial = False
    try:
        with stage("1. parse HTML"):
            # html5lib implements the HTML5 tree construction algorithm (implied nodes).
            root = html5lib.parse((BASE / "input.html").read_text(encoding="utf-8"),
                                  treebuilder="etree", namespaceHTMLElements=False)
            sheets = stylesheet_texts(root, BASE)
        print(f"[1] html5lib: {sum(1 for _ in root.iter())} elements, {len(sheets)} stylesheet(s)")
        with stage("2. parse CSS (tinycss2)"):
            rules = parse_css(sheets)
        print(f"[2] tinycss2: {len(rules)} selector(s) with declarations")
        with stage("3. cascade (cssselect2)"):
            root_wrapper, styles, trace = cascade(root, rules)
            wrappers = sum(1 for _ in root_wrapper.iter_subtree())
        print(f"[3] cssselect2: {wrappers} wrappers, {len(trace)} resolved declarations")
        for tag, prop, value, specificity, order in trace:
            print(f"    {tag:9s} {prop:10s}={value:9s} spec={tuple(specificity)} order={order}")
        with stage("4. block layout"):
            ops = []
            paint(build_layout(root_wrapper, 0.0, 0.0, VIEWPORT[0], styles), ops)
        print(f"[4] layout: {len(ops)} background rect(s)")
        with stage("5. display list"):
            doc = emit_display_list(ops, BASE / "display_list.json")
        print(f"[5] display_list.json: {len(doc['ops'])} ops")
        with stage("6. render"):
            try:
                backend = render_moderngl(doc["ops"], BASE / "render.png")
            except Exception as error:
                print(f"[6] moderngl FAILED: {type(error).__name__}: {error}")
                print("[6] falling back to Pillow (verification remains valid, GPU path absent)")
                backend = render_pillow(doc["ops"], BASE / "render.png")
                partial = True
        print(f"[6] rendered with {backend} -> render.png")
        with stage("7. verify (Pillow)"):
            ok = verify(doc, BASE / "render.png")
    except Exception as error:
        print(f"\nFATAL: {type(error).__name__}: {error}")
        traceback.print_exc()
        print("\nFAIL")
        return 1

    print("\n[timings] ms per stage")
    for name, elapsed in TIMINGS.items():
        print(f"  {name:26s} {elapsed:8.2f}")
    print(f"  {'TOTAL (wall)':26s} {(time.perf_counter() - started) * 1000:8.2f}")
    if not ok:
        print("\nFAIL")
        return 1
    print("\nPARTIAL (Pillow fallback; no OpenGL 3.3 context)" if partial else "\nPASS")
    return 2 if partial else 0

if __name__ == "__main__":
    sys.exit(main())
