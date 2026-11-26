/*
 * wn_common.c - version, thread-local error and string helpers.
 */
#include "webnative.h"
#include "wn_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#if defined(_MSC_VER)
#  define WN_THREAD_LOCAL __declspec(thread)
#else
#  define WN_THREAD_LOCAL _Thread_local
#endif

static WN_THREAD_LOCAL char g_error[WN_ERROR_CAP];
static WN_THREAD_LOCAL int g_has_error;

const char *wn_version(void)
{
    return "0.1.0";
}

uint32_t wn_abi_version(void)
{
    return WN_ABI_VERSION;
}

const char *wn_last_error(void)
{
    return g_has_error ? g_error : NULL;
}

void wn_clear_error(void)
{
    g_has_error = 0;
    g_error[0] = '\0';
}

void wn_set_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
    g_error[sizeof(g_error) - 1] = '\0';
    g_has_error = 1;
}

void wn_set_error_raw(const char *text)
{
    if (text == NULL) {
        text = "unknown error";
    }
    snprintf(g_error, sizeof(g_error), "%s", text);
    g_error[sizeof(g_error) - 1] = '\0';
    g_has_error = 1;
}

char *wn_strdup_(const char *text)
{
    size_t length;
    char *copy;
    if (text == NULL) {
        return NULL;
    }
    length = strlen(text);
    copy = (char *)malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, length + 1);
    return copy;
}

void wn_free_(void *ptr)
{
    free(ptr);
}

double wn_now_ms(void)
{
#if defined(_WIN32)
    LARGE_INTEGER freq;
    LARGE_INTEGER tick;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tick);
    return (double)tick.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    return 0.0;
#endif
}
