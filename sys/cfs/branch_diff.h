/* SPDX-License-Identifier: ISC
 * branch_diff.h -- retained same-image logical filesystem change events.
 */
#ifndef CFS_BRANCH_DIFF_H
#define CFS_BRANCH_DIFF_H

#include "branch_file.h"
#include "branch_symlink.h"
#include "branch_tree_diff.h"

enum {
    CFS_BF_CHANGE_INODE = 1,
    CFS_BF_CHANGE_ENTRY = 2,
    CFS_BF_CHANGE_DATA = 3,
    CFS_BF_CHANGE_SYMLINK = 4
};

/* INODE borrows nullable metadata; ENTRY borrows a name and nullable ids.
 * DATA/SYMLINK borrow result bytes. All pointers expire at callback return. */
typedef struct cfs_bf_change {
    uint32_t kind;
    cfs_bf_id_t id;
    const cfs_bf_inode_t *before;
    const cfs_bf_inode_t *after;
    cfs_bf_id_t before_id;
    cfs_bf_id_t after_id;
    const void *bytes;
    uint32_t length;
    uint64_t offset;
} cfs_bf_change_t;

typedef struct cfs_bf_diff_work {
    uint64_t inode_candidates;
    uint64_t tree_nodes_loaded;
    uint64_t radix_nodes_loaded;
    uint64_t data_loaded;
    uint64_t slots_examined;
    uint64_t subtrees_skipped;
    uint64_t events_delivered;
} cfs_bf_diff_work_t;

typedef struct cfs_bf_diff_file_cursor {
    cfs_bf_file_workspace_t read;
    uint64_t size;
    uint32_t depth;
    uint32_t state;
} cfs_bf_diff_file_cursor_t;

/* Caller-owned private scratch, disjoint from the namespace/store workspaces.
 * Paths borrow their own frames; do not copy or reenter an active workspace. */
typedef struct cfs_bf_diff_workspace {
    cfs_bt_diff_workspace_t inodes;
    cfs_bt_diff_workspace_t directories;
    cfs_bf_diff_file_cursor_t file[2];
    cfs_bf_symlink_workspace_t symlink[2];
    cfs_bf_diff_work_t work;
} cfs_bf_diff_workspace_t;

typedef int (*cfs_bf_diff_visit_fn)(void *context,
    const cfs_bf_change_t *change);

/* Views come from cfs_bf_read and stay retained through return. Rules match
 * cfs_bt_diff; yield also fires on equal bytes and shares visit's context. */
int cfs_bf_diff(cfs_bs_store_t *store, const cfs_bf_view_t *before,
    const cfs_bf_view_t *after, cfs_bf_diff_visit_fn visit, void *context,
    cfs_bt_diff_yield_fn yield,
    cfs_bt_scan_result_t *out, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *namespace_workspace, cfs_bf_diff_workspace_t *workspace);

#endif /* CFS_BRANCH_DIFF_H */
