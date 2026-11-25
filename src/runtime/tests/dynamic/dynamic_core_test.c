/*
 * dynamic_core_test.c - headless acceptance for Dynamic UI API v0.
 *
 * Compiles against tests/dynamic/fixture/ui_data.h and the core only (no GPU,
 * no window). Exercises visibility, clone/append/remove/clear_children,
 * set_image, set_text and the layout/paint invalidation they trigger.
 *
 *   dynamic_core_test.exe out/core_report.json
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_data.h"
#include "wn_core.h"
#include "wn_internal.h"

static wn_core *g_core;
static int g_failures;

static int check(const char *name, int ok, const char *detail)
{
    printf("[%s] core: %s - %s\n", ok ? "ok  " : "FAIL", name, detail);
    if (!ok) {
        ++g_failures;
    }
    return ok;
}

static wn_rect rect_of(int node)
{
    return wn_core_rects(g_core, NULL)[node];
}

static int hit_center(int node)
{
    wn_rect r = rect_of(node);
    return wn_core_hit_test(g_core, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
}

static int hit_rect(wn_rect r)
{
    return wn_core_hit_test(g_core, r.x + r.w * 0.5f, r.y + r.h * 0.5f);
}

/* Clone `count` copies of the card under `parent`, verify they stay alive and
 * lay out, then remove them. Returns the live count measured while they were
 * all present (-1 on allocation failure). */
static int stress_round(int template_node, int parent, int count,
                        double *clone_ms, double *remove_ms,
                        int *created_out, int *alive_out, int *laid_out_out)
{
    int *ids = (int *)malloc(sizeof(int) * (size_t)count);
    int created = 0;
    int alive = 0;
    int laid_out = 0;
    int live = -1;
    int i;
    double t0;

    *clone_ms = 0.0;
    *remove_ms = 0.0;
    *created_out = 0;
    *alive_out = 0;
    *laid_out_out = 0;
    if (ids == NULL) {
        return -1;
    }
    t0 = wn_now_ms();
    for (i = 0; i < count; ++i) {
        int node = wn_core_clone(g_core, template_node, parent);
        if (node < 0) {
            break;
        }
        ids[created++] = node;
    }
    *clone_ms = wn_now_ms() - t0;
    live = wn_core_live_node_count(g_core);
    for (i = 0; i < created; ++i) {
        if (wn_core_node_is_alive(g_core, ids[i]) &&
            wn_core_node_parent(g_core, ids[i]) == parent) {
            ++alive;
        }
    }
    wn_core_relayout(g_core);
    wn_core_build_display_list(g_core);
    for (i = 0; i < created; ++i) {
        if (rect_of(ids[i]).w == 120.0f) {
            ++laid_out;
        }
    }
    t0 = wn_now_ms();
    for (i = 0; i < created; ++i) {
        (void)wn_core_remove(g_core, ids[i]);
    }
    *remove_ms = wn_now_ms() - t0;
    wn_core_relayout(g_core);
    wn_core_build_display_list(g_core);
    free(ids);
    *created_out = created;
    *alive_out = alive;
    *laid_out_out = laid_out;
    return live;
}

static void write_rect(FILE *f, int node)
{
    wn_rect r = rect_of(node);
    fprintf(f, "{\"id\":%d,\"alive\":%d,\"visible\":%d,"
               "\"x\":%.2f,\"y\":%.2f,\"w\":%.2f,\"h\":%.2f}",
            node, wn_core_node_is_alive(g_core, node),
            wn_core_node_is_visible(g_core, node), r.x, r.y, r.w, r.h);
}

static void write_counts(FILE *f)
{
    int solid = 0, glyph = 0, image = 0;
    int solid_batches = 0, glyph_batches = 0, image_batches = 0;
    const wn_display_list *list = wn_core_display_list(g_core);
    if (list != NULL) {
        solid = list->vertex_count;
        glyph = list->glyph_vertex_count;
        image = list->image_vertex_count;
        solid_batches = list->solid_batch_count;
        glyph_batches = list->glyph_batch_count;
        image_batches = list->image_batch_count;
    }
    fprintf(f, "\"live\":%d,\"vertices\":%d,\"solid_batches\":%d,"
               "\"glyphs\":%d,\"glyph_batches\":%d,"
               "\"image_vertices\":%d,\"image_batches\":%d",
            wn_core_live_node_count(g_core), solid, solid_batches, glyph,
            glyph_batches, image, image_batches);
}

static int image_batch_has_src(const char *src)
{
    const wn_display_list *list = wn_core_display_list(g_core);
    int i;
    if (list == NULL || src == NULL) {
        return 0;
    }
    for (i = 0; i < list->image_batch_count; ++i) {
        if (list->image_batches[i].src != NULL &&
            strcmp(list->image_batches[i].src, src) == 0) {
            return 1;
        }
    }
    return 0;
}

static void event_begin(FILE *f, const char *stage)
{
    fprintf(f, "{\"stage\":\"%s\",", stage);
}

static void event_rect(FILE *f, const char *key, int node)
{
    fprintf(f, "\"%s\":", key);
    write_rect(f, node);
    fputc(',', f);
}

static void event_end(FILE *f)
{
    fputs("\"end\":true}\n", f);
}

int main(int argc, char **argv)
{
    const char *out_path = argc > 1 ? argv[1] : "core_events.jsonl";
    FILE *report;
    int cards, card0, card1, image0, text0, image1, text1;
    int clone, clone_image, clone_label, clone_text;
    wn_rect initial0, initial1;
    int initial_hit0, initial_hit1;
    int initial_vertices, initial_glyphs, initial_images;
    int hidden_images, shown_vertices, shown_images;
    int unsupported;
    int alpha_quads, beta_quads;
    int remove_rc, append_rc, clear_rc, cycle_rc;

    g_core = wn_core_create(wn_document_builtin());
    if (g_core == NULL) {
        printf("FAIL: wn_core_create\n");
        return 1;
    }
    report = fopen(out_path, "wb");
    if (report == NULL) {
        printf("FAIL: cannot write %s\n", out_path);
        wn_core_destroy(g_core);
        return 1;
    }
    wn_core_resize(g_core, 400, 300);
    wn_core_relayout(g_core);
    wn_core_build_display_list(g_core);

    cards = wn_core_find_node(g_core, "body.root/div.cards");
    card0 = wn_core_find_node(g_core, "body.root/div.cards/div.card");
    card1 = wn_core_find_node(g_core, "body.root/div.cards/div.card[1]");
    image0 = wn_core_find_node(g_core, "body.root/div.cards/div.card/img.thumb");
    text0 = wn_core_find_node(g_core, "body.root/div.cards/div.card/div.label/text[1]");
    image1 = wn_core_find_node(g_core, "body.root/div.cards/div.card[1]/img.thumb[1]");
    text1 = wn_core_find_node(g_core, "body.root/div.cards/div.card[1]/div.label[1]/text[2]");

    check("lookup cards", cards == 5, "div.cards is node 5");
    check("lookup card0", card0 == 6, "div.card is node 6");
    check("lookup card1", card1 == 10, "div.card[1] is node 10");
    check("lookup image/text", image0 == 7 && text0 == 9 && image1 == 11 && text1 == 13,
          "image and label defaults");
    check("lookup missing", wn_core_find_node(g_core, "div.nope") < 0,
          "unknown name returns -1");
    check("live count", wn_core_live_node_count(g_core) == UI_NODE_COUNT,
          "compiled nodes are live");

    initial0 = rect_of(card0);
    initial1 = rect_of(card1);
    initial_hit0 = hit_center(card0);
    initial_hit1 = hit_center(card1);
    {
        const wn_display_list *list = wn_core_display_list(g_core);
        initial_vertices = list->vertex_count;
        initial_glyphs = list->glyph_vertex_count;
        initial_images = list->image_vertex_count;
    }
    check("initial geometry", initial0.w == 120.0f && initial1.w == 120.0f &&
          initial1.x == initial0.x + initial0.w + 10.0f,
          "two 120px cards with a 10px gap");
    check("initial hit", initial_hit0 == card0 && initial_hit1 == card1,
          "card centers hit their action nodes");
    check("initial paint", initial_vertices > 0 && initial_glyphs > 0 &&
          initial_images == 12,
          "solids + glyphs + two image quads");
    event_begin(report, "initial");
    write_counts(report);
    fputc(',', report);
    event_rect(report, "card0", card0);
    event_rect(report, "card1", card1);
    fprintf(report, "\"hit0\":%d,\"hit1\":%d,", initial_hit0, initial_hit1);
    event_end(report);

    /* ---------------------------------------------------------- visibility */
    check("hide", wn_core_set_visible(g_core, card0, 0) == 0, "WN_OK");
    {
        int early_hit = hit_rect(initial0);
        check("hidden hit before frame", early_hit != card0,
              "hit testing re-layouts before the next frame and skips the hidden card");
    }
    wn_core_relayout(g_core);
    wn_core_build_display_list(g_core);
    {
        wn_rect hidden = rect_of(card0);
        wn_rect moved = rect_of(card1);
        hidden_images = wn_core_display_list(g_core)->image_vertex_count;
        check("hidden geometry", hidden.w == 0.0f && hidden.h == 0.0f,
              "hidden node has an empty box");
        check("hidden reflow", moved.x == initial0.x,
              "card1 moved into card0's slot");
        check("hidden hit", hit_rect(initial0) == card1 && hit_rect(initial1) == -1,
              "old card0 center hits card1 and old card1 slot is empty");
        check("hidden paint", hidden_images == initial_images - 6,
              "one image quad removed");
    }
    event_begin(report, "hidden");
    write_counts(report);
    fputc(',', report);
    event_rect(report, "card0", card0);
    event_rect(report, "card1", card1);
    fprintf(report, "\"hit0\":%d,", hit_rect(initial0));
    event_end(report);

    check("show", wn_core_set_visible(g_core, card0, 1) == 0, "WN_OK");
    wn_core_relayout(g_core);
    wn_core_build_display_list(g_core);
    {
        const wn_display_list *list = wn_core_display_list(g_core);
        shown_vertices = list->vertex_count;
        shown_images = list->image_vertex_count;
        check("show geometry", rect_of(card0).x == initial0.x &&
              rect_of(card1).x == initial1.x, "geometry restored");
        check("show hit", hit_center(card0) == card0, "card0 is hittable again");
        check("show paint", shown_vertices == initial_vertices &&
              shown_images == initial_images, "display list restored");
    }
    event_begin(report, "shown");
    write_counts(report);
    fputc(',', report);
    event_rect(report, "card0", card0);
    event_rect(report, "card1", card1);
    event_end(report);

    /* --------------------------------------------------------------- clone */
    clone = wn_core_clone(g_core, card0, cards);
    wn_core_relayout(g_core);
    wn_core_build_display_list(g_core);
    check("clone created", clone >= 0 && wn_core_node_is_dynamic(g_core, clone),
          "dynamic node returned");
    check("clone count", wn_core_live_node_count(g_core) == UI_NODE_COUNT + 4,
          "card + image + label + text subtree copied");
    check("clone parent", wn_core_node_parent(g_core, clone) == cards,
          "clone appended to div.cards");
    {
        wn_rect r = rect_of(clone);
        check("clone geometry", r.x > rect_of(card1).x && r.y == initial0.y &&
              r.w == 120.0f,
              "clone is the third card in the row");
        check("clone paint", wn_core_display_list(g_core)->image_vertex_count ==
              initial_images + 6, "one more image quad");
    }
    clone_image = wn_core_node_first_child(g_core, clone);
    clone_label = wn_core_node_next_sibling(g_core, clone_image);
    clone_text = wn_core_node_first_child(g_core, clone_label);
    check("clone internals", clone_image > 0 &&
          wn_core_node_image_src(g_core, clone_image) != NULL &&
          wn_core_node_is_text(g_core, clone_text),
          "image and text run cloned");
    event_begin(report, "cloned");
    write_counts(report);
    fputc(',', report);
    event_rect(report, "clone", clone);
    event_rect(report, "card1", card1);
    fprintf(report, "\"clone_image\":%d,\"clone_text\":%d,", clone_image, clone_text);
    event_end(report);

    /* -------------------------------------------- clone navigation (ABI v5) */
    {
        int nav_clone, nav_image, nav_label, nav_text;
        int nav_base_glyphs, nav_glyphs_before, nav_glyphs_after;
        int nav_parent_ok, nav_order_ok;
        wn_rect nested, moved;

        nav_base_glyphs = wn_core_display_list(g_core)->glyph_vertex_count;
        nav_clone = wn_core_clone(g_core, card0, card0);
        wn_core_relayout(g_core);
        wn_core_build_display_list(g_core);
        nested = rect_of(nav_clone);
        nav_glyphs_before = wn_core_display_list(g_core)->glyph_vertex_count;
        nav_image = wn_core_node_first_child(g_core, nav_clone);
        nav_label = wn_core_node_next_sibling(g_core, nav_image);
        nav_text = wn_core_node_first_child(g_core, nav_label);
        nav_parent_ok = wn_core_node_parent(g_core, nav_image) == nav_clone &&
                        wn_core_node_parent(g_core, nav_label) == nav_clone &&
                        wn_core_node_parent(g_core, nav_clone) == card0;
        nav_order_ok = nav_image >= 0 && nav_label >= 0 && nav_text >= 0 &&
                       wn_core_node_next_sibling(g_core, nav_image) == nav_label &&
                       wn_core_node_next_sibling(g_core, nav_label) < 0 &&
                       wn_core_node_first_child(g_core, nav_label) == nav_text &&
                       wn_core_node_next_sibling(g_core, nav_text) < 0;
        check("nav clone root", nav_clone > 0 && nav_image > 0 && nav_label > 0 &&
              nav_text > 0, "clone subtree reached from the root handle");
        check("nav parent", nav_parent_ok, "parent(first_child(clone)) == clone");
        check("nav order", nav_order_ok, "image precedes label; label is the last child");
        check("nav set_text", wn_core_set_text(g_core, nav_text, "Beta") == 0,
              "text child obtained by navigation accepts an override");
        wn_core_build_display_list(g_core);
        nav_glyphs_after = wn_core_display_list(g_core)->glyph_vertex_count;
        check("nav text paint", nav_glyphs_after == nav_glyphs_before - 6,
              "Beta draws one glyph quad fewer than the cloned Alpha");
        check("nav nested rect", nested.y > rect_of(card0).y &&
              nested.x >= rect_of(card0).x,
              "nested clone is laid out inside the template card");
        check("nav append", wn_core_append(g_core, cards, nav_clone) == 0,
              "navigated clone re-parented under div.cards");
        wn_core_relayout(g_core);
        wn_core_build_display_list(g_core);
        moved = rect_of(nav_clone);
        check("nav rect moved", moved.y > nested.y && moved.x == 10.0f &&
              moved.w == 120.0f, "the handle stays valid and the box moves to the next row");
        check("nav parent after append", wn_core_node_parent(g_core, nav_clone) == cards,
              "parent follows the re-parent");
        event_begin(report, "navigated");
        write_counts(report);
        fputc(',', report);
        fprintf(report,
                "\"nav_root\":%d,\"nav_image\":%d,\"nav_label\":%d,\"nav_text\":%d,"
                "\"nav_parent_ok\":%d,\"nav_order_ok\":%d,"
                "\"nav_glyphs_before\":%d,\"nav_glyphs_after\":%d,",
                nav_clone, nav_image, nav_label, nav_text, nav_parent_ok,
                nav_order_ok, nav_glyphs_before, nav_glyphs_after);
        fprintf(report, "\"nested\":{\"x\":%.2f,\"y\":%.2f,\"w\":%.2f,\"h\":%.2f},",
                nested.x, nested.y, nested.w, nested.h);
        event_rect(report, "moved", nav_clone);
        event_end(report);
        wn_core_remove(g_core, nav_clone);
        wn_core_relayout(g_core);
        wn_core_build_display_list(g_core);
        check("nav remove restores", wn_core_live_node_count(g_core) == UI_NODE_COUNT + 4 &&
              wn_core_display_list(g_core)->glyph_vertex_count == nav_base_glyphs,
              "navigated clone removed; clone stage counts restored");
    }

    /* ----------------------------------------------------------- set_image */
    check("set_image", wn_core_set_image(g_core, clone_image, "images/b.png") == 0,
          "WN_OK");
    wn_core_relayout(g_core);
    wn_core_build_display_list(g_core);
    check("set_image src",
          strcmp(wn_core_node_image_src(g_core, clone_image), "images/b.png") == 0 &&
          image_batch_has_src("images/b.png"),
          "new path reaches the image batch");
    check("set_image paint",
          wn_core_display_list(g_core)->image_vertex_count == initial_images + 6,
          "texture swap keeps one quad");
    check("set_image invalid",
          wn_core_set_image(g_core, text0, "images/a.png") == 1 &&
          wn_core_set_image(g_core, clone_image, NULL) == -1,
          "non-image and NULL rejected");
    event_begin(report, "imaged");
    write_counts(report);
    fputc(',', report);
    fprintf(report, "\"src\":\"%s\",\"found\":%d,",
            wn_core_node_image_src(g_core, clone_image),
            image_batch_has_src("images/b.png"));
    event_rect(report, "clone", clone);
    event_end(report);

    /* ------------------------------------------------------------ set_text */
    wn_core_set_text(g_core, clone_text, "Alpha");
    wn_core_build_display_list(g_core);
    alpha_quads = wn_core_display_list(g_core)->glyph_vertex_count;
    check("set_text shaped", wn_core_set_text(g_core, clone_text, "Beta") == 0,
          "new text accepted at runtime");
    check("set_text value", strcmp(wn_core_node_text(g_core, clone_text), "Beta") == 0,
          "override visible through the core");
    wn_core_build_display_list(g_core);
    beta_quads = wn_core_display_list(g_core)->glyph_vertex_count;
    check("set_text paint", beta_quads == alpha_quads - 6,
          "Beta draws one glyph fewer than Alpha");
    unsupported = wn_core_set_text(g_core, clone_text, "\xce\xa9");
    check("set_text unicode", unsupported == 0 &&
          strcmp(wn_core_node_text(g_core, clone_text), "\xce\xa9") == 0,
          "non-ASCII text is shaped at runtime");
    wn_core_build_display_list(g_core);
    check("set_text unicode paint",
          wn_core_display_list(g_core)->glyph_vertex_count == beta_quads - 18,
          "Omega draws a single glyph");
    check("set_text invalid", wn_core_set_text(g_core, image0, "A") == -1,
          "image node rejected");
    event_begin(report, "texted");
    write_counts(report);
    fputc(',', report);
    fprintf(report, "\"text\":\"%s\",\"unicode\":%d,",
            wn_core_node_text(g_core, clone_text), unsupported == 0);
    event_rect(report, "clone", clone);
    event_end(report);

    /* -------------------------------------------------------------- remove */
    remove_rc = wn_core_remove(g_core, clone);
    wn_core_relayout(g_core);
    wn_core_build_display_list(g_core);
    check("remove", remove_rc == 0 && wn_core_live_node_count(g_core) == UI_NODE_COUNT,
          "subtree released");
    check("remove geometry", rect_of(card0).x == initial0.x &&
          rect_of(card1).x == initial1.x, "layout restored");
    check("remove paint", wn_core_display_list(g_core)->vertex_count ==
          initial_vertices, "paint restored");
    check("remove root", wn_core_remove(g_core, 0) != 0, "document root is protected");
    event_begin(report, "removed");
    write_counts(report);
    fputc(',', report);
    event_rect(report, "card0", card0);
    event_rect(report, "card1", card1);
    event_end(report);

    /* -------------------------------------------------------------- append */
    clone = wn_core_clone(g_core, card0, card0);
    wn_core_relayout(g_core);
    check("append nested", clone > 0 && wn_core_node_parent(g_core, clone) == card0,
          "clone started inside the template card");
    cycle_rc = wn_core_append(g_core, clone, cards);
    append_rc = wn_core_append(g_core, cards, clone);
    wn_core_relayout(g_core);
    wn_core_build_display_list(g_core);
    check("append moved", append_rc == 0 && wn_core_node_parent(g_core, clone) == cards,
          "re-parented under div.cards");
    check("append cycle", cycle_rc != 0, "moving an ancestor under a descendant is rejected");
    check("append root", wn_core_append(g_core, cards, 0) != 0, "root cannot be moved");
    event_begin(report, "appended");
    write_counts(report);
    fputc(',', report);
    fprintf(report, "\"moved\":%d,\"cycle_rejected\":%d,",
            append_rc == 0, cycle_rc != 0);
    event_rect(report, "clone", clone);
    event_end(report);
    wn_core_remove(g_core, clone);
    wn_core_relayout(g_core);

    /* -------------------------------------------------- arena growth (A4) */
    {
        enum { kStressClones = 1500, kStressScale = 3000 };
        int body = wn_core_find_node(g_core, "body.root");
        int rebuilds_before = wn_core_hierarchy_rebuilds(g_core);
        double clone_ms = 0.0;
        double remove_ms = 0.0;
        int created = 0;
        int alive = 0;
        int laid_out = 0;
        int live;

        live = body >= 0 ? stress_round(card0, body, kStressClones, &clone_ms,
                                        &remove_ms, &created, &alive, &laid_out)
                         : -1;
        check("stress container", body >= 0, "body.root is the list container");
        check("stress clones created",
              created == kStressClones && live == UI_NODE_COUNT + 4 * kStressClones,
              "1,500 clones of the 4-node card subtree");
        check("stress clones alive", alive == created,
              "every clone is live under the container");
        check("stress layout", laid_out == created &&
              wn_core_node_count(g_core) >= UI_NODE_COUNT + 4 * kStressClones,
              "arena and every clone box grew");
        check("stress hierarchy", wn_core_hierarchy_rebuilds(g_core) == rebuilds_before,
              "no full hierarchy rebuild per clone/remove");
        check("stress removal", wn_core_live_node_count(g_core) == UI_NODE_COUNT,
              "only the compiled tree stays live");
        printf("stress: clones=%d clone_ms=%.2f remove_ms=%.2f rebuilds=%d arena=%d\n",
               created, clone_ms, remove_ms, wn_core_hierarchy_rebuilds(g_core),
               wn_core_node_count(g_core));
        event_begin(report, "stress");
        write_counts(report);
        fprintf(report, ",\"clones\":%d,\"alive\":%d,\"laid_out\":%d,\"arena\":%d,"
                "\"rebuilds\":%d,",
                created, alive, laid_out, wn_core_node_count(g_core),
                wn_core_hierarchy_rebuilds(g_core));
        event_end(report);

        live = body >= 0 ? stress_round(card0, body, kStressScale, &clone_ms,
                                        &remove_ms, &created, &alive, &laid_out)
                         : -1;
        check("stress 3000 clones",
              created == kStressScale && live == UI_NODE_COUNT + 4 * kStressScale &&
                  alive == created && laid_out == created,
              "3,000 clones alive, laid out and removed");
        check("stress 3000 hierarchy",
              wn_core_hierarchy_rebuilds(g_core) == rebuilds_before,
              "hierarchy stays incremental at 3,000 clones");
        printf("stress: clones=%d clone_ms=%.2f remove_ms=%.2f rebuilds=%d arena=%d\n",
               created, clone_ms, remove_ms, wn_core_hierarchy_rebuilds(g_core),
               wn_core_node_count(g_core));
    }

    /* ------------------------------------------------------- clear children */
    clear_rc = wn_core_clear_children(g_core, cards);
    wn_core_relayout(g_core);
    wn_core_build_display_list(g_core);
    check("clear_children", clear_rc == 0 && wn_core_node_first_child(g_core, cards) < 0 &&
          wn_core_live_node_count(g_core) == UI_NODE_COUNT - 8,
          "two 4-node cards removed");
    check("clear paint", wn_core_display_list(g_core)->image_vertex_count == 0 &&
          wn_core_node_is_alive(g_core, cards), "no image quads; container stays");
    event_begin(report, "cleared");
    write_counts(report);
    fputc(',', report);
    fprintf(report, "\"first_child\":%d,", wn_core_node_first_child(g_core, cards));
    event_end(report);
    fclose(report);

    wn_core_destroy(g_core);
    printf("dynamic_core_test: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
