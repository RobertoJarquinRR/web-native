# Archivo histórico

`archive/experiments/` contiene los 16 experimentos que llevaron a webnative.
Son material **histórico**: no se mantienen, no entran en CI y sus comandos
asumen que se ejecutan desde su propio directorio. Se conservan como evidencia
del proceso, no como código de producción.

El código canónico vive en:

- `src/frontend/rust/` — frontend HTML/CSS canónico.
- `src/frontend/python/` — oráculo de paridad y verificación.
- `src/emitter/python/` — `emit_c`, `emit_c_text`, `emit_ui`.
- `src/runtime/c-r2/` — runtime dinámico, core y ABI C.
- `src/runtime/c-static/` — runtime estático R1.
- `docs/` — contratos, arquitectura, ABI, roadmap.

`archive/experiments/16-text-atlas-c/` se conserva como **fixture de
referencia** para `src/runtime/c-static/build_text.ps1` y
`src/frontend/rust/bench-rust.ps1`; por eso no se borra.

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
