# E8 — Extensiones CSS: transition, position, z-index, transform, animation, float

> **Nota:** los `pd`/`pw` de la sección 3 son estimaciones orientativas de
> esfuerzo (±50 %), no un compromiso de calendario. Dependen del alcance real,
> de los casos límite y de la cobertura de tests.

Este documento es un **análisis de prioridad y coste**, no un contrato. Nada de
lo que aparece aquí está en el subset v1: `docs/subset-v1.md` los declara fuera
y `docs/support.md` los marca como **No**. La conclusión es una recomendación
para E8 dentro del roadmap, con criterios go/no-go verificables.

Regla de consistencia: si una extensión se adopta, se hace por el mismo camino
que las pilot v2/v3 — frontend que la parsea y emite, schema nuevo
(`webnative.ui-tree/4`), runtime que la aplica, `--pilot` para opt-in y
`--strict-subset` para rechazarla mientras no sea contrato. El bump de versión
del subset se decide **cuando se promueve**, no en este documento.

Nota sobre `docs/product-spec.md`: allí “animaciones” figura dentro del no-goal
“plataforma web completa”. Aquí se interpreta como el **modelo de animación web**
(keyframes, WAAPI, eventos, timeline genérica), no como una transición cerrada
con cuatro propiedades. Si se adopta la transición limitada, el dueño de
`product-spec.md` debe precisar esa línea para que el no-goal siga siendo
coherente; este documento no lo cambia por su cuenta.

## 1. Prioridad y uso real

| Feature | Prioridad | Dónde se nota hoy | Qué pasa si no está |
|---|---|---|---|
| `transition` | **Muy alta** | hover/press de botones y cards, chips de filtro, paneles que aparecen, foco | La UI se siente brusca y “de laboratorio”; es la diferencia más barata entre demo y producto |
| `position` | **Muy alta** | dropdowns, tooltips, badges, overlays/modales, toasts, cabeceras sticky, FAB | Todo overlay se resuelve con trucos de layout o se queda fuera del piloto. `relative`/`absolute` **ya implementados (blob v4)**; `fixed`/`sticky` pendientes |
| `z-index` | **Alta (con overlays)** | orden de modal/toast/tooltip sobre el contenido, sombras y foco | Sin `position` no tiene sentido; con overlays es obligatorio |
| `transform` | **Alta** | elevación en hover (`translateY`), press (`scale`), paneles deslizantes, spinners | Interacciones planas; además es el target barato de animar (no relayout) |
| `animation` | **Media-alta** | skeleton/shimmer, spinners, progreso indeterminado | Se puede cubrir casi todo con `transition` + un caso concreto de rotación en bucle |
| `float` | **Baja / legacy** | layouts antiguos, texto fluyendo alrededor de una imagen | Nadie diseña UI nueva con floats; el coste/beneficio es malo |

Orden recomendado de inversión: **transition → transform 2D → position
relative/absolute → z-index simple → (decisión aparte) animation keyframes →
no-go float**.

## 2. “CSS completo” vs “contrato limitado”

“CSS completo” significa implementar la especificación del navegador, no una
versión razonable. Ese es el problema: cada propiedad arrastra interacciones con
layout, pintado, hit-test, clipping, texto, scroll y stacking. El coste no es la
propiedad, es el modelo que exige.

| Feature | Qué significa “completo” | Por qué es tamaño navegador | Recomendación limitada v1 |
|---|---|---|---|
| `transition` | Cualquier propiedad animable, `all`, listas, `cubic-bezier()`/`steps()`, delays, reversa, interrupción/retarget, eventos `transitionrun/start/end/cancel`, unidades mezcladas, interpolación de color en varios espacios | Necesita reloj, scheduler de frames, interrupción, invalidación selectiva y semántica de valores para cada propiedad | **Solo `opacity`, `background-color` y `border-color`** en v1 (transform se suma cuando exista transform 2D); duración y `ease`, `linear`, `ease-in`, `ease-out`; sin `all`, sin delays, sin eventos; interrupción = saltar al valor actual y re-interpolar |
| `transform` | Matriz 2D/3D, `transform-origin`, funciones múltiples, `perspective`, anidamiento, interacción con `position: fixed`, `backface-visibility` | Cambia el espacio de pintado, el hit-test, el clip, el rasterizado de texto y sombras; 3D añade proyección y composición | **Solo 2D**: `translate`, `scale`, `rotate` alrededor del centro; una matriz por nodo; sin 3D ni perspectiva; texto solo en nodos sin rotación (o re-raster limitado) |
| `position` | `static`, `relative`, `absolute`, `fixed`, `sticky`, `inset`, `auto` resuelto contra containing block, containing blocks por transform/filter/contain, interacción con scroll y `overflow` | Exige sacar nodos del flujo, calcular containing blocks, clipping por ancestro, scroll y orden de pintado | **`relative` + `absolute`** contra el ancestro posicionado más cercano (o raíz); `inset` en px; `fixed` y `sticky` en una fase posterior |
| `z-index` | Contextos de apilamiento completos (position, transform, opacity, filter, isolation), orden entre contextos, enteros y `auto` | Es un árbol de contextos, no un número: paint order, hit-test y clipping deben compartir el mismo orden | **Implementado (blob v5)**: contextos anidados (raíz, `opacity < 1`, posicionado con `z != auto`), pasos negativos/0/positivos, hit-test en orden inverso; capas z-only sin textura |
| `animation` | `@keyframes`, múltiples animaciones, `%` arbitrarios, `iteration-count`, `direction`, `fill-mode`, `delay`, `play-state`, eventos, composición con transitions | Es una línea de tiempo por nodo con su propio modelo de estados y eventos; equivale a un mini WAAPI | **Un `@keyframes` por nodo**, `transform`/`opacity`, `linear`/`ease`, `infinite`, sin eventos ni `fill-mode`; o directamente **no-go v1** y cubrir spinners en el host |
| `float` | `left`, `right`, `clear`, block formatting contexts, margin collapsing alrededor, line boxes, shrink-to-fit, interacción con flex/grid | Reimplementa el modelo de flujo históricamente más frágil de CSS, incluido el texto que rodea | **No-go**. Si un target real pide texto alrededor de imagen, se evalúa un contrato propio y estrecho, no `float` |

## 3. Coste aproximado

Estimación para **un ingeniero C/runtime con experiencia en el repo**, sin contar
diseño, fixtures ni docs (que suman un 20–30 %). Rango ±50 %. “pd” = persona-día
de trabajo efectivo; “pw” = persona-semana (~5 pd).

| Feature | Limitado (pd) | Completo (pw) | Nota de coste |
|---|---:|---:|---|
| `transition` | **4–8 pd** | 3–6 pw | El reloj/frame timer y la invalidación son el 70 %; interpolar 4 propiedades es barato |
| `transform` 2D | **6–10 pd** | 4–8 pw | Matriz, hit-test inverso, clip y texto; 3D es otro proyecto |
| `position: relative` | **3–5 pd** | — | Offset sin sacar del flujo; barato y de alto valor |
| `position: absolute` | **6–10 pd** | 6–12 pw (familia completa) | Containing block + out-of-flow + medición de auto |
| `position: fixed` | **+2–4 pd** | — | Coordenadas de viewport, ignora scroll; modales/toasts |
| `position: sticky` | **5–8 pd** | — | Clamp por scroll y ancestro con scroll; fácil de romper |
| `z-index` (contextos anidados) | **3-5 pd** | 4-8 pw | **Hecho (blob v5)**: requiere `position`; árbol de contextos con orden CSS simplificado |
| `animation` keyframes | **8–15 pd** | 6–10 pw | Línea de tiempo, fill/direction/iteration y eventos |
| `float` | **8–12 pd** (frágil) | 6–10 pw | Aun el limitado arrastra BFC y margin collapse |
| **Paquete recomendado** | **~5–7 pw** | — | transition + transform 2D + relative/absolute + z-index simple |

Lectura: el paquete limitado cuesta menos que la **suma** de las versiones
completas de transition y transform (7–14 pw), y cubre las cuatro features de
uso más alto. Ese es el argumento central de este documento.

## 4. Por qué cada una es más cara de lo que parece

### 4.1 `transition` necesita reloj y frames

Hoy el host Win32 duerme cuando no hay mensajes (commit `1179320`) y el frame
solo se pinta con dirty. Una transición obliga a lo contrario durante su
duración: **pedir frames a ritmo de monitor** y avanzar un reloj monotónico.
Eso toca:

- `wn_host`: tiempo monotónico + “request frame” (el contrato ya nombra
  “tiempo”, falta el uso).
- Core: estado por nodo (valor inicial, objetivo, inicio, duración, easing),
  muestreo por frame e invalidación de solo las regiones afectadas.
- Interrupción: si llega un nuevo target a mitad, se parte del valor actual, no
  del inicial. Sin esto, hover rápido produce saltos.
- Determinismo: PNG de aceptación y benchmarks no pueden depender del reloj.
  Hace falta `--no-animations` / tiempo congelado y respetar *reduced motion*
  (`docs/platform-v1.md`).
- Propiedades de layout (`width`, `height`, `top`): animarlas fuerza re-layout
  por frame. Recomendación: **fuera del v1**; en P1 solo `opacity` y color
  (composición), y `transform` cuando exista transform 2D.

Coste real: no es “interpolar un float”, es un scheduler con cancelación y una
política de invalidación.

### 4.2 `position` y `z-index` necesitan containing blocks y stacking

R2 calcula un flujo único (block/flex/grid) y pinta en pre-orden. `absolute`
rompe ambas cosas: el nodo sale del flujo, se mide contra un **containing
block** y se pinta en otro orden. `fixed` añade viewport y scroll; `sticky`
añade un clamp dependiente del scroll de cada ancestro.

`z-index` no es un entero suelto: en CSS crea **contextos de apilamiento**, y el
orden entre contextos decide paint, clip y hit-test. La propuesta limitada
original (un solo nivel por padre) quedó superada: la lane v5 implementa el
árbol de contextos anidados del Apéndice E (simplificado sin floats/inline),
con un contexto z-only que solo ordena y un grupo de opacidad que reserva
textura. Eso cubre modales/toasts/tooltips y es testeable.

Riesgo principal: hit-test y clipping deben usar **el mismo orden y la misma
geometría** que el pintado. Si divergen, aparecen clicks en el sitio equivocado,
el bug más caro de diagnosticar.

### 4.3 `transform` afecta hit-test, texto y clip

Un `transform` no es “mover el quad al pintar”:

- **Hit-test**: hay que aplicar la matriz inversa al punto y probar en espacio
  local; con rotación, la caja deja de ser axis-aligned.
- **Clip**: el rect de clip del padre se transforma; la intersección se hace en
  espacio de dispositivo.
- **Texto**: escalar un atlas pre-shapeado lo emborrona. Opciones: re-rasterizar
  a la escala (caro, cachear) o aceptar blur y documentarlo. Rotar texto exige
  pasar de quads a geometría transformada.
- **Sombras/radius**: el blur y el borde siguen la matriz; si se aproximan, la
  paridad visual cae.

El limitado (translate/scale/rotate alrededor del centro, sin 3D) es asumible si
se acepta: texto sin rotación y SSIM con tolerancia en nodos transformados.

### 4.4 `animation` es una línea de tiempo

`@keyframes` añade a la transición: múltiples paradas, `%`, `iteration-count`,
`direction`, `fill-mode`, `delay`, `play-state`, animaciones simultáneas sobre
el mismo nodo y eventos. Es un modelo de estados por nodo con memoria. Para un
spinner o un skeleton no hace falta: se puede resolver con una transición en
bucle o con el host. Por eso la recomendación es **no-go v1** salvo caso
concreto.

### 4.5 `float` no compensa

`float` obliga a reimplementar block formatting contexts, `clear`, margin
collapsing alrededor de flotantes, line boxes y shrink-to-fit, y a convivir con
flex/grid. Su único uso vigente —texto rodeando una imagen— es minoritario en
apps desktop y se puede resolver con layout explícito. **No-go permanente**,
revisable solo con un target real que lo exija.

## 5. Recomendación

1. **Sacar `transition` limitado de “futuro”.** Pasa a **P1 después de E1**
   (fidelidad visual/layout). Es la extensión con mejor relación
   percepción/coste y desbloquea el *feel* nativo.
2. **`transform` 2D y `position` relative/absolute + `z-index` simple: P2**,
   después de transition y de que E1 estabilice layout/pintado. Al llegar
   `transform`, se añade como propiedad animable a la transición limitada.
   Forman el paquete de overlays y microinteracción.
3. **`fixed` y `sticky`: P2/P3** por separado; dependen de scroll y viewport.
4. **`animation` keyframes: futuro**; solo si un target concreto (spinner,
   skeleton) no se cubre con transition.
5. **`float`: no-go**.
6. Todo entra como **pilot** (`--pilot`, schema `/4`) y solo se promueve al
   subset cuando cumple los criterios de abajo y el presupuesto de
   `docs/metrics.md`.

## 6. Criterios go/no-go

Un “go” exige todos los puntos de su bloque; si falla uno, se queda en pilot o
se descarta.

### Transition (limitado)

- [ ] E1 cerrado: baseline visual estable y sin regresiones de layout.
- [ ] `wn_host` expone reloj monotónico y petición de frame; el loop no gira en
      vacío cuando no hay animación activa.
- [ ] Interrupción/retarget correcta en un test de hover rápido (sin saltos).
- [ ] CPU de frame con 50 transiciones activas ≤ **+0,2 ms** a 1280×800.
- [ ] `--no-animations` produce PNG byte-idéntico al estado final; reduced
      motion desactiva transiciones.
- [ ] Tamaño del engine ≤ **+1 MB**; install size sigue ≤ 15 MB.
- [ ] 20+ checks de aceptación (hover, press, foco, retarget, cancelación).

### Transform 2D

- [ ] Hit-test con matriz inversa acierta en 100 % de un fixture con
      translate/scale/rotate.
- [ ] Clip del ancestro transformado coincide con el pintado.
- [ ] Texto: o no se rota (documentado) o se re-rasteriza; SSIM de la región de
      texto ≥ umbral de `docs/visual-parity.md`.
- [ ] Sin 3D, sin perspectiva, sin `transform-origin` arbitrario en v1.

### Position relative/absolute + z-index

- [x] `absolute` mide contra el ancestro posicionado correcto; `auto` resuelve
      sin ambigüedad (blob v4, `position_test` + fixture de píxeles).
- [x] El nodo absoluto no altera el flujo de sus hermanos (test de layout).
- [x] Orden de pintado y hit-test coinciden en un fixture con solape y
      `z-index` negativo/0/positivo (blob v5, `zindex_test` + fixture de
      píxeles 400x550 en D3D11 y wgpu). Contextos anidados: un hijo no escapa
      del padre; `opacity + z` combina textura y orden; el paso 8 pinta los
      posicionados `z: auto`/`0` juntos en orden de árbol, sobre el flujo.
- [x] Clip por `overflow: hidden` del ancestro correcto: el containing block
      recorta, y un `overflow` estático que no es el containing block no
      recorta al absoluto (verificado en `position_test`).
- [x] `fixed`/`sticky` quedan explícitamente fuera hasta su propio go
      (diagnosticados por el subset).

### Animation keyframes

- [ ] Existe un target concreto que no se cubre con transition.
- [ ] Una sola animación por nodo, solo `transform`/`opacity`, sin eventos.
- [ ] Coste y determinismo equivalentes a transition; si no, no-go.

### Float

- [ ] **No-go** por defecto. Revisión solo con un target real y un contrato
      alternativo (p. ej. texto alrededor de imagen) que no reintroduzca BFC.

## 7. Relación con otros documentos

- `docs/subset-v1.md` — features fuera de v1 y regla de versionado.
- `docs/support.md` — matriz real de lo que el runtime aplica hoy.
- `docs/wnui-format.md` — el contrato vigente; una extensión CSS se refleja en
  el blob (los `docs/ui-tree-*.md` son históricos).
- `docs/platform-v1.md` — reduced motion, DPI y foco, que condicionan animación.
- `docs/visual-parity.md` — cómo se mide que transform/position no rompen los
  píxeles.
- `docs/architecture.md` — capas que toca cada extensión (core, host,
  renderer) y regla de que el IR no contiene conceptos de GPU.
