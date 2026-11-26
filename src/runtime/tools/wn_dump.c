/*
 * wnui-dump.c - debug projection of a .wnui blob.
 *
 * The document is loaded with the runtime's own loader
 * (wn_document_load_blob); this tool never parses the file format itself. The
 * output is a *debug view*, not a contract: it may change with the blob
 * version. Use the blob hash for invariants, not this output.
 *
 * Usage:
 *   wnui-dump <file.wnui>            text projection
 *   wnui-dump --json <file.wnui>     JSON projection (stable key order)
 *   wnui-dump --help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "webnative.h"
#include "document/wn_document_internal.h"
#include "wn_internal.h"

#define FORMAT_NAME "wnui/6"

static const char *kind_name(int kind)
{
    switch (kind) {
    case R2_NODE_BLOCK: return "block";
    case R2_NODE_FLEX: return "flex";
    case R2_NODE_TEXT: return "text";
    case R2_NODE_GRID: return "grid";
    case R2_NODE_IMAGE: return "image";
    case R2_NODE_INPUT: return "input";
    default: return "unknown";
    }
}

static const char *justify_name(int value)
{
    switch (value) {
    case JUSTIFY_CENTER: return "center";
    case JUSTIFY_END: return "end";
    case JUSTIFY_SPACE_BETWEEN: return "space-between";
    default: return "start";
    }
}

static const char *align_name(int value)
{
    switch (value) {
    case ALIGN_CENTER: return "center";
    case ALIGN_END: return "end";
    case ALIGN_STRETCH: return "stretch";
    default: return "start";
    }
}

static const char *overflow_name(int value)
{
    switch (value) {
    case R2_OVERFLOW_HIDDEN: return "hidden";
    case R2_OVERFLOW_SCROLL: return "scroll";
    default: return "visible";
    }
}

static const char *object_fit_name(int value)
{
    switch (value) {
    case R2_OBJECT_FIT_COVER: return "cover";
    case R2_OBJECT_FIT_CONTAIN: return "contain";
    default: return "fill";
    }
}

static const char *text_align_name(int value)
{
    switch (value) {
    case 1: return "center";
    case 2: return "right";
    default: return "left";
    }
}

static const char *action_name(int value)
{
    return value == ACTION_SELECT ? "select" : "none";
}

static const char *grid_name(int value)
{
    return value == R2_GRID_AUTO_FILL ? "auto-fill" : "none";
}

static const char *direction_name(int value)
{
    return value == 1 ? "column" : "row";
}

static const char *wrap_name(int value)
{
    return value != 0 ? "wrap" : "nowrap";
}

static const char *box_sizing_name(int value)
{
    return value != 0 ? "border-box" : "content-box";
}

static const char *position_name(int value)
{
    switch (value) {
    case R2_POSITION_RELATIVE: return "relative";
    case R2_POSITION_ABSOLUTE: return "absolute";
    default: return "static";
    }
}

static const char *state_style_name(int value)
{
    switch (value) {
    case R2_STATE_STYLE_HOVER: return "hover";
    case R2_STATE_STYLE_PRESS: return "press";
    case R2_STATE_STYLE_FOCUS: return "focus";
    case R2_STATE_STYLE_DISABLED: return "disabled";
    default: return "unknown";
    }
}

static const UiStateStyle *state_style_at(const UiNode *node, int code)
{
    unsigned char i;
    for (i = 0; i < node->state_style_count; ++i) {
        if (node->state_styles[i].state == (unsigned char)code) {
            return &node->state_styles[i];
        }
    }
    return NULL;
}

/* Emitter style: four decimals, never "-0.0000". */
static const char *fmt_float(char *buffer, size_t size, float value)
{
    snprintf(buffer, size, "%.4f", (double)value);
    if (strcmp(buffer, "-0.0000") == 0) {
        snprintf(buffer, size, "0.0000");
    }
    return buffer;
}

static void quote_text(FILE *out, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)(value != NULL ? value : "");
    fputc('"', out);
    for (; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
        case '"': fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default:
            if (*cursor < 0x20) {
                fprintf(out, "\\u%04x", (unsigned)*cursor);
            } else {
                fputc((int)*cursor, out);
            }
            break;
        }
    }
    fputc('"', out);
}

static void text_string(FILE *out, const char *key, const char *value)
{
    fprintf(out, " %s=", key);
    quote_text(out, value);
}

static void text_int(FILE *out, const char *key, long long value)
{
    fprintf(out, " %s=%lld", key, value);
}

static void text_color(FILE *out, const char *key, unsigned int argb)
{
    fprintf(out, " %s=#%08x", key, argb);
}

static void text_float(FILE *out, const char *key, float value)
{
    char buffer[32];
    fprintf(out, " %s=%s", key, fmt_float(buffer, sizeof(buffer), value));
}

static void text_box4(FILE *out, const char *key, const float values[4])
{
    char a[32], b[32], c[32], d[32];
    fprintf(out, " %s=%s,%s,%s,%s", key,
            fmt_float(a, sizeof(a), values[0]),
            fmt_float(b, sizeof(b), values[1]),
            fmt_float(c, sizeof(c), values[2]),
            fmt_float(d, sizeof(d), values[3]));
}

static void text_state_styles(FILE *out, const UiNode *node)
{
    int code;
    int first = 1;
    fprintf(out, " states=");
    for (code = 0; code < R2_STATE_STYLE_COUNT; ++code) {
        const UiStateStyle *style = state_style_at(node, code);
        if (style == NULL) {
            continue;
        }
        if (!first) {
            fputc(',', out);
        }
        first = 0;
        fprintf(out, "%s(", state_style_name(code));
        if ((style->present & R2_STATE_FIELD_BACKGROUND) != 0u) {
            fprintf(out, "bg=#%08x", style->background);
        }
        if ((style->present & R2_STATE_FIELD_TEXT) != 0u) {
            fprintf(out, "%stext=#%08x", (style->present & R2_STATE_FIELD_BACKGROUND) != 0u ? "," : "",
                    style->text);
        }
        if ((style->present & R2_STATE_FIELD_BORDER) != 0u) {
            fprintf(out, "%sborder=#%08x",
                    (style->present & (R2_STATE_FIELD_BACKGROUND | R2_STATE_FIELD_TEXT)) != 0u ? "," : "",
                    style->border_color);
        }
        fputc(')', out);
    }
    if (first) {
        fputs("none", out);
    }
}

static void text_node(FILE *out, int index, const UiNode *node, const char *name)
{
    fprintf(out, "node %d", index);
    text_int(out, "parent", node->parent);
    fprintf(out, " kind=%s", kind_name(node->kind));
    text_string(out, "name", name);
    fprintf(out, " direction=%s", direction_name(node->direction));
    fprintf(out, " justify=%s", justify_name(node->justify));
    fprintf(out, " align=%s", align_name(node->align));
    fprintf(out, " wrap=%s", wrap_name(node->wrap));
    text_float(out, "width", node->width);
    text_float(out, "height", node->height);
    text_float(out, "width-pct", node->width_pct);
    text_float(out, "height-pct", node->height_pct);
    text_box4(out, "margin", node->margin);
    text_box4(out, "padding", node->padding);
    text_color(out, "background", node->background);
    text_color(out, "hover", node->hover_bg);
    text_color(out, "press", node->press_bg);
    text_color(out, "selected", node->selected_bg);
    fprintf(out, " action=%s", action_name(node->action));
    text_float(out, "gap", node->gap);
    text_float(out, "font-size", node->font_size);
    text_color(out, "color", node->text_color);
    fprintf(out, " grid=%s", grid_name(node->grid_mode));
    text_float(out, "grid-column", node->grid_column_px);
    text_float(out, "auto-rows", node->grid_auto_rows);
    text_float(out, "row-gap", node->row_gap);
    text_float(out, "column-gap", node->column_gap);
    fprintf(out, " overflow=%s", overflow_name(node->overflow));
    text_float(out, "image-width", node->image_width);
    text_float(out, "image-height", node->image_height);
    text_string(out, "src", node->image_src);
    text_string(out, "alt", node->image_alt);
    fprintf(out, " fit=%s", object_fit_name(node->object_fit));
    text_float(out, "border-width", node->border_width);
    text_color(out, "border-color", node->border_color);
    text_float(out, "radius", node->border_radius);
    text_float(out, "shadow-x", node->shadow_x);
    text_float(out, "shadow-y", node->shadow_y);
    text_float(out, "shadow-blur", node->shadow_blur);
    text_float(out, "shadow-spread", node->shadow_spread);
    text_color(out, "shadow-color", node->shadow_color);
    fprintf(out, " text-align=%s", text_align_name(node->text_align));
    text_float(out, "line-height", node->line_height);
    text_int(out, "margin-auto", node->margin_auto);
    fprintf(out, " box-sizing=%s", box_sizing_name(node->box_sizing));
    text_float(out, "grow", node->flex_grow);
    text_float(out, "shrink", node->flex_shrink);
    text_float(out, "basis", node->flex_basis);
    text_string(out, "family", node->font_family);
    text_int(out, "weight", node->font_weight);
    text_float(out, "opacity", node->opacity);
    fprintf(out, " position=%s", position_name(node->position));
    text_box4(out, "inset", node->inset);
    text_int(out, "inset-mask", (long long)node->inset_mask);
    fprintf(out, " z-index=");
    if (node->z_index == R2_Z_INDEX_AUTO) {
        fputs("auto", out);
    } else {
        fprintf(out, "%d", (int)node->z_index);
    }
    text_state_styles(out, node);
    text_string(out, "text", node->text);
    fputc('\n', out);
}

/* ---- JSON projection (stable key order) ------------------------------- */

static void json_string(FILE *out, const char *value)
{
    quote_text(out, value);
}

static void json_float(FILE *out, const char *key, float value)
{
    char buffer[32];
    fprintf(out, "      \"%s\": %s,\n", key, fmt_float(buffer, sizeof(buffer), value));
}

static void json_box4(FILE *out, const char *key, const float values[4])
{
    char a[32], b[32], c[32], d[32];
    fprintf(out, "      \"%s\": [%s, %s, %s, %s],\n", key,
            fmt_float(a, sizeof(a), values[0]),
            fmt_float(b, sizeof(b), values[1]),
            fmt_float(c, sizeof(c), values[2]),
            fmt_float(d, sizeof(d), values[3]));
}

static void json_color(FILE *out, const char *key, unsigned int argb)
{
    fprintf(out, "      \"%s\": \"#%08x\",\n", key, argb);
}

static void json_state_styles(FILE *out, const UiNode *node)
{
    int code;
    int first = 1;
    fprintf(out, "      \"state_styles\": [");
    for (code = 0; code < R2_STATE_STYLE_COUNT; ++code) {
        const UiStateStyle *style = state_style_at(node, code);
        if (style == NULL) {
            continue;
        }
        fprintf(out, "%s\n        { \"state\": \"%s\"",
                first ? "" : ",", state_style_name(code));
        first = 0;
        if ((style->present & R2_STATE_FIELD_BACKGROUND) != 0u) {
            fprintf(out, ", \"background\": \"#%08x\"", style->background);
        }
        if ((style->present & R2_STATE_FIELD_TEXT) != 0u) {
            fprintf(out, ", \"text\": \"#%08x\"", style->text);
        }
        if ((style->present & R2_STATE_FIELD_BORDER) != 0u) {
            fprintf(out, ", \"border\": \"#%08x\"", style->border_color);
        }
        fputc('}', out);
    }
    fprintf(out, "%s],\n", node->state_style_count > 0 ? "\n      " : "");
}

static void json_node(FILE *out, int index, const UiNode *node, const char *name)
{
    fprintf(out, "    {\n");
    fprintf(out, "      \"index\": %d,\n", index);
    fprintf(out, "      \"parent\": %d,\n", node->parent);
    fprintf(out, "      \"kind\": \"%s\",\n", kind_name(node->kind));
    fprintf(out, "      \"name\": ");
    json_string(out, name);
    fprintf(out, ",\n");
    fprintf(out, "      \"direction\": \"%s\",\n", direction_name(node->direction));
    fprintf(out, "      \"justify\": \"%s\",\n", justify_name(node->justify));
    fprintf(out, "      \"align\": \"%s\",\n", align_name(node->align));
    fprintf(out, "      \"wrap\": \"%s\",\n", wrap_name(node->wrap));
    json_float(out, "width", node->width);
    json_float(out, "height", node->height);
    json_float(out, "width_pct", node->width_pct);
    json_float(out, "height_pct", node->height_pct);
    json_box4(out, "margin", node->margin);
    json_box4(out, "padding", node->padding);
    json_color(out, "background", node->background);
    json_color(out, "hover", node->hover_bg);
    json_color(out, "press", node->press_bg);
    json_color(out, "selected", node->selected_bg);
    fprintf(out, "      \"action\": \"%s\",\n", action_name(node->action));
    json_float(out, "gap", node->gap);
    json_float(out, "font_size", node->font_size);
    json_color(out, "color", node->text_color);
    fprintf(out, "      \"grid\": \"%s\",\n", grid_name(node->grid_mode));
    json_float(out, "grid_column_px", node->grid_column_px);
    json_float(out, "grid_auto_rows", node->grid_auto_rows);
    json_float(out, "row_gap", node->row_gap);
    json_float(out, "column_gap", node->column_gap);
    fprintf(out, "      \"overflow\": \"%s\",\n", overflow_name(node->overflow));
    json_float(out, "image_width", node->image_width);
    json_float(out, "image_height", node->image_height);
    fprintf(out, "      \"image_src\": ");
    json_string(out, node->image_src);
    fprintf(out, ",\n      \"image_alt\": ");
    json_string(out, node->image_alt);
    fprintf(out, ",\n");
    fprintf(out, "      \"object_fit\": \"%s\",\n", object_fit_name(node->object_fit));
    json_float(out, "border_width", node->border_width);
    json_color(out, "border_color", node->border_color);
    json_float(out, "border_radius", node->border_radius);
    json_float(out, "shadow_x", node->shadow_x);
    json_float(out, "shadow_y", node->shadow_y);
    json_float(out, "shadow_blur", node->shadow_blur);
    json_float(out, "shadow_spread", node->shadow_spread);
    json_color(out, "shadow_color", node->shadow_color);
    fprintf(out, "      \"text_align\": \"%s\",\n", text_align_name(node->text_align));
    json_float(out, "line_height", node->line_height);
    fprintf(out, "      \"margin_auto\": %d,\n", node->margin_auto);
    fprintf(out, "      \"box_sizing\": \"%s\",\n", box_sizing_name(node->box_sizing));
    json_float(out, "flex_grow", node->flex_grow);
    json_float(out, "flex_shrink", node->flex_shrink);
    json_float(out, "flex_basis", node->flex_basis);
    fprintf(out, "      \"font_family\": ");
    json_string(out, node->font_family);
    fprintf(out, ",\n");
    fprintf(out, "      \"font_weight\": %d,\n", node->font_weight);
    json_float(out, "opacity", node->opacity);
    fprintf(out, "      \"position\": \"%s\",\n", position_name(node->position));
    json_box4(out, "inset", node->inset);
    fprintf(out, "      \"inset_mask\": %u,\n", node->inset_mask);
    if (node->z_index == R2_Z_INDEX_AUTO) {
        fprintf(out, "      \"z_index\": \"auto\",\n");
    } else {
        fprintf(out, "      \"z_index\": %d,\n", (int)node->z_index);
    }
    json_state_styles(out, node);
    fprintf(out, "      \"text\": ");
    json_string(out, node->text);
    fprintf(out, "\n    }");
}

static void usage(FILE *out, const char *program)
{
    fprintf(out,
            "usage: %s <file.wnui> [--json]\n"
            "       %s --help\n"
            "\n"
            "Loads a .wnui blob with the runtime loader and prints a debug\n"
            "projection. This output is NOT a contract; it may change with the\n"
            "blob version. Default text format: one line per node. --json: same\n"
            "data with stable key order.\n",
            program, program);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int json = 0;
    int i;
    wn_document *document;
    int index;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout, argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--json") == 0) {
            json = 1;
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "wnui-dump: unknown option: %s\n", argv[i]);
            usage(stderr, argv[0]);
            return 2;
        }
        if (path != NULL) {
            fprintf(stderr, "wnui-dump: only one input file is accepted\n");
            return 2;
        }
        path = argv[i];
    }
    if (path == NULL) {
        fprintf(stderr, "wnui-dump: missing input file\n");
        usage(stderr, argv[0]);
        return 2;
    }

    document = wn_document_load_blob(path);
    if (document == NULL) {
        const char *error = wn_last_error();
        fprintf(stderr, "wnui-dump: %s: %s\n", path,
                error != NULL ? error : "could not load the blob");
        return 1;
    }

    if (json) {
        printf("{\n");
        printf("  \"format\": \"" FORMAT_NAME "\",\n");
        printf("  \"viewport\": { \"width\": %d, \"height\": %d },\n",
               document->viewport_width, document->viewport_height);
        printf("  \"node_count\": %d,\n", document->node_count);
        printf("  \"input_count\": %d,\n", document->input_count);
        printf("  \"nodes\": [\n");
        for (index = 0; index < document->node_count; ++index) {
            json_node(stdout, index, &document->nodes[index], document->names[index]);
            printf("%s\n", index + 1 < document->node_count ? "," : "");
        }
        printf("  ],\n");
        printf("  \"inputs\": [");
        for (index = 0; index < document->input_count; ++index) {
            const UiInput *input = &document->inputs[index];
            printf("%s\n    {", index == 0 ? "" : ",");
            printf("\"node\": %d, \"value\": ", (int)input->node);
            json_string(stdout, input->value);
            printf(", \"placeholder\": ");
            json_string(stdout, input->placeholder);
            printf(", \"max_length\": %d, \"password\": %s }",
                   (int)input->max_length, input->password != 0 ? "true" : "false");
        }
        printf("%s]\n", document->input_count > 0 ? "\n  " : "");
        printf("}\n");
    } else {
        printf("format=" FORMAT_NAME " viewport=%dx%d nodes=%d inputs=%d\n",
               document->viewport_width, document->viewport_height,
               document->node_count, document->input_count);
        for (index = 0; index < document->node_count; ++index) {
            text_node(stdout, index, &document->nodes[index], document->names[index]);
        }
        for (index = 0; index < document->input_count; ++index) {
            const UiInput *input = &document->inputs[index];
            printf("input node=%d", (int)input->node);
            text_string(stdout, "value", input->value);
            text_string(stdout, "placeholder", input->placeholder);
            text_int(stdout, "max-length", input->max_length);
            text_int(stdout, "password", input->password);
            fputc('\n', stdout);
        }
    }

    wn_document_release(document);
    return 0;
}
