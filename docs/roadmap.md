# Roadmap

## Fase 0 — Fundación (hecha)

- Frontend Rust canónico (HTML/CSS + texto) con paridad byte-idéntica.
- Benchmark C++/Rust/C#/Python.
- R1 estático (ventana/PNG, rects + texto) — **[archivado](archive/r1-static/README.md)**
  tras la consolidación: duplicaba layout y renderer.
- Motor dinámico (layout runtime, eventos, resize, estados) en `src/runtime/c-r2`.
- Rust `--emit-ui` → `ui.json` → `emit_ui.py --ui-json` → `ui_data.h`.
- Repo git + docs base.

## Workstreams E1–E8 (estado 2026-11)

Los workstreams son dueños de archivos disjuntos (regla de `architecture.md`).
Un stream no toca los archivos de otro; se integra por contrato.

| ID | Workstream | Alcance | Depende de | Estado |
|---|---|---|---|---|
| **E1** | Fidelidad visual/layout | `font-weight`, `line-height`, `text-align`, `margin: auto`, scrollbar y focus ring redondeado **(hecho)**; `opacity` compuesta, per-side/per-corner, inset, flex grow/shrink/basis pendiente | E6 (schema) | **Hecho (primer corte)** |
| **E2** | Renderer D3D11 | backend nativo hardware-first, WARP fallback, paridad con wgpu, objetivo ≤ 5 MB | E6 (vtable) | **Hecho** (falta retirar el enlace wgpu para el objetivo de tamaño) |
| **E3** | Plataforma y accesibilidad | foco/tab order, DPI per-monitor, IME, clipboard/DnD, UIA, high contrast, reduced motion | E1, E6 | Pendiente (P1 `platform-v1.md`) |
| **E4** | Texto e i18n | shaping runtime, wrapping/line breaking, bidi, CJK/emoji, re-raster de atlas | E1, E3 | Pendiente |
| **E5** | Rendimiento y escala | dirty rects, culling/virtualización, 100/1k/10k nodos, presupuestos en CI | E1, E2 | Pendiente |
| **E6** | Contratos y tooling | subset diagnostics, schemas IR/ui-tree, ABI, paridad CI, métricas, paridad visual | — | **En curso** (esta ola: docs) |
| **E7** | Release y packaging | versión, artefactos, licencias, installer, signing, smoke de release | E1–E3, E6 | Pendiente |
| **E8** | Extensiones CSS | transition limitada (P1 post-E1); transform 2D + position rel/abs + z-index simple (P2); animation/float futuro/no-go | E1, E6 (schema `/4`), reloj del `wn_host` | **Definido**; ver `e8-css-extensions.md` |

> **Orden de ejecución:** E1 (visual) → E8 transition (P1) → E3/E4/E5 según
> target → E7 release. E2 (D3D11) va en paralelo. E8 transform/position/z-index
> después de transition; animation/float futuro/no-go.

## Estado (2026-11)

- ✅ Runtime C, grid/scroll/imágenes, paridad multi-frontend.
- ✅ ABI v3: UI dinámica (visibilidad, clone/append/remove, set_image) — 53/53.
- ✅ ABI v4: input ASCII, caret/selección/clipboard, `WN_EVENT_INPUT` — 111/111.
- ✅ **CSS core v1**: radius/borde uniforme + primera sombra — 41/41.
- ✅ **E1 fidelidad visual/layout** (primer corte): `font-weight` real,
  `text-align`, `line-height`, `margin: auto`, scrollbar y focus ring
  redondeado — 23/23 fidelity, 69/69 R2, 48/48 pilot, 53/53 dynamic, 111/111 input.
- ✅ **E2 renderer D3D11**: hardware Intel por defecto, wgpu como fallback;
  paridad 99.8 % píxeles exactos, hover 0.02 vs 0.73 núcleos (WARP).
- ✅ Frontend CSS v3 (bordes/radius/sombras/unidades) y host C# v4
  (113/113: búsqueda real, detalle, filtro, `set_visible`/`set_image`).
- ✅ **Frontends C++/C# congelados** en `benchmarks/frontend-parity/SPEC.md`;
  Python es oráculo v1; Rust es canónico.
- ✅ **CI de paridad** (`.github/workflows/frontend-parity.yml`) en `main`.
- ✅ Contratos documentados: ABI v4, subset v1, IR, ui-tree, métricas.
- ⏳ **E6 contratos/tooling** en curso (harness de paridad visual).
- ⏭ Siguiente: E8 transition (P1), virtualización/culling (E5),
  accesibilidad/IME/DPI (E3), texto runtime (E4) y release (E7).

Documentos de esta ola: [`e8-css-extensions.md`](e8-css-extensions.md),
[`visual-parity.md`](visual-parity.md), [`release-checklist.md`](release-checklist.md).

## Fase 1 — Contratos y runtime platform (hecha)

**Objetivo:** ABI C + core/renderer/host separados + dos modos + métricas +
subset diagnostics.

| Workstream | Alcance | Archivos | Entrega |
|---|---|---|---|
| **W1 runtime-platform** | ABI v0, renderer vtable, host Win32, `wn_view_*`, `wn_app_*`, demos C | `src/runtime/c-r2/**`, `include/**` (worktree) | 69/69 R2 intacto; demos standalone y embedded; ABI sin wgpu |
| **W2 metrics** | harness de tamaño/startup/RSS/frame/layout; baseline | `benchmarks/engine-metrics/**` | `BASELINE.md` + JSON reproducible |
| **W3 subset diagnostics** | validación de tags/propiedades v1; `--strict-subset` | `src/frontend/rust/**`, fixtures subset | warnings/errores con línea; tests |
| **W4 docs** | spec/ABI/arquitectura/métricas/subset/roadmap | `docs/**` | este conjunto |

**Dependencias:** W4 (contratos) antes de W1. W2 y W3 son independientes.
**Criterios:** R2 69/69; ABI compila y corre en C; métricas registran baseline;
subset rechaza features fuera de v1; R2 sin regresión.

## Fase 2 — Bindings y demos de host

- Bindings C# (`LibraryImport`, `UnmanagedCallersOnly`, `PublishAot`).
- Demo standalone en consola.
- Demo embedded en WinForms/WPF (después de estabilizar la ABI).
- **Piloto A: biblioteca de video sin playback** (`docs/pilot-media-library.md`).
- Conformance de la ABI: strings, errores, callbacks, resize, destroy.
- Medir footprint del host C#; decidir si el host de producción va en C/C++/Rust.

## Fase 3 — Madurez de R2

- Invalidación incremental / dirty rects. (E5)
- Clipping y scroll. (E5)
- Flex fiel: grow/shrink/basis/min-content, margin collapse. (E1)
- ✅ **API de UI dinámica: crear/actualizar/eliminar nodos, templates/listas.**
- **Virtualización de listas/grids.** (E5, pendiente)
- **Texto dinámico** (shaping runtime o pre-shape por variantes). (E4, pendiente)
- ✅ **Input de texto single-line + foco/teclado** (ASCII; IME queda en E3).
- Texto: wrapping, line breaking, DPI/zoom. (E3/E4)
- **Animaciones/transiciones:** solo la propuesta limitada de E8; el resto es
  futuro/no-go. (ver `docs/e8-css-extensions.md`)

## Fase 4 — CSS y OS integration

- Bordes/radios/sombras, unidades relativas, `@media`. El **frontend** ya
  parsea y emite bordes/radius/sombras/unidades; falta el **render en core**
  (per-side/per-corner/inset en E1).
- IME, clipboard, drag & drop, diálogos, accesibilidad (UIA/ARIA). (E3)
- Controles de texto (input/select) si el target lo exige.

## Fase 5 — Cross-platform y backends

- macOS/Linux: winit/SDL + wgpu Metal/Vulkan.
- Backend D2D/D3D11 para Windows footprint mínimo. (E2, en curso)
- Packaging: instalador, DLL/estático, assets, versionado. (E7)

## Fase 6 — Calidad

- Suite de conformidad HTML/CSS del subset.
- Fuzzing de frontend y parser.
- Paridad visual Chromium vs nativo (`docs/visual-parity.md`); CI futura.
- Benchmark de escalado 1k–10k nodos.
- Presupuestos de memoria/perf en CI.
- Documentación de usuario y ejemplos.

## Non-goals permanentes

- Motor JS, parser HTML/CSS en runtime, plataforma web completa, devtools, red.
- Cualquier parser/JS futuro será un plugin/proyecto separado con decisión
  explícita; no entra al core.

## Reglas de paralelización

1. Un workstream = un dueño de archivos; no dos streams tocan el mismo archivo.
2. Refactors del core/runtime en **worktree**; features aisladas en main.
3. Cada stream define criterios de aceptación verificables.
4. Integración: verificar aceptación, merge, commit convencional.
5. Si un stream bloquea a otro, se parte por contrato (header/schema) y se
   integra después.

## Definición de “hecho”

- Código compila sin warnings nuevos.
- Tests/aceptación ejecutados y verdes.
- Métricas dentro del presupuesto o desviación documentada.
- README/docs actualizados.
- Commit convencional (`feat|fix|docs|refactor|test|build`).
