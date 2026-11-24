# R2 engine fixtures (ui-tree/2, ui-tree/3 and CSS core v1)

These are engine-side fixtures for grid, vertical scroll/clip, images, text
input and the CSS core (radius/border/shadow). They are independent of
`benchmarks/pilot-media-library` (the canonical 30-item pilot lives there and is
exercised by `build.ps1 -Pilot`).

## `pilot-media-library/`

Hand-authored `ui.json` (23 nodes): header, one `overflow:scroll` container
420 px high, a `repeat(auto-fill, 180px)` grid with 18 image cards
(cover/fill/contain, 120x90 posters) and a footer. `make_posters.py` generates
the PNGs, `make_ui.py` regenerates `ui.json`, `verify_pilot.py` checks 38
assertions (columns, object-fit, clip, wheel, clamp, outside-wheel).

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\pilot-media-library\run_pilot.ps1
```

Evidence: `out/render_1280.png`, `out/render_800.png`, `out/wheel_*.png`,
`out/pilot_layout.json`, `out/pilot_verify_report.json`, `out/abi_pilot.log`.
The script also compiles the ABI window renderers (`renderer_wgpu.c` and
`renderer_d3d11.c`) against this tree, runs `standalone --frames 20
--wheel-demo` on the wgpu path (asserting that the real `WM_MOUSEWHEEL` reaches
the scroll container), then captures the same 1280×800 page through D3D11
(`out/abi/capture_d3d11_1280.png`) and compares it with `render_1280.png`
(`out/abi/d3d11_compare_1280.json`). `out/posters/` and `out/` are generated
(gitignored); the hand-authored `ui.json` is tracked.

## `emitter-escapes/`

Regression for `emit_ui.py` string escaping: `×2160`, `Geometría`, `Órbita`,
`tecnología`, `montaña`, `geografía`, `astronomía`. The header must use
3-digit octal UTF-8 escapes, never `\xNN` adjacent to a hex digit.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\emitter-escapes\run_test.ps1
```

It emits `out/ui_data.h`, compiles `check.c` (which includes it), writes the
UTF-8 strings and compares the bytes with Python. No `split_c_escapes.py`
workaround is needed.

## `dynamic/`

Dynamic UI API v0 (ABI v3) fixture: a 400x300 page with two 120x120 cards
(image + text, `data-action="select"`). `page/ui.json` and `fixture/ui_data.h`
are tracked; `make_images.py` generates the two solid thumbnails;
`dynamic_core_test.c` is the headless core test; `runtime_dynamic.exe --dynamic`
renders 11 offscreen PNG stages; `examples/dynamic.c` is the C ABI window demo;
`verify_dynamic.py` checks geometry/counts/pixels (53/53).

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\dynamic\run_dynamic.ps1
# re-emit page\ui.json + fixture\ui_data.h with frontend-rust + emit_ui.py:
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\dynamic\run_dynamic.ps1 -Regenerate
```

Evidence: `out/stage_00.png` ... `out/stage_10.png`, `out/stage_report.json`,
`out/core_events.jsonl`, `out/dynamic_verify_report.json`, `out/abi_dynamic.log`
and `out/run_report.json` (generated, gitignored).

## `input/`

Single-line ASCII text input (`ui-tree/3`, ABI v4): a 480x240 page with two
inputs (`search` 320x34, `maxlength=12`; `empty` 220x28, `maxlength=5`).
`page/ui.json` is tracked; `fixture/ui_data.h` is generated (the global
`.gitignore` ignores `ui_data.h`, so force-add it like `tests/dynamic/fixture`
if the checkout must run without the Rust binary). `input_core_test.c` is the
headless core test; `runtime_input.exe --input` renders 10 offscreen PNG stages
with `stage_report.json` (value/cursor/selection/caret); `examples/input.c` is
the C ABI v4 demo (set/get value, WN_EVENT_INPUT, clipboard, Esc policy);
`verify_input.py` checks stage values and text/caret/selection/focus pixels
(111/111).

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\input\run_input.ps1
# re-emit page\ui.json + fixture\ui_data.h (frontend-rust --pilot + --ui-json):
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\input\run_input.ps1 -Regenerate
```

Evidence: `out/stage_00.png` ... `out/stage_09.png`, `out/stage_report.json`,
`out/core_events.jsonl`, `out/input_verify_report.json`, `out/runtime_input.log`,
`out/window_input.log`, `out/abi_input.log` and `out/run_report.json`
(generated, gitignored).

## `css/`

CSS core v1 (`ui-tree/2`/`/3` style fields): rounded solid backgrounds,
uniform borders, the first non-inset shadow, and rounded textured images.
`page/*.json` are hand-authored emitters fixtures; `fixture/legacy_ui_data.h`
is the old rectangular control; `expected/control_legacy.png` is the tracked
golden of the pre-CSS renderer (`.gitignore` has an explicit exception for it).
`run_css.ps1` re-emits `ui_data.h`, compiles the runtime with `/W4 /WX`, renders
400x240, and `verify_css.py` checks 41 geometry/pixel invariants, including a
byte-identical control comparison.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\css\run_css.ps1
```

Evidence: `out/render_card.png`, `out/render_control.png`,
`out/layout_card.json`, `out/css_verify_report.json`, `out/run_report.json`
(generated, gitignored). Result: **41/41 PASS**.

## `offscreen/`

Offscreen target through the renderer vtable (`WN_SURFACE_OFFSCREEN` +
`readback`), without a window. This is the path tooling and the visual-parity
harness use:

- `offscreen_test.c` creates a view on a texture, loads the compiled fixture,
  renders one frame and writes it with the backend readback.
- `run_offscreen.ps1` compiles it with `/W4 /WX`, runs it with
  `WN_RENDERER=d3d11` and `WN_RENDERER=wgpu`, and compares both PNGs.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\offscreen\run_offscreen.ps1
```

Last run: **PASS**, 960x640, max channel diff **3**, 99.83 % exact, 0 % beyond
tolerance.

## `d3d11/`

Acceptance for the second renderer backend (`renderer_d3d11.c`). It proves the
`wn_renderer_ops` vtable is not wgpu-specific:

- `run_d3d11.ps1` builds `build_abi` (both backends linked), captures a D3D11
  window frame, renders the same compiled document with wgpu offscreen and
  compares the PNGs (`verify_d3d11.py`: per-channel tolerance, bad-pixel
  fraction and blank-frame guard). It also runs `standalone` and `embedded` with
  `WN_RENDERER=wgpu` to keep the original path selectable, and captures the
  pilot image page (18 WIC images) through D3D11.
- `measure_cpu.ps1` posts synthetic `WM_MOUSEMOVE` messages over the four
  interactive nav items and measures process CPU for both renderers.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\d3d11\run_d3d11.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\d3d11\measure_cpu.ps1
```

Evidence: `out/d3d11_960x640.png`, `out/wgpu_960x640.png`,
`out/d3d11_compare.json`, `out/d3d11_pilot_1280x800.png`,
`out/d3d11_compare_pilot.json`, `out/d3d11_run.log`, `out/d3d11_report.json`
and `out/cpu_measure.json` (generated, gitignored). Last run on this machine:
base fixture max channel diff **3**, 99.83 % exact; pilot image page max **1**,
99.76 % exact; hover CPU **0.011–0.033 cores (D3D11)** vs **0.725–0.756 cores
(wgpu/WARP)**.

## `fidelity/`

E1 visual/layout fidelity (`font-weight`, `text-align`, `line-height`,
`margin: auto` and the scrollbar/focus-ring paint paths) checked at the core
level, without a window:

- `fidelity_core_test.c` loads the compiled fixture, runs the core layout,
  asserts the new geometry/paint invariants and also proves the guarded header:
  `ui_data.h` for a page that does not use the new properties stays
  byte-identical (checked against `tests/input/fixture/ui_data.h`).
- `page/index.html` exercises bold weights, centered/right text, explicit
  line-heights, a flex column with `margin-top: auto` and an overflowing
  scroll region.
- `run_fidelity.ps1` emits the page, compiles the test with `/W4 /WX`, runs it
  and writes the render evidence.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\fidelity\run_fidelity.ps1
```

Evidence: `out/render.png`, `out/render_wheel.png`, `out/ui.json`,
`out/emit/ui_data.h`, `out/fidelity_core_test.exe` (generated, gitignored).
Last run: **23/23 checks PASS**, old fixture headers byte-identical.
