/* SPDX-License-Identifier: ISC
 * branch_change_internal.h -- stream-local ordering validation declarations.
 */
#ifndef CFS_BRANCH_CHANGE_INTERNAL_H
#define CFS_BRANCH_CHANGE_INTERNAL_H

#include "branch_change.h"

int cfs_bf_change_order(cfs_bf_change_decoder_t *decoder);
int cfs_bf_change_group_end(const cfs_bf_change_group_t *group);

#endif /* CFS_BRANCH_CHANGE_INTERNAL_H */
