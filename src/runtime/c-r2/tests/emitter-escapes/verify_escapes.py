#!/usr/bin/env python3
"""Verify the non-ASCII C string escaping of emit_ui.py.

The emitter must not produce `\\xNN` escapes that can absorb the following hex
digit (`\\xd72160`). It now writes UTF-8 as 3-digit octal escapes. This script
checks the generated header text and the bytes printed by a C program that
includes it.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

EXPECTED = [
    "Geometría Órbita tecnología montaña geografía astronomía",
    "3840×2160 · 8×8",
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--header", required=True)
    parser.add_argument("--strings", required=True)
    args = parser.parse_args()
    header = Path(args.header).read_text(encoding="utf-8")
    failures: list[str] = []

    if "\\x" in header:
        failures.append("generated header still contains an ambiguous \\xNN escape")
    for needle in ("\\303\\227", "\\303\\255", "\\303\\223", "\\303\\261"):  # ×, í, Ó, ñ
        if needle not in header:
            failures.append(f"missing UTF-8 octal escape {needle}")

    payload = Path(args.strings).read_bytes().decode("utf-8")
    lines = payload.splitlines()
    if len(lines) < len(EXPECTED) or lines[:len(EXPECTED)] != EXPECTED:
        failures.append(f"C output mismatch: {lines!r} != {EXPECTED!r}")

    if failures:
        for item in failures:
            print(f"FAIL: {item}")
        print("emitter-escapes: FAIL")
        return 1
    print("emitter-escapes: PASS (octal UTF-8 escapes, no \\x ambiguity)")
    print(f"  {EXPECTED[0]}")
    print(f"  {EXPECTED[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
