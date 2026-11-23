# Métricas y presupuesto

## Definiciones

| Métrica | Definición | Cómo se mide |
|---|---|---|
| **Install size** | `.exe` + `wgpu_native.dll` + assets (atlas, fuentes) | suma de archivos |
| **Engine size** | binario del runtime sin la DLL | tamaño del `.exe` |
| **Startup** | proceso → primer frame presentado | `--metrics` del runtime o wall externo |
| **RSS** | memoria residente pico | `psutil`/`GetProcessMemoryInfo` |
| **CPU frame** | tiempo de CPU por frame sin vsync | suma de layout + build + submit |
| **Layout** | tiempo de recalcular el árbol | `--report` R2 |
| **Offscreen render** | frame + readback + PNG | wall del proceso |
| **Input latency** | evento → frame visible | instrumentación futura |

## Metodología

- Windows 11 x64, WARP/D3D12 (sin GPU hardware en esta máquina).
- Cada métrica: 10 runs, mediana y min/max.
- Reportar siempre el backend y si es hardware o WARP.
- Los números de WARP **no** son representativos de GPU real; se marcan.
- Los benchmarks viven en `benchmarks/` y escriben JSON + Markdown.

## Presupuesto fase 1

| Métrica | Objetivo | Baseline actual |
|---|---:|---:|
| Install size (wgpu) | ≤ 15 MB | ~9.6 MB (9.1 DLL + 0.15–0.3 exe + atlas) |
| Engine size | ≤ 1 MB | 154–299 KB |
| Startup | ≤ 150 ms | 128–133 ms (R2) / ~100–150 ms (R1) |
| RSS | ≤ 30 MB | por medir |
| CPU frame | ≤ 2 ms | 0.46–0.70 ms |
| Layout 28 nodos | ≤ 0.5 ms | ~0.001 ms |
| Layout 100 nodos | ≤ 0.5 ms | por medir |
| Offscreen frame | informativo | 48–64 ms (readback+PNG) |

## Reglas de aceptación

1. Ninguna fase puede duplicar install size, startup o CPU frame sin justificación.
2. Si una feature rompe el presupuesto, se documenta y se decide.
3. WARP vs hardware se reporta por separado.
4. El harness debe ser reproducible con un comando.
5. Los resultados se guardan con fecha, commit y backend.

## Harness

`benchmarks/engine-metrics/` (fase 1):

- `measure.py` / `run.ps1`: ejecuta R1/R2, recolecta tamaños, startup, RSS,
  frame y layout.
- `baseline.json`: última medición.
- `BASELINE.md`: tabla legible + commit.
- No modifica el runtime; usa sus modos `--render`, `--frames`, `--report`.

## Nota sobre C#

El host C# AOT añade ~5–10 MB. Si el presupuesto se vuelve duro, C# queda como
validación de la ABI y el host de producción puede ser C/C++/Rust. La ABI no
cambia.
