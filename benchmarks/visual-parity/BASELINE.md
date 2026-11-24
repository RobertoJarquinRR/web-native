# Baseline de paridad visual (Chromium vs nativo)

Medición real de la primera ejecución del harness. No es un gate todavía:
`flex-page` y `contract` son gateables; `media-library` es informativo.

Comando canónico:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File benchmarks\visual-parity\run_visual.ps1
```

## Resultado

| Fixture | Viewport | SSIM | Diff % | MAE | Gate | Estado |
|---|---:|---:|---:|---:|---|---|
| `flex-page` | 480×320 | 0.898467 | 27.759 | 4.132 | sí | **FAIL** |
| `contract` | 400×300 | 1.000000 | 0.000 | 0.000 | sí | **PASS** |
| `media-library` | 1280×800 | 0.563605 | 58.752 | 31.058 | no | INFO |

Regiones:

| Fixture | Región | SSIM | Diff % |
|---|---|---:|---:|
| `flex-page` | topbar | 0.882613 | 15.789 |
| `flex-page` | cards | 0.867582 | 23.634 |
| `flex-page` | footer | 0.929352 | 45.152 |
| `media-library` | sidebar | 0.820332 | 48.547 |
| `media-library` | grid | 0.470399 | 64.799 |
| `media-library` | detail | 0.758885 | 76.837 |

En `media-library` la tinta coincide con IoU 0.8284: el texto está donde debe,
lo que falla es la caja que lo contiene.

## Hallazgos (no son del harness)

`contract` da 0 % de diferencia, así que el pipeline de captura/comparación es
exacto para rects. Las diferencias de los otros dos fixtures son de layout:

1. **Modelo de caja.** El motor trata siempre `width`/`height` como
   *border-box*; CSS usa *content-box* por defecto. Con
   `.card { width: 128px; padding: 12px }`, Chromium mide 152 px y el motor
   128 px. Con `.footer { height: 44px; padding: 16px }`, Chromium mide 76 px y
   el motor 44 px.
2. **Columnas de la rejilla.** En `media-library` Chromium coloca 4 columnas y
   el motor 5, y la barra lateral cambia de ancho. Es la misma causa: la barra
   lateral declara `width` y además `padding`, y el motor no suma el padding.

`box-sizing: border-box` es lo único que acepta el subset hoy. El motor no
parsea `box-sizing` para cambiar el cálculo: siempre asume border-box.

**Verificación:** si al fixture se le añade `* { box-sizing: border-box; }`, la
captura de Chromium y la nativa quedan con **SSIM 1.000000 y 0.000 % de
diferencia**. Es decir, no falta un modelo de caja distinto: falta declararlo.
Por eso el contrato es border-box y la página debe declararlo explícitamente.
(El experimento se hizo sobre una copia del fixture, no sobre el original.)

## Entorno de la medición

- Chromium `141.0.7390.37` vía Playwright `1.56.0`, headless shell build 1194,
  DPR 1, 96 DPI, sRGB 8 bits.
- Nativo: `runtime_r2.exe --render` a través de la vtable, **D3D11 hardware**
  (`Intel(R) HD Graphics`) en esta máquina; wgpu/WARP con `WN_RENDERER=wgpu`.
- Commit del repo al medir: `4f07d1a`.

## Cómo se regeneran

`run_visual.ps1` escribe `out/report.md`, `out/report.json` y los reportes por
fixture; `out/` y `reports/` están ignorados por git. Las capturas y los diff
PNG no se commitean. Este archivo resume los números de la última corrida
documentada.
