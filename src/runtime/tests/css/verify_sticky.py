#!/usr/bin/env python3
"""Pixel + layout acceptance for `position: sticky`.

The fixture pins a header to the top of a vertical scroller. The scrolled PNG is
rendered after a wheel event: the header keeps its scrollport rect while the
stripes move under it.
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
GREEN = (34, 170, 102)
YELLOW = (204, 170, 34)
ORANGE = (170, 102, 34)

EXPECTED_RECTS = {
    "scroller": (20, 20, 240, 160),
    "content": (20, 20, 240, 510),
    "sticky": (20, 20, 240, 30),
    "t1": (20, 50, 240, 60),
}

EXPECTED_STYLE = {
    "sticky": ("sticky", [0.0, 0.0, 0.0, 0.0], 0b0001),
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
    for element_id, expected in EXPECTED_STYLE.items():
        node = node_with_id(nodes, element_id)
        if node is None:
            ok = check("dump", element_id, False, "node missing") and ok
            continue
        position, inset, mask = expected
        actual = (node.get("position"), node.get("inset"), node.get("inset_mask"))
        ok = check("dump", element_id, actual == (position, inset, mask),
                   f"{actual} == {(position, inset, mask)}") and ok
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


def color_checks_scroll0(img: Image.Image) -> bool:
    ok = True

    def probe(name: str, point: tuple[float, float],
              expected: tuple[int, int, int]) -> bool:
        actual = px(img, *point)
        return check("raster", name, actual == expected,
                     f"{point}={actual} expected {expected}")

    ok = probe("page background", (350, 290), PAGE_BG) and ok
    ok = probe("sticky header at the scrollport top", (30, 30), RED) and ok
    ok = probe("first stripe under the header", (30, 60), BLUE) and ok
    ok = probe("second stripe below the first", (30, 120), GREEN) and ok
    return ok


def color_checks_scrolled(img: Image.Image) -> bool:
    ok = True

    def probe(name: str, point: tuple[float, float],
              expected: tuple[int, int, int]) -> bool:
        actual = px(img, *point)
        return check("raster scrolled", name, actual == expected,
                     f"{point}={actual} expected {expected}")

    ok = probe("sticky header stayed pinned", (30, 30), RED) and ok
    ok = probe("third stripe moved under the header", (30, 60), YELLOW) and ok
    ok = probe("fourth stripe follows the scroll", (30, 120), ORANGE) and ok
    ok = probe("unchanged page background", (350, 290), PAGE_BG) and ok
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description="sticky fixture verifier")
    parser.add_argument("--png", required=True)
    parser.add_argument("--scrolled", required=True)
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
        ok = color_checks_scroll0(img) and ok

    scrolled = Image.open(args.scrolled).convert("RGB")
    ok = check(f"raster/{args.backend}", "scrolled PNG size",
               scrolled.size == (400, 300), f"{scrolled.size}") and ok
    if scrolled.size == (400, 300):
        ok = color_checks_scrolled(scrolled) and ok

    failed = [item for item in CHECKS if not item["ok"]]
    print(f"\nsticky verify ({args.backend}): "
          f"{len(CHECKS) - len(failed)}/{len(CHECKS)} passed")
    if failed:
        for item in failed:
            print(f"  FAILED {item['group']}: {item['name']} - {item['detail']}")
        return 1
    print(f"sticky verify ({args.backend}): PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
