# Origen

webnative nació como un **experimento personal de curiosidad**, no como un
producto terminado.

## El punto de partida

Vengo del mundo de la UI web: HTML y CSS me parecen una forma muy directa de
describir una interfaz. Lo que no me interesaba era el trabajo de frontend que
suele acompañarlo —el ecosistema de frameworks, el estado, el tooling— ni el
peso de llevar un navegador completo a una app de escritorio.

Electron y WebView resolvían el problema, pero a un coste que no quería asumir:
un runtime enorme, un motor JS que no necesito y una dependencia del navegador
del sistema. Tampoco quería aprender otro framework de UI radicalmente distinto
solo para hacer una app nativa.

La pregunta fue:

> ¿Y si HTML/CSS fueran solo el **lenguaje de vista** y el motor fuera pequeño,
> nativo y controlable, con la lógica en el lenguaje del host?

## Cómo se exploró

Empecé siguiendo el flujo de compilación: parsear HTML/CSS, construir un árbol,
hacer layout, producir una lista de dibujo y renderizar con GPU. Antes de fijar
herramientas probé varias rutas con Python, C++, C#, Rust y distintas librerías
de parseo, layout, texto y render.

Usé IA como **par de programación** para acelerar la exploración y la
implementación; las decisiones de arquitectura, el alcance, los contratos y la
verificación son míos. El objetivo nunca fue “generar código”, sino entender qué
pieza era la correcta y poder defenderla.

Las implementaciones iniciales quedaron como evidencia en
[`archive/experiments/`](../archive/experiments/). El código que sobrevivió se
promovió a `src/`: frontend Rust, emisores, runtime C estático, runtime C
dinámico y ABI.

## Las decisiones que quedaron

- **HTML/CSS en build time**, sin parser en runtime.
- **IR y UI tree como contratos** entre lenguajes.
- **ABI C** con handles opacos para que el host sea C#, C, C++ o Rust.
- **Renderer detrás de una vtable**: D3D11 en Windows y wgpu-native
  (D3D12/Vulkan/Metal) como alternativa, sin tocar el core.
- **Sin JS, sin iframes, sin red** en el core.
- **Paridad medible**: mismo input, mismo IR, mismos píxeles.

## El piloto

Para probar el motor con algo real, el piloto es una **biblioteca/reproductor
personal open source** de video. La biblioteca, el catálogo, la búsqueda y los
favoritos viven en el host; el playback también es responsabilidad del host o de
una etapa posterior. El motor solo pone la vista nativa.

## Qué es y qué no es

Es un **motor/prototipo de UI nativa** para apps de escritorio: HTML/CSS como
vista, C como motor, ABI C para el host.

No es un navegador, ni un visor web, ni una plataforma para aplicaciones con JS,
iframes o contenido remoto. Es un experimento en alfa, con límites explícitos y
un subset cerrado.

Si te interesa el detalle técnico, sigue por el [README](../README.md), la
[arquitectura](architecture.md) y la [matriz de soporte](support.md).
