/*
 * wn_app.h - standalone application facade (Win32 host + wn_view).
 *
 * The functions below the ABI block are internal extensions for demos/tests:
 * auto-close frame limits and statistics. They are not part of webnative.h.
 */
#ifndef WN_APP_H
#define WN_APP_H

#include "webnative.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct wn_app_stats {
    int frames;
    double startup_ms;
    double frame_total_ms;
    double cpu_frame_total_ms;
    double last_frame_ms;
    double last_layout_ms;
    char backend[32];
    char device[256];
} wn_app_stats;

/* Auto-close after `max_frames` successfully presented frames (0 = until the
 * user closes the window). Used by examples/standalone.c --frames N. */
void wn_app_set_max_frames(wn_app *app, int max_frames);
void wn_app_set_resize_demo(wn_app *app, int enabled);

/* Internal: the embedded view owned by the app. Used by tests/demos to inject
 * synthetic wn_view_event() values; not part of the public ABI. */
wn_view *wn_app_view(wn_app *app);

int wn_app_get_stats(const wn_app *app, wn_app_stats *stats);

#if defined(__cplusplus)
}
#endif

#endif /* WN_APP_H */
