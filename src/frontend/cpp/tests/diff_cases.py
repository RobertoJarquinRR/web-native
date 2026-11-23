#!/usr/bin/env python3
"""Differential tester: C++ frontend vs Python reference on extra CSS cases.

The official parity contract is the two benchmark fixtures.  This optional
suite exercises the same semantics with 21 extra documents (shorthands,
flex variants, specificity/!important/inline styles, attribute selectors,
fractional geometry rounding, negative margins, nested auto sizes...) and
compares the C++ IR byte-for-byte with the Python reference.

Run from anywhere (the Python venv must exist):
    python src/frontend/cpp/tests/diff_cases.py
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent          # src/frontend/cpp/tests
CPP_DIR = HERE.parent                           # src/frontend/cpp
ROOT = HERE.parents[3]                          # repo root
PARITY = ROOT / "benchmarks" / "frontend-parity"
CPP = CPP_DIR / "build" / "frontend-cpp.exe"
PY = ROOT / "src" / "frontend" / "python" / ".venv" / "Scripts" / "python.exe"
PYS = ROOT / "src" / "frontend" / "python" / "frontend_only.py"
TMP = CPP_DIR / "build" / "diff-tests"
sys.path.insert(0, str(PARITY / "tools"))
from verify_ir import compare, load  # noqa: E402

CASES = []


def case(name, css, body, width=400, height=300):
    html = f"""<!doctype html>
<html><head><link rel="stylesheet" href="style.css"></head>
<body>
{body}
</body></html>
"""
    CASES.append((name, html, css, width, height))


case("shorthand-block", """
body { margin: 0; background: #fff; }
#root { width: 300px; padding: 10px 20px 30px 40px; background: #111; }
.a { height: 10px; margin: 1px 2px 3px 4px; background: #010203; }
.b { height: 20px; padding: 1px 2px; margin: 5px 6px 7px 8px; background: #0a0b0c; }
.c { height: 30px; width: 100px; margin: 9px; background: #d0e0f0; }
""", '<div id="root"><div class="a"></div><div class="b"></div><div class="c"></div></div>')

case("flex-col-between-center", """
body { margin: 0; background: #202020; }
#root { width: 200px; height: 200px; padding: 10px; background: #fff;
        display: flex; flex-direction: column;
        justify-content: space-between; align-items: center; }
.a { width: 50px; height: 20px; background: #f00; }
.b { width: 60px; height: 30px; background: #0f0; }
""", '<div id="root"><div class="a"></div><div class="b"></div></div>')

case("flex-row-stretch-auto", """
body { margin: 0; background: #fff; }
#root { width: 300px; padding: 5px; display: flex; gap: 10px;
        align-items: stretch; background: #eee; }
.a { width: 50px; background: #123; }
.b { width: 70px; height: 25px; background: #456; }
""", '<div id="root"><div class="a"></div><div class="b"></div></div>')

case("flex-row-end-single", """
body { background: #000; }
#root { width: 120px; height: 60px; background: #fff; display: flex;
        justify-content: flex-end; align-items: flex-end; padding: 5px; }
.a { width: 20px; height: 10px; background: #abc; }
""", '<div id="root"><div class="a"></div></div>')

case("specificity-important-inline", """
body { margin: 0; background: #010101; }
div { background: #111111; }
.a { background: #222222; }
#x { background: #333333; }
div.a#x { background: #444444; }
.a { background: #555555 !important; }
""", '<div id="x" class="a" style="background: #666666; width: 40px; height: 40px"></div>')

case("selectors-descendant-child-comma", """
body { margin: 0; background: #fff; }
#root { padding: 10px; background: #eee; }
.wrap .leaf { background: #f00; height: 10px; }
#root .wrap > .leaf { background: #0f0; }
.leaf, .other { width: 20px; }
.other { height: 5px; background: #00f; }
""", '<div id="root"><div class="wrap"><div class="leaf"></div><div class="other"></div></div></div>')

case("display-none-inline", """
body { margin: 0; background: #fafafa; }
#root { width: 100px; height: 100px; background: #eee; padding: 2px; }
.none { display: none; background: #f00; width: 10px; height: 10px; }
.inl { display: inline; background: #0f0; width: 10px; height: 10px; }
.blk { width: 10px; height: 10px; background: #00f; }
""", '<div id="root"><div class="none"></div><div class="inl"></div><div class="blk"></div></div>')

case("colors", """
body { margin: 0; background: #abc; }
#root { width: 100px; height: 100px; background: #0F0F0F; }
.a { width: 10px; height: 10px; background: #f00; }
.b { width: 10px; height: 10px; background: rgb(1, 2, 3); }
.c { width: 10px; height: 10px; background-color: #00ff00; background: #f0f; }
.d { width: 10px; height: 10px; background: rgba(10, 20, 30, 0.5); }
.e { width: 10px; height: 10px; background: transparent; }
.f { width: 10px; height: 10px; background: red; }
.g { width: 10px; height: 10px; background: #12345; }
""", '<div id="root"><div class="a"></div><div class="b"></div><div class="c"></div><div class="d"></div><div class="e"></div><div class="f"></div><div class="g"></div></div>')

case("gap-fallback", """
body { margin: 0; background: #fff; }
#root { width: 200px; display: flex; gap: 0; column-gap: 10px; background: #eee; padding: 5px; }
.a { width: 20px; height: 20px; background: #f00; }
.b { width: 30px; height: 20px; background: #0f0; }
""", '<div id="root"><div class="a"></div><div class="b"></div></div>')

case("auto-width-margins", """
body { margin: 0; background: #fff; }
#root { width: 200px; height: 100px; padding: 10px; background: #eee; }
.a { margin-left: 20px; margin-right: 30px; height: 10px; background: #f00; }
.b { width: 50%; height: 10px; background: #0f0; }
""", '<div id="root"><div class="a"></div><div class="b"></div></div>')

case("flex-auto-height-column", """
body { margin: 0; background: #fff; }
#root { width: 200px; display: flex; flex-direction: column; gap: 7px;
        justify-content: space-between; background: #eee; padding: 5px; }
.a { width: 30px; height: 10px; background: #f00; }
.b { width: 40px; height: 20px; background: #0f0; }
""", '<div id="root"><div class="a"></div><div class="b"></div></div>')

case("nested-flex", """
body { margin: 0; background: #000; }
#root { width: 260px; height: 120px; padding: 4px; display: flex; background: #111; }
.col { flex-direction: column; display: flex; gap: 3px; width: 100px; }
.row { display: flex; flex-direction: row; gap: 2px; height: 30px; align-items: center; }
.x { width: 10px; height: 10px; background: #f00; }
.y { width: 20px; height: 20px; background: #0f0; }
.z { width: 15px; height: 15px; background: #00f; }
""", '<div id="root"><div class="col"><div class="row"><div class="x"></div><div class="y"></div></div><div class="z"></div></div></div>')

case("body-padding", """
body { margin: 5px 10px; padding: 7px; background: #222; }
#root { height: 50px; background: #ddd; }
""", '<div id="root"></div>')

case("unsupported-pseudo-attr", """
body { margin: 0; background: #fff; }
div:hover { background: #f00; }
[data-x] { background: #0f0; }
#root { width: 100px; height: 50px; }
""", '<div id="root" data-x="1"></div>')

case("fractional-center", """
body { margin: 0; background: #fff; }
#root { width: 200px; display: flex; justify-content: center; background: #eee; }
.a { width: 61px; height: 10px; background: #f00; }
.b { width: 61px; height: 10px; background: #0f0; }
.c { width: 61px; height: 10px; background: #00f; }
""", '<div id="root"><div class="a"></div><div class="b"></div><div class="c"></div></div>')

case("fractional-between", """
body { margin: 0; background: #fff; }
#root { width: 100px; display: flex; justify-content: space-between; background: #eee; }
.a { width: 31px; height: 10px; background: #f00; }
.b { width: 31px; height: 10px; background: #0f0; }
.c { width: 31px; height: 10px; background: #00f; }
""", '<div id="root"><div class="a"></div><div class="b"></div><div class="c"></div></div>')

case("column-end", """
body { margin: 0; background: #fff; }
#root { width: 150px; height: 150px; display: flex; flex-direction: column;
        justify-content: flex-end; align-items: flex-end; background: #eee; padding: 5px; }
.a { width: 20px; height: 10px; background: #f00; }
.b { width: 40px; height: 20px; background: #0f0; }
""", '<div id="root"><div class="a"></div><div class="b"></div></div>')

case("inline-margin", """
body { margin: 0; background: #fff; }
#root { width: 200px; height: 100px; padding: 3px; background: #eee; }
.a { width: 20px; height: 10px; background: #f00; }
""", '<div id="root"><div class="a" style="margin: 5px 10px 15px 20px; padding: 1px 2px 3px 4px"></div></div>')

case("negative-margin-percent-width", """
body { margin: 0; background: #fff; }
#root { width: 200px; padding: 4px; background: #eee; }
.a { height: 10px; margin-top: -5px; margin-left: -3px; width: 50%; background: #f00; }
.b { height: 10px; margin-bottom: 7px; width: 75%; background: #0f0; }
""", '<div id="root"><div class="a"></div><div class="b"></div></div>')

case("empty-body", """
body { margin: 0; background: #123456; }
""", '')

case("auto-height-block-nested", """
body { margin: 0; background: #fff; }
#root { width: 300px; padding: 10px; background: #eee; }
.a { padding: 5px; background: #ddd; }
.b { height: 20px; margin-top: 4px; background: #f00; }
""", '<div id="root"><div class="a"><div class="b"></div></div></div>')


def run(cmd, cwd):
    proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"command failed ({proc.returncode}): {cmd}\n{proc.stdout}\n{proc.stderr}")
    return proc


def main() -> int:
    if TMP.exists():
        shutil.rmtree(TMP)
    TMP.mkdir(parents=True)
    failures = 0
    for name, html, css, width, height in CASES:
        d = TMP / name
        d.mkdir()
        (d / "index.html").write_text(html, encoding="utf-8")
        (d / "style.css").write_text(css, encoding="utf-8")
        py_ir = d / "py.ir.json"
        cpp_ir = d / "cpp.ir.json"
        run([str(PY), str(PYS), "--html", str(d / "index.html"), "--width", str(width),
             "--height", str(height), "--out", str(py_ir)], ROOT)
        run([str(CPP), "--html", str(d / "index.html"), "--width", str(width),
             "--height", str(height), "--out", str(cpp_ir)], ROOT)
        errors = compare(load(py_ir), load(cpp_ir))
        same_bytes = py_ir.read_bytes() == cpp_ir.read_bytes()
        if errors:
            failures += 1
            print(f"FAIL {name}")
            for error in errors[:8]:
                print("   ", error)
        else:
            print(f"PASS {name} (ops={len(load(py_ir).get('ops', []))}, bytes_equal={same_bytes})")
    print(f"\n{len(CASES) - failures}/{len(CASES)} cases match")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
