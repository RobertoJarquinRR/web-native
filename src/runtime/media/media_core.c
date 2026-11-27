/*
 * media_core.c - per-node media state, the provider seam and the typed
 * "no codec" path.
 *
 * Phase A of docs/media-v1.md implements the element and the provider seam,
 * not playback: there is no session driver, decoder, audio sink or clock yet.
 * `open` therefore validates the node, records OPENING -> ERROR and queues the
 * documented WN_MEDIA_EVENT_ERROR; `media_null` is the ordinary error path and
 * nothing pretends to play. The registry keeps registered providers in the
 * deterministic order so a later session driver has the seam already in place.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/wn_core_internal.h"
#include "wn_internal.h"
#include "wn_media.h"

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

/* ------------------------------------------------------------- control */

static void media_fail_open(wn_core *core, int node, WnMediaNode *media,
                            const char *detail)
{
    media->state = WN_MEDIA_STATE_ERROR;
    media->error = WN_MEDIA_UNSUPPORTED_CONTAINER;
    media->position_us = 0;
    media->duration_us = 0;
    media->stream_flags = 0;
    media_queue_event(core, node, WN_MEDIA_EVENT_ERROR, WN_MEDIA_STATE_ERROR,
                      WN_MEDIA_UNSUPPORTED_CONTAINER, 0, 0, detail);
    /* The poster/frame area must repaint even if the host does not. */
    core->dirty = 1;
}

int wn_core_media_open(wn_core *core, int node)
{
    WnMediaNode *media = media_node(core, node);
    const char *src;
    char detail[WN_MEDIA_EVENT_DETAIL];
    if (media == NULL) {
        return -1;
    }
    if (core->media_registry == NULL) {
        return -1;
    }
    /* Every open starts a fresh request. */
    media->state = WN_MEDIA_STATE_OPENING;
    media->error = WN_MEDIA_OK;
    media->position_us = 0;
    media->duration_us = 0;
    media->stream_flags = 0;
    src = wn_core_node_media_src(core, node);
    if (src == NULL || src[0] == '\0') {
        src = "";
    }
    if (wn_media_registry_count(core->media_registry) > 1) {
        /* A provider was registered, but this build has no session driver
         * yet. Do not fake OPENED/PLAYING; report a typed internal error. */
        media->state = WN_MEDIA_STATE_ERROR;
        media->error = WN_MEDIA_INTERNAL;
        snprintf(detail, sizeof(detail),
                 "media provider registered but the session driver is not "
                 "available in this build (%s)", src);
        media_queue_event(core, node, WN_MEDIA_EVENT_ERROR,
                          WN_MEDIA_STATE_ERROR, WN_MEDIA_INTERNAL, 0, 0,
                          detail);
        core->dirty = 1;
        return 0;
    }
    /* media_null: the ordinary "no codec installed" path. */
    snprintf(detail, sizeof(detail), "no media provider claims %s", src);
    media_fail_open(core, node, media, detail);
    return 0;
}

int wn_core_media_play(wn_core *core, int node)
{
    WnMediaNode *media = media_node(core, node);
    if (media == NULL) {
        return -1;
    }
    if (media->state == WN_MEDIA_STATE_IDLE) {
        return wn_core_media_open(core, node);
    }
    if (media->state == WN_MEDIA_STATE_ERROR) {
        return -2; /* terminal until open/set_source */
    }
    /* No session driver in this phase: an already-open state is not reachable.
     * Do not invent PLAYING; leave the state untouched. */
    return 0;
}

int wn_core_media_pause(wn_core *core, int node)
{
    WnMediaNode *media = media_node(core, node);
    if (media == NULL) {
        return -1;
    }
    if (media->state == WN_MEDIA_STATE_PLAYING) {
        media->state = WN_MEDIA_STATE_PAUSED;
        media_queue_event(core, node, WN_MEDIA_EVENT_STATE,
                          WN_MEDIA_STATE_PAUSED, WN_MEDIA_OK,
                          media->position_us, media->duration_us, NULL);
        core->dirty = 1;
    }
    return 0;
}

int wn_core_media_seek(wn_core *core, int node, int64_t position_us)
{
    WnMediaNode *media = media_node(core, node);
    if (media == NULL || position_us < 0) {
        return -1;
    }
    if (media->state == WN_MEDIA_STATE_ERROR) {
        return -2;
    }
    if (media->duration_us > 0 && position_us > media->duration_us) {
        position_us = media->duration_us;
    }
    media->position_us = position_us;
    if (media->state != WN_MEDIA_STATE_IDLE) {
        media_queue_event(core, node, WN_MEDIA_EVENT_PROGRESS,
                          media->state, WN_MEDIA_OK, media->position_us,
                          media->duration_us, NULL);
    }
    core->dirty = 1;
    return 0;
}

int wn_core_media_set_volume(wn_core *core, int node, float gain)
{
    WnMediaNode *media = media_node(core, node);
    if (media == NULL || !isfinite(gain) || gain < 0.0f || gain > 1.0f) {
        return -1;
    }
    media->volume = gain;
    return 0;
}

int wn_core_media_set_muted(wn_core *core, int node, int muted)
{
    WnMediaNode *media = media_node(core, node);
    if (media == NULL) {
        return -1;
    }
    media->muted = muted != 0 ? 1u : 0u;
    return 0;
}

int wn_core_media_set_loop(wn_core *core, int node, int loop)
{
    WnMediaNode *media = media_node(core, node);
    if (media == NULL) {
        return -1;
    }
    media->loop = loop != 0 ? 1u : 0u;
    return 0;
}

int wn_core_media_set_source(wn_core *core, int node, const char *path)
{
    WnMediaNode *media = media_node(core, node);
    char *copy;
    if (media == NULL || path == NULL || path[0] == '\0' ||
        strstr(path, "://") != NULL) {
        return -1;
    }
    copy = wn_strdup_(path);
    if (copy == NULL) {
        return -2;
    }
    wn_free_(media->source_override);
    media->source_override = copy;
    /* A new source destroys the old session and starts at IDLE; the poster
     * stays until the new source has a frame. */
    media->state = WN_MEDIA_STATE_IDLE;
    media->error = WN_MEDIA_OK;
    media->position_us = 0;
    media->duration_us = 0;
    media->stream_flags = 0;
    core->dirty = 1;
    core->needs_layout = 1; /* intrinsic size may change */
    return 0;
}

int wn_core_media_get_state(const wn_core *core, int node,
                            wn_media_state_info *out)
{
    const WnMediaNode *media = media_node((wn_core *)core, node);
    if (media == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    out->state = media->state;
    out->error = media->error;
    out->position_us = media->position_us;
    out->duration_us = media->duration_us;
    out->stream_flags = media->stream_flags;
    out->volume = media->volume;
    out->muted = media->muted;
    out->loop = media->loop;
    return 0;
}
