/*
 * wn_view.h - embedded view facade (engine-owned core + renderer).
 *
 * Internal header: adds diagnostics/stats on top of the frozen wn_view_* ABI.
 */
#ifndef WN_VIEW_H
#define WN_VIEW_H

#include "webnative.h"

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
} wn_view_stats;

int wn_view_get_stats(const wn_view *view, wn_view_stats *stats);
const char *wn_view_backend(const wn_view *view);
const char *wn_view_device(const wn_view *view);
int wn_view_render_width(const wn_view *view);
int wn_view_render_height(const wn_view *view);

/* Internal command surface used by wn_app_*. Same semantics as the ABI. */
wn_status wn_view_set_text(wn_view *view, const char *id, const char *text_utf8);
wn_status wn_view_set_class(wn_view *view, const char *id, const char *class_name,
                            int enabled);

#if defined(__cplusplus)
}
#endif

#endif /* WN_VIEW_H */
