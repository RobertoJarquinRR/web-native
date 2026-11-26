/*
 * wn_core.c - document tables, classes and the mutable node arena.
 *
 * Layout lives in core/layout.c, display-list generation in core/paint.c, the
 * text editor in core/input.c and the pointer state machine in core/hit_test.c.
 * The core owns every heap block; the other modules only read and write it.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin_blob.h"
#include "document/wn_blob.h"
#include "wn_core_internal.h"
#include "wn_internal.h"

/* The builtin page is an embedded .wnui blob parsed once by the same loader
 * the file path uses. Its strings point into kBuiltinBlob, which lives for the
 * whole process; the document itself is cached for the same lifetime. A failed
 * parse is cached too, including its diagnostic: the thread-local error is
 * cleared at the start of every public call, so the message is re-published on
 * each report instead of relying on the previous call leaving it behind. */
static wn_document *g_builtin_document;
static int g_builtin_failed;
static char g_builtin_error[WN_ERROR_CAP];

/* Re-publish the cached loader diagnostic for the failed builtin parse. */
static void builtin_report_failure(void)
{
    if (g_builtin_error[0] != '\0') {
        wn_set_error_raw(g_builtin_error);
    } else {
        wn_set_error("wn_document_builtin: the embedded blob could not be parsed");
    }
}

const wn_document *wn_document_builtin(void)
{
    if (g_builtin_failed) {
        builtin_report_failure();
        return NULL;
    }
    if (g_builtin_document == NULL) {
        wn_document *document = wn_document_load_blob_memory(
            kBuiltinBlob, (size_t)kBuiltinBlobSize);
        if (document == NULL) {
            /* wn_last_error() names the failing header/section; keep a copy so
             * later reports survive wn_clear_error(). */
            const char *message = wn_last_error();
            g_builtin_failed = 1;
            if (message != NULL) {
                size_t length = strlen(message);
                if (length >= sizeof(g_builtin_error)) {
                    length = sizeof(g_builtin_error) - 1;
                }
                memcpy(g_builtin_error, message, length);
                g_builtin_error[length] = '\0';
            }
            builtin_report_failure();
            return NULL;
        }
        document->refs = WN_DOCUMENT_STATIC_REFS;
        g_builtin_document = document;
    }
    return g_builtin_document;
}

int wn_document_is_builtin(const wn_document *document)
{
    /* Compares the cached pointer only: never parses, so callers like
     * retain/release cannot allocate the builtin as a side effect. */
    return document != NULL && document == g_builtin_document;
}

int wn_document_builtin_parsed(void)
{
    return g_builtin_document != NULL;
}

int wn_document_node_count(const wn_document *document)
{
    return document != NULL ? document->node_count : 0;
}

/* --------------------------------------------------------------- document */

static const UiNode *node_at(const wn_core *c, int node)
{
    if (node < 0 || node >= c->node_count || !c->alive[node]) {
        return NULL;
    }
    return &c->nodes[node];
}

static UiNode *node_mut(wn_core *c, int node)
{
    if (node < 0 || node >= c->node_count || !c->alive[node]) {
        return NULL;
    }
    return &c->nodes[node];
}

int wn_core_node_count(const wn_core *core)
{
    return core != NULL ? core->node_count : 0;
}

int wn_core_live_node_count(const wn_core *core)
{
    return core != NULL ? core->live_count : 0;
}

const char *wn_core_node_name(const wn_core *core, int node)
{
    if (core == NULL || node < 0 || node >= core->node_count ||
        !core->alive[node]) {
        return NULL;
    }
    return core->names[node];
}

const char *wn_core_node_text(const wn_core *core, int node)
{
    if (core == NULL || node_at(core, node) == NULL) {
        return NULL;
    }
    if (core->text_override[node] != NULL) {
        return core->text_override[node];
    }
    return core->nodes[node].text;
}

const char *wn_core_node_image_src(const wn_core *core, int node)
{
    if (core == NULL || node_at(core, node) == NULL) {
        return NULL;
    }
    if (core->image_override[node] != NULL) {
        return core->image_override[node];
    }
    return core->nodes[node].image_src;
}

int wn_core_node_is_text(const wn_core *core, int node)
{
    const UiNode *n = core != NULL ? node_at(core, node) : NULL;
    return n != NULL && n->kind == R2_NODE_TEXT;
}

int wn_core_node_is_alive(const wn_core *core, int node)
{
    return core != NULL && node >= 0 && node < core->node_count &&
           core->alive[node] != 0;
}

int wn_core_node_is_visible(const wn_core *core, int node)
{
    if (!wn_core_node_is_alive(core, node)) {
        return 0;
    }
    return core->visible[node] != 0;
}

int wn_core_node_is_dynamic(const wn_core *core, int node)
{
    if (!wn_core_node_is_alive(core, node)) {
        return 0;
    }
    return core->dynamic_node[node] != 0;
}

uint32_t wn_core_node_generation(const wn_core *core, int node)
{
    if (!wn_core_node_is_alive(core, node)) {
        return 0;
    }
    return core->generation[node];
}

int wn_core_node_parent(const wn_core *core, int node)
{
    if (!wn_core_node_is_alive(core, node)) {
        return -1;
    }
    return core->parent[node];
}

int wn_core_node_first_child(const wn_core *core, int node)
{
    if (!wn_core_node_is_alive(core, node)) {
        return -1;
    }
    return core->first_child[node];
}

int wn_core_node_next_sibling(const wn_core *core, int node)
{
    if (!wn_core_node_is_alive(core, node)) {
        return -1;
    }
    return core->next_sibling[node];
}

int wn_core_node_action(const wn_core *core, int node)
{
    const UiNode *n = core != NULL ? node_at(core, node) : NULL;
    return n != NULL ? n->action : 0;
}

static int name_matches_id(const char *name, const char *id)
{
    const char *base;
    if (name == NULL || id == NULL) {
        return 0;
    }
    if (strcmp(name, id) == 0) {
        return 1;
    }
    if (id[0] == '#') {
        ++id;
    }
    base = strrchr(name, '/');
    base = base != NULL ? base + 1 : name;
    return strcmp(base, id) == 0;
}

int wn_core_find_node(const wn_core *core, const char *id)
{
    int i;
    int match = -1;
    int matches = 0;
    if (core == NULL || id == NULL || id[0] == '\0') {
        return -1;
    }
    for (i = 0; i < core->node_count; ++i) {
        if (core->alive[i] && strcmp(core->names[i], id) == 0) {
            return i;
        }
    }
    for (i = 0; i < core->node_count; ++i) {
        if (core->alive[i] && name_matches_id(core->names[i], id)) {
            match = i;
            ++matches;
        }
    }
    return matches == 1 ? match : -1;
}

int wn_core_hover(const wn_core *core)
{
    return core != NULL ? core->hover : -1;
}

int wn_core_press(const wn_core *core)
{
    return core != NULL ? core->press : -1;
}

int wn_core_selected(const wn_core *core)
{
    int i;
    if (core == NULL || core->state == NULL) {
        return -1;
    }
    for (i = 0; i < core->node_count; ++i) {
        if (core->alive[i] && (core->state[i] & WN_STATE_SELECTED) != 0u) {
            return i;
        }
    }
    return -1;
}

int wn_core_node_state(const wn_core *core, int node)
{
    if (!wn_core_node_is_alive(core, node) || core->state == NULL) {
        return 0;
    }
    return core->state[node];
}

/* A disabled node makes its whole subtree inert, like a disabled form control.
 * wn_core_node_at() (diagnostic hit test) still reports the node; the
 * interactive hit test and the focus path call this helper first. */
int wn_core_node_disabled(const wn_core *core, int node)
{
    if (core == NULL || core->state == NULL || node < 0 ||
        node >= core->node_count || !core->alive[node]) {
        return 0;
    }
    while (node >= 0) {
        if ((core->state[node] & WN_STATE_DISABLED) != 0u) {
            return 1;
        }
        node = core->parent[node];
    }
    return 0;
}

/* Test/debug hook: OR a mask without the single-hover/press rule. The
 * offscreen fixture driver uses it to render several state combinations in one
 * frame; the public ABI keeps the exclusive pointer semantics. */
void wn_core_force_state_mask(wn_core *core, int node, unsigned int mask)
{
    if (core == NULL || core->state == NULL || node < 0 ||
        node >= core->node_count || !core->alive[node]) {
        return;
    }
    core->state[node] |= (unsigned char)(mask & 0x1Fu);
    core->dirty = 1;
}

void wn_core_set_state(wn_core *core, int node, int state, int enabled)
{
    if (core == NULL || core->state == NULL || node < 0 ||
        node >= core->node_count || !core->alive[node]) {
        return;
    }
    if (state == WN_STATE_HOVER) {
        if (enabled) {
            if (core->hover >= 0 && core->hover != node) {
                core->state[core->hover] = WN_STATE_CLEAR(core->state[core->hover], WN_STATE_HOVER);
            }
            core->state[node] |= (unsigned char)WN_STATE_HOVER;
            core->hover = node;
        } else {
            core->state[node] = WN_STATE_CLEAR(core->state[node], WN_STATE_HOVER);
            if (core->hover == node) {
                core->hover = -1;
            }
        }
    } else if (state == WN_STATE_PRESS) {
        if (enabled) {
            if (core->press >= 0 && core->press != node) {
                core->state[core->press] = WN_STATE_CLEAR(core->state[core->press], WN_STATE_PRESS);
            }
            core->state[node] |= (unsigned char)WN_STATE_PRESS;
            core->press = node;
        } else {
            core->state[node] = WN_STATE_CLEAR(core->state[node], WN_STATE_PRESS);
            if (core->press == node) {
                core->press = -1;
            }
        }
    } else if (state == WN_STATE_SELECTED) {
        if (enabled) {
            core->state[node] |= (unsigned char)WN_STATE_SELECTED;
        } else {
            core->state[node] = WN_STATE_CLEAR(core->state[node], WN_STATE_SELECTED);
        }
    } else if (state == WN_STATE_FOCUSED) {
        /* Per-node, like selected: the host may mark any node focused. Input
         * focus is the only engine-driven producer. */
        if (enabled) {
            core->state[node] |= (unsigned char)WN_STATE_FOCUSED;
        } else {
            core->state[node] = WN_STATE_CLEAR(core->state[node], WN_STATE_FOCUSED);
        }
    } else if (state == WN_STATE_DISABLED) {
        if (enabled) {
            /* A node that becomes disabled cannot stay hovered/pressed/focused.
             * The pointer machine also refuses it; clearing here keeps the
             * reported mask consistent for hosts that drive states directly. */
            core->state[node] = WN_STATE_CLEAR(core->state[node], WN_STATE_HOVER);
            core->state[node] = WN_STATE_CLEAR(core->state[node], WN_STATE_PRESS);
            core->state[node] = WN_STATE_CLEAR(core->state[node], WN_STATE_FOCUSED);
            if (core->hover == node) {
                core->hover = -1;
            }
            if (core->press == node) {
                core->press = -1;
            }
            if (core->input_focus == node) {
                core->input_focus = -1;
            }
            core->state[node] |= (unsigned char)WN_STATE_DISABLED;
        } else {
            core->state[node] = WN_STATE_CLEAR(core->state[node], WN_STATE_DISABLED);
        }
    } else {
        return;
    }
    core->dirty = 1;
}

/* ---------------------------------------------------------------- classes */

int wn_core_set_class(wn_core *core, int node, const char *class_name, int enabled)
{
    int i;
    int slot = -1;
    int count;
    if (core == NULL || class_name == NULL || class_name[0] == '\0' ||
        node < 0 || node >= core->node_count || !core->alive[node]) {
        return -1;
    }
    if (strcmp(class_name, "hover") == 0) {
        wn_core_set_state(core, node, WN_STATE_HOVER, enabled);
        return 0;
    }
    if (strcmp(class_name, "press") == 0) {
        wn_core_set_state(core, node, WN_STATE_PRESS, enabled);
        return 0;
    }
    if (strcmp(class_name, "selected") == 0) {
        wn_core_set_state(core, node, WN_STATE_SELECTED, enabled);
        return 0;
    }
    if (strcmp(class_name, "focus") == 0) {
        wn_core_set_state(core, node, WN_STATE_FOCUSED, enabled);
        return 0;
    }
    if (strcmp(class_name, "disabled") == 0) {
        wn_core_set_state(core, node, WN_STATE_DISABLED, enabled);
        return 0;
    }

    count = core->class_count[node];
    for (i = 0; i < count; ++i) {
        if (strcmp(core->classes[node * WN_MAX_CLASSES + i], class_name) == 0) {
            slot = i;
            break;
        }
    }
    if (enabled) {
        if (slot >= 0) {
            return 0;
        }
        if (count >= WN_MAX_CLASSES) {
            return 1;
        }
        core->classes[node * WN_MAX_CLASSES + count] = wn_strdup_(class_name);
        if (core->classes[node * WN_MAX_CLASSES + count] == NULL) {
            return 1;
        }
        core->class_count[node] = (unsigned char)(count + 1);
    } else if (slot >= 0) {
        wn_free_(core->classes[node * WN_MAX_CLASSES + slot]);
        for (i = slot; i + 1 < count; ++i) {
            core->classes[node * WN_MAX_CLASSES + i] =
                core->classes[node * WN_MAX_CLASSES + i + 1];
        }
        core->classes[node * WN_MAX_CLASSES + count - 1] = NULL;
        core->class_count[node] = (unsigned char)(count - 1);
    }
    core->dirty = 1;
    return 0;
}

/* ------------------------------------------------------------- text cache */

static int text_supported(const wn_core *c, int node, const char *text)
{
    (void)c;
    (void)node;
    if (text == NULL) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (*p < 0x20 && *p != '\t') {
            return 0;
        }
    }
    return 1;
}

int wn_core_text_override_supported(const wn_core *core, int node, const char *text)
{
    if (core == NULL || !wn_core_node_is_alive(core, node) ||
        core->nodes[node].kind != R2_NODE_TEXT) {
        return -1;
    }
    if (text == NULL) {
        return -1;
    }
    return text_supported(core, node, text) ? 0 : 1;
}

int wn_core_set_text(wn_core *core, int node, const char *text)
{
    char *copy;
    if (core == NULL || !wn_core_node_is_alive(core, node) ||
        core->nodes[node].kind != R2_NODE_TEXT || text == NULL) {
        return -1;
    }
    if (!text_supported(core, node, text)) {
        return 1;
    }
    copy = wn_strdup_(text);
    if (copy == NULL) {
        return -1;
    }
    wn_free_(core->text_override[node]);
    core->text_override[node] = copy;
    core->dirty = 1;
    core->needs_layout = 1;
    return 0;
}

const wn_display_list *wn_core_display_list(const wn_core *core)
{
    return core != NULL ? &core->list : NULL;
}

const wn_rect *wn_core_rects(const wn_core *core, int *count)
{
    if (core == NULL) {
        if (count != NULL) {
            *count = 0;
        }
        return NULL;
    }
    if (count != NULL) {
        *count = core->node_count;
    }
    return core->rects;
}

double wn_core_last_layout_ms(const wn_core *core)
{
    return core != NULL ? core->last_layout_ms : 0.0;
}

/* -------------------------------------------------------------- lifecycle */

wn_core *wn_core_create(const wn_document *document)
{
    wn_core *c;
    int count;
    int capacity;
    int i;
    size_t bytes;
    if (document == NULL) {
        if (g_builtin_failed) {
            /* Report why the embedded builtin is missing rather than whatever
             * unrelated error happened to be pending. */
            builtin_report_failure();
            return NULL;
        }
        wn_set_error("wn_core_create: document is NULL");
        return NULL;
    }
    if (document->node_count <= 0) {
        wn_set_error("wn_core_create: document is empty");
        return NULL;
    }
    c = (wn_core *)calloc(1, sizeof(*c));
    if (c == NULL) {
        wn_set_error("wn_core_create: out of memory for the core");
        return NULL;
    }
    c->doc = document;
    wn_document_retain((wn_document *)document);
    count = document->node_count;
    capacity = count > WN_NODE_MIN_CAPACITY ? count : WN_NODE_MIN_CAPACITY;
    c->node_capacity = capacity;
    c->node_count = count;
    c->live_count = count;
    c->hover = c->press = -1;
    c->width = document->viewport_width;
    c->height = document->viewport_height;

    bytes = (size_t)capacity;
    c->nodes = (UiNode *)calloc(bytes, sizeof(UiNode));
    c->names = (const char **)calloc(bytes, sizeof(char *));
    c->generation = (uint32_t *)calloc(bytes, sizeof(uint32_t));
    c->alive = (unsigned char *)calloc(bytes, sizeof(unsigned char));
    c->dynamic_node = (unsigned char *)calloc(bytes, sizeof(unsigned char));
    c->visible = (unsigned char *)calloc(bytes, sizeof(unsigned char));
    c->visible_eff = (unsigned char *)calloc(bytes, sizeof(unsigned char));
    c->state = (unsigned char *)calloc(bytes, sizeof(unsigned char));
    c->parent = (int *)calloc(bytes, sizeof(int));
    c->order_key = (int *)calloc(bytes, sizeof(int));
    c->free_list = (int *)calloc(bytes, sizeof(int));
    c->rects = (wn_rect *)calloc(bytes, sizeof(wn_rect));
    c->clips = (wn_rect *)calloc(bytes, sizeof(wn_rect));
    c->scroll_y = (float *)calloc(bytes, sizeof(float));
    c->scroll_max_y = (float *)calloc(bytes, sizeof(float));
    c->first_child = (int *)calloc(bytes, sizeof(int));
    c->next_sibling = (int *)calloc(bytes, sizeof(int));
    c->prev_sibling = (int *)calloc(bytes, sizeof(int));
    c->last_child = (int *)calloc(bytes, sizeof(int));
    c->paint_items = (WnPaintItem *)calloc(bytes, sizeof(WnPaintItem));
    c->paint_item_capacity = capacity;
    c->text_override = (char **)calloc(bytes, sizeof(char *));
    c->image_override = (char **)calloc(bytes, sizeof(char *));
    c->input_map = (int *)calloc(bytes, sizeof(int));
    c->input_value = (char **)calloc(bytes, sizeof(char *));
    c->input_cursor = (int *)calloc(bytes, sizeof(int));
    c->input_anchor = (int *)calloc(bytes, sizeof(int));
    c->input_scroll = (float *)calloc(bytes, sizeof(float));
    c->class_count = (unsigned char *)calloc(bytes, sizeof(unsigned char));
    c->classes = (char **)calloc(bytes * WN_MAX_CLASSES, sizeof(char *));
    /* Reserved horizontal pair: allocated after the hot layout arrays so the
     * vertical pair and the rest of the arena keep their historical layout. */
    c->scroll_x = (float *)calloc(bytes, sizeof(float));
    c->scroll_max_x = (float *)calloc(bytes, sizeof(float));
    c->vertex_capacity = count * 6;
    c->vertices = (wn_vertex *)calloc((size_t)c->vertex_capacity, sizeof(wn_vertex));
    c->solid_batch_capacity = count > 0 ? count : 1;
    c->solid_batches = (wn_draw_batch *)calloc(
        (size_t)c->solid_batch_capacity, sizeof(wn_draw_batch));
    c->glyph_vertex_capacity = count * 6;
    c->glyph_vertices = (wn_glyph_vertex *)calloc(
        (size_t)c->glyph_vertex_capacity, sizeof(wn_glyph_vertex));
    c->glyph_batch_capacity = count > 0 ? count : 1;
    c->glyph_batches = (wn_draw_batch *)calloc(
        (size_t)c->glyph_batch_capacity, sizeof(wn_draw_batch));
    c->image_vertex_capacity = count * 6;
    c->image_vertices = (wn_image_vertex *)calloc(
        (size_t)c->image_vertex_capacity, sizeof(wn_image_vertex));
    c->image_batch_capacity = count > 0 ? count : 1;
    c->image_batches = (wn_image_batch *)calloc(
        (size_t)c->image_batch_capacity, sizeof(wn_image_batch));

    if (c->nodes == NULL || c->names == NULL || c->generation == NULL ||
        c->alive == NULL ||
        c->dynamic_node == NULL || c->visible == NULL || c->visible_eff == NULL ||
        c->state == NULL ||
        c->parent == NULL || c->order_key == NULL ||
        c->free_list == NULL || c->rects == NULL || c->clips == NULL ||
        c->scroll_x == NULL || c->scroll_y == NULL ||
        c->scroll_max_x == NULL || c->scroll_max_y == NULL ||
        c->first_child == NULL ||
        c->next_sibling == NULL || c->prev_sibling == NULL ||
        c->last_child == NULL || c->paint_items == NULL ||
        c->text_override == NULL || c->image_override == NULL ||
        c->input_map == NULL || c->input_value == NULL || c->input_cursor == NULL ||
        c->input_anchor == NULL || c->input_scroll == NULL ||
        c->class_count == NULL || c->classes == NULL ||
    c->vertices == NULL || c->solid_batches == NULL ||
        c->glyph_vertices == NULL || c->glyph_batches == NULL ||
        c->image_vertices == NULL || c->image_batches == NULL) {
        wn_set_error("wn_core_create: out of memory for the node arena");
        wn_core_destroy(c);
        return NULL;
    }

    memcpy(c->nodes, document->nodes, (size_t)count * sizeof(UiNode));
    for (i = 0; i < count; ++i) {
        c->names[i] = document->names[i];
        c->generation[i] = 1;
        c->alive[i] = 1;
        c->visible[i] = 1;
        c->visible_eff[i] = 1;
        /* Blob v6: `disabled` on button/input seeds WN_STATE_DISABLED. */
        c->state[i] = c->nodes[i].initial_state;
        c->parent[i] = c->nodes[i].parent;
        c->order_key[i] = i;
        c->first_child[i] = -1;
        c->next_sibling[i] = -1;
        c->prev_sibling[i] = -1;
        c->last_child[i] = -1;
        c->input_map[i] = -1;
        c->input_cursor[i] = 0;
        c->input_anchor[i] = -1;
        c->input_scroll[i] = 0.0f;
    }
    c->input_focus = -1;
    c->next_order = count;

    build_hierarchy(c);
    c->text_atlas = wn_text_atlas_create(NULL, 0, 0);
    if (c->text_atlas == NULL) {
        wn_set_error("wn_core_create: out of memory for the text atlas");
        wn_core_destroy(c);
        return NULL;
    }
    if (build_input_state(c) != 0) {
        wn_set_error("wn_core_create: cannot build the input spec");
        wn_core_destroy(c);
        return NULL;
    }
    layout_page(c, (float)c->width, (float)c->height);
    wn_core_build_display_list(c);
    return c;
}

/* ---------------------------------------------------------- dynamic arena */

static void free_node_payload(wn_core *c, int node)
{
    int i;
    if (c->dynamic_node != NULL && c->dynamic_node[node]) {
        wn_free_((void *)c->names[node]);
        c->names[node] = NULL;
    }
    if (c->text_override != NULL) {
        wn_free_(c->text_override[node]);
        c->text_override[node] = NULL;
    }
    if (c->image_override != NULL) {
        wn_free_(c->image_override[node]);
        c->image_override[node] = NULL;
    }
    if (c->input_value != NULL && c->input_value[node] != NULL) {
        wn_free_(c->input_value[node]);
        c->input_value[node] = NULL;
    }
    if (c->input_map != NULL) {
        c->input_map[node] = -1;
    }
    if (c->input_focus == node) {
        c->input_focus = -1;
    }
    if (c->classes != NULL && c->class_count != NULL) {
        for (i = 0; i < c->class_count[node] && i < WN_MAX_CLASSES; ++i) {
            wn_free_(c->classes[node * WN_MAX_CLASSES + i]);
            c->classes[node * WN_MAX_CLASSES + i] = NULL;
        }
        c->class_count[node] = 0;
    }
}

/* realloc one parallel array. On failure the previous block stays valid: the
 * logical capacity is only published after every array grew, so a partially
 * grown arena is still consistent (the extra capacity is simply unused). */
#define WN_GROW_ARRAY(c, field, elements)                                    \
    do {                                                                     \
        void *grown_ = realloc((c)->field,                                   \
                               (size_t)(elements) * sizeof(*(c)->field));    \
        if (grown_ == NULL) {                                                \
            return -1;                                                       \
        }                                                                    \
        (c)->field = grown_;                                                 \
    } while (0)

/* Grow the arena to at least `minimum` slots. New slots are uninitialised by
 * design: arena_alloc() writes every parallel array for a slot before it can
 * be observed. */
static int arena_reserve(wn_core *c, int minimum)
{
    int capacity;
    if (minimum <= c->node_capacity) {
        return 0;
    }
    if (minimum > WN_NODE_MAX_CAPACITY) {
        return -1;
    }
    capacity = c->node_capacity > 0 ? c->node_capacity : WN_NODE_MIN_CAPACITY;
    while (capacity < minimum) {
        int next = capacity + capacity / 2;
        if (next <= capacity) {
            capacity = minimum;
            break;
        }
        capacity = next;
    }
    if (capacity > WN_NODE_MAX_CAPACITY) {
        capacity = WN_NODE_MAX_CAPACITY;
    }

    WN_GROW_ARRAY(c, nodes, capacity);
    WN_GROW_ARRAY(c, names, capacity);
    WN_GROW_ARRAY(c, generation, capacity);
    WN_GROW_ARRAY(c, alive, capacity);
    WN_GROW_ARRAY(c, dynamic_node, capacity);
    WN_GROW_ARRAY(c, visible, capacity);
    WN_GROW_ARRAY(c, visible_eff, capacity);
    WN_GROW_ARRAY(c, state, capacity);
    WN_GROW_ARRAY(c, parent, capacity);
    WN_GROW_ARRAY(c, order_key, capacity);
    WN_GROW_ARRAY(c, free_list, capacity);
    WN_GROW_ARRAY(c, rects, capacity);
    WN_GROW_ARRAY(c, clips, capacity);
    WN_GROW_ARRAY(c, scroll_y, capacity);
    WN_GROW_ARRAY(c, scroll_max_y, capacity);
    WN_GROW_ARRAY(c, first_child, capacity);
    WN_GROW_ARRAY(c, next_sibling, capacity);
    WN_GROW_ARRAY(c, prev_sibling, capacity);
    WN_GROW_ARRAY(c, last_child, capacity);
    WN_GROW_ARRAY(c, paint_items, capacity);
    c->paint_item_capacity = capacity;
    WN_GROW_ARRAY(c, text_override, capacity);
    WN_GROW_ARRAY(c, image_override, capacity);
    WN_GROW_ARRAY(c, input_map, capacity);
    WN_GROW_ARRAY(c, input_value, capacity);
    WN_GROW_ARRAY(c, input_cursor, capacity);
    WN_GROW_ARRAY(c, input_anchor, capacity);
    WN_GROW_ARRAY(c, input_scroll, capacity);
    WN_GROW_ARRAY(c, class_count, capacity);
    WN_GROW_ARRAY(c, classes, (size_t)capacity * WN_MAX_CLASSES);
    WN_GROW_ARRAY(c, scroll_x, capacity);
    WN_GROW_ARRAY(c, scroll_max_x, capacity);

    c->node_capacity = capacity;
    return 0;
}

#undef WN_GROW_ARRAY

static int arena_alloc(wn_core *c)
{
    int id;
    int i;
    if (c->free_count > 0) {
        id = c->free_list[--c->free_count];
    } else {
        if (c->node_count >= c->node_capacity &&
            arena_reserve(c, c->node_count + 1) != 0) {
            return -1;
        }
        id = c->node_count++;
        c->generation[id] = 0;
    }
    /* Bumped for every allocation, reused slot or not: handles issued before
     * the previous tenant was released stop resolving. */
    c->generation[id] = c->generation[id] == UINT32_MAX
                            ? 1u
                            : c->generation[id] + 1u;
    memset(&c->nodes[id], 0, sizeof(UiNode));
    c->nodes[id].parent = -1;
    c->nodes[id].object_fit = R2_OBJECT_FIT_FILL;
    c->names[id] = NULL;
    c->alive[id] = 1;
    c->dynamic_node[id] = 1;
    c->visible[id] = 1;
    c->visible_eff[id] = 1;
    c->state[id] = 0;
    c->parent[id] = -1;
    c->order_key[id] = c->next_order++;
    c->rects[id].x = c->rects[id].y = 0.0f;
    c->rects[id].w = c->rects[id].h = 0.0f;
    c->clips[id] = c->rects[id];
    c->scroll_x[id] = 0.0f;
    c->scroll_y[id] = 0.0f;
    c->scroll_max_x[id] = 0.0f;
    c->scroll_max_y[id] = 0.0f;
    c->first_child[id] = -1;
    c->next_sibling[id] = -1;
    c->prev_sibling[id] = -1;
    c->last_child[id] = -1;
    c->text_override[id] = NULL;
    c->image_override[id] = NULL;
    c->input_map[id] = -1;
    c->input_value[id] = NULL;
    c->input_cursor[id] = 0;
    c->input_anchor[id] = -1;
    c->input_scroll[id] = 0.0f;
    c->class_count[id] = 0;
    for (i = 0; i < WN_MAX_CLASSES; ++i) {
        c->classes[id * WN_MAX_CLASSES + i] = NULL;
    }
    ++c->live_count;
    return id;
}

static void arena_release(wn_core *c, int node)
{
    if (!c->alive[node]) {
        return;
    }
    free_node_payload(c, node);
    c->alive[node] = 0;
    c->dynamic_node[node] = 0;
    c->names[node] = NULL;
    c->parent[node] = -1;
    c->nodes[node].parent = -1;
    c->first_child[node] = -1;
    c->next_sibling[node] = -1;
    c->prev_sibling[node] = -1;
    c->last_child[node] = -1;
    c->visible[node] = 0;
    c->visible_eff[node] = 0;
    c->state[node] = 0;
    c->rects[node].x = c->rects[node].y = 0.0f;
    c->rects[node].w = c->rects[node].h = 0.0f;
    c->clips[node] = c->rects[node];
    c->scroll_x[node] = 0.0f;
    c->scroll_y[node] = 0.0f;
    c->scroll_max_x[node] = 0.0f;
    c->scroll_max_y[node] = 0.0f;
    if (c->free_count < c->node_capacity) {
        c->free_list[c->free_count++] = node;
    }
    --c->live_count;
}

static void mark_core_dirty(wn_core *c)
{
    c->dirty = 1;
    c->needs_layout = 1;
}

int wn_core_hierarchy_rebuilds(const wn_core *core)
{
    return core != NULL ? core->hierarchy_rebuilds : 0;
}

/* O(1) child-list maintenance. Structural mutations always append at the tail
 * (order_key is monotonically increasing), so the sorted walk of the initial
 * rebuild is never needed at runtime. */
static void link_child(wn_core *c, int parent, int child)
{
    int tail = c->last_child[parent];
    c->parent[child] = parent;
    c->nodes[child].parent = parent;
    c->prev_sibling[child] = tail;
    c->next_sibling[child] = -1;
    if (tail >= 0) {
        c->next_sibling[tail] = child;
    } else {
        c->first_child[parent] = child;
    }
    c->last_child[parent] = child;
}

static void unlink_child(wn_core *c, int child)
{
    int parent = c->parent[child];
    int prev = c->prev_sibling[child];
    int next = c->next_sibling[child];
    if (parent >= 0) {
        if (prev >= 0) {
            c->next_sibling[prev] = next;
        } else {
            c->first_child[parent] = next;
        }
        if (next >= 0) {
            c->prev_sibling[next] = prev;
        } else {
            c->last_child[parent] = prev;
        }
    }
    c->prev_sibling[child] = -1;
    c->next_sibling[child] = -1;
}

int wn_core_dirty(const wn_core *core)
{
    return core != NULL ? core->dirty : 0;
}

int wn_core_needs_layout(const wn_core *core)
{
    return core != NULL ? core->needs_layout : 0;
}

void wn_core_clear_dirty(wn_core *core)
{
    if (core != NULL) {
        core->dirty = 0;
    }
}

void wn_core_mark_dirty(wn_core *core)
{
    if (core != NULL) {
        core->dirty = 1;
    }
}

int wn_core_set_visible(wn_core *core, int node, int visible)
{
    int i;
    if (core == NULL || !wn_core_node_is_alive(core, node)) {
        return -1;
    }
    core->visible[node] = visible != 0 ? 1 : 0;
    if (!core->visible[node]) {
        /* Drop any state bit owned by a node inside the hidden subtree. */
        for (i = 0; i < core->node_count; ++i) {
            int walk;
            if (core->state[i] == 0u) {
                continue;
            }
            walk = i;
            while (walk >= 0 && walk != node) {
                walk = core->parent[walk];
            }
            if (walk == node) {
                core->state[i] = 0;
                if (core->hover == i) {
                    core->hover = -1;
                }
                if (core->press == i) {
                    core->press = -1;
                }
            }
        }
        if (core->input_focus >= 0) {
            int walk = core->input_focus;
            while (walk >= 0) {
                if (walk == node) {
                    core->input_anchor[core->input_focus] = -1;
                    core->input_focus = -1;
                    break;
                }
                walk = core->parent[walk];
            }
        }
    }
    mark_core_dirty(core);
    return 0;
}

int wn_core_set_image(wn_core *core, int node, const char *path)
{
    char *copy;
    UiNode *n;
    if (core == NULL || !wn_core_node_is_alive(core, node) || path == NULL ||
        path[0] == '\0') {
        return -1;
    }
    n = &core->nodes[node];
    if (n->kind != R2_NODE_IMAGE) {
        return 1;
    }
    copy = wn_strdup_(path);
    if (copy == NULL) {
        return -2;
    }
    wn_free_(core->image_override[node]);
    core->image_override[node] = copy;
    mark_core_dirty(core);
    return 0;
}

/* ABI v6: the host knows the new file's pixel size after wn_core_set_image.
 * The mutable node copy carries the intrinsic size used by layout/object-fit,
 * so updating it and marking layout dirty is enough. (0,0) means unknown. */
int wn_core_set_image_size(wn_core *core, int node, float width, float height)
{
    UiNode *n;
    if (core == NULL || !wn_core_node_is_alive(core, node)) {
        return -1;
    }
    if (!isfinite(width) || !isfinite(height) || width < 0.0f || height < 0.0f) {
        return -1;
    }
    n = &core->nodes[node];
    if (n->kind != R2_NODE_IMAGE) {
        return 1;
    }
    n->image_width = width;
    n->image_height = height;
    mark_core_dirty(core);
    return 0;
}

static void collect_subtree(wn_core *c, int node, int *out, int *count)
{
    int child;
    if (!c->alive[node]) {
        return;
    }
    out[(*count)++] = node;
    for (child = c->first_child[node]; child >= 0; child = c->next_sibling[child]) {
        collect_subtree(c, child, out, count);
    }
}

int wn_core_clone(wn_core *core, int template_node, int parent)
{
    int *src_ids;
    int *new_ids;
    int *map;
    int count = 0;
    int i;
    int result = -1;
    int oom = 0;
    int slots;
    char name[64];
    if (core == NULL || !wn_core_node_is_alive(core, template_node) ||
        !wn_core_node_is_alive(core, parent)) {
        return -1;
    }
    slots = core->node_count;
    src_ids = (int *)malloc((size_t)slots * sizeof(int));
    new_ids = (int *)malloc((size_t)slots * sizeof(int));
    map = (int *)malloc((size_t)slots * sizeof(int));
    if (src_ids == NULL || new_ids == NULL || map == NULL) {
        free(src_ids);
        free(new_ids);
        free(map);
        return -2;
    }
    collect_subtree(core, template_node, src_ids, &count);
    if (count <= 0 || core->live_count + count > WN_NODE_MAX_CAPACITY) {
        free(src_ids);
        free(new_ids);
        free(map);
        return -2;
    }
    /* Grow once for the whole subtree instead of per slot. */
    if (arena_reserve(core, core->live_count + count) != 0) {
        free(src_ids);
        free(new_ids);
        free(map);
        return -2;
    }
    for (i = 0; i < slots; ++i) {
        map[i] = -1;
    }
    for (i = 0; i < count; ++i) {
        map[src_ids[i]] = i;
    }
    for (i = 0; i < count; ++i) {
        new_ids[i] = arena_alloc(core);
        if (new_ids[i] < 0) {
            while (i > 0) {
                arena_release(core, new_ids[--i]);
            }
            free(src_ids);
            free(new_ids);
            free(map);
            return -2;
        }
    }
    for (i = 0; i < count; ++i) {
        int src = src_ids[i];
        int dst = new_ids[i];
        int src_parent = core->parent[src];
        core->nodes[dst] = core->nodes[src];
        core->nodes[dst].parent = -1;
        core->parent[dst] = (src_parent >= 0 && map[src_parent] >= 0)
                                ? new_ids[map[src_parent]]
                                : parent;
        if (i == 0) {
            core->parent[dst] = parent;
        }
        core->nodes[dst].parent = core->parent[dst];
        core->visible[dst] = core->visible[src];
        snprintf(name, sizeof(name), "clone#%d", core->clone_serial++);
        core->names[dst] = wn_strdup_(name);
        if (core->names[dst] == NULL) {
            for (i = 0; i < count; ++i) {
                arena_release(core, new_ids[i]);
            }
            free(src_ids);
            free(new_ids);
            free(map);
            return -2;
        }
        core->dynamic_node[dst] = 1;
        if (core->text_override[src] != NULL &&
            (core->text_override[dst] = wn_strdup_(core->text_override[src])) == NULL) {
            oom = 1;
        }
        if (core->image_override[src] != NULL &&
            (core->image_override[dst] = wn_strdup_(core->image_override[src])) == NULL) {
            oom = 1;
        }
        /* A cloned input shares the template's glyph pool but gets its own
         * editable copy of the value. */
        core->input_map[dst] = core->input_map[src];
        if (core->input_map[src] >= 0) {
            const char *source_value =
                core->input_value[src] != NULL ? core->input_value[src] : "";
            core->input_value[dst] = input_alloc_value(source_value);
            if (core->input_value[dst] == NULL) {
                oom = 1;
            }
            core->input_cursor[dst] = core->input_cursor[src];
            core->input_anchor[dst] = core->input_anchor[src];
            core->input_scroll[dst] = core->input_scroll[src];
        }
        core->class_count[dst] = core->class_count[src];
        for (int k = 0; k < core->class_count[src] && k < WN_MAX_CLASSES; ++k) {
            core->classes[dst * WN_MAX_CLASSES + k] =
                wn_strdup_(core->classes[src * WN_MAX_CLASSES + k]);
            if (core->classes[dst * WN_MAX_CLASSES + k] == NULL) {
                oom = 1;
            }
        }
        if (oom) {
            for (i = 0; i < count; ++i) {
                arena_release(core, new_ids[i]);
            }
            free(src_ids);
            free(new_ids);
            free(map);
            return -2;
        }
    }
    /* Link the new nodes only after every payload copy succeeded. Pre-order
     * allocation means a node's parent is already linked when it is reached,
     * so appending at the tail preserves sibling order in O(1) per node. */
    for (i = 0; i < count; ++i) {
        link_child(core, core->parent[new_ids[i]], new_ids[i]);
    }
    mark_core_dirty(core);
    result = new_ids[0];
    free(src_ids);
    free(new_ids);
    free(map);
    return result;
}

int wn_core_remove(wn_core *core, int node)
{
    int *ids;
    int count = 0;
    int i;
    if (core == NULL || !wn_core_node_is_alive(core, node)) {
        return -1;
    }
    if (core->parent[node] < 0) {
        return -1; /* root */
    }
    ids = (int *)malloc((size_t)core->node_count * sizeof(int));
    if (ids == NULL) {
        return -2;
    }
    collect_subtree(core, node, ids, &count);
    /* Detach the whole subtree with one O(1) unlink; the released slots keep
     * their internal links until arena_release() resets them. */
    unlink_child(core, node);
    for (i = 0; i < count; ++i) {
        core->state[ids[i]] = 0;
        if (core->hover == ids[i]) core->hover = -1;
        if (core->press == ids[i]) core->press = -1;
    }
    for (i = count - 1; i >= 0; --i) {
        arena_release(core, ids[i]);
    }
    free(ids);
    mark_core_dirty(core);
    return 0;
}

int wn_core_clear_children(wn_core *core, int node)
{
    int child;
    int result = 0;
    if (core == NULL || !wn_core_node_is_alive(core, node)) {
        return -1;
    }
    child = core->first_child[node];
    while (child >= 0) {
        int next = core->next_sibling[child];
        int status = wn_core_remove(core, child);
        if (status != 0) {
            result = status;
        }
        child = next;
    }
    return result;
}

static int is_ancestor(const wn_core *c, int maybe_ancestor, int node)
{
    int walk = node;
    while (walk >= 0) {
        if (walk == maybe_ancestor) {
            return 1;
        }
        walk = c->parent[walk];
    }
    return 0;
}

int wn_core_append(wn_core *core, int parent, int child)
{
    if (core == NULL || !wn_core_node_is_alive(core, parent) ||
        !wn_core_node_is_alive(core, child)) {
        return -1;
    }
    if (parent == child || is_ancestor(core, child, parent)) {
        return -1;
    }
    if (core->parent[child] < 0) {
        return -1; /* document root */
    }
    unlink_child(core, child);
    core->order_key[child] = core->next_order++;
    link_child(core, parent, child);
    mark_core_dirty(core);
    return 0;
}

void wn_core_destroy(wn_core *core)
{
    int i;
    const wn_document *document;
    if (core == NULL) {
        return;
    }
    document = core->doc;
    if (core->nodes != NULL) {
        for (i = 0; i < core->node_count; ++i) {
            if (core->alive != NULL && core->alive[i]) {
                free_node_payload(core, i);
            }
        }
    }
    wn_free_(core->nodes);
    wn_free_((void *)core->names);
    wn_free_(core->generation);
    wn_free_(core->alive);
    wn_free_(core->dynamic_node);
    wn_free_(core->visible);
    wn_free_(core->visible_eff);
    wn_free_(core->state);
    wn_free_(core->parent);
    wn_free_(core->order_key);
    wn_free_(core->free_list);
    wn_free_(core->rects);
    wn_free_(core->clips);
    wn_free_(core->scroll_x);
    wn_free_(core->scroll_y);
    wn_free_(core->scroll_max_x);
    wn_free_(core->scroll_max_y);
    wn_free_(core->first_child);
    wn_free_(core->next_sibling);
    wn_free_(core->prev_sibling);
    wn_free_(core->last_child);
    wn_free_(core->paint_items);
    wn_free_(core->paint_layers);
    wn_free_(core->paint_commands);
    wn_free_(core->paint_draws);
    wn_free_(core->text_override);
    wn_free_(core->image_override);
    wn_free_(core->input_map);
    wn_free_(core->input_value);
    wn_free_(core->input_cursor);
    wn_free_(core->input_anchor);
    wn_free_(core->input_scroll);
    wn_free_(core->input_records);
    wn_text_atlas_destroy(core->text_atlas);
    wn_free_(core->classes);
    wn_free_(core->class_count);
    wn_free_(core->vertices);
    wn_free_(core->solid_batches);
    wn_free_(core->glyph_vertices);
    wn_free_(core->glyph_batches);
    wn_free_(core->image_vertices);
    wn_free_(core->image_batches);
    wn_free_(core);
    wn_document_release((wn_document *)document);
}
