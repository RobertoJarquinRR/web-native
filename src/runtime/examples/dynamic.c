/*
 * dynamic.c - C ABI v3 demo: host-driven content reactivity.
 *
 *   dynamic.exe [--frames N] [--image-b PATH]
 *
 * The document is the compiled fixture in tests/dynamic/fixture/ui_data.h,
 * emitted by the `dynamic` test suite. The demo hides/shows a card, clones
 * the card template, swaps its image and text, re-parents and removes it, then
 * checks the error surface: set_image on a text node is WN_ERR_UNSUPPORTED and
 * the document root cannot be removed.
 *
 * The window auto-closes after N presented frames (default 8) so the script is
 * headless.
 */
#include "webnative.h"
#include "wn_app.h"
#include "demo_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static wn_node_id g_event_node;
static char g_event_target[256];
static int g_event_has_target;

static void on_event(void *userdata, const wn_event *event)
{
    (void)userdata;
    g_event_node = event->node;
    g_event_has_target = event->target_id != NULL;
    g_event_target[0] = '\0';
    if (event->target_id != NULL) {
        snprintf(g_event_target, sizeof(g_event_target), "%s", event->target_id);
    }
}

static void inject_click(wn_view *view, float x, float y)
{
    wn_event event;
    memset(&event, 0, sizeof(event));
    event.type = WN_EVENT_POINTER_DOWN;
    event.x = x;
    event.y = y;
    (void)wn_view_event(view, &event);
    event.type = WN_EVENT_POINTER_UP;
    (void)wn_view_event(view, &event);
}

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
    printf("dynamic: %s -> %s (expected %s)%s%s\n", what, status_name(got),
           status_name(want), ok ? "" : ": ",
           ok ? "" : (wn_last_error() != NULL ? wn_last_error() : "?"));
    if (!ok) {
        ++g_failures;
    }
    return ok;
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
    const char *image_b = "images/b.png";
    int i;
    int rc;
    wn_config config;
    wn_app *app;
    wn_status status;
    wn_app_stats stats;
    wn_node_id root;
    wn_node_id cards;
    wn_node_id card0;
    wn_node_id image0;
    wn_node_id text0;
    wn_node_id clone;
    int32_t count_initial;
    int32_t count_cloned;
    int32_t count_after_remove;
    int32_t count_after_clear;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = parse_int(argv[++i], frames);
        } else if (strcmp(argv[i], "--image-b") == 0 && i + 1 < argc) {
            image_b = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: dynamic.exe [--frames N] [--image-b PATH]\n");
            return 0;
        }
    }

    SetProcessDPIAware();
    printf("dynamic: webnative %s ABI %u\n", wn_version(), wn_abi_version());

    memset(&config, 0, sizeof(config));
    config.title = "webnative dynamic UI (ABI v5)";
    config.width = 400;
    config.height = 300;
    config.flags = WN_FLAG_NONE;
    app = wn_app_create(&config);
    if (app == NULL) {
        fprintf(stderr, "dynamic: wn_app_create failed: %s\n",
                wn_last_error() != NULL ? wn_last_error() : "(no detail)");
        return 1;
    }
    /* v0 attaches the compiled document; the HTML/CSS strings are retained only
     * as diagnostics, so a marker string is enough for this demo. */
    status = wn_app_load(app, "<!doctype html><html><body class=\"root\"></body></html>",
                         NULL, NULL);
    if (status != WN_OK) {
        fprintf(stderr, "dynamic: wn_app_load failed: %s\n",
                wn_last_error() != NULL ? wn_last_error() : "(no detail)");
        wn_app_destroy(app);
        return 1;
    }

    root = wn_app_root(app);
    cards = wn_app_find(app, "body.root/div.cards");
    card0 = wn_app_find(app, "body.root/div.cards/div.card");
    image0 = wn_app_find(app, "body.root/div.cards/div.card/img.thumb");
    text0 = wn_app_find(app, "body.root/div.cards/div.card/div.label/text[1]");
    count_initial = wn_app_node_count(app);
    printf("dynamic: root=%llu cards=%llu card=%llu image=%llu text=%llu nodes=%d\n",
           (unsigned long long)root, (unsigned long long)cards,
           (unsigned long long)card0, (unsigned long long)image0,
           (unsigned long long)text0, (int)count_initial);
    if (root == WN_NODE_INVALID || cards == WN_NODE_INVALID ||
        card0 == WN_NODE_INVALID || image0 == WN_NODE_INVALID ||
        text0 == WN_NODE_INVALID || count_initial != 17) {
        printf("dynamic: FAIL (lookup)\n");
        wn_app_destroy(app);
        return 1;
    }

    expect_status("set_visible(card,0)", wn_node_set_visible(app, card0, 0), WN_OK);
    expect_status("set_visible(card,1)", wn_node_set_visible(app, card0, 1), WN_OK);

    /* A1: a handle from before a removal must not address the slot's next
     * tenant. wn_node_remove() pushes the freed slots onto a LIFO free list, so
     * the next clone lands in the same indices with a bumped generation. */
    {
        wn_node_id stale = wn_node_clone(app, card0, cards);
        wn_node_id stale_child = wn_node_first_child(app, stale);
        wn_node_id fresh;
        int stale_ok = stale != WN_NODE_INVALID && stale_child != WN_NODE_INVALID;

        expect_status("remove(stale)", wn_node_remove(app, stale), WN_OK);
        fresh = wn_node_clone(app, card0, cards);
        stale_ok = stale_ok && fresh != WN_NODE_INVALID && fresh != stale &&
                   ((stale ^ fresh) >> 32) != 0;        stale_ok = stale_ok &&
            wn_node_set_visible(app, stale, 0) == WN_ERR_INVALID_ARGUMENT &&
            wn_node_set_text(app, stale, "Z") == WN_ERR_INVALID_ARGUMENT &&
            wn_node_set_image(app, stale, image_b) == WN_ERR_INVALID_ARGUMENT &&
            wn_node_set_value(app, stale, "z") == WN_ERR_INVALID_ARGUMENT &&
            wn_node_get_value(app, stale) == NULL &&
            wn_node_remove(app, stale) == WN_ERR_INVALID_ARGUMENT &&
            wn_node_clear_children(app, stale) == WN_ERR_INVALID_ARGUMENT &&
            wn_node_clone(app, stale, cards) == WN_NODE_INVALID &&
            wn_node_clone(app, card0, stale) == WN_NODE_INVALID &&
            wn_node_append(app, cards, stale) == WN_ERR_INVALID_ARGUMENT &&
            wn_node_parent(app, stale) == WN_NODE_INVALID &&
            wn_node_first_child(app, stale) == WN_NODE_INVALID &&
            wn_node_next_sibling(app, stale) == WN_NODE_INVALID &&
            wn_node_parent(app, stale_child) == WN_NODE_INVALID &&
            wn_node_next_sibling(app, stale_child) == WN_NODE_INVALID &&
            wn_node_parent(app, fresh) == cards &&
            wn_node_first_child(app, fresh) != WN_NODE_INVALID &&
            wn_node_set_visible(app, fresh, 1) == WN_OK;

        /* The reuse slot is the third card (x=270): events must report the new
         * handle, never the stale one. */
        (void)wn_app_on_event(app, on_event, NULL);
        inject_click(wn_app_view(app), 270.0f + 60.0f, 50.0f + 60.0f);
        stale_ok = stale_ok && g_event_node == fresh;
        printf("dynamic: stale handle old=%llu new=%llu gen=%u -> %s\n",
               (unsigned long long)stale, (unsigned long long)fresh,
               (unsigned)((fresh >> 32) - (stale >> 32)),
               stale_ok ? "rejected" : "FAIL");
        if (!stale_ok) {
            ++g_failures;
        }
        expect_status("remove(fresh)", wn_node_remove(app, fresh), WN_OK);
    }

    clone = wn_node_clone(app, card0, cards);
    count_cloned = wn_app_node_count(app);
    printf("dynamic: clone(card,cards)=%llu nodes=%d\n",
           (unsigned long long)clone, (int)count_cloned);
    if (clone == WN_NODE_INVALID || count_cloned != count_initial + 4) {
        ++g_failures;
        printf("dynamic: FAIL (clone)\n");
    }

    expect_status("set_image(image,b)", wn_node_set_image(app, image0, image_b), WN_OK);
    expect_status("set_text(text,Beta)", wn_node_set_text(app, text0, "Beta"), WN_OK);
    status = wn_node_set_text(app, text0, "\xce\xa9");
    printf("dynamic: set_text(Omega) -> %s\n",
           status == WN_OK ? "WN_OK" : "unexpected");
    if (status != WN_OK) {
        ++g_failures;
    }

    expect_status("append(cards,clone)", wn_node_append(app, cards, clone), WN_OK);
    expect_status("append cycle", wn_node_append(app, clone, cards), WN_ERR_INVALID_ARGUMENT);

    /* ABI v5: walk the clone and identify clicks by handle. The clone sits in
     * the third card slot (x=270) of the 400x300 fixture. */
    {
        wn_node_id image = wn_node_first_child(app, clone);
        wn_node_id label = wn_node_next_sibling(app, image);
        wn_node_id text = wn_node_first_child(app, label);
        int chain_ok = image != WN_NODE_INVALID && label != WN_NODE_INVALID &&
                       text != WN_NODE_INVALID &&
                       wn_node_parent(app, image) == clone &&
                       wn_node_parent(app, label) == clone &&
                       wn_node_next_sibling(app, image) == label &&
                       wn_node_next_sibling(app, label) == WN_NODE_INVALID &&
                       wn_node_first_child(app, text) == WN_NODE_INVALID;
        int parent_ok = wn_node_parent(app, clone) == cards;
        int invalid_ok = wn_node_parent(app, WN_NODE_INVALID) == WN_NODE_INVALID &&
                         wn_node_first_child(app, WN_NODE_INVALID) == WN_NODE_INVALID &&
                         wn_node_next_sibling(app, WN_NODE_INVALID) == WN_NODE_INVALID;
        printf("dynamic: nav clone=%llu parent=%llu image=%llu label=%llu text=%llu"
               " parent_ok=%d chain_ok=%d invalid_ok=%d\n",
               (unsigned long long)clone,
               (unsigned long long)wn_node_parent(app, clone),
               (unsigned long long)image, (unsigned long long)label,
               (unsigned long long)text, parent_ok, chain_ok, invalid_ok);
        if (!parent_ok || !chain_ok || !invalid_ok) {
            ++g_failures;
            printf("dynamic: FAIL (clone navigation)\n");
        }

        (void)wn_app_on_event(app, on_event, NULL);
        inject_click(wn_app_view(app), 270.0f + 60.0f, 50.0f + 60.0f);
        printf("dynamic: click clone -> node=%llu target=%s\n",
               (unsigned long long)g_event_node,
               g_event_has_target ? g_event_target : "(null)");
        if (g_event_node != clone || g_event_has_target) {
            ++g_failures;
            printf("dynamic: FAIL (clone click identity)\n");
        }
        inject_click(wn_app_view(app), 10.0f + 60.0f, 50.0f + 60.0f);
        printf("dynamic: click card -> node=%llu target=%s\n",
               (unsigned long long)g_event_node,
               g_event_has_target ? g_event_target : "(null)");
        if (g_event_node != card0 || !g_event_has_target ||
            strcmp(g_event_target, "body.root/div.cards/div.card") != 0) {
            ++g_failures;
            printf("dynamic: FAIL (compiled click identity)\n");
        }
    }

    expect_status("remove(clone)", wn_node_remove(app, clone), WN_OK);
    count_after_remove = wn_app_node_count(app);
    if (count_after_remove != count_initial) {
        ++g_failures;
        printf("dynamic: FAIL (remove count %d)\n", (int)count_after_remove);
    }

    /* A4: materialise a large list under the body container. The arena grows
     * from its initial capacity; no rendering happens here. */
    {
        enum { kStressClones = 1500 };
        wn_node_id body = wn_app_find(app, "body.root");
        wn_node_id *clones =
            (wn_node_id *)malloc(sizeof(wn_node_id) * (size_t)kStressClones);
        int created = 0;
        int alive = 0;
        int index;
        int ok;

        for (index = 0; clones != NULL && index < kStressClones; ++index) {
            wn_node_id node = wn_node_clone(app, card0, body);
            if (node == WN_NODE_INVALID) {
                break;
            }
            clones[created++] = node;
        }
        for (index = 0; index < created; ++index) {
            if (wn_node_parent(app, clones[index]) == body &&
                wn_node_first_child(app, clones[index]) != WN_NODE_INVALID) {
                ++alive;
            }
        }
        ok = clones != NULL && body != WN_NODE_INVALID &&
             created == kStressClones && alive == created &&
             wn_app_node_count(app) == count_initial + 4 * kStressClones &&
             wn_node_set_visible(app, clones[created - 1], 1) == WN_OK;
        printf("dynamic: growth clones=%d alive=%d nodes=%d -> %s\n",
               created, alive, (int)wn_app_node_count(app),
               ok ? "grown" : "FAIL");
        if (!ok) {
            ++g_failures;
        }
        for (index = 0; index < created; ++index) {
            (void)wn_node_remove(app, clones[index]);
        }
        free(clones);
        ok = wn_app_node_count(app) == count_initial;
        printf("dynamic: growth removed nodes=%d -> %s\n",
               (int)wn_app_node_count(app), ok ? "restored" : "FAIL");
        if (!ok) {
            ++g_failures;
        }
    }

    expect_status("set_image(text)", wn_node_set_image(app, text0, image_b), WN_ERR_UNSUPPORTED);
    expect_status("set_visible(invalid)", wn_node_set_visible(app, 0, 1), WN_ERR_INVALID_ARGUMENT);
    expect_status("remove(root)", wn_node_remove(app, root), WN_ERR_INVALID_ARGUMENT);

    expect_status("clear_children(cards)", wn_node_clear_children(app, cards), WN_OK);
    count_after_clear = wn_app_node_count(app);
    printf("dynamic: clear_children -> nodes=%d\n", (int)count_after_clear);
    if (count_after_clear != count_initial - 8) {
        ++g_failures;
    }

    wn_app_on_event(app, NULL, NULL);
    wn_app_set_max_frames(app, frames);
    rc = wn_app_run(app);
    memset(&stats, 0, sizeof(stats));
    wn_app_get_stats(app, &stats);
    printf("dynamic: backend=%s exit=%d frames=%d startup_ms=%.2f frame_avg_ms=%.2f\n",
           stats.backend[0] != '\0' ? stats.backend : "(unknown)", rc, stats.frames,
           stats.startup_ms,
           stats.frames > 0 ? stats.frame_total_ms / (double)stats.frames : 0.0);

    /* D18: a second run owns its own frame stats, and a 0x0 surface is fixed up
     * instead of spinning without presenting. */
    {
        wn_app_stats again;
        int rc2;
        int reset_ok;

        (void)wn_view_resize(wn_app_view(app), 0, 0, 0.0f);
        wn_app_set_max_frames(app, 3);
        rc2 = wn_app_run(app);
        memset(&again, 0, sizeof(again));
        wn_app_get_stats(app, &again);
        reset_ok = rc2 == 0 && again.frames == 3 && again.startup_ms > 0.0;
        printf("dynamic: second run frames=%d rc=%d startup_ms=%.2f -> %s\n",
               again.frames, rc2, again.startup_ms,
               reset_ok ? "stats reset" : "FAIL");
        if (!reset_ok) {
            ++g_failures;
        }
    }

    printf("dynamic: %s\n", g_failures == 0 && rc == 0 ? "PASS" : "FAIL");
    wn_app_destroy(app);
    return g_failures == 0 && rc == 0 ? 0 : 1;
}
