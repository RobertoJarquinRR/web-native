/*
 * offscreen_test.c - vtable offscreen smoke test (WN_SURFACE_OFFSCREEN).
 *
 * Creates a window-less view, loads the compiled ui_data.h fixture, renders one
 * frame and writes it to a PNG through the backend readback. Run with
 * WN_RENDERER=d3d11 and WN_RENDERER=wgpu to compare both backends.
 */
#include "wn_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* D16: a nested dispatch where the innermost callback destroys the view that
 * owns the outer dispatch. The duplicate WN_EVENT_INPUT must be suppressed and
 * the view freed only when its outermost dispatch returns. */
static wn_view *g_nested_owner;
static wn_view *g_nested_target;
static int g_nested_owner_events;
static int g_nested_target_events;
static int g_nested_target_destroyed;

static void nested_owner_event(void *userdata, const wn_event *event)
{
    (void)userdata;
    ++g_nested_owner_events;
    if (event->type == WN_EVENT_CHAR && g_nested_target != NULL &&
        !g_nested_target_destroyed) {
        (void)wn_view_event(g_nested_target, event);
    }
}

static void nested_target_event(void *userdata, const wn_event *event)
{
    (void)userdata;
    (void)event;
    ++g_nested_target_events;
    if (!g_nested_target_destroyed) {
        g_nested_target_destroyed = 1;
        wn_view_destroy(g_nested_owner);
    }
}

static wn_view *g_self_view;
static int g_self_events;

static void self_destroy_event(void *userdata, const wn_event *event)
{
    (void)userdata;
    (void)event;
    ++g_self_events;
    wn_view_destroy(g_self_view);
}

static int center_of(wn_core *core, int node, float *x, float *y)
{
    int count = 0;
    const wn_rect *rects = wn_core_rects(core, &count);
    if (rects == NULL || node < 0 || node >= count) {
        return 0;
    }
    *x = rects[node].x + rects[node].w * 0.5f;
    *y = rects[node].y + rects[node].h * 0.5f;
    return 1;
}

static int test_destroy_in_callback(const char *blob)
{
    wn_document *owner_doc = wn_document_load_blob(blob);
    wn_document *target_doc = wn_document_load_blob(blob);
    wn_core *owner_core = owner_doc != NULL ? wn_core_create(owner_doc) : NULL;
    wn_core *target_core = target_doc != NULL ? wn_core_create(target_doc) : NULL;
    wn_view *owner = owner_core != NULL
                         ? wn_view_create_offscreen_with_core(owner_core, 480, 240)
                         : NULL;
    wn_view *target = target_core != NULL
                          ? wn_view_create_offscreen_with_core(target_core, 480, 240)
                          : NULL;
    int ok = 0;
    int input = -1;
    float x = 0.0f;
    float y = 0.0f;
    wn_event event;

    if (owner != NULL && target != NULL &&
        wn_view_load(owner, "", NULL, NULL) == WN_OK &&
        wn_view_load(target, "", NULL, NULL) == WN_OK) {
        input = wn_core_find_node(owner_core, "body.root/div.bar/input#search.search");
        ok = input >= 0 && center_of(owner_core, input, &x, &y);
    }
    if (ok) {
        g_nested_owner = owner;
        g_nested_target = target;
        g_nested_target_destroyed = 0;
        (void)wn_view_on_event(owner, nested_owner_event, NULL);
        (void)wn_view_on_event(target, nested_target_event, NULL);

        memset(&event, 0, sizeof(event));
        event.type = WN_EVENT_POINTER_DOWN;
        event.x = x;
        event.y = y;
        (void)wn_view_event(owner, &event);
        event.type = WN_EVENT_POINTER_UP;
        (void)wn_view_event(owner, &event);
        ok = wn_core_input_focused(owner_core) == input;

        g_nested_owner_events = 0;
        g_nested_target_events = 0;
        memset(&event, 0, sizeof(event));
        event.type = WN_EVENT_CHAR;
        event.codepoint = (uint32_t)'A';
        ok = ok && wn_view_event(owner, &event) == WN_OK &&
             g_nested_target_events == 1 && g_nested_owner_events == 1;
        /* The CHAR must really have changed the value; otherwise the duplicate
         * WN_EVENT_INPUT would never be armed and the guard would go untested. */
        ok = ok && strcmp(wn_core_input_value(owner_core, input), "A") == 0;
        /* The owner is freed by the deferred destroy iff the target callback
         * ran (it is the only place that destroys it). */
        if (g_nested_target_destroyed) {
            owner = NULL;
        }
    }
    if (target != NULL) {
        wn_view_destroy(target);
    }
    if (owner != NULL) {
        wn_view_destroy(owner);
    }
    wn_core_destroy(owner_core);
    wn_core_destroy(target_core);
    wn_document_destroy(owner_doc);
    wn_document_destroy(target_doc);
    printf("offscreen: destroy-in-callback%s\n", ok ? " PASS" : " FAIL");
    return ok;
}

static int test_self_destroy(const char *blob)
{
    wn_document *document = wn_document_load_blob(blob);
    wn_core *core = document != NULL ? wn_core_create(document) : NULL;
    wn_view *view = core != NULL ? wn_view_create_offscreen_with_core(core, 480, 240)
                                 : NULL;
    wn_event event;
    int ok = 0;

    if (view != NULL && wn_view_load(view, "", NULL, NULL) == WN_OK) {
        g_self_view = view;
        g_self_events = 0;
        (void)wn_view_on_event(view, self_destroy_event, NULL);
        memset(&event, 0, sizeof(event));
        event.type = WN_EVENT_POINTER_MOVE;
        ok = wn_view_event(view, &event) == WN_OK && g_self_events == 1;
        view = NULL; /* freed when the dispatch returned */
    }
    if (view != NULL) {
        wn_view_destroy(view);
    }
    wn_core_destroy(core);
    wn_document_destroy(document);
    printf("offscreen: self destroy-in-callback%s\n", ok ? " PASS" : " FAIL");
    return ok;
}

/* D19: internal-only. wn_app_load_document() always loads a fresh document, so
 * only a caller of wn_view_replace_document() can pass the document the view
 * already owns. The second call must not destroy it; wn_view_destroy() frees it
 * exactly once. */
static int test_replace_same_document(const char *blob)
{
    wn_document *document = wn_document_load_blob(blob);
    wn_view *view = wn_view_create_offscreen(480, 240);
    int ok = 0;

    if (document != NULL && view != NULL) {
        ok = wn_view_replace_document(view, document) == 0 &&
             wn_view_replace_document(view, document) == 0 &&
             wn_view_node_count(view) == wn_document_node_count(document);
        wn_view_destroy(view);
    } else {
        if (view != NULL) {
            wn_view_destroy(view);
        }
        wn_document_destroy(document);
    }
    printf("offscreen: same-document replace%s\n", ok ? " PASS" : " FAIL");
    return ok;
}


int main(int argc, char **argv)
{
    wn_view *view;
    wn_status status;
    int width = 960;
    int height = 640;
    const char *path = "offscreen.png";

    if (argc >= 3) {
        width = atoi(argv[1]);
        height = atoi(argv[2]);
    }
    if (argc >= 4) {
        path = argv[3];
    }
    view = wn_view_create_offscreen(width, height);
    if (view == NULL) {
        fprintf(stderr, "offscreen: create failed: %s\n", wn_last_error());
        return 1;
    }
    printf("offscreen: backend=%s device=%s\n", wn_view_backend(view),
           wn_view_device(view) != NULL ? wn_view_device(view) : "(unknown)");
    status = wn_view_load(view, "", NULL, NULL);
    if (status != WN_OK) {
        fprintf(stderr, "offscreen: load failed: %s\n", wn_last_error());
        wn_view_destroy(view);
        return 1;
    }
    if (wn_view_render(view) != 0) {
        fprintf(stderr, "offscreen: render failed: %s\n", wn_last_error());
        wn_view_destroy(view);
        return 1;
    }
    status = wn_view_readback(view, path);
    if (status != WN_OK) {
        fprintf(stderr, "offscreen: readback failed: %s\n", wn_last_error());
        wn_view_destroy(view);
        return 1;
    }
    wn_view_destroy(view);
    printf("offscreen: ok %dx%d -> %s\n", width, height, path);
    if (argc >= 5) {
        int lifecycle = test_destroy_in_callback(argv[4]);
        lifecycle = test_self_destroy(argv[4]) && lifecycle;
        lifecycle = test_replace_same_document(argv[4]) && lifecycle;
        if (!lifecycle) {
            fprintf(stderr, "offscreen: lifecycle tests failed\n");
            return 1;
        }
    }
    return 0;
}
