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
| Display list R1         | `webnative.display-list/1` | ✅                       |
| UI tree base            | `webnative.ui-tree/1`      | ✅                       |
| Grid, scroll e imágenes | `webnative.ui-tree/2`      | ✅ pilot                 |
| Text input              | `webnative.ui-tree/3`      | ✅ pilot                 |
| ABI C                   | `WN_ABI_VERSION 4`         | ✅ standalone + embedded |

## HTML

| Elemento / atributo                                          | Frontend | R1      | R2  | Notas               |
| ------------------------------------------------------------ | -------- | ------- | --- | ------------------- |
| `html`, `head`, `body`, `div`, `span`, `p`, `h1`–`h6`        | Sí       | Sí      | Sí  | base                |
| `button`                                                     | Sí       | Sí      | Sí  | estado por `data-*` |
| `img` local                                                  | Sí       | Parcial | Sí  | WIC; sin red        |
| `ul`, `ol`, `li`                                             | Sí       | Sí      | Sí  | listas estáticas    |
| `link` stylesheet, `style`                                   | Sí       | Sí      | Sí  | CSS local           |
| `input type="text"`                                          | Pilot v3 | warning | Sí  | ASCII 32..126       |
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
| `box-sizing`                                         | fijo     | fijo    | fijo    | **siempre border-box**; el motor no implementa content-box |
| `box-sizing: content-box`                            | No       | No      | No      | el default de CSS; el subset lo rechaza                    |
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
| `text-align`                              | Sí       | No      | Sí      | `left`/`center`/`right`; una línea (R2 no envuelve texto)                                     |
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
| Imágenes WIC + cache                         | ✅  | sin eviction LRU                                         |
| Hover / press / selected                     | ✅  | estados por nodo                                         |
| Resize                                       | ✅  | re-layout                                                |
| Wheel                                        | ✅  | `WN_EVENT_WHEEL`                                         |
| UI dinámica v3                               | ✅  | visibilidad, `set_image`, clone/append/remove            |
| Input v4                                     | ✅  | ASCII, caret, selección, clipboard Win32                 |
| Bordes / radius / sombras v1                 | ✅  | radio/borde uniforme; primera sombra; sin per-side/inset |
| Virtualización / culling                     | ❌  | pendiente                                                |
| Text shaping runtime                         | ✅  | DirectWrite; medición, shaping y atlas dinámico          |
| Documento `.wnui` (blob)                     | ✅  | mmap solo lectura; `wn_app_load_document` vía ABI        |
| Wrapping / line breaking                     | ❌  | una línea por nodo; pendiente                            |
| Bidi, CJK, emoji, IME                        | ❌  | pendiente                                                |
| Accesibilidad / UIA                          | ❌  | pendiente                                                |
| DPI / multi-monitor                          | ❌  | Windows básico                                           |
| Multiventana, diálogos, tray, notificaciones | ❌  | pendiente                                                |
| Red / HTTP                                   | ❌  | no-goal                                                  |
| Parser HTML/CSS runtime                      | ❌  | no-goal                                                  |
| JS / iframes                                 | ❌  | no-goal                                                  |

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
frontend-rust --html page.html --width 1280 --height 800 --out ir.json --strict-subset

# Extensiones del piloto
frontend-rust --html page.html --width 1280 --height 800 --emit-ui ui.json --pilot

# Build + aceptación completa (ABI, CTest, layout, píxeles y estados)
python tools\cli.py test all

# Extensiones del piloto: emite, compila y verifica
python tools\cli.py bench pilot

# Blob: layout y píxeles contra el documento builtin
python tools\cli.py test suite blob
```

Ejecutar un `.exe` o cargar un `.wnui` no necesita Python.

## Lo que no se persigue

webnative **no es un navegador**: no hay motor JS, parser HTML/CSS en runtime,
iframes, red, formularios web completos, devtools ni plataforma web. Cualquier
parser/JS futuro sería un módulo/proyecto separado con una decisión explícita.
