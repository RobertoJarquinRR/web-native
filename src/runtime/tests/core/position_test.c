/*
 * position_test.c - `position: relative|absolute` + insets in the C core.
 *
 * The blobs are built in memory (no HTML/CSS, no GPU) so every case is exact:
 *
 *   relative   - flow position, then offset by left/top or -right/-bottom,
 *                percentages against the flow containing block; siblings keep
 *                their flow slots.
 *   absolute   - out of flow (siblings advance as if removed), nearest
 *                positioned ancestor, root containing block, stretch, auto
 *                width/height, nested absolute and bottom-only placement.
 *   scroll/hit - absolute nodes scroll with their containing block and are
 *                hittable at the placed rect; overflow clips them.
 *   loader     - the v4 position enum and inset mask are validated.
 *
 *   position_test.exe
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/scroll.h"
#include "core/wn_core_internal.h"
#include "document/wn_blob.h"
#include "document/wn_document_format.h"
#include "wn_core.h"

static int checks = 0;
static int failures = 0;

static int check(const char *label, int ok, const char *detail)
{
    checks += 1;
    if (!ok) {
        failures += 1;
    }
    printf("  %s %s%s%s\n", ok ? "[ok]" : "[FAIL]", label,
           detail != NULL ? " - " : "", detail != NULL ? detail : "");
    return ok;
}

typedef struct {
    int parent;
    int kind;
    float width;
    float height;
    float width_pct;
    float height_pct;
    float padding[4];   /* top,right,bottom,left */
    int position;
    float inset[4];     /* top,right,bottom,left */
    unsigned int inset_mask;
    int overflow;
} NodeSpec;

static uint32_t align_up8(uint32_t value)
{
    return (value + 7u) & ~7u;
}

static const char kStrings[] = "\0";

static unsigned char *build_blob(const NodeSpec *spec, uint32_t count,
                                 uint32_t viewport_w, uint32_t viewport_h,
                                 size_t *out_size)
{
    const uint32_t section_bytes = 3u * (uint32_t)sizeof(WnBlobSection);
    const uint32_t nodes_offset = align_up8(WN_BLOB_HEADER_SIZE + section_bytes);
    const uint32_t nodes_bytes = count * (uint32_t)sizeof(WnBlobNode);
    const uint32_t names_offset = align_up8(nodes_offset + nodes_bytes);
    const uint32_t names_bytes = count * (uint32_t)sizeof(uint32_t);
    const uint32_t strings_offset = align_up8(names_offset + names_bytes);
    const uint32_t strings_bytes = (uint32_t)sizeof(kStrings);
    const uint32_t total = align_up8(strings_offset + strings_bytes);
    unsigned char *data = (unsigned char *)calloc(1, total);
    WnBlobHeader *header;
    WnBlobSection *sections;
    WnBlobNode *nodes;
    uint32_t *name_offsets;
    uint32_t i;

    if (data == NULL) {
        return NULL;
    }
    header = (WnBlobHeader *)data;
    header->magic = WN_BLOB_MAGIC;
    header->version = WN_BLOB_VERSION;
    header->section_count = 3u;
    header->file_size = total;
    header->node_count = count;
    header->viewport_width = viewport_w;
    header->viewport_height = viewport_h;

    sections = (WnBlobSection *)(data + WN_BLOB_HEADER_SIZE);
    sections[0].id = WN_BLOB_SECTION_NODES;
    sections[0].offset = nodes_offset;
    sections[0].size = nodes_bytes;
    sections[1].id = WN_BLOB_SECTION_NAMES;
    sections[1].offset = names_offset;
    sections[1].size = names_bytes;
    sections[2].id = WN_BLOB_SECTION_STRINGS;
    sections[2].offset = strings_offset;
    sections[2].size = strings_bytes;

    nodes = (WnBlobNode *)(data + nodes_offset);
    for (i = 0; i < count; ++i) {
        nodes[i].parent = spec[i].parent;
        nodes[i].kind = spec[i].kind;
        nodes[i].width = spec[i].width;
        nodes[i].height = spec[i].height;
        nodes[i].width_pct = spec[i].width_pct;
        nodes[i].height_pct = spec[i].height_pct;
        memcpy(nodes[i].padding, spec[i].padding, sizeof(nodes[i].padding));
        nodes[i].position = spec[i].position;
        memcpy(nodes[i].inset, spec[i].inset, sizeof(nodes[i].inset));
        nodes[i].inset_mask = spec[i].inset_mask;
        nodes[i].overflow = spec[i].overflow;
        nodes[i].font_size = 16.0f;
        nodes[i].flex_shrink = 1.0f;
        nodes[i].flex_basis = -1.0f;
        nodes[i].opacity = 1.0f;
    }
    name_offsets = (uint32_t *)(data + names_offset);
    memset(name_offsets, 0, names_bytes);
    memcpy(data + strings_offset, kStrings, sizeof(kStrings));
    *out_size = total;
    return data;
}

static void spec_init(NodeSpec *spec, int parent, int kind, float width, float height)
{
    memset(spec, 0, sizeof(*spec));
    spec->parent = parent;
    spec->kind = kind;
    spec->width = width;
    spec->height = height;
    spec->position = R2_POSITION_STATIC;
}

static wn_core *load_core(unsigned char *data, size_t size, wn_document **out_doc)
{
    wn_document *document = wn_document_load_blob_memory(data, size);
    wn_core *core;
    if (document == NULL) {
        return NULL;
    }
    core = wn_core_create(document);
    if (core == NULL) {
        wn_document_destroy(document);
        return NULL;
    }
    wn_core_resize(core, document->viewport_width, document->viewport_height);
    *out_doc = document;
    return core;
}

static int rect_is(const wn_core *core, int node, float x, float y, float w, float h)
{
    const wn_rect *r = &core->rects[node];
    return fabsf(r->x - x) < 0.01f && fabsf(r->y - y) < 0.01f &&
           fabsf(r->w - w) < 0.01f && fabsf(r->h - h) < 0.01f;
}

static void report_rect(const wn_core *core, int node, const char *label)
{
    const wn_rect *r = &core->rects[node];
    printf("  %s: node %d -> (%.1f,%.1f,%.1f,%.1f)\n", label, node,
           r->x, r->y, r->w, r->h);
}

static void run_relative(void)
{
    NodeSpec spec[7];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 100.0f, 40.0f);
    spec_init(&spec[2], 0, R2_NODE_BLOCK, 100.0f, 40.0f);
    spec_init(&spec[3], 0, R2_NODE_BLOCK, 100.0f, 40.0f);
    spec_init(&spec[4], 0, R2_NODE_BLOCK, 100.0f, 40.0f);
    spec_init(&spec[5], 0, R2_NODE_BLOCK, 100.0f, 40.0f);
    spec_init(&spec[6], 0, R2_NODE_BLOCK, 50.0f, 20.0f);

    spec[2].position = R2_POSITION_RELATIVE;
    spec[2].inset[0] = 10.0f;  /* top */
    spec[2].inset[3] = 20.0f;  /* left */
    spec[2].inset_mask = 0b1001u;

    spec[3].position = R2_POSITION_RELATIVE;
    spec[3].inset[1] = 15.0f;  /* right */
    spec[3].inset[2] = 5.0f;   /* bottom */
    spec[3].inset_mask = 0b0110u;

    spec[4].position = R2_POSITION_RELATIVE;
    spec[4].inset[0] = 10.0f;  /* top 10% */
    spec[4].inset[3] = 10.0f;  /* left 10% */
    spec[4].inset_mask = 0x99u; /* both percentage */

    spec[6].position = R2_POSITION_RELATIVE;
    spec[6].inset[0] = 7.0f;   /* top wins over bottom */
    spec[6].inset[1] = 99.0f;  /* right ignored */
    spec[6].inset[2] = 99.0f;  /* bottom ignored */
    spec[6].inset[3] = 12.0f;  /* left wins over right */
    spec[6].inset_mask = 0b1111u;

    data = build_blob(spec, 7u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("relative: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    check("relative: left/top offset the node and its flow slot is unchanged",
          rect_is(core, 1, 0, 0, 100, 40) && rect_is(core, 2, 20, 50, 100, 40),
          "node 2 (20,50) after flow y=40, left 20 top 10");
    check("relative: right/bottom offset with negative sign",
          rect_is(core, 3, -15, 75, 100, 40), "node 3 (-15,75)");
    check("relative: percentage insets resolve against the flow containing block",
          rect_is(core, 4, 40, 150, 100, 40), "node 4 (40,150) from 10% of 400x300");
    check("relative: the next sibling keeps its normal-flow position",
          rect_is(core, 5, 0, 160, 100, 40), "node 5 (0,160)");
    check("relative: left/top win when both sides are declared",
          rect_is(core, 6, 12, 207, 50, 20), "node 6 (12,207)");
    report_rect(core, 2, "relative");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_absolute(void)
{
    NodeSpec spec[9];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 100.0f, 30.0f);
    spec_init(&spec[2], 0, R2_NODE_BLOCK, 80.0f, 40.0f);
    spec_init(&spec[3], 0, R2_NODE_BLOCK, 100.0f, 30.0f);
    spec_init(&spec[4], 0, R2_NODE_BLOCK, 20.0f, 20.0f);
    spec_init(&spec[5], 0, R2_NODE_BLOCK, 100.0f, 30.0f);
    spec_init(&spec[6], 0, R2_NODE_BLOCK, 100.0f, -1.0f);
    spec_init(&spec[7], 6, R2_NODE_BLOCK, 100.0f, 25.0f);
    spec_init(&spec[8], 6, R2_NODE_BLOCK, 50.0f, 50.0f);

    spec[2].position = R2_POSITION_ABSOLUTE;
    spec[2].inset[0] = 60.0f;
    spec[2].inset[3] = 50.0f;
    spec[2].inset_mask = 0b1001u;

    spec[4].position = R2_POSITION_ABSOLUTE;
    spec[4].inset[0] = 10.0f;
    spec[4].inset[3] = 150.0f;
    spec[4].inset_mask = 0b1001u;

    spec[8].position = R2_POSITION_ABSOLUTE;
    spec[8].inset[0] = 0.0f;
    spec[8].inset[3] = 0.0f;
    spec[8].inset_mask = 0b1001u;

    data = build_blob(spec, 9u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("absolute: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    check("absolute: placed against the root containing block",
          rect_is(core, 2, 50, 60, 80, 40) && rect_is(core, 4, 150, 10, 20, 20),
          "node 2 (50,60), node 4 (150,10)");
    check("absolute: removed from flow, siblings keep their slots",
          rect_is(core, 1, 0, 0, 100, 30) && rect_is(core, 3, 0, 30, 100, 30) &&
              rect_is(core, 5, 0, 60, 100, 30),
          "static siblings at y 0,30,60");
    check("absolute: an auto-height parent ignores the absolute child",
          rect_is(core, 6, 0, 90, 100, 25) && rect_is(core, 7, 0, 90, 100, 25),
          "node 6 height 25, node 7 at y=90");
    check("absolute: a static parent is not the containing block",
          rect_is(core, 8, 0, 0, 50, 50), "node 8 uses the root (0,0)");
    report_rect(core, 2, "absolute");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_containing_block(void)
{
    NodeSpec spec[10];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    /* 140x110 content + padding 60/40 = 200x150 border box. */
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 140.0f, 110.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 20.0f, 20.0f);
    spec_init(&spec[3], 1, R2_NODE_BLOCK, 50.0f, 50.0f);
    spec_init(&spec[4], 3, R2_NODE_BLOCK, 10.0f, 10.0f);
    spec_init(&spec[5], 0, R2_NODE_BLOCK, 30.0f, 30.0f);
    spec_init(&spec[6], 1, R2_NODE_BLOCK, 100.0f, 80.0f);
    spec_init(&spec[7], 6, R2_NODE_BLOCK, 10.0f, 10.0f);
    spec_init(&spec[8], 6, R2_NODE_BLOCK, 30.0f, 30.0f);
    spec_init(&spec[9], 8, R2_NODE_BLOCK, 5.0f, 5.0f);

    spec[1].position = R2_POSITION_RELATIVE;
    spec[1].inset[0] = 20.0f;
    spec[1].inset[3] = 30.0f;
    spec[1].inset_mask = 0b1001u;
    spec[1].padding[0] = 10.0f;
    spec[1].padding[1] = 20.0f;
    spec[1].padding[2] = 30.0f;
    spec[1].padding[3] = 40.0f;

    spec[2].position = R2_POSITION_ABSOLUTE;
    spec[2].inset[0] = 6.0f;
    spec[2].inset[3] = 5.0f;
    spec[2].inset_mask = 0b1001u;

    spec[4].position = R2_POSITION_ABSOLUTE;
    spec[4].inset[0] = 8.0f;
    spec[4].inset[3] = 7.0f;
    spec[4].inset_mask = 0b1001u;

    spec[5].position = R2_POSITION_ABSOLUTE;
    spec[5].inset[0] = 100.0f;
    spec[5].inset[3] = 100.0f;
    spec[5].inset_mask = 0b1001u;

    spec[6].position = R2_POSITION_ABSOLUTE;
    spec[6].inset[0] = 50.0f;
    spec[6].inset[3] = 50.0f;
    spec[6].inset_mask = 0b1001u;

    spec[7].position = R2_POSITION_ABSOLUTE;
    spec[7].inset[0] = 6.0f;
    spec[7].inset[3] = 5.0f;
    spec[7].inset_mask = 0b1001u;

    spec[9].position = R2_POSITION_ABSOLUTE;
    spec[9].inset[0] = 2.0f;
    spec[9].inset[3] = 1.0f;
    spec[9].inset_mask = 0b1001u;

    data = build_blob(spec, 10u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("containing block: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    check("containing block: relative ancestor padding box",
          rect_is(core, 1, 30, 20, 200, 150) && rect_is(core, 2, 35, 26, 20, 20),
          "node 1 (30,20,200,150), node 2 (35,26)");
    check("containing block: a static intermediate does not change it",
          rect_is(core, 3, 70, 30, 50, 50) && rect_is(core, 4, 37, 28, 10, 10),
          "node 3 (70,30), node 4 (37,28)");
    check("containing block: root fallback when no ancestor is positioned",
          rect_is(core, 5, 100, 100, 30, 30), "node 5 (100,100)");
    check("containing block: nested absolute uses the absolute parent",
          rect_is(core, 6, 80, 70, 100, 80) && rect_is(core, 7, 85, 76, 10, 10),
          "node 6 (80,70), node 7 (85,76)");
    check("containing block: nested absolute under a static child",
          rect_is(core, 8, 80, 70, 30, 30) && rect_is(core, 9, 81, 72, 5, 5),
          "node 8 (80,70), node 9 (81,72)");
    report_rect(core, 2, "containing-block");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_stretch_auto(void)
{
    NodeSpec spec[10];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, -1.0f, -1.0f);
    spec_init(&spec[2], 0, R2_NODE_BLOCK, -1.0f, -1.0f);
    spec_init(&spec[3], 2, R2_NODE_BLOCK, 70.0f, 25.0f);
    spec_init(&spec[4], 0, R2_NODE_BLOCK, 40.0f, 20.0f);
    spec_init(&spec[5], 0, R2_NODE_BLOCK, 30.0f, 10.0f);
    spec_init(&spec[6], 0, R2_NODE_BLOCK, 25.0f, 15.0f);
    spec_init(&spec[7], 0, R2_NODE_BLOCK, 20.0f, 20.0f);
    spec_init(&spec[8], 0, R2_NODE_BLOCK, 20.0f, 20.0f);

    spec[1].position = R2_POSITION_ABSOLUTE;
    spec[1].inset[0] = 10.0f;
    spec[1].inset[1] = 20.0f;
    spec[1].inset[2] = 20.0f;
    spec[1].inset[3] = 10.0f;
    spec[1].inset_mask = 0b1111u;

    spec[2].position = R2_POSITION_ABSOLUTE;
    spec[2].inset[0] = 200.0f;
    spec[2].inset[3] = 10.0f;
    spec[2].inset_mask = 0b1001u;

    spec[4].position = R2_POSITION_ABSOLUTE;
    spec[4].inset[0] = 30.0f;
    spec[4].inset[1] = 30.0f;
    spec[4].inset_mask = 0b0011u; /* top + right */

    spec[5].position = R2_POSITION_ABSOLUTE;
    spec[5].inset[2] = 40.0f;
    spec[5].inset[3] = 50.0f;
    spec[5].inset_mask = 0b1100u;

    spec[6].position = R2_POSITION_ABSOLUTE;

    spec[7].position = R2_POSITION_ABSOLUTE;
    spec[7].inset[0] = 10.0f;  /* 10% */
    spec[7].inset[3] = 10.0f;
    spec[7].inset_mask = 0x99u;

    spec[8].position = R2_POSITION_ABSOLUTE;
    spec[8].inset[1] = 10.0f;  /* right/bottom 10% */
    spec[8].inset[2] = 10.0f;
    spec[8].inset_mask = 0x66u;

    data = build_blob(spec, 9u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("stretch: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    check("absolute: left+right with auto width stretches",
          rect_is(core, 1, 10, 10, 370, 270),
          "node 1 (10,10,370,270)");
    check("absolute: auto width uses the intrinsic width, auto height the content",
          rect_is(core, 2, 10, 200, 70, 25) && rect_is(core, 3, 10, 200, 70, 25),
          "node 2 (10,200,70,25)");
    check("absolute: right-only placement", rect_is(core, 4, 330, 30, 40, 20),
          "node 4 (330,30)");
    check("absolute: bottom-only placement", rect_is(core, 5, 50, 250, 30, 10),
          "node 5 (50,250)");
    check("absolute: all-auto insets use the content origin",
          rect_is(core, 6, 0, 0, 25, 15), "node 6 (0,0)");
    check("absolute: percentage left/top insets",
          rect_is(core, 7, 40, 30, 20, 20), "node 7 (40,30)");
    check("absolute: percentage right/bottom insets",
          rect_is(core, 8, 340, 250, 20, 20), "node 8 (340,250)");
    report_rect(core, 1, "stretch");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_scroll_hit(void)
{
    NodeSpec spec[7];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;
    wn_rect clip;

    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 200.0f, 100.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 100.0f, 400.0f);
    spec_init(&spec[3], 1, R2_NODE_BLOCK, 40.0f, 30.0f);
    spec_init(&spec[4], 1, R2_NODE_BLOCK, 50.0f, 50.0f);
    spec_init(&spec[5], 0, R2_NODE_BLOCK, 100.0f, 100.0f);
    spec_init(&spec[6], 5, R2_NODE_BLOCK, 50.0f, 50.0f);

    spec[1].position = R2_POSITION_RELATIVE;
    spec[1].overflow = R2_OVERFLOW_SCROLL;

    spec[3].position = R2_POSITION_ABSOLUTE;
    spec[3].inset[0] = 60.0f;
    spec[3].inset[3] = 10.0f;
    spec[3].inset_mask = 0b1001u;

    spec[4].position = R2_POSITION_ABSOLUTE;
    spec[4].inset[0] = 10.0f;
    spec[4].inset[3] = 250.0f;
    spec[4].inset_mask = 0b1001u;

    spec[5].overflow = R2_OVERFLOW_HIDDEN;
    spec[6].position = R2_POSITION_ABSOLUTE;
    spec[6].inset[0] = 10.0f;
    spec[6].inset[3] = 150.0f;
    spec[6].inset_mask = 0b1001u;

    data = build_blob(spec, 7u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("scroll/hit: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    check("absolute: scroll max ignores the out-of-flow children",
          fabsf(wn_core_scroll_max(core, 1) - 300.0f) < 0.01f,
          "scroll_max_y=300 from the 400px static child");
    check("absolute: placed inside the positioned scroll container",
          rect_is(core, 3, 10, 60, 40, 30), "node 3 (10,60)");
    check("absolute: hittable at the placed rect",
          wn_core_node_at(core, 20.0f, 70.0f) == 3, "point (20,70) -> node 3");
    check("absolute: clipped by the scroll container",
          wn_core_node_clip(core, 4, &clip) == 0 &&
              fabsf(clip.x) < 0.01f && fabsf(clip.y) < 0.01f &&
              fabsf(clip.w - 200.0f) < 0.01f &&
              fabsf(clip.h - 100.0f) < 0.01f,
          "node 4 clip is the 200x100 content box");
    check("absolute: a rect outside the clip is not hittable",
          wn_core_node_at(core, 260.0f, 20.0f) != 4,
          "point (260,20) cannot hit node 4");
    check("absolute: escapes a static overflow ancestor that is not its containing block",
          rect_is(core, 6, 150, 10, 50, 50) &&
              wn_core_node_clip(core, 6, &clip) == 0 &&
              fabsf(clip.x) < 0.01f && fabsf(clip.y) < 0.01f &&
              fabsf(clip.w - 400.0f) < 0.01f &&
              fabsf(clip.h - 300.0f) < 0.01f &&
              wn_core_node_at(core, 160.0f, 20.0f) == 6,
          "node 6 uses the root clip and is hittable at (160,20)");

    check("scroll: the absolute child moves with the scrolled content",
          wn_core_scroll_by(core, 1, 0.0f, 50.0f) == 1 &&
              rect_is(core, 3, 10, 10, 40, 30),
          "node 3 (10,10) after scrolling 50");
    check("scroll: hit-test follows the scrolled absolute rect",
          wn_core_node_at(core, 20.0f, 20.0f) == 3,
          "point (20,20) -> node 3");
    report_rect(core, 3, "scroll-hit");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static int load_rejected(unsigned char *data, size_t size, const char *needle)
{
    wn_document *document = wn_document_load_blob_memory(data, size);
    const char *error = wn_last_error();
    int ok = document == NULL && error != NULL &&
             (needle == NULL || strstr(error, needle) != NULL);
    printf("  error: %s\n", error != NULL ? error : "(none)");
    if (document != NULL) {
        wn_document_destroy(document);
    }
    return ok;
}

static void run_loader_validation(void)
{
    NodeSpec spec[2];
    unsigned char *original;
    unsigned char *copy;
    size_t size = 0;
    WnBlobSection *nodes_section;

    spec_init(&spec[0], -1, R2_NODE_BLOCK, 100.0f, 100.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 10.0f, 10.0f);
    original = build_blob(spec, 2u, 100u, 100u, &size);
    if (!check("loader: base blob built", original != NULL, NULL)) {
        return;
    }
    copy = (unsigned char *)malloc(size);
    if (!check("loader: copy allocated", copy != NULL, NULL)) {
        free(original);
        return;
    }
    nodes_section = NULL;
    {
        WnBlobHeader *header = (WnBlobHeader *)original;
        WnBlobSection *sections =
            (WnBlobSection *)(original + WN_BLOB_HEADER_SIZE);
        uint32_t i;
        for (i = 0; i < header->section_count; ++i) {
            if (sections[i].id == WN_BLOB_SECTION_NODES) {
                nodes_section = &sections[i];
                break;
            }
        }
    }

    if (nodes_section == NULL) {
        check("loader: nodes section present", 0, NULL);
        free(copy);
        free(original);
        return;
    }
    memcpy(copy, original, size);
    ((WnBlobNode *)(copy + nodes_section->offset))[0].position = 3;
    check("loader: position enum is validated",
          load_rejected(copy, size, "position"), "position 3 rejected");

    memcpy(copy, original, size);
    ((WnBlobNode *)(copy + nodes_section->offset))[0].position = -1;
    check("loader: negative position is rejected",
          load_rejected(copy, size, "position"), "position -1 rejected");

    memcpy(copy, original, size);
    ((WnBlobNode *)(copy + nodes_section->offset))[0].inset_mask = 0x100u;
    check("loader: inset mask reserved bits are rejected",
          load_rejected(copy, size, "inset_mask"), "mask 0x100 rejected");

    memcpy(copy, original, size);
    ((WnBlobNode *)(copy + nodes_section->offset))[0].inset_mask = 0x10u;
    check("loader: a percentage bit without a declared side is rejected",
          load_rejected(copy, size, "inset_mask"), "mask 0x10 rejected");

    memcpy(copy, original, size);
    {
        WnBlobNode *first = (WnBlobNode *)(copy + nodes_section->offset);
        first->inset_mask = 0x1u;
        first->inset[0] = strtof("nan", NULL);
    }
    check("loader: a non-finite inset is rejected",
          load_rejected(copy, size, "non-finite inset"), "NaN inset rejected");

    memcpy(copy, original, size);
    {
        wn_document *document = wn_document_load_blob_memory(copy, size);
        check("loader: a valid v4 position/inset record loads",
              document != NULL && document->nodes[0].position == R2_POSITION_STATIC &&
                  document->nodes[0].inset_mask == 0u,
              document != NULL ? NULL : wn_last_error());
        if (document != NULL) {
            wn_document_destroy(document);
        }
    }

    memcpy(copy, original, size);
    {
        WnBlobNode *first = (WnBlobNode *)(copy + nodes_section->offset);
        wn_document *document;
        first->position = R2_POSITION_ABSOLUTE;
        first->inset[0] = 12.0f;
        first->inset[3] = 25.0f;
        first->inset_mask = 0x99u; /* top+left, both percentages */
        document = wn_document_load_blob_memory(copy, size);
        check("loader: position/inset/mask survive the v4 round trip",
              document != NULL &&
                  document->nodes[0].position == R2_POSITION_ABSOLUTE &&
                  document->nodes[0].inset[0] == 12.0f &&
                  document->nodes[0].inset[3] == 25.0f &&
                  document->nodes[0].inset_mask == 0x99u,
              document != NULL ? NULL : wn_last_error());
        if (document != NULL) {
            wn_document_destroy(document);
        }
    }

    free(copy);
    free(original);
}

int main(void)
{
    run_relative();
    run_absolute();
    run_containing_block();
    run_stretch_auto();
    run_scroll_hit();
    run_loader_validation();

    if (failures != 0) {
        printf("position_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }
    printf("position_test: PASS (%d checks)\n", checks);
    return 0;
}
