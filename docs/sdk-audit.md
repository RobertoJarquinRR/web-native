# Auditoría del SDK

> **Nota:** este documento es un snapshot histórico del audit. C5 y C12 están
> **resueltos** (fase 2b): el frontend Rust es el único compilador, todos los
> fixtures —incluidos los CSS— producen `.wnui` directamente y `wnui-dump`
> cubre la vista de debug del blob. El contrato vivo está en
> `docs/wnui-format.md`.

Inventario de deudas heredadas y límites que hoy bloquean madurar el motor.
No es una spec: es la lista de trabajo, ordenada por riesgo. Cada fila dice el
síntoma real y el arreglo propuesto; "aditivo" significa que no rompe contratos
existentes.

## Resuelto

**Ola 1:** A1 (handles con generación), A3 (LRU de imágenes con presupuesto),
A4 (arena creciente + jerarquía O(1)), A5 (NUL del blob), A6 (cachés hash+LRU),
A7 (wrapping), A8/B9 (stack de fuentes y fallback probado), A10 (medición del
override), B6 (input UTF-8, drag, blink; falta IME), B8 (`WN_WGPU=OFF`), C1 (CI
D3D11 offline + job wgpu fijado) y D1–D19 salvo lo anotado en cada fila.

**Ola 2:** A2 (refcount del documento), A9 (`wn_node_set_image_size`), B2
(estado por nodo), B3 (getters/scroll/foco), B4 (botón/doble clic/rueda
horizontal), B5 (tick público), C5 (compilador único `.wnui`), C11 (paridad de
color), C12 (documento blob-only, fixtures CSS incluidos) y D11.

**Ola 3 (enablers, sin cambio visible):** tabla de capacidades por kind
(`kind_ops`), bloque de línea único para layout/paint/input, servicio de scroll
x/y (X reservado), paint items ordenados con capa/transform/opacidad y culling
calculado. Deja `position`, `z-index`, `transform`, opacidad y virtualización
como features aditivas.

Pendiente de la lista: B1, B7, B10–B13, C2, C3, C6–C10. **Siguiente:** features de
estilo (`opacity` v3, position v4, z-index v5; luego transform). En
pre-alfa el formato rompe libremente; C6 se decide al congelar 1.0.

## P0 — corrección y seguridad

| ID | Área | Síntoma | Causa | Arreglo |
|---|---|---|---|---|
| A1 | ABI | Un handle viejo puede apuntar a otro nodo: `wn_node_remove` devuelve el índice a `free_list` y `wn_node_clone` lo reutiliza | `wn_node_id` es `índice + 1`, sin generación (`core/wn_core.c:arena_alloc`) | codificar generación en el handle (`índice + gen<<32`) o no reutilizar índices. Aditivo |
| A2 | Documento | ~~`wn_core_create(doc)` no posee el documento~~ | contrato de propiedad disperso | **resuelto (v6)**: refcount (`wn_document_retain/release`); el core retiene y suelta; `replace_document` mantiene una referencia |
| A3 | Imágenes | La caché de texturas crece sin límite (cada foto 4K son ~33 MB RGBA) | `image_count` sin desalojo (`renderer_d3d11.c`, `renderer_wgpu.c`); cada decode admite hasta 16384×16384 (~1 GB RGBA) antes de entrar en la caché (`decode_image_rgba`) | LRU con presupuesto (p. ej. 256 MB) y `wn_renderer_stats.images_bytes`. Aditivo |
| A4 | Nodos | Las listas se cortan en 4096 nodos vivos; `wn_node_clone` devuelve `-2` | arena de capacidad fija (`WN_MAX_NODES`) | arena creciente o capacidad declarada en el documento. Aditivo |
| A5 | Blob | Un `.wnui` corrupto puede hacer que `strcmp` lea fuera de la sección de strings | `blob_string` valida el offset, no el NUL dentro de la sección | validar NUL antes del final de la sección; checksum opcional. Aditivo |
| A6 | Texto | Re-shaping cada frame cuando hay más strings que caché (la galería ya tiene ~90) | caché de runs 64 lineal, medición 128 lineal; además `wn_text_atlas_glyph` se consulta por glifo cada frame (sondeo lineal sobre 8192 slots en `text_atlas.c:76-91`) | hash + LRU dimensionada por nodos. Aditivo |
| A7 | Texto | Layout mide texto envuelto (2 líneas) y el pintado dibuja una sola línea | `measure` acepta `max_width`, `shape` no | line breaking en layout+paint (E4). Aditivo |
| A8 | Fuentes | Familia desconocida cae a un ancho heurístico `0.6 * size * len` sin avisar | `measure_text_node` si el backend falla | fallback real + diagnóstico `wn_text_last_error`. Aditivo |
| A9 | Imágenes | `set_image` conserva el tamaño intrínseco compilado | `wn_core_set_image` solo cambia el path | **resuelto (v6)**: `wn_node_set_image_size` re-layout y `object-fit`; rechaza tamaños no finitos |
| A10 | Texto | ~~`set_text` cambia lo que se pinta pero no lo que se mide~~ | `measure_text_node` usaba `n->text`, no el override | **resuelto (ola 1)**: `set_text` marca layout y el bloque de línea mide el override; la galería dejó los anchos fijos |
| D1 | Texto | Un `font-size` grande en un `.wnui` desborda el `scratch` de 64 KB del atlas: `width*height` se multiplica en `int` y el chequeo de capacidad pasa con el producto desbordado | `text_directwrite.cpp:471` compara `capacity < width * height` (int) y `:476` pasa ese producto a `CreateAlphaTexture`; el buffer es `text_atlas.c:214` (`WN_ATLAS_MAX_BITMAP`) y `font_size` entra sin validar desde el blob (`wn_blob.c:244`) | calcular en `int64_t`, tope de em-size (~512 px) y rechazar `font_size` fuera de rango al cargar. Aditivo |

## P1 — semántica y API

| ID | Área | Síntoma | Causa | Arreglo |
|---|---|---|---|---|
| B1 | ABI | `wn_app_load`/`wn_view_load` copian HTML/CSS y no parsean nada (no hay parser runtime) | API heredada de cuando se pensó el runtime como parser | retirar en v6 o documentar como no-op. Destructivo (major) |
| B2 | Estado | ~~Un solo `hover`/`press`/`selected` global~~ | estado en `core->selected` | **resuelto (v6)**: bitset por nodo, varias selecciones; `set_class` sigue por nombre compilado |
| B3 | Lectura | ~~No se puede leer texto/rect ni hacer scroll a un nodo~~ | no había getters | **resuelto (v6)**: `get_text`, `get_rect`, `scroll_into_view`, `focus_input`/`blur_input` |
| B4 | Eventos | ~~Solo botón izquierdo, sin doble clic ni rueda horizontal~~ | `host_win32.c` solo traducía `WM_LBUTTON*` | **resuelto (v6)**: `button`/`click_count`, derecho/central, doble clic y `delta_x` |
| B5 | Tiempo | ~~Sin reloj/timers públicos~~ | bucle solo por eventos | **resuelto (v6)**: `wn_app_set_tick_ms` + `wn_view_tick`; el blink del caret mantiene su timer |
| B6 | Input | ~~Edición ASCII 32..126, una línea, sin IME~~ / sin IME todavía | modelo byte a byte y `WM_CHAR` | **resuelto en la ola 1**: cursor por codepoint, UTF-8, drag-select, blink; IME en E3 |
| B7 | DPI | `dpi_scale` se guarda y no se usa: texto y layout van a píxeles 1:1 | falta escalar fuentes/medidas | aplicar escala en layout+texto (E3). Aditivo |
| B8 | Renderer | `wgpu_native.dll` (9 MB) siempre enlazado aunque D3D11 sea el default | CMake sin interruptor | `WN_NO_WGPU` y cargar backend por vtable. Aditivo |
| B9 | Fuentes | Cualquier genérica (`serif`, `monospace`) se resuelve a Segoe UI; solo se usa la primera familia del stack | `text_runtime.c:resolve_family` | parsear el stack y elegir por disponibilidad. Aditivo |
| B10 | Imágenes | Formatos dependen de los códecs WIC instalados; sin WebP/AVIF garantizado y sin decode asíncrono | decode en UI thread vía WIC | decode en worker + cola; formatos documentados. Aditivo |
| B11 | Input/IME | Sin composición: solo `WM_CHAR` con caracteres ya resueltos; japonés/coreano y teclas muertas no funcionan como en una app nativa | el editor dibuja su propio texto y no habla con IMM32/TSF | `wn_ime_ops` de plataforma (foco, rect del caret/candidatos) + eventos de composición; el core guarda/pinta la composición y confirma una vez. Fake IME para tests; IMM32 primero, TSF después |
| B12 | Accesibilidad | Sin UIA: Narrator/lectores ven una ventana vacía; sin foco por teclado fuera del input | no hay proveedor COM ni roles/nombres accesibles | adaptador UIA sobre el árbol interno (rol, nombre, bounds, estados, foco, Invoke/Value/Scroll) y Tab order. Aditivo |
| B13 | Texto | `font-weight` solo distingue 400 y ≥600; 300/500/600/900 caen en regular/bold | el runtime pide regular o bold | mapear 100–900 a DirectWrite, elegir la cara más cercana y sintetizar si falta |
| D2 | Texto | Si falla el `malloc` de la familia, la entrada de cara queda con `face` ya liberado y `family == NULL`: el siguiente lookup hace `strcmp(NULL, …)` y `destroy` vuelve a hacer `Release()` | `text_directwrite.cpp:216-224` libera `entry->face` y retorna sin limpiar el puntero; `:173` compara `entry->family` sin guarda de NULL | poner `entry->face = NULL` y `entry->family = NULL` en todos los caminos. Aditivo |
| D3 | Renderer | Una sola imagen que no decodifica (path malo o formato sin códec) hace fallar el frame entero; en modo app el bucle cierra la ventana | `ensure_image_texture` devuelve NULL → `renderer_d3d11.c:1194` y `renderer_wgpu.c:1232` devuelven 1 → `wn_view.c:740-744` devuelve `WN_ERR_GPU` → `wn_app.c:101-106` marca `failed` | saltar el batch sin textura (o pintar placeholder) y reportar por nodo, no abortar el frame. Aditivo |
| D4 | Renderer | En offscreen, si `CommandEncoderFinish` falla se libera `gpu->target_view` (que es el `view` del target) y el puntero queda colgando: el frame siguiente lo reusa | `renderer_wgpu.c:1549` aliasa `view = gpu->target_view` y `:1655-1656` lo libera sin distinguir offscreen (ahí `surface_texture.texture` es NULL) | liberar solo si `!gpu->offscreen` y poner `gpu->target_view = NULL`. Aditivo |
| D5 | wgpu | Si `readback` expira (30 s), el callback de `wgpuBufferMapAsync` puede llegar después y escribir en `map_result`, que ya es pila muerta; el buffer se libera con el map pendiente | `renderer_wgpu.c:1453` pasa `&map_result` (local) como `userdata1`; `:1455-1465` abandona con `goto done` y `:1483` retorna | esperar el callback sin timeout, o guardar el resultado en el renderer e invalidar el callback. Aditivo |
| D6 | Blob/layout | Un `.wnui` con una cadena de ~50k nodos desborda la pila: layout, clips y orden de pintado son recursivos por profundidad | `wn_blob.c:178-207` no limita `node_count` y `wn_core.c:402` usa `capacity = max(count, WN_MAX_NODES)`; `layout.c:994`, `:953` y `:967` recursan por hijo | tope de nodos o de profundidad al cargar (rechazar > N) o recorridos con pila explícita. Aditivo |
| D7 | Errores | Varios fallos dejan `wn_last_error()` con el mensaje anterior (o NULL): OOM en `wn_core_create`, OOM del loader y fallo de `wn_view_replace_document`; `wn_view_set_state` devuelve `WN_OK` con un nodo muerto | `wn_core.c:396-503` no llama a `wn_set_error`; `wn_blob.c:212-218` tampoco; `wn_view.c:430-436` retorna 1 en silencio; `wn_view.c:456-464` ignora el nodo (`wn_core.c:218-237`) | setear error en cada salida y devolver `WN_ERR_*` si el nodo no existe (complementa C8). Aditivo |
| D8 | Texto | Un texto de más de 4095 unidades UTF-16 se mide como 0×alto: el layout colapsa y no se pinta nada; `truncated` nunca se enciende | `text_directwrite.cpp:75` pide `-1` (todo o nada) y `:114-121` trata `length == 0` como cadena vacía; `:391` calcula `truncated` sobre un `length` ya fallido | medir por tramos o devolver error explícito y propagar `truncated` desde `measure`. Aditivo |

| C11 | Color | **resuelto**: `argb()` replica tinycss2 1.5.1 (alfa, `hsl`, nombres CSS3) y los colores declarados usan un superset que normaliza a hex antes de emitir el blob | dos gramáticas separadas, corpus de 566 casos | cerrado; el frontend ya no diverge en color |

## P2 — tooling, proceso y contratos

| ID | Área | Síntoma | Causa | Arreglo |
|---|---|---|---|---|
| C1 | CI | No hay CI: wgpu no está versionado y no hay script de descarga | se borró el workflow R1 | workflow con `WN_NO_WGPU` (B8) o fetch con hash |
| C2 | Paridad visual | `benchmarks/visual-parity/tools/render_native.py` apunta a `c-r2` y no hay runner | R1 retirado | retirar a `archive/` o reescribir sobre `wn-cli` |
| C3 | Métricas | `engine-metrics` sin ejecutar desde que se quitó R1 | rama R1 eliminada sin re-baseline | correr `wn-cli bench metrics` y actualizar `BASELINE.md` |
| C4 | Tests | **resuelto**: los suites emiten blobs en `out/` y no regeneran fixtures trackeados | fixtures dentro del repo | cerrado con la consolidación del documento |
| C5 | Compilador | **resuelto**: el frontend Rust emite solo `.wnui`; no hay otro serializador | había dos serializadores | cerrado; el emisor Python se retiró en 2b |
| C12 | Documento | **resuelto (2b)**: runtime blob-only (builtin embebido, loader por memoria), frontend `--emit-blob` único, `wnui-dump` y hashes fijados; fixtures CSS migrados a HTML/CSS | quedaba un árbol JSON intermedio en build y tests | cerrado; ya no existe ningún consumidor del pipeline anterior |
| C6 | Blob | Versión exacta y sin política de compatibilidad | loader exige la versión actual | **pre-alfa: pospuesto a 1.0**; el formato puede romper y subir de versión sin retrocompatibilidad |
| C7 | Hilos | `text_runtime` es singleton global con buffer estático | asume un hilo de UI | documentar o pasar contexto por core |
| C8 | Errores | Mezcla de `-1/-2` y `wn_status`; no hay código estructurado | crecimiento histórico | normalizar a `wn_status` + `wn_last_error` |
| C9 | Input | Valor máximo 255 bytes y sin deshacer/rehacer | buffer fijo | buffer dinámico y undo/redo (E3) |
| C10 | A11y | Sin UIA/ARIA, sin foco por teclado fuera del input, sin contraste alto | E3 pendiente | plan de accesibilidad |
| D9 | Tests | `measure_test` y `atlas_test` imprimen `[FAIL]` pero siempre salen con 0 y el `PASS` es incondicional; `blob_test` tampoco falla si los display lists difieren | `measure_test.c:11-15,88-89` y `atlas_test.c:9-13,83-84` (su `check` no acumula); `blob_test.c:72-75` solo imprime; `suites.py:514,525,608` valida el código de salida y la cadena "PASS" | contador de fallos + exit != 0; comparar también los bytes de vértices, no solo los conteos |
| D10 | Tests | `verify_d3d11.py` imprime FAIL por tamaño distinto pero sale 0, así que la comparación de backends y el blob pasan igual | `verify_d3d11.py:58-64` devuelve el resultado de `_write` y `:120-136` retorna 0 siempre | devolver 1 cuando `failures` no está vacío |
| D11 | Tests | **resuelto** con C12: el paso "legacy header self-compare" comparaba el fixture consigo mismo y se eliminó; la suite CSS fija el hash de cada blob y comprueba sus campos con `wnui-dump --json` | el fixture no probaba nada | cerrado |
| D12 | Host/eventos | Al perder la captura (alt-tab, modal) `press` y `hover` quedan pegados: no se emite `POINTER_UP` ni `POINTER_LEAVE`, y nadie llama a `wn_core_pointer_leave` | `host_win32.c:158-165` hace `SetCapture`/`ReleaseCapture` sin `WM_CAPTURECHANGED`; `:153-157` convierte `WM_MOUSELEAVE` en `POINTER_MOVE` a (-1e6); `hit_test.c:180-185` queda sin uso y `:136-145` no marca `dirty` | manejar `WM_CAPTURECHANGED`/`WM_KILLFOCUS` emitiendo up+leave y marcar `dirty` en el state machine. Aditivo |
| D13 | wgpu | El fallo de init filtra todo lo creado antes de fallar (shaders, pipelines, layouts, sampler, atlas) y no llama `CoUninitialize` | `renderer_wgpu.c:799-801` y el `fail:` de `:821-828` solo liberan surface/queue/device/adapter/instance | liberar cada recurso en `fail:` (o reutilizar un `destroy` parcial). Aditivo |
| D14 | Texto/atlas | Un glifo que no cabe en el atlas queda cacheado como presente con UV (0,0)-(0,0): no se reintenta y pinta un quad degenerado | `text_atlas.c:238` marca `used` y `:244-246` retorna NULL si `pack_bitmap` falla; el lookup `:223-224` lo devuelve `&slot->metrics` para siempre | no marcar `used` hasta empaquetar y reportar el atlas lleno. Aditivo |
| D15 | Paint | Si `realloc` falla, un quad emite solo parte de sus 6 vértices y quedan triángulos basura en la lista | `paint.c:134-149` retorna sin señal y `:151-175`, `:381-393`, `:506-539` llaman a `push_*_vertex` de uno en uno | reservar los 6 (o 3N) vértices antes de emitir, o descartar el quad completo. Aditivo |
| D16 | View | Un callback que destruye la vista o la app desde un evento deja una segunda invocación (`WN_EVENT_INPUT`) sobre memoria liberada | `wn_view.c:689-697` llama `view->callback` dos veces sin revalidar | emitir el semántico primero o copiar y revalidar callback/userdata antes del segundo dispatch. Aditivo |
| D17 | D3D11 | Si `ResizeBuffers` falla, `rtv`/`backbuffer` quedan en NULL con `width/height` ya actualizados: frames deformados o `Present` fallando hasta el próximo resize | `renderer_d3d11.c:1130-1142` ignora el fallo y solo guarda el mensaje | restaurar el tamaño anterior y reintentar, o propagar el error a `wn_view_render`. Aditivo |
| D18 | App | Un segundo `wn_app_run` no reinicia `have_frame`/`startup_ms`; con tamaño 0, `app->frames` consume `max_frames` sin pintar | `wn_app.c:481-485` resetea contadores pero `wn_view.c:759-763` sigue con `have_frame=1`; `wn_app.c:86-89` cuenta el frame antes del early return de `wn_view.c:719-721` | resetear el estado por-run de la vista y no contar frames sin render. Aditivo |
| D19 | View | `wn_view_replace_document(view, doc)` con el mismo `doc` que ya posee la vista destruye el documento que el core nuevo acaba de tomar: punteros colgando | `wn_view.c:433-445` crea el core y luego destruye `view->document` sin comparar con `document` | `if (view->document != document) wn_document_destroy(...)`. Aditivo |

## Lo que ya está maduro

- Texto: medición, shaping y raster en runtime; atlas dinámico; sin glifos en el documento.
- Documento: blob v6 versionado, loader con mmap y por memoria, builtin embebido, hashes fijados por suite, escritor Rust byte-idéntico al de referencia.
- ABI v7: identidad de nodo, estado por nodo (foco/deshabilitado incluidos), getters, eventos con botón, tick y refcount.
- Suites: 10/10 con comparación de píxeles exacta (blob/offscreen), dump y piloto 48/48.

## Validado por la app

La galería (`C:\Proyects\gallery`, `docs/sdk-gaps.md`) usa el SDK para una
biblioteca real: importar carpetas, persistir, clonar tarjetas, filtros, detalle
y navegación. De allí salieron A10 (bug real de medición), A9, B3 (sin foco,
getters ni scroll-to), B4 (sin botón/doble clic), B5 (sin timer), B6 (input
ASCII) y B2 (clases/estado solo en nodos compilados). Los gaps de producto que
no son del motor (diálogo de carpeta, persistencia) están marcados P3 allí.

## Orden recomendado

1. ~~A1, A3, A4~~ (hecho), ~~A6, A7~~ (hecho), ~~B2–B5~~ (hecho en v6).
2. ~~C11~~ (color con tinycss2): hecho; el frontend ya no diverge en color.
3. ~~C5/C12~~ (compilador único y fixtures CSS en HTML/CSS): hechos en la fase 2b.
4. Input v2 (IME, multilínea, undo/redo) e imágenes (decode asíncrono, formatos).
5. A2/A9 ~~hechos~~; quedan A5 (checksum del blob), B7 (DPI), B10 (async),
   C2, C3, C6–C10 (tooling, accesibilidad, undo de UI).
