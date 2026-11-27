#!/usr/bin/env python3
"""Pixel + layout acceptance for `z-index` on static flex/grid items.

CSS applies z-index to flex and grid items even when `position: static`; the
item creates a stacking context and orders over its siblings. The fixture keeps
every item static and overlaps them with negative margins, so a missing context
or a trapped child changes the sampled color.
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
RED = (204, 51, 51)
BLUE = (51, 102, 204)
BLACK = (0, 0, 0)

EXPECTED_RECTS = {
    "row1": (0, 10, 300, 40),
    "a1": (0, 10, 150, 40),
    "a2": (75, 10, 150, 40),
    "row2": (0, 60, 300, 40),
    "b1": (0, 60, 150, 40),
    "b2": (75, 60, 150, 40),
    "grid": (0, 110, 300, 40),
    "g1": (0, 110, 100, 40),
    "g2": (50, 110, 150, 40),
    "row3": (0, 160, 300, 40),
    "nest": (0, 160, 150, 40),
    "deep": (0, 160, 120, 40),
    "top": (75, 160, 150, 40),
}

EXPECTED_Z = {
    "a1": 1,
    "a2": 2,
    "b1": 2,
    "b2": 1,
    "g1": 1,
    "g2": 2,
    "nest": 1,
    "deep": 100,
    "top": 2,
}


def check(group: str, name: str, ok: bool, detail: str) -> bool:
    CHECKS.append({"group": group, "name": name, "ok": bool(ok), "detail": detail})
    print(f"[{'ok  ' if ok else 'FAIL'}] {group}: {name} - {detail}")
    return bool(ok)


def px(img: Image.Image, x: float, y: float) -> tuple[int, int, int]:
    return img.getpixel((int(round(x)), int(round(y))))[:3]


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


def dump_checks(dump: Path, blob: Path) -> bool:
    completed = subprocess.run([str(dump), "--json", str(blob)],
                               capture_output=True, text=True)
    if completed.returncode != 0:
        return check("dump", "wnui-dump --json", False,
                     (completed.stdout + completed.stderr).strip())
    document = json.loads(completed.stdout)
    ok = check("dump", "format is wnui/6",
               document.get("format") == "wnui/6", f"{document.get('format')!r}")
    nodes = document.get("nodes") or []
    for element_id, expected in EXPECTED_Z.items():
        node = node_with_id(nodes, element_id)
        if node is None:
            ok = check("dump", element_id, False, "node missing") and ok
            continue
        actual = node.get("z_index")
        ok = check("dump", element_id, actual == expected,
                   f"z_index {actual} == {expected}") and ok
        if element_id not in ("deep",):
            ok = check("dump", f"{element_id} static",
                       node.get("position") == "static",
                       f"position {node.get('position')}") and ok
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


def color_checks(img: Image.Image) -> bool:
    ok = True

    def probe(name: str, point: tuple[float, float],
              expected: tuple[int, int, int]) -> bool:
        actual = px(img, *point)
        return check("raster", name, actual == expected,
                     f"{point}={actual} expected {expected}")

    ok = probe("page background", (350, 290), PAGE_BG) and ok
    ok = probe("flex z 1 base", (25, 30), RED) and ok
    ok = probe("flex z 2 item paints over z 1", (100, 30), BLUE) and ok
    ok = probe("swapped flex z 2 keeps the first item on top", (100, 80), RED) and ok
    ok = probe("swapped flex z 1 stays below", (25, 80), RED) and ok
    ok = probe("grid z 1 base", (25, 130), RED) and ok
    ok = probe("grid z 2 item paints over z 1", (75, 130), BLUE) and ok
    ok = probe("nested z 100 paints inside its z 1 item", (25, 180), BLACK) and ok
    ok = probe("nested context cannot escape the sibling z 2", (100, 180), BLUE) and ok
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description="static flex/grid z-index verifier")
    parser.add_argument("--png", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--blob", required=True)
    parser.add_argument("--dump", required=True)
    parser.add_argument("--backend", choices=("wgpu", "d3d11"), default="d3d11")
    args = parser.parse_args()

    ok = dump_checks(Path(args.dump), Path(args.blob))
    report = json.loads(Path(args.report).read_text(encoding="utf-8"))
    sizes = {f"{item['width']}x{item['height']}": item for item in report["sizes"]}
    ok = check("report", "400x300 layout present", "400x300" in sizes,
               f"sizes={sorted(sizes)}") and ok
    if ok:
        ok = layout_checks(sizes["400x300"]["nodes"]) and ok

    img = Image.open(args.png).convert("RGB")
    ok = check(f"raster/{args.backend}", "PNG size", img.size == (400, 300),
               f"{img.size}") and ok
    if img.size == (400, 300):
        ok = color_checks(img) and ok

    failed = [item for item in CHECKS if not item["ok"]]
    print(f"\nzitem verify ({args.backend}): "
          f"{len(CHECKS) - len(failed)}/{len(CHECKS)} passed")
    if failed:
        for item in failed:
            print(f"  FAILED {item['group']}: {item['name']} - {item['detail']}")
        return 1
    print(f"zitem verify ({args.backend}): PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
