# R1 estático (archivado)

R1 fue la primera prueba del proyecto: compilar un documento HTML/CSS a una
escena horneada (`display_list.json` → `scene_data.h`) y enlazarla con un
renderizador wgpu mínimo, sin motor de layout en el `.exe`.

Se retiró de la ruta de entrega por consolidación:

- duplicaba el **renderizador** (`runtime/c/` de este directorio) respecto a la
  vtable `wn_renderer`;
- duplicaba el **layout**, que ya vive en el motor C (R2);
- cada propiedad nueva había que implementarla dos veces o R1 y R2 se
  contradecían.

Lo que queda aquí no se compila, no se testea y no forma parte del producto.
Se conserva como referencia histórica y para leer los experimentos de AOT.

El contrato de R2 (`ui.json` pre-layout → `ui_data.h` → motor) está en
[`../../docs/architecture.md`](../../docs/architecture.md) y en
[`../../src/runtime/c-r2/README.md`](../../src/runtime/c-r2/README.md).

Los scripts de este directorio tienen rutas relativas a la estructura antigua
(`src/build.ps1`, `src/runtime/c-static/`, `src/emitter/python/`); no funcionan
tal cual desde `archive/`.
