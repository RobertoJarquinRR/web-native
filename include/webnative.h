/*
 * webnative.h — C ABI v5
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
 *
 * Version history (additive unless stated otherwise):
 *  - v2: wn_event.delta_x/delta_y for WN_EVENT_WHEEL.
 *  - v3: dynamic UI: wn_node_id handles, find/clone/append/remove/set_visible.
 *  - v4: single-line text input and the WN_EVENT_INPUT semantic event.
 *  - v5: wn_event.node carries the stable handle of the event target, so
 *        cloned nodes are addressable; wn_node_parent/first_child/next_sibling
 *        navigate the live tree. Fields are appended; v4 layout is unchanged
 *        up to the new trailing member.
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

#define WN_ABI_VERSION 5u

/* Modifier bits for wn_event.modifiers. */
#define WN_MOD_SHIFT 0x01u
#define WN_MOD_CTRL  0x02u
#define WN_MOD_ALT   0x04u
#define WN_MOD_META  0x08u

/* Reserved flags; must be 0 in v0. */
#define WN_FLAG_NONE 0u

typedef struct wn_app  wn_app;
typedef struct wn_document wn_document;
typedef struct wn_view wn_view;

/* ABI v3: stable node handle for the dynamic UI API. 0 is never a valid node
 * (WN_NODE_INVALID). Handles stay valid until the node (or an ancestor) is
 * removed; do not guess or persist them across wn_app_destroy(). */
typedef uint64_t wn_node_id;
#define WN_NODE_INVALID ((wn_node_id)0)


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
    WN_EVENT_CLOSE        = 9,
    WN_EVENT_WHEEL        = 10,
    /* ABI v4: the focused text input changed value. `node` is the input handle
     * (`target_id` is its compiled name, NULL for cloned inputs); read the new
     * value with wn_node_get_value(). Emitted for typing,
     * Backspace/Delete and paste/cut. It is NOT emitted for
     * wn_node_set_value() (the caller already knows) nor for
     * caret/selection-only moves. */
    WN_EVENT_INPUT        = 11
} wn_event_type;

typedef struct wn_event {
    wn_event_type type;
    /* Compiled element id of the event target, or NULL. Only names that come
     * from the compiled document are reported: cloned nodes have no document
     * name (use `node` instead), and events that do not target a node leave
     * this NULL. Valid only during the callback. */
    const char *target_id;
    float x, y;              /* view coordinates (pointer events) */
    int32_t key;             /* virtual key (key events) */
    uint32_t codepoint;      /* UTF-32 codepoint (WN_EVENT_CHAR) */
    int32_t width, height;   /* client size (WN_EVENT_RESIZE) */
    uint32_t modifiers;      /* WN_MOD_* */
    /* ABI v2: WN_EVENT_WHEEL. deltas are px; positive delta_y scrolls the
     * content down (the host maps WM_MOUSEWHEEL wheel-forward to negative y).
     * v2 only consumes delta_y (vertical scroll); delta_x is reserved. */
    float delta_x, delta_y;
    /* ABI v5: stable handle (wn_node_id) of the event target, or
     * WN_NODE_INVALID when the event does not target a node. Works for
     * compiled and cloned nodes: pointer events report the hit interactive
     * node, key/char events report the focused input. Unlike target_id it
     * stays usable for clones; combine it with the wn_node_* navigation
     * helpers to reach children. */
    wn_node_id node;
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

/* ------------------------------------------------------------ documents */

/* Loads a compiled document (.wnui). The file is mapped read-only; the
 * document stays valid until wn_document_destroy(). Returns NULL and sets
 * wn_last_error() when the file is missing or invalid. */
WN_API wn_document *wn_document_load_blob(const char *path_utf8);
WN_API void wn_document_destroy(wn_document *document);

/* ------------------------------------------------------- standalone app */

WN_API wn_app *wn_app_create(const wn_config *config);

/* html_utf8 and css_utf8 are copied. base_url resolves relative resources and
 * may be NULL. css_utf8 may be NULL when the document links its stylesheet. */
WN_API wn_status wn_app_load(wn_app *app,
                             const char *html_utf8,
                             const char *base_url,
                             const char *css_utf8);

/* Loads a compiled document into the running app. The previous core is
 * released and the window keeps its renderer. Returns WN_ERR_IO when the
 * blob is missing or invalid (see wn_last_error()). */
WN_API wn_status wn_app_load_document(wn_app *app, const char *path_utf8);

WN_API wn_status wn_app_set_text(wn_app *app, const char *id, const char *text_utf8);
WN_API wn_status wn_app_set_class(wn_app *app, const char *id, const char *class_name, int enabled);
WN_API wn_status wn_app_on_event(wn_app *app, wn_event_cb callback, void *userdata);

/* Runs the UI loop until the window closes. Returns the process exit code. */
WN_API int wn_app_run(wn_app *app);
WN_API void wn_app_destroy(wn_app *app);

/* ---------------------------------------------------------- diagnostics */

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

/* Stops the loop after `max_frames` presented frames (0 = until the user
 * closes the window). */
WN_API void wn_app_set_max_frames(wn_app *app, int max_frames);

/* Frame counters and backend info for the last run. Returns 0 on success. */
WN_API int wn_app_get_stats(const wn_app *app, wn_app_stats *stats);

/* ---------------------------------------------------- dynamic UI (ABI v3) */
/*
 * Host-driven content reactivity over the compiled tree. All mutations mark the
 * engine dirty: the next wn_view_render()/frame re-runs layout, clipping and
 * paint. There is no separate commit call.
 *
 * Node handles are 1-based and opaque. Lookup by name only resolves compiled
 * node names (the generated ui_data.h table); clone handles are returned by
 * wn_node_clone() and are not addressable by name in v0.
 */

/* ------------------------------------------------- tree navigation (ABI v5) */
/*
 * Read-only navigation over the same live tree the mutations operate on, so a
 * host can walk the children of a clone handle reported by wn_event.node.
 *
 * Contract:
 *  - Every call returns WN_NODE_INVALID when `app` is NULL, `node` is not a
 *    live handle (never returned by the engine, removed, or stale after
 *    wn_app_destroy()), or there is no such relative.
 *  - parent() of the document root is WN_NODE_INVALID; next_sibling() of the
 *    last child is WN_NODE_INVALID.
 *  - Hidden nodes stay in the tree and are returned; removed children are
 *    unlinked and never returned. Child order matches paint order (insertion
 *    order; re-parented nodes move to the end).
 *  - The tree may mutate between calls; re-query instead of caching siblings
 *    across a mutation.
 */
WN_API wn_node_id wn_node_parent(wn_app *app, wn_node_id node);
WN_API wn_node_id wn_node_first_child(wn_app *app, wn_node_id node);
WN_API wn_node_id wn_node_next_sibling(wn_app *app, wn_node_id node);

/* Root node of the compiled document, or WN_NODE_INVALID. */
WN_API wn_node_id wn_app_root(wn_app *app);

/* Resolve a compiled node name. Exact names win; a unique bare last path
 * component ("div.card", "text[1]") is also accepted. Returns
 * WN_NODE_INVALID and sets wn_last_error() when not found. */
WN_API wn_node_id wn_app_find(wn_app *app, const char *name);

/* Live node count (compiled + cloned - removed). Diagnostic/testing hook. */
WN_API int32_t wn_app_node_count(wn_app *app);

/* Hide/show a node and its subtree. Hidden nodes are excluded from layout,
 * paint and hit testing. visible=0 hides, non-zero shows. */
WN_API wn_status wn_node_set_visible(wn_app *app, wn_node_id node, int visible);

/* Replace the image source of a NODE_IMAGE node. `path` is copied and resolved
 * by the renderer exactly like a compiled image src (relative to the process
 * CWD in v0). The path-keyed texture cache is reused; the compiled intrinsic
 * width/height are kept (no new WIC size probe in v0). */
WN_API wn_status wn_node_set_image(wn_app *app, wn_node_id node, const char *path);

/* Replace a text node's string. v0 has no runtime shaping: the string is only
 * accepted when every non-space byte already has a pre-shaped glyph at the
 * node's font size in the compiled atlas. Otherwise it returns
 * WN_ERR_UNSUPPORTED and wn_last_error() explains it. Layout keeps the
 * compiled text metrics (no reflow). */
WN_API wn_status wn_node_set_text(wn_app *app, wn_node_id node, const char *utf8);

/* Deep-clone `template_node` (styles, text runs, image and data-* fields) and
 * append the copy as the last child of `parent`. Returns the new root handle,
 * or WN_NODE_INVALID with wn_last_error() set. The copy is independent and is
 * never inserted as a document root; cloning a template into its own subtree
 * is allowed because the new nodes carry no cycle. */
WN_API wn_node_id wn_node_clone(wn_app *app, wn_node_id template_node, wn_node_id parent);

/* Remove a node and its subtree. The document root cannot be removed. */
WN_API wn_status wn_node_remove(wn_app *app, wn_node_id node);

/* Remove every child subtree of `node`; `node` itself stays alive. */
WN_API wn_status wn_node_clear_children(wn_app *app, wn_node_id node);

/* Re-parent `child` (and its subtree) as the last child of `parent`. The
 * document root cannot be moved and `parent` must not be inside `child`. */
WN_API wn_status wn_node_append(wn_app *app, wn_node_id parent, wn_node_id child);

/* -------------------------------------------------- text input (ABI v4) */
/*
 * Single-line ASCII text input. `node` must be a compiled `kind:"input"`
 * node (or a clone of one). Values are engine-owned copies of ASCII printable
 * text (32..126); non-ASCII returns WN_ERR_UNSUPPORTED. The length is capped
 * by the compiled `maxlength` (or 255 when absent); longer values return
 * WN_ERR_INVALID_ARGUMENT. set_value places the caret at the end, clears the
 * selection and does NOT emit WN_EVENT_INPUT (the caller already knows).
 * get_value returns an engine-owned string valid until the next webnative
 * call on this thread; NULL (and wn_last_error) for an invalid node.
 *
 * Editing happens through wn_view_event()/the window loop: click focuses and
 * places the caret, WN_EVENT_CHAR inserts printable ASCII, KEY_DOWN handles
 * Backspace/Delete/Left/Right/Home/End (+Shift selection) and Ctrl+A/C/X/V.
 * Esc releases focus. Enter is left to the host (no form/submit in v4).
 * Every user-driven value change emits WN_EVENT_INPUT.
 */
WN_API wn_status wn_node_set_value(wn_app *app, wn_node_id node, const char *utf8);
WN_API const char *wn_node_get_value(wn_app *app, wn_node_id node);

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
