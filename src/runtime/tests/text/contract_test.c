/*
 * contract_test.c - backend-independent text contracts.
 *
 * Every check runs against fake_text.c through wn_text_test_use_backend(), so
 * the contract logic needs no DirectWrite and no fixture blob. What is locked
 * here:
 *
 *   1. `max_width` in wn_text_ops.measure is a hint: the engine's greedy
 *      breaker owns line count, line width and node geometry.
 *   2. Greedy space breaking: exact slices, trailing spaces, overflow.
 *   3. `set_text` changes both measurement and the painted glyph geometry.
 *   4. Line layouts are keyed by text/family/size/weight/width (no bleed) and
 *      the cache counters behave.
 *   5. A failed line insert never consumes capacity or evicts a published
 *      entry (the probe finding, now without the real backend).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "document/wn_document_internal.h"
#include "text/text_runtime.h"
#include "wn_core.h"

/* Provided by fake_text.c; declared here so the test needs no extra header. */
const wn_text_ops *wn_text_fake(void);

#define FAKE "fake"
#define FAKE_SIZE 16.0f
#define FAKE_ADVANCE (FAKE_SIZE * 0.5f)      /* 8 px per codepoint */
#define FAKE_LINE (FAKE_SIZE * 1.25f)        /* 20 px line box */

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

/* Minimal two-node document: a block root with one text child. The document
 * stays alive for the whole run and is not tracked by the blob loader, so the
 * core never frees it. */
static wn_document g_doc;
static UiNode g_nodes[2];
static const char *const g_names[2] = {"body.root", "body.root/span.text"};

static wn_core *build_scene(void)
{
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(&g_doc, 0, sizeof(g_doc));

    g_nodes[0].parent = -1;
    g_nodes[0].kind = R2_NODE_BLOCK;
    g_nodes[0].width = -1.0f;
    g_nodes[0].height = -1.0f;
    g_nodes[0].flex_shrink = 1.0f;

    g_nodes[1].parent = 0;
    g_nodes[1].kind = R2_NODE_TEXT;
    g_nodes[1].text = "aa bb";
    g_nodes[1].font_family = FAKE;
    g_nodes[1].font_size = FAKE_SIZE;
    g_nodes[1].font_weight = 400;
    g_nodes[1].text_color = 0xFF000000u;
    g_nodes[1].width = -1.0f;
    g_nodes[1].height = -1.0f;
    g_nodes[1].flex_shrink = 1.0f;

    g_doc.nodes = g_nodes;
    g_doc.node_count = 2;
    g_doc.names = g_names;
    g_doc.viewport_width = 320;
    g_doc.viewport_height = 200;
    g_doc.refs = WN_DOCUMENT_STATIC_REFS;

    return wn_core_create(&g_doc);
}

int main(void)
{
    const wn_text_line_layout *one;
    const wn_text_line_layout *wrapped;
    const wn_text_line_layout *long_word;
    const wn_text_line_layout *wide;
    const wn_text_line_layout *big;
    const wn_text_line_layout *again;
    const wn_text_line_layout *bold;
    wn_text_metrics hint;
    wn_text_metrics plain;
    wn_text_metrics measured;
    wn_text_glyph glyphs[8];
    wn_text_shaped shaped;
    wn_text_bitmap bitmap;
    unsigned char pixels[4];
    char face[96];
    wn_core *core;

    wn_text_test_use_backend(wn_text_fake());

    /* ------------------------------------------------ fake backend metrics */
    memset(&shaped, 0, sizeof(shaped));
    check("fake shapes one glyph per codepoint",
          wn_text_shape_string("aa bb", FAKE, FAKE_SIZE, 400, glyphs, 8, &shaped) == 0 &&
              shaped.glyph_count == 5);
    check("fake advances are half the font size",
          shaped.glyph_count == 5 && near(glyphs[0].advance, FAKE_ADVANCE) &&
              near(glyphs[4].x, 4.0f * FAKE_ADVANCE));
    memset(&bitmap, 0, sizeof(bitmap));
    memset(pixels, 0, sizeof(pixels));
    check("fake rasterizes a 2x2 bitmap with ink",
          wn_text_rasterize_glyph(FAKE, FAKE_SIZE, 400, glyphs[0].glyph, pixels,
                                  (int)sizeof(pixels), &bitmap) == 0 &&
              bitmap.width == 2 && bitmap.height == 2 &&
              (int)pixels[0] + (int)pixels[1] + (int)pixels[2] + (int)pixels[3] > 0);
    check("fake claims only its own family",
          wn_text_resolve_family("nope, fake", face, (int)sizeof(face)) == 0 &&
              strcmp(face, FAKE) == 0);

    /* ------------------------------- 1. the backend hint is ignored by layout */
    memset(&hint, 0, sizeof(hint));
    check("the fake lies when asked for a width",
          wn_text_measure_string("aa bb", FAKE, FAKE_SIZE, 400, 1000.0f, &hint) == 0 &&
              near(hint.width, 1000.0f));
    check("the fake hint would report three line boxes",
          near(hint.height, 3.0f * FAKE_LINE));
    memset(&plain, 0, sizeof(plain));
    check("without a hint the fake measures its own advances",
          wn_text_measure_string("aa bb", FAKE, FAKE_SIZE, 400, 0.0f, &plain) == 0 &&
              near(plain.width, 5.0f * FAKE_ADVANCE) && near(plain.height, FAKE_LINE));

    one = wn_text_line_layout_get("aa bb", FAKE, FAKE_SIZE, 400, 1000.0f);
    check("a string that fits stays on one line despite the hint",
          one != NULL && one->line_count == 1);
    check("engine line width comes from advances, not max_width",
          one != NULL && near(one->width, 40.0f));
    check("engine line box comes from the meter, not the hint",
          one != NULL && near(one->line_height, FAKE_LINE) &&
              near(one->height, FAKE_LINE));
    check("engine metrics disagree with the hint on purpose",
          one != NULL && hint.width > one->width + 1.0f &&
              hint.height > one->height + 1.0f);

    /* ------------------------------------------- 2. greedy space breaking */
    wrapped = wn_text_line_layout_get("aa bb cc", FAKE, FAKE_SIZE, 400, 45.0f);
    check("two words fit at 45px -> two lines",
          wrapped != NULL && wrapped->line_count == 2);
    if (wrapped != NULL && wrapped->line_count == 2) {
        check("first slice is \"aa bb\"", strcmp(wrapped->lines[0].text, "aa bb") == 0);
        check("first slice spans bytes 0..5",
              wrapped->lines[0].start == 0 && wrapped->lines[0].end == 5);
        check("first slice is 40px wide", near(wrapped->lines[0].width, 40.0f));
        check("second slice is \"cc\"", strcmp(wrapped->lines[1].text, "cc") == 0);
        check("second slice spans bytes 6..8",
              wrapped->lines[1].start == 6 && wrapped->lines[1].end == 8);
        check("second slice is 16px wide", near(wrapped->lines[1].width, 16.0f));
        check("wrapped width is the widest line", near(wrapped->width, 40.0f));
        check("wrapped height is lines * line box", near(wrapped->height, 2.0f * FAKE_LINE));
    }

    long_word = wn_text_line_layout_get("abcdefghij", FAKE, FAKE_SIZE, 400, 45.0f);
    check("a word wider than the box overflows on one line",
          long_word != NULL && long_word->line_count == 1);
    check("the overflowing word keeps its advance width",
          long_word != NULL && near(long_word->width, 80.0f) &&
              long_word->width > 45.0f);

    wide = wn_text_line_layout_get("aa bb cc", FAKE, FAKE_SIZE, 400, 400.0f);
    check("a wide box keeps the same string on one line",
          wide != NULL && wide->line_count == 1 && near(wide->width, 64.0f));

    /* -------------------------------- 3. set_text drives measurement and paint */
    core = build_scene();
    check("minimal core created", core != NULL);
    if (core != NULL) {
        const wn_rect *rects = wn_core_rects(core, NULL);
        const wn_display_list *list;
        int glyph_vertices;

        check("text child is node 1", wn_core_node_is_text(core, 1) == 1);
        check("layout sizes text by the engine advance", near(rects[1].w, 40.0f));
        check("layout sizes the box by one line, not three",
              near(rects[1].h, FAKE_LINE));

        wn_core_build_display_list(core);
        list = wn_core_display_list(core);
        glyph_vertices = list != NULL ? list->glyph_vertex_count : 0;
        check("initial text paints 5 glyph quads", glyph_vertices == 5 * 6);
        check("painted quads have area",
              list != NULL && glyph_vertices >= 6 &&
                  list->glyph_vertices[1].x > list->glyph_vertices[0].x &&
                  list->glyph_vertices[5].y < list->glyph_vertices[0].y);
        check("painted glyphs use a batch", list != NULL && list->glyph_batch_count >= 1);

        check("set_text accepts a longer override",
              wn_core_set_text(core, 1, "webnative runtime engine") == 0);
        check("node_text reports the override",
              wn_core_node_text(core, 1) != NULL &&
                  strcmp(wn_core_node_text(core, 1), "webnative runtime engine") == 0);
        wn_core_relayout(core);
        rects = wn_core_rects(core, NULL);
        check("the laid-out rect grows with the override",
              near(rects[1].w, 24.0f * FAKE_ADVANCE) &&
                  rects[1].w > 40.0f);
        check("the override still ignores the backend hint",
              near(rects[1].h, FAKE_LINE));
        wn_core_build_display_list(core);
        list = wn_core_display_list(core);
        check("the display list paints the new glyph count",
              list != NULL && list->glyph_vertex_count == 24 * 6);

        check("set_text restores the original",
              wn_core_set_text(core, 1, "aa bb") == 0);
        wn_core_relayout(core);
        rects = wn_core_rects(core, NULL);
        wn_core_build_display_list(core);
        list = wn_core_display_list(core);
        check("restoring shrinks the rect back", near(rects[1].w, 40.0f));
        check("restoring paints 5 glyph quads again",
              list != NULL && list->glyph_vertex_count == 5 * 6);
    }

    /* ----------------------------------------- 4. cache isolation and stats */
    wn_text_test_use_backend(wn_text_fake());
    {
        wn_text_cache_stats stats;

        memset(&stats, 0, sizeof(stats));
        wn_text_runtime_cache_stats(&stats);
        check("the hook starts the backend stats cold",
              stats.hits == 0 && stats.misses == 0 && stats.evictions == 0);
        check("the hook starts the miss counter cold",
              wn_text_runtime_cache_misses() == 0);

        wrapped = wn_text_line_layout_get("aa bb cc", FAKE, FAKE_SIZE, 400, 45.0f);
        wide = wn_text_line_layout_get("aa bb cc", FAKE, FAKE_SIZE, 400, 400.0f);
        big = wn_text_line_layout_get("aa bb cc", FAKE, 32.0f, 400, 90.0f);
        bold = wn_text_line_layout_get("aa bb cc", FAKE, FAKE_SIZE, 700, 45.0f);
        again = wn_text_line_layout_get("aa bb cc", FAKE, FAKE_SIZE, 400, 45.0f);

        check("narrow layout wraps", wrapped != NULL && wrapped->line_count == 2);
        check("wide layout stays on one line",
              wide != NULL && wide->line_count == 1 && near(wide->width, 64.0f));
        check("a bigger size uses the bigger advance",
              big != NULL && big->line_count == 2 &&
                  near(big->lines[0].width, 80.0f) && near(big->line_height, 40.0f));
        check("weight is part of the layout key",
              bold != NULL && bold != wrapped && bold->line_count == 2);
        check("repeating a key returns the same entry", again == wrapped);
        check("the hit does not disturb the wide layout",
              wide != NULL && wide->line_count == 1 && near(wide->width, 64.0f));
        check("the hit keeps the narrow slices",
              again != NULL && again->line_count == 2 &&
                  strcmp(again->lines[1].text, "cc") == 0);

        memset(&stats, 0, sizeof(stats));
        wn_text_runtime_cache_stats(&stats);
        check("backend stats stay sane (misses only, no evictions)",
              stats.hits == 0 && stats.misses > 0 && stats.evictions == 0);
        check("cache_misses agrees with cache_stats",
              wn_text_runtime_cache_misses() == stats.misses);
    }

    /* ------------------ 5. failed inserts keep published line entries alive */
    wn_text_test_use_backend(wn_text_fake());
    {
        const wn_text_line_layout *saved[512];
        char warm_names[512][24];
        char storm_name[32];
        int filled = 0;
        int failed = 1;
        int survivors = 0;
        int added_ok = 1;
        int kept = 0;
        const wn_text_line_layout *added[8];
        char added_names[8][24];

        for (int i = 0; i < 512; ++i) {
            snprintf(warm_names[i], sizeof(warm_names[i]), "warm-line-%03d", i);
            saved[i] = wn_text_line_layout_get(warm_names[i], FAKE, FAKE_SIZE, 400, 0.0f);
            if (saved[i] != NULL) {
                filled += 1;
            }
        }
        check("line cache warms to 512 entries", filled == 512);

        wn_text_test_fail_next_lines(600);
        for (int i = 0; i < 600; ++i) {
            snprintf(storm_name, sizeof(storm_name), "warm-fail-%03d", i);
            if (wn_text_line_layout_get(storm_name, FAKE, FAKE_SIZE, 400, 0.0f) != NULL) {
                failed = 0;
            }
        }
        check("600 injected line failures all fail", failed);

        for (int i = 0; i < 512; ++i) {
            if (wn_text_line_layout_get(warm_names[i], FAKE, FAKE_SIZE, 400, 0.0f) ==
                saved[i]) {
                survivors += 1;
            }
        }
        printf("  warm line cache survivors=%d/512 after 600 failures\n", survivors);
        check("no failed insert evicts a published line entry", survivors == 512);

        for (int i = 0; i < 8; ++i) {
            snprintf(added_names[i], sizeof(added_names[i]), "warm-after-%d", i);
            added[i] = wn_text_line_layout_get(added_names[i], FAKE, FAKE_SIZE, 400, 0.0f);
            if (added[i] == NULL) {
                added_ok = 0;
            }
        }
        check("valid inserts still work after the failure storm", added_ok);
        for (int i = 0; i < 8; ++i) {
            if (wn_text_line_layout_get(added_names[i], FAKE, FAKE_SIZE, 400, 0.0f) ==
                added[i]) {
                kept += 1;
            }
        }
        check("the post-storm inserts stay cached", kept == 8);
    }

    if (core != NULL) {
        wn_core_destroy(core);
    }

    /* -------------------------------------------- NULL restores unavailability */
    wn_text_test_use_backend(NULL);
    memset(&measured, 0, sizeof(measured));
    check("the NULL hook fails measurement closed",
          wn_text_measure_string("aa", FAKE, FAKE_SIZE, 400, 0.0f, &measured) != 0);
    check("the NULL hook leaves layout unavailable",
          wn_text_line_layout_get("aa", FAKE, FAKE_SIZE, 400, 0.0f) == NULL);

    if (failures != 0) {
        printf("contract_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }
    printf("contract_test: PASS (%d checks)\n", checks);
    return 0;
}
