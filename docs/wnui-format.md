# Formato `.wnui` y `wnui-dump`

> **No es un contrato.** El documento `.wnui` v6 es el contrato *runtime* entre
> el frontend y el loader. `wnui-dump` es una **proyección de debug** de ese
> blob: su texto y su JSON pueden cambiar cuando cambia la versión del blob.
> La invariante estable es el hash SHA-256 del blob canónico y la comparación
> píxel a píxel (`blob` vs builtin), no la salida del dump.
>
> **Pre-alfa:** el formato puede romperse y subir de versión sin compatibilidad
> hacia atrás. Todos los consumidores (DLL, galería, fixtures) se reconstruyen
> del mismo repo. La política de compatibilidad (C6) se decide al congelar 1.0.

## Secciones del blob

Cabecera de 32 bytes, little-endian:

| Campo | Tipo | Notas |
|---|---|---|
| `magic` | `u32` | `0x49554E57` (`WNUI`) |
| `version` | `u32` | `6` |
| `section_count` | `u32` | secciones en la tabla |
| `file_size` | `u32` | tamaño total; el loader lo exige exacto |
| `node_count` | `u32` | nodos del árbol (máx. 65536) |
| `viewport_width` | `u32` | px de referencia |
| `viewport_height` | `u32` | px de referencia |
| `reserved` | `u32` | 0 |

Cada entrada de la tabla son 16 bytes: `id`, `offset`, `size`, `reserved`.
Las secciones empiezan alineadas a 8 bytes:

| id | Sección | Contenido |
|---|---|---|
| 1 | nodos | `node_count` registros fijos de 252 bytes (`WnBlobNode`) |
| 2 | nombres | `node_count` offsets `u32` al pool de strings |
| 3 | strings | pool NUL-terminado; offset 0 = NULL |
| 4 | inputs | registros `WnBlobInput` (20 bytes); ausente si no hay inputs |
| 5 | states | estilos de estado CSS (v6); **ausente** si ningún nodo tiene estados |
| 6 | text_extras | flags de texto (v6, aditivo; p. ej. ellipsis); **ausente** si no hay flags |

El registro de nodo guarda `parent`, `kind`, dirección/justify/align/wrap,
geometría declarada, márgenes/padding, colores ARGB resueltos, offsets de
texto/imagen/familia, la acción resuelta (`action`), grid, overflow,
borde/radius/sombra, alineación de texto, `line-height`, `margin-auto`,
`box-sizing`, flex, peso de fuente (`font_weight`, int32 100..900; `normal`=400,
`bold`=700) y `opacity` (v3, `float`, 0..1; el loader rechaza valores fuera de
rango). v4 añade al final `position` (entero: 0
`static`, 1 `relative`, 2 `absolute`), `inset[4]` (`float`, orden
top/right/bottom/left) e `inset_mask`. v5 añade `int32 z_index` con el sentinel
documentado `INT32_MIN` (=-2147483648) para `auto`; cualquier otro int32 es un
nivel de apilado explícito (negativo, 0 o positivo). No guarda los atributos
`data-*` crudos:
la cascada ya los convirtió en ARGB y código de acción. El orden y ancho de los
campos están en `src/runtime/document/wn_blob.h`. **v6 no cambia el registro de
nodo** (sigue en 252 bytes): los estilos de estado viajan en la sección opcional
5, sólo para los nodos que los necesitan.

### Sección `states` (v6)

La sección 5 es una secuencia de registros variables, en orden estricto de
índice de nodo (creciente, sin duplicados):

```
WnBlobStateRecord (8 bytes)   int32 node; uint8 state_count (0..6);
                              uint8 initial_state; uint8 reserved[2]
  WnBlobStateStyle (16 bytes) uint8 state; uint8 present; uint16 reserved;
                              uint32 background; uint32 text; uint32 border_color
  ... state_count entradas ...
```

- `initial_state`: bits con los que arranca el nodo. Hoy sólo se siembra
  `WN_STATE_DISABLED` (0x10) desde el atributo `disabled` de `button`/`input`;
  el loader rechaza cualquier otro bit. Un nodo con `disabled` y sin reglas
  `:disabled` igualmente lleva registro (`state_count = 0`).
- `state`: `0` hover, `1` press, `2` focus, `3` disabled, `4` focus-visible,
  `5` focus-within. Los códigos 4–5 son aditivos en v6: el registro sigue
  midiendo 16 bytes y `state_count` sólo puede crecer hasta 6.
- `present`: bit 0 = background, bit 1 = text color, bit 2 = border color,
  bit 3 = `font-weight` (aditivo v6). Sólo los campos presentes se aplican; el
  resto conserva el valor base.
- El `font-weight` de estado viaja en el `uint16 reserved` del registro
  (`100..=900`) cuando el bit 3 está presente. Sin el bit, `reserved` debe ser
  0; el loader valida ambos casos. No hay cambio de tamaño ni de versión.
- Los colores son ARGB. Un campo presente con color `0x00000000` se rechaza:
  esta lane no representa `transparent` como override de estado (el frontend
  normaliza a `#rrggbb` opaco, descarta alfa ≤ 0 y **diagnostica** ese color en
  una regla de estado, para que no quede una declaración silenciosa).
- El loader valida índice de nodo, orden, `state_count`, `initial_state`,
  códigos de estado, máscara `present`, colores y límites de la sección. Lleva
  registro todo nodo con algún estilo no-default **o** un `initial_state`
  distinto de cero (p. ej. `disabled` sin reglas `:disabled`). Un documento sin
  estados no lleva la sección y conserva el coste anterior.

**Combinación (build time).** El frontend resuelve seis estilos efectivos por
nodo aplicando la cascada normal (especificidad, `!important`, orden) con un
conjunto de estados activo:

| Estilo | Estados activos | Contenido |
|---|---|---|
| hover | `:hover` | sólo reglas `:hover` (más base/inline que ganen) |
| press | `:hover` + `:active` | press **incluye** hover: una regla `:active` pisa a `:hover` por orden/especificidad |
| focus | `:focus` | sólo focus |
| disabled | `:disabled` | sólo en nodos que pueden deshabilitarse (`button`/`input`) |
| focus-visible | `:focus` + `:focus-visible` | se materializa sólo si una regla `:focus-visible` aplica; la cascada decide `:focus` vs `:focus-visible` |
| focus-within | `:focus-within` | sólo su propia regla; el runtime la hace efectiva en el nodo enfocado y sus ancestros |

Un estilo de estado sólo guarda un campo si la declaración ganadora proviene de
una regla con pseudo-clase de estado (o si el nodo no tiene `color`/`font-weight`
propio y hereda el valor de estado de su ancestro). Por eso un `:hover` que sólo
cambia texto no pisa el `selected_bg` de la app con el fondo base.

**Prioridad de pintado (runtime).** Por campo, en este orden:

```
disabled > press > hover > focus-visible > focus > focus-within > base
```

`selected` es el color base de la app (`data-selected`), no un estado de la
tabla: se ve cuando ningún estado superior define ese campo. Así
`selected + hover` muestra el color de hover. Los `data-hover`/`data-press`
siguen funcionando como azúcar para variantes sin CSS; los estilos CSS tienen
precedencia. `:disabled` gana siempre y suprime hover/press/selected.

**Prioridad de `font-weight`.** Usa la misma tabla que los colores. Layout,
shaping, atlas y pintura piden el peso efectivo por nodo; una regla
`:hover { font-weight: 700 }` recalcula el ancho/avance y el atlas usa el peso
como parte de la clave.

**Focus-visible y focus-within (runtime).** `WN_STATE_FOCUS_VISIBLE` (0x20) es
un bit aditivo de ABI 7: el host marca foco de teclado/programático pasándolo
junto con `WN_STATE_FOCUSED` (get_state devuelve 0x28); el click de puntero deja
sólo `FOCUSED`. `WN_STATE_FOCUS_WITHIN` (0x40) es derivado y de sólo lectura
para el host: el core lo enciende en el nodo enfocado y todos sus ancestros, y
lo recalcula al cambiar el foco, deshabilitar un subárbol, quitar/reinsertar
nodos. `wn_node_set_state` rechaza una máscara con 0x40. No viaja en el blob
(los estilos ya están por nodo). La prioridad fija por campo es
`focus-visible > focus > focus-within`.

**Diferido:** `:checked` (no hay widget checkbox/radio; el subset lo
diagnostica), transiciones/animaciones, opacidad o layout por estado salvo el
`font-weight`, y combinaciones de más de un estado salvo `:hover:active` (el
subset las diagnostica). Un estado en un compuesto **ancestro**
(`.card:hover .label`) tampoco se compila: el estado sólo se aplica al compuesto
sujeto y el subset lo diagnostica. Una regla de estado transporta colores
(`background`, `color` y color de borde uniforme) y `font-weight`; las demás
propiedades se diagnostican y no se aplican. El color de borde de estado es
uniforme (recolorea los lados visibles); no cambia ancho/estilo por lado.

### Sección `text_extras` (v6, aditiva)

La sección 6 es opcional y sólo aparece si algún run de texto lleva flags.
Contiene registros de 8 bytes en orden estricto de índice de nodo:

```
WnBlobTextExtras (8 bytes) int32 node; uint8 flags; uint8 reserved[3]
```

- `flags` bit 0 = `text-overflow: ellipsis` en una caja con `overflow: hidden`.
  El loader rechaza flags desconocidos, `flags == 0`, nodos fuera de rango,
  orden no creciente y `reserved` distinto de 0.
- El frontend escribe el flag en el **run de texto generado** del elemento, no
  en la caja propietaria. El runtime sólo lo aplica si el layout produce una
  **sola línea** y el texto excede el ancho; en multilínea es inerte. Para que
  una línea larga no envuelva, el fixture usa una caja flex column con un token
  sin espacios (o un ancho de texto declarado): `white-space: nowrap` no forma
  parte del subset y no se usa.
- No cambia el registro de nodo (252 bytes), ni la versión del blob.

### Semántica de `position` e insets (v4)

`inset_mask` bits 0..3 marcan qué lados están declarados (0 = `auto`). Los bits
4..7 marcan que ese lado es un porcentaje; el valor del porcentaje se guarda
como número (p. ej. `50.0`), **nunca** se hornea en px: el runtime lo resuelve
contra el containing block correcto en layout time. El loader valida el enum de
`position`, que no haya bits reservados, que un bit de porcentaje no exista sin
su lado declarado y que todo inset declarado sea finito.

- `relative`: el nodo se dispone en el flujo normal y después se desplaza él y
  su subárbol por `dx = left` si está declarado, si no `-right`; `dy = top` si
  está declarado, si no `-bottom`. Si ambos lados están declarados gana
  left/top. Los hermanos no se mueven.
- `absolute`: sale del flujo (block/flex/grid no lo cuentan ni avanzan el
  cursor; no altera el tamaño del padre). El containing block es el ancestro
  posicionado más cercano (`relative` o `absolute`) o el viewport (initial
  containing block) cuando no hay ninguno. Los insets se resuelven contra su
  **border box**; el layout ignora el ancho de borde, así que ese rect coincide
  con el padding box que define CSS. `left+right` con ancho `auto` estira;
  `top+bottom` con alto `auto` estira; un solo inset coloca ese borde; con los
  cuatro en `auto` el nodo se coloca en el origen del content box del containing
  block (regla simplificada de esta lane, no la static position completa de
  CSS). Los hijos absolutos anidados usan el ancestro posicionado más cercano,
  que puede ser el propio nodo absoluto.
- Porcentajes de `relative` se resuelven contra el containing block del flujo
  (ancho/alto del padre); si el alto es indefinido, un `top/bottom` porcentual
  resuelve a 0.
- **Clip:** un nodo absoluto usa el clip de los hijos de su containing block;
  así un `overflow` de un ancestro estático que no es el containing block no lo
  recorta (comportamiento CSS), mientras que el propio containing block con
  `overflow` sí lo recorta. Scroll, clip y hit-test consumen el mismo rect.
  El walk de pintado ya no poda un subárbol porque un ancestro tenga el clip
  vacío: sigue bajando y omite solo los nodos cuyo propio clip está vacío. Así
  un absoluto que escape de un ancestro **totalmente** recortado (p. ej.
  `relative > overflow:hidden(height:0) > static > absolute`) sí se pinta y
  responde al hit-test en su rect colocado.
- **Diferido:** `position: fixed`, `position: sticky` y el shorthand `inset`.
  `z-index` sí forma parte de v5 (ver abajo).
- **Límites conocidos de esta lane:** un porcentaje de `relative` dentro de un
  grid se resuelve contra el content box del contenedor, no contra el área del
  track (F4b); una imagen `absolute` con ancho `auto` y sin tamaño intrínseco
  resuelto colapsa a ancho 0 en vez del ancho por defecto de un elemento
  reemplazado (F4c). Los hijos fuera de flujo no contribuyen al overflow
  scrolleable del contenedor (F4d), igual que un `absolute` no altera el tamaño
  de su padre.

### Semántica de `z-index` y stacking contexts (v5)

`z-index: auto | <integer>`; el default es `auto` (sentinel `INT32_MIN`). El
frontend acepta enteros de 32 bits, incluidos 0 y negativos, y **diagnostica**
fracciones, unidades, valores fuera de rango y `-2147483648` (reservado para
`auto`). El loader trata `INT32_MIN` como `auto`; cualquier otro int32 vale.

Un **stacking context** lo crea: la raíz; un nodo con `opacity < 1` (v3); y un
nodo **posicionado** (`relative`/`absolute`) con `z-index != auto`. El motor
implementa el orden de pintado simplificado de CSS 2.1 Apéndice E (sin floats
ni cajas inline, que no existen en el subset):

1. el ítem propio del nodo raíz del contexto (fondo/borde/contenido y, si es un
   grupo de opacidad, su capa offscreen);
2. contextos hijos con `z < 0`, de menor a mayor `z` (empate: orden de árbol);
3. contenido en flujo del contexto en orden de árbol: solo los ítems **no
   posicionados** que **no** son contexto, cada uno en su posición del árbol;
4. bloques con `z == 0`/`auto` en orden de árbol: **todos** los grupos de
   `opacity < 1` (posicionados o no; CSS Color 3 §3.2 los pinta en la misma capa
   que un posicionado con `z: 0`), los contextos posicionados con `z: 0` y los
   ítems posicionados con `z-index: auto`, que aquí se emiten como bloques
   atómicos. Es el paso 8 de CSS 2.1, que pinta todos ellos juntos **por encima**
   del contenido en flujo del paso 3;
5. contextos hijos con `z > 0`, de menor a mayor `z` (empate: orden de árbol).

El bloque completo de un bloque de los pasos 2/4/5 se emite como una unidad: sus
descendientes **no pueden escapar** del contexto padre (un hijo con `z: 100`
dentro de un padre `z: 1` sigue debajo de un hermano del padre con `z: 2`). Los
contextos anidados componen recursivamente y el `z` se compara solo entre
hermanos de contexto.

**Simplificaciones documentadas:** `z-index` sobre un nodo `static` se
**ignora** (CSS solo lo aplica a posicionados y a ítems flex/grid; este lane no
crea contextos de ítem flex/grid). No hay floats, cajas inline ni
`position: fixed/sticky`. Un posicionado con `z-index: auto` no es un stacking
context en CSS, pero aquí se emite como un bloque atómico en el paso 4: sus
descendientes posicionados no se unen al contexto abuelo como haría CSS. El
caso normal (la caja y su subárbol sobre el contenido en flujo) coincide con el
paso 8 de CSS. Por la misma regla, un grupo de `opacity < 1` **no posicionado**
tampoco se pinta en el flujo: vive en el bucket del paso 8, en la misma capa que
un posicionado con `z: 0`.

**Capas:** un contexto de `z-index` **solo ordena**, no necesita textura. El
modelo de comandos distingue `needs_texture` (grupo de opacidad → target
offscreen y composición) de un contexto z-only (replay inline). Una página sin
`opacity < 1`, `z-index` ni posicionamiento conserva la ruta plana histórica por
categorías (byte-idéntica); en cuanto hay un grupo, un contexto z o un nodo
posicionado se usa la ruta ordenada por ítem. Un grupo recortado a nada
(`clips[nodo]` vacío) **no** reserva textura: su caja está recortada, pero un
absoluto que escapa conserva su propio clip no vacío y se pinta. Limitación
conocida: ese absoluto que escapa de un grupo `opacity < 1` totalmente recortado
no se atenúa con la opacidad del grupo.

### Semántica de `opacity` (v3)

Un nodo con `opacity < 1` es un **grupo CSS**: `opacity` crea un stacking
context, así que su caja y todo su subárbol se pintan en una superficie
offscreen y se componen una sola vez con alfa constante. Los hijos que se
solapan no se oscurecen dos veces, y los grupos anidados multiplican (0.5
dentro de 0.5 = 0.25). La regla vale también para **hojas**: una hoja puede
pintar varias pasadas (borde sobre fondo, sombra bajo relleno, cuerpo/ring/
selección/caret de un input, texto o imagen sobre fondo) y atenuar cada pasada
por separado no es lo mismo que atenuar el resultado aplanado. Por eso toda
hoja con `opacity < 1` también recibe su capa. `opacity: 1` mantiene la ruta
plana histórica: sin texturas extra y sin cambios de geometría. La
transformación dentro de una capa sigue siendo identidad en esta lane.

**Memoria:** cada nivel de anidamiento con `opacity < 1` usa una textura
offscreen de viewport completo, `W × H × 4` bytes, en ambos backends. Los
targets se reservan por profundidad y se reutilizan entre frames; el pico es
`profundidad_máxima_de_grupos × W × H × 4` (más el target de la surface). Un
grupo pequeño no reduce la textura: el recorte del clip se aplica al componer,
no al reservar.

**Orden de pintado:** una página sin ningún grupo ni contexto `z-index`
conserva el orden plano por categorías (todos los sólidos, después las
imágenes, después los glifos). En cuanto la página tiene al menos un grupo
(`opacity < 1`) **o** un contexto `z-index`, el replay dibuja por item
(sólido/decoración/imagen/texto de cada nodo, en el orden de stacking) para que
cada capa contenga su subárbol completo y los contextos z se ordenen. Es el
orden más cercano a CSS y es el precio de componer grupos y contextos
correctamente; no se puede mezclar el orden global por categorías con la
composición atómica de una capa. Los contextos z-only no reservan textura: solo
reordenan los ítems.

El runtime no tiene parser HTML/CSS. Carga el blob con
`wn_document_load_blob()` (archivo mapeado) o
`wn_document_load_blob_memory()` (builtin embebido). Un blob corrupto, truncado
o con un viewport/`font_size` fuera de rango se rechaza antes de construir el
core.

## `wnui-dump`

Herramienta C de debug (fuente en `src/runtime/tools/wn_dump.c`, objetivo CMake
`wnui-dump`). Usa el **mismo loader** que el runtime; no tiene parser propio.

```
wnui-dump <file.wnui>             # texto
wnui-dump --json <file.wnui>      # JSON con orden de claves estable
wnui-dump --help
```

Un archivo ausente o corrupto imprime `wnui-dump: <ruta>: <error del loader>` en
stderr y sale con código distinto de cero.

### Salida de texto

Primera línea:

```
format=wnui/6 viewport=960x640 nodes=28 inputs=0
```

Una línea por nodo, con nombres simbólicos estables para `kind` (`block`,
`flex`, `text`, `grid`, `image`, `input`), `direction`, `justify`, `align`,
`wrap`, `overflow`, `object_fit`, `text_align`, `action`, `grid`, `opacity`,
`position`, `inset`, `inset-mask` y `z-index` (`auto` o el número). Los colores salen como `#AARRGGBB`
(`#ff0b0e14`) y los floats con cuatro decimales (`%.4f`, sin `-0.0000`), con el
mismo formato estable del dump. Ejemplo recortado:

```
node 0 parent=-1 kind=flex name="body.root" direction=row justify=start align=stretch wrap=nowrap width=-1.0000 height=-1.0000 ... background=#ff0b0e14 ... opacity=1.0000 position=static inset=0.0000,0.0000,0.0000,0.0000 inset-mask=0 z-index=auto text=""
node 4 parent=1 kind=block name="body.root/div.sidebar/div.nav" ... action=select ... hover=#ff2a3140 press=#ff3a4a5c selected=#ff3498db ... text=""
```

`position` se imprime como nombre (`static`, `relative`, `absolute`); `inset`
conserva el orden top/right/bottom/left y el valor crudo (un porcentaje se ve
como su número, con el bit de porcentaje en `inset-mask`). `z-index` se imprime
como `auto` o como el entero explícito. `states` lista los estilos v6 como
`hover(bg=#ff…,text=#ff…,border=#ff…)` separados por comas, o `none`. Sólo se
imprimen los campos presentes en el blob. `initial-state` lista los bits con los
que arranca el nodo (`disabled`, o `none`), p. ej. `initial-state=disabled`.

Si hay inputs se añade una línea por input:

```
input node=2 value="" placeholder="Buscar..." max-length=12 password=0
```

### Salida JSON

Mismas claves y valores, orden fijo:

```json
{
  "format": "wnui/6",
  "viewport": { "width": 960, "height": 640 },
  "node_count": 28,
  "input_count": 0,
  "nodes": [
    {
      "index": 0,
      "parent": -1,
      "kind": "flex",
      "name": "body.root",
      "direction": "row",
      "...": "un campo por línea, siempre en el mismo orden"
    }
  ],
  "inputs": []
}
```

`inputs[]` lleva `node`, `value`, `placeholder`, `max_length` y `password`.
`state_styles[]` lleva `state` (`hover`/`press`/`focus`/`disabled`) y los campos
presentes (`background`, `text`, `border`) como `#AARRGGBB`. `initial_state`
lleva la misma lista que el texto (`"disabled"` o `"none"`).

### Invariante del test

`wn-cli test suite dump` emite el blob canónico, lo proyecta con `wnui-dump` y
compara el texto contra `src/runtime/tests/dump/canonical.txt`. El golden existe
para detectar regresiones silenciosas del dump; si el formato cambia a
propósito, se actualiza con el blob correspondiente. El gate primario es el hash
SHA-256 fijado en `PINNED_BLOBS` (`canonical`, `input`, `fidelity`, `pilot`, más
`dynamic`, `cache`, `css_card`/`css_control`/`css_none`,
`opacity`/`opacity_deep`, `position`, `z` y `state` en sus suites): una
emisión nueva que difiera falla con el hash esperado, el real y la ruta del
archivo. El suite `blob` sigue comparando
además píxel a píxel (`blob-vs-builtin max=0`).
