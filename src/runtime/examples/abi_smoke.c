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
    if (wn_app_root(NULL) != WN_NODE_INVALID ||
        wn_app_find(NULL, "body.root") != WN_NODE_INVALID ||
        wn_app_node_count(NULL) != 0 ||
        wn_node_parent(NULL, 1) != WN_NODE_INVALID ||
        wn_node_first_child(NULL, 1) != WN_NODE_INVALID ||
        wn_node_next_sibling(NULL, 1) != WN_NODE_INVALID) {
        printf("abi_smoke: expected invalid handles from the dynamic API on NULL app\n");
        ++failures;
    }
    if (wn_node_set_visible(NULL, 1, 0) != WN_ERR_INVALID_ARGUMENT ||
        wn_node_set_image(NULL, 1, "x.png") != WN_ERR_INVALID_ARGUMENT ||
        wn_node_set_text(NULL, 1, "x") != WN_ERR_INVALID_ARGUMENT ||
        wn_node_clone(NULL, 1, 1) != WN_NODE_INVALID ||
        wn_node_remove(NULL, 1) != WN_ERR_INVALID_ARGUMENT ||
        wn_node_clear_children(NULL, 1) != WN_ERR_INVALID_ARGUMENT ||
        wn_node_append(NULL, 1, 2) != WN_ERR_INVALID_ARGUMENT) {
        printf("abi_smoke: expected WN_ERR_INVALID_ARGUMENT from dynamic API on NULL app\n");
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
