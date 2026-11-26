/*
 * text_block_test.c - one derived line block for layout, paint and input.
 *
 * Runs against fake_text.c so every number is exact:
 *   - 8px advance per codepoint, 20px line box for 16px text.
 *
 * What is locked here:
 *   1. wn_text_block_get computes line count, natural/line height, box height,
 *      half leading and ascent/descent from the cached line layout, including
 *      a positive line-height override.
 *   2. Layout auto height is exactly the block box height for a wrapped TEXT
 *      node, and the block measured height for an unconstrained single line.
 *      A constrained node always follows the breaker, even when the backend
 *      reports a taller measured box.
 *   3. Paint advances each line by the block line height and vertically centers
 *      the block (node leading + half leading) inside the node box.
 *   4. INPUT auto height is padding + the single-line block line height, and
 *      keeps the legacy padding + 1.35*size fallback when the backend measure
 *      fails even though the line build succeeds.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "document/wn_document_internal.h"
#include "text/text_runtime.h"
#include "wn_core.h"

/* Provided by fake_text.c. */
const wn_text_ops *wn_text_fake(void);
const wn_text_ops *wn_text_fake_measured(void);
const wn_text_ops *wn_text_fake_broken_measure(void);

#define FAKE "fake"
#define FAKE_SIZE 16.0f
#define FAKE_LINE (FAKE_SIZE * 1.25f) /* 20 px */
#define VIEWPORT_W 320.0f
#define VIEWPORT_H 200.0f

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

static int near(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

/* Two-node document: a block root and one TEXT or INPUT child. The core copies
 * the nodes, so the same staging arrays serve every scene. */
static wn_document g_doc;
static UiNode g_nodes[2];
static const char *const g_names[2] = {"body.root", "body.root/span.leaf"};

static wn_core *make_leaf_core(int kind, const char *text, float width, float height,
                               float line_height, float pad)
{
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(&g_doc, 0, sizeof(g_doc));

    g_nodes[0].parent = -1;
    g_nodes[0].kind = R2_NODE_BLOCK;
    g_nodes[0].width = -1.0f;
    g_nodes[0].height = -1.0f;
    g_nodes[0].flex_shrink = 1.0f;
    g_nodes[0].opacity = 1.0f;

    g_nodes[1].parent = 0;
    g_nodes[1].kind = kind;
    g_nodes[1].text = text;
    g_nodes[1].font_family = FAKE;
    g_nodes[1].font_size = FAKE_SIZE;
    g_nodes[1].font_weight = 400;
    g_nodes[1].text_color = 0xFF000000u;
    g_nodes[1].width = width;
    g_nodes[1].height = height;
    g_nodes[1].line_height = line_height;
    g_nodes[1].padding[0] = pad;
    g_nodes[1].padding[1] = pad;
    g_nodes[1].padding[2] = pad;
    g_nodes[1].padding[3] = pad;
    g_nodes[1].flex_shrink = 1.0f;
    g_nodes[1].opacity = 1.0f;

    g_doc.nodes = g_nodes;
    g_doc.node_count = 2;
    g_doc.names = g_names;
    g_doc.viewport_width = (int)VIEWPORT_W;
    g_doc.viewport_height = (int)VIEWPORT_H;
    g_doc.refs = WN_DOCUMENT_STATIC_REFS;

    return wn_core_create(&g_doc);
}

/* Display-list y back to viewport pixels (push_vertex stores clip space). */
static float glyph_pixel_y(const wn_display_list *list, int vertex)
{
    return (1.0f - list->glyph_vertices[vertex].y) * 0.5f * (float)list->height;
}

int main(void)
{
    wn_text_block block;
    wn_text_block overridden;
    wn_text_block measured_block;
    wn_core *core;
    int ok;

    wn_text_test_use_backend(wn_text_fake());

    /* ------------------------------------------- 1. derived block metrics */
    memset(&block, 0xFF, sizeof(block));
    ok = wn_text_block_get("aa bb cc", FAKE, FAKE_SIZE, 400, 45.0f, 0.0f, &block) == 0;
    check("block builds from a wrapped layout", ok);
    check("wrapped block has two lines", block.line_count == 2);
    check("wrapped block keeps the natural line box", near(block.natural, FAKE_LINE));
    check("wrapped block line height defaults to natural", near(block.line_height, FAKE_LINE));
    check("wrapped block box is lines * line height", near(block.box_height, 40.0f));
    check("wrapped block has no leading without an override", near(block.half_leading, 0.0f));
    check("wrapped block exposes the layout ascent", near(block.ascent, FAKE_SIZE));
    check("wrapped block exposes the layout descent", near(block.descent, FAKE_SIZE * 0.25f));
    check("wrapped block keeps the backend measured height",
          near(block.measured_height, FAKE_LINE));
    check("wrapped block reports backend metrics", block.measured != 0);
    check("wrapped block points at the cached layout",
          block.layout != NULL && block.layout->line_count == 2 &&
              near(block.layout->width, 40.0f));

    ok = wn_text_block_get("aa bb cc", FAKE, FAKE_SIZE, 400, 45.0f, 30.0f, &overridden) == 0;
    check("overridden block builds", ok);
    check("override replaces the line height", near(overridden.line_height, 30.0f));
    check("override scales the box", near(overridden.box_height, 60.0f));
    check("override leading is half the difference", near(overridden.half_leading, 5.0f));
    check("override leaves the natural line box alone", near(overridden.natural, FAKE_LINE));
    check("override leaves the measured height alone",
          near(overridden.measured_height, FAKE_LINE));

    ok = wn_text_block_get("aa bb", FAKE, FAKE_SIZE, 400, 0.0f, 0.0f, &block) == 0;
    check("unconstrained single line builds", ok && block.line_count == 1);
    check("unconstrained single line box is one line", near(block.box_height, FAKE_LINE));

    memset(&block, 0xFF, sizeof(block));
    wn_text_test_fail_next_lines(1);
    check("a failed block get zeroes the output",
          wn_text_block_get("zero-check", FAKE, FAKE_SIZE, 400, 0.0f, 0.0f, &block) != 0 &&
              block.layout == NULL && block.line_count == 0 && block.natural == 0.0f &&
              block.box_height == 0.0f && block.measured_height == 0.0f &&
              !block.measured);

    /* ------------------------------- 2. layout consumes the same block */
    core = make_leaf_core(R2_NODE_TEXT, "aa bb cc", 45.0f, -1.0f, 0.0f, 0.0f);
    check("wrapped layout core created", core != NULL);
    if (core != NULL) {
        const wn_rect *rects = wn_core_rects(core, NULL);
        wn_text_block layout_block;
        check("text node wraps at the declared width", near(rects[1].w, 45.0f));
        check("wrapped text auto height equals the block box",
              wn_text_block_get("aa bb cc", FAKE, FAKE_SIZE, 400, 45.0f, 0.0f,
                                &layout_block) == 0 &&
                  near(rects[1].h, layout_block.box_height) &&
                  near(rects[1].h, 40.0f));
        wn_core_destroy(core);
    }

    core = make_leaf_core(R2_NODE_TEXT, "aa bb cc", 45.0f, -1.0f, 30.0f, 0.0f);
    check("override layout core created", core != NULL);
    if (core != NULL) {
        const wn_rect *rects = wn_core_rects(core, NULL);
        check("line-height override drives the wrapped auto height",
              near(rects[1].h, 60.0f));
        wn_core_destroy(core);
    }

    /* ------------------------------- 3. paint places lines with the block */
    core = make_leaf_core(R2_NODE_TEXT, "aa bb cc", 45.0f, -1.0f, 0.0f, 0.0f);
    check("paint core created", core != NULL);
    if (core != NULL) {
        const wn_display_list *list;
        wn_core_build_display_list(core);
        list = wn_core_display_list(core);
        check("paint emits both lines (7 glyphs)",
              list != NULL && list->glyph_vertex_count == 7 * 6);
        if (list != NULL && list->glyph_vertex_count == 7 * 6) {
            float first = glyph_pixel_y(list, 0);
            float second = glyph_pixel_y(list, 5 * 6);
            wn_text_block paint_block;
            check("paint spacing equals the block line height",
                  wn_text_block_get("aa bb cc", FAKE, FAKE_SIZE, 400, 45.0f, 0.0f,
                                    &paint_block) == 0 &&
                      near(second - first, paint_block.line_height) &&
                      near(second - first, 20.0f));
        }
        wn_core_destroy(core);
    }

    core = make_leaf_core(R2_NODE_TEXT, "aa bb cc", 45.0f, -1.0f, 30.0f, 0.0f);
    check("override paint core created", core != NULL);
    if (core != NULL) {
        const wn_display_list *list;
        wn_core_build_display_list(core);
        list = wn_core_display_list(core);
        if (list != NULL && list->glyph_vertex_count == 7 * 6) {
            float first = glyph_pixel_y(list, 0);
            float second = glyph_pixel_y(list, 5 * 6);
            check("paint spacing follows the overridden block line height",
                  near(second - first, 30.0f));
        } else {
            check("paint spacing follows the overridden block line height", 0);
        }
        wn_core_destroy(core);
    }

    /* Vertical centering: a 100px node around a 40px block gets 30px above. */
    core = make_leaf_core(R2_NODE_TEXT, "aa bb cc", 45.0f, 100.0f, 0.0f, 0.0f);
    check("centering core created", core != NULL);
    if (core != NULL) {
        const wn_rect *rects = wn_core_rects(core, NULL);
        const wn_display_list *list;
        wn_core_build_display_list(core);
        list = wn_core_display_list(core);
        check("centering keeps the declared box", near(rects[1].h, 100.0f));
        if (list != NULL && list->glyph_vertex_count == 7 * 6) {
            check("the block is centered inside the node box",
                  near(glyph_pixel_y(list, 0), rects[1].y + 30.0f));
        } else {
            check("the block is centered inside the node box", 0);
        }
        wn_core_destroy(core);
    }

    core = make_leaf_core(R2_NODE_TEXT, "aa bb cc", 45.0f, 100.0f, 30.0f, 0.0f);
    check("override centering core created", core != NULL);
    if (core != NULL) {
        const wn_rect *rects = wn_core_rects(core, NULL);
        const wn_display_list *list;
        wn_core_build_display_list(core);
        list = wn_core_display_list(core);
        if (list != NULL && list->glyph_vertex_count == 7 * 6) {
            /* box 60 -> 20px above, half leading 5 -> first baseline +25px. */
            check("override centering uses half leading in the baseline",
                  near(glyph_pixel_y(list, 0), rects[1].y + 25.0f));
        } else {
            check("override centering uses half leading in the baseline", 0);
        }
        wn_core_destroy(core);
    }

    /* ------------------- 4. unconstrained single line keeps measured height */
    wn_text_test_use_backend(wn_text_fake_measured());
    ok = wn_text_block_get("aa bb", FAKE, FAKE_SIZE, 400, 0.0f, 0.0f, &measured_block) == 0;
    check("measured backend block builds", ok && measured_block.line_count == 1);
    check("measured backend reports a taller measured box",
          near(measured_block.measured_height, FAKE_SIZE * 1.4f) &&
              near(measured_block.natural, FAKE_LINE));
    check("measured backend reports backend metrics", measured_block.measured != 0);
    check("measured backend keeps the line box as block height",
          near(measured_block.box_height, FAKE_LINE));

    core = make_leaf_core(R2_NODE_TEXT, "aa bb", 0.0f, -1.0f, 0.0f, 0.0f);
    check("unconstrained core created", core != NULL);
    if (core != NULL) {
        const wn_rect *rects = wn_core_rects(core, NULL);
        check("unconstrained auto height is the measured height",
              near(rects[1].h, ceilf(measured_block.measured_height)) &&
                  near(rects[1].h, 23.0f));
        wn_core_destroy(core);
    }

    core = make_leaf_core(R2_NODE_TEXT, "aa bb", 45.0f, -1.0f, 0.0f, 0.0f);
    check("constrained measured core created", core != NULL);
    if (core != NULL) {
        const wn_rect *rects = wn_core_rects(core, NULL);
        wn_text_block constrained;
        check("a constrained single line still follows the breaker",
              wn_text_block_get("aa bb", FAKE, FAKE_SIZE, 400, 45.0f, 0.0f,
                                &constrained) == 0 &&
                  constrained.line_count == 1 &&
                  near(rects[1].h, constrained.box_height) && near(rects[1].h, 20.0f));
        wn_core_destroy(core);
    }

    /* ------------------------------- 5. input shares the single-line block */
    wn_text_test_use_backend(wn_text_fake());
    core = make_leaf_core(R2_NODE_INPUT, "", 120.0f, -1.0f, 0.0f, 6.0f);
    check("input layout core created", core != NULL);
    if (core != NULL) {
        const wn_rect *rects = wn_core_rects(core, NULL);
        wn_text_block editor;
        check("input editor block builds",
              wn_text_block_get("Mg", FAKE, FAKE_SIZE, 400, 0.0f, 0.0f, &editor) == 0);
        check("input editor block reports backend metrics", editor.measured != 0);
        check("input auto height is padding plus the block line height",
              near(rects[1].h, 12.0f + editor.line_height) && near(rects[1].h, 32.0f));
        wn_core_destroy(core);
    }

    /* ----------- 6. input keeps the 1.35 fallback when measure breaks */
    wn_text_test_use_backend(wn_text_fake_broken_measure());
    core = make_leaf_core(R2_NODE_INPUT, "", 120.0f, -1.0f, 0.0f, 6.0f);
    check("broken-measure input core created", core != NULL);
    if (core != NULL) {
        const wn_rect *rects = wn_core_rects(core, NULL);
        wn_text_block editor;
        check("broken-measure line still builds from the breaker",
              wn_text_block_get("Mg", FAKE, FAKE_SIZE, 400, 0.0f, 0.0f, &editor) == 0 &&
                  editor.line_count == 1);
        check("broken-measure block reports no backend metrics",
              editor.measured == 0 && near(editor.natural, FAKE_SIZE * 1.2f));
        check("broken-measure input auto height is padding plus 1.35 * size",
              near(rects[1].h, 12.0f + FAKE_SIZE * 1.35f) && near(rects[1].h, 33.6f));
        wn_core_destroy(core);
    }

    if (failures != 0) {
        printf("text_block_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }
    printf("text_block_test: PASS (%d checks)\n", checks);
    return 0;
}
