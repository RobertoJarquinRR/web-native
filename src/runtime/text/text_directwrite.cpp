#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwrite.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "wn_text.h"

#define WN_TEXT_CACHE_CAPACITY 128
#define WN_TEXT_MAX_UTF16 4096
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
    IDWriteFactory *factory;
    WnTextEntry entries[WN_TEXT_CACHE_CAPACITY];
    int next_slot;
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
    text_cache_misses,
};

extern "C" const wn_text_ops *wn_text_directwrite(void)
{
    return &g_directwrite_ops;
}
