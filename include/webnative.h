/*
 * webnative.h — C ABI v8
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
 *        Same-version follow-ups: WN_STATE_FOCUS_VISIBLE (keyboard/programmatic
 *        focus), WN_STATE_FOCUS_WITHIN (engine-derived ancestor focus) and
 *        runtime layout overrides (wn_node_set_size/wn_node_set_position plus
 *        the WN_AUTO sentinel). No struct or event layout changed.
 *  - v7 additive (style lane): runtime text/background/font overrides
 *        (wn_node_set_color/background/font_family/font_size/font_weight,
 *        wn_node_reset_style, wn_app_set_font_family). They are a per-node
 *        override layer over the compiled document, with no runtime CSS and no
 *        blob change. Still v7: no struct or event layout changed.
 *  - v8: `<video>` and the optional media provider seam (docs/media-v1.md).
 *        Adds include/webnative_media.h (WN_MEDIA_ABI_VERSION 1), provider
 *        registration, wn_node_media_* controls and the wn_media_event family.
 *        Additive: no v7 struct changed layout. Blob v7 adds kind 6 (video) and
 *        the optional media section; there is no v6/v7 dual reader. */
#ifndef WEBNATIVE_H
#define WEBNATIVE_H

#include <stddef.h>
#include <stdint.h>

#include "webnative_media.h"

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

#define WN_ABI_VERSION 8u

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
 * Both are additive bits, accepted by set_state and returned by get_state.
 *
 * v7 follow-ups (additive, same version):
 * - WN_STATE_FOCUS_VISIBLE distinguishes keyboard/programmatic focus from
 *   pointer focus (CSS `:focus-visible`). A host marks keyboard focus by
 *   setting FOCUS_VISIBLE together with FOCUSED; a pointer click sets only
 *   FOCUSED. The engine sets FOCUS_VISIBLE on programmatic/keyboard focus and
 *   clears it on a pointer focus.
 * - WN_STATE_FOCUS_WITHIN is engine-derived (CSS `:focus-within`): the focused
 *   node and every ancestor carry it while a focus exists. It is recomputed on
 *   every focus change, subtree disable and remove/reinsert. It is read-only
 *   for hosts: wn_node_set_state() rejects a mask that contains it with
 *   WN_ERR_INVALID_ARGUMENT, and wn_node_get_state() reports it. */
#define WN_STATE_HOVER    0x01u
#define WN_STATE_PRESS    0x02u
#define WN_STATE_SELECTED 0x04u
#define WN_STATE_FOCUSED  0x08u
#define WN_STATE_DISABLED 0x10u
#define WN_STATE_FOCUS_VISIBLE 0x20u
#define WN_STATE_FOCUS_WITHIN  0x40u

/* Reserved flags; must be 0 in v0. */
#define WN_FLAG_NONE 0u

/* ABI v7: layout override sentinel.
 *
 * Pass WN_AUTO to wn_node_set_size()/wn_node_set_position() to restore the
 * compiled value for that axis. The value sits far outside any real UI
 * coordinate, so ordinary negative insets (for example left: -1px) remain
 * representable. Any negative width/height also restores the compiled size;
 * positions only reserve this exact sentinel. */
#define WN_AUTO (-1.0e6f)

/* ABI v7: runtime style override fields for wn_node_reset_style().
 *
 * Each setter records one field on one node; reset_style clears the selected
 * fields and the node falls back to its compiled value (and, for inherited
 * fields, to the nearest ancestor override). The mask is additive to v7: no
 * blob, struct or event changes. */
#define WN_STYLE_TEXT_COLOR  0x01u
#define WN_STYLE_BACKGROUND  0x02u
#define WN_STYLE_FONT_FAMILY 0x04u
#define WN_STYLE_FONT_SIZE   0x08u
#define WN_STYLE_FONT_WEIGHT 0x10u
#define WN_STYLE_ALL         0x1Fu

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

/* ------------------------------------------------------- media (ABI v8) */
/*
 * `<video>` is a first-class replaced element (blob v7 kind 6). The document
 * declares intent (`src`, `poster`, `controls`, `autoplay`, `loop`, `muted`,
 * `preload`); playback state belongs to the live core, never to the blob.
 *
 * This phase implements the element (layout/paint, poster/placeholder) and the
 * provider seam. There is no decoder, audio sink or player UI in the engine:
 * with the built-in media_null provider (always present) an open request ends
 * in WN_MEDIA_EVENT_ERROR with WN_MEDIA_UNSUPPORTED_CONTAINER, and that is a
 * normal, testable path. A host may register a provider for a later session
 * driver, but this build does not decode frames yet.
 *
 * Media events are a separate family from wn_event: the fields (state, error,
 * position, duration) are explicit instead of overloading `key`/`target_id`.
 * They are delivered on the UI thread from wn_view_render()/wn_view_tick(),
 * in order. `detail` is engine-owned and only valid during the callback.
 */
typedef enum wn_media_event_type {
    WN_MEDIA_EVENT_OPENED = 0,        /* duration, streams, intrinsic size */
    WN_MEDIA_EVENT_FIRST_FRAME = 1,   /* the poster can stop being painted */
    WN_MEDIA_EVENT_STATE = 2,         /* wn_media_state changed */
    WN_MEDIA_EVENT_PROGRESS = 3,      /* ~4 Hz while PLAYING; for the scrubber */
    WN_MEDIA_EVENT_ENDED = 4,
    WN_MEDIA_EVENT_ERROR = 5,
    WN_MEDIA_EVENT_AUDIO_DEVICE_LOST = 6,
    WN_MEDIA_EVENT_AUDIO_DEVICE_RESTORED = 7
} wn_media_event_type;

typedef struct wn_media_event {
    uint32_t struct_size;
    uint32_t type;             /* wn_media_event_type */
    wn_node_id node;           /* <video> node that originates it */
    int32_t  state;            /* wn_media_state after the event */
    int32_t  error;            /* wn_media_status; 0 when not applicable */
    int64_t  position_us;
    int64_t  duration_us;
    const char *detail;        /* UTF-8, callback lifetime only */
} wn_media_event;

typedef void (*wn_media_event_cb)(void *userdata, const wn_media_event *event);

/* Copied read of the per-node media state. Never blocks; valid in any state. */
typedef struct wn_media_state_info {
    uint32_t struct_size;
    int32_t  state;          /* wn_media_state */
    int32_t  error;          /* wn_media_status */
    int64_t  position_us;
    int64_t  duration_us;
    uint32_t stream_flags;   /* WN_MEDIA_INFO_* of the open file */
    float    volume;
    int32_t  muted, loop;
} wn_media_state_info;

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
 * priority disabled > press > hover > focus-visible > focus > focus-within >
 * base. Selected is the app's base color and only shows when no
 * higher-priority state overrides that field, so "selected + hover" shows the
 * hover color. CSS state colors (blob v6) win over the compiled data-*
 * hover/press/selected colors; data-* remains the fallback for nodes without
 * CSS state styles.
 *
 * A disabled node (and its subtree) is inert to the pointer machine: no hover,
 * press or selection, and it cannot be focused. wn_node_get_rect() and the
 * diagnostic hit test still see it.
 * Returns WN_ERR_INVALID_ARGUMENT for a dead handle, a zero mask, an unknown
 * bit, or a mask containing WN_STATE_FOCUS_WITHIN (engine-derived; read it via
 * wn_node_get_state()). wn_node_get_state() returns the active mask, or 0 for a
 * dead handle.
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

/* ------------------------------------------- layout overrides (ABI v7) */
/*
 * Runtime-only geometry for an existing node. The compiled .wnui file is never
 * modified; the override lives in the live core until the document is replaced
 * or the node is removed.
 *
 * wn_node_set_size sets the node's CSS width/height (the same box sized by
 * box-sizing; padding and margins are untouched). A negative width or height,
 * including WN_AUTO, restores the compiled value for that axis, including a
 * compiled percentage. Explicit sizes must be finite and >= 0.
 *
 * wn_node_set_position sets the horizontal and vertical offset of a positioned
 * node (relative/absolute/fixed/sticky). x/y become `left`/`top` pixels against
 * the node's existing containing block, and the opposite side on each axis
 * (right/bottom) is cleared. For sticky the same fields keep their CSS meaning
 * (the stick threshold on that axis). WN_AUTO for one axis restores the
 * compiled left/right or top/bottom pair, including its percentage bits. A
 * static node is rejected: compile it as positioned (or clone a positioned
 * node) first.
 *
 * Both calls mark layout and paint dirty; the next frame/render re-runs layout
 * and rebuilds the display list. An invalid app, node handle, property or
 * value returns WN_ERR_INVALID_ARGUMENT and changes nothing.
 */
WN_API wn_status wn_node_set_size(wn_app *app, wn_node_id node,
                                  float width, float height);
WN_API wn_status wn_node_set_position(wn_app *app, wn_node_id node,
                                      float x, float y);

/* ---------------------------------------------- style overrides (ABI v7) */
/*
 * Runtime-only visual styling for a live node. Like the layout overrides, the
 * compiled .wnui file is never modified: the values live in the core until the
 * document is replaced or the node is removed. There is no CSS cascade; this
 * is the host's programmatic equivalent of an inline style.
 *
 * Colors are 0xAARRGGBB. wn_node_set_color() sets the text color and
 * wn_node_set_background() the box background. A text color override inherits
 * down the subtree (a text run uses the nearest self-or-ancestor override), so
 * styling a nav/tab container also colors its generated text runs. Background
 * is per-node and never inherits.
 *
 * Font overrides use the same inheritance: wn_node_set_font_family(),
 * wn_node_set_font_size() and wn_node_set_font_weight() affect the node and its
 * descendants until a nearer override (or a compiled value, after a reset)
 * takes over. A change re-shapes/measures text and marks layout and paint
 * dirty; the next frame/render applies it. Family strings are copied by the
 * engine and must be non-empty, shorter than 96 bytes and free of control
 * characters; an uninstalled family resolves through the text backend's normal
 * fallback, exactly like a compiled font stack. Sizes are 1..1024 px; weights
 * are 100..900.
 *
 * wn_node_reset_style() clears the selected WN_STYLE_* fields on that node.
 * The node then uses its compiled value again (or an ancestor override for an
 * inherited field). Descendants are not reset; call it per node or use
 * wn_app_set_font_family(NULL, 1) to clear the app-wide family layer.
 *
 * wn_app_set_font_family() is the typography-setting entry point. `family` is
 * copied; NULL restores every compiled family (with recursive != 0 it clears
 * every per-node family override in the tree, otherwise only the root's). With
 * recursive != 0 a non-NULL family is written to the root and every live
 * descendant, overriding per-node families; with recursive == 0 only the root
 * is set and descendants inherit it unless they carry their own override.
 *
 * The override layer has higher priority than the compiled base and the
 * compiled state styles (hover/press/focus/disabled): it behaves like an
 * inline style. Colors cannot be malformed; the invalid-argument path is the
 * app/node handle. A dead handle, a NULL app, an empty/oversized family, a
 * non-finite or out-of-range size, a weight outside 100..900, or a zero/unknown
 * reset mask returns WN_ERR_INVALID_ARGUMENT and changes nothing.
 */
WN_API wn_status wn_node_set_color(wn_app *app, wn_node_id node, uint32_t argb);
WN_API wn_status wn_node_set_background(wn_app *app, wn_node_id node,
                                        uint32_t argb);
WN_API wn_status wn_node_set_font_family(wn_app *app, wn_node_id node,
                                         const char *family);
WN_API wn_status wn_node_set_font_size(wn_app *app, wn_node_id node, float size);
WN_API wn_status wn_node_set_font_weight(wn_app *app, wn_node_id node, int weight);
WN_API wn_status wn_node_reset_style(wn_app *app, wn_node_id node,
                                     uint32_t fields);
WN_API wn_status wn_app_set_font_family(wn_app *app, const char *family,
                                        int recursive);

/* ----------------------------------------------------------- media (v8) */
/*
 * Provider registration is per app/view, never global. The registry always
 * ends in the built-in media_null provider (priority 0, never matches), which
 * cannot be removed or duplicated. Registration order is descending priority,
 * then registration order. A provider vtable is validated before it is copied:
 * struct_size must cover receive_audio, abi_version must be
 * WN_MEDIA_ABI_VERSION, name must be non-empty, and caps/open/close/destroy
 * must be non-null. Unregister removes an explicit registration; media_null is
 * not removable. load_* follows the plugin allowlist rules in docs/media-v1.md
 * and never loads code from a document path.
 *
 * wn_node_media_* require a kind==video node (blob v7 kind 6); any other kind
 * returns WN_ERR_UNSUPPORTED, and a dead handle returns
 * WN_ERR_INVALID_ARGUMENT. Without a real provider the calls are safe:
 * open/play accept the request and report WN_MEDIA_EVENT_ERROR with
 * WN_MEDIA_UNSUPPORTED_CONTAINER from the next render/tick; nothing pretends
 * to play. set_source replaces the runtime source and returns to IDLE; the
 * document is never modified. Subtitle selection/delay are part of the
 * contract but return WN_ERR_UNSUPPORTED in this phase (no subtitle pipeline).
 *
 * wn_node_media_get_state is a copied struct, never a pointer into the engine,
 * and is valid in every state. volume is 0..1; muted starts from the document
 * flag and, like loop, is runtime state (never written back to the blob).
 */
WN_API wn_status wn_app_register_media_provider(wn_app *app,
        const wn_media_provider_ops *ops, void *context, int32_t priority);
WN_API wn_status wn_app_unregister_media_provider(wn_app *app,
        const wn_media_provider_ops *ops, void *context);
WN_API wn_status wn_app_load_media_provider(wn_app *app, const char *dll_path);
WN_API wn_status wn_view_register_media_provider(wn_view *view,
        const wn_media_provider_ops *ops, void *context, int32_t priority);
WN_API wn_status wn_view_unregister_media_provider(wn_view *view,
        const wn_media_provider_ops *ops, void *context);
WN_API wn_status wn_view_load_media_provider(wn_view *view, const char *dll_path);

WN_API wn_status wn_app_on_media_event(wn_app *app, wn_media_event_cb cb,
                                       void *userdata);
WN_API wn_status wn_view_on_media_event(wn_view *view, wn_media_event_cb cb,
                                        void *userdata);

WN_API wn_status wn_node_media_open(wn_app *app, wn_node_id node);
WN_API wn_status wn_node_media_play(wn_app *app, wn_node_id node);
WN_API wn_status wn_node_media_pause(wn_app *app, wn_node_id node);
WN_API wn_status wn_node_media_seek(wn_app *app, wn_node_id node,
                                    int64_t position_us);
WN_API wn_status wn_node_media_set_volume(wn_app *app, wn_node_id node,
                                          float gain);
WN_API wn_status wn_node_media_set_muted(wn_app *app, wn_node_id node, int muted);
WN_API wn_status wn_node_media_set_loop(wn_app *app, wn_node_id node, int loop);
WN_API wn_status wn_node_media_set_source(wn_app *app, wn_node_id node,
                                          const char *path_utf8);
WN_API wn_status wn_node_media_select_subtitle(wn_app *app, wn_node_id node,
                                               int32_t stream_index);
WN_API wn_status wn_node_media_set_subtitle_delay(wn_app *app, wn_node_id node,
                                                  int32_t delay_ms);
WN_API wn_status wn_node_media_get_state(wn_app *app, wn_node_id node,
                                         wn_media_state_info *out);

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
