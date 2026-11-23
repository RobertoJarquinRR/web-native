"""Acceptance checks for the canonical R2 runtime (`src/runtime/c-r2`).

Build-time only. Reads layout_report.json + the PNGs produced by
runtime_r2.exe and asserts:

  1. layout: sidebar 30%, content 70%, header = content inner width,
     cards 1 row at 960 and 2 rows at 560/720,
  2. pixels: sidebar/card/accent/pill/panel/line colors,
  3. states: normal/hover/press/selected differ exactly inside the
     interactive node's rect and use the expected colors,
  4. timings are present.

All artifacts live in the build directory (default: ./build next to this
file). Exit code 0 = PASS. Writes verify_report.json there.

Usage:
  .venv\\Scripts\\python.exe verify.py
  .venv\\Scripts\\python.exe verify.py --dir build
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image

DEFAULT_DIR = Path(__file__).resolve().parent / "build"
ROOT = DEFAULT_DIR
COLORS = {
    "sidebar": "#121824",
    "content": "#0b0e14",
    "card": "#1a1f2a",
    "pill": "#2a3140",
    "panel": "#10141b",
    "line": "#2a3140",
    "red": "#e74c3c",
    "green": "#2ecc71",
    "blue": "#3498db",
    "hover": "#2a3140",
    "press": "#3a4a5c",
    "selected": "#3498db",
}
CHECKS: list[dict] = []


def check(group: str, name: str, ok: bool, detail: str) -> None:
    CHECKS.append({"group": group, "name": name, "ok": bool(ok), "detail": detail})
    status = "ok  " if ok else "FAIL"
    print(f"[{status}] {group}: {name} - {detail}")


def close(a: float, b: float, eps: float = 0.51) -> bool:
    return abs(float(a) - float(b)) <= eps


def node(report_size: dict, suffix: str) -> dict:
    for item in report_size["nodes"]:
        if item["name"].endswith(suffix):
            return item
    raise KeyError(f"node ending with {suffix!r} not found")


def px(img: Image.Image, x: float, y: float) -> str:
    r, g, b = img.convert("RGB").getpixel((int(round(x)), int(round(y))))
    return f"#{r:02x}{g:02x}{b:02x}"


def check_pixel(group: str, img: Image.Image, x: float, y: float,
                expected: str, label: str) -> None:
    got = px(img, x, y)
    check(group, label, got == expected, f"({x:.0f},{y:.0f}) {got} == {expected}")


def diff_stats(a: Image.Image, b: Image.Image) -> tuple[int, tuple[int, int, int, int] | None]:
    pa, pb = a.convert("RGBA"), b.convert("RGBA")
    if pa.size != pb.size:
        return -1, None
    w, h = pa.size
    da, db = pa.load(), pb.load()
    count = 0
    x0 = y0 = 10 ** 9
    x1 = y1 = -1
    for y in range(h):
        for x in range(w):
            if da[x, y] != db[x, y]:
                count += 1
                x0 = min(x0, x)
                y0 = min(y0, y)
                x1 = max(x1, x)
                y1 = max(y1, y)
    if count == 0:
        return 0, None
    return count, (x0, y0, x1, y1)


def main() -> int:
    global ROOT
    parser = argparse.ArgumentParser(description="R2 acceptance checks")
    parser.add_argument("--dir", default=str(DEFAULT_DIR),
                        help="directory holding layout_report.json, PNGs and states/")
    args = parser.parse_args()
    ROOT = Path(args.dir).resolve()
    print(f"verify: artifacts in {ROOT}")
    report_path = ROOT / "layout_report.json"
    if not report_path.is_file():
        print("FAIL: layout_report.json is missing")
        return 1
    report = json.loads(report_path.read_text(encoding="utf-8"))
    sizes = {item["width"]: item for item in report["sizes"]}
    for required in (960, 720, 560):
        if required not in sizes:
            print(f"FAIL: layout report has no {required}px size")
            return 1
    check("layout", "report node count", report["node_count"] > 10,
          f"{report['node_count']} nodes")

    for width in (960, 720, 560):
        r = sizes[width]
        sb = node(r, "/div.sidebar")
        ct = node(r, "/div.content")
        hd = node(r, "/div.content/div.header")
        pl = node(r, "/div.content/div.header/div.pill")
        cards = node(r, "/div.cards")
        card0 = node(r, "/div.cards/div.card")
        card1 = node(r, "/div.cards/div.card[1]")
        card2 = node(r, "/div.cards/div.card[2]")
        panel = node(r, "/div.panel")
        group = f"layout@{width}"

        check(group, "sidebar width = 30%", close(sb["w"], width * 0.30),
              f"{sb['w']} == {width * 0.30}")
        check(group, "content width = 70%", close(ct["w"], width * 0.70),
              f"{ct['w']} == {width * 0.70}")
        check(group, "content starts at sidebar right", close(ct["x"], sb["x"] + sb["w"]),
              f"{ct['x']} == {sb['x'] + sb['w']}")
        check(group, "header width = content inner width", close(hd["w"], ct["w"] - 48.0),
              f"{hd['w']} == {ct['w'] - 48.0}")
        check(group, "panel width = content inner width", close(panel["w"], ct["w"] - 48.0),
              f"{panel['w']} == {ct['w'] - 48.0}")
        check(group, "pill right = content inner right",
              close(pl["x"] + pl["w"], ct["x"] + ct["w"] - 24.0),
              f"{pl['x'] + pl['w']} == {ct['x'] + ct['w'] - 24.0}")
        check(group, "cards container inner width", close(cards["w"], ct["w"] - 48.0),
              f"{cards['w']} == {ct['w'] - 48.0}")
        check(group, "layout timing present",
              r.get("layout_ms_avg", -1) >= 0.0,
              f"avg={r.get('layout_ms_avg')} ms best={r.get('layout_ms_best')} ms")

        if width == 960:
            check(group, "3 cards on one row",
                  close(card0["y"], card1["y"]) and close(card1["y"], card2["y"]),
                  f"y={card0['y']},{card1['y']},{card2['y']}")
            check(group, "card width = 180 at 960", close(card0["w"], 180.0),
                  f"{card0['w']}")
            check(group, "card x spaced by 180+16",
                  close(card1["x"] - card0["x"], 196.0),
                  f"{card1['x'] - card0['x']}")
        if width == 560:
            check(group, "2 cards on row 1",
                  close(card0["y"], card1["y"]),
                  f"card0.y={card0['y']} card1.y={card1['y']}")
            check(group, "one card on row 2",
                  card2["y"] > card0["y"] + 100.0,
                  f"card2.y={card2['y']} > card0.y={card0['y']}")
            check(group, "row pitch = 120+16",
                  close(card2["y"] - card0["y"], 136.0),
                  f"{card2['y'] - card0['y']}")
            check(group, "row-1 cards shrink to fit",
                  card0["w"] < 180.0 and card1["w"] < 180.0,
                  f"w={card0['w']},{card1['w']},{card2['w']}")
        if width == 720:
            check(group, "wrap also at 720 (2 rows)",
                  card2["y"] > card0["y"] + 100.0,
                  f"card2.y={card2['y']} card0.y={card0['y']}")

    # ------------------------------------------------------------- pixels
    img960 = Image.open(ROOT / "out_960.png")
    r960 = sizes[960]
    group = "pixels@960"
    check("pixels", "image size", img960.size == (960, 640), f"{img960.size}")
    sb = node(r960, "/div.sidebar")
    ct = node(r960, "/div.content")
    card0 = node(r960, "/div.cards/div.card")
    accent0 = node(r960, "/div.cards/div.card/div.accent")
    accent1 = node(r960, "/div.cards/div.card[1]/div.accent[1]")
    accent2 = node(r960, "/div.cards/div.card[2]/div.accent[2]")
    pill = node(r960, "/div.content/div.header/div.pill")
    panel = node(r960, "/div.panel")
    line1 = node(r960, "/div.panel/div.line")

    check_pixel(group, img960, sb["x"] + 8, sb["y"] + 300, COLORS["sidebar"], "sidebar bg")
    check_pixel(group, img960, ct["x"] + 8, ct["y"] + 300, COLORS["content"], "content bg")
    check_pixel(group, img960, card0["x"] + 90, card0["y"] + 60, COLORS["card"], "card bg")
    check_pixel(group, img960, accent0["x"] + 48, accent0["y"] + 3, COLORS["red"], "accent 1 red")
    check_pixel(group, img960, accent1["x"] + 48, accent1["y"] + 3, COLORS["green"], "accent 2 green")
    check_pixel(group, img960, accent2["x"] + 48, accent2["y"] + 3, COLORS["blue"], "accent 3 blue")
    check_pixel(group, img960, pill["x"] + 40, pill["y"] + 6, COLORS["pill"], "pill bg")
    check_pixel(group, img960, panel["x"] + panel["w"] / 2, panel["y"] + panel["h"] / 2,
                COLORS["panel"], "panel bg")
    check_pixel(group, img960, line1["x"] + 20, line1["y"] + 3, COLORS["line"], "panel line")

    # Reflow proof at the pixel level: card 3 sits on row 2 at 560 and the
    # area to its right is content background (not a fourth card / no overflow).
    img560 = Image.open(ROOT / "out_560.png")
    r560 = sizes[560]
    group = "pixels@560"
    check("pixels", "image size", img560.size == (560, 640), f"{img560.size}")
    c0 = node(r560, "/div.cards/div.card")
    c1 = node(r560, "/div.cards/div.card[1]")
    c2 = node(r560, "/div.cards/div.card[2]")
    check_pixel(group, img560, c0["x"] + 80, c0["y"] + 60, COLORS["card"], "card 1 row 1")
    check_pixel(group, img560, c1["x"] + 80, c1["y"] + 60, COLORS["card"], "card 2 row 1")
    check_pixel(group, img560, c2["x"] + 80, c2["y"] + 60, COLORS["card"], "card 3 row 2")
    check_pixel(group, img560, c1["x"] + c1["w"] + 12, c1["y"] + 60, COLORS["content"],
                "no card past row 1")
    check_pixel(group, img560, c2["x"] + c2["w"] + 12, c2["y"] + 60, COLORS["content"],
                "no card past row 2")

    # ------------------------------------------------------------- text
    def bright_pixels(img: Image.Image, rect: dict, threshold: int = 200) -> int:
        left = max(0, int(rect["x"]))
        top = max(0, int(rect["y"]))
        right = min(img.width, int(rect["x"] + rect["w"] + 0.5))
        bottom = min(img.height, int(rect["y"] + rect["h"] + 0.5))
        if right <= left or bottom <= top:
            return 0
        crop = img.convert("L").crop((left, top, right, bottom))
        return sum(1 for value in crop.tobytes() if value >= threshold)

    group = "text@960"
    title_text = node(r960, "/div.title/text[5]")
    brand_text = node(r960, "/div.brand/text")
    nav_text = node(r960, "/div.nav/text[1]")
    title_px = bright_pixels(img960, title_text)
    brand_px = bright_pixels(img960, brand_text)
    nav_px = bright_pixels(img960, nav_text)
    check(group, "title glyph pixels", title_px > 40, f"{title_px} bright px")
    check(group, "brand glyph pixels", brand_px > 20, f"{brand_px} bright px")
    check(group, "nav label glyph pixels", nav_px > 10, f"{nav_px} bright px")
    check(group, "text is not a solid block",
          title_px < title_text["w"] * title_text["h"] * 0.6,
          f"{title_px} < {title_text['w'] * title_text['h'] * 0.6:.0f}")
    # Text must stay inside its line box: no bright pixel outside all text rects.
    text_rects = [item for item in r960["nodes"] if item["kind"] == "text"]
    mask = Image.new("L", img960.size, 0)
    from PIL import ImageDraw
    draw = ImageDraw.Draw(mask)
    for item in text_rects:
        draw.rectangle([int(item["x"]), int(item["y"]),
                        int(item["x"] + item["w"]), int(item["y"] + item["h"])], fill=255)
    luma = img960.convert("L")
    outside = 0
    for y in range(img960.height):
        for x in range(img960.width):
            if luma.getpixel((x, y)) >= 200 and mask.getpixel((x, y)) == 0:
                outside += 1
    check(group, "no bright glyph pixels outside text boxes", outside == 0,
          f"{outside} stray pixels")

    img720 = Image.open(ROOT / "out_720.png")
    r720 = sizes[720]
    title720 = node(r720, "/div.title/text[5]")
    title720_px = bright_pixels(img720, title720)
    check("text@720", "title glyph pixels", title720_px > 40,
          f"{title720_px} bright px")
    img560t = Image.open(ROOT / "out_560.png")
    title560 = node(r560, "/div.title/text[5]")
    title560_px = bright_pixels(img560t, title560)
    check("text@560", "title glyph pixels", title560_px > 40,
          f"{title560_px} bright px")
    check("text@560", "title follows reflow",
          not close(title560["x"], title720["x"], 1.0) or
          not close(title560["y"], title720["y"], 1.0),
          f"960=({node(r960, '/div.title/text[5]')['x']},{node(r960, '/div.title/text[5]')['y']}) "
          f"720=({title720['x']},{title720['y']}) 560=({title560['x']},{title560['y']})")

    # --------------------------------------------------------- state frames
    states_json = ROOT / "states" / "state_states.json"
    if not states_json.is_file():
        check("states", "state report exists", False, str(states_json))
    else:
        state = json.loads(states_json.read_text(encoding="utf-8"))
        rect = state["rect"]
        group = "states"
        names = ["normal", "hover", "press", "selected"]
        expected = {"hover": COLORS["hover"], "press": COLORS["press"],
                    "selected": COLORS["selected"]}
        images = {name: Image.open(ROOT / "states" / f"state_{name}.png")
                  for name in names}
        normal = images["normal"]
        cx = rect["x"] + rect["w"] / 2
        cy = rect["y"] + rect["h"] / 2
        check_pixel(group, normal, cx, cy, COLORS["sidebar"], "normal = transparent nav")
        for name in ("hover", "press", "selected"):
            img = images[name]
            check_pixel(group, img, cx, cy, expected[name], f"{name} color")
            count, bbox = diff_stats(normal, img)
            check(group, f"{name} diff pixels inside node",
                  0 < count <= int(rect["w"] * rect["h"]) + 8,
                  f"{count} px, rect area {int(rect['w'] * rect['h'])}")
            inside = (bbox is not None and
                      bbox[0] >= rect["x"] - 1 and bbox[1] >= rect["y"] - 1 and
                      bbox[2] <= rect["x"] + rect["w"] and
                      bbox[3] <= rect["y"] + rect["h"])
            check(group, f"{name} changes only that node", inside,
                  f"bbox={bbox} rect=({rect['x']},{rect['y']},{rect['w']},{rect['h']})")
        check(group, "press differs from hover",
              diff_stats(images["press"], images["hover"])[0] > 0,
              f"{diff_stats(images['press'], images['hover'])[0]} px")
        check(group, "selected differs from press",
              diff_stats(images["selected"], images["press"])[0] > 0,
              f"{diff_stats(images['selected'], images['press'])[0]} px")

    # -------------------------------------------------------------- result
    failed = [item for item in CHECKS if not item["ok"]]
    total = len(CHECKS)
    result = {
        "ok": not failed,
        "total_checks": total,
        "failed_checks": len(failed),
        "checks": CHECKS,
        "layout": {
            str(width): {
                "layout_ms_avg": sizes[width]["layout_ms_avg"],
                "layout_ms_best": sizes[width]["layout_ms_best"],
            }
            for width in (960, 720, 560)
        },
    }
    (ROOT / "verify_report.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"\nverify: {total - len(failed)}/{total} checks passed")
    if failed:
        for item in failed:
            print(f"  FAILED {item['group']}: {item['name']} - {item['detail']}")
        return 1
    print("verify: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
