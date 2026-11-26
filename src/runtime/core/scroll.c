/*
 * scroll.c - the single clamp/apply implementation for scroll offsets.
 *
 * Both axes share the same rule: an offset lives in [0, scroll_max_axis]. The
 * max comes from layout; horizontal max is always 0 in this lane, which makes
 * the x half of every call a no-op without any caller-side branch.
 */
#include "scroll.h"

#include <math.h>

#include "wn_core_internal.h"
#include "wn_internal.h"

static float scroll_max_axis(const wn_core *c, int node, int axis)
{
    return axis == WN_SCROLL_X ? c->scroll_max_x[node] : c->scroll_max_y[node];
}

static float *scroll_offset_axis(wn_core *c, int node, int axis)
{
    return axis == WN_SCROLL_X ? &c->scroll_x[node] : &c->scroll_y[node];
}

float wn_core_scroll_clamp(wn_core *c, int node, int axis, float value)
{
    float max;
    if (!wn_core_node_is_alive(c, node) ||
        (axis != WN_SCROLL_X && axis != WN_SCROLL_Y)) {
        return 0.0f;
    }
    max = scroll_max_axis(c, node, axis);
    if (!(max > 0.0f)) {
        max = 0.0f;
    }
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > max) {
        return max;
    }
    return value;
}

int wn_core_scroll_by(wn_core *c, int node, float dx, float dy)
{
    float *offset_x;
    float *offset_y;
    float next_x;
    float next_y;
    /* A NaN would poison the stored offset for the rest of the frame, and an
     * infinity would only be clamped on one axis. Reject the whole call. */
    if (!wn_core_node_is_alive(c, node) || !isfinite(dx) || !isfinite(dy)) {
        return -1;
    }
    offset_x = scroll_offset_axis(c, node, WN_SCROLL_X);
    offset_y = scroll_offset_axis(c, node, WN_SCROLL_Y);
    next_x = wn_core_scroll_clamp(c, node, WN_SCROLL_X, *offset_x + dx);
    next_y = wn_core_scroll_clamp(c, node, WN_SCROLL_Y, *offset_y + dy);
    if (next_x == *offset_x && next_y == *offset_y) {
        return 0;
    }
    *offset_x = next_x;
    *offset_y = next_y;
    {
        double t0 = wn_now_ms();
        layout_page(c, (float)c->width, (float)c->height);
        c->last_layout_ms = wn_now_ms() - t0;
    }
    return 1;
}
