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
    const wn_glyph_vertex *glyph_vertices;
    int glyph_vertex_count;
    wn_atlas_view atlas;
    float clear_color[4];
    int width, height;
} wn_display_list;

/* Opaque compiled document. v0 ships exactly one: the built-in R2 fixture
 * compiled into ui_data.h. */
typedef struct wn_document wn_document;

const wn_document *wn_document_builtin(void);
int wn_document_node_count(const wn_document *document);

typedef struct wn_core wn_core;

wn_core *wn_core_create(const wn_document *document);
void wn_core_destroy(wn_core *core);

int wn_core_node_count(const wn_core *core);
const char *wn_core_node_name(const wn_core *core, int node);
const char *wn_core_node_text(const wn_core *core, int node);
int wn_core_node_is_text(const wn_core *core, int node);
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

/* Test/debug hooks: force a state without going through the pointer machine. */
void wn_core_set_state(wn_core *core, int node, int state, int enabled);

double wn_core_last_layout_ms(const wn_core *core);

#if defined(__cplusplus)
}
#endif

#endif /* WN_CORE_H */
