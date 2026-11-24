#!/usr/bin/env python3
"""Compare the Rust canonical style tree (ui.json) with the Python oracle dump.

The Python oracle emits the same projection with `emit_ui.py --dump-style-json`
(schema webnative.style-compare/1). Rust emits the full ui-tree via
`frontend-rust --emit-ui`; this tool projects the fields both sides agree on.

Why not compare ui.json verbatim: the Python oracle models the v1 subset and
does not carry the pilot extensions (grid/image/border/shadow) as nested data.
The projection is therefore geometry + box model, which is exactly what the
layout engine consumes.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

FIELDS = ("kind", "width_px", "height_px", "width_pct", "height_pct",
          "margin", "padding", "gap", "box_sizing")
TOLERANCE = 0.01


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def project_rust(tree: dict) -> list[dict]:
    nodes = []
    for item in tree.get("nodes") or []:
        style = item.get("style") or {}
        nodes.append({
            "name": item.get("name"),
            "kind": item.get("kind"),
            "width_px": float(style.get("width_px", -1.0)),
            "height_px": float(style.get("height_px", -1.0)),
            "width_pct": float(style.get("width_pct", 0.0)),
            "height_pct": float(style.get("height_pct", 0.0)),
            "margin": [float(v) for v in style.get("margin") or [0, 0, 0, 0]],
            "padding": [float(v) for v in style.get("padding") or [0, 0, 0, 0]],
            "gap": float(style.get("gap", 0.0)),
            "box_sizing": str(style.get("box_sizing") or "content-box"),
        })
    return nodes


def project_python(dump: dict) -> list[dict]:
    nodes = []
    for item in dump.get("nodes") or []:
        nodes.append({
            "name": item.get("name"),
            "kind": item.get("kind"),
            "width_px": float(item.get("width_px", -1.0)),
            "height_px": float(item.get("height_px", -1.0)),
            "width_pct": float(item.get("width_pct", 0.0)),
            "height_pct": float(item.get("height_pct", 0.0)),
            "margin": [float(v) for v in item.get("margin") or [0, 0, 0, 0]],
            "padding": [float(v) for v in item.get("padding") or [0, 0, 0, 0]],
            "gap": float(item.get("gap", 0.0)),
            "box_sizing": str(item.get("box_sizing") or "content-box"),
        })
    return nodes


def same(a, b) -> bool:
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, float) or isinstance(b, float):
        try:
            return abs(float(a) - float(b)) <= TOLERANCE
        except (TypeError, ValueError):
            return a == b
    return a == b


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rust", required=True, help="ui.json from frontend-rust --emit-ui")
    parser.add_argument("--python", required=True, help="dump from emit_ui.py --dump-style-json")
    parser.add_argument("--label", default="style-tree")
    args = parser.parse_args(argv)

    rust = project_rust(load(Path(args.rust)))
    python = project_python(load(Path(args.python)))

    problems: list[str] = []
    if len(rust) != len(python):
        problems.append(f"node count: rust={len(rust)} python={len(python)}")
    for index, (left, right) in enumerate(zip(rust, python)):
        if left.get("name") != right.get("name"):
            problems.append(f"node[{index}] name: {left.get('name')} != {right.get('name')}")
            continue
        for field in FIELDS:
            if not same(left[field], right[field]):
                problems.append(
                    f"node[{index}] {left['name']}.{field}: {left[field]!r} != {right[field]!r}")

    for item in problems[:40]:
        print(f"DIFF {item}")
    print(f"{args.label}: {len(rust)} nodes, {len(problems)} difference(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
