"""exp02-native-bindings entry point: HTML+CSS -> IR -> PNG, all measured."""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

import engine
import render

BASE = Path(__file__).parent


def main() -> int:
    t_start = time.perf_counter()
    times: dict[str, float] = {}

    def stage(name: str, start: float) -> None:
        times[name] = (time.perf_counter() - start) * 1000.0
        print(f"[time] {name:<12} {times[name]:7.2f} ms")

    t = time.perf_counter()
    html_src = (BASE / "input.html").read_text(encoding="utf-8")
    css_src = (BASE / "input.css").read_text(encoding="utf-8")
    root = engine.parse_html(html_src)
    stage("html_parse", t)

    t = time.perf_counter()
    css_min = engine.native_css(css_src)
    rules = engine.parse_rules(css_min)
    stage("css_native", t)
    print(f"[css] lightningcss minified ({len(rules)} rules): {css_min}")

    t = time.perf_counter()
    matched = engine.cascade(root, rules)
    stage("cascade", t)
    print(f"[css] selectors matched: {matched}")

    t = time.perf_counter()
    ordered = engine.layout(root, engine.VIEWPORT)
    stage("layout", t)

    t = time.perf_counter()
    doc = engine.display_list(ordered)
    (BASE / "display_list.json").write_text(
        json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    stage("displaylist", t)
    print("[ir] ops:", [f'{o["fill"]}@{o["x"]},{o["y"]},{o["w"]}x{o["h"]}' for o in doc["ops"]])

    gl_error = None
    backend = None
    img = None
    t = time.perf_counter()
    if "--cpu" not in sys.argv[1:]:
        try:
            img = render.render_gl(doc["ops"], engine.VIEWPORT)
            backend = "moderngl/OpenGL 3.3 FBO"
        except Exception as exc:  # no hardware OpenGL context on this machine
            gl_error = f"{type(exc).__name__}: {exc}"
    else:
        gl_error = "skipped (--cpu)"
    if img is None:
        img = render.render_pillow(doc["ops"], engine.VIEWPORT)
        backend = f"Pillow fallback (GL: {gl_error})"
    img.save(BASE / "render.png")
    stage("render", t)
    print(f"[render] {backend}")

    t = time.perf_counter()
    ok = render.verify(BASE / "render.png", engine.VIEWPORT)
    stage("verify", t)

    total = (time.perf_counter() - t_start) * 1000.0
    times["total"] = total
    print(f"[time] {'TOTAL':<12} {total:7.2f} ms")

    status = "FAILED" if not ok else ("PARTIAL" if gl_error else "SUCCESS")
    print(f"[status] {status} (backend: {'Pillow CPU' if gl_error else 'OpenGL'})")
    (BASE / "run_report.json").write_text(json.dumps({
        "status": status,
        "backend": backend,
        "gl_error": gl_error,
        "rule_count": len(rules),
        "times_ms": {k: round(v, 3) for k, v in times.items()},
        "verify_ok": ok,
    }, indent=2) + "\n", encoding="utf-8")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
