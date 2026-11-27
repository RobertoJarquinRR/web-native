/*
 * layout_override_test.c - ABI v7 runtime layout overrides.
 *
 * The document is built in memory (no HTML/CSS, no GPU), so every rect is
 * exact:
 *   - wn_core_set_size changes the laid-out box, dirties layout/paint and the
 *     rebuilt display list reflects the new geometry;
 *   - WN_AUTO (or any negative size) restores the compiled px/percentage size;
 *   - wn_core_set_position moves a relative/absolute node against its existing
 *     containing block, clears the opposite inset, and restores the compiled
 *     left/right or top/bottom pair (including a right/bottom-only box);
 *   - cloned nodes keep their own base geometry, so "auto" restores the clone
 *     template values;
 *   - static nodes, dead handles, NaN and infinity are rejected without
 *     touching the current geometry.
 *
 *   layout_override_test.exe
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "document/wn_blob.h"
#include "document/wn_document_format.h"
#include "wn_core.h"

static int g_checks;
static int g_failures;

static int check(const char *name, int ok, const char *detail)
{
    ++g_checks;
    printf("[%s] override: %s - %s\n", ok ? "ok  " : "FAIL", name, detail);
    if (!ok) {
        ++g_failures;
    }
    return ok;
}

typedef struct {
    int parent;
    float width;
    float height;
    float width_pct;
    float height_pct;
    int position;
    float inset[4];      /* top,right,bottom,left */
    unsigned int inset_mask;
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
        nodes[i].kind = R2_NODE_BLOCK;
        nodes[i].width = spec[i].width;
        nodes[i].height = spec[i].height;
        nodes[i].width_pct = spec[i].width_pct;
        nodes[i].height_pct = spec[i].height_pct;
        nodes[i].position = spec[i].position;
        memcpy(nodes[i].inset, spec[i].inset, sizeof(nodes[i].inset));
        nodes[i].inset_mask = spec[i].inset_mask;
        /* A non-zero background gives every box a solid pass, so the display
         * list changes when geometry changes. */
        nodes[i].background = 0xFF203040u;
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

static unsigned int list_hash(const wn_display_list *list)
{
    const unsigned char *bytes;
    size_t size;
    size_t i;
    unsigned int hash = 2166136261u;
    if (list == NULL || list->vertices == NULL || list->vertex_count <= 0) {
        return 0u;
    }
    bytes = (const unsigned char *)list->vertices;
    size = (size_t)list->vertex_count * sizeof(wn_vertex);
    for (i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static wn_rect rect_of(wn_core *core, int node)
{
    wn_rect rect;
    memset(&rect, 0, sizeof(rect));
    if (wn_core_node_rect(core, node, &rect) != 0) {
        rect.x = rect.y = rect.w = rect.h = -1.0f;
    }
    return rect;
}

static int rects_equal(wn_rect a, wn_rect b)
{
    return fabsf(a.x - b.x) < 0.01f && fabsf(a.y - b.y) < 0.01f &&
           fabsf(a.w - b.w) < 0.01f && fabsf(a.h - b.h) < 0.01f;
}

int main(void)
{
    enum {
        kRoot = 0,
        kBox = 1,
        kAbs = 2,
        kRel = 3,
        kPct = 4,
        kAbsRight = 5,
        kStatic = 6,
        kCount = 7
    };
    NodeSpec spec[kCount];
    unsigned char *data;
    size_t size = 0;
    wn_document *document;
    wn_core *core;
    wn_rect initial_box;
    wn_rect initial_abs;
    wn_rect initial_rel;
    wn_rect initial_pct;
    wn_rect initial_abs_right;
    wn_rect rect;
    unsigned int initial_hash;
    unsigned int hash;
    int clone;

    memset(spec, 0, sizeof(spec));
    /* 0: 200x150 relative root; it is the containing block of the children. */
    spec[kRoot].parent = -1;
    spec[kRoot].width = 200.0f;
    spec[kRoot].height = 150.0f;
    spec[kRoot].position = R2_POSITION_RELATIVE;
    /* 1: flow box with a compiled 80x40 size. */
    spec[kBox].parent = kRoot;
    spec[kBox].width = 80.0f;
    spec[kBox].height = 40.0f;
    /* 2: absolute box at left/top 10/20. */
    spec[kAbs].parent = kRoot;
    spec[kAbs].width = 60.0f;
    spec[kAbs].height = 30.0f;
    spec[kAbs].position = R2_POSITION_ABSOLUTE;
    spec[kAbs].inset[3] = 10.0f;
    spec[kAbs].inset[0] = 20.0f;
    spec[kAbs].inset_mask = (1u << 3) | (1u << 0);
    /* 3: relative box offset by left/top 6/5. */
    spec[kRel].parent = kRoot;
    spec[kRel].width = 50.0f;
    spec[kRel].height = 20.0f;
    spec[kRel].position = R2_POSITION_RELATIVE;
    spec[kRel].inset[3] = 6.0f;
    spec[kRel].inset[0] = 5.0f;
    spec[kRel].inset_mask = (1u << 3) | (1u << 0);
    /* 4: compiled 50% width (100px of the 200px containing block). */
    spec[kPct].parent = kRoot;
    spec[kPct].width = -1.0f;
    spec[kPct].width_pct = 50.0f;
    spec[kPct].height = 40.0f;
    /* 5: absolute placed by right/bottom 15/25. */
    spec[kAbsRight].parent = kRoot;
    spec[kAbsRight].width = 50.0f;
    spec[kAbsRight].height = 20.0f;
    spec[kAbsRight].position = R2_POSITION_ABSOLUTE;
    spec[kAbsRight].inset[1] = 15.0f;
    spec[kAbsRight].inset[2] = 25.0f;
    spec[kAbsRight].inset_mask = (1u << 1) | (1u << 2);
    /* 6: static node, used only for the rejected property case. */
    spec[kStatic].parent = kRoot;
    spec[kStatic].width = 30.0f;
    spec[kStatic].height = 10.0f;

    data = build_blob(spec, kCount, 300, 200, &size);
    if (data == NULL) {
        printf("FAIL: cannot build the in-memory blob\n");
        return 1;
    }
    document = wn_document_load_blob_memory(data, size);
    if (document == NULL) {
        printf("FAIL: wn_document_load_blob_memory: %s\n",
               wn_last_error() != NULL ? wn_last_error() : "unknown");
        free(data);
        return 1;
    }
    core = wn_core_create(document);
    /* The core retains its own document reference; the caller's one is not
     * needed after creation. The blob buffer still backs the strings. */
    wn_document_release(document);
    if (core == NULL) {
        printf("FAIL: wn_core_create: %s\n",
               wn_last_error() != NULL ? wn_last_error() : "unknown");
        free(data);
        return 1;
    }
    wn_core_resize(core, 300, 200);
    wn_core_relayout(core);
    wn_core_build_display_list(core);

    initial_box = rect_of(core, kBox);
    initial_abs = rect_of(core, kAbs);
    initial_rel = rect_of(core, kRel);
    initial_pct = rect_of(core, kPct);
    initial_abs_right = rect_of(core, kAbsRight);
    initial_hash = list_hash(wn_core_display_list(core));
    printf("override: initial box=%.0fx%.0f abs=%.0f,%.0f rel=%.0f,%.0f "
           "pct=%.0f right=%.0f,%.0f\n",
           initial_box.w, initial_box.h, initial_abs.x, initial_abs.y,
           initial_rel.x, initial_rel.y, initial_pct.w,
           initial_abs_right.x, initial_abs_right.y);
    check("initial geometry",
          initial_box.w == 80.0f && initial_box.h == 40.0f &&
              initial_abs.x == 10.0f && initial_abs.y == 20.0f &&
              initial_rel.x == 6.0f && initial_rel.y == 45.0f &&
              initial_pct.w == 100.0f &&
              initial_abs_right.x == 135.0f && initial_abs_right.y == 105.0f,
          "compiled size/position values are laid out");
    check("initial paint", initial_hash != 0u && wn_core_dirty(core) == 0,
          "display list has geometry and the core is clean");

    /* ------------------------------------------------ size: change/restore */
    check("set_size accepted",
          wn_core_set_size(core, kBox, 120.0f, 60.0f) == 0,
          "WN_OK on an 80x40 box");
    check("set_size dirties",
          wn_core_dirty(core) != 0 && wn_core_needs_layout(core) != 0,
          "layout and paint are marked dirty");
    wn_core_relayout(core);
    wn_core_build_display_list(core);
    rect = rect_of(core, kBox);
    hash = list_hash(wn_core_display_list(core));
    check("set_size reflows", rect.w == 120.0f && rect.h == 60.0f,
          "the laid-out box is now 120x60");
    check("set_size repaints", hash != initial_hash,
          "the rebuilt display list reflects the new geometry");
    check("set_size auto restores",
          wn_core_set_size(core, kBox, WN_AUTO, WN_AUTO) == 0,
          "WN_AUTO accepted");
    wn_core_relayout(core);
    wn_core_build_display_list(core);
    rect = rect_of(core, kBox);
    check("set_size auto geometry",
          rect.w == 80.0f && rect.h == 40.0f &&
              list_hash(wn_core_display_list(core)) == initial_hash,
          "compiled 80x40 and the initial display list are restored");
    check("negative size restores",
          wn_core_set_size(core, kBox, -5.0f, 55.0f) == 0,
          "a negative width restores the compiled axis");
    wn_core_relayout(core);
    rect = rect_of(core, kBox);
    check("negative size geometry", rect.w == 80.0f && rect.h == 55.0f,
          "width is compiled, height is the explicit 55");
    check("size zero accepted",
          wn_core_set_size(core, kBox, 0.0f, 0.0f) == 0,
          "zero is a valid explicit size");
    wn_core_relayout(core);
    rect = rect_of(core, kBox);
    check("size zero geometry", rect.w == 0.0f && rect.h == 0.0f,
          "the box collapses");
    check("size restore for next stage",
          wn_core_set_size(core, kBox, WN_AUTO, WN_AUTO) == 0,
          "compiled box restored");
    wn_core_relayout(core);
    wn_core_build_display_list(core);

    /* ------------------------------------------ percentage size restore */
    check("set_size percentage override",
          wn_core_set_size(core, kPct, 55.0f, WN_AUTO) == 0,
          "explicit width, compiled height");
    wn_core_relayout(core);
    rect = rect_of(core, kPct);
    check("percentage fixed geometry", rect.w == 55.0f && rect.h == 40.0f,
          "width 55 and the compiled 40px height");
    check("set_size percentage auto",
          wn_core_set_size(core, kPct, WN_AUTO, WN_AUTO) == 0,
          "WN_AUTO accepted");
    wn_core_relayout(core);
    rect = rect_of(core, kPct);
    check("percentage restored", rect.w == 100.0f && rect.h == 40.0f,
          "the compiled 50% width is resolved again");

    /* ------------------------------------------- absolute position moves */
    check("set_position accepted",
          wn_core_set_position(core, kAbs, 40.0f, 50.0f) == 0,
          "absolute node at left/top 40/50");
    check("set_position dirties", wn_core_dirty(core) != 0,
          "the next frame relayouts and repaints");
    wn_core_relayout(core);
    wn_core_build_display_list(core);
    rect = rect_of(core, kAbs);
    check("absolute moved", rect.x == 40.0f && rect.y == 50.0f,
          "left/top are resolved against the root containing block");
    check("absolute still same size", rect.w == 60.0f && rect.h == 30.0f,
          "position does not change the box size");
    check("set_position auto",
          wn_core_set_position(core, kAbs, WN_AUTO, WN_AUTO) == 0,
          "WN_AUTO restores both axes");
    wn_core_relayout(core);
    rect = rect_of(core, kAbs);
    check("absolute restored", rect.x == 10.0f && rect.y == 20.0f,
          "compiled left/top are back");
    check("partial auto",
          wn_core_set_position(core, kAbs, 70.0f, WN_AUTO) == 0,
          "x explicit, y restored");
    wn_core_relayout(core);
    rect = rect_of(core, kAbs);
    check("partial auto geometry", rect.x == 70.0f && rect.y == 20.0f,
          "only the vertical axis was restored");
    check("partial auto cleanup",
          wn_core_set_position(core, kAbs, WN_AUTO, WN_AUTO) == 0,
          "both axes restored");
    wn_core_relayout(core);

    /* -------------------------------------- relative and right/bottom base */
    check("relative position accepted",
          wn_core_set_position(core, kRel, 26.0f, 35.0f) == 0,
          "relative node moved by left/top");
    wn_core_relayout(core);
    rect = rect_of(core, kRel);
    check("relative moved", rect.x == 26.0f && rect.y == 75.0f,
          "flow slot (y=40) plus the new left/top offset");
    check("relative restore",
          wn_core_set_position(core, kRel, WN_AUTO, WN_AUTO) == 0,
          "compiled offset restored");
    wn_core_relayout(core);
    rect = rect_of(core, kRel);
    check("relative restored", rect.x == 6.0f && rect.y == 45.0f,
          "compiled left/top 6/5 are back");

    check("right/bottom override",
          wn_core_set_position(core, kAbsRight, 30.0f, 40.0f) == 0,
          "left/top replace right/bottom");
    wn_core_relayout(core);
    rect = rect_of(core, kAbsRight);
    check("right/bottom moved", rect.x == 30.0f && rect.y == 40.0f,
          "the opposite insets are cleared");
    check("right/bottom restore",
          wn_core_set_position(core, kAbsRight, WN_AUTO, WN_AUTO) == 0,
          "compiled right/bottom pair restored");
    wn_core_relayout(core);
    rect = rect_of(core, kAbsRight);
    check("right/bottom restored", rect.x == 135.0f && rect.y == 105.0f,
          "the compiled right/bottom placement is back");

    /* ------------------------------------------------------ clone base */
    clone = wn_core_clone(core, kAbs, kRoot);
    wn_core_relayout(core);
    rect = rect_of(core, clone);
    check("clone starts from template",
          clone > 0 && rect.x == 10.0f && rect.y == 20.0f,
          "the clone copies the compiled position");
    check("clone position override",
          wn_core_set_position(core, clone, 80.0f, 90.0f) == 0,
          "the clone accepts its own override");
    wn_core_relayout(core);
    rect = rect_of(core, clone);
    check("clone moved", rect.x == 80.0f && rect.y == 90.0f,
          "the clone is placed independently");
    check("clone auto restores template base",
          wn_core_set_position(core, clone, WN_AUTO, WN_AUTO) == 0,
          "WN_AUTO on the clone");
    wn_core_relayout(core);
    rect = rect_of(core, clone);
    check("clone restored", rect.x == 10.0f && rect.y == 20.0f,
          "the clone's own base is the template geometry");
    check("clone removed", wn_core_remove(core, clone) == 0,
          "fixture cleaned up");
    wn_core_relayout(core);

    /* --------------------------------------------------------- invalid */
    rect = rect_of(core, kAbs);
    check("invalid handles and values",
          wn_core_set_size(core, -1, 10.0f, 10.0f) == -1 &&
              wn_core_set_size(core, 100000, 10.0f, 10.0f) == -1 &&
              wn_core_set_size(core, kBox, NAN, 10.0f) == -1 &&
              wn_core_set_size(core, kBox, 10.0f, INFINITY) == -1 &&
              wn_core_set_position(core, -1, 1.0f, 1.0f) == -1 &&
              wn_core_set_position(core, kAbs, NAN, 1.0f) == -1 &&
              wn_core_set_position(core, kAbs, 1.0f, INFINITY) == -1,
          "dead handles, NaN and infinity are rejected");
    check("static position rejected",
          wn_core_set_position(core, kStatic, 5.0f, 5.0f) == 1,
          "a static node has no containing-block offset to override");
    wn_core_relayout(core);
    check("invalid changes nothing", rects_equal(rect, rect_of(core, kAbs)),
          "the last valid geometry survived every rejected call");

    wn_core_destroy(core);
    free(data);
    printf("layout_override_test: %s (%d checks)\n",
           g_failures == 0 ? "PASS" : "FAIL", g_checks);
    return g_failures == 0 ? 0 : 1;
}
