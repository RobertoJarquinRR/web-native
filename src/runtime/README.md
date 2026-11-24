# Runtime nativo (`src/runtime`)

Queda **un** runtime: `c-r2`. R1 vivía en `c-static` y se archivó
([`../../archive/r1-static/`](../../archive/r1-static/README.md)) porque
duplicaba layout y renderer.

| | **`c-r2`** |
|---|---|
| Qué consume | `ui_data.h` (árbol de estilos plano: `UiNode[]`) |
| Dónde se calcula el layout | **Runtime, en C** |
| Reflow al redimensionar | **Sí** (`WM_SIZE` → re-layout completo) |
| Eventos / hit testing | **Sí** (hover / pressed / selected) |
| Texto | Horneado en `ui_data.h` (HarfBuzz/FreeType en build time) |
| Entrada del build | `src/runtime/c-r2/build.ps1` |
| Emisor | `src/emitter/python/emit_ui.py` |
| Frontend | Rust canónico (oráculo Python en `--emit-ui` alternativo) |
| Artefactos | `src/runtime/c-r2/build/` |
| Estado | ✅ PASS (69/69 checks); piloto 48/48 |

## Histórico: R1 — `archive/r1-static`

Runtime estático (escena horneada, sin layout en runtime). Ya no se compila ni
se testea; se conserva como referencia. Detalles en su README.

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
