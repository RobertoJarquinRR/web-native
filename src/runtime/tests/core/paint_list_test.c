/*
 * paint_list_test.c - the ordered paint item list and its identity contract.
 *
 * A five-node document is built directly (no blob, no GPU):
 *
 *   root BLOCK 200x120 (no background)
 *     block A  40x30  #112233
 *     block B  40x30  #445566
 *     text     "A"    fake backend (one 2x2 glyph, 6 vertices)
 *     image    20x20  src "a.png" (geometry only; the file is never loaded)
 *
 * What is locked here:
 *   1. Item build/order: items are the visible nodes with a non-empty clip, in
 *      DFS order; defaults are layer 0, opacity 1, identity transform, and
 *      clip == clips[node].
 *   2. Identity is byte-identical: same solid/glyph/image counts and bytes as
 *      the known pre-list geometry, and a second build is identical.
 *   3. A forced translate moves solid, glyph and image vertices by exactly
 *      (dx, dy) in view pixels.
 *   4. Opacity boundaries on solid and glyph: 0/negative drop the vertices,
 *      1 keeps them, >1 clamps, 0.5 rounds to 128. Images are untouched
 *      (documented pending: wn_image_vertex has no alpha).
 *   5. Layers: distinct layers sort stably in O(n log n); an all-equal list
 *      takes the no-copy fast path and keeps the exact DFS order.
 *   6. The culling helper decides correctly, and a real .wnui fixture locks the
 *      build decision: a child fully below the viewport but casting a shadow
 *      into it is still built, so enabling culling by the helper would lose
 *      the shadow. Culling is computed, never applied.
 *   7. Visibility rebuilds the item list.
 *
 *   paint_list_test.exe
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/wn_core_internal.h"
#include "document/wn_blob.h"
#include "document/wn_document_format.h"
#include "document/wn_document_internal.h"
#include "text/text_runtime.h"
#include "wn_core.h"

/* Provided by fake_text.c. */
const wn_text_ops *wn_text_fake(void);

#define FAKE "fake"
#define FAKE_SIZE 16.0f
#define VIEW_W 200.0f
#define VIEW_H 120.0f

#define SOLID_VERTS 12 /* block A + block B, 6 each */
#define GLYPH_VERTS 6  /* one fake glyph */
#define IMAGE_VERTS 6  /* square image, no radius */
#define ITEM_COUNT 5

#define COLOR_A 0xFF112233u
#define COLOR_B 0xFF445566u
#define COLOR_TEXT 0xFFFF8040u

static int checks = 0;
static int failures = 0;

static int check(const char *label, int ok)
{
    checks += 1;
    if (!ok) {
        failures += 1;
    }
    printf("  %s %s\n", ok ? "[ok]" : "[FAIL]", label);
    return ok;
}

static int near(float a, float b)
{
    return fabsf(a - b) < 0.001f;
}

static int transform_is_identity(const wn_transform *t)
{
    return t->m0 == 1.0f && t->m1 == 0.0f && t->m2 == 0.0f &&
           t->m3 == 1.0f && t->m4 == 0.0f && t->m5 == 0.0f;
}

/* Direct static document: the core copies the node table, so the same staging
 * arrays survive every rebuild. */
static wn_document g_doc;
static UiNode g_nodes[ITEM_COUNT];
static const char *const g_names[ITEM_COUNT] = {
    "body.root", "body.root/div.a", "body.root/div.b",
    "body.root/span.text", "body.root/img.poster"
};

static wn_core *make_core(void)
{
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(&g_doc, 0, sizeof(g_doc));

    g_nodes[0].parent = -1;
    g_nodes[0].kind = R2_NODE_BLOCK;
    g_nodes[0].width = VIEW_W;
    g_nodes[0].height = VIEW_H;
    g_nodes[0].flex_shrink = 1.0f;
    g_nodes[0].flex_basis = -1.0f;

    g_nodes[1].parent = 0;
    g_nodes[1].kind = R2_NODE_BLOCK;
    g_nodes[1].width = 40.0f;
    g_nodes[1].height = 30.0f;
    g_nodes[1].background = COLOR_A;
    g_nodes[1].font_size = FAKE_SIZE;
    g_nodes[1].flex_shrink = 1.0f;
    g_nodes[1].flex_basis = -1.0f;

    g_nodes[2].parent = 0;
    g_nodes[2].kind = R2_NODE_BLOCK;
    g_nodes[2].width = 40.0f;
    g_nodes[2].height = 30.0f;
    g_nodes[2].background = COLOR_B;
    g_nodes[2].font_size = FAKE_SIZE;
    g_nodes[2].flex_shrink = 1.0f;
    g_nodes[2].flex_basis = -1.0f;

    g_nodes[3].parent = 0;
    g_nodes[3].kind = R2_NODE_TEXT;
    g_nodes[3].text = "A";
    g_nodes[3].font_family = FAKE;
    g_nodes[3].font_size = FAKE_SIZE;
    g_nodes[3].font_weight = 400;
    g_nodes[3].text_color = COLOR_TEXT;
    g_nodes[3].width = 80.0f;
    g_nodes[3].height = 20.0f;
    g_nodes[3].flex_shrink = 1.0f;
    g_nodes[3].flex_basis = -1.0f;

    g_nodes[4].parent = 0;
    g_nodes[4].kind = R2_NODE_IMAGE;
    g_nodes[4].width = 20.0f;
    g_nodes[4].height = 20.0f;
    g_nodes[4].image_src = "a.png";
    g_nodes[4].image_width = 10.0f;
    g_nodes[4].image_height = 10.0f;
    g_nodes[4].object_fit = R2_OBJECT_FIT_FILL;
    g_nodes[4].font_size = FAKE_SIZE;
    g_nodes[4].flex_shrink = 1.0f;
    g_nodes[4].flex_basis = -1.0f;

    g_doc.nodes = g_nodes;
    g_doc.node_count = ITEM_COUNT;
    g_doc.names = g_names;
    g_doc.viewport_width = (int)VIEW_W;
    g_doc.viewport_height = (int)VIEW_H;
    g_doc.refs = WN_DOCUMENT_STATIC_REFS;
    return wn_core_create(&g_doc);
}

/* ------------------------------------------- culling fixture (.wnui blob)
 *
 * root BLOCK 200x120 border-box, padding-top 130 -> its only child starts at
 * y=130, fully below the 200x120 viewport. The root is overflow:visible, so
 * the child's inherited clip is the viewport and non-empty. The child has no
 * background and one shadow offset -50px, so the 30px box (130..160) casts a
 * 3-layer shadow over 80..110: inside the viewport. If the item build ever
 * culls by wn_paint_item_visible(), the child item disappears and the 18
 * shadow vertices vanish with it. */
#define CULL_VIEW_W 200.0f
#define CULL_VIEW_H 120.0f
#define SHADOW_VERTS 18 /* three radius-0 shadow quads, 6 vertices each */

static const char kCullStrings[] = "\0cull.root\0cull.shadow\0";

static uint32_t align_up8(uint32_t value)
{
    return (value + 7u) & ~7u;
}

static unsigned char *build_cull_blob(size_t *out_size)
{
    const uint32_t node_count = 2u;
    const uint32_t section_bytes = 3u * (uint32_t)sizeof(WnBlobSection);
    const uint32_t nodes_offset = align_up8(WN_BLOB_HEADER_SIZE + section_bytes);
    const uint32_t nodes_bytes = node_count * (uint32_t)sizeof(WnBlobNode);
    const uint32_t names_offset = align_up8(nodes_offset + nodes_bytes);
    const uint32_t names_bytes = node_count * (uint32_t)sizeof(uint32_t);
    const uint32_t strings_offset = align_up8(names_offset + names_bytes);
    const uint32_t strings_bytes = (uint32_t)sizeof(kCullStrings);
    const uint32_t total = align_up8(strings_offset + strings_bytes);
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
    header->viewport_width = (uint32_t)CULL_VIEW_W;
    header->viewport_height = (uint32_t)CULL_VIEW_H;

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
    nodes[0].width = CULL_VIEW_W;
    nodes[0].height = CULL_VIEW_H;
    nodes[0].padding[0] = CULL_VIEW_H + 10.0f; /* pushes the child to y=130 */
    nodes[0].box_sizing = 1;                   /* 120px box, padding included */
    nodes[0].font_size = FAKE_SIZE;
    nodes[0].flex_shrink = 1.0f;
    nodes[0].flex_basis = -1.0f;

    nodes[1].parent = 0;
    nodes[1].kind = R2_NODE_BLOCK;
    nodes[1].width = 40.0f;
    nodes[1].height = 30.0f;
    nodes[1].shadow_x = 0.0f;
    nodes[1].shadow_y = -50.0f; /* 130..160 box -> 80..110 shadow */
    nodes[1].shadow_blur = 0.0f;
    nodes[1].shadow_spread = 0.0f;
    nodes[1].shadow_color = 0xFF000000u;
    nodes[1].font_size = FAKE_SIZE;
    nodes[1].flex_shrink = 1.0f;
    nodes[1].flex_basis = -1.0f;

    name_offsets = (uint32_t *)(data + names_offset);
    name_offsets[0] = 0u;
    name_offsets[1] = 1u;

    strings = (char *)(data + strings_offset);
    memcpy(strings, kCullStrings, sizeof(kCullStrings));

    *out_size = total;
    return data;
}

static int item_index_for_node(const wn_core *c, int node)
{
    int i;
    for (i = 0; i < c->paint_item_count; ++i) {
        if (c->paint_items[i].node == node) {
            return i;
        }
    }
    return -1;
}

static float view_x(const wn_display_list *list, float clip_x)
{
    return (clip_x + 1.0f) * 0.5f * (float)list->width;
}

static float view_y(const wn_display_list *list, float clip_y)
{
    return (1.0f - clip_y) * 0.5f * (float)list->height;
}

/* Copies the three geometry arrays so a modified build can be compared against
 * the identity build byte for byte. */
typedef struct {
    wn_vertex solids[32];
    wn_glyph_vertex glyphs[32];
    wn_image_vertex images[32];
} Geometry;

static void capture(const wn_display_list *list, Geometry *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->solids, list->vertices, (size_t)list->vertex_count * sizeof(wn_vertex));
    memcpy(out->glyphs, list->glyph_vertices,
           (size_t)list->glyph_vertex_count * sizeof(wn_glyph_vertex));
    memcpy(out->images, list->image_vertices,
           (size_t)list->image_vertex_count * sizeof(wn_image_vertex));
}

static void reset_items(wn_core *c)
{
    wn_core_relayout(c);
    wn_core_build_display_list(c);
}

int main(void)
{
    wn_core *core;
    const wn_display_list *list;
    Geometry identity;
    Geometry copy;
    int i;

    wn_text_test_use_backend(wn_text_fake());
    core = make_core();
    if (!check("core is created", core != NULL)) {
        printf("paint_list_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }
    list = wn_core_display_list(core);

    /* --------------------------------------------------- 1. item count/order */
    check("one item per visible node", core->paint_item_count == ITEM_COUNT);
    {
        int ok = core->paint_item_count == ITEM_COUNT;
        for (i = 0; ok && i < ITEM_COUNT; ++i) {
            ok = core->paint_items[i].node == i;
        }
        check("items keep DFS order", ok);
    }
    {
        int ok = 1;
        for (i = 0; i < core->paint_item_count; ++i) {
            const WnPaintItem *item = &core->paint_items[i];
            ok = ok && item->layer == 0 && item->opacity == 1.0f &&
                 transform_is_identity(&item->transform) &&
                 item->clip.x == core->clips[item->node].x &&
                 item->clip.y == core->clips[item->node].y &&
                 item->clip.w == core->clips[item->node].w &&
                 item->clip.h == core->clips[item->node].h;
        }
        check("clip/layer/opacity/transform defaults", ok);
    }

    /* ------------------------------------------------ 2. identity bytes */
    check("identity solid count", list->vertex_count == SOLID_VERTS);
    check("identity glyph count", list->glyph_vertex_count == GLYPH_VERTS);
    check("identity image count", list->image_vertex_count == IMAGE_VERTS);
    capture(list, &identity);
    {
        wn_vertex first = identity.solids[0];
        unsigned int rgb = ((unsigned int)first.r << 16) |
                           ((unsigned int)first.g << 8) | (unsigned int)first.b;
        check("first solid batch follows item order (A)", rgb == (COLOR_A & 0x00FFFFFFu));
    }
    wn_core_build_display_list(core);
    check("a second identity build is byte-identical",
          list->vertex_count == SOLID_VERTS &&
              memcmp(list->vertices, identity.solids,
                     (size_t)SOLID_VERTS * sizeof(wn_vertex)) == 0 &&
              memcmp(list->glyph_vertices, identity.glyphs,
                     (size_t)GLYPH_VERTS * sizeof(wn_glyph_vertex)) == 0 &&
              memcmp(list->image_vertices, identity.images,
                     (size_t)IMAGE_VERTS * sizeof(wn_image_vertex)) == 0);

    /* ------------------------------------------- 3. transform translate */
    {
        int index = item_index_for_node(core, 1);
        int ok = 1;
        core->paint_items[index].transform.m4 = 7.0f;
        core->paint_items[index].transform.m5 = -3.0f;
        wn_core_build_display_list(core);
        ok = list->vertex_count == SOLID_VERTS;
        for (i = 0; ok && i < 6; ++i) {
            ok = near(view_x(list, list->vertices[i].x),
                      view_x(list, identity.solids[i].x) + 7.0f) &&
                 near(view_y(list, list->vertices[i].y),
                      view_y(list, identity.solids[i].y) - 3.0f);
        }
        ok = ok && memcmp(list->vertices + 6, identity.solids + 6,
                          (size_t)6 * sizeof(wn_vertex)) == 0;
        check("forced solid translate moves only that item", ok);
        reset_items(core);
        capture(list, &copy);
        check("reset restores identity bytes",
              memcmp(&copy, &identity, sizeof(identity)) == 0);
    }
    {
        int index = item_index_for_node(core, 3);
        int ok = 1;
        core->paint_items[index].transform.m4 = 5.0f;
        core->paint_items[index].transform.m5 = 2.0f;
        wn_core_build_display_list(core);
        ok = list->glyph_vertex_count == GLYPH_VERTS;
        for (i = 0; ok && i < GLYPH_VERTS; ++i) {
            ok = near(view_x(list, list->glyph_vertices[i].x),
                      view_x(list, identity.glyphs[i].x) + 5.0f) &&
                 near(view_y(list, list->glyph_vertices[i].y),
                      view_y(list, identity.glyphs[i].y) + 2.0f);
        }
        check("forced glyph translate moves text vertices", ok);
        reset_items(core);
    }
    {
        int index = item_index_for_node(core, 4);
        int ok = 1;
        core->paint_items[index].transform.m4 = -4.0f;
        core->paint_items[index].transform.m5 = 6.0f;
        wn_core_build_display_list(core);
        ok = list->image_vertex_count == IMAGE_VERTS;
        for (i = 0; ok && i < IMAGE_VERTS; ++i) {
            ok = near(view_x(list, list->image_vertices[i].x),
                      view_x(list, identity.images[i].x) - 4.0f) &&
                 near(view_y(list, list->image_vertices[i].y),
                      view_y(list, identity.images[i].y) + 6.0f);
        }
        check("forced image translate moves image vertices", ok);
        reset_items(core);
    }

    /* ---------------------------------------------------- 4. opacity */
    {
        static const struct {
            float opacity;
            int alpha;
            int painted;
            const char *label;
        } solid_cases[] = {
            { 0.0f, 0, 0, "solid opacity 0 drops the item's vertices" },
            { 1.0f, 255, 1, "solid opacity 1 keeps alpha 255" },
            { 2.0f, 255, 1, "solid opacity >1 clamps to 255" },
            { -0.5f, 0, 0, "solid negative opacity drops the vertices" },
            { 0.5f, 128, 1, "solid opacity 0.5 rounds to 128" },
        };
        int c;
        for (c = 0; c < (int)(sizeof(solid_cases) / sizeof(solid_cases[0])); ++c) {
            int index = item_index_for_node(core, 1);
            int ok = 1;
            core->paint_items[index].opacity = solid_cases[c].opacity;
            wn_core_build_display_list(core);
            if (!solid_cases[c].painted) {
                ok = list->vertex_count == SOLID_VERTS - 6 &&
                     memcmp(list->vertices, identity.solids + 6,
                            (size_t)6 * sizeof(wn_vertex)) == 0;
            } else {
                ok = list->vertex_count == SOLID_VERTS;
                for (i = 0; ok && i < 6; ++i) {
                    ok = list->vertices[i].a == (uint8_t)solid_cases[c].alpha &&
                         list->vertices[i].r == 0x11 &&
                         list->vertices[i].g == 0x22 &&
                         list->vertices[i].b == 0x33;
                }
                for (i = 6; ok && i < SOLID_VERTS; ++i) {
                    ok = list->vertices[i].a == 255;
                }
            }
            check(solid_cases[c].label, ok);
            reset_items(core);
        }
    }
    {
        static const struct {
            float opacity;
            int alpha;
            int painted;
            const char *label;
        } glyph_cases[] = {
            { 0.0f, 0, 0, "glyph opacity 0 drops the text vertices" },
            { 1.0f, 255, 1, "glyph opacity 1 keeps alpha 255" },
            { 2.0f, 255, 1, "glyph opacity >1 clamps to 255" },
            { -0.5f, 0, 0, "glyph negative opacity drops the vertices" },
            { 0.5f, 128, 1, "glyph opacity 0.5 rounds to 128" },
        };
        int c;
        for (c = 0; c < (int)(sizeof(glyph_cases) / sizeof(glyph_cases[0])); ++c) {
            int index = item_index_for_node(core, 3);
            int ok = 1;
            core->paint_items[index].opacity = glyph_cases[c].opacity;
            wn_core_build_display_list(core);
            if (!glyph_cases[c].painted) {
                ok = list->glyph_vertex_count == 0;
            } else {
                ok = list->glyph_vertex_count == GLYPH_VERTS;
                for (i = 0; ok && i < GLYPH_VERTS; ++i) {
                    ok = list->glyph_vertices[i].a ==
                             (uint8_t)glyph_cases[c].alpha &&
                         list->glyph_vertices[i].r == 0xFF &&
                         list->glyph_vertices[i].g == 0x80 &&
                         list->glyph_vertices[i].b == 0x40;
                }
            }
            check(glyph_cases[c].label, ok);
            reset_items(core);
        }
    }
    {
        int index = item_index_for_node(core, 4);
        core->paint_items[index].opacity = 0.5f;
        wn_core_build_display_list(core);
        check("image opacity is pending (vertices untouched)",
              list->image_vertex_count == IMAGE_VERTS &&
                  memcmp(list->image_vertices, identity.images,
                         (size_t)IMAGE_VERTS * sizeof(wn_image_vertex)) == 0);
        reset_items(core);
    }

    /* ------------------------------------------------------ 5. layers */
    {
        int copies;
        int ok;
        /* DFS [0,1,2,3,4] with layers [0,2,1,2,0]. Stable ascending result:
         * layer 0 keeps 0 then 4; layer 1 is 2; layer 2 keeps 1 then 3. */
        core->paint_items[0].layer = 0;
        core->paint_items[1].layer = 2;
        core->paint_items[2].layer = 1;
        core->paint_items[3].layer = 2;
        core->paint_items[4].layer = 0;
        copies = wn_core_sort_paint_items(core);
        ok = core->paint_item_count == ITEM_COUNT &&
             core->paint_items[0].node == 0 &&
             core->paint_items[1].node == 4 &&
             core->paint_items[2].node == 2 &&
             core->paint_items[3].node == 1 &&
             core->paint_items[4].node == 3;
        check("distinct layers sort stably (ties keep DFS order)",
              ok && copies > 0 && core->paint_items[0].layer == 0 &&
                  core->paint_items[1].layer == 0 &&
                  core->paint_items[2].layer == 1 &&
                  core->paint_items[3].layer == 2 &&
                  core->paint_items[4].layer == 2);

        wn_core_build_display_list(core);
        ok = list->vertex_count == SOLID_VERTS;
        for (i = 0; ok && i < 6; ++i) {
            ok = list->vertices[i].r == 0x44 && list->vertices[i].g == 0x55 &&
                 list->vertices[i].b == 0x66;
        }
        for (i = 6; ok && i < SOLID_VERTS; ++i) {
            ok = list->vertices[i].r == 0x11 && list->vertices[i].g == 0x22 &&
                 list->vertices[i].b == 0x33;
        }
        check("solid batch follows the sorted layer order", ok);
        reset_items(core);
    }
    {
        WnPaintItem before[ITEM_COUNT];
        int copies;
        int ok;
        memset(before, 0, sizeof(before));
        memcpy(before, core->paint_items, sizeof(before));
        copies = wn_core_sort_paint_items(core);
        ok = copies == 0 &&
             memcmp(before, core->paint_items, sizeof(before)) == 0;
        for (i = 0; ok && i < ITEM_COUNT; ++i) {
            ok = core->paint_items[i].node == i && core->paint_items[i].layer == 0;
        }
        check("all-equal layers take the no-copy fast path (0 moves)", ok);
        check("all-equal sort keeps the exact DFS item bytes",
              memcmp(before, core->paint_items, sizeof(before)) == 0);
    }

    /* ----------------------------------------------------- 6. culling */
    {
        int index = item_index_for_node(core, 1);
        wn_rect saved = core->paint_items[index].clip;
        int onscreen = wn_paint_item_visible(core, &core->paint_items[index]);
        core->paint_items[index].transform.m4 = 1000.0f;
        check("culling helper drops an offscreen item",
              onscreen == 1 &&
                  wn_paint_item_visible(core, &core->paint_items[index]) == 0);
        core->paint_items[index].transform.m4 = 0.0f;

        core->paint_items[index].clip.w = 0.0f;
        check("culling helper drops an empty clip",
              wn_paint_item_visible(core, &core->paint_items[index]) == 0);
        core->paint_items[index].clip = saved;

        /* Partially visible: the transformed AABB still overlaps the viewport. */
        core->paint_items[index].transform.m4 = VIEW_W - 5.0f;
        check("culling helper keeps a partially visible item",
              wn_paint_item_visible(core, &core->paint_items[index]) == 1);

        /* Culling is deliberately off: the offscreen item is still built. */
        core->paint_items[index].transform.m4 = 1000.0f;
        wn_core_build_display_list(core);
        check("culling stays off: the offscreen item is still painted",
              core->paint_item_count == ITEM_COUNT &&
                  list->vertex_count == SOLID_VERTS);
        reset_items(core);
    }

    /* ----------------------------- 6b. culling build decision (.wnui fixture) */
    {
        unsigned char *data;
        size_t size = 0;
        wn_document *document = NULL;
        wn_core *cull = NULL;
        const wn_display_list *cull_list;
        int child_item;
        int ok;

        data = build_cull_blob(&size);
        if (data != NULL) {
            document = wn_document_load_blob_memory(data, size);
        }
        check("culling fixture blob built and loaded",
              data != NULL && document != NULL);
        if (document != NULL) {
            cull = wn_core_create(document);
        }
        if (!check("culling fixture core is created", cull != NULL)) {
            printf("paint_list_test: FAIL (%d checks, %d failed)\n", checks, failures);
            return 1;
        }
        wn_core_resize(cull, (int)CULL_VIEW_W, (int)CULL_VIEW_H);
        cull_list = wn_core_display_list(cull);

        check("fixture child sits fully below the viewport",
              cull->rects[1].y >= CULL_VIEW_H);
        child_item = item_index_for_node(cull, 1);
        check("build keeps the offscreen shadow caster in paint_items",
              child_item >= 0 &&
                  !rect_is_empty(cull->paint_items[child_item].clip));
        check("the culling helper would drop that item",
              child_item >= 0 &&
                  wn_paint_item_visible(cull, &cull->paint_items[child_item]) == 0);

        check("the shadow still contributes its vertices",
              cull_list->vertex_count == SHADOW_VERTS);
        ok = cull_list->vertex_count == SHADOW_VERTS;
        for (i = 0; ok && i < SHADOW_VERTS; ++i) {
            /* NDC y maps back to pixels; the shadow rect is 80..110. */
            float py = view_y(cull_list, cull_list->vertices[i].y);
            ok = py > 0.0f && py < CULL_VIEW_H;
        }
        check("the shadow vertices land inside the viewport", ok);

        wn_core_destroy(cull);
        wn_document_destroy(document);
        free(data);
    }

    /* -------------------------------------- 8. visibility rebuilds items */
    {
        int ok;
        wn_core_set_visible(core, 2, 0);
        wn_core_relayout(core);
        ok = core->paint_item_count == ITEM_COUNT - 1 &&
             core->paint_items[0].node == 0 &&
             core->paint_items[1].node == 1 &&
             core->paint_items[2].node == 3 &&
             core->paint_items[3].node == 4;
        wn_core_set_visible(core, 2, 1);
        wn_core_relayout(core);
        ok = ok && core->paint_item_count == ITEM_COUNT &&
             core->paint_items[2].node == 2;
        check("hiding a node drops its item and restores it", ok);
    }

    wn_core_destroy(core);

    if (failures != 0) {
        printf("paint_list_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }
    printf("paint_list_test: PASS (%d checks)\n", checks);
    return 0;
}
