/* SPDX-License-Identifier: ISC
 * branch_working.h -- shared coherent file batches and durable prefixes.
 */
#ifndef CFS_BRANCH_WORKING_H
#define CFS_BRANCH_WORKING_H

#include "branch_file.h"
#include "branch_store_prepare.h"

#define CFS_BW_BATCH_OPERATIONS 8u
#define CFS_BW_BATCH_BYTES 65536u
#define CFS_BW_BATCH_OBJECTS 96u

typedef struct cfs_bw_slot {
    cfs_bs_workspace_t storage;
    cfs_bf_workspace_t ns;
    cfs_bs_prepared_construct_t token;
    cfs_bs_branch_t branch;
    cfs_bf_view_t view;
    cfs_bf_entry_t entry;
    cfs_bf_file_state_t content;
} cfs_bw_slot_t;

/* Caller-owned, off-stack storage. All fields are private. */
typedef struct cfs_bw_workspace {
    cfs_bw_slot_t slots[2];
    cfs_bf_file_workspace_t file;
} cfs_bw_workspace_t;

typedef struct cfs_bw_work {
    uint64_t accepted_operations;
    uint64_t objects_cloned;
    uint64_t admission_attempts;
    uint64_t admission_objects;
    uint64_t checkpoints;
    uint64_t pressure_checkpoints;
} cfs_bw_work_t;

/* Sole store owner; caller serializes calls and brackets raw mutations.
 * Fresh/inactive, disjoint storage; no active copying or relocation. */
typedef struct cfs_bw {
    struct cfs_bw *identity;
    cfs_bs_store_t *store;
    cfs_bw_workspace_t *workspace;
    cfs_bw_work_t work;
    uint64_t accepted_sequence;
    uint64_t durable_sequence;
    uint64_t external_generation;
    uint64_t batch_bytes;
    uint32_t batch_operations;
    uint32_t active;
    uint32_t dirty;
    uint32_t external;
    int delayed_error;
} cfs_bw_t;

typedef struct cfs_bw_stat {
    cfs_bf_id_t id;
    cfs_bf_inode_t inode;
    uint64_t revision;
    uint64_t sequence;
} cfs_bw_stat_t;

typedef struct cfs_bw_status {
    uint64_t accepted_sequence;
    uint64_t durable_sequence;
    uint64_t branch;
    cfs_bf_id_t inode;
    uint64_t batch_bytes;
    uint32_t batch_operations;
    uint32_t stage_objects;
    uint32_t dirty;
    uint32_t external;
    uint32_t needs_checkpoint;
    int delayed_error;
    cfs_bw_work_t work;
} cfs_bw_status_t;

typedef struct cfs_bw_scratch {
    cfs_bs_workspace_t *storage;
    cfs_bf_workspace_t *ns;
    cfs_bf_file_workspace_t *file;
} cfs_bw_scratch_t;

int cfs_bw_init(cfs_bw_t *owner, cfs_bs_store_t *store,
    cfs_bw_workspace_t *workspace);

/* Status is non-I/O and remains available after a delayed failure. */
int cfs_bw_status(const cfs_bw_t *owner, cfs_bw_status_t *out);

/* Nonleased scratch; borrow ends before the next owner call. Raw reads allowed;
 * raw mutations need an external bracket. No retained working view. */
int cfs_bw_borrow(cfs_bw_t *owner, cfs_bw_scratch_t *out);

/* Every stat/read observes current working contents and metadata; revision
 * remains the durable base, sequence names this owner-lifetime observation. */
int cfs_bw_stat(cfs_bw_t *owner, uint64_t branch, cfs_bf_id_t id,
    cfs_bw_stat_t *out);
int cfs_bw_read(cfs_bw_t *owner, uint64_t branch, cfs_bf_id_t id,
    uint64_t offset, void *bytes, uint32_t length, uint32_t *out_length);

/* Changed operations accept once after admission; refusal preserves output.
 * Append is zero/one and ignores offset. Pressure may checkpoint prior work. */
int cfs_bw_write(cfs_bw_t *owner, uint64_t branch, cfs_bf_id_t id,
    uint64_t offset, const void *bytes, uint32_t length, int append,
    cfs_bf_time_t time, uint64_t *out_sequence);
int cfs_bw_truncate(cfs_bw_t *owner, uint64_t branch, cfs_bf_id_t id,
    uint64_t size, cfs_bf_time_t time, uint64_t *out_sequence);

/* Future prefixes refuse; durable ones do no I/O but report latched errors.
 * Publication may include a bounded later prefix; the owner stays valid. */
int cfs_bw_checkpoint(cfs_bw_t *owner, uint64_t through_sequence);
int cfs_bw_flush(cfs_bw_t *owner);

/* Begin flushes and reserves sequence space for one durable raw mutation.
 * End is mandatory with exact status; only a new commit advances frontiers. */
int cfs_bw_external_begin(cfs_bw_t *owner, uint64_t *out_sequence);
int cfs_bw_external_end(cfs_bw_t *owner, int status);

/* Shutdown flushes and invalidates only on success; errors stay observable.
 * Abort cancels reservations and drops unsynced state, without durability. */
int cfs_bw_shutdown(cfs_bw_t *owner);
int cfs_bw_abort(cfs_bw_t *owner);

#endif /* CFS_BRANCH_WORKING_H */
