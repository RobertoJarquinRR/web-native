# Runtimes nativos (`src/runtime`)

Dos runtimes C comparten `third_party/wgpu-native` y **no comparten código**:
son dos etapas distintas del mismo proyecto, no dos backends intercambiables.

| | **R1 — `c-static`** | **R2 — `c-r2`** |
|---|---|---|
| Qué consume | `scene_data.h` (escena ya horneada: vértices + WGSL) | `ui_data.h` (árbol de estilos plano: `UiNode[]`) |
| Dónde se calcula el layout | Build time (frontend Rust → IR) | **Runtime, en C** |
| Reflow al redimensionar | No (geometría fija; solo se reescala el swapchain) | **Sí** (`WM_SIZE` → re-layout completo) |
| Eventos / hit testing | No | **Sí** (hover / pressed / selected) |
| Texto | Horneado en el display list (glyphs + atlas) | Horneado en `ui_data.h` (HarfBuzz/FreeType en build time) |
| Entrada del build | `src/build.ps1 -Html <fixture>` | `src/runtime/c-r2/build.ps1` |
| Emisor | `src/emitter/python/emit_c.py` (o `emit_c_text.py`) | `src/emitter/python/emit_ui.py` |
| Frontend | Rust canónico (u oráculo Python) | **Python (oráculo)** — ver nota |
| Artefactos | `build/<OutDir>/` | `src/runtime/c-r2/build/` |
| Estado | ✅ PASS (rects; texto vía `build_text.ps1`) | ✅ PASS (69/69 checks) |

## R1 — `src/runtime/c-static`

Runtime estático: el `.exe` reproduce una escena ya resuelta. Es el camino
canónico de producción actual y el que usa `src/build.ps1`.

- `runtime_gpu.c` — offscreen PNG (WIC).
- `runtime_window.c` — ventana Win32 + swapchain (rects; texto opcional).
- `runtime_gpu_text.c` — offscreen con texto.
- `build_text.ps1` — build + verificación completa de texto (no sustituye a
  `src/build.ps1`).

## R2 — `src/runtime/c-r2`

Runtime dinámico: el `.exe` recibe un **árbol de estilos pre-layout** y hace
layout, hit testing, estados y reflow en C, con una ventana Win32 real.

- `runtime_r2.c` — motor de layout + host Win32 + wgpu-native + offscreen/CLI.
- `page/` — fixture (sidebar 30 %, content 70 %, header, 3 cards, panel).
- `build.ps1` — emite `ui_data.h`, compila con MSVC, copia `wgpu_native.dll`,
  ejecuta report/render/states/ventana y corre `verify.py`.
- `verify.py` — 69 checks de layout, píxeles, texto y estados.
- `README.md` — arquitectura, comandos, resultados y limitaciones.

Comando:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build.ps1
```

Salida esperada: `src/runtime/c-r2: PASS`, `69/69 checks PASS`, exit 0.

## Nota importante: por qué R2 usa el frontend Python

R2 necesita un **árbol de estilos pre-layout** (nodo, kind, dirección, flex,
`%`, `gap`, colores, `data-*`). Eso es exactamente lo que produce la cascada
del frontend Python (`src/frontend/python/webnative`) antes del layout.

El frontend **Rust** canónico emite un **display list post-layout** (rects y
text runs ya posicionados), que sirve para R1/AOT pero **no** para R2: al
runtime le faltaría el árbol y las reglas para recalcular a otro viewport.

Por eso `src/emitter/python/emit_ui.py` importa el paquete
`src/frontend/python/webnative` vía `sys.path` (no lo copia) y aplana el árbol
con estilos a `UiNode[]`. **Paso pendiente:** extender el frontend Rust para
que pueda emitir ese mismo árbol de estilos (o un contrato equivalente) y así
quitar Python del build de R2. Hasta entonces, R2 es build-time Python +
runtime C puro.

## Regla de oro

Los runtimes no se importan entre sí ni comparten una interfaz `UiBackend`.
R1 consume escena horneada; R2 consume árbol de estilos. Cada uno tiene su
propio build y sus propios artefactos. Unificarlos exige primero un contrato
común de datos, no una abstracción prematura.
