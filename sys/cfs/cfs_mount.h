/* SPDX-License-Identifier: ISC
 *
 * cfs_mount.h -- shared OpenBSD cfs mount-argument ABI.
 */
#ifndef CFS_OPENBSD_MOUNT_H
#define CFS_OPENBSD_MOUNT_H

#ifdef _KERNEL
#include <sys/types.h>
#else
#include <stdint.h>
#endif

#define CFS_MOUNT_ARGS_VERSION 1u
#define CFS_MOUNT_ARGS_SIZE 384u
#define CFS_MOUNT_SOURCE_BYTES 90u
#define CFS_MOUNT_SOURCE_MAX 89u
#define CFS_MOUNT_SNAPSHOT_NAME_BYTES 256u
#define CFS_MOUNT_SNAPSHOT_NAME_MAX 255u

#define CFS_MOUNT_ROOT_CURRENT 1u
#define CFS_MOUNT_ROOT_SNAPSHOT_ID 2u
#define CFS_MOUNT_ROOT_SNAPSHOT_NAME 3u

struct cfs_mount_args {
    uint32_t cma_version;
    uint32_t cma_size;
    uint32_t cma_flags;
    uint32_t cma_root_source;
    uint64_t cma_snapshot_id;
    uint32_t cma_source_len;
    uint32_t cma_snapshot_name_len;
    uint8_t cma_source[CFS_MOUNT_SOURCE_BYTES];
    uint8_t cma_reserved[6];
    uint8_t cma_snapshot_name[CFS_MOUNT_SNAPSHOT_NAME_BYTES];
};

#define CFS_MOUNT_OFFSET(member)                                      \
    __builtin_offsetof(struct cfs_mount_args, member)

_Static_assert(sizeof(uint8_t) == 1, "cfs mount ABI uint8_t width");
_Static_assert(sizeof(uint32_t) == 4, "cfs mount ABI uint32_t width");
_Static_assert(sizeof(uint64_t) == 8, "cfs mount ABI uint64_t width");
_Static_assert(CFS_MOUNT_OFFSET(cma_version) == 0,
    "cfs mount ABI version offset");
_Static_assert(CFS_MOUNT_OFFSET(cma_size) == 4,
    "cfs mount ABI size offset");
_Static_assert(CFS_MOUNT_OFFSET(cma_flags) == 8,
    "cfs mount ABI flags offset");
_Static_assert(CFS_MOUNT_OFFSET(cma_root_source) == 12,
    "cfs mount ABI root-source offset");
_Static_assert(CFS_MOUNT_OFFSET(cma_snapshot_id) == 16,
    "cfs mount ABI snapshot-id offset");
_Static_assert(CFS_MOUNT_OFFSET(cma_source_len) == 24,
    "cfs mount ABI source-length offset");
_Static_assert(CFS_MOUNT_OFFSET(cma_snapshot_name_len) == 28,
    "cfs mount ABI snapshot-name-length offset");
_Static_assert(CFS_MOUNT_OFFSET(cma_source) == 32,
    "cfs mount ABI source offset");
_Static_assert(CFS_MOUNT_OFFSET(cma_reserved) == 122,
    "cfs mount ABI reserved offset");
_Static_assert(CFS_MOUNT_OFFSET(cma_snapshot_name) == 128,
    "cfs mount ABI snapshot-name offset");
_Static_assert(sizeof(struct cfs_mount_args) == CFS_MOUNT_ARGS_SIZE,
    "cfs mount ABI size");
_Static_assert(_Alignof(struct cfs_mount_args) == 8,
    "cfs mount ABI alignment");

#undef CFS_MOUNT_OFFSET

#endif /* CFS_OPENBSD_MOUNT_H */
