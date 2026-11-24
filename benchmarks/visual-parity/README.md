# Visual parity — Chromium headless vs render nativo

Harness de ingeniería para medir **cuánto se parece** el render offscreen de
webnative al de un navegador de referencia con el mismo HTML/CSS del subset.
No es un producto ni un gate de “soportar la web”: es un benchmark repetible
que produce números, capturas y un reporte.

Diseño de referencia: `docs/visual-parity.md` (no se modifica desde aquí).

---

## 1. Alcance

Se compara:

- el mismo HTML/CSS de un fixture estático y autocontenido;
- viewport fijo, DPR 1, 96 DPI, fondo opaco y sRGB;
- layout, color, bordes, radio, texto y posters locales;
- todos los fixtures se compilan y renderizan con el motor (`build.ps1 -Html ...
  -Css ... -OutDir ...` + `runtime_r2.exe --render`); no hay pipeline estático
  paralelo;
- Chromium headless **pinned por Playwright**, nunca Edge/WebView2 instalados.

No se compara:

- webs arbitrarias, red, JS de página, formularios, hover, foco, IME, caret;
- scroll visible, animaciones, HDR, wide gamut ni transparencias de ventana;
- capturas de ventana (DWM/composición del OS). Solo camino offscreen.

La referencia es `chrome-headless-shell` de Playwright 1.56.0:
**Chromium 141.0.7390.37, build 1194**. El pin está en `chromium.json`; el
SHA-256, la ruta exacta y si coincide con el pin (`pin_ok`) se guardan en cada
reporte.

---

## 2. Requisitos (solo desarrollo)

- Windows 10/11.
- Python 3.12 (`py -3.12`).
- MSVC con `vcvars64.bat`, `frontend-rust.exe` ya compilado, `emit_ui.py` y
  `third_party/wgpu-native`. Cada fixture se compila con
  `src/runtime/c-r2/build.ps1 -Html ... -Css ... -OutDir ...`.
- Playwright + Chromium **solo** en este directorio. Nunca en el venv del
  runtime (`src/frontend/python/.venv`) ni enlazado por el engine.

### Instalación del harness

```powershell
py -3.12 -m venv benchmarks\visual-parity\.venv
benchmarks\visual-parity\.venv\Scripts\python.exe -m pip install -r benchmarks\visual-parity\requirements.txt
benchmarks\visual-parity\.venv\Scripts\python.exe -m playwright install chromium-headless-shell
```

`.venv/`, `out/` y los PNG están en `.gitignore`. Los reportes de cada corrida
se archivan en `reports/<timestamp>/report.json|md` (sin imágenes).

Si la descarga de Chromium está bloqueada, el harness falla con:

```text
CHROMIUM NOT AVAILABLE: Playwright Chromium is not installed.
Install the pinned browser with: ... -m playwright install chromium-headless-shell
```

No inventa ni reutiliza Edge/WebView2. El siguiente paso exacto es ejecutar el
`playwright install` de arriba en una máquina con acceso al CDN, o copiar el
caché de Playwright (`%LOCALAPPDATA%\ms-playwright\chromium_headless_shell-1194`).

---

## 3. Fixtures

`fixtures.json` es la única fuente de verdad: viewport, regiones, excludes,
fuentes, tolerancias y modo nativo. **No copia fixtures**: reutiliza los del
repo.

| Fixture | Origen | Viewport | Cobertura | Gate |
|---|---|---:|---|---|
| `flex-page` | `benchmarks/frontend-parity/fixtures/flex-page` | 480×320 | flex anidado, gap, space-between, align | sí |
| `contract` | `benchmarks/frontend-parity/fixtures/contract` | 400×300 | block flow, padding, margin, color | sí |
| `media-library` | `benchmarks/pilot-media-library/page` | 1280×800 | grid, sidebar, detalle, imágenes, texto | no (informativo) |

Regiones actuales:

- `flex-page`: `topbar`, `cards`, `footer` (strict).
- `contract`: `box-a`, `box-b` (strict).
- `media-library`: `sidebar`, `grid`, `detail` (strict) y `text` (modo texto
  con umbral propio + bbox de tinta/IoU).

`exclude` está soportado y **exige `reason`**: una máscara sin razón se rechaza.
Los fixtures actuales no excluyen píxeles.

---

## 4. Comandos

Ejecutar todo:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File benchmarks\visual-parity\run_visual.ps1
```

Un subconjunto:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File benchmarks\visual-parity\run_visual.ps1 -Fixtures flex-page,media-library -EnforceGate
```

Opciones:

| Flag | Uso |
|---|---|
| `-Fixtures a,b` | fixtures (coma o array). Por defecto, todos. |
| `-OutDir <ruta>` | salida alternativa (por defecto `benchmarks/visual-parity/out`). |
| `-SkipChromium` | reutiliza capturas Chromium existentes. |
| `-SkipNative` | reutiliza PNGs nativos existentes. |
| `-RebuildNative` | recompila cada fixture con `build.ps1 -Html ... -Css ... -OutDir ... -SkipRustBuild` antes de renderizar. |
| `-AllowPartial` | continúa si Chromium/nativo falla (genera reporte con ERROR). |
| `-EnforceGate` | sale con código 1 si un fixture `gate=true` falla. |
| `-NoArchive` | no copia el reporte a `reports/<timestamp>/`. |

Salida esperada:

```text
[check] Playwright Chromium + fixture fonts
  chromium 141.0.7390.37 (build 1194)
[check] native renderer prerequisites
  native prerequisites ok
[1/3] flex-page (480x320)
  chromium: ok
  native: ok ...
...
fixture          size            SSIM    diff %       MAE status
flex-page        480x320      0.89847    27.759     4.132 FAIL
contract         400x300            1         0         0 PASS
media-library    1280x800      0.5636    58.752    31.058 INFO
HARNESS OK
```

`HARNESS OK` significa que se midió todo. Un `FAIL` de fixture es un resultado
del benchmark, no un fallo del harness; usa `-EnforceGate` para convertirlo en
código de salida 1. Un error de herramienta/entorno sale con 1 y explica el
siguiente paso.

### Herramientas por separado

```powershell
$py = "benchmarks\visual-parity\.venv\Scripts\python.exe"
& $py benchmarks\visual-parity\tools\capture_chromium.py --check --out benchmarks\visual-parity\out
& $py benchmarks\visual-parity\tools\capture_chromium.py --fixture flex-page `
    --out benchmarks\visual-parity\out\captures\flex-page.chromium.png
& $py benchmarks\visual-parity\tools\render_native.py --fixture flex-page `
    --out benchmarks\visual-parity\out\captures\flex-page.native.png
& $py benchmarks\visual-parity\tools\compare.py --fixtures flex-page --out benchmarks\visual-parity\out
```

`compare.py` también acepta `--reference` y `--native` para un solo fixture.

---

## 5. Salidas

```text
benchmarks/visual-parity/chromium.json # pin de versión/build/SHA del navegador
benchmarks/visual-parity/out/
  report.json / report.md              # resumen combinado
  reports/<name>.report.json|md        # detalle por fixture
  captures/<name>.chromium.png         # referencia (no se commitea)
  captures/<name>.native.png           # nativo (no se commitea)
  meta/<name>.chromium.json|native.json# versión, flags, backend, hashes
  diff/<name>.side-by-side.png         # referencia | nativo
  diff/<name>.diff.png                 # diferencia amplificada x4
  diff/<name>.overlay.png              # diff > 8 en rojo sobre gris
  diff/<name>.<region>.crop.png        # recorte + overlay de regiones FAIL/WARN
  logs/<run>/...                       # stdout/stderr de cada herramienta
  build/r2-<name>/                     # runtime_r2.exe + ui_data.h del fixture
reports/<timestamp>/report.json|md     # archivo de la corrida (sin PNGs)
```

Los reportes archivados conservan las métricas; las capturas y los diff PNG
viven en `out/` y no se commitean. El `README.txt` del archivo apunta a esa
carpeta para no confundir enlaces relativos.

`report.json` incluye: fixture, gate, estado, dimensiones, versiones de
Chromium/Playwright/nativo, SHA-256, backend/adaptador, flags, métricas
globales y por región, umbrales, artefactos, método y caveats.

---

## 6. Métricas

- **SSIM** sobre luma BT.601 con ventana box **8×8** y bordes reflejados;
  media sobre píxeles válidos.
- **Pixel diff %**: píxeles con `max(|dR|,|dG|,|dB|) > 8`.
- **MAE**: media de `|d|` por canal (y por canal R/G/B).
- **BBox del diff**, P99/max, tamaño válido y medias de color.
- **Regiones**: `strict`, `text` y `background`; los `exclude` no cuentan ni
  en global ni en regiones.
- **Texto**: además del SSIM/diff con umbral propio, bbox de tinta e IoU como
  diagnóstico de geometría (baseline, cortes, desplazamientos).
- **Global**: siempre se reporta. Un global bueno con una región mala es FAIL.

Umbrales iniciales (se calibran con la baseline, no son un gate definitivo):

| Ámbito | SSIM | Pixel diff |
|---|---:|---:|
| `strict` rect-only | ≥ 0.995 | ≤ 0.2 % |
| `strict` con imágenes/grid | ≥ 0.980 | ≤ 0.5 % |
| `text` | ≥ 0.950 | ≤ 2 % (+ geometría) |
| Global | ≥ 0.980 | ≤ 1.0 % |

---

## 7. Reproducibilidad

- Playwright 1.56.0 → Chromium **141.0.7390.37**, headless shell build **1194**.
- Flags de Chromium: `--disable-gpu`, `--hide-scrollbars`,
  `--force-device-scale-factor=1`, `--force-color-profile=srgb`,
  `--disable-lcd-text`, sin extensiones/red/audio.
- Viewport y DPR salen del fixture; el script **falla** si el PNG no mide
  exactamente lo declarado. No hay reescalado.
- CSS inyectado y opciones de screenshot desactivan animaciones/transiciones y
  ocultan el caret. Se esperan `document.fonts.ready`, imágenes completas y dos
  rAF antes de capturar.
- Fuentes: allowlist por fixture (`Segoe UI`, `Segoe UI Semibold`, `Consolas`).
  Se comprueban los archivos de `C:\Windows\Fonts`; si falta una, el harness
  falla en vez de comparar un fallback.
- El reporte registra el SHA-256 del ejecutable de Chromium y del nativo, el
  backend/adaptador y si es software (en esta máquina: D3D12 sobre
  **Microsoft Basic Render Driver**, WARP, `type=cpu`).
- El render nativo es la ruta offscreen del motor (`runtime_r2.exe --render`),
  que hoy habla wgpu directamente; no usa todavía la vtable D3D11. El layout
  comparado es el mismo; el pintado se compara contra WARP.

---

## 8. Caveats que hay que leer en cada reporte

1. **Antialiasing de texto.** DirectWrite/GDI y Skia rasterizan distinto; se
   fuerza gris en Chromium, pero el hinting puede mover un píxel. `text` tiene
   umbral propio y geometría de tinta.
2. **DPI.** El gate es 96 DPI / DPR 1. Un monitor al 125/150/200 % es otra
   matriz, no este benchmark.
3. **Fuentes.** Allowlist, sin web fonts, fallo explícito si falta la fuente.
4. **Color.** sRGB 8 bits; un perfil distinto invalida la comparación.
5. **Imágenes.** WIC y Skia remuestrean distinto. El piloto usa posters 180×240
   a 180×240 (sin escalado).
6. **Sombras/blur.** El core aproxima el blur; no hay `box-shadow` en estos
   fixtures. Si se añade, se excluye con `reason` o umbral laxo.
7. **Coordenadas fraccionarias.** El layout admite 0.01 px; el rasterizador
   redondea.
8. **Scroll/overlays.** Scrollbar oculto en Chromium e inexistente en nativo;
   el piloto se captura en scroll 0. `fixed/sticky` necesita fixtures propios.
9. **Composición del OS.** Cero capturas de ventana, DWM o temas.
10. **Backend.** WARP y hardware no son comparables; cada reporte declara
    backend, adaptador y software.
11. **SSIM del harness.** La ventana 8×8 no es la 11×11 gaussiana de otras
    librerías; los números se comparan dentro de este harness.
12. **Dev-only.** Chromium/Playwright/Pillow/NumPy no son dependencia de
    runtime. El engine, la ABI y el install size no cambian.

---

## 9. Sin dependencia en runtime

Todo vive en `benchmarks/visual-parity/`. El engine no enlaza, carga ni invoca
nada del harness. El `.exe` sigue sin navegador, Python o red. Un fallo de
Chromium no bloquea build ni producto; el benchmark es opcional.

---

## 10. Troubleshooting

| Síntoma | Acción |
|---|---|
| `Harness venv not found` | Ejecutar los tres comandos de instalación de §2. |
| `CHROMIUM NOT AVAILABLE` | `...python.exe -m playwright install chromium-headless-shell`. No usar Edge/WebView2. |
| `missing fixture fonts` | Instalar la fuente de la allowlist o quitar el fixture; no se compara un fallback. |
| `r2 build prerequisites missing` | Compilar `frontend-rust`, revisar MSVC/`vcvars64.bat` y `third_party/wgpu-native`. |
| `no runtime_r2.exe found` | Compilar el piloto con `src\runtime\c-r2\build.ps1 -Pilot -SkipRustBuild` o reutilizar el exe existente. |
| `dimension mismatch` | El capturador/backend no respetó el viewport. No se reescala: corregir el fixture o el render. |
| `native check` falla por ediciones concurrentes | Usar el `build_pilot/runtime_r2.exe` existente y reportar el error exacto; no modificar build scripts desde el harness. |

---

## 11. Estado actual (primera baseline)

La primera corrida real sobre esta máquina dio:

- `contract`: **PASS** (SSIM 1.000000, diff 0 %, MAE 0).
- `flex-page`: **FAIL** de gate (SSIM 0.898467, diff 27.759 %, MAE 4.132).
  El nativo pinta las cards con ancho 128 en vez de 152 (padding no sumado al
  box) y el footer con alto 44 en vez de 76; el detalle está en
  `out/reports/flex-page.report.md` y en `diff/flex-page.*.crop.png`.
- `media-library`: **INFO** (SSIM 0.563605, diff 58.752 %, MAE 31.058). La
  diferencia dominante es 4 columnas en Chromium vs 5 en nativo, más el ancho
  del sidebar (box-sizing/flex shrink). Es un hallazgo para el core, no un
  fallo del harness.

Estos números son la baseline informativa; no se convierten en gate hasta
calibrarlos y tener dos corridas verdes consecutivas en `main`.
