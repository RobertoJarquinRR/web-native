/*
 * media_frame_test.c - offscreen RGBA frame path with the synthetic provider.
 *
 * Drives the real D3D11 renderer: poster/placeholder (no provider) -> first
 * decoded frame -> seek frame -> paused frame. The PNGs are checked by
 * verify_media_frames.py for color bars, frame-index bits, the moving marker
 * and object-fit fill/contain/cover geometry. Also asserts the display list
 * carries three video batches and no fake image-node reuse.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/wn_core_internal.h"
#include "webnative.h"
#include "wn_core.h"
#include "wn_view.h"
#include "media/media_test.h"

static int checks = 0;
static int failures = 0;
static int64_t g_now_us = 0;

static void check(const char *label, int ok)
{
    checks += 1;
    if (!ok) {
        failures += 1;
    }
    printf("  %s %s\n", ok ? "[ok]" : "[FAIL]", label);
}

static int64_t fake_now(void *context)
{
    (void)context;
    return g_now_us;
}

static int capture(wn_view *view, const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (wn_view_readback(view, path) != WN_OK) {
        printf("  [FAIL] readback %s: %s\n", path,
               wn_last_error() != NULL ? wn_last_error() : "unknown");
        failures += 1;
        return 0;
    }
    printf("  [ok] wrote %s\n", path);
    checks += 1;
    return 1;
}

static int render(wn_view *view)
{
    if (wn_view_render(view) != WN_OK) {
        printf("  [FAIL] render: %s\n",
               wn_last_error() != NULL ? wn_last_error() : "unknown");
        failures += 1;
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    wn_document *document;
    wn_core *core;
    wn_view *view;
    wn_node_id poster, placeholder, contain, cover;
    int placeholder_index;
    const wn_display_list *list;
    int i;
    int video_batches;
    int all_textured;

    if (argc < 3) {
        fprintf(stderr, "usage: media_frame_test <media.wnui> <out-dir>\n");
        return 2;
    }
    checks = 0;
    failures = 0;
    g_now_us = 0;

    document = wn_document_load_blob(argv[1]);
    check("v7 fixture loads", document != NULL);
    if (document == NULL) {
        printf("media_frame_test: FAIL (%s)\n",
               wn_last_error() != NULL ? wn_last_error() : "load");
        return 1;
    }
    core = wn_core_create(document);
    wn_document_release(document);
    check("core builds", core != NULL);
    view = core != NULL ? wn_view_create_offscreen_with_core(core, 400, 640)
                        : NULL;
    check("offscreen view builds", view != NULL);
    if (view == NULL) {
        wn_core_destroy(core);
        printf("media_frame_test: FAIL (view)\n");
        return 1;
    }
    check("view loads", wn_view_load(view, "", NULL, NULL) == WN_OK);
    wn_view_set_media_clock(view, fake_now, NULL);

    poster = wn_view_find(view, "body.root/video#poster");
    placeholder = wn_view_find(view, "body.root/video#placeholder");
    contain = wn_view_find(view, "body.root/video#contain");
    cover = wn_view_find(view, "body.root/video#cover");
    check("fixture nodes resolve",
          poster != WN_NODE_INVALID && placeholder != WN_NODE_INVALID &&
              contain != WN_NODE_INVALID && cover != WN_NODE_INVALID);
    placeholder_index = wn_view_resolve_handle(view, placeholder);
    check("placeholder index resolves", placeholder_index >= 0);

    /* No provider: verify_media.py's poster/placeholder gate still applies.
     * The first render also exercises the preload auto-open error path. */
    render(view);
    capture(view, argv[2], "frame_poster.png");
    list = wn_core_display_list(core);
    check("no provider emits no video batch", list->video_batch_count == 0);

    check("media_test provider registers",
          wn_view_register_media_provider(view, wn_media_test_ops(), NULL,
                                          100) == WN_OK);
    check("open placeholder/contain/cover",
          wn_view_media_open(view, placeholder) == WN_OK &&
              wn_view_media_open(view, contain) == WN_OK &&
              wn_view_media_open(view, cover) == WN_OK);
    check("play placeholder/contain/cover",
          wn_view_media_play(view, placeholder) == WN_OK &&
              wn_view_media_play(view, contain) == WN_OK &&
              wn_view_media_play(view, cover) == WN_OK);

    /* Frame 0: clock at zero, one render uploads and paints it. */
    g_now_us = 0;
    render(view);
    list = wn_core_display_list(core);
    video_batches = list->video_batch_count;
    all_textured = 1;
    for (i = 0; i < video_batches; ++i) {
        if (list->video_batches[i].texture == 0 ||
            list->video_batches[i].count != 6) {
            all_textured = 0;
        }
    }
    check("three video batches with real textures",
          video_batches == 3 && all_textured);
    check("the frame uses the video batch, not the poster image",
          list->image_batch_count == 1 /* only the untouched poster node */);
    check("the first frame is 320x180",
          core->media[placeholder_index].frame_width == WN_MEDIA_TEST_WIDTH &&
              core->media[placeholder_index].frame_height == WN_MEDIA_TEST_HEIGHT);
    check("the first frame is frame 0",
          core->media[placeholder_index].frame_pts_us == 0 &&
              core->media[placeholder_index].first_frame_seen);
    capture(view, argv[2], "frame_first.png");

    /* Seek to 5 s: the presented frame is the first one at/after the target. */
    check("seek placeholder/contain/cover",
          wn_view_media_seek(view, placeholder, 5000000) == WN_OK &&
              wn_view_media_seek(view, contain, 5000000) == WN_OK &&
              wn_view_media_seek(view, cover, 5000000) == WN_OK);
    render(view);
    check("the seek frame is at/after 5 s",
          core->media[placeholder_index].frame_pts_us >= 5000000 &&
              core->media[placeholder_index].frame_pts_us < 5100000);
    capture(view, argv[2], "frame_seek.png");

    /* Pause freezes the same frame and keeps the texture. */
    check("pause placeholder/contain/cover",
          wn_view_media_pause(view, placeholder) == WN_OK &&
              wn_view_media_pause(view, contain) == WN_OK &&
              wn_view_media_pause(view, cover) == WN_OK);
    {
        int64_t pts = core->media[placeholder_index].frame_pts_us;
        uint64_t texture = core->media[placeholder_index].frame_texture;
        g_now_us += 1000000;
        render(view);
        check("pause keeps the frame and the texture",
              core->media[placeholder_index].frame_pts_us == pts &&
                  core->media[placeholder_index].frame_texture == texture);
        capture(view, argv[2], "frame_paused.png");
    }

    /* Document replacement: every session is closed, every frame buffer and
     * texture released, and the new display list has no video batch. */
    {
        int replaced = wn_view_replace_document(
                           view, (wn_document *)wn_document_builtin()) == 0;
        check("document replacement succeeds", replaced);
        check("replacement closes every session and drops frames",
              wn_media_test_live_frames() == 0);
        if (replaced) {
            list = wn_core_display_list(wn_view_core(view));
            check("replacement rebuilds with no video batch",
                  list != NULL && list->video_batch_count == 0);
        }
        wn_view_destroy(view);
        if (!replaced) {
            wn_core_destroy(core);
        }
    }
    check("no frame buffer leaks after destroy",
          wn_media_test_live_frames() == 0);

    if (failures != 0) {
        printf("media_frame_test: FAIL (%d checks, %d failed)\n", checks,
               failures);
        return 1;
    }
    printf("media_frame_test: PASS (%d checks)\n", checks);
    return 0;
}
