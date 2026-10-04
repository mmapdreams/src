/* SPDX-License-Identifier: ISC
 *
 * cfs_vnops_file.c -- file vnode operations: open, close, access, getattr,
 * setattr, read, write and fsync over the image's shared working owner.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/ucred.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <uvm/uvm.h>

#include "cfs_kern_internal.h"
#include "branch_metadata.h"

#define CFS_VA_BLOCKSIZE 4096u

/* Unlocked callers (fstat) may sleep here while a forced unmount frees
 * node and closes the image, so read node first and check after. */
static int node_stat(struct cfs_kmount *km, const struct cfs_knode *node,
    cfs_bw_stat_t *out) {
    cfs_bf_id_t id = node->cnd_id;

    cfs_owner_enter(km);
    int raw = !km->km_image.open ? CFS_BS_EIO :
        cfs_bw_stat(&km->km_image.working, km->km_image.branch_id, id, out);
    cfs_owner_exit(km);
    return cfs_errno(raw);
}

/* The pagedaemon may not sleep on the owner lock or for memory: a holder
 * may be waiting on it. Contention is EAGAIN; the page stays dirty. */
static int owner_enter_pageout(struct cfs_kmount *km, int pageout) {
    if (!pageout) {
        cfs_owner_enter(km);
        return 0;
    }

    return cfs_owner_try(km) ? 0 : EAGAIN;
}

/* Data-path refusals: an offset or size past INT64_MAX is EFBIG. */
static int data_errno(int raw) {
    return raw == CFS_BS_EOVERFLOW ? EFBIG : cfs_kimage_space_errno(raw);
}

/* Space held by pending reclamation refuses with EWORKSPACE; reclaim and let
 * the caller retry once. Pageout must not reclaim: it retries later. */
static int space_retry(struct cfs_kmount *km, int raw, int pageout) {
    return raw == CFS_BS_EWORKSPACE && !pageout &&
        cfs_kimage_reclaim(&km->km_image) == 0;
}

static struct timespec timespec_of(cfs_bf_time_t time) {
    return (struct timespec){.tv_sec = (time_t)time.seconds,
        .tv_nsec = (long)time.nanoseconds};
}

static int time_of(const struct timespec *ts, cfs_bf_time_t *out) {
    if (ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L) {
        return EINVAL;
    }

    *out = (cfs_bf_time_t){(int64_t)ts->tv_sec, (uint32_t)ts->tv_nsec};
    return 0;
}

int cfs_open(void *v) {
    (void)v;
    return 0;
}

int cfs_close(void *v) {
    struct vop_close_args *ap = v;
    struct cfs_kmount *km = VTOCFSMNT(ap->a_vp);

    if (ap->a_fflag & FWRITE) {
        cfs_owner_enter(km);
        cfs_service_touch(km, CFS_SVC_WRITEBACK);
        cfs_owner_exit(km);
    }

    return 0;
}

int cfs_access(void *v) {
    struct vop_access_args *ap = v;
    struct vnode *vp = ap->a_vp;

    if ((ap->a_mode & VWRITE) && (vp->v_mount->mnt_flag & MNT_RDONLY)) {
        return EROFS;
    }

    cfs_bw_stat_t st;
    int error = node_stat(VTOCFSMNT(vp), VTOCFS(vp), &st);
    if (error != 0) {
        return error;
    }

    return vaccess(vp->v_type, st.inode.mode & 01777u, st.inode.uid,
        st.inode.gid, ap->a_mode, ap->a_cred);
}

int cfs_getattr(void *v) {
    struct vop_getattr_args *ap = v;
    struct vnode *vp = ap->a_vp;
    struct vattr *vap = ap->a_vap;
    struct cfs_knode *node = VTOCFS(vp);
    ino_t ino = node->cnd_ino;
    int32_t fsid = vp->v_mount->mnt_stat.f_fsid.val[0];

    cfs_bw_stat_t st;
    int error = node_stat(VTOCFSMNT(vp), node, &st);
    if (error != 0) {
        return error;
    }

    const cfs_bf_inode_t *inode = &st.inode;
    if (inode->links > UINT32_MAX || inode->size > INT64_MAX) {
        return EOVERFLOW;
    }

    memset(vap, 0, sizeof(*vap));
    vap->va_type = vp->v_type;

    vap->va_mode = inode->mode & 07777u;
    vap->va_nlink = (nlink_t)inode->links;
    vap->va_uid = inode->uid;
    vap->va_gid = inode->gid;
    vap->va_fsid = fsid;
    vap->va_fileid = ino;
    vap->va_size = inode->size;
    vap->va_blocksize = CFS_VA_BLOCKSIZE;
    vap->va_atime = timespec_of(inode->atime);
    vap->va_mtime = timespec_of(inode->mtime);
    vap->va_ctime = timespec_of(inode->ctime);
    vap->va_gen = (u_long)st.revision;
    vap->va_rdev = NODEV;
    vap->va_bytes = roundup(inode->size, CFS_VA_BLOCKSIZE);
    vap->va_filerev = st.sequence;
    return 0;
}

/* The ufs rules: owner or root; chown only as root or to one's own group.
 * A non-root setgid needs the group; a non-root owner change clears set-id. */
static int authorize(struct vnode *vp, const cfs_bf_inode_t *inode,
    const struct vattr *vap, cfs_bf_update_t *u, struct ucred *cred,
    struct proc *p) {
    int root = suser_ucred(cred) == 0;
    int owner = cred->cr_uid == inode->uid;
    uint32_t uid = (u->fields & CFS_BF_SET_UID) ? u->uid : inode->uid;
    uint32_t gid = (u->fields & CFS_BF_SET_GID) ? u->gid : inode->gid;

    if (u->fields & CFS_BF_SET_MODE) {
        if (!root && !owner) {
            return EPERM;
        }

        if (!root && vp->v_type != VDIR && (u->mode & S_ISTXT)) {
            return EFTYPE;
        }

        if (!root && (u->mode & S_ISGID) && !groupmember(gid, cred)) {
            return EPERM;
        }
    }

    if (u->fields & (CFS_BF_SET_UID | CFS_BF_SET_GID)) {
        if (!root && (!owner || uid != inode->uid ||
            (gid != inode->gid && !groupmember(gid, cred)))) {
            return EPERM;
        }

        uint16_t mode = inode->mode;
        if (!root && uid != inode->uid) {
            mode &= (uint16_t)~S_ISUID;
        }

        if (!root && gid != inode->gid) {
            mode &= (uint16_t)~S_ISGID;
        }

        if (!(u->fields & CFS_BF_SET_MODE) && mode != inode->mode) {
            u->mode = mode;
            u->fields |= CFS_BF_SET_MODE;
        }
    }

    if (vap->va_flags != VNOVAL && !root && !owner) {
        return EPERM;
    }

    if ((u->fields & (CFS_BF_SET_ATIME | CFS_BF_SET_MTIME)) && !root &&
        !owner) {
        if (!(vap->va_vaflags & VA_UTIMES_NULL)) {
            return EPERM;
        }

        return VOP_ACCESS(vp, VWRITE, cred, p);
    }

    return 0;
}

static int update_of(struct vnode *vp, const struct vattr *vap,
    cfs_bf_update_t *u) {
    *u = (cfs_bf_update_t){0};
    if (vap->va_size != VNOVAL) {
        if (vp->v_type == VDIR) {
            return EISDIR;
        }

        if (vp->v_type != VREG) {
            return EINVAL;
        }

        if (vap->va_size > INT64_MAX) {
            return EFBIG;
        }

        u->fields |= CFS_BF_SET_SIZE;
        u->size = vap->va_size;
    }

    if (vap->va_uid != (uid_t)VNOVAL) {
        u->fields |= CFS_BF_SET_UID;
        u->uid = vap->va_uid;
    }

    if (vap->va_gid != (gid_t)VNOVAL) {
        u->fields |= CFS_BF_SET_GID;
        u->gid = vap->va_gid;
    }

    if (vap->va_mode != (mode_t)VNOVAL) {
        u->fields |= CFS_BF_SET_MODE;
        u->mode = (uint16_t)(vap->va_mode & 07777u);
    }

    int error = 0;
    if (vap->va_atime.tv_nsec != VNOVAL) {
        u->fields |= CFS_BF_SET_ATIME;
        error = time_of(&vap->va_atime, &u->atime);
    }

    if (error == 0 && vap->va_mtime.tv_nsec != VNOVAL) {
        u->fields |= CFS_BF_SET_MTIME;
        error = time_of(&vap->va_mtime, &u->mtime);
    }

    return error;
}

int cfs_setattr(void *v) {
    struct vop_setattr_args *ap = v;
    struct vnode *vp = ap->a_vp;
    struct vattr *vap = ap->a_vap;
    struct cfs_kmount *km = VTOCFSMNT(vp);
    struct cfs_knode *node = VTOCFS(vp);

    if (vap->va_type != VNON || vap->va_nlink != VNOVAL ||
        vap->va_fsid != VNOVAL || vap->va_fileid != VNOVAL ||
        vap->va_blocksize != VNOVAL || vap->va_rdev != VNOVAL ||
        vap->va_bytes != VNOVAL || vap->va_gen != VNOVAL) {
        return EINVAL;
    }

    if (vap->va_flags != VNOVAL && vap->va_flags != 0) {
        return EOPNOTSUPP;
    }

    cfs_bf_update_t u;
    int error = update_of(vp, vap, &u);
    if (error != 0) {
        return error;
    }

    if (u.fields == 0u && vap->va_flags == VNOVAL &&
        !(vap->va_vaflags & VA_UTIMES_CHANGE)) {
        return 0;
    }

    if (vp->v_mount->mnt_flag & MNT_RDONLY) {
        return EROFS;
    }

    cfs_bw_stat_t st;
    error = node_stat(km, node, &st);
    if (error == 0) {
        error = authorize(vp, &st.inode, vap, &u, ap->a_cred, ap->a_p);
    }

    if (error != 0 || u.fields == 0u) {
        return error;
    }

    cfs_bf_time_t now = cfs_now();
    cfs_owner_enter(km);
    if (u.fields == CFS_BF_SET_SIZE) {
        uint64_t sequence;
        int raw = cfs_bw_truncate(&km->km_image.working,
            km->km_image.branch_id, node->cnd_id, u.size, now, &sequence);
        if (space_retry(km, raw, 0)) {
            raw = cfs_bw_truncate(&km->km_image.working,
                km->km_image.branch_id, node->cnd_id, u.size, now,
                &sequence);
        }

        if (raw == CFS_BS_OK) {
            cfs_service_touch(km, CFS_SVC_WRITEBACK);
        }

        error = data_errno(raw);
    } else {
        cfs_bw_scratch_t scratch;
        uint64_t revision;
        error = cfs_ns_begin(km, &scratch);
        if (error == 0) {
            int raw = cfs_bf_setattr(&km->km_image.store,
                km->km_image.branch_id, km->km_image.branch.revision,
                node->cnd_id, u, now, &revision, scratch.storage, scratch.ns,
                scratch.file);
            error = cfs_ns_end(km, raw);
        }

        if (error == EOVERFLOW && (u.fields & CFS_BF_SET_SIZE)) {
            error = EFBIG;
        }
    }

    cfs_owner_exit(km);

    if (error != 0) {
        return error;
    }

    long hint = NOTE_ATTRIB;
    if (u.fields & CFS_BF_SET_SIZE) {
        uvm_vnp_setsize(vp, (off_t)u.size);
        (void)uvm_vnp_uncache(vp);
        if (u.size < st.inode.size) {
            hint |= NOTE_TRUNCATE;
        }
    }

    VN_KNOTE(vp, hint);
    return 0;
}

/* Non-root writes clear set-id bits as ffs_write does, but before the data:
 * a crash or failed write leaves them cleared. Reclaims and retries once. */
static int clear_setid(struct cfs_kmount *km, struct cfs_knode *node,
    uint16_t mode) {
    cfs_bf_update_t u = {.fields = CFS_BF_SET_MODE,
        .mode = (uint16_t)(mode & ~(S_ISUID | S_ISGID))};
    cfs_bw_scratch_t scratch;
    uint64_t revision;
    int error = 0;

    cfs_owner_enter(km);
    for (int attempt = 0; attempt < 2; attempt++) {
        error = cfs_ns_begin(km, &scratch);
        if (error != 0) {
            break;
        }

        int raw = cfs_bf_setattr(&km->km_image.store, km->km_image.branch_id,
            km->km_image.branch.revision, node->cnd_id, u, cfs_now(),
            &revision, scratch.storage, scratch.ns, scratch.file);
        error = cfs_ns_end(km, raw);
        if (raw != CFS_BS_EWORKSPACE || attempt > 0 ||
            cfs_kimage_reclaim(&km->km_image) != 0) {
            break;
        }
    }

    cfs_owner_exit(km);
    return error;
}

/* No vnode lock taken: VOP_READ also runs from the pager (uvn_io) with the
 * vnode already locked by the faulting path. */
int cfs_read(void *v) {
    struct vop_read_args *ap = v;
    struct vnode *vp = ap->a_vp;
    struct uio *uio = ap->a_uio;
    struct cfs_kmount *km = VTOCFSMNT(vp);
    struct cfs_knode *node = VTOCFS(vp);

    if (vp->v_type == VDIR) {
        return EISDIR;
    }

    if (vp->v_type != VREG || uio->uio_offset < 0) {
        return EINVAL;
    }

    if (uio->uio_resid == 0) {
        return 0;
    }

    void *bounce = cfs_kimage_bounce_get();
    int error = 0;
    while (uio->uio_resid > 0) {
        uint64_t offset = (uint64_t)uio->uio_offset;
        uint64_t length = MIN((uint64_t)uio->uio_resid, CFS_BF_IO_MAX);
        length = MIN(length, INT64_MAX - offset);
        if (length == 0u) {
            break;
        }

        uint32_t count = 0u;
        cfs_owner_enter(km);
        int raw = cfs_bw_read(&km->km_image.working, km->km_image.branch_id,
            node->cnd_id, offset, bounce, (uint32_t)length, &count);
        cfs_owner_exit(km);
        if (raw != CFS_BS_OK) {
            error = cfs_errno(raw);
            break;
        }

        if (count == 0u) {
            break;
        }

        error = uiomove(bounce, count, uio);
        if (error != 0 || count < length) {
            break;
        }
    }

    cfs_kimage_bounce_put(bounce);
    return error;
}

/* Each chunk is one all-or-nothing engine write. A refused chunk after an
 * accepted one is a short write; IO_SYNC forces what was accepted. */
int cfs_write(void *v) {
    struct vop_write_args *ap = v;
    struct vnode *vp = ap->a_vp;
    struct uio *uio = ap->a_uio;
    struct cfs_kmount *km = VTOCFSMNT(vp);
    struct cfs_knode *node = VTOCFS(vp);
    int append = (ap->a_ioflag & IO_APPEND) != 0;

    if (vp->v_type != VREG) {
        return EINVAL;
    }

    if (vp->v_mount->mnt_flag & MNT_RDONLY) {
        return EROFS;
    }

    if (uio->uio_resid == 0) {
        return 0;
    }

    int pageout = curproc == uvm.pagedaemon_proc;
    cfs_bw_stat_t st;
    int error = owner_enter_pageout(km, pageout);
    if (error != 0) {
        return error;
    }

    error = cfs_errno(cfs_bw_stat(&km->km_image.working,
        km->km_image.branch_id, node->cnd_id, &st));
    cfs_owner_exit(km);
    if (error != 0) {
        return error;
    }

    if (!pageout && (st.inode.mode & (S_ISUID | S_ISGID)) &&
        ap->a_cred != NULL && ap->a_cred->cr_uid != 0) {
        error = clear_setid(km, node, st.inode.mode);
        if (error != 0) {
            return error;
        }
    }

    uint64_t size = st.inode.size;
    if (append) {
        uio->uio_offset = (off_t)size;
    }

    if (uio->uio_offset < 0 ||
        (uint64_t)uio->uio_resid > INT64_MAX - (uint64_t)uio->uio_offset) {
        return EFBIG;
    }

    ssize_t overrun;
    error = vn_fsizechk(vp, uio, ap->a_ioflag, &overrun);
    if (error != 0) {
        return error;
    }

    void *bounce = pageout ? malloc(CFS_BF_IO_MAX, M_MISCFSMNT, M_NOWAIT) :
        cfs_kimage_bounce_get();
    if (!bounce) {
        return EAGAIN;
    }

    uint64_t original = size;
    uint64_t sequence = 0u;
    int accepted = 0;
    while (uio->uio_resid > 0) {
        off_t offset = uio->uio_offset;
        ssize_t resid = uio->uio_resid;
        uint32_t length = (uint32_t)MIN((uint64_t)resid, CFS_BF_IO_MAX);
        error = uiomove(bounce, length, uio);
        if (error == 0) {
            error = owner_enter_pageout(km, pageout);
        }

        if (error == 0) {
            int raw = append ? cfs_bw_stat(&km->km_image.working,
                km->km_image.branch_id, node->cnd_id, &st) : CFS_BS_OK;
            if (raw == CFS_BS_OK) {
                raw = cfs_bw_write(&km->km_image.working,
                    km->km_image.branch_id, node->cnd_id, (uint64_t)offset,
                    bounce, length, append, cfs_now(), &sequence);
            }

            if (space_retry(km, raw, pageout)) {
                raw = cfs_bw_write(&km->km_image.working,
                    km->km_image.branch_id, node->cnd_id, (uint64_t)offset,
                    bounce, length, append, cfs_now(), &sequence);
            }

            if (raw == CFS_BS_OK) {
                cfs_service_touch(km, CFS_SVC_WRITEBACK);
            }

            cfs_owner_exit(km);
            error = pageout && raw == CFS_BS_EWORKSPACE ? EAGAIN :
                data_errno(raw);
        }

        if (error != 0) {
            uio->uio_offset = offset;
            uio->uio_resid = resid;
            break;
        }

        accepted = 1;
        uint64_t end = (append ? st.inode.size : (uint64_t)offset) + length;
        if (append) {
            uio->uio_offset = (off_t)end;
        }

        if (end > size) {
            size = end;
            uvm_vnp_setsize(vp, (off_t)size);
        }
    }

    cfs_kimage_bounce_put(bounce);
    uio->uio_resid += overrun;
    if (!accepted) {
        return error;
    }

    /* A pageout writes the cached page itself; uncaching would wait on it. */
    if (!pageout) {
        (void)uvm_vnp_uncache(vp);
    }

    VN_KNOTE(vp, NOTE_WRITE | (size > original ? NOTE_EXTEND : 0));
    error = 0;
    if (ap->a_ioflag & IO_SYNC) {
        cfs_owner_enter(km);
        error = cfs_kimage_checkpoint(&km->km_image, sequence);
        cfs_owner_exit(km);
    }

    return error;
}

/* Image-wide: any vnode, directories included, forces the accepted prefix
 * captured here; later writers do not extend it. */
int cfs_fsync(void *v) {
    struct vop_fsync_args *ap = v;
    struct cfs_kmount *km = VTOCFSMNT(ap->a_vp);

    if (ap->a_waitfor == MNT_LAZY) {
        if (!cfs_owner_try(km)) {
            return 0;
        }
    } else {
        cfs_owner_enter(km);
    }

    cfs_bw_status_t status;
    int error = cfs_errno(cfs_bw_status(&km->km_image.working, &status));
    if (error == 0) {
        error = cfs_kimage_checkpoint(&km->km_image,
            status.accepted_sequence);
    }

    cfs_owner_exit(km);
    return error;
}
