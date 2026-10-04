/* SPDX-License-Identifier: ISC
 * branch_path.h -- bounded path traversal within one retained namespace view.
 */
#ifndef CFS_BRANCH_PATH_H
#define CFS_BRANCH_PATH_H

#include "branch_symlink.h"

#define CFS_BF_PATH_MAX 4096u
#define CFS_BF_FOLLOW_MAX 40u

typedef struct cfs_bf_path_workspace {
    cfs_bf_entry_t entry;
    cfs_bf_symlink_workspace_t target;
    uint8_t pending[CFS_BF_PATH_MAX];
} cfs_bf_path_workspace_t;

/* Serialize the image and retain view throughout. Buffers are disjoint;
 * refusal preserves out. Traversal contract: txt/branch_fs.md. */
int cfs_bf_resolve(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t start, const void *path, uint32_t length, int follow_final,
    cfs_bf_entry_t *out, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *namespace_workspace, cfs_bf_path_workspace_t *workspace);

#endif /* CFS_BRANCH_PATH_H */
