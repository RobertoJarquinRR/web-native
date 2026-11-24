# Fixtures `subset v1`

Dos páginas mínimas para ejercitar los diagnósticos de `--strict-subset` del
frontend Rust (`docs/subset-v1.md`). No forman parte del benchmark de paridad
R1: solo comprueban el contrato del subconjunto.

## `supported/`

`index.html` + `style.css` usan únicamente etiquetas, atributos, propiedades y
valores de v1: bloque/flex, `gap`, márgenes/padding en px, borde `solid`,
`border-radius`, `box-sizing: border-box`, colores hex/rgba, estados
`:hover/:active/:focus/:disabled`, `data-*`, `img/src`, `link/rel/href`,
`button/disabled`, `ul/ol/li` y un bloque `<style>`.

```powershell
src/frontend/rust/target/release/frontend-rust.exe `
  --html benchmarks/frontend-parity/fixtures/subset/supported/index.html `
  --width 400 --height 300 --out "$env:TEMP\supported.ir.json" --strict-subset
```

Salida esperada: **ninguna línea `[subset]`** y **exit 0**. El IR se escribe con
6 ops (el contenido de texto no afecta al diagnóstico).

## `unsupported/`

`index.html` + `style.css` mezclan features fuera de v1: `script`, `iframe`,
`display: grid`, `grid-template-columns`, `position`, `box-shadow`,
`transition`, `animation`, `background` con gradiente y con `url()`,
`width: 10em`, `height: calc(...)`, `border: dashed`, `opacity > 1`,
`hsl()` y `transform` inline. También usa `onclick`, que no está en la lista de
atributos.

```powershell
# modo estricto: 19 diagnósticos como error y exit 1
src/frontend/rust/target/release/frontend-rust.exe `
  --html benchmarks/frontend-parity/fixtures/subset/unsupported/index.html `
  --width 400 --height 300 --out "$env:TEMP\unsupported.ir.json" --strict-subset

# sin el flag: los mismos 19 diagnósticos como warning y exit 0
src/frontend/rust/target/release/frontend-rust.exe `
  --html benchmarks/frontend-parity/fixtures/subset/unsupported/index.html `
  --width 400 --height 300 --out "$env:TEMP\unsupported.ir.json"
```

Diagnósticos esperados (archivo:línea, feature):

| Archivo:línea | Feature | Mensaje |
|---|---|---|
| `style.css:3` | gradiente | `gradientes no soportados (fuera de v1) en 'background'` |
| `style.css:9` | `position` | `propiedad CSS 'position' no soportada` |
| `style.css:10` | `display: grid` | `valor 'grid' no soportado en 'display'` |
| `style.css:11` | `grid-template-columns` | `propiedad CSS 'grid-template-columns' no soportada` |
| `style.css:12` | `1rem` | `unidad o valor '1rem' no soportado en 'gap'` |
| `style.css:16` | `box-shadow` | `propiedad CSS 'box-shadow' no soportada` |
| `style.css:17` | `transition` | `propiedad CSS 'transition' no soportada` |
| `style.css:21` | `10em` | `unidad o valor '10em' no soportado en 'width'` |
| `style.css:22` | `calc()` | `calc() no soportado en 'height'` |
| `style.css:23` | `border: dashed` | `valor 'dashed' no soportado en 'border'` |
| `style.css:27` | `animation` | `propiedad CSS 'animation' no soportada` |
| `style.css:28` | `opacity: 1.5` | `valor '1.5' no soportado en 'opacity'` |
| `style.css:29` | `url()` | `url()/recursos externos no soportados en 'background'` |
| `index.html:5` | `<script>` | `etiqueta <script> no soportada (fuera de v1)` |
| `index.html:8` | `hsl()` | `valor 'hsl(210, 40%, 98%)' no soportado en 'color'` |
| `index.html:9` | `box-shadow` | `propiedad CSS 'box-shadow' no soportada` |
| `index.html:14` | `onclick` | `atributo 'onclick' no soportado` |
| `index.html:15` | `<iframe>` | `etiqueta <iframe> no soportada (fuera de v1)` |
| `index.html:16` | `transform` inline | `propiedad CSS 'transform' no soportada` |

Cada línea incluye además el contexto entre corchetes, por ejemplo:

```
error[subset] benchmarks\frontend-parity\fixtures\subset\unsupported\style.css:10: valor 'grid' no soportado en 'display' (subset v1: block | flex | none) [selector '#root', propiedad 'display']
```

En modo estricto, el binario sigue escribiendo el IR y el atlas (si hay texto),
pero termina con exit 1 para que CI pueda rechazar la página. Sin
`--strict-subset` imprime `warning[subset]` y termina con exit 0.
