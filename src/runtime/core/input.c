/*
 * input.c - single-line ASCII editor state, editing, caret/selection and the
 * input geometry (text run, selection quad, focus ring, caret).
 *
 * Only the input spec lives in the document; the value is shaped and painted
 * by the runtime like any other string.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wn_core_internal.h"
#include "wn_internal.h"

const WnInputRecord *input_record(const wn_core *c, int node)
{
    int index;
    if (c == NULL || node < 0 || node >= c->node_count || c->input_map == NULL ||
        c->input_records == NULL) {
        return NULL;
    }
    index = c->input_map[node];
    if (index < 0 || index >= c->input_record_count) {
        return NULL;
    }
    return &c->input_records[index];
}

static const char *input_text(const wn_core *c, int node)
{
    if (c->input_value == NULL || node < 0 || node >= c->node_count ||
        c->input_value[node] == NULL) {
        return "";
    }
    return c->input_value[node];
}

static int input_length(const wn_core *c, int node)
{
    return (int)strlen(input_text(c, node));
}

static int input_limit(const WnInputRecord *record)
{
    if (record != NULL && record->max_length > 0) {
        return record->max_length;
    }
    return WN_INPUT_MAX_BYTES;
}

static const wn_atlas_run *input_run(const wn_core *c, int node, const char *text)
{
    const UiNode *n = &c->nodes[node];
    if (c->text_atlas == NULL || text == NULL) {
        return NULL;
    }
    return wn_text_atlas_shape(c->text_atlas, text, n->font_family, n->font_size,
                               n->font_weight);
}

static float input_prefix_width(const wn_core *c, int node, const char *text, int upto)
{
    const wn_atlas_run *run = input_run(c, node, text);
    float pen = 0.0f;
    int i;
    if (run == NULL) {
        return 0.0f;
    }
    if (upto > run->count) {
        upto = run->count;
    }
    for (i = 0; i < upto; ++i) {
        pen += run->glyphs[i].advance;
    }
    return pen;
}

static int input_line_metrics(const wn_core *c, int node, float *ascent, float *line_height)
{
    const UiNode *n = &c->nodes[node];
    wn_text_metrics metrics;
    if (wn_text_measure_string("Mg", n->font_family, n->font_size, n->font_weight, 0.0f,
                               &metrics) != 0) {
        return 1;
    }
    if (ascent != NULL) {
        *ascent = metrics.ascent;
    }
    if (line_height != NULL) {
        *line_height = metrics.ascent + metrics.descent;
        if (*line_height <= 0.0f) {
            *line_height = metrics.height;
        }
    }
    return 0;
}

float input_line_height(const wn_core *c, int node)
{
    float height = 0.0f;
    if (input_line_metrics(c, node, NULL, &height) == 0 && height > 0.0f) {
        return height;
    }
    return c->nodes[node].font_size * 1.35f;
}

static wn_rect input_content_rect(const wn_core *c, int node)
{
    const UiNode *n = &c->nodes[node];
    const wn_rect *r = &c->rects[node];
    wn_rect box;
    box.x = r->x + n->padding[3];
    box.y = r->y + n->padding[0];
    box.w = fmaxf(r->w - n->padding[1] - n->padding[3], 0.0f);
    box.h = fmaxf(r->h - n->padding[0] - n->padding[2], 0.0f);
    return box;
}

static int input_has_selection(const wn_core *c, int node)
{
    return c->input_anchor[node] >= 0 &&
           c->input_anchor[node] != c->input_cursor[node];
}

static void input_selection_bounds(const wn_core *c, int node, int *start, int *end)
{
    int anchor = c->input_anchor[node];
    int cursor = c->input_cursor[node];
    if (anchor < 0 || anchor == cursor) {
        *start = cursor;
        *end = cursor;
        return;
    }
    *start = anchor < cursor ? anchor : cursor;
    *end = anchor < cursor ? cursor : anchor;
}

static void input_delete_range(wn_core *c, int node, int start, int end)
{
    char *value = c->input_value != NULL ? c->input_value[node] : NULL;
    int len;
    if (value == NULL) {
        return;
    }
    len = (int)strlen(value);
    if (start < 0) {
        start = 0;
    }
    if (end > len) {
        end = len;
    }
    if (start >= end) {
        return;
    }
    memmove(value + start, value + end, (size_t)(len - end) + 1u);
    c->input_cursor[node] = start;
    c->input_anchor[node] = -1;
}

static void input_move_cursor(wn_core *c, int node, int position, int extend)
{
    int len = input_length(c, node);
    if (position < 0) {
        position = 0;
    }
    if (position > len) {
        position = len;
    }
    if (extend) {
        if (c->input_anchor[node] < 0) {
            c->input_anchor[node] = c->input_cursor[node];
        }
    } else {
        c->input_anchor[node] = -1;
    }
    c->input_cursor[node] = position;
}

static void input_update_scroll(wn_core *c, int node)
{
    const WnInputRecord *record = input_record(c, node);
    const char *value;
    wn_rect content;
    float caret;
    float scroll;
    float text_width;
    float max_scroll;
    if (record == NULL || c->input_scroll == NULL) {
        return;
    }
    content = input_content_rect(c, node);
    if (content.w <= 0.0f) {
        c->input_scroll[node] = 0.0f;
        return;
    }
    value = input_text(c, node);
    caret = input_prefix_width(c, node, value, c->input_cursor[node]);
    scroll = c->input_scroll[node];
    if (caret - scroll > content.w - WN_INPUT_CARET_WIDTH) {
        scroll = caret - content.w + WN_INPUT_CARET_WIDTH;
    }
    if (caret - scroll < 0.0f) {
        scroll = caret;
    }
    text_width = input_prefix_width(c, node, value, (int)strlen(value));
    max_scroll = text_width + WN_INPUT_CARET_WIDTH - content.w;
    if (max_scroll < 0.0f) {
        max_scroll = 0.0f;
    }
    if (scroll > max_scroll) {
        scroll = max_scroll;
    }
    if (scroll < 0.0f) {
        scroll = 0.0f;
    }
    c->input_scroll[node] = scroll;
}

int wn_core_node_is_input(const wn_core *core, int node)
{
    return input_record(core, node) != NULL;
}

int wn_core_input_focused(const wn_core *core)
{
    return core != NULL ? core->input_focus : -1;
}

int wn_core_focus_input(wn_core *core, int node, float pointer_x)
{
    const WnInputRecord *record;
    const char *value;
    wn_rect content;
    float pen;
    int len;
    int caret;
    int i;
    if (core == NULL) {
        return -1;
    }
    record = input_record(core, node);
    if (record == NULL) {
        return -1;
    }
    ensure_layout_for_hit(core);
    if (core->input_focus >= 0 && core->input_focus != node) {
        core->input_anchor[core->input_focus] = -1;
    }
    core->input_focus = node;
    core->input_anchor[node] = -1;
    input_update_scroll(core, node);
    content = input_content_rect(core, node);
    value = input_text(core, node);
    len = (int)strlen(value);
    pen = content.x - core->input_scroll[node];
    caret = len;
    {
        const wn_atlas_run *run = input_run(core, node, value);
        for (i = 0; i < len; ++i) {
            float advance = (run != NULL && i < run->count) ? run->glyphs[i].advance : 0.0f;
            if (pointer_x < pen + advance * 0.5f) {
                caret = i;
                break;
            }
            pen += advance;
        }
    }
    core->input_cursor[node] = caret;
    core->dirty = 1;
    return node;
}

int wn_core_blur_input(wn_core *core)
{
    int node;
    if (core == NULL) {
        return -1;
    }
    node = core->input_focus;
    if (node < 0) {
        return -1;
    }
    core->input_anchor[node] = -1;
    core->input_focus = -1;
    core->dirty = 1;
    return 0;
}

int wn_core_input_char(wn_core *core, uint32_t codepoint,
                       int *value_changed, int *visual_changed)
{
    const WnInputRecord *record;
    char *value;
    int node;
    int len;
    int limit;
    int cursor;
    if (value_changed != NULL) {
        *value_changed = 0;
    }
    if (visual_changed != NULL) {
        *visual_changed = 0;
    }
    if (core == NULL || codepoint < 32u || codepoint > 126u) {
        return -1;
    }
    node = core->input_focus;
    record = input_record(core, node);
    if (record == NULL || core->input_value == NULL) {
        return -1;
    }
    value = core->input_value[node];
    if (value == NULL) {
        return -1;
    }
    if (input_has_selection(core, node)) {
        int start;
        int end;
        input_selection_bounds(core, node, &start, &end);
        input_delete_range(core, node, start, end);
    }
    len = (int)strlen(value);
    limit = input_limit(record);
    cursor = core->input_cursor[node];
    if (cursor < 0) {
        cursor = 0;
    }
    if (cursor > len) {
        cursor = len;
    }
    if (len >= limit) {
        /* Consumed but unchanged: the host must not beep when typing into a
         * full field. */
        return node;
    }
    memmove(value + cursor + 1, value + cursor, (size_t)(len - cursor) + 1u);
    value[cursor] = (char)codepoint;
    core->input_cursor[node] = cursor + 1;
    core->input_anchor[node] = -1;
    core->dirty = 1;
    if (value_changed != NULL) {
        *value_changed = 1;
    }
    if (visual_changed != NULL) {
        *visual_changed = 1;
    }
    return node;
}

int wn_core_input_key(wn_core *core, int key, uint32_t modifiers,
                      int *value_changed, int *visual_changed)
{
    const WnInputRecord *record;
    char *value;
    int node;
    int len;
    int cursor;
    int extend = (modifiers & WN_KEY_MOD_SHIFT) != 0u;
    if (value_changed != NULL) {
        *value_changed = 0;
    }
    if (visual_changed != NULL) {
        *visual_changed = 0;
    }
    if (core == NULL) {
        return -1;
    }
    node = core->input_focus;
    record = input_record(core, node);
    if (record == NULL || core->input_value == NULL) {
        return -1;
    }
    value = core->input_value[node];
    if (value == NULL) {
        return -1;
    }
    len = (int)strlen(value);
    cursor = core->input_cursor[node];
    switch (key) {
    case WN_KEY_ESCAPE:
        wn_core_blur_input(core);
        if (visual_changed != NULL) {
            *visual_changed = 1;
        }
        return node;
    case WN_KEY_LEFT:
        if (!extend && input_has_selection(core, node)) {
            int start;
            int end;
            input_selection_bounds(core, node, &start, &end);
            input_move_cursor(core, node, start, 0);
            (void)end;
        } else {
            input_move_cursor(core, node, cursor - 1, extend);
        }
        core->dirty = 1;
        if (visual_changed != NULL) {
            *visual_changed = 1;
        }
        return node;
    case WN_KEY_RIGHT:
        if (!extend && input_has_selection(core, node)) {
            int start;
            int end;
            input_selection_bounds(core, node, &start, &end);
            input_move_cursor(core, node, end, 0);
            (void)start;
        } else {
            input_move_cursor(core, node, cursor + 1, extend);
        }
        core->dirty = 1;
        if (visual_changed != NULL) {
            *visual_changed = 1;
        }
        return node;
    case WN_KEY_HOME:
        input_move_cursor(core, node, 0, extend);
        core->dirty = 1;
        if (visual_changed != NULL) {
            *visual_changed = 1;
        }
        return node;
    case WN_KEY_END:
        input_move_cursor(core, node, len, extend);
        core->dirty = 1;
        if (visual_changed != NULL) {
            *visual_changed = 1;
        }
        return node;
    case WN_KEY_BACKSPACE:
        if (input_has_selection(core, node)) {
            int start;
            int end;
            input_selection_bounds(core, node, &start, &end);
            input_delete_range(core, node, start, end);
        } else if (cursor > 0) {
            input_delete_range(core, node, cursor - 1, cursor);
        } else {
            return node;
        }
        core->dirty = 1;
        if (value_changed != NULL) {
            *value_changed = 1;
        }
        if (visual_changed != NULL) {
            *visual_changed = 1;
        }
        return node;
    case WN_KEY_DELETE:
        if (input_has_selection(core, node)) {
            int start;
            int end;
            input_selection_bounds(core, node, &start, &end);
            input_delete_range(core, node, start, end);
        } else if (cursor < len) {
            input_delete_range(core, node, cursor, cursor + 1);
        } else {
            return node;
        }
        core->dirty = 1;
        if (value_changed != NULL) {
            *value_changed = 1;
        }
        if (visual_changed != NULL) {
            *visual_changed = 1;
        }
        return node;
    default:
        return -1;
    }
}

int wn_core_input_select_all(wn_core *core, int *visual_changed)
{
    int node;
    if (visual_changed != NULL) {
        *visual_changed = 0;
    }
    if (core == NULL) {
        return -1;
    }
    node = core->input_focus;
    if (input_record(core, node) == NULL) {
        return -1;
    }
    core->input_anchor[node] = 0;
    core->input_cursor[node] = input_length(core, node);
    core->dirty = 1;
    if (visual_changed != NULL) {
        *visual_changed = 1;
    }
    return node;
}

int wn_core_input_copy(wn_core *core, char *out, int capacity)
{
    int node;
    int start;
    int end;
    int count;
    const char *value;
    if (core == NULL || out == NULL || capacity <= 0) {
        return -1;
    }
    node = core->input_focus;
    if (input_record(core, node) == NULL) {
        return -1;
    }
    out[0] = '\0';
    input_selection_bounds(core, node, &start, &end);
    if (start >= end) {
        return 0;
    }
    value = input_text(core, node);
    count = end - start;
    if (count >= capacity) {
        count = capacity - 1;
    }
    memcpy(out, value + start, (size_t)count);
    out[count] = '\0';
    return count;
}

int wn_core_input_cut(wn_core *core, char *out, int capacity,
                      int *value_changed, int *visual_changed)
{
    int node;
    int copied;
    int start;
    int end;
    if (value_changed != NULL) {
        *value_changed = 0;
    }
    if (visual_changed != NULL) {
        *visual_changed = 0;
    }
    copied = wn_core_input_copy(core, out, capacity);
    if (copied <= 0) {
        return copied;
    }
    node = core->input_focus;
    input_selection_bounds(core, node, &start, &end);
    input_delete_range(core, node, start, end);
    core->dirty = 1;
    if (value_changed != NULL) {
        *value_changed = 1;
    }
    if (visual_changed != NULL) {
        *visual_changed = 1;
    }
    return copied;
}

int wn_core_input_paste(wn_core *core, const char *text,
                        int *value_changed, int *visual_changed)
{
    const WnInputRecord *record;
    char *value;
    int node;
    int len;
    int limit;
    int cursor;
    int inserted = 0;
    int deleted = 0;
    int i;
    if (value_changed != NULL) {
        *value_changed = 0;
    }
    if (visual_changed != NULL) {
        *visual_changed = 0;
    }
    if (core == NULL || text == NULL) {
        return -1;
    }
    node = core->input_focus;
    record = input_record(core, node);
    if (record == NULL || core->input_value == NULL || core->input_value[node] == NULL) {
        return -1;
    }
    value = core->input_value[node];
    if (input_has_selection(core, node)) {
        int start;
        int end;
        input_selection_bounds(core, node, &start, &end);
        input_delete_range(core, node, start, end);
        deleted = 1;
    }
    len = (int)strlen(value);
    limit = input_limit(record);
    cursor = core->input_cursor[node];
    if (cursor < 0) {
        cursor = 0;
    }
    if (cursor > len) {
        cursor = len;
    }
    for (i = 0; text[i] != '\0' && len < limit; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 32 || ch > 126) {
            continue;
        }
        memmove(value + cursor + 1, value + cursor, (size_t)(len - cursor) + 1u);
        value[cursor] = (char)ch;
        ++cursor;
        ++len;
        ++inserted;
    }
    if (inserted <= 0 && !deleted) {
        return node;
    }
    core->input_cursor[node] = cursor;
    core->input_anchor[node] = -1;
    core->dirty = 1;
    if (value_changed != NULL) {
        *value_changed = 1;
    }
    if (visual_changed != NULL) {
        *visual_changed = 1;
    }
    return node;
}

int wn_core_set_input_value(wn_core *core, int node, const char *text)
{
    const WnInputRecord *record;
    const unsigned char *p;
    char *copy;
    int len;
    int limit;
    if (core == NULL || text == NULL) {
        return -1;
    }
    record = input_record(core, node);
    if (record == NULL || core->input_value == NULL) {
        return -1;
    }
    for (p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (*p < 32u || *p > 126u) {
            return 1;
        }
    }
    len = (int)strlen(text);
    limit = input_limit(record);
    if (len > limit) {
        return 2;
    }
    copy = input_alloc_value(text);
    if (copy == NULL) {
        return -1;
    }
    wn_free_(core->input_value[node]);
    core->input_value[node] = copy;
    core->input_cursor[node] = len;
    core->input_anchor[node] = -1;
    core->dirty = 1;
    return 0;
}

const char *wn_core_input_value(const wn_core *core, int node)
{
    if (input_record(core, node) == NULL) {
        return NULL;
    }
    return input_text(core, node);
}

int wn_core_input_state(const wn_core *core, int node, wn_input_state *out)
{
    const WnInputRecord *record;
    const char *value;
    wn_rect content;
    float scroll;
    float caret;
    int start;
    int end;
    if (core == NULL || out == NULL) {
        return -1;
    }
    record = input_record(core, node);
    if (record == NULL) {
        return -1;
    }
    ensure_layout_for_hit((wn_core *)core);
    input_update_scroll((wn_core *)core, node);
    value = input_text(core, node);
    content = input_content_rect(core, node);
    scroll = core->input_scroll[node];
    caret = content.x +
            input_prefix_width(core, node, value, core->input_cursor[node]) - scroll;
    memset(out, 0, sizeof(*out));
    out->value = value;
    out->cursor = core->input_cursor[node];
    out->anchor = core->input_anchor[node];
    out->focused = core->input_focus == node;
    out->max_length = record->max_length;
    out->scroll = scroll;
    {
        float ascent = 0.0f;
        float line_height = 0.0f;
        input_line_metrics(core, node, &ascent, &line_height);
        out->baseline = ascent;
        out->line_height = line_height;
    }
    out->caret_x = caret;
    out->rect = core->rects[node];
    out->content = content;
    input_selection_bounds(core, node, &start, &end);
    if (start < end) {
        float x0 = content.x + input_prefix_width(core, node, value, start) - scroll;
        float x1 = content.x + input_prefix_width(core, node, value, end) - scroll;
        out->selection_x0 = x0;
        out->selection_x1 = x1;
    } else {
        out->selection_x0 = caret;
        out->selection_x1 = caret;
    }
    return 0;
}

void build_input_text_geometry(wn_core *c, float viewport_w, float viewport_h)
{
    int order_index;
    for (order_index = 0; order_index < c->paint_count; ++order_index) {
        int node = c->paint_order[order_index];
        const UiNode *n = &c->nodes[node];
        const WnInputRecord *record = input_record(c, node);
        const char *value;
        const char *text;
        unsigned int color;
        uint8_t a, cr, cg, cb;
        wn_rect content;
        wn_rect clip;
        float pen;
        float scroll;
        float line_offset;
        int use_placeholder;
        if (!c->visible_eff[node] || n->kind != R2_NODE_INPUT || record == NULL) {
            continue;
        }
        value = input_text(c, node);
        use_placeholder = value[0] == '\0' && record->placeholder[0] != '\0';
        text = use_placeholder ? record->placeholder : value;
        if (text[0] == '\0') {
            continue;
        }
        color = use_placeholder ? WN_INPUT_PLACEHOLDER_COLOR : n->text_color;
        a = (uint8_t)((color >> 24) & 0xFFu);
        if (a == 0u) {
            continue;
        }
        content = input_content_rect(c, node);
        clip = rect_intersection(c->clips[node], content);
        if (rect_is_empty(clip)) {
            continue;
        }
        input_update_scroll(c, node);
        scroll = c->input_scroll[node];
        set_glyph_clip(c, &clip);
        cr = (uint8_t)((color >> 16) & 0xFFu);
        cg = (uint8_t)((color >> 8) & 0xFFu);
        cb = (uint8_t)(color & 0xFFu);
        /* Single-line inputs center the line box vertically, like a browser
         * text field; a content box shorter than the line still overflows
         * symmetrically and is clipped. */
        {
            const wn_atlas_run *run = input_run(c, node, text);
            float baseline;
            if (run == NULL) {
                continue;
            }
            line_offset = (content.h - (run->ascent + run->descent)) * 0.5f;
            baseline = content.y + line_offset + run->ascent;
            pen = 0.0f;
            for (int i = 0; i < run->count; ++i) {
                const wn_atlas_glyph *packed = wn_text_atlas_glyph(
                    c->text_atlas, n->font_family, n->font_size, n->font_weight,
                    run->glyphs[i].glyph);
                if (packed == NULL || packed->width <= 0 || packed->height <= 0) {
                    continue;
                }
                float x = floorf(content.x + run->glyphs[i].x - scroll +
                                 (float)packed->bearing_x + 0.5f);
                float y = floorf(baseline + (float)packed->bearing_y + 0.5f);
                push_glyph_quad(c, x, y, (float)packed->width, (float)packed->height,
                                packed->u0, packed->v0, packed->u1, packed->v1,
                                viewport_w, viewport_h, cr, cg, cb, a);
            }
        }
    }
}

/* Selection quad, focus ring and caret. Solids are painted before glyphs, so
 * this runs after every background but before build_text_geometry. */
void build_input_decorations(wn_core *c)
{
    int order_index;
    for (order_index = 0; order_index < c->paint_count; ++order_index) {
        int node = c->paint_order[order_index];
        const UiNode *n = &c->nodes[node];
        const WnInputRecord *record = input_record(c, node);
        wn_rect content;
        wn_rect clip;
        float scroll;
        int start;
        int end;
        if (!c->visible_eff[node] || n->kind != R2_NODE_INPUT || record == NULL) {
            continue;
        }
        if (rect_is_empty(c->clips[node])) {
            continue;
        }
        content = input_content_rect(c, node);
        input_update_scroll(c, node);
        scroll = c->input_scroll[node];

        if (c->input_focus == node) {
            const wn_rect *r = &c->rects[node];
            const float ring = 2.0f;
            set_solid_clip(c, &c->clips[node]);
            push_rounded_ring(c, r->x, r->y, r->w, r->h, n->border_radius, ring,
                              WN_INPUT_FOCUS_COLOR);
        }

        clip = rect_intersection(c->clips[node], content);
        if (rect_is_empty(clip)) {
            continue;
        }
        set_solid_clip(c, &clip);

        if (c->input_focus == node && input_has_selection(c, node)) {
            float x0;
            float x1;
            input_selection_bounds(c, node, &start, &end);
            x0 = content.x +
                 input_prefix_width(c, node, input_text(c, node), start) - scroll;
            x1 = content.x + input_prefix_width(c, node, input_text(c, node), end) - scroll;
            if (x0 < content.x) {
                x0 = content.x;
            }
            if (x1 > content.x + content.w) {
                x1 = content.x + content.w;
            }
            if (x1 > x0) {
                push_quad(c, x0, content.y, x1 - x0, content.h,
                          (float)c->width, (float)c->height,
                          WN_INPUT_SELECTION_COLOR);
            }
        }

        if (c->input_focus == node) {
            float caret = content.x +
                          input_prefix_width(c, node, input_text(c, node),
                                             c->input_cursor[node]) -
                          scroll;
            float max_caret = content.x + content.w - WN_INPUT_CARET_WIDTH;
            if (caret < content.x) {
                caret = content.x;
            }
            if (caret > max_caret) {
                caret = max_caret;
            }
            caret = floorf(caret + 0.5f);
            if (content.h > 2.0f) {
                push_quad(c, caret, content.y + 1.0f, WN_INPUT_CARET_WIDTH,
                          content.h - 2.0f, (float)c->width, (float)c->height,
                          n->text_color);
            }
        }
    }
}

/* Copy the generated ui-tree/3 input tables into the core's compact state.
 * Compiled out entirely for pages whose ui_data.h has no input tables. */
char *input_alloc_value(const char *text)
{
    /* Fixed capacity: editing inserts in place up to WN_INPUT_MAX_BYTES. */
    char *buffer = (char *)calloc((size_t)WN_INPUT_MAX_BYTES + 1u, 1u);
    if (buffer != NULL && text != NULL) {
        size_t length = strlen(text);
        if (length > WN_INPUT_MAX_BYTES) {
            length = WN_INPUT_MAX_BYTES;
        }
        memcpy(buffer, text, length);
    }
    return buffer;
}

int build_input_state(wn_core *c)
{
    const wn_document *document = c->doc;
    int i;
    if (document->input_count <= 0) {
        return 0;
    }
    c->input_record_count = document->input_count;
    c->input_records = (WnInputRecord *)calloc(
        (size_t)document->input_count, sizeof(WnInputRecord));
    if (c->input_records == NULL) {
        return -1;
    }
    for (i = 0; i < document->input_count; ++i) {
        const UiInput *spec = &document->inputs[i];
        WnInputRecord *record = &c->input_records[i];
        int node = spec->node;
        record->node = node;
        record->placeholder = spec->placeholder != NULL ? spec->placeholder : "";
        record->max_length = spec->max_length;
        if (record->max_length < 0 || record->max_length > WN_INPUT_MAX_BYTES) {
            record->max_length = WN_INPUT_MAX_BYTES;
        }
        if (node >= 0 && node < c->node_count) {
            const char *value = spec->value != NULL ? spec->value : "";
            c->input_map[node] = i;
            c->input_value[node] = input_alloc_value(value);
            if (c->input_value[node] == NULL) {
                return -1;
            }
            c->input_cursor[node] = (int)strlen(value);
            c->input_anchor[node] = -1;
        }
    }
    return 0;
}
