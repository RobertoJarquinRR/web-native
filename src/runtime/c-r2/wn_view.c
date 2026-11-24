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
    case WN_EVENT_POINTER_MOVE:
        hit = wn_core_pointer_move(view->core, event->x, event->y);
        changed = 1;
        break;
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
    case WN_EVENT_RESIZE:
        view_resize_internal(view, event->width, event->height, 0.0f);
        break;
    case WN_EVENT_CLOSE:
        view->closed = 1;
        break;
    case WN_EVENT_KEY_DOWN:
    case WN_EVENT_KEY_UP:
    case WN_EVENT_CHAR:
    case WN_EVENT_FOCUS_IN:
    case WN_EVENT_FOCUS_OUT:
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
    free(view);
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
