/* SPDX-License-Identifier: ISC
 * branch_orphan.h -- bounded discovery and release of unlinked inodes.
 */
#ifndef CFS_BRANCH_ORPHAN_H
#define CFS_BRANCH_ORPHAN_H

#include "branch_fs.h"

#define CFS_BF_ORPHAN_BATCH_MAX 8u

/* Retain view and exclusive image ownership. Zero starts an exclusive cursor.
 * Validate visited records; ENOENT is exhaustion. See txt/branch_orphan.md. */
int cfs_bf_orphan_next(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t after, cfs_bf_id_t *out, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *workspace);

/* Exclude id's consumers; remove its zero-link inode and orphan atomically.
 * ENOENT means not an orphan. Failure preserves out; uncertain I/O poisons. */
int cfs_bf_orphan_reap(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t id, uint64_t *out_revision,
    cfs_bs_workspace_t *storage, cfs_bf_workspace_t *workspace);

/* Exclude all selected consumers. Delete 1..MAX strictly increasing IDs
 * atomically; fixed workspace admission may refuse even if singles fit. */
int cfs_bf_orphan_reap_batch(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, const cfs_bf_id_t *ids, uint32_t count,
    uint64_t *out_revision, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *workspace);

#endif /* CFS_BRANCH_ORPHAN_H */
