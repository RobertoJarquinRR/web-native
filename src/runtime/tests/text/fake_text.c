/*
 * fake_text.c - deterministic `wn_text_ops` backend for contract tests.
 *
 * One glyph per codepoint at a fixed advance (size * 0.5), so every expected
 * line width can be computed by hand. It also lies on purpose when the caller
 * passes a `max_width`: the contract tests use that to prove the engine breaks
 * lines with its own greedy pass and treats the backend measurement as a hint.
 *
 * No platform headers: the same file builds and runs on any host.
 */
#include <string.h>

#include "text/wn_text.h"

typedef struct {
    int measure_calls;
    int shape_calls;
    int raster_calls;
    /* No-hint `height` reported by measure, as a multiple of the font size.
     * 0 keeps the 1.25 line box. The measured-height variant raises it so the
     * block can be exercised where the backend box and the line box differ. */
    float measured_scale;
    /* The broken-measure variant fails every measure while shape/rasterize
     * keep working, so the synthetic line-box / fallback paths are reachable. */
    int fail_measure;
} WnFakeState;

static WnFakeState g_fake;

/* Advance per codepoint and line box for a given font size. */
static float fake_advance(float size)
{
    return size * 0.5f;
}

static float fake_line_height(float size)
{
    return size * 1.25f;
}

/* Returns the bytes consumed and writes the codepoint. A malformed sequence
 * counts as one U+FFFD-sized codepoint so the fake never reads past the NUL. */
static int decode_codepoint(const unsigned char *p, unsigned int *out)
{
    if (p[0] < 0x80u) {
        *out = p[0];
        return 1;
    }
    if ((p[0] & 0xE0u) == 0xC0u && p[1] != 0u) {
        *out = ((unsigned int)(p[0] & 0x1Fu) << 6) | (unsigned int)(p[1] & 0x3Fu);
        return 2;
    }
    if ((p[0] & 0xF0u) == 0xE0u && p[1] != 0u && p[2] != 0u) {
        *out = ((unsigned int)(p[0] & 0x0Fu) << 12) |
               ((unsigned int)(p[1] & 0x3Fu) << 6) | (unsigned int)(p[2] & 0x3Fu);
        return 3;
    }
    if ((p[0] & 0xF8u) == 0xF0u && p[1] != 0u && p[2] != 0u && p[3] != 0u) {
        *out = ((unsigned int)(p[0] & 0x07u) << 18) |
               ((unsigned int)(p[1] & 0x3Fu) << 12) |
               ((unsigned int)(p[2] & 0x3Fu) << 6) | (unsigned int)(p[3] & 0x3Fu);
        return 4;
    }
    *out = 0xFFFDu;
    return 1;
}

static int fake_codepoints(const char *utf8)
{
    const unsigned char *p = (const unsigned char *)(utf8 != NULL ? utf8 : "");
    int count = 0;
    while (*p != '\0') {
        unsigned int unused = 0;
        p += decode_codepoint(p, &unused);
        ++count;
    }
    return count;
}

static int fake_init(void **context)
{
    if (context == NULL) {
        return 1;
    }
    memset(&g_fake, 0, sizeof(g_fake));
    *context = &g_fake;
    return 0;
}

static void fake_destroy(void *context)
{
    (void)context;
}

static int fake_measure(void *context, const char *utf8, const wn_text_style *style,
                        float max_width, wn_text_metrics *out)
{
    WnFakeState *state = (WnFakeState *)context;
    float size = style != NULL && style->size > 0.0f ? style->size : 16.0f;
    int codepoints;

    if (out == NULL) {
        return 1;
    }
    if (state != NULL) {
        state->measure_calls += 1;
        if (state->fail_measure) {
            return 1;
        }
    }
    codepoints = fake_codepoints(utf8);
    memset(out, 0, sizeof(*out));
    out->ascent = size;
    out->descent = size * 0.25f;
    if (max_width > 0.0f) {
        /* Deliberate lie: pretend the backend emergency-broke the string into
         * three line boxes and clamped the width. Layout must ignore both. */
        out->width = max_width;
        out->height = fake_line_height(size) * 3.0f;
    } else {
        out->width = fake_advance(size) * (float)codepoints;
        out->height = state != NULL && state->measured_scale > 0.0f
                          ? size * state->measured_scale
                          : fake_line_height(size);
    }
    return 0;
}

static int fake_shape(void *context, const char *utf8, const wn_text_style *style,
                      wn_text_glyph *glyphs, int capacity, wn_text_shaped *out)
{
    WnFakeState *state = (WnFakeState *)context;
    const unsigned char *p = (const unsigned char *)(utf8 != NULL ? utf8 : "");
    float size = style != NULL && style->size > 0.0f ? style->size : 16.0f;
    float advance = fake_advance(size);
    float pen = 0.0f;
    int written = 0;
    int total = 0;

    if (out == NULL) {
        return 1;
    }
    if (state != NULL) {
        state->shape_calls += 1;
    }
    while (*p != '\0') {
        unsigned int codepoint = 0;
        p += decode_codepoint(p, &codepoint);
        if (glyphs != NULL && written < capacity) {
            glyphs[written].glyph = (int)(codepoint & 0xFFFFu);
            glyphs[written].advance = advance;
            glyphs[written].x = pen;
            ++written;
        }
        pen += advance;
        ++total;
    }
    memset(out, 0, sizeof(*out));
    out->glyphs = glyphs;
    out->glyph_count = written;
    out->truncated = written < total ? 1 : 0;
    out->width = pen;
    out->height = fake_line_height(size);
    out->ascent = size;
    out->descent = size * 0.25f;
    return 0;
}

static int fake_rasterize(void *context, const wn_text_style *style, int glyph,
                          unsigned char *pixels, int capacity, wn_text_bitmap *out)
{
    WnFakeState *state = (WnFakeState *)context;
    float size = style != NULL && style->size > 0.0f ? style->size : 16.0f;
    static const unsigned char coverage[4] = {0, 96, 192, 255};

    if (out == NULL) {
        return 1;
    }
    if (state != NULL) {
        state->raster_calls += 1;
    }
    memset(out, 0, sizeof(*out));
    /* A deterministic 2x2 mask with ink; one bitmap for every glyph id. */
    out->width = 2;
    out->height = 2;
    out->bearing_x = 0;
    out->bearing_y = -(int)size;
    out->pixels = pixels;
    if (pixels == NULL || capacity < 4) {
        out->width = 0;
        out->height = 0;
        out->pixels = NULL;
        return 1;
    }
    memcpy(pixels, coverage, sizeof(coverage));
    (void)glyph;
    return 0;
}

static int fake_cache_misses(void *context)
{
    WnFakeState *state = (WnFakeState *)context;
    return state != NULL ? state->measure_calls + state->shape_calls : 0;
}

static int fake_has_family(void *context, const char *family)
{
    (void)context;
    return family != NULL && strcmp(family, "fake") == 0;
}

static void fake_cache_stats(void *context, wn_text_cache_stats *out)
{
    WnFakeState *state = (WnFakeState *)context;
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    /* The fake does not cache: every backend call is a miss and nothing is
     * ever evicted. The runtime's line cache is the layer under test. */
    if (state != NULL) {
        out->misses = state->measure_calls + state->shape_calls;
    }
}

static const wn_text_ops g_fake_ops = {
    "fake",
    fake_init,
    fake_destroy,
    fake_measure,
    fake_shape,
    fake_rasterize,
    fake_cache_misses,
    fake_has_family,
    fake_cache_stats,
};

/* Failing-init backend: assigns a context and then reports failure, so the
 * runtime hook has to destroy it instead of clearing the pointer. */
static int g_fake_failed_destroys;

static int fake_failing_init(void **context)
{
    if (context == NULL) {
        return 1;
    }
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake_failed_destroys = 0;
    *context = &g_fake;
    return 1;
}

static void fake_failing_destroy(void *context)
{
    if (context != NULL) {
        g_fake_failed_destroys += 1;
    }
}

static const wn_text_ops g_fake_failing_ops = {
    "fake-failing",
    fake_failing_init,
    fake_failing_destroy,
    fake_measure,
    fake_shape,
    fake_rasterize,
    fake_cache_misses,
    fake_has_family,
    fake_cache_stats,
};

/* Same deterministic backend, but the no-hint measure reports a taller box than
 * the ascent+descent line: layout must keep that measured height only for
 * unconstrained single-line TEXT, while paint follows the line box. */
static WnFakeState g_fake_measured;

static int fake_measured_init(void **context)
{
    if (context == NULL) {
        return 1;
    }
    memset(&g_fake_measured, 0, sizeof(g_fake_measured));
    g_fake_measured.measured_scale = 1.4f;
    *context = &g_fake_measured;
    return 0;
}

static const wn_text_ops g_fake_measured_ops = {
    "fake-measured",
    fake_measured_init,
    fake_destroy,
    fake_measure,
    fake_shape,
    fake_rasterize,
    fake_cache_misses,
    fake_has_family,
    fake_cache_stats,
};

const wn_text_ops *wn_text_fake(void)
{
    return &g_fake_ops;
}

const wn_text_ops *wn_text_fake_measured(void)
{
    return &g_fake_measured_ops;
}

/* Deterministic backend whose measure always fails: shape and rasterize still
 * succeed, so build_lines falls back to its synthetic 1.2 line box and callers
 * (input) can exercise their pre-block 1.35 fallback. */
static WnFakeState g_fake_broken_measure;

static int fake_broken_measure_init(void **context)
{
    if (context == NULL) {
        return 1;
    }
    memset(&g_fake_broken_measure, 0, sizeof(g_fake_broken_measure));
    g_fake_broken_measure.fail_measure = 1;
    *context = &g_fake_broken_measure;
    return 0;
}

static const wn_text_ops g_fake_broken_measure_ops = {
    "fake-broken-measure",
    fake_broken_measure_init,
    fake_destroy,
    fake_measure,
    fake_shape,
    fake_rasterize,
    fake_cache_misses,
    fake_has_family,
    fake_cache_stats,
};

const wn_text_ops *wn_text_fake_broken_measure(void)
{
    return &g_fake_broken_measure_ops;
}

const wn_text_ops *wn_text_fake_failing_init(void)
{
    return &g_fake_failing_ops;
}

int wn_text_fake_failed_destroys(void)
{
    return g_fake_failed_destroys;
}
