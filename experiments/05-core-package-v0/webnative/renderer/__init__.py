"""Contrato de renderer y factoría de backends.

Un renderer cumple `name` + `render(display_list, width=None, height=None) ->
PIL.Image`. Añadir un backend = nuevo módulo que cumpla el protocolo + rama aquí.
"""
from typing import Callable, Protocol

from PIL import Image

from ..ir import DisplayList


class Renderer(Protocol):
    name: str

    def render(self, display_list: DisplayList, width: int | None = None,
               height: int | None = None) -> Image.Image: ...


def rgb01(fill: str) -> tuple[float, float, float]:
    value = fill.lstrip("#")
    if len(value) == 3:
        value = "".join(c * 2 for c in value)
    if len(value) != 6:
        raise ValueError(f"color no válido: {fill!r}")
    return tuple(int(value[i:i + 2], 16) / 255.0 for i in (0, 2, 4))


def rgb255(fill: str) -> tuple[int, int, int]:
    return tuple(round(c * 255) for c in rgb01(fill))


def create_renderer(name: str = "auto",
                    log: Callable[[str], None] | None = None) -> Renderer:
    name = name.lower()
    if name == "cpu":
        from .cpu_renderer import CpuRenderer
        return CpuRenderer()
    if name == "wgpu":
        from .wgpu_renderer import WgpuRenderer
        return WgpuRenderer()
    if name != "auto":
        raise ValueError(f"backend desconocido: {name!r} (auto|cpu|wgpu)")
    try:
        from .wgpu_renderer import WgpuRenderer
        candidate = WgpuRenderer()
        candidate.ensure_ready()  # preflight: falla aquí, no a mitad de frame
        return candidate
    except Exception as exc:
        if log:
            log(f"[auto] wgpu no disponible ({type(exc).__name__}: {exc}); usando cpu")
        from .cpu_renderer import CpuRenderer
        return CpuRenderer()
