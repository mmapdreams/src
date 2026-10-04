/* SPDX-License-Identifier: ISC
 *
 * branch_store_prepare.h -- caller-owned volatile construction admission.
 */
#ifndef CFS_BRANCH_STORE_PREPARE_H
#define CFS_BRANCH_STORE_PREPARE_H

#include "branch_store.h"

/* Fields are private. Use fresh inactive storage; never copy or relocate an
 * active token. Token, workspace, and store stay byte-stable until terminal. */
typedef struct cfs_bs_prepared_construct {
    cfs_bs_store_t *store;
    cfs_bs_workspace_t *workspace;
    uint64_t branch_id;
    uint64_t base_revision;
    uint64_t generation;
    cfs_bs_ref_t root;
    uint64_t revision;
    uint32_t page_count;
    uint32_t object_count;
    uint32_t state;
    uint32_t reserved;
} cfs_bs_prepared_construct_t;

/* Fully validate without writes; inputs are borrowed through return. Failure
 * preserves out. Success leases mutation and permits reads in other storage. */
int cfs_bs_branch_construct_prepare(cfs_bs_store_t *store,
    uint64_t branch_id, uint64_t expected_revision,
    const cfs_bs_object_plan_t *plans, uint32_t plan_count,
    cfs_bs_prepared_construct_t *out, cfs_bs_workspace_t *workspace);

/* Replace an active admission using wholly disjoint token/workspace pairs.
 * Refusal preserves current; success consumes it. No partial aliases. */
int cfs_bs_branch_construct_replace_prepare(cfs_bs_store_t *store,
    cfs_bs_prepared_construct_t *current,
    const cfs_bs_object_plan_t *plans, uint32_t plan_count,
    cfs_bs_prepared_construct_t *out,
    cfs_bs_workspace_t *alternate_workspace);

/* Publish performs no reads. An exact valid call consumes the lease on every
 * result, including poison/I/O failure; invalid args leave it live. */
int cfs_bs_branch_construct_publish(cfs_bs_store_t *store,
    cfs_bs_prepared_construct_t *prepared, cfs_bs_ref_t *out_root,
    uint64_t *out_revision);

/* Cancel an exact active token without provider I/O, including after poison. */
int cfs_bs_branch_construct_cancel(cfs_bs_store_t *store,
    cfs_bs_prepared_construct_t *prepared);

#endif /* CFS_BRANCH_STORE_PREPARE_H */
