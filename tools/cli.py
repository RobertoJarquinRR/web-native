import argparse
import subprocess
import sys

from wn_cli.commands import bench, frontend, runtime, setup, test
from wn_cli.core import CliError


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="wn-cli",
        description="WebNative build, verification, and orchestration CLI",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

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