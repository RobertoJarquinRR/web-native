/*
 * paint.c - solid/glyph/image geometry and the display list builder.
 *
 * Quads and rounded outlines are emitted in viewport pixels and converted to
 * clip space here; the renderer only sees batches and scissor rects.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wn_core_internal.h"

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

/* ------------------------------------------------------- state colors */

/* Generated text runs follow the state of their owning element (their parent
 * in the runtime tree); every other node uses its own mask. */
unsigned char wn_core_effective_state(const wn_core *c, int node)
{
    const UiNode *n = &c->nodes[node];
    if (n->kind == R2_NODE_TEXT && c->parent[node] >= 0) {
        return c->state[c->parent[node]];
    }
    return c->state[node];
}

const UiStateStyle *wn_core_state_entry(const UiNode *n, int code)
{
    unsigned char i;
    for (i = 0; i < n->state_style_count; ++i) {
        if (n->state_styles[i].state == (unsigned char)code) {
            return &n->state_styles[i];
        }
    }
    return NULL;
}

static int state_color(const UiNode *n, int code, unsigned char field,
                       unsigned int *out)
{
    const UiStateStyle *style = wn_core_state_entry(n, code);
    if (style == NULL || (style->present & field) == 0u) {
        return 0;
    }
    switch (field) {
    case R2_STATE_FIELD_BACKGROUND:
        *out = style->background;
        break;
    case R2_STATE_FIELD_TEXT:
        *out = style->text;
        break;
    default:
        *out = style->border_color;
        break;
    }
    return 1;
}

/* State weight override. `reserved` is the additive v6 field; the present bit
 * says whether it applies. */
static int state_weight(const UiNode *n, int code, int *out)
{
    const UiStateStyle *style = wn_core_state_entry(n, code);
    if (style == NULL || (style->present & R2_STATE_FIELD_FONT_WEIGHT) == 0u) {
        return 0;
    }
    *out = (int)style->reserved;
    return 1;
}

/* CSS state styles take precedence; data-* attributes remain the sugar for
 * variants without CSS. Selected is the app's base color: it shows when no
 * higher-priority state overrides the field. */
unsigned int wn_core_node_background(const wn_core *c, int node)
{
    const UiNode *n = &c->nodes[node];
    unsigned char state = wn_core_effective_state(c, node);
    unsigned int color;
    /* ABI v7: a host background override behaves like an inline style and wins
     * over the compiled state styles. It never inherits. */
    if (c->style != NULL &&
        (c->style[node].fields & WN_STYLE_BACKGROUND) != 0u) {
        return c->style[node].background;
    }
    if ((state & WN_STATE_DISABLED) != 0u) {
        if (state_color(n, R2_STATE_STYLE_DISABLED, R2_STATE_FIELD_BACKGROUND,
                        &color)) {
            return color;
        }
        return n->background;
    }
    if ((state & WN_STATE_PRESS) != 0u &&
        state_color(n, R2_STATE_STYLE_PRESS, R2_STATE_FIELD_BACKGROUND, &color)) {
        return color;
    }
    if ((state & WN_STATE_HOVER) != 0u &&
        state_color(n, R2_STATE_STYLE_HOVER, R2_STATE_FIELD_BACKGROUND, &color)) {
        return color;
    }
    if ((state & WN_STATE_FOCUS_VISIBLE) != 0u &&
        state_color(n, R2_STATE_STYLE_FOCUS_VISIBLE, R2_STATE_FIELD_BACKGROUND,
                    &color)) {
        return color;
    }
    if ((state & WN_STATE_FOCUSED) != 0u &&
        state_color(n, R2_STATE_STYLE_FOCUS, R2_STATE_FIELD_BACKGROUND, &color)) {
        return color;
    }
    if ((state & WN_STATE_FOCUS_WITHIN) != 0u &&
        state_color(n, R2_STATE_STYLE_FOCUS_WITHIN, R2_STATE_FIELD_BACKGROUND,
                    &color)) {
        return color;
    }
    if (n->action != ACTION_NONE) {
        if ((state & WN_STATE_PRESS) != 0u && n->press_bg != 0) {
            return n->press_bg;
        }
        if ((state & WN_STATE_HOVER) != 0u && n->hover_bg != 0) {
            return n->hover_bg;
        }
        if ((state & WN_STATE_SELECTED) != 0u && n->selected_bg != 0) {
            return n->selected_bg;
        }
    }
    return n->background;
}

unsigned int wn_core_node_text_color(const wn_core *c, int node)
{
    const UiNode *n = &c->nodes[node];
    unsigned char state = wn_core_effective_state(c, node);
    const WnNodeStyle *style;
    unsigned int color;
    /* ABI v7: text color inherits from the nearest self-or-ancestor override
     * and wins over the compiled state styles (inline-style priority). */
    style = wn_core_style_nearest(c, node, WN_STYLE_TEXT_COLOR);
    if (style != NULL) {
        return style->text_color;
    }
    if ((state & WN_STATE_DISABLED) != 0u) {
        if (state_color(n, R2_STATE_STYLE_DISABLED, R2_STATE_FIELD_TEXT, &color)) {
            return color;
        }
        return n->text_color;
    }
    if ((state & WN_STATE_PRESS) != 0u &&
        state_color(n, R2_STATE_STYLE_PRESS, R2_STATE_FIELD_TEXT, &color)) {
        return color;
    }
    if ((state & WN_STATE_HOVER) != 0u &&
        state_color(n, R2_STATE_STYLE_HOVER, R2_STATE_FIELD_TEXT, &color)) {
        return color;
    }
    if ((state & WN_STATE_FOCUS_VISIBLE) != 0u &&
        state_color(n, R2_STATE_STYLE_FOCUS_VISIBLE, R2_STATE_FIELD_TEXT, &color)) {
        return color;
    }
    if ((state & WN_STATE_FOCUSED) != 0u &&
        state_color(n, R2_STATE_STYLE_FOCUS, R2_STATE_FIELD_TEXT, &color)) {
        return color;
    }
    if ((state & WN_STATE_FOCUS_WITHIN) != 0u &&
        state_color(n, R2_STATE_STYLE_FOCUS_WITHIN, R2_STATE_FIELD_TEXT, &color)) {
        return color;
    }
    return n->text_color;
}

unsigned int wn_core_node_border_color(const wn_core *c, int node)
{
    const UiNode *n = &c->nodes[node];
    unsigned char state = wn_core_effective_state(c, node);
    unsigned int color;
    if ((state & WN_STATE_DISABLED) != 0u) {
        if (state_color(n, R2_STATE_STYLE_DISABLED, R2_STATE_FIELD_BORDER, &color)) {
            return color;
        }
        return n->border_color;
    }
    if ((state & WN_STATE_PRESS) != 0u &&
        state_color(n, R2_STATE_STYLE_PRESS, R2_STATE_FIELD_BORDER, &color)) {
        return color;
    }
    if ((state & WN_STATE_HOVER) != 0u &&
        state_color(n, R2_STATE_STYLE_HOVER, R2_STATE_FIELD_BORDER, &color)) {
        return color;
    }
    if ((state & WN_STATE_FOCUS_VISIBLE) != 0u &&
        state_color(n, R2_STATE_STYLE_FOCUS_VISIBLE, R2_STATE_FIELD_BORDER,
                    &color)) {
        return color;
    }
    if ((state & WN_STATE_FOCUSED) != 0u &&
        state_color(n, R2_STATE_STYLE_FOCUS, R2_STATE_FIELD_BORDER, &color)) {
        return color;
    }
    if ((state & WN_STATE_FOCUS_WITHIN) != 0u &&
        state_color(n, R2_STATE_STYLE_FOCUS_WITHIN, R2_STATE_FIELD_BORDER,
                    &color)) {
        return color;
    }
    return n->border_color;
}

/* Effective font weight for layout, shaping, atlas and paint. Same priority as
 * the colors: disabled > press > hover > focus-visible > focus > focus-within
 * > base. Generated text runs read the owner's state through
 * wn_core_effective_state(). */
int wn_core_node_font_weight(const wn_core *c, int node)
{
    const UiNode *n = &c->nodes[node];
    unsigned char state = wn_core_effective_state(c, node);
    const WnNodeStyle *style;
    int weight;
    /* ABI v7: font weight follows the same inherited runtime layer and wins
     * over the compiled state weight. */
    style = wn_core_style_nearest(c, node, WN_STYLE_FONT_WEIGHT);
    if (style != NULL) {
        return style->font_weight;
    }
    if ((state & WN_STATE_DISABLED) != 0u) {
        if (state_weight(n, R2_STATE_STYLE_DISABLED, &weight)) {
            return weight;
        }
        return n->font_weight;
    }
    if ((state & WN_STATE_PRESS) != 0u &&
        state_weight(n, R2_STATE_STYLE_PRESS, &weight)) {
        return weight;
    }
    if ((state & WN_STATE_HOVER) != 0u &&
        state_weight(n, R2_STATE_STYLE_HOVER, &weight)) {
        return weight;
    }
    if ((state & WN_STATE_FOCUS_VISIBLE) != 0u &&
        state_weight(n, R2_STATE_STYLE_FOCUS_VISIBLE, &weight)) {
        return weight;
    }
    if ((state & WN_STATE_FOCUSED) != 0u &&
        state_weight(n, R2_STATE_STYLE_FOCUS, &weight)) {
        return weight;
    }
    if ((state & WN_STATE_FOCUS_WITHIN) != 0u &&
        state_weight(n, R2_STATE_STYLE_FOCUS_WITHIN, &weight)) {
        return weight;
    }
    return n->font_weight;
}

/* ---------------------------------------------------------------- geometry */

/* Item fast paths. The identity/opaque case returns before any float math so
 * coordinates and colors stay bit-identical to the pre-paint-list builders. */
static int paint_transform_identity(const wn_core *c)
{
    const wn_transform *t;
    if (c->paint_item == NULL) {
        return 1;
    }
    t = &c->paint_item->transform;
    return t->m0 == 1.0f && t->m1 == 0.0f && t->m2 == 0.0f &&
           t->m3 == 1.0f && t->m4 == 0.0f && t->m5 == 0.0f;
}

/* Pixel-space transform, before the clip-space projection. The node clip stays
 * screen-space: transformed-node clipping stays axis-aligned for now. */
static void paint_map_point(const wn_core *c, float *x, float *y)
{
    const wn_transform *t;
    float px, py;
    if (paint_transform_identity(c)) {
        return;
    }
    t = &c->paint_item->transform;
    px = *x;
    py = *y;
    *x = t->m0 * px + t->m2 * py + t->m4;
    *y = t->m1 * px + t->m3 * py + t->m5;
}

/* Effective item opacity. Document opacity < 1 is carried by the item's group
 * layer (the item itself stays 1.0); this factor is the flat path and the
 * layer-cap fallback, plus the per-item value tests inject directly. Identity
 * returns 1.0 with no float math so the fast path stays byte-identical. */
static float paint_opacity(const wn_core *c)
{
    float opacity;
    if (c->paint_item == NULL) {
        return 1.0f;
    }
    opacity = c->paint_item->opacity;
    if (!(opacity > 0.0f)) {
        return 0.0f;
    }
    return opacity > 1.0f ? 1.0f : opacity;
}

/* Solid/glyph alpha multiplier. Identity returns the byte untouched. */
static uint8_t paint_alpha(wn_core *c, uint8_t a)
{
    float opacity = paint_opacity(c);
    if (opacity == 1.0f || a == 0u) {
        return a;
    }
    if (opacity <= 0.0f) {
        return 0;
    }
    return (uint8_t)((float)a * opacity + 0.5f);
}

void wn_paint_use_item(wn_core *c, const WnPaintItem *item)
{
    if (c != NULL) {
        c->paint_item = item;
    }
}

static void push_vertex(wn_core *c, float x, float y, float vw, float vh,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    wn_vertex *v;
    paint_map_point(c, &x, &y);
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

void push_quad(wn_core *c, float x, float y, float w, float h,
                      float vw, float vh, unsigned int argb)
{
    uint8_t a;
    uint8_t r, g, b;
    float x1, y1;
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    a = paint_alpha(c, (uint8_t)((argb >> 24) & 0xFFu));
    if (a == 0) {
        return;
    }
    /* Reserve the whole quad before writing: a partial quad would paint a
     * degenerate triangle if the growth failed mid-way. */
    ensure_vertex_capacity(c, c->vertex_count + 6);
    if (c->vertex_count + 6 > c->vertex_capacity) {
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

/* 8 segments per corner, clockwise from the top-right arc.
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
    a = paint_alpha(c, (uint8_t)((argb >> 24) & 0xFFu));
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
    ensure_vertex_capacity(c, c->vertex_count + WN_ROUND_POINTS * 3);
    if (c->vertex_count + WN_ROUND_POINTS * 3 > c->vertex_capacity) {
        return;
    }
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

static void push_box_shape(wn_core *c, float x, float y, float w, float h,
                           float radius, unsigned int argb)
{
    push_rounded_quad(c, x, y, w, h, radius,
                      (float)c->width, (float)c->height, argb);
}

/* Hollow rounded ring (input focus ring): band between the outer rounded rect
 * and the same rect inset by `ring`. A square box keeps the exact 4-quad path
 * so inputs without border-radius stay byte-identical. */
void push_rounded_ring(wn_core *c, float x, float y, float w, float h,
                              float radius, float ring, unsigned int argb)
{
    float outer[WN_ROUND_POINTS][2];
    float inner[WN_ROUND_POINTS][2];
    uint8_t a, r, g, b;
    int i;
    if (w <= 0.0f || h <= 0.0f || ring <= 0.0f) {
        return;
    }
    a = paint_alpha(c, (uint8_t)((argb >> 24) & 0xFFu));
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
    ensure_vertex_capacity(c, c->vertex_count + WN_ROUND_POINTS * 6);
    if (c->vertex_count + WN_ROUND_POINTS * 6 > c->vertex_capacity) {
        return;
    }
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
    paint_map_point(c, &x, &y);
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

void push_glyph_quad(wn_core *c, float x, float y, float w, float h,
                            float u0, float v0, float u1, float v1,
                            float vw, float vh,
                            uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    float x1 = x + w, y1 = y + h;
    a = paint_alpha(c, a);
    if (a == 0u) {
        return;
    }
    /* Reserve the whole glyph up front: a half-written quad would paint a
     * degenerate triangle if the growth failed mid-way. */
    ensure_glyph_capacity(c, c->glyph_vertex_count + 6);
    if (c->glyph_vertex_count + 6 > c->glyph_vertex_capacity) {
        return;
    }
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

void set_solid_clip(wn_core *c, const wn_rect *clip)
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

void set_glyph_clip(wn_core *c, const wn_rect *clip)
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
                              float vw, float vh, float a)
{
    wn_image_vertex *out;
    paint_map_point(c, &x, &y);
    ensure_image_capacity(c, c->image_vertex_count + 1);
    if (c->image_vertex_count >= c->image_vertex_capacity) {
        return;
    }
    out = &c->image_vertices[c->image_vertex_count++];
    out->x = (x / vw) * 2.0f - 1.0f;
    out->y = 1.0f - (y / vh) * 2.0f;
    out->u = u;
    out->v = v;
    out->a = a;
}

/* An image is clipped by its own border-radius, or by the radius of a parent
 * with overflow:hidden (the media-library pilot cards rely on this). The
 * parent radius is reduced by its border width, matching the padding-box clip.
 * Uniform radius only; the same value rounds all corners. */
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
    float a = paint_opacity(c);
    int needed = radius > 0.5f ? WN_ROUND_POINTS * 3 : 6;
    if (a <= 0.0f) {
        return;
    }
    ensure_image_capacity(c, c->image_vertex_count + needed);
    if (c->image_vertex_count + needed > c->image_vertex_capacity) {
        return;
    }
    if (radius <= 0.5f) {
        push_image_vertex(c, x, y, u0, v0, vw, vh, a);
        push_image_vertex(c, x1, y, u1, v0, vw, vh, a);
        push_image_vertex(c, x1, y1, u1, v1, vw, vh, a);
        push_image_vertex(c, x, y, u0, v0, vw, vh, a);
        push_image_vertex(c, x1, y1, u1, v1, vw, vh, a);
        push_image_vertex(c, x, y1, u0, v1, vw, vh, a);
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
            push_image_vertex(c, cx, cy, cu, cv, vw, vh, a);
            push_image_vertex(c, points[i][0], points[i][1], u_a, v_a, vw, vh, a);
            push_image_vertex(c, points[next][0], points[next][1], u_b, v_b, vw, vh, a);
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
    if (paint_opacity(c) <= 0.0f) {
        return; /* opacity: 0 contributes no geometry (and no empty batch) */
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
    for (order_index = 0; order_index < c->paint_item_count; ++order_index) {
        const WnPaintItem *item = &c->paint_items[order_index];
        int node = item->node;
        const UiNode *n = &c->nodes[node];
        const wn_rect *r;
        unsigned int color;
        uint8_t a, cr, cg, cb;
        int j;
        wn_paint_draw *draw = wn_core_draw_for_item(c, order_index);
        int glyph_first = c->glyph_vertex_count;
        if (draw != NULL) {
            draw->glyph.first = glyph_first;
            draw->glyph.clip = item->clip;
        }
        if (n->kind != R2_NODE_TEXT) {
            continue;
        }
        r = &c->rects[node];
        color = wn_core_node_text_color(c, node);
        a = (uint8_t)((color >> 24) & 0xFFu);
        if (a == 0) {
            continue;
        }
        int weight = wn_core_node_font_weight(c, node);
        const char *family = wn_core_node_font_family(c, node);
        float size = wn_core_node_font_size(c, node);
        wn_paint_use_item(c, item);
        set_glyph_clip(c, &item->clip);
        cr = (uint8_t)((color >> 16) & 0xFFu);
        cg = (uint8_t)((color >> 8) & 0xFFu);
        cb = (uint8_t)(color & 0xFFu);

        {
            const char *text = c->text_override[node] != NULL ? c->text_override[node] : n->text;
            float wrap_width = fmaxf(r->w - n->padding[1] - n->padding[3], 0.0f);
            wn_text_block block;
            const wn_text_line_layout *layout;
            float node_leading;
            float avail = 0.0f;
            int align_per_line = 0;
            if (wn_text_block_get(text, family, size, weight,
                                  wrap_width, WN_LINE_HEIGHT(n), &block) != 0) {
                continue;
            }
            layout = block.layout;
            node_leading = r->h > block.box_height
                               ? (r->h - block.box_height) * 0.5f
                               : 0.0f;
            /* `text-align` shifts each line inside the parent content box.
             * Flex and grid parents already position the text item. */
            if (WN_TEXT_ALIGN(n) != 0 && c->parent[node] >= 0) {
                const UiNode *p = &c->nodes[c->parent[node]];
                if (p->kind == R2_NODE_BLOCK) {
                    avail = c->rects[c->parent[node]].w - p->padding[1] - p->padding[3];
                    align_per_line = 1;
                }
            }
            for (int line_index = 0; line_index < layout->line_count; ++line_index) {
                const wn_text_line *line = &layout->lines[line_index];
                const wn_atlas_run *run = wn_text_atlas_shape(
                    c->text_atlas, line->text, family, size, weight);
                float align_dx = 0.0f;
                float baseline;
                int draw_count;
                int ellipsis = 0;
                float ellipsis_x = 0.0f;
                const wn_atlas_run *ellipsis_run = NULL;
                if (run == NULL) {
                    continue;
                }
                draw_count = run->count;
                /* `text-overflow: ellipsis` is single-line only: with more than
                 * one line the box wraps normally and the flag is inert. The
                 * flag is only emitted for `overflow: hidden` boxes. */
                if ((n->text_flags & WN_TEXT_FLAG_ELLIPSIS) != 0u &&
                    layout->line_count == 1 && line->width > wrap_width) {
                    ellipsis_run = wn_text_atlas_shape(
                        c->text_atlas, "\xE2\x80\xA6", family, size, weight);
                    if (ellipsis_run != NULL && ellipsis_run->count > 0) {
                        float ellipsis_width = ellipsis_run->width;
                        float limit = wrap_width - ellipsis_width;
                        int i;
                        if (limit < 0.0f) {
                            limit = 0.0f;
                        }
                        ellipsis = 1;
                        ellipsis_x = wrap_width - ellipsis_width;
                        draw_count = run->count;
                        for (i = 0; i < run->count; ++i) {
                            if (run->glyphs[i].x + run->glyphs[i].advance >
                                limit + 0.01f) {
                                draw_count = i;
                                break;
                            }
                        }
                    }
                }
                if (align_per_line) {
                    float extent = layout->line_count == 1 ? r->w : line->width;
                    float room = avail - extent;
                    if (room > 0.0f) {
                        align_dx = WN_TEXT_ALIGN(n) == 1 ? room * 0.5f : room;
                    }
                }
                baseline = r->y + node_leading + block.half_leading + block.ascent +
                           (float)line_index * block.line_height;
                for (j = 0; j < draw_count; ++j) {
                    const wn_text_glyph *glyph = &run->glyphs[j];
                    const wn_atlas_glyph *packed = wn_text_atlas_glyph(
                        c->text_atlas, family, size, weight,
                        glyph->glyph);
                    float x;
                    float y;
                    if (packed == NULL || packed->width <= 0 || packed->height <= 0) {
                        continue;
                    }
                    x = floorf(r->x + align_dx + glyph->x + (float)packed->bearing_x + 0.5f);
                    y = floorf(baseline + (float)packed->bearing_y + 0.5f);
                    push_glyph_quad(c, x, y, (float)packed->width, (float)packed->height,
                                    packed->u0, packed->v0, packed->u1, packed->v1,
                                    viewport_w, viewport_h, cr, cg, cb, a);
                }
                if (ellipsis) {
                    int i;
                    for (i = 0; i < ellipsis_run->count; ++i) {
                        const wn_text_glyph *glyph = &ellipsis_run->glyphs[i];
                        const wn_atlas_glyph *packed = wn_text_atlas_glyph(
                            c->text_atlas, family, size, weight,
                            glyph->glyph);
                        float x;
                        float y;
                        if (packed == NULL || packed->width <= 0 ||
                            packed->height <= 0) {
                            continue;
                        }
                        x = floorf(r->x + align_dx + ellipsis_x + glyph->x +
                                   (float)packed->bearing_x + 0.5f);
                        y = floorf(baseline + (float)packed->bearing_y + 0.5f);
                        push_glyph_quad(c, x, y, (float)packed->width,
                                        (float)packed->height, packed->u0,
                                        packed->v0, packed->u1, packed->v1,
                                        viewport_w, viewport_h, cr, cg, cb, a);
                    }
                }
            }
        }
        if (draw != NULL) {
            draw->glyph.count = c->glyph_vertex_count - glyph_first;
        }
    }
}

static void paint_range_set(wn_paint_range *range, int first, int end,
                            wn_rect clip);

/* Visible scrollbars for `overflow:auto|scroll` boxes that really overflow.
 * Subtle track + rounded thumb; painted inside the node clip before
 * images/text. Wheel scrolling needs no hit test on the bar. The horizontal
 * branch is reserved: scroll_max_x is always 0 today, so it never paints, but
 * it keeps the same track/thumb style for when layout computes x overflow. */
static void build_scrollbars(wn_core *c)
{
    const float track_w = 8.0f;
    const float edge = 2.0f;
    int order;
    /* Synthetic post-pass: scrollbars are not nodes/items, but each one borrows
     * the owning item's clip and (currently identity) paint attrs. */
    for (order = 0; order < c->paint_item_count; ++order) {
        const WnPaintItem *item = &c->paint_items[order];
        int node = item->node;
        const UiNode *n = &c->nodes[node];
        const wn_rect *r;
        wn_paint_draw *draw;
        int scrollbar_first = c->vertex_count;
        if (n->overflow != R2_OVERFLOW_SCROLL) {
            continue;
        }
        wn_paint_use_item(c, item);
        r = &c->rects[node];

        if (c->scroll_max_y[node] > 0.5f) {
            float view_h = r->h - n->padding[0] - n->padding[2];
            if (view_h > track_w * 2.0f) {
                float content_h = view_h + c->scroll_max_y[node];
                float track_x = r->x + r->w - n->padding[1] - track_w - edge;
                float track_y = r->y + n->padding[0] + edge;
                float track_h = view_h - edge * 2.0f;
                if (track_h > 0.0f) {
                    float thumb_h = track_h * view_h / content_h;
                    float thumb_y = track_y;
                    float travel;
                    if (thumb_h < 24.0f) {
                        thumb_h = 24.0f;
                    }
                    if (thumb_h > track_h) {
                        thumb_h = track_h;
                    }
                    travel = track_h - thumb_h;
                    if (travel > 0.0f) {
                        thumb_y += travel *
                                   (c->scroll_y[node] / c->scroll_max_y[node]);
                    }
                    set_solid_clip(c, &item->clip);
                    push_rounded_quad(c, track_x, track_y, track_w, track_h,
                                      track_w * 0.5f, (float)c->width,
                                      (float)c->height, 0x33000000u);
                    push_rounded_quad(c, track_x, thumb_y, track_w, thumb_h,
                                      track_w * 0.5f, (float)c->width,
                                      (float)c->height, 0x665A667Au);
                }
            }
        }

        if (c->scroll_max_x[node] > 0.5f) {
            float view_w = r->w - n->padding[1] - n->padding[3];
            if (view_w > track_w * 2.0f) {
                float content_w = view_w + c->scroll_max_x[node];
                float track_x = r->x + n->padding[3] + edge;
                float track_y = r->y + r->h - n->padding[2] - track_w - edge;
                float track_len = view_w - edge * 2.0f;
                if (track_len > 0.0f) {
                    float thumb_w = track_len * view_w / content_w;
                    float thumb_x = track_x;
                    float travel;
                    if (thumb_w < 24.0f) {
                        thumb_w = 24.0f;
                    }
                    if (thumb_w > track_len) {
                        thumb_w = track_len;
                    }
                    travel = track_len - thumb_w;
                    if (travel > 0.0f) {
                        thumb_x += travel *
                                   (c->scroll_x[node] / c->scroll_max_x[node]);
                    }
                    set_solid_clip(c, &item->clip);
                    push_rounded_quad(c, track_x, track_y, track_len, track_w,
                                      track_w * 0.5f, (float)c->width,
                                      (float)c->height, 0x33000000u);
                    push_rounded_quad(c, thumb_x, track_y, thumb_w, track_w,
                                      track_w * 0.5f, (float)c->width,
                                      (float)c->height, 0x665A667Au);
                }
            }
        }
        draw = wn_core_draw_for_item(c, order);
        if (draw != NULL) {
            draw->scrollbar.first = scrollbar_first;
            draw->scrollbar.count = c->vertex_count - scrollbar_first;
            draw->scrollbar.clip = item->clip;
        }
    }
}

wn_paint_draw *wn_core_draw_for_item(wn_core *c, int index)
{
    if (c->paint_draws == NULL || index < 0 || index >= c->paint_draw_capacity) {
        return NULL;
    }
    return &c->paint_draws[index];
}

static void paint_range_set(wn_paint_range *range, int first, int end,
                            wn_rect clip)
{
    range->first = first;
    range->count = end > first ? end - first : 0;
    range->clip = clip;
}

void wn_core_build_display_list(wn_core *core)
{
    int i;
    int layered;
    unsigned int clear = 0xFF0B0E14u;
    int root = -1;
    if (core == NULL) {
        return;
    }
    if (core->needs_layout) {
        layout_page(core, (float)core->width, (float)core->height);
        core->needs_layout = 0;
    }
    layered = core->paint_layer_count > 1 && core->paint_command_count > 0;
    if (layered) {
        if (core->paint_item_count > core->paint_draw_capacity) {
            wn_paint_draw *grown = (wn_paint_draw *)realloc(
                core->paint_draws,
                (size_t)core->paint_item_count * sizeof(wn_paint_draw));
            if (grown != NULL) {
                core->paint_draws = grown;
                core->paint_draw_capacity = core->paint_item_count;
            }
        }
        if (core->paint_draw_capacity < core->paint_item_count) {
            /* Out of memory for the command model: render flat. The group
             * alpha is lost, but a frame still comes out. */
            layered = 0;
        } else {
            memset(core->paint_draws, 0,
                   (size_t)core->paint_item_count * sizeof(wn_paint_draw));
            for (i = 0; i < core->paint_item_count; ++i) {
                wn_paint_draw *draw = &core->paint_draws[i];
                draw->node = core->paint_items[i].node;
                draw->layer = core->paint_items[i].layer;
                draw->solid.clip = core->paint_items[i].clip;
                draw->decoration.clip = core->paint_items[i].clip;
                draw->scrollbar.clip = core->paint_items[i].clip;
                draw->image.clip = core->paint_items[i].clip;
                draw->glyph.clip = core->paint_items[i].clip;
                draw->image_src = wn_core_node_image_src(core, draw->node);
            }
        }
    }
    core->vertex_count = 0;
    core->solid_batch_count = 0;
    core->glyph_vertex_count = 0;
    core->glyph_batch_count = 0;
    core->image_vertex_count = 0;
    core->image_batch_count = 0;

    for (i = 0; i < core->paint_item_count; ++i) {
        const WnPaintItem *item = &core->paint_items[i];
        int node = item->node;
        const UiNode *n = &core->nodes[node];
        unsigned int color;
        unsigned int border_color;
        const wn_rect *r;
        wn_paint_draw *draw = wn_core_draw_for_item(core, i);
        int solid_first = core->vertex_count;
        wn_paint_use_item(core, item);
        r = &core->rects[node];
        if (((n->shadow_color >> 24) & 0xFFu) != 0u) {
            set_solid_clip(core, &item->clip);
            push_box_shadow(core, n, r);
        }
        color = wn_core_node_background(core, node);
        border_color = wn_core_node_border_color(core, node);
        if (color == 0) {
            if (draw != NULL) {
                paint_range_set(&draw->solid, solid_first, core->vertex_count,
                                item->clip);
            }
            continue;
        }
        set_solid_clip(core, &item->clip);
        if (n->border_width > 0.0f && ((border_color >> 24) & 0xFFu) != 0u &&
            ((color >> 24) & 0xFFu) == 0xFFu) {
            /* Opaque fill: border ring first, then the background inset by the
             * uniform border width. Children still paint over the border box
             * (v1 has no layout change). */
            float bw = n->border_width;
            float inner_w = r->w - bw * 2.0f;
            float inner_h = r->h - bw * 2.0f;
            float inner_r = n->border_radius - bw;
            push_box_shape(core, r->x, r->y, r->w, r->h,
                           n->border_radius, border_color);
            push_box_shape(core, r->x + bw, r->y + bw, inner_w, inner_h,
                           inner_r > 0.0f ? inner_r : 0.0f, color);
        } else {
            push_box_shape(core, r->x, r->y, r->w, r->h,
                           n->border_radius, color);
        }
        if (draw != NULL) {
            paint_range_set(&draw->solid, solid_first, core->vertex_count,
                            item->clip);
        }
    }
    build_input_decorations(core);
    build_scrollbars(core);
    for (i = 0; i < core->paint_item_count; ++i) {
        const WnPaintItem *item = &core->paint_items[i];
        wn_paint_draw *draw;
        int image_first = core->image_vertex_count;
        if (core->nodes[item->node].kind == R2_NODE_IMAGE) {
            wn_paint_use_item(core, item);
            push_image_quad(core, item->node, &item->clip);
        }
        draw = wn_core_draw_for_item(core, i);
        if (draw != NULL) {
            paint_range_set(&draw->image, image_first, core->image_vertex_count,
                            item->clip);
            draw->image_src = wn_core_node_image_src(core, item->node);
        }
    }
    build_text_geometry(core, (float)core->width, (float)core->height);
    build_input_text_geometry(core, (float)core->width, (float)core->height);
    wn_paint_use_item(core, NULL);

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
    if (layered) {
        core->list.layers = core->paint_layers;
        core->list.layer_count = core->paint_layer_count;
        core->list.commands = core->paint_commands;
        core->list.command_count = core->paint_command_count;
        core->list.draws = core->paint_draws;
        core->list.draw_count = core->paint_item_count;
    } else {
        core->list.layers = NULL;
        core->list.layer_count = 0;
        core->list.commands = NULL;
        core->list.command_count = 0;
        core->list.draws = NULL;
        core->list.draw_count = 0;
    }
    core->list.atlas.pixels = wn_text_atlas_pixels(core->text_atlas);
    core->list.atlas.width = wn_text_atlas_width(core->text_atlas);
    core->list.atlas.height = wn_text_atlas_height(core->text_atlas);
    core->list.atlas.size = core->list.atlas.width * core->list.atlas.height;
    core->list.atlas.serial = wn_text_atlas_serial(core->text_atlas);
    core->list.clear_color[0] = (float)((clear >> 16) & 0xFFu) / 255.0f;
    core->list.clear_color[1] = (float)((clear >> 8) & 0xFFu) / 255.0f;
    core->list.clear_color[2] = (float)(clear & 0xFFu) / 255.0f;
    core->list.clear_color[3] = (float)((clear >> 24) & 0xFFu) / 255.0f;
    core->list.width = core->width;
    core->list.height = core->height;
    core->dirty = 0;
}
