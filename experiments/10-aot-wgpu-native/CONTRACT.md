# Contrato: display list IR (prueba mínima)

Pipeline objetivo:

1. Parse HTML -> AST
2. Parse CSS + cascade -> estilos computados por elemento
3. Layout block (padding + margin) -> geometria resuelta
4. Display list (IR, lenguaje-agnostico)
5. Render offscreen -> PNG

## Input

`input.html` + `input.css`: dos cajas apiladas dentro de `#root`.

## Geometria esperada (viewport 400x300)

| op | x | y | w | h | fill |
|---|---|---|---|---|---|
| root | 0 | 0 | 400 | 300 | #101014 |
| box a | 20 | 20 | 120 | 80 | #e74c3c |
| box b | 20 | 116 | 120 | 80 | #3498db |

Orden painter's: root primero, luego a, luego b.

## Formato JSON

```json
{
  "viewport": {"width": 400, "height": 300},
  "ops": [
    {"type": "rect", "x": 0, "y": 0, "w": 400, "h": 300, "fill": "#101014"},
    {"type": "rect", "x": 20, "y": 20, "w": 120, "h": 80, "fill": "#e74c3c"},
    {"type": "rect", "x": 20, "y": 116, "w": 120, "h": 80, "fill": "#3498db"}
  ]
}
```

Se permiten ops extra (fondo de body, etc.), pero las 3 anteriores son
obligatorias y deben conservar ese orden relativo.

## Verificacion

- PNG de 400x300 (o 400x300 exacto si el backend lo permite).
- pixel (60,60)   ~= #e74c3c (box a)
- pixel (60,156)  ~= #3498db (box b)
- pixel (200,250) ~= #101014 (root)
