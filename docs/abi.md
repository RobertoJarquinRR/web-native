# ABI C v7

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
   v2 (wheel), v3 (UI dinámica), v4 (text input), v5 (identidad de nodos),
   v6 (estado, lecturas, eventos, timer, ownership) y **v7** (bits de foco y
   deshabilitado) son aditivas. v7 no cambia el tamaño ni el layout de ningún
   struct público.

## Superficie v7

### Común

| Función | Uso |
|---|---|
| `wn_version()` | versión legible |
| `wn_abi_version()` | `WN_ABI_VERSION` (7) |
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

### Estado por nodo (v6, bits v7)

| Función | Uso |
|---|---|
| `wn_node_set_state(app, node, mask, enabled)` | enciende/apaga `WN_STATE_HOVER/PRESS/SELECTED/FOCUSED/DISABLED` |
| `wn_node_get_state(app, node)` | máscara activa; 0 si el handle está muerto |

- `selected` y `focused` son por nodo: el host puede marcar varios nodos.
  Hover y press siguen siendo exclusivos (un cursor).
- **v7:** `WN_STATE_FOCUSED` (0x08) y `WN_STATE_DISABLED` (0x10) son aditivos.
  `wn_app_focus_input`/`wn_app_blur_input` encienden/apagan el bit de foco en el
  input; el host puede marcar cualquier nodo con `set_state`.
- Un nodo deshabilitado (o dentro de un subárbol deshabilitado) es **inerte** al
  puntero: no recibe hover, press ni selección, y no puede enfocarse. El blob
  v6 siembra el bit desde el atributo `disabled` de `button`/`input`; el host
  puede cambiarlo con `wn_node_set_state`. El
  hit-test de interacción lo omite; `wn_node_get_rect` y el hit-test de
  diagnóstico (`wn_core_node_at`) siguen viéndolo. Al habilitar `DISABLED` se
  limpian hover/press/focus del nodo **y de todo su subárbol**, y se hace blur
  del input enfocado dentro de él (caret, selección y edición se detienen); el
  path de tecla/char/paste también rechaza ediciones si el nodo o un ancestro
  está deshabilitado. Mientras un nodo está deshabilitado,
  `set_state(HOVER|PRESS|FOCUSED, enabled=1)` se rechaza (no-op); apagar bits
  siempre se permite.
- **Prioridad de color** (por campo background/text/border):
  `disabled > press > hover > focus > base`. `selected` es el color base de la
  app: se ve si ningún estado superior define ese campo, así que
  `selected + hover` muestra hover. Los estilos CSS del blob v6 ganan a
  `data-hover`/`data-press`/`data-selected`; estos quedan como fallback.
- El click de selección sigue reemplazando la selección para conservar el
  comportamiento y los píxeles de la v5.
- `wn_app_set_class` sigue siendo por nombre y solo para clases compiladas; no
  hay clases por handle. `"hover"`, `"press"`, `"selected"`, `"focus"` y
  `"disabled"` se interpretan como los bits de estado.

### Lecturas y navegación (v6)

| Función | Uso |
|---|---|
| `wn_node_get_text(app, node)` | texto efectivo (override o compilado); buffer del engine |
| `wn_node_get_rect(app, node, &rect)` | caja de borde en px de vista; fuerza layout si está sucio |
| `wn_node_scroll_into_view(app, node, margin)` | mueve el ancestro scrollable más cercano |
| `wn_app_focus_input(app, node)` | foco programático, caret al final, sin evento |
| `wn_app_blur_input(app)` | quita el foco; idempotente, sin evento |

- El buffer de `get_text` vale hasta la próxima llamada `webnative` en el hilo.
- `scroll_into_view` es vertical (el motor no tiene scroll horizontal), respeta
  el margen cuando cabe y no hace nada si el nodo ya es visible o no hay
  ancestro scrollable.
- `focus_input` solo acepta un `kind:"input"`; sincroniza el blink aunque se
  llame fuera de un evento.

### Timer (v6)

- `wn_app_set_tick_ms(app, ms)` arranca un timer Win32 que pide frame y marca el
  core sucio; `0` lo para. Convive con el timer interno del caret (ids
  distintos).

### Imágenes (v6)

- `wn_node_set_image_size(app, node, w, h)` fija el tamaño intrínseco tras
  `set_image` (0,0 = desconocido); el layout y `object-fit` usan el nuevo ratio.
  Rechaza negativos, NaN e infinito.

### Documentos (v6)

- `wn_document_retain`/`wn_document_release`: refcount. `wn_document_destroy`
  suelta una referencia.
- `wn_core_create` retiene el documento y `wn_core_destroy` lo suelta; se puede
  destruir la referencia del caller justo después de crear el core sin UAF.
- `wn_view_replace_document` mantiene su propia referencia y suelta la anterior.

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
    float delta_x, delta_y;  // wheel (v2); delta_x = rueda horizontal
    wn_node_id node;         // target estable (v5); WN_NODE_INVALID si no aplica
    uint8_t button;          // 0 ninguno, 1 izq, 2 der, 3 central (v6)
    uint8_t click_count;     // 1 normal, 2 doble clic (v6)
} wn_event;
```

- `target_id` solo es válido durante el callback.
- El host embedded decide cuándo llamar `wn_view_event` (su loop).
- El app standalone traduce Win32 → `wn_event` internamente.
- v2 añade `WN_EVENT_WHEEL`; el wheel-forward es `delta_y < 0`.
- v4 añade `WN_EVENT_INPUT = 11`.
- v5 añade `wn_event.node` (handle estable, también para clones).
- v6 añade `button` y `click_count`. El botón derecho/central reporta el nodo
  pero no pulsa ni selecciona; un `POINTER_UP` debe comprobar `button` (0 queda
  reservado para eventos que no son de puntero).
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

## Limitaciones de v7 (importante)

- `wn_app_load` / `wn_view_load` **copian** HTML/CSS por compatibilidad de
  host, pero el runtime renderiza el documento `.wnui` compilado. **No hay
  parser HTML/CSS en runtime**; cambiar la página requiere recompilar el blob
  (`frontend-rust --emit-blob`) o cargar otro `.wnui`.
- `set_text` acepta cualquier UTF-8: el runtime shapea y rasteriza el texto. El
  documento solo lleva cadena, familia, tamaño, peso y `line-height`.
- `set_image` conserva el tamaño intrínseco compilado hasta que el host llame
  `wn_node_set_image_size`; no hay probe WIC automático.
- Input: una línea UTF-8 (shapeada en runtime), sin IME/bidi/CJK/emoji, sin
  multilínea, sin password, sin undo/redo; drag-select y caret blink incluidos.
- `selected` y `focused` ya son por nodo, pero las clases siguen siendo por
  nombre compilado (no hay `set_class` por handle).
- Los estilos de estado (blob v6) sólo cubren `background`, `color` y el color
  de borde uniforme. No hay transiciones, `:checked`, `:focus-visible`,
  `:focus-within`, `:nth-*` ni cambios de layout/opacidad por estado. Una regla
  de estado con otras propiedades, o con el estado en un compuesto ancestro
  (`.card:hover .label`), se diagnostica y no se compila.
- `scroll_into_view` es vertical y mueve solo el ancestro scrollable más cercano.
- Sin virtualización, culling de sólidos, accesibilidad/UIA, DPI/multi-monitor
  ni cross-platform todavía.
- Sin path offscreen en la ABI (sigue en `runtime_r2.exe --render/--states`).
- Sin bindings WPF/WinForms; el host C# es consola/WinExe.
- `wn_app_run`/`wn_view_render` son single-thread; no reentrar.

## Fuera de v7

- Motor de texto en Linux (HarfBuzz/FreeType detrás de la misma vtable).
- IME, bidi, CJK, emoji.
- Virtualización de listas/grids y decode asíncrono de imágenes.
- Accesibilidad/UIA, DPI formal, multi-window, drag & drop, diálogos.
- Modelo de componentes/templates más allá de clone/append.
- Scripting/JS, red/HTTP, iframes, parser runtime.

## Archivo

El contrato compilable está en `include/webnative.h`.
