/* SPDX-License-Identifier: ISC
 *
 * block.h -- the device-agnostic block backend: whole-block reads,
 * writes, and sync at byte-defined geometry, driven through a caller-
 * provided device seam.
 *
 * This is the sole core block-I/O seam. It owns the geometry arithmetic
 * (block number to byte offset, per-transfer bounds) and short-transfer
 * retry loop, but opens, closes, or allocates nothing and neither calls
 * libc nor consults errno. All I/O goes through the cfs_block_dev hooks
 * the caller supplies. The concrete userland device (a file descriptor)
 * lives in src/block_posix.{h,c}, outside cfs_core; a kernel device would
 * sit beside it. See txt/block_backend.md.
 *
 * cfs_core is kernel-destined: this header pulls in only stddef.h and
 * stdint.h and defines no allocation, stdio, or floating point.
 */
#ifndef CFS_BLOCK_H
#define CFS_BLOCK_H

#include <stddef.h>
#include <stdint.h>

/* Block-backend result codes, the core's own error vocabulary: 0 is success,
 * every failure a distinct negative value. */
enum {
#define CFS_ERROR_STATUS_OK(symbol, value) symbol = value,
#define CFS_ERROR_STATUS_NEGATIVE(symbol, value, posix, description) \
    symbol = value,
#include "error_catalog/block.def"
#undef CFS_ERROR_STATUS_NEGATIVE
#undef CFS_ERROR_STATUS_OK
};

/* The block-device seam: each hook is optional until an operation needs it.
 * The backend borrows ctx, which must outlive everything that can reach it. */
typedef struct cfs_block_dev {
    void *ctx;

    /* Transfer up to `len` bytes at absolute offset `off`, returning the count
     * moved. A short count is retried; anything else becomes CFS_BLK_EIO. */
    int64_t (*read)(void *ctx, uint64_t off, void *buf, size_t len);
    int64_t (*write)(void *ctx, uint64_t off, const void *buf, size_t len);

    /* Make prior writes durable. Return 0 on success; every nonzero result is
     * a provider failure and is normalized to CFS_BLK_ESYNC. */
    int (*sync)(void *ctx);
} cfs_block_dev;

/* An opened backend: the device seam plus capacity. The image spans
 * block_count * CFS_BLOCK_SIZE bytes; block numbers are [0, block_count). */
typedef struct cfs_block_backend {
    cfs_block_dev dev;
    uint64_t block_count;
} cfs_block_backend;

/* Read `nblocks` whole blocks at `blkno` into `buf`, which must hold
 * nblocks * CFS_BLOCK_SIZE bytes. Needs dev.read. */
int cfs_block_read(const cfs_block_backend *bk, uint64_t blkno,
                   uint32_t nblocks, void *buf);

/* Write `nblocks` whole blocks from `buf` at `blkno`. Needs dev.write; higher
 * COW layers withhold publication when this fails. */
int cfs_block_write(const cfs_block_backend *bk, uint64_t blkno,
                    uint32_t nblocks, const void *buf);

/* Flush the device durable. Only dev.sync is required; a missing hook invokes
 * no hook and is CFS_BLK_EINVAL. Every nonzero hook result is CFS_BLK_ESYNC. */
int cfs_block_sync(const cfs_block_backend *bk);

#endif /* CFS_BLOCK_H */
