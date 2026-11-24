# Convenciones del repositorio

## Nombres

- Todo en minúsculas, con guiones, sin espacios.
- Carpetas de experimentos: `archive/experiments/NN-<stage>-<slug>/`
  - `NN`: orden cronológico de dos dígitos.
  - `stage`: una de `parse`, `cascade`, `layout`, `text`, `render`, `aot`,
    `runtime`, `e2e`, `showcase`.
  - `slug`: descripción corta en inglés.
- Implementaciones canónicas del pipeline: `src/<layer>/<language>/`
  - layers: `ir`, `frontend`, `emitter`, `runtime`.
- Benchmarks: `benchmarks/<name>/` con `fixtures/`, `golden/`, `tools/`,
  `results/`.
- Dependencias externas: `third_party/<name>/`.

## Reglas

1. Los experimentos son **inmutables** una vez cerrados: no se parchean para
   que pasen benchmarks nuevos; se crea otro experimento.
2. `src/` contiene la versión canónica y viva de cada capa.
3. Todo contrato entre capas vive en un archivo versionado
   (`src/ir/schema.json`, `benchmarks/frontend-parity/SPEC.md`).
4. Cada implementación de una capa debe exponer un `run.ps1` con el CLI
   documentado en su README.
5. Los resultados de benchmarks se guardan en `results/` con fecha y versión.
6. Los frontends C++ y C# son **referencias de paridad congeladas** en
   `benchmarks/frontend-parity/SPEC.md`. No se actualizan con features nuevas
   de Rust; un cambio del SPEC decide si se actualizan o quedan fuera del
   subset.

## Mapa actual

```
archive/experiments/          16 experimentos históricos, renombrados por etapa
src/
  ir/                 contrato del display list + schema
  frontend/python/    frontend de referencia / oráculo v1
  frontend/rust/      frontend canónico (producción)
  frontend/cpp/       referencia de paridad congelada (SPEC.md)
  frontend/csharp/    referencia de paridad congelada (SPEC.md)
  emitter/python/     emit_ui.py (estilos -> ui_data.h)
  runtime/c-r2/       motor C: layout en runtime, eventos, resize, renderers
third_party/wgpu-native/
benchmarks/frontend-parity/
benchmarks/visual-parity/
archive/r1-static/    R1 horneado (archivado, no mantenido)
spec/                 fixtures originales del contrato
docs/                 arquitectura, IR y convenciones
```
