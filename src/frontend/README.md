# Frontend Rust

Compilador de UI de WebNative: convierte HTML + CSS en el árbol de estilos
**pre-layout** (`ui.json`) que consume el runtime C. No calcula geometría ni
hornea texto; el motor mide, shapea y pinta en runtime.

## Uso

```
frontend-rust.exe --html app/index.html --width 1280 --height 800 \
                  --emit-ui app/ui.json [--emit-header app/ui_data.h] \
                  [--emit-blob app.wnui] [--css app/styles.css] \
                  [--strict-subset] [--pilot]
```

| Flag | Efecto |
|---|---|
| `--html`, `--css` | página a compilar; `--css` es opcional si el HTML enlaza la hoja |
| `--width`, `--height` | viewport de referencia (resuelve `%`, `vw/vh`, media queries) |
| `--emit-ui` | salida `ui.json`; obligatorio |
| `--emit-header` | `ui_data.h` opcional, byte a byte igual al del emisor Python |
| `--emit-blob` | documento `.wnui` v2 opcional, byte a byte igual al del emisor |
| `--strict-subset` | cualquier feature fuera de `docs/subset-v1.md` es error (exit != 0) |
| `--pilot` | habilita grid, scroll, imágenes y `<input>` (extensiones v2/v3) |

Sin `--emit-ui` el binario no escribe nada y devuelve error de uso.

## Etapas

```
HTML/CSS → parse (dom, css) → cascade → style_ext (grid/scroll/imágenes)
        → ui_tree (nodos pre-orden) → ui.json
```

Con `--emit-blob` el binario escribe el `.wnui` v2 y con `--emit-header`
escribe `ui_data.h` idéntico al de `src/emitter/python/emit_ui.py --ui-json`
(sin atlas ni tablas de glifos): el emisor Python ya no hace falta para
compilar una página.

## Contrato `ui.json`

- `webnative.ui-tree/1`: página base.
- `/2`: añade grid, scroll e imágenes.
- `/3`: añade `<input type="text">` (solo con `--pilot`).

Cada nodo lleva `parent`, `kind`, geometría declarada, márgenes/padding,
colores, `data-*`, y para texto: `content`, `font_family`, `font_size`,
`font_weight` y `line_height`. No hay glifos, posiciones ni atlas: el runtime
mide y shapea. Los nodos de texto salen con `width/height = -1`.

Los inputs llevan solo `value`, `placeholder`, `max_length` y `password`.

`atlas` es siempre `null`; no se escribe `atlas.bin`.

## Diagnósticos

- `warning[subset]`: tag/atributo fuera del subset v1 (no aborta salvo con
  `--strict-subset`).
- `warning[pilot]`: feature permitida por `--pilot` pero fuera del subset.
- Errores de parseo/cascada devuelven exit != 0 con el mensaje en stderr.

## Tests

```
cargo test --release
```

Cubren parseo/cascada, extensiones v2/v3, subset estricto y el contrato de
`ui.json`. Los fixtures del runtime se regeneran con `wn-cli` (`test suite
fidelity` compara el emisor contra sí mismo).

## Histórico

El frontend también emitía el IR estático de R1 (`display_list.json`) y
horneaba texto con HarfBuzz/FreeType. Retirado: el runtime R1 vive en
`archive/r1-static/` y su benchmark congelado en `archive/benchmarks-r1/`.
