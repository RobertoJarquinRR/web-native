# exp05-v0-core — núcleo reutilizable HTML+CSS → display list → render nativo

Paquete Python que consolida lo mejor de exp01 (layout de bloque), exp03
(cascada semántica con cssselect2) y exp04 (renderer offscreen wgpu) en una
librería con fronteras estables entre etapas. **Sin navegador, WebView, Skia ni
Electron**: parseo con html5lib/tinycss2/cssselect2, layout propio, IR JSON
lenguaje-agnóstico y rasterizado con wgpu-native o PIL.

Estado: **SUCCESS**. Contrato de `spec/CONTRACT.md` verificado en ambos backends
con `maxdiff = 0` en los tres píxeles obligatorios. `pytest`: **8 passed**.

## Versiones e instalación

| Componente | Versión |
|---|---|
| Python | 3.12.10 (`py -3.12`) |
| html5lib | 1.1 |
| tinycss2 | 1.5.1 |
| cssselect2 | 0.10.1 |
| wgpu (wgpu-py + wgpu-native) | 0.33.0 |
| Pillow | 13.0.0 |
| pytest | 9.2.0 |

```powershell
cd .\exp05-v0-core
py -3.12 -m venv .venv
.venv\Scripts\python.exe -m pip install --upgrade pip
.venv\Scripts\python.exe -m pip install -r requirements.txt
```

`requirements.txt` incluye pytest porque el test de contrato forma parte del
entregable. El paquete base solo necesita html5lib, tinycss2, cssselect2, wgpu y
Pillow; `import webnative` no carga wgpu ni PIL (verificado). No hace falta
instalar el paquete: se ejecuta desde la raíz de `exp05-v0-core`; para importarlo
desde otro directorio, añade esa carpeta a `PYTHONPATH`.

## Uso

```powershell
# auto: intenta wgpu y cae a cpu si no hay adapter; imprime el backend elegido
.venv\Scripts\python.exe -m webnative input.html --out render-auto.png --ir display_list.json --backend auto

# GPU explícita
.venv\Scripts\python.exe -m webnative input.html --out render-wgpu.png --ir display_list.json --backend wgpu

# CPU explícita (referencia / fallback)
.venv\Scripts\python.exe -m webnative input.html --out render-cpu.png --ir display_list.json --backend cpu
```

Opciones: `--width` y `--height` (default 400x300), `--out` (default
`render.png`), `--ir` (default `display_list.json`), `--backend`
`auto|wgpu|cpu` (default `auto`).

Salida real de `--backend auto` en esta máquina:

```
backend = wgpu (Microsoft Basic Render Driver / D3D12 / CPU)
```

Tests:

```powershell
.venv\Scripts\python.exe -m pytest tests -q
.venv\Scripts\python.exe tests\test_contract.py   # runner sin pytest
```

## Arquitectura y mapa de módulos

```
input.html ──html5lib──▶ tree.Node
input.css  ──tinycss2──▶ css_parse.Rule (selector compilado por cssselect2)
                               │
                     cascade.compute_styles()  (especificidad, orden,
                               │                !important, style="", herencia)
                               ▼
                  dict[Node, ComputedStyle]  (box model + color + tipografía)
                               │
                     layout.layout_tree()      (bloque, border-box, height auto)
                               ▼
                         LayoutBox (geometría resuelta)
                               │
                     layout.to_display_list()
                               ▼
                  DisplayList [RectOp, ...] ──to_json()──▶ display_list.json
                               │
                 renderer.Renderer.render()
                    ├── wgpu_renderer  (wgpu-native, offscreen)
                    └── cpu_renderer   (Pillow, referencia)
                               ▼
                           PIL.Image ──▶ PNG
```

| Archivo | Responsabilidad | Líneas de código |
|---|---|---:|
| `webnative/tree.py` | `Node`/`Element` mínimo: tag, attrs, children, `text` reservado | 17 |
| `webnative/html_parse.py` | html5lib → `Node`; recolección de `<link>`/`<style>` en orden | 33 |
| `webnative/css_parse.py` | tinycss2 → `Rule`; shorthands margin/padding; selector compilado | 46 |
| `webnative/cascade.py` | matcher cssselect2 → `ComputedStyle` por nodo | 93 |
| `webnative/layout.py` | layout de bloque → `LayoutBox`; pintado pre-orden → ops | 71 |
| `webnative/ir.py` | `Op`, `RectOp`, registro de ops, `DisplayList` JSON | 59 |
| `webnative/renderer/__init__.py` | protocolo `Renderer`, `create_renderer`, colores | 41 |
| `webnative/renderer/wgpu_renderer.py` | pipeline WGSL + offscreen readback | 95 |
| `webnative/renderer/cpu_renderer.py` | rasterizado PIL de referencia | 20 |
| `webnative/cli.py` | orquestación, timings `perf_counter`, fallback auto | 92 |
| `webnative/__init__.py` | API pública, `__version__` | 17 |
| `webnative/__main__.py` | entrada `python -m webnative` | 2 |
| **Total** | | **586** |

Las capas puras (`tree`, `html_parse`, `css_parse`, `cascade`, `layout`, `ir`)
no importan PIL ni wgpu. Solo `cli` conoce a la vez pipeline y renderers.

## IR y contrato

`DisplayList.to_dict()` produce exactamente:

```json
{
  "viewport": {"width": 400, "height": 300},
  "ops": [
    {"type": "rect", "x": 0, "y": 0, "w": 400, "h": 300, "fill": "#0f0f12"},
    {"type": "rect", "x": 0, "y": 0, "w": 400, "h": 300, "fill": "#101014"},
    {"type": "rect", "x": 20, "y": 20, "w": 120, "h": 80, "fill": "#e74c3c"},
    {"type": "rect", "x": 20, "y": 116, "w": 120, "h": 80, "fill": "#3498db"}
  ]
}
```

La op de `body` (`#0f0f12`) es la op extra que el CONTRACT permite expresamente
("fondo de body, etc."). Las tres ops obligatorias conservan el orden painter's
`root → a → b`; `tests/test_contract.py` lo comprueba como subsecuencia. Las
coordenadas del JSON son enteras (`round` en `RectOp.to_dict`); los floats
internos permiten añadir subpíxeles/fracciones sin cambiar el contrato.

## Verificación de aceptación

| Check | Resultado |
|---|---|
| IR con root/a/b en orden painter's | PASS (4 ops; extra body) |
| `render-wgpu.png` 400x300 | PASS |
| `render-cpu.png` 400x300 | PASS |
| (60,60) = #e74c3c | PASS, maxdiff 0 en ambos |
| (60,156) = #3498db | PASS, maxdiff 0 en ambos |
| (200,250) = #101014 | PASS, maxdiff 0 en ambos |
| `--backend auto` elige e imprime wgpu | PASS |
| `tests/test_contract.py` | 8 passed |
| Import sin wgpu/PIL/navegador | PASS |
| Paquete ≤600 líneas de código | 586 (sin blancos; 718 físicas con formato) |

Reproducción:

```powershell
.venv\Scripts\python.exe -m pytest tests -q
.venv\Scripts\python.exe -m webnative input.html --out render-wgpu.png --ir display_list.json --backend wgpu
.venv\Scripts\python.exe -m webnative input.html --out render-cpu.png --ir display_list.json --backend cpu
```

## Tiempos medidos (`time.perf_counter`)

CLI de una sola ejecución, proceso frío, Windows, adapter
**Microsoft Basic Render Driver / D3D12 / CPU (WARP)**. La inicialización de
wgpu-native y la compilación perezosa del shader dominan el primer frame.

| Etapa | CPU (ms) | wgpu (ms) |
|---|---:|---:|
| html (html5lib + hojas) | 16.73 | 16.63 |
| css (tinycss2 + selectores) | 1.84 | 1.47 |
| cascade (cssselect2) | 0.51 | 0.48 |
| layout | 0.05 | 0.04 |
| IR / JSON en memoria | 0.02 | 0.02 |
| **pipeline puro** | **19.18** | **18.66** |
| init backend (`ensure_ready`) | 24.10 (import PIL) | 1751.99 (adapter/device/pipeline) |
| render + readback | 0.50 | 631.13 (incluye compilación de shader) |
| guardar PNG | 43.84 | 29.01 |
| **wall total** | **88.96** | **2431.97** |

Con el renderer ya inicializado y el pipeline compilado, el mismo frame tarda
**CPU 0.73 ms / 0.51 ms** y **wgpu 580.88 ms / 4.00 ms** (frame 1 vs frame 2).
Por eso `WgpuRenderer` cachea device y pipeline: el coste de 1.75 s + 0.63 s es de
una sola vez por proceso; en un bucle real el frame wgpu cae a ~4 ms.

## Puntos de extensión

1. **TextOp sin reescribir el IR.** `Op` es una clase base con `type` y
   `to_dict()`, y `register_op` mantiene el registro que usa
   `DisplayList.from_dict`. Para añadir texto:
   ```python
   @register_op
   @dataclass(frozen=True)
   class TextOp(Op):
       x: float; y: float; text: str; fill: str; font_size: float
       type: ClassVar[str] = "text"
       def to_dict(self): ...
       @classmethod
       def from_dict(cls, data): ...
   ```
   `Node.text` y `LayoutBox.text` ya reservan el dato. Falta un pase de line
   breaking/shaping antes de `paint()` y soporte en cada renderer (PIL `ImageFont`
   en CPU; atlas de glyphs en wgpu).
2. **Flexbox.** `ComputedStyle.display` ya conserva `flex`, `LayoutBox.kind`
   distingue el tipo de caja y `layout_tree` despacha por `display`. Añadir
   `_layout_flex()` y llamarlo cuando `style.display == "flex"`; el resto del
   pipeline (IR, JSON, renderers) no cambia. `gap`, `justify-content`, etc. se
   leen de `ComputedStyle.raw`.
3. **Más CSS.** El parser conserva declaraciones crudas en `ComputedStyle.raw`.
   Se pueden añadir bordes, radios, sombras, gradientes, `opacity`, unidades
   (`%`, `rem`, `em`) y `@media` sin tocar HTML/IR: normalizar en `cascade` y
   añadir ops (`RoundRectOp`, `BorderOp`, `ImageOp`, `TextOp`).
4. **Nuevos backends.** Implementar `render(display_list, width, height) ->
   PIL.Image` y añadir una rama en `create_renderer`. El protocolo no exige
   herencia. Un backend de ventana puede compartir `DisplayList` y pipeline.
5. **Rendimiento.** `ensure_ready()` separa init de frame; se puede ampliar con
   caché de texturas por tamaño, batching multi-draw para muchas ops y
   pipeline cache en disco. `build()` devuelve timings por etapa para perfilar.
6. **Carga de CSS.** `stylesheet_texts()` resuelve rutas locales y respeta el
   orden de documento; el loader remoto (`http(s)`) está deliberadamente fuera
   de v0 y es el punto natural para añadir caché/red.

## Limitaciones de v0 (explícitas)

- Solo layout de bloque; `inline`/texto se omiten (no hay TextOp todavía).
- No hay colapso de márgenes; el apilado es secuencial como en exp01.
- Solo `px` y números sin unidad. `auto`/`%`/`rem` se tratan como auto.
- Fondo = color sólido. Sin bordes, radios, gradientes, imágenes, opacidad ni clip.
- Alfa entre 0 y 1 se ignora (se pinta opaco); alfa 0 = sin fondo.
- Se pintan `body` y descendientes; el fondo de `html` no se propaga.
- Pseudo-elementos y at-rules se ignoran; pseudo-clases se evalúan estáticamente
  (p. ej. `:hover` nunca coincide).
- Sin hojas remotas ni `@import`.

## Estructura del experimento

```
exp05-v0-core/
├── input.html, input.css        # copiados sin cambios de spec/
├── display_list.json            # IR del contrato
├── render-auto.png              # salida de --backend auto (wgpu)
├── render-wgpu.png              # salida GPU
├── render-cpu.png               # salida CPU
├── requirements.txt
├── README.md
├── webnative/
│   ├── __init__.py  __main__.py
│   ├── tree.py  html_parse.py  css_parse.py  cascade.py
│   ├── layout.py  ir.py  cli.py
│   └── renderer/
│       ├── __init__.py  wgpu_renderer.py  cpu_renderer.py
└── tests/
    └── test_contract.py
```
