# R2 — runtime C dinámico (`src/runtime/c-r2`)

**Estado: PASS (69/69 checks).** Layout calculado en C en runtime, ventana
Win32 redimensionable, reflow real en `WM_SIZE`, hit testing y estados
hover/pressed/selected, texto pre-shapeado en un atlas R8 y render con
wgpu-native / D3D12. Sin navegador, sin WebView, sin Skia, sin Electron. El
árbol de estilos pre-layout lo emite **el frontend Rust canónico**
(`--emit-ui`); `emit_ui.py` solo lo traduce a `ui_data.h`. Python sigue siendo
build-time y el frontend Python queda como oráculo/fallback.

Este directorio es la promoción canónica de `experiments/15-runtime-r2-c`. Con
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
scroll, ni bordes, ni sombras, ni transformaciones, ni eventos de teclado más
allá de Esc.

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
        └── wgpu-native
              surface swapchain (ventana) o textura RGBA8 (offscreen)
              pipeline sólida + pipeline texturizada R8
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
| `nodes[].kind` | `block` \| `flex` \| `text` |
| `nodes[].style` | `display`, `direction`, `justify`, `align`, `wrap`, `gap`, `width_px/pct`, `height_px/pct`, `margin[4]`, `padding[4]`, `background`, `color`, `font_size`, `font_family`, `font_weight` |
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
| `runtime_r2.c` | **Todo el runtime**: layout, Win32, input, wgpu, offscreen, CLI. |
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

`build.ps1` usa el binario `src/frontend/rust/target/release/frontend-rust.exe`,
el venv canónico `src/frontend/python/.venv`, `src/emitter/python/emit_ui.py`,
MSVC (`vcvars64`) y `third_party/wgpu-native` (include/lib/bin). No referencia
`experiments/`.

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

- `runtime_r2.c` — motor de layout, Win32, input, wgpu, offscreen y CLI.
- `verify.py` — 69 checks.
- `page/index.html`, `page/style.css` — fixture.
- `emit_ui.py` — lógica de aplanado, medición/shaping y atlas; ahora en
  `src/emitter/python`. El modo `--html/--css` (oráculo) importa el frontend
  Python canónico; el modo `--ui-json` consume el árbol Rust.

**Nuevo en la promoción:**

- `src/runtime/c-r2/build.ps1` — build autocontenido (sin `experiments/`),
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

### 13.1 Módulos

| Ruta | Rol |
|---|---|
| `wn_core.h` / `wn_core.c` | Core portable: documento compilado, layout, estados, hit testing y display list (vértices sólidos + glyphs). No conoce OS, GPU ni modo. `runtime_r2.c` lo usa para no duplicar el motor. |
| `wn_renderer.h` | Vtable `wn_renderer_ops`: `init(surface,w,h)`, `resize`, `upload(display list/atlas)`, `render` (draw+present), `destroy`, `last_error`, `get_info`, `get_stats`. |
| `renderer_wgpu.c` | Backend wgpu-native (D3D12 aquí): surface desde HWND, swapchain FIFO, pipeline sólida + pipeline de texto R8, atlas bajo demanda. |
| `wn_host.h` / `host_win32.c` | Host Win32: ventana, message loop, traducción Win32 → `wn_event`, resize, captura de ratón, cierre. |
| `wn_view.h` / `wn_view.c` | Modo embedded: core + renderer sobre un HWND del host. El host posee el loop y llama `wn_view_render()` a demanda. |
| `wn_app.h` / `wn_app.c` | Modo standalone: host Win32 + `wn_view`. `wn_app_run()` bloquea hasta cerrar. |
| `wn_common.c` / `wn_internal.h` | Versión, error thread-local y helpers de copia UTF-8. |
| `examples/standalone.c` | Demo `wn_app_*`; `--frames N` cierra solo. |
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
- `standalone.exe --frames 20`: **exit 0**, adapter **D3D12**, ventana con la
  página R2, 20 frames presentados.
- `embedded.exe --frames 20 --input-demo --resize-demo`: **exit 0**, adapter
  **D3D12**; el padre redimensiona 900×640 → 720×520 → 900×640, el hijo recibe
  `WM_SIZE`, se reenvían `WM_MOUSEMOVE/WM_LBUTTONDOWN/WM_LBUTTONUP`, el click
  selecciona `body.root/div.sidebar/div.nav` y `set_text("text[1]","Texto")`
  devuelve `WN_OK`.

Timings de la última ejecución (Windows 11 x64, VM, WARP/Basic Render Driver;
no representativos de GPU real):

| Métrica | Valor |
|---|---:|
| Build ABI completo | ~15.8 s (`cl` + header checks + smoke + demos) |
| `standalone` startup (proceso→primer frame) | 149.7 ms |
| `standalone` frame medio (FIFO/vsync) | 15.64 ms |
| `standalone` CPU frame (upload+encode+submit) | 0.51 ms |
| `embedded` startup | 153.3 ms |
| `embedded` frame medio (timer) | 0.74 ms |
| `embedded` CPU frame | 0.60 ms |
| `standalone.exe` / `embedded.exe` | 285 184 / 286 720 B |
| `runtime_r2.exe` (R2 + core) | 377 344 B |
| R2 regresión (`build.ps1 -SkipRustBuild`) | **69/69 PASS** |

El coste de arranque incluye instancia/adapter/device/surface y el primer
present; en esta VM varía ~130–180 ms entre ejecuciones (el presupuesto de
150 ms es orientativo y R2 ya lo roza). El frame embedded no está limitado por
vsync en la ventana hija; el standalone sí usa `PresentMode::Fifo`.

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
7. **Sin scroll/clipping**: el overflow se dibuja fuera de la ventana, igual
   que R2.
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
- `runtime_r2.c` quedó reducido a CLI, reporting, Win32 y wgpu; el motor vive
  en `wn_core.c` y se compila desde `build.ps1` (que ahora añade `wn_core.c` y
  `wn_common.c` al binario R2). La aceptación se re-ejecutó con
  `-SkipRustBuild` y sigue en **69/69**.
- La capa ABI es un conjunto de fuentes nuevo (`wn_*.c/h`, `renderer_wgpu.c`,
  `host_win32.c`) compilado por `build_abi.ps1`; comparte `wn_core.c` con R2.
