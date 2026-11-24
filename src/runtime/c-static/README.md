# Runtime C canónico — rects + texto

**Estado: PASS.** El runtime C canónico de `src/runtime/c-static` ya renderiza
rects **y texto** desde datos horneados, tanto en modo offscreen (PNG) como en
ventana Win32 con swapchain. El `.exe` de ventana dibuja los rects y los glyphs
en un solo render pass, sin Python, sin JSON, sin FreeType/HarfBuzz, sin fichero
de fuente y sin navegador en runtime.

La paridad con el pipeline probado de exp16 es exacta: `scene_text.png` es
**pixel-idéntico** a `archive/experiments/16-text-atlas-c/scene_text.png`
(`maxdiff = 0`, `0` píxeles cambiados de 96 000).

---

## 1. Qué se hornea y qué queda en runtime

```
BUILD TIME (Python 3.12, solo build/verify)
  display_list.json  (rect + text_run con glyphs{x,y,w,h,u0..v1,gid})
  atlas.bin          (R8 crudo: width*height bytes)
        │
        ▼  src/emitter/python/emit_c_text.py
  scene_data.h
    ├─ kSceneOps[]        rects (x,y,w,h,rgba)
    ├─ kSceneVertices[]   6 vértices/rect  (pos vec2 + color unorm8x4 = 12 B)
    ├─ kGlyphQuads[]      1 quad/glyph     (x,y,w,h,uv,rgba,run,gid)
    ├─ kGlyphVertices[]   6 vértices/glyph (pos vec2 + uv vec2 + color = 20 B)
    ├─ kAtlasPixels[]     atlas R8 crudo   (width*height bytes)
    ├─ kRectShaderWGSL    pipeline sólido
    ├─ kGlyphShaderWGSL   pipeline texturizado (texture_2d + sampler)
    └─ #defines           viewport, clear, counts, strides, ATLAS_WIDTH/HEIGHT

RUNTIME (C17, /TC, wgpu-native v29.0.1.1)
  scene_text.exe / webnative-text.exe
    1. sube los vértices de rect (12 B) y de glyph (20 B)
    2. sube el atlas como textura R8Unorm + sampler linear/clamp
    3. crea el bind group { texture_2d<f32>, filtering sampler }
    4. 1 render pass:
         draw 1: rects   (pipeline sólido)
         draw 2: glyphs  (pipeline texturizado, alpha blend)
    5. offscreen: copy-to-buffer + WIC -> PNG
       ventana:   wgpuSurfacePresent (swapchain)
    6. offscreen/ventana: lee el atlas de la GPU y compara FNV-1a
       con el array C (prueba de que la textura se subió)
```

### Diagrama del pipeline de texto

```
IR text_run (display_list.json)
  x,y = origen/baseline; color; font_file; glyphs[{gid,x,y,w,h,u0,v0,u1,v1}]
        │
        │  emit_c_text.py: deriva la lista plana de quads del propio IR
        │  (mismas claves y orden que exp16; --quads lo cruza con
        │   glyph_quads.json y falla si hay cualquier diferencia float32)
        ▼
atlas.bin 512x512 R8  +  glyph quads 4
        │
        ├─▶ kGlyphVertices[24]  (TL, BL, BR, TL, BR, TR por glyph)
        ├─▶ kAtlasPixels[262144]
        └─▶ kGlyphShaderWGSL:
              let coverage = textureSample(atlas_tex, atlas_sampler, in.uv).r;
              return vec4f(in.color.rgb, in.color.a * coverage);
        │
        ▼
runtime_window.c / runtime_gpu_text.c
  atlas R8Unorm -> sampler linear/clamp -> bind group
  vértice glyph: arrayStride 20, @location(0)=pos, (1)=uv, (2)=color
  blend: src.rgb*src.a + dst*(1-src.a); alpha: 1*src.a + dst*(1-src.a)
```

El shader de glyph es el mismo contrato que exp16/exp09 (`GLYPH_WGSL`), así que
la cobertura y el blending coinciden exactamente con el render de referencia.

---

## 2. Ficheros

| Fichero | Papel |
|---|---|
| `src/emitter/python/emit_c_text.py` | **nuevo**: emisor texto-capaz. IR + atlas → `scene_data.h` (rects, glyph quads, vértices, atlas, WGSL). Deriva los quads del IR y opcionalmente los cruza con `glyph_quads.json`. |
| `src/runtime/c-static/runtime_window.c` | **extendido**: ventana Win32 + swapchain + atlas + sampler + bind group + pipeline de glyphs + resize. Sigue compilando con el header rect-only (el texto se compila fuera). |
| `src/runtime/c-static/runtime_gpu_text.c` | **nuevo**: offscreen texto-capaz (basado en exp16 `runtime_text.c`), con prueba de hash del atlas y PNG por WIC. También compila con un header rect-only. |
| `src/runtime/c-static/runtime_gpu.c` | **sin cambios**: camino rect-only original. |
| `src/runtime/c-static/build_text.ps1` | **nuevo**: build + verificación completa (texto, ventana, resize, atlas, autocontención y regresión rect-only). No sustituye a `src/build.ps1`. |
| `src/runtime/c-static/verify_text.py` | **nuevo**: verificador build-time con Pillow (modo `text` y modo `rect`). |
| `src/runtime/c-static/README.md` | este documento. |

Artefactos generados en `build/text-runtime/` (no versionados):
`scene_data.h`, `scene_text.exe`, `webnative-text.exe`, `scene_text.png`,
`build_report.json`, `build_timings.json`, `verify_text.json`, logs y
`proof_runtime_only/`.

---

## 3. Comandos exactos

### Build + verificación completa (recomendado)

```powershell
cd .
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-static\build_text.ps1
```

Opciones útiles:

```powershell
# ventana: 20 frames y resize programático a 720x480 tras el frame 5
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-static\build_text.ps1 `
    -Frames 20 -ResizeAfter 5

# otro directorio de build
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-static\build_text.ps1 `
    -OutDir C:\temp\wn-text

# solo offscreen / sin regresión rect
powershell -NoProfile -ExecutionPolicy Bypass -File src\runtime\c-static\build_text.ps1 `
    -SkipWindow -SkipRect
```

### Emisor manual

```powershell
$py = .\src\frontend\python\.venv\Scripts\python.exe
$out = .\build\text-runtime
Copy-Item .\archive\experiments\16-text-atlas-c\display_list.json $out
Copy-Item .\archive\experiments\16-text-atlas-c\atlas.bin $out
Copy-Item .\archive\experiments\16-text-atlas-c\atlas.json $out
Copy-Item .\archive\experiments\16-text-atlas-c\glyph_quads.json $out

& $py .\src\emitter\python\emit_c_text.py `
    --ir "$out\display_list.json" `
    --atlas "$out\atlas.bin" `
    --atlas-json "$out\atlas.json" `
    --quads "$out\glyph_quads.json" `
    --out "$out\scene_data.h"
```

El emisor resuelve las dimensiones del atlas en este orden: flags
`--atlas-width/--atlas-height`, `--atlas.json` (o `atlas.json` junto al
`atlas.bin`), y por último un atlas cuadrado inferido del tamaño del fichero.
Valida `size_bytes`, `sha256` y `format: r8unorm` cuando hay `atlas.json`.

### Compilar a mano (MSVC 18, C17)

```powershell
$wgpu = .\third_party\wgpu-native
cd .\build\text-runtime

# offscreen
cl /nologo /O2 /W3 /std:c17 /TC runtime_gpu_text.c `
   /I"$wgpu\include" /Fe:scene_text.exe `
   /link "$wgpu\lib\wgpu_native.dll.lib" windowscodecs.lib ole32.lib user32.lib

# ventana
cl /nologo /O2 /W3 /std:c17 /TC runtime_window.c `
   /I"$wgpu\include" /Fe:webnative-text.exe `
   /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup `
         "$wgpu\lib\wgpu_native.dll.lib" user32.lib gdi32.lib
```

### Ejecutar

```powershell
cd .\build\text-runtime
Copy-Item ..\..\third_party\wgpu-native\bin\wgpu_native.dll .
$env:PATH = "C:\Windows\System32;$PWD"

.\scene_text.exe scene_text.png
.\webnative-text.exe --frames 20 --resize-after 5
```

### Regresión rect-only (la que usa `src/build.ps1`)

```powershell
$rect = .\build\text-runtime\rect
cd $rect
.\render_rect.exe scene_rect.png       # emit_c.py + runtime_gpu.c
.\webnative-rect.exe --frames 5        # runtime_window.c con header rect-only
```

---

## 4. Resultados (última ejecución)

```
adapter : D3D12 / Microsoft Basic Render Driver (CPU)
          vendorID=0x1414 deviceID=0x008C
atlas   : 512x512 R8Unorm (262144 B) gpu-hash=0xCAB0968C
          cpu-hash=0xCAB0968C MATCH
exe     : scene_text.exe 440 832 B | webnative-text.exe 425 472 B
png     : scene_text.png 400x240, 2 330 B
header  : scene_data.h 576 696 B (262 144 B son el atlas)
imports : webnative-text.exe -> wgpu_native.dll, USER32, GDI32, KERNEL32
          scene_text.exe     -> wgpu_native.dll, ole32, KERNEL32
          (sin Python / FreeType / HarfBuzz)
```

### Texto y chips (verificador `verify_text.py --mode text`, 10/10)

| Check | Resultado |
|---|---|
| tamaño PNG | 400x240 |
| paridad con exp16 | **maxdiff 0**, 0 píxeles cambiados (0.00 %) |
| banda del título (0,10)-(400,70) | **360 píxeles claros** (>=100 exigidos, umbral 100.0) |
| glyph visible | gid 43 (H)=120 px, gid 82 (o)=92, gid 79 (l)=63, gid 68 (a)=85 |
| chips exactos | (20,71)=#e74c3c, (88,71)=#2ecc71, (156,71)=#3498db; **3600/3600 exactos** en cada uno |
| fondo exacto | (300,200), (390,20), (10,200), (260,120) = #101014 |
| conteos | IR=4 glyphs, header=4 quads, 24 vértices de glyph |
| atlas en GPU | FNV-1a `0xCAB0968C` == array C == `atlas.bin` |
| draw calls | 2 (rects + glyphs) en 1 render pass |

### Ventana

```
adapter: backend=D3D12 (4) type=cpu vendor=0x1414 device=0x008C
surface: formats=6 presentModes=3 chosenFormat=28 960x640
atlas:   gpu-hash=0xCAB0968C cpu-hash=0xCAB0968C MATCH
window:  resized to 960x640 (surface reconfigured; glyph pipeline kept)
window:  requested resize to 720x480 client after frame 5
window:  resized to 720x480 (surface reconfigured; glyph pipeline kept)
window:  closed after 20 frame(s), 2 resize(s), 20 glyph frame(s),
         init 137.81 ms, 15.19 ms/frame average
exit:    0
```

La ventana usa el formato preferido del surface (aquí `chosenFormat=28`,
`BGRA8UnormSrgb`). El resize solo reconfigura el swapchain; el atlas, el bind
group y ambos pipelines se conservan, por eso sigue dibujando glyphs después
del resize (20/20 frames con glyphs).

### Regresión rect-only

| Check | Resultado |
|---|---|
| `render_rect.exe` (emit_c.py + runtime_gpu.c) | 320x200, **maxdiff 0** contra referencia Pillow independiente |
| `webnative-rect.exe --frames 5` | exit 0, D3D12, `text=off` (header sin glyphs) |

También se probó el camino «header de texto sin glyphs»: `emit_c_text.py` sobre
un IR sin `text_run` produce `SCENE_GLYPH_QUAD_COUNT 0`; tanto
`runtime_gpu_text.c` como `runtime_window.c` compilan sin warnings con `/W4`,
renderizan los rects (maxdiff 0) y la ventana informa `text=off`.

### Autocontención

`proof_runtime_only/` contiene **solo** `scene_text.exe` + `wgpu_native.dll`
antes de ejecutar. El PNG resultante tiene el mismo SHA-256 que el de
`build/text-runtime/` y paridad `maxdiff 0` con exp16.

### Tiempos (build/verify, esta máquina)

| Etapa | Tiempo |
|---|---|
| emit texto (`emit_c_text.py`, header 576 KB) | **226.47 ms** |
| emit rect (`emit_c.py`, regresión) | 159.73 ms |
| compile (4 exes: texto, ventana, rect, ventana rect) | **6 695.10 ms** |
| run offscreen texto | 556.91 ms (proceso completo; dentro del exe 187.15 ms) |
| verify texto (10 checks + Pillow) | 363.80 ms |
| proof autocontenido | 1 939.61 ms |
| run ventana (20 frames + resize) | 606.71 ms (init 137.81 ms; 15.19 ms/frame) |
| run rect offscreen | 332.87 ms (dentro del exe 125.99 ms) |
| verify rect | 274.75 ms |
| run ventana rect-only (5 frames) | 324.30 ms |

El compile es el coste dominante: cada ejecutable incluye 262 144 B de atlas
como array C en un header de 576 KB. Sigue siendo aceptable para AOT. Los
tiempos varían entre ejecuciones (caché de disco, antivirus, carga de la
máquina); las cifras son de la última ejecución registrada en
`build_report.json`.

---

## 5. Prueba de subida del atlas

Ambos runtimes hacen la misma prueba en el arranque:

1. `wgpuQueueWriteTexture` sube `kAtlasPixels` a una textura `R8Unorm`.
2. Se copia la textura de vuelta a un buffer (`CopySrc` → `CopyDst|MapRead`).
3. Se calcula FNV-1a por filas, saltando el padding de 256 B si lo hubiera
   (`fnv1a_rows`), y se compara con el hash del array C.
4. Se imprime `gpu-hash`, `cpu-hash` y `MATCH`; si no coincide, el runtime
   falla y no escribe el PNG.

Resultado: `0xCAB0968C == 0xCAB0968C`. Además `verify_text.py` cruza ese valor
con el `sha256` de `atlas.json`/`atlas.bin` y con el `MATCH` del stdout.

---

## 6. Limitaciones (honestas)

1. **Orden painter simplificado**: se dibujan *todos* los rects y después
   *todos* los glyphs (2 draw calls en 1 pase). Si un rect posterior tapara
   texto, el resultado no sería el del painter's order real. Arreglo: emitir
   batches contiguos del IR y hacer un draw por batch.
2. **Sin clipping/scissor**: no hay recorte por op ni por contenedor; un glyph
   que se salga de su caja se dibuja igual. El IR tampoco recorta hoy.
3. **Un atlas por documento y tamaño fijo**: el emisor consume `atlas.bin`; no
   empaqueta ni pagina. Si el atlas se desborda, debe fallar el build de texto
   (hoy lo hace el export de exp16, no este emisor).
4. **Solo cobertura grayscale (R8)**: color desde el vértice; no hay emoji a
   color, capas de color ni subpixel LCD.
5. **Texto estático**: HarfBuzz/FreeType trabajan en build time. No hay
   re-shaping, selección, ni fallback de fuentes en runtime.
6. **Sin transformaciones por glyph**: los quads son axis-aligned; no hay
   rotación/escala por glyph (sí la que ya aplicó el layout).
7. **Offscreen `RGBA8Unorm` vs ventana sRGB**: la paridad pixel a pixel está
   probada en offscreen (`RGBA8Unorm`). La ventana usa el formato preferido del
   surface, aquí `BGRA8UnormSrgb` (28); la geometría, UVs, atlas y blending son
   los mismos, pero la conversión de color del swapchain puede cambiar el valor
   final de los píxeles. Si se necesita paridad exacta en ventana, elegir un
   formato no-sRGB o añadir un pase de gamma explícito.
8. **Atlas sin comprimir**: 262 144 B del `.exe` son el atlas; el header pesa
   576 KB y el compile de los 4 exes ronda 6 s.
9. **`runtime_gpu.c` sigue siendo rect-only**: con un header de texto compila y
   dibuja solo los rects (compatibilidad legacy), pero el modo offscreen con
   texto es `runtime_gpu_text.c`.
10. **El emisor no hace shaping ni rasterizado**: requiere que el IR ya traiga
    `text_run.glyphs[]` y que exista `atlas.bin`. El frontend Rust canónico
    todavía no genera atlas; hoy el atlas viene del pipeline Python de exp16 o
    del frontend Python de texto.

---

## 7. Integración con `src/build.ps1`

`src/build.ps1` **no se ha tocado** y su camino rect-only sigue funcionando.
Además de compilar `runtime_window.c` con el header de `emit_c.py`
(`SCENE_HAS_TEXT == 0`), se ejecutó el script completo:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src\build.ps1 `
    -Html benchmarks\frontend-parity\fixtures\contract\input.html `
    -OutDir build\rect-buildscript -Width 320 -Height 200
```

Generó `render_png.exe` y `webnative-rust.exe`; ambos arrancaron con D3D12 y
salieron con código 0.

Para enchufar texto al build canónico:

1. **Atlas**: añadir un paso build-time que produzca `atlas.bin` + `atlas.json`
   (el `export.py` de exp16 o el frontend Python de texto). El frontend Rust
   actual no los emite todavía; es la pieza que falta.
2. **Emisor**: sustituir la llamada a `emit_c.py` por:

   ```powershell
   & $py src\emitter\python\emit_c_text.py `
       --ir "$OutDir\display_list.json" `
       --atlas "$OutDir\atlas.bin" `
       --atlas-json "$OutDir\atlas.json" `
       --out "$OutDir\scene_data.h"
   ```

   `--quads glyph_quads.json` es opcional pero recomendable: cruza los quads
   derivados del IR con el export y falla ante cualquier divergencia float32.
3. **Offscreen**: compilar `runtime_gpu_text.c` en lugar de `runtime_gpu.c`
   (mismos flags y librerías).
4. **Ventana**: no hay que cambiar nada: el mismo `runtime_window.c` detecta
   `SCENE_GLYPH_QUAD_COUNT > 0`, crea atlas/sampler/bind group/pipeline y dibuja
   los glyphs. Con un header rect-only toma el camino `text=off`.
5. **Incluir** `scene_data.h` en el directorio de compilación (o `/I` al
   `$OutDir`), como ya hace `build.ps1`.

`build_text.ps1` es la referencia ejecutable de estos pasos y añade la puerta
de píxeles (`verify_text.py`) que conviene llevar a CI.

---

## 8. Evidencia

En `build/text-runtime/`:

- `build_report.json` — estado PASS, adapter, atlas proof, ventana, rect, autocontención.
- `build_timings.json` — todos los tiempos de la tabla.
- `verify_text.json` — 10 checks de texto con métricas y detalles.
- `offscreen_stdout.txt`, `offscreen_stderr.txt` — ejecución offscreen.
- `window_stdout.txt`, `window_stderr.txt` — ventana, resize y glyph frames.
- `proof_runtime_only/` — ejecución con solo exe + dll; PNG con SHA-256 idéntico.
- `rect/verify_rect.json`, `rect/rect_window_stdout.txt` — regresión rect-only.

Comandos de verificación sueltos:

```powershell
$py = .\src\frontend\python\.venv\Scripts\python.exe
& $py .\src\runtime\c-static\verify_text.py `
    --mode text `
    --png .\build\text-runtime\scene_text.png `
    --reference .\archive\experiments\16-text-atlas-c\scene_text.png `
    --ir .\build\text-runtime\display_list.json `
    --header .\build\text-runtime\scene_data.h `
    --stdout .\build\text-runtime\offscreen_stdout.txt `
    --report .\build\text-runtime\verify_text.json
```
