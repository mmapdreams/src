/* SPDX-License-Identifier: ISC
 *
 * cfs_vfsops.c -- OpenBSD cfs mount, unmount, root, statfs, sync and file
 * handle operations over one kernel image owner per mount.
 */
#include "cfs_kern_internal.h"

#include <sys/disk.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/specdev.h>

#include "cfs_control.h"

_Static_assert(CFS_MOUNT_SOURCE_BYTES == MNAMELEN,
    "cfs mount source bound differs from MNAMELEN");
_Static_assert(sizeof(cfs_bf_id_t) == MAXFIDSZ,
    "cfs file handle does not fill struct fid");

static int zero_bytes(const uint8_t *bytes, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] != 0u) {
            return 0;
        }
    }

    return 1;
}

/* Validate the version 1 ABI and select the branch it names. */
static int mount_branch(const struct cfs_mount_args *args, uint64_t *out) {
    uint32_t length = args->cma_source_len;

    if (args->cma_version != CFS_MOUNT_ARGS_VERSION ||
        args->cma_size != CFS_MOUNT_ARGS_SIZE || args->cma_flags != 0u ||
        !zero_bytes(args->cma_reserved, sizeof(args->cma_reserved))) {
        return EINVAL;
    }

    if (length == 0u || length > CFS_MOUNT_SOURCE_MAX ||
        memchr(args->cma_source, '\0', length) ||
        !zero_bytes(args->cma_source + length,
            CFS_MOUNT_SOURCE_BYTES - length)) {
        return EINVAL;
    }

    switch (args->cma_root_source) {
    case CFS_MOUNT_ROOT_CURRENT:
        if (args->cma_snapshot_id != 0u ||
            args->cma_snapshot_name_len != 0u ||
            !zero_bytes(args->cma_snapshot_name,
                CFS_MOUNT_SNAPSHOT_NAME_BYTES)) {
            return EINVAL;
        }

        *out = CFS_BS_MAIN_ID;
        return 0;
    case CFS_MOUNT_ROOT_SNAPSHOT_ID:
        if (args->cma_snapshot_id == 0u ||
            args->cma_snapshot_name_len != 0u ||
            !zero_bytes(args->cma_snapshot_name,
                CFS_MOUNT_SNAPSHOT_NAME_BYTES)) {
            return EINVAL;
        }

        *out = args->cma_snapshot_id;
        return 0;
    case CFS_MOUNT_ROOT_SNAPSHOT_NAME:
        return EOPNOTSUPP;
    default:
        return EINVAL;
    }
}

/* Wait out unlocked vnode ops that still hold or wait for the owner lock. */
static void owner_drain(struct cfs_kmount *km) {
    while (atomic_load_int(&km->km_owners) != 0u) {
        tsleep_nsec(&km->km_owners, PVFS, "cfsdrain", MSEC_TO_NSEC(10));
    }
}

static void free_mount(struct cfs_kmount *km) {
    cfs_node_table_destroy(km);
    free(km->km_symlink, M_MISCFSMNT, sizeof(*km->km_symlink));
    free(km, M_MISCFSMNT, sizeof(*km));
}

/* Open the image on devvp and attach it to mp; consumes devvp on failure. */
int cfs_mount_device(struct mount *mp, struct vnode *devvp, uint64_t branch,
    struct proc *p) {
    struct cfs_kmount *km;
    ino_t root;
    int error;

    km = malloc(sizeof(*km), M_MISCFSMNT, M_WAITOK | M_ZERO);
    km->km_symlink = malloc(sizeof(*km->km_symlink), M_MISCFSMNT,
        M_WAITOK | M_ZERO);
    km->km_mp = mp;
    km->km_read_only = (mp->mnt_flag & MNT_RDONLY) != 0;
    rw_init(&km->km_lock, "cfsowner");
    cfs_node_table_init(km);
    cfs_service_init(km);

    error = cfs_kimage_open(&km->km_image, devvp, p->p_ucred, p, branch,
        CFS_KIMAGE_CACHE_BLOCKS, km->km_read_only);
    if (error != 0) {
        vrele(devvp);
        free_mount(km);
        return error;
    }

    km->km_root = km->km_image.view.root_id;
    error = cfs_ino(km, km->km_root, &root);
    if (error != 0) {
        printf("cfs: root identity does not map to an inode number\n");
        (void)cfs_kimage_close(&km->km_image, 1);
        free_mount(km);
        return error;
    }

    devvp->v_specmountpoint = mp;
    mp->mnt_data = km;
    mp->mnt_flag |= MNT_LOCAL;
    vfs_getnewfsid(mp);
    mp->mnt_stat.f_namemax = CFS_BF_NAME_MAX;

    /* The covered vnode is locked; a new owner is uncontended, so try it. */
    if (!cfs_owner_try(km)) {
        panic("cfs: new mount owner is contended");
    }

    if (!km->km_image.ready) {
        cfs_service_touch(km, CFS_SVC_REAP);
    }

    cfs_owner_exit(km);
    return 0;
}

/* An update that keeps the read-write state is a no-op; others refuse. */
static int cfs_mount_update(struct mount *mp) {
    struct cfs_kmount *km = mp->mnt_data;
    int read_only;

    if (mp->mnt_flag & MNT_RELOAD) {
        return EOPNOTSUPP;
    }

    read_only = (mp->mnt_flag & MNT_RDONLY) != 0 &&
        (mp->mnt_flag & MNT_WANTRDWR) == 0;
    if (read_only != km->km_read_only) {
        return EOPNOTSUPP;
    }

    return 0;
}

static int cfs_mount(struct mount *mp, const char *path, void *data,
    struct nameidata *ndp, struct proc *p) {
    const struct cfs_mount_args *args = data;
    char fspec[MNAMELEN];
    char fname[MNAMELEN];
    struct vnode *devvp;
    uint64_t branch;
    int error;

    if (mp->mnt_flag & MNT_UPDATE) {
        return cfs_mount_update(mp);
    }

    if (mp->mnt_flag & MNT_RELOAD) {
        return EOPNOTSUPP;
    }

    error = mount_branch(args, &branch);
    if (error != 0) {
        return error;
    }

    memset(fspec, 0, sizeof(fspec));
    memcpy(fspec, args->cma_source, args->cma_source_len);
    if (disk_map(fspec, fname, sizeof(fname), DM_OPENBLCK) == -1) {
        memcpy(fname, fspec, sizeof(fname));
    }

    NDINIT(ndp, LOOKUP, FOLLOW, UIO_SYSSPACE, fname, p);
    error = namei(ndp);
    if (error != 0) {
        return error;
    }

    devvp = ndp->ni_vp;
    if (devvp->v_type != VBLK) {
        vrele(devvp);
        return ENOTBLK;
    }

    if (major(devvp->v_rdev) >= nblkdev) {
        vrele(devvp);
        return ENXIO;
    }

    error = cfs_mount_device(mp, devvp, branch, p);
    if (error != 0) {
        return error;
    }

    memset(mp->mnt_stat.f_mntonname, 0, MNAMELEN);
    strlcpy(mp->mnt_stat.f_mntonname, path, MNAMELEN);
    memset(mp->mnt_stat.f_mntfromname, 0, MNAMELEN);
    strlcpy(mp->mnt_stat.f_mntfromname, fname, MNAMELEN);
    memset(mp->mnt_stat.f_mntfromspec, 0, MNAMELEN);
    strlcpy(mp->mnt_stat.f_mntfromspec, fspec, MNAMELEN);
    return 0;
}

static int cfs_start(struct mount *mp, int flags, struct proc *p) {
    (void)mp;
    (void)flags;
    (void)p;
    return 0;
}

/* Forced admission closes first; vflush repeats until no hold remains and
 * no lookup that unlocked its directory is in flight. */
static int cfs_unmount(struct mount *mp, int mntflags, struct proc *p) {
    struct cfs_kmount *km = VFSTOCFS(mp);
    int force = (mntflags & MNT_FORCE) != 0;
    int error;

    (void)p;
    cfs_owner_enter(km);
    km->km_unmounting = force;
    cfs_owner_exit(km);
    for (;;) {
        error = vflush(mp, NULL, force ? FORCECLOSE : 0);
        if (error != 0) {
            cfs_owner_enter(km);
            km->km_unmounting = 0;
            cfs_owner_exit(km);
            return error;
        }

        mtx_enter(&km->km_node_mtx);
        u_int busy = km->km_node_busy;
        mtx_leave(&km->km_node_mtx);
        cfs_owner_enter(km);
        km->km_unmounting = 1;
        if (busy == 0u && km->km_image.consumers.handles == 0u) {
            break;
        }

        cfs_owner_exit(km);
        tsleep_nsec(km, PVFS, "cfsflush", MSEC_TO_NSEC(10));
    }

    cfs_owner_exit(km);
    cfs_service_stop(km);
    owner_drain(km);
    cfs_owner_enter(km);
    error = cfs_kimage_close(&km->km_image, force);
    cfs_owner_exit(km);
    owner_drain(km);
    if (error != 0) {
        printf("cfs: %s: unmount%s: error %d\n",
            mp->mnt_stat.f_mntonname, force ? " (forced)" : "", error);
    }

    mp->mnt_data = NULL;
    mp->mnt_flag &= ~MNT_LOCAL;
    free_mount(km);
    return 0;
}

static int cfs_root(struct mount *mp, struct vnode **vpp) {
    struct cfs_kmount *km = VFSTOCFS(mp);

    return cfs_node_get(km, km->km_root, vpp);
}

/* No quotas: quotactl(2) carries the online control interface. */
static int cfs_quotactl(struct mount *mp, int cmd, uid_t uid, caddr_t arg,
    struct proc *p) {
    (void)uid;
    return cfs_control(mp, cmd, arg, p);
}

static int cfs_statfs(struct mount *mp, struct statfs *sbp, struct proc *p) {
    struct cfs_kmount *km = VFSTOCFS(mp);
    cfs_kimage_stats_t stats;
    int error = 0;

    (void)p;
    memset(&stats, 0, sizeof(stats));
    cfs_owner_enter(km);
    error = cfs_kimage_stats(&km->km_image, &stats);
    cfs_owner_exit(km);
    if (error != 0) {
        return error;
    }

    sbp->f_bsize = CFS_BLOCK_SIZE;
    sbp->f_iosize = CFS_BLOCK_SIZE;
    sbp->f_blocks = stats.blocks;
    sbp->f_bfree = stats.free;
    sbp->f_bavail = (int64_t)stats.available;
    sbp->f_files = 0;
    sbp->f_ffree = 0;
    sbp->f_favail = 0;
    sbp->f_namemax = CFS_BF_NAME_MAX;
    copy_statfs_info(sbp, mp);
    return 0;
}

/* Capture the accepted frontier and make it durable; lazy never sleeps. */
static int cfs_sync(struct mount *mp, int waitfor, int stall,
    struct ucred *cred, struct proc *p) {
    struct cfs_kmount *km = VFSTOCFS(mp);
    cfs_bw_status_t state;
    int error = 0;

    (void)stall;
    (void)cred;
    (void)p;
    if (waitfor == MNT_LAZY) {
        if (!cfs_owner_try(km)) {
            return 0;
        }

        if (cfs_bw_status(&km->km_image.working, &state) == CFS_BS_OK &&
            state.dirty != 0u) {
            cfs_service_touch(km, CFS_SVC_WRITEBACK);
        }

        cfs_owner_exit(km);
        return 0;
    }

    cfs_owner_enter(km);
    int status = cfs_bw_status(&km->km_image.working, &state);
    if (status != CFS_BS_OK) {
        error = cfs_errno(status);
    } else if (state.delayed_error != CFS_BS_OK) {
        error = cfs_errno(state.delayed_error);
    } else {
        error = cfs_kimage_checkpoint(&km->km_image,
            state.accepted_sequence);
    }

    cfs_owner_exit(km);
    return error;
}

static int cfs_vget(struct mount *mp, ino_t ino, struct vnode **vpp) {
    struct cfs_kmount *km = VFSTOCFS(mp);
    cfs_bf_id_t id;

    int error = cfs_ino_id(km, ino, &id);
    if (error == 0) {
        error = cfs_node_get(km, id, vpp);
    }

    return error == ENOENT ? ESTALE : error;
}

static int cfs_fhtovp(struct mount *mp, struct fid *fhp, struct vnode **vpp) {
    struct cfs_kmount *km = VFSTOCFS(mp);
    cfs_bf_id_t id;
    ino_t ino;

    if (fhp->fid_len != sizeof(*fhp)) {
        return EINVAL;
    }

    memcpy(&id, fhp->fid_data, sizeof(id));
    if (cfs_ino(km, id, &ino) != 0) {
        return ESTALE;
    }

    int error = cfs_node_get(km, id, vpp);
    return error == ENOENT ? ESTALE : error;
}

static int cfs_vptofh(struct vnode *vp, struct fid *fhp) {
    memset(fhp, 0, sizeof(*fhp));
    fhp->fid_len = sizeof(*fhp);
    memcpy(fhp->fid_data, &VTOCFS(vp)->cnd_id, sizeof(cfs_bf_id_t));
    return 0;
}

static int cfs_init(struct vfsconf *vfsp) {
    (void)vfsp;
    pool_init(&cfs_knode_pool, sizeof(struct cfs_knode), 0, IPL_NONE,
        PR_WAITOK, "cfsnode", NULL);
    cfs_service_create();
    return 0;
}

static int cfs_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen, struct proc *p) {
    (void)p;
    return cfs_control_sysctl(name, namelen, oldp, oldlenp, newp, newlen);
}

static int cfs_checkexp(struct mount *mp, struct mbuf *nam, int *exflagsp,
    struct ucred **credanonp) {
    (void)mp;
    (void)nam;
    (void)exflagsp;
    (void)credanonp;
    return EOPNOTSUPP;
}

const struct vfsops cfs_vfsops = {
    .vfs_mount = cfs_mount,
    .vfs_start = cfs_start,
    .vfs_unmount = cfs_unmount,
    .vfs_root = cfs_root,
    .vfs_quotactl = cfs_quotactl,
    .vfs_statfs = cfs_statfs,
    .vfs_sync = cfs_sync,
    .vfs_vget = cfs_vget,
    .vfs_fhtovp = cfs_fhtovp,
    .vfs_vptofh = cfs_vptofh,
    .vfs_init = cfs_init,
    .vfs_sysctl = cfs_sysctl,
    .vfs_checkexp = cfs_checkexp,
};

_Static_assert(sizeof(struct cfs_mount_args) == CFS_MOUNT_ARGS_SIZE,
    "cfs mount argument ABI size changed");
