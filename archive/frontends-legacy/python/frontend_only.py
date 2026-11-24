#!/usr/bin/env python3
"""Frontend-only Python implementation: HTML+CSS -> display-list IR JSON.

This is the reference implementation for the cross-language parity benchmark.
It uses the same pipeline as `python -m webnative`, but skips rendering so the
measured cost is parse + cascade + layout + IR only.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from webnative.cli import build_all  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description="HTML+CSS -> IR JSON (no render)")
    parser.add_argument("--html", required=True, type=Path)
    parser.add_argument("--width", required=True, type=int)
    parser.add_argument("--height", required=True, type=int)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--timings", type=Path)
    args = parser.parse_args()

    result = build_all(args.html, (args.width, args.height))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(result.display.to_json() + "\n", encoding="utf-8")

    timings = dict(result.timings)
    timings["ops"] = len(result.display.ops)
    if args.timings:
        args.timings.parent.mkdir(parents=True, exist_ok=True)
        args.timings.write_text(json.dumps(timings, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(timings))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
