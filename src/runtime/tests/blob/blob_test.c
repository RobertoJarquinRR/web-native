#include <stdio.h>

#include "ui_data.h"
#include "webnative.h"
#include "wn_core.h"

static int checks = 0;

static void check(const char *label, int ok)
{
    checks += 1;
    printf("  %s %s\n", ok ? "[ok]" : "[FAIL]", label);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "fixture/app.wnui";
    const wn_document *builtin = wn_document_builtin();
    wn_document *loaded = wn_document_load_blob(path);
    wn_core *core_builtin;
    wn_core *core_blob;
    const wn_rect *rects_builtin;
    const wn_rect *rects_blob;
    int count_builtin = 0;
    int count_blob = 0;
    int matching_rects = 0;
    const wn_display_list *list_builtin;
    const wn_display_list *list_blob;

    if (loaded == NULL) {
        printf("blob_test: FAIL (%s)\n", wn_last_error() != NULL ? wn_last_error() : "load");
        return 1;
    }
    check("node count survives the round trip",
          wn_document_node_count(loaded) == wn_document_node_count(builtin));
    if (wn_document_node_count(loaded) != wn_document_node_count(builtin)) {
        wn_document_destroy(loaded);
        printf("blob_test: FAIL\n");
        return 1;
    }

    core_builtin = wn_core_create(builtin);
    core_blob = wn_core_create(loaded);
    check("both cores are created", core_builtin != NULL && core_blob != NULL);
    if (core_builtin == NULL || core_blob == NULL) {
        wn_core_destroy(core_builtin);
        wn_core_destroy(core_blob);
        wn_document_destroy(loaded);
        printf("blob_test: FAIL\n");
        return 1;
    }
    wn_core_resize(core_builtin, UI_VIEWPORT_WIDTH, UI_VIEWPORT_HEIGHT);
    wn_core_resize(core_blob, UI_VIEWPORT_WIDTH, UI_VIEWPORT_HEIGHT);
    rects_builtin = wn_core_rects(core_builtin, &count_builtin);
    rects_blob = wn_core_rects(core_blob, &count_blob);
    check("same rect count", count_builtin == count_blob && count_builtin == UI_NODE_COUNT);

    for (int node = 0; node < count_builtin && node < count_blob; ++node) {
        if (rects_builtin[node].x == rects_blob[node].x &&
            rects_builtin[node].y == rects_blob[node].y &&
            rects_builtin[node].w == rects_blob[node].w &&
            rects_builtin[node].h == rects_blob[node].h) {
            ++matching_rects;
        } else {
            printf("  [FAIL] rect[%d] differs: %s\n", node, kUiNodeNames[node]);
        }
    }
    check("layout is identical", matching_rects == count_builtin);

    list_builtin = wn_core_display_list(core_builtin);
    list_blob = wn_core_display_list(core_blob);
    check("display lists match",
          list_builtin->vertex_count == list_blob->vertex_count &&
          list_builtin->glyph_vertex_count == list_blob->glyph_vertex_count &&
          list_builtin->image_vertex_count == list_blob->image_vertex_count);
    printf("  nodes=%d rects=%d/%d solids=%d glyphs=%d images=%d\n",
           UI_NODE_COUNT, matching_rects, count_builtin, list_blob->vertex_count,
           list_blob->glyph_vertex_count, list_blob->image_vertex_count);

    wn_core_destroy(core_builtin);
    wn_core_destroy(core_blob);
    wn_document_destroy(loaded);

    if (matching_rects != count_builtin) {
        printf("blob_test: FAIL (%d checks)\n", checks);
        return 1;
    }
    printf("blob_test: PASS (%d checks)\n", checks);
    return 0;
}
