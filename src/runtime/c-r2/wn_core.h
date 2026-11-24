/*
 * wn_core.h - platform/GPU independent R2 engine core.
 *
 * Owns the compiled document (the pre-layout style tree emitted at build time),
 * runtime layout, hover/press/selected state, hit testing and the display list.
 * It knows nothing about Win32, wgpu or the standalone/embedded mode.
 *
 * One core instance per UI thread. The document tables are immutable and shared;
 * all mutable state lives in wn_core.
 */
#ifndef WN_CORE_H
#define WN_CORE_H

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct {
    float x, y, w, h;
} wn_rect;

/* State slots used by wn_core_set_state(). */
typedef enum wn_state_kind {
    WN_STATE_HOVER = 0,
    WN_STATE_PRESS = 1,
    WN_STATE_SELECTED = 2
} wn_state_kind;

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

/* Image vertex: vec2 clip position + vec2 uv (16 bytes). */
typedef struct {
    float x, y, u, v;
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

/* Immutable atlas description attached to a document. */
typedef struct {
    const unsigned char *pixels;   /* R8 coverage */
    int width;
    int height;
    int size;                      /* width * height */
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
    wn_atlas_view atlas;
    float clear_color[4];
    int width, height;
} wn_display_list;

/* Opaque compiled document. v0 ships exactly one: the built-in R2 fixture
 * compiled into ui_data.h. The document tables stay immutable; wn_core owns a
 * mutable copy of the node table plus a dynamic arena for cloned nodes. */
typedef struct wn_document wn_document;

const wn_document *wn_document_builtin(void);
int wn_document_node_count(const wn_document *document);

typedef struct wn_core wn_core;

wn_core *wn_core_create(const wn_document *document);
void wn_core_destroy(wn_core *core);

/* node_count is the arena high-water mark (compiled nodes + slots ever used).
 * live_node_count excludes removed slots and is what the ABI reports. */
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
 * pointer and applies (delta_x ignored in v2, delta_y in px; positive scrolls
 * content down). Re-runs layout and clamps to [0, content_h - view_h].
 * Returns the scroll container node when the offset changed, -1 otherwise. */
int wn_core_wheel(wn_core *core, float x, float y, float delta_x, float delta_y);

/* Scroll diagnostics. Return -1 for an invalid/non-scrollable node. */
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

/* Runtime text override.
 * Returns 0 on success, 1 when the text cannot be represented with the
 * pre-shaped glyphs of this document/font size (WN_ERR_UNSUPPORTED at the ABI
 * layer), -1 for an invalid node/argument.
 * Layout keeps the compiled text metrics; v0 has no runtime shaping. */
int wn_core_set_text(wn_core *core, int node, const char *text);
int wn_core_text_override_supported(const wn_core *core, int node,
                                    const char *text);

/* Class toggles. "hover", "press" and "selected" drive R2 state colors.
 * Other class names are remembered per node but do not recompute styles
 * (styles are baked at build time). */
int wn_core_set_class(wn_core *core, int node, const char *class_name, int enabled);

/* ------------------------------------------------------ dynamic UI (v0) */
/*
 * Mutable tree operations used by the ABI layer. All int node ids are core
 * indices; the ABI maps them to wn_node_id (index + 1). Mutations mark the core
 * dirty; call wn_core_relayout() before painting (wn_view_render does it
 * automatically on the next frame).
 */

/* 0 ok, -1 invalid argument/state, -2 arena exhausted. */
int wn_core_set_visible(wn_core *core, int node, int visible);

/* 0 ok, 1 unsupported (node is not NODE_IMAGE), -1 invalid argument,
 * -2 out of memory. */
int wn_core_set_image(wn_core *core, int node, const char *path);

/* Deep clone. Returns the new node index or -1 (invalid) / -2 (arena full). */
int wn_core_clone(wn_core *core, int template_node, int parent);
int wn_core_remove(wn_core *core, int node);
int wn_core_clear_children(wn_core *core, int node);
int wn_core_append(wn_core *core, int parent, int child);

int wn_core_dirty(const wn_core *core);
int wn_core_needs_layout(const wn_core *core);
void wn_core_clear_dirty(wn_core *core);

/* Test/debug hooks: force a state without going through the pointer machine. */
void wn_core_set_state(wn_core *core, int node, int state, int enabled);

double wn_core_last_layout_ms(const wn_core *core);

#if defined(__cplusplus)
}
#endif

#endif /* WN_CORE_H */
