// Header-only C++ check: include/webnative.h must compile as C++17.
#include "webnative.h"

#include <cstddef>
#include <cstdint>

#if WN_ABI_VERSION < 3u
#error "dynamic UI needs ABI v3 or newer"
#endif

int main()
{
    wn_config config{};
    wn_event event{};
    wn_status status = WN_OK;
    wn_node_id node = WN_NODE_INVALID;
    std::size_t event_size = sizeof(event);
    std::size_t node_size = sizeof(node);
    (void)config;
    (void)status;
    return event_size > 0 && node_size == sizeof(std::uint64_t) && node == 0 ? 0 : 1;
}
