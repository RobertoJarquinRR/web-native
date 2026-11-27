/*
 * media_blob_test.c - blob v7 media section: round-trip, fields and loader
 * validation.
 *
 * Builds a minimal v7 document in memory (root block + one video node) and
 * patches one field at a time to lock the contract in docs/media-v1.md §2.3:
 * required section, 16-byte records, increasing node order, kind 6, flag bits
 * 4..7 reserved, preload 0..2, reserved zero, local non-empty src, valid poster
 * offset, zero image fields on a video, and exact one-record-per-video
 * association. Every malformed input must return NULL and set wn_last_error().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "document/wn_blob.h"
#include "document/wn_document_internal.h"
#include "webnative.h"

static int checks = 0;
static int failures = 0;

static void check(const char *label, int ok)
{
    checks += 1;
    if (!ok) {
        failures += 1;
    }
    printf("  %s %s\n", ok ? "[ok]" : "[FAIL]", label);
}

#define BLOB_CAP 1024
#define NODES_OFFSET 96u
#define NODES_COUNT 2u
#define NAMES_OFFSET (NODES_OFFSET + NODES_COUNT * (uint32_t)sizeof(WnBlobNode))
#define STRINGS_OFFSET (NAMES_OFFSET + NODES_COUNT * 4u)
#define STRINGS_SIZE 29u
#define MEDIA_OFFSET ((STRINGS_OFFSET + STRINGS_SIZE + 7u) & ~7u)
#define BLOB_SIZE (MEDIA_OFFSET + (uint32_t)sizeof(WnBlobMediaRecord))

#define SRC_CLIP 1u
#define SRC_POSTER 10u
#define SRC_EMPTY 9u
#define SRC_SCHEME 21u

typedef struct {
    unsigned char data[BLOB_CAP];
    WnBlobHeader *header;
    WnBlobSection *sections;
    WnBlobNode *nodes;
    uint32_t *names;
    char *strings;
    WnBlobMediaRecord *media;
} TestBlob;

static void align8(unsigned int *value)
{
    *value = (*value + 7u) & ~7u;
}

static void build(TestBlob *blob)
{
    static const char pool[] = "\0clip.mp4\0poster.png\0bad://x";
    uint32_t sizes[4];
    uint32_t offsets[4];
    uint32_t i;
    memset(blob, 0, sizeof(*blob));

    offsets[0] = NODES_OFFSET;
    sizes[0] = NODES_COUNT * (uint32_t)sizeof(WnBlobNode);
    offsets[1] = NAMES_OFFSET;
    sizes[1] = NODES_COUNT * 4u;
    offsets[2] = STRINGS_OFFSET;
    sizes[2] = STRINGS_SIZE;
    offsets[3] = MEDIA_OFFSET;
    sizes[3] = (uint32_t)sizeof(WnBlobMediaRecord);
    for (i = 0; i < 4; ++i) {
        align8(&offsets[i]);
    }

    blob->header = (WnBlobHeader *)blob->data;
    blob->header->magic = WN_BLOB_MAGIC;
    blob->header->version = WN_BLOB_VERSION;
    blob->header->section_count = 4;
    blob->header->file_size = BLOB_SIZE;
    blob->header->node_count = NODES_COUNT;
    blob->header->viewport_width = 400;
    blob->header->viewport_height = 300;

    blob->sections = (WnBlobSection *)(blob->data + WN_BLOB_HEADER_SIZE);
    blob->sections[0].id = WN_BLOB_SECTION_NODES;
    blob->sections[0].offset = offsets[0];
    blob->sections[0].size = sizes[0];
    blob->sections[1].id = WN_BLOB_SECTION_NAMES;
    blob->sections[1].offset = offsets[1];
    blob->sections[1].size = sizes[1];
    blob->sections[2].id = WN_BLOB_SECTION_STRINGS;
    blob->sections[2].offset = offsets[2];
    blob->sections[2].size = sizes[2];
    blob->sections[3].id = WN_BLOB_SECTION_MEDIA;
    blob->sections[3].offset = offsets[3];
    blob->sections[3].size = sizes[3];

    blob->nodes = (WnBlobNode *)(blob->data + offsets[0]);
    blob->nodes[0].parent = -1;
    blob->nodes[0].kind = 0;
    blob->nodes[0].font_size = 16.0f;
    blob->nodes[0].opacity = 1.0f;
    blob->nodes[1].parent = 0;
    blob->nodes[1].kind = WN_BLOB_NODE_VIDEO;
    blob->nodes[1].font_size = 16.0f;
    blob->nodes[1].opacity = 1.0f;

    blob->names = (uint32_t *)(blob->data + offsets[1]);
    blob->names[0] = 0;
    blob->names[1] = 0;

    blob->strings = (char *)(blob->data + offsets[2]);
    memcpy(blob->strings, pool, sizeof(pool));

    blob->media = (WnBlobMediaRecord *)(blob->data + offsets[3]);
    blob->media->node = 1;
    blob->media->src = SRC_CLIP;
    blob->media->poster = SRC_POSTER;
    blob->media->flags = WN_MEDIA_FLAG_CONTROLS | WN_MEDIA_FLAG_AUTOPLAY |
                         WN_MEDIA_FLAG_LOOP | WN_MEDIA_FLAG_MUTED;
    blob->media->preload = WN_MEDIA_PRELOAD_METADATA;
    blob->media->reserved[0] = 0;
    blob->media->reserved[1] = 0;
}

/* The TestBlob pointers must address the instance's own data array, not the
 * source of a struct assignment. */
static void retarget(TestBlob *blob)
{
    blob->header = (WnBlobHeader *)blob->data;
    blob->sections = (WnBlobSection *)(blob->data + WN_BLOB_HEADER_SIZE);
    blob->nodes = (WnBlobNode *)(blob->data + NODES_OFFSET);
    blob->names = (uint32_t *)(blob->data + NAMES_OFFSET);
    blob->strings = (char *)(blob->data + STRINGS_OFFSET);
    blob->media = (WnBlobMediaRecord *)(blob->data + MEDIA_OFFSET);
}

#define RESET_VARIANT()          \
    do {                         \
        variant = base;          \
        retarget(&variant);      \
    } while (0)

static wn_document *load(const TestBlob *blob)
{
    return wn_document_load_blob_memory(blob->data, (size_t)blob->header->file_size);
}

static void expect_reject(const char *label, const TestBlob *blob)
{
    wn_document *document = load(blob);
    const char *error = wn_last_error();
    check(label, document == NULL && error != NULL);
    wn_document_release(document);
}

int main(void)
{
    TestBlob base;
    TestBlob variant;
    wn_document *document;

    checks = 0;
    failures = 0;
    build(&base);

    /* 1. Valid v7 document: fields survive the loader. */
    document = load(&base);
    check("valid blob v7 loads", document != NULL);
    if (document != NULL) {
        const UiNode *video;
        check("header version is 7", base.header->version == 7u);
        check("document has two nodes", document->node_count == 2);
        video = &document->nodes[1];
        check("node 1 is kind video (6)", video->kind == WN_BLOB_NODE_VIDEO);
        check("media src is parsed",
              video->media_src != NULL && strcmp(video->media_src, "clip.mp4") == 0);
        check("media poster is parsed",
              video->media_poster != NULL &&
                  strcmp(video->media_poster, "poster.png") == 0);
        check("media flags are controls/autoplay/loop/muted",
              video->media_flags == 0x0Fu);
        check("media preload is metadata (1)", video->media_preload == 1u);
        check("video image fields stay zero",
              video->image_src == NULL && video->image_alt == NULL &&
                  video->image_width == 0.0f && video->image_height == 0.0f);
        check("media record is exactly 16 bytes",
              sizeof(WnBlobMediaRecord) == 16u);
        wn_document_release(document);
    }

    /* 2. Missing media section on a video document. */
    RESET_VARIANT();
    variant.header->section_count = 3;
    expect_reject("video without a media section is rejected", &variant);

    /* 3. A record on a non-video node. */
    RESET_VARIANT();
    variant.media->node = 0;
    expect_reject("media record on a non-video node is rejected", &variant);

    /* 4. Record order / duplicates. */
    RESET_VARIANT();
    variant.media->node = 0;
    expect_reject("out-of-range/duplicate node order is rejected", &variant);

    /* 5. Reserved flag bits. */
    RESET_VARIANT();
    variant.media->flags = 0x10u;
    expect_reject("reserved media flag bits are rejected", &variant);

    /* 6. preload range. */
    RESET_VARIANT();
    variant.media->preload = 3u;
    expect_reject("preload above auto (2) is rejected", &variant);

    /* 7. reserved bytes. */
    RESET_VARIANT();
    variant.media->reserved[1] = 1u;
    expect_reject("non-zero media reserved bytes are rejected", &variant);

    /* 8. src: 0 is a source-less host-bound player node; a non-zero offset
     * must be a non-empty local path. */
    RESET_VARIANT();
    variant.media->src = 0u;
    document = load(&variant);
    check("src=0 (host-bound player node) is accepted",
          document != NULL && document->nodes[1].media_src == NULL);
    wn_document_release(document);
    RESET_VARIANT();
    variant.media->src = SRC_EMPTY;
    expect_reject("empty non-zero media src is rejected", &variant);

    /* 9. Local paths only. */
    RESET_VARIANT();
    variant.media->src = SRC_SCHEME;
    expect_reject("media src with a scheme is rejected", &variant);
    RESET_VARIANT();
    variant.media->poster = SRC_SCHEME;
    expect_reject("media poster with a scheme is rejected", &variant);

    /* 10. Poster offset validation (0 is allowed; out of bounds is not). */
    RESET_VARIANT();
    variant.media->poster = STRINGS_SIZE;
    expect_reject("poster offset outside the string pool is rejected", &variant);

    /* 11. Section size must be a whole 16-byte record and non-empty. */
    RESET_VARIANT();
    variant.sections[3].size = 15;
    expect_reject("media section size that is not a whole record is rejected",
                  &variant);
    RESET_VARIANT();
    variant.sections[3].size = 0;
    expect_reject("empty media section is rejected", &variant);

    /* 12. Video nodes may not carry image fields. */
    RESET_VARIANT();
    variant.nodes[1].image_src = SRC_CLIP;
    expect_reject("video node with image_src is rejected", &variant);
    RESET_VARIANT();
    variant.nodes[1].image_width = 10.0f;
    expect_reject("video node with image_width is rejected", &variant);

    /* 13. Every video needs exactly one record: an extra record is rejected. */
    RESET_VARIANT();
    variant.header->section_count = 4;
    variant.sections[3].size = 2 * (uint32_t)sizeof(WnBlobMediaRecord);
    variant.header->file_size = BLOB_SIZE + (uint32_t)sizeof(WnBlobMediaRecord);
    {
        WnBlobMediaRecord *second =
            (WnBlobMediaRecord *)(variant.data + MEDIA_OFFSET +
                                  sizeof(WnBlobMediaRecord));
        memset(second, 0, sizeof(*second));
        second->node = 1;
        second->src = SRC_CLIP;
        second->preload = 1;
    }
    expect_reject("two records for one video are rejected", &variant);

    if (failures != 0) {
        printf("media_blob_test: FAIL (%d checks, %d failed)\n", checks,
               failures);
        return 1;
    }
    printf("media_blob_test: PASS (%d checks)\n", checks);
    return 0;
}
