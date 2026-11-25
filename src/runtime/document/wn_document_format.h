/*
 * wn_document_format.h - POD tables shared by the generated header and the
 * blob loader. The types keep their historical names because ui_data.h and
 * the header parsers reference them.
 */
#ifndef WN_DOCUMENT_FORMAT_H
#define WN_DOCUMENT_FORMAT_H

#include <stdint.h>

typedef struct {
    int32_t cp;                 /* ASCII printable 32..126 */
    int32_t gid;                /* diagnostic */
    float advance;              /* isolated advance, px */
    float x, y;                 /* box relative to the input content box */
    float w, h;
    float u0, v0, u1, v1;       /* R8 atlas UVs */
} UiInputGlyph;

typedef struct {
    int32_t node;               /* UiNode id (NODE_INPUT) */
    const char *value;          /* initial ASCII value */
    const char *placeholder;    /* ASCII, empty = none */
    int32_t max_length;         /* 0 = no limit (cap 255) */
    int32_t password;           /* reserved */
    float baseline;             /* ascender from the content box */
    float line_height;          /* ascender + descender */
    int32_t glyph_first;        /* first glyph in the pool */
    int32_t glyph_count;
} UiInput;

#endif
