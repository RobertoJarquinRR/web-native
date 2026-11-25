# Frontend parity benchmark — specification

> **Estado: congelado (subset v1, 2026-11-24).** Este SPEC documenta el
> contrato de paridad del subset v1 que comparten las **referencias congeladas
> C++ y C#** (`archive/frontends-legacy/`) y el **oráculo Python** (también
> archivado). Rust es el frontend canónico y puede avanzar con features nuevas
> (grid, input, CSS v3); C++/C# no tienen obligación de seguirlas y **no se
> compilan en CI**. Ver [`results/README.md`](results/README.md).

Goal: implement the same HTML+CSS frontend in different languages and prove
that (a) it produces the **same IR**, and (b) measure speed and cost. Hoy el
gate vivo solo ejecuta el frontend Rust; las demás implementaciones quedan como
referencia congelada.

La implementación de referencia histórica es
`archive/frontends-legacy/python/webnative` (paquete v1, test-only).
Las implementaciones congeladas deben coincidir con su salida para los dos
fixtures de abajo, dentro de **0.01 px** de geometría.

## Política de mantenimiento

- **Rust** (`src/frontend`) es el frontend canónico y evoluciona con el
  producto: grid, scroll, imágenes, input y CSS v3.
- **Python** (`archive/frontends-legacy/python`) es el oráculo **test-only** del
  subset v1; se mantiene para goldens y verificación, no para seguir features
  nuevas de Rust.
- **C++** y **C#** (`archive/frontends-legacy/`) son **referencias de paridad
  congeladas** en este SPEC. No se actualizan para seguir a Rust ni se compilan
  en CI; su contrato es producir el mismo IR en los fixtures de este documento.
- Un cambio incompatible del IR o del subset requiere una decisión explícita:
  actualizar los frontends congelados o declararlos fuera del subset actual.
  No se actualizan en silencio.
- Los resultados se versionan en [`results/README.md`](results/README.md) con
  fecha, commit y subset.

## Dos gates, por tipo de contrato

| Gate | Frontends | Contrato | Comando |
|---|---|---|---|
| IR post-layout (congelado) | Rust (vivo); C++/C#/Python congelados | `display_list.json` | `tools/bench.py` |
| Árbol de estilos (vivo) | Rust + oráculo Python | `ui.json` (proyección de estilo) | `frontend-rust` + `emit_ui.py` + `tools/compare_style.py` |

El gate vivo compara **el árbol de estilos pre-layout**, que es lo que consume
el motor. Así `box-sizing`, márgenes o `flex-grow` se comprueban una vez, sin
obligar a reimplementar layout en cada lenguaje. C++/C# quedan congelados en el
IR post-layout y en el modelo border-box, tal como documenta su SPEC.

```powershell
# Rust
frontend-rust --html <html> --css <css> --width W --height H --emit-ui rust.json
# Oráculo Python (test-only)
python src\emitter\python\emit_ui.py --html <html> --css <css> --width W --height H `
  --out py.h --meta py.meta.json --no-text-metrics --dump-style-json py.json
# Comparación
python benchmarks\frontend-parity\tools\compare_style.py --rust rust.json --python py.json
```

Última ejecución: `contract` 4 nodos, `flex-page` 26 nodos, **0 diferencias**.

## Fixtures

| Name | HTML | Viewport | What it exercises |
|---|---|---|---|
| `contract` | `fixtures/contract/input.html` | 400×300 | block flow, padding, margin-top, id/class selectors |
| `flex-page` | `fixtures/flex-page/index.html` | 480×320 | nested flex row/column, gap, space-between, align center, margins, padding |

Both fixtures are **rect-only**: no text nodes. Text is out of scope for this
round (the IR may ignore text; the fixtures contain none).

## CLI contract

Contrato histórico: cada implementación exponía un `run.ps1` en su directorio.
Hoy **ningún frontend vivo usa `.ps1`**: el harness (`tools/bench.py`) lee
`BUILD_INFO.json` e invoca el binario Rust directamente. Las referencias
congeladas no se ejecutan en este gate; sus orquestadores están en
`archive/ps1-legacy/`.

El contrato de argumentos que consume el harness es:

```
<binario> --html <path> --width <W> --height <H> --out <ir.json> --timings <timings.json>
```

Rules:

- Resolve `<link rel="stylesheet" href="...">` relative to the HTML file.
- Write the IR JSON to `--out`.
- Write `--timings` JSON if provided (parse/cascade/layout/emit ms + `ops`).
  `peak_rss_kb` is optional.
- Exit code 0 on success, non-zero on error.
- No browser, WebView, Skia or Electron.

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

It benchmarks the canonical Rust frontend only (`FRONTEND_DIRS = {"rust": ...}`),
verifies parity against the goldens, and writes
`benchmarks/frontend-parity/results/SUMMARY.md` with wall-time medians. C++/C#
y Python quedan fuera de este gate como referencias congeladas.

Report also, in your README:

- build mode and compiler/runtime version,
- binary size,
- internal stage timings (parse / CSS / cascade / layout / emit),
- peak RSS if you can measure it,
- anything the subset forced you to simplify.
