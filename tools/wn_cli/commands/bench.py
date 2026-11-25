import argparse
from pathlib import Path
import sys

from wn_cli.commands import runtime
from wn_cli.core import (
    CliError,
    get_paths,
    get_python_exe,
    log_info,
    log_step,
    run_cmd,
)


def _bench_python(bench_dir: Path) -> Path:
    venv_bin = (
        bench_dir / ".venv" / "Scripts" / "python.exe"
        if sys.platform == "win32"
        else bench_dir / ".venv" / "bin" / "python"
    )
    if venv_bin.is_file():
        return venv_bin
    return get_python_exe()


def _clean_extra_args(raw_args: list[str]) -> list[str]:
    extra = list(raw_args)
    if extra and extra[0] == "--":
        return extra[1:]
    return extra


def cmd_metrics(args: argparse.Namespace) -> int:
    paths = get_paths()
    bench_dir = paths.root / "benchmarks" / "engine-metrics"
    script = bench_dir / "measure.py"
    if not script.is_file():
        raise CliError(f"benchmark script not found: {script}")

    extra = _clean_extra_args(args.extra_args)
    log_step("running engine-metrics benchmark")
    run_cmd([_bench_python(bench_dir), script, *extra], cwd=bench_dir)
    return 0


def cmd_pilot(args: argparse.Namespace) -> int:
    paths = get_paths()
    pilot_dir = paths.root / "benchmarks" / "pilot-media-library"
    if not pilot_dir.is_dir():
        raise CliError(f"pilot directory not found: {pilot_dir}")

    log_step("pilot 1/4: generating deterministic posters")
    run_cmd([_bench_python(pilot_dir), pilot_dir / "make_assets.py"], cwd=pilot_dir)

    if not args.skip_emit:
        log_step("pilot 2/4: emitting pilot UI")
        runtime.cmd_emit(
            argparse.Namespace(
                pilot=True,
                html=None,
                css=None,
                width=0,
                height=0,
                out_dir=None,
                skip_rust=args.skip_rust,
            )
        )

    if not args.skip_build:
        log_step("pilot 3/4: building runtime")
        runtime.cmd_build(
            argparse.Namespace(
                config=args.config,
                clean=False,
                generated_dir=None,
                no_wgpu=args.no_wgpu,
            )
        )

    log_step("pilot 4/4: verifying pilot acceptance")
    runtime.cmd_verify(
        argparse.Namespace(
            config=args.config,
            pilot=True,
            skip_abi=True,
            skip_visual=False,
        )
    )
    if not args.skip_emit or not args.skip_build:
        log_step("pilot: restoring the canonical document")
        runtime.cmd_emit(
            argparse.Namespace(
                pilot=False,
                html=None,
                css=None,
                width=0,
                height=0,
                out_dir=None,
                skip_rust=True,
            )
        )
        runtime.cmd_build(
            argparse.Namespace(
                config=args.config,
                clean=False,
                generated_dir=None,
                no_wgpu=args.no_wgpu,
            )
        )
    log_info("pilot benchmark passed")
    return 0


def cmd_stress(args: argparse.Namespace) -> int:
    paths = get_paths()
    pilot_dir = paths.root / "benchmarks" / "pilot-media-library"
    script = pilot_dir / "stress" / "run_stress.py"
    if not script.is_file():
        raise CliError(f"stress script not found: {script}")

    extra = _clean_extra_args(args.extra_args)
    log_step("running pilot stress benchmark")
    run_cmd([_bench_python(pilot_dir), script, *extra], cwd=pilot_dir)
    return 0


def register_subcommands(subparsers: argparse._SubParsersAction) -> None:
    p_bench = subparsers.add_parser("bench", help="Run performance metrics, pilot, and stress benchmarks")
    bench_sub = p_bench.add_subparsers(dest="bench_cmd", required=True)

    p_metrics = bench_sub.add_parser("metrics", help="Run engine-metrics measurement suite")
    p_metrics.add_argument("extra_args", nargs=argparse.REMAINDER, help="Arguments passed to measure.py")
    p_metrics.set_defaults(func=cmd_metrics)

    p_pilot = bench_sub.add_parser("pilot", help="Emit, build, and verify pilot-media-library")
    p_pilot.add_argument("--config", choices=["Release", "Debug"], default="Release")
    p_pilot.add_argument("--skip-emit", action="store_true", help="Skip UI emission step")
    p_pilot.add_argument("--skip-build", action="store_true", help="Skip CMake build step")
    p_pilot.add_argument("--skip-rust", action="store_true", help="Reuse existing frontend-rust binary")
    p_pilot.add_argument("--no-wgpu", action="store_true",
                         help="Build the runtime with -DWN_WGPU=OFF (D3D11 only)")
    p_pilot.set_defaults(func=cmd_pilot)

    p_stress = bench_sub.add_parser("stress", help="Run pilot stress benchmark suite")
    p_stress.add_argument("extra_args", nargs=argparse.REMAINDER, help="Arguments passed to run_stress.py")
    p_stress.set_defaults(func=cmd_stress)