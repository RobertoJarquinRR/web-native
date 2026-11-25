# Arquitectura

## Capas

```
┌──────────────────────────────────────────────────────────────┐
│ 1. Frontends                                                 │
│    Rust (canónico) · Python (oráculo) · C++/C# (frozen)      │
├──────────────────────────────────────────────────────────────┤
│ 2. IR (contratos)                                            │
│    ui.json / ui_data.h (style tree pre-layout)               │
│    display_list.json (post-layout, solo tooling/paridad)     │
├──────────────────────────────────────────────────────────────┤
│ 3. Emitters (build time)                                     │
│    emit_ui.py                                                │
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
| HTML/CSS → UI tree | `ui.json` | frontend | `emit_ui` |
| UI tree → datos C | `ui_data.h` | emitter | runtime |
| HTML/CSS → IR post-layout | `display_list.json` | frontend | benchmark de paridad con referencias congeladas |
| App ↔ engine | `include/webnative.h` | engine | host (C/C++/Rust/C#/Python) |
| Engine ↔ GPU | vtable `wn_renderer` | core | D3D11 o wgpu |
| Engine ↔ OS | vtable `wn_host` | core | Win32 host |

## Flujo

```
HTML/CSS → frontend Rust --emit-ui → ui.json (+atlas)
         → emit_ui.py → ui_data.h → cl.exe → app.exe
runtime: eventos → estado → layout C → display list → renderer → ventana
host: ABI v4 → set_visible/set_image/set_text/clone (v3) + input (v4)
```

`renderer` es la vtable `wn_renderer`. En Windows el backend por defecto es
**D3D11 hardware** (`renderer_d3d11.c`) con fallback a **wgpu-native**; en otras
plataformas solo wgpu. `WN_RENDERER=d3d11|wgpu` fuerza uno para A/B.

### R1 estático (archivado)

`HTML/CSS → display_list.json → emit_c → scene_data.h → .exe` existió como
primera prueba. Duplicaba layout (en el frontend) y renderer
(`archive/r1-static/runtime`), así que se retiró del producto. Vive en
[`../archive/r1-static/`](../archive/r1-static/README.md); `display_list.json`
sigue generándose para el benchmark de paridad con referencias congeladas.

## Repo

```
src/
  frontend/            canónico en Rust (Cargo.toml, src/, tests/)
  runtime/             motor + core/ABI + renderers
  ir/                  schema + validador
  emitter/python/      emit_ui
tools/
  cli.py + wn_cli/     wn-cli (build/run/test/bench/setup)
include/webnative.h    ABI C v4 (v3 dinámica + v4 input)
benchmarks/            paridad de frontends, paridad visual, engine-metrics
archive/experiments/   histórico inmutable
archive/frontends-legacy/  frontends C++/C#/Python congelados (test-only)
archive/ps1-legacy/    orquestadores PowerShell archivados
archive/r1-static/     R1 horneado (archivado)
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
| runtime-platform | `src/runtime/**`, `include/**` (worktree) |
| metrics | `benchmarks/engine-metrics/**` |
| subset-diagnostics | `src/frontend/**`, fixtures del subset |
| docs | `docs/**` |

Refactors riesgosos (core/runtime) van en **worktree**; los demás en el árbol
principal con fronteras de archivos. Integración: verificar, merge, commit.
