# Subset HTML/CSS v1

El subset es un **contrato cerrado**: lo que está aquí se garantiza; lo demás se
rechaza con diagnóstico (modo `--strict-subset`). No es “soportar la web”.

> **Estado real:** este documento es el contrato objetivo. La matriz de lo que
> el frontend emite y lo que R1/R2 renderizan hoy está en
> [`support.md`](support.md). `font-weight`, `line-height`, `text-align` y
> `opacity` compuesta ya se aplican en R2 (E1); per-side borders siguen
> pendientes.

## HTML

### Etiquetas

`html`, `head`, `body`, `div`, `span`, `p`, `h1`–`h6`, `button`, `img`,
`ul`, `ol`, `li`, `link` (stylesheet), `style` (inline CSS).

### Atributos

- `id`, `class`, `style` (subset de propiedades).
- `data-hover`, `data-press`, `data-selected`, `data-action` (estado R2).
- `src` en `img` (archivo local; sin red en v1).
- `href` en `link` (stylesheet local).
- `title` (tooltip básico, futuro).
- `disabled` en `button`.

### Fuera de v1

`script`, `iframe`, `form` complejo, `input` avanzado, `select`, `table`,
`canvas`, `video`, `audio`, `svg`, `template`, `slot`, `object`, `embed`.

## CSS

### Layout

- `display: block | flex` (`grid` en fase posterior).
- `flex-direction: row | column`.
- `gap`, `row-gap`, `column-gap` (px).
- `justify-content: flex-start | center | flex-end | space-between`.
- `align-items: stretch | flex-start | center | flex-end`.
- `flex-wrap: nowrap | wrap`.
- `width`, `height` (px, %).
- `padding`, `margin` (shorthand + longhands, px).
- `box-sizing: border-box` (fijo en v1).

> **Modelo de caja:** el motor calcula siempre *border-box*: `width`/`height`
> incluyen padding y borde. CSS usa *content-box* por defecto, así que una
> página que no declare `box-sizing: border-box` se verá distinta a un
> navegador. Decláralo en la hoja (`* { box-sizing: border-box }`). Con esa
> declaración la paridad medida es exacta (ver
> `benchmarks/visual-parity/BASELINE.md`). `content-box` está fuera del subset.

### Tipografía y color

- `font-family` (system fonts; sin web fonts en v1).
- `font-size` (px), `font-weight` (400/700).
- `line-height` (px/normal), `text-align` (left/center/right).
- `color`, `background`, `background-color`: hex 3/4/6/8, `rgb()`/`rgba()` y
  `hsl()`/`hsla()` (comas o sintaxis espacio/barra, 3–4 argumentos, alfa
  numérica o en `%`), nombres CSS3 y `rebeccapurple`/`transparent`. La misma
  gramática vale para `border*` y `box-shadow`; una forma fuera
  de ella (`lab()`, `color-mix()`, …) se diagnostica en `--strict-subset`.
- `opacity` (0–1; valores fuera de rango se recortan, porcentajes no se
  aceptan en v1).

> **Runtime actual:** `font-weight ≥ 600` usa la cara bold real del sistema,
> `line-height` y `text-align` se aplican en R2 (E1), `margin: auto` funciona en
> block y flex. `opacity < 1` en un contenedor crea un **grupo**: el subárbol se
> pinta en una capa offscreen y se compone una sola vez (los hijos que se
> solapan no se oscurecen dos veces); en una hoja se aplica alfa directo a su
> geometría. Ver `support.md`.

### Bordes y caja

- `border`, `border-width`, `border-style` (solid), `border-color`.
- `border-radius` (px).
- `overflow: hidden` (sin scroll en v1).

> **Runtime actual:** CSS core v1 ya pinta radio y borde uniformes y la primera
> sombra no-inset. Per-side, per-corner e inset siguen pendientes.
> `box-shadow` es una extensión pilot v3.

### Estados

- `:hover`, `:active`, `:focus`, `:disabled` (en elementos interactivos).

### Fuera de v1

`position` (excepto `relative` futuro), `transform`, `transition`,
`animation`, `gradient`, `@media`, `@font-face`, pseudo-elementos
(`::before/::after`), `calc()`, `float`, `table`, `filter`, `clip-path`,
`z-index`, `columns`.

> `grid`, `box-shadow`, `em/rem/vh/vw`, per-side borders, per-corner radius y
> `min/max` ya no están “fuera de v1” sin más: son **pilot v2/v3** y están en la
> sección de extensiones. Este listado es solo lo que sigue fuera del contrato
> actual.

## Extensiones pilot (no v1)

Estas features no forman parte del subset v1; se habilitan con `--pilot` y el
frontend avisa (o falla con `--strict-subset`) si se usan sin él:

- **v2:** `display:grid` con `repeat(auto-fill, px)`, `grid-auto-rows`, gap,
  `overflow: auto|scroll`, imágenes locales y `object-fit`.
- **v3:** `input type="text"` de una línea (valor inicial ASCII, edición UTF-8
  en runtime); bordes por lado, `border-radius` por esquina, `box-shadow`,
  `min/max-*` y unidades `em/rem/vh/vw`.

El detalle del contrato y de lo que el runtime ya implementa está en
[`support.md`](support.md).

## Diagnósticos

- El frontend Rust emite `warning`/`error` con archivo, línea y propiedad.
- `--strict-subset`: error y exit ≠ 0 ante cualquier feature fuera del subset.
- Sin `--strict-subset`: warning y fallback (ignorar la propiedad).
- Objetivo: que el diseñador sepa exactamente qué puede usar.

## Versionado

- `subset v1` es el contrato inicial.
- Añadir una feature = minor bump del subset.
- Quitar/romper = major bump + migración documentada.
- El IR/UI tree declara `schema` y `generator` para trazabilidad.

## Por qué un subset

1. **Alcance**: evita reimplementar la web platform.
2. **Paridad**: permite que Rust/Python/C++/C# produzcan el mismo IR.
3. **Rendimiento**: el runtime sabe qué esperar (sin layout exótico).
4. **Seguridad**: sin scripts ni recursos remotos por defecto.
5. **Diagnóstico**: errores claros en vez de silencios del navegador.
