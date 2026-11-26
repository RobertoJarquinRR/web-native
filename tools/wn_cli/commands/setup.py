from __future__ import annotations
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

from wn_cli.core import (
    CliError,
    check_binary,
    get_build_env,
    get_paths,
    log_info,
    log_step,
    log_warn,
)
WGPU_VERSION = "v29.0.1.1"
WGPU_ASSET = "wgpu-windows-x86_64-msvc-release.zip"
WGPU_SHA256 = "7e67d7445c42aeb85e30f88930fd8d7d83ee769e3390aeb1ada75ebf3cf78132"
WGPU_URL = f"https://github.com/gfx-rs/wgpu-native/releases/download/{WGPU_VERSION}/{WGPU_ASSET}"

WGPU_ZIP_MAP: dict[str, str] = {
    "include/webgpu/wgpu.h": "include/wgpu.h",
    "include/webgpu/webgpu.h": "include/webgpu.h",
    "lib/wgpu_native.dll": "bin/wgpu_native.dll",
    "lib/wgpu_native.dll.lib": "lib/wgpu_native.dll.lib",
}


def _compute_sha256(file_path: Path) -> str:
    digest = hashlib.sha256()
    with file_path.open("rb") as fh:
        while chunk := fh.read(65536):
            digest.update(chunk)
    return digest.hexdigest()


def _download_archive(url: str) -> Path:
    req = urllib.request.Request(url, headers={"User-Agent": "wn-cli/1.0"})
    with tempfile.NamedTemporaryFile(suffix=".zip", delete=False) as tmp:
        tmp_path = Path(tmp.name)
        with urllib.request.urlopen(req, timeout=600) as resp:
            shutil.copyfileobj(resp, tmp)
    return tmp_path


def _wgpu_dir() -> Path:
    return get_paths().root / "third_party" / "wgpu-native"


def cmd_wgpu(args: argparse.Namespace) -> int:
    dest = _wgpu_dir()
    version_file = dest / "VERSION.txt"
    expected_version = f"{WGPU_VERSION} {WGPU_ASSET}"

    missing = [rel for rel in WGPU_ZIP_MAP.values() if not (dest / rel).is_file()]
    current_version = version_file.read_text(encoding="utf-8").strip() if version_file.is_file() else ""

    if not args.force and not missing and current_version == expected_version:
        log_info(f"wgpu-native already present ({expected_version})")
        return 0

    temp_zip: Path | None = None
    try:
        if args.zip:
            zip_path = Path(args.zip).resolve()
            if not zip_path.is_file():
                raise CliError(f"archive not found: {zip_path}")
        else:
            log_step(f"downloading {WGPU_ASSET} ({WGPU_VERSION})")
            log_info(f"GET {WGPU_URL}")
            temp_zip = _download_archive(WGPU_URL)
            zip_path = temp_zip
            log_info(f"downloaded {zip_path.stat().st_size:,} bytes")

        actual_sha = _compute_sha256(zip_path)
        if actual_sha != WGPU_SHA256:
            raise CliError(
                f"sha256 mismatch for {zip_path}\n"
                f"  expected: {WGPU_SHA256}\n"
                f"  actual:   {actual_sha}"
            )
        log_info(f"sha256 ok ({actual_sha})")

        log_step(f"extracting artifacts to {dest}")
        extracted = 0
        with zipfile.ZipFile(zip_path, "r") as zf:
            for member in zf.infolist():
                rel_target = WGPU_ZIP_MAP.get(member.filename)
                if not rel_target:
                    continue
                target_path = dest / rel_target
                target_path.parent.mkdir(parents=True, exist_ok=True)
                target_path.write_bytes(zf.read(member.filename))
                log_info(f"{member.filename} -> {rel_target}")
                extracted += 1

        if extracted != len(WGPU_ZIP_MAP):
            raise CliError(f"archive missing expected entries ({extracted}/{len(WGPU_ZIP_MAP)} extracted)")

        still_missing = [rel for rel in WGPU_ZIP_MAP.values() if not (dest / rel).is_file()]
        if still_missing:
            raise CliError(f"missing files after extraction: {', '.join(still_missing)}")

        dest.mkdir(parents=True, exist_ok=True)
        version_file.write_text(f"{expected_version}\n", encoding="ascii")
        log_info(f"ready ({expected_version})")
        return 0
    finally:
        if temp_zip and temp_zip.is_file():
            temp_zip.unlink()


def _probe_version(binary: str, env: dict[str, str]) -> str:
    try:
        proc = subprocess.run(
            [binary, "--version"],
            capture_output=True,
            text=True,
            env=env,
            timeout=10,
        )
        output = (proc.stdout or proc.stderr).strip()
        return output.splitlines()[0] if output else "available"
    except Exception:
        return "available"


def cmd_doctor(_args: argparse.Namespace) -> int:
    paths = get_paths()
    build_env = get_build_env()
    log_step("checking toolchain")

    required_tools = ["python", "cmake", "ninja", "cargo", "rustc"]
    if sys.platform == "win32":
        required_tools.append("cl")

    has_errors = False
    for tool in required_tools:
        resolved = check_binary(tool, env=build_env)
        if resolved:
            ver = _probe_version(resolved, env=build_env)
            log_info(f"[ok]   {tool:<8} {ver}")
        else:
            log_warn(f"[miss] {tool:<8} not found in PATH")
            has_errors = True

    log_step("checking dependencies & build state")
    missing_wgpu = [rel for rel in WGPU_ZIP_MAP.values() if not (_wgpu_dir() / rel).is_file()]
    if not missing_wgpu:
        log_info("[ok]   wgpu-native artifacts present")
    else:
        log_warn(f"[miss] wgpu-native missing: {', '.join(missing_wgpu)} (run: setup wgpu)")
        has_errors = True

    checks = [
        ("generated/builtin.wnui", (paths.generated / "builtin.wnui").is_file()),
        ("generated/builtin_blob.h", (paths.generated / "builtin_blob.h").is_file()),
        ("build_release/build.ninja", (paths.runtime_build_release / "build.ninja").is_file()),
        ("build_debug/build.ninja", (paths.runtime_build_debug / "build.ninja").is_file()),
    ]
    for label, ok in checks:
        status = "[ok]  " if ok else "[warn]"
        log_info(f"{status} {label}")

    if has_errors:
        raise CliError("environment check failed")

    log_info("environment ready")
    return 0

def register_subcommands(subparsers: argparse._SubParsersAction) -> None:
    p_setup = subparsers.add_parser("setup", help="Provision dependencies and inspect environment")
    setup_sub = p_setup.add_subparsers(dest="setup_cmd", required=True)

    p_wgpu = setup_sub.add_parser("wgpu", help="Download and verify pinned wgpu-native release")
    p_wgpu.add_argument("--force", action="store_true", help="Overwrite existing wgpu-native artifacts")
    p_wgpu.add_argument("--zip", type=str, default=None, help="Install from a local release zip")
    p_wgpu.set_defaults(func=cmd_wgpu)

    p_doctor = setup_sub.add_parser("doctor", help="Verify toolchain and dependency readiness")
    p_doctor.set_defaults(func=cmd_doctor)