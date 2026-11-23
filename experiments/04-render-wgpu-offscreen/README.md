# exp04 — Backend GPU con wgpu (D3D12 / Vulkan / Metal)

Prueba mínima end-to-end: HTML + CSS → cascade → layout de bloques → display list IR →
render nativo offscreen con **wgpu-py** (bindings a wgpu-native). Sin navegador, WebView,
Skia ni Electron.

**STATUS: SUCCESS** — el render se hizo por GPU API nativa (D3D12) y la verificación de
píxeles pasó con `maxdiff = 0`.

## Cómo ejecutar

```powershell
py -3.12 -m venv .venv
.venv\Scripts\python.exe -m pip install -r requirements.txt
.venv\Scripts\python.exe run.py
```

Salidas: `display_list.json` y `render.png` (400×300).

## Versiones

| Componente | Versión |
|---|---|
| Python | 3.12.10 |
| html5lib | 1.1 |
| tinycss2 | 1.5.1 |
| wgpu (wgpu-py) | 0.33.0 |
| wgpu-native | incluido en el wheel de wgpu-py 0.33.0 |
| Pillow | 13.0.0 |
| Windows | 10.0.19041 (build del driver reportado por el adapter) |

## Backend y adaptador

```
adapter: Microsoft Basic Render Driver
backend: D3D12
type:    CPU
vendor_id: 5140 (Microsoft)
device_id: 140
```

- En esta máquina **no se enumeró ninguna GPU de hardware**: el único adaptador visible es
  **WARP (Microsoft Basic Render Driver)**, un rasterizador por software expuesto a través de
  D3D12. Vulkan no apareció en `enumerate_adapters_sync()`.
- Aun así, el camino es 100 % wgpu-native + D3D12 (no Pillow, no Skia): el mismo código elige
  Vulkan o Metal en otras plataformas sin cambios. La verificación de píxeles es idéntica.
- `wgpu.gpu.request_adapter_sync(power_preference="high-performance")` fue suficiente; no hizo
  falta forzar backend ni fallback por variable de entorno.

## Cobertura por etapa

| Etapa | Estado | Detalle |
|---|---|---|
| 1. Parse HTML | ✅ | `html5lib.parse(treebuilder="etree", namespaceHTMLElements=False)` → árbol propio |
| 2. Parse CSS | ✅ | `tinycss2.parse_stylesheet` + `parse_declaration_list` |
| 3. Cascade propio | ✅ | selectores compuestos tag/id/clase, combinador descendiente, especificidad `(ids, clases, tags)` y orden de fuente |
| 4. Layout de bloques | ✅ | `body` margin 0; `#root` 400×300 en (0,0) con padding 20; `.box` 120×80; `margin-top` 16 en `.b`; apilado en el content box del padre |
| 5. Display list IR | ✅ | exactamente las 3 ops del contrato, orden painter root → a → b |
| 6. Render GPU | ✅ | textura offscreen `rgba8unorm` 400×300, `RENDER_ATTACHMENT|COPY_SRC`; clear al color de root; 2 rects en **un solo draw call** (12 vértices, 2 triángulos por rect) con color por vértice; WGSL mínimo |
| 7. Readback | ✅ | `copy_texture_to_buffer` con filas alineadas a 256 B → `map_sync` → `render.png` con Pillow |
| 8. Verificación | ✅ | tamaño y 3 píxeles con tolerancia ±8 |

Geometría emitida (coincide con `CONTRACT.md`):

```json
[
  {"type": "rect", "x": 0,  "y": 0,   "w": 400, "h": 300, "fill": "#101014"},
  {"type": "rect", "x": 20, "y": 20,  "w": 120, "h": 80,  "fill": "#e74c3c"},
  {"type": "rect", "x": 20, "y": 116, "w": 120, "h": 80,  "fill": "#3498db"}
]
```

## Verificación

Salida real de `run.py`:

```
[wgpu] adapter: Microsoft Basic Render Driver | backend: D3D12 | type: CPU | vendor_id: 5140
[verify] size: (400, 300) (expected (400, 300)) -> OK
[verify] (60,60) box a: got #e74c3c expected #e74c3c maxdiff=0 -> OK
[verify] (60,156) box b: got #3498db expected #3498db maxdiff=0 -> OK
[verify] (200,250) root: got #101014 expected #101014 maxdiff=0 -> OK
PASS
```

La imagen se inspeccionó visualmente: fondo oscuro, caja roja arriba, caja azul debajo con el
gap de 16 px. No hay filas invertidas ni padding de fila visible: el recorte de los 192 B de
padding por fila (400·4 = 1600 → 1792 B) es correcto.

## Tiempos (una corrida en frío, sin profiler)

| Etapa | Tiempo |
|---|---|
| html5lib parse | 14.6 ms |
| tinycss2 parse | 1.0 ms |
| cascade | < 0.1 ms |
| layout + display list | 0.4 ms |
| wgpu render + readback + PNG | 2178.9 ms |
| verificación | 2.1 ms |
| **TOTAL** | **2197.8 ms** |

El costo GPU está dominado por la **inicialización de una sola vez** (carga de wgpu-native,
parseo de cdefs de cffi, creación de adapter/device y compilación del pipeline). Medido en el
mismo proceso, sin volver a inicializar cffi:

```
frame 0: 2145.9 ms
frame 1:   61.9 ms
frame 2:   59.2 ms
```

Es decir, ~60 ms por frame repitiendo incluso la creación de device/pipeline y el guardado de
PNG sobre WARP. En una GPU de hardware esos tiempos bajan mucho más. Para un renderer real
habría que cachear device/pipeline, no recrearlos por frame.

## Problemas encontrados

1. **API keyword-only y enums con nombres inesperados en wgpu 0.33**: `create_texture(size=...)`,
   `TextureFormat.rgba8unorm` (no `rgba8_unorm`), `MapMode.READ` (no `MapMode.read`).
   Se resolvió inspeccionando firmas con `inspect.signature` y los miembros del enum.
2. **`create_buffer_with_data` no acepta `list`**: requiere buffer protocol. Se usó
   `array.array("f", verts)`.
3. **Alineación de filas**: `copy_texture_to_buffer` exige `bytes_per_row` múltiplo de 256.
   Con 400 px hay que usar 1792 B/fila y quitar el padding al reconstruir la imagen.
4. **Ruido del backend GL**: al inicializar, wgpu-native intenta crear un contexto GL y emite
   `Returned GL context is 1.1, when 3.3+ is needed` (3 veces). Es inofensivo: el adaptador
   elegido es D3D12. No se silenció para no ocultar información del entorno.
5. **Solo adaptador software**: no hay GPU de hardware enumerable en esta máquina
   (WARP/D3D12). El resultado demuestra la API y el pipeline, pero no el rendimiento de una
   GPU real.
6. **Formato de color**: se eligió `rgba8unorm` (no `-srgb`) para que los bytes del clear y de
   los vértices coincidan exactamente con los hex del CSS. Con `-srgb` habría que convertir a
   lineal y la verificación byte a byte cambiaría.
7. La primera llamada a wgpu cuesta ~2.1 s por la inicialización de cffi/wgpu-native descrita
   arriba; no es un fallo del render.

## Limitaciones del slice (a propósito)

- Sin texto, bordes, radios, gradientes, sombras ni imágenes: solo rects de color sólido.
- Layout solo de bloques: sin `display: inline/flex/grid`, sin floats, sin margin collapsing,
  sin auto-height real por contenido, sin `box-sizing` alternativo (se asume el modelo del
  contrato: width/height incluyen padding), solo unidades `px`.
- Cascade: sin `!important`, inline styles, herencia, pseudo-clases, media queries ni `@rules`.
  Sí soporta selectores compuestos (`div.a#x`), `*`, listas separadas por coma y descendencia.
- Colores: solo `#rgb` / `#rrggbb` (y `transparent`); no `rgb()`/`hsl()`/nombres.
- Una draw call para todos los rects; sin scissor/clip stack ni z-index.
- El fondo de `body` se calcula en la cascade pero no se emite como op porque `#root` lo cubre
  por completo; el contrato permite ops extra, pero se mantuvieron exactamente las 3
  obligatorias para que el IR sea comparable.

## Recomendación

**wgpu-py funciona bien como capa de abstracción cross-backend para este proyecto.** El flujo
IR (JSON de rects) → vértices → una draw call → textura → readback → PNG quedó verificado sin
tocar nada específico de D3D12. Para el motor real:

- Mantener el display list como IR serializable y generar vértices en CPU; agrupar rects en
  batches (una draw call por lote) es trivial con este esquema.
- Cachear `adapter`/`device`/`pipeline` y reutilizar la textura de swap/offscreen entre frames.
- Añadir un shader de rects redondeados + atlas de texto cuando haga falta; el pipeline WGSL
  actual es el esqueleto mínimo.
- Si se necesita sRGB correcto, usar `rgba8unorm-srgb` y convertir colores CSS a lineal,
  o mantener `rgba8unorm` si se prioriza coincidencia byte a byte.
- Probar en una máquina con GPU de hardware (Vulkan/D3D12) para medir el costo real; aquí WARP
  impide hablar de rendimiento. La API no necesitó cambios por ser software.

## Archivos

- `run.py` — pipeline completo (296 líneas).
- `input.html`, `input.css` — copiados sin cambios desde `spec/`.
- `display_list.json` — IR generado.
- `render.png` — render offscreen 400×300 verificado.
- `requirements.txt` — dependencias fijadas.
