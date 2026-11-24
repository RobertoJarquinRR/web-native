/*
 * input_core_test.c - headless acceptance for the single-line text input
 * (ui-tree/3 + ABI v4 core surface).
 *
 * Compiles against tests/input/fixture/ui_data.h and the core only (no GPU,
 * no window). Exercises focus/caret, typing, Backspace/Delete, arrows,
 * Home/End, Shift selection, copy/cut/paste, max_length, set_value/get_value,
 * Escape blur and input cloning.
 *
 *   input_core_test.exe out/core_events.jsonl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_data.h"
#include "wn_core.h"

static wn_core *g_core;
static int g_failures;
static int g_checks;

static int check(const char *name, int ok, const char *detail)
{
    ++g_checks;
    printf("[%s] core: %s - %s\n", ok ? "ok  " : "FAIL", name, detail);
    if (!ok) {
        ++g_failures;
    }
    return ok;
}

static wn_rect rect_of(int node)
{
    return wn_core_rects(g_core, NULL)[node];
}

static int type_text(int node, const char *text, int *changes)
{
    int i;
    for (i = 0; text[i] != '\0'; ++i) {
        int changed = 0;
        int visual = 0;
        if (wn_core_input_char(g_core, (uint32_t)(unsigned char)text[i],
                               &changed, &visual) != node) {
            return 0;
        }
        if (changed) {
            ++*changes;
        }
    }
    return 1;
}

static int press_key(int node, int key, uint32_t mods, int *changes)
{
    int changed = 0;
    int visual = 0;
    if (wn_core_input_key(g_core, key, mods, &changed, &visual) != node) {
        return 0;
    }
    if (changed) {
        ++*changes;
    }
    return 1;
}

static int expect_value(const char *label, int node, const char *want)
{
    const char *value = wn_core_input_value(g_core, node);
    int ok = value != NULL && strcmp(value, want) == 0;
    char detail[256];
    snprintf(detail, sizeof(detail), "\"%s\" == \"%s\"",
             value != NULL ? value : "(null)", want);
    return check(label, ok, detail);
}

static void write_state(FILE *f, const char *stage, int node, int changes)
{
    wn_input_state state;
    if (wn_core_input_state(g_core, node, &state) != 0) {
        return;
    }
    fprintf(f, "{\"stage\":\"%s\",\"value\":", stage);
    {
        const char *p = state.value;
        fputc('"', f);
        for (; *p != '\0'; ++p) {
            if (*p == '"' || *p == '\\') {
                fputc('\\', f);
            }
            fputc(*p, f);
        }
        fputc('"', f);
    }
    fprintf(f,
            ",\"cursor\":%d,\"anchor\":%d,\"focused\":%d,\"caret_x\":%.3f,"
            "\"selection_x0\":%.3f,\"selection_x1\":%.3f,\"scroll\":%.3f,"
            "\"value_changes\":%d}\n",
            state.cursor, state.anchor, state.focused, state.caret_x,
            state.selection_x0, state.selection_x1, state.scroll, changes);
}

int main(int argc, char **argv)
{
    const char *out_path = argc > 1 ? argv[1] : "core_events.jsonl";
    FILE *report;
    int bar;
    int search;
    int empty;
    int clone;
    int changes = 0;
    wn_rect search_rect;
    wn_rect empty_rect;
    wn_input_state state;
    char clipboard[256];
    int copied;

    g_core = wn_core_create(wn_document_builtin());
    if (g_core == NULL) {
        printf("FAIL: wn_core_create\n");
        return 1;
    }
    report = fopen(out_path, "wb");
    if (report == NULL) {
        printf("FAIL: cannot write %s\n", out_path);
        wn_core_destroy(g_core);
        return 1;
    }
    wn_core_resize(g_core, 480, 240);
    wn_core_relayout(g_core);
    wn_core_build_display_list(g_core);

    bar = wn_core_find_node(g_core, "div.bar");
    search = wn_core_find_node(g_core, "body.root/div.bar/input#search.search");
    empty = wn_core_find_node(g_core, "body.root/div.bar/input#empty.empty");
    check("lookup bar", bar >= 0, "div.bar found");
    check("lookup input#search", search >= 0, "search input found");
    check("lookup input#empty", empty >= 0, "second input found");
    check("input kind", wn_core_node_is_input(g_core, search) &&
                        wn_core_node_is_input(g_core, empty),
          "both nodes are editable inputs");
    check("non-input node", !wn_core_node_is_input(g_core, bar),
          "div.bar is not an input");

    search_rect = rect_of(search);
    empty_rect = rect_of(empty);
    check("input layout", search_rect.x == 16.0f && search_rect.y == 16.0f &&
                          search_rect.w == 320.0f && search_rect.h == 34.0f,
          "search 320x34 at 16,16 (CSS box)");
    check("second input layout", empty_rect.w == 220.0f && empty_rect.h == 28.0f &&
                                empty_rect.y == search_rect.y + 44.0f,
          "empty 220x28 below the search field");

    if (wn_core_input_state(g_core, search, &state) != 0) {
        check("initial state", 0, "wn_core_input_state failed");
    } else {
        check("initial value", state.value[0] == '\0', "empty initial value");
        check("initial focus", !state.focused && wn_core_input_focused(g_core) < 0,
              "no input owns the focus");
        check("initial max_length", state.max_length == 12, "maxlength=12");
        check("initial metrics", state.baseline > 0.0f &&
                                 state.line_height > state.baseline,
              "baseline/line height from the compiled pool");
        check("placeholder pixels",
              wn_core_display_list(g_core)->glyph_vertex_count > 0,
              "placeholder is painted from the ASCII pool");
    }
    write_state(report, "initial", search, changes);

    /* ------------------------------------------------- click focuses + caret */
    check("click hit",
          wn_core_pointer_down(g_core, search_rect.x + search_rect.w - 10.0f,
                               search_rect.y + search_rect.h * 0.5f) == search,
          "pointer down hits input#search");
    wn_core_pointer_up(g_core, search_rect.x + search_rect.w - 10.0f,
                       search_rect.y + search_rect.h * 0.5f);
    check("click focus", wn_core_input_focused(g_core) == search,
          "input owns the focus");
    if (wn_core_input_state(g_core, search, &state) == 0) {
        check("click caret", state.cursor == 0 && state.focused,
              "empty value places the caret at 0");
    }
    write_state(report, "click", search, changes);

    /* ------------------------------------------------------------- typing */
    check("type abc", type_text(search, "abc", &changes), "3 chars consumed");
    expect_value("value after abc", search, "abc");
    if (wn_core_input_state(g_core, search, &state) == 0) {
        check("caret after typing", state.cursor == 3, "caret at end");
        check("caret x", state.caret_x > state.content.x, "caret moved right");
    }
    write_state(report, "type-abc", search, changes);

    /* ------------------------------- click places the caret inside the text */
    press_key(search, WN_KEY_HOME, 0, &changes);
    press_key(search, WN_KEY_RIGHT, 0, &changes);
    if (wn_core_input_state(g_core, search, &state) == 0) {
        float x = state.caret_x + 6.0f; /* past the midpoint of 'b' */
        wn_core_pointer_down(g_core, x, search_rect.y + search_rect.h * 0.5f);
        wn_core_pointer_up(g_core, x, search_rect.y + search_rect.h * 0.5f);
        if (wn_core_input_state(g_core, search, &state) == 0) {
            check("click caret position", state.cursor == 2,
                  "click between b and c sets cursor=2");
        }
    }
    check("insert at click", type_text(search, "X", &changes), "X inserted");
    expect_value("value after click insert", search, "abXc");
    write_state(report, "click-insert", search, changes);

    /* ---------------------------------------------------- editing keys */
    press_key(search, WN_KEY_BACKSPACE, 0, &changes);
    expect_value("backspace", search, "abc");
    press_key(search, WN_KEY_HOME, 0, &changes);
    press_key(search, WN_KEY_DELETE, 0, &changes);
    expect_value("home + delete", search, "bc");
    press_key(search, WN_KEY_END, 0, &changes);
    check("end caret", wn_core_input_state(g_core, search, &state) == 0 &&
                       state.cursor == 2, "End moves the caret to len");
    write_state(report, "editing", search, changes);

    /* ------------------------------------------------------ selection */
    check("select all", wn_core_input_select_all(g_core, NULL) == search,
          "Ctrl+A equivalent");
    if (wn_core_input_state(g_core, search, &state) == 0) {
        check("selection bounds", state.anchor == 0 && state.cursor == 2 &&
                                  state.selection_x1 > state.selection_x0,
              "anchor 0, cursor len, non-empty selection quad");
    }
    copied = wn_core_input_copy(g_core, clipboard, (int)sizeof(clipboard));
    check("copy", copied == 2 && strcmp(clipboard, "bc") == 0,
          "selection copied to the caller buffer");
    check("cut", wn_core_input_cut(g_core, clipboard, (int)sizeof(clipboard),
                                   NULL, NULL) == 2,
          "cut copies the selection");
    expect_value("value after cut", search, "");
    check("paste", wn_core_input_paste(g_core, clipboard, NULL, NULL) == search,
          "paste into the focused input");
    expect_value("value after paste", search, "bc");
    write_state(report, "selection", search, changes);

    /* ------------------------------------------------------ max_length */
    check("set_value 12", wn_core_set_input_value(g_core, search, "0123456789ab") == 0,
          "12 chars = maxlength");
    expect_value("value after set_value", search, "0123456789ab");
    check("set_value too long",
          wn_core_set_input_value(g_core, search, "0123456789abc") == 2,
          "13 chars rejected with code 2");
    check("set_value non-ascii", wn_core_set_input_value(g_core, search, "a\xce\xa9") == 1,
          "non-ASCII rejected with code 1");
    expect_value("value preserved after rejects", search, "0123456789ab");
    wn_core_input_select_all(g_core, NULL);
    press_key(search, WN_KEY_DELETE, 0, &changes);
    check("typing clamps at maxlength", type_text(search, "0123456789abcd", &changes),
          "typing beyond maxlength is consumed");
    expect_value("value after clamp", search, "0123456789ab");
    if (wn_core_input_state(g_core, search, &state) == 0) {
        check("caret after clamp", state.cursor == 12, "caret stays at maxlength");
    }
    write_state(report, "max-length", search, changes);

    /* ------------------------------------------------ focus switching + Esc */
    check("select before leaving", wn_core_input_select_all(g_core, NULL) == search,
          "search has an active selection before the focus moves");
    wn_core_pointer_down(g_core, empty_rect.x + 20.0f,
                         empty_rect.y + empty_rect.h * 0.5f);
    wn_core_pointer_up(g_core, empty_rect.x + 20.0f,
                       empty_rect.y + empty_rect.h * 0.5f);
    check("second input focus", wn_core_input_focused(g_core) == empty,
          "clicking input#empty moves the focus");
    if (wn_core_input_state(g_core, search, &state) == 0) {
        check("old selection cleared", state.anchor < 0,
              "the unfocused input no longer shows a selection");
    }
    expect_value("first value survives blur", search, "0123456789ab");
    type_text(empty, "hi", &changes);
    expect_value("second input edits", empty, "hi");
    press_key(empty, WN_KEY_ESCAPE, 0, &changes);
    check("escape blur", wn_core_input_focused(g_core) < 0,
          "Esc releases the focus");
    write_state(report, "blur", empty, changes);

    /* ------------------------------------------------------------- clones */
    clone = wn_core_clone(g_core, search, bar);
    check("clone input", clone >= 0 && wn_core_node_is_input(g_core, clone),
          "cloning an input yields an editable input");
    if (clone >= 0) {
        expect_value("clone copies the value", clone, "0123456789ab");
        check("clone set_value", wn_core_set_input_value(g_core, clone, "zz") == 0,
              "clone has its own value buffer");
        expect_value("original unchanged", search, "0123456789ab");
        check("clone remove", wn_core_remove(g_core, clone) == 0, "clone removed");
    }

    /* ---------------------------------------------------------- report end */
    write_state(report, "final", search, changes);
    fclose(report);
    wn_core_destroy(g_core);

    printf("input_core_test: %s (%d checks)\n",
           g_failures == 0 ? "PASS" : "FAIL", g_checks);
    return g_failures == 0 ? 0 : 1;
}
