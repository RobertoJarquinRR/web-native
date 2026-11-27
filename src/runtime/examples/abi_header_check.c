/* Header-only C check: include/webnative.h and include/webnative_media.h must
 * compile as C17. */
#include "webnative.h"
#include "webnative_media.h"

#include <stddef.h>

#if WN_ABI_VERSION < 3u
#error "dynamic UI needs ABI v3 or newer"
#endif

#if WN_ABI_VERSION < 7u
#error "focus/disabled state bits need ABI v7 or newer"
#endif

#if WN_ABI_VERSION < 8u
#error "the video element and media seam need ABI v8 or newer"
#endif

#if WN_MEDIA_ABI_VERSION != 1u
#error "media provider ABI must be v1"
#endif

#if WN_STATE_FOCUSED != 0x08u || WN_STATE_DISABLED != 0x10u || \
    WN_STATE_FOCUS_VISIBLE != 0x20u || WN_STATE_FOCUS_WITHIN != 0x40u
#error "ABI v7 state bits changed"
#endif

static void on_event(void *userdata, const wn_event *event)
{
    (void)userdata;
    (void)event;
}

static void on_media_event(void *userdata, const wn_media_event *event)
{
    (void)userdata;
    (void)event;
}

int main(void)
{
    wn_event_cb callback = on_event;
    wn_media_event_cb media_callback = on_media_event;
    wn_config config;
    wn_event event;
    wn_media_event media_event;
    wn_media_state_info media_state;
    wn_status status = WN_OK;
    wn_node_id node = WN_NODE_INVALID;
    size_t event_size = sizeof(event);
    size_t node_size = sizeof(node);
    wn_status (*set_size)(wn_app *, wn_node_id, float, float) = wn_node_set_size;
    wn_status (*set_position)(wn_app *, wn_node_id, float, float) =
        wn_node_set_position;
    wn_status (*set_color)(wn_app *, wn_node_id, uint32_t) = wn_node_set_color;
    wn_status (*set_background)(wn_app *, wn_node_id, uint32_t) =
        wn_node_set_background;
    wn_status (*set_family)(wn_app *, wn_node_id, const char *) =
        wn_node_set_font_family;
    wn_status (*set_font_size)(wn_app *, wn_node_id, float) =
        wn_node_set_font_size;
    wn_status (*set_weight)(wn_app *, wn_node_id, int) =
        wn_node_set_font_weight;
    wn_status (*reset_style)(wn_app *, wn_node_id, uint32_t) =
        wn_node_reset_style;
    wn_status (*app_family)(wn_app *, const char *, int) =
        wn_app_set_font_family;
    wn_status (*register_provider)(wn_app *, const wn_media_provider_ops *,
                                   void *, int32_t) =
        wn_app_register_media_provider;
    wn_status (*unregister_provider)(wn_app *, const wn_media_provider_ops *,
                                     void *) = wn_app_unregister_media_provider;
    wn_status (*media_events)(wn_app *, wn_media_event_cb, void *) =
        wn_app_on_media_event;
    wn_status (*media_open)(wn_app *, wn_node_id) = wn_node_media_open;
    wn_status (*media_play)(wn_app *, wn_node_id) = wn_node_media_play;
    wn_status (*media_pause)(wn_app *, wn_node_id) = wn_node_media_pause;
    wn_status (*media_seek)(wn_app *, wn_node_id, int64_t) =
        wn_node_media_seek;
    wn_status (*media_volume)(wn_app *, wn_node_id, float) =
        wn_node_media_set_volume;
    wn_status (*media_muted)(wn_app *, wn_node_id, int) =
        wn_node_media_set_muted;
    wn_status (*media_loop)(wn_app *, wn_node_id, int) =
        wn_node_media_set_loop;
    wn_status (*media_source)(wn_app *, wn_node_id, const char *) =
        wn_node_media_set_source;
    wn_status (*media_subtitle)(wn_app *, wn_node_id, int32_t) =
        wn_node_media_select_subtitle;
    wn_status (*media_subtitle_delay)(wn_app *, wn_node_id, int32_t) =
        wn_node_media_set_subtitle_delay;
    wn_status (*media_get)(wn_app *, wn_node_id, wn_media_state_info *) =
        wn_node_media_get_state;
    float auto_value = WN_AUTO;
    uint32_t states = WN_STATE_HOVER | WN_STATE_PRESS | WN_STATE_SELECTED |
                      WN_STATE_FOCUSED | WN_STATE_DISABLED |
                      WN_STATE_FOCUS_VISIBLE | WN_STATE_FOCUS_WITHIN;
    uint32_t style_fields = WN_STYLE_TEXT_COLOR | WN_STYLE_BACKGROUND |
                            WN_STYLE_FONT_FAMILY | WN_STYLE_FONT_SIZE |
                            WN_STYLE_FONT_WEIGHT;
    (void)callback;
    (void)config;
    (void)status;
    (void)set_size;
    (void)set_position;
    (void)set_color;
    (void)set_background;
    (void)set_family;
    (void)set_font_size;
    (void)set_weight;
    (void)reset_style;
    (void)app_family;
    (void)register_provider;
    (void)unregister_provider;
    (void)media_events;
    (void)media_callback;
    (void)media_open;
    (void)media_play;
    (void)media_pause;
    (void)media_seek;
    (void)media_volume;
    (void)media_muted;
    (void)media_loop;
    (void)media_source;
    (void)media_subtitle;
    (void)media_subtitle_delay;
    (void)media_get;
    media_state.struct_size = (uint32_t)sizeof(media_state);
    media_state.state = WN_MEDIA_STATE_IDLE;
    media_state.error = WN_MEDIA_UNSUPPORTED_CONTAINER;
    media_event.struct_size = (uint32_t)sizeof(media_event);
    media_event.type = WN_MEDIA_EVENT_ERROR;
    media_event.node = node;
    media_event.state = media_state.state;
    media_event.error = media_state.error;
    media_event.position_us = 0;
    media_event.duration_us = 0;
    media_event.detail = NULL;
    event.node = node;
    return event_size > 0 && node_size == sizeof(uint64_t) &&
           event.node == WN_NODE_INVALID && states == 0x7Fu &&
           style_fields == WN_STYLE_ALL && auto_value < 0.0f &&
           sizeof(wn_media_state_info) == 48u &&
           sizeof(wn_media_event) == 48u &&
           media_event.error == WN_MEDIA_UNSUPPORTED_CONTAINER ? 0 : 1;
}
