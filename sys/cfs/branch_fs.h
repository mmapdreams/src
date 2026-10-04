/* SPDX-License-Identifier: ISC
 * branch_fs.h -- retained namespace reads and atomic namespace mutations.
 */
#ifndef CFS_BRANCH_FS_H
#define CFS_BRANCH_FS_H

#include "branch_fs_codec.h"
#include "branch_tree_read.h"
#include "branch_tree_edit.h"

typedef struct cfs_bf_view {
    cfs_bf_id_t root_id;
    cfs_bs_ref_t inodes;
    cfs_bs_ref_t orphans;
    uint64_t revision;
} cfs_bf_view_t;

typedef struct cfs_bf_entry {
    cfs_bf_id_t id;
    cfs_bf_inode_t inode;
    cfs_bs_ref_t tree;
} cfs_bf_entry_t;

typedef struct cfs_bf_attributes {
    cfs_bf_time_t time;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
} cfs_bf_attributes_t;

typedef struct cfs_bf_created {
    cfs_bf_id_t id;
    uint64_t revision;
} cfs_bf_created_t;

typedef struct cfs_bf_mutation {
    cfs_bs_branch_t branch;
    cfs_bf_view_t view;
    cfs_bf_entry_t parent;
    cfs_bf_entry_t target;
    cfs_bf_inode_t inode;
} cfs_bf_mutation_t;

typedef struct cfs_bf_rename_state {
    cfs_bf_entry_t parent;
    cfs_bf_entry_t replaced;
    cfs_bf_entry_t ancestor;
    int same_parent;
    int replaces;
} cfs_bf_rename_state_t;

/* Caller scratch is separate from the store workspace; fields are private. */
typedef struct cfs_bf_workspace {
    cfs_bf_mutation_t mutation;
    cfs_bf_rename_state_t rename;
    cfs_bt_stage_t stage;
    cfs_bt_edit_workspace_t edit;
    cfs_bt_read_workspace_t read;
    cfs_bt_value_t value;
    uint8_t root_bytes[CFS_BF_ROOT_BYTES];
    uint8_t inode_bytes[CFS_BF_INODE_BYTES];
    uint8_t key[CFS_BF_ID_BYTES];
    uint8_t dirent[CFS_BF_ID_BYTES];
} cfs_bf_workspace_t;

typedef int (*cfs_bf_visit_fn)(void *context, const void *name,
    uint16_t length, cfs_bf_id_t id);

/* Semantic reserve for namespace publication, zero for unsupported geometry.
 * Pass to cfs_bs_format; covers create/link/removal staging, not rename. */
uint32_t cfs_bf_reserve_blocks(uint64_t image_blocks);

/* Retain root at exactly revision and serialize the image throughout use.
 * Views are produced here, not fabricated. Reads never publish or recover. */
int cfs_bf_read(cfs_bs_store_t *store, cfs_bs_ref_t root, uint64_t revision,
    cfs_bf_view_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace);
int cfs_bf_get(cfs_bs_store_t *store, const cfs_bf_view_t *view, cfs_bf_id_t id,
    cfs_bf_entry_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace);
int cfs_bf_lookup(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t directory, const void *name, uint16_t length,
    cfs_bf_entry_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace);
int cfs_bf_list(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t directory, cfs_bf_visit_fn visit, void *context,
    cfs_bt_scan_result_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace);

/* Mutations are synchronous and crash-atomic, with exclusive image ownership.
 * Buffers are disjoint. Refusal preserves out; uncertain I/O poisons store. */
int cfs_bf_initialize(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_attributes_t attributes,
    cfs_bf_created_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace);
int cfs_bf_create(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t parent, const void *name,
    uint16_t length, uint16_t type, cfs_bf_attributes_t attributes,
    cfs_bf_created_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace);

/* Add one name for an existing linked nondirectory inode in the branch. New
 * names only; bumps parent name count, target link count, and timestamps. */
int cfs_bf_link(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t target, cfs_bf_id_t parent,
    const void *name, uint16_t length, cfs_bf_time_t time,
    uint64_t *out_revision, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *workspace);

/* type REGULAR selects nondirectory unlink, DIRECTORY empty-directory removal;
 * keep_open=1 orphans a final-link target, keeping its owning references. */
int cfs_bf_remove(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t parent, const void *name,
    uint16_t length, uint16_t type, int keep_open, cfs_bf_time_t time,
    uint64_t *out_revision, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace);

/* Atomic same-branch move with replacement: directories replace only empty
 * directories, nondirectories any nondirectory. keep_open as in remove. */
int cfs_bf_rename(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t source_parent,
    const void *source_name, uint16_t source_length,
    cfs_bf_id_t destination_parent, const void *destination_name,
    uint16_t destination_length, int keep_open, cfs_bf_time_t time,
    uint64_t *out_revision,
    cfs_bs_workspace_t *storage, cfs_bf_workspace_t *workspace);

#endif /* CFS_BRANCH_FS_H */
