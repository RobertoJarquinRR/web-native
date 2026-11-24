"""Render a display list offscreen with moderngl (OpenGL 3.3 FBO), or fall back
to Pillow if no hardware OpenGL context exists. Verify pixels with Pillow."""
from __future__ import annotations

import struct

from PIL import Image, ImageDraw

VS = """#version 330
in vec2 in_pos;
uniform vec4 rect;      // x, y, w, h in pixels, top-left origin
uniform vec2 viewport;
void main() {
    vec2 p = rect.xy + in_pos * rect.zw;
    vec2 ndc = vec2(p.x / viewport.x * 2.0 - 1.0,
                    1.0 - p.y / viewport.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}"""

FS = """#version 330
uniform vec3 color;
out vec4 frag;
void main() { frag = vec4(color, 1.0); }"""


def rgb(hex_color: str) -> tuple[int, int, int]:
    h = hex_color.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def render_gl(ops: list[dict], size: tuple[int, int]) -> Image.Image:
    import moderngl

    ctx = moderngl.create_standalone_context(require=330)
    fbo = ctx.simple_framebuffer(size)
    fbo.use()
    ctx.clear(0.0, 0.0, 0.0, 1.0)
    prog = ctx.program(vertex_shader=VS, fragment_shader=FS)
    prog["viewport"].value = size
    quad = ctx.buffer(struct.pack("<8f", 0.0, 0.0, 1.0, 0.0,
                                 0.0, 1.0, 1.0, 1.0))
    vao = ctx.vertex_array(prog, [(quad, "2f", "in_pos")])
    for op in ops:
        r, g, b = rgb(op["fill"])
        prog["rect"].value = (op["x"], op["y"], op["w"], op["h"])
        prog["color"].value = (r / 255.0, g / 255.0, b / 255.0)
        vao.render(moderngl.TRIANGLE_STRIP)
    data = fbo.read(components=3)
    img = Image.frombytes("RGB", size, data)
    return img.transpose(Image.Transpose.FLIP_TOP_BOTTOM)


def render_pillow(ops: list[dict], size: tuple[int, int]) -> Image.Image:
    img = Image.new("RGB", size, (0, 0, 0))
    draw = ImageDraw.Draw(img)
    for op in ops:
        x, y, w, h = op["x"], op["y"], op["w"], op["h"]
        draw.rectangle([x, y, x + w - 1, y + h - 1], fill=rgb(op["fill"]))
    return img


CHECKS = [
    ("size 400x300", (60, 60), None),
    ("pixel (60,60) = #e74c3c", (60, 60), "#e74c3c"),
    ("pixel (60,156) = #3498db", (60, 156), "#3498db"),
    ("pixel (200,250) = #101014", (200, 250), "#101014"),
]


def verify(path, size: tuple[int, int]) -> bool:
    img = Image.open(path).convert("RGB")
    ok_all = True
    for label, xy, expected in CHECKS:
        if expected is None:
            actual = img.size
            ok = actual == tuple(size)
            detail = f"{actual}" + ("" if ok else f" expected {tuple(size)}")
        else:
            actual = img.getpixel(xy)
            exp = rgb(expected)
            ok = all(abs(a - e) <= 8 for a, e in zip(actual, exp))
            detail = f"rgb{actual} expected {expected} (+-8/ch)"
        print(f"[{'PASS' if ok else 'FAIL'}] {label}: {detail}")
        ok_all &= ok
    return ok_all
