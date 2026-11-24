from collections.abc import Sequence
from dataclasses import dataclass
from functools import lru_cache
import os
from pathlib import Path
import shutil
import subprocess
import sys


class CliError(Exception):
    """Controlled domain, configuration, or environment error."""

def get_python_exe() -> Path:
    paths = get_paths()
    candidates = [
        paths.root / ".venv" / "Scripts" / "python.exe",
        paths.root / ".venv" / "bin" / "python",
        paths.root / "archive" / "frontends-legacy" / "python" / ".venv" / "Scripts" / "python.exe",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return Path(sys.executable)

@dataclass(frozen=True)
class RepoPaths:
    root: Path
    runtime: Path
    frontend_rust: Path
    emitter: Path
    third_party_wgpu: Path
    build_release: Path
    build_debug: Path
    generated: Path


def get_paths() -> RepoPaths:
    root = Path(__file__).resolve().parents[2]
    runtime = root / "src" / "runtime"
    return RepoPaths(
        root=root,
        runtime=runtime,
        frontend_rust=root / "src" / "frontend" / "rust",
        emitter=root / "src" / "emitter" / "python" / "emit_ui.py",
        third_party_wgpu=root / "third_party" / "wgpu-native",
        build_release=runtime / "build_cmake",
        build_debug=runtime / "build_debug",
        generated=runtime / "generated",
    )


def exe_name(base: str) -> str:
    """Append .exe suffix on Windows platforms."""
    return f"{base}.exe" if sys.platform == "win32" else base


@lru_cache(maxsize=1)
def get_build_env() -> dict[str, str]:
    base_env = os.environ.copy()
    if sys.platform != "win32" or shutil.which("cl", path=base_env.get("PATH")):
        return base_env

    prog_files_x86 = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
    vswhere = prog_files_x86 / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.is_file():
        return base_env

    probe = subprocess.run(
        [
            str(vswhere),
            "-latest",
            "-products",
            "*",
            "-prerelease",
            "-property",
            "installationPath",
        ],
        capture_output=True,
        text=True,
    )
    install_path = probe.stdout.strip()
    if not install_path:
        return base_env

    vcvars = Path(install_path) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    if not vcvars.is_file():
        return base_env

    dump = subprocess.run(
        f'cmd.exe /d /s /c "call "{vcvars}" >nul && set"',
        capture_output=True,
        text=True,
    )
    if dump.returncode == 0:
        for line in dump.stdout.splitlines():
            if "=" in line:
                key, _, value = line.partition("=")
                base_env[key.upper()] = value

    return base_env
def check_binary(name: str, env: dict[str, str] | None = None) -> str | None:
    """Locate an executable in the system or build PATH."""
    search_path = env.get("PATH") if env else None
    return shutil.which(name, path=search_path)


def log_step(message: str) -> None:
    """Print a major execution phase header to stdout."""
    print(f"==> {message}", flush=True)


def log_info(message: str) -> None:
    """Print an indented informational line to stdout."""
    print(f"  {message}", flush=True)


def log_warn(message: str) -> None:
    """Print a warning message to stderr following CLI conventions."""
    print(f"warn: {message}", file=sys.stderr, flush=True)


def run_cmd(
    cmd: Sequence[str | Path],
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    use_build_env: bool = False,
) -> None:
    """
    Execute an external subprocess safely across platforms.
    Raises subprocess.CalledProcessError on non-zero exit codes.
    """
    merged_env = get_build_env().copy() if use_build_env else os.environ.copy()
    if env:
        merged_env.update(env)

    argv = [str(item) for item in cmd]
    log_info(f"$ {' '.join(argv)}")
    subprocess.run(argv, cwd=cwd, env=merged_env, check=True)