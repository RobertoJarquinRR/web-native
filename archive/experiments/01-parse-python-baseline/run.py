"""exp01-pure-python: HTML+CSS -> display-list IR -> offscreen GPU render.

Minimal proof without browser/WebView/Skia/Electron. Five stages, one file.
"""
import json, os, re, struct, subprocess, sys, time
from pathlib import Path
from PIL import Image, ImageDraw

BASE = Path(__file__).resolve().parent
VIEWPORT = (400, 300)
EXPECTED = [
    {"type": "rect", "x": 0, "y": 0, "w": 400, "h": 300, "fill": "#101014"},
    {"type": "rect", "x": 20, "y": 20, "w": 120, "h": 80, "fill": "#e74c3c"},
    {"type": "rect", "x": 20, "y": 116, "w": 120, "h": 80, "fill": "#3498db"},
]
SAMPLES = [((60, 60), "#e74c3c", "box a"), ((60, 156), "#3498db", "box b"),
           ((200, 250), "#101014", "root")]
# Optional dev workaround: Mesa llvmpipe when the machine has no usable WGL ICD.
MESA = BASE / ".mesa" / "runtime" / "x64"


def ms(t0): return round((time.perf_counter() - t0) * 1000, 2)

class Node:
    __slots__ = ("tag", "attrs", "children", "parent")

    def __init__(self, tag, attrs, parent):
        self.tag, self.attrs, self.children, self.parent = tag, attrs, [], parent

    @property
    def classes(self):
        return set(self.attrs.get("class", "").split())


def parse_html(text):
    import html5lib
    tree = html5lib.parse(text, treebuilder="etree", namespaceHTMLElements=False)
    root = tree.getroot() if hasattr(tree, "getroot") else tree

    def conv(el, parent):
        node = Node(el.tag.lower(), dict(el.attrib), parent)
        node.children = [conv(c, node) for c in el if isinstance(c.tag, str)]
        return node

    return conv(root.find(".//body"), None)

LEN_RE = re.compile(r"^(-?\d+(?:\.\d+)?)(px)?$")
SEL_RE = re.compile(r"^([a-zA-Z][\w-]*)?((?:[#.][\w-]+)*)$")


def length(value):
    m = LEN_RE.match(value.strip()) if value else None
    return float(m.group(1)) if m else None

def color(value):
    v = (value or "").strip().lower()
    if re.fullmatch(r"#[0-9a-f]{3}", v):
        return "#" + "".join(c * 2 for c in v[1:])
    return v if re.fullmatch(r"#[0-9a-f]{6}", v) else None

def selector_parts(text):
    """Compound selector only: tag? + #id/.class, no combinators."""
    m = SEL_RE.match(text.strip())
    if not m:
        return None
    tag = m.group(1).lower() if m.group(1) else None
    return tag, re.findall(r"#([\w-]+)", m.group(2)), re.findall(r"\.([\w-]+)", m.group(2))

def matches(node, sel):
    tag, ids, classes = sel
    return ((tag is None or node.tag == tag) and all(node.attrs.get("id") == i for i in ids)
            and all(c in node.classes for c in classes))

def expand(prop, value):
    """margin/padding shorthand -> 4 longhands."""
    if prop not in ("margin", "padding"):
        return {prop: value}
    v = value.split()
    if not 1 <= len(v) <= 4:
        return {}
    top, right = v[0], v[1] if len(v) > 1 else v[0]
    bottom, left = v[2] if len(v) > 2 else top, v[3] if len(v) > 3 else right
    return {f"{prop}-{k}": x for k, x in
            zip(("top", "right", "bottom", "left"), (top, right, bottom, left))}


def parse_css(text):
    import tinycss2
    parsed, order = [], 0
    for rule in tinycss2.parse_stylesheet(text, skip_comments=True, skip_whitespace=True):
        if rule.type != "qualified-rule":
            continue
        decls = []
        for d in tinycss2.parse_declaration_list(rule.content, skip_comments=True,
                                                 skip_whitespace=True):
            if d.type == "declaration" and not d.important:
                decls += list(expand(d.name, tinycss2.serialize(d.value).strip()).items())
        for sel_text in tinycss2.serialize(rule.prelude).split(","):
            sel = selector_parts(sel_text)
            if sel:
                parsed.append(((len(sel[1]), len(sel[2]), 1 if sel[0] else 0), order, sel, decls))
                order += 1
    return parsed

def walk(node):
    yield node
    for child in node.children:
        yield from walk(child)

def cascade(node, rules):
    """Specificity (id > class > tag), then source order."""
    wins = {}
    for el in walk(node):
        hits = []
        for spec, order, sel, decls in rules:
            if matches(el, sel):
                hits += [(spec, order, i, prop, value)
                         for i, (prop, value) in enumerate(decls)]
        hits.sort(key=lambda h: h[:3])
        wins[el] = {prop: value for _, _, _, prop, value in hits}
    return wins

def compute(declared):
    sides = ("top", "right", "bottom", "left")
    out = {}
    for node, d in declared.items():
        out[node] = {**{f"margin-{s}": length(d.get(f"margin-{s}")) or 0.0 for s in sides},
                     **{f"padding-{s}": length(d.get(f"padding-{s}")) or 0.0 for s in sides},
                     "width": length(d.get("width")), "height": length(d.get("height")),
                     "background": color(d.get("background"))}  # does not inherit
    return out

def layout(body, styles, viewport):
    """Block layout, border-box sizing. Returns {node: (x, y, w, h)}."""
    rects = {}

    def children(node, cx, cy, cw):
        cursor = cy
        for child in node.children:
            s = styles[child]
            x, y = cx + s["margin-left"], cursor + s["margin-top"]
            w = s["width"] if s["width"] is not None else cw - s["margin-left"] - s["margin-right"]
            pl, pr = s["padding-left"], s["padding-right"]
            pt, pb = s["padding-top"], s["padding-bottom"]
            inner = children(child, x + pl, y + pt, max(w - pl - pr, 0))
            h = s["height"] if s["height"] is not None else inner + pt + pb
            rects[child] = (x, y, w, h)
            cursor = y + h + s["margin-bottom"]
        return cursor - cy

    s = styles[body]
    inner = children(body, 0, 0, viewport[0])
    bh = s["height"] if s["height"] is not None else inner + s["padding-top"] + s["padding-bottom"]
    rects[body] = (0, 0, viewport[0], bh)
    return rects

def display_list(body, styles, rects):
    ops = []
    for node in walk(body):  # pre-order == painter's order
        s, (x, y, w, h) = styles[node], rects[node]
        if s["background"]:
            ops.append({"type": "rect", "x": round(x), "y": round(y),
                        "w": round(w), "h": round(h), "fill": s["background"]})
    return {"viewport": {"width": VIEWPORT[0], "height": VIEWPORT[1]}, "ops": ops}

VS = """#version 330
in vec2 in_pos; uniform vec4 u_rect; uniform vec2 u_viewport;
void main() {
    vec2 px = u_rect.xy + in_pos * u_rect.zw;
    gl_Position = vec4(px.x / u_viewport.x * 2.0 - 1.0,
                       1.0 - px.y / u_viewport.y * 2.0, 0.0, 1.0);
}"""
FS = """#version 330
uniform vec4 u_color; out vec4 f_color;
void main() { f_color = u_color; }"""


def rgb(hex_color):
    h = hex_color.lstrip("#")
    return tuple(int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4))

def render_gl(display, png_path):
    import moderngl
    W, H = VIEWPORT
    ctx = moderngl.create_standalone_context()
    fbo = ctx.simple_framebuffer((W, H), components=4)
    fbo.use()
    ctx.clear(0.0, 0.0, 0.0, 1.0)
    prog = ctx.program(vertex_shader=VS, fragment_shader=FS)
    quad = ctx.buffer(struct.pack("<8f", 0, 0, 1, 0, 0, 1, 1, 1))
    vao = ctx.vertex_array(prog, [(quad, "2f", "in_pos")])
    prog["u_viewport"].value = (float(W), float(H))
    for op in display["ops"]:
        prog["u_rect"].value = tuple(float(op[k]) for k in ("x", "y", "w", "h"))
        prog["u_color"].value = (*rgb(op["fill"]), 1.0)
        vao.render(moderngl.TRIANGLE_STRIP, vertices=4)
    raw = fbo.read(components=3, alignment=1)
    Image.frombytes("RGB", (W, H), raw).transpose(Image.Transpose.FLIP_TOP_BOTTOM).save(png_path)
    return {k: ctx.info[k] for k in ("GL_VENDOR", "GL_RENDERER", "GL_VERSION")}

def preload_mesa():
    """Load Mesa's opengl32.dll before moderngl/glcontext bind the system one."""
    import ctypes
    os.environ.setdefault("GALLIUM_DRIVER", "llvmpipe")
    ctypes.WinDLL("kernel32").SetDllDirectoryW(str(MESA))
    ctypes.WinDLL(str(MESA / "opengl32.dll"))

def render_pillow(display, png_path):
    W, H = VIEWPORT
    img = Image.new("RGB", (W, H), (0, 0, 0))
    draw = ImageDraw.Draw(img)
    for op in display["ops"]:
        draw.rectangle([op["x"], op["y"], op["x"] + op["w"] - 1, op["y"] + op["h"] - 1],
                       fill=tuple(round(c * 255) for c in rgb(op["fill"])))
    img.save(png_path)

def verify(png_path, display):
    checks = []

    def check(name, ok, detail=""):
        checks.append({"check": name, "pass": bool(ok), "detail": detail})
        print(f"  [{'PASS' if ok else 'FAIL'}] {name} {detail}")

    rects, start, ok = [op for op in display["ops"] if op["type"] == "rect"], 0, True
    for exp in EXPECTED:
        hit = next((i for i in range(start, len(rects))
                    if all(rects[i][k] == exp[k] for k in ("x", "y", "w", "h", "fill"))), None)
        ok, start = ok and hit is not None, (hit + 1) if hit is not None else start
    check("IR has expected 3 rects in painter's order", ok, f"({len(display['ops'])} ops total)")

    img = Image.open(png_path).convert("RGB")
    check("PNG size is 400x300", img.size == VIEWPORT, f"got {img.size}")
    for (x, y), want, label in SAMPLES:
        got = img.getpixel((x, y))
        exp = tuple(int(want[i:i + 2], 16) for i in (1, 3, 5))
        delta = max(abs(a - b) for a, b in zip(got, exp))
        check(f"pixel ({x},{y}) ~= {want} ({label})", delta <= 8,
              f"got #{got[0]:02x}{got[1]:02x}{got[2]:02x} delta={delta}")
    return checks


def main():
    t0, times = time.perf_counter(), {}
    print("exp01-pure-python - HTML+CSS -> display list -> render\n")
    if os.environ.get("EXP01_MESA") == "1" and (MESA / "opengl32.dll").exists():
        preload_mesa()
        print("  [Mesa llvmpipe preloaded]")

    s = time.perf_counter()
    body = parse_html((BASE / "input.html").read_text(encoding="utf-8"))
    times["1_parse_html_ms"] = ms(s)
    s = time.perf_counter()
    styles = compute(cascade(body, parse_css((BASE / "input.css").read_text(encoding="utf-8"))))
    times["2_cascade_ms"] = ms(s)
    s = time.perf_counter()
    rects = layout(body, styles, VIEWPORT)
    times["3_layout_ms"] = ms(s)
    s = time.perf_counter()
    display = display_list(body, styles, rects)
    (BASE / "display_list.json").write_text(json.dumps(display, indent=2), encoding="utf-8")
    times["4_ir_ms"] = ms(s)

    png, backend, gl_error, gl_info = BASE / "render.png", "moderngl/OpenGL", None, None
    s = time.perf_counter()
    try:
        gl_info = render_gl(display, png)
        if "llvmpipe" in gl_info["GL_RENDERER"]:
            backend += " (Mesa llvmpipe, software)"
        print(f"  render backend: moderngl [{gl_info['GL_RENDERER']}]")
    except Exception as exc:  # no WGL ICD -> Mesa retry, else raster fallback
        gl_error = f"{type(exc).__name__}: {exc}"
        if (MESA / "opengl32.dll").exists() and os.environ.get("EXP01_MESA") != "1":
            print(f"  system GL unavailable ({gl_error})", flush=True)
            print("  re-running in a fresh process with Mesa llvmpipe...\n", flush=True)
            return subprocess.call([sys.executable, str(BASE / "run.py")],
                                   env={**os.environ, "EXP01_MESA": "1"})
        backend = "pillow-fallback"
        print(f"  GL context failed: {gl_error}")
        print("  falling back to Pillow rasterization (PARTIAL)")
        render_pillow(display, png)
    times["5_render_ms"] = ms(s)

    s = time.perf_counter()
    checks = verify(png, display)
    times["6_verify_ms"] = ms(s)
    times["total_ms"] = ms(t0)

    passed = all(c["pass"] for c in checks)
    status = "FAILED" if not passed else ("SUCCESS" if backend == "moderngl/OpenGL" else "PARTIAL")
    report = {"status": status, "backend": backend, "gl_info": gl_info, "gl_error": gl_error,
              "timings_ms": times, "checks": checks, "python": sys.version.split()[0]}
    (BASE / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("\nTimings (ms): " + ", ".join(f"{k}={v}" for k, v in times.items()))
    print(f"Backend: {backend}\nSTATUS: {status}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
