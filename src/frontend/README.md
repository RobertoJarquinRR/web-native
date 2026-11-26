# Frontend Rust

Compilador de UI de WebNative: convierte HTML + CSS en el **documento `.wnui`**
(blob v6) que consume el runtime C. No calcula geometría ni hornea texto: el
motor mide, shapea y pinta en runtime.

## Uso

```
frontend-rust.exe --html app/index.html --width 1280 --height 800 \
                  --emit-blob app.wnui [--css app/styles.css] \
                  [--strict-subset] [--pilot]
```

| Flag | Efecto |
|---|---|
| `--html` | página a compilar |
| `--width`, `--height` | viewport de referencia (resuelve `%`, `vw/vh`) |
| `--emit-blob` | salida `.wnui` v4; es la **única** salida y es obligatoria |
| `--css` | aceptado por compatibilidad; las hojas se descubren del `<link>`/`<style>` del documento |
| `--strict-subset` | cualquier feature fuera de `docs/subset-v1.md` es error (exit != 0) |
| `--pilot` | habilita grid, scroll, imágenes y `<input>` (extensiones v2/v3); `position` v4 no necesita flag |

Sin `--emit-blob` el binario no escribe nada y devuelve error de uso. El blob
es la única salida y el contrato.

## Etapas

```
HTML/CSS → parse (dom, css) → cascade → style_ext (grid/scroll/imágenes)
        → ui_tree (nodos pre-orden) → .wnui
```

El pipeline completo vive en la **librería** (`src/lib.rs`), no en `main.rs`:

```rust
use frontend_rust::{build_tree, build_tree_from_file};

let tree = build_tree_from_file(Path::new("page/index.html"), (960, 640))?;
for node in &tree.nodes {
    // node.kind, node.tag, node.classes, node.element_id, node.style, ...
}
tree.write_blob(Path::new("app.wnui"))?;
```

- `build_tree(html, base_dir, viewport)` ejecuta parseo + cascada y devuelve un
  `Tree` **en memoria**. `base_dir` resuelve los `<link>` relativos.
- `Tree.nodes` expone `kind`, `tag`, `classes`, `element_id`, `style`
  (incluye `css3`: bordes, radios, sombras y unidades relativas; `position`,
  `inset` e `inset_mask` para v4), `data`, `argb`, `text`, `image` e `input`.
- `Tree.detailed` indica si la página necesita los campos v2/v3 del blob (grid,
  scroll, imágenes, CSS v3 o input).
- `Tree::write_blob(path)` escribe el `.wnui` canónico.

Los tests de integración usan esa API y no lanzan procesos ni serializan JSON.

## Contenido del árbol

Cada nodo lleva `parent`, `kind`, geometría declarada, márgenes/padding,
colores, `data-*`, y para texto: `content`, `font_family`, `font_size`,
`font_weight` y `line_height`. No hay glifos, posiciones ni atlas: el runtime
mide y shapea. Los nodos de texto salen con `width/height = -1` en el blob.

Los inputs llevan solo `value`, `placeholder`, `max_length` y `password`.

El IME es runtime-only: el blob no lleva estado de composición. `core/input.c`
pinta y confirma la composición a través de `wn_ime.h`/`WN_EVENT_IME_*`, así que
recompilar el frontend no cambia nada para IME.

El formato binario y la vista de debug están en
[`docs/wnui-format.md`](../../docs/wnui-format.md). El dump (`wnui-dump`) es una
proyección de debug, **no un contrato**; la invariante es el hash del blob.

## Diagnósticos

- `warning[subset]`: tag/atributo fuera del subset v1 (no aborta salvo con
  `--strict-subset`).
- `warning[pilot]`: feature permitida por `--pilot` pero fuera del subset.
- Errores de parseo/cascada devuelven exit != 0 con el mensaje en stderr.

## Tests

```
cargo test --release
```

Cubren parseo/cascada, colores, grid, bordes/sombras, unidades relativas,
subset estricto, la API de librería y el determinismo del blob. Los fixtures
del runtime se regeneran con `wn-cli`; `wn-cli test suite blob` compara el blob
contra el documento builtin y `wn-cli test suite dump` fija la proyección de
debug.

## Histórico

El frontend también emitía el IR estático de R1 (`display_list.json`), un árbol
de estilos JSON intermedio y cabeceras C para el oráculo Python, y horneaba
texto con HarfBuzz/FreeType. El IR estático vive en `archive/r1-static/`; los documentos
`docs/ui-tree-v2.md`, `docs/ui-tree-v3.md` y `docs/ir-schema.md` describen el IR
retirado y se conservan solo como historia.
