#!/usr/bin/env python3
"""Random differential fuzz: C++ vs Python reference within the SPEC subset.

Generates 120 deterministic random documents (seeded) inside the supported
subset — block/flex nesting, gaps, justify/align, px sizes, shorthands,
auto dimensions, colors and repeated selectors — and compares the C++ IR with
the Python reference byte-for-byte.  Optional; the official contract is the
two benchmark fixtures.

Run from anywhere (the Python venv must exist):
    python src/frontend/cpp/tests/fuzz_random.py
"""
from __future__ import annotations

import random
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
TMP = CPP_DIR / "build" / "fuzz-tests"
sys.path.insert(0, str(PARITY / "tools"))
from verify_ir import compare, load  # noqa: E402

random.seed(20261123)

DISPLAYS = ["block", "flex"]
DIRECTIONS = ["row", "column", "row", "column", "weird"]
JUSTIFY = ["flex-start", "center", "flex-end", "space-between", "start", "end"]
ALIGN = ["stretch", "flex-start", "center", "flex-end", "normal"]
COLORS = ["#e74c3c", "#2ecc71", "#3498db", "#f0f", "#123456", "#abc",
          "#0a0b0c", "rgb(10, 20, 30)", "rgba(1,2,3,0.5)"]


def val_px(lo=0, hi=80, decimal=False):
    if decimal:
        return f"{random.randint(lo * 10, hi * 10) / 10:g}px"
    return f"{random.randint(lo, hi)}px"


def box_props(cls, is_root=False):
    lines = []
    if random.random() < 0.85:
        lines.append(f"width: {val_px(0, 160, random.random() < 0.2)};")
    if random.random() < 0.6:
        lines.append(f"height: {val_px(0, 120, random.random() < 0.2)};")
    if random.random() < 0.7:
        lines.append("padding: " + " ".join(val_px(0, 12) for _ in range(random.randint(1, 4))) + ";")
    if random.random() < 0.7:
        lines.append("margin: " + " ".join(val_px(0, 12) for _ in range(random.randint(1, 4))) + ";")
    if random.random() < 0.2:
        lines.append(f"margin-top: {val_px(0, 20)};")
    if random.random() < 0.6:
        lines.append(f"display: {random.choice(DISPLAYS)};")
        if random.random() < 0.7:
            lines.append(f"flex-direction: {random.choice(DIRECTIONS)};")
        if random.random() < 0.7:
            lines.append(f"gap: {val_px(0, 15)};")
        if random.random() < 0.7:
            lines.append(f"justify-content: {random.choice(JUSTIFY)};")
        if random.random() < 0.7:
            lines.append(f"align-items: {random.choice(ALIGN)};")
    if random.random() < 0.9:
        lines.append(f"background: {random.choice(COLORS)};")
    return " ".join(lines)


class Node:
    def __init__(self, index):
        self.index = index
        self.cls = f"n{index}"
        self.children = []


def make_tree(counter, depth):
    node = Node(counter[0])
    counter[0] += 1
    if depth > 0:
        for _ in range(random.randint(1, 4)):
            node.children.append(make_tree(counter, depth - 1))
    return node


def emit_html(node, root=False):
    attrs = f'id="root" ' if root else ""
    attrs += f'class="{node.cls}' + (' alt' if random.random() < 0.25 else '') + '"'
    style = ""
    if random.random() < 0.1:
        style = ' style="margin-top: 4px; padding: 1px 2px;"'
    inner = "".join(emit_html(child) for child in node.children)
    return f'<div {attrs}{style}>{inner}</div>'


def emit_css(node, lines):
    lines.append(f".{node.cls} {{ {box_props(node.cls)} }}")
    if random.random() < 0.35:
        # same specificity, later rule: source order must win
        lines.append(f".{node.cls} {{ background: {random.choice(COLORS)}; }}")
    if random.random() < 0.2:
        lines.append(f"div.{node.cls} {{ height: {val_px(0, 80)}; }}")
    if random.random() < 0.15:
        lines.append(f".wrapper .{node.cls} {{ margin-top: {val_px(0, 10)}; }}")
    for child in node.children:
        emit_css(child, lines)


def main() -> int:
    total = 120
    if TMP.exists():
        shutil.rmtree(TMP)
    TMP.mkdir(parents=True)
    failures = 0
    for case_index in range(total):
        counter = [0]
        root = make_tree(counter, random.randint(1, 3))
        html = ("<!doctype html>\n<html><head>\n"
                '<link rel="stylesheet" href="style.css">\n</head><body>\n'
                + emit_html(root, root=True) + "\n</body></html>\n")
        lines = ["body { margin: 0; background: #0f1115; }",
                 f"#root {{ {box_props('root', True)} }}"]
        emit_css(root, lines)
        css = "\n".join(lines)
        d = TMP / f"case{case_index:03d}"
        d.mkdir()
        (d / "index.html").write_text(html, encoding="utf-8")
        (d / "style.css").write_text(css, encoding="utf-8")
        width = random.choice([320, 400, 480])
        height = random.choice([240, 300, 320])
        py_ir = d / "py.ir.json"
        cpp_ir = d / "cpp.ir.json"
        subprocess.run([str(PY), str(PYS), "--html", str(d / "index.html"),
                        "--width", str(width), "--height", str(height),
                        "--out", str(py_ir)], check=True, capture_output=True)
        subprocess.run([str(CPP), "--html", str(d / "index.html"),
                        "--width", str(width), "--height", str(height),
                        "--out", str(cpp_ir)], check=True, capture_output=True)
        errors = compare(load(py_ir), load(cpp_ir))
        if errors or py_ir.read_bytes() != cpp_ir.read_bytes():
            failures += 1
            print(f"FAIL case{case_index:03d} at {d}")
            for error in errors[:6]:
                print("   ", error)
            if not errors:
                print("    bytes differ but semantic compare passed")
            if failures >= 5:
                break
    print(f"\n{total - failures}/{total} random cases match")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
