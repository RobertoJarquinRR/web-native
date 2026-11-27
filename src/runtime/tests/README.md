# Runtime test suites

Los gates vivos se ejecutan con `wn-cli`:

```powershell
python tools\cli.py test all
python tools\cli.py test all --no-wgpu
python tools\cli.py test suite <nombre>
```

| Suite | Directorio | Cubre |
|---|---|---|
| `css` | `css/` | radios, borde uniforme, sombra, imagen; píxeles + campos vía `wnui-dump --json` |
| `fidelity` | `fidelity/` | E1: font-weight, `text-align`, `line-height`, `margin:auto`, flex; overrides de layout/estilo v7 |
| `dynamic` | `dynamic/` | `set_visible`, clone/append/remove, demo ABI |
| `input` | `input/` | caret, selección, clipboard, demo ABI |
| `offscreen` | `offscreen/` | readback, caché de imágenes, D3D11 vs wgpu |
| `text` | `text/` | contrato de texto (backend falso), medición DirectWrite, atlas, wrapping, documento |
| `blob` | `blob/` | loader `.wnui`, builtin vs archivo, píxeles (`max=0`) |
| `dump` | `dump/` | `wnui-dump` texto/JSON, golden y errores de archivo |

Todos los fixtures emiten el documento con `frontend-rust --emit-blob` desde
HTML/CSS y lo embeben como `builtin_blob.h`. No hay árbol JSON intermedio ni
cabeceras de nodos generadas.

## `text`

`contract_test.c` corre sobre `fake_text.c`, un backend determinista instalado con
`wn_text_test_use_backend()` (hook de test, fuera del ABI): la lógica de contrato
no toca DirectWrite en tiempo de ejecución. El binario, en cambio, se compila como
el resto de la suite: incluye `text_directwrite.cpp`, enlaza `dwrite.lib` y necesita
`out/fixture/builtin_blob.h` porque `wn_core.c` lo incluye para
`wn_document_builtin()`. Bloquea que `max_width` siga siendo solo una pista del
backend, el corte greedy con slices exactos, `set_text` (medida + geometría
pintada), el aislamiento de la caché de líneas por texto/tamaño/peso/ancho —todas
las comprobaciones usan la familia `"fake"`, así que la separación por familia no
se ejerce— y la supervivencia de entradas publicadas a una tormenta de fallos.
`text_block_test.c` fija el bloque derivado que comparten layout, paint e input
(alto de caja, leading medio, altura medida de una línea sin restricción, y el
espaciado/centrado vertical de las líneas pintadas). También fija la
bandera interna `measured`: con un backend cuyo `measure` falla pero cuyo
`shape` sigue funcionando, el bloque conserva la caja sintética para
layout/paint y el input vuelve a su fallback histórico `1.35 * font_size`.
`measure_test.c` es el
smoke del backend real: en Windows debe pasar; fuera de Windows, donde no hay
backend, reporta `SKIP`.

## `dump`

`dump/canonical.txt` es el golden de la proyección de texto del blob canónico
(960x640). Lo produce `src/runtime/tools/wn_dump.c` con el mismo loader del
runtime. El golden detecta cambios silenciosos del dump; **no es un contrato**:
la invariante primaria es el SHA-256 del `app.wnui` y el render idéntico al
builtin (`blob`). Si el formato del dump cambia a propósito, se regenera el
golden junto con el blob.

## `css`

`css/page/{card,control,none}.html` + `.css` son páginas reales; la suite las
compila con `frontend-rust --emit-blob`, fija sus tres hashes y comprueba los
campos del documento con `wnui-dump --json`. `verify_css.py` conserva además las
comprobaciones de píxeles contra `expected/control_legacy.png`, que no se
modifica: `control` demuestra que la ruta sin CSS v3 es idéntica a la de antes.

## Layout overrides (ABI v7)

`core/layout_override_test.c` corre dentro del suite `fidelity` con un blob
construido en memoria (sin GPU ni fixtures): fija tamaño y posición, comprueba
el rect resultante y la reconstrucción de la display list, restaura con
`WN_AUTO` (incluido un ancho porcentual y un par `right`/`bottom`), valida la
base de un clon y confirma que handles/valores inválidos y un nodo `static`
para `set_position` no cambian nada.

## Style overrides (ABI v7)

`core/style_override_test.c` corre con el backend de texto falso sensible a
familia/tamaño/peso (sin DirectWrite) y un documento construido en memoria:
fija que el color de texto llega a los vértices de glifo y hereda desde un
contenedor (con prioridad del override propio y reset por nodo/ancestro), que
el fondo cambia la pasada sólida de un nodo `static` y se restaura, que
familia/tamaño/peso cambian la medida y el layout y vuelven a la métrica
compilada, que la capa de familia recursiva llega a los descendientes sin tocar
la caja de una imagen, que un clon copia el override y su reset cae en su base
compilada, y que handles/valores/máscaras inválidos no cambian nada.

## Histórico

El piloto vivo es `benchmarks/pilot-media-library/` y se ejecuta con
`python tools\cli.py bench pilot`; el runtime no consume JSON intermedio.
