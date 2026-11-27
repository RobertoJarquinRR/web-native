/*
 * style_override_test.c - ABI v7 runtime style overrides.
 *
 * Runs on the deterministic family/size/weight-sensitive fake text backend
 * (no DirectWrite, no GPU):
 *   - wn_core_set_color changes the painted glyph color, inherits from a
 *     container and can be reset per node or on the ancestor;
 *   - wn_core_set_background changes the solid pass of a static node and can be
 *     reset;
 *   - font family/size/weight overrides change text measurement/layout, mark
 *     layout dirty, inherit from the container and restore their compiled
 *     metrics;
 *   - the app-wide recursive family layer reaches descendants and leaves an
 *     image box untouched;
 *   - a clone copies the style override and its reset falls back to the
 *     clone's compiled values;
 *   - invalid values, dead handles and unknown reset masks change nothing.
 *
 *   style_override_test.exe
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "document/wn_blob.h"
#include "document/wn_document_format.h"
#include "document/wn_document_internal.h"
#include "text/text_runtime.h"
#include "wn_core.h"

const wn_text_ops *wn_text_fake_style(void);

static int checks = 0;
static int failures = 0;

static int check(const char *label, int ok, const char *detail)
{
    checks += 1;
    if (!ok) {
        failures += 1;
    }
    printf("  %s %s%s%s\n", ok ? "[ok]" : "[FAIL]", label,
           detail != NULL ? " - " : "", detail != NULL ? detail : "");
    return ok;
}

enum {
    kRoot = 0,
    kContainer = 1,
    kTextA = 2,
    kTextB = 3,
    kImage = 4,
    kPlain = 5,
    kCount = 6
};

static wn_document g_doc;
static UiNode g_nodes[kCount];
static const char *const g_names[kCount] = {
    "body.root",
    "body.root/div.tabs",
    "body.root/div.tabs/text[1]",
    "body.root/div.tabs/text[2]",
    "body.root/img.logo",
    "body.root/div.panel"
};

static wn_core *make_core(void)
{
    int i;
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(&g_doc, 0, sizeof(g_doc));

    g_nodes[kRoot].parent = -1;
    g_nodes[kRoot].kind = R2_NODE_BLOCK;
    g_nodes[kRoot].width = 400.0f;
    g_nodes[kRoot].height = 200.0f;
    g_nodes[kRoot].background = 0xFF010203u;

    g_nodes[kContainer].parent = kRoot;
    g_nodes[kContainer].kind = R2_NODE_BLOCK;
    g_nodes[kContainer].width = 300.0f;
    g_nodes[kContainer].height = 60.0f;

    g_nodes[kTextA].parent = kContainer;
    g_nodes[kTextA].kind = R2_NODE_TEXT;
    g_nodes[kTextA].text = "MMMM";
    g_nodes[kTextA].width = -1.0f;
    g_nodes[kTextA].height = 20.0f;
    g_nodes[kTextA].font_family = "fake";
    g_nodes[kTextA].font_size = 16.0f;
    g_nodes[kTextA].font_weight = 400;
    g_nodes[kTextA].text_color = 0xFF111111u;

    g_nodes[kTextB].parent = kContainer;
    g_nodes[kTextB].kind = R2_NODE_TEXT;
    g_nodes[kTextB].text = "II";
    g_nodes[kTextB].width = -1.0f;
    g_nodes[kTextB].height = 20.0f;
    g_nodes[kTextB].font_family = "fake";
    g_nodes[kTextB].font_size = 16.0f;
    g_nodes[kTextB].font_weight = 400;
    g_nodes[kTextB].text_color = 0xFF111111u;

    g_nodes[kImage].parent = kRoot;
    g_nodes[kImage].kind = R2_NODE_IMAGE;
    g_nodes[kImage].width = 40.0f;
    g_nodes[kImage].height = 30.0f;

    /* A static box: style overrides are geometry-independent, so this is a
     * valid target (unlike wn_node_set_position). */
    g_nodes[kPlain].parent = kRoot;
    g_nodes[kPlain].kind = R2_NODE_BLOCK;
    g_nodes[kPlain].width = 50.0f;
    g_nodes[kPlain].height = 20.0f;

    for (i = 0; i < kCount; ++i) {
        g_nodes[i].font_size = g_nodes[i].font_size > 0.0f
                                   ? g_nodes[i].font_size
                                   : 16.0f;
        g_nodes[i].flex_shrink = 1.0f;
        g_nodes[i].flex_basis = -1.0f;
        g_nodes[i].opacity = 1.0f;
        g_nodes[i].grid_auto_rows = -1.0f;
        g_nodes[i].z_index = WN_BLOB_Z_INDEX_AUTO;
    }

    g_doc.nodes = g_nodes;
    g_doc.node_count = kCount;
    g_doc.names = g_names;
    g_doc.viewport_width = 400;
    g_doc.viewport_height = 200;
    g_doc.refs = WN_DOCUMENT_STATIC_REFS;
    return wn_core_create(&g_doc);
}

static wn_rect rect_of(wn_core *core, int node)
{
    wn_rect rect;
    memset(&rect, 0, sizeof(rect));
    if (wn_core_node_rect(core, node, &rect) != 0) {
        rect.x = rect.y = rect.w = rect.h = -1.0f;
    }
    return rect;
}

static int rects_equal(wn_rect a, wn_rect b)
{
    return fabsf(a.x - b.x) < 0.01f && fabsf(a.y - b.y) < 0.01f &&
           fabsf(a.w - b.w) < 0.01f && fabsf(a.h - b.h) < 0.01f;
}

static int has_solid_color(const wn_display_list *list, unsigned int argb)
{
    uint8_t r = (uint8_t)((argb >> 16) & 0xFFu);
    uint8_t g = (uint8_t)((argb >> 8) & 0xFFu);
    uint8_t b = (uint8_t)(argb & 0xFFu);
    uint8_t a = (uint8_t)((argb >> 24) & 0xFFu);
    int i;
    if (list == NULL) {
        return 0;
    }
    for (i = 0; i < list->vertex_count; ++i) {
        const wn_vertex *v = &list->vertices[i];
        if (v->r == r && v->g == g && v->b == b && v->a == a) {
            return 1;
        }
    }
    return 0;
}

static int has_glyph_color(const wn_display_list *list, unsigned int argb)
{
    uint8_t r = (uint8_t)((argb >> 16) & 0xFFu);
    uint8_t g = (uint8_t)((argb >> 8) & 0xFFu);
    uint8_t b = (uint8_t)(argb & 0xFFu);
    uint8_t a = (uint8_t)((argb >> 24) & 0xFFu);
    int i;
    if (list == NULL) {
        return 0;
    }
    for (i = 0; i < list->glyph_vertex_count; ++i) {
        const wn_glyph_vertex *v = &list->glyph_vertices[i];
        if (v->r == r && v->g == g && v->b == b && v->a == a) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    wn_core *core;
    wn_rect before;
    wn_rect after;
    wn_rect image_before;
    int clone;
    char long_family[100];

    memset(long_family, 'x', sizeof(long_family) - 1);
    long_family[sizeof(long_family) - 1] = '\0';

    wn_text_test_use_backend(wn_text_fake_style());
    core = make_core();
    if (!check("style fixture builds", core != NULL, NULL)) {
        wn_text_test_use_backend(NULL);
        return 1;
    }

    /* ------------------------------------------------------- initial state */
    before = rect_of(core, kTextA);
    image_before = rect_of(core, kImage);
    check("initial text metrics",
          before.w == 32.0f && before.h == 20.0f &&
              rect_of(core, kTextB).w == 16.0f,
          "4x8px + 2x8px glyphs at 16px");
    check("initial effective style",
          strcmp(wn_core_node_font_family(core, kTextA), "fake") == 0 &&
              wn_core_node_font_size(core, kTextA) == 16.0f &&
              wn_core_node_font_weight(core, kTextA) == 400 &&
              wn_core_node_text_color(core, kTextA) == 0xFF111111u,
          "compiled fields are the base");

    /* ---------------------------------------------- text color inheritance */
    check("set_color(container) accepted",
          wn_core_set_color(core, kContainer, 0xFF00FF00u) == 0,
          NULL);
    check("color inherits to text runs",
          wn_core_node_text_color(core, kTextA) == 0xFF00FF00u &&
              wn_core_node_text_color(core, kTextB) == 0xFF00FF00u,
          "nearest self-or-ancestor override");
    wn_core_build_display_list(core);
    check("color reaches the glyph vertices",
          has_glyph_color(wn_core_display_list(core), 0xFF00FF00u) &&
              !has_glyph_color(wn_core_display_list(core), 0xFF111111u),
          "paint uses the effective text color");

    check("set_color(text) wins over ancestor",
          wn_core_set_color(core, kTextA, 0xFFFF0000u) == 0 &&
              wn_core_node_text_color(core, kTextA) == 0xFFFF0000u &&
              wn_core_node_text_color(core, kTextB) == 0xFF00FF00u,
          NULL);
    wn_core_build_display_list(core);
    check("both inherited colors are painted",
          has_glyph_color(wn_core_display_list(core), 0xFFFF0000u) &&
              has_glyph_color(wn_core_display_list(core), 0xFF00FF00u),
          NULL);
    check("reset_style(text) restores the ancestor color",
          wn_core_reset_style(core, kTextA, WN_STYLE_TEXT_COLOR) == 0 &&
              wn_core_node_text_color(core, kTextA) == 0xFF00FF00u,
          NULL);
    check("reset_style(container) restores compiled color",
          wn_core_reset_style(core, kContainer, WN_STYLE_TEXT_COLOR) == 0 &&
              wn_core_node_text_color(core, kTextA) == 0xFF111111u &&
              wn_core_node_text_color(core, kTextB) == 0xFF111111u,
          NULL);
    wn_core_build_display_list(core);
    check("compiled glyph color is back",
          has_glyph_color(wn_core_display_list(core), 0xFF111111u) &&
              !has_glyph_color(wn_core_display_list(core), 0xFF00FF00u),
          NULL);

    /* ----------------------------------------------------- background box */
    check("set_background(static node) accepted",
          wn_core_set_background(core, kPlain, 0xFF336699u) == 0 &&
              wn_core_node_background(core, kPlain) == 0xFF336699u,
          "a static node can be styled");
    wn_core_build_display_list(core);
    check("background reaches the solid pass",
          has_solid_color(wn_core_display_list(core), 0xFF336699u),
          NULL);
    check("background does not inherit",
          wn_core_node_background(core, kContainer) == 0u,
          NULL);
    check("reset background restores the compiled value",
          wn_core_reset_style(core, kPlain, WN_STYLE_BACKGROUND) == 0 &&
              wn_core_node_background(core, kPlain) == 0u,
          NULL);
    wn_core_build_display_list(core);
    check("background solid is gone",
          !has_solid_color(wn_core_display_list(core), 0xFF336699u),
          NULL);

    /* ---------------------------------------------------------- font family */
    check("set_font_family(container) accepted",
          wn_core_set_font_family(core, kContainer, "wide") == 0 &&
              wn_core_needs_layout(core) != 0 &&
              strcmp(wn_core_node_font_family(core, kTextA), "wide") == 0,
          "family inherits and requests relayout");
    wn_core_relayout(core);
    check("font family changes measurement",
          rect_of(core, kTextA).w == 64.0f &&
              rect_of(core, kTextB).w == 32.0f,
          "wide doubles the fake advance");
    check("reset font family restores measurement",
          wn_core_reset_style(core, kContainer, WN_STYLE_FONT_FAMILY) == 0,
          NULL);
    wn_core_relayout(core);
    check("compiled family metrics are back",
          rect_of(core, kTextA).w == 32.0f &&
              strcmp(wn_core_node_font_family(core, kTextA), "fake") == 0,
          NULL);

    /* ------------------------------------------------------------ font size */
    check("set_font_size(container) accepted",
          wn_core_set_font_size(core, kContainer, 24.0f) == 0 &&
              wn_core_needs_layout(core) != 0,
          NULL);
    wn_core_relayout(core);
    check("font size changes measurement",
          rect_of(core, kTextA).w == 48.0f &&
              rect_of(core, kTextB).w == 24.0f,
          NULL);
    check("WN_AUTO restores the compiled font size",
          wn_core_set_font_size(core, kContainer, WN_AUTO) == 0,
          NULL);
    wn_core_relayout(core);
    check("compiled font size metrics are back",
          rect_of(core, kTextA).w == 32.0f &&
              wn_core_node_font_size(core, kTextA) == 16.0f,
          NULL);

    /* ---------------------------------------------------------- font weight */
    check("set_font_weight(container) accepted",
          wn_core_set_font_weight(core, kContainer, 800) == 0 &&
              wn_core_needs_layout(core) != 0 &&
              wn_core_node_font_weight(core, kTextA) == 800,
          "weight inherits to the text runs");
    wn_core_relayout(core);
    after = rect_of(core, kTextA);
    check("font weight changes measurement",
          after.w > 44.0f && after.w <= 45.0f,
          "800 maps to a 1.4x fake advance");
    check("weight 0 restores the compiled weight",
          wn_core_set_font_weight(core, kContainer, 0) == 0,
          NULL);
    wn_core_relayout(core);
    check("compiled weight metrics are back",
          rect_of(core, kTextA).w == 32.0f &&
              wn_core_node_font_weight(core, kTextA) == 400,
          NULL);

    /* ----------------------------------------------- app-wide recursive font */
    check("app-wide family accepted",
          wn_core_set_font_family_tree(core, kRoot, "wide", 1) == 0,
          NULL);
    wn_core_relayout(core);
    check("app-wide family reaches descendants",
          rect_of(core, kTextA).w == 64.0f &&
              rect_of(core, kTextB).w == 32.0f,
          NULL);
    check("app-wide font leaves image boxes untouched",
          rects_equal(rect_of(core, kImage), image_before),
          "40x30 stays 40x30");
    check("app-wide reset accepted",
          wn_core_set_font_family_tree(core, kRoot, NULL, 1) == 0,
          NULL);
    wn_core_relayout(core);
    check("app-wide reset restores compiled metrics",
          rect_of(core, kTextA).w == 32.0f &&
              rect_of(core, kTextB).w == 16.0f,
          NULL);

    /* ----------------------------------------------------------- clone copy */
    check("clone source family set",
          wn_core_set_font_family(core, kTextA, "wide") == 0,
          NULL);
    wn_core_relayout(core);
    before = rect_of(core, kTextA);
    clone = wn_core_clone(core, kTextA, kContainer);
    check("clone keeps the style override",
          clone > 0 &&
              strcmp(wn_core_node_font_family(core, clone), "wide") == 0,
          NULL);
    wn_core_relayout(core);
    after = rect_of(core, clone);
    check("clone lays out with the override",
          after.w == before.w && after.h == before.h,
          "the clone inherits the copied family override");
    check("clone reset falls back to its compiled base",
          wn_core_set_font_family(core, clone, NULL) == 0,
          NULL);
    wn_core_relayout(core);
    check("clone compiled metrics restored",
          rect_of(core, clone).w == 32.0f,
          NULL);
    check("source override survives the clone reset",
          rect_of(core, kTextA).w == 64.0f,
          NULL);
    check("clone removed", wn_core_remove(core, clone) == 0, NULL);
    check("source reset", wn_core_reset_style(core, kTextA,
                                              WN_STYLE_FONT_FAMILY) == 0,
          NULL);
    wn_core_relayout(core);

    /* ------------------------------------------------------------- invalid */
    before = rect_of(core, kTextA);
    check("color/background reject dead handles",
          wn_core_set_color(core, -1, 0xFFFFFFFFu) == -1 &&
              wn_core_set_color(core, 100000, 0xFFFFFFFFu) == -1 &&
              wn_core_set_background(core, -1, 0xFFFFFFFFu) == -1 &&
              wn_core_set_background(core, 100000, 0xFFFFFFFFu) == -1,
          NULL);
    check("font size rejects non-finite and out-of-range values",
          wn_core_set_font_size(core, kTextA, NAN) == -1 &&
              wn_core_set_font_size(core, kTextA, INFINITY) == -1 &&
              wn_core_set_font_size(core, kTextA, 0.0f) == -1 &&
              wn_core_set_font_size(core, kTextA, 0.5f) == -1 &&
              wn_core_set_font_size(core, kTextA, 1025.0f) == -1,
          NULL);
    check("font weight rejects values outside 100..900",
          wn_core_set_font_weight(core, kTextA, 99) == -1 &&
              wn_core_set_font_weight(core, kTextA, 901) == -1,
          NULL);
    check("font family rejects empty/control/oversized names",
          wn_core_set_font_family(core, kTextA, "") == -1 &&
              wn_core_set_font_family(core, kTextA, "\x01" "bad") == -1 &&
              wn_core_set_font_family(core, kTextA, long_family) == -1,
          NULL);
    check("reset_style rejects zero and unknown masks",
          wn_core_reset_style(core, kTextA, 0u) == -1 &&
              wn_core_reset_style(core, kTextA, 0x80000000u) == -1,
          NULL);
    check("font setters reject dead handles",
          wn_core_set_font_family(core, -1, "wide") == -1 &&
              wn_core_set_font_size(core, -1, 16.0f) == -1 &&
              wn_core_set_font_weight(core, -1, 400) == -1 &&
              wn_core_set_font_family_tree(core, -1, "wide", 1) == -1 &&
              wn_core_reset_style(core, -1, WN_STYLE_ALL) == -1,
          NULL);
    check("NULL core is rejected",
          wn_core_set_color(NULL, kTextA, 0xFFFFFFFFu) == -1 &&
              wn_core_set_font_family(NULL, kTextA, "wide") == -1 &&
              wn_core_set_font_size(NULL, kTextA, 16.0f) == -1 &&
              wn_core_set_font_weight(NULL, kTextA, 400) == -1 &&
              wn_core_reset_style(NULL, kTextA, WN_STYLE_ALL) == -1 &&
              wn_core_set_font_family_tree(NULL, kRoot, "wide", 1) == -1,
          NULL);
    wn_core_relayout(core);
    check("invalid calls change nothing",
          rects_equal(before, rect_of(core, kTextA)) &&
              strcmp(wn_core_node_font_family(core, kTextA), "fake") == 0 &&
              wn_core_node_text_color(core, kTextA) == 0xFF111111u,
          NULL);

    wn_core_destroy(core);
    wn_text_test_use_backend(NULL);
    printf("style_override_test: %s (%d checks)\n",
           failures == 0 ? "PASS" : "FAIL", checks);
    return failures == 0 ? 0 : 1;
}
