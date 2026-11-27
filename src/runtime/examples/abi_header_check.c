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
    uint32_t states = WN_STATE_HOVER | WN_STATE_PRESS | WN_STATE_SELECTED |
                      WN_STATE_FOCUSED | WN_STATE_DISABLED |
                      WN_STATE_FOCUS_VISIBLE | WN_STATE_FOCUS_WITHIN;
    (void)callback;
    (void)config;
    (void)status;
    event.node = node;
    return event_size > 0 && node_size == sizeof(uint64_t) &&
           event.node == WN_NODE_INVALID && states == 0x7Fu ? 0 : 1;
}
