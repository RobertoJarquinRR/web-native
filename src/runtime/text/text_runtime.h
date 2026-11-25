#ifndef WN_TEXT_RUNTIME_H
#define WN_TEXT_RUNTIME_H

#include "text/wn_text.h"

#if defined(__cplusplus)
extern "C" {
#endif

int wn_text_measure_string(const char *utf8, const char *family, float size, int weight,
                           float max_width, wn_text_metrics *out);
int wn_text_runtime_cache_misses(void);
void wn_text_runtime_shutdown(void);

#if defined(__cplusplus)
}
#endif

#endif
