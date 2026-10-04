/* SPDX-License-Identifier: ISC
 * branch_apply.h -- caller-owned logical receive and publication coordinator.
 */
#ifndef CFS_BRANCH_APPLY_H
#define CFS_BRANCH_APPLY_H

#include "branch_assemble.h"
#include "branch_validate.h"

enum cfs_ba_cleanup_disposition {
    CFS_BA_CLEANUP_NONE,
    CFS_BA_CLEANUP_RELEASED,
    CFS_BA_CLEANUP_RETAINED,
    CFS_BA_CLEANUP_UNKNOWN
};

typedef struct cfs_ba_cleanup {
    enum cfs_ba_cleanup_disposition disposition;
    int status;
    uint32_t reaped;
} cfs_ba_cleanup_t;

/* Bound to one serialized image through publication/cleanup. Progress may
 * release exclusion; mutation observers may not. Context remains borrowed. */
typedef struct cfs_ba_apply {
    cfs_bs_store_t *store;
    uint64_t dest;
    uint64_t expected;
    uint64_t input_limit;
    cfs_bf_validate_yield_fn progress;
    void *context;
    cfs_bf_mutation_observer_t mutations;
    cfs_bs_workspace_t storage;
    cfs_bs_branch_t base;
    cfs_bf_view_t view;
    cfs_bf_change_decoder_t decoder;
    cfs_bf_assemble_t assembly;
    cfs_bs_staging_t stage;
    cfs_bf_change_receipt_t stream;
    cfs_bf_validated_t validated;
    union {
        cfs_bf_assemble_workspace_t assembly;
        cfs_bf_validate_workspace_t validation;
    } work;
    uint64_t input_bytes;
    cfs_bf_change_t pending;
    uint32_t since_cleanup;
    int status;
    int publication_attempted;
    int finished;
    int published;
    uint8_t data[CFS_BF_IO_MAX];
} cfs_ba_apply_t;

/* Begin admits the destination and decoder without staging. Caller holds image
 * exclusion; the copied observer brackets each later durable mutation. */
int cfs_ba_begin(cfs_ba_apply_t *apply, cfs_bs_store_t *store,
    uint64_t dest, uint64_t expected, uint64_t input_limit,
    cfs_bf_validate_yield_fn progress, void *context,
    cfs_bf_mutation_observer_t mutations);

/* Zero-I/O initializer for a disjoint admitted branch/view under uninterrupted
 * image exclusion; it preserves admission-before-allocation ordering. */
int cfs_ba_begin_admitted(cfs_ba_apply_t *apply, cfs_bs_store_t *store,
    const cfs_bs_branch_t *base, const cfs_bf_view_t *view, uint64_t expected,
    uint64_t input_limit, cfs_bf_validate_yield_fn progress, void *context,
    cfs_bf_mutation_observer_t mutations);

/* Feed enforces byte limit before parsing and never retries failed input.
 * Storage errors remain exact; interruption is EINVAL or EPOISON. */
int cfs_ba_feed(cfs_ba_apply_t *apply, const void *bytes, uint32_t count);

/* Only EOF authorizes the suffix, finish, and whole-result audit. Success is
 * a hidden candidate, not visible publication. */
int cfs_ba_finish(cfs_ba_apply_t *apply, cfs_bf_validate_inode_t *inodes,
    size_t capacity, uint64_t validation_budget);

/* Caller checks destination quiescence. Publication is marked after mutation
 * admission and before core I/O; output changes only on success. */
int cfs_ba_publish(cfs_ba_apply_t *apply, cfs_bs_branch_t *out);

/* After failure, refresh/cancel and try one reap; never retry poison. Cleanup
 * has a separate receipt and never replaces apply->status. */
void cfs_ba_cleanup(cfs_ba_apply_t *apply, cfs_ba_cleanup_t *out);

#endif /* CFS_BRANCH_APPLY_H */
