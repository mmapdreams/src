/* SPDX-License-Identifier: ISC
 * branch_file.h -- retained range reads and atomic bounded file mutations.
 */
#ifndef CFS_BRANCH_FILE_H
#define CFS_BRANCH_FILE_H

#include "branch_fs.h"

#define CFS_BF_CHUNK_BYTES 4000u
#define CFS_BF_IO_MAX (31u * CFS_BF_CHUNK_BYTES)
#define CFS_BF_RADIX_SLOTS 128u
#define CFS_BF_RADIX_LEVEL_MAX 7u
#define CFS_BF_RADIX_BYTES 32u

typedef struct cfs_bf_file_state {
    cfs_bs_child_t root;
    uint64_t size;
} cfs_bf_file_state_t;

typedef struct cfs_bf_file_work {
    uint64_t nodes_loaded;
    uint64_t data_loaded;
    uint64_t nodes_emitted;
    uint64_t data_emitted;
    uint64_t slots_examined;
} cfs_bf_file_work_t;

typedef struct cfs_bf_file_frame {
    cfs_bs_child_t children[CFS_BF_RADIX_SLOTS];
    cfs_bs_child_t original;
    uint64_t base;
    uint16_t slot;
    uint8_t level;
    uint8_t dirty;
} cfs_bf_file_frame_t;

/* Private scratch, separate from both namespace and store workspaces. */
typedef struct cfs_bf_file_workspace {
    cfs_bf_file_frame_t path[CFS_BF_RADIX_LEVEL_MAX + 1u];
    cfs_bs_ref_t physical[CFS_BF_RADIX_SLOTS];
    cfs_bs_child_t packed[CFS_BF_RADIX_SLOTS];
    uint8_t node[CFS_BF_RADIX_BYTES];
    uint8_t chunk[CFS_BF_CHUNK_BYTES];
    uint8_t output[CFS_BF_IO_MAX];
    cfs_bf_file_work_t work;
    cfs_bs_branch_t branch;
    cfs_bf_view_t view;
    cfs_bf_entry_t entry;
} cfs_bf_file_workspace_t;

/* Content calls retain inputs and neither reset nor publish. Read refusal
 * preserves stage/output; mutation refusal discards stage and preserves out. */
int cfs_bf_file_content_read(cfs_bs_store_t *store,
    const cfs_bt_stage_t *stage, cfs_bf_file_state_t state, uint64_t offset,
    void *bytes, uint32_t length, uint32_t *out_length,
    cfs_bs_workspace_t *store_workspace, cfs_bf_file_workspace_t *workspace);
int cfs_bf_file_content_write(cfs_bs_store_t *store, cfs_bt_stage_t *stage,
    cfs_bf_file_state_t state, uint64_t offset, const void *bytes,
    uint32_t length, cfs_bf_file_state_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bf_file_workspace_t *workspace);
int cfs_bf_file_content_truncate(cfs_bs_store_t *store, cfs_bt_stage_t *stage,
    cfs_bf_file_state_t state, uint64_t size, cfs_bf_file_state_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bf_file_workspace_t *workspace);

/* Retain the view and serialize the image. Buffers/workspaces are disjoint.
 * Failure preserves bytes and count; only EOF permits a short success. */
int cfs_bf_file_read(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t id, uint64_t offset, void *bytes, uint32_t length,
    uint32_t *out_length, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *namespace_workspace, cfs_bf_file_workspace_t *workspace);

/* One all-or-nothing publication. Empty writes and equal-size truncates are
 * checked no-ops. A failed publication may require poisoned-owner recovery. */
int cfs_bf_file_write(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t id, uint64_t offset,
    const void *bytes, uint32_t length, cfs_bf_time_t time,
    uint64_t *out_revision, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *namespace_workspace, cfs_bf_file_workspace_t *workspace);
int cfs_bf_file_truncate(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t id, uint64_t size,
    cfs_bf_time_t time, uint64_t *out_revision,
    cfs_bs_workspace_t *store_workspace, cfs_bf_workspace_t *namespace_workspace,
    cfs_bf_file_workspace_t *workspace);

#endif /* CFS_BRANCH_FILE_H */
