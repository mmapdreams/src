/* SPDX-License-Identifier: ISC
 * branch_assemble_internal.h -- assembly lifecycle and content step contracts.
 */
#ifndef CFS_BRANCH_ASSEMBLE_INTERNAL_H
#define CFS_BRANCH_ASSEMBLE_INTERNAL_H

#include "branch_assemble.h"

int cfs_bf_assemble_prepare(cfs_bs_store_t *store, cfs_bf_assemble_t *assembly,
    cfs_bs_workspace_t *storage, cfs_bf_assemble_workspace_t *workspace);
int cfs_bf_assemble_fail(cfs_bf_assemble_t *assembly, int status);
int cfs_bf_assemble_save(cfs_bs_store_t *store, cfs_bf_assemble_t *assembly,
    cfs_bs_child_t inodes, cfs_bs_child_t orphans, cfs_bs_child_t content,
    cfs_bs_workspace_t *storage, cfs_bf_assemble_workspace_t *workspace);
int cfs_bf_assemble_flush(cfs_bs_store_t *store, cfs_bf_assemble_t *assembly,
    cfs_bs_workspace_t *storage, cfs_bf_assemble_workspace_t *workspace);
int cfs_bf_assemble_content(cfs_bs_store_t *store, cfs_bf_assemble_t *assembly,
    const cfs_bf_change_t *change, cfs_bs_workspace_t *storage,
    cfs_bf_assemble_workspace_t *workspace);

#endif /* CFS_BRANCH_ASSEMBLE_INTERNAL_H */
