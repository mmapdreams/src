/* SPDX-License-Identifier: ISC
 * branch_validate.h -- complete retained-filesystem semantic validation.
 */
#ifndef CFS_BRANCH_VALIDATE_H
#define CFS_BRANCH_VALIDATE_H

#include "branch_file.h"
#include "branch_symlink.h"

/* Caller-owned scratch records, not an on-disk format or stable result. */
typedef struct cfs_bf_validate_inode {
    cfs_bf_id_t id;
    cfs_bf_id_t parent;
    cfs_bs_ref_t content;
    uint64_t size;
    uint64_t links;
    uint64_t incoming;
    uint64_t subdirectories;
    size_t parent_index;
    uint16_t type;
    uint8_t orphan;
    uint8_t color;
} cfs_bf_validate_inode_t;

typedef struct cfs_bf_validate_counts {
    uint64_t steps;
    uint64_t inodes;
    uint64_t orphans;
    uint64_t directories;
    uint64_t bindings;
    uint64_t tree_nodes;
    uint64_t file_nodes;
    uint64_t data_chunks;
    uint64_t symbolic_bytes;
    uint64_t identity_comparisons;
    uint64_t parent_steps;
} cfs_bf_validate_counts_t;

typedef struct cfs_bf_validated {
    uint8_t uuid[CFS_BS_UUID_SIZE];
    cfs_bs_ref_t root;
    uint64_t revision;
    cfs_bf_id_t root_id;
    cfs_bf_validate_counts_t counts;
} cfs_bf_validated_t;

typedef int (*cfs_bf_validate_yield_fn)(void *context);

typedef struct cfs_bf_validate_workspace {
    cfs_bf_workspace_t namespace;
    cfs_bf_file_workspace_t file;
    cfs_bf_symlink_workspace_t symlink;
    cfs_bf_view_t view;
    cfs_bf_validate_inode_t *inodes;
    size_t capacity;
    size_t count;
    size_t current;
    size_t root_index;
    uint64_t budget;
    cfs_bf_validate_counts_t counts;
    cfs_bs_store_t *store;
    cfs_bf_validate_yield_fn yield;
    void *context;
    int failure;
} cfs_bf_validate_workspace_t;

/* Inputs, scratch, and out must be disjoint; out changes only on full success.
 * Exhaustion is EWORKSPACE, poison EPOISON; a nonzero yield aborts EINVAL. */
int cfs_bf_validate(cfs_bs_store_t *store, cfs_bs_ref_t root,
    uint64_t revision, cfs_bf_validate_inode_t *inodes, size_t capacity,
    uint64_t budget, cfs_bf_validate_yield_fn yield, void *context,
    cfs_bf_validated_t *out, cfs_bs_workspace_t *storage,
    cfs_bf_validate_workspace_t *workspace);

#endif /* CFS_BRANCH_VALIDATE_H */
