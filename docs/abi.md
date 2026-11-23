# ABI C v0

## Principios

1. **C ABI pura**: `extern "C"`, sin name mangling, sin tipos de C++/Rust/.NET.
2. **Handles opacos**: `wn_app*`, `wn_view*`; el host no ve structs internos.
3. **Sin GPU en la ABI**: no se expone wgpu, D3D12, Vulkan ni Metal.
4. **UTF-8** en todas las strings.
5. **Single-threaded UI**: todas las llamadas y callbacks en el hilo de UI.
6. **Strings copiadas** por el engine; el caller conserva ownership.
7. **Sin excepciones**: todo devuelve `wn_status`; error legible en `wn_last_error`.
8. **Versionado**: `WN_ABI_VERSION` + `wn_version()`; breaking bump de major.

## Superficie v0

### Común

| Función | Uso |
|---|---|
| `wn_version()` | versión legible |
| `wn_abi_version()` | `WN_ABI_VERSION` |
| `wn_last_error()` | último error del hilo |

### Standalone (`wn_app_*`)

| Función | Uso |
|---|---|
| `wn_app_create(config)` | crea la app (ventana + loop propios) |
| `wn_app_load(html, base_url, css)` | carga el documento |
| `wn_app_set_text(id, text)` | cambia el texto de un nodo |
| `wn_app_set_class(id, class, on)` | añade/quita una clase |
| `wn_app_on_event(cb, userdata)` | registra callback |
| `wn_app_run()` | corre hasta cerrar; devuelve exit code |
| `wn_app_destroy()` | libera |

### Embedded (`wn_view_*`)

| Función | Uso |
|---|---|
| `wn_view_create(hwnd, w, h, flags)` | adjunta la vista a una ventana existente |
| `wn_view_load(html, base_url, css)` | carga el documento |
| `wn_view_resize(w, h, dpi)` | cambia tamaño/DPI |
| `wn_view_event(event)` | reenvía un evento del host |
| `wn_view_render()` | renderiza y presenta un frame |
| `wn_view_on_event(cb, userdata)` | callback de eventos |
| `wn_view_destroy()` | libera |

## Eventos

```c
typedef struct wn_event {
    wn_event_type type;
    const char *target_id;   // id del elemento o NULL
    float x, y;              // coordenadas de vista
    int32_t key;             // tecla virtual
    uint32_t codepoint;      // UTF-32 para WN_EVENT_CHAR
    int32_t width, height;   // resize
    uint32_t modifiers;      // WN_MOD_*
} wn_event;
```

- `target_id` solo es válido durante el callback.
- El host embedded decide cuándo llamar `wn_view_event` (su loop).
- El app standalone traduce Win32 → `wn_event` internamente.

## Ownership y threading

- El engine **copia** HTML/CSS/strings de entrada.
- `wn_last_error()` devuelve un buffer interno; válido hasta la próxima llamada
  en ese hilo.
- No hay locks internos: una instancia por hilo de UI.
- Destruir en el mismo hilo que creó.
- Los callbacks se invocan en el hilo de UI; no reentrar en `run`/`render`.

## Ejemplo C (standalone)

```c
#include "webnative.h"
#include <string.h>

static void on_event(void *ud, const wn_event *ev) {
    wn_app *app = (wn_app *)ud;
    if (ev->type == WN_EVENT_CLICK && ev->target_id &&
        strcmp(ev->target_id, "btn") == 0) {
        wn_app_set_text(app, "label", "Hola");
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

## Binding C# (fase 2)

```csharp
[LibraryImport("webnative", StringMarshalling = StringMarshalling.Utf8)]
private static partial nint wn_app_create(in wn_config config);

[UnmanagedCallersOnly]
private static void OnEvent(nint userdata, nint ev) { /* ... */ }
```

- `PublishAot=true` para host pequeño.
- Callbacks con `[UnmanagedCallersOnly]` (AOT) o
  `Marshal.GetFunctionPointerForDelegate` (JIT).
- Sin tipos de UI de .NET expuestos a la ABI.

## Fuera de v0

- Múltiples documentos/ventanas por handle (v1).
- Modelo de componentes/templates.
- Scripting/JS.
- Accesibilidad/IME declarativos.
- Text input completo (v1).
- Networking.

## Archivo

El contrato compilable está en `include/webnative.h`.
