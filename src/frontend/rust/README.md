# Frontend Rust — paridad con el frontend Python (v1) + texto

Implementación en Rust del frontend HTML+CSS → IR de display-list, con el mismo
subconjunto que `benchmarks/frontend-parity/SPEC.md` y salida **idéntica** a la
del frontend de referencia (`src/frontend/python`). Incluye layout de
bloque/flex, rectángulos y **texto de una línea**: shaping con `rustybuzz`,
raster con `ab_glyph`, atlas R8 512×512 y ops `text_run` en el formato de
exp16. Sin navegador, sin WebView, sin Skia, sin Electron y sin FreeType ni
HarfBuzz en el binario.

## Resultado

| Fixture | Viewport | Ops | Paridad | Bytes |
|---|---|---:|---|---|
| `contract` | 400×300 | 4 | **PASS** `verify_ir.py` (tol. 0.01 px) | 594 (idéntico byte a byte al golden) |
| `flex-page` | 480×320 | 23 | **PASS** `verify_ir.py` (tol. 0.01 px) | 3018 (idéntico byte a byte al golden) |
| `text-card` | 400×240 | 6 (5 rect + 1 `text_run`) | **PASS** `verify_text.py` (19/19 checks) | 2030 (idéntico byte a byte a exp16 y al Python actual) |

El IR de `text-card` coincide **byte a byte** con
`experiments/16-text-atlas-c/display_list.json` y con lo que produce hoy
`src/frontend/python` para la misma fixture (mismo sha256 de fichero, incluido
CRLF). Los 4 glyphs son `[43, 82, 79, 68]` con avances
`[19.875, 16.40625, 6.78125, 14.25]` px, exactamente los de exp16.

Además del IR de R1, el binario tiene un modo `--emit-ui` que emite el árbol
de estilos **pre-layout** (`ui.json` + atlas) que consume R2. Ver
[Modo R2](#modo-r2-árbol-de-estilos-pre-layout---emit-ui): 28/28 nodos y
estilos idénticos al oráculo Python, 35/35 avances y `x` exactos.

Pipeline interno (mediana de 10 ejecuciones directas, sin PowerShell):
**1,38 ms** (`contract`), **1,85 ms** (`flex-page`) y **6,72 ms** (`text-card`,
incluye shaping + raster + atlas + escritura de 256 KB). Binario release:
**1.683.968 bytes** (~1,61 MiB). RSS pico: **~4,1 / 4,2 / 5,7 MB**.

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
    cascade.rs          estilo computado por nodo (especificidad, herencia, inline)
    layout.rs           layout de bloque/flex + line boxes + pintado pre-orden
    text.rs             shaping (rustybuzz), métricas/raster (ab_glyph), atlas R8
    emit.rs             JSON del IR, del atlas y de timings
    ui_tree.rs          --emit-ui: árbol de estilos pre-layout + texto para R2
    sha256.rs           SHA-256 del atlas (sin dependencias)
    rss.rs              peak working set del proceso (Win32, sin dependencias)
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

### Esquema `webnative.ui-tree/1`

JSON plano, pre-orden, sin dependencias de lenguaje. Índices estables dentro
del documento (el nodo 0 es `body`).

| Campo | Tipo | Semántica |
|---|---|---|
| `schema` | string | `"webnative.ui-tree/1"` |
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
| `kind` | `block` \| `flex` \| `text` | Tipo de nodo |
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
`experiments/16-text-atlas-c/atlas.json` describe.

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
frontend-rust.exe --html <path> --width <W> --height <H> --emit-ui [ui.json]
                  [--css <path>] [--timings <timings.json>]
```

`--atlas` es opcional; por defecto se escribe `atlas.bin` en el directorio del
IR. `atlas.json` se escribe al lado del atlas (mismo stem). Si el documento no
tiene texto no se escribe ningún atlas, así que las fixtures rect siguen
dejando exactamente los mismos artefactos que antes.

`run.ps1` resuelve el ejecutable respecto a sí mismo y las rutas relativas
respecto al directorio actual (igual que el `run.ps1` de Python). Sale con 0 en
éxito y distinto de 0 ante cualquier error (HTML/CSS ilegible, hoja de estilos
no encontrada, fuente no disponible, atlas desbordado, argumentos inválidos).

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
  --reference-atlas ..\..\..\experiments\16-text-atlas-c\atlas.bin `
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
experiments/16-text-atlas-c/display_list.json 2030 bytes
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

- Binario: `target/release/frontend-rust.exe` = **1.683.968 bytes** (~1,61 MiB),
  release con LTO, `codegen-units=1`, `panic=abort` y `strip` (ver
  `BUILD_INFO.json`). Antes del texto eran 887.296 bytes; el incremento
  (~796 KB) es `rustybuzz` + `ttf-parser` + `ab_glyph` + `ab_glyph_rasterizer`
  y el emisor `--emit-ui`.
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
- **Unidades**: solo `px` y números sin unidad. `%`, `em`, `rem`, `vw`,
  `calc()`, `auto`, `none`, `normal` → `auto` (None).
- **Propiedades de layout**: sin `border`, sin `box-sizing: content-box`, sin
  margin collapsing, floats, grid, `position`, `overflow`, `flex-wrap`,
  `flex-grow/shrink`, `order`, `align-self`, `min/max-width/height`.
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
2. **Runtime (C)**: el patrón de `experiments/16-text-atlas-c/runtime_text.c`
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
   build-time pesa 1,61 MiB (frente a 887 KB sin texto). No se distribuye al
   runtime: solo el IR y el atlas.
4. **R2 (layout dinámico)**: no usa `display_list.json`, porque recalcula
   layout en runtime. Usa `--emit-ui` (árbol pre-layout + atlas 256²) →
   `emit_ui.py --ui-json` → `ui_data.h`. Es el camino por defecto de
   `src/runtime/c-r2/build.ps1`; `-UsePythonTree` queda como oráculo/fallback.

Con esto el pipeline AOT puede generar texto sin Python, sin FreeType/HarfBuzz
y sin ficheros de fuente, con paridad de IR medida contra exp16 y contra el
frontend Python actual.
