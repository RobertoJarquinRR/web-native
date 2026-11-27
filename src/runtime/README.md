# Runtime C (R2)

Motor de UI nativa de WebNative. Carga un documento `.wnui`, calcula layout,
estado y pintado en runtime y lo presenta con D3D11 o wgpu-native. No contiene
parser HTML/CSS: el documento compilado es la única entrada.

## Documento

El contrato es el blob `.wnui` v7. El frontend Rust lo produce con
`frontend-rust --emit-blob`; el formato y la vista de debug están en
[`docs/wnui-format.md`](../../docs/wnui-format.md) y
[`src/frontend/README.md`](../frontend/README.md).

- `wn_document_load_blob(path)` mapea el archivo en solo lectura y valida
  cabecera, secciones, offsets, jerarquía y rangos. Devuelve `NULL` con
  `wn_last_error()` si el blob es inválido.
- `wn_document_load_blob_memory(data, size)` hace lo mismo sobre un buffer que el
  caller mantiene vivo. El documento lo usa para el builtin embebido.
- `wn_app_load_document(app, path)` reemplaza el documento de una app viva y
  conserva el renderer.
- Los documentos están reference-counted (`retain`/`release`/`destroy`). El
  builtin tiene refcount estático.

El builtin del runtime es `generated/builtin.wnui` embebido como
`generated/builtin_blob.h` (array de bytes). `wn_document_builtin()` lo parsea
con el mismo loader que un archivo, una sola vez por proceso. Ya no existe
ninguna tabla de nodos generada en C.

Para regenerar el builtin:

```powershell
python tools\cli.py build --emit     # frontend + blob + CMake (incluye wnui-dump)
python tools\cli.py runtime emit     # solo el blob y el header embebido
```

## Módulos

```
src/runtime/
  runtime_r2.c        host standalone (ventana, loop, CLI --render/--report/--frames)
  wn_view.c/.h        API embedded: crear/destruir, cargar, eventos, render, readback
  wn_app.c/.h         capa de app sobre wn_view (documento, input, focus)
  host_win32.c        ventana Win32, WndProc, timers, DPI básico
  renderer_d3d11.c    backend D3D11 (por defecto en Windows)
  renderer_wgpu.c     backend wgpu-native (D3D12/Vulkan/Metal)
  renderer_wgpu_stub.c stub cuando se compila con WN_WGPU=OFF
  wn_renderer.h       vtable de renderer
  wn_ime.h            vtable de IME (focus/blur/cancel, caret y ventana de candidatos)
  wn_media.h          registro de proveedores, prioridades y media_null (interno)
  wn_core.h           tipos del core (UiNode, handles, eventos)
  wn_common.c         errores thread-local, utilidades
  core/
    wn_core.c         documento -> arena mutable, names/classes, dirty flag
    layout.c          block/flex/grid, scroll, imágenes, texto
    kind_ops.c/.h     tabla de capacidades por tipo de nodo (auto-size, intrínseco, hijos)
    paint.c           paint items ordenados (clip, capa, transform, opacidad) + batches
    scroll.c/.h       servicio x/y: clamp, aplicar y relayout (X reservado)
    input.c           editor de una línea: caret, selección, clipboard
    hit_test.c        hover/press/selected, click/doble click, wheel
  document/
    wn_blob.c         loader del .wnui (archivo y memoria)
    wn_blob.h         estructuras on-disk (incluye WnBlobMediaRecord v7)
    wn_document_format.h   UiNode/UiInput internos
    wn_document_internal.h refcount y storage del documento
  media/
    media_registry.c  cadena de proveedores, orden, handshake de plugins
    media_null.c      proveedor nulo siempre presente, nunca acierta
    media_core.c      estado por nodo <video>, win_node_media_* y eventos
  text/
    wn_text.h         vtable: measure, shape, rasterize
    text_directwrite.cpp backend Windows
    text_runtime.c    instancia única, familias genéricas
    text_atlas.c      atlas dinámico y caché de glifos/runs
  tools/
    wn_dump.c         wnui-dump: proyección de debug del blob (no contrato)
```

## Flujo por frame

```
documento .wnui
  → core (nodos mutables, estado, clases)
  → layout (mide texto, resuelve flex/grid/scroll/imágenes)
  → display list (paint)
  → renderer (D3D11 o wgpu) → surface
```

Los cambios de estado (`set_visible`, `set_text`, `set_image`, clones, input)
marcan el core dirty; el siguiente frame re-layouta y repinta. `wn_view_render`
es síncrono y single-thread: no reentrar.

### Capas de opacidad

Todo nodo con `opacity < 1` es un grupo CSS: su item y su subárbol se pintan en
una textura offscreen y se componen una sola vez. Las hojas también, porque
pueden pintar varias pasadas (borde+fondo, sombra+relleno, cuerpo/ring/
selección/caret de un input, texto o imagen sobre fondo).

- **Memoria:** un target de viewport completo por nivel de anidamiento,
  `W × H × 4` bytes. Los targets se reservan por profundidad, se reutilizan
  entre frames y el pico es `profundidad × W × H × 4` (más la surface). El
  clip del grupo recorta al componer, no reduce la textura.
- **Orden:** sin grupos, contextos z ni posicionamiento se mantiene el orden
  plano histórico por categorías (todos los sólidos, luego imágenes, luego
  glifos). Con al menos un grupo, un contexto `z-index` o un nodo posicionado,
  el replay de comandos dibuja por item (sólido/decoración/imagen/texto) en el
  orden de stacking para contener el subárbol completo en cada capa; es el
  orden más cercano a CSS. Un grupo con clip vacío no reserva textura.
- **Límites:** `WN_MAX_PAINT_LAYER_DEPTH` (512, igual que la profundidad del
  documento) y `WN_MAX_PAINT_LAYERS` (65536). El pool wgpu/D3D11 se indexa por
  profundidad, no por cantidad de grupos hermanos.

### Video (blob v7, ABI v8)

`<video>` es `kind == 6`, una hoja reemplazada con caja propia. La intención
(`src`, `poster`, `controls`, `autoplay`, `loop`, `muted`, `preload`) viaja en la
sección opcional 7 del blob; el estado vivo (posición, volumen, mute, loop) vive
en el core y nunca se escribe al documento. El layout usa el tamaño declarado o
el default HTML 300×150; `object-fit` se resuelve en el core y el póster se pinta
por la misma ruta de imagen (cache WIC por ruta). Sin póster se pinta un
placeholder neutro `#232A36`, no un frame falso ni controles.

`media_null` está siempre al final de la cadena y nunca acierta. `open` sin un
proveedor real termina en `WN_MEDIA_EVENT_ERROR` con
`WN_MEDIA_UNSUPPORTED_CONTAINER`; con un proveedor registrado termina en
`WN_MEDIA_INTERNAL` porque esta fase **no** tiene driver de sesión ni
decodificador. `wn_node_media_get_state` es una lectura copiada y
`set_source` vuelve a `IDLE`. Los subtítulos devuelven `WN_ERR_UNSUPPORTED`.
El contrato completo y lo diferido a E3b están en `docs/media-v1.md`.

### Position (blob v4)

`relative` se dispone en el flujo normal y después se traslada el nodo y su
subárbol (`left`/`top`, o `-right`/`-bottom`); el cursor del padre no cambia y
los hermanos conservan su sitio. `absolute` sale del flujo: block/flex/grid lo
saltan, el containing block es el ancestro posicionado más cercano (o el
viewport cuando no hay ninguno) y los insets se resuelven contra su border box (el layout ignora bordes, así
que coincide con el padding box). Una segunda pasada tras el
layout normal coloca los absolutos cuando todos los containing blocks ya son
finales; `left+right`/`top+bottom` con tamaño `auto` estiran y los porcentajes
se resuelven ahí, no en el frontend. Scroll, clip y hit-test usan los rects
resultantes; el clip del absoluto es el de los hijos de su containing block
(un `overflow` estático intermedio que no es containing block no lo recorta).
`fixed` también sale del flujo, pero resuelve siempre contra el viewport
(aunque la raíz sea menor), ignora el scroll y el clip de los ancestros y se
coloca en una pasada posterior a la de los absolutos. `sticky` sigue en flujo:
la última pasada lo desplaza dentro del scroll container más cercano por
`top`/`bottom` (gana `top`), recortado al content box del padre. El scrollport
es el **content box** del scroller (`rect + padding`), no el padding box de
CSS: un scroller con padding fija en el borde del contenido. Solo
`overflow: scroll` cuenta como scroll container (`overflow: hidden` recorta
pero no fija), y un hijo `fixed` del sticky no se traslada. Los hijos
`absolute` de un `fixed` usan su caja como containing block.

### Overrides de layout (ABI v7)

`wn_node_set_size` y `wn_node_set_position` cambian la geometría de un nodo vivo
sin tocar el `.wnui`: el primero fija `width`/`height` y el segundo `left`/`top`
de un nodo ya posicionado, siempre contra su containing block actual. En cada
eje, `set_position` limpia el lado opuesto (`right`/`bottom`) como una
declaración CSS. `WN_AUTO` (o un tamaño negativo) restaura el valor compilado
del eje; los clones guardan su propia base, copiada del template. Ambas llamadas
marcan `dirty`/`needs_layout` y la siguiente pasada re-layouta y repinta; el
archivo compilado permanece inmutable y los overrides desaparecen al reemplazar
el documento o eliminar el nodo.

### Overrides de estilo (ABI v7)

`wn_node_set_color` y `wn_node_set_background` fijan color de texto y fondo sin
tocar el `.wnui`; `wn_node_set_font_family`/`font_size`/`font_weight` cambian la
tipografía y re-miden/relayoutan. La familia y el tamaño/peso usan los helpers
efectivos (`wn_core_node_font_family`/`font_size`) en layout, shaping, atlas,
input y pintura, así que el override no se queda en un campo decorativo. El
color de texto y las propiedades de fuente heredan al subárbol (override propio
o del ancestro más cercano, luego el valor compilado); el fondo es por nodo.
`wn_node_reset_style` limpia campos `WN_STYLE_*` y `wn_app_set_font_family` es la
capa de tipografía global (recursiva o solo root) para el ajuste del usuario.
Los overrides viajan en una arena paralela (`WnNodeStyle`), no mutan `UiNode`;
un clon copia el override y conserva su base compilada para restaurar. Prioridad:
override de host > estados compilados > base. Solo marcan dirty/needs_layout; el
siguiente frame aplica. No hay parser CSS ni cambios de blob.

### Estados CSS (blob v6, ABI v7)

El frontend compila `:hover`, `:active` (press incluye hover), `:focus`,
`:disabled`, `:focus-visible` y `:focus-within` a estilos efectivos de
`background`, `color`, color de borde y `font-weight`. El estado sólo se aplica
al compuesto sujeto (`.card:hover .label` se diagnostica y no se compila) y la
regla sólo transporta esos campos; cualquier otra declaración en una regla de
estado se diagnostica. `disabled`/`focus` del nodo son bits (`WN_STATE_DISABLED`,
`WN_STATE_FOCUSED`); el input enciende el foco al editar y lo apaga al hacer
blur. Aditivo v7: `WN_STATE_FOCUS_VISIBLE` (0x20) marca foco de
teclado/programático; `WN_STATE_FOCUS_WITHIN` (0x40) es derivado y de sólo
lectura para el host (set_state lo rechaza) y el core lo mantiene en el nodo
enfocado y todos sus ancestros (se recalcula al cambiar foco, deshabilitar,
quitar o reinsertar). La prioridad por campo es
`disabled > press > hover > focus-visible > focus > focus-within > base`, con
`selected` como base de la app y `data-*` como fallback. El peso efectivo se usa
en layout, shaping, atlas y pintura. Un nodo deshabilitado (o su subárbol) es
inerte al puntero pero sigue visible en `wn_node_at` y `wn_node_get_rect`; al
deshabilitar un contenedor se limpian hover/press/focus de todo el subárbol y se
hace blur del input enfocado dentro. `text-overflow: ellipsis` viaja en la
sección aditiva `text_extras`: el runtime recorta la línea y pinta `…` sólo en
una línea (multilínea queda inerte).

### Z-index y stacking contexts (blob v5)

Un contexto lo crea la raíz, un nodo con `opacity < 1`, un nodo posicionado
(`relative`/`absolute`/`fixed`/`sticky`) con `z-index != auto`, o un ítem
flex/grid `static` con `z-index != auto` (se pinta como posicionado). Dentro de
cada contexto se pinta: su propio ítem,
contextos hijos con `z < 0` (ascendente), contenido en flujo en orden de árbol
(solo ítems **no** posicionados que no son contexto), bloques con `z == 0`/`auto`
en orden de árbol (paso 8 de CSS, por encima del flujo: **todos** los grupos de
`opacity < 1` —posicionados o no, CSS Color 3 §3.2 los pone en la capa de un
posicionado con `z: 0`—, contextos `z: 0` (posicionados o ítems flex/grid
static) e ítems `z: auto`) y
contextos con `z > 0` (ascendente). El
bloque de un contexto es contiguo: sus descendientes no escapan del padre.
`z-index` en un nodo `static` que no es ítem flex/grid se ignora; en un ítem
flex/grid `static` crea contexto. `z-index: auto` es estructural y no crea
contexto ni reserva textura: un posicionado con `z: auto` se emite como bloque
atómico del paso 8 (sus posicionados no se unen al contexto abuelo,
simplificación documentada). Un contexto z-only no
reserva textura: la capa solo existe para ordenar y el renderer la reproduce
inline (`needs_texture=0`); un grupo de opacidad sí reserva target y compone.
Un grupo recortado a nada no reserva textura, y un absoluto que escapa conserva
su clip no vacío (sin atenuarse por ese grupo: limitación conocida). El
hit-test recorre los ítems al revés, así que gana el z más alto.

El texto no está horneado. El nodo lleva cadena, familia, tamaño, peso y
`line-height`; DirectWrite mide y shapea, y `text_atlas` cachea glifos y runs.
`font-weight` 100–900 se traduce a `DWRITE_FONT_WEIGHT` (`400`/`700` siguen
siendo `NORMAL`/`BOLD`) y `GetFirstMatchingFont`/`CreateTextFormat` eligen la
cara **instalada** más cercana de la familia. Un peso intermedio toma el canónico
más cercano (`450`→400, `550`→500), no la preferencia 400–500 de CSS Fonts 4.
No se fuerza síntesis: si la familia no trae ese peso (p. ej. `500` en Segoe UI),
DirectWrite puede simular bold o medir/shapear con la cara vecina real. La
medición, el layout de líneas y el atlas incluyen el peso
en su clave de cache. Los inputs se editan en `core/input.c` y su spec (`value`,
`placeholder`, `max_length`, `password`) llega en la sección `inputs` del blob.

### IME (ABI v7)

`wn_ime.h` es la frontera de plataforma: `focus(context, caret_rect)`,
`blur(context)`, `cancel(context)`, `set_caret_rect` y `set_candidate_rect`. El
core no conoce Win32: guarda el vtable y avisa en focus/blur, en cada movimiento
del caret (reset del blink) y una vez por frame mientras hay foco (resize,
scroll o reflow movieron el caret). `host_win32.c` implementa IMM32 (adjunta/
suelta el `HIMC` al enfocar/desenfocar un input, coloca `ImmSetCompositionWindow`
y `ImmSetCandidateWindow`, y `cancel` llama `ImmNotifyIME(CPS_CANCEL)`) y
traduce `WM_IME_STARTCOMPOSITION/COMPOSITION/ENDCOMPOSITION` (y `WM_IME_CHAR`) a
`WN_EVENT_IME_START/COMPOSITION/COMMIT/CANCEL`. TSF puede sustituirlo detrás de
la misma vtable.

El backend pertenece al `wn_view`, no a un core concreto: `wn_view_replace_document`
(la ruta de `wn_app_load_document`) reinstala el vtable en el core nuevo y
restaura el foco por nombre de nodo. El core mantiene una composición por input
enfocado (texto UTF-8 + cursor); el valor no se toca hasta `COMMIT`, que borra
la selección e inserta **una vez**, truncando a `max_length`. `COMMIT` vacío o
sin composición activa no inserta. `CANCEL` y Escape solo limpian el estado (el
valor queda intacto); Escape además avisa al IME de plataforma. Las teclas y
`WM_CHAR`/`WM_IME_CHAR` se suprimen mientras se compone, y el texto se pinta en
el caret con subrayado. Un `WM_CHAR` final que repite el commit se descarta para
no insertar dos veces; el buffer de swallow se limpia al cambiar de foco/nodo.
Un input deshabilitado rechaza la composición. No cambia el blob.

`tests/input/ime_test.c` prueba el core con un backend IME falso y el backend de
texto falso; `tests/input/ime_view_test.c` repite el foco/commit después de
`wn_view_replace_document` para cubrir el B1 (backend perdido al cargar un
documento). La ruta Win32 es un smoke manual: en esta máquina compila/linkea con
`/W4 /WX` e `imm32.lib`, pero una composición real con un IME japonés **no** se
verificó a mano; el gate automático es el backend falso.

`runtime_r2.exe` tiene su propio `WndProc` y **no** instala IME. El smoke manual
del host es `standalone.exe`/`embedded.exe` o `examples/input.c`, que pasan por
`wn_app` + `host_win32.c`.

CJK: el motor no hace fallback de fuentes. Con la familia por defecto (Segoe UI)
`にほん` sale como glifo 0 (cajas) aunque el dato UTF-8 y el commit del IME sean
correctos. Hasta que llegue el fallback DirectWrite, el documento debe declarar
una familia CJK instalada (`"Yu Gothic UI"`, `"Microsoft YaHei"`,
`"Malgun Gothic"`); el fixture `tests/input` ya usa esa pila para el smoke.

## Renderers y host

`wn_renderer` es una vtable. En Windows el backend por defecto es **D3D11
hardware**; **wgpu-native** es la alternativa/fallback. `WN_RENDERER=d3d11` o
`WN_RENDERER=wgpu` fuerza uno para A/B. `WN_WGPU=OFF` compila sin wgpu (D3D11
solo) y enlaza el stub.

Dos modos, un core:

| | Standalone | Embedded |
|---|---|---|
| Ventana | la crea `runtime_r2` | la crea el host |
| Loop | del runtime | del host |
| Surface | swapchain propia | HWND/surface del host |
| Render | automático | `wn_view_render()` a demanda |

La ABI pública está en [`include/webnative.h`](../../include/webnative.h)
(v6: handles estables, estado por nodo, getters, input, refcount). El detalle
está en [`docs/abi.md`](../../docs/abi.md).

## Tests

```powershell
python tools\cli.py test all                 # wgpu + D3D11
python tools\cli.py test all --no-wgpu       # solo D3D11
python tools\cli.py test suite blob          # .wnui load, layout, píxeles
python tools\cli.py test suite dump          # golden de wnui-dump
python tools\cli.py test suite fidelity      # E1 layout + renders
python tools\cli.py test unit                # cargo test + ctest
```

Suites de `tools/wn_cli/suites.py`:

| Suite | Cubre |
|---|---|
| `css` | radios/borde/sombra e imagen; HTML/CSS → blob, campos leídos con `wnui-dump --json` |
| `fidelity` | E1: font-weight, text-align, line-height, margin auto, flex; overrides de layout y estilo v7 |
| `dynamic` | visibilidad, clone/append/remove, demo ABI |
| `input` | caret, selección, clipboard, IME core y view-level (backend falso), demo ABI |
| `offscreen` | readback y caché de imágenes, D3D11 vs wgpu |
| `text` | medición DirectWrite, atlas, wrapping, documento |
| `blob` | loader, builtin vs archivo, píxeles; invariante `max=0` |
| `dump` | `wnui-dump` texto/JSON, golden y errores de archivo |

El gate primario del documento es el suite `blob`: el `app.wnui` canónico debe
mantener su SHA-256 y renderizar idéntico al builtin. `wnui-dump` es una
proyección de debug; su golden detecta regresiones del dump, no define el
contrato.

## Reglas

1. El core no conoce el modo (standalone/embedded) ni el backend GPU.
2. La ABI no expone wgpu ni Win32 (solo handles opacos).
3. Una instancia por hilo de UI; sin locks internos.
4. Sin JS, red, iframes ni parser HTML/CSS en runtime.
5. Subset cerrado (`docs/subset-v1.md`); fuera del subset = diagnóstico en
   build time.
