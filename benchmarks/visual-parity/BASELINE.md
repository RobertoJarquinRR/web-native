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
| `flex-page` | 480×320 | 1.000000 | 0.000 | 0.000 | sí | **PASS** |
| `contract` | 400×300 | 1.000000 | 0.000 | 0.000 | sí | **PASS** |
| `media-library` | 1280×800 | 0.957851 | 2.139 | 1.482 | no | INFO |

Regiones:

| Fixture | Región | SSIM | Diff % |
|---|---|---:|---:|
| `flex-page` | topbar / cards / footer | 1.000000 | 0.000 |
| `media-library` | grid | 0.967271 | 2.262 |
| `media-library` | sidebar y detail | ≥ 0.990 | 0.000 |

En `media-library` lo que queda es texto e imágenes (AA de DirectWrite vs
Skia y remuestreo); la caja, la rejilla y las columnas ya coinciden.

## Hallazgos (histórico y estado actual)

La primera corrida (SSIM 0.898 en `flex-page`, 0.564 en `media-library`) señaló
dos causas, y ambas están corregidas:

1. **Modelo de caja.** El motor asumía *border-box*; CSS usa *content-box* por
   defecto. Ahora `box-sizing` se parsea, viaja en `ui.json` y el motor aplica
   content-box salvo que la página declare `border-box`.
2. **Márgenes de bloque.** El flujo avanzaba por la altura del hijo sin sumar
   su `margin-top`, así que el siguiente hermano quedaba un margen más arriba
   (`.footer` en y=208 en vez de 232). Corregido en `layout_node`.

Los fixtures y la app declaran `box-sizing: border-box` donde fueron diseñados
así, de modo que su layout no cambia.

`box-sizing` acepta `border-box` y `content-box`; el motor aplica content-box
por defecto (CSS) y border-box cuando se declara.

**Verificación:** el motor ya implementa ambos modelos. `content-box` es el
default de CSS; las páginas que quieren el comportamiento histórico declaran
`* { box-sizing: border-box }`. Con esa declaración la paridad es exacta
(`flex-page` 0.000 %).

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
