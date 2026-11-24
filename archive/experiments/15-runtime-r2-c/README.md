# exp15-r2-core — primer runtime R2 real (C)

**Estado: PASS.** Layout calculado en runtime, ventana Win32 redimensionable,
reflow real en `WM_SIZE`, hit testing + hover/pressed/selected, y render con
wgpu-native / D3D12. Sin navegador, sin WebView, sin Skia, sin Electron. Python
solo existe en build time.

El **stretch goal de texto también está hecho**: una línea pre-shapeada por nodo
(HarfBuzz), rasterizada con FreeType en un atlas R8 de 256×256 y dibujada con
una segunda pipeline texturizada. No queda texto “pendiente” para una línea;
lo que queda pendiente es texto multilínea/wrapping/bidi/DPI (ver limitaciones).

---

## 1. Qué hace R2 (y qué no)

R2 es el núcleo de un runtime nativo de HTML+CSS:

- Lee una tabla plana de nodos (`ui_data.h`) generada en build time.
- Calcula **todo el layout en C en runtime**: bloque, flex row/column, `gap`,
  `justify-content`, `align-items`, `flex-wrap`, anchos/altos en px y `%`.
- Abre una ventana Win32 real, redimensionable, y en cada `WM_SIZE`
  **recalcula layout + geometría + reconfigura el swapchain**.
- Hace hit testing en orden inverso de pintado y mantiene estado
  `hover` / `pressed` / `selected` a partir de mensajes Win32 reales.
- Dibuja todos los rects con **una sola draw call** en wgpu-native (D3D12 en
  esta máquina) y todos los glyphs con **una segunda draw call** texturizada.
- Puede renderizar la misma escena **offscreen determinista** a PNG
  (`--render W H out.png`), reutilizando exactamente el mismo motor de layout.

Lo que **no** es: no es un navegador, no parsea HTML/CSS en runtime, no tiene
DOM mutable, no implementa el algoritmo completo de CSS Flexbox, no tiene
scroll, ni bordes, ni sombras, ni transformaciones, ni eventos de teclado más
allá de Esc.

---

## 2. Arquitectura

```
page/index.html + page/style.css
        │
        │  build time (Python 3.12, solo build)
        │  html5lib → webnative.tree
        │  tinycss2/cssselect2 → webnative.cascade
        │  HarfBuzz/FreeType → posiciones + bitmaps de glyphs
        ▼
emit_ui.py
        ├── ui_data.h      UiNode[28] + kUiNodeNames + kUiGlyphs[35]
        │                  + kUiTextRuns[6] + atlas R8 256×256
        └── ui_meta.json   nombres, kind, dirección, colores (debug)
        │
        │  runtime (C17, /TC, sin Python)
        ▼
runtime_r2.exe
        ├── layout engine en C
        │     block flow · flex row/column · wrap · gap · % · justify/align
        ├── Win32 host
        │     WM_SIZE → re-layout + reconfig · WM_MOUSEMOVE/LBUTTONDOWN/UP
        │     hit testing inverso · hover/press/selected · Esc cierra
        ├── geometría
        │     N rects × 6 vértices (pos + unorm8x4)
        │     N glyphs × 6 vértices (pos + uv + unorm8x4)
        └── wgpu-native
              surface swapchain (ventana) o textura RGBA8 (offscreen)
              pipeline sólida + pipeline texturizada R8
              readback → buffer → WIC → PNG (solo offscreen)
```

No hay dependencia de Python en el ejecutable. `wgpu_native.dll` se copia al
directorio del exe; el resto es Win32 + VC runtime.

---

## 3. Mapa de módulos

| Ruta | Rol |
|---|---|
| `page/index.html` | Fixture: sidebar 30%, content 70%, header, 3 cards, panel. |
| `page/style.css` | Colores y layout del fixture. |
| `webnative/` | Copia de exp09 (solo capas puras). `html_parse`, `css_parse`, `cascade`, `tree`, `ir`, y `fonts`/`text` para el stretch de texto. `layout.py` queda como referencia portada. |
| `emit_ui.py` | Aplana el árbol con estilos a `UiNode[]`, resuelve `%`, `flex-wrap`, `data-*`; mide/shapea texto y empaqueta el atlas. |
| `ui_data.h` | Generado. Tabla de nodos, nombres, glyphs, runs y atlas R8. |
| `ui_meta.json` | Generado. Metadatos legibles para depurar y para `verify.py`. |
| `runtime_r2.c` | **Todo el runtime**: layout, Win32, input, wgpu, offscreen, CLI. |
| `build.ps1` | Emit → compile (`cl /O2 /TC`) → render → states → ventana → verify. |
| `verify.py` | Aceptación: asserts de layout, píxeles, texto, estados y timings. |
| `layout_report.json` | x,y,w,h de los 28 nodos a 960/720/560. |
| `verify_report.json` | 69 checks con detalle. |
| `build_report.json` / `build_log.txt` | Evidencia de build, adapter, timings, reflow, input. |
| `out_960.png`, `out_720.png`, `out_560.png` | Renders offscreen. |
| `states/state_{normal,hover,press,selected}.png` | Estados del nodo interactivo. |

---

## 4. Semántica de layout (runtime)

Modelo **border-box**, sin bordes. Cada nodo guarda:

```c
typedef struct {
    int parent, kind, direction, justify, align, wrap;
    float width, height;          /* px, -1 = auto */
    float width_pct, height_pct;  /* 0 = none, se resuelve en runtime */
    float margin[4], padding[4];
    unsigned int background;
    const char *text;
    unsigned int hover_bg, press_bg, selected_bg;
    int action;
    float gap;                    /* extensión R2 */
    float font_size;
    unsigned int text_color;
} UiNode;
```

Reglas implementadas (portadas de `exp09/webnative/layout.py` y ampliadas):

- **Bloque**: hijos apilados en vertical; `x = content_x + margin-left`;
  `y = cursor + margin-top`; el cursor avanza `alto + margin-bottom`.
- **Flex row/column**: se mide cada hijo en `(0,0)` con el ancho disponible y
  luego se traslada su subárbol, igual que exp09. Esto mantiene el tamaño
  automático de un hijo basado en su contenido real.
- **Auto width de un flex item**: usa **max-content** (ancho intrínseco del
  subárbol, con las métricas de texto del build). exp09 usaba el ancho del
  contenedor, lo que empujaba la píldora del header fuera de la fila.
- **Stretch**: en el eje cruzado, solo si el hijo no fijó esa dimensión. Se
  estira la caja; los descendientes conservan las posiciones medidas (ver
  limitaciones). En columna con stretch, los hijos se miden ya con el ancho
  final, por eso el header con `space-between` queda correcto.
- **Porcentajes**: `%` de ancho contra el content box del padre; `%` de alto
  solo si el padre tiene altura definida (en otro caso auto).
- **gap**: se suma entre hijos y entre líneas.
- **justify**: start/center/end/space-between.
- **align**: start/center/end/stretch.
- **wrap**: packing por líneas. **Decisión R2**: una línea puede encoger sus
  items hasta un 20% para mantenerse; si no, se rompe. Es lo que produce 2+1
  cards a 560 y 720 con cards de 180px y padding 24px. Un navegador estricto
  rompería por tamaño hipotético y daría 1 card por fila a 560 (ver §11).

---

## 5. Texto (stretch completado)

- `emit_ui.py` usa `webnative.text.TextEngine` (HarfBuzz + FreeType) para
  shapear cada texto y rasterizar sus glyphs a 8 bits.
- Se empaquetan en un atlas **R8 256×256** con guarda de 1px (`GlyphPacker`).
- `ui_data.h` lleva `kUiGlyphs[35]` (posición relativa al nodo de texto,
  tamaño, UVs) y `kUiTextRuns[6]` (nodo, primer glyph, count, baseline).
- En runtime se construyen quads redondeados al píxel y se dibujan con
  `textureSample` sobre el atlas. Blanco sobre fondos oscuros; antialiasing
  del propio bitmap.
- El texto no cambia con el resize (no hay zoom): los tamaños 16/24px se
  mantienen; el layout sí se recalcula y mueve los nodos.

Lo que **no** hace el texto de R2: wrapping, alineación multilínea, bidi,
shaping complejo más allá de una run LTR, fallback de fuentes en runtime,
subpixel positioning, hinting dependiente del DPI, emoji/COLR, selección/IME.

---

## 6. Win32 + input + estados

- Ventana `WS_OVERLAPPEDWINDOW` (redimensionable), cliente inicial 960×640,
  `SetProcessDPIAware`.
- `WM_SIZE` guarda el nuevo tamaño, marca re-layout y reconfiguración; el loop
  hace `layout_page` + `build_geometry` + `wgpuSurfaceConfigure`.
- `WM_MOUSEMOVE` hace hit testing inverso (`for i = N-1 .. 0`) y actualiza
  `g_hover`; `WM_MOUSELEAVE` lo limpia.
- `WM_LBUTTONDOWN` guarda `g_press`; `WM_LBUTTONUP` aplica
  `selected = nodo` si el press y el release caen en el mismo nodo con
  `action=select` (selección única; seleccionar otro limpia el anterior).
- Prioridad de color: press > hover > selected > background.
- Esc cierra; `--frames N` limita el número de frames y sale con 0.

Evidencia real del camino Win32 (de `build_report.json`):

```
WM_SIZE/layout: 960x640 layout 0.020ms card0_y=80.0 card2_y=80.0
resize-demo: frame 5 -> client 560x640
WM_SIZE/layout: 560x640 layout 0.010ms card0_y=80.0 card2_y=216.0
input-demo: WM_MOUSEMOVE (76,67) -> hover=4 body.root/div.sidebar/div.nav
input-demo: WM_LBUTTONDOWN -> press=4 body.root/div.sidebar/div.nav
input-demo: WM_LBUTTONUP -> selected=4 body.root/div.sidebar/div.nav
input-demo: WM_MOUSEMOVE away -> hover=-1
resize-demo: frame 15 -> client 960x640
WM_SIZE/layout: 960x640 layout 0.005ms card0_y=80.0 card2_y=80.0
window: closed after 30 frame(s), init 141.47 ms, 16.00 ms/frame average
```

---

## 7. wgpu-native

- `Gpu` encapsula instance/adapter/device/queue, formato, pipeline sólida,
  pipeline de texto, buffers, atlas, sampler, bind group y bind group layout.
- Pipeline sólida: `Float32x2` clip-space + `Unorm8x4` color (12 bytes).
  El CPU transforma píxeles a NDC con `2x/W-1`, `1-2y/H`. Una draw call para
  todos los rects (84 vértices = 14 rects × 6).
- Pipeline de texto: `Float32x2` pos + `Float32x2` uv + `Unorm8x4` color
  (20 bytes), atlas R8, sampler lineal, blend `SrcAlpha/OneMinusSrcAlpha`.
  Una draw call para todos los glyphs (210 vértices = 35 glyphs × 6).
- Offscreen: textura `RGBA8Unorm` + copia a buffer con filas alineadas a 256
  bytes + `mapAsync` + WIC PNG.
- En ventana se elige un formato **no sRGB** (`BGRA8Unorm`/`RGBA8Unorm`) para
  que los colores coincidan byte a byte con el render offscreen.

Nota: wgpu-native emite al arrancar un warning de sondeo
(`Returned GL context is 3.1, when 3.3+ is needed`) antes de seleccionar
D3D12; es interno del backend y no afecta al render.

---

## 8. Modos de uso

```bat
:: build + aceptación completa
powershell -ExecutionPolicy Bypass -File build.ps1

:: renders deterministas (mismo layout engine que la ventana)
runtime_r2.exe --render 960 640 out_960.png
runtime_r2.exe --render 960x640 out_960.png
runtime_r2.exe --render 960 640 out.png --hover 4
runtime_r2.exe --render 960 640 out.png --press 4
runtime_r2.exe --render 960 640 out.png --selected 4

:: estados automáticos: normal, hover, press, selected
runtime_r2.exe --states states/state [--node 4] [--size 960x640]

:: informe de layout a varios tamaños (sin GPU)
runtime_r2.exe --report layout_report.json --sizes 960x640,720x640,560x640

:: ventana real; 30 frames y demo de resize + input
runtime_r2.exe --frames 30 --resize-demo --input-demo

:: ventana interactiva hasta cerrar/Esc
runtime_r2.exe
```

---

## 9. Evidencia de aceptación

`verify.py` produce `verify_report.json`: **69/69 checks PASS**.

### 9.1 Layout medido

| Viewport | sidebar (30%) | content (70%) | header (inner) | cards y | cards w |
|---|---:|---:|---:|---|---|
| 960×640 | 288 | 672 | 624 | 80, 80, 80 | 180, 180, 180 |
| 720×640 | 216 | 504 | 456 | 80, 80, 216 | 180, 180, 180 |
| 560×640 | 168 | 392 | 344 | 80, 80, 216 | 164, 164, 180 |

- A 960: **3 cards en una fila**.
- A 560: **2 cards en la fila 1 y 1 card en la fila 2** (pitch 120+16 = 136).
- `header.w == content.w - 48` en todos los tamaños.
- `pill.right == content.x + content.w - 24` en todos los tamaños.
- El ancho de `panel` también es el inner width del content.

### 9.2 Píxeles

Sobre `out_960.png` y `out_560.png`:

- sidebar `#121824`, content `#0b0e14`, card `#1a1f2a`.
- accent 1 `#e74c3c`, accent 2 `#2ecc71`, accent 3 `#3498db`.
- pill `#2a3140`, panel `#10141b`, líneas del panel `#2a3140`.
- A 560, la card 3 está en la fila 2 y a la derecha de las filas 1/2 hay
  content background (no hay una cuarta card ni overflow).

### 9.3 Estados

`--states states/state` genera los 4 PNGs. Para el nodo 4
(`body.root/div.sidebar/div.nav`, rect 16,51,120×32):

- normal: fondo transparente → se ve `#121824` del sidebar.
- hover: `#2a3140`; press: `#3a4a5c`; selected: `#3498db`.
- El diff contra normal cae **exactamente** dentro del rect:
  bbox `(16,51,135,82)`, 3812/3813/3824 píxeles cambiados (área 3840; el
  texto blanco tapa el resto).
- press ≠ hover ≠ selected (comparaciones por píxel).

### 9.4 Ventana

- `--frames 30 --resize-demo --input-demo` termina con **exit 0**.
- Adapter impreso: `backend=D3D12 (4) type=cpu vendorID=0x1414 deviceID=0x008C`,
  `device=Microsoft Basic Render Driver` (VM sin GPU discreta).
- Reflow real: `card2_y` 80 → 216 al pasar a 560 y 216 → 80 al volver a 960.
- Input real: hover=4, press=4, selected=4, hover=-1 al salir.
- Determinismo: dos `--render 960 640` consecutivos producen el mismo
  SHA-256 (`9d7db10b…d9fc1`), byte a byte.

### 9.5 Timings

- Layout (100 iteraciones, `layout_report.json`):
  - 960×640: avg **0.0012 ms** (best 0.0009 ms)
  - 720×640: avg **0.0010 ms** (best 0.0009 ms)
  - 560×640: avg **0.0010 ms** (best 0.0009 ms)
  - 28 nodos; tratar como “orden de microsegundos”, el timer es de alta
    resolución pero la cifra es diminuta.
- Offscreen `--render` (misma sesión, Basic Render Driver):
  - layout 0.016–0.025 ms;
  - frame total 52–63 ms, desglosado en GPU encode+submit ≈ 2.5–3.2 ms,
    readback/map ≈ 32–40 ms, PNG WIC ≈ 17–23 ms.
  - El coste offscreen está dominado por readback y PNG, no por el layout.
- Ventana (FIFO, vsync): init ≈ 130–150 ms; **15.8–16.0 ms/frame** de pared
  (≈62.5 Hz, limitado por present); **0.55–0.56 ms/frame** de CPU en
  encode+submit, excluyendo present/vsync. Layout del resize: 0.005–0.020 ms.
- Build: `emit_ui.py` ≈ 0.6–0.9 s; `cl /O2` ≈ 3.9–4.6 s; `verify.py` ≈ 2.1–2.5 s.

### 9.6 Capturas

- `out_960.png`: 3 cards, sidebar 288, header con “Runtime R2” y pill.
- `out_720.png`: 2+1 cards.
- `out_560.png`: 2+1 cards, sidebar 168.
- `states/state_normal.png`, `state_hover.png`, `state_press.png`,
  `state_selected.png`.

---

## 10. Portado vs nuevo

**Portado de exp09 (copiado, sin importar entre directorios):**

- `webnative/html_parse.py`, `css_parse.py`, `cascade.py`, `tree.py`, `ir.py`.
- Semántica de `layout.py`: modelo border-box, block flow, flex
  measure-at-(0,0)-then-translate, `_justify`, `align_items`, métricas de
  texto inyectadas.
- `text.py`/`fonts.py`: shaping HarfBuzz, raster FreeType y métricas, usados
  **solo en build time**.

**Nuevo en R2:**

- Motor de layout en C (`layout_node`, `layout_flex_children`,
  `intrinsic_width`, `translate_subtree`).
- `flex-wrap` con packing por líneas y encogimiento máximo del 20%.
- Resolución runtime de `%`, `gap`, `%` de altura contra padres definidos.
- Ancho intrínseco max-content para flex items (corrige el fill de exp09).
- Ventana Win32, bucle de mensajes, `WM_SIZE`, resize, cierre con Esc.
- Hit testing inverso y máquina de estados hover/press/selected.
- Pipeline wgpu-native sólida + pipeline texturizada R8 + atlas de glyphs.
- Offscreen determinista, `--states`, `--report`, `--frames`, `--input-demo`,
  `--resize-demo`.
- Emisor que fusiona `%`, `flex-wrap` y `data-*` en la tabla `UiNode`.
- Verificación automática de layout, píxeles, texto y estados.

**Decisiones de compatibilidad C:**

- `NODE_TEXT` del enunciado colisiona con un enumerador de `msxml.h` del
  Windows SDK. Se generan `R2_NODE_BLOCK`, `R2_NODE_FLEX`, `R2_NODE_TEXT`.
- `CENTER`/`END` de los enums del enunciado colisionan entre sí en C; se usan
  `JUSTIFY_CENTER`, `JUSTIFY_END`, `ALIGN_CENTER`, `ALIGN_END`.
- Se añadieron a `UiNode`: `gap`, `font_size`, `text_color` (extensiones
  necesarias para gap y texto).

---

## 11. Limitaciones (honestas)

1. **Re-layout completo** en cada resize o cambio de estado. No hay
   invalidación incremental, ni dirty rects, ni caché de layout por nodo.
   Para 28 nodos es ~1 µs; para miles de nodos a 60 Hz no escalaría.
2. **Sin incremental invalidation**: cambiar un color reconstruye toda la
   geometría; cambiar el tamaño re-layouta todo el documento.
3. **Texto**: una sola línea por nodo, sin wrapping ni line breaking; sin
   bidi; sin fallback de fuentes en runtime; sin DPI/zoom (los bitmaps están
   horneados a 16/24 px); sin subpixel positioning; sin selección/IME/emoji.
4. **Flexbox simplificado**: no hay `flex-grow`, ni `flex-basis`, ni
   `align-content`, ni `order`; el shrink solo se permite hasta 20% al hacer
   packing de líneas; los márgenes no consumen el eje principal (semántica
   exp09); no hay colapso de márgenes.
5. **Wrap no es CSS estricto**: a 560, dos cards de 180 con gap 16 necesitan
   376 px y solo hay 344. Para lograr el 2+1 pedido, R2 permite encoger hasta
   20% antes de romper la línea (164+164+16 = 344). Un navegador real daría
   1 card por fila en ese caso; la alternativa estricta sería cards ≤ 160 px
   o padding de content ≤ 8 px. Esta es la desviación más importante y está
   deliberadamente documentada.
6. **Stretch de caja**: al estirar un hijo no se re-layoutan sus descendientes.
   Afecta a hijos con `%` de altura o `justify` dentro de un contenedor
   estirado; el fixture no lo usa.
7. **Sin scroll ni clipping**: el overflow se dibuja fuera de la ventana; no
   hay scrollbars ni contenedores scrolleables.
8. **Sin bordes, radios, sombras, opacidad, transformaciones, gradientes,
   imágenes, pseudo-elementos, `display:inline` real, tablas, grid.**
9. **Cascada en build time**: no hay DOM/CSS mutable en runtime, ni hot reload.
   Cambiar el HTML/CSS exige re-ejecutar `emit_ui.py` y recompilar (o al menos
   regenerar `ui_data.h` y recompilar el exe).
10. **Una ventana, un surface**, sin multi-ventana, sin accesibilidad, sin
    clipboard, sin teclado más allá de Esc.
11. **Adapter software** en esta VM (`Microsoft Basic Render Driver`), así que
    los timings offscreen no representan una GPU discreta.
12. **Formato no sRGB** para igualar colores con el PNG; no hay gestión de
    color.
13. **El texto se dibuja después de todos los fondos**: no hay orden de
    pintado por nodo (un texto nunca queda detrás de un fondo posterior). En
    el fixture no se solapan.

---

## 12. Análisis: qué implica R2

R2 demuestra que el “núcleo duro” de un runtime HTML/CSS nativo es viable en
C sin dependencias grandes: **28 nodos, layout en microsegundos, ventana real,
reflow y estados, todo en ~2.650 líneas de C (incluida toda la inicialización
Win32/wgpu) y una tabla generada**. En la ventana, la parte gráfica
(wgpu-native) no es el cuello de botella; lo es el texto, la invalidación y la
correctitud de CSS. Offscreen, el readback + PNG domina, pero es una
herramienta de verificación, no el camino interactivo.

Pero R2 también muestra dónde está el 80% restante:

- **El layout “casi CSS” se vuelve mentira rápido.** El caso 560 de este
  fixture no se puede resolver con Flexbox estricto y el ancho/padding dados;
  R2 necesitó una regla de shrink del 20%. Un motor serio debe implementar
  min-content/auto minimum, el algoritmo de líneas y el resto de la spec, no
  aproximaciones.
- **El texto horneado no escala.** Un atlas de bitmaps a tamaños fijos es
  suficiente para una demo, pero un runtime real necesita shaping en runtime,
  fallback, wrapping, bidi, DPI y caché de glyphs. Es el subsistema más caro.
- **Full re-layout no es una arquitectura de producción.** Para documentos
  pequeños es imbatible por simplicidad; para aplicaciones grandes hay que
  construir invalidación, medición cacheada y capas.
- **La cascada en build time es una elección de producto.** R2 asume documentos
  estáticos. Un “runtime” de verdad necesita parser CSS + cascade en runtime,
  o al menos un formato intermedio que se pueda recalcular sin recompilar.
- **La GPU no es el problema.** Una draw call de rects y otra de glyphs
  alcanzan de sobra; el coste offscreen está en readback/PNG. El reto es el
  contenido y la corrección, no el número de draw calls.

En resumen: R2 es un **kernel creíble de runtime nativo para documentos
acotados**, no un sustituto de navegador. La arquitectura build-time
frontend + runtime C + wgpu-native funciona y es medible; lo que falta para
producto es texto completo, invalidación incremental, CSS más fiel y
plataforma (accesibilidad, IME, scroll, DPI).

---

## 13. Reproducir

```bat
cd .\exp15-r2-core
py -3.12 -m venv .venv
.venv\Scripts\python.exe -m pip install --upgrade pip
.venv\Scripts\python.exe -m pip install html5lib tinycss2 cssselect2 pillow uharfbuzz freetype-py
powershell -ExecutionPolicy Bypass -File build.ps1
```

`build.ps1` usa `cl` de MSVC y `wgpu-native` de
`.\exp10-aot-gpu\wgpu-native` (solo lectura en build;
copia la DLL al directorio de salida). Salida esperada: `PASS`, 69/69 checks,
`exit 0` y los PNGs listados arriba.
