/*
 * wn_media.h - internal media host: provider registry, sinks and media_null.
 *
 * Not part of the public contract. The provider ABI itself lives in
 * include/webnative_media.h; this header declares the engine-side registry,
 * its deterministic ordering, the always-present null provider and the
 * internal video sink vtable. The session driver lives in media/media_core.c
 * and runs inline on the UI thread in E4.
 */
#ifndef WN_MEDIA_INTERNAL_H
#define WN_MEDIA_INTERNAL_H

#include <stdint.h>

#include "webnative_media.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define WN_MEDIA_PRIORITY_USER     1000
#define WN_MEDIA_PRIORITY_PLUGIN    500
#define WN_MEDIA_PRIORITY_COMPILED  100
#define WN_MEDIA_PRIORITY_NULL        0

/* Sink ABI (docs/media-v1.md §3.3). Sinks are internal: the host never
 * implements them and no GPU type crosses the public ABI. */
#define WN_MEDIA_SINK_ABI 1u

typedef struct wn_media_upload_stats {
    uint32_t struct_size;
    uint32_t uploads;
    uint32_t imports;
    uint32_t dropped;
    double   last_upload_ms;
} wn_media_upload_stats;

typedef struct wn_video_sink_ops {
    uint32_t struct_size;
    uint32_t abi_version;              /* WN_MEDIA_SINK_ABI */
    const char *name;
    uint32_t (*formats)(void *context);        /* mask of wn_pixel_format */
    uint32_t (*native_kind)(void *context);    /* WN_NATIVE_*; 0 = CPU only */
    int (*device)(void *context, void **out_device);
    wn_media_status (*upload)(void *context, const wn_video_frame *frame,
                              uint64_t *out_texture);
    wn_media_status (*import_native)(void *context, void *native_texture,
                                     uint32_t slice, const wn_video_frame *desc,
                                     uint64_t *out_texture);
    void (*release_texture)(void *context, uint64_t texture);
    void (*stats)(void *context, wn_media_upload_stats *out);
} wn_video_sink_ops;

typedef struct wn_media_registry wn_media_registry;

/* Creates an empty registry with media_null already present as the last,
 * non-removable entry. */
wn_media_registry *wn_media_registry_create(void);
/* Destroys the registry. Explicit providers have their `destroy` callback
 * invoked; media_null is internal and never removable. */
void wn_media_registry_destroy(wn_media_registry *registry);

/* Validates the vtable (see docs/media-v1.md §3.4) and copies it. Returns 0 on
 * success, -1 for a bad argument/vtable, -2 for out of memory. Registering the
 * same ops/context twice is rejected. */
int wn_media_registry_add(wn_media_registry *registry,
                          const wn_media_provider_ops *ops, void *context,
                          int32_t priority);
/* Removes an explicit registration. Returns 0 on success, 1 when not found and
 * media_null can never be removed, -1 for a bad argument. */
int wn_media_registry_remove(wn_media_registry *registry,
                             const wn_media_provider_ops *ops, void *context);

/* Loads a dynamic provider (WN_MEDIA_PROVIDER_QUERY) and registers it at
 * WN_MEDIA_PRIORITY_PLUGIN. The path must come from the host or the plugin
 * allowlist, never from a document. Returns 0 on success, a negative value on
 * failure with wn_last_error() set. */
int wn_media_registry_load(wn_media_registry *registry, const char *dll_path);

/* Combined capabilities of the chain (media_null contributes zero). */
void wn_media_registry_query(const wn_media_registry *registry,
                             wn_media_caps *out);

/* Chain size includes media_null. Entry indexes are the deterministic order:
 * descending priority, registration order for ties, media_null last. */
int wn_media_registry_count(const wn_media_registry *registry);
const wn_media_provider_ops *wn_media_registry_at(
    const wn_media_registry *registry, int index, void **context,
    int32_t *priority);
/* 1 when the entry is the built-in null provider. */
int wn_media_registry_is_null(const wn_media_registry *registry, int index);

/* The built-in provider. Always present, caps empty, probe/open return
 * WN_MEDIA_UNSUPPORTED_CONTAINER, so it never matches a file. */
const wn_media_provider_ops *wn_media_null_ops(void);
void *wn_media_null_context(void);

#if defined(__cplusplus)
}
#endif

#endif /* WN_MEDIA_INTERNAL_H */
