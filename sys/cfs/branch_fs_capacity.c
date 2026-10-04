/* SPDX-License-Identifier: ISC
 * branch_fs_capacity.c -- geometry-derived namespace construction reserve.
 */
#include "branch_fs.h"
#include "branch_store_internal.h"

enum {
    MAX_INTERIOR_ENTRY = CFS_BT_SLOT_SIZE + CFS_BT_MAX_KEY +
        sizeof(uint64_t) + CFS_BS_REF_SIZE,
    WEIGHT_FANOUT = (CFS_BT_MIN_ENTRY_BYTES + MAX_INTERIOR_ENTRY - 1u) /
        MAX_INTERIOR_ENTRY,
    MIN_FANOUT = WEIGHT_FANOUT < CFS_BT_MIN_RECORDS ?
        WEIGHT_FANOUT : CFS_BT_MIN_RECORDS
};

/* These encoded limits bound every recurrence below well within uint64_t. */
_Static_assert(MIN_FANOUT == 6u && CFS_BT_MAX_HEIGHT == 16u,
    "rederive namespace reserve arithmetic for changed tree limits");

uint32_t cfs_bf_reserve_blocks(uint64_t image_blocks) {
    if (image_blocks < CFS_BS_MIN_BLOCKS || image_blocks > CFS_BS_MAX_BLOCKS) {
        return 0u;
    }

    uint64_t minimum_nodes = 1u;
    uint64_t next_level = 2u;
    uint32_t height = 1u;
    while (height < CFS_BT_MAX_HEIGHT &&
        next_level <= image_blocks - minimum_nodes) {
        minimum_nodes += next_level;
        next_level *= MIN_FANOUT;
        height++;
    }

    /* Free-block floor for an unlink's three point edits of at most 2h+1
     * objects each, then one root; rename publishes up to seven, not three. */
    return 3u * (2u * height + 1u) + 1u;
}
