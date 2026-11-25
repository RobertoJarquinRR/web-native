from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import Sequence

from .core import CliError, check_binary, get_build_env, get_paths, get_python_exe, log_info, log_step, run_cmd

RUNTIME_SOURCES = (
    "runtime_r2.c",
    "wn_core.c",
    "wn_common.c",
    "wn_view.c",
    "wn_app.c",
    "renderer_wgpu.c",
    "renderer_d3d11.c",
    "host_win32.c",
)
CORE_SOURCES = ("wn_core.c", "wn_common.c")
RUNTIME_LIBS = (
    "d3d11.lib",
    "dxgi.lib",
    "d3dcompiler.lib",
    "windowscodecs.lib",
    "ole32.lib",
    "user32.lib",
    "gdi32.lib",
)
CFLAGS = ("/nologo", "/O2", "/W4", "/WX", "/std:c17", "/TC", "/D_CRT_SECURE_NO_WARNINGS", "/DWN_STATIC")


def _suite_dir(name: str) -> Path:
    return get_paths().runtime / "tests" / name


def _wgpu_dir() -> Path:
    return get_paths().root / "third_party" / "wgpu-native"


def _compile(
    exe: Path,
    sources: Sequence[Path],
    includes: Sequence[Path],
    libs: Sequence[str] = (),
) -> None:
    exe.parent.mkdir(parents=True, exist_ok=True)
    cl = check_binary("cl", get_build_env())
    if not cl:
        raise CliError("cl.exe not found; run wn-cli setup to provision the MSVC toolchain")
    command = [cl, *CFLAGS, *[str(item) for item in sources]]
    command += [f"/I{path}" for path in includes]
    command += [f"/Fo{exe.parent}{os.sep}", f"/Fe{exe}"]
    if libs:
        command += ["/link", *libs]
    run_cmd(command, cwd=exe.parent, use_build_env=True)
    if not exe.is_file():
        raise CliError(f"compiler did not produce {exe}")


def _run(exe: Path, arguments: Sequence[str], cwd: Path, env: dict[str, str] | None = None) -> str:
    merged = None
    if env:
        merged = dict(os.environ)
        merged.update(env)
    completed = subprocess.run(
        [str(exe), *arguments],
        cwd=cwd,
        env=merged,
        capture_output=True,
        text=True,
    )
    for line in (completed.stdout + completed.stderr).splitlines():
        if line.strip():
            log_info(line.rstrip())
    if completed.returncode != 0:
        raise CliError(f"{exe.name} exited {completed.returncode}")
    return completed.stdout


def _python(script: Path, arguments: Sequence[str], cwd: Path) -> str:
    completed = subprocess.run(
        [str(get_python_exe()), str(script), *arguments],
        cwd=cwd,
        capture_output=True,
        text=True,
    )
    for line in (completed.stdout + completed.stderr).splitlines():
        if line.strip():
            log_info(line.rstrip())
    if completed.returncode != 0:
        raise CliError(f"{script.name} exited {completed.returncode}")
    return completed.stdout


def _emit_ui_json(ui_json: Path, out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    _python(
        get_paths().emitter,
        [
            "--ui-json", str(ui_json),
            "--out", str(out_dir / "ui_data.h"),
            "--meta", str(out_dir / "ui_meta.json"),
        ],
        cwd=get_paths().root,
    )


def run_css() -> int:
    suite = _suite_dir("css")
    out = suite / "out"
    page = suite / "page"
    out.mkdir(parents=True, exist_ok=True)

    log_step("css: deterministic poster")
    _python(suite / "make_images.py", [], cwd=suite)

    for name in ("card", "control", "none"):
        log_step(f"css: emit {name} ui_data.h")
        _emit_ui_json(page / f"{name}.json", out / "emit" / name)

    log_step("css: legacy header self-compare")
    legacy = suite / "fixture" / "legacy_ui_data.h"
    _python(get_paths().emitter, ["--compare", str(legacy), str(legacy)], cwd=suite)

    runtime = get_paths().runtime
    for name in ("card", "control"):
        log_step(f"css: compile runtime_css_{name}.exe")
        emit_dir = out / "emit" / name
        _compile(
            out / "build" / name / f"runtime_css_{name}.exe",
            [runtime / source for source in RUNTIME_SOURCES],
            [get_paths().root / "include", runtime, _wgpu_dir() / "include", emit_dir],
            [str(_wgpu_dir() / "lib" / "wgpu_native.dll.lib"), *RUNTIME_LIBS],
        )
        run_cmd(["cmd", "/d", "/c", "copy", "/Y",
                 str(_wgpu_dir() / "bin" / "wgpu_native.dll"),
                 str(out / "build" / name / "wgpu_native.dll")])

    backends = {"WN_RENDERER": "wgpu"}
    card_exe = out / "build" / "card" / "runtime_css_card.exe"
    control_exe = out / "build" / "control" / "runtime_css_control.exe"

    log_step("css: layout report + offscreen renders")
    _run(card_exe, ["--report", str(out / "layout_card.json"), "--sizes", "400x240"], page, backends)
    _run(card_exe, ["--render", "400", "240", str(out / "render_card.png")], page, backends)
    _run(control_exe, ["--render", "400", "240", str(out / "render_control.png")], page, backends)

    log_step("css: verify")
    _python(suite / "verify_css.py", ["--dir", str(out), "--page", str(page)], cwd=suite)
    log_info("css core v1: PASS")
    return 0


def run_fidelity() -> int:
    suite = _suite_dir("fidelity")
    out = suite / "out"
    out.mkdir(parents=True, exist_ok=True)
    rust = get_paths().frontend_rust
    rust_exe = rust / "target" / "release" / "frontend-rust.exe"

    if not rust_exe.is_file():
        log_step("fidelity: cargo build --release")
        run_cmd(["cargo", "build", "--release"], cwd=rust)

    log_step("fidelity: frontend-rust --emit-ui")
    run_cmd(
        [rust_exe, "--html", "page/index.html", "--width", "480", "--height", "560",
         "--emit-ui", str(out / "ui.json"), "--pilot"],
        cwd=suite,
    )

    log_step("fidelity: emit ui_data.h")
    _emit_ui_json(out / "ui.json", out / "emit")
    header = (out / "emit" / "ui_data.h").read_text(encoding="utf-8")
    if "#define UI_STYLE_EXTRAS 1" not in header:
        raise CliError("fidelity header is missing UI_STYLE_EXTRAS")
    if "text_align" in header and "int text_align;" not in header:
        raise CliError("fidelity header has text_align data but no field")

    log_step("fidelity: legacy emitter guard")
    legacy = out / "legacy_input.h"
    _python(
        get_paths().emitter,
        ["--ui-json", str(get_paths().runtime / "tests" / "input" / "page" / "ui.json"),
         "--out", str(legacy), "--meta", str(out / "legacy_input_meta.json")],
        cwd=get_paths().root,
    )
    reference = get_paths().runtime / "tests" / "input" / "fixture" / "ui_data.h"
    if legacy.read_bytes() != reference.read_bytes():
        raise CliError("emitter output for tests/input is no longer byte-identical")

    runtime = get_paths().runtime
    log_step("fidelity: compile+run core test")
    core_exe = out / "fidelity_core_test.exe"
    _compile(
        core_exe,
        [suite / "fidelity_core_test.c", *[runtime / source for source in CORE_SOURCES]],
        [get_paths().root / "include", runtime, out / "emit"],
    )
    core_stdout = _run(core_exe, [], out)
    if "PASS" not in core_stdout:
        raise CliError("fidelity core test did not report PASS")

    log_step("fidelity: compile runtime + renders")
    runtime_exe = out / "runtime_fidelity.exe"
    _compile(
        runtime_exe,
        [runtime / source for source in RUNTIME_SOURCES],
        [get_paths().root / "include", runtime, _wgpu_dir() / "include", out / "emit"],
        [str(_wgpu_dir() / "lib" / "wgpu_native.dll.lib"), *RUNTIME_LIBS],
    )
    run_cmd(["cmd", "/d", "/c", "copy", "/Y",
             str(_wgpu_dir() / "bin" / "wgpu_native.dll"), str(out / "wgpu_native.dll")])
    _run(runtime_exe, ["--render", "480", "560", str(out / "render.png")], out)
    _run(runtime_exe, ["--render", "480", "560", str(out / "render_wheel.png"), "--wheel", "100", "350", "150"], out)

    log_info("fidelity: PASS")
    return 0


def _emit_fixture(suite: Path, page: Path, width: int, height: int, header: Path, pilot: bool = False) -> None:
    rust_exe = get_paths().frontend_rust / "target" / "release" / "frontend-rust.exe"
    if not rust_exe.is_file():
        raise CliError("frontend-rust.exe is missing; run: wn-cli frontend build")
    arguments = [
        "--html", "page/index.html",
        "--css", "page/style.css",
        "--width", str(width),
        "--height", str(height),
        "--emit-ui", str(page / "ui.json"),
    ]
    if pilot:
        arguments.append("--pilot")
    run_cmd([rust_exe, *arguments], cwd=suite)
    header.parent.mkdir(parents=True, exist_ok=True)
    _python(
        get_paths().emitter,
        ["--ui-json", str(page / "ui.json"), "--out", str(header),
         "--meta", str(header.parent / "ui_meta.json")],
        cwd=get_paths().root,
    )


def _expect(text: str, needles: Sequence[str], label: str) -> None:
    missing = [needle for needle in needles if needle not in text]
    if missing:
        raise CliError(f"{label} output is missing: {', '.join(repr(item) for item in missing)}")


def run_dynamic() -> int:
    suite = _suite_dir("dynamic")
    out = suite / "out"
    page = suite / "page"
    fixture = suite / "fixture"
    out.mkdir(parents=True, exist_ok=True)
    fixture.mkdir(parents=True, exist_ok=True)
    for image in ("a.png", "b.png"):
        if not (page / "images" / image).is_file():
            _python(suite / "make_images.py", [str(page)], cwd=suite)
            break

    log_step("dynamic: fixture header")
    _emit_fixture(suite, page, 400, 300, fixture / "ui_data.h")

    runtime = get_paths().runtime
    includes = [get_paths().root / "include", runtime, fixture, _wgpu_dir() / "include"]
    libs = [str(_wgpu_dir() / "lib" / "wgpu_native.dll.lib"), *RUNTIME_LIBS]

    log_step("dynamic: core test")
    core_exe = out / "dynamic_core_test.exe"
    _compile(core_exe, [suite / "dynamic_core_test.c", *[runtime / item for item in CORE_SOURCES]], includes)
    _expect(_run(core_exe, [str(out / "core_events.jsonl")], suite), ["dynamic_core_test: PASS"], "dynamic core test")

    log_step("dynamic: runtime stages")
    runtime_exe = out / "runtime_dynamic.exe"
    _compile(runtime_exe, [runtime / item for item in RUNTIME_SOURCES], includes, libs)
    run_cmd(["cmd", "/d", "/c", "copy", "/Y", str(_wgpu_dir() / "bin" / "wgpu_native.dll"), str(out / "wgpu_native.dll")])
    for stale in out.glob("stage_*.png"):
        stale.unlink()
    _expect(_run(runtime_exe, ["--dynamic", str(out / "stage")], page),
            ["dynamic: stage=10 action=clear cards"], "runtime_dynamic")

    log_step("dynamic: abi demo")
    abi_exe = out / "dynamic.exe"
    _compile(abi_exe, [runtime / item for item in RUNTIME_SOURCES[1:]] + [runtime / "examples" / "dynamic.c"], includes, libs)
    abi_text = _run(abi_exe, ["--frames", "8", "--image-b", str(page / "images" / "b.png")], page)
    _expect(abi_text, [
        "ABI 4",
        "dynamic: PASS",
        "set_text(Omega) -> WN_ERR_UNSUPPORTED",
        "set_image(text) -> WN_ERR_UNSUPPORTED",
        "remove(root) -> WN_ERR_INVALID_ARGUMENT",
        "clear_children -> nodes=9",
        "exit=0 frames=8",
    ], "dynamic abi demo")

    log_step("dynamic: verify")
    _python(suite / "verify_dynamic.py", ["--dir", str(out)], cwd=suite)
    log_info("dynamic UI v0: PASS")
    return 0


def run_input() -> int:
    suite = _suite_dir("input")
    out = suite / "out"
    page = suite / "page"
    fixture = suite / "fixture"
    out.mkdir(parents=True, exist_ok=True)
    fixture.mkdir(parents=True, exist_ok=True)

    log_step("input: fixture header")
    _emit_fixture(suite, page, 480, 240, fixture / "ui_data.h", pilot=True)

    runtime = get_paths().runtime
    includes = [get_paths().root / "include", runtime, fixture, _wgpu_dir() / "include"]
    libs = [str(_wgpu_dir() / "lib" / "wgpu_native.dll.lib"), *RUNTIME_LIBS]

    log_step("input: core test")
    core_exe = out / "input_core_test.exe"
    _compile(core_exe, [suite / "input_core_test.c", *[runtime / item for item in CORE_SOURCES]], includes)
    _expect(_run(core_exe, [str(out / "core_events.jsonl")], suite), ["input_core_test: PASS"], "input core test")

    log_step("input: runtime stages")
    runtime_exe = out / "runtime_input.exe"
    _compile(runtime_exe, [runtime / item for item in RUNTIME_SOURCES], includes, libs)
    run_cmd(["cmd", "/d", "/c", "copy", "/Y", str(_wgpu_dir() / "bin" / "wgpu_native.dll"), str(out / "wgpu_native.dll")])
    for stale in out.glob("stage_*.png"):
        stale.unlink()
    _expect(_run(runtime_exe, ["--input", str(out / "stage")], page),
            ["input: stage=9 action=escape blurs"], "runtime_input")

    log_step("input: window demo")
    window_text = _run(runtime_exe, ["--frames", "16", "--input-demo", "--width", "480", "--height", "240"], page)
    _expect(window_text, [
        "input-demo: WM_CHAR 'a'",
        "input-demo: WM_CHAR 'b'",
        "input-demo: WM_KEYDOWN VK_BACK",
        'input-demo: value="a" focused=1',
    ], "input window demo")

    log_step("input: abi demo")
    abi_exe = out / "input.exe"
    _compile(abi_exe, [runtime / item for item in RUNTIME_SOURCES[1:]] + [runtime / "examples" / "input.c"], includes, libs)
    abi_text = _run(abi_exe, ["--frames", "8"], page)
    _expect(abi_text, [
        "ABI 4",
        "input: PASS",
        "set_value(abc) -> WN_OK",
        "set_value(Omega) -> WN_ERR_UNSUPPORTED",
        "set_value(13 chars) -> WN_ERR_INVALID_ARGUMENT",
        "clipboard copy + paste",
        "cut all",
        "escape -> focused=0",
        "exit=0 frames=8",
    ], "input abi demo")

    log_step("input: verify")
    _python(suite / "verify_input.py", ["--dir", str(out)], cwd=suite)
    log_info("text input v1: PASS")
    return 0


def run_offscreen() -> int:
    suite = _suite_dir("offscreen")
    out = suite / "out"
    out.mkdir(parents=True, exist_ok=True)
    generated = get_paths().runtime / "generated"
    header = generated / "ui_data.h"
    if not header.is_file():
        raise CliError(f"missing {header}; run: wn-cli build --emit")

    runtime = get_paths().runtime
    log_step("offscreen: compile test (/W4 /WX)")
    exe = out / "offscreen_test.exe"
    _compile(
        exe,
        [runtime / item for item in RUNTIME_SOURCES if item != "runtime_r2.c"] + [suite / "offscreen_test.c"],
        [get_paths().root / "include", runtime, _wgpu_dir() / "include", generated],
        [str(_wgpu_dir() / "lib" / "wgpu_native.dll.lib"), *RUNTIME_LIBS],
    )
    run_cmd(["cmd", "/d", "/c", "copy", "/Y", str(_wgpu_dir() / "bin" / "wgpu_native.dll"), str(out / "wgpu_native.dll")])

    d3d11_png = out / "offscreen_d3d11.png"
    wgpu_png = out / "offscreen_wgpu.png"
    log_step("offscreen: render both backends")
    _run(exe, ["960", "640", str(d3d11_png)], out, {"WN_RENDERER": "d3d11"})
    _run(exe, ["960", "640", str(wgpu_png)], out, {"WN_RENDERER": "wgpu"})

    harness_python = get_paths().root / "benchmarks" / "visual-parity" / ".venv" / "Scripts" / "python.exe"
    python = str(harness_python) if harness_python.is_file() else str(get_python_exe())
    compare = out / "offscreen_compare.json"
    log_step("offscreen: compare backends")
    completed = subprocess.run(
        [
            python,
            str(runtime / "tests" / "d3d11" / "verify_d3d11.py"),
            "--capture", str(d3d11_png),
            "--reference", str(wgpu_png),
            "--report", str(compare),
            "--label", "offscreen-d3d11-vs-wgpu",
        ],
        cwd=out,
        capture_output=True,
        text=True,
    )
    for line in (completed.stdout + completed.stderr).splitlines():
        if line.strip():
            log_info(line.rstrip())
    if completed.returncode != 0:
        raise CliError("offscreen pixel comparison failed")

    checks = json.loads(compare.read_text(encoding="utf-8"))["checks"]
    log_info(
        "offscreen: {0}x{1} max_diff={2} bad={3:.2%} exact={4:.2%}".format(
            checks["capture_size"][0], checks["capture_size"][1],
            checks["max_channel_diff"], checks["bad_fraction"], checks["exact_fraction"],
        )
    )
    log_info("offscreen vtable: PASS")
    return 0


SUITES = {
    "css": ("CSS core v1 (rounded boxes, border, shadow, image)", run_css),
    "fidelity": ("E1 visual/layout fidelity (headless core + renders)", run_fidelity),
    "dynamic": ("dynamic UI v0 (visibility, clone/append/remove, ABI demo)", run_dynamic),
    "input": ("text input v1 (caret, selection, clipboard, ABI demo)", run_input),
    "offscreen": ("offscreen vtable (d3d11 vs wgpu readback)", run_offscreen),
}


def run_suite(name: str) -> int:
    entry = SUITES.get(name)
    if entry is None:
        raise CliError(f"unknown suite {name!r}; known: {', '.join(sorted(SUITES))}")
    log_step(f"suite: {name} - {entry[0]}")
    return entry[1]()
