#!/usr/bin/env python3
"""Render the native R1/R2 offscreen PNG for a visual-parity fixture.

R1 (rect-only fixtures): src/build.ps1 bakes the display list and compiles
render_png.exe inside benchmarks/visual-parity/out/build/<fixture>. The PNG is
produced by that offscreen executable; no window is captured.

R2 (pilot): runtime_r2.exe --render W H out.png from the existing pilot build.
The default is to reuse the binary; --rebuild opts into the canonical
build.ps1 -Pilot -SkipRustBuild and records the exact build error if the
concurrent D3D11/E1 work makes it fail, then falls back to the existing exe.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path
from typing import Any

import common as c


class NativeError(RuntimeError):
    """Native renderer missing, build failed or output invalid."""


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", help="fixture name from fixtures.json")
    parser.add_argument("--fixtures", nargs="*", default=None, help="fixture names for --check")
    parser.add_argument("--fixtures-json", default=None)
    parser.add_argument("--out", required=True, help="PNG output path (render) or output dir (--check)")
    parser.add_argument("--meta", default=None, help="metadata JSON output path")
    parser.add_argument("--check", action="store_true", help="verify prerequisites without rendering")
    parser.add_argument("--rebuild", action="store_true", help="rebuild the native renderer first")
    parser.add_argument("--timeout", type=int, default=600, help="build timeout in seconds")
    return parser.parse_args(argv)


def _repo() -> Path:
    return c.repo_root()


def _is_file(path: Path | None) -> bool:
    return bool(path) and path.exists() and path.is_file()


def r2_build_dir(out_dir: Path, fixture_name: str) -> Path:
    return out_dir / "build" / f"r2-{fixture_name}"


def _r2_page_sources() -> list[Path]:
    root = _repo()
    runtime = root / "src" / "runtime" / "c-r2"
    return [
        root / "src" / "frontend" / "rust" / "target" / "release" / "frontend-rust.exe",
        runtime / "build.ps1",
        runtime / "runtime_r2.c",
        runtime / "wn_core.c",
        runtime / "wn_core.h",
        runtime / "wn_common.c",
    ]


def _r2_page_is_newer(render_exe: Path, fixture: dict[str, Any]) -> bool:
    if not _is_file(render_exe):
        return True
    exe_mtime = render_exe.stat().st_mtime
    sources = [Path(fixture["html_path"])]
    if fixture.get("style_path"):
        sources.append(Path(fixture["style_path"]))
    sources.extend(_r2_page_sources())
    return any(path.exists() and path.stat().st_mtime > exe_mtime for path in sources)


def build_r2_page(
    fixture: dict[str, Any],
    out_dir: Path,
    width: int,
    height: int,
    timeout: int,
) -> dict[str, Any]:
    """Compile one fixture into its own throw-away runtime (no R1, no prebuilt exe)."""
    root = _repo()
    build_dir = r2_build_dir(out_dir, fixture["name"])
    exe = build_dir / "runtime_r2.exe"
    if not fixture.get("style_path"):
        raise NativeError(f"{fixture['name']}: r2 mode needs a style path")
    command = [
        "powershell.exe",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(root / "src" / "runtime" / "c-r2" / "build.ps1"),
        "-Html",
        str(Path(fixture["html_path"])),
        "-Css",
        str(Path(fixture["style_path"])),
        "-Width",
        str(int(width)),
        "-Height",
        str(int(height)),
        "-OutDir",
        str(build_dir),
        "-SkipRustBuild",
    ]
    started = time.perf_counter()
    completed = c.run_command(command, cwd=root, timeout=timeout)
    duration_ms = round((time.perf_counter() - started) * 1000.0, 2)
    log_path = out_dir / "logs" / f"{fixture['name']}.r2-build.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    c.write_text(
        log_path,
        "$ " + " ".join(command) + "\n\n[stdout]\n" + completed.stdout + "\n[stderr]\n" + completed.stderr,
    )
    info: dict[str, Any] = {
        "attempted": True,
        "ok": completed.returncode == 0 and _is_file(exe),
        "exit_code": completed.returncode,
        "duration_ms": duration_ms,
        "command": command,
        "log": str(log_path),
        "build_dir": str(build_dir),
        "stdout_tail": c.tail(completed.stdout, 20),
        "stderr_tail": c.tail(completed.stderr, 40),
    }
    if not info["ok"]:
        raise NativeError(
            f"r2 build failed for {fixture['name']} (exit {completed.returncode}). Log: {log_path}\n"
            + "\n".join(c.tail(completed.stderr or completed.stdout, 30))
        )
    return info


def r2_candidates(fixture: dict[str, Any], override: str | None = None) -> list[Path]:
    candidates: list[Path] = []
    if override:
        candidates.append(c.resolve_repo_path(override))
    native = fixture.get("native", {})
    if native.get("exe_path"):
        candidates.append(Path(native["exe_path"]))
    env_exe = os.environ.get("WEBNATIVE_R2_EXE")
    if env_exe:
        candidates.append(c.resolve_repo_path(env_exe))
    root = _repo()
    candidates.extend(
        [
            root / "src" / "runtime" / "c-r2" / "build_pilot" / "runtime_r2.exe",
            root / "src" / "runtime" / "c-r2" / "build" / "runtime_r2.exe",
            root / "benchmarks" / "engine-metrics" / "snapshot" / "runtime_r2.exe",
        ]
    )
    unique: list[Path] = []
    seen: set[str] = set()
    for path in candidates:
        key = str(path).lower()
        if key not in seen:
            seen.add(key)
            unique.append(path)
    return unique


def find_r2_exe(fixture: dict[str, Any], override: str | None = None) -> Path | None:
    for candidate in r2_candidates(fixture, override):
        if _is_file(candidate):
            return candidate
    return None


def _fixture_selection(args: argparse.Namespace) -> list[dict[str, Any]]:
    _, fixtures = c.load_fixtures(args.fixtures_json)
    mapping = c.fixture_map(fixtures)
    if args.fixture:
        if args.fixture not in mapping:
            raise ValueError(f"unknown fixture {args.fixture!r}; known: {', '.join(mapping)}")
        return [mapping[args.fixture]]
    if args.fixtures:
        names = c.split_names(args.fixtures)
        unknown = [name for name in names if name not in mapping]
        if unknown:
            raise ValueError(f"unknown fixtures: {', '.join(unknown)}")
        return [mapping[name] for name in names]
    return fixtures


def check(args: argparse.Namespace, out_dir: Path) -> int:
    selected = _fixture_selection(args)
    entries: list[dict[str, Any]] = []
    ok = True
    for fixture in selected:
        c.validate_no_reasonless_excludes(fixture)
        mode = str(fixture.get("native", {}).get("mode", "r1")).lower()
        entry: dict[str, Any] = {"fixture": fixture["name"], "mode": mode}
        if mode == "r2":
            build_dir = r2_build_dir(out_dir, fixture["name"])
            exe = build_dir / "runtime_r2.exe"
            entry["build_dir"] = str(build_dir)
            entry["exe"] = str(exe)
            missing = {
                "build.ps1": _repo() / "src" / "runtime" / "c-r2" / "build.ps1",
                "frontend-rust": _repo() / "src" / "frontend" / "rust" / "target" / "release" / "frontend-rust.exe",
            }
            entry["missing"] = {name: str(path) for name, path in missing.items() if not path.exists()}
            entry["cached"] = _is_file(exe)
            entry["ok"] = not entry["missing"]
            if not entry["ok"]:
                entry["error"] = "r2 build prerequisites missing"
        elif mode == "r1":
            entry["ok"] = False
            entry["error"] = "r1 retired (archive/r1-static); use native.mode = 'r2'"
        else:
            entry["ok"] = False
            entry["error"] = f"unknown native mode {mode!r}"
        ok = ok and bool(entry.get("ok"))
        entries.append(entry)

    result = {
        "ok": ok,
        "generated_at": c.utc_now(),
        "fixtures": entries,
        "git": c.git_info(),
    }
    meta_path = Path(args.meta) if args.meta else out_dir / "meta" / "native.json"
    c.write_json(meta_path, result)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if not ok:
        print("NATIVE CHECK FAILED:", file=sys.stderr)
        for entry in entries:
            if not entry.get("ok"):
                print(f"  {entry['fixture']} ({entry['mode']}): {entry.get('error') or entry.get('missing')}", file=sys.stderr)
        return 1
    return 0


def render(args: argparse.Namespace, out_path: Path, meta_path: Path) -> int:
    fixture = _fixture_selection(args)[0]
    c.validate_no_reasonless_excludes(fixture)
    width = int(fixture["viewport"]["width"])
    height = int(fixture["viewport"]["height"])
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_root = c.output_root(out_path)
    mode = str(fixture.get("native", {}).get("mode", "r1")).lower()
    started = time.perf_counter()
    r2_build_info: dict[str, Any] | None = None

    if mode == "r1":
        raise NativeError(
            f"{fixture['name']}: native mode 'r1' was retired; R1 is archived in "
            "archive/r1-static. Set native.mode to 'r2'."
        )
    elif mode == "r2":
        override = os.environ.get("WEBNATIVE_R2_EXE") or fixture.get("native", {}).get("exe_path")
        if override:
            exe = find_r2_exe(fixture, override)
            if exe is None:
                raise NativeError(f"r2 override executable not found: {override}")
        else:
            build_dir = r2_build_dir(out_root, fixture["name"])
            exe = build_dir / "runtime_r2.exe"
            if args.rebuild or _r2_page_is_newer(exe, fixture):
                r2_build_info = build_r2_page(fixture, out_root, width, height, args.timeout)
        cwd = Path(fixture["html_path"]).parent
        command = [str(exe), "--render", str(width), str(height), str(out_path)]
        completed = c.run_command(command, cwd=cwd, timeout=args.timeout)
        build_info = None
    else:
        raise NativeError(f"unknown native mode {mode!r}")

    duration_ms = round((time.perf_counter() - started) * 1000.0, 2)
    log_path = out_root / "logs" / f"{fixture['name']}.{mode}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    c.write_text(
        log_path,
        "$ " + " ".join(command) + "\n\n[stdout]\n" + completed.stdout + "\n[stderr]\n" + completed.stderr,
    )

    if completed.returncode != 0:
        raise NativeError(
            f"{mode} render failed with exit {completed.returncode}. Log: {log_path}\n"
            + "\n".join(c.tail(completed.stderr or completed.stdout, 30))
        )
    if not out_path.exists():
        raise NativeError(f"{mode} render exited 0 but {out_path} was not created")
    actual_width, actual_height, image_mode = c.image_size(out_path)
    if (actual_width, actual_height) != (width, height):
        out_path.unlink(missing_ok=True)
        raise NativeError(
            f"native PNG size mismatch: expected {width}x{height}, got "
            f"{actual_width}x{actual_height}"
        )

    combined = (completed.stdout or "") + "\n" + (completed.stderr or "")
    adapter = c.parse_adapter_output(combined)
    render_info = c.parse_render_line(completed.stdout or "")
    exe_sha = c.cached_sha256(exe, out_root / ".cache")
    meta = {
        "ok": True,
        "kind": "native",
        "generated_at": c.utc_now(),
        "fixture": fixture["name"],
        "mode": mode,
        "html": str(Path(fixture["html_path"])),
        "html_sha256": c.sha256_file(fixture["html_path"]),
        "style_sha256": (
            c.sha256_file(fixture["style_path"]) if fixture.get("style_path") else None
        ),
        "viewport": {"width": width, "height": height},
        "dpi": c.DEFAULT_DPI,
        "device_scale_factor": 1,
        "color_profile": "srgb",
        "command": command,
        "cwd": str(cwd),
        "exe": str(exe),
        "exe_sha256": exe_sha,
        "exe_size": exe.stat().st_size,
        "exe_mtime": exe.stat().st_mtime,
        "adapter": adapter,
        "render": render_info,
        "build": build_info,
        "r2_build": r2_build_info,
        "build_policy": (
            "reused existing build_pilot binary; pass --rebuild to run "
            "build.ps1 -Pilot -SkipRustBuild"
            if mode == "r2" and not args.rebuild
            else "rebuilt in this run"
            if args.rebuild
            else "reused cached R1 renderer"
        ),
        "stdout_tail": c.tail(completed.stdout, 40),
        "stderr_tail": c.tail(completed.stderr, 40),
        "log": str(log_path),
        "png": {
            "path": str(out_path),
            "sha256": c.sha256_file(out_path),
            "width": actual_width,
            "height": actual_height,
            "mode": image_mode,
            "bytes": out_path.stat().st_size,
        },
        "duration_ms": duration_ms,
    }
    c.write_json(meta_path, meta)
    if r2_build_info and not r2_build_info.get("ok"):
        print(
            "native: WARNING R2 rebuild failed; reusing the existing runtime_r2.exe. "
            "Exact build error is in the native metadata / r2-build log.",
            file=sys.stderr,
        )
    backend = adapter.get("backend", "unknown")
    device = adapter.get("device", "unknown")
    print(
        f"native: {fixture['name']} {actual_width}x{actual_height} mode={mode} "
        f"backend={backend} device={device} -> {out_path}"
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.fixtures_json is None:
        args.fixtures_json = str(c.harness_dir() / "fixtures.json")
    out = Path(args.out)
    if not out.is_absolute():
        out = (Path.cwd() / out).resolve()
    try:
        if args.check:
            return check(args, out)
        out_path = out
        meta_path = Path(args.meta) if args.meta else (
            c.output_root(out_path) / "meta" / f"{args.fixture}.native.json"
        )
        if not meta_path.is_absolute():
            meta_path = (Path.cwd() / meta_path).resolve()
        return render(args, out_path, meta_path)
    except NativeError as exc:
        print(f"NATIVE RENDER FAILED: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:
        print(f"NATIVE RENDER FAILED: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
