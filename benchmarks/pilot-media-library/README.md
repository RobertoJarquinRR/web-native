# Piloto - biblioteca de video (fixtures + verificador visual)

Este directorio contiene el **fixture visual A0** del piloto A de webnative
(`docs/pilot-media-library.md`): una biblioteca/catálogo de video local, sin
playback, construida con el contrato del **UI tree v2** (`docs/ui-tree-v2.md`)
sobre el subset v1 (`docs/subset-v1.md`).

El objetivo es fijar la estética, la matemática del grid, el clip de scroll y
los píxeles de los posters. `verify.py` comprueba el fixture (catálogo, posters,
HTML, CSS y `expected.json`) y, cuando hay un runtime piloto, ejecuta los
renders offscreen, el layout report y las pruebas de rueda/clamp.

El engine v2 ya está implementado en el working tree de esta sesión (grid,
imágenes locales y scroll con clip) y este fixture se validó contra él:
**PASS 48/48**. En un checkout sin runtime piloto, `verify.py` termina con
`ENGINE FEATURE MISSING` y explica exactamente qué construir.

---

## 1. Qué hay aquí

```
benchmarks/pilot-media-library/
  catalog.json          30 items con id, título, ruta, duración, resolución,
                        tags, poster y favorite (forma A1)
  expected.json         grid, columnas, posiciones de las 30 cards, alto de
                        contenido y clamp de scroll a 1280x800 y 800x600
  make_assets.py        genera posters/ desde catalog.json (Pillow)
  posters/              30 PNG 180x240 + manifest.json
  page/index.html       30 cards estáticas, sidebar, búsqueda, panel de detalle
  page/style.css        contrato CSS del grid/scroll/imagen (extensiones marcadas)
  verify.py             verificador de fixtures + runtime offscreen
  tools/split_c_escapes.py  workaround temporal del emisor (ver §5)
  build/                ui.json/ui_data.h, runtime piloto y salida del verificador
                        (gitignored; build/verify/ contiene el evidence)
  .venv/                Pillow local (gitignored)
```

Todo vive bajo este directorio. El piloto **no modifica** runtime, frontend,
emisores, `build.ps1` ni docs.

### Datos

`catalog.json` es la fuente de verdad de los items. Las rutas `path` apuntan a
una biblioteca local hipotética (`C:/videos/webnative-library/...`); en A0 solo
se distribuyen los posters. `poster` es relativo a la raíz del piloto
(`posters/vid-001.png`), así que `page/index.html` referencia
`../posters/vid-001.png`.

El catálogo y la página están sincronizados: `verify.py` comprueba que cada
`id`, título y `src` del HTML aparece exactamente una vez y que cada poster
existe con el tamaño y los colores esperados.

---

## 2. Generar los assets

Los posters son placeholders **generados**: color base distinto por item,
patrón geométrico (barras, puntos, diagonales, damero o marcos), número grande
(`01`..`30`), banda fina y pie en color de acento. No hay imágenes externas ni
material con derechos.

Requisito: Pillow. En este repo se usa un venv local para no tocar el Python
global:

```powershell
python -m venv benchmarks\pilot-media-library\.venv
benchmarks\pilot-media-library\.venv\Scripts\python -m pip install Pillow
benchmarks\pilot-media-library\.venv\Scripts\python benchmarks\pilot-media-library\make_assets.py
```

Salida:

- `posters/vid-001.png` .. `posters/vid-030.png` (180x240, RGB);
- `posters/manifest.json` con SHA-256, colores y puntos de muestreo.

El generador es idempotente y borra PNGs `vid-*.png` que ya no estén en el
catálogo. Cada poster reserva una zona lisa para el verificador:

| Punto | Coordenada | Uso |
|---|---|---|
| `bg` | `(4,4)` | color base del poster |
| `rule` | `(4,200)` | banda fina de acento (196..204) |
| `footer` | `(4,232)` | pie de acento (224..240) |

El acento es único por poster. Eso permite localizar una card concreta por
color y medir el desplazamiento del scroll sin OCR ni comparación difusa.

---

## 3. Contrato de layout (la matemática de `expected.json`)

La página está construida para que el grid se pueda calcular a mano desde el
CSS, sin `calc()`, `fr`, `minmax` ni `auto-fit` (fuera del subset/v2):

- sidebar `20%`, main `80%`, padding del main `24px`;
- grid `width:100%`, `height:100%` del content box del main;
- `grid-template-columns: repeat(auto-fill, 180px)`;
- `grid-auto-rows: 320px`, `gap: 16px`;
- card `180x320`; poster `180x240`; metadata `180x80`.

Fórmulas (W = ancho de ventana, H = alto):

```
sidebar_w   = 0.20 * W
main_w      = 0.80 * W
grid_x      = sidebar_w + 24
grid_y      = 24
content_w   = main_w - 48
grid_h      = H - 48
columns     = max(1, floor((content_w + 16) / (180 + 16)))
rows        = ceil(30 / columns)
content_h   = rows * 320 + (rows - 1) * 16
scroll_max  = max(0, content_h - grid_h)
card(i).x   = grid_x + (i % columns) * 196
card(i).y   = grid_y + floor(i / columns) * 336
```

Resultados congelados en `expected.json`:

| Viewport | Sidebar | Main | Grid | Columnas | Filas | Alto contenido | Clamp |
|---|---:|---:|---|---:|---:|---:|---:|
| **1280x800** | 256 | 1024 | x=280, y=24, 976x752 | **5** | 6 | 2000 | **1248** |
| **800x600** | 160 | 640 | x=184, y=24, 592x552 | **3** | 10 | 3344 | **2792** |

En 1280x800: 5 columnas x 6 filas = 30 cards exactas.
En 800x600: 3 columnas x 10 filas = 30 cards exactas.

`expected.json` incluye además las 30 posiciones de card, imagen y metadata de
cada viewport, y tres sondas de scroll:

| Sonda | Delta | Qué comprueba |
|---|---:|---|
| `shift` | 100 px | una card visible se desplaza exactamente 100 px |
| `offset` | 400 px | una card de la fila 3 aparece en la posición esperada |
| `clamp` | 999999 | el offset efectivo es `scroll_max` y la última fila queda pegada al borde inferior |

Las bandas de acento esperadas están en `card.y + 196..204` y
`card.y + 224..240`; el verificador las localiza en la columna `card.x + 5`.

`verify.py` **recalcula** toda esta matemática desde `style.css` (no confía en
`expected.json`) y falla si el CSS y el JSON se separan.

---

## 4. Verificador

```powershell
# 1. Solo fixtures (no necesita runtime)
benchmarks\pilot-media-library\.venv\Scripts\python.exe benchmarks\pilot-media-library\verify.py --fixtures-only

# 2. Verificación completa; usa build/runtime_r2_pilot.exe si existe y, si no,
#    cae al build R2 compartido para diagnosticar.
benchmarks\pilot-media-library\.venv\Scripts\python.exe benchmarks\pilot-media-library\verify.py

# 3. Runtime piloto explícito (el que produjo este PASS)
benchmarks\pilot-media-library\.venv\Scripts\python.exe benchmarks\pilot-media-library\verify.py --runtime benchmarks\pilot-media-library\build\runtime_r2_pilot.exe
```

Resultado de la última ejecución real en esta sesión:

```
[ok  ] runtime/report: 1280x800 grid rect - x=280.0 y=24.0 w=976.0 h=752.0
[ok  ] runtime/report: 1280x800 card positions - all 30 cards match
[ok  ] raster/1280x800: poster source colours at sample points - 35 visible samples match
[ok  ] raster/1280x800: no poster pixels outside grid clip - 0 stray pixels
[ok  ] raster/1280x800: scroll shift - measured shift 100.0px
[ok  ] raster/1280x800: scroll clamp - runs [[652.0, 660.0], [680.0, 696.0]]
[ok  ] raster/800x600: no poster pixels outside grid clip - 0 stray pixels
[ok  ] raster/800x600: scroll clamp - runs [[452.0, 460.0], [480.0, 496.0]]
RESULT: PASS (48 checks, 0 failures)
```

Exit codes:

| Código | Significado |
|---:|---|
| `0` | PASS (todos los checks) |
| `1` | FAIL (fixture inconsistente, layout/píxeles/scroll incorrectos) |
| `2` | ENGINE FEATURE MISSING (runtime ausente o feature v2 sin implementar) |

El verificador escribe `build/verify/verify_report.json` con checks, bloqueos,
notas y el runtime usado. Los PNGs intermedios (`*_scroll0.png`,
`*_scroll_*.png`, `probe_scroll_*.png`) quedan en `build/verify/`. El runtime se
ejecuta con `cwd=page/` por defecto para que `src="../posters/..."` resuelva
respecto al HTML; `--runtime-cwd` permite otro directorio.

### Qué comprueba la capa de fixtures (siempre)

- 30 items, campos obligatorios, ids únicos, tipos y rangos.
- 30 posters existentes, 180x240, muestras `bg`/`rule`/`footer`, acentos únicos
  y coincidencia con `posters/manifest.json` (SHA-256 incluido).
- HTML: 30 `.poster-card`, 30 `img.poster`, cada `src` y título del catálogo,
  sidebar/búsqueda/detalle, sin `<script>`, `<input>`, `<video>`, etc.
- CSS: constantes del contrato y fórmula auto-fill; posiciones de cards,
  contenido y clamp recalculados para ambos viewports.
- Sondas de scroll: posiciones y bandas esperadas recalculadas.

### Qué comprueba la capa de runtime

1. `--report` a 1280x800 y 800x600: nodo `div.poster-grid` con `kind=grid`,
   rect del grid, 30 cards y 30 imágenes `img.poster` con `kind=image` y sus
   rects exactos; columnas de la primera fila.
2. `--render W H out.png`: tamaño del PNG y muestras de color de los posters en
   las posiciones esperadas (detecta grid mal calculado o imágenes ausentes).
3. `gap` de 16 px igual al fondo de página; píxeles de acento fuera del rect del
   grid = 0 (clip); una card cuyas bandas caen por debajo del clip no pinta nada.
4. Scroll: `--scroll <grid_node_id> <delta>` (si existe) o `--wheel`; se prueban
   las dos formas de rueda (`--wheel x y dy` del R2 actual y
   `--wheel x y dx dy` del ABI). Se mide el desplazamiento real de las bandas de
   acento para 100 px, 400 px y el clamp a `scroll_max`, y se repite el chequeo
   de clip en cada frame desplazado.

---

## 5. Estado verificado y bloqueos (2026-11-23)

### Verificado

El working tree de esta sesión ya contiene la implementación v2 (frontend Rust,
`emit_ui.py` y runtime R2). Con un runtime piloto compilado localmente desde
`page/index.html`:

- frontend: 301 nodos, 1352 glyphs, 101 runs, schema `webnative.ui-tree/2`;
- `--report`: grid 280,24 976x752 a 1280x800 y 184,24 592x552 a 800x600;
  30 cards y 30 imágenes con rects exactos;
- `--render`: tamaño, colores de posters, gap, clip y scroll (100 px, 400 px,
  clamp 1248/2792) correctos;
- **PASS 48/48**. Evidencia en `build/verify/verify_report.json` y PNGs en
  `build/verify/` (gitignored).
- `--fixtures-only`: **14 checks** y PASS sin runtime.

El build compartido `src/runtime/c-r2/build/runtime_r2.exe` sigue siendo el
fixture de 28 nodos. `verify.py` usa primero `build/runtime_r2_pilot.exe`; si no
existe, cae al build compartido solo para dar un diagnóstico claro de qué falta.

### Build local del runtime piloto

El build se hizo **sin modificar** frontend, emisor, runtime ni `build.ps1`; los
artefactos quedaron bajo `build/`:

```powershell
# 1. arbol ui-tree/2 desde la pagina
src\frontend\rust\target\release\frontend-rust.exe --html benchmarks\pilot-media-library\page\index.html --css benchmarks\pilot-media-library\page\style.css --width 1280 --height 800 --emit-ui benchmarks\pilot-media-library\build\ui.json --pilot

# 2. ui_data.h (imagenes resueltas contra build\..\posters)
src\frontend\python\.venv\Scripts\python.exe src\emitter\python\emit_ui.py --ui-json benchmarks\pilot-media-library\build\ui.json --out benchmarks\pilot-media-library\build\ui_data.h --meta benchmarks\pilot-media-library\build\ui_meta.json

# 3. workaround temporal del bloqueo de escapes C (ver abajo)
benchmarks\pilot-media-library\.venv\Scripts\python.exe benchmarks\pilot-media-library\tools\split_c_escapes.py --in benchmarks\pilot-media-library\build\ui_data.h

# 4. compilar runtime_r2.c + wn_core.c + wn_common.c con /DWN_STATIC,
#    incluyendo include/, src/runtime/c-r2, third_party/wgpu-native/include y build/,
#    enlazando wgpu_native.dll.lib + windowscodecs/ole32/user32/gdi32
#    -> build/runtime_r2_pilot.exe; copiar third_party/wgpu-native/bin/wgpu_native.dll

# 5. verificar
benchmarks\pilot-media-library\.venv\Scripts\python.exe benchmarks\pilot-media-library\verify.py --runtime benchmarks\pilot-media-library\build\runtime_r2_pilot.exe
```

El patrón exacto de compilación (mismos flags y librerías) está en
`src/runtime/c-r2/tests/pilot-media-library/run_pilot.ps1`; solo cambia el
`ui_data.h` de entrada. El `build.ps1` actual apunta al fixture de 28 nodos; el
stream de engine debería añadir un parámetro de fixture (o documentar este
build) para que el piloto no dependa de comandos manuales.

### Bloqueos y pendientes

| Tema | Estado |
|---|---|
| grid + imágenes + scroll/clip v2 | **implementado y verificado** en el working tree; ya no es blocker |
| emisor `ui_data.h`: escapes C ambiguos | **abierto**: `emit_ui.py` escribe bytes no ASCII como `\xNN`; si el byte siguiente es un dígito hex, C lo absorbe (`"\xd72160"`). Afecta a `3840×2160`, `Geometría`, `Órbita`, `tecnología`, `montaña`, `geografía`, `astronomía`. `tools/split_c_escapes.py` inserta `""` como workaround; el fix real es del emisor |
| `build.ps1` con fixture piloto | **abierto**: el build fue manual; no hay flag de fixture |
| CLI `--scroll` | opcional; el R2 actual expone `--wheel <x> <y> <dy>` y el verificador acepta también la forma ABI de 4 valores |
| A1 | input/IME, texto/lista dinámica, virtualización, catálogo/favoritos/abrir en el host |

No se ha tocado ningún archivo fuera de este directorio; el runtime piloto se
compiló contra las fuentes del working tree y todos los artefactos quedaron en
`benchmarks/pilot-media-library/build/`.

### Extensiones del piloto marcadas en el código

En `page/style.css` y `page/index.html` los comentarios `[pilot-extension: ...]`
marcan lo que no es subset v1:

- `display:grid`, `grid-template-columns`, `grid-auto-rows` (grid v2);
- `overflow:auto` con scroll vertical (scroll v2);
- `<img>` local + `object-fit:cover` (image v2);
- búsqueda como `div` estilado (el input/IME real es A1);
- panel de detalle como placeholder (el host A1 lo rellena).

Todo lo demás usa tags y propiedades de `docs/subset-v1.md`.

---

## 6. A0 vs A1

La distinción es la de `docs/pilot-media-library.md`:

- **A0 (este fixture)**: catálogo estático de 30 items, cards hardcodeadas en el
  HTML, posters y metadata precalculados. Sirve para validar estética, grid,
  scroll, clipping y el flujo de detalle **sin** datos dinámicos. El engine ya
  ofrece grid/scroll/imágenes y este fixture las ejercita de punta a punta.
- **A1 (app real)**: el host carga `catalog.json` (o escanea carpeta en A2),
  filtra/busca, mantiene favoritos, selecciona, muestra detalle y abre con el
  reproductor del sistema. Requiere lista dinámica/template, texto dinámico,
  input/IME, truncado y virtualización para 500-1000 items.

Este directorio usa **datos con forma A1** (el `catalog.json` que consumirá el
host) pero renderiza una **vista A0** estática. La intención es que el mismo
catálogo y la misma geometría se reutilicen cuando A1 conecte el host, sin
rehacer el diseño.

No hay playback: el botón "Abrir con el reproductor del sistema" y el estado de
favoritos son responsabilidad del host A1.
