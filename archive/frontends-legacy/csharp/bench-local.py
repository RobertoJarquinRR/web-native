#!/usr/bin/env python3
"""Local benchmark for the C# frontend: direct-binary wall time + stage timings.

Unlike benchmarks/frontend-parity/tools/bench.py (which must spawn Windows
PowerShell for every run, adding ~250 ms of shell startup), this script runs the
compiled binary directly, so the numbers isolate process startup + pipeline.
It also verifies each run against the golden IR and writes
results/csharp/local-bench.json.
"""
from __future__ import annotations

import json
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent          # src/frontend/csharp
ROOT = HERE.parents[2]                          # repo root
PARITY = ROOT / "benchmarks" / "frontend-parity"
RESULTS = PARITY / "results" / "csharp"

sys.path.insert(0, str(PARITY / "tools"))
from verify_ir import compare, load  # noqa: E402

FIXTURES = [
    ("contract", PARITY / "fixtures" / "contract" / "input.html", 400, 300,
     PARITY / "golden" / "contract.ir.json"),
    ("flex-page", PARITY / "fixtures" / "flex-page" / "index.html", 480, 320,
     PARITY / "golden" / "flex-page.ir.json"),
]

STAGES = ("parse", "css", "cascade", "layout", "emit", "total")


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

    runs = 10
    warmup = 2
    report: dict = {
        "binary": info["binary"],
        "build_mode": info["build_mode"],
        "binary_bytes": info["binary_bytes"],
        "runs": runs,
        "warmup": warmup,
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

        for _ in range(warmup):
            subprocess.run(args, check=True, capture_output=True, text=True)

        walls: list[float] = []
        stage_values: dict[str, list[float]] = {stage: [] for stage in STAGES}
        ops = 0
        peak_rss_kb = 0
        for _ in range(runs):
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

        report["fixtures"][name] = {
            "status": "FAIL" if errors else "PASS",
            "ops": ops,
            "wall_ms": stats(walls),
            "stages_ms": {stage: stats(values) for stage, values in stage_values.items()},
            "peak_rss_kb": peak_rss_kb,
            "ir": str(out.relative_to(ROOT)).replace("\\", "/"),
        }

    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / "local-bench.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")

    print()
    print(f"binary: {info['binary']} ({info['build_mode']}, {info['binary_bytes']} bytes)")
    print(f"runs per fixture: {runs} (warmup {warmup}), direct binary, wall includes startup")
    print("| fixture | status | wall median | wall min | total median | peak RSS |")
    print("|---|---:|---:|---:|---:|---:|")
    for name, entry in report["fixtures"].items():
        print(f"| {name} | {entry['status']} | {entry['wall_ms']['median']} ms | "
              f"{entry['wall_ms']['min']} ms | {entry['stages_ms']['total']['median']} ms | "
              f"{entry['peak_rss_kb']} KB |")
    print()
    for name, entry in report["fixtures"].items():
        stages = ", ".join(f"{stage}={entry['stages_ms'][stage]['median']}"
                           for stage in STAGES)
        print(f"{name}: {stages}")

    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
