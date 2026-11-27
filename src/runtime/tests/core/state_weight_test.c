/*
 * state_weight_test.c - blob v6 additive state font-weight + ellipsis.
 *
 * Runs on the deterministic weight-sensitive fake text backend (no
 * DirectWrite): the advance grows with the weight, so a state change is
 * visible in layout metrics and the atlas key.
 *
 *   state_weight_test.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/wn_core_internal.h"
#include "document/wn_blob.h"
#include "document/wn_document_internal.h"
#include "text/text_runtime.h"
#include "wn_core.h"

const wn_text_ops *wn_text_fake_weight(void);

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

typedef struct {
    unsigned char state;
    unsigned char present;
    unsigned short weight;
} WeightState;

#define NODE_COUNT 4
static wn_document g_doc;
static UiNode g_nodes[NODE_COUNT];
static UiStateStyle g_owner_states[1];
static UiStateStyle g_text_states[1];
static const char *const g_names[NODE_COUNT] = {
    "body.root", "body.root/div.owner", "body.root/div.owner/text",
    "body.root/div.ellipsis"
};

static wn_core *make_core(void)
{
    int i;
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(&g_doc, 0, sizeof(g_doc));
    memset(g_owner_states, 0, sizeof(g_owner_states));
    memset(g_text_states, 0, sizeof(g_text_states));

    g_nodes[0].parent = -1;
    g_nodes[0].kind = R2_NODE_BLOCK;
    g_nodes[0].width = 200.0f;
    g_nodes[0].height = 120.0f;

    /* Owner block: `:hover { font-weight: 700 }`. */
    g_nodes[1].parent = 0;
    g_nodes[1].kind = R2_NODE_BLOCK;
    g_nodes[1].width = 120.0f;
    g_nodes[1].height = 40.0f;
    g_nodes[1].font_weight = 400;

    /* Generated text run: carries the same additive weight for the owner
     * state, exactly as the frontend emits it. */
    g_nodes[2].parent = 1;
    g_nodes[2].kind = R2_NODE_TEXT;
    g_nodes[2].text = "MMMM";
    g_nodes[2].font_family = "fake";
    g_nodes[2].font_size = 16.0f;
    g_nodes[2].font_weight = 400;
    g_nodes[2].text_color = 0xFFFFFFFFu;
    g_nodes[2].width = -1.0f;
    g_nodes[2].height = 20.0f;

    /* Single-line ellipsis box: 20px of room, 5 glyphs of 8px. */
    g_nodes[3].parent = 0;
    g_nodes[3].kind = R2_NODE_TEXT;
    g_nodes[3].text = "MMMMM";
    g_nodes[3].font_family = "fake";
    g_nodes[3].font_size = 16.0f;
    g_nodes[3].font_weight = 400;
    g_nodes[3].text_color = 0xFFFFFFFFu;
    g_nodes[3].width = 20.0f;
    g_nodes[3].height = 20.0f;
    g_nodes[3].text_flags = WN_TEXT_FLAG_ELLIPSIS;

    for (i = 0; i < NODE_COUNT; ++i) {
        g_nodes[i].font_size = g_nodes[i].font_size > 0.0f
                                   ? g_nodes[i].font_size
                                   : 16.0f;
        g_nodes[i].flex_shrink = 1.0f;
        g_nodes[i].flex_basis = -1.0f;
        g_nodes[i].opacity = 1.0f;
        g_nodes[i].grid_auto_rows = -1.0f;
        g_nodes[i].z_index = WN_BLOB_Z_INDEX_AUTO;
    }

    g_owner_states[0].state = R2_STATE_STYLE_HOVER;
    g_owner_states[0].present = R2_STATE_FIELD_FONT_WEIGHT;
    g_owner_states[0].reserved = 700;
    g_nodes[1].state_styles = g_owner_states;
    g_nodes[1].state_style_count = 1;

    g_text_states[0].state = R2_STATE_STYLE_HOVER;
    g_text_states[0].present = R2_STATE_FIELD_FONT_WEIGHT;
    g_text_states[0].reserved = 700;
    g_nodes[2].state_styles = g_text_states;
    g_nodes[2].state_style_count = 1;

    g_doc.nodes = g_nodes;
    g_doc.node_count = NODE_COUNT;
    g_doc.names = g_names;
    g_doc.viewport_width = 200;
    g_doc.viewport_height = 120;
    g_doc.refs = WN_DOCUMENT_STATIC_REFS;
    return wn_core_create(&g_doc);
}

int main(void)
{
    wn_core *core;
    wn_rect before;
    wn_rect after;
    const wn_atlas_run *base_run;
    const wn_atlas_run *bold_run;
    const wn_atlas_run *repeat_bold;
    int vertices_before;

    wn_text_test_use_backend(wn_text_fake_weight());
    core = make_core();
    check("weight fixture builds", core != NULL, NULL);
    if (core == NULL) {
        wn_text_test_use_backend(NULL);
        return 1;
    }

    check("base weight is the document weight",
          wn_core_node_font_weight(core, 1) == 400 &&
              wn_core_node_font_weight(core, 2) == 400,
          NULL);
    wn_core_relayout(core);
    check("text rect before hover", wn_core_node_rect(core, 2, &before) == 0 &&
                                        before.w > 0.0f,
          NULL);
    base_run = wn_text_atlas_shape(core->text_atlas, "MMMM", "fake", 16.0f, 400);
    bold_run = wn_text_atlas_shape(core->text_atlas, "MMMM", "fake", 16.0f, 700);
    check("atlas key includes the weight",
          base_run != NULL && bold_run != NULL && base_run != bold_run,
          NULL);

    wn_core_set_state(core, 1, WN_STATE_HOVER, 1);
    check("hover weight reaches the owner text run",
          wn_core_node_font_weight(core, 1) == 700 &&
              wn_core_node_font_weight(core, 2) == 700,
          "generated text follows the owner state");
    check("a weight state requests relayout", wn_core_needs_layout(core) != 0,
          "wn_view render will re-measure before painting");
    wn_core_relayout(core);
    check("state weight changes the advance/metrics",
          wn_core_node_rect(core, 2, &after) == 0 && after.w > before.w + 1.0f,
          "layout rebuild uses the effective state weight");
    repeat_bold =
        wn_text_atlas_shape(core->text_atlas, "MMMM", "fake", 16.0f, 700);
    check("state weight reuses the bold atlas entry", repeat_bold == bold_run,
          NULL);

    wn_core_build_display_list(core);
    {
        const wn_display_list *list = wn_core_display_list(core);
        vertices_before = list->glyph_vertex_count;
        /* The ellipsis box draws one glyph + the ellipsis glyph (6 vertices
         * each) instead of all five; the weight box adds its four glyphs. */
        check("ellipsis truncates the line and adds the glyph",
              vertices_before == (4 + 2) * 6,
              "4 weight glyphs + 1 clipped glyph + the ellipsis");
    }

    wn_core_set_state(core, 1, WN_STATE_HOVER, 0);
    wn_core_relayout(core);
    check("returning to base restores the metrics",
          wn_core_node_rect(core, 2, &after) == 0 &&
              after.w == before.w,
          NULL);

    /* A multi-line box keeps the flag inert (single-line ellipsis only). */
    g_nodes[3].text_flags = 0;
    core->nodes[3].text_flags = 0;
    wn_core_relayout(core);
    wn_core_build_display_list(core);
    check("without the flag all glyphs are emitted",
          wn_core_display_list(core)->glyph_vertex_count ==
              (4 + 5) * 6,
          "the five glyphs are clipped by the box, not truncated");

    wn_core_destroy(core);
    wn_text_test_use_backend(NULL);
    printf("state_weight_test: %s (%d checks)\n",
           failures == 0 ? "PASS" : "FAIL", checks);
    return failures == 0 ? 0 : 1;
}
