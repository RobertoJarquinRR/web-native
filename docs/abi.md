# ABI C v5

Referencia de la superficie pública. El contrato compilable y comentado está
en `include/webnative.h`; este documento explica uso y límites.

## Principios

1. **C ABI pura**: `extern "C"`, sin name mangling, sin tipos de C++/Rust/.NET.
2. **Handles opacos**: `wn_app*`, `wn_view*`, `wn_node_id`; el host no ve structs internos.
3. **Sin GPU en la ABI**: no se expone D3D11, wgpu, D3D12, Vulkan ni Metal.
4. **UTF-8** en todas las strings.
5. **Single-threaded UI**: todas las llamadas y callbacks en el hilo de UI.
6. **Strings copiadas** por el engine; el caller conserva ownership.
7. **Sin excepciones**: todo devuelve `wn_status`; error legible en `wn_last_error`.
8. **Versionado**: `WN_ABI_VERSION` + `wn_version()`; breaking bump de major.
   v2 (wheel), v3 (UI dinámica), v4 (text input) y v5 (identidad de nodos) son
   aditivas.

## Superficie v5

### Común

| Función | Uso |
|---|---|
| `wn_version()` | versión legible |
| `wn_abi_version()` | `WN_ABI_VERSION` (5) |
| `wn_last_error()` | último error del hilo |

### Documentos

| Función | Uso |
|---|---|
| `wn_document_load_blob(path)` | carga un `.wnui`; `NULL` si falla |
| `wn_document_destroy(doc)` | suelta el mapeo del documento |
| `wn_app_load_document(app, path)` | carga el blob en una app viva |

El documento mapea el archivo en solo lectura y vive hasta `destroy`. La app
libera el core anterior, conserva el renderer y devuelve `WN_ERR_IO` si el
blob falta o está corrupto.

### Standalone (`wn_app_*`)

| Función | Uso |
|---|---|
| `wn_app_create(config)` | crea la app (ventana + loop propios) |
| `wn_app_load(html, base_url, css)` | copia HTML/CSS; renderiza el árbol compilado (sin parser runtime) |
| `wn_app_set_text(id, text)` | cambia el texto de un nodo por nombre (legacy) |
| `wn_app_set_class(id, class, on)` | añade/quita una clase por nombre (legacy) |
| `wn_app_on_event(cb, userdata)` | registra callback |
| `wn_app_run()` | corre hasta cerrar; devuelve exit code |
| `wn_app_destroy()` | libera |

### Embedded (`wn_view_*`)

| Función | Uso |
|---|---|
| `wn_view_create(hwnd, w, h, flags)` | adjunta la vista a una ventana existente |
| `wn_view_load(html, base_url, css)` | carga el documento compilado |
| `wn_view_resize(w, h, dpi)` | cambia tamaño/DPI |
| `wn_view_event(event)` | reenvía un evento del host |
| `wn_view_render()` | renderiza y presenta un frame |
| `wn_view_on_event(cb, userdata)` | callback de eventos |
| `wn_view_destroy()` | libera |

### UI dinámica (v3)

`wn_node_id` es un `uint64_t` opaco; `WN_NODE_INVALID` es 0. Dentro lleva la
generación del slot y el índice (`(gen << 32) | (index + 1)`), así que un handle
guardado tras eliminar el nodo se rechaza en vez de apuntar al siguiente
inquilino del slot. No persists handles entre documentos ni tras
`wn_app_destroy`.

| Función | Uso |
|---|---|
| `wn_app_root(app)` | root del documento compilado |
| `wn_app_find(app, name)` | resuelve un nombre compilado (`body.root/div.card`) |
| `wn_app_node_count(app)` | nodos vivos (diagnóstico) |
| `wn_node_set_visible(app, node, visible)` | oculta/muestra el subárbol; fuera de layout, paint y hit test |
| `wn_node_set_image(app, node, path)` | reemplaza el `src` de una imagen; reutiliza la cache |
| `wn_node_set_text(app, node, utf8)` | cambia texto; se shapea y rasteriza en runtime |
| `wn_node_clone(app, template, parent)` | clona un subárbol y lo añade como último hijo |
| `wn_node_remove(app, node)` | elimina el nodo y su subárbol |
| `wn_node_clear_children(app, node)` | elimina todos los hijos |
| `wn_node_append(app, parent, child)` | reparenta un nodo como último hijo |

Toda mutación marca el core dirty: el siguiente frame re-layouta, recorta y
pinta. No hay `commit` separado.

### Text input (v4)

| Función | Uso |
|---|---|
| `WN_EVENT_INPUT` | el valor del input cambió por una acción del usuario |
| `wn_node_set_value(app, node, utf8)` | valor programático; caret al final; **no** emite evento |
| `wn_node_get_value(app, node)` | string del engine, válido hasta la próxima llamada en el hilo |

- UTF-8 de una línea (cursor y selección por codepoint); caracteres de control
  fuera de rango devuelven
  `WN_ERR_UNSUPPORTED`.
- `max_length` compilado (o 255) se aplica a type/paste/set.
- El input debe ser un nodo `kind:"input"` compilado (o un clon).
- El evento se emite en typing, Backspace/Delete y paste/cut; no en mover el
  caret, foco/blur ni `set_value`.
- `get_value` es propiedad del engine; cópialo dentro del callback.

### Identidad y navegación (v5)

- `wn_node_id` es opaco y empaqueta una **generación** por slot: un handle que
  sobrevive a `wn_node_remove` (o a la retirada de un ancestro) deja de resolver.
  Las llamadas `wn_node_*` lo rechazan, la navegación devuelve
  `WN_NODE_INVALID` y los eventos nunca lo reportan.
- `wn_event.node` lleva el handle estable del target, así que los clones son
  direccionables aunque no tengan nombre.
- `wn_node_parent`, `wn_node_first_child` y `wn_node_next_sibling` recorren el
  árbol vivo en O(1) por arista; `parent` de la raíz y `next_sibling` del último
  hijo devuelven `WN_NODE_INVALID`.
- La búsqueda por nombre solo resuelve nodos compilados; los clones se alcanzan
  por handle o por navegación.

## Eventos

```c
typedef struct wn_event {
    wn_event_type type;
    const char *target_id;   // nombre del nodo o NULL; válido solo durante el callback
    float x, y;              // coordenadas de vista
    int32_t key;             // virtual key (Win32 VK en Windows)
    uint32_t codepoint;      // UTF-32 para WN_EVENT_CHAR
    int32_t width, height;   // resize
    uint32_t modifiers;      // WN_MOD_SHIFT/CTRL/ALT/META
    float delta_x, delta_y;  // wheel (v2)
    wn_node_id node;         // target estable (v5); WN_NODE_INVALID si no aplica
} wn_event;
```

- `target_id` solo es válido durante el callback.
- El host embedded decide cuándo llamar `wn_view_event` (su loop).
- El app standalone traduce Win32 → `wn_event` internamente.
- v2 añade `WN_EVENT_WHEEL`; el wheel-forward es `delta_y < 0`.
- v4 añade `WN_EVENT_INPUT = 11`.
- v5 añade `wn_event.node` (handle estable, también para clones).
- No hay `WN_EVENT_CLICK`: un click es `POINTER_DOWN` + `POINTER_UP` sobre el
  mismo nodo.

## Ownership y threading

- El engine **copia** HTML/CSS/strings de entrada.
- `wn_last_error()` y `wn_node_get_value()` devuelven buffers internos; válidos
  hasta la próxima llamada en ese hilo.
- No hay locks internos: una instancia por hilo de UI.
- Destruir en el mismo hilo que creó.
- Los callbacks se invocan en el hilo de UI; no reentrar en `run`/`render`.

## Ejemplo C (standalone + dinámico + input)

```c
#include "webnative.h"
#include <string.h>

static void on_event(void *ud, const wn_event *ev) {
    wn_app *app = (wn_app *)ud;
    if (ev->type == WN_EVENT_INPUT && ev->target_id) {
        wn_node_id input = wn_app_find(app, ev->target_id);
        const char *q = wn_node_get_value(app, input);
        /* filtrar el catálogo con q */
    }
    if (ev->type == WN_EVENT_POINTER_UP && ev->target_id &&
        strcmp(ev->target_id, "card") == 0) {
        wn_node_id poster = wn_app_find(app, "poster");
        wn_node_set_image(app, poster, "posters/vid-002.png");
    }
}

int main(void) {
    wn_config cfg = { .title = "Demo", .width = 800, .height = 600 };
    wn_app *app = wn_app_create(&cfg);
    if (!app) return 1;
    if (wn_app_load(app, html, NULL, css) != WN_OK) return 2;
    wn_app_on_event(app, on_event, app);
    int rc = wn_app_run(app);
    wn_app_destroy(app);
    return rc;
}
```

## Binding C#

```csharp
[LibraryImport("webnative", StringMarshalling = StringMarshalling.Utf8)]
private static partial nint wn_app_create(in wn_config config);

[LibraryImport("webnative")]
private static partial ulong wn_app_find(nint app, string name);

[LibraryImport("webnative")]
private static partial wn_status wn_node_set_image(nint app, ulong node, string path);

[LibraryImport("webnative")]
private static partial nint wn_node_get_value(nint app, ulong node);
```

- `PublishAot=true` para host pequeño.
- Callbacks con `[UnmanagedCallersOnly]` (AOT) o
  `Marshal.GetFunctionPointerForDelegate` (JIT).
- Sin tipos de UI de .NET expuestos a la ABI.
- `wn_node_get_value` se convierte inmediatamente con `Marshal.PtrToStringUTF8`.

## Limitaciones de la base v4/v5 (importante)

- `wn_app_load` / `wn_view_load` **copian** HTML/CSS, pero el runtime renderiza
  el árbol compilado `ui_data.h`. **No hay parser HTML/CSS en runtime**; cambiar
  la página requiere recompilar el árbol o cargar otro `.wnui`.
- `set_text` acepta cualquier UTF-8: el runtime shapea y rasteriza el texto. El
  documento solo lleva cadena, familia, tamaño, peso y `line-height`.
- `set_image` conserva el tamaño intrínseco compilado; no hay probe WIC nuevo.
- Input: una línea UTF-8 (shapeada en runtime), sin IME/bidi/CJK/emoji, sin
  multilínea, sin password, sin undo/redo; drag-select y caret blink incluidos.
- Un solo nodo `selected` a la vez y eventos solo de botón izquierdo; sin
  getters de texto/rect, sin scroll-to ni foco programático (v6).
- El documento no tiene refcount: quien lo crea lo destruye después del core.
- Sin virtualización, culling de sólidos, accesibilidad/UIA, DPI/multi-monitor
  ni cross-platform todavía.
- Sin path offscreen en la ABI (sigue en `runtime_r2.exe --render/--states`).
- Sin bindings WPF/WinForms; el host C# es consola/WinExe.
- `wn_app_run`/`wn_view_render` son single-thread; no reentrar.

## Fuera de v5

- Motor de texto en Linux (HarfBuzz/FreeType detrás de la misma vtable).
- IME, bidi, CJK, emoji.
- Virtualización de listas/grids y decode asíncrono de imágenes.
- Accesibilidad/UIA, DPI formal, multi-window, drag & drop, diálogos.
- Modelo de componentes/templates más allá de clone/append.
- Scripting/JS, red/HTTP, iframes, parser runtime.

## Archivo

El contrato compilable está en `include/webnative.h`.
