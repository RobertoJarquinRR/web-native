/*
 * kind_ops_test.c - the per-kind capability table is complete and consistent.
 *
 * Locks the table layout dispatches through:
 *   1. every R2_NODE_* has an entry with a non-null, unique name;
 *   2. the table order is the NodeKind enum order (the entry at index k is the
 *      one wn_kind_ops(k) returns, and its name is the expected kind);
 *   3. is_leaf is true exactly for TEXT/IMAGE/INPUT and layout_children is null
 *      exactly for leaves;
 *   4. a leaf has an auto_height hook and a container does not;
 *   5. wn_kind_ops() is closed: -1 and R2_NODE_INPUT + 1 return NULL.
 *
 * A missing entry (zero-filled slot) fails (1); flipping a leaf flag or moving
 * a children algorithm to a leaf fails (3); swapping two entries fails (2).
 */
#include <stdio.h>
#include <string.h>

#include "core/kind_ops.h"

static int checks = 0;
static int failures = 0;

static void check(const char *label, int ok)
{
    checks += 1;
    if (!ok) {
        failures += 1;
    }
    printf("  %s %s\n", ok ? "[ok]" : "[FAIL]", label);
}

int main(void)
{
    static const char *const expected_names[] = {
        "block", "flex", "text", "grid", "image", "input"
    };
    static const int expected_leaf[] = { 0, 0, 1, 0, 1, 1 };
    int kind;
    int other;

    checks = 0;
    failures = 0;

    check("R2_NODE_INPUT is the last kind in the table",
          R2_NODE_INPUT + 1 == (int)(sizeof(expected_names) / sizeof(expected_names[0])));

    for (kind = R2_NODE_BLOCK; kind <= R2_NODE_INPUT; ++kind) {
        const WnKindOps *ops = wn_kind_ops(kind);
        char label[128];

        snprintf(label, sizeof(label), "kind %d has a table entry", kind);
        check(label, ops != NULL);
        if (ops == NULL) {
            continue;
        }

        snprintf(label, sizeof(label), "kind %d has a non-null name", kind);
        check(label, ops->name != NULL && ops->name[0] != '\0');

        snprintf(label, sizeof(label), "kind %d entry is at its enum position (%s)",
                 kind, expected_names[kind]);
        check(label, ops->name != NULL && strcmp(ops->name, expected_names[kind]) == 0);

        snprintf(label, sizeof(label), "kind %d name is unique", kind);
        {
            int unique = 1;
            for (other = R2_NODE_BLOCK; other <= R2_NODE_INPUT; ++other) {
                const WnKindOps *peer = wn_kind_ops(other);
                if (other != kind && peer != NULL && peer->name != NULL && ops->name != NULL &&
                    strcmp(peer->name, ops->name) == 0) {
                    unique = 0;
                }
            }
            check(label, unique);
        }

        snprintf(label, sizeof(label), "kind %d (%s) is_leaf=%d", kind, expected_names[kind],
                 expected_leaf[kind]);
        check(label, ops->is_leaf == expected_leaf[kind]);

        snprintf(label, sizeof(label), "kind %d layout_children is null exactly for leaves", kind);
        check(label, (ops->layout_children == NULL) == (ops->is_leaf != 0));

        snprintf(label, sizeof(label), "kind %d auto_width is the TEXT/IMAGE special case", kind);
        check(label, (ops->auto_width != NULL) == (kind == R2_NODE_TEXT || kind == R2_NODE_IMAGE));

        snprintf(label, sizeof(label), "kind %d has an intrinsic_width hook", kind);
        check(label, ops->intrinsic_width != NULL);

        snprintf(label, sizeof(label), "kind %d auto_height only on leaves", kind);
        check(label, (ops->auto_height != NULL) == (ops->is_leaf != 0));
    }

    check("wn_kind_ops(-1) is NULL", wn_kind_ops(-1) == NULL);
    check("wn_kind_ops(R2_NODE_INPUT + 1) is NULL", wn_kind_ops(R2_NODE_INPUT + 1) == NULL);
    check("wn_kind_ops(R2_NODE_BLOCK) is the table head",
          wn_kind_ops(R2_NODE_BLOCK) == wn_kind_ops(0));
    check("wn_kind_ops(R2_NODE_INPUT) is the table tail",
          wn_kind_ops(R2_NODE_INPUT) != NULL &&
          wn_kind_ops(R2_NODE_INPUT) != wn_kind_ops(0));

    if (failures != 0) {
        printf("kind_ops_test: FAIL (%d checks, %d failed)\n", checks, failures);
        return 1;
    }
    printf("kind_ops_test: PASS (%d checks)\n", checks);
    return 0;
}
