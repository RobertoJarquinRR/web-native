"""Verificación end-to-end de exp12: estado, píxeles, igualdad de frames e IR->PNG.

Uso: ``.venv\\Scripts\\python.exe verify.py``. Sale con código 1 si algo falla.
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import runtime as rt  # noqa: E402

EXPECTED_STATES = [
    ("btn-b", None, 0), ("btn-b", "btn-b", 0), ("btn-b", None, 1),
    ("btn-b", None, 1), (None, None, 1), (None, None, 1),
    ("btn-b", None, 1), ("btn-c", None, 1), ("btn-c", "btn-c", 1),
    ("btn-c", None, 2), ("btn-a", "btn-a", 2), (None, None, 2),
]
EXPECTED_HITS = ["btn-b", "btn-b", "btn-b", "btn-b", None, None,
                 "btn-b", "btn-c", "btn-c", "btn-c", "btn-a", None]
IDENTICAL = [(3, 4), (5, 6)]
DIFFERENT = [(0, 1), (1, 2), (2, 3), (3, 5), (5, 7), (7, 8),
             (8, 9), (9, 10), (10, 11), (11, 12), (4, 5), (6, 7)]


class Report:
    def __init__(self) -> None:
        self.items: list[dict] = []

    def check(self, name: str, ok: bool, detail: str = "") -> None:
        self.items.append({"check": name, "ok": bool(ok), "detail": detail})
        line = f"[{'PASS' if ok else 'FAIL'}] {name}"
        print(line if ok else f"{line} — {detail}")

    def pixel(self, image: Image.Image, point: tuple[int, int], fill: str,
              name: str) -> None:
        want = rt.rgb255(fill)
        got = image.getpixel(point)[:3]
        self.check(name, got == want, f"({point[0]},{point[1]}) got {hexc(got)} "
                                      f"want {fill}")


def hexc(rgb: tuple[int, int, int]) -> str:
    return "#%02x%02x%02x" % rgb


def render_ir(doc: dict) -> Image.Image:
    width = int(doc["viewport"]["width"])
    height = int(doc["viewport"]["height"])
    image = Image.new("RGB", (width, height), (0, 0, 0))
    draw = ImageDraw.Draw(image)
    for op in doc["ops"]:
        assert op["type"] == "rect", op
        x, y = round(op["x"]), round(op["y"])
        draw.rectangle([x, y, x + round(op["w"]) - 1, y + round(op["h"]) - 1],
                       fill=rt.rgb255(op["fill"]))
    return image


def main() -> int:
    rep = Report()
    scene = rt.load_scene(HERE / "scene.json")
    events = json.loads((HERE / "events.json").read_text(encoding="utf-8"))
    log = json.loads((HERE / "state_log.json").read_text(encoding="utf-8"))
    frames = log["frames"]

    rep.check("state_log: 1 frame inicial + 1 por evento",
              len(frames) == len(events) + 1,
              f"frames={len(frames)} eventos={len(events)}")
    rep.check("state_log: summary.frame_count/event_count",
              log["summary"]["frame_count"] == len(frames)
              and log["summary"]["event_count"] == len(events),
              json.dumps(log["summary"]))

    initial = frames[0]
    rep.check("frame_000 sin evento y estado inicial",
              initial["frame"] == 0 and initial["event"] is None
              and initial["state"] == {"hovered": None, "pressed": None, "clicks": 0},
              json.dumps(initial["state"]))

    for index, (entry, event) in enumerate(zip(frames[1:], events)):
        number = index + 1
        hovered, pressed, clicks = EXPECTED_STATES[index]
        expected_state = {"hovered": hovered, "pressed": pressed, "clicks": clicks}
        event_slice = {key: entry["event"].get(key)
                       for key in ("t_ms", "type", "x", "y", "note")}
        rep.check(f"frame_{number:03d}: evento #{number} en orden",
                  event_slice == event, f"{event_slice} != {event}")
        rep.check(f"frame_{number:03d}: hit={EXPECTED_HITS[index]}",
                  entry["hit"] == EXPECTED_HITS[index], f"got {entry['hit']}")
        rep.check(f"frame_{number:03d}: estado {expected_state}",
                  entry["state"] == expected_state, f"got {entry['state']}")
        rep.check(f"frame_{number:03d}: op_count=9", entry["op_count"] == 9,
                  f"got {entry['op_count']}")

    images: dict[int, Image.Image] = {}
    for entry in frames:
        number = entry["frame"]
        png = HERE / entry["png"]
        rep.check(f"frame_{number:03d}: PNG existe", png.exists(), str(png))
        if not png.exists():
            continue
        image = Image.open(png).convert("RGB")
        images[number] = image
        rep.check(f"frame_{number:03d}: 400x300", image.size == (scene.width, scene.height),
                  str(image.size))
        digest = hashlib.sha256(png.read_bytes()).hexdigest()
        rep.check(f"frame_{number:03d}: sha256 del log", digest == entry["png_sha256"],
                  "hash distinto")
        ir_path = HERE / entry["display_list"]
        ir = json.loads(ir_path.read_text(encoding="utf-8"))
        expected = render_ir(ir)
        rep.check(f"frame_{number:03d}: IR->PNG exacto",
                  expected.tobytes() == image.tobytes(), "píxeles distintos del IR")

    a = scene.buttons[0].rect
    b = scene.buttons[1].rect
    c = scene.buttons[2].rect
    centers = {
        "btn-a": (round(a.x + a.w / 2), round(a.y + a.h / 2)),
        "btn-b": (round(b.x + b.w / 2), round(b.y + b.h / 2)),
        "btn-c": (round(c.x + c.w / 2), round(c.y + c.h / 2)),
    }
    dots = [round(scene.dots.x + i * (scene.dots.size + scene.dots.gap)
                  + scene.dots.size / 2) for i in range(scene.dots.count)]
    dot_y = round(scene.dots.y + scene.dots.size / 2)
    normal, hover, pressed = scene.buttons[0].fill, scene.buttons[0].hover_fill, \
        scene.buttons[0].pressed_fill
    active, inactive = scene.dots.active_fill, scene.dots.inactive_fill

    rep.pixel(images[0], centers["btn-b"], normal, "frame_000: btn-b normal")
    rep.pixel(images[1], centers["btn-b"], hover, "frame_001: hover en btn-b")
    rep.pixel(images[2], centers["btn-b"], pressed, "frame_002: pressed en btn-b")
    rep.pixel(images[3], (dots[0], dot_y), active, "frame_003: dot 1 activo tras click 1")
    rep.pixel(images[3], (dots[1], dot_y), inactive, "frame_003: dot 2 inactivo")
    rep.pixel(images[3], centers["btn-b"], hover, "frame_003: btn-b sigue hover")
    rep.pixel(images[4], centers["btn-b"], hover, "frame_004: mismo estado visual")
    rep.pixel(images[5], centers["btn-b"], normal, "frame_005: hover limpiado")
    rep.pixel(images[6], centers["btn-b"], normal,
              "frame_006: borde derecho (240,70) fuera")
    rep.pixel(images[7], centers["btn-b"], hover,
              "frame_007: borde izq./sup. (140,40) dentro")
    rep.pixel(images[8], centers["btn-c"], hover, "frame_008: hover en btn-c")
    rep.pixel(images[8], centers["btn-b"], normal, "frame_008: btn-b normal")
    rep.pixel(images[9], centers["btn-c"], pressed, "frame_009: pressed en btn-c")
    rep.pixel(images[10], (dots[0], dot_y), active, "frame_010: dot 1 activo")
    rep.pixel(images[10], (dots[1], dot_y), active, "frame_010: dot 2 activo tras click 2")
    rep.pixel(images[10], (dots[2], dot_y), inactive, "frame_010: dot 3 inactivo")
    rep.pixel(images[11], centers["btn-a"], pressed, "frame_011: pressed en btn-a")
    rep.pixel(images[12], centers["btn-a"], normal, "frame_012: drag-out suelta btn-a")
    rep.pixel(images[12], (dots[1], dot_y), active, "frame_012: dot 2 sigue activo")
    rep.pixel(images[12], (dots[2], dot_y), inactive, "frame_012: dot 3 no activado")
    for number, image in images.items():
        rep.pixel(image, (200, 150), scene.background,
                  f"frame_{number:03d}: fondo en espacio vacío")

    for left, right in IDENTICAL:
        rep.check(f"frames {left:03d} y {right:03d} idénticos",
                  images[left].tobytes() == images[right].tobytes(),
                  "deberían ser iguales")
    for left, right in DIFFERENT:
        rep.check(f"frames {left:03d} y {right:03d} distintos",
                  images[left].tobytes() != images[right].tobytes(),
                  "deberían diferir")

    passed = sum(1 for item in rep.items if item["ok"])
    failed = len(rep.items) - passed
    report = {"ok": failed == 0, "passed": passed, "failed": failed,
              "checks": rep.items}
    (HERE / "verify_report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False),
                                             encoding="utf-8")
    print(f"\n{passed} checks passed, {failed} failed -> "
          f"{'PASS' if failed == 0 else 'FAIL'}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
