/* SPDX-License-Identifier: ISC
 *
 * cfs_control.h -- shared OpenBSD cfs online control ABI: one fixed-size
 * argument block passed by quotactl(2) on any path inside a cfs mount, and
 * the read-only vfs.cfs sysctl counters.
 */
#ifndef CFS_OPENBSD_CONTROL_H
#define CFS_OPENBSD_CONTROL_H

#ifdef _KERNEL
#include <sys/types.h>
#else
#include <stdint.h>
#endif

/* quotactl(2) command word; any other command on a cfs mount refuses. */
#define CFS_CONTROL_CMD 0x63667301
#define CFS_CONTROL_ARGS_VERSION 1u
#define CFS_CONTROL_ARGS_SIZE 1376u
#define CFS_CONTROL_NAME_BYTES 32u
#define CFS_CONTROL_NAME_MAX 31u
#define CFS_CONTROL_ROWS 16u
#define CFS_CONTROL_REAP_MAX 256u

#define CFS_CONTROL_STATS 1u
#define CFS_CONTROL_BRANCHES 2u
#define CFS_CONTROL_FORK 3u
#define CFS_CONTROL_DISCARD 4u
#define CFS_CONTROL_REAP 5u
#define CFS_CONTROL_CHECKPOINT 6u

/* vfs.cfs.<name>: read-only 64-bit counters since boot. */
#define CFS_VFS_MOUNTS 1
#define CFS_VFS_FORKS 2
#define CFS_VFS_DISCARDS 3
#define CFS_VFS_REAPED 4
#define CFS_VFS_CHECKPOINTS 5
#define CFS_VFS_MAXID 6

struct cfs_control_branch {
    uint64_t ccb_id;
    uint64_t ccb_revision;
    uint64_t ccb_origin_id;
    uint64_t ccb_origin_revision;
    uint32_t ccb_mounted;
    uint32_t ccb_name_len;
    uint8_t ccb_name[CFS_CONTROL_NAME_BYTES];
};

/* Space is in 4096-byte blocks; pending counts objects awaiting reap. */
struct cfs_control_stats {
    uint64_t ccs_blocks;
    uint64_t ccs_free;
    uint64_t ccs_available;
    uint64_t ccs_generation;
    uint64_t ccs_total_blocks;
    uint64_t ccs_semantic_start;
    uint64_t ccs_pending;
    uint64_t ccs_reserve;
    uint64_t ccs_accepted_sequence;
    uint64_t ccs_durable_sequence;
    uint64_t ccs_branch_id;
    uint64_t ccs_branch_revision;
    uint32_t ccs_branch_count;
    uint32_t ccs_pin_count;
    uint32_t ccs_stage_count;
    uint32_t ccs_consumers;
    uint32_t ccs_consumer_limit;
    uint32_t ccs_dirty;
    uint32_t ccs_delayed_error;
    uint32_t ccs_service_pending;
    uint32_t ccs_service_error;
    uint32_t ccs_service_retry;
    uint32_t ccs_read_only;
    uint32_t ccs_ready;
};

/* Inputs: version through name. Outputs: count, reaped, stats, rows; the
 * kernel zeroes them before an operation and fills them on success. */
struct cfs_control_args {
    uint32_t cca_version;
    uint32_t cca_size;
    uint32_t cca_op;
    uint32_t cca_flags;
    uint64_t cca_branch;
    uint64_t cca_expected;
    uint32_t cca_limit;
    uint32_t cca_name_len;
    uint8_t cca_name[CFS_CONTROL_NAME_BYTES];
    uint32_t cca_count;
    uint32_t cca_reaped;
    struct cfs_control_stats cca_stats;
    struct cfs_control_branch cca_rows[CFS_CONTROL_ROWS];
};

#define CFS_CONTROL_OFFSET(type, member) __builtin_offsetof(type, member)

_Static_assert(sizeof(struct cfs_control_branch) == 72,
    "cfs control ABI branch row size");
_Static_assert(CFS_CONTROL_OFFSET(struct cfs_control_branch, ccb_mounted) ==
    32, "cfs control ABI branch mounted offset");
_Static_assert(CFS_CONTROL_OFFSET(struct cfs_control_branch, ccb_name) == 40,
    "cfs control ABI branch name offset");
_Static_assert(sizeof(struct cfs_control_stats) == 144,
    "cfs control ABI stats size");
_Static_assert(CFS_CONTROL_OFFSET(struct cfs_control_stats,
    ccs_branch_count) == 96, "cfs control ABI stats count offset");
_Static_assert(CFS_CONTROL_OFFSET(struct cfs_control_args, cca_op) == 8,
    "cfs control ABI op offset");
_Static_assert(CFS_CONTROL_OFFSET(struct cfs_control_args, cca_branch) == 16,
    "cfs control ABI branch offset");
_Static_assert(CFS_CONTROL_OFFSET(struct cfs_control_args, cca_limit) == 32,
    "cfs control ABI limit offset");
_Static_assert(CFS_CONTROL_OFFSET(struct cfs_control_args, cca_name) == 40,
    "cfs control ABI name offset");
_Static_assert(CFS_CONTROL_OFFSET(struct cfs_control_args, cca_count) == 72,
    "cfs control ABI count offset");
_Static_assert(CFS_CONTROL_OFFSET(struct cfs_control_args, cca_stats) == 80,
    "cfs control ABI stats offset");
_Static_assert(CFS_CONTROL_OFFSET(struct cfs_control_args, cca_rows) == 224,
    "cfs control ABI rows offset");
_Static_assert(sizeof(struct cfs_control_args) == CFS_CONTROL_ARGS_SIZE,
    "cfs control ABI size");
_Static_assert(_Alignof(struct cfs_control_args) == 8,
    "cfs control ABI alignment");

#undef CFS_CONTROL_OFFSET

#ifdef _KERNEL
struct mount;
struct proc;

/* cfs_control.c: quotactl(2) and vfs.cfs entry points. */
int cfs_control(struct mount *mp, int cmd, caddr_t arg, struct proc *p);
int cfs_control_sysctl(int *name, u_int namelen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen);
#endif

#endif /* CFS_OPENBSD_CONTROL_H */
