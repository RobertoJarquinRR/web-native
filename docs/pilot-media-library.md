# Piloto A — Biblioteca de video (sin playback)

## Objetivo

Validar webnative con una app real y visual: una **biblioteca/catálogo de video
local** construida con HTML/CSS + lógica en el host. **Sin reproducción** de
video en el engine.

El playback queda para después y, si se hace, en el **host** (C# + FFmpeg/libmpv)
o como paquete aparte. webnative solo dibuja la UI.

## Alcance

### Pantallas

1. **Biblioteca**: sidebar (Todos, Favoritos, Recientes, Carpetas), búsqueda,
   orden, grid de posters.
2. **Detalle**: poster grande, título, metadata (duración, resolución, tags),
   sinopsis, botón “Abrir con el reproductor del sistema”.
3. **Ajustes** (mínimo): tema claro/oscuro, tamaño de poster, carpeta raíz.

### Features

- Grid de posters con scroll.
- Búsqueda/filtro en vivo.
- Selección + hover + teclado (flechas, Enter, Esc).
- Panel de detalle.
- Favoritos (estado en el host).
- “Abrir con el reproductor del sistema” vía host.
- Catálogo desde JSON local (fase A1) y escaneo de carpeta (fase A2, host).

### Datos

`catalog.json`:

```json
{
  "items": [
    {
      "id": "vid-001",
      "title": "Demo",
      "path": "C:/videos/demo.mp4",
      "duration_ms": 125000,
      "width": 1920,
      "height": 1080,
      "tags": ["demo"],
      "poster": "posters/vid-001.jpg",
      "favorite": false
    }
  ]
}
```

Fase A1 usa posters y metadata precalculados. Fase A2 los genera con FFprobe
(host), sin playback.

## Features del engine que el piloto fuerza

| Feature | v1 | Acción |
|---|---|---|
| `grid` | ❌ | Añadir al subset v2 o resolver con flex-wrap |
| Scroll | ❌ | Necesario |
| `img` (decode/cache) | ❌ | Necesario (WIC/FFmpeg decode o textura del host) |
| Input de búsqueda | limitado | Necesario; IME después |
| Truncado/ellipsis | ❌ | Necesario |
| Virtualización | ❌ | Necesario para 1000+ posters |
| Hover/selección/foco | ✅/parcial | Completar |

Estas features se evalúan como **extensiones del piloto**; las que se usen en
más apps se promueven al subset v2.

## Arquitectura

```
Host (C# en fase 2; C en fase 1)          webnative engine (C)
  catálogo JSON / escaneo (A2)   ───────►  vista HTML/CSS
  favoritos / abrir reproductor  ◄──────   eventos (click, input, key)
  posters / metadata             ───────►  imágenes (o texturas del host)
```

- El host posee el catálogo, el sistema de archivos y “abrir con el SO”.
- webnative posee la vista, el layout, el scroll y la interacción.
- Sin playback: el botón “Abrir” lanza el reproductor del sistema.

## Fases

| Fase | Contenido | Depende de |
|---|---|---|
| **A1** | Catálogo JSON + posters precalculados + grid/búsqueda/detalle | ABI + view/app |
| **A2** | Escaneo de carpeta + FFprobe + posters generados | Host C# + FFmpeg |
| **B** | Playback embebido (opcional, host) | decisión futura |

## Criterios de aceptación (A1)

1. 500–1000 items sin degradación perceptible de scroll.
2. Búsqueda < 50 ms para 1000 items.
3. Selección, hover, teclado y detalle funcionando.
4. “Abrir con el SO” funciona.
5. Startup ≤ 150 ms, install ≤ 15 MB, CPU frame ≤ 2 ms (hardware).
6. El diseñador puede cambiar layout/colores sin tocar C.
7. Diagnósticos del subset: la página del piloto declara sus extensiones.

## Métricas del piloto

- Startup, RSS, CPU frame, frame de scroll.
- Memoria con 1000 posters (cache de imágenes).
- Tiempo del diseñador para cambiar una pantalla.
- Nº de features fuera del subset v1 usadas.

## No-goals

- Reproducción, transcoding, streaming, DRM, HDR, subtítulos.
- Edición de metadata.
- Multi-ventana.
- macOS/Linux (fase 5).
