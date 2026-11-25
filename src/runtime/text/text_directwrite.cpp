#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwrite.h>
#include <dwrite_2.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "wn_text.h"

#define WN_TEXT_CACHE_CAPACITY 128
#define WN_TEXT_FACE_CAPACITY 16
#define WN_TEXT_MAX_UTF16 4096
#define WN_TEXT_MAX_GLYPHS 4096
#define WN_TEXT_UNBOUNDED 1000000.0f

typedef struct {
    int used;
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
    WnFontFaceEntry faces[WN_TEXT_FACE_CAPACITY];
    int next_slot;
    int next_face;
    int misses;
} WnDirectWrite;

static bool same_request(const WnTextEntry *entry, const char *family, float size, int weight,
                         float max_width, const char *text)
{
    return entry->weight == weight && entry->size == size && entry->max_width == max_width &&
           std::strcmp(entry->family, family) == 0 && std::strcmp(entry->text, text) == 0;
}

static int cache_index(WnDirectWrite *ctx, const char *family, float size, int weight,
                       float max_width, const char *text)
{
    for (int slot = 0; slot < WN_TEXT_CACHE_CAPACITY; ++slot) {
        const WnTextEntry *entry = &ctx->entries[slot];
        if (entry->used && same_request(entry, family, size, weight, max_width, text)) {
            return slot;
        }
    }
    return -1;
}

static void entry_clear(WnTextEntry *entry)
{
    std::free(entry->family);
    std::free(entry->text);
    std::memset(entry, 0, sizeof(*entry));
}

static int utf8_to_wide(const char *text, wchar_t *wide, int capacity)
{
    int written = MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, capacity - 1);
    if (written <= 0) {
        wide[0] = L'\0';
        return 0;
    }
    wide[written] = L'\0';
    return written - 1;
}

static int create_text_format(WnDirectWrite *ctx, const wn_text_style *style, IDWriteTextFormat **out)
{
    wchar_t family[128];
    DWRITE_FONT_WEIGHT weight =
        style->weight >= 600 ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    if (utf8_to_wide(style->family != nullptr ? style->family : "Segoe UI", family,
                     static_cast<int>(sizeof(family) / sizeof(family[0]))) == 0) {
        return 1;
    }
    HRESULT hr = ctx->factory->CreateTextFormat(
        family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        style->size > 0.0f ? style->size : 16.0f, L"", out);
    return FAILED(hr) || *out == nullptr ? 1 : 0;
}

static int measure_uncached(WnDirectWrite *ctx, const char *utf8, const wn_text_style *style,
                            float max_width, wn_text_metrics *out)
{
    IDWriteTextFormat *format = nullptr;
    IDWriteTextLayout *layout = nullptr;
    wchar_t text[WN_TEXT_MAX_UTF16];
    DWRITE_TEXT_METRICS metrics{};
    DWRITE_LINE_METRICS line{};
    UINT32 line_count = 0;
    HRESULT hr = S_OK;
    int length = 0;

    if (create_text_format(ctx, style, &format) != 0) {
        return 1;
    }
    length = utf8_to_wide(utf8, text, WN_TEXT_MAX_UTF16);
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
        hr = layout->GetLineMetrics(&line, 1, &line_count);
    }
    layout->Release();
    if (FAILED(hr) || line_count == 0) {
        return 1;
    }
    out->width = metrics.widthIncludingTrailingWhitespace;
    out->height = metrics.height;
    out->ascent = line.baseline;
    out->descent = line.height > line.baseline ? line.height - line.baseline : 0.0f;
    return 0;
}

static DWRITE_FONT_WEIGHT dwrite_weight(int weight)
{
    if (weight >= 900) {
        return DWRITE_FONT_WEIGHT_BLACK;
    }
    if (weight >= 800) {
        return DWRITE_FONT_WEIGHT_EXTRA_BOLD;
    }
    if (weight >= 700) {
        return DWRITE_FONT_WEIGHT_BOLD;
    }
    if (weight >= 600) {
        return DWRITE_FONT_WEIGHT_SEMI_BOLD;
    }
    if (weight >= 500) {
        return DWRITE_FONT_WEIGHT_MEDIUM;
    }
    if (weight >= 350) {
        return DWRITE_FONT_WEIGHT_NORMAL;
    }
    return DWRITE_FONT_WEIGHT_LIGHT;
}

static IDWriteFontFace *find_face(WnDirectWrite *ctx, const wn_text_style *style, float *units_per_em)
{
    const char *family = style->family != nullptr ? style->family : "Segoe UI";
    for (int slot = 0; slot < WN_TEXT_FACE_CAPACITY; ++slot) {
        WnFontFaceEntry *entry = &ctx->faces[slot];
        if (entry->face != nullptr && entry->weight == style->weight &&
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
    if (utf8_to_wide(family, wide, static_cast<int>(sizeof(wide) / sizeof(wide[0]))) == 0) {
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
    if (FAILED(font_family->GetFirstMatchingFont(dwrite_weight(style->weight), DWRITE_FONT_STRETCH_NORMAL,
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

    WnFontFaceEntry *entry = &ctx->faces[ctx->next_face];
    ctx->next_face = (ctx->next_face + 1) % WN_TEXT_FACE_CAPACITY;
    if (entry->face != nullptr) {
        entry->face->Release();
    }
    std::free(entry->family);
    entry->family = static_cast<char *>(std::malloc(std::strlen(family) + 1));
    if (entry->family == nullptr) {
        face->Release();
        return nullptr;
    }
    std::strcpy(entry->family, family);
    entry->weight = style->weight;
    entry->face = face;
    entry->units_per_em = static_cast<float>(metrics.designUnitsPerEm > 0 ? metrics.designUnitsPerEm : 2048);
    *units_per_em = entry->units_per_em;
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
                       float max_width, const char *text, const wn_text_metrics *metrics)
{
    char *family_copy = static_cast<char *>(std::malloc(std::strlen(family) + 1));
    char *text_copy = static_cast<char *>(std::malloc(std::strlen(text) + 1));
    if (family_copy == nullptr || text_copy == nullptr) {
        std::free(family_copy);
        std::free(text_copy);
        return 1;
    }
    std::strcpy(family_copy, family);
    std::strcpy(text_copy, text);

    int slot = ctx->next_slot;
    ctx->next_slot = (ctx->next_slot + 1) % WN_TEXT_CACHE_CAPACITY;
    entry_clear(&ctx->entries[slot]);
    WnTextEntry *entry = &ctx->entries[slot];
    entry->used = 1;
    entry->family = family_copy;
    entry->size = style->size;
    entry->weight = style->weight;
    entry->max_width = max_width;
    entry->text = text_copy;
    entry->metrics = *metrics;
    ctx->misses += 1;
    return 0;
}

static int text_measure(void *context, const char *utf8, const wn_text_style *style,
                        float max_width, wn_text_metrics *out)
{
    WnDirectWrite *ctx = static_cast<WnDirectWrite *>(context);
    const char *family;

    if (ctx == nullptr || utf8 == nullptr || style == nullptr || out == nullptr) {
        return 1;
    }
    family = style->family != nullptr ? style->family : "Segoe UI";
    // Wrap width changes the result, so it is part of the cache key.
    int slot = cache_index(ctx, family, style->size, style->weight, max_width, utf8);
    if (slot >= 0) {
        *out = ctx->entries[slot].metrics;
        return 0;
    }
    if (measure_uncached(ctx, utf8, style, max_width, out) != 0) {
        return 1;
    }
    cache_store(ctx, family, style, max_width, utf8, out);
    return 0;
}

static int text_shape(void *context, const char *utf8, const wn_text_style *style,
                      wn_text_glyph *glyphs, int capacity, wn_text_shaped *out)
{
    WnDirectWrite *ctx = static_cast<WnDirectWrite *>(context);
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

    if (ctx == nullptr || utf8 == nullptr || style == nullptr || out == nullptr) {
        return 1;
    }
    face = find_face(ctx, style, &units_per_em);
    if (face == nullptr) {
        return 1;
    }
    length = utf8_to_wide(utf8, text, WN_TEXT_MAX_UTF16);
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
    out->truncated = length >= WN_TEXT_MAX_UTF16 - 1 ? 1 : 0;

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
    IDWriteFontFace *face;
    IDWriteGlyphRunAnalysis *analysis = nullptr;
    DWRITE_GLYPH_RUN run{};
    RECT bounds{};
    float units_per_em = 0.0f;
    float advance;
    int width;
    int height;
    UINT16 index = static_cast<UINT16>(glyph);
    DWRITE_GLYPH_METRICS glyph_metrics{};
    HRESULT hr = S_OK;

    if (ctx == nullptr || style == nullptr || out == nullptr) {
        return 1;
    }
    face = find_face(ctx, style, &units_per_em);
    if (face == nullptr || FAILED(face->GetDesignGlyphMetrics(&index, 1, &glyph_metrics, FALSE))) {
        return 1;
    }
    advance = static_cast<float>(glyph_metrics.advanceWidth) * style->size / units_per_em;
    run.fontFace = face;
    run.fontEmSize = style->size > 0.0f ? style->size : 16.0f;
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
    if (pixels == nullptr || capacity < width * height) {
        analysis->Release();
        return 1;
    }
    if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &bounds, pixels,
                                            static_cast<UINT32>(width * height)))) {
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

static const wn_text_ops g_directwrite_ops = {
    "directwrite",
    text_init,
    text_destroy,
    text_measure,
    text_shape,
    text_rasterize,
    text_cache_misses,
};

extern "C" const wn_text_ops *wn_text_directwrite(void)
{
    return &g_directwrite_ops;
}
