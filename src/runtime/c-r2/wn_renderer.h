/*
 * wn_renderer.h - GPU backend vtable.
 *
 * The core produces a wn_display_list (vertices + pre-shaped glyph vertices +
 * an immutable R8 atlas). A renderer uploads it and presents it into a native
 * surface. No GPU API type crosses this boundary.
 */
#ifndef WN_RENDERER_H
#define WN_RENDERER_H

#include "wn_core.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef enum wn_surface_kind {
    WN_SURFACE_NONE = 0,
    WN_SURFACE_HWND = 1
} wn_surface_kind;

typedef struct wn_surface_desc {
    wn_surface_kind kind;
    void *handle;          /* OS window handle (opaque to the core) */
    void *instance_handle; /* OS module/instance handle, may be NULL */
    int width;
    int height;
} wn_surface_desc;

typedef struct wn_renderer_info {
    char backend[32];
    char device[256];
    int width;
    int height;
    int format;
} wn_renderer_info;

typedef struct wn_renderer_stats {
    int frames;
    double last_cpu_ms;     /* acquire + encode + submit, present excluded */
    double last_present_ms;
} wn_renderer_stats;

typedef struct wn_renderer_ops {
    const char *name;
    int  (*init)(void **context, const wn_surface_desc *surface, int width, int height);
    void (*resize)(void *context, int width, int height);
    int  (*upload)(void *context, const wn_display_list *list);
    int  (*render)(void *context, const wn_display_list *list);
    void (*destroy)(void *context);
    const char *(*last_error)(void *context);
    void (*get_info)(void *context, wn_renderer_info *info);
    void (*get_stats)(void *context, wn_renderer_stats *stats);
} wn_renderer_ops;

const wn_renderer_ops *wn_renderer_wgpu(void);

#if defined(__cplusplus)
}
#endif

#endif /* WN_RENDERER_H */
