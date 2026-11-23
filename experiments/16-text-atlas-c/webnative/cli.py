"""CLI: HTML+CSS -> display list IR (rect + text_run) -> PNG nativo.

Uso: python -m webnative demo/input.html --out render.png --ir display_list.json
"""
import argparse
import sys
import time
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

from . import __version__
from .cascade import ComputedStyle, compute_styles
from .css_parse import parse_stylesheets
from .html_parse import parse_html_file, stylesheet_texts
from .ir import DisplayList
from .layout import LayoutBox, layout_tree, normalize_text, to_display_list
from .renderer import create_renderer
from .tree import Node


@dataclass
class BuildResult:
    """Etapas puras completas, para tests/verificación y para `build()`."""
    root: Node
    styles: dict[Node, ComputedStyle]
    box: LayoutBox
    display: DisplayList
    timings: dict[str, float]


def build_all(html_path: str | Path, viewport: tuple[int, int] = (400, 240)
              ) -> BuildResult:
    """HTML -> CSS -> cascada -> layout (bloque/flex/texto) -> IR."""
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
    engine = None
    if any(normalize_text(style.text) for style in styles.values()):
        from .text import TextEngine
        engine = TextEngine()
    mark("fonts", since)

    since = time.perf_counter()
    box = layout_tree(root, styles, viewport,
                      engine.metrics if engine is not None else None)
    mark("layout", since)

    since = time.perf_counter()
    display = to_display_list(box, viewport, engine)
    mark("text+ir", since)

    timings["pipeline"] = round((time.perf_counter() - start) * 1000, 3)
    return BuildResult(root, styles, box, display, timings)


def build(html_path: str | Path, viewport: tuple[int, int] = (400, 240)
          ) -> tuple[DisplayList, dict[str, float]]:
    """Compatibilidad con v0: `(display_list, timings)`."""
    result = build_all(html_path, viewport)
    return result.display, result.timings


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="webnative",
        description="HTML+CSS -> display list (rect/text_run) -> PNG sin navegador")
    parser.add_argument("input", type=Path, help="documento HTML de entrada")
    parser.add_argument("--out", type=Path, default=Path("render.png"),
                        help="PNG de salida (default: render.png)")
    parser.add_argument("--ir", type=Path, default=Path("display_list.json"),
                        help="JSON del display list (default: display_list.json)")
    parser.add_argument("--backend", choices=("auto", "wgpu", "cpu"), default="auto")
    parser.add_argument("--width", type=int, default=400)
    parser.add_argument("--height", type=int, default=240)
    return parser


def _op_summary(display: DisplayList) -> str:
    counts = Counter(op.type for op in display.ops)
    return ", ".join(f"{count} {name}" for name, count in counts.items())


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    started = time.perf_counter()
    print(f"webnative {__version__} | viewport {args.width}x{args.height}")
    try:
        result = build_all(args.input, (args.width, args.height))
        display, timings = result.display, result.timings
        args.ir.parent.mkdir(parents=True, exist_ok=True)
        args.ir.write_text(display.to_json() + "\n", encoding="utf-8")
        print(f"[ir] {args.ir} ({len(display.ops)} ops: {_op_summary(display)})")

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

        stats = getattr(renderer, "last_frame", None)
        if stats:
            print(f"[frame] rects={stats['rect_ops']} text_runs={stats['text_runs']} "
                  f"glyphs={stats['glyphs']} atlas={stats['atlas_size']}px/"
                  f"{stats['atlas_glyphs']}glyphs")

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


if __name__ == "__main__":
    raise SystemExit(main())
