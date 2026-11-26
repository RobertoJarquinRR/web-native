/*
 * hit_test.c - hit testing, wheel routing and the pointer state machine.
 *
 * Reverse paint-order testing keeps the original press > hover > selected
 * priority and ignores nodes clipped out by an overflow ancestor.
 */
#include <math.h>

#include "wn_core_internal.h"
#include "wn_internal.h"

/* ------------------------------------------------------------- interaction */

static int point_in_rect(float px, float py, const wn_rect *r)
{
    return r->w > 0.0f && r->h > 0.0f &&
           px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

/* Public pointer/hit entry points can run between a mutation and the next
 * frame. Bring geometry up to date first so hidden/removed nodes never win a
 * hit even if the host has not rendered yet. */
void ensure_layout_for_hit(wn_core *core)
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

int wn_core_node_rect(wn_core *core, int node, wn_rect *out)
{
    if (core == NULL || out == NULL || !wn_core_node_is_alive(core, node)) {
        return -1;
    }
    ensure_layout_for_hit(core);
    *out = core->rects[node];
    return 0;
}

int wn_core_scroll_node_into_view(wn_core *core, int node, float margin)
{
    int container;
    wn_rect r;
    wn_rect content;
    const UiNode *cn;
    float top;
    float bottom;
    float next;
    if (core == NULL || !wn_core_node_is_alive(core, node)) {
        return -1;
    }
    ensure_layout_for_hit(core);
    container = core->parent[node];
    while (container >= 0) {
        if (core->nodes[container].overflow == R2_OVERFLOW_SCROLL &&
            core->scroll_max[container] > 0.0f) {
            break;
        }
        container = core->parent[container];
    }
    if (container < 0) {
        return 0; /* nothing scrollable above: already "visible" */
    }
    r = core->rects[node];
    cn = &core->nodes[container];
    content.x = core->rects[container].x + cn->padding[3];
    content.y = core->rects[container].y + cn->padding[0];
    content.w = fmaxf(core->rects[container].w - cn->padding[1] - cn->padding[3], 0.0f);
    content.h = fmaxf(core->rects[container].h - cn->padding[0] - cn->padding[2], 0.0f);
    if (margin < 0.0f) {
        margin = 0.0f;
    }
    top = content.y + margin;
    bottom = content.y + content.h - margin;
    if (bottom < top) {
        top = content.y;
        bottom = content.y + content.h;
    }
    next = core->scroll[container];
    if (r.y < top) {
        next -= top - r.y;
    } else if (r.y + r.h > bottom) {
        next += (r.y + r.h) - bottom;
    }
    if (next < 0.0f) {
        next = 0.0f;
    }
    if (next > core->scroll_max[container]) {
        next = core->scroll_max[container];
    }
    if (next == core->scroll[container]) {
        return 0;
    }
    core->scroll[container] = next;
    {
        double t0 = wn_now_ms();
        layout_page(core, (float)core->width, (float)core->height);
        core->last_layout_ms = wn_now_ms() - t0;
    }
    return 0;
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
    int previous;
    if (core == NULL) {
        return -1;
    }
    previous = core->hover;
    hit = wn_core_hit_test(core, px, py);
    if (previous >= 0 && previous != hit) {
        core->state[previous] = WN_STATE_CLEAR(core->state[previous], WN_STATE_HOVER);
    }
    if (hit >= 0) {
        core->state[hit] |= (unsigned char)WN_STATE_HOVER;
    }
    core->hover = hit;
    if (hit != previous) {
        core->dirty = 1;
    }
    /* Button held on the focused input: extend the selection from the anchor. */
    if (core->input_drag >= 0 && core->press == core->input_drag &&
        core->input_focus == core->input_drag) {
        wn_core_input_drag_to(core, core->input_drag, px);
    }
    return hit;
}

int wn_core_pointer_down(wn_core *core, float px, float py)
{
    int hit;
    if (core == NULL) {
        return -1;
    }
    hit = wn_core_hit_test(core, px, py);
    if (core->hover >= 0 && core->hover != hit) {
        core->state[core->hover] = WN_STATE_CLEAR(core->state[core->hover], WN_STATE_HOVER);
    }
    if (hit >= 0) {
        core->state[hit] |= (unsigned char)WN_STATE_HOVER;
    }
    core->hover = hit;
    if (core->press >= 0 && core->press != hit) {
        core->state[core->press] = WN_STATE_CLEAR(core->state[core->press], WN_STATE_PRESS);
    }
    if (hit >= 0) {
        core->state[hit] |= (unsigned char)WN_STATE_PRESS;
    }
    core->press = hit;
    core->input_drag = -1;
    core->dirty = 1;
    if (hit >= 0 && input_record(core, hit) != NULL) {
        wn_core_focus_input(core, hit, px);
        /* Drag-select anchor starts where the click landed. */
        core->input_anchor[hit] = core->input_cursor[hit];
        core->input_drag = hit;
    } else {
        wn_core_blur_input(core);
    }
    return hit;
}

/* Keep the pre-v6 click rule: a select click replaces the selection. The
 * per-node mask still lets a host mark several nodes selected directly. */
static void clear_selected(wn_core *core)
{
    int i;
    for (i = 0; i < core->node_count; ++i) {
        if (core->alive[i] && (core->state[i] & WN_STATE_SELECTED) != 0u) {
            core->state[i] = WN_STATE_CLEAR(core->state[i], WN_STATE_SELECTED);
        }
    }
}

int wn_core_pointer_up(wn_core *core, float px, float py)
{
    int hit;
    if (core == NULL) {
        return -1;
    }
    hit = wn_core_hit_test(core, px, py);
    if (core->hover >= 0 && core->hover != hit) {
        core->state[core->hover] = WN_STATE_CLEAR(core->state[core->hover], WN_STATE_HOVER);
    }
    if (hit >= 0) {
        core->state[hit] |= (unsigned char)WN_STATE_HOVER;
    }
    core->hover = hit;
    if (core->press >= 0 && hit == core->press &&
        core->nodes[core->press].action == ACTION_SELECT) {
        clear_selected(core);
        core->state[core->press] |= (unsigned char)WN_STATE_SELECTED;
    }
    if (core->press >= 0 && input_record(core, core->press) != NULL &&
        core->input_anchor[core->press] == core->input_cursor[core->press]) {
        /* A plain click leaves no selection. */
        core->input_anchor[core->press] = -1;
    }
    if (core->press >= 0) {
        core->state[core->press] = WN_STATE_CLEAR(core->state[core->press], WN_STATE_PRESS);
    }
    core->press = -1;
    core->input_drag = -1;
    core->dirty = 1;
    return hit;
}

/* The pointer left the window or capture was lost: no node may stay hovered
 * or pressed. A drag in progress ends without touching the selection. */
void wn_core_pointer_leave(wn_core *core)
{
    if (core == NULL) {
        return;
    }
    if (core->hover >= 0 || core->press >= 0 || core->input_drag >= 0) {
        core->dirty = 1;
    }
    if (core->hover >= 0) {
        core->state[core->hover] = WN_STATE_CLEAR(core->state[core->hover], WN_STATE_HOVER);
    }
    if (core->press >= 0) {
        core->state[core->press] = WN_STATE_CLEAR(core->state[core->press], WN_STATE_PRESS);
    }
    core->hover = -1;
    core->press = -1;
    core->input_drag = -1;
}
