/*
 * kind_ops.c - the capability table layout dispatches through.
 *
 * Each entry owns what used to be an `if (n->kind == ...)` inside layout.c:
 * the auto width/height of a leaf, the max-content width and the children
 * algorithm. Every branch reads the same UiNode fields with the same float
 * operations and constants the old chains used, so the table is a pure
 * refactor: no geometry changes.
 */
#include <math.h>
#include <string.h>

#include "kind_ops.h"

/* ------------------------------------------------------------ text helpers */

static const char *node_text(const wn_core *c, int node)
{
    if (c->text_override != NULL && c->text_override[node] != NULL) {
        return c->text_override[node];
    }
    return c->nodes[node].text != NULL ? c->nodes[node].text : "";
}

static void measure_text_node(const wn_core *c, int node, float max_width, wn_text_metrics *out)
{
    const char *text = node_text(c, node);
    int weight = wn_core_node_font_weight(c, node);
    const char *family = wn_core_node_font_family(c, node);
    float size = wn_core_node_font_size(c, node);
    if (wn_text_measure_string(text, family, size, weight,
                               max_width > 0.0f ? max_width : 0.0f, out) == 0) {
        return;
    }
    out->width = size * 0.6f * (float)strlen(text);
    out->height = size * 1.25f;
    out->ascent = size;
    out->descent = 0.0f;
}

static float text_node_width(const wn_core *c, int node, float max_width)
{
    const UiNode *n = &c->nodes[node];
    const char *text = node_text(c, node);
    int weight = wn_core_node_font_weight(c, node);
    const char *family = wn_core_node_font_family(c, node);
    float size = wn_core_node_font_size(c, node);
    wn_text_metrics metrics;
    /* A constrained width picks the widest greedy line; unconstrained keeps
     * the backend's single-line advance width. */
    if (max_width > 0.0f) {
        wn_text_block block;
        if (wn_text_block_get(text, family, size, weight,
                              max_width, 0.0f, &block) == 0) {
            return ceilf(block.layout->width) + WN_DECL_W(n);
        }
    }
    measure_text_node(c, node, 0.0f, &metrics);
    return ceilf(metrics.width) + WN_DECL_W(n);
}

static float text_node_height(const wn_core *c, int node, float max_width)
{
    const UiNode *n = &c->nodes[node];
    const char *text = node_text(c, node);
    int weight = wn_core_node_font_weight(c, node);
    const char *family = wn_core_node_font_family(c, node);
    float size = wn_core_node_font_size(c, node);
    wn_text_block block;
    wn_text_metrics metrics;

    if (wn_text_block_get(text, family, size, weight,
                          max_width > 0.0f ? max_width : 0.0f,
                          WN_LINE_HEIGHT(n), &block) != 0) {
        /* The backend measure with a max_width is a hint only: constrained
         * nodes are sized by the engine breaker above. */
        measure_text_node(c, node, 0.0f, &metrics);
        return ceilf(metrics.height) + WN_DECL_H(n);
    }
    if (block.line_count <= 1 && WN_LINE_HEIGHT(n) <= 0.0f && max_width <= 0.0f) {
        /* Unconstrained single-line text keeps the backend's exact box. A
         * constrained node always follows the engine breaker, even at one
         * line. */
        return ceilf(block.measured_height) + WN_DECL_H(n);
    }
    return ceilf(block.box_height) + WN_DECL_H(n);
}

/* ------------------------------------------------------------ width hooks */

static float text_auto_width(wn_core *c, int node, float available, float parent_content_h)
{
    const UiNode *n = &c->nodes[node];
    (void)parent_content_h;
    return text_node_width(c, node, available - n->margin[1] - n->margin[3]);
}

static float image_auto_width(wn_core *c, int node, float available, float parent_content_h)
{
    const UiNode *n = &c->nodes[node];
    (void)parent_content_h;
    if (n->image_width > 0.0f) {
        return n->image_width;
    }
    return fmaxf(available - n->margin[1] - n->margin[3], 0.0f);
}

/* ----------------------------------------------------------- height hooks */

/* Text: the greedy breaker owns the box; the width is the content width, so
 * subtract the padding exactly like layout_node's content_w. */
static float text_auto_height(wn_core *c, int node, float width, float content_h)
{
    const UiNode *n = &c->nodes[node];
    (void)content_h; /* declared/percentage heights resolve before the hook */
    return text_node_height(c, node, fmaxf(width - n->padding[3] - n->padding[1], 0.0f));
}

/* Image: aspect ratio, and the historical `content_h` fallback. */
static float image_auto_height(wn_core *c, int node, float width, float content_h)
{
    const UiNode *n = &c->nodes[node];
    if (n->image_height > 0.0f && n->image_width > 0.0f) {
        return width * (n->image_height / n->image_width);
    }
    return content_h >= 0.0f ? content_h : 0.0f;
}

/* Input: one editor line (ascender + descender, never below 1px) plus the
 * vertical padding. */
static float input_auto_height(wn_core *c, int node, float width, float content_h)
{
    const UiNode *n = &c->nodes[node];
    (void)width;
    (void)content_h;
    return n->padding[0] + n->padding[2] + fmaxf(input_line_height(c, node), 1.0f);
}

/* -------------------------------------------------------- intrinsic hooks */

static float children_max_intrinsic(wn_core *c, int node, float available)
{
    float maxw = 0.0f;
    int child;
    for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
        float w;
        if (!c->alive[child] || !c->visible[child] ||
            WN_IS_OUT_OF_FLOW(&c->nodes[child])) {
            continue;
        }
        w = wn_intrinsic_width(c, child, available);
        if (w > maxw) {
            maxw = w;
        }
    }
    return maxw;
}

/* Row flex: content widths plus the gaps, not the container width. */
static float children_row_intrinsic(wn_core *c, int node, float available)
{
    const UiNode *n = &c->nodes[node];
    float sum = 0.0f;
    int count = 0;
    int child;
    for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
        if (!c->alive[child] || !c->visible[child] ||
            WN_IS_OUT_OF_FLOW(&c->nodes[child])) {
            continue;
        }
        sum += wn_intrinsic_width(c, child, available);
        ++count;
    }
    if (count > 0) {
        sum += (n->gap > 0.0f ? n->gap : 0.0f) * (float)(count - 1);
    }
    return sum;
}

static float block_intrinsic_width(wn_core *c, int node, float available)
{
    const UiNode *n = &c->nodes[node];
    return children_max_intrinsic(c, node, available) + n->padding[1] + n->padding[3];
}

static float flex_intrinsic_width(wn_core *c, int node, float available)
{
    const UiNode *n = &c->nodes[node];
    float content;
    if (n->direction == 0) {
        content = children_row_intrinsic(c, node, available);
    } else {
        content = children_max_intrinsic(c, node, available);
    }
    return content + n->padding[1] + n->padding[3];
}

static float grid_intrinsic_width(wn_core *c, int node, float available)
{
    const UiNode *n = &c->nodes[node];
    if (n->grid_mode == R2_GRID_AUTO_FILL && n->grid_column_px > 0.0f) {
        return n->grid_column_px + n->padding[1] + n->padding[3];
    }
    return children_max_intrinsic(c, node, available) + n->padding[1] + n->padding[3];
}

static float text_intrinsic_width(wn_core *c, int node, float available)
{
    return text_node_width(c, node, available);
}

static float image_intrinsic_width(wn_core *c, int node, float available)
{
    const UiNode *n = &c->nodes[node];
    if (n->image_width > 0.0f) {
        return n->image_width + n->padding[1] + n->padding[3];
    }
    /* No intrinsic size: same max-of-children fallback as a block. */
    return children_max_intrinsic(c, node, available) + n->padding[1] + n->padding[3];
}

static float input_intrinsic_width(wn_core *c, int node, float available)
{
    const UiNode *n = &c->nodes[node];
    /* Default width of a field without CSS: roughly 20 characters, bounded by
     * max_length when the field declares one. */
    const WnInputRecord *rec = input_record(c, node);
    float width = 200.0f;
    (void)available;
    if (rec != NULL && rec->max_length > 0) {
        float estimated = (float)rec->max_length *
                          wn_core_node_font_size(c, node) * 0.62f;
        width = fmaxf(80.0f, fminf(estimated, 480.0f));
    }
    return width + n->padding[1] + n->padding[3];
}

/* ----------------------------------------------------------- video hooks */
/*
 * A video is a replaced element; its intrinsic size is not baked in the blob.
 * Phase A has no decoder and no intrinsic poster probe, so the documented
 * HTML default (300 x 150) is the placeholder aspect until a provider or a
 * poster reports a real size. Declared CSS width/height always win in
 * layout_node before these hooks run.
 */
#define WN_VIDEO_DEFAULT_WIDTH 300.0f
#define WN_VIDEO_DEFAULT_HEIGHT 150.0f

static float video_auto_width(wn_core *c, int node, float available,
                              float parent_content_h)
{
    (void)c;
    (void)node;
    (void)available;
    (void)parent_content_h;
    return WN_VIDEO_DEFAULT_WIDTH;
}

static float video_auto_height(wn_core *c, int node, float width, float content_h)
{
    (void)c;
    (void)node;
    (void)content_h;
    if (width > 0.0f) {
        return width * (WN_VIDEO_DEFAULT_HEIGHT / WN_VIDEO_DEFAULT_WIDTH);
    }
    return WN_VIDEO_DEFAULT_HEIGHT;
}

static float video_intrinsic_width(wn_core *c, int node, float available)
{
    const UiNode *n = &c->nodes[node];
    (void)available;
    return WN_VIDEO_DEFAULT_WIDTH + n->padding[1] + n->padding[3];
}

/* ------------------------------------------------------------------ table */

/* One entry per R2_NODE_*, in enum order. */
const WnKindOps wn_kind_table[WN_KIND_COUNT] = {    /* R2_NODE_BLOCK */
    { "block", 0, NULL, NULL, block_intrinsic_width, wn_layout_block_children },
    /* R2_NODE_FLEX */
    { "flex", 0, NULL, NULL, flex_intrinsic_width, wn_layout_flex_children },
    /* R2_NODE_TEXT */
    { "text", 1, text_auto_width, text_auto_height, text_intrinsic_width, NULL },
    /* R2_NODE_GRID */
    { "grid", 0, NULL, NULL, grid_intrinsic_width, wn_layout_grid_children },
    /* R2_NODE_IMAGE */
    { "image", 1, image_auto_width, image_auto_height, image_intrinsic_width, NULL },
    /* R2_NODE_INPUT */
    { "input", 1, NULL, input_auto_height, input_intrinsic_width, NULL },
    /* R2_NODE_VIDEO */
    { "video", 1, video_auto_width, video_auto_height, video_intrinsic_width, NULL },
};
