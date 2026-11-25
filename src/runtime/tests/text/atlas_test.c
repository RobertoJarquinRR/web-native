#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text/text_atlas.h"
#include "text/text_runtime.h"

#define RUN_STRINGS 300

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

int main(void)
{
    wn_text_atlas *atlas = wn_text_atlas_create(NULL, 0, 0);
    const wn_atlas_run *run;
    const wn_atlas_run *again;
    const wn_atlas_glyph *glyph;
    const wn_atlas_glyph *repeat;
    unsigned char seed[4] = {7, 9, 11, 13};
    wn_text_atlas *seeded;
    wn_text_cache_stats before;
    wn_text_cache_stats after;
    unsigned char *full_seed;
    wn_text_atlas *full;
    int ink = 0;

    check("atlas is created", atlas != NULL && wn_text_atlas_is_ready(atlas));
    if (atlas == NULL) {
        printf("atlas_test: FAIL\n");
        return 1;
    }
    check("atlas starts at 1024", wn_text_atlas_width(atlas) == 1024);

    run = wn_text_atlas_shape(atlas, "Hola", "Segoe UI", 16.0f, 400);
    check("run is shaped", run != NULL && run->count == 4);
    if (run == NULL) {
        printf("atlas_test: FAIL\n");
        wn_text_atlas_destroy(atlas);
        return 1;
    }
    printf("  run \"Hola\" 16px: %d glyphs, width %.2f, ascent %.2f\n",
           run->count, run->width, run->ascent);

    glyph = wn_text_atlas_glyph(atlas, "Segoe UI", 16.0f, 400, run->glyphs[0].glyph);
    check("glyph is packed", glyph != NULL && glyph->width > 0 && glyph->height > 0);
    if (glyph == NULL) {
        printf("atlas_test: FAIL\n");
        wn_text_atlas_destroy(atlas);
        return 1;
    }
    printf("  glyph %d: %dx%d bearing=(%d,%d) uv=(%.4f,%.4f)-(%.4f,%.4f)\n",
           run->glyphs[0].glyph, glyph->width, glyph->height, glyph->bearing_x,
           glyph->bearing_y, glyph->u0, glyph->v0, glyph->u1, glyph->v1);
    check("uv stays inside the atlas", glyph->u0 >= 0.0f && glyph->u1 <= 1.0f &&
                                       glyph->v0 >= 0.0f && glyph->v1 <= 1.0f);
    check("uv matches the bitmap size", (glyph->u1 - glyph->u0) * 1024.0f >= (float)glyph->width &&
                                        (glyph->u1 - glyph->u0) * 1024.0f <= (float)glyph->width + 0.01f);

    for (int row = 0; row < glyph->height; ++row) {
        for (int col = 0; col < glyph->width; ++col) {
            int x = (int)(glyph->u0 * 1024.0f) + col;
            int y = (int)(glyph->v0 * 1024.0f) + row;
            if (wn_text_atlas_pixels(atlas)[y * 1024 + x] > 0) {
                ++ink;
            }
        }
    }
    check("atlas holds the glyph ink", ink > 0);

    repeat = wn_text_atlas_glyph(atlas, "Segoe UI", 16.0f, 400, run->glyphs[0].glyph);
    check("glyph lookup is cached", repeat == glyph);
    again = wn_text_atlas_shape(atlas, "Hola", "Segoe UI", 16.0f, 400);
    check("run lookup is cached", again == run);

    /* -------------------------------------------------- run cache scaling */
    memset(&before, 0, sizeof(before));
    wn_text_atlas_cache_stats(atlas, &before);
    {
        int run_ok = 1;
        char text[32];
        for (int i = 0; i < RUN_STRINGS; ++i) {
            snprintf(text, sizeof(text), "run-string-%03d", i);
            if (wn_text_atlas_shape(atlas, text, "Segoe UI", 16.0f, 400) == NULL) {
                run_ok = 0;
                break;
            }
        }
        check("300 distinct runs are shaped", run_ok);
    }
    wn_text_atlas_cache_stats(atlas, &after);
    printf("  run cache after %d strings: hits=%d misses=%d evictions=%d\n",
           RUN_STRINGS, after.hits, after.misses, after.evictions);
    check("300 distinct runs fit without evicting",
          after.evictions == before.evictions &&
              after.misses - before.misses == RUN_STRINGS);
    check("existing hits are preserved", after.hits == before.hits);
    check("run reuse is a hit",
          wn_text_atlas_shape(atlas, "run-string-042", "Segoe UI", 16.0f, 400) != NULL);
    wn_text_atlas_cache_stats(atlas, &after);
    check("run reuse increments hits", after.hits == before.hits + 1);
    check("run reuse does not miss", after.misses == before.misses + RUN_STRINGS);

    seeded = wn_text_atlas_create(seed, 2, 2);
    check("seed atlas is created", seeded != NULL);
    if (seeded != NULL) {
        check("seed pixels are copied",
              wn_text_atlas_pixels(seeded)[0] == 7 && wn_text_atlas_pixels(seeded)[1] == 9);
        check("seeded atlas packs below the seed", wn_text_atlas_height(seeded) == 1024);
        wn_text_atlas_destroy(seeded);
    }

    /* D14: a full atlas cannot pack, so the glyph must not be cached. */
    full_seed = (unsigned char *)calloc(1024 * 1024, 1);
    check("full seed allocated", full_seed != NULL);
    if (full_seed != NULL) {
        full = wn_text_atlas_create(full_seed, 1024, 1024);
        check("full atlas created", full != NULL);
        if (full != NULL) {
            const wn_atlas_run *probe_run = wn_text_atlas_shape(full, "Hola", "Segoe UI", 16.0f, 400);
            const wn_atlas_glyph *failed;
            const wn_atlas_glyph *retried;
            check("full atlas can still shape", probe_run != NULL && probe_run->count == 4);
            failed = probe_run != NULL
                         ? wn_text_atlas_glyph(full, "Segoe UI", 16.0f, 400,
                                               probe_run->glyphs[0].glyph)
                         : NULL;
            check("failed pack returns NULL", failed == NULL);
            retried = probe_run != NULL
                          ? wn_text_atlas_glyph(full, "Segoe UI", 16.0f, 400,
                                                probe_run->glyphs[0].glyph)
                          : NULL;
            check("failed pack is not cached", retried == NULL);
            wn_text_atlas_destroy(full);
        }
        free(full_seed);
    }

    /* F1: failed run inserts must not consume cache capacity. */
    {
        wn_text_atlas *fresh = wn_text_atlas_create(NULL, 0, 0);
        check("failure atlas created", fresh != NULL);
        if (fresh != NULL) {
            int all_failed = 1;
            char cache_name[32];
            wn_text_test_fail_next_shapes(1200);
            for (int i = 0; i < 1200; ++i) {
                snprintf(cache_name, sizeof(cache_name), "fail-run-%03d", i);
                if (wn_text_atlas_shape(fresh, cache_name, "Segoe UI", 16.0f, 400) != NULL) {
                    all_failed = 0;
                }
            }
            check("1200 injected run failures fail", all_failed);
            check("run cache recovers after a failure storm",
                  wn_text_atlas_shape(fresh, "run-cache-recovery", "Segoe UI", 16.0f,
                                      400) != NULL);
            wn_text_atlas_destroy(fresh);
        }
    }

    /* Warm run cache: failed inserts must not evict published entries. */
    {
        wn_text_atlas *warm = wn_text_atlas_create(NULL, 0, 0);
        check("warm atlas created", warm != NULL);
        if (warm != NULL) {
            const wn_atlas_run *saved[1024];
            char warm_names[1024][24];
            int filled = 0;
            int failed = 1;
            int survivors = 0;
            char storm_name[32];
            wn_text_cache_stats warm_before;
            wn_text_cache_stats after_failures;
            wn_text_cache_stats after_inserts;
            for (int i = 0; i < 1024; ++i) {
                snprintf(warm_names[i], sizeof(warm_names[i]), "warm-run-%03d", i);
                saved[i] = wn_text_atlas_shape(warm, warm_names[i], "Segoe UI", 16.0f, 400);
                if (saved[i] != NULL) {
                    ++filled;
                }
            }
            check("run cache warms to capacity", filled == 1024);
            memset(&warm_before, 0, sizeof(warm_before));
            wn_text_atlas_cache_stats(warm, &warm_before);
            wn_text_test_fail_next_shapes(1200);
            for (int i = 0; i < 1200; ++i) {
                snprintf(storm_name, sizeof(storm_name), "warm-fail-run-%03d", i);
                if (wn_text_atlas_shape(warm, storm_name, "Segoe UI", 16.0f, 400) != NULL) {
                    failed = 0;
                }
            }
            check("1200 warm run failures fail", failed);
            memset(&after_failures, 0, sizeof(after_failures));
            wn_text_atlas_cache_stats(warm, &after_failures);
            check("failed run inserts do not evict",
                  after_failures.evictions == warm_before.evictions);
            for (int i = 0; i < 1024; ++i) {
                if (wn_text_atlas_shape(warm, warm_names[i], "Segoe UI", 16.0f, 400) ==
                    saved[i]) {
                    ++survivors;
                }
            }
            printf("  warm run cache survivors=%d/1024 after 1200 failures\n", survivors);
            check("all warm runs survive the failure storm", survivors == 1024);
            {
                const wn_atlas_run *added[8];
                char added_names[8][24];
                int added_ok = 1;
                int kept = 0;
                for (int i = 0; i < 8; ++i) {
                    snprintf(added_names[i], sizeof(added_names[i]), "warm-after-run-%d", i);
                    added[i] = wn_text_atlas_shape(warm, added_names[i], "Segoe UI", 16.0f,
                                                   400);
                    if (added[i] == NULL) {
                        added_ok = 0;
                    }
                }
                check("8 valid run inserts after the storm succeed", added_ok);
                memset(&after_inserts, 0, sizeof(after_inserts));
                wn_text_atlas_cache_stats(warm, &after_inserts);
                check("real run inserts evict exactly once",
                      after_inserts.evictions - warm_before.evictions == 8);
                printf("  warm run cache evictions: failures=%d inserts=%d\n",
                       after_failures.evictions - warm_before.evictions,
                       after_inserts.evictions - warm_before.evictions);
                for (int i = 0; i < 8; ++i) {
                    if (wn_text_atlas_shape(warm, added_names[i], "Segoe UI", 16.0f, 400) ==
                        added[i]) {
                        ++kept;
                    }
                }
                check("8 run inserts after the storm stay cached", kept == 8);
            }
            wn_text_atlas_destroy(warm);
        }
    }

    wn_text_atlas_destroy(atlas);
    wn_text_runtime_shutdown();
    if (failures != 0) {
        printf("atlas_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }
    printf("atlas_test: PASS (%d checks)\n", checks);
    return 0;
}
