# Formato `.wnui` y `wnui-dump`

> **No es un contrato.** El documento `.wnui` v3 es el contrato *runtime* entre
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
| `version` | `u32` | `3` |
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
| 1 | nodos | `node_count` registros fijos de 224 bytes (`WnBlobNode`) |
| 2 | nombres | `node_count` offsets `u32` al pool de strings |
| 3 | strings | pool NUL-terminado; offset 0 = NULL |
| 4 | inputs | registros `WnBlobInput` (20 bytes); ausente si no hay inputs |

El registro de nodo guarda `parent`, `kind`, dirección/justify/align/wrap,
geometría declarada, márgenes/padding, colores ARGB resueltos, offsets de
texto/imagen/familia, la acción resuelta (`action`), grid, overflow,
borde/radius/sombra, alineación de texto, `line-height`, `margin-auto`,
`box-sizing`, flex, peso de fuente y `opacity` (v3, `float` al final, 0..1;
el loader rechaza valores fuera de rango). No guarda los atributos `data-*`
crudos: la cascada ya los convirtió en ARGB y código de acción. El orden y
ancho de los campos están en `src/runtime/document/wn_blob.h`.

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

**Orden de pintado:** una página sin ningún grupo conserva el orden plano por
categorías (todos los sólidos, después las imágenes, después los glifos). En
cuanto la página tiene al menos un grupo (`opacity < 1`), el replay dibuja por
item (sólido/decoración/imagen/texto de cada nodo, en orden DFS) para que cada
capa contenga su subárbol completo. Es el orden más cercano a CSS y es el
precio de componer grupos correctamente; no se puede mezclar el orden global
por categorías con la composición atómica de una capa.

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
format=wnui/3 viewport=960x640 nodes=28 inputs=0
```

Una línea por nodo, con nombres simbólicos estables para `kind` (`block`,
`flex`, `text`, `grid`, `image`, `input`), `direction`, `justify`, `align`,
`wrap`, `overflow`, `object_fit`, `text_align`, `action`, `grid` y `opacity`.
Los colores salen como `#AARRGGBB` (`#ff0b0e14`) y los floats con cuatro
decimales (`%.4f`, sin `-0.0000`), con el mismo formato estable del dump.
Ejemplo recortado:

```
node 0 parent=-1 kind=flex name="body.root" direction=row justify=start align=stretch wrap=nowrap width=-1.0000 height=-1.0000 ... background=#ff0b0e14 ... text=""
node 4 parent=1 kind=block name="body.root/div.sidebar/div.nav" ... action=select ... hover=#ff2a3140 press=#ff3a4a5c selected=#ff3498db ... text=""
```

Si hay inputs se añade una línea por input:

```
input node=2 value="" placeholder="Buscar..." max-length=12 password=0
```

### Salida JSON

Mismas claves y valores, orden fijo:

```json
{
  "format": "wnui/3",
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

### Invariante del test

`wn-cli test suite dump` emite el blob canónico, lo proyecta con `wnui-dump` y
compara el texto contra `src/runtime/tests/dump/canonical.txt`. El golden existe
para detectar regresiones silenciosas del dump; si el formato cambia a
propósito, se actualiza con el blob correspondiente. El gate primario es el hash
SHA-256 fijado en `PINNED_BLOBS` (`canonical`, `input`, `fidelity`, `pilot`, más
`dynamic`, `cache`, `css_card`/`css_control`/`css_none` y
`opacity`/`opacity_deep` en sus suites): una
emisión nueva que difiera falla con el hash esperado, el real y la ruta del
archivo. El suite `blob` sigue comparando
además píxel a píxel (`blob-vs-builtin max=0`).
