# Media v1 — `<video>` de primera clase y códecs como proveedores

> **Estado:** **Fase A implementada (E3a) y primera mitad de la Fase B (E4).**
> El elemento `<video>`, el blob v7, la sección `media`, el registro de
> proveedores, `media_null`, la superficie `wn_node_media_*`, el **driver de
> sesión con la máquina de estados**, el proveedor sintético **`media_test`
> (solo tests)** y el **sink RGBA del primer frame** están en el árbol. **No**
> hay decodificador real, demuxer, sink de audio, subtítulos ni UI de
> reproducción: eso es E5 y siguientes. Cualquier implementación que contradiga
> este documento es un bug, no una variante.
>
> **Modo de hilos de E4:** el bucle de media corre **inline** desde
> `wn_core_media_tick` (UI), de forma determinista y sin locks. El modo con un
> hilo por sesión queda para E4b; la API y las reglas de propiedad de §3.6 ya
> describen ese destino.
>
> **Regla de oro:** `<video>` es un **elemento del runtime** (nodo, layout,
> pintado, eventos, estado). El **códec, el demuxer y el sink de audio son
> capacidades opcionales**. El motor nunca depende de FFmpeg para arrancar y
> nunca empaqueta un decodificador en el core.

## 0. Resumen de la decisión

| Decisión | Elegido | Alternativa rechazada |
|---|---|---|
| Identidad del elemento | `kind = 6` (`video`) en el blob v7, con sección `media` | reutilizar `kind:image`, flag opcional en el nodo |
| Atributos del elemento | sección opcional `media` (id 7), 16 bytes por nodo | crecer `WnBlobNode` (252 B) para todos los nodos |
| Versión del blob | **un** bump a `WN_BLOB_VERSION 7` | lector dual v6/v7, shims de compatibilidad |
| Decodificación | proveedores enchufables (`wn_media_provider_ops`) | FFmpeg dentro del core |
| Proveedor obligatorio | `media_null` (siempre presente, nunca acierta) | cadena opcional que puede quedar vacía |
| Superficie de pintado | `wn_video_sink_ops` (interno, renderer) | exponer D3D11/wgpu en la ABI pública |
| Reloj maestro | sink de audio; reloj de pared si no hay audio | reloj de vídeo mandando sobre el audio |
| Subtítulos | cues en el runtime; libass/PGS como módulos opcionales | subtítulos dentro del decodificador |
| Prioridad de fases | software primero; D3D11VA después; wgpu diferido | zero-copy desde el día uno |

El ABI público sube a `WN_ABI_VERSION 8` (aditivo: funciones nuevas, familia de
eventos nueva, ningún struct existente cambia) y estrena cabecera propia
`include/webnative_media.h` con `WN_MEDIA_ABI_VERSION 1`, versionada aparte
porque la compilan terceros.

---

## 1. Alcance y no-goals

### 1.1 Semántica de `<video>`

`<video>` es un **elemento reemplazado** con caja propia, como `<img>`:

- Participa del layout normal (block/flex/grid, `position`, `z-index`,
  `overflow`, `opacity`, `border-radius`). `object-fit` (`fill|cover|contain`)
  vive en el nodo, no en el renderer.
- Su tamaño intrínseco **no se hornea**: lo publica el runtime cuando el
  proveedor abre el contenedor (`OPENED`) o cuando el póster se decodifica.
  Orden de resolución: `width`/`height` declarados en CSS → tamaño intrínseco
  conocido (media info con rotación aplicada, o póster) → `300 × 150` (default
  de elemento reemplazado, igual que HTML).
- `poster` se pinta mientras no se haya presentado ningún frame de vídeo:
  antes de `play()`, en `IDLE`, en `ERROR` y durante `SEEKING` si nunca hubo
  frame. Una vez presentado un frame, el último frame queda congelado en
  pausa; no se vuelve al póster (comportamiento HTML).
- `loop` reinicia en `duration_us` sin pasar por `ENDED`.
- `muted` arranca con ganancia 0 y **sin sink de audio** (ver §5.4).
- `controls` es **intención del documento**; quién dibuja los controles es una
  pregunta abierta (§10.4). En la fase A el host dibuja su overlay.
- `autoplay` se honra siempre, también sin `muted`: esto es una app de
  escritorio, no un navegador, así que no hay política de gesto de usuario.
  Se documenta como desviación deliberada de la web.

Fuentes: **solo archivos locales**. `src` es una ruta (absoluta, relativa al
CWD del proceso — igual que `img` hoy — o UNC). Se rechaza cualquier cadena con
un esquema (`://`). No hay `<source>` hijos, ni `type`, ni `crossorigin`, ni
`MediaSource`, ni red, ni DRM, ni streaming adaptativo.

Sin JS: el host controla la reproducción con las funciones de §3.9. El
documento solo declara intención (`autoplay`, `loop`, `muted`, `controls`,
`preload`).

### 1.2 Qué significa "proveedor opcional"

Un **proveedor** implementa demux + decode. Es una capacidad del *proceso*, no
del documento:

- El documento no nombra códecs ni proveedores. El mismo `.wnui` abre en una
  build con FFmpeg y en una build sin él; la diferencia es qué archivos
  reproducen, no qué documento es válido.
- La cadena de proveedores siempre termina en `media_null`, que **nunca
  acierta** y produce un error tipado. Así "no hay códec" es un camino
  normal y testeable, no un `NULL` sin explicación.
- Un build sin ningún proveedor real sigue siendo válido: el motor compila,
  carga documentos con `<video>`, pinta póster/fondo y emite
  `WN_MEDIA_EVENT_ERROR` al intentar abrir.

### 1.3 No-goals de v1

- Red, HTTP/HLS/DASH, `MediaSource`, DRM (Widevine/PlayReady), captura de
  cámara/micrófono.
- Transcoding, filtros, ecualizador, efectos de audio.
- Multi-pista de vídeo simultánea (>1 stream de vídeo activo).
- `playbackRate != 1.0` (la superficie interna reserva
  `wn_media_session_set_rate`, que rechaza todo salvo `1.0` en las fases A–C).
- Subtítulos con estilos arbitrarios, karaoke, fuentes embebidas, bidi/RTL
  (limitación heredada del motor de texto).
- HDR real (salida PQ/scRGB) hasta la fase D.
- Reproducir audio sin un `<video>` que lo posea (no hay elemento `<audio>` en
  v1; un archivo solo-audio se abre igual en un nodo `video`).

### 1.4 El audio no se pinta

El audio **no entra en la display list**, no genera vértices, no toca el
atlas ni las texturas. Tiene su propio camino:

```
paquete → decode → resample → ring SPSC → sink (WASAPI/miniaudio) → dispositivo
```

El único contacto entre audio y pintado es el **reloj**: el sink de audio
publica la posición audible y el vídeo se agenda contra ella (§3.7, §5.2).

---

## 2. Blob v7

### 2.1 Kind y registro de nodo

`WnBlobNode` **no cambia**: sigue midiendo 252 bytes y no gana campos. Un
`<video>` se identifica por:

```
kind = 6   /* WN_BLOB_NODE_VIDEO / R2_NODE_VIDEO / KIND_VIDEO */
```

`WnBlobNode` es el registro genérico de caja y estilo (geometría, colores,
flex, `position`, `z_index`, `opacity`, `object_fit`…). Todo eso aplica a
`<video>` sin cambios. Lo que es específico de media viaja en la sección 7.

El nodo declara además, obligatoriamente:

| Campo del nodo | Valor en `video` |
|---|---|
| `kind` | `6` |
| `object_fit` | `0/1/2` (`fill/cover/contain`), igual que `img` |
| `image_src` | **0** (el loader lo rechaza si no es 0) |
| `image_alt` | **0** (idem) |
| `image_width`, `image_height` | **0** (el tamaño intrínseco es runtime) |
| resto | igual que cualquier caja |

Reusar `image_src` para la ruta del vídeo sería exactamente el hack que este
documento prohíbe (§2.6).

### 2.2 Sección `media` (id 7)

Sección opcional, **ausente** si el documento no tiene ningún nodo `video`.
Secuencia de registros de 16 bytes en orden estricto de índice de nodo
(creciente, sin duplicados), uno por cada nodo `video`:

```c
/* src/runtime/document/wn_blob.h — blob v7 */
#define WN_BLOB_SECTION_MEDIA 7u

typedef struct {
    int32_t  node;      /* índice del nodo; kind debe ser 6 */
    uint32_t src;       /* strings offset; 0 = sin fuente todavía */
    uint32_t poster;    /* strings offset; 0 = sin póster */
    uint8_t  flags;     /* WN_MEDIA_FLAG_* */
    uint8_t  preload;   /* 0 none, 1 metadata (default), 2 auto */
    uint8_t  reserved[2]; /* 0 */
} WnBlobMediaRecord;   /* 16 bytes */

#define WN_MEDIA_FLAG_CONTROLS 0x01u
#define WN_MEDIA_FLAG_AUTOPLAY 0x02u
#define WN_MEDIA_FLAG_LOOP     0x04u
#define WN_MEDIA_FLAG_MUTED    0x08u
/* bits 4..7 reservados: el loader los rechaza */
```

`preload` replica el atributo HTML:

| Valor | Nombre | Efecto en runtime |
|---|---|---|
| 0 | `none` | no se abre sesión hasta `play()`; se pinta el póster |
| 1 | `metadata` | se abre y se hace `probe` (duración/streams) al cargar; sin decode |
| 2 | `auto` | se abre, `probe` y se decodifica el primer frame (queda en pausa) |

El default es `1`: el póster se ve y la duración está disponible para el
overlay del host sin gastar decode.

**Nodo reproductor sin fuente (`src = 0`).** Un `<video>` puede declararse sin
`src` para que el host lo use como reproductor: el documento reserva la caja, el
póster y el layout, y el host llama `wn_node_media_set_source` antes de
`open`/`play`. No es un hack de "imagen con ruta de vídeo": sigue siendo
`kind = 6`, con su sección `media` obligatoria, sus flags y su ciclo de vida.
`poster` es opcional. Sin fuente, el nodo pinta póster o placeholder y
`wn_node_media_open` devuelve `WN_ERR_STATE` (y emite `WN_MEDIA_EVENT_ERROR`
con `detail`), sin crash. `preload`/`autoplay` no abren un nodo sin fuente.
Esto es lo que la galería necesita para congelar/descongelar su documento sin
hornear una ruta de reproducción (ver §11). El frontend emite `kind = 6` y un
registro `media` con `src = 0` cuando el `<video>` no declara `src`.

### 2.3 Validación del loader

Además de las validaciones actuales (orden, límites, offsets de strings), el
loader de v7 exige:

1. `version == 7`. Cualquier otra versión se rechaza con
   `wn_last_error() = "bad magic or version"`. Sin lector dual.
2. Si existe la sección 7:
   - tamaño múltiplo de 16, al menos un registro;
   - `node` en rango, estrictamente creciente;
   - `kind == 6` para ese nodo;
   - `flags & 0xF0 == 0`; `preload <= 2`; `reserved == 0`;
   - `src` es **0 (sin fuente todavía) o un offset no vacío sin `://`**. El
     registro `media` sigue siendo obligatorio: un nodo `video` sin registro es
     un blob corrupto. Un offset no-cero vacío o con esquema se rechaza;
   - `poster` válido como offset (0 permitido).
3. Todo nodo `kind == 6` tiene **exactamente** un registro en la sección 7.
   Un `video` sin registro, o un registro sobre un nodo que no es `video`, es
   un blob corrupto.
4. `image_src == 0`, `image_alt == 0`, `image_width == 0`, `image_height == 0`
   para nodos `video` (evita que el frontend hornee metadatos de imagen o
   tamaños adivinados).
5. La sección 7 no aparece si no hay nodos `video`.

`wnui-dump` imprime `kind=video` y una línea `media` por nodo; su formato sigue
sin ser contrato.

### 2.4 Lo que NO va en el blob

| Dato | Por qué no |
|---|---|
| proveedor/decodificador | es configuración de instalación, no contenido |
| códec, fourcc, perfil, nivel | lo descubre el proveedor al hacer `probe` |
| contenedor, duración, fps, timebase | metadatos de runtime; cambian con el archivo |
| ancho/alto intrínseco, rotación, HDR | ídem; se publican en `OPENED` |
| frame, textura, `texture_id`, superficie | estado vivo por vista; el blob es inmutable y `mmap` |
| sesión, `handle`, cola, reloj | estado vivo |
| posición, volumen, mute efectivo, pista de subtítulos | preferencias/estado del host |
| sink de audio, dispositivo, latencia | plataforma |
| errores, último estado | runtime |
| pista de subtítulos seleccionada, `subtitle_delay_ms` | preferencia de usuario |

El blob es un **documento**: inmutable, mapeado en solo lectura, compartido por
varias vistas y potencialmente por varios procesos. Meter estado de
reproducción ahí rompe esa propiedad y obliga a remapear/reescribir el archivo.

### 2.5 Política de versión y migración

**Un solo bump: `WN_BLOB_VERSION` de 6 a 7.** No hay bump por feature dentro
de la ola; los campos nuevos de media son aditivos en la sección 7 y no
vuelven a tocar la versión.

El bump es deliberado aunque la sección sea opcional, porque el contrato gana
un **kind de elemento** nuevo: un loader v6 rechazaría el documento con
"unknown kind", que es un diagnóstico peor que "wrong document version". El
número de versión identifica el contrato completo y hace que un toolchain a
medias falle en el sitio correcto.

Consecuencias, todas en el **mismo commit/PR**:

1. `src/runtime/document/wn_blob.h`: `WN_BLOB_VERSION 7`, sección 7 y
   `WnBlobMediaRecord`.
2. `WnBlobNode` **no** cambia de tamaño (252 B). `BLOB_NODE` del frontend Rust
   tampoco.
3. Todos los blobs fijados cambian, porque la cabecera lleva la versión:
   los 18 hashes de `PINNED_BLOBS` (`tools/wn_cli/suites.py`), el builtin
   embebido (`generated/builtin_blob.h`) y el golden
   `src/runtime/tests/dump/canonical.txt` (la primera línea pasa a
   `format=wnui/7`).
4. `docs/wnui-format.md` y `docs/architecture.md` actualizan v6→v7 en la misma
   entrega.
5. Los fixtures se regeneran, no se parchean a mano. El hash nuevo se revisa
   una vez (un punto de auditoría en lugar de dieciocho silenciosos).

**Prohibido:** lector que acepte 6 y 7, campo `version_min`, sección `media`
en v6 "si cabe", o un `#ifdef` de compatibilidad. Pre-1.0 se rompe limpio.

### 2.6 Hacks rechazados (explícito)

| Hack | Por qué se rechaza |
|---|---|
| `kind:image` con `src = "x.mp4"` y sniff por extensión | La identidad del elemento decide hit-test, estado, controles, audio, ciclo de vida y accesibilidad. `wn_node_set_image` "funcionaría" sobre un vídeo y la caché de imágenes (clave = ruta) mezclaría póster y vídeo. |
| Flag opcional `is_media` dentro del nodo imagen | Obliga a ramificar en todos los consumidores igualmente, crece el registro para todas las imágenes y no hay sitio para `controls/autoplay/loop/muted/preload` sin una tabla lateral. |
| Tabla lateral keyed por `node` sin kind nuevo | El nodo seguiría siendo `image`: layout/paint no sabrían que es reemplazado con vídeo, y `set_image`/`set_image_size` seguirían aceptándolo. |
| Meter el códec o el proveedor como string en el blob | Convierte una decisión de instalación en contenido, y crea una superficie de compatibilidad para nombres de códec. |
| Hornear duración/ancho/alto/rotación en el blob | Acopla el compilador a un demuxer, hace el blob dependiente del host y miente si el archivo cambia. |
| Guardar posición/textura/sesión en el blob | El blob es inmutable y compartido; además crece y se reescribe por frame. |
| Nodo `video` que en realidad pinta un `<img>` de póster y delega el playback al host | El host ya puede hacer eso **hoy** con un `img`; no es `<video>`, no hay reloj, ni `loop`, ni subtítulos, ni `object-fit` de vídeo. Se rechaza como sustituto. |

---

## 3. Arquitectura de runtime

### 3.1 Módulos y handles opacos

```
core (C, sin GPU, sin OS)
  └── wn_media_host         registro + sesiones, propiedad de wn_view
        ├── registry        cadena de proveedores, prioridades, plugins
        ├── session         estado, hilos, colas, reloj, scheduling
        └── sinks           vídeo (renderer) y audio (plataforma)

provider (opcional, DLL o compilado)   sinks (renderer / OS)
  demux + decode                          upload / import / present / write
```

Handles, todos opacos e inválidos fuera de su propietario:

```c
typedef struct wn_media_registry wn_media_registry; /* wn_view */
typedef struct wn_media_session  wn_media_session;  /* un nodo <video> vivo */
typedef struct wn_media_packet   wn_media_packet;   /* token del proveedor */
```

`wn_media_packet` no expone bytes al motor: el runtime lo trata como un *token*
que devuelve al mismo proveedor en `decode`. El motor nunca interpreta datos
de códec.

### 3.2 ABI pública de proveedor — `include/webnative_media.h`

Cabecera propia, C puro, sin tipos de C++/STL/.NET, sin excepciones, sin
`wchar_t`. Todo struct empieza por `uint32_t struct_size` (append-only) y toda
enumeración de *kind* reserva 0 para "desconocido".

```c
#define WN_MEDIA_ABI_VERSION 1u

typedef enum wn_media_status {
    WN_MEDIA_OK = 0,
    WN_MEDIA_AGAIN = 1,                 /* reintenta más tarde */
    WN_MEDIA_EOF = 2,
    WN_MEDIA_UNSUPPORTED_CONTAINER = 3,
    WN_MEDIA_UNSUPPORTED_CODEC = 4,
    WN_MEDIA_IO = 5,
    WN_MEDIA_CORRUPT = 6,
    WN_MEDIA_OOM = 7,
    WN_MEDIA_CANCELLED = 8,
    WN_MEDIA_GPU = 9,
    WN_MEDIA_INTERNAL = 10
} wn_media_status;
```

```c
typedef enum wn_container_kind {
    WN_CONTAINER_UNKNOWN = 0, WN_CONTAINER_MP4, WN_CONTAINER_MKV,
    WN_CONTAINER_WEBM, WN_CONTAINER_OGG, WN_CONTAINER_AVI
} wn_container_kind;

typedef enum wn_codec_kind {
    WN_CODEC_UNKNOWN = 0,
    WN_CODEC_H264, WN_CODEC_HEVC, WN_CODEC_VP8, WN_CODEC_VP9, WN_CODEC_AV1,
    WN_CODEC_AAC, WN_CODEC_OPUS, WN_CODEC_VORBIS, WN_CODEC_MP3, WN_CODEC_FLAC
} wn_codec_kind;

#define WN_MEDIA_BIT(kind) (1u << (kind))

typedef enum wn_pixel_format {
    WN_PIXEL_NONE = 0,
    WN_PIXEL_NV12 = 1,      /* 8-bit 4:2:0, 2 planos (Y + UV interleaved) */
    WN_PIXEL_P010 = 2,      /* 10-bit 4:2:0 en 16 bits, 2 planos */
    WN_PIXEL_RGBA8 = 3,     /* 8-bit RGBA, 1 plano */
    WN_PIXEL_NATIVE = 16    /* superficie GPU opaca; ver native_kind */
} wn_pixel_format;

typedef enum wn_sample_format {
    WN_SAMPLE_NONE = 0, WN_SAMPLE_F32 = 1, WN_SAMPLE_S16 = 2
} wn_sample_format;

/* Discriminante de la superficie nativa. Nunca se incluye la cabecera del
 * backend: el puntero es opaco para el motor y solo lo consume el sink. */
typedef enum wn_native_kind {
    WN_NATIVE_NONE = 0, WN_NATIVE_D3D11 = 1, WN_NATIVE_D3D12 = 2,
    WN_NATIVE_VULKAN = 3, WN_NATIVE_METAL = 4
} wn_native_kind;

/* Máscara de color empaquetada en wn_video_frame.color. */
#define WN_MEDIA_COLOR(matrix, range, primaries, transfer) \
    ((uint32_t)(matrix) | ((uint32_t)(range) << 8) | \
     ((uint32_t)(primaries) << 16) | ((uint32_t)(transfer) << 24))
typedef enum wn_color_matrix {
    WN_MATRIX_BT601 = 0, WN_MATRIX_BT709 = 1, WN_MATRIX_BT2020_NCL = 2
} wn_color_matrix;
typedef enum wn_color_range { WN_RANGE_LIMITED = 0, WN_RANGE_FULL = 1 } wn_color_range;
typedef enum wn_color_primaries {
    WN_PRIMARIES_BT709 = 0, WN_PRIMARIES_BT2020 = 1, WN_PRIMARIES_DISPLAY_P3 = 2
} wn_color_primaries;
typedef enum wn_transfer_fn {
    WN_TRANSFER_SDR = 0, WN_TRANSFER_PQ = 1, WN_TRANSFER_HLG = 2
} wn_transfer_fn;
```

Capacidades y descubrimiento:

```c
typedef struct wn_media_caps {
    uint32_t struct_size;
    uint32_t flags;            /* WN_MEDIA_CAP_* */
    uint32_t containers;       /* WN_MEDIA_BIT(wn_container_kind) */
    uint32_t video_codecs;     /* WN_MEDIA_BIT(wn_codec_kind) */
    uint32_t audio_codecs;
    uint32_t pixel_formats;    /* WN_MEDIA_BIT(wn_pixel_format) */
    uint32_t max_video_width, max_video_height;
    uint32_t max_audio_channels;
} wn_media_caps;

#define WN_MEDIA_CAP_ASYNC        0x0001u /* decodifica fuera del hilo de UI */
#define WN_MEDIA_CAP_HW_DECODE    0x0002u /* produce WN_PIXEL_NATIVE */
#define WN_MEDIA_CAP_SUBTITLES    0x0004u /* expone streams TEXT/BITMAP */
#define WN_MEDIA_CAP_AUDIO        0x0008u
#define WN_MEDIA_CAP_SEEK         0x0010u
#define WN_MEDIA_CAP_HDR          0x0020u
```

Streams e info de contenedor:

```c
typedef enum wn_stream_type {
    WN_STREAM_UNKNOWN = 0, WN_STREAM_VIDEO, WN_STREAM_AUDIO,
    WN_STREAM_TEXT, WN_STREAM_BITMAP
} wn_stream_type;

typedef struct wn_media_stream_info {
    uint32_t struct_size;
    uint32_t type;                 /* wn_stream_type */
    uint32_t codec;                /* wn_codec_kind */
    int32_t  index;                /* índice del proveedor */
    int64_t  duration_us;
    int32_t  width, height;        /* vídeo: coded */
    int32_t  display_width, display_height; /* tras rotación y PAR */
    uint32_t rotation_deg;         /* 0/90/180/270 */
    int32_t  sample_rate, channels;/* audio */
    uint32_t color;                /* WN_MEDIA_COLOR(...) */
    char     language[4];          /* ISO-639-2 + NUL; "" si no hay */
} wn_media_stream_info;

typedef struct wn_media_info {
    uint32_t struct_size;
    uint32_t flags;                /* WN_MEDIA_INFO_* */
    uint32_t container;            /* wn_container_kind */
    uint32_t stream_count;
    int64_t  duration_us;
    wn_media_stream_info video;    /* primer stream de vídeo, o type=0 */
    wn_media_stream_info audio;    /* primer stream de audio, o type=0 */
} wn_media_info;

#define WN_MEDIA_INFO_SEEKABLE 0x01u
#define WN_MEDIA_INFO_VFR      0x02u
#define WN_MEDIA_INFO_HDR      0x04u
```

`probe` es barato (cabecera + índice, sin decode) y **debe** costar menos de
16 ms; el runtime lo llama desde el hilo de media, nunca desde el de UI.

Frames, paquetes y *leases*:

```c
/* El runtime llama release() exactamente una vez, desde cualquier hilo,
 * después de que el sink termine con el frame. Debe ser thread-safe y no
 * bloquear. */
typedef struct wn_frame_lease {
    void  *object;
    void  (*release)(void *context, void *object);
    void  *context;
} wn_frame_lease;

typedef struct wn_media_plane {
    const uint8_t *data;   /* NULL en un frame nativo */
    int32_t stride;        /* bytes por fila */
    int32_t width, height; /* muestras del plano */
} wn_media_plane;

typedef struct wn_video_frame {
    uint32_t struct_size;
    uint32_t format;                /* wn_pixel_format */
    int32_t  coded_width, coded_height;
    int32_t  display_width, display_height; /* rotación + PAR aplicados */
    uint32_t rotation_deg;
    uint32_t planes;                /* 1..3 */
    wn_media_plane plane[3];
    int64_t  pts_us;
    int64_t  duration_us;
    uint32_t flags;                 /* WN_FRAME_* */
    uint32_t color;                 /* WN_MEDIA_COLOR(...) */
    void    *native_texture;        /* WN_PIXEL_NATIVE: opaco, nunca deref */
    uint32_t native_kind;           /* wn_native_kind */
    uint32_t native_slice;          /* slice del array (D3D11VA DPB) */
    wn_frame_lease lease;
} wn_video_frame;

#define WN_FRAME_KEY      0x01u
#define WN_FRAME_EOS      0x02u
#define WN_FRAME_CORRUPT  0x04u
#define WN_FRAME_OVERLAY  0x08u  /* subtítulo bitmap (PGS/VobSub) */

typedef struct wn_audio_frame {
    uint32_t struct_size;
    uint32_t sample_format;         /* wn_sample_format */
    int32_t  sample_rate;
    int32_t  channels;
    uint32_t channel_mask;          /* máscara WAVE; 0 = por defecto */
    int32_t  frame_count;           /* muestras por canal */
    const void *data;               /* interleaved: frame_count * channels */
    int64_t  pts_us;
    wn_frame_lease lease;
} wn_audio_frame;

typedef struct wn_media_packet {
    uint32_t struct_size;
    uint32_t stream;                /* índice de wn_media_stream_info.index */
    uint32_t type;                  /* wn_stream_type */
    uint32_t flags;                 /* WN_PACKET_* */
    int64_t  pts_us;
    int64_t  duration_us;
    void    *token;                 /* opaco; válido hasta decode()/flush() */
} wn_media_packet;

#define WN_PACKET_KEY 0x01u
#define WN_PACKET_EOS 0x02u
```

Descripción de apertura y la vtable del proveedor:

```c
typedef struct wn_media_open_desc {
    uint32_t struct_size;
    const char *path_utf8;          /* válida durante open(); el proveedor copia */
    uint32_t flags;                 /* WN_MEDIA_OPEN_* */
    uint32_t allowed_formats;       /* formatos que el sink acepta; 0 = cualquiera CPU */
    uint32_t native_kind;           /* WN_NATIVE_* del sink; 0 = sin HW */
    void    *native_device;         /* opaco; solo para decodificación HW */
    int32_t  max_width, max_height; /* techo de resolución */
    int32_t  audio_sample_rate;     /* 0 = el del dispositivo */
    int32_t  audio_channels;
} wn_media_open_desc;

#define WN_MEDIA_OPEN_VIDEO_OFF 0x01u
#define WN_MEDIA_OPEN_AUDIO_OFF 0x02u
#define WN_MEDIA_OPEN_LOOP      0x04u

typedef struct wn_media_provider_ops {
    uint32_t struct_size;           /* sizeof(wn_media_provider_ops) */
    uint32_t abi_version;           /* WN_MEDIA_ABI_VERSION */
    const char *name;               /* "null", "ffmpeg-lgpl", "mf", ... */

    void (*caps)(void *context, wn_media_caps *out);
    wn_media_status (*probe)(void *context, const char *path_utf8,
                             wn_media_info *out);
    wn_media_status (*open)(void *context, const wn_media_open_desc *desc,
                            wn_media_session **out_session);
    wn_media_status (*streams)(void *context, wn_media_session *session,
                               wn_media_stream_info *out, uint32_t capacity,
                               uint32_t *out_count);
    wn_media_status (*read)(void *context, wn_media_session *session,
                            wn_media_packet *out);
    wn_media_status (*seek)(void *context, wn_media_session *session,
                            int64_t pts_us, uint32_t flags);
    wn_media_status (*decode)(void *context, wn_media_session *session,
                              const wn_media_packet *packet);
    wn_media_status (*receive_video)(void *context, wn_media_session *session,
                                     wn_video_frame *out);
    wn_media_status (*receive_audio)(void *context, wn_media_session *session,
                                     wn_audio_frame *out);
    wn_media_status (*flush)(void *context, wn_media_session *session);
    wn_media_status (*close)(void *context, wn_media_session *session);
    void (*destroy)(void *context);  /* libera el contexto del proveedor */
} wn_media_provider_ops;
```

Reglas de la vtable:

- `open` devuelve `WN_MEDIA_UNSUPPORTED_*` si tras `probe` el contenedor o el
  códec no son suyos; la cadena prueba el siguiente proveedor.
- `read` devuelve `WN_MEDIA_AGAIN` cuando no hay paquete inmediato; `EOF` al
  final; `CANCELLED` si `close`/`flush` está en curso.
- `decode` **no** bloquea más de 100 ms. Si el decodificador tiene su propio
  hilo, `receive_*` devuelve `AGAIN` mientras no haya frame.
- `seek` debe aceptar cualquier `pts_us` y colocar el siguiente `read` en el
  keyframe anterior o igual; el runtime descarta los frames con
  `pts < target`.
- `flush` vacía paquetes y frames pendientes; tras él, el runtime descarta
  todo frame con `serial` anterior (ver §3.6).
- `close` libera la sesión del proveedor; el runtime sigue siendo dueño del
  handle `wn_media_session *` que devolvió `open` solo hasta `close`.
- Ninguna función puede dejar escapar una excepción de C++ ni usar `std::`
  en la firma. Un proveedor C++ está obligado a envolver todo en `catch(...)`
  y devolver `WN_MEDIA_INTERNAL`.
- El proveedor **no** llama callbacks del motor. Todo es *pull*.

### 3.3 Sinks (interno) — `src/runtime/wn_media.h`

Los sinks no están en la cabecera pública: el host no los implementa y la ABI
pública no expone tipos de GPU.

```c
typedef struct wn_media_upload_stats {
    uint32_t struct_size;
    uint32_t uploads;         /* frames subidos desde el arranque */
    uint32_t imports;         /* texturas nativas importadas */
    uint32_t dropped;         /* frames descartados por el scheduler */
    double   last_upload_ms;
} wn_media_upload_stats;

typedef struct wn_video_sink_ops {
    uint32_t struct_size;
    uint32_t abi_version;              /* WN_MEDIA_SINK_ABI 1 */
    const char *name;
    uint32_t (*formats)(void *context);        /* máscara wn_pixel_format */
    uint32_t (*native_kind)(void *context);    /* WN_NATIVE_*; 0 = solo CPU */
    int (*device)(void *context, void **out_device); /* device opaco o NULL */
    /* Ruta software: sube planos NV12/P010/RGBA a una textura del renderer. */
    wn_media_status (*upload)(void *context, const wn_video_frame *frame,
                              uint64_t *out_texture);
    /* Ruta hardware: importa una textura nativa ya creada por el decodificador. */
    wn_media_status (*import_native)(void *context, void *native_texture,
                                     uint32_t slice, const wn_video_frame *desc,
                                     uint64_t *out_texture);
    void (*release_texture)(void *context, uint64_t texture);
    void (*stats)(void *context, wn_media_upload_stats *out);
} wn_video_sink_ops;

typedef struct wn_audio_sink_ops {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *name;
    int (*open)(void *context, int32_t sample_rate, int32_t channels,
                uint32_t sample_format, uint32_t channel_mask);
    int (*write)(void *context, const wn_audio_frame *frame); /* puede bloquear */
    int (*pause)(void *context, int paused);
    int (*flush)(void *context);
    int (*set_gain)(void *context, float gain);   /* 0..1 */
    /* Posición audible en µs del timeline del stream, o -1 si no hay reloj. */
    int64_t (*clock_us)(void *context);
    int64_t (*latency_us)(void *context);
    int (*state)(void *context);                  /* WN_AUDIO_SINK_* */
    void (*close)(void *context);
} wn_audio_sink_ops;

#define WN_AUDIO_SINK_OK        0
#define WN_AUDIO_SINK_UNDERRUN  1
#define WN_AUDIO_SINK_DEVICE_LOST 2
#define WN_AUDIO_SINK_ERROR     3
```

`texture` es un `uint64_t` opaco, monótono, propiedad del renderer. `0` es
inválido. Vale hasta `release_texture` o hasta destruir el renderer. El motor
nunca lo interpreta: solo lo copia al `wn_video_batch` (§4.2).

### 3.4 Registro y descubrimiento

```c
/* src/runtime/wn_media.h */
#define WN_MEDIA_PRIORITY_USER    1000   /* registro explícito del host */
#define WN_MEDIA_PRIORITY_PLUGIN   500   /* DLL cargada por allowlist */
#define WN_MEDIA_PRIORITY_COMPILED 100   /* módulo compilado en la build */
#define WN_MEDIA_PRIORITY_NULL       0   /* terminador; nunca acierta */

int  wn_media_registry_add(wn_media_registry *r,
                           const wn_media_provider_ops *ops, void *context,
                           int32_t priority);
int  wn_media_registry_load(wn_media_registry *r, const char *dll_path);
void wn_media_registry_query(const wn_media_registry *r, wn_media_caps *out);
```

Cadena determinista:

1. Orden descendente de `priority`; empate → orden de registro.
2. `media_null` se añade siempre al final con `WN_MEDIA_PRIORITY_NULL`, no es
   removible y no puede duplicarse.
3. Selección por sesión: se recorre la cadena y se elige el primer proveedor
   cuyo `caps` incluya contenedor y códec **y** cuyo `probe` sobre el archivo
   concreto devuelva `WN_MEDIA_OK`. Si ninguno acierta, gana `media_null` y su
   error tipado.
4. La selección se hace **por archivo**, nunca por nodo ni por documento. Dos
   `<video>` con el mismo archivo pueden acabar en proveedores distintos si el
   primero falló y el segundo no.

**Plugins dinámicos.** Un `.dll` solo se carga si:

- el host lo pide explícitamente (`wn_app_load_media_provider`, §3.9), o
- aparece en la allowlist de configuración del proceso
  (`WN_MEDIA_PLUGINS`, rutas separadas por `;`), o
- está en `%LOCALAPPDATA%\webnative\media\` con el prefijo `wn_media_`.

Nunca se carga un plugin a partir de un archivo multimedia, de `src`, de un
`.wnui` ni de una ruta que venga del documento. Un vídeo no puede provocar la
carga de código.

Símbolo único exportado:

```c
/* exportado por el plugin */
int wn_media_provider_query(uint32_t abi_version,
                            const wn_media_provider_ops **out_ops,
                            void **out_context);
```

Handshake, en este orden y con error legible en `wn_last_error()`:

1. `abi_version != WN_MEDIA_ABI_VERSION` → rechazo.
2. `ops->struct_size < offsetof(...receive_audio) + sizeof(void*)` → rechazo
   (faltan operaciones obligatorias).
3. `ops->abi_version != WN_MEDIA_ABI_VERSION` → rechazo.
4. `name` vacío o `caps`/`open`/`close`/`destroy` nulos → rechazo.
5. Se copia la vtable y el contexto; el motor nunca libera memoria del plugin
   y el plugin nunca libera memoria del motor.

### 3.5 Sesión: ciclo de vida y máquina de estados

```c
typedef enum wn_media_state {
    WN_MEDIA_STATE_IDLE = 0,       /* sin sesión; se pinta el póster */
    WN_MEDIA_STATE_OPENING = 1,
    WN_MEDIA_STATE_PAUSED = 2,     /* lista, posición fija */
    WN_MEDIA_STATE_PLAYING = 3,
    WN_MEDIA_STATE_BUFFERING = 4,  /* faltan datos; último frame congelado */
    WN_MEDIA_STATE_SEEKING = 5,
    WN_MEDIA_STATE_ENDED = 6,
    WN_MEDIA_STATE_ERROR = 7       /* terminal hasta open/set_source */
} wn_media_state;
```

`OPENED` se alcanza desde `OPENING` y desemboca en `PAUSED` (posición 0, sin
frame presentado salvo `preload=auto`). No hay estado `OPEN` separado: "abierto
y en pausa" es el mismo estado y evita transiciones ambiguas.

| Desde | Disparo | A | Efecto |
|---|---|---|---|
| `IDLE` | carga con `preload>=metadata` o `play()` | `OPENING` | crea sesión en el hilo de media |
| `OPENING` | `OPENED` | `PAUSED` | publica `duration_us`, streams, tamaño intrínseco |
| `OPENING` | error | `ERROR` | evento `ERROR`; se pinta póster |
| `PAUSED` | `play()` | `PLAYING` | arranca/ancla el reloj, `audio->pause(0)` |
| `PLAYING` | `pause()` | `PAUSED` | congela el reloj, `audio->pause(1)` |
| `PLAYING` | cola de vídeo vacía y `!eof` | `BUFFERING` | último frame congelado; reloj de audio sigue |
| `BUFFERING` | `frames_ready >= 2` o `eof` | `PLAYING` | el reloj nunca salta hacia delante |
| `*` | `seek(t)` | `SEEKING` | `flush`, `seek`, limpia colas, `serial++` |
| `SEEKING` | primer frame con `pts >= t` | `PAUSED`/`PLAYING` | re-ancla el reloj en el frame |
| `PLAYING`/`BUFFERING` | `eof` y cola vacía | `ENDED` | evento `ENDED`; si `loop`, vuelve a 0 y `PLAYING` |
| `*` | error del proveedor/sink | `ERROR` | evento `ERROR`; sin reintento automático |
| `ERROR` | `open()` / `set_source()` | `OPENING` | nueva sesión; se conserva el póster |

Invariantes:

- Toda transición ocurre en el **hilo de UI** (dentro de `tick`), salvo el
  paso interno `PLAYING→BUFFERING` del hilo de media, que solo publica un flag
  atómico; la transición visible la aplica el hilo de UI.
- `ERROR` es terminal: no hay reintento automático silencioso. El host puede
  llamar `open()` otra vez.
- `ENDED` no libera la sesión: el último frame y la duración siguen legibles
  hasta `close()` o `set_source()`.
- `close()` desde cualquier estado destruye la sesión y vuelve a `IDLE`.

### 3.6 Hilos, colas y propiedad

| Recurso | Dueño | Notas |
|---|---|---|
| `wn_media_registry` | `wn_view` | se crea con la vista, se destruye al final |
| mapa nodo→sesión | hilo de UI | sin locks; una entrada por nodo `video` vivo |
| estado de la sesión (fuente de verdad) | hilo de UI | única escritura; el hilo de media solo publica flags atómicos |
| hilo de media (1 por sesión) | runtime | llama **todas** las ops del proveedor, serializadas |
| `video_ready` (ring de 3 frames) | media → UI | SPSC; el UI consume, el media produce |
| `audio_ready` (ring ~200 ms) | media → sink | SPSC; única sincronización permitida en el módulo |
| `events` (ring de 64) | media → UI | MPSC con contadores atómicos |
| textura del frame actual | renderer | el sink la posee; el motor solo guarda el id |

Reglas:

1. El hilo de UI **nunca** espera al proveedor: no llama `probe`, `read`,
   `decode` ni `receive_*`. Sí hace, una vez por nodo y frame, el
   `upload`/`import_native` acotado del frame ya decodificado, porque el
   renderer es single-thread y su device/context no se comparte.
2. Todas las ops de proveedor se llaman desde el hilo de media, una cada vez.
   Un proveedor con hilos internos (Media Foundation) sigue obligado a
   aceptar llamadas solo desde ese hilo.
3. La única sincronización del módulo son los anillos SPSC de audio y vídeo y
   el anillo de eventos. El core y `wn_view_render` siguen sin locks.
4. `close()` señala cancelación (`CANCELLED` en `read`/`decode`), espera al
   hilo de media como máximo 500 ms y, si no termina, lo desasocia y filtra
   (`wn_media_thread_leaks++` en stats) antes que colgar la UI. Un proveedor
   que bloquee más de 100 ms por op incumple el contrato y se marca como
   defectuoso.
5. `release()` de una *lease* puede llegar desde el hilo de render (el sink
   terminó de subir el frame) o desde el hilo de media (frame descartado).
   Debe ser thread-safe.
6. **E4 corre siempre inline**: el bucle de media se ejecuta desde
   `wn_core_media_tick` (llamado por `wn_view_tick`/`wn_view_render`) y el
   upload del frame elegido desde `wn_core_media_present` (llamado por
   `wn_view_render`, antes de construir la display list). No hay hilos,
   anillos SPSC ni locks en el camino de UI. `WN_MEDIA_THREADS=1` y el hilo
   por sesión son E4b; la propiedad y los presupuestos de esta tabla ya son el
   contrato de destino.
7. `serial` es un contador por sesión que sube en `seek()` y `flush()`. El
   runtime lo guarda en su entrada de cola (no en `wn_video_frame`, que es del
   proveedor) y descarta cualquier frame que llegue con un `serial` anterior.

Presupuestos de cola:

| Cola | Capacidad | Política al llenarse |
|---|---|---|
| paquetes | 32 | el hilo de media para de leer (backpressure del demuxer) |
| vídeo decodificado | 3 frames | se descarta el **más viejo** (`dropped++`) |
| audio | 200 ms | el hilo de media espera (nunca se descarta audio ya decodificado) |
| eventos | 64 | se descarta el más viejo y se incrementa `events_dropped` |

### 3.7 Reloj A/V, buffering y frame drop

Hay dos fuentes de reloj y una sola regla de selección:

```c
/* hilo de UI, una vez por frame */
int64_t wn_media_clock_us(const wn_media_session *s, int64_t now_us)
{
    if (s->audio_master && s->audio_clock_ok)
        return s->audio->clock_us(s->audio_ctx);   /* posición audible */
    /* reloj virtual: solo avanza mientras PLAYING */
    return s->anchor_position_us +
           (s->state == WN_MEDIA_STATE_PLAYING
                ? (now_us - s->anchor_wall_us) * s->rate
                : 0);
}
```

- **Maestro = audio** cuando hay stream de audio, no está `muted` y el sink
  está abierto y sin underrun.
- **Maestro = reloj virtual** en el resto de casos (solo vídeo, `muted`,
  dispositivo perdido, `audio_off`).
- El reloj virtual se ancla en `play()` (`anchor_wall_us = now`,
  `anchor_position_us = position`), se congela en `pause()` y se re-ancla en
  `seek()` y al cambiar de maestro.
- Un underrun de audio **no** adelanta el reloj: la sesión pasa a `BUFFERING` y
  el reloj devuelve la última posición audible. Nunca hay salto hacia delante.

Selección de frame de vídeo:

```
objetivo = clock_us + render_lead_us
render_lead_us = clamp(vsync_interval_us, 0, 2 * frame_interval_us)
frame elegido = el de mayor pts_us con pts_us <= objetivo
```

- `vsync_interval_us` lo publica el renderer (16 667 µs si no hay dato): es el
  adelanto necesario para que el frame esté listo en el siguiente present.

- Si `pts_us < clock_us - 2 * frame_interval_us` → **tarde**: se libera sin
  presentar (`dropped++`), no se reordena la cola.
- Si el frame elegido ya está presentado (`same texture_id`) no se re-sube.
- `frame_interval_us` sale del delta de `pts` de los dos últimos frames (sirve
  para VFR); con un solo frame se usa `duration_us`; sin datos, 16 667 µs.
- `muted` y solo-audio no agendan vídeo.

`BUFFERING` se declara cuando `video_ready == 0 && !eof && state == PLAYING`;
se sale con `video_ready >= 2` o `eof`. El último frame presentado se queda en
pantalla: no se pinta negro.

El motor pide frames al host mientras haya alguna sesión en
`PLAYING`/`BUFFERING`/`SEEKING`:

- `wn_view_needs_render()` devuelve 1.
- `wn_view_media_active()` y `wn_view_media_frame_interval_us()` (interno)
  permiten que `wn_app` ajuste `host_ops->set_tick` sin que el host tenga que
  llamar `wn_app_set_tick_ms`. Un host embedded usa su propio loop y llama
  `wn_view_render` a su ritmo; si llega antes de tiempo, el render es barato.

### 3.8 Eventos al host

```c
typedef enum wn_media_event_type {
    WN_MEDIA_EVENT_OPENED = 0,        /* duración, streams, tamaño intrínseco */
    WN_MEDIA_EVENT_FIRST_FRAME = 1,   /* ya se puede dejar de pintar el póster */
    WN_MEDIA_EVENT_STATE = 2,         /* cambió wn_media_state */
    WN_MEDIA_EVENT_PROGRESS = 3,      /* ~4 Hz mientras PLAYING; para el scrubber */
    WN_MEDIA_EVENT_ENDED = 4,
    WN_MEDIA_EVENT_ERROR = 5,
    WN_MEDIA_EVENT_AUDIO_DEVICE_LOST = 6,
    WN_MEDIA_EVENT_AUDIO_DEVICE_RESTORED = 7
} wn_media_event_type;

typedef struct wn_media_event {
    uint32_t struct_size;
    uint32_t type;             /* wn_media_event_type */
    wn_node_id node;           /* nodo <video> que lo origina */
    int32_t  state;            /* wn_media_state tras el evento */
    int32_t  error;            /* wn_media_status; 0 si no aplica */
    int64_t  position_us;
    int64_t  duration_us;
    const char *detail;        /* UTF-8, solo durante el callback */
} wn_media_event;

typedef void (*wn_media_event_cb)(void *userdata, const wn_media_event *event);
```

- Se entregan en el **hilo de UI**, desde `wn_view_render`/`wn_view_tick`
  (nunca desde el hilo de media), en el orden en que se produjeron.
- `detail` es propiedad del motor y solo vive durante el callback; el host que
  lo guarde debe copiarlo.
- No se reutiliza `wn_event`: sus campos ya están sobrecargados tres veces
  (`target_id`, `key`, `x/y/width/height`) y media necesita `pts`, `duration`,
  `state` y `error` sin ambigüedad.
- `PROGRESS` se emite como máximo cada 250 ms; el host puede ignorarlo y leer
  `wn_node_media_get_state` por frame.

### 3.9 ABI pública (v8, aditiva)

En `include/webnative.h`, sin tocar ningún struct existente:

```c
/* Registro de proveedores (por app/vista, no global). */
WN_API wn_status wn_app_register_media_provider(wn_app *app,
        const wn_media_provider_ops *ops, void *context, int32_t priority);
WN_API wn_status wn_app_load_media_provider(wn_app *app, const char *dll_path);
WN_API wn_status wn_view_register_media_provider(wn_view *view,
        const wn_media_provider_ops *ops, void *context, int32_t priority);
WN_API wn_status wn_view_load_media_provider(wn_view *view, const char *dll_path);

/* Eventos de media (hilo de UI). */
WN_API wn_status wn_app_on_media_event(wn_app *app, wn_media_event_cb cb,
                                       void *userdata);
WN_API wn_status wn_view_on_media_event(wn_view *view, wn_media_event_cb cb,
                                        void *userdata);

/* Control por nodo. `node` debe ser kind==video; si no, WN_ERR_UNSUPPORTED. */
WN_API wn_status wn_node_media_open(wn_app *app, wn_node_id node);
WN_API wn_status wn_node_media_play(wn_app *app, wn_node_id node);
WN_API wn_status wn_node_media_pause(wn_app *app, wn_node_id node);
WN_API wn_status wn_node_media_seek(wn_app *app, wn_node_id node,
                                    int64_t position_us);
WN_API wn_status wn_node_media_set_volume(wn_app *app, wn_node_id node,
                                          float gain);      /* 0..1 */
WN_API wn_status wn_node_media_set_muted(wn_app *app, wn_node_id node, int muted);
WN_API wn_status wn_node_media_set_loop(wn_app *app, wn_node_id node, int loop);
WN_API wn_status wn_node_media_set_source(wn_app *app, wn_node_id node,
                                          const char *path_utf8);
WN_API wn_status wn_node_media_select_subtitle(wn_app *app, wn_node_id node,
                                               int32_t stream_index); /* -1 off */
WN_API wn_status wn_node_media_set_subtitle_delay(wn_app *app, wn_node_id node,
                                                  int32_t delay_ms);

/* Lecturas. Devuelven un struct copiado; sin buffers del motor. */
typedef struct wn_media_state_info {
    uint32_t struct_size;
    int32_t  state;          /* wn_media_state */
    int32_t  error;          /* wn_media_status */
    int64_t  position_us;
    int64_t  duration_us;
    uint32_t stream_flags;   /* WN_MEDIA_INFO_* del archivo abierto */
    float    volume;
    int32_t  muted, loop;
} wn_media_state_info;

WN_API wn_status wn_node_media_get_state(wn_app *app, wn_node_id node,
                                         wn_media_state_info *out);
```

Semántica:

- `open` es **asíncrono**: devuelve `WN_OK` si la petición se aceptó y el
  resultado llega como `OPENED` o `ERROR`. En E4 el bucle es inline, así que la
  transición ocurre dentro de la misma llamada, pero el retorno sigue siendo
  "petición aceptada": un `media_null` devuelve `WN_OK` y encola `ERROR`. Un
  nodo sin `src` es la excepción explícita: devuelve `WN_ERR_STATE` y encola el
  `ERROR` tipado.
- `open` fuerza la apertura aunque `preload = none`; es el "cargar ahora" del
  host.
- `play` sobre `IDLE` abre primero (equivale a `open` + `play`).
- `seek` está permitido en cualquier estado salvo `ERROR`; en `IDLE` solo fija
  la posición de arranque.
- `set_source` destruye la sesión, vuelve a `IDLE`, sustituye la ruta y
  re-aplica `preload`. Marca layout dirty (el tamaño intrínseco cambia).
- `set_volume`/`set_muted`/`set_loop` son estado de runtime: **no** modifican
  el blob.
- `get_state` nunca bloquea y es válido en cualquier estado.
- No hay `wn_node_media_close` en la ABI v8: el cierre interno ocurre al
  destruir el nodo, reemplazar el documento o `set_source`. Un host que quiera
  "stop" hace `pause` + `seek(0)`.
- `wn_node_set_image` sobre un nodo `video` devuelve `WN_ERR_UNSUPPORTED`;
  `wn_node_media_*` sobre un nodo que no es `video`, ídem.
- `wn_node_clone` copia la sección `media` (intención), **no** la sesión: el
  clon arranca en `IDLE` y no reproduce audio hasta que el host lo pida.

### 3.10 Errores y fallback

| `wn_media_status` | `wn_status` expuesto | UX |
|---|---|---|
| `WN_MEDIA_OK` | `WN_OK` | — |
| `WN_MEDIA_AGAIN` | `WN_OK` | se reintenta; invisible |
| `WN_MEDIA_EOF` | `WN_OK` | estado `ENDED` |
| `WN_MEDIA_UNSUPPORTED_CONTAINER` | `WN_ERR_UNSUPPORTED` | póster + evento con `detail` |
| `WN_MEDIA_UNSUPPORTED_CODEC` | `WN_ERR_UNSUPPORTED` | póster + evento |
| `WN_MEDIA_IO` | `WN_ERR_IO` | póster + evento |
| `WN_MEDIA_CORRUPT` | `WN_ERR_PARSE` | póster + evento; se conserva lo decodificado |
| `WN_MEDIA_OOM` | `WN_ERR_OUT_OF_MEMORY` | póster + evento |
| `WN_MEDIA_CANCELLED` | — | sin evento (lo pidió el motor) |
| `WN_MEDIA_GPU` | `WN_ERR_GPU` | un reintento en software; si falla, evento |
| `WN_MEDIA_INTERNAL` | `WN_ERR_INTERNAL` | póster + evento |

Fallback:

1. Si la ruta hardware falla (`GPU` o `import_native` distinto de `OK`), la
   sesión se reabre **una vez** con `native_kind = 0` y `allowed_formats` CPU.
2. Si el proveedor elegido falla en `open`, se prueba el siguiente de la
   cadena **solo** para `UNSUPPORTED_*`; para `IO`/`CORRUPT` no se insiste
   (mismo archivo, mismo fallo).
3. `media_null` produce `UNSUPPORTED_CONTAINER` con
   `detail = "no media provider claims <path>"`.
4. La UI de error **no** la inventa el core: pinta póster si existe, si no el
   fondo del nodo, y emite el evento. El host decide el texto/icono. Esto evita
   que el motor tenga copy ni i18n.

---

## 4. Renderizado

### 4.1 Layout y `object-fit` en el core

`<video>` se layouta como un elemento reemplazado. La caja y el
`object-fit` se resuelven en `core/paint.c` con la misma matemática que
`push_image_quad` (rect + `padding`, UVs de `cover`/`contain`), generalizada a
un `push_replaced_quad(kind, resource, ...)`. El renderer **no** decide layout,
recorte ni `object-fit`: recibe un quad con UVs ya resueltos.

Tamaño intrínseco: el core mantiene un par por nodo
(`intrinsic_w`, `intrinsic_h`) con esta prioridad:

1. `display_width/display_height` del frame decodificado (rotación y PAR ya
   aplicados);
2. tamaño del póster decodificado (WIC, misma caché que `img`);
3. `300 × 150`.

Cuando llega `OPENED` o el primer frame con un tamaño distinto, el core marca
layout dirty (una sola vez por valor). El ABI existente
`wn_node_set_image_size` sigue siendo solo para `img`; el vídeo no lo necesita
porque el dato viene del proveedor.

### 4.2 Display list: `wn_video_batch`

Se añade un array propio, sin reinterpretar `wn_image_batch` (que sigue keyed
por ruta):

```c
/* wn_core.h */
typedef struct {
    int first, count;                 /* rango en image_vertices */
    float clip_x, clip_y, clip_w, clip_h;
    uint64_t texture;                 /* id del sink; 0 = nada que pintar */
    uint32_t format;                  /* wn_pixel_format */
    uint32_t color;                   /* WN_MEDIA_COLOR(...) */
    uint32_t flags;                   /* WN_VIDEO_BATCH_* */
} wn_video_batch;

/* La rotación ya está resuelta en los UV del core; el flag OVERLAY solo marca
 * que el batch se compone encima del frame de vídeo (subtítulo bitmap). */
#define WN_VIDEO_BATCH_OVERLAY 0x01u

typedef struct {
    /* ... campos actuales ... */
    const wn_video_batch *video_batches;
    int video_batch_count;
} wn_display_list;
```

- La geometría reutiliza `wn_image_vertex` (pos + uv + alpha): es el mismo
  quad texturizado y `opacity < 1` sigue funcionando por vértice.
- El batch lleva `format` y `color` porque el shader debe convertir YUV→RGB in
  situ; el renderer no adivina el espacio de color.
- `texture == 0` (sin frame todavía) emite **cero** batches: se pinta solo el
  fondo/póster. Nunca se dibuja una textura sin inicializar.
- Los batches de vídeo entran en el replay por item, con el mismo clip y orden
  de stacking que el resto; un `<video>` dentro de un grupo `opacity < 1` se
  compone como cualquier hoja.

### 4.3 Ruta software (fase B, la primera)

1. El proveedor decodifica a `NV12` (o `P010`/`RGBA8`) en memoria del sistema.
2. El hilo de UI elige el frame y llama `sink->upload(frame, &texture)`.
3. El renderer D3D11 mantiene por sesión un ring de 3 texturas
   `DXGI_FORMAT_NV12` (o `R8G8B8A8_UNORM` para RGBA) y sube con
   `UpdateSubresource`/staging map. El coste esperado de un 1080p NV12 es
   ~1.5 ms en un iGPU moderno; se mide y se publica en `upload_stats`.
4. Un par de shaders nuevos:
   - `video_nv12_ps`: muestrea Y y UV, aplica matriz BT.601/709/2020 y rango
     limitado/completo desde `color`, escribe RGB premultiplicado.
   - `video_rgba_ps`: muestreo directo (póster, subtítulo bitmap, frames RGBA).
5. UVs con rotación: la rotación se aplica al quad/UV, no al contenido, así que
   un frame rotado 90° no necesita copia ni shader distinto.

### 4.4 Ruta hardware (fase C: D3D11VA)

```
provider ── device() ──► decoder D3D11VA (BIND_DECODER | SHADER_RESOURCE)
        └── frame.native_texture = ID3D11Texture2D* (array), native_slice
                │
sink import_native ──► CreateShaderResourceView(Texture2DArray, slice)
                │
                └── texture_id ──► wn_video_batch.texture
```

- El proveedor recibe `native_device` y `native_kind` en `open`; crea su
  decoder con su propio `ID3D11DeviceContext` y devuelve texturas **sin
  mapear**. El motor nunca abre, copia ni lee la textura.
- El sink importa y devuelve un id; `release_texture` se llama cuando el frame
  sale de la cola o el nodo se destruye.
- El pool de superficies de decode (DPB) es del proveedor. Si se agota, el
  proveedor devuelve `AGAIN`; el runtime no inventa superficies.
- Fallo de import o de creación del decoder → `WN_MEDIA_GPU` → reintento
  software (§3.10).
- **wgpu queda diferido a la fase D**: importar una textura D3D11 en wgpu
  exige `wgpu_texture_from_d3d11` (o memoria externa Vulkan) y una ruta de
  sincronización adicional. En fase C, si el renderer activo es wgpu, el sink
  declara `native_kind = 0` y se usa software; no se finge zero-copy.

### 4.5 YUV, HDR/10-bit y rotación

- Matriz y rango salen del contenedor cuando existen; si no, BT.709 limited
  para HD y BT.601 limited para SD. Se puede forzar por variable de entorno
  solo para diagnóstico (`WN_MEDIA_FORCE_MATRIX`), nunca por documento.
- `P010` se decodifica a 10 bits y se convierte en el shader a 8 bits para la
  swapchain actual. **HDR real no entra en v1**: no hay tone mapping, no hay
  salida PQ, no hay swapchain `R10G10B10A2`. Los archivos HDR se ven planos y
  apagados; se documenta y se marca `WN_MEDIA_INFO_HDR`.
- Plan HDR (fase D): swapchain FP16/`R10G10B10A2`, shader que aplica
  `PQ→linear→tonemap` o `scRGB` según `transfer`, y `WN_MEDIA_CAP_HDR` en el
  sink. Gate: barras de color HDR contra una referencia.
- Rotación: `rotation_deg` viene del contenedor (matrix de MP4, projection de
  MKV). Se aplica al quad; `display_width/height` ya viene intercambiado para
  90/270, así que layout y `object-fit` son correctos desde el primer frame.

### 4.6 Reglas de hilo del renderizado

1. `wn_view_render` no decodifica, no espera al proveedor y no llama más de un
   `upload`/`import_native` por nodo y frame.
2. `upload` puede tardar (copia de CPU a GPU) pero está acotado por resolución;
   se mide y se expone en `upload_stats.last_upload_ms`. Si supera 4 ms de
   forma sostenida, se baja la resolución de decode (política del proveedor),
   no se bloquea la UI.
3. Ningún puntero de backend cruza `include/webnative.h`. La única superficie
   con punteros nativos es `webnative_media.h` (`native_texture`,
   `native_device`), y son opacos por contrato: el motor no los desreferencia.
4. El renderer no guarda `wn_media_session *`; solo ids de textura. Cuando la
   sesión muere, el `wn_media_host` llama `release_texture` de todos sus ids.

---

## 5. Audio

### 5.1 Cadena

```
audio packet → decode (proveedor) → resample/convert (runtime)
             → ring SPSC (200 ms) → sink → WASAPI shared / miniaudio
```

- Formato de dispositivo preferido: 48 000 Hz, `F32`, máscara de canal del
  sink (estéreo por defecto). El runtime convierte desde el formato del
  decodificador (S16/F32, cualquier rate, 1..8 canales).
- Resampler: lineal en fase B; sinc (polyphase corto) en fase C si la deriva
  A/V lo exige. Se documenta la calidad y el coste.
- WASAPI en modo **shared**, event-driven (`IAudioClient3`,
  `SetDefaultPeriodInSharedMode`), buffer ~10 ms. Backend por defecto en
  Windows (`src/runtime/media/audio_wasapi.c`).
- `miniaudio` es una alternativa opcional (`WN_MEDIA_AUDIO=miniaudio`) para
  hosts que no quieren COM o para portar a macOS/Linux; misma vtable.
- Un archivo solo-audio usa el mismo nodo `video` con la caja/póster y el
  sink de audio como reloj. No se pinta nada por frame.

### 5.2 El sink de audio es el reloj maestro

`clock_us()` devuelve la **posición audible**, no la escrita:

```
position_us = anchor_pts_us + played_frames / sample_rate * 1e6
played_frames = IAudioClock::GetPosition()      (WASAPI)
```

- `anchor_pts_us` es el `pts` del primer frame escrito tras `open`/`seek`/
  re-ancla.
- La latencia del dispositivo (`latency_us()`) **no** se suma al reloj: ya está
  dentro de `GetPosition`. Se usa solo para decidir el `render_lead_us` del
  vídeo y para el scrubber.
- En `pause`, el sink se pausa y el reloj se congela.
- En `seek`, se hace `flush`, se re-ancla con el `pts` del primer frame nuevo y
  se descarta todo lo anterior.
- Deriva: si `|video_pts - audio_clock| > 40 ms` de forma sostenida durante
  500 ms, el scheduler descarta o repite un frame de vídeo (nunca toca el
  audio). Se cuenta en `av_resyncs`.

### 5.3 Cambio de dispositivo

- Un `IMMNotificationClient` en el sink detecta
  `AUDCLNT_E_DEVICE_INVALIDATED`/`OnDefaultDeviceChanged`.
- Secuencia: `state = BUFFERING`, `AUDIO_DEVICE_LOST`, `close` del sink,
  reapertura del dispositivo por defecto, `flush` del ring, re-ancla en la
  posición actual, `AUDIO_DEVICE_RESTORED`.
- Mientras no hay audio, el maestro pasa a reloj virtual (§3.7) sin salto: la
  posición se conserva.
- Si la reapertura falla 3 veces, el vídeo sigue con reloj virtual y el audio
  queda deshabilitado para esa sesión (`detail` en el evento). No se
  destruye la sesión.

### 5.4 `muted`, solo vídeo y solo audio

| Caso | Decode de audio | Sink | Reloj |
|---|---|---|---|
| audio normal | sí | abierto, ganancia 1 | audio |
| `muted` desde el inicio | **no** | no se abre | virtual |
| `muted` tras `play` | no | se cierra tras `flush` | virtual, re-ancla |
| `unmute` | sí | se abre | audio, re-ancla |
| sin stream de audio | no | no | virtual |
| solo audio | sí | abierto | audio |

`muted` implica no decodificar audio: es la diferencia con "volumen 0", que
mantiene el dispositivo y el reloj. Se documenta porque cambia el consumo de
CPU y el comportamiento en `unmute` (una re-ancla, sin salto audible).

---

## 6. Subtítulos

Los subtítulos **no** son parte del decodificador de vídeo ni del core de
pintado: son una capa de presentación con su propio timing.

| Formato | Ruta | Módulo |
|---|---|---|
| SRT / WebVTT simple | parser a cues en el runtime; se pintan como **texto del motor** (misma vtable `wn_text_ops`, atlas y `line-height`) | `media_subtitles.c` |
| ASS/SSA | render con **libass** a un overlay RGBA | `media_subass.c`, `WN_MEDIA_LIBASS=ON` |
| PGS / VobSub | el proveedor decodifica el bitmap y entrega un `wn_video_frame` con `flags=WN_FRAME_OVERLAY` | proveedor + sink |
| Texto en MKV (SRT embebido) | mismo parser que SRT, cues desde el stream `TEXT` | `media_subtitles.c` |
| Imagen en MKV (PGS) | mismo camino que PGS | proveedor |

- Selección: `wn_node_media_select_subtitle(node, index)` con el índice de
  `wn_media_stream_info.index`; `-1` apaga. Default: la primera pista de
  subtítulos marcada como default por el contenedor, o ninguna.
- Timing: los `pts` de los cues están en el timebase del stream; el runtime
  los convierte a µs y los compara contra el **mismo reloj maestro** que el
  vídeo. `subtitle_delay_ms` es una preferencia de sesión (ABI), no del
  documento.
- En `seek`, el índice de cues se re-posiciona por búsqueda binaria; no se
  recorre desde el principio.
- SRT en el motor de texto: una línea centrada en la parte baja de la caja del
  vídeo, con un fondo semitransparente opcional (`-1` = sin fondo). Sin estilos
  por cue, sin posiciones absolutas, sin fuentes embebidas.
- ASS vía libass: el overlay se sube como textura RGBA y se pinta con
  `WN_VIDEO_BATCH_OVERLAY` encima del frame. libass necesita
  HarfBuzz/FreeType, ya perennes.
- PGS/VobSub: bitmap ya posicionado por el contenedor; se compone tal cual
  (sin OCR, sin escalado).
- Fuera de v1: estilos completos, karaoke, `\pos` arbitrario, RTL/bidi,
  subtítulos 3D.

---

## 7. Proveedores y licencias

### 7.1 `media_null` (siempre)

- `name = "null"`, `caps` vacías, `probe` y `open` devuelven
  `WN_MEDIA_UNSUPPORTED_CONTAINER`.
- Se registra en toda build, incluida la de release más mínima. No se puede
  quitar ni sustituir.
- Existe para que la cadena nunca esté vacía, para que "no hay códec" sea un
  camino normal con error tipado y para que los tests de contrato no dependan
  de un build con códec.

### 7.2 Baseline libre (opcional, `WN_MEDIA_BASELINE=ON`)

| Componente | Licencia | Uso |
|---|---|---|
| libvpx (VP8/VP9) | BSD-3 | decode |
| dav1d (AV1) | BSD-2 | decode |
| libopus | BSD-3 | decode de audio |
| demuxer Matroska/WebM/OGG | en el repo (`demux_mkv.c`) | EBML mínimo, sin dependencias |

- Es una build **sin patentes de pool** para el caso VP8/VP9/AV1 + Opus, y
  cubre el 90 % de un catálogo "descargado de la web" moderno.
- No incluye H.264/H.265/AAC: eso es FFmpeg LGPL o Media Foundation.
- El demuxer in-tree es la única pieza con mantenimiento propio; se limita a
  WebM/Matroska y OGG, que es lo que el baseline necesita.

### 7.3 FFmpeg LGPL como plugin opcional

- Se construye como **DLL separada** `wn_media_ffmpeg.dll`, enlazada
  dinámicamente a un FFmpeg compartido configurado
  `--disable-gpl --disable-nonfree --enable-shared`.
- **Nunca** se enlaza estáticamente dentro de `webnative.dll`. El core no
  incluye cabeceras de FFmpeg.
- Al cargar, el plugin verifica `avutil_configuration()`: si contiene
  `--enable-gpl` o `--enable-nonfree`, se niega a registrarse y escribe el
  motivo en `wn_last_error()`.
- H.264/H.265/AAC/MP4 entran por decodificadores nativos de FFmpeg (LGPL).
  Nada de `libx264`/`libx265` (GPL).
- Nota de patentes: las licencias de H.264/H.265/AAC (pools MPEG-LA/Via LA/
  Access Advance) son responsabilidad de quien distribuye el binario y el
  contenido. El motor no empaqueta códecs ni sublicencia nada. AV1/VP9
  declaran royalty-free (AOMedia/Google), pero esto no es asesoría legal.
- Distribución: el instalador declara el componente, su versión y su licencia
  en `THIRD_PARTY_NOTICES.md`; el DLL vive fuera de `webnative.dll`.

### 7.4 Media Foundation (opcional, Windows)

- Proveedor `media_mf` que usa el decodificador del sistema: H.264/HEVC por
  hardware sin DLL extra, y AAC.
- **Por qué no es el único**: depende de la edición de Windows (N sin Media
  Feature Pack), de la extensión HEVC (de pago), de la versión (VP9/AV1
  irregulares), no cubre MKV/WebM con fiabilidad y es COM/async. Como
  contrato base sería frágil y no portable.
- Se ofrece como conveniencia en Windows, con prioridad por debajo de un
  proveedor explícito del host.

### 7.5 Orden de la cadena recomendado

```
registro explícito del host (1000)
  → plugin cargado por allowlist (500)
    → FFmpeg LGPL compilado/plugin (100..400)
      → Media Foundation (50, Windows)
        → baseline libre (25)
          → media_null (0, nunca acierta)
```

El orden es configurable, pero `media_null` siempre es el último y no se puede
mover.

### 7.6 Reglas de licencia del repo

1. Nada GPL en el core ni en la DLL principal: ni `--enable-gpl`, ni
   `libx264`/`libx265`, ni `libmpv`/mpv (GPL por defecto).
2. Toda dependencia nueva entra en `third_party/` o como plugin separado, con
   su licencia en `THIRD_PARTY_NOTICES.md`.
3. Los plugins se cargan en runtime; el core compila sin ellos
   (`WN_MEDIA_BASELINE=OFF`, `WN_MEDIA_FFMPEG=OFF`) y `media_null` sigue
   presente. El elemento `video`, el registro y `media_null` **no** son
   opcionales: son parte del contrato del documento.
4. Un build "free baseline" debe poder distribuirse sin abogados: VP8/VP9/AV1
   + Opus + WebM/OGG, todo BSD.

---

## 8. Testing

### 8.1 Tests de contrato (sin códec)

Por la regla 7 de `docs/conventions.md`, los contratos se prueban contra la
vtable o un backend falso, y el backend real queda como smoke.

| Test | Qué fija |
|---|---|
| `tests/media/null_test.c` (cubierto por `media_abi_test.c`) | `media_null` siempre presente; `open` devuelve `UNSUPPORTED_CONTAINER`; el nodo pinta póster/fondo; no hay fugas ni sesiones huérfanas |
| `tests/media/media_test_provider_test.c` | proveedor `media_test`: PTS monótono, duración, determinismo pixel a pixel, seek, release/lease |
| `tests/media/media_session_test.c` | máquina de estados completa, preload/autoplay, idempotencia, seek/clamp, loop, ENDED→restart, nodo sin fuente, evento ERROR, sin fugas de buffer |
| `tests/media/media_frame_test.c` + `verify_media_frames.py` | póster→primer frame→seek→pausa en PNG offscreen; batches de vídeo con textura real; `object-fit` fill/contain/cover |
| `tests/media/media_abi_test.c` | handshake, orden de la cadena, eventos y símbolos públicos (v8) |
| `tests/media/media_blob_test.c` | blob v7: `src = 0` aceptado; src vacío/con esquema, flags, reserved, preload y asociación rechazados |
| `tests/media/media_layout_test.c` | layout `object-fit` + `300×150` default + póster; ningún batch con `texture == 0` |
| `tests/media/clock_test.c` | **pendiente (E5)**: sink de audio falso y reloj de audio |
| `tests/media/drop_test.c` | **pendiente (E5)**: 23.976/24/25/29.97/60 con reloj falso |
| `tests/media/upload_test.c` | **pendiente (E5)**: NV12/P010/RGBA con readback |

### 8.2 Matriz de fixtures (smoke con códecs reales)

Suite etiquetada `media-smoke`: se salta si el proveedor no está instalado, y
nunca es gate del core.

| Eje | Valores |
|---|---|
| profundidad / color | SDR 8-bit 4:2:0, SDR 10-bit P010, HDR10 (PQ/BT.2020) |
| fps | 23.976, 24, 25, 29.97, 30, 50, 60 |
| variabilidad | CFR, VFR (mezcla 24/30), all-keyframe, long-GOP |
| rotación | 0, 90, 180, 270 (matrix de contenedor) |
| audio | sin audio, mono, estéreo, 5.1, Opus, AAC, 44.1/48 kHz |
| daño | truncado a mitad de GOP, índice corrupto, `moov` al final, 0 frames |
| duración | 1 frame, 10 s, 2 h (solo probe), 0 bytes |
| subtítulos | SRT embebido, ASS, PGS, ninguno |

Cada celda registra: ¿abre?, `pts` del primer frame, `duration_us`, frames
presentados/descartados, error de barras de color (ΔE o MAE), precisión de
seek (target vs `pts` real), deriva A/V a 60 s.

### 8.3 Determinismo y headless

- **Sin reloj de pared en tests**: el runtime toma el tiempo de un
  `wn_media_clock_ops { int64_t (*now_us)(void*); }` inyectable. El test
  avanza el reloj a mano.
- **`WN_MEDIA_THREADS=0`** ejecuta el bucle de media inline en `tick`: los
  tests son monohilo y byte-reproducibles. El modo con hilos se cubre con un
  único stress (`tests/media/thread_stress.c`) fuera del gate.
- **Headless**: `wn_view_create_offscreen` + `wn_view_readback` ya existen; el
  `media_test` provider es determinista, así que un PNG de vídeo puede ser
  golden. No se usan archivos de códec reales en CI.
- **Sin dependencia de códec en el core**: `ctest` compila y pasa sin ningún
  proveedor real (`WN_MEDIA_BASELINE=OFF`, `WN_MEDIA_FFMPEG=OFF`) sobre
  `media_null`.

---

## 9. Fases de implementación

### Fase A — contrato + `media_null` + nodo/blob v7 + póster

Entregables:

1. Este documento y las actualizaciones de `docs/wnui-format.md`,
   `docs/abi.md`, `docs/subset-v1.md` y `docs/support.md` (en el mismo PR).
2. `include/webnative_media.h` con la ABI de proveedor.
3. `WN_BLOB_VERSION 7`, sección 7, `WnBlobMediaRecord`, validación del loader,
   `kind=6` en el runtime y en el frontend Rust, `wnui-dump` (`kind=video` y
   línea `media`).
4. `media_null` + registro + cadena + `wn_last_error()` legible.
5. Nodo `video` en layout/paint: caja, `object-fit`, fondo, póster, `opacity`,
   `border-radius`, clip.
6. ABI v8: `wn_node_media_open/play/pause/seek/set_source/get_state`,
   `wn_app_on_media_event`; `set_source` y error tipado.
7. `preload` = `none`/`metadata` (+ `auto` aceptado como `metadata`).

Gate A: el documento canónico no cambia salvo la versión; un `<video>` con
póster se pinta idéntico en D3D11 y wgpu; `open` sin proveedor real produce
`WN_MEDIA_EVENT_ERROR` sin crash; todos los blobs re-pinneados; dump golden
actualizado.

### Fase B — proveedor software + sinks + sincronía básica

1. **[E4] `media_test`** (sintético, solo tests). `demux_mkv` + decoders del
   baseline libre quedan **pendientes (E5)**.
2. **[E4] `video_sink` software**: upload **RGBA8** a textura D3D11, un id
   opaco por frame, `object-fit` y cuadro de vídeo en la display list. NV12/P010,
   shaders YUV, rotación de contenido e import nativo quedan pendientes.
3. `audio_sink` WASAPI shared + resampler lineal: **pendiente (E5)**.
4. **[E4] `media_session`**: máquina de estados, colas de 3 frames, reloj
   virtual inyectable, buffering, frame drop, `seek`, `loop`, eventos y
   auto-open por `preload`/`autoplay`. **Inline** y sin locks; el hilo por
   sesión es E4b.
5. Overlay de controles del host en el demo (`examples/media.c`), no en el
   core: pendiente.

Gate B: los tests de §8.1 en verde en D3D11 (el sink wgpu es E4b); `ctest`
pasa sin proveedor real; el frame sintético se ve en un PNG offscreen y no hay
fuga de `wn_frame_lease`. La deriva A/V de 60 s es E5 (no hay audio todavía).

### Fase C — D3D11VA zero-copy

1. `device()` + `import_native` en el sink D3D11.
2. `media_ffmpeg` con `AV_PIX_FMT_D3D11` + `AVD3D11VADeviceContext`.
3. Fallback software automático y contador `imports`.
4. `media_mf` opcional (misma ruta de import).

Gate C: 4K H.264 reproduce con < 5 % de CPU de decode en copia cero; import
fallido cae a software en un frame; wgpu sigue en software sin fingir HW.

### Fase D — subtítulos, HDR y wgpu

1. `media_subtitles.c` (SRT/VTT), `media_subass.c` (libass), overlay PGS.
2. HDR: swapchain 10-bit, tonemap, `WN_MEDIA_CAP_HDR`.
3. Import nativo en wgpu (D3D12/Vulkan external memory) o decisión explícita
   de no hacerlo.

Gate D: subtítulos sincronizados tras seek (error < 1 frame); HDR contra
referencia; paridad D3D11/wgpu en el fixture de color.

### 9.5 Layout de archivos

```
include/
  webnative.h                  ABI v8 (funciones de media, aditivas)
  webnative_media.h            ABI pública de proveedor (WN_MEDIA_ABI_VERSION 1)
src/runtime/
  wn_media.h                   interno: registry, session driver, sinks
  media/
    media_registry.c           cadena, prioridades, allowlist, carga de plugin
    media_core.c               control público + driver de sesión inline (E4)
    media_session.c            (E4b) hilo por sesión, colas, scheduling
    media_clock.c              (E4b) reloj virtual inyectable ya vive en media_core
    media_null.c               proveedor nulo (siempre)
    media_test.c               proveedor sintético (solo tests, nunca en CMake)
    video_sink.c               (E5) despacho al sink, ids de textura, ring
    audio_sink.c               (E5) despacho al sink, selección de backend
    audio_wasapi.c             (E5) WASAPI shared, IMMNotificationClient
    audio_miniaudio.c          opcional
    media_subtitles.c          cues, SRT/VTT, seek/sync
    media_subass.c             libass (opcional)
    demux_mkv.c                EBML Matroska/WebM (baseline)
    dec_vpx.c, dec_dav1d.c     baseline libre (opcional)
  renderer_d3d11.c             E4: sink RGBA + upload (en el mismo TU)
  renderer_d3d11_video.c       (E5) NV12/P010, shaders YUV, import D3D11VA
  renderer_wgpu_video.c        fase D
  plugin/
    ffmpeg/wn_media_ffmpeg.c   objetivo DLL separado (LGPL)
    mf/wn_media_mf.c           objetivo DLL/estático separado (Windows)
src/runtime/tests/media/       §8.1
```

CMake:

```
# El elemento, el registro y media_null no se apagan: son contrato.
option(WN_MEDIA_BASELINE "VP8/VP9/AV1/Opus + MKV" OFF)
option(WN_MEDIA_THREADS "media thread per session" OFF) # E4: inline only
option(WN_MEDIA_LIBASS "ASS/SSA subtitles" OFF)
option(WN_MEDIA_FFMPEG "build the LGPL plugin" OFF)
target wn_media_ffmpeg SHARED ...   # nunca enlazado a webnative
```

### 9.6 Qué se serializa con otros trabajos de blob

El bump a v7 toca un contrato compartido. Debe ir en **un solo PR** y no
solaparse con otra lane que toque el blob:

- `src/runtime/document/wn_blob.h` (versión, sección, registro)
- `src/runtime/document/wn_blob.c` (validación)
- `src/frontend/src/emit/blob.rs` y `emit/ui_tree.rs` (`KIND_VIDEO`,
  `BLOB_NODE` sin cambios de tamaño, escritura de la sección 7)
- `src/frontend/src/cli/subset.rs` (`<video>` aceptado con diagnóstico de
  atributos no soportados)
- `tools/wn_cli/suites.py` (`PINNED_BLOBS`, 18 hashes)
- `src/runtime/tools/wn_dump.c` + `tests/dump/canonical.txt`
- `generated/builtin_blob.h` (regenerado)
- `docs/wnui-format.md`, `docs/architecture.md`, `docs/abi.md`,
  `docs/subset-v1.md`, `docs/support.md`

Regla operativa: mientras esa lane esté abierta, **nadie más** cambia
`WN_BLOB_VERSION`, el registro de nodo ni las secciones. Un conflicto aquí se
resuelve regenerando, nunca a mano.

---

### 9.7 Estado de implementación (E3a)

**Implementado**

- `WN_BLOB_VERSION 7`, `kind = 6` (`video`) y la sección opcional 7
  (`WnBlobMediaRecord`, 16 bytes) con validación completa del loader. Un solo
  lector; v6 se rechaza con "bad magic or version".
- Frontend: `<video>` con `src` (opcional desde E4: sin él emite `src = 0`),
  `poster`, `width`, `height`, `controls`, `autoplay`, `muted`, `loop` y
  `preload`; `src`/`poster` se resuelven como los de `img`; el nodo nunca
  hornea `image_src`/`image_width`/`image_height`.
- Layout/paint: caja reemplazada, `object-fit`, tamaño por defecto **300 × 150**,
  póster por la ruta de imagen existente y placeholder neutro
  (`#232A36`) cuando no hay póster. Hit-test y accesibilidad se comportan como
  cualquier hoja reemplazada.
- ABI v8: `include/webnative_media.h` (`WN_MEDIA_ABI_VERSION 1`), registro de
  proveedores por app/vista con orden determinista, `media_null` siempre
  presente y nunca acierta, `wn_app/wn_view_register_media_provider`,
  unregister/load, la familia `wn_media_event` y las funciones
  `wn_node_media_*` con sus lecturas y errores tipados.
- Tests: `src/runtime/tests/media/` (blob, layout/paint, media_null/ABI y
  píxeles) y el suite `media` de `wn-cli`.

**Diferido en E3a y cerrado por E4**

- `open` ya crea sesión, llama `probe`/`open` reales y recorre la cadena
  determinista; con solo `media_null` termina en
  `WN_MEDIA_EVENT_ERROR`/`WN_MEDIA_UNSUPPORTED_CONTAINER`, nunca en
  `WN_MEDIA_INTERNAL`.
- Hay `wn_video_frame`, textura de vídeo, estado `OPENING…ENDED/ERROR`, seek,
  `loop`, `autoplay`, `preload`, `buffering` y `frame drop`.
- Sigue sin decodificador real, sink de audio, reloj A/V con audio,
  subtítulos, `media_ffmpeg`, `media_mf`, `demux_mkv` ni plugins. El
  tamaño intrínseco del **póster** sigue siendo 300 × 150; el del frame
  decodificado sí se publica y relayouta.

### 9.8 Estado de implementación (E4, primera mitad de la Fase B)

**Implementado**

- **Driver de sesión** en `src/runtime/media/media_core.c`: una sesión por nodo
  `video` vivo, máquina de estados completa, reloj virtual anclado,
  PROGRESS a 4 Hz, eventos `OPENED/FIRST_FRAME/STATE/PROGRESS/ENDED/ERROR`,
  `loop` en EOF, `play()` desde ENDED a 0, seek con flush+serial+filtro de
  frames viejos y `close` a IDLE. Todo **inline** desde `tick` con reloj
  inyectable (`wn_core_set_media_clock` / `wn_view_set_media_clock`).
- **Ciclo de vida**: `wn_view_render` bombea tick+present antes de construir la
  display list; `wn_view_tick` bombea el estado. Documento reemplazado y core
  destruido cierran cada sesión y liberan texturas antes de destruir el
  renderer.
- **`media_test`**: 10 s, 30 fps, 320 × 180 RGBA8, barras + índice de frame en
  bits + marcador móvil, PTS monótono, seek al frame ≤ target y *lease* por
  `malloc`/`free`. Registrado **solo** por los tests; el CMake de producto no
  lo compila.
- **Sink RGBA D3D11** (E4): `wn_video_sink_ops` en `wn_renderer.h`,
  implementación en `renderer_d3d11.c`, id opaco por frame,
  `release_texture`, `wn_video_batch` en la display list y dibujo del cuadro
  con `object-fit` `fill/cover/contain`. wgpu devuelve `video_sink = NULL`:
  el nodo sigue pintando póster/placeholder (declarado E4b, sin fingir).
- **Nodo sin fuente**: `src = 0` aceptado en el blob v7, con el registro
  `media` obligatorio; `open` devuelve `WN_ERR_STATE` y emite ERROR tipado;
  `set_source` recupera y habilita `open`.
- Póster → primer frame → seek → pausa verificados en PNGs offscreen
  (`frame_poster/first/seek/paused.png`).

**Pendiente para E5**

- Demuxer/decoders reales (`demux_mkv`, libvpx/dav1d, FFmpeg LGPL, MF).
- NV12/P010 y shaders YUV; rotación de contenido; superficie HW.
- Sink de audio (WASAPI), resampler, reloj maestro de audio, underrun y
  deriva A/V.
- Subtítulos (SRT/VTT, libass, PGS) y UI de reproducción del host.
- wgpu: upload RGBA (E4b) o import nativo (fase D).
- Hilo de media por sesión (`WN_MEDIA_THREADS`), ring SPSC real y
  `thread_stress`.

## 10. Preguntas abiertas (decisión del owner)

1. **¿Proveedor libre dentro de la caja o solo externo?**
   - (a) `WN_MEDIA_BASELINE=ON` por defecto en el instalador (VP8/VP9/AV1 +
     Opus + WebM/OGG): mejor primera impresión, más binario y más superficie
     de mantenimiento (demuxer in-tree).
   - (b) Solo `media_null` en la caja y todo lo demás como plugin descargable:
     core mínimo, pero "no reproduce nada" de fábrica.
   Recomendación: (a) para el piloto, con el demuxer MKV acotado.

2. **¿Es aceptable el plugin FFmpeg LGPL?**
   Condiciona H.264/H.265/AAC/MP4. Requiere DLL separada, FFmpeg compartido
   `--disable-gpl`, verificación en carga y nota de patentes. Si el owner dice
   no, el catálogo realista se queda en WebM/AV1 + Media Foundation.

3. **¿Prioridad de hardware decode?**
   - (a) `auto`: intentar HW y caer a software (más rápido, más caminos que
     probar, más riesgo de driver).
   - (b) `off` por defecto, opt-in por variable de entorno hasta la fase C
     cerrada.
   Recomendación: (b) en fase B, (a) en fase C con métricas por proveedor.

4. **¿Controles dentro del motor o overlay del host?**
   - (a) Overlay del host (fase A/B): cero coste en el core, cada app hace su
     piel; el documento solo declara `controls`.
   - (b) Controles del motor: consistencia y accesibilidad gratis, pero exige
     hit regions, foco, teclado, i18n y un diseño propio.
   Recomendación: (a) hasta que un segundo consumidor pida (b).

5. **¿La galería añade `<video>` y cuándo se descongela el documento?**
   Ver §11. Es un bloqueante de integración, no una razón para simularlo.

6. **¿`autoplay` sin `muted`?**
   En escritorio no hay política de gesto; se propone honrarlo. Si el owner
   quiere paridad web, `autoplay && !muted` se degrada a `paused`.

7. **¿`muted` sin dispositivo de audio?**
   Se propone que `muted` no abra sink (reloj virtual) y que `unmute`
   re-ancle. Alternativa: mantener el sink a ganancia 0 para no cambiar de
   reloj. La primera gasta menos CPU; la segunda es más estable al alternar.

8. **¿Techo de resolución y presupuesto de memoria?**
   Propuesta: 4K por defecto, 8K solo con `WN_MEDIA_MAX_HEIGHT`, y rechazo
   (`WN_MEDIA_UNSUPPORTED_CODEC` con `detail`) por encima del techo, sin
   downscale silencioso. Falta decidir el presupuesto de texturas por sesión.

---

## 11. Integración con la galería (bloqueante, no hack)

La app de la galería (`benchmarks/pilot-media-library/`) es hoy una
**biblioteca/catálogo sin reproducción**: no tiene `<video>`, ni reproductor,
ni controles. Su documento está **congelado**.

Consecuencias:

1. El runtime puede ganar `<video>` sin tocar la galería: el `.wnui` de la
   galería se regenera a v7 y sigue pintando igual (no tiene nodos `video`).
2. **Usar** `<video>` en la galería requiere, en este orden:
   - soporte de `<video>` en el frontend (subset, diagnósticos),
   - **descongelar y recompilar el documento** de la galería con el nodo nuevo,
   - un proveedor instalado (baseline o FFmpeg),
   - una UI de reproducción (overlay del host, §10.4).
3. Hasta que eso ocurra, la galería **no** puede reproducir vídeo. Y no se
   simula:
   - no se pone un `img` con la ruta del `.mp4`;
   - no se llama `wn_node_set_image` sobre un nodo `video`;
   - no se abre un reproductor externo desde el motor (eso es del host, y ya
     existe como "Abrir con el SO");
   - no se mete estado de reproducción en el documento.

El bloqueante es de **producto y de contrato de documento**, no técnico: el
runtime estará listo en la fase A; la galería no puede consumirlo hasta que su
documento se descongele.
