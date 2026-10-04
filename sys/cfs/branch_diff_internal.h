/* SPDX-License-Identifier: ISC
 * branch_diff_internal.h -- synchronous semantic comparison context.
 */
#ifndef CFS_BRANCH_DIFF_INTERNAL_H
#define CFS_BRANCH_DIFF_INTERNAL_H

#include "branch_diff.h"

typedef struct cfs_bf_diff_context {
    cfs_bs_store_t *store;
    const cfs_bf_view_t *view[2];
    cfs_bs_workspace_t *storage;
    cfs_bf_workspace_t *namespace_workspace;
    cfs_bf_diff_workspace_t *workspace;
    cfs_bf_diff_visit_fn visit;
    cfs_bt_diff_yield_fn yield;
    void *context;
    cfs_bf_id_t id;
    int status;
    int stopped;
} cfs_bf_diff_context_t;

int cfs_bf_diff_deliver(cfs_bf_diff_context_t *context,
    const cfs_bf_change_t *change);
int cfs_bf_diff_checkpoint(void *context);
int cfs_bf_diff_file(cfs_bf_diff_context_t *context,
    const cfs_bf_entry_t *before, const cfs_bf_entry_t *after);

#endif /* CFS_BRANCH_DIFF_INTERNAL_H */
