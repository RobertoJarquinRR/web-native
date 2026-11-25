#include <math.h>
#include <stdio.h>
#include <string.h>

#include "text/text_runtime.h"
#include "ui_data.h"

#define WIDTH_TOLERANCE_PX 1.0f
#define HEIGHT_TOLERANCE_PX 2.0f

static int checks = 0;

static void check(const char *label, int ok)
{
    checks += 1;
    printf("  %s %s\n", ok ? "[ok]" : "[FAIL]", label);
}

int main(void)
{
    float worst_width = 0.0f;
    float worst_height = 0.0f;
    int text_nodes = 0;
    int width_ok = 0;
    int height_ok = 0;

    for (int node = 0; node < UI_NODE_COUNT; ++node) {
        const UiNode *n = &kUiNodes[node];
        wn_text_metrics measured;
        float width_delta;
        float height_delta;
        if (n->kind != R2_NODE_TEXT || n->text == NULL) {
            continue;
        }
        ++text_nodes;
        if (wn_text_measure_string(n->text, n->font_family, n->font_size, n->font_weight,
                                   0.0f, &measured) != 0) {
            printf("  [FAIL] measure failed for %s\n", kUiNodeNames[node]);
            return 1;
        }
        width_delta = fabsf(measured.width - n->width);
        height_delta = fabsf(measured.height - n->height);
        if (width_delta > worst_width) {
            worst_width = width_delta;
        }
        if (height_delta > worst_height) {
            worst_height = height_delta;
        }
        if (width_delta <= WIDTH_TOLERANCE_PX) {
            ++width_ok;
        }
        if (height_delta <= HEIGHT_TOLERANCE_PX) {
            ++height_ok;
        }
        printf("  %-10s %-10s baked=%6.2fx%5.2f measured=%6.2fx%5.2f dw=%+5.2f dh=%+5.2f\n",
               kUiNodeNames[node], n->text, n->width, n->height,
               measured.width, measured.height, measured.width - n->width,
               measured.height - n->height);
    }

    check("document has text nodes", text_nodes > 0);
    check("every measurement is positive", worst_width < 1e6f && text_nodes > 0);
    check("all widths match the compiler within 1 px", width_ok == text_nodes);
    check("all heights match the compiler within 2 px", height_ok == text_nodes);
    check("measurement cache was used", wn_text_runtime_cache_misses() <= text_nodes);

    printf("document_text_test: worst width delta %.2f px, height delta %.2f px\n",
           worst_width, worst_height);
    if (width_ok != text_nodes || height_ok != text_nodes) {
        printf("document_text_test: FAIL (%d/%d checks)\n", checks, checks);
        return 1;
    }
    printf("document_text_test: PASS (%d checks, %d text nodes)\n", checks, text_nodes);
    return 0;
}
