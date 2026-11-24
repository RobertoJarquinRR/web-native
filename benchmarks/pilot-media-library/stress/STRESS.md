# Stress del engine R2 — grid, scroll, imágenes y layout

Este directorio estresa el runtime R2 (`src/runtime/c-r2`) con dos familias de
fixtures generadas y mide layout, render offscreen, rueda, caché de imágenes y
memoria. **No modifica** runtime, frontend, emisores, `build.ps1`, el piloto de
30 cards ni el proyecto Desktop. Todo lo generado vive bajo
`benchmarks/pilot-media-library/stress/`.

- Resultado de la última ejecución real: **94/94 checks PASS**.
- Mediana de **7 ejecuciones** por medición (el mínimo pedido era 5).
- Fecha de la ejecución: 2026-11-23.
- Los números crudos, el método completo y los chequeos están en
  [`stress_report.json`](stress_report.json).

---

## 1. Advertencia de hardware (leer antes de comparar)

La máquina de esta ejecución **no tiene GPU discreta disponible para el
runtime**:

```
backend=D3D12 (4) type=cpu vendorID=0x1414 deviceID=0x008C
device=Microsoft Basic Render Driver
```

Eso es **WARP**, el rasterizador software de Microsoft sobre D3D12. Todas las
cifras de `frame`, `gpu`, `readback`, `png` y memoria son de CPU/WARP:

- no representan el coste en una GPU hardware;
- `readback(map)` y la codificación PNG son costes del modo **offscreen** que no
  existen al presentar una ventana;
- el commit de WARP (~350 MiB de pagefile) domina la memoria y apenas cambia
  entre 30 y 300 cards.

El objetivo de este informe es fijar una **baseline reproducible y un techo
pesimista**, no afirmar cuánto tarda una GPU real.

Host: Windows 10 10.0.19045, `Intel64 Family 6 Model 42 Stepping 7`, 8 CPUs
lógicas, Python 3.12.10.

### Resumen ejecutivo

| Medición (1280x800) | Resultado |
|---|---|
| Layout 300 cards (2718 nodos) | **0.300 ms** |
| Layout 1000 nodos sintéticos | **0.067 ms** |
| Frame offscreen 30 cards | **190.0 ms** |
| Frame offscreen 300 cards | **193.0 ms** (x1.02) |
| Frame offscreen, 1000 nodos sin imágenes | 71.1 ms |
| `readback` + `png` del frame de 300 cards | **60.5% + 19.8%** |
| Primer frame vs warm (300 cards) | **184.1 / 91.7 ms** |
| Rueda a bottom (300 cards, 800x600) | offset **25032/25032**, banda 502–509 |
| Memoria 300 cards | **77.9 MiB WS / 361.6 MiB pagefile** |
| Checks | **94/94 PASS** |

La conclusión corta: **el layout no es el problema**; el coste medido está en el
pase offscreen (WARP) y su readback/PNG, y el engine ya culla imágenes y texto.
Lo que aún escala con el catálogo es la geometría sólida (6 vértices/card), el
layout completo en cada rueda y la caché de imágenes sin eviction.

---

## 2. Qué se genera

`generate.py` escribe `variants/<id>/page/{index.html,style.css}` y
`variants/manifest.json`. Es determinista e idempotente.

### 2.1 Cards (carga real de la biblioteca)

| Variante | Cards | Nodos UI | Imágenes | Posters únicos |
|---|---:|---:|---:|---:|
| `cards-030` | 30 | 288 | 30 | 30 |
| `cards-100` | 100 | 918 | 100 | 30 |
| `cards-300` | **300** | **2718** | **300** | **30** |

Contrato CSS, idéntico al piloto en lo que importa:

```css
.poster-grid {
  width: 100%; height: 100%;
  display: grid;
  grid-template-columns: repeat(auto-fill, 180px);
  grid-auto-rows: 240px;
  gap: 16px;
  overflow: auto;              /* scroll vertical + clip */
}
.poster-card { width: 180px; height: 240px; overflow: hidden; }
.poster      { width: 180px; height: 180px; object-fit: cover; }
.poster-meta { height: 52px; padding: 4px 8px; }  /* 60 de caja */
```

Cada card tiene título, duración/resolución y tags. Los posters son **los 30
PNG del piloto, ciclados** (`i % 30`); no se descarga ni genera ningún asset
nuevo. El sidebar 20% / main 80% con padding 24 reproduce las mismas columnas
que el piloto: **5 columnas a 1280x800** y **3 a 800x600**.

Geometría congelada por el generador:

| Variante | Viewport | Columnas | Filas | Alto contenido | `scroll_max` |
|---|---|---:|---:|---:|---:|
| cards-030 | 1280x800 | 5 | 6 | 1520 | 768 |
| cards-030 | 800x600 | 3 | 10 | 2544 | 1992 |
| cards-100 | 1280x800 | 5 | 20 | 5104 | 4352 |
| cards-100 | 800x600 | 3 | 34 | 8688 | 8136 |
| cards-300 | 1280x800 | 5 | 60 | 15344 | **14592** |
| cards-300 | 800x600 | 3 | 100 | 25584 | **25032** |

`run_stress.py` recalcula estas fórmulas y compara **las 300 posiciones** de
card e imagen del `--report` contra ellas en ambos viewports.

### 2.2 Nodos sintéticos (escalado de layout)

| Variante | Nodos | Frames anidados | Columnas/filas | Celdas | Profundidad |
|---|---:|---:|---:|---:|---:|
| `nodes-0030` | **30** | 8 | 4 | 12 | 12 |
| `nodes-0100` | **100** | 10 | 6 | 76 | 14 |
| `nodes-0300` | **300** | 12 | 8 | 270 | 16 |
| `nodes-1000` | **1000** | 12 | 8 | 970 | 16 |

Son divs anidados sin texto ni imágenes: cada div es exactamente un nodo del
runtime. `1 root + frames + 1 board + columns + rows + cells = N` exacto; el
runtime reporta 30/100/300/1000 y el script lo verifica.

### 2.3 Página 1000 nodos

La página `nodes-1000` tiene 1000 nodos y 16 niveles de anidamiento
(root → 12 frames → board → column → row → cell). Renderiza cuatro colores de
celda, sin texto, y se usa solo para medir layout y geometría.

---

## 3. Cómo se ejecuta

Requisitos: runtime ya compilable con MSVC (mismo entorno que
`src/runtime/c-r2/build.ps1`), `frontend-rust.exe` precompilado, y los 30
posters del piloto. No hay red. Si falta el frontend:

```powershell
cargo build --release --manifest-path src\frontend\rust\Cargo.toml
```

```powershell
# Suite completa (genera páginas, emite, compila y mide)
powershell -NoProfile -ExecutionPolicy Bypass `
  -File benchmarks\pilot-media-library\stress\run_stress.ps1

# Equivalente en Python, con opciones
src\frontend\python\.venv\Scripts\python.exe `
  benchmarks\pilot-media-library\stress\run_stress.py --runs 7

# Iterar sin recompilar / sin reemitir
... run_stress.py --runs 7 --skip-emit --skip-compile

# Una sola variante
... run_stress.py --only cards-300 --runs 7

# Regenerar HTML/CSS desde generate.py
... run_stress.py --regenerate
```

Pasos internos, exactamente los del build canónico pero con otro
`ui_data.h` (el `build.ps1` no se toca):

```text
frontend-rust --html <page>\index.html --css <page>\style.css \
  --width 1280 --height 800 --emit-ui <build>\ui.json --pilot
src\emitter\python\emit_ui.py --ui-json <build>\ui.json \
  --out <build>\ui_data.h --meta <build>\ui_meta.json
cl /nologo /O2 /W3 /std:c17 /TC /D_CRT_SECURE_NO_WARNINGS /DWN_STATIC \
  runtime_r2.c wn_core.c wn_common.c \
  /I include /I src\runtime\c-r2 /I third_party\wgpu-native\include /I <build> \
  /link wgpu_native.dll.lib windowscodecs.lib ole32.lib user32.lib gdi32.lib
```

Artefactos: `variants/<id>/build/runtime_r2.exe`, `ui.json`, `ui_data.h`,
`atlas.bin` y los PNG de evidencia. Los logs por ejecución están en `logs/`;
`artifacts/pilot/` contiene la baseline del piloto.

> `emit_ui.py` actual ya serializa no-ASCII como escapes octales (`\303\251`),
> no como `\xNN`; el workaround `tools/split_c_escapes.py` del piloto no hizo
> falta (0 escapes ambiguos). El runner lo detecta y solo lo aplica si aparece
> un `\xNN` ambiguo.

---

## 4. Método de medida

| Medición | Comando | Cómo se resume |
|---|---|---|
| Layout | `--report layout.json --sizes 1280x800,800x600` | 100 iteraciones de `layout_page()` por tamaño dentro del proceso; el informe toma la **mediana de 7 procesos** de `layout_ms_best` y `layout_ms_avg` |
| Render frío | `--render W H out.png` (proceso nuevo) | `wall_ms` medido alrededor de `Popen`; `layout_ms`, `frame_ms`, `gpu`, `readback`, `png` del runtime; mediana de 7 |
| Rueda | `--render W H out.png --wheel X Y DY` | offset/max de las líneas `wheel:`/`scroll:` + posición Y real de la banda de acento en el PNG; 7 repeticiones por escenario |
| Caché de imagen | `--states prefix --size 1280x800` | 4 frames offscreen en **un solo proceso**: frame 1 frío (WIC decode + upload), frames 2–4 calientes; 7 procesos |
| Memoria | muestreo de `GetProcessMemoryInfo` cada ~4 ms | pico de `WorkingSetSize` y `PeakPagefileUsage` |

Verificación de píxeles (Pillow):

- cada poster tiene una **banda de acento única** en el source `y=196..203`;
- con `object-fit: cover` 180×240 → 180×180, la banda visible cae en
  `card.y + 166..173`;
- se busca el color exacto en el PNG y se comprueba `x` y `y` de la banda;
- se escanean los márgenes fuera del rect del grid y se exige **0 píxeles de
  acento** (clip correcto).

Chequeos de layout:

- `--report`: grid, 300 cards, 300 imágenes y cada rect contra la fórmula CSS;
- sintéticos: `node_count` exacto, root del tamaño del viewport, sin nodos de
  tamaño cero.

### 4.1 Límites del método

- **WARP**: ver §1.
- El árbol UI se emite a 1280x800. El `--report` a 800x600 recalcula cajas, pero
  los glifos vienen pre-shapeados a 1280; no es un reflow de texto completo.
- `frame_ms` **no** incluye `build_geometry()` ni la subida de vértices, aunque
  la etiqueta del runtime diga `frame(build+upload+pass+readback+png)`. El
  cronómetro arranca después de ambas. `layout_ms` se mide aparte.
- `wn_core_wheel()` hace un `layout_page()` completo; `render_offscreen()` ya
  había hecho otro antes. Ese segundo layout **no** entra en `layout_ms` ni en
  `frame_ms`; solo en `wall_ms`.
- `wall_ms` incluye arranque de proceso e init de wgpu/WARP; es ruidoso
  (300–1400 ms) y no debe usarse como coste del motor.
- El frame offscreen incluye `readback(map)` y PNG, que no existen en ventana.
- El render de `nodes-0030` y `nodes-0100` mostró outliers de PNG/WARP; se
  reportan los valores crudos y la mediana. La métrica estable para escalado es
  `--report` (layout), no el frame offscreen.

---

## 5. Resultados

### 5.1 Build / emisión

| Variante | `ui.json` | `ui_data.h` | frontend | emit_ui | cl.exe | exe |
|---|---:|---:|---:|---:|---:|---:|
| cards-030 | 603,128 B | 570,201 B | 125.3 ms | 347.5 ms | 6531.7 ms | 606,208 B |
| cards-100 | 1,939,721 B | 1,082,209 B | 156.7 ms | 528.6 ms | 5326.3 ms | 1,096,704 B |
| cards-300 | 5,755,771 B | 2,543,719 B | 361.9 ms | 1073.9 ms | 5601.9 ms | 2,502,656 B |
| nodes-0030 | 37,657 B | 16,352 B | 18.3 ms | 193.1 ms | 4287.8 ms | 267,264 B |
| nodes-0100 | 130,174 B | 52,181 B | 32.8 ms | 445.1 ms | 7246.2 ms | 307,200 B |
| nodes-0300 | 402,364 B | 161,783 B | 26.0 ms | 240.7 ms | 4323.3 ms | 428,544 B |
| nodes-1000 | 1,347,783 B | 537,402 B | 42.7 ms | 375.6 ms | 4308.7 ms | 846,336 B |

El coste de compilación lo domina MSVC, no el número de nodos: el header de
300 cards son 2.5 MB de C y el `.exe` resultante 2.5 MB.

### 5.2 Layout — nodos sintéticos (mediana de 7)

`layout_ms_best` / `layout_ms_avg` según `--report` (100 iteraciones internas
por tamaño y proceso):

| Nodos | 1280x800 best | 1280x800 avg | 800x600 best | 800x600 avg |
|---:|---:|---:|---:|---:|
| 30 | 0.0040 | **0.0044** | 0.0039 | **0.0043** |
| 100 | 0.0139 | **0.0155** | 0.0138 | **0.0159** |
| 300 | 0.0230 | **0.0237** | 0.0222 | **0.0245** |
| 1000 | 0.0633 | **0.0671** | 0.0631 | **0.0693** |

Coste aproximado: **0.07–0.11 µs/nodo**. El layout es prácticamente lineal y
sub-milisegundo incluso con 1000 nodos y 16 niveles.

### 5.3 Layout — cards reales

| Variante | Nodos | 1280x800 avg | 800x600 avg | µs/nodo (1280) |
|---|---:|---:|---:|---:|
| cards-030 | 288 | **0.0253** | 0.0254 | 0.088 |
| cards-100 | 918 | **0.0834** | 0.0917 | 0.091 |
| cards-300 | 2718 | **0.3000** | 0.3476 | 0.110 |

Escala lineal con los nodos (9 nodos por card: card + img + meta + 3 bloques de
texto + 3 nodos de texto, más 18 de chrome). 300 cards siguen por debajo de
0.4 ms.

### 5.4 Render offscreen (mediana de 7; WARP)

1280x800:

| Variante | wall | frame | gpu | readback | png | vértices | glyph | image | WS | pagefile |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| cards-030 | 493.0 | **190.0** | 35.8 | 115.6 | 41.0 | 300 | 4164 | 90 | 74.6 | 360.2 |
| cards-100 | 496.1 | **191.6** | 36.3 | 117.6 | 43.6 | 720 | 4170 | 90 | 75.7 | 359.7 |
| cards-300 | 477.6 | **193.0** | 34.6 | 116.9 | 38.3 | 1920 | 4170 | 90 | 77.9 | 361.6 |
| nodes-1000 | 325.5 | **71.1** | 1.7 | 35.8 | 33.3 | 5826 | 0 | 0 | 63.2 | 349.2 |

Los tiempos están en ms; WS/pagefile en **MiB** (1024²). 800x600:

| Variante | wall | frame | gpu | readback | png | vértices | glyph | image | WS | pagefile |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| cards-030 | 421.0 | **126.8** | 25.7 | 79.9 | 19.2 | 264 | 2688 | 54 | 64.3 | 354.0 |
| cards-100 | 420.7 | **129.4** | 24.7 | 83.7 | 20.1 | 684 | 2694 | 54 | 65.8 | 355.1 |
| cards-300 | 402.6 | **130.7** | 24.2 | 83.7 | 19.5 | 1884 | 2694 | 54 | 67.8 | 356.4 |
| nodes-1000 | 324.9 | **62.5** | 1.9 | 43.0 | 17.6 | 5826 | 0 | 0 | 57.2 | 347.2 |

Lectura clave a 1280x800: el frame de **30 cards (190.0 ms)** y el de **300
cards (193.0 ms)** es prácticamente el mismo (**x1.02**). El motivo es que el
runtime cull-ea imágenes y glifos por viewport:

- `image_vertices` = **90** en 1280x800 para 30 y para 300 cards (54 a 800x600);
- `glyph_vertices` ≈ **4170** para 30, 100 y 300 cards (2694 a 800x600);
- `vertices` sólidos **sí crecen**: 300 → 720 → 1920 (+6 por card).

Reparto del frame de cards-300 a 1280x800: **gpu 17.9% + readback 60.5% + png
19.8%**. A 800x600: 18.5% + 64.1% + 14.9%. El readback es el mayor coste y es
un artefacto del modo offscreen.

Los sintéticos no tienen imágenes ni texto; su frame (68–111 ms) es casi todo
readback + PNG y no escala con los nodos (1000 nodos = 71.1 ms, similar a 100
nodos = 69.7 ms). El frame de `nodes-0030` (110.6 ms) es un outlier de PNG
(rango crudo 59–137 ms); la mediana de layout sigue siendo 0.0044 ms.

### 5.5 Rueda: shift, clamp y clip (cards-300)

Siete repeticiones por escenario; el offset se lee del runtime y la posición de
la banda de acento se mide en el PNG. La banda esperada es
`card.y + 166..173`.

1280x800, `scroll_max = 14592`:

| Escenario | delta | offset esperado | offsets observados | banda Y observada | frame | wall |
|---|---:|---:|---|---:|---:|---:|
| shift | 100 | 100 | 100 ×7 | 90–97 | 226.3 | 546.7 |
| offset400 | 400 | 400 | 400 ×7 | 302–309 | 211.2 | 514.5 |
| clamp | 999999 | **14592** | 14592 ×7 | 702–709 | 186.8 | 477.1 |
| outside (sidebar) | 400 | 0 | 0 ×7 | 190–197 | 192.6 | 478.9 |

800x600, `scroll_max = 25032`:

| Escenario | delta | offset esperado | offsets observados | banda Y observada | frame | wall |
|---|---:|---:|---|---:|---:|---:|
| shift | 100 | 100 | 100 ×7 | 90–97 | 133.7 | 431.5 |
| offset400 | 400 | 400 | 400 ×7 | 302–309 | 131.0 | 429.8 |
| clamp | 999999 | **25032** | 25032 ×7 | 502–509 | 134.2 | 459.3 |
| outside (sidebar) | 400 | 0 | 0 ×7 | 190–197 | 134.3 | 436.7 |

En el clamp, la última fila queda pegada al borde inferior del grid
(`last_row.y + 240 == grid.y + grid.h`): 536+240=776 a 1280x800 y 336+240=576 a
800x600. La banda medida 702–709 (1280) y 502–509 (800) lo confirma.

El wheel que cambia el offset hace **dos layouts completos** por frame (uno en
`render_offscreen` y otro dentro de `wn_core_wheel`), pero ambos son ~0.3 ms a
300 cards; el coste grande del frame de rueda sigue siendo gpu+readback+png. El
problema es de **medición**: el segundo layout no está en `layout_ms` ni
`frame_ms`.

### 5.6 Imágenes: primer render vs warm, 30 vs 300

`--states` renderiza 4 frames en un proceso. El frame 1 decodifica con WIC las
imágenes visibles únicas; los frames 2–4 reutilizan la caché por path.

| Variante | Imágenes (nodos) | Posters únicos | Frío | Caliente | Diferencia |
|---|---:|---:|---:|---:|---:|
| cards-030 | 30 | 30 | 188.8 | 93.4 | **+95.5** |
| cards-100 | 100 | 30 | 184.0 | 93.6 | **+90.4** |
| cards-300 | 300 | 30 | 184.1 | 91.7 | **+92.5** |
| piloto 30 | 30 | 30 | 175.2 | 88.9 | +86.3 |

**300 imágenes no cuestan más que 30 en el primer frame** en este fixture:
- solo se decodifican las imágenes que sobreviven al culling del viewport
  (~15 únicas a 1280x800, ~9 a 800x600);
- los 300 nodos reutilizan los mismos 30 archivos, así que la caché por path
  las deduplica.

El coste de decodificar+subir las ~15 imágenes visibles es **~90–95 ms** en WARP
(medido como frame frío − frame caliente). Es el segundo coste tras el
readback. Si cada card tuviera un poster distinto, el coste de decodificar las
visibles sería el mismo, pero la caché crecería con el catálogo (ver §6).

### 5.7 Memoria

WS = `WorkingSetSize` pico; pagefile = `PeakPagefileUsage`. Unidades **MiB**
(1024²), no MB decimales.

| Variante | Viewport | WS pico | Pagefile pico |
|---|---|---:|---:|
| cards-030 | 1280x800 | 74.6 MiB | 360.2 MiB |
| cards-100 | 1280x800 | 75.7 MiB | 359.7 MiB |
| cards-300 | 1280x800 | **77.9 MiB** | **361.6 MiB** |
| nodes-1000 | 1280x800 | 63.2 MiB | 349.2 MiB |
| cards-030 | 800x600 | 64.3 MiB | 354.0 MiB |
| cards-300 | 800x600 | 67.8 MiB | 356.4 MiB |
| nodes-1000 | 800x600 | 57.2 MiB | 347.2 MiB |

De 30 a 300 cards el WS sube ~3.3 MiB y el pagefile ~1.4 MiB. El pagefile está
dominado por WARP (~350 MiB incluso sin imágenes). La caché de 30 posters
180x240 RGBA son ~5 MiB (169 KiB por textura), dentro de esos 3–5 MiB de
diferencia.

### 5.8 Comparación con el piloto de 30 cards

El piloto se midió con el `runtime_r2_pilot.exe` ya verificado (48/48) que está
en `benchmarks/pilot-media-library/build/`; el runner no lo recompila. El piloto
usa cards 180x320 (poster 180x240 + meta 80) y más chrome; el stress usa cards
180x240 (poster 180x180 + meta 60). La comparación es de orden de magnitud, no
de píxeles idénticos.

| Métrica (1280x800) | Piloto 30 | Stress 30 | Stress 300 |
|---|---:|---:|---:|
| Nodos UI | 301 | 288 | 2718 |
| Layout avg | 0.025 ms | 0.025 ms | 0.300 ms |
| Frame offscreen | 169.4 ms | 190.0 ms | 193.0 ms |
| gpu | 32.7 ms | 35.8 ms | 34.6 ms |
| readback | 101.9 ms | 115.6 ms | 116.9 ms |
| png | 35.2 ms | 41.0 ms | 38.3 ms |
| Vértices sólidos | 324 | 300 | 1920 |
| Glyph vertices | 4482 | 4164 | 4170 |
| Image vertices | 90 | 90 | 90 |
| WS / pagefile | 74.0 / 358.0 MiB | 74.6 / 360.2 MiB | 77.9 / 361.6 MiB |
| Frío / caliente (states) | 175.2 / 88.9 ms | 188.8 / 93.4 ms | 184.1 / 91.7 ms |
| `scroll_max` 800x600 | 2792 | 1992 | 25032 |

El stress de 300 cards mantiene el frame en ~193 ms (+14% sobre el piloto,
dentro de la variación de WARP) y multiplica el layout por 12. La diferencia de
frame no viene del layout ni de las imágenes (cull-eadas), sino de más
geometría sólida y del coste fijo de WARP/readback/PNG.

---

## 6. Cuellos de botella (con evidencia)

1. **El layout no es el cuello de botella.** 2718 nodos = 0.300 ms; 1000 nodos
   sintéticos = 0.067 ms; <0.2% del frame offscreen. No hace falta optimizar
   `layout_page()` para 300–1000 cards.
2. **La geometría sólida no se culla.** `vertices` crece +6 por card:
   300 → 720 → 1920. Se emiten los 300 fondos de card aunque estén muy por
   debajo del clip. Imágenes y glifos sí se cullan (`image_vertices=90`,
   `glyph_vertices≈4170` constantes).
3. **El wheel dispara un re-layout completo y no se contabiliza.** Dos
   `layout_page()` por frame de rueda; el segundo queda fuera de `layout_ms` y
   `frame_ms`. A 300 cards son ~0.3 ms, pero el patrón no escala a catálogos
   grandes y falsea las métricas.
4. **El frame offscreen está dominado por readback + PNG.** 60.5% + 19.8% del
   frame a 1280x800; solo 17.9% es encode/submit (y dentro de ese `gpu` está la
   decodificación WIC del primer frame). No es representativo de una ventana.
5. **Primer frame paga la decodificación WIC.** +90–95 ms frío vs caliente. Con
   posters únicos, la caché por path no tiene eviction: 180x240 RGBA = 169 KiB;
   1000 posters únicos ≈ 165 MiB de texturas.
6. **Sin virtualización.** Se hace layout y geometría sólida de todas las
   cards; solo se recorta el dibujo de imágenes/glifos. 300 cards = 2718 nodos;
   1000 cards ≈ 9000 nodos y ~6000 vértices sólidos.
7. **Una bind group/draw por imagen** (las batches solo se fusionan si el
   `src` y el clip coinciden consecutivamente). Con posters ciclados hay ~15
   draws visibles; con posters únicos se acercaría a uno por imagen visible.
8. **WARP.** Todo lo anterior está medido sobre CPU; el readback y el rasterizado
   software exageran el coste. Hace falta una pasada en GPU hardware.

---

## 7. Recomendaciones (priorizadas)

1. **Scroll offset-only**: no re-layout en `wn_core_wheel`; trasladar rects o
   ajustar el scissor, y contabilizarlo en el frame. Reservar `layout_page()`
   para resize.
2. **Cull de geometría sólida**: aplicar el clip del scroll viewport (o una
   ventana de filas) a los rects sólidos en `wn_core_build_display_list`, no
   solo a imágenes/glifos.
3. **Virtualización de filas** para 500–1000+ items: mantener visibles +
   overscan y reciclar nodos; así el layout y la geometría sólida quedan
   acotados por el viewport.
4. **Caché de imágenes por bytes con LRU/refcount**, decodificar al tamaño de
   display y considerar atlas/array + instancing para reducir bind groups/draws.
5. **Instrumentación**: exponer en `--render`/`--report` el tiempo de
   `build_geometry`, el número de batches/draws y el tiempo del layout de rueda.
   Corregir la etiqueta `frame(build+upload+...)`, que no incluye build/upload.
6. **Modo warm offscreen** (`--frames N`) para medir caché y frame sin
   `--states` ni ventana, y separar readback/PNG del coste del motor.
7. **Repetir en GPU hardware** antes de tomar decisiones de rendimiento; WARP
   es un techo pesimista y el readback tapa la señal.

---

## 8. Acceptance y evidencia

La ejecución final (`--runs 7`) terminó con:

```text
acceptance: 94/94 checks, 0 failure(s)
```

Cobertura:

- 3 variantes de cards × (node_count + layout report + 2 layouts runs + 2
  render runs + 2 PNG checks + 8 escenarios de rueda + states runs) = 51;
- 4 variantes sintéticas × (node_count + layout report + 2 layout runs + 2
  render runs + 2 PNG checks) = 32;
- baseline del piloto: 2 layouts + 2 renders + 2 PNG + states + 4 ruedas = 11;
- total 94.

Chequeos de layout: las 300 cards y 300 imágenes coinciden con la fórmula CSS
en ambos viewports; los sintéticos reportan exactamente 30/100/300/1000 nodos,
sin nodos de tamaño cero.

Chequeos de rueda: 7/7 offsets exactos y bandas de acento en la posición
esperada para shift 100, offset 400, clamp y rueda fuera del grid, a 1280x800 y
800x600, en cards-030/100/300.

PNG de evidencia:

- `variants/cards-300/build/render_1280x800.png` y `render_800x600.png`;
- `variants/cards-300/build/wheel_{shift,offset400,clamp,outside}_*.png`;
- `variants/nodes-1000/build/render_*.png`;
- baseline del piloto en `artifacts/pilot/`.

`stress_report.json` contiene, por variante: tiempos de emisión/compilación,
los 7 valores crudos de layout/render, mediana/min/max/σ, vértices, memoria,
estados frío/caliente, offsets de rueda, bandas, chequeos y errores. Un fallo
de emisión/compilación/medida se registra con el comando exacto y el stderr en
`logs/` y hace que el script salga con código 1 sin inventar números.

---

## 9. Mapa de archivos

```
benchmarks/pilot-media-library/stress/
  generate.py          genera variants/<id>/page y manifest.json
  run_stress.py        emite, compila, mide, valida y escribe stress_report.json
  run_stress.ps1       wrapper PowerShell de run_stress.py
  STRESS.md            este informe
  stress_report.json   números crudos + método + benchmarks + recomendaciones
  variants/            páginas y builds generados (gitignored)
  logs/                stdout/stderr por proceso (gitignored)
  artifacts/pilot/     baseline del piloto (gitignored)
```

No se ha modificado ningún archivo fuera de `stress/`. El runtime, frontend,
emisores, `build.ps1`, el piloto de 30 cards y el proyecto Desktop quedan
intactos.
