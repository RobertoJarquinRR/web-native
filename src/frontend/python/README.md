# exp09-v1-core — v1: bloque + flex + texto → display list → render nativo

Paquete Python que integra el núcleo de **exp05-v0-core**, el texto de
**exp06-text** (HarfBuzz shaping + atlas FreeType + pipeline de glyphs en wgpu) y
el flexbox de **exp07-flex** (fila/columna, gap, justify-content, align-items)
en una sola librería con etapas separadas y un IR lenguaje-agnóstico.

**Sin navegador, WebView, Skia ni Electron.** Parseo con html5lib/tinycss2/
cssselect2, layout propio, rasterizado con wgpu-native o Pillow.

## Estado: SUCCESS

- `verify.py`: **35/35 checks**, `verify/report.json` con `status: SUCCESS`.
- `pytest`: **14 passed** en ~7.7 s.
- Contrato v0 (los dos divs de exp05) conservado: IR de 4 rects y **PNG
  byte-idéntico CPU vs wgpu** (`sha256 3d9256f6…`).
- Demo: un `text_run`, 5 rects, PNG 400×240, chips con color exacto y título
  visible en ambos backends.

Salidas principales: `render.png` (wgpu), `render-cpu.png`, `render-auto.png`,
`display_list.json` y `verify/report.json`.

## Versiones e instalación

| Componente | Versión |
|---|---|
| Python | 3.12.10 (`py -3.12`) |
| html5lib | 1.1 |
| tinycss2 | 1.5.1 |
| cssselect2 | 0.10.1 |
| wgpu (wgpu-py + wgpu-native) | 0.33.0 |
| Pillow | 13.0.0 |
| uharfbuzz | 0.57.0 |
| freetype-py | 2.5.1 |
| pytest | 9.2.0 |

```powershell
cd .\exp09-v1-core
py -3.12 -m venv .venv
.venv\Scripts\python.exe -m pip install --upgrade pip
.venv\Scripts\python.exe -m pip install -r requirements.txt
```

Requiere Windows para las fuentes del sistema (`C:\Windows\Fonts\segoeui.ttf`
o `arial.ttf`); el resto de la arquitectura es portable. No hace falta instalar
el paquete: se ejecuta desde la raíz del experimento.

## Cómo ejecutar

Comando de aceptación (viewport por defecto 400×240):

```powershell
.venv\Scripts\python.exe -m webnative demo/input.html --out render.png --ir display_list.json --backend wgpu
```

Otros backends y la regresión v0:

```powershell
# CPU (Pillow; texto aproximado)
.venv\Scripts\python.exe -m webnative demo/input.html --out render-cpu.png --ir display_list-cpu.json --backend cpu

# auto: elige wgpu y cae a cpu si no hay adapter; imprime el backend
.venv\Scripts\python.exe -m webnative demo/input.html --out render-auto.png --ir display_list-auto.json --backend auto

# contrato v0 de exp05 (2 divs, 400x300, sin texto)
.venv\Scripts\python.exe -m webnative input.html --out old-wgpu.png --ir old.json --backend wgpu --width 400 --height 300
```

Opciones: `--width`/`--height` (default 400×240), `--out` (default
`render.png`), `--ir` (default `display_list.json`), `--backend`
`auto|wgpu|cpu` (default `auto`).

Tests y verificación completa:

```powershell
.venv\Scripts\python.exe -m pytest tests -q
.venv\Scripts\python.exe verify.py          # escribe verify/report.json
```

`verify.py` ejecuta los CLI reales, valida geometría/IR/píxeles/paridad y mide
tiempos. Sale con código 0 sólo si pasan los 35 checks.

## Arquitectura

```
demo/input.html ──html5lib──▶ tree.Node
demo/input.css  ──tinycss2──▶ css_parse.Rule ──cssselect2──▶ cascade.compute_styles()
                                                                    │
                                                                    ▼
                                                     dict[Node, ComputedStyle]
                                                      (display block/flex, gap,
                                                       tipografía, color, caja)
                                                                    │
         fonts.resolve_font_file() ─────────────▶ webnative.text.TextEngine
         "Segoe UI"/"Arial" → TTF                  FreeType: ascender/descender
                                                                    │
                                                                    ▼
                                              layout.layout_tree()
                                    ┌───────────────┼────────────────┐
                                    │               │                │
                              bloque apilado   flex row/column   line box de texto
                                    └───────────────┼────────────────┘
                                                    ▼
                                        LayoutBox (geometría + TextRun)
                                                    │
                       to_display_list(): HarfBuzz shape + FreeType atlas
                                                    ▼
                                  DisplayList [RectOp, TextOp(text_run)]
                                                    │
                                    to_json() ──────▶ display_list.json
                                                    │
                                     renderer.create_renderer()
                          ┌─────────────────────────┴─────────────────────────┐
                          ▼                                                   ▼
             wgpu_renderer (offscreen)                            cpu_renderer (referencia)
        rect pipeline + glyph pipeline alpha                 ImageDraw rects +
        atlas r8unorm 512², readback RGBA                    ImageFont (mismo TTF)
                          └─────────────────────────┬─────────────────────────┘
                                                    ▼
                                                 PNG
```

Las capas puras no importan PIL, wgpu, FreeType ni HarfBuzz. `webnative.text`
es la única capa nativa de texto y sólo se importa si el documento tiene texto;
los renderers son plugins bajo `webnative.renderer`.

## Mapa de módulos

| Archivo | Responsabilidad | Líneas |
|---|---|---:|
| `webnative/tree.py` | `Node` mínimo: tag, attrs, children, `text` | 26 |
| `webnative/html_parse.py` | html5lib → `Node`; `<link>`/`<style>` en orden | 44 |
| `webnative/css_parse.py` | tinycss2 → `Rule`; shorthands margin/padding; selector compilado | 59 |
| `webnative/cascade.py` | matcher cssselect2 → `ComputedStyle`; herencia, `!important`, `style=""`, props flex | 131 |
| `webnative/fonts.py` | `font-family` CSS → TTF del sistema; fallback determinista | 71 |
| `webnative/layout.py` | layout bloque + flex row/column + line boxes; `LayoutBox`; pintado a ops | 271 |
| `webnative/text.py` | HarfBuzz shaping, FreeType raster/métricas, atlas shelf, `TextRun`→`TextOp` | 258 |
| `webnative/ir.py` | `Op`, `RectOp`, `Glyph`, `TextOp` (`text_run`), registro y JSON | 151 |
| `webnative/renderer/__init__.py` | protocolo `Renderer`, `create_renderer`, colores | 54 |
| `webnative/renderer/wgpu_renderer.py` | pipelines rect + glyph, atlas, batches, readback | 258 |
| `webnative/renderer/cpu_renderer.py` | rects exactos + texto aproximado con Pillow | 53 |
| `webnative/cli.py` | orquestación, timings `perf_counter`, fallback auto | 156 |
| `webnative/__init__.py` | API pública, `__version__ = 1.0.0` | 23 |
| `webnative/__main__.py` | entrada `python -m webnative` | 3 |
| **Total** | | **1297** |

## IR: `rect` y `text_run`

`DisplayList.to_dict()` mantiene la forma de v0 (`viewport` + `ops`) para no
romper el contrato anterior. El fixture produce 6 ops en orden painter's:
body, root, título, chips.

```json
{
  "viewport": {"width": 400, "height": 240},
  "ops": [
    {"type": "rect", "x": 0, "y": 0, "w": 400, "h": 240, "fill": "#0f0f12"},
    {"type": "rect", "x": 0, "y": 0, "w": 400, "h": 240, "fill": "#101014"},
    {
      "type": "text_run",
      "x": 20.0, "y": 51.0,
      "text": "Hola",
      "font": "Segoe UI",
      "font_file": "C:\\Windows\\Fonts\\segoeui.ttf",
      "size": 28.0,
      "color": "#ffffff",
      "glyphs": [
        {"gid": 43, "x": 22.0, "y": 31.0, "w": 16, "h": 20,
         "u0": 0.001953, "v0": 0.001953, "u1": 0.033203, "v1": 0.041016,
         "adv": 19.875}
      ]
    },
    {"type": "rect", "x": 20, "y": 71, "w": 60, "h": 60, "fill": "#e74c3c"},
    {"type": "rect", "x": 88, "y": 71, "w": 60, "h": 60, "fill": "#2ecc71"},
    {"type": "rect", "x": 156, "y": 71, "w": 60, "h": 60, "fill": "#3498db"}
  ]
}
```

- `text_run.x/y` es el origen y la **baseline** de la run, no la esquina superior.
- Cada glyph lleva su quad absoluto (`x,y,w,h`), su `gid`, el avance HarfBuzz
  (`adv`) y UVs del atlas canónico como pista. El renderer reconstruye el atlas
  a partir de `(font_file, gid, size)` y recalcula sus propias UVs; el JSON es
  autocontenido mientras el TTF esté disponible.
- Se añaden `text` y `font_file` al formato de exp06: `text` permite el fallback
  aproximado de CPU sin invertir el cmap, y `font_file` fija la fuente exacta
  usada en el build. Son campos adicionales, no rompen el formato.
- Los rects serializan coordenadas enteras (`round`); el texto conserva 3
  decimales en posiciones y 6 en UVs para que `from_json(to_json(x))` sea
  exacto y reproducible.

## Layout v1

**Bloque** (border-box, sin colapso de márgenes): ancho explícito o el
disponible menos márgenes; alto explícito o contenido + padding; hijos apilados
en el content box. El texto propio del nodo se pinta antes que sus hijos.

**Line box de texto**: `baseline = content_y + ascender(font_size)`;
`height = ascender + descender`. Con Segoe UI a 28 px, FreeType da
`ascender=31`, `descender=8`, por eso el título mide 39 px y su baseline está
en `20+31=51`.

**Flex** (`display:flex`):

1. Se mide cada hijo en `(0,0)` con el ancho disponible (así los tamaños
   automáticos, como una línea de texto, salen del layout real).
2. `used = Σ main + gap·(n−1)`; `free = max(contenido − used, 0)` (no hay
   shrink; el desbordamiento se recorta, no se encoge).
3. `justify-content`: `flex-start`, `center` (`start=free/2`),
   `space-between` (`spacing=gap+free/(n−1)`), `flex-end` (`start=free`).
4. `align-items`: `stretch` (default; estira el eje cruzado si el hijo es auto),
   `flex-start`, `center`, `flex-end`.
5. El subárbol medido se traslada a su posición final; el contenedor devuelve
   el tamaño consumido en el eje principal.

El demo ejercita `column` (card) y `row` (fila de chips); los tests cubren
`center`, `space-between`, `flex-end`, `column` y `align-items:center`.

### Geometría verificada del demo

| Elemento | Rect (x,y,w,h) | Detalle |
|---|---|---|
| body | (0, 0, 400, 240) | `#0f0f12` |
| `#root` | (0, 0, 400, 240) | border-box, padding 20, `#101014` |
| `.title` | (20, 20, 360, 39) | baseline 51 = 20 + ascender(28) |
| `.row` | (20, 71, 360, 60) | 71 = 20 + 39 + gap 12 |
| `.chip.a` | (20, 71, 60, 60) | `#e74c3c` |
| `.chip.b` | (88, 71, 60, 60) | 20 + 60 + 8 |
| `.chip.c` | (156, 71, 60, 60) | 88 + 60 + 8 |

## Resolución de fuentes

| `font-family` CSS | Archivo |
|---|---|
| `"Segoe UI"` (y `sans-serif`, `system-ui`, default) | `C:\Windows\Fonts\segoeui.ttf` |
| `Arial`, `Helvetica`, `Helvetica Neue` | `C:\Windows\Fonts\arial.ttf` |
| `Tahoma`, `Verdana`, `Calibri`, `Times New Roman`, `Georgia`, `Consolas`, `Courier New` | TTF correspondiente |
| desconocida | Segoe UI; si falta, Arial; si falta, error explícito |

La resolución vive en `webnative/fonts.py` y la usan layout (métricas vía
`TextEngine`), el builder del IR y el renderer CPU.

## Pipeline de texto

1. **Métricas** (layout): FreeType `set_pixel_sizes(0, round(size))`;
   ascender/descender de la `FT_Size` (26.6 → px).
2. **Shaping**: HarfBuzz con `font.scale = size·64`, dirección `ltr`, script
   `Latn`; se guardan `gid`, `cluster`, `x_advance`, `x_offset`, `y_offset`.
3. **Raster**: FreeType `FT_LOAD_RENDER` grayscale al tamaño entero redondeado.
4. **Atlas**: shelf packing en 512×512, 1 px de guarda entre glyphs, formato
   `r8unorm`; overflow lanza error explícito.
5. **IR**: quad absoluto de cada glyph (`pen + dx + left`, `baseline − dy − top`)
   y UVs de 6 decimales.
6. **wgpu**: reconstruye el atlas desde los `TextOp`, sube `r8unorm`, muestrea
   con filtro linear y mezcla alfa `src_alpha / one_minus_src_alpha`.
7. **CPU**: vuelve a dibujar la cadena con `PIL.ImageFont.truetype` en la misma
   baseline (`anchor="ls"`), sin usar los quads de HarfBuzz.

## Resultados de aceptación

| Check | Resultado |
|---|---|
| CLI `--backend wgpu`: 1 `text_run` + 5 rects, PNG 400×240 | PASS |
| Geometría root/baseline/fila/chips | PASS (valores de la tabla) |
| Chips con color exacto en wgpu y CPU | PASS: `(231,76,60)`, `(46,204,113)`, `(52,152,219)` |
| Título con >100 píxeles brillantes | PASS: 360 px (wgpu), 357 px (cpu) |
| Fondo `#101014` | PASS en ambos |
| `--backend cpu` renderiza y valida rects | PASS |
| Contrato v0: IR de 4 rects sin texto | PASS |
| Contrato v0: CPU vs wgpu byte-idéntico | PASS: mismo `sha256` de PNG y píxeles |
| `--backend auto` imprime el backend | PASS: `backend = wgpu (Microsoft Basic Render Driver / D3D12 / CPU)` |
| `pytest` | PASS: 14 tests |
| `verify.py` | PASS: 35/35 checks |

`verify/report.json` contiene todos los checks, geometría, muestras de píxel,
paridad, versiones y tiempos.

## Paridad CPU vs wgpu (honesta)

- **Rects**: misma geometría entera y mismo color; en el fixture v0 sin texto
  los PNG son **byte-idénticos** (`sha256 3d9256f6…`).
- **Demo**: fuera de la banda del texto (y = 10..70) ambos backends son
  **píxel-idénticos**. En la banda del título:

| Métrica | Valor |
|---|---:|
| Diferencia media por canal (0–255) | **0.168** |
| Diferencia máxima por canal | 49 |
| Píxeles distintos | 295 / 24 000 (1.23 %) |
| IoU de la máscara de píxeles claros | **0.975** |

La CPU no usa el shaping de HarfBuzz ni el atlas: re-maqueta la cadena con
Pillow. Para este texto latino el resultado es casi idéntico, pero **no se
garantiza paridad byte a byte en texto**; wgpu es la referencia exacta del IR.
Las diferencias se concentran en bordes antialias y en el posicionamiento
subpíxel.

## Tiempos medidos (`time.perf_counter`)

Una ejecución en Windows, adapter **Microsoft Basic Render Driver / D3D12 / CPU
(WARP)**; no es una GPU hardware.

| Etapa | ms |
|---|---:|
| html (html5lib + hojas) | 16.53 |
| css (tinycss2 + selectores) | 2.52 |
| cascade (cssselect2) | 0.72 |
| fuentes (cara ya importada) | 0.02 |
| layout (bloque + flex + texto) | 1.75 |
| text + IR (HarfBuzz + atlas + JSON en memoria) | 2.06 |
| **pipeline puro total (módulos calientes)** | **23.86** |
| CLI `--backend cpu`, proceso frío, pared | 583 |
| CLI `--backend wgpu`, proceso frío, pared | 3 640 |
| CLI `--backend auto`, proceso frío, pared | 3 223 |
| primer frame wgpu (pipelines ya creados) | 102 |
| segundo frame wgpu (atlas/pipelines cacheados) | 3.6 |
| frame CPU | 1.3 |
| guardar PNG (primera vez) | 12–29 |

Los costes grandes son de una sola vez por proceso:

- `import wgpu` + adapter/device/pipelines: ~1.75 s (WARP; el CLI lo mide como
  `backend`).
- compilación perezosa del shader/primer draw: ~0.8 s.
- `import webnative.text` (freetype-py + uharfbuzz): ~130–145 ms; después, la
  cara FreeType y las métricas cuestan ~2 ms. En `verify.py`, que importa el
  módulo al arrancar, `build_fonts` mide 0.02 ms y el coste real aparece en
  `layout` (~1.75 ms).
- `WgpuRenderer` cachea device, pipelines, sampler, atlas y bind group: el
  frame 2 baja a ~3.6 ms.

wgpu-native imprime en stderr `Returned GL context is 1.1, when 3.3+ is
needed` al sondear GL antes de elegir D3D12. Es informativo: el proceso termina
con código 0 y el backend elegido es D3D12.

## Decisiones de diseño

- **`LayoutBox` + `TextRun`**: layout no depende de HarfBuzz ni de FreeType;
  recibe `metrics(family, size)` inyectadas. `estimate_metrics` (0.8/0.2 em)
  permite usar layout en tests sin fuentes; el CLI inyecta `TextEngine.metrics`.
- **`TextOp` con posiciones de glyph**: el layout no se recalcula en el
  renderer y el IR describe exactamente qué pintar. El atlas es un recurso del
  renderer, no del IR; las UVs serializadas son una pista reproducible.
- **`text` y `font_file` en la op**: `text` habilita el fallback CPU y la
  depuración; `font_file` fija la fuente del build. Si el archivo no existe, se
  resuelve `font` y se documenta que las métricas pueden cambiar.
- **Batches por tipo en wgpu**: se preserva el painter's order del IR aunque
  haya pipelines distintos (rects y glyphs) sin crear un draw call por op.
- **Dos caminos de texto independientes**: GPU usa HarfBuzz+FreeType; CPU usa
  Pillow. Eso hace la diferencia de paridad medible y honesta, en lugar de
  esconderla reutilizando el mismo atlas.

## Limitaciones explícitas de v1

- **Texto**: una línea por nodo; sin wrapping, sin múltiples line boxes, sin
  runs inline ni tails (`<div>Hola <b>mundo</b></div>` sólo pinta `Hola`).
  Dirección fija `ltr`/`Latn`; sin bidi, RTL, árabe, devanagari ni fuentes
  complejas. Sin fallback por glyph (un glyph ausente se rasteriza como
  `.notdef`). `font-weight`, `font-style`, `letter-spacing`, `text-align`,
  `line-height` y `white-space` se ignoran: siempre TTF regular, alineado a la
  izquierda y alto de línea = ascender + descender. Las métricas y el raster
  redondean el tamaño a píxel entero; HarfBuzz avanza con el tamaño exacto.
- **Flex**: sin `flex-grow`, `flex-shrink`, `flex-basis`, `wrap`, `order`,
  `align-self`, `align-content` ni `margin:auto`. `space-around`/`space-evenly`
  caen a `flex-start`. Los márgenes de hijos flex se ignoran. Si falta espacio,
  se recorta en vez de encoger.
- **Layout**: sin colapso de márgenes, sin `%`, `rem`, `em`, `vh/vw` (se tratan
  como auto), sin `min/max-width`, sin `position`, `overflow`, `border`,
  `border-radius`, sombras, gradientes, imágenes, `opacity` ni clip. Fondo =
  color sólido; alfa entre 0 y 1 se ignora (0 = sin fondo). El layout y el
  pintado parten de `<body>`; el fondo de `<html>` no se propaga.
- **Cascada**: pseudo-elementos y at-rules se ignoran; pseudo-clases se evalúan
  estáticamente (`:hover` nunca coincide); sin hojas remotas ni `@import`.
- **IR/atlas**: atlas fijo de 512×512; muchas fuentes/tamaños pueden desbordarlo
  (error explícito, no corrupción). `font_file` es una ruta absoluta de Windows.
- **GPU**: sólo offscreen/readback; no hay ventana ni present. En esta máquina
  el adapter es WARP (CPU); en hardware real los tiempos bajarán.
- **CPU**: el texto es una aproximación (ver tabla de paridad), no la
  referencia exacta.

## Estructura del experimento

```
exp09-v1-core/
├── demo/
│   ├── input.html, input.css     # fixture v1 (card flex column + chips row)
├── input.html, input.css         # fixture v0 de exp05 (regresión 2 divs)
├── render.png                    # wgpu, 400x240
├── render-cpu.png                # cpu, 400x240
├── render-auto.png               # auto (wgpu)
├── display_list.json             # IR del demo (5 rect + 1 text_run)
├── display_list-cpu.json
├── display_list-auto.json
├── verify/
│   ├── report.json               # 35 checks + métricas + timings
│   └── old-wgpu.png, old-cpu.png # regresión v0 byte-idéntica
├── verify.py
├── requirements.txt
├── README.md
├── webnative/
│   ├── __init__.py  __main__.py
│   ├── tree.py  html_parse.py  css_parse.py  cascade.py
│   ├── fonts.py  layout.py  text.py  ir.py  cli.py
│   └── renderer/
│       ├── __init__.py  wgpu_renderer.py  cpu_renderer.py
└── tests/
    └── test_v1.py
```

Reproducción rápida:

```powershell
.venv\Scripts\python.exe -m pytest tests -q
.venv\Scripts\python.exe verify.py
# STATUS: SUCCESS (35/35 checks)
```
