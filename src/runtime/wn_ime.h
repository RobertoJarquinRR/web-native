/*
 * wn_ime.h - platform input-method (IME) boundary.
 *
 * The core owns the composition state, painting and the commit rule; it knows
 * nothing about Win32. A platform host implements this vtable and registers it
 * with wn_core_set_ime() (internal surface). IMM32 is the shipped backend in
 * host_win32.c; TSF can replace it later behind the same vtable without
 * touching the core.
 *
 * One composition is active at a time, owned by the focused editable input.
 * Every call gets the opaque `context` the host registered; the core never
 * dereferences it.
 */
#ifndef WN_IME_H
#define WN_IME_H

#include "webnative.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct wn_ime_ops {
    const char *name;
    /* The focused input gained the IME: enable the platform IME and place the
     * composition window at `caret_rect` (view/client pixels). Returns 0 on
     * success, nonzero when the host cannot enable it. */
    int  (*focus)(void *context, const wn_rect *caret_rect);
    /* The focused input (or the whole view) lost the IME: disable it. Called
     * once per focus loss, including a focus switch to another input. */
    void (*blur)(void *context);
    /* The caret moved while an input owns the IME. `caret_rect` is the painted
     * caret box (width is the caret width). */
    void (*set_caret_rect)(void *context, const wn_rect *caret_rect);
    /* Candidate window anchor: the caret's bottom-left corner (zero-sized
     * rect) recomputed with the caret. */
    void (*set_candidate_rect)(void *context, const wn_rect *candidate_rect);
} wn_ime_ops;

#if defined(__cplusplus)
}
#endif

#endif /* WN_IME_H */
