# exp02-native-bindings — HTML/CSS nativo desde Python

## Estado: **PARTIAL**

El pipeline completo funciona y las 4 verificaciones pasan, pero **no hay render GPU**:
esta máquina no tiene ningún ICD de OpenGL registrado y `moderngl` no puede crear un
contexto 3.3. El render se hizo con el fallback autorizado de Pillow (CPU). El código
OpenGL 3.3 está implementado y se ejecuta primero; falla con el error exacto:

```
Exception: wglCreateContextAttribsARB not found
```

Diagnóstico: WGL legacy sólo entrega `GL_RENDERER = GDI Generic`, `GL_VERSION = 1.1.0`
(OpenGL software de Microsoft). La clave `HKLM\SOFTWARE\Microsoft\Windows NT\
CurrentVersion\OpenGLDrivers` no existe, así que el driver Intel no expone ICD. No se
modificó nada fuera del directorio del experimento.

## Combinación exacta

| Componente | Versión | Motor |
|---|---|---|
| Python | 3.12.10 (venv) | — |
| selectolax | 1.0.2 | Lexbor 3.1, C (`cp312-win_amd64`) |
| lightningcss | 0.3.2 | Rust, wheel `cp37-abi3-win_amd64` (`lightningcss.pyd`) |
| moderngl | 5.13.0 | OpenGL, vía glcontext 3.1.0 |
| Pillow | 13.0.0 | raster/encode/verificación |

Sí existe wheel de `lightningcss` para Python 3.12: **no fue necesario el fallback a
tinycss2**. Los parsers nativos están realmente en uso: `lightningcss.process_stylesheet`
minifica y normaliza la hoja (5 reglas), y `selectolax.lexbor` construye el árbol.

## Cobertura por etapa

1. **HTML parse** — `selectolax.lexbor` (Lexbor, C) → árbol de elementos con tag/attrs.
2. **CSS parse** — `lightningcss` (Rust) valida/minifica; de su salida se extraen reglas
   y declaraciones. Prueba: `body{background:#0f0f12;margin:0}#root{...}` (5 reglas).
3. **Cascade** — implementada aquí: matching tag/id/class y especificidad
   `(ids, clases, tags)`; orden estable por orden de aparición. 6 matches en el input.
4. **Layout block** — implementado aquí: body viewport-sized; `#root` 400x300 en (0,0)
   con padding 20; `.box` 120x80; `margin-top:16px` de `.b` produce y=116.
   Simplificación: `width`/`height` se tratan como border-box (necesario para que el
   rect de `#root` mida 400x300 con padding 20).
5. **IR** — `display_list.json` conforme al contrato. Se emite un op extra de fondo de
   body (`#0f0f12`) antes de los 3 obligatorios, permitido por el contrato.
6. **Render** — `moderngl` FBO 400x300 + quad por rect + lectura de píxeles (código
   listo, no ejecutable aquí por falta de contexto); fallback Pillow a `render.png`.
7. **Verificación** — Pillow sólo para abrir/muestrear el PNG.

Ops emitidos (orden painter's): `#0f0f12` 0,0,400x300 → `#101014` 0,0,400x300 →
`#e74c3c` 20,20,120x80 → `#3498db` 20,116,120x80.

## Verificación (ejecutada)

```
[PASS] size 400x300: (400, 300)
[PASS] pixel (60,60) = #e74c3c: rgb(231, 76, 60)
[PASS] pixel (60,156) = #3498db: rgb(52, 152, 219)
[PASS] pixel (200,250) = #101014: rgb(16, 16, 20)
```

## Tiempos (ms, ejecución en caliente, `run.py`)

| Etapa | Con intento GL (default) | Sólo CPU (`--cpu`) |
|---|---:|---:|
| html_parse | 0.73 | 0.85 |
| css_native (lightningcss) | 0.66 | 0.73 |
| cascade | 0.07 | 0.05 |
| layout | 0.04 | 0.04 |
| display_list + JSON | 0.85 | 0.90 |
| render | 94.90 | 24.65 |
| verify | 2.89 | 2.65 |
| **TOTAL** | **100.29** | **30.00** |

El render por defecto incluye ~71-80 ms del intento fallido de crear el contexto WGL
(el total varía ~100-112 ms entre ejecuciones por ese probe).
La rasterización Pillow real es <1 ms; el resto es el primer `save()` PNG (~23 ms en
caliente) y overhead de arranque. La primera ejecución absoluta añade ~60-90 ms de
imports/init (selectolax, lightningcss).

## Archivos

- `engine.py` — parse HTML/CSS nativo, cascade, layout block, display list.
- `render.py` — render OpenGL 3.3 (moderngl) con fallback Pillow, verificación.
- `run.py` — orquestador, tiempos, `display_list.json`, `render.png`, `run_report.json`.
- `input.html` / `input.css` — copiados sin cambios del spec.
- `requirements.txt` — dependencias fijadas.

Total Python: 294 líneas. Ejecutar:

```powershell
.venv\Scripts\python.exe run.py          # intenta GPU, cae a Pillow
.venv\Scripts\python.exe run.py --cpu    # omite el probe GL (30 ms)
```

## Problemas encontrados

1. **Sin OpenGL por hardware** — no hay ICD registrado; `moderngl.create_standalone_context(require=330)`
   lanza `wglCreateContextAttribsARB not found`. Es limitación del entorno, no del código.
2. **`margin-top` longhand** — el primer layout ignoraba longhands al aplicar el
   shorthand; corregido con override top/right/bottom/left (b pasó de y=100 a y=116).
3. **`lightningcss.unused_symbols`** — en 0.3.2 no eliminó una regla `.unused` en la
   prueba rápida; no se usa en el pipeline. El parseo/minificado sí funciona.
4. **Buffer GL** — el quad debía empaquetarse con `struct.pack("<8f", ...)`; bytes
   literales `\x01` no son el float 1.0.

## Recomendación

- **Parsers nativos: sí.** selectolax (Lexbor) y lightningcss son rápidos, con wheels
  directos para 3.12 y sin toolchain. Son una base sólida para el front-end del pipeline.
- **Cascade/layout propios: viables** para el subconjunto del contrato (<1 ms), pero es
  donde está casi todo el trabajo real si se amplía el CSS.
- **GPU en esta máquina: no con moderngl/OpenGL.** Para Windows headless conviene un
  backend nativo sobre D3D11 (p. ej. ANGLE EGL/GLES o wgpu, como exp04) en vez de
  depender del ICD OpenGL del driver. Si se insiste en OpenGL, hace falta registrar un
  ICD (implica tocar el sistema, fuera de este experimento).
- Este experimento sirve como **prueba de concepto y baseline de tiempos de parse**, no
  como prueba de render GPU: el PNG válido se produjo por CPU.
