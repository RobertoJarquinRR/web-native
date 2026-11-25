/*
 * renderer_wgpu_stub.c - link-time stand-in for a build without wgpu-native.
 *
 * renderer_wgpu.c is only compiled when CMake is configured with WN_WGPU=ON.
 * wn_view always resolves wn_renderer_wgpu(), so this stub keeps the symbol
 * available and reports why the backend cannot start instead of failing the
 * link or silently dropping the wgpu request.
 */
#include "wn_renderer.h"

static int stub_init(void **context, const wn_surface_desc *surface, int width,
                     int height)
{
    (void)surface;
    (void)width;
    (void)height;
    if (context != NULL) {
        *context = NULL;
    }
    return 1;
}

static const char *stub_last_error(void *context)
{
    (void)context;
    return "wgpu backend not built (configure with -DWN_WGPU=ON)";
}

static const wn_renderer_ops g_wgpu_stub_ops = {
    .name = "wgpu-native",
    .init = stub_init,
    .last_error = stub_last_error
};

const wn_renderer_ops *wn_renderer_wgpu(void)
{
    return &g_wgpu_stub_ops;
}
