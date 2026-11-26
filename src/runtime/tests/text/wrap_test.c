/*
 * wrap_test.c - greedy line breaking on the core path: line count, node
 * height, explicit line-height, per-line text-align and the multi-baseline
 * display list.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/wn_core_internal.h"
#include "text/text_runtime.h"
#include "wn_core.h"

static int checks = 0;
static int failures = 0;

static void check(const char *label, int ok)
{
    checks += 1;
    if (!ok) {
        failures += 1;
    }
    printf("  %s %s\n", ok ? "[ok]" : "[FAIL]", label);
}

static int first_text_node(const wn_core *c)
{
    for (int node = 0; node < c->node_count; ++node) {
        if (c->alive[node] && c->nodes[node].kind == R2_NODE_TEXT) {
            return node;
        }
    }
    return -1;
}

static int count_glyphs_in_band(const wn_display_list *list, float y0, float y1)
{
    int count = 0;
    if (list == NULL) {
        return 0;
    }
    for (int i = 0; i + 5 < list->glyph_vertex_count; i += 6) {
        float y = (1.0f - list->glyph_vertices[i].y) * 0.5f * (float)list->height;
        if (y >= y0 && y <= y1) {
            count += 1;
        }
    }
    return count;
}

int main(void)
{
    const wn_document *document = wn_document_builtin();
    wn_core *core;
    const wn_text_line_layout *layout;
    const wn_text_line_layout *narrow;
    const wn_text_line_layout *plain;
    const wn_display_list *list;
    const wn_rect *rects;
    int panel;
    int template_node;
    int wrap;
    wn_rect rect;
    float expected_delta;
    float first_x[8];
    float first_y[8];
    int line_seen = 0;
    int distinct = 0;

    core = wn_core_create(document);
    check("core created", core != NULL);
    if (core == NULL) {
        printf("wrap_test: FAIL\n");
        return 1;
    }
    wn_core_resize(core, document->viewport_width, document->viewport_height);

    /* ------------------------------------------------ direct line breaking */
    layout = wn_text_line_layout_get("Supercalifragilisticexpialidocious", "Segoe UI",
                                     16.0f, 400, 60.0f);
    check("long word layout exists", layout != NULL);
    if (layout != NULL) {
        check("long word stays on one line", layout->line_count == 1);
        check("long word overflows instead of being cut", layout->lines[0].width > 60.0f);
    }

    narrow = wn_text_line_layout_get("abc   ", "Segoe UI", 16.0f, 400, 300.0f);
    plain = wn_text_line_layout_get("abc", "Segoe UI", 16.0f, 400, 300.0f);
    check("trailing spaces keep one line", narrow != NULL && narrow->line_count == 1);
    check("trailing spaces do not add width",
          narrow != NULL && plain != NULL &&
              fabsf(narrow->width - plain->width) < 0.01f);

    layout = wn_text_line_layout_get("", "Segoe UI", 16.0f, 400, 120.0f);
    check("empty string is one line", layout != NULL && layout->line_count == 1);
    if (layout != NULL) {
        check("empty string has no width", layout->width == 0.0f);
        check("empty string keeps a line box", layout->line_height > 0.0f &&
                                               layout->measured_height > 0.0f);
    }

    layout = wn_text_line_layout_get("one two three four five", "Segoe UI", 16.0f, 400, 70.0f);
    check("sentence wraps greedily", layout != NULL && layout->line_count >= 2);
    if (layout != NULL) {
        check("wrapped height is lines * line-height",
              fabsf(layout->height - (float)layout->line_count * layout->line_height) < 0.01f);
        for (int i = 0; i < layout->line_count; ++i) {
            check("each wrapped line fits (or is a single overflowing word)",
                  layout->lines[i].width <= 70.0f + 0.01f || layout->line_count == 1);
        }
    }

    /* ------------------------------------------- constrained node geometry */
    panel = wn_core_find_node(core, "div.panel");
    template_node = -1;
    {
        int title = wn_core_find_node(core, "div.title");
        if (title >= 0) {
            int child = wn_core_node_first_child(core, title);
            if (child >= 0 && wn_core_node_is_text(core, child)) {
                template_node = child;
            }
        }
    }
    if (template_node < 0) {
        template_node = first_text_node(core);
    }
    check("block panel found", panel >= 0);
    check("text template found", template_node >= 0);
    if (panel < 0 || template_node < 0) {
        wn_core_destroy(core);
        wn_text_runtime_shutdown();
        printf("wrap_test: FAIL\n");
        return 1;
    }
    wrap = wn_core_clone(core, template_node, panel);
    check("text clone created", wrap >= 0);
    if (wrap < 0) {
        wn_core_destroy(core);
        wn_text_runtime_shutdown();
        printf("wrap_test: FAIL\n");
        return 1;
    }
    core->nodes[wrap].width = 80.0f;
    core->nodes[wrap].height = -1.0f;
    core->nodes[wrap].font_size = 24.0f;
    core->nodes[wrap].font_family = "Segoe UI";
#if defined(UI_STYLE_EXTRAS)
    core->nodes[wrap].line_height = 28.0f;
    core->nodes[wrap].text_align = 1; /* center */
#endif
    check("clone text set", wn_core_set_text(core, wrap, "aaaaaaaa a") == 0);
    wn_core_relayout(core);

    layout = wn_text_line_layout_get("aaaaaaaa a", "Segoe UI", 24.0f, 400, 80.0f);
    check("constrained text wraps to two lines", layout != NULL && layout->line_count == 2);
    rects = wn_core_rects(core, NULL);
    rect = rects[wrap];
    printf("  wrapped node rect: %.2fx%.2f at %.2f,%.2f (lines=%d)\n",
           rect.w, rect.h, rect.x, rect.y, layout != NULL ? layout->line_count : -1);
    check("wrapped node keeps the declared width", fabsf(rect.w - 80.0f) < 0.01f);
#if defined(UI_STYLE_EXTRAS)
    check("wrapped node height is ceil(lines * line-height)",
          fabsf(rect.h - 56.0f) < 0.01f);
#else
    check("wrapped node height comes from the line box",
          layout != NULL && fabsf(rect.h - ceilf(layout->height)) < 0.01f);
#endif

    wn_core_build_display_list(core);
    list = wn_core_display_list(core);
    check("display list exists", list != NULL && list->glyph_vertex_count >= 12);
    if (list != NULL) {
        for (int i = 0; i + 5 < list->glyph_vertex_count; i += 6) {
            float y = (1.0f - list->glyph_vertices[i].y) * 0.5f * (float)list->height;
            float x = (list->glyph_vertices[i].x + 1.0f) * 0.5f * (float)list->width;
            int known = 0;
            /* Lines may be aligned outside the node box, so only the vertical
             * band identifies this wrapped node. */
            if (y < rect.y - 4.0f || y > rect.y + rect.h + 4.0f) {
                continue;
            }
            for (int k = 0; k < distinct; ++k) {
                if (fabsf(first_y[k] - y) < 2.0f) {
                    if (x < first_x[k]) {
                        first_x[k] = x;
                    }
                    known = 1;
                    break;
                }
            }
            if (!known && distinct < 8) {
                first_x[distinct] = x;
                first_y[distinct] = y;
                distinct += 1;
            }
        }
    }
    line_seen = distinct;
    check("display list paints multiple baselines", line_seen >= 2);
#if defined(UI_STYLE_EXTRAS)
    if (layout != NULL && line_seen >= 2) {
        /* Centered lines start at content_x + (avail - width)/2; both lines
         * start with the same glyph, so their x delta is half the width
         * difference of the two lines. */
        expected_delta = (layout->lines[0].width - layout->lines[1].width) * 0.5f;
        check("text-align is applied per line",
              fabsf((first_x[1] - first_x[0]) - expected_delta) < 2.5f);
    }
#endif

    /* -------------------------- runtime text override drives layout size */
    {
        float title_width;
        float short_width;
        float long_width;
        float restored_width;
        const char *compiled = core->nodes[template_node].text;
        rects = wn_core_rects(core, NULL);
        title_width = rects[template_node].w;
        check("set_text short", wn_core_set_text(core, template_node, "R") == 0);
        wn_core_relayout(core);
        rects = wn_core_rects(core, NULL);
        short_width = rects[template_node].w;
        check("shorter override shrinks the rect", short_width < title_width);
        check("set_text long",
              wn_core_set_text(core, template_node, "webnative runtime engine") == 0);
        wn_core_relayout(core);
        rects = wn_core_rects(core, NULL);
        long_width = rects[template_node].w;
        check("longer override grows the rect", long_width > short_width);
        check("set_text original", compiled != NULL && wn_core_set_text(core, template_node,
                                                                        compiled) == 0);
        wn_core_relayout(core);
        rects = wn_core_rects(core, NULL);
        restored_width = rects[template_node].w;
        check("original string keeps the original width",
              fabsf(restored_width - title_width) < 0.01f);
    }

    /* Override + wrapped size + display list in one pass. */
    check("wrapped clone set short", wn_core_set_text(core, wrap, "bbb") == 0);
    wn_core_relayout(core);
    wn_core_build_display_list(core);
    rects = wn_core_rects(core, NULL);
    rect = rects[wrap];
    list = wn_core_display_list(core);
    check("short override collapses the wrapped node", fabsf(rect.h - 28.0f) < 0.01f);
    check("short override paints the new glyph count",
          count_glyphs_in_band(list, rect.y - 4.0f, rect.y + rect.h + 4.0f) == 3);
    check("wrapped clone restored", wn_core_set_text(core, wrap, "aaaaaaaa a") == 0);
    wn_core_relayout(core);
    wn_core_build_display_list(core);
    rects = wn_core_rects(core, NULL);
    rect = rects[wrap];
    list = wn_core_display_list(core);
    check("restored override wraps again", fabsf(rect.h - 56.0f) < 0.01f);
    check("restored override paints the wrapped glyph count",
          count_glyphs_in_band(list, rect.y - 4.0f, rect.y + rect.h + 4.0f) == 9);

    /* Constrained geometry must follow the engine breaker, not the backend
     * measure hint, at the same widths the measurement test probes. */
    {
        static const float widths[] = {30.0f, 60.0f, 98.0f};
        core->nodes[wrap].font_size = 16.0f;
        core->nodes[wrap].width = widths[0];
#if defined(UI_STYLE_EXTRAS)
        core->nodes[wrap].line_height = 0.0f;
        core->nodes[wrap].text_align = 0;
#endif
        check("breaker text set", wn_core_set_text(core, wrap, "one two three") == 0);
        for (int i = 0; i < 3; ++i) {
            const wn_text_line_layout *engine;
            float expected_h;
            core->nodes[wrap].width = widths[i];
            wn_core_relayout(core);
            rects = wn_core_rects(core, NULL);
            rect = rects[wrap];
            engine = wn_text_line_layout_get("one two three", "Segoe UI", 16.0f, 400,
                                             widths[i]);
            expected_h = engine != NULL
                             ? ceilf((float)engine->line_count * engine->line_height)
                             : -1.0f;
            printf("  breaker rect %.0fpx: lines=%d height=%.2f expected=%.2f\n",
                   widths[i], engine != NULL ? engine->line_count : -1, rect.h, expected_h);
            check("rect height matches the engine breaker",
                  engine != NULL && fabsf(rect.h - expected_h) < 0.01f);
        }
    }

    wn_core_destroy(core);
    wn_text_runtime_shutdown();
    if (failures != 0) {
        printf("wrap_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }
    printf("wrap_test: PASS (%d checks)\n", checks);
    return 0;
}
