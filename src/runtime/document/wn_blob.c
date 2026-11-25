#include <stdlib.h>
#include <string.h>

#include "webnative.h"
#include "document/wn_blob.h"
#include "document/wn_document_internal.h"
#include "wn_internal.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef struct {
    void *mapping;
    void *mapping_handle;
    const unsigned char *data;
    size_t size;
} WnBlobMap;

static int blob_map_open(WnBlobMap *map, const char *path)
{
    memset(map, 0, sizeof(*map));
#if defined(_WIN32)
    {
        HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
        LARGE_INTEGER length;
        HANDLE mapping;
        if (file == INVALID_HANDLE_VALUE) {
            return 1;
        }
        if (!GetFileSizeEx(file, &length) || length.QuadPart <= 0 ||
            (uint64_t)length.QuadPart > (uint64_t)(size_t)-1) {
            CloseHandle(file);
            return 1;
        }
        mapping = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
        CloseHandle(file);
        if (mapping == NULL) {
            return 1;
        }
        map->data = (const unsigned char *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        if (map->data == NULL) {
            CloseHandle(mapping);
            return 1;
        }
        map->mapping = (void *)map->data;
        map->mapping_handle = (void *)mapping;
        map->size = (size_t)length.QuadPart;
        return 0;
    }
#else
    {
        FILE *file = fopen(path, "rb");
        long length;
        unsigned char *buffer;
        if (file == NULL) {
            return 1;
        }
        if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
            fseek(file, 0, SEEK_SET) != 0) {
            fclose(file);
            return 1;
        }
        buffer = (unsigned char *)malloc((size_t)length);
        if (buffer == NULL || fread(buffer, 1, (size_t)length, file) != (size_t)length) {
            free(buffer);
            fclose(file);
            return 1;
        }
        fclose(file);
        map->data = buffer;
        map->mapping = buffer;
        map->size = (size_t)length;
        return 0;
    }
#endif
}

static void blob_map_close(WnBlobMap *map)
{
#if defined(_WIN32)
    if (map->mapping != NULL) {
        UnmapViewOfFile(map->mapping);
    }
    if (map->mapping_handle != NULL) {
        CloseHandle((HANDLE)map->mapping_handle);
    }
#else
    free(map->mapping);
#endif
    memset(map, 0, sizeof(*map));
}

static const WnBlobSection *find_section(const WnBlobSection *sections, uint32_t count,
                                         uint32_t id, uint32_t file_size, uint32_t *out_size)
{
    for (uint32_t i = 0; i < count; ++i) {
        const WnBlobSection *section = &sections[i];
        if (section->id != id) {
            continue;
        }
        if (section->offset > file_size || section->size > file_size - section->offset) {
            return NULL;
        }
        *out_size = section->size;
        return section;
    }
    return NULL;
}

static void document_release(wn_document *document)
{
    if (document == NULL) {
        return;
    }
    if (document->owned_nodes == NULL && document->mapping == NULL) {
        return;
    }
    free(document->owned_nodes);
    free(document->owned_names);
    free(document->owned_inputs);
    free(document->owned_glyphs);
    if (document->mapping != NULL || document->mapping_handle != NULL) {
        WnBlobMap map;
        map.mapping = document->mapping;
        map.mapping_handle = document->mapping_handle;
        map.data = NULL;
        map.size = 0;
        blob_map_close(&map);
    }
    free(document);
}

wn_document *wn_document_load_blob(const char *path)
{
    WnBlobMap map;
    const WnBlobHeader *header;
    const WnBlobSection *sections;
    const WnBlobNode *blob_nodes;
    const WnBlobSection *nodes_section;
    const WnBlobSection *names_section;
    const WnBlobSection *strings_section;
    const WnBlobSection *atlas_section;
    const WnBlobSection *inputs_section = NULL;
    const WnBlobSection *glyphs_section = NULL;
    uint32_t nodes_size = 0;
    uint32_t names_size = 0;
    uint32_t strings_size = 0;
    uint32_t atlas_size = 0;
    uint32_t inputs_size = 0;
    uint32_t glyphs_size = 0;
    const char *strings;
    wn_document *document;
    UiNode *nodes;
    const char **names;
    uint32_t i;

    if (path == NULL || blob_map_open(&map, path) != 0) {
        wn_set_error("wn_document_load_blob: cannot open %s", path != NULL ? path : "(null)");
        return NULL;
    }
    if (map.size < WN_BLOB_HEADER_SIZE) {
        wn_set_error("wn_document_load_blob: file too small");
        blob_map_close(&map);
        return NULL;
    }
    header = (const WnBlobHeader *)map.data;
    if (header->magic != WN_BLOB_MAGIC || header->version != WN_BLOB_VERSION) {
        wn_set_error("wn_document_load_blob: bad magic or version");
        blob_map_close(&map);
        return NULL;
    }
    if (header->file_size != (uint32_t)map.size || header->node_count == 0 ||
        WN_BLOB_HEADER_SIZE + (size_t)header->section_count * sizeof(WnBlobSection) > map.size) {
        wn_set_error("wn_document_load_blob: inconsistent header");
        blob_map_close(&map);
        return NULL;
    }
    sections = (const WnBlobSection *)(map.data + WN_BLOB_HEADER_SIZE);
    nodes_section = find_section(sections, header->section_count, WN_BLOB_SECTION_NODES,
                                 header->file_size, &nodes_size);
    names_section = find_section(sections, header->section_count, WN_BLOB_SECTION_NAMES,
                                 header->file_size, &names_size);
    strings_section = find_section(sections, header->section_count, WN_BLOB_SECTION_STRINGS,
                                   header->file_size, &strings_size);
    atlas_section = find_section(sections, header->section_count, WN_BLOB_SECTION_ATLAS,
                                 header->file_size, &atlas_size);
    if (nodes_section == NULL || names_section == NULL || strings_section == NULL ||
        atlas_section == NULL) {
        wn_set_error("wn_document_load_blob: missing required section");
        blob_map_close(&map);
        return NULL;
    }
    inputs_section = find_section(sections, header->section_count, WN_BLOB_SECTION_INPUTS,
                                  header->file_size, &inputs_size);
    if (inputs_section == NULL) {
        inputs_size = 0;
    }
    glyphs_section = find_section(sections, header->section_count,
                                  WN_BLOB_SECTION_INPUT_GLYPHS, header->file_size,
                                  &glyphs_size);
    if (glyphs_section == NULL) {
        glyphs_size = 0;
    }
    if (nodes_size != header->node_count * (uint32_t)sizeof(WnBlobNode) ||
        names_size != header->node_count * (uint32_t)sizeof(uint32_t) ||
        strings_size == 0 || atlas_size == 0) {
        wn_set_error("wn_document_load_blob: wrong section size");
        blob_map_close(&map);
        return NULL;
    }

    document = (wn_document *)calloc(1, sizeof(*document));
    nodes = (UiNode *)calloc(header->node_count, sizeof(UiNode));
    names = (const char **)calloc(header->node_count, sizeof(char *));
    if (document == NULL || nodes == NULL || names == NULL) {
        free(document);
        free(nodes);
        free(names);
        blob_map_close(&map);
        return NULL;
    }

    strings = (const char *)(map.data + strings_section->offset);
    blob_nodes = (const WnBlobNode *)(map.data + nodes_section->offset);
    for (i = 0; i < header->node_count; ++i) {
        const WnBlobNode *src = &blob_nodes[i];
        UiNode *dst = &nodes[i];
        dst->parent = src->parent;
        dst->kind = src->kind;
        dst->direction = src->direction;
        dst->justify = src->justify;
        dst->align = src->align;
        dst->wrap = src->wrap;
        dst->width = src->width;
        dst->height = src->height;
        dst->width_pct = src->width_pct;
        dst->height_pct = src->height_pct;
        memcpy(dst->margin, src->margin, sizeof(dst->margin));
        memcpy(dst->padding, src->padding, sizeof(dst->padding));
        dst->background = src->background;
        dst->text = src->text != 0 ? strings + src->text : NULL;
        dst->hover_bg = src->hover_bg;
        dst->press_bg = src->press_bg;
        dst->selected_bg = src->selected_bg;
        dst->action = src->action;
        dst->gap = src->gap;
        dst->font_size = src->font_size;
        dst->text_color = src->text_color;
        dst->grid_mode = src->grid_mode;
        dst->grid_column_px = src->grid_column_px;
        dst->grid_auto_rows = src->grid_auto_rows;
        dst->row_gap = src->row_gap;
        dst->column_gap = src->column_gap;
        dst->overflow = src->overflow;
        dst->image_width = src->image_width;
        dst->image_height = src->image_height;
        dst->image_src = src->image_src != 0 ? strings + src->image_src : NULL;
        dst->image_alt = src->image_alt != 0 ? strings + src->image_alt : NULL;
        dst->object_fit = src->object_fit;
        dst->border_width = src->border_width;
        dst->border_color = src->border_color;
        dst->border_radius = src->border_radius;
        dst->shadow_x = src->shadow_x;
        dst->shadow_y = src->shadow_y;
        dst->shadow_blur = src->shadow_blur;
        dst->shadow_spread = src->shadow_spread;
        dst->shadow_color = src->shadow_color;
#if defined(UI_STYLE_EXTRAS)
        dst->text_align = src->text_align;
        dst->line_height = src->line_height;
        dst->margin_auto = src->margin_auto;
        dst->box_sizing = src->box_sizing;
        dst->flex_grow = src->flex_grow;
        dst->flex_shrink = src->flex_shrink;
        dst->flex_basis = src->flex_basis;
#else
        /* Headers without E1 extras have no slots for them; the blob keeps
         * the fields but this build ignores them. */
        (void)src->text_align;
        (void)src->line_height;
#endif
        dst->font_family = src->font_family != 0 ? strings + src->font_family : NULL;
        dst->font_weight = src->font_weight;
    }
    {
        const uint32_t *name_offsets =
            (const uint32_t *)(map.data + names_section->offset);
        for (i = 0; i < header->node_count; ++i) {
            names[i] = name_offsets[i] != 0 ? strings + name_offsets[i] : "";
        }
    }

    document->nodes = nodes;
    document->node_count = (int)header->node_count;
    document->names = names;
    document->owned_nodes = nodes;
    document->owned_names = names;
    document->mapping = map.mapping;
    document->mapping_handle = map.mapping_handle;
    document->viewport_width = (int)header->viewport_width;
    document->viewport_height = (int)header->viewport_height;

    /* Atlas section: uint32 width, uint32 height, then width*height R8 bytes. */
    if (atlas_size < 8) {
        wn_set_error("wn_document_load_blob: atlas too small");
        wn_document_destroy(document);
        return NULL;
    }
    {
        const uint32_t *dims = (const uint32_t *)(map.data + atlas_section->offset);
        document->atlas_width = (int)dims[0];
        document->atlas_height = (int)dims[1];
        document->atlas = map.data + atlas_section->offset + 8;
        if (document->atlas_width <= 0 || document->atlas_height <= 0 ||
            atlas_size != 8u + (uint32_t)document->atlas_width *
                                (uint32_t)document->atlas_height) {
            wn_set_error("wn_document_load_blob: atlas size mismatch");
            wn_document_destroy(document);
            return NULL;
        }
    }

    if (inputs_section != NULL && inputs_size > 0) {
        uint32_t count = inputs_size / (uint32_t)sizeof(WnBlobInput);
        const WnBlobInput *blob_inputs =
            (const WnBlobInput *)(map.data + inputs_section->offset);
        UiInput *inputs = (UiInput *)calloc(count, sizeof(UiInput));
        if (inputs == NULL || inputs_size % sizeof(WnBlobInput) != 0) {
            free(inputs);
            wn_set_error("wn_document_load_blob: bad inputs section");
            wn_document_destroy(document);
            return NULL;
        }
        for (i = 0; i < count; ++i) {
            inputs[i].node = blob_inputs[i].node;
            inputs[i].value = blob_inputs[i].value != 0 ? strings + blob_inputs[i].value : "";
            inputs[i].placeholder = blob_inputs[i].placeholder != 0
                                        ? strings + blob_inputs[i].placeholder
                                        : NULL;
            inputs[i].max_length = blob_inputs[i].max_length;
            inputs[i].password = blob_inputs[i].password;
            inputs[i].baseline = blob_inputs[i].baseline;
            inputs[i].line_height = blob_inputs[i].line_height;
            inputs[i].glyph_first = blob_inputs[i].glyph_first;
            inputs[i].glyph_count = blob_inputs[i].glyph_count;
        }
        document->inputs = inputs;
        document->input_count = (int)count;
        document->owned_inputs = inputs;
    }
    if (glyphs_section != NULL && glyphs_size > 0) {
        uint32_t count = glyphs_size / (uint32_t)sizeof(WnBlobInputGlyph);
        const WnBlobInputGlyph *blob_glyphs =
            (const WnBlobInputGlyph *)(map.data + glyphs_section->offset);
        UiInputGlyph *glyphs = (UiInputGlyph *)calloc(count, sizeof(UiInputGlyph));
        if (glyphs == NULL || glyphs_size % sizeof(WnBlobInputGlyph) != 0) {
            free(glyphs);
            wn_set_error("wn_document_load_blob: bad glyph section");
            wn_document_destroy(document);
            return NULL;
        }
        for (i = 0; i < count; ++i) {
            glyphs[i].cp = blob_glyphs[i].cp;
            glyphs[i].gid = blob_glyphs[i].gid;
            glyphs[i].advance = blob_glyphs[i].advance;
            glyphs[i].x = blob_glyphs[i].x;
            glyphs[i].y = blob_glyphs[i].y;
            glyphs[i].w = blob_glyphs[i].w;
            glyphs[i].h = blob_glyphs[i].h;
            glyphs[i].u0 = blob_glyphs[i].u0;
            glyphs[i].v0 = blob_glyphs[i].v0;
            glyphs[i].u1 = blob_glyphs[i].u1;
            glyphs[i].v1 = blob_glyphs[i].v1;
        }
        document->input_glyphs = glyphs;
        document->input_glyph_count = (int)count;
        document->owned_glyphs = glyphs;
    }
    return document;
}

void wn_document_destroy(wn_document *document)
{
    document_release(document);
}
