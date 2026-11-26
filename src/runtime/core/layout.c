/*
 * layout.c - hierarchy, block/flex/grid layout and clipping.
 *
 * Keeps the documented R2 behaviour: same block/flex algorithm, same wrap
 * shrink rule, same margin:auto handling. Kind-specific size and children
 * rules live in the capability table (core/kind_ops.c); this file resolves the
 * declared sizes, dispatches through the table and applies scroll/clip.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "kind_ops.h"
#include "wn_core_internal.h"
#include "wn_internal.h"

static float layout_node(wn_core *c, int node, float origin_x, float origin_y,
                         float available_w, float parent_content_h,
                         int is_root, float forced_w, float forced_h);

/* ------------------------------------------------------------- hierarchy */

/* Full rebuild of first_child/next_sibling/prev_sibling/last_child from the
 * parent links. Only wn_core_create() calls it: mutations link/unlink the lists
 * incrementally, which keeps a clone/remove O(1) in the hierarchy instead of
 * rebuilding O(n) per node (O(n^3) for a list). */
void build_hierarchy(wn_core *c)
{
    int i;
    c->hierarchy_rebuilds += 1;
    for (i = 0; i < c->node_count; ++i) {
        c->first_child[i] = -1;
        c->next_sibling[i] = -1;
        c->prev_sibling[i] = -1;
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
            c->prev_sibling[i] = -1;
            if (c->first_child[parent] >= 0) {
                c->prev_sibling[c->first_child[parent]] = i;
            }
            c->first_child[parent] = i;
        } else {
            c->next_sibling[i] = c->next_sibling[prev];
            c->prev_sibling[i] = prev;
            if (c->next_sibling[i] >= 0) {
                c->prev_sibling[c->next_sibling[i]] = i;
            }
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

/* Resolves one declared inset side against a basis (px stays px, a
 * percentage is applied to the containing block). Only called when the side is
 * marked as set in `inset_mask`. */
static float inset_value(const UiNode *n, int side, float basis)
{
    if (!WN_INSET_SET(n, side)) {
        return 0.0f;
    }
    if (WN_INSET_PCT(n, side)) {
        return basis * n->inset[side] * 0.01f;
    }
    return n->inset[side];
}

/* `position: relative`: the node was laid out in normal flow; shift it and its
 * subtree afterwards. Left/top win over right/bottom when both are present.
 * The flow cursor is untouched, so siblings stay where they were. */
static void apply_relative_offset(wn_core *c, int node, float available_w,
                                  float parent_content_h)
{
    const UiNode *n = &c->nodes[node];
    float dx = 0.0f;
    float dy = 0.0f;
    float height_basis;
    if (n->position != R2_POSITION_RELATIVE) {
        return;
    }
    if (WN_INSET_SET(n, 3)) {
        dx = inset_value(n, 3, available_w);
    } else if (WN_INSET_SET(n, 1)) {
        dx = -inset_value(n, 1, available_w);
    }
    /* A percentage top/bottom needs a definite containing block height; with
     * an auto-height flow containing block the offset resolves to 0. */
    height_basis = parent_content_h >= 0.0f ? parent_content_h : 0.0f;
    if (WN_INSET_SET(n, 0)) {
        dy = inset_value(n, 0, height_basis);
    } else if (WN_INSET_SET(n, 2)) {
        dy = -inset_value(n, 2, height_basis);
    }
    if (dx != 0.0f || dy != 0.0f) {
        translate_subtree(c, node, dx, dy);
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
        if (!c->alive[child] || !c->visible[child] ||
            WN_IS_ABSOLUTE(&c->nodes[child])) {
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
        if (!c->alive[child] || !c->visible[child] ||
            WN_IS_ABSOLUTE(&c->nodes[child])) {
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
                forced = fminf(limit, wn_intrinsic_width(c, id, content_w));
            } else if (p->align != ALIGN_STRETCH) {
                forced = fminf(limit, wn_intrinsic_width(c, id, content_w));
            }
        }
        if (basis >= 0.0f) {
            if (row) {
                forced = basis + WN_DECL_W(n);
            } else {
                float saved = c->nodes[id].height;
                c->nodes[id].height = basis - WN_DECL_H(n);
                layout_node(c, id, 0.0f, 0.0f, content_w, content_h, 0, forced, -1.0f);
                c->nodes[id].height = saved;
                item_w[k] = c->rects[id].w;
                item_h[k] = c->rects[id].h;
                continue;
            }
        }
        layout_node(c, id, 0.0f, 0.0f, content_w, content_h, 0, forced, -1.0f);
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
                    layout_node(c, id, 0.0f, 0.0f, content_w, content_h, 0, item_w[j], -1.0f);
                    item_h[j] = c->rects[id].h;
                } else {
                    float outer = item_h[j] + extra;
                    float saved = c->nodes[id].height;
                    c->nodes[id].height = outer - WN_DECL_H(n);
                    layout_node(c, id, 0.0f, 0.0f, content_w, content_h, 0, -1.0f, -1.0f);
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
        if (!c->alive[child] || !c->visible[child] ||
            WN_IS_ABSOLUTE(&c->nodes[child])) {
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
            if (WN_IS_ABSOLUTE(&c->nodes[child])) {
                continue;
            }
            cursor += layout_node(c, child, content_x, cursor, content_w, content_h, 0, -1.0f, -1.0f);
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
        if (!c->alive[child] || !c->visible[child] ||
            WN_IS_ABSOLUTE(&c->nodes[child])) {
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
        layout_node(c, items[k], 0.0f, 0.0f, content_w, content_h, 0, forced, -1.0f);
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
                    layout_node(c, items[k], x, y, content_w, content_h, 0, item_w[k], -1.0f);
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

/* Children algorithms behind the capability table. They return the y cursor
 * after the last child, so layout_node derives `content_used` with the same
 * float operations the old inline chains used. */

float wn_layout_block_children(wn_core *c, int node, float content_x, float content_y,
                               float content_w, float content_h)
{
    float cursor = content_y;
    int child;
    for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
        float used;
        if (!c->alive[child] || !c->visible[child]) {
            mark_subtree_hidden(c, child);
            continue;
        }
        /* Absolute children leave the flow: no cursor advance, no size
         * contribution. The absolute pass lays them out once the containing
         * block is final. */
        if (WN_IS_ABSOLUTE(&c->nodes[child])) {
            continue;
        }
        used = layout_node(c, child, content_x, cursor, content_w, content_h, 0, -1.0f, -1.0f);
        /* `margin-left/right:auto` in a block container. Both auto margins
         * centre the box; only left pushes it to the right. */
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
    return cursor;
}

float wn_layout_flex_children(wn_core *c, int node, float content_x, float content_y,
                              float content_w, float content_h)
{
    return content_y + layout_flex_children(c, node, content_x, content_y, content_w, content_h);
}

float wn_layout_grid_children(wn_core *c, int node, float content_x, float content_y,
                              float content_w, float content_h)
{
    return content_y + layout_grid_children(c, node, content_x, content_y, content_w, content_h);
}

static float layout_node(wn_core *c, int node, float origin_x, float origin_y,
                         float available_w, float parent_content_h,
                         int is_root, float forced_w, float forced_h)
{
    const UiNode *n = &c->nodes[node];
    const WnKindOps *ops;
    float w;
    if (!c->alive[node] || !c->visible[node]) {
        mark_subtree_hidden(c, node);
        return 0.0f;
    }
    c->visible_eff[node] = 1;

    /* Horizontal scroll is reserved: the layout pass does not compute
     * horizontal overflow yet, so the bound stays 0 for every node (leaves
     * included) and the x half of the scroll service is a no-op. */
    c->scroll_max_x[node] = 0.0f;

    /* The table only owns the auto cases: forced/declared/percentage widths are
     * the same rule for every kind and stay here. */
    ops = wn_kind_ops(n->kind);
    if (ops == NULL) {
        /* A corrupt kind keeps the historical block behaviour. */
        ops = wn_kind_ops(R2_NODE_BLOCK);
    }
    if (forced_w >= 0.0f) {
        w = forced_w;   /* already a border-box width (grid/flex added padding) */
    } else if (n->width >= 0.0f) {
        w = n->width + WN_DECL_W(n);
    } else if (n->width_pct > 0.0f) {
        w = available_w * n->width_pct * 0.01f + WN_DECL_W(n);
    } else if (ops->auto_width != NULL) {
        w = ops->auto_width(c, node, available_w, parent_content_h);
    } else {
        /* No kind-specific rule: fill the parent minus the margins. */
        w = fmaxf(available_w - n->margin[1] - n->margin[3], 0.0f);
    }

    const float pad_l = n->padding[3], pad_r = n->padding[1];
    const float pad_t = n->padding[0], pad_b = n->padding[2];
    float content_h = -1.0f;
    int definite_h = 0;
    if (forced_h >= 0.0f) {
        /* Absolute top+bottom stretch: the forced value is border-box. */
        content_h = fmaxf(forced_h - pad_t - pad_b, 0.0f);
        definite_h = 1;
    } else if (n->height >= 0.0f) {
        content_h = fmaxf(n->height - (WN_BOX_SIZING(n) ? pad_t + pad_b : 0.0f), 0.0f);
        definite_h = 1;
    } else if (n->height_pct > 0.0f && parent_content_h >= 0.0f) {
        content_h = fmaxf(parent_content_h * n->height_pct * 0.01f -
                              (WN_BOX_SIZING(n) ? pad_t + pad_b : 0.0f), 0.0f);
        definite_h = 1;
    }

    /* Leaves never run children: the kind entry owns their height and the
     * early return keeps the historical "no children" contract. Their scroll
     * state needs no write either: only the container path below (and
     * scroll.c, gated on the axis max) ever changes it, so a leaf's
     * scroll offsets and vertical max stay at the zero the arena is
     * initialised with. */
    /* One declared-height rule for every leaf, TEXT included: the percentage is
     * the content box, so WN_DECL_H adds the padding back for content-box and
     * is zero for border-box. IMAGE/INPUT always resolved this way; TEXT used
     * to stop at the clipped content_h. The HTML/CSS emitter never emits a
     * percentage height on TEXT today, but .wnui is public, so the loader can
     * (locked by tests/core/text_pct_height_test.c). */
    if (ops->is_leaf) {
        float h;
        if (forced_h >= 0.0f) {
            h = forced_h;
        } else if (n->height >= 0.0f) {
            h = n->height + WN_DECL_H(n);
        } else if (n->height_pct > 0.0f && parent_content_h >= 0.0f) {
            h = parent_content_h * n->height_pct * 0.01f + WN_DECL_H(n);
        } else {
            h = ops->auto_height(c, node, w, content_h);
        }
        c->rects[node].x = origin_x + n->margin[3];
        c->rects[node].y = origin_y + n->margin[0];
        if (is_root) {
            c->rects[node].x = 0.0f;
            c->rects[node].y = 0.0f;
        }
        c->rects[node].w = w;
        c->rects[node].h = h;
        apply_relative_offset(c, node, available_w, parent_content_h);
        return h + n->margin[2];
    }

    float x = origin_x + n->margin[3];
    float y = origin_y + n->margin[0];
    if (is_root) {
        x = 0.0f;
        y = 0.0f;
    }
    const float content_w = fmaxf(w - pad_l - pad_r, 0.0f);
    const float content_x = x + pad_l;
    const float content_y = y + pad_t;

    const float cursor = ops->layout_children(c, node, content_x, content_y,
                                              content_w, content_h);
    const float content_used = cursor - content_y;

    float h;
    if (forced_h >= 0.0f) {
        h = forced_h;
    } else if (n->height >= 0.0f) {
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
        c->scroll_max_y[node] = max_scroll;
        if (c->scroll_y[node] < 0.0f) {
            c->scroll_y[node] = 0.0f;
        }
        if (c->scroll_y[node] > max_scroll) {
            c->scroll_y[node] = max_scroll;
        }
        if (c->scroll_y[node] > 0.0f) {
            int child;
            for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
                translate_subtree(c, child, 0.0f, -c->scroll_y[node]);
            }
        }
    } else {
        c->scroll_max_y[node] = 0.0f;
        if (c->scroll_y[node] != 0.0f) {
            c->scroll_y[node] = 0.0f;
        }
    }
    apply_relative_offset(c, node, available_w, parent_content_h);
    return h + n->margin[2];
}

/* ------------------------------------------------- absolute positioning */

/* Containing block for an absolute node. `padding` is the reference rect used
 * by the insets: the border box of the nearest positioned ancestor. Layout
 * ignores borders, so that rect is also the padding box CSS specifies.
 * `content_x/y` are its content-box origin, used by the simplified all-auto
 * rule. The viewport is the initial containing block when no ancestor is
 * positioned (`node == -1`), not the root node's box. */
typedef struct {
    int node;
    wn_rect padding;
    float content_x;
    float content_y;
} WnContainingBlock;

static WnContainingBlock containing_block_from_node(wn_core *c, int node)
{
    const UiNode *n = &c->nodes[node];
    const wn_rect *r = &c->rects[node];
    WnContainingBlock cb;
    cb.node = node;
    cb.padding = *r;
    cb.content_x = r->x + n->padding[3];
    cb.content_y = r->y + n->padding[0];
    return cb;
}

/* The initial containing block is the viewport, even when the root node is
 * smaller than it. `node == -1` makes absolute_scroll_offset apply every
 * ancestor scroller (there is no containing-block node to stop at). */
static WnContainingBlock containing_block_viewport(float width, float height)
{
    WnContainingBlock cb;
    cb.node = -1;
    cb.padding.x = 0.0f;
    cb.padding.y = 0.0f;
    cb.padding.w = width;
    cb.padding.h = height;
    cb.content_x = 0.0f;
    cb.content_y = 0.0f;
    return cb;
}

/* Absolute width auto is shrink-to-fit in this lane: intrinsic max-content,
 * clamped to the containing block width. */
static float absolute_auto_width(wn_core *c, int node, float cb_w)
{
    float width = wn_intrinsic_width(c, node, cb_w);
    if (!(width > 0.0f)) {
        width = 0.0f;
    }
    return fminf(width, cb_w);
}

/* Scroll offsets accumulated from the absolute node's parent up to (and
 * including) its containing block node. Scrollers above the containing block
 * already moved the containing block rect and must not be applied twice.
 * `cb_node` is -1 for the viewport fallback, where every ancestor scroller
 * applies. */
static void absolute_scroll_offset(const wn_core *c, int node, int cb_node,
                                   float *dx, float *dy)
{
    int walk;
    *dx = 0.0f;
    *dy = 0.0f;
    for (walk = c->parent[node]; walk >= 0; walk = c->parent[walk]) {
        if (c->nodes[walk].overflow == R2_OVERFLOW_SCROLL) {
            *dx -= c->scroll_x[walk];
            *dy -= c->scroll_y[walk];
        }
        if (walk == cb_node) {
            break;
        }
    }
}

static void layout_absolute_node(wn_core *c, int node, const WnContainingBlock *cb)
{
    const UiNode *n = &c->nodes[node];
    const float cb_w = cb->padding.w;
    const float cb_h = cb->padding.h;
    const int has_left = WN_INSET_SET(n, 3);
    const int has_right = WN_INSET_SET(n, 1);
    const int has_top = WN_INSET_SET(n, 0);
    const int has_bottom = WN_INSET_SET(n, 2);
    const float left = inset_value(n, 3, cb_w);
    const float right = inset_value(n, 1, cb_w);
    const float top = inset_value(n, 0, cb_h);
    const float bottom = inset_value(n, 2, cb_h);
    const int width_auto = n->width < 0.0f && n->width_pct == 0.0f;
    const int height_auto = n->height < 0.0f && n->height_pct == 0.0f;
    float w_outer;
    float forced_h = -1.0f;
    float bx;
    float by;
    float scroll_dx;
    float scroll_dy;
    int position_by_bottom = 0;

    if (width_auto && has_left && has_right) {
        w_outer = fmaxf(cb_w - left - right - n->margin[1] - n->margin[3], 0.0f);
    } else if (width_auto) {
        w_outer = absolute_auto_width(c, node, cb_w);
    } else if (n->width >= 0.0f) {
        w_outer = n->width + WN_DECL_W(n);
    } else {
        w_outer = cb_w * n->width_pct * 0.01f + WN_DECL_W(n);
    }

    if (height_auto && has_top && has_bottom) {
        forced_h = fmaxf(cb_h - top - bottom - n->margin[0] - n->margin[2], 0.0f);
    }

    /* Insets place the margin edge; margins move the border box inside it. */
    if (has_left) {
        bx = cb->padding.x + left + n->margin[3];
    } else if (has_right) {
        bx = cb->padding.x + cb_w - right - n->margin[1] - w_outer;
    } else {
        bx = cb->content_x + n->margin[3];
    }

    if (has_top) {
        by = cb->padding.y + top + n->margin[0];
    } else {
        /* Auto top: content origin when both are auto; a bottom-only box needs
         * its final height before it can be placed, so it is translated after
         * layout. */
        by = cb->content_y + n->margin[0];
        position_by_bottom = !has_top && has_bottom;
    }

    absolute_scroll_offset(c, node, cb->node, &scroll_dx, &scroll_dy);

    layout_node(c, node, bx - n->margin[3], by - n->margin[0], cb_w, cb_h, 0,
                w_outer, forced_h);

    if (position_by_bottom) {
        float target_y = cb->padding.y + cb_h - bottom - n->margin[2] -
                         c->rects[node].h;
        translate_subtree(c, node, 0.0f, target_y - c->rects[node].y);
    }
    if (scroll_dx != 0.0f || scroll_dy != 0.0f) {
        translate_subtree(c, node, scroll_dx, scroll_dy);
    }
}

/* Recursive second pass: lay out absolute nodes in tree order once normal flow
 * (and relative offsets) has settled every containing block. Nested absolute
 * children use the absolute node itself as their containing block. */
static void layout_absolute_children(wn_core *c, int node,
                                     const WnContainingBlock *inherited)
{
    WnContainingBlock cb = *inherited;
    int child;

    if (WN_IS_POSITIONED(&c->nodes[node])) {
        cb = containing_block_from_node(c, node);
    }
    for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
        if (!c->alive[child] || !c->visible[child]) {
            continue;
        }
        if (WN_IS_ABSOLUTE(&c->nodes[child])) {
            layout_absolute_node(c, child, &cb);
        }
        layout_absolute_children(c, child, &cb);
    }
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
 * of an overflow container are additionally clipped to its content box.
 *
 * Absolute nodes use the clip of their containing block's children instead of
 * the DOM parent's clip: CSS does not clip an out-of-flow descendant by an
 * `overflow` ancestor that is not its containing block (or inside it). The
 * fallback root acts as a containing block too, so a static overflow ancestor
 * below the root does not clip an absolute node whose containing block is the
 * root. `cb_child_clip` is the child clip of the nearest positioned ancestor
 * (or the root), passed down through static ancestors. */
static void compute_clips(wn_core *c, int node, wn_rect inherited_clip,
                          wn_rect cb_child_clip)
{
    const UiNode *n = &c->nodes[node];
    wn_rect clip = WN_IS_ABSOLUTE(n) ? cb_child_clip : inherited_clip;
    wn_rect child_clip = clip;
    int child;
    const int is_containing_block = c->parent[node] < 0 || WN_IS_POSITIONED(n);

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
        compute_clips(c, child, child_clip,
                      is_containing_block ? child_clip : cb_child_clip);
    }
}

/* -------------------------------------------------------------- paint list */

static float paint_node_opacity(const wn_core *c, int node)
{
    float opacity = c->nodes[node].opacity;
    if (!(opacity > 0.0f)) {
        return 0.0f;
    }
    if (opacity > 1.0f) {
        return 1.0f;
    }
    return opacity;
}

/* Any node with `opacity < 1` is a CSS group: opacity creates a stacking
 * context, so the node's own item and its whole subtree render into an
 * offscreen layer and composite once. Leaves are groups too. A leaf can paint
 * several passes (border over background, shadow under fill, input ring/body/
 * selection/caret, text or an image over a background); fading each pass
 * separately is not the same as fading the flattened result. */
static int paint_group_root(const wn_core *c, int node)
{
    return c->nodes[node].opacity < 1.0f;
}

/* Blob v5: a positioned node with `z-index != auto` is a stacking context.
 * The root is a context too (handled by paint_context_root). z-index on a
 * static node is ignored: CSS only applies it to positioned boxes and to
 * flex/grid items, and this subset does not create flex/grid item contexts. */
static int paint_z_context(const wn_core *c, int node)
{
    const UiNode *n = &c->nodes[node];
    return c->parent[node] >= 0 && WN_IS_POSITIONED(n) &&
           n->z_index != R2_Z_INDEX_AUTO;
}

static int paint_context_root(const wn_core *c, int node)
{
    return c->parent[node] < 0 || paint_group_root(c, node) ||
           paint_z_context(c, node);
}

/* Stacking level inside the parent context. An opacity group that is not
 * positioned is a z-auto context, so it sorts as 0; a positioned context with
 * an explicit z uses that integer (negative included). */
static int paint_context_z(const wn_core *c, int node)
{
    const UiNode *n = &c->nodes[node];
    if (WN_IS_POSITIONED(n) && n->z_index != R2_Z_INDEX_AUTO) {
        return n->z_index;
    }
    return 0;
}

/* True when the page needs the ordered per-item path: any visible, non-empty
 * group or z-index context. Pages with neither keep the historical flat
 * category path byte for byte. */
static int paint_needs_ordering(const wn_core *c)
{
    int i;
    for (i = 0; i < c->node_count; ++i) {
        if (!c->alive[i] || !c->visible_eff[i] ||
            rect_is_empty(c->clips[i])) {
            continue;
        }
        if (paint_group_root(c, i) || paint_z_context(c, i)) {
            return 1;
        }
    }
    return 0;
}

static int paint_layers_reserve(wn_core *c, int needed)
{
    int capacity;
    wn_paint_layer *grown;
    if (needed <= c->paint_layer_capacity) {
        return 1;
    }
    capacity = c->paint_layer_capacity > 0 ? c->paint_layer_capacity : 8;
    while (capacity < needed) {
        capacity *= 2;
    }
    if (capacity > WN_MAX_PAINT_LAYERS) {
        capacity = WN_MAX_PAINT_LAYERS;
    }
    if (needed > capacity) {
        return 0;
    }
    grown = (wn_paint_layer *)realloc(c->paint_layers,
                                      (size_t)capacity * sizeof(wn_paint_layer));
    if (grown == NULL) {
        return 0;
    }
    c->paint_layers = grown;
    c->paint_layer_capacity = capacity;
    return 1;
}

static int paint_layer_add(wn_core *c, int parent, int node, float opacity,
                           wn_rect clip, int needs_texture)
{
    wn_paint_layer *layer;
    if (c->paint_layer_count >= WN_MAX_PAINT_LAYERS ||
        !paint_layers_reserve(c, c->paint_layer_count + 1)) {
        return -1;
    }
    layer = &c->paint_layers[c->paint_layer_count];
    layer->id = c->paint_layer_count;
    layer->parent = parent;
    layer->node = node;
    layer->opacity = opacity;
    layer->clip = clip;
    layer->needs_texture = needs_texture;
    return c->paint_layer_count++;
}

/* Allocates the root layer and reports whether the page uses groups. Called
 * after layout/clips and before the item walk. */
static int paint_layers_begin(wn_core *c, wn_rect viewport)
{
    wn_paint_layer *root;
    c->paint_layer_count = 0;
    c->paint_command_count = 0;
    if (!paint_needs_ordering(c) || !paint_layers_reserve(c, 1)) {
        return 0;
    }
    root = &c->paint_layers[0];
    root->id = 0;
    root->parent = -1;
    root->node = -1;
    root->opacity = 1.0f;
    root->clip = viewport;
    root->needs_texture = 0;
    c->paint_layer_count = 1;
    return 1;
}

static void append_paint_item(wn_core *c, int node, int layer, float opacity)
{
    WnPaintItem *item;
    if (c->paint_item_count >= c->paint_item_capacity) {
        return;
    }
    item = &c->paint_items[c->paint_item_count++];
    *item = (WnPaintItem){
        node, c->clips[node], layer, opacity, WN_TRANSFORM_IDENTITY_INIT
    };
}

/* Items are visible nodes with a non-empty inherited clip, in DFS order. The
 * clip is the intersection of the viewport with the ancestor content boxes
 * (compute_clips starts at the viewport), so it says nothing about the node's
 * own box: a node whose box is entirely outside the viewport still has a
 * non-empty clip through an overflow:visible ancestor.
 *
 * An empty clip on a node must not prune its subtree. Clips only shrink for
 * in-flow descendants, so every normal child of an empty-clip node is empty
 * too; but an absolutely positioned descendant can replace the inherited clip
 * with the child clip of its containing block (compute_clips), which lies
 * above the clipping node. CSS does not clip such a node by an `overflow`
 * ancestor that is not its containing block, so the walk keeps descending and
 * skips only the individual nodes whose own clip is empty. This is what makes
 * `root > relative > overflow:hidden(height:0) > static > absolute` paint and
 * hit-test at its placed rect. Invisible subtrees are still skipped whole,
 * exactly like the old paint_order build.
 *
 * wn_paint_item_visible() computes the second test (item AABB vs viewport),
 * but the build intentionally does not apply it. A box fully below the
 * viewport can still cast its box-shadow into the viewport, and shadows are
 * not part of the visibility test yet. Culling is computed and tested, never
 * applied; paint_list_test locks the offscreen-shadow-caster case.
 *
 * Cost note (measured on the paint-list refactor, Release/D3D11): canonical
 * page +0.4 % versus the old paint_order walk; a 1201-node synthetic page +7 %
 * layout and +10 % build, still inside the absolute budgets. The AoS stride of
 * 52 B per item (node, clip, layer, opacity, transform) is the tradeoff that
 * keeps one ordered array instead of parallel arrays. */
static void build_paint_items_dfs(wn_core *c, int node, int layer)
{
    int child;
    int own_layer = layer;
    int group = 0;
    if (!c->alive[node] || !c->visible_eff[node]) {
        return;
    }
    if (!rect_is_empty(c->clips[node])) {
        group = paint_group_root(c, node);
        if (group) {
            own_layer = paint_layer_add(c, layer, node, paint_node_opacity(c, node),
                                        c->clips[node], 1);
            if (own_layer < 0) {
                /* Beyond WN_MAX_PAINT_LAYERS: fall back to direct alpha on this
                 * node's own box (deeper groups still layer below the cap). */
                group = 0;
                own_layer = layer;
            }
        }
        append_paint_item(c, node, own_layer,
                          group ? 1.0f : paint_node_opacity(c, node));
    }
    for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
        build_paint_items_dfs(c, child, own_layer);
    }
}

/* -------------------------------------------------- stacking order (v5) */
/* CSS 2.1 Appendix E, simplified to the primitives this engine has (no floats
 * and no inline-level boxes): inside one stacking context paint
 *
 *   1. the context root's own item (background/border/content and, when the
 *      root is an opacity group, its offscreen group);
 *   2. child contexts with negative z, ascending z (tree order breaks ties);
 *   3. in-flow content in tree order. A positioned descendant whose z-index is
 *      auto is not a context, so it stays here at its tree position;
 *   4. child contexts with z 0/auto in tree order;
 *   5. child contexts with positive z, ascending z (tree order breaks ties).
 *
 * A child context's whole block is emitted as one unit, so its descendants can
 * never escape the parent context. Nested contexts compose recursively. */
typedef struct {
    int order;       /* pre-order index, the stable tie-breaker */
    int z;           /* effective stacking level inside the parent context */
    unsigned char is_context;
} WnPaintCtxInfo;

typedef struct {
    int z;
    int order;
    int node;
} WnPaintChild;

static int paint_child_compare(const void *a, const void *b)
{
    const WnPaintChild *left = (const WnPaintChild *)a;
    const WnPaintChild *right = (const WnPaintChild *)b;
    if (left->z != right->z) {
        return left->z < right->z ? -1 : 1;
    }
    if (left->order != right->order) {
        return left->order < right->order ? -1 : 1;
    }
    return 0;
}

static void paint_ctx_info_walk(wn_core *c, WnPaintCtxInfo *info, int node,
                                int *order)
{
    int child;
    int is_context = paint_context_root(c, node);
    info[node].order = (*order)++;
    info[node].is_context = (unsigned char)is_context;
    info[node].z = is_context ? paint_context_z(c, node) : 0;
    for (child = c->first_child[node]; child >= 0;
         child = c->next_sibling[child]) {
        if (!c->alive[child]) {
            continue;
        }
        paint_ctx_info_walk(c, info, child, order);
    }
}

/* Direct child contexts of `node`: walk its descendants in tree order and
 * stop at the first context root of each branch. */
static void paint_collect_child_contexts(wn_core *c, const WnPaintCtxInfo *info,
                                         int node, WnPaintChild *scratch,
                                         int *top)
{
    int child;
    for (child = c->first_child[node]; child >= 0;
         child = c->next_sibling[child]) {
        if (!c->alive[child] || !c->visible_eff[child]) {
            continue;
        }
        if (info[child].is_context) {
            scratch[*top].z = info[child].z;
            scratch[*top].order = info[child].order;
            scratch[*top].node = child;
            ++(*top);
        } else {
            paint_collect_child_contexts(c, info, child, scratch, top);
        }
    }
}

/* Step 3: every visible, non-context node in the context's subtree, in tree
 * order. Context roots are skipped here because they are emitted by their
 * stacking level (steps 2/4/5). */
static void paint_emit_in_flow(wn_core *c, const WnPaintCtxInfo *info, int node,
                               int layer)
{
    int child;
    for (child = c->first_child[node]; child >= 0;
         child = c->next_sibling[child]) {
        if (!c->alive[child] || !c->visible_eff[child] ||
            info[child].is_context) {
            continue;
        }
        if (!rect_is_empty(c->clips[child])) {
            append_paint_item(c, child, layer, paint_node_opacity(c, child));
        }
        paint_emit_in_flow(c, info, child, layer);
    }
}

/* Emits one stacking context: its own item and, in CSS order, its child
 * contexts and in-flow content. `is_root` keeps the root layer allocated by
 * paint_layers_begin(); every other context adds its own layer. */
static void paint_emit_context(wn_core *c, const WnPaintCtxInfo *info, int node,
                               int parent_layer, int is_root,
                               WnPaintChild *scratch, int *top)
{
    const UiNode *n = &c->nodes[node];
    int own_layer = parent_layer;
    int group = 0;
    int start;
    int count;
    int i;

    if (!is_root) {
        if (n->opacity < 1.0f) {
            int added = paint_layer_add(c, parent_layer, node,
                                        paint_node_opacity(c, node),
                                        c->clips[node], 1);
            if (added >= 0) {
                own_layer = added;
                group = 1;
            }
        } else {
            /* z-index-only context: ordering, no texture. */
            int added = paint_layer_add(c, parent_layer, node, 1.0f,
                                        c->clips[node], 0);
            if (added >= 0) {
                own_layer = added;
            }
        }
    } else if (n->opacity < 1.0f) {
        /* The root layer is the surface; an opaque-root is already layer 0.
         * Root opacity still needs a real offscreen group above it. */
        int added = paint_layer_add(c, parent_layer, node,
                                    paint_node_opacity(c, node),
                                    c->clips[node], 1);
        if (added >= 0) {
            own_layer = added;
            group = 1;
        }
    }

    /* Step 1: the context root's own box (and its group, if it is one). */
    if (!rect_is_empty(c->clips[node])) {
        append_paint_item(c, node, own_layer,
                          group ? 1.0f : paint_node_opacity(c, node));
    }

    start = *top;
    paint_collect_child_contexts(c, info, node, scratch, top);
    count = *top - start;
    qsort(&scratch[start], (size_t)count, sizeof(scratch[0]),
          paint_child_compare);

    /* Step 2: negative contexts, from most negative to least. */
    for (i = 0; i < count; ++i) {
        if (scratch[start + i].z < 0) {
            paint_emit_context(c, info, scratch[start + i].node, own_layer, 0,
                               scratch, top);
        }
    }

    /* Step 3: in-flow content of this context in tree order. */
    paint_emit_in_flow(c, info, node, own_layer);

    /* Step 4: z 0/auto contexts in tree order. */
    for (i = 0; i < count; ++i) {
        if (scratch[start + i].z == 0) {
            paint_emit_context(c, info, scratch[start + i].node, own_layer, 0,
                               scratch, top);
        }
    }

    /* Step 5: positive contexts, ascending. */
    for (i = 0; i < count; ++i) {
        if (scratch[start + i].z > 0) {
            paint_emit_context(c, info, scratch[start + i].node, own_layer, 0,
                               scratch, top);
        }
    }

    *top = start;
}

/* Ordered build used whenever the page has at least one group or z-index
 * context. `info` and `scratch` are caller-owned (layout_page) so a frame with
 * no context never pays for them. Returns 0 on allocation failure, in which
 * case layout_page falls back to the historical DFS list. */
static int build_paint_items_ordered(wn_core *c, int root, wn_rect viewport)
{
    WnPaintCtxInfo *info;
    WnPaintChild *scratch;
    int order = 0;
    int top = 0;
    info = (WnPaintCtxInfo *)calloc((size_t)c->node_count, sizeof(*info));
    scratch = (WnPaintChild *)calloc((size_t)c->node_count, sizeof(*scratch));
    if (info == NULL || scratch == NULL) {
        free(info);
        free(scratch);
        return 0;
    }
    (void)viewport;
    paint_ctx_info_walk(c, info, root, &order);
    paint_emit_context(c, info, root, 0, 1, scratch, &top);
    free(info);
    free(scratch);
    return 1;
}

/* True when `ancestor` is `layer` or one of its ancestors. */
static int paint_layer_ancestor(const wn_core *c, int ancestor, int layer)
{
    int walk = layer;
    int guard = 0;
    while (walk >= 0 && guard++ <= WN_MAX_PAINT_LAYER_DEPTH) {
        if (walk == ancestor) {
            return 1;
        }
        walk = c->paint_layers[walk].parent;
    }
    return 0;
}

static int paint_commands_reserve(wn_core *c, int needed)
{
    int capacity;
    wn_paint_command *grown;
    if (needed <= c->paint_command_capacity) {
        return 1;
    }
    capacity = c->paint_command_capacity > 0 ? c->paint_command_capacity : 64;
    while (capacity < needed) {
        capacity *= 2;
    }
    grown = (wn_paint_command *)realloc(
        c->paint_commands, (size_t)capacity * sizeof(wn_paint_command));
    if (grown == NULL) {
        return 0;
    }
    c->paint_commands = grown;
    c->paint_command_capacity = capacity;
    return 1;
}

static int paint_commands_emit(wn_core *c, int kind, int item, int layer)
{
    wn_paint_command *command;
    if (!paint_commands_reserve(c, c->paint_command_count + 1)) {
        return 0;
    }
    command = &c->paint_commands[c->paint_command_count++];
    command->kind = kind;
    command->item = item;
    command->layer = layer;
    return 1;
}

/* Replays the DFS item order as DRAW/BEGIN_LAYER/END_LAYER commands. Items in
 * one layer are contiguous, so a stack walk opens exactly the layers between
 * the previous item and the next one. */
static void build_paint_commands(wn_core *c)
{
    int stack[WN_MAX_PAINT_LAYER_DEPTH];
    int top = 0;
    int i;
    c->paint_command_count = 0;
    if (c->paint_layer_count <= 0) {
        return;
    }
    if (!paint_commands_reserve(c, c->paint_item_count * 2 +
                                       c->paint_layer_count * 2)) {
        c->paint_layer_count = 0;
        return;
    }
    stack[0] = 0;
    for (i = 0; i < c->paint_item_count; ++i) {
        int target = c->paint_items[i].layer;
        while (top > 0 && !paint_layer_ancestor(c, stack[top], target)) {
            paint_commands_emit(c, WN_PAINT_CMD_END_LAYER, -1, stack[top]);
            --top;
        }
        if (stack[top] != target) {
            int chain[WN_MAX_PAINT_LAYER_DEPTH];
            int count = 0;
            int walk = target;
            while (walk >= 0 && walk != stack[top] &&
                   count < WN_MAX_PAINT_LAYER_DEPTH) {
                chain[count++] = walk;
                walk = c->paint_layers[walk].parent;
            }
            while (count > 0) {
                --count;
                if (top + 1 >= WN_MAX_PAINT_LAYER_DEPTH) {
                    break;
                }
                stack[++top] = chain[count];
                paint_commands_emit(c, WN_PAINT_CMD_BEGIN_LAYER, -1,
                                    chain[count]);
            }
        }
        paint_commands_emit(c, WN_PAINT_CMD_DRAW, i, -1);
    }
    while (top > 0) {
        paint_commands_emit(c, WN_PAINT_CMD_END_LAYER, -1, stack[top]);
        --top;
    }
}

/* Stable bottom-up merge sort by layer. Every layer is 0 in production, so the
 * first scan returns before any allocation or element copy (O(n), order
 * untouched, 0 moves). A distinct-layer list pays one malloc plus O(n log n)
 * merge passes. Ties take the left run, preserving the DFS order of equal
 * layers. Returns the number of element copies performed (0 on the all-equal
 * fast path) so the test can prove the fast path; layout ignores the result. */
int wn_core_sort_paint_items(wn_core *c)
{
    int n, i, width, copies = 0;
    WnPaintItem *src, *dst, *tmp;

    if (c == NULL || c->paint_item_count < 2) {
        return 0;
    }
    n = c->paint_item_count;
    for (i = 1; i < n; ++i) {
        if (c->paint_items[i].layer != c->paint_items[0].layer) {
            break;
        }
    }
    if (i == n) {
        return 0; /* all-equal fast path: O(n) scan, no allocation, no copies */
    }

    tmp = (WnPaintItem *)malloc((size_t)n * sizeof(*tmp));
    if (tmp == NULL) {
        /* Out of memory: stable insertion sort keeps the contract. */
        for (i = 1; i < n; ++i) {
            WnPaintItem item = c->paint_items[i];
            int j = i - 1;
            while (j >= 0 && c->paint_items[j].layer > item.layer) {
                c->paint_items[j + 1] = c->paint_items[j];
                --j;
                ++copies;
            }
            if (j + 1 != i) {
                c->paint_items[j + 1] = item;
                ++copies;
            }
        }
        return copies;
    }

    src = c->paint_items;
    dst = tmp;
    for (width = 1; width < n; width *= 2) {
        int left;
        for (left = 0; left < n; left += width * 2) {
            int mid = left + width < n ? left + width : n;
            int right = left + width * 2 < n ? left + width * 2 : n;
            int a = left, b = mid, k = left;
            while (a < mid && b < right) {
                if (src[a].layer <= src[b].layer) {
                    dst[k++] = src[a++]; /* ties keep the left (DFS) run */
                } else {
                    dst[k++] = src[b++];
                }
            }
            while (a < mid) {
                dst[k++] = src[a++];
            }
            while (b < right) {
                dst[k++] = src[b++];
            }
            copies += right - left;
        }
        {
            WnPaintItem *swap = src;
            src = dst;
            dst = swap;
        }
    }
    if (src != c->paint_items) {
        memcpy(c->paint_items, src, (size_t)n * sizeof(*src));
    }
    free(tmp);
    return copies;
}

static int transform_is_identity(const wn_transform *t)
{
    return t->m0 == 1.0f && t->m1 == 0.0f && t->m2 == 0.0f &&
           t->m3 == 1.0f && t->m4 == 0.0f && t->m5 == 0.0f;
}

/* AABB of a rect after the 2x3 matrix. The clip stays the parent's
 * axis-aligned screen rect; only the item geometry is transformed. */
static wn_rect transform_rect_aabb(const wn_transform *t, wn_rect r)
{
    wn_rect out;
    float x0, x1, x2, x3;
    float y0, y1, y2, y3;
    if (transform_is_identity(t)) {
        return r;
    }
    x0 = t->m0 * r.x + t->m2 * r.y + t->m4;
    y0 = t->m1 * r.x + t->m3 * r.y + t->m5;
    x1 = t->m0 * (r.x + r.w) + t->m2 * r.y + t->m4;
    y1 = t->m1 * (r.x + r.w) + t->m3 * r.y + t->m5;
    x2 = t->m0 * (r.x + r.w) + t->m2 * (r.y + r.h) + t->m4;
    y2 = t->m1 * (r.x + r.w) + t->m3 * (r.y + r.h) + t->m5;
    x3 = t->m0 * r.x + t->m2 * (r.y + r.h) + t->m4;
    y3 = t->m1 * r.x + t->m3 * (r.y + r.h) + t->m5;
    out.x = fminf(fminf(x0, x1), fminf(x2, x3));
    out.y = fminf(fminf(y0, y1), fminf(y2, y3));
    out.w = fmaxf(fmaxf(x0, x1), fmaxf(x2, x3)) - out.x;
    out.h = fmaxf(fmaxf(y0, y1), fmaxf(y2, y3)) - out.y;
    return out;
}

int wn_paint_item_visible(const wn_core *c, const WnPaintItem *item)
{
    wn_rect viewport;
    wn_rect box;
    if (c == NULL || item == NULL || rect_is_empty(item->clip)) {
        return 0;
    }
    if (item->node < 0 || item->node >= c->node_count || !c->alive[item->node]) {
        return 0;
    }
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.w = (float)c->width;
    viewport.h = (float)c->height;
    if (rect_is_empty(viewport)) {
        return 0;
    }
    box = transform_rect_aabb(&item->transform, c->rects[item->node]);
    return !rect_is_empty(rect_intersection(box, viewport));
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
    c->paint_item_count = 0;
    if (root >= 0) {
        int layered;
        layout_node(c, root, 0.0f, 0.0f, viewport_w, viewport_h, 1, -1.0f, -1.0f);
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.w = viewport_w;
        viewport.h = viewport_h;
        if (c->visible_eff[root]) {
            /* Out-of-flow nodes: their containing blocks are final now. A
             * static root is not a containing block: the viewport is. */
            WnContainingBlock root_cb =
                WN_IS_POSITIONED(&c->nodes[root])
                    ? containing_block_from_node(c, root)
                    : containing_block_viewport(viewport_w, viewport_h);
            layout_absolute_children(c, root, &root_cb);
            compute_clips(c, root, viewport, viewport);
        }
        layered = paint_layers_begin(c, viewport);
        if (layered) {
            /* Group/z contexts: explicit CSS stacking order. Layer order is
             * explicit too, so the flat category sort is not used. */
            if (!build_paint_items_ordered(c, root, viewport)) {
                /* OOM for the context tables: keep the historical DFS list so
                 * a frame still comes out. */
                build_paint_items_dfs(c, root, 0);
            }
            build_paint_commands(c);
        } else {
            build_paint_items_dfs(c, root, 0);
            wn_core_sort_paint_items(c);
        }
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
