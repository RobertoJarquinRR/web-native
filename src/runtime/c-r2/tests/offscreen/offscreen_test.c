/*
 * offscreen_test.c - vtable offscreen smoke test (WN_SURFACE_OFFSCREEN).
 *
 * Creates a window-less view, loads the compiled ui_data.h fixture, renders one
 * frame and writes it to a PNG through the backend readback. Run with
 * WN_RENDERER=d3d11 and WN_RENDERER=wgpu to compare both backends.
 */
#include "wn_view.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    wn_view *view;
    wn_status status;
    int width = 960;
    int height = 640;
    const char *path = "offscreen.png";

    if (argc >= 3) {
        width = atoi(argv[1]);
        height = atoi(argv[2]);
    }
    if (argc >= 4) {
        path = argv[3];
    }
    view = wn_view_create_offscreen(width, height);
    if (view == NULL) {
        fprintf(stderr, "offscreen: create failed: %s\n", wn_last_error());
        return 1;
    }
    printf("offscreen: backend=%s device=%s\n", wn_view_backend(view),
           wn_view_device(view) != NULL ? wn_view_device(view) : "(unknown)");
    status = wn_view_load(view, "", NULL, NULL);
    if (status != WN_OK) {
        fprintf(stderr, "offscreen: load failed: %s\n", wn_last_error());
        wn_view_destroy(view);
        return 1;
    }
    if (wn_view_render(view) != 0) {
        fprintf(stderr, "offscreen: render failed: %s\n", wn_last_error());
        wn_view_destroy(view);
        return 1;
    }
    status = wn_view_readback(view, path);
    if (status != WN_OK) {
        fprintf(stderr, "offscreen: readback failed: %s\n", wn_last_error());
        wn_view_destroy(view);
        return 1;
    }
    wn_view_destroy(view);
    printf("offscreen: ok %dx%d -> %s\n", width, height, path);
    return 0;
}
