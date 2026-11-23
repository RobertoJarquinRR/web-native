"""exp11-real-page: HTML+CSS -> cascada -> layout -> display list -> PNG.

Flujo:
  page/input.html + page/input.css
    -> html5lib / tinycss2 / cssselect2 (cascada)
    -> layout de bloque + flex row/column + líneas con wrapping
    -> HarfBuzz (shaping) + FreeType (raster) + atlas R8 1024
    -> display_list.json (rect + text_run)
    -> wgpu (D3D12/Vulkan/Metal) o Pillow fallback
    -> render.png + report.json + checks

Uso:
  .venv\\Scripts\\python.exe run.py
"""
import json
import sys
import time
from importlib.metadata import version
from pathlib import Path

from webnative.cascade import compute_styles
from webnative.css_parse import parse_stylesheet
from webnative.html_parse import parse_html
from webnative.ir import DisplayList, build_display_list
from webnative.layout import layout_document, serialize_box
from webnative.renderer import WgpuRenderer, render_pillow
from webnative.text import Atlas, FontBook
from webnative.verify import verify

HERE = Path(__file__).resolve().parent
PAGE = HERE / "page"
VIEWPORT = (640, 480)
ATLAS_SIZE = 1024


def _lines(box):
    for line in box.lines:
        yield line
    for child in box.children:
        yield from _lines(child)


def robustness_probe(fonts) -> dict:
    """CSS no soportado debe ignorarse, no reventar. Evidencia ejecutable."""
    html = """
    <body>
      <div class="grid">
        <div class="a">Alpha con grid</div>
        <div class="b">Beta absoluta</div>
      </div>
    </body>
    """
    css = """
    body { margin: 0; height: 200px; background-color: #000000; color: #ffffff; }
    .grid {
      display: grid; grid-template-columns: 1fr 1fr; gap: 1em;
      padding: 10%; border-radius: 8px; box-shadow: 0 2px 4px #000000;
      transform: translateX(4px); overflow: hidden;
    }
    .a { width: 50%; min-height: 20px; flex: 1 1 auto; }
    .b { position: absolute; top: 10px; left: 10px; border: 1px solid #ffffff; }
    """
    ignored = ["display:grid", "grid-template-columns", "gap:1em", "padding:10%",
               "border-radius", "box-shadow", "transform", "overflow",
               "width:50%", "min-height", "flex:1 1 auto", "position:absolute",
               "border", "box-sizing:border-box"]
    try:
        root = parse_html(html)
        styles = compute_styles(root, parse_stylesheet(css))
        box, engine = layout_document(root, styles, fonts, (320, 200))
        line_count = sum(1 for _ in _lines(box))
        ok = line_count >= 2
        return {"pass": ok, "ignored": ignored, "lines": line_count,
                "warnings": engine.warnings,
                "detail": f"{line_count} líneas; {len(ignored)} features ignoradas sin excepción"}
    except Exception as exc:  # el probe debe reportar, no abortar el experimento
        return {"pass": False, "ignored": ignored, "lines": 0, "warnings": [],
                "detail": f"{type(exc).__name__}: {exc}"}


def main() -> int:
    started = time.perf_counter()
    timings: dict[str, float] = {}

    # ---------------------------------------------------------------- parse
    t = time.perf_counter()
    root = parse_html((PAGE / "input.html").read_text(encoding="utf-8"))
    timings["parse_html"] = (time.perf_counter() - t) * 1000

    t = time.perf_counter()
    css_text = (PAGE / "input.css").read_text(encoding="utf-8")
    rules = parse_stylesheet(css_text)
    timings["parse_css"] = (time.perf_counter() - t) * 1000

    t = time.perf_counter()
    styles = compute_styles(root, rules)
    timings["cascade"] = (time.perf_counter() - t) * 1000
    element_count = sum(1 for node in root.walk() if not node.is_text)
    print(f"[parse] html ok | css rules: {len(rules)} | elements: {element_count}")

    # ---------------------------------------------------------------- fonts
    t = time.perf_counter()
    fonts = FontBook()
    timings["fonts_init"] = (time.perf_counter() - t) * 1000
    info = fonts.describe()
    print(f"[font] regular: {info['regular']}")
    print(f"[font] bold:    {info['bold']}")
    for warning in info["warnings"]:
        print(f"[font] WARNING: {warning}")

    # --------------------------------------------------------------- layout
    t = time.perf_counter()
    layout_root, engine = layout_document(root, styles, fonts, VIEWPORT)
    timings["layout"] = (time.perf_counter() - t) * 1000
    for warning in engine.warnings:
        print(f"[layout] WARNING: {warning}")

    # ---------------------------------------------------- shaping + atlas + IR
    t = time.perf_counter()
    atlas = Atlas(ATLAS_SIZE)
    display = build_display_list(layout_root, fonts, atlas, VIEWPORT)
    timings["shape_atlas_ir"] = (time.perf_counter() - t) * 1000
    payload = display.to_dict()
    (HERE / "display_list.json").write_text(display.to_json(), encoding="utf-8")
    (HERE / "layout.json").write_text(json.dumps(
        serialize_box(layout_root), indent=2, ensure_ascii=False), encoding="utf-8")

    text_runs = display.text_ops()
    print(f"[layout] body {layout_root.w:.0f}x{layout_root.h:.0f} | "
          f"ops: {len(display.ops)} | text runs: {len(text_runs)} | "
          f"glyph quads: {payload['stats']['glyph_quads']} | atlas bitmaps: "
          f"{atlas.unique()}")

    # --------------------------------------------------------------- render
    clear = (0x0f, 0x0f, 0x14)
    if layout_root.style and layout_root.style.background:
        clear = tuple(int(layout_root.style.background[i:i + 2], 16)
                      for i in (1, 3, 5))
    renderer, render_error, adapter = None, None, None
    t = time.perf_counter()
    try:
        renderer = WgpuRenderer(VIEWPORT, ATLAS_SIZE)
        adapter = renderer.info
        backend = f"wgpu/{adapter['backend']}"
        print(f"[backend] {backend} | {adapter['device']} | "
              f"{adapter['adapter_type']} | vendor {adapter['vendor_id']}")
    except Exception as exc:
        render_error = f"{type(exc).__name__}: {exc}"
        backend = "cpu-pillow"
        print(f"[backend] wgpu no disponible: {render_error}; fallback Pillow")
    timings["wgpu_init"] = (time.perf_counter() - t) * 1000

    t = time.perf_counter()
    if renderer is not None:
        try:
            image = renderer.render(payload["ops"], atlas, clear)
        except Exception as exc:
            render_error = f"{type(exc).__name__}: {exc}"
            backend = "cpu-pillow"
            print(f"[render] wgpu falló en render: {render_error}; fallback Pillow")
            image = render_pillow(payload["ops"], atlas, *VIEWPORT, clear)
    else:
        image = render_pillow(payload["ops"], atlas, *VIEWPORT, clear)
    image.save(HERE / "render.png")
    timings["render_png"] = (time.perf_counter() - t) * 1000

    # ----------------------------------------------------------- verification
    t = time.perf_counter()
    checks, stats = verify(layout_root, payload, atlas, image, VIEWPORT)
    probe = robustness_probe(fonts)
    checks.add("robustness", "features no soportadas se ignoran sin crash",
               probe["pass"], "sin excepción", probe["detail"])
    timings["verify"] = (time.perf_counter() - t) * 1000
    timings["TOTAL"] = (time.perf_counter() - started) * 1000

    for check in checks.items:
        mark = "OK  " if check["pass"] else "FAIL"
        print(f"[verify] {mark} {check['kind']:<10} {check['name']}: "
              f"{check['got']}")
    print(f"[probe] {probe['detail']} | warnings: {len(probe['warnings'])}")

    failed = [check for check in checks.items if not check["pass"]]
    status = "FAILED" if failed else (
        "PARTIAL" if (renderer is None and render_error) or fonts.warnings
        else "SUCCESS")

    report = {
        "status": status,
        "experiment": "exp11-real-page",
        "viewport": {"width": VIEWPORT[0], "height": VIEWPORT[1]},
        "backend": backend,
        "adapter": adapter,
        "render_error": render_error,
        "timings_ms": {k: round(v, 2) for k, v in timings.items()},
        "stats": stats,
        "checks": checks.items,
        "check_summary": {"total": len(checks.items),
                          "passed": sum(1 for c in checks.items if c["pass"]),
                          "failed": len(failed)},
        "fonts": info,
        "layout_warnings": engine.warnings,
        "probe": probe,
        "display_list": str(HERE / "display_list.json"),
        "layout_json": str(HERE / "layout.json"),
        "render_png": str(HERE / "render.png"),
        "versions": {
            "python": sys.version.split()[0],
            "html5lib": version("html5lib"),
            "tinycss2": version("tinycss2"),
            "cssselect2": version("cssselect2"),
            "uharfbuzz": version("uharfbuzz"),
            "freetype-py": version("freetype-py"),
            "wgpu": version("wgpu"),
            "pillow": version("Pillow"),
        },
    }
    (HERE / "report.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")

    print("\n[timings]")
    for name, ms in report["timings_ms"].items():
        print(f"    {name:<18} {ms:8.1f} ms")
    print(f"\n[checks] {report['check_summary']['passed']}/"
          f"{report['check_summary']['total']} OK")
    print(f"STATUS: {status}")
    return 0 if status != "FAILED" else 1


if __name__ == "__main__":
    sys.exit(main())
