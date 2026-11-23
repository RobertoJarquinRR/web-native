# IR: display list

El contrato entre el frontend y todo lo demás es un JSON con dos claves:

```json
{
  "viewport": { "width": 480, "height": 320 },
  "ops": [
    { "type": "rect", "x": 0, "y": 0, "w": 480, "h": 320, "fill": "#0f1115" },
    { "type": "rect", "x": 24, "y": 24, "w": 12, "h": 12, "fill": "#e74c3c" }
  ]
}
```

## Reglas

- `viewport` en px enteros.
- `ops` en **orden painter's**: se dibujan en el orden del array.
- `rect`: geometría en px flotantes y `fill` `#rgb`, `#rrggbb` o `#rrggbbaa`.
- `text_run` (opcional): glifos pre-shapeados con UVs de atlas. Ver
  `src/ir/schema.json` y `benchmarks/frontend-parity/SPEC.md`.
- Los fondos se emiten en **pre-orden** (padre antes que hijos).
- `body` con `margin: 0` deja el root en (0,0).
- Box model: **border-box** (width/height incluyen padding).

## Validación

- Schema: `src/ir/schema.json`.
- Comparador con tolerancia: `benchmarks/frontend-parity/tools/verify_ir.py`.

## Ejemplos

- `benchmarks/frontend-parity/golden/contract.ir.json` (400×300, 2 divs).
- `benchmarks/frontend-parity/golden/flex-page.ir.json` (480×320, flex anidado).
