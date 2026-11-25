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
    const char *name;
    int (*init)(void **context);
    void (*destroy)(void *context);
    int (*measure)(void *context, const char *utf8, const wn_text_style *style,
                   float max_width, wn_text_metrics *out);
    int (*cache_misses)(void *context);
} wn_text_ops;

#if defined(_WIN32)
const wn_text_ops *wn_text_directwrite(void);
#endif

#if defined(__cplusplus)
}
#endif

#endif
