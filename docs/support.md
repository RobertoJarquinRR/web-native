# Soporte actual

Este documento responde a una pregunta concreta: **qué se puede usar hoy y qué
no**. El contrato cerrado es `docs/subset-v1.md`; las extensiones del piloto son
opt-in con `--pilot`. El detalle de implementación está en
`src/runtime/README.md` y `src/frontend/README.md`.

Leyenda:

- **Sí**: implementado y verificado.
- **Parcial**: el frontend lo parsea/emite, pero el runtime no lo aplica del
  todo o solo cubre un caso.
- **Pilot**: extensión opt-in (`--pilot`), fuera del subset v1.
- **No**: fuera del alcance actual.

> **Columna R1:** el pipeline estático R1 está archivado
> (`archive/r1-static/`). Se conserva la columna porque describe el render
> horneado histórico; hoy el único runtime soportado es R2.

## Contratos

| Contrato                | Versión                    | Estado                   |
| ----------------------- | -------------------------- | ------------------------ |
| Display list R1         | `webnative.display-list/1` | archivado, ya no se emite |
| Documento `.wnui`       | blob v2                    | ✅                       |
| Grid, scroll e imágenes | campos v2 del blob         | ✅ pilot                 |
| Text input              | sección `inputs` del blob  | ✅ pilot                 |
| ABI C                   | `WN_ABI_VERSION 6`         | ✅ standalone + embedded |

## HTML

| Elemento / atributo                                          | Frontend | R1      | R2  | Notas               |
| ------------------------------------------------------------ | -------- | ------- | --- | ------------------- |
| `html`, `head`, `body`, `div`, `span`, `p`, `h1`–`h6`        | Sí       | Sí      | Sí  | base                |
| `button`                                                     | Sí       | Sí      | Sí  | estado por `data-*` |
| `img` local                                                  | Sí       | Parcial | Sí  | WIC; sin red        |
| `ul`, `ol`, `li`                                             | Sí       | Sí      | Sí  | listas estáticas    |
| `link` stylesheet, `style`                                   | Sí       | Sí      | Sí  | CSS local           |
| `input type="text"`                                          | Pilot v3 | warning | Sí  | UTF-8 en edición    |
| `script`, `iframe`                                           | No       | No      | No  | no-goal             |
| `form`, `select`, `table`, `canvas`, `video`, `audio`, `svg` | No       | No      | No  | fuera del subset    |
| `template`, `slot`, `object`, `embed`                        | No       | No      | No  | fuera del subset    |

Atributos: `id`, `class`, `style`, `data-hover`, `data-press`,
`data-selected`, `data-action`, `src`, `href`, `title`, `disabled`; en input:
`type`, `placeholder`, `value`, `maxlength`.

## CSS: layout

| Propiedad                                            | Frontend | R1      | R2      | Notas                                                      |
| ---------------------------------------------------- | -------- | ------- | ------- | ---------------------------------------------------------- |
| `display: block`                                     | Sí       | Sí      | Sí      | base                                                       |
| `display: flex`                                      | Sí       | Sí      | Sí      | row/column, gap, wrap                                      |
| `display: grid`                                      | Pilot v2 | warning | Sí      | `repeat(auto-fill, px)`                                    |
| `flex-direction`                                     | Sí       | Sí      | Sí      | row/column                                                 |
| `justify-content`                                    | Sí       | Sí      | Sí      | start/center/end/space-between                             |
| `align-items`                                        | Sí       | Sí      | Sí      | stretch/start/center/end                                   |
| `flex-wrap`                                          | Sí       | Sí      | Sí      | nowrap/wrap                                                |
| `gap`, `row-gap`, `column-gap`                       | Sí       | Sí      | Sí      | px                                                         |
| `width`, `height`                                    | Sí       | Sí      | Sí      | px/%                                                       |
| `min-width`, `max-width`, `min-height`, `max-height` | Pilot v3 | Parcial | Parcial | el frontend emite; runtime pendiente de validar            |
| `padding`, `margin`                                  | Sí       | Sí      | Sí      | px; `margin: auto` en block/flex                           |
| `box-sizing`                                         | Sí       | Sí      | Sí      | content-box por defecto (CSS); border-box si se declara |
| `box-sizing: content-box`                            | Sí       | Sí      | Sí      | default de CSS; el motor suma padding/borde             |
| `overflow: hidden`                                   | Sí       | Sí      | Sí      | clip rectangular                                           |
| `overflow: auto/scroll`                              | Pilot v2 | warning | Sí      | scroll vertical                                            |
| `position`, `float`, `z-index`                       | No       | No      | No      | fuera del subset                                           |
| `transform`, `transition`, `animation`               | No       | No      | No      | fuera del subset                                           |

## CSS: tipografía y color

| Propiedad                                 | Frontend | R1      | R2      | Notas                                                                                         |
| ----------------------------------------- | -------- | ------- | ------- | --------------------------------------------------------------------------------------------- |
| `font-family`                             | Sí       | Sí      | Sí      | fuentes del sistema                                                                           |
| `font-size`                               | Sí       | Sí      | Sí      | px                                                                                            |
| `font-weight`                             | Sí       | Sí      | Sí      | ≥600 usa la cara bold real del sistema (Segoe UI/Arial bold); si falta, regular + diagnóstico |
| `line-height`                             | Sí       | No      | Sí      | px o número; afecta altura y baseline (half-leading)                                          |
| `text-align`                              | Sí       | No      | Sí      | `left`/`center`/`right` por línea; el texto envuelve                                          |
| `color`, `background`, `background-color` | Sí       | Sí      | Sí      | `#rgb`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()`                                             |
| `opacity`                                 | Pilot v3 | Parcial | Parcial | el frontend emite; el core aún no lo compone                                                  |
| `font-face`, web fonts                    | No       | No      | No      | no-goal                                                                                       |
| gradientes                                | No       | No      | No      | fuera del subset                                                                              |

## CSS: bordes, radius y sombras

| Propiedad                                                | Frontend | R1      | R2      | Notas                                                                 |
| -------------------------------------------------------- | -------- | ------- | ------- | --------------------------------------------------------------------- |
| `border`, `border-width`, `border-style`, `border-color` | Pilot v3 | warning | Sí (v1) | uniforme; per-side v1 no                                              |
| `border-radius`                                          | Pilot v3 | warning | Sí (v1) | uniforme; per-corner v1 no                                            |
| `box-shadow`                                             | Pilot v3 | warning | Sí (v1) | primera sombra no-inset; blur aproximado                              |
| `em`, `rem`, `vh`, `vw`                                  | Pilot v3 | warning | Parcial | el frontend resuelve px en build; el runtime no re-resuelve al resize |
| `@media`, `@font-face`                                   | No       | No      | No      | fuera del subset                                                      |

## Runtime y ABI

| Capacidad                                    | R2  | Notas                                                    |
| -------------------------------------------- | --- | -------------------------------------------------------- |
| Layout block/flex/grid                       | ✅  | layout en C; `margin: auto` en block y flex              |
| Scroll vertical + clip                       | ✅  | scrollbar track+thumb (sin drag); thumb sigue el wheel   |
| Imágenes WIC + cache                         | ✅  | LRU con presupuesto (`WN_IMAGE_CACHE_MB`, 256 MB)        |
| Tamaño intrínseco tras `set_image`            | ✅  | `wn_node_set_image_size` re-layout y `object-fit` (A9)   |
| Hover / press / selected                     | ✅  | bitset por nodo; varias selecciones, hover/press exclusivos |
| Lecturas y navegación                        | ✅  | `get_text`, `get_rect`, `scroll_into_view`, foco/blur     |
| Resize                                       | ✅  | re-layout                                                |
| Wheel                                        | ✅  | `WN_EVENT_WHEEL`; `delta_x` horizontal llega, el core no lo usa |
| Botones y doble clic                         | ✅  | `button`/`click_count`; derecho/central reportan sin pulsar |
| UI dinámica v3                               | ✅  | visibilidad, `set_image`, clone/append/remove            |
| Input v4                                     | ✅  | UTF-8, caret, selección, drag, blink; texto shapeado en runtime |
| Bordes / radius / sombras v1                 | ✅  | radio/borde uniforme; primera sombra; sin per-side/inset |
| Virtualización / culling                     | ❌  | pendiente                                                |
| Text shaping runtime                         | ✅  | DirectWrite; medición, shaping y atlas dinámico          |
| Documento `.wnui` (blob)                     | ✅  | mmap + memoria, refcount; builtin embebido en la DLL     |
| Wrapping / line breaking                     | ✅  | greedy por palabras; `line-height` y `text-align` por línea |
| Ellipsis / truncado por ancho                | ❌  | pendiente (el host compacta)                             |
| Timers / reloj público                       | ✅  | `wn_app_set_tick_ms` + `wn_view_tick`; blink interno aparte |
| Bidi, CJK, emoji, IME                        | ❌  | pendiente                                                |
| Accesibilidad / UIA                          | ❌  | pendiente                                                |
| DPI / multi-monitor                          | ❌  | Windows básico                                           |
| Multiventana, diálogos, tray, notificaciones | ❌  | pendiente                                                |
| Red / HTTP                                   | ❌  | no-goal                                                  |
| Parser HTML/CSS runtime                      | ❌  | no-goal                                                  |
| JS / iframes                                 | ❌  | no-goal                                                  |

## Paridad con el navegador

Medición de referencia (Chromium 141 headless vs D3D11, DPR 1, offscreen):

| Fixture | SSIM | Diff | Región peor |
|---|---:|---:|---|
| `flex-page` 480×320 | 1.000000 | **0.000 %** | — |
| `contract` 400×300 | 1.000000 | **0.000 %** | — |
| `media-library` 1280×800 | 0.957851 | 2.139 % | grid 2.262 % (texto e imágenes); sidebar/detalle ≥ 0.990 |

Los dos primeros son gateables y coinciden al píxel. En el tercero lo que queda
es antialiasing de DirectWrite vs Skia y remuestreo de pósteres: la caja, la
rejilla y las columnas ya coinciden.

**Caveat:** el runner (`run_visual.ps1`) está archivado, así que estos números
son la última medición documentada, no un gate que corra en CI. Para volver a
medir hay que restaurar el runner en Python (sin `.ps1`). Detalle y entorno en
[`benchmarks/visual-parity/BASELINE.md`](../benchmarks/visual-parity/BASELINE.md).

## Frontends

| Frontend   | Rol                                 | Alcance                                          |
| ---------- | ----------------------------------- | ------------------------------------------------ |
| **Rust**   | canónico / producción               | subset v1 + pilot v2/v3                          |
| **Python** | oráculo v1                          | paridad y goldens; no sigue features nuevas      |
| **C++**    | referencia de paridad **congelada** | fixtures de `benchmarks/frontend-parity/SPEC.md` |
| **C#**     | referencia de paridad **congelada** | fixtures de `benchmarks/frontend-parity/SPEC.md` |

La paridad byte-idéntica se mide contra `benchmarks/frontend-parity/SPEC.md`.
C++/C# quedan **congelados** en ese subset: las features nuevas de Rust no les
obligan a actualizarse. Si el SPEC cambia, se decide explícitamente si se
actualizan o se quedan fuera del subset actual. Snapshot de resultados en
[`benchmarks/frontend-parity/results/README.md`](../benchmarks/frontend-parity/results/README.md).

## Cómo comprobar el soporte

```powershell
# Diagnóstico estricto: falla ante cualquier feature fuera del subset
frontend-rust --html page.html --css style.css --width 1280 --height 800 --emit-blob app.wnui --strict-subset

# Extensiones del piloto
frontend-rust --html page.html --width 1280 --height 800 --emit-blob app.wnui --pilot

# Build + aceptación completa (ABI, CTest, layout, píxeles y estados)
python tools\cli.py test all

# Extensiones del piloto: emite, compila y verifica
python tools\cli.py bench pilot

# Blob: layout y píxeles contra el documento builtin
python tools\cli.py test suite blob

# Vista de debug del blob (texto o --json); no es un contrato
src\runtime\build\release\wnui-dump.exe app.wnui
python tools\cli.py test suite dump
```

`--strict-subset` es literal: `<meta>`, `<title>` y atributos como `lang` no
están en el contrato y fallan, aunque el parser los ignore. Una página estricta
es un fragmento del subset (o compile sin el flag).

Ejecutar un `.exe` o cargar un `.wnui` no necesita Python.

## Lo que no se persigue

webnative **no es un navegador**: no hay motor JS, parser HTML/CSS en runtime,
iframes, red, formularios web completos, devtools ni plataforma web. Cualquier
parser/JS futuro sería un módulo/proyecto separado con una decisión explícita.
