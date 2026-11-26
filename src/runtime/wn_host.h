/*
 * wn_host.h - platform host vtable.
 *
 * The host owns the OS window and its message loop. Standalone uses the Win32
 * implementation in host_win32.c; an embedded host already owns its loop and
 * talks to wn_view_* directly.
 */
#ifndef WN_HOST_H
#define WN_HOST_H

#include "webnative.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct wn_host_config {
    const char *title;
    int width;
    int height;
} wn_host_config;

typedef void (*wn_host_event_fn)(void *userdata, const wn_event *event);
/* force=1 requests a present even when the view is not dirty (WM_PAINT). */
typedef void (*wn_host_frame_fn)(void *userdata, int force);

typedef struct wn_host_ops {
    const char *name;
    int  (*create)(void **context, const wn_host_config *config,
                   wn_host_event_fn on_event, wn_host_frame_fn on_frame,
                   void *userdata);
    int  (*run)(void *context);
    void (*request_close)(void *context);
    void (*show)(void *context);
    void *(*native_window)(void *context);
    void *(*surface_handle)(void *context); /* native surface provider (HWND) */
    void (*set_client_size)(void *context, int width, int height);
    /* Test/demo mode: render every loop even without messages (--frames). */
    void (*set_continuous)(void *context, int enabled);
    /* Caret blink: start/stop the host timer while an input owns the focus. */
    void (*set_caret_blink)(void *context, int enabled);
    /* ABI v6: periodic frame timer (ms <= 0 stops it). */
    void (*set_tick)(void *context, int ms);
    void (*destroy)(void *context);
} wn_host_ops;

const wn_host_ops *wn_host_win32(void);

#if defined(__cplusplus)
}
#endif

#endif /* WN_HOST_H */
