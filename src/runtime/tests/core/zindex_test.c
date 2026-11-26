/*
 * zindex_test.c - blob v5 CSS stacking contexts and `z-index` paint order.
 *
 * Blobs are built in memory (no HTML/CSS, no GPU) so every case is exact:
 *
 *   siblings   - positioned siblings with z 1/2 and the swapped order.
 *   auto/zero  - positioned z auto and z 0 are both step 8, tree order.
 *   negative   - a negative context paints behind the context's in-flow
 *                content.
 *   escape     - a high z inside a low-z parent context cannot escape it.
 *   opacity+z  - a group context with an explicit z orders as a group and
 *                allocates a texture; a z-only context does not.
 *   nested     - nested contexts compose and the parent block stays contiguous.
 *   same z     - equal-z siblings keep tree order, including reversed DOM.
 *   relative   - a first-in-DOM relative z node paints above a later in-flow
 *                sibling that overlaps it.
 *   static group - a non-positioned `opacity < 1` group is a step-8 block, so
 *                it paints above in-flow siblings and composes with a
 *                positioned z auto box by tree order.
 *   static     - z-index on a static (non flex/grid) node is ignored, as CSS;
 *                on a static flex/grid item it creates a context and orders
 *                over siblings (including a trapped nested context).
 *   empty clip - a group clipped to nothing allocates no texture, so an
 *                escaping absolute keeps painting (with and without an
 *                unrelated group on the page).
 *   flat       - a page with no opacity/z/positioning keeps the flat path.
 *   loader     - the v5 sentinel and explicit int32 values survive.
 *
 *   zindex_test.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/wn_core_internal.h"
#include "document/wn_blob.h"
#include "document/wn_document_format.h"
#include "document/wn_document_internal.h"
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
    int position;
    float inset[4];     /* top,right,bottom,left */
    unsigned int inset_mask;
    int z_index;
    int overflow;
    float opacity;
    float margin[4];    /* top,right,bottom,left */
    int direction;      /* 0 row, 1 column */
    float gap;
    int grid_mode;
    float grid_column_px;
    float grid_auto_rows;
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
        nodes[i].position = spec[i].position;
        memcpy(nodes[i].inset, spec[i].inset, sizeof(nodes[i].inset));
        nodes[i].inset_mask = spec[i].inset_mask;
        nodes[i].z_index = spec[i].z_index;
        nodes[i].overflow = spec[i].overflow;
        memcpy(nodes[i].margin, spec[i].margin, sizeof(nodes[i].margin));
        nodes[i].direction = spec[i].direction;
        nodes[i].gap = spec[i].gap;
        nodes[i].grid_mode = spec[i].grid_mode;
        nodes[i].grid_column_px = spec[i].grid_column_px;
        nodes[i].grid_auto_rows = spec[i].grid_auto_rows;
        nodes[i].font_size = 16.0f;
        nodes[i].flex_shrink = 1.0f;
        nodes[i].flex_basis = -1.0f;
        nodes[i].opacity = spec[i].opacity;
    }
    memset(data + names_offset, 0, names_bytes);
    memcpy(data + strings_offset, kStrings, sizeof(kStrings));
    *out_size = total;
    return data;
}

static void spec_init(NodeSpec *spec, int parent, float width, float height)
{
    memset(spec, 0, sizeof(*spec));
    spec->parent = parent;
    spec->kind = R2_NODE_BLOCK;
    spec->width = width;
    spec->height = height;
    spec->position = R2_POSITION_STATIC;
    spec->z_index = R2_Z_INDEX_AUTO;
    spec->opacity = 1.0f;
    spec->grid_auto_rows = -1.0f;
}

static void set_absolute(NodeSpec *spec, float left, float top, float width,
                         float height)
{
    spec->position = R2_POSITION_ABSOLUTE;
    spec->inset[0] = top;
    spec->inset[3] = left;
    spec->inset_mask = 0b1001u;
    spec->width = width;
    spec->height = height;
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

static wn_core *build_core(const NodeSpec *spec, uint32_t count,
                           wn_document **document)
{
    size_t size = 0;
    unsigned char *data = build_blob(spec, count, 400u, 200u, &size);
    wn_core *core;
    if (data == NULL) {
        return NULL;
    }
    core = load_core(data, size, document);
    free(data);
    return core;
}

/* Item indices of the visible paint list, in paint order. */
static int item_order_is(const wn_core *core, const int *expected, int count)
{
    int i;
    if (core->paint_item_count != count) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        if (core->paint_items[i].node != expected[i]) {
            return 0;
        }
    }
    return 1;
}

static void dump_order(const wn_core *core)
{
    int i;
    printf("    order:");
    for (i = 0; i < core->paint_item_count; ++i) {
        printf(" %d", core->paint_items[i].node);
    }
    printf("\n");
}

static int nodes_order(wn_core *core, const int *expected, int count,
                       const char *label)
{
    int ok = item_order_is(core, expected, count);
    check(label, ok, ok ? NULL : "see order below");
    if (!ok) {
        dump_order(core);
    }
    return ok;
}

static int layer_flags_are(const wn_core *core, const int *expected, int count)
{
    int i;
    if (core->paint_layer_count != count) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        if (core->paint_layers[i].needs_texture != expected[i]) {
            return 0;
        }
    }
    return 1;
}

static void run_siblings(void)
{
    NodeSpec spec[3];
    wn_document *document = NULL;
    wn_core *core;
    static const int forward[] = { 0, 1, 2 };
    static const int swapped[] = { 0, 2, 1 };

    /* z 1 then z 2: tree order and z order agree. */
    spec_init(&spec[0], -1, 400.0f, 200.0f);
    spec_init(&spec[1], 0, 100.0f, 100.0f);
    spec_init(&spec[2], 0, 100.0f, 100.0f);
    set_absolute(&spec[1], 0.0f, 0.0f, 100.0f, 100.0f);
    set_absolute(&spec[2], 0.0f, 0.0f, 100.0f, 100.0f);
    spec[1].z_index = 1;
    spec[2].z_index = 2;

    core = build_core(spec, 3u, &document);
    if (!check("siblings: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, forward, 3, "siblings: z 1 paints before z 2");
    check("siblings: topmost z wins the hit", wn_core_node_at(core, 50.0f, 50.0f) == 2,
          "node_at(50,50)");
    wn_core_destroy(core);
    wn_document_destroy(document);

    /* Swapped tree order: z still decides (not the DOM order). */
    spec[1].z_index = 2;
    spec[2].z_index = 1;
    document = NULL;
    core = build_core(spec, 3u, &document);
    if (!check("siblings swapped: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, swapped, 3, "siblings: z 2 paints after z 1 despite DOM order");
    check("siblings swapped: topmost z wins the hit",
          wn_core_node_at(core, 50.0f, 50.0f) == 1, "node_at(50,50)");
    wn_core_destroy(core);
    wn_document_destroy(document);
}

static void run_auto_zero(void)
{
    NodeSpec spec[3];
    wn_document *document = NULL;
    wn_core *core;
    static const int auto_then_zero[] = { 0, 1, 2 };

    /* auto first, zero second: both are positioned, so both are step 8 blocks
     * and tree order decides. */
    spec_init(&spec[0], -1, 400.0f, 200.0f);
    spec_init(&spec[1], 0, 100.0f, 100.0f);
    spec_init(&spec[2], 0, 100.0f, 100.0f);
    set_absolute(&spec[1], 0.0f, 0.0f, 100.0f, 100.0f);
    set_absolute(&spec[2], 0.0f, 0.0f, 100.0f, 100.0f);
    spec[1].z_index = R2_Z_INDEX_AUTO;
    spec[2].z_index = 0;

    core = build_core(spec, 3u, &document);
    if (!check("auto/zero: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, auto_then_zero, 3,
                "auto/zero: z auto then z 0 in tree order");
    check("auto/zero: the later z 0 is above z auto",
          wn_core_node_at(core, 50.0f, 50.0f) == 2, "node_at(50,50)");
    wn_core_destroy(core);
    wn_document_destroy(document);

    /* zero first in the DOM, auto second: CSS step 8 paints both in tree
     * order, so the later positioned z auto box wins. This is the regression
     * for the old behaviour that put z auto in the in-flow phase. */
    spec[1].z_index = 0;
    spec[2].z_index = R2_Z_INDEX_AUTO;
    document = NULL;
    core = build_core(spec, 3u, &document);
    if (!check("auto/zero swapped: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, auto_then_zero, 3,
                "auto/zero swapped: z 0 then z auto in tree order");
    check("auto/zero swapped: the later z auto is above z 0",
          wn_core_node_at(core, 50.0f, 50.0f) == 2, "node_at(50,50)");
    wn_core_destroy(core);
    wn_document_destroy(document);
}

static void run_negative(void)
{
    NodeSpec spec[3];
    wn_document *document = NULL;
    wn_core *core;
    static const int expected[] = { 0, 2, 1 };

    spec_init(&spec[0], -1, 400.0f, 200.0f);
    spec_init(&spec[1], 0, 100.0f, 50.0f);   /* in-flow box */
    spec_init(&spec[2], 0, 100.0f, 50.0f);   /* negative absolute */
    set_absolute(&spec[2], 0.0f, 0.0f, 100.0f, 50.0f);
    spec[2].z_index = -1;

    core = build_core(spec, 3u, &document);
    if (!check("negative: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, expected, 3,
                "negative: -1 context paints behind in-flow content");
    check("negative: the in-flow box wins the hit",
          wn_core_node_at(core, 50.0f, 25.0f) == 1, "node_at(50,25)");
    wn_core_destroy(core);
    wn_document_destroy(document);
}

static void run_escape(void)
{
    NodeSpec spec[4];
    wn_document *document = NULL;
    wn_core *core;
    static const int expected[] = { 0, 1, 2, 3 };
    static const int flags[] = { 0, 0, 0, 0 };

    spec_init(&spec[0], -1, 400.0f, 200.0f);
    spec_init(&spec[1], 0, 100.0f, 100.0f);  /* parent context z 1 */
    spec_init(&spec[2], 1, 80.0f, 80.0f);    /* child z 100 */
    spec_init(&spec[3], 0, 80.0f, 80.0f);    /* root sibling z 2 */
    set_absolute(&spec[1], 0.0f, 0.0f, 100.0f, 100.0f);
    set_absolute(&spec[2], 10.0f, 10.0f, 80.0f, 80.0f);
    set_absolute(&spec[3], 10.0f, 10.0f, 80.0f, 80.0f);
    spec[1].z_index = 1;
    spec[2].z_index = 100;
    spec[3].z_index = 2;

    core = build_core(spec, 4u, &document);
    if (!check("escape: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, expected, 4,
                "escape: parent z 1 block stays before sibling z 2");
    check("escape: the high z child cannot escape the parent context",
          wn_core_node_at(core, 50.0f, 50.0f) == 3, "node_at(50,50)");
    check("escape: z contexts use ordering layers, not textures",
          layer_flags_are(core, flags, 4), "root=0, three z layers=0");
    wn_core_destroy(core);
    wn_document_destroy(document);
}

static void run_opacity_z(void)
{
    NodeSpec spec[4];
    wn_document *document = NULL;
    wn_core *core;
    const wn_display_list *list;
    static const int expected[] = { 0, 1, 2, 3 };
    static const int flags[] = { 0, 1, 0 };

    spec_init(&spec[0], -1, 400.0f, 200.0f);
    spec_init(&spec[1], 0, 100.0f, 100.0f);  /* group z 2 */
    spec_init(&spec[2], 1, 50.0f, 50.0f);    /* in-flow group child */
    spec_init(&spec[3], 0, 100.0f, 100.0f);  /* z 3 box */
    set_absolute(&spec[1], 0.0f, 0.0f, 100.0f, 100.0f);
    set_absolute(&spec[3], 0.0f, 0.0f, 100.0f, 100.0f);
    spec[1].opacity = 0.5f;
    spec[1].z_index = 2;
    spec[3].z_index = 3;

    core = build_core(spec, 4u, &document);
    if (!check("opacity+z: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, expected, 4,
                "opacity+z: group block (with child) then the higher z box");
    check("opacity+z: group has a texture layer, z box only ordering",
          layer_flags_are(core, flags, 3), "root=0, group=1, z=0");

    list = wn_core_display_list(core);
    check("opacity+z: the display list replays the ordered path",
          list != NULL && list->command_count > 0 && list->draw_count == 4,
          "commands/draws");
    check("opacity+z: the topmost z wins the hit",
          wn_core_node_at(core, 50.0f, 50.0f) == 3, "node_at(50,50)");
    wn_core_destroy(core);
    wn_document_destroy(document);
}

static void run_nested(void)
{
    NodeSpec spec[4];
    wn_document *document = NULL;
    wn_core *core;
    static const int expected[] = { 0, 1, 2, 3 };

    spec_init(&spec[0], -1, 400.0f, 200.0f);
    spec_init(&spec[1], 0, 200.0f, 200.0f);  /* A z 1 */
    spec_init(&spec[2], 1, 100.0f, 100.0f);  /* B z 10 inside A */
    spec_init(&spec[3], 0, 150.0f, 150.0f);  /* D z 5 at the root */
    set_absolute(&spec[1], 0.0f, 0.0f, 200.0f, 200.0f);
    set_absolute(&spec[2], 20.0f, 20.0f, 100.0f, 100.0f);
    set_absolute(&spec[3], 10.0f, 10.0f, 150.0f, 150.0f);
    spec[1].z_index = 1;
    spec[2].z_index = 10;
    spec[3].z_index = 5;

    core = build_core(spec, 4u, &document);
    if (!check("nested: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, expected, 4,
                "nested: A (with B) composes as one block before D");
    check("nested: D z 5 is above B z 10 inside A z 1",
          wn_core_node_at(core, 60.0f, 60.0f) == 3, "node_at(60,60)");
    wn_core_destroy(core);
    wn_document_destroy(document);
}

static void run_relative_and_static(void)
{
    NodeSpec spec[3];
    wn_document *document = NULL;
    wn_core *core;
    static const int relative_order[] = { 0, 2, 1 };
    static const int static_order[] = { 0, 1, 2 };

    /* The relative z 1 box comes first in the DOM and its offset makes it
     * overlap the later static sibling. Step 3 paints the static sibling,
     * step 4 paints the positioned box above it. */
    spec_init(&spec[0], -1, 400.0f, 200.0f);
    spec_init(&spec[1], 0, 100.0f, 100.0f);
    spec_init(&spec[2], 0, 100.0f, 100.0f);
    spec[1].position = R2_POSITION_RELATIVE;
    spec[1].inset[0] = 60.0f;    /* top: 60px -> overlaps the later box */
    spec[1].inset_mask = 0b0001u;
    spec[1].z_index = 1;

    core = build_core(spec, 3u, &document);
    if (!check("relative: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, relative_order, 3,
                "relative: first-in-DOM z 1 paints after the later static box");
    check("relative: the relative z box wins the hit over the later box",
          wn_core_node_at(core, 50.0f, 120.0f) == 1, "node_at(50,120)");
    wn_core_destroy(core);
    wn_document_destroy(document);

    /* Static z 5 is ignored: the absolute z 2 still paints on top. */
    spec_init(&spec[0], -1, 400.0f, 200.0f);
    spec_init(&spec[1], 0, 100.0f, 100.0f);
    spec_init(&spec[2], 0, 100.0f, 100.0f);
    spec[1].z_index = 5;         /* static: not a context */
    set_absolute(&spec[2], 0.0f, 0.0f, 100.0f, 100.0f);
    spec[2].z_index = 2;
    document = NULL;
    core = build_core(spec, 3u, &document);
    if (!check("static z: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, static_order, 3,
                "static z: ignored, in-flow box then the z 2 context");
    check("static z: the positioned z 2 box is on top",
          wn_core_node_at(core, 50.0f, 50.0f) == 2, "node_at(50,50)");
    wn_core_destroy(core);
    wn_document_destroy(document);
}

/* CSS Flexbox/Grid: an explicit z-index on a static item creates a stacking
 * context, unlike the same value on an ordinary static block. */
static void run_static_flex_grid_items(void)
{
    NodeSpec spec[4];
    wn_document *document = NULL;
    wn_core *core;
    static const int forward[] = { 0, 1, 2 };
    static const int swapped[] = { 0, 2, 1 };

    /* Flex row: z 1 then z 2. The negative left margin makes the boxes
     * overlap so the hit-test is meaningful. */
    spec_init(&spec[0], -1, 200.0f, 100.0f);
    spec_init(&spec[1], 0, 100.0f, 100.0f);
    spec_init(&spec[2], 0, 100.0f, 100.0f);
    spec[0].kind = R2_NODE_FLEX;
    spec[1].z_index = 1;
    spec[1].margin[3] = 0.0f;
    spec[2].z_index = 2;
    spec[2].margin[3] = -50.0f;

    core = build_core(spec, 3u, &document);
    if (!check("flex item z: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, forward, 3, "flex item z: z 1 paints before z 2");
    check("flex item z: the higher static item wins the hit",
          wn_core_node_at(core, 60.0f, 10.0f) == 2, "node_at(60,10)");
    wn_core_destroy(core);
    wn_document_destroy(document);

    /* Swapped z: the earlier flex item is now on top despite DOM order. */
    spec[1].z_index = 2;
    spec[2].z_index = 1;
    document = NULL;
    core = build_core(spec, 3u, &document);
    if (!check("flex item z swapped: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, swapped, 3,
                "flex item z swapped: z 2 paints after z 1 despite DOM order");
    check("flex item z swapped: the earlier item wins the hit",
          wn_core_node_at(core, 60.0f, 10.0f) == 1, "node_at(60,10)");
    wn_core_destroy(core);
    wn_document_destroy(document);

    /* Nested context: a z 100 absolute inside the z 1 flex item is trapped
     * below the sibling flex item with z 2. */
    spec_init(&spec[0], -1, 200.0f, 100.0f);
    spec_init(&spec[1], 0, 100.0f, 100.0f);
    spec_init(&spec[2], 1, 80.0f, 80.0f);
    spec_init(&spec[3], 0, 100.0f, 100.0f);
    spec[0].kind = R2_NODE_FLEX;
    spec[1].z_index = 1;
    set_absolute(&spec[2], 0.0f, 0.0f, 80.0f, 80.0f);
    spec[2].z_index = 100;
    spec[3].z_index = 2;
    spec[3].margin[3] = -50.0f;
    document = NULL;
    core = build_core(spec, 4u, &document);
    if (!check("flex item nested z: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    {
        static const int trapped[] = { 0, 1, 2, 3 };
        nodes_order(core, trapped, 4,
                    "flex item nested z: the z 100 child cannot escape z 1");
    }
    check("flex item nested z: the sibling z 2 wins the hit",
          wn_core_node_at(core, 60.0f, 10.0f) == 3, "node_at(60,10)");
    wn_core_destroy(core);
    wn_document_destroy(document);

    /* Grid items: same rule, and z orders them even though tracks do not
     * overlap. z 2 then z 1 in the DOM paints z 1 first. */
    spec_init(&spec[0], -1, 200.0f, 50.0f);
    spec_init(&spec[1], 0, 100.0f, 50.0f);
    spec_init(&spec[2], 0, 100.0f, 50.0f);
    spec[0].kind = R2_NODE_GRID;
    spec[0].grid_mode = R2_GRID_AUTO_FILL;
    spec[0].grid_column_px = 100.0f;
    spec[1].z_index = 2;
    spec[2].z_index = 1;
    document = NULL;
    core = build_core(spec, 3u, &document);
    if (!check("grid item z: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, swapped, 3,
                "grid item z: z 1 paints before the earlier z 2");
    wn_core_destroy(core);
    wn_document_destroy(document);
}

/* CSS Color 3 section 3.2: a non-positioned `opacity < 1` group lives in the
 * same layer as a positioned `z: 0` box (CSS step 8), not in the in-flow
 * content. e4b9c36 placed it there; ec15ce3 moved it into step 3 and broke
 * these orders. A positioned z auto item is also step 8, so tree order decides
 * between it and a static group. */
static int texture_layer_count(const wn_core *core);

static void run_static_group_step8(void)
{
    NodeSpec spec[3];
    wn_document *document = NULL;
    wn_core *core;
    static const int group_after_late_sibling[] = { 0, 2, 1 };
    static const int step8_tree_order[] = { 0, 1, 2 };

    /* 1) Static group first, later in-flow sibling: the group is step 8, so it
     * paints last. */
    spec_init(&spec[0], -1, 400.0f, 200.0f);
    spec_init(&spec[1], 0, 100.0f, 100.0f);
    spec_init(&spec[2], 0, 100.0f, 100.0f);
    spec[1].opacity = 0.5f;

    core = build_core(spec, 3u, &document);
    if (!check("static group: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, group_after_late_sibling, 3,
                "static group: paints after the later in-flow sibling");
    check("static group: the group allocates a texture",
          texture_layer_count(core) == 1, "one opacity target");
    wn_core_destroy(core);
    wn_document_destroy(document);

    /* 2) Positioned z auto first, static group later: both are step 8, so tree
     * order puts the group above the absolute. */
    spec_init(&spec[0], -1, 400.0f, 200.0f);
    spec_init(&spec[1], 0, 100.0f, 100.0f);
    spec_init(&spec[2], 0, 100.0f, 100.0f);
    set_absolute(&spec[1], 0.0f, 0.0f, 100.0f, 100.0f);
    spec[2].opacity = 0.5f;

    document = NULL;
    core = build_core(spec, 3u, &document);
    if (!check("abs then group: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, step8_tree_order, 3,
                "abs then group: absolute z auto before the later group");
    check("abs then group: the static group wins the overlap",
          wn_core_node_at(core, 50.0f, 50.0f) == 2, "node_at(50,50)");
    wn_core_destroy(core);
    wn_document_destroy(document);

    /* 3) Static group first, positioned z auto later: tree order puts the
     * absolute above the group. */
    spec_init(&spec[0], -1, 400.0f, 200.0f);
    spec_init(&spec[1], 0, 100.0f, 100.0f);
    spec_init(&spec[2], 0, 100.0f, 100.0f);
    set_absolute(&spec[2], 0.0f, 0.0f, 100.0f, 100.0f);
    spec[1].opacity = 0.5f;

    document = NULL;
    core = build_core(spec, 3u, &document);
    if (!check("group then abs: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, step8_tree_order, 3,
                "group then abs: the group before the later absolute z auto");
    check("group then abs: the later absolute wins the overlap",
          wn_core_node_at(core, 50.0f, 50.0f) == 2, "node_at(50,50)");
    wn_core_destroy(core);
    wn_document_destroy(document);
}

static void run_same_z(void)
{
    NodeSpec spec[3];
    wn_document *document = NULL;
    wn_core *core;
    static const int expected[] = { 0, 1, 2 };

    /* Two siblings with the same z: the comparator's explicit tree-order
     * tie-break decides. Left box first in the DOM. */
    spec_init(&spec[0], -1, 400.0f, 200.0f);
    spec_init(&spec[1], 0, 100.0f, 100.0f);
    spec_init(&spec[2], 0, 100.0f, 100.0f);
    set_absolute(&spec[1], 0.0f, 0.0f, 100.0f, 100.0f);
    set_absolute(&spec[2], 30.0f, 0.0f, 100.0f, 100.0f);
    spec[1].z_index = 1;
    spec[2].z_index = 1;

    core = build_core(spec, 3u, &document);
    if (!check("same z: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, expected, 3, "same z: left then right in tree order");
    check("same z: the later sibling wins the overlap",
          wn_core_node_at(core, 50.0f, 50.0f) == 2, "node_at(50,50)");
    check("same z: the earlier sibling still owns its own area",
          wn_core_node_at(core, 10.0f, 50.0f) == 1, "node_at(10,50)");
    wn_core_destroy(core);
    wn_document_destroy(document);

    /* Reversed DOM order with the same z: the right box is first, so the left
     * box is the later sibling and paints on top. */
    set_absolute(&spec[1], 30.0f, 0.0f, 100.0f, 100.0f);
    set_absolute(&spec[2], 0.0f, 0.0f, 100.0f, 100.0f);
    document = NULL;
    core = build_core(spec, 3u, &document);
    if (!check("same z reversed: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, expected, 3,
                "same z reversed: right then left in tree order");
    check("same z reversed: the later left box wins the overlap",
          wn_core_node_at(core, 50.0f, 50.0f) == 2, "node_at(50,50)");
    check("same z reversed: the earlier right box owns its own area",
          wn_core_node_at(core, 115.0f, 50.0f) == 1, "node_at(115,50)");
    wn_core_destroy(core);
    wn_document_destroy(document);
}

/* True when the item list carries `node` with a non-empty clip. */
static int item_has_node(const wn_core *core, int node)
{
    int i;
    for (i = 0; i < core->paint_item_count; ++i) {
        if (core->paint_items[i].node == node &&
            !rect_is_empty(core->paint_items[i].clip)) {
            return 1;
        }
    }
    return 0;
}

static int texture_layer_count(const wn_core *core)
{
    int i;
    int count = 0;
    for (i = 0; i < core->paint_layer_count; ++i) {
        if (core->paint_layers[i].needs_texture) {
            ++count;
        }
    }
    return count;
}

/* F1: a group under a zero-height clipped ancestor has an empty clip, so it
 * must not allocate an opacity texture. Its escaping absolute keeps its own
 * non-empty clip and paints. The unrelated opacity div switches the page
 * between the flat and ordered paths in the old code; both builds must paint
 * and hit the escaping absolute. */
static void run_empty_clip_group(int with_unrelated)
{
    NodeSpec spec[5];
    wn_document *document = NULL;
    wn_core *core;
    uint32_t count = with_unrelated ? 5u : 4u;
    const char *label = with_unrelated ? "empty clip + unrelated group"
                                       : "empty clip alone";

    spec_init(&spec[0], -1, 400.0f, 200.0f);
    spec_init(&spec[1], 0, 100.0f, 0.0f);    /* clipper: height 0 */
    spec_init(&spec[2], 1, 100.0f, 50.0f);   /* group under the clipper */
    spec_init(&spec[3], 2, 50.0f, 50.0f);    /* escaping absolute */
    spec_init(&spec[4], 0, 50.0f, 50.0f);    /* unrelated group */
    spec[1].overflow = R2_OVERFLOW_HIDDEN;
    spec[2].opacity = 0.5f;
    set_absolute(&spec[3], 10.0f, 10.0f, 50.0f, 50.0f);
    if (with_unrelated) {
        set_absolute(&spec[4], 250.0f, 100.0f, 50.0f, 50.0f);
        spec[4].opacity = 0.5f;
    }

    core = build_core(spec, count, &document);
    if (!check("empty clip: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    check("empty clip: the escaping absolute is in the paint list",
          item_has_node(core, 3), label);
    check("empty clip: the escaping absolute wins the hit",
          wn_core_node_at(core, 20.0f, 20.0f) == 3, label);
    check("empty clip: the empty-clip group allocates no texture",
          texture_layer_count(core) == (with_unrelated ? 1 : 0),
          with_unrelated ? "only the unrelated group has one"
                         : "no texture layers");
    wn_core_destroy(core);
    wn_document_destroy(document);
}

static void run_flat_path(void)
{
    NodeSpec spec[3];
    wn_document *document = NULL;
    wn_core *core;
    const wn_display_list *list;
    static const int expected[] = { 0, 1, 2 };

    spec_init(&spec[0], -1, 400.0f, 200.0f);
    spec_init(&spec[1], 0, 100.0f, 50.0f);
    spec_init(&spec[2], 0, 100.0f, 50.0f);

    core = build_core(spec, 3u, &document);
    if (!check("flat: core built", core != NULL,
               core != NULL ? NULL : wn_last_error())) {
        return;
    }
    nodes_order(core, expected, 3,
                "flat: pages without opacity/z/positioning keep DFS order");
    list = wn_core_display_list(core);
    check("flat: no layers and no commands (historical path)",
          list != NULL && list->layer_count == 0 && list->command_count == 0 &&
              list->draws == NULL,
          "layer_count=0 command_count=0 draws=NULL");
    wn_core_destroy(core);
    wn_document_destroy(document);
}

static void run_loader(void)
{
    NodeSpec spec[5];
    size_t size = 0;
    unsigned char *data;
    wn_document *document;

    spec_init(&spec[0], -1, 400.0f, 200.0f);
    spec_init(&spec[1], 0, 10.0f, 10.0f);
    spec_init(&spec[2], 0, 10.0f, 10.0f);
    spec_init(&spec[3], 0, 10.0f, 10.0f);
    spec_init(&spec[4], 0, 10.0f, 10.0f);
    spec[1].z_index = R2_Z_INDEX_AUTO;
    spec[2].z_index = R2_Z_INDEX_AUTO + 1; /* most negative explicit value */
    spec[3].z_index = 0;
    spec[4].z_index = INT32_MAX;           /* most positive explicit value */

    data = build_blob(spec, 5u, 400u, 200u, &size);
    document = data != NULL ? wn_document_load_blob_memory(data, size) : NULL;
    if (!check("loader: v5 blob loads", document != NULL,
               document != NULL ? NULL : wn_last_error())) {
        free(data);
        return;
    }
    check("loader: auto sentinel survives",
          document->nodes[1].z_index == R2_Z_INDEX_AUTO, "INT32_MIN");
    check("loader: int32 extremes and zero survive",
          document->nodes[2].z_index == R2_Z_INDEX_AUTO + 1 &&
              document->nodes[3].z_index == 0 &&
              document->nodes[4].z_index == INT32_MAX,
          "INT32_MIN+1, 0, INT32_MAX");
    wn_document_destroy(document);
    free(data);
}

int main(void)
{
    run_siblings();
    run_auto_zero();
    run_negative();
    run_escape();
    run_opacity_z();
    run_nested();
    run_same_z();
    run_relative_and_static();
    run_static_flex_grid_items();
    run_static_group_step8();
    run_empty_clip_group(0);
    run_empty_clip_group(1);
    run_flat_path();
    run_loader();

    if (failures != 0) {
        printf("zindex_test: FAIL (%d/%d checks failed)\n", failures, checks);
        return 1;
    }
    printf("zindex_test: PASS (%d checks)\n", checks);
    return 0;
}
