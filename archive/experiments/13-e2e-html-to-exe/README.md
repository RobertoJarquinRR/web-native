# exp13-html-to-exe — HTML/CSS real → binario nativo

Objetivo: probar la cadena completa del objetivo de webnative, sin atajos:
**HTML + CSS → IR → C + WGSL → .exe GPU**, sin navegador ni Python en runtime.

## Cadena

```
page/index.html + page/style.css
  → frontend v1 (exp09: html5lib + tinycss2 + cssselect2 + layout block/flex)
  → build/display_list.json          (23 ops rect, 480×320)
  → emit_c.py (exp10)                (baked vertices + WGSL)
  → build/scene_data.h               (138 vertices)
  → cl.exe (MSVC 19.51)              (/TC, wgpu-native v29.0.1.1)
  → build/webnative-html.exe         (ventana Win32 + swapchain)
  → build/render_aot.exe             (offscreen → PNG)
```

## Comandos

```powershell
# 1) HTML/CSS -> IR + render de referencia del frontend
cd .\exp09-v1-core
.\.venv\Scripts\python.exe -m webnative `
  .\exp13-html-to-exe\page\index.html `
  --out .\exp13-html-to-exe\build\render_frontend.png `
  --ir  .\exp13-html-to-exe\build\display_list.json `
  --backend wgpu --width 480 --height 320

# 2) IR -> C + compilar
cd .\exp13-html-to-exe\build
py -3.12 emit_c.py --ir display_list.json --out scene_data.h
cmd /c build.cmd

# 3) render offscreen + verificación
render_aot.exe render_aot.png
.\.venv\Scripts\python.exe verify.py
```

## Resultado

- 23 ops `rect`, 138 vértices, 1 draw call.
- El render del frontend (Python/wgpu) y el del binario AOT (C/wgpu-native)
  son **byte-idénticos**: `ImageChops.difference().getbbox() == None`,
  0 píxeles distintos en 480×320.
- 13/13 muestras de color exactas (dots, pill, cards, accents, footer).
- Ventana validada con `--frames 20`: exit 0, D3D12 (WARP).

## Limitación encontrada

Los flex items con `display:flex` y ancho automático miden el ancho completo
del contenedor (no hay shrink-to-fit). Por eso `.dots` declara `width: 52px`.
Está documentado en el README del escritorio; es el siguiente arreglo de
layout pendiente.

## Límites del runtime C

Solo ops `rect`. El texto (`text_run` + atlas de glifos) existe en la lane
Python (exp06/exp09/exp11) pero aún no está portado a `runtime_gpu.c` /
`runtime_window.c`. Ese es el siguiente paso para que una página con texto
compile a .exe.
