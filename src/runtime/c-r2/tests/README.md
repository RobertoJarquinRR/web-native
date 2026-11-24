# Pilot tests (ui-tree/2)

These are engine-side fixtures for grid, vertical scroll/clip, images and the
wheel event. They are independent of `benchmarks/pilot-media-library` (the
canonical 30-item pilot lives there and is exercised by
`build.ps1 -Pilot`).

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
The script also compiles the ABI window renderer (`renderer_wgpu.c`) against
this tree and runs `standalone --frames 20 --wheel-demo`, asserting that the
real `WM_MOUSEWHEEL` reaches the scroll container. `out/posters/` and `out/`
are generated (gitignored); the hand-authored `ui.json` is tracked.

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
