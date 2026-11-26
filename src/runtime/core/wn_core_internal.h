/*
 * wn_core_internal.h - state shared by the core translation units.
 *
 * One struct per core instance, the compiled input spec, the style macros and
 * the helpers that more than one core module calls. Everything else stays
 * static in the file that owns it.
 */
#ifndef WN_CORE_INTERNAL_H
#define WN_CORE_INTERNAL_H

#include <stdint.h>

#include "document/wn_blob.h"
#include "document/wn_document_internal.h"
#include "text/text_atlas.h"
#include "wn_core.h"

/* The on-disk node record is a fixed contract shared with the Rust writer and
 * the blob loader. A field added without bumping both sides must fail here. */
_Static_assert(sizeof(WnBlobNode) == 252, "WnBlobNode must stay 252 bytes (v5)");

#define WN_MAX_CLASSES 8

/* Clear one state bit without a truncating constant cast (MSVC /W4 warns on
 * `(unsigned char)~WN_STATE_x`). */
#define WN_STATE_CLEAR(value, bit)                                           \
    ((unsigned char)((value) & (unsigned char)(0xFFu ^ (unsigned int)(bit))))

/* The node arena starts at WN_NODE_MIN_CAPACITY slots and grows on demand in
 * 1.5x steps, so a host can materialise large lists. WN_NODE_MAX_CAPACITY keeps
 * the int index arithmetic and the per-array byte sizes bounded. */
#define WN_NODE_MIN_CAPACITY 512
#define WN_NODE_MAX_CAPACITY (1 << 20)

/* 2x3 affine matrix in view pixels:
 *   x' = m0*x + m2*y + m4
 *   y' = m1*x + m3*y + m5
 * Identity is {1,0,0,1,0,0}. Only the geometry helpers consume it; the clip
 * rect in WnPaintItem stays screen-space (see the paint list notes below). */
typedef struct {
    float m0, m1, m2, m3, m4, m5;
} wn_transform;

#define WN_TRANSFORM_IDENTITY_INIT {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}

/* One entry of the ordered paint list built after layout. Today every item is
 * identity (layer 0, opacity 1, identity transform), so the batches it drives
 * are byte-identical to the historical DFS order. The fields exist so
 * z-index/transform/opacity lanes can change one value without re-plumbing the
 * builders:
 *   - clip: the node's screen-space scissor from compute_clips(). Transform is
 *     applied to geometry only; a transformed node stays clipped by its
 *     axis-aligned parent clip until the clipping lane lands.
 *   - layer: group layer id from the command list; 0 is the root layer. The
 *     stable sort by layer is a no-op for opacity-free pages, where every item
 *     stays 0.
 *   - opacity: direct-alpha factor for items on the flat path and the fallback
 *     when the layer cap is reached. A group root keeps 1 here because its
 *     layer carries the opacity. Multiplied into solid and glyph alpha and into
 *     the image vertex alpha.
 *   - transform: applied to solid/shadow/glyph/image vertex positions in pixel
 *     space before the clip-space projection.
 * Synthetic passes (input decorations, scrollbars) are not nodes and stay
 * post-passes in paint.c; they borrow the owning item's clip and attrs. */
typedef struct {
    int node;
    wn_rect clip;
    int layer;
    float opacity;
    wn_transform transform;
} WnPaintItem;

/* ui-tree/3 text input. Only the spec lives in the document; the text is
 * shaped and rasterized by the runtime like any other string. */
typedef struct {
    int node;
    const char *placeholder;
    int max_length;
} WnInputRecord;

#define WN_INPUT_MAX_BYTES 255
#define WN_INPUT_FOCUS_COLOR 0xFF3B82F6u
#define WN_INPUT_SELECTION_COLOR 0x663B82F6u
#define WN_INPUT_PLACEHOLDER_COLOR 0xFF6B7785u
#define WN_INPUT_CARET_WIDTH 2.0f

/* The runtime always carries the E1/CSS style extras (UI_STYLE_EXTRAS is set
 * by wn_document_format.h). Blob v2 writes every field, so pages that do not
 * use them keep the zero defaults and their geometry stays byte-identical. */
#define WN_TEXT_ALIGN(n) ((n)->text_align)
#define WN_LINE_HEIGHT(n) ((n)->line_height)
#define WN_MARGIN_AUTO(n, bit) ((((n)->margin_auto) >> (bit)) & 1)
/* 1 = border-box (width/height include padding); 0 = content-box (CSS
 * default). Pages that don't declare box-sizing keep the browser behaviour. */
#define WN_BOX_SIZING(n) ((n)->box_sizing)
#define WN_FLEX_GROW(n) ((n)->flex_grow)
#define WN_FLEX_SHRINK(n) ((n)->flex_shrink)
#define WN_FLEX_BASIS(n) ((n)->flex_basis)

/* Outer (border-box) width/height of the declared size. */
#define WN_DECL_W(n) ((WN_BOX_SIZING(n)) ? 0.0f : ((n)->padding[1] + (n)->padding[3]))
#define WN_DECL_H(n) ((WN_BOX_SIZING(n)) ? 0.0f : ((n)->padding[0] + (n)->padding[2]))

/* Blob v4 positioning. `inset` order is top,right,bottom,left. */
#define WN_POSITION(n) ((n)->position)
#define WN_IS_ABSOLUTE(n) ((n)->position == R2_POSITION_ABSOLUTE)
#define WN_IS_POSITIONED(n) ((n)->position != R2_POSITION_STATIC)
#define WN_INSET_SET(n, side) ((((n)->inset_mask) >> (side)) & 1u)
#define WN_INSET_PCT(n, side) ((((n)->inset_mask) >> (4u + (side))) & 1u)

#ifndef R2_NODE_INPUT
#define R2_NODE_INPUT 5
#endif

struct wn_core {
    const wn_document *doc;

    /* Mutable node arena. [0, compiled_count) are copies of the compiled nodes;
     * slots from compiled_count up are cloned at runtime. Removed slots are
     * recycled through free_list; generation[i] invalidates handles that were
     * issued before the slot was reused. */
    UiNode *nodes;
    const char **names;          /* compiled names are borrowed from wn_document */
    int node_count;              /* high-water mark */
    int live_count;
    int node_capacity;
    uint32_t *generation;
    unsigned char *alive;
    unsigned char *dynamic_node;
    unsigned char *visible;
    unsigned char *visible_eff;
    /* ABI v6: WN_STATE_HOVER|PRESS|SELECTED bits per slot. hover/press below
     * cache the single pointer-driven node; selected is read from this mask so
     * several nodes can stay selected. */
    unsigned char *state;
    int *parent;
    int *order_key;
    int next_order;
    int *free_list;
    int free_count;
    int clone_serial;
    int hierarchy_rebuilds;      /* build_hierarchy() calls; diagnostics */

    wn_rect *rects;
    wn_rect *clips;
    int *first_child;
    int *next_sibling;
    int *prev_sibling;
    int *last_child;

    /* Wheel/trackpad offsets per axis. scroll_max_* is the bound the layout
     * pass computes. Horizontal is reserved in this lane: layout keeps
     * scroll_max_x at 0 for every node, so the x half of the scroll service is
     * inert while the vertical behaviour stays exactly as it was. */
    float *scroll_x;
    float *scroll_y;
    float *scroll_max_x;
    float *scroll_max_y;

    int hover;
    int press;

    wn_vertex *vertices;
    int vertex_count;
    int vertex_capacity;
    wn_draw_batch *solid_batches;
    int solid_batch_count;
    int solid_batch_capacity;

    wn_glyph_vertex *glyph_vertices;
    int glyph_vertex_count;
    int glyph_vertex_capacity;
    wn_draw_batch *glyph_batches;
    int glyph_batch_count;
    int glyph_batch_capacity;

    wn_image_vertex *image_vertices;
    int image_vertex_count;
    int image_vertex_capacity;
    wn_image_batch *image_batches;
    int image_batch_count;
    int image_batch_capacity;

    wn_display_list list;

    int width;
    int height;
    double last_layout_ms;

    char **text_override;
    char **image_override;

    char **classes;
    unsigned char *class_count;

    wn_text_atlas *text_atlas;
    /* ui-tree/3 text input state. `input_map[node]` indexes input_records
     * (shared by clones); -1 means "not an input". */
    WnInputRecord *input_records;
    int input_record_count;
    int *input_map;
    char **input_value;
    int *input_cursor;
    int *input_anchor;
    float *input_scroll;         /* editor-local text offset, not page scroll */
    int input_focus;
    int input_drag;
    double caret_blink_ms;

    /* Ordered paint list: visible nodes with a non-empty clip, DFS order, then
     * a stable sort by layer. Capacity tracks node_capacity. */
    WnPaintItem *paint_items;
    int paint_item_count;
    int paint_item_capacity;
    /* Group layers and command list, allocated lazily: a page with no
     * `opacity < 1` group keeps these NULL and the renderers' historical
     * single-pass fast path. `paint_draws` is per paint item and only built
     * when `paint_layer_count > 1`. */
    wn_paint_layer *paint_layers;
    int paint_layer_count;
    int paint_layer_capacity;
    wn_paint_command *paint_commands;
    int paint_command_count;
    int paint_command_capacity;
    wn_paint_draw *paint_draws;
    int paint_draw_capacity;
    /* Geometry helper context: the item whose transform/opacity the push_*
     * helpers apply. NULL means identity/opaque (also the post-build reset). */
    const WnPaintItem *paint_item;

    int dirty;
    int needs_layout;
};

/* Cross-module helpers. Mutations link/unlink the child lists incrementally;
 * build_hierarchy() is only the initial O(n) pass over the compiled document. */
void build_hierarchy(wn_core *c);
void layout_page(wn_core *c, float viewport_w, float viewport_h);
void ensure_layout_for_hit(wn_core *core);

wn_rect rect_intersection(wn_rect a, wn_rect b);
int rect_is_empty(wn_rect r);

const WnInputRecord *input_record(const wn_core *c, int node);
float input_line_height(const wn_core *c, int node);
char *input_alloc_value(const char *text);
void wn_core_input_drag_to(wn_core *core, int node, float pointer_x);
int wn_core_caret_visible(const wn_core *core);
void wn_core_set_caret_epoch(wn_core *core, double ms);
int build_input_state(wn_core *c);
void build_input_text_geometry(wn_core *c, float viewport_w, float viewport_h);
void build_input_decorations(wn_core *c);

/* Per-item geometry ranges for the layer command model (NULL when the page
 * has no group layers). Defined in paint.c; input.c records its synthetic
 * passes there too. */
wn_paint_draw *wn_core_draw_for_item(wn_core *c, int item);

/* Paint list maintenance (layout.c). The item build walks the same visible
 * subtrees as the old DFS order and skips empty clips. The sort is a stable
 * merge sort; it returns the number of element copies (0 when all layers are
 * equal, the production fast path) so tests can assert the no-copy path. */
int wn_core_sort_paint_items(wn_core *c);
/* Culling helper: transformed node AABB vs the viewport plus a non-empty clip.
 * Computed and tested, never applied by the item build: a box outside the
 * viewport can still cast a box-shadow into it, so dropping the item is not
 * pixel-safe until shadows join the visibility test. */
int wn_paint_item_visible(const wn_core *c, const WnPaintItem *item);
/* Publishes the item whose transform/opacity the push_* helpers apply. NULL
 * restores the identity/opaque fast path. */
void wn_paint_use_item(wn_core *c, const WnPaintItem *item);

void push_quad(wn_core *c, float x, float y, float w, float h,
               float vw, float vh, unsigned int argb);
void push_rounded_ring(wn_core *c, float x, float y, float w, float h,
                       float radius, float ring, unsigned int argb);
void push_glyph_quad(wn_core *c, float x, float y, float w, float h,
                     float u0, float v0, float u1, float v1,
                     float vw, float vh,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void set_solid_clip(wn_core *c, const wn_rect *clip);
void set_glyph_clip(wn_core *c, const wn_rect *clip);

#endif /* WN_CORE_INTERNAL_H */
