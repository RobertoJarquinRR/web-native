#ifndef WN_TEXT_ATLAS_H
#define WN_TEXT_ATLAS_H

#include "text/text_runtime.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct {
    int width;
    int height;
    int bearing_x;
    int bearing_y;
    float advance;
    float u0, v0, u1, v1;
} wn_atlas_glyph;

typedef struct {
    wn_text_glyph *glyphs;
    int count;
    float width;
    float height;
    float ascent;
    float descent;
} wn_atlas_run;

typedef struct wn_text_atlas wn_text_atlas;

/* `seed` is the document atlas (input pool) that keeps its original UVs. */
wn_text_atlas *wn_text_atlas_create(const unsigned char *seed, int seed_width, int seed_height);
void wn_text_atlas_destroy(wn_text_atlas *atlas);
int wn_text_atlas_is_ready(const wn_text_atlas *atlas);
const unsigned char *wn_text_atlas_pixels(const wn_text_atlas *atlas);
int wn_text_atlas_width(const wn_text_atlas *atlas);
int wn_text_atlas_height(const wn_text_atlas *atlas);
unsigned int wn_text_atlas_serial(const wn_text_atlas *atlas);
const wn_atlas_glyph *wn_text_atlas_glyph(wn_text_atlas *atlas, const char *family, float size,
                                          int weight, int glyph);
const wn_atlas_run *wn_text_atlas_shape(wn_text_atlas *atlas, const char *text, const char *family,
                                        float size, int weight);

#if defined(__cplusplus)
}
#endif

#endif
