/*
 * wn_core.h - platform/GPU independent R2 engine core.
 *
 * Retains the compiled document (it may outlive the caller's reference),
 * owns runtime layout, per-node hover/press/selected state, hit testing and
 * the display list. It knows nothing about Win32, wgpu or the standalone or
 * embedded mode.
 *
 * One core instance per UI thread. The document tables are immutable and
 * shared; all mutable state lives in wn_core.
 */
#ifndef WN_CORE_H
#define WN_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "webnative.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* Solid quad vertex: vec2 clip position + unorm8x4 color (12 bytes). */
typedef struct {
    float x, y;
    uint8_t r, g, b, a;
} wn_vertex;

/* Textured glyph vertex: vec2 clip position + vec2 uv + unorm8x4 (20 bytes). */
typedef struct {
    float x, y, u, v;
    uint8_t r, g, b, a;
} wn_glyph_vertex;

/* Image vertex: vec2 clip position + vec2 uv + float alpha (20 bytes). The
 * shader multiplies the sampled texture alpha by it, so a leaf image with
 * `opacity < 1` fades without an offscreen layer. */
typedef struct {
    float x, y, u, v;
    float a;
} wn_image_vertex;

/* One scissored draw range into the solid/glyph vertex arrays. `clip` is the
 * pixel-space clip rect; a full-viewport rect means "no scissor". */
typedef struct {
    int first;
    int count;
    float clip_x, clip_y, clip_w, clip_h;
} wn_draw_batch;

/* One image draw: 6 vertices (two triangles) for a single texture + clip. */
typedef struct {
    int first;
    int count;
    float clip_x, clip_y, clip_w, clip_h;
    const char *src;
} wn_image_batch;

/* ----------------------------------------------------- group layers (v3) */
/* Stacking contexts are represented as paint layers. Two kinds exist:
 *
 *   - `needs_texture == 1`: a CS3 group (`opacity < 1`). The core renders its
 *     item and subtree into an offscreen layer and composites it once with
 *     `opacity`. Leaves are groups too; a leaf may paint several passes
 *     (border + fill, shadow + fill, input decorations, background + text or
 *     image) that must fade as one flattened result.
 *   - `needs_texture == 0`: an ordering-only context (`position: relative |
 *     absolute` with `z-index != auto`, and the root). It owns no texture: the
 *     renderer replays its items inline, which is enough because a stacking
 *     context only needs its block to stay contiguous inside the parent.
 *
 * `layers[0]` is the root layer. Layer ids are assigned in paint order;
 * `parent` is the enclosing layer/context. `commands` replay the nesting in
 * paint order: DRAW(item), BEGIN_LAYER(layer), END_LAYER(layer). */
#define WN_MAX_PAINT_LAYERS 65536
/* Nesting depth bound: matches the document tree depth limit, so a layer per
 * ancestor is always representable. Renderer target pools are per depth. */
#define WN_MAX_PAINT_LAYER_DEPTH 512

enum {
    WN_PAINT_CMD_DRAW = 1,
    WN_PAINT_CMD_BEGIN_LAYER = 2,
    WN_PAINT_CMD_END_LAYER = 3
};

typedef struct {
    int first;
    int count;
    wn_rect clip;
} wn_paint_range;

/* Per-item geometry ranges. Each pass (background, decorations, scrollbars,
 * images, text, input text) is contiguous both in the vertex arrays and in
 * item order, so a draw is a set of ranges. `clip` is the scissor the pass
 * used, which is not always the item clip (input content). */
typedef struct {
    int node;
    int layer;
    wn_paint_range solid;
    wn_paint_range decoration;
    wn_paint_range input_body;
    wn_paint_range scrollbar;
    wn_paint_range image;
    const char *image_src;
    wn_paint_range glyph;
    wn_paint_range input_glyph;
} wn_paint_draw;

typedef struct {
    int id;
    int parent;   /* -1 for the root layer */
    int node;     /* context-root node; -1 for the root layer */
    float opacity;
    wn_rect clip;
    /* 1: render into an offscreen target and composite (opacity group).
     * 0: ordering-only stacking context (z-index); replay inline. */
    int needs_texture;
} wn_paint_layer;

typedef struct {
    int kind;   /* WN_PAINT_CMD_* */
    int item;   /* DRAW: item index, -1 otherwise */
    int layer;  /* BEGIN/END: layer id, -1 otherwise */
} wn_paint_command;

/* Immutable atlas description attached to a document. */
typedef struct {
    const unsigned char *pixels;   /* R8 coverage */
    int width;
    int height;
    int size;                      /* width * height */
    unsigned int serial;           /* bumps whenever the pixels change in place */
} wn_atlas_view;

/* A display list is rebuilt by the core whenever layout or state changes.
 * The pointers are owned by the core and remain valid until the next
 * wn_core_build_display_list() call. */
typedef struct {
    const wn_vertex *vertices;
    int vertex_count;
    const wn_draw_batch *solid_batches;
    int solid_batch_count;
    const wn_glyph_vertex *glyph_vertices;
    int glyph_vertex_count;
    const wn_draw_batch *glyph_batches;
    int glyph_batch_count;
    const wn_image_vertex *image_vertices;
    int image_vertex_count;
    const wn_image_batch *image_batches;
    int image_batch_count;
    /* Group layers (blob v3 `opacity`). `layers[0]` is the root; the renderer
     * keeps its historical single-pass path while layer_count <= 1. `draws`
     * are per paint item and `commands` replay the nesting exactly. */
    const wn_paint_layer *layers;
    int layer_count;
    const wn_paint_command *commands;
    int command_count;
    const wn_paint_draw *draws;
    int draw_count;
    wn_atlas_view atlas;
    float clear_color[4];
    int width, height;
} wn_display_list;

/* Opaque compiled document. v0 ships exactly one: the built-in R2 fixture as
 * an embedded .wnui blob parsed at first use. The document tables stay
 * immutable; wn_core owns a mutable copy of the node table plus a dynamic
 * arena for cloned nodes. */
typedef struct wn_document wn_document;

const wn_document *wn_document_builtin(void);
/* Identity check against the cached builtin pointer. Allocation-free: unlike
 * wn_document_builtin() it never triggers the lazy parse. */
int wn_document_is_builtin(const wn_document *document);
/* Test/diagnostic hook: 1 once the embedded builtin has been parsed. Never
 * triggers a parse. */
int wn_document_builtin_parsed(void);
int wn_document_node_count(const wn_document *document);

typedef struct wn_core wn_core;

wn_core *wn_core_create(const wn_document *document);
void wn_core_destroy(wn_core *core);

/* node_count is the arena high-water mark (compiled nodes + slots ever used).
 * live_node_count excludes removed slots and is what the ABI reports. The
 * capacity starts at max(compiled_count, 512) and grows on demand. */
int wn_core_node_count(const wn_core *core);
int wn_core_live_node_count(const wn_core *core);
const char *wn_core_node_name(const wn_core *core, int node);
const char *wn_core_node_text(const wn_core *core, int node);
const char *wn_core_node_image_src(const wn_core *core, int node);
int wn_core_node_is_text(const wn_core *core, int node);
int wn_core_node_is_alive(const wn_core *core, int node);
int wn_core_node_is_visible(const wn_core *core, int node);
int wn_core_node_is_dynamic(const wn_core *core, int node);
int wn_core_node_parent(const wn_core *core, int node);
int wn_core_node_first_child(const wn_core *core, int node);
int wn_core_node_next_sibling(const wn_core *core, int node);
int wn_core_node_action(const wn_core *core, int node);

/* Number of full hierarchy rebuilds since creation. Structural mutations link
 * and unlink in O(1), so this only grows on wn_core_create(); a growing value
 * during cloning/removal means an O(n) rebuild crept back in. */
int wn_core_hierarchy_rebuilds(const wn_core *core);

/* Per-slot generation, bumped every time the slot is allocated. 0 for an
 * invalid or dead node. The ABI layer packs it into wn_node_id so a handle kept
 * across a removal cannot address the slot's next tenant. */
uint32_t wn_core_node_generation(const wn_core *core, int node);

/* Resolve an ABI id. Exact node names win. A bare last path component is
 * accepted only when it is unambiguous. Returns -1 when not found. */
int wn_core_find_node(const wn_core *core, const char *id);

void wn_core_resize(wn_core *core, int width, int height);
void wn_core_relayout(wn_core *core);
void wn_core_build_display_list(wn_core *core);
const wn_display_list *wn_core_display_list(const wn_core *core);
const wn_rect *wn_core_rects(const wn_core *core, int *count);

/* Hit testing in view coordinates. Returns an interactive node or -1. */
int wn_core_hit_test(const wn_core *core, float x, float y);

/* Topmost node whose clipped box contains the point (any node, -1 none).
 * Used by the wheel router and diagnostics. */
int wn_core_node_at(const wn_core *core, float x, float y);

/* Wheel/trackpad scroll: finds the nearest ancestor scroll container under the
 * pointer and applies (delta_x, delta_y in px; positive scrolls content right/
 * down). Both axes go through one internal clamp, but layout computes only the
 * vertical bound today, so horizontal delta_x is a no-op (ABI v6 keeps the
 * vertical behaviour). Re-runs layout and clamps to the axis max.
 * Returns the scroll container node when an offset changed, -1 otherwise. */
int wn_core_wheel(wn_core *core, float x, float y, float delta_x, float delta_y);

/* Scroll diagnostics (vertical offset/bound; the x pair is internal). Return
 * -1 for an invalid/non-scrollable node. */
float wn_core_scroll_offset(const wn_core *core, int node);
float wn_core_scroll_max(const wn_core *core, int node);

/* Effective paint clip for a node (viewport ∩ overflow ancestors). Returns 0
 * on success, -1 for an invalid node. Diagnostic/testing hook. */
int wn_core_node_clip(const wn_core *core, int node, wn_rect *out);

/* Pointer state machine. All return the hit node (or -1). pointer_up applies
 * the v0 click rule: press and release on the same ACTION_SELECT node selects
 * it. */
int wn_core_pointer_move(wn_core *core, float x, float y);
int wn_core_pointer_down(wn_core *core, float x, float y);
int wn_core_pointer_up(wn_core *core, float x, float y);
void wn_core_pointer_leave(wn_core *core);

int wn_core_hover(const wn_core *core);
int wn_core_press(const wn_core *core);
int wn_core_selected(const wn_core *core);

/* Per-node state bits (WN_STATE_*). wn_core_set_state applies one bit;
 * wn_core_node_state reports the live mask (0 for a dead node). */
int wn_core_node_state(const wn_core *core, int node);

/* 1 when `node` or any ancestor carries WN_STATE_DISABLED. */
int wn_core_node_disabled(const wn_core *core, int node);

/* Effective paint colors after CSS state styles and data-* sugar. The text
 * helper uses the parent's state mask for generated text runs. Diagnostic/
 * test hooks; paint uses the same functions. */
unsigned int wn_core_node_background(const wn_core *core, int node);
unsigned int wn_core_node_text_color(const wn_core *core, int node);
unsigned int wn_core_node_border_color(const wn_core *core, int node);

/* Laid-out border box, forcing layout first when the tree is dirty. */
int wn_core_node_rect(wn_core *core, int node, wn_rect *out);

/* Scrolls the nearest scrollable ancestor until `node` is visible. Returns 0
 * on success or when already visible; -1 for an invalid node. */
int wn_core_scroll_node_into_view(wn_core *core, int node, float margin);

/* Programmatic input focus with the caret at the end and no selection. Emits
 * no events (the ABI blur wrapper is idempotent). */
int wn_core_focus_input_at_end(wn_core *core, int node);

/* Runtime text override.
 * Returns 0 on success, 1 when the text contains control characters other
 * than tab (WN_ERR_UNSUPPORTED at the ABI layer), -1 for an invalid
 * node/argument. Glyphs missing from the document atlas are shaped and packed
 * into the runtime atlas on demand. */
int wn_core_set_text(wn_core *core, int node, const char *text);
int wn_core_text_override_supported(const wn_core *core, int node,
                                    const char *text);

/* Class toggles. "hover", "press" and "selected" drive the per-node state
 * bits (and therefore the compiled R2 state colors). Other class names are
 * remembered per node but do not recompute styles (styles are baked at build
 * time). */
int wn_core_set_class(wn_core *core, int node, const char *class_name, int enabled);

/* ------------------------------------------------------ dynamic UI (v0) */
/*
 * Mutable tree operations used by the ABI layer. All int node ids are core
 * indices; the ABI packs (generation, index) into wn_node_id. Mutations mark
 * the core dirty; call wn_core_relayout() before painting (wn_view_render does
 * it automatically on the next frame). The arena grows on demand.
 */

/* 0 ok, -1 invalid argument/state. */
int wn_core_set_visible(wn_core *core, int node, int visible);

/* 0 ok, 1 unsupported (node is not NODE_IMAGE), -1 invalid argument,
 * -2 out of memory. */
int wn_core_set_image(wn_core *core, int node, const char *path);

/* ABI v6: intrinsic size override for an image (0,0 = unknown). 0 ok,
 * 1 unsupported (not an image), -1 invalid argument. Marks layout dirty. */
int wn_core_set_image_size(wn_core *core, int node, float width, float height);

/* Deep clone. Returns the new node index or -1 (invalid) / -2 (out of memory or
 * beyond WN_NODE_MAX_CAPACITY). */
int wn_core_clone(wn_core *core, int template_node, int parent);
int wn_core_remove(wn_core *core, int node);
int wn_core_clear_children(wn_core *core, int node);
int wn_core_append(wn_core *core, int parent, int child);

int wn_core_dirty(const wn_core *core);
int wn_core_needs_layout(const wn_core *core);
void wn_core_clear_dirty(wn_core *core);

/* Virtual key codes used by the text-input editor (Win32 VK values; the ABI
 * documents `wn_event.key` as a virtual key). */
enum {
    WN_KEY_BACKSPACE = 8,
    WN_KEY_TAB = 9,
    WN_KEY_ENTER = 13,
    WN_KEY_ESCAPE = 27,
    WN_KEY_END = 35,
    WN_KEY_HOME = 36,
    WN_KEY_LEFT = 37,
    WN_KEY_UP = 38,
    WN_KEY_RIGHT = 39,
    WN_KEY_DOWN = 40,
    WN_KEY_DELETE = 46,
    WN_KEY_A = 65,
    WN_KEY_C = 67,
    WN_KEY_V = 86,
    WN_KEY_X = 88
};

/* Shift bit inside wn_event.modifiers (mirrors WN_MOD_SHIFT). */
#define WN_KEY_MOD_SHIFT 0x01u

/* Snapshot of one editable input for diagnostics/tests. `value`, `caret_x`,
 * `selection_x0` and `selection_x1` are valid until the next core call. */
typedef struct {
    const char *value;        /* UTF-8 buffer, engine owned */
    int cursor;               /* codepoint index 0..len */
    int anchor;               /* selection anchor (codepoint index), -1 none */
    int focused;
    int max_length;           /* 0 unlimited (hard cap 255) */
    float scroll;             /* horizontal text offset, px */
    float baseline;           /* ascender from the content box top */
    float line_height;
    float caret_x;            /* view px: text origin + caret - scroll */
    float selection_x0;       /* view px, == caret_x when empty */
    float selection_x1;
    wn_rect rect;             /* border box */
    wn_rect content;          /* content box (padding removed) */
} wn_input_state;

/* ------------------------------------------------------ text input (v5) */
/*
 * Single-line UTF-8 editor embedded in the core. The value buffer stays UTF-8
 * and cursor/anchor are codepoint indices. Hosts read the value through the
 * ABI (wn_node_get_value) or the helpers below. All editing entry points return
 * the affected node (or -1 when the key/char was not consumed) and report
 * whether the value or only the visual state changed.
 */
int wn_core_node_is_input(const wn_core *core, int node);
int wn_core_input_focused(const wn_core *core);
int wn_core_focus_input(wn_core *core, int node, float pointer_x);
int wn_core_blur_input(wn_core *core);

int wn_core_input_char(wn_core *core, uint32_t codepoint,
                       int *value_changed, int *visual_changed);
int wn_core_input_key(wn_core *core, int key, uint32_t modifiers,
                      int *value_changed, int *visual_changed);
int wn_core_input_select_all(wn_core *core, int *visual_changed);
/* Copy/cut the selection into a caller buffer (NUL terminated). Returns the
 * number of bytes copied, 0 when there is no selection, -1 when no input is
 * focused. */
int wn_core_input_copy(wn_core *core, char *out, int capacity);
int wn_core_input_cut(wn_core *core, char *out, int capacity,
                      int *value_changed, int *visual_changed);
/* Insert printable ASCII, truncating at max_length. */
int wn_core_input_paste(wn_core *core, const char *text,
                        int *value_changed, int *visual_changed);

/* 0 ok, 1 non-ASCII, 2 too long, -1 invalid node/argument (WN_ERR_* mapping
 * lives in the ABI layer). */
int wn_core_set_input_value(wn_core *core, int node, const char *text);
const char *wn_core_input_value(const wn_core *core, int node);
int wn_core_input_state(const wn_core *core, int node, wn_input_state *out);

/* Test/debug hooks: force a state without going through the pointer machine.
 * `state` is one WN_STATE_* bit. `wn_core_force_state_mask` ORs a whole mask
 * and skips the single-hover/press rule (offscreen pixel fixtures).
 * `wn_core_set_state` refuses HOVER/PRESS/FOCUSED on a node that is disabled
 * (itself or an ancestor); enabling DISABLED clears those bits across the whole
 * subtree and blurs an input focused inside it. */
void wn_core_set_state(wn_core *core, int node, int state, int enabled);
void wn_core_force_state_mask(wn_core *core, int node, unsigned int mask);

/* Marks the core dirty and asks layout/paint to run on the next frame
 * (wn_view_tick). */
void wn_core_mark_dirty(wn_core *core);

double wn_core_last_layout_ms(const wn_core *core);

#if defined(__cplusplus)
}
#endif

#endif /* WN_CORE_H */
