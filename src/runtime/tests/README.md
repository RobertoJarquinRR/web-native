# Runtime test suites

Los gates vivos se ejecutan con `wn-cli`:

```powershell
python tools\cli.py test all
python tools\cli.py test all --no-wgpu
python tools\cli.py test suite <nombre>
```

| Suite | Directorio | Cubre |
|---|---|---|
| `css` | `css/` | radios, borde uniforme, sombra, imagen; **única** ruta que aún usa `emit_ui.py` sobre `page/*.json` (fase 2b) |
| `fidelity` | `fidelity/` | E1: font-weight, `text-align`, `line-height`, `margin:auto`, flex |
| `dynamic` | `dynamic/` | `set_visible`, clone/append/remove, demo ABI |
| `input` | `input/` | caret, selección, clipboard, demo ABI |
| `offscreen` | `offscreen/` | readback, caché de imágenes, D3D11 vs wgpu |
| `text` | `text/` | medición DirectWrite, atlas, wrapping, documento |
| `blob` | `blob/` | loader `.wnui`, builtin vs archivo, píxeles (`max=0`) |
| `dump` | `dump/` | `wnui-dump` texto/JSON, golden y errores de archivo |

Todos los fixtures excepto `css/` emiten el documento con
`frontend-rust --emit-blob` y lo embeben como `builtin_blob.h`. No se generan
`ui.json` ni `ui_data.h` para esos suites.

## `dump`

`dump/canonical.txt` es el golden de la proyección de texto del blob canónico
(960x640). Lo produce `src/runtime/tools/wn_dump.c` con el mismo loader del
runtime. El golden detecta cambios silenciosos del dump; **no es un contrato**:
la invariante primaria es el SHA-256 del `app.wnui` y el render idéntico al
builtin (`blob`). Si el formato del dump cambia a propósito, se regenera el
golden junto con el blob.

## `css` (excepción temporal)

`css/page/{card,control,none}.json` son fixtures JSON escritos a mano; la suite
los pasa por `emit_ui.py --ui-json` para producir `ui_data.h` + `app.wnui` y
`verify_css.py` conserva sus comprobaciones de campos sobre el header. Es la
única dependencia Python/CSS que queda para la fase 2b, que migrará estos
fixtures a HTML/CSS y borrará `emit_ui.py`.

## Histórico

`tests/pilot-media-library/` (y `emitter-escapes/`, retirado) pertenecen a
carriles anteriores. El piloto vivo es
`benchmarks/pilot-media-library/` y se ejecuta con
`python tools\cli.py bench pilot`; el runtime ya no consume JSON intermedio.
