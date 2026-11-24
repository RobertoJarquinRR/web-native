/*
 * webnative.h — C ABI v0
 *
 * Public contract between the webnative engine and host applications.
 *
 * Rules:
 *  - Plain C ABI; no C++/Rust/.NET types.
 *  - Opaque handles; no internal structs are exposed.
 *  - No GPU API types in this header.
 *  - All strings are UTF-8 and copied by the engine.
 *  - Single-threaded UI: call everything on the UI thread.
 *  - No exceptions: every call returns wn_status; details in wn_last_error().
 *
 * ABI changes that break layout/semantics bump WN_ABI_VERSION (major).
 */
#ifndef WEBNATIVE_H
#define WEBNATIVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#  if defined(WN_STATIC)
#    define WN_API
#  elif defined(WN_BUILD_DLL)
#    define WN_API __declspec(dllexport)
#  else
#    define WN_API __declspec(dllimport)
#  endif
#else
#  define WN_API __attribute__((visibility("default")))
#endif

#define WN_ABI_VERSION 1u

/* Modifier bits for wn_event.modifiers. */
#define WN_MOD_SHIFT 0x01u
#define WN_MOD_CTRL  0x02u
#define WN_MOD_ALT   0x04u
#define WN_MOD_META  0x08u

/* Reserved flags; must be 0 in v0. */
#define WN_FLAG_NONE 0u

typedef struct wn_app  wn_app;
typedef struct wn_view wn_view;

typedef enum wn_status {
    WN_OK                    = 0,
    WN_ERR_INVALID_ARGUMENT  = 1,
    WN_ERR_OUT_OF_MEMORY     = 2,
    WN_ERR_IO                = 3,
    WN_ERR_PARSE             = 4,
    WN_ERR_UNSUPPORTED       = 5,
    WN_ERR_GPU               = 6,
    WN_ERR_STATE             = 7,
    WN_ERR_INTERNAL          = 8
} wn_status;

typedef enum wn_event_type {
    WN_EVENT_POINTER_MOVE = 0,
    WN_EVENT_POINTER_DOWN = 1,
    WN_EVENT_POINTER_UP   = 2,
    WN_EVENT_KEY_DOWN     = 3,
    WN_EVENT_KEY_UP       = 4,
    WN_EVENT_CHAR         = 5,
    WN_EVENT_RESIZE       = 6,
    WN_EVENT_FOCUS_IN     = 7,
    WN_EVENT_FOCUS_OUT    = 8,
    WN_EVENT_CLOSE        = 9
} wn_event_type;

typedef struct wn_event {
    wn_event_type type;
    const char *target_id;   /* element id, or NULL; valid only during the callback */
    float x, y;              /* view coordinates (pointer events) */
    int32_t key;             /* virtual key (key events) */
    uint32_t codepoint;      /* UTF-32 codepoint (WN_EVENT_CHAR) */
    int32_t width, height;   /* client size (WN_EVENT_RESIZE) */
    uint32_t modifiers;      /* WN_MOD_* */
} wn_event;

typedef void (*wn_event_cb)(void *userdata, const wn_event *event);

typedef struct wn_config {
    const char *title;       /* window title (standalone) */
    int32_t width;           /* client width in px */
    int32_t height;          /* client height in px */
    float dpi_scale;         /* 0 = system DPI */
    uint32_t flags;          /* reserved, must be 0 */
} wn_config;

/* ---------------------------------------------------------------- common */

WN_API const char *wn_version(void);
WN_API uint32_t wn_abi_version(void);

/* Last error on the calling thread. Owned by the engine; valid until the next
 * webnative call on this thread. Returns NULL if there is no error. */
WN_API const char *wn_last_error(void);

/* ------------------------------------------------------- standalone app */

WN_API wn_app *wn_app_create(const wn_config *config);

/* html_utf8 and css_utf8 are copied. base_url resolves relative resources and
 * may be NULL. css_utf8 may be NULL when the document links its stylesheet. */
WN_API wn_status wn_app_load(wn_app *app,
                             const char *html_utf8,
                             const char *base_url,
                             const char *css_utf8);

WN_API wn_status wn_app_set_text(wn_app *app, const char *id, const char *text_utf8);
WN_API wn_status wn_app_set_class(wn_app *app, const char *id, const char *class_name, int enabled);
WN_API wn_status wn_app_on_event(wn_app *app, wn_event_cb callback, void *userdata);

/* Runs the UI loop until the window closes. Returns the process exit code. */
WN_API int wn_app_run(wn_app *app);
WN_API void wn_app_destroy(wn_app *app);

/* ----------------------------------------------------------- embedded view */

/* native_window_handle is the OS window handle on Windows (future: native
 * view/window pointers on other platforms).
 * width/height are the initial client size. flags is reserved (must be 0). */
WN_API wn_view *wn_view_create(void *native_window_handle,
                               int32_t width,
                               int32_t height,
                               uint32_t flags);

WN_API wn_status wn_view_load(wn_view *view,
                              const char *html_utf8,
                              const char *base_url,
                              const char *css_utf8);

WN_API wn_status wn_view_resize(wn_view *view, int32_t width, int32_t height, float dpi_scale);

/* Forward a host event. The host owns its message loop. */
WN_API wn_status wn_view_event(wn_view *view, const wn_event *event);

/* Render and present one frame. Call from the host's paint/timer. */
WN_API wn_status wn_view_render(wn_view *view);

WN_API wn_status wn_view_on_event(wn_view *view, wn_event_cb callback, void *userdata);
WN_API void wn_view_destroy(wn_view *view);

#ifdef __cplusplus
}
#endif

#endif /* WEBNATIVE_H */
