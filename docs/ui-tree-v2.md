# UI tree v2 — grid, scroll e imágenes

> **Documento histórico.** `webnative.ui-tree/2` era el IR JSON intermedio que
> el frontend emitía hacia el emisor. El runtime ya no lo consume: el contrato
> vivo es el blob `.wnui` v2 (`docs/wnui-format.md`). Se conserva para explicar
> el origen de los campos; no describe el flujo actual.

Schema: `webnative.ui-tree/2`. El emisor sigue aceptando `…/1` para páginas
existentes. Este documento es el contrato entre el frontend Rust y el runtime R2
para el piloto de biblioteca.

## Nodos

```json
{
  "id": 0,
  "parent": -1,
  "children": [1, 2],
  "kind": "block | flex | grid | text | image",
  "style": { }
}
```

### Nuevos `kind`

- `grid`: contenedor de layout grid.
- `image`: imagen local.

## Estilos nuevos

| Campo | Valores | Notas |
|---|---|---|
| `display` | `block`, `flex`, `grid` | `grid` es v2 |
| `grid_template_columns` | `{ "mode": "auto_fill", "size_px": 180 }` | solo `auto_fill` en v2 |
| `grid_auto_rows` | `{ "px": 240 }` o `"auto"` | altura de fila; `auto` = max child |
| `gap` / `row_gap` / `column_gap` | px | grid y flex |
| `overflow` | `hidden`, `auto`, `scroll` | scroll solo vertical en v2 |
| `object_fit` | `fill`, `cover`, `contain` | imágenes |

`width`/`height` px/% y padding/margin/colores siguen como v1.

## Nodo imagen

```json
{
  "kind": "image",
  "style": { "width_px": 180, "height_px": 240 },
  "image": {
    "src": "posters/vid-001.png",
    "alt": "Demo",
    "object_fit": "cover"
  }
}
```

- `src` es una ruta local (relativa al HTML o absoluta). Sin red.
- Si falta `width_px`/`height_px`, el runtime usa el tamaño intrínseco (WIC).
- Decodificación: WIC → RGBA8; cache por ruta.

## Grid v2

- `repeat(auto-fill, <size>px)`: columnas = `max(1, floor((content_w + gap) / (size + gap)))`.
- Items en orden row-major.
- Altura de fila: `grid_auto_rows` o el máximo de los hijos.
- Sin `fr`, `minmax`, `auto-fit`, `grid-area`, `span`, ni grid anidado en v2.

## Scroll v2

- Contenedor con `overflow: auto|scroll` y altura fija.
- Estado `scroll_y` por contenedor; se actualiza con `WN_EVENT_WHEEL`
  (`delta_y`), clamp `[0, content_h - viewport_h]`.
- Hit testing suma el offset del contenedor.
- Clip rect = content box del contenedor; el renderer agrupa draws por clip y
  aplica scissor.
- Solo scroll vertical; sin scroll anidado en v2.0.

## ABI v2

Añadir a `wn_event`:

```c
WN_EVENT_WHEEL = 10,
float delta_x, delta_y;
```

`WN_ABI_VERSION` pasa a `2`. El host standalone mapea `WM_MOUSEWHEEL`; el host
embedded reenvía el evento. Sin cambios en el resto de la ABI.

## Compatibilidad

- Páginas sin grid/scroll/imágenes emiten `/1` y siguen funcionando.
- El runtime consume el blob `.wnui` v2; esta descripción de `/1` y `/2` es histórica.
- El subset v1 no incluye grid/scroll; el frontend las trata como
  **extensiones del piloto** (warning por defecto, `--pilot` para permitirlas).
