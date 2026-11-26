/*
 * abi_v6_core_test.c - headless acceptance for the ABI v6 read/state/image API.
 *
 * Compiles against tests/fidelity/out/emit/ui_data.h and the core only. It
 * covers the three core-visible additions of ABI v6:
 *   B2  per-node state bits (several selected nodes, exclusive hover)
 *   B3  get_rect forcing layout and scroll_into_view
 *   A9  intrinsic image size override and the layout reflow it triggers
 * plus the static refcount no-op for the builtin document (A2).
 *
 *   abi_v6_core_test.exe
 */
#include <stdio.h>
#include <string.h>

#include "ui_data.h"
#include "wn_core.h"

static wn_core *g_core;
static int g_failures;
static int g_checks;

static int check(const char *name, int ok, const char *detail)
{
    printf("[%s] abi6: %s - %s\n", ok ? "ok  " : "FAIL", name, detail);
    ++g_checks;
    if (!ok) {
        ++g_failures;
    }
    return ok;
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

static int rect_inside_node(const wn_rect *child, const wn_rect *container,
                            float margin)
{
    return child->y >= container->y + margin - 0.01f &&
           child->y + child->h <= container->y + container->h - margin + 0.01f;
}

int main(void)
{
    const wn_document *builtin = wn_document_builtin();
    int scroll, scrollcontent, deep, search, image;
    int before_offset;
    int after_offset;
    wn_rect sc;
    wn_rect child;
    int state0;
    int state1;

    g_core = wn_core_create(builtin);
    if (g_core == NULL) {
        printf("FAIL: wn_core_create\n");
        return 1;
    }

    /* --------------------------------------- A2: static builtin refcount */
    wn_document_retain((wn_document *)builtin);
    wn_document_release((wn_document *)builtin);
    wn_document_release((wn_document *)builtin);
    check("builtin refcount", wn_core_node_count(g_core) == UI_NODE_COUNT,
          "retain/release on the builtin document is a no-op");

    scroll = find("body.root/div#scroll.scrollbox");
    scrollcontent = find("body.root/div#scroll.scrollbox/div#scrollcontent.scrollcontent");
    deep = find("body.root/div#scroll.scrollbox/div#scrollcontent.scrollcontent/div#deep.deep");
    search = find("body.root/input#search.field");
    image = find("body.root/img#ratio.ratio");

    /* -------------------------------------------- B2: per-node state bits */
    if (scroll >= 0 && deep >= 0) {
        wn_core_set_state(g_core, scroll, WN_STATE_SELECTED, 1);
        wn_core_set_state(g_core, deep, WN_STATE_SELECTED, 1);
        state0 = wn_core_node_state(g_core, scroll);
        state1 = wn_core_node_state(g_core, deep);
        check("state selected mask", state0 == WN_STATE_SELECTED &&
              state1 == WN_STATE_SELECTED &&
              wn_core_selected(g_core) == scroll,
              "two nodes carry the selected bit; the legacy getter returns the first");
        wn_core_set_state(g_core, scroll, WN_STATE_HOVER, 1);
        wn_core_set_state(g_core, deep, WN_STATE_HOVER, 1);
        check("state hover exclusive",
              wn_core_node_state(g_core, scroll) == WN_STATE_SELECTED &&
              wn_core_node_state(g_core, deep) ==
                  (WN_STATE_SELECTED | WN_STATE_HOVER) &&
              wn_core_hover(g_core) == deep,
              "a second hover clears the first node's hover bit");
        wn_core_set_state(g_core, scroll, WN_STATE_SELECTED, 0);
        wn_core_set_state(g_core, deep, WN_STATE_SELECTED, 0);
        wn_core_set_state(g_core, deep, WN_STATE_HOVER, 0);
        check("state clear", wn_core_node_state(g_core, scroll) == 0 &&
              wn_core_node_state(g_core, deep) == 0 &&
              wn_core_selected(g_core) == -1 && wn_core_hover(g_core) == -1,
              "clearing the bits leaves no state behind");
    }

    /* ----------------------------------- B3: laid-out rect + scroll_into_view */
    if (scroll >= 0 && deep >= 0) {
        wn_core_node_rect(g_core, scroll, &sc);
        wn_core_node_rect(g_core, deep, &child);
        before_offset = (int)(wn_core_scroll_offset(g_core, scroll) + 0.5f);
        check("scroll pre-state", before_offset == 0 &&
              child.h > 0.0f && child.y + child.h > sc.y + sc.h,
              "the deep node starts below the scroll viewport");
        check("scroll_into_view",
              wn_core_scroll_node_into_view(g_core, deep, 4.0f) == 0,
              "WN_OK");
        after_offset = (int)(wn_core_scroll_offset(g_core, scroll) + 0.5f);
        wn_core_node_rect(g_core, scroll, &sc);
        wn_core_node_rect(g_core, deep, &child);
        check("scroll offset moved", after_offset > before_offset &&
              after_offset <= (int)(wn_core_scroll_max(g_core, scroll) + 0.5f),
              "the nearest scroll ancestor moved");
        check("scroll target visible", rect_inside_node(&child, &sc, 4.0f),
              "rect is inside the container content box with the margin");
        check("scroll_into_view no-op",
              wn_core_scroll_node_into_view(g_core, deep, 4.0f) == 0 &&
              (int)(wn_core_scroll_offset(g_core, scroll) + 0.5f) == after_offset,
              "already visible => no scroll");
        check("scroll_into_view no ancestor",
              wn_core_scroll_node_into_view(g_core, search, 0.0f) == 0,
              "a node without a scrollable ancestor is a no-op");
        check("scroll_into_view invalid",
              wn_core_scroll_node_into_view(g_core, 100000, 0.0f) == -1,
              "out-of-range node is rejected");
    }

    /* ------------------------------- B3: get_rect forces pending layout (A9) */
    if (image >= 0) {
        wn_rect initial;
        wn_rect wide;
        wn_rect unknown;
        wn_core_node_rect(g_core, image, &initial);
        check("image intrinsic layout", initial.w == 100.0f &&
              initial.h == 50.0f,
              "100px wide 20x10 image lays out 50px tall");
        check("image size accepted",
              wn_core_set_image_size(g_core, image, 20.0f, 40.0f) == 0,
              "WN_OK");
        check("image size marks layout", wn_core_needs_layout(g_core) != 0,
              "set_image_size dirties layout");
        wn_core_node_rect(g_core, image, &wide);
        check("image size reflow", wide.w == 100.0f && wide.h == 200.0f,
              "a 1:2 intrinsic ratio makes the same box 200px tall");
        check("image size unknown",
              wn_core_set_image_size(g_core, image, 0.0f, 0.0f) == 0,
              "0,0 resets to unknown");
        wn_core_node_rect(g_core, image, &unknown);
        check("image size unknown layout", unknown.w == 100.0f &&
              unknown.h == 0.0f,
              "unknown intrinsic size collapses the auto height");
        check("image size invalid",
              wn_core_set_image_size(g_core, search, 10.0f, 10.0f) == 1 &&
              wn_core_set_image_size(g_core, image, -1.0f, 2.0f) == -1,
              "non-image and negative size are rejected");
        check("image size restore",
              wn_core_set_image_size(g_core, image, 20.0f, 10.0f) == 0,
              "restored for later stages");
    }

    wn_core_destroy(g_core);
    printf("abi_v6_core_test: %s (%d checks)\n",
           g_failures == 0 ? "PASS" : "FAIL", g_checks);
    return g_failures == 0 ? 0 : 1;
}
