/* SPDX-License-Identifier: ISC
 * branch_symlink.h -- atomic symbolic inode construction and target reads.
 */
#ifndef CFS_BRANCH_SYMLINK_H
#define CFS_BRANCH_SYMLINK_H

#include "branch_fs.h"

typedef struct cfs_bf_symlink_workspace {
    uint8_t bytes[CFS_BF_SYMLINK_MAX];
    uint8_t header[16];
} cfs_bf_symlink_workspace_t;

/* Exclusive image ownership and disjoint buffers, as for namespace creation.
 * Target is 1..4096 non-NUL bytes, never resolved during creation. */
int cfs_bf_symlink(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t parent, const void *name,
    uint16_t name_length, const void *target, uint32_t target_length,
    cfs_bf_attributes_t attributes, cfs_bf_created_t *out,
    cfs_bs_workspace_t *storage, cfs_bf_workspace_t *workspace);

/* Retain the exact view and serialize the image. Outputs change only after full
 * target validation; no terminator is appended. All buffers disjoint. */
int cfs_bf_readlink(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t id, void *bytes, uint32_t capacity, uint32_t *out_length,
    cfs_bs_workspace_t *storage, cfs_bf_workspace_t *namespace_workspace,
    cfs_bf_symlink_workspace_t *workspace);

#endif /* CFS_BRANCH_SYMLINK_H */
