# Engine metrics — baseline

Generado: **2026-11-23T23:58:29+00:00** · commit **522499e** (árbol de trabajo con cambios concurrentes de otro stream fuera de benchmarks/engine-metrics; el snapshot congelado corresponde al commit 522499e) · 10 runs por métrica + 1 warm-up.

Host: Windows-10-10.0.19045-SP0 · Intel64 Family 6 Model 42 Stepping 7, GenuineIntel · 8 cores · Python 3.14.2 · psutil 7.2.2

Backend: **D3D12** · type=cpu · vendor=0x1414 device=0x008C · Microsoft Basic Render Driver · **WARP (software rasterizer, no hardware GPU)**

> Los números de GPU/offscreen y parte del RSS están dominados por WARP (`Microsoft Basic Render Driver`). No son representativos de una GPU real; sirven como cota superior de CPU/software.

## Snapshot medido

- `snapshot/runtime_r2.exe` — 299,008 B / 0.30 MB — SHA-256 `4ba2e269232779dce7e8d1455c145612b3a6edf60e9cc03cd98a752ae42ec38f`
- `snapshot/wgpu_native.dll` — 9,117,696 B / 9.12 MB — SHA-256 `5e26249c40dcdac43f5adca3195fcf40e4ec6c67472b38510165f411fcc198e7`
- `snapshot/assets/atlas.bin` — 65,536 B / 0.07 MB (horneado en el exe, no se carga en runtime)
- Procedencia: `src/runtime/c-r2/build` en commit `522499e`; hashes verificados: sí
- Recompilación de control: `runtime_r2.c` del commit `522499e` + árbol de nodos embebido → 299,008 B (mismo tamaño que el snapshot); PNG 960×640 idéntico (`5ed6b6c02f690cac71346cc051674e12b3fb3b47f3685ed6862708356df26054`).
- Determinismo del snapshot: dos `--render 960 640` → mismo SHA-256 (`5ed6b6c02f690cac71346cc051674e12b3fb3b47f3685ed6862708356df26054`).
- R1: build de `benchmarks\frontend-parity\fixtures\flex-page\index.html` a 480x320 con `src/build.ps1` (binario desechable en `work/r1-build`)

## Tabla comparativa vs presupuesto fase 1

| Métrica | R2 | R1 | Presupuesto | Estado |
|---|---:|---:|---:|---|
| Install size (exe + DLL + assets) | 9.42 MB | 9.28 MB | ≤ 15 MB | PASS |
| Engine size (.exe) | 0.30 MB | 0.16 MB | ≤ 1 MB | PASS |
| Startup (init → primer frame) | 99.47 ms | 94.09 ms | ≤ 150 ms | PASS |
| RSS pico (ventana) | 53.47 MB | 49.94 MB | ≤ 30 MB | FAIL |
| CPU frame (R2 print) | 0.514 ms | n/a | ≤ 2 ms | PASS |
| CPU frame + layout inicial | 0.527 ms | n/a | ≤ 2 ms | PASS |
| Layout 28 nodos (960/720/560) | 0.0011 / 0.0010 / 0.0010 ms | n/a | ≤ 0.5 ms | PASS |
| Layout ~100 nodos (960) | 0.0043 ms | n/a | ≤ 0.5 ms | PASS |
| Offscreen 960×640 (wall proceso) | 268.6 ms | 251.6 ms | informativo | — |
| Offscreen 960×640 (interno)† | 57.6 ms | 130.7 ms | informativo | — |
| Frame ventana (vsync) | 15.78 ms | 15.54 ms | — | — |

† R2 `frame_ms` = build+upload+pass+readback+PNG (tras el init); R1 «total interno» = init+upload+pipeline+render+map+PNG. No son directamente comparables entre sí; el wall del proceso sí. El frame de ventana está limitado por vsync (~16 ms) y no es CPU puro.

Los valores de la tabla son **medianas** de 10 ejecuciones. `n/a` significa no medible de forma fiable con el binario actual (se explica abajo).

## Muestras por métrica (mediana / mínimo / máximo)

| Escenario | Métrica | Mediana | Mín | Máx |
|---|---|---:|---:|---:|
| R2 ventana 30 frames | startup init | 99.465 ms | 94.500 ms | 107.940 ms |
| R2 ventana 30 frames | CPU frame | 0.514 ms | 0.480 ms | 0.566 ms |
| R2 ventana 30 frames | frame pared (vsync) | 15.780 ms | 15.570 ms | 15.950 ms |
| R2 ventana 30 frames | layout del WM_SIZE | 0.013 ms | 0.011 ms | 0.028 ms |
| R2 ventana 30 frames | wall del proceso | 733.207 ms | 720.450 ms | 746.367 ms |
| R2 ventana 30 frames | RSS pico (working set) | 53.47 MB | 53.38 MB | 53.79 MB |
| R2 ventana 30 frames | commit pico (pagefile, informativo) | 294.82 MB | 294.72 MB | 295.20 MB |
| R2 --frames 1 | startup init | 99.820 ms | 96.790 ms | 102.900 ms |
| R2 --frames 1 | wall proceso (cota superior) | 268.427 ms | 263.951 ms | 271.603 ms |
| R2 layout 960x640 (28 nodos) | layout avg | 0.0011 ms | 0.0011 ms | 0.0015 ms |
| R2 layout 960x640 (28 nodos) | layout best | 0.0009 ms | 0.0009 ms | 0.0010 ms |
| R2 layout 720x640 (28 nodos) | layout avg | 0.0010 ms | 0.0010 ms | 0.0015 ms |
| R2 layout 720x640 (28 nodos) | layout best | 0.0009 ms | 0.0009 ms | 0.0010 ms |
| R2 layout 560x640 (28 nodos) | layout avg | 0.0010 ms | 0.0010 ms | 0.0012 ms |
| R2 layout 560x640 (28 nodos) | layout best | 0.0009 ms | 0.0009 ms | 0.0010 ms |
| R2 layout 560x640 (100 nodos) | layout avg | 0.0047 ms | 0.0046 ms | 0.0052 ms |
| R2 layout 720x640 (100 nodos) | layout avg | 0.0045 ms | 0.0044 ms | 0.0046 ms |
| R2 layout 960x640 (100 nodos) | layout avg | 0.0043 ms | 0.0042 ms | 0.0047 ms |
| R2 offscreen 960x640 | wall proceso | 268.562 ms | 264.777 ms | 286.161 ms |
| R2 offscreen 960x640 | layout | 0.015 ms | 0.014 ms | 0.021 ms |
| R2 offscreen 960x640 | frame interno | 57.626 ms | 53.698 ms | 61.750 ms |
| R2 offscreen 960x640 | gpu encode+submit | 2.214 ms | 2.064 ms | 2.426 ms |
| R2 offscreen 960x640 | readback/map | 34.400 ms | 30.879 ms | 38.891 ms |
| R2 offscreen 960x640 | PNG | 20.922 ms | 20.349 ms | 22.185 ms |
| R2 offscreen 960x640 | RSS pico | 51.21 MB | 51.11 MB | 51.72 MB |
| R2 offscreen 720x640 | wall proceso | 278.116 ms | 266.630 ms | 333.165 ms |
| R2 offscreen 720x640 | layout | 0.015 ms | 0.013 ms | 0.040 ms |
| R2 offscreen 720x640 | frame interno | 56.651 ms | 47.414 ms | 67.673 ms |
| R2 offscreen 720x640 | gpu encode+submit | 2.216 ms | 2.111 ms | 2.824 ms |
| R2 offscreen 720x640 | readback/map | 36.861 ms | 28.792 ms | 46.223 ms |
| R2 offscreen 720x640 | PNG | 17.338 ms | 16.510 ms | 18.696 ms |
| R2 offscreen 720x640 | RSS pico | 49.59 MB | 49.45 MB | 50.01 MB |
| R2 offscreen 560x640 | wall proceso | 268.465 ms | 265.463 ms | 271.588 ms |
| R2 offscreen 560x640 | layout | 0.014 ms | 0.013 ms | 0.019 ms |
| R2 offscreen 560x640 | frame interno | 50.919 ms | 43.988 ms | 54.556 ms |
| R2 offscreen 560x640 | gpu encode+submit | 2.189 ms | 2.134 ms | 2.966 ms |
| R2 offscreen 560x640 | readback/map | 34.854 ms | 27.965 ms | 37.972 ms |
| R2 offscreen 560x640 | PNG | 13.755 ms | 13.593 ms | 15.005 ms |
| R2 offscreen 560x640 | RSS pico | 48.16 MB | 48.11 MB | 48.39 MB |
| R1 ventana 30 frames | startup init | 94.095 ms | 89.960 ms | 140.920 ms |
| R1 ventana 30 frames | frame pared (vsync) | 15.540 ms | 15.350 ms | 15.970 ms |
| R1 ventana 30 frames | wall del proceso | 680.668 ms | 662.229 ms | 759.054 ms |
| R1 ventana 30 frames | RSS pico (working set) | 49.94 MB | 49.82 MB | 51.33 MB |
| R1 ventana 30 frames | commit pico (pagefile, informativo) | 292.56 MB | 292.30 MB | 292.80 MB |
| R1 --frames 1 | startup init | 90.765 ms | 87.850 ms | 92.610 ms |
| R1 --frames 1 | wall proceso (cota superior) | 217.727 ms | 203.815 ms | 223.623 ms |
| R1 offscreen | wall proceso | 251.611 ms | 249.543 ms | 255.071 ms |
| R1 offscreen | init+adapter+device+upload | 84.785 ms | 83.320 ms | 87.990 ms |
| R1 offscreen | pipeline | 5.115 ms | 4.800 ms | 5.910 ms |
| R1 offscreen | render+copy+submit | 1.690 ms | 1.570 ms | 2.130 ms |
| R1 offscreen | map | 30.385 ms | 23.680 ms | 33.700 ms |
| R1 offscreen | PNG | 9.480 ms | 8.370 ms | 10.350 ms |
| R1 offscreen | total interno | 130.675 ms | 127.700 ms | 134.510 ms |
| R1 offscreen | RSS pico | 43.53 MB | 43.49 MB | 43.68 MB |

La fila «commit pico (pagefile)» es la carga de commit máxima, no memoria residente: con WARP/D3D12 incluye reservas grandes del rasterizador software (el working set es la métrica de RSS del presupuesto).

## Comparación con el presupuesto

- **Install size R2**: 9.417 contra 15 → **PASS** (margen 5.583)
- **Engine size R2**: 0.299 contra 1 → **PASS** (margen 0.701)
- **Startup R2**: 99.465 contra 150 → **PASS** (margen 50.535)
- **RSS R2**: 53.467 contra 30 → **FAIL** (exceso 23.467)
- **CPU frame R2**: 0.514 contra 2 → **PASS** (margen 1.486)
- **Layout 28 nodos R2**: 0.001 contra 0.5 → **PASS** (margen 0.499)
- **Layout 100 nodos R2**: 0.004 contra 0.5 → **PASS** (margen 0.496)

> Con WARP, install/engine/startup/CPU-frame son válidos como medida de software, pero RSS y offscreen incluyen el rasterizador software de D3D12 y **no** deben compararse con una GPU real.

Layout R2: `layout_report.json` reporta **28 nodos** y 100 iteraciones por viewport; se mide el `layout_ms_avg` de cada ejecución y se agrega la mediana de las ejecuciones.

Layout ~100 nodos: fixture `benchmarks/engine-metrics/fixtures/layout-100/index.html` con **100 nodos**, mismo `runtime_r2.c` recompilado contra su árbol de nodos. Solo mide layout; no entra en install/engine size.

## Observaciones (derivadas de esta medición)

- **RSS**: R2 53.47 MB vs R1 49.94 MB (Δ +3.53 MB). Ambos superan el presupuesto de 30 MB; con R1 (un `.exe` de 0.16 MB, sin layout en runtime) ya en 49.94 MB, el grueso del RSS es el stack wgpu/D3D12/WARP y no el motor de R2. Hay que repetir la medida en GPU hardware antes de tratar el presupuesto de 30 MB como fallo del engine.
- **Startup**: R2 99.47 ms vs R1 94.09 ms (Δ +5.37 ms). El coste dominante en ambos es crear instancia/adaptador/dispositivo/pipelines de wgpu, no el layout.
- **Arranque en frío vs caliente**: el `build_report.json` del snapshot (primera ejecución tras compilar) registró init 128.46 ms; la mediana de esta tanda es 99.47 ms con un warm-up descartado. El presupuesto se compara con la mediana en caliente (estable); para un SLA de primer arranque hay que medir cold start de forma explícita.
- **Install**: la DLL de wgpu es 9.12 MB, el 96.8 % del total; el `.exe` de R2 es 0.299 MB. El atlas (64 KiB) ya está dentro del `.exe`.
- **CPU frame**: 0.514 ms de media (sin layout ni vsync), muy por debajo de 2 ms; el frame de ventana de 15.78 ms está limitado por present/vsync.
- **Offscreen**: 268.6 ms de wall, de los cuales ~57.6 ms son el frame interno (readback + PNG dominan); el resto es arranque de wgpu por proceso.

## Incidencias de la ejecución

Ninguna: todas las ejecuciones terminaron con exit 0.

## Método (resumen)

- **install_size**: bytes(exe) + bytes(wgpu_native.dll) + assets externos de runtime; el atlas va compilado dentro del ejecutable, así que los assets externos son 0 y el atlas horneado se reporta aparte (sin contarlo dos veces)
- **engine_size**: solo bytes del .exe del runtime
- **startup**: init reportado por el runtime en la ejecución de ventana (arranque del loop -> instancia/adaptador/dispositivo/pipelines listos; el primer frame viene inmediatamente después). El wall externo de `--frames 1` se reporta como cota superior porque incluye spawn del proceso y teardown.
- **rss**: PeakWorkingSetSize vía Win32 GetProcessMemoryInfo muestreado cada 2 ms mientras corre el proceso (cruce con psutil `memory_info().peak_wset`); se reporta el mayor de los dos. También se guarda el pico de pagefile (commit) como dato informativo.
- **cpu_frame**: R2 imprime `window: cpu frame X ms average` (upload+encode+submit, sin layout ni present/vsync). R1 no instrumenta CPU-only: `n/a`; se reporta su `ms/frame average` limitado por vsync como referencia.
- **layout**: `--report` de R2 ejecuta el layout 100 veces por viewport e imprime best/avg; se reporta la mediana entre ejecuciones del `layout_ms_avg`. El escenario de ~100 nodos usa un fixture propio y un binario R2 variante (mismo runtime_r2.c, árbol de 100 nodos).
- **offscreen**: wall de `--render W H out.png` (proceso completo) más los tiempos internos que imprime el runtime (layout/frame/gpu/readback/png). En R1 el total interno incluye init+pipeline+render+map+png.
- **backend**: línea de adaptador del runtime; WARP es type=cpu + vendorID=0x1414

El JSON crudo con todas las muestras es `baseline.json` (`webnative.engine-metrics/1`).

