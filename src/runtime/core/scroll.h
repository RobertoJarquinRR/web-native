/*
 * scroll.h - the one clamp/apply service for x/y scroll offsets.
 *
 * Internal to the core: no ABI change. Wheel routing, scroll_into_view and the
 * future horizontal path all go through these two functions instead of
 * reimplementing the clamp. The public wn_core_scroll_offset/max diagnostics
 * stay vertical-only for ABI v6 compatibility.
 */
#ifndef WN_CORE_SCROLL_H
#define WN_CORE_SCROLL_H

#include "wn_core.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* Axis selector for wn_core_scroll_clamp(). */
enum {
    WN_SCROLL_X = 0,
    WN_SCROLL_Y = 1
};

/* Clamp `value` into [0, scroll_max_<axis>] for `node`. Pure: never re-runs
 * layout and never moves anything. Returns 0 for an invalid core/node/axis. */
float wn_core_scroll_clamp(wn_core *c, int node, int axis, float value);

/* Apply (dx, dy) through the clamp above. With scroll_max_x == 0 the x half is
 * a no-op, so callers can always pass both deltas. Returns 1 when either
 * offset moved, 0 when the position did not change, -1 for an invalid
 * core/node or a non-finite delta. Re-runs layout after a move, exactly like
 * the wheel did. */
int wn_core_scroll_by(wn_core *c, int node, float dx, float dy);

#if defined(__cplusplus)
}
#endif

#endif /* WN_CORE_SCROLL_H */
