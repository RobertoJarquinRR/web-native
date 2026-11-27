/*
 * media_test.c - deterministic synthetic RGBA video provider (test only).
 *
 * Contract fixture for the session driver and the video sink:
 *   - 10 s, 30 fps, RGBA8, 320x180, monotonically increasing PTS;
 *   - eight color bars, the frame index encoded in ten 10x10 bit blocks at the
 *     top-left and a 24x20 marker that moves with the frame index;
 *   - seek accepts any PTS and places the next read on the frame at or before
 *     it (the runtime drops frames below the target);
 *   - every received frame owns its buffer through wn_frame_lease; release()
 *     is a plain free, exactly once.
 *
 * This file is never added to the product sources or CMake targets.
 */
#include <stdlib.h>
#include <string.h>

#include "media_test.h"

#define TEST_BAR_COUNT 8
#define TEST_BAR_WIDTH (WN_MEDIA_TEST_WIDTH / TEST_BAR_COUNT)
#define TEST_BIT_COUNT 10
#define TEST_BIT_SIZE 10
#define TEST_BIT_STEP 12
#define TEST_MARKER_W 24
#define TEST_MARKER_H 20

static int g_live_frames = 0;

int64_t wn_media_test_pts_us(int frame_index)
{
    if (frame_index < 0) {
        frame_index = 0;
    }
    if (frame_index >= WN_MEDIA_TEST_FRAMES) {
        frame_index = WN_MEDIA_TEST_FRAMES - 1;
    }
    return (int64_t)frame_index * WN_MEDIA_TEST_DURATION_US /
           WN_MEDIA_TEST_FRAMES;
}

int64_t wn_media_test_duration_us(int frame_index)
{
    int64_t start = wn_media_test_pts_us(frame_index);
    int64_t end = (frame_index + 1 >= WN_MEDIA_TEST_FRAMES)
                      ? WN_MEDIA_TEST_DURATION_US
                      : wn_media_test_pts_us(frame_index + 1);
    return end - start;
}

static uint32_t bar_color(int bar)
{
    static const uint32_t colors[TEST_BAR_COUNT] = {
        0xFFFFFFFFu, /* white */
        0xFFFFFF00u, /* yellow */
        0xFF00FFFFu, /* cyan */
        0xFF00FF00u, /* green */
        0xFFFF00FFu, /* magenta */
        0xFFFF0000u, /* red */
        0xFF0000FFu, /* blue */
        0xFF000000u  /* black */
    };
    return colors[bar % TEST_BAR_COUNT];
}

uint32_t wn_media_test_pixel(int frame_index, int x, int y)
{
    int bar;
    int marker_x;
    if (frame_index < 0) {
        frame_index = 0;
    }
    if (frame_index >= WN_MEDIA_TEST_FRAMES) {
        frame_index = WN_MEDIA_TEST_FRAMES - 1;
    }
    if (x < 0 || x >= WN_MEDIA_TEST_WIDTH ||
        y < 0 || y >= WN_MEDIA_TEST_HEIGHT) {
        return 0;
    }
    /* Frame index in the top-left bit blocks. */
    if (y < TEST_BIT_SIZE) {
        int bit = x / TEST_BIT_STEP;
        if (bit < TEST_BIT_COUNT &&
            x - bit * TEST_BIT_STEP < TEST_BIT_SIZE) {
            return ((frame_index >> bit) & 1) ? 0xFFFFFFFFu : 0xFF000000u;
        }
    }
    /* Moving marker near the bottom edge. */
    marker_x = frame_index * (WN_MEDIA_TEST_WIDTH - TEST_MARKER_W) /
               (WN_MEDIA_TEST_FRAMES - 1);
    if (x >= marker_x && x < marker_x + TEST_MARKER_W &&
        y >= WN_MEDIA_TEST_HEIGHT - TEST_MARKER_H &&
        y < WN_MEDIA_TEST_HEIGHT) {
        return 0xFFFF8000u; /* orange, unlike every bar */
    }
    bar = x / TEST_BAR_WIDTH;
    return bar_color(bar);
}

/* ------------------------------------------------------------- provider */

typedef struct {
    int next_frame; /* index of the next packet read() */
    int pending;    /* decoded frame waiting for receive_video, -1 none */
    int closed;
} TestSession;

static void test_release(void *context, void *object)
{
    (void)context;
    if (object != NULL) {
        free(object);
        g_live_frames -= 1;
    }
}

int wn_media_test_live_frames(void)
{
    return g_live_frames;
}

static void test_caps(void *context, wn_media_caps *out)
{
    (void)context;
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->flags = WN_MEDIA_CAP_SEEK;
    out->containers = WN_MEDIA_BIT(WN_CONTAINER_MP4);
    out->video_codecs = WN_MEDIA_BIT(WN_CODEC_H264);
    out->pixel_formats = WN_MEDIA_BIT(WN_PIXEL_RGBA8);
    out->max_video_width = 1920;
    out->max_video_height = 1080;
}

static void fill_stream(wn_media_stream_info *stream)
{
    memset(stream, 0, sizeof(*stream));
    stream->struct_size = (uint32_t)sizeof(*stream);
    stream->type = WN_STREAM_VIDEO;
    stream->codec = WN_CODEC_H264;
    stream->index = 0;
    stream->duration_us = WN_MEDIA_TEST_DURATION_US;
    stream->width = WN_MEDIA_TEST_WIDTH;
    stream->height = WN_MEDIA_TEST_HEIGHT;
    stream->display_width = WN_MEDIA_TEST_WIDTH;
    stream->display_height = WN_MEDIA_TEST_HEIGHT;
    stream->rotation_deg = 0;
    stream->color = WN_MEDIA_COLOR(WN_MATRIX_BT709, WN_RANGE_FULL,
                                   WN_PRIMARIES_BT709, WN_TRANSFER_SDR);
}

static wn_media_status test_probe(void *context, const char *path_utf8,
                                  wn_media_info *out)
{
    (void)context;
    if (path_utf8 == NULL || path_utf8[0] == '\0') {
        return WN_MEDIA_IO;
    }
    if (out == NULL) {
        return WN_MEDIA_OK;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->flags = WN_MEDIA_INFO_SEEKABLE;
    out->container = WN_CONTAINER_MP4;
    out->stream_count = 1;
    out->duration_us = WN_MEDIA_TEST_DURATION_US;
    fill_stream(&out->video);
    return WN_MEDIA_OK;
}

static wn_media_status test_open(void *context, const wn_media_open_desc *desc,
                                 wn_media_session **out_session)
{
    TestSession *session;
    (void)context;
    if (out_session == NULL || desc == NULL || desc->path_utf8 == NULL) {
        return WN_MEDIA_INTERNAL;
    }
    if (desc->allowed_formats != 0 &&
        (desc->allowed_formats & WN_MEDIA_BIT(WN_PIXEL_RGBA8)) == 0) {
        return WN_MEDIA_UNSUPPORTED_CODEC;
    }
    session = (TestSession *)calloc(1, sizeof(*session));
    if (session == NULL) {
        return WN_MEDIA_OOM;
    }
    session->pending = -1;
    *out_session = (wn_media_session *)session;
    return WN_MEDIA_OK;
}

static wn_media_status test_streams(void *context, wn_media_session *session,
                                    wn_media_stream_info *out, uint32_t capacity,
                                    uint32_t *out_count)
{
    (void)context;
    (void)session;
    if (out_count != NULL) {
        *out_count = capacity > 0 ? 1u : 0u;
    }
    if (out != NULL && capacity > 0) {
        fill_stream(&out[0]);
    }
    return WN_MEDIA_OK;
}

static wn_media_status test_read(void *context, wn_media_session *session,
                                 wn_media_packet *out)
{
    TestSession *state = (TestSession *)session;
    (void)context;
    if (state == NULL || out == NULL || state->closed) {
        return WN_MEDIA_CANCELLED;
    }
    if (state->next_frame >= WN_MEDIA_TEST_FRAMES) {
        return WN_MEDIA_EOF;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->stream = 0;
    out->type = WN_STREAM_VIDEO;
    out->flags = state->next_frame == 0 ? WN_PACKET_KEY : 0u;
    out->pts_us = wn_media_test_pts_us(state->next_frame);
    out->duration_us = wn_media_test_duration_us(state->next_frame);
    out->token = (void *)(uintptr_t)(state->next_frame + 1);
    state->next_frame += 1;
    return WN_MEDIA_OK;
}

static wn_media_status test_seek(void *context, wn_media_session *session,
                                 int64_t pts_us, uint32_t flags)
{
    TestSession *state = (TestSession *)session;
    (void)context;
    (void)flags;
    if (state == NULL || state->closed) {
        return WN_MEDIA_CANCELLED;
    }
    if (pts_us < 0) {
        pts_us = 0;
    }
    if (pts_us > WN_MEDIA_TEST_DURATION_US) {
        pts_us = WN_MEDIA_TEST_DURATION_US;
    }
    /* Frame at or before the target: the runtime drops frames < target. */
    state->next_frame = (int)(pts_us * WN_MEDIA_TEST_FRAMES /
                              WN_MEDIA_TEST_DURATION_US);
    if (state->next_frame >= WN_MEDIA_TEST_FRAMES) {
        state->next_frame = WN_MEDIA_TEST_FRAMES - 1;
    }
    state->pending = -1;
    return WN_MEDIA_OK;
}

static wn_media_status test_decode(void *context, wn_media_session *session,
                                   const wn_media_packet *packet)
{
    TestSession *state = (TestSession *)session;
    int index;
    (void)context;
    if (state == NULL || packet == NULL || state->closed) {
        return WN_MEDIA_CANCELLED;
    }
    index = (int)(uintptr_t)packet->token - 1;
    if (index < 0 || index >= WN_MEDIA_TEST_FRAMES) {
        return WN_MEDIA_CORRUPT;
    }
    state->pending = index;
    return WN_MEDIA_OK;
}

static wn_media_status test_receive_video(void *context,
                                          wn_media_session *session,
                                          wn_video_frame *out)
{
    TestSession *state = (TestSession *)session;
    unsigned char *pixels;
    int x, y;
    (void)context;
    if (state == NULL || out == NULL || state->closed) {
        return WN_MEDIA_CANCELLED;
    }
    if (state->pending < 0) {
        return WN_MEDIA_AGAIN;
    }
    pixels = (unsigned char *)malloc((size_t)WN_MEDIA_TEST_WIDTH *
                                     WN_MEDIA_TEST_HEIGHT * 4u);
    if (pixels == NULL) {
        return WN_MEDIA_OOM;
    }
    for (y = 0; y < WN_MEDIA_TEST_HEIGHT; ++y) {
        uint32_t *row = (uint32_t *)(pixels +
                                     (size_t)y * WN_MEDIA_TEST_WIDTH * 4u);
        for (x = 0; x < WN_MEDIA_TEST_WIDTH; ++x) {
            /* Memory order R,G,B,A; the helper returns 0xAARRGGBB. */
            uint32_t argb = wn_media_test_pixel(state->pending, x, y);
            row[x] = ((argb >> 16) & 0xFFu) | (argb & 0xFF00u) |
                     ((argb & 0xFFu) << 16) | (argb & 0xFF000000u);
        }
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->format = WN_PIXEL_RGBA8;
    out->coded_width = WN_MEDIA_TEST_WIDTH;
    out->coded_height = WN_MEDIA_TEST_HEIGHT;
    out->display_width = WN_MEDIA_TEST_WIDTH;
    out->display_height = WN_MEDIA_TEST_HEIGHT;
    out->rotation_deg = 0;
    out->planes = 1;
    out->plane[0].data = pixels;
    out->plane[0].stride = WN_MEDIA_TEST_WIDTH * 4;
    out->plane[0].width = WN_MEDIA_TEST_WIDTH;
    out->plane[0].height = WN_MEDIA_TEST_HEIGHT;
    out->pts_us = wn_media_test_pts_us(state->pending);
    out->duration_us = wn_media_test_duration_us(state->pending);
    out->flags = 0;
    out->color = WN_MEDIA_COLOR(WN_MATRIX_BT709, WN_RANGE_FULL,
                                WN_PRIMARIES_BT709, WN_TRANSFER_SDR);
    out->native_texture = NULL;
    out->native_kind = WN_NATIVE_NONE;
    out->native_slice = 0;
    out->lease.object = pixels;
    out->lease.release = test_release;
    out->lease.context = NULL;
    g_live_frames += 1;
    state->pending = -1;
    return WN_MEDIA_OK;
}

static wn_media_status test_receive_audio(void *context,
                                          wn_media_session *session,
                                          wn_audio_frame *out)
{
    (void)context;
    (void)session;
    (void)out;
    return WN_MEDIA_AGAIN;
}

static wn_media_status test_flush(void *context, wn_media_session *session)
{
    TestSession *state = (TestSession *)session;
    (void)context;
    if (state != NULL) {
        state->pending = -1;
    }
    return WN_MEDIA_OK;
}

static wn_media_status test_close(void *context, wn_media_session *session)
{
    (void)context;
    if (session != NULL) {
        TestSession *state = (TestSession *)session;
        state->closed = 1;
        free(state);
    }
    return WN_MEDIA_OK;
}

static void test_destroy(void *context)
{
    (void)context;
}

static const wn_media_provider_ops g_test_ops = {
    (uint32_t)sizeof(wn_media_provider_ops),
    WN_MEDIA_ABI_VERSION,
    "test",
    test_caps,
    test_probe,
    test_open,
    test_streams,
    test_read,
    test_seek,
    test_decode,
    test_receive_video,
    test_receive_audio,
    test_flush,
    test_close,
    test_destroy
};

const wn_media_provider_ops *wn_media_test_ops(void)
{
    return &g_test_ops;
}
