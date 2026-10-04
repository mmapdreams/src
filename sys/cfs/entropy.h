/* SPDX-License-Identifier: ISC
 *
 * entropy.h -- borrowed exact-fill entropy capability for cfs_core.
 *
 * The caller owns the seam, provider context, scratch arena, and output.
 * Core borrows them synchronously and retains none of them. Concrete strong
 * platform sources belong to separately built providers.
 */

#ifndef CFS_ENTROPY_H
#define CFS_ENTROPY_H

#include <stddef.h>
#include <stdint.h>

#include "scratch.h"

enum {
#define CFS_ERROR_STATUS_OK(symbol, value) symbol = value,
#define CFS_ERROR_STATUS_NEGATIVE(symbol, value, posix, description) \
    symbol = value,
#include "error_catalog/entropy.def"
#undef CFS_ERROR_STATUS_NEGATIVE
#undef CFS_ERROR_STATUS_OK
};

typedef struct {
    void *ctx;
    int64_t (*read)(void *ctx, void *buf, size_t length);
} cfs_entropy_t;

/* Fill exactly length bytes through caller-owned scratch, publishing to out
 * after every source call succeeds; nonpositive or over-long read is EIO. */
int cfs_entropy_fill(const cfs_entropy_t *entropy, cfs_scratch_t *scratch,
    void *out, size_t length);

#endif /* CFS_ENTROPY_H */
