# Engine metrics — harness de métricas de webnative

Medición reproducible de tamaño, arranque, memoria y frame del runtime C:

- **Runtime** (`src/runtime/c-r2`): layout en runtime, Win32, D3D11/wgpu.

R1 (`src/runtime/c-static`) está archivado; si `src/build.ps1` no existe, el
harness avisa y omite las filas de R1.

El resultado es `baseline.json` (todas las muestras crudas) + `BASELINE.md` (tabla
legible y metodología). **Ningún número está inventado**: cada valor sale de una
ejecución real registrada en el JSON.

Baseline actual: **commit `522499e`**, 2026-11-23, Windows 10 x64, backend
**D3D12 sobre WARP** (`Microsoft Basic Render Driver`), 10 runs por métrica.

---

## 1. Un comando

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File benchmarks\engine-metrics\run.ps1
```

El script:

1. crea `benchmarks/engine-metrics/.venv` e instala `psutil` (solo si falta);
2. **congela** el build actual de R2 (`runtime_r2.exe` + `wgpu_native.dll` +
   atlas) en `snapshot/` y escribe `snapshot.json` con commit y SHA-256. Si el
   build no existe, lo compila una vez con `src/runtime/c-r2/build.ps1`;
3. (opcional) compila R1 con `src/build.ps1` en `work/r1-build` si el script
   todavía existe; si no, omite las filas de R1 y sigue;
4. compila una variante de R2 con **100 nodos** en `work/layout100` (mismo
   `runtime_r2.c`, `ui_data.h` distinto) para medir el layout del presupuesto;
5. mide 10 runs por métrica y escribe `baseline.json` + `BASELINE.md`.

Opciones:

| Flag | Efecto |
|---|---|
| `-Runs N` | runs medidos por métrica (por defecto 10; siempre 1 warm-up extra) |
| `-ForceSnapshot` | vuelve a copiar el build actual de R2 al snapshot |
| `-RebuildR1` | recompila R1 con `src/build.ps1` (solo si el script existe; archivado) |
| `-SkipR1` | no mide R1 |
| `-SkipLayout100` | no compila/mide la variante de 100 nodos |
| `-NoWarmup` | sin ejecución de calentamiento |

Ejecución directa (si el snapshot y `work/r1-build` ya existen):

```powershell
benchmarks\engine-metrics\.venv\Scripts\python.exe benchmarks\engine-metrics\measure.py --runs 10
```

Requisitos: Windows 10/11 x64, Python 3 (probado 3.14), MSVC con
`vcvars64.bat`, `third_party/wgpu-native`, y el frontend Rust ya compilado
(`src/frontend/rust/target/release/frontend-rust.exe`) para la variante de 100
nodos. Si falta algo, el harness lo reporta y sigue con lo que pueda medir.

**No modifica** runtime, frontend, emisores, docs ni `src/build.ps1`. Solo
escribe bajo `benchmarks/engine-metrics/`. La única excepción es que, si no hay
snapshot ni build de R2, ejecuta `src/runtime/c-r2/build.ps1`, que genera
artefactos bajo `src/runtime/c-r2/build/` (nunca fuentes).

---

## 2. Estructura

```
benchmarks/engine-metrics/
  run.ps1                 entrada única (venv + snapshot + builds + measure)
  measure.py              medición y generación de baseline.json/BASELINE.md
  baseline.json           todas las muestras, commit, host, backend, método
  BASELINE.md             tabla legible + comparación con el presupuesto
  snapshot/               R2 congelado (exe + DLL + atlas + snapshot.json)
  fixtures/layout-100/    página de 100 nodos para el layout sintético
  work/                   builds desechables, PNGs y reports (gitignored)
  .venv/                  psutil (gitignored)
```

`snapshot/` y `work/` no se versionan: los binarios se reproducen desde el
commit. `snapshot.json` sí se conserva como evidencia (commit, tamaños, SHA-256
y verificación de recompilación).

---

## 3. Qué mide cada métrica

| Métrica | Definición | Cómo se obtiene |
|---|---|---|
| **Install size** | `.exe` + `wgpu_native.dll` + assets externos (atlas, fuentes) | suma de bytes de los ficheros. En R1 y R2 el atlas va **compilado dentro del `.exe`**, así que los assets externos son 0 y el atlas horneado se reporta aparte (sin contarlo dos veces). |
| **Engine size** | binario del runtime sin la DLL | bytes del `.exe` de ventana. |
| **Startup** | proceso → primer frame presentado | `init` que imprime el runtime: arranque del loop → instancia/adaptador/dispositivo/pipelines listos (el primer frame se presenta inmediatamente después). Se añade el **wall externo de `--frames 1`** como cota superior, que incluye spawn del proceso y teardown. |
| **RSS** | memoria residente pico | `PeakWorkingSetSize` con `GetProcessMemoryInfo` (Win32), muestreado cada 2 ms; se cruza con `psutil.Process.memory_info().peak_wset`. Se reporta el mayor. También se guarda el pico de *pagefile/commit* como dato informativo (no es RSS). |
| **CPU frame** | coste de CPU por frame sin vsync | R2 imprime `window: cpu frame X ms average` (upload + encode + submit; sin layout ni present/vsync). R1 **no instrumenta** CPU puro: se marca `n/a` y se reporta su `ms/frame average` limitado por vsync como referencia. |
| **Layout** | recalcular el árbol | `runtime_r2.exe --report` ejecuta el layout 100 veces por viewport (960/720/560) e imprime `layout_ms_best/avg`; se agrega la mediana entre procesos. El fixture canónico tiene **28 nodos**. |
| **Layout ~100 nodos** | escalado del layout | variante de R2 compilada contra un `ui_data.h` de **100 nodos** (`fixtures/layout-100`). Solo mide layout; no entra en install/engine size. |
| **Offscreen render** | frame + readback + PNG | wall del proceso `--render W H out.png`, más los tiempos internos del runtime (`layout`, `frame`, `gpu encode+submit`, `readback/map`, `png`). Solo se renderiza en R2; R1 usa `render_png.exe`. |
| **Backend** | D3D12/WARP/hardware | línea `adapter:` del runtime. WARP = `type=cpu` + `vendorID=0x1414`. |
| **Frame de ventana** | pared con vsync | `ms/frame average` del runtime; está limitado por present (~16 ms), no es CPU puro. |

Detalles de método completos en `baseline.json → method` y en `BASELINE.md`.

### Qué queda `n/a` y por qué

- **CPU frame de R1**: el binario no imprime tiempo de CPU aislado y no se toca
  el runtime para añadirlo. Se reporta su frame de pared (vsync) como contexto.
- **Layout de R1**: R1 no tiene layout en runtime; el display list está
  horneado. No hay nada equivalente a `--report`.
- **Input latency**: no instrumentada todavía (docs/metrics.md la marca como
  instrumentación futura). No se inventa.

---

## 4. Aviso WARP (leer antes de comparar)

Esta máquina no tiene GPU hardware: el adaptador es
`Microsoft Basic Render Driver` (D3D12 sobre **WARP**, rasterizador software).
El harness lo detecta y lo marca en `BASELINE.md` y en `baseline.json`
(`is_warp: true`).

Consecuencias:

- **Install / engine / layout / CPU frame**: válidos como medida del motor.
- **Startup**: válido, pero crear el dispositivo WARP cuesta más que en una GPU
  discreta; el número es una cota superior.
- **RSS**: incluye el stack D3D12/WARP y las reservas del driver software.
  **No** debe tomarse como el RSS de producción en hardware. De hecho, R1 (un
  `.exe` de 0.16 MB sin layout en runtime) ya consume ~50 MB, lo que apunta a
  que el grueso es el backend, no el motor de R2.
- **Offscreen**: dominado por readback/PNG y por WARP; los 50–60 ms internos no
  son representativos de una GPU real.

Cualquier comparación con el presupuesto debe repetirse en hardware antes de
convertir un FAIL de RSS/offscreen en una decisión de diseño.

---

## 5. Baseline vs presupuesto (`docs/metrics.md`)

Mediana de 10 runs (detalle y min/max en `BASELINE.md`):

| Métrica | R2 | R1 | Presupuesto | Estado |
|---|---:|---:|---:|---|
| Install size | 9.42 MB | 9.28 MB | ≤ 15 MB | PASS |
| Engine size | 0.30 MB | 0.16 MB | ≤ 1 MB | PASS |
| Startup (init) | 99.47 ms | 94.09 ms | ≤ 150 ms | PASS |
| RSS pico | 53.47 MB | 49.94 MB | ≤ 30 MB | **FAIL (WARP)** |
| CPU frame | 0.514 ms | n/a | ≤ 2 ms | PASS |
| Layout 28 nodos (960/720/560) | 0.0011 / 0.0010 / 0.0010 ms | n/a | ≤ 0.5 ms | PASS |
| Layout 100 nodos (960) | 0.0043 ms | n/a | ≤ 0.5 ms | PASS |
| Offscreen 960×640 (wall) | 268.6 ms | 251.6 ms | informativo | — |

Lectura:

- El install está dominado por la DLL de wgpu (9.12 MB, 96.8 %); el `.exe` de R2
  es 0.299 MB (incluye el atlas de 64 KiB).
- Startup y CPU frame tienen margen amplio; el layout es microsegundos incluso
  con 100 nodos.
- El único FAIL es RSS, y R1 (mucho menos código) falla igual: es el footprint
  de wgpu/D3D12/WARP. El presupuesto de 30 MB exige medir en hardware y, si
  sigue alto, evaluar backend D2D/D3D11 o inicialización perezosa.

---

## 6. Snapshot y reproducibilidad

El workstream de runtime puede cambiar `src/runtime/c-r2/**` mientras se mide.
Para que el baseline sea reproducible:

1. `run.ps1` copia el build actual a `snapshot/` **antes** de medir y escribe
   `snapshot.json` con commit, tamaños y SHA-256.
2. `measure.py` solo ejecuta `snapshot/runtime_r2.exe`, nunca el build vivo.
3. Se verifica la procedencia: recompilar `runtime_r2.c` del commit con el
   `ui_data.h` del build produce el mismo tamaño de `.exe` y un PNG 960×640
   **byte-idéntico** (`5ed6b6c0…`). El hash del `.exe` completo difiere solo por
   el timestamp que MSVC incrusta en el PE.
4. Se comprueba determinismo: dos `--render 960 640` dan el mismo SHA-256.

Los binarios del snapshot no se versionan (ver `.gitignore`); se regeneran
desde el commit. `snapshot.json` y `baseline.json` conservan los hashes como
evidencia.

### Limitaciones honestas

- **R1 y R2 no renderizan la misma escena**: R1 usa el fixture `flex-page`
  (480×320, 23 rects, sin texto) y R2 el fixture canónico de 28 nodos con texto.
  Sirven para comparar tamaño/arranque/backend, no para paridad de píxeles.
- La VM está compartida con otros streams; los timings de CPU pueden variar.
  Por eso se reportan mediana y min/max de 10 runs, no un único valor.
- El `init` del runtime empieza después de crear la ventana; no incluye el
  spawn del proceso. El wall de `--frames 1` cubre esa parte como cota superior.
- El harness descarta un warm-up por escenario: la mediana representa arranque
  en caliente. Como referencia, el `build_report.json` del snapshot (primera
  ejecución tras compilar) registró **128.46 ms** de init frente a la mediana
  de esta tanda; para un SLA de primer arranque hay que medir cold start aparte.
- El peak working set incluye páginas compartidas de DLLs; el commit
  (`pagefile`) es informativo y en WARP incluye reservas grandes.
- No hay timestamp queries de GPU: el coste GPU se mide por wall/readback, no
  por contadores internos.

---

## 7. Próximos pasos recomendados

1. **Repetir en GPU hardware** (D3D12 real): es la única forma de validar el
   presupuesto de RSS y el coste offscreen. Mantener este baseline WARP como
   suelo de software.
2. **Instrumentar CPU frame en R1** (mismo patrón que R2: medir
   encode+submit sin present) para completar la comparación.
3. **Distribución de frame**: añadir p50/p95/p99 además de la media, sobre todo
   para el frame de ventana y el layout en resize.
4. **Layout 100 nodos canónico**: mover el fixture de 100 nodos a
   `benchmarks/frontend-parity` o a un fixture compartido si el equipo quiere
   que sea contrato, en vez de una variante local del harness.
5. **Umbrales de CI**: usar `baseline.json` para comparar contra el presupuesto
   y fallar si install size, startup o CPU frame se duplican (regla 1 de
   `docs/metrics.md`).
6. Si el RSS sigue fuera de presupuesto en hardware: evaluar backend
   D2D/D3D11, liberar recursos tras el init y medir el coste real de WARP como
   fallback software.

---

## 8. Referencias

- `docs/metrics.md` — definiciones y presupuesto fase 1.
- `docs/product-spec.md` — presupuesto de producto.
- `src/runtime/c-r2/README.md` — CLI `--render`, `--report`, `--frames`, `--states`.
- `archive/r1-static/README.md` — R1 archivado y `build_text.ps1`.
- `BASELINE.md` — resultados de la última ejecución.
