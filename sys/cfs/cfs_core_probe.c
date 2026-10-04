/* SPDX-License-Identifier: ISC
 *
 * cfs_core_probe.c -- compile-only OpenBSD kernel/core seam probe.
 */

#include "cfs_kern.h"

#include "alloc.h"
#include "block.h"
#include "clock.h"
#include "entropy.h"
#include "format.h"
#include "lock.h"
#include "scratch.h"

int cfs_openbsd_core_probe(void);

int cfs_openbsd_core_probe(void) {
    int (*alloc_acquire)(const cfs_alloc_t *, size_t, size_t, size_t,
        void **) = cfs_alloc_acquire;
    int (*block_read)(const cfs_block_backend *, uint64_t, uint32_t,
        void *) = cfs_block_read;
    int (*clock_realtime)(const cfs_clock_t *, int64_t *) =
        cfs_clock_realtime_ns;
    int (*entropy_fill)(const cfs_entropy_t *, cfs_scratch_t *, void *,
        size_t) = cfs_entropy_fill;
    int (*lock_shared)(const cfs_lock_t *) = cfs_lock_acquire_shared;

    (void)alloc_acquire;
    (void)block_read;
    (void)clock_realtime;
    (void)entropy_fill;
    (void)lock_shared;

    return CFS_BLOCK_SIZE == 4096u ? 0 : EINVAL;
}
