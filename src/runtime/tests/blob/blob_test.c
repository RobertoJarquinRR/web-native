#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "document/wn_blob.h"
#include "document/wn_document_internal.h"
#include "webnative.h"
#include "wn_core.h"
#include "wn_internal.h"

static int checks = 0;
static int failures = 0;

static void check(const char *label, int ok)
{
    checks += 1;
    if (!ok) {
        failures += 1;
    }
    printf("  %s %s\n", ok ? "[ok]" : "[FAIL]", label);
}

/* Copies the valid blob and corrupts the last byte of the string pool: the
 * first node that carries a string is re-pointed at that byte, so its string
 * has no NUL before the section ends. */
static int write_unterminated_copy(const char *source, const char *target)
{
    FILE *file;
    long length;
    unsigned char *data;
    const WnBlobHeader *header;
    const WnBlobSection *sections;
    const WnBlobSection *strings_section = NULL;
    const WnBlobSection *nodes_section = NULL;
    WnBlobNode *nodes;
    uint32_t i;
    uint32_t field;
    int pointed = 0;
    int written = 0;

    file = fopen(source, "rb");
    if (file == NULL) {
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    data = (unsigned char *)malloc((size_t)length);
    if (data == NULL || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);

    if (length < (long)WN_BLOB_HEADER_SIZE) {
        free(data);
        return 0;
    }
    header = (const WnBlobHeader *)data;
    sections = (const WnBlobSection *)(data + WN_BLOB_HEADER_SIZE);
    for (i = 0; i < header->section_count; ++i) {
        if (sections[i].id == WN_BLOB_SECTION_STRINGS) {
            strings_section = &sections[i];
        } else if (sections[i].id == WN_BLOB_SECTION_NODES) {
            nodes_section = &sections[i];
        }
    }
    if (strings_section == NULL || nodes_section == NULL ||
        strings_section->size == 0) {
        free(data);
        return 0;
    }
    nodes = (WnBlobNode *)(data + nodes_section->offset);
    for (i = 0; i < header->node_count && !pointed; ++i) {
        uint32_t *fields[4];
        fields[0] = &nodes[i].text;
        fields[1] = &nodes[i].image_src;
        fields[2] = &nodes[i].image_alt;
        fields[3] = &nodes[i].font_family;
        for (field = 0; field < 4; ++field) {
            if (*fields[field] != 0) {
                *fields[field] = strings_section->size - 1;
                pointed = 1;
                break;
            }
        }
    }
    if (pointed) {
        data[strings_section->offset + strings_section->size - 1] = 'X';
        file = fopen(target, "wb");
        if (file != NULL) {
            written = fwrite(data, 1, (size_t)length, file) == (size_t)length;
            fclose(file);
        }
    }
    free(data);
    return written;
}

static uint32_t align_up8(uint32_t value)
{
    return (value + 7u) & ~7u;
}

static int write_file(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    int ok;
    if (file == NULL) {
        return 0;
    }
    ok = fwrite(data, 1, size, file) == size;
    fclose(file);
    return ok;
}

static int buffers_match(const void *a, int count_a, const void *b, int count_b,
                         size_t element_size)
{
    if (count_a != count_b) {
        return 0;
    }
    if (count_a <= 0) {
        return 1;
    }
    if (a == NULL || b == NULL) {
        return 0;
    }
    return memcmp(a, b, (size_t)count_a * element_size) == 0;
}

/* Minimal .wnui whose nodes form a single chain (parent i-1). Used to prove the
 * loader's depth limit, which protects the recursive layout/paint walk. */
static int write_chain_blob(const char *path, uint32_t node_count)
{
    uint32_t section_bytes = 3u * (uint32_t)sizeof(WnBlobSection);
    uint32_t nodes_offset = align_up8(WN_BLOB_HEADER_SIZE + section_bytes);
    uint32_t nodes_bytes = node_count * (uint32_t)sizeof(WnBlobNode);
    uint32_t names_offset = align_up8(nodes_offset + nodes_bytes);
    uint32_t names_bytes = node_count * (uint32_t)sizeof(uint32_t);
    uint32_t strings_offset = align_up8(names_offset + names_bytes);
    uint32_t strings_bytes = 1u;
    uint32_t total = align_up8(strings_offset + strings_bytes);
    unsigned char *data = (unsigned char *)calloc(1, total);
    WnBlobHeader *header;
    WnBlobSection *sections;
    WnBlobNode *nodes;
    uint32_t i;
    int ok;

    if (data == NULL) {
        return 0;
    }
    header = (WnBlobHeader *)data;
    header->magic = WN_BLOB_MAGIC;
    header->version = WN_BLOB_VERSION;
    header->section_count = 3u;
    header->file_size = total;
    header->node_count = node_count;
    header->viewport_width = 400u;
    header->viewport_height = 300u;

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
    for (i = 0; i < node_count; ++i) {
        nodes[i].parent = i == 0 ? -1 : (int32_t)(i - 1);
        nodes[i].font_size = 16.0f;
        nodes[i].opacity = 1.0f;
    }
    ok = write_file(path, data, (size_t)total);
    free(data);
    return ok;
}

/* Header-only file that claims more nodes than the loader accepts. */
static int write_oversized_header(const char *path)
{
    WnBlobHeader header;
    memset(&header, 0, sizeof(header));
    header.magic = WN_BLOB_MAGIC;
    header.version = WN_BLOB_VERSION;
    header.section_count = 0;
    header.file_size = (uint32_t)sizeof(header);
    header.node_count = WN_BLOB_MAX_NODES + 1u;
    return write_file(path, &header, sizeof(header));
}

static unsigned char *read_whole_file(const char *path, long *length)
{
    FILE *file = fopen(path, "rb");
    long size;
    unsigned char *data;

    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = (unsigned char *)malloc((size_t)size);
    if (data == NULL || fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *length = size;
    return data;
}

static WnBlobSection *find_blob_section(unsigned char *data, uint32_t id)
{
    WnBlobHeader *header = (WnBlobHeader *)data;
    WnBlobSection *sections = (WnBlobSection *)(data + WN_BLOB_HEADER_SIZE);
    uint32_t i;
    for (i = 0; i < header->section_count; ++i) {
        if (sections[i].id == id) {
            return &sections[i];
        }
    }
    return NULL;
}

static int write_zero_viewport_copy(const char *source, const char *target)
{
    long length = 0;
    unsigned char *data = read_whole_file(source, &length);
    int ok = 0;

    if (data == NULL) {
        return 0;
    }
    ((WnBlobHeader *)data)->viewport_width = 0;
    ok = write_file(target, data, (size_t)length);
    free(data);
    return ok;
}

/* Re-points the first input record past the node table. has_inputs is 0 when
 * the fixture has no INPUTS section. */
static int write_bad_input_copy(const char *source, const char *target,
                                int *has_inputs)
{
    long length = 0;
    unsigned char *data = read_whole_file(source, &length);
    WnBlobSection *inputs;
    int ok = 0;

    *has_inputs = 0;
    if (data == NULL) {
        return 0;
    }
    inputs = find_blob_section(data, WN_BLOB_SECTION_INPUTS);
    if (inputs == NULL || inputs->size < (uint32_t)sizeof(WnBlobInput) ||
        (uint32_t)length < inputs->offset + inputs->size) {
        free(data);
        return 0;
    }
    *has_inputs = 1;
    {
        WnBlobInput *records = (WnBlobInput *)(data + inputs->offset);
        WnBlobHeader *header = (WnBlobHeader *)data;
        records[0].node = (int32_t)header->node_count;
    }
    ok = write_file(target, data, (size_t)length);
    free(data);
    return ok;
}

/* Blows up the font size of the first node that has one. has_font is 0 when
 * the fixture has no sized node. */
static int write_bad_font_size_copy(const char *source, const char *target,
                                    int *has_font)
{
    long length = 0;
    unsigned char *data = read_whole_file(source, &length);
    WnBlobSection *nodes_section;
    int ok = 0;

    *has_font = 0;
    if (data == NULL) {
        return 0;
    }
    nodes_section = find_blob_section(data, WN_BLOB_SECTION_NODES);
    if (nodes_section == NULL || nodes_section->size < (uint32_t)sizeof(WnBlobNode) ||
        (uint32_t)length < nodes_section->offset + nodes_section->size) {
        free(data);
        return 0;
    }
    {
        WnBlobNode *nodes = (WnBlobNode *)(data + nodes_section->offset);
        uint32_t count = nodes_section->size / (uint32_t)sizeof(WnBlobNode);
        uint32_t i;
        for (i = 0; i < count; ++i) {
            if (nodes[i].font_size > 0.0f) {
                nodes[i].font_size = 100000.0f;
                *has_font = 1;
                ok = write_file(target, data, (size_t)length);
                break;
            }
        }
    }
    free(data);
    return ok;
}

/* Overwrites node 0's opacity. The fixture always has at least one node. */
static int write_opacity_copy(const char *source, const char *target,
                              float opacity)
{
    long length = 0;
    unsigned char *data = read_whole_file(source, &length);
    WnBlobSection *nodes_section;
    int ok = 0;

    if (data == NULL) {
        return 0;
    }
    nodes_section = find_blob_section(data, WN_BLOB_SECTION_NODES);
    if (nodes_section != NULL &&
        nodes_section->size >= (uint32_t)sizeof(WnBlobNode) &&
        (uint32_t)length >= nodes_section->offset + nodes_section->size) {
        WnBlobNode *nodes = (WnBlobNode *)(data + nodes_section->offset);
        nodes[0].opacity = opacity;
        ok = write_file(target, data, (size_t)length);
    }
    free(data);
    return ok;
}

static int strings_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

/* Field-by-field comparison: pointers differ when the same document is parsed
 * from two buffers, so string contents are compared instead of addresses. */
static int node_equal(const UiNode *a, const UiNode *b)
{
    return a->parent == b->parent && a->kind == b->kind &&
           a->direction == b->direction && a->justify == b->justify &&
           a->align == b->align && a->wrap == b->wrap &&
           a->width == b->width && a->height == b->height &&
           a->width_pct == b->width_pct && a->height_pct == b->height_pct &&
           memcmp(a->margin, b->margin, sizeof(a->margin)) == 0 &&
           memcmp(a->padding, b->padding, sizeof(a->padding)) == 0 &&
           a->background == b->background &&
           strings_equal(a->text, b->text) &&
           a->hover_bg == b->hover_bg && a->press_bg == b->press_bg &&
           a->selected_bg == b->selected_bg && a->action == b->action &&
           a->gap == b->gap && a->font_size == b->font_size &&
           a->text_color == b->text_color && a->grid_mode == b->grid_mode &&
           a->grid_column_px == b->grid_column_px &&
           a->grid_auto_rows == b->grid_auto_rows &&
           a->row_gap == b->row_gap && a->column_gap == b->column_gap &&
           a->overflow == b->overflow &&
           a->image_width == b->image_width &&
           a->image_height == b->image_height &&
           strings_equal(a->image_src, b->image_src) &&
           strings_equal(a->image_alt, b->image_alt) &&
           a->object_fit == b->object_fit &&
           a->border_width == b->border_width &&
           a->border_color == b->border_color &&
           a->border_radius == b->border_radius &&
           a->shadow_x == b->shadow_x && a->shadow_y == b->shadow_y &&
           a->shadow_blur == b->shadow_blur &&
           a->shadow_spread == b->shadow_spread &&
           a->shadow_color == b->shadow_color &&
           a->text_align == b->text_align &&
           a->line_height == b->line_height &&
           a->margin_auto == b->margin_auto &&
           a->box_sizing == b->box_sizing &&
           a->flex_grow == b->flex_grow &&
           a->flex_shrink == b->flex_shrink &&
           a->flex_basis == b->flex_basis &&
           strings_equal(a->font_family, b->font_family) &&
           a->font_weight == b->font_weight &&
           a->opacity == b->opacity &&
           a->position == b->position &&
           memcmp(a->inset, b->inset, sizeof(a->inset)) == 0 &&
           a->inset_mask == b->inset_mask;
}

static int documents_equal(const wn_document *a, const wn_document *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a->node_count != b->node_count ||
        a->viewport_width != b->viewport_width ||
        a->viewport_height != b->viewport_height ||
        a->input_count != b->input_count) {
        return 0;
    }
    for (int i = 0; i < a->node_count; ++i) {
        if (!node_equal(&a->nodes[i], &b->nodes[i]) ||
            !strings_equal(a->names[i], b->names[i])) {
            return 0;
        }
    }
    for (int i = 0; i < a->input_count; ++i) {
        if (a->inputs[i].node != b->inputs[i].node ||
            a->inputs[i].max_length != b->inputs[i].max_length ||
            a->inputs[i].password != b->inputs[i].password ||
            !strings_equal(a->inputs[i].value, b->inputs[i].value) ||
            !strings_equal(a->inputs[i].placeholder, b->inputs[i].placeholder)) {
            return 0;
        }
    }
    return 1;
}

static int memory_rejected(const unsigned char *data, size_t size, const char *needle)
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

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "fixture/app.wnui";
    const wn_document *builtin;
    wn_document *loaded;
    wn_core *core_builtin;
    wn_core *core_blob;
    const wn_rect *rects_builtin;
    const wn_rect *rects_blob;
    int count_builtin = 0;
    int count_blob = 0;
    int matching_rects = 0;
    const wn_display_list *list_builtin;
    const wn_display_list *list_blob;

    /* F1: retain/release must not lazily parse the builtin. This runs in a
     * fresh process, before wn_document_builtin() is called below. */
    check("builtin is not parsed at startup", wn_document_builtin_parsed() == 0);
    {
        wn_document *file_probe = wn_document_load_blob(path);
        long memory_length = 0;
        unsigned char *memory = read_whole_file(path, &memory_length);
        wn_document *memory_probe = NULL;

        check("file probe loads before the builtin", file_probe != NULL);
        if (file_probe != NULL) {
            wn_clear_error();
            wn_set_error("release sentinel");
            wn_document_destroy(file_probe);
            check("releasing a file document does not parse the builtin",
                  wn_document_builtin_parsed() == 0);
            check("releasing a file document leaves wn_last_error() untouched",
                  wn_last_error() != NULL &&
                      strcmp(wn_last_error(), "release sentinel") == 0);
        }

        check("memory probe read", memory != NULL && memory_length > 0);
        if (memory != NULL && memory_length > 0) {
            memory_probe = wn_document_load_blob_memory(memory, (size_t)memory_length);
            check("memory probe loads before the builtin", memory_probe != NULL);
            if (memory_probe != NULL) {
                wn_clear_error();
                wn_set_error("release sentinel");
                wn_document_destroy(memory_probe);
                check("releasing a memory document does not parse the builtin",
                      wn_document_builtin_parsed() == 0);
                check("releasing a memory document leaves wn_last_error() untouched",
                      wn_last_error() != NULL &&
                          strcmp(wn_last_error(), "release sentinel") == 0);
            }
        }
        free(memory);
    }

    builtin = wn_document_builtin();
    /* The builtin must come from the embedded blob alone: no file is open yet
     * and the fixture path is only used below. */
    check("builtin document parses without an external file",
          builtin != NULL && builtin->node_count > 0 && builtin->nodes != NULL &&
              builtin->names != NULL &&
              builtin->nodes[0].parent == -1);
    if (builtin == NULL) {
        printf("blob_test: FAIL (%s)\n", wn_last_error() != NULL ? wn_last_error() : "builtin");
        return 1;
    }

    /* F2: a stale pending error must not become the reported reason. The
     * builtin parsed above, so there is no cached loader failure to preserve. */
    {
        wn_core *null_core;
        const char *error;
        wn_clear_error();
        wn_set_error("stale");
        null_core = wn_core_create(NULL);
        error = wn_last_error();
        check("wn_core_create(NULL) reports a core error, not a stale one",
              null_core == NULL && error != NULL &&
                  strstr(error, "wn_core_create") != NULL &&
                  strstr(error, "stale") == NULL);
        wn_core_destroy(null_core);
    }

    loaded = wn_document_load_blob(path);
    if (loaded == NULL) {
        printf("blob_test: FAIL (%s)\n", wn_last_error() != NULL ? wn_last_error() : "load");
        return 1;
    }
    check("node count survives the round trip",
          wn_document_node_count(loaded) == wn_document_node_count(builtin));
    if (wn_document_node_count(loaded) != wn_document_node_count(builtin)) {
        wn_document_destroy(loaded);
        printf("blob_test: FAIL\n");
        return 1;
    }

    core_builtin = wn_core_create(builtin);
    core_blob = wn_core_create(loaded);
    check("both cores are created", core_builtin != NULL && core_blob != NULL);
    if (core_builtin == NULL || core_blob == NULL) {
        wn_core_destroy(core_builtin);
        wn_core_destroy(core_blob);
        wn_document_destroy(loaded);
        printf("blob_test: FAIL\n");
        return 1;
    }
    wn_core_resize(core_builtin, builtin->viewport_width, builtin->viewport_height);
    wn_core_resize(core_blob, builtin->viewport_width, builtin->viewport_height);
    rects_builtin = wn_core_rects(core_builtin, &count_builtin);
    rects_blob = wn_core_rects(core_blob, &count_blob);
    check("same rect count",
          count_builtin == count_blob && count_builtin == builtin->node_count);

    for (int node = 0; node < count_builtin && node < count_blob; ++node) {
        if (rects_builtin[node].x == rects_blob[node].x &&
            rects_builtin[node].y == rects_blob[node].y &&
            rects_builtin[node].w == rects_blob[node].w &&
            rects_builtin[node].h == rects_blob[node].h) {
            ++matching_rects;
        } else {
            printf("  [FAIL] rect[%d] differs: %s\n", node, builtin->names[node]);
        }
    }
    check("layout is identical", matching_rects == count_builtin);

    list_builtin = wn_core_display_list(core_builtin);
    list_blob = wn_core_display_list(core_blob);
    check("display lists match",
          list_builtin->vertex_count == list_blob->vertex_count &&
          list_builtin->glyph_vertex_count == list_blob->glyph_vertex_count &&
          list_builtin->image_vertex_count == list_blob->image_vertex_count);
    check("solid vertex bytes match",
          buffers_match(list_builtin->vertices, list_builtin->vertex_count,
                        list_blob->vertices, list_blob->vertex_count,
                        sizeof(wn_vertex)));
    check("glyph vertex bytes match",
          buffers_match(list_builtin->glyph_vertices,
                        list_builtin->glyph_vertex_count,
                        list_blob->glyph_vertices,
                        list_blob->glyph_vertex_count,
                        sizeof(wn_glyph_vertex)));
    check("image vertex bytes match",
          buffers_match(list_builtin->image_vertices,
                        list_builtin->image_vertex_count,
                        list_blob->image_vertices,
                        list_blob->image_vertex_count,
                        sizeof(wn_image_vertex)));
    printf("  nodes=%d rects=%d/%d solids=%d glyphs=%d images=%d\n",
           builtin->node_count, matching_rects, count_builtin, list_blob->vertex_count,
           list_blob->glyph_vertex_count, list_blob->image_vertex_count);

    /* ------------------------- memory loader vs the file path ------------ */
    {
        long length = 0;
        unsigned char *data = read_whole_file(path, &length);
        unsigned char *before = data != NULL ? (unsigned char *)malloc((size_t)length) : NULL;
        wn_document *from_memory = NULL;
        wn_core *memory_core = NULL;

        check("memory fixture read", data != NULL && before != NULL && length > 0);
        if (data != NULL && before != NULL) {
            memcpy(before, data, (size_t)length);
            from_memory = wn_document_load_blob_memory(data, (size_t)length);
            check("memory load succeeds", from_memory != NULL);
            if (from_memory != NULL) {
                check("memory document matches the file document",
                      documents_equal(from_memory, loaded));
                check("memory buffer is not modified",
                      memcmp(before, data, (size_t)length) == 0);
                memory_core = wn_core_create(from_memory);
                check("memory document builds a core", memory_core != NULL);
                if (memory_core != NULL) {
                    const wn_rect *memory_rects;
                    int memory_count = 0;
                    int same = 1;
                    wn_core_resize(memory_core, from_memory->viewport_width,
                                   from_memory->viewport_height);
                    memory_rects = wn_core_rects(memory_core, &memory_count);
                    if (memory_count != count_blob) {
                        same = 0;
                    } else {
                        for (int node = 0; node < memory_count; ++node) {
                            if (memory_rects[node].x != rects_blob[node].x ||
                                memory_rects[node].y != rects_blob[node].y ||
                                memory_rects[node].w != rects_blob[node].w ||
                                memory_rects[node].h != rects_blob[node].h) {
                                same = 0;
                                break;
                            }
                        }
                    }
                    check("memory pixels match the file layout", same);
                    wn_core_destroy(memory_core);
                }
                wn_document_destroy(from_memory);
            }
            free(before);
            free(data);
        }
    }

    /* --------------------------- malformed memory buffers ---------------- */
    {
        long length = 0;
        unsigned char *data = read_whole_file(path, &length);
        unsigned char small[WN_BLOB_HEADER_SIZE];

        check("malformed fixture read", data != NULL && length > 0);
        memset(small, 0, sizeof(small));
        check("truncated memory is rejected",
              memory_rejected(small, sizeof(small) - 1u, "too small"));
        check("null memory is rejected",
              memory_rejected(NULL, 0, "empty buffer"));

        if (data != NULL) {
            unsigned char *copy = (unsigned char *)malloc((size_t)length);
            WnBlobSection *sections;
            uint32_t section_count;

            check("memory copy allocated", copy != NULL);
            if (copy != NULL) {
                memcpy(copy, data, (size_t)length);
                ((WnBlobHeader *)copy)->magic = 0;
                check("bad magic in memory is rejected",
                      memory_rejected(copy, (size_t)length, "bad magic"));

                memcpy(copy, data, (size_t)length);
                ((WnBlobHeader *)copy)->version = WN_BLOB_VERSION + 1u;
                check("bad version in memory is rejected",
                      memory_rejected(copy, (size_t)length, "bad magic or version"));

                memcpy(copy, data, (size_t)length);
                ((WnBlobHeader *)copy)->file_size = (uint32_t)length + 1u;
                check("wrong memory size in the header is rejected",
                      memory_rejected(copy, (size_t)length, "inconsistent header"));

                memcpy(copy, data, (size_t)length);
                sections = (WnBlobSection *)(copy + WN_BLOB_HEADER_SIZE);
                section_count = ((WnBlobHeader *)copy)->section_count;
                for (uint32_t i = 0; i < section_count; ++i) {
                    if (sections[i].id == WN_BLOB_SECTION_NODES) {
                        sections[i].offset = (uint32_t)length;
                        break;
                    }
                }
                check("section offset past the end is rejected",
                      memory_rejected(copy, (size_t)length, "missing required section"));

                memcpy(copy, data, (size_t)length);
                {
                    /* Valid magic/version, but the section table does not fit
                     * in the buffer: the loader must not read past `size`. */
                    size_t cut = WN_BLOB_HEADER_SIZE + sizeof(WnBlobSection);
                    memcpy(copy, data, cut);
                    ((WnBlobHeader *)copy)->file_size = (uint32_t)cut;
                    check("truncated section table is rejected",
                          memory_rejected(copy, cut, "inconsistent header"));
                }

                free(copy);
            }
            free(data);
        }
    }

    {
        const char *corrupt = "corrupt_nul.wnui";
        wn_document *rejected;
        const char *error;
        int copied = write_unterminated_copy(path, corrupt);
        check("unterminated copy was written", copied);
        if (copied) {
            rejected = wn_document_load_blob(corrupt);
            error = wn_last_error();
            check("unterminated string is rejected",
                  rejected == NULL && error != NULL &&
                      strstr(error, "unterminated") != NULL);
            printf("  error: %s\n", error != NULL ? error : "(none)");
            if (rejected != NULL) {
                wn_document_destroy(rejected);
            }
            remove(corrupt);
        }
    }

    {
        const char *deep = "deep_chain.wnui";
        const char *limit = "limit_chain.wnui";
        const char *oversized = "oversized_nodes.wnui";
        wn_document *rejected;
        const char *error;

        check("deep chain written", write_chain_blob(deep, WN_BLOB_MAX_DEPTH + 2u));
        rejected = wn_document_load_blob(deep);
        error = wn_last_error();
        check("deep chain is rejected",
              rejected == NULL && error != NULL &&
                  strstr(error, "deeper than") != NULL);
        printf("  error: %s\n", error != NULL ? error : "(none)");
        if (rejected != NULL) {
            wn_document_destroy(rejected);
        }
        remove(deep);

        check("limit chain written", write_chain_blob(limit, WN_BLOB_MAX_DEPTH));
        rejected = wn_document_load_blob(limit);
        check("depth limit is inclusive", rejected != NULL);
        if (rejected != NULL) {
            wn_document_destroy(rejected);
        }
        remove(limit);

        check("oversized header written", write_oversized_header(oversized));
        rejected = wn_document_load_blob(oversized);
        error = wn_last_error();
        check("node cap is enforced",
              rejected == NULL && error != NULL &&
                  strstr(error, "too many nodes") != NULL);
        printf("  error: %s\n", error != NULL ? error : "(none)");
        if (rejected != NULL) {
            wn_document_destroy(rejected);
        }
        remove(oversized);
    }

    {
        const char *view = "zero_viewport.wnui";
        const char *bad_input = "bad_input_node.wnui";
        wn_document *rejected;
        const char *error;
        int has_inputs = 0;

        check("zero viewport copy written", write_zero_viewport_copy(path, view));
        rejected = wn_document_load_blob(view);
        error = wn_last_error();
        check("zero viewport is rejected",
              rejected == NULL && error != NULL &&
                  strstr(error, "viewport") != NULL);
        printf("  error: %s\n", error != NULL ? error : "(none)");
        if (rejected != NULL) {
            wn_document_destroy(rejected);
        }
        remove(view);

        if (write_bad_input_copy(path, bad_input, &has_inputs)) {
            rejected = wn_document_load_blob(bad_input);
            error = wn_last_error();
            check("input node range is enforced",
                  rejected == NULL && error != NULL &&
                      strstr(error, "input 0 references node") != NULL);
            printf("  error: %s\n", error != NULL ? error : "(none)");
            if (rejected != NULL) {
                wn_document_destroy(rejected);
            }
            remove(bad_input);
        } else if (has_inputs) {
            check("bad input node copy written", 0);
        } else {
            printf("  (no inputs section in this fixture)\n");
        }
    }

    {
        const char *bad_font = "bad_font_size.wnui";
        wn_document *rejected;
        const char *error;
        int has_font = 0;

        if (write_bad_font_size_copy(path, bad_font, &has_font)) {
            rejected = wn_document_load_blob(bad_font);
            error = wn_last_error();
            check("font size range is enforced",
                  rejected == NULL && error != NULL &&
                      strstr(error, "font_size") != NULL);
            printf("  error: %s\n", error != NULL ? error : "(none)");
            if (rejected != NULL) {
                wn_document_destroy(rejected);
            }
            remove(bad_font);
        } else if (has_font) {
            check("bad font size copy written", 0);
        } else {
            printf("  (no sized node in this fixture)\n");
        }
    }

    {
        /* The loader accepts the inclusive range and rejects everything else,
         * including NaN (both comparisons false) and infinity. */
        static const struct {
            float value;
            int accepted;
            const char *label;
        } opacity_cases[] = {
            { 0.0f, 1, "opacity 0 is accepted" },
            { 1.0f, 1, "opacity 1 is accepted" },
            { -0.001f, 0, "negative opacity is rejected" },
            { 1.001f, 0, "opacity above 1 is rejected" },
            { (float)NAN, 0, "NaN opacity is rejected" },
            { (float)INFINITY, 0, "infinite opacity is rejected" },
        };
        const char *opacity_file = "opacity_range.wnui";
        int c;
        for (c = 0; c < (int)(sizeof(opacity_cases) / sizeof(opacity_cases[0])); ++c) {
            wn_document *candidate = NULL;
            const char *error;
            int written = write_opacity_copy(path, opacity_file,
                                             opacity_cases[c].value);
            if (written) {
                candidate = wn_document_load_blob(opacity_file);
            }
            error = wn_last_error();
            if (opacity_cases[c].accepted) {
                check(opacity_cases[c].label, written && candidate != NULL);
            } else {
                check(opacity_cases[c].label,
                      written && candidate == NULL && error != NULL &&
                          strstr(error, "opacity") != NULL);
                printf("  error: %s\n", error != NULL ? error : "(none)");
            }
            if (candidate != NULL) {
                wn_document_destroy(candidate);
            }
            if (written) {
                remove(opacity_file);
            }
        }
    }

    wn_core_destroy(core_builtin);
    wn_core_destroy(core_blob);
    wn_document_destroy(loaded);

    if (failures != 0 || matching_rects != count_builtin) {
        printf("blob_test: FAIL (%d/%d checks failed)\n", failures, checks);
        return 1;
    }
    printf("blob_test: PASS (%d checks)\n", checks);
    return 0;
}

