"""Backends de render. `WgpuRenderer` es el camino principal; Pillow el fallback."""
from .cpu_renderer import render_pillow
from .wgpu_renderer import WgpuRenderer

__all__ = ["WgpuRenderer", "render_pillow"]
