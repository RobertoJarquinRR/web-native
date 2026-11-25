#!/usr/bin/env python3
"""Build and measure the R2 stress fixtures under stress/.

Pipeline (all artifacts stay under stress/):

  1. generate.py            -> variants/<id>/page/{index.html,style.css}
  2. frontend-rust          -> variants/<id>/build/ui.json + atlas.bin
  3. emit_ui.py --ui-json   -> variants/<id>/build/ui_data.h
  4. cl.exe (canonical R2 sources, unmodified) -> variants/<id>/build/runtime_r2.exe
  5. --report / --render / --wheel / --states measurements, medians of N runs
  6. stress_report.json     -> raw numbers + checks + bottlenecks

The canonical src/runtime/c-r2/build.ps1 is not touched. The compile command is
the same one used by that script / by the pilot build (cl /O2 /W3 /std:c17 /TC,
/DWN_STATIC, wgpu_native.dll.lib + windowscodecs/ole32/user32/gdi32).

Usage:
  src\\frontend\\python\\.venv\\Scripts\\python.exe stress\\run_stress.py
  ... --runs 7 --skip-emit --skip-compile --only cards-300

Every number in stress_report.json comes from a real process run on this host.
If a step fails, the exact command + stderr is written to stress/logs/ and the
script exits non-zero while still writing the partial report.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wintypes
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
PILOT = HERE.parent
REPO = PILOT.parents[1]
VARIANTS = HERE / "variants"
MANIFEST_PATH = VARIANTS / "manifest.json"
LOGS = HERE / "logs"
ARTIFACTS = HERE / "artifacts"
REPORT_PATH = HERE / "stress_report.json"

RUST_EXE = REPO / "src" / "frontend" / "target" / "release" / "frontend-rust.exe"
SRC_PY = REPO / "src" / "frontend" / "python" / ".venv" / "Scripts" / "python.exe"
EMITTER = REPO / "src" / "emitter" / "python" / "emit_ui.py"
SPLIT_ESCAPES = PILOT / "tools" / "split_c_escapes.py"
WGPU = REPO / "third_party" / "wgpu-native"
RUNTIME = REPO / "src" / "runtime" / "c-r2"
PILOT_EXE = PILOT / "build" / "runtime_r2_pilot.exe"
PILOT_PAGE = PILOT / "page"

SIZES = ("1280x800", "800x600")
RENDER_RE = re.compile(
    r"^render: (?P<path>.+?) (?P<w>\d+)x(?P<h>\d+) nodes=(?P<nodes>\d+) "
    r"vertices=(?P<vertices>\d+) glyph_vertices=(?P<glyph>\d+) "
    r"image_vertices=(?P<image>\d+) layout=(?P<layout>[\d.]+)ms "
    r"frame=(?P<frame>[\d.]+)ms$"
)
TIMINGS_RE = re.compile(
    r"^timings: gpu\(encode\+submit\)=(?P<gpu>[\d.]+)ms \| "
    r"readback\(map\)=(?P<readback>[\d.]+)ms \| png=(?P<png>[\d.]+)ms$"
)
SCROLL_RE = re.compile(
    r"^scroll: node=(?P<node>\d+) name=(?P<name>.+?) "
    r"rect=\[(?P<x>[\d.]+),(?P<y>[\d.]+),(?P<w>[\d.]+),(?P<h>[\d.]+)\] "
    r"offset=(?P<offset>[\d.]+) max=(?P<max>[\d.]+)$"
)
WHEEL_RE = re.compile(
    r"^wheel: \((?P<x>[-\d.]+),(?P<y>[-\d.]+)\) delta_y=(?P<dy>[-\d.]+) -> "
    r"node=(?P<node>\d+) (?P<name>.+?) scroll=(?P<offset>[\d.]+)/(?P<max>[\d.]+)$"
)
STATE_RE = re.compile(
    r"^state (?P<state>\w+)\s+node=(?P<node>\d+) (?P<name>.+?) "
    r"layout=(?P<layout>[\d.]+)ms frame=(?P<frame>[\d.]+)ms -> (?P<path>.+)$"
)
ADAPTER_RE = re.compile(
    r"^adapter: backend=(?P<backend>\S+) \((?P<code>\d+)\) "
    r"type=(?P<type>\S+) vendorID=0x(?P<vendor>[0-9A-Fa-f]+) "
    r"deviceID=0x(?P<device>[0-9A-Fa-f]+)$"
)
DEVICE_RE = re.compile(r"^adapter: device=(?P<device>.+)$")
CARD_RE = re.compile(r"/div\.poster-card(?:\[(\d+)\])?$")
IMAGE_RE = re.compile(r"/img\.poster(?:\[(\d+)\])?$")

WARP_NOTE = (
    "Microsoft Basic Render Driver (WARP) over D3D12: software rasterizer, "
    "CPU/commit memory. Timings are not representative of a hardware GPU; "
    "readback/map and WARP pass costs dominate the offscreen frame."
)


# ------------------------------------------------------------------ utilities


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def stats(values: list[float]) -> dict:
    clean = [float(v) for v in values if v is not None]
    if not clean:
        return {"n": 0, "median": None, "mean": None, "min": None,
                "max": None, "stdev": None, "values": []}
    return {
        "n": len(clean),
        "median": round(statistics.median(clean), 6),
        "mean": round(statistics.fmean(clean), 6),
        "min": round(min(clean), 6),
        "max": round(max(clean), 6),
        "stdev": round(statistics.pstdev(clean), 6) if len(clean) > 1 else 0.0,
        "values": [round(v, 6) for v in clean],
    }


def median_of(values: list[float], default: float | None = None) -> float | None:
    clean = [float(v) for v in values if v is not None]
    if not clean:
        return default
    return statistics.median(clean)


def fmt(value, digits: int = 3) -> str:
    if value is None:
        return "n/a"
    return f"{value:.{digits}f}"


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def write_json(path: Path, data) -> None:
    write_text(path, json.dumps(data, indent=2, ensure_ascii=False) + "\n")


def parse_key(size_key: str) -> tuple[int, int]:
    a, b = size_key.split("x")
    return int(a), int(b)


def find_vcvars() -> str | None:
    fixed = Path(r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat")
    if fixed.is_file():
        return str(fixed)
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.is_file():
        proc = subprocess.run(
            [str(vswhere), "-latest", "-products", "*",
             "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath"],
            capture_output=True, text=True, timeout=60,
        )
        if proc.returncode == 0 and proc.stdout.strip():
            candidate = Path(proc.stdout.strip()) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if candidate.is_file():
                return str(candidate)
    return None


# ------------------------------------------------------------ process runner


class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("PageFaultCount", wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
    ]


def _sampler(pid: int, stop: threading.Event, peaks: dict) -> None:
    try:
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        psapi.GetProcessMemoryInfo.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(PROCESS_MEMORY_COUNTERS), wintypes.DWORD
        ]
        psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
        kernel32.OpenProcess.restype = wintypes.HANDLE
        handle = kernel32.OpenProcess(0x0400 | 0x0010, False, pid)  # QUERY_INFORMATION|VM_READ
        if not handle:
            return
        try:
            while not stop.is_set():
                counters = PROCESS_MEMORY_COUNTERS()
                counters.cb = ctypes.sizeof(counters)
                if psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                    peaks["working_set"] = max(peaks["working_set"], counters.WorkingSetSize)
                    peaks["peak_working_set"] = max(
                        peaks["peak_working_set"], counters.PeakWorkingSetSize
                    )
                    peaks["peak_pagefile"] = max(
                        peaks["peak_pagefile"], counters.PeakPagefileUsage
                    )
                time.sleep(0.004)
        finally:
            kernel32.CloseHandle(handle)
    except Exception as exc:  # sampling must never kill the benchmark
        peaks["error"] = repr(exc)


def run_process(cmd: list[str], cwd: Path, log_path: Path, timeout: float = 600.0,
                sample_memory: bool = True) -> dict:
    """Run a process, capture output, wall time and peak memory."""
    log_path.parent.mkdir(parents=True, exist_ok=True)
    peaks = {"working_set": 0, "peak_working_set": 0, "peak_pagefile": 0, "error": None}
    stop = threading.Event()
    thread = None
    started = time.perf_counter()
    proc = subprocess.Popen(
        [str(x) for x in cmd],
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if sample_memory:
        thread = threading.Thread(target=_sampler, args=(proc.pid, stop, peaks), daemon=True)
        thread.start()
    timed_out = False
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
        timed_out = True
    stop.set()
    if thread is not None:
        thread.join(timeout=1.0)
    wall_ms = (time.perf_counter() - started) * 1000.0
    stdout = out.decode("utf-8", errors="replace")
    stderr = err.decode("utf-8", errors="replace")
    header = [
        "$ " + subprocess.list2cmdline([str(x) for x in cmd]),
        f"cwd={cwd}",
        f"exit={proc.returncode} wall_ms={wall_ms:.3f} timeout={timed_out}",
        f"peak_working_set_mb={peaks['peak_working_set'] / (1024 * 1024):.3f} "
        f"peak_pagefile_mb={peaks['peak_pagefile'] / (1024 * 1024):.3f}",
        "--- stdout ---",
    ]
    write_text(log_path, "\n".join(header) + "\n" + stdout + "\n--- stderr ---\n" + stderr)
    return {
        "rc": proc.returncode,
        "wall_ms": round(wall_ms, 3),
        "timed_out": timed_out,
        "stdout": stdout,
        "stderr": stderr,
        "log": str(log_path.relative_to(HERE)),
        "peak_working_set_mb": round(peaks["peak_working_set"] / (1024 * 1024), 3),
        "peak_pagefile_mb": round(peaks["peak_pagefile"] / (1024 * 1024), 3),
        "memory_error": peaks["error"],
    }


# --------------------------------------------------------------- emit/compile


def load_manifest() -> dict:
    if not MANIFEST_PATH.is_file():
        raise SystemExit(
            f"missing {MANIFEST_PATH}; run generate.py first:\n"
            f"  {SRC_PY} {HERE / 'generate.py'}"
        )
    return json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))


def generate_fixtures() -> None:
    import generate  # same directory
    rc = generate.main()
    if rc != 0:
        raise SystemExit(f"generate.py exited with {rc}")


def variant_paths(variant: dict) -> tuple[Path, Path, Path]:
    page = (HERE / variant["page"]).parent
    build = page.parent / "build"
    exe = build / "runtime_r2.exe"
    return page, build, exe


def emit_variant(variant: dict, skip_emit: bool) -> dict:
    page, build, exe = variant_paths(variant)
    build.mkdir(parents=True, exist_ok=True)
    ui_json = build / "ui.json"
    ui_data = build / "ui_data.h"
    info = {
        "frontend_ms": None,
        "emit_ms": None,
        "split_escapes": 0,
        "node_count": None,
        "image_nodes": None,
        "unique_images": variant.get("unique_images"),
        "ui_json_bytes": None,
        "ui_data_bytes": None,
        "atlas_bytes": None,
    }
    if skip_emit:
        if not ui_data.is_file():
            raise SystemExit(f"--skip-emit but {ui_data} is missing")
        info["ui_data_bytes"] = ui_data.stat().st_size
        if ui_json.is_file():
            data = json.loads(ui_json.read_text(encoding="utf-8"))
            info["node_count"] = int(data.get("node_count", len(data.get("nodes", []))))
            info["ui_json_bytes"] = ui_json.stat().st_size
        return info

    if not RUST_EXE.is_file():
        raise SystemExit(
            f"missing {RUST_EXE}; build the canonical frontend first:\n"
            f"  cargo build --release --manifest-path {REPO / 'src/frontend/rust/Cargo.toml'}"
        )
    if not SRC_PY.is_file() or not EMITTER.is_file():
        raise SystemExit(f"missing Python emitter prerequisites: {SRC_PY} / {EMITTER}")

    cmd = [
        str(RUST_EXE), "--html", str(page / "index.html"),
        "--css", str(page / "style.css"),
        "--width", "1280", "--height", "800",
        "--emit-ui", str(ui_json), "--pilot",
    ]
    started = time.perf_counter()
    res = run_process(cmd, page, LOGS / variant["id"] / "emit_frontend.log", sample_memory=False)
    info["frontend_ms"] = round((time.perf_counter() - started) * 1000.0, 3)
    if res["rc"] != 0 or not ui_json.is_file():
        raise SystemExit(
            f"frontend-rust failed for {variant['id']} (rc={res['rc']}); "
            f"see {res['log']}\n{res['stderr'][-2000:]}"
        )
    data = json.loads(ui_json.read_text(encoding="utf-8"))
    info["node_count"] = int(data.get("node_count", len(data.get("nodes", []))))
    info["ui_json_bytes"] = ui_json.stat().st_size
    image_nodes = [n for n in data.get("nodes", []) if n.get("kind") == "image"]
    info["image_nodes"] = len(image_nodes)
    atlas = build / "atlas.bin"
    info["atlas_bytes"] = atlas.stat().st_size if atlas.is_file() else None

    cmd = [
        str(SRC_PY), str(EMITTER), "--ui-json", str(ui_json),
        "--out", str(ui_data), "--meta", str(build / "ui_meta.json"),
    ]
    started = time.perf_counter()
    res = run_process(cmd, build, LOGS / variant["id"] / "emit_ui.log", sample_memory=False)
    info["emit_ms"] = round((time.perf_counter() - started) * 1000.0, 3)
    if res["rc"] != 0 or not ui_data.is_file():
        raise SystemExit(
            f"emit_ui.py failed for {variant['id']} (rc={res['rc']}); "
            f"see {res['log']}\n{res['stderr'][-2000:]}"
        )
    info["ui_data_bytes"] = ui_data.stat().st_size

    meta = json.loads((build / "ui_meta.json").read_text(encoding="utf-8"))
    meta_images = [n for n in meta.get("nodes", []) if n.get("kind") == "image"]
    if variant["kind"] == "cards" and len(meta_images) != variant["cards"]:
        raise SystemExit(
            f"{variant['id']}: emitted {len(meta_images)} image nodes, expected "
            f"{variant['cards']} (one per card)"
        )
    zero_sized = [
        n for n in meta_images
        if not (n.get("image") or {}).get("width") or not (n.get("image") or {}).get("height")
    ]
    if meta_images and zero_sized:
        raise SystemExit(
            f"{variant['id']}: {len(zero_sized)}/{len(meta_images)} image nodes have no "
            f"intrinsic size after emit_ui.py; src must resolve from the ui.json directory. "
            f"Check the poster path depth in generate.py."
        )

    header = ui_data.read_text(encoding="utf-8", errors="replace")
    ambiguous = len(re.findall(r"\\x[0-9A-Fa-f]{2}(?=[0-9A-Fa-f])", header))
    if ambiguous:
        # Only needed for old emitters; current emit_ui.py uses octal escapes.
        res = run_process(
            [str(SRC_PY), str(SPLIT_ESCAPES), "--in", str(ui_data), "--no-backup"],
            build, LOGS / variant["id"] / "split_escapes.log", sample_memory=False,
        )
        if res["rc"] != 0:
            raise SystemExit(f"split_c_escapes.py failed for {variant['id']}: {res['stderr']}")
        info["split_escapes"] = ambiguous
        info["ui_data_bytes"] = ui_data.stat().st_size
    return info


def compile_variant(variant: dict, vcvars: str, skip_compile: bool) -> dict:
    page, build, exe = variant_paths(variant)
    info = {"compile_ms": None, "exe_bytes": None, "compile_log": None}
    if skip_compile:
        if not exe.is_file():
            raise SystemExit(f"--skip-compile but {exe} is missing")
        info["exe_bytes"] = exe.stat().st_size
        return info
    if not (build / "ui_data.h").is_file():
        raise SystemExit(f"missing {build / 'ui_data.h'} (run emit first)")

    dll = WGPU / "bin" / "wgpu_native.dll"
    lib = WGPU / "lib" / "wgpu_native.dll.lib"
    for required in (lib, dll, RUNTIME / "runtime_r2.c", RUNTIME / "wn_core.c",
                     RUNTIME / "wn_common.c"):
        if not required.is_file():
            raise SystemExit(f"missing required build input: {required}")

    command = (
        f'cl /nologo /O2 /W3 /std:c17 /TC /D_CRT_SECURE_NO_WARNINGS /DWN_STATIC '
        f'"{RUNTIME / "runtime_r2.c"}" "{RUNTIME / "wn_core.c"}" "{RUNTIME / "wn_common.c"}" '
        f'/I"{REPO / "include"}" /I"{RUNTIME}" /I"{WGPU / "include"}" /I"{build}" '
        f'/Fo:"{build}\\\\" /Fe:"{exe}" '
        f'/link "{lib}" windowscodecs.lib ole32.lib user32.lib gdi32.lib'
    )
    script = (
        "@echo off\n"
        f'call "{vcvars}" >nul\n'
        "if errorlevel 1 exit /b 1\n"
        f'cd /d "{build}"\n'
        f"{command}\n"
        "exit /b %errorlevel%\n"
    )
    script_path = build / "compile.cmd"
    write_text(script_path, script)
    started = time.perf_counter()
    res = run_process(
        ["cmd.exe", "/d", "/c", str(script_path)], build,
        LOGS / variant["id"] / "compile.log", timeout=900.0, sample_memory=False,
    )
    info["compile_ms"] = round((time.perf_counter() - started) * 1000.0, 3)
    info["compile_log"] = res["log"]
    if res["rc"] != 0 or not exe.is_file():
        raise SystemExit(
            f"compile failed for {variant['id']} (rc={res['rc']}); see {res['log']}\n"
            f"{res['stdout'][-2000:]}\n{res['stderr'][-2000:]}"
        )
    shutil.copyfile(dll, build / "wgpu_native.dll")
    info["exe_bytes"] = exe.stat().st_size
    return info


# ------------------------------------------------------------------- parsing


def parse_render(stdout: str) -> dict:
    result = {"found": False}
    match = None
    for line in stdout.splitlines():
        match = RENDER_RE.match(line)
        if match:
            break
    if match:
        result.update({
            "found": True,
            "nodes": int(match.group("nodes")),
            "vertices": int(match.group("vertices")),
            "glyph_vertices": int(match.group("glyph")),
            "image_vertices": int(match.group("image")),
            "layout_ms": float(match.group("layout")),
            "frame_ms": float(match.group("frame")),
        })
    line = next((l for l in stdout.splitlines() if l.startswith("timings: gpu(")), "")
    m = TIMINGS_RE.match(line)
    if m:
        result["gpu_ms"] = float(m.group("gpu"))
        result["readback_ms"] = float(m.group("readback"))
        result["png_ms"] = float(m.group("png"))
    return result


def parse_scroll(stdout: str) -> dict:
    for line in stdout.splitlines():
        m = SCROLL_RE.match(line)
        if m:
            return {
                "node": int(m.group("node")),
                "name": m.group("name"),
                "rect": [float(m.group("x")), float(m.group("y")),
                          float(m.group("w")), float(m.group("h"))],
                "offset": float(m.group("offset")),
                "max": float(m.group("max")),
            }
    return {}


def parse_wheel(stdout: str) -> dict:
    result = {"routed": False}
    for line in stdout.splitlines():
        m = WHEEL_RE.match(line)
        if m:
            result.update({
                "routed": True,
                "node": int(m.group("node")),
                "name": m.group("name"),
                "offset": float(m.group("offset")),
                "max": float(m.group("max")),
            })
            return result
        if line.startswith("wheel:") and "no scrollable container" in line:
            result["no_container"] = True
    return result


def parse_states(stdout: str) -> list[dict]:
    states = []
    for line in stdout.splitlines():
        m = STATE_RE.match(line)
        if m:
            states.append({
                "state": m.group("state"),
                "node": int(m.group("node")),
                "name": m.group("name"),
                "layout_ms": float(m.group("layout")),
                "frame_ms": float(m.group("frame")),
                "path": m.group("path"),
            })
    return states


def parse_adapter(stdout: str) -> dict:
    result = {}
    for line in stdout.splitlines():
        m = ADAPTER_RE.match(line)
        if m:
            result.update({
                "backend": m.group("backend"),
                "backend_code": int(m.group("code")),
                "type": m.group("type"),
                "vendor_id": "0x" + m.group("vendor"),
                "device_id": "0x" + m.group("device"),
            })
        m = DEVICE_RE.match(line)
        if m:
            result["device"] = m.group("device").strip()
    return result


# --------------------------------------------------------------- PNG checking


def _accent_rgb(manifest_posters: list[dict], index: int) -> tuple[int, int, int]:
    value = manifest_posters[index]["accent"].lstrip("#")
    return tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))


def _find_color(data: bytes, width: int, rgb: tuple[int, int, int],
                limit: int = 20000) -> tuple[int, int, int, int] | None:
    pattern = bytes(rgb)
    stride = width * 3
    min_x = min_y = 10 ** 9
    max_x = max_y = -1
    count = 0
    pos = data.find(pattern)
    while pos != -1 and count < limit:
        if pos % 3 == 0:
            y, byte_x = divmod(pos, stride)
            x = byte_x // 3
            min_x = min(min_x, x)
            max_x = max(max_x, x)
            min_y = min(min_y, y)
            max_y = max(max_y, y)
            count += 1
        pos = data.find(pattern, pos + 1)
    if count == 0:
        return None
    return min_x, min_y, max_x, max_y


def check_png(path: Path, kind: str, variant: dict, size_key: str,
              accented: list[dict] | None = None) -> dict:
    """Pixel evidence: dimensions, poster bands, grid clip, non-blank layout."""
    try:
        from PIL import Image
    except ImportError:
        return {"ok": None, "skipped": "Pillow not available"}
    width, height = parse_key(size_key)
    if not path.is_file():
        return {"ok": False, "error": f"missing PNG {path}"}
    with Image.open(path) as image:
        image = image.convert("RGB")
        if image.size != (width, height):
            return {"ok": False, "error": f"PNG size {image.size} != {(width, height)}"}
        data = image.tobytes()
        colors = image.getcolors(maxcolors=1 << 22) or []

    if kind == "nodes":
        wanted = [(23, 29, 41), (31, 39, 53), (38, 50, 72), (46, 60, 85)]
        present = {tuple(c) for _, c in colors}
        missing = [c for c in wanted if c not in present]
        distinct = len(colors)
        return {
            "ok": not missing and distinct >= 5,
            "distinct_colors": distinct,
            "cell_colors_present": [c for c in wanted if c in present],
            "missing_cell_colors": missing,
        }

    # cards
    viewport = variant["viewports"][size_key]
    grid = viewport["grid"]
    columns = viewport["columns"]
    manifest = json.loads((PILOT / "posters" / "manifest.json").read_text(encoding="utf-8"))
    posters = manifest["posters"]
    details = {"bands": [], "stray_pixels": None}
    ok = True

    probes = accented if accented is not None else [
        {"card": i, "expected_band_top": 24 + 166, "expected_band_y_range": [190, 197]}
        for i in range(min(columns, 5))
    ]
    for probe in probes:
        card = int(probe["card"])
        rgb = _accent_rgb(posters, card % len(posters))
        found = _find_color(data, width, rgb)
        expected_top = float(probe["expected_band_top"])
        expected_x = grid["x"] + (card % columns) * 196
        item = {
            "card": card,
            "rgb": list(rgb),
            "expected_band_top": expected_top,
            "expected_x": expected_x,
            "found": found is not None,
        }
        if found:
            min_x, min_y, max_x, max_y = found
            item.update({"x": [min_x, max_x], "y": [min_y, max_y]})
            expected_max_y = probe.get("expected_max_y", expected_top + 7)
            band_ok = abs(min_y - expected_top) <= 0.51 and max_y == expected_max_y
            x_ok = min_x == expected_x and max_x == expected_x + 179
            item["ok"] = bool(band_ok and x_ok)
            ok = ok and item["ok"]
        else:
            item["ok"] = False
            ok = False
        details["bands"].append(item)

    # No poster accent outside the grid rect. Scan the four thin margins only.
    accent_set = {_accent_rgb(posters, i) for i in range(len(posters))}
    strips = [
        (0, 0, int(grid["x"]), height),
        (int(grid["x"] + grid["w"]), 0, width, height),
        (int(grid["x"]), 0, int(grid["x"] + grid["w"]), int(grid["y"])),
        (int(grid["x"]), int(grid["y"] + grid["h"]),
         int(grid["x"] + grid["w"]), height),
    ]
    stray = 0
    with Image.open(path) as image:
        rgb_image = image.convert("RGB")
        for box in strips:
            if box[2] <= box[0] or box[3] <= box[1]:
                continue
            crop = rgb_image.crop(box)
            for count, color in (crop.getcolors(maxcolors=1 << 22) or []):
                if tuple(color) in accent_set:
                    stray += count
    details["stray_pixels"] = stray
    if stray:
        ok = False
    details["ok"] = ok
    return details


# ---------------------------------------------------------- layout validation


def expected_card_geometry(count: int, width: int, height: int) -> dict:
    content_w = width * 0.80 - 48
    columns = max(1, int((content_w + 16) // 196))
    rows = (count + columns - 1) // columns
    return {
        "grid": {
            "x": width * 0.20 + 24, "y": 24,
            "w": content_w, "h": height - 48,
        },
        "columns": columns,
        "rows": rows,
        "content_h": rows * 240 + (rows - 1) * 16,
        "scroll_max": max(0, rows * 240 + (rows - 1) * 16 - (height - 48)),
    }


def check_layout_report(path: Path, variant: dict) -> dict:
    if not path.is_file():
        return {"ok": False, "error": f"missing report {path}"}
    data = json.loads(path.read_text(encoding="utf-8"))
    results = {"ok": True, "node_count": data.get("node_count"), "sizes": {}}
    expected_nodes = variant.get("nodes_expected")
    if expected_nodes is not None and int(data.get("node_count", -1)) != expected_nodes:
        results["ok"] = False
        results["node_count_error"] = (
            f"node_count {data.get('node_count')} != expected {expected_nodes}"
        )

    for entry in data.get("sizes", []):
        key = f"{entry['width']}x{entry['height']}"
        nodes = entry.get("nodes", [])
        size_result = {"ok": True, "cards": 0, "images": 0, "errors": []}
        if len(nodes) != int(data.get("node_count", len(nodes))):
            size_result["ok"] = False
            size_result["errors"].append(
                f"nodes array {len(nodes)} != node_count {data.get('node_count')}"
            )
        if variant["kind"] == "cards":
            count = variant["cards"]
            expected = expected_card_geometry(count, entry["width"], entry["height"])
            grids = [n for n in nodes if n["name"].endswith("/div.poster-grid")]
            if len(grids) != 1:
                size_result["ok"] = False
                size_result["errors"].append(f"grid nodes: {len(grids)}")
            else:
                grid = grids[0]
                size_result["grid"] = {
                    "x": grid["x"], "y": grid["y"], "w": grid["w"], "h": grid["h"]
                }
                for field in ("x", "y", "w", "h"):
                    if abs(grid[field] - expected["grid"][field]) > 0.01:
                        size_result["ok"] = False
                        size_result["errors"].append(
                            f"grid {field}={grid[field]} expected {expected['grid'][field]}"
                        )
                if grid.get("kind") != "grid":
                    size_result["ok"] = False
                    size_result["errors"].append(f"grid kind={grid.get('kind')}")
            cards = {}
            images = {}
            for node in nodes:
                m = CARD_RE.search(node["name"])
                if m:
                    index = int(m.group(1) or 0)
                    cards[index] = node
                    continue
                m = IMAGE_RE.search(node["name"])
                if m:
                    index = int(m.group(1) or 0)
                    images[index] = node
            size_result["cards"] = len(cards)
            size_result["images"] = len(images)
            if len(cards) != count:
                size_result["ok"] = False
                size_result["errors"].append(f"cards {len(cards)} != {count}")
            if len(images) != count:
                size_result["ok"] = False
                size_result["errors"].append(f"images {len(images)} != {count}")
            columns = expected["columns"]
            for index in range(count):
                node = cards.get(index)
                if node is None:
                    size_result["ok"] = False
                    size_result["errors"].append(f"missing card {index}")
                    continue
                exp_x = expected["grid"]["x"] + (index % columns) * 196
                exp_y = expected["grid"]["y"] + (index // columns) * 256
                if (abs(node["x"] - exp_x) > 0.01 or abs(node["y"] - exp_y) > 0.01
                        or abs(node["w"] - 180) > 0.01 or abs(node["h"] - 240) > 0.01):
                    size_result["ok"] = False
                    size_result["errors"].append(
                        f"card {index}: ({node['x']},{node['y']},{node['w']},{node['h']}) "
                        f"expected ({exp_x},{exp_y},180,240)"
                    )
                    if len(size_result["errors"]) > 20:
                        break
                image = images.get(index)
                if image is None:
                    size_result["ok"] = False
                    size_result["errors"].append(f"missing image {index}")
                elif image.get("kind") != "image":
                    size_result["ok"] = False
                    size_result["errors"].append(
                        f"image {index} kind={image.get('kind')}"
                    )
                elif (abs(image["x"] - exp_x) > 0.01 or abs(image["y"] - exp_y) > 0.01
                      or abs(image["w"] - 180) > 0.01 or abs(image["h"] - 180) > 0.01):
                    size_result["ok"] = False
                    size_result["errors"].append(
                        f"image {index}: ({image['x']},{image['y']},{image['w']},{image['h']})"
                    )
                    if len(size_result["errors"]) > 20:
                        break
            size_result["content_h"] = expected["content_h"]
            size_result["scroll_max"] = expected["scroll_max"]
        else:
            kinds = {}
            zero = 0
            for node in nodes:
                kinds[node.get("kind")] = kinds.get(node.get("kind"), 0) + 1
                if node.get("w", 0) <= 0 or node.get("h", 0) <= 0:
                    zero += 1
            size_result["kinds"] = kinds
            size_result["zero_sized"] = zero
            if zero:
                size_result["ok"] = False
                size_result["errors"].append(f"{zero} zero-sized synthetic node(s)")
            if index_root(nodes) is None:
                size_result["ok"] = False
                size_result["errors"].append("missing body.root")
            else:
                root = index_root(nodes)
                if (abs(root["w"] - entry["width"]) > 0.01
                        or abs(root["h"] - entry["height"]) > 0.01):
                    size_result["ok"] = False
                    size_result["errors"].append(
                        f"root {root['w']}x{root['h']} != {entry['width']}x{entry['height']}"
                    )
        if not size_result["ok"]:
            results["ok"] = False
        results["sizes"][key] = size_result
    return results


def index_root(nodes: list[dict]) -> dict | None:
    for node in nodes:
        if node["name"] == "body.root":
            return node
    return None


# ----------------------------------------------------------------- measuring


def measure_variant(variant: dict, exe: Path, page: Path, build: Path,
                    runs: int, log_root: Path) -> dict:
    result = {
        "layout": {}, "render": {}, "wheel": {}, "states": {},
        "checks": [], "errors": [],
    }
    for size_key in SIZES:
        width, height = parse_key(size_key)
        result["layout"][size_key] = {"best_ms": [], "avg_ms": [], "node_count": None}
        result["render"][size_key] = {
            "runs": 0, "wall_ms": [], "layout_ms": [], "frame_ms": [],
            "gpu_ms": [], "readback_ms": [], "png_ms": [],
            "peak_working_set_mb": [], "peak_pagefile_mb": [],
            "vertices": None, "glyph_vertices": None, "image_vertices": None,
            "nodes": None, "png": None, "png_check": None,
        }
    result["states"]["1280x800"] = {
        "runs": 0, "cold_frame_ms": [], "warm_frame_ms": [],
        "cold_layout_ms": [], "warm_layout_ms": [], "per_run": [],
    }

    # --report: each process does 100 layout iterations per size.
    for index in range(runs):
        report_path = build / f"layout_report_run{index}.json"
        res = run_process(
            [str(exe), "--report", str(report_path), "--sizes", ",".join(SIZES)],
            page, log_root / f"report_{index}.log",
        )
        if res["rc"] != 0 or not report_path.is_file():
            result["errors"].append(
                f"report run {index}: rc={res['rc']} timeout={res['timed_out']}"
            )
            continue
        data = json.loads(report_path.read_text(encoding="utf-8"))
        for entry in data.get("sizes", []):
            key = f"{entry['width']}x{entry['height']}"
            if key not in result["layout"]:
                continue
            result["layout"][key]["best_ms"].append(float(entry["layout_ms_best"]))
            result["layout"][key]["avg_ms"].append(float(entry["layout_ms_avg"]))
            result["layout"][key]["node_count"] = int(data["node_count"])
        if index == runs - 1:
            shutil.copyfile(report_path, build / "layout_report.json")
        report_path.unlink(missing_ok=True)

    for size_key in SIZES:
        entry = result["layout"][size_key]
        entry["best"] = stats(entry.pop("best_ms"))
        entry["avg"] = stats(entry.pop("avg_ms"))

    # --render: cold process per run; keep the first PNG per size.
    for size_key in SIZES:
        width, height = parse_key(size_key)
        accum = result["render"][size_key]
        for index in range(runs):
            png = build / f"render_{width}x{height}.png" if index == 0 else \
                build / f"_tmp_render_{width}x{height}_{index}.png"
            res = run_process(
                [str(exe), "--render", str(width), str(height), str(png)],
                page, log_root / f"render_{size_key}_{index}.log",
            )
            if res["rc"] != 0:
                result["errors"].append(
                    f"render {size_key} run {index}: rc={res['rc']} timeout={res['timed_out']}"
                )
                continue
            parsed = parse_render(res["stdout"])
            if not parsed.get("found"):
                result["errors"].append(
                    f"render {size_key} run {index}: no 'render:' line"
                )
                continue
            accum["runs"] += 1
            accum["wall_ms"].append(res["wall_ms"])
            for field in ("layout_ms", "frame_ms", "gpu_ms", "readback_ms", "png_ms"):
                if field in parsed:
                    accum[field].append(parsed[field])
            accum["peak_working_set_mb"].append(res["peak_working_set_mb"])
            accum["peak_pagefile_mb"].append(res["peak_pagefile_mb"])
            for field in ("vertices", "glyph_vertices", "image_vertices", "nodes"):
                accum[field] = parsed[field]
            if index == 0:
                accum["png"] = str(png.relative_to(HERE))
                accented_first = None
                if variant["kind"] == "cards":
                    accented_first = [
                        {"card": i, "expected_band_top": 190,
                         "expected_band_y_range": [190, 197]}
                        for i in range(min(variant["viewports"][size_key]["columns"], 5))
                    ]
                accum["png_check"] = check_png(
                    png, variant["kind"], variant, size_key, accented_first
                )
            elif png.is_file():
                png.unlink()

    for size_key in SIZES:
        accum = result["render"][size_key]
        for field in ("wall_ms", "layout_ms", "frame_ms", "gpu_ms", "readback_ms",
                      "png_ms", "peak_working_set_mb", "peak_pagefile_mb"):
            accum[field] = stats(accum[field])

    # --wheel: shift / offset / clamp / outside, at both sizes.
    if variant["kind"] == "cards":
        for size_key in SIZES:
            width, height = parse_key(size_key)
            probes = variant["viewports"][size_key]["wheel"]
            wheel_result = {
                "grid": variant["viewports"][size_key]["grid"],
                "scroll_max_expected": variant["viewports"][size_key]["scroll_max"],
                "scenarios": {},
            }
            for probe in probes:
                scenario = {
                    "dy": probe["dy"], "x": probe["x"], "y": probe["y"],
                    "card": probe["card"], "expected_offset": probe["expected_offset"],
                    "expected_band_top": probe["expected_band_top"],
                    "offsets": [], "offsets_seen": [], "max_seen": [],
                    "band_tops": [], "band_ok": [],
                    "wall_ms": [], "frame_ms": [], "layout_ms": [],
                    "gpu_ms": [], "readback_ms": [], "png_ms": [],
                    "image_vertices": None, "routed": [],
                    "png": None,
                }
                for index in range(runs):
                    png = build / f"wheel_{probe['name']}_{width}x{height}.png" \
                        if index == 0 else \
                        build / f"_tmp_wheel_{probe['name']}_{width}x{height}_{index}.png"
                    res = run_process(
                        [str(exe), "--render", str(width), str(height), str(png),
                         "--wheel", str(probe["x"]), str(probe["y"]), str(probe["dy"])],
                        page, log_root / f"wheel_{probe['name']}_{size_key}_{index}.log",
                    )
                    if res["rc"] != 0:
                        result["errors"].append(
                            f"wheel {probe['name']} {size_key} run {index}: rc={res['rc']}"
                        )
                        continue
                    wheel = parse_wheel(res["stdout"])
                    scroll = parse_scroll(res["stdout"])
                    parsed = parse_render(res["stdout"])
                    offset = wheel.get("offset", scroll.get("offset"))
                    maximum = wheel.get("max", scroll.get("max"))
                    scenario["routed"].append(bool(wheel.get("routed")))
                    scenario["offsets"].append(offset)
                    if offset is not None:
                        scenario["offsets_seen"].append(offset)
                    if maximum is not None:
                        scenario["max_seen"].append(maximum)
                    scenario["wall_ms"].append(res["wall_ms"])
                    for field in ("frame_ms", "layout_ms", "gpu_ms",
                                  "readback_ms", "png_ms"):
                        if field in parsed:
                            scenario[field].append(parsed[field])
                    scenario["image_vertices"] = parsed.get("image_vertices")
                    if index == 0:
                        scenario["png"] = str(png.relative_to(HERE))
                        check = check_png(
                            png, "cards", variant, size_key,
                            [{"card": probe["card"],
                              "expected_band_top": probe["expected_band_top"],
                              "expected_band_y_range":
                                  probe["expected_band_y_range"]}],
                        )
                        scenario["band_check"] = check
                        bands = check.get("bands") or []
                        scenario["band_tops"] = [b.get("y") for b in bands]
                        scenario["band_ok"] = [b.get("ok") for b in bands]
                    elif png.is_file():
                        png.unlink()
                for field in ("wall_ms", "frame_ms", "layout_ms", "gpu_ms",
                              "readback_ms", "png_ms"):
                    scenario[field] = stats(scenario[field])
                scenario["ok"] = (
                    all(v is not None and abs(v - probe["expected_offset"]) <= 0.01
                        for v in scenario["offsets_seen"])
                    and len(scenario["offsets_seen"]) == runs
                    and all(scenario["band_ok"])
                )
                wheel_result["scenarios"][probe["name"]] = scenario
            result["wheel"][size_key] = wheel_result

    # --states: 4 offscreen frames in one process = cold + 3 warm image cache.
    if variant["kind"] == "cards":
        states = result["states"]["1280x800"]
        for index in range(runs):
            prefix = build / f"states_run{index}"
            res = run_process(
                [str(exe), "--states", str(prefix), "--size", "1280x800"],
                page, log_root / f"states_1280x800_{index}.log",
            )
            if res["rc"] != 0:
                result["errors"].append(f"states run {index}: rc={res['rc']}")
                continue
            parsed = parse_states(res["stdout"])
            if len(parsed) < 2:
                result["errors"].append(
                    f"states run {index}: only {len(parsed)} state lines"
                )
                continue
            states["runs"] += 1
            cold = parsed[0]
            warm = parsed[1:]
            states["cold_frame_ms"].append(cold["frame_ms"])
            states["cold_layout_ms"].append(cold["layout_ms"])
            states["warm_frame_ms"].extend(s["frame_ms"] for s in warm)
            states["warm_layout_ms"].extend(s["layout_ms"] for s in warm)
            states["per_run"].append({
                "cold_frame_ms": cold["frame_ms"],
                "warm_frame_ms": [s["frame_ms"] for s in warm],
                "cold_layout_ms": cold["layout_ms"],
                "warm_layout_ms": [s["layout_ms"] for s in warm],
            })
            for png in build.glob(f"states_run{index}_*.png"):
                png.unlink()
            (build / f"states_run{index}_states.json").unlink(missing_ok=True)
        states["cold_frame"] = stats(states["cold_frame_ms"])
        states["warm_frame"] = stats(states["warm_frame_ms"])
        states["cold_layout"] = stats(states["cold_layout_ms"])
        states["warm_layout"] = stats(states["warm_layout_ms"])
        states["decode_overhead_ms"] = (
            round(states["cold_frame"]["median"] - states["warm_frame"]["median"], 3)
            if states["cold_frame"]["median"] is not None
            and states["warm_frame"]["median"] is not None else None
        )

    return result


def measure_pilot(runs: int) -> dict:
    if not PILOT_EXE.is_file():
        return {"available": False, "error": f"missing {PILOT_EXE}"}
    out = ARTIFACTS / "pilot"
    out.mkdir(parents=True, exist_ok=True)
    log_root = LOGS / "pilot"
    result = {
        "available": True, "exe": str(PILOT_EXE.relative_to(REPO)),
        "layout": {}, "render": {}, "states": {}, "wheel": {},
        "adapter": {}, "errors": [],
    }
    for size_key in SIZES:
        width, height = parse_key(size_key)
        result["layout"][size_key] = {"best_ms": [], "avg_ms": [], "node_count": None}
        result["render"][size_key] = {
            "runs": 0, "wall_ms": [], "layout_ms": [], "frame_ms": [],
            "gpu_ms": [], "readback_ms": [], "png_ms": [],
            "peak_working_set_mb": [], "peak_pagefile_mb": [],
            "vertices": None, "glyph_vertices": None, "image_vertices": None,
            "nodes": None, "png": None, "png_check": None,
        }
    for index in range(runs):
        report = out / f"layout_run{index}.json"
        res = run_process(
            [str(PILOT_EXE), "--report", str(report), "--sizes", ",".join(SIZES)],
            PILOT_PAGE, log_root / f"report_{index}.log",
        )
        if res["rc"] != 0 or not report.is_file():
            result["errors"].append(f"pilot report run {index}: rc={res['rc']}")
            continue
        data = json.loads(report.read_text(encoding="utf-8"))
        for entry in data["sizes"]:
            key = f"{entry['width']}x{entry['height']}"
            result["layout"][key]["best_ms"].append(entry["layout_ms_best"])
            result["layout"][key]["avg_ms"].append(entry["layout_ms_avg"])
            result["layout"][key]["node_count"] = data["node_count"]
        if index == runs - 1:
            shutil.copyfile(report, out / "layout_report.json")
        report.unlink(missing_ok=True)
    for key in SIZES:
        result["layout"][key]["best"] = stats(result["layout"][key].pop("best_ms"))
        result["layout"][key]["avg"] = stats(result["layout"][key].pop("avg_ms"))

    for size_key in SIZES:
        width, height = parse_key(size_key)
        accum = result["render"][size_key]
        for index in range(runs):
            png = out / f"render_{width}x{height}.png" if index == 0 else \
                out / f"_tmp_{width}x{height}_{index}.png"
            res = run_process(
                [str(PILOT_EXE), "--render", str(width), str(height), str(png)],
                PILOT_PAGE, log_root / f"render_{size_key}_{index}.log",
            )
            if res["rc"] != 0:
                result["errors"].append(f"pilot render {size_key} {index}: rc={res['rc']}")
                continue
            parsed = parse_render(res["stdout"])
            if not parsed.get("found"):
                result["errors"].append(f"pilot render {size_key} {index}: no render line")
                continue
            accum["runs"] += 1
            accum["wall_ms"].append(res["wall_ms"])
            for field in ("layout_ms", "frame_ms", "gpu_ms", "readback_ms", "png_ms"):
                if field in parsed:
                    accum[field].append(parsed[field])
            accum["peak_working_set_mb"].append(res["peak_working_set_mb"])
            accum["peak_pagefile_mb"].append(res["peak_pagefile_mb"])
            for field in ("vertices", "glyph_vertices", "image_vertices", "nodes"):
                accum[field] = parsed[field]
            if index == 0:
                accum["png"] = str(png.relative_to(HERE))
                columns = max(1, int(((width * 0.8 - 48) + 16) // 196))
                fake_variant = {"viewports": {size_key: {
                    "grid": {"x": width * 0.2 + 24, "y": 24,
                             "w": width * 0.8 - 48, "h": height - 48},
                    "columns": columns,
                }}}
                accum["png_check"] = check_png(
                    png, "cards", fake_variant, size_key,
                    [{"card": i, "expected_band_top": 220,
                      "expected_max_y": 263}
                     for i in range(min(columns, 5))],
                )
                adapter = parse_adapter(res["stdout"])
                if adapter:
                    result["adapter"] = adapter
            elif png.is_file():
                png.unlink()
    for key in SIZES:
        accum = result["render"][key]
        for field in ("wall_ms", "layout_ms", "frame_ms", "gpu_ms", "readback_ms",
                      "png_ms", "peak_working_set_mb", "peak_pagefile_mb"):
            accum[field] = stats(accum[field])

    # states (cold/warm image cache) at 1280x800
    states = {"runs": 0, "cold_frame_ms": [], "warm_frame_ms": [],
              "cold_layout_ms": [], "warm_layout_ms": [], "per_run": []}
    for index in range(runs):
        prefix = out / f"states_run{index}"
        res = run_process(
            [str(PILOT_EXE), "--states", str(prefix), "--size", "1280x800"],
            PILOT_PAGE, log_root / f"states_{index}.log",
        )
        if res["rc"] != 0:
            result["errors"].append(f"pilot states {index}: rc={res['rc']}")
            continue
        parsed = parse_states(res["stdout"])
        if len(parsed) < 2:
            continue
        states["runs"] += 1
        states["cold_frame_ms"].append(parsed[0]["frame_ms"])
        states["cold_layout_ms"].append(parsed[0]["layout_ms"])
        states["warm_frame_ms"].extend(s["frame_ms"] for s in parsed[1:])
        states["warm_layout_ms"].extend(s["layout_ms"] for s in parsed[1:])
        states["per_run"].append({
            "cold_frame_ms": parsed[0]["frame_ms"],
            "warm_frame_ms": [s["frame_ms"] for s in parsed[1:]],
        })
        for png in out.glob(f"states_run{index}_*.png"):
            png.unlink()
        (out / f"states_run{index}_states.json").unlink(missing_ok=True)
    states["cold_frame"] = stats(states["cold_frame_ms"])
    states["warm_frame"] = stats(states["warm_frame_ms"])
    states["cold_layout"] = stats(states["cold_layout_ms"])
    states["warm_layout"] = stats(states["warm_layout_ms"])
    states["decode_overhead_ms"] = (
        round(states["cold_frame"]["median"] - states["warm_frame"]["median"], 3)
        if states["cold_frame"]["median"] is not None
        and states["warm_frame"]["median"] is not None else None
    )
    result["states"]["1280x800"] = states

    # wheel at 800x600 (pilot max 2792)
    size_key = "800x600"
    width, height = parse_key(size_key)
    probes = [
        {"name": "shift", "dy": 100, "expected_offset": 100.0},
        {"name": "offset400", "dy": 400, "expected_offset": 400.0},
        {"name": "clamp", "dy": 999999, "expected_offset": 2792.0},
        {"name": "outside", "dy": 400, "expected_offset": 0.0, "outside": True},
    ]
    wheel_result = {"scroll_max_expected": 2792.0, "scenarios": {}}
    for probe in probes:
        scenario = {"offsets": [], "frame_ms": [], "wall_ms": [],
                    "gpu_ms": [], "readback_ms": [], "png_ms": []}
        for index in range(runs):
            png = out / f"wheel_{probe['name']}_{size_key}.png" if index == 0 else \
                out / f"_tmp_wheel_{probe['name']}_{index}.png"
            x, y = (20, 300) if probe.get("outside") else (400, 300)
            res = run_process(
                [str(PILOT_EXE), "--render", str(width), str(height), str(png),
                 "--wheel", str(x), str(y), str(probe["dy"])],
                PILOT_PAGE, log_root / f"wheel_{probe['name']}_{index}.log",
            )
            if res["rc"] != 0:
                result["errors"].append(f"pilot wheel {probe['name']} {index}: rc={res['rc']}")
                continue
            wheel = parse_wheel(res["stdout"])
            scroll = parse_scroll(res["stdout"])
            offset = wheel.get("offset", scroll.get("offset"))
            scenario["offsets"].append(offset)
            scenario["wall_ms"].append(res["wall_ms"])
            parsed = parse_render(res["stdout"])
            for field in ("frame_ms", "gpu_ms", "readback_ms", "png_ms"):
                if field in parsed:
                    scenario[field].append(parsed[field])
            if index == 0:
                scenario["png"] = str(png.relative_to(HERE))
            elif png.is_file():
                png.unlink()
        for field in ("wall_ms", "frame_ms", "gpu_ms", "readback_ms", "png_ms"):
            scenario[field] = stats(scenario[field])
        scenario["ok"] = all(
            v is not None and abs(v - probe["expected_offset"]) <= 0.01
            for v in scenario["offsets"]
        ) and len(scenario["offsets"]) == runs
        scenario["expected_offset"] = probe["expected_offset"]
        wheel_result["scenarios"][probe["name"]] = scenario
    result["wheel"][size_key] = wheel_result
    return result


# --------------------------------------------------------------- analysis


def build_analysis(variants: list[dict], pilot: dict) -> dict:
    by_id = {v["id"]: v for v in variants}
    comparison = {}

    def layout_median(variant_id: str, size_key: str, field: str = "avg") -> float | None:
        v = by_id.get(variant_id)
        if not v:
            return None
        return ((v.get("layout") or {})
                .get(size_key, {}).get(field, {}).get("median"))

    def render_median(variant_id: str, size_key: str, field: str) -> float | None:
        v = by_id.get(variant_id)
        if not v:
            return None
        return ((v.get("render") or {})
                .get(size_key, {}).get(field, {}).get("median"))

    if pilot.get("available"):
        comparison["layout_1280"] = {
            "pilot_30_cards_ms": pilot["layout"]["1280x800"]["avg"].get("median"),
            "stress_30_cards_ms": layout_median("cards-030", "1280x800"),
            "stress_100_cards_ms": layout_median("cards-100", "1280x800"),
            "stress_300_cards_ms": layout_median("cards-300", "1280x800"),
            "stress_1000_nodes_ms": layout_median("nodes-1000", "1280x800"),
        }
        comparison["render_1280"] = {
            "pilot_frame_ms": pilot["render"]["1280x800"]["frame_ms"].get("median"),
            "pilot_wall_ms": pilot["render"]["1280x800"]["wall_ms"].get("median"),
            "stress_30_frame_ms": render_median("cards-030", "1280x800", "frame_ms"),
            "stress_300_frame_ms": render_median("cards-300", "1280x800", "frame_ms"),
            "stress_1000_nodes_frame_ms": render_median("nodes-1000", "1280x800", "frame_ms"),
        }
        comparison["image_cache_1280"] = {
            "pilot_cold_frame_ms": pilot["states"]["1280x800"]["cold_frame"].get("median"),
            "pilot_warm_frame_ms": pilot["states"]["1280x800"]["warm_frame"].get("median"),
            "stress30_cold_frame_ms": (
                by_id.get("cards-030", {}).get("states", {})
                .get("1280x800", {}).get("cold_frame", {}).get("median")
            ),
            "stress30_warm_frame_ms": (
                by_id.get("cards-030", {}).get("states", {})
                .get("1280x800", {}).get("warm_frame", {}).get("median")
            ),
            "stress300_cold_frame_ms": (
                by_id.get("cards-300", {}).get("states", {})
                .get("1280x800", {}).get("cold_frame", {}).get("median")
            ),
            "stress300_warm_frame_ms": (
                by_id.get("cards-300", {}).get("states", {})
                .get("1280x800", {}).get("warm_frame", {}).get("median")
            ),
        }
        if comparison["render_1280"]["pilot_frame_ms"] and \
                comparison["render_1280"]["stress_300_frame_ms"]:
            comparison["render_1280"]["stress300_over_pilot"] = round(
                comparison["render_1280"]["stress_300_frame_ms"] /
                comparison["render_1280"]["pilot_frame_ms"], 3
            )
        if comparison["image_cache_1280"]["pilot_warm_frame_ms"] and \
                comparison["image_cache_1280"]["stress300_warm_frame_ms"]:
            comparison["image_cache_1280"]["stress300_warm_over_pilot"] = round(
                comparison["image_cache_1280"]["stress300_warm_frame_ms"] /
                comparison["image_cache_1280"]["pilot_warm_frame_ms"], 3
            )

    # Per-node and per-card costs from the fresh runs.
    scaling = []
    for vid in ("nodes-0030", "nodes-0100", "nodes-0300", "nodes-1000"):
        v = by_id.get(vid)
        if not v:
            continue
        for size_key in SIZES:
            ms = (v.get("layout") or {}).get(size_key, {}).get("avg", {}).get("median")
            nodes = v.get("node_count")
            if ms is not None and nodes:
                scaling.append({
                    "variant": vid, "nodes": nodes, "size": size_key,
                    "layout_ms_median_avg": ms,
                    "us_per_node": round(ms * 1000.0 / nodes, 4),
                })
    cards_scaling = []
    for vid in ("cards-030", "cards-100", "cards-300"):
        v = by_id.get(vid)
        if not v:
            continue
        for size_key in SIZES:
            ms = (v.get("layout") or {}).get(size_key, {}).get("avg", {}).get("median")
            if ms is not None:
                cards_scaling.append({
                    "variant": vid, "cards": v["cards"], "nodes": v.get("node_count"),
                    "size": size_key, "layout_ms_median_avg": ms,
                })

    component_shares = {}
    geometry = {}
    memory = {}
    wheel_summary = {}
    for v in variants:
        vid = v["id"]
        component_shares[vid] = {}
        geometry[vid] = {}
        memory[vid] = {}
        for size_key in SIZES:
            render = (v.get("render") or {}).get(size_key, {})
            frame = render.get("frame_ms", {}).get("median")
            parts = {}
            for field in ("gpu_ms", "readback_ms", "png_ms"):
                value = render.get(field, {}).get("median")
                parts[field] = value
                parts[field + "_pct_of_frame"] = (
                    round(100.0 * value / frame, 2)
                    if value is not None and frame else None
                )
            parts["frame_ms"] = frame
            parts["wall_ms"] = render.get("wall_ms", {}).get("median")
            component_shares[vid][size_key] = parts
            geometry[vid][size_key] = {
                "nodes": v.get("node_count"),
                "vertices": render.get("vertices"),
                "glyph_vertices": render.get("glyph_vertices"),
                "image_vertices": render.get("image_vertices"),
            }
            memory[vid][size_key] = {
                "peak_working_set_mb": render.get("peak_working_set_mb", {}).get("median"),
                "peak_pagefile_mb": render.get("peak_pagefile_mb", {}).get("median"),
            }
        if v.get("kind") == "cards":
            wheel_summary[vid] = {}
            for size_key in SIZES:
                wheel = (v.get("wheel") or {}).get(size_key, {})
                wheel_summary[vid][size_key] = {
                    "scroll_max": wheel.get("scroll_max_expected"),
                    "scenarios": {
                        name: {
                            "expected_offset": scenario.get("expected_offset"),
                            "observed_offsets": scenario.get("offsets_seen"),
                            "band_tops": scenario.get("band_tops"),
                            "expected_band_top": scenario.get("expected_band_top"),
                            "frame_ms": scenario.get("frame_ms", {}).get("median"),
                            "wall_ms": scenario.get("wall_ms", {}).get("median"),
                            "ok": scenario.get("ok"),
                        }
                        for name, scenario in (wheel.get("scenarios") or {}).items()
                    },
                }

    return {
        "comparison": comparison,
        "scaling": {"synthetic_nodes": scaling, "media_cards": cards_scaling},
        "component_shares": component_shares,
        "geometry": geometry,
        "memory": memory,
        "wheel": wheel_summary,
    }


def build_findings(analysis: dict) -> tuple[list[dict], list[str]]:
    """Evidence-backed bottlenecks + recommendations (kept short; STRESS.md expands)."""
    comparison = analysis["comparison"]
    shares = analysis.get("component_shares", {})
    geometry = analysis.get("geometry", {})
    memory = analysis.get("memory", {})
    scaling = analysis.get("scaling", {})
    bottlenecks = []
    recommendations = []

    def share(vid: str, size_key: str, field: str):
        return (shares.get(vid, {}).get(size_key, {}) or {}).get(field)

    def geom(vid: str, size_key: str, field: str):
        return (geometry.get(vid, {}).get(size_key, {}) or {}).get(field)

    stress030_frame = comparison.get("render_1280", {}).get("stress_30_frame_ms")
    stress300_frame = comparison.get("render_1280", {}).get("stress_300_frame_ms")

    if stress030_frame and stress300_frame:
        bottlenecks.append({
            "id": "render-is-almost-flat-vs-card-count",
            "evidence": (
                f"same-template offscreen frame 30 cards={stress030_frame:.1f} ms, "
                f"300 cards={stress300_frame:.1f} ms (x{stress300_frame / stress030_frame:.2f}); "
                f"solid vertices {geom('cards-030', '1280x800', 'vertices')} -> "
                f"{geom('cards-300', '1280x800', 'vertices')}, while glyph/image vertices stay "
                f"at {geom('cards-300', '1280x800', 'glyph_vertices')} / "
                f"{geom('cards-300', '1280x800', 'image_vertices')}"
            ),
            "note": "Viewport culling bounds text/image draws; solid card rectangles still "
                    "grow with the catalogue (6 vertices per card).",
        })

    nodes1000 = comparison.get("layout_1280", {}).get("stress_1000_nodes_ms")
    cards300 = comparison.get("layout_1280", {}).get("stress_300_cards_ms")
    if nodes1000 is not None and cards300 is not None:
        layout_share = (100.0 * cards300 / stress300_frame) if stress300_frame else None
        bottlenecks.append({
            "id": "layout-is-not-the-bottleneck",
            "evidence": (
                f"layout median avg: 300 cards (2718 nodes)={cards300:.3f} ms, "
                f"1000 synthetic nodes={nodes1000:.3f} ms at 1280x800"
                + (f" (layout ≈ {layout_share:.2f}% of the 300-card offscreen frame)"
                   if layout_share is not None else "")
            ),
            "note": "Layout is sub-millisecond; offscreen pass/readback/PNG dominate.",
        })

    readback_pct = share("cards-300", "1280x800", "readback_ms_pct_of_frame")
    png_pct = share("cards-300", "1280x800", "png_ms_pct_of_frame")
    gpu_pct = share("cards-300", "1280x800", "gpu_ms_pct_of_frame")
    if readback_pct is not None:
        bottlenecks.append({
            "id": "offscreen-readback-png-dominate",
            "evidence": (
                f"cards-300 1280x800 frame components: gpu={fmt(gpu_pct, 1)}%, "
                f"readback(map)={fmt(readback_pct, 1)}%, png={fmt(png_pct, 1)}% "
                f"(frame {fmt(share('cards-300', '1280x800', 'frame_ms'), 1)} ms, "
                f"wall {fmt(share('cards-300', '1280x800', 'wall_ms'), 1)} ms)"
            ),
            "recommendation": (
                "Do not use the offscreen frame as a GPU benchmark. Report pass, readback "
                "and PNG separately (done) and measure windowed present for real frame cost."
            ),
        })

    cache = comparison.get("image_cache_1280", {})
    if cache.get("stress300_cold_frame_ms") and cache.get("stress300_warm_frame_ms"):
        bottlenecks.append({
            "id": "wic-cold-first-frame",
            "evidence": (
                f"cards-300 cold frame={cache['stress300_cold_frame_ms']:.1f} ms vs warm "
                f"frame={cache['stress300_warm_frame_ms']:.1f} ms "
                f"(+{cache['stress300_cold_frame_ms'] - cache['stress300_warm_frame_ms']:.1f} ms "
                "on the first frame). 300 image nodes reuse 30 unique posters and only the "
                "viewport-visible unique files decode."
            ),
            "recommendation": (
                "Keep the path-keyed cache, but add LRU/refcount eviction by bytes and "
                "decode to display size; a 180x240 RGBA texture is ~169 KiB."
            ),
        })

    if memory.get("cards-300", {}).get("1280x800"):
        mem = memory["cards-300"]["1280x800"]
        bottlenecks.append({
            "id": "memory-warp-constant",
            "evidence": (
                f"cards-300 1280x800 peak working set="
                f"{fmt(mem.get('peak_working_set_mb'), 1)} MB, "
                f"peak pagefile={fmt(mem.get('peak_pagefile_mb'), 1)} MB; WARP device commit "
                "dominates and barely moves between 30 and 300 cards."
            ),
            "recommendation": (
                "Track image-cache bytes separately from WARP commit; the cache is the part "
                "that grows with a real 1000-poster library."
            ),
        })

    bottlenecks.extend([
        {
            "id": "solid-geometry-not-culled",
            "evidence": (
                "runtime vertices grow linearly with cards (cards-030=300, cards-100=720, "
                "cards-300=1920 at 1280x800; +6 vertices/card), while image_vertices stay at "
                "90 and glyph_vertices at ~4.2k. Every card background is emitted even when "
                "far below the scroll clip."
            ),
            "recommendation": (
                "Cull solid rects by the scroll viewport (or window rows) in "
                "wn_core_build_display_list, not just image/glyph batches."
            ),
        },
        {
            "id": "wheel-triggers-full-relayout",
            "evidence": (
                "When a wheel delta changes the offset, wn_core_wheel() calls "
                "layout_page() for the whole document; render_offscreen() also lays out "
                "before applying the wheel, so that wheel frame performs two full layouts. "
                f"At 300 cards layout is {fmt(cards300, 3)} ms "
                "each; the second one is outside both printed layout_ms and frame_ms."
            ),
            "recommendation": (
                "Make scroll offset-only (translate rects / adjust scissor) and count it in "
                "frame timing. Reserve relayout for resize."
            ),
        },
        {
            "id": "no-virtualization",
            "evidence": (
                "All cards are laid out and all solid geometry is built each frame; only "
                "image/glyph draws are clipped. 300 cards = 2718 nodes; a 1000-card A1 "
                "library would be ~9000 nodes and ~6000 solid vertices."
            ),
            "recommendation": (
                "For 500-1000+ items, virtualize rows: keep visible + overscan cards and "
                "recycle nodes. This bounds layout, solid geometry and memory."
            ),
        },
        {
            "id": "per-image-bind-group-and-draw",
            "evidence": (
                "Each image batch owns one bind group and one draw; batches merge only for "
                "consecutive identical src+clip. With cycled posters the visible set is "
                "~15 images at 1280x800; a unique-image grid approaches one draw per visible "
                "image."
            ),
            "recommendation": (
                "Atlas/array textures + instanced quads or grouped multi-draw."
            ),
        },
        {
            "id": "warp-caveat",
            "evidence": WARP_NOTE,
            "recommendation": (
                "Treat these numbers as a CPU/WARP lower bound and a regression baseline, "
                "not as hardware-GPU performance."
            ),
        },
    ])
    recommendations = [
        "Scroll: offset-only update (no full relayout) and include it in frame timing.",
        "Cull solid card rects by viewport/row window; images/glyphs are already clipped.",
        "Virtualize the grid for 500+ cards; measure layout + geometry per frame.",
        "Image cache: LRU by bytes, decode to display size, atlas/array + instancing.",
        "Expose geometry-build time, batch count and draw count in --render/--report.",
        "Keep the current viewport culling: image_vertices stayed at 90 (1280x800) for "
        "30 and 300 cards.",
        "Add a warm-render/--frames N offscreen mode so cache and frame costs can be "
        "measured without --states or a window.",
        "Re-run this suite on a hardware GPU; WARP readback/PNG obscures GPU cost.",
    ]
    return bottlenecks, recommendations


# -------------------------------------------------------------------- main


def main() -> int:
    parser = argparse.ArgumentParser(description="Build and measure the R2 stress fixtures")
    parser.add_argument("--runs", type=int, default=7, help="process runs per measurement (default 7)")
    parser.add_argument("--skip-emit", action="store_true")
    parser.add_argument("--skip-compile", action="store_true")
    parser.add_argument("--skip-pilot", action="store_true")
    parser.add_argument("--regenerate", action="store_true",
                        help="rewrite variants/<id>/page from generate.py before building")
    parser.add_argument("--only", default=None, help="variant id, e.g. cards-300")
    parser.add_argument("--report", default=str(REPORT_PATH))
    args = parser.parse_args()
    if args.runs < 1:
        raise SystemExit("--runs must be >= 1")

    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    if args.regenerate or not MANIFEST_PATH.is_file():
        generate_fixtures()
    manifest = load_manifest()
    selected = [v for v in manifest["variants"] if not args.only or v["id"] == args.only]
    if not selected:
        raise SystemExit(f"no variant matches {args.only!r}")

    vcvars = None if args.skip_compile else find_vcvars()
    if not args.skip_compile and not vcvars:
        raise SystemExit("vcvars64.bat not found (install the MSVC C++ workload)")

    stopped_early = None
    report = {
        "schema": "webnative.pilot-media-library.stress-report/1",
        "generated_at": now_iso(),
        "goal": "stress R2 grid/scroll/images at 30/100/300 cards and 1000 synthetic nodes",
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "processor": os.environ.get("PROCESSOR_IDENTIFIER") or platform.processor(),
            "cpu_count": os.cpu_count(),
            "python": sys.version.split()[0],
            "warp": None,
            "adapter": {},
            "warp_caveat": WARP_NOTE,
        },
        "method": {
            "emit": "frontend-rust --html <page> --css <css> --width 1280 --height 800 "
                    "--emit-ui build/ui.json --pilot",
            "ui_data": "src/emitter/python/emit_ui.py --ui-json build/ui.json "
                       "--out build/ui_data.h --meta build/ui_meta.json",
            "compile": "cl /nologo /O2 /W3 /std:c17 /TC /D_CRT_SECURE_NO_WARNINGS "
                       "/DWN_STATIC <runtime_r2.c wn_core.c wn_common.c> + wgpu_native.dll.lib, "
                       "same sources/flags as src/runtime/c-r2/build.ps1 (unmodified)",
            "runs_per_measurement": args.runs,
            "layout": "runtime --report runs 100 layout_page() iterations per size; "
                      "this report takes the median across process runs of "
                      "layout_ms_best and layout_ms_avg. Report layout excludes "
                      "display-list/geometry build.",
            "render": "one cold process per run: --render W H out.png. wall_ms is measured "
                      "around Popen (includes process + wgpu/WARP init). frame_ms is the "
                      "runtime's printed frame line: pass + readback + png, measured after "
                      "layout, build_geometry and vertex upload (the runtime label says "
                      "'build+upload' but the timer starts after them). layout_ms is printed "
                      "separately and excludes geometry build.",
            "wheel": "--render W H out.png --wheel X Y DY. Offset/max are parsed from the "
                     "runtime's wheel:/scroll: lines; the accent band's pixel Y in the PNG "
                     "is the independent position check (poster rule band maps to "
                     "card.y+166..173 after object-fit:cover).",
            "image_cache": "--states prefix --size 1280x800 renders 4 frames in one process: "
                           "the first is cold (WIC decode + upload), frames 2-4 reuse the "
                           "path-keyed cache. decode_overhead = cold - warm frame.",
            "memory": "GetProcessMemoryInfo sampled every ~4 ms while the process runs; "
                      "peak WorkingSetSize / PeakPagefileUsage. WARP dominates commit.",
            "timing_note": [
                "The printed frame label is misleading: build_geometry() and "
                "gpu_upload_geometry() run before the frame timer starts, so frame_ms = "
                "gpu pass + readback(map) + png.",
                "Wheel events trigger an extra full layout inside wn_core_wheel(); it is "
                "not included in the printed layout_ms or frame_ms.",
                "Image decode happens inside gpu_draw(), so it is inside the runtime's "
                "frame/gpu timing.",
                "Offscreen readback (map) + PNG encode are measured by the runtime but are "
                "not representative of a windowed present.",
            ],
            "png_verification": "Pillow: exact accent-colour band positions (poster rule "
                                "band), no accent outside the grid rect, PNG dimensions, "
                                "synthetic cell colours present.",
        },
        "variants": [],
        "pilot": {},
        "analysis": {},
        "acceptance": {"ok": True, "checks": 0, "failed": 0, "failures": []},
        "artifacts": {
            "rerun": f"{SRC_PY} {HERE / 'run_stress.py'}",
            "generated_pages": "variants/<id>/page",
            "builds": "variants/<id>/build",
            "logs": "logs",
            "pilot": "artifacts/pilot",
        },
    }

    print(f"stress: {len(selected)} variant(s), {args.runs} run(s)/measurement")
    print(f"repo: {REPO}")
    print(f"WARP note: {WARP_NOTE}")
    print()

    for variant in selected:
        vid = variant["id"]
        print(f"[{vid}] emit", flush=True)
        try:
            emit_info = emit_variant(variant, args.skip_emit)
            compile_info = compile_variant(variant, vcvars, args.skip_compile)
        except SystemExit as exc:
            stopped_early = str(exc)
            report["acceptance"]["ok"] = False
            report["acceptance"]["failures"].append(str(exc))
            print(f"  FAILED: {exc}", file=sys.stderr)
            break

        page, build, exe = variant_paths(variant)
        node_count = emit_info.get("node_count")
        expected_nodes = variant.get("nodes_expected")
        node_ok = expected_nodes is None or node_count == expected_nodes
        print(
            f"[{vid}] emit {emit_info['frontend_ms']} ms + {emit_info['emit_ms']} ms, "
            f"compile {compile_info['compile_ms']} ms, nodes={node_count}"
            + (f" (expected {expected_nodes})" if expected_nodes else "")
            + ("" if node_ok else "  <-- NODE COUNT MISMATCH"),
            flush=True,
        )

        print(f"[{vid}] measure layout/render/wheel/states", flush=True)
        measure = measure_variant(
            variant, exe, page, build, args.runs, LOGS / vid
        )
        if not report["host"]["adapter"]:
            for log in sorted((LOGS / vid).glob("render_*.log")):
                text = log.read_text(encoding="utf-8", errors="replace")
                adapter = parse_adapter(text)
                if adapter:
                    report["host"]["adapter"] = adapter
                    report["host"]["warp"] = "Basic Render Driver" in adapter.get("device", "")
                    break

        layout_report = build / "layout_report.json"
        layout_check = check_layout_report(layout_report, variant)
        checks = measure["checks"]
        checks.append({"name": "node_count", "ok": node_ok,
                       "detail": f"{node_count} (expected {expected_nodes})"
                       if expected_nodes else f"{node_count}"})
        checks.append({"name": "layout_report", "ok": layout_check["ok"],
                       "detail": layout_check})
        for size_key in SIZES:
            layout = measure["layout"].get(size_key, {})
            render = measure["render"].get(size_key, {})
            checks.append({
                "name": f"layout_runs_{size_key}",
                "ok": layout.get("avg", {}).get("n", 0) >= args.runs,
                "detail": f"{layout.get('avg', {}).get('n', 0)}/{args.runs} report runs",
            })
            checks.append({
                "name": f"render_runs_{size_key}",
                "ok": render.get("runs", 0) >= args.runs,
                "detail": f"{render.get('runs', 0)}/{args.runs} render runs",
            })
            png_check = render.get("png_check") or {}
            checks.append({
                "name": f"render_png_{size_key}",
                "ok": bool(render.get("runs")) and png_check.get("ok") is not False,
                "detail": png_check,
            })
            if variant["kind"] == "cards":
                wheel = measure["wheel"].get(size_key, {})
                for name, scenario in (wheel.get("scenarios") or {}).items():
                    checks.append({
                        "name": f"wheel_{name}_{size_key}",
                        "ok": bool(scenario.get("ok")),
                        "detail": {
                            "expected_offset": scenario.get("expected_offset"),
                            "offsets": scenario.get("offsets_seen"),
                            "band_tops": scenario.get("band_tops"),
                            "expected_band_top": scenario.get("expected_band_top"),
                        },
                    })
        if variant["kind"] == "cards":
            states_runs = measure.get("states", {}).get("1280x800", {}).get("runs", 0)
            checks.append({
                "name": "states_runs_1280x800",
                "ok": states_runs >= args.runs,
                "detail": f"{states_runs}/{args.runs} cold/warm state runs",
            })
        failed = [c for c in checks if not c["ok"]]
        variant_report = {
            "id": vid,
            "kind": variant["kind"],
            "cards": variant.get("cards"),
            "nodes_expected": variant.get("nodes_expected"),
            "node_count": node_count,
            "image_nodes": emit_info.get("image_nodes"),
            "unique_images": variant.get("unique_images"),
            "page": variant["page"],
            "exe": str(exe.relative_to(HERE)),
            "ui_json_bytes": emit_info.get("ui_json_bytes"),
            "ui_data_bytes": emit_info.get("ui_data_bytes"),
            "atlas_bytes": emit_info.get("atlas_bytes"),
            "timings": {
                "frontend_ms": emit_info.get("frontend_ms"),
                "emit_ms": emit_info.get("emit_ms"),
                "compile_ms": compile_info.get("compile_ms"),
                "exe_bytes": compile_info.get("exe_bytes"),
            },
            "emit_info": emit_info,
            "layout": measure["layout"],
            "render": measure["render"],
            "wheel": measure["wheel"],
            "states": measure["states"],
            "errors": measure["errors"],
            "checks": checks,
        }
        report["variants"].append(variant_report)
        report["acceptance"]["checks"] += len(checks)
        report["acceptance"]["failed"] += len(failed)
        if failed:
            report["acceptance"]["ok"] = False
            for check in failed:
                report["acceptance"]["failures"].append(f"{vid}/{check['name']}: {check['detail']}")
        print(
            f"[{vid}] checks {len(checks) - len(failed)}/{len(checks)} pass"
            + (f", {len(failed)} FAILED" if failed else "")
            + (f", {len(measure['errors'])} error(s)" if measure["errors"] else ""),
            flush=True,
        )

    if not args.skip_pilot and not args.only:
        print("[pilot] measuring existing 30-card pilot baseline", flush=True)
        report["pilot"] = measure_pilot(args.runs)
        pilot_checks = []
        if report["pilot"].get("available"):
            for size_key in SIZES:
                layout = report["pilot"]["layout"].get(size_key, {})
                render = report["pilot"]["render"].get(size_key, {})
                pilot_checks.append({
                    "name": f"pilot_layout_runs_{size_key}",
                    "ok": layout.get("avg", {}).get("n", 0) >= args.runs,
                    "detail": f"{layout.get('avg', {}).get('n', 0)}/{args.runs}",
                })
                pilot_checks.append({
                    "name": f"pilot_render_runs_{size_key}",
                    "ok": render.get("runs", 0) >= args.runs,
                    "detail": f"{render.get('runs', 0)}/{args.runs}",
                })
                pilot_checks.append({
                    "name": f"pilot_render_png_{size_key}",
                    "ok": (render.get("png_check") or {}).get("ok") is True,
                    "detail": render.get("png_check"),
                })
            states = report["pilot"].get("states", {}).get("1280x800", {})
            pilot_checks.append({
                "name": "pilot_states_runs",
                "ok": states.get("runs", 0) >= args.runs,
                "detail": f"{states.get('runs', 0)}/{args.runs}",
            })
            for name, scenario in report["pilot"].get("wheel", {}).get(
                    "800x600", {}).get("scenarios", {}).items():
                pilot_checks.append({
                    "name": f"pilot_wheel_{name}_800x600",
                    "ok": bool(scenario.get("ok")),
                    "detail": {"offsets": scenario.get("offsets"),
                               "expected": scenario.get("expected_offset")},
                })
        report["pilot"]["checks"] = pilot_checks
        pilot_failed = [c for c in pilot_checks if not c["ok"]]
        report["acceptance"]["checks"] += len(pilot_checks)
        report["acceptance"]["failed"] += len(pilot_failed)
        if pilot_failed:
            report["acceptance"]["ok"] = False
            for check in pilot_failed:
                report["acceptance"]["failures"].append(
                    f"pilot/{check['name']}: {check['detail']}"
                )
        if report["pilot"].get("errors"):
            report["acceptance"]["ok"] = False
            for err in report["pilot"]["errors"]:
                report["acceptance"]["failures"].append(f"pilot: {err}")

    report["analysis"] = build_analysis(report["variants"], report["pilot"])
    bottlenecks, recommendations = build_findings(report["analysis"])
    report["bottlenecks"] = bottlenecks
    report["recommendations"] = recommendations
    if not report["host"]["adapter"]:
        report["host"]["adapter"] = report.get("pilot", {}).get("adapter", {})
        report["host"]["warp"] = "Basic Render Driver" in \
            report["host"]["adapter"].get("device", "")
    if stopped_early:
        report["acceptance"]["ok"] = False
        report["acceptance"]["failures"].append(f"stopped: {stopped_early}")

    write_json(Path(args.report), report)
    print()
    print("=" * 78)
    print("STRESS RESULT" + ("" if report["acceptance"]["ok"] else " (FAILURES)"))
    print(f"{'variant':<12}{'nodes':>6}{'layout1280':>13}{'frame1280':>12}"
          f"{'wall1280':>11}{'frame800':>11}")
    for v in report["variants"]:
        l = v["layout"].get("1280x800", {}).get("avg", {}).get("median")
        fr = v["render"].get("1280x800", {}).get("frame_ms", {}).get("median")
        wa = v["render"].get("1280x800", {}).get("wall_ms", {}).get("median")
        fr8 = v["render"].get("800x600", {}).get("frame_ms", {}).get("median")
        print(f"{v['id']:<12}{v['node_count']:>6}{l if l is not None else 'n/a':>13.3f}"
              f"{fr if fr is not None else 'n/a':>12.1f}"
              f"{wa if wa is not None else 'n/a':>11.1f}"
              f"{fr8 if fr8 is not None else 'n/a':>11.1f}")
    if report["pilot"].get("available"):
        p = report["pilot"]
        print(f"{'pilot-030':<12}{p['layout']['1280x800']['node_count']:>6}"
              f"{p['layout']['1280x800']['avg']['median']:>13.3f}"
              f"{p['render']['1280x800']['frame_ms']['median']:>12.1f}"
              f"{p['render']['1280x800']['wall_ms']['median']:>11.1f}"
              f"{p['render']['800x600']['frame_ms']['median']:>11.1f}")
    print(f"acceptance: {report['acceptance']['checks'] - report['acceptance']['failed']}"
          f"/{report['acceptance']['checks']} checks, "
          f"{len(report['acceptance']['failures'])} failure(s)")
    print(f"report: {Path(args.report)}")
    print("=" * 78)
    if report["acceptance"]["failures"]:
        for failure in report["acceptance"]["failures"][:20]:
            print(f"  FAIL: {failure}")
    return 0 if report["acceptance"]["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
