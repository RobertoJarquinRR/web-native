"""Resolución de `font-family` CSS -> archivo TTF del sistema.

Capa pura (solo pathlib/os): no importa FreeType ni HarfBuzz. Es el único punto
que conoce rutas de fuentes; layout no la usa y los renderers la reutilizan.
Si la familia no está mapeada o el archivo no existe se cae a Segoe UI y, si
tampoco está, a Arial. El fallback es determinista y se documenta en el README.
"""
import os
from pathlib import Path

WINDOWS_FONTS = Path(os.environ.get("WINDIR", r"C:\Windows")) / "Fonts"

# Mapa explícito v1. Las claves van en minúsculas y sin comillas.
FONT_FAMILIES: dict[str, str] = {
    "segoe ui": "segoeui.ttf",
    "segoe ui light": "segoeuil.ttf",
    "segoe ui semibold": "seguisb.ttf",
    "segoe ui semilight": "segoeuisl.ttf",
    "arial": "arial.ttf",
    "helvetica": "arial.ttf",
    "helvetica neue": "arial.ttf",
    "tahoma": "tahoma.ttf",
    "verdana": "verdana.ttf",
    "calibri": "calibri.ttf",
    "times new roman": "times.ttf",
    "georgia": "georgia.ttf",
    "courier new": "cour.ttf",
    "consolas": "consola.ttf",
}

GENERIC = {
    "sans-serif": "segoe ui",
    "system-ui": "segoe ui",
    "-apple-system": "segoe ui",
    "ui-sans-serif": "segoe ui",
    "serif": "times new roman",
    "monospace": "consolas",
}
DEFAULT_FAMILY = "segoe ui"


def first_family(font_family: str) -> str:
    """Primera familia de una lista CSS: '"Segoe UI", Arial, sans-serif'."""
    return (font_family or "").split(",")[0].strip().strip("'\"") or DEFAULT_FAMILY


def resolve_font_file(font_family: str) -> Path:
    """Devuelve un TTF existente; nunca lanza si Segoe UI/Arial están presentes."""
    family = first_family(font_family).lower()
    family = GENERIC.get(family, family)

    filename = FONT_FAMILIES.get(family)
    if filename:
        path = WINDOWS_FONTS / filename
        if path.is_file():
            return path

    # Coincidencia parcial: p. ej. "Segoe UI Semibold" o "Arial Black".
    for name in sorted(FONT_FAMILIES, key=len, reverse=True):
        if name in family:
            path = WINDOWS_FONTS / FONT_FAMILIES[name]
            if path.is_file():
                return path

    for fallback in ("segoeui.ttf", "arial.ttf"):
        path = WINDOWS_FONTS / fallback
        if path.is_file():
            return path
    raise FileNotFoundError(
        f"no hay fuente usable para {font_family!r}: "
        f"faltan {WINDOWS_FONTS / 'segoeui.ttf'} y {WINDOWS_FONTS / 'arial.ttf'}")
