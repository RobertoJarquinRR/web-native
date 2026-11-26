# ui-tree/3 — input de texto

`/3` es aditivo sobre `/2`: una página con `<input type="text">` (y `--pilot`)
emite nodos `kind:"input"` con su spec. No hay piscina de glifos ni atlas en el
documento: el runtime shapea y mide el texto como cualquier otro.

## Nodo

| Campo | Regla |
|---|---|
| `kind` | `"input"`; hoja, sin hijos. |
| `value` | Valor inicial, ASCII imprimible 32..126, recortado a `max_length`. |
| `placeholder` | Texto de ayuda si `value` está vacío; ASCII imprimible. |
| `max_length` | 1..255; `0`/ausente = sin límite, tope duro 255. |
| `password` | Reservado; siempre `false`. |

`placeholder` y `value` se sanean en el frontend; la edición en runtime acepta
UTF-8 (ver abajo), pero lo que viaja en el documento es ASCII.

## Runtime

- Shaping y raster con el mismo backend que los textos; el ancho del caret y la
  selección salen de los avances shapeados.
- Cursor y ancla por **codepoint**, no por byte.
- Una línea: `Enter` queda para el host, no hay wrapping ni multilínea.
- Drag-select, `Shift+flechas/Home/End`, `Ctrl+A/C/X/V` y caret con blink.
- `WN_EVENT_INPUT` se emite en cada cambio del usuario; `set_value` no lo emite.

## Selección de schema

- `/1` página base, `/2` grid/scroll/imágenes, `/3` añade input.
- Solo se emite `/3` si la página tiene un `<input type="text">` y se compiló
  con `--pilot`.

## Limitaciones

- Sin IME, composición, bidi, CJK ni emoji en el teclado; pegar UTF-8 sí entra.
- Sin multilínea, password real, undo/redo ni doble clic para seleccionar
  palabra.
- Un valor por nodo; `max_length` se cuenta en codepoints y el buffer duro son
  255 bytes.
