/* SPDX-License-Identifier: ISC
 * branch_validate_internal.h -- private semantic census and traversal seams.
 */
#ifndef CFS_BRANCH_VALIDATE_INTERNAL_H
#define CFS_BRANCH_VALIDATE_INTERNAL_H

#include "branch_validate.h"

int cfs_bf_validate_checkpoint(cfs_bf_validate_workspace_t *workspace);
int cfs_bf_validate_step(cfs_bf_validate_workspace_t *workspace);
size_t cfs_bf_validate_find(cfs_bf_validate_workspace_t *workspace,
    cfs_bf_id_t id);
int cfs_bf_validate_scan(cfs_bs_store_t *store, cfs_bs_ref_t root,
    cfs_bt_visit_fn visit, uint64_t *count, cfs_bs_workspace_t *storage,
    cfs_bf_validate_workspace_t *workspace);
int cfs_bf_validate_content(cfs_bs_store_t *store,
    cfs_bs_workspace_t *storage, cfs_bf_validate_workspace_t *workspace);
int cfs_bf_validate_graph(cfs_bf_validate_workspace_t *workspace);

#endif /* CFS_BRANCH_VALIDATE_INTERNAL_H */
