# Widgets v1 — catálogo de controles

## Alcance

Los widgets se declaran en el HTML del subset y se **compilan en build time**; el
runtime no parsea HTML/CSS. Un widget es un nodo con rol semántico, máquina de
estados y comportamiento de teclado; **no es un objeto de la ABI**. El host sigue
viendo `target_id`, eventos y comandos.

`button` e `img` ya están en el subset v1. `input`, `select`, `progress` y
`dialog` son **extensiones del subset v2** (minor bump + `ui-tree/3`). No hay
formularios web: sin `form`, `action`, validación ni `submit`.

## Modelo común

### Estados

| Estado | Se expresa con | Regla |
|---|---|---|
| Normal | — | Base. |
| Hover | `:hover` | Solo puntero; no mueve el foco. |
| Press | `:active` | Entre down y up sobre el mismo nodo. |
| Focus | `:focus` | Anillo visible con teclado; en text input, caret. |
| Disabled | `disabled` + `:disabled` | No recibe foco, eventos ni activación; UIA lo marca no disponible. |
| Selected | `data-selected` / `aria-selected` | Estado de selección, no estilo; lo gobierna el host. |
| Checked | `checked` / `aria-checked` | Alternancia binaria (checkbox, radio, toggle). |

Estados específicos (`readonly`, `error`, `loading`, `indeterminado`) se suman a
esta base y se documentan en la fila del widget.

### Foco y teclado

- Un solo dueño de foco por vista; el foco es **por nodo**, no por vista.
- `Tab`/`Shift+Tab` en orden de documento. Los composites (list/grid, tabs,
  menu, radio group) usan **roving tabindex**: un solo hijo en el tab order y
  flechas dentro.
- Defaults: `Enter` = acción primaria; `Space` = toggle/selección; `Esc` =
  cerrar/cancelar; flechas = navegar; `Home`/`End` = extremos; `PageUp/Down` =
  página. Key repeat solo en flechas/Page/Home/End.
- Cerrar una capa (dialog, menu, select) **restaura el foco** al disparador.
- El engine no define atajos globales; los pone el host.

### Eventos y comandos (propuesta)

- Eventos semánticos: `input` (cambio de valor), `change` (commit), `select`,
  `open`/`close`, `scroll`. Todos con `target_id`.
- Comandos: `set_value(id, utf8)`, `set_checked(id, bool)`,
  `set_selected(id, bool)`, `focus(id)`, `scroll_to(id, y)`.
- El `wn_event` actual no lleva valor. Preferir funciones
  `wn_app_get_value(id)` / `wn_app_set_value(id, …)` antes que agrandar la
  struct (rompe ABI por tamaño). Decisión en ABI v3.

### Accesibilidad

Cada widget declara **rol + nombre + estado + valor**. Nombre accesible:
`aria-label`, `title` o contenido. ARIA mínimo (`role`, `aria-label`,
`aria-pressed`, `aria-checked`, `aria-selected`, `aria-valuenow`, `aria-modal`),
no ARIA completo. El puente UIA traduce rol → ControlType (`docs/platform-v1.md`). Un
control custom no es accesible gratis: el rol es un contrato a implementar.

## Catálogo

| Widget | Contrato | Estados | Teclado | Rol | Nativo/custom | Prio |
|---|---|---|---|---|---|---|
| **button** | `<button>` (v1) | normal, hover, press, focus, disabled; `aria-pressed` si toggle | `Enter`/`Space` activan; `Tab` entra/sale | `button` (UIA Button) | Custom; ya dibujado | **P0** |
| **text input** | `<input type="text">` (v2) | normal, focus, disabled, readonly, error | Editor completo (ver *Text input*): flechas, `Home`/`End`, `Shift`+selección, `Ctrl+A/C/X/V/Z/Y`, `Enter`, `Esc` | `textbox` (UIA Edit) | Custom + servicios del SO | **P0** |
| **checkbox / radio** | `<input type="checkbox\|radio">` (v2) | normal, hover, press, focus, disabled, checked | `Space` alterna; radio: flechas mueven y seleccionan; `Tab` por grupo | `checkbox` / `radio` + `radiogroup` | Custom | P1 |
| **select** | `<select>` (v2) | cerrado, abierto, hover, focus, disabled | `Space`/`Alt+↓` abre; flechas; `Enter`; `Esc`; type-ahead | `combobox` + `listbox`/`option` | Custom; popup propio | P1 |
| **scrollbar** | `overflow: auto\|scroll` (v2) | normal, hover, press (thumb/track), focus, auto-oculta | Wheel; `Page`/`Home`/`End`/flechas con foco en el contenedor; drag | `scrollbar` o valor del contenedor | Custom | **P0** |
| **list / grid** | `<ul>/<li>`, `role=list/grid` (v2) | normal, hover, selected, focus (roving), disabled item | Flechas, `Home`/`End`, `Page`, `Enter`, `Space`, `Ctrl+A`, `Shift`+flechas | `list`/`listbox`/`grid` + `option`/`gridcell` | Custom + virtualización | **P0** |
| **image** | `<img>` (v1; decode v2) | loading, loaded, error; hover/selected vía padre | `Enter`/`Space` solo si es accionable | `img` (`alt`); decorativa = presentación | Custom (WIC + cache) | **P0** |
| **tabs** | `role=tablist` (v2) | normal, hover, selected, focus, disabled | Flechas mueven foco; `Enter`/`Space` activan; `Home`/`End` | `tablist`/`tab`/`tabpanel` | Custom | P2 |
| **menu** | `role=menu` (v2) | cerrado, abierto, hover, focus, disabled, checked | `Alt`/`F10` abre; flechas; `Enter`; `Esc`; letras | `menu`/`menuitem` | Custom (popup) | P2 |
| **tooltip** | `title` o `data-tooltip` (v2) | oculto, visible (delay), descartado | `Esc` cierra; aparece en hover y foco | `tooltip` | Custom | P2 |
| **dialog** | `<dialog>` o `role=dialog` (v2) | cerrado, abierto, modal, busy | `Esc` cierra; `Tab` atrapado; foco inicial y restaurado | `dialog` + `aria-modal` | Custom (in-engine) | P1 |
| **progress** | `<progress>` (v2) | indeterminado, determinado, completo, error | No focusable | `progressbar` | Custom | P1 |

### Notas de decisión

- **button**: nada de control nativo; el click es down+up sobre el mismo nodo
  (coincide con la semántica actual de la ABI).
- **select**: popup propio sobre la capa de menu; el `select` nativo se rechaza
  por styling, z-order y modo embedded.
- **scrollbar**: no es un nodo que el diseñador coloque; es una affordance del
  contenedor con scroll, dibujada por el engine. Sin propiedades CSS de
  scrollbar en v2: tema fijo tipo sistema, tokens después.
- **list/grid**: el modelo de selección (simple/múltiple) lo define el host; el
  engine solo trackea hover/foco/selección por item. Virtualización exige ids
  estables.
- **dialog**: modal dentro del árbol, no una ventana del SO. Los diálogos de
  archivo son del host (`platform-v1`).
- **tabs**: pestañas dentro de la app, **no** pestañas de navegador (el no-goal
  del product spec se refiere al navegador).
- **image**: decode WIC → RGBA8, cache por ruta, placeholder en error; sin red.

## Text input: nativo vs custom

| Criterio | `EDIT` nativo | Editor custom |
|---|---|---|
| IME | Gratis (TSF/IMM) | A integrar en fase 4 |
| Selección/undo | Gratis y probado | Caret, rangos y undo propios |
| Clipboard | Gratis | Vía `wn_host` (simple) |
| Styling | Limitado (fuente/color/borde) | Total y coherente |
| Accesibilidad | UIA Edit gratis | TextPattern propio |
| Embedded/offscreen | Difícil (HWND hijo, z-order) | Funciona en cualquier surface |
| Clip/scroll/DPI | El HWND ignora el scissor | Un solo pipeline |
| Multiplataforma | Una implementación por OS | Una sola |

**Recomendación:** editor **custom** para el MVP, con alcance cerrado:
single-line, sin bidi, sin rich text, sin multilínea, sin spellcheck. Es la única
opción que conserva un renderer, el modo embedded y el styling. El engine
mantiene buffer de edición y caret; el host recibe `input` y puede corregir con
`set_value` (el valor de verdad es del host; el engine no valida). El coste real
es IME + UIA, que se pagan en fase 4 con TSF y TextPattern. Si el target exige
CJK antes, se permite un `EDIT` nativo temporal como overlay tras un flag,
documentado como deuda: no cambia la arquitectura ni la ABI.

MVP obligatorio: caret con blink y posicionamiento por click, selección por
drag y doble click, `Shift`+flechas/`Home`/`End`, `Ctrl+A/C/X/V`, undo/redo
(≥100 pasos), `Enter` commit, `Esc` cancelar/limpiar, max length y placeholder.
Fuera: multilínea, rich text, bidi, IME, autocompletado, password y drag de
texto.

## P0 mínimo para la biblioteca

| Widget | Uso | Criterio de aceptación |
|---|---|---|
| text input | búsqueda/filtro | Filtra < 50 ms con 1000 items; `Esc` limpia; `Ctrl+A/C/V` |
| button | Abrir, Favorito, Ajustes | `Enter`/`Space`; disabled visible; toggle con `aria-pressed` |
| scrollbar | grid de posters | Wheel + teclado + drag; sin saltos ni drifting |
| list/grid | catálogo | Hover, selección, flechas, `Enter`; virtualizado |
| image | posters | WIC + cache; `object-fit: cover`; placeholder en error |

`image` es P0 en la práctica (sin posters no hay biblioteca) y ya está en
`ui-tree-v2`. **No** hacen falta para A1: checkbox/radio (Favorito es un botón
toggle), select (orden con botones), dialog (detalle como panel), progress (solo
A2), tabs, menu y tooltip.

Aceptación del set P0: flujo completo solo con teclado; 1000 items sin
degradación; 100/125/150/200 % DPI con foco nítido; UIA expone rol, nombre y
valor; páginas sin widgets no regresan.

## Fuera de alcance

- **iframes, JavaScript, formularios web** (validación, `submit`, autofill) y
  controles de navegador (address bar, devtools, PDF, media).
- `contenteditable`, rich text, multilínea, date/time picker, file input,
  color picker, canvas, tablas con sorting y drag-reorder.
- Un control nativo por widget; web components/shadow DOM; ARIA completo.

## Impacto en subset/ABI

- **subset v2**: tags `input` (`text|checkbox|radio`), `select`, `progress`,
  `dialog`; atributos `type`, `value`, `checked`, `placeholder`, `readonly`,
  `maxlength`, `disabled` y ARIA mínimo.
- **`ui-tree/3`**: `role`, `value`, `checked`, `selected`, `placeholder`,
  `disabled` por nodo.
- **ABI v3**: eventos de valor/selección/scroll y comandos nuevos; añadir
  funciones es compatible, agrandar `wn_event` no.
- `wn_renderer` no cambia: todo se dibuja con quads y texto existentes.
