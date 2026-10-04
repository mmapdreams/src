/* SPDX-License-Identifier: ISC
 * branch_assemble.h -- hidden filesystem assembly from validated change events.
 */
#ifndef CFS_BRANCH_ASSEMBLE_H
#define CFS_BRANCH_ASSEMBLE_H

#include "branch_change.h"
#include "branch_store_staging.h"

/* Both callbacks are NULL or both set. Each successful begin pairs one end. */
typedef struct cfs_bf_mutation_observer {
    void *context;
    int (*begin)(void *context);
    void (*end)(void *context, int raw_status);
} cfs_bf_mutation_observer_t;

/* Valid only in its serialized image. Failure invalidates state but retains
 * staging identity; observer context is borrowed through termination. */
typedef struct cfs_bf_assemble {
    cfs_bs_staging_t staging;
    cfs_bf_change_header_t header;
    cfs_bf_mutation_observer_t mutations;
    cfs_bs_ref_t base_root;
    cfs_bf_view_t base;
    cfs_bs_ref_t inodes;
    cfs_bs_ref_t orphans;
    cfs_bs_ref_t content;
    cfs_bf_entry_t original;
    cfs_bf_inode_t result;
    cfs_bf_id_t current;
    uint64_t events;
    uint32_t active;
    uint32_t physical_before;
    uint32_t result_present;
    uint32_t content_seen;
    uint32_t state;
    int failure;
} cfs_bf_assemble_t;

typedef struct cfs_bf_assemble_workspace {
    cfs_bf_workspace_t namespace;
    cfs_bf_file_workspace_t file;
    cfs_bf_symlink_workspace_t symlink;
    cfs_bf_assemble_t next;
    cfs_bs_ref_t refs[4];
    uint8_t payload[CFS_BF_ROOT_BYTES];
    uint8_t metadata[CFS_BF_INODE_BYTES];
} cfs_bf_assemble_workspace_t;

/* Caller provides a pristine equivalent base. Begin retains it and copies the
 * observer; output changes only on success. Recover after provider failure. */
int cfs_bf_assemble_begin(cfs_bs_store_t *store, uint64_t stage,
    uint64_t expected, const cfs_bf_change_header_t *header,
    cfs_bf_assemble_t *out, cfs_bs_workspace_t *storage,
    cfs_bf_assemble_workspace_t *workspace,
    cfs_bf_mutation_observer_t mutations);

/* Ordered decoder events may combine regular data to CFS_BF_IO_MAX. No input
 * pointer is retained; calls may commit prior work separately. */
int cfs_bf_assemble_change(cfs_bs_store_t *store, cfs_bf_assemble_t *assembly,
    const cfs_bf_change_t *change, cfs_bs_workspace_t *storage,
    cfs_bf_assemble_workspace_t *workspace);

/* Produces only a hidden candidate. Receipt and whole-result validation remain
 * required before publication; output changes only on success. */
int cfs_bf_assemble_finish(cfs_bs_store_t *store, cfs_bf_assemble_t *assembly,
    cfs_bs_staging_t *out, cfs_bs_workspace_t *storage,
    cfs_bf_assemble_workspace_t *workspace);

#endif /* CFS_BRANCH_ASSEMBLE_H */
