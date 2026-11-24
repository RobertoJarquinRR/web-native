"""Suite de contrato v1: geometría, IR, píxeles, paridad y CLI.

Ejecutar:  .venv\\Scripts\\python.exe -m pytest tests -q
"""
import inspect
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from PIL import Image  # noqa: E402

from webnative.cascade import compute_styles  # noqa: E402
from webnative.cli import build_all  # noqa: E402
from webnative.css_parse import parse_stylesheet  # noqa: E402
from webnative.fonts import WINDOWS_FONTS, resolve_font_file  # noqa: E402
from webnative.html_parse import parse_html  # noqa: E402
from webnative.ir import DisplayList, RectOp, TextOp  # noqa: E402
from webnative.layout import estimate_metrics, layout_tree  # noqa: E402
from webnative.renderer import create_renderer  # noqa: E402
from webnative.text import TextEngine, build_atlas, text_op_glyph_keys  # noqa: E402

VIEWPORT = (400, 240)
OLD_VIEWPORT = (400, 300)
TITLE_BAND = (0, 10, 400, 70)   # única zona donde puede haber texto en el demo

DEMO = ROOT / "demo" / "input.html"
OLD = ROOT / "input.html"
DEMO_CHIPS = [(20, "#e74c3c"), (88, "#2ecc71"), (156, "#3498db")]


def rgb255(color: str) -> tuple[int, int, int]:
    return tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))


def luminance(pixel) -> float:
    return 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2]


def find_box(box, *, node_id: str | None = None, css_class: str | None = None):
    for candidate in box.walk():
        if node_id is not None and candidate.node.attrs.get("id") == node_id:
            return candidate
        if css_class is not None and css_class in candidate.node.attrs.get("class", "").split():
            return candidate
    raise AssertionError(f"LayoutBox no encontrada (id={node_id!r}, class={css_class!r})")


def region(image: Image.Image, box) -> list:
    x0, y0, x1, y1 = box
    return [image.getpixel((x, y)) for y in range(y0, y1) for x in range(x0, x1)]


def lit_count(pixels, threshold: float = 100.0) -> int:
    return sum(1 for pixel in pixels if luminance(pixel) > threshold)


@pytest.fixture(scope="session")
def demo():
    return build_all(DEMO, VIEWPORT)


@pytest.fixture(scope="session")
def old():
    return build_all(OLD, OLD_VIEWPORT)


@pytest.fixture(scope="session")
def wgpu():
    return create_renderer("wgpu")


@pytest.fixture(scope="session")
def cpu():
    return create_renderer("cpu")


# --------------------------------------------------------------------- geometría

def test_demo_geometry(demo):
    body, root = demo.box, find_box(demo.box, node_id="root")
    title = find_box(demo.box, css_class="title")
    row = find_box(demo.box, css_class="row")
    chips = [find_box(demo.box, css_class=name) for name in ("a", "b", "c")]

    assert (body.x, body.y, body.width, body.height) == (0, 0, 400, 240)
    assert (root.x, root.y, root.width, root.height) == (0, 0, 400, 240), "root = 400x240"
    assert root.kind == "block" and find_box(demo.box, css_class="card").kind == "flex"
    assert row.kind == "flex"

    ascender, descender = TextEngine().metrics("Segoe UI", 28)
    assert ascender > 28 and descender > 0, "métricas FreeType inesperadas"
    assert title.x == 20 and title.y == 20
    assert title.height == ascender + descender, "alto de línea = ascender + descender"
    assert title.text_runs[0].baseline == pytest.approx(20 + ascender), \
        "baseline = content_y + ascender(28px)"

    assert row.y == pytest.approx(title.bottom + 12), "fila = bottom del título + gap 12"
    for chip, (x, _color) in zip(chips, DEMO_CHIPS):
        assert (chip.x, chip.y) == (x, pytest.approx(row.y))
        assert (chip.width, chip.height) == (60, 60)
    assert chips[1].x - chips[0].x == 68 and chips[2].x - chips[1].x == 68, "60 + gap 8"


def test_flex_justify_and_align_variants():
    html = ('<html><body><div id="r">'
            + "".join('<div class="c"></div>' for _ in range(3))
            + "</div></body></html>")
    base = ("#r{width:400px;height:160px;padding:20px;display:flex;gap:8px;%s}"
            ".c{width:60px;height:60px}")
    root = parse_html(html)

    def chips(css: str):
        styles = compute_styles(root, parse_stylesheet(base % css))
        box = layout_tree(root, styles, (400, 160), metrics=estimate_metrics)
        container = find_box(box, node_id="r")
        return [child for child in container.children if child.node.attrs.get("class") == "c"]

    centered = chips("justify-content:center;align-items:center")
    assert [c.x for c in centered] == [102, 170, 238]  # free=164 -> start=82
    assert [c.y for c in centered] == [50, 50, 50]     # (120-60)/2

    between = chips("justify-content:space-between")
    assert [c.x for c in between] == [20, 170, 320]    # gap 8 + 164/2 = 90
    assert [c.y for c in between] == [20, 20, 20]

    end = chips("justify-content:flex-end;align-items:flex-end")
    assert [c.x for c in end] == [184, 252, 320]
    assert [c.y for c in end] == [80, 80, 80]          # 120-60

    column_css = ("#r{width:400px;height:240px;padding:20px;display:flex;"
                  "flex-direction:column;gap:10px;align-items:center}"
                  ".c{width:60px;height:60px}")
    styles = compute_styles(root, parse_stylesheet(column_css))
    column = find_box(layout_tree(root, styles, (400, 240), metrics=estimate_metrics),
                      node_id="r")
    assert [c.y for c in column.children] == [20, 90, 160]
    assert [c.x for c in column.children] == [170, 170, 170]


# ---------------------------------------------------------------------------- IR

def test_demo_ir_contract(demo):
    display = demo.display
    assert (display.width, display.height) == VIEWPORT
    assert all(isinstance(op, (RectOp, TextOp)) for op in display.ops)
    assert sum(isinstance(op, RectOp) for op in display.ops) == 5
    text_ops = [op for op in display.ops if isinstance(op, TextOp)]
    assert len(text_ops) == 1, "el fixture debe producir exactamente un text_run"

    text = text_ops[0]
    assert text.type == "text_run"
    assert (text.text, text.font, text.size, text.color) == ("Hola", "Segoe UI", 28, "#ffffff")
    assert text.x == 20 and text.y == 51
    assert len(text.glyphs) == 4 and all(g.w > 0 and g.h > 0 and g.gid > 0 for g in text.glyphs)
    assert text.font_file.lower().endswith("segoeui.ttf")
    payload = text.to_dict()
    for key in ("type", "x", "y", "font", "size", "color", "glyphs"):
        assert key in payload, f"falta {key} en el formato exp06"
    assert {"gid", "x", "y", "w", "h", "u0", "v0", "u1", "v1", "adv"} <= set(payload["glyphs"][0])

    ops = [op.to_dict() for op in display.ops]
    assert {"type": "rect", "x": 0, "y": 0, "w": 400, "h": 240,
            "fill": "#101014"} in ops
    row_y = round(find_box(demo.box, css_class="row").y)
    for x, color in DEMO_CHIPS:
        assert {"type": "rect", "x": x, "y": row_y, "w": 60, "h": 60,
                "fill": color} in ops
    assert all(isinstance(op["x"], int) for op in ops if op["type"] == "rect")


def test_json_roundtrip_and_render_from_ir(demo, wgpu):
    display = demo.display
    again = DisplayList.from_json(display.to_json())
    assert again.to_dict() == display.to_dict()
    assert again.ops == display.ops

    direct = wgpu.render(display)
    rebuilt = wgpu.render(again)
    assert direct.size == rebuilt.size == VIEWPORT
    # Gracias al redondeo canónico del IR (3 decimales en posiciones, 6 en UVs),
    # build y roundtrip producen exactamente los mismos píxeles.
    assert direct.tobytes() == rebuilt.tobytes(), "el IR debe ser autocontenido"


def test_atlas_hints_match_rebuilt_atlas(demo):
    engine = TextEngine()
    keys = text_op_glyph_keys(demo.display.ops, engine)
    atlas = build_atlas(engine, keys)
    assert atlas.glyph_count == 4
    for op in demo.display.ops:
        if not isinstance(op, TextOp):
            continue
        for glyph in op.glyphs:
            key = (op.font_file, glyph.gid, int(round(op.size)))
            u0, v0, u1, v1 = atlas.uv(key)
            assert (glyph.u0, glyph.v0, glyph.u1, glyph.v1) == \
                pytest.approx((u0, v0, u1, v1), abs=1e-6)


# ------------------------------------------------------------------------ píxeles

def test_wgpu_demo_pixels(demo, wgpu):
    image = wgpu.render(demo.display)
    assert image.size == VIEWPORT
    row_y = round(find_box(demo.box, css_class="row").y)
    for x, color in DEMO_CHIPS:
        got = image.getpixel((x + 30, row_y + 30))
        assert got == rgb255(color), f"chip en x={x}: {got} != {rgb255(color)}"
    assert image.getpixel((350, 200)) == (16, 16, 20), "fondo #101014"
    assert lit_count(region(image, TITLE_BAND)) >= 100, "texto del título visible"
    assert wgpu.last_frame["text_runs"] == 1
    assert wgpu.last_frame["atlas_glyphs"] == 4


def test_cpu_demo_pixels(demo, cpu):
    image = cpu.render(demo.display)
    assert image.size == VIEWPORT
    row_y = round(find_box(demo.box, css_class="row").y)
    for x, color in DEMO_CHIPS:
        got = image.getpixel((x + 30, row_y + 30))
        assert got == rgb255(color), f"chip en x={x}: {got} != {rgb255(color)}"
    assert image.getpixel((350, 200)) == (16, 16, 20)
    assert lit_count(region(image, TITLE_BAND)) >= 100, "texto aproximado visible en CPU"


def test_cpu_vs_wgpu_text_difference(demo, wgpu, cpu):
    """Los rects deben ser idénticos; el texto se compara y se reporta."""
    gpu_image, cpu_image = wgpu.render(demo.display), cpu.render(demo.display)
    x0, y0, x1, y1 = TITLE_BAND
    for y in range(gpu_image.height):
        for x in range(gpu_image.width):
            if x0 <= x < x1 and y0 <= y < y1:
                continue
            assert gpu_image.getpixel((x, y)) == cpu_image.getpixel((x, y)), \
                f"diferencia fuera de la banda de texto en ({x},{y})"

    gpu_pixels = region(gpu_image, TITLE_BAND)
    cpu_pixels = region(cpu_image, TITLE_BAND)
    diffs = [max(abs(a - b) for a, b in zip(p, q)) for p, q in zip(gpu_pixels, cpu_pixels)]
    mean_abs = sum(sum(abs(a - b) for a, b in zip(p, q))
                   for p, q in zip(gpu_pixels, cpu_pixels)) / (len(gpu_pixels) * 3)
    gpu_lit = [luminance(p) > 100 for p in gpu_pixels]
    cpu_lit = [luminance(p) > 100 for p in cpu_pixels]
    iou = sum(a and b for a, b in zip(gpu_lit, cpu_lit)) / sum(a or b for a, b in zip(gpu_lit, cpu_lit))
    print(f"[parity] text band mean_abs={mean_abs:.2f} max={max(diffs)} "
          f"differing={sum(1 for d in diffs if d)}/{len(diffs)} lit_iou={iou:.3f}")
    assert mean_abs < 5.0
    assert iou > 0.90
    assert sum(1 for d in diffs if d) / len(diffs) < 0.10


# ------------------------------------------------------- regresión del contrato v0

def test_old_contract_ir(old):
    display = old.display
    assert (display.width, display.height) == OLD_VIEWPORT
    expected = [
        {"type": "rect", "x": 0, "y": 0, "w": 400, "h": 300, "fill": "#0f0f12"},
        {"type": "rect", "x": 0, "y": 0, "w": 400, "h": 300, "fill": "#101014"},
        {"type": "rect", "x": 20, "y": 20, "w": 120, "h": 80, "fill": "#e74c3c"},
        {"type": "rect", "x": 20, "y": 116, "w": 120, "h": 80, "fill": "#3498db"},
    ]
    assert [op.to_dict() for op in display.ops] == expected
    assert not any(isinstance(op, TextOp) for op in display.ops)


def test_old_contract_cpu_wgpu_byte_identical(old, wgpu, cpu):
    gpu_image, cpu_image = wgpu.render(old.display), cpu.render(old.display)
    assert gpu_image.size == cpu_image.size == OLD_VIEWPORT
    assert gpu_image.tobytes() == cpu_image.tobytes(), "píxeles CPU != wgpu"
    for point, color in (((60, 60), "#e74c3c"), ((60, 156), "#3498db"),
                         ((200, 250), "#101014")):
        assert gpu_image.getpixel(point) == rgb255(color)

    with tempfile.TemporaryDirectory() as directory:
        gpu_path, cpu_path = Path(directory) / "g.png", Path(directory) / "c.png"
        gpu_image.save(gpu_path)
        cpu_image.save(cpu_path)
        assert gpu_path.read_bytes() == cpu_path.read_bytes(), "PNG no byte-idéntico"


# ----------------------------------------------------------------------- fuentes

def test_font_resolution():
    segoe = resolve_font_file('"Segoe UI", sans-serif')
    arial = resolve_font_file("Arial")
    unknown = resolve_font_file("Familia Inexistente")
    assert segoe == WINDOWS_FONTS / "segoeui.ttf" and segoe.is_file()
    assert arial == WINDOWS_FONTS / "arial.ttf" and arial.is_file()
    assert unknown.is_file() and unknown.name in ("segoeui.ttf", "arial.ttf")


# --------------------------------------------------------------------------- CLI

def _run_cli(*args: str, cwd: Path = ROOT):
    return subprocess.run([sys.executable, "-m", "webnative", *args], cwd=cwd,
                          capture_output=True, text=True, check=False)


def test_cli_auto_selects_and_prints_backend(tmp_path: Path):
    out, ir = tmp_path / "render-auto.png", tmp_path / "auto.json"
    result = _run_cli("demo/input.html", "--out", str(out), "--ir", str(ir),
                      "--backend", "auto")
    assert result.returncode == 0, result.stderr
    assert "backend = wgpu" in result.stdout, result.stdout
    assert "backend = " in result.stdout
    assert Image.open(out).size == VIEWPORT
    ops = [op.to_dict() for op in DisplayList.from_json(
        ir.read_text(encoding="utf-8")).ops]
    assert sum(op["type"] == "text_run" for op in ops) == 1


def test_cli_cpu_end_to_end(tmp_path: Path, demo):
    out, ir = tmp_path / "render-cpu.png", tmp_path / "cpu.json"
    result = _run_cli("demo/input.html", "--out", str(out), "--ir", str(ir),
                      "--backend", "cpu")
    assert result.returncode == 0, result.stderr
    assert "backend = cpu" in result.stdout
    image = Image.open(out).convert("RGB")
    assert image.size == VIEWPORT
    row_y = round(find_box(demo.box, css_class="row").y)
    for x, color in DEMO_CHIPS:
        assert image.getpixel((x + 30, row_y + 30)) == rgb255(color)


# ------------------------------------------------------------------- dependencias

def test_no_browser_or_skia_dependencies():
    import ast

    forbidden = ("webview", "selenium", "playwright", "electron", "skia", "cefpython")
    for path in (ROOT / "webnative").rglob("*.py"):
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                modules = [alias.name for alias in node.names]
            elif isinstance(node, ast.ImportFrom):
                modules = [node.module or ""]
            else:
                continue
            for module in modules:
                assert not any(token in module.lower() for token in forbidden), \
                    f"{path.name} importa {module}"

    pure = ("tree.py", "html_parse.py", "css_parse.py", "cascade.py", "layout.py",
            "ir.py", "fonts.py")
    native = {"pil", "wgpu", "freetype", "uharfbuzz"}
    for name in pure:
        tree = ast.parse((ROOT / "webnative" / name).read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                modules = [alias.name for alias in node.names]
            elif isinstance(node, ast.ImportFrom):
                modules = [node.module or ""]
            else:
                continue
            roots = {module.split(".")[0].lower() for module in modules}
            assert not (roots & native), f"{name} no debe importar nativo: {roots & native}"

    import webnative
    loaded = " ".join(sys.modules).lower()
    for token in forbidden:
        assert token not in loaded, f"módulo cargado: {token}"


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temp_dir:
        failures, cache = 0, {}

        def fixture(name):
            if name not in cache:
                cache[name] = build_all(DEMO if name == "demo" else OLD,
                                        VIEWPORT if name == "demo" else OLD_VIEWPORT)
            return cache[name]

        for name, fn in sorted(globals().items()):
            if not (name.startswith("test_") and callable(fn)):
                continue
            params = inspect.signature(fn).parameters
            kwargs = {}
            for key in params:
                if key == "tmp_path":
                    kwargs[key] = Path(temp_dir)
                elif key == "demo":
                    kwargs[key] = fixture("demo")
                elif key == "old":
                    kwargs[key] = fixture("old")
                elif key in ("wgpu", "cpu"):
                    if key not in cache:
                        cache[key] = create_renderer(key)
                    kwargs[key] = cache[key]
            try:
                fn(**kwargs)
                print(f"PASS {name}")
            except Exception as exc:  # runner sin pytest
                failures += 1
                print(f"FAIL {name}: {type(exc).__name__}: {exc}")
        print("PASS" if not failures else f"FAIL ({failures})")
        raise SystemExit(1 if failures else 0)
