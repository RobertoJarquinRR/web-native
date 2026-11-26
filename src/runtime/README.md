# Runtime C (R2)

Motor de UI nativa de WebNative. Carga un documento `.wnui`, calcula layout,
estado y pintado en runtime y lo presenta con D3D11 o wgpu-native. No contiene
parser HTML/CSS: el documento compilado es la única entrada.

## Documento

El contrato es el blob `.wnui` v2. El frontend Rust lo produce con
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
