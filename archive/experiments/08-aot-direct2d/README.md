# exp08-aot — prueba de compilación AOT real

Objetivo: demostrar que el pipeline **no necesita Python en runtime**. El display list (IR
lenguaje‑agnóstico, JSON) se convierte en datos C estáticos, se compila con MSVC y el `.exe`
resultante pinta el PNG por sí solo.

**Resultado: SUCCESS.** `scene.exe` renderiza `scene.png` 400x300 con **maxdiff = 0** frente al
IR, sin abrir ningún JSON y sin Python en el PATH.

---

## Pipeline AOT

```
 display_list.json                  IR lenguaje-agnóstico (3/4 rects, painter's order)
        │
        │  emit_c.py                 ← Python SOLO en build time
        ▼
 scene_data.h                       RectOp kSceneOps[] + SCENE_WIDTH/HEIGHT/OP_COUNT
        │
        │  cl.exe /O2 /Brepro        ← compilador nativo (MSVC 19.51, x64)
        ▼
 scene.exe                          runtime genérico: recorre kSceneOps y pinta
        │
        │  .\scene.exe scene.png     ← ejecución nativa, sin Python
        ▼
 scene.png                          400x300
        │
        │  verify.py (Pillow)        ← Python SOLO en build time (verificación)
        ▼
 PASS (maxdiff 0, pixels exactos)
```

El header generado es todo el "programa" específico de la escena:

```c
typedef struct {
    float x, y, w, h;
    unsigned int rgba; /* 0xRRGGBBAA */
} RectOp;

#define SCENE_WIDTH  400
#define SCENE_HEIGHT 300
#define SCENE_OP_COUNT 3

static const RectOp kSceneOps[SCENE_OP_COUNT] = {
    {    0.0f,    0.0f,  400.0f,  300.0f, 0x101014FFu }, /* #101014 */
    {   20.0f,   20.0f,  120.0f,   80.0f, 0xE74C3CFFu }, /* #e74c3c */
    {   20.0f,  116.0f,  120.0f,   80.0f, 0x3498DBFFu }, /* #3498db */
};
```

`runtime_d2d.cpp` / `runtime.c` no contienen geometría ni colores: solo el bucle genérico
`for (i = 0; i < SCENE_OP_COUNT; ++i) FillRectangle(kSceneOps[i])`.

---

## Por qué esto es "compilado" y no un script

1. **El IR se convierte en datos C** en build time (`emit_c.py` → `scene_data.h`). No hay parser
   de JSON en el binario: un escaneo de bytes de `scene.exe` no encuentra `viewport`, `ops` ni los
   hex del IR (`e74c3c`, `#101014`).
2. **El `.exe` es un PE x64 nativo**. `dumpbin /dependents scene.exe` lista `d2d1.dll`,
   `ole32.dll`, `KERNEL32.dll`; ninguna DLL de Python.
3. **Se ejecutó con el PATH reducido a `C:\Windows\System32`**, demostrando que no hay Python ni
   venv disponible: `scene.exe scene_direct.png` produjo un PNG idéntico byte a byte a `scene.png`.
4. Python aparece únicamente en dos pasos de build: `emit_c.py` (genera C) y `verify.py`
   (compara el PNG con el IR usando Pillow). Ninguno se ejecuta al correr `scene.exe`.

---

## Backends

| Backend | Archivo | Estado | Notas |
|---|---|---|---|
| **Direct2D + WIC** (primario) | `runtime_d2d.cpp` | verificado | Se compila como C++ (`/TP`): el SDK de Windows solo expone vtables C++ para D2D (`d2d1.h` en modo C únicamente forward‑declara las interfaces; intentarlo da C2037). |
| **CPU software + WIC PNG** (fallback) | `runtime.c` | verificado | C puro (`/TC`), rasteriza rects con source‑over y codifica PNG con WIC. No usa D2D. |

Ambos backends producen **PNG byte‑idénticos** (mismo SHA‑256 `9e1bf828…`), lo que valida que el
IR y el bucle de pintado son equivalentes.

---

## Comandos exactos

Toolchain: `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`.

```powershell
# preparación (solo la primera vez, para build-time)
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install pillow

# build + run + verify (backend D2D primario)
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
#   emit:    <venv>\python.exe emit_c.py --ir display_list.json --out scene_data.h
#   compile: cl /nologo /O2 /W4 /wd4201 /Brepro /EHsc /TP runtime_d2d.cpp /Fe:scene.exe
#            /link /Brepro d2d1.lib windowscodecs.lib ole32.lib uuid.lib
#   run:     .\scene.exe scene.png
#   verify:  <venv>\python.exe verify.py --png scene.png --ir display_list.json

# fallback C puro
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Backend cpu -OutPng scene_cpu.png `
  -Report build_report_cpu.json -VerifyReport verify_report_cpu.json

# prueba fuerte (4º rect)
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 `
  -Ir proof\display_list_proof.json -OutPng proof\scene_proof.png `
  -Report proof\build_report_proof.json -VerifyReport proof\verify_report_proof.json `
  -ExtraPoint "260,60,#2ecc71"
```

`build.ps1` imprime cada comando, importa el entorno de `vcvars64.bat`, mide tiempos y escribe
`build_report.json`.

---

## Verificación (aceptación)

`verify.py` reconstruye la imagen esperada desde el IR, compara **todos** los píxeles y además
comprueba los puntos del contrato con tolerancia 8 por canal.

PNG 400x300. Valores medidos:

| Punto | Esperado | Obtenido | Resultado |
|---|---|---|---|
| (60,60) box a | `#e74c3c` | `#e74c3c` | PASS |
| (60,156) box b | `#3498db` | `#3498db` | PASS |
| (200,250) root | `#101014` | `#101014` | PASS |

- `maxdiff = 0` (0 píxeles fuera de tolerancia en toda la imagen).
- `scene.exe` se ejecutó directamente (no a través de Python) y produjo `scene.png` y
  `scene_direct.png` idénticos.
- SHA‑256 del PNG: `9e1bf828e0fe87f9cf63b5c3ee127a30e29042ae1b9d954b9406ddf7a838dbb9`.

### Prueba fuerte: el JSON dirige el binario

`proof/display_list_proof.json` añade un cuarto rect `#2ecc71` en (200,20,120,80):

| Evidencia | Base (3 ops) | Proof (4 ops) |
|---|---|---|
| Ops en el IR | 3 | 4 |
| `scene_data.h` generado | `SCENE_OP_COUNT 3` | `SCENE_OP_COUNT 4` |
| SHA‑256 de `scene.exe` (reproducible con `/Brepro`) | `c4bac2fc…d143f739` | `50b03ef4…7e7ef330` |
| SHA‑256 del PNG | `9e1bf828…` | `82c24a2b…` |
| Píxel (260,60) | `#101014` (fondo) | `#2ecc71` |

- Se reconstruyó el mismo `runtime_d2d.cpp` con las mismas flags; lo único que cambió fue el IR.
- El diff de imágenes da **exactamente 9600 píxeles** cambiados (120×80) con bounding box
  `(200,20,320,100)`: precisamente el rect nuevo, ni uno más.
- El backend CPU con el mismo IR produce un PNG byte‑idéntico al D2D (`82c24a2b…`).
- El `.exe` base se compiló dos veces y dio el mismo SHA‑256, así que la diferencia de hash no es
  un timestamp del linker.

Capturas/artefactos en `proof/`.

---

## Tiempos medidos (esta máquina)

Una corrida representativa; `cl.exe` varía con la carga del sistema. El reporte exacto de cada
build queda en `build_report*.json`.

| Paso | D2D (ms) | CPU fallback (ms) |
|---|---:|---:|
| emit | 143.5 | 148.5 |
| compile | 1049.5 | 565.8 |
| run | 92.4 | 75.7 |
| verify | 294.6 | 291.9 |
| **total** | **1689.9** | **1191.2** |

Compilador: `Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36260 for x64`
(Visual Studio 18 Community). El costo dominante es `cl.exe`; el render en sí es ~90 ms,
incluyendo arranque del proceso, inicialización COM/D2D y escritura del PNG.

---

## Estructura

```
exp08-aot/
├─ input.html, input.css, CONTRACT.md     copias del spec
├─ display_list.json                      IR de entrada (3 rects)
├─ emit_c.py                              IR JSON -> scene_data.h
├─ scene_data.h                           generado (datos "bakeados")
├─ runtime_d2d.cpp                        renderer genérico Direct2D + WIC (primario)
├─ runtime.c                              renderer genérico C puro + WIC (fallback)
├─ build.ps1                              orquesta emit -> cl -> exe -> verify
├─ verify.py                              comparación full-image con Pillow
├─ scene.exe, scene.png                   artefactos del build base
├─ scene_cpu.png                          salida del fallback C
├─ scene_direct.png                       salida ejecutando sin Python en PATH
├─ build_report*.json, verify_report*.json
└─ proof/
   ├─ display_list_proof.json             IR con el 4º rect
   ├─ scene_data_proof.h                  header generado con 4 ops (evidencia)
   ├─ scene_proof.png, scene_proof_cpu.png
   └─ build_report_proof*.json, verify_report_proof*.json
```

---

## Limitaciones

- **Windows únicamente**: Direct2D, WIC y COM. El fallback C también usa WIC para el PNG.
- **Solo rects**: el IR no tiene texto, gradientes, bordes, radios, sombras, clips, grupos de
  opacidad ni transformaciones. `emit_c.py` rechaza cualquier `type` distinto de `rect`.
- **Coordenadas enteras**: el fallback rasteriza sin antialiasing (centros de píxel); con
  geometría fraccionaria diferiría de D2D. El IR actual usa enteros.
- **Fondo opaco**: ambos backends limpian con el color de la primera op y asumen canvas opaco;
  alpha < 255 funciona en D2D, pero el fallback solo hace source‑over sobre destino opaco.
- **Sin GPU explícita**: D2D elige el backend (WARP/GPU) por su cuenta; no hay vertex buffers ni
  shaders propios.
- El build requiere MSVC, Python y Pillow. Nada de eso viaja al `.exe`.

## Siguiente paso

Sustituir D2D por un backend GPU real: que `emit_c.py` (o un emitter nuevo) produzca **vertex
buffers e índices** (dos triángulos por rect, color por vértice) y un shader trivial, en vez de
llamar a `FillRectangle`. Eso convierte el IR en buffers de GPU y permite portar el runtime a
D3D12/Vulkan/Metal sin cambiar el IR. Después: atlas de glifos para texto, pila de clips/opacidad
y comparación pixel a pixel contra los experimentos previos (exp01/exp04).
