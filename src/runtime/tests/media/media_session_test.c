/*
 * media_session_test.c - the E4 session driver against the synthetic provider.
 *
 * Deterministic, headless and thread-free: the clock is advanced by hand and
 * every tick/present is explicit. Fixes the documented state machine, event
 * cadence, seek/loop/ENDED behaviour, source-less nodes, preload/autoplay and
 * the provider lease contract (no frame buffer is leaked).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "document/wn_blob.h"
#include "webnative.h"
#include "wn_core.h"
#include "wn_media.h"
#include "media/media_test.h"

/* MSVC /O2 can report a false-positive C4702 chain in this single-threaded
 * test after inlining check() around external media calls. The binary is the
 * gate: every check must print. */
#if defined(_MSC_VER)
#  pragma warning(disable : 4702)
#endif

static int checks = 0;
static int failures = 0;
static int64_t g_now_us = 0;
static int g_events = 0;
static int g_last_type = -1;
static int g_last_state = -1;
static int g_last_error = 0;
static int g_first_frame_events = 0;
static int g_ended_events = 0;
static int g_opened_events = 0;
static int g_progress_events = 0;
static int64_t g_last_event_pts = -1;

static int g_uploads = 0;
static int g_release_textures = 0;
static int64_t g_last_pts = -1;
static int g_last_w = 0;
static int g_last_h = 0;

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

static uint32_t sink_formats(void *context)
{
    (void)context;
    return WN_MEDIA_BIT(WN_PIXEL_RGBA8);
}

static uint32_t sink_native_kind(void *context)
{
    (void)context;
    return 0;
}

static int sink_device(void *context, void **out)
{
    (void)context;
    if (out != NULL) {
        *out = NULL;
    }
    return 1;
}

static wn_media_status sink_upload(void *context, const wn_video_frame *frame,
                                   uint64_t *out_texture)
{
    (void)context;
    g_uploads += 1;
    g_last_pts = frame->pts_us;
    g_last_w = frame->display_width;
    g_last_h = frame->display_height;
    *out_texture = (uint64_t)g_uploads;
    return WN_MEDIA_OK;
}

static wn_media_status sink_import(void *context, void *native_texture,
                                   uint32_t slice, const wn_video_frame *desc,
                                   uint64_t *out_texture)
{
    (void)context;
    (void)native_texture;
    (void)slice;
    (void)desc;
    *out_texture = 0;
    return WN_MEDIA_UNSUPPORTED_CODEC;
}

static void sink_release(void *context, uint64_t texture)
{
    (void)context;
    (void)texture;
    g_release_textures += 1;
}

static void sink_stats(void *context, wn_media_upload_stats *out)
{
    (void)context;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->struct_size = (uint32_t)sizeof(*out);
    }
}

static const wn_video_sink_ops g_sink = {
    (uint32_t)sizeof(wn_video_sink_ops),
    WN_MEDIA_SINK_ABI,
    "test-sink",
    sink_formats,
    sink_native_kind,
    sink_device,
    sink_upload,
    sink_import,
    sink_release,
    sink_stats
};

static void on_event(const wn_media_event *event)
{
    g_events += 1;
    g_last_type = (int)event->type;
    g_last_state = event->state;
    g_last_error = event->error;
    g_last_event_pts = event->position_us;
    switch (event->type) {
    case WN_MEDIA_EVENT_OPENED:
        g_opened_events += 1;
        break;
    case WN_MEDIA_EVENT_FIRST_FRAME:
        g_first_frame_events += 1;
        break;
    case WN_MEDIA_EVENT_ENDED:
        g_ended_events += 1;
        break;
    case WN_MEDIA_EVENT_PROGRESS:
        g_progress_events += 1;
        break;
    default:
        break;
    }
}

/* The core queues events; the view normally drains them. Here the test is the
 * UI thread, so it drains and forwards to the same shape. */
static void drain_events(wn_core *core)
{
    int node = 0;
    int type = 0;
    int state = 0;
    int error = 0;
    int64_t position_us = 0;
    int64_t duration_us = 0;
    const char *detail = NULL;
    while (wn_core_media_take_event(core, &node, &type, &state, &error,
                                    &position_us, &duration_us, &detail)) {
        wn_media_event event;
        memset(&event, 0, sizeof(event));
        event.struct_size = (uint32_t)sizeof(event);
        event.type = (uint32_t)type;
        event.node = (wn_node_id)node;
        event.state = state;
        event.error = error;
        event.position_us = position_us;
        event.duration_us = duration_us;
        event.detail = detail;
        on_event(&event);
    }
}

static void reset_events(void)
{
    g_events = 0;
    g_last_type = -1;
    g_last_state = -1;
    g_last_error = 0;
    g_first_frame_events = 0;
    g_ended_events = 0;
    g_opened_events = 0;
    g_progress_events = 0;
    g_last_event_pts = -1;
}

static void pump(wn_core *core)
{
    wn_core_media_tick(core);
    (void)wn_core_media_present(core);
    drain_events(core);
}

/* Advances the injected clock in vsync-sized steps, like a real host loop. */
static void run_for(wn_core *core, int64_t duration_us)
{
    int64_t step = 16667;
    int64_t end = g_now_us + duration_us;
    while (g_now_us < end) {
        g_now_us += step;
        if (g_now_us > end) {
            g_now_us = end;
        }
        pump(core);
    }
}

static int state_of(wn_core *core, int node)
{
    wn_media_state_info info;
    memset(&info, 0, sizeof(info));
    if (wn_core_media_get_state(core, node, &info) != 0) {
        return -1;
    }
    return info.state;
}

static int frame_index_of(int64_t pts_us)
{
    return (int)(pts_us * WN_MEDIA_TEST_FRAMES / WN_MEDIA_TEST_DURATION_US);
}

/* ------------------------------------------------- source-less fixture */

#define SS_BLOB_CAP 1024
#define SS_NODES_OFFSET 96u
#define SS_NODES_COUNT 2u
#define SS_NAMES_OFFSET (SS_NODES_OFFSET + SS_NODES_COUNT * 252u)
#define SS_STRINGS_OFFSET (SS_NAMES_OFFSET + SS_NODES_COUNT * 4u)
#define SS_STRINGS_SIZE 1u
#define SS_MEDIA_OFFSET ((SS_STRINGS_OFFSET + SS_STRINGS_SIZE + 7u) & ~7u)
#define SS_BLOB_SIZE (SS_MEDIA_OFFSET + 16u)

static wn_document *build_source_less(void)
{
    static unsigned char data[SS_BLOB_CAP];
    WnBlobHeader *header;
    WnBlobSection *sections;
    WnBlobNode *nodes;
    uint32_t *names;
    WnBlobMediaRecord *media;
    memset(data, 0, sizeof(data));
    header = (WnBlobHeader *)data;
    header->magic = WN_BLOB_MAGIC;
    header->version = WN_BLOB_VERSION;
    header->section_count = 4;
    header->file_size = SS_BLOB_SIZE;
    header->node_count = SS_NODES_COUNT;
    header->viewport_width = 200;
    header->viewport_height = 200;
    sections = (WnBlobSection *)(data + WN_BLOB_HEADER_SIZE);
    sections[0].id = WN_BLOB_SECTION_NODES;
    sections[0].offset = SS_NODES_OFFSET;
    sections[0].size = SS_NODES_COUNT * (uint32_t)sizeof(WnBlobNode);
    sections[1].id = WN_BLOB_SECTION_NAMES;
    sections[1].offset = SS_NAMES_OFFSET;
    sections[1].size = SS_NODES_COUNT * 4u;
    sections[2].id = WN_BLOB_SECTION_STRINGS;
    sections[2].offset = SS_STRINGS_OFFSET;
    sections[2].size = SS_STRINGS_SIZE;
    sections[3].id = WN_BLOB_SECTION_MEDIA;
    sections[3].offset = SS_MEDIA_OFFSET;
    sections[3].size = (uint32_t)sizeof(WnBlobMediaRecord);
    nodes = (WnBlobNode *)(data + SS_NODES_OFFSET);
    nodes[0].parent = -1;
    nodes[0].kind = 0;
    nodes[0].font_size = 16.0f;
    nodes[0].opacity = 1.0f;
    nodes[0].width = -1.0f;
    nodes[0].height = -1.0f;
    nodes[0].width_pct = 100.0f;
    nodes[0].height_pct = 100.0f;
    nodes[1].parent = 0;
    nodes[1].kind = WN_BLOB_NODE_VIDEO;
    nodes[1].font_size = 16.0f;
    nodes[1].opacity = 1.0f;
    nodes[1].width = -1.0f;
    nodes[1].height = -1.0f;
    names = (uint32_t *)(data + SS_NAMES_OFFSET);
    names[0] = 0;
    names[1] = 0;
    data[SS_STRINGS_OFFSET] = '\0';
    media = (WnBlobMediaRecord *)(data + SS_MEDIA_OFFSET);
    media->node = 1;
    media->src = 0; /* host-bound player node */
    media->poster = 0;
    media->flags = 0;
    media->preload = WN_MEDIA_PRELOAD_METADATA;
    return wn_document_load_blob_memory(data, SS_BLOB_SIZE);
}

int main(int argc, char **argv)
{
    wn_document *document;
    wn_core *core;
    int poster, placeholder, contain, auto_node;
    wn_media_state_info info;

    if (argc < 2) {
        fprintf(stderr, "usage: media_session_test <media.wnui>\n");
        return 2;
    }
    checks = 0;
    failures = 0;

    document = wn_document_load_blob(argv[1]);
    check("v7 fixture loads", document != NULL);
    if (document == NULL) {
        printf("media_session_test: FAIL (%s)\n",
               wn_last_error() != NULL ? wn_last_error() : "load");
        return 1;
    }
    core = wn_core_create(document);
    wn_document_release(document);
    check("core builds", core != NULL);
    if (core == NULL) {
        printf("media_session_test: FAIL (core)\n");
        return 1;
    }
    poster = wn_core_find_node(core, "body.root/video#poster");
    placeholder = wn_core_find_node(core, "body.root/video#placeholder");
    contain = wn_core_find_node(core, "body.root/video#contain");
    auto_node = wn_core_find_node(core, "body.root/video#auto");
    check("fixture nodes resolve",
          poster >= 0 && placeholder >= 0 && contain >= 0 && auto_node >= 0);

    wn_core_set_media_clock(core, fake_now, NULL);
    wn_core_set_media_sink(core, &g_sink, NULL);
    check("media_test provider registers",
          wn_core_register_media_provider(core, wn_media_test_ops(), NULL,
                                          100) == 0);

    /* First tick: preload >= metadata opens every node; autoplay starts the
     * poster and only the poster. */
    reset_events();
    g_now_us = 0;
    pump(core);
    check("preload opens all metadata nodes", state_of(core, placeholder) ==
                                                  WN_MEDIA_STATE_PAUSED);
    check("autoplay starts only the flagged node",
          state_of(core, poster) == WN_MEDIA_STATE_PLAYING &&
              state_of(core, contain) == WN_MEDIA_STATE_PAUSED);
    check("OPENED events are published",
          g_opened_events >= 4 &&
              wn_core_media_get_state(core, placeholder, &info) == 0 &&
              info.duration_us == WN_MEDIA_TEST_DURATION_US);
    check("open reports the intrinsic 320x180 size",
          wn_core_media_get_state(core, placeholder, &info) == 0 &&
              info.stream_flags == WN_MEDIA_INFO_SEEKABLE);
    check("pause on an autoplay node freezes it",
          wn_core_media_pause(core, poster) == 0 &&
              state_of(core, poster) == WN_MEDIA_STATE_PAUSED);

    /* Second open is a fresh request, not a crash. */
    check("open is repeatable", wn_core_media_open(core, placeholder) == 0 &&
                                    state_of(core, placeholder) ==
                                        WN_MEDIA_STATE_PAUSED);
    check("play twice is idempotent",
          wn_core_media_play(core, placeholder) == 0 &&
              wn_core_media_play(core, placeholder) == 0 &&
              state_of(core, placeholder) == WN_MEDIA_STATE_PLAYING);
    check("pause twice is idempotent",
          wn_core_media_pause(core, placeholder) == 0 &&
              wn_core_media_pause(core, placeholder) == 0 &&
              state_of(core, placeholder) == WN_MEDIA_STATE_PAUSED);
    check("play after pause resumes",
          wn_core_media_play(core, placeholder) == 0 &&
              state_of(core, placeholder) == WN_MEDIA_STATE_PLAYING);

    /* Frame presentation: every vsync step yields at most one upload. */
    reset_events();
    g_uploads = 0;
    g_last_pts = -1;
    run_for(core, 500000);
    check("playing uploads real frames", g_uploads > 10);
    check("frames are 320x180 RGBA", g_last_w == WN_MEDIA_TEST_WIDTH &&
                                        g_last_h == WN_MEDIA_TEST_HEIGHT);
    check("presented PTS tracks the clock",
          g_last_pts >= 450000 && g_last_pts <= 520000);
    check("FIRST_FRAME is published once", g_first_frame_events == 1);
    check("PROGRESS is ~4 Hz", g_progress_events >= 1 &&
                                   g_progress_events <= 3);
    check("position follows the clock",
          wn_core_media_get_state(core, placeholder, &info) == 0 &&
              info.position_us >= 450000 && info.position_us <= 520000);

    wn_core_media_pause(core, placeholder);
    {
        int64_t frozen = wn_core_media_position_us(core, placeholder);
        run_for(core, 1000000);
        check("pause freezes the position",
              wn_core_media_position_us(core, placeholder) == frozen);
    }

    /* seek: flush, re-anchor and present the first frame at or after target. */
    reset_events();
    g_uploads = 0;
    check("seek enters SEEKING",
          wn_core_media_seek(core, placeholder, 5000000) == 0 &&
              state_of(core, placeholder) == WN_MEDIA_STATE_SEEKING);
    pump(core);
    check("the first frame after the seek is presented",
          g_uploads == 1 && g_last_pts >= 5000000 &&
              g_last_pts < 5100000);
    check("seek settles in PAUSED when it started paused",
          state_of(core, placeholder) == WN_MEDIA_STATE_PAUSED);
    check("seek position is the presented frame",
          wn_core_media_get_state(core, placeholder, &info) == 0 &&
              info.position_us == g_last_pts);

    /* seek clamping. */
    check("seek past the duration clamps",
          wn_core_media_seek(core, placeholder, 99000000) == 0);
    pump(core);
    check("clamped seek lands at EOF",
          state_of(core, placeholder) == WN_MEDIA_STATE_PAUSED &&
              wn_core_media_get_state(core, placeholder, &info) == 0 &&
              info.position_us <= WN_MEDIA_TEST_DURATION_US);

    /* ENDED, then play() restarts at zero. */
    wn_core_media_seek(core, placeholder, 0);
    pump(core);
    wn_core_media_play(core, placeholder);
    reset_events();
    g_ended_events = 0;
    run_for(core, WN_MEDIA_TEST_DURATION_US + 200000);
    check("playing to the end reaches ENDED",
          state_of(core, placeholder) == WN_MEDIA_STATE_ENDED);
    check("ENDED is published once", g_ended_events == 1);
    check("ENDED keeps the session (duration readable)",
          wn_core_media_get_state(core, placeholder, &info) == 0 &&
              info.duration_us == WN_MEDIA_TEST_DURATION_US);
    g_uploads = 0;
    check("play from ENDED restarts at zero",
          wn_core_media_play(core, placeholder) == 0 &&
              state_of(core, placeholder) == WN_MEDIA_STATE_PLAYING &&
              wn_core_media_get_state(core, placeholder, &info) == 0 &&
              info.position_us == 0);
    pump(core);
    check("the restart presents frame zero", g_uploads >= 1 &&
                                                 frame_index_of(g_last_pts) == 0);

    /* loop: never reaches ENDED and wraps to the beginning. */
    wn_core_media_pause(core, placeholder);
    wn_core_media_set_loop(core, contain, 1);
    wn_core_media_play(core, contain);
    reset_events();
    run_for(core, WN_MEDIA_TEST_DURATION_US + 100000);
    check("loop never publishes ENDED", g_ended_events == 0);
    check("loop restarts at the beginning",
          wn_core_media_get_state(core, contain, &info) == 0 &&
              info.position_us < WN_MEDIA_TEST_DURATION_US / 2);
    wn_core_media_pause(core, contain);

    /* close returns to IDLE and releases the frame queue. */
    wn_core_media_close(core, placeholder);
    check("close returns to IDLE",
          state_of(core, placeholder) == WN_MEDIA_STATE_IDLE &&
              wn_core_media_get_state(core, placeholder, &info) == 0 &&
              info.position_us == 0 && info.duration_us == 0);
    {
        int node = 0;
        int type = 0;
        int state = 0;
        int error = 0;
        int64_t position_us = 0;
        int64_t duration_us = 0;
        const char *detail = NULL;
        int stale = 0;
        while (wn_core_media_take_event(core, &node, &type, &state, &error,
                                        &position_us, &duration_us, &detail)) {
            if (node == placeholder) {
                stale = 1;
            }
        }
        check("close drops the queued events of the node", stale == 0);
    }
    check("open after close works again",
          wn_core_media_open(core, placeholder) == 0 &&
              state_of(core, placeholder) == WN_MEDIA_STATE_PAUSED);

    /* Idle play: IDLE + play() opens first. */
    wn_core_media_close(core, auto_node);
    check("play from IDLE opens and plays",
          wn_core_media_play(core, auto_node) == 0 &&
              state_of(core, auto_node) == WN_MEDIA_STATE_PLAYING);
    wn_core_media_pause(core, auto_node);

    /* Source-less host-bound node. */
    {
        wn_document *source_less = build_source_less();
        wn_core *bare;
        int video;
        check("source-less v7 blob loads", source_less != NULL);
        bare = source_less != NULL ? wn_core_create(source_less) : NULL;
        wn_document_release(source_less);
        check("source-less core builds", bare != NULL);
        if (bare != NULL) {
            video = 1; /* root 0, video 1 (source-less fixture) */
            wn_core_set_media_clock(bare, fake_now, NULL);
            wn_core_set_media_sink(bare, &g_sink, NULL);
            wn_core_register_media_provider(bare, wn_media_test_ops(), NULL,
                                            100);
            wn_core_build_display_list(bare);
            check("a source-less node paints the placeholder, not a frame",
                  wn_core_display_list(bare)->video_batch_count == 0 &&
                      wn_core_display_list(bare)->vertex_count > 0);
            reset_events();
            check("open on a source-less node is WN_ERR_STATE",
                  wn_core_media_open(bare, video) == -2 &&
                      state_of(bare, video) == WN_MEDIA_STATE_ERROR);
            drain_events(bare);
            check("source-less open publishes a typed ERROR",
                  g_last_type == WN_MEDIA_EVENT_ERROR &&
                      g_last_error == WN_MEDIA_UNSUPPORTED_CONTAINER);
            check("set_source recovers to IDLE",
                  wn_core_media_set_source(bare, video, "clips/live.mp4") == 0 &&
                      state_of(bare, video) == WN_MEDIA_STATE_IDLE);
            check("set_source enables open",
                  wn_core_media_open(bare, video) == 0 &&
                      state_of(bare, video) == WN_MEDIA_STATE_PAUSED);
            wn_core_destroy(bare);
        }
    }

    /* Teardown drops every lease: no provider frame buffer survives. */
    wn_core_media_close(core, poster);
    wn_core_media_close(core, placeholder);
    wn_core_media_close(core, contain);
    wn_core_media_close(core, auto_node);
    check("closing every session releases the frame buffers",
          wn_media_test_live_frames() == 0);
    wn_core_destroy(core);
    check("core destroy releases the provider sessions",
          wn_media_test_live_frames() == 0);

    if (failures != 0) {
        printf("media_session_test: FAIL (%d checks, %d failed)\n", checks,
               failures);
        return 1;
    }
    printf("media_session_test: PASS (%d checks)\n", checks);
    return 0;
}
