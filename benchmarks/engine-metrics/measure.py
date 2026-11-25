#!/usr/bin/env python3
"""engine-metrics harness for webnative (R2 dynamic runtime).

Measures, for N runs each, and writes baseline.json + BASELINE.md:

  * install size   .exe + wgpu_native.dll + external assets (atlas/fonts)
  * engine size    .exe only
  * startup        runtime-reported init (process -> device/pipelines ready)
                   plus external wall of `--frames 1` as an upper bound
  * RSS            peak working set (Win32 GetProcessMemoryInfo, psutil cross-check)
  * CPU frame      R2 `window: cpu frame ...` (layout/present excluded)
                   and wall of `--frames N`
  * layout         R2 `--report` (960/720/560, 28 nodes, 100 iterations)
  * offscreen      wall of `--render` plus the runtime's internal timings
  * backend        D3D12/WARP/hardware, from the runtime's adapter lines

It never modifies the runtime: it runs a frozen snapshot of R2
(`snapshot/runtime_r2.exe` + DLL).

Usage:
    python measure.py --runs 10
    python measure.py --runs 3 --out results/probe.json --md results/probe.md

The one-command entry point is `wn-cli bench metrics`, which runs this script
with the repository virtualenv.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
SNAPSHOT_DIR = HERE / "snapshot"
WORK_DIR = HERE / "work"
RESULTS_DIR = HERE / "results"
FRAMES_WINDOW = 30
LAYOUT_SIZES = "960x640,720x640,560x640"
OFFSCREEN_SIZES = ((960, 640), (720, 640), (560, 640))

BUDGET_INSTALL_MB = 15.0
BUDGET_ENGINE_MB = 1.0
BUDGET_STARTUP_MS = 150.0
BUDGET_RSS_MB = 30.0
BUDGET_CPU_FRAME_MS = 2.0
BUDGET_LAYOUT_MS = 0.5


# --------------------------------------------------------------- Win32 / RSS

class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
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
        ("PrivateUsage", ctypes.c_size_t),
    ]


_psapi = None
if os.name == "nt":
    _psapi = ctypes.WinDLL("psapi", use_last_error=True)
    _psapi.GetProcessMemoryInfo.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(PROCESS_MEMORY_COUNTERS_EX),
        wintypes.DWORD,
    ]
    _psapi.GetProcessMemoryInfo.restype = wintypes.BOOL

try:
    import psutil  # local venv; optional fallback
    PSUTIL_VERSION = psutil.__version__
except Exception:  # pragma: no cover - only when the venv is broken
    psutil = None
    PSUTIL_VERSION = None


def win32_peak_wset(handle: int) -> int | None:
    """PeakWorkingSetSize of an open process handle (bytes), or None."""
    if _psapi is None or not handle:
        return None
    counters = PROCESS_MEMORY_COUNTERS_EX()
    counters.cb = ctypes.sizeof(counters)
    ok = _psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb)
    if not ok:
        return None
    return int(counters.PeakWorkingSetSize)


# ------------------------------------------------------------------ process

def run_measured(exe: Path, args: list[str], timeout_s: float = 120.0) -> dict:
    """Run exe to completion, sampling peak working set; return wall + output."""
    exe = exe.resolve()
    if not exe.exists():
        raise FileNotFoundError(f"runtime not found: {exe}")

    WORK_DIR.mkdir(parents=True, exist_ok=True)
    out_fd, out_name = tempfile.mkstemp(prefix="wn-out-", suffix=".txt", dir=WORK_DIR)
    err_fd, err_name = tempfile.mkstemp(prefix="wn-err-", suffix=".txt", dir=WORK_DIR)
    env = dict(os.environ)
    # Same isolation the acceptance scripts use: System32 + runtime directory.
    env["PATH"] = f"{os.environ.get('WINDIR', r'C:\Windows')}\\System32;{exe.parent}"

    peak_win32 = 0
    peak_psutil = 0
    peak_pagefile = 0
    rss_samples = 0
    timed_out = False
    started = time.perf_counter()
    proc = None
    try:
        with os.fdopen(out_fd, "w", encoding="utf-8", errors="replace") as out_f, \
             os.fdopen(err_fd, "w", encoding="utf-8", errors="replace") as err_f:
            proc = subprocess.Popen(
                [str(exe), *args],
                cwd=str(exe.parent),
                env=env,
                stdout=out_f,
                stderr=err_f,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
            psinfo = None
            if psutil is not None:
                try:
                    psinfo = psutil.Process(proc.pid)
                except Exception:
                    psinfo = None

            deadline = started + timeout_s
            while proc.poll() is None:
                if time.perf_counter() > deadline:
                    timed_out = True
                    proc.kill()
                    break
                handle = int(getattr(proc, "_handle", 0) or 0)
                value = win32_peak_wset(handle)
                if value:
                    peak_win32 = max(peak_win32, value)
                if psinfo is not None:
                    try:
                        info = psinfo.memory_info()
                        p = int(getattr(info, "peak_wset", 0) or 0)
                        if p:
                            peak_psutil = max(peak_psutil, p)
                        pf = int(getattr(info, "peak_pagefile", 0) or 0)
                        if pf:
                            peak_pagefile = max(peak_pagefile, pf)
                        rss_samples += 1
                    except Exception:
                        psinfo = None
                time.sleep(0.002)
            proc.wait(timeout=30)
            # One final read while the handle is still open (process object may
            # still expose the final counters after termination).
            handle = int(getattr(proc, "_handle", 0) or 0)
            value = win32_peak_wset(handle)
            if value:
                peak_win32 = max(peak_win32, value)
            if psinfo is not None:
                try:
                    info = psinfo.memory_info()
                    p = int(getattr(info, "peak_wset", 0) or 0)
                    if p:
                        peak_psutil = max(peak_psutil, p)
                    pf = int(getattr(info, "peak_pagefile", 0) or 0)
                    if pf:
                        peak_pagefile = max(peak_pagefile, pf)
                except Exception:
                    pass
        wall_ms = (time.perf_counter() - started) * 1000.0
        stdout = Path(out_name).read_text(encoding="utf-8", errors="replace")
        stderr = Path(err_name).read_text(encoding="utf-8", errors="replace")
    finally:
        for name in (out_name, err_name):
            try:
                os.unlink(name)
            except OSError:
                pass

    peak = max(peak_win32, peak_psutil) or None
    return {
        "exit_code": None if proc is None else proc.returncode,
        "timed_out": timed_out,
        "wall_ms": round(wall_ms, 3),
        "stdout": stdout,
        "stderr": stderr,
        "peak_wset_bytes": peak,
        "peak_pagefile_bytes": peak_pagefile or None,
        "peak_win32_bytes": peak_win32 or None,
        "peak_psutil_bytes": peak_psutil or None,
        "rss_samples": rss_samples,
    }


# ------------------------------------------------------------------ parsing

ADAPTER_RE = re.compile(
    r"^adapter: backend=(\S+) \((\d+)\) type=(\S+) vendor(?:ID)?=0x([0-9A-Fa-f]+) device(?:ID)?=0x([0-9A-Fa-f]+)",
    re.MULTILINE,
)
ADAPTER_DEVICE_RE = re.compile(r"^adapter: device=(.+)$", re.MULTILINE)


def parse_adapter(stdout: str) -> dict:
    adapter = {"backend": None, "backend_code": None, "type": None,
               "vendor_id": None, "device_id": None, "device": None,
               "is_warp": None}
    m = ADAPTER_RE.search(stdout)
    if m:
        adapter.update(
            backend=m.group(1),
            backend_code=int(m.group(2)),
            type=m.group(3),
            vendor_id="0x" + m.group(4).upper(),
            device_id="0x" + m.group(5).upper(),
        )
    d = ADAPTER_DEVICE_RE.search(stdout)
    if d:
        adapter["device"] = d.group(1).strip()
    if adapter["type"] is not None:
        vendor = (adapter["vendor_id"] or "").lower()
        adapter["is_warp"] = adapter["type"] == "cpu" and vendor == "0x1414"
    return adapter


def parse_r2_window(stdout: str) -> dict:
    out = {}
    m = re.search(
        r"^window: closed after (\d+) frame\(s\), init ([\d.]+) ms, ([\d.]+) ms/frame average",
        stdout, re.MULTILINE)
    if m:
        out["frames"] = int(m.group(1))
        out["init_ms"] = float(m.group(2))
        out["window_frame_avg_ms"] = float(m.group(3))
    m = re.search(r"^window: last layout ([\d.]+) ms at (\d+)x(\d+)", stdout, re.MULTILINE)
    if m:
        out["last_layout_ms"] = float(m.group(1))
        out["last_layout_size"] = f"{m.group(2)}x{m.group(3)}"
    m = re.search(r"^window: cpu frame ([\d.]+) ms average", stdout, re.MULTILINE)
    if m:
        out["cpu_frame_ms"] = float(m.group(1))
    return out


def parse_r2_render(stdout: str) -> dict:
    out = {}
    m = re.search(
        r"^render: .*? layout=([\d.]+)ms frame=([\d.]+)ms", stdout, re.MULTILINE)
    if m:
        out["layout_ms"] = float(m.group(1))
        out["frame_ms"] = float(m.group(2))
    m = re.search(
        r"^timings: gpu\(encode\+submit\)=([\d.]+)ms \| readback\(map\)=([\d.]+)ms \| png=([\d.]+)ms",
        stdout, re.MULTILINE)
    if m:
        out["gpu_ms"] = float(m.group(1))
        out["readback_ms"] = float(m.group(2))
        out["png_ms"] = float(m.group(3))
    return out


# -------------------------------------------------------------- statistics

def series(values: list) -> dict:
    vals = [float(v) for v in values
            if isinstance(v, (int, float)) and not isinstance(v, bool)]
    if not vals:
        return {"median": None, "min": None, "max": None, "count": 0, "samples": []}
    return {
        "median": round(statistics.median(vals), 4),
        "min": round(min(vals), 4),
        "max": round(max(vals), 4),
        "count": len(vals),
        "samples": [round(v, 4) for v in vals],
    }


def run_series(scenario: str, exe: Path, args_factory, runs: int,
               parser, warmup: bool = True) -> dict:
    """Run a scenario `runs` times and aggregate parsed fields.

    Returns {"scenario", "exe", "args", "samples": [{wall,peak,parsed...}],
             "metrics": {field: series}, "adapter": {...}, "errors": [...]}
    """
    errors = []
    samples = []
    if warmup:
        try:
            run_measured(exe, args_factory(0), timeout_s=180)
        except Exception as exc:  # warmup failures surface again below
            errors.append(f"warmup: {exc}")
    for index in range(runs):
        try:
            result = run_measured(exe, args_factory(index), timeout_s=180)
        except Exception as exc:
            errors.append(f"run {index}: {exc}")
            continue
        parsed = parser(result["stdout"]) if parser else {}
        if result["exit_code"] != 0:
            errors.append(
                f"run {index}: exit={result['exit_code']} timed_out={result['timed_out']} "
                f"stderr={result['stderr'].strip()[:300]}")
        samples.append({
            "run": index,
            "exit_code": result["exit_code"],
            "wall_ms": result["wall_ms"],
            "peak_wset_bytes": result["peak_wset_bytes"],
            "peak_pagefile_bytes": result["peak_pagefile_bytes"],
            "parsed": parsed,
        })

    fields = set()
    for sample in samples:
        fields.update(k for k, v in sample["parsed"].items() if isinstance(v, (int, float)))
    metrics = {field: series([s["parsed"].get(field) for s in samples]) for field in sorted(fields)}
    metrics["wall_ms"] = series([s["wall_ms"] for s in samples])
    metrics["peak_wset_bytes"] = series([s["peak_wset_bytes"] for s in samples])
    metrics["peak_pagefile_bytes"] = series([s["peak_pagefile_bytes"] for s in samples])

    return {
        "scenario": scenario,
        "exe": str(exe),
        "args": args_factory(0),
        "runs": runs,
        "samples": samples,
        "metrics": metrics,
        "errors": errors,
    }


# ---------------------------------------------------------------- utilities

def sha256(path: Path) -> str:
    import hashlib
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


REPO_PREFIX = str(REPO.resolve()) + os.sep


def relativize_paths(value):
    """Recursively rewrite absolute repo paths stored in the JSON as relative."""
    if isinstance(value, dict):
        return {key: relativize_paths(item) for key, item in value.items()}
    if isinstance(value, list):
        return [relativize_paths(item) for item in value]
    if isinstance(value, str) and value.startswith(REPO_PREFIX):
        return str(Path(value).relative_to(REPO.resolve()))
    return value


def file_bytes(path: Path) -> int:
    return path.stat().st_size


def git_info() -> dict:
    def git(*args: str) -> str:
        try:
            res = subprocess.run(["git", *args], cwd=REPO, capture_output=True,
                                 text=True, timeout=30)
            return res.stdout.strip() if res.returncode == 0 else ""
        except Exception:
            return ""

    status = git("status", "--porcelain")
    status_lines = status.splitlines() if status else []
    outside = [line for line in status_lines
               if "benchmarks/engine-metrics/" not in line.replace("\\", "/")]
    return {
        "commit": git("rev-parse", "--short", "HEAD"),
        "commit_full": git("rev-parse", "HEAD"),
        "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "dirty": bool(status),
        "dirty_outside_engine_metrics": bool(outside),
        "status_porcelain": status_lines,
    }


def host_info() -> dict:
    return {
        "os": platform.platform(),
        "windows_version": platform.version(),
        "machine": platform.machine(),
        "cpu": os.environ.get("PROCESSOR_IDENTIFIER", platform.processor()),
        "cores": os.cpu_count(),
        "python": platform.python_version(),
        "psutil": PSUTIL_VERSION,
        "repo": str(REPO),
    }


def load_snapshot_meta() -> dict:
    meta_path = SNAPSHOT_DIR / "snapshot.json"
    meta = {}
    if meta_path.exists():
        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
        except Exception:
            meta = {}
    exe = SNAPSHOT_DIR / "runtime_r2.exe"
    dll = SNAPSHOT_DIR / "wgpu_native.dll"
    atlas = SNAPSHOT_DIR / "assets" / "atlas.bin"
    actual = {
        "exe_bytes": file_bytes(exe) if exe.exists() else None,
        "exe_sha256": sha256(exe) if exe.exists() else None,
        "dll_bytes": file_bytes(dll) if dll.exists() else None,
        "dll_sha256": sha256(dll) if dll.exists() else None,
        "atlas_bytes": file_bytes(atlas) if atlas.exists() else None,
        "atlas_sha256": sha256(atlas) if atlas.exists() else None,
    }
    verified = True
    for key, value in actual.items():
        if meta.get(key) not in (None, value):
            verified = False
    source_build = {}
    source_report = SNAPSHOT_DIR / "source_build_report.json"
    if source_report.exists():
        try:
            report = json.loads(source_report.read_text(encoding="utf-8"))
            window = report.get("window") or {}
            source_build = {
                "started": report.get("started"),
                "init_ms": window.get("init_ms"),
                "frame_avg_ms": window.get("frame_avg_ms"),
                "exe_bytes": (report.get("exe") or {}).get("bytes"),
            }
        except Exception:
            source_build = {}
    return {
        **meta,
        **actual,
        "path": str(exe),
        "meta_path": str(meta_path) if meta_path.exists() else None,
        "verified": verified,
        "source_build": source_build or None,
    }


def mb(num_bytes: float | None) -> float | None:
    return round(num_bytes / 1_000_000.0, 3) if num_bytes else None


def fmt(value, digits=3, suffix="") -> str:
    if value is None:
        return "n/a"
    return f"{value:.{digits}f}{suffix}"


def fmt_bytes(num_bytes: int | None) -> str:
    if num_bytes is None:
        return "n/a"
    return f"{num_bytes:,} B / {num_bytes / 1_000_000.0:.2f} MB"


def status_of(value, budget, lower_is_better=True) -> str:
    if value is None:
        return "n/a"
    if lower_is_better:
        return "PASS" if value <= budget else "FAIL"
    return "PASS" if value >= budget else "FAIL"


# ------------------------------------------------------------------- main

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runs", type=int, default=10, help="measured runs per metric (default 10)")
    parser.add_argument("--out", type=Path, default=HERE / "baseline.json")
    parser.add_argument("--md", type=Path, default=HERE / "BASELINE.md")
    parser.add_argument("--results-dir", type=Path, default=RESULTS_DIR)
    parser.add_argument("--skip-offscreen-sizes", action="store_true",
                        help="only render 960x640 offscreen (saves ~2/3 of that scenario)")
    parser.add_argument("--layout100-exe", type=Path, default=None,
                        help="optional ~100-node R2 variant; measured with --report like the "
                             "28-node fixture")
    parser.add_argument("--no-warmup", action="store_true")
    args = parser.parse_args()

    runs = max(1, args.runs)
    warmup = not args.no_warmup
    args.results_dir.mkdir(parents=True, exist_ok=True)
    WORK_DIR.mkdir(parents=True, exist_ok=True)

    issues: list[str] = []
    baseline = {
        "schema": "webnative.engine-metrics/1",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "runs_per_metric": runs,
        "warmup_runs": 1 if warmup else 0,
        "git": git_info(),
        "host": host_info(),
        "budget": {
            "install_mb": BUDGET_INSTALL_MB,
            "engine_mb": BUDGET_ENGINE_MB,
            "startup_ms": BUDGET_STARTUP_MS,
            "rss_mb": BUDGET_RSS_MB,
            "cpu_frame_ms": BUDGET_CPU_FRAME_MS,
            "layout_ms": BUDGET_LAYOUT_MS,
        },
        "method": {
            "install_size": "bytes(exe) + bytes(wgpu_native.dll) + assets externos de runtime; "
                            "el atlas va compilado dentro del ejecutable, así que los assets "
                            "externos son 0 y el atlas horneado se reporta aparte (sin contarlo dos veces)",
            "engine_size": "solo bytes del .exe del runtime",
            "startup": "init reportado por el runtime en la ejecución de ventana (arranque del "
                       "loop -> instancia/adaptador/dispositivo/pipelines listos; el primer frame "
                       "viene inmediatamente después). El wall externo de `--frames 1` se reporta "
                       "como cota superior porque incluye spawn del proceso y teardown.",
            "rss": "PeakWorkingSetSize vía Win32 GetProcessMemoryInfo muestreado cada 2 ms "
                   "mientras corre el proceso (cruce con psutil `memory_info().peak_wset`); se "
                   "reporta el mayor de los dos. También se guarda el pico de pagefile (commit) "
                   "como dato informativo.",
            "cpu_frame": "R2 imprime `window: cpu frame X ms average` (upload+encode+submit, sin "
                         "layout ni present/vsync).",
            "layout": "`--report` de R2 ejecuta el layout 100 veces por viewport e imprime "
                      "best/avg; se reporta la mediana entre ejecuciones del `layout_ms_avg`. "
                      "El escenario de ~100 nodos usa un fixture propio y un binario R2 variante "
                      "(mismo runtime_r2.c, ui_data.h de 100 nodos).",
            "offscreen": "wall de `--render W H out.png` (proceso completo) más los tiempos "
                         "internos que imprime el runtime (layout/frame/gpu/readback/png).",
            "backend": "línea de adaptador del runtime; WARP es type=cpu + vendorID=0x1414",
        },
        "snapshot": load_snapshot_meta(),
        "r2": {},
        "issues": issues,
    }

    if not (SNAPSHOT_DIR / "runtime_r2.exe").exists():
        print("ERROR: snapshot/runtime_r2.exe is missing; run run.ps1 first", file=sys.stderr)
        return 2
    if not (SNAPSHOT_DIR / "wgpu_native.dll").exists():
        print("ERROR: snapshot/wgpu_native.dll is missing; run run.ps1 first", file=sys.stderr)
        return 2

    r2_exe = SNAPSHOT_DIR / "runtime_r2.exe"

    # ---------------------------------------------------------------- R2 run
    print(f"[r2] window --frames {FRAMES_WINDOW} x{runs}")
    window = run_series(
        "r2-window", r2_exe, lambda i: ["--frames", str(FRAMES_WINDOW)],
        runs, parse_r2_window, warmup=warmup)
    issues.extend(window["errors"])
    # Adapter: one cheap extra run (window init, one frame) instead of keeping
    # every stdout in memory.
    probe = run_measured(r2_exe, ["--frames", "1"], timeout_s=180)
    adapter = parse_adapter(probe["stdout"])
    if probe["exit_code"] != 0:
        issues.append(f"r2 adapter probe failed: exit={probe['exit_code']}")

    print(f"[r2] startup wall --frames 1 x{runs}")
    startup_wall = run_series(
        "r2-frames-1", r2_exe, lambda i: ["--frames", "1"],
        runs, parse_r2_window, warmup=warmup)
    issues.extend(startup_wall["errors"])

    print(f"[r2] layout report {LAYOUT_SIZES} x{runs}")
    layout_runs = []
    layout_errors = []
    if warmup:
        layout_path = WORK_DIR / "layout_warmup.json"
        run_measured(r2_exe, ["--report", str(layout_path), "--sizes", LAYOUT_SIZES],
                     timeout_s=180)
    for index in range(runs):
        layout_path = WORK_DIR / f"layout_{index}.json"
        try:
            layout_path.unlink(missing_ok=True)
            result = run_measured(
                r2_exe, ["--report", str(layout_path), "--sizes", LAYOUT_SIZES], timeout_s=180)
            if result["exit_code"] != 0:
                layout_errors.append(f"run {index}: exit={result['exit_code']}")
                continue
            report = json.loads(layout_path.read_text(encoding="utf-8"))
            entry = {"run": index, "wall_ms": result["wall_ms"],
                     "peak_wset_bytes": result["peak_wset_bytes"],
                     "node_count": report.get("node_count"), "sizes": {}}
            for size in report.get("sizes", []):
                key = f"{size['width']}x{size['height']}"
                entry["sizes"][key] = {
                    "avg_ms": size.get("layout_ms_avg"),
                    "best_ms": size.get("layout_ms_best"),
                }
            layout_runs.append(entry)
        except Exception as exc:
            layout_errors.append(f"run {index}: {exc}")
    issues.extend(layout_errors)

    layout = {"node_count": None, "iterations": 100, "sizes": {}}
    if layout_runs:
        layout["node_count"] = layout_runs[0]["node_count"]
        layout["wall_ms"] = series([r["wall_ms"] for r in layout_runs])
        size_keys = sorted({k for r in layout_runs for k in r["sizes"]})
        for key in size_keys:
            layout["sizes"][key] = {
                "avg_ms": series([r["sizes"].get(key, {}).get("avg_ms") for r in layout_runs]),
                "best_ms": series([r["sizes"].get(key, {}).get("best_ms") for r in layout_runs]),
            }
        layout["raw"] = layout_runs
    else:
        issues.append("r2 layout report: no successful run")

    # Optional ~100-node variant (built by run.ps1 when --layout100 is used).
    layout100 = None
    if args.layout100_exe:
        if not args.layout100_exe.exists():
            issues.append(f"layout100 exe missing: {args.layout100_exe}")
        else:
            print(f"[r2] layout ~100 nodes ({args.layout100_exe.name}) x{runs}")
            runs100 = []
            errors100 = []
            if warmup:
                probe_path = WORK_DIR / "layout100_warmup.json"
                run_measured(args.layout100_exe,
                             ["--report", str(probe_path), "--sizes", LAYOUT_SIZES],
                             timeout_s=180)
            for index in range(runs):
                report_path = WORK_DIR / f"layout100_{index}.json"
                try:
                    report_path.unlink(missing_ok=True)
                    result = run_measured(
                        args.layout100_exe,
                        ["--report", str(report_path), "--sizes", LAYOUT_SIZES],
                        timeout_s=180)
                    if result["exit_code"] != 0:
                        errors100.append(f"run {index}: exit={result['exit_code']}")
                        continue
                    report = json.loads(report_path.read_text(encoding="utf-8"))
                    entry = {"run": index, "wall_ms": result["wall_ms"],
                             "node_count": report.get("node_count"), "sizes": {}}
                    for size in report.get("sizes", []):
                        key = f"{size['width']}x{size['height']}"
                        entry["sizes"][key] = {
                            "avg_ms": size.get("layout_ms_avg"),
                            "best_ms": size.get("layout_ms_best"),
                        }
                    runs100.append(entry)
                except Exception as exc:
                    errors100.append(f"run {index}: {exc}")
            issues.extend(errors100)
            if runs100:
                sizes100 = {}
                for key in sorted({k for r in runs100 for k in r["sizes"]}):
                    sizes100[key] = {
                        "avg_ms": series([r["sizes"].get(key, {}).get("avg_ms")
                                          for r in runs100]),
                        "best_ms": series([r["sizes"].get(key, {}).get("best_ms")
                                           for r in runs100]),
                    }
                layout100 = {
                    "exe": str(args.layout100_exe),
                    "node_count": runs100[0]["node_count"],
                    "fixture": "benchmarks/engine-metrics/fixtures/layout-100/index.html",
                    "note": "mismo runtime_r2.c recompilado contra un ui_data.h de 100 nodos; "
                            "solo mide layout, no forma parte del install/engine size",
                    "sizes": sizes100,
                    "runs": runs100,
                }

    offscreen = {}
    sizes = OFFSCREEN_SIZES if not args.skip_offscreen_sizes else OFFSCREEN_SIZES[:1]
    for width, height in sizes:
        key = f"{width}x{height}"
        print(f"[r2] offscreen --render {width} {height} x{runs}")
        png = WORK_DIR / f"r2_out_{width}.png"
        scenario = run_series(
            f"r2-render-{key}", r2_exe,
            lambda i, w=width, h=height, p=png: ["--render", str(w), str(h), str(p)],
            runs, parse_r2_render, warmup=warmup)
        issues.extend(scenario["errors"])
        offscreen[key] = {
            "metrics": scenario["metrics"],
            "runs": scenario["samples"],
        }

    # Determinism: re-render 960x640 once and compare the PNG with the last
    # measured run (the runtime is deterministic by design).
    determinism = None
    if "960x640" in offscreen:
        measured_png = WORK_DIR / "r2_out_960.png"
        repeat_png = WORK_DIR / "r2_out_960_repeat.png"
        repeat = run_measured(r2_exe, ["--render", "960", "640", str(repeat_png)],
                              timeout_s=180)
        if repeat["exit_code"] == 0 and measured_png.exists() and repeat_png.exists():
            first_hash = sha256(measured_png)
            repeat_hash = sha256(repeat_png)
            determinism = {
                "render_960_sha256": first_hash,
                "repeat_sha256": repeat_hash,
                "matches": first_hash == repeat_hash,
            }
            if first_hash != repeat_hash:
                issues.append("offscreen render is not deterministic at 960x640")
        else:
            issues.append(f"determinism re-render failed: exit={repeat['exit_code']}")

    baseline["r2"] = {
        "exe": str(r2_exe),
        "adapter": adapter,
        "window": window,
        "startup_wall": startup_wall,
        "layout": layout,
        "layout_100_nodes": layout100,
        "offscreen": offscreen,
        "determinism": determinism,
    }

    # ------------------------------------------------------------- sizes R2
    exe_bytes = file_bytes(r2_exe)
    dll_bytes = file_bytes(SNAPSHOT_DIR / "wgpu_native.dll")
    atlas_path = SNAPSHOT_DIR / "assets" / "atlas.bin"
    atlas_bytes = file_bytes(atlas_path) if atlas_path.exists() else 0
    baseline["r2"]["install"] = {
        "exe_bytes": exe_bytes,
        "dll_bytes": dll_bytes,
        "external_assets_bytes": 0,
        "install_bytes": exe_bytes + dll_bytes,
        "install_mb": mb(exe_bytes + dll_bytes),
        "baked_assets_bytes": atlas_bytes,
        "note": "atlas.bin is compiled into runtime_r2.exe (kUiAtlasPixels); "
                "no external runtime assets are loaded",
    }
    baseline["r2"]["engine"] = {
        "exe_bytes": exe_bytes,
        "exe_mb": mb(exe_bytes),
        "note": "includes the baked 64 KiB R8 atlas",
    }

    # ------------------------------------------------------------- summary
    r2_window_metrics = window["metrics"]
    r2_cpu = r2_window_metrics.get("cpu_frame_ms", series([]))
    summary = {
        "r2_install_mb": baseline["r2"]["install"]["install_mb"],
        "r2_engine_mb": baseline["r2"]["engine"]["exe_mb"],
        "r2_startup_init_ms": r2_window_metrics.get("init_ms", {}).get("median"),
        "r2_startup_wall_frames1_ms": startup_wall["metrics"]["wall_ms"]["median"],
        "r2_rss_peak_mb": mb(r2_window_metrics.get("peak_wset_bytes", {}).get("median")),
        "r2_cpu_frame_ms": r2_cpu.get("median"),
        "r2_cpu_frame_plus_layout_ms": (
            round((r2_cpu.get("median") or 0.0)
                  + (r2_window_metrics.get("last_layout_ms", {}).get("median") or 0.0), 4)
            if r2_cpu.get("median") is not None else None),
        "r2_window_frame_avg_ms": r2_window_metrics.get("window_frame_avg_ms", {}).get("median"),
        "r2_layout_28_960_ms": layout.get("sizes", {}).get("960x640", {})
                               .get("avg_ms", {}).get("median"),
        "r2_layout_28_720_ms": layout.get("sizes", {}).get("720x640", {})
                               .get("avg_ms", {}).get("median"),
        "r2_layout_28_560_ms": layout.get("sizes", {}).get("560x640", {})
                               .get("avg_ms", {}).get("median"),
        "r2_layout_100_ms": ((layout100 or {}).get("sizes", {}).get("960x640", {})
                             .get("avg_ms", {}) or {}).get("median"),
        "r2_offscreen_960_wall_ms": offscreen.get("960x640", {})
                                    .get("metrics", {}).get("wall_ms", {}).get("median"),
        "r2_offscreen_960_frame_ms": offscreen.get("960x640", {})
                                     .get("metrics", {}).get("frame_ms", {}).get("median"),
        "backend": adapter.get("backend"),
        "is_warp": adapter.get("is_warp"),
    }
    baseline["summary"] = summary
    baseline["issues"] = issues
    baseline["output"] = str(args.out)

    def budget_entry(name: str, actual, budget, unit: str, note: str = "") -> dict:
        if actual is None:
            status = "n/a"
        elif budget is None:
            status = "info"
        elif actual <= budget:
            status = "PASS"
        else:
            status = "FAIL"
        return {"metric": name, "actual": actual, "budget": budget, "unit": unit,
                "status": status, "note": note}

    baseline["budget_check"] = [
        budget_entry("install_size", summary.get("r2_install_mb"), BUDGET_INSTALL_MB, "MB"),
        budget_entry("engine_size", summary.get("r2_engine_mb"), BUDGET_ENGINE_MB, "MB"),
        budget_entry("startup", summary.get("r2_startup_init_ms"), BUDGET_STARTUP_MS, "ms"),
        budget_entry("rss", summary.get("r2_rss_peak_mb"), BUDGET_RSS_MB, "MB",
                     "peak working set; WARP inflates this on this host"),
        budget_entry("cpu_frame", summary.get("r2_cpu_frame_ms"), BUDGET_CPU_FRAME_MS, "ms"),
        budget_entry("layout_28_nodes_960", summary.get("r2_layout_28_960_ms"),
                     BUDGET_LAYOUT_MS, "ms"),
        budget_entry("layout_100_nodes_960", summary.get("r2_layout_100_ms"),
                     BUDGET_LAYOUT_MS, "ms"),
        budget_entry("offscreen_960_wall", summary.get("r2_offscreen_960_wall_ms"),
                     None, "ms", "informational, no budget"),
    ]

    # --------------------------------------------------------------- write
    baseline = relativize_paths(baseline)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(baseline, indent=2, ensure_ascii=False) + "\n",
                        encoding="utf-8")
    markdown = render_markdown(baseline)
    args.md.write_text(markdown, encoding="utf-8")
    print(f"\nwrote {args.out}")
    print(f"wrote {args.md}")
    if issues:
        print(f"\n{len(issues)} issue(s):")
        for issue in issues:
            print(f"  - {issue}")
    return 0


# ------------------------------------------------------------- markdown

def render_markdown(b: dict) -> str:
    s = b["summary"]
    git = b["git"]
    adapter = b["r2"].get("adapter", {})
    lines: list[str] = []

    lines.append("# Engine metrics — baseline\n")
    dirty = git.get("dirty")
    dirty_outside = git.get("dirty_outside_engine_metrics")
    if dirty_outside:
        tree_note = (" (árbol de trabajo con cambios concurrentes de otro stream fuera de "
                     "benchmarks/engine-metrics; el snapshot congelado corresponde al commit "
                     f"{b.get('snapshot', {}).get('commit') or git.get('commit')})")
    elif dirty:
        tree_note = " (solo cambios sin trackear en benchmarks/engine-metrics)"
    else:
        tree_note = " (árbol limpio)"
    lines.append(f"Generado: **{b['generated_at_utc']}** · commit "
                 f"**{git.get('commit') or 'n/a'}**{tree_note} · "
                 f"{b['runs_per_metric']} runs por métrica"
                 f"{(' + 1 warm-up' if b.get('warmup_runs') else '')}.\n")
    lines.append(f"Host: {b['host'].get('os')} · {b['host'].get('cpu')} · "
                 f"{b['host'].get('cores')} cores · Python {b['host'].get('python')} · "
                 f"psutil {b['host'].get('psutil')}\n")
    backend = adapter.get("backend") or "n/a"
    warp = adapter.get("is_warp")
    device = adapter.get("device") or "n/a"
    lines.append(f"Backend: **{backend}** · type={adapter.get('type')} · "
                 f"vendor={adapter.get('vendor_id')} device={adapter.get('device_id')} · "
                 f"{device} · "
                 f"{'**WARP (software rasterizer, no hardware GPU)**' if warp else 'hardware'}\n")
    if warp:
        lines.append("> Los números de GPU/offscreen y parte del RSS están dominados por WARP "
                     "(`Microsoft Basic Render Driver`). No son representativos de una GPU real; "
                     "sirven como cota superior de CPU/software.\n")

    lines.append("## Snapshot medido\n")
    snap = b.get("snapshot", {})
    lines.append(f"- `snapshot/runtime_r2.exe` — {fmt_bytes(snap.get('exe_bytes'))} — "
                 f"SHA-256 `{snap.get('exe_sha256')}`")
    lines.append(f"- `snapshot/wgpu_native.dll` — {fmt_bytes(snap.get('dll_bytes'))} — "
                 f"SHA-256 `{snap.get('dll_sha256')}`")
    lines.append(f"- `snapshot/assets/atlas.bin` — {fmt_bytes(snap.get('atlas_bytes'))} "
                 f"(horneado en el exe, no se carga en runtime)")
    lines.append(f"- Procedencia: `src/runtime/c-r2/build` en commit "
                 f"`{snap.get('commit') or git.get('commit')}`; "
                 f"hashes verificados: {'sí' if snap.get('verified') else 'NO'}")
    prov = snap.get("provenance") or {}
    if prov:
        lines.append(
            f"- Recompilación de control: `runtime_r2.c` del commit "
            f"`{prov.get('source_commit') or snap.get('commit')}` + `build/ui_data.h` → "
            f"{prov.get('rebuild_exe_bytes', 'n/a'):,} B (mismo tamaño que el snapshot); "
            f"PNG 960×640 "
            f"{'idéntico' if prov.get('render_matches') else 'DISTINTO'} "
            f"(`{prov.get('render_960_sha256')}`).")
    det = b["r2"].get("determinism") or {}
    if det:
        lines.append(f"- Determinismo del snapshot: dos `--render 960 640` → "
                     f"{'mismo SHA-256' if det.get('matches') else 'SHA-256 DISTINTO'} "
                     f"(`{det.get('render_960_sha256')}`).")
    lines.append("")

    lines.append("## Tabla comparativa vs presupuesto fase 1\n")
    lines.append("| Métrica | R2 | Presupuesto | Estado |")
    lines.append("|---|---:|---:|---|")

    def row(label, r2_value, budget, status):
        lines.append(f"| {label} | {r2_value} | {budget} | {status} |")

    r2_install = s.get("r2_install_mb")
    row("Install size (exe + DLL + assets)",
        fmt(r2_install, 2, " MB"),
        "≤ 15 MB",
        status_of(r2_install, BUDGET_INSTALL_MB))
    row("Engine size (.exe)",
        fmt(s.get("r2_engine_mb"), 2, " MB"),
        "≤ 1 MB",
        status_of(s.get("r2_engine_mb"), BUDGET_ENGINE_MB))
    row("Startup (init → primer frame)",
        fmt(s.get("r2_startup_init_ms"), 2, " ms"),
        "≤ 150 ms",
        status_of(s.get("r2_startup_init_ms"), BUDGET_STARTUP_MS))
    row("RSS pico (ventana)",
        fmt(s.get("r2_rss_peak_mb"), 2, " MB"),
        "≤ 30 MB",
        status_of(s.get("r2_rss_peak_mb"), BUDGET_RSS_MB))
    row("CPU frame (R2 print)",
        fmt(s.get("r2_cpu_frame_ms"), 3, " ms"),
        "≤ 2 ms",
        status_of(s.get("r2_cpu_frame_ms"), BUDGET_CPU_FRAME_MS))
    row("CPU frame + layout inicial",
        fmt(s.get("r2_cpu_frame_plus_layout_ms"), 3, " ms"),
        "≤ 2 ms",
        status_of(s.get("r2_cpu_frame_plus_layout_ms"), BUDGET_CPU_FRAME_MS))
    row("Layout 28 nodos (960/720/560)",
        f"{fmt(s.get('r2_layout_28_960_ms'), 4)} / {fmt(s.get('r2_layout_28_720_ms'), 4)} / "
        f"{fmt(s.get('r2_layout_28_560_ms'), 4)} ms",
        "≤ 0.5 ms",
        status_of(max(filter(None, [s.get('r2_layout_28_960_ms'),
                                    s.get('r2_layout_28_720_ms'),
                                    s.get('r2_layout_28_560_ms')]), default=None),
                  BUDGET_LAYOUT_MS))
    row("Layout ~100 nodos (960)",
        fmt(s.get("r2_layout_100_ms"), 4, " ms"),
        "≤ 0.5 ms",
        status_of(s.get("r2_layout_100_ms"), BUDGET_LAYOUT_MS))
    row("Offscreen 960×640 (wall proceso)",
        fmt(s.get("r2_offscreen_960_wall_ms"), 1, " ms"),
        "informativo", "—")
    row("Offscreen 960×640 (interno)",
        fmt(s.get("r2_offscreen_960_frame_ms"), 1, " ms"),
        "informativo", "—")
    row("Frame ventana (vsync)",
        fmt(s.get("r2_window_frame_avg_ms"), 2, " ms"),
        "—", "—")
    lines.append("")
    lines.append("El frame de ventana está limitado por vsync (~16 ms) y no es CPU puro.")
    lines.append("")
    lines.append("Los valores de la tabla son **medianas** de "
                 f"{b['runs_per_metric']} ejecuciones. `n/a` significa no medible de forma fiable "
                 "con el binario actual (se explica abajo).\n")

    lines.append("## Muestras por métrica (mediana / mínimo / máximo)\n")
    lines.append("| Escenario | Métrica | Mediana | Mín | Máx |")
    lines.append("|---|---|---:|---:|---:|")

    def add_rows(label: str, scenario: dict, fields: list[tuple[str, str, str]]):
        metrics = scenario.get("metrics", {})
        for key, name, unit in fields:
            data = metrics.get(key, {})
            if data.get("median") is None:
                continue
            if key.endswith("_bytes"):
                lines.append(
                    f"| {label} | {name} | {data['median'] / 1e6:.2f} MB | "
                    f"{data['min'] / 1e6:.2f} MB | {data['max'] / 1e6:.2f} MB |")
                continue
            lines.append(f"| {label} | {name} | {fmt(data['median'], 3, unit)} | "
                         f"{fmt(data['min'], 3, unit)} | {fmt(data['max'], 3, unit)} |")

    add_rows("R2 ventana 30 frames", b["r2"]["window"], [
        ("init_ms", "startup init", " ms"),
        ("cpu_frame_ms", "CPU frame", " ms"),
        ("window_frame_avg_ms", "frame pared (vsync)", " ms"),
        ("last_layout_ms", "layout del WM_SIZE", " ms"),
        ("wall_ms", "wall del proceso", " ms"),
        ("peak_wset_bytes", "RSS pico (working set)", " B"),
        ("peak_pagefile_bytes", "commit pico (pagefile, informativo)", " B"),
    ])
    if b["r2"].get("startup_wall"):
        add_rows("R2 --frames 1", b["r2"]["startup_wall"], [
            ("init_ms", "startup init", " ms"),
            ("wall_ms", "wall proceso (cota superior)", " ms"),
        ])
    for key in ("960x640", "720x640", "560x640"):
        data = b["r2"].get("layout", {}).get("sizes", {}).get(key)
        if not data:
            continue
        avg = data["avg_ms"]
        best = data["best_ms"]
        if avg.get("median") is not None:
            lines.append(f"| R2 layout {key} (28 nodos) | layout avg | "
                         f"{fmt(avg['median'], 4, ' ms')} | {fmt(avg['min'], 4, ' ms')} | "
                         f"{fmt(avg['max'], 4, ' ms')} |")
        if best.get("median") is not None:
            lines.append(f"| R2 layout {key} (28 nodos) | layout best | "
                         f"{fmt(best['median'], 4, ' ms')} | {fmt(best['min'], 4, ' ms')} | "
                         f"{fmt(best['max'], 4, ' ms')} |")
    l100 = b["r2"].get("layout_100_nodes") or {}
    for key, data in (l100.get("sizes") or {}).items():
        avg = (data.get("avg_ms") or {})
        if avg.get("median") is not None:
            lines.append(f"| R2 layout {key} ({l100.get('node_count')} nodos) | layout avg | "
                         f"{fmt(avg['median'], 4, ' ms')} | {fmt(avg.get('min'), 4, ' ms')} | "
                         f"{fmt(avg.get('max'), 4, ' ms')} |")
    for key, data in b["r2"].get("offscreen", {}).items():
        fields = [
            ("wall_ms", "wall proceso", " ms"),
            ("layout_ms", "layout", " ms"),
            ("frame_ms", "frame interno", " ms"),
            ("gpu_ms", "gpu encode+submit", " ms"),
            ("readback_ms", "readback/map", " ms"),
            ("png_ms", "PNG", " ms"),
            ("peak_wset_bytes", "RSS pico", " B"),
        ]
        add_rows(f"R2 offscreen {key}", data, fields)
    lines.append("")
    lines.append("La fila «commit pico (pagefile)» es la carga de commit máxima, no memoria "
                 "residente: con WARP/D3D12 incluye reservas grandes del rasterizador software "
                 "(el working set es la métrica de RSS del presupuesto).")
    lines.append("")

    lines.append("## Comparación con el presupuesto\n")
    def verdict(value, budget, label, unit):
        if value is None:
            return f"- **{label}**: `n/a` — {unit}"
        if value <= budget:
            return (f"- **{label}**: {value:.3f} contra {budget:g} → **PASS** "
                    f"(margen {budget - value:.3f})")
        return (f"- **{label}**: {value:.3f} contra {budget:g} → **FAIL** "
                f"(exceso {value - budget:.3f})")

    lines.append(verdict(s.get("r2_install_mb"), BUDGET_INSTALL_MB,
                         "Install size R2", "sin dato"))
    lines.append(verdict(s.get("r2_engine_mb"), BUDGET_ENGINE_MB,
                         "Engine size R2", "sin dato"))
    lines.append(verdict(s.get("r2_startup_init_ms"), BUDGET_STARTUP_MS,
                         "Startup R2", "sin dato"))
    lines.append(verdict(s.get("r2_rss_peak_mb"), BUDGET_RSS_MB,
                         "RSS R2", "sin dato"))
    lines.append(verdict(s.get("r2_cpu_frame_ms"), BUDGET_CPU_FRAME_MS,
                         "CPU frame R2", "sin dato"))
    lines.append(verdict(s.get("r2_layout_28_960_ms"), BUDGET_LAYOUT_MS,
                         "Layout 28 nodos R2", "sin dato"))
    if s.get("r2_layout_100_ms") is not None:
        lines.append(verdict(s.get("r2_layout_100_ms"), BUDGET_LAYOUT_MS,
                             "Layout 100 nodos R2", "sin dato"))
    else:
        lines.append("- **Layout 100 nodos R2**: `n/a` — el fixture canónico tiene 28 nodos y "
                     "no hay un escenario de 100 nodos en este baseline; medirlo exige un "
                     "fixture y un binario variante (ver «Pendiente»).")
    if warp:
        lines.append("")
        lines.append("> Con WARP, install/engine/startup/CPU-frame son válidos como medida "
                     "de software, pero RSS y offscreen incluyen el rasterizador software de "
                     "D3D12 y **no** deben compararse con una GPU real.\n")

    layout_note = (b["r2"].get("layout") or {})
    if layout_note.get("node_count"):
        lines.append(f"Layout R2: `layout_report.json` reporta **{layout_note['node_count']} "
                     f"nodos** y 100 iteraciones por viewport; se mide el `layout_ms_avg` de "
                     "cada ejecución y se agrega la mediana de las ejecuciones.\n")
    if l100 and l100.get("node_count"):
        lines.append(f"Layout ~100 nodos: fixture `{l100.get('fixture')}` con "
                     f"**{l100['node_count']} nodos**, mismo `runtime_r2.c` recompilado contra su "
                     "`ui_data.h`. Solo mide layout; no entra en install/engine size.\n")

    lines.append("## Observaciones (derivadas de esta medición)\n")
    source_build = (b.get("snapshot", {}).get("source_build") or {})
    if source_build.get("init_ms") is not None and s.get("r2_startup_init_ms") is not None:
        lines.append(
            f"- **Arranque en frío vs caliente**: el `build_report.json` del snapshot (primera "
            f"ejecución tras compilar) registró init {source_build['init_ms']:.2f} ms; la mediana "
            f"de esta tanda es {s['r2_startup_init_ms']:.2f} ms con un warm-up descartado. El "
            "presupuesto se compara con la mediana en caliente (estable); para un SLA de primer "
            "arranque hay que medir cold start de forma explícita.")
    install = b["r2"]["install"]
    if install.get("install_bytes"):
        share = 100.0 * install["dll_bytes"] / install["install_bytes"]
        lines.append(
            f"- **Install**: la DLL de wgpu es {install['dll_bytes'] / 1e6:.2f} MB, el "
            f"{share:.1f} % del total; el `.exe` de R2 es {install['exe_bytes'] / 1e6:.3f} MB. "
            "El atlas (64 KiB) ya está dentro del `.exe`.")
    if s.get("r2_cpu_frame_ms") is not None:
        lines.append(
            f"- **CPU frame**: {s['r2_cpu_frame_ms']:.3f} ms de media (sin layout ni vsync), muy "
            f"por debajo de {BUDGET_CPU_FRAME_MS:g} ms; el frame de ventana de "
            f"{fmt(s.get('r2_window_frame_avg_ms'), 2)} ms está limitado por present/vsync.")
    if s.get("r2_offscreen_960_wall_ms") is not None:
        lines.append(
            f"- **Offscreen**: {s['r2_offscreen_960_wall_ms']:.1f} ms de wall, de los cuales "
            f"~{s.get('r2_offscreen_960_frame_ms') or 0:.1f} ms son el frame interno "
            "(readback + PNG dominan); el resto es arranque de wgpu por proceso.")
    lines.append("")

    issues = b.get("issues") or []
    if issues:
        lines.append("## Incidencias de la ejecución\n")
        for issue in issues:
            lines.append(f"- {issue}")
        lines.append("")
    else:
        lines.append("## Incidencias de la ejecución\n\nNinguna: todas las ejecuciones "
                     "terminaron con exit 0.\n")

    lines.append("## Método (resumen)\n")
    for key, text in b["method"].items():
        lines.append(f"- **{key}**: {text}")
    lines.append("")
    lines.append(f"El JSON crudo con todas las muestras es `{Path(b.get('output', 'baseline.json')).name}` "
                 f"(`{b.get('schema')}`).\n")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    sys.exit(main())
