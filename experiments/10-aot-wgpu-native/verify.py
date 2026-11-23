#!/usr/bin/env python3
"""Verify the PNG produced by scene_gpu.exe against CONTRACT.md.

Build/CI-time only (Pillow is never a runtime dependency of the .exe).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image

BASE_CHECKS = [
    ("box-a", 60, 60, "#e74c3c"),
    ("box-b", 60, 156, "#3498db"),
    ("root", 200, 250, "#101014"),
]


def parse_hex(text: str) -> tuple[int, int, int]:
    digits = text.strip().lstrip("#")
    if len(digits) == 3:
        digits = "".join(ch * 2 for ch in digits)
    if len(digits) != 6:
        raise ValueError(f"invalid color {text!r}")
    return tuple(int(digits[i : i + 2], 16) for i in range(0, 6, 2))


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify scene_gpu.png pixels")
    parser.add_argument("--png", default="scene_gpu.png", type=Path)
    parser.add_argument("--report", default="verify_report.json", type=Path)
    parser.add_argument("--expect-size", default="400x300")
    parser.add_argument("--tolerance", default=8, type=int)
    parser.add_argument(
        "--extra",
        action="append",
        default=[],
        metavar="NAME,X,Y,#RRGGBB",
        help="additional pixel check (repeatable)",
    )
    args = parser.parse_args()

    expected_w, expected_h = (int(v) for v in args.expect_size.lower().split("x"))
    checks = list(BASE_CHECKS)
    for spec in args.extra:
        name, x, y, color = spec.split(",")
        checks.append((name, int(x), int(y), color))

    if not args.png.exists():
        print(f"error: {args.png} not found", file=sys.stderr)
        return 1

    image = Image.open(args.png).convert("RGBA")
    width, height = image.size
    size_ok = (width, height) == (expected_w, expected_h)

    results = []
    for name, x, y, color in checks:
        expected = parse_hex(color)
        actual = image.getpixel((x, y))
        diff = max(abs(actual[i] - expected[i]) for i in range(3))
        alpha_ok = actual[3] == 255
        ok = diff <= args.tolerance and alpha_ok
        results.append(
            {
                "name": name,
                "x": x,
                "y": y,
                "expected": "#%02x%02x%02x" % expected,
                "actual": "#%02x%02x%02x" % actual[:3],
                "actual_rgba": list(actual),
                "maxdiff": diff,
                "alpha_255": alpha_ok,
                "ok": ok,
            }
        )
        print(
            f"  {name:8s} ({x:3d},{y:3d}) expected #{expected[0]:02x}{expected[1]:02x}{expected[2]:02x} "
            f"actual #{actual[0]:02x}{actual[1]:02x}{actual[2]:02x} maxdiff={diff} -> {'OK' if ok else 'FAIL'}"
        )

    all_ok = size_ok and all(item["ok"] for item in results)
    report = {
        "png": str(args.png),
        "size": [width, height],
        "expected_size": [expected_w, expected_h],
        "size_ok": size_ok,
        "tolerance": args.tolerance,
        "checks": results,
        "passed": all_ok,
    }
    args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(f"verify: size={'OK' if size_ok else 'FAIL'} ({width}x{height}), "
          f"{sum(1 for r in results if r['ok'])}/{len(results)} pixel checks passed")
    print(f"verify: {'PASS' if all_ok else 'FAIL'} -> {args.report}")
    return 0 if all_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
