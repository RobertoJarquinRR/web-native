#!/usr/bin/env python3
"""Lightweight IR validator (no external dependencies).

Checks the structure defined in schema.json / docs/ir-schema.md.
Exit 0 = valid, 1 = invalid.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

COLOR_CHARS = set("0123456789abcdefABCDEF")


def valid_color(value: object) -> bool:
    if not isinstance(value, str) or not value.startswith("#"):
        return False
    digits = value[1:]
    return len(digits) in (3, 6, 8) and all(ch in COLOR_CHARS for ch in digits)


def validate(doc: object) -> list[str]:
    errors: list[str] = []
    if not isinstance(doc, dict):
        return ["root must be an object"]

    viewport = doc.get("viewport")
    if not isinstance(viewport, dict):
        errors.append("viewport must be an object")
    else:
        for key in ("width", "height"):
            value = viewport.get(key)
            if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
                errors.append(f"viewport.{key} must be a positive integer")

    ops = doc.get("ops")
    if not isinstance(ops, list):
        errors.append("ops must be an array")
        return errors

    for index, op in enumerate(ops):
        prefix = f"ops[{index}]"
        if not isinstance(op, dict):
            errors.append(f"{prefix} must be an object")
            continue
        kind = op.get("type")
        if kind == "rect":
            for key in ("x", "y", "w", "h"):
                value = op.get(key)
                if not isinstance(value, (int, float)) or isinstance(value, bool):
                    errors.append(f"{prefix}.{key} must be a number")
            if not valid_color(op.get("fill")):
                errors.append(f"{prefix}.fill must be #rgb, #rrggbb or #rrggbbaa")
        elif kind == "text_run":
            for key in ("x", "y"):
                if not isinstance(op.get(key), (int, float)):
                    errors.append(f"{prefix}.{key} must be a number")
            if not valid_color(op.get("color")):
                errors.append(f"{prefix}.color must be a hex color")
            if not isinstance(op.get("glyphs"), list):
                errors.append(f"{prefix}.glyphs must be an array")
        else:
            errors.append(f"{prefix}.type must be 'rect' or 'text_run'")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate a display-list IR file")
    parser.add_argument("path", type=Path)
    args = parser.parse_args()
    try:
        doc = json.loads(args.path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    errors = validate(doc)
    if errors:
        print(f"INVALID: {args.path}", file=sys.stderr)
        for line in errors:
            print(f"  {line}", file=sys.stderr)
        return 1
    print(f"OK: {args.path} ({len(doc['ops'])} ops)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
