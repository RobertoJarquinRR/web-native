/*
 * abi_smoke.c - links against the ABI objects and exercises the error surface
 * without creating a window or a GPU device.
 */
#include "webnative.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *version;
    uint32_t abi;
    wn_view *view;
    wn_status status;
    int failures = 0;

    version = wn_version();
    abi = wn_abi_version();
    printf("abi_smoke: version=%s abi=%u\n", version != NULL ? version : "(null)", abi);
    if (version == NULL || abi != WN_ABI_VERSION) {
        ++failures;
    }

    view = wn_view_create(NULL, 100, 100, 0);
    if (view != NULL) {
        ++failures;
        wn_view_destroy(view);
    }
    if (wn_last_error() == NULL) {
        printf("abi_smoke: expected wn_last_error after invalid view create\n");
        ++failures;
    } else {
        printf("abi_smoke: invalid view create error='%s'\n", wn_last_error());
    }

    status = wn_view_render(NULL);
    if (status != WN_ERR_INVALID_ARGUMENT) {
        ++failures;
    }
    status = wn_view_event(NULL, NULL);
    if (status != WN_ERR_INVALID_ARGUMENT) {
        ++failures;
    }
    status = wn_app_set_text(NULL, "x", "y");
    if (status != WN_ERR_INVALID_ARGUMENT) {
        ++failures;
    }
    {
        wn_config config;
        wn_app *app;
        memset(&config, 0, sizeof(config));
        config.flags = 1;
        app = wn_app_create(&config);
        if (app != NULL) {
            ++failures;
            wn_app_destroy(app);
        }
        if (wn_last_error() == NULL) {
            printf("abi_smoke: expected wn_last_error after reserved flags\n");
            ++failures;
        }
    }

    printf("abi_smoke: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
