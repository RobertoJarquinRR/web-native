#!/usr/bin/env python3
"""Regenerate tests/golden/color_hover.h from the Python emitter (oracle).

The fixture `tests/fixtures/color_hover.{html,css}` is compiled by
`frontend-rust --emit-ui` (only to produce the tree that `ui.json` carries),
then `emit_ui.py --ui-json` writes the reference header.  The checked-in
golden is that output; the Rust `--emit-header` must be byte-identical.

Usage (repository root, project venv):

    .venv\\Scripts\\python.exe src/frontend/tests/make_color_hover_golden.py
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
RUST = ROOT / "src" / "frontend" / "target" / "release" / "frontend-rust.exe"
EMIT = ROOT / "src" / "emitter" / "python" / "emit_ui.py"
OUT = HERE / "golden" / "color_hover.h"
TMP = ROOT / "src" / "frontend" / "target" / "color_hover_golden"


def main() -> int:
    TMP.mkdir(parents=True, exist_ok=True)
    ui = TMP / "ui.json"
    subprocess.run(
        [
            str(RUST),
            "--html",
            str(HERE / "fixtures" / "color_hover" / "index.html"),
            "--css",
            str(HERE / "fixtures" / "color_hover" / "style.css"),
            "--width",
            "320",
            "--height",
            "200",
            "--emit-ui",
            str(ui),
        ],
        check=True,
        cwd=ROOT,
    )
    subprocess.run(
        [
            sys.executable,
            str(EMIT),
            "--ui-json",
            str(ui),
            "--out",
            str(OUT),
            "--meta",
            str(TMP / "ui_meta.json"),
        ],
        check=True,
        cwd=ROOT,
    )
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
