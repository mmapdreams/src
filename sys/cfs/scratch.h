/* SPDX-License-Identifier: ISC
 *
 * scratch.h -- bounded caller-owned temporary storage for cfs_core.
 *
 * A scratch arena borrows its backing buffer and carves aligned slices from
 * it without allocation. Marks support nested, caller-managed lifetimes. The
 * core has no arena teardown operation and never frees or clears the backing.
 */

#ifndef CFS_SCRATCH_H
#define CFS_SCRATCH_H

#include <stddef.h>
#include <stdint.h>

enum {
#define CFS_ERROR_STATUS_OK(symbol, value) symbol = value,
#define CFS_ERROR_STATUS_NEGATIVE(symbol, value, posix, description) \
    symbol = value,
#include "error_catalog/scratch.def"
#undef CFS_ERROR_STATUS_NEGATIVE
#undef CFS_ERROR_STATUS_OK
};

typedef struct {
    uint8_t *base;
    size_t length;
    size_t used;
} cfs_scratch_t;

/* Arena, mark, and backing are caller-owned, disjoint, and naturally aligned.
 * Shared backing requires serialization. */
typedef struct {
    const cfs_scratch_t *arena;
    size_t used;
} cfs_scratch_mark_t;

/* Initialize an arena over borrowed storage, beginning one epoch that reinit or
 * abandonment ends. NULL is valid only with length zero. */
int cfs_scratch_init(cfs_scratch_t *scratch, void *buf, size_t length);

/* Record the current offset for a later reverse-order reset. */
/* On failure, *out, the arena, and its backing bytes are unchanged. */
int cfs_scratch_mark(const cfs_scratch_t *scratch,
    cfs_scratch_mark_t *out);

/* Carve a nonempty slice at a power-of-two absolute-address alignment. EINVAL,
 * EOVERFLOW on unrepresentable arithmetic, ENOSPC on exhaustion. */
int cfs_scratch_take(cfs_scratch_t *scratch, size_t size,
    size_t alignment, void **out);

/* Rewind to an intact, nonforward mark from this arena without clearing backing
 * bytes. Authenticity, epoch, and strict LIFO use are caller preconditions. */
int cfs_scratch_reset(cfs_scratch_t *scratch, cfs_scratch_mark_t mark);

#endif /* CFS_SCRATCH_H */
