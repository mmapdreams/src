/* SPDX-License-Identifier: ISC
 *
 * block.c -- device-agnostic block backend: geometry, bounds, and the
 * short-transfer retry loop over the cfs_block_dev seam.
 */
#include "block.h"

#include "format.h"

/* Validate a [blkno, blkno + nblocks) request and a non-NULL buffer. Returns
 * CFS_BLK_OK with *off and *len set, or a negative CFS_BLK_*. */
static int block_check(const cfs_block_backend *bk, uint64_t blkno,
                       uint32_t nblocks, const void *buf, uint64_t *off,
                       size_t *len) {
    if (bk == NULL || buf == NULL) {
        return CFS_BLK_EINVAL;
    }

    if (nblocks == 0) {
        return CFS_BLK_EINVAL;
    }

    /* Overflow-safe bounds: never form blkno + nblocks. Also rejects the
     * CFS_BLKNO_NONE sentinel, which exceeds any real block_count. */
    if (blkno >= bk->block_count ||
        (uint64_t)nblocks > bk->block_count - blkno) {
        return CFS_BLK_ERANGE;
    }

    /* nblocks is bounded by block_count above, so blkno + nblocks cannot
     * overflow. Reject a geometry whose end byte offset would wrap uint64. */
    uint64_t end_block = blkno + (uint64_t)nblocks;
    if (end_block > UINT64_MAX / CFS_BLOCK_SIZE) {
        return CFS_BLK_ERANGE;
    }

    /* Byte length must fit size_t. On 32-bit hosts the ~44-bit product wraps
     * and shorts the transfer; the divide-back catches only that case. */
    size_t len_bytes = (size_t)nblocks * CFS_BLOCK_SIZE;
    if (len_bytes / CFS_BLOCK_SIZE != nblocks) {
        return CFS_BLK_EINVAL;
    }

    *off = cfs_block_byte_offset(blkno);
    *len = len_bytes;
    return CFS_BLK_OK;
}

int cfs_block_read(const cfs_block_backend *bk, uint64_t blkno,
                   uint32_t nblocks, void *buf) {
    uint64_t off;
    size_t len;
    int rc = block_check(bk, blkno, nblocks, buf, &off, &len);
    if (rc != CFS_BLK_OK) {
        return rc;
    }

    if (!bk->dev.read) {
        return CFS_BLK_EINVAL;
    }

    uint8_t *p = (uint8_t *)buf;
    size_t done = 0;
    while (done < len) {
        int64_t n = bk->dev.read(bk->dev.ctx, off + done, p + done,
                                 len - done);
        if (n < 0) {
            return CFS_BLK_EIO;
        }

        if (n == 0) {
            return CFS_BLK_EIO;
        }

        /* A device that reports more than was requested is broken; refuse
         * it rather than advance past the buffer on the next iteration. */
        if ((uint64_t)n > len - done) {
            return CFS_BLK_EIO;
        }

        done += (size_t)n;
    }

    return CFS_BLK_OK;
}

int cfs_block_write(const cfs_block_backend *bk, uint64_t blkno,
                    uint32_t nblocks, const void *buf) {
    uint64_t off;
    size_t len;
    int rc = block_check(bk, blkno, nblocks, buf, &off, &len);
    if (rc != CFS_BLK_OK) {
        return rc;
    }

    if (!bk->dev.write) {
        return CFS_BLK_EINVAL;
    }

    const uint8_t *p = (const uint8_t *)buf;
    size_t done = 0;
    while (done < len) {
        int64_t n = bk->dev.write(bk->dev.ctx, off + done, p + done,
                                  len - done);
        if (n < 0) {
            return CFS_BLK_EIO;
        }

        if (n == 0) {
            return CFS_BLK_EIO;
        }

        if ((uint64_t)n > len - done) {
            return CFS_BLK_EIO;
        }

        done += (size_t)n;
    }

    return CFS_BLK_OK;
}

int cfs_block_sync(const cfs_block_backend *bk) {
    if (bk == NULL || bk->dev.sync == NULL) {
        return CFS_BLK_EINVAL;
    }

    if (bk->dev.sync(bk->dev.ctx) != 0) {
        return CFS_BLK_ESYNC;
    }

    return CFS_BLK_OK;
}
