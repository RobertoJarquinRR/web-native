#include <math.h>
#include <stdio.h>
#include <string.h>

#include "text/text_runtime.h"
#include "ui_data.h"
#include "wn_core.h"

static int checks = 0;

static void check(const char *label, int ok)
{
    checks += 1;
    printf("  %s %s\n", ok ? "[ok]" : "[FAIL]", label);
}

int main(void)
{
    const wn_document *document = wn_document_builtin();
    wn_core *core;
    const wn_rect *rects;
    int rect_count = 0;
    int text_nodes = 0;
    int width_matches = 0;
    int height_matches = 0;
    int baked_sizes = 0;

    if (document == NULL) {
        printf("document_text_test: FAIL (no document)\n");
        return 1;
    }
    core = wn_core_create(document);
    if (core == NULL) {
        printf("document_text_test: FAIL (no core)\n");
        return 1;
    }
    wn_core_resize(core, UI_VIEWPORT_WIDTH, UI_VIEWPORT_HEIGHT);
    rects = wn_core_rects(core, &rect_count);

    for (int node = 0; node < UI_NODE_COUNT && node < rect_count; ++node) {
        const UiNode *n = &kUiNodes[node];
        wn_text_metrics measured;
        float expected_w;
        float expected_h;
        if (n->kind != R2_NODE_TEXT || n->text == NULL) {
            continue;
        }
        ++text_nodes;
        if (n->width >= 0.0f || n->height >= 0.0f) {
            ++baked_sizes;
        }
        if (wn_text_measure_string(n->text, n->font_family, n->font_size, n->font_weight,
                                   0.0f, &measured) != 0) {
            printf("  [FAIL] measure failed for node %d\n", node);
            wn_core_destroy(core);
            return 1;
        }
        expected_w = ceilf(measured.width);
        expected_h = n->line_height > 0.0f ? ceilf(n->line_height) : ceilf(measured.height);
        if (fabsf(rects[node].w - expected_w) <= 0.51f) {
            ++width_matches;
        }
        if (fabsf(rects[node].h - expected_h) <= 0.51f) {
            ++height_matches;
        }
        printf("  %-48s %-10s rect=%6.2fx%5.2f measured=%6.2fx%5.2f\n",
               kUiNodeNames[node], n->text, rects[node].w, rects[node].h,
               measured.width, measured.height);
    }

    check("document has text nodes", text_nodes > 0);
    check("no text size is baked into the document", baked_sizes == 0);
    check("layout width comes from runtime measurement", width_matches == text_nodes);
    check("layout height comes from runtime measurement", height_matches == text_nodes);

    wn_core_destroy(core);
    wn_text_runtime_shutdown();
    if (baked_sizes != 0 || width_matches != text_nodes || height_matches != text_nodes) {
        printf("document_text_test: FAIL (%d checks, %d text nodes)\n", checks, text_nodes);
        return 1;
    }
    printf("document_text_test: PASS (%d checks, %d text nodes)\n", checks, text_nodes);
    return 0;
}
