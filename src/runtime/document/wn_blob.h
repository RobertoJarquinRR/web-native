#ifndef WN_BLOB_H
#define WN_BLOB_H

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define WN_BLOB_MAGIC 0x49554E57u /* "WNUI" */
#define WN_BLOB_VERSION 7u
#define WN_BLOB_HEADER_SIZE 32u

/* Blob v7: `<video>` is a first-class node kind. A video node carries box and
 * style like any other node; the media-specific intent travels in the optional
 * section 7. Reusing kind 5 (image) with a video path is explicitly rejected
 * by the contract: identity decides layout, hit-test, state and lifecycle. */
#define WN_BLOB_NODE_VIDEO 6

/* Blob v5: `z_index` uses INT32_MIN as the "auto" sentinel, so every other
 * int32 is a valid explicit CSS `z-index` (negative, zero and positive).
 * An explicit `z-index: -2147483648` is not representable in the blob; the
 * frontend diagnoses it and falls back to auto. */
#define WN_BLOB_Z_INDEX_AUTO INT32_MIN

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
    WN_BLOB_SECTION_INPUTS = 4,
    /* Blob v6: optional per-node state styles. Absent when no node has one. */
    WN_BLOB_SECTION_STATES = 5,
    /* Blob v6: optional per-node text extras (ellipsis). Additive section;
     * absent when no text run carries a flag. */
    WN_BLOB_SECTION_TEXT_EXTRAS = 6,
    /* Blob v7: optional per-video media intent. Absent when the document has
     * no kind==video node. */
    WN_BLOB_SECTION_MEDIA = 7
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
    /* Blob v4: positioning. `position` is 0 static, 1 relative, 2 absolute,
     * 3 fixed, 4 sticky. `inset` is top,right,bottom,left; `inset_mask` bit
     * 0..3 marks a declared side and bit 4..7 marks that side as a percentage
     * (value stays a percentage number; the runtime resolves it against the
     * containing block). Mask 0 = all four insets are `auto`. */
    int32_t position;
    float inset[4];
    uint32_t inset_mask;
    /* Blob v5: CSS `z-index`. WN_BLOB_Z_INDEX_AUTO (INT32_MIN) means `auto`;
     * every other int32 is an explicit stacking level. A positioned node
     * (relative/absolute/fixed/sticky) or a static flex/grid item with a value
     * other than auto creates a stacking context; z-index on any other static
     * node is ignored by the runtime, matching CSS. */
    int32_t z_index;
} WnBlobNode;

typedef struct {
    int32_t node;
    uint32_t value;       /* strings offset */
    uint32_t placeholder; /* strings offset */
    int32_t max_length;
    int32_t password;
} WnBlobInput;

/* Blob v6 optional states section. A sequence of records:
 *
 *   WnBlobStateRecord { int32 node; uint8 state_count; uint8 initial_state;
 *                        uint8 reserved[2]; }
 *   followed by `state_count` WnBlobStateStyle entries (16 bytes each).
 *
 * Only nodes with at least one non-default state carry a record. The loader
 * validates the node index, the state mask (1..4 unique states), the presence
 * bits and the record boundaries.
 */
typedef struct {
    uint8_t state;       /* 0 hover, 1 press, 2 focus, 3 disabled,
                          * 4 focus-visible, 5 focus-within */
    uint8_t present;     /* bit0 background, bit1 text, bit2 border,
                          * bit3 font-weight (in `reserved`) */
    uint16_t reserved;   /* v6 additive: state font-weight 100..900 */
    uint32_t background; /* ARGB, valid when present bit 0 */
    uint32_t text;       /* ARGB, valid when present bit 1 */
    uint32_t border_color; /* ARGB, valid when present bit 2 */
} WnBlobStateStyle;

typedef struct {
    int32_t node;
    uint8_t state_count;    /* 0..6 style entries */
    uint8_t initial_state;  /* runtime bits seeded at load (disabled = 0x10) */
    uint8_t reserved[2];
    /* WnBlobStateStyle entries follow in the section. */
} WnBlobStateRecord;

/* Blob v6 optional text extras section. A sequence of 8-byte records in
 * strictly increasing node order:
 *
 *   WnBlobTextExtras { int32 node; uint8 flags; uint8 reserved[3]; }
 *
 * Only text nodes with a nonzero flag byte carry a record. Bit 0 marks
 * `text-overflow: ellipsis` on a clipped (`overflow: hidden`) box. */
typedef struct {
    int32_t node;
    uint8_t flags;
    uint8_t reserved[3];
} WnBlobTextExtras;

#define WN_TEXT_FLAG_ELLIPSIS 0x01u

/* Blob v7 optional media section (id 7). A sequence of 16-byte records in
 * strictly increasing node order, one per kind==video node:
 *
 *   WnBlobMediaRecord { int32 node; uint32 src; uint32 poster;
 *                       uint8 flags; uint8 preload; uint8 reserved[2]; }
 *
 * `src` is a mandatory non-empty strings offset and must not contain a scheme
 * (`://`). `poster` is a strings offset or 0. `flags` uses WN_MEDIA_FLAG_* and
 * bits 4..7 are rejected. `preload` is 0 none, 1 metadata (default), 2 auto.
 * The section is absent when the document has no video node, every video node
 * has exactly one record, and a record may not target any other kind.
 */
#define WN_MEDIA_FLAG_CONTROLS 0x01u
#define WN_MEDIA_FLAG_AUTOPLAY 0x02u
#define WN_MEDIA_FLAG_LOOP     0x04u
#define WN_MEDIA_FLAG_MUTED    0x08u
#define WN_MEDIA_FLAG_ALL      0x0Fu

#define WN_MEDIA_PRELOAD_NONE     0u
#define WN_MEDIA_PRELOAD_METADATA 1u
#define WN_MEDIA_PRELOAD_AUTO     2u

typedef struct {
    int32_t  node;       /* node index; kind must be 6 */
    uint32_t src;        /* strings offset; mandatory, != 0, non-empty */
    uint32_t poster;     /* strings offset; 0 = no poster */
    uint8_t  flags;      /* WN_MEDIA_FLAG_* */
    uint8_t  preload;    /* 0 none, 1 metadata, 2 auto */
    uint8_t  reserved[2];/* 0 */
} WnBlobMediaRecord;

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
