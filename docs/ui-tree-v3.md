# UI tree v3 — text input

Schema: `webnative.ui-tree/3`. Es **aditivo sobre `/2`**: hereda grid, scroll,
imágenes, estilos y campos de layout. Solo se emite cuando la página tiene al
menos un nodo `kind:"input"` visible; las páginas `/1` y `/2` conservan sus
bytes y su schema.

El contrato completo y la implementación están en
`src/runtime/README.md` §16 y `docs/abi.md`.

## Nodo

```json
{
  "id": 2,
  "parent": 1,
  "children": [],
  "kind": "input",
  "name": "body.root/div.bar/input#search.search",
  "tag": "input",
  "element_id": "search",
  "classes": ["search"],
  "style": {
    "display": "block",
    "width_px": 320.0,
    "height_px": 34.0,
    "padding": [7.0, 9.0, 7.0, 9.0],
    "font_size": 20.0,
    "color": "#f2f5f8",
    "background": "#1c2430"
  },
  "text": null,
  "input": {
    "value": "",
    "placeholder": "Buscar...",
    "max_length": 12,
    "password": false,
    "baseline": 22.0,
    "line_height": 28.0,
    "glyphs": [
      { "cp": 32, "gid": 3, "advance": 5.484375, "x": 0.0, "y": 22.0,
        "w": 0, "h": 0, "u0": 0.0, "v0": 0.0, "u1": 0.0, "v1": 0.0 }
    ]
  }
}
```

## Campos

| Campo | Regla |
|---|---|
| `kind` | `"input"`; nodo hoja (void, sin hijos). |
| `value` | Valor inicial; solo ASCII imprimible (32..126), recortado a `max_length`. |
| `placeholder` | Texto de ayuda cuando `value` está vacío; ASCII. |
| `max_length` | 1..255; `0`/ausente = sin límite, tope duro 255. |
| `password` | Reservado; siempre `false` en v1. |
| `baseline` | Ascender en px desde el top del content box. |
| `line_height` | Ascender + descender; altura por defecto si el CSS no fija `height`. |
| `glyphs[]` | Piscina ASCII completa y ordenada 32..126 (95 entradas). |

## HTML

```html
<input id="search" type="text" placeholder="Buscar..." value="abc" maxlength="12">
```

- `type` ausente o `text`; otros tipos se emiten como bloque normal con warning.
- `disabled`/`readonly` se aceptan y se ignoran: el widget v1 es editable; el
  host decide si lo enfoca.
- `value`/`placeholder` no ASCII se descartan con warning.
- Sin `width`/`height` CSS: ancho por defecto 200 px (o estimado por
  `maxlength`, 80..480) y alto = padding + línea de la fuente.
- R1 (`--out` sin `--emit-ui`) no edita ni compone texto: emite un warning.

## Piscina ASCII y atlas

- El frontend hornea ASCII imprimible 32..126 por `(familia, tamaño)`.
- Las páginas con input usan un atlas **512×512**; las páginas sin input siguen
  en 256×256 y son byte-idénticas.
- Cada glifo lleva `cp`, `gid`, `advance`, caja (`x/y/w/h`) y UVs.
- `space` tiene `w=h=0` pero `advance > 0`.
- El runtime compone con `pen += advance`; **no hay shaping ni kerning** en
  runtime. Es una decisión de contrato, no un bug.
- El emisor deduplica piscinas idénticas y emite `kUiInputs[]` /
  `kUiInputGlyphs[]`; el core los copia a records compactos por nodo.

## ABI y eventos

- `WN_EVENT_INPUT = 11` cuando el usuario cambia el valor.
- `wn_node_set_value` / `wn_node_get_value` (ABI v4).
- El evento no se emite por mover caret/selección, foco/blur ni `set_value`.
- Ver `docs/abi.md` para la semántica completa.

## Teclado y foco

Click enfoca y coloca el caret; click fuera hace blur. ASCII imprimible inserta;
`Backspace`/`Delete` borran; flechas/Home/End mueven; `Shift+flechas/Home/End`
selecciona; `Ctrl+A/C/X/V` usa el clipboard Win32; `Esc` hace blur; `Enter` y
`Tab` quedan para el host. `maxlength` se aplica a type/paste/set. Caret fijo (sin
blink) en v1.

## Limitaciones

- Solo ASCII 32..126; sin IME, bidi, CJK, emoji ni shaping runtime.
- Sin multilínea, wrapping, password real, undo/redo, drag-select ni
  double-click-select.
- Sin formularios/submit; `Enter` lo decide el host.
- Tope 255 bytes por input; un valor por nodo.
- Atlas de 512×512: ~4–5 tamaños de fuente de input.
- API de valor app-level; el host embedded usa el evento y su propio modelo.
- Sin accesibilidad/UIA todavía.
