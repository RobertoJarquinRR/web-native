/*
 * wn_app.c - standalone mode: the Win32 host from host_win32.c drives a
 * wn_view. There is only one engine (core + renderer) for both modes.
 */
#include "webnative.h"
#include "wn_app.h"
#include "wn_host.h"
#include "wn_view.h"
#include "wn_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct wn_app {
    const wn_host_ops *host_ops;
    void *host;
    wn_view *view;
    char *title;
    int width;
    int height;
    int loaded;
    int closed;
    int failed;
    int max_frames;
    int frames;
    int resize_demo;
    int demo_stage;
    int caret_blink;
    int tick_ms;
    double created_ms;
    wn_event_cb callback;
    void *userdata;
    wn_app_stats stats;
};

/* The host timer only runs while a text input owns the focus. */
static void app_sync_caret_blink(wn_app *app)
{
    int focused;
    if (app == NULL || app->view == NULL || app->host_ops == NULL ||
        app->host == NULL || app->host_ops->set_caret_blink == NULL) {
        return;
    }
    focused = wn_view_input_focused(app->view) ? 1 : 0;
    if (focused != app->caret_blink) {
        app->caret_blink = focused;
        app->host_ops->set_caret_blink(app->host, focused);
    }
}

static void app_host_event(void *userdata, const wn_event *event)
{
    wn_app *app = (wn_app *)userdata;
    if (app == NULL || event == NULL) {
        return;
    }
    if (event->type == WN_EVENT_CLOSE) {
        app->closed = 1;
    }
    /* Esc closes the window only when no text input owns the focus. With an
     * input focused the view consumes Esc to release focus (ABI v4). */
    if (event->type == WN_EVENT_KEY_DOWN && event->key == 27 &&
        app->view != NULL && !wn_view_input_focused(app->view)) {
        app->closed = 1;
        if (app->host_ops != NULL && app->host != NULL &&
            app->host_ops->request_close != NULL) {
            app->host_ops->request_close(app->host);
        }
        return;
    }
    if (app->view != NULL) {
        (void)wn_view_event(app->view, event);
        app_sync_caret_blink(app);
    }
}

static void app_view_event(void *userdata, const wn_event *event)
{
    wn_app *app = (wn_app *)userdata;
    if (app == NULL) {
        return;
    }
    if (app->callback != NULL) {
        app->callback(app->userdata, event);
    }
}

static void app_host_frame(void *userdata, int force)
{
    wn_app *app = (wn_app *)userdata;
    wn_view_stats view_stats;
    wn_status status;

    if (app == NULL || app->view == NULL || app->closed) {
        return;
    }
    /* ABI v6: while a tick is active every frame callback marks the core
     * dirty, so animations get a fresh layout/paint even without host
     * mutations. The tick timer itself only wakes the message loop. */
    if (app->tick_ms > 0) {
        (void)wn_view_tick(app->view);
    }
    if (!force && app->max_frames <= 0 && !wn_view_needs_render(app->view)) {
        return;
    }
    if (wn_view_render_width(app->view) <= 0 || wn_view_render_height(app->view) <= 0) {
        Sleep(10);
        return;
    }

    if (app->resize_demo) {
        if (app->demo_stage == 0 && app->frames >= 5) {
            app->demo_stage = 1;
            app->host_ops->set_client_size(app->host, 560, 640);
        } else if (app->demo_stage == 1 && app->frames >= 15) {
            app->demo_stage = 2;
            app->host_ops->set_client_size(app->host, 960, 640);
        }
    }

    status = wn_view_render(app->view);
    if (status != WN_OK) {
        app->failed = 1;
        app->closed = 1;
        app->host_ops->request_close(app->host);
        return;
    }
    app->frames += 1;
    memset(&view_stats, 0, sizeof(view_stats));
    if (wn_view_get_stats(app->view, &view_stats) == 0) {
        app->stats.frames = view_stats.frames;
        app->stats.startup_ms = view_stats.startup_ms;
        app->stats.frame_total_ms = view_stats.frame_total_ms;
        app->stats.cpu_frame_total_ms = view_stats.cpu_frame_total_ms;
        app->stats.last_frame_ms = view_stats.last_render_ms;
        app->stats.last_layout_ms = view_stats.last_layout_ms;
    }
    if (app->max_frames > 0 && app->frames >= app->max_frames) {
        app->closed = 1;
        app->host_ops->request_close(app->host);
    }
}

wn_app *wn_app_create(const wn_config *config)
{
    wn_app *app;
    wn_host_config host_config;
    wn_config defaults;
    void *window;

    wn_clear_error();
    memset(&defaults, 0, sizeof(defaults));
    if (config == NULL) {
        config = &defaults;
    }
    if (config->flags != WN_FLAG_NONE) {
        wn_set_error("wn_app_create: flags must be 0 (got 0x%X)", (unsigned)config->flags);
        return NULL;
    }

    app = (wn_app *)calloc(1, sizeof(*app));
    if (app == NULL) {
        wn_set_error("wn_app_create: out of memory");
        return NULL;
    }
    app->width = config->width > 0 ? config->width : 960;
    app->height = config->height > 0 ? config->height : 640;
    app->title = wn_strdup_(config->title != NULL ? config->title : "webnative ABI");
    if (app->title == NULL) {
        wn_set_error("wn_app_create: out of memory copying title");
        free(app);
        return NULL;
    }
    app->created_ms = wn_now_ms();
    app->host_ops = wn_host_win32();

    memset(&host_config, 0, sizeof(host_config));
    host_config.title = app->title;
    host_config.width = app->width;
    host_config.height = app->height;
    if (app->host_ops == NULL || app->host_ops->create == NULL ||
        app->host_ops->create(&app->host, &host_config, app_host_event,
                              app_host_frame, app) != 0) {
        wn_set_error("wn_app_create: Win32 host creation failed");
        wn_free_(app->title);
        free(app);
        return NULL;
    }

    window = app->host_ops->surface_handle(app->host);
    if (window == NULL) {
        window = app->host_ops->native_window(app->host);
    }
    app->view = wn_view_create(window, app->width, app->height, WN_FLAG_NONE);
    if (app->view == NULL) {
        char message[512];
        snprintf(message, sizeof(message), "%s",
                 wn_last_error() != NULL ? wn_last_error() : "view creation failed");
        wn_set_error("wn_app_create: %s", message);
        app->host_ops->destroy(app->host);
        wn_free_(app->title);
        free(app);
        return NULL;
    }
    (void)wn_view_on_event(app->view, app_view_event, app);
    /* ABI v7: hand the platform IME to the core; the core drives focus/blur and
     * window placement, the host only translates WM_IME_* into events. */
    if (app->host_ops->ime_backend != NULL) {
        void *ime_context = NULL;
        const wn_ime_ops *ime = app->host_ops->ime_backend(app->host, &ime_context);
        wn_view_set_ime(app->view, ime, ime_context);
    }
    return app;
}

wn_status wn_app_load(wn_app *app, const char *html_utf8,
                      const char *base_url, const char *css_utf8)
{
    wn_status status;
    wn_clear_error();
    if (app == NULL) {
        wn_set_error("wn_app_load: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    status = wn_view_load(app->view, html_utf8, base_url, css_utf8);
    app->loaded = status == WN_OK;
    return status;
}

wn_status wn_app_load_document(wn_app *app, const char *path_utf8)
{
    wn_document *document;

    wn_clear_error();
    if (app == NULL || app->view == NULL || path_utf8 == NULL) {
        wn_set_error("wn_app_load_document: app and path are required");
        return WN_ERR_INVALID_ARGUMENT;
    }
    document = wn_document_load_blob(path_utf8);
    if (document == NULL) {
        return WN_ERR_IO;
    }
    if (wn_view_replace_document(app->view, document) != 0) {
        char detail[512];
        snprintf(detail, sizeof(detail), "%s",
                 wn_last_error() != NULL ? wn_last_error() : "cannot build a core");
        wn_document_destroy(document);
        wn_set_error("wn_app_load_document: %s", detail);
        return WN_ERR_INTERNAL;
    }
    /* The view retained its own reference; drop the loader's. */
    wn_document_release(document);
    app->loaded = 1;
    return WN_OK;
}

wn_status wn_app_set_text(wn_app *app, const char *id, const char *text_utf8){
    wn_clear_error();
    if (app == NULL) {
        wn_set_error("wn_app_set_text: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    return wn_view_set_text(app->view, id, text_utf8);
}

wn_status wn_app_set_class(wn_app *app, const char *id, const char *class_name,
                           int enabled)
{
    wn_clear_error();
    if (app == NULL) {
        wn_set_error("wn_app_set_class: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    return wn_view_set_class(app->view, id, class_name, enabled);
}

wn_status wn_app_on_event(wn_app *app, wn_event_cb callback, void *userdata)
{
    wn_clear_error();
    if (app == NULL) {
        wn_set_error("wn_app_on_event: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    app->callback = callback;
    app->userdata = userdata;
    return WN_OK;
}

/* ---------------------------------------------------- dynamic UI (ABI v3) */

wn_node_id wn_app_root(wn_app *app)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_app_root: app is NULL");
        return WN_NODE_INVALID;
    }
    return wn_view_root(app->view);
}

wn_node_id wn_app_find(wn_app *app, const char *name)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_app_find: app is NULL");
        return WN_NODE_INVALID;
    }
    return wn_view_find(app->view, name);
}

int32_t wn_app_node_count(wn_app *app)
{
    if (app == NULL || app->view == NULL) {
        return 0;
    }
    return wn_view_node_count(app->view);
}

/* -------------------------------------------------- tree navigation (ABI v5) */

static wn_node_id app_node_handle(const wn_app *app, int index)
{
    return app != NULL ? wn_view_handle(app->view, index) : WN_NODE_INVALID;
}

/* Validates an ABI handle against the live slot and its generation. */
static int app_node_index(const wn_app *app, wn_node_id node)
{
    if (app == NULL || app->view == NULL) {
        return -1;
    }
    return wn_view_resolve_handle(app->view, node);
}

wn_node_id wn_node_parent(wn_app *app, wn_node_id node)
{
    int index;
    const wn_core *core;
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_parent: app is NULL");
        return WN_NODE_INVALID;
    }
    index = app_node_index(app, node);
    if (index < 0) {
        return WN_NODE_INVALID;
    }
    core = wn_view_core(app->view);
    return app_node_handle(app, wn_core_node_parent(core, index));
}

wn_node_id wn_node_first_child(wn_app *app, wn_node_id node)
{
    int index;
    int child;
    const wn_core *core;
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_first_child: app is NULL");
        return WN_NODE_INVALID;
    }
    index = app_node_index(app, node);
    if (index < 0) {
        return WN_NODE_INVALID;
    }
    core = wn_view_core(app->view);
    child = wn_core_node_first_child(core, index);
    /* Hierarchy links only hold live nodes; the loop keeps the skip-removed
     * contract explicit if a future mutation unlinks lazily. */
    while (child >= 0 && !wn_core_node_is_alive(core, child)) {
        child = wn_core_node_next_sibling(core, child);
    }
    return app_node_handle(app, child);
}

wn_node_id wn_node_next_sibling(wn_app *app, wn_node_id node)
{
    int index;
    int sibling;
    const wn_core *core;
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_next_sibling: app is NULL");
        return WN_NODE_INVALID;
    }
    index = app_node_index(app, node);
    if (index < 0) {
        return WN_NODE_INVALID;
    }
    core = wn_view_core(app->view);
    sibling = wn_core_node_next_sibling(core, index);
    while (sibling >= 0 && !wn_core_node_is_alive(core, sibling)) {
        sibling = wn_core_node_next_sibling(core, sibling);
    }
    return app_node_handle(app, sibling);
}

wn_status wn_node_set_visible(wn_app *app, wn_node_id node, int visible)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_set_visible: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    return wn_view_set_visible(app->view, node, visible);
}

wn_status wn_node_set_image(wn_app *app, wn_node_id node, const char *path)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_set_image: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    return wn_view_set_image(app->view, node, path);
}

wn_status wn_node_set_text(wn_app *app, wn_node_id node, const char *utf8)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_set_text: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    return wn_view_set_node_text(app->view, node, utf8);
}

wn_node_id wn_node_clone(wn_app *app, wn_node_id template_node, wn_node_id parent)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_clone: app is NULL");
        return WN_NODE_INVALID;
    }
    return wn_view_clone(app->view, template_node, parent);
}

wn_status wn_node_remove(wn_app *app, wn_node_id node)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_remove: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    return wn_view_remove(app->view, node);
}

wn_status wn_node_clear_children(wn_app *app, wn_node_id node)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_clear_children: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    return wn_view_clear_children(app->view, node);
}

wn_status wn_node_append(wn_app *app, wn_node_id parent, wn_node_id child)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_append: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    return wn_view_append(app->view, parent, child);
}

/* ----------------------------------------------------- node state (ABI v6) */

wn_status wn_node_set_state(wn_app *app, wn_node_id node, uint32_t state,
                            int enabled)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_set_state: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (state == 0u || (state & ~(uint32_t)(WN_STATE_HOVER | WN_STATE_PRESS |
                                            WN_STATE_SELECTED |
                                            WN_STATE_FOCUSED |
                                            WN_STATE_DISABLED)) != 0u) {
        wn_set_error("wn_node_set_state: unknown state mask 0x%X",
                     (unsigned)state);
        return WN_ERR_INVALID_ARGUMENT;
    }
    return wn_view_set_state_handle(app->view, node, state, enabled);
}

uint32_t wn_node_get_state(wn_app *app, wn_node_id node)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        return 0;
    }
    return wn_view_get_state_handle(app->view, node);
}

/* ------------------------------------------------------ node reads (ABI v6) */

const char *wn_node_get_text(wn_app *app, wn_node_id node)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_get_text: app is NULL");
        return NULL;
    }
    return wn_view_get_text(app->view, node);
}

wn_status wn_node_get_rect(wn_app *app, wn_node_id node, wn_rect *out)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_get_rect: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    return wn_view_get_rect(app->view, node, out);
}

wn_status wn_node_scroll_into_view(wn_app *app, wn_node_id node, float margin)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_scroll_into_view: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    return wn_view_scroll_into_view(app->view, node, margin);
}

wn_status wn_app_focus_input(wn_app *app, wn_node_id node)
{
    wn_status status;
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_app_focus_input: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    status = wn_view_focus_input(app->view, node);
    /* Called outside an event dispatch, so the blink timer would not sync. */
    if (status == WN_OK) {
        app_sync_caret_blink(app);
    }
    return status;
}

wn_status wn_app_blur_input(wn_app *app)
{
    wn_status status;
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_app_blur_input: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    status = wn_view_blur_input(app->view);
    if (status == WN_OK) {
        app_sync_caret_blink(app);
    }
    return status;
}

wn_status wn_node_set_image_size(wn_app *app, wn_node_id node,
                                 float width, float height)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_set_image_size: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    return wn_view_set_image_size(app->view, node, width, height);
}

/* ------------------------------------------------------ text input (v4) */

wn_status wn_node_set_value(wn_app *app, wn_node_id node, const char *utf8)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_set_value: app is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    return wn_view_set_value(app->view, node, utf8);
}

const char *wn_node_get_value(wn_app *app, wn_node_id node)
{
    wn_clear_error();
    if (app == NULL || app->view == NULL) {
        wn_set_error("wn_node_get_value: app is NULL");
        return NULL;
    }
    return wn_view_get_value(app->view, node);
}

/* Internal (wn_app.h): lets demos/tests inject wn_view_event() without a real
 * keyboard. Not part of the public ABI. */
wn_view *wn_app_view(wn_app *app)
{
    return app != NULL ? app->view : NULL;
}

int wn_app_run(wn_app *app)
{
    wn_clear_error();
    if (app == NULL) {
        wn_set_error("wn_app_run: app is NULL");
        return 1;
    }
    if (!app->loaded) {
        wn_set_error("wn_app_run: document not loaded (call wn_app_load first)");
        return 1;
    }
    app->closed = 0;
    app->failed = 0;
    app->frames = 0;
    /* A second run must report its own startup and frame totals. */
    memset(&app->stats, 0, sizeof(app->stats));
    wn_view_reset_stats(app->view);
    if (wn_view_render_width(app->view) <= 0 ||
        wn_view_render_height(app->view) <= 0) {
        /* A 0x0 backing surface would spin the loop without ever presenting a
         * frame (and never reaching max_frames). Restore the configured size. */
        if (wn_view_resize(app->view, app->width, app->height, 0.0f) != WN_OK) {
            wn_set_error("wn_app_run: cannot restore a nonzero client size");
            return 1;
        }
    }
    app->host_ops->show(app->host);
    app->host_ops->run(app->host);
    if (app->failed) {
        const char *message = wn_last_error();
        if (message != NULL) {
            fprintf(stderr, "wn_app_run: %s\n", message);
        }
        return 1;
    }
    return 0;
}

void wn_app_destroy(wn_app *app)
{
    if (app == NULL) {
        return;
    }
    wn_clear_error();
    if (app->view != NULL) {
        wn_view_destroy(app->view);
        /* DestroyWindow() may still deliver WM_KILLFOCUS/WM_SIZE to the host;
         * app_host_event must not touch the freed view. */
        app->view = NULL;
    }
    if (app->host_ops != NULL && app->host != NULL) {
        app->host_ops->destroy(app->host);
    }
    wn_free_(app->title);
    free(app);
}

void wn_app_set_max_frames(wn_app *app, int max_frames)
{
    if (app != NULL) {
        app->max_frames = max_frames > 0 ? max_frames : 0;
        if (app->host_ops != NULL && app->host != NULL &&
            app->host_ops->set_continuous != NULL) {
            app->host_ops->set_continuous(app->host, app->max_frames > 0);
        }
    }
}

/* ABI v6: periodic host tick. The timer wakes the loop; app_host_frame marks
 * the core dirty while it is active. 0 stops the timer. */
void wn_app_set_tick_ms(wn_app *app, int ms)
{
    if (app == NULL) {
        return;
    }
    app->tick_ms = ms > 0 ? ms : 0;
    if (app->view != NULL && app->tick_ms > 0) {
        (void)wn_view_tick(app->view);
    }
    if (app->host_ops != NULL && app->host != NULL &&
        app->host_ops->set_tick != NULL) {
        app->host_ops->set_tick(app->host, app->tick_ms);
    }
}

void wn_app_set_resize_demo(wn_app *app, int enabled)
{
    if (app != NULL) {
        app->resize_demo = enabled != 0;
    }
}

int wn_app_get_stats(const wn_app *app, wn_app_stats *stats)
{
    if (app == NULL || stats == NULL) {
        return 1;
    }
    *stats = app->stats;
    if (app->view != NULL) {
        const char *backend = wn_view_backend(app->view);
        const char *device = wn_view_device(app->view);
        if (backend != NULL) {
            snprintf(stats->backend, sizeof(stats->backend), "%s", backend);
        }
        if (device != NULL) {
            snprintf(stats->device, sizeof(stats->device), "%s", device);
        }
    }
    return 0;
}
