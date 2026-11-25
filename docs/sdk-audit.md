# Auditoría del SDK

Inventario de deudas heredadas y límites que hoy bloquean madurar el motor.
No es una spec: es la lista de trabajo, ordenada por riesgo. Cada fila dice el
síntoma real y el arreglo propuesto; "aditivo" significa que no rompe contratos
existentes.

## P0 — corrección y seguridad

| ID | Área | Síntoma | Causa | Arreglo |
|---|---|---|---|---|
| A1 | ABI | Un handle viejo puede apuntar a otro nodo: `wn_node_remove` devuelve el índice a `free_list` y `wn_node_clone` lo reutiliza | `wn_node_id` es `índice + 1`, sin generación (`core/wn_core.c:arena_alloc`) | codificar generación en el handle (`índice + gen<<32`) o no reutilizar índices. Aditivo |
| A2 | Documento | `wn_core_create(doc)` no posee el documento; destruirlo antes que el core deja punteros colgando | contrato de propiedad disperso (`wn_document_internal.h`) | refcount o copia de tablas; documentar en `webnative.h`. Aditivo |
| A3 | Imágenes | La caché de texturas crece sin límite (cada foto 4K son ~33 MB RGBA) | `image_count` sin desalojo (`renderer_d3d11.c`, `renderer_wgpu.c`) | LRU con presupuesto (p. ej. 256 MB) y `wn_renderer_stats.images_bytes`. Aditivo |
| A4 | Nodos | Las listas se cortan en 4096 nodos vivos; `wn_node_clone` devuelve `-2` | arena de capacidad fija (`WN_MAX_NODES`) | arena creciente o capacidad declarada en el documento. Aditivo |
| A5 | Blob | Un `.wnui` corrupto puede hacer que `strcmp` lea fuera de la sección de strings | `blob_string` valida el offset, no el NUL dentro de la sección | validar NUL antes del final de la sección; checksum opcional. Aditivo |
| A6 | Texto | Re-shaping cada frame cuando hay más strings que caché (la galería ya tiene ~90) | caché de runs 64 lineal, medición 128 lineal | hash + LRU dimensionada por nodos. Aditivo |
| A7 | Texto | Layout mide texto envuelto (2 líneas) y el pintado dibuja una sola línea | `measure` acepta `max_width`, `shape` no | line breaking en layout+paint (E4). Aditivo |
| A8 | Fuentes | Familia desconocida cae a un ancho heurístico `0.6 * size * len` sin avisar | `measure_text_node` si el backend falla | fallback real + diagnóstico `wn_text_last_error`. Aditivo |
| A9 | Imágenes | `set_image` conserva el tamaño intrínseco compilado; una foto de otra relación de aspecto queda mal si el CSS no fija caja | `wn_core_set_image` solo cambia el path | medir la imagen nueva en el renderer o aceptar `wn_node_set_image_size`. Aditivo |

## P1 — semántica y API

| ID | Área | Síntoma | Causa | Arreglo |
|---|---|---|---|---|
| B1 | ABI | `wn_app_load`/`wn_view_load` copian HTML/CSS y no parsean nada (no hay parser runtime) | API heredada de cuando se pensó el runtime como parser | retirar en v6 o documentar como no-op. Destructivo (major) |
| B2 | Estado | Un solo `hover`/`press`/`selected` global; `wn_app_set_class` solo acepta nombres, así que un clon no puede llevar clase | estado en `core->selected` y búsqueda por nombre | estado por nodo (bitset) y `set_class` por handle. Aditivo |
| B3 | Lectura | No se puede leer el texto de un nodo, ni su rect, ni hacer scroll a un nodo | no hay `wn_node_get_text/rect/scroll_to` | añadir getters y `wn_node_scroll_into_view`. Aditivo |
| B4 | Eventos | Solo botón izquierdo, sin doble clic, sin rueda horizontal, sin botón en `wn_event` | `host_win32.c` traduce `WM_LBUTTON*` y wheel vertical | `button` y `click_count` en el evento; `WM_RBUTTON/MBUTTON/DBLCLK`. Aditivo |
| B5 | Tiempo | Sin reloj/timers: no hay caret blink, ni debounce, ni animaciones | el bucle es solo por eventos | `wn_host` con timer o `wn_view_tick(ms)`. Aditivo |
| B6 | Input | Edición ASCII 32..126, una línea, sin IME; pegar `Café` inserta `Caf` | modelo byte a byte y `WM_CHAR` | input v5: cursor por codepoint, UTF-8, drag-select, blink; IME en E3. Aditivo |
| B7 | DPI | `dpi_scale` se guarda y no se usa: texto y layout van a píxeles 1:1 | falta escalar fuentes/medidas | aplicar escala en layout+texto (E3). Aditivo |
| B8 | Renderer | `wgpu_native.dll` (9 MB) siempre enlazado aunque D3D11 sea el default | CMake sin interruptor | `WN_NO_WGPU` y cargar backend por vtable. Aditivo |
| B9 | Fuentes | Cualquier genérica (`serif`, `monospace`) se resuelve a Segoe UI; solo se usa la primera familia del stack | `text_runtime.c:resolve_family` | parsear el stack y elegir por disponibilidad. Aditivo |
| B10 | Imágenes | Formatos dependen de los códecs WIC instalados; sin WebP/AVIF garantizado y sin decode asíncrono | decode en UI thread vía WIC | decode en worker + cola; formatos documentados. Aditivo |

## P2 — tooling, proceso y contratos

| ID | Área | Síntoma | Causa | Arreglo |
|---|---|---|---|---|
| C1 | CI | No hay CI: wgpu no está versionado y no hay script de descarga | se borró el workflow R1 | workflow con `WN_NO_WGPU` (B8) o fetch con hash |
| C2 | Paridad visual | `benchmarks/visual-parity/tools/render_native.py` apunta a `c-r2` y no hay runner | R1 retirado | retirar a `archive/` o reescribir sobre `wn-cli` |
| C3 | Métricas | `engine-metrics` sin ejecutar desde que se quitó R1 | rama R1 eliminada sin re-baseline | correr `wn-cli bench metrics` y actualizar `BASELINE.md` |
| C4 | Tests | `test all` regenera `tests/*/page/ui.json` trackeados y deja el árbol sucio | fixtures dentro del repo | generarlos en `out/` o commitearlos como golden y no regenerarlos |
| C5 | Compilador | Dos serializadores: Rust escribe `.wnui`, Python escribe `ui_data.h`; normalizaciones duplicadas | transición a medias | `--emit-header` en Rust y retirar el emisor |
| C6 | Blob | Solo hay versión exacta; sin política de compatibilidad | loader exige `version == 2` | aceptar versiones ≤ actual con secciones opcionales |
| C7 | Hilos | `text_runtime` es singleton global con buffer estático | asume un hilo de UI | documentar o pasar contexto por core |
| C8 | Errores | Mezcla de `-1/-2` y `wn_status`; no hay código estructurado | crecimiento histórico | normalizar a `wn_status` + `wn_last_error` |
| C9 | Input | Valor máximo 255 bytes y sin deshacer/rehacer | buffer fijo | buffer dinámico y undo/redo (E3) |
| C10 | A11y | Sin UIA/ARIA, sin foco por teclado fuera del input, sin contraste alto | E3 pendiente | plan de accesibilidad |

## Lo que ya está maduro

- Texto: medición, shaping y raster en runtime; atlas dinámico; sin glifos en el documento.
- Documento: blob v2 versionado, loader con mmap, offsets validados (salvo A5), escritor Rust byte-idéntico al de referencia.
- ABI v5: identidad de nodo en eventos, navegación de subárbol, carga de documento, stats.
- Suites: 9/9 con comparación de píxeles exacta (blob/offscreen) y piloto 48/48.

## Orden recomendado

1. A1, A3, A4 (handles, memoria de imágenes, capacidad): correctness para apps reales.
2. B8 (wgpu opcional) → desbloquea CI (C1) y baja el runtime a ~190 KB.
3. A6, A7 (cachés y wrapping): escala y texto correcto.
4. B2, B3, B4, B5 (estado por nodo, getters, botones/doble clic, timer): lo que pide una app como la galería.
5. C5 (`--emit-header`) y B1 (limpiar ABI v6).
