# Subset HTML/CSS v1

El subset es un **contrato cerrado**: lo que está aquí se garantiza; lo demás se
rechaza con diagnóstico (modo `--strict-subset`). No es “soportar la web”.

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

### Bordes y caja

- `border`, `border-width`, `border-style` (solid), `border-color`.
- `border-radius` (px).
- `overflow: hidden` (sin scroll en v1).

### Estados

- `:hover`, `:active`, `:focus`, `:disabled` (en elementos interactivos).

### Fuera de v1

`grid`, `position` (excepto `relative` futuro), `transform`, `transition`,
`animation`, `box-shadow`, `gradient`, `@media`, `@font-face`, pseudo-elementos
(`::before/::after`), `calc()`, `em/rem/vh/vw`, `float`, `table`, `filter`,
`clip-path`, `z-index`, `columns`.

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
