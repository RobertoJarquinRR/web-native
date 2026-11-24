# Subset HTML/CSS v1

El subset es un **contrato cerrado**: lo que está aquí se garantiza; lo demás se
rechaza con diagnóstico (modo `--strict-subset`). No es “soportar la web”.

> **Estado real:** este documento es el contrato objetivo. La matriz de lo que
> el frontend emite y lo que R1/R2 renderizan hoy está en
> [`support.md`](support.md). Varias propiedades v1 se parsean pero su render
> runtime sigue pendiente (por ejemplo `font-weight`, `text-align`,
> `line-height`, `opacity`, `border-radius` y `box-shadow`).

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

### Tipografía y color

- `font-family` (system fonts; sin web fonts en v1).
- `font-size` (px), `font-weight` (400/700).
- `line-height` (px/normal), `text-align` (left/center/right).
- `color`, `background`, `background-color` (`#rgb`, `#rrggbb`, `#rrggbbaa`,
  `rgb()`, `rgba()`, nombres básicos).
- `opacity` (0–1).

> **Runtime actual:** `font-weight` no selecciona todavía una variante real de
> la fuente; `line-height` y `text-align` no se aplican de forma fiable en R2;
> `opacity` se emite pero el core aún no lo compone. Ver `support.md`.

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
- **v3:** `input type="text"` de una línea con pool ASCII; bordes por lado,
  `border-radius` por esquina, `box-shadow`, `min/max-*` y unidades
  `em/rem/vh/vw`.

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
