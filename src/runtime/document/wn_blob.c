#include <math.h>
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

_Static_assert(sizeof(WnBlobStateRecord) == 8, "WnBlobStateRecord must stay 8 bytes");
_Static_assert(sizeof(WnBlobStateStyle) == 16, "WnBlobStateStyle must stay 16 bytes");
_Static_assert(sizeof(WnBlobTextExtras) == 8, "WnBlobTextExtras must stay 8 bytes");
_Static_assert(sizeof(WnBlobMediaRecord) == 16, "WnBlobMediaRecord must stay 16 bytes");

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
    free(document->owned_states);
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
    if (wn_document_is_builtin(document)) {
        return; /* the builtin document has a static refcount */
    }
    if (document == NULL || !document_live(document)) {
        return;
    }
    if (document->refs < WN_DOCUMENT_STATIC_REFS - 1) {
        ++document->refs;
    }
}

void wn_document_release(wn_document *document)
{
    if (wn_document_is_builtin(document)) {
        return;
    }
    if (document == NULL || !document_live(document)) {
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
    const WnBlobSection *states_section = NULL;
    const WnBlobSection *text_extras_section = NULL;
    const WnBlobSection *media_section = NULL;
    uint32_t nodes_size = 0;
    uint32_t names_size = 0;
    uint32_t strings_size = 0;
    uint32_t inputs_size = 0;
    uint32_t states_size = 0;
    uint32_t text_extras_size = 0;
    uint32_t media_size = 0;
    uint32_t media_count = 0;
    uint32_t media_cursor = 0;
    uint32_t video_count = 0;
    const WnBlobMediaRecord *media_records = NULL;
    uint32_t state_total = 0;
    UiStateStyle *states = NULL;
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
    states_section = find_section(sections, header->section_count, WN_BLOB_SECTION_STATES,
                                  header->file_size, &states_size);
    if (states_section == NULL) {
        states_size = 0;
    }
    text_extras_section = find_section(sections, header->section_count,
                                       WN_BLOB_SECTION_TEXT_EXTRAS, header->file_size,
                                       &text_extras_size);
    if (text_extras_section == NULL) {
        text_extras_size = 0;
    }
    media_section = find_section(sections, header->section_count,
                                 WN_BLOB_SECTION_MEDIA, header->file_size,
                                 &media_size);
    if (media_section == NULL) {
        media_size = 0;
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
        if (media_section != NULL) {
            int32_t previous_node = -1;
            if (media_size == 0 ||
                media_size % (uint32_t)sizeof(WnBlobMediaRecord) != 0) {
                wn_set_error("wn_document_load_blob: %s: bad media section "
                             "(size %u)", origin, media_size);
                return NULL;
            }
            media_count = media_size / (uint32_t)sizeof(WnBlobMediaRecord);
            media_records = (const WnBlobMediaRecord *)(data + media_section->offset);
            for (i = 0; i < media_count; ++i) {
                const WnBlobMediaRecord *record = &media_records[i];
                const char *src;
                const char *poster;
                if (record->node < 0 ||
                    (uint32_t)record->node >= header->node_count) {
                    wn_set_error("wn_document_load_blob: %s: media record %u "
                                 "references node %d", origin, i,
                                 (int)record->node);
                    return NULL;
                }
                if (record->node <= previous_node) {
                    wn_set_error("wn_document_load_blob: %s: media records are "
                                 "not in strictly increasing node order (%d "
                                 "after %d)", origin, (int)record->node,
                                 (int)previous_node);
                    return NULL;
                }
                previous_node = record->node;
                if (blob_nodes[record->node].kind != WN_BLOB_NODE_VIDEO) {
                    wn_set_error("wn_document_load_blob: %s: media record %u "
                                 "targets node %d of kind %d (expected video)",
                                 origin, i, (int)record->node,
                                 (int)blob_nodes[record->node].kind);
                    return NULL;
                }
                if ((record->flags & 0xF0u) != 0u) {
                    wn_set_error("wn_document_load_blob: %s: node %d has media "
                                 "flags 0x%x (bits 4..7 are reserved)",
                                 origin, (int)record->node,
                                 (unsigned)record->flags);
                    return NULL;
                }
                if (record->preload > WN_MEDIA_PRELOAD_AUTO) {
                    wn_set_error("wn_document_load_blob: %s: node %d has "
                                 "preload %u (expected 0..2)", origin,
                                 (int)record->node, (unsigned)record->preload);
                    return NULL;
                }
                if (record->reserved[0] != 0u || record->reserved[1] != 0u) {
                    wn_set_error("wn_document_load_blob: %s: node %d has "
                                 "non-zero media reserved bytes", origin,
                                 (int)record->node);
                    return NULL;
                }
                if (!blob_check_string(strings, strings_size, record->src,
                                       "media src", i, origin) ||
                    !blob_check_string(strings, strings_size, record->poster,
                                       "media poster", i, origin)) {
                    return NULL;
                }
                src = blob_string(strings, strings_size, record->src);
                if (src == NULL || src[0] == '\0') {
                    wn_set_error("wn_document_load_blob: %s: node %d has an "
                                 "empty media src", origin, (int)record->node);
                    return NULL;
                }
                if (strstr(src, "://") != NULL) {
                    wn_set_error("wn_document_load_blob: %s: node %d media src "
                                 "'%s' is not a local path", origin,
                                 (int)record->node, src);
                    return NULL;
                }
                poster = blob_string(strings, strings_size, record->poster);
                if (record->poster != 0u &&
                    (poster == NULL || poster[0] == '\0')) {
                    wn_set_error("wn_document_load_blob: %s: node %d has an "
                                 "empty media poster", origin,
                                 (int)record->node);
                    return NULL;
                }
                if (poster != NULL && strstr(poster, "://") != NULL) {
                    wn_set_error("wn_document_load_blob: %s: node %d media "
                                 "poster '%s' is not a local path", origin,
                                 (int)record->node, poster);
                    return NULL;
                }
            }
        }
    }
    if (states_section != NULL && states_size > 0) {
        /* First pass: validate the variable-length records and count the
         * entries so the per-node pointers can be carved from one block. */
        const unsigned char *state_data = data + states_section->offset;
        uint32_t offset = 0;
        int32_t previous_node = -1;
        while (offset < states_size) {
            WnBlobStateRecord record;
            uint8_t seen = 0;
            uint32_t entry;
            if (states_size - offset < (uint32_t)sizeof(WnBlobStateRecord)) {
                wn_set_error("wn_document_load_blob: %s: truncated states record",
                             origin);
                return NULL;
            }
            memcpy(&record, state_data + offset, sizeof(record));
            offset += (uint32_t)sizeof(record);
            if (record.node < 0 ||
                (uint32_t)record.node >= header->node_count) {
                wn_set_error("wn_document_load_blob: %s: state record references "
                             "node %d", origin, (int)record.node);
                return NULL;
            }
            /* Records are written in node order; requiring it rejects
             * duplicates and keeps the carve-out single pass. */
            if (record.node <= previous_node) {
                wn_set_error("wn_document_load_blob: %s: state records are not in "
                             "strictly increasing node order (%d after %d)",
                             origin, (int)record.node, (int)previous_node);
                return NULL;
            }
            previous_node = record.node;
            if (record.state_count > 6) {
                wn_set_error("wn_document_load_blob: %s: node %d has %u state "
                             "entries (expected 0..6)", origin, (int)record.node,
                             (unsigned)record.state_count);
                return NULL;
            }
            if ((record.initial_state & ~(uint8_t)WN_STATE_DISABLED) != 0u) {
                wn_set_error("wn_document_load_blob: %s: node %d has initial state "
                             "0x%x (only WN_STATE_DISABLED is seeded by the "
                             "document)", origin, (int)record.node,
                             (unsigned)record.initial_state);
                return NULL;
            }
            if ((states_size - offset) / (uint32_t)sizeof(WnBlobStateStyle) <
                (uint32_t)record.state_count) {
                wn_set_error("wn_document_load_blob: %s: truncated state entries "
                             "for node %d", origin, (int)record.node);
                return NULL;
            }
            for (entry = 0; entry < (uint32_t)record.state_count; ++entry) {
                WnBlobStateStyle style;
                uint8_t mask;
                memcpy(&style, state_data + offset + entry * sizeof(style),
                       sizeof(style));
                if (style.state >= R2_STATE_STYLE_COUNT) {
                    wn_set_error("wn_document_load_blob: %s: node %d has state "
                                 "code %u", origin, (int)record.node,
                                 (unsigned)style.state);
                    return NULL;
                }
                mask = (uint8_t)(1u << style.state);
                if ((seen & mask) != 0u) {
                    wn_set_error("wn_document_load_blob: %s: node %d repeats "
                                 "state %u", origin, (int)record.node,
                                 (unsigned)style.state);
                    return NULL;
                }
                seen |= mask;
                if (style.present == 0u ||
                    (style.present & ~(uint8_t)(R2_STATE_FIELD_BACKGROUND |
                                                R2_STATE_FIELD_TEXT |
                                                R2_STATE_FIELD_BORDER |
                                                R2_STATE_FIELD_FONT_WEIGHT)) != 0u) {
                    wn_set_error("wn_document_load_blob: %s: node %d state %u has "
                                 "invalid present mask 0x%x", origin,
                                 (int)record.node, (unsigned)style.state,
                                 (unsigned)style.present);
                    return NULL;
                }
                if (((style.present & R2_STATE_FIELD_BACKGROUND) != 0u &&
                     style.background == 0u) ||
                    ((style.present & R2_STATE_FIELD_TEXT) != 0u &&
                     style.text == 0u) ||
                    ((style.present & R2_STATE_FIELD_BORDER) != 0u &&
                     style.border_color == 0u)) {
                    wn_set_error("wn_document_load_blob: %s: node %d state %u has "
                                 "a zero color (transparent is not representable in "
                                 "a blob v6 state style)", origin,
                                 (int)record.node, (unsigned)style.state);
                    return NULL;
                }
                if ((style.present & R2_STATE_FIELD_FONT_WEIGHT) != 0u) {
                    if (style.reserved < 100u || style.reserved > 900u) {
                        wn_set_error("wn_document_load_blob: %s: node %d state %u "
                                     "has font-weight %u (expected 100..900)",
                                     origin, (int)record.node,
                                     (unsigned)style.state,
                                     (unsigned)style.reserved);
                        return NULL;
                    }
                } else if (style.reserved != 0u) {
                    wn_set_error("wn_document_load_blob: %s: node %d state %u "
                                 "carries a weight (%u) without the present bit",
                                 origin, (int)record.node, (unsigned)style.state,
                                 (unsigned)style.reserved);
                    return NULL;
                }
            }
            offset += (uint32_t)record.state_count * (uint32_t)sizeof(WnBlobStateStyle);
            state_total += (uint32_t)record.state_count;
        }
        if (offset != states_size) {
            wn_set_error("wn_document_load_blob: %s: states section has trailing bytes",
                         origin);
            return NULL;
        }
        if (state_total > header->node_count * 6u) {
            wn_set_error("wn_document_load_blob: %s: too many state entries (%u)",
                         origin, state_total);
            return NULL;
        }
    }
    if (text_extras_section != NULL && text_extras_size > 0) {
        const unsigned char *extras_data = data + text_extras_section->offset;
        uint32_t offset = 0;
        int32_t previous_node = -1;
        if (text_extras_size % (uint32_t)sizeof(WnBlobTextExtras) != 0) {
            wn_set_error("wn_document_load_blob: %s: bad text_extras section",
                         origin);
            return NULL;
        }
        while (offset < text_extras_size) {
            WnBlobTextExtras record;
            memcpy(&record, extras_data + offset, sizeof(record));
            offset += (uint32_t)sizeof(record);
            if (record.node < 0 || (uint32_t)record.node >= header->node_count) {
                wn_set_error("wn_document_load_blob: %s: text_extras record "
                             "references node %d", origin, (int)record.node);
                return NULL;
            }
            if (record.node <= previous_node) {
                wn_set_error("wn_document_load_blob: %s: text_extras records are "
                             "not in strictly increasing node order (%d after %d)",
                             origin, (int)record.node, (int)previous_node);
                return NULL;
            }
            previous_node = record.node;
            if (record.flags == 0u ||
                (record.flags & ~(uint8_t)WN_TEXT_FLAG_ELLIPSIS) != 0u ||
                record.reserved[0] != 0u || record.reserved[1] != 0u ||
                record.reserved[2] != 0u) {
                wn_set_error("wn_document_load_blob: %s: node %d has invalid "
                             "text_extras flags 0x%x", origin, (int)record.node,
                             (unsigned)record.flags);
                return NULL;
            }
        }
    }

    document = (wn_document *)calloc(1, sizeof(*document));
    nodes = (UiNode *)calloc(header->node_count, sizeof(UiNode));
    names = (const char **)calloc(header->node_count, sizeof(char *));
    depth = (int *)calloc(header->node_count, sizeof(int));
    if (state_total > 0) {
        states = (UiStateStyle *)calloc(state_total, sizeof(UiStateStyle));
    }
    if (document == NULL || nodes == NULL || names == NULL || depth == NULL ||
        (state_total > 0 && states == NULL)) {
        wn_set_error("wn_document_load_blob: %s: out of memory for %u nodes",
                     origin, header->node_count);
        free(document);
        free(nodes);
        free(names);
        free(depth);
        free(states);
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
        free(states);
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
        if (!(src->opacity >= 0.0f && src->opacity <= 1.0f)) {
            wn_set_error("wn_document_load_blob: %s: node %u has opacity %.4f "
                         "(expected 0..1)", origin, i, (double)src->opacity);
            goto fail;
        }
        if (src->position < R2_POSITION_STATIC ||
            src->position > R2_POSITION_STICKY) {
            wn_set_error("wn_document_load_blob: %s: node %u has position %d "
                         "(expected 0 static, 1 relative, 2 absolute, 3 fixed, "
                         "4 sticky)", origin, i, (int)src->position);
            goto fail;
        }
        if ((src->inset_mask & ~0xFFu) != 0u ||
            ((src->inset_mask >> 4) & ~(src->inset_mask & 0x0Fu)) != 0u) {
            wn_set_error("wn_document_load_blob: %s: node %u has inset_mask 0x%x "
                         "(bits 0..3 declared, bits 4..7 percentage only on a "
                         "declared side)", origin, i, src->inset_mask);
            goto fail;
        }
        {
            uint32_t side;
            for (side = 0; side < 4u; ++side) {
                if ((src->inset_mask & (1u << side)) != 0u &&
                    !isfinite(src->inset[side])) {
                    wn_set_error("wn_document_load_blob: %s: node %u has a "
                                 "non-finite inset[%u]", origin, i, side);
                    goto fail;
                }
            }
        }
        if (src->kind == WN_BLOB_NODE_VIDEO) {
            ++video_count;
            if (media_records == NULL ||
                media_cursor >= media_count ||
                media_records[media_cursor].node != (int32_t)i) {
                wn_set_error("wn_document_load_blob: %s: video node %u has no "
                             "media record", origin, i);
                goto fail;
            }
            if (src->image_src != 0u || src->image_alt != 0u ||
                src->image_width != 0.0f || src->image_height != 0.0f) {
                wn_set_error("wn_document_load_blob: %s: video node %u carries "
                             "image fields (src/alt/width/height must be 0)",
                             origin, i);
                goto fail;
            }
            dst->media_src = blob_string(strings, strings_size,
                                         media_records[media_cursor].src);
            dst->media_poster = blob_string(strings, strings_size,
                                            media_records[media_cursor].poster);
            dst->media_flags = media_records[media_cursor].flags;
            dst->media_preload = media_records[media_cursor].preload;
            ++media_cursor;
        } else if (media_records != NULL && media_cursor < media_count &&
                   media_records[media_cursor].node == (int32_t)i) {
            /* The first pass already rejected a record on a non-video kind;
             * keep the invariant explicit at the association point too. */
            wn_set_error("wn_document_load_blob: %s: media record targets "
                         "non-video node %u", origin, i);
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
        dst->opacity = src->opacity;
        dst->position = src->position;
        memcpy(dst->inset, src->inset, sizeof(dst->inset));
        dst->inset_mask = src->inset_mask;
        /* Blob v5: INT32_MIN is the documented `auto` sentinel; every other
         * int32 is an explicit CSS z-index. The int32 field itself is the
         * range validation: the frontend rejects non-integers, values outside
         * int32 and the reserved sentinel before writing. */
        dst->z_index = src->z_index;
        dst->text_flags = 0u;
    }
    if (media_cursor != media_count) {
        wn_set_error("wn_document_load_blob: %s: media section has %u records "
                     "for %u video nodes", origin, media_count, video_count);
        goto fail;
    }
    if (video_count > 0u && media_records == NULL) {
        wn_set_error("wn_document_load_blob: %s: document has %u video nodes "
                     "but no media section", origin, video_count);
        goto fail;
    }
    if (video_count == 0u && media_records != NULL) {
        wn_set_error("wn_document_load_blob: %s: media section present without "
                     "video nodes", origin);
        goto fail;
    }
    {
        const uint32_t *name_offsets =
            (const uint32_t *)(data + names_section->offset);
        for (i = 0; i < header->node_count; ++i) {
            const char *name = blob_string(strings, strings_size, name_offsets[i]);
            names[i] = name != NULL ? name : "";
        }
    }
    if (states_section != NULL && states_size > 0) {
        const unsigned char *state_data = data + states_section->offset;
        uint32_t offset = 0;
        uint32_t entry = 0;
        while (offset < states_size) {
            WnBlobStateRecord record;
            uint32_t index;
            memcpy(&record, state_data + offset, sizeof(record));
            offset += (uint32_t)sizeof(record);
            nodes[record.node].initial_state = record.initial_state;
            if (record.state_count > 0) {
                nodes[record.node].state_styles = &states[entry];
                nodes[record.node].state_style_count = record.state_count;
            }
            for (index = 0; index < (uint32_t)record.state_count; ++index) {
                memcpy(&states[entry], state_data + offset + index * sizeof(states[entry]),
                       sizeof(states[entry]));
                ++entry;
            }
            offset += (uint32_t)record.state_count * (uint32_t)sizeof(WnBlobStateStyle);
        }
        document->owned_states = states;
    }
    if (text_extras_section != NULL && text_extras_size > 0) {
        const unsigned char *extras_data = data + text_extras_section->offset;
        uint32_t offset = 0;
        while (offset < text_extras_size) {
            WnBlobTextExtras record;
            memcpy(&record, extras_data + offset, sizeof(record));
            offset += (uint32_t)sizeof(record);
            nodes[record.node].text_flags = record.flags;
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
            /* Internal free: the failure path must not re-enter the public
             * release path (and through it the builtin identity check). */
            document_free(document);
            return NULL;
        }
        if (inputs_size % sizeof(WnBlobInput) != 0) {
            free(inputs);
            wn_set_error("wn_document_load_blob: %s: bad inputs section", origin);
            document_free(document);
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
    free(states);
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
