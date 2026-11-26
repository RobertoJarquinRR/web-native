/*
 * host_win32.c - standalone Win32 host behind wn_host_ops.
 *
 * Creates a resizable top-level window, translates Win32 messages into
 * wn_event values and pumps the loop. It never draws: the frame callback is
 * where wn_app calls wn_view_render().
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wn_host.h"
#include "wn_internal.h"

#define WN_HOST_CLASS L"WebNativeAbiHostWindow"

typedef struct {
    HWND hwnd;
    HINSTANCE instance;
    int width;
    int height;
    int running;
    int continuous;
    int mouse_tracking;
    int button_down;      /* 0 none, else 1 left, 2 right, 3 middle */
    int click_count;      /* 2 while a double click release is pending */
    int caret_blink;
    int tick_ms;
    char *title;
    wn_host_event_fn on_event;
    wn_host_frame_fn on_frame;
    void *userdata;
} WnHostWin32;

#define WN_TIMER_CARET 1
#define WN_TIMER_TICK  2

static int g_class_registered;

static uint32_t current_modifiers(void)
{
    uint32_t mods = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= WN_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= WN_MOD_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= WN_MOD_ALT;
    return mods;
}

static void emit_event(WnHostWin32 *host, wn_event_type type, const char *target,
                       float x, float y, int32_t key, uint32_t codepoint,
                       int width, int height)
{
    wn_event event;
    if (host == NULL || host->on_event == NULL) {
        return;
    }
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.target_id = target;
    event.x = x;
    event.y = y;
    event.key = key;
    event.codepoint = codepoint;
    event.width = width;
    event.height = height;
    event.modifiers = current_modifiers();
    host->on_event(host->userdata, &event);
}

static void emit_pointer(WnHostWin32 *host, wn_event_type type, LPARAM lparam)
{
    int x = (int)(short)LOWORD(lparam);
    int y = (int)(short)HIWORD(lparam);
    emit_event(host, type, NULL, (float)x, (float)y, 0, 0, 0, 0);
}

/* ABI v6: button pointer events carry which button and the click count. */
static void emit_pointer_button(WnHostWin32 *host, wn_event_type type,
                                LPARAM lparam, uint8_t button,
                                uint8_t click_count)
{
    wn_event event;
    if (host == NULL || host->on_event == NULL) {
        return;
    }
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.x = (float)(int)(short)LOWORD(lparam);
    event.y = (float)(int)(short)HIWORD(lparam);
    event.modifiers = current_modifiers();
    event.button = button;
    event.click_count = click_count;
    host->on_event(host->userdata, &event);
}

/* WM_MOUSEWHEEL/MOUSEHWHEEL carry screen coordinates. One notch is mapped to
 * 120 px; wheel-forward (away from the user) scrolls content up = delta_y < 0,
 * matching DOM wheel conventions. */
static void emit_wheel(WnHostWin32 *host, LPARAM lparam, int32_t wheel_delta,
                       int horizontal)
{
    wn_event event;
    POINT point;
    if (host == NULL || host->on_event == NULL) {
        return;
    }
    point.x = (int)(short)LOWORD(lparam);
    point.y = (int)(short)HIWORD(lparam);
    ScreenToClient(host->hwnd, &point);
    memset(&event, 0, sizeof(event));
    event.type = WN_EVENT_WHEEL;
    event.x = (float)point.x;
    event.y = (float)point.y;
    event.modifiers = current_modifiers();
    if (horizontal) {
        event.delta_x = (float)wheel_delta * (120.0f / (float)WHEEL_DELTA);
    } else {
        event.delta_y = -(float)wheel_delta * (120.0f / (float)WHEEL_DELTA);
    }
    host->on_event(host->userdata, &event);
}

static LRESULT CALLBACK host_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    WnHostWin32 *host = (WnHostWin32 *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lparam;
        host = (WnHostWin32 *)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)host);
        if (host != NULL) {
            host->hwnd = hwnd;
        }
    }
    if (host == NULL) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    switch (msg) {
    case WM_CLOSE:
        emit_event(host, WN_EVENT_CLOSE, NULL, 0.0f, 0.0f, 0, 0, 0, 0);
        host->running = 0;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_SIZE:
        host->width = (int)LOWORD(lparam);
        host->height = (int)HIWORD(lparam);
        emit_event(host, WN_EVENT_RESIZE, NULL, 0.0f, 0.0f, 0, 0,
                   host->width, host->height);
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track;
        emit_pointer(host, WN_EVENT_POINTER_MOVE, lparam);
        if (!host->mouse_tracking) {
            memset(&track, 0, sizeof(track));
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            if (TrackMouseEvent(&track)) {
                host->mouse_tracking = 1;
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        /* A captured drag keeps ownership of the pointer outside the client
         * area: do not translate the leave into a pointer_leave while the
         * button is down, or the selection would be abandoned mid-drag. */
        host->mouse_tracking = 0;
        if (!host->button_down) {
            emit_event(host, WN_EVENT_POINTER_MOVE, NULL,
                       -1000000.0f, -1000000.0f, 0, 0, 0, 0);
        }
        return 0;
    case WM_LBUTTONDOWN:
        host->button_down = 1;
        host->click_count = 1;
        SetCapture(hwnd);
        emit_pointer_button(host, WN_EVENT_POINTER_DOWN, lparam, 1, 1);
        return 0;
    case WM_LBUTTONDBLCLK:
        /* CS_DBLCLKS turns the second press into this message. */
        host->button_down = 1;
        host->click_count = 2;
        SetCapture(hwnd);
        emit_pointer_button(host, WN_EVENT_POINTER_DOWN, lparam, 1, 2);
        return 0;
    case WM_LBUTTONUP: {
        uint8_t clicks = (uint8_t)(host->click_count > 0 ? host->click_count : 1);
        host->button_down = 0;
        host->click_count = 0;
        emit_pointer_button(host, WN_EVENT_POINTER_UP, lparam, 1, clicks);
        ReleaseCapture();
        return 0;
    }
    case WM_RBUTTONDOWN:
        host->button_down = 2;
        host->click_count = 1;
        SetCapture(hwnd);
        emit_pointer_button(host, WN_EVENT_POINTER_DOWN, lparam, 2, 1);
        return 0;
    case WM_RBUTTONUP:
        host->button_down = 0;
        host->click_count = 0;
        emit_pointer_button(host, WN_EVENT_POINTER_UP, lparam, 2, 1);
        ReleaseCapture();
        return 0;
    case WM_MBUTTONDOWN:
        host->button_down = 3;
        host->click_count = 1;
        SetCapture(hwnd);
        emit_pointer_button(host, WN_EVENT_POINTER_DOWN, lparam, 3, 1);
        return 0;
    case WM_MBUTTONUP:
        host->button_down = 0;
        host->click_count = 0;
        emit_pointer_button(host, WN_EVENT_POINTER_UP, lparam, 3, 1);
        ReleaseCapture();
        return 0;
    case WM_TIMER:
        /* Caret blink and the host tick both only ask for a repaint; the app
         * frame callback decides whether the core needs to be marked dirty. */
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_CAPTURECHANGED:
        /* Capture stolen while the button was held: drop hover/press so
         * nothing sticks. A normal button-up clears button_down first. */
        host->mouse_tracking = 0;
        if (host->button_down) {
            int held = host->button_down;
            POINT point;
            LPARAM where;
            point.x = 0;
            point.y = 0;
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            where = MAKELPARAM((short)point.x, (short)point.y);
            host->button_down = 0;
            host->click_count = 0;
            emit_pointer_button(host, WN_EVENT_POINTER_UP, where,
                                (uint8_t)held, 1);
            emit_event(host, WN_EVENT_POINTER_MOVE, NULL,
                       -1000000.0f, -1000000.0f, 0, 0, 0, 0);
        }
        return 0;
    case WM_MOUSEWHEEL:
        emit_wheel(host, lparam, GET_WHEEL_DELTA_WPARAM(wparam), 0);
        return 0;
    case WM_MOUSEHWHEEL:
        emit_wheel(host, lparam, GET_WHEEL_DELTA_WPARAM(wparam), 1);
        return 0;
    case WM_KEYDOWN:
        emit_event(host, WN_EVENT_KEY_DOWN, NULL, 0.0f, 0.0f,
                   (int32_t)wparam, 0, 0, 0);
        /* Esc does not close here: wn_app closes only when no text input owns
         * the focus (ABI v4). With focus, the view consumes it. */
        return 0;
    case WM_KEYUP:
        emit_event(host, WN_EVENT_KEY_UP, NULL, 0.0f, 0.0f,
                   (int32_t)wparam, 0, 0, 0);
        return 0;
    case WM_CHAR:
        emit_event(host, WN_EVENT_CHAR, NULL, 0.0f, 0.0f, 0,
                   (uint32_t)wparam, 0, 0);
        return 0;
    case WM_SETFOCUS:
        emit_event(host, WN_EVENT_FOCUS_IN, NULL, 0.0f, 0.0f, 0, 0, 0, 0);
        return 0;
    case WM_KILLFOCUS:
        /* The window lost the pointer capture context with the focus. */
        host->mouse_tracking = 0;
        host->button_down = 0;
        emit_event(host, WN_EVENT_POINTER_MOVE, NULL,
                   -1000000.0f, -1000000.0f, 0, 0, 0, 0);
        emit_event(host, WN_EVENT_FOCUS_OUT, NULL, 0.0f, 0.0f, 0, 0, 0, 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static int host_create(void **context, const wn_host_config *config,
                       wn_host_event_fn on_event, wn_host_frame_fn on_frame,
                       void *userdata)
{
    WnHostWin32 *host;
    HINSTANCE instance = GetModuleHandleW(NULL);
    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rect;
    int width;
    int height;
    WNDCLASSEXW wc;
    wchar_t wide_title[256];

    if (context == NULL) {
        return 1;
    }
    *context = NULL;
    host = (WnHostWin32 *)calloc(1, sizeof(*host));
    if (host == NULL) {
        return 1;
    }
    width = config != NULL && config->width > 0 ? config->width : 960;
    height = config != NULL && config->height > 0 ? config->height : 640;
    host->instance = instance;
    host->width = width;
    host->height = height;
    host->on_event = on_event;
    host->on_frame = on_frame;
    host->userdata = userdata;
    host->title = wn_strdup_(config != NULL && config->title != NULL
                                 ? config->title
                                 : "webnative");

    SetProcessDPIAware();

    if (!g_class_registered) {
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = host_wnd_proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = WN_HOST_CLASS;
        if (!RegisterClassExW(&wc)) {
            wn_free_(host->title);
            free(host);
            return 1;
        }
        g_class_registered = 1;
    }

    rect.left = 0;
    rect.top = 0;
    rect.right = width;
    rect.bottom = height;
    AdjustWindowRectEx(&rect, style, FALSE, 0);
    MultiByteToWideChar(CP_UTF8, 0,
                        host->title != NULL ? host->title : "webnative", -1,
                        wide_title, (int)(sizeof(wide_title) / sizeof(wide_title[0])));
    host->hwnd = CreateWindowExW(0, WN_HOST_CLASS, wide_title, style,
                                 CW_USEDEFAULT, CW_USEDEFAULT,
                                 rect.right - rect.left, rect.bottom - rect.top,
                                 NULL, NULL, instance, host);
    if (host->hwnd == NULL) {
        wn_free_(host->title);
        free(host);
        return 1;
    }
    *context = host;
    return 0;
}

static int host_run(void *context)
{
    WnHostWin32 *host = (WnHostWin32 *)context;
    if (host == NULL) {
        return 1;
    }
    host->running = 1;
    {
        int first = 1;
        while (host->running) {
            MSG msg;
            BOOL got = 0;
            int force = 0;
            /* Drain the batch first. Mouse moves can arrive much faster than a
             * WARP frame; process the final pointer state and paint once. */
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                got = 1;
                if (msg.message == WM_QUIT) {
                    host->running = 0;
                    break;
                }
                if (msg.message == WM_PAINT || msg.message == WM_SHOWWINDOW) {
                    force = 1;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (!host->running) {
                    break;
                }
            }
            if (!host->running) {
                break;
            }
            /* Idle UIs render once at startup and after each batch. Tests and
             * demos that need a fixed number of frames opt into continuous mode
             * through wn_app_set_max_frames(). */
            if (host->continuous || got || first) {
                if (host->on_frame != NULL) {
                    host->on_frame(host->userdata, force || first);
                }
                first = 0;
                if (host->continuous && !got) {
                    Sleep(1);
                }
            } else if (host->on_frame != NULL) {
                WaitMessage();
            } else {
                Sleep(1);
            }
        }
    }
    return 0;
}

static void host_request_close(void *context)
{
    WnHostWin32 *host = (WnHostWin32 *)context;
    if (host == NULL) {
        return;
    }
    host->running = 0;
    if (host->hwnd != NULL) {
        PostMessageW(host->hwnd, WM_NULL, 0, 0);
    }
}

static void host_show(void *context)
{
    WnHostWin32 *host = (WnHostWin32 *)context;
    if (host == NULL || host->hwnd == NULL) {
        return;
    }
    ShowWindow(host->hwnd, SW_SHOW);
    UpdateWindow(host->hwnd);
    SetForegroundWindow(host->hwnd);
}

static void *host_native_window(void *context)
{
    WnHostWin32 *host = (WnHostWin32 *)context;
    return host != NULL ? (void *)host->hwnd : NULL;
}

static void *host_surface_handle(void *context)
{
    return host_native_window(context);
}

static void host_set_client_size(void *context, int width, int height)
{
    WnHostWin32 *host = (WnHostWin32 *)context;
    RECT rect;
    DWORD style;
    DWORD exstyle;
    if (host == NULL || host->hwnd == NULL || width <= 0 || height <= 0) {
        return;
    }
    style = (DWORD)GetWindowLongPtrW(host->hwnd, GWL_STYLE);
    exstyle = (DWORD)GetWindowLongPtrW(host->hwnd, GWL_EXSTYLE);
    rect.left = 0;
    rect.top = 0;
    rect.right = width;
    rect.bottom = height;
    AdjustWindowRectEx(&rect, style, FALSE, exstyle);
    SetWindowPos(host->hwnd, NULL, 0, 0, rect.right - rect.left,
                 rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void host_set_continuous(void *context, int enabled)
{
    WnHostWin32 *host = (WnHostWin32 *)context;
    if (host != NULL) {
        host->continuous = enabled != 0;
    }
}

static void host_set_caret_blink(void *context, int enabled)
{
    WnHostWin32 *host = (WnHostWin32 *)context;
    if (host == NULL || host->hwnd == NULL) {
        return;
    }
    if (enabled != 0) {
        if (!host->caret_blink) {
            SetTimer(host->hwnd, WN_TIMER_CARET, 530, NULL);
            host->caret_blink = 1;
        }
    } else if (host->caret_blink) {
        KillTimer(host->hwnd, WN_TIMER_CARET);
        host->caret_blink = 0;
    }
}

/* ABI v6: periodic frame timer. The app frame callback marks the core dirty
 * while a tick is active; here we only own the OS timer. */
static void host_set_tick(void *context, int ms)
{
    WnHostWin32 *host = (WnHostWin32 *)context;
    if (host == NULL || host->hwnd == NULL) {
        return;
    }
    KillTimer(host->hwnd, WN_TIMER_TICK);
    host->tick_ms = ms > 0 ? ms : 0;
    if (ms > 0) {
        SetTimer(host->hwnd, WN_TIMER_TICK, (UINT)ms, NULL);
    }
    InvalidateRect(host->hwnd, NULL, FALSE);
}

static void host_destroy(void *context)
{
    WnHostWin32 *host = (WnHostWin32 *)context;
    if (host == NULL) {
        return;
    }
    if (host->hwnd != NULL) {
        if (host->caret_blink) {
            KillTimer(host->hwnd, WN_TIMER_CARET);
            host->caret_blink = 0;
        }
        if (host->tick_ms > 0) {
            KillTimer(host->hwnd, WN_TIMER_TICK);
            host->tick_ms = 0;
        }
        DestroyWindow(host->hwnd);
        host->hwnd = NULL;
    }
    wn_free_(host->title);
    free(host);
}

static const wn_host_ops g_host_win32_ops = {
    "win32",
    host_create,
    host_run,
    host_request_close,
    host_show,
    host_native_window,
    host_surface_handle,
    host_set_client_size,
    host_set_continuous,
    host_set_caret_blink,
    host_set_tick,
    host_destroy
};

const wn_host_ops *wn_host_win32(void)
{
    return &g_host_win32_ops;
}
