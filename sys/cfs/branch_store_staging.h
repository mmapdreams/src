/* SPDX-License-Identifier: ISC
 * branch_store_staging.h -- hidden apply ownership and checked publication.
 */
#ifndef CFS_BRANCH_STORE_STAGING_H
#define CFS_BRANCH_STORE_STAGING_H

#include "branch_store.h"

typedef struct cfs_bs_staging {
    uint64_t id;
    uint64_t revision;
    uint64_t destination;
    uint64_t expected_destination;
    cfs_bs_ref_t root;
} cfs_bs_staging_t;

/* Exclusive image ownership through return, all buffers disjoint. Manages graph
 * ownership only; provider failure poisons the store and out is untouched. */
int cfs_bs_staging_begin(cfs_bs_store_t *store, uint64_t destination,
    uint64_t expected_destination, cfs_bs_staging_t *out,
    cfs_bs_workspace_t *workspace);
int cfs_bs_staging_get(cfs_bs_store_t *store, uint64_t id,
    cfs_bs_staging_t *out, cfs_bs_workspace_t *workspace);
int cfs_bs_staging_construct(cfs_bs_store_t *store, uint64_t id,
    uint64_t expected_revision, const cfs_bs_object_plan_t *plans,
    uint32_t plan_count, cfs_bs_ref_t *out_root, uint64_t *out_revision,
    cfs_bs_workspace_t *workspace);

/* Caller has validated this revision's complete root and holds destination
 * quiescence. Success gives a fresh revision; refusal leaves staging live. */
int cfs_bs_staging_publish(cfs_bs_store_t *store, uint64_t id,
    uint64_t expected_revision, cfs_bs_branch_t *out,
    cfs_bs_workspace_t *workspace);

/* Cancel excludes an active apply. An abandoned stage is cancelable after
 * recovery, never resumable; space recovery still needs a bounded reap. */
int cfs_bs_staging_cancel(cfs_bs_store_t *store, uint64_t id,
    uint64_t expected_revision, cfs_bs_workspace_t *workspace);

/* Cancel head's donor and every donor whose head is gone or moved on. */
int cfs_bs_branch_unshare_cancel(cfs_bs_store_t *store, uint64_t head,
    cfs_bs_workspace_t *workspace);

/* Hold the plans' retained edges in a donor for head's next construction
 * (r0125 7.3). EWORKSPACE: nothing retained, nothing written. */
int cfs_bs_branch_unshare(cfs_bs_store_t *store, uint64_t head,
    uint64_t expected_revision, const cfs_bs_object_plan_t *plans,
    uint32_t plan_count, cfs_bs_workspace_t *workspace);
#endif /* CFS_BRANCH_STORE_STAGING_H */
