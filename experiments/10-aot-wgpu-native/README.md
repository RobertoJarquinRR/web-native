# exp10-aot-gpu — IR de display-list → geometría GPU + WGSL → wgpu-native

**Estado: PASS** (con prueba fuerte adicional). Experimento 10 de `webnative`.

Compila el IR JSON (`display_list.json`) a datos de vértices y a un shader WGSL en
**tiempo de build**, y renderiza la escena de forma nativa con **wgpu-native** desde C.
El `.exe` resultante no usa Python, ni navegador, ni WebView, ni Skia, ni Electron.

---

## 1. Pipeline

```
 display_list.json                  (IR agnóstico, CONTRACT.md)
        │
        │  emit_c.py  (build time, Python)
        ▼
 scene_data.h
   ├─ kSceneVertices[18]   6 vértices/rect: vec2 clip-space + color unorm8x4
   ├─ kSceneOps[3]         metadatos para el bucle genérico / reporte
   ├─ kSceneShaderWGSL     shader generado, embebido como string C
   └─ SCENE_WIDTH/HEIGHT + clear color (op[0])
        │
        │  cl.exe  (C nativo, sin parser JSON en runtime)
        ▼
 scene_gpu.exe  ──►  wgpu-native (webgpu.h C API)
                        │
                        ├─ D3D12  (esta máquina: Microsoft Basic Render Driver / WARP)
                        ├─ Vulkan
                        └─ Metal
                        │
                        ▼
              textura 400x300 rgba8unorm (RENDER_ATTACHMENT | COPY_SRC)
                        │  render pass: clear + 1 draw call (18 vértices)
                        ▼
              copy_texture_to_buffer (filas alineadas a 256 B → 1792 B/fila)
                        │  map read
                        ▼
              PNG vía WIC (windowscodecs.lib)
                        │
                        ▼
              scene_gpu.png  ──►  verify.py (Pillow, solo build/verificación)
```

### Qué significa AOT aquí

- `emit_c.py` corre **solo en build**. Lee el JSON, valida el contrato y escribe C.
- La geometría ya está en **clip space** (`-1..1`), calculada con el viewport: no hay
  matrices ni uniforms en runtime.
- El shader WGSL también se genera y se embebe en `scene_data.h`; no se lee de disco.
- `runtime_gpu.c` es un consumidor genérico: no conoce la escena, solo recorre
  `kSceneOps`, sube `kSceneVertices` y dibuja. Cambiar el JSON cambia el `.exe`.

---

## 2. Entorno y versiones

| Componente | Versión |
|---|---|
| wgpu-native | **v29.0.1.1** (`wgpu-windows-x86_64-msvc-release.zip`, 17,062,968 B) |
| DLL | `wgpu_native.dll` 9,117,696 B |
| Compilador | MSVC `19.51.36260` x64 (VS 18 Community) |
| Python | 3.12 (solo build/verificación) |
| Pillow | 13.0.0 (solo verificación) |
| SO | Windows 10/11 x64 |

Descarga con **Python urllib** (curl/wget bloqueados en este entorno):

```powershell
py -3.12 fetch_wgpu.py
```

`fetch_wgpu.py` fija `VERSION = v29.0.1.1`, descarga el zip, extrae y normaliza:

```
wgpu-native/
  include/wgpu.h, include/webgpu.h, include/webgpu/{wgpu.h,webgpu.h}
  lib/wgpu_native.dll.lib      (import library, 62 KB — la usada al enlazar)
  lib/wgpu_native.lib          (static lib, 54 MB — no usada)
  bin/wgpu_native.dll
  VERSION.txt
```

> Nota: el enlace usa `wgpu_native.dll.lib` (import library del DLL), no el
> `wgpu_native.lib` estático de 54 MB. El resultado es el mismo DLL que exige la
> acceptance y evita enlazar todo wgpu dentro del `.exe`. El DLL se copia junto al
> ejecutable.

---

## 3. Comandos exactos

### Build completo (recomendado)

```powershell
cd .\exp10-aot-gpu
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
# con prueba fuerte (rect extra, re-emit, recompila, verifica y restaura):
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Proof
```

`build.ps1` imprime cada comando, mide tiempos y deja `build_report.json`.

### Paso a paso manual

```powershell
# 1. dependencia nativa (una vez)
py -3.12 fetch_wgpu.py

# 2. AOT: JSON -> vertex data + WGSL
py -3.12 emit_c.py --ir display_list.json --out scene_data.h

# 3. compilar (C, no C++)
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" >nul && ^
cl /nologo /O2 /W3 /std:c17 /TC runtime_gpu.c /Iwgpu-native\include /Fe:scene_gpu.exe ^
/link wgpu-native\lib\wgpu_native.dll.lib windowscodecs.lib ole32.lib user32.lib"

# 4. DLL junto al exe
copy wgpu-native\bin\wgpu_native.dll .

# 5. ejecutar (headless, escribe PNG)
.\scene_gpu.exe scene_gpu.png

# 6. verificar píxeles (solo build; requiere Pillow)
.\.venv\Scripts\python.exe verify.py --png scene_gpu.png --report verify_report.json
```

### Ejecución aislada (acceptance)

El build ejecuta el `.exe` con un `PATH` que contiene **solo** `System32` y el
directorio del DLL, y sin Python:

```powershell
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = ".\exp10-aot-gpu\scene_gpu.exe"
$psi.Arguments = "scene_gpu.png"
$psi.WorkingDirectory = ".\exp10-aot-gpu"
$psi.UseShellExecute = $false
$psi.EnvironmentVariables["PATH"] = "C:\Windows\System32;.\exp10-aot-gpu"
[System.Diagnostics.Process]::Start($psi).WaitForExit()
```

Resultado real:

```
isolated: exit=0
PATH=C:\Windows\System32;.\exp10-aot-gpu
```

---

## 4. Adaptador y backend

Salida real de `scene_gpu.exe`:

```
scene_gpu: AOT GPU scene 400x300, 3 rect op(s), 18 vertices
adapter: backend=D3D12 (4) type=cpu vendorID=0x1414 deviceID=0x008C
adapter: vendor=10.0.19041.5794
adapter: architecture=(none)
adapter: device=Microsoft Basic Render Driver
adapter: description=(none)
scene_gpu: wrote scene_gpu.png (400x300)
timings: init+adapter+device+upload=89.90ms | pipeline=5.47ms | render+copy+submit=2.52ms | map=24.37ms | png=9.64ms | total=131.90ms
```

- Backend elegido: **D3D12**.
- Adaptador: **Microsoft Basic Render Driver** = **WARP** (rasterizador software).
  `type=cpu`, `vendorID=0x1414` (Microsoft), `deviceID=0x008C`.
- Esta máquina no tiene GPU hardware; es lo esperado. La misma ruta de código usa
  Vulkan/Metal/D3D12 real en otras máquinas.
- wgpu primero sondea OpenGL y lo descarta:
  `[wgpu 2] Returned GL context is 3.1, when 3.3+ is needed`
  (mensaje informativo de wgpu-native, nivel `Warn`; no es un fallo).

---

## 5. Verificación

`verify.py` (Pillow, solo build) comprueba tamaño y píxeles contra `CONTRACT.md`,
con tolerancia 8 por canal. Resultado: **maxdiff = 0 en los tres puntos**.

| Punto | Esperado | Obtenido | maxdiff | Estado |
|---|---|---|---|---|
| (60,60) box a | `#e74c3c` | `#e74c3c` | 0 | OK |
| (60,156) box b | `#3498db` | `#3498db` | 0 | OK |
| (200,250) root | `#101014` | `#101014` | 0 | OK |

PNG: `400x300`, RGBA, alpha 255. `verify_report.json` guarda el detalle.

### Por qué no hay diferencias de gamma

La textura es `RGBA8Unorm` (no `RGBA8UnormSrgb`) y el color de clear se pasa como
`componente/255.0f`. El shader devuelve exactamente el `unorm8x4` que viene del
vértice. Unorm8 almacena `round(valor × 255)`, así que los bytes del IR llegan
intactos al PNG:

- `#101014` → (16,16,20)
- `#e74c3c` → (231,76,60)
- `#3498db` → (52,152,219)

Con un target sRGB habría conversión lineal↔sRGB y el diff esperado sería > 8.
La tolerancia de 8 se mantiene por si otro backend redondea distinto; aquí no se usa.

---

## 6. Prueba fuerte: el JSON manda en la GPU

`build.ps1 -Proof` hace el ciclo completo:

1. Guarda el `display_list.json` canónico.
2. Añade un cuarto rect: `x=260 y=180 w=80 h=60 fill=#2ecc71`.
3. Re-emite `scene_data.h` (24 vértices), recompila y ejecuta.
4. Verifica `(300,210) = #2ecc71` **y** los tres píxeles originales.
5. Restaura el JSON canónico, re-emite, recompila y re-verifica el artefacto final.

Evidencia:

```
proof_add_rect: appended {...} -> 4 ops
emit_c: 4 rect ops, 24 vertices
scene_gpu: AOT GPU scene 400x300, 4 rect op(s), 24 vertices
  op[3] rect x=260.0 y=180.0 w=80.0 h=60.0 rgba=0x2ECC71FF
  proof-rect (300,210) expected #2ecc71 actual #2ecc71 maxdiff=0 -> OK
verify: PASS -> proof\verify_report_proof.json
```

Artefactos: `proof/scene_gpu_proof.exe`, `proof/scene_gpu_proof.png`,
`proof/verify_report_proof.json`.

---

## 7. Tiempos (build.ps1 -Proof, WARP)

| Fase | Tiempo (aprox.) |
|---|---|
| `emit` (Python + escritura de header) | ~145 ms |
| `compile` (vcvars + cl /O2 + link) | ~3.2 s |
| `run` (proceso aislado completo) | ~400 ms |
| `verify` (Pillow) | ~225 ms |
| `proof` (re-emit + recompila + run + verify) | ~3.7 s |

Dentro del `.exe` (una ejecución):

| Fase GPU | Tiempo (aprox.) |
|---|---|
| instance + adapter + device + upload | ~90 ms |
| crear shader + pipeline | ~5.5 ms |
| render pass + copy + submit | ~2.5 ms |
| map del readback | ~25 ms |
| PNG WIC | ~10 ms |
| **total interno** | **~132 ms** |

Los tiempos varían ±10% entre ejecuciones (WARP, arranque de proceso y map).

El coste dominante es la inicialización de wgpu/D3D12 WARP y el map síncrono, no el
dibujo (18 vértices, 1 draw call).

---

## 8. Hallazgos y decisiones

### 8.1 Blocker real de wgpu-native v29.0.1.1: `wgpuInstanceWaitAny` no existe

En esta release, `wgpuInstanceWaitAny` es literalmente `unimplemented!()` en
`src/unimplemented.rs:233` y **aborta el proceso** con un panic de Rust:

```
thread '<unnamed>' panicked at src\unimplemented.rs:233:5: not implemented
```

Solución adoptada: usar `WGPUCallbackMode_AllowProcessEvents` y bombear
`wgpuInstanceProcessEvents(instance)` en un bucle hasta que el callback marque
`done`. Es la ruta soportada y la que usan los ejemplos de wgpu-native. Aplica a
`wgpuInstanceRequestAdapter`, `wgpuAdapterRequestDevice` y `wgpuBufferMapAsync`.

Consecuencia práctica: no se puede usar el patrón "WaitAnyOnly + WaitAny" con esta
versión. Si se actualiza wgpu-native, revisar si ya está implementado.

### 8.2 `/TC` en vez de `/TP`

La instrucción sugería `cl /O2 /TP`. Se compila como **C** (`/TC`) porque:

- `webgpu.h` es una API C y se usa directamente (sin envoltorios C++).
- `wincodec.h` en modo C++ expone interfaces COM con métodos (`factory->CreateBitmap`),
  mientras que en C expone `lpVtbl` (el código de exp08 usa ese estilo).
- El objetivo del experimento es un runtime C nativo; `/TP` obligaría a reescribir
  WIC al estilo C++ sin aportar nada.

El binario sigue siendo un `.exe` nativo MSVC x64 sin runtime de Python.

### 8.3 Formato y alineación

- Textura `RGBA8Unorm`, `RENDER_ATTACHMENT | COPY_SRC`.
- `bytesPerRow` debe ser múltiplo de 256: 400×4 = 1600 → **1792** (7×256).
  Buffer de readback: 1792×300 = 537,600 B.
- El PNG se escribe con WIC (`GUID_WICPixelFormat32bppRGBA`), enlazando
  `windowscodecs.lib ole32.lib user32.lib`.

### 8.4 WARP

No hay GPU hardware. D3D12 WARP es el fallback correcto y permite validar toda la
tubería (shader, pipeline, render target, copia, readback). Los tiempos no son
representativos de una GPU real.

### 8.5 Un solo draw call y orden de pintado

Los 3 rects se concatenan en un único vertex buffer y se dibujan con
`wgpuRenderPassEncoderDraw(pass, 18, 1, 0, 0)`. El orden painter's se conserva
(root, a, b) y el blend straight-alpha (`SrcAlpha`/`OneMinusSrcAlpha`) permite
futuras ops con alpha. El clear usa el color de `op[0]`; además se dibuja el rect
root, por lo que el resultado es idéntico y el shader no necesita casos especiales.

---

## 9. Limitaciones

- **Solo rects**: no hay texto, bordes, radios, gradientes, sombras, transforms ni
  opacidad de grupo.
- **Sin clips**: no hay scissor stack ni máscaras; un clip requeriría scissor por op
  (varias passes) o un shader con planos/máscara.
- **Sin superficie/present**: render headless a textura y readback. No hay ventana,
  swapchain ni vsync.
- **Un frame por ejecución**: sin loop de render ni doble buffer.
- **WARP**: rendimiento software; los tiempos no reflejan hardware.
- **Readback síncrono**: `map` bloquea hasta que la GPU termina; en producción se
  usaría doble buffer y fences.
- **PNG Windows-only** (WIC). La capa GPU es portable; el encoder no.
- **ABI**: `webgpu.h` y `wgpu_native.dll` deben ser de la misma release. v29.0.1.1
  tiene funciones sin implementar (ver 8.1).
- **Sin caché de pipeline**: se crea en cada arranque (~5 ms con WARP).

---

## 10. Próximos pasos

1. **Text atlas**: generar un atlas de glifos en build, embeberlo como bytes y dibujar
   quads con un bind group (textura + sampler). Primer caso real de muestreo.
2. **Clips**: pila de scissor rects; para clips no rectangulares, máscara o
   `discard` en fragment shader.
3. **Batching/instancing**: buffer de instancias (`rect` + color) y un draw call
   instanciado; reduce vértices y prepara el camino para miles de ops.
4. **sRGB correcto**: decidir compositing lineal vs sRGB y usar `RGBA8UnormSrgb` +
   conversión en shader según el contrato.
5. **Ventana real**: surface Win32 (`WGPUSurfaceSourceWindowsHWND`), swapchain y
   loop de presentación; el mismo pipeline sirve.
6. **Async**: pipeline compile asíncrono y readback con doble buffer/fences.
7. **Métricas GPU**: timestamp queries para medir el pass sin el coste del map.

---

## 11. Archivos

| Archivo | Rol |
|---|---|
| `display_list.json` | IR de entrada (3 ops, contrato) |
| `emit_c.py` | AOT: JSON → `scene_data.h` (vértices + WGSL + constantes) |
| `scene_data.h` | Generado; datos de GPU y shader embebidos |
| `runtime_gpu.c` | Runtime C: wgpu-native, render pass, readback, PNG WIC |
| `build.ps1` | Orquesta fetch/emit/compile/copy/run/verify; `-Proof` |
| `fetch_wgpu.py` | Descarga y normaliza wgpu-native v29.0.1.1 (urllib) |
| `verify.py` | Verificación de píxeles con Pillow (build) |
| `proof_add_rect.py` | Añade el rect de la prueba fuerte |
| `scene_gpu.exe` | **Artefacto final** (172,032 B) + `wgpu_native.dll` |
| `scene_gpu.png` | Salida 400×300 |
| `verify_report.json` | Resultado de píxeles |
| `build_report.json` | Versiones, comandos, adapter, tiempos, aislamiento |
| `build_log.txt` | Salida completa del build final con `-Proof` |
| `proof/` | Evidencia de la prueba fuerte |

Referencias de solo lectura: `../exp01-pure-python` … `../exp08-aot`.

---

## 12. Reproducir desde cero

```powershell
cd .\exp10-aot-gpu

# Python solo para build/verificación
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install pillow

# dependencia nativa + build + verificación + prueba fuerte
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Proof
```

Salida esperada: `================ exp10-aot-gpu: PASS ================` con
`adapter: backend=D3D12`, `device=Microsoft Basic Render Driver`, `maxdiff=0` y
`exit=0` en el run aislado.
