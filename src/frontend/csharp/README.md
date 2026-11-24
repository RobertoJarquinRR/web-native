# Frontend C# — paridad con la referencia Python

> **Congelado (2026-11):** referencia de paridad del subset de
> `benchmarks/frontend-parity/SPEC.md`. No se actualiza con las features nuevas
> de Rust (grid, input, CSS v3). Si el SPEC cambia, se decide explícitamente si
> se actualiza o se queda fuera del subset.

Implementación en **C# / .NET 10** del frontend `HTML + CSS → display-list IR`
(rects) definido en `benchmarks/frontend-parity/SPEC.md`. No usa navegador,
WebView, Skia, Electron ni servidor; tampoco async. Convierte el documento en un
árbol DOM, resuelve la cascada, hace layout de bloque y flex y emite el JSON de
la display list.

## Resultado de paridad

Verificado con `benchmarks/frontend-parity/tools/verify_ir.py` (tolerancia
0.01 px, sin relajarla):

| Fixture | Ops | `verify_ir.py` | Bytes idénticos a Python |
|---|---:|---|---|
| `contract` | 4 | **PASS** | sí (594 B) |
| `flex-page` | 23 | **PASS** | sí (3018 B) |

Además de la igualdad semántica, el IR es **byte a byte igual** al que produce
la referencia Python en Windows: mismo orden de claves, enteros redondeados con
el mismo criterio (`round` half-to-even) y finales de línea CRLF (Python abre el
archivo en modo texto y en Windows traduce `\n` a `\r\n`).

## Arquitectura

Pipeline síncrono de una sola pasada, con medidas por etapa:

```
HTML ──► DOM (AngleSharp) ──► reglas CSS (AngleSharp.Css)
     ──► cascada por elemento (especificidad, orden, !important, herencia)
     ──► layout block/flex (border-box)
     ──► rects en pre-orden ──► IR JSON
```

| Archivo | Responsabilidad |
|---|---|
| `Program.cs` | CLI, orquestación, etapas, medidas, escritura de IR/timings |
| `Css.cs` | Parseo de stylesheets y `style=""`, expansión de shorthands, especificidad, longitudes y colores |
| `Cascade.cs` | Modelo DOM auxiliar, `ComputedStyle`, prioridad `(important, origen, especificidad, orden, índice)`, herencia |
| `Layout.cs` | Layout de bloque, flex row/column, gap, justify-content, align-items, auto width/height |
| `Ir.cs` | `DisplayList` + serializador idéntico a `to_json(indent=2)` |
| `Json.cs` | Números/strings JSON con formato estable |
| `run.ps1` | Entry point del harness (`-Html -Width -Height -Out [-Timings]`) |
| `build.ps1` | Publica el binario (AOT por defecto) y escribe `BUILD_INFO.json` |
| `bench-local.py` | Benchmark local contra el binario directo (sin PowerShell), escribe `results/csharp/local-bench.json` |

### Decisiones de port fieles a la referencia

- **Cascada**: se usa `QuerySelectorAll(selectorText)` de AngleSharp por regla;
  la especificidad se calcula con un escáner propio y el desempate replica la
  tupla de Python: `(important, origen, (ids, clases, tipos), orden de selector,
  índice de declaración)`. `style=""` gana a selectores normales salvo
  `!important`, igual que en Python.
- **Herencia**: solo `color`, `font-family`, `font-size`, `font-weight`,
  `line-height`, `text-align`, `white-space`, copiadas del `raw` del padre.
- **UA display**: `head/link/style/script/...` → `none`; `a/span/em/...` →
  `inline`; el resto → `block`. Solo se renderizan hijos `block`/`flex`.
- **Layout**: `width`/`height` son border-box. El bloque apila en `content_y` y
  avanza a `child.bottom + margin_bottom`. El flex mide cada hijo en `(0,0)` con
  el ancho de contenido del padre y luego traslada el subárbol (idéntico al
  `translate` de Python). `space-between` usa `gap + free/(n-1)`; `stretch`
  solo actúa si el hijo no fijó esa dimensión.
- **Pintado**: pre-orden desde `body`; fondo del elemento antes que sus hijos.

## Paquetes NuGet

| Paquete | Versión | Uso |
|---|---|---|
| `AngleSharp` | 1.10.5 | Parser HTML5 (html/head/body implícitos), DOM, `QuerySelectorAll` |
| `AngleSharp.Css` | 1.5.0 | Parser CSS, `ICssStyleRule`/`ICssStyleDeclaration` (expande shorthands a longhands) |

Target: `net10.0`, SDK **10.0.403**. `InvariantGlobalization`, sin GC de
servidor, una sola pasada sin hilos adicionales.

## Build y uso

```powershell
# Publica el binario nativo AOT en out\ y escribe BUILD_INFO.json
.\build.ps1

# Fuerza el publish framework-dependent (sin AOT) en out-jit\
.\build.ps1 -Jit

# Ejecutar un fixture (el harness llama exactamente así)
.\run.ps1 -Html ..\..\..\benchmarks\frontend-parity\fixtures\contract\input.html `
          -Width 400 -Height 300 -Out ..\..\..\benchmarks\frontend-parity\results\csharp\contract.ir.json `
          -Timings ..\..\..\benchmarks\frontend-parity\results\csharp\contract.timings.json
```

- Las rutas relativas de `run.ps1` se resuelven contra el directorio del script.
- `run.ps1` elige el binario más rápido disponible: `out\` (AOT), `out-jit\`
  (JIT publicado) y, como último recurso, `bin\Release\net10.0\...`.
- `BUILD_INFO.json`: `{ binary, build_mode, dotnet_version, binary_bytes, built_at }`.
- Salida: código 0 en éxito; 1 en error de pipeline (p. ej. stylesheet
  inexistente); 2 en error de argumentos.

## AOT vs JIT

El binario que se entrega es **Native AOT** (`PublishAot=true`, `win-x64`,
`OptimizationPreference=Speed`, `StripSymbols=true`). Se probó también el
publish framework-dependent (JIT) con el mismo código:

| Build | Tamaño | Wall directo (mediana) | Total (mediana) | Peak RSS |
|---|---:|---:|---:|---:|
| **Native AOT** | 8,379,392 B | **≈ 36–41 ms** | **11–12.3 ms** | ≈ 18 MB |
| JIT framework-dependent | 162,304 B | ≈ 641 ms | ≈ 529 ms | ≈ 52 MB |

El JIT paga la inicialización del runtime y la compilación JIT de AngleSharp.Css
en cada proceso (etapa `css` ≈ 300 ms en frío); AOT arranca en milisegundos y
usa menos memoria. Por eso AOT es el modo por defecto y el que usa el benchmark.
`build.ps1` cae automáticamente al publish JIT si la toolchain nativa no está
disponible, y lo refleja en `BUILD_INFO.json`.

## Tiempos

Las cifras varían ±10–20 % entre ejecuciones (carga de la máquina, caché de
ficheros). Las tablas siguientes son las medianas de los artefactos publicados
en `benchmarks/frontend-parity/results/csharp/`.

### Harness oficial (`bench.py --runs 10`)

El harness usa directamente el binario indicado en `BUILD_INFO.json` cuando
existe (`mode=direct`, sin shell intermedio):

| Fixture | Modo | Wall mediana | Min | Max | Ops |
|---|---|---:|---:|---:|---:|
| `contract` | direct | 39.0 ms | 34.4 ms | 44.4 ms | 4 |
| `flex-page` | direct | 41.0 ms | 34.1 ms | 54.8 ms | 23 |

Si se ejecuta a través de `run.ps1` (el contrato del CLI), se añade el arranque
de PowerShell: ≈ 220 ms en esta máquina, medido con un script vacío. Con ese
camino las medianas eran ≈ 287 ms (`contract`) y ≈ 308 ms (`flex-page`).
`run.ps1` evita cmdlets de PowerShell a propósito (cargan
`Microsoft.PowerShell.Management`, ≈ 100 ms extra): solo usa APIs de .NET
(`System.IO.Path`, `System.IO.File`).

### Binario directo, medición local (`bench-local.py`, 10 runs + 2 warmup)

| Fixture | Wall mediana | Min | Max | `total` mediana | Peak RSS |
|---|---:|---:|---:|---:|---:|
| `contract` | 36.1 ms | 34.3 ms | 41.2 ms | 11.0 ms | 18,152 KB |
| `flex-page` | 37.2 ms | 35.1 ms | 40.8 ms | 12.3 ms | 18,532 KB |

El wall directo incluye creación de proceso y carga de la imagen (≈ 25 ms);
`total` es el tiempo medido dentro del proceso (la suma de las etapas medidas).

### Etapas internas (mediana, ms)

| Fixture | parse (HTML) | css | cascade | layout | emit | total |
|---|---:|---:|---:|---:|---:|---:|
| `contract` | 4.14 | 5.51 | 0.29 | 0.009 | 0.76 | 11.04 |
| `flex-page` | 4.22 | 6.01 | 0.60 | 0.026 | 0.69 | 12.26 |

`css` incluye la inicialización/parseo de AngleSharp.Css (el coste dominante del
pipeline); `emit` incluye serializar y escribir el IR. `peak_rss_kb` se toma del
propio proceso (`Process.PeakWorkingSet64`).

## Tamaño del binario

`out/frontend-csharp.exe`: **8,379,392 bytes** (7.99 MiB), autocontenido, sin
runtime .NET instalado. El publish genera además un PDB de depuración que
`build.ps1` elimina del directorio entregado.

## Limitaciones (subconjunto de la ronda)

- Solo op `rect`; **no hay texto** (`text_run`). Los fixtures son rect-only y el
  layout ignora el texto propio de los nodos (Python avanzaría una line box con
  métricas de fuente). Los nodos de texto en blanco se ignoran, como en SPEC.
- Propiedades soportadas: `display: block|flex`, `flex-direction`, `gap`,
  `justify-content`, `align-items`, `width`, `height`, `padding*`, `margin*`,
  `background`/`background-color`, y `color`/`font-size`/`font-family` solo
  parseadas. El resto se parsea y se ignora.
- Sin margin collapsing, bordes, floats, grid, porcentajes, `calc()`, wrap,
  grow/shrink. Un contenedor con altura fija no recorta (los ops pueden salir).
- Selectores: tipo, clase, id, compuesto, descendiente y listas con coma.
  Los pseudo-elementos (`::before`, `:before`…) descartan la regla, como
  `pseudo_element is None` en Python. Pseudo-clases de estado (`:hover`) no
  matchean nunca. `@media`/at-rules se ignoran (la referencia también lee solo
  reglas cualificadas de primer nivel).
- Colores: `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb()/rgba()`,
  `hsl()/hsla()` y una tabla parcial de nombres CSS (≈ 70). Alfa ≤ 0 ⇒ sin fondo.
- AngleSharp.Css valida las declaraciones al parsear: las inválidas se
  descartan y las duplicadas se colapsan (la última gana, conservando
  `!important`). La referencia conserva el texto crudo y resuelve con su propia
  prioridad; para el subconjunto de los fixtures el resultado es idéntico.
- `BUILD_INFO.json` y las medidas corresponden a Windows `win-x64`.

## Reproducir la verificación

```powershell
.\build.ps1
python ..\..\..\benchmarks\frontend-parity\tools\verify_ir.py `
  ..\..\..\benchmarks\frontend-parity\golden\contract.ir.json `
  ..\..\..\benchmarks\frontend-parity\results\csharp\contract.ir.json
python ..\..\..\benchmarks\frontend-parity\tools\bench.py --runs 10 --only csharp
python .\bench-local.py
```
