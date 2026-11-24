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

struct wn_core {
    const wn_document *doc;

    wn_rect *rects;
    wn_rect *clips;
    int *first_child;
    int *next_sibling;
    int *subtree_end;
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

    char **classes;
    unsigned char *class_count;

    GlyphSet *glyph_sets;
    int glyph_set_count;
};

/* Max-content width. Flex items use their content width, not the container
 * width (documented R2 behaviour). */
static float intrinsic_width(wn_core *c, int node, float available);
static float layout_node(wn_core *c, int node, float origin_x, float origin_y,
                         float available_w, float parent_content_h,
                         int is_root, float forced_w);
static void layout_page(wn_core *c, float viewport_w, float viewport_h);

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
    if (node < 0 || node >= c->doc->node_count) {
        return NULL;
    }
    return &c->doc->nodes[node];
}

int wn_core_node_count(const wn_core *core)
{
    return core != NULL ? core->doc->node_count : 0;
}

const char *wn_core_node_name(const wn_core *core, int node)
{
    if (core == NULL || node < 0 || node >= core->doc->node_count) {
        return NULL;
    }
    return core->doc->names[node];
}

const char *wn_core_node_text(const wn_core *core, int node)
{
    if (core == NULL || node < 0 || node >= core->doc->node_count) {
        return NULL;
    }
    if (core->text_override[node] != NULL) {
        return core->text_override[node];
    }
    return core->doc->nodes[node].text;
}

int wn_core_node_is_text(const wn_core *core, int node)
{
    const UiNode *n = core != NULL ? node_at(core, node) : NULL;
    return n != NULL && n->kind == R2_NODE_TEXT;
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
    for (i = 0; i < core->doc->node_count; ++i) {
        if (strcmp(core->doc->names[i], id) == 0) {
            return i;
        }
    }
    for (i = 0; i < core->doc->node_count; ++i) {
        if (name_matches_id(core->doc->names[i], id)) {
            match = i;
            ++matches;
        }
    }
    return matches == 1 ? match : -1;
}

/* ------------------------------------------------------------- hierarchy */

static void build_hierarchy(wn_core *c)
{
    int i;
    const int count = c->doc->node_count;
    for (i = 0; i < count; ++i) {
        c->first_child[i] = -1;
        c->next_sibling[i] = -1;
        c->last_child[i] = -1;
        c->subtree_end[i] = i + 1;
    }
    for (i = 0; i < count; ++i) {
        int parent = c->doc->nodes[i].parent;
        if (parent < 0 || parent >= count) {
            continue;
        }
        if (c->first_child[parent] < 0) {
            c->first_child[parent] = i;
        } else {
            c->next_sibling[c->last_child[parent]] = i;
        }
        c->last_child[parent] = i;
    }
    for (i = count - 1; i >= 0; --i) {
        int end = i + 1;
        int child;
        for (child = c->first_child[i]; child >= 0; child = c->next_sibling[child]) {
            if (c->subtree_end[child] > end) {
                end = c->subtree_end[child];
            }
        }
        c->subtree_end[i] = end;
    }
}

static void translate_subtree(wn_core *c, int node, float dx, float dy)
{
    int end = c->subtree_end[node];
    int i;
    for (i = node; i < end; ++i) {
        c->rects[i].x += dx;
        c->rects[i].y += dy;
    }
}

static float intrinsic_width(wn_core *c, int node, float available)
{
    const UiNode *n = &c->doc->nodes[node];
    if (n->width >= 0.0f) {
        return n->width;
    }
    if (n->width_pct > 0.0f) {
        return available * n->width_pct * 0.01f;
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
    float content = 0.0f;
    if (n->kind == R2_NODE_FLEX && n->direction == 0) {
        float sum = 0.0f;
        int count = 0;
        int child;
        for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
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
            float w = intrinsic_width(c, child, available);
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
    const UiNode *p = &c->doc->nodes[parent];
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
        const UiNode *n = &c->doc->nodes[id];
        float forced = -1.0f;
        if (n->width < 0.0f && n->width_pct == 0.0f) {
            float margins = n->margin[1] + n->margin[3];
            float limit = fmaxf(content_w - margins, 0.0f);
            if (row) {
                forced = fminf(limit, intrinsic_width(c, id, content_w));
            } else if (p->align != ALIGN_STRETCH) {
                forced = fminf(limit, intrinsic_width(c, id, content_w));
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
        float start_off, spacing;
        justify_offsets(p->justify, free, take, gap, &start_off, &spacing);
        float cursor = start_off;
        int j;
        for (j = start; j < start + take; ++j) {
            int id = items[j];
            const UiNode *n = &c->doc->nodes[id];
            float w = item_w[j];
            float h = item_h[j];
            float cross_extent = line_cross[l];
            if (row) {
                if (p->align == ALIGN_STRETCH && n->height < 0.0f && n->height_pct == 0.0f) {
                    h = cross_extent;
                }
                float off = align_offset(p->align, cross_extent, h);
                c->rects[id].w = w;
                c->rects[id].h = h;
                translate_subtree(c, id, content_x + cursor, content_y + cross_origin + off);
                cursor += w + spacing;
            } else {
                if (p->align == ALIGN_STRETCH && n->width < 0.0f && n->width_pct == 0.0f) {
                    w = cross_extent;
                }
                float off = align_offset(p->align, cross_extent, w);
                c->rects[id].w = w;
                c->rects[id].h = h;
                translate_subtree(c, id, content_x + cross_origin + off, content_y + cursor);
                cursor += h + spacing;
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
    const UiNode *p = &c->doc->nodes[parent];
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
        ++count;
    }
    if (count == 0) {
        return 0.0f;
    }
    if (p->grid_mode != R2_GRID_AUTO_FILL || p->grid_column_px <= 0.0f) {
        /* Not a v2 grid: behave like a block container. */
        float cursor = content_y;
        for (child = c->first_child[parent]; child >= 0; child = c->next_sibling[child]) {
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
        items[k] = child;
        ++k;
    }

    /* Pass 1: measure each item at (0,0) with the track width forced. */
    for (k = 0; k < count; ++k) {
        const UiNode *n = &c->doc->nodes[items[k]];
        float forced = track;
        if (n->width >= 0.0f) {
            forced = n->width;
        } else if (n->width_pct > 0.0f) {
            forced = content_w * n->width_pct * 0.01f;
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
    const UiNode *n = &c->doc->nodes[node];
    float w;
    if (forced_w >= 0.0f) {
        w = forced_w;
    } else if (n->width >= 0.0f) {
        w = n->width;
    } else if (n->width_pct > 0.0f) {
        w = available_w * n->width_pct * 0.01f;
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
        content_h = fmaxf(n->height - pad_t - pad_b, 0.0f);
        definite_h = 1;
    } else if (n->height_pct > 0.0f && parent_content_h >= 0.0f) {
        content_h = fmaxf(parent_content_h * n->height_pct * 0.01f - pad_t - pad_b, 0.0f);
        definite_h = 1;
    }

    const float content_x = x + pad_l;
    const float content_y = y + pad_t;

    if (n->kind == R2_NODE_TEXT) {
        float h = n->height >= 0.0f ? n->height : (content_h >= 0.0f ? content_h : 0.0f);
        c->rects[node].x = x;
        c->rects[node].y = y;
        c->rects[node].w = w;
        c->rects[node].h = h;
        return h + n->margin[2];
    }
    if (n->kind == R2_NODE_IMAGE) {
        float h;
        if (n->height >= 0.0f) {
            h = n->height;
        } else if (n->height_pct > 0.0f && parent_content_h >= 0.0f) {
            h = parent_content_h * n->height_pct * 0.01f;
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

    float cursor = content_y;
    if (n->kind == R2_NODE_FLEX) {
        cursor += layout_flex_children(c, node, content_x, content_y, content_w, content_h);
    } else if (n->kind == R2_NODE_GRID) {
        cursor += layout_grid_children(c, node, content_x, content_y, content_w, content_h);
    } else {
        int child;
        for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
            cursor += layout_node(c, child, content_x, cursor, content_w, content_h, 0, -1.0f);
        }
    }
    const float content_used = cursor - content_y;

    float h;
    if (n->height >= 0.0f) {
        h = n->height;
    } else if (n->height_pct > 0.0f && parent_content_h >= 0.0f) {
        h = parent_content_h * n->height_pct * 0.01f;
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
    const UiNode *n = &c->doc->nodes[node];
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
        compute_clips(c, child, child_clip);
    }
}

static void layout_page(wn_core *c, float viewport_w, float viewport_h)
{
    int root = 0;
    int i;
    wn_rect viewport;
    for (i = 0; i < c->doc->node_count; ++i) {
        if (c->doc->nodes[i].parent < 0) {
            root = i;
            break;
        }
    }
    layout_node(c, root, 0.0f, 0.0f, viewport_w, viewport_h, 1, -1.0f);
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.w = viewport_w;
    viewport.h = viewport_h;
    compute_clips(c, root, viewport);
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
}

/* ------------------------------------------------------------- interaction */

static int point_in_rect(float px, float py, const wn_rect *r)
{
    return r->w > 0.0f && r->h > 0.0f &&
           px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

int wn_core_node_at(const wn_core *core, float px, float py)
{
    int i;
    if (core == NULL) {
        return -1;
    }
    for (i = core->doc->node_count - 1; i >= 0; --i) {
        if (point_in_rect(px, py, &core->rects[i]) &&
            point_in_rect(px, py, &core->clips[i])) {
            return i;
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
    for (i = core->doc->node_count - 1; i >= 0; --i) {
        if (core->doc->nodes[i].action != ACTION_NONE &&
            point_in_rect(px, py, &core->rects[i]) &&
            point_in_rect(px, py, &core->clips[i])) {
            return i;
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
        if (core->doc->nodes[node].overflow == R2_OVERFLOW_SCROLL &&
            core->scroll_max[node] > 0.0f) {
            container = node;
            break;
        }
        node = core->doc->nodes[node].parent;
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
    if (core == NULL || node < 0 || node >= core->doc->node_count) {
        return -1.0f;
    }
    return core->scroll[node];
}

float wn_core_scroll_max(const wn_core *core, int node)
{
    if (core == NULL || node < 0 || node >= core->doc->node_count) {
        return -1.0f;
    }
    return core->scroll_max[node];
}

int wn_core_node_clip(const wn_core *core, int node, wn_rect *out)
{
    if (core == NULL || out == NULL || node < 0 || node >= core->doc->node_count) {
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
        core->doc->nodes[core->press].action == ACTION_SELECT) {
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
    const UiNode *n = &c->doc->nodes[node];
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
    if (core == NULL || node < 0 || node >= core->doc->node_count) {
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
}

/* ---------------------------------------------------------------- classes */

int wn_core_set_class(wn_core *core, int node, const char *class_name, int enabled)
{
    int i;
    int slot = -1;
    int count;
    if (core == NULL || class_name == NULL || class_name[0] == '\0' ||
        node < 0 || node >= core->doc->node_count) {
        return -1;
    }
    if (strcmp(class_name, "hover") == 0) {
        if (enabled) {
            core->hover = node;
        } else if (core->hover == node) {
            core->hover = -1;
        }
        return 0;
    }
    if (strcmp(class_name, "press") == 0) {
        if (enabled) {
            core->press = node;
        } else if (core->press == node) {
            core->press = -1;
        }
        return 0;
    }
    if (strcmp(class_name, "selected") == 0) {
        if (enabled) {
            core->selected = node;
        } else if (core->selected == node) {
            core->selected = -1;
        }
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
        if (run->node < 0 || run->node >= c->doc->node_count || run->count <= 0) {
            continue;
        }
        node = &c->doc->nodes[run->node];
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
        if (c->glyph_sets[i].font_px == (int)(c->doc->nodes[node].font_size + 0.5f)) {
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
    if (core == NULL || node < 0 || node >= core->doc->node_count ||
        core->doc->nodes[node].kind != R2_NODE_TEXT) {
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
    if (core == NULL || node < 0 || node >= core->doc->node_count ||
        core->doc->nodes[node].kind != R2_NODE_TEXT || text == NULL) {
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

/* object-fit fill/cover/contain -> destination quad + source UVs. */
static void push_image_quad(wn_core *c, int node, const wn_rect *clip)
{
    const UiNode *n = &c->doc->nodes[node];
    const wn_rect *r = &c->rects[node];
    wn_image_batch *batch;
    float x = r->x + n->padding[3];
    float y = r->y + n->padding[0];
    float w = fmaxf(r->w - n->padding[1] - n->padding[3], 0.0f);
    float h = fmaxf(r->h - n->padding[0] - n->padding[2], 0.0f);
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    float iw = n->image_width;
    float ih = n->image_height;
    float x1, y1;

    if (n->image_src == NULL || w <= 0.0f || h <= 0.0f) {
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

    ensure_image_batch_capacity(c, c->image_batch_count + 1);
    if (c->image_batch_count >= c->image_batch_capacity) {
        return;
    }
    if (c->image_batch_count > 0) {
        wn_image_batch *last = &c->image_batches[c->image_batch_count - 1];
        if (last->src != NULL && strcmp(last->src, n->image_src) == 0 &&
            last->clip_x == clip->x && last->clip_y == clip->y &&
            last->clip_w == clip->w && last->clip_h == clip->h) {
            last->count += 6;
            batch = last;
            goto push_vertices;
        }
    }
    batch = &c->image_batches[c->image_batch_count++];
    batch->first = c->image_vertex_count;
    batch->count = 6;
    batch->clip_x = clip->x;
    batch->clip_y = clip->y;
    batch->clip_w = clip->w;
    batch->clip_h = clip->h;
    batch->src = n->image_src;

push_vertices:
    (void)batch;
    x1 = x + w;
    y1 = y + h;
    push_image_vertex(c, x, y, u0, v0, (float)c->width, (float)c->height);
    push_image_vertex(c, x1, y, u1, v0, (float)c->width, (float)c->height);
    push_image_vertex(c, x1, y1, u1, v1, (float)c->width, (float)c->height);
    push_image_vertex(c, x, y, u0, v0, (float)c->width, (float)c->height);
    push_image_vertex(c, x1, y1, u1, v1, (float)c->width, (float)c->height);
    push_image_vertex(c, x, y1, u0, v1, (float)c->width, (float)c->height);
}

static void build_text_geometry(wn_core *c, float viewport_w, float viewport_h)
{
    int run_index;
    for (run_index = 0; run_index < c->doc->run_count; ++run_index) {
        const UiTextRun *run = &c->doc->runs[run_index];
        const wn_rect *r;
        unsigned int color;
        uint8_t a, cr, cg, cb;
        int j;
        if (run->node < 0 || run->node >= c->doc->node_count || run->count <= 0) {
            continue;
        }
        r = &c->rects[run->node];
        color = c->doc->nodes[run->node].text_color;
        a = (uint8_t)((color >> 24) & 0xFFu);
        if (a == 0) {
            continue;
        }
        if (rect_is_empty(c->clips[run->node])) {
            continue;
        }
        set_glyph_clip(c, &c->clips[run->node]);
        cr = (uint8_t)((color >> 16) & 0xFFu);
        cg = (uint8_t)((color >> 8) & 0xFFu);
        cb = (uint8_t)(color & 0xFFu);

        if (c->text_override[run->node] != NULL && c->text_override[run->node][0] != '\0') {
            int font_px = (int)(c->doc->nodes[run->node].font_size + 0.5f);
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
            for (const unsigned char *p = (const unsigned char *)c->text_override[run->node];
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
                float x = floorf(r->x + pen + entry->off_x + 0.5f);
                float y = floorf(r->y + entry->off_y + 0.5f);
                push_glyph_quad(c, x, y, entry->w, entry->h,
                                entry->u0, entry->v0, entry->u1, entry->v1,
                                viewport_w, viewport_h, cr, cg, cb, a);
                pen += entry->advance;
            }
            continue;
        }

        for (j = 0; j < run->count; ++j) {
            const UiGlyph *g = &c->doc->glyphs[run->first + j];
            float x, y;
            if (g->w <= 0.0f || g->h <= 0.0f) {
                continue;
            }
            x = floorf(r->x + g->x + 0.5f);
            y = floorf(r->y + g->y + 0.5f);
            push_glyph_quad(c, x, y, g->w, g->h, g->u0, g->v0, g->u1, g->v1,
                            viewport_w, viewport_h, cr, cg, cb, a);
        }
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
    core->vertex_count = 0;
    core->solid_batch_count = 0;
    core->glyph_vertex_count = 0;
    core->glyph_batch_count = 0;
    core->image_vertex_count = 0;
    core->image_batch_count = 0;

    for (i = 0; i < core->doc->node_count; ++i) {
        unsigned int color = node_color(core, i);
        const wn_rect *r;
        if (color == 0) {
            continue;
        }
        if (rect_is_empty(core->clips[i])) {
            continue;
        }
        r = &core->rects[i];
        set_solid_clip(core, &core->clips[i]);
        push_quad(core, r->x, r->y, r->w, r->h,
                  (float)core->width, (float)core->height, color);
    }
    for (i = 0; i < core->doc->node_count; ++i) {
        if (core->doc->nodes[i].kind == R2_NODE_IMAGE &&
            !rect_is_empty(core->clips[i])) {
            push_image_quad(core, i, &core->clips[i]);
        }
    }
    build_text_geometry(core, (float)core->width, (float)core->height);

    finalize_draw_batches(core->solid_batches, &core->solid_batch_count,
                          core->vertex_count);
    finalize_draw_batches(core->glyph_batches, &core->glyph_batch_count,
                          core->glyph_vertex_count);

    for (i = 0; i < core->doc->node_count; ++i) {
        if (core->doc->nodes[i].parent < 0) {
            root = i;
            break;
        }
    }
    if (root >= 0) {
        unsigned int bg = core->doc->nodes[root].background;
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
        *count = core->doc->node_count;
    }
    return core->rects;
}

double wn_core_last_layout_ms(const wn_core *core)
{
    return core != NULL ? core->last_layout_ms : 0.0;
}

/* -------------------------------------------------------------- lifecycle */

wn_core *wn_core_create(const wn_document *document)
{
    wn_core *c;
    int count;
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
    c->hover = c->press = c->selected = -1;
    c->width = document->viewport_width;
    c->height = document->viewport_height;

    bytes = (size_t)count;
    c->rects = (wn_rect *)calloc(bytes, sizeof(wn_rect));
    c->clips = (wn_rect *)calloc(bytes, sizeof(wn_rect));
    c->scroll = (float *)calloc(bytes, sizeof(float));
    c->scroll_max = (float *)calloc(bytes, sizeof(float));
    c->first_child = (int *)calloc(bytes, sizeof(int));
    c->next_sibling = (int *)calloc(bytes, sizeof(int));
    c->subtree_end = (int *)calloc(bytes, sizeof(int));
    c->last_child = (int *)calloc(bytes, sizeof(int));
    c->text_override = (char **)calloc(bytes, sizeof(char *));
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

    if (c->rects == NULL || c->clips == NULL || c->scroll == NULL ||
        c->scroll_max == NULL || c->first_child == NULL || c->next_sibling == NULL ||
        c->subtree_end == NULL || c->last_child == NULL || c->text_override == NULL ||
        c->class_count == NULL || c->classes == NULL ||
        c->glyph_sets == NULL || c->vertices == NULL || c->solid_batches == NULL ||
        c->glyph_vertices == NULL || c->glyph_batches == NULL ||
        c->image_vertices == NULL || c->image_batches == NULL) {
        wn_core_destroy(c);
        return NULL;
    }

    build_hierarchy(c);
    build_glyph_sets(c);
    layout_page(c, (float)c->width, (float)c->height);
    wn_core_build_display_list(c);
    return c;
}

void wn_core_destroy(wn_core *core)
{
    int i;
    if (core == NULL) {
        return;
    }
    if (core->text_override != NULL) {
        for (i = 0; i < core->doc->node_count; ++i) {
            wn_free_(core->text_override[i]);
        }
    }
    if (core->classes != NULL && core->class_count != NULL) {
        for (i = 0; i < core->doc->node_count * WN_MAX_CLASSES; ++i) {
            wn_free_(core->classes[i]);
        }
    }
    wn_free_(core->rects);
    wn_free_(core->clips);
    wn_free_(core->scroll);
    wn_free_(core->scroll_max);
    wn_free_(core->first_child);
    wn_free_(core->next_sibling);
    wn_free_(core->subtree_end);
    wn_free_(core->last_child);
    wn_free_(core->text_override);
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
