/*
 * runtime_r2.c - reference driver for the R2 core: CLI, layout report,
 * offscreen renders and the standalone Win32 window.
 *
 * Usage:
 *   runtime_r2.exe --render 960 640 out.png
 *   runtime_r2.exe --render 960x640 out.png [--hover ID] [--press ID] [--selected ID]
 *   runtime_r2.exe --states out_prefix [--node ID] [--size 960x640]
 *   runtime_r2.exe --report layout_report.json [--sizes 960x640,720x640,560x640]
 *   runtime_r2.exe --dynamic out_prefix        (11-stage Dynamic UI v0 demo)
 *   runtime_r2.exe --input out_prefix          (10-stage text input v1 demo)
 *   runtime_r2.exe --frames 30 [--resize-demo] [--input-demo]
 *   runtime_r2.exe                      (window; Esc blurs an input, else closes)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wchar.h>

#include <wincodec.h>
#if WN_WGPU
#include <wgpu.h>
#endif

#include "document/wn_document_internal.h"
#include "wn_core.h"
#include "wn_internal.h"
#include "wn_view.h"

#define ARRAY_LEN(a) ((size_t)(sizeof(a) / sizeof((a)[0])))

/* --------------------------------------------------------------- utilities */

static double now_ms(void)
{
    LARGE_INTEGER freq, tick;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tick);
    return (double)tick.QuadPart * 1000.0 / (double)freq.QuadPart;
}

#if WN_WGPU
static void fprint_view(FILE *stream, WGPUStringView view)
{
    if (view.data == NULL || view.length == 0) {
        fputs("(none)", stream);
    } else if (view.length == WGPU_STRLEN) {
        fputs(view.data, stream);
    } else {
        int n = view.length > 4096 ? 4096 : (int)view.length;
        fprintf(stream, "%.*s", n, view.data);
    }
}

static void print_view(const char *label, WGPUStringView view)
{
    fputs(label, stdout);
    fprint_view(stdout, view);
}

/* --------------------------------------------------------------- callbacks */

static void on_log(WGPULogLevel level, WGPUStringView message, void *userdata)
{
    (void)userdata;
    if (level <= WGPULogLevel_Warn) {
        fprintf(stderr, "[wgpu %d] ", (int)level);
        fprint_view(stderr, message);
        fputc('\n', stderr);
        fflush(stderr);
    }
}
#endif /* WN_WGPU */

/* ------------------------------------------------------- core bridge (R2) */

/*
 * The layout, state machine, hit testing and display-list generation live in
 * wn_core.c. runtime_r2.c keeps only the CLI, reporting and Win32 code, so the
 * reference executable and the ABI layer share one engine.
 */

typedef wn_rect Rect;
typedef wn_vertex Vertex;
typedef wn_glyph_vertex GlyphVertex;

static wn_core *g_core;
static const wn_document *g_document;

#define g_rects (wn_core_rects(g_core, NULL))
#define g_hover (wn_core_hover(g_core))
#define g_press (wn_core_press(g_core))
#define g_selected (wn_core_selected(g_core))
#define g_vertices (wn_core_display_list(g_core)->vertices)
#define g_glyph_vertices (wn_core_display_list(g_core)->glyph_vertices)
#define g_image_vertices (wn_core_display_list(g_core)->image_vertices)

static int g_vertex_count = 0;
static int g_glyph_vertex_count = 0;
static int g_image_vertex_count = 0;
static int g_last_image_cache = -1;
static int g_wheel_active = 0;
static float g_wheel_x = 0.0f;
static float g_wheel_y = 0.0f;
static float g_wheel_dy = 0.0f;

static void layout_page(float viewport_w, float viewport_h)
{
    wn_core_resize(g_core, (int)viewport_w, (int)viewport_h);
    wn_core_relayout(g_core);
}

static void build_geometry(float viewport_w, float viewport_h)
{
    const wn_display_list *list;
    (void)viewport_w;
    (void)viewport_h;
    wn_core_build_display_list(g_core);
    list = wn_core_display_list(g_core);
    g_vertex_count = list != NULL ? list->vertex_count : 0;
    g_glyph_vertex_count = list != NULL ? list->glyph_vertex_count : 0;
    g_image_vertex_count = list != NULL ? list->image_vertex_count : 0;
}

static void set_state_from_arg(int state, int node)
{
    if (node < 0) {
        wn_core_set_state(g_core, 0, state, 0);
    } else {
        wn_core_set_state(g_core, node, state, 1);
    }
}

static int render_offscreen_vt(int width, int height, const wchar_t *path,
                               double *layout_ms_out, double *frame_ms_out,
                               int *vertices_out, int *glyphs_out, int *images_out)
{
    wn_view *view;
    wn_view_stats stats;
    char narrow[MAX_PATH];
    double t0, t1;
    int wheel_x = 0;
    int wheel_y = 0;
    int wheel_dy = 0;
    int wheel_active = g_wheel_active;

    if (width <= 0 || height <= 0) {
        fprintf(stderr, "error: bad render size %dx%d\n", width, height);
        return 1;
    }
    if (wheel_active) {
        wheel_x = (int)g_wheel_x;
        wheel_y = (int)g_wheel_y;
        wheel_dy = (int)g_wheel_dy;
        g_wheel_active = 0;
    }
    t0 = now_ms();
    view = wn_view_create_offscreen_with_core(g_core, width, height);
    if (view == NULL) {
        fprintf(stderr, "error: %s\n", wn_last_error());
        return 1;
    }
    if (wn_view_load(view, "", NULL, NULL) != WN_OK) {
        fprintf(stderr, "error: %s\n", wn_last_error());
        wn_view_destroy(view);
        return 1;
    }
    if (g_hover >= 0) wn_view_set_state(view, g_hover, WN_STATE_HOVER, 1);
    if (g_press >= 0) wn_view_set_state(view, g_press, WN_STATE_PRESS, 1);
    if (g_selected >= 0) wn_view_set_state(view, g_selected, WN_STATE_SELECTED, 1);
    if (wheel_active) {
        /* Hit testing needs the first layout, so lay out once before wheeling. */
        int container;
        if (wn_view_render(view) != 0) {
            fprintf(stderr, "error: %s\n", wn_last_error());
            wn_view_destroy(view);
            return 1;
        }
        container = wn_view_wheel_node(view, (float)wheel_x, (float)wheel_y, 0.0f,
                                       (float)wheel_dy);
        if (container >= 0) {
            printf("wheel: (%d,%d) delta_y=%d -> node=%d %s scroll=%.1f/%.1f\n", wheel_x,
                   wheel_y, wheel_dy, container, g_document->names[container],
                   wn_core_scroll_offset(g_core, container),
                   wn_core_scroll_max(g_core, container));
        } else {
            printf("wheel: (%d,%d) delta_y=%d -> no scrollable container\n", wheel_x,
                   wheel_y, wheel_dy);
        }
        fflush(stdout);
    }
    if (wn_view_render(view) != 0) {
        fprintf(stderr, "error: %s\n", wn_last_error());
        wn_view_destroy(view);
        return 1;
    }
    t1 = now_ms();
    if (frame_ms_out != NULL) *frame_ms_out = t1 - t0;

    memset(&stats, 0, sizeof(stats));
    if (wn_view_get_stats(view, &stats) == 0) {
        if (layout_ms_out != NULL) *layout_ms_out = stats.last_layout_ms;
    } else if (layout_ms_out != NULL) {
        *layout_ms_out = 0.0;
    }
    wn_view_display_counts(view, vertices_out, glyphs_out, images_out);

    if (WideCharToMultiByte(CP_ACP, 0, path, -1, narrow, MAX_PATH, NULL, NULL) <= 0) {
        wn_view_destroy(view);
        return 1;
    }
    if (wn_view_readback(view, narrow) != WN_OK) {
        fprintf(stderr, "error: %s\n", wn_last_error());
        wn_view_destroy(view);
        return 1;
    }
    printf("adapter: backend=%s\n",
           wn_view_backend(view) != NULL ? wn_view_backend(view) : "(unknown)");
    printf("adapter: device=%s\n",
           wn_view_device(view) != NULL ? wn_view_device(view) : "(unknown)");
    wn_view_destroy(view);
    return 0;
}

/* Persistent offscreen view for the multi-stage suites: the renderer (and its
 * image cache) survives across stages. */
static wn_view *g_core_view = NULL;
static int g_core_view_w = 0;
static int g_core_view_h = 0;

static int core_view_open(int width, int height)
{
    if (g_core_view != NULL && g_core_view_w == width && g_core_view_h == height) {
        return 0;
    }
    if (g_core_view != NULL) {
        wn_view_destroy(g_core_view);
        g_core_view = NULL;
    }
    g_core_view = wn_view_create_offscreen_with_core(g_core, width, height);
    if (g_core_view == NULL) {
        fprintf(stderr, "error: %s\n", wn_last_error());
        return 1;
    }
    if (wn_view_load(g_core_view, "", NULL, NULL) != WN_OK) {
        fprintf(stderr, "error: %s\n", wn_last_error());
        wn_view_destroy(g_core_view);
        g_core_view = NULL;
        return 1;
    }
    g_core_view_w = width;
    g_core_view_h = height;
    return 0;
}

static void core_view_close(void)
{
    if (g_core_view != NULL) {
        wn_view_destroy(g_core_view);
        g_core_view = NULL;
        g_core_view_w = 0;
        g_core_view_h = 0;
    }
}

/* Render the current g_core state to a PNG through the vtable, without touching
 * hover/press/selected (the caller already set them). */
static int render_core_png(int width, int height, const wchar_t *path,
                           double *layout_ms_out, double *frame_ms_out)
{
    wn_view_stats stats;
    char narrow[MAX_PATH];

    if (width <= 0 || height <= 0) {
        return 1;
    }
    if (core_view_open(width, height) != 0) {
        return 1;
    }
    if (wn_view_render(g_core_view) != 0) {
        fprintf(stderr, "error: %s\n", wn_last_error());
        return 1;
    }
    memset(&stats, 0, sizeof(stats));
    if (wn_view_get_stats(g_core_view, &stats) == 0 && layout_ms_out != NULL) {
        *layout_ms_out = stats.last_layout_ms;
    }
    if (frame_ms_out != NULL) {
        *frame_ms_out = stats.frame_total_ms;
    }
    if (WideCharToMultiByte(CP_ACP, 0, path, -1, narrow, MAX_PATH, NULL, NULL) <= 0 ||
        wn_view_readback(g_core_view, narrow) != WN_OK) {
        fprintf(stderr, "error: %s\n", wn_last_error());
        return 1;
    }
    wn_view_display_counts(g_core_view, &g_vertex_count, &g_glyph_vertex_count,
                           &g_image_vertex_count);
    g_last_image_cache = wn_view_image_cache_count(g_core_view);
    return 0;
}

static void json_escape(FILE *f, const char *text)
{
    fputc('"', f);
    for (const char *p = text; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc(c, f);
        } else if (c < 0x20) {
            fprintf(f, "\\u%04x", c);
        } else {
            fputc(c, f);
        }
    }
    fputc('"', f);
}

static const char *kind_name(int kind)
{
    switch (kind) {
    case R2_NODE_FLEX: return "flex";
    case R2_NODE_TEXT: return "text";
    case R2_NODE_GRID: return "grid";
    case R2_NODE_IMAGE: return "image";
    default: return "block";
    }
}

static int parse_size(const char *text, int *w, int *h)
{
    int a = 0, b = 0;
    if (sscanf(text, "%dx%d", &a, &b) != 2) {
        return 1;
    }
    if (a <= 0 || b <= 0 || a > 16384 || b > 16384) {
        return 1;
    }
    *w = a;
    *h = b;
    return 0;
}

static int write_layout_report(const char *path, const int *widths, const int *heights,
                               int count)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "error: cannot open report %s\n", path);
        return 1;
    }
    fprintf(f, "{\n  \"generated_by\": \"runtime_r2.c\",\n  \"node_count\": %d,\n", g_document->node_count);
    fprintf(f, "  \"sizes\": [\n");
    for (int s = 0; s < count; ++s) {
        const int w = widths[s], h = heights[s];
        const int iterations = 100;
        double best = 1e9, total = 0.0;
        for (int it = 0; it < iterations; ++it) {
            double t0 = now_ms();
            layout_page((float)w, (float)h);
            double dt = now_ms() - t0;
            total += dt;
            if (dt < best) best = dt;
        }
        layout_page((float)w, (float)h);
        fprintf(f, "    {\n");
        fprintf(f, "      \"width\": %d,\n      \"height\": %d,\n", w, h);
        fprintf(f, "      \"layout_ms_best\": %.4f,\n      \"layout_ms_avg\": %.4f,\n",
                best, total / (double)iterations);
        fprintf(f, "      \"nodes\": [\n");
        for (int i = 0; i < g_document->node_count; ++i) {
            const Rect *r = &g_rects[i];
            fprintf(f, "        {\"id\": %d, \"name\": ", i);
            json_escape(f, g_document->names[i]);
            fprintf(f, ", \"kind\": \"%s\", \"x\": %.2f, \"y\": %.2f, \"w\": %.2f, \"h\": %.2f}%s\n",
                    kind_name(g_document->nodes[i].kind), r->x, r->y, r->w, r->h,
                    i + 1 < g_document->node_count ? "," : "");
        }
        fprintf(f, "      ]\n    }%s\n", s + 1 < count ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    printf("report: wrote %s (%d size(s), %d nodes each)\n", path, count, g_document->node_count);
    return 0;
}

static int name_is(const char *name, const char *exact)
{
    return strcmp(name, exact) == 0;
}

static int is_card_node(const char *name)
{
    const char *tag = "/div.cards/div.card";
    const char *at = strstr(name, tag);
    if (at == NULL) {
        return 0;
    }
    const char *tail = at + strlen(tag);
    if (*tail == '[') {
        ++tail;
        while (*tail >= '0' && *tail <= '9') {
            ++tail;
        }
        if (*tail == ']') {
            ++tail;
        }
    }
    return *tail == '\0';
}

static void print_layout_summary(float w, float h)
{
    int sidebar = -1, content = -1, header = -1, cards[8], card_count = 0;
    for (int i = 0; i < g_document->node_count; ++i) {
        const char *name = g_document->names[i];
        if (name_is(name, "body.root/div.sidebar")) sidebar = i;
        else if (name_is(name, "body.root/div.content")) content = i;
        else if (name_is(name, "body.root/div.content/div.header")) header = i;
        else if (is_card_node(name) && card_count < 8) {
            cards[card_count++] = i;
        }
    }
    printf("layout %gx%g:", w, h);
    if (sidebar >= 0) printf(" sidebar=[%.1f,%.1f,%.1f,%.1f]",
                             g_rects[sidebar].x, g_rects[sidebar].y,
                             g_rects[sidebar].w, g_rects[sidebar].h);
    if (content >= 0) printf(" content=[%.1f,%.1f,%.1f,%.1f]",
                             g_rects[content].x, g_rects[content].y,
                             g_rects[content].w, g_rects[content].h);
    if (header >= 0) printf(" header_w=%.1f", g_rects[header].w);
    printf(" cards_y=");
    for (int i = 0; i < card_count; ++i) {
        printf("%s%.1f", i ? "," : "", g_rects[cards[i]].y);
    }
    printf(" cards_w=");
    for (int i = 0; i < card_count; ++i) {
        printf("%s%.1f", i ? "," : "", g_rects[cards[i]].w);
    }
    printf("\n");
}

/* One line per scroll container, machine-readable for the pilot verifier. */
static void print_scroll_summary(void)
{
    for (int i = 0; i < g_document->node_count; ++i) {
        if (g_document->nodes[i].overflow != R2_OVERFLOW_SCROLL) {
            continue;
        }
        printf("scroll: node=%d name=%s rect=[%.1f,%.1f,%.1f,%.1f] "
               "offset=%.2f max=%.2f\n",
               i, g_document->names[i], g_rects[i].x, g_rects[i].y,
               g_rects[i].w, g_rects[i].h,
               wn_core_scroll_offset(g_core, i),
               wn_core_scroll_max(g_core, i));
    }
}

/* ------------------------------------------------------------------ states */

static int first_interactive_node(void)
{
    for (int i = 0; i < g_document->node_count; ++i) {
        if (g_document->nodes[i].action != ACTION_NONE) {
            return i;
        }
    }
    return -1;
}

static int first_input_node(void)
{
    for (int i = 0; i < g_document->node_count; ++i) {
        if (g_document->nodes[i].kind == R2_NODE_INPUT) {
            return i;
        }
    }
    return -1;
}

static int run_states(const char *prefix, int node, int width, int height)
{
    char path[MAX_PATH];
    double layout_ms = 0.0, frame_ms = 0.0;
    const char *suffixes[4] = {"normal", "hover", "press", "selected"};

    if (node < 0 || node >= g_document->node_count || g_document->nodes[node].action == ACTION_NONE) {
        fprintf(stderr, "error: node %d is not interactive\n", node);
        return 1;
    }

    wn_core_set_state(g_core, 0, WN_STATE_HOVER, 0);
    wn_core_set_state(g_core, 0, WN_STATE_PRESS, 0);
    wn_core_set_state(g_core, 0, WN_STATE_SELECTED, 0);
    for (int state = 0; state < 4; ++state) {
        wn_core_set_state(g_core, node, WN_STATE_HOVER, state == 1);
        wn_core_set_state(g_core, node, WN_STATE_PRESS, state == 2);
        wn_core_set_state(g_core, node, WN_STATE_SELECTED, state == 3);
        snprintf(path, sizeof(path), "%s_%s.png", prefix, suffixes[state]);
        wchar_t wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
        if (render_core_png(width, height, wide, &layout_ms, &frame_ms) != 0) {
            return 1;
        }
        printf("state %-8s node=%d %-40s layout=%.3fms frame=%.3fms -> %s\n",
               suffixes[state], node, g_document->names[node], layout_ms, frame_ms, path);
    }

    /* Small machine-readable state report for verify.py. */
    snprintf(path, sizeof(path), "%s_states.json", prefix);
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        const Rect *r = &g_rects[node];
        fprintf(f, "{\n  \"node\": %d,\n  \"name\": ", node);
        json_escape(f, g_document->names[node]);
        fprintf(f, ",\n  \"rect\": {\"x\": %.2f, \"y\": %.2f, \"w\": %.2f, \"h\": %.2f},\n",
                r->x, r->y, r->w, r->h);
        fprintf(f, "  \"colors\": {\n");
        fprintf(f, "    \"normal\": \"#%06X\",\n", g_document->nodes[node].background & 0xFFFFFFu);
        fprintf(f, "    \"hover\": \"#%06X\",\n", g_document->nodes[node].hover_bg & 0xFFFFFFu);
        fprintf(f, "    \"press\": \"#%06X\",\n", g_document->nodes[node].press_bg & 0xFFFFFFu);
        fprintf(f, "    \"selected\": \"#%06X\"\n", g_document->nodes[node].selected_bg & 0xFFFFFFu);
        fprintf(f, "  },\n  \"pngs\": [");
        for (int state = 0; state < 4; ++state) {
            snprintf(path, sizeof(path), "%s_%s.png", prefix, suffixes[state]);
            fprintf(f, "%s\n    ", state ? "," : "");
            json_escape(f, path);
        }
        fprintf(f, "\n  ]\n}\n");
        fclose(f);
        printf("states: wrote %s_states.json\n", prefix);
    }

    core_view_close();
    return 0;
}

/* ------------------------------------------------------------------ window */

#define WINDOW_CLASS L"WebNativeR2Window"
#define WINDOW_TITLE L"webnative R2 core - runtime layout + wgpu-native"

static volatile LONG g_running = 1;
static int g_window_width = 960;
static int g_window_height = 640;
static int g_need_layout = 1;
static int g_need_geometry = 1;
static int g_need_configure = 1;
static int g_max_frames = 0;
static int g_resize_demo = 0;
static int g_input_demo = 0;
static int g_frame_index = 0;
static double g_last_layout_ms = 0.0;
static double g_cpu_frame_total = 0.0;
static int g_demo_stage = 0;

static int find_node_contains(const char *fragment)
{
    for (int i = 0; i < g_document->node_count; ++i) {
        if (strstr(g_document->names[i], fragment)) {
            return i;
        }
    }
    return -1;
}

static void log_reflow(const char *why)
{
    double t0 = now_ms();
    layout_page((float)g_window_width, (float)g_window_height);
    g_last_layout_ms = now_ms() - t0;
    build_geometry((float)g_window_width, (float)g_window_height);
    g_need_layout = 0;
    g_need_geometry = 0;
    int c0 = find_node_contains("/div.card");
    int c2 = find_node_contains("/div.card[2]");
    printf("%s: %dx%d layout %.3fms", why, g_window_width, g_window_height, g_last_layout_ms);
    if (c0 >= 0 && c2 >= 0) {
        printf(" card0_y=%.1f card2_y=%.1f", g_rects[c0].y, g_rects[c2].y);
    }
    printf(" vertices=%d glyph_vertices=%d image_vertices=%d\n",
           g_vertex_count, g_glyph_vertex_count, g_image_vertex_count);
    fflush(stdout);
}

static void set_client_size(HWND hwnd, int client_w, int client_h)
{
    RECT rect = {0, 0, client_w, client_h};
    DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    DWORD exstyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&rect, style, FALSE, exstyle);
    SetWindowPos(hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ------------------------------------------------ window text input (v4) */

static uint32_t input_modifiers(void)
{
    uint32_t mods = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= WN_KEY_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= 0x02u;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= 0x04u;
    return mods;
}

static void r2_clipboard_set(const char *text)
{
    int length;
    HGLOBAL memory;
    wchar_t *wide;
    if (text == NULL || !OpenClipboard(NULL)) {
        return;
    }
    EmptyClipboard();
    length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (length > 0) {
        memory = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)length * sizeof(wchar_t));
        if (memory != NULL) {
            wide = (wchar_t *)GlobalLock(memory);
            if (wide != NULL) {
                MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, length);
                GlobalUnlock(memory);
                if (SetClipboardData(CF_UNICODETEXT, memory) == NULL) {
                    GlobalFree(memory);
                }
            } else {
                GlobalFree(memory);
            }
        }
    }
    CloseClipboard();
}

static int r2_clipboard_get(char *out, int capacity)
{
    int copied = 0;
    if (out == NULL || capacity <= 0) {
        return 0;
    }
    out[0] = '\0';
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(NULL)) {
        return 0;
    }
    {
        HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        if (handle != NULL) {
            const wchar_t *wide = (const wchar_t *)GlobalLock(handle);
            if (wide != NULL) {
                int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
                if (bytes > 1) {
                    char *utf8 = (char *)malloc((size_t)bytes);
                    if (utf8 != NULL) {
                        if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, bytes,
                                                NULL, NULL) > 0) {
                            copied = (int)strlen(utf8);
                            if (copied >= capacity) {
                                copied = capacity - 1;
                            }
                            memcpy(out, utf8, (size_t)copied);
                            out[copied] = '\0';
                        }
                        free(utf8);
                    }
                }
                GlobalUnlock(handle);
            }
        }
    }
    CloseClipboard();
    return copied;
}

static void log_input_value(const char *why, int node)
{
    const char *value = wn_core_input_value(g_core, node);
    printf("input: %s node=%d value=\"%s\"\n", why, node, value != NULL ? value : "");
    fflush(stdout);
}

/* Returns 1 when the key was consumed. */
static int window_input_key(int key)
{
    char buffer[256];
    uint32_t mods = input_modifiers();
    int node = wn_core_input_focused(g_core);
    int value_changed = 0;
    int visual_changed = 0;
    if (node < 0) {
        return 0;
    }
    if (mods & 0x02u) {
        if (key == WN_KEY_A) {
            wn_core_input_select_all(g_core, &visual_changed);
        } else if (key == WN_KEY_C) {
            int copied = wn_core_input_copy(g_core, buffer, (int)sizeof(buffer));
            if (copied > 0) r2_clipboard_set(buffer);
            log_input_value("copy", node);
        } else if (key == WN_KEY_X) {
            int copied = wn_core_input_cut(g_core, buffer, (int)sizeof(buffer),
                                           &value_changed, &visual_changed);
            if (copied > 0) r2_clipboard_set(buffer);
            if (value_changed) log_input_value("cut", node);
        } else if (key == WN_KEY_V) {
            if (r2_clipboard_get(buffer, (int)sizeof(buffer)) > 0) {
                wn_core_input_paste(g_core, buffer, &value_changed, &visual_changed);
            }
            if (value_changed) log_input_value("paste", node);
        } else {
            return 0;
        }
    } else {
        if (wn_core_input_key(g_core, key, mods, &value_changed, &visual_changed) < 0) {
            return 0;
        }
        if (value_changed) log_input_value("key", node);
    }
    if (visual_changed || value_changed) {
        g_need_geometry = 1;
    }
    return 1;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CLOSE:
        g_running = 0;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        g_window_width = LOWORD(lparam);
        g_window_height = HIWORD(lparam);
        g_need_layout = 1;
        g_need_configure = 1;
        return 0;
    case WM_KEYDOWN:
        if (window_input_key((int)wparam)) {
            return 0;
        }
        if (wparam == VK_ESCAPE && wn_core_input_focused(g_core) < 0) {
            g_running = 0;
            return 0;
        }
        return 0;
    case WM_CHAR: {
        int value_changed = 0;
        int visual_changed = 0;
        if (wn_core_input_char(g_core, (uint32_t)wparam,
                               &value_changed, &visual_changed) >= 0) {
            if (value_changed) {
                log_input_value("char", wn_core_input_focused(g_core));
            }
            if (value_changed || visual_changed) {
                g_need_geometry = 1;
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = (int)(short)LOWORD(lparam);
        int y = (int)(short)HIWORD(lparam);
        int before = g_hover;
        int hit = wn_core_pointer_move(g_core, (float)x, (float)y);
        if (hit != before) {
            g_need_geometry = 1;
        }
        TRACKMOUSEEVENT track;
        memset(&track, 0, sizeof(track));
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hover != -1) {
            wn_core_pointer_leave(g_core);
            g_need_geometry = 1;
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT point;
        int notches;
        int container;
        point.x = (int)(short)LOWORD(lparam);
        point.y = (int)(short)HIWORD(lparam);
        ScreenToClient(hwnd, &point);
        notches = (int)GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        if (notches == 0) {
            notches = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 1 : -1;
        }
        container = wn_core_wheel(g_core, (float)point.x, (float)point.y,
                                  0.0f, (float)(-notches * 120));
        if (container >= 0) {
            g_need_geometry = 1;
            printf("wheel: (%ld,%ld) notches=%d -> node=%d %s scroll=%.1f/%.1f\n",
                   point.x, point.y, notches, container, g_document->names[container],
                   wn_core_scroll_offset(g_core, container),
                   wn_core_scroll_max(g_core, container));
            fflush(stdout);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = (int)(short)LOWORD(lparam);
        int y = (int)(short)HIWORD(lparam);
        wn_core_pointer_down(g_core, (float)x, (float)y);
        SetCapture(hwnd);
        g_need_geometry = 1;
        return 0;
    }
    case WM_LBUTTONUP: {
        int x = (int)(short)LOWORD(lparam);
        int y = (int)(short)HIWORD(lparam);
        int press = g_press;
        int hit = wn_core_pointer_up(g_core, (float)x, (float)y);
        if (press >= 0 && hit == press && g_document->nodes[press].action == ACTION_SELECT) {
            printf("click: node=%d %s -> selected\n", press, g_document->names[press]);
            fflush(stdout);
        }
        ReleaseCapture();
        g_need_geometry = 1;
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static int run_window(void)
{
    wn_view *view = NULL;
    HWND hwnd = NULL;
    int exit_code = 1;
    int frames = 0;
    double t_start, t_ready;

    {
        HINSTANCE hinst = GetModuleHandleW(NULL);
        WNDCLASSEXW wc;
        DWORD style = WS_OVERLAPPEDWINDOW;
        RECT rect = {0, 0, g_window_width, g_window_height};

        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = WINDOW_CLASS;
        if (!RegisterClassExW(&wc)) {
            fprintf(stderr, "error: RegisterClassExW failed\n");
            return 1;
        }
        AdjustWindowRect(&rect, style, FALSE);
        hwnd = CreateWindowExW(0, WINDOW_CLASS, WINDOW_TITLE, style,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               rect.right - rect.left, rect.bottom - rect.top,
                               NULL, NULL, hinst, NULL);
        if (hwnd == NULL) {
            fprintf(stderr, "error: CreateWindowExW failed\n");
            return 1;
        }
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    t_start = now_ms();

    view = wn_view_create_with_core(hwnd, g_core, g_window_width, g_window_height);
    if (view == NULL) {
        fprintf(stderr, "error: %s\n", wn_last_error());
        goto cleanup;
    }
    if (wn_view_load(view, "", NULL, NULL) != WN_OK) {
        fprintf(stderr, "error: %s\n", wn_last_error());
        goto cleanup;
    }
    printf("adapter: backend=%s\n",
           wn_view_backend(view) != NULL ? wn_view_backend(view) : "(unknown)");
    printf("adapter: device=%s\n",
           wn_view_device(view) != NULL ? wn_view_device(view) : "(unknown)");
    fflush(stdout);

    t_ready = now_ms();

    while (g_running) {
        MSG msg;

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = 0;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) {
            break;
        }
        if (g_window_width <= 0 || g_window_height <= 0 || IsIconic(hwnd)) {
            Sleep(10);
            continue;
        }

        if (g_need_layout) {
            log_reflow("WM_SIZE/layout");
        } else if (g_need_geometry) {
            build_geometry((float)g_window_width, (float)g_window_height);
            g_need_geometry = 0;
        }
        if (g_need_configure) {
            wn_view_resize(view, g_window_width, g_window_height, 0.0f);
            g_need_configure = 0;
            printf("surface: reconfigured %dx%d\n", g_window_width, g_window_height);
            fflush(stdout);
        }
        {
            double t_cpu_frame = now_ms();
            if (wn_view_render(view) != 0) {
                fprintf(stderr, "error: %s\n", wn_last_error());
                break;
            }
            g_cpu_frame_total += now_ms() - t_cpu_frame;
            wn_view_display_counts(view, &g_vertex_count, &g_glyph_vertex_count,
                                   &g_image_vertex_count);
            g_last_image_cache = wn_view_image_cache_count(view);
        }

        ++frames;
        ++g_frame_index;

        if (g_resize_demo) {
            if (g_demo_stage == 0 && g_frame_index >= 5) {
                g_demo_stage = 1;
                printf("resize-demo: frame %d -> client 560x640\n", g_frame_index);
                fflush(stdout);
                set_client_size(hwnd, 560, 640);
            } else if (g_demo_stage == 1 && g_frame_index >= 15) {
                g_demo_stage = 2;
                printf("resize-demo: frame %d -> client 960x640\n", g_frame_index);
                fflush(stdout);
                set_client_size(hwnd, 960, 640);
            }
        }

        /* Real Win32 input path: synthetic WM_MOUSEMOVE / LBUTTONDOWN / UP
         * are dispatched through the window procedure and hit testing. When
         * the fixture has no action node, the demo targets the first text
         * input and adds WM_CHAR / VK_BACK. */
        if (g_input_demo) {
            int node = first_interactive_node();
            int input_mode = 0;
            if (node < 0) {
                node = first_input_node();
                input_mode = node >= 0;
            }
            if (node >= 0) {
                int cx = (int)(g_rects[node].x + g_rects[node].w * 0.5f);
                int cy = (int)(g_rects[node].y + g_rects[node].h * 0.5f);
                LPARAM point = MAKELPARAM(cx, cy);
                if (g_frame_index == 8) {
                    SendMessageW(hwnd, WM_MOUSEMOVE, 0, point);
                    printf("input-demo: WM_MOUSEMOVE (%d,%d) -> hover=%d %s\n",
                           cx, cy, g_hover, g_hover >= 0 ? g_document->names[g_hover] : "(none)");
                    fflush(stdout);
                } else if (g_frame_index == 9) {
                    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
                    printf("input-demo: WM_LBUTTONDOWN -> press=%d %s\n",
                           g_press, g_press >= 0 ? g_document->names[g_press] : "(none)");
                    fflush(stdout);
                } else if (g_frame_index == 10) {
                    SendMessageW(hwnd, WM_LBUTTONUP, 0, point);
                    printf("input-demo: WM_LBUTTONUP -> selected=%d %s\n",
                           g_selected, g_selected >= 0 ? g_document->names[g_selected] : "(none)");
                    fflush(stdout);
                } else if (input_mode && g_frame_index == 12) {
                    SendMessageW(hwnd, WM_CHAR, (WPARAM)'a', 0);
                    printf("input-demo: WM_CHAR 'a'\n");
                    fflush(stdout);
                } else if (input_mode && g_frame_index == 13) {
                    SendMessageW(hwnd, WM_CHAR, (WPARAM)'b', 0);
                    printf("input-demo: WM_CHAR 'b'\n");
                    fflush(stdout);
                } else if (input_mode && g_frame_index == 14) {
                    SendMessageW(hwnd, WM_KEYDOWN, VK_BACK, 0);
                    printf("input-demo: WM_KEYDOWN VK_BACK\n");
                    fflush(stdout);
                } else if (input_mode && g_frame_index == 15) {
                    const char *value = wn_core_input_value(g_core, node);
                    printf("input-demo: value=\"%s\" focused=%d\n",
                           value != NULL ? value : "(null)",
                           wn_core_input_focused(g_core) == node ? 1 : 0);
                    fflush(stdout);
                } else if (!input_mode && g_frame_index == 12) {
                    SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(900, 600));
                    printf("input-demo: WM_MOUSEMOVE away -> hover=%d\n", g_hover);
                    fflush(stdout);
                }
            }
        }

        if (g_max_frames > 0 && frames >= g_max_frames) {
            break;
        }
    }

    printf("window: closed after %d frame(s), init %.2f ms, %.2f ms/frame average\n",
           frames, t_ready - t_start,
           frames > 0 ? (now_ms() - t_ready) / (double)frames : 0.0);
    printf("window: last layout %.3f ms at %dx%d, vertices=%d glyph_vertices=%d\n",
           g_last_layout_ms, g_window_width, g_window_height, g_vertex_count,
           g_glyph_vertex_count);
    printf("window: cpu frame %.3f ms average (layout excluded, present/vsync excluded)\n",
           frames > 0 ? g_cpu_frame_total / (double)frames : 0.0);
    fflush(stdout);
    exit_code = 0;

cleanup:
    if (view != NULL) {
        wn_view_destroy(view);
    }
    if (hwnd != NULL) DestroyWindow(hwnd);
    return exit_code;
}

/* ------------------------------------------------------------ dynamic demo */

static int dynamic_rect_json(FILE *f, int node)
{
    const wn_rect *rects;
    wn_rect r;
    int hit = -1;
    int alive;
    if (g_core == NULL || node < 0) {
        fputs("null", f);
        return -1;
    }
    alive = wn_core_node_is_alive(g_core, node);
    rects = wn_core_rects(g_core, NULL);
    r = rects[node];
    if (alive && wn_core_node_is_visible(g_core, node) && r.w > 0.0f && r.h > 0.0f) {
        hit = wn_core_hit_test(g_core, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
    }
    fprintf(f, "{\"id\":%d,\"alive\":%d,\"visible\":%d,"
               "\"x\":%.2f,\"y\":%.2f,\"w\":%.2f,\"h\":%.2f,\"hit\":%d}",
            node, alive, wn_core_node_is_visible(g_core, node),
            r.x, r.y, r.w, r.h, hit);
    return alive;
}

static int dynamic_stage(FILE *report, const char *prefix, int stage,
                         const char *action, int card0, int card1, int clone)
{
    char path[MAX_PATH];
    wchar_t wide[MAX_PATH];
    double layout_ms = 0.0;
    double frame_ms = 0.0;
    int node_count;
    int image_cache = -1;

    snprintf(path, sizeof(path), "%s_%02d.png", prefix, stage);
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    if (render_core_png(400, 300, wide, &layout_ms, &frame_ms) != 0) {
        fprintf(stderr, "error: dynamic stage %d render failed\n", stage);
        return 1;
    }
    image_cache = g_last_image_cache;
    node_count = wn_core_live_node_count(g_core);
    printf("dynamic: stage=%d action=%s live=%d vertices=%d glyphs=%d images=%d "
           "cache=%d layout=%.3fms frame=%.3fms\n",
           stage, action, node_count, g_vertex_count, g_glyph_vertex_count,
           g_image_vertex_count, image_cache, layout_ms, frame_ms);
    fflush(stdout);

    fprintf(report, "%s{\"stage\":%d,\"action\":\"%s\",\"live\":%d,"
                    "\"vertices\":%d,\"glyphs\":%d,\"image_vertices\":%d,"
                    "\"image_cache\":%d,\"layout_ms\":%.3f,\"frame_ms\":%.3f,",
            stage == 0 ? "" : ",\n    ", stage, action, node_count,
            g_vertex_count, g_glyph_vertex_count, g_image_vertex_count,
            image_cache, layout_ms, frame_ms);
    fputs("\"card0\":", report);
    dynamic_rect_json(report, card0);
    fputs(",\"card1\":", report);
    dynamic_rect_json(report, card1);
    fputs(",\"clone\":", report);
    if (clone >= 0 && wn_core_node_is_alive(g_core, clone)) {
        dynamic_rect_json(report, clone);
    } else {
        fputs("null", report);
    }
    fputs(",\"card0_image\":", report);
    {
        const char *src = NULL;
        if (card0 >= 0 && wn_core_node_is_alive(g_core, card0)) {
            int image = wn_core_node_first_child(g_core, card0);
            src = wn_core_node_image_src(g_core, image);
        }
        if (src != NULL) {
            json_escape(report, src);
        } else {
            fputs("null", report);
        }
    }
    fputs(",\"clone_image\":", report);
    if (clone >= 0 && wn_core_node_is_alive(g_core, clone)) {
        int image = wn_core_node_first_child(g_core, clone);
        const char *src = wn_core_node_image_src(g_core, image);
        if (src != NULL) {
            json_escape(report, src);
        } else {
            fputs("null", report);
        }
    } else {
        fputs("null", report);
    }
    fputc('}', report);
    return 0;
}

static int run_dynamic(const char *prefix)
{
    FILE *report = NULL;
    char report_path[MAX_PATH];
    int cards;
    int card0;
    int card1;
    int image0;
    int label0;
    int text0;
    int clone = -1;
    int clone_image;
    int clone_label;
    int clone_text;
    int rc = 1;

    if (g_core == NULL || prefix == NULL || prefix[0] == '\0') {
        fprintf(stderr, "error: --dynamic needs an output prefix\n");
        return 1;
    }
    cards = wn_core_find_node(g_core, "body.root/div.cards");
    if (cards < 0) {
        cards = wn_core_find_node(g_core, "div.cards");
    }
    if (cards < 0) {
        fprintf(stderr, "error: --dynamic fixture has no div.cards\n");
        return 1;
    }
    card0 = wn_core_node_first_child(g_core, cards);
    card1 = card0 >= 0 ? wn_core_node_next_sibling(g_core, card0) : -1;
    image0 = card0 >= 0 ? wn_core_node_first_child(g_core, card0) : -1;
    label0 = image0 >= 0 ? wn_core_node_next_sibling(g_core, image0) : -1;
    text0 = label0 >= 0 ? wn_core_node_first_child(g_core, label0) : -1;
    if (card1 < 0 || text0 < 0) {
        fprintf(stderr, "error: --dynamic fixture is not the expected two-card page\n");
        return 1;
    }

    snprintf(report_path, sizeof(report_path), "%s_report.json", prefix);
    report = fopen(report_path, "wb");
    if (report == NULL) {
        fprintf(stderr, "error: cannot create %s\n", report_path);
        return 1;
    }
    fprintf(report, "{\n  \"fixture\": \"tests/dynamic/page\",\n  "
                    "\"viewport\": {\"width\": 400, \"height\": 300},\n  "
                    "\"cards\": %d, \"card0\": %d, \"card1\": %d,\n  "
                    "\"stages\": [\n    ", cards, card0, card1);

    if (dynamic_stage(report, prefix, 0, "initial", card0, card1, clone) != 0) goto done;
    if (wn_core_set_visible(g_core, card0, 0) != 0) goto done;
    if (dynamic_stage(report, prefix, 1, "hide card0", card0, card1, clone) != 0) goto done;
    if (wn_core_set_visible(g_core, card0, 1) != 0) goto done;
    if (dynamic_stage(report, prefix, 2, "show card0", card0, card1, clone) != 0) goto done;

    clone = wn_core_clone(g_core, card0, cards);
    if (clone < 0) {
        fprintf(stderr, "error: clone failed\n");
        goto done;
    }
    clone_image = wn_core_node_first_child(g_core, clone);
    clone_label = wn_core_node_next_sibling(g_core, clone_image);
    clone_text = wn_core_node_first_child(g_core, clone_label);
    if (dynamic_stage(report, prefix, 3, "clone card0", card0, card1, clone) != 0) goto done;

    if (wn_core_set_image(g_core, clone_image, "images/b.png") != 0) {
        fprintf(stderr, "error: set_image(clone) failed\n");
        goto done;
    }
    if (dynamic_stage(report, prefix, 4, "set_image clone b", card0, card1, clone) != 0) goto done;

    if (wn_core_set_text(g_core, clone_text, "Beta") != 0) {
        fprintf(stderr, "error: set_text(clone) failed\n");
        goto done;
    }
    if (dynamic_stage(report, prefix, 5, "set_text clone Beta", card0, card1, clone) != 0) goto done;

    if (wn_core_remove(g_core, clone) != 0) {
        fprintf(stderr, "error: remove(clone) failed\n");
        goto done;
    }
    if (dynamic_stage(report, prefix, 6, "remove clone", card0, card1, -1) != 0) goto done;
    clone = -1;

    if (wn_core_append(g_core, cards, card1) != 0) {
        fprintf(stderr, "error: append(card1) failed\n");
        goto done;
    }
    if (dynamic_stage(report, prefix, 7, "append card1 last", card0, card1, clone) != 0) goto done;

    if (wn_core_set_image(g_core, image0, "images/b.png") != 0) {
        fprintf(stderr, "error: set_image(card0) failed\n");
        goto done;
    }
    if (dynamic_stage(report, prefix, 8, "set_image card0 b", card0, card1, clone) != 0) goto done;
    if (wn_core_set_image(g_core, image0, "images/a.png") != 0) {
        fprintf(stderr, "error: set_image(card0 a) failed\n");
        goto done;
    }
    if (dynamic_stage(report, prefix, 9, "set_image card0 a", card0, card1, clone) != 0) goto done;

    if (wn_core_set_text(g_core, text0, "\xce\xa9") == 0) {
        printf("dynamic: set_text(Omega) -> shaped at runtime\n");
    } else {
        fprintf(stderr, "error: set_text(Omega) was rejected\n");
        goto done;
    }
    if (wn_core_clear_children(g_core, cards) != 0) {
        fprintf(stderr, "error: clear_children(cards) failed\n");
        goto done;
    }
    if (dynamic_stage(report, prefix, 10, "clear cards", card0, card1, clone) != 0) goto done;

    fprintf(report, "\n  ]\n}\n");
    rc = 0;

done:
    if (!rc) {
        fclose(report);
        report = NULL;
    } else if (report != NULL) {
        fprintf(report, "\n  ]\n}\n");
        fclose(report);
    }
    core_view_close();
    return rc;
}

/* ---------------------------------------------------------------- input v1 */

static int input_stage(FILE *report, const char *prefix, int stage,
                       const char *action, int node, int value_changes)
{
    char path[MAX_PATH];
    wchar_t wide[MAX_PATH];
    double layout_ms = 0.0;
    double frame_ms = 0.0;
    wn_input_state state;
    const wn_display_list *list;

    snprintf(path, sizeof(path), "%s_%02d.png", prefix, stage);
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    if (render_core_png(480, 240, wide, &layout_ms, &frame_ms) != 0) {
        fprintf(stderr, "error: input stage %d render failed\n", stage);
        return 1;
    }
    if (wn_core_input_state(g_core, node, &state) != 0) {
        fprintf(stderr, "error: input state failed\n");
        return 1;
    }
    list = wn_core_display_list(g_core);
    printf("input: stage=%d action=%s value=\"%s\" cursor=%d anchor=%d focus=%d "
           "caret=%.2f sel=(%.2f,%.2f) scroll=%.2f glyphs=%d layout=%.3fms\n",
           stage, action, state.value, state.cursor, state.anchor, state.focused,
           state.caret_x, state.selection_x0, state.selection_x1, state.scroll,
           list != NULL ? list->glyph_vertex_count : 0, layout_ms);
    fflush(stdout);
    fprintf(report,
            "%s{\"stage\":%d,\"action\":\"%s\",\"value\":",
            stage == 0 ? "" : ",\n    ", stage, action);
    json_escape(report, state.value);
    fprintf(report,
            ",\"cursor\":%d,\"anchor\":%d,\"focused\":%d,"
            "\"caret_x\":%.3f,\"selection_x0\":%.3f,\"selection_x1\":%.3f,"
            "\"scroll\":%.3f,\"baseline\":%.3f,\"line_height\":%.3f,"
            "\"rect\":{\"x\":%.2f,\"y\":%.2f,\"w\":%.2f,\"h\":%.2f},"
            "\"content\":{\"x\":%.2f,\"y\":%.2f,\"w\":%.2f,\"h\":%.2f},"
            "\"value_changes\":%d,\"glyph_vertices\":%d,\"png\":",
            state.cursor, state.anchor, state.focused, state.caret_x,
            state.selection_x0, state.selection_x1, state.scroll, state.baseline,
            state.line_height, state.rect.x, state.rect.y, state.rect.w, state.rect.h,
            state.content.x, state.content.y, state.content.w, state.content.h,
            value_changes, list != NULL ? list->glyph_vertex_count : 0);
    json_escape(report, path);
    fputc('}', report);
    return 0;
}

static int input_type(int node, const char *text, int *value_changes)
{
    int i;
    for (i = 0; text[i] != '\0'; ++i) {
        int changed = 0;
        int visual = 0;
        if (wn_core_input_char(g_core, (uint32_t)(unsigned char)text[i],
                               &changed, &visual) < 0) {
            return -1;
        }
        if (changed) {
            ++*value_changes;
        }
    }
    return node;
}

static int input_key(int node, int key, uint32_t mods, int *value_changes)
{
    int changed = 0;
    int visual = 0;
    if (wn_core_input_key(g_core, key, mods, &changed, &visual) < 0) {
        return -1;
    }
    if (changed) {
        ++*value_changes;
    }
    return node;
}

static int run_input(const char *prefix)
{

    FILE *report = NULL;
    char report_path[MAX_PATH];
    int node;
    int value_changes = 0;
    wn_rect rect;
    wn_input_state state;
    char clipboard[256];
    int copied;
    int paste_changed = 0;
    int paste_visual = 0;
    int rc = 1;

    if (g_core == NULL || prefix == NULL || prefix[0] == '\0') {
        fprintf(stderr, "error: --input needs an output prefix\n");
        return 1;
    }
    node = wn_core_find_node(g_core, "body.root/div.bar/input#search.search");
    if (node < 0) {
        node = wn_core_find_node(g_core, "input#search.search");
    }
    if (node < 0 || !wn_core_node_is_input(g_core, node)) {
        fprintf(stderr, "error: --input fixture has no input#search\n");
        return 1;
    }
    snprintf(report_path, sizeof(report_path), "%s_report.json", prefix);
    report = fopen(report_path, "wb");
    if (report == NULL) {
        fprintf(stderr, "error: cannot create %s\n", report_path);
        return 1;
    }
    {
        wn_rect content;
        if (wn_core_input_state(g_core, node, &state) != 0) {
            goto done;
        }
        content = state.content;
        fprintf(report,
                "{\n  \"fixture\": \"tests/input/page\",\n"
                "  \"viewport\": {\"width\": 480, \"height\": 240},\n"
                "  \"node\": %d,\n  \"field\": \"input#search\",\n"
                "  \"max_length\": %d,\n  \"stages\": [\n    ",
                node, state.max_length);
        (void)content;
    }

    if (input_stage(report, prefix, 0, "initial", node, value_changes) != 0) goto done;

    rect = wn_core_rects(g_core, NULL)[node];
    wn_core_pointer_down(g_core, rect.x + rect.w - 12.0f, rect.y + rect.h * 0.5f);
    wn_core_pointer_up(g_core, rect.x + rect.w - 12.0f, rect.y + rect.h * 0.5f);
    if (input_stage(report, prefix, 1, "click focuses", node, value_changes) != 0) goto done;

    if (input_type(node, "abc", &value_changes) < 0) goto done;
    if (input_stage(report, prefix, 2, "type abc", node, value_changes) != 0) goto done;

    /* Caret by click: Home+Right leaves the caret after 'a'; click 6 px past
     * that boundary to land after 'b', then insert X -> "abXc". */
    if (input_key(node, WN_KEY_HOME, 0, &value_changes) < 0) goto done;
    if (input_key(node, WN_KEY_RIGHT, 0, &value_changes) < 0) goto done;
    if (wn_core_input_state(g_core, node, &state) != 0) goto done;
    wn_core_pointer_down(g_core, state.caret_x + 6.0f, rect.y + rect.h * 0.5f);
    wn_core_pointer_up(g_core, state.caret_x + 6.0f, rect.y + rect.h * 0.5f);
    if (input_type(node, "X", &value_changes) < 0) goto done;
    if (input_stage(report, prefix, 3, "click caret + insert X", node,
                    value_changes) != 0) goto done;

    if (input_key(node, WN_KEY_BACKSPACE, 0, &value_changes) < 0) goto done;
    if (input_stage(report, prefix, 4, "backspace", node, value_changes) != 0) goto done;

    if (input_key(node, WN_KEY_HOME, 0, &value_changes) < 0) goto done;
    if (input_key(node, WN_KEY_RIGHT, WN_KEY_MOD_SHIFT, &value_changes) < 0) goto done;
    if (input_key(node, WN_KEY_RIGHT, WN_KEY_MOD_SHIFT, &value_changes) < 0) goto done;
    if (input_stage(report, prefix, 5, "shift selection", node,
                    value_changes) != 0) goto done;

    copied = wn_core_input_copy(g_core, clipboard, (int)sizeof(clipboard));
    printf("input: copy selection = \"%s\" (%d bytes)\n", clipboard, copied);
    fflush(stdout);
    if (input_key(node, WN_KEY_END, 0, &value_changes) < 0) goto done;
    if (wn_core_input_paste(g_core, clipboard, &paste_changed, &paste_visual) < 0) goto done;
    if (paste_changed) {
        ++value_changes;
    }
    if (input_stage(report, prefix, 6, "copy + paste", node,
                    value_changes) != 0) goto done;

    if (wn_core_input_select_all(g_core, NULL) < 0) goto done;
    if (input_key(node, WN_KEY_DELETE, 0, &value_changes) < 0) goto done;
    if (input_stage(report, prefix, 7, "select all + delete", node,
                    value_changes) != 0) goto done;

    if (input_type(node, "0123456789abcd", &value_changes) < 0) goto done;
    if (input_stage(report, prefix, 8, "max_length clamps at 12", node,
                    value_changes) != 0) goto done;

    if (input_key(node, WN_KEY_ESCAPE, 0, &value_changes) < 0) goto done;
    if (input_stage(report, prefix, 9, "escape blurs", node,
                    value_changes) != 0) goto done;

    fprintf(report, "\n  ]\n}\n");
    rc = 0;

done:
    if (rc != 0 && report != NULL) {
        fprintf(report, "\n  ]\n}\n");
    }
    if (report != NULL) {
        fclose(report);
    }
    core_view_close();
    return rc;
}

/* -------------------------------------------------------------------- main */

static int parse_size_list(const char *text, int *widths, int *heights, int max_count)
{
    int count = 0;
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s", text);
    char *save = NULL;
    for (char *token = strtok_s(buffer, ",", &save); token != NULL;
         token = strtok_s(NULL, ",", &save)) {
        int w = 0, h = 0;
        if (parse_size(token, &w, &h) != 0) {
            fprintf(stderr, "error: bad size '%s' (expected WxH)\n", token);
            return -1;
        }
        if (count < max_count) {
            widths[count] = w;
            heights[count] = h;
            ++count;
        }
    }
    return count;
}

int main(int argc, char **argv)
{
    int exit_code = 1;
    int mode_render = 0, mode_states = 0, mode_report = 0, mode_dump = 0, mode_dynamic = 0;
    int mode_input = 0;
    const char *dynamic_prefix = "dynamic";
    const char *input_prefix = "input";
    int render_w = 0, render_h = 0;
    const char *render_path = "out.png";
    const char *states_prefix = "state";
    const char *report_path = "layout_report.json";
    int state_node = -1;
    int width_arg = 0, height_arg = 0;
    int default_sizes = 1;
    int widths[8] = {960, 720, 560, 0, 0, 0, 0, 0};
    int heights[8] = {640, 640, 640, 0, 0, 0, 0, 0};
    int size_count = 3;
    const char *document_path = NULL;
    const wn_document *document = NULL;
    int focus_node = -1;
    int select_all = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    SetProcessDPIAware();
#if WN_WGPU
    wgpuSetLogLevel(WGPULogLevel_Warn);
    wgpuSetLogCallback(on_log, NULL);
#endif

    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--document") == 0) {
            document_path = argv[i + 1];
        }
    }
    document = document_path != NULL ? wn_document_load_blob(document_path)
                                     : wn_document_builtin();
    if (document == NULL) {
        fprintf(stderr, "error: %s\n", wn_last_error() != NULL ? wn_last_error() : "no document");
        exit_code = 1;
        goto done;
    }
    g_document = document;
    render_w = document->viewport_width;
    render_h = document->viewport_height;
    g_core = wn_core_create(document);
    if (g_core == NULL) {
        fprintf(stderr, "error: wn_core_create failed\n");
        exit_code = 1;
        goto done;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--render") == 0) {
            mode_render = 1;
            if (i + 1 < argc && strchr(argv[i + 1], 'x') != NULL) {
                if (parse_size(argv[i + 1], &render_w, &render_h) != 0) {
                    fprintf(stderr, "error: bad --render size\n");
                    return 1;
                }
                if (i + 2 >= argc) {
                    fprintf(stderr, "error: --render needs an output path\n");
                    return 1;
                }
                render_path = argv[i + 2];
                i += 2;
            } else {
                if (i + 3 >= argc) {
                    fprintf(stderr, "error: --render needs W H out.png\n");
                    return 1;
                }
                render_w = atoi(argv[i + 1]);
                render_h = atoi(argv[i + 2]);
                render_path = argv[i + 3];
                i += 3;
            }
        } else if (strcmp(argv[i], "--states") == 0) {
            mode_states = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                states_prefix = argv[++i];
            }
        } else if (strcmp(argv[i], "--node") == 0 && i + 1 < argc) {
            state_node = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--hover") == 0 && i + 1 < argc) {
            set_state_from_arg(WN_STATE_HOVER, atoi(argv[++i]));
        } else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc) {
            set_state_from_arg(WN_STATE_PRESS, atoi(argv[++i]));
        } else if (strcmp(argv[i], "--selected") == 0 && i + 1 < argc) {
            set_state_from_arg(WN_STATE_SELECTED, atoi(argv[++i]));
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (parse_size(argv[++i], &render_w, &render_h) != 0) {
                fprintf(stderr, "error: bad --size\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--dynamic") == 0) {
            mode_dynamic = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                dynamic_prefix = argv[++i];
            }
        } else if (strcmp(argv[i], "--input") == 0) {
            mode_input = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                input_prefix = argv[++i];
            }
        } else if (strcmp(argv[i], "--report") == 0) {
            mode_report = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                report_path = argv[++i];
            }
        } else if (strcmp(argv[i], "--sizes") == 0 && i + 1 < argc) {
            int n = parse_size_list(argv[++i], widths, heights, 8);
            if (n <= 0) {
                return 1;
            }
            size_count = n;
            default_sizes = 0;
        } else if (strcmp(argv[i], "--dump") == 0) {
            mode_dump = 1;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            g_max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--resize-demo") == 0) {
            g_resize_demo = 1;
        } else if (strcmp(argv[i], "--input-demo") == 0) {
            g_input_demo = 1;
        } else if (strcmp(argv[i], "--wheel") == 0 && i + 3 < argc) {
            g_wheel_active = 1;
            g_wheel_x = (float)atof(argv[i + 1]);
            g_wheel_y = (float)atof(argv[i + 2]);
            g_wheel_dy = (float)atof(argv[i + 3]);
            i += 3;
        } else if (strcmp(argv[i], "--document") == 0 && i + 1 < argc) {
            /* Consumed by the pre-scan before the core exists; skip the value
             * here so it does not surface as an ignored argument. */
            ++i;
        } else if (strcmp(argv[i], "--focus") == 0 && i + 1 < argc) {
            focus_node = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--select-all") == 0) {
            select_all = 1;
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width_arg = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height_arg = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: runtime_r2.exe [--document FILE] [--render W H out.png] "
                   "[--states PREFIX] [--report FILE] [--dynamic PREFIX] "
                   "[--input PREFIX] [--frames N] [--resize-demo] "
                   "[--focus N] [--select-all]\n");
            CoUninitialize();
            return 0;
        } else {
            fprintf(stderr, "warning: ignoring argument '%s'\n", argv[i]);
        }
    }
    if (focus_node >= 0) {
        /* Test/debug hook: focus an input before an offscreen render so the
         * focus ring, selection and caret passes are exercised. */
        if (focus_node < document->node_count &&
            wn_core_node_is_input(g_core, focus_node)) {
            wn_core_focus_input_at_end(g_core, focus_node);
            if (select_all) {
                int visual_changed = 0;
                wn_core_input_select_all(g_core, &visual_changed);
            }
        } else {
            fprintf(stderr, "warning: --focus %d is not an input\n", focus_node);
        }
    }
    if (width_arg > 0 && height_arg > 0) {
        g_window_width = width_arg;
        g_window_height = height_arg;
        render_w = width_arg;
        render_h = height_arg;
    }
    (void)default_sizes;
    (void)height_arg;

    if (mode_dump) {
        printf("nodes: %d\n", g_document->node_count);
        for (int i = 0; i < g_document->node_count; ++i) {
            const UiNode *n = &g_document->nodes[i];
            printf("[%2d] parent=%2d %-42s kind=%s dir=%s justify=%d align=%d wrap=%d "
                   "w=%.1f h=%.1f wpct=%.1f hpct=%.1f gap=%.1f bg=0x%08X action=%d\n",
                   i, n->parent, g_document->names[i], kind_name(n->kind),
                   n->direction ? "col" : "row", n->justify, n->align, n->wrap,
                   n->width, n->height, n->width_pct, n->height_pct, n->gap,
                   n->background, n->action);
        }
        exit_code = 0;
        goto done;
    }

    if (mode_report) {
        exit_code = write_layout_report(report_path, widths, heights, size_count);
        goto done;
    }

    if (mode_dynamic) {
        exit_code = run_dynamic(dynamic_prefix);
        goto done;
    }

    if (mode_input) {
        exit_code = run_input(input_prefix);
        goto done;
    }

    if (mode_states) {
        if (state_node < 0) {
            state_node = first_interactive_node();
        }
        exit_code = run_states(states_prefix, state_node, render_w, render_h);
        goto done;
    }

    if (mode_render) {
        double layout_ms = 0.0, frame_ms = 0.0;
        int vertices = 0, glyphs = 0, images = 0;
        wchar_t wide[MAX_PATH];
        if (render_w <= 0 || render_h <= 0) {
            fprintf(stderr, "error: bad render size %dx%d\n", render_w, render_h);
            return 1;
        }
        MultiByteToWideChar(CP_ACP, 0, render_path, -1, wide, MAX_PATH);
        if (render_offscreen_vt(render_w, render_h, wide, &layout_ms, &frame_ms,
                                &vertices, &glyphs, &images) != 0) {
            return 1;
        }
        print_layout_summary((float)render_w, (float)render_h);
        print_scroll_summary();
        printf("render: %s %dx%d nodes=%d vertices=%d glyph_vertices=%d "
               "image_vertices=%d layout=%.3fms frame=%.3fms\n",
               render_path, render_w, render_h, g_document->node_count, vertices, glyphs, images,
               layout_ms, frame_ms);
        printf("timings: layout=%.3fms | frame(build+upload+pass+readback+png)=%.3fms\n",
               layout_ms, frame_ms);
        exit_code = 0;
        goto done;
    }

    exit_code = run_window();

done:
    wn_core_destroy(g_core);
    g_core = NULL;
    if (document_path != NULL) {
        wn_document_destroy((wn_document *)document);
        document = NULL;
    }
    CoUninitialize();
    return exit_code;
}
