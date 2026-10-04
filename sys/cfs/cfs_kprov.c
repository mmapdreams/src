/* SPDX-License-Identifier: ISC
 *
 * cfs_kprov.c -- OpenBSD partition backend and memory, clock and entropy
 * providers for the freestanding engine.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/disklabel.h>
#include <sys/dkio.h>
#include <sys/fcntl.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/specdev.h>
#include <sys/time.h>
#include <sys/ucred.h>
#include <sys/vnode.h>

#include "cfs_kprov.h"

/* Device transfers are at most MAXPHYS bytes and block aligned. */
#define CFS_KPROV_CHUNK (MAXPHYS - MAXPHYS % CFS_BLOCK_SIZE)

static size_t chunk(size_t remaining) {
    return remaining < CFS_KPROV_CHUNK ? remaining : CFS_KPROV_CHUNK;
}

static daddr_t device_block(uint64_t offset) {
    return (daddr_t)(offset / DEV_BSIZE);
}

/* Buffers are released uncached: the engine caches, and B_NOCACHE also keeps
 * bwrite synchronous on an MNT_ASYNC mount, so errors reach the caller. */
static int64_t backend_read(void *context, uint64_t offset, void *bytes,
    size_t length) {
    cfs_kprov_backend_t *backend = context;
    uint64_t first;

    if (bytes == NULL || cfs_kprov_span(backend->block.block_count, offset,
        length, &first) != CFS_BLK_OK) {
        return -1;
    }

    for (size_t done = 0u; done < length;) {
        size_t size = chunk(length - done);
        struct buf *bp = NULL;
        int error = bread(backend->devvp, device_block(offset + done),
            (int)size, &bp);
        if (error == 0 && bp->b_resid != 0) {
            error = EIO;
        }

        if (error == 0) {
            memcpy((uint8_t *)bytes + done, bp->b_data, size);
        }

        if (bp) {
            SET(bp->b_flags, B_NOCACHE);
            brelse(bp);
        }

        if (error != 0) {
            return -1;
        }

        done += size;
    }

    return (int64_t)length;
}

static int64_t backend_write(void *context, uint64_t offset,
    const void *bytes, size_t length) {
    cfs_kprov_backend_t *backend = context;
    uint64_t first;

    if (backend->read_only) {
        backend->write_refused = 1;
        return -1;
    }

    if (bytes == NULL || cfs_kprov_span(backend->block.block_count, offset,
        length, &first) != CFS_BLK_OK) {
        return -1;
    }

    for (size_t done = 0u; done < length;) {
        size_t size = chunk(length - done);
        struct buf *bp = getblk(backend->devvp, device_block(offset + done),
            (int)size, 0, INFSLP);
        if (!bp) {
            return -1;
        }

        memcpy(bp->b_data, (const uint8_t *)bytes + done, size);
        SET(bp->b_flags, B_NOCACHE);
        if (bwrite(bp) != 0) {
            return -1;
        }

        done += size;
    }

    return (int64_t)length;
}

static int cache_sync(struct vnode *devvp, struct ucred *cred,
    struct proc *p) {
    int force = 1;

    return VOP_IOCTL(devvp, DIOCCACHESYNC, &force, FWRITE, cred, p);
}

static int backend_sync(void *context) {
    cfs_kprov_backend_t *backend = context;
    struct proc *p = curproc;

    if (backend->read_only) {
        backend->write_refused = 1;
        return -1;
    }

    vn_lock(backend->devvp, LK_EXCLUSIVE | LK_RETRY);
    int error = VOP_FSYNC(backend->devvp, backend->cred, MNT_WAIT, p);
    VOP_UNLOCK(backend->devvp);
    if (error == 0 && backend->cache_flush) {
        error = cache_sync(backend->devvp, backend->cred, p);
    }

    return error;
}

/* Probe the cache flush once, so a later failure is a real sync failure. */
static int probe_cache_flush(struct vnode *devvp, struct proc *proc,
    int *out) {
    int error = cache_sync(devvp, FSCRED, proc);

    *out = error == 0;
    if (cfs_kprov_no_cache_flush(error)) {
        printf("cfs: device %d,%d has no cache flush\n", major(devvp->v_rdev),
            minor(devvp->v_rdev));
        error = 0;
    }

    return error;
}

int cfs_kprov_backend_open(cfs_kprov_backend_t *backend, struct vnode *devvp,
    struct ucred *cred, struct proc *proc, int read_only) {
    struct partinfo part;
    uint64_t blocks;
    int mode = read_only ? FREAD : FREAD | FWRITE;
    int cache_flush = 0;

    if (backend == NULL || devvp == NULL || cred == NULL) {
        return EINVAL;
    }

    memset(backend, 0, sizeof(*backend));
    if (devvp->v_type != VBLK) {
        return ENOTBLK;
    }

    int error = vfs_mountedon(devvp);
    if (error != 0) {
        return error;
    }

    if (vcount(devvp) > 1 && devvp != rootvp) {
        return EBUSY;
    }

    vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
    error = vinvalbuf(devvp, V_SAVE, cred, proc, 0, INFSLP);
    VOP_UNLOCK(devvp);
    if (error != 0) {
        return error;
    }

    error = VOP_OPEN(devvp, mode, FSCRED, proc);
    if (error != 0) {
        return error;
    }

    error = VOP_IOCTL(devvp, DIOCGPART, &part, FREAD, FSCRED, proc);
    if (error == 0 && cfs_kprov_block_count(part.disklab->d_secsize,
        DL_GETPSIZE(part.part), &blocks) != CFS_BLK_OK) {
        error = EINVAL;
    }

    if (error == 0 && !read_only) {
        error = probe_cache_flush(devvp, proc, &cache_flush);
    }

    if (error != 0) {
        vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
        (void)VOP_CLOSE(devvp, mode, NOCRED, proc);
        VOP_UNLOCK(devvp);
        return error;
    }

    crhold(cred);
    backend->devvp = devvp;
    backend->cred = cred;
    backend->sector_size = part.disklab->d_secsize;
    backend->bytes = blocks * CFS_BLOCK_SIZE;
    backend->open = 1;
    backend->read_only = read_only != 0;
    backend->cache_flush = cache_flush;
    backend->block = (cfs_block_backend){
        .dev = {.ctx = backend, .read = backend_read,
            .write = backend_write, .sync = backend_sync},
        .block_count = blocks,
    };
    return 0;
}

int cfs_kprov_backend_abort(cfs_kprov_backend_t *backend, struct proc *proc) {
    if (backend == NULL || !backend->open) {
        return EINVAL;
    }

    struct vnode *devvp = backend->devvp;
    vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
    int error = vinvalbuf(devvp, V_SAVE, backend->cred, proc, 0, INFSLP);
    devvp->v_specmountpoint = NULL;
    int closed = VOP_CLOSE(devvp, backend->read_only ? FREAD :
        FREAD | FWRITE, NOCRED, proc);
    VOP_UNLOCK(devvp);
    crfree(backend->cred);
    memset(backend, 0, sizeof(*backend));
    return error != 0 ? error : closed;
}

int cfs_kprov_backend_close(cfs_kprov_backend_t *backend, struct proc *proc) {
    if (backend == NULL || !backend->open) {
        return EINVAL;
    }

    struct vnode *devvp = backend->devvp;
    int error = cfs_kprov_backend_abort(backend, proc);
    vrele(devvp);
    return error;
}

/* Kernel malloc buckets are aligned to their size, up to a page. */
static size_t alloc_shape(size_t size, size_t alignment) {
    return size > alignment ? size : alignment;
}

static int kprov_alloc(void *context, size_t size, size_t alignment,
    void **out) {
    (void)context;
    if (out == NULL || size == 0u || alignment == 0u ||
        alignment > PAGE_SIZE || (alignment & (alignment - 1u)) != 0u) {
        return CFS_ALLOC_EINVAL;
    }

    void *ptr = malloc(alloc_shape(size, alignment), M_MISCFSMNT,
        M_WAITOK | M_CANFAIL);
    if (!ptr) {
        return CFS_ALLOC_ENOMEM;
    }

    *out = ptr;
    return CFS_ALLOC_OK;
}

static void kprov_free(void *context, void *ptr, size_t size,
    size_t alignment) {
    (void)context;
    free(ptr, M_MISCFSMNT, alloc_shape(size, alignment));
}

cfs_alloc_t cfs_kprov_alloc(void) {
    return (cfs_alloc_t){.alloc = kprov_alloc, .free = kprov_free};
}

static int clock_ns(const struct timespec *ts, int64_t *out) {
    if (ts->tv_sec > INT64_MAX / 1000000000 ||
        ts->tv_sec < INT64_MIN / 1000000000 + 1) {
        return CFS_CLOCK_ERANGE;
    }

    *out = (int64_t)ts->tv_sec * 1000000000 + ts->tv_nsec;
    return CFS_CLOCK_OK;
}

static int kprov_realtime(void *context, int64_t *out) {
    struct timespec ts;

    (void)context;
    nanotime(&ts);
    return clock_ns(&ts, out);
}

static int kprov_monotonic(void *context, uint64_t *out) {
    struct timespec ts;
    int64_t ns;

    (void)context;
    nanouptime(&ts);
    int status = clock_ns(&ts, &ns);
    if (status == CFS_CLOCK_OK) {
        *out = (uint64_t)ns;
    }

    return status;
}

cfs_clock_t cfs_kprov_clock(void) {
    return (cfs_clock_t){.realtime_ns = kprov_realtime,
        .monotonic_ns = kprov_monotonic};
}

static int64_t kprov_entropy(void *context, void *bytes, size_t length) {
    (void)context;
    if (bytes == NULL || length == 0u || length > INT64_MAX) {
        return -1;
    }

    arc4random_buf(bytes, length);
    return (int64_t)length;
}

cfs_entropy_t cfs_kprov_entropy(void) {
    return (cfs_entropy_t){.read = kprov_entropy};
}
