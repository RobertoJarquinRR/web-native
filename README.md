# webnative

Compilador experimental de **HTML + CSS a render nativo GPU**, sin navegador,
sin WebView, sin Skia y sin Electron.

El contrato central de **R1** es un **display list IR** en JSON: el frontend lo
produce, el emitter lo hornea en datos C + WGSL, y `cl.exe` genera un `.exe`
que renderiza con **wgpu-native** (D3D12 / Vulkan / Metal). **R2** usa un
**árbol de estilos pre-layout** (`ui_data.h`) y calcula el layout en C en
runtime.

## Estructura

```
src/
  ir/                 contrato del IR + schema
  frontend/python/    frontend de referencia / oráculo
  frontend/rust/      frontend canónico (producción)
  frontend/cpp/       alternativa del benchmark
  frontend/csharp/    alternativa del benchmark
  emitter/python/     emit_c.py (IR -> scene_data.h) · emit_ui.py (estilos -> ui_data.h)
  runtime/c-static/   runtime C estático (R1): escena horneada, ventana + PNG
  runtime/c-r2/       runtime C dinámico (R2): layout en runtime, eventos, resize
benchmarks/
  frontend-parity/    fixtures, goldens, verifier y benchmark multi-lenguaje
experiments/          16 experimentos históricos (R2 promovido a src/runtime/c-r2)
spec/                 fixtures originales del contrato
docs/                 arquitectura, IR y convenciones
third_party/          wgpu-native v29.0.1.1
```

## Estado

| Capa | Estado |
|---|---|
| Frontend **Rust** (canónico) | ✅ paridad byte-idéntica, ~10 ms wall, 887 KB |
| Frontend Python v1 (oráculo) | ✅ parse + cascada + block/flex + texto (HarfBuzz/FreeType) |
| Frontend C++ / C# (benchmark) | ✅ alternativas medidas |
| IR | ✅ schema + goldens |
| Emitter C | ✅ `emit_c.py` (rects) y `emit_c_text.py` (texto) |
| Runtime C estático (R1) | ✅ `src/runtime/c-static` — ventana + PNG, escena horneada |
| Runtime C dinámico (R2) | ✅ `src/runtime/c-r2` — layout en runtime + eventos + resize (69/69 checks) |
| AOT HTML → `.exe` | ✅ rects; texto vía `c-static/build_text.ps1` |
| Interactividad | ✅ R2 (`src/runtime/c-r2`); prototipo histórico en `experiments/12` |

## Quick start

```powershell
# R1: HTML/CSS -> .exe con escena horneada (rects)
powershell -NoProfile -ExecutionPolicy Bypass -File src\build.ps1 `
  -Html benchmarks\frontend-parity\fixtures\contract\input.html `
  -OutDir build\rect -Width 400 -Height 300

# R2: build + aceptación del runtime dinámico (layout, reflow, estados, texto)
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build.ps1

# Frontend Python: HTML/CSS -> IR + PNG
cd src\frontend\python
.\.venv\Scripts\python.exe -m webnative ..\..\..\spec\input.html --width 400 --height 300

# Benchmark multi-lenguaje (cuando existan los run.ps1)
python benchmarks\frontend-parity\tools\bench.py --runs 10

# Verificar un IR contra el golden
python benchmarks\frontend-parity\tools\verify_ir.py `
  benchmarks\frontend-parity\golden\contract.ir.json `
  benchmarks\frontend-parity\results\python\contract.ir.json
```

## Documentación

- `docs/architecture.md` — pipeline, contratos y lanes.
- `src/runtime/README.md` — R1 (`c-static`) vs R2 (`c-r2`) y por qué R2 usa el frontend Python.
- `docs/ir-schema.md` — formato del IR.
- `docs/conventions.md` — nombres y reglas del repo.
- `benchmarks/frontend-parity/SPEC.md` — subset exacto para las
  implementaciones C++/Rust/C# y reglas de paridad.

## Principios

1. **Sin navegador.** Nada de WebView, Chromium, Skia ni Electron.
2. **El IR es el contrato.** Cualquier lenguaje puede producir o consumir el IR.
3. **Build time vs runtime.** El frontend puede correr en build time (AOT) o en
   runtime (R2), pero el `.exe` no depende de Python.
4. **Paridad medible.** Mismo input → mismo IR → mismos píxeles.
