/* SPDX-License-Identifier: ISC
 * branch_tree_diff.h -- bounded same-image ordered-record comparison.
 */
#ifndef CFS_BRANCH_TREE_DIFF_H
#define CFS_BRANCH_TREE_DIFF_H

#include "branch_tree_read.h"

typedef struct cfs_bt_diff_work {
    uint64_t nodes_loaded;
    uint64_t edges_checked;
    uint64_t key_comparisons;
    uint64_t records_compared;
    uint64_t changes_delivered;
    uint64_t subtrees_skipped;
} cfs_bt_diff_work_t;

typedef struct cfs_bt_diff_cursor {
    cfs_bt_read_workspace_t read;
    cfs_bs_ref_t reference[CFS_BT_MAX_HEIGHT];
    uint32_t state;
} cfs_bt_diff_cursor_t;

/* Only work is diagnostic. The two paths borrow their own ancestor frames;
 * do not copy this workspace or reenter it while a call is active. */
typedef struct cfs_bt_diff_workspace {
    cfs_bt_diff_cursor_t cursor[2];
    cfs_bt_diff_work_t work;
} cfs_bt_diff_workspace_t;

/* A missing before/after record means addition/removal. Optional child
 * references are values, not traversed payloads. All pointers are borrowed. */
typedef int (*cfs_bt_diff_visit_fn)(void *context,
    const cfs_bt_record_t *before, const cfs_bs_ref_t *before_child,
    const cfs_bt_record_t *after, const cfs_bs_ref_t *after_child);

/* Optional checkpoint before each paired-frontier step, even without changes.
 * CONTINUE/STOP/invalid rules match visit; checkpoints are not records. */
typedef int (*cfs_bt_diff_yield_fn)(void *context);

/* Roots and store stay retained across callbacks; the store is never recovered
 * or replaced. A callback may drop serialization while scratch is its own. */
int cfs_bt_diff(cfs_bs_store_t *store, cfs_bs_ref_t before, cfs_bs_ref_t after,
    cfs_bt_diff_visit_fn visit, void *context, cfs_bt_diff_yield_fn yield,
    cfs_bt_scan_result_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bt_diff_workspace_t *workspace);

#endif /* CFS_BRANCH_TREE_DIFF_H */
