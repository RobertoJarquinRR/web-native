/*
 * layout.c - hierarchy, intrinsic sizes, block/flex/grid layout and clipping.
 *
 * Keeps the documented R2 behaviour: same block/flex algorithm, same wrap
 * shrink rule, same margin:auto handling.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wn_core_internal.h"
#include "wn_internal.h"

/* Max-content width. Flex items use their content width, not the container
 * width (documented R2 behaviour). */
static float intrinsic_width(wn_core *c, int node, float available);
static float layout_node(wn_core *c, int node, float origin_x, float origin_y,
                         float available_w, float parent_content_h,
                         int is_root, float forced_w);

/* ------------------------------------------------------------- hierarchy */

/* Rebuild first_child/next_sibling/last_child from the parent links. Cheap
 * (O(nodes)) and called after every structural mutation. */
void build_hierarchy(wn_core *c)
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

static void measure_text_node(const UiNode *n, float max_width, wn_text_metrics *out)
{
    const char *text = n->text != NULL ? n->text : "";
    if (wn_text_measure_string(text, n->font_family, n->font_size, n->font_weight,
                               max_width > 0.0f ? max_width : 0.0f, out) == 0) {
        return;
    }
    out->width = n->font_size * 0.6f * (float)strlen(text);
    out->height = n->font_size * 1.25f;
    out->ascent = n->font_size;
    out->descent = 0.0f;
}

static float text_node_width(const UiNode *n, float max_width)
{
    wn_text_metrics metrics;
    measure_text_node(n, max_width, &metrics);
    return ceilf(metrics.width) + WN_DECL_W(n);
}

static float text_node_height(const UiNode *n, float max_width)
{
    wn_text_metrics metrics;
    if (WN_LINE_HEIGHT(n) > 0.0f) {
        return ceilf(WN_LINE_HEIGHT(n)) + WN_DECL_H(n);
    }
    measure_text_node(n, max_width, &metrics);
    return ceilf(metrics.height) + WN_DECL_H(n);
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
        return text_node_width(n, available);
    }
    if (n->kind == R2_NODE_IMAGE && n->image_width > 0.0f) {
        return n->image_width + n->padding[1] + n->padding[3];
    }
    if (n->kind == R2_NODE_GRID && n->grid_mode == R2_GRID_AUTO_FILL &&
        n->grid_column_px > 0.0f) {
        return n->grid_column_px + n->padding[1] + n->padding[3];
    }
    if (n->kind == R2_NODE_INPUT) {
        /* Default width of a field without CSS: roughly 20 characters,
         * bounded by max_length when the field declares one. */
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
        /* Main-axis auto margins absorb the free space before
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
    } else if (n->kind == R2_NODE_TEXT) {
        w = text_node_width(n, available_w - n->margin[1] - n->margin[3]);
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
        float h;
        if (n->height >= 0.0f) {
            h = n->height + WN_DECL_H(n);
        } else if (content_h >= 0.0f) {
            h = content_h;
        } else {
            h = text_node_height(n, content_w);
        }
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
        /* Single-line editor: a leaf with no children. Without CSS the height
         * is the font line (ascender + descender) plus the padding. */
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
            /* `margin-left/right:auto` in a block container. Both auto
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

wn_rect rect_intersection(wn_rect a, wn_rect b)
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

int rect_is_empty(wn_rect r)
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

void layout_page(wn_core *c, float viewport_w, float viewport_h)
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
