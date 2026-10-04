/* SPDX-License-Identifier: ISC
 * branch_tree_diff.c -- paired forward traversal with shared-subtree skipping.
 */
#include "branch_tree_diff.h"

#include <string.h>

#include "branch_store_internal.h"
#include "branch_tree_read_internal.h"

enum {
    CURSOR_DONE = 0,
    CURSOR_NODE = 1,
    CURSOR_RECORD = 2
};

static cfs_bt_read_frame_t *frame(cfs_bt_diff_cursor_t *cursor) {
    return &cursor->read.path[cursor->read.depth];
}

static int load(cfs_bs_store_t *store, cfs_bs_ref_t ref, uint32_t depth,
    cfs_bs_workspace_t *store_workspace, cfs_bt_diff_cursor_t *cursor) {
    int rc = cfs_bt_read_load_node(store, ref, depth, store_workspace,
        &cursor->read);
    if (rc == CFS_BS_OK) {
        cursor->read.depth = depth;
        cursor->reference[depth] = ref;
        cursor->state = CURSOR_NODE;
    }

    return rc;
}

static int expand(cfs_bs_store_t *store, cfs_bs_workspace_t *store_workspace,
    cfs_bt_diff_cursor_t *cursor) {
    cfs_bt_read_frame_t *current = frame(cursor);
    if (current->node.level == 0u) {
        cursor->state = CURSOR_RECORD;
        return CFS_BS_OK;
    }

    return load(store, current->children[0].value.retained, cursor->read.depth + 1u,
        store_workspace, cursor);
}

/* A node frontier consumes its whole subtree; a record consumes one slot. */
static int advance(cfs_bs_store_t *store, cfs_bs_workspace_t *store_workspace,
    cfs_bt_diff_cursor_t *cursor) {
    cfs_bt_read_frame_t *current = frame(cursor);
    if (cursor->state == CURSOR_RECORD &&
        ++current->slot < current->node.count) {
        return CFS_BS_OK;
    }

    while (cursor->read.depth != 0u) {
        cursor->read.depth--;
        current = frame(cursor);
        if (++current->slot < current->node.count) {
            return load(store, current->children[current->slot].value.retained,
                cursor->read.depth + 1u, store_workspace, cursor);
        }
    }

    cursor->state = CURSOR_DONE;
    return CFS_BS_OK;
}

static const cfs_bs_ref_t *record(cfs_bt_diff_cursor_t *cursor,
    cfs_bt_record_t *out) {
    cfs_bt_read_frame_t *current = frame(cursor);
    (void)cfs_bt_node_record(&current->node, current->slot, out);
    return out->child_index == CFS_BT_NO_CHILD ? NULL :
        &current->children[out->child_index].value.retained;
}

static int values_equal(const cfs_bt_record_t *before,
    const cfs_bs_ref_t *before_child, const cfs_bt_record_t *after,
    const cfs_bs_ref_t *after_child) {
    if (before->value_length != after->value_length ||
        memcmp(before->value, after->value, before->value_length) != 0) {
        return 0;
    }

    if (before_child == NULL || after_child == NULL) {
        return before_child == after_child;
    }

    return cfs_bs_ref_equal(*before_child, *after_child);
}

static int walk(cfs_bs_store_t *store, cfs_bt_diff_visit_fn visit, void *context,
    cfs_bt_diff_yield_fn yield,
    cfs_bt_scan_result_t *result, cfs_bs_workspace_t *store_workspace,
    cfs_bt_diff_workspace_t *workspace) {
    cfs_bt_diff_cursor_t *left = &workspace->cursor[0];
    cfs_bt_diff_cursor_t *right = &workspace->cursor[1];
    while (left->state != CURSOR_DONE || right->state != CURSOR_DONE) {
        if (yield) {
            int answer = yield(context);
            if (store->poisoned) {
                return CFS_BS_EPOISON;
            }

            if (answer == CFS_BT_SCAN_STOP) {
                result->complete = 0;
                return CFS_BS_OK;
            }

            if (answer != CFS_BT_SCAN_CONTINUE) {
                return CFS_BS_EINVAL;
            }
        }

        int rc;
        if (left->state == CURSOR_NODE && right->state == CURSOR_NODE &&
            cfs_bs_ref_equal(left->reference[left->read.depth],
                right->reference[right->read.depth])) {
            workspace->work.subtrees_skipped++;
            rc = advance(store, store_workspace, left);
            if (rc == CFS_BS_OK) {
                rc = advance(store, store_workspace, right);
            }

            if (rc != CFS_BS_OK) {
                return rc;
            }

            continue;
        }

        cfs_bt_record_t before = {0};
        cfs_bt_record_t after = {0};
        const cfs_bs_ref_t *before_child = NULL;
        const cfs_bs_ref_t *after_child = NULL;
        int order = left->state == CURSOR_DONE ? 1 : -1;
        if (left->state != CURSOR_DONE) {
            before_child = record(left, &before);
        }

        if (right->state != CURSOR_DONE) {
            after_child = record(right, &after);
        }

        if (left->state != CURSOR_DONE && right->state != CURSOR_DONE) {
            workspace->work.key_comparisons++;
            order = cfs_bt_key_compare(before.key, before.key_length,
                after.key, after.key_length);
        }

        if (left->state == CURSOR_NODE && (right->state == CURSOR_DONE ||
                order < 0 || (order == 0 && (right->state != CURSOR_NODE ||
                    frame(left)->node.level >= frame(right)->node.level)))) {
            rc = expand(store, store_workspace, left);
        } else if (right->state == CURSOR_NODE &&
            (left->state == CURSOR_DONE || order >= 0)) {
            rc = expand(store, store_workspace, right);
        } else {
            if (order == 0) {
                workspace->work.records_compared++;
            }

            if (order != 0 || !values_equal(&before, before_child,
                    &after, after_child)) {
                workspace->work.changes_delivered++;
                int answer = visit(context, order <= 0 ? &before : NULL,
                    order <= 0 ? before_child : NULL,
                    order >= 0 ? &after : NULL,
                    order >= 0 ? after_child : NULL);
                if (store->poisoned) {
                    return CFS_BS_EPOISON;
                }

                if (answer == CFS_BT_SCAN_STOP) {
                    result->complete = 0;
                    return CFS_BS_OK;
                }

                if (answer != CFS_BT_SCAN_CONTINUE) {
                    return CFS_BS_EINVAL;
                }
            }

            rc = order <= 0 ? advance(store, store_workspace, left) : CFS_BS_OK;
            if (rc == CFS_BS_OK && order >= 0) {
                rc = advance(store, store_workspace, right);
            }
        }

        if (rc != CFS_BS_OK) {
            return rc;
        }
    }

    return CFS_BS_OK;
}

int cfs_bt_diff(cfs_bs_store_t *store, cfs_bs_ref_t before, cfs_bs_ref_t after,
    cfs_bt_diff_visit_fn visit, void *context, cfs_bt_diff_yield_fn yield,
    cfs_bt_scan_result_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bt_diff_workspace_t *workspace) {
    if (store == NULL || visit == NULL || out == NULL ||
        store_workspace == NULL || workspace == NULL) {
        return CFS_BS_EINVAL;
    }

    cfs_bs_ref_t roots[2] = {before, after};
    cfs_bt_scan_result_t result = {.complete = 1};
    int rc = CFS_BS_OK;
    memset(&workspace->work, 0, sizeof(workspace->work));
    for (uint32_t i = 0; i < 2u; i++) {
        memset(&workspace->cursor[i].read.work, 0,
            sizeof(workspace->cursor[i].read.work));
        workspace->cursor[i].state = CURSOR_DONE;
    }

    for (uint32_t i = 0; i < 2u && rc == CFS_BS_OK; i++) {
        rc = cfs_bt_read_prepare(store, roots[i], store_workspace,
            &workspace->cursor[i].read);
    }

    for (uint32_t i = 0; i < 2u && rc == CFS_BS_OK; i++) {
        if (roots[i].block != CFS_BS_BLOCK_NONE) {
            rc = load(store, roots[i], 0u, store_workspace,
                &workspace->cursor[i]);
        }
    }

    if (rc == CFS_BS_OK) {
        rc = walk(store, visit, context, yield, &result, store_workspace, workspace);
    }

    for (uint32_t i = 0; i < 2u; i++) {
        workspace->work.nodes_loaded += workspace->cursor[i].read.work.nodes_loaded;
        workspace->work.edges_checked += workspace->cursor[i].read.work.edges_checked;
    }

    if (rc == CFS_BS_OK) {
        result.count = workspace->work.changes_delivered;
        *out = result;
    }

    return rc;
}
