# webnative

Motor experimental de **HTML + CSS a UI nativa GPU**, sin navegador, sin
WebView, sin Skia y sin Electron.

El contrato central es el **documento `.wnui`** (blob v7): el frontend Rust lo
produce en build time desde HTML/CSS, y el motor calcula layout, estado y
pintado en runtime sobre una vtable de backend: **D3D11** en Windows y
**wgpu-native** (D3D12 / Vulkan / Metal) como alternativa y fallback. La página
builtin va embebida como blob en la DLL y la carga el mismo loader que un
consumidor.

> **Estado:** el runtime es blob-only. El frontend Rust produce `.wnui` con
> `--emit-blob`; el builtin es un blob embebido. `wnui-dump` imprime una
> proyección de debug del blob (texto o `--json`), que **no es un contrato**.
> Todos los fixtures de test, incluidos los CSS, compilan HTML/CSS a `.wnui`.

> **R1 está archivado.** La primera prueba del proyecto —hornear un
> `display_list.json` post-layout en un `.exe` estático— duplicaba renderizador
> y layout. Vive en [`archive/r1-static/`](archive/r1-static/README.md) como
> referencia histórica, fuera del producto y de los tests.

> **Origen:** nació como un experimento personal para usar HTML/CSS como
> lenguaje de vista de apps desktop nativas, sin el peso de Electron/WebView y
> sin aprender otro framework de UI. La historia completa está en
> [`docs/origin.md`](docs/origin.md).

## Qué es / Qué no es

| Es                                                    | No es                            |
| ----------------------------------------------------- | -------------------------------- |
| Una plataforma de UI nativa con HTML/CSS como vista   | Un navegador o un WebView        |
| Un motor C pequeño con ABI C para hosts C#/C/C++/Rust | Un motor JS o una plataforma web |
| Layout, grid, scroll, imágenes y texto en GPU         | Un visor de webs content-heavy   |
| Un subset cerrado y medible                           | “Soportar toda la web”           |

La lógica de negocio vive en el **host**; el motor solo pone la vista y los
eventos. El detalle de lo que se puede usar hoy está en
[`docs/support.md`](docs/support.md).

## Estructura

```
src/
  ir/                 histórico del IR retirado (no producción)
  frontend/           frontend canónico en Rust (Cargo.toml + src/ + tests/)
  runtime/            motor C: layout en runtime, eventos, resize, renderer
tools/
  cli.py              wn-cli: entry point de build/run/test/bench/setup
  wn_cli/             implementación del CLI (comandos y rutas del repo)
benchmarks/
  frontend-parity/    fixtures, goldens, verifier y benchmark del frontend Rust
  visual-parity/      Chromium vs nativo (SSIM, diff, regiones); pendiente de runner
archive/
  experiments/        16 experimentos históricos (no mantenidos)
  frontends-legacy/   frontends C++/C#/Python congelados (referencias de paridad)
  ps1-legacy/         orquestadores PowerShell archivados (sin reemplazo vivo)
  r1-static/          R1 horneado (archivado): escena estática + su renderer
spec/                 fixtures originales del contrato
docs/                 arquitectura, blob, ABI y convenciones
third_party/          wgpu-native v29.0.1.1 (headers; binaries vía wn-cli setup wgpu)
```

## Frontends

| Frontend   | Rol                                      | Alcance                                              |
| ---------- | ---------------------------------------- | ---------------------------------------------------- |
| **Rust**   | canónico / producción                    | `src/frontend`; subset v1 + extensiones pilot v2/v3  |
| **Python** | oráculo v1 **test-only**                 | `archive/frontends-legacy/python`; goldens y paridad |
| **C++**    | referencia de paridad **congelada**      | `archive/frontends-legacy/cpp`; fixtures de SPEC.md  |
| **C#**     | referencia de paridad **congelada**      | `archive/frontends-legacy/csharp`; fixtures SPEC.md  |

La paridad medible es el argumento central del proyecto: mismo input, mismo
blob, mismos píxeles. El gate vivo compara el frontend Rust contra el oráculo Python;
C++/C# quedan congelados en el subset de `SPEC.md` dentro de
`archive/frontends-legacy/` y las features nuevas de Rust (CSS v3, input, etc.)
no les obligan a seguirlas. Los resultados se versionan en
[`benchmarks/frontend-parity/results/README.md`](benchmarks/frontend-parity/results/README.md).

## Estado

| Capa                          | Estado                                                                      |
| ----------------------------- | --------------------------------------------------------------------------- |
| Frontend **Rust** (canónico)  | ✅ paridad byte-idéntica, ~10 ms wall, 1.05 MB                              |
| Frontend Python (oráculo)     | ✅ test-only en `archive/frontends-legacy/python`; no sigue features nuevas |
| Frontend C++/C# (frozen)      | ✅ referencias congeladas en `archive/frontends-legacy`                     |
| Documento `.wnui` (blob v7)   | ✅ schema binario + hash canónico                                            |
| Debug del blob                | ✅ `wnui-dump` (texto/JSON; proyección, no contrato)                        |
| Runtime C                     | ✅ `src/runtime` — layout, grid, scroll, imágenes y eventos (69/69)         |
| ABI C v7                      | ✅ standalone/embedded; v3 dinámica + v4 input + v5 nodos + v6 estado/lecturas + v7 foco/disabled/foco-visible/foco-dentro (bits aditivos) |
| Dynamic UI (v3)               | ✅ `set_visible`, `set_image`, `set_text`, clone/append/remove (61/61)      |
| Text input (v4)               | ✅ UTF-8 single-line, caret/selección/drag/blink, `WN_EVENT_INPUT` (80+118) |
| Texto en runtime              | ✅ DirectWrite, wrapping, atlas dinámico y cachés (66/34/4/40)              |
| CSS v3 (frontend)             | ✅ bordes/radius/sombras/unidades                                           |
| CSS core v1                   | ✅ radius/borde uniforme + primera sombra (52/52)                           |
| Renderer                      | ✅ D3D11 hardware por defecto; wgpu como fallback                           |
| Host C# (piloto)              | ✅ galería real (`C:\Proyects\gallery`, 33/33 selftest) + piloto 48/48       |
| AOT HTML → `.exe`             | 🗄️ archivado en `archive/r1-static` (histórico)                             |
| Interactividad                | ✅ R2 (`src/runtime`); prototipo histórico en `archive/experiments/12`      |

## Soporte y subset

El contrato cerrado es [`docs/subset-v1.md`](docs/subset-v1.md). Las
extensiones v2/v3 del piloto son opt-in con `--pilot`; v4 (position) y v5 (z-index) se aceptan
sin flag:

| Extensión | Qué añade                                              | En el blob   |
| --------- | ------------------------------------------------------ | ------------ |
| **v2**    | grid, scroll vertical, imágenes, `object-fit`          | campos v2    |
| **v3**    | input UTF-8, bordes/radius/sombras, unidades relativas | `inputs` + CSS |
| **v4**    | `position: relative\|absolute\|fixed\|sticky` + `top/right/bottom/left` | `position` + `inset`/`inset_mask` |
| **v5**    | `z-index` y stacking contexts (orden CSS simplificado; también ítems flex/grid static) | `z_index` (INT32_MIN = auto) |
| **v6**    | estados CSS (hover/press/focus/disabled/focus-visible/focus-within, `font-weight` de estado) y `text_extras` (ellipsis) | `states` + `text_extras` (secciones opcionales) |

Resumen de lo que hay hoy:

| Área        | Estado                                                                  |
| ----------- | ----------------------------------------------------------------------- |
| Layout      | block, flex, grid, scroll vertical, resize, relative/absolute/fixed/sticky v4 |
| Renderer    | D3D11 hardware (Windows, por defecto); wgpu-native (D3D12/Vulkan/Metal) |
| Contenido   | texto shapeado en runtime, imágenes locales (WIC, caché LRU)            |
| Interacción | hover/press/selected/focus/focus-visible/focus-within/disabled por nodo, botones/doble clic, wheel, input de una línea + IME IMM32 |
| Dinamismo   | visibilidad, `set_image`, clone/append/remove                           |
| CSS visual  | radius/bordes/sombras v1; `:nth-child`/`:nth-of-type`; ellipsis de una línea; per-side/per-corner/inset pendiente |
| Fuera       | JS, iframes, red, parser runtime, formularios web, UIA                  |

La matriz completa —frontend, runtime y limitaciones— está en
[`docs/support.md`](docs/support.md). `--strict-subset` convierte cualquier
feature fuera del contrato en error.

## Quick start

```powershell
# Dependencias nativas: descarga wgpu-native v29.0.1.1 y verifica su SHA-256
python tools\cli.py setup wgpu

# Build del frontend Rust + runtime; binarios limpios en build\release
python tools\cli.py build

# Suites de verificación (frontend Rust + ABI C + runtime)
python tools\cli.py test all

# Benchmark del piloto: emite, compila y verifica pilot-media-library
python tools\cli.py bench pilot

# Render offscreen de la UI staged con el runtime
python tools\cli.py run -- --render 1280 800 out.png
```

El snapshot de paridad de frontends (`benchmarks/frontend-parity/results/`) se
conserva como historia y **no tiene runner**: sus números no forman parte de los
gates actuales.

La paridad visual contra Chromium (`benchmarks/visual-parity/`) todavía no
tiene runner: su orquestador PowerShell está archivado en `archive/ps1-legacy`
y su tooling Python aún apunta a la ruta antigua del runtime. Hasta que se
restaure un runner, no forma parte del flujo de verificación.

## Documentación

- [`docs/origin.md`](docs/origin.md) — por qué existe el proyecto.
- [`docs/support.md`](docs/support.md) — matriz de soportado / no soportado.
- [`docs/subset-v1.md`](docs/subset-v1.md) — contrato HTML/CSS v1.
- [`docs/architecture.md`](docs/architecture.md) — pipeline, contratos y lanes.
- [`docs/wnui-format.md`](docs/wnui-format.md) — secciones del blob y `wnui-dump` (no contrato).
- [`docs/ui-tree-v3.md`](docs/ui-tree-v3.md) — histórico del IR retirado (input y schema `/3`).
- [`src/runtime/README.md`](src/runtime/README.md) — runtime R2, ABI y tests.
- [`benchmarks/frontend-parity/SPEC.md`](benchmarks/frontend-parity/SPEC.md) — subset de paridad y reglas.
- [`docs/conventions.md`](docs/conventions.md) — nombres y reglas del repo.
- [`tools/cli.py`](tools/cli.py) — `wn-cli`, entry point de build/run/test/bench.

## Principios

1. **Sin navegador.** Nada de WebView, Chromium, Skia ni Electron.
2. **El documento `.wnui` es el contrato.** Cualquier lenguaje puede producir o consumir el blob.
3. **Build time vs runtime.** El frontend puede correr en build time (AOT) o en
   runtime (R2), pero el `.exe` no depende de Python.
4. **Paridad medible.** Mismo input → mismo blob → mismos píxeles.

## Licencia

MIT. Ver `LICENSE` y `THIRD_PARTY_NOTICES.md` para las dependencias
(wgpu-native y Gumbo).
