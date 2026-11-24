/* Header-only C check: include/webnative.h must compile as C17. */
#include "webnative.h"

#include <stddef.h>

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
    size_t event_size = sizeof(event);
    (void)callback;
    (void)config;
    (void)status;
    return event_size > 0 ? 0 : 1;
}
