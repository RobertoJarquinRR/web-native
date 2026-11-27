/*
 * media_registry.c - deterministic provider chain for the optional media seam.
 *
 * docs/media-v1.md §3.4: descending priority, registration order for ties,
 * media_null always last and never removable. Selection is per file, never per
 * node or document; this phase only builds and queries the chain (the session
 * driver that would call probe/open is a later phase).
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "wn_internal.h"
#include "wn_media.h"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

typedef struct {
    const wn_media_provider_ops *ops;
    void *context;
    int32_t priority;
} WnMediaEntry;

struct wn_media_registry {
    WnMediaEntry *entries;
    int count;
    int capacity;
};

static int registry_reserve(wn_media_registry *registry, int minimum)
{
    WnMediaEntry *grown;
    int capacity;
    if (minimum <= registry->capacity) {
        return 0;
    }
    capacity = registry->capacity > 0 ? registry->capacity : 4;
    while (capacity < minimum) {
        capacity *= 2;
    }
    grown = (WnMediaEntry *)realloc(registry->entries,
                                    (size_t)capacity * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    registry->entries = grown;
    registry->capacity = capacity;
    return 0;
}

wn_media_registry *wn_media_registry_create(void)
{
    wn_media_registry *registry =
        (wn_media_registry *)calloc(1, sizeof(*registry));
    return registry;
}

void wn_media_registry_destroy(wn_media_registry *registry)
{
    int i;
    if (registry == NULL) {
        return;
    }
    for (i = 0; i < registry->count; ++i) {
        const wn_media_provider_ops *ops = registry->entries[i].ops;
        if (ops != NULL && ops->destroy != NULL) {
            ops->destroy(registry->entries[i].context);
        }
    }
    free(registry->entries);
    free(registry);
}

static int registry_find(const wn_media_registry *registry,
                         const wn_media_provider_ops *ops, void *context)
{
    int i;
    for (i = 0; i < registry->count; ++i) {
        if (registry->entries[i].ops == ops &&
            registry->entries[i].context == context) {
            return i;
        }
    }
    return -1;
}

int wn_media_registry_add(wn_media_registry *registry,
                          const wn_media_provider_ops *ops, void *context,
                          int32_t priority)
{
    int insert;
    if (registry == NULL || ops == NULL) {
        wn_set_error("wn_media_registry_add: registry and ops are required");
        return -1;
    }
    /* Handshake order from §3.4. A short struct_size means an operation up to
     * receive_audio is missing. */
    if (ops->struct_size <
        (uint32_t)(offsetof(wn_media_provider_ops, receive_audio) +
                   sizeof(void *))) {
        wn_set_error("wn_media_registry_add: provider vtable is too short "
                     "(struct_size %u)", (unsigned)ops->struct_size);
        return -1;
    }
    if (ops->abi_version != WN_MEDIA_ABI_VERSION) {
        wn_set_error("wn_media_registry_add: provider ABI %u does not match "
                     "%u", (unsigned)ops->abi_version,
                     (unsigned)WN_MEDIA_ABI_VERSION);
        return -1;
    }
    if (ops->name == NULL || ops->name[0] == '\0') {
        wn_set_error("wn_media_registry_add: provider name is empty");
        return -1;
    }
    if (strcmp(ops->name, "null") == 0) {
        wn_set_error("wn_media_registry_add: the built-in 'null' provider "
                     "cannot be duplicated");
        return -1;
    }
    if (ops->caps == NULL || ops->open == NULL || ops->close == NULL ||
        ops->destroy == NULL) {
        wn_set_error("wn_media_registry_add: provider '%s' is missing an "
                     "operation (caps/open/close/destroy)", ops->name);
        return -1;
    }
    if (registry_find(registry, ops, context) >= 0) {
        wn_set_error("wn_media_registry_add: provider '%s' is already "
                     "registered", ops->name);
        return -1;
    }
    if (registry_reserve(registry, registry->count + 1) != 0) {
        wn_set_error("wn_media_registry_add: out of memory");
        return -2;
    }
    /* Descending priority. Insert before the first strictly lower priority so
     * equal priorities keep registration order. media_null lives outside this
     * array, so it is always last. */
    insert = registry->count;
    for (int i = 0; i < registry->count; ++i) {
        if (registry->entries[i].priority < priority) {
            insert = i;
            break;
        }
    }
    if (insert < registry->count) {
        memmove(&registry->entries[insert + 1], &registry->entries[insert],
                (size_t)(registry->count - insert) * sizeof(*registry->entries));
    }
    registry->entries[insert].ops = ops;
    registry->entries[insert].context = context;
    registry->entries[insert].priority = priority;
    registry->count += 1;
    return 0;
}

int wn_media_registry_remove(wn_media_registry *registry,
                             const wn_media_provider_ops *ops, void *context)
{
    int index;
    if (registry == NULL || ops == NULL) {
        wn_set_error("wn_media_registry_remove: registry and ops are required");
        return -1;
    }
    index = registry_find(registry, ops, context);
    if (index < 0) {
        return 1; /* not found; media_null is never in this array */
    }
    if (ops->destroy != NULL) {
        ops->destroy(context);
    }
    memmove(&registry->entries[index], &registry->entries[index + 1],
            (size_t)(registry->count - index - 1) * sizeof(*registry->entries));
    registry->count -= 1;
    return 0;
}

int wn_media_registry_load(wn_media_registry *registry, const char *dll_path)
{
    if (registry == NULL || dll_path == NULL || dll_path[0] == '\0') {
        wn_set_error("wn_media_registry_load: registry and path are required");
        return -1;
    }
#if defined(_WIN32)
    {
        HMODULE module = LoadLibraryA(dll_path);
        wn_media_provider_query_fn query;
        const wn_media_provider_ops *ops = NULL;
        void *context = NULL;
        int result;
        if (module == NULL) {
            wn_set_error("wn_media_registry_load: cannot load '%s'", dll_path);
            return -1;
        }
        query = (wn_media_provider_query_fn)(void *)GetProcAddress(
            module, WN_MEDIA_PROVIDER_QUERY);
        if (query == NULL) {
            wn_set_error("wn_media_registry_load: '%s' does not export %s",
                         dll_path, WN_MEDIA_PROVIDER_QUERY);
            FreeLibrary(module);
            return -1;
        }
        if (query(WN_MEDIA_ABI_VERSION, &ops, &context) != 0 || ops == NULL) {
            wn_set_error("wn_media_registry_load: '%s' rejected the provider "
                         "handshake", dll_path);
            FreeLibrary(module);
            return -1;
        }
        result = wn_media_registry_add(registry, ops, context,
                                       WN_MEDIA_PRIORITY_PLUGIN);
        if (result != 0) {
            /* Keep the module loaded: the context may contain code pointers
             * owned by it, and the engine never frees plugin memory. */
            return result;
        }
        return 0;
    }
#else
    (void)registry;
    wn_set_error("wn_media_registry_load: dynamic providers are only "
                 "supported on Windows in this phase");
    return -1;
#endif
}

void wn_media_registry_query(const wn_media_registry *registry,
                             wn_media_caps *out)
{
    int i;
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->struct_size = (uint32_t)sizeof(*out);
    if (registry == NULL) {
        return;
    }
    for (i = 0; i < registry->count; ++i) {
        wn_media_caps caps;
        const wn_media_provider_ops *ops = registry->entries[i].ops;
        if (ops->caps == NULL) {
            continue;
        }
        memset(&caps, 0, sizeof(caps));
        caps.struct_size = (uint32_t)sizeof(caps);
        ops->caps(registry->entries[i].context, &caps);
        out->flags |= caps.flags;
        out->containers |= caps.containers;
        out->video_codecs |= caps.video_codecs;
        out->audio_codecs |= caps.audio_codecs;
        out->pixel_formats |= caps.pixel_formats;
        if (caps.max_video_width > out->max_video_width) {
            out->max_video_width = caps.max_video_width;
        }
        if (caps.max_video_height > out->max_video_height) {
            out->max_video_height = caps.max_video_height;
        }
        if (caps.max_audio_channels > out->max_audio_channels) {
            out->max_audio_channels = caps.max_audio_channels;
        }
    }
}

int wn_media_registry_count(const wn_media_registry *registry)
{
    /* media_null always terminates the chain. */
    return registry != NULL ? registry->count + 1 : 0;
}

const wn_media_provider_ops *wn_media_registry_at(
    const wn_media_registry *registry, int index, void **context,
    int32_t *priority)
{
    if (registry == NULL || index < 0 || index > registry->count) {
        return NULL;
    }
    if (index == registry->count) {
        if (context != NULL) {
            *context = wn_media_null_context();
        }
        if (priority != NULL) {
            *priority = WN_MEDIA_PRIORITY_NULL;
        }
        return wn_media_null_ops();
    }
    if (context != NULL) {
        *context = registry->entries[index].context;
    }
    if (priority != NULL) {
        *priority = registry->entries[index].priority;
    }
    return registry->entries[index].ops;
}

int wn_media_registry_is_null(const wn_media_registry *registry, int index)
{
    return registry != NULL && index == registry->count;
}
