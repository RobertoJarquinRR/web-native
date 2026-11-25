#!/usr/bin/env python3
"""Cross-language frontend benchmark + parity harness.

For every frontend, and every fixture:
  - warm up, then run N times
  - measure wall time (process start + parse + cascade + layout + IR write)
  - verify the produced IR against the golden IR
  - collect internal timings (if the frontend wrote a timings JSON)

Invocation:
  - if <frontend>/BUILD_INFO.json exists and its `binary` exists, run it
    directly (fast path, no shell startup)
  - otherwise fall back to `<frontend>/run.ps1`

Outputs: results/benchmark.json and results/SUMMARY.md
"""
from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
PARITY = TOOLS.parent
ROOT = PARITY.parents[1]

sys.path.insert(0, str(TOOLS))
from verify_ir import compare, load  # noqa: E402

FRONTEND_DIRS = {
    "rust": ROOT / "src" / "frontend",
    
}

def rel(path: Path) -> str:
    """Stable, repo-relative path for committed result JSON."""
    try:
        return str(path.resolve().relative_to(ROOT))
    except ValueError:
        return str(path)


FIXTURES = [
    {
        "name": "contract",
        "html": PARITY / "fixtures" / "contract" / "input.html",
        "width": 400,
        "height": 300,
        "golden": PARITY / "golden" / "contract.ir.json",
    },
    {
        "name": "flex-page",
        "html": PARITY / "fixtures" / "flex-page" / "index.html",
        "width": 480,
        "height": 320,
        "golden": PARITY / "golden" / "flex-page.ir.json",
    },
]


def build_command(frontend_dir: Path, fixture: dict, out: Path, timings: Path) -> tuple[list[str], str]:
    info_path = frontend_dir / "BUILD_INFO.json"
    if info_path.exists():
        try:
            info = json.loads(info_path.read_text(encoding="utf-8-sig"))
        except (OSError, json.JSONDecodeError):
            info = {}
        binary = info.get("binary")
        if binary:
            binary_path = Path(binary)
            if not binary_path.is_absolute():
                binary_path = frontend_dir / binary_path
            if binary_path.exists():
                prefix = [str(item) for item in info.get("args_prefix", [])]
                cmd = [str(binary_path), *prefix]
                cmd += [
                    "--html", str(fixture["html"]),
                    "--width", str(fixture["width"]),
                    "--height", str(fixture["height"]),
                    "--out", str(out),
                    "--timings", str(timings),
                ]
                return cmd, "direct"
    run_ps1 = frontend_dir / "run.ps1"
    cmd = [
        "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", str(run_ps1),
        "-Html", str(fixture["html"]),
        "-Width", str(fixture["width"]),
        "-Height", str(fixture["height"]),
        "-Out", str(out),
        "-Timings", str(timings),
    ]
    return cmd, "run.ps1"


def run_once(frontend_dir: Path, fixture: dict, out: Path, timings: Path) -> tuple[float, str, str]:
    cmd, mode = build_command(frontend_dir, fixture, out, timings)
    started = time.perf_counter()
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=180,
                          cwd=str(frontend_dir))
    elapsed = (time.perf_counter() - started) * 1000.0
    if proc.returncode != 0:
        raise RuntimeError(
            f"{' '.join(cmd)}\nexit {proc.returncode}\n"
            f"stdout:\n{proc.stdout[-2000:]}\nstderr:\n{proc.stderr[-2000:]}"
        )
    return elapsed, mode, (proc.stdout or "") + (proc.stderr or "")


def main() -> int:
    parser = argparse.ArgumentParser(description="Benchmark + verify frontends")
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--only", nargs="*", default=None, help="subset of frontend keys")
    args = parser.parse_args()

    results_dir = PARITY / "results"
    results_dir.mkdir(parents=True, exist_ok=True)

    report: dict = {"runs": args.runs, "fixtures": {}, "frontends": {}}
    rows: list[dict] = []

    for fixture in FIXTURES:
        if not fixture["golden"].exists():
            print(f"skip {fixture['name']}: golden missing: {fixture['golden']}")
            continue
        golden = load(fixture["golden"])
        report["fixtures"][fixture["name"]] = {
            "html": rel(fixture["html"]),
            "viewport": [fixture["width"], fixture["height"]],
            "golden_ops": len(golden.get("ops", [])),
        }

        for key, frontend_dir in FRONTEND_DIRS.items():
            if args.only and key not in args.only:
                continue
            has_direct = (frontend_dir / "BUILD_INFO.json").exists()
            has_script = (frontend_dir / "run.ps1").exists()
            if not (has_direct or has_script):
                print(f"skip {key}: no BUILD_INFO.json and no run.ps1")
                continue

            fdir = results_dir / key
            fdir.mkdir(parents=True, exist_ok=True)
            out = fdir / f"{fixture['name']}.ir.json"
            timings_path = fdir / f"{fixture['name']}.timings.json"

            status = "PASS"
            error = ""
            times: list[float] = []
            internal: dict = {}
            mode = "?"
            try:
                for _ in range(args.warmup):
                    run_once(frontend_dir, fixture, out, timings_path)
                for _ in range(args.runs):
                    elapsed, mode, _ = run_once(frontend_dir, fixture, out, timings_path)
                    times.append(elapsed)
                errors = compare(golden, load(out))
                if errors:
                    status = "FAIL"
                    error = "; ".join(errors[:5])
                if timings_path.exists():
                    internal = json.loads(timings_path.read_text(encoding="utf-8-sig"))
            except Exception as exc:  # noqa: BLE001
                status = "ERROR"
                error = f"{type(exc).__name__}: {exc}"

            entry = {
                "fixture": fixture["name"],
                "status": status,
                "mode": mode,
                "error": error,
                "wall_ms": {
                    "median": round(statistics.median(times), 3) if times else None,
                    "min": round(min(times), 3) if times else None,
                    "max": round(max(times), 3) if times else None,
                },
                "internal_timings_ms": internal,
                "ir": rel(out),
            }
            report["frontends"].setdefault(key, {})[fixture["name"]] = entry
            rows.append({"frontend": key, **entry})
            print(f"{key:8s} {fixture['name']:10s} {status:5s} mode={mode:8s} "
                  f"wall={entry['wall_ms']['median']} ms")

    (results_dir / "benchmark.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )

    lines = [
        "# Frontend parity benchmark",
        "",
        f"Runs per frontend/fixture: {args.runs} (warmup {args.warmup}).",
        "Wall time includes process startup, HTML+CSS parse, cascade, layout and IR write.",
        "`direct` runs the binary; `run.ps1` includes PowerShell startup.",
        "",
        "| Frontend | Fixture | Status | Mode | Wall median (ms) | Wall min | Wall max | Ops |",
        "|---|---|---:|---|---:|---:|---:|---:|",
    ]
    for row in rows:
        wall = row["wall_ms"]
        ops = row["internal_timings_ms"].get("ops", "")
        lines.append(
            f"| {row['frontend']} | {row['fixture']} | {row['status']} | {row['mode']} | "
            f"{wall['median'] if wall['median'] is not None else '-'} | "
            f"{wall['min'] if wall['min'] is not None else '-'} | "
            f"{wall['max'] if wall['max'] is not None else '-'} | {ops} |"
        )
    lines.append("")
    lines.append("Parity is measured against `golden/*.ir.json` with 0.01 px tolerance.")
    (results_dir / "SUMMARY.md").write_text("\n".join(lines) + "\n", encoding="utf-8")

    bad = [row for row in rows if row["status"] != "PASS"]
    print(f"\nwrote {results_dir / 'SUMMARY.md'}")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
