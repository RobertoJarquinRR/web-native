#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text/text_runtime.h"

#if defined(_WIN32)
#define WN_TEXT_HAS_BACKEND 1
#else
#define WN_TEXT_HAS_BACKEND 0
#endif

#define WN_FAMILY_MAX 96
#define WN_FAMILY_CACHE 32
#define WN_LINE_CACHE_CAPACITY 512
#define WN_LINE_BUCKETS 1024
#define WN_LINE_BUCKET_MASK (WN_LINE_BUCKETS - 1)

static const wn_text_ops *g_ops;
static void *g_context;
static int g_initialized;
static int g_available;
static char g_text_error[192];
static int g_test_fail_lines;
static int g_test_fail_shapes;

static int ensure_backend(void);

static void set_text_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_text_error, sizeof(g_text_error), fmt, args);
    va_end(args);
    g_text_error[sizeof(g_text_error) - 1] = '\0';
}

const char *wn_text_last_error(void)
{
    return g_text_error;
}

void wn_text_test_fail_next_lines(int count)
{
    g_test_fail_lines = count > 0 ? count : 0;
}

void wn_text_test_fail_next_shapes(int count)
{
    g_test_fail_shapes = count > 0 ? count : 0;
}

/* ------------------------------------------------------- family resolution */

typedef struct {
    int used;
    int fallback;
    char input[WN_FAMILY_MAX];
    char face[WN_FAMILY_MAX];
} WnFamilyEntry;

static WnFamilyEntry g_family_cache[WN_FAMILY_CACHE];
static int g_family_next;

static void copy_text(char *out, int capacity, const char *text)
{
    int length;
    if (capacity <= 0) {
        return;
    }
    length = text != NULL ? (int)strlen(text) : 0;
    if (length > capacity - 1) {
        length = capacity - 1;
    }
    if (length > 0) {
        memcpy(out, text, (size_t)length);
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

static const char *generic_face(const char *family)
{
    if (strcmp(family, "serif") == 0) {
        return "Times New Roman";
    }
    if (strcmp(family, "monospace") == 0) {
        return "Consolas";
    }
    if (strcmp(family, "cursive") == 0) {
        return "Comic Sans MS";
    }
    if (strcmp(family, "fantasy") == 0) {
        return "Impact";
    }
    return "Segoe UI";
}

/* Reads one comma-separated family, unquoting it and trimming whitespace. */
static int next_family(const char *list, int *offset, char *out, int capacity)
{
    const char *p = list + *offset;
    int length = 0;
    int quote = 0;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '\0') {
        *offset = (int)(p - list);
        return 0;
    }
    if (*p == '"' || *p == '\'') {
        quote = *p;
        ++p;
    }
    while (*p != '\0') {
        if (quote != 0) {
            if (*p == quote) {
                ++p;
                break;
            }
        } else if (*p == ',') {
            break;
        }
        if (length < capacity - 1) {
            out[length++] = *p;
        }
        ++p;
    }
    out[length] = '\0';
    while (length > 0 && isspace((unsigned char)out[length - 1])) {
        out[--length] = '\0';
    }
    while (*p != '\0' && *p != ',') {
        ++p;
    }
    if (*p == ',') {
        ++p;
    }
    *offset = (int)(p - list);
    return length > 0;
}

static int backend_has_family(const char *family)
{
#if WN_TEXT_HAS_BACKEND
    if (g_available && g_ops != NULL && g_ops->has_family != NULL) {
        return g_ops->has_family(g_context, family);
    }
#else
    (void)family;
#endif
    return 0;
}

/* Only replace a missing stack with a face the backend proves it has; an
 * unproven name would make measure substitute silently while shape fails. */
static const char *proven_fallback(void)
{
    static const char *const candidates[] = {
        "Segoe UI", "Arial", "Tahoma", "Times New Roman", "Consolas",
    };
    int i;
    for (i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); ++i) {
        if (backend_has_family(candidates[i])) {
            return candidates[i];
        }
    }
    return "Segoe UI";
}

/* Returns 1 when the stack had to be replaced by the proven fallback. */
static int resolve_family_uncached(const char *family, char *out, int capacity)
{
    char item[WN_FAMILY_MAX];
    char picked[WN_FAMILY_MAX];
    int offset = 0;
    int have_family = 0;

    if (family == NULL || family[0] == '\0') {
        copy_text(out, capacity, "Segoe UI");
        return 0;
    }
    while (next_family(family, &offset, item, (int)sizeof(item))) {
        const char *candidate = is_generic_family(item) ? generic_face(item) : item;
        have_family = 1;
        if (backend_has_family(candidate)) {
            copy_text(out, capacity, candidate);
            return 0;
        }
    }
    copy_text(picked, (int)sizeof(picked), proven_fallback());
    copy_text(out, capacity, picked);
    return have_family;
}

static void resolve_family(const char *family, char *out, int capacity)
{
    int index;
    const char *key = family != NULL ? family : "";

    if (g_available && strlen(key) < (size_t)WN_FAMILY_MAX) {
        for (index = 0; index < WN_FAMILY_CACHE; ++index) {
            if (g_family_cache[index].used &&
                strcmp(g_family_cache[index].input, key) == 0) {
                copy_text(out, capacity, g_family_cache[index].face);
                if (g_family_cache[index].fallback) {
                    set_text_error("font stack '%s' has no installed family; using %s",
                                   key, out);
                }
                return;
            }
        }
    }
    {
        int fallback = resolve_family_uncached(key, out, capacity);
        if (fallback) {
            set_text_error("font stack '%s' has no installed family; using %s",
                           key, out);
        }
        if (g_available && strlen(key) < (size_t)WN_FAMILY_MAX) {
            WnFamilyEntry *entry = &g_family_cache[g_family_next % WN_FAMILY_CACHE];
            g_family_next = (g_family_next + 1) % WN_FAMILY_CACHE;
            entry->used = 1;
            entry->fallback = fallback;
            copy_text(entry->input, (int)sizeof(entry->input), key);
            copy_text(entry->face, (int)sizeof(entry->face), out);
        }
    }
}

int wn_text_resolve_family(const char *family, char *out, int capacity)
{
    if (out == NULL || capacity <= 0) {
        return 1;
    }
    g_text_error[0] = '\0';
    (void)ensure_backend();
    resolve_family(family, out, capacity);
    return 0;
}

/* ------------------------------------------------------------ line layout */

typedef struct {
    int used;
    unsigned int hash;
    int bucket_next;
    int lru_prev;
    int lru_next;
    char *text;
    char *family;
    float size;
    int weight;
    float max_width;
    wn_text_line *lines;
    int line_count;
    wn_text_line_layout layout;
} WnLineEntry;

static WnLineEntry g_lines[WN_LINE_CACHE_CAPACITY];
static int g_line_buckets[WN_LINE_BUCKETS];
static int g_line_head = -1;
static int g_line_tail = -1;
static int g_line_used;
static int g_line_ready;

static unsigned int hash_bytes(unsigned int value, const char *text)
{
    const unsigned char *p = (const unsigned char *)(text != NULL ? text : "");
    while (*p != '\0') {
        value ^= *p++;
        value *= 16777619u;
    }
    return value;
}

static unsigned int hash_key(const char *text, const char *family, float size,
                             int weight, float max_width)
{
    union {
        float f;
        unsigned int u;
    } bits;
    unsigned int value = 2166136261u;
    value = hash_bytes(value, text);
    value ^= 0xFFu;
    value *= 16777619u;
    value = hash_bytes(value, family);
    bits.f = size;
    value ^= bits.u;
    value *= 16777619u;
    value ^= (unsigned int)weight;
    value *= 16777619u;
    bits.f = max_width;
    value ^= bits.u;
    value *= 16777619u;
    return value;
}

static void line_entry_clear(WnLineEntry *entry)
{
    if (entry->lines != NULL) {
        for (int i = 0; i < entry->line_count; ++i) {
            free((void *)entry->lines[i].text);
        }
        free(entry->lines);
    }
    free(entry->text);
    free(entry->family);
    memset(entry, 0, sizeof(*entry));
    entry->bucket_next = -1;
    entry->lru_prev = -1;
    entry->lru_next = -1;
}

static void line_cache_reset(void)
{
    for (int i = 0; i < WN_LINE_CACHE_CAPACITY; ++i) {
        if (g_lines[i].used) {
            line_entry_clear(&g_lines[i]);
        }
    }
    for (int i = 0; i < WN_LINE_BUCKETS; ++i) {
        g_line_buckets[i] = -1;
    }
    g_line_head = -1;
    g_line_tail = -1;
    g_line_used = 0;
    g_line_ready = 1;
}

/* Installs `ops` as the only backend and starts from cold caches: the previous
 * context is destroyed (its measure/shape caches die with it), the line and
 * family caches are cleared and the fault counters are disarmed. */
void wn_text_test_use_backend(const wn_text_ops *ops)
{
    if (g_available && g_ops != NULL && g_ops->destroy != NULL) {
        g_ops->destroy(g_context);
    }
    g_context = NULL;
    g_ops = NULL;
    g_available = 0;
    g_initialized = 1;

    line_cache_reset();
    memset(g_family_cache, 0, sizeof(g_family_cache));
    g_family_next = 0;
    g_text_error[0] = '\0';
    g_test_fail_lines = 0;
    g_test_fail_shapes = 0;

    if (ops == NULL) {
        return;
    }
    g_ops = ops;
    if (ops->init == NULL) {
        g_ops = NULL;
        g_context = NULL;
        return;
    }
    if (ops->init(&g_context) != 0) {
        /* A backend may assign a context before failing; do not leak it. */
        if (g_context != NULL && ops->destroy != NULL) {
            ops->destroy(g_context);
        }
        g_ops = NULL;
        g_context = NULL;
        return;
    }
    g_available = 1;
}

static void line_lru_unlink(int slot)
{
    WnLineEntry *entry = &g_lines[slot];
    if (entry->lru_prev >= 0) {
        g_lines[entry->lru_prev].lru_next = entry->lru_next;
    } else {
        g_line_head = entry->lru_next;
    }
    if (entry->lru_next >= 0) {
        g_lines[entry->lru_next].lru_prev = entry->lru_prev;
    } else {
        g_line_tail = entry->lru_prev;
    }
    entry->lru_prev = -1;
    entry->lru_next = -1;
}

static void line_lru_push(int slot)
{
    WnLineEntry *entry = &g_lines[slot];
    entry->lru_prev = -1;
    entry->lru_next = g_line_head;
    if (g_line_head >= 0) {
        g_lines[g_line_head].lru_prev = slot;
    }
    g_line_head = slot;
    if (g_line_tail < 0) {
        g_line_tail = slot;
    }
}

static void line_bucket_unlink(int slot)
{
    WnLineEntry *entry = &g_lines[slot];
    int bucket = (int)(entry->hash & WN_LINE_BUCKET_MASK);
    int index = g_line_buckets[bucket];
    int previous = -1;
    while (index >= 0) {
        if (index == slot) {
            if (previous < 0) {
                g_line_buckets[bucket] = g_lines[index].bucket_next;
            } else {
                g_lines[previous].bucket_next = g_lines[index].bucket_next;
            }
            break;
        }
        previous = index;
        index = g_lines[index].bucket_next;
    }
    entry->bucket_next = -1;
}

static void line_bucket_push(int slot)
{
    WnLineEntry *entry = &g_lines[slot];
    int bucket = (int)(entry->hash & WN_LINE_BUCKET_MASK);
    entry->bucket_next = g_line_buckets[bucket];
    g_line_buckets[bucket] = slot;
}

static int line_entry_matches(const WnLineEntry *entry, const char *text, const char *family,
                              float size, int weight, float max_width)
{
    return entry->used && entry->weight == weight && entry->size == size &&
           entry->max_width == max_width && strcmp(entry->text, text) == 0 &&
           strcmp(entry->family, family) == 0;
}

static int line_append(WnLineEntry *entry, int start, int end, float width)
{
    wn_text_line *line = &entry->lines[entry->line_count];
    size_t bytes = (size_t)(end - start);
    char *slice = (char *)malloc(bytes + 1u);
    if (slice == NULL) {
        return 1;
    }
    if (bytes > 0) {
        memcpy(slice, entry->text + start, bytes);
    }
    slice[bytes] = '\0';
    line->start = start;
    line->end = end;
    line->width = width;
    line->text = slice;
    entry->line_count += 1;
    return 0;
}

static int measure_slice(const char *text, int start, int end, const char *family,
                         float size, int weight, float *width)
{
    char stack_buffer[256];
    char *buffer = stack_buffer;
    wn_text_metrics metrics;
    int length = end - start;
    int result;
    if (length <= 0) {
        *width = 0.0f;
        return 0;
    }
    if (length >= (int)sizeof(stack_buffer)) {
        buffer = (char *)malloc((size_t)length + 1u);
        if (buffer == NULL) {
            return 1;
        }
    }
    memcpy(buffer, text + start, (size_t)length);
    buffer[length] = '\0';
    memset(&metrics, 0, sizeof(metrics));
    result = wn_text_measure_string(buffer, family, size, weight, 0.0f, &metrics);
    if (buffer != stack_buffer) {
        free(buffer);
    }
    if (result != 0) {
        return 1;
    }
    *width = metrics.width;
    return 0;
}

/* Greedy: break at spaces; a word that does not fit alone overflows its line.
 * Trailing spaces never extend a line and never create an empty last line. */
static int build_lines(WnLineEntry *entry)
{
    const char *text = entry->text;
    int length;
    if (g_test_fail_lines > 0) {
        g_test_fail_lines -= 1;
        return 1;
    }
    length = (int)strlen(text);
    wn_text_metrics whole;
    float line_height;
    int have_whole;
    int measured;

    memset(&whole, 0, sizeof(whole));
    have_whole = wn_text_measure_string(text, entry->family, entry->size, entry->weight,
                                        0.0f, &whole) == 0;
    measured = have_whole && (whole.ascent + whole.descent > 0.0f || whole.height > 0.0f);
    line_height = have_whole && whole.ascent + whole.descent > 0.0f
                      ? whole.ascent + whole.descent
                      : (have_whole && whole.height > 0.0f
                             ? whole.height
                             : (entry->size > 0.0f ? entry->size * 1.2f : 19.2f));
    entry->layout.measured_height = have_whole && whole.height > 0.0f
                                        ? whole.height
                                        : line_height;
    entry->layout.ascent = have_whole ? whole.ascent : line_height;
    entry->layout.descent = have_whole ? whole.descent : 0.0f;
    entry->layout.line_height = line_height;
    entry->layout.measured = measured;

    entry->lines = (wn_text_line *)calloc((size_t)length + 1u, sizeof(wn_text_line));
    if (entry->lines == NULL) {
        return 1;
    }

    if (length == 0 || entry->max_width <= 0.0f) {
        if (line_append(entry, 0, length, have_whole ? whole.width : 0.0f) != 0) {
            return 1;
        }
    } else {
        int cursor = 0;
        while (cursor < length) {
            int line_start;
            int line_end = -1;
            int last_break = -1;
            int scan;
            float width = 0.0f;

            while (cursor < length && text[cursor] == ' ') {
                ++cursor;
            }
            if (cursor >= length) {
                break;
            }
            line_start = cursor;
            scan = cursor;
            while (scan < length) {
                int word_start = scan;
                int word_end;
                float candidate_width = 0.0f;
                while (scan < length && text[scan] != ' ') {
                    ++scan;
                }
                word_end = scan;
                if (word_end <= word_start) {
                    ++scan;
                    continue;
                }
                if (measure_slice(text, line_start, word_end, entry->family, entry->size,
                                  entry->weight, &candidate_width) != 0) {
                    return 1;
                }
                if (candidate_width <= entry->max_width + 0.01f) {
                    last_break = word_end;
                    width = candidate_width;
                    while (scan < length && text[scan] == ' ') {
                        ++scan;
                    }
                    if (scan >= length) {
                        line_end = word_end;
                        break;
                    }
                } else if (last_break < 0) {
                    /* First word is wider than the line: let it overflow. */
                    last_break = word_end;
                    line_end = word_end;
                    width = candidate_width;
                    break;
                } else {
                    line_end = last_break;
                    break;
                }
            }
            if (line_end < 0) {
                line_end = last_break >= 0 ? last_break : length;
            }
            if (line_end <= line_start) {
                line_end = line_start + 1;
            }
            if (line_append(entry, line_start, line_end, width) != 0) {
                return 1;
            }
            cursor = line_end;
            while (cursor < length && text[cursor] == ' ') {
                ++cursor;
            }
        }
        if (entry->line_count == 0) {
            if (line_append(entry, 0, 0, 0.0f) != 0) {
                return 1;
            }
        }
    }

    entry->layout.lines = entry->lines;
    entry->layout.line_count = entry->line_count;
    entry->layout.width = 0.0f;
    for (int i = 0; i < entry->line_count; ++i) {
        if (entry->lines[i].width > entry->layout.width) {
            entry->layout.width = entry->lines[i].width;
        }
    }
    entry->layout.height = (float)entry->line_count * line_height;
    return 0;
}

static int line_find_unused(void)
{
    for (int i = 0; i < WN_LINE_CACHE_CAPACITY; ++i) {
        if (!g_lines[i].used) {
            return i;
        }
    }
    return -1;
}

const wn_text_line_layout *wn_text_line_layout_get(const char *utf8, const char *family,
                                                  float size, int weight, float max_width)
{
    char face[WN_FAMILY_MAX];
    unsigned int hash;
    const char *text = utf8 != NULL ? utf8 : "";
    const char *raw_family = family != NULL ? family : "";
    int slot;
    WnLineEntry *entry;

    if (!ensure_backend()) {
        return NULL;
    }
    if (!g_line_ready) {
        line_cache_reset();
    }
    if (size <= 0.0f) {
        size = 16.0f;
    }
    if (weight <= 0) {
        weight = 400;
    }
    resolve_family(raw_family, face, (int)sizeof(face));
    if (max_width < 0.0f) {
        max_width = 0.0f;
    }
    hash = hash_key(text, face, size, weight, max_width);

    for (slot = g_line_buckets[hash & WN_LINE_BUCKET_MASK]; slot >= 0;
         slot = g_lines[slot].bucket_next) {
        entry = &g_lines[slot];
        if (line_entry_matches(entry, text, face, size, weight, max_width)) {
            line_lru_unlink(slot);
            line_lru_push(slot);
            return &entry->layout;
        }
    }

    /* Build into a scratch entry first: a failed insert must not evict a
     * published entry or consume capacity. */
    WnLineEntry scratch;
    int fresh = 0;
    memset(&scratch, 0, sizeof(scratch));
    scratch.text = (char *)malloc(strlen(text) + 1u);
    scratch.family = (char *)malloc(strlen(face) + 1u);
    if (scratch.text == NULL || scratch.family == NULL) {
        line_entry_clear(&scratch);
        set_text_error("line cache allocation failed");
        return NULL;
    }
    strcpy(scratch.text, text);
    strcpy(scratch.family, face);
    scratch.size = size;
    scratch.weight = weight;
    scratch.max_width = max_width;
    scratch.hash = hash;
    if (build_lines(&scratch) != 0) {
        line_entry_clear(&scratch);
        set_text_error("line layout failed for '%.32s'", text);
        return NULL;
    }

    if (g_line_used < WN_LINE_CACHE_CAPACITY) {
        slot = g_line_used;
        fresh = 1;
    } else {
        slot = g_line_tail;
        if (slot >= 0) {
            line_lru_unlink(slot);
            line_bucket_unlink(slot);
        } else {
            slot = line_find_unused();
            if (slot < 0) {
                line_entry_clear(&scratch);
                set_text_error("line cache is full with no evictable entry");
                return NULL;
            }
        }
    }
    line_entry_clear(&g_lines[slot]);
    g_lines[slot] = scratch;
    g_lines[slot].used = 1;
    if (fresh) {
        g_line_used += 1;
    }
    line_bucket_push(slot);
    line_lru_push(slot);
    return &g_lines[slot].layout;
}

int wn_text_block_get(const char *utf8, const char *family, float size, int weight,
                      float wrap_width, float line_height_override, wn_text_block *out)
{
    const wn_text_line_layout *layout;
    float natural;
    float line_height;

    if (out == NULL) {
        return -1;
    }
    layout = wn_text_line_layout_get(utf8, family, size, weight, wrap_width);
    if (layout == NULL) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    /* Same float order layout and paint used: natural first, the override
     * replaces it when positive, then the line count scales it. */
    natural = layout->line_height;
    line_height = line_height_override > 0.0f ? line_height_override : natural;
    out->layout = layout;
    out->line_count = layout->line_count;
    out->natural = natural;
    out->line_height = line_height;
    out->box_height = (float)layout->line_count * line_height;
    out->half_leading = (line_height - natural) * 0.5f;
    out->ascent = layout->ascent;
    out->descent = layout->descent;
    out->measured_height = layout->measured_height;
    out->measured = layout->measured;
    return 0;
}

/* --------------------------------------------------------------- backend */

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
    char face[WN_FAMILY_MAX];
    int result;
    if (out == NULL) {
        return 1;
    }
    if (!ensure_backend()) {
        set_text_error("text backend is not available");
        return 1;
    }
    resolve_family(family, face, (int)sizeof(face));
    style.family = face;
    style.size = size > 0.0f ? size : 16.0f;
    style.weight = weight > 0 ? weight : 400;
    result = g_ops->measure(g_context, utf8 != NULL ? utf8 : "", &style, max_width, out);
    if (result != 0) {
        set_text_error("measure failed (family=%s size=%.1f)", face, style.size);
    }
    return result;
}

int wn_text_shape_string(const char *utf8, const char *family, float size, int weight,
                         wn_text_glyph *glyphs, int capacity, wn_text_shaped *out)
{
    wn_text_style style;
    char face[WN_FAMILY_MAX];
    int result;
    if (out == NULL) {
        return 1;
    }
    if (!ensure_backend()) {
        set_text_error("text backend is not available");
        return 1;
    }
    if (g_test_fail_shapes > 0) {
        g_test_fail_shapes -= 1;
        set_text_error("shape failed (injected)");
        return 1;
    }
    resolve_family(family, face, (int)sizeof(face));
    style.family = face;
    style.size = size > 0.0f ? size : 16.0f;
    style.weight = weight > 0 ? weight : 400;
    result = g_ops->shape(g_context, utf8 != NULL ? utf8 : "", &style, glyphs, capacity, out);
    if (result != 0) {
        set_text_error("shape failed (family=%s size=%.1f)", face, style.size);
    }
    return result;
}

int wn_text_rasterize_glyph(const char *family, float size, int weight, int glyph,
                            unsigned char *pixels, int capacity, wn_text_bitmap *out)
{
    wn_text_style style;
    char face[WN_FAMILY_MAX];
    int result;
    if (out == NULL) {
        return 1;
    }
    if (!ensure_backend()) {
        set_text_error("text backend is not available");
        return 1;
    }
    resolve_family(family, face, (int)sizeof(face));
    style.family = face;
    style.size = size > 0.0f ? size : 16.0f;
    style.weight = weight > 0 ? weight : 400;
    result = g_ops->rasterize(g_context, &style, glyph, pixels, capacity, out);
    if (result != 0) {
        set_text_error("rasterize failed (family=%s size=%.1f glyph=%d)",
                       face, style.size, glyph);
    }
    return result;
}

int wn_text_runtime_cache_misses(void)
{
    if (!ensure_backend()) {
        return -1;
    }
    return g_ops->cache_misses(g_context);
}

int wn_text_runtime_cache_stats(wn_text_cache_stats *out)
{
    if (out == NULL) {
        return 1;
    }
    memset(out, 0, sizeof(*out));
    if (!ensure_backend() || g_ops->cache_stats == NULL) {
        return 1;
    }
    g_ops->cache_stats(g_context, out);
    return 0;
}

void wn_text_runtime_shutdown(void)
{
    if (g_line_ready) {
        line_cache_reset();
        g_line_ready = 0;
        for (int i = 0; i < WN_LINE_BUCKETS; ++i) {
            g_line_buckets[i] = -1;
        }
    }
    memset(g_family_cache, 0, sizeof(g_family_cache));
    g_family_next = 0;
    g_text_error[0] = '\0';
    g_test_fail_lines = 0;
    g_test_fail_shapes = 0;
    if (g_available && g_ops != NULL && g_ops->destroy != NULL) {
        g_ops->destroy(g_context);
    }
    g_context = NULL;
    g_ops = NULL;
    g_available = 0;
    g_initialized = 0;
}
