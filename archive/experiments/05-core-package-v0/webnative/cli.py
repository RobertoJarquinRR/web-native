"""CLI: HTML+CSS -> display list IR -> PNG nativo.

Uso: python -m webnative input.html --out out.png --ir out.json --backend auto
"""
import argparse
import sys
import time
from pathlib import Path

from . import __version__
from .cascade import compute_styles
from .css_parse import parse_stylesheets
from .html_parse import parse_html_file, stylesheet_texts
from .ir import DisplayList
from .layout import layout_tree, to_display_list
from .renderer import create_renderer


def build(html_path: str | Path, viewport: tuple[int, int] = (400, 300)
          ) -> tuple[DisplayList, dict[str, float]]:
    """Etapas puras (sin render): HTML -> CSS -> cascada -> layout -> IR."""
    start = time.perf_counter()
    timings: dict[str, float] = {}

    def mark(name: str, since: float) -> None:
        timings[name] = round((time.perf_counter() - since) * 1000, 3)

    path = Path(html_path)
    since = time.perf_counter()
    root = parse_html_file(path)
    stylesheets = stylesheet_texts(root, path.parent)
    mark("html", since)
    since = time.perf_counter()
    rules = parse_stylesheets(stylesheets)
    mark("css", since)
    since = time.perf_counter()
    styles = compute_styles(root, rules)
    mark("cascade", since)
    since = time.perf_counter()
    box = layout_tree(root, styles, viewport)
    mark("layout", since)
    since = time.perf_counter()
    display = to_display_list(box, viewport)
    mark("ir", since)
    timings["pipeline"] = round((time.perf_counter() - start) * 1000, 3)
    return display, timings


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="webnative",
                                     description="HTML+CSS -> display list -> PNG sin navegador")
    parser.add_argument("input", type=Path, help="documento HTML de entrada")
    parser.add_argument("--out", type=Path, default=Path("render.png"),
                        help="PNG de salida (default: render.png)")
    parser.add_argument("--ir", type=Path, default=Path("display_list.json"),
                        help="JSON del display list (default: display_list.json)")
    parser.add_argument("--backend", choices=("auto", "wgpu", "cpu"), default="auto")
    parser.add_argument("--width", type=int, default=400)
    parser.add_argument("--height", type=int, default=300)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    started = time.perf_counter()
    print(f"webnative {__version__} | viewport {args.width}x{args.height}")
    try:
        display, timings = build(args.input, (args.width, args.height))
        args.ir.parent.mkdir(parents=True, exist_ok=True)
        args.ir.write_text(display.to_json() + "\n", encoding="utf-8")
        print(f"[ir] {args.ir} ({len(display.ops)} ops)")

        since = time.perf_counter()
        renderer = create_renderer(args.backend, log=print)
        if hasattr(renderer, "ensure_ready"):
            renderer.ensure_ready()  # init GPU fuera del tiempo de frame
        timings["backend"] = round((time.perf_counter() - since) * 1000, 3)
        print(f"backend = {renderer.name} ({renderer.backend_info})")

        since = time.perf_counter()
        try:
            image = renderer.render(display, args.width, args.height)
        except Exception as exc:
            if args.backend != "auto" or renderer.name != "wgpu":
                raise
            print(f"[auto] fallo al renderizar con wgpu ({type(exc).__name__}: {exc}); "
                  "reintentando cpu")
            renderer = create_renderer("cpu")
            print(f"backend = {renderer.name} ({renderer.backend_info})")
            image = renderer.render(display, args.width, args.height)
        timings["render"] = round((time.perf_counter() - since) * 1000, 3)

        since = time.perf_counter()
        args.out.parent.mkdir(parents=True, exist_ok=True)
        image.save(args.out)
        timings["png"] = round((time.perf_counter() - since) * 1000, 3)
        print(f"[png] {args.out} ({image.width}x{image.height})")
    except Exception as exc:
        print(f"ERROR: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1

    timings["wall"] = round((time.perf_counter() - started) * 1000, 3)
    print("\ntimings (ms, perf_counter):")
    for name, elapsed in timings.items():
        print(f"  {name:<9} {elapsed:8.2f}")
    return 0
