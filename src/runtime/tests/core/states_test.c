/*
 * states_test.c - blob v6 CSS state styles + ABI v7 focus/disabled bits.
 *
 * Builds blobs in memory (no HTML/CSS, no GPU):
 *
 *   bits       - set/get WN_STATE_FOCUSED and WN_STATE_DISABLED.
 *   priority   - disabled > press > hover > focus > base per color field.
 *   selected   - selected is the app base; a CSS state overrides it.
 *   disabled   - no hover/press/selection; hit-test stays diagnostic.
 *   focus      - input focus sets/clears the focused bit.
 *   focusmatrix- visible implies focused, clearing rules, multiple focused
 *                nodes, remove/reinsert re-derives focus-within, disabled
 *                refuses the visible bit, fixed focus-family priority and the
 *                focused node's own focus-within weight requesting relayout.
 *   loader     - bad node index, duplicate state and present mask are rejected.
 *
 *   states_test.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/wn_core_internal.h"
#include "document/wn_blob.h"
#include "document/wn_document_format.h"
#include "document/wn_document_internal.h"
#include "wn_core.h"

static int checks = 0;
static int failures = 0;

/* wn_document_load_blob_memory() borrows the buffer: the document keeps
 * pointing into it for its whole life (and the core retains the document), so
 * the test must not free the blob until every core/document that borrowed it is
 * gone. The leases are released once in main() after the last destroy. */
typedef struct BlobLease {
    unsigned char *data;
    struct BlobLease *next;
} BlobLease;

static BlobLease *leases;

static void lease_blob(unsigned char *data)
{
    BlobLease *lease = (BlobLease *)malloc(sizeof(*lease));
    if (lease == NULL) {
        /* The test is doomed either way; keep the buffer alive rather than
         * freeing memory the document borrows. */
        return;
    }
    lease->data = data;
    lease->next = leases;
    leases = lease;
}

static void release_leases(void)
{
    while (leases != NULL) {
        BlobLease *next = leases->next;
        free(leases->data);
        free(leases);
        leases = next;
    }
}

static int check(const char *label, int ok, const char *detail)
{
    checks += 1;
    if (!ok) {
        failures += 1;
    }
    printf("  %s %s%s%s\n", ok ? "[ok]" : "[FAIL]", label,
           detail != NULL ? " - " : "", detail != NULL ? detail : "");
    return ok;
}

typedef struct {
    int parent;
    int kind;
    float width;
    float height;
    unsigned int background;
    unsigned int text_color;
    float border_width;
    unsigned int border_color;
    int font_weight;
    int action;
    unsigned int hover_bg;
    unsigned int press_bg;
    unsigned int selected_bg;
} NodeSpec;

typedef struct {
    unsigned char state;
    unsigned char present;
    unsigned short font_weight;
    unsigned int background;
    unsigned int text;
    unsigned int border_color;
} StateSpec;

typedef struct {
    int node;
    int count;
    unsigned char initial_state;
    StateSpec entries[4];
} StateRecordSpec;

static uint32_t align_up8(uint32_t value)
{
    return (value + 7u) & ~7u;
}

static const char kStrings[] = "\0";

static unsigned char *build_blob(const NodeSpec *spec, uint32_t count,
                                 const StateRecordSpec *states, uint32_t state_count,
                                 const int *input_node, uint32_t viewport_w,
                                 uint32_t viewport_h, size_t *out_size)
{
    uint32_t section_count = 3u + (state_count > 0 ? 1u : 0u) +
                             (input_node != NULL ? 1u : 0u);
    uint32_t offset = WN_BLOB_HEADER_SIZE + section_count * (uint32_t)sizeof(WnBlobSection);
    uint32_t nodes_offset = align_up8(offset);
    uint32_t nodes_bytes = count * (uint32_t)sizeof(WnBlobNode);
    uint32_t names_offset = align_up8(nodes_offset + nodes_bytes);
    uint32_t names_bytes = count * (uint32_t)sizeof(uint32_t);
    uint32_t strings_offset = align_up8(names_offset + names_bytes);
    uint32_t strings_bytes = (uint32_t)sizeof(kStrings);
    uint32_t states_offset = align_up8(strings_offset + strings_bytes);
    uint32_t states_bytes = 0;
    uint32_t inputs_offset;
    uint32_t inputs_bytes = (uint32_t)sizeof(WnBlobInput);
    uint32_t total;
    unsigned char *data;
    WnBlobHeader *header;
    WnBlobSection *sections;
    WnBlobNode *nodes;
    uint32_t i;
    uint32_t section = 0;

    for (i = 0; i < state_count; ++i) {
        states_bytes += (uint32_t)sizeof(WnBlobStateRecord) +
                        (uint32_t)states[i].count * (uint32_t)sizeof(WnBlobStateStyle);
    }
    inputs_offset = align_up8(states_offset + states_bytes);
    total = align_up8(inputs_offset + (input_node != NULL ? inputs_bytes : 0u));
    data = (unsigned char *)calloc(1, total);
    if (data == NULL) {
        return NULL;
    }
    header = (WnBlobHeader *)data;
    header->magic = WN_BLOB_MAGIC;
    header->version = WN_BLOB_VERSION;
    header->section_count = section_count;
    header->file_size = total;
    header->node_count = count;
    header->viewport_width = viewport_w;
    header->viewport_height = viewport_h;

    sections = (WnBlobSection *)(data + WN_BLOB_HEADER_SIZE);
    sections[section].id = WN_BLOB_SECTION_NODES;
    sections[section].offset = nodes_offset;
    sections[section].size = nodes_bytes;
    ++section;
    sections[section].id = WN_BLOB_SECTION_NAMES;
    sections[section].offset = names_offset;
    sections[section].size = names_bytes;
    ++section;
    sections[section].id = WN_BLOB_SECTION_STRINGS;
    sections[section].offset = strings_offset;
    sections[section].size = strings_bytes;
    ++section;

    nodes = (WnBlobNode *)(data + nodes_offset);
    for (i = 0; i < count; ++i) {
        nodes[i].parent = spec[i].parent;
        nodes[i].kind = spec[i].kind;
        nodes[i].width = spec[i].width;
        nodes[i].height = spec[i].height;
        nodes[i].background = spec[i].background;
        nodes[i].text_color = spec[i].text_color;
        nodes[i].border_width = spec[i].border_width;
        nodes[i].border_color = spec[i].border_color;
        nodes[i].font_weight = spec[i].font_weight > 0 ? spec[i].font_weight : 400;
        nodes[i].action = spec[i].action;
        nodes[i].hover_bg = spec[i].hover_bg;
        nodes[i].press_bg = spec[i].press_bg;
        nodes[i].selected_bg = spec[i].selected_bg;
        nodes[i].font_size = 16.0f;
        nodes[i].flex_shrink = 1.0f;
        nodes[i].flex_basis = -1.0f;
        nodes[i].opacity = 1.0f;
        nodes[i].grid_auto_rows = -1.0f;
        nodes[i].z_index = R2_Z_INDEX_AUTO;
    }
    memset(data + names_offset, 0, names_bytes);
    memcpy(data + strings_offset, kStrings, sizeof(kStrings));

    if (state_count > 0) {
        unsigned char *cursor = data + states_offset;
        sections[section].id = WN_BLOB_SECTION_STATES;
        sections[section].offset = states_offset;
        sections[section].size = states_bytes;
        ++section;
        for (i = 0; i < state_count; ++i) {
            WnBlobStateRecord record;
            int entry;
            record.node = states[i].node;
            record.state_count = (uint8_t)states[i].count;
            record.initial_state = states[i].initial_state;
            memset(record.reserved, 0, sizeof(record.reserved));
            memcpy(cursor, &record, sizeof(record));
            cursor += sizeof(record);
            for (entry = 0; entry < states[i].count; ++entry) {
                WnBlobStateStyle style;
                memset(&style, 0, sizeof(style));
                style.state = states[i].entries[entry].state;
                style.present = states[i].entries[entry].present;
                style.reserved = states[i].entries[entry].font_weight;
                style.background = states[i].entries[entry].background;
                style.text = states[i].entries[entry].text;
                style.border_color = states[i].entries[entry].border_color;
                memcpy(cursor, &style, sizeof(style));
                cursor += sizeof(style);
            }
        }
    }

    if (input_node != NULL) {
        WnBlobInput *input = (WnBlobInput *)(data + inputs_offset);
        sections[section].id = WN_BLOB_SECTION_INPUTS;
        sections[section].offset = inputs_offset;
        sections[section].size = inputs_bytes;
        input->node = *input_node;
        input->max_length = 0;
    }
    *out_size = total;
    return data;
}

static void spec_init(NodeSpec *spec, int parent, int kind, float width, float height)
{
    memset(spec, 0, sizeof(*spec));
    spec->parent = parent;
    spec->kind = kind;
    spec->width = width;
    spec->height = height;
}

static wn_core *build_core(const NodeSpec *spec, uint32_t count,
                           const StateRecordSpec *states, uint32_t state_count,
                           const int *input_node, wn_document **out_doc)
{
    size_t size = 0;
    unsigned char *data = build_blob(spec, count, states, state_count, input_node,
                                     200u, 120u, &size);
    wn_document *document;
    wn_core *core;
    if (data == NULL) {
        return NULL;
    }
    document = wn_document_load_blob_memory(data, size);
    if (document == NULL) {
        free(data);
        return NULL;
    }
    /* The document borrows `data`; keep it alive until the core is destroyed. */
    lease_blob(data);
    core = wn_core_create(document);
    if (core == NULL) {
        wn_document_destroy(document);
        return NULL;
    }
    wn_core_resize(core, document->viewport_width, document->viewport_height);
    *out_doc = document;
    return core;
}

static StateSpec color_state(unsigned char state, unsigned int bg,
                             unsigned int text, unsigned int border)
{
    StateSpec style;
    memset(&style, 0, sizeof(style));
    style.state = state;
    style.present = (unsigned char)(R2_STATE_FIELD_BACKGROUND |
                                    R2_STATE_FIELD_TEXT |
                                    R2_STATE_FIELD_BORDER);
    style.background = bg;
    style.text = text;
    style.border_color = border;
    return style;
}

/* Root + one interactive button with every state color. */
static int make_button_page(NodeSpec nodes[2], StateRecordSpec states[1])
{
    spec_init(&nodes[0], -1, R2_NODE_BLOCK, 200.0f, 120.0f);
    nodes[0].background = 0xFF000000u;
    spec_init(&nodes[1], 0, R2_NODE_BLOCK, 80.0f, 30.0f);
    nodes[1].background = 0xFF101010u;
    nodes[1].text_color = 0xFFFFFFFFu;
    nodes[1].border_width = 2.0f;
    nodes[1].border_color = 0xFF202020u;
    nodes[1].action = ACTION_SELECT;
    /* data-* sugar: CSS state styles must win over these. */
    nodes[1].hover_bg = 0xFFABCDEFu;
    nodes[1].press_bg = 0xFF123456u;
    nodes[1].selected_bg = 0xFFFF00FFu;

    states[0].node = 1;
    states[0].count = 4;
    states[0].entries[0] = color_state(R2_STATE_STYLE_HOVER, 0xFF2A3140u,
                                       0xFFFFFF00u, 0xFFFF0000u);
    states[0].entries[1] = color_state(R2_STATE_STYLE_PRESS, 0xFF3A4A5Cu,
                                       0xFF00FFFFu, 0xFF00FF00u);
    states[0].entries[2] = color_state(R2_STATE_STYLE_FOCUS, 0xFF003366u,
                                       0xFF0000FFu, 0xFF0000FFu);
    states[0].entries[3] = color_state(R2_STATE_STYLE_DISABLED, 0xFF444444u,
                                       0xFF999999u, 0xFF555555u);
    return 1;
}

static void test_bits(wn_core *core)
{
    wn_core_set_state(core, 1, WN_STATE_FOCUSED, 1);
    check("focused bit accepts", (wn_core_node_state(core, 1) & WN_STATE_FOCUSED) != 0,
          "wn_core_set_state(FOCUSED)");
    wn_core_set_state(core, 1, WN_STATE_DISABLED, 1);
    check("disabled bit clears hover/press/focus",
          wn_core_node_state(core, 1) == WN_STATE_DISABLED,
          "only the disabled bit remains");
    wn_core_set_state(core, 1, WN_STATE_DISABLED, 0);
    check("disabled bit clears", wn_core_node_state(core, 1) == 0,
          "set_state(DISABLED, 0)");
}

static void test_priority(wn_core *core)
{
    check("base colors", wn_core_node_background(core, 1) == 0xFF101010u &&
                         wn_core_node_text_color(core, 1) == 0xFFFFFFFFu &&
                         wn_core_node_border_color(core, 1) == 0xFF202020u,
          "no state -> base bg/text/border");
    wn_core_set_state(core, 1, WN_STATE_FOCUSED, 1);
    check("focus priority", wn_core_node_background(core, 1) == 0xFF003366u &&
                            wn_core_node_text_color(core, 1) == 0xFF0000FFu,
          "focus overrides base");
    wn_core_set_state(core, 1, WN_STATE_HOVER, 1);
    check("hover over focus", wn_core_node_background(core, 1) == 0xFF2A3140u &&
                              wn_core_node_border_color(core, 1) == 0xFFFF0000u,
          "hover > focus; CSS hover beats data-hover");
    wn_core_set_state(core, 1, WN_STATE_PRESS, 1);
    check("press over hover", wn_core_node_background(core, 1) == 0xFF3A4A5Cu &&
                              wn_core_node_text_color(core, 1) == 0xFF00FFFFu,
          "press > hover; CSS press beats data-press");
    wn_core_set_state(core, 1, WN_STATE_DISABLED, 1);
    check("disabled over press", wn_core_node_background(core, 1) == 0xFF444444u &&
                                 wn_core_node_text_color(core, 1) == 0xFF999999u &&
                                 wn_core_node_border_color(core, 1) == 0xFF555555u,
          "disabled > press > hover > focus > base");
    wn_core_set_state(core, 1, WN_STATE_DISABLED, 0);
    wn_core_set_state(core, 1, WN_STATE_PRESS, 0);
    wn_core_set_state(core, 1, WN_STATE_HOVER, 0);
    wn_core_set_state(core, 1, WN_STATE_FOCUSED, 0);
}

static void test_selected(wn_core *core)
{
    wn_core_set_state(core, 1, WN_STATE_SELECTED, 1);
    check("selected is the app base",
          wn_core_node_background(core, 1) == 0xFFFF00FFu,
          "selected_bg with no state style active");
    wn_core_set_state(core, 1, WN_STATE_HOVER, 1);
    check("selected + hover", wn_core_node_background(core, 1) == 0xFF2A3140u,
          "the CSS hover state composes over the selected base");
    wn_core_set_state(core, 1, WN_STATE_HOVER, 0);
    wn_core_set_state(core, 1, WN_STATE_SELECTED, 0);
    check("selected cleared", wn_core_node_background(core, 1) == 0xFF101010u,
          "back to the base background");
}

static void test_disabled_pointer(wn_core *core)
{
    int hit;
    int before;
    ensure_layout_for_hit(core);
    hit = wn_core_hit_test(core, 40.0f, 15.0f);
    check("enabled button is interactive", hit == 1, "hit test returns the button");
    wn_core_set_state(core, 1, WN_STATE_DISABLED, 1);
    check("diagnostic hit still sees the node",
          wn_core_node_at(core, 40.0f, 15.0f) == 1, "wn_core_node_at ignores disabled");
    hit = wn_core_hit_test(core, 40.0f, 15.0f);
    check("disabled button is inert", hit == -1, "interactive hit test skips it");
    check("pointer_down does not press", wn_core_pointer_down(core, 40.0f, 15.0f) == -1 &&
                                        wn_core_press(core) == -1,
          "no press target");
    before = wn_core_node_state(core, 1);
    check("pointer_move does not hover",
          wn_core_pointer_move(core, 40.0f, 15.0f) == -1 &&
          wn_core_hover(core) == -1 &&
          wn_core_node_state(core, 1) == before,
          "no hover bit");
    wn_core_set_state(core, 1, WN_STATE_DISABLED, 0);
}

/* Focus-visible and focus-within: the derived WITHIN bit reaches every
 * ancestor and the visible bit only applies while focused. */
static void test_focus_visible_and_within(void)
{
    NodeSpec nodes[3];
    StateRecordSpec states[2];
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&nodes[0], -1, R2_NODE_BLOCK, 200.0f, 120.0f);
    nodes[0].background = 0xFF000000u;
    spec_init(&nodes[1], 0, R2_NODE_BLOCK, 120.0f, 40.0f);
    nodes[1].background = 0xFF101010u;
    spec_init(&nodes[2], 1, R2_NODE_BLOCK, 80.0f, 30.0f);
    nodes[2].background = 0xFF202020u;
    memset(states, 0, sizeof(states));
    states[0].node = 1;
    states[0].count = 1;
    memset(&states[0].entries[0], 0, sizeof(StateSpec));
    states[0].entries[0].state = R2_STATE_STYLE_FOCUS_WITHIN;
    states[0].entries[0].present = R2_STATE_FIELD_BACKGROUND;
    states[0].entries[0].background = 0xFF112233u;
    states[1].node = 2;
    states[1].count = 2;
    memset(&states[1].entries[0], 0, sizeof(StateSpec));
    states[1].entries[0].state = R2_STATE_STYLE_FOCUS;
    states[1].entries[0].present = R2_STATE_FIELD_BACKGROUND;
    states[1].entries[0].background = 0xFF003366u;
    memset(&states[1].entries[1], 0, sizeof(StateSpec));
    states[1].entries[1].state = R2_STATE_STYLE_FOCUS_VISIBLE;
    states[1].entries[1].present = R2_STATE_FIELD_BACKGROUND;
    states[1].entries[1].background = 0xFF00AA55u;

    core = build_core(nodes, 3, states, 2, NULL, &document);
    check("focus-visible page builds", core != NULL, NULL);
    if (core == NULL) {
        return;
    }
    check("focus-within idle", wn_core_node_background(core, 1) == 0xFF101010u &&
                               (wn_core_node_state(core, 1) &
                                WN_STATE_FOCUS_WITHIN) == 0,
          NULL);
    wn_core_set_state(core, 2, WN_STATE_FOCUSED, 1);
    check("focus marks the ancestors focus-within",
          (wn_core_node_state(core, 1) & WN_STATE_FOCUS_WITHIN) != 0 &&
              wn_core_node_background(core, 1) == 0xFF112233u,
          "container paints its focus-within style");
    check("plain focus paints the focus style",
          wn_core_node_background(core, 2) == 0xFF003366u, NULL);
    wn_core_set_state(core, 2, WN_STATE_FOCUS_VISIBLE, 1);
    check("focus-visible implies focus and wins",
          (wn_core_node_state(core, 2) &
           (WN_STATE_FOCUSED | WN_STATE_FOCUS_VISIBLE)) ==
              (WN_STATE_FOCUSED | WN_STATE_FOCUS_VISIBLE) &&
              wn_core_node_background(core, 2) == 0xFF00AA55u,
          "visible > focus");
    wn_core_set_state(core, 2, WN_STATE_FOCUSED, 0);
    check("clearing focus clears visible and focus-within",
          wn_core_node_state(core, 2) == 0 &&
              (wn_core_node_state(core, 1) & WN_STATE_FOCUS_WITHIN) == 0 &&
              wn_core_node_background(core, 1) == 0xFF101010u,
          "no stale derived state");
    wn_core_destroy(core);
    wn_document_destroy(document);
}

/* Focus matrix: FOCUS_VISIBLE implies FOCUSED, clearing one or the other is
 * consistent, multiple focused nodes coexist, and a remove/reinsert (append
 * moves the subtree) re-derives FOCUS_WITHIN without stale ancestors. */
static void test_focus_matrix(void)
{
    NodeSpec nodes[4];
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&nodes[0], -1, R2_NODE_BLOCK, 200.0f, 120.0f);
    spec_init(&nodes[1], 0, R2_NODE_BLOCK, 180.0f, 40.0f);
    spec_init(&nodes[2], 0, R2_NODE_BLOCK, 180.0f, 40.0f);
    spec_init(&nodes[3], 2, R2_NODE_BLOCK, 80.0f, 30.0f);
    core = build_core(nodes, 4, NULL, 0, NULL, &document);
    check("focus matrix page builds", core != NULL, NULL);
    if (core == NULL) {
        return;
    }

    wn_core_set_state(core, 1, WN_STATE_FOCUS_VISIBLE, 1);
    check("focus-visible alone implies focused",
          (wn_core_node_state(core, 1) &
           (WN_STATE_FOCUSED | WN_STATE_FOCUS_VISIBLE)) ==
              (WN_STATE_FOCUSED | WN_STATE_FOCUS_VISIBLE),
          "the host-visible focus bits are 0x28");
    check("the focused node also derives its own focus-within",
          (wn_core_node_state(core, 1) & WN_STATE_FOCUS_WITHIN) != 0, NULL);
    check("focus-visible marks the ancestors focus-within",
          (wn_core_node_state(core, 0) & WN_STATE_FOCUS_WITHIN) != 0, NULL);
    wn_core_set_state(core, 1, WN_STATE_FOCUS_VISIBLE, 0);
    check("clearing visible keeps focused",
          (wn_core_node_state(core, 1) &
           (WN_STATE_FOCUSED | WN_STATE_FOCUS_VISIBLE)) == WN_STATE_FOCUSED, NULL);
    wn_core_set_state(core, 1, WN_STATE_FOCUS_VISIBLE, 1);
    wn_core_set_state(core, 1, WN_STATE_FOCUSED, 0);
    check("clearing focused clears visible",
          wn_core_node_state(core, 1) == 0, "no stale 0x20");

    wn_core_set_state(core, 3, WN_STATE_FOCUSED, 1);
    check("a focused child marks every ancestor",
          (wn_core_node_state(core, 2) & WN_STATE_FOCUS_WITHIN) != 0 &&
              (wn_core_node_state(core, 0) & WN_STATE_FOCUS_WITHIN) != 0 &&
              (wn_core_node_state(core, 1) & WN_STATE_FOCUS_WITHIN) == 0,
          "only the chain to the root");
    wn_core_append(core, 1, 3);
    check("remove/reinsert re-derives focus-within",
          (wn_core_node_state(core, 1) & WN_STATE_FOCUS_WITHIN) != 0 &&
              (wn_core_node_state(core, 2) & WN_STATE_FOCUS_WITHIN) == 0 &&
              (wn_core_node_state(core, 0) & WN_STATE_FOCUS_WITHIN) != 0,
          "the old parent loses it, the new one gains it");
    wn_core_remove(core, 3);
    check("removing the focused node clears derived focus-within",
          (wn_core_node_state(core, 1) & WN_STATE_FOCUS_WITHIN) == 0 &&
              (wn_core_node_state(core, 0) & WN_STATE_FOCUS_WITHIN) == 0,
          "no stale derived bits");

    wn_core_set_state(core, 1, WN_STATE_FOCUSED, 1);
    wn_core_set_state(core, 2, WN_STATE_FOCUSED, 1);
    check("multiple focused nodes are independent",
          (wn_core_node_state(core, 1) & WN_STATE_FOCUSED) != 0 &&
              (wn_core_node_state(core, 2) & WN_STATE_FOCUSED) != 0 &&
              (wn_core_node_state(core, 0) & WN_STATE_FOCUS_WITHIN) != 0,
          NULL);
    wn_core_set_state(core, 1, WN_STATE_FOCUSED, 0);
    check("clearing one focus keeps the other's within",
          (wn_core_node_state(core, 2) & WN_STATE_FOCUSED) != 0 &&
              (wn_core_node_state(core, 0) & WN_STATE_FOCUS_WITHIN) != 0,
          NULL);
    wn_core_set_state(core, 2, WN_STATE_FOCUSED, 0);
    check("clearing the last focus clears within",
          (wn_core_node_state(core, 0) & WN_STATE_FOCUS_WITHIN) == 0, NULL);

    wn_core_set_state(core, 2, WN_STATE_DISABLED, 1);
    wn_core_set_state(core, 2, WN_STATE_FOCUS_VISIBLE, 1);
    check("disabled refuses the focus-visible bit",
          (wn_core_node_state(core, 2) &
           (WN_STATE_FOCUSED | WN_STATE_FOCUS_VISIBLE)) == 0,
          "interactive bits stay off while disabled");

    wn_core_destroy(core);
    wn_document_destroy(document);
}

/* Fixed color priority inside the focus family: focus-visible > focus >
 * focus-within. */
static void test_focus_color_priority(void)
{
    NodeSpec nodes[2];
    StateRecordSpec states[1];
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&nodes[0], -1, R2_NODE_BLOCK, 200.0f, 120.0f);
    spec_init(&nodes[1], 0, R2_NODE_BLOCK, 120.0f, 30.0f);
    nodes[1].background = 0xFF101010u;
    memset(states, 0, sizeof(states));
    states[0].node = 1;
    states[0].count = 3;
    states[0].entries[0] = color_state(R2_STATE_STYLE_FOCUS, 0xFF003366u,
                                       0xFF003366u, 0xFF003366u);
    states[0].entries[1] = color_state(R2_STATE_STYLE_FOCUS_VISIBLE, 0xFF00AA55u,
                                       0xFF00AA55u, 0xFF00AA55u);
    states[0].entries[2] = color_state(R2_STATE_STYLE_FOCUS_WITHIN, 0xFF112233u,
                                       0xFF112233u, 0xFF112233u);
    core = build_core(nodes, 2, states, 1, NULL, &document);
    check("focus priority page builds", core != NULL, NULL);
    if (core == NULL) {
        return;
    }
    wn_core_set_state(core, 1, WN_STATE_FOCUS_WITHIN, 1);
    check("focus-within paints when it is the only focus bit",
          wn_core_node_background(core, 1) == 0xFF112233u, NULL);
    wn_core_set_state(core, 1, WN_STATE_FOCUSED, 1);
    check("focus beats focus-within",
          wn_core_node_background(core, 1) == 0xFF003366u, NULL);
    wn_core_set_state(core, 1, WN_STATE_FOCUS_VISIBLE, 1);
    check("focus-visible beats focus",
          wn_core_node_background(core, 1) == 0xFF00AA55u, NULL);
    wn_core_set_state(core, 1, WN_STATE_FOCUS_VISIBLE, 0);
    check("clearing visible falls back to focus",
          wn_core_node_background(core, 1) == 0xFF003366u, NULL);
    wn_core_destroy(core);
    wn_document_destroy(document);
}

/* A focused node also carries the derived FOCUS_WITHIN bit: a :focus-within
 * font-weight on the node itself must request relayout even with no focused
 * ancestor. */
static void test_focus_within_own_layout(void)
{
    NodeSpec nodes[2];
    StateRecordSpec states[1];
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&nodes[0], -1, R2_NODE_BLOCK, 200.0f, 120.0f);
    spec_init(&nodes[1], 0, R2_NODE_BLOCK, 120.0f, 30.0f);
    nodes[1].font_weight = 400;
    memset(states, 0, sizeof(states));
    states[0].node = 1;
    states[0].count = 1;
    memset(&states[0].entries[0], 0, sizeof(StateSpec));
    states[0].entries[0].state = R2_STATE_STYLE_FOCUS_WITHIN;
    states[0].entries[0].present = R2_STATE_FIELD_FONT_WEIGHT;
    states[0].entries[0].font_weight = 500;

    core = build_core(nodes, 2, states, 1, NULL, &document);
    check("own focus-within weight page builds", core != NULL, NULL);
    if (core == NULL) {
        return;
    }
    wn_core_relayout(core);
    check("a clean layout has no pending request",
          wn_core_needs_layout(core) == 0, NULL);
    wn_core_set_state(core, 1, WN_STATE_FOCUSED, 1);
    check("focus applies the node's own focus-within weight",
          wn_core_node_font_weight(core, 1) == 500, "effective weight 500");
    check("the derived own focus-within weight requests relayout",
          wn_core_needs_layout(core) != 0,
          "own weight rule, not only a focused ancestor");
    wn_core_destroy(core);
    wn_document_destroy(document);
}

/* State font-weight travels in the reserved u16 and follows the same priority
 * as the colors. Text runs read their owner's state mask. */
static void test_state_font_weight(void)
{
    NodeSpec nodes[3];
    StateRecordSpec states[2];
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&nodes[0], -1, R2_NODE_BLOCK, 200.0f, 120.0f);
    spec_init(&nodes[1], 0, R2_NODE_BLOCK, 120.0f, 40.0f);
    nodes[1].font_weight = 400;
    spec_init(&nodes[2], 1, R2_NODE_TEXT, 80.0f, 16.0f);
    nodes[2].font_weight = 400;
    memset(states, 0, sizeof(states));
    states[0].node = 1;
    states[0].count = 3;
    memset(&states[0].entries[0], 0, sizeof(StateSpec));
    states[0].entries[0].state = R2_STATE_STYLE_HOVER;
    states[0].entries[0].present = R2_STATE_FIELD_FONT_WEIGHT;
    states[0].entries[0].font_weight = 700;
    memset(&states[0].entries[1], 0, sizeof(StateSpec));
    states[0].entries[1].state = R2_STATE_STYLE_FOCUS_VISIBLE;
    states[0].entries[1].present = R2_STATE_FIELD_FONT_WEIGHT;
    states[0].entries[1].font_weight = 600;
    memset(&states[0].entries[2], 0, sizeof(StateSpec));
    states[0].entries[2].state = R2_STATE_STYLE_FOCUS_WITHIN;
    states[0].entries[2].present = R2_STATE_FIELD_FONT_WEIGHT;
    states[0].entries[2].font_weight = 500;
    states[1].node = 2;
    states[1].count = 2;
    memset(&states[1].entries[0], 0, sizeof(StateSpec));
    states[1].entries[0].state = R2_STATE_STYLE_HOVER;
    states[1].entries[0].present = R2_STATE_FIELD_FONT_WEIGHT;
    states[1].entries[0].font_weight = 700;
    memset(&states[1].entries[1], 0, sizeof(StateSpec));
    states[1].entries[1].state = R2_STATE_STYLE_FOCUS_VISIBLE;
    states[1].entries[1].present = R2_STATE_FIELD_FONT_WEIGHT;
    states[1].entries[1].font_weight = 600;

    core = build_core(nodes, 3, states, 2, NULL, &document);
    check("state weight page builds", core != NULL, NULL);
    if (core == NULL) {
        return;
    }
    check("base weight", wn_core_node_font_weight(core, 1) == 400 &&
                             wn_core_node_font_weight(core, 2) == 400,
          NULL);
    wn_core_set_state(core, 1, WN_STATE_HOVER, 1);
    check("hover weight reaches the owner and its text run",
          wn_core_node_font_weight(core, 1) == 700 &&
              wn_core_node_font_weight(core, 2) == 700,
          "generated text follows the owner state");
    wn_core_set_state(core, 1, WN_STATE_HOVER, 0);
    wn_core_set_state(core, 1, WN_STATE_FOCUSED, 1);
    wn_core_set_state(core, 1, WN_STATE_FOCUS_VISIBLE, 1);
    check("focus-visible weight beats focus-within",
          wn_core_node_font_weight(core, 1) == 600,
          "visible > within with no focus-weight entry");
    wn_core_set_state(core, 1, WN_STATE_HOVER, 0);
    wn_core_set_state(core, 1, WN_STATE_FOCUSED, 0);
    wn_core_set_state(core, 1, WN_STATE_FOCUS_VISIBLE, 0);
    wn_core_set_state(core, 1, WN_STATE_FOCUS_WITHIN, 1);
    check("focus-within weight applies without focus",
          wn_core_node_font_weight(core, 1) == 500, NULL);
    wn_core_set_state(core, 1, WN_STATE_FOCUS_WITHIN, 0);
    check("weight returns to base", wn_core_node_font_weight(core, 1) == 400,
          NULL);
    wn_core_destroy(core);
    wn_document_destroy(document);
}

static void test_input_focus(void)
{
    NodeSpec nodes[2];
    StateRecordSpec states[1];
    int input_node = 1;
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&nodes[0], -1, R2_NODE_BLOCK, 200.0f, 120.0f);
    spec_init(&nodes[1], 0, R2_NODE_INPUT, 100.0f, 24.0f);
    nodes[1].background = 0xFF101010u;
    nodes[1].text_color = 0xFFFFFFFFu;
    memset(states, 0, sizeof(*states));
    states[0].node = 1;
    states[0].count = 1;
    states[0].entries[0] = color_state(R2_STATE_STYLE_FOCUS, 0xFF003366u,
                                       0xFF00FF00u, 0xFF0000FFu);
    core = build_core(nodes, 2, states, 1, &input_node, &document);
    check("input page builds", core != NULL, NULL);
    if (core == NULL) {
        return;
    }
    check("input focus sets the focused bit",
          wn_core_focus_input_at_end(core, 1) == 1 &&
          (wn_core_node_state(core, 1) &
           (WN_STATE_FOCUSED | WN_STATE_FOCUS_VISIBLE)) ==
              (WN_STATE_FOCUSED | WN_STATE_FOCUS_VISIBLE),
          "programmatic focus also marks :focus-visible");
    check("focus-within reaches the input ancestors",
          (wn_core_node_state(core, 0) & WN_STATE_FOCUS_WITHIN) != 0, NULL);
    check("pointer focus clears focus-visible",
          wn_core_focus_input(core, 1, 1.0f) == 1 &&
              (wn_core_node_state(core, 1) & WN_STATE_FOCUS_VISIBLE) == 0,
          "wn_core_focus_input is the pointer path");
    check("focus paint color", wn_core_node_background(core, 1) == 0xFF003366u,
          "focus state applies to the input");
    check("blur clears the focused bit",
          wn_core_blur_input(core) == 0 &&
          (wn_core_node_state(core, 1) &
           (WN_STATE_FOCUSED | WN_STATE_FOCUS_VISIBLE)) == 0 &&
          (wn_core_node_state(core, 0) & WN_STATE_FOCUS_WITHIN) == 0,
          "wn_core_blur_input");
    wn_core_set_state(core, 1, WN_STATE_DISABLED, 1);
    check("disabled input refuses programmatic focus",
          wn_core_focus_input_at_end(core, 1) == -1 &&
          wn_core_input_focused(core) == -1,
          "no focus on a disabled node");
    wn_core_destroy(core);
    wn_document_destroy(document);
}

static void test_data_sugar_fallback(void)
{
    NodeSpec nodes[2];
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&nodes[0], -1, R2_NODE_BLOCK, 200.0f, 120.0f);
    spec_init(&nodes[1], 0, R2_NODE_BLOCK, 80.0f, 30.0f);
    nodes[1].background = 0xFF101010u;
    nodes[1].action = ACTION_SELECT;
    nodes[1].hover_bg = 0xFF2A3140u;
    nodes[1].press_bg = 0xFF3A4A5Cu;
    nodes[1].selected_bg = 0xFF3498DBu;
    core = build_core(nodes, 2, NULL, 0, NULL, &document);
    check("data-only page builds", core != NULL, NULL);
    if (core == NULL) {
        return;
    }
    wn_core_set_state(core, 1, WN_STATE_HOVER, 1);
    check("data-hover fallback", wn_core_node_background(core, 1) == 0xFF2A3140u, NULL);
    wn_core_set_state(core, 1, WN_STATE_PRESS, 1);
    check("data-press fallback", wn_core_node_background(core, 1) == 0xFF3A4A5Cu, NULL);
    wn_core_set_state(core, 1, WN_STATE_PRESS, 0);
    wn_core_set_state(core, 1, WN_STATE_HOVER, 0);
    wn_core_set_state(core, 1, WN_STATE_SELECTED, 1);
    check("data-selected fallback",
          wn_core_node_background(core, 1) == 0xFF3498DBu, NULL);
    wn_core_destroy(core);
    wn_document_destroy(document);
}

static void test_text_run_follows_owner(void)
{
    NodeSpec nodes[3];
    StateRecordSpec states[2];
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&nodes[0], -1, R2_NODE_BLOCK, 200.0f, 120.0f);
    spec_init(&nodes[1], 0, R2_NODE_BLOCK, 80.0f, 30.0f);
    nodes[1].text_color = 0xFFFFFFFFu;
    spec_init(&nodes[2], 1, R2_NODE_TEXT, 80.0f, 16.0f);
    nodes[2].text_color = 0xFFFFFFFFu;
    memset(states, 0, sizeof(states));
    states[0].node = 1;
    states[0].count = 1;
    states[0].entries[0] = color_state(R2_STATE_STYLE_HOVER, 0xFF2A3140u,
                                       0xFFFFFF00u, 0xFFFF0000u);
    states[1].node = 2;
    states[1].count = 1;
    memset(&states[1].entries[0], 0, sizeof(StateSpec));
    states[1].entries[0].state = R2_STATE_STYLE_HOVER;
    states[1].entries[0].present = R2_STATE_FIELD_TEXT;
    states[1].entries[0].text = 0xFFFFFF00u;

    core = build_core(nodes, 3, states, 2, NULL, &document);
    check("text page builds", core != NULL, NULL);
    if (core == NULL) {
        return;
    }
    check("text run keeps its base color when idle",
          wn_core_node_text_color(core, 2) == 0xFFFFFFFFu, NULL);
    wn_core_set_state(core, 1, WN_STATE_HOVER, 1);
    check("text run follows the owner state",
          wn_core_node_text_color(core, 2) == 0xFFFFFF00u,
          "hover on the element recolors its generated text run");
    wn_core_set_state(core, 1, WN_STATE_HOVER, 0);
    check("text run returns to base",
          wn_core_node_text_color(core, 2) == 0xFFFFFFFFu, NULL);
    wn_core_destroy(core);
    wn_document_destroy(document);
}

/* A disabled container must make its whole subtree inert, not only itself:
 * hover/press/focus bits clear, a focused input blurs, and every edit entry
 * point rejects input even when a focus bit is forced back in (the guard is the
 * second line of defense). */
static void test_disabled_subtree(void)
{
    NodeSpec nodes[3];
    int input_node = 2;
    wn_document *document = NULL;
    wn_core *core;
    int value_changed;
    int visual_changed;

    spec_init(&nodes[0], -1, R2_NODE_BLOCK, 200.0f, 120.0f);
    spec_init(&nodes[1], 0, R2_NODE_BLOCK, 120.0f, 30.0f);
    spec_init(&nodes[2], 1, R2_NODE_INPUT, 100.0f, 24.0f);
    core = build_core(nodes, 3, NULL, 0, &input_node, &document);
    check("disabled subtree page builds", core != NULL, NULL);
    if (core == NULL) {
        return;
    }

    check("input focus seeds FOCUSED",
          wn_core_focus_input_at_end(core, 2) == 2 &&
              (wn_core_node_state(core, 2) & WN_STATE_FOCUSED) != 0,
          "wn_core_focus_input_at_end");
    value_changed = 0;
    visual_changed = 0;
    check("enabled input accepts a char",
          wn_core_input_char(core, (uint32_t)'a', &value_changed,
                             &visual_changed) == 2 &&
              value_changed != 0 &&
              strcmp(wn_core_input_value(core, 2), "a") == 0,
          "value=\"a\"");
    wn_core_set_state(core, 2, WN_STATE_HOVER, 1);
    check("input hover before disabling",
          (wn_core_node_state(core, 2) & WN_STATE_HOVER) != 0, NULL);

    wn_core_set_state(core, 1, WN_STATE_DISABLED, 1);
    check("disabled container keeps only its bit",
          wn_core_node_state(core, 1) == WN_STATE_DISABLED,
          "hover/press/focused cleared on the node too");
    check("disabled ancestor clears subtree interaction bits",
          (wn_core_node_state(core, 2) &
           (WN_STATE_HOVER | WN_STATE_PRESS | WN_STATE_FOCUSED)) == 0,
          "descendant hover and focus are gone");
    check("disabled ancestor blurs the descendant input",
          wn_core_input_focused(core) == -1 &&
              wn_core_node_disabled(core, 2) == 1,
          "input_focus is -1 and the node inherits disabled");

    value_changed = 0;
    visual_changed = 0;
    check("typing after disabling is rejected",
          wn_core_input_char(core, (uint32_t)'b', &value_changed,
                             &visual_changed) == -1 &&
              value_changed == 0 &&
              strcmp(wn_core_input_value(core, 2), "a") == 0,
          "no focus target, value unchanged");

    /* Simulate a leaked focus/bit (e.g. a host writing internal state) to
     * prove the edit entry points check the disabled ancestor themselves. */
    core->input_focus = 2;
    core->state[2] |= (unsigned char)WN_STATE_FOCUSED;
    value_changed = 0;
    visual_changed = 0;
    check("leaked focus still rejects char",
          wn_core_input_char(core, (uint32_t)'c', &value_changed,
                             &visual_changed) == -1 &&
              value_changed == 0 &&
              strcmp(wn_core_input_value(core, 2), "a") == 0,
          "disabled ancestor guard");
    value_changed = 0;
    visual_changed = 0;
    check("leaked focus still rejects keys",
          wn_core_input_key(core, WN_KEY_BACKSPACE, 0u, &value_changed,
                            &visual_changed) == -1 &&
              value_changed == 0 &&
              strcmp(wn_core_input_value(core, 2), "a") == 0,
          "backspace does not delete");
    value_changed = 0;
    visual_changed = 0;
    check("leaked focus still rejects paste",
          wn_core_input_paste(core, "zz", &value_changed,
                              &visual_changed) == -1 &&
              value_changed == 0 &&
              strcmp(wn_core_input_value(core, 2), "a") == 0,
          "paste is ignored");

    core->state[2] = WN_STATE_CLEAR(core->state[2], WN_STATE_FOCUSED);
    core->input_focus = -1;
    wn_core_set_state(core, 2, WN_STATE_HOVER, 1);
    wn_core_set_state(core, 2, WN_STATE_FOCUSED, 1);
    check("hover/focused rejected on a disabled subtree",
          (wn_core_node_state(core, 2) &
           (WN_STATE_HOVER | WN_STATE_FOCUSED)) == 0 &&
              wn_core_hover(core) == -1,
          "set_state(HOVER/FOCUSED, 1) is a no-op while disabled");

    wn_core_set_state(core, 1, WN_STATE_DISABLED, 0);
    check("re-enabled subtree stays clean",
          wn_core_node_disabled(core, 2) == 0 &&
              wn_core_input_focused(core) == -1 &&
              (wn_core_node_state(core, 2) &
               (WN_STATE_HOVER | WN_STATE_FOCUSED)) == 0,
          "no state is resurrected");

    wn_core_destroy(core);
    wn_document_destroy(document);
}

static void test_initial_disabled(void)
{
    NodeSpec nodes[2];
    StateRecordSpec states[1];
    wn_document *document = NULL;
    wn_core *core;

    spec_init(&nodes[0], -1, R2_NODE_BLOCK, 200.0f, 120.0f);
    spec_init(&nodes[1], 0, R2_NODE_BLOCK, 80.0f, 30.0f);
    nodes[1].background = 0xFF101010u;
    nodes[1].action = ACTION_SELECT;
    memset(states, 0, sizeof(states));
    states[0].node = 1;
    states[0].count = 1;
    states[0].initial_state = WN_STATE_DISABLED;
    states[0].entries[0] = color_state(R2_STATE_STYLE_DISABLED, 0xFF444444u,
                                       0xFF999999u, 0xFF555555u);
    core = build_core(nodes, 2, states, 1, NULL, &document);
    check("initial disabled page builds", core != NULL, NULL);
    if (core == NULL) {
        return;
    }
    check("disabled attribute seeds the state bit",
          wn_core_node_state(core, 1) == WN_STATE_DISABLED,
          "initial_state from the states record");
    check("seeded disabled paints the disabled style",
          wn_core_node_background(core, 1) == 0xFF444444u, NULL);
    check("seeded disabled is inert to the pointer",
          wn_core_hit_test(core, 40.0f, 15.0f) == -1, NULL);
    wn_core_destroy(core);
    wn_document_destroy(document);
}

static int load_states_blob(StateRecordSpec *states, uint32_t state_count)
{
    NodeSpec nodes[2];
    size_t size = 0;
    unsigned char *data;
    wn_document *document;
    StateRecordSpec copy[1];
    spec_init(&nodes[0], -1, R2_NODE_BLOCK, 200.0f, 120.0f);
    spec_init(&nodes[1], 0, R2_NODE_BLOCK, 80.0f, 30.0f);
    memcpy(copy, states, sizeof(copy));
    data = build_blob(nodes, 2, copy, state_count, NULL, 200u, 120u, &size);
    if (data == NULL) {
        return -1;
    }
    document = wn_document_load_blob_memory(data, size);
    free(data);
    if (document == NULL) {
        return 0;
    }
    wn_document_destroy(document);
    return 1;
}

static void test_loader_validation(void)
{
    StateRecordSpec states[1];
    memset(states, 0, sizeof(states));
    states[0].node = 1;
    states[0].count = 1;
    states[0].entries[0] = color_state(R2_STATE_STYLE_HOVER, 0xFF112233u,
                                       0, 0);
    states[0].entries[0].present = R2_STATE_FIELD_BACKGROUND;
    check("valid states record loads", load_states_blob(states, 1) == 1, NULL);

    states[0].node = 7;
    check("state node index is validated", load_states_blob(states, 1) == 0,
          "node 7 is out of range");
    states[0].node = 1;

    states[0].entries[0].state = 9;
    check("state code is validated", load_states_blob(states, 1) == 0,
          "state 9 is not a color state");
    states[0].entries[0].state = R2_STATE_STYLE_HOVER;

    states[0].entries[0].present = 0;
    check("present mask is validated", load_states_blob(states, 1) == 0,
          "present 0 has no fields");
    states[0].entries[0].present = R2_STATE_FIELD_BACKGROUND;

    states[0].entries[0].background = 0;
    check("present color is validated", load_states_blob(states, 1) == 0,
          "a present color cannot be the transparent sentinel");
    states[0].entries[0].background = 0xFF112233u;

    states[0].count = 2;
    states[0].entries[1] = states[0].entries[0];
    check("duplicate state is validated", load_states_blob(states, 1) == 0,
          "the same state appears twice");
    states[0].count = 1;

    /* Additive v6 weight: the present bit and the reserved u16 are validated
     * together, and the record size does not change. */
    memset(&states[0].entries[0], 0, sizeof(StateSpec));
    states[0].entries[0].state = R2_STATE_STYLE_HOVER;
    states[0].entries[0].present = R2_STATE_FIELD_FONT_WEIGHT;
    states[0].entries[0].font_weight = 700;
    check("valid state weight loads", load_states_blob(states, 1) == 1, NULL);
    states[0].entries[0].font_weight = 50;
    check("state weight range is validated", load_states_blob(states, 1) == 0,
          "50 is not a CSS weight");
    states[0].entries[0].font_weight = 700;
    states[0].entries[0].present = R2_STATE_FIELD_BACKGROUND;
    states[0].entries[0].background = 0xFF112233u;
    check("weight without the present bit is rejected",
          load_states_blob(states, 1) == 0,
          "reserved must stay zero when the field is absent");
}

int main(void)
{
    NodeSpec nodes[2];
    StateRecordSpec states[1];
    wn_document *document = NULL;
    wn_core *core;

    memset(states, 0, sizeof(states));
    make_button_page(nodes, states);
    core = build_core(nodes, 2, states, 1, NULL, &document);
    check("button page builds", core != NULL, NULL);
    if (core == NULL) {
        return 1;
    }
    test_bits(core);
    test_priority(core);
    test_selected(core);
    test_disabled_pointer(core);
    test_disabled_subtree();
    test_data_sugar_fallback();
    test_text_run_follows_owner();
    test_focus_visible_and_within();
    test_focus_matrix();
    test_focus_color_priority();
    test_focus_within_own_layout();
    test_state_font_weight();
    test_initial_disabled();
    test_input_focus();
    test_loader_validation();

    wn_core_destroy(core);
    wn_document_destroy(document);
    release_leases();
    printf("states_test: %s (%d checks)\n", failures == 0 ? "PASS" : "FAIL",
           checks);
    return failures == 0 ? 0 : 1;
}
