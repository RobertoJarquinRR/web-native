# Release checklist

Checklist para publicar una versión de webnative. Aplica a la **primera release
pública** y a las siguientes; hoy el proyecto se usa desde el repo, así que el
documento también fija qué significa “release” cuando todavía no hay una.

Principios:

- Un release sale de un **commit etiquetado**, no de un árbol con cambios.
- El tag es **inmutable**: si algo falla, se publica otro patch.
- Nada de binarios generados a mano sin SHA-256 en las notas.
- La release no cambia contratos en silencio: ABI, subset y schemas se declaran.

## 1. Congelar y versionar

- [ ] `main` verde en CI (`frontend-parity`) y sin cambios sin commitear.
- [ ] Elegir versión **semver** `vX.Y.Z`; pre-1.0 mientras la API no sea estable.
- [ ] Confirmar que `wn_version()` devuelve exactamente `X.Y.Z`
      (`src/runtime/wn_common.c`; hoy `0.1.0`).
- [ ] Confirmar `WN_ABI_VERSION` (`include/webnative.h`; hoy `7`) y que el bump,
      si lo hay, está justificado y documentado en `docs/abi.md`.
- [ ] Declarar en las notas: versión de subset (`v1`), schema de UI tree
      (`/1`, `/2`, `/3`, o el nuevo), y versión de `benchmarks/frontend-parity/SPEC.md`.
- [ ] Revisar que `docs/roadmap.md` refleja el estado real (hecho / en curso /
      pendiente) y que los docs nuevos (`e8-css-extensions`, `visual-parity`,
      `release-checklist`) están enlazados donde toca.
- [ ] Crear tag anotado `vX.Y.Z` sobre el commit verificado; no moverlo después.

No hay `CHANGELOG.md` todavía: las notas del release hacen ese papel. Si se
añade, se genera desde los commits convencionales, no a mano.

## 2. Toolchain y requisitos de build

Plataforma soportada: **Windows 10/11 x64**. No hay release ARM64 ni x86.

| Requisito  | Versión / nota                                                                                                                           |
| ---------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| Windows    | 10 1809+ o 11, x64                                                                                                                       |
| MSVC       | Visual Studio 2022/2026 Build Tools, workload **Desktop development with C++**, MSVC x64 + Windows 10/11 SDK; `vcvars64.bat` localizable |
| Rust       | stable (`rustc`/`cargo`); el frontend canónico se compila en release                                                                     |
| .NET SDK   | **10.0.x** solo para el host C#/benchmarks; no es requisito del engine                                                                   |
| Python     | **3.12** + venv para tooling y tests (`requirements.txt`)                                                                                 |
| PowerShell | 5.1 opcional; los orquestadores viven en `archive/ps1-legacy` y el flujo actual es `python tools/cli.py`                                 |
| GPU        | D3D11 o D3D12; WARP como fallback software (más lento, RSS alto)                                                                         |
| Red        | solo en build (`python tools\cli.py setup wgpu`); **ninguna** en runtime                                                                 |

- [ ] Compilar en una máquina limpia (o runner limpio) con esas versiones.
- [ ] Registrar versiones exactas (`rustc -V`, `cl`, `dotnet --version`,
      `python --version`) en las notas.
- [ ] Verificar que no se requiere Python, Rust ni .NET para **ejecutar** un
      `.exe` ya construido.

## 3. Dependencias nativas: wgpu-native

- [ ] Ejecutar `python tools\cli.py setup wgpu`; versión fijada **v29.0.1.1**.
- [ ] Verificar el SHA-256 que imprime el script antes de compilar.
- [ ] Confirmar que `wgpu_native.dll` **no** está commiteada (solo headers en
      `third_party/wgpu-native/`).
- [ ] Para el paquete de release, incluir `wgpu_native.dll` junto al `.exe` o
      documentar que se descarga aparte; nunca mezclar versiones.
- [ ] Confirmar que el backend **D3D11** se enlaza dentro del binario
      (`d3d11.lib`, `dxgi.lib`, `d3dcompiler.lib`) y **no** añade DLL; el
      fallback wgpu sigue necesitando `wgpu_native.dll`.

## 4. Artefactos

Construir y conservar (nombres reales del repo):

| Artefacto                                 | Origen                           | Qué es                                          |
| ----------------------------------------- | -------------------------------- | ----------------------------------------------- |
| `build/release/wgpu_native.dll`           | `python tools\cli.py setup wgpu` | Runtime GPU opcional (`--no-wgpu` no lo necesita) |
| `build/release/runtime_r2.exe`            | `python tools\cli.py build`      | Layout runtime, eventos, `--render`, `--frames` |
| `build/release/standalone.exe`            | `python tools\cli.py build`      | Demo standalone (ABI v8)                        |
| `build/release/embedded.exe`              | `python tools\cli.py build`      | Demo embedded (ABI v8)                          |
| `build/release/webnative.dll` (+ `.lib`)  | `python tools\cli.py build`      | Motor como DLL (ABI C)                          |
| `src/runtime/build/release/abi_smoke.exe` | `python tools\cli.py build`      | Smoke de la ABI                                 |
| `include/webnative.h`                     | repo                             | Contrato C v7 (SDK)                             |
| `src/ir/schema.json`                      | repo                             | Contrato del display list                       |

- [ ] Build reproducible desde el tag: `python tools\cli.py build` (R1 queda
      archivado en `archive/r1-static/` y no se compila).
- [ ] Cada artefacto con su tamaño y SHA-256 en `SHA256SUMS.txt`.
- [ ] Confirmar que las demos `standalone.exe --frames 1` y
      `embedded.exe --frames 1 --input-demo --resize-demo` salen con código 0.
- [ ] No empaquetar `obj/`, `build/` intermedios, venvs, `.pdb` ni fuentes del
      sistema.
- [ ] Decidir si se publica el frontend Rust (`frontend-rust.exe`) como tooling
      del SDK o solo como fuente; si se publica, va con su licencia y version.

## 5. Smoke tests obligatorios

Ejecutar en el commit del tag, en una máquina con GPU si es posible (si no, WARP
y se declara):

```powershell
# Frontend Rust + ABI (header C17/C++17, smoke) + CTest
python tools\cli.py test unit

# Aceptación completa: ABI, CTest, layout, render, estados y texto (69/69)
python tools\cli.py test all

# Piloto: emite, compila y verifica pilot-media-library
python tools\cli.py bench pilot
```

- [ ] R2 **69/69**.
- [ ] `python tools\cli.py test all` encadena Rust, ABI, **CSS 52/52**,
      **fidelity 23/23**, **abi_v6 19**, **dynamic 61/61**, **input 83 + 118/118**,
      **text**, **blob**, **offscreen** (d3d11 vs wgpu) y **dump** (10/10 suites).
- [ ] Los diecisiete blobs fijados (canonical/input/fidelity/pilot/dynamic/cache,
      css_card/css_control/css_none, opacity/opacity_deep, position,
      fixed/sticky/zitem, z y state)
      coinciden con `PINNED_BLOBS`; un byte de diferencia falla el build.
- [ ] Piloto (`python tools\cli.py bench pilot`) y su `verify.py` verdes.
- [ ] Blob: `wn-cli test suite blob` da 28/28 rects y 100.00% de píxeles
      exactos contra el documento builtin, más el caso de input.
- [ ] Un consumidor carga `app.wnui` con `wn_app_load_document` y corre sin
      recompilar `webnative.dll`.
- [ ] Render smoke: `runtime_r2.exe --render 960 640 out.png` genera un PNG del
      tamaño exacto y no vacío.
- [ ] Subset: un fixture con feature fuera de v1 falla con `--strict-subset`
      (exit ≠ 0) y solo avisa sin el flag.
- [ ] `python tools\cli.py test unit` (cargo test + CTest) verde. El snapshot
      de paridad de `benchmarks/frontend-parity` es histórico y no tiene runner.
- [ ] Paridad visual: `docs/visual-parity.md` en modo informativo; se adjunta
      `report.md` si el harness ya existe.
- [ ] D3D11: si el stream del renderer está cerrado, su test de paridad contra
      wgpu debe estar verde y reportar `backend=D3D11`; si el equipo no tiene
      GPU y cae a WARP, se declara en las notas.
- [ ] Métricas de `docs/metrics.md`: install ≤ 15 MB, startup ≤ 150 ms, CPU
      frame ≤ 2 ms. RSS se reporta; en WARP es informativo, no gate.

Un smoke en rojo bloquea la release. Un FAIL conocido se documenta en
“Limitaciones conocidas” y se acepta explícitamente en las notas.

## 6. Licencias y avisos

- [ ] `LICENSE` (MIT) incluido en el paquete.
- [ ] `THIRD_PARTY_NOTICES.md` actualizado:
  - **wgpu-native v29.0.1.1** — MIT OR Apache-2.0; DLL no commiteada; textos en
    `third_party/wgpu-native/LICENSE.MIT` y `LICENSE.APACHE`.
  - **Gumbo 0.10.1** — Apache-2.0; solo en el frontend C++ de benchmark.
  - **Fuentes** — no se redistribuyen; se usan las del sistema.
  - **Crates Rust / paquetes .NET** — declarados en `Cargo.lock` y `*.csproj`.
- [ ] Verificar que no se ha añadido una dependencia nueva sin aviso.
- [ ] Confirmar que el paquete no incluye Chromium, Skia, WebView2 ni runtimes
      de Python/Node.
- [ ] Revisar que las notas no atribuyen el proyecto a wgpu-native/Gumbo
      (trademarks: solo se usan para identificar).

## 7. Capturas y demo

- [ ] Tres capturas mínimo, generadas del commit del tag:
  - biblioteca (grid + sidebar + detalle, 1280×800);
  - detalle con imagen y texto;
  - panel embedded dentro de un host (WinForms/WPF o demo C).
- [ ] Una captura de `--render` offscreen por fixture del subset (contract,
      flex-page, piloto) para reproducibilidad.
- [ ] Demo de 2–3 minutos: abrir, buscar, filtrar, seleccionar, resize,
      teclado/input. Sin edición que oculte fallos.
- [ ] Guardar las imágenes en `docs/` o enlazarlas desde `benchmarks/` con
      fecha/commit; no capturas de una build distinta.
- [ ] Comprobar que las capturas no muestran rutas personales, nombres de
      usuario ni ventanas ajenas.

## 8. Limitaciones conocidas (incluir en las notas)

Estado real según `docs/support.md`; no prometer lo que no hay:

| Área          | Limitación                                                                                                                |
| ------------- | ------------------------------------------------------------------------------------------------------------------------- |
| Plataforma    | Solo Windows x64; macOS/Linux en fase 5                                                                                   |
| Accesibilidad | Sin UIA/ARIA; sin lector de pantalla                                                                                      |
| IME / i18n    | IME IMM32 de una línea (composición/commit/cancel); sin TSF, bidi, CJK fuera del IME ni emoji                        |
| DPI           | System-aware básico; sin `WM_DPICHANGED` ni multi-monitor                                                                 |
| Texto         | Shape/raster/medición en runtime (DirectWrite); `font-weight`, `line-height`, `text-align`; sin wrapping todavía |
| CSS           | `position: relative/absolute/fixed/sticky` (blob v4, enum extendido) con insets px/%/auto (incluido el shorthand `inset`) y `z-index` + stacking contexts (blob v5, también ítems flex/grid static) + estados CSS (blob v7, ABI v7); sin `transform`, `transition`, `animation`, `float`; borders/radius uniformes; una sombra no-inset |
| Layout        | Sin virtualización/culling; grids con `repeat(auto-fill, px)` pilot                                                       |
| Host          | Una ventana por app; sin tray, diálogos, notificaciones ni drag & drop                                                    |
| Renderer      | wgpu/D3D12 por defecto; D3D11 en curso; WARP lento y con RSS alto                                                         |
| Tooling       | Python/Rust/.NET solo en build; el `.exe` no los necesita                                                                 |

- [ ] Cada limitación tiene dueño/fase en `docs/roadmap.md`.
- [ ] Las notas enlazan `docs/support.md` en vez de repetir la tabla entera.
- [ ] Si una limitación se ha cerrado, se quita de aquí y se añade a “Novedades”.

## 9. Publicación

- [ ] Release en GitHub con tag `vX.Y.Z`, título corto y notas:
      qué es, novedades, contratos (ABI/subset/schema), requisitos, artefactos,
      SHA-256, limitaciones conocidas, cómo reportar bugs.
- [ ] Adjuntar zip portable (`.exe` + `wgpu_native.dll` + `LICENSE` +
      `THIRD_PARTY_NOTICES.md` + `include/webnative.h` si es SDK) y
      `SHA256SUMS.txt`.
- [ ] Enlazar capturas/demo y `benchmarks/.../results/README.md` del commit.
- [ ] Anunciar en el canal habitual; no publicar binarios por otro medio sin
      checksum.
- [ ] Verificar la descarga desde otro equipo: descomprimir, ejecutar, cerrar.

## 10. Futuro: instalador y firma de código

Fuera de la primera release (P3 en `docs/platform-v1.md`), pero se planifica:

- **Instalador**: MSIX o WiX/Inno; decidir después de estabilizar la ABI.
  Debe instalar/desinstalar, registrar versión y no tocar el sistema.
- **Firma**: certificado Authenticode; sin firma, SmartScreen mostrará
  “Unknown publisher”. Se puede publicar una preview sin firma **documentando**
  el aviso; no se promete “sin warnings”.
- **Actualizaciones**: canal manual (descargar zip) hasta que haya instalador;
  sin auto-update en v1.
- **Gate de release**: installer + firma son requisito de “usable” para
  release; la firma admite excepción documentada en una preview.

## 11. Rollback

- [ ] Si el smoke falla después de publicar: marcar la release como
      “pre-release” o retirarla, publicar el patch anterior como “latest” y
      abrir issue con el fallo.
- [ ] No borrar el tag; añadir una nota en las release notes.
- [ ] Si el fallo es de contrato (ABI/subset), documentar la migración antes de
      volver a publicar.

## 12. Definición de release hecho

- Tag anotado y build reproducible desde él.
- Smoke tests verdes o FAIL aceptado por escrito.
- Artefactos con SHA-256 y licencias completas.
- Capturas/demo del mismo commit.
- Limitaciones conocidas publicadas y enlazadas.
- Roadmap actualizado con lo entregado y lo que queda.
