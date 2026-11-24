# Platform v1 — madurez de plataforma

## Estado y definiciones

Hoy el host Win32 es mínimo: ventana, message loop, resize, puntero, teclado
(`WM_KEY*`/`WM_CHAR`), wheel, focus in/out y DPI **system-aware**
(`SetProcessDPIAware`). Una ventana, sin integración con el SO.

Prioridad y fase coinciden: **P1** antes de la beta del piloto (junto a UI
dinámica y text input), **P2** integración de SO antes del release, **P3**
producto y cross-platform. Gate: **Usable** = sin esto no es una app de escritorio;
**Polish** = calidad percibida; **Later** = no ahora.

## Checklist

| Área | Por qué importa | Estado actual (Win32 básico) | Prio | Gate |
|---|---|---|---|---|
| **DPI / zoom / multi-monitor** | A 125–200 % todo debe verse nítido; laptop+dock con escalas distintas es lo normal. Layout, atlas y hit-test dependen de la escala. | Parcial: system-aware; `dpi_scale` solo al crear/redimensionar; sin `WM_DPICHANGED`, sin re-rasterizar atlas ni multi-monitor. | P1 | Usable (1 monitor) / Polish (hot-swap) |
| **Foco / navegación teclado** | Sin foco por nodo no hay `Tab`, `Enter`, `Esc` ni accesibilidad; es la mitad del feel nativo. | Parcial: `WM_SETFOCUS/KILLFOCUS` a nivel vista y key/char; sin modelo de foco por nodo ni tab order. | P1 | Usable |
| **IME** | CJK y varios idiomas exigen composición; sin IME el input no sirve fuera de LATAM/US. | No: sin `WM_IME_*` ni TSF. | P1 si el target lo exige; si no P2 | Usable (target) / Polish |
| **Clipboard** | Copiar/pegar texto y rutas es básico; sin esto el input se siente roto. | No: sin `OpenClipboard`. Entra con el MVP de text input. | P1 | Usable |
| **Drag & drop** | Importar archivos/carpetas arrastrando y reordenar. | No: sin `WM_DROPFILES`/`IDropTarget`. | P2 | Polish |
| **File dialogs** | Elegir carpeta raíz y abrir archivos. **No** se implementan en el engine: el host abre `IFileDialog` y pasa la ruta. | No. | P2 | Usable (A2) |
| **Notifications** | Avisar de fin de escaneo o errores con la ventana detrás. | No: sin `Shell_NotifyIcon`/toast. | P2 | Polish |
| **Tray** | App residente en segundo plano. | No. | P3 | Later |
| **Multi-window** | Detalle o preferencias en ventana aparte; el piloto no lo necesita. | No: 1 app = 1 ventana. | P3 | Later |
| **Single instance** | Doble clic en un archivo debe reutilizar la instancia; evita dos catálogos. | No: sin mutex ni `WM_COPYDATA`. | P2 | Usable si hay file association |
| **Crash handling** | Un fallo debe dejar minidump y reiniciar limpio; sin esto no hay beta. | No: crash = cierre. | P2 | Polish (beta gate) |
| **Packaging / installer** | Instalar, desinstalar, actualizar, versionar DLL y assets. | No. | P3 | Usable (release) |
| **Code signing** | SmartScreen y confianza del usuario. | No. | P3 | Usable (release), admite excepción documentada |
| **Accessibility (UIA)** | Sin UIA no hay lector de pantalla ni automatización; requisito de “casi nativo”. | No: sin provider UIA. | P1 | Usable (baseline) |
| **High contrast** | Usuarios con HC necesitan colores del sistema y bordes visibles. | No: paleta propia fija. | P2 | Usable (a11y baseline) |
| **Reduced motion** | Respetar “animaciones desactivadas”. | N/A: aún no hay animaciones. | P3 | Polish |

## Qué falta para “usable” vs “polish”

**Usable (gate de release v1):** foco/teclado completo; DPI 100–200 % nítido en
un monitor; text input + clipboard; UIA mínimo (ventana, button, list, text);
high contrast; installer/uninstaller con versión; crash handler con dump;
single instance si hay asociación de archivos.

**Polish:** hot-swap multi-monitor, IME si no es target, drag & drop,
notifications, tray, multi-window, reduced motion y firma de código (trust, no
función).

## Cross-platform

La frontera ya existe: **`wn_host`** (ventana, input, superficie, tiempo) y
**`wn_renderer`** (wgpu). El core C no debe ganar `#ifdef` de plataforma.

| Portable (core C) | Adaptador `wn_host` por OS | Fuera del engine |
|---|---|---|
| tree, style, layout, hit test | ventana, loop, swapchain | installer, updates, signing |
| foco, widgets, estados | input mapping, DPI, IME | single instance, crash reporting |
| IR, ABI, handles, UTF-8 | clipboard, DnD, file dialogs | tray, notifications, asociaciones |
| texto (pre-shape en build; HarfBuzz después) | a11y bridge, HC, reduced motion | code signing, notarización |

- **Windows:** seguir con Win32. Es la mejor ruta para IME/UIA; no migrar a SDL.
- **macOS/Linux:** host en Rust con **winit + wgpu-native** (una pila para
  ventana y GPU; el core C entra por FFI). SDL3 es aceptable si se exige host C,
  pero la accesibilidad sigue siendo nativa (Cocoa / AT-SPI).
- **Orden:** macOS primero (Metal), Linux después (Vulkan/X11-Wayland). El
  windowing es ~20 % del trabajo; IME, a11y, packaging y signing son el 80 %.

## Non-goals (permanentes)

Motor JS, parser HTML/CSS en runtime, iframes, stack de red (HTTP/WebSocket),
plataforma web completa (formularios, `@media`, web fonts, service workers),
devtools/extensiones, reproducción de media y multi-tab de navegador. Cualquier
parser o JS futuro es plugin/proyecto separado, fuera del core.

## Próximas 3 inversiones (tras UI dinámica + text input)

1. **Navegación por teclado y modelo de foco completo.** El input ya tiene
   caret; falta el resto: foco por nodo, `Tab`/`Shift+Tab`, roving tabindex,
   `Esc`/`Enter` consumidos por el widget (hoy `Esc` cierra la app en
   `host_win32.c`), restauración de foco y anillo visible. *Aceptación:* flujo
   de la biblioteca solo con teclado, sin trampas de foco.
2. **DPI / zoom / multi-monitor.** Per-monitor v2 en el manifest,
   `WM_DPICHANGED`, contrato de **px lógicos** (1 px = 1/96"), re-raster de
   atlas, resize de swapchain y hit-test coherente. *Aceptación:* 100/125/150/
   200 % sin blur; mover la ventana entre monitores sin reiniciar.
3. **Accesibilidad UIA.** Provider con árbol, rol, nombre, valor, estado y foco;
   patrones Invoke, Toggle, Selection y TextPattern básico. *Aceptación:* un
   lector de pantalla recorre y opera búsqueda, grid y botones; UIA Automation
   puede hacer el flujo A1.

**Siguiente ola:** installer + signing, single instance, crash handler y, si el
target lo pide, IME/TSF. Se planifican como release gates, no como arquitectura.
