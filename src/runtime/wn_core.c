/*
 * wn_core.c - R2 layout, state, hit testing and display-list generation.
 *
 * This is the runtime half of R2 extracted from runtime_r2.c. Behaviour is
 * intentionally identical: same block/flex algorithm, same wrap shrink rule,
 * same reverse hit testing, same press > hover > selected color priority and
 * the same pre-shaped glyph quads.
 *
 * The compiled document comes from ui_data.h (build time). The core never
 * parses HTML/CSS and never touches a GPU or an OS window.
 */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_data.h"
#include "wn_core.h"
#include "wn_internal.h"

#define WN_MAX_GLYPH_SETS 8
#define WN_MAX_CLASSES 8
#define WN_MAX_NODES 4096

struct wn_document {
    const UiNode *nodes;
    int node_count;
    const char *const *names;
    const UiGlyph *glyphs;
    int glyph_count;
    const UiTextRun *runs;
    int run_count;
    const unsigned char *atlas;
    int atlas_width;
    int atlas_height;
    int viewport_width;
    int viewport_height;
};

static const wn_document g_builtin_document = {
    kUiNodes,
    UI_NODE_COUNT,
    kUiNodeNames,
    kUiGlyphs,
    UI_GLYPH_COUNT,
    kUiTextRuns,
    UI_TEXT_RUN_COUNT,
    kUiAtlas,
    UI_ATLAS_SIZE,
    UI_ATLAS_SIZE,
    UI_VIEWPORT_WIDTH,
    UI_VIEWPORT_HEIGHT
};

const wn_document *wn_document_builtin(void)
{
    return &g_builtin_document;
}

int wn_document_node_count(const wn_document *document)
{
    return document != NULL ? document->node_count : 0;
}

/* ------------------------------------------------------------ glyph cache */

typedef struct {
    int valid;
    float off_x, off_y;
    float w, h;
    float u0, v0, u1, v1;
    float advance;
} GlyphEntry;

typedef struct {
    int font_px;
    float space_advance;
    GlyphEntry ascii[128];
} GlyphSet;

typedef struct {
    char ch;
    int glyph; /* index into the document glyph array, -1 for a space */
} RunChar;

/* ui-tree/3 text input. The generated header (input pages only) carries a
 * fixed ASCII pool per unique font size; the core copies it into this compact
 * form so the rest of the engine compiles identically for pages without
 * inputs. */
typedef struct {
    float advance;
    float x, y, w, h;
    float u0, v0, u1, v1;
} WnInputGlyph;

typedef struct {
    int node;
    const char *placeholder;
    int max_length;
    float baseline;
    float line_height;
    const WnInputGlyph *glyphs;
    int glyph_count;
    short cp_index[128]; /* codepoint -> pool index, -1 none */
} WnInputRecord;

#define WN_INPUT_MAX_BYTES 255
#define WN_INPUT_FOCUS_COLOR 0xFF3B82F6u
#define WN_INPUT_SELECTION_COLOR 0x663B82F6u
#define WN_INPUT_PLACEHOLDER_COLOR 0xFF6B7785u
#define WN_INPUT_CARET_WIDTH 2.0f

/* E1 style extras are emitted only for pages that use them (the generated
 * header defines UI_STYLE_EXTRAS). Source pages without text-align,
 * line-height or margin:auto compile with the zero defaults, so their
 * geometry stays byte-identical. */
#if defined(UI_STYLE_EXTRAS)
#define WN_TEXT_ALIGN(n) ((n)->text_align)
#define WN_MARGIN_AUTO(n, bit) ((((n)->margin_auto) >> (bit)) & 1)
/* 1 = border-box (width/height include padding); 0/absent = content-box (CSS
 * default). Pages that don't declare box-sizing keep the browser behaviour. */
#define WN_BOX_SIZING(n) ((n)->box_sizing)
#define WN_FLEX_GROW(n) ((n)->flex_grow)
#define WN_FLEX_SHRINK(n) ((n)->flex_shrink)
#define WN_FLEX_BASIS(n) ((n)->flex_basis)
#else
#define WN_TEXT_ALIGN(n) 0
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
    int *run_first;
    int *run_count;
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

    GlyphSet *glyph_sets;
    int glyph_set_count;

    /* ui-tree/3 text input state. `input_map[node]` indexes input_records
     * (shared by clones); -1 means "not an input". */
    WnInputRecord *input_records;
    int input_record_count;
    WnInputGlyph *input_pool;
    int input_pool_count;
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

/* Max-content width. Flex items use their content width, not the container
 * width (documented R2 behaviour). */
static float intrinsic_width(wn_core *c, int node, float available);
static float layout_node(wn_core *c, int node, float origin_x, float origin_y,
                         float available_w, float parent_content_h,
                         int is_root, float forced_w);
static void layout_page(wn_core *c, float viewport_w, float viewport_h);
static const WnInputRecord *input_record(const wn_core *c, int node);
static float input_line_height(const wn_core *c, int node);
static void input_update_scroll(wn_core *c, int node);
static char *input_alloc_value(const char *text);

static void ensure_vertex_capacity(wn_core *c, int needed)
{
    if (needed <= c->vertex_capacity) {
        return;
    }
    int capacity = c->vertex_capacity > 0 ? c->vertex_capacity : 64;
    while (capacity < needed) {
        capacity *= 2;
    }
    wn_vertex *grown = (wn_vertex *)realloc(c->vertices, (size_t)capacity * sizeof(wn_vertex));
    if (grown != NULL) {
        c->vertices = grown;
        c->vertex_capacity = capacity;
    }
}

static void ensure_glyph_capacity(wn_core *c, int needed)
{
    if (needed <= c->glyph_vertex_capacity) {
        return;
    }
    int capacity = c->glyph_vertex_capacity > 0 ? c->glyph_vertex_capacity : 64;
    while (capacity < needed) {
        capacity *= 2;
    }
    wn_glyph_vertex *grown = (wn_glyph_vertex *)realloc(
        c->glyph_vertices, (size_t)capacity * sizeof(wn_glyph_vertex));
    if (grown != NULL) {
        c->glyph_vertices = grown;
        c->glyph_vertex_capacity = capacity;
    }
}

static void ensure_image_capacity(wn_core *c, int needed)
{
    if (needed <= c->image_vertex_capacity) {
        return;
    }
    int capacity = c->image_vertex_capacity > 0 ? c->image_vertex_capacity : 64;
    while (capacity < needed) {
        capacity *= 2;
    }
    wn_image_vertex *grown = (wn_image_vertex *)realloc(
        c->image_vertices, (size_t)capacity * sizeof(wn_image_vertex));
    if (grown != NULL) {
        c->image_vertices = grown;
        c->image_vertex_capacity = capacity;
    }
}

static void ensure_solid_batch_capacity(wn_core *c, int needed)
{
    if (needed <= c->solid_batch_capacity) {
        return;
    }
    int capacity = c->solid_batch_capacity > 0 ? c->solid_batch_capacity : 16;
    while (capacity < needed) {
        capacity *= 2;
    }
    wn_draw_batch *grown = (wn_draw_batch *)realloc(
        c->solid_batches, (size_t)capacity * sizeof(wn_draw_batch));
    if (grown != NULL) {
        c->solid_batches = grown;
        c->solid_batch_capacity = capacity;
    }
}

static void ensure_glyph_batch_capacity(wn_core *c, int needed)
{
    if (needed <= c->glyph_batch_capacity) {
        return;
    }
    int capacity = c->glyph_batch_capacity > 0 ? c->glyph_batch_capacity : 16;
    while (capacity < needed) {
        capacity *= 2;
    }
    wn_draw_batch *grown = (wn_draw_batch *)realloc(
        c->glyph_batches, (size_t)capacity * sizeof(wn_draw_batch));
    if (grown != NULL) {
        c->glyph_batches = grown;
        c->glyph_batch_capacity = capacity;
    }
}

static void ensure_image_batch_capacity(wn_core *c, int needed)
{
    if (needed <= c->image_batch_capacity) {
        return;
    }
    int capacity = c->image_batch_capacity > 0 ? c->image_batch_capacity : 16;
    while (capacity < needed) {
        capacity *= 2;
    }
    wn_image_batch *grown = (wn_image_batch *)realloc(
        c->image_batches, (size_t)capacity * sizeof(wn_image_batch));
    if (grown != NULL) {
        c->image_batches = grown;
        c->image_batch_capacity = capacity;
    }
}

/* --------------------------------------------------------------- document */

static const UiNode *node_at(const wn_core *c, int node)
{
    if (node < 0 || node >= c->node_count || !c->alive[node]) {
        return NULL;
    }
    return &c->nodes[node];
}

static UiNode *node_mut(wn_core *c, int node)
{
    if (node < 0 || node >= c->node_count || !c->alive[node]) {
        return NULL;
    }
    return &c->nodes[node];
}

int wn_core_node_count(const wn_core *core)
{
    return core != NULL ? core->node_count : 0;
}

int wn_core_live_node_count(const wn_core *core)
{
    return core != NULL ? core->live_count : 0;
}

const char *wn_core_node_name(const wn_core *core, int node)
{
    if (core == NULL || node < 0 || node >= core->node_count ||
        !core->alive[node]) {
        return NULL;
    }
    return core->names[node];
}

const char *wn_core_node_text(const wn_core *core, int node)
{
    if (core == NULL || node_at(core, node) == NULL) {
        return NULL;
    }
    if (core->text_override[node] != NULL) {
        return core->text_override[node];
    }
    return core->nodes[node].text;
}

const char *wn_core_node_image_src(const wn_core *core, int node)
{
    if (core == NULL || node_at(core, node) == NULL) {
        return NULL;
    }
    if (core->image_override[node] != NULL) {
        return core->image_override[node];
    }
    return core->nodes[node].image_src;
}

int wn_core_node_is_text(const wn_core *core, int node)
{
    const UiNode *n = core != NULL ? node_at(core, node) : NULL;
    return n != NULL && n->kind == R2_NODE_TEXT;
}

int wn_core_node_is_alive(const wn_core *core, int node)
{
    return core != NULL && node >= 0 && node < core->node_count &&
           core->alive[node] != 0;
}

int wn_core_node_is_visible(const wn_core *core, int node)
{
    if (!wn_core_node_is_alive(core, node)) {
        return 0;
    }
    return core->visible[node] != 0;
}

int wn_core_node_is_dynamic(const wn_core *core, int node)
{
    if (!wn_core_node_is_alive(core, node)) {
        return 0;
    }
    return core->dynamic_node[node] != 0;
}

int wn_core_node_parent(const wn_core *core, int node)
{
    if (!wn_core_node_is_alive(core, node)) {
        return -1;
    }
    return core->parent[node];
}

int wn_core_node_first_child(const wn_core *core, int node)
{
    if (!wn_core_node_is_alive(core, node)) {
        return -1;
    }
    return core->first_child[node];
}

int wn_core_node_next_sibling(const wn_core *core, int node)
{
    if (!wn_core_node_is_alive(core, node)) {
        return -1;
    }
    return core->next_sibling[node];
}

int wn_core_node_action(const wn_core *core, int node)
{
    const UiNode *n = core != NULL ? node_at(core, node) : NULL;
    return n != NULL ? n->action : 0;
}

static int name_matches_id(const char *name, const char *id)
{
    const char *base;
    if (name == NULL || id == NULL) {
        return 0;
    }
    if (strcmp(name, id) == 0) {
        return 1;
    }
    if (id[0] == '#') {
        ++id;
    }
    base = strrchr(name, '/');
    base = base != NULL ? base + 1 : name;
    return strcmp(base, id) == 0;
}

int wn_core_find_node(const wn_core *core, const char *id)
{
    int i;
    int match = -1;
    int matches = 0;
    if (core == NULL || id == NULL || id[0] == '\0') {
        return -1;
    }
    for (i = 0; i < core->node_count; ++i) {
        if (core->alive[i] && strcmp(core->names[i], id) == 0) {
            return i;
        }
    }
    for (i = 0; i < core->node_count; ++i) {
        if (core->alive[i] && name_matches_id(core->names[i], id)) {
            match = i;
            ++matches;
        }
    }
    return matches == 1 ? match : -1;
}

/* ------------------------------------------------------------- hierarchy */

/* Rebuild first_child/next_sibling/last_child from the parent links. Cheap
 * (O(nodes)) and called after every structural mutation. */
static void build_hierarchy(wn_core *c)
{
    int i;
    for (i = 0; i < c->node_count; ++i) {
        c->first_child[i] = -1;
        c->next_sibling[i] = -1;
        c->last_child[i] = -1;
        c->nodes[i].parent = c->parent[i];
    }
    for (i = 0; i < c->node_count; ++i) {
        int parent;
        int prev = -1;
        int walk;
        if (!c->alive[i]) {
            continue;
        }
        parent = c->parent[i];
        if (parent < 0 || parent >= c->node_count || !c->alive[parent]) {
            if (parent >= 0) {
                c->parent[i] = -1;
                c->nodes[i].parent = -1;
            }
            continue;
        }
        /* Insert sorted by order_key so append/re-parent keeps an explicit
         * child order even when arena slots are recycled. */
        walk = c->first_child[parent];
        while (walk >= 0 && c->order_key[walk] < c->order_key[i]) {
            prev = walk;
            walk = c->next_sibling[walk];
        }
        if (prev < 0) {
            if (c->first_child[parent] < 0) {
                c->last_child[parent] = i;
            }
            c->next_sibling[i] = c->first_child[parent];
            c->first_child[parent] = i;
        } else {
            c->next_sibling[i] = c->next_sibling[prev];
            c->next_sibling[prev] = i;
            if (c->next_sibling[i] < 0) {
                c->last_child[parent] = i;
            }
        }
    }
}

static void translate_subtree(wn_core *c, int node, float dx, float dy)
{
    int child;
    if (!c->alive[node] || !c->visible_eff[node]) {
        return;
    }
    c->rects[node].x += dx;
    c->rects[node].y += dy;
    for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
        translate_subtree(c, child, dx, dy);
    }
}

static void mark_subtree_hidden(wn_core *c, int node)
{
    int child;
    if (!c->alive[node]) {
        return;
    }
    c->visible_eff[node] = 0;
    c->rects[node].x = 0.0f;
    c->rects[node].y = 0.0f;
    c->rects[node].w = 0.0f;
    c->rects[node].h = 0.0f;
    c->clips[node] = c->rects[node];
    for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
        mark_subtree_hidden(c, child);
    }
}

static float intrinsic_width(wn_core *c, int node, float available)
{
    const UiNode *n = &c->nodes[node];
    if (n->width >= 0.0f) {
        return n->width + WN_DECL_W(n);
    }
    if (n->width_pct > 0.0f) {
        return available * n->width_pct * 0.01f + WN_DECL_W(n);
    }
    if (n->kind == R2_NODE_TEXT) {
        return n->width > 0.0f ? n->width : 0.0f;
    }
    if (n->kind == R2_NODE_IMAGE && n->image_width > 0.0f) {
        return n->image_width + n->padding[1] + n->padding[3];
    }
    if (n->kind == R2_NODE_GRID && n->grid_mode == R2_GRID_AUTO_FILL &&
        n->grid_column_px > 0.0f) {
        return n->grid_column_px + n->padding[1] + n->padding[3];
    }
    if (n->kind == R2_NODE_INPUT) {
        /* Ancho por defecto de un campo sin CSS: 20 caracteres aproximados,
         * acotado por max_length cuando existe. */
        const WnInputRecord *rec = input_record(c, node);
        float width = 200.0f;
        if (rec != NULL && rec->max_length > 0) {
            float estimated = (float)rec->max_length * n->font_size * 0.62f;
            width = fmaxf(80.0f, fminf(estimated, 480.0f));
        }
        return width + n->padding[1] + n->padding[3];
    }
    float content = 0.0f;
    if (n->kind == R2_NODE_FLEX && n->direction == 0) {
        float sum = 0.0f;
        int count = 0;
        int child;
        for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
            if (!c->alive[child] || !c->visible[child]) {
                continue;
            }
            sum += intrinsic_width(c, child, available);
            ++count;
        }
        if (count > 0) {
            sum += (n->gap > 0.0f ? n->gap : 0.0f) * (float)(count - 1);
        }
        content = sum;
    } else {
        float maxw = 0.0f;
        int child;
        for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
            float w;
            if (!c->alive[child] || !c->visible[child]) {
                continue;
            }
            w = intrinsic_width(c, child, available);
            if (w > maxw) {
                maxw = w;
            }
        }
        content = maxw;
    }
    return content + n->padding[1] + n->padding[3];
}

static float align_offset(int align, float extent, float size)
{
    if (align == ALIGN_CENTER) {
        return (extent - size) * 0.5f;
    }
    if (align == ALIGN_END) {
        return extent - size;
    }
    return 0.0f;
}

static void justify_offsets(int justify, float free, int count, float gap,
                            float *start, float *spacing)
{
    if (count <= 1) {
        *start = (justify == JUSTIFY_CENTER) ? free * 0.5f
               : (justify == JUSTIFY_END) ? free : 0.0f;
        *spacing = gap;
        return;
    }
    if (justify == JUSTIFY_CENTER) {
        *start = free * 0.5f;
        *spacing = gap;
    } else if (justify == JUSTIFY_END) {
        *start = free;
        *spacing = gap;
    } else if (justify == JUSTIFY_SPACE_BETWEEN) {
        *start = 0.0f;
        *spacing = gap + free / (float)(count - 1);
    } else {
        *start = 0.0f;
        *spacing = gap;
    }
}

static float layout_flex_children(wn_core *c, int parent, float content_x,
                                  float content_y, float content_w,
                                  float content_h)
{
    const UiNode *p = &c->nodes[parent];
    int *items;
    float *item_w;
    float *item_h;
    int *line_start;
    int *line_count;
    float *line_cross;
    float *line_main;
    char *block;
    int count = 0;
    int child;

    for (child = c->first_child[parent]; child >= 0; child = c->next_sibling[child]) {
        if (!c->alive[child] || !c->visible[child]) {
            continue;
        }
        ++count;
    }
    if (count == 0) {
        return 0.0f;
    }
    block = (char *)malloc((size_t)count * (3u * sizeof(int) + 4u * sizeof(float)));
    if (block == NULL) {
        return 0.0f;
    }
    items = (int *)block;
    line_start = items + count;
    line_count = line_start + count;
    item_w = (float *)(line_count + count);
    item_h = item_w + count;
    line_cross = item_h + count;
    line_main = line_cross + count;

    count = 0;
    for (child = c->first_child[parent]; child >= 0; child = c->next_sibling[child]) {
        if (!c->alive[child] || !c->visible[child]) {
            continue;
        }
        items[count] = child;
        ++count;
    }

    const int row = (p->direction == 0);
    const int wrap = (p->wrap != 0);
    const float gap = p->gap > 0.0f ? p->gap : 0.0f;
    int k;

    /* 1. Measure every item at (0,0). */
    for (k = 0; k < count; ++k) {
        int id = items[k];
        const UiNode *n = &c->nodes[id];
        float forced = -1.0f;
        float basis = WN_FLEX_BASIS(n);
        if (n->width < 0.0f && n->width_pct == 0.0f) {
            float margins = n->margin[1] + n->margin[3];
            float limit = fmaxf(content_w - margins, 0.0f);
            if (row) {
                forced = fminf(limit, intrinsic_width(c, id, content_w));
            } else if (p->align != ALIGN_STRETCH) {
                forced = fminf(limit, intrinsic_width(c, id, content_w));
            }
        }
        if (basis >= 0.0f) {
            if (row) {
                forced = basis + WN_DECL_W(n);
            } else {
                float saved = c->nodes[id].height;
                c->nodes[id].height = basis - WN_DECL_H(n);
                layout_node(c, id, 0.0f, 0.0f, content_w, content_h, 0, forced);
                c->nodes[id].height = saved;
                item_w[k] = c->rects[id].w;
                item_h[k] = c->rects[id].h;
                continue;
            }
        }
        layout_node(c, id, 0.0f, 0.0f, content_w, content_h, 0, forced);
        item_w[k] = c->rects[id].w;
        item_h[k] = c->rects[id].h;
    }

    /* 2. Break into lines (wrap) or use one line with up-to-20% shrink. */
    int lines = 0;
    if (wrap) {
        int start = 0;
        while (start < count) {
            int remaining = count - start;
            int take = 1;
            int used_shrink = 0;
            float shrink_scale = 1.0f;
            if (remaining == 1) {
                take = 1;
                float base = row ? item_w[start] : item_h[start];
                float limit = row ? content_w : content_h;
                if (limit < 0.0f) {
                    limit = base;
                }
                if (base > limit) {
                    shrink_scale = limit / base;
                    used_shrink = 1;
                }
            } else {
                for (k = remaining; k >= 2; --k) {
                    float sum = 0.0f;
                    int j;
                    for (j = start; j < start + k; ++j) {
                        sum += row ? item_w[j] : item_h[j];
                    }
                    float need = sum + gap * (float)(k - 1);
                    float limit = row ? content_w : content_h;
                    if (limit < 0.0f) {
                        continue;
                    }
                    if (need <= limit + 0.01f) {
                        take = k;
                        break;
                    }
                    float scale = (limit - gap * (float)(k - 1)) / sum;
                    if (scale >= 0.80f) {
                        take = k;
                        shrink_scale = scale;
                        used_shrink = 1;
                        break;
                    }
                }
                if (take == 1 && remaining > 1) {
                    float base = row ? item_w[start] : item_h[start];
                    float limit = row ? content_w : content_h;
                    if (limit >= 0.0f && base > limit) {
                        shrink_scale = limit / base;
                        used_shrink = 1;
                    }
                }
            }
            if (used_shrink) {
                int j;
                for (j = start; j < start + take; ++j) {
                    if (row) {
                        item_w[j] *= shrink_scale;
                    } else {
                        item_h[j] *= shrink_scale;
                    }
                }
            }
            line_start[lines] = start;
            line_count[lines] = take;
            line_main[lines] = 0.0f;
            float cross = 0.0f;
            int j;
            for (j = start; j < start + take; ++j) {
                line_main[lines] += row ? item_w[j] : item_h[j];
                float value = row ? item_h[j] : item_w[j];
                if (value > cross) {
                    cross = value;
                }
            }
            line_main[lines] += gap * (float)(take - 1);
            line_cross[lines] = cross;
            ++lines;
            start += take;
        }
    } else {
        line_start[0] = 0;
        line_count[0] = count;
        line_main[0] = 0.0f;
        line_cross[0] = 0.0f;
        int j;
        for (j = 0; j < count; ++j) {
            line_main[0] += row ? item_w[j] : item_h[j];
            float value = row ? item_h[j] : item_w[j];
            if (value > line_cross[0]) {
                line_cross[0] = value;
            }
        }
        line_main[0] += gap * (float)(count - 1);
        lines = 1;
        float limit = row ? content_w : content_h;
        if (limit >= 0.0f && line_main[0] > limit + 0.01f && count > 0) {
            float base_sum = 0.0f;
            for (j = 0; j < count; ++j) {
                base_sum += row ? item_w[j] : item_h[j];
            }
            float scale = (limit - gap * (float)(count - 1)) / base_sum;
            if (scale < 1.0f && scale >= 0.80f) {
                for (j = 0; j < count; ++j) {
                    if (row) {
                        item_w[j] *= scale;
                    } else {
                        item_h[j] *= scale;
                    }
                }
                line_main[0] = limit;
            }
        }
    }

    /* 2b. Grow: distribute free main-axis space among flex-grow items and
     * re-layout them so their children reflow. */
    {
        int li;
        for (li = 0; li < lines; ++li) {
            int start = line_start[li];
            int take = line_count[li];
            float limit = row ? content_w : content_h;
            float free;
            float grow_sum = 0.0f;
            int j;
            if (limit < 0.0f || take <= 0) {
                continue;
            }
            free = limit - line_main[li];
            if (free <= 0.01f) {
                continue;
            }
            for (j = start; j < start + take; ++j) {
                grow_sum += WN_FLEX_GROW(&c->nodes[items[j]]);
            }
            if (grow_sum <= 0.0f) {
                continue;
            }
            for (j = start; j < start + take; ++j) {
                int id = items[j];
                const UiNode *n = &c->nodes[id];
                float g = WN_FLEX_GROW(n);
                float extra;
                if (g <= 0.0f) {
                    continue;
                }
                extra = free * g / grow_sum;
                if (row) {
                    item_w[j] += extra;
                    layout_node(c, id, 0.0f, 0.0f, content_w, content_h, 0, item_w[j]);
                    item_h[j] = c->rects[id].h;
                } else {
                    float outer = item_h[j] + extra;
                    float saved = c->nodes[id].height;
                    c->nodes[id].height = outer - WN_DECL_H(n);
                    layout_node(c, id, 0.0f, 0.0f, content_w, content_h, 0, -1.0f);
                    c->nodes[id].height = saved;
                    item_h[j] = outer;
                    item_w[j] = c->rects[id].w;
                }
            }
            line_main[li] = limit;
            for (j = start; j < start + take; ++j) {
                float value = row ? item_h[j] : item_w[j];
                if (value > line_cross[li]) {
                    line_cross[li] = value;
                }
            }
        }
    }

    /* Single line: the line cross size is the container's inner cross size
     * when that is definite. */
    if (lines == 1) {
        if (row && content_h >= 0.0f) {
            line_cross[0] = content_h;
        } else if (!row && content_w >= 0.0f) {
            line_cross[0] = content_w;
        }
    }

    /* 3. Place lines and translate the measured subtrees. */
    float cross_origin = 0.0f;
    float return_main = 0.0f;
    int l;
    for (l = 0; l < lines; ++l) {
        int start = line_start[l];
        int take = line_count[l];
        float limit = row ? content_w : (content_h >= 0.0f ? content_h : line_main[l]);
        float free = limit - line_main[l];
        if (free < 0.0f) {
            free = 0.0f;
        }
        /* E1: main-axis auto margins absorb the free space before
         * justify-content (CSS). `margin-top:auto` in a column pins the item
         * to the bottom; `margin-left/right:auto` centres/splits in a row. */
        float auto_unit = 0.0f;
        {
            int auto_sides = 0;
            int scan;
            for (scan = start; scan < start + take; ++scan) {
                const UiNode *an = &c->nodes[items[scan]];
                (void)an;
                if (row) {
                    auto_sides += WN_MARGIN_AUTO(an, 3) + WN_MARGIN_AUTO(an, 1);
                } else {
                    auto_sides += WN_MARGIN_AUTO(an, 0) + WN_MARGIN_AUTO(an, 2);
                }
            }
            if (auto_sides > 0) {
                auto_unit = free / (float)auto_sides;
                free = 0.0f;
            }
        }
        float start_off, spacing;
        justify_offsets(p->justify, free, take, gap, &start_off, &spacing);
        float cursor = start_off;
        float auto_used = 0.0f;
        int j;
        for (j = start; j < start + take; ++j) {
            int id = items[j];
            const UiNode *n = &c->nodes[id];
            float w = item_w[j];
            float h = item_h[j];
            float cross_extent = line_cross[l];
            if (row) {
                if (WN_MARGIN_AUTO(n, 3)) {
                    auto_used += auto_unit;
                }
                if (p->align == ALIGN_STRETCH && n->height < 0.0f && n->height_pct == 0.0f) {
                    h = cross_extent;
                }
                float off = align_offset(p->align, cross_extent, h);
                if (WN_MARGIN_AUTO(n, 0) && WN_MARGIN_AUTO(n, 2)) {
                    off = (cross_extent - h) * 0.5f;
                } else if (WN_MARGIN_AUTO(n, 0)) {
                    off = cross_extent - h;
                } else if (WN_MARGIN_AUTO(n, 2)) {
                    off = 0.0f;
                }
                c->rects[id].w = w;
                c->rects[id].h = h;
                translate_subtree(c, id, content_x + cursor + auto_used,
                                  content_y + cross_origin + off);
                cursor += w + spacing;
                if (WN_MARGIN_AUTO(n, 1)) {
                    auto_used += auto_unit;
                }
            } else {
                if (WN_MARGIN_AUTO(n, 0)) {
                    auto_used += auto_unit;
                }
                if (p->align == ALIGN_STRETCH && n->width < 0.0f && n->width_pct == 0.0f) {
                    w = cross_extent;
                }
                float off = align_offset(p->align, cross_extent, w);
                if (WN_MARGIN_AUTO(n, 3) && WN_MARGIN_AUTO(n, 1)) {
                    off = (cross_extent - w) * 0.5f;
                } else if (WN_MARGIN_AUTO(n, 3)) {
                    off = cross_extent - w;
                } else if (WN_MARGIN_AUTO(n, 1)) {
                    off = 0.0f;
                }
                c->rects[id].w = w;
                c->rects[id].h = h;
                translate_subtree(c, id, content_x + cross_origin + off,
                                  content_y + cursor + auto_used);
                cursor += h + spacing;
                if (WN_MARGIN_AUTO(n, 2)) {
                    auto_used += auto_unit;
                }
            }
        }
        cross_origin += line_cross[l] + gap;
        if (row) {
            return_main += line_cross[l];
            if (l + 1 < lines) {
                return_main += gap;
            }
        } else if (line_main[l] > return_main) {
            return_main = line_main[l];
        }
    }

    free(block);
    return return_main;
}

/* Grid with repeat(auto-fill, <px>): row-major items, explicit row height or
 * max child height, row/column gaps. No fr/minmax/span in v2. */
static float layout_grid_children(wn_core *c, int parent, float content_x,
                                  float content_y, float content_w,
                                  float content_h)
{
    const UiNode *p = &c->nodes[parent];
    int *items;
    float *item_w;
    float *item_h;
    float *row_h;
    int count = 0;
    int columns;
    int rows;
    int child;
    int k;

    for (child = c->first_child[parent]; child >= 0; child = c->next_sibling[child]) {
        if (!c->alive[child] || !c->visible[child]) {
            continue;
        }
        ++count;
    }
    if (count == 0) {
        return 0.0f;
    }
    if (p->grid_mode != R2_GRID_AUTO_FILL || p->grid_column_px <= 0.0f) {
        /* Not a v2 grid: behave like a block container. */
        float cursor = content_y;
        for (child = c->first_child[parent]; child >= 0; child = c->next_sibling[child]) {
            if (!c->alive[child] || !c->visible[child]) {
                continue;
            }
            cursor += layout_node(c, child, content_x, cursor, content_w, content_h, 0, -1.0f);
        }
        return cursor - content_y;
    }

    const float column_gap = p->column_gap > 0.0f ? p->column_gap : 0.0f;
    const float row_gap = p->row_gap > 0.0f ? p->row_gap : 0.0f;
    const float track = p->grid_column_px;
    columns = (int)floorf((content_w + column_gap) / (track + column_gap));
    if (columns < 1) {
        columns = 1;
    }
    rows = (count + columns - 1) / columns;

    items = (int *)malloc((size_t)count * sizeof(int));
    item_w = (float *)malloc((size_t)count * sizeof(float));
    item_h = (float *)malloc((size_t)count * sizeof(float));
    row_h = (float *)calloc((size_t)rows, sizeof(float));
    if (items == NULL || item_w == NULL || item_h == NULL || row_h == NULL) {
        free(items);
        free(item_w);
        free(item_h);
        free(row_h);
        return 0.0f;
    }

    k = 0;
    for (child = c->first_child[parent]; child >= 0; child = c->next_sibling[child]) {
        if (!c->alive[child] || !c->visible[child]) {
            continue;
        }
        items[k] = child;
        ++k;
    }

    /* Pass 1: measure each item at (0,0) with the track width forced. */
    for (k = 0; k < count; ++k) {
        const UiNode *n = &c->nodes[items[k]];
        float forced = track;
        if (n->width >= 0.0f) {
            forced = n->width + WN_DECL_W(n);
        } else if (n->width_pct > 0.0f) {
            forced = content_w * n->width_pct * 0.01f + WN_DECL_W(n);
        }
        forced = fmaxf(forced - n->margin[1] - n->margin[3], 0.0f);
        layout_node(c, items[k], 0.0f, 0.0f, content_w, content_h, 0, forced);
        item_w[k] = forced;
        item_h[k] = c->rects[items[k]].h;
        if (p->grid_auto_rows < 0.0f) {
            int row = k / columns;
            if (item_h[k] > row_h[row]) {
                row_h[row] = item_h[k];
            }
        }
    }
    if (p->grid_auto_rows >= 0.0f) {
        for (k = 0; k < rows; ++k) {
            row_h[k] = p->grid_auto_rows;
        }
    }

    /* Pass 2: place each item at its track origin. */
    {
        float y = content_y;
        float total = 0.0f;
        int row;
        for (row = 0; row < rows; ++row) {
            for (k = 0; k < count; ++k) {
                if (k / columns != row) {
                    continue;
                }
                {
                    int col = k % columns;
                    float x = content_x + (float)col * (track + column_gap);
                    layout_node(c, items[k], x, y, content_w, content_h, 0, item_w[k]);
                }
            }
            total += row_h[row];
            y += row_h[row] + row_gap;
        }
        if (rows > 1) {
            total += row_gap * (float)(rows - 1);
        }
        free(items);
        free(item_w);
        free(item_h);
        free(row_h);
        return total;
    }
}

static float layout_node(wn_core *c, int node, float origin_x, float origin_y,
                         float available_w, float parent_content_h,
                         int is_root, float forced_w)
{
    const UiNode *n = &c->nodes[node];
    float w;
    if (!c->alive[node] || !c->visible[node]) {
        mark_subtree_hidden(c, node);
        return 0.0f;
    }
    c->visible_eff[node] = 1;
    if (forced_w >= 0.0f) {
        w = forced_w;   /* already a border-box width (grid/flex added padding) */
    } else if (n->width >= 0.0f) {
        w = n->width + WN_DECL_W(n);
    } else if (n->width_pct > 0.0f) {
        w = available_w * n->width_pct * 0.01f + WN_DECL_W(n);
    } else if (n->kind == R2_NODE_IMAGE && n->image_width > 0.0f) {
        w = n->image_width;
    } else {
        w = fmaxf(available_w - n->margin[1] - n->margin[3], 0.0f);
    }

    float x = origin_x + n->margin[3];
    float y = origin_y + n->margin[0];
    if (is_root) {
        x = 0.0f;
        y = 0.0f;
    }

    const float pad_l = n->padding[3], pad_r = n->padding[1];
    const float pad_t = n->padding[0], pad_b = n->padding[2];
    const float content_w = fmaxf(w - pad_l - pad_r, 0.0f);
    float content_h = -1.0f;
    int definite_h = 0;
    if (n->height >= 0.0f) {
        content_h = fmaxf(n->height - (WN_BOX_SIZING(n) ? pad_t + pad_b : 0.0f), 0.0f);
        definite_h = 1;
    } else if (n->height_pct > 0.0f && parent_content_h >= 0.0f) {
        content_h = fmaxf(parent_content_h * n->height_pct * 0.01f -
                              (WN_BOX_SIZING(n) ? pad_t + pad_b : 0.0f), 0.0f);
        definite_h = 1;
    }

    const float content_x = x + pad_l;
    const float content_y = y + pad_t;

    if (n->kind == R2_NODE_TEXT) {
        float h = n->height >= 0.0f ? n->height + WN_DECL_H(n) : (content_h >= 0.0f ? content_h : 0.0f);
        c->rects[node].x = x;
        c->rects[node].y = y;
        c->rects[node].w = w;
        c->rects[node].h = h;
        return h + n->margin[2];
    }
    if (n->kind == R2_NODE_IMAGE) {
        float h;
        if (n->height >= 0.0f) {
            h = n->height + WN_DECL_H(n);
        } else if (n->height_pct > 0.0f && parent_content_h >= 0.0f) {
            h = parent_content_h * n->height_pct * 0.01f + WN_DECL_H(n);
        } else if (n->image_height > 0.0f && n->image_width > 0.0f) {
            h = w * (n->image_height / n->image_width);
        } else {
            h = content_h >= 0.0f ? content_h : 0.0f;
        }
        c->rects[node].x = x;
        c->rects[node].y = y;
        c->rects[node].w = w;
        c->rects[node].h = h;
        return h + n->margin[2];
    }
    if (n->kind == R2_NODE_INPUT) {
        /* Editor de una linea: hoja sin hijos. Sin CSS la altura es la linea
         * de la fuente (ascender + descender) mas el padding. */
        float h;
        if (n->height >= 0.0f) {
            h = n->height + WN_DECL_H(n);
        } else if (n->height_pct > 0.0f && parent_content_h >= 0.0f) {
            h = parent_content_h * n->height_pct * 0.01f + WN_DECL_H(n);
        } else {
            h = pad_t + pad_b + fmaxf(input_line_height(c, node), 1.0f);
        }
        c->rects[node].x = x;
        c->rects[node].y = y;
        c->rects[node].w = w;
        c->rects[node].h = h;
        c->scroll_max[node] = 0.0f;
        c->scroll[node] = 0.0f;
        return h + n->margin[2];
    }

    float cursor = content_y;
    if (n->kind == R2_NODE_FLEX) {
        cursor += layout_flex_children(c, node, content_x, content_y, content_w, content_h);
    } else if (n->kind == R2_NODE_GRID) {
        cursor += layout_grid_children(c, node, content_x, content_y, content_w, content_h);
    } else {
        int child;
        for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
            float used;
            if (!c->alive[child] || !c->visible[child]) {
                mark_subtree_hidden(c, child);
                continue;
            }
            used = layout_node(c, child, content_x, cursor, content_w, content_h, 0, -1.0f);
            /* E1: `margin-left/right:auto` in a block container. Both auto
             * margins centre the box; only left pushes it to the right. */
            {
                int auto_left = WN_MARGIN_AUTO(&c->nodes[child], 3);
                int auto_right = WN_MARGIN_AUTO(&c->nodes[child], 1);
                if (auto_left || auto_right) {
                    float dx = 0.0f;
                    float room = content_w - c->rects[child].w;
                    if (room > 0.0f) {
                        if (auto_left && auto_right) {
                            dx = room * 0.5f;
                        } else if (auto_left) {
                            dx = room;
                        }
                    }
                    if (dx > 0.0f) {
                        translate_subtree(c, child, dx, 0.0f);
                    }
                }
            }
            /* The child was placed at cursor + its top margin, so the flow must
             * advance by both; skipping margin[0] made every following sibling
             * sit one margin too high. */
            cursor += c->nodes[child].margin[0] + used;
        }
    }
    const float content_used = cursor - content_y;

    float h;
    if (n->height >= 0.0f) {
        h = n->height + WN_DECL_H(n);
    } else if (n->height_pct > 0.0f && parent_content_h >= 0.0f) {
        h = parent_content_h * n->height_pct * 0.01f + WN_DECL_H(n);
    } else {
        h = content_used + pad_t + pad_b;
    }
    c->rects[node].x = x;
    c->rects[node].y = y;
    c->rects[node].w = w;
    c->rects[node].h = h;

    /* Vertical scroll: fixed-height containers clip and translate their
     * children by -scroll_y. The offset is clamped to the real overflow. */
    if (n->overflow == R2_OVERFLOW_SCROLL && definite_h) {
        const float view_h = fmaxf(h - pad_t - pad_b, 0.0f);
        const float max_scroll = fmaxf(content_used - view_h, 0.0f);
        c->scroll_max[node] = max_scroll;
        if (c->scroll[node] < 0.0f) {
            c->scroll[node] = 0.0f;
        }
        if (c->scroll[node] > max_scroll) {
            c->scroll[node] = max_scroll;
        }
        if (c->scroll[node] > 0.0f) {
            int child;
            for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
                translate_subtree(c, child, 0.0f, -c->scroll[node]);
            }
        }
    } else {
        c->scroll_max[node] = 0.0f;
        if (c->scroll[node] != 0.0f) {
            c->scroll[node] = 0.0f;
        }
    }
    return h + n->margin[2];
}

/* --------------------------------------------------------------- clipping */

static wn_rect rect_intersection(wn_rect a, wn_rect b)
{
    wn_rect out;
    float x0 = a.x > b.x ? a.x : b.x;
    float y0 = a.y > b.y ? a.y : b.y;
    float x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    float y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    out.x = x0;
    out.y = y0;
    out.w = x1 > x0 ? x1 - x0 : 0.0f;
    out.h = y1 > y0 ? y1 - y0 : 0.0f;
    return out;
}

static int rect_is_empty(wn_rect r)
{
    return r.w <= 0.0f || r.h <= 0.0f;
}

/* clips[node] is the scissor applied when painting the node itself; children
 * of an overflow container are additionally clipped to its content box. */
static void compute_clips(wn_core *c, int node, wn_rect clip)
{
    const UiNode *n = &c->nodes[node];
    wn_rect child_clip = clip;
    int child;
    c->clips[node] = clip;
    if (n->overflow != R2_OVERFLOW_VISIBLE) {
        const wn_rect *r = &c->rects[node];
        wn_rect content;
        content.x = r->x + n->padding[3];
        content.y = r->y + n->padding[0];
        content.w = fmaxf(r->w - n->padding[1] - n->padding[3], 0.0f);
        content.h = fmaxf(r->h - n->padding[0] - n->padding[2], 0.0f);
        child_clip = rect_intersection(clip, content);
    }
    for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
        if (!c->alive[child] || !c->visible_eff[child]) {
            continue;
        }
        compute_clips(c, child, child_clip);
    }
}

static void build_paint_order(wn_core *c, int node)
{
    int child;
    if (!c->alive[node] || !c->visible_eff[node]) {
        return;
    }
    if (c->paint_count < c->node_capacity) {
        c->paint_order[c->paint_count++] = node;
    }
    for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
        build_paint_order(c, child);
    }
}

static void layout_page(wn_core *c, float viewport_w, float viewport_h)
{
    int root = -1;
    int i;
    wn_rect viewport;
    for (i = 0; i < c->node_count; ++i) {
        c->visible_eff[i] = 0;
        if (c->alive[i]) {
            c->rects[i].x = 0.0f;
            c->rects[i].y = 0.0f;
            c->rects[i].w = 0.0f;
            c->rects[i].h = 0.0f;
            c->clips[i] = c->rects[i];
        }
    }
    for (i = 0; i < c->node_count; ++i) {
        if (c->alive[i] && c->parent[i] < 0) {
            root = i;
            break;
        }
    }
    c->paint_count = 0;
    if (root >= 0) {
        layout_node(c, root, 0.0f, 0.0f, viewport_w, viewport_h, 1, -1.0f);
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.w = viewport_w;
        viewport.h = viewport_h;
        if (c->visible_eff[root]) {
            compute_clips(c, root, viewport);
        }
        build_paint_order(c, root);
    }
}

void wn_core_resize(wn_core *core, int width, int height)
{
    double t0;
    if (core == NULL) {
        return;
    }
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    if (core->width == width && core->height == height) {
        return;
    }
    core->width = width;
    core->height = height;
    t0 = wn_now_ms();
    layout_page(core, (float)width, (float)height);
    core->last_layout_ms = wn_now_ms() - t0;
    core->needs_layout = 0;
}

void wn_core_relayout(wn_core *core)
{
    double t0;
    if (core == NULL) {
        return;
    }
    t0 = wn_now_ms();
    layout_page(core, (float)core->width, (float)core->height);
    core->last_layout_ms = wn_now_ms() - t0;
    core->needs_layout = 0;
}

/* ------------------------------------------------------------- interaction */

static int point_in_rect(float px, float py, const wn_rect *r)
{
    return r->w > 0.0f && r->h > 0.0f &&
           px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

/* Public pointer/hit entry points can run between a mutation and the next
 * frame. Bring geometry up to date first so hidden/removed nodes never win a
 * hit even if the host has not rendered yet. */
static void ensure_layout_for_hit(wn_core *core)
{
    if (core != NULL && core->needs_layout) {
        layout_page(core, (float)core->width, (float)core->height);
        core->needs_layout = 0;
    }
}

int wn_core_node_at(const wn_core *core, float px, float py)
{
    int i;
    if (core == NULL) {
        return -1;
    }
    ensure_layout_for_hit((wn_core *)core);
    for (i = core->paint_count - 1; i >= 0; --i) {
        int node = core->paint_order[i];
        if (!core->visible_eff[node]) {
            continue;
        }
        if (point_in_rect(px, py, &core->rects[node]) &&
            point_in_rect(px, py, &core->clips[node])) {
            return node;
        }
    }
    return -1;
}

int wn_core_hit_test(const wn_core *core, float px, float py)
{
    int i;
    if (core == NULL) {
        return -1;
    }
    ensure_layout_for_hit((wn_core *)core);
    for (i = core->paint_count - 1; i >= 0; --i) {
        int node = core->paint_order[i];
        if (!core->visible_eff[node]) {
            continue;
        }
        if ((core->nodes[node].action != ACTION_NONE ||
             core->nodes[node].kind == R2_NODE_INPUT) &&
            point_in_rect(px, py, &core->rects[node]) &&
            point_in_rect(px, py, &core->clips[node])) {
            return node;
        }
    }
    return -1;
}

int wn_core_wheel(wn_core *core, float x, float y, float delta_x, float delta_y)
{
    int node;
    int container = -1;
    float next;
    (void)delta_x; /* v2 scrolls vertically only */
    if (core == NULL) {
        return -1;
    }
    node = wn_core_node_at(core, x, y);
    while (node >= 0) {
        if (core->nodes[node].overflow == R2_OVERFLOW_SCROLL &&
            core->scroll_max[node] > 0.0f) {
            container = node;
            break;
        }
        node = core->nodes[node].parent;
    }
    if (container < 0) {
        return -1;
    }
    next = core->scroll[container] + delta_y;
    if (next < 0.0f) {
        next = 0.0f;
    }
    if (next > core->scroll_max[container]) {
        next = core->scroll_max[container];
    }
    if (next == core->scroll[container]) {
        return -1;
    }
    core->scroll[container] = next;
    {
        double t0 = wn_now_ms();
        layout_page(core, (float)core->width, (float)core->height);
        core->last_layout_ms = wn_now_ms() - t0;
    }
    return container;
}

float wn_core_scroll_offset(const wn_core *core, int node)
{
    if (!wn_core_node_is_alive(core, node)) {
        return -1.0f;
    }
    return core->scroll[node];
}

float wn_core_scroll_max(const wn_core *core, int node)
{
    if (!wn_core_node_is_alive(core, node)) {
        return -1.0f;
    }
    return core->scroll_max[node];
}

int wn_core_node_clip(const wn_core *core, int node, wn_rect *out)
{
    if (core == NULL || out == NULL || !wn_core_node_is_alive(core, node)) {
        return -1;
    }
    *out = core->clips[node];
    return 0;
}

int wn_core_pointer_move(wn_core *core, float px, float py)
{
    int hit;
    if (core == NULL) {
        return -1;
    }
    hit = wn_core_hit_test(core, px, py);
    core->hover = hit;
    return hit;
}

int wn_core_pointer_down(wn_core *core, float px, float py)
{
    int hit;
    if (core == NULL) {
        return -1;
    }
    hit = wn_core_hit_test(core, px, py);
    core->hover = hit;
    core->press = hit;
    if (hit >= 0 && input_record(core, hit) != NULL) {
        wn_core_focus_input(core, hit, px);
    } else {
        wn_core_blur_input(core);
    }
    return hit;
}

int wn_core_pointer_up(wn_core *core, float px, float py)
{
    int hit;
    if (core == NULL) {
        return -1;
    }
    hit = wn_core_hit_test(core, px, py);
    core->hover = hit;
    if (core->press >= 0 && hit == core->press &&
        core->nodes[core->press].action == ACTION_SELECT) {
        core->selected = core->press;
    }
    core->press = -1;
    return hit;
}

void wn_core_pointer_leave(wn_core *core)
{
    if (core != NULL) {
        core->hover = -1;
    }
}

/* ---------------------------------------------------------- text input v4 */

static const WnInputRecord *input_record(const wn_core *c, int node)
{
    int index;
    if (c == NULL || node < 0 || node >= c->node_count || c->input_map == NULL ||
        c->input_records == NULL) {
        return NULL;
    }
    index = c->input_map[node];
    if (index < 0 || index >= c->input_record_count) {
        return NULL;
    }
    return &c->input_records[index];
}

static const char *input_text(const wn_core *c, int node)
{
    if (c->input_value == NULL || node < 0 || node >= c->node_count ||
        c->input_value[node] == NULL) {
        return "";
    }
    return c->input_value[node];
}

static int input_length(const wn_core *c, int node)
{
    return (int)strlen(input_text(c, node));
}

static int input_limit(const WnInputRecord *record)
{
    if (record != NULL && record->max_length > 0) {
        return record->max_length;
    }
    return WN_INPUT_MAX_BYTES;
}

static const WnInputGlyph *input_glyph(const WnInputRecord *record, unsigned char ch)
{
    int index;
    if (record == NULL || ch >= 128) {
        return NULL;
    }
    index = record->cp_index[ch];
    if (index < 0 || index >= record->glyph_count) {
        return NULL;
    }
    return &record->glyphs[index];
}

static float input_prefix_width(const WnInputRecord *record, const char *text, int upto)
{
    float pen = 0.0f;
    int i;
    if (record == NULL || text == NULL) {
        return 0.0f;
    }
    for (i = 0; i < upto && text[i] != '\0'; ++i) {
        const WnInputGlyph *glyph = input_glyph(record, (unsigned char)text[i]);
        if (glyph != NULL) {
            pen += glyph->advance;
        }
    }
    return pen;
}

static float input_line_height(const wn_core *c, int node)
{
    const WnInputRecord *record = input_record(c, node);
    if (record != NULL && record->line_height > 0.0f) {
        return record->line_height;
    }
    return c->nodes[node].font_size * 1.35f;
}

static wn_rect input_content_rect(const wn_core *c, int node)
{
    const UiNode *n = &c->nodes[node];
    const wn_rect *r = &c->rects[node];
    wn_rect box;
    box.x = r->x + n->padding[3];
    box.y = r->y + n->padding[0];
    box.w = fmaxf(r->w - n->padding[1] - n->padding[3], 0.0f);
    box.h = fmaxf(r->h - n->padding[0] - n->padding[2], 0.0f);
    return box;
}

static int input_has_selection(const wn_core *c, int node)
{
    return c->input_anchor[node] >= 0 &&
           c->input_anchor[node] != c->input_cursor[node];
}

static void input_selection_bounds(const wn_core *c, int node, int *start, int *end)
{
    int anchor = c->input_anchor[node];
    int cursor = c->input_cursor[node];
    if (anchor < 0 || anchor == cursor) {
        *start = cursor;
        *end = cursor;
        return;
    }
    *start = anchor < cursor ? anchor : cursor;
    *end = anchor < cursor ? cursor : anchor;
}

static void input_delete_range(wn_core *c, int node, int start, int end)
{
    char *value = c->input_value != NULL ? c->input_value[node] : NULL;
    int len;
    if (value == NULL) {
        return;
    }
    len = (int)strlen(value);
    if (start < 0) {
        start = 0;
    }
    if (end > len) {
        end = len;
    }
    if (start >= end) {
        return;
    }
    memmove(value + start, value + end, (size_t)(len - end) + 1u);
    c->input_cursor[node] = start;
    c->input_anchor[node] = -1;
}

static void input_move_cursor(wn_core *c, int node, int position, int extend)
{
    int len = input_length(c, node);
    if (position < 0) {
        position = 0;
    }
    if (position > len) {
        position = len;
    }
    if (extend) {
        if (c->input_anchor[node] < 0) {
            c->input_anchor[node] = c->input_cursor[node];
        }
    } else {
        c->input_anchor[node] = -1;
    }
    c->input_cursor[node] = position;
}

static void input_update_scroll(wn_core *c, int node)
{
    const WnInputRecord *record = input_record(c, node);
    const char *value;
    wn_rect content;
    float caret;
    float scroll;
    float text_width;
    float max_scroll;
    if (record == NULL || c->input_scroll == NULL) {
        return;
    }
    content = input_content_rect(c, node);
    if (content.w <= 0.0f) {
        c->input_scroll[node] = 0.0f;
        return;
    }
    value = input_text(c, node);
    caret = input_prefix_width(record, value, c->input_cursor[node]);
    scroll = c->input_scroll[node];
    if (caret - scroll > content.w - WN_INPUT_CARET_WIDTH) {
        scroll = caret - content.w + WN_INPUT_CARET_WIDTH;
    }
    if (caret - scroll < 0.0f) {
        scroll = caret;
    }
    text_width = input_prefix_width(record, value, (int)strlen(value));
    max_scroll = text_width + WN_INPUT_CARET_WIDTH - content.w;
    if (max_scroll < 0.0f) {
        max_scroll = 0.0f;
    }
    if (scroll > max_scroll) {
        scroll = max_scroll;
    }
    if (scroll < 0.0f) {
        scroll = 0.0f;
    }
    c->input_scroll[node] = scroll;
}

int wn_core_node_is_input(const wn_core *core, int node)
{
    return input_record(core, node) != NULL;
}

int wn_core_input_focused(const wn_core *core)
{
    return core != NULL ? core->input_focus : -1;
}

int wn_core_focus_input(wn_core *core, int node, float pointer_x)
{
    const WnInputRecord *record;
    const char *value;
    wn_rect content;
    float pen;
    int len;
    int caret;
    int i;
    if (core == NULL) {
        return -1;
    }
    record = input_record(core, node);
    if (record == NULL) {
        return -1;
    }
    ensure_layout_for_hit(core);
    if (core->input_focus >= 0 && core->input_focus != node) {
        core->input_anchor[core->input_focus] = -1;
    }
    core->input_focus = node;
    core->input_anchor[node] = -1;
    input_update_scroll(core, node);
    content = input_content_rect(core, node);
    value = input_text(core, node);
    len = (int)strlen(value);
    pen = content.x - core->input_scroll[node];
    caret = len;
    for (i = 0; i < len; ++i) {
        const WnInputGlyph *glyph = input_glyph(record, (unsigned char)value[i]);
        float advance = glyph != NULL ? glyph->advance : 0.0f;
        if (pointer_x < pen + advance * 0.5f) {
            caret = i;
            break;
        }
        pen += advance;
    }
    core->input_cursor[node] = caret;
    core->dirty = 1;
    return node;
}

int wn_core_blur_input(wn_core *core)
{
    int node;
    if (core == NULL) {
        return -1;
    }
    node = core->input_focus;
    if (node < 0) {
        return -1;
    }
    core->input_anchor[node] = -1;
    core->input_focus = -1;
    core->dirty = 1;
    return 0;
}

int wn_core_input_char(wn_core *core, uint32_t codepoint,
                       int *value_changed, int *visual_changed)
{
    const WnInputRecord *record;
    char *value;
    int node;
    int len;
    int limit;
    int cursor;
    if (value_changed != NULL) {
        *value_changed = 0;
    }
    if (visual_changed != NULL) {
        *visual_changed = 0;
    }
    if (core == NULL || codepoint < 32u || codepoint > 126u) {
        return -1;
    }
    node = core->input_focus;
    record = input_record(core, node);
    if (record == NULL || core->input_value == NULL) {
        return -1;
    }
    value = core->input_value[node];
    if (value == NULL) {
        return -1;
    }
    if (input_has_selection(core, node)) {
        int start;
        int end;
        input_selection_bounds(core, node, &start, &end);
        input_delete_range(core, node, start, end);
    }
    len = (int)strlen(value);
    limit = input_limit(record);
    cursor = core->input_cursor[node];
    if (cursor < 0) {
        cursor = 0;
    }
    if (cursor > len) {
        cursor = len;
    }
    if (len >= limit) {
        /* Consumido pero sin cambio: el host no debe hacer beep al teclear
         * sobre un campo lleno. */
        return node;
    }
    memmove(value + cursor + 1, value + cursor, (size_t)(len - cursor) + 1u);
    value[cursor] = (char)codepoint;
    core->input_cursor[node] = cursor + 1;
    core->input_anchor[node] = -1;
    core->dirty = 1;
    if (value_changed != NULL) {
        *value_changed = 1;
    }
    if (visual_changed != NULL) {
        *visual_changed = 1;
    }
    return node;
}

int wn_core_input_key(wn_core *core, int key, uint32_t modifiers,
                      int *value_changed, int *visual_changed)
{
    const WnInputRecord *record;
    char *value;
    int node;
    int len;
    int cursor;
    int extend = (modifiers & WN_KEY_MOD_SHIFT) != 0u;
    if (value_changed != NULL) {
        *value_changed = 0;
    }
    if (visual_changed != NULL) {
        *visual_changed = 0;
    }
    if (core == NULL) {
        return -1;
    }
    node = core->input_focus;
    record = input_record(core, node);
    if (record == NULL || core->input_value == NULL) {
        return -1;
    }
    value = core->input_value[node];
    if (value == NULL) {
        return -1;
    }
    len = (int)strlen(value);
    cursor = core->input_cursor[node];
    switch (key) {
    case WN_KEY_ESCAPE:
        wn_core_blur_input(core);
        if (visual_changed != NULL) {
            *visual_changed = 1;
        }
        return node;
    case WN_KEY_LEFT:
        if (!extend && input_has_selection(core, node)) {
            int start;
            int end;
            input_selection_bounds(core, node, &start, &end);
            input_move_cursor(core, node, start, 0);
            (void)end;
        } else {
            input_move_cursor(core, node, cursor - 1, extend);
        }
        core->dirty = 1;
        if (visual_changed != NULL) {
            *visual_changed = 1;
        }
        return node;
    case WN_KEY_RIGHT:
        if (!extend && input_has_selection(core, node)) {
            int start;
            int end;
            input_selection_bounds(core, node, &start, &end);
            input_move_cursor(core, node, end, 0);
            (void)start;
        } else {
            input_move_cursor(core, node, cursor + 1, extend);
        }
        core->dirty = 1;
        if (visual_changed != NULL) {
            *visual_changed = 1;
        }
        return node;
    case WN_KEY_HOME:
        input_move_cursor(core, node, 0, extend);
        core->dirty = 1;
        if (visual_changed != NULL) {
            *visual_changed = 1;
        }
        return node;
    case WN_KEY_END:
        input_move_cursor(core, node, len, extend);
        core->dirty = 1;
        if (visual_changed != NULL) {
            *visual_changed = 1;
        }
        return node;
    case WN_KEY_BACKSPACE:
        if (input_has_selection(core, node)) {
            int start;
            int end;
            input_selection_bounds(core, node, &start, &end);
            input_delete_range(core, node, start, end);
        } else if (cursor > 0) {
            input_delete_range(core, node, cursor - 1, cursor);
        } else {
            return node;
        }
        core->dirty = 1;
        if (value_changed != NULL) {
            *value_changed = 1;
        }
        if (visual_changed != NULL) {
            *visual_changed = 1;
        }
        return node;
    case WN_KEY_DELETE:
        if (input_has_selection(core, node)) {
            int start;
            int end;
            input_selection_bounds(core, node, &start, &end);
            input_delete_range(core, node, start, end);
        } else if (cursor < len) {
            input_delete_range(core, node, cursor, cursor + 1);
        } else {
            return node;
        }
        core->dirty = 1;
        if (value_changed != NULL) {
            *value_changed = 1;
        }
        if (visual_changed != NULL) {
            *visual_changed = 1;
        }
        return node;
    default:
        return -1;
    }
}

int wn_core_input_select_all(wn_core *core, int *visual_changed)
{
    int node;
    if (visual_changed != NULL) {
        *visual_changed = 0;
    }
    if (core == NULL) {
        return -1;
    }
    node = core->input_focus;
    if (input_record(core, node) == NULL) {
        return -1;
    }
    core->input_anchor[node] = 0;
    core->input_cursor[node] = input_length(core, node);
    core->dirty = 1;
    if (visual_changed != NULL) {
        *visual_changed = 1;
    }
    return node;
}

int wn_core_input_copy(wn_core *core, char *out, int capacity)
{
    int node;
    int start;
    int end;
    int count;
    const char *value;
    if (core == NULL || out == NULL || capacity <= 0) {
        return -1;
    }
    node = core->input_focus;
    if (input_record(core, node) == NULL) {
        return -1;
    }
    out[0] = '\0';
    input_selection_bounds(core, node, &start, &end);
    if (start >= end) {
        return 0;
    }
    value = input_text(core, node);
    count = end - start;
    if (count >= capacity) {
        count = capacity - 1;
    }
    memcpy(out, value + start, (size_t)count);
    out[count] = '\0';
    return count;
}

int wn_core_input_cut(wn_core *core, char *out, int capacity,
                      int *value_changed, int *visual_changed)
{
    int node;
    int copied;
    int start;
    int end;
    if (value_changed != NULL) {
        *value_changed = 0;
    }
    if (visual_changed != NULL) {
        *visual_changed = 0;
    }
    copied = wn_core_input_copy(core, out, capacity);
    if (copied <= 0) {
        return copied;
    }
    node = core->input_focus;
    input_selection_bounds(core, node, &start, &end);
    input_delete_range(core, node, start, end);
    core->dirty = 1;
    if (value_changed != NULL) {
        *value_changed = 1;
    }
    if (visual_changed != NULL) {
        *visual_changed = 1;
    }
    return copied;
}

int wn_core_input_paste(wn_core *core, const char *text,
                        int *value_changed, int *visual_changed)
{
    const WnInputRecord *record;
    char *value;
    int node;
    int len;
    int limit;
    int cursor;
    int inserted = 0;
    int deleted = 0;
    int i;
    if (value_changed != NULL) {
        *value_changed = 0;
    }
    if (visual_changed != NULL) {
        *visual_changed = 0;
    }
    if (core == NULL || text == NULL) {
        return -1;
    }
    node = core->input_focus;
    record = input_record(core, node);
    if (record == NULL || core->input_value == NULL || core->input_value[node] == NULL) {
        return -1;
    }
    value = core->input_value[node];
    if (input_has_selection(core, node)) {
        int start;
        int end;
        input_selection_bounds(core, node, &start, &end);
        input_delete_range(core, node, start, end);
        deleted = 1;
    }
    len = (int)strlen(value);
    limit = input_limit(record);
    cursor = core->input_cursor[node];
    if (cursor < 0) {
        cursor = 0;
    }
    if (cursor > len) {
        cursor = len;
    }
    for (i = 0; text[i] != '\0' && len < limit; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 32 || ch > 126) {
            continue;
        }
        memmove(value + cursor + 1, value + cursor, (size_t)(len - cursor) + 1u);
        value[cursor] = (char)ch;
        ++cursor;
        ++len;
        ++inserted;
    }
    if (inserted <= 0 && !deleted) {
        return node;
    }
    core->input_cursor[node] = cursor;
    core->input_anchor[node] = -1;
    core->dirty = 1;
    if (value_changed != NULL) {
        *value_changed = 1;
    }
    if (visual_changed != NULL) {
        *visual_changed = 1;
    }
    return node;
}

int wn_core_set_input_value(wn_core *core, int node, const char *text)
{
    const WnInputRecord *record;
    const unsigned char *p;
    char *copy;
    int len;
    int limit;
    if (core == NULL || text == NULL) {
        return -1;
    }
    record = input_record(core, node);
    if (record == NULL || core->input_value == NULL) {
        return -1;
    }
    for (p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (*p < 32u || *p > 126u) {
            return 1;
        }
    }
    len = (int)strlen(text);
    limit = input_limit(record);
    if (len > limit) {
        return 2;
    }
    copy = input_alloc_value(text);
    if (copy == NULL) {
        return -1;
    }
    wn_free_(core->input_value[node]);
    core->input_value[node] = copy;
    core->input_cursor[node] = len;
    core->input_anchor[node] = -1;
    core->dirty = 1;
    return 0;
}

const char *wn_core_input_value(const wn_core *core, int node)
{
    if (input_record(core, node) == NULL) {
        return NULL;
    }
    return input_text(core, node);
}

int wn_core_input_state(const wn_core *core, int node, wn_input_state *out)
{
    const WnInputRecord *record;
    const char *value;
    wn_rect content;
    float scroll;
    float caret;
    int start;
    int end;
    if (core == NULL || out == NULL) {
        return -1;
    }
    record = input_record(core, node);
    if (record == NULL) {
        return -1;
    }
    ensure_layout_for_hit((wn_core *)core);
    input_update_scroll((wn_core *)core, node);
    value = input_text(core, node);
    content = input_content_rect(core, node);
    scroll = core->input_scroll[node];
    caret = content.x + input_prefix_width(record, value, core->input_cursor[node]) - scroll;
    memset(out, 0, sizeof(*out));
    out->value = value;
    out->cursor = core->input_cursor[node];
    out->anchor = core->input_anchor[node];
    out->focused = core->input_focus == node;
    out->max_length = record->max_length;
    out->scroll = scroll;
    out->baseline = record->baseline;
    out->line_height = record->line_height;
    out->caret_x = caret;
    out->rect = core->rects[node];
    out->content = content;
    input_selection_bounds(core, node, &start, &end);
    if (start < end) {
        float x0 = content.x + input_prefix_width(record, value, start) - scroll;
        float x1 = content.x + input_prefix_width(record, value, end) - scroll;
        out->selection_x0 = x0;
        out->selection_x1 = x1;
    } else {
        out->selection_x0 = caret;
        out->selection_x1 = caret;
    }
    return 0;
}

int wn_core_hover(const wn_core *core)
{
    return core != NULL ? core->hover : -1;
}

int wn_core_press(const wn_core *core)
{
    return core != NULL ? core->press : -1;
}

int wn_core_selected(const wn_core *core)
{
    return core != NULL ? core->selected : -1;
}

static unsigned int node_color(const wn_core *c, int node)
{
    const UiNode *n = &c->nodes[node];
    if (n->action == ACTION_NONE) {
        return n->background;
    }
    if (c->press == node && n->press_bg != 0) {
        return n->press_bg;
    }
    if (c->hover == node && n->hover_bg != 0) {
        return n->hover_bg;
    }
    if (c->selected == node && n->selected_bg != 0) {
        return n->selected_bg;
    }
    return n->background;
}

void wn_core_set_state(wn_core *core, int node, int state, int enabled)
{
    int value;
    if (core == NULL || node < 0 || node >= core->node_count) {
        return;
    }
    if (!enabled) {
        value = -1;
    } else {
        value = node;
    }
    if (state == WN_STATE_HOVER) {
        core->hover = value;
    } else if (state == WN_STATE_PRESS) {
        core->press = value;
    } else if (state == WN_STATE_SELECTED) {
        core->selected = value;
    }
    core->dirty = 1;
}

/* ---------------------------------------------------------------- classes */

int wn_core_set_class(wn_core *core, int node, const char *class_name, int enabled)
{
    int i;
    int slot = -1;
    int count;
    if (core == NULL || class_name == NULL || class_name[0] == '\0' ||
        node < 0 || node >= core->node_count || !core->alive[node]) {
        return -1;
    }
    if (strcmp(class_name, "hover") == 0) {
        if (enabled) {
            core->hover = node;
        } else if (core->hover == node) {
            core->hover = -1;
        }
        core->dirty = 1;
        return 0;
    }
    if (strcmp(class_name, "press") == 0) {
        if (enabled) {
            core->press = node;
        } else if (core->press == node) {
            core->press = -1;
        }
        core->dirty = 1;
        return 0;
    }
    if (strcmp(class_name, "selected") == 0) {
        if (enabled) {
            core->selected = node;
        } else if (core->selected == node) {
            core->selected = -1;
        }
        core->dirty = 1;
        return 0;
    }

    count = core->class_count[node];
    for (i = 0; i < count; ++i) {
        if (strcmp(core->classes[node * WN_MAX_CLASSES + i], class_name) == 0) {
            slot = i;
            break;
        }
    }
    if (enabled) {
        if (slot >= 0) {
            return 0;
        }
        if (count >= WN_MAX_CLASSES) {
            return 1;
        }
        core->classes[node * WN_MAX_CLASSES + count] = wn_strdup_(class_name);
        if (core->classes[node * WN_MAX_CLASSES + count] == NULL) {
            return 1;
        }
        core->class_count[node] = (unsigned char)(count + 1);
    } else if (slot >= 0) {
        wn_free_(core->classes[node * WN_MAX_CLASSES + slot]);
        for (i = slot; i + 1 < count; ++i) {
            core->classes[node * WN_MAX_CLASSES + i] =
                core->classes[node * WN_MAX_CLASSES + i + 1];
        }
        core->classes[node * WN_MAX_CLASSES + count - 1] = NULL;
        core->class_count[node] = (unsigned char)(count - 1);
    }
    core->dirty = 1;
    return 0;
}

/* ------------------------------------------------------------- glyph cache */

static GlyphSet *glyph_set_for(wn_core *c, int font_px)
{
    int i;
    GlyphSet *set;
    for (i = 0; i < c->glyph_set_count; ++i) {
        if (c->glyph_sets[i].font_px == font_px) {
            return &c->glyph_sets[i];
        }
    }
    if (c->glyph_set_count >= WN_MAX_GLYPH_SETS) {
        return NULL;
    }
    set = &c->glyph_sets[c->glyph_set_count++];
    memset(set, 0, sizeof(*set));
    set->font_px = font_px;
    set->space_advance = (float)font_px * 0.30f;
    return set;
}

static void build_glyph_sets(wn_core *c)
{
    int r;
    for (r = 0; r < c->doc->run_count; ++r) {
        const UiTextRun *run = &c->doc->runs[r];
        const UiNode *node;
        const char *text;
        GlyphSet *set;
        RunChar chars[256];
        int char_count = 0;
        int glyph_index;
        int i;
        int text_len;
        if (run->node < 0 || run->node >= c->node_count || run->count <= 0 ||
            !c->alive[run->node]) {
            continue;
        }
        node = &c->nodes[run->node];
        text = node->text;
        if (text == NULL) {
            continue;
        }
        text_len = (int)strlen(text);
        if (text_len <= 0 || text_len > 256) {
            continue;
        }
        set = glyph_set_for(c, (int)(node->font_size + 0.5f));
        if (set == NULL) {
            continue;
        }
        glyph_index = run->first;
        for (i = 0; i < text_len; ++i) {
            unsigned char ch = (unsigned char)text[i];
            if (ch >= 128) {
                char_count = 0;
                break;
            }
            if (ch == ' ') {
                chars[char_count].ch = ' ';
                chars[char_count].glyph = -1;
                ++char_count;
            } else if (glyph_index < run->first + run->count &&
                       glyph_index < c->doc->glyph_count) {
                chars[char_count].ch = (char)ch;
                chars[char_count].glyph = glyph_index;
                ++glyph_index;
                ++char_count;
            }
        }
        if (char_count == 0) {
            continue;
        }

        for (i = 0; i < char_count; ++i) {
            const UiGlyph *g;
            GlyphEntry *entry;
            int next_nonspace = i + 1;
            float advance;
            if (chars[i].glyph < 0) {
                continue;
            }
            while (next_nonspace < char_count && chars[next_nonspace].glyph < 0) {
                ++next_nonspace;
            }
            g = &c->doc->glyphs[chars[i].glyph];
            entry = &set->ascii[(unsigned char)chars[i].ch];
            if (next_nonspace < char_count) {
                const UiGlyph *next = &c->doc->glyphs[chars[next_nonspace].glyph];
                if (next_nonspace == i + 1) {
                    advance = next->x - g->x;
                } else {
                    advance = g->w + 1.0f;
                    if (next->x > g->x + g->w) {
                        float space = next->x - (g->x + g->w);
                        if (space > set->space_advance) {
                            set->space_advance = space;
                        }
                    }
                }
            } else {
                advance = (float)node->width - g->x;
                if (advance <= 0.0f || advance > (float)set->font_px * 2.5f) {
                    advance = g->w + 1.0f;
                }
            }
            if (!entry->valid) {
                entry->valid = 1;
                entry->off_x = (i == 0) ? g->x : 0.0f;
                entry->off_y = g->y;
                entry->w = g->w;
                entry->h = g->h;
                entry->u0 = g->u0;
                entry->v0 = g->v0;
                entry->u1 = g->u1;
                entry->v1 = g->v1;
                entry->advance = advance;
            }
        }
    }
}

static int text_supported(const wn_core *c, int node, const char *text)
{
    int i;
    const GlyphSet *set = NULL;
    if (text == NULL) {
        return 0;
    }
    for (i = 0; c->glyph_sets != NULL && i < c->glyph_set_count; ++i) {
        if (c->glyph_sets[i].font_px == (int)(c->nodes[node].font_size + 0.5f)) {
            set = &c->glyph_sets[i];
            break;
        }
    }
    if (set == NULL) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (*p >= 128) {
            return 0;
        }
        if (*p == ' ') {
            continue;
        }
        if (!set->ascii[*p].valid) {
            return 0;
        }
    }
    return 1;
}

int wn_core_text_override_supported(const wn_core *core, int node, const char *text)
{
    if (core == NULL || !wn_core_node_is_alive(core, node) ||
        core->nodes[node].kind != R2_NODE_TEXT) {
        return -1;
    }
    if (text == NULL) {
        return -1;
    }
    return text_supported(core, node, text) ? 0 : 1;
}

int wn_core_set_text(wn_core *core, int node, const char *text)
{
    char *copy;
    if (core == NULL || !wn_core_node_is_alive(core, node) ||
        core->nodes[node].kind != R2_NODE_TEXT || text == NULL) {
        return -1;
    }
    if (!text_supported(core, node, text)) {
        return 1;
    }
    copy = wn_strdup_(text);
    if (copy == NULL) {
        return -1;
    }
    wn_free_(core->text_override[node]);
    core->text_override[node] = copy;
    core->dirty = 1;
    core->needs_layout = 1;
    return 0;
}

/* ---------------------------------------------------------------- geometry */

static void push_vertex(wn_core *c, float x, float y, float vw, float vh,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    wn_vertex *v;
    ensure_vertex_capacity(c, c->vertex_count + 1);
    if (c->vertex_count >= c->vertex_capacity) {
        return;
    }
    v = &c->vertices[c->vertex_count++];
    v->x = (x / vw) * 2.0f - 1.0f;
    v->y = 1.0f - (y / vh) * 2.0f;
    v->r = r;
    v->g = g;
    v->b = b;
    v->a = a;
}

static void push_quad(wn_core *c, float x, float y, float w, float h,
                      float vw, float vh, unsigned int argb)
{
    uint8_t a;
    uint8_t r, g, b;
    float x1, y1;
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    a = (uint8_t)((argb >> 24) & 0xFFu);
    if (a == 0) {
        return;
    }
    r = (uint8_t)((argb >> 16) & 0xFFu);
    g = (uint8_t)((argb >> 8) & 0xFFu);
    b = (uint8_t)(argb & 0xFFu);
    x1 = x + w;
    y1 = y + h;
    push_vertex(c, x, y, vw, vh, r, g, b, a);
    push_vertex(c, x1, y, vw, vh, r, g, b, a);
    push_vertex(c, x1, y1, vw, vh, r, g, b, a);
    push_vertex(c, x, y, vw, vh, r, g, b, a);
    push_vertex(c, x1, y1, vw, vh, r, g, b, a);
    push_vertex(c, x, y1, vw, vh, r, g, b, a);
}

/* CSS core v1: 8 segments per corner, clockwise from the top-right arc.
 * A zero/negative radius falls back to the exact 6-vertex push_quad path so
 * pages without border-radius keep byte-identical geometry. */
#define WN_CORNER_SEGMENTS 8
#define WN_ROUND_POINTS (4 * (WN_CORNER_SEGMENTS + 1))
#define WN_PI_ 3.14159265358979323846f

static void rounded_outline(float x, float y, float w, float h, float radius,
                            float points[WN_ROUND_POINTS][2])
{
    const float half_pi = WN_PI_ * 0.5f;
    float centers_x[4];
    float centers_y[4];
    float start[4];
    int corner;
    int step;
    int count = 0;
    centers_x[0] = x + w - radius;
    centers_x[1] = x + w - radius;
    centers_x[2] = x + radius;
    centers_x[3] = x + radius;
    centers_y[0] = y + radius;
    centers_y[1] = y + h - radius;
    centers_y[2] = y + h - radius;
    centers_y[3] = y + radius;
    start[0] = -half_pi;
    start[1] = 0.0f;
    start[2] = half_pi;
    start[3] = WN_PI_;
    for (corner = 0; corner < 4; ++corner) {
        for (step = 0; step <= WN_CORNER_SEGMENTS; ++step) {
            float angle = start[corner] +
                          half_pi * ((float)step / (float)WN_CORNER_SEGMENTS);
            points[count][0] = centers_x[corner] + radius * cosf(angle);
            points[count][1] = centers_y[corner] + radius * sinf(angle);
            ++count;
        }
    }
}

static void push_rounded_quad(wn_core *c, float x, float y, float w, float h,
                              float radius, float vw, float vh,
                              unsigned int argb)
{
    float points[WN_ROUND_POINTS][2];
    float cx, cy;
    uint8_t a, r, g, b;
    float limit;
    int i;
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    a = (uint8_t)((argb >> 24) & 0xFFu);
    if (a == 0) {
        return;
    }
    limit = 0.5f * fminf(w, h);
    if (radius > limit) {
        radius = limit;
    }
    if (radius <= 0.5f) {
        push_quad(c, x, y, w, h, vw, vh, argb);
        return;
    }
    r = (uint8_t)((argb >> 16) & 0xFFu);
    g = (uint8_t)((argb >> 8) & 0xFFu);
    b = (uint8_t)(argb & 0xFFu);
    rounded_outline(x, y, w, h, radius, points);
    cx = x + w * 0.5f;
    cy = y + h * 0.5f;
    for (i = 0; i < WN_ROUND_POINTS; ++i) {
        int next = (i + 1) % WN_ROUND_POINTS;
        push_vertex(c, cx, cy, vw, vh, r, g, b, a);
        push_vertex(c, points[i][0], points[i][1], vw, vh, r, g, b, a);
        push_vertex(c, points[next][0], points[next][1], vw, vh, r, g, b, a);
    }
}

/* Border box fill: rounded when the node has a radius, exact quad otherwise. */
static void push_box_shape(wn_core *c, float x, float y, float w, float h,
                           float radius, unsigned int argb)
{
    push_rounded_quad(c, x, y, w, h, radius,
                      (float)c->width, (float)c->height, argb);
}

/* Hollow rounded ring (E1 focus ring): band between the outer rounded rect and
 * the same rect inset by `ring`. A square box keeps the exact 4-quad path so
 * inputs without border-radius stay byte-identical. */
static void push_rounded_ring(wn_core *c, float x, float y, float w, float h,
                              float radius, float ring, unsigned int argb)
{
    float outer[WN_ROUND_POINTS][2];
    float inner[WN_ROUND_POINTS][2];
    uint8_t a, r, g, b;
    int i;
    if (w <= 0.0f || h <= 0.0f || ring <= 0.0f) {
        return;
    }
    a = (uint8_t)((argb >> 24) & 0xFFu);
    if (a == 0u) {
        return;
    }
    if (radius <= 0.5f) {
        const float vw = (float)c->width;
        const float vh = (float)c->height;
        push_quad(c, x, y, w, ring, vw, vh, argb);
        push_quad(c, x, y + h - ring, w, ring, vw, vh, argb);
        push_quad(c, x, y, ring, h, vw, vh, argb);
        push_quad(c, x + w - ring, y, ring, h, vw, vh, argb);
        return;
    }
    {
        float inner_w = w - ring * 2.0f;
        float inner_h = h - ring * 2.0f;
        float inner_radius = radius - ring;
        float limit = 0.5f * fminf(w, h);
        if (radius > limit) {
            radius = limit;
        }
        if (inner_w <= 0.0f || inner_h <= 0.0f) {
            return;
        }
        if (inner_radius < 0.0f) {
            inner_radius = 0.0f;
        }
        limit = 0.5f * fminf(inner_w, inner_h);
        if (inner_radius > limit) {
            inner_radius = limit;
        }
        rounded_outline(x, y, w, h, radius, outer);
        rounded_outline(x + ring, y + ring, inner_w, inner_h, inner_radius, inner);
    }
    r = (uint8_t)((argb >> 16) & 0xFFu);
    g = (uint8_t)((argb >> 8) & 0xFFu);
    b = (uint8_t)(argb & 0xFFu);
    for (i = 0; i < WN_ROUND_POINTS; ++i) {
        int next = (i + 1) % WN_ROUND_POINTS;
        push_vertex(c, outer[i][0], outer[i][1], (float)c->width,
                    (float)c->height, r, g, b, a);
        push_vertex(c, outer[next][0], outer[next][1], (float)c->width,
                    (float)c->height, r, g, b, a);
        push_vertex(c, inner[i][0], inner[i][1], (float)c->width,
                    (float)c->height, r, g, b, a);
        push_vertex(c, inner[next][0], inner[next][1], (float)c->width,
                    (float)c->height, r, g, b, a);
        push_vertex(c, outer[next][0], outer[next][1], (float)c->width,
                    (float)c->height, r, g, b, a);
        push_vertex(c, inner[i][0], inner[i][1], (float)c->width,
                    (float)c->height, r, g, b, a);
    }
}

/* Approximate the first non-inset box-shadow with three translucent layers:
 * the outer one is the blur-cap (lowest alpha), the inner one the spread-only
 * box. They are painted largest first so overlap accumulates towards the box.
 * v1 draws filled layers behind the node; an opaque background hides the part
 * under the border box (documented limitation for semi-transparent fills). */
static void push_box_shadow(wn_core *c, const UiNode *n, const wn_rect *r)
{
    static const float layer_alpha[3] = { 0.50f, 0.35f, 0.25f };
    static const float layer_expand[3] = { 1.00f, 0.50f, 0.00f };
    unsigned int color = n->shadow_color;
    unsigned int a = (color >> 24) & 0xFFu;
    float blur;
    int layer;
    if (a == 0u) {
        return;
    }
    blur = fmaxf(n->shadow_blur, 0.0f);
    for (layer = 0; layer < 3; ++layer) {
        float expand = n->shadow_spread + blur * layer_expand[layer];
        unsigned int alpha = (unsigned int)((float)a * layer_alpha[layer] + 0.5f);
        unsigned int layer_color = (alpha << 24) | (color & 0x00FFFFFFu);
        push_box_shape(c,
                       r->x + n->shadow_x - expand,
                       r->y + n->shadow_y - expand,
                       r->w + expand * 2.0f,
                       r->h + expand * 2.0f,
                       n->border_radius + expand,
                       layer_color);
    }
}


static void push_glyph_vertex(wn_core *c, float x, float y, float u, float v,
                              float vw, float vh,
                              uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    wn_glyph_vertex *out;
    ensure_glyph_capacity(c, c->glyph_vertex_count + 1);
    if (c->glyph_vertex_count >= c->glyph_vertex_capacity) {
        return;
    }
    out = &c->glyph_vertices[c->glyph_vertex_count++];
    out->x = (x / vw) * 2.0f - 1.0f;
    out->y = 1.0f - (y / vh) * 2.0f;
    out->u = u;
    out->v = v;
    out->r = r;
    out->g = g;
    out->b = b;
    out->a = a;
}

static void push_glyph_quad(wn_core *c, float x, float y, float w, float h,
                            float u0, float v0, float u1, float v1,
                            float vw, float vh,
                            uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    float x1 = x + w, y1 = y + h;
    push_glyph_vertex(c, x, y, u0, v0, vw, vh, r, g, b, a);
    push_glyph_vertex(c, x1, y, u1, v0, vw, vh, r, g, b, a);
    push_glyph_vertex(c, x1, y1, u1, v1, vw, vh, r, g, b, a);
    push_glyph_vertex(c, x, y, u0, v0, vw, vh, r, g, b, a);
    push_glyph_vertex(c, x1, y1, u1, v1, vw, vh, r, g, b, a);
    push_glyph_vertex(c, x, y1, u0, v1, vw, vh, r, g, b, a);
}

/* ---------------------------------------------------------- draw batches */

static int same_clip(const wn_draw_batch *b, const wn_rect *clip)
{
    return b->clip_x == clip->x && b->clip_y == clip->y &&
           b->clip_w == clip->w && b->clip_h == clip->h;
}

static void set_solid_clip(wn_core *c, const wn_rect *clip)
{
    wn_draw_batch *b;
    if (c->solid_batch_count > 0) {
        b = &c->solid_batches[c->solid_batch_count - 1];
        if (same_clip(b, clip)) {
            b->count = c->vertex_count - b->first;
            return;
        }
    }
    ensure_solid_batch_capacity(c, c->solid_batch_count + 1);
    if (c->solid_batch_count >= c->solid_batch_capacity) {
        return;
    }
    b = &c->solid_batches[c->solid_batch_count++];
    b->first = c->vertex_count;
    b->count = 0;
    b->clip_x = clip->x;
    b->clip_y = clip->y;
    b->clip_w = clip->w;
    b->clip_h = clip->h;
}

static void set_glyph_clip(wn_core *c, const wn_rect *clip)
{
    wn_draw_batch *b;
    if (c->glyph_batch_count > 0) {
        b = &c->glyph_batches[c->glyph_batch_count - 1];
        if (same_clip(b, clip)) {
            b->count = c->glyph_vertex_count - b->first;
            return;
        }
    }
    ensure_glyph_batch_capacity(c, c->glyph_batch_count + 1);
    if (c->glyph_batch_count >= c->glyph_batch_capacity) {
        return;
    }
    b = &c->glyph_batches[c->glyph_batch_count++];
    b->first = c->glyph_vertex_count;
    b->count = 0;
    b->clip_x = clip->x;
    b->clip_y = clip->y;
    b->clip_w = clip->w;
    b->clip_h = clip->h;
}

static void finalize_draw_batches(wn_draw_batch *batches, int *count, int vertex_count)
{
    int input = *count;
    int out = 0;
    int i;
    for (i = 0; i < input; ++i) {
        int end = (i + 1 < input) ? batches[i + 1].first : vertex_count;
        int span = end - batches[i].first;
        if (span > 0) {
            batches[i].count = span;
            if (out != i) {
                batches[out] = batches[i];
            }
            ++out;
        }
    }
    *count = out;
}

static void push_image_vertex(wn_core *c, float x, float y, float u, float v,
                              float vw, float vh)
{
    wn_image_vertex *out;
    ensure_image_capacity(c, c->image_vertex_count + 1);
    if (c->image_vertex_count >= c->image_vertex_capacity) {
        return;
    }
    out = &c->image_vertices[c->image_vertex_count++];
    out->x = (x / vw) * 2.0f - 1.0f;
    out->y = 1.0f - (y / vh) * 2.0f;
    out->u = u;
    out->v = v;
}

/* CSS core v1: an image is clipped by its own border-radius, or by the radius
 * of a parent with overflow:hidden (the media-library pilot cards rely on
 * this). The parent radius is reduced by its border width, matching the
 * padding-box clip. Uniform radius only; the same value rounds all corners. */
static float image_corner_radius(const wn_core *c, int node, float w, float h)
{
    const UiNode *n = &c->nodes[node];
    float radius = n->border_radius;
    if (radius <= 0.0f && c->parent[node] >= 0) {
        const UiNode *parent = &c->nodes[c->parent[node]];
        if (parent->overflow == R2_OVERFLOW_HIDDEN && parent->border_radius > 0.0f) {
            radius = parent->border_radius - parent->border_width;
        }
    }
    if (radius > 0.0f) {
        float limit = 0.5f * fminf(w, h);
        if (radius > limit) {
            radius = limit;
        }
    }
    return radius > 0.5f ? radius : 0.0f;
}

static void push_image_textured(wn_core *c, float x, float y, float w, float h,
                                float u0, float v0, float u1, float v1,
                                float radius)
{
    float vw = (float)c->width;
    float vh = (float)c->height;
    float x1 = x + w;
    float y1 = y + h;
    if (radius <= 0.5f) {
        push_image_vertex(c, x, y, u0, v0, vw, vh);
        push_image_vertex(c, x1, y, u1, v0, vw, vh);
        push_image_vertex(c, x1, y1, u1, v1, vw, vh);
        push_image_vertex(c, x, y, u0, v0, vw, vh);
        push_image_vertex(c, x1, y1, u1, v1, vw, vh);
        push_image_vertex(c, x, y1, u0, v1, vw, vh);
    } else {
        float points[WN_ROUND_POINTS][2];
        float cx = x + w * 0.5f;
        float cy = y + h * 0.5f;
        float cu = (u0 + u1) * 0.5f;
        float cv = (v0 + v1) * 0.5f;
        int i;
        rounded_outline(x, y, w, h, radius, points);
        for (i = 0; i < WN_ROUND_POINTS; ++i) {
            int next = (i + 1) % WN_ROUND_POINTS;
            float u_a = u0 + (points[i][0] - x) / w * (u1 - u0);
            float v_a = v0 + (points[i][1] - y) / h * (v1 - v0);
            float u_b = u0 + (points[next][0] - x) / w * (u1 - u0);
            float v_b = v0 + (points[next][1] - y) / h * (v1 - v0);
            push_image_vertex(c, cx, cy, cu, cv, vw, vh);
            push_image_vertex(c, points[i][0], points[i][1], u_a, v_a, vw, vh);
            push_image_vertex(c, points[next][0], points[next][1], u_b, v_b, vw, vh);
        }
    }
}

/* object-fit fill/cover/contain -> destination quad + source UVs. */
static void push_image_quad(wn_core *c, int node, const wn_rect *clip)
{
    const UiNode *n = &c->nodes[node];
    const wn_rect *r = &c->rects[node];
    const char *src = wn_core_node_image_src(c, node);
    wn_image_batch *batch;
    float x = r->x + n->padding[3];
    float y = r->y + n->padding[0];
    float w = fmaxf(r->w - n->padding[1] - n->padding[3], 0.0f);
    float h = fmaxf(r->h - n->padding[0] - n->padding[2], 0.0f);
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    float iw = n->image_width;
    float ih = n->image_height;
    float radius;
    int vertices;

    if (src == NULL || w <= 0.0f || h <= 0.0f) {
        return;
    }
    if (iw > 0.0f && ih > 0.0f) {
        if (n->object_fit == R2_OBJECT_FIT_COVER) {
            float sx = w / iw;
            float sy = h / ih;
            float scale = sx > sy ? sx : sy;
            float vis_w = w / scale;
            float vis_h = h / scale;
            u0 = ((iw - vis_w) * 0.5f) / iw;
            v0 = ((ih - vis_h) * 0.5f) / ih;
            u1 = 1.0f - u0;
            v1 = 1.0f - v0;
        } else if (n->object_fit == R2_OBJECT_FIT_CONTAIN) {
            float sx = w / iw;
            float sy = h / ih;
            float scale = sx < sy ? sx : sy;
            float draw_w = iw * scale;
            float draw_h = ih * scale;
            x += (w - draw_w) * 0.5f;
            y += (h - draw_h) * 0.5f;
            w = draw_w;
            h = draw_h;
        }
    }
    radius = image_corner_radius(c, node, w, h);
    vertices = radius > 0.5f ? WN_ROUND_POINTS * 3 : 6;

    ensure_image_batch_capacity(c, c->image_batch_count + 1);
    if (c->image_batch_count >= c->image_batch_capacity) {
        return;
    }
    if (c->image_batch_count > 0) {
        wn_image_batch *last = &c->image_batches[c->image_batch_count - 1];
        if (last->src != NULL && strcmp(last->src, src) == 0 &&
            last->clip_x == clip->x && last->clip_y == clip->y &&
            last->clip_w == clip->w && last->clip_h == clip->h) {
            last->count += vertices;
            batch = last;
            goto push_vertices;
        }
    }
    batch = &c->image_batches[c->image_batch_count++];
    batch->first = c->image_vertex_count;
    batch->count = vertices;
    batch->clip_x = clip->x;
    batch->clip_y = clip->y;
    batch->clip_w = clip->w;
    batch->clip_h = clip->h;
    batch->src = src;

push_vertices:
    (void)batch;
    push_image_textured(c, x, y, w, h, u0, v0, u1, v1, radius);
}

static void build_text_geometry(wn_core *c, float viewport_w, float viewport_h)
{
    int order_index;
    for (order_index = 0; order_index < c->paint_count; ++order_index) {
        int node = c->paint_order[order_index];
        const UiNode *n = &c->nodes[node];
        const wn_rect *r;
        unsigned int color;
        uint8_t a, cr, cg, cb;
        float align_dx = 0.0f;
        int j;
        if (!c->visible_eff[node] || n->kind != R2_NODE_TEXT) {
            continue;
        }
        if (c->run_count[node] <= 0 && c->text_override[node] == NULL) {
            continue;
        }
        r = &c->rects[node];
        /* E1 `text-align`: a text node is the inline content of a block
         * container, so shift the run inside the parent content box. Flex and
         * grid parents already position the text item (justify/align), so the
         * shift would double-apply there. */
        if (WN_TEXT_ALIGN(n) != 0 && c->parent[node] >= 0) {
            const UiNode *p = &c->nodes[c->parent[node]];
            if (p->kind == R2_NODE_BLOCK) {
                float avail = c->rects[c->parent[node]].w - p->padding[1] - p->padding[3];
                float room = avail - r->w;
                if (room > 0.0f) {
                    align_dx = WN_TEXT_ALIGN(n) == 1 ? room * 0.5f : room;
                }
            }
        }
        color = n->text_color;
        a = (uint8_t)((color >> 24) & 0xFFu);
        if (a == 0) {
            continue;
        }
        if (rect_is_empty(c->clips[node])) {
            continue;
        }
        set_glyph_clip(c, &c->clips[node]);
        cr = (uint8_t)((color >> 16) & 0xFFu);
        cg = (uint8_t)((color >> 8) & 0xFFu);
        cb = (uint8_t)(color & 0xFFu);

        if (c->text_override[node] != NULL) {
            int font_px = (int)(n->font_size + 0.5f);
            GlyphSet *set = NULL;
            float pen = 0.0f;
            int s;
            for (s = 0; s < c->glyph_set_count; ++s) {
                if (c->glyph_sets[s].font_px == font_px) {
                    set = &c->glyph_sets[s];
                    break;
                }
            }
            if (set == NULL) {
                continue;
            }
            for (const unsigned char *p = (const unsigned char *)c->text_override[node];
                 *p != '\0'; ++p) {
                const GlyphEntry *entry;
                if (*p >= 128) {
                    break;
                }
                if (*p == ' ') {
                    pen += set->space_advance;
                    continue;
                }
                entry = &set->ascii[*p];
                if (!entry->valid) {
                    break;
                }
                float x = floorf(r->x + align_dx + pen + entry->off_x + 0.5f);
                float y = floorf(r->y + entry->off_y + 0.5f);
                push_glyph_quad(c, x, y, entry->w, entry->h,
                                entry->u0, entry->v0, entry->u1, entry->v1,
                                viewport_w, viewport_h, cr, cg, cb, a);
                pen += entry->advance;
            }
            continue;
        }

        for (j = 0; j < c->run_count[node]; ++j) {
            const UiGlyph *g = &c->doc->glyphs[c->run_first[node] + j];
            float x, y;
            if (g->w <= 0.0f || g->h <= 0.0f) {
                continue;
            }
            x = floorf(r->x + align_dx + g->x + 0.5f);
            y = floorf(r->y + g->y + 0.5f);
            push_glyph_quad(c, x, y, g->w, g->h, g->u0, g->v0, g->u1, g->v1,
                            viewport_w, viewport_h, cr, cg, cb, a);
        }
    }
}

static void build_input_text_geometry(wn_core *c, float viewport_w, float viewport_h)
{
    int order_index;
    for (order_index = 0; order_index < c->paint_count; ++order_index) {
        int node = c->paint_order[order_index];
        const UiNode *n = &c->nodes[node];
        const WnInputRecord *record = input_record(c, node);
        const char *value;
        const char *text;
        unsigned int color;
        uint8_t a, cr, cg, cb;
        wn_rect content;
        wn_rect clip;
        float pen;
        float scroll;
        float line_offset;
        int use_placeholder;
        if (!c->visible_eff[node] || n->kind != R2_NODE_INPUT || record == NULL) {
            continue;
        }
        value = input_text(c, node);
        use_placeholder = value[0] == '\0' && record->placeholder[0] != '\0';
        text = use_placeholder ? record->placeholder : value;
        if (text[0] == '\0') {
            continue;
        }
        color = use_placeholder ? WN_INPUT_PLACEHOLDER_COLOR : n->text_color;
        a = (uint8_t)((color >> 24) & 0xFFu);
        if (a == 0u) {
            continue;
        }
        content = input_content_rect(c, node);
        clip = rect_intersection(c->clips[node], content);
        if (rect_is_empty(clip)) {
            continue;
        }
        input_update_scroll(c, node);
        scroll = c->input_scroll[node];
        set_glyph_clip(c, &clip);
        cr = (uint8_t)((color >> 16) & 0xFFu);
        cg = (uint8_t)((color >> 8) & 0xFFu);
        cb = (uint8_t)(color & 0xFFu);
        /* Single-line inputs center the line box vertically, like a browser
         * text field; a content box shorter than the line still overflows
         * symmetrically and is clipped. */
        line_offset = (content.h - record->line_height) * 0.5f;
        pen = 0.0f;
        for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
            const WnInputGlyph *glyph = input_glyph(record, *p);
            if (glyph == NULL) {
                continue;
            }
            if (glyph->w > 0.0f && glyph->h > 0.0f) {
                float x = floorf(content.x + pen + glyph->x - scroll + 0.5f);
                float y = floorf(content.y + line_offset + glyph->y + 0.5f);
                push_glyph_quad(c, x, y, glyph->w, glyph->h,
                                glyph->u0, glyph->v0, glyph->u1, glyph->v1,
                                viewport_w, viewport_h, cr, cg, cb, a);
            }
            pen += glyph->advance;
        }
    }
}

/* Selection quad, focus ring and caret. Solids are painted before glyphs, so
 * this runs after every background but before build_text_geometry. */
static void build_input_decorations(wn_core *c)
{
    int order_index;
    for (order_index = 0; order_index < c->paint_count; ++order_index) {
        int node = c->paint_order[order_index];
        const UiNode *n = &c->nodes[node];
        const WnInputRecord *record = input_record(c, node);
        wn_rect content;
        wn_rect clip;
        float scroll;
        int start;
        int end;
        if (!c->visible_eff[node] || n->kind != R2_NODE_INPUT || record == NULL) {
            continue;
        }
        if (rect_is_empty(c->clips[node])) {
            continue;
        }
        content = input_content_rect(c, node);
        input_update_scroll(c, node);
        scroll = c->input_scroll[node];

        if (c->input_focus == node) {
            const wn_rect *r = &c->rects[node];
            const float ring = 2.0f;
            set_solid_clip(c, &c->clips[node]);
            push_rounded_ring(c, r->x, r->y, r->w, r->h, n->border_radius, ring,
                              WN_INPUT_FOCUS_COLOR);
        }

        clip = rect_intersection(c->clips[node], content);
        if (rect_is_empty(clip)) {
            continue;
        }
        set_solid_clip(c, &clip);

        if (c->input_focus == node && input_has_selection(c, node)) {
            float x0;
            float x1;
            input_selection_bounds(c, node, &start, &end);
            x0 = content.x + input_prefix_width(record, input_text(c, node), start) - scroll;
            x1 = content.x + input_prefix_width(record, input_text(c, node), end) - scroll;
            if (x0 < content.x) {
                x0 = content.x;
            }
            if (x1 > content.x + content.w) {
                x1 = content.x + content.w;
            }
            if (x1 > x0) {
                push_quad(c, x0, content.y, x1 - x0, content.h,
                          (float)c->width, (float)c->height,
                          WN_INPUT_SELECTION_COLOR);
            }
        }

        if (c->input_focus == node) {
            float caret = content.x +
                          input_prefix_width(record, input_text(c, node),
                                             c->input_cursor[node]) -
                          scroll;
            float max_caret = content.x + content.w - WN_INPUT_CARET_WIDTH;
            if (caret < content.x) {
                caret = content.x;
            }
            if (caret > max_caret) {
                caret = max_caret;
            }
            caret = floorf(caret + 0.5f);
            if (content.h > 2.0f) {
                push_quad(c, caret, content.y + 1.0f, WN_INPUT_CARET_WIDTH,
                          content.h - 2.0f, (float)c->width, (float)c->height,
                          n->text_color);
            }
        }
    }
}

/* E1: visible vertical scrollbar for `overflow:auto|scroll` boxes that really
 * overflow. Subtle track + rounded thumb; painted inside the node clip before
 * images/text. Wheel scrolling needs no hit test on the bar. */
static void build_scrollbars(wn_core *c)
{
    const float track_w = 8.0f;
    const float edge = 2.0f;
    int order;
    for (order = 0; order < c->paint_count; ++order) {
        int node = c->paint_order[order];
        const UiNode *n = &c->nodes[node];
        const wn_rect *r;
        float view_h;
        float content_h;
        float track_x, track_y, track_h;
        float thumb_h, thumb_y, travel;
        if (!c->visible_eff[node] || n->overflow != R2_OVERFLOW_SCROLL) {
            continue;
        }
        if (c->scroll_max[node] <= 0.5f || rect_is_empty(c->clips[node])) {
            continue;
        }
        r = &c->rects[node];
        view_h = r->h - n->padding[0] - n->padding[2];
        if (view_h <= track_w * 2.0f) {
            continue;
        }
        content_h = view_h + c->scroll_max[node];
        track_x = r->x + r->w - n->padding[1] - track_w - edge;
        track_y = r->y + n->padding[0] + edge;
        track_h = view_h - edge * 2.0f;
        if (track_h <= 0.0f) {
            continue;
        }
        thumb_h = track_h * view_h / content_h;
        if (thumb_h < 24.0f) {
            thumb_h = 24.0f;
        }
        if (thumb_h > track_h) {
            thumb_h = track_h;
        }
        travel = track_h - thumb_h;
        thumb_y = track_y;
        if (c->scroll_max[node] > 0.0f && travel > 0.0f) {
            thumb_y += travel * (c->scroll[node] / c->scroll_max[node]);
        }
        set_solid_clip(c, &c->clips[node]);
        push_rounded_quad(c, track_x, track_y, track_w, track_h,
                          track_w * 0.5f, (float)c->width, (float)c->height,
                          0x33000000u);
        push_rounded_quad(c, track_x, thumb_y, track_w, thumb_h,
                          track_w * 0.5f, (float)c->width, (float)c->height,
                          0x665A667Au);
    }
}

void wn_core_build_display_list(wn_core *core)
{
    int i;
    unsigned int clear = 0xFF0B0E14u;
    int root = -1;
    if (core == NULL) {
        return;
    }
    if (core->needs_layout) {
        layout_page(core, (float)core->width, (float)core->height);
        core->needs_layout = 0;
    }
    core->vertex_count = 0;
    core->solid_batch_count = 0;
    core->glyph_vertex_count = 0;
    core->glyph_batch_count = 0;
    core->image_vertex_count = 0;
    core->image_batch_count = 0;

    for (i = 0; i < core->paint_count; ++i) {
        int node = core->paint_order[i];
        const UiNode *n = &core->nodes[node];
        unsigned int color;
        const wn_rect *r;
        if (!core->visible_eff[node]) {
            continue;
        }
        if (rect_is_empty(core->clips[node])) {
            continue;
        }
        r = &core->rects[node];
        if (((n->shadow_color >> 24) & 0xFFu) != 0u) {
            set_solid_clip(core, &core->clips[node]);
            push_box_shadow(core, n, r);
        }
        color = node_color(core, node);
        if (color == 0) {
            continue;
        }
        set_solid_clip(core, &core->clips[node]);
        if (n->border_width > 0.0f && ((n->border_color >> 24) & 0xFFu) != 0u &&
            ((color >> 24) & 0xFFu) == 0xFFu) {
            /* Opaque fill: border ring first, then the background inset by the
             * uniform border width. Children still paint over the border box
             * (v1 has no layout change). */
            float bw = n->border_width;
            float inner_w = r->w - bw * 2.0f;
            float inner_h = r->h - bw * 2.0f;
            float inner_r = n->border_radius - bw;
            push_box_shape(core, r->x, r->y, r->w, r->h,
                           n->border_radius, n->border_color);
            push_box_shape(core, r->x + bw, r->y + bw, inner_w, inner_h,
                           inner_r > 0.0f ? inner_r : 0.0f, color);
        } else {
            push_box_shape(core, r->x, r->y, r->w, r->h,
                           n->border_radius, color);
        }
    }
    build_input_decorations(core);
    build_scrollbars(core);
    for (i = 0; i < core->paint_count; ++i) {
        int node = core->paint_order[i];
        if (core->nodes[node].kind == R2_NODE_IMAGE &&
            !rect_is_empty(core->clips[node])) {
            push_image_quad(core, node, &core->clips[node]);
        }
    }
    build_text_geometry(core, (float)core->width, (float)core->height);
    build_input_text_geometry(core, (float)core->width, (float)core->height);

    finalize_draw_batches(core->solid_batches, &core->solid_batch_count,
                          core->vertex_count);
    finalize_draw_batches(core->glyph_batches, &core->glyph_batch_count,
                          core->glyph_vertex_count);

    for (i = 0; i < core->node_count; ++i) {
        if (core->alive[i] && core->parent[i] < 0) {
            root = i;
            break;
        }
    }
    if (root >= 0) {
        unsigned int bg = core->nodes[root].background;
        if ((bg >> 24) != 0) {
            clear = bg;
        }
    }

    core->list.vertices = core->vertices;
    core->list.vertex_count = core->vertex_count;
    core->list.solid_batches = core->solid_batches;
    core->list.solid_batch_count = core->solid_batch_count;
    core->list.glyph_vertices = core->glyph_vertices;
    core->list.glyph_vertex_count = core->glyph_vertex_count;
    core->list.glyph_batches = core->glyph_batches;
    core->list.glyph_batch_count = core->glyph_batch_count;
    core->list.image_vertices = core->image_vertices;
    core->list.image_vertex_count = core->image_vertex_count;
    core->list.image_batches = core->image_batches;
    core->list.image_batch_count = core->image_batch_count;
    core->list.atlas.pixels = core->doc->atlas;
    core->list.atlas.width = core->doc->atlas_width;
    core->list.atlas.height = core->doc->atlas_height;
    core->list.atlas.size = core->doc->atlas_width * core->doc->atlas_height;
    core->list.clear_color[0] = (float)((clear >> 16) & 0xFFu) / 255.0f;
    core->list.clear_color[1] = (float)((clear >> 8) & 0xFFu) / 255.0f;
    core->list.clear_color[2] = (float)(clear & 0xFFu) / 255.0f;
    core->list.clear_color[3] = (float)((clear >> 24) & 0xFFu) / 255.0f;
    core->list.width = core->width;
    core->list.height = core->height;
    core->dirty = 0;
}

const wn_display_list *wn_core_display_list(const wn_core *core)
{
    return core != NULL ? &core->list : NULL;
}

const wn_rect *wn_core_rects(const wn_core *core, int *count)
{
    if (core == NULL) {
        if (count != NULL) {
            *count = 0;
        }
        return NULL;
    }
    if (count != NULL) {
        *count = core->node_count;
    }
    return core->rects;
}

double wn_core_last_layout_ms(const wn_core *core)
{
    return core != NULL ? core->last_layout_ms : 0.0;
}

/* -------------------------------------------------------------- lifecycle */

/* Copy the generated ui-tree/3 input tables into the core's compact state.
 * Compiled out entirely for pages whose ui_data.h has no input tables. */
static char *input_alloc_value(const char *text)
{
    /* Fixed capacity: editing inserts in place up to WN_INPUT_MAX_BYTES. */
    char *buffer = (char *)calloc((size_t)WN_INPUT_MAX_BYTES + 1u, 1u);
    if (buffer != NULL && text != NULL) {
        size_t length = strlen(text);
        if (length > WN_INPUT_MAX_BYTES) {
            length = WN_INPUT_MAX_BYTES;
        }
        memcpy(buffer, text, length);
    }
    return buffer;
}

static int build_input_state(wn_core *c)
{
#ifdef UI_INPUT_COUNT
    int i;
    int j;
    if (UI_INPUT_COUNT <= 0) {
        return 0;
    }
    c->input_record_count = UI_INPUT_COUNT;
    c->input_records = (WnInputRecord *)calloc(
        (size_t)UI_INPUT_COUNT, sizeof(WnInputRecord));
    c->input_pool_count = UI_INPUT_GLYPH_COUNT;
    c->input_pool = (WnInputGlyph *)calloc(
        (size_t)(UI_INPUT_GLYPH_COUNT > 0 ? UI_INPUT_GLYPH_COUNT : 1),
        sizeof(WnInputGlyph));
    if (c->input_records == NULL || c->input_pool == NULL) {
        return -1;
    }
    for (i = 0; i < UI_INPUT_GLYPH_COUNT; ++i) {
        WnInputGlyph *dst = &c->input_pool[i];
        dst->advance = kUiInputGlyphs[i].advance;
        dst->x = kUiInputGlyphs[i].x;
        dst->y = kUiInputGlyphs[i].y;
        dst->w = kUiInputGlyphs[i].w;
        dst->h = kUiInputGlyphs[i].h;
        dst->u0 = kUiInputGlyphs[i].u0;
        dst->v0 = kUiInputGlyphs[i].v0;
        dst->u1 = kUiInputGlyphs[i].u1;
        dst->v1 = kUiInputGlyphs[i].v1;
    }
    for (i = 0; i < UI_INPUT_COUNT; ++i) {
        WnInputRecord *record = &c->input_records[i];
        int first = kUiInputs[i].glyph_first;
        int count = kUiInputs[i].glyph_count;
        int node = kUiInputs[i].node;
        record->node = node;
        record->placeholder =
            kUiInputs[i].placeholder != NULL ? kUiInputs[i].placeholder : "";
        record->max_length = kUiInputs[i].max_length;
        if (record->max_length < 0 || record->max_length > WN_INPUT_MAX_BYTES) {
            record->max_length = WN_INPUT_MAX_BYTES;
        }
        record->baseline = kUiInputs[i].baseline;
        record->line_height = kUiInputs[i].line_height;
        if (first < 0 || first > c->input_pool_count) {
            first = 0;
            count = 0;
        }
        if (count < 0 || first + count > c->input_pool_count) {
            count = c->input_pool_count - first;
        }
        record->glyphs = c->input_pool + first;
        record->glyph_count = count;
        for (j = 0; j < 128; ++j) {
            record->cp_index[j] = -1;
        }
        for (j = 0; j < count; ++j) {
            int cp = kUiInputGlyphs[first + j].cp;
            if (cp >= 0 && cp < 128) {
                record->cp_index[cp] = (short)j;
            }
        }
        if (node >= 0 && node < c->node_count) {
            const char *value = kUiInputs[i].value != NULL ? kUiInputs[i].value : "";
            c->input_map[node] = i;
            c->input_value[node] = input_alloc_value(value);
            if (c->input_value[node] == NULL) {
                return -1;
            }
            c->input_cursor[node] = (int)strlen(value);
            c->input_anchor[node] = -1;
        }
    }
#else
    (void)c;
#endif
    return 0;
}

wn_core *wn_core_create(const wn_document *document)
{
    wn_core *c;
    int count;
    int capacity;
    int i;
    size_t bytes;
    if (document == NULL || document->node_count <= 0) {
        return NULL;
    }
    c = (wn_core *)calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->doc = document;
    count = document->node_count;
    capacity = count > WN_MAX_NODES ? count : WN_MAX_NODES;
    c->node_capacity = capacity;
    c->node_count = count;
    c->live_count = count;
    c->hover = c->press = c->selected = -1;
    c->width = document->viewport_width;
    c->height = document->viewport_height;

    bytes = (size_t)capacity;
    c->nodes = (UiNode *)calloc(bytes, sizeof(UiNode));
    c->names = (const char **)calloc(bytes, sizeof(char *));
    c->alive = (unsigned char *)calloc(bytes, sizeof(unsigned char));
    c->dynamic_node = (unsigned char *)calloc(bytes, sizeof(unsigned char));
    c->visible = (unsigned char *)calloc(bytes, sizeof(unsigned char));
    c->visible_eff = (unsigned char *)calloc(bytes, sizeof(unsigned char));
    c->parent = (int *)calloc(bytes, sizeof(int));
    c->order_key = (int *)calloc(bytes, sizeof(int));
    c->run_first = (int *)calloc(bytes, sizeof(int));
    c->run_count = (int *)calloc(bytes, sizeof(int));
    c->free_list = (int *)calloc(bytes, sizeof(int));
    c->rects = (wn_rect *)calloc(bytes, sizeof(wn_rect));
    c->clips = (wn_rect *)calloc(bytes, sizeof(wn_rect));
    c->scroll = (float *)calloc(bytes, sizeof(float));
    c->scroll_max = (float *)calloc(bytes, sizeof(float));
    c->first_child = (int *)calloc(bytes, sizeof(int));
    c->next_sibling = (int *)calloc(bytes, sizeof(int));
    c->last_child = (int *)calloc(bytes, sizeof(int));
    c->paint_order = (int *)calloc(bytes, sizeof(int));
    c->text_override = (char **)calloc(bytes, sizeof(char *));
    c->image_override = (char **)calloc(bytes, sizeof(char *));
    c->input_map = (int *)calloc(bytes, sizeof(int));
    c->input_value = (char **)calloc(bytes, sizeof(char *));
    c->input_cursor = (int *)calloc(bytes, sizeof(int));
    c->input_anchor = (int *)calloc(bytes, sizeof(int));
    c->input_scroll = (float *)calloc(bytes, sizeof(float));
    c->class_count = (unsigned char *)calloc(bytes, sizeof(unsigned char));
    c->classes = (char **)calloc(bytes * WN_MAX_CLASSES, sizeof(char *));
    c->glyph_sets = (GlyphSet *)calloc(WN_MAX_GLYPH_SETS, sizeof(GlyphSet));
    c->vertex_capacity = count * 6;
    c->vertices = (wn_vertex *)calloc((size_t)c->vertex_capacity, sizeof(wn_vertex));
    c->solid_batch_capacity = count > 0 ? count : 1;
    c->solid_batches = (wn_draw_batch *)calloc(
        (size_t)c->solid_batch_capacity, sizeof(wn_draw_batch));
    c->glyph_vertex_capacity = (document->glyph_count > 0 ? document->glyph_count : 1) * 6;
    c->glyph_vertices = (wn_glyph_vertex *)calloc(
        (size_t)c->glyph_vertex_capacity, sizeof(wn_glyph_vertex));
    c->glyph_batch_capacity = count > 0 ? count : 1;
    c->glyph_batches = (wn_draw_batch *)calloc(
        (size_t)c->glyph_batch_capacity, sizeof(wn_draw_batch));
    c->image_vertex_capacity = count * 6;
    c->image_vertices = (wn_image_vertex *)calloc(
        (size_t)c->image_vertex_capacity, sizeof(wn_image_vertex));
    c->image_batch_capacity = count > 0 ? count : 1;
    c->image_batches = (wn_image_batch *)calloc(
        (size_t)c->image_batch_capacity, sizeof(wn_image_batch));

    if (c->nodes == NULL || c->names == NULL || c->alive == NULL ||
        c->dynamic_node == NULL || c->visible == NULL || c->visible_eff == NULL ||
        c->parent == NULL || c->order_key == NULL || c->run_first == NULL || c->run_count == NULL ||
        c->free_list == NULL || c->rects == NULL || c->clips == NULL ||
        c->scroll == NULL || c->scroll_max == NULL || c->first_child == NULL ||
        c->next_sibling == NULL || c->last_child == NULL || c->paint_order == NULL ||
        c->text_override == NULL || c->image_override == NULL ||
        c->input_map == NULL || c->input_value == NULL || c->input_cursor == NULL ||
        c->input_anchor == NULL || c->input_scroll == NULL ||
        c->class_count == NULL || c->classes == NULL ||
        c->glyph_sets == NULL || c->vertices == NULL || c->solid_batches == NULL ||
        c->glyph_vertices == NULL || c->glyph_batches == NULL ||
        c->image_vertices == NULL || c->image_batches == NULL) {
        wn_core_destroy(c);
        return NULL;
    }

    memcpy(c->nodes, document->nodes, (size_t)count * sizeof(UiNode));
    for (i = 0; i < count; ++i) {
        c->names[i] = document->names[i];
        c->alive[i] = 1;
        c->visible[i] = 1;
        c->visible_eff[i] = 1;
        c->parent[i] = c->nodes[i].parent;
        c->order_key[i] = i;
        c->run_first[i] = -1;
        c->run_count[i] = 0;
        c->first_child[i] = -1;
        c->next_sibling[i] = -1;
        c->last_child[i] = -1;
        c->input_map[i] = -1;
        c->input_cursor[i] = 0;
        c->input_anchor[i] = -1;
        c->input_scroll[i] = 0.0f;
    }
    c->input_focus = -1;
    for (i = 0; i < document->run_count; ++i) {
        const UiTextRun *run = &document->runs[i];
        if (run->node >= 0 && run->node < count && run->count > 0) {
            c->run_first[run->node] = run->first;
            c->run_count[run->node] = run->count;
        }
    }
    c->next_order = count;

    build_hierarchy(c);
    build_glyph_sets(c);
    if (build_input_state(c) != 0) {
        wn_core_destroy(c);
        return NULL;
    }
    layout_page(c, (float)c->width, (float)c->height);
    wn_core_build_display_list(c);
    return c;
}

/* ---------------------------------------------------------- dynamic arena */

static void free_node_payload(wn_core *c, int node)
{
    int i;
    if (c->dynamic_node != NULL && c->dynamic_node[node]) {
        wn_free_((void *)c->names[node]);
        c->names[node] = NULL;
    }
    if (c->text_override != NULL) {
        wn_free_(c->text_override[node]);
        c->text_override[node] = NULL;
    }
    if (c->image_override != NULL) {
        wn_free_(c->image_override[node]);
        c->image_override[node] = NULL;
    }
    if (c->input_value != NULL && c->input_value[node] != NULL) {
        wn_free_(c->input_value[node]);
        c->input_value[node] = NULL;
    }
    if (c->input_map != NULL) {
        c->input_map[node] = -1;
    }
    if (c->input_focus == node) {
        c->input_focus = -1;
    }
    if (c->classes != NULL && c->class_count != NULL) {
        for (i = 0; i < c->class_count[node] && i < WN_MAX_CLASSES; ++i) {
            wn_free_(c->classes[node * WN_MAX_CLASSES + i]);
            c->classes[node * WN_MAX_CLASSES + i] = NULL;
        }
        c->class_count[node] = 0;
    }
}

static int arena_alloc(wn_core *c)
{
    int id;
    int i;
    if (c->free_count > 0) {
        id = c->free_list[--c->free_count];
    } else {
        if (c->node_count >= c->node_capacity) {
            return -1;
        }
        id = c->node_count++;
    }
    memset(&c->nodes[id], 0, sizeof(UiNode));
    c->nodes[id].parent = -1;
    c->nodes[id].object_fit = R2_OBJECT_FIT_FILL;
    c->names[id] = NULL;
    c->alive[id] = 1;
    c->dynamic_node[id] = 1;
    c->visible[id] = 1;
    c->visible_eff[id] = 1;
    c->parent[id] = -1;
    c->order_key[id] = c->next_order++;
    c->run_first[id] = -1;
    c->run_count[id] = 0;
    c->rects[id].x = c->rects[id].y = 0.0f;
    c->rects[id].w = c->rects[id].h = 0.0f;
    c->clips[id] = c->rects[id];
    c->scroll[id] = 0.0f;
    c->scroll_max[id] = 0.0f;
    c->first_child[id] = -1;
    c->next_sibling[id] = -1;
    c->last_child[id] = -1;
    c->text_override[id] = NULL;
    c->image_override[id] = NULL;
    c->input_map[id] = -1;
    c->input_value[id] = NULL;
    c->input_cursor[id] = 0;
    c->input_anchor[id] = -1;
    c->input_scroll[id] = 0.0f;
    c->class_count[id] = 0;
    for (i = 0; i < WN_MAX_CLASSES; ++i) {
        c->classes[id * WN_MAX_CLASSES + i] = NULL;
    }
    ++c->live_count;
    return id;
}

static void arena_release(wn_core *c, int node)
{
    if (!c->alive[node]) {
        return;
    }
    free_node_payload(c, node);
    c->alive[node] = 0;
    c->dynamic_node[node] = 0;
    c->names[node] = NULL;
    c->parent[node] = -1;
    c->nodes[node].parent = -1;
    c->first_child[node] = -1;
    c->next_sibling[node] = -1;
    c->last_child[node] = -1;
    c->visible[node] = 0;
    c->visible_eff[node] = 0;
    c->rects[node].x = c->rects[node].y = 0.0f;
    c->rects[node].w = c->rects[node].h = 0.0f;
    c->clips[node] = c->rects[node];
    c->scroll[node] = 0.0f;
    c->scroll_max[node] = 0.0f;
    if (c->free_count < c->node_capacity) {
        c->free_list[c->free_count++] = node;
    }
    --c->live_count;
}

static void mark_core_dirty(wn_core *c)
{
    c->dirty = 1;
    c->needs_layout = 1;
}

int wn_core_dirty(const wn_core *core)
{
    return core != NULL ? core->dirty : 0;
}

int wn_core_needs_layout(const wn_core *core)
{
    return core != NULL ? core->needs_layout : 0;
}

void wn_core_clear_dirty(wn_core *core)
{
    if (core != NULL) {
        core->dirty = 0;
    }
}

int wn_core_set_visible(wn_core *core, int node, int visible)
{
    int state[3];
    int i;
    if (core == NULL || !wn_core_node_is_alive(core, node)) {
        return -1;
    }
    core->visible[node] = visible != 0 ? 1 : 0;
    if (!core->visible[node]) {
        state[0] = core->hover;
        state[1] = core->press;
        state[2] = core->selected;
        for (i = 0; i < 3; ++i) {
            int walk = state[i];
            while (walk >= 0) {
                if (walk == node) {
                    if (i == 0) core->hover = -1;
                    else if (i == 1) core->press = -1;
                    else core->selected = -1;
                    break;
                }
                walk = core->parent[walk];
            }
        }
        if (core->input_focus >= 0) {
            int walk = core->input_focus;
            while (walk >= 0) {
                if (walk == node) {
                    core->input_anchor[core->input_focus] = -1;
                    core->input_focus = -1;
                    break;
                }
                walk = core->parent[walk];
            }
        }
    }
    mark_core_dirty(core);
    return 0;
}

int wn_core_set_image(wn_core *core, int node, const char *path)
{
    char *copy;
    UiNode *n;
    if (core == NULL || !wn_core_node_is_alive(core, node) || path == NULL ||
        path[0] == '\0') {
        return -1;
    }
    n = &core->nodes[node];
    if (n->kind != R2_NODE_IMAGE) {
        return 1;
    }
    copy = wn_strdup_(path);
    if (copy == NULL) {
        return -2;
    }
    wn_free_(core->image_override[node]);
    core->image_override[node] = copy;
    mark_core_dirty(core);
    return 0;
}

static void collect_subtree(wn_core *c, int node, int *out, int *count)
{
    int child;
    if (!c->alive[node]) {
        return;
    }
    out[(*count)++] = node;
    for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
        collect_subtree(c, child, out, count);
    }
}

int wn_core_clone(wn_core *core, int template_node, int parent)
{
    int *src_ids;
    int *new_ids;
    int *map;
    int count = 0;
    int i;
    int result = -1;
    int oom = 0;
    char name[64];
    if (core == NULL || !wn_core_node_is_alive(core, template_node) ||
        !wn_core_node_is_alive(core, parent)) {
        return -1;
    }
    if (core->live_count >= core->node_capacity) {
        return -2;
    }
    src_ids = (int *)malloc((size_t)core->node_capacity * sizeof(int));
    new_ids = (int *)malloc((size_t)core->node_capacity * sizeof(int));
    map = (int *)malloc((size_t)core->node_capacity * sizeof(int));
    if (src_ids == NULL || new_ids == NULL || map == NULL) {
        free(src_ids);
        free(new_ids);
        free(map);
        return -2;
    }
    collect_subtree(core, template_node, src_ids, &count);
    if (count <= 0 || core->live_count + count > core->node_capacity) {
        free(src_ids);
        free(new_ids);
        free(map);
        return -2;
    }
    for (i = 0; i < core->node_capacity; ++i) {
        map[i] = -1;
    }
    for (i = 0; i < count; ++i) {
        map[src_ids[i]] = i;
    }
    for (i = 0; i < count; ++i) {
        new_ids[i] = arena_alloc(core);
        if (new_ids[i] < 0) {
            while (i > 0) {
                arena_release(core, new_ids[--i]);
            }
            free(src_ids);
            free(new_ids);
            free(map);
            return -2;
        }
    }
    for (i = 0; i < count; ++i) {
        int src = src_ids[i];
        int dst = new_ids[i];
        int src_parent = core->parent[src];
        core->nodes[dst] = core->nodes[src];
        core->nodes[dst].parent = -1;
        core->parent[dst] = (src_parent >= 0 && map[src_parent] >= 0)
                                ? new_ids[map[src_parent]]
                                : parent;
        if (i == 0) {
            core->parent[dst] = parent;
        }
        core->nodes[dst].parent = core->parent[dst];
        core->visible[dst] = core->visible[src];
        core->run_first[dst] = core->run_first[src];
        core->run_count[dst] = core->run_count[src];
        snprintf(name, sizeof(name), "clone#%d", core->clone_serial++);
        core->names[dst] = wn_strdup_(name);
        if (core->names[dst] == NULL) {
            for (i = 0; i < count; ++i) {
                arena_release(core, new_ids[i]);
            }
            free(src_ids);
            free(new_ids);
            free(map);
            return -2;
        }
        core->dynamic_node[dst] = 1;
        if (core->text_override[src] != NULL &&
            (core->text_override[dst] = wn_strdup_(core->text_override[src])) == NULL) {
            oom = 1;
        }
        if (core->image_override[src] != NULL &&
            (core->image_override[dst] = wn_strdup_(core->image_override[src])) == NULL) {
            oom = 1;
        }
        /* Un clon de un input comparte la piscina de glyphs del template pero
         * recibe su propia copia editable del valor. */
        core->input_map[dst] = core->input_map[src];
        if (core->input_map[src] >= 0) {
            const char *source_value =
                core->input_value[src] != NULL ? core->input_value[src] : "";
            core->input_value[dst] = input_alloc_value(source_value);
            if (core->input_value[dst] == NULL) {
                oom = 1;
            }
            core->input_cursor[dst] = core->input_cursor[src];
            core->input_anchor[dst] = core->input_anchor[src];
            core->input_scroll[dst] = core->input_scroll[src];
        }
        core->class_count[dst] = core->class_count[src];
        for (int k = 0; k < core->class_count[src] && k < WN_MAX_CLASSES; ++k) {
            core->classes[dst * WN_MAX_CLASSES + k] =
                wn_strdup_(core->classes[src * WN_MAX_CLASSES + k]);
            if (core->classes[dst * WN_MAX_CLASSES + k] == NULL) {
                oom = 1;
            }
        }
        if (oom) {
            for (i = 0; i < count; ++i) {
                arena_release(core, new_ids[i]);
            }
            free(src_ids);
            free(new_ids);
            free(map);
            return -2;
        }
    }
    build_hierarchy(core);
    mark_core_dirty(core);
    result = new_ids[0];
    free(src_ids);
    free(new_ids);
    free(map);
    return result;
}

int wn_core_remove(wn_core *core, int node)
{
    int *ids;
    int count = 0;
    int i;
    if (core == NULL || !wn_core_node_is_alive(core, node)) {
        return -1;
    }
    if (core->parent[node] < 0) {
        return -1; /* root */
    }
    ids = (int *)malloc((size_t)core->node_capacity * sizeof(int));
    if (ids == NULL) {
        return -2;
    }
    collect_subtree(core, node, ids, &count);
    for (i = 0; i < count; ++i) {
        if (core->hover == ids[i]) core->hover = -1;
        if (core->press == ids[i]) core->press = -1;
        if (core->selected == ids[i]) core->selected = -1;
    }
    for (i = count - 1; i >= 0; --i) {
        arena_release(core, ids[i]);
    }
    free(ids);
    build_hierarchy(core);
    mark_core_dirty(core);
    return 0;
}

int wn_core_clear_children(wn_core *core, int node)
{
    int child;
    int result = 0;
    if (core == NULL || !wn_core_node_is_alive(core, node)) {
        return -1;
    }
    child = core->first_child[node];
    while (child >= 0) {
        int next = core->next_sibling[child];
        int status = wn_core_remove(core, child);
        if (status != 0) {
            result = status;
        }
        child = next;
    }
    return result;
}

static int is_ancestor(const wn_core *c, int maybe_ancestor, int node)
{
    int walk = node;
    while (walk >= 0) {
        if (walk == maybe_ancestor) {
            return 1;
        }
        walk = c->parent[walk];
    }
    return 0;
}

int wn_core_append(wn_core *core, int parent, int child)
{
    if (core == NULL || !wn_core_node_is_alive(core, parent) ||
        !wn_core_node_is_alive(core, child)) {
        return -1;
    }
    if (parent == child || is_ancestor(core, child, parent)) {
        return -1;
    }
    if (core->parent[child] < 0) {
        return -1; /* document root */
    }
    core->parent[child] = parent;
    core->nodes[child].parent = parent;
    core->order_key[child] = core->next_order++;
    build_hierarchy(core);
    mark_core_dirty(core);
    return 0;
}

void wn_core_destroy(wn_core *core)
{
    int i;
    if (core == NULL) {
        return;
    }
    if (core->nodes != NULL) {
        for (i = 0; i < core->node_count; ++i) {
            if (core->alive != NULL && core->alive[i]) {
                free_node_payload(core, i);
            }
        }
    }
    wn_free_(core->nodes);
    wn_free_((void *)core->names);
    wn_free_(core->alive);
    wn_free_(core->dynamic_node);
    wn_free_(core->visible);
    wn_free_(core->visible_eff);
    wn_free_(core->parent);
    wn_free_(core->order_key);
    wn_free_(core->run_first);
    wn_free_(core->run_count);
    wn_free_(core->free_list);
    wn_free_(core->rects);
    wn_free_(core->clips);
    wn_free_(core->scroll);
    wn_free_(core->scroll_max);
    wn_free_(core->first_child);
    wn_free_(core->next_sibling);
    wn_free_(core->last_child);
    wn_free_(core->paint_order);
    wn_free_(core->text_override);
    wn_free_(core->image_override);
    wn_free_(core->input_map);
    wn_free_(core->input_value);
    wn_free_(core->input_cursor);
    wn_free_(core->input_anchor);
    wn_free_(core->input_scroll);
    wn_free_(core->input_records);
    wn_free_(core->input_pool);
    wn_free_(core->classes);
    wn_free_(core->class_count);
    wn_free_(core->vertices);
    wn_free_(core->solid_batches);
    wn_free_(core->glyph_vertices);
    wn_free_(core->glyph_batches);
    wn_free_(core->image_vertices);
    wn_free_(core->image_batches);
    wn_free_(core->glyph_sets);
    wn_free_(core);
}
