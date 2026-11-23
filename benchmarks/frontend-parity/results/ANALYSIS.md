# Análisis: frontend HTML/CSS en 4 lenguajes

Fecha: 2026-11-23. Harness: `benchmarks/frontend-parity/tools/bench.py`,
10 runs por frontend/fixture + 2 warmup, invocación directa del binario
(sin PowerShell), mediana reportada.

## Paridad

| Frontend | contract (4 ops) | flex-page (23 ops) | IR byte-idéntico |
|---|---|---:|---|
| Python (referencia) | PASS | PASS | — |
| C++ | PASS | PASS | ✅ |
| Rust | PASS | PASS | ✅ |
| C# (.NET AOT) | PASS | PASS | ✅ |

Los tres alternativos producen exactamente los mismos bytes que el golden de
Python. C++ validó además 21 casos curados + 120 documentos aleatorios; Rust,
9 casos diferenciales (justify/align/column/selectores/especificidad/
`!important`/shorthands/`%`/flex anidado).

## Velocidad

| Frontend | Wall contract | Wall flex-page | Pipeline contract | Pipeline flex-page |
|---|---:|---:|---:|---:|
| Python | 393.8 ms | 402.6 ms | 18.67 ms | 24.65 ms |
| **C++** | 11.8 ms | 12.1 ms | **0.75 ms** | **1.01 ms** |
| **Rust** | **9.9 ms** | **10.7 ms** | 1.43 ms | 2.28 ms |
| C# AOT | 31.7 ms | 35.6 ms | 9.85 ms | 12.34 ms |

- **Rust gana en wall** (arranque + proceso): ~10–11 ms.
- **C++ gana en pipeline puro** (sin contar arranque): ~0.75–1.0 ms.
- **C#** paga AngleSharp (parse ~4.3 ms, CSS ~4.6–6.6 ms) y el arranque AOT;
  aun así es ~2× más rápido que Python en pipeline.

## Eficiencia

| Frontend | Binario / deploy | Peak RSS |
|---|---:|---:|
| **C++** | **715 KB** | **3.5 MB** |
| Rust | 887 KB | 4.0 MB |
| C# AOT | 8.38 MB | 18.2 MB |
| Python | venv 35.4 MB + CPython | n/d |

## Veredicto

1. **Paridad: empate técnico.** Los tres son byte-idénticos en los fixtures y
   pasan las pruebas diferenciales. La elección no es de correctness.
2. **Velocidad: Rust y C++ empatan en la práctica.** Rust arranca ~2 ms antes;
   C++ procesa ~0.5–1.2 ms menos. En un pipeline real (muchos documentos) el
   pipeline pesa más → C++; en CLI de invocación única → Rust.
3. **Eficiencia: C++.** Menor binario (715 KB vs 887 KB vs 8.4 MB) y menor RSS
   (3.5 MB). Rust es casi igual; C# es 10× más pesado.
4. **Python queda como oráculo y herramienta de iteración**, no como runtime de
   producción: 20–40× más lento y 35 MB+ de dependencias.

## Recomendación

- **Frontend de producción:** Rust o C++. Rust si importa el ecosistema,
  seguridad de memoria y mantenibilidad; C++ si importan el tamaño mínimo y el
  pipeline absoluto.
- **C#:** válido si el equipo vive en .NET; el coste es 8 MB y 18 MB RSS.
- **Python:** mantenerlo como referencia/oráculo y para prototipar features
  nuevas antes de portarlas.

## Caveats

- Fixtures pequeños (4 y 23 ops). El ranking de pipeline puede cambiar con
  documentos grandes; falta un benchmark de escalado (1k–10k nodos).
- Wall incluye arranque del runtime; el pipeline interno separa el algoritmo.
- Parsers distintos (Gumbo vs html5ever vs AngleSharp) coinciden en estos
  fixtures; casos malformados o CSS exótico pueden divergir.
- Sin texto (`text_run`) en esta ronda: el texto añade HarfBuzz/FreeType y
  cambia el coste de parse/layout.
