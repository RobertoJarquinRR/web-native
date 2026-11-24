# exp11 — Página real mínima, render nativo y GAPS honestos

**STATUS: SUCCESS del subconjunto implementado.** `render.png` se produce con **wgpu/D3D12**,
la geometría y los colores son exactos y las **88/88 comprobaciones** pasan. No es un navegador:
`GAPS.md` documenta, con evidencia ejecutable, todo lo que falta. La página se ve bien porque
el fixture evita o simula esos huecos a propósito.

Sin navegador, WebView, Skia ni Electron. Solo Python + html5lib + tinycss2/cssselect2 +
HarfBuzz + FreeType + wgpu.

## Cómo ejecutar

```powershell
py -3.12 -m venv .venv
.venv\Scripts\python.exe -m pip install -r requirements.txt
.venv\Scripts\python.exe run.py
```

Salidas en esta carpeta:

| Archivo | Contenido |
|---|---|
| `render.png` | Render final 640×480 (RGBA) |
| `display_list.json` | IR: 9 rects + 16 text runs con quads, UVs y colores |
| `layout.json` | Árbol de cajas con rects, líneas, baselines, flex info |
| `report.json` | Checks, tiempos, backend, stats, versiones |
| `gaps_evidence.json` | Números de las 14 sondas de `probes.py` |

Para reproducir las sondas de gaps:

```powershell
.venv\Scripts\python.exe probes.py
```

El proceso termina con código 0 solo si no hay checks fallidos. Un fallo de wgpu degrada a
`cpu-pillow` y el estado sería **PARTIAL**, no un crash.

## Qué se renderizó

Fixture `page/input.html` + `page/input.css`, viewport 640×480, solo fuentes del sistema:

- **Nav** 0..56, `display:flex` row, `space-between`, padding 16, logo “webnative” 18 px bold
  y links Docs/Demo/About 14 px `#9aa0aa`. Links alineados a la derecha: borde 624.
- **Hero** y=56, alto 130.5, padding 32. H1 “Compila la web” 28 px bold blanco; párrafo 16 px
  `#9aa0aa` en una línea (256.2 px de 576 disponibles).
- **Cards** y=202.5, tres cards de 180×120 en x=16, 212 y 408, gap 16. Cada card es flex column
  con padding 12, gap 8, fondo `#1a1a22`, badge de 4 px (`#e74c3c`, `#2ecc71`, `#3498db`),
  título 16 px bold y cuerpo 13 px que **envuelve a 2 líneas** en las tres cards.
- **Footer** y=448..480, alto 32, fondo `#0b0b0e`, texto 12 px `#6b7280` centrado con flex.

Geometría exacta (no tolerada): nav 56; cards 180×120 en 16/212/408 con gap 16; footer
bottom 480 y alto 32; contenido de card 156×96 con 75.9 usados y 20.1 libres.

Texto: **16 runs**, **231 quads** de glifo (204 con área), **111 bitmaps** únicos en un atlas
R8 de 1024×1024 (1 MiB), 0 overflow. Dos caras reales: `segoeui.ttf` y `segoeuib.ttf`.

## Pipeline

```
page/input.html + page/input.css
   │ html5lib (DOM con #text) · tinycss2 · cssselect2 (cascada + herencia)
   ▼
layout de bloque + flex row/column + líneas con wrapping
   │ border-box forzado · max-content para ítems flex · margin-top:auto
   ▼
HarfBuzz (shaping LTR/Latn) → FreeType (raster grayscale) → atlas R8 1024²
   │ 16 runs, 231 quads con UVs
   ▼
display_list.json (rect + text_run)
   ▼
wgpu offscreen 640×480 · pipeline de rects + pipeline de glifos · readback 256B
   ▼
render.png + verificación (geometría, IR, 11 muestras de color, presencia de texto)
```

## Verificación (resumen real)

`report.json`: **88 checks, 88 pass, 0 fail**.

- `render.png` mide 640×480.
- Fondo de página, nav, cards, footer y los 3 badges: muestras exactas (`delta = 0`).
- Texto presente por región: logo 514 px claros; links 288 px muted; h1 1725 px claros;
  hero p 697 px muted; títulos 398/380/580; cuerpos 423/413/398; footer 692.
- Wrap: los 3 cuerpos de card tienen 2 líneas cada uno.
- IR: 25 ops, 16 text runs, todos con glifos; 204 quads dibujables; atlas sin overflow.
- Probe de robustez: 14 features no soportadas (`grid`, `position:absolute`, `border-radius`,
  `box-shadow`, `transform`, `overflow`, `width:50%`, `min-height`, `flex`, `border`,
  `box-sizing`, `padding:10%`, `gap:1em`, `grid-template-columns`) se ignoran sin excepción.

## Tiempos (una corrida en frío, WARP; varían ±10% entre corridas)

| Etapa | ms (aprox.) |
|---|---:|
| parse HTML | 17 |
| parse CSS | 7 |
| cascada | 3 |
| init fuentes | 3 |
| **layout** | **6** |
| shaping + atlas + IR | 21 |
| **init wgpu (una vez)** | **2130** |
| **render + readback + PNG** | **688** |
| verificación + probe | 311 |
| **total del proceso** | **3194** |

El adaptador es **Microsoft Basic Render Driver** (WARP, CPU, D3D12, vendor 5140): no hay GPU
de hardware enumerable. El coste dominante es el arranque de wgpu-native (~2.0 s) y el
rasterizado por software (~0.69 s por PNG). Layout, shaping y atlas juntos son ~27 ms. Estos
números no representan rendimiento en hardware real.

## Las tres brechas de mayor impacto

1. **No hay inline formatting context.** Un contenedor que mezcla texto y bloques pierde el
   texto suelto: `<div>Hola <p>mundo</p> tail</div>` pinta solo “mundo”. Es estructural: hoy
   solo funcionan “hojas de texto” puras. Evidencia: `gaps_evidence.json` → `mixed_inline_block`.
2. **No hay tamaños responsive.** `%`, `em/rem`, `vh/vw`, `min/max` y `calc()` no existen.
   El fixture cablea `body { height:480px }`; a 640×400 el body sigue en 480 y el footer cae
   fuera. No hay `100vh` ni `min-height`, así que el sticky footer no es general. Evidencia:
   `fixed_viewport_height` y `percent_sizes`.
3. **No hay hoja UA ni resolución real de fuentes.** Sin `*{margin:0}` el h1 sale 16 px/400 y
   sin márgenes; `font-family:Arial` sigue cargando `segoeui.ttf`. La página solo es correcta
   porque declara todos los tamaños, pesos y colores y porque pide Segoe UI, que es la única
   familia que el motor carga. Evidencia: `no_ua_stylesheet` y `font_family_ignored`.

Detrás quedan: bordes/radios/sombras/gradientes (cards planas, badges como `<div>`), flex
grow/shrink/wrap, posición/overflow/scroll, bidi/scripts complejos, HiDPI e interactividad.
El detalle completo, con estado y evidencia por feature, está en **`GAPS.md`**.

## Decisiones y limitaciones de esta entrega

- **Border-box forzado.** Todo `width`/`height` incluye padding. El fixture declara
  `box-sizing:border-box`; el motor ignora `content-box` (un navegador daría cards 204×144).
- **Una sola familia efectiva.** Se cargan Segoe UI regular/bold; el primer token de
  `font-family` no se resuelve. `font-weight:600` usa el archivo bold (700).
- **`line-height:normal` desde extents de HarfBuzz.** Asc+desc+gap: 21.28 px a 16 px, 37.25 px
  a 28 px bold. Coincide con el navegador para Segoe UI; el hero mide 130.5 y las cards y=202.5.
  No se leen OS/2 typo metrics ni overrides tipográficos.
- **Footer con flex, no `text-align`.** El centrado horizontal y vertical sale de
  `justify-content:center` + `align-items:center`; `text-align` está implementado pero no se usó.
- **Workarounds del fixture:** reset universal, `height:480px`, `<span>` en el footer, badges
  como divs, sin imágenes/gradientes/radios. Están listados en `GAPS.md` §D.
- **Orden de pintado simplificado:** todos los fondos y después todo el texto. No hay
  solapamientos en esta página.
- **Solo offscreen:** no hay ventana, frame loop, input, scroll ni accesibilidad.

## Estructura

```
exp11-real-page/
  run.py                     orquesta parse → cascada → layout → IR → render → verify
  probes.py                  sondas de gaps → gaps_evidence.json
  page/input.html            fixture de la página
  page/input.css             estilos explícitos (sin hoja UA)
  webnative/
    tree.py                  DOM con nodos #text y tails
    html_parse.py            html5lib → Node; text_content(); stylesheet_texts()
    css_parse.py             tinycss2 + cssselect2; expande margin/padding/gap
    cascade.py               ComputedStyle, herencia, !important, style=""
    text.py                  FontBook (HarfBuzz/FreeType), atlas R8 shelf
    layout.py                bloque + flex row/column + wrapping + alineación
    ir.py                    RectOp/TextOp, quads con UVs, DisplayList JSON
    verify.py                checks de geometría, IR, píxeles y texto
    renderer/wgpu_renderer.py  backend principal (2 pipelines)
    renderer/cpu_renderer.py   fallback Pillow
  display_list.json          IR serializado
  layout.json                evidencia de cajas y líneas
  report.json                resultado de la corrida
  gaps_evidence.json         evidencia de GAPS.md
  render.png                 render 640×480
  GAPS.md                    tabla honesta de cobertura
  README.md                  este documento
```

## Siguiente paso recomendado

Antes de añadir features visuales (radios, sombras), atacar las tres brechas estructurales:

1. **Inline formatting context** con line boxes que mezclen `#text` e inline boxes, y
   anónimos para texto suelto en flex.
2. **Sizing real**: `box-sizing` configurable, porcentajes contra el containing block,
   `min/max`, `min-height`, y un containing block de viewport para `100vh`.
3. **Fuentes**: resolver la lista `font-family`, cargar varias familias y hacer fallback por
   glifo; después, hoja UA mínima (h1/p/márgenes/pesos) para no depender del reset del autor.

Con eso, la misma página seguiría viéndose igual y además sobreviviría a un viewport distinto,
a contenido mixto y a una fuente distinta de Segoe UI.
