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
    int action;
    unsigned int hover_bg;
    unsigned int press_bg;
    unsigned int selected_bg;
} NodeSpec;

typedef struct {
    unsigned char state;
    unsigned char present;
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
    free(data);
    if (document == NULL) {
        return NULL;
    }
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
          (wn_core_node_state(core, 1) & WN_STATE_FOCUSED) != 0,
          "wn_core_focus_input_at_end");
    check("focus paint color", wn_core_node_background(core, 1) == 0xFF003366u,
          "focus state applies to the input");
    check("blur clears the focused bit",
          wn_core_blur_input(core) == 0 &&
          (wn_core_node_state(core, 1) & WN_STATE_FOCUSED) == 0,
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
    test_data_sugar_fallback();
    test_text_run_follows_owner();
    test_initial_disabled();
    test_input_focus();
    test_loader_validation();

    wn_core_destroy(core);
    wn_document_destroy(document);
    printf("states_test: %s (%d checks)\n", failures == 0 ? "PASS" : "FAIL",
           checks);
    return failures == 0 ? 0 : 1;
}
