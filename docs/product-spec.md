# Product spec — webnative

## Visión

UI nativa ligera y reutilizable usando **HTML + CSS como lenguaje de vista**, con
la lógica de la aplicación en el **lenguaje host** a través de una **ABI C**.
Sin navegador, sin WebView, sin motor JS en el core.

## Problema

- Electron / WebView: pesados o dependientes del WebView del OS.
- Qt/QML, XAML, SwiftUI, Flutter: obligan a aprender otro lenguaje/ecosistema de UI.
- Los diseñadores ya saben HTML/CSS; los desarrolladores nativos quieren un motor
  pequeño y embebible.

## Targets

| Prioridad | Modo | Descripción |
|---|---|---|
| **1** | **Standalone** | App de escritorio completa; webnative es el framework (ventana, loop, render). |
| **2** | **Embedded** | Vista dentro de una app existente; el host posee ventana y loop y webnative renderiza en un HWND/surface. |
| No | Documento | Visor de contenido; posible, no prioritario. |

**No es**: navegador, visor web general, apps con JS, sitios content-heavy.

## Usuarios

- Diseñadores/desarrolladores web que quieren UI nativa sin aprender XAML/QML.
- Desarrolladores C/C++/Rust/C# que quieren una UI moderna sin embeber un navegador.

## Modelo de interacción

- **HTML/CSS = vista**. Subset cerrado (`docs/subset-v1.md`).
- **Lógica = lenguaje host** vía ABI C.
- **Eventos** (`click`, `input`, `key`, `resize`) y **comandos**
  (`set_text`, `set_class`).
- **Sin JS en el core**. Scripting opcional como plugin, no como dependencia.

## Plataformas

- **Windows 10/11 x64** primero (Win32 + wgpu-native/D3D12).
- macOS/Linux después (winit/SDL + wgpu Metal/Vulkan).
- Backend GPU alternativo (D2D/D3D11) futuro para footprint mínimo.

## Presupuesto (fase 1)

| Métrica | Objetivo |
|---|---:|
| Instalación total (engine + wgpu DLL + assets) | ≤ 15 MB |
| RAM (RSS) | ≤ 30 MB en hardware (WARP informativo) |
| Arranque a primer frame | ≤ 150 ms |
| CPU por frame (sin vsync) | ≤ 2 ms |
| Layout, 100 nodos | ≤ 0.5 ms |
| Binario del engine (sin wgpu DLL) | ≤ 1 MB |

Futuro Windows-only con D2D/D3D11: ≤ 5 MB total.

## Criterios de éxito (fase 1)

1. ABI C v0 (`include/webnative.h`) con `wn_app_*` y `wn_view_*`.
2. Un solo core para standalone y embedded.
3. R2 69/69 sin regresión.
4. Demos C de ambos modos (sin WPF/WinForms todavía).
5. Harness de métricas con baseline y presupuesto.
6. Subset v1 documentado + diagnósticos estrictos.
7. Renderer detrás de vtable; la ABI no expone wgpu.

## No-goals v1

- Navegador, pestañas, devtools, extensiones.
- Motor JS.
- Web completa: iframes, `@media`, animaciones, web fonts, `grid`.
- Red/HTTP.
- Accesibilidad/IME completos (fase 4; debe planificarse).
- macOS/Linux (fase 5).

## Riesgos

1. **Scope creep** hacia navegador → subset cerrado + no-goals.
2. **Texto/i18n** (bidi, shaping, DPI) más difícil que el layout.
3. **Accesibilidad** sin UIA/ARIA no hay “casi nativo”.
4. **Integración de plataforma** (IME, focus, clipboard) es el 50 % del feel.
5. **Seguridad** de HTML/CSS no confiable; sin scripts por defecto.
6. **Controles nativos vs custom**; decidir por target.

## Relación con el estado actual

Ya existen: frontend Rust con texto, IR display list, UI tree, emisores C,
runtime estático R1 y runtime dinámico R2, paridad multi-lenguaje y benchmark.
La fase 1 convierte eso en un **producto con contratos estables** (ABI, core,
renderer, host, métricas, subset).
