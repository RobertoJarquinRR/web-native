/*
 * media_null.c - the built-in provider that is always present and never
 * matches (docs/media-v1.md §7.1).
 *
 * It exists so the provider chain is never empty and "no codec installed" is a
 * normal, typed path instead of a NULL. It has empty caps, refuses probe/open
 * with WN_MEDIA_UNSUPPORTED_CONTAINER and never reads or decodes anything.
 */
#include <string.h>

#include "wn_media.h"

static void null_caps(void *context, wn_media_caps *out)
{
    (void)context;
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
}

static wn_media_status null_probe(void *context, const char *path_utf8,
                                  wn_media_info *out)
{
    (void)context;
    (void)path_utf8;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->struct_size = (uint32_t)sizeof(*out);
    }
    return WN_MEDIA_UNSUPPORTED_CONTAINER;
}

static wn_media_status null_open(void *context, const wn_media_open_desc *desc,
                                 wn_media_session **out_session)
{
    (void)context;
    (void)desc;
    if (out_session != NULL) {
        *out_session = NULL;
    }
    return WN_MEDIA_UNSUPPORTED_CONTAINER;
}

static wn_media_status null_streams(void *context, wn_media_session *session,
                                    wn_media_stream_info *out, uint32_t capacity,
                                    uint32_t *out_count)
{
    (void)context;
    (void)session;
    (void)out;
    (void)capacity;
    if (out_count != NULL) {
        *out_count = 0;
    }
    return WN_MEDIA_UNSUPPORTED_CONTAINER;
}

static wn_media_status null_read(void *context, wn_media_session *session,
                                 wn_media_packet *out)
{
    (void)context;
    (void)session;
    (void)out;
    return WN_MEDIA_UNSUPPORTED_CONTAINER;
}

static wn_media_status null_seek(void *context, wn_media_session *session,
                                 int64_t pts_us, uint32_t flags)
{
    (void)context;
    (void)session;
    (void)pts_us;
    (void)flags;
    return WN_MEDIA_UNSUPPORTED_CONTAINER;
}

static wn_media_status null_decode(void *context, wn_media_session *session,
                                   const wn_media_packet *packet)
{
    (void)context;
    (void)session;
    (void)packet;
    return WN_MEDIA_UNSUPPORTED_CONTAINER;
}

static wn_media_status null_receive_video(void *context,
                                          wn_media_session *session,
                                          wn_video_frame *out)
{
    (void)context;
    (void)session;
    (void)out;
    return WN_MEDIA_UNSUPPORTED_CONTAINER;
}

static wn_media_status null_receive_audio(void *context,
                                          wn_media_session *session,
                                          wn_audio_frame *out)
{
    (void)context;
    (void)session;
    (void)out;
    return WN_MEDIA_UNSUPPORTED_CONTAINER;
}

static wn_media_status null_flush(void *context, wn_media_session *session)
{
    (void)context;
    (void)session;
    return WN_MEDIA_OK;
}

static wn_media_status null_close(void *context, wn_media_session *session)
{
    (void)context;
    (void)session;
    return WN_MEDIA_OK;
}

static void null_destroy(void *context)
{
    (void)context;
}

static const wn_media_provider_ops g_null_ops = {
    (uint32_t)sizeof(wn_media_provider_ops),
    WN_MEDIA_ABI_VERSION,
    "null",
    null_caps,
    null_probe,
    null_open,
    null_streams,
    null_read,
    null_seek,
    null_decode,
    null_receive_video,
    null_receive_audio,
    null_flush,
    null_close,
    null_destroy
};

const wn_media_provider_ops *wn_media_null_ops(void)
{
    return &g_null_ops;
}

void *wn_media_null_context(void)
{
    return NULL;
}
