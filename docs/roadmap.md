# Roadmap

## Fase 0 — Fundación (hecha)

- Frontend Rust canónico (HTML/CSS + texto) con paridad byte-idéntica.
- Benchmark C++/Rust/C#/Python.
- R1 estático (ventana/PNG, rects + texto, sin Python en runtime).
- R2 dinámico (layout runtime, eventos, resize, estados) en `src/runtime/c-r2`.
- Rust `--emit-ui` → `ui.json` → `emit_ui.py --ui-json` → `ui_data.h`.
- Repo git + docs base.

## Estado (2026-11)

- ✅ R1/R2, grid/scroll/imágenes, paridad multi-frontend.
- ✅ ABI v3: UI dinámica (visibilidad, clone/append/remove, set_image).
- ✅ ABI v4: input ASCII de una línea, caret/selección/clipboard, `WN_EVENT_INPUT`.
- ✅ Host C# v3 (65/65): detalle con `set_image`, filtro con `set_visible`.
- ✅ Frontend CSS v3: bordes/radius/sombras/unidades (render en core pendiente).
- ⏳ Siguiente: render CSS v3, virtualización/culling, accesibilidad/IME, DPI,
  cross-platform.

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
subset rechaza features fuera de v1; R1 sin regresión.

## Fase 2 — Bindings y demos de host

- Bindings C# (`LibraryImport`, `UnmanagedCallersOnly`, `PublishAot`).
- Demo standalone en consola.
- Demo embedded en WinForms/WPF (después de estabilizar la ABI).
- **Piloto A: biblioteca de video sin playback** (`docs/pilot-media-library.md`).
- Conformance de la ABI: strings, errores, callbacks, resize, destroy.
- Medir footprint del host C#; decidir si el host de producción va en C/C++/Rust.

## Fase 3 — Madurez de R2

- Invalidación incremental / dirty rects.
- Clipping y scroll.
- Flex fiel: grow/shrink/basis/min-content, margin collapse.
- ✅ **API de UI dinámica: crear/actualizar/eliminar nodos, templates/listas.**
- **Virtualización de listas/grids.** (pendiente)
- **Texto dinámico** (shaping runtime o pre-shape por variantes). (pendiente)
- ✅ **Input de texto single-line + foco/teclado** (ASCII; IME queda en fase 4).
- Texto: wrapping, line breaking, DPI/zoom.
- Animaciones/transiciones (futuro cercano).

## Fase 4 — CSS y OS integration

- Bordes/radios/sombras, unidades relativas, `@media`. El **frontend** ya
  parsea y emite bordes/radius/sombras/unidades; falta el **render en core**.
- IME, clipboard, drag & drop, diálogos, accesibilidad (UIA/ARIA).
- Controles de texto (input/select) si el target lo exige.

## Fase 5 — Cross-platform y backends

- macOS/Linux: winit/SDL + wgpu Metal/Vulkan.
- Backend D2D/D3D11 para Windows footprint mínimo.
- Packaging: instalador, DLL/estático, assets, versionado.

## Fase 6 — Calidad

- Suite de conformidad HTML/CSS del subset.
- Fuzzing de frontend y parser.
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
