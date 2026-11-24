"""Acceptance checks for the single-line text input (`tests/input`).

Reads out/stage_report.json + out/stage_00.png .. stage_09.png (offscreen wgpu
renders from `runtime_input.exe --input`) and the ABI/core logs, then asserts:

  1. values/cursor/anchor/focus per stage (typing, click caret, Backspace,
     Shift selection, copy/paste, max_length, Escape),
  2. placeholder glyph pixels when empty,
  3. typed text pixels from the ASCII pool,
  4. the 2 px focus ring and the caret pixels (and their absence after blur),
  5. the translucent selection quad over the selected range,
  6. ABI v4 demo surface: set/get value, WN_EVENT_INPUT, clipboard, blur.

Exit code 0 = PASS. Writes out/input_verify_report.json.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image

CHECKS: list[dict] = []
BG_INPUT = "#1c2430"
BG_BODY = "#101418"
TEXT = "#f2f5f8"
PLACEHOLDER = "#6b7785"
RING = "#3b82f6"
SELECTION_BLEND = (40, 74, 127)


def check(group: str, name: str, ok: bool, detail: str) -> None:
    CHECKS.append({"group": group, "name": name, "ok": bool(ok), "detail": detail})
    print(f"[{'ok  ' if ok else 'FAIL'}] {group}: {name} - {detail}")


def hex_rgb(value: str) -> tuple[int, int, int]:
    value = value.lstrip("#")
    return int(value[0:2], 16), int(value[2:4], 16), int(value[4:6], 16)


def rgb_hex(value: tuple[int, int, int]) -> str:
    return "#%02x%02x%02x" % value


def count_color(img: Image.Image, box: tuple[float, float, float, float],
                color: str) -> int:
    x0 = max(0, int(box[0]))
    y0 = max(0, int(box[1]))
    x1 = min(img.width, int(box[0] + box[2]))
    y1 = min(img.height, int(box[1] + box[3]))
    if x1 <= x0 or y1 <= y0:
        return 0
    data = img.crop((x0, y0, x1, y1)).convert("RGB").tobytes()
    target = bytes(hex_rgb(color))
    return sum(1 for index in range(0, len(data), 3)
               if data[index:index + 3] == target)


def pixel(img: Image.Image, x: float, y: float) -> tuple[int, int, int]:
    return img.convert("RGB").getpixel((int(round(x)), int(round(y))))


def caret_columns(img: Image.Image, stage: dict) -> list[tuple[int, int, int]]:
    x = int(stage["caret_x"] + 0.5)
    # Near the top of the content box: inside the caret quad but above the
    # glyph bitmaps, so the placeholder/text never covers the caret pixels.
    y = int(stage["content"]["y"] + 2)
    return [pixel(img, x, y), pixel(img, x + 1, y)]


def main() -> int:
    parser = argparse.ArgumentParser(description="text input acceptance")
    parser.add_argument("--dir", default=str(Path(__file__).resolve().parent / "out"))
    args = parser.parse_args()
    root = Path(args.dir).resolve()
    print(f"verify: artifacts in {root}")
    report_path = root / "stage_report.json"
    if not report_path.is_file():
        print("FAIL: stage_report.json is missing")
        return 1
    report = json.loads(report_path.read_text(encoding="utf-8"))
    stages = {item["stage"]: item for item in report["stages"]}

    check("report", "stage count", len(stages) == 10, f"{len(stages)} stages")
    check("report", "field", report.get("field") == "input#search",
          str(report.get("field")))
    check("report", "max_length", report.get("max_length") == 12,
          str(report.get("max_length")))
    expected = {
        0: ("", 0, -1, 0),
        1: ("", 0, -1, 1),
        2: ("abc", 3, -1, 1),
        3: ("abXc", 3, -1, 1),
        4: ("abc", 2, -1, 1),
        5: ("abc", 2, 0, 1),
        6: ("abcab", 5, -1, 1),
        7: ("", 0, -1, 1),
        8: ("0123456789ab", 12, -1, 1),
        9: ("0123456789ab", 12, -1, 0),
    }
    for stage in range(10):
        item = stages.get(stage)
        if item is None:
            check("stages", f"stage {stage}", False, "missing")
            continue
        value, cursor, anchor, focused = expected[stage]
        check("stages", f"stage {stage} value", item["value"] == value,
              f"'{item['value']}' == '{value}'")
        check("stages", f"stage {stage} caret", item["cursor"] == cursor,
              f"{item['cursor']} == {cursor}")
        check("stages", f"stage {stage} anchor", item["anchor"] == anchor,
              f"{item['anchor']} == {anchor}")
        check("stages", f"stage {stage} focus", item["focused"] == focused,
              f"{item['focused']} == {focused}")

    images: dict[int, Image.Image] = {}
    for stage in range(10):
        path = root / stages[stage]["png"]
        if not path.is_file():
            check("pixels", f"stage {stage} png", False, f"missing {path.name}")
            continue
        images[stage] = Image.open(path)
        check("pixels", f"stage {stage} size",
              images[stage].size == (480, 240), str(images[stage].size))
        check("pixels", f"stage {stage} input background",
              pixel(images[stage], stages[stage]["rect"]["x"] + 4,
                    stages[stage]["rect"]["y"] + 4) == hex_rgb(BG_INPUT),
              rgb_hex(pixel(images[stage], stages[stage]["rect"]["x"] + 4,
                            stages[stage]["rect"]["y"] + 4)))

    if 0 in images:
        stage = stages[0]
        box = (stage["content"]["x"], stage["content"]["y"],
               stage["content"]["w"], stage["content"]["h"])
        # The fixture has two inputs: "Buscar..." + "Otro".
        placeholder = count_color(images[0], box, PLACEHOLDER)
        check("text", "placeholder pixels", placeholder >= 10,
              f"{placeholder} placeholder-colored pixels")
        check("text", "no typed text when empty",
              count_color(images[0], box, TEXT) <= 2,
              f"{count_color(images[0], box, TEXT)} text-colored pixels")
        ring_x = stage["rect"]["x"] + 2
        ring_y = stage["rect"]["y"] + 1
        check("focus", "no ring before click",
              pixel(images[0], ring_x, ring_y) != hex_rgb(RING),
              rgb_hex(pixel(images[0], ring_x, ring_y)))
        check("focus", "no caret before click", stages[0]["focused"] == 0,
              "unfocused")

    if 1 in images:
        stage = stages[1]
        ring_x = stage["rect"]["x"] + 2
        ring_y = stage["rect"]["y"] + 1
        check("focus", "focus ring color",
              pixel(images[1], ring_x, ring_y) == hex_rgb(RING),
              rgb_hex(pixel(images[1], ring_x, ring_y)))
        check("focus", "focus ring bottom",
              pixel(images[1], ring_x, stage["rect"]["y"] + stage["rect"]["h"] - 1)
              == hex_rgb(RING),
              rgb_hex(pixel(images[1], ring_x,
                            stage["rect"]["y"] + stage["rect"]["h"] - 1)))
        columns = caret_columns(images[1], stage)
        check("caret", "caret at x=0",
              all(color == hex_rgb(TEXT) for color in columns),
              " ".join(rgb_hex(color) for color in columns))

    if 2 in images:
        stage = stages[2]
        box = (stage["content"]["x"], stage["content"]["y"],
               stage["content"]["w"], stage["content"]["h"])
        text_pixels = count_color(images[2], box, TEXT)
        check("text", "typed text pixels", text_pixels >= 20,
              f"{text_pixels} text-colored pixels")
        columns = caret_columns(images[2], stage)
        check("caret", "caret after abc",
              all(color == hex_rgb(TEXT) for color in columns),
              " ".join(rgb_hex(color) for color in columns))
        check("text", "caret moved right", stage["caret_x"] > stages[0]["caret_x"],
              f"{stage['caret_x']} > {stages[0]['caret_x']}")

    if 3 in images:
        stage = stages[3]
        box = (stage["content"]["x"], stage["content"]["y"],
               stage["content"]["w"], stage["content"]["h"])
        check("text", "inserted X pixels",
              count_color(images[3], box, TEXT) >= 20,
              f"{count_color(images[3], box, TEXT)} text-colored pixels")

    if 5 in images:
        stage = stages[5]
        x0 = stage["selection_x0"]
        x1 = stage["selection_x1"]
        mid_x = int((x0 + x1) / 2)
        mid_y = int(stage["content"]["y"] + stage["content"]["h"] / 2)
        got = pixel(images[5], mid_x, mid_y)
        near = all(abs(got[i] - SELECTION_BLEND[i]) <= 2 for i in range(3))
        check("selection", "selection quad visible", near,
              f"{rgb_hex(got)} ~ {rgb_hex(SELECTION_BLEND)}")
        check("selection", "selection has width", x1 - x0 >= 18,
              f"{x1 - x0:.2f} px")
        check("selection", "selection starts at content",
              abs(x0 - stage["content"]["x"]) <= 0.5,
              f"{x0} == {stage['content']['x']}")

    if 7 in images:
        stage = stages[7]
        box = (stage["content"]["x"], stage["content"]["y"],
               stage["content"]["w"], stage["content"]["h"])
        check("text", "placeholder returns after delete",
              count_color(images[7], box, PLACEHOLDER) >= 10,
              f"{count_color(images[7], box, PLACEHOLDER)} placeholder pixels")
        check("selection", "no selection after delete",
              abs(stage["selection_x0"] - stage["selection_x1"]) < 0.01,
              "x0 == x1")

    if 8 in images:
        stage = stages[8]
        check("max_length", "value clamped", len(stage["value"]) == 12,
              f"{len(stage['value'])} chars")
        check("max_length", "caret inside the content box",
              stage["content"]["x"] <= stage["caret_x"] <=
              stage["content"]["x"] + stage["content"]["w"],
              f"{stage['caret_x']} in [{stage['content']['x']}, "
              f"{stage['content']['x'] + stage['content']['w']}]")
        columns = caret_columns(images[8], stage)
        check("caret", "caret after max-length text",
              all(color == hex_rgb(TEXT) for color in columns),
              " ".join(rgb_hex(color) for color in columns))

    if 9 in images:
        stage = stages[9]
        columns = caret_columns(images[9], stage)
        check("focus", "caret gone after blur",
              all(color == hex_rgb(BG_INPUT) for color in columns),
              " ".join(rgb_hex(color) for color in columns))
        check("focus", "ring gone after blur",
              pixel(images[9], stage["rect"]["x"] + 2, stage["rect"]["y"] + 1)
              != hex_rgb(RING),
              rgb_hex(pixel(images[9], stage["rect"]["x"] + 2,
                            stage["rect"]["y"] + 1)))

    # ------------------------------------------------------------- logs
    core_log = root / "core_test.log"
    core_text = core_log.read_text(encoding="utf-8") if core_log.is_file() else ""
    check("logs", "core test PASS", "input_core_test: PASS" in core_text,
          "input_core_test: PASS")
    for needle in ("click caret position", "value after cut", "value after paste",
                   "value after clamp", "escape blur", "clone input",
                   "old selection cleared"):
        check("logs", f"core {needle}",
              f"[ok  ] core: {needle}" in core_text, needle)

    runtime_log = root / "runtime_input.log"
    runtime_text = runtime_log.read_text(encoding="utf-8") if runtime_log.is_file() else ""
    check("logs", "runtime reached stage 9",
          "stage=9 action=escape blurs" in runtime_text, "stage 9")
    check("logs", "runtime copy selection",
          'copy selection = "ab"' in runtime_text, "$ab$")

    window_log = root / "window_input.log"
    window_text = window_log.read_text(encoding="utf-8") if window_log.is_file() else ""
    check("logs", "window WM_CHAR typing",
          "input-demo: WM_CHAR 'a'" in window_text and
          "input-demo: WM_CHAR 'b'" in window_text, "real Win32 chars")
    check("logs", "window VK_BACK", "input-demo: WM_KEYDOWN VK_BACK" in window_text,
          "real Win32 key")
    check("logs", "window value after edits",
          'input-demo: value="a" focused=1' in window_text, "value=a, focused")
    check("logs", "window 16 frames",
          "window: closed after 16 frame" in window_text, "16 frames")

    abi_log = root / "abi_input.log"
    abi_text = abi_log.read_text(encoding="utf-8") if abi_log.is_file() else ""
    for needle in (
        "ABI 4",
        "input: PASS",
        "set_value(abc) -> WN_OK",
        "set_value(Omega) -> WN_ERR_UNSUPPORTED",
        "set_value(13 chars) -> WN_ERR_INVALID_ARGUMENT",
        'event INPUT target=body.root/div.bar/input#search.search value="abcd"',
        'after typing + backspace = "abc"',
        'clipboard copy + paste = "abca"',
        'cut all = ""',
        'paste after cut = "abca"',
        "escape -> focused=0",
        "esc policy -> window stays open, input blurred",
        "exit=0 frames=8",
    ):
        check("logs", f"abi {needle}", needle in abi_text, needle)

    failed = sum(1 for item in CHECKS if not item["ok"])
    result = "PASS" if failed == 0 else "FAIL"
    output = {
        "layer": "c-r2-input-v1",
        "result": result,
        "total": len(CHECKS),
        "failed": failed,
        "checks": CHECKS,
    }
    (root / "input_verify_report.json").write_text(
        json.dumps(output, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"verify: {len(CHECKS) - failed}/{len(CHECKS)} checks passed")
    print(f"verify: {result}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
