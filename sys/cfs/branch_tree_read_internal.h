/* SPDX-License-Identifier: ISC
 * branch_tree_read_internal.h -- shared authenticated path admission and loads.
 */
#ifndef CFS_BRANCH_TREE_READ_INTERNAL_H
#define CFS_BRANCH_TREE_READ_INTERNAL_H

#include "branch_tree_read.h"

typedef struct cfs_bt_node_load_work {
    uint32_t nodes_loaded;
    uint32_t edges_checked;
} cfs_bt_node_load_work_t;

/* Shared copied-node resolution and contextual validation. Diagnostic work
 * reports completed checks on failure; other outputs are private scratch. */
int cfs_bt_read_load_frame(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    cfs_bs_child_t ref, cfs_bt_read_frame_t *frame,
    const cfs_bt_read_frame_t *parent, uint16_t slot,
    cfs_bs_ref_t *physical_children, cfs_bs_workspace_t *store_workspace,
    cfs_bt_node_load_work_t *work);

int cfs_bt_read_prepare(cfs_bs_store_t *store, cfs_bs_ref_t root,
    cfs_bs_workspace_t *store_workspace, cfs_bt_read_workspace_t *workspace);
int cfs_bt_read_load_node(cfs_bs_store_t *store, cfs_bs_ref_t ref, uint32_t depth,
    cfs_bs_workspace_t *store_workspace, cfs_bt_read_workspace_t *workspace);

#endif /* CFS_BRANCH_TREE_READ_INTERNAL_H */
