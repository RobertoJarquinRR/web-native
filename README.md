# webnative

Motor experimental de **HTML + CSS a UI nativa GPU**, sin navegador, sin
WebView, sin Skia y sin Electron.

El contrato central es el **árbol de estilos pre-layout** (`ui.json` →
`ui_data.h`): el frontend Rust lo produce en build time, `emit_ui.py` lo
convierte en datos C, y el motor calcula layout, estado y pintado en runtime
sobre una vtable de backend: **D3D11** en Windows y **wgpu-native**
(D3D12 / Vulkan / Metal) como alternativa y fallback.

> **R1 está archivado.** La primera prueba del proyecto —hornear un
> `display_list.json` post-layout en un `.exe` estático— duplicaba renderizador
> y layout. Vive en [`archive/r1-static/`](archive/r1-static/README.md) como
> referencia histórica, fuera del producto y de los tests.

> **Origen:** nació como un experimento personal para usar HTML/CSS como
> lenguaje de vista de apps desktop nativas, sin el peso de Electron/WebView y
> sin aprender otro framework de UI. La historia completa está en
> [`docs/origin.md`](docs/origin.md).

## Qué es / Qué no es

| Es | No es |
|---|---|
| Una plataforma de UI nativa con HTML/CSS como vista | Un navegador o un WebView |
| Un motor C pequeño con ABI C para hosts C#/C/C++/Rust | Un motor JS o una plataforma web |
| Layout, grid, scroll, imágenes y texto en GPU | Un visor de webs content-heavy |
| Un subset cerrado y medible | “Soportar toda la web” |

La lógica de negocio vive en el **host**; el motor solo pone la vista y los
eventos. El detalle de lo que se puede usar hoy está en
[`docs/support.md`](docs/support.md).

## Estructura

```
src/
  ir/                 contrato del IR + schema
  frontend/python/    frontend de referencia / oráculo
  frontend/rust/      frontend canónico (producción)
  frontend/cpp/       alternativa del benchmark
  frontend/csharp/    alternativa del benchmark
  emitter/python/     emit_ui.py (estilos -> ui_data.h)
  runtime/c-r2/       motor C: layout en runtime, eventos, resize, renderer
benchmarks/
  frontend-parity/    fixtures, goldens, verifier y benchmark multi-lenguaje
  visual-parity/      Chromium vs nativo (SSIM, diff, regiones)
archive/
  experiments/        16 experimentos históricos (no mantenidos)
  r1-static/          R1 horneado (archivado): escena estática + su renderer
spec/                 fixtures originales del contrato
docs/                 arquitectura, IR y convenciones
third_party/          wgpu-native v29.0.1.1 (headers; binaries fetched)
tools/                fetch-wgpu.ps1 y utilidades del repo
```

## Frontends

| Frontend | Rol | Alcance |
|---|---|---|
| **Rust** | canónico / producción | subset v1 + extensiones pilot v2/v3 |
| **Python** | oráculo v1 | goldens y paridad; no sigue features nuevas |
| **C++** | referencia de paridad **congelada** | fixtures de `benchmarks/frontend-parity/SPEC.md` |
| **C#** | referencia de paridad **congelada** | fixtures de `benchmarks/frontend-parity/SPEC.md` |

La paridad byte-idéntica entre los cuatro es el argumento central del proyecto:
mismo input, mismo IR, mismos píxeles. C++/C# quedan congelados en el subset de
`SPEC.md`; las features nuevas de Rust (CSS v3, input, etc.) no les obligan a
seguirlas. Los resultados se versionan en
[`benchmarks/frontend-parity/results/README.md`](benchmarks/frontend-parity/results/README.md).

## Estado

| Capa | Estado |
|---|---|
| Frontend **Rust** (canónico) | ✅ paridad byte-idéntica, ~10 ms wall, 887 KB |
| Frontend Python v1 (oráculo) | ✅ parse + cascada + block/flex + texto (HarfBuzz/FreeType) |
| Frontend C++ / C# (benchmark) | ✅ alternativas medidas |
| IR de estilos (`ui.json`) | ✅ schema + goldens |
| Emitter | ✅ `emit_ui.py` (estilos → `ui_data.h`) |
| Runtime C | ✅ `src/runtime/c-r2` — layout, grid, scroll, imágenes y eventos (69/69) |
| ABI C v4 | ✅ standalone/embedded; v3 dinámica + v4 input |
| Dynamic UI (v3) | ✅ `set_visible`, `set_image`, `set_text`, clone/append/remove (53/53) |
| Text input (v4) | ✅ ASCII single-line, caret/selección/clipboard, `WN_EVENT_INPUT` (111/111) |
| CSS v3 (frontend) | ✅ bordes/radius/sombras/unidades |
| CSS core v1 | ✅ radius/borde uniforme + primera sombra (41/41) |
| Renderer | ✅ D3D11 hardware por defecto; wgpu como fallback |
| Host C# (piloto) | ✅ 30 cards, búsqueda real, detalle, filtros, open (113/113) |
| AOT HTML → `.exe` | 🗄️ archivado en `archive/r1-static` (histórico) |
| Interactividad | ✅ R2 (`src/runtime/c-r2`); prototipo histórico en `archive/experiments/12` |

## Soporte y subset

El contrato cerrado es [`docs/subset-v1.md`](docs/subset-v1.md). Las
extensiones del piloto son opt-in con `--pilot`:

| Extensión | Qué añade | Schema |
|---|---|---|
| **v2** | grid, scroll vertical, imágenes, `object-fit` | `ui-tree/2` |
| **v3** | input ASCII, bordes/radius/sombras, unidades relativas | `ui-tree/3` |

Resumen de lo que hay hoy:

| Área | Estado |
|---|---|
| Layout | block, flex, grid, scroll vertical, resize |
| Renderer | D3D11 hardware (Windows, por defecto); wgpu-native (D3D12/Vulkan/Metal) |
| Contenido | texto pre-shapeado, imágenes locales (WIC) |
| Interacción | hover/press/selected, wheel, input de una línea |
| Dinamismo | visibilidad, `set_image`, clone/append/remove |
| CSS visual | radius/bordes/sombras v1; per-side/per-corner/inset pendiente |
| Fuera | JS, iframes, red, parser runtime, formularios web, UIA/IME |

La matriz completa —frontend, runtime y limitaciones— está en
[`docs/support.md`](docs/support.md). `--strict-subset` convierte cualquier
feature fuera del contrato en error.

## Quick start

```powershell
# Dependencias nativas: descarga wgpu-native v29.0.1.1 y verifica su SHA-256
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch-wgpu.ps1

# Build + aceptación del runtime (layout, reflow, estados, texto)
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build.ps1

# El runtime usa D3D11 hardware en Windows. Para comparar con wgpu:
#   $env:WN_RENDERER='wgpu'; ...\build_abi.ps1 ; Remove-Item Env:\WN_RENDERER

# Compilar una página arbitraria a un runtime desechable (tooling/harness)
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-r2\build.ps1 `
  -Html benchmarks\frontend-parity\fixtures\contract\input.html `
  -Css benchmarks\frontend-parity\fixtures\contract\input.css `
  -Width 400 -Height 300 -OutDir build\contract

# Paridad visual contra Chromium (dev-only, requiere Playwright)
powershell -NoProfile -ExecutionPolicy Bypass -File benchmarks\visual-parity\run_visual.ps1

# Frontend Python: HTML/CSS -> IR + PNG
cd src\frontend\python
.\.venv\Scripts\python.exe -m webnative ..\..\..\spec\input.html --width 400 --height 300

# Benchmark multi-lenguaje de paridad (requiere los frontends compilados)
python benchmarks\frontend-parity\tools\bench.py --runs 3

# Verificar un IR contra el golden
python benchmarks\frontend-parity\tools\verify_ir.py `
  benchmarks\frontend-parity\golden\contract.ir.json `
  benchmarks\frontend-parity\results\python\contract.ir.json
```

## Documentación

- [`docs/origin.md`](docs/origin.md) — por qué existe el proyecto.
- [`docs/support.md`](docs/support.md) — matriz de soportado / no soportado.
- [`docs/subset-v1.md`](docs/subset-v1.md) — contrato HTML/CSS v1.
- [`docs/architecture.md`](docs/architecture.md) — pipeline, contratos y lanes.
- [`docs/ui-tree-v3.md`](docs/ui-tree-v3.md) — input y schema `/3`.
- [`src/runtime/c-r2/README.md`](src/runtime/c-r2/README.md) — runtime R2, ABI y tests.
- [`benchmarks/frontend-parity/SPEC.md`](benchmarks/frontend-parity/SPEC.md) — subset de paridad y reglas.
- [`docs/conventions.md`](docs/conventions.md) — nombres y reglas del repo.

## Principios

1. **Sin navegador.** Nada de WebView, Chromium, Skia ni Electron.
2. **El IR es el contrato.** Cualquier lenguaje puede producir o consumir el IR.
3. **Build time vs runtime.** El frontend puede correr en build time (AOT) o en
   runtime (R2), pero el `.exe` no depende de Python.
4. **Paridad medible.** Mismo input → mismo IR → mismos píxeles.

## Licencia

MIT. Ver `LICENSE` y `THIRD_PARTY_NOTICES.md` para las dependencias
(wgpu-native y Gumbo).
