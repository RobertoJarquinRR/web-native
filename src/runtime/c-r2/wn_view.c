/*
 * wn_view.c - embedded mode: core + renderer attached to a host-provided
 * native window. The host owns the loop and calls wn_view_render() on demand.
 */
#include "webnative.h"
#include "wn_view.h"
#include "wn_core.h"
#include "wn_renderer.h"
#include "wn_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

struct wn_view {
    wn_core *core;
    const wn_renderer_ops *ops;
    void *renderer;
    void *native_window;
    int width;
    int height;
    float dpi_scale;
    int loaded;
    int closed;
    int geometry_dirty;
    int upload_dirty;
    int have_frame;
    double created_ms;
    double first_frame_ms;
    char *html;
    char *base_url;
    char *css;
    char *value_scratch;      /* wn_view_get_value result, freed on next call */
    wn_event_cb callback;
    void *userdata;
    wn_view_stats stats;
};

static void view_mark_geometry(wn_view *view)
{
    view->geometry_dirty = 1;
}

static void view_resize_internal(wn_view *view, int width, int height, float dpi)
{
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    view->width = width;
    view->height = height;
    if (dpi > 0.0f) {
        view->dpi_scale = dpi;
    }
    wn_core_resize(view->core, width, height);
    if (view->ops != NULL && view->renderer != NULL) {
        view->ops->resize(view->renderer, width, height);
    }
    view_mark_geometry(view);
}

static void set_renderer_error(wn_view *view, const char *fallback)
{
    const char *message = NULL;
    if (view->ops != NULL && view->renderer != NULL && view->ops->last_error != NULL) {
        message = view->ops->last_error(view->renderer);
    }
    wn_set_error("%s", message != NULL ? message : fallback);
}

/* ---------------------------------------------------- clipboard (Win32) */
/*
 * The core is platform independent: copy/cut/paste only move text between the
 * value buffer and caller-owned strings. The OS clipboard lives here, so both
 * the embedded host (WN_EVENT_KEY_DOWN through wn_view_event) and the
 * standalone window get Ctrl+C/X/V without extra ABI.
 */
static void view_clipboard_set(const char *text)
{
#if defined(_WIN32)
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
#else
    (void)text;
#endif
}

static int view_clipboard_get(char *out, int capacity)
{
    int copied = 0;
    if (out == NULL || capacity <= 0) {
        return 0;
    }
    out[0] = '\0';
#if defined(_WIN32)
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
#endif
    return copied;
}

/* Keyboard editing for the focused input. Ctrl+A/C/X/V use the OS clipboard;
 * everything else is core logic. Returns the focused node (or -1). */
static int view_input_key(wn_view *view, const wn_event *event,
                          int *value_changed, int *visual_changed)
{
    char buffer[256];
    int node;
    int copied;
    if (value_changed != NULL) {
        *value_changed = 0;
    }
    if (visual_changed != NULL) {
        *visual_changed = 0;
    }
    node = wn_core_input_focused(view->core);
    if (node < 0) {
        return -1;
    }
    if ((event->modifiers & WN_MOD_CTRL) != 0u) {
        if (event->key == WN_KEY_A) {
            return wn_core_input_select_all(view->core, visual_changed);
        }
        if (event->key == WN_KEY_C) {
            copied = wn_core_input_copy(view->core, buffer, (int)sizeof(buffer));
            if (copied > 0) {
                view_clipboard_set(buffer);
            }
            return node;
        }
        if (event->key == WN_KEY_X) {
            copied = wn_core_input_cut(view->core, buffer, (int)sizeof(buffer),
                                       value_changed, visual_changed);
            if (copied > 0) {
                view_clipboard_set(buffer);
            }
            return node;
        }
        if (event->key == WN_KEY_V) {
            if (view_clipboard_get(buffer, (int)sizeof(buffer)) > 0) {
                return wn_core_input_paste(view->core, buffer,
                                           value_changed, visual_changed);
            }
            return node;
        }
        return -1;
    }
    return wn_core_input_key(view->core, event->key, event->modifiers,
                             value_changed, visual_changed);
}

wn_view *wn_view_create(void *native_window_handle, int32_t width, int32_t height,
                        uint32_t flags)
{
    wn_view *view;
    wn_surface_desc surface;

    wn_clear_error();
    if (native_window_handle == NULL) {
        wn_set_error("wn_view_create: native_window_handle is NULL");
        return NULL;
    }
    if (width <= 0 || height <= 0) {
        wn_set_error("wn_view_create: width/height must be positive (got %dx%d)",
                     (int)width, (int)height);
        return NULL;
    }
    if (flags != WN_FLAG_NONE) {
        wn_set_error("wn_view_create: flags must be 0 (got 0x%X)", (unsigned)flags);
        return NULL;
    }

    view = (wn_view *)calloc(1, sizeof(*view));
    if (view == NULL) {
        wn_set_error("wn_view_create: out of memory");
        return NULL;
    }
    view->core = wn_core_create(wn_document_builtin());
    if (view->core == NULL) {
        wn_set_error("wn_view_create: core creation failed");
        free(view);
        return NULL;
    }
    view->ops = wn_renderer_wgpu();
    view->native_window = native_window_handle;
    view->width = width;
    view->height = height;
    view->dpi_scale = 1.0f;
    view->created_ms = wn_now_ms();

    memset(&surface, 0, sizeof(surface));
    surface.kind = WN_SURFACE_HWND;
    surface.handle = native_window_handle;
    surface.instance_handle = NULL;
    surface.width = width;
    surface.height = height;
    if (view->ops == NULL || view->ops->init == NULL ||
        view->ops->init(&view->renderer, &surface, width, height) != 0) {
        const char *message = view->ops != NULL && view->ops->last_error != NULL &&
                                      view->renderer != NULL
                                  ? view->ops->last_error(view->renderer)
                                  : NULL;
        wn_set_error("wn_view_create: renderer init failed: %s",
                     message != NULL ? message : "unknown");
        if (view->renderer != NULL && view->ops->destroy != NULL) {
            view->ops->destroy(view->renderer);
        }
        wn_core_destroy(view->core);
        free(view);
        return NULL;
    }
    wn_core_resize(view->core, width, height);
    view->geometry_dirty = 1;
    view->upload_dirty = 1;
    return view;
}

wn_status wn_view_load(wn_view *view, const char *html_utf8,
                       const char *base_url, const char *css_utf8)
{
    char *html;
    char *base = NULL;
    char *css = NULL;

    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_view_load: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (html_utf8 == NULL) {
        wn_set_error("wn_view_load: html_utf8 is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    html = wn_strdup_(html_utf8);
    if (html == NULL) {
        wn_set_error("wn_view_load: out of memory copying html");
        return WN_ERR_OUT_OF_MEMORY;
    }
    if (base_url != NULL) {
        base = wn_strdup_(base_url);
        if (base == NULL) {
            wn_free_(html);
            wn_set_error("wn_view_load: out of memory copying base_url");
            return WN_ERR_OUT_OF_MEMORY;
        }
    }
    if (css_utf8 != NULL) {
        css = wn_strdup_(css_utf8);
        if (css == NULL) {
            wn_free_(html);
            wn_free_(base);
            wn_set_error("wn_view_load: out of memory copying css");
            return WN_ERR_OUT_OF_MEMORY;
        }
    }

    wn_free_(view->html);
    wn_free_(view->base_url);
    wn_free_(view->css);
    view->html = html;
    view->base_url = base;
    view->css = css;
    view->loaded = 1;
    view->closed = 0;

    /* v0 attaches the compiled R2 document. The strings are retained as the
     * document source/diagnostics until a real runtime parser exists. */
    wn_core_resize(view->core, view->width, view->height);
    wn_core_relayout(view->core);
    view->geometry_dirty = 1;
    view->upload_dirty = 1;
    return WN_OK;
}

wn_status wn_view_resize(wn_view *view, int32_t width, int32_t height, float dpi_scale)
{
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_view_resize: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (width < 0 || height < 0 || width > 16384 || height > 16384) {
        wn_set_error("wn_view_resize: invalid size %dx%d", (int)width, (int)height);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_resize_internal(view, width, height, dpi_scale);
    return WN_OK;
}

wn_status wn_view_event(wn_view *view, const wn_event *event)
{
    wn_event out;
    const char *target = NULL;
    int hit = -1;
    int changed = 0;
    int input_value_changed = 0;
    int input_visual_changed = 0;

    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_view_event: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (event == NULL) {
        wn_set_error("wn_view_event: event is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    out = *event;
    out.target_id = NULL;

    switch (event->type) {
    case WN_EVENT_POINTER_MOVE: {
        int previous_hover = wn_core_hover(view->core);
        hit = wn_core_pointer_move(view->core, event->x, event->y);
        changed = hit != previous_hover;
        break;
    }
    case WN_EVENT_POINTER_DOWN:
        hit = wn_core_pointer_down(view->core, event->x, event->y);
        changed = 1;
        break;
    case WN_EVENT_POINTER_UP:
        hit = wn_core_pointer_up(view->core, event->x, event->y);
        changed = 1;
        break;
    case WN_EVENT_WHEEL:
        hit = wn_core_wheel(view->core, event->x, event->y,
                            event->delta_x, event->delta_y);
        changed = hit >= 0;
        break;
    case WN_EVENT_CHAR:
        hit = wn_core_input_char(view->core, event->codepoint,
                                 &input_value_changed, &input_visual_changed);
        changed = input_visual_changed;
        break;
    case WN_EVENT_KEY_DOWN:
        hit = view_input_key(view, event, &input_value_changed,
                             &input_visual_changed);
        changed = input_visual_changed;
        break;
    case WN_EVENT_FOCUS_OUT:
        if (wn_core_blur_input(view->core) == 0) {
            changed = 1;
        }
        break;
    case WN_EVENT_RESIZE:
        view_resize_internal(view, event->width, event->height, 0.0f);
        break;
    case WN_EVENT_CLOSE:
        view->closed = 1;
        break;
    case WN_EVENT_KEY_UP:
    case WN_EVENT_FOCUS_IN:
    default:
        break;
    }
    if (hit >= 0) {
        target = wn_core_node_name(view->core, hit);
    }
    out.target_id = target;
    if (changed) {
        view_mark_geometry(view);
    }
    if (view->callback != NULL) {
        view->callback(view->userdata, &out);
        if (input_value_changed) {
            wn_event semantic = out;
            semantic.type = WN_EVENT_INPUT;
            semantic.target_id = target;
            view->callback(view->userdata, &semantic);
        }
    }
    return WN_OK;
}

wn_status wn_view_render(wn_view *view)
{
    const wn_display_list *list;
    wn_renderer_stats renderer_stats;
    double t0;
    double t1;
    double t_upload;
    double cpu_ms = 0.0;

    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_view_render: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (!view->loaded) {
        wn_set_error("wn_view_render: document not loaded (call wn_view_load first)");
        return WN_ERR_STATE;
    }
    if (view->width <= 0 || view->height <= 0) {
        return WN_OK;
    }

    t0 = wn_now_ms();
    if (wn_core_dirty(view->core)) {
        /* Host mutations mark the core dirty; make sure the display list is
         * rebuilt even when the setter did not touch view->geometry_dirty. */
        view->geometry_dirty = 1;
    }
    if (wn_core_needs_layout(view->core)) {
        wn_core_relayout(view->core);
        view->geometry_dirty = 1;
    }
    if (view->geometry_dirty) {
        wn_core_build_display_list(view->core);
        view->geometry_dirty = 0;
        view->upload_dirty = 1;
    }
    list = wn_core_display_list(view->core);
    t_upload = wn_now_ms();
    if (view->upload_dirty) {
        if (view->ops->upload(view->renderer, list) != 0) {
            set_renderer_error(view, "display list upload failed");
            return WN_ERR_GPU;
        }
        view->upload_dirty = 0;
    }
    cpu_ms += wn_now_ms() - t_upload;

    if (view->ops->render(view->renderer, list) != 0) {
        set_renderer_error(view, "render/present failed");
        return WN_ERR_GPU;
    }
    if (view->ops->get_stats != NULL) {
        memset(&renderer_stats, 0, sizeof(renderer_stats));
        view->ops->get_stats(view->renderer, &renderer_stats);
        cpu_ms += renderer_stats.last_cpu_ms;
    }
    t1 = wn_now_ms();
    if (!view->have_frame) {
        view->have_frame = 1;
        view->first_frame_ms = t1;
        view->stats.startup_ms = t1 - view->created_ms;
    }
    view->stats.frames += 1;
    view->stats.frame_total_ms += t1 - t0;
    view->stats.cpu_frame_total_ms += cpu_ms;
    view->stats.last_render_ms = t1 - t0;
    view->stats.last_layout_ms = wn_core_last_layout_ms(view->core);
    return WN_OK;
}

wn_status wn_view_on_event(wn_view *view, wn_event_cb callback, void *userdata)
{
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_view_on_event: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    view->callback = callback;
    view->userdata = userdata;
    return WN_OK;
}

void wn_view_destroy(wn_view *view)
{
    if (view == NULL) {
        return;
    }
    wn_clear_error();
    if (view->ops != NULL && view->renderer != NULL) {
        view->ops->destroy(view->renderer);
    }
    wn_core_destroy(view->core);
    wn_free_(view->html);
    wn_free_(view->base_url);
    wn_free_(view->css);
    wn_free_(view->value_scratch);
    free(view);
}

int wn_view_needs_render(const wn_view *view)
{
    if (view == NULL || !view->loaded) {
        return 0;
    }
    if (view->width <= 0 || view->height <= 0) {
        return 0;
    }
    if (!view->have_frame || view->geometry_dirty || view->upload_dirty) {
        return 1;
    }
    if (wn_core_needs_layout(view->core) || wn_core_dirty(view->core)) {
        return 1;
    }
    return 0;
}

int wn_view_get_stats(const wn_view *view, wn_view_stats *stats)
{
    if (view == NULL || stats == NULL) {
        return 1;
    }
    *stats = view->stats;
    return 0;
}

const char *wn_view_backend(const wn_view *view)
{
    static char backend[32];
    wn_renderer_info info;
    if (view == NULL || view->ops == NULL || view->renderer == NULL ||
        view->ops->get_info == NULL) {
        return NULL;
    }
    memset(&info, 0, sizeof(info));
    view->ops->get_info(view->renderer, &info);
    memcpy(backend, info.backend, sizeof(backend));
    backend[sizeof(backend) - 1] = '\0';
    return backend[0] != '\0' ? backend : NULL;
}

const char *wn_view_device(const wn_view *view)
{
    static char device[256];
    wn_renderer_info info;
    if (view == NULL || view->ops == NULL || view->renderer == NULL ||
        view->ops->get_info == NULL) {
        return NULL;
    }
    memset(&info, 0, sizeof(info));
    view->ops->get_info(view->renderer, &info);
    memcpy(device, info.device, sizeof(device));
    device[sizeof(device) - 1] = '\0';
    return device[0] != '\0' ? device : NULL;
}

int wn_view_render_width(const wn_view *view)
{
    return view != NULL ? view->width : 0;
}

int wn_view_render_height(const wn_view *view)
{
    return view != NULL ? view->height : 0;
}

wn_status wn_view_set_text(wn_view *view, const char *id, const char *text_utf8)
{
    int node;
    int result;
    wn_clear_error();
    if (view == NULL || id == NULL || text_utf8 == NULL) {
        wn_set_error("wn_view_set_text: view, id and text are required");
        return WN_ERR_INVALID_ARGUMENT;
    }
    node = wn_core_find_node(view->core, id);
    if (node < 0) {
        wn_set_error("wn_view_set_text: node '%s' not found", id);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (!wn_core_node_is_text(view->core, node)) {
        wn_set_error("wn_view_set_text: node '%s' is not a text node", id);
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_set_text(view->core, node, text_utf8);
    if (result == 1) {
        wn_set_error("wn_view_set_text: '%s' cannot be rendered with the pre-shaped "
                     "glyphs of '%s' (runtime shaping is out of v0)", text_utf8, id);
        return WN_ERR_UNSUPPORTED;
    }
    if (result != 0) {
        wn_set_error("wn_view_set_text: invalid text node '%s'", id);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_set_class(wn_view *view, const char *id, const char *class_name,
                            int enabled)
{
    int node;
    int result;
    wn_clear_error();
    if (view == NULL || id == NULL || class_name == NULL || class_name[0] == '\0') {
        wn_set_error("wn_view_set_class: view, id and class_name are required");
        return WN_ERR_INVALID_ARGUMENT;
    }
    node = wn_core_find_node(view->core, id);
    if (node < 0) {
        wn_set_error("wn_view_set_class: node '%s' not found", id);
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_set_class(view->core, node, class_name, enabled);
    if (result != 0) {
        wn_set_error("wn_view_set_class: cannot %s class '%s' on '%s'",
                     enabled ? "add" : "remove", class_name, id);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

/* ---------------------------------------------------- dynamic UI (ABI v3) */

static int view_node_index(const wn_view *view, wn_node_id node)
{
    int index;
    if (view == NULL || node == WN_NODE_INVALID) {
        return -1;
    }
    index = (int)(node - 1u);
    if (!wn_core_node_is_alive(view->core, index)) {
        return -1;
    }
    return index;
}

static wn_node_id view_node_handle(int index)
{
    return index >= 0 ? (wn_node_id)((uint64_t)index + 1u) : WN_NODE_INVALID;
}

wn_node_id wn_view_find(wn_view *view, const char *name)
{
    int node;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_app_find: view is NULL");
        return WN_NODE_INVALID;
    }
    if (name == NULL || name[0] == '\0') {
        wn_set_error("wn_app_find: name is NULL or empty");
        return WN_NODE_INVALID;
    }
    node = wn_core_find_node(view->core, name);
    if (node < 0) {
        wn_set_error("wn_app_find: node '%s' not found", name);
        return WN_NODE_INVALID;
    }
    return view_node_handle(node);
}

wn_node_id wn_view_root(wn_view *view)
{
    int i;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_app_root: view is NULL");
        return WN_NODE_INVALID;
    }
    for (i = 0; i < wn_core_node_count(view->core); ++i) {
        if (wn_core_node_is_alive(view->core, i) &&
            wn_core_node_parent(view->core, i) < 0) {
            return view_node_handle(i);
        }
    }
    wn_set_error("wn_app_root: the document has no root");
    return WN_NODE_INVALID;
}

int32_t wn_view_node_count(wn_view *view)
{
    if (view == NULL) {
        return 0;
    }
    return (int32_t)wn_core_live_node_count(view->core);
}

wn_status wn_view_set_visible(wn_view *view, wn_node_id node, int visible)
{
    int index;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_visible: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_set_visible: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (wn_core_set_visible(view->core, index, visible) != 0) {
        wn_set_error("wn_node_set_visible: node handle %llu is not alive",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_set_image(wn_view *view, wn_node_id node, const char *path)
{
    int index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_image: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_set_image: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (path == NULL || path[0] == '\0') {
        wn_set_error("wn_node_set_image: path is NULL or empty");
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_set_image(view->core, index, path);
    if (result == 1) {
        wn_set_error("wn_node_set_image: node handle %llu is not an image node",
                     (unsigned long long)node);
        return WN_ERR_UNSUPPORTED;
    }
    if (result == -2) {
        wn_set_error("wn_node_set_image: out of memory copying '%s'", path);
        return WN_ERR_OUT_OF_MEMORY;
    }
    if (result != 0) {
        wn_set_error("wn_node_set_image: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_set_node_text(wn_view *view, wn_node_id node, const char *utf8)
{
    int index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_text: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_set_text: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (utf8 == NULL) {
        wn_set_error("wn_node_set_text: utf8 is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (!wn_core_node_is_text(view->core, index)) {
        wn_set_error("wn_node_set_text: node handle %llu is not a text node",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_set_text(view->core, index, utf8);
    if (result == 1) {
        wn_set_error("wn_node_set_text: '%s' cannot be rendered with the pre-shaped "
                     "glyphs of node %llu (runtime shaping is out of v0)",
                     utf8, (unsigned long long)node);
        return WN_ERR_UNSUPPORTED;
    }
    if (result != 0) {
        wn_set_error("wn_node_set_text: invalid text node %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_node_id wn_view_clone(wn_view *view, wn_node_id template_node, wn_node_id parent)
{
    int template_index;
    int parent_index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_clone: view is NULL");
        return WN_NODE_INVALID;
    }
    template_index = view_node_index(view, template_node);
    if (template_index < 0) {
        wn_set_error("wn_node_clone: invalid template handle %llu",
                     (unsigned long long)template_node);
        return WN_NODE_INVALID;
    }
    parent_index = view_node_index(view, parent);
    if (parent_index < 0) {
        wn_set_error("wn_node_clone: invalid parent handle %llu",
                     (unsigned long long)parent);
        return WN_NODE_INVALID;
    }
    result = wn_core_clone(view->core, template_index, parent_index);
    if (result == -2) {
        wn_set_error("wn_node_clone: node arena is full (%d nodes)",
                     wn_core_node_count(view->core));
        return WN_NODE_INVALID;
    }
    if (result < 0) {
        wn_set_error("wn_node_clone: cannot clone handle %llu under %llu",
                     (unsigned long long)template_node,
                     (unsigned long long)parent);
        return WN_NODE_INVALID;
    }
    view_mark_geometry(view);
    return view_node_handle(result);
}

wn_status wn_view_remove(wn_view *view, wn_node_id node)
{
    int index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_remove: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_remove: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_remove(view->core, index);
    if (result == -2) {
        wn_set_error("wn_node_remove: out of memory");
        return WN_ERR_OUT_OF_MEMORY;
    }
    if (result != 0) {
        wn_set_error("wn_node_remove: cannot remove handle %llu (root or invalid)",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_clear_children(wn_view *view, wn_node_id node)
{
    int index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_clear_children: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_clear_children: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_clear_children(view->core, index);
    if (result != 0) {
        wn_set_error("wn_node_clear_children: cannot clear handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_append(wn_view *view, wn_node_id parent, wn_node_id child)
{
    int parent_index;
    int child_index;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_append: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    parent_index = view_node_index(view, parent);
    if (parent_index < 0) {
        wn_set_error("wn_node_append: invalid parent handle %llu",
                     (unsigned long long)parent);
        return WN_ERR_INVALID_ARGUMENT;
    }
    child_index = view_node_index(view, child);
    if (child_index < 0) {
        wn_set_error("wn_node_append: invalid child handle %llu",
                     (unsigned long long)child);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (wn_core_append(view->core, parent_index, child_index) != 0) {
        wn_set_error("wn_node_append: cannot append handle %llu to %llu "
                     "(root, self or cycle)",
                     (unsigned long long)child, (unsigned long long)parent);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

/* ------------------------------------------------------ text input (v4) */

int wn_view_input_focused(const wn_view *view)
{
    if (view == NULL) {
        return 0;
    }
    return wn_core_input_focused(view->core) >= 0;
}

wn_status wn_view_set_value(wn_view *view, wn_node_id node, const char *utf8)
{
    int index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_value: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_set_value: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (utf8 == NULL) {
        wn_set_error("wn_node_set_value: utf8 is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (!wn_core_node_is_input(view->core, index)) {
        wn_set_error("wn_node_set_value: node handle %llu is not a text input",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_set_input_value(view->core, index, utf8);
    if (result == 1) {
        wn_set_error("wn_node_set_value: '%s' is not printable ASCII (32..126); "
                     "runtime shaping/IME are out of ABI v4", utf8);
        return WN_ERR_UNSUPPORTED;
    }
    if (result == 2) {
        wn_set_error("wn_node_set_value: '%s' exceeds the input max_length", utf8);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (result != 0) {
        wn_set_error("wn_node_set_value: invalid input node %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

const char *wn_view_get_value(wn_view *view, wn_node_id node)
{
    int index;
    const char *value;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_get_value: view is NULL");
        return NULL;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_get_value: invalid node handle %llu",
                     (unsigned long long)node);
        return NULL;
    }
    if (!wn_core_node_is_input(view->core, index)) {
        wn_set_error("wn_node_get_value: node handle %llu is not a text input",
                     (unsigned long long)node);
        return NULL;
    }
    value = wn_core_input_value(view->core, index);
    if (value == NULL) {
        wn_set_error("wn_node_get_value: input %llu has no value",
                     (unsigned long long)node);
        return NULL;
    }
    wn_free_(view->value_scratch);
    view->value_scratch = wn_strdup_(value);
    if (view->value_scratch == NULL) {
        wn_set_error("wn_node_get_value: out of memory");
        return NULL;
    }
    return view->value_scratch;
}
