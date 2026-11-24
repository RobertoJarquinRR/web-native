"""Shared helpers for the visual-parity harness.

This module is dev-only tooling: it lives under benchmarks/ and is never
imported by the engine, the ABI, the Rust frontend or the emitter.
"""

from __future__ import annotations

import hashlib
import importlib.metadata
import json
import os
import platform
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

HARNESS_VERSION = "visual-parity-harness/1"
DEFAULT_DPI = 96
DEFAULT_DPR = 1
DEFAULT_PIXEL_DIFF_THRESHOLD = 8


def _configure_stdio() -> None:
    """Avoid cp1252 crashes when Playwright returns Unicode in an error."""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


_configure_stdio()


def repo_root() -> Path:
    """benchmarks/visual-parity/tools/common.py -> repository root."""
    return Path(__file__).resolve().parents[3]


def harness_dir() -> Path:
    return Path(__file__).resolve().parents[1]


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def local_stamp() -> str:
    return datetime.now().strftime("%Y-%m-%d-%H%M%S")


def load_json(path: str | Path) -> Any:
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def write_json(path: str | Path, data: Any) -> None:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    with open(target, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(data, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def write_text(path: str | Path, text: str) -> None:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    with open(target, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def ensure_dir(path: str | Path) -> Path:
    target = Path(path)
    target.mkdir(parents=True, exist_ok=True)
    return target


def output_root(path: str | Path) -> Path:
    """Given out/captures/x.png (or out/x.png) return the harness out/ root."""
    target = Path(path)
    if target.is_dir():
        return target.resolve()
    parent = target.parent
    if parent.name.lower() in {"captures", "meta", "diff", "reports", "logs"}:
        return parent.parent.resolve()
    return parent.resolve()


def sha256_file(path: str | Path, chunk_size: int = 4 * 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            chunk = handle.read(chunk_size)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def cached_sha256(path: str | Path, cache_dir: str | Path) -> str:
    """Hash a (possibly large) executable once per mtime/size."""
    target = Path(path).resolve()
    cache_path = Path(cache_dir) / "sha256-cache.json"
    stat = target.stat()
    cache: dict[str, Any] = {}
    if cache_path.exists():
        try:
            cache = load_json(cache_path)
        except (OSError, ValueError):
            cache = {}
    key = str(target)
    entry = cache.get(key)
    if (
        isinstance(entry, dict)
        and entry.get("size") == stat.st_size
        and entry.get("mtime_ns") == stat.st_mtime_ns
    ):
        return str(entry["sha256"])
    digest = sha256_file(target)
    cache[key] = {
        "size": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
        "sha256": digest,
    }
    write_json(cache_path, cache)
    return digest


def package_version(name: str) -> str | None:
    try:
        return importlib.metadata.version(name)
    except importlib.metadata.PackageNotFoundError:
        return None


def resolve_repo_path(value: str | Path, root: Path | None = None) -> Path:
    path = Path(value)
    if path.is_absolute():
        return path
    base = root if root is not None else repo_root()
    return (base / path).resolve()


def load_fixtures(path: str | Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Load fixtures.json and resolve paths. Raises on malformed input."""
    config = load_json(path)
    fixtures: list[dict[str, Any]] = []
    root = repo_root()
    seen: set[str] = set()
    for raw in config.get("fixtures", []):
        if not isinstance(raw, dict) or not raw.get("name"):
            raise ValueError("fixtures.json: each fixture needs a name")
        name = str(raw["name"])
        if name in seen:
            raise ValueError(f"fixtures.json: duplicated fixture name {name!r}")
        seen.add(name)
        fixture = dict(raw)
        fixture["name"] = name
        if "html" not in fixture:
            raise ValueError(f"fixtures.json: fixture {name!r} has no html")
        fixture["html_path"] = resolve_repo_path(fixture["html"], root)
        if fixture.get("style"):
            fixture["style_path"] = resolve_repo_path(fixture["style"], root)
        else:
            fixture["style_path"] = None
        fixture.setdefault("viewport", {"width": 0, "height": 0})
        fixture.setdefault("regions", [])
        fixture.setdefault("exclude", [])
        fixture.setdefault("fonts", [])
        fixture.setdefault("tolerances", {})
        fixture.setdefault("notes", [])
        fixture.setdefault("gate", False)
        native = fixture.setdefault("native", {"mode": "r1"})
        if native.get("exe"):
            native["exe_path"] = resolve_repo_path(native["exe"], root)
        if native.get("cwd"):
            native["cwd_path"] = resolve_repo_path(native["cwd"], root)
        else:
            native["cwd_path"] = fixture["html_path"].parent
        fixtures.append(fixture)
    return config, fixtures


def fixture_map(fixtures: Iterable[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    return {str(f["name"]): f for f in fixtures}


def split_names(values: Iterable[str] | None) -> list[str]:
    """Accept both 'a', 'b' and 'a,b' on the command line."""
    result: list[str] = []
    for value in values or []:
        for part in str(value).split(","):
            name = part.strip()
            if name:
                result.append(name)
    return result


def validate_no_reasonless_excludes(fixture: dict[str, Any]) -> None:
    for item in fixture.get("exclude", []):
        reason = str(item.get("reason", "")).strip()
        if not reason:
            raise ValueError(
                f"fixture {fixture['name']!r}: exclude {item.get('name')!r} "
                "must carry a non-empty reason"
            )


def run_command(
    command: list[str],
    cwd: str | Path | None = None,
    timeout: int = 300,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    merged_env = os.environ.copy()
    merged_env.setdefault("PYTHONIOENCODING", "utf-8")
    if env:
        merged_env.update(env)
    return subprocess.run(
        command,
        cwd=str(cwd) if cwd else None,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        env=merged_env,
        check=False,
    )


def tail(text: str | None, lines: int = 30) -> list[str]:
    if not text:
        return []
    cleaned = text.replace("\r\n", "\n").replace("\r", "\n").rstrip("\n")
    if not cleaned:
        return []
    return cleaned.split("\n")[-lines:]


def parse_adapter_output(text: str | None) -> dict[str, Any]:
    """Parse the adapter lines printed by runtime_r2.exe / render_png.exe."""
    output = text or ""
    info: dict[str, Any] = {}
    patterns = {
        "backend": r"adapter:\s*backend=([^\s]+)",
        "type": r"adapter:\s*type=([^\s]+)",
        "vendor_id": r"adapter:\s*vendorID=([^\s]+)",
        "device_id": r"adapter:\s*deviceID=([^\s]+)",
        "device": r"adapter:\s*device=([^\n\r]+)",
        "vendor": r"adapter:\s*vendor=([^\n\r]+)",
        "architecture": r"adapter:\s*architecture=([^\n\r]+)",
        "description": r"adapter:\s*description=([^\n\r]+)",
    }
    for key, pattern in patterns.items():
        match = re.search(pattern, output)
        if match:
            value = match.group(1).strip()
            if value and value != "(none)":
                info[key] = value
    device = str(info.get("device", ""))
    adapter_type = str(info.get("type", "")).lower()
    info["software"] = bool(
        "basic render" in device.lower()
        or "warp" in device.lower()
        or "llvmpipe" in device.lower()
        or adapter_type in {"cpu", "software"}
    )
    return info


def parse_render_line(text: str | None) -> dict[str, Any]:
    """Extract layout/frame timings from runtime_r2.exe --render."""
    output = text or ""
    result: dict[str, Any] = {}
    match = re.search(
        r"render:\s+.+?\s+(\d+)x(\d+)\s+nodes=(\d+)\s+vertices=(\d+)\s+"
        r"glyph_vertices=(\d+)\s+image_vertices=(\d+)\s+"
        r"layout=([0-9.]+)ms\s+frame=([0-9.]+)ms",
        output,
    )
    if match:
        result.update(
            {
                "width": int(match.group(1)),
                "height": int(match.group(2)),
                "nodes": int(match.group(3)),
                "vertices": int(match.group(4)),
                "glyph_vertices": int(match.group(5)),
                "image_vertices": int(match.group(6)),
                "layout_ms": float(match.group(7)),
                "frame_ms": float(match.group(8)),
            }
        )
    return result


def git_info(root: Path | None = None) -> dict[str, Any]:
    base = root if root is not None else repo_root()
    info: dict[str, Any] = {"commit": None, "dirty": None, "changed_count": None}
    try:
        commit = run_command(["git", "-C", str(base), "rev-parse", "--short", "HEAD"], timeout=15)
        if commit.returncode == 0:
            info["commit"] = commit.stdout.strip()
        status = run_command(["git", "-C", str(base), "status", "--porcelain"], timeout=30)
        if status.returncode == 0:
            lines = [line for line in status.stdout.splitlines() if line.strip()]
            info["changed_count"] = len(lines)
            info["dirty"] = len(lines) > 0
    except (OSError, subprocess.SubprocessError):
        pass
    return info


def environment_info() -> dict[str, Any]:
    return {
        "os": platform.platform(),
        "machine": platform.machine(),
        "python": sys.version.split()[0],
        "python_executable": sys.executable,
        "pillow": package_version("pillow"),
        "numpy": package_version("numpy"),
        "playwright": package_version("playwright"),
        "harness": HARNESS_VERSION,
        "git": git_info(),
    }


def image_size(path: str | Path) -> tuple[int, int, str]:
    from PIL import Image

    with Image.open(path) as image:
        return image.width, image.height, image.mode


def format_float(value: Any, digits: int = 6) -> str:
    if value is None:
        return "—"
    try:
        return f"{float(value):.{digits}f}"
    except (TypeError, ValueError):
        return str(value)


def markdown_table(headers: list[str], rows: list[list[Any]]) -> str:
    def cell(value: Any) -> str:
        text = "—" if value is None else str(value)
        return text.replace("|", "\\|").replace("\n", " ")

    lines = [
        "| " + " | ".join(cell(h) for h in headers) + " |",
        "|" + "|".join("---" for _ in headers) + "|",
    ]
    for row in rows:
        lines.append("| " + " | ".join(cell(v) for v in row) + " |")
    return "\n".join(lines)


CAVEATS: list[dict[str, str]] = [
    {
        "id": "text-aa",
        "title": "Antialiasing de texto",
        "text": (
            "DirectWrite/GDI y Skia rasterizan distinto aunque usen la misma fuente. "
            "Se fuerza antialiasing en escala de grises en Chromium (--disable-lcd-text); "
            "aun asi el hinting puede mover un pixel. Las regiones text tienen umbral "
            "propio y se reporta ademas geometria de tinta (bbox/IoU)."
        ),
    },
    {
        "id": "dpi",
        "title": "DPI y DPR",
        "text": (
            "El gate corre a 96 DPI / DPR 1. Chromium usa "
            "--force-device-scale-factor=1. Un monitor al 125/150/200 % es otra matriz "
            "y no se compara aqui; no se rescata la escala de la ventana."
        ),
    },
    {
        "id": "fonts",
        "title": "Fuentes",
        "text": (
            "Allowlist por fixture (Segoe UI, Segoe UI Semibold, Consolas). El harness "
            "comprueba los archivos de C:\\Windows\\Fonts y falla si falta la fuente, en "
            "vez de comparar un fallback silencioso. Sin web fonts."
        ),
    },
    {
        "id": "color",
        "title": "Color",
        "text": (
            "sRGB, 8 bits, sin HDR. Chromium usa --force-color-profile=srgb. Un perfil "
            "distinto invalida la comparacion; el perfil usado se registra en el reporte."
        ),
    },
    {
        "id": "images",
        "title": "Imagenes",
        "text": (
            "WIC y Skia remuestrean distinto. El piloto usa posters 180x240 a 180x240 "
            "(sin escalado). Si un fixture futuro escala imagenes, esa region se marca "
            "y se reporta aparte."
        ),
    },
    {
        "id": "shadows",
        "title": "Sombras y blur",
        "text": (
            "El core aproxima el blur. Los fixtures actuales no usan box-shadow; si se "
            "anaden, la banda de sombra se excluye o usa umbral laxo con reason "
            "explicita en fixtures.json."
        ),
    },
    {
        "id": "fractional",
        "title": "Coordenadas fraccionarias",
        "text": (
            "El layout admite 0.01 px; el rasterizador redondea. Los fixtures del gate "
            "usan geometria entera o medios pixeles estables."
        ),
    },
    {
        "id": "scroll",
        "title": "Scroll y overlays",
        "text": (
            "Sin scroll visible: Chromium arranca con --hide-scrollbars y el nativo no "
            "dibuja scrollbar. El piloto se captura en scroll 0. position: fixed/sticky "
            "requiere fixtures propios cuando exista."
        ),
    },
    {
        "id": "os-composition",
        "title": "Composicion del OS",
        "text": (
            "No se capturan ventanas: solo el camino offscreen del renderer. Nada de "
            "DWM, sombras de ventana, transparencias ni temas del sistema."
        ),
    },
    {
        "id": "backend",
        "title": "Backend nativo",
        "text": (
            "Cada corrida declara backend, adaptador y si es software. WARP y hardware "
            "no son comparables entre si; no se mezclan en un reporte. En esta maquina "
            "de referencia el nativo suele usar D3D12 sobre Microsoft Basic Render "
            "Driver (WARP, type=cpu)."
        ),
    },
    {
        "id": "harness-only",
        "title": "Dependencia dev-only",
        "text": (
            "Chromium, Playwright, Pillow y NumPy viven solo en benchmarks/visual-parity. "
            "El engine, la ABI, el install size y el .exe no cambian ni enlazan nada del "
            "harness. Un fallo de Chromium no bloquea build ni producto."
        ),
    },
    {
        "id": "ssim",
        "title": "Implementacion de SSIM",
        "text": (
            "SSIM se calcula sobre luma con ventana box 8x8 y bordes reflejados, segun "
            "docs/visual-parity.md. No es identico a la ventana 11x11 gaussiana de otras "
            "librerias; los numeros son comparables dentro de este harness, no contra "
            "reportes de otras herramientas."
        ),
    },
    {
        "id": "r1-r2",
        "title": "R1 vs R2",
        "text": (
            "Los fixtures rect-only usan R1 (render_png.exe, escena horneada via "
            "src/build.ps1). El piloto usa R2 (runtime_r2.exe --render). Son dos caminos "
            "distintos; no se promedian ni se comparan entre si."
        ),
    },
]


def caveats_json() -> list[dict[str, str]]:
    return [dict(item) for item in CAVEATS]
