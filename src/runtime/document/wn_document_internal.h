#ifndef WN_DOCUMENT_INTERNAL_H
#define WN_DOCUMENT_INTERNAL_H

#include "document/wn_document_format.h"
#include "wn_core.h"

/* ABI v6: documents are reference counted. The builtin document uses this
 * sentinel so retain/release never touch its storage. */
#define WN_DOCUMENT_STATIC_REFS 0x7FFFFFFF

struct wn_document {
    const UiNode *nodes;
    int node_count;
    const char *const *names;
    int viewport_width;
    int viewport_height;
    const UiInput *inputs;
    int input_count;

    /* Owned by the loader when the document came from a .wnui file. */
    void *mapping;
    void *mapping_handle;
    /* 1 only when this document owns the mapping above. Documents parsed from
     * a caller-owned memory buffer leave it 0 and never unmap. */
    int owns_mapping;
    UiNode *owned_nodes;
    const char **owned_names;
    UiInput *owned_inputs;
    /* Blob v6 state styles; UiNode.state_styles points into this block. */
    UiStateStyle *owned_states;

    int refs;
};

#endif
