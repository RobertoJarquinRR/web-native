from __future__ import annotations
import argparse
import shutil
import subprocess
import sys

from wn_cli.commands import bench, frontend, runtime, setup, test
from wn_cli.core import CliError, exe_name, get_paths, log_info, log_step, run_cmd

def _stage_master_outputs(config: str, no_wgpu: bool = False) -> None:
    paths = get_paths()
    is_debug = config == "Debug"
    out_dir = paths.out_debug if is_debug else paths.out_release
    rt_build = paths.runtime_build_debug if is_debug else paths.runtime_build_release
    rust_profile = "debug" if is_debug else "release"
    rust_bin = paths.frontend_rust / "target" / rust_profile / exe_name("frontend-rust")

    out_dir.mkdir(parents=True, exist_ok=True)
    if no_wgpu:
        # Never keep a stale wgpu payload in the staged output after an OFF build.
        (out_dir / "wgpu_native.dll").unlink(missing_ok=True)

    if rust_bin.is_file():
        shutil.copy2(rust_bin, out_dir / rust_bin.name)

    runtime_artifacts = [
        exe_name("runtime_r2"),
        exe_name("standalone"),
        exe_name("embedded"),
        "wgpu_native.dll" if sys.platform == "win32" else "libwgpu_native.so",
        "webnative.dll" if sys.platform == "win32" else "libwebnative.so",
        "webnative.lib",
    ]
    for name in runtime_artifacts:
        if no_wgpu and name in ("wgpu_native.dll", "libwgpu_native.so"):
            continue
        src_file = rt_build / name
        if src_file.is_file():
            shutil.copy2(src_file, out_dir / name)

    public_header = paths.root / "include" / "webnative.h"
    if public_header.is_file():
        inc_out = out_dir / "include"
        inc_out.mkdir(parents=True, exist_ok=True)
        shutil.copy2(public_header, inc_out / "webnative.h")


def cmd_root_build(args: argparse.Namespace) -> int:
    paths = get_paths()
    is_debug = args.config == "Debug"
    out_dir = paths.out_debug if is_debug else paths.out_release

    if args.clean and out_dir.exists():
        log_step(f"cleaning staged artifacts in {out_dir}")
        shutil.rmtree(out_dir)

    log_step(f"master 1/3: building frontend-rust ({args.config.lower()})")
    frontend.cmd_build(argparse.Namespace(debug=is_debug))

    if args.emit or not (paths.generated / "builtin_blob.h").is_file():
        log_step("master 2/3: emitting the canonical blob")
        runtime.cmd_emit(
            argparse.Namespace(
                pilot=args.pilot,
                html=None,
                css=None,
                width=0,
                height=0,
                out_dir=None,
                skip_rust=True,
            )
        )

    log_step(f"master 3/3: building runtime ({args.config})")
    runtime.cmd_build(
        argparse.Namespace(
            config=args.config,
            clean=args.clean,
            generated_dir=None,
            no_wgpu=args.no_wgpu,
        )
    )

    _stage_master_outputs(args.config, args.no_wgpu)
    out_dir = paths.out_debug if is_debug else paths.out_release
    log_info(f"staged clean binaries in {out_dir}")
    return 0


def cmd_root_run(args: argparse.Namespace) -> int:
    paths = get_paths()
    out_dir = paths.out_debug if args.config == "Debug" else paths.out_release
    runtime_exe = out_dir / exe_name("runtime_r2")
    if not runtime_exe.is_file():
        raise CliError(f"missing {runtime_exe} (run: wn-cli build)")

    extra = list(args.extra_args)
    if extra and extra[0] == "--":
        extra = extra[1:]

    log_step(f"running {runtime_exe}")
    run_cmd([runtime_exe, *extra], cwd=out_dir)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="wn-cli",
        description="WebNative framework development, verification, and orchestration CLI",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    p_build = subparsers.add_parser(
        "build",
        help="Build frontend and runtime incrementally and stage clean binaries in build/<config>",
    )
    p_build.add_argument("--config", choices=["Release", "Debug"], default="Release")
    p_build.add_argument("--clean", action="store_true", help="Clean CMake cache before building runtime")
    p_build.add_argument("--emit", action="store_true", help="Re-emit UI data header before building runtime")
    p_build.add_argument("--pilot", action="store_true", help="Emit canonical pilot fixture when --emit is set")
    p_build.add_argument("--no-wgpu", action="store_true",
                         help="Configure the runtime with -DWN_WGPU=OFF (D3D11 only)")
    p_build.set_defaults(func=cmd_root_build)

    p_run = subparsers.add_parser("run", help="Run staged runtime_r2 from build/<config>")
    p_run.add_argument("--config", choices=["Release", "Debug"], default="Release")
    p_run.add_argument("extra_args", nargs=argparse.REMAINDER, help="Arguments passed to runtime_r2")
    p_run.set_defaults(func=cmd_root_run)

    setup.register_subcommands(subparsers)
    runtime.register_subcommands(subparsers)
    frontend.register_subcommands(subparsers)
    test.register_subcommands(subparsers)
    bench.register_subcommands(subparsers)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except CliError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as exc:
        print(f"error: command exited with status {exc.returncode}", file=sys.stderr)
        return exc.returncode or 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())