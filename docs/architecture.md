# Arquitectura

## Capas

```
┌──────────────────────────────────────────────────────────────┐
│ 1. Frontends                                                 │
│    Rust (canónico) · Python (oráculo) · C++/C# (benchmarks)  │
├──────────────────────────────────────────────────────────────┤
│ 2. IR (contratos)                                            │
│    display_list.json (R1, post-layout)                       │
│    ui.json / ui_data.h (R2, style tree pre-layout)           │
├──────────────────────────────────────────────────────────────┤
│ 3. Emitters (build time)                                     │
│    emit_c.py · emit_c_text.py · emit_ui.py                   │
├──────────────────────────────────────────────────────────────┤
│ 4. Core C                                                    │
│    tree · styles · layout · state · hit test · display list  │
├──────────────────────────────────────────────────────────────┤
│ 5. Renderer backend (vtable)                                 │
│    D3D11 (Windows) · wgpu-native (D3D12/Vulkan/Metal/GL)     │
├──────────────────────────────────────────────────────────────┤
│ 6. Platform host                                             │
│    Win32 standalone · host-provided embedded (HWND)          │
├──────────────────────────────────────────────────────────────┤
│ 7. ABI C                                                     │
│    wn_app_* (standalone) · wn_view_* (embedded)              │
└──────────────────────────────────────────────────────────────┘
```

## Dos modos, un core

| | Standalone | Embedded |
|---|---|---|
| Ventana | la crea webnative | la crea el host |
| Loop | webnative | el host |
| Surface | swapchain propia | HWND/surface del host |
| Render | automático por frame | `wn_view_render()` a demanda |
| Uso | app completa | panel dentro de app existente |

`wn_app_*` es un **host Win32 incluido** sobre `wn_view_*`. No hay dos engines.

## Contratos

| Frontera | Formato | Productor | Consumidor |
|---|---|---|---|
| HTML/CSS → IR R1 | `display_list.json` | frontend | `emit_c` / renderer |
| HTML/CSS → UI tree R2 | `ui.json` | frontend | `emit_ui` |
| IR → datos C | `scene_data.h` / `ui_data.h` | emitters | runtime |
| App ↔ engine | `include/webnative.h` | engine | host (C/C++/Rust/C#/Python) |
| Engine ↔ GPU | vtable `wn_renderer` | core | D3D11 o wgpu |
| Engine ↔ OS | vtable `wn_host` | core | Win32 host |

## Flujos

### R1 estático

```
HTML/CSS → frontend Rust → display_list.json → emit_c(_text)
         → scene_data.h → cl.exe → app.exe → renderer → ventana/PNG
```

### R2 dinámico

```
HTML/CSS → frontend Rust --emit-ui → ui.json (+atlas)
         → emit_ui.py → ui_data.h → cl.exe → app.exe
runtime: eventos → estado → layout C → display list → renderer → ventana
host: ABI v4 → set_visible/set_image/set_text/clone (v3) + input (v4)
```

`renderer` es la vtable `wn_renderer`. En Windows el backend por defecto es
**D3D11 hardware** (`renderer_d3d11.c`) con fallback a **wgpu-native**; en otras
plataformas solo wgpu. `WN_RENDERER=d3d11|wgpu` fuerza uno para A/B.

## Repo

```
src/
  frontend/rust/       canónico
  frontend/python/     oráculo (goldens, fallback)
  frontend/cpp|csharp/ benchmarks archivados
  ir/                  schema + validador
  emitter/python/      emit_c, emit_c_text, emit_ui
  runtime/c-static/    R1
  runtime/c-r2/        R2 + core/ABI (fase 1)
  build.ps1            driver R1
include/webnative.h    ABI C v4 (v3 dinámica + v4 input)
benchmarks/            paridad de frontends + engine-metrics
archive/experiments/           histórico inmutable
docs/                  spec, arquitectura, ABI, métricas, subset, roadmap
third_party/wgpu-native/
```

## Reglas de diseño

1. El **core no conoce** el modo (standalone/embedded) ni el backend GPU.
2. El **IR/UI tree no contiene** conceptos de GPU.
3. La **ABI no expone** wgpu ni Win32 (solo un handle opaco).
4. Los **emitters son backends**: C hoy; binario/Rust después.
5. Los **frontends alternativos no son producción**: viven en benchmarks.
6. **Una instancia por hilo de UI**; sin locks internos.
7. **Sin JS** en el core; scripting opcional como plugin.
8. **Subset cerrado** (`docs/subset-v1.md`); fuera del subset = diagnóstico.

## Paralelización

Los workstreams tocan archivos disjuntos:

| Workstream | Dueño de |
|---|---|
| runtime-platform | `src/runtime/c-r2/**`, `include/**` (worktree) |
| metrics | `benchmarks/engine-metrics/**` |
| subset-diagnostics | `src/frontend/rust/**`, fixtures del subset |
| docs | `docs/**` |

Refactors riesgosos (core/runtime) van en **worktree**; los demás en el árbol
principal con fronteras de archivos. Integración: verificar, merge, commit.
