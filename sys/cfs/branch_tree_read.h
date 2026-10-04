/* SPDX-License-Identifier: ISC
 * branch_tree_read.h -- retained and staged point reads and synchronous scans.
 */
#ifndef CFS_BRANCH_TREE_READ_H
#define CFS_BRANCH_TREE_READ_H

#include "branch_tree_stage.h"

#define CFS_BT_MAX_HEIGHT (CFS_BT_MAX_LEVEL + 1u)

typedef struct cfs_bt_value {
    cfs_bs_ref_t child;
    uint32_t length;
    uint8_t bytes[CFS_BT_MAX_VALUE];
} cfs_bt_value_t;

/* Tagged values create no pin and last only through their stage lifetime.
 * Reads preserve the stage and its counters; refusals preserve output. */
typedef struct cfs_bt_staged_value {
    cfs_bs_child_t child;
    uint32_t length;
    uint8_t bytes[CFS_BT_MAX_VALUE];
} cfs_bt_staged_value_t;

typedef struct cfs_bt_read_work {
    uint64_t nodes_loaded;
    uint64_t edges_checked;
    uint64_t search_comparisons;
    uint64_t records_delivered;
} cfs_bt_read_work_t;

typedef struct cfs_bt_read_frame {
    cfs_bs_child_t children[CFS_BT_MAX_RECORDS];
    uint8_t payload[CFS_BT_PAYLOAD_MAX];
    cfs_bt_node_view_t node;
    const uint8_t *upper;
    uint16_t upper_length;
    uint16_t slot;
} cfs_bt_read_frame_t;

/* Caller-owned path storage is separate from the branch-store workspace.
 * Its internal views and bounds borrow earlier frames; do not copy it. */
typedef struct cfs_bt_read_workspace {
    cfs_bt_read_frame_t path[CFS_BT_MAX_HEIGHT];
    cfs_bs_ref_t physical_children[CFS_BT_MAX_RECORDS];
    cfs_bt_read_work_t work;
    uint32_t depth;
} cfs_bt_read_workspace_t;

enum {
    CFS_BT_SCAN_CONTINUE = 0,
    CFS_BT_SCAN_STOP = 1
};

/* Yield only with roots retained and scratch exclusive; reacquire on return.
 * Never recover/replace the owner or reenter an active traversal path. */
typedef int (*cfs_bt_visit_fn)(void *context, const cfs_bt_record_t *record,
    const cfs_bs_ref_t *child);

typedef int (*cfs_bt_stage_visit_fn)(void *context,
    const cfs_bt_record_t *record, const cfs_bs_child_t *child);

typedef struct cfs_bt_scan_result {
    uint64_t count;
    int complete;
} cfs_bt_scan_result_t;

/* The caller retains root and serializes the image through return.
 * All input/output/workspace storage is disjoint; failures preserve out. */
int cfs_bt_lookup(cfs_bs_store_t *store, cfs_bs_ref_t root,
    const void *key, uint16_t key_length, cfs_bt_value_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bt_read_workspace_t *workspace);

/* Empty bounds are unbounded; equal bounds are empty. Callbacks return 0/1.
 * Results stay provisional; callback poison returns EPOISON even after STOP. */
int cfs_bt_scan(cfs_bs_store_t *store, cfs_bs_ref_t root,
    const void *lower, uint16_t lower_length,
    const void *upper, uint16_t upper_length, cfs_bt_visit_fn visit,
    void *context, cfs_bt_scan_result_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bt_read_workspace_t *workspace);

/* Retained-read rules apply. Keep the editable stage immutable through return,
 * including callbacks, and retain every physical dependency. */
int cfs_bt_stage_lookup(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    cfs_bs_child_t root, const void *key, uint16_t key_length,
    cfs_bt_staged_value_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bt_read_workspace_t *workspace);

/* Same stage lifetime as lookup. Pure prepared paths perform no I/O. */
int cfs_bt_stage_scan(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    cfs_bs_child_t root, const void *lower, uint16_t lower_length,
    const void *upper, uint16_t upper_length, cfs_bt_stage_visit_fn visit,
    void *context, cfs_bt_scan_result_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bt_read_workspace_t *workspace);

#endif /* CFS_BRANCH_TREE_READ_H */
