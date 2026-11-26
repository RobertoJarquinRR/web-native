#ifndef WN_BLOB_H
#define WN_BLOB_H

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define WN_BLOB_MAGIC 0x49554E57u /* "WNUI" */
#define WN_BLOB_VERSION 3u
#define WN_BLOB_HEADER_SIZE 32u

/* Loader limits: the layout/paint walk is recursive, so a crafted chain would
 * otherwise overflow the stack. Both are rejected before a core is built. */
#define WN_BLOB_MAX_NODES 65536u
#define WN_BLOB_MAX_DEPTH 512

/* The text backend clamps at paint time; reject out-of-range sizes up front so
 * a document cannot claim text it will never draw. */
#define WN_BLOB_MIN_FONT_SIZE 1.0f
#define WN_BLOB_MAX_FONT_SIZE 1024.0f

typedef enum {
    WN_BLOB_SECTION_NODES = 1,
    WN_BLOB_SECTION_NAMES = 2,
    WN_BLOB_SECTION_STRINGS = 3,
    WN_BLOB_SECTION_INPUTS = 4
} wn_blob_section_id;

typedef struct {
    uint32_t id;
    uint32_t offset;
    uint32_t size;
    uint32_t reserved;
} WnBlobSection;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t section_count;
    uint32_t file_size;
    uint32_t node_count;
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t reserved;
} WnBlobHeader;

/* Fixed-width on-disk projection of UiNode: every string becomes a byte
 * offset into the strings section (0 means NULL). The field order and width
 * must match the BLOB_NODE struct that the Rust frontend writes. */
typedef struct {
    int32_t parent;
    int32_t kind;
    int32_t direction;
    int32_t justify;
    int32_t align;
    int32_t wrap;
    float width;
    float height;
    float width_pct;
    float height_pct;
    float margin[4];
    float padding[4];
    uint32_t background;
    uint32_t text;
    uint32_t hover_bg;
    uint32_t press_bg;
    uint32_t selected_bg;
    int32_t action;
    float gap;
    float font_size;
    uint32_t text_color;
    int32_t grid_mode;
    float grid_column_px;
    float grid_auto_rows;
    float row_gap;
    float column_gap;
    int32_t overflow;
    float image_width;
    float image_height;
    uint32_t image_src;
    uint32_t image_alt;
    int32_t object_fit;
    float border_width;
    uint32_t border_color;
    float border_radius;
    float shadow_x;
    float shadow_y;
    float shadow_blur;
    float shadow_spread;
    uint32_t shadow_color;
    int32_t text_align;
    float line_height;
    int32_t margin_auto;
    int32_t box_sizing;
    float flex_grow;
    float flex_shrink;
    float flex_basis;
    uint32_t font_family;
    int32_t font_weight;
    float opacity;
} WnBlobNode;

typedef struct {
    int32_t node;
    uint32_t value;       /* strings offset */
    uint32_t placeholder; /* strings offset */
    int32_t max_length;
    int32_t password;
} WnBlobInput;

struct wn_document;

/* Parse a .wnui image already resident in memory. The buffer is validated
 * exactly like wn_document_load_blob() but is never modified or freed: the
 * document borrows the string pool.
 *
 * Internal loader. Documents are reference counted and wn_core_create()
 * retains the one it is given, so `data` must stay alive and unchanged until
 * the LAST reference is released - not merely until the caller's
 * wn_document_destroy(), which only drops the caller's reference. The builtin
 * document satisfies this with static storage (kBuiltinBlob). */
struct wn_document *wn_document_load_blob_memory(const void *data, size_t size);

#if defined(__cplusplus)
}
#endif

#endif
