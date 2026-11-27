/*
 * wn_view.c - embedded mode: core + renderer attached to a host-provided
 * native window. The host owns the loop and calls wn_view_render() on demand.
 */
#include "webnative.h"
#include "wn_view.h"
#include "wn_core.h"
#include "wn_renderer.h"
#include "wn_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

static int view_node_index(const wn_view *view, wn_node_id node);

struct wn_view {
    wn_core *core;
    const wn_renderer_ops *ops;
    void *renderer;
    void *native_window;
    int width;
    int height;
    float dpi_scale;
    int owns_core;   /* 0 when the core is borrowed (tooling/tests) */
    const wn_document *document; /* owned when set by wn_view_replace_document */
    int loaded;
    int closed;
    int geometry_dirty;
    int upload_dirty;
    int have_frame;
    double created_ms;
    double first_frame_ms;
    char *html;
    char *base_url;
    char *css;
    char *value_scratch;      /* wn_view_get_value result, freed on next call */
    char *text_scratch;       /* wn_view_get_text result, freed on next call */
    /* ABI v7: the platform IME belongs to the view, not to one core. Kept here
     * so wn_view_replace_document can install it on the replacement core. */
    const wn_ime_ops *ime_ops;
    void *ime_context;
    wn_event_cb callback;
    void *userdata;
    /* ABI v8: media events have their own callback family and are drained on
     * the UI thread from render/tick. */
    wn_media_event_cb media_callback;
    void *media_userdata;
    /* Explicit provider registrations mirrored here so a document replacement
     * (which builds a fresh core) keeps the view's media seam. media_null is
     * never stored: every core registry creates its own. */
    struct {
        const wn_media_provider_ops *ops;
        void *context;
        int32_t priority;
    } *media_providers;
    int media_provider_count;
    int media_provider_capacity;
    /* Re-entrancy guard: a callback may destroy this view while it is still
     * dispatching (own event or a nested wn_view_event on another view). The
     * free is deferred until the outermost dispatch returns so the flags below
     * are always valid to read. */
    int dispatch_depth;
    int dispatch_destroyed;
    int destroy_pending;
    wn_view_stats stats;
};

static void view_mark_geometry(wn_view *view)
{
    view->geometry_dirty = 1;
}

/* Actual teardown. wn_view_destroy() defers it while dispatch_depth > 0. */
static void view_free(wn_view *view)
{
    if (view->ops != NULL && view->renderer != NULL) {
        view->ops->destroy(view->renderer);
    }
    if (view->owns_core) {
        wn_core_destroy(view->core);
    }
    if (view->document != NULL) {
        wn_document_release((wn_document *)view->document);
    }
    wn_free_(view->html);
    wn_free_(view->base_url);
    wn_free_(view->css);
    wn_free_(view->value_scratch);
    wn_free_(view->text_scratch);
    free(view->media_providers);
    free(view);
}

static int view_media_provider_find(const wn_view *view,
                                    const wn_media_provider_ops *ops,
                                    void *context)
{
    int i;
    for (i = 0; i < view->media_provider_count; ++i) {
        if (view->media_providers[i].ops == ops &&
            view->media_providers[i].context == context) {
            return i;
        }
    }
    return -1;
}

static int view_media_provider_remember(wn_view *view,
                                        const wn_media_provider_ops *ops,
                                        void *context, int32_t priority)
{
    int capacity;
    if (view_media_provider_find(view, ops, context) >= 0) {
        return 0;
    }
    if (view->media_provider_count == view->media_provider_capacity) {
        void *grown;
        capacity = view->media_provider_capacity > 0
                       ? view->media_provider_capacity * 2
                       : 4;
        grown = realloc(view->media_providers,
                        (size_t)capacity * sizeof(*view->media_providers));
        if (grown == NULL) {
            return -1;
        }
        view->media_providers = grown;
        view->media_provider_capacity = capacity;
    }
    view->media_providers[view->media_provider_count].ops = ops;
    view->media_providers[view->media_provider_count].context = context;
    view->media_providers[view->media_provider_count].priority = priority;
    view->media_provider_count += 1;
    return 0;
}

static void view_media_provider_forget(wn_view *view,
                                       const wn_media_provider_ops *ops,
                                       void *context)
{
    int index = view_media_provider_find(view, ops, context);
    if (index >= 0) {
        view->media_providers[index] =
            view->media_providers[view->media_provider_count - 1];
        view->media_provider_count -= 1;
    }
}

static void view_media_provider_restore(wn_view *view)
{
    int i;
    if (view == NULL || view->core == NULL) {
        return;
    }
    for (i = 0; i < view->media_provider_count; ++i) {
        (void)wn_core_register_media_provider(
            view->core, view->media_providers[i].ops,
            view->media_providers[i].context,
            view->media_providers[i].priority);
    }
}

static void view_resize_internal(wn_view *view, int width, int height, float dpi)
{
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    view->width = width;
    view->height = height;
    if (dpi > 0.0f) {
        view->dpi_scale = dpi;
    }
    wn_core_resize(view->core, width, height);
    if (view->ops != NULL && view->renderer != NULL) {
        view->ops->resize(view->renderer, width, height);
    }
    view_mark_geometry(view);
}

static void set_renderer_error(wn_view *view, const char *fallback)
{
    const char *message = NULL;
    if (view->ops != NULL && view->renderer != NULL && view->ops->last_error != NULL) {
        message = view->ops->last_error(view->renderer);
    }
    wn_set_error("%s", message != NULL ? message : fallback);
}

/* ---------------------------------------------------- clipboard (Win32) */
/*
 * The core is platform independent: copy/cut/paste only move text between the
 * value buffer and caller-owned strings. The OS clipboard lives here, so both
 * the embedded host (WN_EVENT_KEY_DOWN through wn_view_event) and the
 * standalone window get Ctrl+C/X/V without extra ABI.
 */
static void view_clipboard_set(const char *text)
{
#if defined(_WIN32)
    int length;
    HGLOBAL memory;
    wchar_t *wide;
    if (text == NULL || !OpenClipboard(NULL)) {
        return;
    }
    EmptyClipboard();
    length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (length > 0) {
        memory = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)length * sizeof(wchar_t));
        if (memory != NULL) {
            wide = (wchar_t *)GlobalLock(memory);
            if (wide != NULL) {
                MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, length);
                GlobalUnlock(memory);
                if (SetClipboardData(CF_UNICODETEXT, memory) == NULL) {
                    GlobalFree(memory);
                }
            } else {
                GlobalFree(memory);
            }
        }
    }
    CloseClipboard();
#else
    (void)text;
#endif
}

static int view_clipboard_get(char *out, int capacity)
{
    int copied = 0;
    if (out == NULL || capacity <= 0) {
        return 0;
    }
    out[0] = '\0';
#if defined(_WIN32)
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(NULL)) {
        return 0;
    }
    {
        HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        if (handle != NULL) {
            const wchar_t *wide = (const wchar_t *)GlobalLock(handle);
            if (wide != NULL) {
                int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
                if (bytes > 1) {
                    char *utf8 = (char *)malloc((size_t)bytes);
                    if (utf8 != NULL) {
                        if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, bytes,
                                                NULL, NULL) > 0) {
                            copied = (int)strlen(utf8);
                            if (copied >= capacity) {
                                copied = capacity - 1;
                            }
                            memcpy(out, utf8, (size_t)copied);
                            out[copied] = '\0';
                        }
                        free(utf8);
                    }
                }
                GlobalUnlock(handle);
            }
        }
    }
    CloseClipboard();
#endif
    return copied;
}

/* Keyboard editing for the focused input. Ctrl+A/C/X/V use the OS clipboard;
 * everything else is core logic. Returns the focused node (or -1). */
static int view_input_key(wn_view *view, const wn_event *event,
                          int *value_changed, int *visual_changed)
{
    char buffer[256];
    int node;
    int copied;
    if (value_changed != NULL) {
        *value_changed = 0;
    }
    if (visual_changed != NULL) {
        *visual_changed = 0;
    }
    node = wn_core_input_focused(view->core);
    if (node < 0) {
        return -1;
    }
    /* While composing the platform owns the text: Ctrl+A/C/X/V and every
     * editing key are suppressed. wn_core_input_key handles Escape as a
     * composition cancel. */
    if (wn_core_input_composition(view->core, NULL, NULL) != 0) {
        return wn_core_input_key(view->core, event->key, event->modifiers,
                                 value_changed, visual_changed);
    }
    if ((event->modifiers & WN_MOD_CTRL) != 0u) {
        if (event->key == WN_KEY_A) {
            return wn_core_input_select_all(view->core, visual_changed);
        }
        if (event->key == WN_KEY_C) {
            copied = wn_core_input_copy(view->core, buffer, (int)sizeof(buffer));
            if (copied > 0) {
                view_clipboard_set(buffer);
            }
            return node;
        }
        if (event->key == WN_KEY_X) {
            copied = wn_core_input_cut(view->core, buffer, (int)sizeof(buffer),
                                       value_changed, visual_changed);
            if (copied > 0) {
                view_clipboard_set(buffer);
            }
            return node;
        }
        if (event->key == WN_KEY_V) {
            if (view_clipboard_get(buffer, (int)sizeof(buffer)) > 0) {
                return wn_core_input_paste(view->core, buffer,
                                           value_changed, visual_changed);
            }
            return node;
        }
        return -1;
    }
    return wn_core_input_key(view->core, event->key, event->modifiers,
                             value_changed, visual_changed);
}

/* ABI v7: IME events carry their UTF-8 payload in `target_id` (the only string
 * field in wn_event) and the composition caret index in `key`; `node` is still
 * the focused input and x/y/width/height the caret rect. The core validates,
 * so a stray event with no focused input is a no-op. */
static int view_input_ime(wn_view *view, const wn_event *event,
                          int *value_changed, int *visual_changed)
{
    return wn_core_input_ime(view->core, (int)event->type, event->target_id,
                             event->key, value_changed, visual_changed);
}

/* -------------------------------------------------- renderer selection */
/*
 * Default on Windows: D3D11 hardware (the Intel/AMD/NVIDIA D3D11 driver is
 * present even when the machine's D3D12 support is missing), falling back to
 * wgpu if init fails. WN_RENDERER=d3d11|wgpu|auto overrides the choice for
 * A/B testing; an explicit value is strict so a test can prove one backend.
 */
static int view_candidates(const wn_renderer_ops **out, int capacity, int *strict)
{
    const char *choice = getenv("WN_RENDERER");
    int count = 0;

    *strict = 0;
    if (choice != NULL && choice[0] != '\0' && strcmp(choice, "auto") != 0) {
        if (strcmp(choice, "d3d11") == 0) {
#if defined(_WIN32)
            if (count < capacity) out[count++] = wn_renderer_d3d11();
            *strict = 1;
            return count;
#else
            fprintf(stderr, "wn_view: WN_RENDERER=d3d11 is only available on Windows\n");
            return 0;
#endif
        }
        if (strcmp(choice, "wgpu") == 0) {
            if (count < capacity) out[count++] = wn_renderer_wgpu();
            *strict = 1;
            return count;
        }
        fprintf(stderr, "wn_view: unknown WN_RENDERER='%s' (expected d3d11|wgpu|auto); "
                        "using auto\n", choice);
    }

#if defined(_WIN32)
    if (count < capacity) out[count++] = wn_renderer_d3d11();
#endif
    if (count < capacity) out[count++] = wn_renderer_wgpu();
    return count;
}

static int view_init_renderer(wn_view *view, const wn_surface_desc *surface,
                              int width, int height, char *errors, size_t errors_cap)
{
    const wn_renderer_ops *candidates[3];
    int strict = 0;
    int count;
    int i;

    count = view_candidates(candidates, 3, &strict);
    errors[0] = '\0';
    for (i = 0; i < count; ++i) {
        const wn_renderer_ops *ops = candidates[i];
        void *renderer = NULL;
        if (ops == NULL || ops->init == NULL) {
            continue;
        }
        if (ops->init(&renderer, surface, width, height) == 0 && renderer != NULL) {
            view->ops = ops;
            view->renderer = renderer;
            return 0;
        }
        {
            const char *detail = ops->last_error != NULL
                                     ? ops->last_error(renderer)
                                     : NULL;
            size_t used = strlen(errors);
            snprintf(errors + used, errors_cap - used, "%s%s: %s",
                     used > 0 ? "; " : "", ops->name,
                     detail != NULL ? detail : "init failed");
        }
        if (renderer != NULL && ops->destroy != NULL) {
            ops->destroy(renderer);
        }
        if (strict) {
            break;
        }
    }
    return 1;
}

/* Common constructor: the surface decides window vs offscreen. */
static wn_view *view_create_with_surface(const wn_surface_desc *surface,
                                         int32_t width, int32_t height,
                                         wn_core *borrowed_core)
{
    wn_view *view;
    char renderer_errors[512];

    view = (wn_view *)calloc(1, sizeof(*view));
    if (view == NULL) {
        wn_set_error("wn_view_create: out of memory");
        return NULL;
    }
    if (borrowed_core != NULL) {
        view->core = borrowed_core;
        view->owns_core = 0;
    } else {
        view->core = wn_core_create(wn_document_builtin());
        view->owns_core = 1;
    }
    if (view->core == NULL) {
        char detail[512];
        snprintf(detail, sizeof(detail), "%s",
                 wn_last_error() != NULL ? wn_last_error() : "core creation failed");
        wn_set_error("wn_view_create: %s", detail);
        free(view);
        return NULL;
    }
    view->native_window = surface->handle;
    view->width = width;
    view->height = height;
    view->dpi_scale = 1.0f;
    view->created_ms = wn_now_ms();

    if (view_init_renderer(view, surface, width, height, renderer_errors,
                           sizeof(renderer_errors)) != 0) {
        wn_set_error("wn_view_create: renderer init failed: %s",
                     renderer_errors[0] != '\0' ? renderer_errors : "unknown");
        wn_core_destroy(view->core);
        free(view);
        return NULL;
    }
    wn_core_resize(view->core, width, height);
    view->geometry_dirty = 1;
    view->upload_dirty = 1;
    return view;
}

wn_view *wn_view_create(void *native_window_handle, int32_t width, int32_t height,
                        uint32_t flags)
{
    wn_surface_desc surface;

    wn_clear_error();
    if (native_window_handle == NULL) {
        wn_set_error("wn_view_create: native_window_handle is NULL");
        return NULL;
    }
    if (width <= 0 || height <= 0) {
        wn_set_error("wn_view_create: width/height must be positive (got %dx%d)",
                     (int)width, (int)height);
        return NULL;
    }
    if (flags != WN_FLAG_NONE) {
        wn_set_error("wn_view_create: flags must be 0 (got 0x%X)", (unsigned)flags);
        return NULL;
    }
    memset(&surface, 0, sizeof(surface));
    surface.kind = WN_SURFACE_HWND;
    surface.handle = native_window_handle;
    surface.instance_handle = NULL;
    surface.width = width;
    surface.height = height;
    return view_create_with_surface(&surface, width, height, NULL);
}

wn_view *wn_view_create_with_core(void *native_window_handle, wn_core *core,
                                  int32_t width, int32_t height)
{
    wn_surface_desc surface;

    wn_clear_error();
    if (native_window_handle == NULL || core == NULL) {
        wn_set_error("wn_view_create_with_core: window and core are required");
        return NULL;
    }
    if (width <= 0 || height <= 0) {
        wn_set_error("wn_view_create_with_core: bad size %dx%d", (int)width, (int)height);
        return NULL;
    }
    memset(&surface, 0, sizeof(surface));
    surface.kind = WN_SURFACE_HWND;
    surface.handle = native_window_handle;
    surface.instance_handle = NULL;
    surface.width = width;
    surface.height = height;
    return view_create_with_surface(&surface, width, height, core);
}

wn_view *wn_view_create_offscreen_with_core(wn_core *core, int32_t width, int32_t height)
{
    wn_surface_desc surface;

    wn_clear_error();
    if (core == NULL) {
        wn_set_error("wn_view_create_offscreen_with_core: core is NULL");
        return NULL;
    }
    if (width <= 0 || height <= 0) {
        wn_set_error("wn_view_create_offscreen_with_core: bad size %dx%d",
                     (int)width, (int)height);
        return NULL;
    }
    memset(&surface, 0, sizeof(surface));
    surface.kind = WN_SURFACE_OFFSCREEN;
    surface.handle = NULL;
    surface.instance_handle = NULL;
    surface.width = width;
    surface.height = height;
    return view_create_with_surface(&surface, width, height, core);
}

wn_view *wn_view_create_offscreen(int32_t width, int32_t height)
{
    wn_surface_desc surface;

    wn_clear_error();
    if (width <= 0 || height <= 0) {
        wn_set_error("wn_view_create_offscreen: width/height must be positive (got %dx%d)",
                     (int)width, (int)height);
        return NULL;
    }
    memset(&surface, 0, sizeof(surface));
    surface.kind = WN_SURFACE_OFFSCREEN;
    surface.handle = NULL;
    surface.instance_handle = NULL;
    surface.width = width;
    surface.height = height;
    return view_create_with_surface(&surface, width, height, NULL);
}

/* Replaces the compiled document: a fresh core is built from it and the
 * previous one is released. The view retains its own reference to `document`
 * (the caller keeps theirs); passing the current document again rebuilds the
 * core without destroying a document the new core already holds. */
int wn_view_replace_document(wn_view *view, wn_document *document)
{
    wn_core *core;
    char *focus_name = NULL;
    int restore_focus = 0;
    struct wn_media_registry *moved_registry = NULL;

    if (view == NULL || document == NULL) {
        wn_set_error("wn_view_replace_document: view and document are required");
        return 1;
    }
    /* Remember the focused field by name: the replacement core has fresh node
     * indices, so an index would point at the wrong node (or a dead slot). */
    if (view->core != NULL) {
        int focused = wn_core_input_focused(view->core);
        if (focused >= 0) {
            const char *name = wn_core_node_name(view->core, focused);
            if (name != NULL) {
                focus_name = wn_strdup_(name);
                restore_focus = focus_name != NULL;
            }
        }
    }
    core = wn_core_create(document);
    if (core == NULL) {
        char detail[512];
        snprintf(detail, sizeof(detail), "%s",
                 wn_last_error() != NULL ? wn_last_error() : "cannot build a core");
        wn_set_error("wn_view_replace_document: %s", detail);
        wn_free_(focus_name);
        return 1;
    }
    if (view->owns_core && view->core != NULL) {
        /* The registry belongs to the view, not to one core: move it before
         * destroying the old core so provider contexts are not destroyed and
         * later reused. */
        moved_registry = wn_core_detach_media_registry(view->core);
        /* The old core blurs its platform IME in wn_core_destroy(). */
        wn_core_destroy(view->core);
    } else if (view->core != NULL) {
        /* Borrowed core: release the platform IME even though the caller keeps
         * ownership of the core itself. */
        wn_core_ime_abort(view->core);
    }
    if (view->document != NULL) {
        wn_document_release((wn_document *)view->document);
    }
    wn_document_retain(document);
    view->core = core;
    view->document = document;
    view->owns_core = 1;
    view->loaded = 1;
    /* The view size is authoritative; a blob compiled at another viewport must
     * not leave the new core laid out for the wrong surface. */
    wn_core_resize(core, view->width, view->height);
    /* ABI v7 regression: the backend belongs to the view, so the new core must
     * receive it before any focus is restored or reported to the platform. */
    wn_core_set_ime(core, view->ime_ops, view->ime_context);
    /* ABI v8: the registry belongs to the view, so a fresh core gets every
     * explicit provider back (media_null is created by the core itself). */
    if (moved_registry != NULL) {
        wn_core_attach_media_registry(core, moved_registry);
    } else {
        view_media_provider_restore(view);
    }
    if (restore_focus) {
        int node = wn_core_find_node(core, focus_name);
        if (node >= 0 && wn_core_focus_input_at_end(core, node) >= 0) {
            view->geometry_dirty = 1;
        }
    }
    wn_free_(focus_name);
    view->geometry_dirty = 1;
    view->upload_dirty = 1;
    return 0;
}

/* Tooling/test surface: states, wheel and counts on the view's own core. */
wn_status wn_view_set_state(wn_view *view, int node, int state, int enabled)
{
    wn_clear_error();
    if (view == NULL || view->core == NULL) {
        wn_set_error("wn_view_set_state: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (state <= 0 || (state & ~(WN_STATE_HOVER | WN_STATE_PRESS |
                                 WN_STATE_SELECTED | WN_STATE_FOCUSED |
                                 WN_STATE_DISABLED | WN_STATE_FOCUS_VISIBLE |
                                 WN_STATE_FOCUS_WITHIN)) != 0) {
        wn_set_error("wn_view_set_state: unknown state 0x%X", (unsigned)state);
        return WN_ERR_INVALID_ARGUMENT;
    }
    /* Derived, not host-settable: the core recomputes it from FOCUSED on every
     * focus, disable and tree change. Accepting it here would let a host leave
     * a stale 0x40 bit behind the next recompute. Fixtures use the internal
     * wn_core_force_state_mask hook instead. */
    if ((state & WN_STATE_FOCUS_WITHIN) != 0) {
        wn_set_error("wn_view_set_state: WN_STATE_FOCUS_WITHIN (0x40) is derived "
                     "from WN_STATE_FOCUSED and cannot be set by the host");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (!wn_core_node_is_alive(view->core, node)) {
        wn_set_error("wn_view_set_state: node %d is not alive", node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if ((state & WN_STATE_HOVER) != 0) {
        wn_core_set_state(view->core, node, WN_STATE_HOVER, enabled);
    }
    if ((state & WN_STATE_PRESS) != 0) {
        wn_core_set_state(view->core, node, WN_STATE_PRESS, enabled);
    }
    if ((state & WN_STATE_SELECTED) != 0) {
        wn_core_set_state(view->core, node, WN_STATE_SELECTED, enabled);
    }
    if ((state & WN_STATE_FOCUSED) != 0) {
        wn_core_set_state(view->core, node, WN_STATE_FOCUSED, enabled);
    }
    if ((state & WN_STATE_FOCUS_VISIBLE) != 0) {
        wn_core_set_state(view->core, node, WN_STATE_FOCUS_VISIBLE, enabled);
    }
    if ((state & WN_STATE_DISABLED) != 0) {
        wn_core_set_state(view->core, node, WN_STATE_DISABLED, enabled);
    }
    view->geometry_dirty = 1;
    view->upload_dirty = 1;
    return WN_OK;
}

wn_status wn_view_set_state_handle(wn_view *view, wn_node_id node, uint32_t state,
                                   int enabled)
{
    int index;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_state: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_set_state: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    return wn_view_set_state(view, index, (int)state, enabled);
}

uint32_t wn_view_get_state_handle(const wn_view *view, wn_node_id node)
{
    int index;
    if (view == NULL) {
        return 0;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        return 0;
    }
    return (uint32_t)wn_core_node_state(view->core, index);
}

int wn_view_wheel_node(wn_view *view, float x, float y, float delta_x, float delta_y)
{
    int container;
    wn_clear_error();
    if (view == NULL || view->core == NULL) {
        wn_set_error("wn_view_wheel: view is NULL");
        return -1;
    }
    container = wn_core_wheel(view->core, x, y, delta_x, delta_y);
    if (container >= 0) {
        view->geometry_dirty = 1;
        view->upload_dirty = 1;
    }
    return container;
}

void wn_view_display_counts(const wn_view *view, int *vertices, int *glyphs, int *images)
{
    const wn_display_list *list;
    if (vertices != NULL) *vertices = 0;
    if (glyphs != NULL) *glyphs = 0;
    if (images != NULL) *images = 0;
    if (view == NULL || view->core == NULL) {
        return;
    }
    list = wn_core_display_list(view->core);
    if (list == NULL) {
        return;
    }
    if (vertices != NULL) *vertices = list->vertex_count;
    if (glyphs != NULL) *glyphs = list->glyph_vertex_count;
    if (images != NULL) *images = list->image_vertex_count;
}

int wn_view_image_cache_count(const wn_view *view)
{
    if (view == NULL || view->ops == NULL || view->renderer == NULL ||
        view->ops->image_cache_count == NULL) {
        return -1;
    }
    return view->ops->image_cache_count(view->renderer);
}

long long wn_view_image_cache_bytes(const wn_view *view)
{
    if (view == NULL || view->ops == NULL || view->renderer == NULL ||
        view->ops->image_cache_bytes == NULL) {
        return -1;
    }
    return view->ops->image_cache_bytes(view->renderer);
}

wn_status wn_view_readback(wn_view *view, const char *png_path)
{
    const wn_renderer_ops *ops;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_view_readback: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (png_path == NULL || png_path[0] == '\0') {
        wn_set_error("wn_view_readback: path is empty");
        return WN_ERR_INVALID_ARGUMENT;
    }
    ops = view->ops;
    if (ops == NULL || ops->readback == NULL) {
        wn_set_error("wn_view_readback: backend has no readback");
        return WN_ERR_UNSUPPORTED;
    }
    if (ops->readback(view->renderer, png_path) != 0) {
        const char *error = ops->last_error != NULL
                                ? ops->last_error(view->renderer)
                                : NULL;
        wn_set_error("wn_view_readback: %s", error != NULL ? error : "backend error");
        return WN_ERR_GPU;
    }
    return WN_OK;
}

wn_status wn_view_load(wn_view *view, const char *html_utf8,
                       const char *base_url, const char *css_utf8)
{
    char *html;
    char *base = NULL;
    char *css = NULL;

    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_view_load: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (html_utf8 == NULL) {
        wn_set_error("wn_view_load: html_utf8 is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    html = wn_strdup_(html_utf8);
    if (html == NULL) {
        wn_set_error("wn_view_load: out of memory copying html");
        return WN_ERR_OUT_OF_MEMORY;
    }
    if (base_url != NULL) {
        base = wn_strdup_(base_url);
        if (base == NULL) {
            wn_free_(html);
            wn_set_error("wn_view_load: out of memory copying base_url");
            return WN_ERR_OUT_OF_MEMORY;
        }
    }
    if (css_utf8 != NULL) {
        css = wn_strdup_(css_utf8);
        if (css == NULL) {
            wn_free_(html);
            wn_free_(base);
            wn_set_error("wn_view_load: out of memory copying css");
            return WN_ERR_OUT_OF_MEMORY;
        }
    }

    wn_free_(view->html);
    wn_free_(view->base_url);
    wn_free_(view->css);
    view->html = html;
    view->base_url = base;
    view->css = css;
    view->loaded = 1;
    view->closed = 0;

    /* v0 attaches the compiled R2 document. The strings are retained as the
     * document source/diagnostics until a real runtime parser exists. */
    wn_core_resize(view->core, view->width, view->height);
    wn_core_relayout(view->core);
    view->geometry_dirty = 1;
    view->upload_dirty = 1;
    return WN_OK;
}

wn_status wn_view_resize(wn_view *view, int32_t width, int32_t height, float dpi_scale)
{
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_view_resize: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (width < 0 || height < 0 || width > 16384 || height > 16384) {
        wn_set_error("wn_view_resize: invalid size %dx%d", (int)width, (int)height);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_resize_internal(view, width, height, dpi_scale);
    return WN_OK;
}

wn_status wn_view_event(wn_view *view, const wn_event *event)
{
    wn_event out;
    const char *target = NULL;
    int hit = -1;
    int event_node = -1;
    int changed = 0;
    int input_value_changed = 0;
    int input_visual_changed = 0;
    uint8_t button = 0;
    int ime_event = 0;

    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_view_event: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (event == NULL) {
        wn_set_error("wn_view_event: event is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (view->destroy_pending) {
        wn_set_error("wn_view_event: view is being destroyed");
        return WN_ERR_STATE;
    }
    out = *event;
    out.target_id = NULL;
    out.node = WN_NODE_INVALID;
    button = event->button;
    ime_event = event->type == WN_EVENT_IME_START ||
                event->type == WN_EVENT_IME_COMPOSITION ||
                event->type == WN_EVENT_IME_COMMIT ||
                event->type == WN_EVENT_IME_CANCEL;

    switch (event->type) {
    case WN_EVENT_POINTER_MOVE: {
        int previous_hover = wn_core_hover(view->core);
        /* The Win32 host reports leave/capture-loss as an out-of-window move. */
        if (event->x <= -100000.0f || event->y <= -100000.0f) {
            wn_core_pointer_leave(view->core);
            hit = -1;
            changed = previous_hover >= 0;
            break;
        }
        hit = wn_core_pointer_move(view->core, event->x, event->y);
        event_node = hit;
        changed = hit != previous_hover;
        break;
    }
    case WN_EVENT_POINTER_DOWN:
        if (button == 0 || button == 1) {
            hit = wn_core_pointer_down(view->core, event->x, event->y);
            changed = 1;
        } else {
            /* Secondary buttons do not press/select, but the event still
             * targets the interactive node (or the topmost node) under it. */
            hit = wn_core_hit_test(view->core, event->x, event->y);
        }
        event_node = hit >= 0 ? hit
                              : wn_core_node_at(view->core, event->x, event->y);
        break;
    case WN_EVENT_POINTER_UP:
        if (button == 0 || button == 1) {
            hit = wn_core_pointer_up(view->core, event->x, event->y);
            changed = 1;
        } else {
            hit = wn_core_hit_test(view->core, event->x, event->y);
        }
        event_node = hit >= 0 ? hit
                              : wn_core_node_at(view->core, event->x, event->y);
        break;
    case WN_EVENT_WHEEL:
        hit = wn_core_wheel(view->core, event->x, event->y,
                            event->delta_x, event->delta_y);
        event_node = hit;
        changed = hit >= 0;
        break;
    case WN_EVENT_CHAR:
        hit = wn_core_input_char(view->core, event->codepoint,
                                 &input_value_changed, &input_visual_changed);
        event_node = hit;
        changed = input_visual_changed;
        break;
    case WN_EVENT_KEY_DOWN:
        hit = view_input_key(view, event, &input_value_changed,
                             &input_visual_changed);
        event_node = hit;
        changed = input_visual_changed;
        break;
    case WN_EVENT_FOCUS_OUT:
        if (wn_core_blur_input(view->core) == 0) {
            changed = 1;
        }
        break;
    case WN_EVENT_IME_START:
    case WN_EVENT_IME_COMPOSITION:
    case WN_EVENT_IME_COMMIT:
    case WN_EVENT_IME_CANCEL:
        hit = view_input_ime(view, event, &input_value_changed,
                             &input_visual_changed);
        event_node = hit;
        changed = input_visual_changed;
        break;
    case WN_EVENT_RESIZE:
        view_resize_internal(view, event->width, event->height, 0.0f);
        break;
    case WN_EVENT_CLOSE:
        view->closed = 1;
        break;
    case WN_EVENT_KEY_UP:
    case WN_EVENT_FOCUS_IN:
    default:
        break;
    }
    if (event_node >= 0) {
        /* ABI v5: the handle is the identity for every node, clones included.
         * target_id keeps exposing compiled element ids only: clones carry
         * synthetic arena names ("clone#N") that are not document ids. */
        out.node = wn_view_handle(view, event_node);
        if (!wn_core_node_is_dynamic(view->core, event_node)) {
            target = wn_core_node_name(view->core, event_node);
        }
    }
    out.target_id = ime_event ? event->target_id : target;
    if (changed) {
        view_mark_geometry(view);
    }
    if (view->callback != NULL) {
        wn_event_cb callback = view->callback;
        void *userdata = view->userdata;

        view->dispatch_depth += 1;
        callback(userdata, &out);
        if (input_value_changed && !view->dispatch_destroyed) {
            wn_event semantic = out;
            semantic.type = WN_EVENT_INPUT;
            semantic.target_id = target;
            callback(userdata, &semantic);
        }
        view->dispatch_depth -= 1;
        if (view->dispatch_depth == 0 && view->destroy_pending) {
            view_free(view);
            return WN_OK;
        }
    }
    return WN_OK;
}

/* ABI v8: drain queued media events on the UI thread. The callback may destroy
 * the view, so it runs inside the same dispatch guard as wn_view_event(). */
static void view_flush_media_events(wn_view *view)
{
    int node;
    int type;
    int state;
    int error;
    int64_t position_us;
    int64_t duration_us;
    const char *detail;
    while (view != NULL && view->core != NULL &&
           wn_core_media_take_event(view->core, &node, &type, &state, &error,
                                    &position_us, &duration_us, &detail)) {
        wn_media_event event;
        wn_media_event_cb callback = view->media_callback;
        void *userdata = view->media_userdata;
        if (callback == NULL) {
            continue; /* no consumer: discard instead of growing the ring */
        }
        memset(&event, 0, sizeof(event));
        event.struct_size = (uint32_t)sizeof(event);
        event.type = (uint32_t)type;
        event.node = wn_view_handle(view, node);
        event.state = state;
        event.error = error;
        event.position_us = position_us;
        event.duration_us = duration_us;
        event.detail = detail;
        view->dispatch_depth += 1;
        callback(userdata, &event);
        view->dispatch_depth -= 1;
        if (view->dispatch_depth == 0 && view->destroy_pending) {
            view_free(view);
            return;
        }
        if (view->dispatch_destroyed) {
            return;
        }
    }
}

wn_status wn_view_render(wn_view *view)
{
    const wn_display_list *list;
    wn_renderer_stats renderer_stats;
    double t0;
    double t1;
    double t_upload;
    double cpu_ms = 0.0;

    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_view_render: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (!view->loaded) {
        wn_set_error("wn_view_render: document not loaded (call wn_view_load first)");
        return WN_ERR_STATE;
    }
    if (view->width <= 0 || view->height <= 0) {
        return WN_OK;
    }

    t0 = wn_now_ms();
    if (wn_core_dirty(view->core)) {
        /* Host mutations mark the core dirty; make sure the display list is
         * rebuilt even when the setter did not touch view->geometry_dirty. */
        view->geometry_dirty = 1;
    }
    if (wn_core_needs_layout(view->core)) {
        wn_core_relayout(view->core);
        view->geometry_dirty = 1;
    }
    if (wn_core_input_focused(view->core) >= 0) {
        /* The caret blink phase is time based and lives outside the cached
         * display list, so a focused input rebuilds it on every frame. Resize,
         * scroll and reflow also move the caret without a blink reset, so sync
         * the platform IME boxes here before the list is rebuilt. */
        wn_core_ime_sync(view->core);
        view->geometry_dirty = 1;
    }
    if (view->geometry_dirty) {
        wn_core_build_display_list(view->core);
        view->geometry_dirty = 0;
        view->upload_dirty = 1;
    }
    list = wn_core_display_list(view->core);
    t_upload = wn_now_ms();
    if (view->upload_dirty) {
        if (view->ops->upload(view->renderer, list) != 0) {
            set_renderer_error(view, "display list upload failed");
            return WN_ERR_GPU;
        }
        view->upload_dirty = 0;
    }
    cpu_ms += wn_now_ms() - t_upload;

    if (view->ops->render(view->renderer, list) != 0) {
        set_renderer_error(view, "render/present failed");
        return WN_ERR_GPU;
    }
    if (view->ops->get_stats != NULL) {
        memset(&renderer_stats, 0, sizeof(renderer_stats));
        view->ops->get_stats(view->renderer, &renderer_stats);
        cpu_ms += renderer_stats.last_cpu_ms;
        view->stats.image_failures = renderer_stats.image_failures;
    }
    t1 = wn_now_ms();
    if (!view->have_frame) {
        view->have_frame = 1;
        view->first_frame_ms = t1;
        view->stats.startup_ms = t1 - view->created_ms;
    }
    view->stats.frames += 1;
    view->stats.frame_total_ms += t1 - t0;
    view->stats.cpu_frame_total_ms += cpu_ms;
    view->stats.last_render_ms = t1 - t0;
    view->stats.last_layout_ms = wn_core_last_layout_ms(view->core);
    view_flush_media_events(view);
    return WN_OK;
}

wn_status wn_view_on_event(wn_view *view, wn_event_cb callback, void *userdata)
{
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_view_on_event: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    view->callback = callback;
    view->userdata = userdata;
    return WN_OK;
}

void wn_view_destroy(wn_view *view)
{
    if (view == NULL) {
        return;
    }
    wn_clear_error();
    if (view->destroy_pending) {
        return; /* already scheduled at the end of the dispatch */
    }
    if (view->dispatch_depth > 0) {
        /* A callback may destroy the view (or its app) while the dispatch is
         * still using the struct. Mark it and let wn_view_event() free it once
         * the outermost dispatch returns. */
        view->dispatch_destroyed = 1;
        view->destroy_pending = 1;
        return;
    }
    view_free(view);
}

int wn_view_needs_render(const wn_view *view)
{
    if (view == NULL || !view->loaded) {
        return 0;
    }
    if (view->width <= 0 || view->height <= 0) {
        return 0;
    }
    if (!view->have_frame || view->geometry_dirty || view->upload_dirty) {
        return 1;
    }
    if (wn_core_needs_layout(view->core) || wn_core_dirty(view->core)) {
        return 1;
    }
    return 0;
}

int wn_view_get_stats(const wn_view *view, wn_view_stats *stats)
{
    if (view == NULL || stats == NULL) {
        return 1;
    }
    *stats = view->stats;
    return 0;
}

void wn_view_reset_stats(wn_view *view)
{
    if (view == NULL) {
        return;
    }
    memset(&view->stats, 0, sizeof(view->stats));
    view->have_frame = 0;
    view->first_frame_ms = 0.0;
    view->created_ms = wn_now_ms();
}

const char *wn_view_backend(const wn_view *view)
{
    static char backend[32];
    wn_renderer_info info;
    if (view == NULL || view->ops == NULL || view->renderer == NULL ||
        view->ops->get_info == NULL) {
        return NULL;
    }
    memset(&info, 0, sizeof(info));
    view->ops->get_info(view->renderer, &info);
    memcpy(backend, info.backend, sizeof(backend));
    backend[sizeof(backend) - 1] = '\0';
    return backend[0] != '\0' ? backend : NULL;
}

const char *wn_view_device(const wn_view *view)
{
    static char device[256];
    wn_renderer_info info;
    if (view == NULL || view->ops == NULL || view->renderer == NULL ||
        view->ops->get_info == NULL) {
        return NULL;
    }
    memset(&info, 0, sizeof(info));
    view->ops->get_info(view->renderer, &info);
    memcpy(device, info.device, sizeof(device));
    device[sizeof(device) - 1] = '\0';
    return device[0] != '\0' ? device : NULL;
}

int wn_view_render_width(const wn_view *view)
{
    return view != NULL ? view->width : 0;
}

int wn_view_render_height(const wn_view *view)
{
    return view != NULL ? view->height : 0;
}

wn_status wn_view_set_text(wn_view *view, const char *id, const char *text_utf8)
{
    int node;
    int result;
    wn_clear_error();
    if (view == NULL || id == NULL || text_utf8 == NULL) {
        wn_set_error("wn_view_set_text: view, id and text are required");
        return WN_ERR_INVALID_ARGUMENT;
    }
    node = wn_core_find_node(view->core, id);
    if (node < 0) {
        wn_set_error("wn_view_set_text: node '%s' not found", id);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (!wn_core_node_is_text(view->core, node)) {
        wn_set_error("wn_view_set_text: node '%s' is not a text node", id);
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_set_text(view->core, node, text_utf8);
    if (result == 1) {
        wn_set_error("wn_view_set_text: '%s' cannot be rendered with the pre-shaped "
                     "glyphs of '%s' (runtime shaping is out of v0)", text_utf8, id);
        return WN_ERR_UNSUPPORTED;
    }
    if (result != 0) {
        wn_set_error("wn_view_set_text: invalid text node '%s'", id);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_set_class(wn_view *view, const char *id, const char *class_name,
                            int enabled)
{
    int node;
    int result;
    wn_clear_error();
    if (view == NULL || id == NULL || class_name == NULL || class_name[0] == '\0') {
        wn_set_error("wn_view_set_class: view, id and class_name are required");
        return WN_ERR_INVALID_ARGUMENT;
    }
    node = wn_core_find_node(view->core, id);
    if (node < 0) {
        wn_set_error("wn_view_set_class: node '%s' not found", id);
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_set_class(view->core, node, class_name, enabled);
    if (result != 0) {
        wn_set_error("wn_view_set_class: cannot %s class '%s' on '%s'",
                     enabled ? "add" : "remove", class_name, id);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

/* ---------------------------------------------------- dynamic UI (ABI v3) */
/*
 * wn_node_id = (generation << 32) | (index + 1). The generation comes from the
 * arena slot, so a handle kept across a removal does not resolve once the slot
 * is reused. Both halves are required: 0 is WN_NODE_INVALID.
 */
wn_node_id wn_view_handle(const wn_view *view, int index)
{
    uint32_t generation;
    if (view == NULL || view->core == NULL || index < 0) {
        return WN_NODE_INVALID;
    }
    generation = wn_core_node_generation(view->core, index);
    if (generation == 0) {
        return WN_NODE_INVALID;
    }
    return ((wn_node_id)generation << 32) |
           (wn_node_id)(uint32_t)(index + 1);
}

int wn_view_resolve_handle(const wn_view *view, wn_node_id node)
{
    uint32_t generation;
    uint32_t index;
    if (view == NULL || view->core == NULL || node == WN_NODE_INVALID) {
        return -1;
    }
    index = (uint32_t)(node & 0xFFFFFFFFu);
    generation = (uint32_t)(node >> 32);
    if (index == 0 || index > (uint32_t)INT32_MAX || generation == 0) {
        return -1;
    }
    if (!wn_core_node_is_alive(view->core, (int)(index - 1))) {
        return -1;
    }
    if (wn_core_node_generation(view->core, (int)(index - 1)) != generation) {
        return -1;
    }
    return (int)(index - 1);
}

static int view_node_index(const wn_view *view, wn_node_id node)
{
    return wn_view_resolve_handle(view, node);
}

static wn_node_id view_node_handle(const wn_view *view, int index)
{
    return wn_view_handle(view, index);
}

wn_node_id wn_view_find(wn_view *view, const char *name)
{
    int node;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_app_find: view is NULL");
        return WN_NODE_INVALID;
    }
    if (name == NULL || name[0] == '\0') {
        wn_set_error("wn_app_find: name is NULL or empty");
        return WN_NODE_INVALID;
    }
    node = wn_core_find_node(view->core, name);
    if (node < 0) {
        wn_set_error("wn_app_find: node '%s' not found", name);
        return WN_NODE_INVALID;
    }
    return view_node_handle(view, node);
}

wn_node_id wn_view_root(wn_view *view)
{
    int i;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_app_root: view is NULL");
        return WN_NODE_INVALID;
    }
    for (i = 0; i < wn_core_node_count(view->core); ++i) {
        if (wn_core_node_is_alive(view->core, i) &&
            wn_core_node_parent(view->core, i) < 0) {
            return view_node_handle(view, i);
        }
    }
    wn_set_error("wn_app_root: the document has no root");
    return WN_NODE_INVALID;
}

int32_t wn_view_node_count(wn_view *view)
{
    if (view == NULL) {
        return 0;
    }
    return (int32_t)wn_core_live_node_count(view->core);
}

const wn_core *wn_view_core(const wn_view *view)
{
    return view != NULL ? view->core : NULL;
}

wn_status wn_view_set_visible(wn_view *view, wn_node_id node, int visible)
{
    int index;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_visible: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_set_visible: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (wn_core_set_visible(view->core, index, visible) != 0) {
        wn_set_error("wn_node_set_visible: node handle %llu is not alive",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_set_image(wn_view *view, wn_node_id node, const char *path)
{
    int index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_image: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_set_image: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (path == NULL || path[0] == '\0') {
        wn_set_error("wn_node_set_image: path is NULL or empty");
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_set_image(view->core, index, path);
    if (result == 1) {
        wn_set_error("wn_node_set_image: node handle %llu is not an image node",
                     (unsigned long long)node);
        return WN_ERR_UNSUPPORTED;
    }
    if (result == -2) {
        wn_set_error("wn_node_set_image: out of memory copying '%s'", path);
        return WN_ERR_OUT_OF_MEMORY;
    }
    if (result != 0) {
        wn_set_error("wn_node_set_image: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_set_node_text(wn_view *view, wn_node_id node, const char *utf8)
{
    int index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_text: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_set_text: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (utf8 == NULL) {
        wn_set_error("wn_node_set_text: utf8 is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (!wn_core_node_is_text(view->core, index)) {
        wn_set_error("wn_node_set_text: node handle %llu is not a text node",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_set_text(view->core, index, utf8);
    if (result == 1) {
        wn_set_error("wn_node_set_text: '%s' cannot be rendered with the pre-shaped "
                     "glyphs of node %llu (runtime shaping is out of v0)",
                     utf8, (unsigned long long)node);
        return WN_ERR_UNSUPPORTED;
    }
    if (result != 0) {
        wn_set_error("wn_node_set_text: invalid text node %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_node_id wn_view_clone(wn_view *view, wn_node_id template_node, wn_node_id parent)
{
    int template_index;
    int parent_index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_clone: view is NULL");
        return WN_NODE_INVALID;
    }
    template_index = view_node_index(view, template_node);
    if (template_index < 0) {
        wn_set_error("wn_node_clone: invalid template handle %llu",
                     (unsigned long long)template_node);
        return WN_NODE_INVALID;
    }
    parent_index = view_node_index(view, parent);
    if (parent_index < 0) {
        wn_set_error("wn_node_clone: invalid parent handle %llu",
                     (unsigned long long)parent);
        return WN_NODE_INVALID;
    }
    result = wn_core_clone(view->core, template_index, parent_index);
    if (result == -2) {
        wn_set_error("wn_node_clone: cannot grow the node arena (%d slots)",
                     wn_core_node_count(view->core));
        return WN_NODE_INVALID;
    }
    if (result < 0) {
        wn_set_error("wn_node_clone: cannot clone handle %llu under %llu",
                     (unsigned long long)template_node,
                     (unsigned long long)parent);
        return WN_NODE_INVALID;
    }
    view_mark_geometry(view);
    return view_node_handle(view, result);
}

wn_status wn_view_remove(wn_view *view, wn_node_id node)
{
    int index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_remove: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_remove: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_remove(view->core, index);
    if (result == -2) {
        wn_set_error("wn_node_remove: out of memory");
        return WN_ERR_OUT_OF_MEMORY;
    }
    if (result != 0) {
        wn_set_error("wn_node_remove: cannot remove handle %llu (root or invalid)",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_clear_children(wn_view *view, wn_node_id node)
{
    int index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_clear_children: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_clear_children: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_clear_children(view->core, index);
    if (result != 0) {
        wn_set_error("wn_node_clear_children: cannot clear handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_append(wn_view *view, wn_node_id parent, wn_node_id child)
{
    int parent_index;
    int child_index;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_append: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    parent_index = view_node_index(view, parent);
    if (parent_index < 0) {
        wn_set_error("wn_node_append: invalid parent handle %llu",
                     (unsigned long long)parent);
        return WN_ERR_INVALID_ARGUMENT;
    }
    child_index = view_node_index(view, child);
    if (child_index < 0) {
        wn_set_error("wn_node_append: invalid child handle %llu",
                     (unsigned long long)child);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (wn_core_append(view->core, parent_index, child_index) != 0) {
        wn_set_error("wn_node_append: cannot append handle %llu to %llu "
                     "(root, self or cycle)",
                     (unsigned long long)child, (unsigned long long)parent);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

/* ------------------------------------------------------ text input (v4) */

int wn_view_input_focused(const wn_view *view)
{
    if (view == NULL) {
        return 0;
    }
    return wn_core_input_focused(view->core) >= 0;
}

wn_status wn_view_set_value(wn_view *view, wn_node_id node, const char *utf8)
{
    int index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_value: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_set_value: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (utf8 == NULL) {
        wn_set_error("wn_node_set_value: utf8 is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (!wn_core_node_is_input(view->core, index)) {
        wn_set_error("wn_node_set_value: node handle %llu is not a text input",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_set_input_value(view->core, index, utf8);
    if (result == 1) {
        wn_set_error("wn_node_set_value: '%s' is not valid UTF-8 or contains a "
                     "control character (tab is allowed)", utf8);
        return WN_ERR_UNSUPPORTED;
    }
    if (result == 2) {
        wn_set_error("wn_node_set_value: '%s' exceeds the input max_length or byte capacity",
                     utf8);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (result != 0) {
        wn_set_error("wn_node_set_value: invalid input node %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

const char *wn_view_get_value(wn_view *view, wn_node_id node)
{
    int index;
    const char *value;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_get_value: view is NULL");
        return NULL;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_get_value: invalid node handle %llu",
                     (unsigned long long)node);
        return NULL;
    }
    if (!wn_core_node_is_input(view->core, index)) {
        wn_set_error("wn_node_get_value: node handle %llu is not a text input",
                     (unsigned long long)node);
        return NULL;
    }
    value = wn_core_input_value(view->core, index);
    if (value == NULL) {
        wn_set_error("wn_node_get_value: input %llu has no value",
                     (unsigned long long)node);
        return NULL;
    }
    wn_free_(view->value_scratch);
    view->value_scratch = wn_strdup_(value);
    if (view->value_scratch == NULL) {
        wn_set_error("wn_node_get_value: out of memory");
        return NULL;
    }
    return view->value_scratch;
}

/* ---------------------------------------------------- node reads (ABI v6) */

const char *wn_view_get_text(wn_view *view, wn_node_id node)
{
    int index;
    const char *text;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_get_text: view is NULL");
        return NULL;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_get_text: invalid node handle %llu",
                     (unsigned long long)node);
        return NULL;
    }
    text = wn_core_node_text(view->core, index);
    if (text == NULL) {
        wn_set_error("wn_node_get_text: node handle %llu has no text",
                     (unsigned long long)node);
        return NULL;
    }
    wn_free_(view->text_scratch);
    view->text_scratch = wn_strdup_(text);
    if (view->text_scratch == NULL) {
        wn_set_error("wn_node_get_text: out of memory");
        return NULL;
    }
    return view->text_scratch;
}

wn_status wn_view_get_rect(wn_view *view, wn_node_id node, wn_rect *out)
{
    int index;
    wn_clear_error();
    if (view == NULL || out == NULL) {
        wn_set_error("wn_node_get_rect: view and out are required");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_get_rect: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (wn_core_node_rect(view->core, index, out) != 0) {
        wn_set_error("wn_node_get_rect: cannot lay out node %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    return WN_OK;
}

wn_status wn_view_scroll_into_view(wn_view *view, wn_node_id node, float margin)
{
    int index;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_scroll_into_view: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_scroll_into_view: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (wn_core_scroll_node_into_view(view->core, index, margin) != 0) {
        wn_set_error("wn_node_scroll_into_view: cannot scroll node %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_focus_input(wn_view *view, wn_node_id node)
{
    int index;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_app_focus_input: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_app_focus_input: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (wn_core_focus_input_at_end(view->core, index) < 0) {
        wn_set_error("wn_app_focus_input: node handle %llu is not a focused-capable "
                     "text input (missing, disabled or not an input)",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_blur_input(wn_view *view)
{
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_app_blur_input: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    (void)wn_core_blur_input(view->core);
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_set_image_size(wn_view *view, wn_node_id node,
                                 float width, float height)
{
    int index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_image_size: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_set_image_size: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_set_image_size(view->core, index, width, height);
    if (result == 1) {
        wn_set_error("wn_node_set_image_size: node handle %llu is not an image node",
                     (unsigned long long)node);
        return WN_ERR_UNSUPPORTED;
    }
    if (result != 0) {
        wn_set_error("wn_node_set_image_size: invalid size %.2fx%.2f",
                     (double)width, (double)height);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_set_size(wn_view *view, wn_node_id node,
                           float width, float height)
{
    int index;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_size: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_set_size: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (wn_core_set_size(view->core, index, width, height) != 0) {
        wn_set_error("wn_node_set_size: invalid size %.2fx%.2f",
                     (double)width, (double)height);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_set_position(wn_view *view, wn_node_id node, float x, float y)
{
    int index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_position: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("wn_node_set_position: invalid node handle %llu",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_set_position(view->core, index, x, y);
    if (result == 1) {
        wn_set_error("wn_node_set_position: node handle %llu is not positioned",
                     (unsigned long long)node);
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (result != 0) {
        wn_set_error("wn_node_set_position: invalid offset %.2f,%.2f",
                     (double)x, (double)y);
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_mark_geometry(view);
    return WN_OK;
}

/* ------------------------------------------------ style overrides (ABI v7) */

static wn_status view_style_status(int result, const char *call, wn_node_id node)
{
    if (result == -2) {
        wn_set_error("%s: out of memory for node %llu", call,
                     (unsigned long long)node);
        return WN_ERR_OUT_OF_MEMORY;
    }
    wn_set_error("%s: invalid node or value for handle %llu", call,
                 (unsigned long long)node);
    return WN_ERR_INVALID_ARGUMENT;
}

wn_status wn_view_set_color(wn_view *view, wn_node_id node, uint32_t argb)
{
    int index;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_color: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0 || wn_core_set_color(view->core, index, argb) != 0) {
        return view_style_status(-1, "wn_node_set_color", node);
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_set_background(wn_view *view, wn_node_id node, uint32_t argb)
{
    int index;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_background: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0 || wn_core_set_background(view->core, index, argb) != 0) {
        return view_style_status(-1, "wn_node_set_background", node);
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_set_font_family(wn_view *view, wn_node_id node,
                                  const char *family)
{
    int index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_font_family: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        return view_style_status(-1, "wn_node_set_font_family", node);
    }
    result = wn_core_set_font_family(view->core, index, family);
    if (result != 0) {
        return view_style_status(result, "wn_node_set_font_family", node);
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_set_font_size(wn_view *view, wn_node_id node, float size)
{
    int index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_font_size: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        return view_style_status(-1, "wn_node_set_font_size", node);
    }
    result = wn_core_set_font_size(view->core, index, size);
    if (result != 0) {
        return view_style_status(result, "wn_node_set_font_size", node);
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_set_font_weight(wn_view *view, wn_node_id node, int weight)
{
    int index;
    int result;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_set_font_weight: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        return view_style_status(-1, "wn_node_set_font_weight", node);
    }
    result = wn_core_set_font_weight(view->core, index, weight);
    if (result != 0) {
        return view_style_status(result, "wn_node_set_font_weight", node);
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_reset_style(wn_view *view, wn_node_id node, uint32_t fields)
{
    int index;
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_node_reset_style: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_node_index(view, node);
    if (index < 0 ||
        wn_core_reset_style(view->core, index, (unsigned int)fields) != 0) {
        return view_style_status(-1, "wn_node_reset_style", node);
    }
    view_mark_geometry(view);
    return WN_OK;
}

wn_status wn_view_set_font_family_tree(wn_view *view, const char *family,
                                       int recursive)
{
    int root = -1;
    int i;
    int result;
    wn_clear_error();
    if (view == NULL || view->core == NULL) {
        wn_set_error("wn_app_set_font_family: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < wn_core_node_count(view->core); ++i) {
        if (wn_core_node_is_alive(view->core, i) &&
            wn_core_node_parent(view->core, i) < 0) {
            root = i;
            break;
        }
    }
    if (root < 0) {
        wn_set_error("wn_app_set_font_family: the document has no root");
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_set_font_family_tree(view->core, root, family, recursive);
    if (result != 0) {
        return view_style_status(result, "wn_app_set_font_family",
                                 WN_NODE_INVALID);
    }
    view_mark_geometry(view);
    return WN_OK;
}

/* ---------------------------------------------------------- media (v8) */

static int view_media_index(wn_view *view, wn_node_id node, const char *label)
{
    int index;
    if (view == NULL || view->core == NULL) {
        wn_set_error("%s: view is NULL", label);
        return -1;
    }
    index = view_node_index(view, node);
    if (index < 0) {
        wn_set_error("%s: node handle is not alive", label);
        return -1;
    }
    if (!wn_core_node_is_video(view->core, index)) {
        wn_set_error("%s: node is not a video node", label);
        return -2;
    }
    return index;
}

static wn_status view_media_status(int result, const char *label)
{
    if (result == 0) {
        return WN_OK;
    }
    if (result == -2) {
        return WN_ERR_STATE;
    }
    if (result == -1) {
        wn_set_error("%s: invalid node or value", label);
    }
    return WN_ERR_INVALID_ARGUMENT;
}

wn_status wn_view_register_media_provider(wn_view *view,
        const wn_media_provider_ops *ops, void *context, int32_t priority)
{
    int result;
    wn_clear_error();
    if (view == NULL || view->core == NULL) {
        wn_set_error("wn_view_register_media_provider: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_register_media_provider(view->core, ops, context, priority);
    if (result == -2) {
        return WN_ERR_OUT_OF_MEMORY;
    }
    if (result != 0) {
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (view_media_provider_remember(view, ops, context, priority) != 0) {
        (void)wn_core_unregister_media_provider(view->core, ops, context);
        return WN_ERR_OUT_OF_MEMORY;
    }
    return WN_OK;
}

wn_status wn_view_unregister_media_provider(wn_view *view,
        const wn_media_provider_ops *ops, void *context)
{
    int result;
    wn_clear_error();
    if (view == NULL || view->core == NULL) {
        wn_set_error("wn_view_unregister_media_provider: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_unregister_media_provider(view->core, ops, context);
    if (result == 1) {
        wn_set_error("wn_view_unregister_media_provider: provider is not "
                     "registered");
        return WN_ERR_INVALID_ARGUMENT;
    }
    if (result != 0) {
        return WN_ERR_INVALID_ARGUMENT;
    }
    view_media_provider_forget(view, ops, context);
    return WN_OK;
}

wn_status wn_view_load_media_provider(wn_view *view, const char *dll_path)
{
    int result;
    wn_clear_error();
    if (view == NULL || view->core == NULL) {
        wn_set_error("wn_view_load_media_provider: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_load_media_provider(view->core, dll_path);
    if (result != 0) {
        return WN_ERR_IO;
    }
    /* Mirror the plugin so a document replacement keeps it. */
    {
        int count = wn_core_media_provider_count(view->core);
        int i;
        for (i = 0; i < count; ++i) {
            const wn_media_provider_ops *ops;
            void *context = NULL;
            int32_t priority = 0;
            if (wn_core_media_provider_is_null(view->core, i)) {
                break;
            }
            ops = wn_core_media_provider_at(view->core, i, &context, &priority);
            if (ops != NULL &&
                view_media_provider_find(view, ops, context) < 0) {
                if (view_media_provider_remember(view, ops, context, priority) !=
                    0) {
                    return WN_ERR_OUT_OF_MEMORY;
                }
            }
        }
    }
    return WN_OK;
}

wn_status wn_view_on_media_event(wn_view *view, wn_media_event_cb cb,
                                 void *userdata)
{
    wn_clear_error();
    if (view == NULL) {
        wn_set_error("wn_view_on_media_event: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    view->media_callback = cb;
    view->media_userdata = userdata;
    return WN_OK;
}

wn_status wn_view_media_open(wn_view *view, wn_node_id node)
{
    int index;
    wn_clear_error();
    index = view_media_index(view, node, "wn_node_media_open");
    if (index < 0) {
        return index == -2 ? WN_ERR_UNSUPPORTED : WN_ERR_INVALID_ARGUMENT;
    }
    return view_media_status(wn_core_media_open(view->core, index),
                             "wn_node_media_open");
}

wn_status wn_view_media_play(wn_view *view, wn_node_id node)
{
    int index;
    wn_clear_error();
    index = view_media_index(view, node, "wn_node_media_play");
    if (index < 0) {
        return index == -2 ? WN_ERR_UNSUPPORTED : WN_ERR_INVALID_ARGUMENT;
    }
    return view_media_status(wn_core_media_play(view->core, index),
                             "wn_node_media_play");
}

wn_status wn_view_media_pause(wn_view *view, wn_node_id node)
{
    int index;
    wn_clear_error();
    index = view_media_index(view, node, "wn_node_media_pause");
    if (index < 0) {
        return index == -2 ? WN_ERR_UNSUPPORTED : WN_ERR_INVALID_ARGUMENT;
    }
    return view_media_status(wn_core_media_pause(view->core, index),
                             "wn_node_media_pause");
}

wn_status wn_view_media_seek(wn_view *view, wn_node_id node,
                             int64_t position_us)
{
    int index;
    wn_clear_error();
    index = view_media_index(view, node, "wn_node_media_seek");
    if (index < 0) {
        return index == -2 ? WN_ERR_UNSUPPORTED : WN_ERR_INVALID_ARGUMENT;
    }
    return view_media_status(wn_core_media_seek(view->core, index, position_us),
                             "wn_node_media_seek");
}

wn_status wn_view_media_set_volume(wn_view *view, wn_node_id node, float gain)
{
    int index;
    wn_clear_error();
    index = view_media_index(view, node, "wn_node_media_set_volume");
    if (index < 0) {
        return index == -2 ? WN_ERR_UNSUPPORTED : WN_ERR_INVALID_ARGUMENT;
    }
    return view_media_status(wn_core_media_set_volume(view->core, index, gain),
                             "wn_node_media_set_volume");
}

wn_status wn_view_media_set_muted(wn_view *view, wn_node_id node, int muted)
{
    int index;
    wn_clear_error();
    index = view_media_index(view, node, "wn_node_media_set_muted");
    if (index < 0) {
        return index == -2 ? WN_ERR_UNSUPPORTED : WN_ERR_INVALID_ARGUMENT;
    }
    return view_media_status(wn_core_media_set_muted(view->core, index, muted),
                             "wn_node_media_set_muted");
}

wn_status wn_view_media_set_loop(wn_view *view, wn_node_id node, int loop)
{
    int index;
    wn_clear_error();
    index = view_media_index(view, node, "wn_node_media_set_loop");
    if (index < 0) {
        return index == -2 ? WN_ERR_UNSUPPORTED : WN_ERR_INVALID_ARGUMENT;
    }
    return view_media_status(wn_core_media_set_loop(view->core, index, loop),
                             "wn_node_media_set_loop");
}

wn_status wn_view_media_set_source(wn_view *view, wn_node_id node,
                                   const char *path_utf8)
{
    int index;
    int result;
    wn_clear_error();
    index = view_media_index(view, node, "wn_node_media_set_source");
    if (index < 0) {
        return index == -2 ? WN_ERR_UNSUPPORTED : WN_ERR_INVALID_ARGUMENT;
    }
    result = wn_core_media_set_source(view->core, index, path_utf8);
    if (result == -2) {
        return WN_ERR_OUT_OF_MEMORY;
    }
    return view_media_status(result, "wn_node_media_set_source");
}

wn_status wn_view_media_get_state(wn_view *view, wn_node_id node,
                                  wn_media_state_info *out)
{
    int index;
    wn_clear_error();
    if (out == NULL) {
        wn_set_error("wn_node_media_get_state: out is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    index = view_media_index(view, node, "wn_node_media_get_state");
    if (index < 0) {
        return index == -2 ? WN_ERR_UNSUPPORTED : WN_ERR_INVALID_ARGUMENT;
    }
    return wn_core_media_get_state(view->core, index, out) == 0
               ? WN_OK
               : WN_ERR_INVALID_ARGUMENT;
}

wn_status wn_view_tick(wn_view *view)
{
    wn_clear_error();
    if (view == NULL || view->core == NULL) {
        wn_set_error("wn_view_tick: view is NULL");
        return WN_ERR_INVALID_ARGUMENT;
    }
    wn_core_mark_dirty(view->core);
    view->geometry_dirty = 1;
    view_flush_media_events(view);
    return WN_OK;
}

void wn_view_set_ime(wn_view *view, const wn_ime_ops *ops, void *context)
{
    if (view == NULL) {
        return;
    }
    /* Persist first: a later document replacement must install the same backend
     * on the new core (wn_view_replace_document). */
    view->ime_ops = ops;
    view->ime_context = context;
    if (view->core != NULL) {
        wn_core_set_ime(view->core, ops, context);
    }
}
