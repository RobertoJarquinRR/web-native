#!/usr/bin/env python3
"""Verify a rendered PNG against the display-list IR.

Verification runs at BUILD time (Pillow is allowed here only). It rebuilds the
expected image from the IR and compares every pixel, then checks the contract's
sample points explicitly.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

HEX = set("0123456789abcdef")
TOLERANCE = 8

CONTRACT_POINTS = [
    (60, 60, "#e74c3c", "box a"),
    (60, 156, "#3498db", "box b"),
    (200, 250, "#101014", "root background"),
]


def parse_color(value: str) -> tuple[int, int, int]:
    text = value.strip().lower()
    if not text.startswith("#"):
        raise ValueError(f"color must start with '#': {value!r}")
    digits = text[1:]
    if len(digits) == 3:
        digits = "".join(ch * 2 for ch in digits)
    elif len(digits) == 6:
        digits += "ff"
    elif len(digits) != 8:
        raise ValueError(f"bad color: {value!r}")
    if any(ch not in HEX for ch in digits):
        raise ValueError(f"bad color: {value!r}")
    return int(digits[0:2], 16), int(digits[2:4], 16), int(digits[4:6], 16)


def load_ir(path: Path) -> tuple[int, int, list[tuple[float, float, float, float, str]]]:
    doc = json.loads(path.read_text(encoding="utf-8-sig"))
    viewport = doc["viewport"]
    ops = [
        (float(op["x"]), float(op["y"]), float(op["w"]), float(op["h"]), op["fill"])
        for op in doc["ops"]
        if op.get("type") == "rect"
    ]
    if not ops:
        raise ValueError("IR has no rect ops")
    return int(viewport["width"]), int(viewport["height"]), ops


def expected_image(width: int, height: int, ops: list) -> Image.Image:
    image = Image.new("RGBA", (width, height), (0, 0, 0, 255))
    draw = ImageDraw.Draw(image)
    for x, y, w, h, fill in ops:
        color = parse_color(fill)
        draw.rectangle([x, y, x + w - 1, y + h - 1], fill=color + (255,))
    return image


def parse_point(text: str) -> tuple[int, int, tuple[int, int, int]]:
    parts = text.split(",")
    if len(parts) != 3:
        raise ValueError(f"point must be x,y,#hex: {text!r}")
    x, y, color = int(parts[0]), int(parts[1]), parts[2].strip()
    return x, y, parse_color(color)


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare a PNG against display_list.json")
    parser.add_argument("--png", default="scene.png", type=Path)
    parser.add_argument("--ir", default="display_list.json", type=Path)
    parser.add_argument("--report", default="verify_report.json", type=Path)
    parser.add_argument("--extra-point", action="append", default=[], metavar="X,Y,#HEX")
    args = parser.parse_args()

    width, height, ops = load_ir(args.ir)
    actual = Image.open(args.png).convert("RGBA")
    checks: list[dict] = []
    failures: list[str] = []

    size_ok = actual.size == (width, height)
    if not size_ok:
        failures.append(f"size {actual.size} != expected {(width, height)}")

    expected = expected_image(width, height, ops)
    diff = ImageChops.difference(actual, expected)
    extrema = diff.getextrema()
    maxdiff = max(band[1] for band in extrema)
    pixels_over = 0
    over_points: list[tuple[int, int, tuple]] = []
    raw = diff.tobytes()
    for offset in range(0, len(raw), 4):
        if max(raw[offset : offset + 4]) > TOLERANCE:
            pixels_over += 1
            if len(over_points) < 10:
                index = offset // 4
                over_points.append(
                    (index % width, index // width, actual.getpixel((index % width, index // width)))
                )

    if maxdiff > TOLERANCE:
        failures.append(f"maxdiff {maxdiff} > {TOLERANCE}")
    if pixels_over:
        failures.append(f"{pixels_over} pixel(s) over tolerance")

    def check_point(x: int, y: int, hex_value: str, label: str) -> None:
        expected_rgb = parse_color(hex_value)
        got = actual.getpixel((x, y))[:3]
        delta = max(abs(got[i] - expected_rgb[i]) for i in range(3))
        ok = delta <= TOLERANCE
        checks.append(
            {
                "label": label,
                "point": [x, y],
                "expected": "#%02x%02x%02x" % expected_rgb,
                "actual": "#%02x%02x%02x" % got,
                "maxdiff": delta,
                "pass": ok,
            }
        )
        if not ok:
            failures.append(f"{label} at ({x},{y}) is {got}, expected {expected_rgb}")

    for x, y, hex_value, label in CONTRACT_POINTS:
        check_point(x, y, hex_value, label)

    extra_checks = []
    for raw in args.extra_point:
        x, y, expected_rgb = parse_point(raw)
        got = actual.getpixel((x, y))[:3]
        delta = max(abs(got[i] - expected_rgb[i]) for i in range(3))
        ok = delta <= TOLERANCE
        extra_checks.append(
            {
                "point": [x, y],
                "expected": "#%02x%02x%02x" % expected_rgb,
                "actual": "#%02x%02x%02x" % got,
                "maxdiff": delta,
                "pass": ok,
            }
        )
        if not ok:
            failures.append(f"extra point ({x},{y}) is {got}, expected {expected_rgb}")

    passed = not failures
    report = {
        "png": str(args.png),
        "ir": str(args.ir),
        "size": list(actual.size),
        "expected_size": [width, height],
        "size_ok": size_ok,
        "ops": len(ops),
        "tolerance": TOLERANCE,
        "maxdiff": maxdiff,
        "pixels_over_tolerance": pixels_over,
        "sample_over_tolerance": over_points,
        "contract_points": checks,
        "extra_points": extra_checks,
        "pass": passed,
        "failures": failures,
    }
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    print(f"verify: {args.png} vs {args.ir}")
    print(f"  size: {actual.size[0]}x{actual.size[1]} (expected {width}x{height})")
    print(f"  ops: {len(ops)}  maxdiff: {maxdiff}  pixels over tol({TOLERANCE}): {pixels_over}")
    for item in checks:
        print(
            f"  {item['label']:<16} ({item['point'][0]},{item['point'][1]}) "
            f"expected {item['expected']} got {item['actual']}  "
            f"{'PASS' if item['pass'] else 'FAIL'}"
        )
    for item in extra_checks:
        print(
            f"  extra            ({item['point'][0]},{item['point'][1]}) "
            f"expected {item['expected']} got {item['actual']}  "
            f"{'PASS' if item['pass'] else 'FAIL'}"
        )
    print(f"  RESULT: {'PASS' if passed else 'FAIL'}")
    for failure in failures:
        print(f"  - {failure}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
