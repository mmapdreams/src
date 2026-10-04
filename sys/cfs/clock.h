/* SPDX-License-Identifier: ISC
 *
 * clock.h -- borrowed realtime and monotonic clock capability for cfs_core.
 *
 * The caller owns the seam and provider context. The core borrows both
 * synchronously and retains neither. Concrete platform clocks belong to
 * separately built providers.
 */

#ifndef CFS_CLOCK_H
#define CFS_CLOCK_H

#include <stdint.h>

enum {
#define CFS_ERROR_STATUS_OK(symbol, value) symbol = value,
#define CFS_ERROR_STATUS_NEGATIVE(symbol, value, posix, description) \
    symbol = value,
#include "error_catalog/clock.def"
#undef CFS_ERROR_STATUS_NEGATIVE
#undef CFS_ERROR_STATUS_OK
};

typedef struct {
    void *ctx;
    int (*realtime_ns)(void *ctx, int64_t *out);
    int (*monotonic_ns)(void *ctx, uint64_t *out);
} cfs_clock_t;

/* Read signed Unix nanoseconds through the realtime hook. Realtime may move
 * backward after a wall-clock correction and is the only persisted domain. */
int cfs_clock_realtime_ns(const cfs_clock_t *clock, int64_t *out);

/* Read unsigned nanoseconds from a provider-private monotonic epoch. Comparable
 * only within one provider context and boot; never persisted. */
int cfs_clock_monotonic_ns(const cfs_clock_t *clock, uint64_t *out);

/* Each wrapper calls its selected hook once, publishing only on CFS_CLOCK_OK.
 * NULL seam, output, or hook is EINVAL; results but EIO/ERANGE become EIO. */

#endif /* CFS_CLOCK_H */
