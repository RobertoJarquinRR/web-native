#ifndef WN_TEXT_H
#define WN_TEXT_H

#include <stddef.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct {
    const char *family;
    float size;
    int weight;
} wn_text_style;

typedef struct {
    float width;
    float height;
    float ascent;
    float descent;
} wn_text_metrics;

typedef struct {
    int glyph;
    float advance;
    float x;
} wn_text_glyph;

typedef struct {
    wn_text_glyph *glyphs;
    int glyph_count;
    int truncated;
    float width;
    float height;
    float ascent;
    float descent;
} wn_text_shaped;

typedef struct {
    int width;
    int height;
    int bearing_x;
    int bearing_y;
    const unsigned char *pixels;
} wn_text_bitmap;

typedef struct {
    const char *name;
    int (*init)(void **context);
    void (*destroy)(void *context);
    int (*measure)(void *context, const char *utf8, const wn_text_style *style,
                   float max_width, wn_text_metrics *out);
    int (*shape)(void *context, const char *utf8, const wn_text_style *style,
                 wn_text_glyph *glyphs, int capacity, wn_text_shaped *out);
    int (*rasterize)(void *context, const wn_text_style *style, int glyph,
                     unsigned char *pixels, int capacity, wn_text_bitmap *out);
    int (*cache_misses)(void *context);
} wn_text_ops;

#if defined(_WIN32)
const wn_text_ops *wn_text_directwrite(void);
#endif

#if defined(__cplusplus)
}
#endif

#endif
