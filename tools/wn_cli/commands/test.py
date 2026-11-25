import argparse

from wn_cli import suites
from wn_cli.commands import frontend, runtime
from wn_cli.core import log_info, log_step


def cmd_all(args: argparse.Namespace) -> int:
    suites.set_wgpu_enabled(not args.no_wgpu)
    log_step("suite 1/9: frontend-rust tests")
    frontend.cmd_test(argparse.Namespace())

    log_step("suite 2/9: runtime ABI and verification")
    runtime.cmd_verify(
        argparse.Namespace(
            config=args.config,
            pilot=False,
            skip_abi=False,
            skip_visual=args.skip_visual,
        )
    )

    log_step("suite 3/9: css core v1")
    suites.run_suite("css")

    log_step("suite 4/9: e1 fidelity")
    suites.run_suite("fidelity")

    log_step("suite 5/9: dynamic UI")
    suites.run_suite("dynamic")

    log_step("suite 6/9: text input")
    suites.run_suite("input")

    log_step("suite 7/9: offscreen vtable")
    suites.run_suite("offscreen")

    log_step("suite 8/9: runtime text")
    suites.run_suite("text")

    log_step("suite 9/9: blob v2")
    suites.run_suite("blob")

    log_info("all test suites passed")
    return 0


def cmd_suite(args: argparse.Namespace) -> int:
    suites.set_wgpu_enabled(not args.no_wgpu)
    return suites.run_suite(args.name)



def cmd_unit(args: argparse.Namespace) -> int:
    log_step("unit suite 1/2: frontend-rust tests")
    frontend.cmd_test(argparse.Namespace())

    log_step("unit suite 2/2: runtime ABI & CTest")
    runtime.cmd_verify(
        argparse.Namespace(
            config=args.config,
            pilot=False,
            skip_abi=False,
            skip_visual=True,
        )
    )
    log_info("unit test suites passed")
    return 0


def cmd_abi(args: argparse.Namespace) -> int:
    return runtime.cmd_verify(
        argparse.Namespace(
            config=args.config,
            pilot=False,
            skip_abi=False,
            skip_visual=True,
        )
    )


def register_subcommands(subparsers: argparse._SubParsersAction) -> None:
    p_test = subparsers.add_parser("test", help="Run unified repository test suites")
    test_sub = p_test.add_subparsers(dest="test_cmd", required=True)

    p_all = test_sub.add_parser("all", help="Run all frontend, ABI, and runtime verification suites")
    p_all.add_argument("--config", choices=["Release", "Debug"], default="Release")
    p_all.add_argument("--skip-visual", action="store_true", help="Skip visual pixel verification")
    p_all.add_argument("--no-wgpu", action="store_true",
                       help="Skip the wgpu-native backend (must match the build switch)")
    p_all.set_defaults(func=cmd_all)

    p_unit = test_sub.add_parser("unit", help="Run fast headless tests (cargo test + ctest)")
    p_unit.add_argument("--config", choices=["Release", "Debug"], default="Release")
    p_unit.set_defaults(func=cmd_unit)

    p_abi = test_sub.add_parser("abi", help="Run C ABI header and CTest checks only")
    p_abi.add_argument("--config", choices=["Release", "Debug"], default="Release")
    p_abi.set_defaults(func=cmd_abi)

    p_suite = test_sub.add_parser("suite", help="Run one runtime fixture suite")
    p_suite.add_argument("name", choices=sorted(suites.SUITES))
    p_suite.add_argument("--no-wgpu", action="store_true",
                         help="Skip the wgpu-native backend (must match the build switch)")
    p_suite.set_defaults(func=cmd_suite)
