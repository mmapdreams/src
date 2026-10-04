/* SPDX-License-Identifier: ISC
 * branch_working_internal.h -- private coherent working-owner lifecycle.
 */
#ifndef CFS_BRANCH_WORKING_INTERNAL_H
#define CFS_BRANCH_WORKING_INTERNAL_H

#include "branch_working.h"

int cfs_bw_owner_valid(const cfs_bw_t *owner);
int cfs_bw_ready(cfs_bw_t *owner);
int cfs_bw_observe(cfs_bw_t *owner, uint64_t branch, cfs_bf_id_t id,
    cfs_bw_slot_t **out, int *working);
int cfs_bw_admit(cfs_bw_t *owner, uint32_t candidate, uint32_t root,
    uint64_t bytes, uint64_t *out_sequence);
int cfs_bw_checkpoint_internal(cfs_bw_t *owner, uint64_t through,
    int pressure);
int cfs_bw_latch(cfs_bw_t *owner, int status);

#endif /* CFS_BRANCH_WORKING_INTERNAL_H */
