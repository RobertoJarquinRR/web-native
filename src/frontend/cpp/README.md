# Frontend C++ — paridad con la referencia Python

Implementación nativa en C++20 del frontend HTML+CSS → IR de display-list, sin
navegador, WebView, Skia ni Electron. Reproduce la semántica de
`src/frontend/python/webnative` y genera un IR **byte-idéntico** al golden
para los dos fixtures del benchmark.

| Fixture | Ops | `verify_ir.py` | SHA-256 IR | Bytes |
|---|---:|---|---|---:|
| `contract` | 4 | **PASS** | `94c022c8…e36e2` (igual al golden) | 594 |
| `flex-page` | 23 | **PASS** | `d2129495…e515f` (igual al golden) | 3018 |

No es solo equivalencia geométrica: los ficheros de salida son iguales byte a
byte a `golden/contract.ir.json` y `golden/flex-page.ir.json` (mismo JSON,
indentación, CRLF y redondeo de geometría).

## Estado

- Paridad obligatoria: **2/2 PASS**, tolerancia 0.01 px, sin relajarla.
- Paridad extra contra Python: **21/21** documentos diferenciales y **120/120**
  documentos aleatorios (semilla fija), todos también byte-idénticos. Ver
  `tests/diff_cases.py` y `tests/fuzz_random.py`.
- Benchmark: 10 ejecuciones por fixture (+2 de warmup), tiempos internos y wall
  time registrados en `benchmarks/frontend-parity/results/cpp/local-bench.json`.
- `frontend-cpp.exe`: **715 264 bytes** (698,5 KiB), Release `/O2 /MT`, CRT
  estático (no requiere redistribuible de Visual C++).

## Arquitectura

```
src/
  main.cpp      CLI, orquestación y medida de etapas
  html.hpp/.cpp HTML -> árbol Node con Gumbo (html5lib-equivalente)
  css.hpp/.cpp  parser CSS: reglas, declaraciones, shorthands, selectores,
                especificidad, longitudes px y colores
  cascade.hpp/.cpp  matching derecha-izquierda, cascada, herencia, style="",
                !important, display UA y estilo computado
  layout.hpp/.cpp   bloque + flex row/column, gap, justify-content, align-items,
                stretch, border-box, tamaños automáticos; pintado pre-orden
  ir.hpp/.cpp   serialización del IR y de los timings compatible con Python
  util.hpp/.cpp IO de ficheros UTF-8/wide, tiempos, formato de números JSON
third_party/gumbo/
  src/           Gumbo 0.10.1 vendorizado (11 fuentes C, Apache-2.0)
  compat/        shim MSVC para <strings.h> (strcasecmp -> _stricmp)
tests/
  diff_cases.py  suite diferencial opcional C++ vs Python (21 casos)
  fuzz_random.py 120 documentos aleatorios deterministas C++ vs Python
```

Flujo: leer HTML → `gumbo_parse` → árbol `Node` propio → recolectar
`<style>` y `<link rel=stylesheet>` (href relativo al HTML) → parsear CSS →
cascada por nodo → layout del `<body>` → rects en pre-orden → JSON.

Decisiones que garantizan la paridad:

- **Modelo border-box**: `width`/`height` incluyen padding, como la v1 Python.
- **Bloque**: el cursor avanza con `bottom + margin-bottom`; `margin-top` solo
  desplaza al hijo (no se colapsa ni se suma al cursor), exactamente como
  `layout.py`.
- **Flex**: los hijos se miden en `(0,0)` con el ancho de contenido del padre y
  luego se trasladan. `stretch` solo actúa si el hijo no fijó la dimensión
  cruzada; el extent es la altura de contenido (row) o el ancho de contenido
  (column). `space-between` reparte `free/(n-1)`; con un solo hijo el offset es
  0 (o `free/2`/`free` para center/end), igual que `_justify`.
- **Cascada**: clave `(important, inline, ids, classes, types, orden de regla,
  índice de declaración)`; `!important` gana, `style=""` gana a selectores
  normales, y a igualdad gana la última declaración. Cada selector de una lista
  con comas consume su propio índice de orden.
- **Herencia**: solo `color`, `font-family`, `font-size`, `font-weight`,
  `line-height`, `text-align`, `white-space`, como Python.
- **Gap**: se usa `gap` si está definido, incluso `gap: 0`; solo si falta se
  prueba `row-gap`/`column-gap`. (Detectado con un test diferencial: tratar
  `gap: 0` como ausencia producía 10 px de diferencia.)
- **Redondeo**: la geometría del IR usa `round()` de Python (ties-to-even) a
  entero, vía `nearbyint`; el JSON replica `json.dumps(indent=2)` con CRLF.
- **Pintado**: fondo del `<body>` primero y después pre-orden; un rect por
  elemento con `background`/`background-color` válido.

### Dependencia HTML: Gumbo 0.10.1 (vendorizado)

Se eligió **Gumbo** (Google, C, Apache-2.0) frente a Lexbor por tamaño y
simplicidad: implementa el algoritmo de construcción de árbol HTML5, crea los
nodos implícitos `html/head/body` y gestiona comentarios/atributos como
html5lib. Está archivado (última release 2015), pero es estable y suficiente
para el subconjunto; se vendorizan sus 11 fuentes C en `third_party/gumbo/src`
para que el build no dependa de red. El único parche es
`third_party/gumbo/compat/strings.h`, que mapea las funciones POSIX
`strcasecmp`/`strncasecmp` a `_stricmp`/`_strnicmp` en MSVC.

No hay más dependencias de terceros: el parser CSS, la cascada, el layout y el
serializador JSON están escritos a mano.

## Build

Requiere Windows + Visual Studio con el workload C++ (probado con MSVC
19.51.36260, toolset 14.51.36231, VS 18 Community) y PowerShell 5.1.

```powershell
cd .\src\frontend\cpp
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
# limpio:
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Clean
```

`build.ps1` localiza `vcvars64.bat` (ruta por defecto o `-VsPath`, con fallback a
`vswhere`), importa el entorno, compila Gumbo como C (`/W0`) y el frontend como
C++20 (`/O2 /MT /W4 /EHsc`), enlaza `build\frontend-cpp.exe` y escribe
`BUILD_INFO.json`:

```json
{
  "binary": "build/frontend-cpp.exe",
  "build_mode": "release (/O2, /MT, C++20)",
  "compiler": "Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36260 for x64",
  "binary_bytes": 715264,
  "built_at": "2026-11-23T20:53:53Z"
}
```

`CMakeLists.txt` se incluye como alternativa para IDE/CMake
(`build-cmake\Release\frontend-cpp.exe`); el build canónico y el medido es
`build.ps1`.

## Uso

Wrapper del harness (las rutas relativas se resuelven contra el directorio del
script, igual que el frontend C#; el ejecutable también se busca relativo al
script):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\run.ps1 `
  -Html .\benchmarks\frontend-parity\fixtures\contract\input.html `
  -Width 400 -Height 300 `
  -Out  .\benchmarks\frontend-parity\results\cpp\contract.ir.json `
  -Timings .\benchmarks\frontend-parity\results\cpp\contract.timings.json
```

CLI directa:

```text
frontend-cpp.exe --html <path> --width <W> --height <H> --out <ir.json>
                 [--timings <timings.json>]
```

Salida 0 en éxito; 1 en error de parseo/fichero; 2 en error de argumentos. El
`<link rel="stylesheet">` se resuelve respecto al directorio del HTML; se
ignoran `http:`, `https:` y `//`.

## Verificación de paridad

```powershell
cd .
python benchmarks\frontend-parity\tools\verify_ir.py `
  benchmarks\frontend-parity\golden\contract.ir.json `
  benchmarks\frontend-parity\results\cpp\contract.ir.json
# PASS: ...\contract.ir.json == ...\contract.ir.json

python benchmarks\frontend-parity\tools\verify_ir.py `
  benchmarks\frontend-parity\golden\flex-page.ir.json `
  benchmarks\frontend-parity\results\cpp\flex-page.ir.json
# PASS: ...\flex-page.ir.json == ...\flex-page.ir.json
```

Comprobación adicional (suites diferenciales opcionales; usan la venv de Python
de la referencia, solo lectura):

```powershell
python src\frontend\cpp\tests\diff_cases.py
# 21/21 cases match

python src\frontend\cpp\tests\fuzz_random.py
# 120/120 random cases match
```

## Benchmark

`bench-local.py` ejecuta el binario directamente y también a través de
`run.ps1` (el camino exacto de `tools/bench.py`), 10 repeticiones + 2 warmup por
fixture. Guarda todo en
`benchmarks/frontend-parity/results/cpp/local-bench.json` y refresca
`<fixture>.ir.json` / `<fixture>.timings.json`.

```powershell
python src\frontend\cpp\bench-local.py
```

### Wall time

| Fixture | Binario directo (mediana) | min | max | Vía `run.ps1` (mediana) | Pipeline interno (mediana) | Peak RSS |
|---|---:|---:|---:|---:|---:|---:|
| `contract` | 11.706 ms | 11.190 | 12.123 | 245.776 ms | 0.617 ms | 3 536 KB |
| `flex-page` | 12.329 ms | 11.538 | 13.277 | 246.080 ms | 1.058 ms | 3 596 KB |

El wall directo está dominado por el arranque del proceso en Windows (Defender,
creación de proceso, CRT estático); el pipeline real es <2 ms. El wall vía
`run.ps1` incluye ~250 ms de arranque de PowerShell (hasta ~400 ms bajo carga) y
es el que mide el harness oficial, por lo que no aísla el coste del frontend.

### Etapas internas (mediana de 10 ejecuciones, ms)

Nombres idénticos al esquema de Python: `html` = lectura HTML + Gumbo +
lectura de CSS; `text+ir` = pintado + serialización JSON (etapa *emit*);
`pipeline` = total hasta tener el IR (sin escribir ficheros).

| Etapa | `contract` C++ | `flex-page` C++ | `contract` Python (golden) | `flex-page` Python (golden) |
|---|---:|---:|---:|---:|
| `html` (parse) | 0.496 | 0.635 | 34.904 | 36.473 |
| `css` | 0.043 | 0.110 | 1.590 | 7.798 |
| `cascade` | 0.067 | 0.262 | 0.516 | 1.878 |
| `fonts` | 0.000 | 0.000 | 0.008 | 0.017 |
| `layout` | 0.007 | 0.019 | 0.044 | 0.206 |
| `text+ir` (emit) | 0.004 | 0.014 | 0.018 | 0.049 |
| **`pipeline` (total)** | **0.617** | **1.058** | **37.115** | **46.465** |

Aceleración del pipeline: **~60×** (`contract`) y **~44×** (`flex-page`). La
mayor parte viene de `html`, porque la referencia paga la importación de
html5lib en cada proceso y aquí Gumbo parsea directamente; `css` y `cascade`
también bajan por el parser específico y el matching simple. Los tiempos Python
son una sola ejecución de los ficheros `golden/*.timings.json`, no un benchmark
controlado; la comparación es indicativa.

Pico de RSS: ~3,5 MB (medido con `K32GetProcessMemoryInfo`, Working Set pico).
Los `.timings.json` incluyen `peak_rss_kb` además de las claves de Python
(permitido por la SPEC).

## Limitaciones

El contrato es el subconjunto de la SPEC y los fixtures son rect-only. Fuera de
ese alcance:

- **Texto**: se ignoran los nodos de texto; no se emiten `text_run`, no se
  cargan fuentes y `fonts` siempre es 0. Un documento con texto no coincide con
  Python (que aplica métricas reales y desplaza el cursor vertical).
- Solo ops `rect`; sin bordes, radios, sombras, gradientes, imágenes ni texto.
- CSS: `display: block | flex` (otros valores se parsean para filtrar hijos);
  `flex-direction: row | column` (cualquier otro valor se trata como row);
  `gap`, `row-gap`, `column-gap`; `justify-content` start/center/end/
  space-between; `align-items` stretch/flex-start/center/flex-end/normal;
  longitudes **solo px** (%, em, rem, calc(), var() → auto). Sin wrap, grow,
  shrink, order, align-self ni align-content.
- Selectores: tipo, clase, id, universal, compuestos, descendiente, hijo (`>`),
  listas con comas y selectores de atributo simples (`[a]`, `[a=v]`, `~=`, `|=`,
  `^=`, `$=`, `*=`). Los combinadores hermanos (`+`, `~`), pseudoclases y
  pseudoelementos se ignoran (nunca casan); los at-rules (`@media`, `@import`…)
  se saltan sin cargar hojas externas.
- Colores: `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb()`/`rgba()` y una
  tabla corta de nombres (`red`, `blue`, `lime`, `transparent`…). `hsl()` y
  nombres no listados → sin fondo. `background` solo se acepta si su valor
  completo es un color, igual que el `parse_color(raw.get(...))` de Python.
- Sin `<base>`, sin hojas remotas, sin `@import`, sin herencia de propiedades
  fuera del conjunto de Python.
- Build solo Windows/MSVC (`wmain`); no se compila con GCC/Clang ni en Linux.
- La igualdad byte a byte se verificó en Windows con este toolchain (CRLF,
  `json.dumps(indent=2)` y `round()` ties-to-even). En otra plataforma el JSON
  sería semánticamente igual pero podría diferir en fin de línea.

## Reproducir todo

```powershell
cd .\src\frontend\cpp
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Clean
python .\bench-local.py
python .\tests\diff_cases.py
python .\tests\fuzz_random.py
cd .
python benchmarks\frontend-parity\tools\verify_ir.py benchmarks\frontend-parity\golden\contract.ir.json benchmarks\frontend-parity\results\cpp\contract.ir.json
python benchmarks\frontend-parity\tools\verify_ir.py benchmarks\frontend-parity\golden\flex-page.ir.json benchmarks\frontend-parity\results\cpp\flex-page.ir.json
```

## Ficheros generados

- `build\frontend-cpp.exe` — binario Release.
- `BUILD_INFO.json` — binario, modo, compilador, bytes, fecha UTC.
- `benchmarks\frontend-parity\results\cpp\<fixture>.ir.json` — IR verificado.
- `benchmarks\frontend-parity\results\cpp\<fixture>.timings.json` — timings de
  la última ejecución.
- `benchmarks\frontend-parity\results\cpp\local-bench.json` — 10 runs, stats de
  wall y de cada etapa, peak RSS.
