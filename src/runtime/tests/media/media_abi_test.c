/*
 * media_abi_test.c - the ABI v8 media seam: media_null, deterministic
 * provider order, handshake validation, public symbols and the UI-thread
 * event path.
 *
 * Runs on the offscreen view with the media fixture as the builtin document.
 * No decoder is involved: media_null is the ordinary "no codec" path.
 */
#include <stdio.h>
#include <string.h>

#include "webnative.h"
#include "wn_media.h"
#include "wn_view.h"

static int checks = 0;
static int failures = 0;
static int destroyed = 0;

static void check(const char *label, int ok)
{
    checks += 1;
    if (!ok) {
        failures += 1;
    }
    printf("  %s %s\n", ok ? "[ok]" : "[FAIL]", label);
}

static void fake_caps(void *context, wn_media_caps *out)
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
    out->pixel_formats = WN_MEDIA_BIT(WN_PIXEL_NV12);
    out->max_video_width = 1920;
    out->max_video_height = 1080;
    out->max_audio_channels = 2;
}

static wn_media_status fake_probe(void *context, const char *path,
                                  wn_media_info *out)
{
    (void)context;
    (void)path;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->struct_size = (uint32_t)sizeof(*out);
    }
    return WN_MEDIA_UNSUPPORTED_CONTAINER;
}

static wn_media_status fake_open(void *context, const wn_media_open_desc *desc,
                                 wn_media_session **out_session)
{
    (void)context;
    (void)desc;
    if (out_session != NULL) {
        *out_session = NULL;
    }
    return WN_MEDIA_UNSUPPORTED_CONTAINER;
}

static wn_media_status fake_streams(void *context, wn_media_session *session,
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

static wn_media_status fake_read(void *context, wn_media_session *session,
                                 wn_media_packet *out)
{
    (void)context;
    (void)session;
    (void)out;
    return WN_MEDIA_UNSUPPORTED_CONTAINER;
}

static wn_media_status fake_seek(void *context, wn_media_session *session,
                                 int64_t pts, uint32_t flags)
{
    (void)context;
    (void)session;
    (void)pts;
    (void)flags;
    return WN_MEDIA_UNSUPPORTED_CONTAINER;
}

static wn_media_status fake_decode(void *context, wn_media_session *session,
                                   const wn_media_packet *packet)
{
    (void)context;
    (void)session;
    (void)packet;
    return WN_MEDIA_UNSUPPORTED_CONTAINER;
}

static wn_media_status fake_receive_video(void *context, wn_media_session *session,
                                          wn_video_frame *out)
{
    (void)context;
    (void)session;
    (void)out;
    return WN_MEDIA_UNSUPPORTED_CONTAINER;
}

static wn_media_status fake_receive_audio(void *context, wn_media_session *session,
                                          wn_audio_frame *out)
{
    (void)context;
    (void)session;
    (void)out;
    return WN_MEDIA_UNSUPPORTED_CONTAINER;
}

static wn_media_status fake_flush(void *context, wn_media_session *session)
{
    (void)context;
    (void)session;
    return WN_MEDIA_OK;
}

static wn_media_status fake_close(void *context, wn_media_session *session)
{
    (void)context;
    (void)session;
    return WN_MEDIA_OK;
}

static void fake_destroy(void *context)
{
    (void)context;
    destroyed += 1;
}

static const wn_media_provider_ops kFakeOps = {
    (uint32_t)sizeof(wn_media_provider_ops),
    WN_MEDIA_ABI_VERSION,
    "fake",
    fake_caps,
    fake_probe,
    fake_open,
    fake_streams,
    fake_read,
    fake_seek,
    fake_decode,
    fake_receive_video,
    fake_receive_audio,
    fake_flush,
    fake_close,
    fake_destroy
};

static int g_events = 0;
static wn_node_id g_event_node = WN_NODE_INVALID;
static int g_event_type = -1;
static int g_event_state = -1;
static int g_event_error = -1;
static char g_event_detail[256];

static void on_media_event(void *userdata, const wn_media_event *event)
{
    (void)userdata;
    g_events += 1;
    g_event_node = event->node;
    g_event_type = (int)event->type;
    g_event_state = event->state;
    g_event_error = event->error;
    if (event->detail != NULL) {
        snprintf(g_event_detail, sizeof(g_event_detail), "%s", event->detail);
    } else {
        g_event_detail[0] = '\0';
    }
}

int main(void)
{
    wn_view *view;
    wn_node_id video;
    wn_node_id root;
    int a = 0;
    int b = 0;
    int c = 0;
    int d = 0;
    const wn_media_provider_ops *ops;
    void *context = NULL;
    int32_t priority = -1;
    wn_media_caps caps;
    wn_media_state_info state;
    wn_media_provider_ops bad;

    checks = 0;
    failures = 0;
    destroyed = 0;

    check("public ABI is v8", WN_ABI_VERSION == 8u && wn_abi_version() == 8u);
    check("provider ABI is v1", WN_MEDIA_ABI_VERSION == 1u);
    check("media state info is 48 bytes",
          sizeof(wn_media_state_info) == 48u);
    check("media event is 48 bytes", sizeof(wn_media_event) == 48u);
    check("provider vtable exposes receive_audio",
          sizeof(wn_media_provider_ops) >=
              offsetof(wn_media_provider_ops, receive_audio) + sizeof(void *));

    /* A NULL view/app must reject every media entry point, never crash. */
    check("NULL view rejects registration",
          wn_view_register_media_provider(NULL, &kFakeOps, NULL, 0) ==
              WN_ERR_INVALID_ARGUMENT);
    check("NULL view rejects an event callback",
          wn_view_on_media_event(NULL, on_media_event, NULL) ==
              WN_ERR_INVALID_ARGUMENT);
    check("NULL app rejects node media control",
          wn_node_media_open(NULL, 1) == WN_ERR_INVALID_ARGUMENT &&
              wn_node_media_get_state(NULL, 1, &state) ==
                  WN_ERR_INVALID_ARGUMENT);
    check("NULL app rejects provider registration",
          wn_app_register_media_provider(NULL, &kFakeOps, NULL, 0) ==
              WN_ERR_INVALID_ARGUMENT);

    view = wn_view_create_offscreen(400, 640);
    check("offscreen view builds", view != NULL);
    if (view == NULL) {
        printf("media_abi_test: FAIL (%s)\n",
               wn_last_error() != NULL ? wn_last_error() : "view");
        return 1;
    }
    video = wn_view_find(view, "body.root/video#poster");
    root = wn_view_find(view, "body.root");
    check("fixture video resolves", video != WN_NODE_INVALID);
    check("fixture root resolves", root != WN_NODE_INVALID);

    /* media_null is always the last entry and never matches. */
    check("chain starts with exactly media_null",
          wn_core_media_provider_count(wn_view_core(view)) == 1);
    ops = wn_core_media_provider_at(wn_view_core(view), 0, &context, &priority);
    check("the only entry is the built-in null provider",
          ops != NULL && wn_core_media_provider_is_null(wn_view_core(view), 0) &&
              strcmp(ops->name, "null") == 0 &&
              priority == WN_MEDIA_PRIORITY_NULL);
    memset(&caps, 0xFF, sizeof(caps));
    ops->caps(context, &caps);
    check("null caps are empty",
          caps.struct_size == (uint32_t)sizeof(caps) && caps.flags == 0 &&
              caps.containers == 0 && caps.video_codecs == 0);
    check("null never matches a probe",
          ops->probe(context, "clips/poster.mp4", NULL) ==
              WN_MEDIA_UNSUPPORTED_CONTAINER);
    check("null never opens a session",
          ops->open(context, NULL, NULL) == WN_MEDIA_UNSUPPORTED_CONTAINER);

    /* Media-null open path: accepted, typed ERROR event on the next tick. */
    check("event callback registers",
          wn_view_on_media_event(view, on_media_event, NULL) == WN_OK);
    g_events = 0;
    g_event_node = WN_NODE_INVALID;
    g_event_type = -1;
    check("video open is accepted", wn_view_media_open(view, video) == WN_OK);
    check("open queued an event until tick", g_events == 0);
    check("tick flushes the event on the UI thread",
          wn_view_tick(view) == WN_OK && g_events == 1);
    check("event carries the typed media_null error",
          g_event_node == video && g_event_type == WN_MEDIA_EVENT_ERROR &&
              g_event_state == WN_MEDIA_STATE_ERROR &&
              g_event_error == WN_MEDIA_UNSUPPORTED_CONTAINER);
    check("event detail names the source",
          strstr(g_event_detail, "clips/poster.mp4") != NULL);
    memset(&state, 0, sizeof(state));
    check("state is ERROR/UNSUPPORTED after open",
          wn_view_media_get_state(view, video, &state) == WN_OK &&
              state.state == WN_MEDIA_STATE_ERROR &&
              state.error == WN_MEDIA_UNSUPPORTED_CONTAINER);
    check("play on an ERROR node is WN_ERR_STATE",
          wn_view_media_play(view, video) == WN_ERR_STATE);
    check("set_source clears the error and returns to IDLE",
          wn_view_media_set_source(view, video, "clips/other.mp4") == WN_OK &&
              wn_view_media_get_state(view, video, &state) == WN_OK &&
              state.state == WN_MEDIA_STATE_IDLE &&
              state.error == WN_MEDIA_OK);
    check("set_source rejects a scheme",
          wn_view_media_set_source(view, video, "https://x/y.mp4") ==
              WN_ERR_INVALID_ARGUMENT);
    check("set_volume accepts 0..1",
          wn_view_media_set_volume(view, video, 0.25f) == WN_OK &&
              wn_view_media_get_state(view, video, &state) == WN_OK &&
              state.volume == 0.25f);
    check("set_volume rejects out-of-range/NaN",
          wn_view_media_set_volume(view, video, 1.5f) ==
                  WN_ERR_INVALID_ARGUMENT &&
              wn_view_media_set_volume(view, video, -0.1f) ==
                  WN_ERR_INVALID_ARGUMENT);
    check("set_muted/set_loop update runtime state",
          wn_view_media_set_muted(view, video, 0) == WN_OK &&
              wn_view_media_set_loop(view, video, 0) == WN_OK &&
              wn_view_media_get_state(view, video, &state) == WN_OK &&
              state.muted == 0 && state.loop == 0);
    check("seek is allowed in IDLE",
          wn_view_media_seek(view, video, 5000) == WN_OK &&
              wn_view_media_get_state(view, video, &state) == WN_OK &&
              state.position_us == 5000);
    check("media controls reject a non-video node",
          wn_view_media_open(view, root) == WN_ERR_UNSUPPORTED);
    check("media controls reject a dead handle",
          wn_view_media_open(view, WN_NODE_INVALID) == WN_ERR_INVALID_ARGUMENT);

    /* Deterministic order: descending priority, registration order for ties,
     * media_null always last. */
    check("register compiled provider",
          wn_view_register_media_provider(view, &kFakeOps, &a, 100) == WN_OK);
    check("register user provider",
          wn_view_register_media_provider(view, &kFakeOps, &b, 1000) == WN_OK);
    check("register plugin provider",
          wn_view_register_media_provider(view, &kFakeOps, &c, 500) == WN_OK);
    check("register tied plugin provider",
          wn_view_register_media_provider(view, &kFakeOps, &d, 500) == WN_OK);
    check("chain now has four explicit providers plus null",
          wn_core_media_provider_count(wn_view_core(view)) == 5);
    {
        static const int32_t expected_priority[] = { 1000, 500, 500, 100, 0 };
        int i;
        int order_ok = 1;
        for (i = 0; i < 5; ++i) {
            ops = wn_core_media_provider_at(wn_view_core(view), i, &context,
                                             &priority);
            if (ops == NULL || priority != expected_priority[i]) {
                order_ok = 0;
            }
        }
        check("lookup order is priority desc, then registration, then null",
              order_ok);
        check("the last entry is null",
              wn_core_media_provider_is_null(wn_view_core(view), 4));
    }
    wn_core_media_provider_at(wn_view_core(view), 1, &context, &priority);
    check("tie keeps registration order (first 500 first)", context == &c);
    wn_core_media_provider_at(wn_view_core(view), 2, &context, &priority);
    check("tie keeps registration order (second 500 second)", context == &d);

    /* The registry belongs to the view: a document replacement must keep the
     * same explicit providers (and not destroy/reuse their contexts). */
    check("document replacement keeps the provider chain",
          wn_view_replace_document(view, (wn_document *)wn_document_builtin()) == 0 &&
              wn_core_media_provider_count(wn_view_core(view)) == 5);
    check("replacement keeps media_null last",
          wn_core_media_provider_is_null(wn_view_core(view), 4));

    /* With a registered provider this phase still has no session driver: the
     * open must not fake OPENED/PLAYING. */
    check("open with a provider still refuses to fake playback",
          wn_view_media_open(view, video) == WN_OK &&
              wn_view_media_get_state(view, video, &state) == WN_OK &&
              state.state == WN_MEDIA_STATE_ERROR &&
              state.error == WN_MEDIA_INTERNAL);

    /* Handshake rejections. */
    bad = kFakeOps;
    bad.struct_size = 8;
    check("short provider vtable is rejected",
          wn_view_register_media_provider(view, &bad, &a, 10) ==
              WN_ERR_INVALID_ARGUMENT);
    bad = kFakeOps;
    bad.abi_version = WN_MEDIA_ABI_VERSION + 1u;
    check("provider ABI mismatch is rejected",
          wn_view_register_media_provider(view, &bad, &a, 10) ==
              WN_ERR_INVALID_ARGUMENT);
    bad = kFakeOps;
    bad.name = "";
    check("empty provider name is rejected",
          wn_view_register_media_provider(view, &bad, &a, 10) ==
              WN_ERR_INVALID_ARGUMENT);
    bad = kFakeOps;
    bad.caps = NULL;
    check("provider without caps is rejected",
          wn_view_register_media_provider(view, &bad, &a, 10) ==
              WN_ERR_INVALID_ARGUMENT);
    bad = kFakeOps;
    bad.destroy = NULL;
    check("provider without destroy is rejected",
          wn_view_register_media_provider(view, &bad, &a, 10) ==
              WN_ERR_INVALID_ARGUMENT);
    bad = kFakeOps;
    bad.name = "null";
    check("duplicating media_null is rejected",
          wn_view_register_media_provider(view, &bad, &a, 10) ==
              WN_ERR_INVALID_ARGUMENT);
    check("duplicate registration is rejected",
          wn_view_register_media_provider(view, &kFakeOps, &a, 100) ==
              WN_ERR_INVALID_ARGUMENT);

    check("unregister compiled provider", 
          wn_view_unregister_media_provider(view, &kFakeOps, &a) == WN_OK);
    check("unregister user provider",
          wn_view_unregister_media_provider(view, &kFakeOps, &b) == WN_OK);
    check("unregister destroys the provider context", destroyed == 2);
    check("unregistering null is rejected",
          wn_view_unregister_media_provider(view, wn_media_null_ops(),
                                            wn_media_null_context()) ==
              WN_ERR_INVALID_ARGUMENT);
    check("chain is back to two explicit + null",
          wn_core_media_provider_count(wn_view_core(view)) == 3);

    /* Destroying the view destroys the remaining explicit contexts. */
    wn_view_destroy(view);
    check("view destroy releases the registered contexts", destroyed == 4);

    /* Public symbols are linkable function pointers. */
    {
        wn_status (*open_fn)(wn_app *, wn_node_id) = wn_node_media_open;
        wn_status (*events_fn)(wn_app *, wn_media_event_cb, void *) =
            wn_app_on_media_event;
        wn_status (*state_fn)(wn_app *, wn_node_id, wn_media_state_info *) =
            wn_node_media_get_state;
        check("public media symbols are present",
              open_fn != NULL && events_fn != NULL && state_fn != NULL);
    }

    if (failures != 0) {
        printf("media_abi_test: FAIL (%d checks, %d failed)\n", checks,
               failures);
        return 1;
    }
    printf("media_abi_test: PASS (%d checks)\n", checks);
    return 0;
}
