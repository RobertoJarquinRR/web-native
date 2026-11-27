/*
 * media_core.c - the per-node media session driver.
 *
 * E4 of docs/media-v1.md: one session per live `video` node, the documented
 * state machine (IDLE -> OPENING -> PAUSED/PLAYING -> BUFFERING -> SEEKING ->
 * ENDED/ERROR), an inline media loop and the decoded-frame path to the
 * renderer's video sink. The gate is the inline mode (WN_MEDIA_THREADS=0):
 * every provider call happens from `wn_core_media_tick` on the UI thread, so
 * playback is deterministic and no locks exist on the UI path.
 *
 * The renderer sink is optional: a core without one still drives the clock and
 * the state machine, it simply cannot turn a decoded RGBA frame into a texture.
 * media_null remains the ordinary "no provider installed" path.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/wn_core_internal.h"
#include "wn_internal.h"
#include "wn_media.h"

#ifndef WN_MEDIA_THREADS
#define WN_MEDIA_THREADS 0
#endif

/* 250 ms: at most four PROGRESS events per second of media time (§3.8). */
#define WN_MEDIA_PROGRESS_US 250000
/* 3 decoded frames, drop the oldest (§3.6). */
#define WN_MEDIA_VIDEO_QUEUE 3
/* Default vsync interval while the renderer does not publish one (§3.7). */
#define WN_MEDIA_DEFAULT_VSYNC_US 16667
#define WN_MEDIA_MAX_DIM 4096

static WnMediaNode *media_node(wn_core *core, int node)
{
    if (core == NULL || !wn_core_node_is_alive(core, node)) {
        return NULL;
    }
    if (core->nodes[node].kind != R2_NODE_VIDEO) {
        return NULL;
    }
    return &core->media[node];
}

int wn_core_node_is_video(const wn_core *core, int node)
{
    return core != NULL && wn_core_node_is_alive(core, node) &&
           core->nodes[node].kind == R2_NODE_VIDEO;
}

const char *wn_core_node_media_src(const wn_core *core, int node)
{
    if (!wn_core_node_is_video(core, node)) {
        return NULL;
    }
    if (core->media[node].source_override != NULL) {
        return core->media[node].source_override;
    }
    return core->nodes[node].media_src;
}

const char *wn_core_node_media_poster(const wn_core *core, int node)
{
    if (!wn_core_node_is_video(core, node)) {
        return NULL;
    }
    return core->nodes[node].media_poster;
}

/* ------------------------------------------------------------- clock */

int64_t wn_core_media_now_us(const wn_core *core)
{
    if (core != NULL && core->media_now_us != NULL) {
        return core->media_now_us(core->media_now_context);
    }
    return (int64_t)(wn_now_ms() * 1000.0);
}

void wn_core_set_media_clock(wn_core *core, int64_t (*now_us)(void *context),
                             void *context)
{
    if (core == NULL) {
        return;
    }
    core->media_now_us = now_us;
    core->media_now_context = context;
}

void wn_core_set_media_sink(wn_core *core, const wn_video_sink_ops *ops,
                            void *context)
{
    if (core == NULL) {
        return;
    }
    core->media_sink = ops;
    core->media_sink_context = context;
}

/* ------------------------------------------------------------- events */

static void media_queue_event(wn_core *core, int node, int type, int state,
                              int error, int64_t position_us,
                              int64_t duration_us, const char *detail)
{
    WnMediaPendingEvent *slot;
    int index;
    if (core == NULL) {
        return;
    }
    if (core->media_event_count == WN_MEDIA_EVENT_QUEUE) {
        /* Drop the oldest, matching the event-ring policy in the contract. */
        core->media_event_head =
            (core->media_event_head + 1) % WN_MEDIA_EVENT_QUEUE;
        core->media_event_count -= 1;
    }
    index = core->media_event_tail;
    slot = &core->media_events[index];
    memset(slot, 0, sizeof(*slot));
    slot->node = node;
    slot->type = type;
    slot->state = state;
    slot->error = error;
    slot->position_us = position_us;
    slot->duration_us = duration_us;
    if (detail != NULL) {
        snprintf(slot->detail, sizeof(slot->detail), "%s", detail);
    }
    core->media_event_tail = (index + 1) % WN_MEDIA_EVENT_QUEUE;
    core->media_event_count += 1;
}

int wn_core_media_take_event(wn_core *core, int *node_index, int *type,
                             int *state, int *error, int64_t *position_us,
                             int64_t *duration_us, const char **detail)
{
    WnMediaPendingEvent *slot;
    int index;
    if (core == NULL || core->media_event_count == 0) {
        return 0;
    }
    index = core->media_event_head;
    slot = &core->media_events[index];
    if (node_index != NULL) {
        *node_index = slot->node;
    }
    if (type != NULL) {
        *type = slot->type;
    }
    if (state != NULL) {
        *state = slot->state;
    }
    if (error != NULL) {
        *error = slot->error;
    }
    if (position_us != NULL) {
        *position_us = slot->position_us;
    }
    if (duration_us != NULL) {
        *duration_us = slot->duration_us;
    }
    if (detail != NULL) {
        *detail = slot->detail;
    }
    core->media_event_head = (index + 1) % WN_MEDIA_EVENT_QUEUE;
    core->media_event_count -= 1;
    return 1;
}

/* ----------------------------------------------------------- registry */

int wn_core_register_media_provider(wn_core *core,
                                    const wn_media_provider_ops *ops,
                                    void *context, int32_t priority)
{
    if (core == NULL || core->media_registry == NULL) {
        wn_set_error("media: no live core registry");
        return -1;
    }
    return wn_media_registry_add(core->media_registry, ops, context, priority);
}

int wn_core_unregister_media_provider(wn_core *core,
                                      const wn_media_provider_ops *ops,
                                      void *context)
{
    if (core == NULL || core->media_registry == NULL) {
        wn_set_error("media: no live core registry");
        return -1;
    }
    return wn_media_registry_remove(core->media_registry, ops, context);
}

int wn_core_load_media_provider(wn_core *core, const char *dll_path)
{
    if (core == NULL || core->media_registry == NULL) {
        wn_set_error("media: no live core registry");
        return -1;
    }
    return wn_media_registry_load(core->media_registry, dll_path);
}

int wn_core_media_provider_count(const wn_core *core)
{
    if (core == NULL || core->media_registry == NULL) {
        return 0;
    }
    return wn_media_registry_count(core->media_registry);
}

const wn_media_provider_ops *wn_core_media_provider_at(
    const wn_core *core, int index, void **context, int32_t *priority)
{
    if (core == NULL || core->media_registry == NULL) {
        return NULL;
    }
    return wn_media_registry_at(core->media_registry, index, context, priority);
}

int wn_core_media_provider_is_null(const wn_core *core, int index)
{
    if (core == NULL || core->media_registry == NULL) {
        return 0;
    }
    return wn_media_registry_is_null(core->media_registry, index);
}

struct wn_media_registry *wn_core_detach_media_registry(wn_core *core)
{
    struct wn_media_registry *registry;
    if (core == NULL) {
        return NULL;
    }
    registry = core->media_registry;
    core->media_registry = NULL;
    return registry;
}

void wn_core_attach_media_registry(wn_core *core,
                                   struct wn_media_registry *registry)
{
    if (core == NULL) {
        return;
    }
    if (core->media_registry != NULL && core->media_registry != registry) {
        wn_media_registry_destroy(core->media_registry);
    }
    core->media_registry = registry;
}

/* ------------------------------------------------------- session state */

static void media_frame_release(WnMediaFrameSlot *slot)
{
    if (slot->frame.lease.release != NULL) {
        slot->frame.lease.release(slot->frame.lease.context,
                                  slot->frame.lease.object);
    }
    memset(&slot->frame, 0, sizeof(slot->frame));
    slot->serial = 0;
    slot->used = 0;
}

static void media_release_texture(wn_core *core, WnMediaNode *m)
{
    if (m->frame_texture != 0 && core->media_sink != NULL &&
        core->media_sink->release_texture != NULL) {
        core->media_sink->release_texture(core->media_sink_context,
                                          m->frame_texture);
    }
    m->frame_texture = 0;
    m->frame_format = 0;
    m->frame_color = 0;
    m->frame_width = 0;
    m->frame_height = 0;
    m->frame_pts_us = 0;
    m->frame_duration_us = 0;
    m->frame_interval_us = 0;
    m->first_frame_seen = 0;
}

static void media_clear_queue(WnMediaSession *s)
{
    int i;
    for (i = 0; i < s->frame_count; ++i) {
        media_frame_release(&s->frames[i]);
    }
    s->frame_count = 0;
}

static void media_session_destroy(wn_core *core, WnMediaNode *m)
{
    WnMediaSession *s = m->session;
    (void)core;
    if (s == NULL) {
        return;
    }
    if (s->session != NULL && s->ops != NULL && s->ops->close != NULL) {
        s->ops->close(s->context, s->session);
    }
    media_clear_queue(s);
    free(s);
    m->session = NULL;
}

/* Silent close: IDLE and no frame. Used by close/set_source/teardown. */
static void media_reset_node(wn_core *core, WnMediaNode *m)
{
    media_session_destroy(core, m);
    media_release_texture(core, m);
    m->state = WN_MEDIA_STATE_IDLE;
    m->error = WN_MEDIA_OK;
    m->position_us = 0;
    m->duration_us = 0;
    m->stream_flags = 0;
    m->auto_open = 0;
    m->intrinsic_w = 0.0f;
    m->intrinsic_h = 0.0f;
    m->presented = 0;
    m->dropped = 0;
}

static void media_fail(wn_core *core, int node, WnMediaNode *m, int status,
                       const char *detail)
{
    media_session_destroy(core, m);
    m->state = WN_MEDIA_STATE_ERROR;
    m->error = status;
    m->auto_open = 0;
    m->position_us = 0;
    media_queue_event(core, node, WN_MEDIA_EVENT_ERROR, WN_MEDIA_STATE_ERROR,
                      status, m->position_us, m->duration_us, detail);
    core->dirty = 1;
}

static void media_state_event(wn_core *core, int node, WnMediaNode *m)
{
    media_queue_event(core, node, WN_MEDIA_EVENT_STATE, m->state, WN_MEDIA_OK,
                      m->position_us, m->duration_us, NULL);
}

/* Drops the events still queued for one node. A closed/source-replaced session
 * must not deliver a stale PROGRESS or FIRST_FRAME after close (§3.8). */
static void media_purge_events(wn_core *core, int node)
{
    WnMediaPendingEvent kept[WN_MEDIA_EVENT_QUEUE];
    int count = 0;
    int i;
    if (core == NULL || core->media_event_count == 0) {
        return;
    }
    for (i = 0; i < core->media_event_count; ++i) {
        int index = (core->media_event_head + i) % WN_MEDIA_EVENT_QUEUE;
        if (core->media_events[index].node == node) {
            continue;
        }
        kept[count++] = core->media_events[index];
    }
    for (i = 0; i < count; ++i) {
        int index = (core->media_event_head + i) % WN_MEDIA_EVENT_QUEUE;
        core->media_events[index] = kept[i];
    }
    core->media_event_tail = (core->media_event_head + count) %
                             WN_MEDIA_EVENT_QUEUE;
    core->media_event_count = count;
}

static int64_t media_clock_us(const wn_core *core, WnMediaNode *m,
                              WnMediaSession *s, int64_t now_us)
{
    int64_t position;
    (void)core;
    if (s == NULL) {
        return m->position_us;
    }
    if (m->state != WN_MEDIA_STATE_PLAYING) {
        return m->position_us;
    }
    position = s->anchor_position_us + (now_us - s->anchor_wall_us);
    if (position < 0) {
        position = 0;
    }
    if (m->duration_us > 0 && position > m->duration_us) {
        position = m->duration_us;
    }
    return position;
}

int64_t wn_core_media_position_us(const wn_core *core, int node)
{
    const WnMediaNode *m;
    WnMediaSession *s;
    if (!wn_core_node_is_video(core, node)) {
        return 0;
    }
    m = &core->media[node];
    s = m->session;
    return media_clock_us(core, (WnMediaNode *)m, s, wn_core_media_now_us(core));
}

/* ------------------------------------------------------- provider open */

static void media_apply_open_info(wn_core *core, WnMediaNode *m,
                                  const wn_media_info *info)
{
    float iw, ih;
    m->duration_us = info != NULL ? info->duration_us : 0;
    m->stream_flags = info != NULL ? info->flags : 0;
    if (info == NULL || info->video.type != WN_STREAM_VIDEO) {
        return;
    }
    iw = (float)(info->video.display_width > 0 ? info->video.display_width
                                               : info->video.width);
    ih = (float)(info->video.display_height > 0 ? info->video.display_height
                                                : info->video.height);
    if (iw <= 0.0f || ih <= 0.0f) {
        return;
    }
    if (iw != m->intrinsic_w || ih != m->intrinsic_h) {
        m->intrinsic_w = iw;
        m->intrinsic_h = ih;
        /* Intrinsic size is layout input: dirty once per new value (§4.1). */
        core->needs_layout = 1;
        core->dirty = 1;
    }
}

/* Walks the deterministic chain. Only UNSUPPORTED_* moves to the next provider;
 * IO/CORRUPT/OOM are the same file and are reported immediately (§3.10). */
static int media_open_chain(wn_core *core, const char *src, WnMediaNode *m,
                            WnMediaSession *s, wn_media_info *info_out,
                            char *detail, size_t detail_cap, int *status_out)
{
    int count = wn_core_media_provider_count(core);
    int index;
    int saw_error = 0;
    int error_status = WN_MEDIA_UNSUPPORTED_CONTAINER;

    for (index = 0; index < count; ++index) {
        void *context = NULL;
        int32_t priority = 0;
        const wn_media_provider_ops *ops =
            wn_core_media_provider_at(core, index, &context, &priority);
        wn_media_info info;
        wn_media_open_desc desc;
        wn_media_status status;
        (void)priority;
        if (ops == NULL || ops->probe == NULL || ops->open == NULL) {
            continue;
        }
        memset(&info, 0, sizeof(info));
        info.struct_size = (uint32_t)sizeof(info);
        status = ops->probe(context, src, &info);
        if (status == WN_MEDIA_UNSUPPORTED_CONTAINER ||
            status == WN_MEDIA_UNSUPPORTED_CODEC) {
            continue;
        }
        if (status == WN_MEDIA_AGAIN) {
            continue;
        }
        if (status != WN_MEDIA_OK) {
            saw_error = 1;
            error_status = (int)status;
            snprintf(detail, detail_cap, "media probe failed for %s (%s)",
                     src, ops->name != NULL ? ops->name : "provider");
            *status_out = error_status;
            return -1;
        }
        memset(&desc, 0, sizeof(desc));
        desc.struct_size = (uint32_t)sizeof(desc);
        desc.path_utf8 = src;
        desc.flags = WN_MEDIA_OPEN_AUDIO_OFF |
                     (m->loop ? WN_MEDIA_OPEN_LOOP : 0u);
        if (core->media_sink != NULL && core->media_sink->formats != NULL) {
            desc.allowed_formats =
                core->media_sink->formats(core->media_sink_context);
        }
        desc.native_kind = 0; /* E4: software RGBA only */
        desc.native_device = NULL;
        desc.max_width = WN_MEDIA_MAX_DIM;
        desc.max_height = WN_MEDIA_MAX_DIM;
        desc.audio_sample_rate = 0;
        desc.audio_channels = 0;
        status = ops->open(context, &desc, &s->session);
        if (status == WN_MEDIA_OK && s->session != NULL) {
            s->ops = ops;
            s->context = context;
            if (info_out != NULL) {
                *info_out = info;
            }
            snprintf(detail, detail_cap, "%s",
                     ops->name != NULL ? ops->name : "provider");
            *status_out = WN_MEDIA_OK;
            return 0;
        }
        s->session = NULL;
        if (status == WN_MEDIA_UNSUPPORTED_CONTAINER ||
            status == WN_MEDIA_UNSUPPORTED_CODEC) {
            continue;
        }
        saw_error = 1;
        error_status = (int)status;
        snprintf(detail, detail_cap, "media open failed for %s (%s, status %d)",
                 src, ops->name != NULL ? ops->name : "provider", (int)status);
        *status_out = error_status;
        return -1;
    }
    if (saw_error) {
        *status_out = error_status;
        return -1;
    }
    snprintf(detail, detail_cap, "no media provider claims %s", src);
    *status_out = WN_MEDIA_UNSUPPORTED_CONTAINER;
    return -1;
}

static void media_start_playing(wn_core *core, int node, WnMediaNode *m,
                                WnMediaSession *s, int64_t now_us)
{
    (void)core;
    (void)node;
    s->anchor_position_us = m->position_us;
    s->anchor_wall_us = now_us;
    s->last_progress_position_us = m->position_us;
    m->state = WN_MEDIA_STATE_PLAYING;
}

static void media_begin_open(wn_core *core, int node, WnMediaNode *m)
{
    const char *src = wn_core_node_media_src(core, node);
    WnMediaSession *s;
    wn_media_info info;
    char detail[WN_MEDIA_EVENT_DETAIL];
    int status = 0;
    int64_t now_us;

    m->auto_open = 0;
    if (src == NULL || src[0] == '\0') {
        media_fail(core, node, m, WN_MEDIA_UNSUPPORTED_CONTAINER,
                   "video node has no source (host-bound player node)");
        return;
    }
    media_purge_events(core, node);
    /* A fresh request drops the previous frame; the poster is painted again. */
    media_session_destroy(core, m);
    media_release_texture(core, m);
    m->state = WN_MEDIA_STATE_OPENING;
    m->error = WN_MEDIA_OK;
    m->position_us = 0;
    m->duration_us = 0;
    m->stream_flags = 0;
    m->intrinsic_w = 0.0f;
    m->intrinsic_h = 0.0f;
    m->presented = 0;
    m->dropped = 0;

    s = (WnMediaSession *)calloc(1, sizeof(*s));
    if (s == NULL) {
        media_fail(core, node, m, WN_MEDIA_OOM,
                   "out of memory allocating the media session");
        return;
    }
    m->session = s;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t)sizeof(info);
    if (media_open_chain(core, src, m, s, &info, detail, sizeof(detail),
                         &status) != 0) {
        media_fail(core, node, m, status, detail);
        return;
    }
    media_apply_open_info(core, m, &info);
    m->state = WN_MEDIA_STATE_PAUSED;
    now_us = wn_core_media_now_us(core);
    s->anchor_position_us = 0;
    s->anchor_wall_us = now_us;
    s->last_progress_position_us = 0;
    media_queue_event(core, node, WN_MEDIA_EVENT_OPENED, m->state, WN_MEDIA_OK,
                      0, m->duration_us, detail);
    core->dirty = 1;

    if ((core->nodes[node].media_flags & WN_MEDIA_FLAG_AUTOPLAY) != 0u) {
        media_start_playing(core, node, m, s, now_us);
        media_state_event(core, node, m);
    } else if (core->nodes[node].media_preload == WN_MEDIA_PRELOAD_AUTO) {
        s->decode_one = 1;
    }
}

/* ----------------------------------------------------------- decoding */

static int media_pump_decode(wn_core *core, int node, WnMediaNode *m,
                             WnMediaSession *s, int seeking)
{
    int guard = 0;
    int produced = 0;
    if (s->ops == NULL || s->session == NULL) {
        return 0;
    }
    while (s->frame_count < WN_MEDIA_VIDEO_QUEUE && !s->eof && guard++ < 32) {
        wn_media_packet packet;
        wn_media_status status;
        int receive_guard = 0;
        memset(&packet, 0, sizeof(packet));
        packet.struct_size = (uint32_t)sizeof(packet);
        status = s->ops->read(s->context, s->session, &packet);
        if (status == WN_MEDIA_AGAIN) {
            break;
        }
        if (status == WN_MEDIA_EOF) {
            s->eof = 1;
            break;
        }
        if (status == WN_MEDIA_CANCELLED) {
            return produced;
        }
        if (status != WN_MEDIA_OK) {
            media_fail(core, node, m, (int)status, "media read failed");
            return -1;
        }
        if (packet.type != 0 && packet.type != WN_STREAM_VIDEO) {
            continue; /* E4 has no audio path */
        }
        status = s->ops->decode(s->context, s->session, &packet);
        if (status == WN_MEDIA_AGAIN) {
            continue;
        }
        if (status == WN_MEDIA_EOF) {
            s->eof = 1;
            break;
        }
        if (status == WN_MEDIA_CANCELLED) {
            return produced;
        }
        if (status != WN_MEDIA_OK) {
            media_fail(core, node, m, (int)status, "media decode failed");
            return -1;
        }
        while (s->frame_count < WN_MEDIA_VIDEO_QUEUE && receive_guard++ < 8) {
            WnMediaFrameSlot *slot;
            wn_video_frame frame;
            memset(&frame, 0, sizeof(frame));
            frame.struct_size = (uint32_t)sizeof(frame);
            status = s->ops->receive_video(s->context, s->session, &frame);
            if (status == WN_MEDIA_AGAIN) {
                break;
            }
            if (status == WN_MEDIA_EOF) {
                s->eof = 1;
                break;
            }
            if (status == WN_MEDIA_CANCELLED) {
                return produced;
            }
            if (status != WN_MEDIA_OK) {
                media_fail(core, node, m, (int)status,
                           "media receive_video failed");
                return -1;
            }
            if (seeking && frame.pts_us < s->seek_target_us) {
                /* The provider may return the keyframe before the target. */
                if (frame.lease.release != NULL) {
                    frame.lease.release(frame.lease.context, frame.lease.object);
                }
                m->dropped += 1;
                continue;
            }
            slot = &s->frames[s->frame_count];
            slot->frame = frame;
            slot->serial = s->serial;
            slot->used = 1;
            s->frame_count += 1;
            produced = 1;
        }
    }
    return produced;
}

/* Drops frames already behind the presentation window and counts them. */
static void media_drop_late(WnMediaNode *m, WnMediaSession *s, int64_t clock_us)
{
    int64_t late_before;
    int keep = 0;
    int i;
    if (m->frame_interval_us <= 0) {
        return;
    }
    late_before = clock_us - 2 * m->frame_interval_us;
    for (i = 0; i < s->frame_count; ++i) {
        if (s->frames[i].frame.pts_us < late_before) {
            media_frame_release(&s->frames[i]);
            m->dropped += 1;
        } else {
            if (keep != i) {
                s->frames[keep] = s->frames[i];
                memset(&s->frames[i], 0, sizeof(s->frames[i]));
            }
            keep += 1;
        }
    }
    s->frame_count = keep;
}

int wn_core_media_present(wn_core *core)
{
    int index;
    int uploaded = 0;
    int64_t now_us;
    if (core == NULL) {
        return 0;
    }
    now_us = wn_core_media_now_us(core);
    for (index = 0; index < core->node_count; ++index) {
        WnMediaNode *m;
        WnMediaSession *s;
        int64_t clock_us;
        int64_t lead;
        int64_t target;
        int chosen = -1;
        int i;
        wn_video_frame frame;
        uint64_t texture = 0;
        int have_texture = 0;

        if (!core->alive[index] ||
            core->nodes[index].kind != R2_NODE_VIDEO) {
            continue;
        }
        m = &core->media[index];
        s = m->session;
        if (s == NULL || s->frame_count == 0) {
            continue;
        }
        if (m->state != WN_MEDIA_STATE_PLAYING &&
            m->state != WN_MEDIA_STATE_PAUSED &&
            m->state != WN_MEDIA_STATE_BUFFERING &&
            m->state != WN_MEDIA_STATE_SEEKING) {
            continue;
        }
        clock_us = media_clock_us(core, m, s, now_us);
        lead = WN_MEDIA_DEFAULT_VSYNC_US;
        if (m->frame_interval_us > 0 &&
            lead > 2 * m->frame_interval_us) {
            lead = 2 * m->frame_interval_us;
        }
        target = clock_us + lead;
        media_drop_late(m, s, clock_us);
        if (s->frame_count == 0) {
            continue;
        }
        for (i = 0; i < s->frame_count; ++i) {
            if (s->frames[i].frame.pts_us <= target) {
                chosen = i;
            } else {
                break;
            }
        }
        if (chosen < 0) {
            if (m->state == WN_MEDIA_STATE_PAUSED ||
                m->state == WN_MEDIA_STATE_SEEKING) {
                chosen = 0; /* first frame after a seek/open */
            } else {
                continue; /* ahead of the clock: wait for the next tick */
            }
        }
        /* Frames before the chosen one are superseded, not presented. */
        for (i = 0; i < chosen; ++i) {
            media_frame_release(&s->frames[i]);
            m->dropped += 1;
        }
        frame = s->frames[chosen].frame;
        memset(&s->frames[chosen], 0, sizeof(s->frames[chosen]));
        if (chosen + 1 < s->frame_count) {
            memmove(&s->frames[chosen], &s->frames[chosen + 1],
                    (size_t)(s->frame_count - chosen - 1) *
                        sizeof(s->frames[0]));
        }
        memset(&s->frames[s->frame_count - 1], 0,
               sizeof(s->frames[0]));
        s->frame_count -= 1;

        if (core->media_sink != NULL && core->media_sink->upload != NULL) {
            wn_media_status status = core->media_sink->upload(
                core->media_sink_context, &frame, &texture);
            have_texture = status == WN_MEDIA_OK && texture != 0;
        }
        if (have_texture) {
            if (m->frame_texture != 0 &&
                core->media_sink->release_texture != NULL) {
                core->media_sink->release_texture(core->media_sink_context,
                                                  m->frame_texture);
            }
            m->frame_texture = texture;
            m->frame_format = frame.format;
            m->frame_color = frame.color;
            uploaded = 1;
        }
        if (m->presented > 0 && frame.pts_us > m->frame_pts_us) {
            m->frame_interval_us = frame.pts_us - m->frame_pts_us;
        } else if (frame.duration_us > 0) {
            m->frame_interval_us = frame.duration_us;
        }
        m->frame_pts_us = frame.pts_us;
        m->frame_duration_us = frame.duration_us;
        m->frame_width = frame.display_width > 0 ? frame.display_width
                                                 : frame.coded_width;
        m->frame_height = frame.display_height > 0 ? frame.display_height
                                                   : frame.coded_height;
        if (m->state != WN_MEDIA_STATE_PLAYING &&
            frame.pts_us >= 0) {
            /* A paused/seeked frame is the new anchor position. */
            m->position_us = frame.pts_us;
            if (s->anchor_position_us != frame.pts_us) {
                s->anchor_position_us = frame.pts_us;
                s->anchor_wall_us = now_us;
            }
        }
        if (frame.lease.release != NULL) {
            frame.lease.release(frame.lease.context, frame.lease.object);
        }
        m->presented += 1;
        if (!m->first_frame_seen) {
            m->first_frame_seen = 1;
            media_queue_event(core, index, WN_MEDIA_EVENT_FIRST_FRAME,
                              m->state, WN_MEDIA_OK, m->position_us,
                              m->duration_us, NULL);
        }
        if (m->frame_width > 0 && m->frame_height > 0) {
            float iw = (float)m->frame_width;
            float ih = (float)m->frame_height;
            if (iw != m->intrinsic_w || ih != m->intrinsic_h) {
                m->intrinsic_w = iw;
                m->intrinsic_h = ih;
                core->needs_layout = 1;
            }
        }
        core->dirty = 1;
    }
    return uploaded;
}

/* -------------------------------------------------------------- ticks */

static void media_tick_playing(wn_core *core, int node, WnMediaNode *m,
                               WnMediaSession *s, int64_t now_us)
{
    int64_t clock_us = media_clock_us(core, m, s, now_us);
    m->position_us = clock_us;
    if (media_pump_decode(core, node, m, s, 0) < 0) {
        return;
    }
    if (s->eof && s->frame_count == 0) {
        if (m->loop) {
            int64_t restart = 0;
            if (s->ops->flush != NULL) {
                s->ops->flush(s->context, s->session);
            }
            if (s->ops->seek != NULL) {
                s->ops->seek(s->context, s->session, 0, 0);
            }
            media_clear_queue(s);
            s->serial += 1;
            s->eof = 0;
            s->anchor_position_us = restart;
            s->anchor_wall_us = now_us;
            s->last_progress_position_us = restart;
            m->position_us = restart;
            core->dirty = 1;
            return;
        }
        m->state = WN_MEDIA_STATE_ENDED;
        m->position_us = m->duration_us > 0 ? m->duration_us
                                             : s->anchor_position_us;
        media_queue_event(core, node, WN_MEDIA_EVENT_ENDED, m->state,
                          WN_MEDIA_OK, m->position_us, m->duration_us, NULL);
        core->dirty = 1;
        return;
    }
    if (s->frame_count == 0) {
        m->state = WN_MEDIA_STATE_BUFFERING;
        s->anchor_position_us = m->position_us;
        s->anchor_wall_us = now_us;
        media_state_event(core, node, m);
        core->dirty = 1;
        return;
    }
    if (clock_us - s->last_progress_position_us >= WN_MEDIA_PROGRESS_US) {
        s->last_progress_position_us = clock_us;
        media_queue_event(core, node, WN_MEDIA_EVENT_PROGRESS, m->state,
                          WN_MEDIA_OK, m->position_us, m->duration_us, NULL);
    }
    /* Keep the sink fed while the clock advances. */
    core->dirty = 1;
}

static void media_tick_buffering(wn_core *core, int node, WnMediaNode *m,
                                 WnMediaSession *s, int64_t now_us)
{
    if (media_pump_decode(core, node, m, s, 0) < 0) {
        return;
    }
    if (s->eof && s->frame_count == 0) {
        if (m->loop) {
            if (s->ops->flush != NULL) {
                s->ops->flush(s->context, s->session);
            }
            if (s->ops->seek != NULL) {
                s->ops->seek(s->context, s->session, 0, 0);
            }
            media_clear_queue(s);
            s->serial += 1;
            s->eof = 0;
            s->anchor_position_us = 0;
            s->anchor_wall_us = now_us;
            s->last_progress_position_us = 0;
            m->position_us = 0;
            m->state = WN_MEDIA_STATE_PLAYING;
            media_state_event(core, node, m);
            return;
        }
        m->state = WN_MEDIA_STATE_ENDED;
        m->position_us = m->duration_us > 0 ? m->duration_us
                                             : s->anchor_position_us;
        media_queue_event(core, node, WN_MEDIA_EVENT_ENDED, m->state,
                          WN_MEDIA_OK, m->position_us, m->duration_us, NULL);
        core->dirty = 1;
        return;
    }
    if (s->frame_count >= 2 || s->eof) {
        m->state = WN_MEDIA_STATE_PLAYING;
        s->anchor_position_us = m->position_us;
        s->anchor_wall_us = now_us;
        s->last_progress_position_us = m->position_us;
        media_state_event(core, node, m);
        core->dirty = 1;
    }
}

static void media_tick_seeking(wn_core *core, int node, WnMediaNode *m,
                               WnMediaSession *s, int64_t now_us)
{
    if (media_pump_decode(core, node, m, s, 1) < 0) {
        return;
    }
    if (s->frame_count > 0) {
        int64_t pts = s->frames[0].frame.pts_us;
        m->position_us = pts >= 0 ? pts : s->seek_target_us;
        s->anchor_position_us = m->position_us;
        s->anchor_wall_us = now_us;
        s->last_progress_position_us = m->position_us;
        m->state = s->resume_playing ? WN_MEDIA_STATE_PLAYING
                                     : WN_MEDIA_STATE_PAUSED;
        media_state_event(core, node, m);
        core->dirty = 1;
        return;
    }
    if (s->eof) {
        m->position_us = s->seek_target_us;
        s->anchor_position_us = m->position_us;
        s->anchor_wall_us = now_us;
        m->state = s->resume_playing ? WN_MEDIA_STATE_PLAYING
                                     : WN_MEDIA_STATE_PAUSED;
        media_state_event(core, node, m);
        core->dirty = 1;
    }
}

void wn_core_media_tick(wn_core *core)
{
    int index;
    int64_t now_us;
    if (core == NULL) {
        return;
    }
    now_us = wn_core_media_now_us(core);
    for (index = 0; index < core->node_count; ++index) {
        WnMediaNode *m;
        WnMediaSession *s;
        if (!core->alive[index] ||
            core->nodes[index].kind != R2_NODE_VIDEO) {
            continue;
        }
        m = &core->media[index];
        if (m->auto_open && m->state == WN_MEDIA_STATE_IDLE) {
            if (wn_core_node_media_src(core, index) == NULL ||
                wn_core_node_media_src(core, index)[0] == '\0') {
                m->auto_open = 0; /* host-bound player node, nothing to open */
            } else {
                media_begin_open(core, index, m);
            }
        }
        s = m->session;
        if (s == NULL) {
            continue;
        }
        switch (m->state) {
        case WN_MEDIA_STATE_PLAYING:
            media_tick_playing(core, index, m, s, now_us);
            break;
        case WN_MEDIA_STATE_BUFFERING:
            media_tick_buffering(core, index, m, s, now_us);
            break;
        case WN_MEDIA_STATE_SEEKING:
            media_tick_seeking(core, index, m, s, now_us);
            break;
        case WN_MEDIA_STATE_PAUSED:
            if (s->decode_one) {
                s->decode_one = 0;
                (void)media_pump_decode(core, index, m, s, 0);
            }
            break;
        default:
            break;
        }
    }
}

int wn_core_media_active(const wn_core *core)
{
    int index;
    if (core == NULL) {
        return 0;
    }
    for (index = 0; index < core->node_count; ++index) {
        const WnMediaNode *m;
        if (!core->alive[index] ||
            core->nodes[index].kind != R2_NODE_VIDEO) {
            continue;
        }
        m = &core->media[index];
        if (m->auto_open) {
            return 1;
        }
        if (m->session != NULL &&
            (m->state == WN_MEDIA_STATE_PLAYING ||
             m->state == WN_MEDIA_STATE_BUFFERING ||
             m->state == WN_MEDIA_STATE_SEEKING ||
             m->session->decode_one || m->session->frame_count > 0)) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------ controls */

int wn_core_media_open(wn_core *core, int node)
{
    WnMediaNode *m = media_node(core, node);
    const char *src;
    if (m == NULL || core->media_registry == NULL) {
        return -1;
    }
    src = wn_core_node_media_src(core, node);
    if (src == NULL || src[0] == '\0') {
        /* Source-less host-bound player node: typed ERROR, never a crash. */
        media_session_destroy(core, m);
        media_release_texture(core, m);
        media_purge_events(core, node);
        m->state = WN_MEDIA_STATE_ERROR;
        m->error = WN_MEDIA_UNSUPPORTED_CONTAINER;
        m->position_us = 0;
        m->auto_open = 0;
        media_queue_event(core, node, WN_MEDIA_EVENT_ERROR,
                          WN_MEDIA_STATE_ERROR, WN_MEDIA_UNSUPPORTED_CONTAINER,
                          0, 0, "video node has no source (host-bound player node)");
        core->dirty = 1;
        return -2;
    }
    media_begin_open(core, node, m);
    /* `open` is asynchronous by contract: the request was accepted even when
     * the provider chain fails inline; the result is the queued ERROR event. */
    return 0;
}

int wn_core_media_play(wn_core *core, int node)
{
    WnMediaNode *m = media_node(core, node);
    WnMediaSession *s;
    int64_t now_us;
    if (m == NULL) {
        return -1;
    }
    if (m->state == WN_MEDIA_STATE_ERROR) {
        return -2;
    }
    if (m->state == WN_MEDIA_STATE_IDLE) {
        if (wn_core_media_open(core, node) != 0) {
            return -2;
        }
        if (m->state == WN_MEDIA_STATE_ERROR) {
            return -2;
        }
    }
    s = m->session;
    if (s == NULL) {
        return -2;
    }
    now_us = wn_core_media_now_us(core);
    if (m->state == WN_MEDIA_STATE_PLAYING) {
        return 0;
    }
    if (m->state == WN_MEDIA_STATE_SEEKING) {
        s->resume_playing = 1;
        return 0;
    }
    if (m->state == WN_MEDIA_STATE_ENDED) {
        /* play() from ENDED restarts at 0 without a new session. */
        if (s->ops != NULL && s->ops->flush != NULL) {
            s->ops->flush(s->context, s->session);
        }
        if (s->ops != NULL && s->ops->seek != NULL) {
            s->ops->seek(s->context, s->session, 0, 0);
        }
        media_clear_queue(s);
        s->serial += 1;
        s->eof = 0;
        s->seek_target_us = 0;
        m->position_us = 0;
    }
    media_start_playing(core, node, m, s, now_us);
    media_state_event(core, node, m);
    core->dirty = 1;
    return 0;
}

int wn_core_media_pause(wn_core *core, int node)
{
    WnMediaNode *m = media_node(core, node);
    int64_t now_us;
    if (m == NULL) {
        return -1;
    }
    if (m->state == WN_MEDIA_STATE_PLAYING ||
        m->state == WN_MEDIA_STATE_BUFFERING) {
        now_us = wn_core_media_now_us(core);
        m->position_us = media_clock_us(core, m, m->session, now_us);
        m->state = WN_MEDIA_STATE_PAUSED;
        if (m->session != NULL) {
            m->session->resume_playing = 0;
            m->session->decode_one = 0;
            /* Frames decoded ahead are not presented while paused: the last
             * presented frame stays frozen and the position does not move. */
            media_clear_queue(m->session);
        }
        media_state_event(core, node, m);
        core->dirty = 1;
    }
    return 0;
}

int wn_core_media_seek(wn_core *core, int node, int64_t position_us)
{
    WnMediaNode *m = media_node(core, node);
    WnMediaSession *s;
    if (m == NULL || position_us < 0) {
        return -1;
    }
    if (m->state == WN_MEDIA_STATE_ERROR) {
        return -2;
    }
    if (m->duration_us > 0 && position_us > m->duration_us) {
        position_us = m->duration_us;
    }
    m->position_us = position_us;
    s = m->session;
    if (m->state == WN_MEDIA_STATE_IDLE || s == NULL) {
        core->dirty = 1;
        return 0;
    }
    s->resume_playing = (m->state == WN_MEDIA_STATE_PLAYING ||
                         m->state == WN_MEDIA_STATE_BUFFERING)
                            ? 1
                            : 0;
    if (s->ops != NULL && s->ops->flush != NULL) {
        s->ops->flush(s->context, s->session);
    }
    if (s->ops != NULL && s->ops->seek != NULL) {
        wn_media_status status =
            s->ops->seek(s->context, s->session, position_us, 0);
        if (status != WN_MEDIA_OK && status != WN_MEDIA_AGAIN &&
            status != WN_MEDIA_CANCELLED) {
            media_fail(core, node, m, (int)status, "media seek failed");
            return -2;
        }
    }
    media_clear_queue(s);
    s->serial += 1;
    s->eof = 0;
    s->seek_target_us = position_us;
    s->anchor_position_us = position_us;
    s->anchor_wall_us = wn_core_media_now_us(core);
    s->last_progress_position_us = position_us;
    m->state = WN_MEDIA_STATE_SEEKING;
    media_state_event(core, node, m);
    core->dirty = 1;
    return 0;
}

int wn_core_media_set_volume(wn_core *core, int node, float gain)
{
    WnMediaNode *m = media_node(core, node);
    if (m == NULL || !isfinite(gain) || gain < 0.0f || gain > 1.0f) {
        return -1;
    }
    m->volume = gain;
    return 0;
}

int wn_core_media_set_muted(wn_core *core, int node, int muted)
{
    WnMediaNode *m = media_node(core, node);
    if (m == NULL) {
        return -1;
    }
    m->muted = muted != 0 ? 1u : 0u;
    return 0;
}

int wn_core_media_set_loop(wn_core *core, int node, int loop)
{
    WnMediaNode *m = media_node(core, node);
    if (m == NULL) {
        return -1;
    }
    m->loop = loop != 0 ? 1u : 0u;
    return 0;
}

int wn_core_media_set_source(wn_core *core, int node, const char *path)
{
    WnMediaNode *m = media_node(core, node);
    char *copy;
    if (m == NULL || path == NULL || path[0] == '\0' ||
        strstr(path, "://") != NULL) {
        return -1;
    }
    copy = wn_strdup_(path);
    if (copy == NULL) {
        return -2;
    }
    media_session_destroy(core, m);
    media_release_texture(core, m);
    wn_free_(m->source_override);
    media_purge_events(core, node);
    m->source_override = copy;
    m->state = WN_MEDIA_STATE_IDLE;
    m->error = WN_MEDIA_OK;
    m->position_us = 0;
    m->duration_us = 0;
    m->stream_flags = 0;
    m->intrinsic_w = 0.0f;
    m->intrinsic_h = 0.0f;
    m->presented = 0;
    m->dropped = 0;
    /* Re-apply preload: the next tick opens when it is metadata or auto. */
    m->auto_open = core->nodes[node].media_preload >= WN_MEDIA_PRELOAD_METADATA
                       ? 1u
                       : 0u;
    core->dirty = 1;
    core->needs_layout = 1;
    return 0;
}

int wn_core_media_get_state(const wn_core *core, int node,
                            wn_media_state_info *out)
{
    const WnMediaNode *m;
    int64_t now_us;
    if (!wn_core_node_is_video(core, node) || out == NULL) {
        return -1;
    }
    m = &core->media[node];
    now_us = wn_core_media_now_us(core);
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->state = m->state;
    out->error = m->error;
    out->position_us = media_clock_us(core, (WnMediaNode *)m, m->session,
                                      now_us);
    out->duration_us = m->duration_us;
    out->stream_flags = m->stream_flags;
    out->volume = m->volume;
    out->muted = m->muted;
    out->loop = m->loop;
    return 0;
}

void wn_core_media_close(wn_core *core, int node)
{
    WnMediaNode *m;
    if (core == NULL || !wn_core_node_is_video(core, node)) {
        return;
    }
    m = &core->media[node];
    media_purge_events(core, node);
    media_reset_node(core, m);
    core->dirty = 1;
    core->needs_layout = 1;
}

void wn_core_media_shutdown(wn_core *core)
{
    int index;
    if (core == NULL) {
        return;
    }
    for (index = 0; index < core->node_count; ++index) {
        if (!core->alive[index] ||
            core->nodes[index].kind != R2_NODE_VIDEO) {
            continue;
        }
        media_reset_node(core, &core->media[index]);
    }
}
