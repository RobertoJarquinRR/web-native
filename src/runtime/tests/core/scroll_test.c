/*
 * scroll_test.c - the one x/y scroll service, without a GPU or a real page.
 *
 * A three-node blob is built in memory:
 *
 *   root BLOCK 100x100, overflow:scroll   (viewport 100x100)
 *     child 1 BLOCK 400x200               (overflows both axes)
 *     child 2 BLOCK 100x200
 *
 * Vertical overflow is real: content 400 - view 100 => scroll_max_y == 300.
 * Horizontal overflow is NOT computed by layout in this lane, so max_x stays 0
 * and the x half of every call must be a no-op. The test forces the reserved
 * internal max_x to prove the clamp, scroll_by and scroll_into_view x paths
 * work and move only x; then restores 0.
 *
 *   scroll_test.exe
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

static int near(float value, float expected)
{
    return fabsf(value - expected) < 0.01f;
}

static uint32_t align_up8(uint32_t value)
{
    return (value + 7u) & ~7u;
}

static const char kStrings[] = "\0";

static unsigned char *build_blob(size_t *out_size)
{
    const uint32_t node_count = 3u;
    const uint32_t section_bytes = 3u * (uint32_t)sizeof(WnBlobSection);
    const uint32_t nodes_offset = align_up8(WN_BLOB_HEADER_SIZE + section_bytes);
    const uint32_t nodes_bytes = node_count * (uint32_t)sizeof(WnBlobNode);
    const uint32_t names_offset = align_up8(nodes_offset + nodes_bytes);
    const uint32_t names_bytes = node_count * (uint32_t)sizeof(uint32_t);
    const uint32_t strings_offset = align_up8(names_offset + names_bytes);
    const uint32_t strings_bytes = (uint32_t)sizeof(kStrings);
    const uint32_t total = align_up8(strings_offset + strings_bytes);
    unsigned char *data = (unsigned char *)calloc(1, total);
    WnBlobHeader *header;
    WnBlobSection *sections;
    WnBlobNode *nodes;
    uint32_t *name_offsets;

    if (data == NULL) {
        return NULL;
    }
    header = (WnBlobHeader *)data;
    header->magic = WN_BLOB_MAGIC;
    header->version = WN_BLOB_VERSION;
    header->section_count = 3u;
    header->file_size = total;
    header->node_count = node_count;
    header->viewport_width = 100u;
    header->viewport_height = 100u;

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
    nodes[0].parent = -1;
    nodes[0].kind = R2_NODE_BLOCK;
    nodes[0].width = 100.0f;
    nodes[0].height = 100.0f;
    nodes[0].overflow = R2_OVERFLOW_SCROLL;
    nodes[0].font_size = 16.0f;
    nodes[0].flex_shrink = 1.0f;
    nodes[0].flex_basis = -1.0f;

    nodes[1].parent = 0;
    nodes[1].kind = R2_NODE_BLOCK;
    nodes[1].width = 400.0f;
    nodes[1].height = 200.0f;
    nodes[1].font_size = 16.0f;
    nodes[1].flex_shrink = 1.0f;
    nodes[1].flex_basis = -1.0f;

    nodes[2].parent = 0;
    nodes[2].kind = R2_NODE_BLOCK;
    nodes[2].width = 100.0f;
    nodes[2].height = 200.0f;
    nodes[2].font_size = 16.0f;
    nodes[2].flex_shrink = 1.0f;
    nodes[2].flex_basis = -1.0f;
    for (int index = 0; index < 3; ++index) {
        nodes[index].opacity = 1.0f;
    }

    name_offsets = (uint32_t *)(data + names_offset);
    memset(name_offsets, 0, names_bytes);

    memcpy(data + strings_offset, kStrings, sizeof(kStrings));
    *out_size = total;
    return data;
}

int main(void)
{
    unsigned char *data;
    size_t size = 0;
    wn_document *document;
    wn_core *core;
    float nan_delta;
    float inf_delta;
    int wheel;

    data = build_blob(&size);
    if (!check("in-memory blob built", data != NULL && size > 0u, NULL)) {
        printf("scroll_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }
    document = wn_document_load_blob_memory(data, size);
    if (!check("blob loads through the public memory loader", document != NULL,
               wn_last_error())) {
        free(data);
        printf("scroll_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }
    core = wn_core_create(document);
    if (!check("core is created", core != NULL, wn_last_error())) {
        wn_document_destroy(document);
        free(data);
        printf("scroll_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }

    /* Read the deltas from strtof so the compiler cannot fold a NaN constant
     * warning under /W4 /WX; the service must reject both. */
    nan_delta = strtof("nan", NULL);
    inf_delta = strtof("inf", NULL);

    /* ------------------------------------------- vertical: clamp and apply */
    check("layout computes vertical overflow only",
          near(wn_core_scroll_max(core, 0), 300.0f) &&
              near(wn_core_scroll_offset(core, 0), 0.0f) &&
              near(core->scroll_max_x[0], 0.0f),
          "max_y=300, offset=0, max_x=0");

    /* The geometry assertion is the other half of the contract: the offset is
     * applied by translating the children during the layout re-run, so a
     * service that only stores the offset (or translates with the wrong sign)
     * must fail here. */
    check("scroll_by moves y and translates the child by -offset",
          wn_core_scroll_by(core, 0, 0.0f, 100.0f) == 1 &&
              near(wn_core_scroll_offset(core, 0), 100.0f) &&
              near(core->rects[1].y, -100.0f),
          "offset=100, child y=-100");

    check("scroll_by clamps to max_y and translates by -max",
          wn_core_scroll_by(core, 0, 0.0f, 1000000.0f) == 1 &&
              near(wn_core_scroll_offset(core, 0), 300.0f) &&
              near(core->rects[1].y, -300.0f),
          "offset=scroll_max_y=300, child y=-300");

    check("a second identical clamp delta is a no-op",
          wn_core_scroll_by(core, 0, 0.0f, 1000000.0f) == 0 &&
              near(wn_core_scroll_offset(core, 0), 300.0f) &&
              near(core->rects[1].y, -300.0f),
          "returns 0, offset and geometry unchanged");

    check("scroll_by at max_y with +10 is a no-op",
          wn_core_scroll_by(core, 0, 0.0f, 10.0f) == 0 &&
              near(wn_core_scroll_offset(core, 0), 300.0f) &&
              near(core->rects[1].y, -300.0f),
          "returns 0, offset and geometry unchanged");

    /* Direct clamp assertions: layout re-clamps scroll_y after a service call,
     * so only the pure function can show the service bound itself working. */
    check("vertical clamp caps above max_y",
          near(wn_core_scroll_clamp(core, 0, WN_SCROLL_Y, 1000000.0f), 300.0f),
          "max=300");
    check("vertical clamp floors below zero",
          near(wn_core_scroll_clamp(core, 0, WN_SCROLL_Y, -5.0f), 0.0f),
          "min=0");
    check("vertical clamp keeps the exact max",
          near(wn_core_scroll_clamp(core, 0, WN_SCROLL_Y, 300.0f), 300.0f),
          "300 -> 300");

    check("scroll_by rejects non-finite deltas",
          wn_core_scroll_by(core, 0, nan_delta, 0.0f) == -1 &&
              wn_core_scroll_by(core, 0, 0.0f, inf_delta) == -1 &&
              near(wn_core_scroll_offset(core, 0), 300.0f) &&
              near(core->rects[1].y, -300.0f),
          "NaN/inf return -1 and leave the offset and geometry");

    check("scroll_by clamps negatives to 0",
          wn_core_scroll_by(core, 0, 0.0f, -1000000.0f) == 1 &&
              near(wn_core_scroll_offset(core, 0), 0.0f),
          "offset=0");

    check("scroll_by with a zero delta is a no-op",
          wn_core_scroll_by(core, 0, 0.0f, 0.0f) == 0 &&
              near(wn_core_scroll_offset(core, 0), 0.0f) &&
              near(core->scroll_x[0], 0.0f),
          "returns 0 and leaves both offsets");

    check("scroll_by x is inert while scroll_max_x == 0",
          wn_core_scroll_by(core, 0, 100.0f, 0.0f) == 0 &&
              near(wn_core_scroll_offset(core, 0), 0.0f) &&
              near(core->scroll_x[0], 0.0f),
          "x delta ignored, y unchanged");

    /* ---------------------------------------- reserved x clamp and apply */
    /* Layout writes scroll_max_x = 0 for every node it visits, so forcing the
     * bound here proves the clamp/apply state math only: scroll_by below
     * re-runs layout and restores 0. Layout does not compute horizontal
     * overflow in this lane. */
    core->scroll_max_x[0] = 500.0f;
    check("clamp uses the forced horizontal bound",
          near(wn_core_scroll_clamp(core, 0, WN_SCROLL_X, 1000000.0f), 500.0f) &&
              near(wn_core_scroll_clamp(core, 0, WN_SCROLL_X, -5.0f), 0.0f),
          "max=500, min=0");

    check("clamp rejects an invalid axis or node",
          near(wn_core_scroll_clamp(core, 0, 99, 10.0f), 0.0f) &&
              near(wn_core_scroll_clamp(core, 100000, WN_SCROLL_X, 10.0f), 0.0f),
          "both return 0");

    wn_core_scroll_by(core, 0, 0.0f, 100.0f);
    core->scroll_max_x[0] = 500.0f;
    check("scroll_by moves x only with a forced horizontal bound",
          wn_core_scroll_by(core, 0, 100.0f, 0.0f) == 1 &&
              near(core->scroll_x[0], 100.0f) &&
              near(wn_core_scroll_offset(core, 0), 100.0f),
          "x=100, y stays 100");

    /* ------------------------------------------ reserved x into-view path */
    core->scroll_x[0] = 0.0f;
    core->scroll_max_x[0] = 500.0f;
    core->rects[1].x = core->rects[0].x + 150.0f; /* right of the 100px box */
    core->rects[1].y = core->rects[0].y + 10.0f;  /* already visible in y */
    core->rects[1].w = 20.0f;
    core->rects[1].h = 10.0f;
    check("scroll_into_view moves x only",
          wn_core_scroll_node_into_view(core, 1, 0.0f) == 0 &&
              near(core->scroll_x[0], 70.0f) &&
              near(wn_core_scroll_offset(core, 0), 100.0f),
          "x=70 (right edge aligned), y stays 100");

    /* Restore the lane invariant before the synthetic-core wheel check; the
     * pilot suite is the real-page wheel coverage. */
    core->scroll_x[0] = 0.0f;
    core->scroll_max_x[0] = 0.0f;

    wheel = wn_core_wheel(core, 50.0f, 50.0f, 100.0f, 50.0f);
    check("wheel with both deltas moves only y",
          wheel == 0 && near(wn_core_scroll_offset(core, 0), 150.0f) &&
              near(core->scroll_x[0], 0.0f),
          "container=root, y=150, x=0");

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);

    if (failures != 0) {
        printf("scroll_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }
    printf("scroll_test: PASS (%d checks)\n", checks);
    return 0;
}
