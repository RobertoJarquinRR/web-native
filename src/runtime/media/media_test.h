/*
 * media_test.h - deterministic synthetic video provider (tests only).
 *
 * Never compiled into the shipped runtime: the media suite and the frame
 * harness add media_test.c explicitly. It exists so the session driver, the
 * sink and the offscreen PNG gate can run against real frames with no codec,
 * file or wall clock.
 */
#ifndef WN_MEDIA_TEST_H
#define WN_MEDIA_TEST_H

#include <stdint.h>

#include "webnative_media.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define WN_MEDIA_TEST_WIDTH 320
#define WN_MEDIA_TEST_HEIGHT 180
#define WN_MEDIA_TEST_FRAMES 300          /* 30 fps for 10 s */
#define WN_MEDIA_TEST_DURATION_US 10000000

/* Shot PTS for a frame index (integer, strictly increasing). */
int64_t wn_media_test_pts_us(int frame_index);
/* Duration of one frame; the last frame is one microsecond tail longer. */
int64_t wn_media_test_duration_us(int frame_index);
/* Expected ARGB pixel of a generated frame (0xAARRGGBB, as the engine uses). */
uint32_t wn_media_test_pixel(int frame_index, int x, int y);
/* Frame buffers handed out and not yet released (`release` must be called). */
int wn_media_test_live_frames(void);

const wn_media_provider_ops *wn_media_test_ops(void);

#if defined(__cplusplus)
}
#endif

#endif /* WN_MEDIA_TEST_H */
