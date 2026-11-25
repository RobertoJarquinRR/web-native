#include <stdlib.h>
#include <string.h>

#include "text/text_atlas.h"

#define WN_ATLAS_SIDE 1024
#define WN_ATLAS_SLOT_COUNT 8192
#define WN_ATLAS_SLOT_MASK (WN_ATLAS_SLOT_COUNT - 1)
#define WN_ATLAS_RUN_CACHE 1024
#define WN_ATLAS_RUN_BUCKETS 2048
#define WN_ATLAS_RUN_MASK (WN_ATLAS_RUN_BUCKETS - 1)
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
    unsigned int hash;
    int bucket_next;
    int lru_prev;
    int lru_next;
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
    int run_buckets[WN_ATLAS_RUN_BUCKETS];
    int run_head;
    int run_tail;
    int run_used;
    int run_hits;
    int run_misses;
    int run_evictions;
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

static unsigned int run_hash(const char *text, const char *family, float size, int weight)
{
    union {
        float f;
        unsigned int u;
    } bits;
    unsigned int value = 2166136261u;
    value = hash_text(value, text);
    value ^= 0xFFu;
    value *= 16777619u;
    value = hash_text(value, family);
    bits.f = size;
    value = hash_mix(value, bits.u);
    value = hash_mix(value, (unsigned int)weight);
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
    atlas->run_head = -1;
    atlas->run_tail = -1;
    for (int i = 0; i < WN_ATLAS_RUN_BUCKETS; ++i) {
        atlas->run_buckets[i] = -1;
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
    wn_atlas_glyph metrics;
    unsigned char scratch[WN_ATLAS_MAX_BITMAP];
    char *family_copy;
    int x = 0;
    int y = 0;

    if (atlas == NULL || atlas->slots == NULL) {
        return NULL;
    }
    family = family != NULL ? family : "Segoe UI";
    slot = slot_lookup(atlas, family, size, weight, glyph);
    if (slot == NULL) {
        return NULL;
    }
    if (slot->used && same_key(slot, family, size, weight, glyph)) {
        return &slot->metrics;
    }
    memset(&bitmap, 0, sizeof(bitmap));
    if (wn_text_rasterize_glyph(family, size, weight, glyph, scratch, WN_ATLAS_MAX_BITMAP,
                                &bitmap) != 0) {
        return NULL;
    }
    memset(&metrics, 0, sizeof(metrics));
    metrics.width = bitmap.width;
    metrics.height = bitmap.height;
    metrics.bearing_x = bitmap.bearing_x;
    metrics.bearing_y = bitmap.bearing_y;
    family_copy = copy_string(family);
    if (family_copy == NULL) {
        return NULL;
    }
    if (bitmap.width > 0 && bitmap.height > 0) {
        if (pack_bitmap(atlas, bitmap.width, bitmap.height, &x, &y) != 0) {
            /* Do not cache a failed pack: the next request retries instead of
             * returning a degenerate (0,0)-(0,0) quad forever. */
            free(family_copy);
            return NULL;
        }
        for (int row = 0; row < bitmap.height; ++row) {
            memcpy(atlas->pixels + (size_t)(y + row) * atlas->width + x,
                   bitmap.pixels + (size_t)row * bitmap.width, (size_t)bitmap.width);
        }
        set_uv(atlas, &metrics, x, y, bitmap.width, bitmap.height);
        atlas->serial += 1;
    }
    slot->family = family_copy;
    slot->size = size;
    slot->weight = weight;
    slot->glyph = glyph;
    slot->used = 1;
    slot->metrics = metrics;
    return &slot->metrics;
}

static void run_entry_clear(WnAtlasRun *run)
{
    free(run->view.glyphs);
    free(run->text);
    free(run->family);
    memset(run, 0, sizeof(*run));
    run->bucket_next = -1;
    run->lru_prev = -1;
    run->lru_next = -1;
}

static void run_lru_unlink(wn_text_atlas *atlas, int slot)
{
    WnAtlasRun *run = &atlas->runs[slot];
    if (run->lru_prev >= 0) {
        atlas->runs[run->lru_prev].lru_next = run->lru_next;
    } else {
        atlas->run_head = run->lru_next;
    }
    if (run->lru_next >= 0) {
        atlas->runs[run->lru_next].lru_prev = run->lru_prev;
    } else {
        atlas->run_tail = run->lru_prev;
    }
    run->lru_prev = -1;
    run->lru_next = -1;
}

static void run_lru_push(wn_text_atlas *atlas, int slot)
{
    WnAtlasRun *run = &atlas->runs[slot];
    run->lru_prev = -1;
    run->lru_next = atlas->run_head;
    if (atlas->run_head >= 0) {
        atlas->runs[atlas->run_head].lru_prev = slot;
    }
    atlas->run_head = slot;
    if (atlas->run_tail < 0) {
        atlas->run_tail = slot;
    }
}

static void run_bucket_unlink(wn_text_atlas *atlas, int slot)
{
    WnAtlasRun *run = &atlas->runs[slot];
    int bucket = (int)(run->hash & WN_ATLAS_RUN_MASK);
    int index = atlas->run_buckets[bucket];
    int previous = -1;
    while (index >= 0) {
        if (index == slot) {
            if (previous < 0) {
                atlas->run_buckets[bucket] = atlas->runs[index].bucket_next;
            } else {
                atlas->runs[previous].bucket_next = atlas->runs[index].bucket_next;
            }
            break;
        }
        previous = index;
        index = atlas->runs[index].bucket_next;
    }
    run->bucket_next = -1;
}

static void run_bucket_push(wn_text_atlas *atlas, int slot)
{
    WnAtlasRun *run = &atlas->runs[slot];
    int bucket = (int)(run->hash & WN_ATLAS_RUN_MASK);
    run->bucket_next = atlas->run_buckets[bucket];
    atlas->run_buckets[bucket] = slot;
}

const wn_atlas_run *wn_text_atlas_shape(wn_text_atlas *atlas, const char *text, const char *family,
                                        float size, int weight)
{
    wn_text_shaped shaped;
    unsigned int hash;
    int capacity;
    int slot;
    int fresh = 0;

    if (atlas == NULL || text == NULL) {
        return NULL;
    }
    family = family != NULL ? family : "Segoe UI";
    hash = run_hash(text, family, size, weight);
    for (slot = atlas->run_buckets[hash & WN_ATLAS_RUN_MASK]; slot >= 0;
         slot = atlas->runs[slot].bucket_next) {
        WnAtlasRun *entry = &atlas->runs[slot];
        if (entry->used && entry->size == size && entry->weight == weight &&
            strcmp(entry->text, text) == 0 && strcmp(entry->family, family) == 0) {
            run_lru_unlink(atlas, slot);
            run_lru_push(atlas, slot);
            atlas->run_hits += 1;
            return &entry->view;
        }
    }

    atlas->run_misses += 1;
    /* Build into a scratch entry first: a failed insert must not evict a
     * published entry or consume capacity. */
    WnAtlasRun scratch;
    memset(&scratch, 0, sizeof(scratch));
    capacity = (int)strlen(text) + 1;
    scratch.view.glyphs = (wn_text_glyph *)calloc((size_t)capacity, sizeof(wn_text_glyph));
    scratch.text = copy_string(text);
    scratch.family = copy_string(family);
    if (scratch.view.glyphs == NULL || scratch.text == NULL || scratch.family == NULL) {
        run_entry_clear(&scratch);
        return NULL;
    }
    memset(&shaped, 0, sizeof(shaped));
    if (wn_text_shape_string(text, family, size, weight, scratch.view.glyphs, capacity,
                            &shaped) != 0) {
        run_entry_clear(&scratch);
        return NULL;
    }
    scratch.hash = hash;
    scratch.size = size;
    scratch.weight = weight;
    scratch.view.count = shaped.glyph_count;
    scratch.view.width = shaped.width;
    scratch.view.height = shaped.height;
    scratch.view.ascent = shaped.ascent;
    scratch.view.descent = shaped.descent;

    if (atlas->run_used < WN_ATLAS_RUN_CACHE) {
        slot = atlas->run_used;
        fresh = 1;
    } else {
        slot = atlas->run_tail;
        if (slot >= 0) {
            run_lru_unlink(atlas, slot);
            run_bucket_unlink(atlas, slot);
            atlas->run_evictions += 1;
        } else {
            slot = -1;
            for (int i = 0; i < WN_ATLAS_RUN_CACHE; ++i) {
                if (!atlas->runs[i].used) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                run_entry_clear(&scratch);
                return NULL;
            }
        }
    }
    run_entry_clear(&atlas->runs[slot]);
    atlas->runs[slot] = scratch;
    atlas->runs[slot].used = 1;
    if (fresh) {
        atlas->run_used += 1;
    }
    run_bucket_push(atlas, slot);
    run_lru_push(atlas, slot);
    return &atlas->runs[slot].view;
}

void wn_text_atlas_cache_stats(const wn_text_atlas *atlas, wn_text_cache_stats *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (atlas == NULL) {
        return;
    }
    out->hits = atlas->run_hits;
    out->misses = atlas->run_misses;
    out->evictions = atlas->run_evictions;
}
