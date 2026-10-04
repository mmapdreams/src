/* SPDX-License-Identifier: ISC
 * branch_fs_internal.h -- shared namespace validation declarations.
 */
#ifndef CFS_BRANCH_FS_INTERNAL_H
#define CFS_BRANCH_FS_INTERNAL_H

#include "branch_fs.h"
#include "branch_store_staging.h"

int cfs_bf_prepare(cfs_bs_store_t *store, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace);
int cfs_bf_directory(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t id, cfs_bf_entry_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace);
int cfs_bf_edit_inode(cfs_bs_store_t *store, cfs_bs_child_t root,
    cfs_bf_id_t id, cfs_bf_inode_t inode, cfs_bs_child_t tree, uint32_t kind,
    cfs_bs_child_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace);
int cfs_bf_stage_root(cfs_bs_store_t *store, cfs_bf_view_t view,
    cfs_bs_child_t inodes, cfs_bs_child_t orphans, uint32_t *out_index,
    cfs_bf_workspace_t *workspace);
int cfs_bf_publish(cfs_bs_store_t *store, cfs_bs_branch_t branch,
    cfs_bf_view_t view, cfs_bs_child_t inodes, uint64_t *out_revision,
    cfs_bs_workspace_t *store_workspace, cfs_bf_workspace_t *workspace);

/* A refused second attempt cancels the donor the first attempt made. */
int cfs_bf_retry_done(cfs_bs_store_t *store, uint64_t branch_id, int status,
    cfs_bs_workspace_t *store_workspace);

/* The ordinary publisher retains view.orphans. Lifecycle mutations supply
 * both staged index roots explicitly instead. */
int cfs_bf_publish_roots(cfs_bs_store_t *store, cfs_bs_branch_t branch,
    cfs_bf_view_t view, cfs_bs_child_t inodes, cfs_bs_child_t orphans,
    uint64_t *out_revision, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *workspace);

typedef struct cfs_bf_roots {
    cfs_bs_child_t inodes;
    cfs_bs_child_t orphans;
} cfs_bf_roots_t;

/* Drop one observed binding; the caller already checked linked target/type.
 * Decrement surviving nondirectory links, or retain/drop a final inode. */
int cfs_bf_drop_link(cfs_bs_store_t *store, cfs_bf_entry_t *entry,
    int keep_open, cfs_bf_time_t time, cfs_bf_roots_t roots,
    cfs_bf_roots_t *out, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *workspace);

int cfs_bf_create_records(cfs_bs_store_t *store, const void *name,
    uint16_t length, cfs_bs_child_t content, cfs_bf_created_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bf_workspace_t *workspace);

#endif /* CFS_BRANCH_FS_INTERNAL_H */
