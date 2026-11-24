# exp03 — Etapa semántica CSS real con cssselect2

**Estado: PARTIAL** — el pipeline completo (HTML → CSS → cascada → layout → IR → PNG)
funciona y la verificación de píxeles da **PASS**, pero el render con GPU no pudo
crear contexto OpenGL en esta máquina (solo existe el renderer *Microsoft GDI
Generic 1.1*). El PNG se generó con el fallback de Pillow; el código del renderer
moderngl está implementado y se ejecuta automáticamente donde haya OpenGL ≥ 3.3.

Este experimento valida la hipótesis del proyecto: **reutilizar el frontend CSS de
WeasyPrint (html5lib + tinycss2 + cssselect2) da semántica de selectores real,
specificity y orden de cascada, sin arrastrar el motor de WeasyPrint ni sus DLLs
nativas**.

`run.py` tiene **299 líneas** (prueba mínima end-to-end).

---

## Cómo ejecutar

```powershell
py -3.12 -m venv .venv
.venv\Scripts\python.exe -m pip install -r requirements.txt
.venv\Scripts\python.exe run.py
```

Salida: `display_list.json` + `render.png`. Códigos de salida:

| código | significado |
|---|---|
| `0` | PASS (render GPU + verificación) |
| `2` | PARTIAL (fallback CPU + verificación PASS) |
| `1` | FAIL |

Comprobaciones adicionales:

```powershell
.venv\Scripts\python.exe cascade_selftest.py   # id/clase/tag, empate, !important
.venv\Scripts\python.exe gl_probe.py           # renderer OpenGL real de la máquina
.venv\Scripts\python.exe weasyprint_probe.py   # ¿motor WeasyPrint usable?
```

WeasyPrint se instaló solo para el sondeo: `.venv\Scripts\python.exe -m pip install weasyprint`.

---

## Resultado visual

![render](render.png)

---

## Archivos

| archivo | qué es |
|---|---|
| `run.py` | pipeline end-to-end (299 líneas) |
| `input.html`, `input.css` | copia **sin cambios** del spec compartido |
| `display_list.json` | IR generada (4 ops: fondo de body + 3 rects obligatorias) |
| `render.png` | 400×300, generado por el fallback Pillow |
| `cascade_selftest.py` | prueba extra de specificity / orden / `!important` |
| `gl_probe.py` | diagnóstico WGL/OpenGL de la máquina |
| `weasyprint_probe.py` | intento de importar y usar WeasyPrint |
| `requirements.txt` | dependencias del pipeline |

---

## Librerías y versiones

| componente | versión | papel |
|---|---|---|
| Python | 3.12.10 | venv con `py -3.12` |
| **html5lib** | **1.1** | parse HTML5 → `xml.etree` (treebuilder `etree`) |
| **tinycss2** | **1.5.1** | parse CSS → qualified rules + declarations |
| **cssselect2** | **0.10.1** | `ElementWrapper` + `Matcher`: selectores y specificity reales |
| moderngl | 5.13.0 | render OpenGL standalone (intentado) |
| glcontext | 3.1.0 | backend WGL de moderngl |
| Pillow | 13.0.0 | fallback de render + verificación/encode |
| weasyprint | 70.0 | **solo sondeo**: import falla por DLLs nativas |

---

## Cobertura por etapa

| # | etapa | implementación | cobertura |
|---|---|---|---|
| 1 | Parse HTML | html5lib 1.1 | algoritmo de tree-construction HTML5 completo: `html/head/body` implícitos, 7 elementos; resuelve `<link rel=stylesheet>` locales y `<style>` |
| 2 | Parse CSS | tinycss2 1.5.1 | stylesheets, qualified rules, declarations, comentarios/whitespace, `!important`; **no** evalúa at-rules (`@media`, `@import`) |
| 3 | **Cascada semántica** | cssselect2 0.10.1 | `ElementWrapper.from_html_root` + `Matcher.add_selector(CompiledSelector, payload)`; `matcher.match()` devuelve `(specificity, order, pseudo_element, payload)`; se aplica `(important, specificity, order, decl_index)` ascendente. Herencia limitada a `color`, `font-size`, `font-family`. Sin UA stylesheet. Pseudo-elementos omitidos |
| 4 | Layout | propio | block flow; `width/height` explícitos, `width:auto`, `height:auto` = suma de hijos; `margin`/`padding` shorthand 1–4 valores + longhands. Sin colapso de márgenes, bordes, inline ni texto |
| 5 | Display list | JSON | ops `rect` en orden painter; fondo de body como op extra permitida |
| 6 | Render | moderngl 5.13 | FBO 400×300, shader GLSL 330, 2 triángulos por rect, `read` + flip vertical. **No disponible en esta máquina** (ver hallazgo) |
| 7 | Verificación | Pillow 13 | tamaño 400×300, geometría/orden de las 3 ops, 3 píxeles con tolerancia ±8/canal |

---

## La etapa semántica: evidencia

cssselect2 hace el matching de verdad; no hay selectores hechos a mano. Para cada
elemento se recogen las declaraciones de todos los selectores que matchean y se
resuelve el ganador por propiedad con la clave
`(important, specificity, order, declaration_index)`, aplicando de menor a mayor.

Salida real de `run.py` (extracto de las 13 declaraciones resueltas):

```text
div#root  width     =400px     spec=(1, 0, 0) order=2
div#root  padding   =20px      spec=(1, 0, 0) order=2
div.a.box background=#e74c3c   spec=(0, 1, 0) order=4
div.b.box background=#3498db   spec=(0, 1, 0) order=5
div.b.box margin-top=16px      spec=(0, 1, 0) order=5
```

El input compartido no tiene propiedades en conflicto, así que
`cascade_selftest.py` añade un caso sintético que sí ejercita las decisiones:

| elemento | reglas en conflicto | resultado | por qué |
|---|---|---|---|
| `p#main.note` | `p{color:red}`, `.note{color:blue}`, `#main{color:green}`, `.note{color:yellow}`, `#main{color:cyan!important}` | `#00ffff` | `!important` gana; sin él ganaría el id |
| `p.note` | `p{color:red}`, `.note{color:blue}`, `.note{color:yellow}` | `#ffff00` | misma specificity de clase: gana la regla posterior |
| ambos | `p{background:#111}`, `p{background:#222}` | `#222222` | empate exacto: gana el orden posterior |

Resultado: **PASS**.

---

## Layout mínimo y geometría

Modelo block simple, sin colapso de márgenes:

- caja border-box = `margin` + borde (no implementado) + `padding` + contenido;
- `x = origen_x + margin-left`, `y = origen_y + margin-top`;
- el flujo avanza `y += alto + margin-bottom` por hijo;
- `height:auto` = fondo del último hijo + padding.

**Decisión deliberada:** el spec pide `#root` de 400×300 **con** 20 px de padding,
así que `width/height` explícitos se interpretan como *border-box*. En CSS estándar
(content-box) la caja exterior sería 440×340 y no coincidiría con el contrato. Es
la única desviación del modelo CSS y está aislada en `build_layout`.

Geometría resultante (idéntica a `CONTRACT.md`):

| op | x | y | w | h | fill |
|---|---|---|---|---|---|
| body (extra) | 0 | 0 | 400 | 300 | `#0f0f12` |
| root | 0 | 0 | 400 | 300 | `#101014` |
| a | 20 | 20 | 120 | 80 | `#e74c3c` |
| b | 20 | 116 | 120 | 80 | `#3498db` |

`b.y = 20 (padding) + 80 (a) + 16 (margin-top) = 116`.

`display_list.json` cumple el contrato: la op de body es extra y las tres
obligatorias conservan su orden relativo (root → a → b).

---

## Verificación (Pillow, solo encode/sample)

```text
[verify] display list geometry
  OK   {'type': 'rect', 'x': 0, 'y': 0, 'w': 400, 'h': 300, 'fill': '#101014'}
  OK   {'type': 'rect', 'x': 20, 'y': 20, 'w': 120, 'h': 80, 'fill': '#e74c3c'}
  OK   {'type': 'rect', 'x': 20, 'y': 116, 'w': 120, 'h': 80, 'fill': '#3498db'}
  OK   mandatory ops present and in painter's order

[verify] PNG render.png: 400x300
  OK   ( 60, 60) box a  got=(231, 76, 60) expected=(231, 76, 60) maxdiff=0
  OK   ( 60,156) box b  got=(52, 152, 219) expected=(52, 152, 219) maxdiff=0
  OK   (200,250) root   got=(16, 16, 20) expected=(16, 16, 20) maxdiff=0
```

Los tres píxeles coinciden exactamente (maxdiff 0, tolerancia ±8).

---

## Timings (ms)

Ejecución representativa en esta máquina (Windows, Python 3.12.10):

| etapa | ms |
|---|---:|
| 1. parse HTML (html5lib) | 15.00 |
| 2. parse CSS (tinycss2) | 1.01 |
| 3. cascada (cssselect2) | 0.20 |
| 4. block layout | 0.13 |
| 5. display list | 0.62 |
| 6. render (intento WGL + fallback Pillow) | 112.01 |
| 7. verificación (Pillow) | 2.31 |
| **TOTAL** | **131.58** |

Observaciones: la etapa semántica completa (CSS + cascada) es **~1.2 ms**. El total
está dominado por el intento fallido de crear contexto WGL y por el primer uso de
`ImageDraw`. Medidas aisladas: intento WGL ~64 ms (varía entre ~60 y ~111 ms según
la ejecución) y primer `render_pillow` ~54 ms (inicialización de `ImageDraw`);
llamadas siguientes ~3–5 ms. En ejecuciones repetidas el total osciló entre
~131 y ~196 ms.

---

## Hallazgo: WeasyPrint completo NO es usable en esta máquina

`pip install weasyprint` instala **70.0** sin problema (es Python + cffi), pero el
import falla porque no existen las DLLs nativas de Pango/GObject/GTK:

```text
import weasyprint: FAILED
  OSError: cannot load library 'libgobject-2.0-0': error 0x7e.  Additionally,
  ctypes.util.find_library() did not manage to locate a library called 'libgobject-2.0-0'

import weasyprint.layout: FAILED
  OSError: cannot load library 'libgobject-2.0-0': error 0x7e. ...
```

WeasyPrint imprime además el aviso oficial: *“WeasyPrint could not import some
external libraries. Please carefully follow the installation steps…”*.

Cadena exacta: `weasyprint/__init__.py` → `css/__init__.py` →
`text/fonts.py` → `text/constants.py` → `text/ffi.py` →
`cffi.dlopen('libgobject-2.0-0')`. Sin un runtime GTK3 en `PATH`
(libgobject, libpango, harfbuzz, cairo, fontconfig, gdk-pixbuf) **no hay motor
WeasyPrint**. Habría que instalar el stack nativo GTK, y aun así usar el motor
completo iría contra la restricción del proyecto.

**Conclusión:** se confirma la estrategia de reutilizar solo su *frontend*:
html5lib + tinycss2 + cssselect2. (Nota de precisión: WeasyPrint 70 usa
`tinyhtml5` en lugar de `html5lib`, pero su pipeline CSS sigue siendo
tinycss2 + cssselect2; la etapa semántica validada es exactamente la misma.)

---

## Hallazgo: OpenGL moderno no disponible (GDI Generic 1.1)

```text
[6] moderngl FAILED: Exception: wglCreateContextAttribsARB not found
```

`gl_probe.py` confirma la causa:

```text
GL_VENDOR  : b'Microsoft Corporation'
GL_RENDERER: b'GDI Generic'
GL_VERSION : b'1.1.0'
wglCreateContextAttribsARB: None
```

La máquina tiene una Intel HD Graphics con driver 9.17.10.4459, pero la sesión WGL
solo expone el renderer software de Microsoft (OpenGL **1.1**), sin la extensión
`ARB_create_context`. moderngl exige OpenGL ≥ 3.3, por lo que no puede crear el
contexto standalone. No es un fallo del código: en una máquina con driver OpenGL
moderno, `render_moderngl()` se usa automáticamente y produce el mismo PNG.

Fallback aplicado: `render_pillow()` dibuja cada rect con `ImageDraw`, y la
verificación de píxeles sigue siendo válida (mismo color, misma geometría). Por eso
el estado es **PARTIAL**, no SUCCESS.

---

## Problemas y limitaciones

- **GPU ausente en esta máquina**: solo GDI Generic 1.1; sin OpenGL 3.3 no hay
  moderngl. No se intentó instalar Mesa/llvmpipe (sería rasterizado CPU disfrazado
  de GPU) ni ANGLE; el fallback estaba previsto por el enunciado.
- **Modelo de caja**: `width/height` explícitos como border-box para cuadrar con el
  contrato (CSS puro sería content-box → 440×340). No hay `box-sizing`.
- **Layout muy limitado**: sin colapso de márgenes, bordes, `min/max-width`, texto,
  inline, floats, flex/grid, posicionamiento ni overflow.
- **CSS limitado**: solo colores hex (`#rgb`/`#rrggbb`); no `rgb()`, nombres, HSL,
  `opacity`, gradientes ni shorthand `background`. Se ignoran at-rules y
  pseudo-elementos. Herencia solo de `color`/`font-size`/`font-family`.
- **Sin UA stylesheet**: los valores por defecto son vacío/0; `html`/`head` no se
  pintan. El fondo de `body` se emite como rect de viewport (op extra permitida),
  no como propagación real de canvas.
- **IR mínima**: solo ops `rect`.
- **WeasyPrint**: motor completo inutilizable sin GTK/Pango; no se usa.

---

## Recomendación

1. **Adoptar html5lib + tinycss2 + cssselect2 como frontend canónico.** La etapa
   semántica es barata (~0.2 ms aquí), usa el mismo matcher que WeasyPrint y evita
   reimplementar selectores, specificity y `!important`.
2. **No integrar WeasyPrint como motor**: exige GTK/Pango nativo, es pesado y va
   contra la restricción. Solo sus librerías de frontend aportan valor.
3. **Mantener el layout como módulo propio.** El layout block mínimo es suficiente
   para el contrato, pero el siguiente esfuerzo debe ir a valores computados
   (normalización de colores/longitudes), colapso de márgenes y inline/texto.
4. **Render**: conservar el camino moderngl y añadir un rasterizador software
   propio para Windows sin OpenGL 3.3 (RDP, VMs, drivers antiguos). En esta máquina
   el fallback Pillow es solo una prueba de geometría/color, no un renderer real.
5. **Siguiente experimento**: ampliar selectores y at-rules, inherit/initial/unset,
   `box-sizing`, y medir la cascada con un CSS grande para confirmar que cssselect2
   escala.
