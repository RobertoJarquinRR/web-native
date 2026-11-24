"""Test de contrato exp05-v0-core.

Ejecutar:  .venv\\Scripts\\python.exe -m pytest tests -q
o bien:    .venv\\Scripts\\python.exe tests\\test_contract.py
"""
import inspect
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from PIL import Image  # noqa: E402

from webnative.cli import build  # noqa: E402
from webnative.cascade import compute_styles  # noqa: E402
from webnative.css_parse import parse_stylesheet  # noqa: E402
from webnative.html_parse import parse_html  # noqa: E402
from webnative.ir import DisplayList, RectOp  # noqa: E402
from webnative.renderer import create_renderer  # noqa: E402

VIEWPORT = (400, 300)
MANDATORY = [
    {"type": "rect", "x": 0, "y": 0, "w": 400, "h": 300, "fill": "#101014"},
    {"type": "rect", "x": 20, "y": 20, "w": 120, "h": 80, "fill": "#e74c3c"},
    {"type": "rect", "x": 20, "y": 116, "w": 120, "h": 80, "fill": "#3498db"},
]
SAMPLES = [((60, 60), "#e74c3c", "box a"), ((60, 156), "#3498db", "box b"),
           ((200, 250), "#101014", "root")]


def _display() -> DisplayList:
    display, _timings = build(ROOT / "input.html", VIEWPORT)
    return display


def _assert_pixels(image: Image.Image) -> None:
    assert image.size == VIEWPORT, f"tamaño {image.size}, esperado {VIEWPORT}"
    for (x, y), expected_hex, label in SAMPLES:
        got = image.getpixel((x, y))[:3]
        expected = tuple(int(expected_hex[i:i + 2], 16) for i in (1, 3, 5))
        delta = max(abs(a - b) for a, b in zip(got, expected))
        assert delta <= 8, f"{label} ({x},{y}): {got} vs {expected}, maxdiff={delta}"


def test_ir_contract() -> None:
    display = _display()
    assert (display.width, display.height) == VIEWPORT
    ops = [op.to_dict() for op in display.ops]
    cursor = 0
    for expected in MANDATORY:
        index = next((i for i in range(cursor, len(ops)) if ops[i] == expected), None)
        assert index is not None, f"falta {expected} en {ops}"
        cursor = index + 1
    assert all(isinstance(op, RectOp) for op in display.ops)
    assert all(isinstance(op.to_dict()["x"], int) for op in display.ops), "el JSON debe ser int"


def test_json_roundtrip() -> None:
    display = _display()
    again = DisplayList.from_json(display.to_json())
    assert again.to_dict() == display.to_dict()
    assert again.ops == display.ops
    path = Path(tempfile.mkdtemp()) / "display_list.json"
    path.write_text(display.to_json(), encoding="utf-8")
    assert DisplayList.from_json(path.read_text(encoding="utf-8")).ops == display.ops


def test_cpu_backend(tmp_path: Path) -> None:
    display = _display()
    image = create_renderer("cpu").render(display, *VIEWPORT)
    _assert_pixels(image)
    path = tmp_path / "render-cpu.png"
    image.save(path)
    _assert_pixels(Image.open(path).convert("RGB"))


def test_wgpu_backend(tmp_path: Path) -> None:
    display = _display()
    image = create_renderer("wgpu").render(display, *VIEWPORT)
    _assert_pixels(image)
    path = tmp_path / "render-wgpu.png"
    image.save(path)
    _assert_pixels(Image.open(path).convert("RGB"))


def test_auto_prefers_wgpu() -> None:
    renderer = create_renderer("auto")
    assert renderer.name == "wgpu", f"auto eligió {renderer.name}"
    _assert_pixels(renderer.render(_display(), *VIEWPORT))


def test_cascade_specificity_inline_and_shorthand() -> None:
    root = parse_html('<html><body><div id="x" class="a" '
                      'style="background:#00ff00;width:10px"></div></body></html>')
    css = (".a{background:#ff0000;width:20px;margin:1px 2px 3px 4px;padding:10px}"
           "#x{background:#0000ff}")
    styles = compute_styles(root, parse_stylesheet(css))
    div = next(node for node in root.walk() if node.tag == "div")
    style = styles[div]
    assert style.background == "#00ff00", "style='' debe ganar a #id"
    assert style.width == 10.0
    assert style.margin == (1.0, 2.0, 3.0, 4.0)
    assert style.padding == (10.0, 10.0, 10.0, 10.0)


def test_cli_end_to_end(tmp_path: Path) -> None:
    out, ir = tmp_path / "out.png", tmp_path / "out.json"
    result = subprocess.run(
        [sys.executable, "-m", "webnative", "input.html", "--out", str(out),
         "--ir", str(ir), "--backend", "cpu"],
        cwd=ROOT, capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stderr
    assert "backend = cpu" in result.stdout, result.stdout
    ops = [op.to_dict() for op in DisplayList.from_json(
        ir.read_text(encoding="utf-8")).ops]
    for expected in MANDATORY:
        assert expected in ops, f"falta {expected}"
    _assert_pixels(Image.open(out).convert("RGB"))


def test_no_browser_dependencies() -> None:
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
    import webnative  # noqa: F401
    loaded = " ".join(sys.modules).lower()
    for token in forbidden:
        assert token not in loaded, f"módulo cargado: {token}"


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temp_dir:
        failures = 0
        for name, fn in sorted(globals().items()):
            if name.startswith("test_") and callable(fn):
                kwargs = ({"tmp_path": Path(temp_dir)}
                          if "tmp_path" in inspect.signature(fn).parameters else {})
                try:
                    fn(**kwargs)
                    print(f"PASS {name}")
                except Exception as exc:  # runner mínimo sin pytest
                    failures += 1
                    print(f"FAIL {name}: {type(exc).__name__}: {exc}")
        print("PASS" if not failures else f"FAIL ({failures})")
        raise SystemExit(1 if failures else 0)
