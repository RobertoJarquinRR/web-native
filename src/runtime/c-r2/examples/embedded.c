/*
 * embedded.c - C ABI demo: a host-owned parent window with a wn_view attached
 * to a child window. The host owns the loop and forwards pointer/resize
 * messages; the view renders on a timer.
 *
 *   embedded.exe [--frames N] [--input-demo] [--resize-demo]
 */
#include "webnative.h"
#include "wn_view.h"
#include "demo_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARENT_CLASS L"WnAbiEmbeddedParent"
#define CHILD_CLASS  L"WnAbiEmbeddedChild"

static wn_view *g_view;
static HWND g_parent;
static HWND g_child;
static int g_max_frames = 0;
static int g_frames = 0;
static int g_failed = 0;
static int g_input_demo = 0;
static int g_resize_demo = 0;
static int g_stage = 0;
static int g_pointer_ups = 0;
static int g_child_tracking = 0;
static double g_t0 = 0.0;
static double g_first_frame_ms = 0.0;
static double g_frame_total_ms = 0.0;
static double g_cpu_total_ms = 0.0;

static uint32_t demo_modifiers(void)
{
    uint32_t mods = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= WN_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= WN_MOD_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= WN_MOD_ALT;
    return mods;
}

static void on_view_event(void *userdata, const wn_event *event)
{
    (void)userdata;
    if (event == NULL) {
        return;
    }
    switch (event->type) {
    case WN_EVENT_POINTER_MOVE:
        printf("embedded: event POINTER_MOVE (%.0f,%.0f) target=%s\n",
               event->x, event->y,
               event->target_id != NULL ? event->target_id : "(none)");
        break;
    case WN_EVENT_POINTER_DOWN:
        printf("embedded: event POINTER_DOWN target=%s\n",
               event->target_id != NULL ? event->target_id : "(none)");
        break;
    case WN_EVENT_POINTER_UP:
        printf("embedded: event POINTER_UP target=%s\n",
               event->target_id != NULL ? event->target_id : "(none)");
        if (event->target_id != NULL) {
            ++g_pointer_ups;
            if (strstr(event->target_id, "/div.nav") != NULL && g_view != NULL) {
                wn_status status = wn_view_set_class(g_view, event->target_id,
                                                     "selected", 1);
                printf("embedded: set_class(selected) -> %s\n",
                       status == WN_OK ? "WN_OK" : "error");
                status = wn_view_set_text(g_view, "text[1]", "Texto");
                printf("embedded: set_text(text[1], \"Texto\") -> %s%s%s\n",
                       status == WN_OK ? "WN_OK" : "error",
                       status == WN_OK ? "" : ": ",
                       status == WN_OK ? "" : (wn_last_error() != NULL ? wn_last_error() : "?"));
            }
        }
        break;
    case WN_EVENT_RESIZE:
        printf("embedded: event RESIZE %dx%d\n", event->width, event->height);
        break;
    case WN_EVENT_CLOSE:
        printf("embedded: event CLOSE\n");
        break;
    default:
        break;
    }
}

static void forward_pointer(UINT msg, LPARAM lparam)
{
    wn_event event;
    int x;
    int y;
    wn_status status;
    if (g_view == NULL) {
        return;
    }
    x = (int)(short)LOWORD(lparam);
    y = (int)(short)HIWORD(lparam);
    memset(&event, 0, sizeof(event));
    if (msg == WM_MOUSEMOVE) {
        event.type = WN_EVENT_POINTER_MOVE;
    } else if (msg == WM_LBUTTONDOWN) {
        event.type = WN_EVENT_POINTER_DOWN;
    } else if (msg == WM_LBUTTONUP) {
        event.type = WN_EVENT_POINTER_UP;
    } else {
        return;
    }
    event.x = (float)x;
    event.y = (float)y;
    event.modifiers = demo_modifiers();
    status = wn_view_event(g_view, &event);
    printf("embedded: %s (%d,%d) forwarded -> %s\n",
           msg == WM_MOUSEMOVE ? "WM_MOUSEMOVE" :
           (msg == WM_LBUTTONDOWN ? "WM_LBUTTONDOWN" : "WM_LBUTTONUP"),
           x, y, status == WN_OK ? "WN_OK" : "error");
    if (msg == WM_LBUTTONDOWN) {
        SetCapture(g_child);
    } else if (msg == WM_LBUTTONUP) {
        ReleaseCapture();
    }
}

static LRESULT CALLBACK child_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        forward_pointer(msg, lparam);
        if (msg == WM_MOUSEMOVE && !g_child_tracking) {
            TRACKMOUSEEVENT track;
            memset(&track, 0, sizeof(track));
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            if (TrackMouseEvent(&track)) {
                g_child_tracking = 1;
            }
        }
        return 0;
    case WM_MOUSELEAVE: {
        wn_event event;
        g_child_tracking = 0;
        memset(&event, 0, sizeof(event));
        event.type = WN_EVENT_POINTER_MOVE;
        event.x = -1000000.0f;
        event.y = -1000000.0f;
        printf("embedded: WM_MOUSELEAVE forwarded\n");
        if (g_view != NULL) {
            wn_view_event(g_view, &event);
        }
        return 0;
    }
    case WM_SIZE: {
        int width = (int)LOWORD(lparam);
        int height = (int)HIWORD(lparam);
        printf("embedded: WM_SIZE child %dx%d\n", width, height);
        if (g_view != NULL) {
            wn_view_resize(g_view, width, height, 1.0f);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    (void)wparam;
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void set_parent_client_size(int width, int height)
{
    RECT rect;
    DWORD style;
    DWORD exstyle;
    if (g_parent == NULL) {
        return;
    }
    style = (DWORD)GetWindowLongPtrW(g_parent, GWL_STYLE);
    exstyle = (DWORD)GetWindowLongPtrW(g_parent, GWL_EXSTYLE);
    rect.left = 0;
    rect.top = 0;
    rect.right = width;
    rect.bottom = height;
    AdjustWindowRectEx(&rect, style, FALSE, exstyle);
    SetWindowPos(g_parent, NULL, 0, 0, rect.right - rect.left,
                 rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void run_input_demo(void)
{
    RECT client;
    int width;
    int height;
    if (!g_input_demo || g_child == NULL) {
        return;
    }
    GetClientRect(g_child, &client);
    width = client.right - client.left;
    height = client.bottom - client.top;
    if (g_frames == 8) {
        SendMessageW(g_child, WM_MOUSEMOVE, 0, MAKELPARAM(76, 67));
    } else if (g_frames == 9) {
        SendMessageW(g_child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(76, 67));
    } else if (g_frames == 10) {
        SendMessageW(g_child, WM_LBUTTONUP, 0, MAKELPARAM(76, 67));
    } else if (g_frames == 12) {
        SendMessageW(g_child, WM_MOUSEMOVE, 0, MAKELPARAM(width - 20, height - 20));
    }
}

static LRESULT CALLBACK parent_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_SIZE:
        if (g_child != NULL) {
            int width = (int)LOWORD(lparam);
            int height = (int)HIWORD(lparam);
            printf("embedded: WM_SIZE parent %dx%d -> child\n", width, height);
            SetWindowPos(g_child, NULL, 0, 0, width, height,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_TIMER: {
        wn_view_stats before;
        wn_view_stats after;
        wn_status status;
        double t0;
        double t1;
        if (g_failed || g_view == NULL) {
            return 0;
        }
        if (IsIconic(g_parent) || wn_view_render_width(g_view) <= 0 ||
            wn_view_render_height(g_view) <= 0) {
            return 0;
        }
        if (g_resize_demo) {
            if (g_stage == 0 && g_frames >= 5) {
                g_stage = 1;
                printf("embedded: resize-demo frame %d -> client 720x520\n", g_frames);
                set_parent_client_size(720, 520);
            } else if (g_stage == 1 && g_frames >= 15) {
                g_stage = 2;
                printf("embedded: resize-demo frame %d -> client 900x640\n", g_frames);
                set_parent_client_size(900, 640);
            }
        }
        run_input_demo();
        memset(&before, 0, sizeof(before));
        memset(&after, 0, sizeof(after));
        wn_view_get_stats(g_view, &before);
        t0 = demo_now_ms();
        status = wn_view_render(g_view);
        t1 = demo_now_ms();
        if (status != WN_OK) {
            printf("embedded: wn_view_render failed: %s\n",
                   wn_last_error() != NULL ? wn_last_error() : "(no detail)");
            g_failed = 1;
            PostQuitMessage(1);
            return 0;
        }
        wn_view_get_stats(g_view, &after);
        g_frame_total_ms += t1 - t0;
        g_cpu_total_ms += after.cpu_frame_total_ms - before.cpu_frame_total_ms;
        if (g_frames == 0) {
            g_first_frame_ms = t1 - g_t0;
        }
        ++g_frames;
        printf("embedded: frame %d rendered %dx%d\n", g_frames,
               wn_view_render_width(g_view), wn_view_render_height(g_view));
        if (g_max_frames > 0 && g_frames >= g_max_frames) {
            PostQuitMessage(0);
        }
        return 0;
    }
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            PostQuitMessage(0);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static int parse_int(const char *text, int fallback)
{
    char *end = NULL;
    long value;
    if (text == NULL) {
        return fallback;
    }
    value = strtol(text, &end, 10);
    if (end == text || value < -1 || value > 1000000) {
        return fallback;
    }
    return (int)value;
}

static HWND create_parent(int width, int height, HINSTANCE instance)
{
    RECT rect;
    DWORD style = WS_OVERLAPPEDWINDOW;
    rect.left = 0;
    rect.top = 0;
    rect.right = width;
    rect.bottom = height;
    AdjustWindowRectEx(&rect, style, FALSE, 0);
    return CreateWindowExW(0, PARENT_CLASS, L"webnative embedded (C ABI v0)",
                           style, CW_USEDEFAULT, CW_USEDEFAULT,
                           rect.right - rect.left, rect.bottom - rect.top,
                           NULL, NULL, instance, NULL);
}

int main(int argc, char **argv)
{
    int width = 900;
    int height = 640;
    int i;
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    MSG msg;
    RECT client;
    char html_path[MAX_PATH];
    char css_path[MAX_PATH];
    char base_url[MAX_PATH];
    char *html = NULL;
    char *css = NULL;
    long html_len = 0;
    long css_len = 0;
    int rc;
    const char *backend;
    const char *device;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            g_max_frames = parse_int(argv[++i], 0);
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = parse_int(argv[++i], width);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = parse_int(argv[++i], height);
        } else if (strcmp(argv[i], "--input-demo") == 0) {
            g_input_demo = 1;
        } else if (strcmp(argv[i], "--resize-demo") == 0) {
            g_resize_demo = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: embedded.exe [--frames N] [--width W] [--height H] "
                   "[--input-demo] [--resize-demo]\n");
            return 0;
        } else {
            fprintf(stderr, "warning: ignoring argument '%s'\n", argv[i]);
        }
    }

    SetProcessDPIAware();
    printf("embedded: webnative %s ABI %u\n", wn_version(), wn_abi_version());

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = parent_wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = PARENT_CLASS;
    if (!RegisterClassExW(&wc)) {
        fprintf(stderr, "embedded: RegisterClassExW(parent) failed\n");
        return 1;
    }
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = child_wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = CHILD_CLASS;
    if (!RegisterClassExW(&wc)) {
        fprintf(stderr, "embedded: RegisterClassExW(child) failed\n");
        return 1;
    }

    g_parent = create_parent(width, height, instance);
    if (g_parent == NULL) {
        fprintf(stderr, "embedded: CreateWindowExW(parent) failed\n");
        return 1;
    }
    GetClientRect(g_parent, &client);
    g_child = CreateWindowExW(0, CHILD_CLASS, L"", WS_CHILD | WS_VISIBLE,
                              0, 0, client.right, client.bottom,
                              g_parent, NULL, instance, NULL);
    if (g_child == NULL) {
        fprintf(stderr, "embedded: CreateWindowExW(child) failed\n");
        DestroyWindow(g_parent);
        return 1;
    }
    ShowWindow(g_parent, SW_SHOW);
    UpdateWindow(g_parent);
    GetClientRect(g_child, &client);
    printf("embedded: parent hwnd=%p child hwnd=%p client=%ldx%ld\n",
           (void *)g_parent, (void *)g_child, client.right, client.bottom);

    if (demo_resolve_page(html_path, sizeof(html_path), css_path, sizeof(css_path))) {
        html = demo_read_file(html_path, &html_len);
        css = demo_read_file(css_path, &css_len);
        demo_base_dir(html_path, base_url, sizeof(base_url));
        printf("embedded: loaded page %s (%ld bytes html, %ld bytes css)\n",
               html_path, html_len, css_len);
    }
    if (html == NULL) {
        html = (char *)malloc(strlen(DEMO_INLINE_HTML) + 1);
        if (html != NULL) {
            strcpy(html, DEMO_INLINE_HTML);
            html_len = (long)strlen(html);
        }
        base_url[0] = '\0';
        printf("embedded: using inline fixture (%ld bytes html)\n", html_len);
    }
    if (css == NULL) {
        css = (char *)malloc(strlen(DEMO_INLINE_CSS) + 1);
        if (css != NULL) {
            strcpy(css, DEMO_INLINE_CSS);
            css_len = (long)strlen(css);
        }
    }
    if (html == NULL) {
        fprintf(stderr, "embedded: out of memory\n");
        free(css);
        DestroyWindow(g_parent);
        return 1;
    }

    g_t0 = demo_now_ms();
    g_view = wn_view_create(g_child, (int)client.right, (int)client.bottom, WN_FLAG_NONE);
    if (g_view == NULL) {
        fprintf(stderr, "embedded: wn_view_create failed: %s\n",
                wn_last_error() != NULL ? wn_last_error() : "(no detail)");
        free(html);
        free(css);
        DestroyWindow(g_parent);
        return 1;
    }
    {
        wn_status status = wn_view_load(g_view, html, base_url[0] != '\0' ? base_url : NULL, css);
        if (status != WN_OK) {
            fprintf(stderr, "embedded: wn_view_load failed (%d): %s\n", (int)status,
                    wn_last_error() != NULL ? wn_last_error() : "(no detail)");
            wn_view_destroy(g_view);
            free(html);
            free(css);
            DestroyWindow(g_parent);
            return 1;
        }
    }
    printf("embedded: wn_view_load -> WN_OK (html=%ld css=%ld), view=%dx%d\n",
           html_len, css_len, wn_view_render_width(g_view), wn_view_render_height(g_view));
    wn_view_on_event(g_view, on_view_event, NULL);
    SetTimer(g_parent, 1, 16, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    KillTimer(g_parent, 1);
    rc = g_failed ? 1 : 0;
    backend = wn_view_backend(g_view);
    device = wn_view_device(g_view);
    printf("embedded: backend=%s device=%s\n",
           backend != NULL ? backend : "(unknown)",
           device != NULL ? device : "(unknown)");
    printf("embedded: exit=%d frames=%d startup_ms=%.2f frame_avg_ms=%.2f "
           "cpu_frame_avg_ms=%.3f pointer_ups=%d\n",
           rc, g_frames, g_first_frame_ms,
           g_frames > 0 ? g_frame_total_ms / (double)g_frames : 0.0,
           g_frames > 0 ? g_cpu_total_ms / (double)g_frames : 0.0,
           g_pointer_ups);
    fflush(stdout);

    wn_view_destroy(g_view);
    g_view = NULL;
    free(html);
    free(css);
    if (g_parent != NULL) {
        DestroyWindow(g_parent);
        g_parent = NULL;
    }
    return rc;
}
