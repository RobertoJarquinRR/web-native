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
    double created_ms;
    wn_event_cb callback;
    void *userdata;
    wn_app_stats stats;
};

static void app_host_event(void *userdata, const wn_event *event)
{
    wn_app *app = (wn_app *)userdata;
    if (app == NULL || event == NULL) {
        return;
    }
    if (event->type == WN_EVENT_CLOSE) {
        app->closed = 1;
    }
    if (app->view != NULL) {
        (void)wn_view_event(app->view, event);
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

static void app_host_frame(void *userdata)
{
    wn_app *app = (wn_app *)userdata;
    wn_view_stats view_stats;
    wn_status status;

    if (app == NULL || app->view == NULL || app->closed) {
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

wn_status wn_app_set_text(wn_app *app, const char *id, const char *text_utf8)
{
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
