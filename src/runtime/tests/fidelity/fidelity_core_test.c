/*
 * fidelity_core_test.c - headless acceptance for the E1 visual/layout gaps:
 *
 *   1. real font-weight 700 (Segoe UI Bold) vs 400,
 *   2. text-align left/center/right shifting the run inside the block,
 *   3. line-height driving the text box height and the baseline,
 *   4. margin:auto (flex column bottom pin + horizontal centring in a block),
 *   5. visible scrollbar (track + thumb) and thumb movement on wheel,
 *   6. rounded focus ring for a text input with border-radius.
 *
 * Compiles against tests/fidelity/out/emit/ui_data.h and the core only (no GPU,
 * no window). All checks are deterministic geometry/vertex invariants.
 *
 *   fidelity_core_test.exe
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_data.h"
#include "wn_core.h"

#ifndef UI_STYLE_EXTRAS
#error "fidelity fixture must be emitted with text-align/line-height/margin:auto (UI_STYLE_EXTRAS)"
#endif

static wn_core *g_core;
static int g_checks;
static int g_failures;

static int check(const char *name, int ok, const char *detail)
{
    ++g_checks;
    printf("[%s] fidelity: %s - %s\n", ok ? "ok  " : "FAIL", name, detail);
    if (!ok) {
        ++g_failures;
    }
    return ok;
}

static wn_rect rect_of(int node)
{
    return wn_core_rects(g_core, NULL)[node];
}

static int find(const char *name)
{
    int node = wn_core_find_node(g_core, name);
    if (node < 0) {
        printf("FAIL: node not found: %s\n", name);
        ++g_failures;
        ++g_checks;
    }
    return node;
}

/* Display-list vertices live in clip space; convert to view pixels. */
static float px_x(const wn_vertex *v)
{
    return (v->x + 1.0f) * 0.5f * (float)UI_VIEWPORT_WIDTH;
}

static float px_y(const wn_vertex *v)
{
    return (1.0f - v->y) * 0.5f * (float)UI_VIEWPORT_HEIGHT;
}

static float gpx_x(const wn_glyph_vertex *v)
{
    return (v->x + 1.0f) * 0.5f * (float)UI_VIEWPORT_WIDTH;
}

static float gpx_y(const wn_glyph_vertex *v)
{
    return (1.0f - v->y) * 0.5f * (float)UI_VIEWPORT_HEIGHT;
}

/* Ink bounding box (glyph quad corners) inside a row rect. */
static int ink_bbox(wn_rect row, float *x0, float *x1, float *y0, float *y1)
{
    const wn_display_list *list = wn_core_display_list(g_core);
    int count = 0;
    int i;
    *x0 = 1e9f;
    *x1 = -1e9f;
    *y0 = 1e9f;
    *y1 = -1e9f;
    if (list == NULL) {
        return 0;
    }
    for (i = 0; i < list->glyph_vertex_count; ++i) {
        const wn_glyph_vertex *v = &list->glyph_vertices[i];
        float x = gpx_x(v);
        float y = gpx_y(v);
        if (x < row.x - 1.0f || x > row.x + row.w + 1.0f ||
            y < row.y || y > row.y + row.h) {
            continue;
        }
        if (x < *x0) *x0 = x;
        if (x > *x1) *x1 = x;
        if (y < *y0) *y0 = y;
        if (y > *y1) *y1 = y;
        ++count;
    }
    return count;
}

/* Number of solid vertices with an exact RGBA color; optional min/max y. */
static int count_solid(unsigned int r, unsigned int g, unsigned int b,
                       unsigned int a, float *min_y, float *max_y)
{
    const wn_display_list *list = wn_core_display_list(g_core);
    int count = 0;
    int i;
    if (min_y != NULL) *min_y = 1e9f;
    if (max_y != NULL) *max_y = -1e9f;
    if (list == NULL) {
        return 0;
    }
    for (i = 0; i < list->vertex_count; ++i) {
        const wn_vertex *v = &list->vertices[i];
        float y;
        if (v->r != r || v->g != g || v->b != b || v->a != a) {
            continue;
        }
        ++count;
        y = px_y(v);
        if (min_y != NULL && y < *min_y) *min_y = y;
        if (max_y != NULL && y > *max_y) *max_y = y;
    }
    return count;
}

/* Focus ring: no vertex sits exactly on the square corner. */
static int ring_has_corner(float x, float y)
{
    const wn_display_list *list = wn_core_display_list(g_core);
    int i;
    if (list == NULL) {
        return 1;
    }
    for (i = 0; i < list->vertex_count; ++i) {
        const wn_vertex *v = &list->vertices[i];
        if (v->r == 0x3Bu && v->g == 0x82u && v->b == 0xF6u && v->a == 0xFFu &&
            fabsf(px_x(v) - x) < 0.6f && fabsf(px_y(v) - y) < 0.6f) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    int regular, bold, left, center, right, line, pin, foot, autobox, child;
    int pinmid = -1, rbar = -1, rchild = -1;
    int scroll, input_node;
    float x0, x1, y0, y1;
    float cx, cy;
    char detail[256];
    int before_count, after_count;
    float before_y, after_y;

    g_core = wn_core_create(wn_document_builtin());
    if (g_core == NULL) {
        printf("FAIL: wn_core_create\n");
        return 1;
    }
    wn_core_resize(g_core, UI_VIEWPORT_WIDTH, UI_VIEWPORT_HEIGHT);
    wn_core_relayout(g_core);
    wn_core_build_display_list(g_core);

    /* ------------------------------------------------ 1. real font-weight */
    regular = find("body.root/div#regular.row/text");
    bold = find("body.root/div#bold.row/text[1]");
    if (regular >= 0 && bold >= 0) {
        wn_rect r = rect_of(regular);
        wn_rect b = rect_of(bold);
        snprintf(detail, sizeof(detail), "regular=%.0fx%.0f bold=%.0fx%.0f",
                 r.w, r.h, b.w, b.h);
        check("bold variant is wider than regular", b.w >= r.w + 2.0f, detail);
        check("bold keeps the same line box height", fabsf(b.h - r.h) < 0.01f,
              detail);
    }

    /* ------------------------------------------------------ 2. text-align */
    left = find("body.root/div#left.row");
    center = find("body.root/div#center.row");
    right = find("body.root/div#right.row");
    if (left >= 0 && center >= 0 && right >= 0) {
        wn_rect lr = rect_of(left);
        wn_rect cr = rect_of(center);
        wn_rect rr = rect_of(right);
        const wn_display_list *list = wn_core_display_list(g_core);
        check("text-align fields reached the C tree",
              list != NULL && kUiNodes[center].text_align == 1 &&
              kUiNodes[right].text_align == 2,
              "center node=1, right node=2");
        if (ink_bbox(lr, &x0, &x1, &y0, &y1) > 0) {
            cx = (x0 + x1) * 0.5f;
            snprintf(detail, sizeof(detail), "left ink center=%.1f row center=%.1f",
                     cx, lr.x + lr.w * 0.5f);
            check("left text starts at the block edge", x0 < lr.x + 6.0f, detail);
        }
        if (ink_bbox(cr, &x0, &x1, &y0, &y1) > 0) {
            cx = (x0 + x1) * 0.5f;
            snprintf(detail, sizeof(detail), "center ink center=%.1f row center=%.1f",
                     cx, cr.x + cr.w * 0.5f);
            check("center text is centred in the block",
                  fabsf(cx - (cr.x + cr.w * 0.5f)) <= 4.0f, detail);
        }
        if (ink_bbox(rr, &x0, &x1, &y0, &y1) > 0) {
            snprintf(detail, sizeof(detail), "right ink right=%.1f row right=%.1f",
                     x1, rr.x + rr.w);
            check("right text ends at the block edge", x1 > rr.x + rr.w - 6.0f,
                  detail);
        }
    }

    /* ---------------------------------------------------- 3. line-height */
    line = find("body.root/div#line.row");
    if (line >= 0) {
        wn_rect row = rect_of(line);
        int text = find("body.root/div#line.row/text[5]");
        if (text >= 0) {
            wn_rect t = rect_of(text);
            snprintf(detail, sizeof(detail), "text box h=%.1f row h=%.1f",
                     t.h, row.h);
            check("line-height sets the text box height", fabsf(t.h - 40.0f) < 0.01f,
                  detail);
            check("line-height sets the block height", fabsf(row.h - 40.0f) < 0.01f,
                  detail);
            if (ink_bbox(row, &x0, &x1, &y0, &y1) > 0) {
                cy = (y0 + y1) * 0.5f;
                snprintf(detail, sizeof(detail), "ink y=[%.1f,%.1f] center=%.1f",
                         y0, y1, cy);
                check("glyphs stay inside the line box",
                      y0 > row.y + 1.0f && y1 < row.y + row.h - 1.0f, detail);
                check("half-leading centres the glyphs vertically",
                      fabsf(cy - (row.y + 20.0f)) <= 6.0f, detail);
            }
        }
        check("line-height is exported on the text node",
              kUiNodes[line].line_height > 0.0f ||
              (line + 1 < UI_NODE_COUNT &&
               kUiNodes[line + 1].line_height > 0.0f),
              "line_height > 0");
    }

    /* ----------------------------------------------------- 4. margin:auto */
    pin = find("body.root/div#pin.pin");
    foot = find("body.root/div#pin.pin/div#pinfoot.pinfoot");
    if (pin >= 0 && foot >= 0) {
        wn_rect p = rect_of(pin);
        wn_rect f = rect_of(foot);
        snprintf(detail, sizeof(detail), "foot y=%.1f pin bottom=%.1f",
                 f.y, p.y + p.h);
        check("margin-top:auto pins the flex footer to the bottom",
              fabsf(f.y + f.h - (p.y + p.h)) < 0.01f, detail);
    }
    pinmid = find("body.root/div#pin.pin/div#pinmid.pinmid");
    if (pin >= 0 && pinmid >= 0) {
        wn_rect p = rect_of(pin);
        wn_rect m = rect_of(pinmid);
        snprintf(detail, sizeof(detail), "mid x=%.1f expected=%.1f",
                 m.x, p.x + (p.w - m.w) * 0.5f);
        check("horizontal auto margins centre a flex column item",
              fabsf(m.x - (p.x + (p.w - m.w) * 0.5f)) < 0.01f, detail);
    }
    rbar = find("body.root/div#rbar.rbar");
    rchild = find("body.root/div#rbar.rbar/div#rchild.rchild");
    if (rbar >= 0 && rchild >= 0) {
        wn_rect b = rect_of(rbar);
        wn_rect c = rect_of(rchild);
        snprintf(detail, sizeof(detail), "child right=%.1f bar right=%.1f",
                 c.x + c.w, b.x + b.w);
        check("margin-left:auto pushes a flex row item to the end",
              fabsf(c.x + c.w - (b.x + b.w)) < 0.01f, detail);
    }
    autobox = find("body.root/div#autobox.autobox");
    child = find("body.root/div#autobox.autobox/div#autochild.autochild");
    if (autobox >= 0 && child >= 0) {
        wn_rect a = rect_of(autobox);
        wn_rect c = rect_of(child);
        snprintf(detail, sizeof(detail), "child x=%.1f expected=%.1f",
                 c.x, a.x + (a.w - c.w) * 0.5f);
        check("horizontal auto margins centre a block child",
              fabsf(c.x - (a.x + (a.w - c.w) * 0.5f)) < 0.01f, detail);
    }

    /* ------------------------------------------------------- 5. scrollbar */
    scroll = find("body.root/div#scroll.scrollbox");
    if (scroll >= 0) {
        wn_rect s = rect_of(scroll);
        before_count = count_solid(0x5Au, 0x66u, 0x7Au, 0x66u, &before_y, NULL);
        snprintf(detail, sizeof(detail), "thumb vertices=%d first y=%.1f",
                 before_count, before_y);
        check("scrollbar thumb is drawn (rounded => 108 vertices)",
              before_count == 108, detail);
        before_count = count_solid(0x00u, 0x00u, 0x00u, 0x33u, NULL, NULL);
        snprintf(detail, sizeof(detail), "track vertices=%d", before_count);
        check("scrollbar track is drawn (rounded => 108 vertices)",
              before_count == 108, detail);
        {
            int wheel = wn_core_wheel(g_core, s.x + 40.0f, s.y + 30.0f, 0.0f, 150.0f);
            float offset;
            check("wheel reaches the overflow container", wheel == scroll,
                  wheel == scroll ? "scroll node returned" : "not returned");
            wn_core_build_display_list(g_core);
            offset = wn_core_scroll_offset(g_core, scroll);
            snprintf(detail, sizeof(detail), "offset=%.1f", offset);
            check("wheel scrolls the overflow content", fabsf(offset - 150.0f) < 0.01f,
                  detail);
            after_count = count_solid(0x5Au, 0x66u, 0x7Au, 0x66u, &after_y, NULL);
            snprintf(detail, sizeof(detail), "thumb y %.1f -> %.1f, vertices=%d",
                     before_y, after_y, after_count);
            check("thumb follows the scroll offset",
                  after_count == 108 && after_y > before_y + 20.0f, detail);
        }
    }

    /* --------------------------------------------------- 6. rounded ring */
    input_node = find("body.root/input#search.field");
    if (input_node >= 0) {
        wn_rect r = rect_of(input_node);
        wn_core_focus_input(g_core, input_node, r.x + 10.0f);
        check("input focused", wn_core_input_focused(g_core) == input_node,
              "input_focus == input#search");
        wn_core_build_display_list(g_core);
        before_count = count_solid(0x3Bu, 0x82u, 0xF6u, 0xFFu, NULL, NULL);
        snprintf(detail, sizeof(detail), "focus ring vertices=%d", before_count);
        check("rounded focus ring (16 segments per corner => 216 vertices)",
              before_count == 216, detail);
        check("focus ring has no square corner vertex",
              !ring_has_corner(r.x, r.y) && !ring_has_corner(r.x + r.w, r.y) &&
              !ring_has_corner(r.x, r.y + r.h) &&
              !ring_has_corner(r.x + r.w, r.y + r.h),
              "corners are arcs, not the border-box extreme");
    }

    wn_core_destroy(g_core);
    printf("fidelity_core_test: %s (%d checks)\n",
           g_failures == 0 ? "PASS" : "FAIL", g_checks);
    return g_failures == 0 ? 0 : 1;
}
