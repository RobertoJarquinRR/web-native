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
| Documento `.wnui`       | blob v6                    | ✅                       |
| Grid, scroll e imágenes | campos v2 del blob         | ✅ pilot                 |
| Text input              | sección `inputs` del blob  | ✅ pilot                 |
| IME (IMM32, una línea)  | eventos v7 + `wn_ime_ops`  | Parcial: composición/commit/cancel con backend falso; smoke manual Win32 pendiente |
| ABI C                   | `WN_ABI_VERSION 7`         | ✅ standalone + embedded |
| Estados CSS             | sección `states` del blob  | ✅ hover/press/focus/disabled (color; `disabled` en HTML siembra el bit) |

## HTML

| Elemento / atributo                                          | Frontend | R1      | R2  | Notas               |
| ------------------------------------------------------------ | -------- | ------- | --- | ------------------- |
| `html`, `head`, `body`, `div`, `span`, `p`, `h1`–`h6`        | Sí       | Sí      | Sí  | base                |
| `button`                                                     | Sí       | Sí      | Sí  | estado por `data-*` |
| `img` local                                                  | Sí       | Parcial | Sí  | WIC; sin red        |
| `ul`, `ol`, `li`                                             | Sí       | Sí      | Sí  | listas estáticas    |
| `link` stylesheet, `style`                                   | Sí       | Sí      | Sí  | CSS local           |
| `input type="text"`                                          | Pilot v3 | warning | Sí  | UTF-8 en edición; IME en runtime (no cambia el blob) |
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
| `position: static/relative/absolute` + `top/right/bottom/left` | Sí (v4)  | No | Sí | nodos relativos/absolutos, containing block, px/%/auto; ver `wnui-format.md` |
| `z-index: auto | <integer>` (v5)                     | Sí       | No      | Sí      | contextos de apilado; solo posicionados (static se ignora); ver `wnui-format.md` |
| `position: fixed/sticky`, `inset`                    | No       | No      | No      | fuera de esta lane; se diagnostican                          |
| `:nth-child` / `:nth-of-type` (An+B)                 | Sí       | No      | n/a     | match estático en el frontend; `odd`/`even`; se diagnostican `of` y `nth-last-*` |
| `float`                                              | No       | No      | No      | fuera del subset                                             |
| `transform`, `transition`, `animation`               | No       | No      | No      | fuera del subset                                           |

## CSS: tipografía y color

| Propiedad                                 | Frontend | R1      | R2      | Notas                                                                                         |
| ----------------------------------------- | -------- | ------- | ------- | --------------------------------------------------------------------------------------------- |
| `font-family`                             | Sí       | Sí      | Sí      | fuentes del sistema                                                                           |
| `font-size`                               | Sí       | Sí      | Sí      | px                                                                                            |
| `font-weight`                             | Sí       | Sí      | Sí      | `normal`=400, `bold`=700 y enteros 100–900; DirectWrite mapea a `DWRITE_FONT_WEIGHT` y usa la cara instalada más cercana; un valor intermedio toma el canónico más cercano (`450`→400, `550`→500), no la preferencia 400–500 de CSS Fonts 4; no se fuerza síntesis (DirectWrite puede simular bold si falta el peso). En una regla de estado (v6) el peso se guarda en el `u16 reserved` y afecta avance/métricas/atlas |
| `line-height`                             | Sí       | No      | Sí      | px o número; afecta altura y baseline (half-leading)                                          |
| `text-align`                              | Sí       | No      | Sí      | `left`/`center`/`right` por línea; el texto envuelve                                          |
| `color`, `background`, `background-color` | Sí       | Sí      | Sí      | `#rgb`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()`                                             |
| `opacity`                                 | Sí       | Parcial | Sí      | todo nodo < 1 es grupo offscreen (también hojas); compone una vez; 0..1 |
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
| Layout block/flex/grid                       | ✅  | layout en C; `margin: auto` en block y flex; relative/absolute v4 |
| Position relative/absolute                   | ✅  | fuera de flujo, containing block, stretch, scroll/clip, hit-test |
| Z-index / stacking contexts (v5)             | ✅  | orden CSS simplificado (negativos, 0/auto, positivos), anidado y hit-test |
| Scroll vertical + clip                       | ✅  | scrollbar track+thumb (sin drag); thumb sigue el wheel   |
| Imágenes WIC + cache                         | ✅  | LRU con presupuesto (`WN_IMAGE_CACHE_MB`, 256 MB)        |
| Tamaño intrínseco tras `set_image`            | ✅  | `wn_node_set_image_size` re-layout y `object-fit` (A9)   |
| Hover / press / selected / focus / disabled  | ✅  | bitset por nodo (v7); selección/foco múltiples, hover/press exclusivos; disabled inerte. Aditivo v7: `FOCUS_VISIBLE` (0x20, host) y `FOCUS_WITHIN` (0x40, derivado y sólo lectura; `set_state` lo rechaza) |
| Lecturas y navegación                        | ✅  | `get_text`, `get_rect`, `scroll_into_view`, foco/blur     |
| Resize                                       | ✅  | re-layout                                                |
| Wheel                                        | ✅  | `WN_EVENT_WHEEL`; `delta_x` horizontal llega, el core no lo usa |
| Botones y doble clic                         | ✅  | `button`/`click_count`; derecho/central reportan sin pulsar |
| UI dinámica v3                               | ✅  | visibilidad, `set_image`, clone/append/remove            |
| Input v4                                     | ✅  | UTF-8, caret, selección, drag, blink; texto shapeado en runtime |
| IME IMM32 (v7)                               | Parcial | composición en el caret con subrayado, commit una vez, cancel/Escape notifica a IMM32; `wn_ime_ops` + eventos `WN_EVENT_IME_*`; gate automático con backend falso, smoke manual Win32 pendiente |
| IME TSF / segmentos / multilínea             | ❌  | pendiente; misma vtable                         |
| Bordes / radius / sombras v1                 | ✅  | radio/borde uniforme; primera sombra; sin per-side/inset |
| Virtualización / culling                     | ❌  | pendiente                                                |
| Text shaping runtime                         | ✅  | DirectWrite; medición, shaping y atlas dinámico          |
| Documento `.wnui` (blob)                     | ✅  | mmap + memoria, refcount; builtin embebido en la DLL     |
| Wrapping / line breaking                     | ✅  | greedy por palabras; `line-height` y `text-align` por línea |
| Ellipsis / truncado por ancho                | Parcial | `text-overflow: ellipsis` en caja de una línea con `overflow: hidden` (sección aditiva `text_extras` del blob v6); multilínea pospuesta; sin `white-space: nowrap` (usa token sin espacios o ancho declarado) |
| Timers / reloj público                       | ✅  | `wn_app_set_tick_ms` + `wn_view_tick`; blink interno aparte |
| Bidi, CJK, emoji                             | ❌  | pendiente (el IME compone texto CJK, pero el motor no implementa bidi/emoji) |
| Accesibilidad / UIA                          | ❌  | pendiente                                                |
| DPI / multi-monitor                          | ❌  | Windows básico                                           |
| Multiventana, diálogos, tray, notificaciones | ❌  | pendiente                                                |
| Red / HTTP                                   | ❌  | no-goal                                                  |
| Parser HTML/CSS runtime                      | ❌  | no-goal                                                  |
| JS / iframes                                 | ❌  | no-goal                                                  |

Notas de IME y CJK:

- El gate automático del IME es el backend falso (`tests/input/ime_test.c` y
  `ime_view_test.c`). La ruta Win32 IMM32 compila y enlaza con `/W4 /WX`, pero
  una composición real con un IME japonés **no** se verificó a mano todavía.
  `runtime_r2.exe` tiene su propio `WndProc` y **no** instala IME: para el smoke
  manual usa `standalone.exe`/`embedded.exe` o el demo `examples/input.c`, que
  pasan por `wn_app` y `host_win32.c`.
- El texto CJK necesita una familia CJK instalada. Con Segoe UI los glifos
  faltantes se pintan como glifo 0 (cajas); el dato UTF-8 y el IME son
  correctos. Hasta que llegue el fallback DirectWrite, declara
  `font-family: "Yu Gothic UI", "Microsoft YaHei", "Malgun Gothic", ...` (el
  fixture `tests/input` ya lo hace para el smoke manual).

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
