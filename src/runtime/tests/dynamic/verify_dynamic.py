#!/usr/bin/env python3
"""Verify Dynamic UI API v0: core events, runtime stages and rendered pixels.

Reads:
  out/core_events.jsonl  - dynamic_core_test.exe (headless core)
  out/stage_report.json  - runtime_dynamic.exe --dynamic (core + wgpu)
  out/stage_NN.png       - one offscreen frame per stage

Writes out/dynamic_verify_report.json and exits 0 on PASS.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image, ImageChops

CHECKS: list[dict] = []
BACKGROUND = "#0b0e14"
CARD = "#1a1f2a"
THUMB_A = "#e74c3c"
THUMB_B = "#3498db"


def check(group: str, name: str, ok: bool, detail: str) -> None:
    CHECKS.append({"group": group, "name": name, "ok": bool(ok), "detail": detail})
    print(f"[{'ok  ' if ok else 'FAIL'}] {group}: {name} - {detail}")


def close(a: float, b: float, eps: float = 0.51) -> bool:
    return abs(float(a) - float(b)) <= eps


def px(img: Image.Image, x: int, y: int) -> str:
    r, g, b = img.getpixel((x, y))
    return f"#{r:02x}{g:02x}{b:02x}"


def diff_bbox(a: Image.Image, b: Image.Image) -> tuple[int, int, int, int] | None:
    if a.size != b.size:
        return None
    return ImageChops.difference(a.convert("RGB"), b.convert("RGB")).getbbox()


def diff_count(a: Image.Image, b: Image.Image) -> int:
    d = ImageChops.difference(a.convert("RGB"), b.convert("RGB"))
    w, h = d.size
    data = d.load()
    count = 0
    for y in range(h):
        for x in range(w):
            if data[x, y] != (0, 0, 0):
                count += 1
    return count


def node_xy(node: dict) -> tuple[float, float, float, float]:
    return node["x"], node["y"], node["w"], node["h"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", default=str(Path(__file__).resolve().parent / "out"))
    args = parser.parse_args()
    root = Path(args.dir).resolve()
    print(f"verify dynamic: {root}")

    core_path = root / "core_events.jsonl"
    stage_path = root / "stage_report.json"
    if not core_path.is_file() or not stage_path.is_file():
        print("FAIL: missing core_events.jsonl or stage_report.json")
        return 1

    core = {}
    for line in core_path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            event = json.loads(line)
            core[event["stage"]] = event
    for required in ("initial", "hidden", "shown", "cloned", "imaged", "texted",
                     "removed", "appended", "cleared"):
        if required not in core:
            print(f"FAIL: core event '{required}' is missing")
            return 1

    initial = core["initial"]
    hidden = core["hidden"]
    shown = core["shown"]
    cloned = core["cloned"]
    imaged = core["imaged"]
    texted = core["texted"]
    removed = core["removed"]
    appended = core["appended"]
    cleared = core["cleared"]

    check("core", "compiled live nodes", initial["live"] == 17, f"{initial['live']} nodes")
    ix, iy, iw, ih = node_xy(initial["card0"])
    jx, jy, jw, jh = node_xy(initial["card1"])
    check("core", "initial card geometry", close(ix, 10) and close(iy, 50) and
          close(iw, 120) and close(ih, 120), f"card0=({ix},{iy},{iw},{ih})")
    check("core", "initial gap", close(jx, ix + iw + 10), f"card1.x={jx}")
    check("core", "initial hit", initial["hit0"] == initial["card0"]["id"] and
          initial["hit1"] == initial["card1"]["id"], "both cards are action targets")
    check("core", "initial paint", initial["vertices"] == 42 and
          initial["glyphs"] == 126 and initial["image_vertices"] == 12,
          "42 solid, 126 glyph, 12 image vertices")

    hx, hy, hw, hh = node_xy(hidden["card0"])
    check("core", "hidden has empty box", hidden["card0"]["visible"] == 0 and
          close(hw, 0) and close(hh, 0), f"visible=0 box=({hw},{hh})")
    check("core", "hidden reflows sibling", close(hidden["card1"]["x"], ix),
          f"card1.x={hidden['card1']['x']} == card0.x={ix}")
    check("core", "hidden never hits", hidden["hit0"] == hidden["card1"]["id"],
          f"hit at old card0 centre -> {hidden['hit0']}")
    check("core", "hidden not painted", hidden["image_vertices"] == initial["image_vertices"] - 6 and
          hidden["vertices"] < initial["vertices"], "one image quad and card solids removed")

    check("core", "show restores geometry", close(shown["card0"]["x"], ix) and
          close(shown["card1"]["x"], jx), "both cards return to their slots")
    check("core", "show restores paint", shown["vertices"] == initial["vertices"] and
          shown["image_vertices"] == initial["image_vertices"], "display list restored")

    cx, cy, cw, ch = node_xy(cloned["clone"])
    check("core", "clone live count", cloned["live"] == 21, f"{cloned['live']} nodes")
    check("core", "clone geometry", close(cx, jx + jw + 10) and close(cy, iy) and
          close(cw, 120), f"clone=({cx},{cy},{cw},{ch})")
    check("core", "clone paint", cloned["image_vertices"] == 18 and
          cloned["glyphs"] == 156, "one image + one text run added")
    check("core", "clone internals", cloned["clone_image"] is not None,
          "image source copied")

    check("core", "set_image src", imaged["src"] == "images/b.png" and
          imaged["found"] == 1, "new source is in the display list")
    check("core", "set_image keeps quad", imaged["image_vertices"] == 18,
          "replacement does not add geometry")

    check("core", "set_text shaped", texted["text"] == "\u03a9" and
          texted["unicode"] == 1, "Omega accepted and shaped at runtime")
    check("core", "set_text glyphs", texted["glyphs"] == 132,
          "Omega renders a single glyph quad")

    check("core", "remove live count", removed["live"] == 17, f"{removed['live']} nodes")
    check("core", "remove restores layout", close(removed["card0"]["x"], ix) and
          close(removed["card1"]["x"], jx), "siblings return")
    check("core", "remove restores paint", removed["vertices"] == initial["vertices"],
          f"{removed['vertices']} vertices")

    check("core", "append moves node", appended["moved"] == 1 and
          close(appended["clone"]["x"], cx), "re-parented as the last card")
    check("core", "append rejects cycle", appended["cycle_rejected"] == 1,
          "ancestor under descendant refused")

    check("core", "clear_children", cleared["live"] == 9 and
          cleared["first_child"] == -1, f"live={cleared['live']} first_child=-1")
    check("core", "clear removes images", cleared["image_vertices"] == 0,
          "no image quads remain")

    # ------------------------------------------------------------- runtime
    report = json.loads(stage_path.read_text(encoding="utf-8"))
    stages = {item["stage"]: item for item in report["stages"]}
    if len(stages) != 11:
        print(f"FAIL: expected 11 runtime stages, got {len(stages)}")
        return 1
    expected_actions = {
        0: "initial", 1: "hide card0", 2: "show card0", 3: "clone card0",
        4: "set_image clone b", 5: "set_text clone Beta", 6: "remove clone",
        7: "append card1 last", 8: "set_image card0 b", 9: "set_image card0 a",
        10: "clear cards",
    }
    check("runtime", "stage actions",
          all(stages[i]["action"] == name for i, name in expected_actions.items()),
          "11 documented stages")
    check("runtime", "image cache reuse",
          all(item["image_cache"] == 2 for item in stages.values()),
          "only images/a.png and images/b.png are ever decoded")

    s0, s1, s2, s3 = stages[0], stages[1], stages[2], stages[3]
    s4, s5, s6, s7, s8, s9, s10 = (stages[i] for i in range(4, 11))
    check("runtime", "initial geometry", close(s0["card0"]["x"], 10) and
          close(s0["card1"]["x"], 140) and s0["card0"]["hit"] == s0["card0"]["id"],
          "cards at x=10/140")
    check("runtime", "hidden layout", s1["card0"]["visible"] == 0 and
          close(s1["card1"]["x"], 10) and s1["image_vertices"] == 6,
          "card0 hidden, card1 at x=10, one image quad")
    check("runtime", "hidden hit", s1["card0"]["hit"] == -1 and
          s1["card1"]["hit"] == s1["card1"]["id"],
          "hidden card is not hittable")
    check("runtime", "clone tree", s3["live"] == 21 and s3["clone"] is not None and
          close(s3["clone"]["x"], 270), "third card live at x=270")
    check("runtime", "clone image", s3["clone_image"] == "images/a.png" and
          s4["clone_image"] == "images/b.png", "set_image updates the clone")
    check("runtime", "clone text", s5["glyphs"] == 150 and s4["glyphs"] == 156,
          "Beta replaces Alpha glyphs")
    check("runtime", "remove restores", s6["live"] == 17 and
          s6["vertices"] == s2["vertices"] and s6["image_vertices"] == 12,
          "stage 6 has the stage 2 tree/paint")
    check("runtime", "append keeps paint", s7["live"] == 17 and
          s7["vertices"] == s2["vertices"], "same tree and display list")
    check("runtime", "set_image template", s8["card0_image"] == "images/b.png" and
          s9["card0_image"] == "images/a.png", "template image swapped and restored")
    check("runtime", "clear children", s10["live"] == 9 and
          s10["image_vertices"] == 0 and s10["card0"]["alive"] == 0,
          "cards subtree removed")

    # -------------------------------------------------------------- pixels
    images = {}
    for index in range(11):
        path = root / f"stage_{index:02d}.png"
        if not path.is_file():
            print(f"FAIL: missing {path.name}")
            return 1
        images[index] = Image.open(path).convert("RGB")

    check("pixels", "card background", px(images[0], 12, 60) == CARD,
          f"(12,60) {px(images[0], 12, 60)} == {CARD}")
    check("pixels", "thumb a", px(images[0], 70, 88) == THUMB_A,
          f"(70,88) {px(images[0], 70, 88)} == {THUMB_A}")
    check("pixels", "thumb b", px(images[0], 200, 88) == THUMB_B,
          f"(200,88) {px(images[0], 200, 88)} == {THUMB_B}")

    bbox = diff_bbox(images[0], images[1])
    check("pixels", "hide changes card region", bbox is not None and
          bbox[0] <= 20 and bbox[1] == 50 and bbox[2] >= 260 and bbox[3] >= 170,
          f"diff bbox={bbox}")
    check("pixels", "hidden card not painted", px(images[1], 70, 88) == THUMB_B and
          px(images[1], 150, 60) == BACKGROUND,
          "card1 moved into slot; old card1 area is background")

    check("pixels", "show is byte-identical", diff_bbox(images[0], images[2]) is None,
          "stage 0 == stage 2")

    bbox = diff_bbox(images[2], images[3])
    check("pixels", "clone paints third card", bbox is not None and
          bbox[0] >= 269 and bbox[2] <= 391 and px(images[3], 272, 60) == CARD and
          px(images[3], 330, 88) == THUMB_A, f"diff bbox={bbox}")
    check("pixels", "clone image swap", px(images[4], 330, 88) == THUMB_B,
          f"(330,88) {px(images[4], 330, 88)} == {THUMB_B}")

    text_bbox = diff_bbox(images[4], images[5])
    check("pixels", "set_text repaints", text_bbox is not None and
          diff_count(images[4], images[5]) > 20 and
          text_bbox[0] >= 270 and text_bbox[2] <= 390 and
          text_bbox[1] >= 118 and text_bbox[3] <= 170,
          f"diff bbox={text_bbox} inside clone label")

    check("pixels", "remove is byte-identical", diff_bbox(images[2], images[6]) is None,
          "stage 2 == stage 6")
    check("pixels", "append is byte-identical", diff_bbox(images[2], images[7]) is None,
          "stage 2 == stage 7")

    bbox = diff_bbox(images[2], images[8])
    check("pixels", "set_image template repaints", bbox is not None and
          bbox == (18, 58, 122, 118) and px(images[8], 70, 88) == THUMB_B,
          f"diff bbox={bbox} is the template image")
    check("pixels", "set_image restore is identical", diff_bbox(images[2], images[9]) is None,
          "stage 2 == stage 9")

    check("pixels", "clear leaves container", px(images[10], 20, 50) == BACKGROUND and
          px(images[10], 330, 88) != THUMB_A and px(images[10], 330, 88) != THUMB_B,
          "no card pixels remain")
    bbox = diff_bbox(images[2], images[10])
    check("pixels", "clear changes the page", bbox is not None and
          bbox[1] >= 40 and bbox[2] >= 390, f"diff bbox={bbox}")

    failed = [item for item in CHECKS if not item["ok"]]
    result = "PASS" if not failed else "FAIL"
    (root / "dynamic_verify_report.json").write_text(
        json.dumps({"result": result, "total": len(CHECKS),
                    "failed": len(failed), "checks": CHECKS}, indent=2) + "\n",
        encoding="utf-8")
    print(f"\ndynamic verify: {len(CHECKS) - len(failed)}/{len(CHECKS)} checks, {result}")
    return 0 if result == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
