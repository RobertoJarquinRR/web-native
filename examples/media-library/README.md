# Media Library — proyecto de ejemplo

Proyecto de ejemplo para webnative: una **biblioteca de video local** con
sidebar, búsqueda, grid de posters y panel de detalle. Sin playback: la
reproducción vive en el host (o en un paquete aparte).

Este directorio es un **diseño de proyecto**, no una app compilada todavía:
las features de motor que necesita (grid, scroll, imágenes) están en la ola A.

## Estructura

```
media-library/
  app/
    index.html        vista (subset + extensiones del piloto)
    styles.css        diseño
    catalog.json      datos del catálogo (host)
    posters/          posters generados (make_posters.py)
  host/
    Program.cs        sketch del host C# sobre la ABI (bindings pendientes)
  design/
    mockup.svg        cómo se vería la app
  make_posters.py     genera posters placeholder
  README.md
```

## Cómo se ve

Abre `design/mockup.svg` en el explorador. Es el layout objetivo:

- sidebar 220 px (Todos, Favoritos, Recientes, Carpetas, Ajustes);
- topbar con búsqueda, orden y toggle de vista;
- grid 4 columnas de posters 160×214, con scroll;
- panel de detalle 300 px con poster, título, metadata, tags y acciones.

La app A0 usa una ventana fija de **1280×800**; A1 hará el layout responsive.

## Pipeline completo

```
[autoría]
  app/index.html + styles.css + catalog.json + posters/
        │
        │  frontend Rust: parse + cascada + --emit-ui
        ▼
  ui.json (style tree v2) + atlas
        │
        │  emitter Python: emit_ui.py --ui-json
        ▼
  ui_data.h (nodos + estilos + texto + imágenes)
        │
        │  cl.exe + runtime R2 + wgpu-native
        ▼
  media-library.exe  (ventana nativa)
        │
[host]
  catalog.json -> host -> ABI (set_text/set_class/eventos)
  no playback en el engine
```

### Comandos (cuando la ola A esté integrada)

```powershell
# 1. UI tree desde la vista
src\frontend\rust\target\release\frontend-rust.exe --emit-ui `
  examples\media-library\app\index.html `
  --width 1280 --height 800 `
  --out examples\media-library\build\ui.json

# 2. Datos C
src\frontend\python\.venv\Scripts\python.exe `
  src\emitter\python\emit_ui.py --ui-json examples\media-library\build\ui.json `
  --out examples\media-library\build\ui_data.h

# 3. Compilar runtime + host
cl /O2 /TC src\runtime\c-r2\runtime_r2.c ... ui_data.h ...
```

El host C# se conectará con la ABI v1/v2 (`LibraryImport`), cargará el catálogo
y actualizará la vista. La lógica no entra al engine.

## Separación UI / lógica

| Engine (C) | Host (C#) |
|---|---|
| layout, estilos, scroll, imágenes | catálogo, búsqueda, favoritos |
| eventos, estado visual, render | abrir reproductor del SO |
| ABI `wn_view_*` / `wn_app_*` | bindings y lógica de app |

## Estado

- Motor: R2 con layout runtime, eventos, resize, texto.
- Ola A en curso: grid, scroll/clipping, imágenes, wheel.
- Pendiente: bindings C#, input de búsqueda, lista dinámica/virtualización.
- Sin playback; sin red; sin DRM.

## Limitaciones del ejemplo

- Ventana fija 1280×800 (A0).
- 12 posters placeholder; el catálogo real tendrá 24–36.
- `catalog.json` es estático en A0; A1 lo llena el host.
- `host/Program.cs` es un sketch; los bindings C# aún no existen.
