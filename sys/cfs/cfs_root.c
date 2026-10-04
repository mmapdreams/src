/* SPDX-License-Identifier: ISC
 *
 * cfs_root.c -- cfs as the root filesystem: open the image on rootdev
 * read-write and select the boot branch from CFS_ROOT_BRANCH or the console.
 * The loader boots the kernel from partition a, an FFS boot partition; a root
 * device naming a moves to partition CFS_ROOT_PART of the same disk.
 */
#include "cfs_kern_internal.h"

#include <sys/device.h>
#include <sys/disklabel.h>
#include <sys/proc.h>
#include <sys/reboot.h>

#include <dev/cons.h>

#ifndef CFS_ROOT_BRANCH
#define CFS_ROOT_BRANCH 16
#endif

#ifndef CFS_ROOT_PART
#define CFS_ROOT_PART 3
#endif

_Static_assert(CFS_ROOT_BRANCH > 0, "CFS_ROOT_BRANCH must be nonzero");
_Static_assert(CFS_ROOT_PART > 0 && CFS_ROOT_PART < MAXPARTITIONS,
    "CFS_ROOT_PART must name a partition other than a");

/* Decimal branch id; zero, overflow and junk refuse. */
static int parse_branch(const char *text, uint64_t *out) {
    uint64_t value = 0;

    if (*text == '\0') {
        return EINVAL;
    }

    for (; *text != '\0'; text++) {
        if (*text < '0' || *text > '9' ||
            value > (UINT64_MAX - (uint64_t)(*text - '0')) / 10u) {
            return EINVAL;
        }

        value = value * 10u + (uint64_t)(*text - '0');
    }

    if (value == 0u) {
        return EINVAL;
    }

    *out = value;
    return 0;
}

/* RB_ASKNAME: read a branch id from the console; empty keeps the default. */
static uint64_t ask_branch(uint64_t fallback) {
    char buf[32];
    uint64_t branch;
    int s;

    for (;;) {
        printf("cfs root branch (default %llu): ",
            (unsigned long long)fallback);
        s = splhigh();
        cnpollc(1);
        size_t len = getsn(buf, sizeof(buf));
        cnpollc(0);
        splx(s);
        if (len == 0) {
            return fallback;
        }

        if (parse_branch(buf, &branch) == 0) {
            return branch;
        }

        printf("cfs: bad branch id \"%s\"\n", buf);
    }
}

/* Newest of the root inode's mtime and ctime, for inittodr. */
static time_t root_time(struct mount *mp) {
    struct cfs_kmount *km = VFSTOCFS(mp);
    cfs_bw_stat_t st;
    time_t when = 0;

    cfs_owner_enter(km);
    if (cfs_bw_stat(&km->km_image.working, km->km_image.branch_id,
        km->km_root, &st) == CFS_BS_OK) {
        when = (time_t)st.inode.mtime.seconds;
        if ((time_t)st.inode.ctime.seconds > when) {
            when = (time_t)st.inode.ctime.seconds;
        }
    }

    cfs_owner_exit(km);
    return when;
}

int cfs_mountroot(void) {
    struct proc *p = curproc;
    uint64_t branch = CFS_ROOT_BRANCH;
    char name[MNAMELEN];
    struct mount *mp;
    int error;

    if (DISKPART(rootdev) == 0) {
        rootdev = MAKEDISKDEV(major(rootdev), DISKUNIT(rootdev),
            CFS_ROOT_PART);
    }

    for (;;) {
        if (boothowto & RB_ASKNAME) {
            branch = ask_branch(branch);
        }

        error = bdevvp(rootdev, &rootvp);
        if (error != 0) {
            printf("cfs_mountroot: can't setup bdevvp\n");
            return error;
        }

        snprintf(name, sizeof(name), "/dev/%s%d%c",
            findblkname(major(rootdev)), DISKUNIT(rootdev),
            DL_PARTNUM2NAME(DISKPART(rootdev)));
        error = vfs_rootmountalloc(MOUNT_CFS, name, &mp);
        if (error != 0) {
            vrele(rootvp);
            rootvp = NULL;
            return error;
        }

        mp->mnt_flag &= ~MNT_RDONLY;
        error = cfs_mount_device(mp, rootvp, branch, p);
        if (error == 0) {
            break;
        }

        vfs_unbusy(mp);
        vfs_mount_free(mp);
        rootvp = NULL;
        printf("cfs_mountroot: branch %llu: error %d\n",
            (unsigned long long)branch, error);
        if (!(boothowto & RB_ASKNAME)) {
            return error;
        }
    }

    TAILQ_INSERT_TAIL(&mountlist, mp, mnt_list);
    (void)VFS_STATFS(mp, &mp->mnt_stat, p);
    printf("cfs: root on %s branch %llu\n", name,
        (unsigned long long)branch);
    time_t when = root_time(mp);
    vfs_unbusy(mp);
    inittodr(when);
    return 0;
}
