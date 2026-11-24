#!/usr/bin/env python3
"""Local benchmark for the C++ frontend: direct-binary wall + stage timings.

The official harness (benchmarks/frontend-parity/tools/bench.py) spawns Windows
PowerShell for every measurement (~250-500 ms of shell startup), which hides the
cost of a native frontend.  This script runs build/frontend-cpp.exe directly, so
wall time isolates process startup + parse + cascade + layout + emit, and also
verifies every output against the golden IR.

Writes results/cpp/local-bench.json and refreshes
results/cpp/<fixture>.ir.json / .timings.json (the paths the harness reuses).
"""
from __future__ import annotations

import json
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent          # src/frontend/cpp
ROOT = HERE.parents[2]                          # repo root
PARITY = ROOT / "benchmarks" / "frontend-parity"
RESULTS = PARITY / "results" / "cpp"

sys.path.insert(0, str(PARITY / "tools"))
from verify_ir import compare, load  # noqa: E402

FIXTURES = [
    ("contract", PARITY / "fixtures" / "contract" / "input.html", 400, 300,
     PARITY / "golden" / "contract.ir.json"),
    ("flex-page", PARITY / "fixtures" / "flex-page" / "index.html", 480, 320,
     PARITY / "golden" / "flex-page.ir.json"),
]

# Names emitted by the binary (kept identical to the Python reference schema;
# "text+ir" is the emit stage, "pipeline" is the total).
STAGES = ("html", "css", "cascade", "fonts", "layout", "text+ir", "pipeline")
RUNS = 10
WARMUP = 2


def stats(values: list[float]) -> dict:
    return {
        "median": round(statistics.median(values), 3),
        "min": round(min(values), 3),
        "max": round(max(values), 3),
        "runs": [round(value, 3) for value in values],
    }


def main() -> int:
    info = json.loads((HERE / "BUILD_INFO.json").read_text(encoding="utf-8"))
    binary = HERE / info["binary"]
    if not binary.exists():
        print(f"missing binary: {binary}", file=sys.stderr)
        return 2

    report: dict = {
        "binary": info["binary"],
        "build_mode": info["build_mode"],
        "compiler": info.get("compiler", ""),
        "binary_bytes": info["binary_bytes"],
        "runs": RUNS,
        "warmup": WARMUP,
        "fixtures": {},
    }
    failures = 0

    for name, html, width, height, golden_path in FIXTURES:
        out = RESULTS / f"{name}.ir.json"
        timings_path = RESULTS / f"{name}.timings.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        golden = load(golden_path)

        args = [
            str(binary),
            "--html", str(html),
            "--width", str(width),
            "--height", str(height),
            "--out", str(out),
            "--timings", str(timings_path),
        ]

        for _ in range(WARMUP):
            subprocess.run(args, check=True, capture_output=True, text=True)

        walls: list[float] = []
        stage_values: dict[str, list[float]] = {stage: [] for stage in STAGES}
        ops = 0
        peak_rss_kb = 0
        for _ in range(RUNS):
            started = time.perf_counter()
            proc = subprocess.run(args, check=True, capture_output=True, text=True)
            walls.append((time.perf_counter() - started) * 1000.0)
            payload = json.loads(proc.stdout.strip().splitlines()[-1])
            for stage in STAGES:
                stage_values[stage].append(float(payload.get(stage, 0.0)))
            ops = int(payload.get("ops", 0))
            peak_rss_kb = int(payload.get("peak_rss_kb", peak_rss_kb))

        errors = compare(golden, load(out))
        if errors:
            failures += 1
            print(f"{name}: FAIL {'; '.join(errors[:5])}", file=sys.stderr)
        else:
            print(f"{name}: PASS ({ops} ops)")

        # Harness-equivalent wall time: every run goes through run.ps1 in a new
        # PowerShell process, exactly like tools/bench.py does.
        ps_args = [
            "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", str(HERE / "run.ps1"),
            "-Html", str(html),
            "-Width", str(width),
            "-Height", str(height),
            "-Out", str(out),
            "-Timings", str(timings_path),
        ]
        for _ in range(WARMUP):
            subprocess.run(ps_args, check=True, capture_output=True, text=True)
        ps_walls: list[float] = []
        for _ in range(RUNS):
            started = time.perf_counter()
            subprocess.run(ps_args, check=True, capture_output=True, text=True)
            ps_walls.append((time.perf_counter() - started) * 1000.0)

        report["fixtures"][name] = {
            "status": "FAIL" if errors else "PASS",
            "ops": ops,
            "wall_ms": stats(walls),
            "run_ps1_wall_ms": stats(ps_walls),
            "stages_ms": {
                stage: stats(values) for stage, values in stage_values.items()
            },
            "peak_rss_kb": peak_rss_kb,
            "ir": str(out.relative_to(ROOT)).replace("\\", "/"),
        }

    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / "local-bench.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")

    print()
    print(f"binary: {info['binary']} ({info['build_mode']}, {info['binary_bytes']} bytes)")
    print(f"runs per fixture: {RUNS} (warmup {WARMUP}), direct binary, wall includes startup")
    print("| fixture | status | wall median | wall min | run.ps1 median | pipeline median | peak RSS |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for name, entry in report["fixtures"].items():
        print(f"| {name} | {entry['status']} | {entry['wall_ms']['median']} ms | "
              f"{entry['wall_ms']['min']} ms | "
              f"{entry['run_ps1_wall_ms']['median']} ms | "
              f"{entry['stages_ms']['pipeline']['median']} ms | "
              f"{entry['peak_rss_kb']} KB |")
    print()
    for name, entry in report["fixtures"].items():
        stages = ", ".join(
            f"{stage}={entry['stages_ms'][stage]['median']}" for stage in STAGES)
        print(f"{name}: {stages}")

    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
