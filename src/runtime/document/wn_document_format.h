/*
 * wn_document_format.h - POD tables shared by the generated header and the
 * blob loader. The types keep their historical names because ui_data.h and
 * the header parsers reference them.
 */
#ifndef WN_DOCUMENT_FORMAT_H
#define WN_DOCUMENT_FORMAT_H

#include <stdint.h>

typedef struct {
    int32_t node;               /* UiNode id (NODE_INPUT) */
    const char *value;          /* initial value */
    const char *placeholder;    /* empty = none */
    int32_t max_length;         /* 0 = no limit (cap 255) */
    int32_t password;           /* reserved */
} UiInput;

#endif
