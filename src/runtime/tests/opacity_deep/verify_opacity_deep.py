#!/usr/bin/env python3
"""Pixel acceptance for deeply nested opacity groups (blob v3).

Ten nested groups with `opacity: 0.9` and an opaque fill at the bottom. The
renderer allocates one full-viewport layer per nesting depth, so this drives
the per-depth target pool through its growth/realloc path on both backends.

Before the B2 fix, wgpu held a pointer into `layer_targets` across the
recursive call that reallocs it and crashed (0xC0000005) around this depth;
D3D11 was already iterative. The expected pixel is `0.9^10` composited over the
page, accumulated with 8-bit rounding at each layer.

Usage:
  python verify_opacity_deep.py --png render.png --report layout.json
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image

PAGE_BG = (240, 240, 240)
BLUE = (0, 0, 255)
STEPS = 10
ALPHA = 0.9
CHECKS: list[dict] = []


def check(group: str, name: str, ok: bool, detail: str) -> bool:
    CHECKS.append({"group": group, "name": name, "ok": bool(ok), "detail": detail})
    print(f"[{'ok  ' if ok else 'FAIL'}] {group}: {name} - {detail}")
    return bool(ok)


def px(img: Image.Image, x: float, y: float) -> tuple[int, int, int]:
    return img.getpixel((int(round(x)), int(round(y))))[:3]


def close(actual: tuple[int, int, int], expected: tuple[int, int, int],
          tolerance: int) -> bool:
    return all(abs(a - b) <= tolerance for a, b in zip(actual, expected))


def expected_color() -> tuple[int, int, int]:
    color = BLUE
    for _ in range(STEPS):
        color = tuple(int(round(c * ALPHA + b * (1.0 - ALPHA)))
                      for c, b in zip(color, PAGE_BG))
    return color


def main() -> int:
    parser = argparse.ArgumentParser(description="deep group opacity verifier")
    parser.add_argument("--png", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--backend", default="?")
    args = parser.parse_args()

    img = Image.open(args.png).convert("RGB")
    report = json.loads(Path(args.report).read_text(encoding="utf-8"))
    sizes = {f"{item['width']}x{item['height']}": item for item in report["sizes"]}
    if "400x300" not in sizes:
        print(f"FAIL: report has no 400x300 size: {sorted(sizes)}")
        return 1
    nodes = sizes["400x300"]["nodes"]
    backend = args.backend
    ok = check("capture", "PNG size", img.size == (400, 300), f"{img.size}")

    by_name = {str(node.get("name", "")): node for node in nodes}
    missing = [f"#d{i}.d" for i in range(1, STEPS + 1)
               if not any(name.endswith(f"#d{i}.d") for name in by_name)]
    ok = check("layout", "ten nested groups are present", not missing,
               f"missing {missing}") and ok

    fill = None
    for node in nodes:
        if str(node.get("name", "")).endswith("#deep-fill.fill"):
            fill = node
            break
    if fill is None:
        print("FAIL: deep fill node not found")
        return 1
    x, y, w, h = fill["x"], fill["y"], fill["w"], fill["h"]
    actual = px(img, x + w / 2, y + h / 2)
    expected = expected_color()
    ok = check(f"deep/{backend}", "10 nested 0.9 groups composite 0.9^10",
               close(actual, expected, 6),
               f"{actual} == {expected}") and ok

    # Outside the outermost group the page background must be untouched.
    ok = check(f"deep/{backend}", "outside the nested stack stays page background",
               close(px(img, 300, 220), PAGE_BG, 1),
               f"{px(img, 300, 220)} == {PAGE_BG}") and ok

    failed = [item for item in CHECKS if not item["ok"]]
    result = {
        "ok": not failed,
        "backend": backend,
        "total_checks": len(CHECKS),
        "failed_checks": len(failed),
        "checks": CHECKS,
    }
    Path(args.png).with_suffix(".json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"\nopacity deep verify ({backend}): {len(CHECKS) - len(failed)}/{len(CHECKS)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
