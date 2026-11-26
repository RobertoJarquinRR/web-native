/*
 * measure_test.c - real-backend smoke for DirectWrite measurement.
 *
 * This file talks to the installed backend on purpose: it proves the actual
 * font stack measures, shapes and rasterizes. The backend-independent text
 * contracts (hint-only max_width, greedy breaking, set_text, cache isolation)
 * live in contract_test.c against fake_text.c. Off Windows it reports SKIP when
 * no backend exists; on Windows the real backend is required and any missing
 * piece or failed init is a failure, never a skip.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text/text_runtime.h"
#include "text/wn_text.h"

#define GLYPH_CAPACITY 16
#define BITMAP_CAPACITY (128 * 128)
#define LONG_CAPACITY 5000
#define CACHE_STRINGS 300

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
#if !defined(_WIN32)
    printf("text_measure_test: SKIP (no text backend on this platform)\n");
    return 0;
#else
    const wn_text_ops *ops = wn_text_directwrite();
    wn_text_style regular = {"Segoe UI", 16.0f, 400};
    wn_text_style bold = {"Segoe UI", 16.0f, 700};
    wn_text_style missing = {"NoSuchFontFamilyXYZ", 16.0f, 400};
    wn_text_style huge = {"Segoe UI", 100000.0f, 400};
    wn_text_metrics first;
    wn_text_metrics second;
    wn_text_metrics other;
    wn_text_metrics empty;
    wn_text_metrics huge_metrics;
    wn_text_metrics mono;
    wn_text_metrics sans;
    wn_text_glyph glyphs[GLYPH_CAPACITY];
    wn_text_shaped shaped;
    wn_text_bitmap bitmap;
    unsigned char pixels[BITMAP_CAPACITY];
    unsigned char *big_pixels;
    wn_text_glyph *long_glyphs;
    char *long_text;
    char face[96];
    char label[128];
    long coverage = 0;
    float advance_sum = 0.0f;
    void *context = NULL;
    wn_text_cache_stats before;
    wn_text_cache_stats after;
    int measure_status;

    if (ops == NULL || ops->init == NULL || ops->measure == NULL || ops->shape == NULL ||
        ops->rasterize == NULL) {
        printf("text_measure_test: FAIL (incomplete DirectWrite backend on _WIN32)\n");
        return 1;
    }
    if (ops->init(&context) != 0) {
        printf("text_measure_test: FAIL (DirectWrite backend init failed)\n");
        return 1;
    }

    check("measure returns success", ops->measure(context, "Hola", &regular, 0.0f, &first) == 0);
    printf("  measured \"Hola\" 16px Segoe UI: width=%.2f height=%.2f ascent=%.2f descent=%.2f\n",
           first.width, first.height, first.ascent, first.descent);
    check("width is positive", first.width > 0.0f);
    check("height covers the font size", first.height >= 15.0f);
    check("ascent plus descent is the height", first.ascent + first.descent <= first.height + 0.01f);

    check("second measure returns success", ops->measure(context, "Hola", &regular, 0.0f, &second) == 0);
    check("second measure is identical", memcmp(&first, &second, sizeof(first)) == 0);
    check("cache recorded one miss", ops->cache_misses(context) == 1);

    check("bold measures", ops->measure(context, "Hola", &bold, 0.0f, &other) == 0);
    check("bold is a separate cache entry", ops->cache_misses(context) == 2);
    check("bold width differs from regular", other.width != first.width);

    check("empty string measures", ops->measure(context, "", &regular, 0.0f, &empty) == 0);
    check("empty string has no width", empty.width == 0.0f);

    memset(&shaped, 0, sizeof(shaped));
    check("shapes the string", ops->shape(context, "Hola", &regular, glyphs, GLYPH_CAPACITY, &shaped) == 0);
    check("one glyph per character", shaped.glyph_count == 4);
    for (int i = 0; i < shaped.glyph_count; ++i) {
        advance_sum += glyphs[i].advance;
    }
    check("every glyph has an id", glyphs[0].glyph > 0 && glyphs[3].glyph > 0);
    check("advances agree with the measured width", advance_sum > first.width - 1.5f &&
                                                     advance_sum < first.width + 1.5f);

    memset(&bitmap, 0, sizeof(bitmap));
    check("rasterizes the first glyph", ops->rasterize(context, &regular, glyphs[0].glyph,
                                                      pixels, BITMAP_CAPACITY, &bitmap) == 0);
    for (int i = 0; i < bitmap.width * bitmap.height; ++i) {
        coverage += pixels[i];
    }
    printf("  glyph 'H' bitmap: %dx%d bearing=(%d,%d) coverage=%ld\n",
           bitmap.width, bitmap.height, bitmap.bearing_x, bitmap.bearing_y, coverage);
    check("bitmap has size", bitmap.width > 0 && bitmap.height > 0);
    check("bitmap has ink", coverage > 0);
    check("bitmap fits the line box", bitmap.height <= (int)first.height + 1);

    check("rasterize rejects a small buffer", ops->rasterize(context, &regular, glyphs[0].glyph,
                                                            pixels, 4, &bitmap) == 1);

    /* ------------------------------------------------------- font stack */
    check("family stack skips missing faces",
          wn_text_resolve_family("NoSuchFont, Segoe UI", face, (int)sizeof(face)) == 0 &&
              strcmp(face, "Segoe UI") == 0);
    printf("  resolved \"NoSuchFont, Segoe UI\" -> %s\n", face);
    check("serif maps to Times New Roman",
          wn_text_resolve_family("serif", face, (int)sizeof(face)) == 0 &&
              strcmp(face, "Times New Roman") == 0);
    check("monospace maps to Consolas",
          wn_text_resolve_family("monospace", face, (int)sizeof(face)) == 0 &&
              strcmp(face, "Consolas") == 0);
    check("monospace measures", wn_text_measure_string("MMMM", "monospace", 16.0f, 400, 0.0f,
                                                       &mono) == 0);
    check("sans-serif measures", wn_text_measure_string("MMMM", "sans-serif", 16.0f, 400, 0.0f,
                                                        &sans) == 0);
    check("monospace advance differs from sans-serif", mono.width != sans.width);

    /* A stack with nothing installed must land on a face that exists. */
    check("missing-only stack falls back",
          wn_text_resolve_family("NoSuchFontFamilyXYZ", face, (int)sizeof(face)) == 0 &&
              strcmp(face, "Segoe UI") == 0);
    check("fallback diagnostic is observable",
          wn_text_last_error() != NULL && wn_text_last_error()[0] != '\0');
    {
        wn_text_metrics fallback_metrics;
        wn_text_shaped fallback_shaped;
        wn_text_glyph fallback_glyphs[GLYPH_CAPACITY];
        memset(&fallback_metrics, 0, sizeof(fallback_metrics));
        memset(&fallback_shaped, 0, sizeof(fallback_shaped));
        check("fallback measures",
              wn_text_measure_string("Hola", "NoSuchFontFamilyXYZ", 16.0f, 400, 0.0f,
                                     &fallback_metrics) == 0 &&
                  fallback_metrics.width > 0.0f);
        check("fallback shapes the same string",
              wn_text_shape_string("Hola", "NoSuchFontFamilyXYZ", 16.0f, 400, fallback_glyphs,
                                   GLYPH_CAPACITY, &fallback_shaped) == 0 &&
                  fallback_shaped.glyph_count == 4);
        check("valid stack still works after the fallback",
              wn_text_measure_string("Hola", "Segoe UI", 16.0f, 400, 0.0f,
                                     &fallback_metrics) == 0 &&
                  fallback_metrics.width > 0.0f);
    }

    /* Wrapped measurement must return multi-line metrics, not fail. */
    {
        wn_text_metrics m30 = {0.0f, 0.0f, 0.0f, 0.0f};
        wn_text_metrics m60 = {0.0f, 0.0f, 0.0f, 0.0f};
        wn_text_metrics m98 = {0.0f, 0.0f, 0.0f, 0.0f};
        const wn_text_line_layout *w30;
        const wn_text_line_layout *w60;
        const wn_text_line_layout *w98;
        int ok = 1;
        ok = ok && ops->measure(context, "one two three", &regular, 30.0f, &m30) == 0;
        ok = ok && ops->measure(context, "one two three", &regular, 60.0f, &m60) == 0;
        ok = ok && ops->measure(context, "one two three", &regular, 98.0f, &m98) == 0;
        check("wrapped measure returns metrics", ok);
        w30 = wn_text_line_layout_get("one two three", "Segoe UI", 16.0f, 400, 30.0f);
        w60 = wn_text_line_layout_get("one two three", "Segoe UI", 16.0f, 400, 60.0f);
        w98 = wn_text_line_layout_get("one two three", "Segoe UI", 16.0f, 400, 98.0f);
        printf("  wrap widths: 30=%.2f(%d lines) 60=%.2f(%d lines) 98=%.2f(%d lines)\n",
               w30 ? w30->width : -1.0f, w30 ? w30->line_count : -1,
               w60 ? w60->width : -1.0f, w60 ? w60->line_count : -1,
               w98 ? w98->width : -1.0f, w98 ? w98->line_count : -1);
        printf("  measured heights: 30=%.2f 60=%.2f 98=%.2f\n",
               m30.height, m60.height, m98.height);
        printf("  breaker heights: 30=%.2f(%d) 60=%.2f(%d) 98=%.2f(%d)\n",
               w30 ? w30->height : -1.0f, w30 ? w30->line_count : -1,
               w60 ? w60->height : -1.0f, w60 ? w60->line_count : -1,
               w98 ? w98->height : -1.0f, w98 ? w98->line_count : -1);
        check("narrow wrap has at least as many lines as wide wrap",
              w30 != NULL && w60 != NULL && w98 != NULL && w30->line_count >= 2 &&
                  w60->line_count >= 2 && w30->line_count >= w60->line_count &&
                  w60->line_count >= w98->line_count &&
                  w30->line_count > w98->line_count);
        check("wrapped height reflects the line count",
              m30.height > m60.height && m60.height >= m98.height &&
                  m60.height > m30.ascent + m30.descent);
        check("wrapped measure reports more than one line box",
              m30.height > (m30.ascent + m30.descent) * 3.0f - 1.0f);
        check("wrapped ascent and descent stay sane",
              m30.ascent > 0.0f && m30.ascent + m30.descent <= m30.height + 0.01f);
        /* At 30 the backend emergency-breaks "three"; at 60/98 no word needs
         * an emergency break, so the backend hint and the engine agree. */
        check("measure agrees with the breaker at 60/98",
              w60 != NULL && w98 != NULL &&
                  fabsf(m60.height - w60->height) < 1.0f &&
                  fabsf(m98.height - w98->height) < 1.0f);
    }

    /* ------------------------------------------------ measurement cache */
    memset(&before, 0, sizeof(before));
    check("cache stats available", ops->cache_stats != NULL);
    if (ops->cache_stats != NULL) {
        int cache_ok = 1;
        ops->cache_stats(context, &before);
        for (int i = 0; i < CACHE_STRINGS; ++i) {
            snprintf(label, sizeof(label), "cache-string-%03d", i);
            if (ops->measure(context, label, &regular, 0.0f, &other) != 0) {
                cache_ok = 0;
                break;
            }
        }
        check("300 distinct strings measure", cache_ok);
        ops->cache_stats(context, &after);
        printf("  cache after %d strings: hits=%d misses=%d evictions=%d\n",
               CACHE_STRINGS, after.hits, after.misses, after.evictions);
        check("300 distinct strings fit without evicting",
              after.evictions == before.evictions && after.misses - before.misses == CACHE_STRINGS);
        check("every new string was a miss", after.hits == before.hits);
        check("reuse is a hit", ops->measure(context, "cache-string-007", &regular, 0.0f,
                                             &other) == 0);
        ops->cache_stats(context, &after);
        check("reuse increments hits", after.hits == before.hits + 1);
        check("reuse does not miss", after.misses == before.misses + CACHE_STRINGS);
    }

    /* -------------------------------------------- D2 face-cache integrity */
    {
        wn_text_glyph scratch_glyphs[GLYPH_CAPACITY];
        wn_text_shaped scratch_shaped;
        int missing_status;
        int valid_status;
        memset(&scratch_shaped, 0, sizeof(scratch_shaped));
        missing_status = ops->shape(context, "Hola", &missing, scratch_glyphs, GLYPH_CAPACITY,
                                    &scratch_shaped);
        check("nonexistent family fails cleanly", missing_status == 1);
        valid_status = ops->shape(context, "Hola", &regular, scratch_glyphs, GLYPH_CAPACITY,
                                  &scratch_shaped);
        check("valid family still shapes after a failed lookup", valid_status == 0 &&
                                                                  scratch_shaped.glyph_count == 4);
        memset(&scratch_shaped, 0, sizeof(scratch_shaped));
        valid_status = ops->shape(context, "Hola", &regular, scratch_glyphs, GLYPH_CAPACITY,
                                  &scratch_shaped);
        check("face cache is reusable afterwards", valid_status == 0 &&
                                                   scratch_shaped.glyph_count == 4);
    }

    /* ------------------------------------------- D1 untrusted font sizes */
    measure_status = ops->measure(context, "W", &huge, 0.0f, &huge_metrics);
    check("absurd font size is clamped, not rejected", measure_status == 0 &&
                                                       huge_metrics.width > 0.0f);
    memset(&shaped, 0, sizeof(shaped));
    check("absurd size still shapes", ops->shape(context, "W", &huge, glyphs, GLYPH_CAPACITY,
                                                 &shaped) == 0 && shaped.glyph_count == 1);
    big_pixels = (unsigned char *)malloc(64 * 1024);
    check("scratch buffer allocated", big_pixels != NULL);
    if (big_pixels != NULL) {
        int untouched = 1;
        int clean = 1;
        int rejected = 0;
        float probe_sizes[] = {200.0f, 300.0f, 400.0f, 512.0f, 100000.0f};
        for (int i = 0; i < 5; ++i) {
            wn_text_style probe = {"Segoe UI", probe_sizes[i], 400};
            wn_text_glyph probe_glyphs[1];
            wn_text_shaped probe_shaped;
            int status;
            memset(&probe_shaped, 0, sizeof(probe_shaped));
            memset(&bitmap, 0, sizeof(bitmap));
            memset(big_pixels, 0xAA, 64 * 1024);
            if (ops->shape(context, "W", &probe, probe_glyphs, 1, &probe_shaped) != 0) {
                clean = 0;
                break;
            }
            status = ops->rasterize(context, &probe, probe_glyphs[0].glyph, big_pixels,
                                    64 * 1024, &bitmap);
            printf("  probe size %.0f: status=%d size=%dx%d\n",
                   probe_sizes[i], status, bitmap.width, bitmap.height);
            if (status == 1) {
                rejected = 1;
            }
            if (status == 0 && bitmap.width > 0 && bitmap.height > 0) {
                /* A glyph that fits may write; the buffer is reset each pass. */
                continue;
            }
            for (int b = 0; b < 64 * 1024; ++b) {
                if (big_pixels[b] != 0xAA) {
                    untouched = 0;
                    break;
                }
            }
        }
        check("absurd font size fails cleanly", clean);
        check("glyph larger than the scratch buffer is rejected", rejected);
        check("rejected glyphs never write to the buffer", untouched);
        free(big_pixels);
    }

    /* ------------------------------------------------ D8 long strings */
    long_text = (char *)malloc(LONG_CAPACITY + 1);
    long_glyphs = (wn_text_glyph *)malloc(4096 * sizeof(wn_text_glyph));
    check("long fixtures allocated", long_text != NULL && long_glyphs != NULL);
    if (long_text != NULL && long_glyphs != NULL) {
        memset(long_text, 'A', LONG_CAPACITY);
        long_text[LONG_CAPACITY] = '\0';
        memset(&other, 0, sizeof(other));
        check("long string measures its prefix instead of zero",
              ops->measure(context, long_text, &regular, 0.0f, &other) == 0 &&
                  other.width > 0.0f);
        memset(&shaped, 0, sizeof(shaped));
        check("long string shapes its truncated prefix",
              ops->shape(context, long_text, &regular, long_glyphs, 4096, &shaped) == 0 &&
                  shaped.glyph_count > 0);
        check("shape reports truncation", shaped.truncated == 1);
    }
    free(long_text);
    free(long_glyphs);

    /* F1: a failed line insert must not consume cache capacity. More than the
     * 512-entry capacity fails here; the next valid lookup must still work. */
    wn_text_runtime_shutdown();
    wn_text_test_fail_next_lines(600);
    {
        int all_failed = 1;
        char cache_name[32];
        const wn_text_line_layout *recovered;
        for (int i = 0; i < 600; ++i) {
            snprintf(cache_name, sizeof(cache_name), "fail-line-%03d", i);
            if (wn_text_line_layout_get(cache_name, "Segoe UI", 16.0f, 400, 0.0f) != NULL) {
                all_failed = 0;
            }
        }
        check("600 injected line failures fail", all_failed);
        recovered = wn_text_line_layout_get("line-cache-recovery", "Segoe UI", 16.0f, 400, 0.0f);
        check("line cache recovers after a failure storm",
              recovered != NULL && recovered->line_count == 1);
        check("recovered line has a line box", recovered != NULL && recovered->line_height > 0.0f);
    }

    /* Warm cache: failed inserts must not evict published entries. */
    wn_text_runtime_shutdown();
    {
        const wn_text_line_layout *saved[512];
        char warm_names[512][24];
        int filled = 0;
        int failed = 1;
        int survivors = 0;
        char storm_name[32];
        for (int i = 0; i < 512; ++i) {
            snprintf(warm_names[i], sizeof(warm_names[i]), "warm-line-%03d", i);
            saved[i] = wn_text_line_layout_get(warm_names[i], "Segoe UI", 16.0f, 400, 0.0f);
            if (saved[i] != NULL) {
                ++filled;
            }
        }
        check("line cache warms to capacity", filled == 512);
        wn_text_test_fail_next_lines(600);
        for (int i = 0; i < 600; ++i) {
            snprintf(storm_name, sizeof(storm_name), "warm-fail-%03d", i);
            if (wn_text_line_layout_get(storm_name, "Segoe UI", 16.0f, 400, 0.0f) != NULL) {
                failed = 0;
            }
        }
        check("600 warm line failures fail", failed);
        for (int i = 0; i < 512; ++i) {
            if (wn_text_line_layout_get(warm_names[i], "Segoe UI", 16.0f, 400, 0.0f) ==
                saved[i]) {
                ++survivors;
            }
        }
        printf("  warm line cache survivors=%d/512 after 600 failures\n", survivors);
        check("all warm line entries survive the failure storm", survivors == 512);
        {
            const wn_text_line_layout *added[8];
            char added_names[8][24];
            int added_ok = 1;
            int kept = 0;
            for (int i = 0; i < 8; ++i) {
                snprintf(added_names[i], sizeof(added_names[i]), "warm-after-%d", i);
                added[i] = wn_text_line_layout_get(added_names[i], "Segoe UI", 16.0f, 400,
                                                   0.0f);
                if (added[i] == NULL) {
                    added_ok = 0;
                }
            }
            check("8 valid inserts after the storm succeed", added_ok);
            for (int i = 0; i < 8; ++i) {
                if (wn_text_line_layout_get(added_names[i], "Segoe UI", 16.0f, 400, 0.0f) ==
                    added[i]) {
                    ++kept;
                }
            }
            check("8 inserts after the storm stay cached", kept == 8);
        }
    }

    ops->destroy(context);
    wn_text_runtime_shutdown();
    if (failures != 0) {
        printf("text_measure_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }
    printf("text_measure_test: PASS (%d checks)\n", checks);
    return 0;
#endif
}
