/* SPDX-License-Identifier: ISC
 *
 * lock.h -- borrowed shared/exclusive lock capability for cfs_core.
 *
 * The caller owns the capability and provider context. The core borrows both
 * synchronously. It retains neither and never constructs, destroys, or
 * otherwise assumes ownership of either. Concrete synchronization belongs to
 * a separately built provider.
 */

#ifndef CFS_LOCK_H
#define CFS_LOCK_H

enum {
#define CFS_ERROR_STATUS_OK(symbol, value) symbol = value,
#define CFS_ERROR_STATUS_NEGATIVE(symbol, value, posix, description) \
    symbol = value,
#include "error_catalog/lock.def"
#undef CFS_ERROR_STATUS_NEGATIVE
#undef CFS_ERROR_STATUS_OK
};

typedef struct {
    void *ctx;
    int (*lock_shared)(void *ctx);
    int (*unlock_shared)(void *ctx);
    int (*lock_exclusive)(void *ctx);
    int (*unlock_exclusive)(void *ctx);
} cfs_lock_t;

/* Acquire through the selected provider hook, establishing its acquire
 * ordering. A NULL seam or missing hook is CFS_LOCK_EINVAL, no callback. */
int cfs_lock_acquire_shared(const cfs_lock_t *lock);

/* Release publishes prior writes with the provider's release ordering. */
int cfs_lock_release_shared(const cfs_lock_t *lock);

/* Providers return only CFS_LOCK_OK or CFS_LOCK_EIO; anything else normalizes
 * to CFS_LOCK_EIO. A failed acquisition grants no ownership. */
int cfs_lock_acquire_exclusive(const cfs_lock_t *lock);

/* A failed release leaves ownership indeterminate: fatal to production
 * callers. */
int cfs_lock_release_exclusive(const cfs_lock_t *lock);

#endif /* CFS_LOCK_H */
