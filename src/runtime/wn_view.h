/*
 * wn_view.h - embedded view facade (engine-owned core + renderer).
 *
 * Internal header: adds diagnostics/stats on top of the frozen wn_view_* ABI.
 */
#ifndef WN_VIEW_H
#define WN_VIEW_H

#include "webnative.h"
#include "wn_core.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct wn_view_stats {
    int frames;
    double startup_ms;        /* create -> first presented frame */
    double frame_total_ms;    /* wall time of every wn_view_render call */
    double cpu_frame_total_ms;/* upload + encode + submit, present excluded */
    double last_layout_ms;
    double last_render_ms;
    int image_failures;       /* images skipped by the backend so far */
} wn_view_stats;

int wn_view_get_stats(const wn_view *view, wn_view_stats *stats);
/* Clears the per-run counters and restarts the startup clock (wn_app_run). */
void wn_view_reset_stats(wn_view *view);
/* 1 when the next frame would change pixels (dirty/have no frame yet). */
int wn_view_needs_render(const wn_view *view);
/* Texture target without a window (tooling, tests, offscreen PNG). */
wn_view *wn_view_create_offscreen(int32_t width, int32_t height);
/* Same, but borrowing an existing core (the caller keeps ownership). */
wn_view *wn_view_create_offscreen_with_core(wn_core *core, int32_t width, int32_t height);
wn_view *wn_view_create_with_core(void *native_window_handle, wn_core *core,
                                  int32_t width, int32_t height);
/* Copy the last offscreen frame to a PNG. WN_OK on success. */
wn_status wn_view_readback(wn_view *view, const char *png_path);
/* Replaces the compiled document; the view retains its own reference (the
 * caller keeps theirs). Returns 0 on success. */
int wn_view_replace_document(wn_view *view, wn_document *document);

/* Tooling/test surface: states and wheel on the view's own core.
 * `state` is one or more WN_STATE_* bits. */
wn_status wn_view_set_state(wn_view *view, int node, int state, int enabled);
/* Applies the wheel and returns the scroll container node, or -1. */
int wn_view_wheel_node(wn_view *view, float x, float y, float delta_x, float delta_y);
void wn_view_display_counts(const wn_view *view, int *vertices, int *glyphs, int *images);
int wn_view_image_cache_count(const wn_view *view);
long long wn_view_image_cache_bytes(const wn_view *view);
const char *wn_view_backend(const wn_view *view);
const char *wn_view_device(const wn_view *view);
int wn_view_render_width(const wn_view *view);
int wn_view_render_height(const wn_view *view);

/* Internal command surface used by wn_app_*. Same semantics as the ABI. */
wn_status wn_view_set_text(wn_view *view, const char *id, const char *text_utf8);
wn_status wn_view_set_class(wn_view *view, const char *id, const char *class_name,
                            int enabled);

/* Dynamic UI (ABI v3) internal surface. wn_node_id values are the same opaque
 * handles exposed by include/webnative.h. */
wn_node_id wn_view_find(wn_view *view, const char *name);
wn_node_id wn_view_root(wn_view *view);
int32_t wn_view_node_count(wn_view *view);
/* Handle encoding used by the ABI: wn_view_handle() packs the slot generation
 * and the 1-based index; wn_view_resolve_handle() rejects stale handles. */
wn_node_id wn_view_handle(const wn_view *view, int index);
int wn_view_resolve_handle(const wn_view *view, wn_node_id node);
/* Borrowed core handle (never destroyed by the caller); used by the ABI layer
 * to read tree relations without duplicating the handle mapping. */
const wn_core *wn_view_core(const wn_view *view);
wn_status wn_view_set_visible(wn_view *view, wn_node_id node, int visible);
wn_status wn_view_set_image(wn_view *view, wn_node_id node, const char *path);
wn_status wn_view_set_node_text(wn_view *view, wn_node_id node, const char *utf8);
wn_node_id wn_view_clone(wn_view *view, wn_node_id template_node, wn_node_id parent);
wn_status wn_view_remove(wn_view *view, wn_node_id node);
wn_status wn_view_clear_children(wn_view *view, wn_node_id node);
wn_status wn_view_append(wn_view *view, wn_node_id parent, wn_node_id child);
wn_status wn_view_set_state_handle(wn_view *view, wn_node_id node, uint32_t state,
                                   int enabled);
uint32_t wn_view_get_state_handle(const wn_view *view, wn_node_id node);
const char *wn_view_get_text(wn_view *view, wn_node_id node);
wn_status wn_view_get_rect(wn_view *view, wn_node_id node, wn_rect *out);
wn_status wn_view_scroll_into_view(wn_view *view, wn_node_id node, float margin);
wn_status wn_view_set_image_size(wn_view *view, wn_node_id node,
                                 float width, float height);

/* Text input (ABI v4) internal surface. */
int wn_view_input_focused(const wn_view *view);
wn_status wn_view_set_value(wn_view *view, wn_node_id node, const char *utf8);
const char *wn_view_get_value(wn_view *view, wn_node_id node);
wn_status wn_view_focus_input(wn_view *view, wn_node_id node);
wn_status wn_view_blur_input(wn_view *view);

/* ABI v6: marks the core dirty so the next wn_view_render() re-runs paint.
 * Hosts with their own loop call it from their timer/frame callback. */
wn_status wn_view_tick(wn_view *view);

#if defined(__cplusplus)
}
#endif

#endif /* WN_VIEW_H */
