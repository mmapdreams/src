/* SPDX-License-Identifier: ISC
 * branch_tree_edit.h -- caller-owned copy-on-write ordered tree mutation.
 */
#ifndef CFS_BRANCH_TREE_EDIT_H
#define CFS_BRANCH_TREE_EDIT_H

#include "branch_tree_read.h"

enum {
    CFS_BT_INSERT = 1,
    CFS_BT_REPLACE = 2,
    CFS_BT_UPSERT = 3,
    CFS_BT_DELETE = 4
};

typedef struct cfs_bt_change {
    uint32_t kind;
    const void *key;
    const void *value;
    const cfs_bs_child_t *child;
    uint16_t key_length;
    uint16_t value_length;
} cfs_bt_change_t;

typedef cfs_bt_read_frame_t cfs_bt_edit_frame_t;

typedef struct cfs_bt_edit_entry {
    cfs_bt_record_t record;
    cfs_bs_child_t child;
} cfs_bt_edit_entry_t;

typedef struct cfs_bt_edit_result {
    cfs_bs_child_t child;
    uint8_t key[CFS_BT_MAX_KEY];
    uint8_t count[8];
    uint16_t key_length;
} cfs_bt_edit_result_t;

typedef struct cfs_bt_edit_work {
    uint64_t nodes_loaded;
    uint64_t edges_checked;
    uint64_t entries_copied;
    uint64_t nodes_emitted;
    uint64_t splits;
    uint64_t merges;
    uint64_t redistributions;
    uint64_t root_collapses;
} cfs_bt_edit_work_t;

/* Private scratch fields; only work is diagnostic. Do not copy during a call.
 * This workspace, the stage, store workspace, and inputs are disjoint. */
typedef struct cfs_bt_edit_workspace {
    cfs_bt_edit_frame_t path[CFS_BT_MAX_LEVEL + 1u];
    cfs_bt_edit_frame_t sibling;
    cfs_bs_ref_t physical_children[CFS_BT_MAX_RECORDS];
    cfs_bt_edit_entry_t entries[2u * CFS_BT_MAX_RECORDS + 1u];
    cfs_bt_record_t records[CFS_BT_MAX_RECORDS];
    cfs_bs_child_t children[CFS_BT_MAX_RECORDS];
    uint8_t payload[CFS_BT_PAYLOAD_MAX];
    cfs_bt_edit_result_t results[3][2];
    cfs_bt_edit_work_t work;
} cfs_bt_edit_workspace_t;

/* Retain inputs and serialize the image through construction; no writes here.
 * Earlier roots stay unchanged. Refusals invalidate stage and preserve out. */
int cfs_bt_edit(cfs_bs_store_t *store, cfs_bt_stage_t *stage,
    cfs_bs_child_t root, cfs_bt_change_t change, cfs_bs_child_t *out_root,
    cfs_bs_workspace_t *store_workspace, cfs_bt_edit_workspace_t *workspace);

#endif /* CFS_BRANCH_TREE_EDIT_H */
