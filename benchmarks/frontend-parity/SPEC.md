# Frontend parity benchmark — specification

> **Estado: congelado (subset v1, 2026-11-24).** Este SPEC es el contrato de
> paridad de **C++ y C#**. Rust es el frontend canónico y puede avanzar con
> features nuevas (grid, input, CSS v3); C++/C# no tienen obligación de
> seguirlas. Python es el oráculo de este mismo subset v1. Ver
> [`results/README.md`](results/README.md).

Goal: implement the same HTML+CSS frontend in different languages and prove
that (a) it produces the **same IR**, and (b) measure speed and cost.

The reference implementation is `src/frontend/python` (the v1 package).
The other implementations must match its output for the two fixtures below,
within **0.01 px** geometry tolerance.

## Política de mantenimiento

- **Rust** (`src/frontend/rust`) es el frontend canónico y evoluciona con el
  producto: grid, scroll, imágenes, input y CSS v3.
- **Python** (`src/frontend/python`) es el oráculo del subset v1; se mantiene
  para goldens y verificación, no para seguir features nuevas de Rust.
- **C++** y **C#** son **referencias de paridad congeladas** en este SPEC. No
  se actualizan para seguir a Rust; su contrato es producir el mismo IR en los
  fixtures de este documento.
- Un cambio incompatible del IR o del subset requiere una decisión explícita:
  actualizar los frontends congelados o declararlos fuera del subset actual.
  No se actualizan en silencio.
- Los resultados se versionan en [`results/README.md`](results/README.md) con
  fecha, commit y subset.

## Fixtures

| Name | HTML | Viewport | What it exercises |
|---|---|---|---|
| `contract` | `fixtures/contract/input.html` | 400×300 | block flow, padding, margin-top, id/class selectors |
| `flex-page` | `fixtures/flex-page/index.html` | 480×320 | nested flex row/column, gap, space-between, align center, margins, padding |

Both fixtures are **rect-only**: no text nodes. Text is out of scope for this
round (the IR may ignore text; the fixtures contain none).

## CLI contract

Every implementation must provide a `run.ps1` in its directory:

```
run.ps1 -Html <path> -Width <W> -Height <H> -Out <ir.json> [-Timings <timings.json>]
```

Rules:

- Resolve `<link rel="stylesheet" href="...">` relative to the HTML file.
- Write the IR JSON to `-Out`.
- Write `-Timings` JSON if provided (parse/cascade/layout/emit ms + `ops`).
  `peak_rss_kb` is optional.
- Exit code 0 on success, non-zero on error.
- No browser, WebView, Skia or Electron.

The harness invokes `run.ps1` via PowerShell; the implementation may call any
binary or script.

## IR output

```json
{
  "viewport": { "width": 480, "height": 320 },
  "ops": [
    { "type": "rect", "x": 0.0, "y": 0.0, "w": 480.0, "h": 320.0, "fill": "#0f1115" }
  ]
}
```

Rules that must match the Python reference exactly:

1. **One `rect` op per element that has a `background`/`background-color`**,
   plus the `body` background (emitted first).
2. **Painter's order: pre-order** — parent background, then children in
   document order.
3. Colors: lowercase `#rrggbb` (accept `#rgb` input, expand it).
4. Geometry: floats in CSS px; tolerance 0.01.
5. **Border-box**: `width`/`height` include `padding` (the Python v1 behavior).
6. `body { margin: 0 }` puts the root at (0,0).
7. Auto width of a block child fills the parent's content box.
8. Auto height comes from children (block) / flex line height / wrap.
9. Whitespace-only text nodes are ignored.

## CSS subset to implement

Selectors:
- type (`div`), class (`.card`), id (`#root`), compound (`div.card`),
- descendant (`.cards .card`), comma lists,
- specificity `(ids, classes, types)`, then source order.

Properties:
- `display: block | flex`
- `flex-direction: row | column`
- `gap: <px>`
- `justify-content: flex-start | center | flex-end | space-between`
- `align-items: stretch | flex-start | center | flex-end`
- `width`, `height` in px
- `padding` shorthand in px; `margin` shorthand and `margin-top` in px
- `background` / `background-color`: `#rgb`, `#rrggbb`
- `color`, `font-size`, `font-family`: parsed but ignored (no text in fixtures)

Anything outside the subset may be ignored, but must not crash the parser.

## Layout algorithm (must match Python v1)

- Block: children stack at `content_y`; `cursor_y = child.bottom + margin_bottom`.
- Flex row: content width = parent content box; used = sum(child widths) + gaps;
  free = max(content - used, 0).
  - `flex-start`: start 0, spacing gap.
  - `center`: start free/2, spacing gap.
  - `flex-end`: start free, spacing gap.
  - `space-between`: start 0, spacing = gap + free/(n-1) (n>1).
- Flex column: same on the vertical axis; `align-items` centers/shifts on x.
- `align-items: center`: cross offset = (line_height - child_height)/2.
- Fixed height containers clip nothing (ops can extend beyond; the fixtures fit).
- No margin collapsing, no borders, no floats, no grid, no percentages.

## Verification

```
python benchmarks/frontend-parity/tools/verify_ir.py \
  benchmarks/frontend-parity/golden/<fixture>.ir.json \
  benchmarks/frontend-parity/results/<lang>/<fixture>.ir.json
```

Must print `PASS`.

## Benchmark

```
python benchmarks/frontend-parity/tools/bench.py --runs 10
```

It runs every frontend that has a `run.ps1`, verifies parity, and writes
`benchmarks/frontend-parity/results/SUMMARY.md` with wall-time medians.

Report also, in your README:

- build mode and compiler/runtime version,
- binary size,
- internal stage timings (parse / CSS / cascade / layout / emit),
- peak RSS if you can measure it,
- anything the subset forced you to simplify.
