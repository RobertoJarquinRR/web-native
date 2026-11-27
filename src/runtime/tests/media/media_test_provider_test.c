/*
 * media_test_provider_test.c - contract of the synthetic provider itself.
 *
 * Fixes RGBA8/dimensions/duration, strictly increasing PTS, the visual
 * determinism of a frame, seek placement, and the wn_frame_lease rule
 * (release exactly once, no buffer kept after the last release).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media/media_test.h"

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

static int read_frame(const wn_media_provider_ops *ops, void *ctx,
                      wn_media_session *session, wn_video_frame *frame)
{
    wn_media_packet packet;
    wn_media_status status;
    memset(&packet, 0, sizeof(packet));
    packet.struct_size = (uint32_t)sizeof(packet);
    status = ops->read(ctx, session, &packet);
    if (status != WN_MEDIA_OK) {
        return 0;
    }
    status = ops->decode(ctx, session, &packet);
    if (status != WN_MEDIA_OK) {
        return 0;
    }
    status = ops->receive_video(ctx, session, frame);
    return status == WN_MEDIA_OK;
}

int main(void)
{
    const wn_media_provider_ops *ops = wn_media_test_ops();
    wn_media_caps caps;
    wn_media_info info;
    wn_media_open_desc desc;
    wn_media_session *session = NULL;
    wn_video_frame frame;
    int64_t last_pts = -1;
    int i;
    int monotonic = 1;
    uint32_t pixel;
    int ok;

    checks = 0;
    failures = 0;

    check("provider name is test", strcmp(ops->name, "test") == 0);
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = (uint32_t)sizeof(caps);
    ops->caps(NULL, &caps);
    check("caps claim RGBA8", (caps.pixel_formats &
                               WN_MEDIA_BIT(WN_PIXEL_RGBA8)) != 0);
    check("caps claim plain seek", (caps.flags & WN_MEDIA_CAP_SEEK) != 0);

    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    check("probe accepts a path", ops->probe(NULL, "clips/x.mp4", &info) ==
                                     WN_MEDIA_OK);
    check("probe reports 10 s at 320x180",
          info.duration_us == WN_MEDIA_TEST_DURATION_US &&
              info.video.display_width == WN_MEDIA_TEST_WIDTH &&
              info.video.display_height == WN_MEDIA_TEST_HEIGHT);
    check("probe is seekable", (info.flags & WN_MEDIA_INFO_SEEKABLE) != 0);

    memset(&desc, 0, sizeof(desc));
    desc.struct_size = (uint32_t)sizeof(desc);
    desc.path_utf8 = "clips/x.mp4";
    desc.allowed_formats = WN_MEDIA_BIT(WN_PIXEL_RGBA8);
    check("open returns a session",
          ops->open(NULL, &desc, &session) == WN_MEDIA_OK && session != NULL);

    /* Read every packet: PTS must be strictly increasing. */
    for (i = 0; i < WN_MEDIA_TEST_FRAMES; ++i) {
        wn_media_packet packet;
        memset(&packet, 0, sizeof(packet));
        packet.struct_size = (uint32_t)sizeof(packet);
        if (ops->read(NULL, session, &packet) != WN_MEDIA_OK) {
            monotonic = 0;
            break;
        }
        if (last_pts >= 0 && packet.pts_us <= last_pts) {
            monotonic = 0;
        }
        last_pts = packet.pts_us;
        if (ops->decode(NULL, session, &packet) != WN_MEDIA_OK) {
            monotonic = 0;
            break;
        }
        memset(&frame, 0, sizeof(frame));
        frame.struct_size = (uint32_t)sizeof(frame);
        if (ops->receive_video(NULL, session, &frame) != WN_MEDIA_OK) {
            monotonic = 0;
            break;
        }
        frame.lease.release(frame.lease.context, frame.lease.object);
    }
    check("all 300 packets have strictly increasing PTS", monotonic);
    check("last PTS leaves room for the 10 s duration",
          last_pts > 9900000 && last_pts < WN_MEDIA_TEST_DURATION_US);
    {
        wn_media_packet eof;
        memset(&eof, 0, sizeof(eof));
        eof.struct_size = (uint32_t)sizeof(eof);
        check("the stream ends with EOF",
              ops->read(NULL, session, &eof) == WN_MEDIA_EOF);
    }

    /* Determinism: same index -> same pixels. */
    {
        unsigned char *copy = NULL;
        size_t bytes = (size_t)WN_MEDIA_TEST_WIDTH * WN_MEDIA_TEST_HEIGHT * 4u;
        memset(&frame, 0, sizeof(frame));
        ops->seek(NULL, session, 0, 0);
        check("seek(0) reads frame 0",
              read_frame(ops, NULL, session, &frame) == 1 &&
                  frame.pts_us == wn_media_test_pts_us(0));
        pixel = *(const uint32_t *)(const void *)(frame.plane[0].data +
                                                  (100 * 4 +
                                                   25 * frame.plane[0].stride));
        {
            uint32_t argb = wn_media_test_pixel(0, 100, 25);
            uint32_t rgba = (argb & 0xFF000000u) | ((argb & 0xFFu) << 16) |
                            (argb & 0xFF00u) | ((argb >> 16) & 0xFFu);
            check("frame 0 has the expected bar pixel", pixel == rgba);
        }
        copy = (unsigned char *)malloc(bytes);
        check("copied frame 0", copy != NULL);
        if (copy != NULL) {
            memcpy(copy, frame.plane[0].data, bytes);
        }
        frame.lease.release(frame.lease.context, frame.lease.object);
        memset(&frame, 0, sizeof(frame));
        ops->seek(NULL, session, 0, 0);
        check("second read of frame 0", read_frame(ops, NULL, session, &frame));
        check("frame 0 is byte-deterministic",
              copy != NULL && frame.plane[0].data != NULL &&
                  memcmp(copy, frame.plane[0].data, bytes) == 0);
        frame.lease.release(frame.lease.context, frame.lease.object);
        free(copy);
    }
    /* Seek: next read is the frame at or before the target. */
    ops->seek(NULL, session, 5000000, 0);
    memset(&frame, 0, sizeof(frame));
    check("seek(5s) reads the keyframe at or before the target",
          read_frame(ops, NULL, session, &frame) == 1 &&
              frame.pts_us == wn_media_test_pts_us(150));
    frame.lease.release(frame.lease.context, frame.lease.object);
    memset(&frame, 0, sizeof(frame));
    check("the next frame crosses the target",
          read_frame(ops, NULL, session, &frame) == 1 &&
              frame.pts_us >= 5000000);
    frame.lease.release(frame.lease.context, frame.lease.object);

    /* Lease accounting. */
    ops->seek(NULL, session, 0, 0);
    memset(&frame, 0, sizeof(frame));
    ok = read_frame(ops, NULL, session, &frame);
    check("a received frame holds one live buffer",
          ok && wn_media_test_live_frames() == 1);
    frame.lease.release(frame.lease.context, frame.lease.object);
    check("release drops the live buffer", wn_media_test_live_frames() == 0);

    check("flush clears the pending frame",
          ops->flush(NULL, session) == WN_MEDIA_OK);
    check("close frees the provider session",
          ops->close(NULL, session) == WN_MEDIA_OK);
    ops->destroy(NULL);

    if (failures != 0) {
        printf("media_test_provider_test: FAIL (%d checks, %d failed)\n",
               checks, failures);
        return 1;
    }
    printf("media_test_provider_test: PASS (%d checks)\n", checks);
    return 0;
}
