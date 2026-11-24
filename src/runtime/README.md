# R2 — runtime C dinámico (`src/runtime/c-r2`)

**Estado: PASS (69/69 checks).** Layout calculado en C en runtime, ventana
Win32 redimensionable, reflow real en `WM_SIZE`, hit testing y estados
hover/pressed/selected, texto pre-shapeado en un atlas R8 y render con
wgpu-native / D3D12. Sin navegador, sin WebView, sin Skia, sin Electron. El
árbol de estilos pre-layout lo emite **el frontend Rust canónico**
(`--emit-ui`); `emit_ui.py` solo lo traduce a `ui_data.h`. Python sigue siendo
build-time y el frontend Python queda como oráculo/fallback.

**ui-tree/2 (piloto):** grid `auto-fill`, scroll vertical con clipping, imágenes
locales PNG/JPEG y `WN_EVENT_WHEEL` (ABI v2). El piloto canónico
(`benchmarks/pilot-media-library`) pasa **48/48**, el fixture local
`tests/pilot-media-library` **38/38** y el fixture v1 conserva **69/69**;
`build_abi.ps1` queda en PASS (§14).

**ui-tree/3 + ABI v4 (text input):** `<input type="text">` de una línea con
piscina ASCII imprimible horneada en el atlas, caret/selección, teclado,
clipboard Win32, `maxlength`, `WN_EVENT_INPUT`, `wn_node_set_value` y
`wn_node_get_value` (§16). El fixture nuevo pasa **111/111** checks.

Este directorio es la promoción canónica de `archive/experiments/15-runtime-r2-c`. Con
el árbol Python (`-UsePythonTree`) el `ui_data.h` generado sigue siendo
**byte-idéntico** al de exp15. Con el árbol Rust (por defecto) los 28 nodos,
todos los estilos, nombres, `data-*`, runs y posiciones/avances de glyph son
idénticos; el atlas difiere solo en antialiasing y en la caja de 1 glyph
(hinting de FreeType vs `ab_glyph`, ver §9.6).

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
bordes, sombras ni transformaciones. Desde ui-tree/2 sí tiene un subconjunto de
**grid auto-fill, scroll vertical con clipping e imágenes locales** (§14) y
desde ui-tree/3 un **text input ASCII de una línea** (§16); no hay IME, shaping
en runtime, wrapping, password ni undo.

---

## 2. Arquitectura

```
page/index.html + page/style.css
        │
        │  build time: frontend Rust canónico
        │  src/frontend/rust --emit-ui  (html5ever · cssparser · rustybuzz)
        │  árbol de estilos pre-layout, sin layout, con texto shapeado
        ▼
build/ui.json + build/atlas.bin + build/atlas.json
        │
        │  build time: src/emitter/python/emit_ui.py --ui-json
        │  (traduce el árbol; no re-parsea HTML/CSS)
        ▼
build/ui_data.h      UiNode[28] + kUiNodeNames + kUiGlyphs[35]
                     + kUiTextRuns[6] + atlas R8 256×256
build/ui_meta.json   nombres, kind, dirección, colores (debug)
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
        │     N imágenes × 6 vértices (pos + uv) + batches por clip
        └── wgpu-native
              surface swapchain (ventana) o textura RGBA8 (offscreen)
              pipeline sólida + R8 de texto + RGBA8 de imágenes
              scissor por batch (clip del scroll) · WIC PNG/JPEG → RGBA8
              readback → buffer → WIC → PNG (solo offscreen)
```

No hay dependencia de Python en el ejecutable. `wgpu_native.dll` se copia al
directorio de build; el resto es Win32 + VC runtime.

### De dónde sale el árbol de estilos

R2 necesita un **árbol de estilos pre-layout**; el IR de R1 es un display list
post-layout y no sirve para recalcular a otro viewport. Desde esta ronda:

1. `frontend-rust --emit-ui build/ui.json` parsea HTML/CSS, ejecuta la misma
   cascada que R1 y serializa el árbol **sin ejecutar layout**: `parent`,
   `children`, `kind`, estilos computados (incl. `%`, `flex-wrap`, gap,
   justify/align), `data-*`, y por nodo de texto el contenido + glyphs
   pre-shapeados (gid, avance, x/y, w/h, UVs) + referencia al atlas.
2. `emit_ui.py --ui-json build/ui.json` aplana ese árbol al mismo `ui_data.h`
   que producía el oráculo Python, sin importar `webnative` (solo usa
   `tinycss2` para normalizar colores).
3. `build.ps1` usa ese camino por defecto. `-UsePythonTree` mantiene el
   camino antiguo (`emit_ui.py --html/--css` con HarfBuzz/FreeType) como
   oráculo y fallback. `-SkipParityCheck` desactiva la comparación automática.

Los dos caminos generan el mismo contrato; el runtime C no sabe cuál se usó.

### `ui.json` en una tabla

Esquema `webnative.ui-tree/1`; el detalle completo está en
`src/frontend/rust/README.md` (§ Modo R2). Lo esencial:

| Campo | Contenido |
|---|---|
| `nodes[].parent` / `children[]` | Índices planos; raíz `parent=-1` |
| `nodes[].kind` | `block` \| `flex` \| `grid` \| `text` \| `image` |
| `nodes[].style` | `display`, `direction`, `justify`, `align`, `wrap`, `gap`, `width_px/pct`, `height_px/pct`, `margin[4]`, `padding[4]`, `background`, `color`, `font_size`, `font_family`, `font_weight`; v2: `grid_template_columns`, `grid_auto_rows`, `row_gap`, `column_gap`, `overflow`, `object_fit` |
| `nodes[].image` | v2: `src`, `alt`, `object_fit` |
| `nodes[].data` | `hover`, `press`, `selected`, `action` (atributos `data-*` crudos) |
| `nodes[].argb` | Los mismos colores como `0xAARRGGBB` |
| `nodes[].text` | `content`, `baseline`, `glyphs[] {gid, advance, x, y, w, h, u0..v1}` |
| `atlas` | `file`, `width`, `height`, `format`, `sha256`, `glyph_count`, `pixels_nonzero` |

`emit_ui.py --ui-json` no re-parsea HTML/CSS: solo traduce `ui.json` +
`atlas.bin` al `UiNode[]` de C. La única propiedad CSS que reinterpreta son los
colores (tinycss2), para no duplicar la tabla de nombres CSS en Rust.

---

## 3. Mapa de módulos

| Ruta | Rol |
|---|---|
| `page/index.html` | Fixture: sidebar 30 %, content 70 %, header, 3 cards, panel. |
| `page/style.css` | Colores y layout del fixture. |
| `src/frontend/rust/src/ui_tree.rs` | `--emit-ui`: aplana el árbol con estilos a JSON pre-layout, mide/shapea texto y empaqueta el atlas R8 256×256. |
| `src/emitter/python/emit_ui.py` | Dos modos de entrada: `--ui-json` (árbol Rust, por defecto) y `--html/--css` (oráculo Python). Genera `ui_data.h` + `ui_meta.json`. `--compare` verifica paridad. |
| `runtime_r2.c` | Layout, Win32, input, CLI y modos offscreen; **renderiza por la vtable** (sin wgpu propio). |
| `build.ps1` | Rust build + emit-ui → `--ui-json` → compile (`cl /O2 /TC`) → report → render → states → ventana → verify. |
| `verify.py` | Aceptación: asserts de layout, píxeles, texto, estados y timings. |
| `build/ui.json` | Generado por Rust. Árbol de estilos pre-layout documentado (§2). |
| `build/atlas.bin` / `build/atlas.json` | Generados por Rust. Atlas R8 256×256 y sus metadatos. |
| `build/ui_data.h` | Generado por `emit_ui.py`. Tabla de nodos, nombres, glyphs, runs y atlas R8. |
| `build/ui_meta.json` | Generado. Metadatos legibles para depurar y para `verify.py`. |
| `build/oracle/ui_data.h` | Generado en la comprobación de paridad (`emit_ui.py --html`). |
| `build/parity_report.txt` | Salida de `emit_ui.py --compare` (nodos, glyphs, atlas). |
| `build/layout_report.json` | x,y,w,h de los 28 nodos a 960/720/560. |
| `build/verify_report.json` | 69 checks con detalle. |
| `build/build_report.json` / `build/build_log.txt` | Evidencia de build, árbol usado, adapter, timings, reflow, input. |
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
    /* ui-tree/2 */
    int grid_mode;                /* R2_GRID_AUTO_FILL */
    float grid_column_px;
    float grid_auto_rows;         /* -1 = auto */
    float row_gap, column_gap;
    int overflow;                 /* VISIBLE|HIDDEN|SCROLL */
    float image_width, image_height;
    const char *image_src, *image_alt;
    int object_fit;               /* FILL|COVER|CONTAIN */
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
- **grid (v2)**: solo `repeat(auto-fill, <px>)`.
  `columnas = max(1, floor((content_w + column_gap) / (size + column_gap)))`,
  items en orden row-major, ancho de track = `size` (no se estira), altura de
  fila = `grid_auto_rows` o el máximo de los hijos de la fila, y
  `row_gap`/`column_gap` (o `gap`). Sin `fr`, `minmax`, `auto-fit`, `span` ni
  grid anidado.
- **scroll (v2)**: contenedor con altura definida y `overflow: scroll|auto`.
  Guarda `scroll_y` por nodo, desplaza sus hijos y recorta al content box;
  `WN_EVENT_WHEEL` aplica `delta_y` y hace clamp `[0, content_h - view_h]`.
  Solo vertical; sin scroll anidado real (el más interno con overflow gana).
- **imágenes (v2)**: `kind=image` con `src` local. El tamaño intrínseco
  (PNG/JPEG) se resuelve en build time en `emit_ui.py`; el layout lo usa si
  faltan `width/height`. `object-fit` se resuelve en el core (UVs y caja).
- **clipping (v2)**: cada nodo guarda su clip efectivo (intersección de los
  content boxes con overflow de sus ancestros y el viewport). El core agrupa
  draws por clip (`wn_draw_batch`) y el renderer aplica scissor por batch.

---

## 5. Texto

- `frontend-rust --emit-ui` usa `rustybuzz` (shaping, mismo HarfBuzz que el
  oráculo) y `ab_glyph` (raster) para hornear cada texto. `-UsePythonTree` usa
  HarfBuzz/FreeType vía `webnative.text`.
- Se empaquetan en un atlas **R8 256×256** con guarda de 1 px (mismo shelf
  packer en ambos caminos).
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
  Un batch por clip (en la página v1: una draw call para 84 vértices = 14
  rects × 6).
- Pipeline de texto: `Float32x2` pos + `Float32x2` uv + `Unorm8x4` color
  (20 bytes), atlas R8, sampler lineal, blend `SrcAlpha/OneMinusSrcAlpha`.
  Un batch por clip (v1: 210 vértices = 35 glyphs × 6).
- Pipeline de imágenes: `Float32x2` pos + `Float32x2` uv (16 bytes), textura
  RGBA8 por ruta (caché WIC), mismo bind group layout que el atlas; scissor
  por batch (`SetScissorRect`).
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
# build + aceptación completa (Rust tree por defecto: cargo + --emit-ui + --ui-json)
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build.ps1

# fallback: árbol del oráculo Python (HarfBuzz/FreeType)
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build.ps1 -UsePythonTree

# reutilizar ui_data.h ya emitido / solo compilar
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build.ps1 -SkipEmit

# solo build (sin ejecutar la aceptación)
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build.ps1 -SkipRun

# sin recompilar Rust ni comparar contra el oráculo
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build.ps1 -SkipRustBuild -SkipParityCheck

# fixture canónico del piloto (benchmarks/pilot-media-library, 1280x800)
# → build_pilot\ + verify.py del piloto (48 checks)
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build.ps1 -Pilot -SkipRustBuild

# regresión del escaping C no-ASCII del emisor
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\emitter-escapes\run_test.ps1

# UI dinámica v0 (ABI v3): core headless + 11 PNGs + demo ABI + 53 checks
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\dynamic\run_dynamic.ps1
```

Emisión manual (misma secuencia que `build.ps1`):

```powershell
src\frontend\rust\target\release\frontend-rust.exe `
    --html src\runtime\c-r2\page\index.html --css src\runtime\c-r2\page\style.css `
    --width 960 --height 640 --emit-ui src\runtime\c-r2\build\ui.json

src\frontend\python\.venv\Scripts\python.exe src\emitter\python\emit_ui.py `
    --ui-json src\runtime\c-r2\build\ui.json `
    --out src\runtime\c-r2\build\ui_data.h --meta src\runtime\c-r2\build\ui_meta.json

# paridad semántica contra el oráculo Python
src\frontend\python\.venv\Scripts\python.exe src\emitter\python\emit_ui.py `
    --html src\runtime\c-r2\page\index.html --css src\runtime\c-r2\page\style.css `
    --out src\runtime\c-r2\build\oracle\ui_data.h --meta src\runtime\c-r2\build\oracle\ui_meta.json
src\frontend\python\.venv\Scripts\python.exe src\emitter\python\emit_ui.py `
    --compare src\runtime\c-r2\build\oracle\ui_data.h src\runtime\c-r2\build\ui_data.h
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

Piloto ui-tree/2 (grid + scroll + imágenes), desde la raíz del repo:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
    -File src\runtime\c-r2\tests\pilot-media-library\run_pilot.ps1
```

Genera `out\posters\*.png` y `ui.json`, emite `out\ui_data.h`, compila
`out\runtime_r2_pilot.exe`, renderiza 1280×800 y 800×600, aplica wheel
(`--wheel X Y DY`), verifica 38 checks y compila además `out\abi\abi_pilot.exe`
contra `renderer_wgpu.c` (ventana, 20 frames). Evidencia en
`tests\pilot-media-library\out\` (`render_1280.png`, `render_800.png`,
`wheel_*.png`, `pilot_layout.json`, `pilot_verify_report.json`).

`build.ps1` usa el binario `src/frontend/rust/target/release/frontend-rust.exe`,
el venv canónico `src/frontend/python/.venv`, `src/emitter/python/emit_ui.py`,
MSVC (`vcvars64`) y `third_party/wgpu-native` (include/lib/bin). No referencia
`archive/experiments/`.

---

## 9. Evidencia de aceptación (última ejecución)

`build.ps1` (árbol Rust por defecto) terminó con **exit 0**, la comprobación de
paridad con **PASS** y `verify.py` con **69/69 checks PASS**. `-UsePythonTree`
también termina con **69/69 PASS**.

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
  bbox `(16,51,135,82)`, 3819/3821/3826 píxeles cambiados (área 3840; el
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
  SHA-256 (`5ed6b6c02f690cac71346cc051674e12b3fb3b47f3685ed6862708356df26054`).

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
- Build completo (árbol Rust): `cargo build` incremental 0.25 s (frío 35.6 s);
  `--emit-ui` 0.02–0.12 s; `emit_ui.py --ui-json` **~0.20 s**; paridad vs
  oráculo **~0.83 s**; `cl /O2` 3.5 s; `verify.py` 2.0 s.
  `runtime_r2.exe` = 299 008 B; `ui_data.h` = 351 767 B (65 536 B de atlas).
- Con `-UsePythonTree`, `emit_ui.py` (HarfBuzz/FreeType) tarda **~0.56 s**:
  es la diferencia principal entre ambos caminos en build time.

### 9.6 Paridad semántica Rust vs Python

`build.ps1` (árbol Rust) genera además `build/oracle/ui_data.h` con el oráculo
Python y ejecuta `emit_ui.py --compare`; el build falla si hay diferencias
semánticas. Resultado con el fixture 960×640:

| Dimensión | Resultado |
|---|---|
| Nodos / `parent` / `children` / `kind` | 28 = 28; 0 diferencias |
| Estilos (display, direction, justify, align, wrap, gap, px/%, margin, padding, background, color, font-size/family/weight) | 0 diferencias |
| `data-hover` / `data-press` / `data-selected` / `data-action` | Idénticos (también en `ui_meta.json`) |
| Nombres (`kUiNodeNames` / `ui_meta.json`) | Idénticos, incluidos `[n]` |
| Glyphs / text runs | 35 / 6 = 35 / 6 |
| Avances y `x` de glyph | 35/35 exactos |
| UVs | 33/35 exactas; 2 siguen a una caja 1 px distinta |
| Atlas | 256×256 = 256×256; 1 242/65 536 bytes distintos (AA) |
| `ui_data.h` | Idéntico línea a línea salvo 2 líneas de glyph y el bloque del atlas |

**Única diferencia de glyph**: `i` (gid 76) a 24 px. FreeType aplica TrueType
hinting (`bitmap_top=17`, `rows=17`); `ab_glyph` no (`top=18`, `rows=18`). Ese
píxel extra desplaza el bitmap 1 px y la UV del último glyph de la misma shelf.
No cambia ningún `x`, avance, nodo, estilo ni el tamaño del atlas; el render
verificado (69/69, texto incluido) es correcto. `emit_ui.py --compare` lo
reporta como `known` y solo falla con `--strict`.

### 9.7 Contra la promoción

- Con `-UsePythonTree`, `build/ui_data.h` es **byte-idéntico** al `ui_data.h`
  de exp15 (mismo hash): mismos nodos, estilos, glyphs y atlas.
- `ui_meta.json` coincide nodo a nodo en ambos caminos; solo cambia la ruta
  `source`, que ahora es relativa al repo.
- `layout_report.json` coincide en geometría nodo a nodo; solo difieren los
  timings, como es esperable.

### 9.8 Regresión R1 (frontend Rust)

El modo `--emit-ui` no toca el camino R1. Verificado tras los cambios:

| Comprobación | Resultado |
|---|---|
| `cargo build --release` | OK |
| `bench.py --runs 3 --only rust` | `contract` **PASS**, `flex-page` **PASS** |
| IR `contract` / `flex-page` | **byte-idéntico** al golden |
| IR `text-card` | **byte-idéntico** al artefacto previo |
| `verify_text.py` (text-card) | **18/18 PASS** (jsonschema no instalado: check omitido) |

---

## 10. Portado vs nuevo

**Portado de exp15 (sin cambios de comportamiento):**

- `runtime_r2.c` — motor de layout, Win32, input, CLI y offscreen por la vtable.
- `verify.py` — 69 checks.
- `page/index.html`, `page/style.css` — fixture.
- `emit_ui.py` — lógica de aplanado, medición/shaping y atlas; ahora en
  `src/emitter/python`. El modo `--html/--css` (oráculo) importa el frontend
  Python canónico; el modo `--ui-json` consume el árbol Rust.

**Nuevo en la promoción:**

- `src/runtime/c-r2/build.ps1` — build autocontenido (sin `archive/experiments/`),
  árbol Rust por defecto, `-UsePythonTree`/`-SkipEmit`/`-SkipRun`/
  `-SkipRustBuild`/`-SkipParityCheck`, reporte JSON y salida no-cero en fallo.
- `src/frontend/rust/src/ui_tree.rs` — `--emit-ui`: árbol pre-layout + atlas.
- `src/emitter/python/emit_ui.py` — modos `--ui-json` y `--compare`; imports
  de `webnative` diferidos al modo oráculo; `source` relativo en
  `ui_meta.json`.
- `src/runtime/c-r2/build/` — todos los artefactos generados, incluidos
  `ui.json`, `atlas.bin`, `atlas.json` y `oracle/ui_data.h`.

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
7. **Scroll v2 solo vertical** y con un contenedor efectivo por punto (gana el
   más interno); no hay scroll horizontal real (`delta_x` se ignora), momentum,
   scrollbars dibujadas, ruedas anidadas ni rueda sobre `overflow:hidden`.
8. **Sin bordes, radios, sombras, opacidad, transformaciones, gradientes,
   pseudo-elementos, `display:inline` real, tablas ni grid completo.** El grid
   v2 es solo `auto-fill` con tracks fijos; las imágenes son locales (sin red),
   PNG/JPEG vía WIC, sin `srcset`, lazy loading ni animación.
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

## 12. Acoplamiento a Python: estado y gap restante

El árbol de estilos ya lo emite Rust (`--emit-ui`) y `emit_ui.py --ui-json` lo
traduce sin importar `webnative`. Lo que queda:

1. **`emit_ui.py` sigue siendo un script Python** para traducir JSON → C. Es
   ~200 ms y no vuelve a parsear HTML/CSS, pero requiere el intérprete. Para
   eliminar Python del build de R2 habría que portar ese traductor a Rust (o
   hacer que Rust emita directamente `ui_data.h`), incluido el formato
   `c_float` a 4 decimales y el volcado del atlas.
2. **tinycss2 en `--ui-json`**: el consumidor reinterpreta `background`,
   `color` y `data-*` con la tabla CSS completa. Rust ya emite
   `argb.*` con hex/rgb()/nombres básicos; si se acepta esa tabla (o se
   completa), `--ui-json` puede ser stdlib puro.
3. **Hinting vertical**: `ab_glyph` no aplica TrueType hinting. 1 glyph de 35
   (`i`, gid 76, 24 px) tiene 1 px más de alto que FreeType; el resto es
   idéntico. Si se exige byte-identidad de atlas habría que rasterizar con
   FreeType (o portar el intérprete de hinting).
4. **El modo oráculo (`--html/--css`) sigue importando `src/frontend/python`**
   para HarfBuzz/FreeType. Es el fallback y la referencia de paridad, no el
   camino por defecto.

El runtime C sigue siendo independiente de Python: solo el build lo usa.

---

## 13. Runtime platform ABI v0 (core / renderer / host, standalone + embedded)

Esta capa implementa `include/webnative.h` sin cambiar la ABI y sin exponer
wgpu, D3D12 ni Win32 en el header público. `runtime_r2.c` sigue siendo el
binario de referencia y el sujeto de la aceptación 69/69; la capa ABI usa el
mismo algoritmo extraído a `wn_core.c` y se compila aparte.

> **ABI v3 (aditiva)**: la UI dinámica de la sección 15 añade `wn_node_id`,
> visibilidad, `set_image`, `set_text`, clonado y borrado. Los structs y eventos
> v2 (`wn_event`, `wn_config`, wheel) no cambian.
>
> **ABI v4 (aditiva)**: la sección 16 añade `WN_EVENT_INPUT = 11`,
> `wn_node_set_value` y `wn_node_get_value`; `wn_event` conserva tamaño y
> layout. `wn_abi_version()` devuelve 4.

### 13.1 Módulos

| Ruta | Rol |
|---|---|
| `wn_core.h` / `wn_core.c` | Core portable: documento compilado, layout, estados, hit testing y display list (vértices sólidos + glyphs). No conoce OS, GPU ni modo. `runtime_r2.c` lo usa para no duplicar el motor. |
| `wn_renderer.h` | Vtable `wn_renderer_ops`: `init(surface,w,h)`, `resize`, `upload(display list/atlas)`, `render` (draw+present), `destroy`, `last_error`, `get_info`, `get_stats`. |
| `renderer_wgpu.c` | Backend wgpu-native (D3D12 aquí): surface desde HWND, swapchain FIFO, pipeline sólida + pipeline de texto R8, atlas bajo demanda. |
| `renderer_d3d11.c` | Segundo backend, D3D11 nativo: `D3D11CreateDevice` hardware (WARP solo como fallback), swapchain flip-discard DXGI, HLSL con `D3DCompile`, atlas R8, imágenes WIC RGBA8 cacheadas, scissor por batch. Ver sección 18. |
| `wn_host.h` / `host_win32.c` | Host Win32: ventana, message loop, traducción Win32 → `wn_event`, resize, captura de ratón, cierre. |
| `wn_view.h` / `wn_view.c` | Modo embedded: core + renderer sobre un HWND del host. El host posee el loop y llama `wn_view_render()` a demanda. Elige D3D11 por defecto en Windows (`WN_RENDERER=d3d11|wgpu|auto`). |
| `wn_app.h` / `wn_app.c` | Modo standalone: host Win32 + `wn_view`. `wn_app_run()` bloquea hasta cerrar. |
| `wn_common.c` / `wn_internal.h` | Versión, error thread-local y helpers de copia UTF-8. |
| `examples/standalone.c` | Demo `wn_app_*`; `--frames N` cierra solo. |
| `examples/dynamic.c` | Demo de UI dinámica (ABI v3): visibilidad, clon, imagen, texto, append/remove; `--frames N` cierra solo. |
| `examples/embedded.c` | Demo host: ventana padre + HWND hijo con `wn_view`; reenvía `WM_MOUSEMOVE`/`WM_LBUTTONDOWN`/`WM_LBUTTONUP`/`WM_SIZE`, renderiza en timer, `--frames N` cierra solo. |
| `examples/abi_header_check.c/.cpp` | Compilan `webnative.h` como C17 y C++17 con `/W4`. |
| `examples/abi_smoke.c` | Comprueba versión, error thread-local y validación de argumentos sin crear ventana/GPU. |
| `build_abi.ps1` | Compila la capa, ejecuta header checks, smoke y ambas demos; escribe `build_abi/abi_report.json`. |

### 13.2 Dos modos, un core

| | Standalone | Embedded |
|---|---|---|
| Ventana | La crea `wn_app_create` | La crea el host (padre + hijo) |
| Loop | `wn_app_run` | El host |
| Render | Automático por frame | `wn_view_render()` a demanda |
| Eventos | Win32 → host → view | El host llama `wn_view_event()` |
| Superficie | HWND de la ventana propia | HWND hijo proporcionado |

### 13.3 Comandos

```powershell
# build + header checks + smoke + demos + auto-close (no ejecuta cargo)
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build_abi.ps1

# artefactos
.\src\runtime\c-r2\build_abi\standalone.exe --frames 20
.\src\runtime\c-r2\build_abi\embedded.exe --frames 20 --input-demo --resize-demo

# solo compilar
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build_abi.ps1 -SkipRun

# forzar un backend concreto (A/B): D3D11 (por defecto) o wgpu
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build_abi.ps1 -Renderer d3d11
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build_abi.ps1 -Renderer wgpu

# captura + comparación de píxeles D3D11 vs wgpu (fixture base y piloto con imágenes)
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\d3d11\run_d3d11.ps1
# coste CPU idle/hover (D3D11 hardware vs wgpu/WARP)
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\d3d11\measure_cpu.ps1
```

Si falta `build/ui_data.h`, `build_abi.ps1` llama a
`build.ps1 -SkipRustBuild -SkipParityCheck -SkipRun` (reutiliza el
`frontend-rust.exe` ya compilado y el emisor Python; no lanza cargo).

### 13.4 Semántica de la ABI en v0

- **Strings**: todo se copia; el caller conserva ownership. UTF-8.
- **Threading**: una instancia por hilo de UI; callbacks en el hilo de UI; sin
  locks. Destruir en el mismo hilo.
- **`wn_last_error`**: buffer thread-local; se limpia al inicio de cada llamada
  pública y se devuelve `NULL` si no hubo error.
- **`wn_view_create`**: el handle nativo debe ser un HWND válido; `flags` debe
  ser 0; crea core + renderer. El host embedded decide cuándo llamar
  `wn_view_event` y `wn_view_render`.
- **`wn_app_create`**: crea host Win32 + view; `wn_app_run` muestra la ventana y
  bombea hasta `WM_CLOSE`/Esc o hasta `wn_app_set_max_frames(N)`.
- **`load`**: copia HTML/CSS/base. En v0 el documento que se renderiza es el
  árbol R2 compilado en `ui_data.h`; **no hay parser HTML/CSS en runtime** (ver
  límites). `base_url` y el texto se conservan como fuente/diagnóstico.
- **`target_id`**: nombre de nodo del árbol compilado (por ejemplo
  `body.root/div.sidebar/div.nav`), no un atributo `id` HTML. `set_text` y
  `set_class` aceptan la ruta exacta o el último componente si es único.
- **Click**: la ABI v0 congelada tiene `POINTER_DOWN`/`POINTER_UP`, no
  `WN_EVENT_CLICK`. La regla R2 es: si down y up caen en el mismo nodo con
  `data-action="select"`, el nodo queda `selected`. El callback recibe
  `WN_EVENT_POINTER_UP` con su `target_id`.
- **Wheel (ABI v2)**: `WN_EVENT_WHEEL = 10` con `float delta_x, delta_y` al
  final de `wn_event`; `WN_ABI_VERSION = 2`. `host_win32.c` mapea
  `WM_MOUSEWHEEL`/`WM_MOUSEHWHEEL` (1 notch = 120 px; rueda hacia delante =
  `delta_y = -120`). `wn_view_event` enruta al contenedor con scroll más
  cercano bajo `(x,y)` y devuelve su nombre en `target_id`; `wn_app` reenvía el
  evento tal cual. Solo se consume `delta_y`.
- **`set_class`**: `"hover"`, `"press"` y `"selected"` cambian los colores de
  estado R2. Otras clases se recuerdan por nodo pero los estilos están horneados
  en build time (no hay cascada en runtime).
- **`set_text`**: v0 no tiene shaping en runtime. El cambio se acepta si todos
  sus caracteres existen ya en el atlas del mismo tamaño de fuente (el fixture
  permite, por ejemplo, `"Texto"`); en otro caso devuelve
  `WN_ERR_UNSUPPORTED`. Las métricas de layout del nodo no cambian.

### 13.5 Evidencia de aceptación (última ejecución)

`build_abi.ps1` termina con **exit 0**:

- `include/webnative.h` compila como C17 y C++17 con MSVC `/W4`.
- Grep proof: no aparecen `WGPU|wgpu|ID3D|D3D12|D3D11|HWND|HINSTANCE|IDXGI`.
- `abi_smoke.exe`: PASS (versión, error thread-local, argumentos inválidos).
- `standalone.exe --frames 20`: **exit 0**, `backend=D3D11`,
  `device=Intel(R) HD Graphics` (hardware, feature level 10_1), 20 frames
  presentados con swapchain flip-discard.
- `embedded.exe --frames 20 --input-demo --resize-demo`: **exit 0**,
  `backend=D3D11` + Intel; el padre redimensiona 900×640 → 720×520 → 900×640,
  el hijo recibe `WM_SIZE`, se reenvían
  `WM_MOUSEMOVE/WM_LBUTTONDOWN/WM_LBUTTONUP`, el click selecciona
  `body.root/div.sidebar/div.nav` y `set_text("text[1]","Texto")` devuelve
  `WN_OK`.
- `-Renderer wgpu`: mismo resultado con `backend=D3D12` +
  `Microsoft Basic Render Driver` (WARP); el camino wgpu sigue intacto.

Timings de la última ejecución D3D11 (Windows 11 x64, Intel HD Graphics):

| Métrica | Valor |
|---|---:|
| Build ABI completo (ambos backends) | ~16.8 s (`cl` + `/W4` D3D11 + header checks + smoke + demos) |
| `standalone` startup (proceso→primer frame) | 42.4 ms |
| `standalone` frame medio | 0.67 ms (Present FIFO/vsync) |
| `standalone` CPU frame (upload+encode+submit) | 0.257 ms |
| `embedded` startup | 54.5 ms |
| `embedded` frame medio | 0.75 ms |
| `embedded` CPU frame | 0.232 ms |
| `standalone.exe` / `embedded.exe` | 345 088 / 345 600 B |
| `runtime_r2.exe` (R2 + core, wgpu offscreen) | 377 344 B |
| R2 regresión (`build.ps1 -SkipRustBuild`) | **69/69 PASS** |

El coste de arranque incluye device/swapchain/HLSL y el primer present. En la
ventana hija embedded `Present(1,0)` también limita a vsync cuando la ventana
es visible; con la ventana ocluida DXGI devuelve inmediatamente.

### 13.6 Paridad del core con R2

Antes de conectar la ABI se volcaron los rects del core a 960×640, 720×640 y
560×640 y se compararon con `build/layout_report.json` nodo a nodo:
**0 diferencias** en `x/y/w/h`, y **84 vértices + 210 glyph vertices** en los
tres tamaños, igual que R2. El core reutiliza el mismo algoritmo de bloque/flex,
wrap con shrink ≤20 %, hit testing inverso y prioridad
press > hover > selected > background.

### 13.7 Límites y gaps restantes (v0)

1. **Sin parser HTML/CSS en runtime**: `wn_view_load`/`wn_app_load` copian el
   documento, pero el árbol que se pinta es el `ui_data.h` compilado. Cargar
   otro HTML exige regenerar `ui_data.h` y recompilar (o un parser/estilo en
   runtime, fuera de v0).
2. **Texto**: sin shaping/wrapping/bidi/fallback en runtime. `set_text` solo
   reutiliza glyphs pre-horneados del atlas para el mismo `font-size`; no
   cambia el layout. Sin selección, IME ni portapapeles.
3. **Teclado**: se traducen `WM_KEYDOWN/UP/CHAR`, pero el motor v0 no tiene
   foco/navegación por teclado ni edición de texto. Esc cierra la ventana
   standalone.
4. **Accesibilidad**: no hay UIA/ARIA, roles ni árbol accesible. Fase 4.
5. **Una vista/ventana por instancia**: no hay multi-documento ni multi-ventana
   por handle. Una instancia por hilo de UI.
6. **Sin invalidación incremental**: cambiar estado/texto reconstruye toda la
   display list; un resize re-layouta todo el documento.
7. **Scroll/clipping**: desde ABI v2 el core soporta scroll vertical y scissor
   por contenedor; `wn_view_event` reenvía `WN_EVENT_WHEEL` al core. Sigue sin
   scroll horizontal ni barras.
8. **DPI**: `dpi_scale` se guarda, pero el layout trabaja en píxeles de cliente
   y no hay escalado por monitor.
9. **Offscreen/PNG**: no forma parte de la ABI; sigue disponible en el CLI de
   `runtime_r2.exe` (`--render`, `--states`, `--report`).
10. **Tablas `ui_data.h` duplicadas en el binario R2**: `runtime_r2.c` y
    `wn_core.c` incluyen el header generado en TUs distintas, así que el
    `.exe` lleva dos copias de las tablas estáticas (~78 KB extra; 377 KB
    total). No hay lógica duplicada: el motor es solo `wn_core.c`. Unificarlo
    en un único TU es un refactor menor posterior.
11. **Doc vs header**: `docs/abi.md` muestra `WN_EVENT_CLICK`, que no existe en
    el header v0 congelado. Las demos usan `WN_EVENT_POINTER_UP`; si se quiere
    un evento click explícito, sería un cambio aditivo de ABI a decidir.
12. **Sin bindings C#/WPF/WinForms** todavía: solo demos C y host Win32.

### 13.8 Por qué no rompe R1 ni R2

- No se tocó `src/runtime/c-static`, `src/build.ps1`, `benchmarks/**`,
  `docs/**` ni `src/frontend/rust/**`.
- `runtime_r2.c` quedó reducido a CLI, reporting y Win32 (más tarde también
  perdió su renderer: hoy pinta por la vtable); el motor vive
  en `wn_core.c` y se compila desde `build.ps1` (que ahora añade `wn_core.c` y
  `wn_common.c` al binario R2). La aceptación se re-ejecutó con
  `-SkipRustBuild` y sigue en **69/69**.
- La capa ABI es un conjunto de fuentes nuevo (`wn_*.c/h`, `renderer_wgpu.c`,
  `host_win32.c`) compilado por `build_abi.ps1`; comparte `wn_core.c` con R2.

---

## 14. ui-tree/2: grid, scroll vertical e imágenes

Contrato congelado en `docs/ui-tree-v2.md`. El emisor acepta `…/1` (páginas
existentes, sin cambios de layout) y `…/2`. R2 y la capa ABI consumen el mismo
`ui_data.h`; todo lo nuevo es aditivo y el fixture v1 conserva **69/69**.

### 14.1 Campos nuevos de `UiNode`

| Campo | Tipo | Valores |
|---|---|---|
| `kind` | enum | `R2_NODE_GRID`, `R2_NODE_IMAGE` (además de block/flex/text) |
| `grid_mode` | int | `R2_GRID_NONE`, `R2_GRID_AUTO_FILL` |
| `grid_column_px` | float | tamaño del track en `repeat(auto-fill, <px>)` |
| `grid_auto_rows` | float | px, o `-1` = auto (máximo de la fila) |
| `row_gap`, `column_gap` | float | gaps resueltos (specific o `gap`) |
| `overflow` | int | `VISIBLE`, `HIDDEN`, `SCROLL` (`auto`/`scroll`) |
| `image_width`, `image_height` | float | tamaño intrínseco PNG/JPEG (0 = desconocido) |
| `image_src`, `image_alt` | string | ruta local; nunca red |
| `object_fit` | int | `FILL`, `COVER`, `CONTAIN` |

El emisor (`emit_ui.py --ui-json`) además resuelve el tamaño intrínseco de
PNG/JPEG en build time (cabeceras IHDR/SOF, sin PIL) y lo escribe en
`image_width/image_height`; el runtime lo usa si faltan `width_px/height_px`.
`ui_meta.json` incluye `grid` e `image` por nodo para el verificador.
`grid_auto_rows` acepta `240`, `"240px"` o `{"px":240}`. Las cadenas no ASCII
se emiten como UTF-8 con escapes octales de 3 dígitos (`"\303\227"`), sin la
ambigüedad de `\xNN` seguido de un dígito hex.

### 14.2 Display list y renderer

`wn_display_list` añade:

- `solid_batches[]` / `glyph_batches[]`: `{first, count, clip_x/y/w/h}`.
  El core agrupa vértices consecutivos con el mismo clip efectivo.
- `image_vertices[]` (`vec2 pos + vec2 uv`, 16 B) e `image_batches[]`
  (`{first, count, clip, src}`); una imagen = 6 vértices.

Los dos backends (`renderer_wgpu.c` y `renderer_d3d11.c`) aplican
`SetScissorRect` por batch antes de cada draw, en este orden: sólidos →
imágenes → glyphs. El atlas de texto y las imágenes comparten el bind group
layout `texture_2d<f32> + sampler`; cada imagen tiene su textura RGBA8 y bind
group. WIC decodifica PNG/JPEG a RGBA8 con caché por ruta; las filas se
padding a 256 B antes de `QueueWriteTexture`. Se enlaza `windowscodecs.lib`.

Con clip = viewport (página v1) hay un único batch y el dibujo es idéntico al
anterior: 84 vértices sólidos + 210 de glyph, una draw call cada uno.

### 14.3 Scroll y wheel

- Estado `scroll_y` y `scroll_max` por nodo. `layout_node` desplaza los hijos
  del contenedor y `compute_clips` propaga el clip (content box ∩ ancestros ∩
  viewport). El hit testing exige punto dentro de rect **y** clip.
- `wn_core_wheel(core, x, y, dx, dy)` busca el ancestro con `overflow:scroll`
  bajo el puntero, aplica `dy`, hace clamp y re-ejecuta layout. Devuelve el
  nodo contenedor o `-1`.
- ABI v2: `WN_EVENT_WHEEL = 10`, `delta_x/delta_y` al final de `wn_event`,
  `WN_ABI_VERSION = 2`. `host_win32.c` traduce `WM_MOUSEWHEEL` (1 notch =
  120 px) y `WM_MOUSEHWHEEL`; `wn_view_event` lo enruta y `wn_app` lo reenvía.
- CLI de referencia: `--wheel X Y DY` antes de `--render`, y
  `scroll: node=… offset=… max=…` en la salida para verificación.

### 14.4 Evidencia (última ejecución)

`tests/pilot-media-library/run_pilot.ps1` termina con **38/38** y compila
también la variante ABI (ventana, `renderer_wgpu.c`) con 20 frames y D3D12.
Página: header, contenedor `overflow:scroll` 1232×420 con padding 16, grid
`repeat(auto-fill,180px)` gap 16 y 18 imágenes 180×240 (temas warm/cool/purple,
fuente 120×90).

| Comprobación | Resultado |
|---|---|
| Columnas 1280×800 | **6** (x = 40, 236, 432, 628, 824, 1020) |
| Columnas 800×600 | **3** (x = 40, 236, 432) |
| Filas | row-major, pitch 240+16 = 256 |
| `object-fit: cover` | llena la caja y recorta el source en horizontal |
| `object-fit: fill` | estira las 3 bandas a todo el ancho |
| `object-fit: contain` | 180×135 centrado, letterbox `#1a1f2a` arriba/abajo |
| Clip | el footer y el gap bajo el contenedor no reciben píxeles del grid |
| Wheel 800 (+240) | offset 0 → **240** (max 1132); 263 520 px cambian solo dentro del clip |
| Wheel 1280 (+200) | offset 0 → **200** (max 364); 985 740 px cambian solo dentro del clip |
| Wheel clamp (+4000) | offset → **1132 = max**; footer/gap intactos |
| Wheel fuera del contenedor (400,560) | 0 px de cambio; runtime informa "no scrollable container" |
| Imágenes | 18 batches × 6 = 108 vértices; caché WIC por ruta |
| Timings (VM WARP) | layout 0.007–0.010 ms; frame offscreen 87–158 ms (readback/PNG) |
| ABI ventana | `abi_pilot.exe --frames 20` exit 0, D3D12, 17.3 ms/frame |
| Piloto benchmark (301 nodos/30 posters) | **48/48 PASS** con build fresco (5 columnas a 1280, 3 a 800, wheel/clamp) |

El piloto de `benchmarks/pilot-media-library` (fixture de 30 posters, texto
pre-shapeado y `grid` con `overflow:auto`) se compiló contra estas fuentes y se
verificó con su `verify.py` redirigiendo la salida fuera del benchmark
(`--out`); reporte y PNGs copiados a `out/benchmark/` como evidencia. Confirma
la compatibilidad con el contrato real del piloto, incluido el clamp y el
`object-fit: cover`.

`build.ps1 -Pilot -SkipRustBuild` es el camino canónico: emite con
`frontend-rust --pilot` a `build_pilot\ui.json`, compila allí el runtime y
ejecuta el `verify.py` del benchmark con `--out build_pilot\verify`.
Resultado de la última ejecución: **48/48**, 1280×800/800×600, grid 5/3
columnas, clamp 1248/2792 y wheel de 100 px, sin el workaround
`tools/split_c_escapes.py`.

Artefactos: `tests/pilot-media-library/out/render_1280.png`,
`render_800.png`, `wheel_1280.png`, `wheel_800.png`, `wheel_max.png`,
`wheel_outside.png`, `pilot_layout.json`, `pilot_verify_report.json`,
`pilot_report.json`, `abi_pilot.log`.

### 14.5 Límites v2 (honestos)

1. **Solo scroll vertical**: `delta_x` se ignora; sin scroll horizontal,
   momentum, scrollbars dibujadas ni ruedas anidadas reales.
2. **Solo `repeat(auto-fill, <px>)`**: sin `fr`, `minmax`, `auto-fit`, `span`,
   `grid-area`, alineación por celda ni grid anidado. El track no se estira:
   el espacio sobrante queda a la derecha.
3. **Solo imágenes locales** (PNG/JPEG vía WIC): sin red, `srcset`, lazy
   loading, animación ni SVG. La ruta relativa se resuelve contra el CWD del
   proceso; el piloto copia `posters/` junto al ejecutable/lanzador.
4. **Tamaño intrínseco en build time**: si la imagen no existe al emitir,
   `image_width/height` quedan 0 y el layout usa `width/height` o el content
   box. El runtime no re-layouta a partir del tamaño WIC.
5. **Orden de pintado global**: sólidos → imágenes → texto; no hay stacking
   contexts ni z-index. En el piloto las imágenes no se solapan con texto.
6. **El clip se aplica por batch consecutivo**, no se ordenan draws por clip;
   con muchos clips intercalados aumentan las draw calls, no la corrección. La
   caché de imágenes (WIC → RGBA8) no tiene evicción: una textura por ruta
   única durante la vida del renderer.
7. **Wheel en ventana**: `runtime_r2.exe` y `host_win32.c` lo mapean. La demo
   `abi_pilot.exe --wheel-demo` envía un `WM_MOUSEWHEEL` real y verifica que el
   callback recibe `target=body.root/div.scroll` (host → app → view → core);
   los píxeles desplazados se verifican en los renders offscreen del piloto.

### 14.6 Escaping C de texto no-ASCII

`emit_ui.py` escribe los bytes UTF-8 con escapes **octales de 3 dígitos**
(`"3840\303\2272160"`, `"Geometr\303\255a"`), no con `\xNN`. Así el escape
nunca absorbe un dígito hexadecimal siguiente (el bug `\xd72160` /
`C7744: escape sequence out of range`). `tests/emitter-escapes/run_test.ps1`
emite un `ui.json` con `×`, `í`, `Ó` y `ñ`, compila un `.c` que incluye el
header y compara los bytes UTF-8; ya no hace falta el workaround
`benchmarks/pilot-media-library/tools/split_c_escapes.py`.

## 15. UI dinámica v0 (ABI v3)

La API de contenido reactivo permite que el host cambie el árbol compilado sin
recompilar ni volver a emitir `ui_data.h`: visibilidad, imagen, texto, clonado
de plantillas, re-parentado y borrado. Es **aditiva** sobre la ABI v2: los
structs, eventos y funciones existentes no cambian; `wn_event` sigue teniendo el
mismo tamaño.

### 15.1 Handles y búsqueda

`wn_node_id` es un `uint64_t` opaco, 1-based (`0 == WN_NODE_INVALID`). El core
guarda un arena mutable de hasta 4096 nodos: los nodos compilados se copian al
crear el core y los clones se asignan desde una free list.

```c
wn_node_id root  = wn_app_root(app);                 /* body.root */
wn_node_id cards = wn_app_find(app, "body.root/div.cards");
wn_node_id card  = wn_app_find(app, "body.root/div.cards/div.card");
int32_t live     = wn_app_node_count(app);           /* 17 en el fixture */
```

`wn_app_find` resuelve **nombres compilados**: primero el nombre completo y,
si no hay match, un último componente de ruta único (`div.card`, `text[1]`).
Los handles devueltos por `wn_node_clone` no se buscan por nombre en v0; hay que
conservarlos.

### 15.2 API

```c
/* Visibilidad: excluye el subárbol de layout, paint y hit-test. */
wn_node_set_visible(app, card, 0);
wn_node_set_visible(app, card, 1);

/* Imagen: reemplaza image_src; reutiliza la caché de texturas por ruta. */
wn_node_set_image(app, image, "images/b.png");

/* Texto: solo strings ya pre-shaped para ese font-size en el atlas compilado. */
wn_node_set_text(app, label, "Beta");      /* WN_OK */
wn_node_set_text(app, label, "Ω");         /* WN_ERR_UNSUPPORTED + wn_last_error() */

/* Plantilla/lista. */
wn_node_id copy = wn_node_clone(app, card, cards);  /* deep clone, último hijo */
wn_node_append(app, cards, copy);                   /* re-parenta al final */
wn_node_clear_children(app, cards);
wn_node_remove(app, copy);
```

Reglas de error: handles inválidos o muertos devuelven
`WN_ERR_INVALID_ARGUMENT`; `wn_node_clone` devuelve `WN_NODE_INVALID` y deja el
detalle en `wn_last_error()`; el root del documento no se puede borrar ni mover;
un `append` que crearía un ciclo se rechaza. `wn_node_set_image` sobre un nodo
que no es `NODE_IMAGE` devuelve `WN_ERR_UNSUPPORTED`.

### 15.3 Invalidación

Toda mutación marca el core `dirty` y `needs_layout`. El siguiente
`wn_view_render()` (o frame de `wn_app_run`) re-ejecuta layout, clipping, hit
testing y display list **antes** de subir geometría. El host no llama a
`relayout` a mano. Resize y wheel conservan su comportamiento v2; el wheel sigue
buscando el contenedor `overflow:scroll` subiendo por los parent links del arena.

El layout ignora nodos ocultos al medir y al colocar (flex, grid, block y
scroll), y el paint/hit recorren un `paint_order` DFS que solo contiene nodos
visibles. Ocultar una card reflowa sus hermanas; volver a mostrarla restaura la
geometría y el display list exactamente.

### 15.4 Clonado profundo

`wn_node_clone` copia el subárbol completo: `UiNode` (estilos, márgenes,
padding, colores, `action`, `hover/press/selected`, `image_*`, `data-*`
reflejados en `UiNode`), el rango de runs de texto pre-shaped, el `image_src` y
los overrides runtime (`set_text`/`set_image`) del template. El clon raíz se
inserta como último hijo (`order_key` monótono, no el índice del arena), así que
el paint order sigue siendo correcto aunque se reciclen slots. Los nombres
internos son `clone#N` y son únicos.

El borrado libera el subárbol y sus strings, limpia hover/press/selected si
apuntaban dentro y devuelve los slots a la free list. `wn_node_clear_children`
hace lo mismo con cada hijo sin tocar el contenedor.

### 15.5 `set_image`: caché y límites

El renderer decodifica PNG/JPEG con WIC y cachea **por ruta** durante la vida
del renderer (`image_cache` en el reporte de test se mantiene en 2 al alternar
`images/a.png` y `images/b.png`). Cambiar `image_src` no duplica geometría: el
nodo sigue emitiendo 6 vértices. La ruta se resuelve contra el CWD como en v2.

Limitación v0: el tamaño intrínseco (`image_width/height`) sigue siendo el
compilado; no se sondea el nuevo archivo con WIC ni se re-layouta el aspect
ratio. `object-fit` usa las dimensiones viejas hasta que se re-emita el fixture.

### 15.6 `set_text`: shaping en build time

No hay shaping en runtime. `wn_core_set_text` acepta el string solo si cada byte
no espacio tiene un `GlyphEntry` válido en el `GlyphSet` del `font-size` del nodo
(construido con los runs compilados). Si no, devuelve `WN_ERR_UNSUPPORTED` con
un mensaje que incluye el nodo. Tampoco se recalculan métricas/width: el nodo
conserva su caja compilada y el texto se pinta con los avances pre-shaped. Un
string vacío borra el texto visible.

Esto cubre el caso “cambiar una etiqueta por otra del mismo atlas” (por ejemplo
`Alpha` → `Beta`) pero no contenido arbitrario, i18n en runtime ni text input.

### 15.7 Demo y tests

```powershell
# R2 canónico (69/69), piloto (48/48) y ABI v0 siguen igual:
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build.ps1 -Pilot
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build_abi.ps1

# Dynamic UI v0: core headless + 11 renders offscreen + demo ABI + 53 checks:
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\dynamic\run_dynamic.ps1
```

El fixture ([tests/dynamic](tests/dynamic)) es una página de 400×300 con dos
cards de 120×120 (imagen 104×60 + label) y `data-action="select"`. El script:

1. `dynamic_core_test.exe` (solo `wn_core.c`) valida búsqueda, geometría,
   hit-testing, conteos, clone/append/remove/clear, `set_image`, `set_text` y
   los vertex counts de la display list.
2. `runtime_dynamic.exe --dynamic out\stage` renderiza 11 PNGs:
   inicial, hide, show, clone, imagen del clon, texto del clon, remove, append,
   imagen del template, restore y clear. `stage_report.json` registra rects,
   hits, conteos y `image_cache`.
3. `dynamic.exe` (demo C ABI) crea una ventana real, ejecuta la misma secuencia
   vía `wn_app_*` y auto-cierra a los 8 frames.
4. `verify_dynamic.py` comprueba geometría, conteos y píxeles: escenario
   **53/53 PASS** con diff bboxes exactos
   (`hide=(18,50,260,170)`, `clone=(270,50,390,170)`,
   `set_text=(278,124,314,137)`, `set_image=(18,58,122,118)`), igualdad
   byte-a-byte tras show/remove/append/restore y ausencia de píxeles de card
   tras `clear_children`.

Evidencia en `tests/dynamic/out/`: `stage_00.png` … `stage_10.png`,
`stage_report.json`, `core_events.jsonl`, `dynamic_verify_report.json`,
`runtime_dynamic.log`, `abi_dynamic.log` y `run_report.json`.

### 15.8 Gaps restantes

1. **Texto dinámico real**: no hay shaping runtime (HarfBuzz/FreeType), métricas
   nuevas, wrapping ni reflow por longitud. `set_text` es un override de
   glifos pre-shaped; no crea runs ni actualiza `width/height`.
2. **Input**: resuelto para single-line ASCII en la sección 16. Siguen fuera
   IME, bidi, password, undo/redo, multilínea y drag de texto.
3. **Virtualización/reciclado**: clonar/borrar es O(subárbol) y la arena es fija
   (4096 nodos, error claro al agotarse). No hay pooling de items visibles,
   `key`/reconciliación, lazy lists ni reciclado de texturas.
4. **Animación/transiciones**: no hay timeline, spring, layout animation ni
   repintado por frame sin mutación. Cada mutación fuerza layout completo.
5. **HTML/CSS en runtime**: el árbol sigue siendo el compilado; no se parsea
   contenido nuevo. Los nombres de clones no son direccionables por
   `wn_app_find` (solo handles).
6. **Imágenes**: sin `srcset`, red, SVG, animación, sondeo de tamaño runtime ni
   evicción de caché. `set_image` mantiene las dimensiones intrínsecas
   compiladas.
7. **Orden de pintado**: sigue siendo sólidos → imágenes → texto y sin
   `z-index`/stacking contexts; el arena garantiza el orden de hijos, no capas
   arbitrarias.

---

## 16. Text input v1 (`ui-tree/3`, ABI v4)

Input de texto **de una línea, ASCII, sin IME y sin shaping en runtime**. El
frontend hornea una piscina ASCII imprimible por cada tamaño de fuente; el core
mantiene el buffer de edición, caret y selección, y compone el texto sumando
avances. El host ve eventos semánticos (`WN_EVENT_INPUT`) y puede leer/escribir
el valor con la ABI. Es la base de la búsqueda del piloto de biblioteca.

### 16.1 Schema `webnative.ui-tree/3`

`/3` es **aditivo sobre `/2`** (hereda todos sus campos de estilo). Solo se emite
cuando la página tiene al menos un nodo `kind:"input"`; las páginas `/1` y `/2`
conservan exactamente sus bytes y su schema. Un `<input>` dentro de
`display:none` no cuenta.

```json
{
  "id": 2,
  "parent": 1,
  "children": [],
  "kind": "input",
  "name": "body.root/div.bar/input#search.search",
  "tag": "input",
  "element_id": "search",
  "classes": ["search"],
  "style": {
    "display": "block",
    "width_px": 320.0,
    "height_px": 34.0,
    "padding": [7.0, 9.0, 7.0, 9.0],
    "font_size": 20.0,
    "color": "#f2f5f8",
    "background": "#1c2430"
  },
  "text": null,
  "input": {
    "value": "",
    "placeholder": "Buscar...",
    "max_length": 12,
    "password": false,
    "baseline": 22.0,
    "line_height": 28.0,
    "glyphs": [
      { "cp": 32, "gid": 3, "advance": 5.484375, "x": 0.0, "y": 22.0,
        "w": 0, "h": 0, "u0": 0.0, "v0": 0.0, "u1": 0.0, "v1": 0.0 },
      { "cp": 97, "gid": 68, "advance": 10.171875, "x": 0.0, "y": 12.0,
        "w": 9, "h": 10, "u0": 0.449, "v0": 0.041, "u1": 0.466, "v1": 0.060 }
    ]
  }
}
```

| Campo | Regla |
|---|---|
| `kind` | `"input"`; nodo hoja (void, sin hijos). |
| `value` | Valor inicial, solo ASCII imprimible (32..126). El frontend descarta el resto y recorta a `maxlength`. |
| `placeholder` | Texto de ayuda; se pinta cuando `value` está vacío, con color atenuado. ASCII. |
| `max_length` | 1..255; `0` o ausente = sin límite (tope duro 255). |
| `password` | Reservado; siempre `false` en v1. |
| `baseline` | Ascender en px desde el top del content box. |
| `line_height` | Ascender + descender; altura por defecto si el CSS no fija `height`. |
| `glyphs[]` | Piscina ASCII **completa y ordenada** 32..126 (95 entradas), ver §16.2. |

HTML aceptado (extensión del piloto v2; sin `--pilot` avisa y `--strict-subset`
falla):

```html
<input id="search" type="text" placeholder="Buscar..." value="abc" maxlength="12">
```

- `type` ausente o `text`; cualquier otro tipo se emite como bloque normal y el
  subset avisa (no hay checkbox/radio/password en v1).
- `disabled`/`readonly` se aceptan y se ignoran: el widget v1 es siempre
  editable (el host decide no enfocarlo).
- `value`/`placeholder` no ASCII: se descartan y el subset avisa.
- `maxlength` inválido se ignora.
- Sin `width`/`height` CSS: ancho por defecto 200 px (o estimado por
  `maxlength`, 80..480) y alto = padding + línea de la fuente.
- R1 (`--out` sin `--emit-ui`) no edita ni compone texto: imprime
  `warning[r1] ... <input> es el widget text input de R2`.

### 16.2 Piscina ASCII y atlas

`emit_ui.py --ui-json` copia la piscina del `ui.json` a dos tablas del header:

```c
typedef struct {
    int cp; int gid;
    float advance;
    float x, y, w, h;        /* caja relativa al content box */
    float u0, v0, u1, v1;    /* UVs del atlas R8 */
} UiInputGlyph;

typedef struct {
    int node;
    const char *value;
    const char *placeholder;
    int max_length;
    int password;
    float baseline, line_height;
    int glyph_first, glyph_count;
} UiInput;

#define UI_INPUT_COUNT n
#define UI_INPUT_GLYPH_COUNT m
```

- Una piscina por `(familia, tamaño)` usada por los inputs; el atlas deduplica
  por `(path, gid, size)` y `emit_ui.py` deduplica bloques idénticos.
- Las páginas con input usan un atlas **512×512** (la piscina son ~95 glyphs por
  tamaño). Las páginas sin input siguen en 256×256, byte-idénticas.
- `space` (32) tiene `w = h = 0` pero `advance > 0`; el core solo avanza el pen.
- **Sin shaping en runtime**: nada de HarfBuzz/FreeType en el core. El ancho de
  una cadena es la suma de avances aislados, así que los pares con kerning
  (`AV`) pueden diferir del shaping de un nodo de texto normal. Es una decisión
  de contrato, no un bug.
- El core copia las tablas a `WnInputRecord`/`WnInputGlyph` y construye
  `input_map[node] -> piscina`; `input_record()` es la única puerta. Un clone
  comparte la piscina del template y recibe su propia copia del valor.
- Buffer de edición: `255 + 1` bytes por input, asignado al crear; el caret es
  un offset de byte (ASCII: bytes == caracteres).
- El modo oráculo `emit_ui.py --html/--css` **no** soporta input (no hornea la
  piscina): aborta con un mensaje. El camino válido es `frontend-rust
  --emit-ui --pilot` + `emit_ui.py --ui-json`.

### 16.3 ABI v4

`WN_ABI_VERSION` pasa a **4**. Todo es aditivo: `wn_event` conserva tamaño,
orden y semántica.

```c
#define WN_ABI_VERSION 4u

typedef enum wn_event_type {
    /* ... v0..v2 sin cambios ... */
    WN_EVENT_WHEEL = 10,
    WN_EVENT_INPUT = 11
} wn_event_type;

WN_API wn_status wn_node_set_value(wn_app *app, wn_node_id node, const char *utf8);
WN_API const char *wn_node_get_value(wn_app *app, wn_node_id node);
```

- `WN_EVENT_INPUT`: `target_id` es el input; el valor nuevo se lee con
  `wn_node_get_value()` **dentro del callback**. Se emite en cada cambio de
  valor provocado por el usuario (CHAR, Backspace/Delete, paste/cut). No se
  emite por mover el caret/selección, por foco/blur ni por
  `wn_node_set_value()` (el caller ya conoce el valor).
- `wn_node_set_value`:
  - `WN_OK` y caret al final, selección limpia, sin evento.
  - `WN_ERR_UNSUPPORTED` si hay un byte fuera de 32..126 (no hay shaping/IME).
  - `WN_ERR_INVALID_ARGUMENT` si supera `maxlength` (o 255) o el nodo no es un
    input.
  - El valor anterior se conserva si falla.
- `wn_node_get_value`: string **del engine**, válido hasta la siguiente llamada
  webnative en ese hilo; `NULL` + `wn_last_error()` para un nodo inválido. No
  reserva memoria nueva en cada llamada (buffer scratch del view).
- Handles: `wn_app_find` resuelve nombres compilados completos
  (`body.root/div.bar/input#search.search`). El sufijo de clase forma parte del
  nombre; usa el valor de `ui_meta.json` o el `element_id` si es único.
- En v4 la API de valor es **app-level** (igual que la UI dinámica v3). Un host
  embedded recibe `WN_EVENT_INPUT` y puede mantener su propio modelo; las
  funciones `wn_view_set_value`/`wn_view_get_value` existen como superficie
  interna (`wn_view.h`) para tests/demos, no están en `include/webnative.h`.

### 16.4 Teclado, foco y clipboard

| Entrada | Comportamiento |
|---|---|
| Click en el input | Enfoca y coloca el caret en el límite más cercano del click. Click fuera: blur. |
| ASCII imprimible (`WM_CHAR`) | Inserta en el caret; si hay selección, la reemplaza. |
| `Backspace` / `Delete` | Borra selección o el carácter anterior/siguiente. |
| `←` / `→` | Mueve el caret; con selección, colapsa al extremo. |
| `Shift+←/→/Home/End` | Extiende la selección desde el ancla. |
| `Home` / `End` | Inicio/fin del valor. |
| `Ctrl+A` | Selecciona todo. |
| `Ctrl+C` / `Ctrl+X` | Copia/corta la selección a `CF_UNICODETEXT` (Win32). |
| `Ctrl+V` | Pega ASCII imprimible hasta `maxlength` (trunca el resto). |
| `Esc` | Suelta el foco sin borrar el valor. |
| `Enter` | **No** lo consume el engine (no hay formularios/submit en v4); el host decide. |
| `Tab` | No lo consume el engine; el host gestiona el orden de foco. |

- **Esc y cierre**: `host_win32.c` ya no cierra con Esc. `wn_app.c` cierra solo
  si **ningún** input tiene foco; con foco, reenvía el evento y el view hace
  blur. `runtime_r2.exe` aplica la misma regla en su `WM_KEYDOWN`.
- El cursor horizontal auto-scroll mantiene visible el caret; el texto se
  recorta al content box y la línea se centra verticalmente (si el content box
  es más bajo que la línea, se recorta simétricamente).
- Colores fijos v1: anillo de foco `#3b82f6` (2 px), selección
  `#3b82f6` al 40 %, placeholder `#6b7785`. El caret usa el `color` del nodo.
- `WM_KILLFOCUS` (ventana sin foco) también hace blur.
- El caret es **fijo** (sin parpadeo) en v1 para que los PNG de aceptación sean
  deterministas. El blink es una mejora de host/tiempo, no de layout.
- El placeholder se pinta en el pase de glyphs (encima del caret); el caret
  sigue siendo visible a los lados/arriba del glyph.

### 16.5 Demo y tests

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\input\run_input.ps1
# re-emitir page\ui.json + fixture\ui_data.h (frontend-rust --pilot + --ui-json):
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\input\run_input.ps1 -Regenerate
```

Cuatro capas contra `tests/input/page` (480×240, dos inputs: `search` 320×34
con `maxlength=12` y `empty` 220×28 con `maxlength=5`):

1. `input_core_test.exe` (solo core): lookup, layout, click/caret, typing,
   inserción en el caret, Backspace/Delete, Home/End, selección Shift,
   copy/cut/paste con buffer, `maxlength`, `set_value`/`get_value` (rechazos),
   blur con Esc, cambio de foco y clone independiente.
2. `runtime_input.exe --input out\stage`: 10 PNGs offscreen deterministas.
   `stage_report.json` registra por stage `value`, `cursor`, `anchor`,
   `focused`, `caret_x`, `selection_x0/x1`, `scroll`, rects, baseline y
   `glyph_vertices`.
3. `input.exe` (ABI v4): `set_value`/`get_value`, rechazos, `WN_EVENT_INPUT`
   con lectura dentro del callback, typing/Backspace, Home+Shift+Right,
   `Ctrl+C/X/V` reales, Esc blur y la política "Esc no cierra con foco".
4. `runtime_input.exe --frames 16 --input-demo --width 480 --height 240`:
   ventana Win32 real; el demo sintetiza `WM_LBUTTONDOWN/UP`, `WM_CHAR 'a'/'b'`
   y `VK_BACK` a través del `wnd_proc` y registra `value="a" focused=1`.

`verify_input.py` comprueba valores/cursores/anchors, píxeles de placeholder,
texto, anillo, caret y selección, y los logs. Evidencia en
`tests/input/out/`: `stage_00.png` … `stage_09.png`, `stage_report.json`,
`core_events.jsonl`, `runtime_input.log`, `window_input.log`, `abi_input.log`,
`input_verify_report.json` y `run_report.json`.

Resultado de la última ejecución: **111/111 checks PASS**, 0 fallos.

### 16.6 Limitaciones (honestas)

1. **Solo ASCII imprimible (32..126)**. No hay IME, composición, bidi, CJK,
   emoji ni shaping en runtime; el host debe filtrar/convertir la entrada.
2. **Sin wrapping ni multilínea**: un input es una línea; el texto sobrante se
   recorta y se desplaza en horizontal.
3. **Sin password real**: `password` está en el schema pero siempre `false`;
   `type="password"` no se emite como input.
4. **Sin undo/redo** ni `Ctrl+Z/Y`; tampoco `Ctrl+←/→` (palabra),
   `Ctrl+Backspace`, drag-select, doble-click-select ni autoscroll por drag.
5. **Caret sin blink** y sin animación; selección de un solo rango.
6. **Sin formularios**: no hay `submit`, validación, `change`/commit ni
   autofill. `Enter` queda para el host.
7. **Sin kerning en la composición** (avances aislados de la piscina).
8. **Tope 255 bytes** por input (además de `maxlength`), un `value` por nodo.
9. **Atlas 512×512**: caben ~4-5 tamaños de fuente de input por página; si se
   desborda, el frontend falla con un error claro (no hay atlas múltiple).
10. **API de valor app-level**: el host embedded no tiene `wn_node_get_value`;
   usa el evento y su propio modelo (o la superficie interna de `wn_view.h`).
11. **Sin accesibilidad/UIA** todavía (rol textbox, valor, selección).

### 16.7 Integración con el host C# (WPF/WinForms)

1. **Compila el árbol con input**: `frontend-rust --emit-ui --pilot` y
   `emit_ui.py --ui-json`. El `ui_data.h` resultante expone
   `R2_NODE_INPUT`, `kUiInputs` y `kUiInputGlyphs`; el runtime debe compilarse
   contra ese header (no contra el fixture por defecto).
2. **P/Invoke** (ABI v4, UTF-8, `PublishAot` o JIT):
   ```csharp
   [LibraryImport("webnative", StringMarshalling = StringMarshalling.Utf8)]
   private static partial nint wn_app_create(in wn_config config);

   [LibraryImport("webnative", StringMarshalling = StringMarshalling.Utf8)]
   private static partial wn_status wn_node_set_value(nint app, ulong node, string value);

   [LibraryImport("webnative")]
   private static partial nint wn_node_get_value(nint app, ulong node);
   ```
   `wn_node_get_value` devuelve un puntero del engine: conviértelo con
   `Marshal.PtrToStringUTF8` **inmediatamente** (válido hasta la siguiente
   llamada webnative en ese hilo).
3. **Callback de eventos**: registra `wn_app_on_event`. En `WN_EVENT_INPUT (11)`
   lee `wn_node_get_value(app, node)` y actualiza el binding de la UI; no
   vuelvas a llamar a `set_value` desde el callback (bucle).
4. **Reenvía teclado**: `KeyDown` → `wn_event.type = WN_EVENT_KEY_DOWN`,
   `key = (int)KeyInterop.VirtualKeyFromKey(e.Key)`, `modifiers` con
   `WN_MOD_SHIFT/CTRL/ALT`; `TextInput`/`PreviewTextInput` → `WN_EVENT_CHAR` con
   el codepoint (solo 32..126). El engine hace el resto (caret, selección,
   Backspace, Ctrl+A/C/X/V).
5. **Foco**: un `WN_EVENT_POINTER_DOWN` sobre el input lo enfoca y coloca el
   caret; `WN_EVENT_POINTER_UP` completa el click. `Esc` hace blur; cierra la
   ventana solo si tú lo decides cuando no hay foco.
6. **Nombre del nodo**: `wn_app_find(app, "body.root/div.bar/input#search.search")`
   (nombre compilado completo). Si usas el `element_id`, recuerda que el nombre
   incluye `tag#id.clase`; el id suelto no resuelve si hay clase.
7. **Valor**: el engine es la fuente de verdad del buffer de edición. El host
   refleja `WN_EVENT_INPUT`; para cambios programáticos usa `set_value` (no
   emite evento) y refresca el binding.
8. **Clipboard**: no implementes Ctrl+C/X/V en C# si reenvías las teclas al
   engine; `wn_view_event` usa el clipboard del sistema (Win32). Si tu host
   intercepta esos atajos, desactívalos para el input.
9. **Hilos**: todo en el hilo de UI; no reentrar en `wn_app_run`/render desde el
   callback.
10. **Comprueba `wn_abi_version() == 4`** al arrancar y degrada a solo lectura si
    el runtime es anterior.

---

## 17. CSS core v1 (bordes, radius y sombras)

Consume los campos CSS v3 que el frontend Rust ya emitía en `ui.json`
(`border_width_px`, `border_color`, `border_style`, `border_radius_px`,
`box_shadow`) y los pinta con el pipeline sólido existente, sin pipeline GPU
nuevo. `opacity` se ignora en v1.

### 17.1 Campos horneados

`emit_ui.py --ui-json` añade al final de `UiNode`:

```c
float border_width;        /* px, 0 none */
unsigned int border_color; /* 0xAARRGGBB */
float border_radius;       /* px, uniforme (border_radius_px) */
float shadow_x, shadow_y;  /* primer box-shadow no-inset */
float shadow_blur, shadow_spread;
unsigned int shadow_color;
```

`border_style:"none"` fuerza `border_width = 0`. Los campos ausentes quedan a 0,
así que las páginas sin CSS v3 conservan el render rectangular anterior.
`--compare` acepta cabeceras antiguas (sin la cola CSS) y las compara con los
campos nuevos a 0.

### 17.2 Cómo se dibuja

- **Fondos**: `push_box_shape()` usa el quad exacto de 6 vértices si el radio es
  0; si hay radio, tesela un triangle fan con 8 segmentos por esquina
  (`push_rounded_quad()`).
- **Bordes**: si `border_width > 0`, `border_color` tiene alfa y el fondo
  efectivo es opaco, se pinta el rect redondeado exterior en color de borde y
  el interior (inset por `border_width`, radio reducido) con el fondo. Sin
  fondo opaco el borde se omite en v1 (no hay anillo transparente).
- **Sombras**: la primera sombra no-inset se pinta detrás del nodo con 3 capas
  translúcidas rellenas, expandidas por `spread + blur * {1, 0.5, 0}` y
  desplazadas por `shadow_x/y`; no hay blur gaussiano exacto.
- **Imágenes**: un `image` con radio se tesela con UVs interpoladas (el
  `object-fit` se conserva y el batch pasa a tener un número variable de
  vértices). Si el padre tiene `overflow:hidden` y radio, la imagen usa ese
  radio menos el borde del padre (caso de las cards del piloto).
- **Layout**: no cambia; el borde se pinta sobre el border box y los hijos
  pueden solaparlo por el ancho del borde. Es una limitación de v1.

### 17.3 Tests

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\css\run_css.ps1
```

Emite tres cabeceras (`card`, `control`, `none`), compila el runtime con
`/W4 /WX`, renderiza 400x240 por `runtime_r2.exe --render` y comprueba:

- esquinas fuera del radio = fondo de página, centro = fondo de la card,
- los 4 lados del borde en blanco, fondo interior tras el borde,
- sombra visible y con 3 niveles de opacidad decrecientes hacia fuera,
- imagen redondeada por el radio del padre con las 4 UVs correctas,
- borde omitido con fondo transparente y `border_style:none` a 0,
- el control sin campos CSS es **byte-idéntico** al renderer anterior
  (`expected/control_legacy.png`, generado con el código pre-CSS).

Resultado: **41/41 PASS**. `build.ps1` (69/69), `-Pilot` (48/48),
`dynamic` (53/53), `input` (111/111) y `build_abi.ps1` (PASS) siguen igual.

### 17.4 Limitaciones

1. Radio uniforme; sin `border-radius` por esquina (el frontend lo emite, el
   core usa el valor uniforme).
2. Borde uniforme; sin `border_sides` por lado (los estilos `dashed`/`dotted`
   se pintan como sólidos).
3. Solo la primera sombra no-inset; sin `inset`, sin blur gaussiano exacto.
   Las capas son rectángulos rellenos detrás del nodo: con fondo
   semi-transparente la parte interior de la sombra puede verse.
4. Sin `opacity` compuesta.
5. Sin anillo de borde si el fondo es transparente.
6. Sin clipping redondeado general de hijos; solo el caso imagen/padre
   `overflow:hidden`. Un hijo sólido puede tapar las esquinas del padre.
7. Sin scrollbar, `text-align`, `font-weight`, `line-height` ni `margin:auto`;
   siguen siendo gaps de layout/texto.

---

## 18. Backend D3D11 (segundo renderer, Windows)

### 18.1 Objetivo y prueba de la abstracción

`wn_renderer_ops` (sección 13) estaba implementado por un solo backend
(`renderer_wgpu.c`). Esta máquina tiene una Intel HD Graphics que **no expone
D3D12**, así que wgpu seleccionaba WARP (Microsoft Basic Render Driver) y el
coste de rasterizado caía en CPU. `renderer_d3d11.c` implementa **la misma
vtable completa** (`init`, `resize`, `upload`, `render`, `destroy`,
`last_error`, `get_info`, `get_stats`) sobre D3D11 nativo, usa la GPU Intel y
sirve como segunda implementación real de la abstracción: `wn_view.c` elige
backend sin que el core, la display list ni `include/webnative.h` cambien.

### 18.2 Diseño

| Pieza | Implementación |
|---|---|
| Device | `D3D11CreateDevice(D3D_DRIVER_TYPE_HARDWARE)` con feature levels 11_1 → 10_0 y `D3D11_CREATE_DEVICE_BGRA_SUPPORT`; si falla, segundo intento con `D3D_DRIVER_TYPE_WARP` y se registra `driver=warp`. |
| Superficie | DXGI desde el HWND: `CreateSwapChainForHwnd` con `DXGI_SWAP_EFFECT_FLIP_DISCARD`, 2 buffers, `DXGI_FORMAT_B8G8R8A8_UNORM`, `DXGI_SCALING_STRETCH`, alpha ignore. Fallback a `CreateSwapChain`/`DISCARD` si el flip model no está disponible. |
| Present | `IDXGISwapChain::Present(1, 0)` (vsync). `ResizeBuffers` + recreación de backbuffer/RTV; si la ventana está ocluida DXGI puede devolver inmediatamente. |
| Shaders | HLSL embebido compilado en runtime con `D3DCompile` (`vs_5_0/ps_5_0`, o `4_0` si el feature level es 10_x). Sin ficheros aparte ni `fxc` en build time. |
| Sólidos | `wn_vertex` 12 B: `R32G32_FLOAT` posición NDC + `R8G8B8A8_UNORM` color. Un solo buffer dinámico, scissor por `wn_draw_batch`. |
| Texto | `wn_glyph_vertex` 20 B: posición + UV + color; atlas `R8_UNORM` y `float4(color.rgb, color.a * atlas.r)`; sampler lineal clamp. |
| Imágenes | `wn_image_vertex` 16 B: posición + UV; WIC PNG/JPEG → `R8G8B8A8_UNORM` + SRV, caché por ruta sin evicción. |
| Estado | Blend `SrcAlpha/OneMinusSrcAlpha` (alpha `One/OneMinusSrcAlpha`), cull none, `ScissorEnable`. Mismo orden que wgpu: sólidos → imágenes → glyphs. |
| Coordenadas | Posiciones ya en NDC con el mismo convenio que WebGPU (+Y arriba), así que no hay flip de Y; los UV también coinciden. Scissor con `floor/ceil` y clamp a la vista, igual que `set_scissor` de wgpu. |
| Stats/info | `backend="D3D11"`, `device` desde `DXGI_ADAPTER_DESC1.Description` (p. ej. `Intel(R) HD Graphics`), `format=DXGI_FORMAT_B8G8R8A8_UNORM`, frames/CPU/present. |
| Captura (tests) | `WN_CAPTURE=<png>` + `WN_CAPTURE_FRAME=N`: copia el backbuffer a una staging texture y escribe PNG con WIC. Solo lo usan los tests visuales. |

### 18.3 Selección de backend

- `WN_RENDERER` no definido o `auto`: en Windows intenta **D3D11 hardware** y,
  si `init` falla, cae a **wgpu**. Los mensajes de error de ambos se agregan en
  el error de `wn_view_create`.
- `WN_RENDERER=d3d11`: estricto, solo D3D11 (falla si no inicializa). Es el modo
  que usan los tests para probar hardware.
- `WN_RENDERER=wgpu`: estricto, solo wgpu-native (A/B y regresión del camino
  original).
- Un valor desconocido imprime un aviso y usa `auto`.
- Las demos ya existentes reportan el resultado:
  `standalone: backend=D3D11 device=Intel(R) HD Graphics` y el renderer
  imprime al arrancar
  `renderer: backend=D3D11 device=... driver=hardware feature_level=0xA100 swapchain=flip-discard`.

### 18.4 Build y librerías

`build_abi.ps1` compila `renderer_d3d11.c` aparte con **`/W4 /WX`** (sin
warnings) y mantiene `renderer_wgpu.c` en el mismo binario. Librerías añadidas
al link (además de wgpu y las existentes):

```
d3d11.lib dxgi.lib d3dcompiler.lib windowscodecs.lib ole32.lib user32.lib gdi32.lib
```

`renderer_d3d11.c` incluye `<initguid.h>` para definir en su TU
`IID_ID3D11Texture2D`/`IID_IDXGIFactory2` (no todas las import libs del SDK las
exportan).

```powershell
# ABI completa con D3D11 (por defecto)
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build_abi.ps1

# ABI completa con el camino wgpu original
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build_abi.ps1 -Renderer wgpu

# captura + comparación de píxeles + embedded/standalone en ambos backends
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\d3d11\run_d3d11.ps1

# coste CPU (idle y hover sintético sobre los nav interactivos)
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\tests\d3d11\measure_cpu.ps1
```

`tests/pilot-media-library/run_pilot.ps1`, `tests/dynamic/run_dynamic.ps1` y
`tests/input/run_input.ps1` enlazan ahora los dos renderers y las mismas
librerías. El ABI del piloto se ejecuta con `WN_RENDERER=wgpu` para conservar
la comprobación del camino wgpu; además captura la misma página 1280×800 con
D3D11 y la compara contra el PNG offscreen del piloto.

### 18.5 Evidencia (última ejecución, esta máquina)

| Prueba | Resultado |
|---|---|
| `build_abi.ps1` (auto/D3D11) | **PASS**; `standalone` y `embedded` 20 frames, `backend=D3D11 device=Intel(R) HD Graphics`, exit 0 |
| `build_abi.ps1 -Renderer wgpu` | **PASS**; `backend=D3D12 device=Microsoft Basic Render Driver` (WARP), exit 0 |
| `/W4 /WX` `renderer_d3d11.c` | sin warnings |
| Captura base 960×640 vs wgpu offscreen | **max canal 3**, 99.83 % píxeles idénticos, 0 % fuera de tolerancia (8), contenido 70.2 % |
| Captura piloto 1280×800 (18 imágenes WIC, glyphs, clips) | **max canal 1**, 99.76 % idénticos, contenido 60.6 % |
| CPU idle (2 s) | D3D11 ≈ 0.00–0.02 cores; wgpu/WARP ≈ 0.00 cores |
| CPU hover sintético (5 s sobre los 4 nav) | **D3D11 0.011–0.033 cores** vs **wgpu/WARP 0.725–0.756 cores** (≈23–66× menos) |
| R2 (`build.ps1 -SkipRustBuild`) | **69/69 PASS** |
| Piloto canónico (`build.ps1 -Pilot -SkipRustBuild`) | **48/48 PASS** |
| Dynamic UI | **53/53 PASS** |
| Text input | **111/111 PASS** |
| CSS core | **41/41 PASS** |
| Rust (`cargo test --release`) | **27/27 PASS** |

Artefactos (gitignored): `tests/d3d11/out/d3d11_compare.json`,
`tests/d3d11/out/d3d11_compare_pilot.json`,
`tests/d3d11/out/d3d11_960x640.png`,
`tests/d3d11/out/wgpu_960x640.png`,
`tests/d3d11/out/cpu_measure.json` y
`tests/pilot-media-library/out/abi/capture_d3d11_1280.png`.

Sobre el “~3 cores” del hover: el `host_win32.c` actual ya coalesce mensajes y
solo repinta en dirty (commit `perf(host)`), así que en el fixture 960×640 el
peor caso medido con WARP es 0.73–0.76 cores. Lo que elimina D3D11 es el coste
de rasterizado software restante (23–66× en hover, y el frame CPU baja de
~0.44 ms a ~0.18–0.26 ms). En la app de escritorio, cuyo host puede forzar más
repintados, la
diferencia debería ser mayor en la misma dirección.

### 18.6 Limitaciones (honestas)

1. Solo Windows y solo D3D11; no hay backend D3D12 propio (para eso está wgpu).
2. El binario sigue importando `wgpu_native.dll` porque `renderer_wgpu.c` está
   enlazado. Mantener la DLL junto al ejecutable/DLL incluso si se usa D3D11.
3. Sin recuperación automática de device removed/reset: `Present` devuelve
   error y el frame falla.
4. Sin mipmaps, sin MSAA y sin sRGB; se usa `B8G8R8A8_UNORM` para que los
   colores coincidan byte a byte con el render offscreen de wgpu.
5. Caché de imágenes sin evicción (igual que wgpu v1).
6. `D3DCompile` necesita `d3dcompiler_47.dll` (incluida en Windows 10/11; en
   Windows 7/8 hay que redistribuirla).
7. La captura PNG es un hook de test (`WN_CAPTURE`), no forma parte de la ABI.
8. Si no hay adaptador hardware, se usa WARP pero `backend` sigue siendo
   `D3D11`; el string `device` lo delata (`Microsoft Basic Render Driver`).

### 18.7 Integración en la DLL del Desktop (`media-library`)

El proyecto de escritorio consume una copia del runtime en
`framework\src\runtime\c-r2`. Para reconstruir su `webnative.dll`:

1. Copiar/actualizar desde este repo:
   - nuevo: `src\runtime\c-r2\renderer_d3d11.c`
   - modificados: `src\runtime\c-r2\wn_renderer.h`,
     `src\runtime\c-r2\wn_view.c`
   (el resto del runtime no cambia).
2. En `build_dll.ps1`, añadir el fuente a `$sources`:

   ```powershell
   (Join-Path $runtime 'renderer_wgpu.c'),
   (Join-Path $runtime 'renderer_d3d11.c'),   # nuevo
   (Join-Path $root 'host\native_shim.c')
   ```

3. En la línea de `link`, añadir las librerías:

   ```text
   "$wgpu\lib\wgpu_native.dll.lib" d3d11.lib dxgi.lib d3dcompiler.lib windowscodecs.lib ole32.lib user32.lib gdi32.lib
   ```

4. Recompilar (`build_dll.ps1`) y arrancar la app. En stdout debe aparecer
   `renderer: backend=D3D11 device=Intel...` y el panel de stats
   `backend=D3D11`. Para A/B: `set WN_RENDERER=wgpu` antes de arrancar.
5. No hace falta tocar `native_shim.c`, `wn_app_*` ni la ABI: la selección vive
   en `wn_view_create`. Mantener `wgpu_native.dll` junto a la DLL (punto 2 de
   limitaciones).

---
