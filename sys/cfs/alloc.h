/* SPDX-License-Identifier: ISC
 *
 * alloc.h -- explicit host-memory allocation capability for cfs_core.
 *
 * This seam is distinct from allocation of logical filesystem blocks. The
 * caller owns the capability and its context; core operations borrow both
 * synchronously and retain neither. Allocation and release always carry the
 * complete byte size and alignment so no hidden ownership header is needed.
 */

#ifndef CFS_ALLOC_H
#define CFS_ALLOC_H

#include <stddef.h>

enum {
#define CFS_ERROR_STATUS_OK(symbol, value) symbol = value,
#define CFS_ERROR_STATUS_NEGATIVE(symbol, value, posix, description) \
    symbol = value,
#include "error_catalog/alloc.def"
#undef CFS_ERROR_STATUS_NEGATIVE
#undef CFS_ERROR_STATUS_OK
};

/* A copyable, non-owning capability: copies borrow the same hooks and exact
 * context, and never clone, retain, or reference-count it. */
typedef struct {
    void *ctx;
    int (*alloc)(void *ctx, size_t size, size_t alignment, void **out);
    void (*free)(void *ctx, void *ptr, size_t size, size_t alignment);
} cfs_alloc_t;

/* Acquire count * element_size writable bytes aligned to alignment. Both hooks
 * are required; on failure *out is unchanged and nothing is caller-owned. */
int cfs_alloc_acquire(const cfs_alloc_t *alloc, size_t count,
    size_t element_size, size_t alignment, void **out);

/* Release with the exact shape used to acquire, exactly once, through the
 * originating capability or a copy of it. Mismatch is not diagnosed. */
int cfs_alloc_release(const cfs_alloc_t *alloc, void *ptr, size_t count,
    size_t element_size, size_t alignment);

#endif /* CFS_ALLOC_H */
