/*
 * media_layout_test.c - a frontend-emitted v7 blob, loaded and laid out.
 *
 * The fixture page has five `<video>` nodes: poster/placeholder/contain/cover
 * and one with no CSS size. Locks kind 6 + media fields, the 300x150 default,
 * declared sizes, object-fit selection, the neutral placeholder, hit testing
 * and the typed media-null open path.
 */
#include <stdio.h>
#include <string.h>

#include "webnative.h"
#include "wn_core.h"

static int checks = 0;
static int failures = 0;

static void check(const char *label, int ok)
{
    checks += 1;
    if (!ok) {
        failures += 1;
    }
    printf("  %s %s\n", ok ? "[ok]" : "[FAIL]", label);
}

static int rect_is(wn_core *core, int node, float x, float y, float w, float h)
{
    wn_rect rect;
    if (wn_core_node_rect(core, node, &rect) != 0) {
        return 0;
    }
    return rect.x == x && rect.y == y && rect.w == w && rect.h == h;
}

int main(int argc, char **argv)
{
    wn_document *document;
    wn_core *core;
    int poster;
    int placeholder;
    int contain;
    int cover;
    int auto_node;

    if (argc < 2) {
        fprintf(stderr, "usage: media_layout_test <media.wnui>\n");
        return 2;
    }
    checks = 0;
    failures = 0;

    document = wn_document_load_blob(argv[1]);
    check("v7 fixture blob loads", document != NULL);
    if (document == NULL) {
        printf("media_layout_test: FAIL (%s)\n",
               wn_last_error() != NULL ? wn_last_error() : "load failed");
        return 1;
    }
    check("fixture has six nodes (root + five videos)",
          wn_document_node_count(document) == 6);
    core = wn_core_create(document);
    wn_document_release(document);
    check("core builds from the fixture", core != NULL);
    if (core == NULL) {
        printf("media_layout_test: FAIL (core)\n");
        return 1;
    }

    poster = wn_core_find_node(core, "body.root/video#poster");
    placeholder = wn_core_find_node(core, "body.root/video#placeholder");
    contain = wn_core_find_node(core, "body.root/video#contain");
    cover = wn_core_find_node(core, "body.root/video#cover");
    auto_node = wn_core_find_node(core, "body.root/video#auto");
    check("all five video nodes resolve by name",
          poster >= 0 && placeholder >= 0 && contain >= 0 && cover >= 0 &&
              auto_node >= 0);
    if (failures != 0) {
        wn_core_destroy(core);
        printf("media_layout_test: FAIL\n");
        return 1;
    }

    check("poster node is kind video", wn_core_node_is_video(core, poster));
    check("poster src is the compiled path",
          wn_core_node_media_src(core, poster) != NULL &&
              strcmp(wn_core_node_media_src(core, poster), "clips/poster.mp4") == 0);
    check("poster path is the compiled poster",
          wn_core_node_media_poster(core, poster) != NULL &&
              strcmp(wn_core_node_media_poster(core, poster),
                     "posters/poster.png") == 0);
    check("placeholder has no poster",
          wn_core_node_media_poster(core, placeholder) == NULL);

    /* Declared sizes and the documented 300x150 default. Videos are block
     * replaced boxes, so the fixture stacks them vertically. */
    check("poster box is 300x150 at the origin",
          rect_is(core, poster, 0.0f, 0.0f, 300.0f, 150.0f));
    check("placeholder box is 160x90",
          rect_is(core, placeholder, 0.0f, 150.0f, 160.0f, 90.0f));
    check("contain box is 100x100",
          rect_is(core, contain, 0.0f, 240.0f, 100.0f, 100.0f));
    check("cover box is 100x100",
          rect_is(core, cover, 0.0f, 340.0f, 100.0f, 100.0f));
    check("video without CSS size uses the 300x150 default",
          rect_is(core, auto_node, 0.0f, 440.0f, 300.0f, 150.0f));

    /* Paint: three posters produce image batches; the two poster-less videos
     * produce none (their placeholder is a solid pass). */
    wn_core_build_display_list(core);
    {
        const wn_display_list *list = wn_core_display_list(core);
        int image_batches = list->image_batch_count;
        int i;
        int poster_batches = 0;
        for (i = 0; i < image_batches; ++i) {
            if (list->image_batches[i].src != NULL &&
                strcmp(list->image_batches[i].src, "posters/poster.png") == 0) {
                ++poster_batches;
            }
        }
        /* Same texture and clip batches merge; all three posters are one
         * 3-quad draw. Poster-less videos contribute no image geometry. */
        check("the three posters form one merged image batch", poster_batches == 1);
        check("poster image geometry is 18 vertices (3 quads)",
              list->image_vertex_count == 18);
        check("poster-less videos emit no image batch", image_batches == 1);
        check("placeholder contributes solid geometry", list->vertex_count > 0);
    }

    /* Hit testing sees the replaced boxes themselves, like an image. */
    check("hit test resolves the poster video",
          wn_core_node_at(core, 50.0f, 50.0f) == poster);
    check("hit test resolves the placeholder video",
          wn_core_node_at(core, 50.0f, 195.0f) == placeholder);
    check("hit test resolves the default-size video",
          wn_core_node_at(core, 50.0f, 515.0f) == auto_node);

    /* State starts IDLE and seeds mute/loop from the document only. */
    {
        wn_media_state_info state;
        memset(&state, 0, sizeof(state));
        check("initial media state is IDLE",
              wn_core_media_get_state(core, poster, &state) == 0 &&
                  state.state == WN_MEDIA_STATE_IDLE &&
                  state.error == WN_MEDIA_OK);
        check("state info carries the fixed struct size",
              state.struct_size == (uint32_t)sizeof(state));
        check("muted/loop come from the document flags",
              state.muted == 1 && state.loop == 1);
        check("volume starts at 1.0", state.volume == 1.0f);
    }

    /* The provider seam always ends in media_null: open is accepted but the
     * node goes to ERROR and queues the documented event. Nothing pretends to
     * play. */
    check("open is accepted by the core",
          wn_core_media_open(core, poster) == 0);
    {
        wn_media_state_info state;
        int node = -1;
        int type = -1;
        int event_state = -1;
        int event_error = 0;
        const char *detail = NULL;
        memset(&state, 0, sizeof(state));
        check("open without a real provider ends in ERROR",
              wn_core_media_get_state(core, poster, &state) == 0 &&
                  state.state == WN_MEDIA_STATE_ERROR &&
                  state.error == WN_MEDIA_UNSUPPORTED_CONTAINER);
        check("open queues one ERROR event",
              wn_core_media_take_event(core, &node, &type, &event_state,
                                       &event_error, NULL, NULL, &detail) == 1);
        check("event targets the video node and carries the typed error",
              node == poster && type == WN_MEDIA_EVENT_ERROR &&
                  event_state == WN_MEDIA_STATE_ERROR &&
                  event_error == WN_MEDIA_UNSUPPORTED_CONTAINER);
        check("event detail names the path",
              detail != NULL && strstr(detail, "clips/poster.mp4") != NULL);
        check("event queue is empty afterwards",
              wn_core_media_take_event(core, NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL) == 0);
    }

    /* set_source replaces the runtime path, returns to IDLE and marks layout
     * dirty; the blob/document is untouched. */
    check("set_source accepts a local path",
          wn_core_media_set_source(core, poster, "clips/other.mp4") == 0);
    check("set_source updates the effective source",
          strcmp(wn_core_node_media_src(core, poster), "clips/other.mp4") == 0);
    {
        wn_media_state_info reset;
        memset(&reset, 0, sizeof(reset));
        check("set_source returns to IDLE",
              wn_core_media_get_state(core, poster, &reset) == 0 &&
                  reset.state == WN_MEDIA_STATE_IDLE &&
                  reset.error == WN_MEDIA_OK && reset.position_us == 0);
    }
    check("set_source rejects a scheme",
          wn_core_media_set_source(core, poster, "http://x/y.mp4") == -1);
    check("media controls reject an image node",
          wn_core_media_open(core, wn_core_find_node(core, "body.root")) == -1);

    wn_core_destroy(core);
    if (failures != 0) {
        printf("media_layout_test: FAIL (%d checks, %d failed)\n", checks,
               failures);
        return 1;
    }
    printf("media_layout_test: PASS (%d checks)\n", checks);
    return 0;
}
