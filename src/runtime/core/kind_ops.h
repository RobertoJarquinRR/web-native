/*
 * kind_ops.h - per-node-kind capability table for layout.
 *
 * layout.c never branches on UiNode.kind: every R2_NODE_* owns one entry here
 * with its auto-size rules, its max-content width and the algorithm that lays
 * out its children. Adding a kind means adding an entry, not another `if`.
 *
 * Internal header: no ABI, no exported symbol, no public include.
 */
#ifndef WN_KIND_OPS_H
#define WN_KIND_OPS_H

#include "wn_core_internal.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct WnKindOps {
    const char *name;
    int is_leaf;
    /* Border-box width used only when the node has no forced/declared width.
     * `available` is the parent content width the node is resolved against. */
    float (*auto_width)(wn_core *c, int node, float available, float parent_content_h);
    /* Border-box height of a leaf when neither a declared px height nor a
     * percentage height applies. `width` is the resolved border-box width and
     * `content_h` the inner height already resolved by the caller (-1 when the
     * node is auto-sized); NULL for containers, which use layout_children. */
    float (*auto_height)(wn_core *c, int node, float width, float content_h);
    /* Max-content border-box width. Declared px/% widths are handled by the
     * caller before the hook runs. */
    float (*intrinsic_width)(wn_core *c, int node, float available);
    /* Lay out the children and return the y cursor after the last one (the
     * caller derives the used content height from it). NULL for leaves. */
    float (*layout_children)(wn_core *c, int node, float content_x, float content_y,
                             float content_w, float content_h);
} WnKindOps;

/* Entry for `kind`, or NULL outside R2_NODE_BLOCK..R2_NODE_INPUT. Callers that
 * need to keep the historical behaviour for a corrupt kind fall back to
 * R2_NODE_BLOCK (see layout_node). */
const WnKindOps *wn_kind_ops(int kind);

/* Max-content width: the caller's declared px/% rule, then the kind hook.
 * Recursive so parent hooks ask their children the same question. */
float wn_intrinsic_width(wn_core *c, int node, float available);

/* Children algorithms exposed for the table. All return the y cursor after the
 * last child; the flex/grid wrappers reproduce the exact cursor arithmetic
 * layout_node used before the table existed. core/layout.c owns them. */
float wn_layout_block_children(wn_core *c, int node, float content_x, float content_y,
                               float content_w, float content_h);
float wn_layout_flex_children(wn_core *c, int node, float content_x, float content_y,
                              float content_w, float content_h);
float wn_layout_grid_children(wn_core *c, int node, float content_x, float content_y,
                              float content_w, float content_h);

#if defined(__cplusplus)
}
#endif

#endif /* WN_KIND_OPS_H */
