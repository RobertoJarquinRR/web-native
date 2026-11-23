# exp16-text-c — pipeline de texto (HarfBuzz + FreeType) en C/wgpu-native

**Estado: PASS.** Un `.exe` de 440 KB renderiza el demo de exp09 (texto «Hola» +
3 chips + tarjeta) a un PNG 400x240 con **cero Python en runtime**, y el
resultado es **pixel-idéntico** al render wgpu de exp09 (`maxdiff = 0`,
`0.00 %` de píxeles cambiados en la imagen completa y en el área de texto).

La meta del experimento era portar el pipeline de texto al runtime nativo
C/wgpu-native: shaping HarfBuzz + raster FreeType en build time, atlas R8 y
quads de glyph horneados en el binario, y en runtime solo subir buffers/textura
y dibujar. Eso es exactamente lo que hay aquí.

---

## 1. Qué se hornea y qué queda en runtime

```
BUILD TIME (Python 3.12, .venv)
  demo/input.html + input.css
        │  exp09 frontend (html5lib + cascade + layout + HarfBuzz/FreeType)
        ▼
  display_list.json   (ops: 5 rect + 1 text_run "Hola" con 4 glyphs)
        │  webnative/export.py
        ├─────────────────────────────┐
        ▼                             ▼
  atlas.bin (512² R8, 262144 B)   glyph_quads.json (4 quads)
        │                             │
        └──────────────┬──────────────┘
                       ▼  emit_c.py
                scene_data.h  (rect verts + glyph verts + atlas + WGSL)

RUNTIME (C, /TC, wgpu-native)
  scene_text.exe
    1. sube 30 vértices de rect (12 B) y 24 de glyph (20 B)
    2. sube el atlas 512² como textura R8Unorm + sampler linear/clamp
    3. crea bind group (texture + sampler) y 2 pipelines
    4. 1 render pass: draw rects -> draw glyphs (alpha blend)
    5. copy-to-buffer + WIC -> scene_text.png
    6. lee el atlas de la GPU y compara FNV-1a (prueba de upload)
```

### Diagrama del pipeline de texto (lo específico de exp16)

```
IR text_run (display_list.json)
  x,y = origen/baseline; color; font_file; glyphs[{gid,x,y,w,h,u0,v0,u1,v1}]
        │
        │  webnative/export.py: mismas claves y orden que el renderer wgpu
        │  (text_op_glyph_keys -> build_atlas -> shelf packing 1px de guarda)
        ▼
atlas.bin  (512x512 R8, 262 144 B; H,o,l,a en la primera shelf: x=1,19,36,41)
        │                                      │
        │                                      └─▶ glyph_quads.json (autoritativo:
        │                                          se valida w/h y UV contra el IR)
        ▼
emit_c.py
  ├─ kAtlasPixels[262144]        (array C, sin comprimir, sin zlib)
  ├─ kGlyphQuads[4]              (x,y,w,h,u0,v0,u1,v1,rgba,run,gid)
  ├─ kGlyphVertices[24]          (6/glyph: pos vec2 + uv vec2 + color unorm8x4 = 20 B)
  ├─ kSceneVertices[30]          (6/rect: pos vec2 + color unorm8x4 = 12 B)
  ├─ kRectShaderWGSL / kGlyphShaderWGSL
  └─ #defines (viewport, clear, counts, strides, ATLAS_WIDTH/HEIGHT)
        │
        ▼
runtime_text.c
  ├─ wgpuQueueWriteTexture(kAtlasPixels) -> texture 512² R8Unorm
  ├─ sampler linear + clamp_to_edge
  ├─ bind group layout {0: texture_2d<f32>, 1: filtering sampler} (visibilidad Fragment)
  ├─ pipeline glyph: arrayStride 20, @location(0)=pos, (1)=uv, (2)=color, alpha blend
  └─ WGSL: let a = textureSample(atlas_tex, atlas_sampler, in.uv).r;
           return vec4f(in.color.rgb, in.color.a * a);
```

El shader de glyph es el mismo contrato que `renderer/wgpu_renderer.py`
(`GLYPH_WGSL`), así que la cobertura y el blending coinciden exactamente.

---

## 2. Ficheros

| Fichero | Papel |
|---|---|
| `demo/input.html`, `demo/input.css` | copia del demo de exp09 (400x240, «Hola» Segoe UI 28px #fff + 3 chips + tarjeta) |
| `webnative/` | copia local del frontend de exp09 (html/cascade/layout/text) |
| `webnative/export.py` | **añadido local**: exporta `atlas.bin`, `atlas.json`, `glyph_quads.json` y valida UV/tamaños contra el IR |
| `run.py` | entrada build-time única: IR → atlas → `scene_data.h` → `render_frontend.png` |
| `emit_c.py` | adaptación de `exp10-aot-gpu/emit_c.py`: rects + glyph quads + atlas + WGSL |
| `runtime_text.c` | runtime C: 2 pipelines, 1 render pass, 2 draw calls, readback + PNG |
| `verify.py` | 26 checks de píxeles, conteos, atlas y paridad con exp09 |
| `build.ps1` | pipeline completo reproducible (venv, bake, compile, run aislado, verify, dumpbin) |
| `scene_data.h` | **generado** (576 KB; 262 144 B son el atlas) |
| `atlas.bin`, `atlas.json`, `glyph_quads.json`, `display_list.json` | **generados** |
| `scene_text.exe`, `scene_text.png` | **artefactos** |
| `render_frontend.png` | referencia de exp09 (build-time), pixel-idéntica a `exp09-v1-core/render.png` |
| `verify_report.json`, `build_report.json`, `build_log.txt` | evidencia |
| `proof/dependents.txt` | `dumpbin /dependents`: sin Python/FreeType/HarfBuzz |
| `proof/runtime_only/` | ejecución con solo `scene_text.exe` + `wgpu_native.dll` |
| `proof/text_compare_4x.png` | zoom 4x: arriba C, abajo exp09 (idénticos) |
| `proof/atlas_used_8x.png` | zona usada del atlas (los 4 glyphs) a 8x |

`webnative/` es la copia de exp09 **sin modificar**; el único añadido es
`webnative/export.py`. Los exp01..exp14 no se han tocado (wgpu-native de exp10
se usa en modo lectura).

---

## 3. Comandos exactos

Setup (una vez):

```powershell
cd .\exp16-text-c
py -3.12 -m venv .venv
.venv\Scripts\python.exe -m pip install --upgrade pip
.venv\Scripts\python.exe -m pip install html5lib tinycss2 cssselect2 uharfbuzz freetype-py pillow wgpu
Copy-Item .\exp10-aot-gpu\wgpu-native\bin\wgpu_native.dll . -Force
```

Build-time (IR + atlas + header + referencia):

```powershell
.venv\Scripts\python.exe run.py
#   run.py --skip-frontend-render   # sin wgpu-py (no genera render_frontend.png)
#   emit_c.py --ir display_list.json --atlas atlas.bin --quads glyph_quads.json --out scene_data.h
```

Compilar y ejecutar (MSVC 18, C17, sin C++):

```powershell
# desde un prompt con vcvars64.bat cargado
cl /nologo /O2 /W3 /std:c17 /TC runtime_text.c `
   /I.\exp10-aot-gpu\wgpu-native\include `
   /Fe:scene_text.exe `
   /link .\exp10-aot-gpu\wgpu-native\lib\wgpu_native.dll.lib `
   windowscodecs.lib ole32.lib user32.lib

# aceptación: PATH solo System32 + directorio del exe
$env:PATH = "C:\Windows\System32;.\exp16-text-c"
.\scene_text.exe scene_text.png
```

Todo en uno (venv, bake, compile, run aislado, verify, prueba de
autocontención y dumpbin):

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Verificación sola:

```powershell
.venv\Scripts\python.exe verify.py --png scene_text.png --frontend render_frontend.png `
    --header scene_data.h --ir display_list.json --atlas atlas.bin `
    --quads glyph_quads.json --stdout run_stdout.txt --report verify_report.json `
    --visuals proof --exp09 ..\exp09-v1-core\render.png
```

---

## 4. Resultados (última ejecución)

```
adapter : D3D12 / Microsoft Basic Render Driver (CPU), vendorID=0x1414 deviceID=0x008C
atlas   : uploaded 512x512 R8Unorm (262144 bytes) gpu-hash=0xCAB0968C cpu-hash=0xCAB0968C MATCH
exe     : scene_text.exe 440 832 bytes
png     : scene_text.png 400x240, 2 330 bytes
imports : wgpu_native.dll, ole32.dll, KERNEL32.dll   (0 Python / FreeType / HarfBuzz)
verify  : 26/26 checks
```

### Texto y chips

| Check | Resultado |
|---|---|
| tamaño PNG | 400x240 |
| banda del título (0,10)-(400,70), luminancia > max(100, fondo+40=56.3) | **360 píxeles claros** (>=100 exigidos) |
| bbox del texto | (22, 30) – (76.062, 51), dentro de la banda; 1 135 px de bbox |
| «no es un bloque sólido» | 360 claros de 1 135 px de bbox (cobertura real de glifos) |
| glyph visible (px claros dentro de cada quad) | gid 43 (H)=120, gid 82 (o)=92, gid 79 (l)=63, gid 68 (a)=85 |
| chips exactos | puntos (50,100)=#e74c3c, (118,100)=#2ecc71, (186,100)=#3498db; **3600/3600 px exactos en cada rect de 60x60** |
| fondo exacto | (300,200), (390,20), (10,200), (260,120) = #101014 |
| conteos | IR=4 glyphs, header=4 quads, atlas=4 glyphs, 24 vértices |
| atlas en GPU | FNV-1a `0xCAB0968C` == `atlas.bin` == array C, `MATCH` |
| draw calls | 2 (rects + glyphs) en 1 render pass |

### Prueba fuerte: C vs render wgpu de exp09

| Región | maxdiff | píxeles cambiados |
|---|---|---|
| imagen completa (400x240 = 96 000 px) | **0** | **0 (0.00 %)** |
| área de texto (20,28)-(78,53) = 1 450 px | **0** | **0 (0.00 %)** |
| área de chips (20,71)-(216,131) = 11 760 px | **0** | **0 (0.00 %)** |
| `exp09-v1-core/render.png` (artefacto original de exp09) | **0** | **0 (0.00 %)** |

`render_frontend.png` (producido por la copia local del frontend) es además
pixel-idéntico al `render.png` que exp09 dejó commiteado. Es decir: el `.exe`
reproduce **el artefacto original de exp09**, no solo una re-renderización.

No hay diferencias que explicar: mismos gids, mismo atlas, mismas UVs, mismas
posiciones y mismo target `RGBA8Unorm`. La única diferencia de setup es que C
hace clear con el color de `op[0]` (#0f0f12) y exp09 con negro; es invisible
porque `op[0]` cubre el viewport. Los vértices se emiten con el **mismo float32**
que usa `array("f")` en wgpu-py (round-trip `struct.pack('<f')` + `repr`), lo que
elimina cualquier redondeo de formato.

### Tiempos

| Etapa | Tiempo |
|---|---|
| emit (bake completo: IR + atlas + `scene_data.h` + render de referencia) | **3 448 ms** |
| — frontend IR (HTML+CSS+layout+texto) | 25.2 ms |
| — export de atlas + quads | 11.7 ms |
| — `emit_c.py` (576 KB de header) | 62.2 ms |
| — render de referencia exp09 (wgpu-py, incluye init de device) | 2 701 ms |
| compile (`cl /O2`, header de 576 KB) | **3 621 ms** |
| run (proceso completo, PATH aislado) | **461 ms** |
| verify (26 checks + diffs con Pillow) | **541 ms** |
| run autocontenido (solo exe + dll en el directorio) | 404 ms |
| dentro del exe | init+device+upload 87 ms · atlas+shaders 1.7 ms · pipelines 50 ms · render+copy+submit 1.8 ms · map 28 ms · PNG 8 ms · total 177 ms |

El bake «puro» (sin el render de referencia de exp09) son ~99 ms. El compile
de 3.6 s es el precio del array de 262 144 bytes en el header; sigue siendo
aceptable para AOT. El setup desde cero (crear `.venv` + instalar los 7
paquetes) se verificó también: 71.5 s, una sola vez.

---

## 5. Detalles de implementación relevantes

- **Atlas determinista**: `GlyphAtlas` empaqueta en orden de primera aparición
  con shelf packing y 1 px de guarda por glyph (evita sangrado del sampler
  linear). `export.py` usa exactamente las mismas claves/orden que
  `wgpu_renderer._ensure_atlas`, y **falla el build** si un `w/h` o una UV del
  IR no coincide con el atlas reconstruido.
- **Formato del atlas**: `atlas.bin` = 512×512 bytes R8 crudos, fila a fila, sin
  padding; `atlas.json` lleva `width/height/format/glyph_count/sha256/pixels_nonzero`.
- **Quads**: `glyph_quads.json` contiene la lista plana pedida
  `{run_id, color, x, y, w, h, u0, v0, u1, v1}` (+ `gid`) y metadatos por run.
- **Vértices de glyph**: 6 por quad (TL, BL, BR, TL, BR, TR), stride 20 B; el
  orden de triángulos replica `glyph_vertices()` de exp09.
- **Rect pipeline**: idéntico a exp10 (12 B, blend src-alpha/one-minus; con
  alpha=255 es no-op), sin tocar el comportamiento previo.
- **Prueba de subida del atlas**: el runtime copia la textura 512² de vuelta a
  un buffer, calcula FNV-1a y lo compara con el hash del array C; imprime
  `gpu-hash`/`cpu-hash` y `verify.py` lo cruza con `atlas.bin`.
- **Autocontención**: se ejecuta en `proof/runtime_only/`, un directorio que
  **antes de ejecutar solo contiene** `scene_text.exe` y `wgpu_native.dll`, con
  `PATH=C:\Windows\System32;<ese directorio>`; el PNG resultante tiene el mismo
  SHA-256 que el de la raíz.
- **Sin Python**: `dumpbin /dependents` lista solo `wgpu_native.dll`, `ole32.dll`
  y `KERNEL32.dll` (WIC se carga por COM desde `windowscodecs.dll` del sistema).

---

## 6. Limitaciones (honestas)

1. **Orden painter simplificado**: se dibujan *todos* los rects y después
   *todos* los glyphs (2 draw calls). exp09 respeta el orden del IR con batches
   contiguos. En este demo el texto no se solapa con los chips, así que el
   resultado es idéntico; con texto tapado por un rect posterior el orden
   importaría. Arreglo: emitir batches contiguos y hacer `draw` por batch.
2. **Sin clipping/overflow**: no hay scissor por op ni recorte de contenedores;
   un glyph que se salga de su caja se dibuja igual (el IR tampoco recorta).
3. **Un atlas por documento y tamaño fijo 512²**: si el documento necesita más
   glyphs de los que caben, el build falla (`atlas de glyphs desbordado`). No
   hay atlas múltiples ni páginas.
4. **Solo cobertura grayscale (R8)**: color desde el vértice; no hay emoji a
   color, ni glyphs con capas de color, ni subpixel LCD (solo antialiasing
   grayscale de FreeType).
5. **Posicionamiento ya resuelto en build time**: HarfBuzz/FreeType no están en
   runtime; no hay re-shaping, ni selección de texto, ni cambios de tamaño en
   vivo. Texto estático, que es el objetivo de AOT.
6. **Sin transformaciones de glyph**: los quads son axis-aligned; no hay
   rotación/escala por glyph (sí la que ya aplicó el layout).
7. **Sin MSAA y target `RGBA8Unorm` (no sRGB)**: se mantiene igual que exp09
   para que la paridad sea exacta; un backend sRGB necesitaría re-verificar
   blending/antialiasing.
8. **Coste de compilación**: 262 KB de atlas como array C hacen el header de
   576 KB y ~3.6 s de `cl`. Si creciera, habría que trocear el array en varios
   TUs o empaquetar el atlas (hoy prohibido zlib).
9. **Una fuente por run**: el pipeline soporta varias fuentes porque el atlas
   indexa por `(font_file, gid, size)`, pero el demo usa solo Segoe UI; no hay
   fallback de fuentes en runtime (no hay cmap ni fuentes).
10. **El atlas va sin comprimir**: 262 144 B de los 440 832 B del `.exe`.

---

## 7. Siguiente paso: enchufarlo al runtime R2

El contrato ya está separado en dos mitades limpias:

1. **Build (Python)**: `webnative/export.py` + `emit_c.py` producen un
   `scene_data.h` autocontenido. Para R2 basta con que el emisor escriba el
   bloque de texto (atlas + quads + WGSL) en el formato que R2 consuma, sin
   cambiar el frontend.
2. **Runtime (C)**: `runtime_text.c` demuestra el patrón mínimo — textura R8 +
   sampler + bind group layout + pipeline con alpha blend + 2 draws en un solo
   pass. Para R2:
   - mover la creación de textura/sampler/bind group al arranque del device
     (una vez), no por frame;
   - subir `kGlyphVertices` a un buffer persistente y hacer un draw por run o
     por batch contiguo del IR, intercalado con los rects para recuperar el
     painter's order exacto;
   - añadir scissor por op cuando el layout lo necesite (clipping);
   - si R2 usa target sRGB, decidir gamma explícita y volver a medir la paridad
     contra exp09 (hoy es Unorm a propósito);
   - conservar la prueba de hash del atlas como smoke test de upload en CI;
   - presupuestar el atlas como recurso por documento (512² R8 = 256 KB) y
     fallar el build si desborda, igual que aquí.

Con eso, R2 podría renderizar texto sin Python, sin FreeType/HarfBuzz, sin
ficheros de fuente y sin navegador, con la garantía medida de que el resultado
coincide píxel a píxel con el frontend de referencia.
