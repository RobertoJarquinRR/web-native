/* Header-only C check: include/webnative.h must compile as C17. */
#include "webnative.h"

#include <stddef.h>

#if WN_ABI_VERSION < 3u
#error "dynamic UI needs ABI v3 or newer"
#endif

#if WN_ABI_VERSION < 7u
#error "focus/disabled state bits need ABI v7 or newer"
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

int main(void)
{
    wn_event_cb callback = on_event;
    wn_config config;
    wn_event event;
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
    event.node = node;
    return event_size > 0 && node_size == sizeof(uint64_t) &&
           event.node == WN_NODE_INVALID && states == 0x7Fu &&
           style_fields == WN_STYLE_ALL && auto_value < 0.0f ? 0 : 1;
}
