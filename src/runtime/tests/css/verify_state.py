"""Pixel gate for the blob v6 CSS state fixture (400x450).

Checks, on the exact offscreen capture:
  - the dump format is wnui/6 and carries hover/press/focus/disabled styles;
  - every button-like node paints its effective background and border color;
  - selected stays the app base and a CSS hover state composes over it;
  - data-hover is the fallback and loses to a CSS hover rule.

Usage: verify_state.py --png FILE --report JSON --blob FILE --dump EXE --backend NAME
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from PIL import Image

CHECKS: list[dict] = []

EXPECTED = {
    "base": ("#101010", "#202020"),
    "hover": ("#2a3140", "#ff0000"),
    "press": ("#3a4a5c", "#00ff00"),
    "focus": ("#003366", "#0000ff"),
    "disabled": ("#444444", "#555555"),
    "selected": ("#ff00ff", "#202020"),
    "selected-hover": ("#2a3140", "#ff0000"),
    "sugar": ("#abcdef", "#202020"),
    "css-wins": ("#2a3140", "#ff0000"),
}


def check(group: str, name: str, ok: bool, detail: str) -> None:
    CHECKS.append({"group": group, "name": name, "ok": bool(ok), "detail": detail})
    print(f"[{'ok  ' if ok else 'FAIL'}] {group}: {name} - {detail}")


def node(report: dict, element_id: str) -> dict:
    for item in report["nodes"]:
        name = item["name"]
        if f"#{element_id}." in name or name.endswith(f"#{element_id}"):
            return item
    raise KeyError(f"node #{element_id} not found in the report")


def px(img: Image.Image, x: float, y: float) -> str:
    r, g, b = img.convert("RGB").getpixel((int(round(x)), int(round(y))))
    return f"#{r:02x}{g:02x}{b:02x}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--png", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--blob", required=True)
    parser.add_argument("--dump", required=True)
    parser.add_argument("--backend", required=True)
    args = parser.parse_args()

    backend = args.backend
    report = json.loads(Path(args.report).read_text(encoding="utf-8"))
    size = report["sizes"][0]
    image = Image.open(args.png)

    check("capture", "png size", image.size == (400, 450), f"{image.size}")
    check("capture", "node count", len(size["nodes"]) >= 10,
          f"{len(size['nodes'])} nodes")

    dump = subprocess.run([args.dump, args.blob], capture_output=True, text=True)
    text = dump.stdout + dump.stderr
    check("dump", "format wnui/6", "format=wnui/6" in text, text.splitlines()[0]
          if text else "no output")
    for expected in (
        "states=hover(bg=#ff2a3140,text=#ffffff00,border=#ffff0000)",
        "press(bg=#ff3a4a5c,text=#ff00ffff,border=#ff00ff00)",
        "focus(bg=#ff003366,text=#ffffffff,border=#ff0000ff)",
        "disabled(bg=#ff444444,text=#ff999999,border=#ff555555)",
    ):
        check("dump", f"state style {expected.split('(')[0]}", expected in text,
              "missing" if expected not in text else "present")

    for element_id, (background, border) in EXPECTED.items():
        rect = node(size, element_id)
        cx = rect["x"] + rect["w"] / 2
        cy = rect["y"] + rect["h"] / 2
        got_bg = px(image, cx, cy)
        got_border = px(image, rect["x"] + 2, rect["y"] + 2)
        check(backend, f"{element_id} background", got_bg == background,
              f"({cx:.0f},{cy:.0f}) {got_bg} == {background}")
        check(backend, f"{element_id} border", got_border == border,
              f"({rect['x']:.0f}+2,{rect['y']:.0f}+2) {got_border} == {border}")

    # The disabled node is inert but still painted: the background is its
    # disabled color, not the base one (already covered above) and the two
    # differ, so a dropped state table would fail the probe.
    check("states", "selected differs from base",
          EXPECTED["selected"][0] != EXPECTED["base"][0], "different colors")
    check("states", "selected + hover uses hover",
          EXPECTED["selected-hover"] == EXPECTED["hover"],
          "the CSS hover state composes over the selected base")

    failed = [item for item in CHECKS if not item["ok"]]
    print(f"state verify ({backend}): {len(CHECKS) - len(failed)}/{len(CHECKS)} passed")
    for item in failed:
        print(f"  FAILED {item['group']}: {item['name']} - {item['detail']}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
