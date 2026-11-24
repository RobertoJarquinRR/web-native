/*
 * standalone.c - C ABI demo: wn_app_* owns the Win32 window and loop.
 *
 *   standalone.exe [--frames N] [--width W] [--height H] [--resize-demo]
 *
 * With --frames N the demo closes itself after N presented frames, which is
 * what build_abi.ps1 uses for the headless acceptance run.
 */
#include "webnative.h"
#include "wn_app.h"
#include "demo_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wn_app *g_app;
static int g_pointer_ups;

static void on_event(void *userdata, const wn_event *event)
{
    (void)userdata;
    if (event == NULL) {
        return;
    }
    switch (event->type) {
    case WN_EVENT_POINTER_MOVE:
        printf("standalone: POINTER_MOVE (%.0f,%.0f) target=%s\n",
               event->x, event->y,
               event->target_id != NULL ? event->target_id : "(none)");
        break;
    case WN_EVENT_POINTER_DOWN:
        printf("standalone: POINTER_DOWN (%.0f,%.0f) target=%s\n",
               event->x, event->y,
               event->target_id != NULL ? event->target_id : "(none)");
        break;
    case WN_EVENT_POINTER_UP:
        printf("standalone: POINTER_UP (%.0f,%.0f) target=%s\n",
               event->x, event->y,
               event->target_id != NULL ? event->target_id : "(none)");
        if (event->target_id != NULL) {
            ++g_pointer_ups;
            if (strstr(event->target_id, "/div.nav") != NULL) {
                char text_id[512];
                wn_status status = wn_app_set_class(g_app, event->target_id, "selected", 1);
                printf("standalone: set_class(%s, selected) -> %s\n",
                       event->target_id, status == WN_OK ? "WN_OK" : "error");
                snprintf(text_id, sizeof(text_id), "%s", "text[1]");
                status = wn_app_set_text(g_app, text_id, "Texto");
                printf("standalone: set_text(text[1], \"Texto\") -> %s%s%s\n",
                       status == WN_OK ? "WN_OK" : "error",
                       status == WN_OK ? "" : ": ",
                       status == WN_OK ? "" : (wn_last_error() != NULL ? wn_last_error() : "?"));
            }
        }
        break;
    case WN_EVENT_RESIZE:
        printf("standalone: RESIZE %dx%d\n", event->width, event->height);
        break;
    case WN_EVENT_WHEEL:
        printf("standalone: WHEEL (%.0f,%.0f) dy=%.0f target=%s\n",
               event->x, event->y, event->delta_y,
               event->target_id != NULL ? event->target_id : "(none)");
        break;
    case WN_EVENT_KEY_DOWN:
        printf("standalone: KEY_DOWN key=%d\n", (int)event->key);
        break;
    case WN_EVENT_CLOSE:
        printf("standalone: CLOSE\n");
        break;
    default:
        break;
    }
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

int main(int argc, char **argv)
{
    int frames = 0;
    int width = 960;
    int height = 640;
    int resize_demo = 0;
    int wheel_demo = 0;
    int i;
    int rc;
    char html_path[MAX_PATH];
    char css_path[MAX_PATH];
    char base_url[MAX_PATH];
    char *html = NULL;
    char *css = NULL;
    long html_len = 0;
    long css_len = 0;
    double t0;
    double t1;
    wn_config config;
    wn_status status;
    wn_app_stats stats;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = parse_int(argv[++i], 0);
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = parse_int(argv[++i], width);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = parse_int(argv[++i], height);
        } else if (strcmp(argv[i], "--resize-demo") == 0) {
            resize_demo = 1;
        } else if (strcmp(argv[i], "--wheel-demo") == 0) {
            wheel_demo = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: standalone.exe [--frames N] [--width W] [--height H] "
                   "[--resize-demo] [--wheel-demo]\n");
            return 0;
        } else {
            fprintf(stderr, "warning: ignoring argument '%s'\n", argv[i]);
        }
    }

    SetProcessDPIAware();
    printf("standalone: webnative %s ABI %u\n", wn_version(), wn_abi_version());

    if (demo_resolve_page(html_path, sizeof(html_path), css_path, sizeof(css_path))) {
        html = demo_read_file(html_path, &html_len);
        css = demo_read_file(css_path, &css_len);
        demo_base_dir(html_path, base_url, sizeof(base_url));
        printf("standalone: loaded page %s (%ld bytes html, %ld bytes css)\n",
               html_path, html_len, css_len);
    }
    if (html == NULL) {
        html = (char *)malloc(strlen(DEMO_INLINE_HTML) + 1);
        if (html != NULL) {
            strcpy(html, DEMO_INLINE_HTML);
            html_len = (long)strlen(html);
        }
        base_url[0] = '\0';
        printf("standalone: using inline fixture (%ld bytes html)\n", html_len);
    }
    if (css == NULL) {
        css = (char *)malloc(strlen(DEMO_INLINE_CSS) + 1);
        if (css != NULL) {
            strcpy(css, DEMO_INLINE_CSS);
            css_len = (long)strlen(css);
        }
    }
    if (html == NULL) {
        fprintf(stderr, "standalone: out of memory\n");
        free(css);
        return 1;
    }

    memset(&config, 0, sizeof(config));
    config.title = "webnative standalone (C ABI v0)";
    config.width = width;
    config.height = height;
    config.dpi_scale = 0.0f;
    config.flags = WN_FLAG_NONE;

    t0 = demo_now_ms();
    g_app = wn_app_create(&config);
    if (g_app == NULL) {
        fprintf(stderr, "standalone: wn_app_create failed: %s\n",
                wn_last_error() != NULL ? wn_last_error() : "(no detail)");
        free(html);
        free(css);
        return 1;
    }
    status = wn_app_load(g_app, html, base_url[0] != '\0' ? base_url : NULL, css);
    if (status != WN_OK) {
        fprintf(stderr, "standalone: wn_app_load failed (%d): %s\n", (int)status,
                wn_last_error() != NULL ? wn_last_error() : "(no detail)");
        wn_app_destroy(g_app);
        free(html);
        free(css);
        return 1;
    }
    printf("standalone: wn_app_load -> WN_OK (html=%ld css=%ld)\n", html_len, css_len);
    wn_app_on_event(g_app, on_event, g_app);
    if (frames > 0) {
        wn_app_set_max_frames(g_app, frames);
    }
    if (resize_demo) {
        wn_app_set_resize_demo(g_app, 1);
    }
    if (wheel_demo) {
        /* Queue a real WM_MOUSEWHEEL before the loop starts: it exercises
         * host_win32.c -> wn_app -> wn_view_event -> wn_core_wheel. */
        HWND hwnd = FindWindowW(NULL, L"webnative standalone (C ABI v0)");
        if (hwnd != NULL) {
            RECT client;
            POINT point;
            GetClientRect(hwnd, &client);
            point.x = (client.right - client.left) / 2;
            point.y = (client.bottom - client.top) / 2;
            ClientToScreen(hwnd, &point);
            PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(-WHEEL_DELTA)),
                         MAKELPARAM(point.x, point.y));
            printf("standalone: queued WM_MOUSEWHEEL (scroll down) at (%ld,%ld)\n",
                   point.x, point.y);
        } else {
            printf("standalone: wheel-demo could not find the host window\n");
        }
    }

    rc = wn_app_run(g_app);
    t1 = demo_now_ms();
    memset(&stats, 0, sizeof(stats));
    wn_app_get_stats(g_app, &stats);
    printf("standalone: backend=%s device=%s\n",
           stats.backend[0] != '\0' ? stats.backend : "(unknown)",
           stats.device[0] != '\0' ? stats.device : "(unknown)");
    printf("standalone: exit=%d frames=%d startup_ms=%.2f frame_avg_ms=%.2f "
           "cpu_frame_avg_ms=%.3f last_layout_ms=%.3f wall_ms=%.2f\n",
           rc, stats.frames, stats.startup_ms,
           stats.frames > 0 ? stats.frame_total_ms / (double)stats.frames : 0.0,
           stats.frames > 0 ? stats.cpu_frame_total_ms / (double)stats.frames : 0.0,
           stats.last_layout_ms, t1 - t0);
    printf("standalone: pointer_ups=%d\n", g_pointer_ups);
    fflush(stdout);

    wn_app_destroy(g_app);
    g_app = NULL;
    free(html);
    free(css);
    return rc;
}
