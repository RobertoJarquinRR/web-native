/*
 * host_win32.c - standalone Win32 host behind wn_host_ops.
 *
 * Creates a resizable top-level window, translates Win32 messages into
 * wn_event values and pumps the loop. It never draws: the frame callback is
 * where wn_app calls wn_view_render().
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <imm.h>

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
    /* ABI v7 IME: the input context detached while no editable input owns the
     * focus, the last caret/candidate boxes in client px, and the composition
     * bookkeeping used to tell a commit from a cancel at ENDCOMPOSITION. */
    HIMC ime_context;
    int ime_attached;
    int ime_composing;
    int ime_had_result;
    wn_rect ime_caret;
    wn_rect ime_candidate;
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

/* ------------------------------------------------------------ IME (v7) */
/*
 * IMM32 backend. The core calls these through wn_ime_ops; the WndProc turns
 * WM_IME_* into the generic WN_EVENT_IME_* events. TSF can replace both halves
 * behind this vtable later.
 */
static void emit_ime(WnHostWin32 *host, wn_event_type type, const char *text_utf8,
                     int caret)
{
    wn_event event;
    if (host == NULL || host->on_event == NULL) {
        return;
    }
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.target_id = text_utf8;
    event.key = (int32_t)caret;
    event.x = host->ime_caret.x;
    event.y = host->ime_caret.y;
    event.width = (int32_t)host->ime_caret.w;
    event.height = (int32_t)host->ime_caret.h;
    event.modifiers = current_modifiers();
    host->on_event(host->userdata, &event);
}

/* Place the composition window at the caret and the candidate window at the
 * anchor the core computed. Called on focus and every caret move. */
static void host_ime_apply_position(WnHostWin32 *host)
{
    HIMC himc;
    COMPOSITIONFORM composition;
    CANDIDATEFORM candidate;
    if (host == NULL || host->hwnd == NULL) {
        return;
    }
    himc = ImmGetContext(host->hwnd);
    if (himc == NULL) {
        return;
    }
    memset(&composition, 0, sizeof(composition));
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos.x = (LONG)host->ime_caret.x;
    composition.ptCurrentPos.y = (LONG)host->ime_caret.y;
    ImmSetCompositionWindow(himc, &composition);
    memset(&candidate, 0, sizeof(candidate));
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_CANDIDATEPOS | CFS_EXCLUDE;
    candidate.ptCurrentPos.x = (LONG)host->ime_candidate.x;
    candidate.ptCurrentPos.y = (LONG)host->ime_candidate.y;
    candidate.rcArea.left = (LONG)host->ime_caret.x;
    candidate.rcArea.top = (LONG)host->ime_caret.y;
    candidate.rcArea.right = (LONG)(host->ime_caret.x + host->ime_caret.w);
    candidate.rcArea.bottom = (LONG)(host->ime_caret.y + host->ime_caret.h);
    ImmSetCandidateWindow(himc, &candidate);
    ImmReleaseContext(host->hwnd, himc);
}

static int host_ime_focus(void *context, const wn_rect *caret_rect)
{
    WnHostWin32 *host = (WnHostWin32 *)context;
    if (host == NULL || host->hwnd == NULL) {
        return 1;
    }
    if (caret_rect != NULL) {
        host->ime_caret = *caret_rect;
        host->ime_candidate.x = caret_rect->x;
        host->ime_candidate.y = caret_rect->y + caret_rect->h;
        host->ime_candidate.w = 0.0f;
        host->ime_candidate.h = 0.0f;
    }
    if (!host->ime_attached) {
        if (host->ime_context != NULL) {
            ImmAssociateContext(host->hwnd, host->ime_context);
        } else {
            /* No saved context (no IME at window creation): ask IMM32 for the
             * default one so an IME installed later still works. */
            ImmAssociateContextEx(host->hwnd, NULL, IACE_DEFAULT);
        }
        host->ime_attached = 1;
    }
    host_ime_apply_position(host);
    return 0;
}

/* Escape/editor cancel: discard the platform composition so IMM32 does not
 * keep a preedit alive after the core already cleared its buffer. */
static void host_ime_cancel(void *context)
{
    WnHostWin32 *host = (WnHostWin32 *)context;
    HIMC himc;
    if (host == NULL || host->hwnd == NULL) {
        return;
    }
    himc = ImmGetContext(host->hwnd);
    if (himc != NULL) {
        ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmReleaseContext(host->hwnd, himc);
    }
    /* A cancel may not produce ENDCOMPOSITION on every IME; do not leave the
     * host suppressing WM_CHAR forever. */
    host->ime_composing = 0;
    host->ime_had_result = 0;
}

static void host_ime_blur(void *context)
{
    WnHostWin32 *host = (WnHostWin32 *)context;
    HIMC previous;
    if (host == NULL || host->hwnd == NULL || !host->ime_attached) {
        return;
    }
    previous = ImmAssociateContext(host->hwnd, NULL);
    if (previous != NULL) {
        host->ime_context = previous;
    }
    host->ime_attached = 0;
    host->ime_composing = 0;
    host->ime_had_result = 0;
}

static void host_ime_set_caret(void *context, const wn_rect *caret_rect)
{
    WnHostWin32 *host = (WnHostWin32 *)context;
    if (host == NULL || caret_rect == NULL) {
        return;
    }
    host->ime_caret = *caret_rect;
    if (host->ime_candidate.w == 0.0f && host->ime_candidate.h == 0.0f) {
        host->ime_candidate.x = caret_rect->x;
        host->ime_candidate.y = caret_rect->y + caret_rect->h;
    }
    host_ime_apply_position(host);
}

static void host_ime_set_candidate(void *context, const wn_rect *candidate_rect)
{
    WnHostWin32 *host = (WnHostWin32 *)context;
    if (host == NULL || candidate_rect == NULL) {
        return;
    }
    host->ime_candidate = *candidate_rect;
    host_ime_apply_position(host);
}

static const wn_ime_ops g_ime_win32_ops = {
    "win32-imm32",
    host_ime_focus,
    host_ime_blur,
    host_ime_cancel,
    host_ime_set_caret,
    host_ime_set_candidate
};

static const wn_ime_ops *host_ime_backend(void *context, void **ime_context)
{
    if (ime_context != NULL) {
        *ime_context = context;
    }
    return &g_ime_win32_ops;
}

/* UTF-16 units [0, cursor) as codepoints (surrogate pairs count once). */
static int ime_codepoints_before(const WCHAR *wide, int cursor)
{
    int i;
    int count = 0;
    for (i = 0; i < cursor; ++i) {
        if (wide[i] >= 0xD800 && wide[i] <= 0xDBFF && i + 1 < cursor &&
            wide[i + 1] >= 0xDC00 && wide[i + 1] <= 0xDFFF) {
            ++i;
        }
        ++count;
    }
    return count;
}

/* Reads a composition/result string into UTF-8. `caret` receives the codepoint
 * cursor for GCS_COMPSTR. Returns the byte count written, 0 when empty. */
static int ime_read_string(HWND hwnd, DWORD flag, char *utf8, int capacity,
                           int *caret)
{
    HIMC himc;
    LONG bytes;
    WCHAR wide[256];
    int units;
    int written;
    if (utf8 == NULL || capacity <= 0) {
        return 0;
    }
    utf8[0] = '\0';
    if (caret != NULL) {
        *caret = 0;
    }
    himc = ImmGetContext(hwnd);
    if (himc == NULL) {
        return 0;
    }
    bytes = ImmGetCompositionStringW(himc, flag, NULL, 0);
    if (bytes <= 0) {
        ImmReleaseContext(hwnd, himc);
        return 0;
    }
    units = (int)(bytes / (LONG)sizeof(WCHAR));
    if (units > (int)(sizeof(wide) / sizeof(wide[0]))) {
        units = (int)(sizeof(wide) / sizeof(wide[0]));
    }
    ImmGetCompositionStringW(himc, flag, wide, (DWORD)(units * (int)sizeof(WCHAR)));
    if (caret != NULL) {
        LONG cursor = ImmGetCompositionStringW(himc, GCS_CURSORPOS, NULL, 0);
        if (cursor < 0) {
            cursor = 0;
        }
        if (cursor > units) {
            cursor = units;
        }
        *caret = ime_codepoints_before(wide, (int)cursor);
    }
    written = WideCharToMultiByte(CP_UTF8, 0, wide, units, utf8, capacity - 1,
                                  NULL, NULL);
    ImmReleaseContext(hwnd, himc);
    if (written <= 0) {
        utf8[0] = '\0';
        return 0;
    }
    utf8[written] = '\0';
    return written;
}

static void host_ime_composition(WnHostWin32 *host, LPARAM lparam)
{
    char utf8[512];
    int caret = 0;
    if ((lparam & GCS_RESULTSTR) != 0) {
        if (ime_read_string(host->hwnd, GCS_RESULTSTR, utf8, (int)sizeof(utf8),
                            NULL) > 0) {
            host->ime_had_result = 1;
            emit_ime(host, WN_EVENT_IME_COMMIT, utf8, 0);
        }
        return;
    }
    if ((lparam & GCS_COMPSTR) != 0) {
        int written = ime_read_string(host->hwnd, GCS_COMPSTR, utf8,
                                      (int)sizeof(utf8), &caret);
        emit_ime(host, WN_EVENT_IME_COMPOSITION, utf8, written > 0 ? caret : 0);
    }
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
        /* The IME owns the text while a composition is active (and the core
         * also suppresses chars there). The commit is delivered through
         * WM_IME_COMPOSITION; a duplicate WM_CHAR after it is swallowed by the
         * core, so forwarding both can never insert twice. */
        if (host->ime_composing) {
            return 0;
        }
        emit_event(host, WN_EVENT_CHAR, NULL, 0.0f, 0.0f, 0,
                   (uint32_t)wparam, 0, 0);
        return 0;
    case WM_IME_CHAR:
        /* Same rule for the IME's character message: never insert while the
         * composition is active; after a commit the core swallow buffer drops
         * the duplicate. */
        if (host->ime_composing) {
            return 0;
        }
        emit_event(host, WN_EVENT_CHAR, NULL, 0.0f, 0.0f, 0,
                   (uint32_t)wparam, 0, 0);
        return 0;
    case WM_IME_STARTCOMPOSITION:
        host->ime_composing = 1;
        host->ime_had_result = 0;
        emit_ime(host, WN_EVENT_IME_START, NULL, 0);
        return 0;
    case WM_IME_SETCONTEXT:
        /* The view paints the composition text itself (at the caret), so hide
         * the system composition window; the candidate window stays. */
        lparam &= ~ISC_SHOWUICOMPOSITIONWINDOW;
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_IME_COMPOSITION:
        host_ime_composition(host, lparam);
        return 0;
    case WM_IME_ENDCOMPOSITION:
        /* END follows both a commit and a cancel; only the latter reaches it
         * without a result string. */
        if (!host->ime_had_result) {
            emit_ime(host, WN_EVENT_IME_CANCEL, NULL, 0);
        }
        host->ime_composing = 0;
        host->ime_had_result = 0;
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
    /* Detach the IME until an editable input takes the focus; host_ime_focus
     * restores this context. ImmAssociateContext(NULL) returns the previous
     * context, which may itself be NULL when the system has no IME. */
    host->ime_context = ImmAssociateContext(host->hwnd, NULL);
    host->ime_attached = 0;
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
    host_ime_backend,
    host_destroy
};

const wn_host_ops *wn_host_win32(void)
{
    return &g_host_win32_ops;
}
