# GAPS — exp11-real-page

**Veredicto corto:** la página se renderiza y las 88 comprobaciones pasan, pero eso demuestra
cobertura del **subconjunto implementado**, no paridad con un navegador. El fixture está
escrito para evitar, con workarounds explícitos, varios huecos del motor. Aquí se separa lo
que el motor hace de lo que la página disimula.

- Render: `render.png` 640×480, backend wgpu/D3D12 (WARP), 25 ops (9 rects + 16 text runs),
  231 quads de glifo (204 con área), 111 bitmaps en atlas 1024×1024, 0 overflow.
- Checks: **88/88 OK**. Geometría exacta: nav 0..56, cards x=16/212/408, footer y=448..480.
- Evidencia de sondas: `probes.py` → `gaps_evidence.json` (14 casos ejecutados).
- Todas las afirmaciones de “falla” de abajo tienen un número o una ruta de código.

## A. Features que esta página usa

| # | Feature | Estado | Evidencia (qué pasó realmente) | Impacto |
|---:|---|---|---|---|
| 1 | Parseo HTML con html/head/body implícitos | implemented | html5lib; 31 elementos, 18 reglas CSS, nodos `#text` y tails conservados | — |
| 2 | Selectores CSS: `*`, tag, clase, descendiente (`.hero h1`, `.links a`) | implemented | cssselect2 compila y casa; 18 reglas aplicadas | — |
| 3 | Cascada: especificidad, orden, `!important`, `style=""`, herencia | implemented | ruta de exp05 reutilizada; herencia de color/fuente verificada (muted `#9aa0aa` llega a los hijos) | — |
| 4 | Hoja de estilos de usuario (UA) | **missing** | Sin `*{margin:0}` el probe da `h1 = 16px / 400 / margin 0`; un navegador daría 32px/700 y márgenes. El fixture compensa con reset y valores explícitos | **high** |
| 5 | `display:block` anidado | implemented | `main > nav/hero/cards`; hero 640×130.5, cards 640×152 | — |
| 6 | `display:flex` fila | implemented | nav, `.links`, `.cards`, footer; reparto y alineación verificados | — |
| 7 | `display:flex` columna | implemented | body y `.card`; card content 156×96, usado 75.9, libre 20.1 | — |
| 8 | `justify-content:space-between` | implemented | logo x=16, links x=486.03, borde derecho 624.0 (padding 16) | — |
| 9 | `justify-content:center` | implemented | footer span x=211.29, w=217.42, centrado en 640 | — |
| 10 | `align-items:center` | implemented | logo y=16.03 en content 16..40; links y=18.69 | — |
| 11 | `gap:16px` fila y `gap:8px` columna | implemented | cards exactamente en x=16/212/408 (180+16); hijos de card separados 8 | — |
| 12 | `width`/`height` fijos en px | implemented | nav 56, cards 180×120, footer 32, badges 4 | — |
| 13 | `box-sizing` | **workaround** | El motor **fuerza border-box**. `box-sizing:content-box` se parsea y se ignora: el probe mantiene 180×120 donde un navegador daría 204×144. El fixture declara `box-sizing:border-box` para ser honesto | **high** |
| 14 | Ancho automático de ítems flex (max-content) | implemented | logo 87.16, links 137.97, span footer 217.42; calculados con avances HarfBuzz | — |
| 15 | Alto automático por contenido | implemented | hero 130.5 (32+37.25+8+21.28+32); `.cards` 152 (16+120+16) | — |
| 16 | `padding` shorthand (16/32/12) | implemented | h1/p x=32, logo x=16, contenido de card x=28 (card 16+12) | — |
| 17 | `margin` longhand (`margin-bottom:8px`) | implemented | h1 bottom 125.25 → p y=133.25 | — |
| 18 | `margin-top:auto` en flex | implemented | footer y=448 exacto: 480 − 32; el libre 109.5 se consume en el margen auto | — |
| 19 | `margin:auto` vertical en bloque | **missing** | `_layout_block` hace `mt = margin[0] or 0`; un auto vertical se convierte en 0. No se usa en la página | low |
| 20 | Colapso de márgenes | **missing** | Probe: `margin-bottom:20` + `margin-top:30` produce un hueco de **50**, no 30. La página no tiene márgenes verticales adyacentes | medium |
| 21 | Footer pegado al fondo | **workaround** | `body{display:flex;flex-direction:column;height:480px}` + `margin-top:auto`. **No** hay `position:fixed`, `min-height`, `100vh` ni porcentajes: a viewport 640×400 el body sigue midiendo 480 y el footer cae en 448..480, fuera de la ventana (probe) | **high** |
| 22 | `background-color` sólido | implemented | muestras exactas: página `#0f0f14`, nav `#12121a`, card `#1a1a22`, footer `#0b0b0e`, badges `#e74c3c/#2ecc71/#3498db` | — |
| 23 | `color` y herencia | implemented | muted heredado a `.links a`, `.hero p`, `.card-body`; footer `#6b7280` | — |
| 24 | `font-family` de sistema | **workaround** | `FontBook` **siempre carga Segoe UI**; no lee `font-family`. Probe: CSS `Arial` sigue usando `segoeui.ttf`. La página pide “Segoe UI”, así que coincide por casualidad, no por resolución | **high** |
| 25 | `font-size` en px | implemented | 18/14/28/16/13/12; sin em/rem/%/vw | — |
| 26 | `font-weight` | **workaround** | Solo 400 y 700. El logo y los títulos piden 600 y usan `segoeuib.ttf` (700): quedan más gruesos que lo pedido. Sin italic, sin pesos variables, sin bold sintético | medium |
| 27 | Runs de texto con HarfBuzz + FreeType + atlas + wgpu | implemented | 16 runs, 231 quads, 111 bitmaps, 0 overflow; Segoe UI regular y bold | — |
| 28 | Word wrapping | implemented | Los 3 cuerpos de card ocupan 2 líneas (145.5/23.8, 121.9/51.0, 118.1/46.4 px). El párrafo del hero (256.2 px) cabe en 576 y no envuelve | — |
| 29 | Centrado del texto del footer | **workaround** | Se logra con `display:flex; justify-content:center`; `text-align` existe en el motor pero no se usó aquí | low |
| 30 | Colapso de espacios y trimming | implemented | `" ".join(text.split())`; sin `white-space:pre` real | — |
| 31 | `line-height:normal` | implemented | Se calcula con las extents de HarfBuzz (asc+desc+line_gap): **21.28 px a 16 px**, **17.30 a 13 px**, **37.25 a 28 px bold**. Hero 130.5 y cards y=202.5. No se leen OS/2 typo metrics ni overrides de `line-height`, pero para Segoe UI coincide con el navegador | — |
| 32 | Dos pesos reales (regular/bold) | implemented | `segoeui.ttf` + `segoeuib.ttf`, ambos upem 2048 | — |
| 33 | Fuentes del sistema | implemented | `C:\Windows\Fonts`; si faltara Segoe UI hay fallback Arial, pero **sin fallback por glifo** | medium |
| 34 | `text-align` left/center/right | implemented | Implementado en `_align_offset`; **no ejercitado** por la página (el footer centra con flex) | — |
| 35 | PNG offscreen + verificación | implemented | 640×480, 30 KB; 88 checks: tamaño, geometría, IR, 11 muestras de color, conteos de texto, wrap, atlas y probe de robustez | — |
| 36 | Fallback CPU | implemented | `render_pillow` compone rects y máscaras del atlas; no se usó porque wgpu/D3D12 funcionó | — |

## B. Lo que un navegador hace y este motor no

| # | Feature | Estado | Evidencia | Impacto |
|---:|---|---|---|---|
| 37 | Inline formatting context (texto mezclado con bloques) | **missing** | Probe `<div>Hola <p>mundo</p> tail</div>` pinta solo `mundo`; “Hola” y “tail” se descartan. `_layout_block` ignora nodos `#text` | **high** |
| 38 | Ítems flex anónimos (texto directo en flex) | **missing** | Probe `<div style="display:flex"><span>a</span>texto suelto</div>` pinta solo `a`. El footer necesita un `<span>` | medium |
| 39 | Porcentajes | **missing** | Probe `height:50%` dentro de 200 px da **23 px** (alto de contenido), no 100. `length()` devuelve `None` | **high** |
| 40 | em / rem / vh / vw / `calc()` / min / max | **missing** | Mismo `length()`: todo lo que no sea px o número se trata como auto; `min-height` y `max-width` no existen | **high** |
| 41 | `flex-grow` / `shrink` / `basis` | **missing** | Probe: contenedor 400, dos hijos de 100 con `flex-grow:1` → siguen en 100; `free=200` no se reparte | medium |
| 42 | `flex-wrap`, `order`, `align-self`, `align-content` | **missing** | No hay código; solo row/column sin wrap | medium |
| 43 | `border`, `border-radius` | **missing** | Probe: `border:4px solid` no ocupa espacio ni se pinta; los badges son `<div>` de 4 px como workaround. Cards con esquinas rectas | medium |
| 44 | `box-shadow`, `outline`, gradientes | **missing** | Sin código; el diseño queda plano | low |
| 45 | Imágenes, SVG, icon fonts, `background-image` | **missing** | Sin decodificadores ni ops de imagen; el fixture no las usa a propósito | **high** (general) |
| 46 | `position`, `float`, `z-index`, `overflow`, `clip`, `transform`, `opacity`, `visibility` | **missing** | El probe de robustez enumera 14 features ignoradas sin excepción; no hay stacking contexts ni recorte | **high** (general) |
| 47 | `@media`, `@supports`, `@font-face`, `@import` | **missing** | `parse_stylesheet` solo procesa `qualified-rule`; el resto se descarta | **high** |
| 48 | Variables CSS, `var()`, `calc()` | **missing** | No hay resolución de custom properties | **high** |
| 49 | Pseudo-clases dinámicas (`:hover`, `:focus`) y pseudo-elementos (`::before/::after`) | **missing** | cssselect2 casa las estáticas; las dinámicas nunca casan y los pseudo-elementos se filtran en `parse_stylesheet` | medium |
| 50 | Keywords CSS-wide: `inherit`, `initial`, `unset`, `revert` | **missing** | Probe: `color:inherit` en un hijo de rojo calcula **`#000000`** (`parse_color("inherit") → None → negro`) | medium |
| 51 | Bidi, RTL, scripts complejos | **missing** | `Face.shape` fija `direction="ltr"`, `script="Latn"`; árabe/hebreo/devanagari no se shapean | **high** (i18n) |
| 52 | Fallback de fuente por glifo, italic, fuentes variables, emoji de color | **missing** | Una sola face por peso; glifos ausentes → `.notdef`; sin atlas RGBA | medium |
| 53 | `text-decoration` (subrayado de links) | **missing** | No se pinta; el diseño no lo pide, pero un link por defecto saldría sin subrayar | low |
| 54 | `text-align:justify`, `text-indent`, `letter-spacing`, `word-spacing`, `text-transform` | **missing** | No hay soporte; `justify` cae a left | low |
| 55 | `white-space:pre`, `overflow-wrap`, `word-break`, `text-overflow:ellipsis` | **missing** | `pre` se trata como `nowrap`; las palabras largas se parten **por carácter** (probe), cosa que un navegador no hace por defecto | medium |
| 56 | Scroll y recorte por viewport | **missing** | El render es una textura fija; si el contenido supera 480 px se sale del PNG. No hay barras ni scroll | **high** |
| 57 | devicePixelRatio / HiDPI | **missing** | Render 1× fijo; glifos rasterizados a ppem entero, sin subpíxel RGB | medium |
| 58 | Accesibilidad, semántica, hit-testing, links, IME, selección | **missing** | La salida es un PNG; `<a href>` no es interactivo | **high** (producto) |
| 59 | Orden de pintado y stacking contexts | **workaround** | `build_display_list` pinta **todos** los fondos y después **todo** el texto; no hay z-index ni solapamientos correctos. La página no solapa nada | medium |

Features menores que tampoco existen (no las necesita este fixture):

- **Alpha/transparencia:** `parse_color` descarta el canal alpha; `rgba(255,0,0,0.5)` se pinta
  opaco. `opacity` se ignora. Impacto medium.
- **Filtros y blending:** `filter`, `backdrop-filter`, `mix-blend-mode`, `will-change` no se leen.
  Impacto low/medium.
- **Variantes tipográficas:** `font-variant`, `font-feature-settings`, `font-stretch`, `font-kerning`
  no se leen; HarfBuzz aplica sus defaults. Impacto low.
- **Interacción:** `cursor`, `user-select`, `pointer-events` no aplican porque la salida es un PNG.
  Impacto high para producto, fuera del alcance de este renderer.

## C. Render y runtime

| # | Feature | Estado | Evidencia | Impacto |
|---:|---|---|---|---|
| 60 | wgpu en GPU de hardware | **missing** | Solo se enumeró **Microsoft Basic Render Driver** (WARP, D3D12, tipo CPU, vendor 5140). Los tiempos no representan hardware | medium |
| 61 | Init de wgpu reutilizable | implemented | ~2.0 s de init (una vez); el render siguiente sería más barato, pero no se mide un segundo frame | — |
| 62 | Caché de atlas entre frames | **missing** | Atlas 1024² fijo, shelf packing, sin evicción; 111 bitmaps, 0 overflow. Si se llena, **omite glifos en silencio** (`Atlas.overflow`) | medium |
| 63 | Caché de target/buffers entre frames | **missing** | Cada render crea textura target, buffers y readback; ~0.69 s por PNG en WARP | medium |
| 64 | Anti-aliasing de texto | implemented | Grayscale AA de FreeType; sin subpíxel RGB ni stem darkening | low |
| 65 | Posicionamiento subpíxel de glifos | implemented | Quads con x/y fraccionarios y sampler lineal; las métricas se redondean a ppem entero | low |
| 66 | Rectas a coordenadas enteras | **workaround** | El IR redondea rects (`round`); el texto conserva fracciones. En esta página no se nota | low |
| 67 | Reproducibilidad de fuentes | **missing** | Depende de la versión de Segoe UI instalada en Windows; no se empaqueta la fuente | medium |
| 68 | Frame loop / ventana / vsync | **missing** | Solo render offscreen a PNG; no hay bucle, presentación ni input | **high** (runtime) |

## D. Workarounds que el fixture aplica (no son capacidades del motor)

1. `* { margin:0; padding:0; box-sizing:border-box }` sustituye la hoja UA y hace explícito
   el border-box forzado.
2. `body { height:480px }` está cableado al viewport. No hay `100%`, `100vh`, `min-height`
   ni layout responsive; a 640×400 el footer queda fuera (probe).
3. El texto del footer va dentro de `<span class="footer-text">`; texto directo en un flex
   container se ignora (probe).
4. Los badges son `<div>` de 4 px con `background-color`; no se usan `border-top`,
   `border-radius` ni `box-shadow`.
5. Todos los tamaños, pesos y colores de fuente están declarados explícitamente; sin eso,
   no hay hoja UA ni resolución de `font-family`.
6. Las cards declaran `box-sizing:border-box`; el motor lo ignora y aplica border-box de
   todas formas. Un `content-box` real daría 204×144, no 180×120 (probe).
7. La página no usa imágenes, iconos, gradientes, sombras, bordes ni estados hover.
8. `layout_document` fuerza el body al alto del viewport si no declara `height`; no es el
   comportamiento CSS (auto = alto de contenido). En esta página el `height:480px` explícito
   domina, pero el fallback del root es una decisión del motor, no del estándar.

## E. Evidencia reproducible

`.venv\Scripts\python.exe probes.py` escribe `gaps_evidence.json`. Números clave:

- **Viewport fijo:** viewport 640×400 → body 480, footer 448..480 (fuera).
- **Sin UA:** h1 16px/400/margin 0; p margin 0.
- **font-family:** CSS `Arial` → `C:\Windows\Fonts\segoeui.ttf`.
- **content-box:** `width:180; padding:12; box-sizing:content-box` → 180×120 (navegador 204×144).
- **`color:inherit`:** padre `#ff0000` → hijo `#000000`.
- **Inline+bloque:** `<div>Hola <p>mundo</p> tail</div>` → solo `mundo`.
- **Flex anónimo:** solo `a`, se pierde “texto suelto”.
- **flex-grow:** 400 px, hijos 100+100 con grow 1 → 100+100, free 200.
- **%:** `height:50%` en 200 px → 23 px.
- **Márgenes:** 20 + 30 → hueco 50 (navegador 30).
- **Palabra larga:** 80 px, 16 px → varias líneas partidas por carácter.
- **Peso:** 600 → face `bold` (`segoeuib.ttf`).
- **Métricas:** 16 px → asc 17.27 + desc 4.02 + gap 0 = 21.28 px de línea (extents de HarfBuzz).
- **Border:** `4px solid` → caja 100×40 sin borde.

## F. Qué rompería primero si la página creciera

1. **Contenido inline mezclado con bloques** (texto perdido, no solo desalineado).
2. **Responsive real** (porcentajes/vh/min-height): hoy el footer solo funciona a 480 px.
3. **Flex grow/shrink/wrap** para cards o nav que deban adaptarse.
4. **Bordes/radios/sombras** para que el diseño deje de verse plano.
5. **Resolución de fuentes y fallback por glifo** para salir de Segoe UI/Windows.

## G. Lo que sí queda demostrado

- El pipeline completo HTML+CSS → cascada → layout → IR → wgpu → PNG funciona en una página
  con 4 secciones, 3 cards, 16 runs de texto y dos pesos.
- La geometría del subconjunto es exacta (no aproximada): 56, 16/212/408, 448, 32.
- Los colores son exactos en las 11 muestras (delta 0 con el render wgpu).
- El texto se shapea con HarfBuzz, se rasteriza con FreeType y se compone en GPU con un
  atlas R8; no hay navegador, WebView, Skia ni Electron en el camino.
- Las features desconocidas se ignoran sin crash (probe de 14 declaraciones).
