# R2 — runtime C dinámico (`src/runtime/c-r2`)

**Estado: PASS (69/69 checks).** Layout calculado en C en runtime, ventana
Win32 redimensionable, reflow real en `WM_SIZE`, hit testing y estados
hover/pressed/selected, texto pre-shapeado en un atlas R8 y render con
wgpu-native / D3D12. Sin navegador, sin WebView, sin Skia, sin Electron. Python
solo existe en build time.

Este directorio es la promoción canónica de `experiments/15-runtime-r2-c`. El
`ui_data.h` generado es **byte-idéntico** al de exp15 (mismos 28 nodos, mismos
estilos, mismos 35 glyphs, mismas 6 text runs y mismo atlas), así que el
comportamiento es el mismo; lo que cambia es la ubicación, el emisor
(`src/emitter/python/emit_ui.py`) y que el frontend Python se importa desde
`src/frontend/python` en vez de estar copiado.

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
- Dibuja todos los rects con **una draw call** en wgpu-native (D3D12 en esta
  máquina) y todos los glyphs con **una segunda draw call** texturizada.
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
        │  src/frontend/python/webnative (oráculo)
        │  html5lib → tree · tinycss2/cssselect2 → cascade
        │  HarfBuzz/FreeType → posiciones + bitmaps de glyphs
        ▼
src/emitter/python/emit_ui.py
        ├── build/ui_data.h      UiNode[28] + kUiNodeNames + kUiGlyphs[35]
        │                        + kUiTextRuns[6] + atlas R8 256×256
        └── build/ui_meta.json   nombres, kind, dirección, colores (debug)
        │
        │  runtime (C17, /TC, sin Python)
        ▼
build/runtime_r2.exe
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
directorio de build; el resto es Win32 + VC runtime.

### Por qué R2 usa el frontend Python (y no el Rust)

R2 necesita un **árbol de estilos pre-layout**. El frontend Rust canónico
emite un **display list post-layout** (rects y text runs ya posicionados), que
es el contrato de R1/AOT pero no contiene el árbol ni las reglas para
recalcular a otro viewport.

Por eso `emit_ui.py` añade `src/frontend/python` a `sys.path` e importa
`webnative.cascade`, `webnative.html_parse`, `webnative.css_parse`,
`webnative.tree` y `webnative.text` (HarfBuzz/FreeType). No copia el paquete.
**Paso pendiente:** extender el frontend Rust para que emita el mismo árbol de
estilos (o un contrato equivalente); entonces `emit_ui.py` podrá consumir Rust
y R2 dejará de depender del oráculo Python en build time.

---

## 3. Mapa de módulos

| Ruta | Rol |
|---|---|
| `page/index.html` | Fixture: sidebar 30 %, content 70 %, header, 3 cards, panel. |
| `page/style.css` | Colores y layout del fixture. |
| `src/emitter/python/emit_ui.py` | Aplana el árbol con estilos a `UiNode[]`, resuelve `%`, `flex-wrap`, `data-*`; mide/shapea texto y empaqueta el atlas. Importa el frontend Python de `src/frontend/python`. |
| `runtime_r2.c` | **Todo el runtime**: layout, Win32, input, wgpu, offscreen, CLI. |
| `build.ps1` | Emit → compile (`cl /O2 /TC`) → report → render → states → ventana → verify. |
| `verify.py` | Aceptación: asserts de layout, píxeles, texto, estados y timings. |
| `build/ui_data.h` | Generado. Tabla de nodos, nombres, glyphs, runs y atlas R8. |
| `build/ui_meta.json` | Generado. Metadatos legibles para depurar y para `verify.py`. |
| `build/layout_report.json` | x,y,w,h de los 28 nodos a 960/720/560. |
| `build/verify_report.json` | 69 checks con detalle. |
| `build/build_report.json` / `build/build_log.txt` | Evidencia de build, adapter, timings, reflow, input. |
| `build/out_960.png`, `out_720.png`, `out_560.png` | Renders offscreen. |
| `build/states/state_{normal,hover,press,selected}.png` | Estados del nodo interactivo. |

Todos los artefactos quedan bajo `src/runtime/c-r2/build/`; el directorio de
fuentes solo contiene fixture, runtime, build y verificador.

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
  luego se traslada su subárbol. Esto mantiene el tamaño automático de un hijo
  basado en su contenido real.
- **Auto width de un flex item**: usa **max-content** (ancho intrínseco del
  subárbol, con las métricas de texto del build).
- **Stretch**: en el eje cruzado, solo si el hijo no fijó esa dimensión. Se
  estira la caja; los descendientes conservan las posiciones medidas.
- **Porcentajes**: `%` de ancho contra el content box del padre; `%` de alto
  solo si el padre tiene altura definida (en otro caso auto).
- **gap**: se suma entre hijos y entre líneas.
- **justify**: start/center/end/space-between.
- **align**: start/center/end/stretch.
- **wrap**: packing por líneas. **Decisión R2**: una línea puede encoger sus
  items hasta un 20 % para mantenerse; si no, se rompe. Es lo que produce 2+1
  cards a 560 y 720 con cards de 180 px y padding 24 px.

---

## 5. Texto

- `emit_ui.py` usa `webnative.text.TextEngine` (HarfBuzz + FreeType) para
  shapear cada texto y rasterizar sus glyphs a 8 bits.
- Se empaquetan en un atlas **R8 256×256** con guarda de 1 px (`GlyphPacker`).
- `ui_data.h` lleva `kUiGlyphs[35]` (posición relativa al nodo de texto,
  tamaño, UVs) y `kUiTextRuns[6]` (nodo, primer glyph, count, baseline).
- En runtime se construyen quads redondeados al píxel y se dibujan con
  `textureSample` sobre el atlas.
- El texto no cambia con el resize (no hay zoom): los tamaños 16/24 px se
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
  `action=select` (selección única).
- Prioridad de color: press > hover > selected > background.
- Esc cierra; `--frames N` limita el número de frames y sale con 0.

---

## 7. wgpu-native

- `Gpu` encapsula instance/adapter/device/queue, formato, pipeline sólida,
  pipeline de texto, buffers, atlas, sampler, bind group y bind group layout.
- Pipeline sólida: `Float32x2` clip-space + `Unorm8x4` color (12 bytes).
  Una draw call para todos los rects (84 vértices = 14 rects × 6).
- Pipeline de texto: `Float32x2` pos + `Float32x2` uv + `Unorm8x4` color
  (20 bytes), atlas R8, sampler lineal, blend `SrcAlpha/OneMinusSrcAlpha`.
  Una draw call para todos los glyphs (210 vértices = 35 glyphs × 6).
- Offscreen: textura `RGBA8Unorm` + copia a buffer con filas alineadas a 256
  bytes + `mapAsync` + WIC PNG.
- En ventana se elige un formato **no sRGB** para que los colores coincidan
  byte a byte con el render offscreen.

Nota: wgpu-native emite al arrancar un warning de sondeo
(`Returned GL context is 3.1, when 3.3+ is needed`) antes de seleccionar
D3D12; es interno del backend y no afecta al render.

---

## 8. Comandos

```powershell
# build + aceptación completa (emit, compile, report, renders, states, ventana, verify)
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build.ps1

# reutilizar ui_data.h ya emitido / solo compilar
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build.ps1 -SkipEmit

# solo build (sin ejecutar la aceptación)
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build.ps1 -SkipRun
```

CLI del ejecutable (desde `src/runtime/c-r2/build`):

```powershell
.\runtime_r2.exe --render 960 640 out_960.png
.\runtime_r2.exe --render 960x640 out_960.png
.\runtime_r2.exe --render 960 640 out.png --hover 4
.\runtime_r2.exe --render 960 640 out.png --press 4
.\runtime_r2.exe --render 960 640 out.png --selected 4
.\runtime_r2.exe --states states/state [--node 4] [--size 960x640]
.\runtime_r2.exe --report layout_report.json --sizes 960x640,720x640,560x640
.\runtime_r2.exe --frames 30 --resize-demo --input-demo
.\runtime_r2.exe                     # ventana interactiva hasta cerrar/Esc
```

`build.ps1` usa el venv canónico `src/frontend/python/.venv`,
`src/emitter/python/emit_ui.py`, MSVC (`vcvars64`) y
`third_party/wgpu-native` (include/lib/bin). No referencia `experiments/`.

---

## 9. Evidencia de aceptación (última ejecución)

`build.ps1` terminó con **exit 0** y `verify.py` con **69/69 checks PASS**.

### 9.1 Layout medido

| Viewport | sidebar (30 %) | content (70 %) | header (inner) | cards y | cards w |
|---|---:|---:|---:|---|---|
| 960×640 | 288 | 672 | 624 | 80, 80, 80 | 180, 180, 180 |
| 720×640 | 216 | 504 | 456 | 80, 80, 216 | 180, 180, 180 |
| 560×640 | 168 | 392 | 344 | 80, 80, 216 | 164, 164, 180 |

- A 960: **3 cards en una fila**.
- A 720: **2 cards en la fila 1 y 1 card en la fila 2**.
- A 560: **2 cards en la fila 1 y 1 card en la fila 2** (pitch 120+16 = 136).
- `header.w == content.w - 48` y `panel.w == content.w - 48` en todos.
- `pill.right == content.x + content.w - 24` en todos.

### 9.2 Píxeles

Sobre `out_960.png` y `out_560.png`:

- sidebar `#121824`, content `#0b0e14`, card `#1a1f2a`.
- accent 1 `#e74c3c`, accent 2 `#2ecc71`, accent 3 `#3498db`.
- pill `#2a3140`, panel `#10141b`, líneas del panel `#2a3140`.
- A 560, la card 3 está en la fila 2 y a la derecha de las filas 1/2 hay
  content background (no hay una cuarta card ni overflow).

### 9.3 Estados

`--states build/states/state` genera los 4 PNGs. Para el nodo 4
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
- `surface: formats=6 chosenFormat=27`; el swapchain se reconfigura en cada
  resize.
- Determinismo: dos `--render 960 640` consecutivos producen el mismo
  SHA-256 (`9d7db10b17329ae99c725bb002603892edf47e1da4ab90dacc56c11e049d9fc1`).

### 9.5 Timings (VM, Basic Render Driver)

- Layout (100 iteraciones, `layout_report.json`):
  - 960×640: avg **0.0012 ms** (best 0.0009 ms)
  - 720×640: avg **0.0011 ms** (best 0.0009 ms)
  - 560×640: avg **0.0010 ms** (best 0.0009 ms)
  - 28 nodos; el timer es de alta resolución pero la cifra es de microsegundos.
- Offscreen `--render`:
  - 960: layout 0.015 ms, frame 61.1 ms (readback/map 38.7 ms, PNG 20.0 ms).
  - 720: layout 0.016 ms, frame 50.9 ms (readback/map 32.6 ms, PNG 16.0 ms).
  - 560: layout 0.017 ms, frame 59.7 ms (readback/map 42.7 ms, PNG 14.8 ms).
  - El coste offscreen está dominado por readback y PNG, no por el layout.
- Ventana (FIFO, vsync): init 133.33 ms; **15.99 ms/frame** de pared
  (≈63 Hz, limitado por present); **0.508 ms/frame** de CPU en
  encode+submit, excluyendo present/vsync. Layout del resize: 0.005–0.014 ms.
- Build completo: `emit_ui.py` 635 ms; `cl /O2` 3 501 ms; `verify.py` 1 970 ms.
  `runtime_r2.exe` = 299 008 B; `ui_data.h` = 351 767 B (65 536 B de atlas).

### 9.6 Contra la promoción

- `build/ui_data.h` es **byte-idéntico** al `ui_data.h` de exp15 (mismo hash):
  mismos nodos, mismos estilos, mismos glyphs, mismo atlas.
- `ui_meta.json` coincide nodo a nodo; solo cambia la ruta `source`, que ahora
  es relativa al repo.
- `layout_report.json` coincide en geometría nodo a nodo; solo difieren los
  timings, como es esperable.

---

## 10. Portado vs nuevo

**Portado de exp15 (sin cambios de comportamiento):**

- `runtime_r2.c` — motor de layout, Win32, input, wgpu, offscreen y CLI.
- `verify.py` — 69 checks.
- `page/index.html`, `page/style.css` — fixture.
- `emit_ui.py` — lógica de aplanado, medición/shaping y atlas; ahora en
  `src/emitter/python` e importando el frontend Python canónico.

**Nuevo en la promoción:**

- `src/runtime/c-r2/build.ps1` — build autocontenido (sin `experiments/`),
  con `-SkipEmit`/`-SkipRun`, reporte JSON y salida no-cero en fallo.
- `src/runtime/c-r2/build/` — todos los artefactos generados.
- `src/runtime/README.md` — mapa R1 vs R2.
- `src/emitter/python/emit_ui.py` — `sys.path` a `src/frontend/python`,
  defaults al fixture de c-r2 y `source` relativo en `ui_meta.json`.

**Decisiones de compatibilidad C (heredadas):**

- `NODE_TEXT` colisiona con un enumerador de `msxml.h`; se generan
  `R2_NODE_BLOCK`, `R2_NODE_FLEX`, `R2_NODE_TEXT`.
- `CENTER`/`END` colisionan entre enums; se usan `JUSTIFY_*` y `ALIGN_*`.
- `UiNode` añade `gap`, `font_size` y `text_color`.

---

## 11. Limitaciones (honestas)

1. **Re-layout completo** en cada resize o cambio de estado. No hay
   invalidación incremental, dirty rects ni caché de layout por nodo.
2. **Sin invalidación incremental**: cambiar un color reconstruye toda la
   geometría; cambiar el tamaño re-layouta todo el documento.
3. **Texto**: una sola línea por nodo, sin wrapping ni line breaking; sin
   bidi; sin fallback de fuentes en runtime; sin DPI/zoom; sin subpixel
   positioning; sin selección/IME/emoji.
4. **Flexbox simplificado**: no hay `flex-grow`, `flex-basis`, `align-content`
   ni `order`; el shrink solo se permite hasta 20 % al hacer packing de
   líneas; los márgenes no consumen el eje principal; no hay colapso de
   márgenes.
5. **Wrap no es CSS estricto**: a 560, dos cards de 180 con gap 16 necesitan
   376 px y solo hay 344. R2 permite encoger hasta 20 % antes de romper la
   línea (164+164+16 = 344). Un navegador real daría 1 card por fila. Es la
   desviación más importante y está deliberadamente documentada.
6. **Stretch de caja**: al estirar un hijo no se re-layoutan sus
   descendientes. Afecta a hijos con `%` de altura o `justify` dentro de un
   contenedor estirado; el fixture no lo usa.
7. **Sin scroll ni clipping**: el overflow se dibuja fuera de la ventana.
8. **Sin bordes, radios, sombras, opacidad, transformaciones, gradientes,
   imágenes, pseudo-elementos, `display:inline` real, tablas ni grid.**
9. **Cascada en build time**: no hay DOM/CSS mutable en runtime ni hot reload.
   Cambiar HTML/CSS exige re-ejecutar `emit_ui.py` y recompilar.
10. **Una ventana, un surface**, sin multi-ventana, accesibilidad, clipboard ni
    teclado más allá de Esc.
11. **Adapter software** en esta VM (`Microsoft Basic Render Driver`), así que
    los timings no representan una GPU discreta.
12. **Formato no sRGB** para igualar colores con el PNG; no hay gestión de
    color.
13. **El texto se dibuja después de todos los fondos**: no hay orden de
    pintado por nodo. En el fixture no se solapan.

---

## 12. Paso pendiente: árbol de estilos en Rust

El único acoplamiento a Python en R2 es el **build**. Para eliminarlo:

1. Extender `src/frontend/rust` para emitir un **árbol de estilos pre-layout**
   (por nodo: tag/id/clases, `display`, `flex-direction`, `justify`, `align`,
   `flex-wrap`, `gap`, width/height px/%, márgenes, padding, background,
   color, fuente y `data-*`), o un contrato intermedio equivalente.
2. Hacer que `emit_ui.py` consuma ese JSON en vez de importar
   `webnative.cascade` (el aplanado a `UiNode[]`, el atlas y la verificación se
   quedan igual).
3. Comparar el `ui_data.h` resultante con el del oráculo Python; debe ser
   byte-idéntico o justificar cada diferencia.
4. Solo entonces R2 podrá construirse sin `src/frontend/python/.venv`.

Mientras eso no exista, el frontend Python es el oráculo correcto para R2 y el
runtime C sigue siendo independiente de Python.
