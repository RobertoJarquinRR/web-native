#!/usr/bin/env python3
"""Pixel + layout acceptance for `position: relative|absolute`.

The fixture is a real HTML/CSS page compiled by `frontend-rust --emit-blob`.
Exact rects come from the runtime layout report (`--report`), and the PNG is
probed at the moved/placed corners and at the old flow slots, so a node that
stays in flow, an offset with the wrong sign or a missing containing block
fails here.

Usage:
  python verify_position.py --png out/render_position_d3d11.png \
      --report out/layout_position_d3d11.json --backend d3d11
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
FLOW = (51, 102, 204)        # #3366cc
REL = (34, 170, 102)         # #22aa66
REL2 = (170, 102, 34)        # #aa6622
ABS = (204, 51, 51)          # #cc3333
AFTER = (68, 68, 68)         # #444444
STRETCH = (153, 51, 204)     # #9933cc
ABSPCT = (0, 119, 170)       # #0077aa
HOST = (221, 221, 221)       # #dddddd
NESTED = (0, 0, 0)
ESCAPE = (204, 153, 0)       # #cc9900


def check(group: str, name: str, ok: bool, detail: str) -> bool:
    CHECKS.append({"group": group, "name": name, "ok": bool(ok), "detail": detail})
    print(f"[{'ok  ' if ok else 'FAIL'}] {group}: {name} - {detail}")
    return bool(ok)


def px(img: Image.Image, x: float, y: float) -> tuple[int, int, int]:
    return img.getpixel((int(round(x)), int(round(y))))[:3]


def node_named(nodes: list[dict], suffix: str) -> dict | None:
    for node in nodes:
        if str(node.get("name", "")).endswith(suffix):
            return node
    return None


def rect_of(nodes: list[dict], suffix: str) -> tuple[int, int, int, int]:
    node = node_named(nodes, suffix)
    if node is None:
        raise KeyError(suffix)
    return (int(round(node["x"])), int(round(node["y"])),
            int(round(node["w"])), int(round(node["h"])))


EXPECTED_RECTS = {
    "/div.flow": (0, 0, 100, 30),
    "/div.rel": (20, 40, 40, 30),
    "/div.rel2": (90, 55, 40, 30),
    "/div.abs": (150, 90, 60, 40),
    "/div.after": (0, 90, 60, 20),
    "/div.absstretch": (10, 150, 380, 40),
    "/div.abspct": (100, 150, 20, 10),
    "/div.nested-host": (200, 130, 80, 40),
    "/div.nested": (205, 135, 20, 15),
    "/div.escape": (300, 90, 30, 30),
}


EXPECTED_STYLE = {
    "/div.flow": ("static", [0.0, 0.0, 0.0, 0.0], 0),
    "/div.rel": ("relative", [10.0, 0.0, 0.0, 20.0], 0b1001),
    "/div.rel2": ("relative", [0.0, 10.0, 5.0, 0.0], 0b0110),
    "/div.abs": ("absolute", [90.0, 0.0, 0.0, 150.0], 0b1001),
    "/div.absstretch": ("absolute", [150.0, 10.0, 10.0, 10.0], 0b1111),
    "/div.abspct": ("absolute", [75.0, 0.0, 0.0, 25.0], 0b1001_1001),
    "/div.nested": ("absolute", [5.0, 0.0, 0.0, 5.0], 0b1001),
    "/div.escape": ("absolute", [90.0, 0.0, 0.0, 300.0], 0b1001),
}


def dump_checks(dump: Path, blob: Path) -> bool:
    """The v4 position/inset fields through the same debug projection."""
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
    for suffix, expected in EXPECTED_STYLE.items():
        node = node_named(nodes, suffix)
        if node is None:
            ok = check("dump", suffix, False, "node missing") and ok
            continue
        position, inset, mask = expected
        actual = (node.get("position"), node.get("inset"), node.get("inset_mask"))
        ok = check("dump", suffix, actual == (position, inset, mask),
                   f"{actual} == {(position, inset, mask)}") and ok
    return ok


def layout_checks(nodes: list[dict]) -> bool:
    ok = True
    for suffix, expected in EXPECTED_RECTS.items():
        try:
            actual = rect_of(nodes, suffix)
        except KeyError:
            ok = check("layout", suffix, False, "node missing") and ok
            continue
        ok = check("layout", suffix, actual == expected,
                   f"{actual} == {expected}") and ok
    return ok


def color_checks(img: Image.Image) -> bool:
    ok = True

    def probe(name: str, point: tuple[float, float],
              expected: tuple[int, int, int], group: str = "raster") -> bool:
        actual = px(img, *point)
        return check(group, name, actual == expected,
                     f"{point}={actual} expected {expected}")

    ok = probe("page background", (380, 5), PAGE_BG) and ok
    ok = probe("flow box", (50, 15), FLOW) and ok

    # Relative: new rect is painted, old flow slot is not.
    ok = probe("relative moved corner", (20, 40), REL) and ok
    ok = probe("relative kept its old slot empty", (5, 35), PAGE_BG) and ok
    ok = probe("relative right/bottom moved corner", (90, 55), REL2) and ok
    ok = probe("relative right/bottom old slot empty", (135, 60), PAGE_BG) and ok

    # The absolute block did not shift the next sibling.
    ok = probe("sibling after the absolute keeps its flow slot", (30, 95), AFTER) and ok
    ok = probe("gap above the sibling is empty", (30, 85), PAGE_BG) and ok
    ok = probe("absolute placed top-left", (150, 90), ABS) and ok
    ok = probe("absolute placed bottom-right", (209, 129), ABS) and ok
    ok = probe("left of the absolute is background", (149, 100), PAGE_BG) and ok
    ok = probe("above the absolute is background", (155, 89), PAGE_BG) and ok

    # left+right/top+bottom stretch.
    ok = probe("absolute stretch corner", (10, 150), STRETCH) and ok
    ok = probe("absolute stretch right edge", (389, 189), STRETCH) and ok
    ok = probe("outside the stretch is background", (395, 195), PAGE_BG) and ok
    ok = probe("percentage absolute is painted over the stretch", (110, 155), ABSPCT) and ok

    # Nearest positioned ancestor for the nested absolute.
    ok = probe("nested host background", (270, 160), HOST) and ok
    ok = probe("nested absolute inside the host", (210, 140), NESTED) and ok
    ok = probe("host corner outside the nested absolute", (201, 131), HOST) and ok

    # F1: an absolute whose containing block is above a fully clipped static
    # ancestor (height:0 + overflow:hidden) still paints at its placed rect.
    ok = probe("absolute escapes a fully clipped static ancestor", (310, 100), ESCAPE) and ok
    ok = probe("left of the escaped absolute is background", (299, 95), PAGE_BG) and ok
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description="position fixture verifier")
    parser.add_argument("--png", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--blob", required=True)
    parser.add_argument("--dump", required=True)
    parser.add_argument("--backend", choices=("wgpu", "d3d11"), default="d3d11")
    args = parser.parse_args()

    ok = dump_checks(Path(args.dump), Path(args.blob))
    report = json.loads(Path(args.report).read_text(encoding="utf-8"))
    sizes = {f"{item['width']}x{item['height']}": item for item in report["sizes"]}
    ok = check("report", "400x200 layout present", "400x200" in sizes,
               f"sizes={sorted(sizes)}") and ok
    if ok:
        ok = layout_checks(sizes["400x200"]["nodes"]) and ok

    img = Image.open(args.png).convert("RGB")
    ok = check(f"raster/{args.backend}", "PNG size", img.size == (400, 200),
               f"{img.size}") and ok
    if img.size == (400, 200):
        ok = color_checks(img) and ok

    failed = [item for item in CHECKS if not item["ok"]]
    print(f"\nposition verify ({args.backend}): "
          f"{len(CHECKS) - len(failed)}/{len(CHECKS)} passed")
    if failed:
        for item in failed:
            print(f"  FAILED {item['group']}: {item['name']} - {item['detail']}")
        return 1
    print(f"position verify ({args.backend}): PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
