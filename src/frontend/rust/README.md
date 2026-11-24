# Frontend Rust — paridad con el frontend Python (v1) + texto

Implementación en Rust del frontend HTML+CSS → IR de display-list, con el mismo
subconjunto que `benchmarks/frontend-parity/SPEC.md` y salida **idéntica** a la
del frontend de referencia (`src/frontend/python`). Incluye layout de
bloque/flex, rectángulos y **texto de una línea**: shaping con `rustybuzz`,
raster con `ab_glyph`, atlas R8 512×512 y ops `text_run` en el formato de
exp16. Además valida el contrato cerrado `docs/subset-v1.md` con
`--strict-subset` (warnings por defecto). Sin navegador, sin WebView, sin Skia,
sin Electron y sin FreeType ni HarfBuzz en el binario.

## Resultado

| Fixture | Viewport | Ops | Paridad | Bytes |
|---|---|---:|---|---|
| `contract` | 400×300 | 4 | **PASS** `verify_ir.py` (tol. 0.01 px) | 594 (idéntico byte a byte al golden) |
| `flex-page` | 480×320 | 23 | **PASS** `verify_ir.py` (tol. 0.01 px) | 3018 (idéntico byte a byte al golden) |
| `text-card` | 400×240 | 6 (5 rect + 1 `text_run`) | **PASS** `verify_text.py` (19/19 checks) | 2030 (idéntico byte a byte a exp16 y al Python actual) |

El IR de `text-card` coincide **byte a byte** con
`archive/experiments/16-text-atlas-c/display_list.json` y con lo que produce hoy
`src/frontend/python` para la misma fixture (mismo sha256 de fichero, incluido
CRLF). Los 4 glyphs son `[43, 82, 79, 68]` con avances
`[19.875, 16.40625, 6.78125, 14.25]` px, exactamente los de exp16.

Además del IR de R1, el binario tiene un modo `--emit-ui` que emite el árbol
de estilos **pre-layout** (`ui.json` + atlas) que consume R2. Ver
[Modo R2](#modo-r2-árbol-de-estilos-pre-layout---emit-ui): 28/28 nodos y
estilos idénticos al oráculo Python, 35/35 avances y `x` exactos.

El modo `--emit-ui` también emite el contrato **v2** (`webnative.ui-tree/2`)
para grid, scroll e imágenes, y la capa **v3** de bordes, radios, sombras y
unidades relativas, con `--pilot` para habilitar las extensiones en el
diagnóstico de subset. Ver
[Grid, scroll e imágenes v2](#grid-scroll-e-imágenes-v2-único-en---emit-ui) y
[Bordes, radios, sombras y unidades relativas](#bordes-radios-sombras-y-unidades-relativas-v3).

Pipeline interno (mediana de 10 ejecuciones directas, sin PowerShell):
**1,38 ms** (`contract`), **1,85 ms** (`flex-page`) y **6,72 ms** (`text-card`,
incluye shaping + raster + atlas + escritura de 256 KB). Binario release:
**1.722.368 bytes** (~1,64 MiB). RSS pico: **~4,1 / 4,2 / 5,7 MB**.

## Estructura

```
src/frontend/rust/
  Cargo.toml            dependencias fijadas + perfil release
  Cargo.lock            versiones exactas resueltas
  build.ps1             cargo build --release + BUILD_INFO.json
  run.ps1               contrato del harness (-Html -Width -Height -Out -Timings [-Atlas])
  bench-rust.ps1        benchmark local: 3 fixtures, 2 warmup + 10 runs, results/rust/
  verify_text.py        checks del IR de texto + atlas contra exp16 (19 checks)
  BUILD_INFO.json       binario, modo, rustc, tamaño, fecha UTC
  src/
    main.rs             CLI, orquestación del pipeline y timings
    dom.rs              HTML -> árbol con html5ever (TreeSink propio)
    css.rs              hojas de estilo, declaraciones y selectores
    color.rs            longitudes (`length`) y colores (`parse_color`)
    grid.rs             valores v2: repeat(auto-fill, <px>) y grid-auto-rows
    cascade.rs          estilo computado por nodo (especificidad, herencia, inline)
    layout.rs           layout de bloque/flex + line boxes + pintado pre-orden
    text.rs             shaping (rustybuzz), métricas/raster (ab_glyph), atlas R8
    emit.rs             JSON del IR, del atlas y de timings
    ui_tree.rs          --emit-ui: árbol de estilos pre-layout + texto para R2
    r1.rs               avisos de grid/scroll/imágenes en el camino R1
    sha256.rs           SHA-256 del atlas (sin dependencias)
    rss.rs              peak working set del proceso (Win32, sin dependencias)
    subset.rs           diagnóstico del contrato subset v1 (--strict-subset, --pilot)
  tests/
    ui_tree_v2.rs       tests de integración del contrato v2 (binario real)
```

Fixture nueva (copia del demo de exp09, sin tocar las fixtures rect):

```
benchmarks/frontend-parity/fixtures/text-card/input.html   (400×240, "Hola" 28px + 3 chips)
benchmarks/frontend-parity/fixtures/text-card/input.css
```

Artefactos de la última ejecución en `benchmarks/frontend-parity/results/rust/`:
`text-card.ir.json`, `text-card.atlas.bin`, `text-card.atlas.json`,
`text-card.timings.json`, `text-card.verify.json`,
`text-card.python-reference.json`, `direct-benchmark.json` y `benchmark.json`
(3 fixtures).

## Modo R2: árbol de estilos pre-layout (`--emit-ui`)

El IR normal es un display list **post-layout**: no permite recalcular a otro
viewport. `--emit-ui` emite, con el mismo parseo HTML/CSS y la misma cascada,
un árbol **pre-layout** para el runtime R2: sin geometría de cajas, con los
valores computados y el texto ya shapeado. El pase de layout no se ejecuta.

```powershell
frontend-rust.exe --html src\runtime\c-r2\page\index.html `
                  --css  src\runtime\c-r2\page\style.css `
                  --width 960 --height 640 `
                  --emit-ui build\ui.json
# -> build\ui.json + build\atlas.bin + build\atlas.json (si hay texto)
```

La ruta es opcional (`--emit-ui` sin valor usa `--out` o `ui.json`). Sin
`--emit-ui` el CLI se comporta exactamente como antes y no escribe `ui.json`.

### Esquema `webnative.ui-tree/1` (y `/2` con features v2)

JSON plano, pre-orden, sin dependencias de lenguaje. Índices estables dentro
del documento (el nodo 0 es `body`). Una página sin grid/scroll/imágenes ni la
capa CSS v3 (bordes/radios/sombras/unidades relativas) emite `/1` exactamente
como antes; con cualquiera de esas features emite `/2` y añade los campos
descritos en
[Grid, scroll e imágenes v2](#grid-scroll-e-imágenes-v2-único-en---emit-ui) y
[Bordes, radios, sombras y unidades relativas](#bordes-radios-sombras-y-unidades-relativas-v3).

| Campo | Tipo | Semántica |
|---|---|---|
| `schema` | string | `"webnative.ui-tree/1"` o `"webnative.ui-tree/2"` |
| `generator` | string | `"frontend-rust"` |
| `source` | `{html, css}` | Rutas tal como se pasaron al CLI |
| `viewport` | `{width, height}` | Viewport de compilación (960×640 en R2) |
| `rule_count` | int | Reglas CSS que casaron (incluye reglas sin nodos) |
| `node_count` | int | Longitud de `nodes` |
| `text` | `{glyph_count, text_run_count}` | Conteos globales de texto |
| `atlas` | objeto o `null` | `file`, `json`, `width`, `height`, `format`, `size_bytes`, `sha256`, `glyph_count`, `pixels_nonzero` |
| `nodes[]` | array | Nodos en pre-orden |

Cada nodo:

| Campo | Tipo | Semántica |
|---|---|---|
| `id` | int | Índice en `nodes` |
| `parent` | int | Índice del padre (`-1` en la raíz) |
| `children` | int[] | Hijos en orden de emisión (texto propio primero) |
| `kind` | `block` \| `flex` \| `grid` \| `text` \| `image` | Tipo de nodo (`grid`/`image` solo en `/2`) |
| `name` | string | `tag#id.clase` con sufijo `[n]` para duplicados |
| `tag` | string | Tag HTML (`#text` en nodos de texto) |
| `classes` | string[] | `class` separado por whitespace |
| `element_id` | string o null | Atributo `id` |
| `style.display` | string o null | `block`/`flex`; `null` en texto |
| `style.direction` | 0/1 | 0 row, 1 column (enum de R2) |
| `style.justify` | 0..3 | start, center, end, space-between |
| `style.align` | 0..3 | start, center, end, stretch |
| `style.wrap` | 0/1 | `flex-wrap: wrap`/`wrap-reverse` |
| `style.flex_direction`, `justify_content`, `align_items`, `flex_wrap` | string | Valores computados originales (además del enum numérico) |
| `style.gap` | float | px |
| `style.width_px`, `width_pct` | float | `-1` = auto; `%` conservado (30.0 = 30 %) |
| `style.height_px`, `height_pct` | float | Igual |
| `style.margin`, `padding` | float[4] | top, right, bottom, left |
| `style.background` | string o null | `#rrggbb` normalizado |
| `style.color` | string | Color de texto heredado (`#rrggbb`) |
| `style.font_size`, `font_family`, `font_weight` | float/string | Computados y heredados |
| `data.hover/press/selected/action` | string o null | Atributos `data-*` crudos |
| `argb.background/text/hover/press/selected` | uint32 | `0xAARRGGBB` precalculado (0 = ninguno) |
| `text` | objeto o null | Solo nodos `kind=text` |

En nodos de texto el objeto `style` es uniforme pero solo son significativos
`width_px`/`height_px` (caja medida), `font_size`, `font_family`, `font_weight`
y `color`; los campos flex quedan en valores neutros y `display` es `null`.

Nodo de texto (`text`):

| Campo | Tipo | Semántica |
|---|---|---|
| `content` | string | Texto colapsado (`white-space: normal`) |
| `baseline` | float | Ascender dentro del line box |
| `glyphs[].gid` | int | Glyph id del TTF |
| `glyphs[].advance` | float | Avance HarfBuzz (px; no lo consume R2) |
| `glyphs[].x`, `y` | float | Esquina superior izquierda relativa al nodo |
| `glyphs[].w`, `h` | int | Tamaño del bitmap |
| `glyphs[].u0/v0/u1/v1` | float | UVs del atlas R8 256×256 |

`atlas.bin` es R8 puro (`width*height` bytes, 256×256 para este contrato) y
`atlas.json` documenta tamaño, sha256, glyphs únicos y píxeles no cero. El
empaquetado es el shelf packer del oráculo (guarda de 1 px, orden de primera
aparición).

### Grid, scroll e imágenes v2 (único en `--emit-ui`)

El contrato completo está en `docs/ui-tree-v2.md`. Esta sección documenta lo
que el frontend Rust **parsea y emite**; el layout, el scroll y el decode de
imágenes son responsabilidad del runtime R2. En el camino R1 (`--out`) estas
features siguen sin implementarse: solo se avisa (ver
[R1 vs R2](#r1-vs-r2-avisos-y-paridad)).

`--pilot` habilita las extensiones en el diagnóstico de subset:

```
frontend-rust.exe --html page.html --width 480 --height 320 `
                  --emit-ui build\ui.json --pilot
                  [--strict-subset]
```

#### Cuándo se emite `webnative.ui-tree/2`

Se emite `/2` si **algún nodo emitido** usa una feature v2 (los nodos dentro de
un subárbol `display: none` no cuentan):

- `display: grid`;
- `overflow: auto` o `overflow: scroll` (scroll);
- un `<img>` (aunque el CSS lo deje `inline`);
- `grid-template-columns` o `grid-auto-rows` declarados (aunque el nodo no sea
  grid);
- cualquier feature de la capa CSS v3: `border*`, `border-radius`,
  `box-shadow`, `opacity != 1`, `min/max-*` o una unidad `em/rem/vh/vw`.

En caso contrario se emite `/1` y **no se escribe ninguno de los campos v2/v3**,
de modo que las páginas existentes siguen siendo byte a byte idénticas.
`overflow: hidden` no fuerza `/2`: ya existía en el subset v1 y el campo v2 solo
se emite cuando el schema es `/2`.

#### Campos de estilo v2

Se añaden al objeto `style` de **todos** los nodos cuando el schema es `/2`
(`null` = no declarado; los nodos de texto usan valores neutros):

| Campo | Tipo | Semántica |
|---|---|---|
| `grid_template_columns` | `{mode, size_px}` o `null` | `{"mode":"auto_fill","size_px":180.0}`; solo `auto-fill` |
| `grid_auto_rows` | `{"px": n}`, `"auto"` o `null` | altura de fila; `auto` = máximo de los hijos |
| `row_gap`, `column_gap` | float | px efectivos: el longhand gana a `gap` (`row-gap ?? gap ?? 0`) |
| `overflow` | `"hidden" \| "auto" \| "scroll"` o `null` | scroll solo vertical en v2 |
| `display` | string | en contenedores `block`/`flex`/`grid`; en imágenes se conserva el computado (`inline` o el que fije el CSS) |
| `height_px`, `width_px` | float | `-1.0` = auto (en imágenes, tamaño intrínseco del runtime) |

`gap` se mantiene con la semántica v1 (shorthand o fallback truthy a
`row-gap`/`column-gap`). `row_gap`/`column_gap` son los valores que debe usar el
runtime para separar celdas y filas.

Valores CSS aceptados (el resto se ignora y el subset lo diagnostica):

| Propiedad | Formas válidas |
|---|---|
| `display` | `block`, `flex`, `grid` (v2), `none` |
| `grid-template-columns` | `repeat(auto-fill, 180px)`; espacios y mayúsculas libres. Sin `auto-fit`, `fr`, `minmax()`, `span` ni varias pistas |
| `grid-auto-rows` | `240px` (o número sin unidad) o `auto` |
| `gap`, `row-gap`, `column-gap` | px (ya en v1) |
| `overflow` | `hidden`, `auto`, `scroll` (`visible` = sin campo) |
| `object-fit` | `fill`, `cover`, `contain` (default `fill`) |

#### Nodo imagen

Un `<img>` produce `kind:"image"` con `image` (solo en ese tipo de nodo):

```json
{
  "kind": "image",
  "tag": "img",
  "style": { "width_px": 180.0, "height_px": 240.0, "...": "..." },
  "image": {
    "src": "poster.png",
    "alt": "Demo",
    "object_fit": "cover"
  }
}
```

- `src`: atributo `src` tal cual (ruta local relativa al HTML o absoluta); el
  frontend **no** comprueba que exista ni la resuelve. `null` si falta.
- `alt`: atributo `alt` o `null` si falta.
- `object_fit`: `fill` por defecto; `cover`/`contain` si el CSS lo declara.
- Tamaño: `width`/`height` del CSS en px ganan; si no, los atributos HTML
  `width`/`height` (número o `Npx`); si tampoco, `-1.0` = intrínseco (WIC en el
  runtime).
- `<img>` es void: no tiene texto propio ni hijos. `display: none` lo excluye.

Los hijos se emiten en orden de documento (row-major para el grid lo resuelve
el runtime).

#### R1 vs R2: avisos y paridad

En el camino R1 (`--out`, display list) grid, scroll e imágenes **no** se
implementan. Para no producir rects silenciosamente incorrectos, el binario
recorre DOM+cascada después de calcular estilos e imprime:

```
warning[r1] div#gallery.gallery: 'display: grid' solo se implementa en R2 (--emit-ui); el IR de R1 no calcula layout grid
warning[r1] div#gallery.gallery: 'overflow: auto' (scroll) solo existe en R2 (--emit-ui); R1 no recorta ni desplaza
warning[r1] img.poster: <img> no se renderiza en el IR de R1; R2 (--emit-ui) emite kind:image
r1: 3 features solo-R2 ignoradas en el IR (usa --emit-ui para el arbol v2)
```

- Son avisos informativos: no cambian el exit code, ni siquiera con
  `--strict-subset`.
- El layout R1 sigue igual que antes: solo entran hijos `block`/`flex`; el
  subárbol grid/img se descarta (no se inventan rects) y el padre conserva su
  altura calculada como si no existiera.
- `--pilot` solo afecta al contrato de subset; **no** hace que R1 layout grid o
  pinte imágenes.

#### Diagnóstico de subset con `--pilot`

`grid` y `overflow` (auto/scroll) son **extensiones del piloto**; `object-fit`
también. `img` es v1. La matriz es:

| Feature | default | `--strict-subset` sin `--pilot` | `--pilot` |
|---|---|---|---|
| `display: grid` | warning | error, exit ≠ 0 | permitida |
| `grid-template-columns: repeat(auto-fill, <px>)` | warning | error, exit ≠ 0 | permitida |
| `grid-auto-rows: <px> \| auto` | warning | error, exit ≠ 0 | permitida |
| `overflow: auto \| scroll` | warning | error, exit ≠ 0 | permitida |
| `object-fit: fill \| cover \| contain` | warning | error, exit ≠ 0 | permitida |
| `overflow: hidden` | limpio (v1) | limpio | limpio |
| `<img>` y sus atributos `src`/`alt`/`width`/`height` | limpio (v1) | limpio | limpio |

`--pilot` no relaja valores inválidos: `grid-template-columns: 1fr`,
`grid-auto-rows: minmax(...)` o `object-fit: zoom` se siguen diagnosticando.

Decisión documentada: `alt`, `width` y `height` en `<img>` se aceptan como
**aditivos al subset v1** (el contrato solo nombra `src`) porque el nodo imagen
v2 los necesita. `object-fit` se trata como extensión del piloto, no como v1.

Campos exactos que debe consumir el runtime: `kind` (`grid`/`image`),
`style.grid_template_columns.mode`/`.size_px`, `style.grid_auto_rows` (`{"px"}`
o `"auto"`), `style.row_gap`, `style.column_gap`, `style.overflow`,
`style.width_px`/`style.height_px` (`-1` = auto/intrínseco) e
`image.src`/`image.alt`/`image.object_fit`.

### Bordes, radios, sombras y unidades relativas (v3)

Extensión **solo de frontend** (`--emit-ui`): el binario parsea, cascadea y
emite los campos; el layout y el pintado (borde, esquinas redondeadas, blur de
sombra) son responsabilidad del runtime R2. En R1 no se pinta nada de esta capa:
solo se avisa (ver [R1 vs R2](#r1-vs-r2-avisos-y-paridad)). El frontend **no**
modifica el display list de R1 ni inventa rects para bordes/sombras.

Se emite `webnative.ui-tree/2` si algún nodo emitido declara la capa v3
(`border*`, `border-radius`, `box-shadow`, `opacity != 1`, `min/max-*` o
cualquier unidad relativa). Los campos se añaden al objeto `style` de **todos**
los nodos cuando el schema es `/2`; los nodos de texto usan valores neutros
(`0`, `null`, `"none"`, `1.0`). Las páginas que no usan la capa v3 siguen
emitiendo `/1` **byte a byte idénticas**.

#### Forma exacta de los campos

Bordes (uniformes + detalle por lado):

| Campo | Tipo | Semántica |
|---|---|---|
| `border_width_px` | float | Ancho efectivo uniforme: máximo de los lados visibles (`solid` y ancho > 0); `0.0` si no hay borde |
| `border_width_em`/`_rem`/`_vh`/`_vw` | float o `null` | Unidad declarada si **los cuatro** lados visibles comparten exactamente la misma unidad relativa |
| `border_color` | string o `null` | Color del primer lado visible (top→left), `#rrggbb` o `#rrggbbaa` (alfa conservado); `null` sin borde visible |
| `border_style` | `"solid"` \| `"none"` | `solid` si algún lado visible es sólido; `none` si no hay borde efectivo |
| `border_sides` | objeto | `{ "top" \| "right" \| "bottom" \| "left": { "width_px": float, "width_em" \| "_rem" \| "_vh" \| "_vw": float \| null, "color": string \| null, "style": "solid" \| "none" } }`. El ancho se conserva aunque `style` sea `none` |

`border_sides.top.width_px` etc. solo se resuelven a px en build time; la
unidad relativa uniforme se expone en `border_width_*`. Si los lados mezclan
unidades, `border_width_*` queda `null` y el runtime usa `border_sides` (o el
máximo px como fallback).

Radio y sombras:

| Campo | Tipo | Semántica |
|---|---|---|
| `border_radius_px` | float | Radio uniforme; si las esquinas difieren, el máximo (el runtime puede usar `border_radii_px`) |
| `border_radius_em`/`_rem`/`_vh`/`_vw` | float o `null` | Radio declarado uniforme (las 4 esquinas con la misma unidad) |
| `border_radii_px` | float[4] | `[top-left, top-right, bottom-right, bottom-left]`, px resueltos |
| `box_shadow` | array o `null` | Lista de sombras; `null` = sin sombra |
| `box_shadow[].offset_x_px`/`.offset_y_px` | float | Desplazamiento (px; puede ser negativo) |
| `box_shadow[].blur_px` | float | Radio de difuminado (≥ 0) |
| `box_shadow[].spread_px` | float | Expansión (puede ser negativa) |
| `box_shadow[].color` | string | `#rrggbb` o `#rrggbbaa`; alfa `rgba()` conservado |
| `box_shadow[].inset` | bool | Sombra interior |
| `opacity` | float | `0.0`–`1.0` (se emite en `/2`; el valor inicial es `1.0`) |

Unidades relativas y `min/max`:

| Campo | Tipo | Semántica |
|---|---|---|
| `width_em`/`_rem`/`_vh`/`_vw` | float o `null` | Unidad declarada de `width`; `width_px` sigue siendo la resolución en build time |
| `height_em`/`_rem`/`_vh`/`_vw` | float o `null` | Igual para `height` |
| `min_width_px`/`_pct`, `max_width_px`/`_pct` | float | `min_*_px`/`max_*_px` = `-1.0` si no se declaró; `%` se conserva en `*_pct` |
| `min_width_em`/`_rem`/`_vh`/`_vw` | float o `null` | Unidad declarada de `min-width` |
| `max_width_em`/`_rem`/`_vh`/`_vw` | float o `null` | Unidad declarada de `max-width` |
| `min_height_px`/`_pct`/`_em`/`_rem`/`_vh`/`_vw` | float/`null` | Igual para `min-height` |
| `max_height_px`/`_pct`/`_em`/`_rem`/`_vh`/`_vw` | float/`null` | Igual para `max-height` |
| `margin_em`/`_rem`/`_vh`/`_vw` | `(float\|null)[4]` | `[top, right, bottom, left]`; `null` por lado si no usa esa unidad |
| `padding_em`/`_rem`/`_vh`/`_vw` | `(float\|null)[4]` | Igual para padding |
| `gap_em`/`_rem`/`_vh`/`_vw` | float o `null` | Unidad declarada de `gap` |
| `row_gap_em`/`_rem`/`_vh`/`_vw` | float o `null` | Unidad efectiva de `row-gap` (si no se declaró, hereda la de `gap`, igual que el px) |
| `column_gap_em`/`_rem`/`_vh`/`_vw` | float o `null` | Unidad efectiva de `column-gap` |

Los arrays `margin`/`padding` (px) y `gap`/`row_gap`/`column_gap` (px) siguen
existiendo con la semántica v1. Para cada propiedad, si el campo `*_em`,
`*_rem`, `*_vh` o `*_vw` no es `null`, el runtime debe usarlo (y recalcularlo al
cambiar el viewport); el `*_px` es la resolución de build time y sirve de
fallback.

Ejemplo (viewport 480×320, `html { font-size: 16px }`, card con
`font-size: 20px`):

```json
"style": {
  "width_px": 400.0,
  "width_em": 20.0,
  "height_px": 240.0,
  "height_rem": 15.0,
  "max_width_px": 288.0,
  "max_width_vw": 60.0,
  "padding": [20.0, 32.0, 20.0, 32.0],
  "padding_em": [1.0, null, 1.0, null],
  "padding_rem": [null, 2.0, null, 2.0],
  "gap": 3.2,
  "gap_vh": 1.0,
  "border_width_px": 2.0,
  "border_color": "#334155",
  "border_style": "solid",
  "border_sides": {
    "top": { "width_px": 2.0, "width_em": null, "width_rem": null, "width_vh": null, "width_vw": null, "color": "#334155", "style": "solid" },
    "right": { "width_px": 2.0, "width_em": null, "width_rem": null, "width_vh": null, "width_vw": null, "color": "#334155", "style": "solid" },
    "bottom": { "width_px": 2.0, "width_em": null, "width_rem": null, "width_vh": null, "width_vw": null, "color": "#334155", "style": "solid" },
    "left": { "width_px": 2.0, "width_em": null, "width_rem": null, "width_vh": null, "width_vw": null, "color": "#334155", "style": "solid" }
  },
  "border_radius_px": 12.0,
  "border_radii_px": [12.0, 12.0, 12.0, 12.0],
  "box_shadow": [
    { "offset_x_px": 0.0, "offset_y_px": 4.0, "blur_px": 12.0, "spread_px": 0.0, "color": "#00000066", "inset": false }
  ],
  "opacity": 1.0
}
```

Reglas de parseo/cascada:

- `border`, `border-<side>` y `border-width`/`border-style`/`border-color`
  expanden a longhands por lado en `css.rs` (1-4 valores con semántica CSS).
  El longhand declarado después del shorthand gana; `!important` e inline
  siguen la cascada normal.
- Solo `border-style: solid` y `none` son válidos; otros estilos ignoran el
  borde (el subset los diagnostica). El color inicial es `currentColor`.
- `border-radius` acepta 1-4 valores (uniforme o por esquina). La sintaxis
  elíptica `a / b` solo usa la parte horizontal y el subset la rechaza; los
  longhands `border-top-left-radius` etc. no se parsean (usa el shorthand).
- `box-shadow` acepta listas separadas por coma, `inset`, 2-4 longitudes
  (`x y [blur] [spread]`) y color opcional (por defecto `currentColor`).
  `rgba()` conserva el alfa (`#rrggbbaa`). Una sombra inválida descarta toda la
  declaración, como CSS.
- `em` usa el `font-size` computado del nodo; `rem` el de `html` (16 px por
  defecto); `vh`/`vw` el viewport de compilación (`1vh = height/100`).
- `min/max-*` aceptan px, `%`, `auto` y unidades relativas. El layout de R1 las
  ignora; solo se emiten para el runtime.

#### Subset y `--pilot`

El subset v1 mantiene **limpios** `border`/`border-width`/`border-style:
solid`/`border-color` y `border-radius` en px (contrato congelado, ver
`docs/subset-v1.md`). Las extensiones nuevas son del **piloto v3**:

| Feature | default | `--strict-subset` sin `--pilot` | `--pilot` |
|---|---|---|---|
| `box-shadow` válido | warning | error, exit ≠ 0 | permitida |
| `border-<side>-*` y `border-<side>` | warning | error, exit ≠ 0 | permitida |
| `min-width`/`max-width`/`min-height`/`max-height` válidos | warning | error, exit ≠ 0 | permitida |
| unidades `em/rem/vh/vw` en width/height/padding/margin/gap/border-width/radio | warning | error, exit ≠ 0 | permitida |
| `border: ...` con ancho relativo | warning | error, exit ≠ 0 | permitida |
| `border: <px> solid <color>` y `border-radius: <px>` | limpio (v1) | limpio | limpio |

`--pilot` no relaja valores inválidos: `box-shadow: 1px`, `border-style:
dashed`, `border-radius: 1 / 2` o una unidad desconocida siguen siendo
diagnóstico.

#### R1: avisos y ningún rect nuevo

En el camino R1 (`--out`) bordes, radios y sombras se ignoran: el display list
solo pinta fondos y texto. Para que no pase en silencio se emite:

```
warning[r1] div#card.card: 'border' solo se renderiza en R2 (--emit-ui); el IR de R1 pinta la caja sin borde
warning[r1] div#card.card: 'border-radius' solo se renderiza en R2 (--emit-ui); R1 no redondea esquinas
warning[r1] div#card.card: 'box-shadow' solo se renderiza en R2 (--emit-ui); R1 no pinta sombras
```

No se añaden ops nuevos ni se modifican los rects existentes. Las unidades
relativas sí se resuelven en la cascada para que el layout R1 sea correcto en el
viewport de compilación (R1 no reflowea).

#### Campos exactos que debe consumir el runtime

`style.border_width_px`, `style.border_color`, `style.border_style`,
`style.border_sides` (opcional si solo se pintan bordes uniformes),
`style.border_radius_px` y `style.border_radii_px`, `style.box_shadow[]`
(`offset_*_px`, `blur_px`, `spread_px`, `color`, `inset`), `style.opacity`, y
los campos de unidad `width_*`, `height_*`, `min_width_*`, `max_width_*`,
`min_height_*`, `max_height_*`, `margin_*`, `padding_*`, `gap_*`, `row_gap_*`
y `column_gap_*`. Si un campo de unidad es `null`, se usa el px equivalente ya
resuelto.

### Paridad con el oráculo Python

`emit_ui.py --compare` compara dos `ui_data.h`: nodos, estilos, `data-*`,
glyphs, runs y atlas. Resultado con este fixture (960×640):

| Dimensión | Resultado |
|---|---|
| Nodos / padres / hijos / kind | 28 = 28, 0 diferencias |
| Campos de estilo (incl. `%`, wrap, márgenes, padding, colores) | 0 diferencias |
| Nombres y `data-*` (`ui_meta.json`) | Idéntico |
| Glyphs / runs | 35 / 6 = 35 / 6 |
| Avances y `x` de glyph | 35/35 exactos |
| UVs | 33/35 exactas; 2 siguen a una caja 1 px distinta |
| Atlas | 256×256 = 256×256; 1 242/65 536 bytes distintos (AA) |

Única diferencia: el glyph `i` (gid 76) a 24 px. FreeType aplica TrueType
hinting y deja `bitmap_top=17`, `rows=17`; `ab_glyph` (sin hinting) da
`top=18`, `rows=18`. Eso desplaza 1 px ese bitmap y, como consecuencia, la
UV del último glyph de la misma shelf. No afecta a ningún `x`, avance, nodo,
estilo ni al tamaño del atlas. Es la diferencia esperada entre rasterizadores
y está reportada por `--compare` como `known`, no como fallo semántico.

```powershell
# oráculo Python para la misma página
python src\emitter\python\emit_ui.py --html src\runtime\c-r2\page\index.html `
  --css src\runtime\c-r2\page\style.css `
  --out build\oracle\ui_data.h --meta build\oracle\ui_meta.json

# comparación semántica (sale 1 solo con diferencias reales)
python src\emitter\python\emit_ui.py --compare `
  build\oracle\ui_data.h build\ui_data.h
# --strict: también falla por AA/bitmap-box/hinting
```

El `build.ps1` de R2 ejecuta estos pasos automáticamente (salvo
`-SkipParityCheck`).

## Arquitectura

La secuencia replica `webnative.cli.build_all`, etapa por etapa y con las
mismas estructuras de datos:

1. **HTML (`dom.rs`)** — `html5ever` 0.39 parsea el documento con un `TreeSink`
   propio sobre una arena indexada (`Vec<RawNode>` + enlaces
   padre/hijo/hermano), sin `RcDom`. Después se convierte a un árbol de solo
   elementos que imita a `html5lib` con `treebuilder="etree"`:
   `text` guarda el texto propio anterior al primer hijo, y los comentarios y el
   doctype se descartan. `scripting_enabled = false` para igualar el default de
   html5lib.
2. **Hojas de estilo** — se recorren los nodos en orden de documento: `<style>`
   aporta su texto y `<link rel=stylesheet>` se resuelve contra el directorio
   del HTML (se ignoran `http:`, `https:` y `//`). Si el fichero no existe se
   aborta con error, igual que la referencia.
3. **CSS (`css.rs`)** — `cssparser` 0.37 trocea la hoja en reglas cualificadas
   (preludio + bloque crudo) y las declaraciones en `;` de primer nivel,
   saltando comentarios y bloques anidados. Los valores se interpretan con las
   mismas reglas que `css_parse.py`: expansión de `margin`/`padding` a
   longhands, `!important` y `style="..."` reutilizando el mismo parser.
   Los selectores del subconjunto (tipo, `.clase`, `#id`, compuestos,
   descendiente, hijo `>` y listas con coma) se compilan a una estructura con
   especificidad `(ids, clases, tipos)`; lo no soportado descarta la regla (no
   rompe el parseo).
4. **Cascada (`cascade.rs`)** — para cada nodo en pre-orden: se heredan las
   propiedades de `cascade.INHERITED`, se ofertan las declaraciones de las
   reglas que casan y luego el `style=""`, con la misma clave de prioridad que
   el Python: `(!important, inline, especificidad, orden de regla, índice de
   declaración)` y reemplazo con `>=` (gana la última). Finalmente se normaliza
   a `ComputedStyle`: longitudes en px, `background-color` con prioridad sobre
   `background`, `display` con fallback a la tabla UA (head/link/style/... →
   `none`, `a`/`span`/... → `inline`, resto `block`), `gap` con el
   cortocircuito "truthy" del Python (0.0 es falsy) y palabras clave de flex en
   minúsculas. Se conservan `color`, `font-family` y `font-size` para el texto.
5. **Layout (`layout.rs`)** — puerto directo de `layout.py`: modelo border-box,
   bloque con `cursor_y = child.bottom + margin_bottom`, flex row/column con
   `gap`, `justify-content` (`flex-start|center|flex-end|space-between`),
   `align-items` (`stretch|flex-start|center|flex-end`), medición de cada hijo
   en (0,0) y traslado posterior del subárbol, `stretch` solo cuando el hijo no
   fijó la dimensión cruzada, y altura automática a partir del contenido.
   **Texto**: el texto propio del nodo se colapsa con `normalize_text` (`" ".join(
   text.split())`), se coloca antes de los hijos en `content_x`, con
   `baseline = content_y + ascender`, y avanza `ascender + descender` en y.
   Las métricas se inyectan como closure (`MetricsFn`) para que layout no
   dependa del rasterizador. Una sola línea por nodo, LTR.
6. **Texto (`text.rs`)** — tres pasos, igual que `text.py`:
   - **Métricas**: se replican `FT_Request_Metrics` + `FT_MulFix`/`FT_PIX_CEIL`
     /`FT_PIX_FLOOR` con los valores de `ttf-parser` (ascender/descender/upem
     del mismo TTF). Para Segoe UI 28 px da `(31.0, 8.0)`: baseline 20+31=51 y
     alto de línea 39, idénticos a FreeType.
   - **Shaping**: `rustybuzz` (puerto puro de HarfBuzz) sobre el TTF resuelto,
     buffer LTR + script `Latn` + language `en`, sin features explícitas (las
     default del font). La escala se redondea como el Python
     (`max(1, round(size*64))`) y cada avance/offset se convierte de unidades
     de fuente a 26.6 con redondeo (así `1454` unidades de 'H' → `1272` =
     `19.875 px`, igual que HarfBuzz). Para la fixture los gids, avances y
     offsets coinciden exactamente.
   - **Raster + atlas**: `ab_glyph` rasteriza el contorno sin TrueType hinting.
     Ojo con su API: `PxScale` es la **altura** de la fuente, no el em; se usa
     `PxScale = px * height_unscaled / upem`. La cobertura se cuantiza a 8 bits
     (`round(c*255)`). El atlas es shelf packing de 512×512 R8 con 1 px de
     guarda por glyph y claves `(font_file, gid, round(size))` en orden de
     primera aparición, igual que `GlyphAtlas` de Python.
7. **Pintado (`layout.rs`)** — pre-orden: rect del nodo si tiene background,
   después sus `TextRun` y después sus hijos, con el fondo del `body` primero.
   El orden de ops coincide con `_paint_all` del Python.
8. **Emisión (`emit.rs`)** — JSON escrito a mano con el mismo formato que
   `json.dumps(indent=2)`: geometría rect con el `round()` de Python
   (redondeo a par más cercano, `f64::round_ties_even`), floats de texto con
   `round(x, 3|4|6)` + repr corto (`.0` si es integral), escapes JSON para
   `text`/`font`/`font_file`, y los `\\` de la ruta de la fuente. En Windows se
   traduce `\n` a `\r\n`, como el `write_text` de Python, de modo que los
   artefactos quedan **byte a byte** iguales a los goldens.
9. **Timings** — `Instant` por etapa: `html` (lectura + parseo + hojas), `css`,
   `cascade`, `fonts` (creación del motor), `layout` (incluye la carga y
   métricas de la fuente la primera vez), `text` (shaping + raster + atlas),
   `emit` (serialización y escritura de IR + atlas) y `pipeline` (total). Se
   añade `parse` como alias de `html` (nombre usado por el SPEC) y
   `peak_rss_kb` vía `K32GetProcessMemoryInfo` (`kernel32`), sin dependencias
   externas.

### Shaping y rasterización: qué es contrato y qué no

El contrato de paridad es **contenido, posiciones y avances**:

| gid | glyph | x | y | w | h | adv | UV (u0,v0,u1,v1) |
|---:|---|---:|---:|---:|---:|---:|---|
| 43 | H | 22.0 | 31.0 | 16 | 20 | 19.875 | 0.001953, 0.001953, 0.033203, 0.041016 |
| 82 | o | 40.875 | 37.0 | 15 | 14 | 16.4062 | 0.037109, 0.001953, 0.066406, 0.029297 |
| 79 | l | 58.281 | 30.0 | 3 | 21 | 6.7812 | 0.070312, 0.001953, 0.076172, 0.042969 |
| 68 | a | 64.062 | 37.0 | 12 | 14 | 14.25 | 0.080078, 0.001953, 0.103516, 0.029297 |

Los 4 valores coinciden con exp16 sin tolerancia (el IR completo es idéntico).
Las **UVs coinciden porque el packing también**: rects `(1,1,16,20)`,
`(19,1,15,14)`, `(36,1,3,21)`, `(41,1,12,14)`, los mismos que
`archive/experiments/16-text-atlas-c/atlas.json` describe.

La **cobertura de píxeles no es idéntica** y no se pretende: FreeType usa el
intérprete TrueType (`FT_LOAD_TARGET_NORMAL`) y `ab_glyph` rasteriza el
contorno sin hinting. Para que las posiciones verticales cuadren con FreeType,
`text.rs` redondea los extremos verticales del contorno
(`top = round(ymax)`, `bottom = round(ymin)`) y recorta el bitmap a esa caja
entera. Con Segoe UI 28 px eso reproduce exactamente `bitmap_top`/`rows` de
FreeType para estos glyphs (incluido el overshoot de `o`/`a`); en otras
fuentes/tamaños puede desviarse ±1 px (ver limitaciones). El atlas resultante
tiene 462 bytes no nulos frente a 455 de FreeType, y mismas cajas/UVs.

### Resolución de fuentes

`font-family` se resuelve con el mismo mapa que `webnative/fonts.py`

| Familia CSS | Fichero |
|---|---|
| `Segoe UI` / `system-ui` / `sans-serif` | `%WINDIR%\Fonts\segoeui.ttf` |
| `Arial` / `Helvetica` / `Helvetica Neue` | `%WINDIR%\Fonts\arial.ttf` |
| `Verdana`, `Tahoma`, `Calibri`, `Georgia`, `Times New Roman`, `Consolas`, `Courier New`, `Segoe UI Light/Semibold/Semilight` | sus TTF de Windows |

Si la familia no está mapeada se busca una coincidencia parcial; si no, se cae
a `segoeui.ttf` y después a `arial.ttf`. El path absoluto acaba en el IR como
`font_file`.

## Crates y versiones

| Crate | Versión fijada | Uso |
|---|---|---|
| `html5ever` | `=0.39.0` | tokenizer + tree builder HTML5 |
| `cssparser` | `=0.37.0` | troceado de reglas, bloques y declaraciones |
| `rustybuzz` | `=0.20.1` | shaping (puerto puro Rust de HarfBuzz) |
| `ab_glyph` | `=0.2.32` | contornos + rasterización grayscale |

Dependencias transitivas relevantes (resueltas en `Cargo.lock`):
`ttf-parser 0.25.1`, `owned_ttf_parser 0.25.1`, `ab_glyph_rasterizer 0.1.10`,
`bytemuck 1.25.2`, `unicode-bidi-mirroring 0.4.0`, `unicode-ccc 0.4.0`,
`unicode-properties 0.1.5`, `unicode-script 0.5.9`, `smallvec 1.16.6`,
`bitflags 2.13.2`, `core_maths 0.1.1`, `libm 0.2.16`, `log 0.4.35`, además de
`markup5ever 0.39.0`, `tendril 0.5.1`, `web_atoms 0.2.6`,
`string_cache 0.9.0`, `phf 0.13.1`, `cssparser-macros 0.7.1`,
`dtoa-short 0.3.5`.

No se usa `serde`: el JSON del IR, del atlas y de timings se serializa a mano
para controlar el formato exacto. `sha256.rs` implementa SHA-256 (FIPS 180-4)
en ~100 líneas para no añadir otra dependencia. No hay dependencias del sistema
ni nada vendorizado: todo viene de crates.io (caché local de cargo).

Toolchain: `rustc 1.93.0 (254b59607 2026-01-19)` / `cargo 1.93.0`, edition 2021.
Perfil release: `opt-level = 3`, `lto = "fat"`, `codegen-units = 1`,
`panic = "abort"`, `strip = true`.

## Compilar y ejecutar

```powershell
cd .\src\frontend\rust
.\build.ps1                     # cargo build --release + BUILD_INFO.json

# Fixture de texto (escribe IR + atlas.bin/atlas.json junto al IR)
.\run.ps1 -Html ..\..\..\benchmarks\frontend-parity\fixtures\text-card\input.html `
          -Width 400 -Height 240 `
          -Out ..\..\..\benchmarks\frontend-parity\results\rust\text-card.ir.json `
          -Timings ..\..\..\benchmarks\frontend-parity\results\rust\text-card.timings.json `
          -Atlas ..\..\..\benchmarks\frontend-parity\results\rust\text-card.atlas.bin
```

CLI del binario:

```
frontend-rust.exe --html <path> --width <W> --height <H> --out <ir.json>
                  [--timings <timings.json>] [--atlas <atlas.bin>]
                  [--strict-subset] [--pilot]
frontend-rust.exe --html <path> --width <W> --height <H> --emit-ui [ui.json]
                  [--css <path>] [--timings <timings.json>]
                  [--strict-subset] [--pilot]
```

`--atlas` es opcional; por defecto se escribe `atlas.bin` en el directorio del
IR. `atlas.json` se escribe al lado del atlas (mismo stem). Si el documento no
tiene texto no se escribe ningún atlas, así que las fixtures rect siguen
dejando exactamente los mismos artefactos que antes.

`run.ps1` resuelve el ejecutable respecto a sí mismo y las rutas relativas
respecto al directorio actual (igual que el `run.ps1` de Python). Sale con 0 en
éxito y distinto de 0 ante cualquier error (HTML/CSS ilegible, hoja de estilos
no encontrada, fuente no disponible, atlas desbordado, argumentos inválidos) o
ante cualquier diagnóstico con `--strict-subset`. Acepta `-StrictSubset` y
`-Pilot` y los reenvía al binario.

## Diagnóstico de subset v1 (`--strict-subset`)

El binario valida el contrato cerrado de `docs/subset-v1.md` **sin tocar el
IR**: solo inspecciona el HTML de entrada, los bloques `<style>` embebidos, las
hojas `<link rel="stylesheet">` locales y los `style="..."` inline. Se ejecuta
tanto en el modo IR normal como con `--emit-ui`.

```powershell
frontend-rust.exe --html page.html --width 960 --height 640 `
                  --out page.ir.json --strict-subset
```

| Modo | Salida | Exit |
|---|---|---|
| sin flag (default) | `warning[subset] ...` por stderr; el pipeline continúa | 0 |
| `--pilot` | igual, pero no diagnostica `display: grid`, `grid-*`, `overflow: auto\|scroll`, `object-fit`, `box-shadow`, los bordes por lado, `min/max-*` ni las unidades `em/rem/vh/vw` (pilotos v2/v3) | 0 |
| `--strict-subset` | `error[subset] ...` por stderr; IR/atlas se escriben igual | 1 si hay ≥1 diagnóstico |
| `--strict-subset --pilot` | solo errores reales (valores inválidos, otras propiedades) | 1 si hay ≥1 error |

Formato de cada línea (archivo y línea siempre que el parser los da; si no, el
contexto entre corchetes es el selector, la propiedad o la etiqueta):

```
error[subset] <archivo>:<línea>: <mensaje> [<contexto>]
```

Ejemplos reales de `fixtures/subset/unsupported`:

```
error[subset] benchmarks\frontend-parity\fixtures\subset\unsupported\style.css:10: valor 'grid' no soportado en 'display' (subset v1: block | flex | none) [selector '#root', propiedad 'display']
error[subset] benchmarks\frontend-parity\fixtures\subset\unsupported\index.html:14: atributo 'onclick' no soportado por subset v1 [<div>]
error[subset] benchmarks\frontend-parity\fixtures\subset\unsupported\index.html:16: propiedad CSS 'transform' no soportada por subset v1 [style inline en <div>, propiedad 'transform']
```

Qué valida:

- **Etiquetas**: lista cerrada de v1 (`html`, `head`, `body`, `div`, `span`,
  `p`, `h1`–`h6`, `button`, `img`, `ul`, `ol`, `li`, `link`, `style`).
  `script`, `iframe`, `form`, `input`, `select`, `table`, `canvas`, `video`,
  `audio`, `svg`, `template`, `slot`, `object` y `embed` se marcan como fuera
  de v1.
- **Atributos**: `id`, `class`, `style`, `data-hover`, `data-press`,
  `data-selected`, `data-action` y `title` globales; `src` en `img`,
  `href`/`rel` en `link` y `disabled` en `button`. `src`/`href` remotos
  (`http:`, `https:`, `//`, `data:`) también se rechazan.
- **CSS**: allowlist de propiedades y valores obvios fuera del subset
  (`display: grid`, `grid-template-*`, `position`, `transform`, `transition`,
  `animation`, `box-shadow`, `float`, `filter`, `clip-path`, `z-index`,
  `columns`, `em/rem/vh/vw`, `calc()`, gradientes, `url()`, `hsl()`,
  `border: dashed`, `opacity > 1`, etc.). Las extensiones de los pilotos
  v2/v3 (`grid`, `object-fit`, `box-shadow`, bordes por lado, `min/max-*` y
  unidades relativas) pasan con `--pilot`.
- **At-rules y selectores**: `@media`, `@font-face`, `@import` y cualquier otra
  at-rule; pseudo-elementos (`::before`/`::after`), `:not()`/`:has()` y
  combinadores `+`/`~`.

La validación es previa al layout y no modifica la cascada ni la emisión. Las
fixtures `contract`, `flex-page` y `text-card` siguen produciendo el mismo IR
byte a byte y `bench.py` sigue en PASS.

### Comprobar las fixtures de subset

```powershell
# supported: sin diagnósticos, exit 0
frontend-rust.exe --html benchmarks\frontend-parity\fixtures\subset\supported\index.html `
                  --width 400 --height 300 --out build\supported.ir.json --strict-subset

# unsupported: 19 features fuera de v1, exit 1
frontend-rust.exe --html benchmarks\frontend-parity\fixtures\subset\unsupported\index.html `
                  --width 400 --height 300 --out build\unsupported.ir.json --strict-subset
```

Las rutas, la lista exacta de los 19 diagnósticos y los comandos sin
`--strict-subset` están en
`benchmarks/frontend-parity/fixtures/subset/README.md`.

`cargo test` ejecuta los tests unitarios de `grid.rs` (parseo de
`repeat(auto-fill, <px>)` y `grid-auto-rows`), los de `style_ext.rs` (unidades
relativas, bordes, radio y sombras), los de `subset.rs` (líneas de HTML/CSS,
comentarios, at-rules, valores y la matriz `--pilot`) y el vector conocido de
SHA-256. Además, `tests/ui_tree_v2.rs` ejecuta el binario real (`--emit-ui` y
`--out`) sobre fixtures temporales y verifica schema, kinds, estilos
grid/scroll, campos de imagen, la capa v3 (bordes/radios/sombras/unidades) y los
avisos/exit codes de R1 y del subset. No necesita el runtime C ni el oráculo
Python.

### Ambigüedades resueltas y límites del diagnóstico

- `rel` en `<link>` se acepta aunque `docs/subset-v1.md` solo nombre `href`: es
  necesario para reconocer la hoja de estilos.
- `display: none` y `width`/`height: auto` se aceptan como valores prácticos
  (el motor ya los soporta); el contrato solo nombra `block | flex` y `px | %`.
- `:root`, `:first-child`, `:last-child` y `:only-child` se aceptan porque el
  motor de selectores ya las resuelve. Las pseudo-clases no listadas, los
  pseudo-elementos y los combinadores `+`/`~` se diagnostican; el combinador
  hijo `>` y los selectores de atributo no se diagnostican.
- `font-family` no se valida contra la lista de fuentes del sistema; solo se
  rechazan `url()` y `@font-face`. `img src` tampoco comprueba que el archivo
  exista (eso es responsabilidad del runtime).
- Las posiciones HTML vienen de un escáner de etiquetas ligero (no de
  `html5ever`, que no expone offsets). Los atributos de una etiqueta ya
  rechazada no se inspeccionan por separado; el contenido de `script`, `style`,
  `title`, `textarea`, `iframe`, `xmp`, `noembed`, `noframes` y `plaintext` no
  se interpreta como HTML.
- La comprobación de selectores es heurística, no un parser completo. Los
  comentarios CSS se enmascaran conservando líneas para que no generen falsos
  positivos.
- `--strict-subset` no impide escribir IR/atlas/`ui.json`: solo cambia el exit
  code. CI debe mirar el código de salida.
- La página interna de R2 (`src/runtime/c-r2/page/`) emite tres warnings
  esperados (`lang`, `meta`, `title`): no es una página v1. Con
  `--strict-subset` fallaría, por diseño.

## Verificación de paridad

### Fixtures rect (sin cambios)

```powershell
python benchmarks\frontend-parity\tools\verify_ir.py `
  benchmarks\frontend-parity\golden\contract.ir.json `
  benchmarks\frontend-parity\results\rust\contract.ir.json
python benchmarks\frontend-parity\tools\verify_ir.py `
  benchmarks\frontend-parity\golden\flex-page.ir.json `
  benchmarks\frontend-parity\results\rust\flex-page.ir.json
```

```
PASS: benchmarks\frontend-parity\results\rust\contract.ir.json == benchmarks\frontend-parity\golden\contract.ir.json
PASS: benchmarks\frontend-parity\results\rust\flex-page.ir.json == benchmarks\frontend-parity\golden\flex-page.ir.json
```

Además de la tolerancia de 0,01 px, ambos JSON coinciden **byte a byte** con el
golden (594 y 3018 bytes), incluido el CRLF final.

Harness común (solo Rust):

```powershell
python benchmarks\frontend-parity\tools\bench.py --runs 3 --only rust
```

```
rust     contract   PASS  mode=direct   wall=12.868 ms
rust     flex-page  PASS  mode=direct   wall=12.971 ms
```

La ejecución completa (`--runs 3`, los 4 frontends) también pasa: Python, C++,
Rust y C# dan `PASS` en las dos fixtures rect.

### Fixture de texto

```powershell
cd .\src\frontend\rust
python verify_text.py `
  --ir ..\..\..\benchmarks\frontend-parity\results\rust\text-card.ir.json `
  --atlas ..\..\..\benchmarks\frontend-parity\results\rust\text-card.atlas.bin `
  --atlas-json ..\..\..\benchmarks\frontend-parity\results\rust\text-card.atlas.json `
  --reference ..\..\..\benchmarks\frontend-parity\results\rust\text-card.python-reference.json `
  --reference-atlas ..\..\..\archive\experiments\16-text-atlas-c\atlas.bin `
  --report ..\..\..\benchmarks\frontend-parity\results\rust\text-card.verify.json
```

```
PASS ir_json_parse
PASS ir_schema                        (jsonschema, src/ir/schema.json)
PASS viewport_400x240
PASS has_text_run                     1 op text_run
PASS glyph_count_4                    4 glyphs
PASS glyph_ids                        gids=[43, 82, 79, 68]
PASS run_origin                       (20.0, 51.0)
PASS text_metadata                    Hola / Segoe UI / segoeui.ttf / 28.0 / #ffffff
PASS reference_glyph_count            4 vs 4
PASS glyph_xy_vs_reference            tolerancia 0.5 px
PASS advances_vs_reference            tolerancia 0.5 px
PASS op_order_vs_reference            rect, rect, text_run, rect, rect, rect
PASS uv_matches_shelf_packing         rects=[(1,1,16,20),(19,1,15,14),(36,1,3,21),(41,1,12,14)]
PASS atlas_bytes_512x512_r8           262144 bytes
PASS atlas_guard_and_outside_zero     guard=[] outside_nonzero=0
PASS atlas_has_pixels                 462 bytes != 0
PASS atlas_json_sha256                cd8ece22…588a0
PASS atlas_json_metadata              512x512 r8unorm glyph_count=4
PASS atlas_same_geometry              misma caja/UV; cobertura distinta
19/19 checks PASS
```

El IR de la fixture es **byte a byte** igual a exp16 y al Python actual:

```
rust text-card.ir.json                        2030 bytes
src/frontend/python (build_all, text-card)    2030 bytes  == exp16 display_list.json
archive/experiments/16-text-atlas-c/display_list.json 2030 bytes
sha256 de los tres ficheros: 8d32d008b37694c6ed92056641d3204b747973da14b32b1059d722697743e8e6
```

Atlas de la fixture (`text-card.atlas.bin`):

| Dato | Valor |
|---|---|
| Tamaño | 512×512 R8 crudo, 262.144 bytes |
| Glyphs | 4 (H, o, l, a) |
| Bytes no nulos | 462 (FreeType: 455) |
| sha256 | `cd8ece2213648b45a419823f58381e675837914d6d59948a6783c737105588a0` |
| Rects shelf | `(1,1,16,20)`, `(19,1,15,14)`, `(36,1,3,21)`, `(41,1,12,14)` |
| Guarda 1 px / exterior | todo 0 |

Diferencia de cobertura contra el atlas de FreeType (misma caja y UV):

| gid | w×h | píxeles no nulos (ref/rust) | cobertura (ref/rust) | max diff | mean diff | píxeles idénticos |
|---:|---|---|---|---:|---:|---:|
| 43 H | 16×20 | 150 / 160 | 28810 / 28313 | 101 | 1,853 | 74,69 % |
| 82 o | 15×14 | 120 / 117 | 20298 / 20476 | 78 | 10,762 | 52,86 % |
| 79 l | 3×21 | 63 / 63 | 12075 / 11835 | 70 | 3,810 | 31,75 % |
| 68 a | 12×14 | 122 / 122 | 20900 / 20564 | 105 | 15,845 | 39,88 % |

La cobertura total por glyph difiere menos del 2 %; lo que cambia es el
antialiasing (unhinted vs hinting TrueType), no la geometría.

### Prueba diferencial adicional (fuera del repo, ad-hoc)

Una página con 5 runs (`Segoe UI` 28, `Arial` 16, `Segoe UI` 12, `Verdana` 20
y fallback a Arial) y 38 glyphs se comparó contra `build_all` del Python:

- **gids**: 100 % iguales.
- **avances**: diferencia máxima 0,016 px (1/64 px por redondeo de kerning de
  `rustybuzz` en Arial 16).
- **x acumulada**: diferencia máxima 0,031 px.
- **y / w / h**: exactos en Segoe UI 12/28 y Verdana 20; en Arial 16 cuatro
  mayúsculas y `o`/`a` difieren 1 px por el encaje vertical (ver limitaciones).
- **font_file / font / size / color / orden de ops**: exactos.

## Rendimiento

Metodología directa: 2 warmups + 10 ejecuciones del binario (sin PowerShell),
midiendo wall con `time.perf_counter` y timings internos del JSON.

| Fixture | Wall mediana | Wall min | Wall max | Pipeline mediana |
|---|---:|---:|---:|---:|
| `contract` | 10,097 ms | 9,724 ms | 11,595 ms | 1,357 ms |
| `flex-page` | 10,780 ms | 10,256 ms | 12,079 ms | 1,800 ms |
| `text-card` | 15,674 ms | 14,808 ms | 16,078 ms | 6,729 ms |

Timings internos medianos (`bench-rust.ps1`, 10 runs, ms):

| Etapa | `contract` | `flex-page` | `text-card` |
|---|---:|---:|---:|
| `html` (parse HTML + hojas) | 0,507 | 0,631 | 0,598 |
| `css` (parse CSS) | 0,068 | 0,197 | 0,097 |
| `cascade` | 0,084 | 0,242 | 0,106 |
| `fonts` (crear motor) | 0,000 | 0,000 | 0,000 |
| `layout` (+ métricas de texto) | 0,007 | 0,027 | 1,074 |
| `text` (shaping + raster + atlas) | 0,002 | 0,004 | 0,611 |
| `emit` (IR + atlas a disco) | 0,717 | 0,766 | 4,204 |
| `pipeline` | **1,380** | **1,854** | **6,717** |
| `ops` | 4 | 23 | 6 |
| `peak_rss_kb` | 4.052 | 4.152 | 5.736 |

`layout` en `text-card` incluye la lectura y validación del TTF de 955 KB la
primera vez; `emit` incluye escribir el atlas de 262 KB (de ahí sus ~4,2 ms,
dominados por I/O y antivirus, no por CPU). El coste de shaping+raster+atlas en
sí es **~0,6 ms**.

Comparación con la referencia Python en la misma máquina para `text-card`:

| Implementación | Primera ejecución (frío) | Mediana en caliente (6 builds en el mismo proceso) |
|---|---:|---:|
| Python `build_all` (IR, sin atlas) | 214,9 ms (192 ms cargando FreeType/HarfBuzz/fuente) | 7,68 ms |
| Rust release (IR + atlas) | 6,72 ms de pipeline (15,7 ms wall con arranque) | 6,72 ms |

En caliente ambos pipelines son comparables (Rust además escribe el atlas);
en frío Rust evita la inicialización de FreeType/HarfBuzz y las DLLs de Python.

Wall vía PowerShell (`run.ps1`) es ~270–310 ms en las tres fixtures: lo domina
el arranque de PowerShell, no el frontend (el harness lanza un proceso nuevo).

## Tamaño y RSS

- Binario: `target/release/frontend-rust.exe` = **1.722.368 bytes** (~1,64 MiB),
  release con LTO, `codegen-units=1`, `panic=abort` y `strip` (ver
  `BUILD_INFO.json`). Antes del texto eran 887.296 bytes; el incremento
  (~796 KB) es `rustybuzz` + `ttf-parser` + `ab_glyph` + `ab_glyph_rasterizer`
  y el emisor `--emit-ui`. El diagnóstico de subset v1 añade ~37,5 KB respecto
  a la versión anterior (1.683.968 bytes).
- Peak working set: **~4,1 MB** (`contract`), **~4,2 MB** (`flex-page`) y
  **~5,7 MB** (`text-card`, incluye el TTF de 955 KB y el atlas de 256 KB).
- El RSS lo mide el propio binario con `K32GetProcessMemoryInfo` y se escribe en
  el JSON de timings como `peak_rss_kb`.

## Decisiones y detalles replicados a propósito

- **Redondeo bancario**: la geometría rect se emite con `round()` de Python (a
  par más cercano), no con `round half away from zero`. Afecta a valores como
  107,5 → 108 y 0,5 → 0. En texto, `round(x, 3|4|6)` se replica con
  `round_ties_even` sobre la cuantización decimal.
- **CRLF**: Python escribe con `write_text` en Windows y traduce `\n` a `\r\n`;
  el binario hace lo mismo para que el IR sea byte-idéntico.
- **`background-color` gana a `background`** siempre que exista, sin importar el
  orden en el CSS (es el `or` del Python sobre el dict de declaraciones).
- **`gap: 0` es falsy** en el fallback a `row-gap`/`column-gap`, como en Python.
- **`width: 50%` / `margin: auto`** se tratan como `auto` (sin unidad px válida).
- **`style=""`** se computa con la misma expansión de shorthands y clave de
  prioridad que las reglas, con `inline=1`.
- **Hijos renderizables**: solo `display: block` o `flex`; `inline`, `none` y
  el resto se descartan para layout, igual que `_renderable_children`.
- **Sin colapso de márgenes**: `margin-top` de un hijo suma, no colapsa.
- **Motor de texto perezoso**: el Python crea `TextEngine` si *cualquier* estilo
  tiene texto no vacío (incluido el CSS de un `<style>`); el Rust solo lo usa
  cuando un nodo con texto se layouta. La salida es idéntica y las fixtures rect
  no pagan la lectura del TTF. Es una desviación consciente, solo de rendimiento.
- **Avances a 26.6**: `rustybuzz` devuelve unidades de fuente; la conversión
  replica el redondeo de HarfBuzz con `font.scale = 26.6`, en vez de usar el
  float sin redondear. Así `adv` y las x acumuladas coinciden con exp16.
- **Atlas determinista**: claves en orden de primera aparición, shelf packing,
  1 px de guarda y fallo de build si desborda, igual que `GlyphAtlas`.
- **sha256 propio**: el hash del atlas se calcula con `sha256.rs` (vectores de
  test `""`, `"abc"` y el mensaje de 56 bytes de FIPS) para no añadir `sha2`.
- **`font` vs `font_file`**: `font` es la primera familia CSS tal cual (p. ej.
  `sans-serif` o `Nope`), `font_file` es el TTF realmente resuelto; igual que
  `TextOp` del Python.

## Limitaciones

- **Texto**: una sola línea por nodo, LTR y Latin. Sin wrapping, `text-align`,
  `line-height`, `letter-spacing`, `text-decoration`, `white-space: pre`, bidi,
  ni elementos inline dentro de una misma línea. Sin fallback de fuentes por
  glyph: toda la run usa la familia resuelta (si falta un glyph, se emite con
  w=h=0). Sin subpixel LCD: solo cobertura grayscale R8.
- **Hinting**: no se implementa el intérprete TrueType. El encaje vertical
  (`round(ymax)`/`round(ymin)`) reproduce a FreeType en la fixture (Segoe UI
  28 px) y en Segoe UI 12 / Verdana 20, pero puede desviar ±1 px en otras
  combinaciones (p. ej. Arial 16: `A`, `V`, `W`, `T`, `o`, `a`). Es una
  diferencia de *posicionamiento de bitmap*, no de avance ni de contenido.
- **Antialiasing distinto**: los bytes del atlas no son los de FreeType (misma
  caja y UV, cobertura distinta). La paridad de píxeles con exp09 no se puede
  afirmar para el atlas Rust; sí la paridad de IR/geometría.
- **HarfBuzz vs rustybuzz**: puede haber diferencias de 1/64 px en avances con
  kerning en algunas fuentes/tamaños (visto en Arial 16; máximo 0,016 px). Para
  la fixture Segoe UI 28 no hay ninguna.
- **Un atlas por documento de 512×512**: si no caben los glyphs, el build falla
  con error (`atlas de glyphs desbordado`). Sin páginas múltiples ni atlas
  incremental.
- **Selectores**: tipo/universal/`.clase`/`#id`/`[attr]`/`[attr=valor]`,
  descendiente, hijo `>` y listas con coma. Sin `+`/`~`, sin pseudo-clases más
  allá de `:root`, `:first-child`, `:last-child`, `:only-child`, sin
  pseudo-elementos ni `:not()`. Una regla con algo no soportado se descarta
  silenciosamente.
- **At-rules**: `@media`, `@supports`, `@import`, etc. se ignoran; no se
  desciende a bloques anidados.
- **Colores**: `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb()`/`rgba()` (coma o
  espacios, porcentajes) y una tabla corta de nombres. Sin `hsl()`, `lab()`,
  `color()` ni nombres exóticos. Alfa 0 → sin fondo.
- **Unidades**: en R1 `px` y números sin unidad; `%` solo en `width`/`height`.
  La capa v3 (`--emit-ui`) además parsea y emite `em`, `rem`, `vh`, `vw` en
  width/height/padding/margin/gap/border-width/radio y en `min/max-*`; se
  resuelven a px con el font-size/viewport de compilación y conservan la unidad
  declarada para el runtime. `calc()` y unidades desconocidas siguen fuera.
- **Propiedades de layout**: sin `border` pintado, sin `box-sizing: content-box`,
  sin margin collapsing, floats, `position`, `overflow` real (R1 no recorta),
  `flex-wrap`, `flex-grow/shrink`, `order`, `align-self` ni aplicación de
  `min/max-width/height` al layout. `display: grid`,
  `grid-template-columns`, `grid-auto-rows`, `overflow: auto|scroll`, `<img>` y
  `object-fit` se **parsean y emiten** en `--emit-ui` para R2, pero este
  frontend no calcula su layout, scroll ni decode. Bordes, radios, sombras y
  `opacity` también se **parsean/cascadean/emiten**; su pintado es del runtime.
- **Bordes v3**: los campos uniformes (`border_width_px`, `border_color`,
  `border_style`) cubren el caso de un borde igual en los cuatro lados; el
  detalle por lado queda en `border_sides`. No se calcula geometría de borde:
  el runtime decide si pinta por lado o solo el uniforme. Un `border-radius`
  con porcentajes o sintaxis elíptica (`1px / 2px`) no se emite (el subset lo
  diagnostica).
- **Sombras v3**: se emiten offset/blur/spread/color/inset; el blur real
  (Gaussian/box) lo implementa el runtime. Sin `drop-shadow` ni filtros.
- **Unidades relativas y reflow**: `width_px`/`height_px`/`margin`/`padding`/
  `gap` son la resolución en el viewport de compilación. Para reflow real el
  runtime debe preferir `*_em`/`*_rem`/`*_vh`/`*_vw` cuando no sean `null`.
- **Profundidad de layout** acotada a 512 niveles para no desbordar la pila.
- **Una sola pasada**: no hay recálculo ni incrementalidad; el benchmark mide
  una carga en frío por proceso.
- **Windows**: CRLF, RSS, rutas de fuentes y `%WINDIR%` son específicos de
  Windows (el resto es portable; en otras plataformas se emite `\n` y
  `peak_rss_kb = 0`).
- **`--emit-ui` y hinting**: `ab_glyph` no aplica el intérprete TrueType de
  FreeType. En este fixture discrepa 1 px la caja del glyph `i` a 24 px
  (17→18 px de alto); el resto de glyphs, avances, posiciones y UVs coinciden.
  El atlas difiere en antialiasing.
- **`--emit-ui` no implementa layout**: emite estilos y texto pre-shapeado.
  `%`, `flex-wrap`, `data-*` y tipos de nodo se conservan, pero la geometría es
  responsabilidad del runtime R2.
- **Grid v2**: solo `repeat(auto-fill, <px>)` con un tamaño fijo; sin `auto-fit`,
  `fr`, `minmax()`, `grid-area`, `span`, grid anidado ni scroll horizontal. El
  alto de fila `auto` lo resuelve el runtime con el máximo de los hijos. No hay
  validación de que `grid-template-columns` y `display: grid` coexistan: si
  falta el template, el runtime decide el fallback.
- **Imágenes v2**: el frontend no resuelve la ruta `src`, no comprueba que el
  archivo exista y no lee el tamaño intrínseco (eso es WIC/cache del runtime).
  Sin `width`/`height` ni CSS, emite `-1.0` para que el runtime use el
  intrínseco. Sin soporte de `srcset`, `loading`, `object-position` ni recorte
  en el frontend.
- **R1 y features v2/v3**: grid/scroll/imágenes y bordes/radios/sombras se
  ignoran en el display list; se emite `warning[r1]` por stderr para que no haya
  rects silenciosamente incorrectos. `--pilot` no cambia este comportamiento.

## Reproducir el benchmark

```powershell
cd .\src\frontend\rust
.\build.ps1
.\bench-rust.ps1            # 3 fixtures, 2 warmup + 10 runs; results/rust/benchmark.json
```

También se puede usar el harness común (solo cubre las fixtures rect):

```powershell
cd .
python benchmarks\frontend-parity\tools\bench.py --runs 10
```

## Integración con el runtime C (recomendación)

El contrato ya está separado en dos mitades limpias:

1. **Build (Rust)**: `frontend-rust.exe` produce
   `display_list.json` + `atlas.bin` + `atlas.json`. El IR de texto es
   **exactamente** el de exp16, así que `emit_c.py` puede consumirlo sin
   cambios: los quads salen de `ops[].glyphs[]` y el atlas de `atlas.bin`.
2. **Runtime (C)**: el patrón de `archive/experiments/16-text-atlas-c/runtime_text.c`
   sirve tal cual:
   - subir el atlas 512×512 como textura `R8Unorm`, sampler `linear` +
     `clamp_to_edge`;
   - pipeline de glyphs con `textureSample(atlas, sampler, uv).r`, color por
     vértice y alpha blend;
   - los `x/y/w/h` de cada glyph ya son absolutos (esquina superior izquierda);
     no hace falta fuente, HarfBuzz ni FreeType en runtime;
   - conservar el smoke test de hash del atlas (sha256 en build, FNV-1a al
     subir a GPU) y fallar el build si el atlas desborda;
   - **orden painter**: el `text_run` va intercalado en el IR (aquí antes de
     los chips). El runtime de exp16 dibuja todos los rects y luego todos los
     glyphs; en esta fixture no se solapan y el resultado es correcto, pero para
     el caso general conviene emitir batches contiguos y respetar el orden de
     ops;
   - si R2 usa un target sRGB, decidir gamma explícita y volver a medir; exp16
     usa `RGBA8Unorm` a propósito.
3. **Presupuesto**: atlas 512² R8 = 256 KB por documento; el binario Rust de
   build-time pesa ~1,64 MiB (frente a 887 KB sin texto). No se distribuye al
   runtime: solo el IR y el atlas.
4. **R2 (layout dinámico)**: no usa `display_list.json`, porque recalcula
   layout en runtime. Usa `--emit-ui` (árbol pre-layout + atlas 256²) →
   `emit_ui.py --ui-json` → `ui_data.h`. Es el camino por defecto de
   `src/runtime/c-r2/build.ps1`; `-UsePythonTree` queda como oráculo/fallback.

Con esto el pipeline AOT puede generar texto sin Python, sin FreeType/HarfBuzz
y sin ficheros de fuente, con paridad de IR medida contra exp16 y contra el
frontend Python actual.
