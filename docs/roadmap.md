# Roadmap

## Fase 0 — Fundación (hecha)

- Frontend Rust canónico (HTML/CSS + texto) con paridad byte-idéntica.
- Benchmark Rust + referencias congeladas C++/C#/Python (hoy en
  `archive/frontends-legacy/`).
- R1 estático (ventana/PNG, rects + texto) — **[archivado](archive/r1-static/README.md)**
  tras la consolidación: duplicaba layout y renderer.
- Motor dinámico (layout runtime, eventos, resize, estados) en `src/runtime`.
- Rust `--emit-blob` → `.wnui` (documento único); builtin embebido en la DLL.
  El frontend emite solo `.wnui`; `wnui-dump` es la vista de debug.
- Repo git + docs base.

## Workstreams E1–E8 (estado 2026-11)

Los workstreams son dueños de archivos disjuntos (regla de `architecture.md`).
Un stream no toca los archivos de otro; se integra por contrato.

| ID | Workstream | Alcance | Depende de | Estado |
|---|---|---|---|---|---|
| **E1** | Fidelidad visual/layout | `font-weight`, `line-height`, `text-align`, `margin: auto`, scrollbar y focus ring redondeado **(hecho)**; `opacity` compuesta con capas offscreen **(hecho, blob v3)**; `position: relative/absolute` + insets **(hecho, blob v4)**; `z-index` y stacking contexts **(hecho, blob v5)**; per-side/per-corner, `fixed/sticky`, inset flex pendiente | E6 (schema) | **Hecho (primer corte)** |
| **E2** | Renderer D3D11 | backend nativo hardware-first, WARP fallback, paridad con wgpu, objetivo ≤ 5 MB | E6 (vtable) | **Hecho** (falta retirar el enlace wgpu para el objetivo de tamaño) |
| **E3** | Plataforma y accesibilidad | foco/tab order, DPI per-monitor, IME, clipboard/DnD, UIA, high contrast, reduced motion | E1, E6 | Pendiente (P1 `platform-v1.md`) |
| **E4** | Texto e i18n | shaping runtime **(hecho)**, wrapping/line breaking **(hecho)**, bloque de línea único **(hecho)**; faltan bidi, CJK/emoji y re-raster de atlas | E1, E3 | **En curso**: falta bidi/CJK |
| **E5** | Rendimiento y escala | dirty rects, culling/virtualización, 100/1k/10k nodos, presupuestos en CI | E1, E2 | Pendiente |
| **E6** | Contratos y tooling | subset diagnostics, schemas IR/ui-tree, ABI v7, blob v6, SDK genérica, suites, paridad, métricas | - | **En curso**: falta CI |
| **E7** | Release y packaging | versión, artefactos, licencias, installer, signing, smoke de release | E1–E3, E6 | Pendiente |
| **E8** | Extensiones CSS | transition limitada (P1 post-E1); **position rel/abs (hecho, blob v4)**; **z-index y stacking contexts (hecho, blob v5)**; **estados CSS `:hover/:active/:focus/:disabled` (hecho, blob v6 + ABI v7)**; transform 2D (P2); `fixed`/`sticky` (P2/P3); animation/float futuro/no-go | E1, E6 (schema `/6`), reloj del `wn_host` | **En curso**; ver `e8-css-extensions.md` |

> **Orden de ejecución:** CI y contratos (E6) → ABI v7 (estado/lecturas) →
> documento blob-only y retirada del emisor (C5/C12) → enablers (capabilities,
> textblock, scroll, paintlist) → **opacity (blob v3, capas offscreen)** →
> **position relative/absolute (blob v4)** → **z-index (blob v5)** →
> **estados CSS (blob v6, ABI v7)** → transform
> → E4 input v2
> → E5 escala → E3 plataforma → E7 release. E2 va en paralelo. La
> compatibilidad del blob (C6) se decide al congelar 1.0, no en pre-alfa. E8
> animation/float futuro/no-go.

## Estado (2026-11)

- ✅ Runtime C en módulos (`core/`, `document/`, `text/`), grid/scroll/imágenes.
- ✅ **Texto en runtime**: medición, shaping y atlas dinámico; el documento no
  lleva glifos, runs ni atlas, tampoco para inputs.
- ✅ **Blob v6 `.wnui`**: escritor en Rust (byte-idéntico al emisor), loader C
  con mmap y **loader por memoria**; el builtin de la DLL es un blob embebido
  parseado por el mismo loader. `wn_app_load_document` en la ABI. v3 añadió
  `float opacity` y capas de grupo; v4 añadió `position` + `inset[4]`/`inset_mask`
  (relative/absolute); v5 añadió `int32 z_index` (sentinel `INT32_MIN` = auto)
  al final del registro (252 bytes); v6 añade la sección opcional 5 `states`
  (estilos de color hover/press/focus/disabled por nodo), sin tocar el registro.
- ✅ **Position relative/absolute** (blob v4): relative offsets sin mover
  hermanos, absolute fuera de flujo con containing block posicionado/root,
  stretch left+right/top+bottom, auto width/height, anidado, scroll/clip y
  hit-test. Fixture de píxeles en D3D11 + wgpu.
- ✅ **Z-index / stacking contexts** (blob v5): orden CSS 2.1 simplificado
  (contextos negativos, contenido en flujo, grupos de opacidad y posicionados
  0/auto en el paso 8, y positivos), anidados que no escapan del padre,
  auto/0/negativos, opacidad+z, hit-test por orden de pintado. Las capas z-only
  solo ordenan (sin textura); fixture de píxeles 400x690 en D3D11 + wgpu.
- ✅ **Estados CSS** (blob v6, ABI v7): `:hover`, `:active` (press incluye
  hover), `:focus` y `:disabled` se compilan a cuatro estilos efectivos de
  `background`/`color`/borde; prioridad runtime
  `disabled > press > hover > focus > base`, con `selected` como base y los
  `data-*` como fallback. Bits `WN_STATE_FOCUSED`/`WN_STATE_DISABLED`; los nodos
  deshabilitados son inertes al puntero. Fixture de píxeles 400x450 en D3D11 +
  wgpu y páginas canónica/dynamic/pilot migradas a reglas CSS.
- ✅ **SDK genérica**: `webnative.dll` (181 KiB con wgpu, 160 KiB sin él) +
  `app.wnui`; la galería (`C:\Proyects\gallery`) consume solo `sdk/` y su
  documento.
- ✅ ABI v4: dinámica, input de una línea, stats públicas; v5: identidad de
  nodos y navegación; v6: estado por nodo, getters/scroll/foco, botón y doble
  clic, tick, refcount del documento y tamaño de imagen; **v7**: bits de foco y
  deshabilitado (aditivos).
- ✅ CSS core v1 (52/52), E1 (23/23), dynamic (61/61), input (83 + 118/118),
  text (144), offscreen (100 % exacto), blob; `test all` 10/10 y piloto 48/48.
- ✅ Frontend Rust por etapas (`parse/style/emit/cli`); R1 retirado y su
  benchmark archivado (`archive/benchmarks-r1/`). `.wnui` es la única salida:
  HTML/CSS → blob, incluidos todos los fixtures de test. El emisor Python y su
  árbol JSON intermedio ya no existen.
- ✅ **E6**: CI de dos jobs — D3D11 offline y wgpu con artefacto fijado por
  SHA-256. La paridad visual sigue sin runner.
- ⏭ Siguiente: input v2 (IME, multilínea, undo), imágenes (async, formatos) y
  escala (culling/DPI).

Documentos de esta ola: [`e8-css-extensions.md`](e8-css-extensions.md),
[`visual-parity.md`](visual-parity.md), [`release-checklist.md`](release-checklist.md)
y la auditoría de deudas del SDK en [`sdk-audit.md`](sdk-audit.md).

## Fase 1 — Contratos y runtime platform (hecha)

**Objetivo:** ABI C + core/renderer/host separados + dos modos + métricas +
subset diagnostics.

| Workstream | Alcance | Archivos | Entrega |
|---|---|---|---|
| **W1 runtime-platform** | ABI v0, renderer vtable, host Win32, `wn_view_*`, `wn_app_*`, demos C | `src/runtime/**`, `include/**` (worktree) | 69/69 R2 intacto; demos standalone y embedded; ABI sin wgpu |
| **W2 metrics** | harness de tamaño/startup/RSS/frame/layout; baseline | `benchmarks/engine-metrics/**` | `BASELINE.md` + JSON reproducible |
| **W3 subset diagnostics** | validación de tags/propiedades v1; `--strict-subset` | `src/frontend/**`, fixtures subset | warnings/errores con línea; tests |
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
- ✅ **Texto dinámico**: shaping, rasterizado y medición en runtime.
- ✅ **Input de texto single-line + foco/teclado** (UTF-8 por codepoint; IME
  queda en E3).
- **Texto**: wrapping ✅; faltan ellipsis, bidi y DPI/zoom. (E3/E4)
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
