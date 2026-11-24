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
    check("set_text shaped", wn_core_set_text(g_core, clone_text, "Beta") == 0,
          "pre-shaped string accepted");
    check("set_text value", strcmp(wn_core_node_text(g_core, clone_text), "Beta") == 0,
          "override visible through the core");
    wn_core_build_display_list(g_core);
    check("set_text paint",
          wn_core_display_list(g_core)->glyph_vertex_count == initial_glyphs + 4 * 6,
          "Beta renders four pre-shaped glyph quads");
    unsupported = wn_core_set_text(g_core, clone_text, "\xce\xa9");
    check("set_text unsupported", unsupported == 1 &&
          strcmp(wn_core_node_text(g_core, clone_text), "Beta") == 0,
          "Omega is not pre-shaped; previous text kept");
    check("set_text invalid", wn_core_set_text(g_core, image0, "A") == -1,
          "image node rejected");
    event_begin(report, "texted");
    write_counts(report);
    fputc(',', report);
    fprintf(report, "\"text\":\"%s\",\"unsupported\":%d,",
            wn_core_node_text(g_core, clone_text), unsupported == 1);
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
