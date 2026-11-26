from __future__ import annotations
import argparse
import json
from pathlib import Path
import re
import shutil

from wn_cli.core import (
    CliError,
    exe_name,
    get_paths,
    get_python_exe,
    log_info,
    log_step,
    run_cmd,
)

FORBIDDEN_HEADER_TOKENS = re.compile(
    r"\b(WGPU|wgpu|ID3D|D3D12|D3D11|HWND|HINSTANCE|IDXGI|Win32)\b|windows\.h"
)


def _resolve_build_dir(config: str) -> Path:
    paths = get_paths()
    return paths.runtime_build_debug if config == "Debug" else paths.runtime_build_release

def cmd_emit(args: argparse.Namespace) -> int:
    paths = get_paths()
    rust_exe = paths.frontend_rust / "target" / "release" / exe_name("frontend-rust")

    if args.html:
        if not args.out_dir:
            raise CliError("--out-dir is required when using --html")
        fixture_html = Path(args.html).resolve()
        fixture_css = Path(args.css).resolve() if args.css else fixture_html.with_suffix(".css")
        out_dir = Path(args.out_dir).resolve()
    elif args.pilot:
        pilot_dir = paths.root / "benchmarks" / "pilot-media-library" / "page"
        fixture_html = pilot_dir / "index.html"
        fixture_css = pilot_dir / "style.css"
        out_dir = Path(args.out_dir).resolve() if args.out_dir else paths.generated
    else:
        page_dir = paths.runtime / "page"
        fixture_html = page_dir / "index.html"
        fixture_css = page_dir / "style.css"
        out_dir = Path(args.out_dir).resolve() if args.out_dir else paths.generated

    if not fixture_html.is_file():
        raise CliError(f"html input not found: {fixture_html}")
    if not paths.emitter.is_file():
        raise CliError(f"emitter script not found: {paths.emitter}")

    if not args.skip_rust:
        log_step("building frontend-rust (release)")
        run_cmd(
            ["cargo", "build", "--release", "--manifest-path", paths.frontend_rust / "Cargo.toml"],
            cwd=paths.root,
        )

    if not rust_exe.is_file():
        raise CliError(f"missing {rust_exe} (run without --skip-rust)")

    width = args.width or (1280 if args.pilot else 960)
    height = args.height or (800 if args.pilot else 640)

    out_dir.mkdir(parents=True, exist_ok=True)
    ui_json = out_dir / "ui.json"
    ui_header = out_dir / "ui_data.h"
    ui_meta = out_dir / "ui_meta.json"
    blob_path = out_dir / "builtin.wnui"
    embed_header = out_dir / "builtin_blob.h"

    log_step("emitting ui tree with frontend-rust")
    rust_cmd: list[str | Path] = [
        rust_exe,
        "--html",
        fixture_html,
        "--width",
        str(width),
        "--height",
        str(height),
        "--emit-ui",
        ui_json,
        "--emit-blob",
        blob_path,
    ]
    if fixture_css.is_file():
        rust_cmd.extend(["--css", fixture_css])
    if args.pilot:
        rust_cmd.append("--pilot")
    run_cmd(rust_cmd, cwd=paths.root)

    if not blob_path.is_file():
        raise CliError(f"frontend-rust did not write {blob_path}")

    log_step("generating C header with emit_ui.py")
    run_cmd(
        [
            get_python_exe(),
            paths.emitter,
            "--ui-json",
            ui_json,
            "--out",
            ui_header,
            "--meta",
            ui_meta,
        ],
        cwd=paths.root,
    )
    log_info(f"generated {ui_header}")

    log_step("embedding the canonical blob for the builtin document")
    embed_script = Path(__file__).resolve().parents[1] / "embed_blob.py"
    run_cmd(
        [
            get_python_exe(),
            embed_script,
            "--blob",
            blob_path,
            "--header",
            embed_header,
        ],
        cwd=paths.root,
    )
    # The SDK artifact keeps its historical name; it is the same canonical blob.
    shutil.copy2(blob_path, out_dir / "app.wnui")
    log_info(f"generated {embed_header} from {blob_path.name}")
    return 0


def cmd_build(args: argparse.Namespace) -> int:
    paths = get_paths()
    build_dir = _resolve_build_dir(args.config)
    generated_dir = Path(args.generated_dir).resolve() if args.generated_dir else paths.generated

    if not (generated_dir / "builtin_blob.h").is_file():
        raise CliError(f"missing {generated_dir / 'builtin_blob.h'} (run: runtime emit)")

    if args.clean and build_dir.exists():
        log_step(f"cleaning {build_dir}")
        shutil.rmtree(build_dir)

    log_step(f"configuring CMake ({args.config})")
    wgpu_flag = "OFF" if getattr(args, "no_wgpu", False) else "ON"
    run_cmd(
        [
            "cmake",
            "-S",
            paths.runtime,
            "-B",
            build_dir,
            "-G",
            "Ninja",
            f"-DCMAKE_BUILD_TYPE={args.config}",
            f"-DWN_GENERATED_DIR={generated_dir}",
            f"-DWN_WGPU={wgpu_flag}",
        ],
        cwd=paths.root,
        use_build_env=True,
    )

    log_step(f"building runtime targets ({args.config})")
    run_cmd(["cmake", "--build", build_dir], cwd=paths.root, use_build_env=True)

    if wgpu_flag == "OFF":
        # A previous ON build leaves this copied DLL behind; OFF must not ship it.
        (build_dir / "wgpu_native.dll").unlink(missing_ok=True)
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    build_dir = _resolve_build_dir(args.config)
    runtime_exe = build_dir / exe_name("runtime_r2")
    if not runtime_exe.is_file():
        raise CliError(f"missing {runtime_exe} (run: runtime build)")

    extra = list(args.extra_args)
    if extra and extra[0] == "--":
        extra = extra[1:]

    log_step(f"running {runtime_exe.name}")
    run_cmd([runtime_exe, *extra], cwd=build_dir)
    return 0


def _check_public_abi_header() -> None:
    paths = get_paths()
    header = paths.root / "include" / "webnative.h"
    if not header.is_file():
        raise CliError(f"public header not found: {header}")

    content = header.read_text(encoding="utf-8")
    if match := FORBIDDEN_HEADER_TOKENS.search(content):
        raise CliError(f"include/webnative.h leaked forbidden token: {match.group(0)}")
    log_info("[ok] include/webnative.h ABI hygiene")


def cmd_verify(args: argparse.Namespace) -> int:
    paths = get_paths()
    build_dir = _resolve_build_dir(args.config)
    runtime_exe = build_dir / exe_name("runtime_r2")
    py_exe = get_python_exe()

    if not args.skip_abi:
        log_step("verifying C ABI contract")
        _check_public_abi_header()
        if (build_dir / "CTestTestfile.cmake").is_file():
            run_cmd(
                ["ctest", "--test-dir", build_dir, "--output-on-failure"],
                cwd=paths.root,
                use_build_env=True,
            )

    if not args.skip_visual:
        if not runtime_exe.is_file():
            raise CliError(f"missing {runtime_exe} (run: runtime build)")

        if args.pilot:
            log_step("verifying pilot media library benchmark")
            pilot_verify = paths.root / "benchmarks" / "pilot-media-library" / "verify.py"
            verify_out = build_dir / "verify_pilot"
            verify_out.mkdir(parents=True, exist_ok=True)
            run_cmd(
                [py_exe, pilot_verify, "--runtime", runtime_exe, "--out", verify_out],
                cwd=paths.root,
            )
            report_file = verify_out / "verify_report.json"
            if report_file.is_file():
                data = json.loads(report_file.read_text(encoding="utf-8"))
                if data.get("result") != "PASS":
                    raise CliError("pilot verification reported FAIL")
        else:
            log_step("generating runtime artifacts for visual verification")
            run_cmd(
                [runtime_exe, "--report", "layout_report.json", "--sizes", "960x640,720x640,560x640"],
                cwd=build_dir,
            )
            for width in (960, 720, 560):
                run_cmd(
                    [runtime_exe, "--render", str(width), "640", f"out_{width}.png"],
                    cwd=build_dir,
                )
            (build_dir / "states").mkdir(parents=True, exist_ok=True)
            run_cmd([runtime_exe, "--states", "states/state"], cwd=build_dir)
            run_cmd(
                [runtime_exe, "--frames", "30", "--resize-demo", "--input-demo"],
                cwd=build_dir,
            )

            log_step("verifying runtime layout, pixels, and states")
            verify_py = paths.runtime / "verify.py"
            run_cmd([py_exe, verify_py, "--dir", build_dir], cwd=paths.root)
            report_file = build_dir / "verify_report.json"
            if report_file.is_file():
                data = json.loads(report_file.read_text(encoding="utf-8"))
                if not data.get("ok", False):
                    raise CliError(
                        f"verify.py failed ({data.get('failed_checks')}/{data.get('total_checks')} failed)"
                    )

    log_info("runtime verification passed")
    return 0


def register_subcommands(subparsers: argparse._SubParsersAction) -> None:
    p_runtime = subparsers.add_parser("runtime", help="Emit UI, build C runtime, and run verification")
    rt_sub = p_runtime.add_subparsers(dest="runtime_cmd", required=True)

    p_emit = rt_sub.add_parser("emit", help="Compile UI into generated/ui_data.h")
    p_emit.add_argument("--pilot", action="store_true", help="Emit canonical pilot-media-library page")
    p_emit.add_argument("--html", type=str, default=None, help="Custom HTML entry file")
    p_emit.add_argument("--css", type=str, default=None, help="Custom CSS stylesheet")
    p_emit.add_argument("--width", type=int, default=0, help="Viewport width override")
    p_emit.add_argument("--height", type=int, default=0, help="Viewport height override")
    p_emit.add_argument("--out-dir", type=str, default=None, help="Custom output directory")
    p_emit.add_argument("--skip-rust", action="store_true", help="Reuse existing frontend-rust binary")
    p_emit.set_defaults(func=cmd_emit)

    p_build = rt_sub.add_parser("build", help="Configure and build C runtime via CMake/Ninja")
    p_build.add_argument("--config", choices=["Release", "Debug"], default="Release")
    p_build.add_argument("--clean", action="store_true", help="Remove build directory before configuring")
    p_build.add_argument("--generated-dir", type=str, default=None, help="Directory containing ui_data.h")
    p_build.add_argument("--no-wgpu", action="store_true",
                         help="Configure with -DWN_WGPU=OFF (D3D11 only, no wgpu-native link)")
    p_build.set_defaults(func=cmd_build)

    p_run = rt_sub.add_parser("run", help="Execute runtime_r2 binary")
    p_run.add_argument("--config", choices=["Release", "Debug"], default="Release")
    p_run.add_argument("extra_args", nargs=argparse.REMAINDER, help="Arguments passed to runtime_r2")
    p_run.set_defaults(func=cmd_run)

    p_verify = rt_sub.add_parser("verify", help="Run ABI hygiene, CTest, and visual acceptance checks")
    p_verify.add_argument("--config", choices=["Release", "Debug"], default="Release")
    p_verify.add_argument("--pilot", action="store_true", help="Verify pilot-media-library fixture")
    p_verify.add_argument("--skip-abi", action="store_true", help="Skip ABI header and CTest checks")
    p_verify.add_argument("--skip-visual", action="store_true", help="Skip visual acceptance script")
    p_verify.set_defaults(func=cmd_verify)