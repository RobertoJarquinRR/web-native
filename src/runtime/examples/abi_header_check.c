/* Header-only C check: include/webnative.h must compile as C17. */
#include "webnative.h"

#include <stddef.h>

#if WN_ABI_VERSION < 3u
#error "dynamic UI needs ABI v3 or newer"
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
    (void)callback;
    (void)config;
    (void)status;
    event.node = node;
    return event_size > 0 && node_size == sizeof(uint64_t) &&
           event.node == WN_NODE_INVALID ? 0 : 1;
}
