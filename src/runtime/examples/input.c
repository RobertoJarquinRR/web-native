/*
 * input.c - C ABI demo: single-line text input end to end.
 *
 *   input.exe [--frames N]
 *
 * The document is the compiled fixture in tests/input/fixture/ui_data.h.
 * The demo exercises the host-facing surface only:
 *   - wn_node_set_value / wn_node_get_value (WN_ERR_UNSUPPORTED for non-ASCII,
 *     WN_ERR_INVALID_ARGUMENT above maxlength),
 *   - WN_EVENT_INPUT delivered on every user-driven value change, with
 *     wn_node_get_value readable from inside the callback,
 *   - typing, Backspace, Home/Shift+Right selection, Ctrl+C/X/V clipboard and
 *     Esc blur through wn_view_event().
 *
 * The window auto-closes after N presented frames (default 8) so the script is
 * headless.
 */
#include "webnative.h"
#include "wn_app.h"
#include "wn_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

static int g_failures;
static wn_app *g_app;
static wn_view *g_view;
static wn_node_id g_search;
static int g_input_events;
static char g_last_event_value[256];

static const char *status_name(wn_status status)
{
    switch (status) {
    case WN_OK: return "WN_OK";
    case WN_ERR_INVALID_ARGUMENT: return "WN_ERR_INVALID_ARGUMENT";
    case WN_ERR_OUT_OF_MEMORY: return "WN_ERR_OUT_OF_MEMORY";
    case WN_ERR_IO: return "WN_ERR_IO";
    case WN_ERR_PARSE: return "WN_ERR_PARSE";
    case WN_ERR_UNSUPPORTED: return "WN_ERR_UNSUPPORTED";
    case WN_ERR_GPU: return "WN_ERR_GPU";
    case WN_ERR_STATE: return "WN_ERR_STATE";
    case WN_ERR_INTERNAL: return "WN_ERR_INTERNAL";
    default: return "WN_ERR_UNKNOWN";
    }
}

static int expect_status(const char *what, wn_status got, wn_status want)
{
    int ok = got == want;
    printf("input: %s -> %s (expected %s)%s%s\n", what, status_name(got),
           status_name(want), ok ? "" : ": ",
           ok ? "" : (wn_last_error() != NULL ? wn_last_error() : "?"));
    if (!ok) {
        ++g_failures;
    }
    return ok;
}

static int expect_value(const char *what, const char *want)
{
    const char *value = wn_node_get_value(g_app, g_search);
    int ok = value != NULL && strcmp(value, want) == 0;
    printf("input: %s = \"%s\" (expected \"%s\")%s\n", what,
           value != NULL ? value : "(null)", want, ok ? "" : " -> FAIL");
    if (!ok) {
        ++g_failures;
    }
    return ok;
}

static void on_event(void *userdata, const wn_event *event)
{
    (void)userdata;
    if (event->type == WN_EVENT_INPUT) {
        const char *value = wn_node_get_value(g_app, g_search);
        ++g_input_events;
        snprintf(g_last_event_value, sizeof(g_last_event_value), "%s",
                 value != NULL ? value : "");
        printf("input: event INPUT target=%s value=\"%s\"\n",
               event->target_id != NULL ? event->target_id : "(null)",
               g_last_event_value);
    }
}

static void inject_char(uint32_t codepoint)
{
    wn_event event;
    memset(&event, 0, sizeof(event));
    event.type = WN_EVENT_CHAR;
    event.codepoint = codepoint;
    (void)wn_view_event(g_view, &event);
}

static void inject_key(int key, uint32_t modifiers)
{
    wn_event event;
    memset(&event, 0, sizeof(event));
    event.type = WN_EVENT_KEY_DOWN;
    event.key = key;
    event.modifiers = modifiers;
    (void)wn_view_event(g_view, &event);
}

static void inject_click(float x, float y)
{
    wn_event event;
    memset(&event, 0, sizeof(event));
    event.type = WN_EVENT_POINTER_DOWN;
    event.x = x;
    event.y = y;
    (void)wn_view_event(g_view, &event);
    event.type = WN_EVENT_POINTER_UP;
    (void)wn_view_event(g_view, &event);
}

static int parse_int(const char *text, int fallback)
{
    char *end = NULL;
    long value;
    if (text == NULL) {
        return fallback;
    }
    value = strtol(text, &end, 10);
    if (end == text || value < 0 || value > 100000) {
        return fallback;
    }
    return (int)value;
}

int main(int argc, char **argv)
{
    int frames = 8;
    int i;
    wn_config config;
    wn_status status;
    wn_app_stats stats;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = parse_int(argv[++i], frames);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: input.exe [--frames N]\n");
            return 0;
        }
    }

    SetProcessDPIAware();
    printf("input: webnative %s ABI %u\n", wn_version(), wn_abi_version());
    if (wn_abi_version() != WN_ABI_VERSION) {
        printf("input: FAIL (expected ABI %u)\n", (unsigned)WN_ABI_VERSION);
        return 1;
    }

    memset(&config, 0, sizeof(config));
    config.title = "webnative text input (ABI v5)";
    config.width = 480;
    config.height = 240;
    config.flags = WN_FLAG_NONE;
    g_app = wn_app_create(&config);
    if (g_app == NULL) {
        fprintf(stderr, "input: wn_app_create failed: %s\n",
                wn_last_error() != NULL ? wn_last_error() : "(no detail)");
        return 1;
    }
    status = wn_app_load(g_app, "<!doctype html><html><body class=\"root\"></body></html>",
                         NULL, NULL);
    if (status != WN_OK) {
        fprintf(stderr, "input: wn_app_load failed: %s\n",
                wn_last_error() != NULL ? wn_last_error() : "(no detail)");
        wn_app_destroy(g_app);
        return 1;
    }
    g_search = wn_app_find(g_app, "body.root/div.bar/input#search.search");
    if (g_search == WN_NODE_INVALID) {
        fprintf(stderr, "input: input#search not found: %s\n",
                wn_last_error() != NULL ? wn_last_error() : "(no detail)");
        wn_app_destroy(g_app);
        return 1;
    }
    expect_value("initial value", "");
    expect_status("set_value(abc)", wn_node_set_value(g_app, g_search, "abc"), WN_OK);
    expect_value("after set_value", "abc");
    expect_status("set_value(Omega)", wn_node_set_value(g_app, g_search, "\xce\xa9"),
                  WN_ERR_UNSUPPORTED);
    expect_status("set_value(13 chars)",
                  wn_node_set_value(g_app, g_search, "0123456789abc"),
                  WN_ERR_INVALID_ARGUMENT);
    expect_value("value survived rejects", "abc");

    /* Value edits through user events: click focuses, then typing + Backspace. */
    g_view = wn_app_view(g_app);
    if (g_view == NULL) {
        printf("input: FAIL (wn_app_view returned NULL)\n");
        wn_app_destroy(g_app);
        return 1;
    }
    (void)wn_app_on_event(g_app, on_event, NULL);
    inject_click(16.0f + 160.0f, 16.0f + 17.0f);
    if (!wn_view_input_focused(g_view)) {
        printf("input: FAIL (click did not focus the input)\n");
        ++g_failures;
    }
    inject_char('d');
    inject_key(8, 0);          /* VK_BACK */
    if (g_input_events != 2 || strcmp(g_last_event_value, "abc") != 0) {
        printf("input: FAIL (expected 2 INPUT events, last value \"abc\", got %d \"%s\")\n",
               g_input_events, g_last_event_value);
        ++g_failures;
    }
    expect_value("after typing + backspace", "abc");

    /* Home, Shift+Right -> select 'a', Ctrl+C, End, Ctrl+V -> "abca". */
    inject_key(36, 0);                  /* VK_HOME */
    inject_key(39, 0x01u);              /* VK_RIGHT + Shift */
    inject_key(67, 0x02u);              /* Ctrl+C */
    inject_key(35, 0);                  /* VK_END */
    inject_key(86, 0x02u);              /* Ctrl+V */
    expect_value("clipboard copy + paste", "abca");

    /* Ctrl+A, Ctrl+X -> "", Ctrl+V -> "abca" (maxlength 12 still applies). */
    inject_key(65, 0x02u);              /* Ctrl+A */
    inject_key(88, 0x02u);              /* Ctrl+X */
    expect_value("cut all", "");
    inject_key(86, 0x02u);              /* Ctrl+V */
    expect_value("paste after cut", "abca");

    /* Esc releases the focus without clearing the value. */
    inject_key(27, 0);
    if (wn_view_input_focused(g_view)) {
        printf("input: FAIL (Esc did not blur the input)\n");
        ++g_failures;
    } else {
        printf("input: escape -> focused=0 value=\"%s\"\n",
               wn_node_get_value(g_app, g_search));
    }

    expect_status("set_value(final)", wn_node_set_value(g_app, g_search, "final"), WN_OK);
    expect_value("final value", "final");

    /* Esc policy: with an input focused the key must blur it and NOT close the
     * window. Post a real WM_KEYDOWN so host_win32 -> wn_app decides. */
    inject_click(16.0f + 160.0f, 16.0f + 17.0f);
    {
        HWND hwnd = FindWindowW(NULL, L"webnative text input (ABI v5)");
        if (hwnd != NULL) {
            PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
            printf("input: queued WM_KEYDOWN Esc with focus\n");
        } else {
            printf("input: FAIL (host window not found for the Esc policy)\n");
            ++g_failures;
        }
    }

    wn_app_set_max_frames(g_app, frames);
    {
        int rc = wn_app_run(g_app);
        memset(&stats, 0, sizeof(stats));
        (void)wn_app_get_stats(g_app, &stats);
        printf("input: frames=%d backend=%s\n", stats.frames, stats.backend);
        if (rc != 0) {
            printf("input: FAIL (wn_app_run rc=%d)\n", rc);
            ++g_failures;
        }
        if (stats.frames != frames) {
            printf("input: FAIL (expected %d frames, got %d)\n", frames, stats.frames);
            ++g_failures;
        }
        if (wn_view_input_focused(g_view)) {
            printf("input: FAIL (Esc with focus did not blur)\n");
            ++g_failures;
        } else {
            printf("input: esc policy -> window stays open, input blurred\n");
        }
    }

    printf("input: exit=0 frames=%d input_events=%d\n", frames, g_input_events);
    printf("input: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    wn_app_destroy(g_app);
    g_app = NULL;
    return g_failures == 0 ? 0 : 1;
}
