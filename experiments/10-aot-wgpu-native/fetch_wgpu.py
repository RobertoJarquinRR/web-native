#!/usr/bin/env python3
"""Download and unpack the pinned wgpu-native Windows x64 MSVC release.

Build-time only: uses urllib (curl/wget are blocked in this environment).
"""

from __future__ import annotations

import io
import json
import sys
import urllib.request
import zipfile
from pathlib import Path

VERSION = "v29.0.1.1"
ASSET = "wgpu-windows-x86_64-msvc-release.zip"
URL = f"https://github.com/gfx-rs/wgpu-native/releases/download/{VERSION}/{ASSET}"

HERE = Path(__file__).resolve().parent
DEPS = HERE / "wgpu-native"


def main() -> int:
    DEPS.mkdir(parents=True, exist_ok=True)
    marker = DEPS / "VERSION.txt"
    if marker.exists() and (DEPS / "include" / "wgpu.h").exists() and (DEPS / "lib" / "wgpu_native.lib").exists():
        print(f"fetch_wgpu: already present ({marker.read_text(encoding='utf-8').strip()})")
        return 0

    print(f"fetch_wgpu: GET {URL}")
    with urllib.request.urlopen(URL, timeout=120) as response:
        payload = response.read()
    print(f"fetch_wgpu: downloaded {len(payload)} bytes")

    with zipfile.ZipFile(io.BytesIO(payload)) as archive:
        names = archive.namelist()
        print("fetch_wgpu: archive entries:")
        for name in names:
            print(f"  {name}")
        archive.extractall(DEPS)

    # The layout differs between releases; normalise to include/ lib/ bin/.
    want = {"wgpu.h": "include", "webgpu.h": "include", "wgpu_native.lib": "lib",
            "wgpu_native.dll": "bin"}
    found: dict[str, Path] = {}
    for path in DEPS.rglob("*"):
        if path.is_file() and path.name in want and path.name not in found:
            found[path.name] = path
    missing = [name for name in want if name not in found and name != "webgpu.h"]
    if missing:
        print(f"error: missing files in archive: {missing}", file=sys.stderr)
        return 1
    for name, dest in want.items():
        if name in found:
            target = DEPS / dest / name
            target.parent.mkdir(parents=True, exist_ok=True)
            if found[name].resolve() != target.resolve():
                found[name].replace(target)
    marker.write_text(f"{VERSION} {ASSET}\n", encoding="utf-8")
    print(f"fetch_wgpu: ready in {DEPS}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
