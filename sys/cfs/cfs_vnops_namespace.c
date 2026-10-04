/* SPDX-License-Identifier: ISC
 *
 * cfs_vnops_namespace.c -- namespace vnode operations: lookup, create,
 * mknod, mkdir, remove, rmdir, link, symlink, readlink, rename, readdir.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/dirent.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/ucred.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include "cfs_kern_internal.h"

/* Readdir cookies: inode birth above CFS_COOKIE_TIE_BITS of name hash. */
#define CFS_COOKIE_TIE_BITS 12u
#define CFS_COOKIE_DOT 1
#define CFS_COOKIE_DOTDOT 2
#define CFS_READDIR_ENTRIES 64u

/* Space refusal: end the bracket, reclaim, reopen one for a single retry.
 * Returns 1 to retry; 0 when the refusal stands with the bracket closed. */
static int ns_space_retry(struct cfs_kmount *km, cfs_bw_scratch_t *scratch,
    int *raw) {
    if (*raw != CFS_BS_EWORKSPACE) {
        return 0;
    }

    (void)cfs_ns_end(km, *raw);
    if (cfs_kimage_reclaim(&km->km_image) != 0 ||
        cfs_ns_begin(km, scratch) != 0) {
        return 0;
    }

    return 1;
}

static int stat_locked(struct cfs_kmount *km, cfs_bf_id_t id,
    cfs_bw_stat_t *out) {
    return cfs_errno(cfs_bw_stat(&km->km_image.working,
        km->km_image.branch_id, id, out));
}

static uint16_t engine_type(enum vtype type) {
    switch (type) {
    case VREG:
        return CFS_BF_REGULAR;
    case VDIR:
        return CFS_BF_DIRECTORY;
    case VLNK:
        return CFS_BF_SYMLINK;
    default:
        return 0;
    }
}

static int is_dot(const struct componentname *cnp) {
    return cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.';
}

static int is_dotdot(const struct componentname *cnp) {
    return cnp->cn_namelen == 2 && cnp->cn_nameptr[0] == '.' &&
        cnp->cn_nameptr[1] == '.';
}

static void free_name(struct componentname *cnp) {
    if ((cnp->cn_flags & (HASBUF | SAVESTART)) == HASBUF) {
        pool_put(&namei_pool, cnp->cn_pnbuf);
        cnp->cn_flags &= ~HASBUF;
    }
}

/* The parent of a directory; the selected root is its own parent. */
static int parent_of(struct cfs_kmount *km, cfs_bf_id_t id, cfs_bf_id_t *out) {
    cfs_bw_stat_t st;
    int error = stat_locked(km, id, &st);
    if (error == 0) {
        *out = cfs_bf_id_equal(id, km->km_root) ? id : st.inode.parent;
    }

    return error;
}

static int lookup_dotdot(struct vnode *dvp, struct vnode **vpp) {
    struct cfs_kmount *km = VTOCFSMNT(dvp);
    cfs_bf_id_t self = VTOCFS(dvp)->cnd_id, parent;

    cfs_owner_enter(km);
    int error = parent_of(km, self, &parent);
    cfs_owner_exit(km);
    if (error != 0) {
        return error;
    }

    if (cfs_bf_id_equal(parent, self)) {
        vref(dvp);
        *vpp = dvp;
        return 0;
    }

    return cfs_node_get_unlocked(dvp, parent, vpp);
}

/* Name, id and type from the current view; owners for the sticky check. */
typedef struct lookup_result {
    cfs_bf_id_t id;
    uint16_t type;
    uint16_t dir_mode;
    uint32_t dir_uid;
    uint32_t uid;
} lookup_result_t;

static int lookup_name(struct cfs_kmount *km, cfs_bf_id_t dir,
    const struct componentname *cnp, int owners, lookup_result_t *out) {
    cfs_bw_scratch_t scratch;
    cfs_bf_entry_t entry;
    cfs_bw_stat_t st;

    cfs_owner_enter(km);
    int error = cfs_view_locked(km, &scratch);
    int raw = CFS_BS_OK;
    if (error == 0) {
        raw = cfs_bf_lookup(&km->km_image.store, &km->km_image.view, dir,
            cnp->cn_nameptr, (uint16_t)cnp->cn_namelen, &entry,
            scratch.storage, scratch.ns);
        error = raw == CFS_BS_ENOENT ? ENOENT : cfs_errno(raw);
    }

    if (error == 0) {
        out->id = entry.id;
        out->type = entry.inode.type;
    }

    if (error == 0 && owners) {
        error = stat_locked(km, dir, &st);
        out->dir_mode = st.inode.mode;
        out->dir_uid = st.inode.uid;
        if (error == 0 && (st.inode.mode & S_ISTXT)) {
            error = stat_locked(km, entry.id, &st);
            out->uid = st.inode.uid;
        }
    }

    cfs_owner_exit(km);
    return error;
}

int cfs_lookup(void *v) {
    struct vop_lookup_args *ap = v;
    struct vnode *dvp = ap->a_dvp, **vpp = ap->a_vpp;
    struct componentname *cnp = ap->a_cnp;
    struct ucred *cred = cnp->cn_cred;
    struct cfs_kmount *km = VTOCFSMNT(dvp);
    const int lastcn = (cnp->cn_flags & ISLASTCN) != 0;
    const int lockparent = (cnp->cn_flags & LOCKPARENT) != 0;
    const u_long op = cnp->cn_nameiop;
    lookup_result_t found;
    int error;

    cnp->cn_flags &= ~PDIRUNLOCK;
    *vpp = NULL;

    error = VOP_ACCESS(dvp, VEXEC, cred, curproc);
    if (error != 0) {
        return error;
    }

    if (lastcn && (dvp->v_mount->mnt_flag & MNT_RDONLY) &&
        (op == DELETE || op == RENAME)) {
        return EROFS;
    }

    error = cache_lookup(dvp, vpp, cnp);
    if (error == ENOENT) {
        return ENOENT;
    } else if (error != -1) {
        return 0;
    }

    if (cnp->cn_flags & ISDOTDOT) {
        if (lastcn && op == RENAME) {
            return EINVAL;
        }

        if (lastcn && op == DELETE) {
            cnp->cn_flags |= SAVENAME;
        }

        error = lookup_dotdot(dvp, vpp);
        if (error != 0 || *vpp == dvp) {
            return error;
        }

        goto out;
    }

    if (is_dot(cnp)) {
        if (lastcn && op == RENAME) {
            return EISDIR;
        }

        vref(dvp);
        *vpp = dvp;
        error = 0;
        goto done;
    }

    if (cnp->cn_namelen > CFS_BF_NAME_MAX) {
        return ENAMETOOLONG;
    }

    const int modifying = lastcn && (op == DELETE || op == RENAME);
    error = lookup_name(km, VTOCFS(dvp)->cnd_id, cnp, modifying, &found);
    if (error == ENOENT) {
        if (lastcn && (op == CREATE || op == RENAME)) {
            error = VOP_ACCESS(dvp, VWRITE, cred, curproc);
            if (error != 0) {
                return error;
            }

            cnp->cn_flags |= SAVENAME;
            error = EJUSTRETURN;
        }

        goto done;
    }

    if (error != 0) {
        return error;
    }

    if (!lastcn && found.type != CFS_BF_DIRECTORY &&
        found.type != CFS_BF_SYMLINK) {
        return ENOTDIR;
    }

    if (modifying) {
        error = VOP_ACCESS(dvp, VWRITE, cred, curproc);
        if (error != 0) {
            return error;
        }

        if ((found.dir_mode & S_ISTXT) && cred->cr_uid != 0 &&
            cred->cr_uid != found.dir_uid && cred->cr_uid != found.uid) {
            return EPERM;
        }

        if (op == DELETE) {
            cnp->cn_flags |= SAVENAME;
        }
    }

    error = cfs_node_get(km, found.id, vpp);
    if (error != 0) {
        return error;
    }

done:
    if ((cnp->cn_flags & MAKEENTRY) && op != CREATE) {
        cache_enter(dvp, *vpp, cnp);
    }

out:
    if ((error == 0 || error == EJUSTRETURN) &&
        (*vpp != dvp || (cnp->cn_flags & ISDOTDOT)) &&
        (!lockparent || !lastcn)) {
        VOP_UNLOCK(dvp);
        cnp->cn_flags |= PDIRUNLOCK;
    }

    return error;
}

/* Remove a created name whose vnode could not be made, if it still names id. */
static void unpublish(struct cfs_kmount *km, cfs_bf_id_t parent,
    const struct componentname *cnp, uint16_t type, cfs_bf_id_t id) {
    cfs_kimage_t *image = &km->km_image;
    cfs_bw_scratch_t scratch;
    cfs_bf_entry_t entry;
    uint64_t revision;

    cfs_owner_enter(km);
    if (cfs_ns_begin(km, &scratch) == 0) {
        int raw = cfs_bf_lookup(&image->store, &image->view, parent,
            cnp->cn_nameptr, (uint16_t)cnp->cn_namelen, &entry,
            scratch.storage, scratch.ns);
        if (raw == CFS_BS_OK && cfs_bf_id_equal(entry.id, id)) {
            raw = cfs_bf_remove(&image->store, image->branch_id,
                image->branch.revision, parent, cnp->cn_nameptr,
                (uint16_t)cnp->cn_namelen,
                type == CFS_BF_DIRECTORY ? CFS_BF_DIRECTORY : CFS_BF_REGULAR,
                1, cfs_now(), &revision, scratch.storage, scratch.ns);
        }

        (void)cfs_ns_end(km, raw);
    }

    cfs_owner_exit(km);
}

/* Create one inode under dvp; *vpp is locked and referenced on success. */
static int make_node(struct vnode *dvp, struct vnode **vpp,
    struct componentname *cnp, struct vattr *vap, const char *target) {
    struct cfs_kmount *km = VTOCFSMNT(dvp);
    uint16_t type = engine_type(vap->va_type);
    cfs_bf_id_t parent = VTOCFS(dvp)->cnd_id;
    size_t target_length = target != NULL ? strlen(target) : 0;
    cfs_bw_scratch_t scratch;
    cfs_bf_created_t created;
    cfs_bw_stat_t st;
    int error;

    *vpp = NULL;
    if (type == 0 || (type == CFS_BF_SYMLINK) != (target != NULL)) {
        return EOPNOTSUPP;
    }

    if (type == CFS_BF_SYMLINK &&
        (target_length == 0 || target_length > CFS_BF_SYMLINK_MAX)) {
        return target_length == 0 ? EINVAL : ENAMETOOLONG;
    }

    if (dvp->v_mount->mnt_flag & MNT_RDONLY) {
        return EROFS;
    }

    cfs_owner_enter(km);
    error = stat_locked(km, parent, &st);
    if (error == 0) {
        error = cfs_ns_begin(km, &scratch);
    }

    if (error == 0) {
        cfs_kimage_t *image = &km->km_image;
        cfs_bf_attributes_t attributes = {.time = cfs_now(),
            .uid = cnp->cn_cred->cr_uid, .gid = st.inode.gid,
            .mode = (uint16_t)(vap->va_mode & 07777)};

        /* As ufs_makeinode: setgid needs the inherited group or root. */
        if ((attributes.mode & S_ISGID) &&
            !groupmember(attributes.gid, cnp->cn_cred) &&
            suser_ucred(cnp->cn_cred) != 0) {
            attributes.mode &= (uint16_t)~S_ISGID;
        }

        int raw;
        int retried = 0;
        for (;;) {
            if (type == CFS_BF_SYMLINK) {
                raw = cfs_bf_symlink(&image->store, image->branch_id,
                    image->branch.revision, parent, cnp->cn_nameptr,
                    (uint16_t)cnp->cn_namelen, target,
                    (uint32_t)target_length, attributes, &created,
                    scratch.storage, scratch.ns);
            } else {
                raw = cfs_bf_create(&image->store, image->branch_id,
                    image->branch.revision, parent, cnp->cn_nameptr,
                    (uint16_t)cnp->cn_namelen, type, attributes, &created,
                    scratch.storage, scratch.ns);
            }

            if (retried || !ns_space_retry(km, &scratch, &raw)) {
                break;
            }

            retried = 1;
        }

        error = raw == CFS_BS_EWORKSPACE && retried ? ENOSPC :
            cfs_ns_end(km, raw);
    }

    cfs_owner_exit(km);
    if (error == 0) {
        error = cfs_node_get(km, created.id, vpp);
        if (error != 0) {
            unpublish(km, parent, cnp, type, created.id);
        }
    }

    return error;
}

int cfs_create(void *v) {
    struct vop_create_args *ap = v;
    int error = make_node(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, NULL);
    free_name(ap->a_cnp);
    return error;
}

int cfs_mknod(void *v) {
    struct vop_mknod_args *ap = v;
    free_name(ap->a_cnp);
    return EOPNOTSUPP;
}

int cfs_mkdir(void *v) {
    struct vop_mkdir_args *ap = v;
    int error = make_node(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, NULL);
    free_name(ap->a_cnp);
    vput(ap->a_dvp);
    return error;
}

int cfs_symlink(void *v) {
    struct vop_symlink_args *ap = v;
    ap->a_vap->va_type = VLNK;
    int error = make_node(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap,
        ap->a_target);
    free_name(ap->a_cnp);
    vput(ap->a_dvp);
    if (error == 0) {
        vput(*ap->a_vpp);
    }

    return error;
}

int cfs_link(void *v) {
    struct vop_link_args *ap = v;
    struct vnode *dvp = ap->a_dvp, *vp = ap->a_vp;
    struct componentname *cnp = ap->a_cnp;
    struct cfs_kmount *km = VTOCFSMNT(dvp);
    cfs_bw_scratch_t scratch;
    int error;

    if (vp->v_type == VDIR) {
        error = EPERM;
    } else if (vp->v_mount != dvp->v_mount) {
        error = EXDEV;
    } else if (dvp->v_mount->mnt_flag & MNT_RDONLY) {
        error = EROFS;
    } else {
        vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
        cfs_owner_enter(km);
        error = cfs_ns_begin(km, &scratch);
        if (error == 0) {
            cfs_kimage_t *image = &km->km_image;
            uint64_t revision;
            int raw = cfs_bf_link(&image->store, image->branch_id,
                image->branch.revision, VTOCFS(vp)->cnd_id,
                VTOCFS(dvp)->cnd_id, cnp->cn_nameptr,
                (uint16_t)cnp->cn_namelen, cfs_now(), &revision,
                scratch.storage, scratch.ns);
            if (ns_space_retry(km, &scratch, &raw)) {
                raw = cfs_bf_link(&image->store, image->branch_id,
                    image->branch.revision, VTOCFS(vp)->cnd_id,
                    VTOCFS(dvp)->cnd_id, cnp->cn_nameptr,
                    (uint16_t)cnp->cn_namelen, cfs_now(), &revision,
                    scratch.storage, scratch.ns);
                error = cfs_ns_end(km, raw);
            } else {
                error = raw == CFS_BS_EWORKSPACE ? ENOSPC :
                    cfs_ns_end(km, raw);
            }

            if (raw == CFS_BS_EOVERFLOW) {
                error = EMLINK;
            } else if (raw == CFS_BS_EISDIR) {
                error = EPERM;
            }
        }

        cfs_owner_exit(km);
        VOP_UNLOCK(vp);
    }

    free_name(cnp);
    vput(dvp);
    return error;
}

/* One unlink or rmdir; an open target survives as an orphan. */
static int remove_name(struct vnode *dvp, struct vnode *vp,
    struct componentname *cnp, uint16_t type) {
    struct cfs_kmount *km = VTOCFSMNT(dvp);
    cfs_bw_scratch_t scratch;

    cfs_owner_enter(km);
    int error = cfs_ns_begin(km, &scratch);
    if (error == 0) {
        cfs_kimage_t *image = &km->km_image;
        uint64_t revision;
        int raw = cfs_bf_remove(&image->store, image->branch_id,
            image->branch.revision, VTOCFS(dvp)->cnd_id, cnp->cn_nameptr,
            (uint16_t)cnp->cn_namelen, type, 1, cfs_now(), &revision,
            scratch.storage, scratch.ns);
        error = cfs_ns_end(km, raw);
    }

    cfs_owner_exit(km);
    if (error == 0) {
        VTOCFS(vp)->cnd_flags |= CFS_CND_UNLINKED;
    }

    return error;
}

int cfs_remove(void *v) {
    struct vop_remove_args *ap = v;
    struct vnode *dvp = ap->a_dvp, *vp = ap->a_vp;
    int error;

    if (vp->v_type == VDIR) {
        error = EPERM;
    } else {
        error = remove_name(dvp, vp, ap->a_cnp, CFS_BF_REGULAR);
    }

    if (error == 0) {
        cache_purge(vp);
    }

    free_name(ap->a_cnp);
    return error;
}

int cfs_rmdir(void *v) {
    struct vop_rmdir_args *ap = v;
    struct vnode *dvp = ap->a_dvp, *vp = ap->a_vp;
    struct componentname *cnp = ap->a_cnp;
    int error;

    if (is_dotdot(cnp)) {
        error = ENOTEMPTY;
    } else if (is_dot(cnp) || dvp == vp) {
        error = EINVAL;
    } else if ((vp->v_flag & VROOT) || vp->v_mountedhere != NULL) {
        error = EBUSY;
    } else if (vp->v_type != VDIR) {
        error = ENOTDIR;
    } else {
        error = remove_name(dvp, vp, cnp, CFS_BF_DIRECTORY);
    }

    if (error == 0) {
        cache_purge(dvp);
        cache_purge(vp);
    }

    free_name(cnp);
    if (dvp == vp) {
        vrele(dvp);
    } else {
        vput(dvp);
    }

    vput(vp);
    return error;
}

static void rename_release(struct vop_rename_args *ap) {
    if (ap->a_tdvp == ap->a_tvp) {
        vrele(ap->a_tdvp);
    } else {
        vput(ap->a_tdvp);
    }

    if (ap->a_tvp) {
        vput(ap->a_tvp);
    }

    vrele(ap->a_fdvp);
    vrele(ap->a_fvp);
}

static int rename_abort(struct vop_rename_args *ap, int error) {
    VOP_ABORTOP(ap->a_tdvp, ap->a_tcnp);
    VOP_ABORTOP(ap->a_fdvp, ap->a_fcnp);
    rename_release(ap);
    return error;
}

/* Both names still resolve as looked up; a moved directory needs write. */
static int rename_check_locked(struct cfs_kmount *km,
    struct vop_rename_args *ap) {
    cfs_bf_id_t fid = VTOCFS(ap->a_fvp)->cnd_id;
    cfs_bw_scratch_t scratch;
    cfs_bf_entry_t entry;
    cfs_bw_stat_t st;

    int error = cfs_view_locked(km, &scratch);
    if (error == 0) {
        int raw = cfs_bf_lookup(&km->km_image.store, &km->km_image.view,
            VTOCFS(ap->a_fdvp)->cnd_id, ap->a_fcnp->cn_nameptr,
            (uint16_t)ap->a_fcnp->cn_namelen, &entry, scratch.storage,
            scratch.ns);
        error = cfs_errno(raw);
        if (error == 0 && !cfs_bf_id_equal(entry.id, fid)) {
            error = ENOENT;
        }
    }

    if (error == 0) {
        int raw = cfs_bf_lookup(&km->km_image.store, &km->km_image.view,
            VTOCFS(ap->a_tdvp)->cnd_id, ap->a_tcnp->cn_nameptr,
            (uint16_t)ap->a_tcnp->cn_namelen, &entry, scratch.storage,
            scratch.ns);
        if (raw != CFS_BS_OK && raw != CFS_BS_ENOENT) {
            error = cfs_errno(raw);
        } else if (ap->a_tvp == NULL ? raw == CFS_BS_OK : raw != CFS_BS_OK ||
            !cfs_bf_id_equal(entry.id, VTOCFS(ap->a_tvp)->cnd_id)) {
            error = ENOENT;
        }
    }

    if (error == 0 && ap->a_fvp->v_type == VDIR &&
        ap->a_fdvp != ap->a_tdvp) {
        error = stat_locked(km, fid, &st);
        if (error == 0) {
            error = vaccess(VDIR, st.inode.mode & 01777u, st.inode.uid,
                st.inode.gid, VWRITE, ap->a_fcnp->cn_cred);
        }
    }

    return error;
}

int cfs_rename(void *v) {
    struct vop_rename_args *ap = v;
    struct vnode *fdvp = ap->a_fdvp, *fvp = ap->a_fvp;
    struct vnode *tdvp = ap->a_tdvp, *tvp = ap->a_tvp;
    struct componentname *fcnp = ap->a_fcnp, *tcnp = ap->a_tcnp;
    struct cfs_kmount *km = VTOCFSMNT(tdvp);
    cfs_bw_scratch_t scratch;

    if (fvp->v_mount != tdvp->v_mount || fdvp->v_mount != fvp->v_mount ||
        (tvp != NULL && tvp->v_mount != fvp->v_mount)) {
        return rename_abort(ap, EXDEV);
    }

    if (fvp == tvp) {
        return rename_abort(ap, 0);
    }

    if (is_dot(fcnp) || is_dotdot(fcnp) || fdvp == fvp || tdvp == fvp ||
        (fcnp->cn_flags & ISDOTDOT) || (tcnp->cn_flags & ISDOTDOT)) {
        return rename_abort(ap, EINVAL);
    }

    if (tvp != NULL && (tvp->v_flag & VROOT || tvp->v_mountedhere != NULL)) {
        return rename_abort(ap, EBUSY);
    }

    if (tdvp->v_mount->mnt_flag & MNT_RDONLY) {
        return rename_abort(ap, EROFS);
    }

    cfs_owner_enter(km);
    int error = rename_check_locked(km, ap);
    if (error == 0) {
        error = cfs_ns_begin(km, &scratch);
    }

    if (error == 0) {
        cfs_kimage_t *image = &km->km_image;
        uint64_t revision;
        int raw = cfs_bf_rename(&image->store, image->branch_id,
            image->branch.revision, VTOCFS(fdvp)->cnd_id, fcnp->cn_nameptr,
            (uint16_t)fcnp->cn_namelen, VTOCFS(tdvp)->cnd_id,
            tcnp->cn_nameptr, (uint16_t)tcnp->cn_namelen, tvp != NULL,
            cfs_now(), &revision, scratch.storage, scratch.ns);
        if (ns_space_retry(km, &scratch, &raw)) {
            raw = cfs_bf_rename(&image->store, image->branch_id,
                image->branch.revision, VTOCFS(fdvp)->cnd_id,
                fcnp->cn_nameptr, (uint16_t)fcnp->cn_namelen,
                VTOCFS(tdvp)->cnd_id, tcnp->cn_nameptr,
                (uint16_t)tcnp->cn_namelen, tvp != NULL, cfs_now(),
                &revision, scratch.storage, scratch.ns);
            error = cfs_ns_end(km, raw);
        } else {
            error = raw == CFS_BS_EWORKSPACE ? ENOSPC : cfs_ns_end(km, raw);
        }
    }

    cfs_owner_exit(km);
    if (error == 0) {
        cache_purge(fdvp);
        if (tdvp != fdvp) {
            cache_purge(tdvp);
        }

        cache_purge(fvp);
        if (tvp) {
            VTOCFS(tvp)->cnd_flags |= CFS_CND_UNLINKED;
            cache_purge(tvp);
        }
    }

    rename_release(ap);
    return error;
}

int cfs_readlink(void *v) {
    struct vop_readlink_args *ap = v;
    struct vnode *vp = ap->a_vp;
    struct cfs_kmount *km = VTOCFSMNT(vp);
    cfs_bw_scratch_t scratch;
    uint32_t length = 0;

    if (vp->v_type != VLNK) {
        return EINVAL;
    }

    char *target = malloc(CFS_BF_SYMLINK_MAX, M_TEMP, M_WAITOK);
    cfs_owner_enter(km);
    int error = cfs_view_locked(km, &scratch);
    if (error == 0) {
        error = cfs_errno(cfs_bf_readlink(&km->km_image.store,
            &km->km_image.view, VTOCFS(vp)->cnd_id, target,
            CFS_BF_SYMLINK_MAX, &length, scratch.storage, scratch.ns,
            km->km_symlink));
    }

    cfs_owner_exit(km);
    if (error == 0) {
        error = uiomove(target, length, ap->a_uio);
    }

    free(target, M_TEMP, CFS_BF_SYMLINK_MAX);
    return error;
}

/* Readdir runs in cookie order: birth over a name hash, never moved by
 * other names; names sharing a cookie go out in one call. */
typedef struct dir_name {
    off_t cookie;
    ino_t ino;
    uint16_t length;
    char bytes[CFS_BF_NAME_MAX + 1];
} dir_name_t;

typedef struct dir_scan {
    struct cfs_kmount *km;
    dir_name_t *names;
    uint32_t count;
    off_t after;
    off_t limit; /* Smallest cookie dropped for space; 0 if none. */
    int error;
} dir_scan_t;

static uint32_t name_hash(const void *name, uint16_t length) {
    const uint8_t *bytes = name;
    uint32_t hash = 2166136261u;
    for (uint16_t i = 0; i < length; i++) {
        hash = (hash ^ bytes[i]) * 16777619u;
    }

    return (hash ^ (hash >> 16)) & ((1u << CFS_COOKIE_TIE_BITS) - 1u);
}

static int name_before(const dir_name_t *a, off_t cookie, const void *name,
    uint16_t length) {
    if (a->cookie != cookie) {
        return a->cookie < cookie;
    }

    int order = memcmp(a->bytes, name, MIN(a->length, length));
    return order < 0 || (order == 0 && a->length < length);
}

static void drop_cookie(dir_scan_t *s, off_t cookie) {
    if (s->limit == 0 || cookie < s->limit) {
        s->limit = cookie;
    }
}

static int collect(void *context, const void *name, uint16_t length,
    cfs_bf_id_t id) {
    dir_scan_t *s = context;
    ino_t ino;

    if (id.birth >= (1ull << (63 - CFS_COOKIE_TIE_BITS))) {
        s->error = EOVERFLOW;
        return CFS_BT_SCAN_STOP;
    }

    s->error = cfs_ino(s->km, id, &ino);
    if (s->error != 0) {
        return CFS_BT_SCAN_STOP;
    }

    off_t cookie = (off_t)(id.birth << CFS_COOKIE_TIE_BITS |
        name_hash(name, length));
    if (cookie <= s->after) {
        return CFS_BT_SCAN_CONTINUE;
    }

    if (s->limit != 0 && cookie >= s->limit) {
        return CFS_BT_SCAN_CONTINUE;
    }

    uint32_t at = s->count;
    while (at > 0 && !name_before(&s->names[at - 1], cookie, name, length)) {
        at--;
    }

    if (s->count == CFS_READDIR_ENTRIES) {
        if (at == s->count) {
            drop_cookie(s, cookie);
            return CFS_BT_SCAN_CONTINUE;
        }

        drop_cookie(s, s->names[--s->count].cookie);
    }

    memmove(&s->names[at + 1], &s->names[at],
        (s->count - at) * sizeof(*s->names));
    dir_name_t *slot = &s->names[at];
    slot->cookie = cookie;
    slot->ino = ino;
    slot->length = length;
    memcpy(slot->bytes, name, length);
    slot->bytes[length] = '\0';
    s->count++;
    return CFS_BT_SCAN_CONTINUE;
}

static int emit(struct uio *uio, ino_t ino, off_t cookie, uint8_t type,
    const char *name, uint16_t length) {
    struct dirent d;

    memset(&d, 0, sizeof(d));
    d.d_fileno = ino;
    d.d_off = cookie;
    d.d_reclen = DIRENT_RECSIZE(length);
    d.d_type = type;
    d.d_namlen = (uint8_t)length;
    memcpy(d.d_name, name, length);
    return uiomove(&d, d.d_reclen, uio);
}

int cfs_readdir(void *v) {
    struct vop_readdir_args *ap = v;
    struct vnode *vp = ap->a_vp;
    struct uio *uio = ap->a_uio;
    struct cfs_kmount *km = VTOCFSMNT(vp);
    struct cfs_knode *node = VTOCFS(vp);
    cfs_bw_scratch_t scratch;
    cfs_bt_scan_result_t result;
    cfs_bf_id_t parent;
    ino_t parent_ino = 0;
    int error;

    if (vp->v_type != VDIR) {
        return ENOTDIR;
    }

    if (uio->uio_offset < 0) {
        return EINVAL;
    }

    size_t bytes = CFS_READDIR_ENTRIES * sizeof(dir_name_t);
    dir_scan_t scan = {.km = km, .after = MAX(uio->uio_offset,
        CFS_COOKIE_DOTDOT)};
    scan.names = malloc(bytes, M_TEMP, M_WAITOK);

    cfs_owner_enter(km);
    error = parent_of(km, node->cnd_id, &parent);
    if (error == 0) {
        error = cfs_ino(km, parent, &parent_ino);
    }

    if (error == 0) {
        error = cfs_view_locked(km, &scratch);
    }

    if (error == 0) {
        int raw = cfs_bf_list(&km->km_image.store, &km->km_image.view,
            node->cnd_id, collect, &scan, &result, scratch.storage,
            scratch.ns);
        if (raw == CFS_BS_ENOENT && (node->cnd_flags & CFS_CND_UNLINKED)) {
            raw = CFS_BS_OK;
        }

        error = cfs_errno(raw);
    }

    if (error == 0) {
        error = scan.error;
    }

    cfs_owner_exit(km);

    /* Names at or past the first dropped cookie wait for the next call. */
    uint32_t count = scan.count;
    while (count > 0 && scan.limit != 0 &&
        scan.names[count - 1].cookie >= scan.limit) {
        count--;
    }

    if (error == 0 && count == 0 && scan.limit != 0) {
        error = EOVERFLOW;
    }

    off_t offset = uio->uio_offset;
    int emitted = 0;
    if (error == 0 && offset < CFS_COOKIE_DOT &&
        DIRENT_RECSIZE(1) <= uio->uio_resid) {
        error = emit(uio, node->cnd_ino, CFS_COOKIE_DOT, DT_DIR, ".", 1);
        offset = CFS_COOKIE_DOT;
        emitted++;
    }

    if (error == 0 && offset == CFS_COOKIE_DOT &&
        DIRENT_RECSIZE(2) <= uio->uio_resid) {
        error = emit(uio, parent_ino, CFS_COOKIE_DOTDOT, DT_DIR, "..", 2);
        offset = CFS_COOKIE_DOTDOT;
        emitted++;
    }

    uint32_t next = 0;
    while (error == 0 && offset >= CFS_COOKIE_DOTDOT && next < count) {
        uint32_t end = next;
        size_t group = 0;
        while (end < count && scan.names[end].cookie == scan.names[next].cookie) {
            group += DIRENT_RECSIZE(scan.names[end].length);
            end++;
        }

        if (group > uio->uio_resid) {
            break;
        }

        for (; error == 0 && next < end; next++) {
            dir_name_t *n = &scan.names[next];
            error = emit(uio, n->ino, n->cookie, DT_UNKNOWN, n->bytes,
                n->length);
            emitted++;
        }

        offset = scan.names[end - 1].cookie;
    }

    if (error == 0 && emitted == 0 &&
        (offset < CFS_COOKIE_DOTDOT || next < count)) {
        error = EINVAL;
    }

    if (error == 0) {
        uio->uio_offset = offset;
        if (ap->a_eofflag) {
            *ap->a_eofflag = offset >= CFS_COOKIE_DOTDOT && next == count &&
                scan.limit == 0;
        }
    }

    free(scan.names, M_TEMP, bytes);
    return error;
}
