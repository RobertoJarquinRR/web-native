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

#include "document/wn_document_internal.h"
#include "text/text_atlas.h"
#include "wn_core.h"

#define WN_MAX_CLASSES 8
#define WN_MAX_NODES 4096

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

/* Style extras are emitted only for pages that use them (the generated
 * header defines UI_STYLE_EXTRAS). Source pages without text-align,
 * line-height or margin:auto compile with the zero defaults, so their
 * geometry stays byte-identical. */
#if defined(UI_STYLE_EXTRAS)
#define WN_TEXT_ALIGN(n) ((n)->text_align)
#define WN_LINE_HEIGHT(n) ((n)->line_height)
#define WN_MARGIN_AUTO(n, bit) ((((n)->margin_auto) >> (bit)) & 1)
/* 1 = border-box (width/height include padding); 0/absent = content-box (CSS
 * default). Pages that don't declare box-sizing keep the browser behaviour. */
#define WN_BOX_SIZING(n) ((n)->box_sizing)
#define WN_FLEX_GROW(n) ((n)->flex_grow)
#define WN_FLEX_SHRINK(n) ((n)->flex_shrink)
#define WN_FLEX_BASIS(n) ((n)->flex_basis)
#else
#define WN_TEXT_ALIGN(n) 0
#define WN_LINE_HEIGHT(n) 0.0f
#define WN_MARGIN_AUTO(n, bit) 0
#define WN_BOX_SIZING(n) 0
#define WN_FLEX_GROW(n) 0.0f
#define WN_FLEX_SHRINK(n) 1.0f
#define WN_FLEX_BASIS(n) (-1.0f)
#endif

/* Outer (border-box) width/height of the declared size. */
#define WN_DECL_W(n) ((WN_BOX_SIZING(n)) ? 0.0f : ((n)->padding[1] + (n)->padding[3]))
#define WN_DECL_H(n) ((WN_BOX_SIZING(n)) ? 0.0f : ((n)->padding[0] + (n)->padding[2]))

#ifndef R2_NODE_INPUT
#define R2_NODE_INPUT 5
#endif

struct wn_core {
    const wn_document *doc;

    /* Mutable node arena. [0, compiled_count) are copies of the compiled nodes;
     * slots from compiled_count up are cloned at runtime. Removed slots are
     * recycled through free_list. */
    UiNode *nodes;
    const char **names;          /* compiled names are borrowed from wn_document */
    int node_count;              /* high-water mark */
    int live_count;
    int node_capacity;
    unsigned char *alive;
    unsigned char *dynamic_node;
    unsigned char *visible;
    unsigned char *visible_eff;
    int *parent;
    int *order_key;
    int next_order;
    int *free_list;
    int free_count;
    int clone_serial;

    wn_rect *rects;
    wn_rect *clips;
    int *first_child;
    int *next_sibling;
    int *last_child;

    float *scroll;
    float *scroll_max;

    int hover;
    int press;
    int selected;

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
    float *input_scroll;
    int input_focus;

    int *paint_order;
    int paint_count;

    int dirty;
    int needs_layout;
};

/* Cross-module helpers. */
void build_hierarchy(wn_core *c);
void layout_page(wn_core *c, float viewport_w, float viewport_h);
void ensure_layout_for_hit(wn_core *core);

wn_rect rect_intersection(wn_rect a, wn_rect b);
int rect_is_empty(wn_rect r);

const WnInputRecord *input_record(const wn_core *c, int node);
float input_line_height(const wn_core *c, int node);
char *input_alloc_value(const char *text);
int build_input_state(wn_core *c);
void build_input_text_geometry(wn_core *c, float viewport_w, float viewport_h);
void build_input_decorations(wn_core *c);

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
