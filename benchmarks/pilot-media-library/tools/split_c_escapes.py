#!/usr/bin/env python3
"""Temporary build workaround for ambiguous C hex escapes.

`emit_ui.py` serialises non-ASCII text bytes as `\\xNN`. When the next source
byte is an ASCII hex digit, C reads it as part of the escape:
`"\\xd72160"` -> escape `\\xd72160` (out of range). The C fix is to split the
literal: `"\\xd7""2160"`.

This script rewrites a generated `ui_data.h` in place (with a `.orig.h`
backup). It is a pilot-local workaround; the real fix belongs in the emitter.
Remove this tool once `emit_ui.py` emits unambiguous escapes.

Usage:
    python tools/split_c_escapes.py build/ui_data.h
    python tools/split_c_escapes.py --in build/ui_data.h --out build/ui_data.fixed.h
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

ESCAPE = re.compile(r'\\x([0-9A-Fa-f]{2})(?=[0-9A-Fa-f])')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--in", dest="source", type=Path, required=True)
    parser.add_argument("--out", dest="target", type=Path, default=None)
    parser.add_argument("--no-backup", action="store_true")
    args = parser.parse_args()

    source = args.source.resolve()
    target = (args.target or source).resolve()
    text = source.read_text(encoding="utf-8")
    fixed, count = ESCAPE.subn(r'\\x\1""', text)
    if target == source and not args.no_backup:
        backup = source.with_suffix(".orig.h")
        backup.write_text(text, encoding="utf-8")
        print(f"backup: {backup}")
    target.write_text(fixed, encoding="utf-8")
    print(f"split {count} ambiguous hex escape(s) -> {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
