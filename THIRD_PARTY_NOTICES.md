# Third-party notices

webnative includes or depends on the following third-party components. The
project itself is licensed under the MIT license (see `LICENSE`).

## wgpu-native v29.0.1.1

- Upstream: <https://github.com/gfx-rs/wgpu-native>
- License: MIT OR Apache-2.0
- Vendored files: the headers under `third_party/wgpu-native/include/`.
- The runtime DLL and the import library (`wgpu_native.dll` and
  `wgpu_native.dll.lib`) are **not committed**. Run
  `tools/fetch-wgpu.ps1` to download the pinned release and verify its
  SHA-256 before building or running the engine.
- License texts: `third_party/wgpu-native/LICENSE.MIT` and
  `third_party/wgpu-native/LICENSE.APACHE`.

## Gumbo 0.10.1

- Upstream: <https://github.com/google/gumbo-parser>
- License: Apache-2.0
- Vendored files: `src/frontend/cpp/third_party/gumbo/`.
- License text: `src/frontend/cpp/third_party/gumbo/COPYING`.

## Fonts

The engine uses fonts installed on the host system. No font files are
redistributed with this repository.

## Rust crates and .NET packages

Dependencies are declared in `Cargo.lock` and the `*.csproj` files. They are
restored by Cargo/NuGet and are not vendored in this repository.

## Trademarks

wgpu-native, Gumbo and the names of their contributors are used only to
identify the upstream projects. This project is not affiliated with or
endorsed by them.
