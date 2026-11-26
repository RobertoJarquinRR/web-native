from __future__ import annotations
import argparse
from pathlib import Path

from wn_cli.core import (
    CliError,
    exe_name,
    get_paths,
    log_step,
    run_cmd,
)


def _manifest_path() -> Path:
    manifest = get_paths().frontend_rust / "Cargo.toml"
    if not manifest.is_file():
        raise CliError(f"Cargo.toml not found: {manifest}")
    return manifest


def cmd_build(args: argparse.Namespace) -> int:
    manifest = _manifest_path()
    profile = "debug" if args.debug else "release"
    log_step(f"building frontend-rust ({profile})")

    cmd: list[str | Path] = ["cargo", "build", "--manifest-path", manifest]
    if not args.debug:
        cmd.append("--release")

    run_cmd(cmd, cwd=get_paths().root)
    return 0


def cmd_test(_args: argparse.Namespace) -> int:
    manifest = _manifest_path()
    log_step("running frontend-rust tests")
    run_cmd(["cargo", "test", "--manifest-path", manifest], cwd=get_paths().root)
    return 0


def cmd_check(args: argparse.Namespace) -> int:
    manifest = _manifest_path()
    log_step("checking frontend-rust")
    run_cmd(["cargo", "check", "--manifest-path", manifest], cwd=get_paths().root)
    if not args.no_clippy:
        run_cmd(
            ["cargo", "clippy", "--manifest-path", manifest, "--", "-D", "warnings"],
            cwd=get_paths().root,
        )
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    paths = get_paths()
    profile = "debug" if args.debug else "release"
    binary = paths.frontend_rust / "target" / profile / exe_name("frontend-rust")
    if not binary.is_file():
        raise CliError(f"missing {binary} (run: frontend build{' --debug' if args.debug else ''})")

    extra = list(args.extra_args)
    if extra and extra[0] == "--":
        extra = extra[1:]

    log_step(f"running {binary.name} ({profile})")
    run_cmd([binary, *extra], cwd=paths.root)
    return 0


def register_subcommands(subparsers: argparse._SubParsersAction) -> None:
    p_frontend = subparsers.add_parser("frontend", help="Build, check, test, and run Rust frontend")
    fe_sub = p_frontend.add_subparsers(dest="frontend_cmd", required=True)

    p_build = fe_sub.add_parser("build", help="Compile frontend-rust binary")
    p_build.add_argument("--debug", action="store_true", help="Build debug profile instead of release")
    p_build.set_defaults(func=cmd_build)

    p_test = fe_sub.add_parser("test", help="Run cargo test for frontend-rust")
    p_test.set_defaults(func=cmd_test)

    p_check = fe_sub.add_parser("check", help="Run cargo check and clippy lints")
    p_check.add_argument("--no-clippy", action="store_true", help="Skip cargo clippy")
    p_check.set_defaults(func=cmd_check)

    p_run = fe_sub.add_parser("run", help="Execute frontend-rust binary")
    p_run.add_argument("--debug", action="store_true", help="Run debug binary")
    p_run.add_argument("extra_args", nargs=argparse.REMAINDER, help="Arguments passed to frontend-rust")
    p_run.set_defaults(func=cmd_run)