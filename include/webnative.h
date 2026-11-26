/*
 * webnative.h — C ABI v7
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
 *        navigate the live tree. Handles pack a per-slot generation (see
 *        wn_node_id), so a handle kept across a removal is rejected instead of
 *        addressing the slot's next tenant. Fields are appended; v4 layout is
 *        unchanged up to the new trailing member.
 *  - v6: per-node hover/press/selected state bits (wn_node_set_state /
 *        wn_node_get_state, WN_STATE_* masks) instead of one global selected
 *        slot; wn_node_get_text/get_rect/scroll_into_view, wn_app_focus_input
 *        and wn_app_blur_input; wn_event.button and wn_event.click_count
 *        (appended after `node`); wn_app_set_tick_ms + internal wn_view_tick
 *        for host-driven frames; reference-counted documents
 *        (wn_document_retain/release) and wn_node_set_image_size for the
 *        intrinsic size of a replaced image. All additions are append-only:
 *        the v5 layout is unchanged up to the new trailing event members.
 *        wn_event.node is now also reported for right/middle buttons and for
 *        left clicks on non-interactive nodes; hosts that act on POINTER_UP
 *        must gate on `button` (0 is reserved for non-pointer events).
 *  - v7: additive focus/disabled state bits: WN_STATE_FOCUSED and
 *        WN_STATE_DISABLED. wn_node_set_state() accepts them and
 *        wn_node_get_state() returns them. Input focus sets/clears the focused
 *        bit; a disabled node (or a descendant of one) is inert to the pointer
 *        state machine (no hover, press or selection) while wn_node_get_rect()
 *        and the diagnostic hit test still see it. Also additive in v7:
 *        WN_EVENT_IME_START/COMPOSITION/COMMIT/CANCEL for platform IME
 *        composition on the focused single-line input (IMM32 today). No struct
 *        layout changed: the composition UTF-8 rides in `target_id`, the
 *        codepoint cursor in `key`, and the caret rect in x/y/width/height.
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

#define WN_ABI_VERSION 7u

/* Modifier bits for wn_event.modifiers. */
#define WN_MOD_SHIFT 0x01u
#define WN_MOD_CTRL  0x02u
#define WN_MOD_ALT   0x04u
#define WN_MOD_META  0x08u

/* ABI v6: per-node interaction state bits. wn_node_set_state() accepts one or
 * more of these OR-ed together; wn_node_get_state() returns the active mask.
 * Hover and press stay single-node (the pointer has one cursor); selected is a
 * per-node bit, so a host may keep several nodes selected at once. Text input
 * focus is tracked by wn_app_focus_input()/wn_app_blur_input(), which also
 * set/clear WN_STATE_FOCUSED (ABI v7) on the focused node.
 *
 * ABI v7: WN_STATE_FOCUSED lets a host mark any node as focused (CSS `:focus`
 * styles). WN_STATE_DISABLED makes the node and its subtree inert to the
 * pointer machine; :disabled styles stored by the frontend start applying.
 * Both are additive bits, accepted by set_state and returned by get_state. */
#define WN_STATE_HOVER    0x01u
#define WN_STATE_PRESS    0x02u
#define WN_STATE_SELECTED 0x04u
#define WN_STATE_FOCUSED  0x08u
#define WN_STATE_DISABLED 0x10u

/* Reserved flags; must be 0 in v0. */
#define WN_FLAG_NONE 0u

/* Axis-aligned rectangle in view pixels. ABI v6: public so hosts can read
 * laid-out geometry through wn_node_get_rect(). */
typedef struct wn_rect {
    float x, y, w, h;
} wn_rect;

typedef struct wn_app  wn_app;
typedef struct wn_document wn_document;
typedef struct wn_view wn_view;

/* ABI v3: stable node handle for the dynamic UI API. 0 is never a valid node
 * (WN_NODE_INVALID).
 *
 * Handles are opaque. Internally a handle packs the arena slot generation in
 * the high 32 bits and the 1-based slot index in the low 32 bits:
 * `(generation << 32) | (index + 1)`. The generation changes whenever the slot
 * is reused, so a handle kept across wn_node_remove() (or removal of an
 * ancestor) stops resolving: wn_node_* calls reject it, navigation returns
 * WN_NODE_INVALID and events never report it. Handles stay valid until the node
 * (or an ancestor) is removed; do not guess or persist them across
 * wn_app_destroy() or a document replacement. */
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
    WN_EVENT_INPUT        = 11,
    /* ABI v7: generic IME composition on the focused single-line input. The
     * payload is UTF-8 in `target_id` (engine/event owned, valid only during
     * the callback): START and CANCEL carry NULL, COMPOSITION the current
     * composition string, COMMIT the final text inserted exactly once.
     * COMPOSITION puts its codepoint cursor in `key`; x/y/width/height carry
     * the caret rect in view pixels and `node` the focused input handle. A
     * final WM_CHAR that repeats a commit is swallowed by the core, so hosts
     * may forward both without inserting twice. */
    WN_EVENT_IME_START       = 12,
    WN_EVENT_IME_COMPOSITION = 13,
    WN_EVENT_IME_COMMIT      = 14,
    WN_EVENT_IME_CANCEL      = 15
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
     * ABI v6: delta_x carries horizontal wheel/trackpad scroll (positive
     * scrolls content right); the core still only applies the vertical axis. */
    float delta_x, delta_y;
    /* ABI v5: stable handle (wn_node_id) of the event target, or
     * WN_NODE_INVALID when the event does not target a node. Works for
     * compiled and cloned nodes: pointer events report the hit interactive
     * node, key/char events report the focused input. Unlike target_id it
     * stays usable for clones; combine it with the wn_node_* navigation
     * helpers to reach children. */
    wn_node_id node;
    /* ABI v6: pointer buttons. 0 = none (move/wheel/key events), 1 = left,
     * 2 = right, 3 = middle. A pointer click that lands on a non-interactive
     * node still reports that node in `node`; the core only changes
     * hover/press/selection for the left button. */
    uint8_t button;
    /* ABI v6: 1 for a normal press/release, 2 for the press of a double click
     * (WM_LBUTTONDBLCLK and the release paired with it). 0 when the event is
     * not a button event. */
    uint8_t click_count;
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

/* Loads a compiled document (.wnui). The file is mapped read-only.
 *
 * ABI v6: documents are reference counted. wn_document_load_blob() returns one
 * reference owned by the caller; wn_document_retain() adds one and
 * wn_document_release()/wn_document_destroy() drop one. The builtin embedded
 * document has a static reference count, so retaining/releasing it is a no-op.
 * Every core retains the document it was created from, which means the caller
 * may release its reference immediately after wn_core_create() while the core
 * keeps working. Releasing the same reference twice is clamped (it does not
 * crash, but it is still a caller bug). */
WN_API wn_document *wn_document_load_blob(const char *path_utf8);
WN_API void wn_document_retain(wn_document *document);
WN_API void wn_document_release(wn_document *document);
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
/* Class toggles stay name-based in v6: they resolve compiled document ids only
 * (clones have no document name). The baked style values are fixed, so
 * "hover"/"press"/"selected"/"focus"/"disabled" are interpreted as the
 * per-node state bits; other names are remembered per node but do not
 * recompute CSS. Use wn_node_set_state() to drive state on an arbitrary
 * handle. */
WN_API wn_status wn_app_set_class(wn_app *app, const char *id, const char *class_name, int enabled);
WN_API wn_status wn_app_on_event(wn_app *app, wn_event_cb callback, void *userdata);

/* ABI v6: periodic frames for a host that needs animation/debounce. While
 * enabled (ms > 0) the standalone host starts an OS timer that marks the core
 * dirty and requests a frame at roughly that interval; 0 stops it. The caret
 * blink of a focused input keeps its own timer. */
WN_API void wn_app_set_tick_ms(wn_app *app, int ms);

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
 * Node handles are opaque (see wn_node_id). Lookup by name only resolves
 * compiled node names (the names in the embedded `.wnui` document); clone
 * handles are returned by wn_node_clone() and are not addressable by name in
 * v0.
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

/* --------------------------------------------- node state (ABI v6, v7 bits) */
/*
 * Per-node interaction state. `state` is a non-zero mask of WN_STATE_* bits
 * (hover, press, selected, focused, disabled); `enabled` sets or clears those
 * bits on `node`. Hover and press remain exclusive (the pointer has a single
 * cursor); selected and focused are per-node: a host may mark several nodes
 * selected or focused. Input focus drives WN_STATE_FOCUSED automatically.
 *
 * Painting resolves each color field independently with the documented
 * priority disabled > press > hover > focus > base. Selected is the app's base
 * color and only shows when no higher-priority state overrides that field, so
 * "selected + hover" shows the hover color. CSS state colors (blob v6) win over
 * the compiled data-* hover/press/selected colors; data-* remains the fallback
 * for nodes without CSS state styles.
 *
 * A disabled node (and its subtree) is inert to the pointer machine: no hover,
 * press or selection, and it cannot be focused. wn_node_get_rect() and the
 * diagnostic hit test still see it.
 * Returns WN_ERR_INVALID_ARGUMENT for a dead handle, a zero mask or an unknown
 * bit. wn_node_get_state() returns the active mask, or 0 for a dead handle.
 */
WN_API wn_status wn_node_set_state(wn_app *app, wn_node_id node, uint32_t state,
                                   int enabled);
WN_API uint32_t wn_node_get_state(wn_app *app, wn_node_id node);

/* -------------------------------------------------- node reads (ABI v6) */
/*
 * wn_node_get_text returns the effective text of a text node (the runtime
 * override when present, otherwise the compiled string). The buffer is owned
 * by the engine and stays valid until the next webnative call on this thread.
 * Non-text nodes without an override return NULL and set wn_last_error().
 *
 * wn_node_get_rect writes the laid-out border box (view px), forcing layout
 * first when the tree is dirty. Returns WN_OK or WN_ERR_INVALID_ARGUMENT.
 *
 * wn_node_scroll_into_view scrolls the nearest scrollable ancestor so the
 * node's box is inside its content box, leaving `margin` px of breathing room
 * where it fits. It is a no-op (WN_OK) when the node is already visible or has
 * no scrollable ancestor.
 */
WN_API const char *wn_node_get_text(wn_app *app, wn_node_id node);
WN_API wn_status wn_node_get_rect(wn_app *app, wn_node_id node, wn_rect *out);
WN_API wn_status wn_node_scroll_into_view(wn_app *app, wn_node_id node, float margin);

/* ------------------------------------------------- text input focus (ABI v6) */
/*
 * Focus an input programmatically with the caret at the end and no selection.
 * blur clears the current focus (idempotent). Neither emits WN_EVENT_INPUT or
 * WN_EVENT_FOCUS_*; they only change the editor state for the next frame.
 */
WN_API wn_status wn_app_focus_input(wn_app *app, wn_node_id node);
WN_API wn_status wn_app_blur_input(wn_app *app);

/* ------------------------------------------------- image size (ABI v6) */
/*
 * Override the intrinsic size used by layout/object-fit after wn_node_set_image
 * (0, 0 = unknown). Useful when the host knows the new file's pixel size: an
 * image box without a CSS height then follows the new aspect ratio. Negative,
 * NaN or infinite sizes are rejected. The call marks layout dirty; the next
 * frame/render applies it. */
WN_API wn_status wn_node_set_image_size(wn_app *app, wn_node_id node,
                                        float width, float height);

/* -------------------------------------------------- text input (ABI v5) */
/*
 * Single-line UTF-8 text input. `node` must be a compiled `kind:"input"`
 * node (or a clone of one). Values are engine-owned UTF-8 copies; cursor and
 * selection indices are codepoints. Control characters below U+0020 (except
 * tab) and invalid UTF-8 return WN_ERR_UNSUPPORTED. The length is capped by
 * the compiled `maxlength` in codepoints (or 255 when absent) and by the
 * 255-byte buffer; longer values return WN_ERR_INVALID_ARGUMENT. set_value
 * places the caret at the end, clears the selection and does NOT emit
 * WN_EVENT_INPUT (the caller already knows). get_value returns an engine-owned
 * string valid until the next webnative call on this thread; NULL (and
 * wn_last_error) for an invalid node.
 *
 * Editing happens through wn_view_event()/the window loop: click focuses and
 * places the caret, WN_EVENT_CHAR inserts any valid codepoint, pointer drag
 * extends the selection, KEY_DOWN handles Backspace/Delete/Left/Right/Home/
 * End (+Shift selection) and Ctrl+A/C/X/V. Esc releases focus. Enter is left
 * to the host (no form/submit). IME composition is not implemented.
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
