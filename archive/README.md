# Archivo histórico

`archive/experiments/` contiene los 16 experimentos que llevaron a webnative.
Son material **histórico**: no se mantienen, no entran en CI y sus comandos
asumen que se ejecutan desde su propio directorio. Se conservan como evidencia
del proceso, no como código de producción.

El código canónico vive en:

- `src/frontend/` — frontend HTML/CSS canónico (Rust) y tests.
- `src/runtime/` — motor C: core, documento, texto, renderers y host.
- `docs/` — contratos, arquitectura, ABI, roadmap y auditoría.

Solo el contenido **trackeado** del archivo (~3.5 MB de fuente, READMEs y
referencias) forma parte del repo. Los `.venv/`, `build/`, `target/`,
`node_modules/`, `__pycache__/` y capturas son artefactos ignorados y
desechables: se pueden borrar sin tocar la historia, y el pack de git sigue
siendo de ~2 MB. Por eso el archivo se queda aquí y no se mueve a una rama:
una rama no reduce el tamaño del clon.

`archive/experiments/16-text-atlas-c/` se conserva como **fixture de
referencia** para `src/frontend/verify_text.py` (sus `display_list.json` y
`atlas.bin`); por eso no se borra.

| Experimento | Tema | Estado |
|---|---|---|
| 01 | Parse HTML con Python (baseline) | histórico |
| 02 | Librerías nativas de parse | histórico |
| 03 | Cascada CSS con cssselect2 | histórico |
| 04 | Render wgpu offscreen | histórico |
| 05 | Paquete core v0 | histórico |
| 06 | Texto HarfBuzz + atlas | histórico |
| 07 | Layout flexbox | histórico |
| 08 | AOT con Direct2D | histórico |
| 09 | Paquete core v1 | histórico |
| 10 | AOT con wgpu-native | histórico |
| 11 | Página realista y gaps | histórico |
| 12 | Prototipo interactivo | histórico |
| 13 | HTML → `.exe` end-to-end | histórico |
| 14 | Showcase Python | histórico |
| 15 | Runtime R2 en C | **promovido** a `src/runtime/c-r2` |
| 16 | Atlas de texto en C | **referencia** para `c-static` / Rust |

No añadir features nuevas aquí. Si un experimento vuelve a ser necesario, se
promueve a `src/` con su propio contrato y tests.
