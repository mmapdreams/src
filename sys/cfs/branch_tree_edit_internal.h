/* SPDX-License-Identifier: ISC
 * branch_tree_edit_internal.h -- declarations shared by tree edit phases.
 */
#ifndef CFS_BRANCH_TREE_EDIT_INTERNAL_H
#define CFS_BRANCH_TREE_EDIT_INTERNAL_H

#include "branch_tree_edit.h"

void cfs_bt_edit_require(int condition);
int cfs_bt_edit_load(cfs_bs_store_t *store, cfs_bt_stage_t *stage,
    cfs_bs_child_t ref, cfs_bt_edit_frame_t *frame,
    const cfs_bt_edit_frame_t *parent, uint16_t slot,
    cfs_bs_workspace_t *store_workspace, cfs_bt_edit_workspace_t *workspace);
void cfs_bt_edit_unpack(cfs_bt_edit_workspace_t *workspace,
    const cfs_bt_edit_frame_t *frame, uint16_t offset);
uint32_t cfs_bt_edit_weight(const cfs_bt_edit_entry_t *entries, uint16_t count);
int cfs_bt_edit_fits(uint16_t count, uint32_t weight);
int cfs_bt_edit_filled(uint16_t count, uint32_t weight);
int cfs_bt_edit_emit(cfs_bt_stage_t *stage, uint16_t level,
    uint16_t first, uint16_t count, int root, cfs_bt_edit_result_t *out,
    cfs_bt_edit_workspace_t *workspace);
int cfs_bt_edit_split(cfs_bt_stage_t *stage, uint16_t level,
    uint16_t count, cfs_bt_edit_result_t *out,
    cfs_bt_edit_workspace_t *workspace);
cfs_bt_edit_entry_t cfs_bt_edit_parent_entry(const cfs_bt_edit_result_t *result);

#endif /* CFS_BRANCH_TREE_EDIT_INTERNAL_H */
