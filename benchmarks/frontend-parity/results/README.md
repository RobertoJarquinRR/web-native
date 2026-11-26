# Frontend parity — snapshot congelado

**Subset:** `benchmarks/frontend-parity/SPEC.md` v1 (rect-only: `contract` y
`flex-page`). **Fecha:** 2026-11-24. **Código medido:** `55b8f7f`.

> **Snapshot histórico, sin runner.** El harness (incluido el comparador de
> estilos) ya no existe y este directorio no forma parte del flujo de
> verificación. C++/C#/Python quedan congelados en `archive/frontends-legacy/`;
> las filas de abajo documentan la corrida de cuatro frontends de esa fecha.

Los cuatro frontends pasaban la paridad byte-idéntica contra los goldens, con
tolerancia 0.01 px:

| Frontend | `contract` (ms) | `flex-page` (ms) | Estado |
|---|---:|---:|---|
| Python (oráculo) | 386.318 | 451.296 | PASS |
| C++ | 11.756 | 14.265 | PASS |
| Rust (canónico) | 10.158 | 11.831 | PASS |
| C# | 30.211 | 31.459 | PASS |

Mediana de 3 ejecuciones + 2 de warmup; wall incluye arranque del proceso,
parseo, cascada, layout y escritura del IR. El detalle por ejecución está en
`SUMMARY.md` y `benchmark.json`.

## Qué significa “congelado”

- **Rust** es el frontend canónico y sigue avanzando con CSS v3, grid, input,
  etc.
- **Python** es el oráculo del subset v1; no se actualiza con las features
  nuevas de Rust.
- **C++ y C#** son **referencias de paridad congeladas** en `SPEC.md`. No se
  actualizan para seguir a Rust; su obligación es producir el mismo IR en los
  fixtures del SPEC.

Si el IR o el SPEC cambian de forma incompatible, se decide explícitamente:
actualizar los frontends congelados o declararlos fuera del subset actual.
No se mantienen “en silencio”.

## Tamaño y coste (snapshot de 10 runs)

Del análisis histórico en `ANALYSIS.md`:

| Frontend | Binario / deploy | RSS pico |
|---|---:|---:|
| C++ | 715 KB | 3.5 MB |
| Rust | 887 KB | 4.0 MB |
| C# | 8.38 MB | 18.2 MB |
| Python | venv 35.4 MB | n/d |

Los binarios actuales pueden variar; lo estable es la comparación relativa.

## Reproducir

No hay comando de reproducción: el runner se retiró junto con el IR intermedio.
Este `README.md` y los resultados de la corrida se conservan como snapshot
estable; los gates vivos son `wn-cli` (`test all`, `bench pilot`).

## Archivos

- `SUMMARY.md` — tabla generada de la última corrida.
- `benchmark.json` — datos crudos de la última corrida (rutas relativas).
- `ANALYSIS.md` — análisis comparativo de 10 runs.
- `results/<lang>/` — IR y timings por frontend/fixture.
- `golden/` — IR de referencia del oráculo Python.
