#!/usr/bin/env python3
"""Append one extra rect to display_list.json for the strong-proof build.

The protocol keeps the canonical three ops untouched and appends a fourth
non-overlapping rect, so the original contract pixels stay valid while a new
pixel proves the JSON drives GPU geometry end to end.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

PROOF_RECT = {"type": "rect", "x": 260, "y": 180, "w": 80, "h": 60, "fill": "#2ecc71"}


def main() -> int:
    parser = argparse.ArgumentParser(description="Append the proof rect to an IR file")
    parser.add_argument("--ir", default="display_list.json", type=Path)
    args = parser.parse_args()

    doc = json.loads(args.ir.read_text(encoding="utf-8-sig"))
    ops = doc.get("ops")
    if not isinstance(ops, list) or not ops:
        print("error: IR has no ops", file=__import__("sys").stderr)
        return 1
    ops.append(dict(PROOF_RECT))
    args.ir.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"proof_add_rect: appended {PROOF_RECT} to {args.ir} -> {len(ops)} ops")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
