/*
 * runtime_r2.c - webnative R2 core runtime (src/runtime/c-r2).
 *
 * A real runtime, not an AOT viewer:
 *   - layout is computed in C at runtime from the generated UiNode table,
 *   - the Win32 window is resizable and WM_SIZE triggers a full re-layout,
 *   - hit testing drives hover / pressed / selected state,
 *   - all rectangles are rendered by wgpu-native (D3D12 on this machine),
 *   - the same layout engine can render deterministic offscreen PNGs.
 *
 * There is no browser, no WebView, no Skia, no Electron and no Python at
 * runtime. Build-time Python only produces ui_data.h (node table + strings).
 *
 * Usage:
 *   runtime_r2.exe --render 960 640 out.png
 *   runtime_r2.exe --render 960x640 out.png [--hover ID] [--press ID] [--selected ID]
 *   runtime_r2.exe --states out_prefix [--node ID] [--size 960x640]
 *   runtime_r2.exe --report layout_report.json [--sizes 960x640,720x640,560x640]
 *   runtime_r2.exe --frames 30 [--resize-demo]
 *   runtime_r2.exe                      (window, close with Esc)
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
#include <wgpu.h>

#include "ui_data.h"

#define ARRAY_LEN(a) ((size_t)(sizeof(a) / sizeof((a)[0])))
#define MAX_FLEX_ITEMS UI_NODE_COUNT

/* --------------------------------------------------------------- utilities */

static WGPUStringView sv(const char *text)
{
    WGPUStringView view;
    view.data = text;
    view.length = WGPU_STRLEN;
    return view;
}

static double now_ms(void)
{
    LARGE_INTEGER freq, tick;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tick);
    return (double)tick.QuadPart * 1000.0 / (double)freq.QuadPart;
}

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

static const char *backend_name(WGPUBackendType type)
{
    switch (type) {
    case WGPUBackendType_D3D11: return "D3D11";
    case WGPUBackendType_D3D12: return "D3D12";
    case WGPUBackendType_Metal: return "Metal";
    case WGPUBackendType_Vulkan: return "Vulkan";
    case WGPUBackendType_OpenGL: return "OpenGL";
    case WGPUBackendType_OpenGLES: return "OpenGLES";
    case WGPUBackendType_WebGPU: return "WebGPU";
    case WGPUBackendType_Null: return "Null";
    default: return "Undefined";
    }
}

static const char *adapter_type_name(WGPUAdapterType type)
{
    switch (type) {
    case WGPUAdapterType_DiscreteGPU: return "discrete-gpu";
    case WGPUAdapterType_IntegratedGPU: return "integrated-gpu";
    case WGPUAdapterType_CPU: return "cpu";
    case WGPUAdapterType_Unknown: return "unknown";
    default: return "undefined";
    }
}

/* wgpu-native v29.0.1.1 does not implement wgpuInstanceWaitAny; poll. */
static int wait_flag(WGPUInstance instance, volatile int *flag, const char *what)
{
    DWORD start = GetTickCount();
    while (!*flag) {
        wgpuInstanceProcessEvents(instance);
        if (GetTickCount() - start > 30000u) {
            fprintf(stderr, "error: %s: timed out after 30s\n", what);
            return 1;
        }
        Sleep(1);
    }
    return 0;
}

/* --------------------------------------------------------------- callbacks */

typedef struct {
    WGPUAdapter adapter;
    WGPURequestAdapterStatus status;
    volatile int done;
    char message[512];
} AdapterResult;

typedef struct {
    WGPUDevice device;
    WGPURequestDeviceStatus status;
    volatile int done;
    char message[512];
} DeviceResult;

typedef struct {
    WGPUMapAsyncStatus status;
    volatile int done;
    char message[512];
} MapResult;

static void copy_message(char *dst, size_t cap, WGPUStringView message)
{
    if (message.data == NULL || message.length == 0) {
        dst[0] = '\0';
        return;
    }
    if (message.length == WGPU_STRLEN) {
        snprintf(dst, cap, "%s", message.data);
    } else {
        size_t n = message.length < cap - 1 ? message.length : cap - 1;
        memcpy(dst, message.data, n);
        dst[n] = '\0';
    }
}

static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                       WGPUStringView message, void *userdata1, void *userdata2)
{
    AdapterResult *result = (AdapterResult *)userdata1;
    (void)userdata2;
    result->status = status;
    result->adapter = adapter;
    copy_message(result->message, sizeof(result->message), message);
    result->done = 1;
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device,
                      WGPUStringView message, void *userdata1, void *userdata2)
{
    DeviceResult *result = (DeviceResult *)userdata1;
    (void)userdata2;
    result->status = status;
    result->device = device;
    copy_message(result->message, sizeof(result->message), message);
    result->done = 1;
}

static void on_map(WGPUMapAsyncStatus status, WGPUStringView message,
                   void *userdata1, void *userdata2)
{
    MapResult *result = (MapResult *)userdata1;
    (void)userdata2;
    result->status = status;
    copy_message(result->message, sizeof(result->message), message);
    result->done = 1;
}

static void on_uncaptured_error(WGPUDevice const *device, WGPUErrorType type,
                                WGPUStringView message, void *userdata1, void *userdata2)
{
    (void)device;
    (void)userdata1;
    (void)userdata2;
    fprintf(stderr, "wgpu uncaptured error (type %d): ", (int)type);
    fprint_view(stderr, message);
    fputc('\n', stderr);
    fflush(stderr);
}

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

/* ------------------------------------------------------------- layout engine */

typedef struct { float x, y, w, h; } Rect;

static Rect g_rects[UI_NODE_COUNT];
static int g_first_child[UI_NODE_COUNT];
static int g_next_sibling[UI_NODE_COUNT];
static int g_subtree_end[UI_NODE_COUNT];

static void build_hierarchy(void)
{
    int last_child[UI_NODE_COUNT];
    for (int i = 0; i < UI_NODE_COUNT; ++i) {
        g_first_child[i] = -1;
        g_next_sibling[i] = -1;
        last_child[i] = -1;
        g_subtree_end[i] = i + 1;
    }
    for (int i = 0; i < UI_NODE_COUNT; ++i) {
        int parent = kUiNodes[i].parent;
        if (parent < 0 || parent >= UI_NODE_COUNT) {
            continue;
        }
        if (g_first_child[parent] < 0) {
            g_first_child[parent] = i;
        } else {
            g_next_sibling[last_child[parent]] = i;
        }
        last_child[parent] = i;
    }
    for (int i = UI_NODE_COUNT - 1; i >= 0; --i) {
        int end = i + 1;
        for (int child = g_first_child[i]; child >= 0; child = g_next_sibling[child]) {
            if (g_subtree_end[child] > end) {
                end = g_subtree_end[child];
            }
        }
        g_subtree_end[i] = end;
    }
}

static void translate_subtree(int node, float dx, float dy)
{
    int end = g_subtree_end[node];
    for (int i = node; i < end; ++i) {
        g_rects[i].x += dx;
        g_rects[i].y += dy;
    }
}

/* Max-content width. Needed because a flex item's auto main size is its
 * content width, not the container width (exp09 filled the container; that
 * would push the header pill out of the row). */
static float intrinsic_width(int node, float available)
{
    const UiNode *n = &kUiNodes[node];
    if (n->width >= 0.0f) {
        return n->width;
    }
    if (n->width_pct > 0.0f) {
        return available * n->width_pct * 0.01f;
    }
    if (n->kind == R2_NODE_TEXT) {
        return n->width > 0.0f ? n->width : 0.0f;
    }
    float content = 0.0f;
    if (n->kind == R2_NODE_FLEX && n->direction == 0) {
        float sum = 0.0f;
        int count = 0;
        for (int c = g_first_child[node]; c >= 0; c = g_next_sibling[c]) {
            sum += intrinsic_width(c, available);
            ++count;
        }
        if (count > 0) {
            sum += (n->gap > 0.0f ? n->gap : 0.0f) * (float)(count - 1);
        }
        content = sum;
    } else {
        float maxw = 0.0f;
        for (int c = g_first_child[node]; c >= 0; c = g_next_sibling[c]) {
            float w = intrinsic_width(c, available);
            if (w > maxw) {
                maxw = w;
            }
        }
        content = maxw;
    }
    return content + n->padding[1] + n->padding[3];
}

static float layout_node(int node, float origin_x, float origin_y,
                         float available_w, float parent_content_h,
                         int is_root, float forced_w);

typedef struct {
    int id;
    float w, h;
} FlexItem;

static float align_offset(int align, float extent, float size)
{
    if (align == ALIGN_CENTER) {
        return (extent - size) * 0.5f;
    }
    if (align == ALIGN_END) {
        return extent - size;
    }
    return 0.0f; /* start, stretch */
}

static void justify_offsets(int justify, float free, int count, float gap,
                            float *start, float *spacing)
{
    if (count <= 1) {
        *start = (justify == JUSTIFY_CENTER) ? free * 0.5f
               : (justify == JUSTIFY_END) ? free : 0.0f;
        *spacing = gap;
        return;
    }
    if (justify == JUSTIFY_CENTER) {
        *start = free * 0.5f;
        *spacing = gap;
    } else if (justify == JUSTIFY_END) {
        *start = free;
        *spacing = gap;
    } else if (justify == JUSTIFY_SPACE_BETWEEN) {
        *start = 0.0f;
        *spacing = gap + free / (float)(count - 1);
    } else {
        *start = 0.0f;
        *spacing = gap;
    }
}

/*
 * Flex layout. Ported from exp09 webnative/layout.py, with three R2 needs:
 *   - wrap (exp09 had none),
 *   - proportional shrink capped at 20% so a wrap decision can keep two
 *     180px cards on one line inside 344px of content (fixture at 560px),
 *   - stretch is applied after measuring (box only, descendants keep the
 *     measured positions; documented limitation).
 *
 * Items are measured at (0,0) and then translated, exactly like exp09.
 */
static float layout_flex_children(int parent, float content_x, float content_y,
                                  float content_w, float content_h)
{
    const UiNode *p = &kUiNodes[parent];
    FlexItem items[MAX_FLEX_ITEMS];
    int count = 0;

    for (int child = g_first_child[parent]; child >= 0; child = g_next_sibling[child]) {
        items[count].id = child;
        ++count;
    }
    if (count == 0) {
        return 0.0f;
    }

    const int row = (p->direction == 0);
    const int wrap = (p->wrap != 0);
    const float gap = p->gap > 0.0f ? p->gap : 0.0f;

    /* 1. Measure every item. */
    for (int k = 0; k < count; ++k) {
        int id = items[k].id;
        const UiNode *n = &kUiNodes[id];
        float forced = -1.0f;
        if (n->width < 0.0f && n->width_pct == 0.0f) {
            float margins = n->margin[1] + n->margin[3];
            float limit = fmaxf(content_w - margins, 0.0f);
            if (row) {
                forced = fminf(limit, intrinsic_width(id, content_w));
            } else if (p->align != ALIGN_STRETCH) {
                forced = fminf(limit, intrinsic_width(id, content_w));
            }
        }
        layout_node(id, 0.0f, 0.0f, content_w, content_h, 0, forced);
        items[k].w = g_rects[id].w;
        items[k].h = g_rects[id].h;
    }

    /* 2. Break into lines. Only when flex-wrap is set; non-wrap is one line
     *    (with optional proportional shrink if it overflows). */
    int line_start[MAX_FLEX_ITEMS];
    int line_count[MAX_FLEX_ITEMS];
    float line_cross[MAX_FLEX_ITEMS];
    float line_main[MAX_FLEX_ITEMS];
    int lines = 0;

    if (wrap) {
        int start = 0;
        while (start < count) {
            int remaining = count - start;
            int take = 1;
            int used_shrink = 0;
            float shrink_scale = 1.0f;
            if (remaining == 1) {
                take = 1;
                float base = row ? items[start].w : items[start].h;
                float limit = row ? content_w : content_h;
                if (limit < 0.0f) {
                    limit = base;
                }
                if (base > limit) {
                    shrink_scale = limit / base;
                    used_shrink = 1;
                }
            } else {
                for (int k = remaining; k >= 2; --k) {
                    float sum = 0.0f;
                    for (int j = start; j < start + k; ++j) {
                        sum += row ? items[j].w : items[j].h;
                    }
                    float need = sum + gap * (float)(k - 1);
                    float limit = row ? content_w : content_h;
                    if (limit < 0.0f) {
                        continue;
                    }
                    if (need <= limit + 0.01f) {
                        take = k;
                        break;
                    }
                    float scale = (limit - gap * (float)(k - 1)) / sum;
                    if (scale >= 0.80f) {
                        take = k;
                        shrink_scale = scale;
                        used_shrink = 1;
                        break;
                    }
                }
                if (take == 1 && remaining > 1) {
                    /* One item per line. Still guarantee it fits. */
                    float base = row ? items[start].w : items[start].h;
                    float limit = row ? content_w : content_h;
                    if (limit >= 0.0f && base > limit) {
                        shrink_scale = limit / base;
                        used_shrink = 1;
                    }
                }
            }
            if (used_shrink) {
                for (int j = start; j < start + take; ++j) {
                    if (row) {
                        items[j].w *= shrink_scale;
                    } else {
                        items[j].h *= shrink_scale;
                    }
                }
            }
            line_start[lines] = start;
            line_count[lines] = take;
            line_main[lines] = 0.0f;
            float cross = 0.0f;
            for (int j = start; j < start + take; ++j) {
                line_main[lines] += row ? items[j].w : items[j].h;
                float c = row ? items[j].h : items[j].w;
                if (c > cross) {
                    cross = c;
                }
            }
            line_main[lines] += gap * (float)(take - 1);
            line_cross[lines] = cross;
            ++lines;
            start += take;
        }
    } else {
        line_start[0] = 0;
        line_count[0] = count;
        line_main[0] = 0.0f;
        line_cross[0] = 0.0f;
        for (int j = 0; j < count; ++j) {
            line_main[0] += row ? items[j].w : items[j].h;
            float c = row ? items[j].h : items[j].w;
            if (c > line_cross[0]) {
                line_cross[0] = c;
            }
        }
        line_main[0] += gap * (float)(count - 1);
        lines = 1;
        /* No-wrap overflow: shrink up to 20%, else let it overflow. */
        float limit = row ? content_w : content_h;
        if (limit >= 0.0f && line_main[0] > limit + 0.01f && count > 0) {
            float base_sum = 0.0f;
            for (int j = 0; j < count; ++j) {
                base_sum += row ? items[j].w : items[j].h;
            }
            float scale = (limit - gap * (float)(count - 1)) / base_sum;
            if (scale < 1.0f && scale >= 0.80f) {
                for (int j = 0; j < count; ++j) {
                    if (row) {
                        items[j].w *= scale;
                    } else {
                        items[j].h *= scale;
                    }
                }
                line_main[0] = limit;
            }
        }
    }

    /* Single line: the line's cross size is the container's inner cross size
     * when that is definite (CSS single-line flex container). */
    if (lines == 1) {
        if (row && content_h >= 0.0f) {
            line_cross[0] = content_h;
        } else if (!row && content_w >= 0.0f) {
            line_cross[0] = content_w;
        }
    }

    /* 3. Place the lines and translate the measured subtrees. */
    float cross_origin = 0.0f;
    float return_main = 0.0f;
    for (int l = 0; l < lines; ++l) {
        int start = line_start[l];
        int take = line_count[l];
        float limit = row ? content_w : (content_h >= 0.0f ? content_h : line_main[l]);
        float free = limit - line_main[l];
        if (free < 0.0f) {
            free = 0.0f;
        }
        float start_off, spacing;
        justify_offsets(p->justify, free, take, gap, &start_off, &spacing);
        float cursor = start_off;
        for (int j = start; j < start + take; ++j) {
            int id = items[j].id;
            const UiNode *n = &kUiNodes[id];
            float w = items[j].w;
            float h = items[j].h;
            float cross_extent = line_cross[l];
            if (row) {
                if (p->align == ALIGN_STRETCH && n->height < 0.0f && n->height_pct == 0.0f) {
                    h = cross_extent;
                }
                float off = align_offset(p->align, cross_extent, h);
                g_rects[id].w = w;
                g_rects[id].h = h;
                translate_subtree(id, content_x + cursor, content_y + cross_origin + off);
                cursor += w + spacing;
            } else {
                if (p->align == ALIGN_STRETCH && n->width < 0.0f && n->width_pct == 0.0f) {
                    w = cross_extent;
                }
                float off = align_offset(p->align, cross_extent, w);
                g_rects[id].w = w;
                g_rects[id].h = h;
                translate_subtree(id, content_x + cross_origin + off, content_y + cursor);
                cursor += h + spacing;
            }
        }
        cross_origin += line_cross[l] + gap;
        if (row) {
            return_main += line_cross[l];
            if (l + 1 < lines) {
                return_main += gap;
            }
        } else if (line_main[l] > return_main) {
            return_main = line_main[l];
        }
    }

    return return_main;
}

static float layout_node(int node, float origin_x, float origin_y,
                         float available_w, float parent_content_h,
                         int is_root, float forced_w)
{
    const UiNode *n = &kUiNodes[node];
    float w;
    if (forced_w >= 0.0f) {
        w = forced_w;
    } else if (n->width >= 0.0f) {
        w = n->width;
    } else if (n->width_pct > 0.0f) {
        w = available_w * n->width_pct * 0.01f;
    } else {
        w = fmaxf(available_w - n->margin[1] - n->margin[3], 0.0f);
    }

    float x = origin_x + n->margin[3];
    float y = origin_y + n->margin[0];
    if (is_root) {
        x = 0.0f;
        y = 0.0f;
    }

    const float pad_l = n->padding[3], pad_r = n->padding[1];
    const float pad_t = n->padding[0], pad_b = n->padding[2];
    const float content_w = fmaxf(w - pad_l - pad_r, 0.0f);
    float content_h = -1.0f;
    if (n->height >= 0.0f) {
        content_h = fmaxf(n->height - pad_t - pad_b, 0.0f);
    } else if (n->height_pct > 0.0f && parent_content_h >= 0.0f) {
        content_h = fmaxf(parent_content_h * n->height_pct * 0.01f - pad_t - pad_b, 0.0f);
    }

    const float content_x = x + pad_l;
    const float content_y = y + pad_t;

    if (n->kind == R2_NODE_TEXT) {
        float h = n->height >= 0.0f ? n->height : (content_h >= 0.0f ? content_h : 0.0f);
        g_rects[node].x = x;
        g_rects[node].y = y;
        g_rects[node].w = w;
        g_rects[node].h = h;
        return h + n->margin[2];
    }

    float cursor = content_y;
    if (n->kind == R2_NODE_FLEX) {
        cursor += layout_flex_children(node, content_x, content_y, content_w, content_h);
    } else {
        for (int child = g_first_child[node]; child >= 0; child = g_next_sibling[child]) {
            cursor += layout_node(child, content_x, cursor, content_w, content_h, 0, -1.0f);
        }
    }
    const float content_used = cursor - content_y;

    float h;
    if (n->height >= 0.0f) {
        h = n->height;
    } else if (n->height_pct > 0.0f && parent_content_h >= 0.0f) {
        h = parent_content_h * n->height_pct * 0.01f;
    } else {
        h = content_used + pad_t + pad_b;
    }
    g_rects[node].x = x;
    g_rects[node].y = y;
    g_rects[node].w = w;
    g_rects[node].h = h;
    return h + n->margin[2];
}

static void layout_page(float viewport_w, float viewport_h)
{
    int root = 0;
    for (int i = 0; i < UI_NODE_COUNT; ++i) {
        if (kUiNodes[i].parent < 0) {
            root = i;
            break;
        }
    }
    layout_node(root, 0.0f, 0.0f, viewport_w, viewport_h, 1, -1.0f);
}

/* ------------------------------------------------------------- interaction */

static int g_hover = -1;
static int g_press = -1;
static int g_selected = -1;

static int point_in_rect(float px, float py, const Rect *r)
{
    return r->w > 0.0f && r->h > 0.0f &&
           px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

static int hit_test(float px, float py)
{
    for (int i = UI_NODE_COUNT - 1; i >= 0; --i) {
        if (kUiNodes[i].action != ACTION_NONE &&
            point_in_rect(px, py, &g_rects[i])) {
            return i;
        }
    }
    return -1;
}

static unsigned int node_color(int node)
{
    const UiNode *n = &kUiNodes[node];
    if (n->action == ACTION_NONE) {
        return n->background;
    }
    if (g_press == node && n->press_bg != 0) {
        return n->press_bg;
    }
    if (g_hover == node && n->hover_bg != 0) {
        return n->hover_bg;
    }
    if (g_selected == node && n->selected_bg != 0) {
        return n->selected_bg;
    }
    return n->background;
}

/* --------------------------------------------------------------- geometry */

typedef struct {
    float x, y;
    uint8_t r, g, b, a;
} Vertex;

_Static_assert(sizeof(Vertex) == 12, "Vertex must stay 12 bytes (vec2 + unorm8x4)");

static Vertex g_vertices[UI_NODE_COUNT * 6];
static int g_vertex_count = 0;

static void push_vertex(float x, float y, float vw, float vh,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    Vertex *v = &g_vertices[g_vertex_count++];
    v->x = (x / vw) * 2.0f - 1.0f;
    v->y = 1.0f - (y / vh) * 2.0f;
    v->r = r; v->g = g; v->b = b; v->a = a;
}

static void push_quad(float x, float y, float w, float h,
                      float vw, float vh, unsigned int argb)
{
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    uint8_t a = (uint8_t)((argb >> 24) & 0xFFu);
    if (a == 0) {
        return;
    }
    uint8_t r = (uint8_t)((argb >> 16) & 0xFFu);
    uint8_t g = (uint8_t)((argb >> 8) & 0xFFu);
    uint8_t b = (uint8_t)(argb & 0xFFu);
    float x1 = x + w, y1 = y + h;
    push_vertex(x,  y,  vw, vh, r, g, b, a);
    push_vertex(x1, y,  vw, vh, r, g, b, a);
    push_vertex(x1, y1, vw, vh, r, g, b, a);

    push_vertex(x,  y,  vw, vh, r, g, b, a);
    push_vertex(x1, y1, vw, vh, r, g, b, a);
    push_vertex(x,  y1, vw, vh, r, g, b, a);
}

static void build_text_geometry(float viewport_w, float viewport_h);

static void build_geometry(float viewport_w, float viewport_h)
{
    g_vertex_count = 0;
    for (int i = 0; i < UI_NODE_COUNT; ++i) {
        unsigned int color = node_color(i);
        if (color == 0) {
            continue;
        }
        const Rect *r = &g_rects[i];
        push_quad(r->x, r->y, r->w, r->h, viewport_w, viewport_h, color);
    }
    build_text_geometry(viewport_w, viewport_h);
}

/* Pre-shaped glyph quads: one textured pipeline, one draw call for all text. */
typedef struct {
    float x, y, u, v;
    uint8_t r, g, b, a;
} GlyphVertex;

_Static_assert(sizeof(GlyphVertex) == 20, "GlyphVertex must stay 20 bytes");

static GlyphVertex g_glyph_vertices[(UI_GLYPH_COUNT > 0 ? UI_GLYPH_COUNT : 1) * 6];
static int g_glyph_vertex_count = 0;

static void push_glyph_vertex(float x, float y, float u, float v,
                              float vw, float vh,
                              uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    GlyphVertex *out = &g_glyph_vertices[g_glyph_vertex_count++];
    out->x = (x / vw) * 2.0f - 1.0f;
    out->y = 1.0f - (y / vh) * 2.0f;
    out->u = u;
    out->v = v;
    out->r = r; out->g = g; out->b = b; out->a = a;
}

static void push_glyph_quad(float x, float y, const UiGlyph *g, float vw, float vh,
                            uint8_t r, uint8_t gr, uint8_t b, uint8_t a)
{
    float x1 = x + g->w, y1 = y + g->h;
    push_glyph_vertex(x,  y,  g->u0, g->v0, vw, vh, r, gr, b, a);
    push_glyph_vertex(x1, y,  g->u1, g->v0, vw, vh, r, gr, b, a);
    push_glyph_vertex(x1, y1, g->u1, g->v1, vw, vh, r, gr, b, a);

    push_glyph_vertex(x,  y,  g->u0, g->v0, vw, vh, r, gr, b, a);
    push_glyph_vertex(x1, y1, g->u1, g->v1, vw, vh, r, gr, b, a);
    push_glyph_vertex(x,  y1, g->u0, g->v1, vw, vh, r, gr, b, a);
}

static void build_text_geometry(float viewport_w, float viewport_h)
{
    g_glyph_vertex_count = 0;
    for (int run_index = 0; run_index < UI_TEXT_RUN_COUNT; ++run_index) {
        const UiTextRun *run = &kUiTextRuns[run_index];
        if (run->node < 0 || run->node >= UI_NODE_COUNT || run->count <= 0) {
            continue;
        }
        const Rect *r = &g_rects[run->node];
        unsigned int color = kUiNodes[run->node].text_color;
        uint8_t a = (uint8_t)((color >> 24) & 0xFFu);
        if (a == 0) {
            continue;
        }
        uint8_t cr = (uint8_t)((color >> 16) & 0xFFu);
        uint8_t cg = (uint8_t)((color >> 8) & 0xFFu);
        uint8_t cb = (uint8_t)(color & 0xFFu);
        for (int j = 0; j < run->count; ++j) {
            const UiGlyph *g = &kUiGlyphs[run->first + j];
            if (g->w <= 0.0f || g->h <= 0.0f) {
                continue;
            }
            /* Round to the pixel grid: the bitmaps were rasterized at integer
             * positions, so subpixel placement only adds blur. */
            float x = floorf(r->x + g->x + 0.5f);
            float y = floorf(r->y + g->y + 0.5f);
            push_glyph_quad(x, y, g, viewport_w, viewport_h, cr, cg, cb, a);
        }
    }
}

/* ------------------------------------------------------------------- wgpu */

typedef struct {
    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;
    WGPUShaderModule shader;
    WGPURenderPipeline pipeline;
    WGPUBuffer vertex_buffer;
    WGPUTextureFormat format;
    size_t vertex_capacity;
    /* R8 glyph atlas + textured pipeline (stretch goal) */
    WGPUShaderModule text_shader;
    WGPURenderPipeline text_pipeline;
    WGPUBuffer glyph_buffer;
    size_t glyph_capacity;
    WGPUTexture atlas_texture;
    WGPUTextureView atlas_view;
    WGPUSampler sampler;
    WGPUBindGroupLayout text_bgl;
    WGPUPipelineLayout text_pl;
    WGPUBindGroup text_bg;
} Gpu;

static const char *kShaderWGSL =
    "struct VSOut {\n"
    "  @builtin(position) position: vec4<f32>,\n"
    "  @location(0) color: vec4<f32>,\n"
    "};\n"
    "\n"
    "@vertex\n"
    "fn vs_main(@location(0) position: vec2<f32>, @location(1) color: vec4<f32>) -> VSOut {\n"
    "  var out: VSOut;\n"
    "  out.position = vec4<f32>(position, 0.0, 1.0);\n"
    "  out.color = color;\n"
    "  return out;\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fs_main(in: VSOut) -> @location(0) vec4<f32> {\n"
    "  return in.color;\n"
    "}\n";

/* Text uses the alpha channel of an R8 atlas. Positions are pre-shaped at
 * build time, so the vertex shader only needs the current viewport transform
 * (already baked into clip space by the CPU). */
static const char *kTextShaderWGSL =
    "@group(0) @binding(0) var atlas_texture: texture_2d<f32>;\n"
    "@group(0) @binding(1) var atlas_sampler: sampler;\n"
    "\n"
    "struct VSOut {\n"
    "  @builtin(position) position: vec4<f32>,\n"
    "  @location(0) uv: vec2<f32>,\n"
    "  @location(1) color: vec4<f32>,\n"
    "};\n"
    "\n"
    "@vertex\n"
    "fn vs_main(@location(0) position: vec2<f32>, @location(1) uv: vec2<f32>,\n"
    "           @location(2) color: vec4<f32>) -> VSOut {\n"
    "  var out: VSOut;\n"
    "  out.position = vec4<f32>(position, 0.0, 1.0);\n"
    "  out.uv = uv;\n"
    "  out.color = color;\n"
    "  return out;\n"
    "}\n"
    "\n"
    "@fragment\n"
    "fn fs_main(in: VSOut) -> @location(0) vec4<f32> {\n"
    "  let coverage = textureSample(atlas_texture, atlas_sampler, in.uv).r;\n"
    "  return vec4<f32>(in.color.rgb, in.color.a * coverage);\n"
    "}\n";

static void gpu_release(Gpu *gpu)
{
    if (gpu->vertex_buffer) wgpuBufferRelease(gpu->vertex_buffer);
    if (gpu->glyph_buffer) wgpuBufferRelease(gpu->glyph_buffer);
    if (gpu->text_bg) wgpuBindGroupRelease(gpu->text_bg);
    if (gpu->text_pl) wgpuPipelineLayoutRelease(gpu->text_pl);
    if (gpu->text_bgl) wgpuBindGroupLayoutRelease(gpu->text_bgl);
    if (gpu->sampler) wgpuSamplerRelease(gpu->sampler);
    if (gpu->atlas_view) wgpuTextureViewRelease(gpu->atlas_view);
    if (gpu->atlas_texture) wgpuTextureRelease(gpu->atlas_texture);
    if (gpu->text_pipeline) wgpuRenderPipelineRelease(gpu->text_pipeline);
    if (gpu->text_shader) wgpuShaderModuleRelease(gpu->text_shader);
    if (gpu->pipeline) wgpuRenderPipelineRelease(gpu->pipeline);
    if (gpu->shader) wgpuShaderModuleRelease(gpu->shader);
    if (gpu->queue) wgpuQueueRelease(gpu->queue);
    if (gpu->device) wgpuDeviceRelease(gpu->device);
    if (gpu->adapter) wgpuAdapterRelease(gpu->adapter);
    if (gpu->instance) wgpuInstanceRelease(gpu->instance);
    memset(gpu, 0, sizeof(*gpu));
}

static int gpu_create_text(Gpu *gpu);

static int gpu_init(Gpu *gpu, WGPUSurface compatible_surface,
                    int request_surface_format, WGPUTextureFormat forced_format,
                    const char *label)
{
    AdapterResult adapter_result;
    DeviceResult device_result;
    WGPUAdapterInfo info;

    memset(gpu, 0, sizeof(*gpu));
    gpu->instance = wgpuCreateInstance(NULL);
    if (gpu->instance == NULL) {
        fprintf(stderr, "error: wgpuCreateInstance returned NULL\n");
        return 1;
    }

    memset(&adapter_result, 0, sizeof(adapter_result));
    {
        WGPURequestAdapterOptions options;
        WGPURequestAdapterCallbackInfo cb;
        memset(&options, 0, sizeof(options));
        memset(&cb, 0, sizeof(cb));
        options.powerPreference = WGPUPowerPreference_HighPerformance;
        options.featureLevel = WGPUFeatureLevel_Core;
        options.compatibleSurface = compatible_surface;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = on_adapter;
        cb.userdata1 = &adapter_result;
        (void)wgpuInstanceRequestAdapter(gpu->instance, &options, cb);
        if (wait_flag(gpu->instance, &adapter_result.done, "request adapter") != 0) {
            return 1;
        }
    }
    if (adapter_result.status != WGPURequestAdapterStatus_Success || adapter_result.adapter == NULL) {
        fprintf(stderr, "error: no adapter (status %d): %s\n",
                (int)adapter_result.status, adapter_result.message);
        return 1;
    }
    gpu->adapter = adapter_result.adapter;

    memset(&info, 0, sizeof(info));
    if (wgpuAdapterGetInfo(gpu->adapter, &info) == WGPUStatus_Success) {
        printf("adapter: backend=%s (%d) type=%s vendorID=0x%04X deviceID=0x%04X\n",
               backend_name(info.backendType), (int)info.backendType,
               adapter_type_name(info.adapterType), info.vendorID, info.deviceID);
        print_view("adapter: device=", info.device);
        printf("\n");
        print_view("adapter: description=", info.description);
        printf("\n");
        fflush(stdout);
    }

    memset(&device_result, 0, sizeof(device_result));
    {
        WGPUDeviceDescriptor descriptor;
        WGPURequestDeviceCallbackInfo cb;
        memset(&descriptor, 0, sizeof(descriptor));
        memset(&cb, 0, sizeof(cb));
        descriptor.label = sv(label);
        descriptor.requiredFeatureCount = 0;
        descriptor.uncapturedErrorCallbackInfo.callback = on_uncaptured_error;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = on_device;
        cb.userdata1 = &device_result;
        (void)wgpuAdapterRequestDevice(gpu->adapter, &descriptor, cb);
        if (wait_flag(gpu->instance, &device_result.done, "request device") != 0) {
            return 1;
        }
    }
    if (device_result.status != WGPURequestDeviceStatus_Success || device_result.device == NULL) {
        fprintf(stderr, "error: no device (status %d): %s\n",
                (int)device_result.status, device_result.message);
        return 1;
    }
    gpu->device = device_result.device;
    gpu->queue = wgpuDeviceGetQueue(gpu->device);

    /* Pick a format: prefer a plain (non-sRGB) 8-bit format so offscreen and
     * surface colors match byte for byte. */
    gpu->format = forced_format;
    if (request_surface_format) {
        WGPUSurfaceCapabilities caps;
        memset(&caps, 0, sizeof(caps));
        if (wgpuSurfaceGetCapabilities(compatible_surface, gpu->adapter, &caps) != WGPUStatus_Success ||
            caps.formatCount == 0) {
            fprintf(stderr, "error: surface capabilities failed\n");
            return 1;
        }
        gpu->format = caps.formats[0];
        for (size_t i = 0; i < caps.formatCount; ++i) {
            if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm ||
                caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
                gpu->format = caps.formats[i];
                break;
            }
        }
        printf("surface: formats=%zu chosenFormat=%d\n", caps.formatCount, (int)gpu->format);
    }

    {
        WGPUShaderModuleDescriptor descriptor;
        WGPUShaderSourceWGSL wgsl;
        memset(&descriptor, 0, sizeof(descriptor));
        memset(&wgsl, 0, sizeof(wgsl));
        wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
        wgsl.code = sv(kShaderWGSL);
        descriptor.label = sv("r2-shader");
        descriptor.nextInChain = &wgsl.chain;
        gpu->shader = wgpuDeviceCreateShaderModule(gpu->device, &descriptor);
        if (gpu->shader == NULL) {
            fprintf(stderr, "error: shader module creation failed\n");
            return 1;
        }
    }

    {
        WGPUVertexAttribute attributes[2];
        WGPUVertexBufferLayout buffer_layout;
        WGPUColorTargetState target;
        WGPUBlendState blend;
        WGPUFragmentState fragment;
        WGPUVertexState vertex;
        WGPUPrimitiveState primitive;
        WGPUMultisampleState multisample;
        WGPURenderPipelineDescriptor descriptor;

        memset(attributes, 0, sizeof(attributes));
        attributes[0].format = WGPUVertexFormat_Float32x2;
        attributes[0].offset = 0;
        attributes[0].shaderLocation = 0;
        attributes[1].format = WGPUVertexFormat_Unorm8x4;
        attributes[1].offset = 8;
        attributes[1].shaderLocation = 1;

        memset(&buffer_layout, 0, sizeof(buffer_layout));
        buffer_layout.stepMode = WGPUVertexStepMode_Vertex;
        buffer_layout.arrayStride = sizeof(Vertex);
        buffer_layout.attributeCount = (uint32_t)ARRAY_LEN(attributes);
        buffer_layout.attributes = attributes;

        memset(&blend, 0, sizeof(blend));
        blend.color.operation = WGPUBlendOperation_Add;
        blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blend.alpha.operation = WGPUBlendOperation_Add;
        blend.alpha.srcFactor = WGPUBlendFactor_One;
        blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

        memset(&target, 0, sizeof(target));
        target.format = gpu->format;
        target.blend = &blend;
        target.writeMask = WGPUColorWriteMask_All;

        memset(&fragment, 0, sizeof(fragment));
        fragment.module = gpu->shader;
        fragment.entryPoint = sv("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &target;

        memset(&vertex, 0, sizeof(vertex));
        vertex.module = gpu->shader;
        vertex.entryPoint = sv("vs_main");
        vertex.bufferCount = 1;
        vertex.buffers = &buffer_layout;

        memset(&primitive, 0, sizeof(primitive));
        primitive.topology = WGPUPrimitiveTopology_TriangleList;
        primitive.frontFace = WGPUFrontFace_CCW;
        primitive.cullMode = WGPUCullMode_None;

        memset(&multisample, 0, sizeof(multisample));
        multisample.count = 1;
        multisample.mask = 0xFFFFFFFFu;

        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-rect-pipeline");
        descriptor.layout = NULL;
        descriptor.vertex = vertex;
        descriptor.primitive = primitive;
        descriptor.depthStencil = NULL;
        descriptor.multisample = multisample;
        descriptor.fragment = &fragment;
        gpu->pipeline = wgpuDeviceCreateRenderPipeline(gpu->device, &descriptor);
        if (gpu->pipeline == NULL) {
            fprintf(stderr, "error: render pipeline creation failed\n");
            return 1;
        }
    }

    gpu->vertex_capacity = (size_t)UI_NODE_COUNT * 6u * sizeof(Vertex);
    {
        WGPUBufferDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-vertices");
        descriptor.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        descriptor.size = gpu->vertex_capacity;
        gpu->vertex_buffer = wgpuDeviceCreateBuffer(gpu->device, &descriptor);
        if (gpu->vertex_buffer == NULL) {
            fprintf(stderr, "error: vertex buffer creation failed\n");
            return 1;
        }
    }
    return gpu_create_text(gpu);
}

/* R8 glyph atlas + textured pipeline. Created once; the atlas is immutable. */
static int gpu_create_text(Gpu *gpu)
{
    {
        WGPUShaderModuleDescriptor descriptor;
        WGPUShaderSourceWGSL wgsl;
        memset(&descriptor, 0, sizeof(descriptor));
        memset(&wgsl, 0, sizeof(wgsl));
        wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
        wgsl.code = sv(kTextShaderWGSL);
        descriptor.label = sv("r2-text-shader");
        descriptor.nextInChain = &wgsl.chain;
        gpu->text_shader = wgpuDeviceCreateShaderModule(gpu->device, &descriptor);
        if (gpu->text_shader == NULL) {
            fprintf(stderr, "error: text shader creation failed\n");
            return 1;
        }
    }

    {
        WGPUTextureDescriptor descriptor;
        WGPUTexelCopyTextureInfo destination;
        WGPUTexelCopyBufferLayout layout;
        WGPUExtent3D extent;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-glyph-atlas");
        descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        descriptor.dimension = WGPUTextureDimension_2D;
        descriptor.size.width = UI_ATLAS_SIZE;
        descriptor.size.height = UI_ATLAS_SIZE;
        descriptor.size.depthOrArrayLayers = 1;
        descriptor.format = WGPUTextureFormat_R8Unorm;
        descriptor.mipLevelCount = 1;
        descriptor.sampleCount = 1;
        gpu->atlas_texture = wgpuDeviceCreateTexture(gpu->device, &descriptor);
        if (gpu->atlas_texture == NULL) {
            fprintf(stderr, "error: glyph atlas creation failed\n");
            return 1;
        }
        gpu->atlas_view = wgpuTextureCreateView(gpu->atlas_texture, NULL);
        if (gpu->atlas_view == NULL) {
            fprintf(stderr, "error: glyph atlas view failed\n");
            return 1;
        }
        memset(&destination, 0, sizeof(destination));
        destination.texture = gpu->atlas_texture;
        destination.mipLevel = 0;
        destination.aspect = WGPUTextureAspect_All;
        memset(&layout, 0, sizeof(layout));
        layout.offset = 0;
        layout.bytesPerRow = UI_ATLAS_SIZE;
        layout.rowsPerImage = UI_ATLAS_SIZE;
        memset(&extent, 0, sizeof(extent));
        extent.width = UI_ATLAS_SIZE;
        extent.height = UI_ATLAS_SIZE;
        extent.depthOrArrayLayers = 1;
        wgpuQueueWriteTexture(gpu->queue, &destination, kUiAtlas, sizeof(kUiAtlas),
                              &layout, &extent);
    }

    {
        WGPUSamplerDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-atlas-sampler");
        descriptor.addressModeU = WGPUAddressMode_ClampToEdge;
        descriptor.addressModeV = WGPUAddressMode_ClampToEdge;
        descriptor.addressModeW = WGPUAddressMode_ClampToEdge;
        descriptor.magFilter = WGPUFilterMode_Linear;
        descriptor.minFilter = WGPUFilterMode_Linear;
        descriptor.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        descriptor.lodMinClamp = 0.0f;
        descriptor.lodMaxClamp = 32.0f;
        descriptor.maxAnisotropy = 1;
        gpu->sampler = wgpuDeviceCreateSampler(gpu->device, &descriptor);
        if (gpu->sampler == NULL) {
            fprintf(stderr, "error: sampler creation failed\n");
            return 1;
        }
    }

    {
        WGPUBindGroupLayoutEntry entries[2];
        WGPUBindGroupLayoutDescriptor descriptor;
        memset(entries, 0, sizeof(entries));
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Fragment;
        entries[0].texture.sampleType = WGPUTextureSampleType_Float;
        entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Fragment;
        entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-text-bgl");
        descriptor.entryCount = 2;
        descriptor.entries = entries;
        gpu->text_bgl = wgpuDeviceCreateBindGroupLayout(gpu->device, &descriptor);
        if (gpu->text_bgl == NULL) {
            fprintf(stderr, "error: bind group layout creation failed\n");
            return 1;
        }
    }

    {
        WGPUPipelineLayoutDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-text-pipeline-layout");
        descriptor.bindGroupLayoutCount = 1;
        descriptor.bindGroupLayouts = &gpu->text_bgl;
        gpu->text_pl = wgpuDeviceCreatePipelineLayout(gpu->device, &descriptor);
        if (gpu->text_pl == NULL) {
            fprintf(stderr, "error: pipeline layout creation failed\n");
            return 1;
        }
    }

    {
        WGPUBindGroupEntry entries[2];
        WGPUBindGroupDescriptor descriptor;
        memset(entries, 0, sizeof(entries));
        entries[0].binding = 0;
        entries[0].textureView = gpu->atlas_view;
        entries[1].binding = 1;
        entries[1].sampler = gpu->sampler;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-text-bind-group");
        descriptor.layout = gpu->text_bgl;
        descriptor.entryCount = 2;
        descriptor.entries = entries;
        gpu->text_bg = wgpuDeviceCreateBindGroup(gpu->device, &descriptor);
        if (gpu->text_bg == NULL) {
            fprintf(stderr, "error: bind group creation failed\n");
            return 1;
        }
    }

    {
        WGPUVertexAttribute attributes[3];
        WGPUVertexBufferLayout buffer_layout;
        WGPUColorTargetState target;
        WGPUBlendState blend;
        WGPUFragmentState fragment;
        WGPUVertexState vertex;
        WGPUPrimitiveState primitive;
        WGPUMultisampleState multisample;
        WGPURenderPipelineDescriptor descriptor;

        memset(attributes, 0, sizeof(attributes));
        attributes[0].format = WGPUVertexFormat_Float32x2;
        attributes[0].offset = 0;
        attributes[0].shaderLocation = 0;
        attributes[1].format = WGPUVertexFormat_Float32x2;
        attributes[1].offset = 8;
        attributes[1].shaderLocation = 1;
        attributes[2].format = WGPUVertexFormat_Unorm8x4;
        attributes[2].offset = 16;
        attributes[2].shaderLocation = 2;

        memset(&buffer_layout, 0, sizeof(buffer_layout));
        buffer_layout.stepMode = WGPUVertexStepMode_Vertex;
        buffer_layout.arrayStride = 20; /* GlyphVertex */
        buffer_layout.attributeCount = 3;
        buffer_layout.attributes = attributes;

        memset(&blend, 0, sizeof(blend));
        blend.color.operation = WGPUBlendOperation_Add;
        blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blend.alpha.operation = WGPUBlendOperation_Add;
        blend.alpha.srcFactor = WGPUBlendFactor_One;
        blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

        memset(&target, 0, sizeof(target));
        target.format = gpu->format;
        target.blend = &blend;
        target.writeMask = WGPUColorWriteMask_All;

        memset(&fragment, 0, sizeof(fragment));
        fragment.module = gpu->text_shader;
        fragment.entryPoint = sv("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &target;

        memset(&vertex, 0, sizeof(vertex));
        vertex.module = gpu->text_shader;
        vertex.entryPoint = sv("vs_main");
        vertex.bufferCount = 1;
        vertex.buffers = &buffer_layout;

        memset(&primitive, 0, sizeof(primitive));
        primitive.topology = WGPUPrimitiveTopology_TriangleList;
        primitive.frontFace = WGPUFrontFace_CCW;
        primitive.cullMode = WGPUCullMode_None;

        memset(&multisample, 0, sizeof(multisample));
        multisample.count = 1;
        multisample.mask = 0xFFFFFFFFu;

        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-text-pipeline");
        descriptor.layout = gpu->text_pl;
        descriptor.vertex = vertex;
        descriptor.primitive = primitive;
        descriptor.depthStencil = NULL;
        descriptor.multisample = multisample;
        descriptor.fragment = &fragment;
        gpu->text_pipeline = wgpuDeviceCreateRenderPipeline(gpu->device, &descriptor);
        if (gpu->text_pipeline == NULL) {
            fprintf(stderr, "error: text pipeline creation failed\n");
            return 1;
        }
    }

    {
        WGPUBufferDescriptor descriptor;
        size_t glyphs = UI_GLYPH_COUNT > 0 ? (size_t)UI_GLYPH_COUNT : 1u;
        gpu->glyph_capacity = glyphs * 6u * 20u;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-glyph-vertices");
        descriptor.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        descriptor.size = gpu->glyph_capacity;
        gpu->glyph_buffer = wgpuDeviceCreateBuffer(gpu->device, &descriptor);
        if (gpu->glyph_buffer == NULL) {
            fprintf(stderr, "error: glyph vertex buffer creation failed\n");
            return 1;
        }
    }
    return 0;
}

static void gpu_upload_geometry(Gpu *gpu)
{
    size_t bytes = (size_t)g_vertex_count * sizeof(Vertex);
    if (bytes > 0) {
        wgpuQueueWriteBuffer(gpu->queue, gpu->vertex_buffer, 0, g_vertices, bytes);
    }
    size_t glyph_bytes = (size_t)g_glyph_vertex_count * sizeof(GlyphVertex);
    if (glyph_bytes > 0) {
        wgpuQueueWriteBuffer(gpu->queue, gpu->glyph_buffer, 0, g_glyph_vertices, glyph_bytes);
    }
}

static void gpu_draw(Gpu *gpu, WGPUCommandEncoder encoder, WGPUTextureView view,
                     const float clear[4])
{
    WGPURenderPassColorAttachment attachment;
    WGPURenderPassDescriptor descriptor;
    WGPURenderPassEncoder pass;
    memset(&attachment, 0, sizeof(attachment));
    attachment.view = view;
    attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    attachment.loadOp = WGPULoadOp_Clear;
    attachment.storeOp = WGPUStoreOp_Store;
    attachment.clearValue.r = clear[0];
    attachment.clearValue.g = clear[1];
    attachment.clearValue.b = clear[2];
    attachment.clearValue.a = clear[3];

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.label = sv("r2-pass");
    descriptor.colorAttachmentCount = 1;
    descriptor.colorAttachments = &attachment;

    pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
    if (pass == NULL) {
        return;
    }
    wgpuRenderPassEncoderSetPipeline(pass, gpu->pipeline);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, gpu->vertex_buffer, 0, WGPU_WHOLE_SIZE);
    if (g_vertex_count > 0) {
        wgpuRenderPassEncoderDraw(pass, (uint32_t)g_vertex_count, 1, 0, 0);
    }
    if (g_glyph_vertex_count > 0 && gpu->text_pipeline != NULL) {
        wgpuRenderPassEncoderSetPipeline(pass, gpu->text_pipeline);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, gpu->glyph_buffer, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, gpu->text_bg, 0, NULL);
        wgpuRenderPassEncoderDraw(pass, (uint32_t)g_glyph_vertex_count, 1, 0, 0);
    }
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

/* -------------------------------------------------------------- WIC -> PNG */

static int report_hr(HRESULT hr, const char *what)
{
    fprintf(stderr, "error: %s failed (hr=0x%08lX)\n", what, (unsigned long)hr);
    return 1;
}

static int write_png_rgba(const unsigned char *rgba, unsigned int width,
                          unsigned int height, unsigned int src_stride,
                          const wchar_t *path)
{
    HRESULT hr;
    IWICImagingFactory *factory = NULL;
    IWICBitmap *bitmap = NULL;
    IWICBitmapLock *lock = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *options = NULL;
    WICRect area;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    UINT dst_stride = 0, dst_bytes = 0;
    BYTE *dst = NULL;
    unsigned int y;
    int exit_code = 1;

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(hr)) {
        return report_hr(hr, "CoCreateInstance(WICImagingFactory)");
    }

    hr = factory->lpVtbl->CreateBitmap(factory, width, height, &GUID_WICPixelFormat32bppRGBA,
                                       WICBitmapCacheOnLoad, &bitmap);
    if (FAILED(hr)) {
        report_hr(hr, "IWICImagingFactory::CreateBitmap");
        goto cleanup;
    }

    area.X = 0;
    area.Y = 0;
    area.Width = (UINT)width;
    area.Height = (UINT)height;
    hr = bitmap->lpVtbl->Lock(bitmap, &area, WICBitmapLockWrite, &lock);
    if (FAILED(hr)) {
        report_hr(hr, "IWICBitmap::Lock");
        goto cleanup;
    }
    hr = lock->lpVtbl->GetStride(lock, &dst_stride);
    if (FAILED(hr) || dst_stride < width * 4) {
        report_hr(FAILED(hr) ? hr : E_FAIL, "IWICBitmapLock::GetStride");
        goto cleanup;
    }
    hr = lock->lpVtbl->GetDataPointer(lock, &dst_bytes, &dst);
    if (FAILED(hr) || dst == NULL || dst_bytes < dst_stride * height) {
        report_hr(FAILED(hr) ? hr : E_FAIL, "IWICBitmapLock::GetDataPointer");
        goto cleanup;
    }
    for (y = 0; y < height; ++y) {
        memcpy(dst + (size_t)y * dst_stride, rgba + (size_t)y * src_stride, width * 4);
    }
    lock->lpVtbl->Release(lock);
    lock = NULL;

    hr = factory->lpVtbl->CreateStream(factory, &stream);
    if (FAILED(hr)) {
        report_hr(hr, "CreateStream");
        goto cleanup;
    }
    hr = stream->lpVtbl->InitializeFromFilename(stream, path, GENERIC_WRITE);
    if (FAILED(hr)) {
        report_hr(hr, "IWICStream::InitializeFromFilename");
        goto cleanup;
    }
    hr = factory->lpVtbl->CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (FAILED(hr)) {
        report_hr(hr, "CreateEncoder(png)");
        goto cleanup;
    }
    hr = encoder->lpVtbl->Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        report_hr(hr, "IWICBitmapEncoder::Initialize");
        goto cleanup;
    }
    hr = encoder->lpVtbl->CreateNewFrame(encoder, &frame, &options);
    if (FAILED(hr)) {
        report_hr(hr, "CreateNewFrame");
        goto cleanup;
    }
    if (options != NULL) {
        options->lpVtbl->Release(options);
        options = NULL;
    }
    hr = frame->lpVtbl->Initialize(frame, NULL);
    if (FAILED(hr)) {
        report_hr(hr, "IWICBitmapFrameEncode::Initialize");
        goto cleanup;
    }
    hr = frame->lpVtbl->SetSize(frame, width, height);
    if (FAILED(hr)) {
        report_hr(hr, "IWICBitmapFrameEncode::SetSize");
        goto cleanup;
    }
    hr = frame->lpVtbl->SetPixelFormat(frame, &format);
    if (FAILED(hr)) {
        report_hr(hr, "IWICBitmapFrameEncode::SetPixelFormat");
        goto cleanup;
    }
    hr = frame->lpVtbl->WriteSource(frame, (IWICBitmapSource *)bitmap, NULL);
    if (FAILED(hr)) {
        report_hr(hr, "IWICBitmapFrameEncode::WriteSource");
        goto cleanup;
    }
    hr = frame->lpVtbl->Commit(frame);
    if (FAILED(hr)) {
        report_hr(hr, "IWICBitmapFrameEncode::Commit");
        goto cleanup;
    }
    hr = encoder->lpVtbl->Commit(encoder);
    if (FAILED(hr)) {
        report_hr(hr, "IWICBitmapEncoder::Commit");
        goto cleanup;
    }
    exit_code = 0;

cleanup:
    if (options != NULL) options->lpVtbl->Release(options);
    if (frame != NULL) frame->lpVtbl->Release(frame);
    if (encoder != NULL) encoder->lpVtbl->Release(encoder);
    if (stream != NULL) stream->lpVtbl->Release(stream);
    if (lock != NULL) lock->lpVtbl->Release(lock);
    if (bitmap != NULL) bitmap->lpVtbl->Release(bitmap);
    if (factory != NULL) factory->lpVtbl->Release(factory);
    return exit_code;
}

/* ----------------------------------------------------------- offscreen PNG */

static double g_last_pass_ms = 0.0;
static double g_last_readback_ms = 0.0;
static double g_last_png_ms = 0.0;

static int render_offscreen(Gpu *gpu, int width, int height, const wchar_t *path,
                            double *layout_ms_out, double *frame_ms_out)
{
    WGPUTexture texture = NULL;
    WGPUTextureView view = NULL;
    WGPUBuffer readback = NULL;
    WGPUCommandEncoder encoder = NULL;
    WGPUCommandBuffer commands = NULL;
    MapResult map_result;
    const unsigned char *mapped = NULL;
    unsigned int bytes_per_row;
    uint64_t readback_size;
    const float clear[4] = {0.043137f, 0.054902f, 0.078431f, 1.0f};
    int exit_code = 1;
    double t0, t1;

    t0 = now_ms();
    layout_page((float)width, (float)height);
    t1 = now_ms();
    if (layout_ms_out) *layout_ms_out = t1 - t0;
    build_geometry((float)width, (float)height);
    gpu_upload_geometry(gpu);

    t0 = now_ms();
    {
        WGPUTextureDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-offscreen-target");
        descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
        descriptor.dimension = WGPUTextureDimension_2D;
        descriptor.size.width = (uint32_t)width;
        descriptor.size.height = (uint32_t)height;
        descriptor.size.depthOrArrayLayers = 1;
        descriptor.format = gpu->format;
        descriptor.mipLevelCount = 1;
        descriptor.sampleCount = 1;
        texture = wgpuDeviceCreateTexture(gpu->device, &descriptor);
        if (texture == NULL) {
            fprintf(stderr, "error: offscreen texture creation failed\n");
            goto cleanup;
        }
        view = wgpuTextureCreateView(texture, NULL);
        if (view == NULL) {
            fprintf(stderr, "error: offscreen texture view failed\n");
            goto cleanup;
        }
    }

    bytes_per_row = ((unsigned int)width * 4u + 255u) & ~255u;
    readback_size = (uint64_t)bytes_per_row * (unsigned int)height;
    {
        WGPUBufferDescriptor descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-readback");
        descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        descriptor.size = readback_size;
        readback = wgpuDeviceCreateBuffer(gpu->device, &descriptor);
        if (readback == NULL) {
            fprintf(stderr, "error: readback buffer creation failed\n");
            goto cleanup;
        }
    }

    encoder = wgpuDeviceCreateCommandEncoder(gpu->device, NULL);
    if (encoder == NULL) {
        fprintf(stderr, "error: command encoder creation failed\n");
        goto cleanup;
    }
    gpu_draw(gpu, encoder, view, clear);
    {
        WGPUTexelCopyTextureInfo source;
        WGPUTexelCopyBufferInfo destination;
        WGPUExtent3D extent;
        memset(&source, 0, sizeof(source));
        memset(&destination, 0, sizeof(destination));
        memset(&extent, 0, sizeof(extent));
        source.texture = texture;
        source.mipLevel = 0;
        source.aspect = WGPUTextureAspect_All;
        destination.layout.offset = 0;
        destination.layout.bytesPerRow = bytes_per_row;
        destination.layout.rowsPerImage = (uint32_t)height;
        destination.buffer = readback;
        extent.width = (uint32_t)width;
        extent.height = (uint32_t)height;
        extent.depthOrArrayLayers = 1;
        wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &destination, &extent);
    }
    commands = wgpuCommandEncoderFinish(encoder, NULL);
    if (commands == NULL) {
        fprintf(stderr, "error: command encoder finish failed\n");
        goto cleanup;
    }
    wgpuQueueSubmit(gpu->queue, 1, &commands);
    wgpuCommandEncoderRelease(encoder);
    encoder = NULL;
    g_last_pass_ms = now_ms() - t0;

    memset(&map_result, 0, sizeof(map_result));
    {
        WGPUBufferMapCallbackInfo cb;
        memset(&cb, 0, sizeof(cb));
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = on_map;
        cb.userdata1 = &map_result;
        (void)wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, readback_size, cb);
        if (wait_flag(gpu->instance, &map_result.done, "map readback") != 0) {
            goto cleanup;
        }
    }
    if (map_result.status != WGPUMapAsyncStatus_Success) {
        fprintf(stderr, "error: buffer map failed (status %d): %s\n",
                (int)map_result.status, map_result.message);
        goto cleanup;
    }
    mapped = (const unsigned char *)wgpuBufferGetConstMappedRange(readback, 0, readback_size);
    if (mapped == NULL) {
        fprintf(stderr, "error: mapped range is NULL\n");
        goto cleanup;
    }

    double t_after_map = now_ms();
    g_last_readback_ms = t_after_map - (t0 + g_last_pass_ms);

    if (write_png_rgba(mapped, (unsigned)width, (unsigned)height, bytes_per_row, path) != 0) {
        goto cleanup;
    }
    wgpuBufferUnmap(readback);
    t1 = now_ms();
    g_last_png_ms = t1 - t_after_map;
    if (frame_ms_out) *frame_ms_out = t1 - t0;
    exit_code = 0;

cleanup:
    if (commands != NULL) wgpuCommandBufferRelease(commands);
    if (encoder != NULL) wgpuCommandEncoderRelease(encoder);
    if (readback != NULL) wgpuBufferRelease(readback);
    if (view != NULL) wgpuTextureViewRelease(view);
    if (texture != NULL) wgpuTextureRelease(texture);
    return exit_code;
}

/* --------------------------------------------------------------- reporting */

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
    return kind == R2_NODE_FLEX ? "flex" : (kind == R2_NODE_TEXT ? "text" : "block");
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
    fprintf(f, "{\n  \"generated_by\": \"runtime_r2.c\",\n  \"node_count\": %d,\n", UI_NODE_COUNT);
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
        for (int i = 0; i < UI_NODE_COUNT; ++i) {
            const Rect *r = &g_rects[i];
            fprintf(f, "        {\"id\": %d, \"name\": ", i);
            json_escape(f, kUiNodeNames[i]);
            fprintf(f, ", \"kind\": \"%s\", \"x\": %.2f, \"y\": %.2f, \"w\": %.2f, \"h\": %.2f}%s\n",
                    kind_name(kUiNodes[i].kind), r->x, r->y, r->w, r->h,
                    i + 1 < UI_NODE_COUNT ? "," : "");
        }
        fprintf(f, "      ]\n    }%s\n", s + 1 < count ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    printf("report: wrote %s (%d size(s), %d nodes each)\n", path, count, UI_NODE_COUNT);
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
    for (int i = 0; i < UI_NODE_COUNT; ++i) {
        const char *name = kUiNodeNames[i];
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

/* ------------------------------------------------------------------ states */

static int first_interactive_node(void)
{
    for (int i = 0; i < UI_NODE_COUNT; ++i) {
        if (kUiNodes[i].action != ACTION_NONE) {
            return i;
        }
    }
    return -1;
}

static int run_states(const char *prefix, int node, int width, int height)
{
    Gpu gpu;
    char path[MAX_PATH];
    double layout_ms = 0.0, frame_ms = 0.0;
    const char *suffixes[4] = {"normal", "hover", "press", "selected"};

    if (gpu_init(&gpu, NULL, 0, WGPUTextureFormat_RGBA8Unorm, "r2-states-device") != 0) {
        gpu_release(&gpu);
        return 1;
    }
    if (node < 0 || node >= UI_NODE_COUNT || kUiNodes[node].action == ACTION_NONE) {
        fprintf(stderr, "error: node %d is not interactive\n", node);
        gpu_release(&gpu);
        return 1;
    }

    g_hover = g_press = g_selected = -1;
    for (int state = 0; state < 4; ++state) {
        g_hover = (state == 1) ? node : -1;
        g_press = (state == 2) ? node : -1;
        g_selected = (state == 3) ? node : -1;
        snprintf(path, sizeof(path), "%s_%s.png", prefix, suffixes[state]);
        wchar_t wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
        if (render_offscreen(&gpu, width, height, wide, &layout_ms, &frame_ms) != 0) {
            gpu_release(&gpu);
            return 1;
        }
        printf("state %-8s node=%d %-40s layout=%.3fms frame=%.3fms -> %s\n",
               suffixes[state], node, kUiNodeNames[node], layout_ms, frame_ms, path);
    }

    /* Small machine-readable state report for verify.py. */
    snprintf(path, sizeof(path), "%s_states.json", prefix);
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        const Rect *r = &g_rects[node];
        fprintf(f, "{\n  \"node\": %d,\n  \"name\": ", node);
        json_escape(f, kUiNodeNames[node]);
        fprintf(f, ",\n  \"rect\": {\"x\": %.2f, \"y\": %.2f, \"w\": %.2f, \"h\": %.2f},\n",
                r->x, r->y, r->w, r->h);
        fprintf(f, "  \"colors\": {\n");
        fprintf(f, "    \"normal\": \"#%06X\",\n", kUiNodes[node].background & 0xFFFFFFu);
        fprintf(f, "    \"hover\": \"#%06X\",\n", kUiNodes[node].hover_bg & 0xFFFFFFu);
        fprintf(f, "    \"press\": \"#%06X\",\n", kUiNodes[node].press_bg & 0xFFFFFFu);
        fprintf(f, "    \"selected\": \"#%06X\"\n", kUiNodes[node].selected_bg & 0xFFFFFFu);
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

    gpu_release(&gpu);
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
    for (int i = 0; i < UI_NODE_COUNT; ++i) {
        if (strstr(kUiNodeNames[i], fragment)) {
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
    printf(" vertices=%d glyph_vertices=%d\n", g_vertex_count, g_glyph_vertex_count);
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
        if (wparam == VK_ESCAPE) {
            g_running = 0;
            return 0;
        }
        return 0;
    case WM_MOUSEMOVE: {
        int x = (int)(short)LOWORD(lparam);
        int y = (int)(short)HIWORD(lparam);
        int hit = hit_test((float)x, (float)y);
        if (hit != g_hover) {
            g_hover = hit;
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
            g_hover = -1;
            g_need_geometry = 1;
        }
        return 0;
    case WM_LBUTTONDOWN: {
        int x = (int)(short)LOWORD(lparam);
        int y = (int)(short)HIWORD(lparam);
        int hit = hit_test((float)x, (float)y);
        g_press = hit;
        SetCapture(hwnd);
        g_need_geometry = 1;
        return 0;
    }
    case WM_LBUTTONUP: {
        int x = (int)(short)LOWORD(lparam);
        int y = (int)(short)HIWORD(lparam);
        int hit = hit_test((float)x, (float)y);
        if (g_press >= 0 && hit == g_press && kUiNodes[g_press].action == ACTION_SELECT) {
            g_selected = g_press;
            printf("click: node=%d %s -> selected\n", g_press, kUiNodeNames[g_press]);
            fflush(stdout);
        }
        g_press = -1;
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
    Gpu gpu;
    HWND hwnd = NULL;
    WGPUSurface surface = NULL;
    WGPUSurfaceConfiguration surface_config;
    int exit_code = 1;
    int frames = 0;
    double t_start, t_ready;
    WGPUAdapterInfo info;

    memset(&gpu, 0, sizeof(gpu));

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

    {
        /* Window path: instance -> surface -> adapter -> device, because the
         * surface must be created from the instance that stays alive. */
        WGPUInstance instance = wgpuCreateInstance(NULL);
        WGPUSurfaceSourceWindowsHWND source;
        WGPUSurfaceDescriptor descriptor;
        AdapterResult adapter_result;
        DeviceResult device_result;
        WGPUShaderModule shader;
        WGPUBufferDescriptor buffer_desc;

        if (instance == NULL) {
            fprintf(stderr, "error: wgpuCreateInstance failed\n");
            goto cleanup;
        }
        gpu.instance = instance;

        memset(&source, 0, sizeof(source));
        source.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
        source.hinstance = GetModuleHandleW(NULL);
        source.hwnd = hwnd;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.label = sv("r2-surface");
        descriptor.nextInChain = &source.chain;
        surface = wgpuInstanceCreateSurface(instance, &descriptor);
        if (surface == NULL) {
            fprintf(stderr, "error: wgpuInstanceCreateSurface failed\n");
            goto cleanup;
        }

        memset(&adapter_result, 0, sizeof(adapter_result));
        {
            WGPURequestAdapterOptions options;
            WGPURequestAdapterCallbackInfo cb;
            memset(&options, 0, sizeof(options));
            memset(&cb, 0, sizeof(cb));
            options.powerPreference = WGPUPowerPreference_HighPerformance;
            options.featureLevel = WGPUFeatureLevel_Core;
            options.compatibleSurface = surface;
            cb.mode = WGPUCallbackMode_AllowProcessEvents;
            cb.callback = on_adapter;
            cb.userdata1 = &adapter_result;
            (void)wgpuInstanceRequestAdapter(instance, &options, cb);
            if (wait_flag(instance, &adapter_result.done, "request adapter") != 0) {
                goto cleanup;
            }
        }
        if (adapter_result.status != WGPURequestAdapterStatus_Success || adapter_result.adapter == NULL) {
            fprintf(stderr, "error: no adapter: %s\n", adapter_result.message);
            goto cleanup;
        }
        gpu.adapter = adapter_result.adapter;

        memset(&info, 0, sizeof(info));
        if (wgpuAdapterGetInfo(gpu.adapter, &info) == WGPUStatus_Success) {
            printf("adapter: backend=%s (%d) type=%s vendorID=0x%04X deviceID=0x%04X\n",
                   backend_name(info.backendType), (int)info.backendType,
                   adapter_type_name(info.adapterType), info.vendorID, info.deviceID);
            print_view("adapter: device=", info.device);
            printf("\n");
            print_view("adapter: description=", info.description);
            printf("\n");
            fflush(stdout);
        }

        memset(&device_result, 0, sizeof(device_result));
        {
            WGPUDeviceDescriptor device_desc;
            WGPURequestDeviceCallbackInfo cb;
            memset(&device_desc, 0, sizeof(device_desc));
            memset(&cb, 0, sizeof(cb));
            device_desc.label = sv("r2-window-device");
            device_desc.uncapturedErrorCallbackInfo.callback = on_uncaptured_error;
            cb.mode = WGPUCallbackMode_AllowProcessEvents;
            cb.callback = on_device;
            cb.userdata1 = &device_result;
            (void)wgpuAdapterRequestDevice(gpu.adapter, &device_desc, cb);
            if (wait_flag(instance, &device_result.done, "request device") != 0) {
                goto cleanup;
            }
        }
        if (device_result.status != WGPURequestDeviceStatus_Success || device_result.device == NULL) {
            fprintf(stderr, "error: no device: %s\n", device_result.message);
            goto cleanup;
        }
        gpu.device = device_result.device;
        gpu.queue = wgpuDeviceGetQueue(gpu.device);

        {
            WGPUSurfaceCapabilities caps;
            memset(&caps, 0, sizeof(caps));
            if (wgpuSurfaceGetCapabilities(surface, gpu.adapter, &caps) != WGPUStatus_Success ||
                caps.formatCount == 0) {
                fprintf(stderr, "error: surface capabilities failed\n");
                goto cleanup;
            }
            gpu.format = caps.formats[0];
            for (size_t i = 0; i < caps.formatCount; ++i) {
                if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm ||
                    caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
                    gpu.format = caps.formats[i];
                    break;
                }
            }
            printf("surface: formats=%zu chosenFormat=%d\n", caps.formatCount, (int)gpu.format);
        }

        memset(&surface_config, 0, sizeof(surface_config));
        surface_config.device = gpu.device;
        surface_config.format = gpu.format;
        surface_config.usage = WGPUTextureUsage_RenderAttachment;
        surface_config.width = (uint32_t)g_window_width;
        surface_config.height = (uint32_t)g_window_height;
        surface_config.presentMode = WGPUPresentMode_Fifo;
        surface_config.alphaMode = WGPUCompositeAlphaMode_Auto;
        wgpuSurfaceConfigure(surface, &surface_config);
        g_need_configure = 0;

        {
            WGPUShaderModuleDescriptor shader_desc;
            WGPUShaderSourceWGSL wgsl;
            memset(&shader_desc, 0, sizeof(shader_desc));
            memset(&wgsl, 0, sizeof(wgsl));
            wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
            wgsl.code = sv(kShaderWGSL);
            shader_desc.label = sv("r2-window-shader");
            shader_desc.nextInChain = &wgsl.chain;
            shader = wgpuDeviceCreateShaderModule(gpu.device, &shader_desc);
            if (shader == NULL) {
                fprintf(stderr, "error: shader creation failed\n");
                goto cleanup;
            }
            gpu.shader = shader;
        }

        {
            WGPUVertexAttribute attributes[2];
            WGPUVertexBufferLayout buffer_layout;
            WGPUColorTargetState target;
            WGPUBlendState blend;
            WGPUFragmentState fragment;
            WGPUVertexState vertex;
            WGPUPrimitiveState primitive;
            WGPUMultisampleState multisample;
            WGPURenderPipelineDescriptor pipeline_desc;

            memset(attributes, 0, sizeof(attributes));
            attributes[0].format = WGPUVertexFormat_Float32x2;
            attributes[0].offset = 0;
            attributes[0].shaderLocation = 0;
            attributes[1].format = WGPUVertexFormat_Unorm8x4;
            attributes[1].offset = 8;
            attributes[1].shaderLocation = 1;

            memset(&buffer_layout, 0, sizeof(buffer_layout));
            buffer_layout.stepMode = WGPUVertexStepMode_Vertex;
            buffer_layout.arrayStride = sizeof(Vertex);
            buffer_layout.attributeCount = (uint32_t)ARRAY_LEN(attributes);
            buffer_layout.attributes = attributes;

            memset(&blend, 0, sizeof(blend));
            blend.color.operation = WGPUBlendOperation_Add;
            blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
            blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
            blend.alpha.operation = WGPUBlendOperation_Add;
            blend.alpha.srcFactor = WGPUBlendFactor_One;
            blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

            memset(&target, 0, sizeof(target));
            target.format = gpu.format;
            target.blend = &blend;
            target.writeMask = WGPUColorWriteMask_All;

            memset(&fragment, 0, sizeof(fragment));
            fragment.module = gpu.shader;
            fragment.entryPoint = sv("fs_main");
            fragment.targetCount = 1;
            fragment.targets = &target;

            memset(&vertex, 0, sizeof(vertex));
            vertex.module = gpu.shader;
            vertex.entryPoint = sv("vs_main");
            vertex.bufferCount = 1;
            vertex.buffers = &buffer_layout;

            memset(&primitive, 0, sizeof(primitive));
            primitive.topology = WGPUPrimitiveTopology_TriangleList;
            primitive.frontFace = WGPUFrontFace_CCW;
            primitive.cullMode = WGPUCullMode_None;

            memset(&multisample, 0, sizeof(multisample));
            multisample.count = 1;
            multisample.mask = 0xFFFFFFFFu;

            memset(&pipeline_desc, 0, sizeof(pipeline_desc));
            pipeline_desc.label = sv("r2-window-pipeline");
            pipeline_desc.layout = NULL;
            pipeline_desc.vertex = vertex;
            pipeline_desc.primitive = primitive;
            pipeline_desc.depthStencil = NULL;
            pipeline_desc.multisample = multisample;
            pipeline_desc.fragment = &fragment;
            gpu.pipeline = wgpuDeviceCreateRenderPipeline(gpu.device, &pipeline_desc);
            if (gpu.pipeline == NULL) {
                fprintf(stderr, "error: pipeline creation failed\n");
                goto cleanup;
            }
        }

        gpu.vertex_capacity = (size_t)UI_NODE_COUNT * 6u * sizeof(Vertex);
        memset(&buffer_desc, 0, sizeof(buffer_desc));
        buffer_desc.label = sv("r2-window-vertices");
        buffer_desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        buffer_desc.size = gpu.vertex_capacity;
        gpu.vertex_buffer = wgpuDeviceCreateBuffer(gpu.device, &buffer_desc);
        if (gpu.vertex_buffer == NULL) {
            fprintf(stderr, "error: vertex buffer creation failed\n");
            goto cleanup;
        }
        if (gpu_create_text(&gpu) != 0) {
            fprintf(stderr, "error: text pipeline creation failed\n");
            goto cleanup;
        }
    }

    t_ready = now_ms();

    while (g_running) {
        MSG msg;
        WGPUSurfaceTexture surface_texture;
        WGPUTextureView view;
        WGPUCommandEncoder encoder;
        WGPUCommandBuffer commands;
        const float clear[4] = {0.043137f, 0.054902f, 0.078431f, 1.0f};

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
            surface_config.width = (uint32_t)g_window_width;
            surface_config.height = (uint32_t)g_window_height;
            wgpuSurfaceConfigure(surface, &surface_config);
            g_need_configure = 0;
            printf("surface: reconfigured %dx%d\n", g_window_width, g_window_height);
            fflush(stdout);
        }
        gpu_upload_geometry(&gpu);

        memset(&surface_texture, 0, sizeof(surface_texture));
        wgpuSurfaceGetCurrentTexture(surface, &surface_texture);
        if (surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
            surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
            wgpuSurfaceConfigure(surface, &surface_config);
            continue;
        }
        if (surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
            surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
            if (surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Timeout) {
                Sleep(1);
                continue;
            }
            fprintf(stderr, "error: getCurrentTexture status %d\n", (int)surface_texture.status);
            break;
        }
        double t_cpu_frame = now_ms();
        view = wgpuTextureCreateView(surface_texture.texture, NULL);
        if (view == NULL) {
            wgpuTextureRelease(surface_texture.texture);
            break;
        }
        encoder = wgpuDeviceCreateCommandEncoder(gpu.device, NULL);
        gpu_draw(&gpu, encoder, view, clear);
        commands = wgpuCommandEncoderFinish(encoder, NULL);
        if (commands == NULL) {
            wgpuCommandEncoderRelease(encoder);
            wgpuTextureViewRelease(view);
            wgpuTextureRelease(surface_texture.texture);
            break;
        }
        wgpuQueueSubmit(gpu.queue, 1, &commands);
        wgpuCommandBufferRelease(commands);
        wgpuCommandEncoderRelease(encoder);
        g_cpu_frame_total += now_ms() - t_cpu_frame;
        (void)wgpuSurfacePresent(surface);
        wgpuTextureViewRelease(view);
        wgpuTextureRelease(surface_texture.texture);

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
         * are dispatched through the window procedure and hit testing. */
        if (g_input_demo) {
            int node = first_interactive_node();
            if (node >= 0) {
                int cx = (int)(g_rects[node].x + g_rects[node].w * 0.5f);
                int cy = (int)(g_rects[node].y + g_rects[node].h * 0.5f);
                LPARAM point = MAKELPARAM(cx, cy);
                if (g_frame_index == 8) {
                    SendMessageW(hwnd, WM_MOUSEMOVE, 0, point);
                    printf("input-demo: WM_MOUSEMOVE (%d,%d) -> hover=%d %s\n",
                           cx, cy, g_hover, g_hover >= 0 ? kUiNodeNames[g_hover] : "(none)");
                    fflush(stdout);
                } else if (g_frame_index == 9) {
                    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
                    printf("input-demo: WM_LBUTTONDOWN -> press=%d %s\n",
                           g_press, g_press >= 0 ? kUiNodeNames[g_press] : "(none)");
                    fflush(stdout);
                } else if (g_frame_index == 10) {
                    SendMessageW(hwnd, WM_LBUTTONUP, 0, point);
                    printf("input-demo: WM_LBUTTONUP -> selected=%d %s\n",
                           g_selected, g_selected >= 0 ? kUiNodeNames[g_selected] : "(none)");
                    fflush(stdout);
                } else if (g_frame_index == 12) {
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
    if (surface != NULL) wgpuSurfaceRelease(surface);
    gpu_release(&gpu);
    if (hwnd != NULL) DestroyWindow(hwnd);
    return exit_code;
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
    int mode_render = 0, mode_states = 0, mode_report = 0, mode_dump = 0;
    int render_w = UI_VIEWPORT_WIDTH, render_h = UI_VIEWPORT_HEIGHT;
    const char *render_path = "out.png";
    const char *states_prefix = "state";
    const char *report_path = "layout_report.json";
    int state_node = -1;
    int width_arg = 0, height_arg = 0;
    int default_sizes = 1;
    int widths[8] = {960, 720, 560, 0, 0, 0, 0, 0};
    int heights[8] = {640, 640, 640, 0, 0, 0, 0, 0};
    int size_count = 3;

    setvbuf(stdout, NULL, _IONBF, 0);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    SetProcessDPIAware();
    wgpuSetLogLevel(WGPULogLevel_Warn);
    wgpuSetLogCallback(on_log, NULL);

    build_hierarchy();

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
            g_hover = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc) {
            g_press = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--selected") == 0 && i + 1 < argc) {
            g_selected = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (parse_size(argv[++i], &render_w, &render_h) != 0) {
                fprintf(stderr, "error: bad --size\n");
                return 1;
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
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width_arg = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height_arg = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: runtime_r2.exe [--render W H out.png] [--states PREFIX] "
                   "[--report FILE] [--frames N] [--resize-demo]\n");
            CoUninitialize();
            return 0;
        } else {
            fprintf(stderr, "warning: ignoring argument '%s'\n", argv[i]);
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
        printf("nodes: %d\n", UI_NODE_COUNT);
        for (int i = 0; i < UI_NODE_COUNT; ++i) {
            const UiNode *n = &kUiNodes[i];
            printf("[%2d] parent=%2d %-42s kind=%s dir=%s justify=%d align=%d wrap=%d "
                   "w=%.1f h=%.1f wpct=%.1f hpct=%.1f gap=%.1f bg=0x%08X action=%d\n",
                   i, n->parent, kUiNodeNames[i], kind_name(n->kind),
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

    if (mode_states) {
        if (state_node < 0) {
            state_node = first_interactive_node();
        }
        exit_code = run_states(states_prefix, state_node, render_w, render_h);
        goto done;
    }

    if (mode_render) {
        Gpu gpu;
        double layout_ms = 0.0, frame_ms = 0.0;
        wchar_t wide[MAX_PATH];
        if (render_w <= 0 || render_h <= 0) {
            fprintf(stderr, "error: bad render size %dx%d\n", render_w, render_h);
            return 1;
        }
        if (gpu_init(&gpu, NULL, 0, WGPUTextureFormat_RGBA8Unorm, "r2-offscreen-device") != 0) {
            gpu_release(&gpu);
            return 1;
        }
        MultiByteToWideChar(CP_ACP, 0, render_path, -1, wide, MAX_PATH);
        if (render_offscreen(&gpu, render_w, render_h, wide, &layout_ms, &frame_ms) != 0) {
            gpu_release(&gpu);
            return 1;
        }
        gpu_release(&gpu);
        print_layout_summary((float)render_w, (float)render_h);
        printf("render: %s %dx%d nodes=%d vertices=%d glyph_vertices=%d "
               "layout=%.3fms frame=%.3fms\n",
               render_path, render_w, render_h, UI_NODE_COUNT, g_vertex_count,
               g_glyph_vertex_count, layout_ms, frame_ms);
        printf("timings: layout=%.3fms | frame(build+upload+pass+readback+png)=%.3fms\n",
               layout_ms, frame_ms);
        printf("timings: gpu(encode+submit)=%.3fms | readback(map)=%.3fms | png=%.3fms\n",
               g_last_pass_ms, g_last_readback_ms, g_last_png_ms);
        exit_code = 0;
        goto done;
    }

    exit_code = run_window();

done:
    CoUninitialize();
    return exit_code;
}
