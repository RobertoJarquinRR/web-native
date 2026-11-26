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
| `fidelity` | `fidelity/` | E1: font-weight, `text-align`, `line-height`, `margin:auto`, flex |
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

`contract_test.c` corre contra `fake_text.c`, un backend determinista instalado
con `wn_text_test_use_backend()` (hook de test, fuera del ABI): sin DirectWrite y
sin blob de fixture. Bloquea que `max_width` siga siendo solo una pista del
backend, el corte greedy con slices exactos, `set_text` (medida + geometría
pintada), el aislamiento de la caché de líneas por texto/familia/tamaño/peso/ancho
y la supervivencia de entradas publicadas a una tormenta de fallos. `measure_test.c`
es el smoke del backend real: si no hay backend, reporta `SKIP` en vez de fallar.

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

## Histórico

El piloto vivo es `benchmarks/pilot-media-library/` y se ejecuta con
`python tools\cli.py bench pilot`; el runtime no consume JSON intermedio.
