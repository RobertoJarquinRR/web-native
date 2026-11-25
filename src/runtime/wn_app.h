/*
 * wn_app.h - standalone application facade (Win32 host + wn_view).
 *
 * wn_app_set_max_frames and wn_app_get_stats live in the public header; the
 * remaining functions are internal extensions for demos/tests.
 */
#ifndef WN_APP_H
#define WN_APP_H

#include "webnative.h"

#if defined(__cplusplus)
extern "C" {
#endif

void wn_app_set_resize_demo(wn_app *app, int enabled);

/* Internal: the embedded view owned by the app. Used by tests/demos to inject
 * synthetic wn_view_event() values; not part of the public ABI. */
wn_view *wn_app_view(wn_app *app);

#if defined(__cplusplus)
}
#endif

#endif /* WN_APP_H */
