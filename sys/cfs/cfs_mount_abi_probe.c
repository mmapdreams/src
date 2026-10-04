/* SPDX-License-Identifier: ISC
 *
 * cfs_mount_abi_probe.c -- compile-only kernel-side mount ABI probe.
 */

#include "cfs_kern.h"
#include "cfs_mount.h"

int cfs_openbsd_mount_abi_probe(const struct cfs_mount_args *);

int cfs_openbsd_mount_abi_probe(const struct cfs_mount_args *args) {
    if (args == 0) {
        return EINVAL;
    }

    return args->cma_version == CFS_MOUNT_ARGS_VERSION &&
            args->cma_size == CFS_MOUNT_ARGS_SIZE
        ? 0
        : EINVAL;
}
