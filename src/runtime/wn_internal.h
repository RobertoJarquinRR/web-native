/*
 * wn_internal.h - small shared helpers for the ABI runtime layer.
 *
 * Not part of the public contract (include/webnative.h).
 */
#ifndef WN_INTERNAL_H
#define WN_INTERNAL_H

#include <stddef.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* Thread-local readable error. wn_set_error() copies/formats; wn_clear_error()
 * resets it at the start of every public engine call. */
void wn_clear_error(void);
void wn_set_error(const char *fmt, ...);
void wn_set_error_raw(const char *text);

/* UTF-8 helpers: always copy, never take ownership of the input. */
char *wn_strdup_(const char *text);
void wn_free_(void *ptr);

double wn_now_ms(void);

#if defined(__cplusplus)
}
#endif

#endif /* WN_INTERNAL_H */
