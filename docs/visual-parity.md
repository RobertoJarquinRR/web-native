# Paridad visual — benchmark Chromium headless vs render nativo

Objetivo: medir con números **cuánto se parece** el render de webnative al de un
navegador de referencia para el mismo HTML/CSS del subset, sin convertir el
navegador en dependencia del producto. Es un benchmark de ingeniería, no un
gate de “soportar la web”.

Este documento define el diseño. La implementación vivirá en
`benchmarks/visual-parity/` y no toca el runtime, la ABI ni el install size.

## 1. Qué se compara y qué no

| Se compara | No se compara |
|---|---|
| Mismo HTML/CSS del subset v1 + pilot | Webs arbitrarias o content-heavy |
| Viewport fijo, DPR 1, sin animaciones | Scroll, hover, foco, caret, IME |
| Fondo opaco y color sRGB | HDR, wide gamut, transparencias de ventana |
| Layout, color, bordes/radius/sombras, texto, imágenes | Comportamiento del navegador (JS, red, forms) |
| R1 (escena horneada) y R2 (layout runtime) | WebView2/Electron como referencia |

La referencia es **Chromium headless** (`chrome-headless-shell`) fijado por
versión y SHA. Se descarta WebView2: depende del runtime del OS, no es
reproducible entre máquinas y no expone flags de rasterizado. Se descarta
“Edge instalado”: misma base, cadencia distinta.

## 2. Flujo

```
fixture/index.html + style.css + mask.json
        │
        ├─► Chromium headless ── screenshot PNG  (referencia)
        │     --disable-gpu --force-device-scale-factor=1
        │     --force-color-profile=srgb --hide-scrollbars --disable-lcd-text
        │
        └─► frontend Rust ─► IR / ui.json ─► runtime (R1/R2) ─► PNG nativo
                                                      │
        referencia ◄──────── compare.py ─────────────►│
                    SSIM + pixel diff + máscaras
                              │
                        report.json / report.md / diff/*.png
```

Reglas del flujo:

1. **Mismo input, mismo viewport.** El fixture declara `width × height`; ambos
   capturadores lo respetan y fallan si la imagen no mide exactamente eso.
2. **PNG sin pérdida, sin escalado.** Nada de reescalar para “que cuadre”.
3. **Un backend nativo por corrida, registrado.** Se reporta
   `wgpu/D3D12/WARP` o `D3D11/hardware`; no se mezclan en la misma comparación.
4. **Nada de capturas de ventana** para el gate: solo offscreen. La captura de
   ventana del stream del renderer sirve para validar ese backend, no para
   paridad visual, porque incluye composición del OS.
5. **Determinismo:** sin animaciones, sin caret, sin foco, sin scroll, fuentes
   de una allowlist, reloj congelado.

## 3. Capturadores

### 3.1 Chromium

```powershell
chrome-headless-shell.exe `
  --disable-gpu --hide-scrollbars `
  --force-device-scale-factor=1 --force-color-profile=srgb `
  --disable-lcd-text --window-size=1280,800 `
  --screenshot=out/chromium.png file:///.../fixture/index.html
```

- `--disable-gpu`: rasterizado software (Skia CPU). Es más lento y es lo que
  queremos: elimina la variación de drivers.
- `--disable-lcd-text`: fuerza antialiasing en escala de grises. Comparar
  subpíxel RGB contra un rasterizador distinto produce diferencias de color que
  no son bugs de layout.
- `--force-color-profile=srgb` y fondo opaco en el CSS del fixture.
- Versión y SHA en `chromium.json`; la descarga la hace el harness, no el repo.

### 3.2 Nativo

```powershell
# R1
powershell -NoProfile -ExecutionPolicy Bypass -File src\build.ps1 `
  -Html benchmarks\visual-parity\fixtures\<name>\index.html `
  -OutDir build\visual\<name> -Width 1280 -Height 800

# R2 (tras build.ps1 del runtime)
.\runtime_r2.exe --render 1280 800 out_1280.png
```

- R1 y R2 deben producir la misma imagen para fixtures sin texto ni estados
  dinámicos; si divergen, se reporta como hallazgo, no se promedia.
- Se usa el camino offscreen disponible (`--render`); no se captura la ventana.
- Se prohíbe `--states`/`--hover` en el gate: el fixture es estático.

## 4. Fixtures y máscaras

### 4.1 Fixtures

| Fixture | Qué cubre | Gate |
|---|---|---|
| `contract` (`benchmarks/frontend-parity/fixtures/contract`) | block flow, padding, margin, colores | Sí (rect-only) |
| `flex-page` (`.../flex-page`) | flex anidado, gap, space-between, align | Sí (rect-only) |
| `media-library` (`benchmarks/pilot-media-library/page`) | grid, sidebar, detalle, imágenes, texto | No (informativo primero) |
| `layout-100` (`benchmarks/engine-metrics/fixtures/layout-100`) | escalado de grid y densidad | Sí (solo geometría) |
| `overlays` (nuevo, cuando E8) | position/z-index/sombra/radio | Futuro |

Los fixtures son **estáticos y autocontenidos**: CSS local, imágenes locales de
tamaño entero, sin fuentes web, sin red. Un fixture que necesita red no entra.

### 4.2 Máscaras por región

Cada fixture trae `mask.json`:

```json
{
  "viewport": { "width": 1280, "height": 800 },
  "regions": [
    { "name": "sidebar", "rect": [0, 0, 280, 800], "mode": "strict" },
    { "name": "grid",    "rect": [280, 96, 1000, 704], "mode": "strict" },
    { "name": "text",    "rect": [0, 0, 1280, 800], "mode": "text" },
    { "name": "detail",  "rect": [900, 0, 380, 800], "mode": "strict" }
  ],
  "exclude": [
    { "name": "caret", "rect": [100, 200, 2, 24] },
    { "name": "shadow-band", "rect": [0, 88, 1280, 12], "reason": "blur aproximado" }
  ]
}
```

Semántica:

- **strict**: SSIM y diff completos; un píxel cuenta.
- **text**: se evalúa geometría (cajas, baselines, cortes de línea) y SSIM con
  banda de tolerancia por antialiasing; el diff de color puro no es gate.
- **exclude**: no cuenta (caret, scrollbar, bandas de sombra aproximada,
  decoración conocida). Todo `exclude` debe llevar `reason`; una máscara sin
  razón se rechaza en revisión.
- Las regiones pueden solaparse; el reporte muestra **global** y **por región**.
  Un buen global con una región mala es un FAIL, no un aprobado.

Regiones mínimas del piloto: **grid**, **sidebar**, **detail** y **text**. El
resto del viewport es `background`.

## 5. Métricas y umbrales

| Métrica | Cómo | Uso |
|---|---|---|
| **SSIM** (gris, ventana 8×8) | `compare.py` | Métrica principal de estructura |
| **Pixel diff** | % de píxeles con `max(abs(ΔR), abs(ΔG), abs(ΔB)) > 8` | Detecta manchas y desplazamientos |
| **MAE** | media de diferencia absoluta por canal | Sensible a color/AA global |
| **Bounding boxes** | cajas del IR vs detección de regiones | Diagnóstico de layout, no gate visual |
| **Histograma** | diferencia por región | Color plano mal compuesto |

Umbrales iniciales (se calibran con la primera baseline, no se inventan como
gate definitivo):

| Región | SSIM | Pixel diff |
|---|---:|---:|
| `strict` rect-only (`contract`, `flex-page`) | ≥ 0,995 | ≤ 0,2 % |
| `strict` con imágenes/grid | ≥ 0,980 | ≤ 0,5 % |
| `text` | ≥ 0,950 + geometría | ≤ 2 % (solo AA) |
| Global | ≥ 0,980 | ≤ 1,0 % |

Un FAIL se reporta con: región, SSIM, diff %, recorte lado a lado y overlay de
diferencia. Sin ese paquete no se abre un bug.

## 6. Caveats que hay que escribir en cada reporte

1. **Antialiasing de texto.** GDI/DirectWrite y Skia rasterizan distinto aunque
   usen la misma fuente. Se fuerza gris en ambos; aun así, el hinting puede
   mover un píxel. Por eso `text` tiene umbral propio y se compara geometría.
2. **DPI.** El gate corre a **96 DPI / DPR 1**. Un monitor al 125 % escala la
   ventana y cambia el rasterizado; eso es otra matriz (100/125/150/200 %), no
   este benchmark. `--force-device-scale-factor=1` y una escala lógica de 1.0
   son obligatorios; el reporte lo declara.
3. **Fuentes.** Allowlist por fixture (`Segoe UI`, `Segoe UI Semibold`,
   `Consolas`), sin fallback silencioso: si la fuente no existe, el harness
   falla en vez de comparar otra cosa. Nada de web fonts.
4. **Color.** sRGB, 8 bits, sin HDR. Un perfil de color distinto invalida la
   comparación; se registra el perfil usado.
5. **Imágenes.** WIC y Skia remuestrean distinto. Los fixtures usan imágenes a
   tamaño entero (sin escalar) o `object-fit: fill` con dimensiones exactas. Si
   hay escalado, la región se marca y se reporta aparte.
6. **Sombras y blur.** El core aproxima el blur (`support.md`); la banda de
   sombra se excluye o usa umbral laxo, con razón explícita.
7. **Coordenadas fraccionarias.** El layout admite 0,01 px; el rasterizador
   redondea. Los fixtures del gate usan enteros o medios píxeles estables.
8. **Scroll y overlays.** Sin scroll visible; scrollbars ocultas en Chromium y
   inexistentes en nativo. `position: fixed/sticky` (cuando existan) requieren
   fixtures propios.
9. **Composición del OS.** Nada de capturas de ventana, DWM, sombras de
   ventana, transparencias ni temas del sistema.
10. **Backend.** WARP y hardware no son comparables entre sí; cada reporte
    declara backend, adaptador y si es software.

## 7. Estructura propuesta

```
benchmarks/visual-parity/
  README.md
  chromium.json                 # versión, URL y SHA-256
  fixtures/<name>/
    index.html
    style.css
    mask.json
    fixture.json                # viewport, fuentes, tolerancias, gate
  tools/
    capture_chromium.ps1
    capture_native.ps1
    compare.py                  # SSIM, diff, máscaras, report.json
    report.py                   # report.md + diff/*.png
  results/
    <YYYY-MM-DD>-<commit>/
      report.json
      report.md
      diff/<name>.png
      capture/<name>.chromium.png
      capture/<name>.native.png
  baseline.json                 # última corrida aceptada
```

- Los PNG de referencia y los `diff/` **no se commitean**: se regeneran con la
  versión fijada y se cachean por hash de `chromium.json` + fixture. El repo
  guarda `report.json`/`report.md` y, si acaso, recortes pequeños para revisión;
  las capturas completas van como artifacts de CI.
- `results/` sigue el mismo criterio que `benchmarks/frontend-parity/results/`:
  fecha, commit, backend, versión de Chromium y subset en el reporte.
- Dependencias del harness: Python 3.12 + Pillow; `numpy`/`scikit-image` solo si
  se añaden como dev-deps del benchmark, nunca al runtime.

## 8. CI (futuro)

No entra en el CI actual (`.github/workflows/frontend-parity.yml`) todavía. El
diseño para cuando madure:

- Workflow **opcional** `visual-parity.yml`, `windows-latest`, disparado por
  cambios en `src/runtime/**`, `src/frontend/rust/**`,
  `benchmarks/visual-parity/**` y `docs/visual-parity.md`.
- Descarga de `chrome-headless-shell` con SHA verificado y caché por versión.
- `tools/fetch-wgpu.ps1` + build R2 → capturas → `compare.py --gate`.
- Se publican `report.md` y `diff/` como artifact; el job falla solo en
  fixtures con `"gate": true`.
- Sin GPU: todo software (Chromium `--disable-gpu`, nativo WARP/D3D11 WARP).
  El reporte lo declara; un cambio de backend puede mover los números.
- Hasta tener baseline estable, el job es **informativo** (`continue-on-error`).
  Convertirlo en gate requiere dos corridas verdes consecutivas en `main`.

## 9. Sin dependencia en runtime

- Chromium, Python, Pillow y el harness viven **solo** en `benchmarks/`.
- El engine no enlaza, carga ni invoca nada del benchmark; no cambia la ABI ni
  el install size.
- El `.exe` sigue sin dependencia de navegador, Python o red. Esta es la misma
  regla de `docs/architecture.md`: el core no conoce el benchmark.
- El benchmark puede fallar en una máquina sin Chromium; eso no bloquea build
  ni producto.

## 10. Triage de una diferencia

Orden barato → caro:

1. **Layout**: ¿la caja del IR coincide? Comparar bounding boxes antes de mirar
   píxeles. Un offset de 1 px explica casi todo.
2. **Clip/scroll**: ¿el contenido se sale o se recorta distinto?
3. **Color/fill**: ¿un color plano distinto? Revisar cascada, alpha y
   premultiplicado.
4. **Bordes/radius/sombra**: diferencias locales en el borde, no en el relleno.
5. **Texto**: forma, baseline, corte de línea; después AA.
6. **AA/rasterizado**: solo si 1–5 están limpios y la diferencia es de 1 píxel
   en bordes.

El reporte incluye el overlay y las métricas por región para no adivinar.

## 11. Relación con otros documentos

- `docs/metrics.md` — presupuesto de tamaño/CPU; este benchmark no lo duplica.
- `benchmarks/frontend-parity/SPEC.md` — paridad semántica del IR, que sigue
  siendo el primer gate. Visual sin IR correcto no significa nada.
- `docs/support.md` — limitaciones que explican diferencias esperadas
  (sombras, `line-height`, `text-align`, `opacity`).
- `docs/e8-css-extensions.md` — transform/position/transition obligan a
  `--no-animations` y a máscaras nuevas.
- `docs/release-checklist.md` — capturas y demo que se publican en release.
