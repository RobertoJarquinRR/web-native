#!/usr/bin/env python3
"""Compare two display-list IR JSON files for cross-language parity.

Rules (see ../SPEC.md):
- same viewport
- same number of ops, same order
- same op type and fill (case-insensitive, normalized to lowercase)
- numeric geometry equal within tolerance (default 0.01 px)

Exit code 0 = identical, 1 = differences, 2 = invalid input.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

GEOMETRY = ("x", "y", "w", "h")


def load(path: Path) -> dict:
    with path.open("r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def normalize_fill(value: object) -> str:
    text = str(value or "").strip().lower()
    if text.startswith("#") and len(text) == 4:
        text = "#" + "".join(ch * 2 for ch in text[1:])
    return text


def compare(golden: dict, candidate: dict, tolerance: float = 0.01) -> list[str]:
    errors: list[str] = []

    gv = golden.get("viewport") or {}
    cv = candidate.get("viewport") or {}
    for key in ("width", "height"):
        if gv.get(key) != cv.get(key):
            errors.append(f"viewport.{key}: expected {gv.get(key)!r}, got {cv.get(key)!r}")

    gops = golden.get("ops")
    cops = candidate.get("ops")
    if not isinstance(gops, list) or not isinstance(cops, list):
        return errors + ["ops must be arrays"]
    if len(gops) != len(cops):
        errors.append(f"op count: expected {len(gops)}, got {len(cops)}")

    for index, (gop, cop) in enumerate(zip(gops, cops)):
        prefix = f"op[{index}]"
        if gop.get("type") != cop.get("type"):
            errors.append(f"{prefix}.type: expected {gop.get('type')!r}, got {cop.get('type')!r}")
            continue
        if gop.get("type") != "rect":
            errors.append(f"{prefix}: unsupported op type {gop.get('type')!r} (benchmark is rect-only)")
            continue
        for key in GEOMETRY:
            expected = float(gop.get(key, 0.0))
            got = float(cop.get(key, 0.0))
            if abs(expected - got) > tolerance:
                errors.append(f"{prefix}.{key}: expected {expected:g}, got {got:g}")
        gfill = normalize_fill(gop.get("fill"))
        cfill = normalize_fill(cop.get("fill"))
        if gfill != cfill:
            errors.append(f"{prefix}.fill: expected {gfill}, got {cfill}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare IR JSON files")
    parser.add_argument("golden", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--tol", type=float, default=0.01)
    parser.add_argument("--max-report", type=int, default=25)
    args = parser.parse_args()

    try:
        golden = load(args.golden)
        candidate = load(args.candidate)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    errors = compare(golden, candidate, args.tol)
    if not errors:
        print(f"PASS: {args.candidate} == {args.golden}")
        return 0

    print(f"FAIL: {len(errors)} difference(s) in {args.candidate}", file=sys.stderr)
    for line in errors[: args.max_report]:
        print(f"  {line}", file=sys.stderr)
    if len(errors) > args.max_report:
        print(f"  ... {len(errors) - args.max_report} more", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
