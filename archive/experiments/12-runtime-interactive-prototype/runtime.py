"""exp12 — runtime interactivo mínimo, headless: escena -> hit test -> estado ->
display list (IR) -> render CPU.

Sin navegador, WebView, Skia ni Electron. La entrada se simula con ``events.json``;
cada evento produce un frame PNG y una entrada en ``state_log.json``. El IR sigue
el contrato de ``spec/CONTRACT.md`` y exp05.
"""
from __future__ import annotations

import hashlib
import json
import statistics
import time
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
FRAME_DIR = HERE / "frames"


# --------------------------------------------------------------------------- IR
@dataclass(frozen=True)
class RectOp:
    x: float; y: float; w: float; h: float; fill: str

    def to_dict(self) -> dict:
        return {"type": "rect", "x": round(self.x), "y": round(self.y),
                "w": round(self.w), "h": round(self.h), "fill": self.fill.lower()}


@dataclass(frozen=True)
class DisplayList:
    width: int; height: int; ops: tuple[RectOp, ...]

    def to_dict(self) -> dict:
        return {"viewport": {"width": self.width, "height": self.height},
                "ops": [op.to_dict() for op in self.ops]}


# ------------------------------------------------------------------ hit testing
@dataclass(frozen=True)
class Rect:
    x: float; y: float; w: float; h: float

    def contains(self, px: float, py: float) -> bool:
        """Half-open: incluye borde izq./sup., excluye der./inf.

        Coincide con el rasterizado: el píxel x+w ya pertenece al vecino o al fondo.
        """
        return self.x <= px < self.x + self.w and self.y <= py < self.y + self.h


@dataclass(frozen=True)
class Region:
    id: str; rect: Rect


def hit_test(regions: tuple[Region, ...], px: float, py: float) -> str | None:
    """Id de la región pintada más arriba que contiene el punto (o None)."""
    for region in reversed(regions):  # painter's order: lo último pintado gana
        if region.rect.contains(float(px), float(py)):
            return region.id
    return None


# ---------------------------------------------------------------------- escena
@dataclass(frozen=True)
class Button:
    id: str; rect: Rect; fill: str; hover_fill: str; pressed_fill: str


@dataclass(frozen=True)
class DotStrip:
    count: int; x: float; y: float; size: float; gap: float
    inactive_fill: str; active_fill: str


@dataclass(frozen=True)
class Scene:
    width: int; height: int; background: str
    buttons: tuple[Button, ...]; dots: DotStrip

    @property
    def regions(self) -> tuple[Region, ...]:
        return tuple(Region(button.id, button.rect) for button in self.buttons)


def load_scene(path: str | Path) -> Scene:
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    viewport, dots = data["viewport"], data["dots"]
    buttons = tuple(
        Button(item["id"], Rect(*(float(item[key]) for key in ("x", "y", "w", "h"))),
               item["fill"], item["hover_fill"], item["pressed_fill"])
        for item in data["buttons"])
    return Scene(int(viewport["width"]), int(viewport["height"]), data["background"], buttons,
                 DotStrip(int(dots["count"]), float(dots["x"]), float(dots["y"]),
                          float(dots["size"]), float(dots["gap"]),
                          dots["inactive_fill"], dots["active_fill"]))


# ---------------------------------------------------------------------- estado
@dataclass(frozen=True)
class State:
    hovered: str | None = None; pressed: str | None = None; clicks: int = 0

    def to_dict(self) -> dict:
        return {"hovered": self.hovered, "pressed": self.pressed, "clicks": self.clicks}


def apply_event(scene: Scene, state: State, event: dict) -> tuple[State, str | None]:
    """Aplica un evento inmutablemente y devuelve (nuevo_estado, id_golpeado)."""
    kind = event["type"]
    hit = hit_test(scene.regions, float(event["x"]), float(event["y"]))
    if kind == "pointer_move":
        return State(hit, state.pressed, state.clicks), hit
    if kind == "pointer_down":
        return State(hit, hit, state.clicks), hit
    if kind == "pointer_up":
        click = state.pressed is not None and hit == state.pressed
        return State(hit, None, state.clicks + int(click)), hit
    raise ValueError(f"evento no soportado: {kind!r}")


def button_fill(button: Button, state: State) -> str:
    # :active solo mientras el puntero sigue encima, como un botón real.
    if state.pressed == button.id and state.hovered == button.id:
        return button.pressed_fill
    return button.hover_fill if state.hovered == button.id else button.fill


def build_display_list(scene: Scene, state: State) -> DisplayList:
    """Reconstruye la escena completa aplicando los overrides de estado."""
    ops = [RectOp(0, 0, scene.width, scene.height, scene.background)]
    ops += [RectOp(b.rect.x, b.rect.y, b.rect.w, b.rect.h, button_fill(b, state))
            for b in scene.buttons]
    dots = scene.dots
    ops += [RectOp(dots.x + i * (dots.size + dots.gap), dots.y, dots.size, dots.size,
                   dots.active_fill if i < state.clicks else dots.inactive_fill)
            for i in range(dots.count)]
    return DisplayList(scene.width, scene.height, tuple(ops))


# -------------------------------------------------------------------- renderer
def rgb255(fill: str) -> tuple[int, int, int]:
    value = fill.lstrip("#")
    value = "".join(c * 2 for c in value) if len(value) == 3 else value
    if len(value) != 6:
        raise ValueError(f"color inválido: {fill!r}")
    return tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))


def render_cpu(display_list: DisplayList) -> Image.Image:
    """Rasterizador de referencia: rects opacos en painter's order."""
    image = Image.new("RGB", (display_list.width, display_list.height),
                      rgb255(display_list.ops[0].fill))
    draw = ImageDraw.Draw(image)
    for op in display_list.ops[1:]:
        x, y = round(op.x), round(op.y)
        draw.rectangle([x, y, x + round(op.w) - 1, y + round(op.h) - 1],
                       fill=rgb255(op.fill))
    return image


# ---------------------------------------------------------------- bucle runtime
def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stats(values: list[float]) -> dict:
    ordered = sorted(values)
    return {"min": round(ordered[0], 3), "median": round(statistics.median(ordered), 3),
            "mean": round(statistics.fmean(ordered), 3),
            "p95": round(ordered[-(-len(ordered) * 95 // 100) - 1], 3),
            "max": round(ordered[-1], 3)}


def run(scene: Scene, events: list[dict], out_dir: Path = FRAME_DIR) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    state = State()
    entries: list[dict] = []
    stages: dict[str, list[float]] = {key: [] for key in
                                      ("hit_test", "build_display_list", "render_cpu",
                                       "save_png", "total")}

    def emit(number: int, event: dict | None, hit: str | None,
             hit_ms: float, frame_start: float) -> None:
        t_build = time.perf_counter()
        display_list = build_display_list(scene, state)
        t_render = time.perf_counter()
        image = render_cpu(display_list)
        t_save = time.perf_counter()
        png = out_dir / f"frame_{number:03d}.png"
        image.save(png)
        ir_path = out_dir / f"display_list_{number:03d}.json"
        ir_path.write_text(json.dumps(display_list.to_dict(), indent=2), encoding="utf-8")
        t_end = time.perf_counter()
        timing = {
            "hit_test": round(hit_ms, 3),
            "build_display_list": round((t_render - t_build) * 1000, 3),
            "render_cpu": round((t_save - t_render) * 1000, 3),
            "save_png": round((t_end - t_save) * 1000, 3),
            "total": round((t_end - frame_start) * 1000, 3),
        }
        for key, value in timing.items():
            stages[key].append(value)
        entries.append({
            "frame": number, "png": png.relative_to(HERE).as_posix(),
            "display_list": ir_path.relative_to(HERE).as_posix(),
            "event": event, "hit": hit, "state": state.to_dict(),
            "op_count": len(display_list.ops), "png_sha256": sha256(png),
            "timing_ms": timing,
        })
        print(f"[frame {number:03d}] {event['type'] if event else 'initial':<12} "
              f"hit={str(hit):<5} hover={str(state.hovered):<5} "
              f"pressed={str(state.pressed):<5} clicks={state.clicks} "
              f"{timing['total']:6.2f} ms")

    wall_start = time.perf_counter()
    emit(0, None, None, 0.0, time.perf_counter())
    for number, event in enumerate(events, 1):
        frame_start = time.perf_counter()
        t_hit = time.perf_counter()
        state, hit = apply_event(scene, state, event)
        emit(number, event, hit, (time.perf_counter() - t_hit) * 1000, frame_start)
    summary = {
        "frame_count": len(entries), "event_count": len(events),
        "wall_ms": round((time.perf_counter() - wall_start) * 1000, 3),
        "timing_ms": {key: stats(values) for key, values in stages.items()},
        "steady_state_timing_ms": {key: stats(values[1:]) for key, values in stages.items()},
    }
    log = {"scene": "scene.json", "events_file": "events.json",
           "viewport": {"width": scene.width, "height": scene.height},
           "frames": entries, "summary": summary}
    (HERE / "state_log.json").write_text(json.dumps(log, indent=2, ensure_ascii=False),
                                         encoding="utf-8")
    return log


def main() -> int:
    scene = load_scene(HERE / "scene.json")
    events = json.loads((HERE / "events.json").read_text(encoding="utf-8"))
    log = run(scene, events)
    summary = log["summary"]
    print(f"\n{summary['frame_count']} frames | {summary['event_count']} eventos | "
          f"wall {summary['wall_ms']:.1f} ms")
    for stage, values in summary["steady_state_timing_ms"].items():
        print(f"  {stage:<20} mediana {values['median']:6.3f} ms | media {values['mean']:6.3f}"
              f" | p95 {values['p95']:6.3f} | max {values['max']:6.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
