# exp12-interactivity — runtime interactivo mínimo (headless)

Objetivo: demostrar que el mismo display-list IR de webnative puede alimentar un
runtime con **hit testing, estado y re-render por evento**, sin navegador, WebView,
Skia ni Electron. La entrada del usuario se simula desde `events.json`; no se abre
ninguna ventana.

**Estado: SUCCESS.**

- 13 frames PNG (1 inicial + 12 eventos), 400x300.
- 8 tests de hit testing/semántica: **8 passed** (pytest y runner directo).
- Verificación end-to-end: **150 checks, 0 failed** (`verify_report.json`).
- Demo opcional wgpu del frame 010: **maxdiff 0** contra el PNG CPU.
- Runtime: ~3,2 ms por frame en régimen estable (CPU/Pillow, 400x300).

---

## 1. Arquitectura

```
 scene.json                         events.json (ordenados por t_ms)
     │                                   │
     ▼                                   ▼
 ┌─────────────────────────────────────────────────────────┐
 │ runtime.py                                              │
 │                                                         │
 │  load_scene ──▶ Scene {buttons, dots, background}       │
 │                       │                                 │
 │  event (x,y) ──▶ hit_test(regiones, x, y)               │
 │                       │  topmost = reverse painter's    │
 │                       │  boundary = [x, x+w) x [y, y+h) │
 │                       ▼                                 │
 │                  State {hovered, pressed, clicks}       │
 │                       │                                 │
 │                       ▼                                 │
 │        build_display_list(scene, state)  ← overrides    │
 │                       │                                 │
 │                       ▼                                 │
 │        IR {viewport, ops:[RectOp...]}                   │
 │                       │                                 │
 │                       ▼                                 │
 │        render_cpu (Pillow, painter's order)             │
 │                       │                                 │
 │                       ▼                                 │
 │        frames/frame_NNN.png                             │
 │        frames/display_list_NNN.json                     │
 │        state_log.json (hit, estado, hash, timings)      │
 └─────────────────────────────────────────────────────────┘
```

Cada evento produce exactamente un frame: no hay batching ni coalescing. El IR de
cada frame se guarda junto al PNG para poder auditarlo sin re-ejecutar el runtime.

### Geometría de la escena

Viewport 400x300, fondo `#101014`.

| Elemento | id | x | y | w | h | normal | hover | pressed |
|---|---|---:|---:|---:|---:|---|---|---|
| Botón A | `btn-a` | 20 | 40 | 100 | 60 | `#2b2b36` | `#3a3a48` | `#4a4a5c` |
| Botón B | `btn-b` | 140 | 40 | 100 | 60 | `#2b2b36` | `#3a3a48` | `#4a4a5c` |
| Botón C | `btn-c` | 260 | 40 | 100 | 60 | `#2b2b36` | `#3a3a48` | `#4a4a5c` |

Cinco dots de 10x10 con gap 8, desde x=20, y=220: posiciones x = 20, 38, 56, 74, 92.
Inactivo `#2b2b36`; activo `#3498db`. Los dots **no** son interactivos: no están en
la lista de regiones de hit testing.

---

## 2. Semántica de eventos y de hit testing

### Hit testing

- **Half-open**: un punto pertenece a `Rect(x,y,w,h)` si
  `x <= px < x+w` y `y <= py < y+h`. El borde izquierdo/superior es inclusivo; el
  derecho/inferior es exclusivo. Así el píxel `x+w` pertenece al vecino o al fondo,
  igual que en el rasterizado.
- **Topmost gana**: se recorre la lista de regiones en orden inverso al pintado
  (painter's order). La última pintada que contiene el punto es la que recibe el
  evento. Los botones no se solapan en esta escena; el caso solapado se prueba en
  `tests/test_hit_test.py` con dos rects sintéticos.
- Un punto fuera de toda región devuelve `None` (espacio vacío).

### Máquina de estados

| Evento | `hovered` | `pressed` | `clicks` |
|---|---|---|---|
| `pointer_move(x,y)` | `hit_test(x,y)` | sin cambios | sin cambios |
| `pointer_down(x,y)` | `hit_test(x,y)` | `hit_test(x,y)` | sin cambios |
| `pointer_up(x,y)` | `hit_test(x,y)` | `None` | +1 solo si `pressed != None` y `pressed == hit` |

Detalles deliberados:

- Un **click** exige down y up sobre el **mismo id**. Si se presiona `btn-a` y se
  suelta en el fondo (drag-out), no cuenta y el contador no cambia.
- Mientras se mantiene el botón presionado, `pressed` sobrevive a los `pointer_move`.
- El color *pressed* solo se aplica si el botón está presionado **y** el puntero
  sigue encima (`pressed == hovered == id`), como un `:active` real. Si se arrastra
  fuera, el botón vuelve a su color normal pero conserva el estado `pressed` hasta
  el `pointer_up`.
- Tras un `pointer_up`, `hovered` pasa a ser el punto de suelte, no el de presión.
- `t_ms` es metadata del guion; el runtime procesa los eventos en el orden del
  archivo, sin reloj ni timers.

### Guion de eventos (12)

| # | Frame | Evento | Punto | Hit | Estado resultante (hover/pressed/clicks) | Para qué |
|---:|---:|---|---|---|---|---|
| — | 000 | initial | — | — | `None / None / 0` | referencia base |
| 1 | 001 | move | 190,70 | btn-b | `btn-b / None / 0` | hover |
| 2 | 002 | down | 190,70 | btn-b | `btn-b / btn-b / 0` | pressed |
| 3 | 003 | up | 190,70 | btn-b | `btn-b / None / 1` | click 1 |
| 4 | 004 | move | 195,80 | btn-b | `btn-b / None / 1` | mismo estado visual |
| 5 | 005 | move | 200,150 | — | `None / None / 1` | espacio vacío |
| 6 | 006 | move | 240,70 | — | `None / None / 1` | borde derecho de btn-b (exclusivo) |
| 7 | 007 | move | 140,40 | btn-b | `btn-b / None / 1` | borde izq./sup. (inclusivo) |
| 8 | 008 | move | 310,70 | btn-c | `btn-c / None / 1` | hover |
| 9 | 009 | down | 310,70 | btn-c | `btn-c / btn-c / 1` | pressed |
| 10 | 010 | up | 310,70 | btn-c | `btn-c / None / 2` | click 2 |
| 11 | 011 | down | 70,70 | btn-a | `btn-a / btn-a / 2` | pressed |
| 12 | 012 | up | 200,150 | — | `None / None / 2` | drag-out: no click |

Frames que deben ser **idénticos** (mismo estado ⇒ mismo IR ⇒ mismos píxeles):

- `003` vs `004`: moverse dentro del botón ya hovereado no cambia nada.
- `005` vs `006`: ambos caen en espacio vacío (`240,70` es el borde derecho
  exclusivo de btn-b).

Frames que deben **diferir**: `000→001→002→003` (hover, pressed, click), `003→005`
(hover limpiado), `005→007` (borde inclusivo), `007→008→009→010` (hover/pressed/
click 2), `010→011→012` (pressed de btn-a y drag-out).

---

## 3. IR y renderer

El IR es el mismo contrato que `spec/CONTRACT.md` y exp05:

```json
{
  "viewport": {"width": 400, "height": 300},
  "ops": [
    {"type": "rect", "x": 0,   "y": 0,  "w": 400, "h": 300, "fill": "#101014"},
    {"type": "rect", "x": 140, "y": 40, "w": 100, "h": 60,  "fill": "#4a4a5c"},
    {"type": "rect", "x": 20,  "y": 220, "w": 10, "h": 10,  "fill": "#3498db"}
  ]
}
```

Cada frame reconstruye la lista completa: fondo, 3 botones, 5 dots ⇒ **9 ops**.
`render_cpu` usa Pillow/ImageDraw y respeta painter's order (source-over de rects
opacos). El rasterizado usa `[x, x+w-1]` inclusivo, coherente con el hit testing
half-open.

Demo opcional GPU (`wgpu_frame.py`): toma el `display_list_010.json` del log y lo
pinta con wgpu; el resultado se compara píxel a píxel con el PNG CPU.

```powershell
.\.venv\Scripts\python.exe runtime.py
.\.venv\Scripts\python.exe verify.py
.\.venv\Scripts\python.exe wgpu_frame.py 10
```

---

## 4. Verificación

`verify.py` comprueba:

1. **Estado**: 13 entradas, evento #N en orden, hit y `{hovered,pressed,clicks}`
   esperados para cada frame.
2. **Píxeles**: color hover en el centro de btn-b (`190,70`), pressed en el centro,
   dot 1 activo tras el primer click, dots 1 y 2 activos tras el segundo, dot 3
   inactivo, fondo en espacio vacío, pressed de btn-a y su liberación por drag-out.
3. **Igualdad**: `003 == 004` y `005 == 006`; las 12 parejas que deben diferir,
   difieren.
4. **Integridad**: SHA-256 del PNG coincide con el log; y **cada PNG es
   byte-idéntico al render independiente de su `display_list_NNN.json`**
   (IR → PNG, maxdiff 0).
5. **Geometría**: todos los PNG miden 400x300; cada frame tiene 9 ops.

Resultado: **150 checks, 0 failed**. Los tests de hit testing: **8 passed**.

Casos de borde cubiertos en `tests/test_hit_test.py`: interior, exterior, borde
exacto (izq./sup. inclusivo, der./inf. exclusivo), solapamiento (topmost gana y el
orden del painter manda), espacio vacío, bordes reales de los tres botones, dots no
interactivos, drag-out sin click y `pointer_up` sin `pointer_down`.

---

## 5. Timings medidos

Máquina: Windows, Python 3.12.10, Pillow 13.0.0. `perf_counter()`, un proceso nuevo
por corrida. El frame 000 paga la inicialización perezosa de Pillow (~30 ms de
guardado); por eso se reporta también el régimen estable (frames 001–012). Tabla de
una corrida representativa; los valores exactos de la última corrida están en
`state_log.json`.

| Frame | Evento | Hit | total (ms) | hit (ms) | build IR (ms) | render (ms) | save PNG (ms) |
|---:|---|---|---:|---:|---:|---:|---:|
| 000 | initial | — | 31,049 | 0,000 | 0,028 | 0,358 | 30,661 |
| 001 | move | btn-b | 3,566 | 0,028 | 0,026 | 0,274 | 3,237 |
| 002 | down | btn-b | 3,232 | 0,036 | 0,023 | 0,275 | 2,896 |
| 003 | up | btn-b | 3,176 | 0,023 | 0,022 | 0,261 | 2,868 |
| 004 | move | btn-b | 3,003 | 0,019 | 0,022 | 0,248 | 2,713 |
| 005 | move | — | 3,437 | 0,037 | 0,022 | 0,261 | 3,116 |
| 006 | move | — | 3,117 | 0,023 | 0,021 | 0,252 | 2,818 |
| 007 | move | btn-b | 2,994 | 0,018 | 0,021 | 0,250 | 2,704 |
| 008 | move | btn-c | 3,194 | 0,018 | 0,021 | 0,255 | 2,899 |
| 009 | down | btn-c | 3,174 | 0,024 | 0,023 | 0,267 | 2,858 |
| 010 | up | btn-c | 3,224 | 0,026 | 0,022 | 0,281 | 2,892 |
| 011 | down | btn-a | 3,151 | 0,019 | 0,022 | 0,252 | 2,858 |
| 012 | up | — | 3,228 | 0,026 | 0,022 | 0,267 | 2,912 |

Régimen estable (001–012): total mediana **3,185 ms**, media 3,208, p95 3,566.
Desglose: hit test 0,024 ms; build IR 0,022 ms; render CPU 0,261 ms; guardar PNG
2,880 ms. **~90% del frame es codificar el PNG**, no la lógica interactiva. Wall
total de la corrida (13 frames): 95,7 ms.

Demo wgpu del frame 010 (incluye creación de adapter/device, no es representativo
de un frame en caliente): 2.103,61 ms totales, 1.100,95 ms de inicialización,
`Microsoft Basic Render Driver / D3D12 / CPU`; maxdiff **0** contra CPU. Para 12
frames, CPU Pillow es la opción correcta.

---

## 6. Por qué runtime y no AOT

exp08 demuestra que un display list **estático** se puede convertir a datos C y
compilar a un `.exe` sin Python: el binario recorre `kSceneOps[]` y pinta. Eso es
ideal cuando la escena no cambia.

La interactividad cambia el contrato: el frame ya no es una constante de build,
depende de `(hovered, pressed, clicks)`. El runtime debe:

1. recibir un evento y hacer hit testing contra geometría viva;
2. actualizar estado;
3. re-evaluar los overrides de paint (hover/pressed/dots);
4. producir un nuevo display list y pintarlo.

Un AOT puro tendría que precompilar **todas** las combinaciones de estado (3 botones
× 3 estados + contador) y aun así necesitaría el hit test y el event loop en
runtime. El camino correcto es híbrido:

- **AOT**: geometría estática, tablas de regiones, layouts, fuentes, capas de fondo.
- **Runtime**: hit testing, estado, overrides de estilo, dirty rectangles y
  composición.

El IR sigue siendo la frontera: el mismo `display_list_NNN.json` que consume Pillow
puede consumir wgpu o un backend nativo. El runtime no sabe nada de HTML/CSS; eso se
resuelve en build time (o en una etapa previa), como en exp01–exp08.

---

## 7. Limitaciones (explícitas)

- **Sin entrada real del SO**: los eventos vienen de `events.json`. No hay Win32
  `WM_MOUSEMOVE`/`WM_LBUTTONDOWN`, ni cursor, ni foco, ni teclado, ni multi-touch.
- **Sin timers ni animaciones**: `t_ms` no se usa para scheduling; no hay reloj,
  frames por segundo ni interpolación.
- **Sin invalidación de layout**: la geometría es fija; solo cambian colores y
  dots. No hay reflow, ni cambios de tamaño, ni texto que afecte layout.
- **Rebuild completo por frame**: se reconstruyen las 9 ops y se rasteriza toda la
  escena, aunque solo cambie un botón. No hay dirty rectangles ni display-list diff.
- **PNG completo por frame**: se codifica un PNG nuevo; no hay framebuffer
  persistente ni swap de buffers. Codificar domina el costo.
- **Un solo puntero** y sin captura explícita: el drag-out se modela con `pressed`,
  pero no hay pointer capture, hover sintético tras soltar, ni touch.
- **Sin z-index/transform/clip/text/imágenes**: el IR mínimo solo tiene rects
  opacos; no hay opacidad, bordes redondeados ni antialiasing.
- **Dots no interactivos** y viewport fijo 400x300.
- **Sin accesibilidad, scroll, focus ring ni hit slop**.
- La demo wgpu crea device y textura por frame; no es un bucle GPU en caliente.

---

## 8. Archivos

| Archivo | Rol |
|---|---|
| `scene.json` | viewport, fondo, 3 botones con sus 3 colores, y tira de 5 dots |
| `events.json` | 12 eventos ordenados con `t_ms`, tipo, x, y y nota |
| `runtime.py` | Scene, hit test, State, event loop, build IR, render CPU, log |
| `verify.py` | 150 checks: estado, píxeles, igualdad, hash e IR→PNG |
| `tests/test_hit_test.py` | 8 tests de hit testing y semántica (pytest o runner) |
| `wgpu_frame.py` | demo opcional: un frame con wgpu + comparación con CPU |
| `run_all.ps1` | ejecuta runtime + tests + verificación y falla si algo falla |
| `frames/frame_NNN.png` | 13 frames renderizados |
| `frames/display_list_NNN.json` | IR exacto usado por cada frame |
| `frames/frame_010_wgpu.png` | frame 010 pintado con wgpu |
| `state_log.json` | hit, estado, hashes y timings por frame + resumen |
| `verify_report.json` | detalle de los 150 checks |
| `wgpu_report.json` | adapter, tiempos y maxdiff de la demo GPU |

---

## 9. Reproducir

```powershell
cd .\exp12-interactivity
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install --upgrade pip
.\.venv\Scripts\python.exe -m pip install pillow wgpu html5lib tinycss2 cssselect2 pytest

# 0) todo de una vez (runtime + pytest + runner + verify)
powershell -NoProfile -ExecutionPolicy Bypass -File .\run_all.ps1

# 1) runtime: 13 frames + state_log.json
.\.venv\Scripts\python.exe runtime.py

# 2) tests de hit testing (dos formas)
.\.venv\Scripts\python.exe -m pytest tests -q
.\.venv\Scripts\python.exe tests\test_hit_test.py

# 3) verificación end-to-end (sale 0 si todo pasa)
.\.venv\Scripts\python.exe verify.py

# 4) opcional: un frame con wgpu, maxdiff contra CPU
.\.venv\Scripts\python.exe wgpu_frame.py 10
```

Versiones verificadas: Python 3.12.10, Pillow 13.0.0, wgpu 0.33.0, pytest 9.2.0.
`html5lib`, `tinycss2` y `cssselect2` están instalados para mantener el entorno del
proyecto, pero exp12 no los importa: la escena ya es datos, no HTML/CSS.

---

## 10. Recomendaciones siguientes

1. **Dirty rectangles / damage tracking**: comparar display lists y repintar solo
   las regiones cambiadas; con 3 botones y 5 dots el área dañada es mínima.
2. **Framebuffer persistente**: mantener la imagen y hacer `paste` de las regiones;
   evitar PNG por frame (o encodear solo para debug). El evento debería terminar en
   un swap de buffer, no en un archivo.
3. **Fuente de eventos real**: Win32 (`WM_MOUSEMOVE`, `WM_LBUTTONDOWN/UP`,
   `WM_MOUSELEAVE`) o Wayland/X11, con timestamps del sistema. Mantener
   `events.json` como fixture de tests.
4. **Hit-test index**: para muchas regiones, grid/interval tree en vez de escaneo
   lineal inverso; hoy 3 regiones, trivial.
5. **Invalidación de layout**: cuando haya texto o tamaño dinámico, separar
   `layout_dirty` de `paint_dirty` y reusar geometría si no cambió.
6. **Diff de IR**: hashear cada op o la lista; si el frame no cambia (caso 003/004),
   no renderizar ni guardar. Eso ya se detecta en verificación; falta explotarlo.
7. **Captura de puntero y multi-pointer**: generalizar `pressed` a un mapa por
   pointer id.
8. **GPU en caliente**: un device/textura persistentes y un solo pass por frame;
   la demo actual mide inicialización, no throughput.

## Key Learnings:

1. El hit testing half-open `[x, x+w)` coincide exactamente con el rasterizado de
   Pillow `[x, x+w-1]`, lo que evita ambigüedad en los bordes y permite tests exactos.
2. El costo real por frame en un pipeline CPU headless está dominado por la
   codificación PNG (~3 ms de 3,4 ms), no por hit test (0,02 ms) ni por construir el
   IR (0,02 ms); el dirty-rectangle es la optimización de mayor impacto.
3. Modelar `pressed` independiente de `hovered` permite drag-out correcto: el click
   solo se cuenta si down y up comparten id, y el color pressed requiere además que
   el puntero siga encima.
4. Un runtime de estados sobre IR no necesita HTML/CSS en caliente: la escena y las
   regiones se cargan una vez y cada evento solo re-evalúa overrides de paint.
