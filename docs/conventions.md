# Convenciones del repositorio

## Nombres

- Todo en minúsculas, con guiones, sin espacios.
- Carpetas de experimentos: `archive/experiments/NN-<stage>-<slug>/`
  - `NN`: orden cronológico de dos dígitos.
  - `stage`: una de `parse`, `cascade`, `layout`, `text`, `render`, `aot`,
    `runtime`, `e2e`, `showcase`.
  - `slug`: descripción corta en inglés.
- Implementaciones canónicas del pipeline: `src/<layer>/`
  - layers: `ir` (histórico), `frontend` (Rust), `runtime` (C).
- Benchmarks: `benchmarks/<name>/` con `fixtures/`, `golden/`, `tools/`,
  `results/`.
- Dependencias externas: `third_party/<name>/`.

## Reglas

1. Los experimentos son **inmutables** una vez cerrados: no se parchean para
   que pasen benchmarks nuevos; se crea otro experimento.
2. `src/` contiene la versión canónica y viva de cada capa.
3. Todo contrato entre capas vive en un archivo versionado
   (`src/ir/schema.json`, `benchmarks/frontend-parity/SPEC.md`).
4. La entrada canónica de build/run/test/bench es `python tools/cli.py`
   (`wn-cli`); no se añaden orquestadores `.ps1` nuevos. Las compilaciones
   sueltas de `cl.exe` (probes, harnesses) deben escribir objetos y binarios en
   `%TEMP%` con `/Fo` y `/Fe`; la raíz del repo no es un directorio de build.
5. Los resultados de benchmarks se guardan en `results/` con fecha y versión.
6. Los frontends C++ y C# son **referencias de paridad congeladas** en
   `benchmarks/frontend-parity/SPEC.md` y viven en `archive/frontends-legacy/`.
   No se actualizan con features nuevas de Rust; un cambio del SPEC decide si
   se actualizan o quedan fuera del subset.
7. Un probe o harness temporal que descubra un contrato o un bug se convierte
   en test permanente de la suite antes de cerrar el hallazgo; el probe se
   borra. Los tests de contrato no dependen de un backend concreto: usan la
   vtable o un backend falso; el backend real queda como smoke. Ejemplo:
   `src/runtime/tests/text/contract_test.c` corre sobre `fake_text.c` y fija
   que `max_width` sea solo una pista (el backend falso miente a propósito),
   mientras `measure_test.c` queda como smoke del backend real.

## Mapa actual

```
archive/experiments/          16 experimentos históricos, renombrados por etapa
src/
  ir/                 contrato del display list + schema
  frontend/           frontend canónico en Rust (producción)
  runtime/            motor C: layout en runtime, eventos, resize, renderers
tools/
  cli.py + wn_cli/    wn-cli: build/run/test/bench/setup
third_party/wgpu-native/
benchmarks/frontend-parity/
benchmarks/visual-parity/     (sin runner tras archivar su .ps1)
archive/frontends-legacy/     C++/C#/Python congelados (referencias de paridad)
archive/ps1-legacy/           orquestadores PowerShell archivados
archive/r1-static/    R1 horneado (archivado, no mantenido)
spec/                 fixtures originales del contrato
docs/                 arquitectura, IR y convenciones
```
