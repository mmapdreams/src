/* SPDX-License-Identifier: ISC
 * branch_tree_edit.c -- ordered leaf changes and copy-on-write path balancing.
 */
#include "branch_tree_edit_internal.h"

#include <string.h>

static int operand_valid(const cfs_bs_store_t *store,
    const cfs_bt_stage_t *stage, cfs_bs_child_t operand, int allow_empty) {
    if (operand.kind == CFS_BS_CHILD_PREPARED) {
        return operand.value.prepared_index < stage->count;
    }

    if (operand.kind != CFS_BS_CHILD_RETAINED) {
        return 0;
    }

    cfs_bs_ref_t ref = operand.value.retained;
    if (ref.block == CFS_BS_BLOCK_NONE) {
        return allow_empty && ref.incarnation == 0u && ref.hash == 0u;
    }

    return ref.block >= store->semantic_start && ref.block < store->total_blocks &&
        ref.incarnation != 0u && (ref.incarnation >> CFS_BS_INCARNATION_INDEX_BITS) <= store->generation;
}

static int prepare(cfs_bs_store_t *store, cfs_bt_stage_t *stage,
    cfs_bs_child_t root, cfs_bt_change_t change, cfs_bs_child_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bt_edit_workspace_t *workspace) {
    if (store == NULL || stage == NULL || out == NULL ||
        store_workspace == NULL || workspace == NULL ||
        stage->state != CFS_BT_STAGE_EDITABLE ||
        change.kind < CFS_BT_INSERT || change.kind > CFS_BT_DELETE ||
        change.key == NULL || change.key_length == 0u ||
        change.key_length > CFS_BT_MAX_KEY ||
        change.value_length > CFS_BT_MAX_VALUE ||
        (change.value_length != 0u && change.value == NULL) ||
        (change.kind == CFS_BT_DELETE && (change.value != NULL ||
            change.value_length != 0u || change.child != NULL))) {
        return CFS_BS_EINVAL;
    }

    memset(&workspace->work, 0, sizeof(workspace->work));
    if (store->poisoned != 0u) {
        return CFS_BS_EPOISON;
    }

    if (store->generation == 0u) {
        return CFS_BS_ECORRUPT;
    }

    if (!operand_valid(store, stage, root, 1) ||
        (change.child != NULL && !operand_valid(store, stage, *change.child, 0))) {
        return CFS_BS_EINVAL;
    }

    return CFS_BS_OK;
}

static uint16_t lower_bound(const cfs_bt_node_view_t *node,
    const void *key, uint16_t length, int *equal) {
    uint16_t first = 0u;
    uint16_t end = node->count;
    *equal = 0;
    while (first < end) {
        uint16_t middle = first + (end - first) / 2u;
        cfs_bt_record_t record;
        (void)cfs_bt_node_record(node, middle, &record);
        int comparison = cfs_bt_key_compare(record.key, record.key_length,
            key, length);
        if (comparison < 0) {
            first = middle + 1u;
        } else {
            end = middle;
            if (comparison == 0) {
                *equal = 1;
            }
        }
    }

    return first;
}

static int descend(cfs_bs_store_t *store, cfs_bt_stage_t *stage,
    cfs_bs_child_t root, cfs_bt_change_t change, uint16_t *depth,
    cfs_bs_workspace_t *store_workspace, cfs_bt_edit_workspace_t *workspace) {
    *depth = 0u;
    const cfs_bt_edit_frame_t *parent = NULL;
    uint16_t slot = 0u;
    for (;;) {
        cfs_bt_edit_frame_t *frame = &workspace->path[*depth];
        int rc = cfs_bt_edit_load(store, stage, root, frame, parent, slot,
            store_workspace, workspace);
        if (rc != CFS_BS_OK || frame->node.level == 0u) {
            return rc;
        }

        int equal;
        slot = lower_bound(&frame->node, change.key, change.key_length, &equal);
        if (slot != 0u && !equal) {
            slot--;
        }

        frame->slot = slot;
        root = frame->children[slot];
        parent = frame;
        (*depth)++;
    }
}

static int change_leaf(cfs_bt_change_t change, const cfs_bt_edit_frame_t *leaf,
    uint16_t *count, cfs_bt_edit_workspace_t *workspace) {
    int equal = 0;
    uint16_t slot = 0u;
    *count = 0u;
    if (leaf) {
        slot = lower_bound(&leaf->node, change.key, change.key_length, &equal);
        *count = leaf->node.count;
        cfs_bt_edit_unpack(workspace, leaf, 0u);
    }

    if (equal && change.kind == CFS_BT_INSERT) {
        return CFS_BS_EEXIST;
    }

    if (!equal && (change.kind == CFS_BT_REPLACE || change.kind == CFS_BT_DELETE)) {
        return CFS_BS_ENOENT;
    }

    cfs_bt_edit_entry_t *entries = workspace->entries;
    if (change.kind == CFS_BT_DELETE) {
        memmove(&entries[slot], &entries[slot + 1u],
            (*count - slot - 1u) * sizeof(*entries));
        (*count)--;
    } else {
        if (!equal) {
            memmove(&entries[slot + 1u], &entries[slot],
                (*count - slot) * sizeof(*entries));
            (*count)++;
        }

        entries[slot].record = (cfs_bt_record_t){
            .key = change.key, .key_length = change.key_length,
            .value = change.value, .value_length = change.value_length,
            .child_index = change.child == NULL ? CFS_BT_NO_CHILD : 0u
        };
        if (change.child) {
            entries[slot].child = *change.child;
        }
    }

    return CFS_BS_OK;
}

static int balance(cfs_bs_store_t *store, cfs_bt_stage_t *stage,
    uint16_t depth, uint16_t count, uint16_t *first, uint16_t *removed,
    uint16_t *emitted, cfs_bt_edit_result_t *result,
    cfs_bs_workspace_t *store_workspace, cfs_bt_edit_workspace_t *workspace) {
    cfs_bt_edit_frame_t *parent = &workspace->path[depth - 1u];
    uint16_t level = workspace->path[depth].node.level;
    uint32_t weight = cfs_bt_edit_weight(workspace->entries, count);
    *first = parent->slot;
    *removed = 1u;
    if (!cfs_bt_edit_filled(count, weight)) {
        uint16_t sibling_slot = parent->slot == 0u ? 1u : parent->slot - 1u;
        cfs_bt_edit_frame_t *sibling = &workspace->sibling;
        int rc = cfs_bt_edit_load(store, stage, parent->children[sibling_slot],
            sibling, parent, sibling_slot, store_workspace, workspace);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        uint16_t sibling_count = sibling->node.count;
        if (sibling_slot < parent->slot) {
            memmove(&workspace->entries[sibling_count], workspace->entries,
                count * sizeof(*workspace->entries));
            cfs_bt_edit_unpack(workspace, sibling, 0u);
            *first = sibling_slot;
        } else {
            cfs_bt_edit_unpack(workspace, sibling, count);
        }

        count += sibling_count;
        weight = cfs_bt_edit_weight(workspace->entries, count);
        *removed = 2u;
    }

    if (cfs_bt_edit_fits(count, weight)) {
        *emitted = 1u;
        if (*removed == 2u) {
            workspace->work.merges++;
        }

        return cfs_bt_edit_emit(stage, level, 0u, count, 0, result, workspace);
    }

    *emitted = 2u;
    if (*removed == 2u) {
        workspace->work.redistributions++;
    } else {
        workspace->work.splits++;
    }

    return cfs_bt_edit_split(stage, level, count, result, workspace);
}

static int finish_root(cfs_bt_stage_t *stage, uint16_t level, uint16_t count,
    cfs_bt_edit_result_t *result, cfs_bs_child_t *out,
    cfs_bt_edit_workspace_t *workspace) {
    if (count == 0u) {
        *out = (cfs_bs_child_t){.kind = CFS_BS_CHILD_RETAINED,
            .value.retained = {CFS_BS_BLOCK_NONE, 0u, 0u}};
        return CFS_BS_OK;
    }

    if (level != 0u && count == 1u) {
        *out = workspace->entries[0].child;
        workspace->work.root_collapses++;
        return CFS_BS_OK;
    }

    uint32_t weight = cfs_bt_edit_weight(workspace->entries, count);
    int rc;
    if (cfs_bt_edit_fits(count, weight)) {
        rc = cfs_bt_edit_emit(stage, level, 0u, count, 1, result, workspace);
    } else {
        if (level == CFS_BT_MAX_LEVEL) {
            return CFS_BS_EOVERFLOW;
        }

        rc = cfs_bt_edit_split(stage, level, count, result, workspace);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        workspace->work.splits++;
        workspace->entries[0] = cfs_bt_edit_parent_entry(&result[0]);
        workspace->entries[1] = cfs_bt_edit_parent_entry(&result[1]);
        result = workspace->results[2];
        rc = cfs_bt_edit_emit(stage, level + 1u, 0u, 2u, 1, result, workspace);
    }

    if (rc == CFS_BS_OK) {
        *out = result->child;
    }

    return rc;
}

int cfs_bt_edit(cfs_bs_store_t *store, cfs_bt_stage_t *stage,
    cfs_bs_child_t root, cfs_bt_change_t change, cfs_bs_child_t *out_root,
    cfs_bs_workspace_t *store_workspace, cfs_bt_edit_workspace_t *workspace) {
    int rc = prepare(store, stage, root, change, out_root, store_workspace,
        workspace);
    uint16_t depth = 0u;
    int empty = root.kind == CFS_BS_CHILD_RETAINED &&
        root.value.retained.block == CFS_BS_BLOCK_NONE;
    if (rc == CFS_BS_OK && !empty) {
        rc = descend(store, stage, root, change, &depth, store_workspace, workspace);
    }

    uint16_t count = 0u;
    if (rc == CFS_BS_OK) {
        rc = change_leaf(change, empty ? NULL : &workspace->path[depth],
            &count, workspace);
    }

    uint16_t result_slot = 0u;
    while (rc == CFS_BS_OK && depth != 0u) {
        uint16_t first;
        uint16_t removed;
        uint16_t emitted;
        cfs_bt_edit_result_t *result = workspace->results[result_slot];
        rc = balance(store, stage, depth, count, &first, &removed, &emitted,
            result, store_workspace, workspace);
        if (rc != CFS_BS_OK) {
            break;
        }

        cfs_bt_edit_frame_t *parent = &workspace->path[--depth];
        cfs_bt_edit_unpack(workspace, parent, 0u);
        cfs_bt_edit_entry_t *entries = workspace->entries;
        memmove(&entries[first + emitted], &entries[first + removed],
            (parent->node.count - first - removed) * sizeof(*entries));
        for (uint16_t i = 0u; i < emitted; i++) {
            entries[first + i] = cfs_bt_edit_parent_entry(&result[i]);
        }

        count = parent->node.count - removed + emitted;
        result_slot ^= 1u;
    }

    if (rc == CFS_BS_OK) {
        uint16_t level = empty ? 0u : workspace->path[0].node.level;
        rc = finish_root(stage, level, count, workspace->results[result_slot],
            out_root, workspace);
    }

    if (rc != CFS_BS_OK) {
        cfs_bt_stage_discard(stage);
    }

    return rc;
}
