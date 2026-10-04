/* SPDX-License-Identifier: ISC
 *
 * cfs_vnops.c -- the cfs vnode operations table and its trivial operations.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/lockf.h>
#include <sys/unistd.h>

#include "cfs_kern_internal.h"

const struct vops cfs_vops = {
    .vop_lock = cfs_lock,
    .vop_unlock = cfs_unlock,
    .vop_islocked = cfs_islocked,
    .vop_abortop = cfs_abortop,
    .vop_access = cfs_access,
    .vop_advlock = cfs_advlock,
    .vop_bmap = cfs_bmap,
    .vop_bwrite = vop_generic_bwrite,
    .vop_close = cfs_close,
    .vop_create = cfs_create,
    .vop_fsync = cfs_fsync,
    .vop_getattr = cfs_getattr,
    .vop_inactive = cfs_inactive,
    .vop_ioctl = cfs_ioctl,
    .vop_link = cfs_link,
    .vop_lookup = cfs_lookup,
    .vop_mknod = cfs_mknod,
    .vop_open = cfs_open,
    .vop_pathconf = cfs_pathconf,
    .vop_print = cfs_print,
    .vop_read = cfs_read,
    .vop_readdir = cfs_readdir,
    .vop_readlink = cfs_readlink,
    .vop_reclaim = cfs_reclaim,
    .vop_remove = cfs_remove,
    .vop_rename = cfs_rename,
    .vop_revoke = vop_generic_revoke,
    .vop_mkdir = cfs_mkdir,
    .vop_rmdir = cfs_rmdir,
    .vop_setattr = cfs_setattr,
    .vop_strategy = cfs_strategy,
    .vop_symlink = cfs_symlink,
    .vop_write = cfs_write,
    .vop_kqfilter = cfs_kqfilter,
};

int cfs_pathconf(void *v) {
    struct vop_pathconf_args *ap = v;

    switch (ap->a_name) {
    case _PC_LINK_MAX:
        *ap->a_retval = LINK_MAX;
        return 0;
    case _PC_NAME_MAX:
        *ap->a_retval = CFS_NAME_MAX;
        return 0;
    case _PC_CHOWN_RESTRICTED:
    case _PC_NO_TRUNC:
    case _PC_TIMESTAMP_RESOLUTION:
        *ap->a_retval = 1;
        return 0;
    case _PC_FILESIZEBITS:
        *ap->a_retval = 64;
        return 0;
    default:
        return EINVAL;
    }
}

int cfs_ioctl(void *v) {
    (void)v;
    return ENOTTY;
}

static void cfs_filt_detach(struct knote *kn) {
    struct vnode *vp = (struct vnode *)kn->kn_hook;

    klist_remove_locked(&vp->v_klist, kn);
}

/* File data is never cached here, so reads and writes are always ready. */
static int cfs_filt_ready(struct knote *kn, long hint) {
    if (hint == NOTE_REVOKE) {
        kn->kn_flags |= EV_EOF | EV_ONESHOT;
    }

    return 1;
}

static int cfs_filt_vnode(struct knote *kn, long hint) {
    if (kn->kn_sfflags & hint) {
        kn->kn_fflags |= hint;
    }

    if (hint == NOTE_REVOKE) {
        kn->kn_flags |= EV_EOF;
        return 1;
    }

    return kn->kn_fflags != 0;
}

static const struct filterops cfs_ready_filtops = {
    .f_flags = FILTEROP_ISFD,
    .f_detach = cfs_filt_detach,
    .f_event = cfs_filt_ready,
};

static const struct filterops cfs_vnode_filtops = {
    .f_flags = FILTEROP_ISFD,
    .f_detach = cfs_filt_detach,
    .f_event = cfs_filt_vnode,
};

int cfs_kqfilter(void *v) {
    struct vop_kqfilter_args *ap = v;
    struct vnode *vp = ap->a_vp;
    struct knote *kn = ap->a_kn;

    switch (kn->kn_filter) {
    case EVFILT_READ:
    case EVFILT_WRITE:
        kn->kn_fop = &cfs_ready_filtops;
        break;
    case EVFILT_VNODE:
        kn->kn_fop = &cfs_vnode_filtops;
        break;
    default:
        return EINVAL;
    }

    kn->kn_hook = (caddr_t)vp;
    klist_insert_locked(&vp->v_klist, kn);
    return 0;
}

int cfs_bmap(void *v) {
    (void)v;
    return EOPNOTSUPP;
}

int cfs_strategy(void *v) {
    (void)v;
    return EOPNOTSUPP;
}

int cfs_advlock(void *v) {
    struct vop_advlock_args *ap = v;
    struct cfs_knode *np = VTOCFS(ap->a_vp);
    struct cfs_kmount *km = np->cnd_km;
    cfs_bw_stat_t st;
    int status;

    cfs_owner_enter(km);
    status = cfs_bw_stat(&km->km_image.working, km->km_image.branch_id,
        np->cnd_id, &st);
    cfs_owner_exit(km);
    if (status != CFS_BS_OK) {
        return cfs_errno(status);
    }

    if (st.inode.size > INT64_MAX) {
        return EFBIG;
    }

    return lf_advlock(&np->cnd_lockf, (off_t)st.inode.size, ap->a_id,
        ap->a_op, ap->a_fl, ap->a_flags);
}

int cfs_abortop(void *v) {
    return vop_generic_abortop(v);
}
