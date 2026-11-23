# Arquitectura

## Pipeline

Dos lanes comparten frontend y GPU, pero difieren en **cuándo** se calcula el
layout.

### R1 — escena horneada (AOT, producción actual)

```
[build time]
HTML + CSS
  │  frontend Rust canónico (u oráculo Python)
  ▼
display_list.json            IR portable post-layout (src/ir/schema.json)
  │  emitter (Python)
  ▼
scene_data.h                 vértices C + WGSL
  │  cl.exe + runtime C + wgpu_native.dll.lib
  ▼
app.exe
  │
[runtime]  wgpu_native.dll -> D3D12 / Vulkan / Metal -> ventana
```

### R2 — árbol de estilos + layout en runtime

```
[build time]
HTML + CSS
  │  frontend Python (oráculo: cascada pre-layout)
  ▼
árbol de estilos             nodos + reglas (sin posiciones)
  │  src/emitter/python/emit_ui.py
  ▼
ui_data.h                    UiNode[] + glyphs + atlas R8
  │  cl.exe + runtime_r2.c + wgpu_native.dll.lib
  ▼
runtime_r2.exe
  │
[runtime]  layout en C -> hit testing -> wgpu_native.dll -> ventana
           WM_SIZE -> re-layout + reconfig del swapchain
```

## Contratos

| Frontera | Formato | Productor | Consumidor |
|---|---|---|---|
| HTML/CSS → IR | `display_list.json` (post-layout) | frontend | emitter / renderer |
| HTML/CSS → árbol de estilos | `ui_data.h` (pre-layout) | `emit_ui.py` | `runtime_r2.c` |
| IR → datos C | `scene_data.h` | `emit_c.py` | `runtime_gpu.c` / `runtime_window.c` |
| App → GPU | wgpu-native C API | runtime | `wgpu_native.dll` |

## Lanes

- **Python (runtime, autoría):** parsea, hace cascada, layout, texto y
  renderiza con `wgpu-py` o CPU. Es el oráculo de referencia y la herramienta
  de iteración.
- **C estático (build time, artefacto):** el frontend corre una vez, el IR se
  hornea y `cl.exe` produce un `.exe` sin Python en runtime.
- **C dinámico (build time + runtime):** el build hornea un árbol de estilos;
  el `.exe` calcula layout, reflow y estados en C. Sigue sin Python en runtime.

## Estado (runtime)

- **R1 (`src/runtime/c-static`):** el `.exe` consume una escena horneada.
  Ventana, present y offscreen; sin layout ni eventos. Build:
  `src/build.ps1`.
- **R2 (`src/runtime/c-r2`):** el `.exe` consume un árbol de estilos
  (`ui_data.h`) y calcula layout en runtime; eventos Win32, resize con reflow,
  hit testing y estado hover/press/selected. Sigue usando wgpu-native como
  runtime GPU. Build: `src/runtime/c-r2/build.ps1`.

R1 y R2 **no comparten una interfaz `UiBackend`**: son etapas distintas con
contratos distintos (escena horneada vs árbol de estilos). Unificarlos exige
primero un contrato de datos común.

## Nota: R2 y el frontend Rust

R2 necesita el **árbol de estilos pre-layout**. El frontend Rust canónico
emite un **display list post-layout** (rects y text runs ya posicionados), que
sirve para R1/AOT pero no contiene el árbol ni las reglas para recalcular a
otro viewport.

Por eso `src/emitter/python/emit_ui.py` importa el paquete
`src/frontend/python/webnative` vía `sys.path` (no lo copia) y aplana la
cascada a `UiNode[]`. **Paso pendiente:** extender el frontend Rust para que
emita ese árbol de estilos (o un contrato equivalente) y retirar el oráculo
Python del build de R2. Hasta entonces, R2 es build-time Python + runtime C
puro.

## Regla de oro

Las capas no se importan entre sí: se comunican por archivos versionados.
Cambiar el frontend de lenguaje no debe tocar ni el emitter ni el runtime.
