/* SPDX-License-Identifier: ISC
 *
 * cfs_kern.h -- minimal OpenBSD kernel compile surface for cfs_core.
 *
 * This is a compile-only adapter header.  The verbatim core receives its
 * documented freestanding C headers separately; this file adds only the real
 * OpenBSD kernel types and errno vocabulary needed by the adapter probe.  It
 * owns no provider, storage, mount, or VFS state.
 */

#ifndef CFS_OPENBSD_KERN_H
#define CFS_OPENBSD_KERN_H

#ifndef _KERNEL
#error "cfs_kern.h requires the OpenBSD kernel compile environment"
#endif

#include <sys/types.h>
#include <sys/errno.h>

#define CFS_OPENBSD_KERNEL_SCAFFOLD_VERSION 1u

_Static_assert(sizeof(size_t) == 8, "OpenBSD amd64 size_t width changed");
_Static_assert(sizeof(void *) == 8, "OpenBSD amd64 pointer width changed");

#endif /* CFS_OPENBSD_KERN_H */
