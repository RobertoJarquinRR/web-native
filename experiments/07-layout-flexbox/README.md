# exp07 — Flexbox mínimo (fila) → display-list IR → render nativo

Experimento end-to-end: HTML + CSS → cascade → **layout flex de una sola fila** →
display list JSON → render offscreen con **wgpu** (D3D12). Sin navegador, WebView,
Skia ni Electron.

**STATUS: SUCCESS** — 4/4 variantes con geometría exacta, IR exacto y 24/24 muestras
de píxel con `delta = 0`. `run.py` tiene 499 líneas.

## Cómo ejecutar

```powershell
py -3.12 -m venv .venv
.venv\Scripts\python.exe -m pip install -r requirements.txt
.venv\Scripts\python.exe run.py
```

Salidas:

- `report.json` — PASS/FAIL, esperado vs obtenido, tiempos, versiones y backend.
- `out/v1..v4/display_list.json` — IR de cada variante (4 rects).
- `out/v1..v4/render.png` — PNG 400×120 verificado de cada variante.

El proceso termina con código 0 solo si las 4 variantes pasan todas sus checks.

## Versiones

| Componente | Versión |
|---|---|
| Python | 3.12.10 |
| html5lib | 1.1 |
| tinycss2 | 1.5.1 |
| wgpu (wgpu-py) | 0.33.0 |
| wgpu-native | incluido en el wheel de wgpu-py 0.33.0 |
| Pillow | 13.0.0 |
| Windows | 10 Pro 19045 |

## Fixtures y geometría esperada

Raíz de 400×120, `padding: 20px`, fondo `#101014`; tres cajas de 80×60 con
`gap: 16px`: `a=#e74c3c`, `b=#2ecc71`, `c=#3498db`.

El content box de la raíz es `400−40 = 360` de ancho y `120−40 = 80` de alto.
Para las tres cajas: `used = 80·3 + 16·2 = 272` y `free = 360−272 = 88`.

| Variante | justify-content | align-items | a (x,y) | b (x,y) | c (x,y) | start | spacing |
|---|---|---|---|---|---:|---:|
| v1 | flex-start | flex-start | (20,20) | (116,20) | (212,20) | 0 | 16 |
| v2 | center | flex-start | (64,20) | (160,20) | (256,20) | 44 | 16 |
| v3 | space-between | flex-start | (20,20) | (160,20) | (300,20) | 0 | 60 |
| v4 | center | center | (64,30) | (160,30) | (256,30) | 44 | 16 |

Todas las cajas conservan `w=80`, `h=60`. En v4 el offset de cross-axis es
`(80−60)/2 = 10`, por eso `y = 20+10 = 30`.

## Algoritmo flex implementado

Funciones puras (no mutan el árbol de nodos):

```python
flex_layout(content_box, child_styles, parent_style)
    -> (list[Rect], FlexInfo)
layout_tree(node, border_rect) -> LayoutBox
```

Pasos:

1. **Content box**: se resta el padding de la raíz:
   `content = Rect(x+padL, y+padT, width−padL−padR, height−padT−padB)`.
2. **Tamaño usado**: `used = Σ child.width + gap·(n−1)`.
   No hay grow ni shrink: los hijos deben tener ancho/alto fijos en px.
3. **Espacio libre**: `free = content.width − used`.
   Si `free < 0` se lanza `LayoutError` en vez de encoger.
4. **Reparto en el eje principal**:
   - `flex-start`: `start = 0`, separación entre cajas `spacing = gap`.
   - `center`: `start = free/2`, `spacing = gap`.
   - `space-between`: `start = 0`, `spacing = gap + free/(n−1)`; el hueco
     original sigue siendo el mínimo y el espacio libre se reparte entre los
     `n−1` intervalos. Para v3: `16 + 88/2 = 60`.
5. **Eje cruzado**:
   - `flex-start`: `cross = content.y`.
   - `center`: `cross = content.y + (content.height − child.height)/2`.
6. Se emite un `Rect` por caja, en orden de pintado `root → a → b → c`, y se
   renderiza el display list en una textura `rgba8unorm`.

La geometría no se redondea silenciosamente: el IR exige coordenadas enteras y
falla si aparece una fracción (las 4 variantes son enteras por construcción).

## Verificación

`fixtures/expected.json` contiene la geometría esperada como dato. Por variante
se comprueban 12 cosas (48 checks totales):

- **1 check de flex**: content box, `used=272`, `free=88`, `start_offset`,
  `spacing` y los offsets de cross-axis, comparados exactamente.
- **3 checks de geometría**: `(x,y,w,h)` exactos de a, b y c.
- **1 check de display list**: los 4 rects del IR con posición y color exactos.
- **1 check de tamaño PNG**: 400×120.
- **6 muestras de píxel**: esquina y banda inferior del fondo, hueco entre a y b,
  y centro de cada caja. Tolerancia ±8; en la corrida real todas dieron
  `delta = 0`.

Salida real (resumida):

```text
[v1] flex-start/flex-start free=88 start=0 spacing=16
  a=(20,20,80,60) b=(116,20,80,60) c=(212,20,80,60)
  geometry 3/3 exact | IR OK | pixels 6/6 | PASS
[v2] center/flex-start free=88 start=44 spacing=16
  a=(64,20,80,60) b=(160,20,80,60) c=(256,20,80,60)
  geometry 3/3 exact | IR OK | pixels 6/6 | PASS
[v3] space-between/flex-start free=88 start=0 spacing=60
  a=(20,20,80,60) b=(160,20,80,60) c=(300,20,80,60)
  geometry 3/3 exact | IR OK | pixels 6/6 | PASS
[v4] center/center free=88 start=44 spacing=16
  a=(64,30,80,60) b=(160,30,80,60) c=(256,30,80,60)
  geometry 3/3 exact | IR OK | pixels 6/6 | PASS
STATUS: SUCCESS (4/4 variants)
```

Pruebas de robustez ejecutadas en una copia temporal (no tocan los entregables):

- **Fallo esperado**: cambiar la `x` esperada de la caja `a` de v1 a 21 produce
  `exit=1`, `status=FAILED`, `v1.pass=false` y el reporte guarda el esperado 21
  frente al obtenido 20. Las otras 3 variantes siguen pasando.
- **Fallback**: forzar `WgpuRenderer` a lanzar `RuntimeError` hace que se use
  Pillow; el resultado es `status=PARTIAL`, `backend=pillow-fallback`,
  `render_error=RuntimeError: forced wgpu failure`, 48/48 checks OK y `exit=0`.

## Backend y tiempos

```
adapter: Microsoft Basic Render Driver
backend: D3D12
type:    CPU (WARP, rasterizador software)
vendor:  5140
```

wgpu emite al inicializar `Returned GL context is 1.1, when 3.3+ is needed`
(3 veces); es ruido inofensivo de la sonda GL, el adaptador elegido es D3D12.

Tiempos de una corrida en frío (4 variantes, sin profiler):

| Etapa | Tiempo |
|---|---:|
| init wgpu (adapter/device/pipeline, una vez) | 1680.12 ms |
| parse HTML (4) | 19.59 ms |
| parse CSS (4) | 4.92 ms |
| cascade (4) | 0.94 ms |
| **layout flex (4)** | **0.18 ms** |
| display list + JSON (4) | 2.56 ms |
| render wgpu + readback + PNG (4) | 625.33 ms |
| verificación (4) | 9.98 ms |
| **total del proceso** | **2347.21 ms** |

El layout en sí cuesta ~0.05 ms por variante. El costo dominante es la
inicialización única de wgpu-native/WARP (~1.6–2.1 s según la corrida) y el
render por software (~150–160 ms por PNG); el adapter/device/pipeline se
reutilizan entre variantes. En una GPU de hardware estos números no son
representativos.

## Estructura y reutilización

```
exp07-flex/
  run.py                  pipeline completo (499 líneas)
  requirements.txt
  fixtures/
    v1.html ... v4.html   HTML de cada variante
    v1.css  ... v4.css    CSS de cada variante
    expected.json         geometría y colores esperados (datos)
  out/v1..v4/
    display_list.json
    render.png
  report.json
  README.md
```

Tipos y funciones pensados para integrarse en v0:

- `Style`, `Edges`, `Rect`, `FlexInfo`, `LayoutBox` — modelo inmutable.
- `content_rect(rect, padding)` — content box.
- `flex_layout(content, children, parent)` — reparto puro; devuelve rects e
  información del cálculo (`used`, `free`, `start_offset`, `spacing`,
  `cross_offsets`).
- `layout_tree(node, border)` — construye el árbol geométrico sin mutar `Node`.
- `build_display_list(root)` — IR pre-order listo para cualquier backend.
- `WgpuRenderer` — adapter/device/pipeline cacheados; `render_pillow` es el
  fallback solo si wgpu falla (en ese caso el estado sería `PARTIAL`).

## Limitaciones (a propósito)

- Solo `flex-direction: row`; sin `column`, `row-reverse`, wrapping ni `order`.
- Sin `flex-grow`, `flex-shrink`, `flex-basis`, `min/max-width` ni tamaños
  automáticos: los hijos deben declarar `width` y `height` en px.
- `justify-content` solo `flex-start | center | space-between`;
  `align-items` solo `flex-start | center` (sin `stretch`, `flex-end`,
  `space-around`, `space-evenly`); sin `align-self` ni `align-content`.
- `gap` solo en su forma corta y en px.
- `padding` solo shorthand en px; no hay margins, borders, `box-sizing` ni
  porcentajes. `width`/`height` se interpretan como **border-box** en este slice.
- El documento se modela como el único hijo elemento de `<body>`, colocado en
  `(0,0)`; no se implementa el flujo de bloque de `body` ni el colapso de
  márgenes.
- Cascade propia con selectores **compuestos** (`tag`, `#id`, `.class`), sin
  combinadores, pseudos, inline styles, herencia, `!important` ni `@rules`.
  La especificidad es `(ids, clases, tags)` y luego orden de fuente.
- Colores solo `#rgb` / `#rrggbb`; sin `rgb()`, `hsl()` ni nombres.
- Sin texto, bordes, radios, gradientes, sombras, imágenes, clipping ni
  z-index. El pintado es el orden del display list.
- La raíz debe cubrir el viewport (400×120) para que el clear negro no se vea.
- Tolerancia de píxel ±8 por si otro backend redondea distinto; el camino wgpu
  actual da diferencias 0.

## Recomendación

`flex_layout` y `layout_tree` están listos para entrar en v0 como el caso
`display:flex` del dispatcher de layout: reciben estilos ya cascados y
devuelven un árbol `LayoutBox` inmutable, sin acoplarse a HTML, CSS ni al
backend. El siguiente paso natural es añadir `flex-grow`/`flex-shrink` (reparto
del free space con factores), luego `flex-direction: column` y wrapping; después
tamaños automáticos y margins. Para rendimiento real hay que probar en una GPU
de hardware, porque WARP domina los tiempos de este experimento.
