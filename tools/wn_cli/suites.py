from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Sequence

from .core import CliError, check_binary, get_build_env, get_paths, get_python_exe, log_info, log_step, run_cmd

RUNTIME_SOURCES = (
    "runtime_r2.c",
    "core/wn_core.c",
    "core/layout.c",
    "core/kind_ops.c",
    "core/paint.c",
    "core/input.c",
    "core/hit_test.c",
    "core/scroll.c",
    "wn_common.c",
    "document/wn_blob.c",
    "wn_view.c",
    "wn_app.c",
    "renderer_wgpu.c",
    "renderer_d3d11.c",
    "host_win32.c",
    "text/text_runtime.c",
    "text/text_atlas.c",
    "text/text_directwrite.cpp",
)
# renderer_wgpu.c is replaced by a link-time stub when wgpu is disabled.
RUNTIME_SOURCES_NO_WGPU = tuple(
    "renderer_wgpu_stub.c" if source == "renderer_wgpu.c" else source
    for source in RUNTIME_SOURCES
)
CORE_SOURCES = ("core/wn_core.c", "core/layout.c", "core/kind_ops.c", "core/paint.c",
                "core/input.c", "core/hit_test.c", "core/scroll.c", "wn_common.c",
                "document/wn_blob.c", "text/text_runtime.c", "text/text_atlas.c",
                "text/text_directwrite.cpp")
CORE_LIBS = ("dwrite.lib",)
RUNTIME_LIBS = (
    "d3d11.lib",
    "dxgi.lib",
    "d3dcompiler.lib",
    "windowscodecs.lib",
    "ole32.lib",
    "user32.lib",
    "gdi32.lib",
    "dwrite.lib",
)
CFLAGS = ("/nologo", "/O2", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS", "/DWN_STATIC")
CXX_SUFFIXES = (".cpp", ".cc", ".cxx")

# Blob hashes pinned by the suites that own each fixture (blob, dynamic,
# offscreen, css). The SHA-256 is the primary gate for the `.wnui` contract: a
# fresh emission must be byte-identical to the reviewed artifact. `blob` also
# compares the builtin and file renders, but that comparison alone is
# tautological (both sides use the same emission).
PINNED_BLOBS = {
    "canonical": "eae247a4c3146d035078935b1f60b746ad0b1bd0871ced64c20e2b4726157e9f",
    "input": "a13e2b0a97aac6cad434c9e3340dd0d9db141e255500acb5ed9a297a0f31f916",
    "fidelity": "c71dad3835d3c130a5085ce37c637ab5b312a6b69566c946b1a66bf6214fd8df",
    "pilot": "55279171e5a77e795a7cd1822383578a588774030a475158fbee2079ccaecf4d",
    "dynamic": "412a93f0fff9b25a54a5fd7954b4e15f90a55d28c5ca86e8cb4294fb92dfe8a2",
    "cache": "67d39833e128d832686f02f4ec1784fcdd02a95378e965d8e5da1031b607b0dd",
    # CSS suite fixtures (HTML/CSS -> blob, same pinning rule as the corpus).
    "css_card": "ffa1f754942ad82812d9b86259c376243e587c32b6abc093e088fc2fd7425335",
    "css_control": "6d7f24e90cf1a0fe645006879a46309cb8d22082e487e41bab4e9db52182e798",
    "css_none": "e55d9fba99b45742c723ea3414dc1446996a51cc46d193a2cec7d553f7ba11f0",
}


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _assert_pinned_blob(name: str, blob: Path) -> None:
    """Fails when a freshly emitted blob differs from the reviewed artifact."""
    expected = PINNED_BLOBS.get(name)
    if expected is None:
        raise CliError(f"no pinned hash for corpus {name!r}")
    if not blob.is_file():
        raise CliError(f"cannot pin {name}: blob not found at {blob}")
    actual = _sha256(blob)
    if actual != expected:
        raise CliError(
            f"blob hash drift for {name}:\n"
            f"  expected {expected}\n"
            f"  actual   {actual}\n"
            f"  file     {blob}\n"
            "If the new blob is intended, update PINNED_BLOBS in "
            "tools/wn_cli/suites.py."
        )
    log_info(f"blob hash {name}: {actual}")


def _language_flags(source: Path) -> list[str]:
    if source.suffix.lower() in CXX_SUFFIXES:
        return ["/std:c++17"]
    return ["/std:c17", "/TC"]


def _suite_dir(name: str) -> Path:
    return get_paths().runtime / "tests" / name


def _wgpu_dir() -> Path:
    return get_paths().root / "third_party" / "wgpu-native"


_wgpu_requested = True


def set_wgpu_enabled(enabled: bool) -> None:
    """Selects whether suite builds include the wgpu-native backend."""
    global _wgpu_requested
    _wgpu_requested = enabled


def wgpu_available() -> bool:
    """True when the backend was requested and its pinned artifacts exist."""
    if not _wgpu_requested:
        return False
    root = _wgpu_dir()
    return (root / "lib" / "wgpu_native.dll.lib").is_file() and \
        (root / "bin" / "wgpu_native.dll").is_file()


def runtime_sources(skip_main: bool = False) -> list[Path]:
    runtime = get_paths().runtime
    names = RUNTIME_SOURCES if wgpu_available() else RUNTIME_SOURCES_NO_WGPU
    selected = names[1:] if skip_main else names
    return [runtime / name for name in selected]


def runtime_includes(*extra: Path) -> list[Path]:
    includes = [get_paths().root / "include", get_paths().runtime, *extra]
    if wgpu_available():
        includes.append(_wgpu_dir() / "include")
    return includes


def runtime_libs() -> list[str]:
    libs = list(RUNTIME_LIBS)
    if wgpu_available():
        libs.insert(0, str(_wgpu_dir() / "lib" / "wgpu_native.dll.lib"))
    return libs


def copy_wgpu_dll(dest_dir: Path) -> None:
    if not wgpu_available():
        return
    run_cmd(["cmd", "/d", "/c", "copy", "/Y",
             str(_wgpu_dir() / "bin" / "wgpu_native.dll"),
             str(dest_dir / "wgpu_native.dll")])


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

    objects_dir = exe.parent / "obj"
    objects_dir.mkdir(parents=True, exist_ok=True)
    include_flags = [f"/I{path}" for path in includes]
    cflags = list(CFLAGS)
    cflags.append(f"/DWN_WGPU={1 if wgpu_available() else 0}")
    objects: list[str] = []
    stems = [Path(source).stem for source in sources]
    for source in sources:
        source = Path(source)
        stem = source.stem
        if stems.count(stem) > 1:
            stem = f"{source.parent.name}_{stem}"
        object_file = objects_dir / f"{stem}.obj"
        run_cmd(
            [cl, *cflags, *_language_flags(source), *include_flags, "/c",
             f"/Fo{object_file}", str(source)],
            cwd=exe.parent,
            use_build_env=True,
        )
        objects.append(str(object_file))

    command = [cl, *cflags, *objects, f"/Fe{exe}"]
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


def _run_logged(exe: Path, arguments: Sequence[str], cwd: Path,
                log_path: Path) -> str:
    text = _run(exe, arguments, cwd)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(text, encoding="utf-8")
    return text


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


def _embed_blob(blob: Path, header: Path) -> None:
    """Generate the embedded C header the runtime's builtin document uses."""
    if not blob.is_file():
        raise CliError(f"blob not found: {blob}")
    header.parent.mkdir(parents=True, exist_ok=True)
    _python(
        Path(__file__).resolve().parent / "embed_blob.py",
        ["--blob", str(blob), "--header", str(header)],
        cwd=get_paths().root,
    )


def _emit_css_fixture(name: str, page_dir: Path, width: int, height: int,
                      fixture: Path) -> Path:
    """Emit one CSS fixture with the same HTML/CSS -> blob path as every suite."""
    rust_exe = get_paths().frontend_rust / "target" / "release" / "frontend-rust.exe"
    if not rust_exe.is_file():
        raise CliError("frontend-rust.exe is missing; run: wn-cli frontend build")
    fixture.mkdir(parents=True, exist_ok=True)
    blob = fixture / "app.wnui"
    run_cmd(
        [rust_exe, "--html", str(page_dir / f"{name}.html"),
         "--css", str(page_dir / f"{name}.css"),
         "--width", str(width), "--height", str(height),
         "--emit-blob", str(blob), "--pilot"],
        cwd=page_dir,
    )
    if not blob.is_file():
        raise CliError(f"frontend-rust did not write {name} app.wnui")
    _embed_blob(blob, fixture / "builtin_blob.h")
    return blob


def run_css() -> int:
    suite = _suite_dir("css")
    out = suite / "out"
    page = suite / "page"
    out.mkdir(parents=True, exist_ok=True)

    log_step("css: deterministic poster")
    _python(suite / "make_images.py", [], cwd=suite)

    for name, (width, height) in (("card", (400, 240)),
                                  ("control", (400, 240)),
                                  ("none", (120, 80))):
        log_step(f"css: frontend-rust --emit-blob ({name}.html)")
        blob = _emit_css_fixture(name, page, width, height, out / "emit" / name)
        _assert_pinned_blob(f"css_{name}", blob)

    runtime = get_paths().runtime
    log_step("css: compile wnui-dump")
    dump_exe = out / "wnui-dump.exe"
    _compile(
        dump_exe,
        [runtime / "tools" / "wn_dump.c",
         *[runtime / source for source in CORE_SOURCES]],
        [get_paths().root / "include", runtime, out / "emit" / "card"],
        CORE_LIBS,
    )

    for name in ("card", "control"):
        log_step(f"css: compile runtime_css_{name}.exe")
        emit_dir = out / "emit" / name
        _compile(
            out / "build" / name / f"runtime_css_{name}.exe",
            runtime_sources(),
            runtime_includes(emit_dir),
            runtime_libs(),
        )
        copy_wgpu_dll(out / "build" / name)

    # The css reference was captured with wgpu; use D3D11 when wgpu is compiled
    # out (the verifier relaxes only the poster-sampling tolerance there).
    backend = "wgpu" if wgpu_available() else "d3d11"
    backends = {"WN_RENDERER": backend}
    card_exe = out / "build" / "card" / "runtime_css_card.exe"
    control_exe = out / "build" / "control" / "runtime_css_control.exe"

    log_step("css: layout report + offscreen renders")
    _run(card_exe, ["--report", str(out / "layout_card.json"), "--sizes", "400x240"], page, backends)
    _run(card_exe, ["--render", "400", "240", str(out / "render_card.png")], page, backends)
    _run(control_exe, ["--render", "400", "240", str(out / "render_control.png")], page, backends)

    log_step("css: verify")
    _python(suite / "verify_css.py", ["--dir", str(out), "--page", str(page),
                                      "--dump", str(dump_exe),
                                      "--backend", backend], cwd=suite)
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

    log_step("fidelity: deterministic ratio image")
    # `out/images` is what the runtime render loads at run time.
    _python(suite / "make_images.py", [], cwd=suite)
    # The blob bakes intrinsic sizes from the HTML directory, so the same asset
    # must also exist next to the page.
    _python(suite / "make_images.py", [str(suite / "page" / "images")], cwd=suite)

    log_step("fidelity: frontend-rust --emit-blob")
    run_cmd(
        [rust_exe, "--html", "page/index.html", "--width", "480", "--height", "560",
         "--emit-blob", str(out / "fidelity.wnui"), "--pilot"],
        cwd=suite,
    )

    log_step("fidelity: embed the builtin blob")
    _embed_blob(out / "fidelity.wnui", out / "emit" / "builtin_blob.h")

    log_step("fidelity: blob determinism guard")
    second_blob = out / "fidelity_second.wnui"
    run_cmd(
        [rust_exe, "--html", "page/index.html", "--width", "480", "--height", "560",
         "--emit-blob", str(second_blob), "--pilot"],
        cwd=suite,
    )
    if (out / "fidelity.wnui").read_bytes() != second_blob.read_bytes():
        raise CliError("frontend-rust blob output for the fidelity page is not deterministic")

    runtime = get_paths().runtime
    log_step("fidelity: compile+run core test")
    core_exe = out / "fidelity_core_test.exe"
    _compile(
        core_exe,
        [suite / "fidelity_core_test.c", *[runtime / source for source in CORE_SOURCES]],
        [get_paths().root / "include", runtime, out / "emit"],
        CORE_LIBS,
    )
    core_stdout = _run(core_exe, [], out)
    if "PASS" not in core_stdout:
        raise CliError("fidelity core test did not report PASS")

    log_step("fidelity: ABI v6 core test")
    abi_exe = out / "abi_v6_core_test.exe"
    _compile(
        abi_exe,
        [suite / "abi_v6_core_test.c", *[runtime / source for source in CORE_SOURCES]],
        [get_paths().root / "include", runtime, out / "emit"],
        CORE_LIBS,
    )
    _expect(_run(abi_exe, [], out), ["abi_v6_core_test: PASS"], "ABI v6 core test")

    log_step("fidelity: kind capability table")
    kind_exe = out / "kind_ops_test.exe"
    _compile(
        kind_exe,
        [_suite_dir("core") / "kind_ops_test.c", *[runtime / source for source in CORE_SOURCES]],
        [get_paths().root / "include", runtime, out / "emit"],
        CORE_LIBS,
    )
    kind_text = _run(kind_exe, [], out)
    _expect(kind_text, ["kind_ops_test: PASS"], "kind ops test")
    # 9 checks per kind entry (6 kinds) + 5 table-level checks. Deleting one
    # kind iteration drops this to 50, so the floor is the full 59.
    _expect_checks(kind_text, "kind ops test", 59)

    log_step("fidelity: TEXT percentage height")
    pct_exe = out / "text_pct_height_test.exe"
    _compile(
        pct_exe,
        [_suite_dir("core") / "text_pct_height_test.c", *[runtime / source for source in CORE_SOURCES]],
        [get_paths().root / "include", runtime, out / "emit"],
        CORE_LIBS,
    )
    pct_text = _run(pct_exe, [], out)
    _expect(pct_text, ["text_pct_height_test: PASS"], "TEXT percentage height test")
    _expect_checks(pct_text, "TEXT percentage height test", 9)

    log_step("fidelity: scroll service (x/y clamp, wheel, into-view)")
    scroll_exe = out / "scroll_test.exe"
    _compile(
        scroll_exe,
        [_suite_dir("core") / "scroll_test.c", *[runtime / source for source in CORE_SOURCES]],
        [get_paths().root / "include", runtime, out / "emit"],
        CORE_LIBS,
    )
    scroll_text = _run(scroll_exe, [], out)
    _expect(scroll_text, ["scroll_test: PASS"], "scroll service test")
    # 14 checks: vertical clamp/apply, the inert x delta, the forced x clamp
    # and apply, the forced x into-view path, and the real-page wheel.
    _expect_checks(scroll_text, "scroll service test", 14)

    log_step("fidelity: compile runtime + renders")
    runtime_exe = out / "runtime_fidelity.exe"
    _compile(
        runtime_exe,
        runtime_sources(),
        runtime_includes(out / "emit"),
        runtime_libs(),
    )
    copy_wgpu_dll(out)
    _run(runtime_exe, ["--render", "480", "560", str(out / "render.png")], out)
    _run(runtime_exe, ["--render", "480", "560", str(out / "render_wheel.png"), "--wheel", "100", "350", "150"], out)

    log_info("fidelity: PASS")
    return 0


def _emit_fixture(suite: Path, page: Path, width: int, height: int, fixture: Path,
                  pilot: bool = False) -> Path:
    rust_exe = get_paths().frontend_rust / "target" / "release" / "frontend-rust.exe"
    if not rust_exe.is_file():
        raise CliError("frontend-rust.exe is missing; run: wn-cli frontend build")
    fixture.mkdir(parents=True, exist_ok=True)
    blob = fixture / "app.wnui"
    arguments = [
        "--html", "page/index.html",
        "--css", "page/style.css",
        "--width", str(width),
        "--height", str(height),
        "--emit-blob", str(blob),
    ]
    if pilot:
        arguments.append("--pilot")
    run_cmd([rust_exe, *arguments], cwd=suite)
    if not blob.is_file():
        raise CliError("frontend-rust did not write app.wnui")
    _embed_blob(blob, fixture / "builtin_blob.h")
    return blob


def _expect(text: str, needles: Sequence[str], label: str) -> None:
    missing = [needle for needle in needles if needle not in text]
    if missing:
        raise CliError(f"{label} output is missing: {', '.join(repr(item) for item in missing)}")


def _expect_measure_result(text: str, label: str) -> None:
    """Real-backend smoke: PASS is required on Windows; SKIP is legal elsewhere."""
    if "PASS" in text:
        return
    if sys.platform != "win32" and "SKIP" in text:
        return
    expected = "PASS" if sys.platform == "win32" else "PASS or SKIP"
    raise CliError(f"{label} did not report {expected}")


def _expect_checks(text: str, label: str, minimum: int) -> None:
    """Requires `label: PASS (N checks)` with N >= minimum."""
    marker = "PASS ("
    start = text.find(marker)
    if start < 0:
        raise CliError(f"{label} did not report PASS")
    start += len(marker)
    end = text.find(" ", start)
    if end < 0:
        raise CliError(f"{label} PASS line has no check count")
    try:
        count = int(text[start:end])
    except ValueError as exc:
        raise CliError(f"{label} PASS line has an unreadable check count") from exc
    if count < minimum:
        raise CliError(f"{label} ran only {count} checks (expected at least {minimum})")


def run_dynamic() -> int:
    suite = _suite_dir("dynamic")
    out = suite / "out"
    page = suite / "page"
    fixture = suite / "fixture"
    out.mkdir(parents=True, exist_ok=True)
    fixture.mkdir(parents=True, exist_ok=True)
    for image in ("a.png", "b.png"):
        if not (page / "images" / image).is_file():
            _python(suite / "make_images.py", [str(page / "images")], cwd=suite)
            break

    log_step("dynamic: fixture blob")
    dynamic_blob = _emit_fixture(suite, page, 400, 300, fixture)
    _assert_pinned_blob("dynamic", dynamic_blob)

    runtime = get_paths().runtime
    includes = runtime_includes(fixture)
    libs = runtime_libs()

    log_step("dynamic: core test")
    core_exe = out / "dynamic_core_test.exe"
    _compile(core_exe, [suite / "dynamic_core_test.c", *[runtime / item for item in CORE_SOURCES]], includes, CORE_LIBS)
    _expect(_run(core_exe, [str(out / "core_events.jsonl")], suite), ["dynamic_core_test: PASS"], "dynamic core test")

    log_step("dynamic: runtime stages")
    runtime_exe = out / "runtime_dynamic.exe"
    _compile(runtime_exe, runtime_sources(), includes, libs)
    copy_wgpu_dll(out)
    for stale in out.glob("stage_*.png"):
        stale.unlink()
    _expect(_run(runtime_exe, ["--dynamic", str(out / "stage")], page),
            ["dynamic: stage=10 action=clear cards"], "runtime_dynamic")

    log_step("dynamic: abi demo")
    abi_exe = out / "dynamic.exe"
    _compile(abi_exe, runtime_sources(skip_main=True) + [runtime / "examples" / "dynamic.c"], includes, libs)
    abi_text = _run(abi_exe, ["--frames", "8", "--image-b", str(page / "images" / "b.png")], page)
    _expect(abi_text, [
        "ABI 6",
        "dynamic: PASS",
        "set_text(Omega) -> WN_OK",
        "set_image(text) -> WN_ERR_UNSUPPORTED",
        "remove(root) -> WN_ERR_INVALID_ARGUMENT",
        "clear_children -> nodes=9",
        "dynamic: nav clone=",
        "parent_ok=1 chain_ok=1 invalid_ok=1",
        "dynamic: stale handle",
        "-> rejected",
        "dynamic: growth clones=1500 alive=1500 nodes=6017 -> grown",
        "dynamic: growth removed nodes=17 -> restored",
        "dynamic: second run frames=3",
        "-> stats reset",
        "dynamic: click clone -> node=",
        "target=(null)",
        "dynamic: click card -> node=",
        "target=body.root/div.cards/div.card",
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

    log_step("input: fixture blob")
    _emit_fixture(suite, page, 480, 240, fixture, pilot=True)

    runtime = get_paths().runtime
    includes = runtime_includes(fixture)
    libs = runtime_libs()

    log_step("input: core test")
    core_exe = out / "input_core_test.exe"
    _compile(core_exe, [suite / "input_core_test.c", *[runtime / item for item in CORE_SOURCES]], includes, CORE_LIBS)
    _expect(_run_logged(core_exe, [str(out / "core_events.jsonl")], suite,
                        out / "core_test.log"), ["input_core_test: PASS"], "input core test")

    log_step("input: runtime stages")
    runtime_exe = out / "runtime_input.exe"
    _compile(runtime_exe, runtime_sources(), includes, libs)
    copy_wgpu_dll(out)
    for stale in out.glob("stage_*.png"):
        stale.unlink()
    _expect(_run_logged(runtime_exe, ["--input", str(out / "stage")], page,
                        out / "runtime_input.log"),
            ["input: stage=9 action=escape blurs"], "runtime_input")

    log_step("input: window demo")
    window_text = _run_logged(runtime_exe, ["--frames", "16", "--input-demo", "--width", "480", "--height", "240"], page,
                              out / "window_input.log")
    _expect(window_text, [
        "input-demo: WM_CHAR 'a'",
        "input-demo: WM_CHAR 'b'",
        "input-demo: WM_KEYDOWN VK_BACK",
        'input-demo: value="a" focused=1',
    ], "input window demo")

    log_step("input: abi demo")
    abi_exe = out / "input.exe"
    _compile(abi_exe, runtime_sources(skip_main=True) + [runtime / "examples" / "input.c"], includes, libs)
    abi_text = _run_logged(abi_exe, ["--frames", "8"], page, out / "abi_input.log")
    _expect(abi_text, [
        "ABI 6",
        "input: PASS",
        "set_value(abc) -> WN_OK",
        "set_value(Omega) -> WN_OK",
        "set_value(13 chars) -> WN_ERR_INVALID_ARGUMENT",
        "value survived rejects = \"abc\"",
        "clipboard copy + paste",
        "cut all",
        "escape -> focused=0",
        "exit=0 frames=8",
    ], "input abi demo")

    log_step("input: verify")
    _python(suite / "verify_input.py", ["--dir", str(out)], cwd=suite)
    log_info("text input v1: PASS")
    return 0


def _run_image_cache(out: Path, suite: Path, cache_exe: Path, backend: str) -> None:
    cache_page = suite / "cache_page"
    png = out / f"image_cache_{backend}.png"
    missing_png = out / f"image_cache_missing_{backend}.png"
    log_step(f"offscreen: image cache budget ({backend})")
    _run(cache_exe, [str(png), str(missing_png)], cache_page,
         {"WN_RENDERER": backend, "WN_IMAGE_CACHE_MB": "4"})
    _python(suite / "verify_cache.py", ["--png", str(png)], cwd=suite)
    _python(suite / "verify_cache.py", ["--png", str(missing_png)], cwd=suite)


def run_offscreen() -> int:
    suite = _suite_dir("offscreen")
    out = suite / "out"
    fixture = out / "fixture"
    cache_fixture = out / "cache_fixture"
    out.mkdir(parents=True, exist_ok=True)
    fixture.mkdir(parents=True, exist_ok=True)
    cache_fixture.mkdir(parents=True, exist_ok=True)
    runtime = get_paths().runtime
    wgpu = wgpu_available()

    log_step("offscreen: canonical fixture blob")
    rust_exe = get_paths().frontend_rust / "target" / "release" / "frontend-rust.exe"
    if not rust_exe.is_file():
        raise CliError("frontend-rust.exe is missing; run: wn-cli frontend build")
    page = runtime / "page"
    run_cmd(
        [rust_exe, "--html", str(page / "index.html"), "--css", str(page / "style.css"),
         "--width", "960", "--height", "640",
         "--emit-blob", str(fixture / "app.wnui")],
        cwd=runtime,
    )
    _embed_blob(fixture / "app.wnui", fixture / "builtin_blob.h")

    log_step("offscreen: compile test (/W4 /WX)")
    exe = out / "offscreen_test.exe"
    _compile(
        exe,
        runtime_sources(skip_main=True) + [suite / "offscreen_test.c"],
        runtime_includes(fixture),
        runtime_libs(),
    )
    copy_wgpu_dll(out)

    log_step("offscreen: input fixture blob (destroy-in-callback)")
    input_blob = _blob_emit(runtime / "tests" / "input" / "page",
                            out / "fixture" / "input", 480, 240, True)

    d3d11_png = out / "offscreen_d3d11.png"
    wgpu_png = out / "offscreen_wgpu.png"
    log_step("offscreen: render d3d11" + (" and wgpu" if wgpu else ""))
    _run(exe, ["960", "640", str(d3d11_png), str(input_blob)], out, {"WN_RENDERER": "d3d11"})
    if not wgpu:
        log_info("offscreen: wgpu backend unavailable (WN_WGPU=OFF or wgpu-native "
                 "artifacts missing); skipping the d3d11-vs-wgpu pixel comparison")
    else:
        _run(exe, ["960", "640", str(wgpu_png), str(input_blob)], out, {"WN_RENDERER": "wgpu"})

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

    log_step("offscreen: image cache fixture blob")
    _python(suite / "make_posters.py", [], cwd=suite)
    cache_blob = _blob_emit(suite / "cache_page", cache_fixture, 800, 300, False)
    _assert_pinned_blob("cache", cache_blob)
    cache_exe = out / "image_cache_test.exe"
    _compile(
        cache_exe,
        runtime_sources(skip_main=True) + [suite / "image_cache_test.c"],
        runtime_includes(cache_fixture),
        runtime_libs(),
    )
    copy_wgpu_dll(out)

    _run_image_cache(out, suite, cache_exe, "d3d11")
    if wgpu:
        _run_image_cache(out, suite, cache_exe, "wgpu")

    log_info("offscreen vtable: PASS")
    return 0


def run_text() -> int:
    suite = _suite_dir("text")
    out = suite / "out"
    fixture = out / "fixture"
    out.mkdir(parents=True, exist_ok=True)
    fixture.mkdir(parents=True, exist_ok=True)
    runtime = get_paths().runtime

    log_step("text: measure backend")
    exe = out / "text_measure_test.exe"
    atlas_exe = out / "atlas_test.exe"
    _compile(
        exe,
        [suite / "measure_test.c", runtime / "text" / "text_runtime.c",
         runtime / "text" / "text_directwrite.cpp"],
        [get_paths().root / "include", runtime],
        ["dwrite.lib"],
    )
    _expect_measure_result(_run(exe, [], out), "text measure test")

    log_step("text: atlas")
    atlas_exe = out / "atlas_test.exe"
    _compile(
        atlas_exe,
        [suite / "atlas_test.c", runtime / "text" / "text_atlas.c",
         runtime / "text" / "text_runtime.c", runtime / "text" / "text_directwrite.cpp"],
        [get_paths().root / "include", runtime],
        CORE_LIBS,
    )
    _expect(_run(atlas_exe, [], out), ["atlas_test: PASS"], "text atlas test")

    log_step("text: canonical document fixture blob")
    rust_exe = get_paths().frontend_rust / "target" / "release" / "frontend-rust.exe"
    if not rust_exe.is_file():
        raise CliError("frontend-rust.exe is missing; run: wn-cli frontend build")
    page = runtime / "page"
    run_cmd(
        [rust_exe, "--html", str(page / "index.html"), "--css", str(page / "style.css"),
         "--width", "960", "--height", "640",
         "--emit-blob", str(fixture / "app.wnui")],
        cwd=runtime,
    )
    _embed_blob(fixture / "app.wnui", fixture / "builtin_blob.h")

    log_step("text: document measurement")
    document_exe = out / "document_text_test.exe"
    _compile(
        document_exe,
        [suite / "document_test.c", *[runtime / item for item in CORE_SOURCES]],
        [get_paths().root / "include", runtime, fixture],
        CORE_LIBS,
    )
    _expect(_run(document_exe, [], out), ["document_text_test: PASS"], "document text test")

    log_step("text: greedy wrapping")
    wrap_exe = out / "wrap_test.exe"
    _compile(
        wrap_exe,
        [suite / "wrap_test.c", *[runtime / item for item in CORE_SOURCES]],
        [get_paths().root / "include", runtime, fixture],
        CORE_LIBS,
    )
    _expect(_run(wrap_exe, [], out), ["wrap_test: PASS"], "text wrap test")

    log_step("text: contracts (fake backend, no DirectWrite)")
    contract_exe = out / "contract_test.exe"
    _compile(
        contract_exe,
        [suite / "fake_text.c", suite / "contract_test.c",
         *[runtime / item for item in CORE_SOURCES]],
        [get_paths().root / "include", runtime, fixture],
        CORE_LIBS,
    )
    contract_text = _run(contract_exe, [], out)
    _expect(contract_text, ["contract_test: PASS"], "text contract test")
    _expect_checks(contract_text, "text contract test", 45)

    log_step("text: derived block (layout/paint/input)")
    block_exe = out / "text_block_test.exe"
    _compile(
        block_exe,
        [suite / "fake_text.c", suite / "text_block_test.c",
         *[runtime / item for item in CORE_SOURCES]],
        [get_paths().root / "include", runtime, fixture],
        CORE_LIBS,
    )
    block_text = _run(block_exe, [], out)
    _expect(block_text, ["text_block_test: PASS"], "text block test")
    _expect_checks(block_text, "text block test", 51)
    log_info("runtime text v0: PASS")
    return 0


def _blob_emit(page_dir: Path, fixture: Path, width: int, height: int,
               pilot: bool) -> Path:
    runtime = get_paths().runtime
    rust_exe = get_paths().frontend_rust / "target" / "release" / "frontend-rust.exe"
    fixture.mkdir(parents=True, exist_ok=True)
    arguments = [
        "--html", str(page_dir / "index.html"),
        "--css", str(page_dir / "style.css"),
        "--width", str(width),
        "--height", str(height),
        "--emit-blob", str(fixture / "app.wnui"),
    ]
    if pilot:
        arguments.append("--pilot")
    run_cmd([rust_exe, *arguments], cwd=runtime)
    blob = fixture / "app.wnui"
    if not blob.is_file():
        raise CliError("frontend-rust did not write app.wnui")
    _embed_blob(blob, fixture / "builtin_blob.h")
    return blob


def run_blob() -> int:
    suite = _suite_dir("blob")
    out = suite / "out"
    runtime = get_paths().runtime
    out.mkdir(parents=True, exist_ok=True)
    rust_exe = get_paths().frontend_rust / "target" / "release" / "frontend-rust.exe"
    if not rust_exe.is_file():
        raise CliError("frontend-rust.exe is missing; run: wn-cli frontend build")

    log_step("blob: pinned corpus hashes (canonical, input, fidelity, pilot)")
    root = get_paths().root
    fidelity_page = root / "src" / "runtime" / "tests" / "fidelity" / "page"
    # The fidelity blob bakes a 20x10 intrinsic size from the HTML directory.
    _python(
        root / "src" / "runtime" / "tests" / "fidelity" / "make_images.py",
        [str(fidelity_page / "images")],
        cwd=root / "src" / "runtime" / "tests" / "fidelity",
    )
    corpora = (
        ("canonical", root / "src" / "runtime" / "page", 960, 640, False),
        ("input", root / "src" / "runtime" / "tests" / "input" / "page", 480, 240, True),
        ("fidelity", fidelity_page, 480, 560, True),
        ("pilot", root / "benchmarks" / "pilot-media-library" / "page", 1280, 800, True),
    )
    for name, page, width, height, pilot in corpora:
        pinned = _blob_emit(page, out / "pinned" / name, width, height, pilot)
        _assert_pinned_blob(name, pinned)

    log_step("blob: canonical document")
    page = runtime / "page"
    fixture = out / "fixture" / "canonical"
    blob = _blob_emit(page, fixture, 960, 640, False)

    log_step("blob: load and compare against the builtin document")
    exe = out / "blob_test.exe"
    _compile(
        exe,
        [suite / "blob_test.c", *[runtime / item for item in CORE_SOURCES]],
        [get_paths().root / "include", runtime, fixture],
        CORE_LIBS,
    )
    _expect(_run(exe, [str(blob)], out), ["blob_test: PASS"], "blob test")

    log_step("blob: render builtin vs blob")
    runtime_exe = out / "runtime_blob.exe"
    _compile(
        runtime_exe,
        runtime_sources(),
        runtime_includes(fixture),
        runtime_libs(),
    )
    copy_wgpu_dll(out)
    builtin_png = out / "render_builtin.png"
    blob_png = out / "render_blob.png"
    environment = {"WN_RENDERER": "d3d11"}
    _run(runtime_exe, ["--render", "960", "640", str(builtin_png)], out, environment)
    _run(runtime_exe, ["--render", "960", "640", str(blob_png), "--document", str(blob)],
         out, environment)

    log_step("blob: compare pixels")
    compare = out / "blob_compare.json"
    completed = subprocess.run(
        [
            str(get_python_exe()),
            str(runtime / "tests" / "d3d11" / "verify_d3d11.py"),
            "--capture", str(blob_png),
            "--reference", str(builtin_png),
            "--report", str(compare),
            "--label", "blob-vs-builtin",
        ],
        cwd=out,
        capture_output=True,
        text=True,
    )
    for line in (completed.stdout + completed.stderr).splitlines():
        if line.strip():
            log_info(line.rstrip())
    if completed.returncode != 0:
        raise CliError("blob render differs from the builtin render")
    checks = json.loads(compare.read_text(encoding="utf-8"))["checks"]
    log_info(
        "blob: max_diff={0} bad={1:.2%} exact={2:.2%}".format(
            checks["max_channel_diff"], checks["bad_fraction"], checks["exact_fraction"],
        )
    )

    log_step("blob: input page (spec + runtime text)")
    input_fixture = out / "fixture" / "input"
    input_blob = _blob_emit(runtime / "tests" / "input" / "page", input_fixture, 480, 240, True)
    input_exe = out / "blob_input_test.exe"
    _compile(
        input_exe,
        [suite / "blob_test.c", *[runtime / item for item in CORE_SOURCES]],
        [get_paths().root / "include", runtime, input_fixture],
        CORE_LIBS,
    )
    _expect(_run(input_exe, [str(input_blob)], out), ["blob_test: PASS"], "input blob test")
    log_info("blob v2: PASS")
    return 0


def run_dump() -> int:
    suite = _suite_dir("dump")
    out = suite / "out"
    out.mkdir(parents=True, exist_ok=True)
    runtime = get_paths().runtime
    rust_exe = get_paths().frontend_rust / "target" / "release" / "frontend-rust.exe"
    if not rust_exe.is_file():
        raise CliError("frontend-rust.exe is missing; run: wn-cli frontend build")

    log_step("dump: canonical blob")
    canonical_fixture = out / "fixture" / "canonical"
    canonical = _blob_emit(runtime / "page", canonical_fixture, 960, 640, False)

    log_step("dump: compile wnui-dump")
    exe = out / "wnui-dump.exe"
    _compile(
        exe,
        [runtime / "tools" / "wn_dump.c",
         *[runtime / source for source in CORE_SOURCES]],
        [get_paths().root / "include", runtime, canonical_fixture],
        CORE_LIBS,
    )

    log_step("dump: canonical text projection vs golden")
    text = _run(exe, [str(canonical)], out).replace("\r\n", "\n")
    golden_path = suite / "canonical.txt"
    if not golden_path.is_file():
        raise CliError(f"missing dump golden: {golden_path}")
    golden = golden_path.read_text(encoding="utf-8").replace("\r\n", "\n")
    if text != golden:
        actual_path = out / "canonical.actual.txt"
        actual_path.write_text(text, encoding="utf-8")
        raise CliError(
            "wnui-dump text output changed; inspect "
            f"{actual_path} and update {golden_path} only for an intended dump change"
        )

    log_step("dump: canonical JSON projection")
    document = json.loads(_run(exe, ["--json", str(canonical)], out))
    if document.get("format") != "wnui/2":
        raise CliError(f"unexpected dump format: {document.get('format')!r}")
    if document.get("viewport") != {"width": 960, "height": 640}:
        raise CliError(f"unexpected dump viewport: {document.get('viewport')!r}")
    if len(document.get("nodes", [])) != document.get("node_count"):
        raise CliError("dump nodes[] does not match node_count")
    if document.get("input_count") != 0 or document.get("inputs") != []:
        raise CliError("canonical page must not carry inputs")

    log_step("dump: input page projection")
    input_fixture = out / "fixture" / "input"
    input_blob = _blob_emit(runtime / "tests" / "input" / "page", input_fixture, 480, 240, True)
    input_text = _run(exe, [str(input_blob)], out)
    _expect(input_text, [
        "nodes=4 inputs=2",
        " kind=input ",
        'name="body.root/div.bar/input#search.search"',
        'value=""',
        'placeholder="Buscar..."',
        "max-length=12",
        'placeholder="Otro"',
        "max-length=5",
    ], "wnui-dump input projection")

    log_step("dump: missing and corrupt files fail clearly")
    for name, payload in (("missing.wnui", None), ("corrupt.wnui", b"not a wnui blob")):
        bad = out / name
        if payload is not None:
            bad.write_bytes(payload)
        else:
            bad.unlink(missing_ok=True)
        completed = subprocess.run([str(exe), str(bad)], cwd=out,
                                   capture_output=True, text=True)
        combined = completed.stdout + completed.stderr
        if completed.returncode == 0:
            raise CliError(f"wnui-dump accepted {name}")
        if "wnui-dump:" not in combined or str(bad) not in combined:
            raise CliError(f"wnui-dump did not report {name} clearly: {combined!r}")

    log_info("wnui-dump: PASS")
    return 0


SUITES = {
    "css": ("CSS core v1 (rounded boxes, border, shadow, image)", run_css),
    "fidelity": ("E1 visual/layout fidelity (headless core + renders)", run_fidelity),
    "dynamic": ("dynamic UI v0 (visibility, clone/append/remove, ABI demo)", run_dynamic),
    "input": ("text input v1 (caret, selection, clipboard, ABI demo)", run_input),
    "offscreen": ("offscreen vtable (d3d11 vs wgpu readback)", run_offscreen),
    "text": ("runtime text v0 (contracts on the fake backend + DirectWrite smoke)", run_text),
    "blob": ("blob v2 (.wnui load, layout and pixels)", run_blob),
    "dump": ("wnui-dump debug projection (text/JSON/errors)", run_dump),
}


def run_suite(name: str) -> int:
    entry = SUITES.get(name)
    if entry is None:
        raise CliError(f"unknown suite {name!r}; known: {', '.join(sorted(SUITES))}")
    log_step(f"suite: {name} - {entry[0]}")
    return entry[1]()
