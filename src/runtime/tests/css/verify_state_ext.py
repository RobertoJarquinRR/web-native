"""Pixel gate for the extended blob v6 state fixture (400x164).

Checks, on the exact offscreen capture:
  - static `:nth-child(2)` / `:nth-child(odd)` matching;
  - `:focus` vs `:focus-visible` (keyboard bit 0x20);
  - derived `:focus-within` on an ancestor bit 0x40;
  - state font-weight in the dump;
  - `text-overflow: ellipsis` in the optional text_extras section (dump), and a
    real single-line truncation: the text run is one line high and the display
    list emits fewer glyphs than the full unbreakable token.

Usage: verify_state_ext.py --png FILE --report JSON --blob FILE --dump EXE
                           --runtime EXE --backend NAME
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

from PIL import Image

# The fixture's #ellipsis token (58 ASCII letters, no spaces, no ligature pairs)
# plus "Inside" and "Weight" in the other rows. Without truncation the display
# list carries every glyph of the token.
ELLIPSIS_TOKEN = "Llanfairpwllgwyngyllgogerychwyrndrobwllllantysiliogogogoch"
OTHER_GLYPHS = len("Inside") + len("Weight")

CHECKS: list[dict] = []


def check(group: str, name: str, ok: bool, detail: str) -> None:
    CHECKS.append({"group": group, "name": name, "ok": bool(ok), "detail": detail})
    print(f"[{'ok  ' if ok else 'FAIL'}] {group}: {name} - {detail}")


def node(report: dict, element_id: str) -> dict:
    for item in report["nodes"]:
        leaf = item["name"].rsplit("/", 1)[-1]
        if f"#{element_id}." in leaf or leaf.endswith(f"#{element_id}"):
            return item
    raise KeyError(f"node #{element_id} not found in the report")


def text_run(report: dict, element_id: str) -> dict:
    owner = node(report, element_id)["name"]
    for item in report["nodes"]:
        if item["name"].startswith(f"{owner}/") and item["kind"] == "text":
            return item
    raise KeyError(f"no text run under #{element_id}")





def px(img: Image.Image, x: float, y: float) -> str:
    r, g, b = img.convert("RGB").getpixel((int(round(x)), int(round(y))))
    return f"#{r:02x}{g:02x}{b:02x}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--png", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--blob", required=True)
    parser.add_argument("--dump", required=True)
    parser.add_argument("--runtime", required=True)
    parser.add_argument("--backend", required=True)
    args = parser.parse_args()

    backend = args.backend
    report = json.loads(Path(args.report).read_text(encoding="utf-8"))
    size = report["sizes"][0]
    image = Image.open(args.png)

    check("capture", "png size", image.size == (400, 164), f"{image.size}")

    dump = subprocess.run([args.dump, args.blob], capture_output=True, text=True)
    text = dump.stdout + dump.stderr
    check("dump", "format wnui/7", "format=wnui/7" in text, text.splitlines()[0]
          if text else "no output")
    check("dump", "focus-visible style",
          "focus-visible(bg=#ff00aa55)" in text,
          "missing" if "focus-visible(bg=#ff00aa55)" not in text else "present")
    check("dump", "focus-within style",
          "focus-within(bg=#ff112233)" in text,
          "missing" if "focus-within(bg=#ff112233)" not in text else "present")
    check("dump", "state font-weight",
          "hover(bg=#ff445566,weight=700)" in text,
          "missing" if "hover(bg=#ff445566,weight=700)" not in text else "present")
    check("dump", "text_extras ellipsis", "text-flags=ellipsis" in text,
          "missing" if "text-flags=ellipsis" not in text else "present")

    payload = json.loads(subprocess.run([args.dump, "--json", args.blob],
                                        capture_output=True, text=True).stdout)
    ellipsis = text_run(payload, "ellipsis")
    check("dump", "json text_flags", ellipsis.get("text_flags") == ["ellipsis"],
          f"{ellipsis.get('text_flags')!r}")
    ellipsis_rect = text_run(size, "ellipsis")
    weight = node(payload, "weight")
    hover_styles = [item for item in weight["state_styles"]
                    if item["state"] == "hover"]
    check("dump", "json state weight",
          bool(hover_styles) and hover_styles[0].get("weight") == 700,
          f"{hover_styles!r}")

    # Real truncation, not just the blob flag: the run must be exactly one line
    # and the display list must drop glyphs of the unbreakable token. Rendering
    # to a scratch PNG gives us the runtime's glyph_vertices counter.
    scratch = Path(args.png).with_name(Path(args.png).stem + "_glyphcount.png")
    environment = dict(os.environ)
    environment["WN_RENDERER"] = backend
    rendered = subprocess.run(
        [args.runtime, "--document", args.blob, "--render", "400", "164",
         str(scratch)],
        capture_output=True, text=True, env=environment,
    )
    match = re.search(r"glyph_vertices=(\d+)", rendered.stdout)
    one_line_height = ellipsis_rect.get("h", 999.0) <= 24.0
    if match is not None:
        glyph_vertices = int(match.group(1))
        # Full line = other text + every token letter, six vertices per glyph;
        # the ellipsis adds one more glyph but still stays below the bound.
        full_vertices = (OTHER_GLYPHS + len(ELLIPSIS_TOKEN)) * 6
        truncated = glyph_vertices < full_vertices
        detail = (f"h={ellipsis_rect.get('h')} "
                  f"glyph_vertices={glyph_vertices} < full {full_vertices}")
    else:
        truncated = False
        detail = (f"h={ellipsis_rect.get('h')} no glyph_vertices in runtime "
                  f"output (rc={rendered.returncode})")
    check("ellipsis", "truncates the single line",
          one_line_height and truncated, detail)

    EXPECTED = {
        "n1": "#00ff00",
        "n2": "#ff0000",
        "n3": "#00ff00",
        "n4": "#00ff00",
        "focus-plain": "#3366cc",
        "focus-visible": "#00aa55",
        "within": "#112233",
        "weight": "#445566",
        "ellipsis": "#1a1a1a",
    }
    for element_id, background in EXPECTED.items():
        rect = node(size, element_id)
        if element_id in ("within", "weight", "ellipsis"):
            # Text boxes: sample below the line box and away from the glyphs.
            cx = rect["x"] + rect["w"] - 4
            cy = rect["y"] + rect["h"] - 2
        else:
            cx = rect["x"] + rect["w"] / 2
            cy = rect["y"] + rect["h"] / 2
        got = px(image, cx, cy)
        check(backend, f"{element_id} background", got == background,
              f"({cx:.0f},{cy:.0f}) {got} == {background}")

    failed = [item for item in CHECKS if not item["ok"]]
    print(f"state_ext verify ({backend}): {len(CHECKS) - len(failed)}/{len(CHECKS)} passed")
    for item in failed:
        print(f"  FAILED {item['group']}: {item['name']} - {item['detail']}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
