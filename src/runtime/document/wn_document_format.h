/*
 * wn_document_format.h - POD tables the runtime uses for every document.
 *
 * The blob loader builds these from a .wnui file; the builtin document is an
 * embedded .wnui blob parsed by the same loader. Historical type names are
 * kept so the rest of the core does not change.
 */
#ifndef WN_DOCUMENT_FORMAT_H
#define WN_DOCUMENT_FORMAT_H

#include <stdint.h>

/* Blob v5: CSS `z-index`. INT32_MIN means `auto`. Any other int32 is an
 * explicit stacking level (negative, zero or positive). */
#define R2_Z_INDEX_AUTO INT32_MIN

/* The runtime always carries the E1/CSS style extras. Blob v3 writes every
 * field for every node, so a page that does not use them gets the same
 * defaults the old header-less builds used (text_align 0, line_height 0,
 * margin_auto 0, box_sizing 0, flex_grow 0, flex_shrink 1, flex_basis -1,
 * opacity 1). */
#define UI_STYLE_EXTRAS 1

typedef enum {
    R2_NODE_BLOCK,
    R2_NODE_FLEX,
    R2_NODE_TEXT,
    R2_NODE_GRID,
    R2_NODE_IMAGE,
    R2_NODE_INPUT
} NodeKind;

typedef enum {
    JUSTIFY_START,
    JUSTIFY_CENTER,
    JUSTIFY_END,
    JUSTIFY_SPACE_BETWEEN
} Justify;

typedef enum {
    ALIGN_START,
    ALIGN_CENTER,
    ALIGN_END,
    ALIGN_STRETCH
} Align;

typedef enum { ACTION_NONE = 0, ACTION_SELECT = 1 } Action;
typedef enum { R2_GRID_NONE = 0, R2_GRID_AUTO_FILL = 1 } GridMode;
typedef enum {
    R2_OVERFLOW_VISIBLE = 0,
    R2_OVERFLOW_HIDDEN = 1,
    R2_OVERFLOW_SCROLL = 2
} OverflowMode;
typedef enum {
    R2_OBJECT_FIT_FILL = 0,
    R2_OBJECT_FIT_COVER = 1,
    R2_OBJECT_FIT_CONTAIN = 2
} ObjectFit;
typedef enum {
    R2_POSITION_STATIC = 0,
    R2_POSITION_RELATIVE = 1,
    R2_POSITION_ABSOLUTE = 2
} PositionMode;

typedef struct {
    int parent;                 /* -1 root */
    int kind;
    int direction;              /* 0 row, 1 column */
    int justify, align, wrap;
    float width, height;        /* px, -1 auto */
    float width_pct, height_pct;/* 0 none, resolved at runtime */
    float margin[4], padding[4];/* top,right,bottom,left */
    unsigned int background;    /* 0xAARRGGBB, 0 none */
    const char *text;           /* NODE_TEXT only */
    unsigned int hover_bg, press_bg, selected_bg;
    int action;
    /* R2 extensions (documented in README) */
    float gap;                  /* main-axis gap, px */
    float font_size;            /* text nodes, build-time metric */
    unsigned int text_color;    /* text nodes, 0xAARRGGBB */
    /* ui-tree/2: grid, scroll and images */
    int grid_mode;              /* R2_GRID_* */
    float grid_column_px;       /* repeat(auto-fill, <px>) */
    float grid_auto_rows;       /* px, -1 auto */
    float row_gap, column_gap;  /* resolved gaps, px */
    int overflow;               /* R2_OVERFLOW_* */
    float image_width, image_height; /* intrinsic px, 0 unknown */
    const char *image_src;      /* local path, NODE_IMAGE only */
    const char *image_alt;
    int object_fit;             /* R2_OBJECT_FIT_* */
    /* CSS core v1: rounded background, uniform border, first shadow. */
    float border_width;         /* px, 0 none */
    unsigned int border_color;  /* 0xAARRGGBB, 0 none */
    float border_radius;        /* uniform px, 0 square */
    float shadow_x, shadow_y;   /* first non-inset shadow offset, px */
    float shadow_blur, shadow_spread;
    unsigned int shadow_color;  /* 0xAARRGGBB, 0 none */
    /* E1: text alignment, line box, auto margins and box model. */
    int text_align;             /* 0 left, 1 center, 2 right */
    float line_height;          /* px, 0 = font metrics */
    int margin_auto;            /* bit0 top, 1 right, 2 bottom, 3 left */
    int box_sizing;             /* 0 content-box (CSS default), 1 border-box */
    float flex_grow;            /* >= 0 */
    float flex_shrink;          /* >= 0, default 1 */
    float flex_basis;           /* px, -1 = auto */
    const char *font_family;    /* text nodes, CSS family */
    int font_weight;            /* text nodes, 100..900 */
    /* Blob v3: CSS group opacity in [0, 1], 1 = opaque. A node below 1 is a
     * group: the runtime renders its subtree into an offscreen layer and
     * composites it once. Not inherited. */
    float opacity;
    /* Blob v4: positioning. `position` is R2_POSITION_*; `inset` is
     * top,right,bottom,left; `inset_mask` bit 0..3 marks a declared side and
     * bit 4..7 marks a percentage value. `relative` offsets after normal flow;
     * `absolute` leaves the flow and resolves against the nearest positioned
     * ancestor (or the root). */
    int position;
    float inset[4];
    unsigned int inset_mask;
    /* Blob v5: CSS `z-index`; R2_Z_INDEX_AUTO means `auto`. A positioned node
     * with an explicit value is a stacking context (paint order only, no
     * offscreen texture). Ignored on a static non-flex/grid node. */
    int z_index;
} UiNode;

typedef struct {
    int32_t node;               /* UiNode id (NODE_INPUT) */
    const char *value;          /* initial value */
    const char *placeholder;    /* empty = none */
    int32_t max_length;         /* 0 = no limit (cap 255) */
    int32_t password;           /* reserved */
} UiInput;

#endif
