/*
 * image_cache_test.c - decoded-texture LRU budget (A3).
 *
 * Renders a page with three image slots while cycling twelve 512x512 posters
 * (1 MiB of RGBA each) under WN_IMAGE_CACHE_MB=4. The cache must stay inside
 * the budget, keep serving the current frame, and keep its survivors across a
 * repeated frame instead of dropping and re-decoding the whole library.
 */
#include "wn_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 3
#define ROUNDS 4
#define POSTER_BYTES (512u * 512u * 4u)

static int fail(const char *message, long long value, long long expected)
{
    fprintf(stderr, "image_cache_test: FAIL %s (got %lld, expected %lld)\n",
            message, value, expected);
    return 1;
}

int main(int argc, char **argv)
{
    const char *png_path = argc >= 2 ? argv[1] : "image_cache.png";
    const char *missing_png = argc >= 3 ? argv[2] : "image_cache_missing.png";
    const char *budget_env = getenv("WN_IMAGE_CACHE_MB");
    long long budget_mb = budget_env != NULL ? atoll(budget_env) : 256;
    long long budget = budget_mb * 1024 * 1024;
    long long expected_count = budget / (long long)POSTER_BYTES;
    wn_node_id slots[SLOTS];
    wn_view *view;
    long long last_count = -1;
    long long last_bytes = -1;
    int round;

    if (budget <= 0) {
        return fail("WN_IMAGE_CACHE_MB must be positive for this test", budget, 1);
    }
    view = wn_view_create_offscreen(800, 300);
    if (view == NULL) {
        fprintf(stderr, "image_cache_test: create failed: %s\n", wn_last_error());
        return 1;
    }
    if (wn_view_load(view, "", NULL, NULL) != WN_OK) {
        fprintf(stderr, "image_cache_test: load failed: %s\n", wn_last_error());
        wn_view_destroy(view);
        return 1;
    }
    slots[0] = wn_view_find(view, "body.root/img.slot0");
    slots[1] = wn_view_find(view, "body.root/img.slot1");
    slots[2] = wn_view_find(view, "body.root/img.slot2");
    if (slots[0] == 0 || slots[1] == 0 || slots[2] == 0) {
        fprintf(stderr, "image_cache_test: fixture image slots not found\n");
        wn_view_destroy(view);
        return 1;
    }

    printf("image_cache_test: backend=%s budget=%lld bytes (%lld posters)\n",
           wn_view_backend(view), budget, expected_count);

    for (round = 0; round < ROUNDS; ++round) {
        long long count;
        long long bytes;
        char path[64];
        int slot;

        for (slot = 0; slot < SLOTS; ++slot) {
            snprintf(path, sizeof(path), "posters/p%d.png", round * SLOTS + slot);
            if (wn_view_set_image(view, slots[slot], path) != WN_OK) {
                fprintf(stderr, "image_cache_test: set_image(%s) failed: %s\n",
                        path, wn_last_error());
                wn_view_destroy(view);
                return 1;
            }
        }
        if (wn_view_render(view) != WN_OK) {
            fprintf(stderr, "image_cache_test: render failed: %s\n", wn_last_error());
            wn_view_destroy(view);
            return 1;
        }
        count = wn_view_image_cache_count(view);
        bytes = wn_view_image_cache_bytes(view);
        {
            int vertices = 0, glyphs = 0, images = 0;
            wn_view_stats cache_stats;
            memset(&cache_stats, 0, sizeof(cache_stats));
            wn_view_display_counts(view, &vertices, &glyphs, &images);
            wn_view_get_stats(view, &cache_stats);
            printf("image_cache_test: round=%d count=%lld bytes=%lld "
                   "display(vertices=%d glyphs=%d images=%d) failures=%d error=%s\n",
                   round, count, bytes, vertices, glyphs, images,
                   cache_stats.image_failures,
                   wn_last_error() != NULL ? wn_last_error() : "(none)");
        }
        if (count < SLOTS || count > expected_count) {
            wn_view_destroy(view);
            return fail("cache entry count out of range", count, expected_count);
        }
        if (bytes < (long long)SLOTS * POSTER_BYTES || bytes > budget) {
            wn_view_destroy(view);
            return fail("cache bytes exceeded the budget", bytes, budget);
        }
        if (round == 0 && wn_view_readback(view, png_path) != WN_OK) {
            fprintf(stderr, "image_cache_test: readback failed: %s\n", wn_last_error());
            wn_view_destroy(view);
            return 1;
        }
        last_count = count;
        last_bytes = bytes;
    }

    /* Same frame again: every slot must hit the cache and survivors must stay. */
    for (round = 0; round < SLOTS; ++round) {
        char path[64];
        snprintf(path, sizeof(path), "posters/p%d.png",
                 (ROUNDS - 1) * SLOTS + round);
        if (wn_view_set_image(view, slots[round], path) != WN_OK ||
            wn_view_render(view) != WN_OK) {
            fprintf(stderr, "image_cache_test: repeated frame failed: %s\n",
                    wn_last_error());
            wn_view_destroy(view);
            return 1;
        }
    }
    {
        long long count = wn_view_image_cache_count(view);
        long long bytes = wn_view_image_cache_bytes(view);
        printf("image_cache_test: repeat count=%lld bytes=%lld\n", count, bytes);
        if (count != last_count || bytes != last_bytes) {
            wn_view_destroy(view);
            return fail("repeated frame thrashed the cache", count, last_count);
        }
        if (count != expected_count || bytes != budget) {
            wn_view_destroy(view);
            return fail("cache did not retain its LRU survivors", count, expected_count);
        }
    }

    /* D3: one undecodable poster must not fail the frame or hide the valid ones. */
    {
        wn_view_stats stats;
        memset(&stats, 0, sizeof(stats));
        long long count;
        if (wn_view_set_image(view, slots[0], "posters/p0.png") != WN_OK ||
            wn_view_set_image(view, slots[1], "posters/missing.png") != WN_OK ||
            wn_view_set_image(view, slots[2], "posters/p2.png") != WN_OK) {
            fprintf(stderr, "image_cache_test: missing-image setup failed: %s\n",
                    wn_last_error());
            wn_view_destroy(view);
            return 1;
        }
        if (wn_view_render(view) != WN_OK) {
            fprintf(stderr, "image_cache_test: missing image aborted the frame: %s\n",
                    wn_last_error());
            wn_view_destroy(view);
            return 1;
        }
        if (wn_view_readback(view, missing_png) != WN_OK) {
            fprintf(stderr, "image_cache_test: missing-image readback failed: %s\n",
                    wn_last_error());
            wn_view_destroy(view);
            return 1;
        }
        if (wn_view_get_stats(view, &stats) != 0 || stats.image_failures != 1) {
            wn_view_destroy(view);
            return fail("missing image was not recorded once", stats.image_failures, 1);
        }
        count = wn_view_image_cache_count(view);
        printf("image_cache_test: missing-image frame ok count=%lld failures=%d\n",
               count, stats.image_failures);
        if (count < 2) {
            wn_view_destroy(view);
            return fail("valid posters were dropped with the broken one", count, 2);
        }
    }

    wn_view_destroy(view);
    printf("image_cache_test: PASS (count=%lld bytes=%lld budget=%lld)\n",
           last_count, last_bytes, budget);
    return 0;
}
