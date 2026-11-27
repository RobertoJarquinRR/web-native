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
 *   fixed      - out of flow, always the viewport containing block, ignores
 *                ancestor scroll and clips, nested fixed/absolute, and excluded
 *                from flex/grid flow and intrinsic width.
 *   sticky     - stays in flow, pins to the nearest scroll container's
 *                scrollport by top/bottom inset, clamped by its parent block;
 *                top wins over bottom, the scrollport is the content box, only
 *                `overflow: scroll` counts, and a fixed child stays anchored.
 *   scroll/hit - absolute nodes scroll with their containing block and are
 *                hittable at the placed rect; overflow clips them.
 *   loader     - the v4 position enum (through sticky) and inset mask are
 *                validated.
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
    /* Flex/grid knobs for the out-of-flow exclusion cases. */
    int direction;      /* 0 row, 1 column */
    float gap;
    int grid_mode;
    float grid_column_px;
    float grid_auto_rows;
    float row_gap;
    float column_gap;
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
        nodes[i].direction = spec[i].direction;
        nodes[i].gap = spec[i].gap;
        nodes[i].grid_mode = spec[i].grid_mode;
        nodes[i].grid_column_px = spec[i].grid_column_px;
        nodes[i].grid_auto_rows = spec[i].grid_auto_rows;
        nodes[i].row_gap = spec[i].row_gap;
        nodes[i].column_gap = spec[i].column_gap;
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
    spec->grid_auto_rows = -1.0f;
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

    check("scroll: the tall static child sets the scroll max",
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

static void run_relative_subtree(void)
{
    NodeSpec spec[5];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 100.0f, 50.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 30.0f, 10.0f);
    spec_init(&spec[3], 0, R2_NODE_BLOCK, 100.0f, 10.0f);
    spec_init(&spec[4], 1, R2_NODE_BLOCK, 20.0f, 20.0f);

    spec[1].position = R2_POSITION_RELATIVE;
    spec[1].inset[0] = 25.0f;
    spec[1].inset[3] = 15.0f;
    spec[1].inset_mask = 0b1001u;

    spec[4].position = R2_POSITION_ABSOLUTE;
    spec[4].inset[0] = 5.0f;
    spec[4].inset[3] = 5.0f;
    spec[4].inset_mask = 0b1001u;

    data = build_blob(spec, 5u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("relative subtree: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    check("relative: shifts its whole in-flow subtree",
          rect_is(core, 1, 15, 25, 100, 50) && rect_is(core, 2, 15, 25, 30, 10),
          "node 1 (15,25) and node 2 (15,25)");
    check("relative: the next flow sibling keeps its slot",
          rect_is(core, 3, 0, 50, 100, 10), "node 3 (0,50)");
    check("relative: an absolute descendant resolves against the moved ancestor",
          rect_is(core, 4, 20, 30, 20, 20), "node 4 (20,30)");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_relative_scroll(void)
{
    NodeSpec spec[4];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 200.0f, 100.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 30.0f, 20.0f);
    spec_init(&spec[3], 1, R2_NODE_BLOCK, 100.0f, 400.0f);

    spec[1].position = R2_POSITION_RELATIVE;
    spec[1].overflow = R2_OVERFLOW_SCROLL;

    spec[2].position = R2_POSITION_RELATIVE;
    spec[2].inset[0] = 20.0f;
    spec[2].inset[3] = 110.0f; /* clear of the 100px static sibling below it */
    spec[2].inset_mask = 0b1001u;

    data = build_blob(spec, 4u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("relative scroll: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    check("relative: scroll max counts only the in-flow children",
          fabsf(wn_core_scroll_max(core, 1) - 320.0f) < 0.01f,
          "scroll_max_y=320 from 20+400 content in a 100px viewport");
    check("relative: placed inside the scrolled content",
          rect_is(core, 2, 110, 20, 30, 20), "node 2 (110,20)");
    check("relative: hittable at the placed rect",
          wn_core_node_at(core, 115.0f, 25.0f) == 2, "point (115,25) -> node 2");
    check("relative: moves with the scroll offset",
          wn_core_scroll_by(core, 1, 0.0f, 10.0f) == 1 &&
              rect_is(core, 2, 110, 10, 30, 20),
          "node 2 (110,10) after scrolling 10");
    check("relative: hit-test follows the scrolled rect",
          wn_core_node_at(core, 115.0f, 15.0f) == 2,
          "point (115,15) -> node 2");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_flow_removal(void)
{
    NodeSpec spec[12];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    /* Flex row: the 150x80 absolute child must not widen or heighten the line. */
    spec_init(&spec[1], 0, R2_NODE_FLEX, 200.0f, -1.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 40.0f, 20.0f);
    spec_init(&spec[3], 1, R2_NODE_BLOCK, 150.0f, 80.0f);
    spec_init(&spec[4], 1, R2_NODE_BLOCK, 40.0f, 20.0f);
    /* Grid: the absolute child must not add a row nor set the row height. */
    spec_init(&spec[5], 0, R2_NODE_GRID, 200.0f, -1.0f);
    spec_init(&spec[6], 5, R2_NODE_BLOCK, 20.0f, 20.0f);
    spec_init(&spec[7], 5, R2_NODE_BLOCK, 150.0f, 90.0f);
    spec_init(&spec[8], 5, R2_NODE_BLOCK, 20.0f, 20.0f);
    /* Intrinsic width: an absolute auto-width box ignores its absolute child. */
    spec_init(&spec[9], 0, R2_NODE_BLOCK, -1.0f, -1.0f);
    spec_init(&spec[10], 9, R2_NODE_BLOCK, 20.0f, 10.0f);
    spec_init(&spec[11], 9, R2_NODE_BLOCK, 120.0f, 10.0f);

    spec[1].direction = 0;
    spec[5].grid_mode = R2_GRID_AUTO_FILL;
    spec[5].grid_column_px = 50.0f;

    spec[3].position = R2_POSITION_ABSOLUTE;
    spec[7].position = R2_POSITION_ABSOLUTE;
    spec[9].position = R2_POSITION_ABSOLUTE;
    spec[9].inset[0] = 200.0f;
    spec[9].inset[3] = 250.0f;
    spec[9].inset_mask = 0b1001u;
    spec[11].position = R2_POSITION_ABSOLUTE;

    data = build_blob(spec, 12u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("flow removal: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    check("absolute: removed from flex flow (size and line height)",
          rect_is(core, 1, 0, 0, 200, 20) && rect_is(core, 2, 0, 0, 40, 20) &&
              rect_is(core, 4, 40, 0, 40, 20) && rect_is(core, 3, 0, 0, 150, 80),
          "flex height 20, second static at x=40, absolute at (0,0)");
    check("absolute: removed from grid flow (no extra row, row height)",
          rect_is(core, 5, 0, 20, 200, 20) && rect_is(core, 6, 0, 20, 20, 20) &&
              rect_is(core, 8, 50, 20, 20, 20),
          "grid height 20, second static at (50,20)");
    check("absolute: excluded from the parent intrinsic width",
          rect_is(core, 9, 250, 200, 20, 10) && rect_is(core, 11, 250, 200, 120, 10),
          "node 9 auto width 20, its absolute child 120 wide");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_fixed_flow_removal(void)
{
    NodeSpec spec[12];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    /* Flex row: the 150x80 fixed child must not take a slot, widen the line
     * nor set its height. */
    spec_init(&spec[1], 0, R2_NODE_FLEX, 200.0f, -1.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 40.0f, 20.0f);
    spec_init(&spec[3], 1, R2_NODE_BLOCK, 150.0f, 80.0f);
    spec_init(&spec[4], 1, R2_NODE_BLOCK, 40.0f, 20.0f);
    /* Grid: the fixed child must not add a row nor set the row height. */
    spec_init(&spec[5], 0, R2_NODE_GRID, 200.0f, -1.0f);
    spec_init(&spec[6], 5, R2_NODE_BLOCK, 20.0f, 20.0f);
    spec_init(&spec[7], 5, R2_NODE_BLOCK, 150.0f, 90.0f);
    spec_init(&spec[8], 5, R2_NODE_BLOCK, 20.0f, 20.0f);
    /* Intrinsic width: a shrink-to-fit box ignores its fixed child. */
    spec_init(&spec[9], 0, R2_NODE_BLOCK, -1.0f, -1.0f);
    spec_init(&spec[10], 9, R2_NODE_BLOCK, 20.0f, 10.0f);
    spec_init(&spec[11], 9, R2_NODE_BLOCK, 120.0f, 10.0f);

    spec[1].direction = 0;
    spec[5].grid_mode = R2_GRID_AUTO_FILL;
    spec[5].grid_column_px = 50.0f;

    spec[3].position = R2_POSITION_FIXED;
    spec[7].position = R2_POSITION_FIXED;
    spec[9].position = R2_POSITION_ABSOLUTE;
    spec[9].inset[0] = 200.0f;
    spec[9].inset[3] = 250.0f;
    spec[9].inset_mask = 0b1001u;
    spec[11].position = R2_POSITION_FIXED;

    data = build_blob(spec, 12u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("fixed flow removal: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    check("fixed: removed from flex flow (slot and line height)",
          rect_is(core, 1, 0, 0, 200, 20) && rect_is(core, 2, 0, 0, 40, 20) &&
              rect_is(core, 4, 40, 0, 40, 20) && rect_is(core, 3, 0, 0, 150, 80),
          "flex height 20, second static at x=40, fixed at (0,0)");
    check("fixed: removed from grid flow (no extra cell nor row height)",
          rect_is(core, 5, 0, 20, 200, 20) && rect_is(core, 6, 0, 20, 20, 20) &&
              rect_is(core, 8, 50, 20, 20, 20) && rect_is(core, 7, 0, 0, 150, 90),
          "grid height 20, second static at (50,20), fixed at (0,0)");
    check("fixed: excluded from the parent intrinsic width",
          rect_is(core, 9, 250, 200, 20, 10) && rect_is(core, 11, 0, 0, 120, 10),
          "node 9 auto width 20, its fixed child 120 wide");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_scroll_max_exclusion(void)
{
    NodeSpec spec[4];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 200.0f, 100.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 100.0f, 60.0f);
    spec_init(&spec[3], 1, R2_NODE_BLOCK, 50.0f, 50.0f);

    spec[1].position = R2_POSITION_RELATIVE;
    spec[1].overflow = R2_OVERFLOW_SCROLL;

    spec[3].position = R2_POSITION_ABSOLUTE;
    spec[3].inset[0] = 400.0f;
    spec[3].inset[3] = 10.0f;
    spec[3].inset_mask = 0b1001u;

    data = build_blob(spec, 4u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("scroll max: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    /* Only the absolute node could overflow here: 60px of static content in a
     * 100px viewport must leave max_y at 0. */
    check("absolute: out-of-flow children add no scrollable overflow",
          fabsf(wn_core_scroll_max(core, 1)) < 0.01f &&
              wn_core_scroll_by(core, 1, 0.0f, 50.0f) == 0,
          "scroll_max_y=0 with a 400px absolute child");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_clipped_ancestor_escape(void)
{
    NodeSpec spec[5];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;
    wn_rect clipped;
    wn_rect escaped;
    int item;
    int painted = 0;

    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 100.0f, 100.0f); /* positioned parent */
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 100.0f, 0.0f);   /* fully clipped */
    spec_init(&spec[3], 2, R2_NODE_BLOCK, 20.0f, 20.0f);   /* static intermediary */
    spec_init(&spec[4], 3, R2_NODE_BLOCK, 30.0f, 30.0f);   /* absolute escapee */

    spec[1].position = R2_POSITION_RELATIVE;
    spec[2].overflow = R2_OVERFLOW_HIDDEN;
    spec[4].position = R2_POSITION_ABSOLUTE;
    spec[4].inset[0] = 5.0f;
    spec[4].inset[3] = 5.0f;
    spec[4].inset_mask = 0b1001u;

    data = build_blob(spec, 5u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("clipped ancestor: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    for (item = 0; item < core->paint_item_count; ++item) {
        if (core->paint_items[item].node == 4) {
            painted = 1;
            break;
        }
    }
    check("absolute: kept in the paint list below a fully clipped static ancestor",
          wn_core_node_clip(core, 3, &clipped) == 0 && rect_is_empty(clipped) &&
              wn_core_node_clip(core, 4, &escaped) == 0 &&
              !rect_is_empty(escaped) && painted,
          "node 3 clip empty, node 4 clip non-empty and painted");
    check("absolute: hittable at its placed rect after escaping the clip",
          rect_is(core, 4, 5, 5, 30, 30) && wn_core_node_at(core, 10.0f, 10.0f) == 4,
          "node 4 (5,5) and point (10,10) -> node 4");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_viewport_containing_block(void)
{
    NodeSpec spec[2];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    /* Root smaller than the viewport: the initial containing block is still the
     * 400x300 viewport, not the 200x100 root box. */
    spec_init(&spec[0], -1, R2_NODE_BLOCK, 200.0f, 100.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 20.0f, 20.0f);

    spec[1].position = R2_POSITION_ABSOLUTE;
    spec[1].inset[1] = 0.0f;   /* right */
    spec[1].inset[2] = 0.0f;   /* bottom */
    spec[1].inset_mask = 0b0110u;

    data = build_blob(spec, 2u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("viewport cb: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    check("absolute: a static root falls back to the viewport, not its own box",
          rect_is(core, 1, 380, 280, 20, 20), "node 1 (380,280) in a 400x300 viewport");

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

static void run_fixed_positioning(void)
{
    NodeSpec spec[5];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    /* Root smaller than the viewport: fixed always uses the 400x300 viewport,
     * not the root box. */
    spec_init(&spec[0], -1, R2_NODE_BLOCK, 200.0f, 100.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, -1.0f, -1.0f);
    spec_init(&spec[2], 0, R2_NODE_BLOCK, 20.0f, 10.0f);
    spec_init(&spec[3], 0, R2_NODE_BLOCK, 40.0f, 20.0f);
    spec_init(&spec[4], 0, R2_NODE_BLOCK, 100.0f, 20.0f);

    spec[1].position = R2_POSITION_FIXED;
    spec[1].inset[0] = 10.0f;  /* top */
    spec[1].inset[1] = 10.0f;  /* right */
    spec[1].inset[2] = 10.0f;  /* bottom */
    spec[1].inset[3] = 10.0f;  /* left */
    spec[1].inset_mask = 0b1111u;

    spec[2].position = R2_POSITION_FIXED;
    spec[2].inset[0] = 50.0f;  /* top 50% */
    spec[2].inset[3] = 25.0f;  /* left 25% */
    spec[2].inset_mask = 0x99u;

    spec[3].position = R2_POSITION_FIXED;
    spec[3].inset[1] = 0.0f;   /* right */
    spec[3].inset[2] = 0.0f;   /* bottom */
    spec[3].inset_mask = 0b0110u;

    data = build_blob(spec, 5u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("fixed: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    check("fixed: left+right/top+bottom stretch against the viewport",
          rect_is(core, 1, 10, 10, 380, 280), "node 1 (10,10,380,280)");
    check("fixed: percentage insets resolve against the viewport",
          rect_is(core, 2, 100, 150, 20, 10), "node 2 (100,150)");
    check("fixed: right/bottom anchor to the viewport corner",
          rect_is(core, 3, 360, 280, 40, 20), "node 3 (360,280)");
    check("fixed: an in-flow sibling keeps the root flow",
          rect_is(core, 4, 0, 0, 100, 20), "node 4 at the root origin");
    check("fixed: the topmost viewport box wins the hit",
          wn_core_node_at(core, 380.0f, 290.0f) == 3, "node_at(380,290)");
    check("fixed: the stretch box is hittable away from the corner",
          wn_core_node_at(core, 50.0f, 50.0f) == 1, "node_at(50,50)");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_fixed_scroll_independence(void)
{
    NodeSpec spec[6];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    /* A scroller with tall content and two fixed badges inside it. Fixed
     * ignores the scroller: no clip and no scroll translation. */
    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 200.0f, 100.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 200.0f, 400.0f);
    spec_init(&spec[3], 1, R2_NODE_BLOCK, 20.0f, 20.0f);
    spec_init(&spec[4], 1, R2_NODE_BLOCK, 20.0f, 20.0f);
    spec_init(&spec[5], 3, R2_NODE_BLOCK, 10.0f, 10.0f);
    spec[1].overflow = R2_OVERFLOW_SCROLL;

    spec[3].position = R2_POSITION_FIXED;
    spec[3].inset[0] = 10.0f;
    spec[3].inset[3] = 10.0f;
    spec[3].inset_mask = 0b1001u;

    spec[4].position = R2_POSITION_FIXED;
    spec[4].inset[1] = 0.0f;
    spec[4].inset[2] = 0.0f;
    spec[4].inset_mask = 0b0110u;

    /* Nested fixed: still viewport-anchored, not relative to node 3. */
    spec[5].position = R2_POSITION_FIXED;
    spec[5].inset[1] = 5.0f;
    spec[5].inset[2] = 5.0f;
    spec[5].inset_mask = 0b0110u;

    data = build_blob(spec, 6u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("fixed scroll: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }
    check("fixed scroll: badge starts at the viewport inset",
          rect_is(core, 3, 10, 10, 20, 20), "node 3 (10,10)");
    check("fixed scroll: nested badge is viewport-anchored",
          rect_is(core, 5, 385, 285, 10, 10), "node 5 (385,285)");
    check("fixed scroll: the scroller clips its flow content",
          core->clips[2].y == 0.0f && core->clips[2].h == 100.0f,
          "node 2 clip starts at the scrollport");
    check("fixed scroll: the fixed badge uses the viewport clip",
          core->clips[3].x == 0.0f && core->clips[3].y == 0.0f &&
              core->clips[3].w == 400.0f && core->clips[3].h == 300.0f,
          "node 3 clip is the viewport");

    check("fixed scroll: scroll_by(100) succeeds",
          wn_core_scroll_by(core, 1, 0.0f, 100.0f) > 0, "scroll y=100");
    check("fixed scroll: the flow content scrolls",
          rect_is(core, 2, 0, -100, 200, 400), "node 2 y=-100");
    check("fixed scroll: the fixed badge does not move",
          rect_is(core, 3, 10, 10, 20, 20), "node 3 stays (10,10)");
    check("fixed scroll: the corner badge does not move",
          rect_is(core, 4, 380, 280, 20, 20), "node 4 stays (380,280)");
    check("fixed scroll: the nested badge does not move",
          rect_is(core, 5, 385, 285, 10, 10), "node 5 stays (385,285)");
    check("fixed scroll: the badge is hittable over scrolled content",
          wn_core_node_at(core, 15.0f, 15.0f) == 3, "node_at(15,15)");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_fixed_nested_absolute(void)
{
    NodeSpec spec[3];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    /* A fixed node is the containing block of its absolute descendants, even
     * though the absolute pass runs before the fixed pass. */
    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 100.0f, 80.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 20.0f, 15.0f);
    spec[1].position = R2_POSITION_FIXED;
    spec[1].inset[0] = 20.0f;
    spec[1].inset[3] = 30.0f;
    spec[1].inset_mask = 0b1001u;
    spec[2].position = R2_POSITION_ABSOLUTE;
    spec[2].inset[0] = 5.0f;
    spec[2].inset[3] = 6.0f;
    spec[2].inset_mask = 0b1001u;

    data = build_blob(spec, 3u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("fixed nested absolute: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }
    check("fixed nested absolute: the fixed box resolves against the viewport",
          rect_is(core, 1, 30, 20, 100, 80), "node 1 (30,20)");
    check("fixed nested absolute: the absolute child uses the fixed box",
          rect_is(core, 2, 36, 25, 20, 15), "node 2 (36,25)");
    check("fixed nested absolute: the child wins the hit",
          wn_core_node_at(core, 40.0f, 30.0f) == 2, "node_at(40,30)");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_sticky_top(void)
{
    NodeSpec spec[5];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 200.0f, 100.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 200.0f, 300.0f);
    spec_init(&spec[3], 2, R2_NODE_BLOCK, 200.0f, 20.0f);
    spec_init(&spec[4], 2, R2_NODE_BLOCK, 200.0f, 280.0f);
    spec[1].overflow = R2_OVERFLOW_SCROLL;
    spec[3].position = R2_POSITION_STICKY;
    spec[3].inset[0] = 10.0f;
    spec[3].inset_mask = 0b0001u;

    data = build_blob(spec, 5u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("sticky top: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }
    check("sticky top: stays in flow at the threshold",
          rect_is(core, 3, 0, 10, 200, 20), "node 3 pinned at y=10");
    check("sticky top: the sibling content keeps its flow slot",
          rect_is(core, 4, 0, 20, 200, 280), "node 4 y=20");

    check("sticky top: scroll_by(50) succeeds",
          wn_core_scroll_by(core, 1, 0.0f, 50.0f) > 0, "scroll y=50");
    check("sticky top: pinned to scrollport top + inset",
          rect_is(core, 3, 0, 10, 200, 20), "node 3 stays y=10");
    check("sticky top: flow content scrolled under it",
          rect_is(core, 4, 0, -30, 200, 280), "node 4 y=-30");
    check("sticky top: hit-test at the pinned rect",
          wn_core_node_at(core, 50.0f, 15.0f) == 3, "node_at(50,15)");

    check("sticky top: scroll_by(150) to the end",
          wn_core_scroll_by(core, 1, 0.0f, 150.0f) > 0, "scroll y=200");
    check("sticky top: still pinned at max scroll",
          rect_is(core, 3, 0, 10, 200, 20), "node 3 stays y=10");
    check("sticky top: flow content reached the end",
          rect_is(core, 4, 0, -180, 200, 280), "node 4 y=-180");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_sticky_clamp(void)
{
    NodeSpec spec[5];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    /* Short containing block: once its bottom reaches the scrollport, the
     * sticky box is clamped and scrolls away with the parent. */
    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 200.0f, 100.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 200.0f, 40.0f);
    spec_init(&spec[3], 2, R2_NODE_BLOCK, 200.0f, 20.0f);
    spec_init(&spec[4], 1, R2_NODE_BLOCK, 200.0f, 300.0f);
    spec[1].overflow = R2_OVERFLOW_SCROLL;
    spec[3].position = R2_POSITION_STICKY;
    spec[3].inset[0] = 10.0f;
    spec[3].inset_mask = 0b0001u;

    data = build_blob(spec, 5u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("sticky clamp: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }
    check("sticky clamp: pinned at scroll 0",
          rect_is(core, 3, 0, 10, 200, 20), "node 3 y=10");
    wn_core_scroll_by(core, 1, 0.0f, 10.0f);
    check("sticky clamp: still pinned while the block fits",
          rect_is(core, 3, 0, 10, 200, 20), "node 3 y=10");
    wn_core_scroll_by(core, 1, 0.0f, 10.0f);
    check("sticky clamp: bottom edge reaches the block bottom",
          rect_is(core, 3, 0, 0, 200, 20), "node 3 y=0 at scroll 20");
    wn_core_scroll_by(core, 1, 0.0f, 10.0f);
    check("sticky clamp: clamped box scrolls away with its parent",
          rect_is(core, 3, 0, -10, 200, 20), "node 3 y=-10 at scroll 30");
    check("sticky clamp: the tall sibling still drives the scroll range",
          rect_is(core, 4, 0, 10, 200, 300), "node 4 y=10 at scroll 30");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_sticky_bottom(void)
{
    NodeSpec spec[6];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 200.0f, 100.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 200.0f, 220.0f);
    spec_init(&spec[3], 2, R2_NODE_BLOCK, 200.0f, 150.0f);
    spec_init(&spec[4], 2, R2_NODE_BLOCK, 200.0f, 20.0f);
    spec_init(&spec[5], 2, R2_NODE_BLOCK, 200.0f, 50.0f);
    spec[1].overflow = R2_OVERFLOW_SCROLL;
    spec[4].position = R2_POSITION_STICKY;
    spec[4].inset[2] = 10.0f; /* bottom */
    spec[4].inset_mask = 0b0100u;

    data = build_blob(spec, 6u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("sticky bottom: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }
    check("sticky bottom: pulled up to the bottom inset",
          rect_is(core, 4, 0, 70, 200, 20), "node 4 stays (0,70)");
    wn_core_scroll_by(core, 1, 0.0f, 50.0f);
    check("sticky bottom: pinned while flow is below the threshold",
          rect_is(core, 4, 0, 70, 200, 20), "node 4 y=70 at scroll 50");
    wn_core_scroll_by(core, 1, 0.0f, 50.0f);
    check("sticky bottom: returns to flow once it clears the threshold",
          rect_is(core, 4, 0, 50, 200, 20), "node 4 y=50 at scroll 100");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_sticky_without_scroller(void)
{
    NodeSpec spec[2];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 100.0f, 20.0f);
    spec[1].position = R2_POSITION_STICKY;
    spec[1].inset[0] = 10.0f;
    spec[1].inset_mask = 0b0001u;

    data = build_blob(spec, 2u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("sticky no scroller: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }
    check("sticky no scroller: behaves as in-flow content",
          rect_is(core, 1, 0, 0, 100, 20), "node 1 (0,0)");
    check("sticky no scroller: still hittable",
          wn_core_node_at(core, 50.0f, 10.0f) == 1, "node_at(50,10)");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_sticky_fixed_child(void)
{
    NodeSpec spec[6];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    /* The sticky header shifts its flow subtree, but a fixed child ignores
     * every ancestor offset: it stays anchored to the viewport. */
    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 200.0f, 100.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 200.0f, 300.0f);
    spec_init(&spec[3], 2, R2_NODE_BLOCK, 200.0f, 20.0f);
    spec_init(&spec[4], 3, R2_NODE_BLOCK, 20.0f, 20.0f);
    spec_init(&spec[5], 2, R2_NODE_BLOCK, 200.0f, 280.0f);
    spec[1].overflow = R2_OVERFLOW_SCROLL;
    spec[3].position = R2_POSITION_STICKY;
    spec[3].inset[0] = 10.0f;
    spec[3].inset_mask = 0b0001u;
    spec[4].position = R2_POSITION_FIXED;
    spec[4].inset[0] = 5.0f;
    spec[4].inset[3] = 5.0f;
    spec[4].inset_mask = 0b1001u;

    data = build_blob(spec, 6u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("sticky fixed child: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    check("sticky fixed child: the header pins and the badge stays viewport-anchored",
          rect_is(core, 3, 0, 10, 200, 20) && rect_is(core, 4, 5, 5, 20, 20),
          "node 3 (0,10), node 4 (5,5)");
    check("sticky fixed child: the tall sibling keeps its flow slot",
          rect_is(core, 5, 0, 20, 200, 280), "node 5 (0,20)");

    check("sticky fixed child: scroll_by(50) succeeds",
          wn_core_scroll_by(core, 1, 0.0f, 50.0f) > 0, "scroll y=50");
    check("sticky fixed child: the header stays pinned while the flow moves",
          rect_is(core, 3, 0, 10, 200, 20) && rect_is(core, 5, 0, -30, 200, 280),
          "node 3 (0,10), node 5 (0,-30)");
    check("sticky fixed child: the fixed badge does not move with the sticky shift",
          rect_is(core, 4, 5, 5, 20, 20), "node 4 stays (5,5) after scrolling");
    check("sticky fixed child: the header is hittable away from the badge",
          wn_core_node_at(core, 150.0f, 15.0f) == 3, "node_at(150,15)");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_sticky_nearest_scroller(void)
{
    NodeSpec spec[9];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    /* Nested scrollers: the inner header must use the inner scrollport and the
     * outer header the outer one. */
    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 300.0f, 200.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 300.0f, 600.0f);
    spec_init(&spec[3], 2, R2_NODE_BLOCK, 300.0f, 50.0f);
    spec_init(&spec[4], 2, R2_NODE_BLOCK, 150.0f, 100.0f);
    spec_init(&spec[5], 4, R2_NODE_BLOCK, 150.0f, 300.0f);
    spec_init(&spec[6], 5, R2_NODE_BLOCK, 150.0f, 20.0f);
    spec_init(&spec[7], 5, R2_NODE_BLOCK, 150.0f, 280.0f);
    spec_init(&spec[8], 2, R2_NODE_BLOCK, 300.0f, 20.0f);
    spec[1].overflow = R2_OVERFLOW_SCROLL;
    spec[4].overflow = R2_OVERFLOW_SCROLL;
    spec[6].position = R2_POSITION_STICKY;
    spec[6].inset[0] = 10.0f;
    spec[6].inset_mask = 0b0001u;
    spec[8].position = R2_POSITION_STICKY;
    spec[8].inset[0] = 10.0f;
    spec[8].inset_mask = 0b0001u;

    data = build_blob(spec, 9u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("sticky nearest scroller: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    check("sticky nearest scroller: the inner header starts in the inner scrollport",
          rect_is(core, 4, 0, 50, 150, 100) && rect_is(core, 6, 0, 60, 150, 20),
          "inner scroller (0,50), header y=60 from its top + 10");
    check("sticky nearest scroller: the outer header stays in flow at scroll 0",
          rect_is(core, 8, 0, 150, 300, 20), "node 8 (0,150)");

    check("sticky nearest scroller: scroll_by inner(50) succeeds",
          wn_core_scroll_by(core, 4, 0.0f, 50.0f) > 0, "inner scroll y=50");
    check("sticky nearest scroller: the inner header follows the inner scroller",
          rect_is(core, 6, 0, 60, 150, 20) && rect_is(core, 7, 0, 20, 150, 280),
          "node 6 stays y=60, node 7 y=20");
    check("sticky nearest scroller: the outer header ignores the inner scroll",
          rect_is(core, 8, 0, 150, 300, 20), "node 8 stays (0,150)");

    check("sticky nearest scroller: scroll_by outer(250) succeeds",
          wn_core_scroll_by(core, 1, 0.0f, 250.0f) > 0, "outer scroll y=250");
    check("sticky nearest scroller: the outer header pins to the outer scrollport",
          rect_is(core, 8, 0, 10, 300, 20), "node 8 (0,10)");
    check("sticky nearest scroller: the inner header still pins to the inner scrollport",
          rect_is(core, 4, 0, -200, 150, 100) && rect_is(core, 6, 0, -190, 150, 20),
          "node 6 (0,-190), not the outer threshold (0,10)");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_sticky_top_bottom(void)
{
    NodeSpec spec[6];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    /* At scroll 0 `bottom: 10` would pull the box up to y=70; `top` wins, so it
     * stays at y=80. Later `top` pins it while the bottom rule is inert. */
    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 200.0f, 100.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 200.0f, 300.0f);
    spec_init(&spec[3], 2, R2_NODE_BLOCK, 200.0f, 80.0f);
    spec_init(&spec[4], 2, R2_NODE_BLOCK, 200.0f, 20.0f);
    spec_init(&spec[5], 2, R2_NODE_BLOCK, 200.0f, 200.0f);
    spec[1].overflow = R2_OVERFLOW_SCROLL;
    spec[4].position = R2_POSITION_STICKY;
    spec[4].inset[0] = 10.0f;  /* top */
    spec[4].inset[2] = 10.0f;  /* bottom */
    spec[4].inset_mask = 0b0101u;

    data = build_blob(spec, 6u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("sticky top+bottom: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    check("sticky top+bottom: top wins at scroll 0 (no bottom pull)",
          rect_is(core, 4, 0, 80, 200, 20), "node 4 (0,80), not the bottom threshold y=70");

    check("sticky top+bottom: scroll_by(100) succeeds",
          wn_core_scroll_by(core, 1, 0.0f, 100.0f) > 0, "scroll y=100");
    check("sticky top+bottom: top pins once the flow scrolls past the threshold",
          rect_is(core, 4, 0, 10, 200, 20), "node 4 (0,10)");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_sticky_padded_scrollport(void)
{
    NodeSpec spec[5];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    /* This engine uses the content box (rect + padding) as the scrollport, so a
     * padded scroller pins at the content edge: 8 (padding) + 10 (top). */
    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 200.0f, 100.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 200.0f, 300.0f);
    spec_init(&spec[3], 2, R2_NODE_BLOCK, 200.0f, 20.0f);
    spec_init(&spec[4], 2, R2_NODE_BLOCK, 200.0f, 280.0f);
    spec[1].padding[0] = 8.0f;
    spec[1].overflow = R2_OVERFLOW_SCROLL;
    spec[3].position = R2_POSITION_STICKY;
    spec[3].inset[0] = 10.0f;
    spec[3].inset_mask = 0b0001u;

    data = build_blob(spec, 5u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("sticky padded scrollport: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    check("sticky padded scrollport: pins at rect + padding + inset",
          rect_is(core, 3, 0, 18, 200, 20), "node 3 y=18, not the padding box y=10");

    check("sticky padded scrollport: scroll_by(50) succeeds",
          wn_core_scroll_by(core, 1, 0.0f, 50.0f) > 0 &&
              rect_is(core, 3, 0, 18, 200, 20),
          "node 3 stays y=18 after scrolling");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
}

static void run_sticky_hidden_not_scroller(void)
{
    NodeSpec spec[5];
    unsigned char *data;
    size_t size = 0;
    wn_document *document = NULL;
    wn_core *core;

    /* Only `overflow: scroll` is a sticky scroll container in this engine;
     * `overflow: hidden` clips but does not pin. */
    spec_init(&spec[0], -1, R2_NODE_BLOCK, 400.0f, 300.0f);
    spec_init(&spec[1], 0, R2_NODE_BLOCK, 200.0f, 100.0f);
    spec_init(&spec[2], 1, R2_NODE_BLOCK, 200.0f, 300.0f);
    spec_init(&spec[3], 2, R2_NODE_BLOCK, 200.0f, 20.0f);
    spec_init(&spec[4], 2, R2_NODE_BLOCK, 200.0f, 280.0f);
    spec[1].overflow = R2_OVERFLOW_HIDDEN;
    spec[3].position = R2_POSITION_STICKY;
    spec[3].inset[0] = 10.0f;
    spec[3].inset_mask = 0b0001u;

    data = build_blob(spec, 5u, 400u, 300u, &size);
    core = data != NULL ? load_core(data, size, &document) : NULL;
    if (!check("sticky hidden ancestor: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }

    check("sticky hidden ancestor: overflow hidden is not a scroll container",
          rect_is(core, 3, 0, 0, 200, 20) &&
              fabsf(wn_core_scroll_max(core, 1)) < 0.01f,
          "node 3 stays at y=0 and the hidden box does not scroll");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);
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
    ((WnBlobNode *)(copy + nodes_section->offset))[0].position = 5;
    check("loader: position enum is validated",
          load_rejected(copy, size, "position"), "position 5 rejected");

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

    memcpy(copy, original, size);
    {
        WnBlobNode *first = (WnBlobNode *)(copy + nodes_section->offset);
        wn_document *document;
        first->position = R2_POSITION_FIXED;
        first->inset[0] = 4.0f;
        first->inset_mask = 0x1u;
        document = wn_document_load_blob_memory(copy, size);
        check("loader: fixed survives the extended enum",
              document != NULL && document->nodes[0].position == R2_POSITION_FIXED,
              document != NULL ? NULL : wn_last_error());
        if (document != NULL) {
            wn_document_destroy(document);
        }
    }

    memcpy(copy, original, size);
    {
        WnBlobNode *first = (WnBlobNode *)(copy + nodes_section->offset);
        wn_document *document;
        first->position = R2_POSITION_STICKY;
        first->inset[0] = 6.0f;
        first->inset_mask = 0x1u;
        document = wn_document_load_blob_memory(copy, size);
        check("loader: sticky survives the extended enum",
              document != NULL && document->nodes[0].position == R2_POSITION_STICKY,
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
    run_relative_subtree();
    run_relative_scroll();
    run_flow_removal();
    run_fixed_flow_removal();
    run_scroll_max_exclusion();
    run_clipped_ancestor_escape();
    run_viewport_containing_block();
    run_fixed_positioning();
    run_fixed_scroll_independence();
    run_fixed_nested_absolute();
    run_sticky_top();
    run_sticky_clamp();
    run_sticky_bottom();
    run_sticky_without_scroller();
    run_sticky_fixed_child();
    run_sticky_nearest_scroller();
    run_sticky_top_bottom();
    run_sticky_padded_scrollport();
    run_sticky_hidden_not_scroller();
    run_loader_validation();

    if (failures != 0) {
        printf("position_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }
    printf("position_test: PASS (%d checks)\n", checks);
    return 0;
}
