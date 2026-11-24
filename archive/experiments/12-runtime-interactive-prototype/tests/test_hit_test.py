"""Tests de hit testing y semántica de eventos.

Se ejecutan con pytest (``pytest tests -q``) o directamente:
``.venv\\Scripts\\python.exe tests\\test_hit_test.py``.
"""
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))
import runtime as rt  # noqa: E402

OVERLAP = (
    rt.Region("back", rt.Rect(0, 0, 100, 100)),
    rt.Region("front", rt.Rect(50, 50, 100, 100)),
)


def test_inside():
    assert rt.hit_test(OVERLAP, 10, 10) == "back"
    assert rt.hit_test(OVERLAP, 75, 75) == "front"


def test_outside():
    assert rt.hit_test(OVERLAP, -1, 50) is None
    assert rt.hit_test(OVERLAP, 160, 75) is None
    assert rt.hit_test(OVERLAP, 100.001, 25) is None


def test_exact_boundary():
    rect = rt.Rect(10, 20, 100, 60)  # [10,110) x [20,80)
    assert rect.contains(10, 20)            # esquina superior-izquierda inclusiva
    assert rect.contains(10, 79.999)        # borde inferior aún dentro
    assert rect.contains(109.999, 20)       # borde derecho aún dentro
    assert not rect.contains(110, 20)       # x1 exclusivo
    assert not rect.contains(10, 80)        # y1 exclusivo
    assert not rect.contains(109.999, 80)


def test_overlapping_topmost_wins():
    assert rt.hit_test(OVERLAP, 50, 50) == "front"      # borde compartido: gana el último
    assert rt.hit_test(OVERLAP, 49.999, 75) == "back"
    assert rt.hit_test(tuple(reversed(OVERLAP)), 75, 75) == "back"  # painter's order manda


def test_empty_space():
    assert rt.hit_test(OVERLAP, 300, 300) is None
    scene = rt.load_scene(HERE / "scene.json")
    assert rt.hit_test(scene.regions, 130, 70) is None   # hueco entre btn-a y btn-b
    assert rt.hit_test(scene.regions, 200, 150) is None
    assert rt.hit_test(scene.regions, 25, 225) is None   # los dots no son interactivos


def test_scene_boundaries():
    scene = rt.load_scene(HERE / "scene.json")
    assert rt.hit_test(scene.regions, 70, 70) == "btn-a"
    assert rt.hit_test(scene.regions, 190, 70) == "btn-b"
    assert rt.hit_test(scene.regions, 310, 70) == "btn-c"
    assert rt.hit_test(scene.regions, 140, 40) == "btn-b"     # borde izq./sup. inclusivo
    assert rt.hit_test(scene.regions, 240, 40) is None       # borde der. exclusivo
    assert rt.hit_test(scene.regions, 260, 99.999) == "btn-c"
    assert rt.hit_test(scene.regions, 260, 100) is None      # borde inferior exclusivo
    assert rt.hit_test(scene.regions, 360, 70) is None


def test_pointer_semantics():
    scene = rt.load_scene(HERE / "scene.json")
    state = rt.State()
    state, hit = rt.apply_event(scene, state, {"type": "pointer_move", "x": 190, "y": 70})
    assert (hit, state.hovered, state.pressed, state.clicks) == ("btn-b", "btn-b", None, 0)
    state, _ = rt.apply_event(scene, state, {"type": "pointer_down", "x": 190, "y": 70})
    assert (state.hovered, state.pressed, state.clicks) == ("btn-b", "btn-b", 0)
    state, _ = rt.apply_event(scene, state, {"type": "pointer_move", "x": 200, "y": 150})
    assert (state.hovered, state.pressed) == (None, "btn-b")  # pressed sobrevive al drag
    state, _ = rt.apply_event(scene, state, {"type": "pointer_up", "x": 200, "y": 150})
    assert (state.hovered, state.pressed, state.clicks) == (None, None, 0)  # drag-out no click
    state, _ = rt.apply_event(scene, state, {"type": "pointer_down", "x": 190, "y": 70})
    state, _ = rt.apply_event(scene, state, {"type": "pointer_up", "x": 190, "y": 70})
    assert state.clicks == 1
    state, _ = rt.apply_event(scene, state, {"type": "pointer_up", "x": 190, "y": 70})
    assert state.clicks == 1  # up sin down no incrementa


def test_display_list_overrides():
    scene = rt.load_scene(HERE / "scene.json")
    initial = rt.build_display_list(scene, rt.State())
    assert [op.fill for op in initial.ops[1:4]] == ["#2b2b36"] * 3
    hover = rt.build_display_list(scene, rt.State(hovered="btn-b"))
    assert hover.ops[2].fill == "#3a3a48" and hover.ops[1].fill == "#2b2b36"
    pressed = rt.build_display_list(scene, rt.State(hovered="btn-b", pressed="btn-b"))
    assert pressed.ops[2].fill == "#4a4a5c"
    away = rt.build_display_list(scene, rt.State(hovered="btn-a", pressed="btn-b"))
    assert away.ops[2].fill == "#2b2b36"  # btn-b conserva pressed pero ya no está hovered
    two = rt.build_display_list(scene, rt.State(clicks=2))
    assert [op.fill for op in two.ops[4:9]] == [
        "#3498db", "#3498db", "#2b2b36", "#2b2b36", "#2b2b36"]


if __name__ == "__main__":
    tests = [value for name, value in sorted(globals().items())
             if name.startswith("test_") and callable(value) and value.__module__ == __name__]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"\n{len(tests)} tests passed")
