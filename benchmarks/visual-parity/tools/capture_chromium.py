#!/usr/bin/env python3
"""Capture the Chromium reference PNG with Playwright (dev-only).

This script is deliberately outside the runtime: it only runs when a developer
executes the visual-parity benchmark. It never becomes a product dependency.

The reference is Playwright's pinned Chromium headless shell, with the flags
from docs/visual-parity.md: software raster (--disable-gpu), sRGB, DPR 1,
grayscale text AA (--disable-lcd-text), hidden scrollbars and no animations or
caret. Fonts declared by the fixture are checked against the OS font files so a
missing font fails instead of silently comparing a fallback.
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

LAUNCH_ARGS = [
    "--disable-gpu",
    "--hide-scrollbars",
    "--force-device-scale-factor=1",
    "--force-color-profile=srgb",
    "--disable-lcd-text",
    "--no-first-run",
    "--no-default-browser-check",
    "--disable-extensions",
    "--disable-background-networking",
    "--disable-sync",
    "--mute-audio",
]

FONT_FILES: dict[str, list[str]] = {
    "Segoe UI": ["segoeui.ttf"],
    "Segoe UI Semibold": ["seguisb.ttf"],
    "Segoe UI Bold": ["segoeuib.ttf"],
    "Consolas": ["consola.ttf"],
    "Consolas Bold": ["consolab.ttf"],
}

DISABLE_STYLE = """
*, *::before, *::after {
  animation: none !important;
  animation-delay: 0s !important;
  animation-duration: 0s !important;
  transition: none !important;
  transition-delay: 0s !important;
  transition-duration: 0s !important;
  caret-color: transparent !important;
}
html { scroll-behavior: auto !important; }
"""

WAIT_JS = """
async () => {
  if (document.fonts && document.fonts.ready) {
    await document.fonts.ready;
  }
  const images = Array.from(document.images);
  await Promise.all(images.map((img) => {
    if (img.complete) return Promise.resolve();
    return new Promise((resolve) => {
      img.addEventListener('load', resolve, { once: true });
      img.addEventListener('error', resolve, { once: true });
      setTimeout(resolve, 5000);
    });
  }));
  await new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));
  return {
    images: images.length,
    brokenImages: images.filter((img) => !img.naturalWidth).map((img) => img.currentSrc || img.src),
    fontsStatus: document.fonts ? document.fonts.status : 'unavailable',
    devicePixelRatio: window.devicePixelRatio,
    innerWidth: window.innerWidth,
    innerHeight: window.innerHeight,
    clientWidth: document.documentElement.clientWidth,
    clientHeight: document.documentElement.clientHeight,
    scrollWidth: document.documentElement.scrollWidth,
    scrollHeight: document.documentElement.scrollHeight,
    title: document.title
  };
}
"""


class DependencyError(RuntimeError):
    """Playwright or its browser is not installed."""


def _ascii_message(text: str) -> str:
    """Keep launch errors readable in Windows consoles without breaking paths."""
    cleaned = "".join(
        char if (char in "\n\t" or 32 <= ord(char) < 127) else " " for char in text
    )
    return "\n".join(line.rstrip() for line in cleaned.splitlines() if line.strip())


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", help="fixture name from fixtures.json")
    parser.add_argument(
        "--fixtures",
        nargs="*",
        default=None,
        help="fixture names for --check (default: all)",
    )
    parser.add_argument("--fixtures-json", default=None, help="path to fixtures.json")
    parser.add_argument("--out", required=True, help="PNG output path (capture) or output dir (--check)")
    parser.add_argument("--meta", default=None, help="metadata JSON output path")
    parser.add_argument("--check", action="store_true", help="only verify Playwright/Chromium/fonts")
    parser.add_argument("--timeout-ms", type=int, default=30000)
    return parser.parse_args(argv)


def _playwright_api():
    try:
        from playwright.sync_api import sync_playwright
    except ImportError as exc:  # pragma: no cover - exercised only on broken installs
        raise DependencyError(
            "the Python package 'playwright' is not installed in the harness venv. "
            "Run: benchmarks\\visual-parity\\.venv\\Scripts\\python.exe -m pip install "
            "-r benchmarks\\visual-parity\\requirements.txt"
        ) from exc
    return sync_playwright


def _headless_shell_path(pw) -> Path | None:
    """Resolve the headless shell that Playwright actually uses.

    browser_type.executable_path points at the full Chromium build even when
    headless mode launches chrome-headless-shell. Passing the shell explicitly
    makes the capture reproducible and lets the report hash the real binary.
    """
    try:
        full = Path(pw.chromium.executable_path)
    except Exception:
        return None
    build_dir = full.parent.parent if full.parent.name == "chrome-win" else full.parent
    name = build_dir.name
    if not name.startswith("chromium-"):
        return None
    build = name.split("-", 1)[1]
    shell_dir = build_dir.parent / f"chromium_headless_shell-{build}"
    if not shell_dir.exists():
        return None
    for pattern in (
        "chrome-headless-shell.exe",
        "headless_shell.exe",
        "chrome-headless-shell",
        "headless_shell",
    ):
        matches = list(shell_dir.rglob(pattern))
        if matches:
            return matches[0]
    return None


def _launch(pw):
    try:
        kwargs: dict[str, Any] = {"headless": True, "args": LAUNCH_ARGS}
        shell = _headless_shell_path(pw)
        if shell:
            kwargs["executable_path"] = str(shell)
        browser = pw.chromium.launch(**kwargs)
        return browser, str(shell) if shell else None
    except Exception as exc:  # Playwright raises its own Error subclasses
        message = _ascii_message(str(exc))
        action = (
            "Install the pinned browser with: "
            "benchmarks\\visual-parity\\.venv\\Scripts\\python.exe -m playwright "
            "install chromium-headless-shell"
        )
        if "Executable doesn't exist" in message or "playwright install" in message:
            raise DependencyError(f"Playwright Chromium is not installed. {action}\n{message}") from exc
        raise DependencyError(f"Chromium failed to launch. {action}\n{message}") from exc


def check_fonts(families: list[str]) -> list[dict[str, str]]:
    fonts_dir = Path(os.environ.get("WINDIR", r"C:\Windows")) / "Fonts"
    missing: list[dict[str, str]] = []
    for family in families:
        candidates = FONT_FILES.get(family)
        if not candidates:
            missing.append({"family": family, "reason": "not in the harness font allowlist"})
            continue
        if not any((fonts_dir / name).exists() for name in candidates):
            missing.append(
                {
                    "family": family,
                    "reason": "missing " + ", ".join(str(fonts_dir / n) for n in candidates),
                }
            )
    return missing


def _browser_info(
    browser, cache_dir: Path, actual_executable: str | None = None
) -> dict[str, Any]:
    browser_type = browser.browser_type
    default_executable = getattr(browser_type, "executable_path", None)
    executable = actual_executable or default_executable
    info: dict[str, Any] = {
        "name": browser_type.name,
        "version": browser.version,
        "executable": executable,
        "default_executable": default_executable,
        "headless": True,
        "launch_args": LAUNCH_ARGS,
    }
    if executable and Path(executable).exists():
        info["executable_sha256"] = c.cached_sha256(executable, cache_dir)
        info["executable_size"] = Path(executable).stat().st_size
        build = None
        for part in Path(executable).parts:
            if part.startswith("chromium_headless_shell-"):
                build = part.rsplit("-", 1)[-1]
            elif part.startswith("chromium-") and build is None:
                build = part.rsplit("-", 1)[-1]
        info["playwright_browser_build"] = build
    info["playwright_version"] = c.package_version("playwright")
    pin_path = c.harness_dir() / "chromium.json"
    if pin_path.exists():
        try:
            pin = c.load_json(pin_path)
        except (OSError, ValueError):
            pin = None
        if pin:
            matches = {
                "playwright": str(pin.get("playwright")) == str(info.get("playwright_version")),
                "version": str(pin.get("version")) == str(info.get("version")),
                "build": str(pin.get("playwright_browser_build"))
                == str(info.get("playwright_browser_build")),
                "sha256": str(pin.get("executable_sha256"))
                == str(info.get("executable_sha256")),
            }
            info["pinned"] = {
                "playwright": pin.get("playwright"),
                "version": pin.get("version"),
                "build": pin.get("playwright_browser_build"),
                "sha256": pin.get("executable_sha256"),
            }
            info["pin_match"] = matches
            info["pin_ok"] = all(matches.values())
    return info


def _fixture_selection(config: dict[str, Any], args: argparse.Namespace) -> list[dict[str, Any]]:
    _, fixtures = c.load_fixtures(args.fixtures_json)
    mapping = c.fixture_map(fixtures)
    if args.fixture:
        if args.fixture not in mapping:
            raise ValueError(f"unknown fixture {args.fixture!r}; known: {', '.join(mapping)}")
        return [mapping[args.fixture]]
    if args.check and args.fixtures:
        names = c.split_names(args.fixtures)
        unknown = [name for name in names if name not in mapping]
        if unknown:
            raise ValueError(f"unknown fixtures: {', '.join(unknown)}")
        return [mapping[name] for name in names]
    if args.check:
        return fixtures
    raise ValueError("--fixture is required unless --check is used")


def _check(args: argparse.Namespace, out_dir: Path) -> int:
    config, fixtures = c.load_fixtures(args.fixtures_json)
    selected = _fixture_selection(config, args)
    families: list[str] = []
    for fixture in selected:
        c.validate_no_reasonless_excludes(fixture)
        for family in fixture.get("fonts", []):
            if family not in families:
                families.append(family)
    missing_fonts = check_fonts(families)

    sync_playwright = _playwright_api()
    browser_info: dict[str, Any] = {}
    error = None
    try:
        with sync_playwright() as pw:
            browser, actual_executable = _launch(pw)
            try:
                browser_info = _browser_info(
                    browser, out_dir / ".cache", actual_executable
                )
            finally:
                browser.close()
    except DependencyError as exc:
        error = str(exc)

    result: dict[str, Any] = {
        "ok": error is None and not missing_fonts,
        "generated_at": c.utc_now(),
        "playwright": c.package_version("playwright"),
        "browser": browser_info or None,
        "fonts": {"checked": families, "missing": missing_fonts},
        "error": error,
    }
    meta_path = Path(args.meta) if args.meta else out_dir / "meta" / "chromium.json"
    c.write_json(meta_path, result)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if error or missing_fonts:
        if missing_fonts:
            print(
                "CHROMIUM CHECK FAILED: missing fixture fonts: "
                + ", ".join(item["family"] for item in missing_fonts),
                file=sys.stderr,
            )
        if error:
            print(f"CHROMIUM CHECK FAILED: {error}", file=sys.stderr)
        return 1
    return 0


def _capture(args: argparse.Namespace, out_path: Path, meta_path: Path) -> int:
    config, fixtures = c.load_fixtures(args.fixtures_json)
    selected = _fixture_selection(config, args)
    fixture = selected[0]
    c.validate_no_reasonless_excludes(fixture)

    missing_fonts = check_fonts(list(fixture.get("fonts", [])))
    if missing_fonts:
        raise RuntimeError(
            "fixture font check failed: "
            + ", ".join(item["family"] for item in missing_fonts)
        )

    html_path = Path(fixture["html_path"]).resolve()
    if not html_path.exists():
        raise FileNotFoundError(f"fixture HTML not found: {html_path}")
    width = int(fixture["viewport"]["width"])
    height = int(fixture["viewport"]["height"])
    out_path.parent.mkdir(parents=True, exist_ok=True)

    sync_playwright = _playwright_api()
    started = time.perf_counter()
    blocked: list[str] = []
    failed_requests: list[str] = []

    with sync_playwright() as pw:
        browser, actual_executable = _launch(pw)
        try:
            browser_info = _browser_info(
                browser, c.output_root(out_path) / ".cache", actual_executable
            )
            context = browser.new_context(
                viewport={"width": width, "height": height},
                screen={"width": width, "height": height},
                device_scale_factor=1,
                color_scheme="light",
                locale="en-US",
                timezone_id="UTC",
                reduced_motion="reduce",
                service_workers="block",
                ignore_https_errors=False,
            )
            context.route("http://**", lambda route: _block_route(route, blocked))
            context.route("https://**", lambda route: _block_route(route, blocked))
            page = context.new_page()
            page.on("requestfailed", lambda request: failed_requests.append(request.url))
            page.emulate_media(reduced_motion="reduce")
            page.goto(html_path.as_uri(), wait_until="load", timeout=args.timeout_ms)
            page.add_style_tag(content=DISABLE_STYLE)
            diagnostics = page.evaluate(WAIT_JS)
            if int(diagnostics.get("devicePixelRatio", 0)) != 1:
                raise RuntimeError(
                    "Chromium devicePixelRatio is not 1: "
                    f"{diagnostics.get('devicePixelRatio')}"
                )
            if diagnostics.get("brokenImages"):
                raise RuntimeError(
                    "fixture has images that failed to load: "
                    + ", ".join(diagnostics["brokenImages"])
                )
            page.wait_for_timeout(50)
            page.screenshot(
                path=str(out_path),
                full_page=False,
                animations="disabled",
                caret="hide",
                omit_background=False,
                type="png",
            )
            context.close()
        finally:
            browser.close()

    actual_width, actual_height, mode = c.image_size(out_path)
    if (actual_width, actual_height) != (width, height):
        out_path.unlink(missing_ok=True)
        raise RuntimeError(
            f"screenshot size mismatch: expected {width}x{height}, got "
            f"{actual_width}x{actual_height}"
        )

    duration_ms = round((time.perf_counter() - started) * 1000.0, 2)
    meta = {
        "ok": True,
        "kind": "chromium",
        "generated_at": c.utc_now(),
        "fixture": fixture["name"],
        "html": str(html_path),
        "html_sha256": c.sha256_file(html_path),
        "style_sha256": (
            c.sha256_file(fixture["style_path"]) if fixture.get("style_path") else None
        ),
        "browser": browser_info,
        "viewport": {"width": width, "height": height},
        "device_scale_factor": 1,
        "dpi": c.DEFAULT_DPI,
        "color_profile": "srgb",
        "flags": {
            "disable_gpu": True,
            "hide_scrollbars": True,
            "force_device_scale_factor": 1,
            "force_color_profile": "srgb",
            "disable_lcd_text": True,
            "animations": "disabled",
            "caret": "hidden",
        },
        "page": diagnostics,
        "blocked_requests": blocked,
        "failed_requests": failed_requests,
        "screenshot": {
            "path": str(out_path),
            "sha256": c.sha256_file(out_path),
            "width": actual_width,
            "height": actual_height,
            "mode": mode,
            "bytes": out_path.stat().st_size,
        },
        "duration_ms": duration_ms,
    }
    c.write_json(meta_path, meta)
    print(
        f"chromium: {fixture['name']} {actual_width}x{actual_height} -> {out_path} "
        f"({c.package_version('playwright')} / {browser_info.get('version')})"
    )
    return 0


def _block_route(route, blocked: list[str]) -> None:
    blocked.append(route.request.url)
    route.abort()


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    fixtures_json = args.fixtures_json
    if fixtures_json is None:
        fixtures_json = str(c.harness_dir() / "fixtures.json")
    args.fixtures_json = fixtures_json
    out = Path(args.out)
    if not out.is_absolute():
        out = (Path.cwd() / out).resolve()

    try:
        if args.check:
            return _check(args, out)
        out_path = out
        meta_path = Path(args.meta) if args.meta else (
            c.output_root(out_path) / "meta" / f"{args.fixture}.chromium.json"
        )
        if not meta_path.is_absolute():
            meta_path = (Path.cwd() / meta_path).resolve()
        return _capture(args, out_path, meta_path)
    except DependencyError as exc:
        print(f"CHROMIUM NOT AVAILABLE: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:
        print(f"CHROMIUM CAPTURE FAILED: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
