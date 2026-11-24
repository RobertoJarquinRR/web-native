#!/usr/bin/env python3
"""Build-time entry point of exp16-text-c (one command, no manual steps).

    .venv\\Scripts\\python.exe run.py

Stages:
  1. exp09 frontend: HTML+CSS -> display_list.json (rect + text_run ops)
  2. atlas export:   IR glyphs -> atlas.bin (512x512 R8) + glyph_quads.json
  3. AOT bake:       IR + atlas -> scene_data.h (rect verts, glyph verts, atlas)
  4. reference:      exp09 wgpu renderer -> render_frontend.png (strong proof)

Nothing from stages 1-4 is needed at runtime: runtime_text.c only includes the
generated scene_data.h.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))

from webnative.cli import build_all                    # noqa: E402
from webnative.export import export_document           # noqa: E402
import emit_c                                          # noqa: E402


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="exp16 build-time pipeline")
    parser.add_argument("--html", type=Path, default=ROOT / "demo" / "input.html")
    parser.add_argument("--width", type=int, default=400)
    parser.add_argument("--height", type=int, default=240)
    parser.add_argument("--out-dir", type=Path, default=ROOT)
    parser.add_argument("--skip-frontend-render", action="store_true",
                        help="do not need wgpu at build time (no reference PNG)")
    args = parser.parse_args(argv)

    out = args.out_dir
    out.mkdir(parents=True, exist_ok=True)
    timings: dict[str, float] = {}
    started = time.perf_counter()

    def mark(name: str, since: float) -> None:
        timings[name] = round((time.perf_counter() - since) * 1000, 3)

    # 1. frontend ---------------------------------------------------------
    since = time.perf_counter()
    result = build_all(args.html, (args.width, args.height))
    display = result.display
    ir_path = out / "display_list.json"
    ir_path.write_text(display.to_json() + "\n", encoding="utf-8")
    mark("frontend_ir", since)

    counts: dict[str, int] = {}
    for op in display.ops:
        counts[op.type] = counts.get(op.type, 0) + 1
    print(f"[ir] {ir_path.name}: {len(display.ops)} ops "
          f"({', '.join(f'{n} {t}' for t, n in counts.items())})")

    # 2. atlas export -----------------------------------------------------
    since = time.perf_counter()
    atlas = export_document(display, out)
    mark("atlas_export", since)
    print(f"[atlas] {atlas['width']}x{atlas['height']} {atlas['format']} "
          f"{atlas['size_bytes']} B, {atlas['glyph_count']} glyphs, "
          f"nonzero={atlas['pixels_nonzero']}, sha256={atlas['sha256'][:16]}")
    for run in atlas["runs"]:
        print(f"[atlas] run {run['run_id']} {run['text']!r} {run['font']} "
              f"{run['size']:g}px {run['color']} -> {run['glyph_count']} quads")

    # 3. bake C header ----------------------------------------------------
    since = time.perf_counter()
    emit_args = [
        "--ir", str(ir_path),
        "--atlas", str(out / "atlas.bin"),
        "--quads", str(out / "glyph_quads.json"),
        "--out", str(out / "scene_data.h"),
    ]
    if emit_c.main(emit_args) != 0:
        print("error: emit_c.py failed", file=sys.stderr)
        return 1
    mark("emit_c", since)

    # 4. exp09 reference render (build-time proof only) -------------------
    if not args.skip_frontend_render:
        since = time.perf_counter()
        from webnative.renderer import create_renderer
        renderer = create_renderer("wgpu")
        if hasattr(renderer, "ensure_ready"):
            renderer.ensure_ready()
        image = renderer.render(display, args.width, args.height)
        image.save(out / "render_frontend.png")
        mark("frontend_render", since)
        stats = getattr(renderer, "last_frame", {})
        print(f"[frontend] wgpu={renderer.backend_info} -> render_frontend.png")
        print(f"[frontend] frame: rects={stats.get('rect_ops')} "
              f"text_runs={stats.get('text_runs')} glyphs={stats.get('glyphs')} "
              f"atlas={stats.get('atlas_size')}px/{stats.get('atlas_glyphs')}glyphs")
    else:
        timings["frontend_render"] = 0.0

    timings["total"] = round((time.perf_counter() - started) * 1000, 3)
    (out / "build_timings.json").write_text(json.dumps(timings, indent=2) + "\n",
                                            encoding="utf-8")
    print("[timings ms] " + ", ".join(f"{k}={v:.2f}" for k, v in timings.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
