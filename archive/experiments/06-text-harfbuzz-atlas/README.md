# exp06 — Primer pipeline de texto nativo (HarfBuzz + FreeType + atlas GPU)

**STATUS: SUCCESS.** HTML + CSS → layout de línea → shaping con HarfBuzz → rasterizado con
FreeType → atlas R8 → display list → render nativo con **wgpu** (D3D12). Sin navegador,
WebView, Skia ni Electron. Verificación de píxeles y de IR: **PASS**.

## Cómo ejecutar

```powershell
py -3.12 -m venv .venv
.venv\Scripts\python.exe -m pip install -r requirements.txt
.venv\Scripts\python.exe run.py
```

Salidas: `display_list.json`, `shaped.json`, `render.png` (400×200) y `title_crop.png`
(recorte 2× para inspección visual). Código: `run.py`, 500 líneas exactas.

## Diagrama del pipeline

```
input.html + input.css
        │  html5lib · tinycss2 · cascade (tag/id/clase, especificidad, herencia de fuente)
        ▼
layout de bloques + line boxes
        │  ascender/descender de FreeType · sin wrapping · una línea por run
        │  title: baseline 55 (20 + asc 35) · body: baseline 94 (margen 8)
        ▼
shaping (uharfbuzz)
        │  hb.Face(bytes) → hb.Font · scale = size·64 · LTR / Latn
        │  gids + avances (26.6) + clusters → shaped.json
        ▼
rasterizado (freetype-py)
        │  un bitmap grayscale por (gid, tamaño) · 12 bitmaps únicos
        ▼
atlas R8 512×512
        │  shelf packing · padding 1 px · dedupe · UVs normalizadas
        ▼
display_list.json
        │  2 rects de fondo + 2 ops text_run con gids, quads y UVs
        ▼
wgpu offscreen 400×200
        │  pipeline de rects + pipeline de glifos (textura R8 × color por vértice)
        │  alpha blending · readback · PNG con Pillow
        ▼
render.png + verificación PASS/FAIL
```

## Versiones y entorno

| Componente | Versión |
|---|---|
| Python | 3.12.10 |
| html5lib | 1.1 |
| tinycss2 | 1.5.1 |
| uharfbuzz | 0.57.0 |
| freetype-py | 2.5.1 (FreeType 2.13.2) |
| wgpu (wgpu-py) | 0.33.0 |
| Pillow | 13.0.0 |
| Windows | 10.0.19045 |
| Adaptador wgpu | Microsoft Basic Render Driver (WARP), D3D12, tipo CPU, vendor 5140 |

No se enumeró ninguna GPU de hardware: el render corre sobre **WARP** (rasterizador software
expuesto por D3D12). El camino de código es el mismo que usaría Vulkan o Metal.

## Fuente

- Usada: **`C:\Windows\Fonts\segoeui.ttf`** — existe, 955.804 bytes, `upem = 2048`.
- Fallback implementado: `C:\Windows\Fonts\arial.ttf` (no fue necesario; se reporta si se usa).
- Familia CSS resuelta: `"Segoe UI"`. Solo se carga una fuente; no hay cadena de fallback por glifo.

## Layout y métricas reales

`#root` mide 400×200 en (0,0) con padding 20 (modelo border-box, igual que exp04), así que el
content box empieza en (20,20).

| Run | Tamaño | Ascender | Descender | Altura de línea | Baseline | Avance total |
|---|---|---|---|---|---|---|
| `Hola` (`.title`) | 32 px | 35 px | 9 px | 44 px | 20 + 35 = **55** | 65,50 px |
| `webnative` (`.body`) | 20 px | 22 px | 6 px | 28 px | 55+9+8+22 = **94** | 89,81 px |

El body arranca en y=72 (línea del título termina en 64 + `margin-top:8`), por lo que sus
glifos quedan dentro de la región de verificación y≈70..100.

## Shaping (uharfbuzz)

- `hb_scale = font_size · 64`: **2048** para 32 px y **1280** para 20 px.
- Dirección `ltr`, script `Latn`, idioma `en`, sin features extra (defaults de HarfBuzz).
- `Hola`: 4 glifos, gids `[43, 82, 79, 68]`, clusters `[0,1,2,3]`.
- `webnative`: 9 glifos, gids `[90, 72, 69, 81, 68, 87, 76, 89, 72]`, clusters `[0..8]`.
- 13 glifos en el IR; **12 bitmaps únicos** en el atlas (`a` se repite a 20 y 32 px, y además
  aparece en ambos runs con distinto tamaño).
- Los avances son 26.6 fixed-point: se dividen entre 64 para obtener píxeles. Para Segoe UI,
  `upem = 2048`; a 32 px la escala coincide numéricamente con el upem, lo que puede confundir
  al depurar (a 20 px se ve claro: 1280).

## Atlas y GPU

- Atlas **R8 512×512**, shelf packing, 1 px de padding transparente por glifo, UVs normalizadas.
- Bitmaps FreeType en escala de grises con hinting normal (`FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL`).
- `text_run` guarda por glifo: `gid`, quad absoluto `x,y,w,h`, UVs `u0,v0,u1,v1` y `adv`.
- Render offscreen `rgba8unorm` 400×200; clear `#0f0f12`; 2 rects y 2 draw calls (rects, glifos).
- El pipeline de glifos muestrea la R8 y usa `.r` como alpha, multiplicando por color por vértice.
  Blending: `src_alpha` / `one_minus_src_alpha` (alpha independiente: `one` / `one_minus_src_alpha`).
- Readback con filas alineadas a 256 B (400·4 = 1600 → 1792 B por fila) y reconstrucción del PNG.

## Verificación (salida real de `run.py`)

```
[verify] png size 400x200: (400, 200) -> OK
[verify] title lit px >40 (>=100): 572 px (>56: 553) -> OK
[verify] body blue px (>=50): 343 px -> OK
[verify] shaped glyphs per run > 0: [4, 9] -> OK
[verify] text_run ops == 2: 2 -> OK
PASS
```

- Región del título (y 20..60): 572 píxeles con luminancia > 40 y 553 con luminancia > fondo+40
  (el fondo `#101014` tiene luminancia ≈ 16,3). Muy por encima del mínimo de 100.
- Región del body (y 70..100): 343 píxeles más cercanos a `#3498db` que al fondo (mínimo 50).
- Inspección de píxeles: núcleo blanco exacto `#ffffff` y azul exacto `#3498db`; sin filas
  invertidas ni padding visible.
- `title_crop.png` (800×100, 2× nearest) muestra “Hola” nítido en blanco.

## Tiempos (una corrida en frío, sin profiler)

| Etapa | Tiempo |
|---|---|
| html5lib + tinycss2 + cascade | 16,6 ms |
| layout (bloques + line boxes) | 0,6 ms |
| **shaping (HarfBuzz)** | **0,5 ms** |
| **atlas (raster FreeType + packing)** | **2,8 ms** |
| ensamblado del IR | 0,3 ms |
| **render wgpu + readback + PNG** | **2574,7 ms** |
| verificación + crop | 79,3 ms |
| **TOTAL** | **2680,1 ms** |

El render está dominado por la **inicialización de una sola vez** de wgpu-native/cffi y la
compilación de los dos pipelines (el mismo orden de magnitud que exp04: ~2,1 s de arranque,
~60 ms por frame posterior sobre WARP). La primera llamada a HarfBuzz también pagó creación de
`hb.Font` en algunas corridas (~16 ms); en régimen estable el shaping de estos 13 glifos es
sub-milisegundo. No se midió un segundo frame en este experimento.

## Limitaciones (a propósito)

- **Solo LTR Latin**: sin bidi, sin scripts complejos (árabe, devanagari, Thai), sin shaping
  contextual más allá de lo que HarfBuzz aplica por defecto.
- **Sin wrapping**: una línea por run; no hay `text-align`, inline flow, `line-height` CSS ni
  colapso de espacios real (solo se normalizan espacios en blanco).
- **Una sola fuente**: `font-family` se parsea pero se resuelve a Segoe UI; sin fallback por
  glifo ni `@font-face`.
- Tamaños de fuente en píxeles enteros para el raster; sin `devicePixelRatio` ni posicionamiento
  subpíxel (los quads pueden caer en coordenadas fraccionarias y el sampler lineal las suaviza).
- Métricas de línea de FreeType, avances de HarfBuzz: con hinting puede haber pequeñas
  diferencias entre el ancho del bitmap y el avance; el quad usa el avance de HB.
- Sin kerning explícito configurable, sin ligaduras opcionales, sin fuentes variables ni emoji
  de color.
- Atlas fijo de 512×512 sin evicción ni mipmaps; shelf packing simple. Si se llena, falla con
  error explícito.
- Cascada y layout mínimos: selectores tag/id/clase + descendiente, sin `!important`, inline
  styles, pseudo-clases, media queries ni `@rules`. Colores solo `#rgb`/`#rrggbb`.
- Sin clip/scissor, sombras, bordes ni gradientes. Una draw call por pipeline.

## Problemas encontrados

1. **API de wgpu 0.33**: `r8unorm` (no `r8_unorm`), `MapMode.READ`, `write_texture` vive en
   `device.queue`, y `copy_texture_to_buffer` exige `bytes_per_row` múltiplo de 256. Se resolvió
   inspeccionando firmas y enums antes de escribir el renderer.
2. **Escala de HarfBuzz**: hay que llamar `hb.ot_font_set_funcs(font)` y después fijar
   `font.scale = (size*64, size*64)`. Los avances vienen en 26.6 y se dividen entre 64. El caso
   32 px/upem 2048 enmascara errores de escala; 20 px/1280 los evidencia.
3. **Pitch del bitmap de FreeType**: no coincide siempre con `width`; hay que cortar cada fila
   con `pitch` antes de copiarla al atlas. Asumir `width` produce basura desplazada.
4. **Coordenadas del glifo**: el quad se coloca con `x = pen + dx + bitmap_left` y
   `y = baseline - dy - bitmap_top`; el avance del pen sale de HarfBuzz. Es fácil confundir
   `bitmap_top` (positivo hacia arriba) con coordenadas de pantalla (y hacia abajo).
5. **Padding del atlas**: sin 1 px transparente, el filtrado lineal sangra glifos vecinos. Con
   padding y UVs exactas el resultado es limpio; aun así el posicionamiento fraccionario produce
   un ligero resampleo (aceptable a 20/32 px).
6. **Ruido del backend GL**: wgpu-native emite `Returned GL context is 1.1, when 3.3+ is needed`
   al inicializar; es inofensivo porque el adaptador elegido es D3D12. No se silencia.
7. **Solo WARP**: sin GPU de hardware enumerable, los tiempos no representan rendimiento real.
8. **Presupuesto de 500 líneas**: obligó a un estilo compacto (varias sentencias por línea,
   menos separación visual). La lógica está separada por secciones y el README documenta cada
   etapa; si el experimento crece, conviene dividir en módulos.

## Recomendación

**El pipeline HarfBuzz → FreeType → atlas R8 → wgpu funciona y es el esqueleto correcto para el
motor.** Para el siguiente paso:

- Mantener el `text_run` del display list como contrato serializable: quads + UV + color por
  vértice permiten batchear todos los glifos en una sola draw call y cachear el atlas entre frames.
- Cachear `adapter`/`device`/pipelines y el atlas a nivel de proceso; el costo actual es casi
  todo arranque, no render.
- Usar avances de HarfBuzz para layout y FreeType solo para cobertura (lo ya hecho). Si se
  necesita consistencia exacta con hinting, evaluar `hb-ft` o desactivar hinting y trabajar en
  subpíxeles.
- Añadir en este orden: wrapping + `line-height`, cadena de fallback de fuentes, bidi
  (SheenBidi), y luego scripts complejos. El atlas debería crecer a 1024/2048 con evicción LRU.
- Para emoji/color, un segundo atlas RGBA y pipeline; para texto normal, R8 es suficiente y barato.
- Medir en una máquina con GPU de hardware (Vulkan/D3D12) antes de optimizar; sobre WARP solo se
  puede validar corrección.

## Archivos

- `run.py` — pipeline completo (500 líneas): parse, cascade, layout, shaping, atlas, IR, wgpu, verify.
- `input.html`, `input.css` — fixture exacto del experimento.
- `shaped.json` — runs con gids, clusters, avances y escala HB.
- `display_list.json` — viewport, metadatos del atlas y ops (`rect`, `text_run`).
- `render.png` — render offscreen 400×200 verificado.
- `title_crop.png` — recorte del título (y 20..70) ampliado 2×.
- `requirements.txt` — dependencias fijadas.
