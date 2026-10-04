/* SPDX-License-Identifier: ISC
 * branch_symlink_internal.h -- authenticated symbolic target loading.
 */
#ifndef CFS_BRANCH_SYMLINK_INTERNAL_H
#define CFS_BRANCH_SYMLINK_INTERNAL_H

#include "branch_symlink.h"

/* Validate and stage one complete target, without namespace edits or I/O.
 * Inputs and output are disjoint; failure invalidates the staging context. */
int cfs_bf_symlink_stage(cfs_bt_stage_t *stage, const void *target,
    uint32_t length, cfs_bs_child_t *out);

/* Entry is an authenticated symbolic inode; output borrows workspace bytes. */
int cfs_bf_symlink_load(cfs_bs_store_t *store, const cfs_bf_entry_t *entry,
    cfs_bs_workspace_t *storage, cfs_bf_symlink_workspace_t *workspace);

#endif /* CFS_BRANCH_SYMLINK_INTERNAL_H */
