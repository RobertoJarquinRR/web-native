#include <stdlib.h>
#include <string.h>

#include "text/text_atlas.h"

#define WN_ATLAS_SIDE 1024
#define WN_ATLAS_SLOT_COUNT 8192
#define WN_ATLAS_SLOT_MASK (WN_ATLAS_SLOT_COUNT - 1)
#define WN_ATLAS_RUN_CACHE 64
#define WN_ATLAS_PADDING 1
#define WN_ATLAS_MAX_BITMAP (256 * 256)

typedef struct {
    int used;
    char *family;
    float size;
    int weight;
    int glyph;
    wn_atlas_glyph metrics;
} WnAtlasSlot;

typedef struct {
    int used;
    char *text;
    char *family;
    float size;
    int weight;
    wn_atlas_run view;
} WnAtlasRun;

struct wn_text_atlas {
    unsigned char *pixels;
    int width;
    int height;
    int shelf_x;
    int shelf_y;
    int shelf_height;
    unsigned int serial;
    WnAtlasSlot *slots;
    WnAtlasRun runs[WN_ATLAS_RUN_CACHE];
    int next_run;
};

static unsigned int hash_mix(unsigned int value, unsigned int incoming)
{
    value ^= incoming;
    value *= 16777619u;
    return value;
}

static unsigned int hash_text(unsigned int value, const char *text)
{
    const unsigned char *p = (const unsigned char *)(text != NULL ? text : "");
    while (*p != '\0') {
        value = hash_mix(value, *p++);
    }
    return value;
}

static unsigned int slot_hash(const char *family, float size, int weight, int glyph)
{
    unsigned int value = 2166136261u;
    value = hash_text(value, family);
    value = hash_mix(value, (unsigned int)(size * 64.0f + 0.5f));
    value = hash_mix(value, (unsigned int)weight);
    value = hash_mix(value, (unsigned int)glyph);
    return value;
}

static int same_key(const WnAtlasSlot *slot, const char *family, float size, int weight, int glyph)
{
    return slot->size == size && slot->weight == weight && slot->glyph == glyph &&
           strcmp(slot->family, family) == 0;
}

static WnAtlasSlot *slot_lookup(wn_text_atlas *atlas, const char *family, float size, int weight,
                                int glyph)
{
    unsigned int index = slot_hash(family, size, weight, glyph) & WN_ATLAS_SLOT_MASK;
    for (int probe = 0; probe < WN_ATLAS_SLOT_COUNT; ++probe) {
        WnAtlasSlot *slot = &atlas->slots[index];
        if (!slot->used) {
            return slot;
        }
        if (same_key(slot, family, size, weight, glyph)) {
            return slot;
        }
        index = (index + 1) & WN_ATLAS_SLOT_MASK;
    }
    return NULL;
}

static char *copy_string(const char *text)
{
    size_t length = strlen(text) + 1;
    char *copy = (char *)malloc(length);
    if (copy != NULL) {
        memcpy(copy, text, length);
    }
    return copy;
}

static int pack_bitmap(wn_text_atlas *atlas, int width, int height, int *out_x, int *out_y)
{
    if (width <= 0 || height <= 0) {
        *out_x = 0;
        *out_y = 0;
        return 0;
    }
    if (atlas->shelf_x + width + WN_ATLAS_PADDING > atlas->width) {
        atlas->shelf_x = 0;
        atlas->shelf_y += atlas->shelf_height + WN_ATLAS_PADDING;
        atlas->shelf_height = 0;
    }
    if (atlas->shelf_y + height + WN_ATLAS_PADDING > atlas->height) {
        return 1;
    }
    *out_x = atlas->shelf_x;
    *out_y = atlas->shelf_y;
    atlas->shelf_x += width + WN_ATLAS_PADDING;
    if (height > atlas->shelf_height) {
        atlas->shelf_height = height;
    }
    return 0;
}

wn_text_atlas *wn_text_atlas_create(const unsigned char *seed, int seed_width, int seed_height)
{
    wn_text_atlas *atlas = (wn_text_atlas *)calloc(1, sizeof(*atlas));
    unsigned char *scratch;
    if (atlas == NULL) {
        return NULL;
    }
    atlas->width = WN_ATLAS_SIDE;
    atlas->height = WN_ATLAS_SIDE;
    atlas->pixels = (unsigned char *)calloc((size_t)WN_ATLAS_SIDE * WN_ATLAS_SIDE, 1);
    atlas->slots = (WnAtlasSlot *)calloc(WN_ATLAS_SLOT_COUNT, sizeof(WnAtlasSlot));
    scratch = (unsigned char *)malloc(WN_ATLAS_MAX_BITMAP);
    if (atlas->pixels == NULL || atlas->slots == NULL || scratch == NULL) {
        free(scratch);
        wn_text_atlas_destroy(atlas);
        return NULL;
    }
    free(scratch);

    if (seed != NULL && seed_width > 0 && seed_height > 0 && seed_width <= WN_ATLAS_SIDE &&
        seed_height <= WN_ATLAS_SIDE) {
        for (int row = 0; row < seed_height; ++row) {
            memcpy(atlas->pixels + (size_t)row * atlas->width,
                   seed + (size_t)row * seed_width, (size_t)seed_width);
        }
        atlas->shelf_y = seed_height;
    }
    atlas->serial = 1;
    return atlas;
}

void wn_text_atlas_destroy(wn_text_atlas *atlas)
{
    if (atlas == NULL) {
        return;
    }
    for (int i = 0; i < WN_ATLAS_SLOT_COUNT; ++i) {
        free(atlas->slots != NULL ? atlas->slots[i].family : NULL);
    }
    for (int i = 0; i < WN_ATLAS_RUN_CACHE; ++i) {
        free(atlas->runs[i].text);
        free(atlas->runs[i].family);
        free(atlas->runs[i].view.glyphs);
    }
    free(atlas->slots);
    free(atlas->pixels);
    free(atlas);
}

int wn_text_atlas_is_ready(const wn_text_atlas *atlas)
{
    return atlas != NULL && atlas->pixels != NULL;
}

const unsigned char *wn_text_atlas_pixels(const wn_text_atlas *atlas)
{
    return atlas != NULL ? atlas->pixels : NULL;
}

int wn_text_atlas_width(const wn_text_atlas *atlas)
{
    return atlas != NULL ? atlas->width : 0;
}

int wn_text_atlas_height(const wn_text_atlas *atlas)
{
    return atlas != NULL ? atlas->height : 0;
}

unsigned int wn_text_atlas_serial(const wn_text_atlas *atlas)
{
    return atlas != NULL ? atlas->serial : 0u;
}

static void set_uv(wn_text_atlas *atlas, wn_atlas_glyph *metrics, int x, int y, int width, int height)
{
    metrics->u0 = (float)x / (float)atlas->width;
    metrics->v0 = (float)y / (float)atlas->height;
    metrics->u1 = (float)(x + width) / (float)atlas->width;
    metrics->v1 = (float)(y + height) / (float)atlas->height;
}

const wn_atlas_glyph *wn_text_atlas_glyph(wn_text_atlas *atlas, const char *family, float size,
                                          int weight, int glyph)
{
    WnAtlasSlot *slot;
    wn_text_bitmap bitmap;
    unsigned char scratch[WN_ATLAS_MAX_BITMAP];
    int x;
    int y;

    if (atlas == NULL || atlas->slots == NULL) {
        return NULL;
    }
    family = family != NULL ? family : "Segoe UI";
    slot = slot_lookup(atlas, family, size, weight, glyph);
    if (slot == NULL || (slot->used && same_key(slot, family, size, weight, glyph))) {
        return slot != NULL ? &slot->metrics : NULL;
    }
    memset(&bitmap, 0, sizeof(bitmap));
    if (wn_text_rasterize_glyph(family, size, weight, glyph, scratch, WN_ATLAS_MAX_BITMAP,
                                &bitmap) != 0) {
        return NULL;
    }
    slot->family = copy_string(family);
    if (slot->family == NULL) {
        return NULL;
    }
    slot->size = size;
    slot->weight = weight;
    slot->glyph = glyph;
    slot->used = 1;
    slot->metrics.width = bitmap.width;
    slot->metrics.height = bitmap.height;
    slot->metrics.bearing_x = bitmap.bearing_x;
    slot->metrics.bearing_y = bitmap.bearing_y;
    if (bitmap.width > 0 && bitmap.height > 0) {
        if (pack_bitmap(atlas, bitmap.width, bitmap.height, &x, &y) != 0) {
            return NULL;
        }
        for (int row = 0; row < bitmap.height; ++row) {
            memcpy(atlas->pixels + (size_t)(y + row) * atlas->width + x,
                   bitmap.pixels + (size_t)row * bitmap.width, (size_t)bitmap.width);
        }
        set_uv(atlas, &slot->metrics, x, y, bitmap.width, bitmap.height);
        atlas->serial += 1;
    }
    return &slot->metrics;
}

const wn_atlas_run *wn_text_atlas_shape(wn_text_atlas *atlas, const char *text, const char *family,
                                        float size, int weight)
{
    WnAtlasRun *run = NULL;
    wn_text_shaped shaped;
    int capacity;
    int slot;

    if (atlas == NULL || text == NULL) {
        return NULL;
    }
    family = family != NULL ? family : "Segoe UI";
    for (int i = 0; i < WN_ATLAS_RUN_CACHE; ++i) {
        WnAtlasRun *entry = &atlas->runs[i];
        if (entry->used && entry->size == size && entry->weight == weight &&
            strcmp(entry->text, text) == 0 && strcmp(entry->family, family) == 0) {
            return &entry->view;
        }
    }

    capacity = (int)strlen(text) + 1;
    slot = atlas->next_run;
    atlas->next_run = (atlas->next_run + 1) % WN_ATLAS_RUN_CACHE;
    run = &atlas->runs[slot];
    free(run->view.glyphs);
    free(run->text);
    free(run->family);
    memset(run, 0, sizeof(*run));
    run->view.glyphs = (wn_text_glyph *)calloc((size_t)capacity, sizeof(wn_text_glyph));
    run->text = copy_string(text);
    run->family = copy_string(family);
    if (run->view.glyphs == NULL || run->text == NULL || run->family == NULL) {
        free(run->view.glyphs);
        free(run->text);
        free(run->family);
        memset(run, 0, sizeof(*run));
        return NULL;
    }
    memset(&shaped, 0, sizeof(shaped));
    if (wn_text_shape_string(text, family, size, weight, run->view.glyphs, capacity, &shaped) != 0) {
        return NULL;
    }
    run->used = 1;
    run->size = size;
    run->weight = weight;
    run->view.count = shaped.glyph_count;
    run->view.width = shaped.width;
    run->view.height = shaped.height;
    run->view.ascent = shaped.ascent;
    run->view.descent = shaped.descent;
    return &run->view;
}
