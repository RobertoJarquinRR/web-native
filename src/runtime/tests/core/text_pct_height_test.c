/*
 * text_pct_height_test.c - percentage height on TEXT follows the leaf rule.
 *
 * The capability table gives every leaf one declared-height rule:
 *
 *   border-box height = parent_content_h * height_pct * 0.01 + WN_DECL_H(node)
 *
 * so a TEXT node with height:50% resolves exactly like IMAGE/INPUT: the
 * percentage is the content box and the padding is added back for content-box.
 *
 * This geometry is unreachable from the HTML/CSS emitter today (it never emits
 * percentages on TEXT), but the public .wnui loader accepts it, so the rule is
 * locked here through wn_document_load_blob_memory() + a real core. The fixture
 * is built in memory; no file, no GPU, no emitted page.
 *
 *   root BLOCK 100x100 content box, viewport 200x200
 *     TEXT width 100, height:50%, padding 10, content-box -> 70 (50 + 20)
 *     TEXT width 100, height:50%, padding 10, border-box  -> 50 (50 + 0)
 *     TEXT width 100, line-height 20, no pct/padding      -> 20 (auto path)
 *
 * The viewport is twice the root on purpose: 70/50 prove the percentage was
 * resolved against the parent's 100px content box, not the viewport.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static uint32_t align_up8(uint32_t value)
{
    return (value + 7u) & ~7u;
}

/* One strings pool, split at compile time. Offset 0 is the NULL sentinel, so
 * every real string starts at byte 1. */
static const char kStrings[] = "\0pct-content-box\0pct-border-box\0plain";

static unsigned char *build_blob(size_t *out_size)
{
    const uint32_t node_count = 4u;
    const uint32_t section_bytes = 3u * (uint32_t)sizeof(WnBlobSection);
    const uint32_t nodes_offset = align_up8(WN_BLOB_HEADER_SIZE + section_bytes);
    const uint32_t nodes_bytes = node_count * (uint32_t)sizeof(WnBlobNode);
    const uint32_t names_offset = align_up8(nodes_offset + nodes_bytes);
    const uint32_t names_bytes = node_count * (uint32_t)sizeof(uint32_t);
    const uint32_t strings_offset = align_up8(names_offset + names_bytes);
    const uint32_t strings_bytes = (uint32_t)sizeof(kStrings);
    const uint32_t total = align_up8(strings_offset + strings_bytes);
    const char *content_text = kStrings + 1;
    const char *border_text = content_text + strlen(content_text) + 1;
    const char *plain_text = border_text + strlen(border_text) + 1;
    unsigned char *data = (unsigned char *)calloc(1, total);
    WnBlobHeader *header;
    WnBlobSection *sections;
    WnBlobNode *nodes;
    uint32_t *name_offsets;
    char *strings;

    if (data == NULL) {
        return NULL;
    }
    header = (WnBlobHeader *)data;
    header->magic = WN_BLOB_MAGIC;
    header->version = WN_BLOB_VERSION;
    header->section_count = 3u;
    header->file_size = total;
    header->node_count = node_count;
    header->viewport_width = 200u;
    header->viewport_height = 200u;

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
    /* Root: block, 100x100, no padding. Its content box is what both TEXT
     * percentage children resolve against. */
    nodes[0].parent = -1;
    nodes[0].kind = R2_NODE_BLOCK;
    nodes[0].width = 100.0f;
    nodes[0].height = 100.0f;
    nodes[0].font_size = 16.0f;
    nodes[0].flex_shrink = 1.0f;
    nodes[0].flex_basis = -1.0f;

    /* Content-box TEXT: height:50% + 10px padding on all sides. */
    nodes[1].parent = 0;
    nodes[1].kind = R2_NODE_TEXT;
    nodes[1].width = 100.0f;
    nodes[1].height = -1.0f;
    nodes[1].height_pct = 50.0f;
    nodes[1].padding[0] = 10.0f;
    nodes[1].padding[1] = 10.0f;
    nodes[1].padding[2] = 10.0f;
    nodes[1].padding[3] = 10.0f;
    nodes[1].font_size = 16.0f;
    nodes[1].text = (uint32_t)(content_text - kStrings);
    nodes[1].flex_shrink = 1.0f;
    nodes[1].flex_basis = -1.0f;

    /* Same declaration under border-box: the 50% already includes padding. */
    nodes[2].parent = 0;
    nodes[2].kind = R2_NODE_TEXT;
    nodes[2].width = 100.0f;
    nodes[2].height = -1.0f;
    nodes[2].height_pct = 50.0f;
    nodes[2].padding[0] = 10.0f;
    nodes[2].padding[1] = 10.0f;
    nodes[2].padding[2] = 10.0f;
    nodes[2].padding[3] = 10.0f;
    nodes[2].box_sizing = 1;
    nodes[2].font_size = 16.0f;
    nodes[2].text = (uint32_t)(border_text - kStrings);
    nodes[2].flex_shrink = 1.0f;
    nodes[2].flex_basis = -1.0f;

    /* Normal TEXT: no percentage, no padding. Auto height must still be the
     * line box (line-height 20), untouched by the leaf percentage rule. */
    nodes[3].parent = 0;
    nodes[3].kind = R2_NODE_TEXT;
    nodes[3].width = 100.0f;
    nodes[3].height = -1.0f;
    nodes[3].font_size = 16.0f;
    nodes[3].line_height = 20.0f;
    nodes[3].text = (uint32_t)(plain_text - kStrings);
    nodes[3].flex_shrink = 1.0f;
    nodes[3].flex_basis = -1.0f;

    name_offsets = (uint32_t *)(data + names_offset);
    memset(name_offsets, 0, names_bytes);

    strings = (char *)(data + strings_offset);
    memcpy(strings, kStrings, sizeof(kStrings));

    *out_size = total;
    return data;
}

static int near(float value, float expected)
{
    return fabsf(value - expected) < 0.01f;
}

int main(void)
{
    unsigned char *data;
    size_t size = 0;
    wn_document *document;
    wn_core *core;
    const wn_rect *rects;
    int count = 0;
    char detail[128];

    checks = 0;
    failures = 0;

    data = build_blob(&size);
    if (!check("in-memory blob built", data != NULL && size > 0u, NULL)) {
        printf("text_pct_height_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }

    document = wn_document_load_blob_memory(data, size);
    if (!check("blob loads through the public memory loader", document != NULL,
               wn_last_error())) {
        free(data);
        printf("text_pct_height_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }

    core = wn_core_create(document);
    if (!check("core is created", core != NULL, wn_last_error())) {
        wn_document_destroy(document);
        free(data);
        printf("text_pct_height_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }
    wn_core_resize(core, 200, 200);
    rects = wn_core_rects(core, &count);

    snprintf(detail, sizeof(detail), "count=%d", count);
    if (check("all four fixture nodes have a rect", count == 4, detail)) {
        snprintf(detail, sizeof(detail), "root h=%.1f", rects[0].h);
        check("root content box is 100px", near(rects[0].h, 100.0f), detail);

        snprintf(detail, sizeof(detail), "h=%.1f expected=70", rects[1].h);
        check("TEXT height:50% content-box includes the 20px padding",
              near(rects[1].h, 70.0f), detail);

        snprintf(detail, sizeof(detail), "h=%.1f expected=50", rects[2].h);
        check("TEXT height:50% border-box keeps the percentage as the box",
              near(rects[2].h, 50.0f), detail);

        snprintf(detail, sizeof(detail), "h=%.1f expected=20", rects[3].h);
        check("TEXT without pct/padding keeps the auto line box",
              near(rects[3].h, 20.0f), detail);

        /* 70 would be 120 if the percentage had used the 200px viewport. */
        snprintf(detail, sizeof(detail), "h=%.1f", rects[1].h);
        check("TEXT percentage resolves against the parent content box",
              !near(rects[1].h, 120.0f), detail);
    }

    wn_core_destroy(core);
    wn_document_destroy(document);
    free(data);

    if (failures != 0) {
        printf("text_pct_height_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }
    printf("text_pct_height_test: PASS (%d checks)\n", checks);
    return 0;
}
