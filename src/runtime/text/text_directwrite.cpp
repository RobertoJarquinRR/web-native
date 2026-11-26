#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwrite.h>
#include <dwrite_2.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#include "wn_text.h"

#define WN_TEXT_CACHE_CAPACITY 1024
#define WN_TEXT_CACHE_BUCKETS 2048
#define WN_TEXT_CACHE_MASK (WN_TEXT_CACHE_BUCKETS - 1)
#define WN_TEXT_FACE_CAPACITY 16
#define WN_TEXT_MAX_UTF16 4096
#define WN_TEXT_MAX_GLYPHS 4096
#define WN_TEXT_UNBOUNDED 1000000.0f

typedef struct {
    int used;
    unsigned int hash;
    int bucket_next;
    int lru_prev;
    int lru_next;
    char *family;
    float size;
    int weight;
    float max_width;
    char *text;
    wn_text_metrics metrics;
} WnTextEntry;

typedef struct {
    char *family;
    int weight;
    IDWriteFontFace *face;
    float units_per_em;
} WnFontFaceEntry;

typedef struct {
    IDWriteFactory *factory;
    IDWriteFactory2 *factory2;
    IDWriteRenderingParams *rendering;
    WnTextEntry entries[WN_TEXT_CACHE_CAPACITY];
    int buckets[WN_TEXT_CACHE_BUCKETS];
    int lru_head;
    int lru_tail;
    int used_count;
    WnFontFaceEntry faces[WN_TEXT_FACE_CAPACITY];
    int next_face;
    int hits;
    int misses;
    int evictions;
} WnDirectWrite;

static bool same_request(const WnTextEntry *entry, const char *family, float size, int weight,
                         float max_width, const char *text)
{
    return entry->weight == weight && entry->size == size && entry->max_width == max_width &&
           std::strcmp(entry->family, family) == 0 && std::strcmp(entry->text, text) == 0;
}

static unsigned int request_hash(const char *family, float size, int weight,
                                 float max_width, const char *text)
{
    union {
        float f;
        unsigned int u;
    } bits;
    unsigned int value = 2166136261u;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p != 0; ++p) {
        value ^= *p;
        value *= 16777619u;
    }
    value ^= 0xFFu;
    value *= 16777619u;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(family); *p != 0; ++p) {
        value ^= *p;
        value *= 16777619u;
    }
    bits.f = size;
    value ^= bits.u;
    value *= 16777619u;
    value ^= static_cast<unsigned int>(weight);
    value *= 16777619u;
    bits.f = max_width;
    value ^= bits.u;
    value *= 16777619u;
    return value;
}

static void entry_clear(WnTextEntry *entry)
{
    std::free(entry->family);
    std::free(entry->text);
    std::memset(entry, 0, sizeof(*entry));
    entry->bucket_next = -1;
    entry->lru_prev = -1;
    entry->lru_next = -1;
}

static void cache_lru_unlink(WnDirectWrite *ctx, int slot)
{
    WnTextEntry *entry = &ctx->entries[slot];
    if (entry->lru_prev >= 0) {
        ctx->entries[entry->lru_prev].lru_next = entry->lru_next;
    } else {
        ctx->lru_head = entry->lru_next;
    }
    if (entry->lru_next >= 0) {
        ctx->entries[entry->lru_next].lru_prev = entry->lru_prev;
    } else {
        ctx->lru_tail = entry->lru_prev;
    }
    entry->lru_prev = -1;
    entry->lru_next = -1;
}

static void cache_lru_push(WnDirectWrite *ctx, int slot)
{
    WnTextEntry *entry = &ctx->entries[slot];
    entry->lru_prev = -1;
    entry->lru_next = ctx->lru_head;
    if (ctx->lru_head >= 0) {
        ctx->entries[ctx->lru_head].lru_prev = slot;
    }
    ctx->lru_head = slot;
    if (ctx->lru_tail < 0) {
        ctx->lru_tail = slot;
    }
}

static void cache_bucket_unlink(WnDirectWrite *ctx, int slot)
{
    WnTextEntry *entry = &ctx->entries[slot];
    int bucket = static_cast<int>(entry->hash & WN_TEXT_CACHE_MASK);
    int index = ctx->buckets[bucket];
    int previous = -1;
    while (index >= 0) {
        if (index == slot) {
            if (previous < 0) {
                ctx->buckets[bucket] = ctx->entries[index].bucket_next;
            } else {
                ctx->entries[previous].bucket_next = ctx->entries[index].bucket_next;
            }
            break;
        }
        previous = index;
        index = ctx->entries[index].bucket_next;
    }
    entry->bucket_next = -1;
}

static void cache_bucket_push(WnDirectWrite *ctx, int slot)
{
    WnTextEntry *entry = &ctx->entries[slot];
    int bucket = static_cast<int>(entry->hash & WN_TEXT_CACHE_MASK);
    entry->bucket_next = ctx->buckets[bucket];
    ctx->buckets[bucket] = slot;
}

static int cache_lookup(WnDirectWrite *ctx, const char *family, float size, int weight,
                        float max_width, const char *text, unsigned int hash)
{
    for (int slot = ctx->buckets[hash & WN_TEXT_CACHE_MASK]; slot >= 0;
         slot = ctx->entries[slot].bucket_next) {
        WnTextEntry *entry = &ctx->entries[slot];
        if (entry->used && same_request(entry, family, size, weight, max_width, text)) {
            cache_lru_unlink(ctx, slot);
            cache_lru_push(ctx, slot);
            ctx->hits += 1;
            return slot;
        }
    }
    return -1;
}

static int decode_utf8(const unsigned char *p, uint32_t *out)
{
    uint32_t cp;
    int extra;
    int i;
    if (*p < 0x80u) {
        *out = (uint32_t)*p;
        return 1;
    }
    if ((*p & 0xE0u) == 0xC0u) {
        cp = (uint32_t)(*p & 0x1Fu);
        extra = 1;
    } else if ((*p & 0xF0u) == 0xE0u) {
        cp = (uint32_t)(*p & 0x0Fu);
        extra = 2;
    } else if ((*p & 0xF8u) == 0xF0u) {
        cp = (uint32_t)(*p & 0x07u);
        extra = 3;
    } else {
        return 0;
    }
    for (i = 1; i <= extra; ++i) {
        unsigned char next = p[i];
        if (next == 0u || (next & 0xC0u) != 0x80u) {
            return 0;
        }
        cp = (cp << 6) | (uint32_t)(next & 0x3Fu);
    }
    if ((extra == 1 && cp < 0x80u) || (extra == 2 && cp < 0x800u) ||
        (extra == 3 && cp < 0x10000u)) {
        return 0;
    }
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        return 0;
    }
    *out = cp;
    return extra + 1;
}

/* Converts up to capacity-1 UTF-16 units. Longer strings keep their prefix
 * (size truncation, not a hard error) and report it through `truncated`. */
static int utf8_to_wide(const char *text, wchar_t *wide, int capacity, int *truncated)
{
    const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
    int units = 0;
    if (truncated != nullptr) {
        *truncated = 0;
    }
    if (capacity <= 1) {
        if (capacity == 1) {
            wide[0] = L'\0';
        }
        if (truncated != nullptr && text != nullptr && text[0] != '\0') {
            *truncated = 1;
        }
        return 0;
    }
    while (*p != 0) {
        uint32_t cp = 0;
        int length = decode_utf8(p, &cp);
        int needed;
        if (length <= 0) {
            ++p;
            continue;
        }
        needed = cp >= 0x10000u ? 2 : 1;
        if (units + needed > capacity - 1) {
            if (truncated != nullptr) {
                *truncated = 1;
            }
            break;
        }
        if (needed == 2) {
            uint32_t value = cp - 0x10000u;
            wide[units++] = static_cast<wchar_t>(0xD800u + (value >> 10));
            wide[units++] = static_cast<wchar_t>(0xDC00u + (value & 0x3FFu));
        } else {
            wide[units++] = static_cast<wchar_t>(cp);
        }
        p += length;
    }
    wide[units] = L'\0';
    return units;
}

/* Font sizes come from an untrusted .wnui; keep every downstream area
 * computation bounded. */
static float clamp_font_size(float size)
{
    if (!(size > 0.0f)) {
        return 16.0f;
    }
    if (size < 1.0f) {
        return 1.0f;
    }
    if (size > 1024.0f) {
        return 1024.0f;
    }
    return size;
}

/* CSS `font-weight` (100..=900) -> DWRITE_FONT_WEIGHT. The canonical CSS
 * numbers are the DWRITE constants; an in-between value (e.g. 450 or 550) picks
 * the nearest canonical weight (450 -> 400, 550 -> 500). That is deliberately
 * *not* CSS Fonts 4's 400-500 preference, which would pick 500 for 450: the
 * runtime follows the DirectWrite canonical scale so measure and shape agree.
 * 400 and 700 map to NORMAL/BOLD exactly as before, so pages that only used
 * those weights keep byte-identical output.
 *
 * DirectWrite then selects the nearest *installed* face of the family
 * (`GetFirstMatchingFont` / text-format fallback). Synthesis is not forced:
 * when the family lacks the requested weight DirectWrite may simulate bold,
 * otherwise it renders with the closest real face (or with the family fallback
 * if the family itself is missing). */
static DWRITE_FONT_WEIGHT dwrite_weight(int weight)
{
    if (weight < 150) {
        return DWRITE_FONT_WEIGHT_THIN; /* 100 */
    }
    if (weight < 250) {
        return DWRITE_FONT_WEIGHT_EXTRA_LIGHT; /* 200 */
    }
    if (weight < 325) {
        return DWRITE_FONT_WEIGHT_LIGHT; /* 300 */
    }
    if (weight < 375) {
        return DWRITE_FONT_WEIGHT_SEMI_LIGHT; /* 350 */
    }
    if (weight <= 450) {
        return DWRITE_FONT_WEIGHT_NORMAL; /* 400 */
    }
    if (weight <= 550) {
        return DWRITE_FONT_WEIGHT_MEDIUM; /* 500 */
    }
    if (weight <= 650) {
        return DWRITE_FONT_WEIGHT_SEMI_BOLD; /* 600 */
    }
    if (weight <= 750) {
        return DWRITE_FONT_WEIGHT_BOLD; /* 700 */
    }
    if (weight <= 850) {
        return DWRITE_FONT_WEIGHT_EXTRA_BOLD; /* 800 */
    }
    return DWRITE_FONT_WEIGHT_BLACK; /* 900 */
}

static int create_text_format(WnDirectWrite *ctx, const wn_text_style *style, IDWriteTextFormat **out)
{
    wchar_t family[128];
    DWRITE_FONT_WEIGHT weight = dwrite_weight(style->weight);
    if (utf8_to_wide(style->family != nullptr ? style->family : "Segoe UI", family,
                     static_cast<int>(sizeof(family) / sizeof(family[0])), nullptr) == 0) {
        return 1;
    }
    HRESULT hr = ctx->factory->CreateTextFormat(
        family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        clamp_font_size(style->size), L"", out);
    return FAILED(hr) || *out == nullptr ? 1 : 0;
}

static int measure_uncached(WnDirectWrite *ctx, const char *utf8, const wn_text_style *style,
                            float max_width, wn_text_metrics *out)
{
    IDWriteTextFormat *format = nullptr;
    IDWriteTextLayout *layout = nullptr;
    wchar_t text[WN_TEXT_MAX_UTF16];
    DWRITE_TEXT_METRICS metrics{};
    std::unique_ptr<DWRITE_LINE_METRICS[]> lines;
    UINT32 line_count = 0;
    HRESULT hr = S_OK;
    int length = 0;

    if (create_text_format(ctx, style, &format) != 0) {
        return 1;
    }
    length = utf8_to_wide(utf8, text, WN_TEXT_MAX_UTF16, nullptr);
    if (length == 0) {
        out->width = 0.0f;
        out->height = style->size > 0.0f ? style->size : 16.0f;
        out->ascent = out->height;
        out->descent = 0.0f;
        format->Release();
        return 0;
    }
    hr = ctx->factory->CreateTextLayout(
        text, static_cast<UINT32>(length), format,
        max_width > 0.0f ? max_width : WN_TEXT_UNBOUNDED, WN_TEXT_UNBOUNDED, &layout);
    format->Release();
    if (FAILED(hr) || layout == nullptr) {
        return 1;
    }
    hr = layout->GetMetrics(&metrics);
    if (SUCCEEDED(hr)) {
        /* Wrapped layouts have more than one line: query all of them (one
         * entry would return E_NOT_SUFFICIENT_BUFFER). */
        UINT32 requested = metrics.lineCount > 0 ? metrics.lineCount : 1;
        lines.reset(new (std::nothrow) DWRITE_LINE_METRICS[requested]);
        if (lines == nullptr) {
            hr = E_OUTOFMEMORY;
        } else {
            hr = layout->GetLineMetrics(lines.get(), requested, &line_count);
        }
    }
    layout->Release();
    if (FAILED(hr) || line_count == 0 || lines == nullptr) {
        return 1;
    }
    out->width = metrics.widthIncludingTrailingWhitespace;
    out->height = metrics.height;
    out->ascent = lines[0].baseline;
    out->descent = lines[0].height > lines[0].baseline
                       ? lines[0].height - lines[0].baseline
                       : 0.0f;
    return 0;
}

static IDWriteFontFace *find_face(WnDirectWrite *ctx, const wn_text_style *style, float *units_per_em)
{
    const char *family = style->family != nullptr ? style->family : "Segoe UI";
    const DWRITE_FONT_WEIGHT requested = dwrite_weight(style->weight);
    for (int slot = 0; slot < WN_TEXT_FACE_CAPACITY; ++slot) {
        WnFontFaceEntry *entry = &ctx->faces[slot];
        if (entry->face != nullptr && entry->family != nullptr &&
            entry->weight == static_cast<int>(requested) &&
            std::strcmp(entry->family, family) == 0) {
            *units_per_em = entry->units_per_em;
            return entry->face;
        }
    }

    IDWriteFontCollection *collection = nullptr;
    IDWriteFontFamily *font_family = nullptr;
    IDWriteFont *font = nullptr;
    IDWriteFontFace *face = nullptr;
    DWRITE_FONT_METRICS metrics{};
    wchar_t wide[128];
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (utf8_to_wide(family, wide, static_cast<int>(sizeof(wide) / sizeof(wide[0])),
                     nullptr) == 0) {
        return nullptr;
    }
    if (FAILED(ctx->factory->GetSystemFontCollection(&collection, FALSE)) || collection == nullptr) {
        return nullptr;
    }
    if (FAILED(collection->FindFamilyName(wide, &index, &exists)) || !exists ||
        FAILED(collection->GetFontFamily(index, &font_family)) || font_family == nullptr) {
        collection->Release();
        return nullptr;
    }
    collection->Release();
    /* Nearest installed face: DirectWrite compares the requested weight with
     * the faces of the family and returns the closest one. No synthetic bold
     * is applied. */
    if (FAILED(font_family->GetFirstMatchingFont(requested, DWRITE_FONT_STRETCH_NORMAL,
                                                 DWRITE_FONT_STYLE_NORMAL, &font)) ||
        font == nullptr) {
        font_family->Release();
        return nullptr;
    }
    font_family->Release();
    if (FAILED(font->CreateFontFace(&face)) || face == nullptr) {
        font->Release();
        return nullptr;
    }
    font->Release();
    face->GetMetrics(&metrics);

    /* Commit the slot only after every allocation succeeded: a failure here
     * must leave the existing entry intact. */
    {
        char *family_copy = static_cast<char *>(std::malloc(std::strlen(family) + 1));
        WnFontFaceEntry *entry;
        if (family_copy == nullptr) {
            face->Release();
            return nullptr;
        }
        std::strcpy(family_copy, family);
        entry = &ctx->faces[ctx->next_face];
        ctx->next_face = (ctx->next_face + 1) % WN_TEXT_FACE_CAPACITY;
        if (entry->face != nullptr) {
            entry->face->Release();
        }
        std::free(entry->family);
        entry->family = family_copy;
        entry->weight = static_cast<int>(requested);
        entry->face = face;
        entry->units_per_em = static_cast<float>(
            metrics.designUnitsPerEm > 0 ? metrics.designUnitsPerEm : 2048);
        *units_per_em = entry->units_per_em;
    }
    return face;
}

static int text_init(void **out)
{
    WnDirectWrite *ctx = nullptr;
    HRESULT hr;

    if (out == nullptr) {
        return 1;
    }
    *out = nullptr;
    ctx = static_cast<WnDirectWrite *>(std::calloc(1, sizeof(*ctx)));
    if (ctx == nullptr) {
        return 1;
    }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown **>(&ctx->factory));
    if (FAILED(hr) || ctx->factory == nullptr) {
        std::free(ctx);
        return 1;
    }
    if (FAILED(ctx->factory->CreateRenderingParams(&ctx->rendering))) {
        ctx->rendering = nullptr;
    }
    if (FAILED(ctx->factory->QueryInterface(__uuidof(IDWriteFactory2),
                                            reinterpret_cast<void **>(&ctx->factory2)))) {
        ctx->factory2 = nullptr;
    }
    for (int slot = 0; slot < WN_TEXT_CACHE_BUCKETS; ++slot) {
        ctx->buckets[slot] = -1;
    }
    ctx->lru_head = -1;
    ctx->lru_tail = -1;
    *out = ctx;
    return 0;
}

static void text_destroy(void *context)
{
    WnDirectWrite *ctx = static_cast<WnDirectWrite *>(context);

    if (ctx == nullptr) {
        return;
    }
    for (int slot = 0; slot < WN_TEXT_CACHE_CAPACITY; ++slot) {
        entry_clear(&ctx->entries[slot]);
    }
    for (int slot = 0; slot < WN_TEXT_FACE_CAPACITY; ++slot) {
        if (ctx->faces[slot].face != nullptr) {
            ctx->faces[slot].face->Release();
        }
        std::free(ctx->faces[slot].family);
    }
    if (ctx->rendering != nullptr) {
        ctx->rendering->Release();
    }
    if (ctx->factory2 != nullptr) {
        ctx->factory2->Release();
    }
    if (ctx->factory != nullptr) {
        ctx->factory->Release();
    }
    std::free(ctx);
}

static int cache_store(WnDirectWrite *ctx, const char *family, const wn_text_style *style,
                       float max_width, const char *text, const wn_text_metrics *metrics,
                       unsigned int hash)
{
    char *family_copy = static_cast<char *>(std::malloc(std::strlen(family) + 1));
    char *text_copy = static_cast<char *>(std::malloc(std::strlen(text) + 1));
    int slot;
    WnTextEntry *entry;

    if (family_copy == nullptr || text_copy == nullptr) {
        std::free(family_copy);
        std::free(text_copy);
        return 1;
    }
    std::strcpy(family_copy, family);
    std::strcpy(text_copy, text);

    if (ctx->used_count < WN_TEXT_CACHE_CAPACITY) {
        slot = ctx->used_count;
        ctx->used_count += 1;
        entry_clear(&ctx->entries[slot]);
    } else {
        slot = ctx->lru_tail;
        if (slot < 0) {
            std::free(family_copy);
            std::free(text_copy);
            return 1;
        }
        cache_lru_unlink(ctx, slot);
        cache_bucket_unlink(ctx, slot);
        entry_clear(&ctx->entries[slot]);
        ctx->evictions += 1;
    }
    entry = &ctx->entries[slot];
    entry->used = 1;
    entry->hash = hash;
    entry->family = family_copy;
    entry->size = style->size;
    entry->weight = style->weight;
    entry->max_width = max_width;
    entry->text = text_copy;
    entry->metrics = *metrics;
    cache_bucket_push(ctx, slot);
    cache_lru_push(ctx, slot);
    ctx->misses += 1;
    return 0;
}

static int text_measure(void *context, const char *utf8, const wn_text_style *style,
                        float max_width, wn_text_metrics *out)
{
    WnDirectWrite *ctx = static_cast<WnDirectWrite *>(context);
    wn_text_style safe;
    const char *family;
    unsigned int hash;
    int slot;

    if (ctx == nullptr || utf8 == nullptr || style == nullptr || out == nullptr) {
        return 1;
    }
    safe = *style;
    safe.size = clamp_font_size(safe.size);
    if (safe.weight <= 0) {
        safe.weight = 400;
    }
    style = &safe;
    family = style->family != nullptr ? style->family : "Segoe UI";
    // Wrap width changes the result, so it is part of the cache key.
    hash = request_hash(family, style->size, style->weight, max_width, utf8);
    slot = cache_lookup(ctx, family, style->size, style->weight, max_width, utf8, hash);
    if (slot >= 0) {
        *out = ctx->entries[slot].metrics;
        return 0;
    }
    if (measure_uncached(ctx, utf8, style, max_width, out) != 0) {
        return 1;
    }
    cache_store(ctx, family, style, max_width, utf8, out, hash);
    return 0;
}

static int text_shape(void *context, const char *utf8, const wn_text_style *style,
                      wn_text_glyph *glyphs, int capacity, wn_text_shaped *out)
{
    WnDirectWrite *ctx = static_cast<WnDirectWrite *>(context);
    wn_text_style safe;
    IDWriteFontFace *face;
    wchar_t text[WN_TEXT_MAX_UTF16];
    UINT32 codepoints[WN_TEXT_MAX_GLYPHS];
    UINT16 indices[WN_TEXT_MAX_GLYPHS];
    DWRITE_GLYPH_METRICS glyph_metrics[WN_TEXT_MAX_GLYPHS];
    wn_text_metrics metrics;
    float units_per_em = 0.0f;
    float pen = 0.0f;
    int length;
    int count = 0;
    int written = 0;
    int truncated = 0;

    if (ctx == nullptr || utf8 == nullptr || style == nullptr || out == nullptr) {
        return 1;
    }
    safe = *style;
    safe.size = clamp_font_size(safe.size);
    if (safe.weight <= 0) {
        safe.weight = 400;
    }
    style = &safe;
    face = find_face(ctx, style, &units_per_em);
    if (face == nullptr) {
        return 1;
    }
    length = utf8_to_wide(utf8, text, WN_TEXT_MAX_UTF16, &truncated);
    for (int i = 0; i < length && count < WN_TEXT_MAX_GLYPHS; ++i) {
        UINT32 cp = static_cast<UINT16>(text[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < length) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<UINT16>(text[i + 1]) - 0xDC00);
            ++i;
        }
        codepoints[count++] = cp;
    }
    if (count > 0 &&
        FAILED(face->GetGlyphIndices(codepoints, static_cast<UINT32>(count), indices))) {
        return 1;
    }
    if (count > 0 && FAILED(face->GetDesignGlyphMetrics(indices, static_cast<UINT32>(count),
                                                        glyph_metrics, FALSE))) {
        return 1;
    }
    if (measure_uncached(ctx, utf8, style, 0.0f, &metrics) != 0) {
        return 1;
    }

    out->glyphs = glyphs;
    out->width = metrics.width;
    out->height = metrics.height;
    out->ascent = metrics.ascent;
    out->descent = metrics.descent;
    out->truncated = truncated;

    for (int i = 0; i < count; ++i) {
        float advance = static_cast<float>(glyph_metrics[i].advanceWidth) *
                        style->size / units_per_em;
        if (glyphs != nullptr && written < capacity) {
            glyphs[written].glyph = indices[i];
            glyphs[written].advance = advance;
            glyphs[written].x = pen;
            ++written;
        }
        pen += advance;
    }
    out->glyph_count = written;
    return 0;
}

static int text_rasterize(void *context, const wn_text_style *style, int glyph,
                          unsigned char *pixels, int capacity, wn_text_bitmap *out)
{
    WnDirectWrite *ctx = static_cast<WnDirectWrite *>(context);
    wn_text_style safe;
    IDWriteFontFace *face;
    IDWriteGlyphRunAnalysis *analysis = nullptr;
    DWRITE_GLYPH_RUN run{};
    RECT bounds{};
    float units_per_em = 0.0f;
    float advance;
    int width;
    int height;
    size_t area;
    UINT16 index = static_cast<UINT16>(glyph);
    DWRITE_GLYPH_METRICS glyph_metrics{};
    HRESULT hr = S_OK;

    if (ctx == nullptr || style == nullptr || out == nullptr) {
        return 1;
    }
    safe = *style;
    safe.size = clamp_font_size(safe.size);
    if (safe.weight <= 0) {
        safe.weight = 400;
    }
    style = &safe;
    face = find_face(ctx, style, &units_per_em);
    if (face == nullptr || FAILED(face->GetDesignGlyphMetrics(&index, 1, &glyph_metrics, FALSE))) {
        return 1;
    }
    advance = static_cast<float>(glyph_metrics.advanceWidth) * style->size / units_per_em;
    run.fontFace = face;
    run.fontEmSize = style->size;
    run.glyphCount = 1;
    run.glyphIndices = &index;
    run.glyphAdvances = &advance;
    run.glyphOffsets = nullptr;
    run.bidiLevel = 0;
    run.isSideways = FALSE;

    if (ctx->factory2 != nullptr) {
        hr = ctx->factory2->CreateGlyphRunAnalysis(
            &run, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
            DWRITE_MEASURING_MODE_NATURAL, DWRITE_GRID_FIT_MODE_DEFAULT,
            DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0.0f, 0.0f, &analysis);
    } else {
        // Without IDWriteFactory2 only the aliased path produces a 1x1 alpha texture.
        hr = ctx->factory->CreateGlyphRunAnalysis(
            &run, 1.0f, nullptr, DWRITE_RENDERING_MODE_ALIASED,
            DWRITE_MEASURING_MODE_NATURAL, 0.0f, 0.0f, &analysis);
    }
    if (FAILED(hr) || analysis == nullptr) {
        return 1;
    }
    // Aliased 1x1 keeps grayscale coverage and is deterministic across GPUs.
    if (FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &bounds))) {
        analysis->Release();
        return 1;
    }
    width = bounds.right - bounds.left;
    height = bounds.bottom - bounds.top;
    out->pixels = nullptr;
    out->width = width > 0 ? width : 0;
    out->height = height > 0 ? height : 0;
    out->bearing_x = bounds.left;
    out->bearing_y = bounds.top;
    if (width <= 0 || height <= 0) {
        analysis->Release();
        return 0;
    }
    area = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixels == nullptr || capacity <= 0 || area > static_cast<size_t>(capacity) ||
        area > 0xFFFFFFFFu) {
        analysis->Release();
        return 1;
    }
    if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &bounds, pixels,
                                            static_cast<UINT32>(area)))) {
        analysis->Release();
        return 1;
    }
    analysis->Release();
    out->pixels = pixels;
    return 0;
}

static int text_cache_misses(void *context)
{
    WnDirectWrite *ctx = static_cast<WnDirectWrite *>(context);
    return ctx != nullptr ? ctx->misses : -1;
}

static void text_cache_stats(void *context, wn_text_cache_stats *out)
{
    WnDirectWrite *ctx = static_cast<WnDirectWrite *>(context);
    if (out == nullptr) {
        return;
    }
    out->hits = ctx != nullptr ? ctx->hits : 0;
    out->misses = ctx != nullptr ? ctx->misses : 0;
    out->evictions = ctx != nullptr ? ctx->evictions : 0;
}

static int text_has_family(void *context, const char *family)
{
    WnDirectWrite *ctx = static_cast<WnDirectWrite *>(context);
    IDWriteFontCollection *collection = nullptr;
    wchar_t wide[128];
    UINT32 index = 0;
    BOOL exists = FALSE;
    HRESULT hr;

    if (ctx == nullptr || ctx->factory == nullptr || family == nullptr) {
        return 0;
    }
    if (utf8_to_wide(family, wide, static_cast<int>(sizeof(wide) / sizeof(wide[0])),
                     nullptr) == 0) {
        return 0;
    }
    if (FAILED(ctx->factory->GetSystemFontCollection(&collection, FALSE)) || collection == nullptr) {
        return 0;
    }
    hr = collection->FindFamilyName(wide, &index, &exists);
    collection->Release();
    return SUCCEEDED(hr) && exists ? 1 : 0;
}

static const wn_text_ops g_directwrite_ops = {
    "directwrite",
    text_init,
    text_destroy,
    text_measure,
    text_shape,
    text_rasterize,
    text_cache_misses,
    text_has_family,
    text_cache_stats,
};

extern "C" const wn_text_ops *wn_text_directwrite(void)
{
    return &g_directwrite_ops;
}

/* Test hook: the DWRITE_FONT_WEIGHT a CSS `font-weight` maps to. Not part of
 * the public ABI; the weight suite asserts the 100..900 table without
 * duplicating it. */
extern "C" int wn_text_directwrite_weight(int weight)
{
    return static_cast<int>(dwrite_weight(weight));
}
