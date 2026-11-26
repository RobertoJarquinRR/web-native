/*
 * weight_test.cpp - CSS `font-weight` 100..900 on the real DirectWrite backend.
 *
 * The frontend now emits the real weight. This fixture locks the runtime side:
 *   1. Every requested weight maps to the documented DWRITE_FONT_WEIGHT.
 *   2. measure() and shape() both use the same nearest installed face, so the
 *      summed advances agree with the measured width.
 *   3. When the family actually has two distinct installed faces, their
 *      metrics differ. A weight the family does not ship falls back to the
 *      nearest face; that pair is reported and its exact-metric assertion is
 *      skipped instead of failing on the installed font set.
 *   4. The line layout key includes the requested weight (300 != 500).
 *
 * There is no pixel pin: the installed face set is machine-specific, so a
 * pinned bitmap would encode this machine's Segoe UI faces instead of the
 * weight contract. The measure/shape metrics are the portable assertion.
 *
 * On a machine without Segoe UI the fixture reports SKIP for the family-specific
 * assertions but still checks the mapping table. Off Windows it is a SKIP.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwrite.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "text/text_runtime.h"
#include "text/wn_text.h"

/* Test hook exported by text_directwrite.cpp (not part of the public ABI). */
extern "C" int wn_text_directwrite_weight(int weight);

#define FAMILY "Segoe UI"
#define SAMPLE "Hamburgefonts0123"
#define CACHE_SAMPLE "cache-weight-abc"
#define SAMPLE_SIZE 32.0f
#define GLYPH_CAPACITY 32

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

static int close_enough(float a, float b)
{
    return std::fabs(a - b) < 0.01f;
}

/* Distinct installed DWRITE weights of `family`; -1 when the family is absent. */
static int family_face_weights(const wchar_t *family, int *out, int capacity)
{
    IDWriteFactory *factory = nullptr;
    IDWriteFontCollection *collection = nullptr;
    IDWriteFontFamily *font_family = nullptr;
    UINT32 index = 0;
    UINT32 count = 0;
    BOOL exists = FALSE;
    int found = 0;

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown **>(&factory))) ||
        factory == nullptr) {
        return -1;
    }
    if (FAILED(factory->GetSystemFontCollection(&collection, FALSE)) || collection == nullptr) {
        factory->Release();
        return -1;
    }
    if (FAILED(collection->FindFamilyName(family, &index, &exists)) || !exists ||
        FAILED(collection->GetFontFamily(index, &font_family)) || font_family == nullptr) {
        collection->Release();
        factory->Release();
        return -1;
    }
    count = font_family->GetFontCount();
    for (UINT32 i = 0; i < count; ++i) {
        IDWriteFont *font = nullptr;
        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
        int value;
        int seen = 0;
        if (FAILED(font_family->GetFont(i, &font)) || font == nullptr) {
            continue;
        }
        weight = font->GetWeight();
        font->Release();
        value = static_cast<int>(weight);
        for (int slot = 0; slot < found; ++slot) {
            if (out[slot] == value) {
                seen = 1;
                break;
            }
        }
        if (!seen && found < capacity) {
            out[found++] = value;
        }
    }
    font_family->Release();
    collection->Release();
    factory->Release();
    return found;
}

static int has_face(int *weights, int count, int value)
{
    for (int i = 0; i < count; ++i) {
        if (weights[i] == value) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
#if !defined(_WIN32)
    printf("text_weight_test: SKIP (no text backend on this platform)\n");
    return 0;
#else
    static const int css_weights[] = {100, 300, 400, 500, 600, 700, 900};
    static const int canonical[] = {100, 200, 300, 350, 400, 500, 600, 700, 800, 900};
    const int weight_count = static_cast<int>(sizeof(css_weights) / sizeof(css_weights[0]));
    int face_weights[32];
    int face_count;
    wn_text_metrics metrics[7];
    int face_installed[7];
    int mapped[7];
    int measure_ok = 1;
    int shape_ok = 1;
    const wn_text_ops *ops;
    void *context = nullptr;

    /* ------------------------------------------------ mapping table */
    for (int i = 0; i < static_cast<int>(sizeof(canonical) / sizeof(canonical[0])); ++i) {
        char label[96];
        snprintf(label, sizeof(label), "weight %d maps to DWRITE %d",
                 canonical[i], canonical[i]);
        check(label, wn_text_directwrite_weight(canonical[i]) == canonical[i]);
    }
    check("weight 450 maps to NORMAL (400)", wn_text_directwrite_weight(450) == 400);
    check("weight 550 maps to MEDIUM (500)", wn_text_directwrite_weight(550) == 500);
    check("weight 0 clamps to THIN (100)", wn_text_directwrite_weight(0) == 100);
    check("weight 1000 clamps to BLACK (900)", wn_text_directwrite_weight(1000) == 900);

    face_count = family_face_weights(L"Segoe UI", face_weights,
                                     static_cast<int>(sizeof(face_weights) / sizeof(face_weights[0])));
    if (face_count < 0) {
        printf("  text_weight_test: SKIP family assertions (no installed %s)\n", FAMILY);
        face_count = 0;
    } else {
        printf("  %s installed DWRITE weights:", FAMILY);
        for (int i = 0; i < face_count; ++i) {
            printf(" %d", face_weights[i]);
        }
        printf("\n");
    }

    ops = wn_text_directwrite();
    if (ops == nullptr || ops->init == nullptr || ops->measure == nullptr || ops->shape == nullptr ||
        ops->init(&context) != 0) {
        printf("text_weight_test: FAIL (DirectWrite init failed)\n");
        return 1;
    }

    /* ------------------------------------- measure + shape every weight */
    for (int i = 0; i < weight_count; ++i) {
        wn_text_style style = {FAMILY, SAMPLE_SIZE, css_weights[i]};
        wn_text_glyph glyphs[GLYPH_CAPACITY];
        wn_text_shaped shaped;
        float advance_sum = 0.0f;
        char label[96];

        mapped[i] = wn_text_directwrite_weight(css_weights[i]);
        face_installed[i] = has_face(face_weights, face_count, mapped[i]);
        memset(&metrics[i], 0, sizeof(metrics[i]));
        snprintf(label, sizeof(label), "weight %d measures", css_weights[i]);
        if (ops->measure(context, SAMPLE, &style, 0.0f, &metrics[i]) != 0 ||
            !(metrics[i].width > 0.0f)) {
            measure_ok = 0;
            check(label, 0);
            continue;
        }
        check(label, 1);

        memset(&shaped, 0, sizeof(shaped));
        snprintf(label, sizeof(label), "weight %d shapes every glyph", css_weights[i]);
        if (ops->shape(context, SAMPLE, &style, glyphs, GLYPH_CAPACITY, &shaped) != 0 ||
            shaped.glyph_count != static_cast<int>(std::strlen(SAMPLE))) {
            shape_ok = 0;
            check(label, 0);
            continue;
        }
        check(label, 1);
        for (int glyph = 0; glyph < shaped.glyph_count; ++glyph) {
            advance_sum += glyphs[glyph].advance;
        }
        snprintf(label, sizeof(label), "weight %d advances match the measured width",
                 css_weights[i]);
        check(label, advance_sum > metrics[i].width - 1.5f &&
                         advance_sum < metrics[i].width + 1.5f);
        printf("  weight %3d -> DWRITE %3d (%s) width=%.2f height=%.2f\n",
               css_weights[i], mapped[i], face_installed[i] ? "face" : "nearest fallback",
               metrics[i].width, metrics[i].height);
    }
    check("every weight measured", measure_ok);
    check("every weight shaped", shape_ok);

    /* ------------------- distinct faces produce distinct metrics */
    if (face_count > 0) {
        int compared = 0;
        int distinct = 0;
        check("400 and 700 are distinct installed faces",
              has_face(face_weights, face_count, 400) && has_face(face_weights, face_count, 700));
        for (int i = 0; i < weight_count; ++i) {
            if (!face_installed[i]) {
                continue;
            }
            for (int j = i + 1; j < weight_count; ++j) {
                if (!face_installed[j] || mapped[i] == mapped[j] ||
                    !(metrics[i].width > 0.0f) || !(metrics[j].width > 0.0f)) {
                    continue;
                }
                compared += 1;
                if (!close_enough(metrics[i].width, metrics[j].width)) {
                    distinct += 1;
                } else {
                    printf("  note: weights %d and %d share width %.2f\n",
                           css_weights[i], css_weights[j], metrics[i].width);
                }
            }
        }
        printf("  distinct installed pairs: %d/%d differ in width\n", distinct, compared);
        check("distinct installed faces have distinct metrics", compared > 0 && distinct == compared);
    }

    /* ------------------------------------------ weight is a cache key */
    {
        wn_text_metrics repeated;
        wn_text_cache_stats before;
        wn_text_cache_stats after;
        wn_text_style light = {FAMILY, SAMPLE_SIZE, 300};
        wn_text_style medium = {FAMILY, SAMPLE_SIZE, 500};
        memset(&before, 0, sizeof(before));
        memset(&after, 0, sizeof(after));
        ops->cache_stats(context, &before);
        check("300 and 500 measure separately",
              ops->measure(context, CACHE_SAMPLE, &light, 0.0f, &repeated) == 0 &&
                  ops->measure(context, CACHE_SAMPLE, &medium, 0.0f, &repeated) == 0);
        ops->cache_stats(context, &after);
        check("300 and 500 are two misses", after.misses - before.misses == 2);
        check("re-measuring 300 is a hit",
              ops->measure(context, CACHE_SAMPLE, &light, 0.0f, &repeated) == 0);
        ops->cache_stats(context, &after);
        check("the 300 hit does not miss", after.misses - before.misses == 2);
    }

    /* ------------------------- line layout key includes the weight */
    {
        const wn_text_line_layout *light;
        const wn_text_line_layout *medium;
        const wn_text_line_layout *again;
        wn_text_runtime_shutdown();
        light = wn_text_line_layout_get(SAMPLE, FAMILY, SAMPLE_SIZE, 300, 0.0f);
        medium = wn_text_line_layout_get(SAMPLE, FAMILY, SAMPLE_SIZE, 500, 0.0f);
        again = wn_text_line_layout_get(SAMPLE, FAMILY, SAMPLE_SIZE, 300, 0.0f);
        check("line layout is keyed by weight",
              light != nullptr && medium != nullptr && light != medium);
        check("repeating a weight returns its entry", again == light);
    }

    ops->destroy(context);
    wn_text_runtime_shutdown();
    if (failures != 0) {
        printf("text_weight_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }
    printf("text_weight_test: PASS (%d checks)\n", checks);
    return 0;
#endif
}
