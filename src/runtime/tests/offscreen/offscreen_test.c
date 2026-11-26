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
        if (ok) {
            /* ABI v6: the view holds its own reference, so the caller can drop
             * theirs while the view stays usable. */
            wn_document_release(document);
            ok = wn_view_node_count(view) == wn_document_node_count(document);
        } else {
            wn_document_release(document);
        }
        wn_view_destroy(view);
    } else {
        if (view != NULL) {
            wn_view_destroy(view);
        }
        wn_document_release(document);
    }
    printf("offscreen: same-document replace%s\n", ok ? " PASS" : " FAIL");
    return ok;
}

/* A2: the core retains the document; releasing the caller's reference right
 * after wn_core_create() must leave the core usable, and a release after the
 * last reference freed the document is clamped (no crash). */
static int test_document_refcount(const char *blob)
{
    wn_document *document = wn_document_load_blob(blob);
    wn_core *core = document != NULL ? wn_core_create(document) : NULL;
    int ok = 0;

    if (document != NULL && core != NULL) {
        wn_document_release(document);
        ok = wn_core_node_count(core) > 0 &&
             wn_core_node_is_alive(core, 0);
        wn_core_relayout(core);
        wn_core_build_display_list(core);
        ok = ok && wn_core_display_list(core)->vertex_count > 0;
        wn_core_destroy(core);       /* drops the last reference */
        wn_document_release(document); /* double release: clamped */
        wn_document_release(document);
    } else {
        wn_core_destroy(core);
        wn_document_release(document);
    }
    printf("offscreen: document refcount%s\n", ok ? " PASS" : " FAIL");
    return ok;
}

/* B3: view-level text/rect getters. */
static int test_view_reads(void)
{
    wn_view *view = wn_view_create_offscreen(960, 640);
    int ok = 0;

    if (view != NULL && wn_view_load(view, "", NULL, NULL) == WN_OK) {
        wn_node_id brand = wn_view_find(view, "body.root/div.sidebar/div.brand/text");
        const char *text = wn_view_get_text(view, brand);
        ok = brand != WN_NODE_INVALID && text != NULL &&
             strcmp(text, "webnative") == 0;
        ok = ok && wn_view_set_node_text(view, brand, "ABI six") == 0 &&
             strcmp(wn_view_get_text(view, brand), "ABI six") == 0;

        if (ok) {
            wn_node_id root = wn_view_root(view);
            wn_rect via_view;
            wn_rect via_core;
            int index;
            ok = wn_view_get_rect(view, brand, &via_view) == WN_OK;
            index = wn_view_resolve_handle(view, brand);
            ok = ok && wn_core_node_rect((wn_core *)wn_view_core(view), index,
                                         &via_core) == 0 &&
                 via_view.x == via_core.x && via_view.y == via_core.y &&
                 via_view.w == via_core.w && via_view.h == via_core.h;
            ok = ok && root != WN_NODE_INVALID;
            {
                wn_rect out;
                memset(&out, 0x7f, sizeof(out));
                ok = ok && wn_view_get_rect(view, WN_NODE_INVALID, &out) ==
                               WN_ERR_INVALID_ARGUMENT;
            }
        }
    }
    if (view != NULL) {
        wn_view_destroy(view);
    }
    printf("offscreen: view text/rect getters%s\n", ok ? " PASS" : " FAIL");
    return ok;
}

/* B5: wn_view_tick marks the core dirty and produces a real frame. */
static int test_view_tick(void)
{
    wn_view *view = wn_view_create_offscreen(960, 640);
    wn_view_stats stats;
    int frames_before = 0;
    int ok = 0;

    if (view != NULL && wn_view_load(view, "", NULL, NULL) == WN_OK &&
        wn_view_render(view) == WN_OK) {
        memset(&stats, 0, sizeof(stats));
        (void)wn_view_get_stats(view, &stats);
        frames_before = stats.frames;
        ok = wn_view_needs_render(view) == 0;
        ok = ok && wn_view_tick(view) == WN_OK &&
             wn_core_dirty(wn_view_core(view)) == 1 &&
             wn_view_needs_render(view) == 1;
        ok = ok && wn_view_render(view) == WN_OK;
        memset(&stats, 0, sizeof(stats));
        (void)wn_view_get_stats(view, &stats);
        ok = ok && stats.frames == frames_before + 1;
        ok = ok && wn_view_tick(view) == WN_OK &&
             wn_view_needs_render(view) == 1;
    }
    if (view != NULL) {
        wn_view_destroy(view);
    }
    printf("offscreen: view tick frames%s\n", ok ? " PASS" : " FAIL");
    return ok;
}

/* B3 + B4: focus/blur and pointer button/click_count reporting. */
static wn_event g_last_event;
static int g_event_count;

static void capture_event(void *userdata, const wn_event *event)
{
    (void)userdata;
    g_last_event = *event;
    ++g_event_count;
}

static int node_center(wn_view *view, wn_node_id handle, float *x, float *y)
{
    int index = wn_view_resolve_handle(view, handle);
    wn_rect rect;
    if (index < 0 || wn_core_node_rect((wn_core *)wn_view_core(view), index,
                                       &rect) != 0) {
        return 0;
    }
    *x = rect.x + rect.w * 0.5f;
    *y = rect.y + rect.h * 0.5f;
    return rect.w > 0.0f && rect.h > 0.0f;
}

static int test_view_input_focus(const char *blob)
{
    wn_document *document = wn_document_load_blob(blob);
    wn_view *view = wn_view_create_offscreen(480, 240);
    int ok = 0;

    if (document != NULL && view != NULL &&
        wn_view_replace_document(view, document) == 0) {
        wn_document_release(document);
        document = NULL;
        if (wn_view_load(view, "", NULL, NULL) == WN_OK) {
            wn_node_id input = wn_view_find(
                view, "body.root/div.bar/input#search.search");
            wn_core *core = (wn_core *)wn_view_core(view);
            int index = wn_view_resolve_handle(view, input);
            wn_input_state state;
            ok = input != WN_NODE_INVALID && index >= 0 &&
                 wn_view_set_value(view, input, "hello") == WN_OK;
            /* Move the caret to 0 first so focus-at-end is observable. */
            ok = ok && wn_core_focus_input(core, index, 0.0f) == index;
            ok = ok && wn_view_focus_input(view, input) == WN_OK &&
                 wn_view_input_focused(view);
            memset(&state, 0, sizeof(state));
            ok = ok && wn_core_input_state(core, index, &state) == 0 &&
                 state.cursor == 5 && state.anchor < 0;
            ok = ok && wn_view_blur_input(view) == WN_OK &&
                 !wn_view_input_focused(view) &&
                 wn_view_blur_input(view) == WN_OK;
        }
    }
    wn_document_release(document);
    if (view != NULL) {
        wn_view_destroy(view);
    }
    printf("offscreen: view focus/blur%s\n", ok ? " PASS" : " FAIL");
    return ok;
}

static int test_pointer_events(void)
{
    wn_view *view = wn_view_create_offscreen(960, 640);
    wn_core *core;
    wn_node_id nav;
    wn_node_id nav2;
    wn_node_id brand;
    float x = 0.0f;
    float y = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    float bx = 0.0f;
    float by = 0.0f;
    wn_event event;
    int ok = 0;

#define POINTER_CHECK(cond, label)                                           \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  pointer check FAILED: %s\n", label);                   \
            ok = 0;                                                          \
        }                                                                    \
    } while (0)

    if (view != NULL && wn_view_load(view, "", NULL, NULL) == WN_OK) {
        (void)wn_view_on_event(view, capture_event, NULL);
        core = (wn_core *)wn_view_core(view);
        nav = wn_view_find(view, "body.root/div.sidebar/div.nav");
        nav2 = wn_view_find(view, "body.root/div.sidebar/div.nav[1]");
        brand = wn_view_find(view, "body.root/div.sidebar/div.brand/text");
        ok = nav != WN_NODE_INVALID && nav2 != WN_NODE_INVALID &&
             brand != WN_NODE_INVALID && node_center(view, nav, &x, &y) &&
             node_center(view, nav2, &nx, &ny) &&
             node_center(view, brand, &bx, &by);
        POINTER_CHECK(ok, "nodes found and centred");

        if (ok) {
            g_event_count = 0;
            memset(&event, 0, sizeof(event));
            event.type = WN_EVENT_POINTER_MOVE;
            event.x = x;
            event.y = y;
            POINTER_CHECK(wn_view_event(view, &event) == WN_OK && g_event_count == 1, "move delivered once");
            POINTER_CHECK(g_last_event.node == nav, "move targets nav");
            POINTER_CHECK(g_last_event.button == 0 &&
                          g_last_event.click_count == 0,
                          "move has no button/click count");

            memset(&event, 0, sizeof(event));
            event.type = WN_EVENT_POINTER_DOWN;
            event.x = x;
            event.y = y;
            event.button = 1;
            event.click_count = 1;
            POINTER_CHECK(wn_view_event(view, &event) == WN_OK && g_last_event.node == nav,
                          "left down targets nav");
            POINTER_CHECK(g_last_event.button == 1 &&
                          g_last_event.click_count == 1,
                          "left down carries button 1 count 1");
            event.type = WN_EVENT_POINTER_UP;
            POINTER_CHECK(wn_view_event(view, &event) == WN_OK && g_last_event.node == nav,
                          "left up targets nav");
            POINTER_CHECK(g_last_event.button == 1, "left up carries button 1");
            {
                int nav_state = wn_core_node_state(
                    core, wn_view_resolve_handle(view, nav));
                POINTER_CHECK((nav_state & (WN_STATE_SELECTED | WN_STATE_PRESS)) ==
                                  WN_STATE_SELECTED,
                              "left up selects nav");
            }

            /* Double click: the host maps WM_LBUTTONDBLCLK to count 2. */
            event.type = WN_EVENT_POINTER_DOWN;
            event.click_count = 2;
            POINTER_CHECK(wn_view_event(view, &event) == WN_OK && g_last_event.click_count == 2 &&
                          g_last_event.button == 1,
                          "double down carries count 2");
            event.type = WN_EVENT_POINTER_UP;
            POINTER_CHECK(wn_view_event(view, &event) == WN_OK && g_last_event.click_count == 2,
                          "double up keeps count 2");

            /* Right click: reports the node but does not press/select. */
            memset(&event, 0, sizeof(event));
            event.type = WN_EVENT_POINTER_DOWN;
            event.x = nx;
            event.y = ny;
            event.button = 2;
            event.click_count = 1;
            POINTER_CHECK(wn_view_event(view, &event) == WN_OK && g_last_event.node == nav2,
                          "right down targets nav2");
            POINTER_CHECK(g_last_event.button == 2 && wn_core_press(core) == -1,
                          "right down does not press");
            POINTER_CHECK((wn_core_node_state(
                               core, wn_view_resolve_handle(view, nav2)) &
                           (WN_STATE_PRESS | WN_STATE_SELECTED)) == 0,
                          "right down does not select nav2");

            /* Middle click on a non-interactive node reports that node. */
            memset(&event, 0, sizeof(event));
            event.type = WN_EVENT_POINTER_DOWN;
            event.x = bx;
            event.y = by;
            event.button = 3;
            event.click_count = 1;
            POINTER_CHECK(wn_view_event(view, &event) == WN_OK && g_last_event.node == brand,
                          "middle down targets the brand text");
            POINTER_CHECK(g_last_event.button == 3 && wn_core_press(core) == -1,
                          "middle down does not press");

            /* Left click on a non-interactive node also reports the hit node
             * but leaves the pointer state machine empty. */
            memset(&event, 0, sizeof(event));
            event.type = WN_EVENT_POINTER_DOWN;
            event.x = bx;
            event.y = by;
            event.button = 1;
            event.click_count = 1;
            POINTER_CHECK(wn_view_event(view, &event) == WN_OK && g_last_event.node == brand,
                          "left down targets the brand text");
            POINTER_CHECK(wn_core_press(core) == -1,
                          "left down on non-interactive leaves press empty");
            event.type = WN_EVENT_POINTER_UP;
            POINTER_CHECK(wn_view_event(view, &event) == WN_OK && g_last_event.node == brand,
                          "left up targets the brand text");
        }
    }
#undef POINTER_CHECK
    if (view != NULL) {
        wn_view_destroy(view);
    }
    printf("offscreen: pointer button/click_count%s\n", ok ? " PASS" : " FAIL");
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

    {
        int abi6 = test_view_reads();
        abi6 = test_view_tick() && abi6;
        abi6 = test_pointer_events() && abi6;
        if (!abi6) {
            fprintf(stderr, "offscreen: ABI v6 view tests failed\n");
            return 1;
        }
    }
    if (argc >= 5) {
        int lifecycle = test_destroy_in_callback(argv[4]);
        lifecycle = test_self_destroy(argv[4]) && lifecycle;
        lifecycle = test_replace_same_document(argv[4]) && lifecycle;
        lifecycle = test_document_refcount(argv[4]) && lifecycle;
        lifecycle = test_view_input_focus(argv[4]) && lifecycle;
        if (!lifecycle) {
            fprintf(stderr, "offscreen: lifecycle tests failed\n");
            return 1;
        }
    }
    return 0;
}
