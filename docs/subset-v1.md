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
`video` (blob v7, elemento reemplazado), `ul`, `ol`, `li`, `link` (stylesheet),
`style` (inline CSS).

### Atributos

- `id`, `class`, `style` (subset de propiedades).
- `data-hover`, `data-press`, `data-selected`, `data-action` (estado R2).
- `src` en `img` (archivo local; sin red en v1).
- `src`, `poster`, `width`, `height`, `controls`, `autoplay`, `muted`, `loop`,
  `preload` en `video` (rutas locales; sin esquema `://` ni red).
- `href` en `link` (stylesheet local).
- `title` (tooltip básico, futuro).
- `disabled` en `button`.

### Fuera de v1

`script`, `iframe`, `form` complejo, `input` avanzado, `select`, `table`,
`canvas`, `audio`, `svg`, `template`, `slot`, `object`, `embed`.

## CSS

### Selectores

- Tipo, `.clase`, `#id`, `[attr]`/`[attr="valor"]`, compuestos, descendiente
  (` `), hijo (`>`) y listas separadas por comas.
- Estructurales estáticos: `:root`, `:first-child`, `:last-child`,
  `:only-child`, `:nth-child(An+B)` y `:nth-of-type(An+B)`.
- `An+B` acepta `odd`, `even`, un entero y las formas `n`, `2n`, `-n+3`,
  `2n+1`/`2n + 1`. El índice es 1-based entre los hijos elemento; `nth-of-type`
  cuenta sólo hermanos con la misma etiqueta.
- Se diagnostican y descartan: `of` (`:nth-child(2n of .x)`), `:nth-last-*`,
  argumentos inválidos, combinadores `+`/`~`, pseudo-elementos y pseudos no
  listadas (incluida `:checked`, diferida porque no hay checkbox/radio).
- `:has(<compuesto>)` y `:has(> <compuesto>)` con match **estático**: el
  compuesto interno se busca en cualquier descendiente o solo en un hijo
  directo. Acepta tipo, universal, `.clase`, `#id`, `[attr]` y pseudos
  estructurales. Se diagnostican y descartan las listas, los combinadores
  dentro del argumento, los estados, `:has()` anidado y las formas vacías.

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
- `position: static | relative | absolute | fixed | sticky` (v4; `static` es
  el default). `fixed` usa siempre el viewport como containing block e ignora
  el scroll/clip de los ancestros; `sticky` sigue en flujo y se ancla al
  scroll container más cercano (`overflow: scroll`) con `top`/`bottom`; su
  scrollport es el content box del scroller (ver `wnui-format.md`).
- `top`, `right`, `bottom`, `left` en px, `%` o `auto` (los porcentajes se
  resuelven en layout contra el containing block; no se hornean).
- `inset` (1-4 valores: px, `%` o `auto`, negativos incluidos) se expande en
  el frontend a `top`/`right`/`bottom`/`left`; el blob no cambia.
- `z-index: auto | <integer>` (v5; enteros de 32 bits, negativos y 0 incluidos;
  `-2147483648` reservado para `auto`). Se aplica a nodos posicionados
  (`relative`/`absolute`/`fixed`/`sticky`) y a ítems flex/grid `static`; `auto`
  no crea contexto y en cualquier otro `static` se ignora, como en CSS.

> **Modelo de caja:** el motor calcula siempre *border-box*: `width`/`height`
> incluyen padding y borde. CSS usa *content-box* por defecto, así que una
> página que no declare `box-sizing: border-box` se verá distinta a un
> navegador. Decláralo en la hoja (`* { box-sizing: border-box }`). Con esa
> declaración la paridad medida es exacta (ver
> `benchmarks/visual-parity/BASELINE.md`). `content-box` está fuera del subset.

### Tipografía y color

- `font-family` (system fonts; sin web fonts en v1).
- `font-size` (px).
- `font-weight`: `normal` (=400), `bold` (=700) o un entero 100–900.
  Cualquier otro valor (unidad, decimal, fuera de rango) es inválido: la
  declaración se descarta como en CSS (el nodo conserva el peso heredado) y el
  subset la diagnostica. El peso es heredado.
- `line-height` (px/normal), `text-align` (left/center/right).
- `text-overflow: ellipsis` en una caja de **una línea** con
  `overflow: hidden`: el runtime recorta la línea y pinta `…`. En multilínea el
  flag es inerte; `clip` es el valor por defecto.
- `color`, `background`, `background-color`: hex 3/4/6/8, `rgb()`/`rgba()` y
  `hsl()`/`hsla()` (comas o sintaxis espacio/barra, 3–4 argumentos, alfa
  numérica o en `%`), nombres CSS3 y `rebeccapurple`/`transparent`. La misma
  gramática vale para `border*` y `box-shadow`; una forma fuera
  de ella (`lab()`, `color-mix()`, …) se diagnostica en `--strict-subset`.
- `opacity` (0–1; valores fuera de rango se recortan, porcentajes no se
  aceptan en v1).

> **Runtime actual:** `font-weight` 100–900 se mapea a `DWRITE_FONT_WEIGHT`.
> Un valor intermedio elige el canónico más cercano, no la preferencia 400–500
> de CSS Fonts 4: `450` cae en `400` y `550` en `500`. Después DirectWrite
> selecciona la cara instalada más próxima de la familia. No se fuerza
> síntesis: si la familia no trae el peso, DirectWrite puede simular bold (o
> usar la cara real más cercana). `line-height` y `text-align` se aplican en R2
> (E1), `margin: auto`
> funciona en block y flex. Todo nodo con `opacity < 1` crea un **grupo**: su
> caja y su subárbol se pintan en una capa offscreen y se componen una sola vez.
> Las hojas también, porque pueden pintar varias pasadas (borde+fondo, sombra+
> relleno, decoraciones de input, texto/imagen sobre fondo). Ver `support.md`.

### Variables CSS

- `--nombre: valor` declara una custom property; se hereda como cualquier
  propiedad y el nombre es *case-sensitive*.
- `var(--nombre)` y `var(--nombre, fallback)` se sustituyen en la cascada,
  antes de interpretar el valor. El fallback se usa si el token no existe o es
  inválido (ciclo). Sin token y sin fallback la declaración es inválida en
  tiempo de cálculo y el nodo conserva el valor heredado o inicial.
- El frontend diagnostica `var()` mal formada y tokens no definidos en el
  documento sin fallback (con `--strict-subset` son error). Un `var()` dentro
  de `border`/`border-*` se diagnostica porque el shorthand se expande antes de
  resolver variables. En `inset`/`margin`/`padding` un `var()` debe resolver a
  un solo componente.

### Bordes y caja

- `border`, `border-width`, `border-style` (solid), `border-color`.
- `border-radius` (px).
- `overflow: hidden` (sin scroll en v1).

> **Runtime actual:** CSS core v1 ya pinta radio y borde uniformes y la primera
> sombra no-inset. Per-side, per-corner y `box-shadow: inset` siguen pendientes.
> `box-shadow` es una extensión pilot v3.

### Estados

- `:hover`, `:active`, `:focus`, `:disabled`, `:focus-visible` y
  `:focus-within` (v6). Son **semánticos**: el frontend resuelve seis estilos
  efectivos por nodo (hover, press = `:hover`+`:active`, focus, disabled,
  focus-visible y focus-within) y viajan `background`, `color`, el color de
  borde uniforme y `font-weight`. `:disabled` sólo se guarda para
  `button`/`input`. La entrada focus-visible pliega las declaraciones `:focus`
  en la misma cascada; focus-within es un estado propio del nodo y de sus
  ancestros.
- Se admite **una** pseudo-clase de estado por regla; `:hover:active` es la
  combinación de press y también se admite. Otras combinaciones
  (`:hover:focus`, …) se diagnostican y no se aplican.
- La pseudo-clase de estado sólo es válida en el **compuesto sujeto** (el último
  de la regla). `.card:hover .label` pone el estado en un ancestro: este runtime
  no lo propaga, así que la regla se descarta y se diagnostica en lugar de
  compilarse en silencio.
- Una regla de estado sólo aporta **colores y `font-weight`**:
  `background`/`background-color`, `color`, `border-color` (o `border-*-color`)
  y `font-weight`. `border` sin más sólo aporta su color;
  `border-width`/`border-style`, `opacity`, layout y `z-index` dentro de una
  regla de estado se diagnostican y no se aplican. Un color de estado con alfa 0
  (`transparent`, `rgba(…,0)`) también se diagnostica: el blob no lo representa
  como override.
- `data-hover`/`data-press`/`data-selected` siguen existiendo como azúcar para
  variantes sin CSS. Los estilos CSS tienen prioridad.
- Diferido: `:checked` (no hay widget checkbox/radio; se diagnostica) y
  transiciones.

### Fuera de v1

`position: fixed | sticky` ya está implementado en el runtime actual. Siguen
fuera de v1 `transform`, `transition`, `animation`,
`gradient`, `@media`, `@font-face`, pseudo-elementos (`::before/::after`),
`calc()`/`clamp()`, `float`, `table`, `filter`, `clip-path`, `columns`.

> `grid`, `box-shadow`, `em/rem/vh/vw`, per-side borders, per-corner radius y
> `min/max` ya no están “fuera de v1” sin más: son **pilot v2/v3** y están en la
> sección de extensiones. Este listado es solo lo que sigue fuera del contrato
> actual.

## Extensiones pilot (no v1)

Estas features no forman parte del subset v1 original. v2/v3 se habilitan con
`--pilot` y el frontend avisa (o falla con `--strict-subset`) si se usan sin él;
v4 (`position`) se acepta directamente en esta lane:

- **v2:** `display:grid` con `repeat(auto-fill, px)`, `grid-auto-rows`, gap,
  `overflow: auto|scroll`, imágenes locales y `object-fit`.
- **v3:** `input type="text"` de una línea (valor inicial ASCII, edición UTF-8
  en runtime); bordes por lado, `border-radius` por esquina, `box-shadow`,
  `min/max-*` y unidades `em/rem/vh/vw`.
- **v4:** `position: relative | absolute | fixed | sticky` con
  `top/right/bottom/left` en px/%/auto (el enum de `position` se extiende sin
  bump de blob). `fixed` se ancla al viewport e ignora el scroll de los
  ancestros; `sticky` sigue en flujo y se ancla al scroll container más cercano
  (`top`/`bottom`, con clamp al containing block). El shorthand `inset` se
  expande en el frontend y viaja por esos mismos campos (blob sin cambios).
- **v5:** `z-index: auto | <integer>` y el orden de pintado por stacking
  contexts (negativos, 0/auto y positivos, anidados). Se acepta sin `--pilot`.

El detalle del contrato y de lo que el runtime ya implementa está en
[`support.md`](support.md).

## Diagnósticos

- El frontend Rust emite `warning`/`error` con archivo, línea y propiedad.
  Valores de `position` desconocidos, unidades no soportadas en insets, formas
  de `:has()` fuera del subconjunto, `var()` mal formada o con un token no
  definido sin fallback, `calc()`/`clamp()`, valores de `z-index`
  no enteros/fuera de rango, pseudo-clases no soportadas y combinaciones de
  estado distintas de `:hover:active` se diagnostican.
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
