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
    "media/media_registry.c",
    "media/media_null.c",
    "media/media_core.c",
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
                "document/wn_blob.c", "media/media_registry.c", "media/media_null.c",
                "media/media_core.c", "text/text_runtime.c", "text/text_atlas.c",
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
    "imm32.lib",
)
CFLAGS = ("/nologo", "/O2", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS", "/DWN_STATIC")
CXX_SUFFIXES = (".cpp", ".cc", ".cxx")

# Blob hashes pinned by the suites that own each fixture (blob, dynamic,
# offscreen, css). The SHA-256 is the primary gate for the `.wnui` contract: a
# fresh emission must be byte-identical to the reviewed artifact. `blob` also
# compares the builtin and file renders, but that comparison alone is
# tautological (both sides use the same emission).
PINNED_BLOBS = {
    "canonical": "32fde35e5892cebb93f5da93a8d62669869cc1a0666125f38c1e2e3ed0b97f1c",
    "input": "0fc0d7257cf5aebf762622ab3825bb0dae443238246c39b3d98c16f9cdd2fe2d",
    "fidelity": "39ea1bb988edfff1913616d1f3c84de18eb2b6ae4a5132651234606c45575fbf",
    "pilot": "8ea1dce911a4c9cf163cabaf071cf15edd882722cd99d909a51dbdca45dd0850",
    "dynamic": "4f344ffb917bfab3c9e7fb158491038732096fa5d356ebec75e573385a9b8897",
    "cache": "c3f92f67808d62569f1802e0f185c1735a564442f676fd395052cbb35597bb8b",
    # CSS suite fixtures (HTML/CSS -> blob, same pinning rule as the corpus).
    "css_card": "32a1404eddf8a9914757b2e91088b8eb1c9eaab893f9d653826a7b24a9a334cd",
    "css_control": "f96eb1f06f42593d3b95937ef3e0afd4bc8224694388cf138eeca36ad524e309",
    "css_none": "71011fbc09890966e624e7d2eeb454f1697261eafdc3f395c274c1fda4e81ef9",
    # Group-opacity pixel fixtures (emitted by the css suite, verified there).
    "opacity": "b28fd9579a018d98d3f16a075e953f11a4e3a693f44a3901cc8591d6d54c3427",
    "opacity_deep": "d9db23445b426b1ad500efc261b2ed503567beee7917eeac1c3e1162923eeb27",
    # Position fixture (relative/absolute boxes, verified on d3d11 + wgpu).
    "position": "f635f249a8430064ee0783fa4497f035732cc9d55256da0825b186a9397815f3",
    # Fixed / sticky / static-item z-index fixtures (v4 position + v5 z-index).
    "fixed": "b2f6faababaa1d62f1e306e8f3451102280eb9b786ca40dfefd6fe8d98d44fb6",
    "sticky": "0ba5bd0e4a46f5f4d4e9d3b8fd277b749e8e6550441d2c5f2bddfb46bf3de1c6",
    "zitem": "69c46217c750a5594e1c15d06a19994424560158263c5c8ec8805cb5cc2b7817",
    # z-index / stacking-context fixture (blob v5, d3d11 + wgpu).
    "z": "28167c57e2b30fa6d23121126e714138c493d706a67676793ce119d8849b00dc",
    # CSS state fixture (blob v6, d3d11 + wgpu).
    "state": "0ac0005f064c9e5596d2a1873a422500209e89c2a2be183d3e8ddc7cf6bf7cdd",
    # Extended state fixture: nth-child, focus-visible/within, state weight,
    # ellipsis (blob v6 additive, d3d11 + wgpu).
    "state_ext": "289a213505e483dd2845f1d5d99d0776fda69cc94ad0c91e68a5d7b69a04504b",
    # Media fixture: kind 6 video nodes + the optional media section (blob v7).
    "media": "9fc5ef986adf55ef6b7309cd750210f07f0a0bcea704c2a48d4037524b0fd583",
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

    log_step("opacity: group-layer pixel fixture (d3d11" +
             (" + wgpu)" if wgpu_available() else ")"))
    opacity_suite = _suite_dir("opacity")
    opacity_page = opacity_suite / "page"
    _python(opacity_suite / "make_images.py", [], cwd=opacity_suite)
    opacity_fixture = out / "emit" / "opacity"
    opacity_fixture.mkdir(parents=True, exist_ok=True)
    opacity_blob = opacity_fixture / "app.wnui"
    rust_exe = get_paths().frontend_rust / "target" / "release" / "frontend-rust.exe"
    run_cmd(
        [rust_exe, "--html", str(opacity_page / "index.html"),
         "--css", str(opacity_page / "style.css"),
         "--width", "400", "--height", "500", "--emit-blob", str(opacity_blob)],
        cwd=opacity_page,
    )
    _assert_pinned_blob("opacity", opacity_blob)
    opacity_backends = ["d3d11"] + (["wgpu"] if wgpu_available() else [])
    for opacity_backend in opacity_backends:
        png = out / f"render_opacity_{opacity_backend}.png"
        report = out / f"layout_opacity_{opacity_backend}.json"
        environment = {"WN_RENDERER": opacity_backend}
        _run(card_exe, ["--document", str(opacity_blob),
                        "--report", str(report), "--sizes", "400x500"],
             opacity_page, environment)
        # Focus the input leaf and select all so the ring/selection/caret
        # passes (multi-pass leaf) are part of the pixel check.
        input_id = None
        report_json = json.loads(report.read_text(encoding="utf-8"))
        for size in report_json["sizes"]:
            for node in size["nodes"]:
                if str(node.get("name", "")).endswith("#leaf-input.leaf-input"):
                    input_id = node["id"]
                    break
            if input_id is not None:
                break
        if input_id is None:
            raise CliError("opacity fixture has no #leaf-input node")
        _run(card_exe, ["--document", str(opacity_blob),
                        "--render", "400", "500", str(png),
                        "--focus", str(input_id), "--select-all"],
             opacity_page, environment)
        # 21 checks: capture size, the group overlap trio, nested 0.5-in-0.5,
        # solid/text/image leaf alpha, the bordered/border-pair/shadow/input/
        # text-bg/image-bg multi-pass leaves, and the opacity-0 region.
        _expect_reported_checks(
            _python(opacity_suite / "verify_opacity.py",
                    ["--png", str(png), "--report", str(report),
                     "--backend", opacity_backend], cwd=opacity_suite),
            f"opacity verify ({opacity_backend})", 21)

    log_step("opacity: deep nesting (10 groups) on d3d11" +
             (" + wgpu" if wgpu_available() else ")"))
    deep_suite = _suite_dir("opacity_deep")
    deep_page = deep_suite / "page"
    deep_fixture = out / "emit" / "opacity_deep"
    deep_fixture.mkdir(parents=True, exist_ok=True)
    deep_blob = deep_fixture / "app.wnui"
    run_cmd(
        [rust_exe, "--html", str(deep_page / "index.html"),
         "--css", str(deep_page / "style.css"),
         "--width", "400", "--height", "300", "--emit-blob", str(deep_blob)],
        cwd=deep_page,
    )
    _assert_pinned_blob("opacity_deep", deep_blob)
    for deep_backend in opacity_backends:
        png = out / f"render_opacity_deep_{deep_backend}.png"
        report = out / f"layout_opacity_deep_{deep_backend}.json"
        environment = {"WN_RENDERER": deep_backend}
        _run(card_exe, ["--document", str(deep_blob),
                        "--report", str(report), "--sizes", "400x300"],
             deep_page, environment)
        _run(card_exe, ["--document", str(deep_blob),
                        "--render", "400", "300", str(png)],
             deep_page, environment)
        # 4 checks: capture size, the ten nested groups, the 0.9^10 pixel and
        # the untouched page background outside the stack.
        _expect_reported_checks(
            _python(deep_suite / "verify_opacity_deep.py",
                    ["--png", str(png), "--report", str(report),
                     "--backend", deep_backend], cwd=deep_suite),
            f"opacity deep verify ({deep_backend})", 4)

    log_step("position: relative/absolute pixel fixture (d3d11" +
             (" + wgpu)" if wgpu_available() else ")"))
    position_blob = _emit_css_fixture("position", page, 400, 200,
                                      out / "emit" / "position")
    _assert_pinned_blob("position", position_blob)
    position_backends = ["d3d11"] + (["wgpu"] if wgpu_available() else [])
    for position_backend in position_backends:
        png = out / f"render_position_{position_backend}.png"
        report = out / f"layout_position_{position_backend}.json"
        environment = {"WN_RENDERER": position_backend}
        _run(card_exe, ["--document", str(position_blob),
                        "--report", str(report), "--sizes", "400x200"],
             page, environment)
        _run(card_exe, ["--document", str(position_blob),
                        "--render", "400", "200", str(png)],
             page, environment)
        _expect_reported_checks(
            _python(suite / "verify_position.py",
                    ["--png", str(png), "--report", str(report),
                     "--blob", str(position_blob), "--dump", str(dump_exe),
                     "--backend", position_backend], cwd=suite),
            f"position verify ({position_backend})", 42)
    if wgpu_available():
        log_step("position: compare d3d11 and wgpu")
        compare = out / "position_compare.json"
        completed = subprocess.run(
            [
                str(get_python_exe()),
                str(runtime / "tests" / "d3d11" / "verify_d3d11.py"),
                "--capture", str(out / "render_position_d3d11.png"),
                "--reference", str(out / "render_position_wgpu.png"),
                "--report", str(compare),
                "--label", "position-d3d11-vs-wgpu",
            ],
            cwd=out,
            capture_output=True,
            text=True,
        )
        for line in (completed.stdout + completed.stderr).splitlines():
            if line.strip():
                log_info(line.rstrip())
        if completed.returncode != 0:
            raise CliError("position pixel comparison failed")

    log_step("fixed: viewport anchoring + scroll independence (d3d11" +
             (" + wgpu)" if wgpu_available() else ")"))
    fixed_blob = _emit_css_fixture("fixed", page, 400, 300,
                                   out / "emit" / "fixed")
    _assert_pinned_blob("fixed", fixed_blob)
    fixed_backends = ["d3d11"] + (["wgpu"] if wgpu_available() else [])
    for fixed_backend in fixed_backends:
        png = out / f"render_fixed_{fixed_backend}.png"
        scrolled = out / f"render_fixed_scrolled_{fixed_backend}.png"
        report = out / f"layout_fixed_{fixed_backend}.json"
        environment = {"WN_RENDERER": fixed_backend}
        _run(card_exe, ["--document", str(fixed_blob),
                        "--report", str(report), "--sizes", "400x300"],
             page, environment)
        _run(card_exe, ["--document", str(fixed_blob),
                        "--render", "400", "300", str(png)],
             page, environment)
        _run(card_exe, ["--document", str(fixed_blob),
                        "--render", "400", "300", str(scrolled),
                        "--wheel", "100", "100", "200"],
             page, environment)
        # 27 checks: dump position/insets, layout rects at scroll 0, the
        # analytic colors, and the same boxes plus the moved flow after wheel.
        _expect_reported_checks(
            _python(suite / "verify_fixed.py",
                    ["--png", str(png), "--scrolled", str(scrolled),
                     "--report", str(report), "--blob", str(fixed_blob),
                     "--dump", str(dump_exe), "--backend", fixed_backend],
                    cwd=suite),
            f"fixed verify ({fixed_backend})", 27)
    if wgpu_available():
        log_step("fixed: compare d3d11 and wgpu")
        compare = out / "fixed_compare.json"
        completed = subprocess.run(
            [
                str(get_python_exe()),
                str(runtime / "tests" / "d3d11" / "verify_d3d11.py"),
                "--capture", str(out / "render_fixed_d3d11.png"),
                "--reference", str(out / "render_fixed_wgpu.png"),
                "--report", str(compare),
                "--label", "fixed-d3d11-vs-wgpu",
            ],
            cwd=out,
            capture_output=True,
            text=True,
        )
        for line in (completed.stdout + completed.stderr).splitlines():
            if line.strip():
                log_info(line.rstrip())
        if completed.returncode != 0:
            raise CliError("fixed pixel comparison failed")

    log_step("sticky: scrollport pinning (d3d11" +
             (" + wgpu)" if wgpu_available() else ")"))
    sticky_blob = _emit_css_fixture("sticky", page, 400, 300,
                                    out / "emit" / "sticky")
    _assert_pinned_blob("sticky", sticky_blob)
    sticky_backends = ["d3d11"] + (["wgpu"] if wgpu_available() else [])
    for sticky_backend in sticky_backends:
        png = out / f"render_sticky_{sticky_backend}.png"
        scrolled = out / f"render_sticky_scrolled_{sticky_backend}.png"
        report = out / f"layout_sticky_{sticky_backend}.json"
        environment = {"WN_RENDERER": sticky_backend}
        _run(card_exe, ["--document", str(sticky_blob),
                        "--report", str(report), "--sizes", "400x300"],
             page, environment)
        _run(card_exe, ["--document", str(sticky_blob),
                        "--render", "400", "300", str(png)],
             page, environment)
        _run(card_exe, ["--document", str(sticky_blob),
                        "--render", "400", "300", str(scrolled),
                        "--wheel", "100", "100", "120"],
             page, environment)
        # 17 checks: dump position/inset, in-flow layout rects, the header and
        # stripe colors at scroll 0, then the pinned header and the stripes that
        # scrolled under it.
        _expect_reported_checks(
            _python(suite / "verify_sticky.py",
                    ["--png", str(png), "--scrolled", str(scrolled),
                     "--report", str(report), "--blob", str(sticky_blob),
                     "--dump", str(dump_exe), "--backend", sticky_backend],
                    cwd=suite),
            f"sticky verify ({sticky_backend})", 17)
    if wgpu_available():
        log_step("sticky: compare d3d11 and wgpu")
        compare = out / "sticky_compare.json"
        completed = subprocess.run(
            [
                str(get_python_exe()),
                str(runtime / "tests" / "d3d11" / "verify_d3d11.py"),
                "--capture", str(out / "render_sticky_scrolled_d3d11.png"),
                "--reference", str(out / "render_sticky_scrolled_wgpu.png"),
                "--report", str(compare),
                "--label", "sticky-d3d11-vs-wgpu",
            ],
            cwd=out,
            capture_output=True,
            text=True,
        )
        for line in (completed.stdout + completed.stderr).splitlines():
            if line.strip():
                log_info(line.rstrip())
        if completed.returncode != 0:
            raise CliError("sticky pixel comparison failed")

    log_step("z-index: static flex/grid items (d3d11" +
             (" + wgpu)" if wgpu_available() else ")"))
    zitem_blob = _emit_css_fixture("zitem", page, 400, 300,
                                   out / "emit" / "zitem")
    _assert_pinned_blob("zitem", zitem_blob)
    zitem_backends = ["d3d11"] + (["wgpu"] if wgpu_available() else [])
    for zitem_backend in zitem_backends:
        png = out / f"render_zitem_{zitem_backend}.png"
        report = out / f"layout_zitem_{zitem_backend}.json"
        environment = {"WN_RENDERER": zitem_backend}
        _run(card_exe, ["--document", str(zitem_blob),
                        "--report", str(report), "--sizes", "400x300"],
             page, environment)
        _run(card_exe, ["--document", str(zitem_blob),
                        "--render", "400", "300", str(png)],
             page, environment)
        # 42 checks: dump z-index/static position, nine exact rects, and the
        # analytic colors for flex 1/2 (both DOM orders), grid, and the trapped
        # nested context.
        _expect_reported_checks(
            _python(suite / "verify_zitem.py",
                    ["--png", str(png), "--report", str(report),
                     "--blob", str(zitem_blob), "--dump", str(dump_exe),
                     "--backend", zitem_backend], cwd=suite),
            f"zitem verify ({zitem_backend})", 42)
    if wgpu_available():
        log_step("z-index static items: compare d3d11 and wgpu")
        compare = out / "zitem_compare.json"
        completed = subprocess.run(
            [
                str(get_python_exe()),
                str(runtime / "tests" / "d3d11" / "verify_d3d11.py"),
                "--capture", str(out / "render_zitem_d3d11.png"),
                "--reference", str(out / "render_zitem_wgpu.png"),
                "--report", str(compare),
                "--label", "zitem-d3d11-vs-wgpu",
            ],
            cwd=out,
            capture_output=True,
            text=True,
        )
        for line in (completed.stdout + completed.stderr).splitlines():
            if line.strip():
                log_info(line.rstrip())
        if completed.returncode != 0:
            raise CliError("static-item z-index pixel comparison failed")

    log_step("z-index: stacking-context pixel fixture (d3d11" +
             (" + wgpu)" if wgpu_available() else ")"))
    z_blob = _emit_css_fixture("z", page, 400, 690, out / "emit" / "z")
    _assert_pinned_blob("z", z_blob)
    z_backends = ["d3d11"] + (["wgpu"] if wgpu_available() else [])
    for z_backend in z_backends:
        png = out / f"render_z_{z_backend}.png"
        report = out / f"layout_z_{z_backend}.json"
        environment = {"WN_RENDERER": z_backend}
        _run(card_exe, ["--document", str(z_blob),
                        "--report", str(report), "--sizes", "400x690"],
             page, environment)
        _run(card_exe, ["--document", str(z_blob),
                        "--render", "400", "690", str(png)],
             page, environment)
        # 110 checks: format wnui/7, twenty-seven z-index dump values, the
        # layout report, thirty-three exact rects, the PNG size and
        # forty-seven analytic color probes (siblings, swapped order, auto/0,
        # step 8, equal z, negative, trapped child, opacity+z, nested,
        # relative, ignored static z, the empty-clip escaping absolute and the
        # static opacity group bucket against flow and positioned z auto).
        _expect_reported_checks(
            _python(suite / "verify_z.py",
                    ["--png", str(png), "--report", str(report),
                     "--blob", str(z_blob), "--dump", str(dump_exe),
                     "--backend", z_backend], cwd=suite),
            f"z verify ({z_backend})", 110)
    if wgpu_available():
        log_step("z-index: compare d3d11 and wgpu")
        compare = out / "z_compare.json"
        completed = subprocess.run(
            [
                str(get_python_exe()),
                str(runtime / "tests" / "d3d11" / "verify_d3d11.py"),
                "--capture", str(out / "render_z_d3d11.png"),
                "--reference", str(out / "render_z_wgpu.png"),
                "--report", str(compare),
                "--label", "z-d3d11-vs-wgpu",
            ],
            cwd=out,
            capture_output=True,
            text=True,
        )
        for line in (completed.stdout + completed.stderr).splitlines():
            if line.strip():
                log_info(line.rstrip())
        if completed.returncode != 0:
            raise CliError("z-index pixel comparison failed")

    log_step("states: CSS state pixel fixture (d3d11" +
             (" + wgpu)" if wgpu_available() else ")"))
    state_blob = _emit_css_fixture("state", page, 400, 450,
                                   out / "emit" / "state")
    _assert_pinned_blob("state", state_blob)
    state_backends = ["d3d11"] + (["wgpu"] if wgpu_available() else [])
    for state_backend in state_backends:
        png = out / f"render_state_{state_backend}.png"
        report = out / f"layout_state_{state_backend}.json"
        environment = {"WN_RENDERER": state_backend}
        _run(card_exe, ["--document", str(state_blob),
                        "--report", str(report), "--sizes", "400x450"],
             page, environment)
        ids = {}
        report_json = json.loads(report.read_text(encoding="utf-8"))
        for item in report_json["sizes"][0]["nodes"]:
            for element_id in ("base", "hover", "press", "focus", "disabled",
                               "selected", "selected-hover", "sugar",
                               "css-wins"):
                if (f"#{element_id}." in item["name"] or
                        item["name"].endswith(f"#{element_id}")):
                    ids[element_id] = item["id"]
        missing = [name for name in ("base", "hover", "press", "focus",
                                     "disabled", "selected", "selected-hover",
                                     "sugar", "css-wins")
                   if name not in ids]
        if missing:
            raise CliError(f"state fixture is missing nodes: {missing}")
        force = [
            (ids["hover"], 0x01),
            (ids["press"], 0x02),
            (ids["focus"], 0x08),
            (ids["disabled"], 0x10),
            (ids["selected"], 0x04),
            (ids["selected-hover"], 0x04 | 0x01),
            (ids["sugar"], 0x01),
            (ids["css-wins"], 0x01),
        ]
        arguments = ["--document", str(state_blob),
                     "--render", "400", "450", str(png)]
        for node_id, mask in force:
            arguments += ["--force-state", f"{node_id}:{mask}"]
        _run(card_exe, arguments, page, environment)
        # 31 checks: capture size, the wnui/7 dump + four state styles, the
        # text/JSON initial_state (disabled/none), and background+border for
        # nine nodes on each backend.
        _expect_reported_checks(
            _python(suite / "verify_state.py",
                    ["--png", str(png), "--report", str(report),
                     "--blob", str(state_blob), "--dump", str(dump_exe),
                     "--backend", state_backend], cwd=suite),
            f"state verify ({state_backend})", 31)
    if wgpu_available():
        log_step("states: compare d3d11 and wgpu")
        compare = out / "state_compare.json"
        completed = subprocess.run(
            [
                str(get_python_exe()),
                str(runtime / "tests" / "d3d11" / "verify_d3d11.py"),
                "--capture", str(out / "render_state_d3d11.png"),
                "--reference", str(out / "render_state_wgpu.png"),
                "--report", str(compare),
                "--label", "state-d3d11-vs-wgpu",
            ],
            cwd=out,
            capture_output=True,
            text=True,
        )
        for line in (completed.stdout + completed.stderr).splitlines():
            if line.strip():
                log_info(line.rstrip())
        if completed.returncode != 0:
            raise CliError("state pixel comparison failed")

    log_step("state_ext: nth/focus-visible/focus-within/weight/ellipsis (d3d11" +
             (" + wgpu)" if wgpu_available() else ")"))
    ext_blob = _emit_css_fixture("state_ext", page, 400, 164,
                                 out / "emit" / "state_ext")
    _assert_pinned_blob("state_ext", ext_blob)
    ext_backends = ["d3d11"] + (["wgpu"] if wgpu_available() else [])
    for ext_backend in ext_backends:
        png = out / f"render_state_ext_{ext_backend}.png"
        report = out / f"layout_state_ext_{ext_backend}.json"
        environment = {"WN_RENDERER": ext_backend}
        _run(card_exe, ["--document", str(ext_blob),
                        "--report", str(report), "--sizes", "400x164"],
             page, environment)
        ids = {}
        report_json = json.loads(report.read_text(encoding="utf-8"))
        for item in report_json["sizes"][0]["nodes"]:
            leaf = item["name"].rsplit("/", 1)[-1]
            for element_id in ("focus-plain", "focus-visible", "within",
                               "weight"):
                if (f"#{element_id}." in leaf or
                        leaf.endswith(f"#{element_id}")):
                    ids[element_id] = item["id"]
        missing = [name for name in ("focus-plain", "focus-visible", "within",
                                     "weight") if name not in ids]
        if missing:
            raise CliError(f"state_ext fixture is missing nodes: {missing}")
        force = [
            (ids["focus-plain"], 0x08),
            (ids["focus-visible"], 0x08 | 0x20),
            (ids["within"], 0x40),
            (ids["weight"], 0x01),
        ]
        arguments = ["--document", str(ext_blob),
                     "--render", "400", "164", str(png)]
        for node_id, mask in force:
            arguments += ["--force-state", f"{node_id}:{mask}"]
        _run(card_exe, arguments, page, environment)
        # 17 checks: capture + wnui/7, the four new dump fields (visible,
        # within, weight, text-flags), JSON weight/text_flags, nine pixel probes
        # (nth row, focus pair, within, weight, ellipsis) and the real
        # single-line truncation (one-line run + glyph_vertices below the full
        # unbreakable token).
        _expect_reported_checks(
            _python(suite / "verify_state_ext.py",
                    ["--png", str(png), "--report", str(report),
                     "--blob", str(ext_blob), "--dump", str(dump_exe),
                     "--runtime", str(card_exe),
                     "--backend", ext_backend], cwd=suite),
            f"state_ext verify ({ext_backend})", 18)
    if wgpu_available():
        log_step("state_ext: compare d3d11 and wgpu")
        compare = out / "state_ext_compare.json"
        completed = subprocess.run(
            [
                str(get_python_exe()),
                str(runtime / "tests" / "d3d11" / "verify_d3d11.py"),
                "--capture", str(out / "render_state_ext_d3d11.png"),
                "--reference", str(out / "render_state_ext_wgpu.png"),
                "--report", str(compare),
                "--label", "state-ext-d3d11-vs-wgpu",
            ],
            cwd=out,
            capture_output=True,
            text=True,
        )
        for line in (completed.stdout + completed.stderr).splitlines():
            if line.strip():
                log_info(line.rstrip())
        if completed.returncode != 0:
            raise CliError("state_ext pixel comparison failed")

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

    log_step("fidelity: ABI v7 runtime layout overrides")
    override_exe = out / "layout_override_test.exe"
    _compile(
        override_exe,
        [_suite_dir("core") / "layout_override_test.c",
         *[runtime / source for source in CORE_SOURCES]],
        [get_paths().root / "include", runtime, out / "emit"],
        CORE_LIBS,
    )
    override_text = _run(override_exe, [], out)
    _expect(override_text, ["layout_override_test: PASS"], "layout override test")
    # 43 checks: size change/reflow/repaint, negative and WN_AUTO restore
    # (including a compiled percentage), zero size, absolute and relative
    # placement, right/bottom restoration, partial auto, clone base, and the
    # rejected handle/value/static cases.
    _expect_checks(override_text, "layout_override_test", 43)

    log_step("fidelity: ABI v7 runtime style overrides (color/background/font)")
    style_exe = out / "style_override_test.exe"
    _compile(
        style_exe,
        [_suite_dir("core") / "style_override_test.c",
         _suite_dir("text") / "fake_text.c",
         *[runtime / source for source in CORE_SOURCES]],
        [get_paths().root / "include", runtime, out / "emit"],
        CORE_LIBS,
    )
    style_text = _run(style_exe, [], out)
    _expect(style_text, ["style_override_test: PASS"], "style override test")
    # 49 checks: text-color paint + inheritance/priority/reset, background paint
    # and per-node reset, family/size/weight measurement + relayout + restore,
    # the app-wide recursive family layer (image box untouched), clone copy and
    # reset, and every rejected handle/value/mask leaving the last valid style
    # intact.
    _expect_checks(style_text, "style override test", 49)

    log_step("fidelity: ABI v7 header (focus/disabled bits)")
    header_exe = out / "abi_v7_header_check.exe"
    _compile(
        header_exe,
        [runtime / "examples" / "abi_header_check.c"],
        [get_paths().root / "include", runtime],
        CORE_LIBS,
    )
    _run(header_exe, [], out)
    log_info("ABI v7 header check: PASS")

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
    # 9 checks per kind entry (7 kinds) + 5 table-level checks. Deleting one
    # kind iteration drops this to 59, so the floor is the full 68.
    _expect_checks(kind_text, "kind ops test", 68)

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
    # 20 checks: direct vertical clamp bounds, vertical apply plus child
    # geometry (-offset, repeated no-op at max), non-finite rejection, the
    # inert x delta, the forced x clamp and apply, the forced x into-view
    # path, and the synthetic-core wheel.
    _expect_checks(scroll_text, "scroll service test", 20)

    log_step("fidelity: relative/absolute position (layout, clip, hit-test)")
    position_exe = out / "position_test.exe"
    _compile(
        position_exe,
        [_suite_dir("core") / "position_test.c",
         *[runtime / source for source in CORE_SOURCES]],
        [get_paths().root / "include", runtime, out / "emit"],
        CORE_LIBS,
    )
    position_text = _run(position_exe, [], out)
    _expect(position_text, ["position_test: PASS"], "position layout test")
    # Relative offsets plus subtree/scroll, absolute flow/containing-block/
    # stretch/nested cases, flex/grid/intrinsic exclusion for absolute and
    # fixed, the viewport fallback, the fully clipped ancestor escape,
    # scroll+clip+hit-test, the fixed viewport/scroll/clip cases, sticky
    # top/bottom/clamp, sticky+nested-scroller nearest, top-over-bottom,
    # content-box scrollport, hidden-not-a-scroller, sticky-with-fixed-child,
    # and the v4 loader validation through the extended enum.
    _expect_checks(position_text, "position_test", 140)

    log_step("fidelity: z-index stacking contexts (order, hit-test, layers)")
    zindex_exe = out / "zindex_test.exe"
    _compile(
        zindex_exe,
        [_suite_dir("core") / "zindex_test.c",
         *[runtime / source for source in CORE_SOURCES]],
        [get_paths().root / "include", runtime, out / "emit"],
        CORE_LIBS,
    )
    zindex_text = _run(zindex_exe, [], out)
    _expect(zindex_text, ["zindex_test: PASS"], "z-index stacking test")
    # 75 checks: positioned sibling z 1/2 and swapped, positioned z auto/0 in
    # both tree orders (step 8), negative z behind in-flow, a high-z child
    # trapped by a low-z parent, opacity+z texture-vs-ordering layers, nested
    # contexts, the equal-z tree-order tie-break (including reversed DOM), a
    # first-in-DOM relative z over a later in-flow sibling, static z ignored on
    # a block, static flex/grid items with z 1/2 (including swapped DOM and a
    # trapped nested context), non-positioned opacity groups in the step-8
    # bucket against a later in-flow sibling and against positioned z auto in
    # both tree orders, the empty-clip group with and without an unrelated
    # group, the flat no-context path, and the v5 loader sentinel/range
    # roundtrip.
    _expect_checks(zindex_text, "zindex_test", 75)

    log_step("fidelity: CSS state styles + ABI v7 focus/disabled")
    states_exe = out / "states_test.exe"
    _compile(
        states_exe,
        [_suite_dir("core") / "states_test.c",
         *[runtime / source for source in CORE_SOURCES]],
        [get_paths().root / "include", runtime, out / "emit"],
        CORE_LIBS,
    )
    states_text = _run(states_exe, [], out)
    _expect(states_text, ["states_test: PASS"], "state styles test")
    # 70 checks: the v6 colors/priority/selected/data-sugar/loader set, the
    # disabled-subtree cases (bits cleared across the subtree, input blurred,
    # char/key/paste rejected even with a leaked focus, interactive set_state
    # refused while disabled), the initial-disabled seed, plus the v7 focus
    # matrix (visible implies focused, clearing rules, multiple focused nodes,
    # remove/reinsert re-derives focus-within, disabled refuses visible), the
    # fixed focus-family color priority and the own focus-within weight that
    # must request relayout.
    _expect_checks(states_text, "state styles test", 70)

    log_step("fidelity: state font-weight + ellipsis (fake weight backend)")
    weight_state_exe = out / "state_weight_test.exe"
    _compile(
        weight_state_exe,
        [_suite_dir("core") / "state_weight_test.c",
         _suite_dir("text") / "fake_text.c",
         *[runtime / source for source in CORE_SOURCES]],
        [get_paths().root / "include", runtime, out / "emit"],
        CORE_LIBS,
    )
    weight_state_text = _run(weight_state_exe, [], out)
    _expect(weight_state_text, ["state_weight_test: PASS"],
            "state weight test")
    # 11 checks: base/effective weight, hover advance change and atlas key,
    # the relayout request, focus priority, ellipsis truncation, base restore
    # and flag-off geometry.
    _expect_checks(weight_state_text, "state_weight_test", 11)

    log_step("fidelity: blob v6 text_extras (ellipsis section)")
    extras_exe = out / "text_extras_test.exe"
    _compile(
        extras_exe,
        [_suite_dir("core") / "text_extras_test.c",
         *[runtime / source for source in CORE_SOURCES]],
        [get_paths().root / "include", runtime, out / "emit"],
        CORE_LIBS,
    )
    extras_text = _run(extras_exe, [], out)
    _expect(extras_text, ["text_extras_test: PASS"], "text extras loader test")
    # 8 checks: valid load, unknown/zero flags, node range, non-zero reserved
    # bytes (two positions), a section size that is not a whole record, and the
    # strictly increasing node order.
    _expect_checks(extras_text, "text_extras_test", 8)

    log_step("fidelity: paint item list (identity, transform, opacity, layers)")
    paint_exe = out / "paint_list_test.exe"
    _compile(
        paint_exe,
        [_suite_dir("core") / "paint_list_test.c",
         _suite_dir("text") / "fake_text.c",
         *[runtime / source for source in CORE_SOURCES]],
        [get_paths().root / "include", runtime, out / "emit"],
        CORE_LIBS,
    )
    paint_text = _run(paint_exe, [], out)
    _expect(paint_text, ["paint_list_test: PASS"], "paint list test")
    # 56 checks: item count/DFS order/defaults, known identity counts and a
    # byte-identical second build, solid/glyph/image translate, direct-alpha
    # boundaries (0, 1, >1, negative, 0.5 rounding) on solid and glyph plus the
    # image vertex alpha, group layers (leaf and container layers, parented
    # nested layers, command replay, opacity 0), leaf vs the same box with a
    # child, stable layer sort with its no-copy fast path, four culling
    # decisions, the .wnui offscreen-shadow-caster build fixture, and the
    # visibility rebuild.
    _expect_checks(paint_text, "paint list test", 56)

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


def _expect_reported_checks(text: str, label: str, minimum: int) -> None:
    """Requires the `<label>: N/M passed` summary with N == M and M >= minimum.

    The pixel verifiers count probes themselves instead of printing the
    `PASS (N checks)` line `_expect_checks` reads. The floor makes a deleted
    probe fail the suite instead of shrinking it silently.
    """
    prefix = f"{label}: "
    summary = None
    for line in text.splitlines():
        if line.startswith(prefix) and line.endswith(" passed"):
            summary = line
    if summary is None:
        raise CliError(f"{label} did not report a `/N passed` summary")
    counts = summary[len(prefix):-len(" passed")]
    try:
        passed_text, total_text = counts.split("/", 1)
        passed, total = int(passed_text), int(total_text)
    except ValueError as exc:
        raise CliError(
            f"{label} summary has an unreadable check count: {summary!r}"
        ) from exc
    if passed != total:
        raise CliError(f"{label} reported {passed}/{total} passing checks")
    if total < minimum:
        raise CliError(f"{label} ran only {total} checks (expected at least {minimum})")


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
        "ABI 8",
        "dynamic: PASS",
        "set_text(Omega) -> WN_OK",
        "set_image(text) -> WN_ERR_UNSUPPORTED",
        "set_state(unknown bit) -> WN_ERR_INVALID_ARGUMENT",
        "set_state(zero mask) -> WN_ERR_INVALID_ARGUMENT",
        "set_size(card,160,140) -> WN_OK",
        "set_size(card,auto) -> WN_OK",
        "layout size 120x120 -> 160x140 -> auto 120x120 -> restored",
        "set_position(static) -> WN_ERR_INVALID_ARGUMENT",
        "set_color(card) -> WN_OK",
        "set_background(card) -> WN_OK",
        "set_font_family(card) -> WN_OK",
        "set_font_size(card) -> WN_OK",
        "set_font_weight(card) -> WN_OK",
        "set_font_size(card,0) -> WN_ERR_INVALID_ARGUMENT",
        "reset_style(card) -> WN_OK",
        "app_set_font_family -> WN_OK",
        "app_set_font_family(auto) -> WN_OK",
        "set_state(disabled) -> WN_OK",
        "set_state(disabled off) -> WN_OK",
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

    log_step("input: IME composition (fake backend)")
    ime_exe = out / "ime_test.exe"
    _compile(
        ime_exe,
        [suite / "ime_test.c", _suite_dir("text") / "fake_text.c",
         *[runtime / item for item in CORE_SOURCES]],
        includes,
        CORE_LIBS,
    )
    ime_text = _run(ime_exe, [], suite)
    _expect(ime_text, ["ime_test: PASS"], "IME test")
    # The full set: focus/blur vtable + caret rect, START/COMPOSITION rendering
    # (glyph + underline), key suppression, Escape/explicit cancel, commit once,
    # no double insertion after a following char, commit over a selection,
    # disabled rejection and the focus switch. Deleting a probe drops this.
    _expect_checks(ime_text, "ime_test", 67)

    log_step("input: IME backend survives wn_view_replace_document")
    view_exe = out / "ime_view_test.exe"
    _compile(
        view_exe,
        [suite / "ime_view_test.c", _suite_dir("text") / "fake_text.c",
         *runtime_sources(skip_main=True)],
        includes,
        libs,
    )
    copy_wgpu_dll(out)
    view_text = _run(view_exe, [], suite)
    _expect(view_text, ["ime_view_test: PASS"], "IME view test")
    # View-level B1: focus before replace, old core blur, new core focus/caret,
    # explicit focus after replace, and a composition/commit on the new core.
    _expect_checks(view_text, "ime_view_test", 14)

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
        "ABI 8",
        "input: PASS",
        "set_state(focused|visible) -> WN_OK",
        "state(focused|visible)=0x28 (expected 0x28)",
        "set_state(focus-within) -> WN_ERR_INVALID_ARGUMENT",
        "pointer focus focused=1 visible=0",
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

    log_step("text: font-weight 100-900 (nearest face, measure/shape)")
    weight_exe = out / "weight_test.exe"
    _compile(
        weight_exe,
        [suite / "weight_test.cpp", runtime / "text" / "text_runtime.c",
         runtime / "text" / "text_directwrite.cpp"],
        [get_paths().root / "include", runtime],
        ["dwrite.lib"],
    )
    weight_text = _run(weight_exe, [], out)
    _expect_measure_result(weight_text, "text weight test")
    _expect_checks(weight_text, "text weight test", 43)

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
    log_info("blob v7: PASS")
    return 0


def run_media() -> int:
    suite = _suite_dir("media")
    out = suite / "out"
    page = suite / "page"
    fixture = out / "fixture"
    out.mkdir(parents=True, exist_ok=True)
    fixture.mkdir(parents=True, exist_ok=True)
    runtime = get_paths().runtime

    log_step("media: deterministic 300x150 poster")
    _python(suite / "make_poster.py", [], cwd=suite)

    log_step("media: frontend-rust --emit-blob (video element + media section)")
    blob = _blob_emit(page, fixture, 400, 640, True)
    _assert_pinned_blob("media", blob)

    includes = runtime_includes(fixture)
    libs = runtime_libs()
    objects = [runtime / item for item in CORE_SOURCES]

    log_step("media: blob v7 media section + malformed inputs")
    blob_exe = out / "media_blob_test.exe"
    _compile(blob_exe, [suite / "media_blob_test.c", *objects], includes,
             CORE_LIBS)
    blob_text = _run(blob_exe, [], out)
    _expect(blob_text, ["media_blob_test: PASS"], "media blob test")
    _expect_checks(blob_text, "media_blob_test", 26)

    log_step("media: HTML <video> -> blob -> load -> kind 6 + layout")
    layout_exe = out / "media_layout_test.exe"
    _compile(layout_exe, [suite / "media_layout_test.c", *objects], includes,
             CORE_LIBS)
    layout_text = _run(layout_exe, [str(blob)], out)
    _expect(layout_text, ["media_layout_test: PASS"], "media layout test")
    _expect_checks(layout_text, "media_layout_test", 25)

    log_step("media: media_null, provider order, handshake, event path")
    abi_exe = out / "media_abi_test.exe"
    _compile(
        abi_exe,
        [suite / "media_abi_test.c", *runtime_sources(skip_main=True)],
        includes,
        libs,
    )
    copy_wgpu_dll(out)
    abi_text = _run(abi_exe, [], out)
    _expect(abi_text, ["media_abi_test: PASS"], "media ABI test")
    _expect_checks(abi_text, "media_abi_test", 59)

    log_step("media: synthetic provider contract (PTS, seek, lease)")
    provider_exe = out / "media_test_provider_test.exe"
    _compile(
        provider_exe,
        [suite / "media_test_provider_test.c", runtime / "media" / "media_test.c"],
        includes,
        CORE_LIBS,
    )
    provider_text = _run(provider_exe, [], out)
    _expect(provider_text, ["media_test_provider_test: PASS"],
            "media synthetic provider test")
    _expect_checks(provider_text, "media_test_provider_test", 21)

    log_step("media: session state machine (inline, injected clock)")
    session_exe = out / "media_session_test.exe"
    _compile(
        session_exe,
        [suite / "media_session_test.c", runtime / "media" / "media_test.c",
         *objects],
        includes,
        CORE_LIBS,
    )
    session_text = _run(session_exe, [str(blob)], out)
    _expect(session_text, ["media_session_test: PASS"], "media session test")
    _expect_checks(session_text, "media_session_test", 46)

    log_step("media: offscreen RGBA frame path (poster -> frame -> seek)")
    frame_exe = out / "media_frame_test.exe"
    _compile(
        frame_exe,
        [suite / "media_frame_test.c", runtime / "media" / "media_test.c",
         *runtime_sources(skip_main=True)],
        includes,
        libs,
    )
    copy_wgpu_dll(out)
    frame_env = {"WN_RENDERER": "d3d11"}
    frame_text = _run(frame_exe, [str(blob), str(out)], page, frame_env)
    _expect(frame_text, ["media_frame_test: PASS"], "media frame test")
    _expect_checks(frame_text, "media_frame_test", 26)
    _expect_reported_checks(
        _python(suite / "verify_media_frames.py",
                ["--dir", str(out), "--backend", "d3d11"], cwd=suite),
        "media frames (d3d11)", 30,
    )

    log_step("media: wnui-dump projection (kind=video + media line)")
    dump_exe = out / "wnui-dump.exe"
    _compile(dump_exe, [runtime / "tools" / "wn_dump.c", *objects], includes,
             CORE_LIBS)
    dump_text = _run(dump_exe, [str(blob)], out)
    _expect(dump_text, [
        "format=wnui/7",
        " kind=video ",
        'media node=1 src="clips/poster.mp4" poster="posters/poster.png" '
        "flags=controls,autoplay,loop,muted preload=metadata",
    ], "media dump projection")
    dump_json = json.loads(_run(dump_exe, ["--json", str(blob)], out))
    media_nodes = [node for node in dump_json.get("nodes", [])
                   if node.get("kind") == "video"]
    if len(media_nodes) != 5:
        raise CliError(f"media dump has {len(media_nodes)} video nodes, expected 5")
    if media_nodes[0].get("media_src") != "clips/poster.mp4" or \
            media_nodes[0].get("media_poster") != "posters/poster.png":
        raise CliError("media dump lost the video src/poster")
    if media_nodes[0].get("media_flags") != ["controls", "autoplay", "loop",
                                             "muted"]:
        raise CliError("media dump lost the video flags")

    log_step("media: offscreen render (placeholder + poster)")
    runtime_exe = out / "runtime_media.exe"
    _compile(runtime_exe, runtime_sources(), includes, libs)
    copy_wgpu_dll(out)
    backends = ["d3d11"] + (["wgpu"] if wgpu_available() else [])
    for backend in backends:
        png = out / f"media_{backend}.png"
        report = out / f"layout_media_{backend}.json"
        environment = {"WN_RENDERER": backend}
        _run(runtime_exe, ["--report", str(report), "--sizes", "400x640"], page,
             environment)
        _run(runtime_exe, ["--render", "400", "640", str(png)], page,
             environment)
        _expect_reported_checks(
            _python(suite / "verify_media.py",
                    ["--png", str(png), "--report", str(report),
                     "--backend", backend], cwd=suite),
            f"media verify ({backend})", 21)
        log_info(f"media: {backend} png {png}")

    log_info("media v1 phase A: PASS")
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
    if document.get("format") != "wnui/7":
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
    "blob": ("blob v7 (.wnui load, layout and pixels)", run_blob),
    "media": ("media v1 phase A (video element, blob v7, media_null)", run_media),
    "dump": ("wnui-dump debug projection (text/JSON/errors)", run_dump),
}


def run_suite(name: str) -> int:
    entry = SUITES.get(name)
    if entry is None:
        raise CliError(f"unknown suite {name!r}; known: {', '.join(sorted(SUITES))}")
    log_step(f"suite: {name} - {entry[0]}")
    return entry[1]()
