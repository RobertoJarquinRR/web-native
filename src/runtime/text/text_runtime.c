#include <ctype.h>
#include <string.h>

#include "text/text_runtime.h"

#if defined(_WIN32)
#define WN_TEXT_HAS_BACKEND 1
#else
#define WN_TEXT_HAS_BACKEND 0
#endif

#define WN_FAMILY_MAX 96

static const wn_text_ops *g_ops;
static void *g_context;
static int g_initialized;
static int g_available;

static void copy_trimmed(char *out, int capacity, const char *begin, const char *end)
{
    int length = 0;
    while (begin < end && isspace((unsigned char)*begin)) {
        ++begin;
    }
    while (end > begin && isspace((unsigned char)end[-1])) {
        --end;
    }
    if (end - begin >= 2 && (*begin == '"' || *begin == '\'') && end[-1] == *begin) {
        ++begin;
        --end;
    }
    while (begin < end && length < capacity - 1) {
        out[length++] = *begin++;
    }
    out[length] = '\0';
}

static int is_generic_family(const char *family)
{
    static const char *const generic[] = {
        "sans-serif", "system-ui", "-apple-system", "ui-sans-serif", "serif",
        "monospace", "cursive", "fantasy",
    };
    int i;
    for (i = 0; i < (int)(sizeof(generic) / sizeof(generic[0])); ++i) {
        if (strcmp(family, generic[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *resolve_family(const char *family)
{
    static char buffer[WN_FAMILY_MAX];
    const char *comma;
    if (family == NULL || family[0] == '\0') {
        return "Segoe UI";
    }
    comma = strchr(family, ',');
    copy_trimmed(buffer, WN_FAMILY_MAX, family, comma != NULL ? comma : family + strlen(family));
    if (buffer[0] == '\0' || is_generic_family(buffer)) {
        return "Segoe UI";
    }
    return buffer;
}

static int ensure_backend(void)
{
    if (g_initialized) {
        return g_available;
    }
    g_initialized = 1;
#if WN_TEXT_HAS_BACKEND
    g_ops = wn_text_directwrite();
    g_available = g_ops != NULL && g_ops->init(&g_context) == 0;
#endif
    return g_available;
}

int wn_text_measure_string(const char *utf8, const char *family, float size, int weight,
                           float max_width, wn_text_metrics *out)
{
    wn_text_style style;
    if (out == NULL) {
        return 1;
    }
    if (!ensure_backend()) {
        return 1;
    }
    style.family = resolve_family(family);
    style.size = size > 0.0f ? size : 16.0f;
    style.weight = weight > 0 ? weight : 400;
    return g_ops->measure(g_context, utf8 != NULL ? utf8 : "", &style, max_width, out);
}

int wn_text_runtime_cache_misses(void)
{
    if (!ensure_backend()) {
        return -1;
    }
    return g_ops->cache_misses(g_context);
}

void wn_text_runtime_shutdown(void)
{
    if (g_available && g_ops != NULL && g_ops->destroy != NULL) {
        g_ops->destroy(g_context);
    }
    g_context = NULL;
    g_ops = NULL;
    g_available = 0;
    g_initialized = 0;
}
