#ifndef WN_TEXT_RUNTIME_H
#define WN_TEXT_RUNTIME_H

#include "text/wn_text.h"

#if defined(__cplusplus)
extern "C" {
#endif

int wn_text_measure_string(const char *utf8, const char *family, float size, int weight,
                           float max_width, wn_text_metrics *out);

int wn_text_shape_string(const char *utf8, const char *family, float size, int weight,
                         wn_text_glyph *glyphs, int capacity, wn_text_shaped *out);

int wn_text_rasterize_glyph(const char *family, float size, int weight, int glyph,
                            unsigned char *pixels, int capacity, wn_text_bitmap *out);

/* One greedy line: byte range into the source string plus the NUL-terminated
 * slice that is cached for shaping. `width` excludes trailing spaces. */
typedef struct {
    int start;
    int end;
    float width;
    const char *text;
} wn_text_line;

typedef struct {
    const wn_text_line *lines;
    int line_count;
    float width;          /* widest line */
    float height;         /* line_count * line_height */
    float measured_height;/* one-line height reported by the backend */
    float ascent;
    float descent;
    float line_height;    /* natural line box (ascent + descent) */
} wn_text_line_layout;

/* Resolve a CSS family list against the backend. Tries every family in order,
 * maps generics (serif/monospace/...) to concrete faces and falls back to the
 * first declared family only when none of them is installed. The result is the
 * face name handed to the backend. */
int wn_text_resolve_family(const char *family, char *out, int capacity);

/* Cached greedy line breaking (spaces are the only break opportunity; a word
 * wider than max_width overflows on its own line). max_width <= 0 keeps the
 * string on one line. The pointer stays valid until the next call. */
const wn_text_line_layout *wn_text_line_layout_get(const char *utf8, const char *family,
                                                  float size, int weight, float max_width);

int wn_text_runtime_cache_misses(void);
int wn_text_runtime_cache_stats(wn_text_cache_stats *out);
void wn_text_runtime_shutdown(void);

/* Last fallback/measurement/shape diagnostic (empty when there is nothing to
 * report). The string is owned by the runtime. */
const char *wn_text_last_error(void);

/* Test-only fault injection: make the next `count` line builds or shapes fail
 * so the cache failure paths are exercised deterministically. */
void wn_text_test_fail_next_lines(int count);
void wn_text_test_fail_next_shapes(int count);

#if defined(__cplusplus)
}
#endif

#endif
