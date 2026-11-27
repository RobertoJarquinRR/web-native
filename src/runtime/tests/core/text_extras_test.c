/*
 * text_extras_test.c - blob v6 optional text_extras section.
 *
 * Validates the additive ellipsis section without HTML/CSS: valid load,
 * reserved/unknown flag rejection, node range and strictly increasing order.
 *
 *   text_extras_test.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "document/wn_blob.h"
#include "document/wn_document_internal.h"
#include "wn_internal.h"

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

static const char kStrings[] = "\0";

/* Two-node chain (root + text child) plus `count` text_extras records. */
static unsigned char *build_extras_blob(const WnBlobTextExtras *extras,
                                        uint32_t count, size_t *out_size)
{
    uint32_t section_count = 3u + (count > 0 ? 1u : 0u);
    uint32_t offset = WN_BLOB_HEADER_SIZE +
                      section_count * (uint32_t)sizeof(WnBlobSection);
    uint32_t nodes_offset = align_up8(offset);
    uint32_t nodes_bytes = 2u * (uint32_t)sizeof(WnBlobNode);
    uint32_t names_offset = align_up8(nodes_offset + nodes_bytes);
    uint32_t names_bytes = 2u * (uint32_t)sizeof(uint32_t);
    uint32_t strings_offset = align_up8(names_offset + names_bytes);
    uint32_t strings_bytes = (uint32_t)sizeof(kStrings);
    uint32_t extras_offset = align_up8(strings_offset + strings_bytes);
    uint32_t extras_bytes = count * (uint32_t)sizeof(WnBlobTextExtras);
    uint32_t total = align_up8(extras_offset + extras_bytes);
    unsigned char *data = (unsigned char *)calloc(1, total);
    WnBlobHeader *header;
    WnBlobSection *sections;
    WnBlobNode *nodes;
    uint32_t section = 0;

    if (data == NULL) {
        return NULL;
    }
    header = (WnBlobHeader *)data;
    header->magic = WN_BLOB_MAGIC;
    header->version = WN_BLOB_VERSION;
    header->section_count = section_count;
    header->file_size = total;
    header->node_count = 2u;
    header->viewport_width = 200u;
    header->viewport_height = 120u;

    sections = (WnBlobSection *)(data + WN_BLOB_HEADER_SIZE);
    sections[section].id = WN_BLOB_SECTION_NODES;
    sections[section].offset = nodes_offset;
    sections[section].size = nodes_bytes;
    ++section;
    sections[section].id = WN_BLOB_SECTION_NAMES;
    sections[section].offset = names_offset;
    sections[section].size = names_bytes;
    ++section;
    sections[section].id = WN_BLOB_SECTION_STRINGS;
    sections[section].offset = strings_offset;
    sections[section].size = strings_bytes;
    ++section;
    if (count > 0) {
        sections[section].id = WN_BLOB_SECTION_TEXT_EXTRAS;
        sections[section].offset = extras_offset;
        sections[section].size = extras_bytes;
        memcpy(data + extras_offset, extras, extras_bytes);
    }

    nodes = (WnBlobNode *)(data + nodes_offset);
    nodes[0].parent = -1;
    nodes[0].kind = R2_NODE_BLOCK;
    nodes[0].width = 200.0f;
    nodes[0].height = 120.0f;
    nodes[0].font_size = 16.0f;
    nodes[0].flex_shrink = 1.0f;
    nodes[0].flex_basis = -1.0f;
    nodes[0].opacity = 1.0f;
    nodes[0].grid_auto_rows = -1.0f;
    nodes[0].z_index = WN_BLOB_Z_INDEX_AUTO;
    nodes[1].parent = 0;
    nodes[1].kind = R2_NODE_TEXT;
    nodes[1].width = 80.0f;
    nodes[1].height = 20.0f;
    nodes[1].font_size = 16.0f;
    nodes[1].flex_shrink = 1.0f;
    nodes[1].flex_basis = -1.0f;
    nodes[1].opacity = 1.0f;
    nodes[1].grid_auto_rows = -1.0f;
    nodes[1].z_index = WN_BLOB_Z_INDEX_AUTO;
    memcpy(data + strings_offset, kStrings, sizeof(kStrings));
    *out_size = total;
    return data;
}

static int load_extras(const WnBlobTextExtras *extras, uint32_t count,
                       unsigned int *flags_out)
{
    size_t size = 0;
    unsigned char *data = build_extras_blob(extras, count, &size);
    wn_document *document;
    if (data == NULL) {
        return -1;
    }
    document = wn_document_load_blob_memory(data, size);
    free(data);
    if (document == NULL) {
        return 0;
    }
    if (flags_out != NULL) {
        *flags_out = document->nodes[1].text_flags;
    }
    wn_document_destroy(document);
    return 1;
}

int main(void)
{
    WnBlobTextExtras record;
    WnBlobTextExtras pair[2];
    unsigned int flags = 0;

    memset(&record, 0, sizeof(record));
    record.node = 1;
    record.flags = WN_TEXT_FLAG_ELLIPSIS;
    check("valid text_extras record loads",
          load_extras(&record, 1, &flags) == 1 && flags == WN_TEXT_FLAG_ELLIPSIS,
          "flags reach UiNode.text_flags");

    record.flags = 0x02u;
    check("unknown text_extras flag is rejected",
          load_extras(&record, 1, NULL) == 0, NULL);
    record.flags = 0;
    check("zero text_extras flags are rejected",
          load_extras(&record, 1, NULL) == 0, "a zero record is not emitted");
    record.flags = WN_TEXT_FLAG_ELLIPSIS;
    record.node = 5;
    check("text_extras node range is validated",
          load_extras(&record, 1, NULL) == 0, "node 5 is out of range");

    memset(pair, 0, sizeof(pair));
    pair[0].node = 1;
    pair[0].flags = WN_TEXT_FLAG_ELLIPSIS;
    pair[1].node = 1;
    pair[1].flags = WN_TEXT_FLAG_ELLIPSIS;
    check("text_extras order is strictly increasing",
          load_extras(pair, 2, NULL) == 0, "duplicate node records are rejected");

    printf("text_extras_test: %s (%d checks)\n",
           failures == 0 ? "PASS" : "FAIL", checks);
    return failures == 0 ? 0 : 1;
}
