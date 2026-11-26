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

/* ABI v6: live-document registry. release() checks it before dereferencing so
 * a double release of an already-freed document is clamped instead of walking
 * into freed memory. Single-threaded UI contract, like the rest of the core. */
static wn_document **g_documents;
static int g_document_count;
static int g_document_capacity;

static int document_live(const wn_document *document)
{
    int i;
    for (i = 0; i < g_document_count; ++i) {
        if (g_documents[i] == document) {
            return 1;
        }
    }
    return 0;
}

static int document_track(wn_document *document)
{
    if (g_document_count == g_document_capacity) {
        int capacity = g_document_capacity > 0 ? g_document_capacity * 2 : 8;
        wn_document **grown = (wn_document **)realloc(
            g_documents, (size_t)capacity * sizeof(*g_documents));
        if (grown == NULL) {
            return -1;
        }
        g_documents = grown;
        g_document_capacity = capacity;
    }
    g_documents[g_document_count++] = document;
    return 0;
}

static void document_untrack(const wn_document *document)
{
    int i;
    for (i = 0; i < g_document_count; ++i) {
        if (g_documents[i] == document) {
            g_documents[i] = g_documents[--g_document_count];
            return;
        }
    }
}

static void document_free(wn_document *document)
{
    if (document == NULL) {
        return;
    }
    document_untrack(document);
    free(document->owned_nodes);
    free(document->owned_names);
    free(document->owned_inputs);
    if (document->owns_mapping &&
        (document->mapping != NULL || document->mapping_handle != NULL)) {
        WnBlobMap map;
        map.mapping = document->mapping;
        map.mapping_handle = document->mapping_handle;
        map.data = NULL;
        map.size = 0;
        blob_map_close(&map);
    }
    free(document);
}

void wn_document_retain(wn_document *document)
{
    if (document == NULL || document == wn_document_builtin()) {
        return; /* the builtin document has a static refcount */
    }
    if (!document_live(document)) {
        return;
    }
    if (document->refs < WN_DOCUMENT_STATIC_REFS - 1) {
        ++document->refs;
    }
}

void wn_document_release(wn_document *document)
{
    if (document == NULL || document == wn_document_builtin()) {
        return;
    }
    if (!document_live(document)) {
        return; /* double release: clamp instead of touching freed memory */
    }
    if (document->refs > 0) {
        --document->refs;
    }
    if (document->refs == 0) {
        document_free(document);
    }
}

static const char *blob_string(const char *pool, uint32_t pool_size, uint32_t offset)
{
    if (offset == 0 || offset >= pool_size) {
        return NULL;
    }
    return pool + offset;
}

/* A string offset is either 0 (absent) or points inside the pool at a string
 * whose NUL ends before the section does. Without the terminator strcmp() and
 * the text runtime could read past the mapping. */
static int blob_string_terminated(const char *pool, uint32_t pool_size,
                                  uint32_t offset)
{
    if (offset == 0) {
        return 1;
    }
    if (offset >= pool_size) {
        return 0;
    }
    return memchr(pool + offset, '\0', (size_t)(pool_size - offset)) != NULL;
}

static int blob_check_string(const char *pool, uint32_t pool_size,
                             uint32_t offset, const char *what, uint32_t index,
                             const char *origin)
{
    if (blob_string_terminated(pool, pool_size, offset)) {
        return 1;
    }
    wn_set_error("wn_document_load_blob: %s: %s %u references an unterminated "
                 "string (offset %u, pool %u bytes)",
                 origin, what, index, offset, pool_size);
    return 0;
}

/* Shared parser for the file and memory entry points. `owned_map` is non-NULL
 * only for a file mapping; on success its handles move into the document and
 * it is zeroed. The caller keeps ownership on failure. */
static wn_document *blob_parse(const unsigned char *data, size_t size,
                               WnBlobMap *owned_map, const char *origin)
{
    const WnBlobHeader *header;
    const WnBlobSection *sections;
    const WnBlobNode *blob_nodes;
    const WnBlobSection *nodes_section;
    const WnBlobSection *names_section;
    const WnBlobSection *strings_section;
    const WnBlobSection *inputs_section = NULL;
    uint32_t nodes_size = 0;
    uint32_t names_size = 0;
    uint32_t strings_size = 0;
    uint32_t inputs_size = 0;
    const char *strings;
    wn_document *document;
    UiNode *nodes;
    const char **names;
    int *depth = NULL;
    uint32_t i;

    if (size < WN_BLOB_HEADER_SIZE) {
        wn_set_error("wn_document_load_blob: %s: file too small", origin);
        return NULL;
    }
    if (size > (size_t)UINT32_MAX) {
        wn_set_error("wn_document_load_blob: %s: file too large", origin);
        return NULL;
    }
    header = (const WnBlobHeader *)data;
    if (header->magic != WN_BLOB_MAGIC || header->version != WN_BLOB_VERSION) {
        wn_set_error("wn_document_load_blob: %s: bad magic or version", origin);
        return NULL;
    }
    if (header->file_size != (uint32_t)size || header->node_count == 0 ||
        header->node_count > (uint32_t)INT32_MAX ||
        WN_BLOB_HEADER_SIZE + (size_t)header->section_count * sizeof(WnBlobSection) > size) {
        wn_set_error("wn_document_load_blob: %s: inconsistent header", origin);
        return NULL;
    }
    if (header->node_count > WN_BLOB_MAX_NODES) {
        wn_set_error("wn_document_load_blob: %s: too many nodes (%u, max %u)",
                     origin, header->node_count, WN_BLOB_MAX_NODES);
        return NULL;
    }
    if (header->viewport_width == 0 || header->viewport_height == 0 ||
        header->viewport_width > (uint32_t)INT32_MAX ||
        header->viewport_height > (uint32_t)INT32_MAX) {
        wn_set_error("wn_document_load_blob: %s: invalid viewport %ux%u",
                     origin, header->viewport_width, header->viewport_height);
        return NULL;
    }
    sections = (const WnBlobSection *)(data + WN_BLOB_HEADER_SIZE);
    nodes_section = find_section(sections, header->section_count, WN_BLOB_SECTION_NODES,
                                 header->file_size, &nodes_size);
    names_section = find_section(sections, header->section_count, WN_BLOB_SECTION_NAMES,
                                 header->file_size, &names_size);
    strings_section = find_section(sections, header->section_count, WN_BLOB_SECTION_STRINGS,
                                   header->file_size, &strings_size);
    if (nodes_section == NULL || names_section == NULL || strings_section == NULL) {
        wn_set_error("wn_document_load_blob: %s: missing required section", origin);
        return NULL;
    }
    inputs_section = find_section(sections, header->section_count, WN_BLOB_SECTION_INPUTS,
                                  header->file_size, &inputs_size);
    if (inputs_section == NULL) {
        inputs_size = 0;
    }
    if ((uint64_t)header->node_count * sizeof(WnBlobNode) != nodes_size ||
        (uint64_t)header->node_count * sizeof(uint32_t) != names_size ||
        strings_size == 0) {
        wn_set_error("wn_document_load_blob: %s: wrong section size", origin);
        return NULL;
    }

    strings = (const char *)(data + strings_section->offset);
    blob_nodes = (const WnBlobNode *)(data + nodes_section->offset);
    {
        const uint32_t *name_offsets =
            (const uint32_t *)(data + names_section->offset);
        /* Validate every offset before any string reaches the engine: a
         * corrupt NUL would turn later strcmp()/shaping calls into out-of-bounds
         * reads. */
        for (i = 0; i < header->node_count; ++i) {
            const WnBlobNode *src = &blob_nodes[i];
            if (!blob_check_string(strings, strings_size, src->text,
                                   "node text", i, origin) ||
                !blob_check_string(strings, strings_size, src->image_src,
                                   "node image_src", i, origin) ||
                !blob_check_string(strings, strings_size, src->image_alt,
                                   "node image_alt", i, origin) ||
                !blob_check_string(strings, strings_size, src->font_family,
                                   "node font_family", i, origin) ||
                !blob_check_string(strings, strings_size, name_offsets[i],
                                   "node name", i, origin)) {
                return NULL;
            }
        }
        if (inputs_section != NULL && inputs_size > 0) {
            uint32_t count;
            const WnBlobInput *blob_inputs;
            if (inputs_size % (uint32_t)sizeof(WnBlobInput) != 0) {
                wn_set_error("wn_document_load_blob: %s: bad inputs section", origin);
                return NULL;
            }
            count = inputs_size / (uint32_t)sizeof(WnBlobInput);
            blob_inputs = (const WnBlobInput *)(data + inputs_section->offset);
            for (i = 0; i < count; ++i) {
                if (blob_inputs[i].node < 0 ||
                    (uint32_t)blob_inputs[i].node >= header->node_count) {
                    wn_set_error("wn_document_load_blob: %s: input %u references node %d",
                                 origin, i, (int)blob_inputs[i].node);
                    return NULL;
                }
                if (!blob_check_string(strings, strings_size,
                                       blob_inputs[i].value, "input value", i, origin) ||
                    !blob_check_string(strings, strings_size,
                                       blob_inputs[i].placeholder,
                                       "input placeholder", i, origin)) {
                    return NULL;
                }
            }
        }
    }

    document = (wn_document *)calloc(1, sizeof(*document));
    nodes = (UiNode *)calloc(header->node_count, sizeof(UiNode));
    names = (const char **)calloc(header->node_count, sizeof(char *));
    depth = (int *)calloc(header->node_count, sizeof(int));
    if (document == NULL || nodes == NULL || names == NULL || depth == NULL) {
        wn_set_error("wn_document_load_blob: %s: out of memory for %u nodes",
                     origin, header->node_count);
        free(document);
        free(nodes);
        free(names);
        free(depth);
        return NULL;
    }
    document->refs = 1;
    if (document_track(document) != 0) {
        wn_set_error("wn_document_load_blob: %s: out of memory tracking the document",
                     origin);
        free(document);
        free(nodes);
        free(names);
        free(depth);
        return NULL;
    }

    for (i = 0; i < header->node_count; ++i) {
        const WnBlobNode *src = &blob_nodes[i];
        UiNode *dst = &nodes[i];
        /* The writer emits pre-order, so every parent index is smaller than the
         * node's. Enforcing it resolves the depth in one pass and rejects
         * cycles/forward links; the recursive layout then cannot overflow. */
        if (i == 0) {
            if (src->parent != -1) {
                wn_set_error("wn_document_load_blob: %s: node 0 is not the root", origin);
                goto fail;
            }
            depth[0] = 1;
        } else {
            if (src->parent < 0 || (uint32_t)src->parent >= i) {
                wn_set_error("wn_document_load_blob: %s: node %u has an out-of-order "
                             "parent %d", origin, i, (int)src->parent);
                goto fail;
            }
            if (depth[src->parent] >= WN_BLOB_MAX_DEPTH) {
                wn_set_error("wn_document_load_blob: %s: node tree is deeper than %d "
                             "(node %u)", origin, WN_BLOB_MAX_DEPTH, i);
                goto fail;
            }
            depth[i] = depth[src->parent] + 1;
        }
        if (!(src->font_size >= WN_BLOB_MIN_FONT_SIZE &&
              src->font_size <= WN_BLOB_MAX_FONT_SIZE)) {
            wn_set_error("wn_document_load_blob: %s: node %u has font_size %.2f "
                         "(expected %.0f..%.0f)", origin, i, (double)src->font_size,
                         (double)WN_BLOB_MIN_FONT_SIZE,
                         (double)WN_BLOB_MAX_FONT_SIZE);
            goto fail;
        }
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
        dst->text = blob_string(strings, strings_size, src->text);
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
        dst->image_src = blob_string(strings, strings_size, src->image_src);
        dst->image_alt = blob_string(strings, strings_size, src->image_alt);
        dst->object_fit = src->object_fit;
        dst->border_width = src->border_width;
        dst->border_color = src->border_color;
        dst->border_radius = src->border_radius;
        dst->shadow_x = src->shadow_x;
        dst->shadow_y = src->shadow_y;
        dst->shadow_blur = src->shadow_blur;
        dst->shadow_spread = src->shadow_spread;
        dst->shadow_color = src->shadow_color;
        dst->text_align = src->text_align;
        dst->line_height = src->line_height;
        dst->margin_auto = src->margin_auto;
        dst->box_sizing = src->box_sizing;
        dst->flex_grow = src->flex_grow;
        dst->flex_shrink = src->flex_shrink;
        dst->flex_basis = src->flex_basis;
        dst->font_family = blob_string(strings, strings_size, src->font_family);
        dst->font_weight = src->font_weight;
    }
    {
        const uint32_t *name_offsets =
            (const uint32_t *)(data + names_section->offset);
        for (i = 0; i < header->node_count; ++i) {
            const char *name = blob_string(strings, strings_size, name_offsets[i]);
            names[i] = name != NULL ? name : "";
        }
    }
    free(depth);
    depth = NULL;

    document->nodes = nodes;
    document->node_count = (int)header->node_count;
    document->names = names;
    document->owned_nodes = nodes;
    document->owned_names = names;
    if (owned_map != NULL) {
        document->mapping = owned_map->mapping;
        document->mapping_handle = owned_map->mapping_handle;
        document->owns_mapping = 1;
        memset(owned_map, 0, sizeof(*owned_map));
    }
    document->viewport_width = (int)header->viewport_width;
    document->viewport_height = (int)header->viewport_height;

    if (inputs_section != NULL && inputs_size > 0) {
        uint32_t count = inputs_size / (uint32_t)sizeof(WnBlobInput);
        const WnBlobInput *blob_inputs =
            (const WnBlobInput *)(data + inputs_section->offset);
        UiInput *inputs = (UiInput *)calloc(count, sizeof(UiInput));
        if (inputs == NULL) {
            wn_set_error("wn_document_load_blob: %s: out of memory for %u inputs",
                         origin, count);
            wn_document_destroy(document);
            return NULL;
        }
        if (inputs_size % sizeof(WnBlobInput) != 0) {
            free(inputs);
            wn_set_error("wn_document_load_blob: %s: bad inputs section", origin);
            wn_document_destroy(document);
            return NULL;
        }
        for (i = 0; i < count; ++i) {
            inputs[i].node = blob_inputs[i].node;
            const char *value = blob_string(strings, strings_size, blob_inputs[i].value);
            inputs[i].value = value != NULL ? value : "";
            inputs[i].placeholder = blob_string(strings, strings_size,
                                                blob_inputs[i].placeholder);
            inputs[i].max_length = blob_inputs[i].max_length;
            inputs[i].password = blob_inputs[i].password;
        }
        document->inputs = inputs;
        document->input_count = (int)count;
        document->owned_inputs = inputs;
    }
    return document;

fail:
    free(nodes);
    free(names);
    free(depth);
    document_untrack(document);
    free(document);
    return NULL;
}

wn_document *wn_document_load_blob(const char *path)
{
    WnBlobMap map;
    wn_document *document;

    if (path == NULL || blob_map_open(&map, path) != 0) {
        wn_set_error("wn_document_load_blob: cannot open %s", path != NULL ? path : "(null)");
        return NULL;
    }
    document = blob_parse(map.data, map.size, &map, path);
    if (document == NULL) {
        blob_map_close(&map);
    }
    return document;
}

wn_document *wn_document_load_blob_memory(const void *data, size_t size)
{
    if (data == NULL || size == 0) {
        wn_set_error("wn_document_load_blob: memory: empty buffer");
        return NULL;
    }
    return blob_parse((const unsigned char *)data, size, NULL, "memory");
}

void wn_document_destroy(wn_document *document)
{
    wn_document_release(document);
}
