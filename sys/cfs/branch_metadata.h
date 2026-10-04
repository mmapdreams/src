/* SPDX-License-Identifier: ISC
 * branch_metadata.h -- atomic selected inode attributes and optional resize.
 */
#ifndef CFS_BRANCH_METADATA_H
#define CFS_BRANCH_METADATA_H

#include "branch_file.h"

enum {
    CFS_BF_SET_MODE = 1u,
    CFS_BF_SET_UID = 2u,
    CFS_BF_SET_GID = 4u,
    CFS_BF_SET_ATIME = 8u,
    CFS_BF_SET_MTIME = 16u,
    CFS_BF_SET_SIZE = 32u,
    CFS_BF_SET_ALL = 63u
};

typedef struct cfs_bf_update {
    cfs_bf_time_t atime;
    cfs_bf_time_t mtime;
    uint64_t size;
    uint32_t fields;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
} cfs_bf_update_t;

/* One expected-head publication of the selected fields; file scratch only for
 * SIZE. Callers serialize; full contract: txt/branch_metadata.md. */
int cfs_bf_setattr(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t id, cfs_bf_update_t update,
    cfs_bf_time_t time, uint64_t *out_revision, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *workspace, cfs_bf_file_workspace_t *file_workspace);

#endif /* CFS_BRANCH_METADATA_H */
