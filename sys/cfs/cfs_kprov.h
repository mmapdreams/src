/* SPDX-License-Identifier: ISC
 *
 * cfs_kprov.h -- OpenBSD kernel providers for the freestanding engine: the
 * block-device backend over a disk partition vnode and the memory, clock and
 * entropy capabilities.
 *
 * Every function here sleeps and must be called from process context with
 * no spin lock held. The geometry helpers below touch no kernel header so a
 * hosted test can check them.
 *
 * Backend open takes a referenced, unlocked VBLK vnode, applies the ffs
 * exclusivity checks, opens it FREAD, or FREAD|FWRITE unless read_only, and
 * sizes the backend from its disklabel partition. On success the backend
 * owns the caller's reference and close releases it; on failure the caller
 * still owns it. Transfers are whole 4096-byte blocks through the device
 * buffer cache. A read-only backend refuses every write and sync and
 * records the refusal. Sync writes out dirty device buffers and issues
 * DIOCCACHESYNC if the probe at open found a cache flush. Close clears
 * devvp->v_specmountpoint after the last device write.
 */
#ifndef CFS_OPENBSD_KPROV_H
#define CFS_OPENBSD_KPROV_H

#include <stddef.h>
#include <stdint.h>
#include <sys/errno.h>

#include "alloc.h"
#include "block.h"
#include "clock.h"
#include "entropy.h"
#include "format.h"

struct proc;
struct ucred;
struct vnode;

/* An open partition. Fields are private; the engine sees only `block`. */
typedef struct cfs_kprov_backend {
    struct vnode *devvp;
    struct ucred *cred;
    uint64_t bytes;
    uint32_t sector_size;
    int open;
    int read_only;
    int write_refused;
    int cache_flush;
    cfs_block_backend block;
} cfs_kprov_backend_t;

/* Returns an errno; see the file comment for devvp ownership. */
int cfs_kprov_backend_open(cfs_kprov_backend_t *backend, struct vnode *devvp,
    struct ucred *cred, struct proc *proc, int read_only);

/* Flush, close and release devvp; always closes, returns the first errno. */
int cfs_kprov_backend_close(cfs_kprov_backend_t *backend, struct proc *proc);

/* Flush and close, leaving devvp's reference with the caller. */
int cfs_kprov_backend_abort(cfs_kprov_backend_t *backend, struct proc *proc);

cfs_alloc_t cfs_kprov_alloc(void);
cfs_clock_t cfs_kprov_clock(void);
cfs_entropy_t cfs_kprov_entropy(void);

/* Whole blocks in `sectors` sectors; the sector size must divide the block.
 * Returns CFS_BLK_OK, CFS_BLK_EINVAL or CFS_BLK_ERANGE on overflow. */
static inline int cfs_kprov_block_count(uint32_t sector_size,
    uint64_t sectors, uint64_t *out) {
    if (out == NULL || sector_size == 0u ||
        CFS_BLOCK_SIZE % sector_size != 0u) {
        return CFS_BLK_EINVAL;
    }

    if (sectors > UINT64_MAX / sector_size) {
        return CFS_BLK_ERANGE;
    }

    *out = sectors * sector_size / CFS_BLOCK_SIZE;
    return CFS_BLK_OK;
}

/* Admit a nonempty whole-block span inside the image and set its first block.
 * Partial blocks are CFS_BLK_EINVAL; out of range is CFS_BLK_ERANGE. */
static inline int cfs_kprov_span(uint64_t block_count, uint64_t offset,
    size_t length, uint64_t *first) {
    if (first == NULL || length == 0u || offset % CFS_BLOCK_SIZE != 0u ||
        length % CFS_BLOCK_SIZE != 0u) {
        return CFS_BLK_EINVAL;
    }

    uint64_t start = offset / CFS_BLOCK_SIZE;
    uint64_t count = (uint64_t)(length / CFS_BLOCK_SIZE);
    if (start > block_count || count > block_count - start) {
        return CFS_BLK_ERANGE;
    }

    *first = start;
    return CFS_BLK_OK;
}

/* DIOCCACHESYNC errors that mean the device has no cache flush. */
static inline int cfs_kprov_no_cache_flush(int error) {
    return error == ENOTTY || error == EOPNOTSUPP || error == ENODEV;
}

#endif /* CFS_OPENBSD_KPROV_H */
