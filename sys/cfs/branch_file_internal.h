/* SPDX-License-Identifier: ISC
 * branch_file_internal.h -- sparse radix framing and traversal declarations.
 */
#ifndef CFS_BRANCH_FILE_INTERNAL_H
#define CFS_BRANCH_FILE_INTERNAL_H

#include "branch_file.h"

enum { CFS_BF_FILE_READ = 1, CFS_BF_FILE_WRITE = 2, CFS_BF_FILE_TRUNCATE = 3 };

typedef struct cfs_bf_file_request {
    cfs_bs_child_t root;
    uint64_t old_size;
    uint64_t new_size;
    uint64_t offset;
    const uint8_t *bytes;
    uint32_t length;
    uint32_t kind;
} cfs_bf_file_request_t;

cfs_bs_child_t cfs_bf_file_empty(void);

/* Stage size changes only, without inode edits or publication. Validated
 * lengths and disjoint scratch are supplied by the public operation layer. */
int cfs_bf_file_resize(cfs_bs_store_t *store, cfs_bs_ref_t root,
    uint64_t old_size, uint64_t size, cfs_bs_child_t *out,
    cfs_bs_workspace_t *storage, cfs_bf_workspace_t *namespace_workspace,
    cfs_bf_file_workspace_t *workspace);
int cfs_bf_file_present(cfs_bs_child_t child);
uint64_t cfs_bf_file_chunks(uint64_t size);
int cfs_bf_file_node_decode(const void *bytes, uint32_t length,
    uint32_t children, uint8_t *out_level);
int cfs_bf_file_load(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    cfs_bs_child_t child, int level, uint64_t base, uint64_t size, int root,
    cfs_bf_file_frame_t *frame, cfs_bs_workspace_t *store_workspace,
    cfs_bf_file_workspace_t *workspace);
int cfs_bf_file_emit(cfs_bt_stage_t *stage, const cfs_bf_file_frame_t *frame,
    cfs_bs_child_t *out, cfs_bf_file_workspace_t *workspace);
int cfs_bf_file_chunk(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    cfs_bs_child_t child, uint64_t number, uint64_t size,
    cfs_bs_workspace_t *store_workspace, cfs_bf_file_workspace_t *workspace);
int cfs_bf_file_walk(cfs_bs_store_t *store,
    const cfs_bt_stage_t *source_stage, cfs_bt_stage_t *destination_stage,
    cfs_bf_file_request_t request, cfs_bs_child_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bf_file_workspace_t *workspace);

#endif /* CFS_BRANCH_FILE_INTERNAL_H */
