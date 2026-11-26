# Formato `.wnui` y `wnui-dump`

> **No es un contrato.** El documento `.wnui` v2 es el contrato *runtime* entre
> el frontend y el loader. `wnui-dump` es una **proyección de debug** de ese
> blob: su texto y su JSON pueden cambiar cuando cambia la versión del blob.
> La invariante estable es el hash SHA-256 del blob canónico y la comparación
> píxel a píxel (`blob` vs builtin), no la salida del dump.

## Secciones del blob

Cabecera de 32 bytes, little-endian:

| Campo | Tipo | Notas |
|---|---|---|
| `magic` | `u32` | `0x49554E57` (`WNUI`) |
| `version` | `u32` | `2` |
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
| 1 | nodos | `node_count` registros fijos de 220 bytes (`WnBlobNode`) |
| 2 | nombres | `node_count` offsets `u32` al pool de strings |
| 3 | strings | pool NUL-terminado; offset 0 = NULL |
| 4 | inputs | registros `WnBlobInput` (20 bytes); ausente si no hay inputs |

El registro de nodo guarda `parent`, `kind`, dirección/justify/align/wrap,
geometría declarada, márgenes/padding, colores ARGB resueltos, offsets de
texto/imagen/familia, la acción resuelta (`action`), grid, overflow,
borde/radius/sombra, alineación de texto, `line-height`, `margin-auto`,
`box-sizing`, flex y peso de fuente. No guarda los atributos `data-*` crudos: la
cascada ya los convirtió en ARGB y código de acción. El orden y ancho de los
campos están en `src/runtime/document/wn_blob.h`.

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
format=wnui/2 viewport=960x640 nodes=28 inputs=0
```

Una línea por nodo, con nombres simbólicos estables para `kind` (`block`,
`flex`, `text`, `grid`, `image`, `input`), `direction`, `justify`, `align`,
`wrap`, `overflow`, `object_fit`, `text_align`, `action` y `grid`. Los colores
salen como `#AARRGGBB` (`#ff0b0e14`) y los floats con cuatro decimales (`%.4f`,
sin `-0.0000`), con el mismo formato estable del dump. Ejemplo recortado:

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
  "format": "wnui/2",
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
`dynamic`, `cache` y `css_card`/`css_control`/`css_none` en sus suites): una
emisión nueva que difiera falla con el hash esperado, el real y la ruta del
archivo. El suite `blob` sigue comparando
además píxel a píxel (`blob-vs-builtin max=0`).
