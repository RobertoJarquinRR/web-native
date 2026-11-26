# Runtime C (R2)

Motor de UI nativa de WebNative. Carga un documento `.wnui`, calcula layout,
estado y pintado en runtime y lo presenta con D3D11 o wgpu-native. No contiene
parser HTML/CSS: el documento compilado es la única entrada.

## Documento

El contrato es el blob `.wnui` v4. El frontend Rust lo produce con
`frontend-rust --emit-blob`; el formato y la vista de debug están en
[`docs/wnui-format.md`](../../docs/wnui-format.md) y
[`src/frontend/README.md`](../frontend/README.md).

- `wn_document_load_blob(path)` mapea el archivo en solo lectura y valida
  cabecera, secciones, offsets, jerarquía y rangos. Devuelve `NULL` con
  `wn_last_error()` si el blob es inválido.
- `wn_document_load_blob_memory(data, size)` hace lo mismo sobre un buffer que el
  caller mantiene vivo. El documento lo usa para el builtin embebido.
- `wn_app_load_document(app, path)` reemplaza el documento de una app viva y
  conserva el renderer.
- Los documentos están reference-counted (`retain`/`release`/`destroy`). El
  builtin tiene refcount estático.

El builtin del runtime es `generated/builtin.wnui` embebido como
`generated/builtin_blob.h` (array de bytes). `wn_document_builtin()` lo parsea
con el mismo loader que un archivo, una sola vez por proceso. Ya no existe
ninguna tabla de nodos generada en C.

Para regenerar el builtin:

```powershell
python tools\cli.py build --emit     # frontend + blob + CMake (incluye wnui-dump)
python tools\cli.py runtime emit     # solo el blob y el header embebido
```

## Módulos

```
src/runtime/
  runtime_r2.c        host standalone (ventana, loop, CLI --render/--report/--frames)
  wn_view.c/.h        API embedded: crear/destruir, cargar, eventos, render, readback
  wn_app.c/.h         capa de app sobre wn_view (documento, input, focus)
  host_win32.c        ventana Win32, WndProc, timers, DPI básico
  renderer_d3d11.c    backend D3D11 (por defecto en Windows)
  renderer_wgpu.c     backend wgpu-native (D3D12/Vulkan/Metal)
  renderer_wgpu_stub.c stub cuando se compila con WN_WGPU=OFF
  wn_renderer.h       vtable de renderer
  wn_core.h           tipos del core (UiNode, handles, eventos)
  wn_common.c         errores thread-local, utilidades
  core/
    wn_core.c         documento -> arena mutable, names/classes, dirty flag
    layout.c          block/flex/grid, scroll, imágenes, texto
    kind_ops.c/.h     tabla de capacidades por tipo de nodo (auto-size, intrínseco, hijos)
    paint.c           paint items ordenados (clip, capa, transform, opacidad) + batches
    scroll.c/.h       servicio x/y: clamp, aplicar y relayout (X reservado)
    input.c           editor de una línea: caret, selección, clipboard
    hit_test.c        hover/press/selected, click/doble click, wheel
  document/
    wn_blob.c         loader del .wnui (archivo y memoria)
    wn_blob.h         estructuras on-disk
    wn_document_format.h   UiNode/UiInput internos
    wn_document_internal.h refcount y storage del documento
  text/
    wn_text.h         vtable: measure, shape, rasterize
    text_directwrite.cpp backend Windows
    text_runtime.c    instancia única, familias genéricas
    text_atlas.c      atlas dinámico y caché de glifos/runs
  tools/
    wn_dump.c         wnui-dump: proyección de debug del blob (no contrato)
```

## Flujo por frame

```
documento .wnui
  → core (nodos mutables, estado, clases)
  → layout (mide texto, resuelve flex/grid/scroll/imágenes)
  → display list (paint)
  → renderer (D3D11 o wgpu) → surface
```

Los cambios de estado (`set_visible`, `set_text`, `set_image`, clones, input)
marcan el core dirty; el siguiente frame re-layouta y repinta. `wn_view_render`
es síncrono y single-thread: no reentrar.

### Capas de opacidad

Todo nodo con `opacity < 1` es un grupo CSS: su item y su subárbol se pintan en
una textura offscreen y se componen una sola vez. Las hojas también, porque
pueden pintar varias pasadas (borde+fondo, sombra+relleno, cuerpo/ring/
selección/caret de un input, texto o imagen sobre fondo).

- **Memoria:** un target de viewport completo por nivel de anidamiento,
  `W × H × 4` bytes. Los targets se reservan por profundidad, se reutilizan
  entre frames y el pico es `profundidad × W × H × 4` (más la surface). El
  clip del grupo recorta al componer, no reduce la textura.
- **Orden:** sin grupos, contextos z ni posicionamiento se mantiene el orden
  plano histórico por categorías (todos los sólidos, luego imágenes, luego
  glifos). Con al menos un grupo, un contexto `z-index` o un nodo posicionado,
  el replay de comandos dibuja por item (sólido/decoración/imagen/texto) en el
  orden de stacking para contener el subárbol completo en cada capa; es el
  orden más cercano a CSS. Un grupo con clip vacío no reserva textura.
- **Límites:** `WN_MAX_PAINT_LAYER_DEPTH` (512, igual que la profundidad del
  documento) y `WN_MAX_PAINT_LAYERS` (65536). El pool wgpu/D3D11 se indexa por
  profundidad, no por cantidad de grupos hermanos.

### Position (blob v4)

`relative` se dispone en el flujo normal y después se traslada el nodo y su
subárbol (`left`/`top`, o `-right`/`-bottom`); el cursor del padre no cambia y
los hermanos conservan su sitio. `absolute` sale del flujo: block/flex/grid lo
saltan, el containing block es el ancestro posicionado más cercano (o el
viewport cuando no hay ninguno) y los insets se resuelven contra su border box (el layout ignora bordes, así
que coincide con el padding box). Una segunda pasada tras el
layout normal coloca los absolutos cuando todos los containing blocks ya son
finales; `left+right`/`top+bottom` con tamaño `auto` estiran y los porcentajes
se resuelven ahí, no en el frontend. Scroll, clip y hit-test usan los rects
resultantes; el clip del absoluto es el de los hijos de su containing block
(un `overflow` estático intermedio que no es containing block no lo recorta).
`fixed` y `sticky` quedan fuera de esta lane.

### Estados CSS (blob v6, ABI v7)

El frontend compila `:hover`, `:active` (press incluye hover), `:focus` y
`:disabled` a cuatro estilos efectivos de `background`, `color` y color de
borde. `disabled`/`focus` del nodo son bits (`WN_STATE_DISABLED`,
`WN_STATE_FOCUSED`); el input enciende el foco al editar y lo apaga al hacer
blur. La prioridad por campo es `disabled > press > hover > focus > base`, con
`selected` como base de la app y `data-*` como fallback. Un nodo deshabilitado
(o su subárbol) es inerte al puntero pero sigue visible en `wn_node_at` y
`wn_node_get_rect`.

### Z-index y stacking contexts (blob v5)

Un contexto lo crea la raíz, un nodo con `opacity < 1` o un nodo posicionado
con `z-index != auto`. Dentro de cada contexto se pinta: su propio ítem,
contextos hijos con `z < 0` (ascendente), contenido en flujo en orden de árbol
(solo ítems **no** posicionados que no son contexto), bloques con `z == 0`/`auto`
en orden de árbol (paso 8 de CSS, por encima del flujo: **todos** los grupos de
`opacity < 1` —posicionados o no, CSS Color 3 §3.2 los pone en la capa de un
posicionado con `z: 0`—, contextos posicionados `z: 0` e ítems `z: auto`) y
contextos con `z > 0` (ascendente). El
bloque de un contexto es contiguo: sus descendientes no escapan del padre.
`z-index` en un nodo `static` se ignora (sin ítems flex/grid); un posicionado
con `z: auto` se emite como bloque atómico del paso 8 (sus posicionados no se
unen al contexto abuelo, simplificación documentada). Un contexto z-only no
reserva textura: la capa solo existe para ordenar y el renderer la reproduce
inline (`needs_texture=0`); un grupo de opacidad sí reserva target y compone.
Un grupo recortado a nada no reserva textura, y un absoluto que escapa conserva
su clip no vacío (sin atenuarse por ese grupo: limitación conocida). El
hit-test recorre los ítems al revés, así que gana el z más alto.

El texto no está horneado. El nodo lleva cadena, familia, tamaño, peso y
`line-height`; DirectWrite mide y shapea, y `text_atlas` cachea glifos y runs.
Los inputs se editan en `core/input.c` y su spec (`value`, `placeholder`,
`max_length`, `password`) llega en la sección `inputs` del blob.

## Renderers y host

`wn_renderer` es una vtable. En Windows el backend por defecto es **D3D11
hardware**; **wgpu-native** es la alternativa/fallback. `WN_RENDERER=d3d11` o
`WN_RENDERER=wgpu` fuerza uno para A/B. `WN_WGPU=OFF` compila sin wgpu (D3D11
solo) y enlaza el stub.

Dos modos, un core:

| | Standalone | Embedded |
|---|---|---|
| Ventana | la crea `runtime_r2` | la crea el host |
| Loop | del runtime | del host |
| Surface | swapchain propia | HWND/surface del host |
| Render | automático | `wn_view_render()` a demanda |

La ABI pública está en [`include/webnative.h`](../../include/webnative.h)
(v6: handles estables, estado por nodo, getters, input, refcount). El detalle
está en [`docs/abi.md`](../../docs/abi.md).

## Tests

```powershell
python tools\cli.py test all                 # wgpu + D3D11
python tools\cli.py test all --no-wgpu       # solo D3D11
python tools\cli.py test suite blob          # .wnui load, layout, píxeles
python tools\cli.py test suite dump          # golden de wnui-dump
python tools\cli.py test suite fidelity      # E1 layout + renders
python tools\cli.py test unit                # cargo test + ctest
```

Suites de `tools/wn_cli/suites.py`:

| Suite | Cubre |
|---|---|
| `css` | radios/borde/sombra e imagen; HTML/CSS → blob, campos leídos con `wnui-dump --json` |
| `fidelity` | E1: font-weight, text-align, line-height, margin auto, flex |
| `dynamic` | visibilidad, clone/append/remove, demo ABI |
| `input` | caret, selección, clipboard, demo ABI |
| `offscreen` | readback y caché de imágenes, D3D11 vs wgpu |
| `text` | medición DirectWrite, atlas, wrapping, documento |
| `blob` | loader, builtin vs archivo, píxeles; invariante `max=0` |
| `dump` | `wnui-dump` texto/JSON, golden y errores de archivo |

El gate primario del documento es el suite `blob`: el `app.wnui` canónico debe
mantener su SHA-256 y renderizar idéntico al builtin. `wnui-dump` es una
proyección de debug; su golden detecta regresiones del dump, no define el
contrato.

## Reglas

1. El core no conoce el modo (standalone/embedded) ni el backend GPU.
2. La ABI no expone wgpu ni Win32 (solo handles opacos).
3. Una instancia por hilo de UI; sin locks internos.
4. Sin JS, red, iframes ni parser HTML/CSS en runtime.
5. Subset cerrado (`docs/subset-v1.md`); fuera del subset = diagnóstico en
   build time.
