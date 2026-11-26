/*
 * ime_test.c - headless IME composition acceptance (ABI v7).
 *
 * Runs against the embedded input fixture and the core only (no GPU, no
 * window). A fake wn_ime_ops records the platform calls and a helper injects
 * the generic START/COMPOSITION/COMMIT/CANCEL events; text is shaped with the
 * deterministic fake text backend so the composition glyph/underline counts are
 * exact. No Windows IME is required.
 *
 *   ime_test.exe
 */
#include <stdio.h>
#include <string.h>

#include "text/text_runtime.h"
#include "wn_core.h"
#include "wn_internal.h"

/* fake_text.c backend (same pattern as the text contract suite). */
const wn_text_ops *wn_text_fake(void);

/* "にほん" as UTF-8 bytes, kept escaped so the test builds under any code page. */
#define NIHON "\xe3\x81\xab\xe3\x81\xbb\xe3\x82\x93"
#define CP_NI  0x306Bu
#define CP_HO  0x307Bu
#define CP_N   0x3093u

typedef struct {
    int focus_calls;
    int blur_calls;
    int cancel_calls;
    int caret_calls;
    int candidate_calls;
    wn_rect focus_rect;
    wn_rect caret_rect;
    wn_rect candidate_rect;
} FakeIme;

static wn_core *g_core;
static int g_failures;
static int g_checks;
static FakeIme g_fake;

static int check(const char *name, int ok, const char *detail)
{
    ++g_checks;
    printf("[%s] ime: %s - %s\n", ok ? "ok  " : "FAIL", name, detail);
    if (!ok) {
        ++g_failures;
    }
    return ok;
}

/* ------------------------------------------------------------- fake backend */

static int fake_focus(void *context, const wn_rect *caret_rect)
{
    FakeIme *ime = (FakeIme *)context;
    ime->focus_calls += 1;
    if (caret_rect != NULL) {
        ime->focus_rect = *caret_rect;
    }
    return 0;
}

static void fake_blur(void *context)
{
    FakeIme *ime = (FakeIme *)context;
    ime->blur_calls += 1;
}

static void fake_cancel(void *context)
{
    FakeIme *ime = (FakeIme *)context;
    ime->cancel_calls += 1;
}

static void fake_set_caret(void *context, const wn_rect *caret_rect)
{
    FakeIme *ime = (FakeIme *)context;
    ime->caret_calls += 1;
    if (caret_rect != NULL) {
        ime->caret_rect = *caret_rect;
    }
}

static void fake_set_candidate(void *context, const wn_rect *candidate_rect)
{
    FakeIme *ime = (FakeIme *)context;
    ime->candidate_calls += 1;
    if (candidate_rect != NULL) {
        ime->candidate_rect = *candidate_rect;
    }
}

static const wn_ime_ops g_fake_ime_ops = {
    "fake-ime",
    fake_focus,
    fake_blur,
    fake_cancel,
    fake_set_caret,
    fake_set_candidate
};

static int ime_event(int type, const char *text, int caret, int *value_changed,
                     int *visual_changed)
{
    return wn_core_input_ime(g_core, type, text, caret, value_changed,
                             visual_changed);
}

/* --------------------------------------------------------------- helpers */

/* ASCII-only rendering of a value so the suite log stays decodable on any
 * console code page (the fixture text itself is UTF-8). */
static void escape_ascii(const char *text, char *out, int capacity)
{
    const unsigned char *p = (const unsigned char *)(text != NULL ? text : "");
    int used = 0;
    if (out == NULL || capacity <= 0) {
        return;
    }
    while (*p != '\0') {
        if (*p >= 0x20u && *p < 0x7Fu && *p != (unsigned char)'\\') {
            if (used + 1 >= capacity) {
                break;
            }
            out[used++] = (char)*p;
        } else {
            int written;
            if (used + 4 >= capacity) {
                break;
            }
            written = snprintf(out + used, (size_t)(capacity - used), "\\x%02X",
                               (unsigned int)*p);
            if (written <= 0) {
                break;
            }
            used += written;
        }
        ++p;
    }
    out[used] = '\0';
}

static int expect_value(const char *label, int node, const char *want)
{
    const char *value = wn_core_input_value(g_core, node);
    int ok = value != NULL && strcmp(value, want) == 0;
    char got[256];
    char expected[256];
    char detail[600];
    escape_ascii(value != NULL ? value : "(null)", got, (int)sizeof(got));
    escape_ascii(want, expected, (int)sizeof(expected));
    snprintf(detail, sizeof(detail), "\"%s\" == \"%s\"", got, expected);
    return check(label, ok, detail);
}

static int composition_is(const char *label, const char *want, int caret)
{
    const char *text = NULL;
    int actual_caret = -1;
    int active = wn_core_input_composition(g_core, &text, &actual_caret);
    char escaped[256];
    char detail[400];
    escape_ascii(text != NULL ? text : "(null)", escaped, (int)sizeof(escaped));
    snprintf(detail, sizeof(detail), "active=%d text=\"%s\" caret=%d",
             active, escaped, actual_caret);
    return check(label, active && text != NULL && strcmp(text, want) == 0 &&
                        actual_caret == caret, detail);
}

static int composition_inactive(const char *label)
{
    const char *text = NULL;
    int caret = -1;
    int active = wn_core_input_composition(g_core, &text, &caret);
    return check(label, active == 0, "no composition is active");
}

static void type_codepoint(uint32_t cp)
{
    int value_changed = 0;
    int visual_changed = 0;
    (void)wn_core_input_char(g_core, cp, &value_changed, &visual_changed);
}

int main(void)
{
    int search;
    int empty;
    int bar;
    int value_changed = 0;
    int visual_changed = 0;
    int node;
    int glyphs_before;
    int glyphs_after;
    int solids_before;
    int solids_after;
    const wn_display_list *list;

    wn_text_test_use_backend(wn_text_fake());
    g_core = wn_core_create(wn_document_builtin());
    if (g_core == NULL) {
        printf("FAIL: wn_core_create\n");
        return 1;
    }
    wn_core_resize(g_core, 480, 240);
    wn_core_relayout(g_core);
    wn_core_build_display_list(g_core);

    bar = wn_core_find_node(g_core, "div.bar");
    search = wn_core_find_node(g_core, "body.root/div.bar/input#search.search");
    empty = wn_core_find_node(g_core, "body.root/div.bar/input#empty.empty");
    check("fixture nodes", bar >= 0 && search >= 0 && empty >= 0,
          "bar + two inputs found");

    check("install backend", (wn_core_set_ime(g_core, &g_fake_ime_ops, &g_fake),
                              wn_core_input_composition(g_core, NULL, NULL) == 0),
          "fake IME vtable installed, no composition");

    /* ------------------------------------------------- focus/blur vtable */
    check("value before IME", wn_core_set_input_value(g_core, search, "ab") == 0,
          "value is ab");
    check("focus input", wn_core_focus_input_at_end(g_core, search) == search,
          "search owns the focus");
    check("focus callback", g_fake.focus_calls == 1, "platform focus called once");
    check("focus caret rect", g_fake.focus_rect.w > 0.0f &&
                              g_fake.focus_rect.h > 0.0f &&
                              g_fake.focus_rect.x > 0.0f,
          "focus carries a non-empty caret rect");
    check("caret callback", g_fake.caret_calls >= 1 && g_fake.candidate_calls >= 1,
          "caret and candidate rects reported");

    /* The platform caret must follow caret moves and typing, not only focus. */
    {
        float end_x = g_fake.caret_rect.x;
        float home_x;
        check("caret rect follows Home",
              wn_core_input_key(g_core, WN_KEY_HOME, 0u, NULL, NULL) == search &&
                  g_fake.caret_rect.x < end_x,
              "set_caret_rect moved left with the caret");
        home_x = g_fake.caret_rect.x;
        type_codepoint((uint32_t)'x');
        check("caret rect follows typing", g_fake.caret_rect.x > home_x,
              "set_caret_rect moved right after an insertion");
        check("reset value after caret probe",
              wn_core_set_input_value(g_core, search, "ab") == 0 &&
                  wn_core_focus_input_at_end(g_core, search) == search,
              "value/focus restored for the composition cases");
    }

    wn_core_build_display_list(g_core);
    list = wn_core_display_list(g_core);
    glyphs_before = list->glyph_vertex_count;
    solids_before = list->vertex_count;

    /* ------------------------------------------- START + COMPOSITION */
    check("start", ime_event(WN_EVENT_IME_START, NULL, 0, &value_changed,
                             &visual_changed) == search,
          "START targets the focused input");
    check("start visual", visual_changed && !value_changed,
          "START only changes the visual state");
    composition_is("start empty", "", 0);

    check("composition", ime_event(WN_EVENT_IME_COMPOSITION, NIHON, 2,
                                   &value_changed, &visual_changed) == search,
          "COMPOSITION targets the focused input");
    check("composition visual", visual_changed && !value_changed,
          "COMPOSITION does not touch the value");
    composition_is("composition text", NIHON, 2);
    expect_value("value while composing", search, "ab");
    type_codepoint((uint32_t)'q');
    expect_value("char suppressed while composing", search, "ab");
    composition_is("composition survives char", NIHON, 2);

    check("composition caret clamped",
          ime_event(WN_EVENT_IME_COMPOSITION, NIHON, 99, NULL, NULL) == search,
          "out-of-range caret accepted");
    composition_is("caret clamped to length", NIHON, 3);

    wn_core_build_display_list(g_core);
    list = wn_core_display_list(g_core);
    glyphs_after = list->glyph_vertex_count;
    solids_after = list->vertex_count;
    check("composition painted", glyphs_after == glyphs_before + 3 * 6,
          "three composition glyph quads above the value");
    check("composition underline", solids_after >= solids_before + 6,
          "one underline quad added");

    /* ------------------------------------- keys are suppressed while composing */
    check("backspace suppressed",
          wn_core_input_key(g_core, WN_KEY_BACKSPACE, 0u, &value_changed,
                            &visual_changed) == search &&
              !value_changed,
          "editing key consumed without an edit");
    expect_value("value after suppressed key", search, "ab");
    composition_is("composition after key", NIHON, 3);
    check("escape cancels composition",
          wn_core_input_key(g_core, WN_KEY_ESCAPE, 0u, &value_changed,
                            &visual_changed) == search,
          "Esc is consumed by the composition");
    composition_inactive("escape cleared composition");
    check("escape keeps focus", wn_core_input_focused(g_core) == search,
          "focus is not released");
    check("escape notifies platform cancel", g_fake.cancel_calls == 1,
          "wn_ime_ops.cancel called once");
    expect_value("value after escape", search, "ab");

    /* -------------------------------------------------- explicit CANCEL */
    ime_event(WN_EVENT_IME_START, NULL, 0, NULL, NULL);
    ime_event(WN_EVENT_IME_COMPOSITION, NIHON, 1, NULL, NULL);
    check("cancel", ime_event(WN_EVENT_IME_CANCEL, NULL, 0, &value_changed,
                              &visual_changed) == search,
          "CANCEL targets the focused input");
    check("cancel visual", visual_changed && !value_changed,
          "CANCEL leaves the value unchanged");
    composition_inactive("cancel cleared composition");
    expect_value("value after cancel", search, "ab");

    /* ---------------------------------------------------- commit once */
    ime_event(WN_EVENT_IME_START, NULL, 0, NULL, NULL);
    ime_event(WN_EVENT_IME_COMPOSITION, NIHON, 3, NULL, NULL);
    check("commit", ime_event(WN_EVENT_IME_COMMIT, NIHON, 0, &value_changed,
                              &visual_changed) == search,
          "COMMIT targets the focused input");
    check("commit changed once", value_changed && visual_changed,
          "commit reports a single value change");
    expect_value("value after commit", search, "ab" NIHON);
    composition_inactive("commit cleared composition");

    /* The host may still deliver WM_CHAR for the final text: the core drops
     * exactly the just-committed codepoints. */
    type_codepoint(CP_NI);
    type_codepoint(CP_HO);
    type_codepoint(CP_N);
    expect_value("no double insertion", search, "ab" NIHON);
    type_codepoint((uint32_t)'Z');
    expect_value("typing resumes after commit", search, "ab" NIHON "Z");

    /* --------------------------- stale and empty commits never insert text */
    check("stale commit ignored",
          ime_event(WN_EVENT_IME_COMMIT, "ZZ", 0, &value_changed,
                    &visual_changed) == search &&
              !value_changed,
          "COMMIT without an active composition is a no-op");
    expect_value("stale commit leaves value", search, "ab" NIHON "Z");
    ime_event(WN_EVENT_IME_START, NULL, 0, NULL, NULL);
    check("empty commit ignored",
          ime_event(WN_EVENT_IME_COMMIT, "", 0, &value_changed,
                    &visual_changed) == search &&
              !value_changed,
          "empty COMMIT clears the preedit without inserting");
    composition_inactive("empty commit cleared composition");
    expect_value("empty commit leaves value", search, "ab" NIHON "Z");

    /* ------------------------------------------- commit replaces selection */
    check("reset for selection", wn_core_set_input_value(g_core, search, "abcd") == 0,
          "value reset to abcd");
    check("select all", wn_core_input_select_all(g_core, NULL) == search,
          "selection covers the value");
    ime_event(WN_EVENT_IME_START, NULL, 0, NULL, NULL);
    ime_event(WN_EVENT_IME_COMPOSITION, "\xe3\x81\x82", 1, NULL, NULL); /* あ */
    ime_event(WN_EVENT_IME_COMMIT, "\xe3\x81\x82", 0, NULL, NULL);
    expect_value("commit replaces selection", search, "\xe3\x81\x82");

    /* --------------------------------------------- disabled input rejects */
    wn_core_force_state_mask(g_core, search, WN_STATE_DISABLED);
    check("disabled start rejected",
          ime_event(WN_EVENT_IME_START, NULL, 0, &value_changed,
                    &visual_changed) == -1,
          "START returns -1 on a disabled input");
    check("disabled composition rejected",
          ime_event(WN_EVENT_IME_COMPOSITION, NIHON, 0, NULL, NULL) == -1,
          "COMPOSITION returns -1 on a disabled input");
    composition_inactive("disabled leaves no composition");
    expect_value("disabled value unchanged", search, "\xe3\x81\x82");
    /* force_state_mask ORs, so clear the bit explicitly. */
    wn_core_set_state(g_core, search, WN_STATE_DISABLED, 0);

    /* --------------------------- the post-commit swallow dies with the focus */
    check("reset search before swallow probe",
          wn_core_set_input_value(g_core, search, "ab") == 0 &&
              wn_core_focus_input_at_end(g_core, search) == search,
          "search focused with value ab");
    ime_event(WN_EVENT_IME_START, NULL, 0, NULL, NULL);
    ime_event(WN_EVENT_IME_COMPOSITION, NIHON, 3, NULL, NULL);
    ime_event(WN_EVENT_IME_COMMIT, NIHON, 0, NULL, NULL);
    expect_value("commit before focus switch", search, "ab" NIHON);
    check("focus other input clears swallow",
          wn_core_focus_input_at_end(g_core, empty) == empty,
          "focus moved to input#empty");
    check("focus search again",
          wn_core_focus_input_at_end(g_core, search) == search,
          "focus moved back to input#search");
    type_codepoint(CP_NI);
    expect_value("swallow cleared on focus change", search,
                 "ab" NIHON "\xe3\x81\xab");

    /* ------------------------------------- commit truncates at max_length */
    check("focus empty for truncation",
          wn_core_set_input_value(g_core, empty, "abc") == 0 &&
              wn_core_focus_input_at_end(g_core, empty) == empty,
          "input#empty (maxlength=5) owns the focus");
    ime_event(WN_EVENT_IME_START, NULL, 0, NULL, NULL);
    ime_event(WN_EVENT_IME_COMPOSITION, "1234567", 7, NULL, NULL);
    check("commit truncates at max_length",
          ime_event(WN_EVENT_IME_COMMIT, "1234567", 0, &value_changed,
                    &visual_changed) == empty &&
              value_changed,
          "COMMIT inserts only the bytes that fit");
    expect_value("value truncated to max_length", empty, "abc12");
    composition_inactive("truncated commit cleared composition");

    /* ------------------------------------------------- focus switch + blur */
    check("focus second input",
          wn_core_focus_input_at_end(g_core, empty) == empty,
          "focus moves to input#empty");
    check("focus switched", wn_core_input_focused(g_core) == empty,
          "second input owns the focus");
    check("blur callback", g_fake.blur_calls >= 1,
          "platform blur called on the focus switch");
    node = wn_core_blur_input(g_core);
    check("blur", node == 0 && wn_core_input_focused(g_core) < 0,
          "blur releases the editor");
    check("blur callback after release", g_fake.blur_calls >= 2,
          "platform blur called on blur");

    wn_core_set_ime(g_core, NULL, NULL);
    wn_core_destroy(g_core);
    printf("ime_test: %s (%d checks)\n",
           g_failures == 0 ? "PASS" : "FAIL", g_checks);
    return g_failures == 0 ? 0 : 1;
}
