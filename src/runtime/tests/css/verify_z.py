#!/usr/bin/env python3
"""Pixel + layout acceptance for blob v5 `z-index` / stacking contexts.

The fixture is a real HTML/CSS page compiled by `frontend-rust --emit-blob`.
Eleven isolated regions exercise the CSS stacking order implemented by the
runtime:

  * positioned siblings z 1/2 and the same pair in swapped DOM order;
  * positioned z 0 before positioned z auto (CSS step 8, tree order);
  * a negative context painting behind in-flow content;
  * a z 100 child that cannot escape a z 1 parent (root z 2 wins);
  * opacity + z (the group allocates a texture, the z box only ordering);
  * nested contexts; z on a relative node; z on a static node (ignored);
  * equal-z siblings, where tree order breaks the tie;
  * a group clipped to nothing whose absolute escapes with the region clip.

Colors are analytic and probes are axis-aligned interior pixels, so the same
script runs against D3D11 and wgpu.

Usage:
  python verify_z.py --png out/render_z_d3d11.png \
      --report out/layout_z_d3d11.json --blob z/app.wnui \
      --dump out/wnui-dump.exe --backend d3d11
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from PIL import Image

CHECKS: list[dict] = []

PAGE_BG = (240, 240, 240)
GREEN = (34, 170, 102)       # #22aa66
RED = (204, 51, 51)          # #cc3333
BLUE = (51, 102, 204)        # #3366cc
YELLOW = (204, 170, 34)      # #ccaa22
HOST = (221, 221, 221)       # #dddddd
OPAQUE_BLUE = (0, 0, 255)    # #0000ff
GROUP_RED = (248, 120, 120)  # red at 0.5 over the page


def check(group: str, name: str, ok: bool, detail: str) -> bool:
    CHECKS.append({"group": group, "name": name, "ok": bool(ok), "detail": detail})
    print(f"[{'ok  ' if ok else 'FAIL'}] {group}: {name} - {detail}")
    return bool(ok)


def px(img: Image.Image, x: float, y: float) -> tuple[int, int, int]:
    return img.getpixel((int(round(x)), int(round(y))))[:3]


def close(actual: tuple[int, int, int], expected: tuple[int, int, int],
          tolerance: int = 0) -> bool:
    return all(abs(a - b) <= tolerance for a, b in zip(actual, expected))


def node_with_id(nodes: list[dict], element_id: str) -> dict | None:
    needle = f"#{element_id}"
    for node in nodes:
        if needle in str(node.get("name", "")):
            return node
    return None


def rect_of(nodes: list[dict], element_id: str) -> tuple[int, int, int, int]:
    node = node_with_id(nodes, element_id)
    if node is None:
        raise KeyError(element_id)
    return (int(round(node["x"])), int(round(node["y"])),
            int(round(node["w"])), int(round(node["h"])))


EXPECTED_RECTS = {
    "sib-a": (20, 30, 70, 70),
    "sib-b": (60, 30, 70, 70),
    "swap-a": (155, 30, 70, 70),
    "swap-b": (195, 30, 70, 70),
    "auto-a": (280, 30, 70, 70),
    "zero-a": (320, 30, 70, 70),
    "flowbox": (20, 170, 70, 70),
    "neg": (60, 170, 70, 70),
    "trap-parent": (145, 150, 120, 120),
    "trap-child": (150, 155, 60, 60),
    "trap-sibling": (170, 175, 80, 80),
    "ogroup": (270, 150, 80, 80),
    "otop": (310, 150, 70, 70),
    "nested-a": (10, 290, 110, 110),
    "nested-b": (15, 295, 60, 60),
    "nested-d": (30, 310, 80, 80),
    "rel-base": (155, 300, 100, 80),
    "rel-z": (155, 300, 50, 80),
    "static-z": (280, 310, 70, 70),
    "abs-over": (310, 310, 70, 70),
    "step8-red": (20, 440, 70, 70),
    "step8-blue": (40, 440, 70, 70),
    "tie-a": (155, 440, 70, 70),
    "tie-b": (185, 440, 70, 70),
    "f1-clip": (270, 430, 100, 0),
    "f1-group": (270, 430, 100, 60),
    "f1-escape": (280, 450, 40, 40),
}

EXPECTED_Z = {
    "sib-a": 1,
    "sib-b": 2,
    "swap-a": 2,
    "swap-b": 1,
    "auto-a": "auto",
    "zero-a": 0,
    "neg": -1,
    "trap-child": 100,
    "trap-sibling": 2,
    "ogroup": 2,
    "otop": 3,
    "nested-b": 10,
    "nested-d": 5,
    "rel-z": 1,
    "static-z": 5,
    "abs-over": 2,
    "step8-red": 0,
    "step8-blue": "auto",
    "tie-a": 1,
    "tie-b": 1,
    "f1-escape": "auto",
}

PROBES = (
    # group, name, x, y, expected, tolerance
    ("page", "top-left background", 5, 5, PAGE_BG, 0),
    ("page", "bottom-right background", 395, 415, PAGE_BG, 0),
    # Row 1a: z 1 then z 2.
    ("siblings", "z 1 only", 25, 60, GREEN, 0),
    ("siblings", "z 2 over the overlap", 80, 60, RED, 0),
    ("siblings", "z 2 only", 125, 60, RED, 0),
    # Row 1b: swapped DOM order.
    ("swapped", "z 2 first in the DOM", 160, 60, BLUE, 0),
    ("swapped", "z 2 beats the later z 1 in the overlap", 210, 60, BLUE, 0),
    ("swapped", "later z 1 only", 255, 60, YELLOW, 0),
    # Row 1c: auto vs zero.
    ("auto/zero", "z auto only", 285, 60, GREEN, 0),
    ("auto/zero", "z 0 over the overlap", 340, 60, RED, 0),
    ("auto/zero", "z 0 only", 385, 60, RED, 0),
    # Row 2a: negative behind in-flow.
    ("negative", "in-flow only", 25, 200, GREEN, 0),
    ("negative", "in-flow beats the negative in the overlap", 70, 200, GREEN, 0),
    ("negative", "negative only", 115, 200, RED, 0),
    # Row 2b: child cannot escape parent.
    ("escape", "parent background", 147, 152, HOST, 0),
    ("escape", "z 100 child outside the sibling", 153, 158, GREEN, 0),
    ("escape", "root z 2 sibling beats the trapped child", 200, 200, RED, 0),
    ("escape", "root z 2 sibling only", 245, 250, RED, 0),
    # Row 2c: opacity + z.
    ("opacity", "group red composites once at 0.5", 280, 190, GROUP_RED, 2),
    ("opacity", "z 3 box over the group", 330, 190, OPAQUE_BLUE, 0),
    ("opacity", "z 3 box only", 350, 215, OPAQUE_BLUE, 0),
    # Row 3a: nested contexts.
    ("nested", "parent background", 12, 292, HOST, 0),
    ("nested", "z 10 child inside the z 1 parent", 17, 297, GREEN, 0),
    ("nested", "root z 5 sibling over the parent block", 50, 330, RED, 0),
    ("nested", "root z 5 sibling only", 95, 380, RED, 0),
    # Row 3b: z on a relative node.
    ("relative", "relative z 1 over the in-flow box", 160, 340, RED, 0),
    ("relative", "in-flow box outside the relative box", 210, 340, GREEN, 0),
    # Row 3c: static z is ignored.
    ("static", "static box only", 290, 340, GREEN, 0),
    ("static", "z 2 absolute over the static z 5", 330, 340, RED, 0),
    # Row 4a: positioned z 0 before positioned z auto (CSS step 8).
    ("step8", "z 0 only", 25, 475, RED, 0),
    ("step8", "later z auto over the z 0", 60, 475, BLUE, 0),
    ("step8", "z auto only", 105, 475, BLUE, 0),
    # Row 4b: equal z, tree order decides.
    ("same-z", "first equal-z box only", 160, 475, GREEN, 0),
    ("same-z", "later equal-z box over the first", 200, 475, RED, 0),
    ("same-z", "later equal-z box only", 240, 475, RED, 0),
    # Row 4c: empty-clip group does not swallow its escaping absolute.
    ("empty-clip", "escaping absolute paints", 285, 455, BLUE, 0),
    ("empty-clip", "clipped group background is not painted", 275, 435, PAGE_BG, 0),
    ("empty-clip", "group area beside the absolute stays page", 350, 470, PAGE_BG, 0),
)


def dump_checks(dump: Path, blob: Path) -> bool:
    completed = subprocess.run(
        [str(dump), "--json", str(blob)],
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        return check("dump", "wnui-dump --json", False,
                     (completed.stdout + completed.stderr).strip())
    document = json.loads(completed.stdout)
    ok = check("dump", "format is wnui/5",
               document.get("format") == "wnui/5",
               f"{document.get('format')!r}")
    nodes = document.get("nodes") or []
    for element_id, expected in EXPECTED_Z.items():
        node = node_with_id(nodes, element_id)
        if node is None:
            ok = check("dump", element_id, False, "node missing") and ok
            continue
        actual = node.get("z_index")
        ok = check("dump", element_id, actual == expected,
                   f"{actual!r} == {expected!r}") and ok
    return ok


def layout_checks(nodes: list[dict]) -> bool:
    ok = True
    for element_id, expected in EXPECTED_RECTS.items():
        try:
            actual = rect_of(nodes, element_id)
        except KeyError:
            ok = check("layout", element_id, False, "node missing") and ok
            continue
        ok = check("layout", element_id, actual == expected,
                   f"{actual} == {expected}") and ok
    return ok


def color_checks(img: Image.Image, backend: str) -> bool:
    ok = True
    for group, name, x, y, expected, tolerance in PROBES:
        actual = px(img, x, y)
        ok = check(f"raster/{group}/{backend}", name,
                   close(actual, expected, tolerance),
                   f"({x},{y})={actual} expected {expected}") and ok
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description="z-index fixture verifier")
    parser.add_argument("--png", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--blob", required=True)
    parser.add_argument("--dump", required=True)
    parser.add_argument("--backend", choices=("wgpu", "d3d11"), default="d3d11")
    args = parser.parse_args()

    ok = dump_checks(Path(args.dump), Path(args.blob))
    report = json.loads(Path(args.report).read_text(encoding="utf-8"))
    sizes = {f"{item['width']}x{item['height']}": item for item in report["sizes"]}
    ok = check("report", "400x550 layout present", "400x550" in sizes,
               f"sizes={sorted(sizes)}") and ok
    if "400x550" in sizes:
        ok = layout_checks(sizes["400x550"]["nodes"]) and ok

    img = Image.open(args.png).convert("RGB")
    ok = check(f"raster/{args.backend}", "PNG size", img.size == (400, 550),
               f"{img.size}") and ok
    if img.size == (400, 550):
        ok = color_checks(img, args.backend) and ok

    failed = [item for item in CHECKS if not item["ok"]]
    print(f"\nz verify ({args.backend}): "
          f"{len(CHECKS) - len(failed)}/{len(CHECKS)} passed")
    if failed:
        for item in failed:
            print(f"  FAILED {item['group']}: {item['name']} - {item['detail']}")
        return 1
    print(f"z verify ({args.backend}): PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
