// Header-only C++ check: include/webnative.h must compile as C++17.
#include "webnative.h"

#include <cstddef>

int main()
{
    wn_config config{};
    wn_event event{};
    wn_status status = WN_OK;
    std::size_t event_size = sizeof(event);
    (void)config;
    (void)status;
    return event_size > 0 ? 0 : 1;
}
