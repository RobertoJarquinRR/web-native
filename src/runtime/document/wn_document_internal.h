#ifndef WN_DOCUMENT_INTERNAL_H
#define WN_DOCUMENT_INTERNAL_H

#include "document/wn_document_format.h"
#include "ui_data.h"
#include "wn_core.h"

/* ABI v6: documents are reference counted. The builtin compiled document uses
 * this sentinel so retain/release never touch its storage. */
#define WN_DOCUMENT_STATIC_REFS 0x7FFFFFFF

struct wn_document {
    const UiNode *nodes;
    int node_count;
    const char *const *names;
    int viewport_width;
    int viewport_height;
    const UiInput *inputs;
    int input_count;

    /* Owned by the loader when the document came from a .wnui blob. */
    void *mapping;
    void *mapping_handle;
    UiNode *owned_nodes;
    const char **owned_names;
    UiInput *owned_inputs;

    int refs;
};

#endif
