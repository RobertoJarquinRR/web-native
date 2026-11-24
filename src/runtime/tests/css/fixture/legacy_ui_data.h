/* Old-format (pre CSS core v1) header fixture: two nodes, no border/shadow
 * tail. `emit_ui.py --compare` and parse_header() must still accept it and
 * default every CSS v1 field to 0. Do not regenerate. */
#ifndef UI_DATA_H
#define UI_DATA_H

typedef enum { R2_NODE_BLOCK, R2_NODE_FLEX, R2_NODE_TEXT, R2_NODE_GRID, R2_NODE_IMAGE } NodeKind;
typedef enum { JUSTIFY_START, JUSTIFY_CENTER, JUSTIFY_END, JUSTIFY_SPACE_BETWEEN } Justify;
typedef enum { ALIGN_START, ALIGN_CENTER, ALIGN_END, ALIGN_STRETCH } Align;
typedef enum { ACTION_NONE = 0, ACTION_SELECT = 1 } Action;
typedef enum { R2_GRID_NONE = 0, R2_GRID_AUTO_FILL = 1 } GridMode;
typedef enum { R2_OVERFLOW_VISIBLE = 0, R2_OVERFLOW_HIDDEN = 1, R2_OVERFLOW_SCROLL = 2 } OverflowMode;
typedef enum { R2_OBJECT_FIT_FILL = 0, R2_OBJECT_FIT_COVER = 1, R2_OBJECT_FIT_CONTAIN = 2 } ObjectFit;

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
} UiNode;

#define UI_NODE_COUNT 2
#define UI_VIEWPORT_WIDTH 120
#define UI_VIEWPORT_HEIGHT 80

static const UiNode kUiNodes[UI_NODE_COUNT] = {
    { -1, 1, 0, 0, 0, 0, -1.0000f, -1.0000f, 100.0000f, 100.0000f, { 0.0000f, 0.0000f, 0.0000f, 0.0000f }, { 0.0000f, 0.0000f, 0.0000f, 0.0000f }, 0xFFFFFFFFu, NULL, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0.0000f, 16.0000f, 0xFF000000u, 0, 0.0000f, -1.0000f, 0.0000f, 0.0000f, 0, 0.0000f, 0.0000f, NULL, NULL, 0 },
    { 0, 0, 1, 0, 3, 0, 40.0000f, 40.0000f, 0.0000f, 0.0000f, { 20.0000f, 20.0000f, 0.0000f, 20.0000f }, { 0.0000f, 0.0000f, 0.0000f, 0.0000f }, 0xFF000000u, NULL, 0x00000000u, 0x00000000u, 0x00000000u, 0, 0.0000f, 16.0000f, 0xFF000000u, 0, 0.0000f, -1.0000f, 0.0000f, 0.0000f, 0, 0.0000f, 0.0000f, NULL, NULL, 0 },
};

static const char *const kUiNodeNames[UI_NODE_COUNT] = {
    "body.root",
    "body.root/div.card",
};

/* Pre-shaped text (HarfBuzz positions, FreeType R8 bitmaps). */
#define UI_ATLAS_SIZE 1
#define UI_GLYPH_COUNT 0
#define UI_TEXT_RUN_COUNT 0

static const unsigned char kUiAtlas[1] = {0};

typedef struct {
    float x, y, w, h;          /* relative to the text node top-left */
    float u0, v0, u1, v1;      /* R8 atlas UVs */
} UiGlyph;

typedef struct {
    int node;                  /* UiNode id (NODE_TEXT) */
    int first;                 /* first glyph in kUiGlyphs */
    int count;
    float baseline;            /* ascender inside the line box */
} UiTextRun;

static const UiGlyph kUiGlyphs[UI_GLYPH_COUNT > 0 ? UI_GLYPH_COUNT : 1] = {
    { 0, 0, 0, 0, 0, 0, 0, 0 },
};

static const UiTextRun kUiTextRuns[UI_TEXT_RUN_COUNT > 0 ? UI_TEXT_RUN_COUNT : 1] = {
    { -1, 0, 0, 0.0f },
};

#endif /* UI_DATA_H */
