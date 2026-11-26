/*
 * ime_view_test.c - view-level IME regression for document replacement (v7).
 *
 * B1: wn_app_load_document() -> wn_view_replace_document() builds a new core.
 * The platform IME backend belongs to the view, so the replacement core must
 * receive it too; otherwise the documented SDK path loses IME after loading a
 * document. This test drives an offscreen view (no Win32 window) with a fake
 * backend and asserts focus/caret callbacks after the replacement.
 *
 *   ime_view_test.exe
 */
#include <stdio.h>
#include <string.h>

#include "text/text_runtime.h"
#include "wn_core.h"
#include "wn_view.h"
#include "wn_internal.h"

/* fake_text.c backend (same pattern as ime_test.c). */
const wn_text_ops *wn_text_fake(void);

#define NIHON "\xe3\x81\xab\xe3\x81\xbb\xe3\x82\x93"

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

static int g_failures;
static int g_checks;

static int check(const char *name, int ok, const char *detail)
{
    ++g_checks;
    printf("[%s] ime-view: %s - %s\n", ok ? "ok  " : "FAIL", name, detail);
    if (!ok) {
        ++g_failures;
    }
    return ok;
}

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

int main(void)
{
    FakeIme fake;
    wn_view *view;
    wn_node_id search;
    wn_document *document = (wn_document *)wn_document_builtin();
    int focus_before;
    int caret_before;
    int blur_before;
    wn_event event;

    memset(&fake, 0, sizeof(fake));
    wn_text_test_use_backend(wn_text_fake());
    view = wn_view_create_offscreen(480, 240);
    if (view == NULL) {
        printf("FAIL: wn_view_create_offscreen: %s\n",
               wn_last_error() != NULL ? wn_last_error() : "unknown");
        return 1;
    }
    wn_view_set_ime(view, &g_fake_ime_ops, &fake);
    search = wn_view_find(view, "body.root/div.bar/input#search.search");
    check("fixture input", search != WN_NODE_INVALID,
          "input#search found in the embedded fixture");

    /* Focus before the replacement: the old core owns the platform IME. */
    check("focus before replace", wn_view_focus_input(view, search) == WN_OK &&
                                     wn_view_input_focused(view) &&
                                     fake.focus_calls == 1,
          "old core focused and called the fake backend");
    check("caret before replace", fake.caret_calls >= 1 &&
                                     fake.caret_rect.w > 0.0f,
          "old core reported a caret rect");

    focus_before = fake.focus_calls;
    caret_before = fake.caret_calls;
    blur_before = fake.blur_calls;
    check("replace document",
          wn_view_replace_document(view, document) == 0,
          "replacement core built from the builtin document");
    {
        char detail[96];
        snprintf(detail, sizeof(detail), "blur calls %d -> %d",
                 blur_before, fake.blur_calls);
        check("old core blurred", fake.blur_calls == blur_before + 1, detail);
    }
    check("focus restored by name",
          wn_view_input_focused(view) && fake.focus_calls == focus_before + 1,
          "new core has the backend and re-focused input#search");
    check("caret restored on new core",
          fake.caret_calls > caret_before && fake.caret_rect.w > 0.0f,
          "set_caret_rect fired on the replacement core");

    /* Explicit focus after a replacement must also reach the new core. */
    check("blur after replace", wn_view_blur_input(view) == WN_OK &&
                                   !wn_view_input_focused(view),
          "focus released");
    focus_before = fake.focus_calls;
    caret_before = fake.caret_calls;
    search = wn_view_find(view, "body.root/div.bar/input#search.search");
    check("focus after replace",
          search != WN_NODE_INVALID && wn_view_focus_input(view, search) == WN_OK &&
              wn_view_input_focused(view),
          "new core accepts focus");
    check("new core focus callbacks",
          fake.focus_calls == focus_before + 1 && fake.caret_calls > caret_before,
          "fake focus/caret called by the new core");

    /* The new core is a real editor too: compose + commit through the view. */
    memset(&event, 0, sizeof(event));
    event.type = WN_EVENT_IME_START;
    check("start on new core", wn_view_event(view, &event) == WN_OK,
          "WN_EVENT_IME_START accepted");
    memset(&event, 0, sizeof(event));
    event.type = WN_EVENT_IME_COMPOSITION;
    event.target_id = NIHON;
    event.key = 3;
    check("composition on new core", wn_view_event(view, &event) == WN_OK,
          "WN_EVENT_IME_COMPOSITION accepted");
    memset(&event, 0, sizeof(event));
    event.type = WN_EVENT_IME_COMMIT;
    event.target_id = NIHON;
    check("commit on new core", wn_view_event(view, &event) == WN_OK,
          "WN_EVENT_IME_COMMIT accepted");
    {
        const char *value = wn_view_get_value(view, search);
        check("value on new core", value != NULL && strcmp(value, NIHON) == 0,
              "commit inserted once into the replacement core");
    }

    wn_view_destroy(view);
    printf("ime_view_test: %s (%d checks)\n",
           g_failures == 0 ? "PASS" : "FAIL", g_checks);
    return g_failures == 0 ? 0 : 1;
}
