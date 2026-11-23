# exp01-pure-python

Prueba mínima end-to-end: **HTML + CSS -> display list (IR) -> render offscreen**,
sin navegador, WebView, Skia ni Electron.

## Estado: PARTIAL

El pipeline completo funciona y cumple el contrato (`display_list.json` y
`render.png` verificados), pero **esta máquina no puede crear un contexto OpenGL
3.3+ por hardware**. El render se ha validado con **Mesa llvmpipe (OpenGL 4.6
software)** a través de moderngl; además existe y está probado un fallback de
rasterización con Pillow. El estado es PARTIAL porque el backend efectivo no es
una GPU real, no porque falle ninguna etapa.

Error exacto del driver del sistema:

```
Exception: wglCreateContextAttribsARB not found
```

## Versiones

| Componente | Versión |
|---|---|
| Python | 3.12.10 |
| html5lib | 1.1 |
| tinycss2 | 1.5.1 |
| moderngl | 5.13.0 |
| glcontext | 3.1.0 (dependencia de moderngl) |
| Pillow | 13.0.0 |
| Mesa llvmpipe (opcional) | 26.3.1, `release-msvc` |

## Cobertura por etapa

| # | Etapa | Implementación | Estado |
|---|---|---|---|
| 1 | Parse HTML | `html5lib` -> element tree -> `Node` propio | OK |
| 2 | Parse CSS + cascada | `tinycss2` + cascada y selectores propios | OK |
| 3 | Layout block | propio: padding, márgenes, apilado | OK |
| 4 | Display list IR | JSON exacto según `CONTRACT.md` | OK |
| 5 | Render offscreen | moderngl + FBO 400x300 -> `render.png` | OK vía Mesa llvmpipe |
| 6 | Verificación | Pillow solo para leer/muestrear el PNG | 5/5 PASS |

La cascada implementa selectores compuestos (`tag`, `#id`, `.class`),
especificidad `(ids, clases, tags)`, orden de origen y shorthand
`margin`/`padding`. `background` no se hereda: el fondo del `body` se emite como
op extra y el de `#root` lo cubre.

El layout usa **border-box**: el `width/height` declarado es el rectángulo
exterior. Es lo que exige el contrato para `#root` (400x300 con `padding: 20px`;
en CSS estándar content-box serían 440x340). El contenido de `#root` empieza en
(20,20), las cajas miden 120x80 y el `margin-top: 16px` de `.b` la coloca en
y=116. No se implementa colapso de márgenes.

## Geometría obtenida

| op | x | y | w | h | fill |
|---|---|---|---|---|---|
| body (extra) | 0 | 0 | 400 | 300 | `#0f0f12` |
| root | 0 | 0 | 400 | 300 | `#101014` |
| box a | 20 | 20 | 120 | 80 | `#e74c3c` |
| box b | 20 | 116 | 120 | 80 | `#3498db` |

Orden painter's: body, root, a, b. Las 3 ops obligatorias conservan el orden
relativo pedido.

## Tiempos (ejecución de referencia, Mesa llvmpipe)

| Etapa | ms |
|---|---|
| 1 parse HTML | 156.5 |
| 2 cascada + estilos computados | 10.7 |
| 3 layout | 0.01 |
| 4 display list + JSON | 0.8 |
| 5 render (contexto + draw + readback + PNG) | 280.5 |
| 6 verificación | 2.1 |
| **Total** | **502.0** |

Los tiempos varían ±10% entre ejecuciones.

Matices medidos aparte: el primer `import html5lib + tinycss2` cuesta ~207 ms
(por eso el parse inicial marca ~160-180 ms; un parse en caliente tarda 0.7 ms),
y crear el contexto GL con llvmpipe cuesta ~170 ms.

## Verificación

```
[PASS] IR has expected 3 rects in painter's order (4 ops total)
[PASS] PNG size is 400x300 (got (400, 300))
[PASS] pixel (60,60)   ~= #e74c3c (box a)  got #e74c3c delta=0
[PASS] pixel (60,156)  ~= #3498db (box b)  got #3498db delta=0
[PASS] pixel (200,250) ~= #101014 (root)   got #101014 delta=0
```

El PNG tiene exactamente 3 colores y 120000 píxeles:
`#101014` x100800 + `#e74c3c` x9600 + `#3498db` x9600. No está en blanco.

## Problemas encontrados

1. **Sin OpenGL por hardware.** El ICD de Intel existe (`ig4icd64.dll`) pero falta
   su registro en `HKLM\...\OpenGLDrivers`; `opengl32.dll` cae al renderer GDI
   genérico (OpenGL 1.1), que no expone `wglCreateContextAttribsARB`. No es una
   sesión RDP (la sesión es `console`), es el driver de 2016 sin ICD registrado.
   No se tocó el registro ni se instalaron drivers (fuera del alcance).
2. **Mesa como workaround.** Se descargó `mesa3d-26.3.1-release-msvc.7z`
   (66 MB) y se copiaron `opengl32.dll`, `libgallium_wgl.dll` y `dxil.dll` a
   `.mesa/runtime/x64/`. `py7zr` no soporta el filtro BCJ2 del archivo; hubo que
   usar `7zr.exe`.
3. **Orden de carga de DLLs.** glcontext cachea el `opengl32.dll` del sistema
   tras el primer intento fallido. Por eso el reintento con Mesa se hace en un
   **proceso nuevo** (`EXP01_MESA=1`), cargando la DLL con ruta absoluta y
   `SetDllDirectoryW` para que resuelva `libgallium_wgl.dll`.
4. **Tamaño.** Las DLLs de Mesa ocupan ~63 MB y no son una dependencia Python.
   Si se borra `.mesa/`, `run.py` cae a Pillow, marca PARTIAL y conserva el
   error original en `report.json` / `report-pillow-fallback.json`.

## Limitaciones del motor (a propósito)

- Solo elementos block; sin inline, texto, float, flex ni grid.
- Sin colapso de márgenes, sin `overflow`/clip, sin `box-sizing` configurable.
- Selectores compuestos simples; sin combinadores, pseudoclases ni inline style.
- Sin herencia real (no hay propiedades heredables en este subset).
- Un color de fondo plano por caja; sin alpha blending, bordes, radios ni sombras.
- Sin unidades relativas (`%`, `em`, `rem`); solo `px` y `0`.

## Recomendación

- **El contrato IR funciona** y separa bien las etapas; es un buen punto de
  comparación entre lenguajes. Mantener `display_list.json` como interfaz.
- El coste real de un motor así está en **CSS layout**, no en el render: este
  caso es trivial, pero implementar block flow, márgenes colapsados, inline y
  texto requiere mucho más que 300 líneas. Hay que acotar el subset por escrito.
- **moderngl es suficiente y muy simple** para el renderer (un shader, un quad,
  un FBO). El riesgo en Windows es el driver: para CI/headless conviene incluir
  Mesa llvmpipe como fallback explícito, o exigir GL 3.3+ real.
- Python + html5lib/tinycss2 sirve como baseline y oráculo de corrección; el
  rendimiento de parse/cascada es irrelevante a esta escala. Para un motor real,
  Rust/C++ aportarían velocidad y control, pero el diseño del IR no cambia.
- Siguiente experimento sugerido: mismo contrato con layout en Rust o C++ y
  comparar tiempos de layout/render, no solo de parse.

## Ejecución

```powershell
py -3.12 -m venv .venv
.venv\Scripts\python.exe -m pip install --upgrade pip
.venv\Scripts\python.exe -m pip install -r requirements.txt
.venv\Scripts\python.exe run.py
```

Salidas: `display_list.json`, `render.png`, `report.json`.

Nota: `.mesa/runtime/x64/` es opcional. Si está presente, `run.py` detecta el
fallo de WGL y reintenta automáticamente en un proceso nuevo con llvmpipe.
