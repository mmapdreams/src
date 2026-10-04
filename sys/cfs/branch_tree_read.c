/* SPDX-License-Identifier: ISC
 * branch_tree_read.c -- bounded authenticated tree paths and forward scans.
 */
#include "branch_tree_read_internal.h"

#include <string.h>

static int span_valid(const void *bytes, uint16_t length) {
    return length <= CFS_BT_MAX_KEY && (length == 0u || bytes != NULL);
}

static cfs_bs_child_t retained(cfs_bs_ref_t ref) {
    return (cfs_bs_child_t){.kind = CFS_BS_CHILD_RETAINED,
        .value.retained = ref};
}

static int empty(cfs_bs_child_t root) {
    return root.kind == CFS_BS_CHILD_RETAINED &&
        root.value.retained.block == CFS_BS_BLOCK_NONE;
}

static int prepare(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    cfs_bs_child_t root, cfs_bs_workspace_t *store_workspace,
    cfs_bt_read_workspace_t *workspace) {
    if (store == NULL || store_workspace == NULL || workspace == NULL ||
        (stage != NULL && stage->state != CFS_BT_STAGE_EDITABLE)) {
        return CFS_BS_EINVAL;
    }

    memset(&workspace->work, 0, sizeof(workspace->work));
    workspace->depth = 0u;
    if (store->poisoned != 0u) {
        return CFS_BS_EPOISON;
    }

    if (store->generation == 0u) {
        return CFS_BS_ECORRUPT;
    }

    if (root.kind == CFS_BS_CHILD_PREPARED) {
        return stage != NULL && root.value.prepared_index < stage->count ?
            CFS_BS_OK : CFS_BS_EINVAL;
    }

    if (root.kind != CFS_BS_CHILD_RETAINED) {
        return CFS_BS_EINVAL;
    }

    cfs_bs_ref_t ref = root.value.retained;
    if (ref.block == CFS_BS_BLOCK_NONE) {
        return ref.incarnation == 0u && ref.hash == 0u ?
            CFS_BS_OK : CFS_BS_EINVAL;
    }

    if (ref.block < store->semantic_start || ref.block >= store->total_blocks ||
        ref.incarnation == 0u || (ref.incarnation >> CFS_BS_INCARNATION_INDEX_BITS) > store->generation) {
        return CFS_BS_EINVAL;
    }

    return CFS_BS_OK;
}

int cfs_bt_read_prepare(cfs_bs_store_t *store, cfs_bs_ref_t root,
    cfs_bs_workspace_t *store_workspace, cfs_bt_read_workspace_t *workspace) {
    return prepare(store, NULL, retained(root), store_workspace, workspace);
}

int cfs_bt_read_load_frame(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    cfs_bs_child_t ref, cfs_bt_read_frame_t *frame,
    const cfs_bt_read_frame_t *parent, uint16_t slot,
    cfs_bs_ref_t *physical_children, cfs_bs_workspace_t *store_workspace,
    cfs_bt_node_load_work_t *work) {
    uint32_t children;
    uint32_t length;
    int rc;
    *work = (cfs_bt_node_load_work_t){0};
    if (ref.kind == CFS_BS_CHILD_PREPARED) {
        cfs_bs_object_plan_t plan;
        rc = cfs_bt_stage_view(stage, ref.value.prepared_index, &plan);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        if (plan.child_count > CFS_BT_MAX_RECORDS ||
            plan.payload_length > sizeof(frame->payload)) {
            return CFS_BS_ECORRUPT;
        }

        children = plan.child_count;
        length = plan.payload_length;
        memcpy(frame->payload, plan.payload, length);
        memcpy(frame->children, plan.children,
            children * sizeof(*frame->children));
    } else if (ref.kind == CFS_BS_CHILD_RETAINED) {
        rc = cfs_bs_object_read(store, ref.value.retained, physical_children,
            CFS_BT_MAX_RECORDS, &children, frame->payload,
            sizeof(frame->payload), &length, store_workspace);
        if (rc == CFS_BS_EWORKSPACE ||
            (parent != NULL && rc == CFS_BS_ENOENT)) {
            return CFS_BS_ECORRUPT;
        }

        if (rc != CFS_BS_OK) {
            return rc;
        }

        for (uint32_t i = 0u; i < children; i++) {
            frame->children[i] = retained(physical_children[i]);
        }
    } else {
        return CFS_BS_EINVAL;
    }

    rc = cfs_bt_node_decode(frame->payload, length, children, parent == NULL,
        &frame->node);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    work->nodes_loaded = 1u;
    frame->slot = 0u;
    frame->upper = NULL;
    frame->upper_length = 0u;
    if (parent) {
        rc = cfs_bt_node_check_child(&parent->node, slot, &frame->node);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        frame->upper = parent->upper;
        frame->upper_length = parent->upper_length;
        if (slot + 1u < parent->node.count) {
            cfs_bt_record_t next;
            (void)cfs_bt_node_record(&parent->node, slot + 1u, &next);
            frame->upper = next.key;
            frame->upper_length = next.key_length;
        }

        rc = cfs_bt_node_check_range(&frame->node, NULL, 0u,
            frame->upper, frame->upper_length);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        work->edges_checked = 1u;
    }

    return CFS_BS_OK;
}

static int load_node(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    cfs_bs_child_t ref, uint32_t depth, cfs_bs_workspace_t *store_workspace,
    cfs_bt_read_workspace_t *workspace) {
    const cfs_bt_read_frame_t *parent = depth == 0u ? NULL :
        &workspace->path[depth - 1u];
    cfs_bt_node_load_work_t work;
    int rc = cfs_bt_read_load_frame(store, stage, ref, &workspace->path[depth],
        parent, parent == NULL ? 0u : parent->slot,
        workspace->physical_children, store_workspace, &work);
    workspace->work.nodes_loaded += work.nodes_loaded;
    workspace->work.edges_checked += work.edges_checked;
    return rc;
}

int cfs_bt_read_load_node(cfs_bs_store_t *store, cfs_bs_ref_t ref, uint32_t depth,
    cfs_bs_workspace_t *store_workspace, cfs_bt_read_workspace_t *workspace) {
    return load_node(store, NULL, retained(ref), depth, store_workspace, workspace);
}

static int compare_record(cfs_bt_read_workspace_t *workspace,
    const cfs_bt_node_view_t *node, uint16_t index,
    const void *key, uint16_t key_length) {
    cfs_bt_record_t record;
    (void)cfs_bt_node_record(node, index, &record);
    workspace->work.search_comparisons++;
    return cfs_bt_key_compare(record.key, record.key_length, key, key_length);
}

static uint16_t lower_bound(cfs_bt_read_workspace_t *workspace,
    const cfs_bt_node_view_t *node, const void *key, uint16_t key_length) {
    uint16_t first = 0u;
    uint16_t end = node->count;
    while (first < end) {
        uint16_t middle = first + (end - first) / 2u;
        if (compare_record(workspace, node, middle, key, key_length) < 0) {
            first = middle + 1u;
        } else {
            end = middle;
        }
    }

    return first;
}

static int descend(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    const void *key, uint16_t key_length,
    cfs_bs_workspace_t *store_workspace, cfs_bt_read_workspace_t *workspace) {
    for (;;) {
        cfs_bt_read_frame_t *frame = &workspace->path[workspace->depth];
        uint16_t slot = key_length == 0u ? 0u :
            lower_bound(workspace, &frame->node, key, key_length);
        if (frame->node.level == 0u) {
            frame->slot = slot;
            return CFS_BS_OK;
        }

        if (slot != 0u && (slot == frame->node.count ||
                compare_record(workspace, &frame->node, slot, key,
                    key_length) != 0)) {
            slot--;
        }

        frame->slot = slot;

        /* A validated root is at most level 15; each checked edge lowers
         * the level by one, so this next frame is inside the fixed path. */
        uint32_t depth = workspace->depth + 1u;
        int rc = load_node(store, stage, frame->children[slot], depth,
            store_workspace, workspace);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        workspace->depth = depth;
    }
}

static int next_leaf(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    int *available,
    cfs_bs_workspace_t *store_workspace, cfs_bt_read_workspace_t *workspace) {
    *available = 0;
    while (workspace->depth != 0u) {
        uint32_t depth = workspace->depth;
        cfs_bt_read_frame_t *parent = &workspace->path[depth - 1u];
        parent->slot++;
        if (parent->slot < parent->node.count) {
            int rc = load_node(store, stage, parent->children[parent->slot],
                depth, store_workspace, workspace);
            if (rc != CFS_BS_OK) {
                return rc;
            }

            rc = descend(store, stage, NULL, 0u, store_workspace, workspace);
            if (rc == CFS_BS_OK) {
                *available = 1;
            }

            return rc;
        }

        workspace->depth--;
    }

    return CFS_BS_OK;
}

static int lookup(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    cfs_bs_child_t root, const void *key, uint16_t key_length,
    cfs_bt_staged_value_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bt_read_workspace_t *workspace) {
    cfs_bt_staged_value_t value;
    cfs_bt_record_t record;
    int rc;
    if (out == NULL || key_length == 0u || !span_valid(key, key_length)) {
        return CFS_BS_EINVAL;
    }

    rc = prepare(store, stage, root, store_workspace, workspace);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (empty(root)) {
        return CFS_BS_ENOENT;
    }

    rc = load_node(store, stage, root, 0u, store_workspace, workspace);
    if (rc == CFS_BS_OK) {
        rc = descend(store, stage, key, key_length, store_workspace, workspace);
    }

    if (rc != CFS_BS_OK) {
        return rc;
    }

    cfs_bt_read_frame_t *leaf = &workspace->path[workspace->depth];
    if (leaf->slot == leaf->node.count ||
        compare_record(workspace, &leaf->node, leaf->slot, key, key_length) != 0) {
        return CFS_BS_ENOENT;
    }

    (void)cfs_bt_node_record(&leaf->node, leaf->slot, &record);
    memset(&value, 0, sizeof(value));
    value.child = retained((cfs_bs_ref_t){CFS_BS_BLOCK_NONE, 0u, 0u});
    value.length = record.value_length;
    memcpy(value.bytes, record.value, record.value_length);
    if (record.child_index != CFS_BT_NO_CHILD) {
        value.child = leaf->children[record.child_index];
    }

    *out = value;
    workspace->work.records_delivered = 1u;
    return CFS_BS_OK;
}

static int deliver_scan(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    const void *upper, uint16_t upper_length, cfs_bt_stage_visit_fn visit,
    void *context, cfs_bt_scan_result_t *result,
    cfs_bs_workspace_t *store_workspace, cfs_bt_read_workspace_t *workspace) {
    for (;;) {
        cfs_bt_read_frame_t *leaf = &workspace->path[workspace->depth];
        while (leaf->slot < leaf->node.count) {
            cfs_bt_record_t record;
            const cfs_bs_child_t *child = NULL;
            if (upper_length != 0u && compare_record(workspace, &leaf->node,
                    leaf->slot, upper, upper_length) >= 0) {
                return CFS_BS_OK;
            }

            (void)cfs_bt_node_record(&leaf->node, leaf->slot, &record);
            if (record.child_index != CFS_BT_NO_CHILD) {
                child = &leaf->children[record.child_index];
            }

            workspace->work.records_delivered++;
            int answer = visit(context, &record, child);
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

            leaf->slot++;
        }

        int available;
        int rc = next_leaf(store, stage, &available, store_workspace, workspace);
        if (rc != CFS_BS_OK || !available) {
            return rc;
        }
    }
}

static int scan(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    cfs_bs_child_t root,
    const void *lower, uint16_t lower_length,
    const void *upper, uint16_t upper_length, cfs_bt_stage_visit_fn visit,
    void *context, cfs_bt_scan_result_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bt_read_workspace_t *workspace) {
    cfs_bt_scan_result_t result;
    int rc;
    if (visit == NULL || out == NULL || !span_valid(lower, lower_length) ||
        !span_valid(upper, upper_length)) {
        return CFS_BS_EINVAL;
    }

    int order = lower_length != 0u && upper_length != 0u ?
        cfs_bt_key_compare(lower, lower_length, upper, upper_length) : -1;
    if (order > 0) {
        return CFS_BS_EINVAL;
    }

    rc = prepare(store, stage, root, store_workspace, workspace);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    memset(&result, 0, sizeof(result));
    result.complete = 1;
    if (!empty(root) && order != 0) {
        rc = load_node(store, stage, root, 0u, store_workspace, workspace);
        if (rc == CFS_BS_OK) {
            rc = descend(store, stage, lower, lower_length,
                store_workspace, workspace);
        }

        if (rc == CFS_BS_OK) {
            rc = deliver_scan(store, stage, upper, upper_length, visit, context,
                &result, store_workspace, workspace);
        }
    }

    if (rc == CFS_BS_OK) {
        result.count = workspace->work.records_delivered;
        *out = result;
    }

    return rc;
}

int cfs_bt_stage_lookup(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    cfs_bs_child_t root, const void *key, uint16_t key_length,
    cfs_bt_staged_value_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bt_read_workspace_t *workspace) {
    if (!stage) {
        return CFS_BS_EINVAL;
    }

    return lookup(store, stage, root, key, key_length, out,
        store_workspace, workspace);
}

int cfs_bt_stage_scan(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    cfs_bs_child_t root, const void *lower, uint16_t lower_length,
    const void *upper, uint16_t upper_length, cfs_bt_stage_visit_fn visit,
    void *context, cfs_bt_scan_result_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bt_read_workspace_t *workspace) {
    if (!stage) {
        return CFS_BS_EINVAL;
    }

    return scan(store, stage, root, lower, lower_length, upper, upper_length,
        visit, context, out, store_workspace, workspace);
}

int cfs_bt_lookup(cfs_bs_store_t *store, cfs_bs_ref_t root,
    const void *key, uint16_t key_length, cfs_bt_value_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bt_read_workspace_t *workspace) {
    if (!out) {
        return CFS_BS_EINVAL;
    }

    cfs_bt_staged_value_t staged;
    int rc = lookup(store, NULL, retained(root), key, key_length, &staged,
        store_workspace, workspace);
    if (rc == CFS_BS_OK) {
        cfs_bt_value_t value;
        memset(&value, 0, sizeof(value));
        value.child = staged.child.value.retained;
        value.length = staged.length;
        memcpy(value.bytes, staged.bytes, staged.length);
        *out = value;
    }

    return rc;
}

typedef struct retained_visit {
    cfs_bt_visit_fn visit;
    void *context;
} retained_visit_t;

static int visit_retained(void *context, const cfs_bt_record_t *record,
    const cfs_bs_child_t *child) {
    retained_visit_t *visitor = context;
    return visitor->visit(visitor->context, record,
        child == NULL ? NULL : &child->value.retained);
}

int cfs_bt_scan(cfs_bs_store_t *store, cfs_bs_ref_t root,
    const void *lower, uint16_t lower_length,
    const void *upper, uint16_t upper_length, cfs_bt_visit_fn visit,
    void *context, cfs_bt_scan_result_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bt_read_workspace_t *workspace) {
    if (!visit) {
        return CFS_BS_EINVAL;
    }

    retained_visit_t visitor = {.visit = visit, .context = context};
    return scan(store, NULL, retained(root), lower, lower_length, upper,
        upper_length, visit_retained, &visitor, out, store_workspace, workspace);
}
