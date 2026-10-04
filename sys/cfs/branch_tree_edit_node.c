/* SPDX-License-Identifier: ISC
 * branch_tree_edit_node.c -- authenticated edit frames and bounded node packing.
 */
#include "branch_tree_edit_internal.h"

#include <string.h>

#include "branch_store_internal.h"
#include "branch_tree_read_internal.h"

void cfs_bt_edit_require(int condition) {
    if (!condition) {
        /* A validated edit must produce a representable node partition. */
        __builtin_trap();
    }
}

int cfs_bt_edit_load(cfs_bs_store_t *store, cfs_bt_stage_t *stage,
    cfs_bs_child_t ref, cfs_bt_edit_frame_t *frame,
    const cfs_bt_edit_frame_t *parent, uint16_t slot,
    cfs_bs_workspace_t *store_workspace, cfs_bt_edit_workspace_t *workspace) {
    cfs_bt_node_load_work_t work;
    int rc = cfs_bt_read_load_frame(store, stage, ref, frame, parent, slot,
        workspace->physical_children, store_workspace, &work);
    workspace->work.nodes_loaded += work.nodes_loaded;
    workspace->work.edges_checked += work.edges_checked;
    return rc;
}

void cfs_bt_edit_unpack(cfs_bt_edit_workspace_t *workspace,
    const cfs_bt_edit_frame_t *frame, uint16_t offset) {
    for (uint16_t i = 0u; i < frame->node.count; i++) {
        cfs_bt_edit_entry_t *entry = &workspace->entries[offset + i];
        (void)cfs_bt_node_record(&frame->node, i, &entry->record);
        if (entry->record.child_index != CFS_BT_NO_CHILD) {
            entry->child = frame->children[entry->record.child_index];
        }
    }

    workspace->work.entries_copied += frame->node.count;
}

uint32_t cfs_bt_edit_weight(const cfs_bt_edit_entry_t *entries, uint16_t count) {
    uint32_t weight = 0u;
    for (uint16_t i = 0u; i < count; i++) {
        const cfs_bt_record_t *record = &entries[i].record;
        weight += CFS_BT_SLOT_SIZE + record->key_length + record->value_length;
        if (record->child_index != CFS_BT_NO_CHILD) {
            weight += CFS_BS_REF_SIZE;
        }
    }

    return weight;
}

int cfs_bt_edit_fits(uint16_t count, uint32_t weight) {
    return count <= CFS_BT_MAX_RECORDS &&
        weight <= CFS_BT_PAYLOAD_MAX - CFS_BT_HEADER_SIZE;
}

int cfs_bt_edit_filled(uint16_t count, uint32_t weight) {
    return count >= CFS_BT_MIN_RECORDS || weight >= CFS_BT_MIN_ENTRY_BYTES;
}

static uint64_t load_count(const uint8_t *bytes) {
    uint64_t count = 0u;
    for (uint32_t i = 0u; i < 8u; i++) {
        count |= (uint64_t)bytes[i] << (8u * i);
    }

    return count;
}

int cfs_bt_edit_emit(cfs_bt_stage_t *stage, uint16_t level,
    uint16_t first, uint16_t count, int root, cfs_bt_edit_result_t *out,
    cfs_bt_edit_workspace_t *workspace) {
    uint32_t children = 0u;
    uint64_t items = level == 0u ? count : 0u;
    for (uint16_t i = 0u; i < count; i++) {
        const cfs_bt_edit_entry_t *entry = &workspace->entries[first + i];
        workspace->records[i] = entry->record;
        if (entry->record.child_index != CFS_BT_NO_CHILD) {
            workspace->records[i].child_index = (uint16_t)children;
            workspace->children[children++] = entry->child;
        }

        if (level != 0u) {
            uint64_t child_items = load_count(entry->record.value);
            if (UINT64_MAX - items < child_items) {
                return CFS_BS_EOVERFLOW;
            }

            items += child_items;
        }
    }

    uint32_t length;
    uint32_t framed_children;
    int rc = cfs_bt_node_frame(level, workspace->records, count, root,
        workspace->payload, sizeof(workspace->payload), &length,
        &framed_children);
    cfs_bt_edit_require(rc == CFS_BS_OK && children == framed_children);
    cfs_bs_object_plan_t plan = {
        .children = workspace->children,
        .child_count = children,
        .payload = workspace->payload,
        .payload_length = length
    };
    uint32_t index;
    rc = cfs_bt_stage_add(stage, &plan, &index);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    const cfs_bt_record_t *minimum = &workspace->entries[first].record;
    out->key_length = minimum->key_length;
    memcpy(out->key, minimum->key, minimum->key_length);
    for (uint32_t i = 0u; i < 8u; i++) {
        out->count[i] = (uint8_t)(items >> (8u * i));
    }

    out->child.kind = CFS_BS_CHILD_PREPARED;
    out->child.value.prepared_index = index;
    workspace->work.nodes_emitted++;
    return CFS_BS_OK;
}

int cfs_bt_edit_split(cfs_bt_stage_t *stage, uint16_t level,
    uint16_t count, cfs_bt_edit_result_t *out,
    cfs_bt_edit_workspace_t *workspace) {
    uint32_t total = cfs_bt_edit_weight(workspace->entries, count);
    uint32_t left = 0u;
    uint32_t best_difference = UINT32_MAX;
    uint16_t split = 0u;
    for (uint16_t i = 1u; i < count; i++) {
        left += cfs_bt_edit_weight(&workspace->entries[i - 1u], 1u);
        uint32_t right = total - left;
        uint32_t difference = left > right ? left - right : right - left;
        if (cfs_bt_edit_fits(i, left) &&
            cfs_bt_edit_fits(count - i, right) &&
            cfs_bt_edit_filled(i, left) &&
            cfs_bt_edit_filled(count - i, right) &&
            difference < best_difference) {
            split = i;
            best_difference = difference;
        }
    }

    cfs_bt_edit_require(split != 0u);
    int rc = cfs_bt_edit_emit(stage, level, 0u, split, 0, &out[0], workspace);
    if (rc == CFS_BS_OK) {
        rc = cfs_bt_edit_emit(stage, level, split, count - split, 0,
            &out[1], workspace);
    }

    return rc;
}

cfs_bt_edit_entry_t cfs_bt_edit_parent_entry(const cfs_bt_edit_result_t *result) {
    cfs_bt_edit_entry_t entry = {
        .record = {
            .key = result->key,
            .key_length = result->key_length,
            .value = result->count,
            .value_length = 8u,
            .child_index = 0u
        },
        .child = result->child
    };
    return entry;
}
