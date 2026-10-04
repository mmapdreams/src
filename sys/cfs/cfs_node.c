/* SPDX-License-Identifier: ISC
 *
 * cfs_node.c -- OpenBSD cfs node table, vnode lifetime, owner-lock helpers
 * and the scheduled writeback and reclamation service.
 */
#include "cfs_kern_internal.h"

#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/lockf.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/proc.h>

struct pool cfs_knode_pool;
static struct taskq *cfs_service_tq;

static struct cfs_knode_list *node_bucket(struct cfs_kmount *km,
    cfs_bf_id_t id) {
    uint64_t h = id.birth * 0x9e3779b97f4a7c15ULL ^ id.ordinal;
    return &km->km_node_hash[(h >> 20) & km->km_node_mask];
}

static struct cfs_knode *table_find(struct cfs_kmount *km, cfs_bf_id_t id) {
    struct cfs_knode *node;

    MUTEX_ASSERT_LOCKED(&km->km_node_mtx);
    LIST_FOREACH(node, node_bucket(km, id), cnd_hash) {
        if (cfs_bf_id_equal(node->cnd_id, id)) {
            return node;
        }
    }

    return NULL;
}

void cfs_node_table_init(struct cfs_kmount *km) {
    mtx_init(&km->km_node_mtx, IPL_NONE);
    km->km_node_hash = hashinit(CFS_KNODE_HASH_BUCKETS, M_MISCFSMNT,
        M_WAITOK, &km->km_node_mask);
}

void cfs_node_table_destroy(struct cfs_kmount *km) {
    u_long i;

    if (!km->km_node_hash) {
        return;
    }

    for (i = 0; i <= km->km_node_mask; i++) {
        if (!LIST_EMPTY(&km->km_node_hash[i])) {
            panic("cfs: node table not empty at unmount");
        }
    }

    hashfree(km->km_node_hash, CFS_KNODE_HASH_BUCKETS, M_MISCFSMNT);
    km->km_node_hash = NULL;
}

/* Owner lock held. Stat and admit one consumer hold for id. */
static int admit(struct cfs_kmount *km, cfs_bf_id_t id, uint16_t *type,
    uint64_t *hold) {
    cfs_kimage_t *image = &km->km_image;
    cfs_bw_stat_t stat;
    uint32_t consumer;

    if (km->km_unmounting) {
        return EIO;
    }

    /* A miss on an inode with no name has no consumer either: stale. */
    int status = cfs_bw_stat(&image->working, image->branch_id, id, &stat);
    if (status == CFS_BS_OK && stat.inode.links == 0u) {
        status = CFS_BS_ENOENT;
    }

    if (status == CFS_BS_OK) {
        status = cfs_bcons_admit(&image->consumers, id,
            image->consumer_sequence, &consumer);
        if (status == CFS_BS_EMFILE) {
            status = CFS_BS_ENFILE;
        }
    }

    if (status != CFS_BS_OK) {
        return cfs_errno(status);
    }

    *type = stat.inode.type;
    *hold = cfs_bcons_attach(&image->consumers, &image->consumer_sequence,
        consumer, id, stat.inode.type);
    return 0;
}

/* Owner lock held. The hold is released exactly once. */
static void release_hold(struct cfs_kmount *km, struct cfs_knode *node) {
    if (node->cnd_hold == 0u) {
        return;
    }

    if (cfs_bcons_release(&km->km_image.consumers, node->cnd_hold) !=
        CFS_BS_OK) {
        panic("cfs: stale consumer hold %llx", node->cnd_hold);
    }

    node->cnd_hold = 0u;
    if (node->cnd_flags & CFS_CND_UNLINKED) {
        cfs_service_touch(km, CFS_SVC_REAP);
    }
}

static enum vtype vnode_type(uint16_t type) {
    switch (type) {
    case CFS_BF_REGULAR:
        return VREG;
    case CFS_BF_DIRECTORY:
        return VDIR;
    case CFS_BF_SYMLINK:
        return VLNK;
    default:
        return VBAD;
    }
}

/* Holds live as long as vnodes; a full table frees the unreferenced ones. */
static int recycle_unused(struct vnode *vp, void *arg) {
    (void)arg;
    (void)vrecycle(vp, curproc);
    return 0;
}

int cfs_node_get(struct cfs_kmount *km, cfs_bf_id_t id, struct vnode **vpp) {
    struct cfs_knode *node;
    struct vnode *vp;
    uint64_t hold;
    uint16_t type;
    ino_t ino;
    int error;

    error = cfs_ino(km, id, &ino);
    if (error != 0) {
        return error;
    }

loop:
    mtx_enter(&km->km_node_mtx);
    node = table_find(km, id);
    if (node) {
        vp = node->cnd_vnode;
        mtx_leave(&km->km_node_mtx);
        error = vget(vp, LK_EXCLUSIVE);
        if (error == ENOENT) {
            goto loop;
        }

        if (error != 0) {
            return error;
        }

        if (VTOCFS(vp) != node || !cfs_bf_id_equal(node->cnd_id, id)) {
            vput(vp);
            goto loop;
        }

        *vpp = vp;
        return 0;
    }

    mtx_leave(&km->km_node_mtx);

    cfs_owner_enter(km);
    error = admit(km, id, &type, &hold);
    cfs_owner_exit(km);
    if (error == ENFILE) {
        (void)vfs_mount_foreach_vnode(km->km_mp, recycle_unused, NULL);
        cfs_owner_enter(km);
        error = admit(km, id, &type, &hold);
        cfs_owner_exit(km);
    }

    if (error != 0) {
        return error;
    }

    /* No sleep between getnewvnode and v_data: vflush may reclaim it. */
    node = pool_get(&cfs_knode_pool, PR_WAITOK | PR_ZERO);
    if (vnode_type(type) == VBAD) {
        error = EIO;
    } else {
        error = getnewvnode(VT_CFS, km->km_mp, &cfs_vops, &vp);
    }

    if (error != 0) {
        pool_put(&cfs_knode_pool, node);
        cfs_owner_enter(km);
        if (cfs_bcons_release(&km->km_image.consumers, hold) != CFS_BS_OK) {
            panic("cfs: stale consumer hold %llx", hold);
        }

        cfs_owner_exit(km);
        return error;
    }

    rrw_init_flags(&node->cnd_lock, "cfsnode", RWL_DUPOK | RWL_IS_VNODE);
    node->cnd_vnode = vp;
    node->cnd_km = km;
    node->cnd_id = id;
    node->cnd_hold = hold;
    node->cnd_ino = ino;
    node->cnd_type = type;
    vp->v_data = node;
    vp->v_type = vnode_type(type);
    if (ino == 1) {
        vp->v_flag |= VROOT;
    }

    rrw_enter(&node->cnd_lock, RW_WRITE);
    mtx_enter(&km->km_node_mtx);
    if (table_find(km, id)) {
        mtx_leave(&km->km_node_mtx);
        cfs_owner_enter(km);
        release_hold(km, node);
        cfs_owner_exit(km);
        vp->v_type = VNON;
        vput(vp);
        goto loop;
    }

    LIST_INSERT_HEAD(node_bucket(km, id), node, cnd_hash);
    node->cnd_flags |= CFS_CND_HASHED;
    mtx_leave(&km->km_node_mtx);
    *vpp = vp;
    return 0;
}

int cfs_node_get_unlocked(struct vnode *dvp, cfs_bf_id_t id,
    struct vnode **vpp) {
    struct cfs_kmount *km = VTOCFSMNT(dvp);

    mtx_enter(&km->km_node_mtx);
    km->km_node_busy++;
    mtx_leave(&km->km_node_mtx);
    VOP_UNLOCK(dvp);
    int error = cfs_node_get(km, id, vpp);
    vn_lock(dvp, LK_EXCLUSIVE | LK_RETRY);
    mtx_enter(&km->km_node_mtx);
    km->km_node_busy--;
    mtx_leave(&km->km_node_mtx);
    return error;
}

int cfs_inactive(void *v) {
    struct vop_inactive_args *ap = v;
    struct vnode *vp = ap->a_vp;
    struct cfs_knode *node = VTOCFS(vp);
    struct cfs_kmount *km = node->cnd_km;
    cfs_bw_stat_t stat;
    int unlinked = 0;

    if (node->cnd_flags & CFS_CND_UNLINKED) {
        cfs_owner_enter(km);
        int status = cfs_bw_stat(&km->km_image.working,
            km->km_image.branch_id, node->cnd_id, &stat);
        unlinked = status != CFS_BS_OK || stat.inode.links == 0u;
        cfs_owner_exit(km);
    }

    if (!(node->cnd_flags & CFS_CND_HASHED)) {
        unlinked = 1;
    }

    VOP_UNLOCK(vp);
    if (unlinked) {
        vrecycle(vp, ap->a_p);
    }

    return 0;
}

int cfs_reclaim(void *v) {
    struct vop_reclaim_args *ap = v;
    struct vnode *vp = ap->a_vp;
    struct cfs_knode *node = VTOCFS(vp);
    struct cfs_kmount *km = node->cnd_km;

    if (node->cnd_flags & CFS_CND_HASHED) {
        mtx_enter(&km->km_node_mtx);
        LIST_REMOVE(node, cnd_hash);
        mtx_leave(&km->km_node_mtx);
    }

    cfs_owner_enter(km);
    release_hold(km, node);
    cfs_owner_exit(km);

    cache_purge(vp);
    lf_purgelocks(&node->cnd_lockf);
    vp->v_data = NULL;
    pool_put(&cfs_knode_pool, node);
    return 0;
}

int cfs_lock(void *v) {
    struct vop_lock_args *ap = v;

    return rrw_enter(&VTOCFS(ap->a_vp)->cnd_lock, ap->a_flags & LK_RWFLAGS);
}

int cfs_unlock(void *v) {
    struct vop_unlock_args *ap = v;

    rrw_exit(&VTOCFS(ap->a_vp)->cnd_lock);
    return 0;
}

int cfs_islocked(void *v) {
    struct vop_islocked_args *ap = v;

    return rrw_status(&VTOCFS(ap->a_vp)->cnd_lock);
}

int cfs_print(void *v) {
    struct vop_print_args *ap = v;
    struct cfs_knode *node = VTOCFS(ap->a_vp);

    printf("tag VT_CFS, id %llu.%llu, ino %llu, type %u, hold %llx, "
        "flags 0x%x\n", node->cnd_id.birth, node->cnd_id.ordinal,
        (unsigned long long)node->cnd_ino, node->cnd_type, node->cnd_hold,
        node->cnd_flags);
    return 0;
}

int cfs_view_locked(struct cfs_kmount *km, cfs_bw_scratch_t *scratch) {
    cfs_kimage_t *image = &km->km_image;

    cfs_owner_assert(km);
    int error = cfs_kimage_head(image);
    if (error == 0) {
        error = cfs_errno(cfs_bw_borrow(&image->working, scratch));
    }

    return error;
}

int cfs_ns_begin(struct cfs_kmount *km, cfs_bw_scratch_t *scratch) {
    cfs_kimage_t *image = &km->km_image;

    cfs_owner_assert(km);
    if (km->km_read_only) {
        return EROFS;
    }

    int status = cfs_kimage_mutation_begin(image);
    if (status != CFS_BS_OK) {
        return cfs_errno(status);
    }

    int error = cfs_kimage_head(image);
    if (error == 0) {
        status = cfs_bw_borrow(&image->working, scratch);
        error = cfs_errno(status);
    } else {
        status = image->store.poisoned ? CFS_BS_EIO : CFS_BS_EINVAL;
    }

    if (error != 0) {
        cfs_kimage_mutation_end(image, status);
    }

    return error;
}

int cfs_ns_end(struct cfs_kmount *km, int status) {
    cfs_kimage_t *image = &km->km_image;

    cfs_owner_assert(km);
    cfs_kimage_mutation_end(image, status);
    int error = cfs_kimage_space_errno(status);
    int head = cfs_kimage_head(image);
    if (error == 0) {
        error = head;
    }

    cfs_service_touch(km, CFS_SVC_WRITEBACK | CFS_SVC_REAP);
    return error;
}

static uint64_t uptime_msec(void) {
    return getnsecuptime() / 1000000ULL;
}

static void service_timeout(void *arg) {
    struct cfs_kmount *km = arg;

    task_add(cfs_service_tq, &km->km_svc_task);
}

/* One pass: writeback when quiet or old, then one bounded reap step. */
static void service_task(void *arg) {
    struct cfs_kmount *km = arg;
    cfs_kimage_t *image = &km->km_image;
    cfs_bw_status_t state;
    int more = 0;
    int error = 0;

    cfs_owner_enter(km);
    km->km_svc_pending = 0;
    if (km->km_svc_stopping || km->km_svc_error != 0 ||
        cfs_bw_status(&image->working, &state) != CFS_BS_OK) {
        cfs_owner_exit(km);
        return;
    }

    uint64_t now = uptime_msec();
    if (state.dirty != 0u) {
        if (km->km_svc_first_dirty == 0u) {
            km->km_svc_first_dirty = now;
        }

        int quiet = state.accepted_sequence == km->km_svc_seen;
        int old = now - km->km_svc_first_dirty >= CFS_SERVICE_AGE_MSEC;
        if (quiet || old || state.needs_checkpoint != 0u) {
            error = cfs_kimage_checkpoint(image, state.accepted_sequence);
            km->km_svc_first_dirty = 0u;
        } else {
            more = 1;
        }
    } else {
        km->km_svc_first_dirty = 0u;
    }

    km->km_svc_seen = state.accepted_sequence;
    if (error == 0) {
        km->km_svc_retry = 0;
    }

    if (error == 0 && !more && !km->km_read_only) {
        error = cfs_kimage_reap_step(image, CFS_BS_REAP_MAX_OBJECTS, &more);
        if (error == ENOSPC || error == ENOMEM) {
            error = 0;
            more = 0;
        }
    }

    if (error == 0 && cfs_bw_status(&image->working, &state) == CFS_BS_OK &&
        state.dirty != 0u) {
        more = 1;
    }

    if (error != 0 && (image->store.poisoned ||
        (cfs_bw_status(&image->working, &state) == CFS_BS_OK &&
        state.delayed_error != CFS_BS_OK))) {
        km->km_svc_error = error;
        printf("cfs: %s: service stopped: error %d\n",
            km->km_mp->mnt_stat.f_mntonname, error);
    } else if (error != 0) {
        if (error != km->km_svc_retry) {
            printf("cfs: %s: service retrying: error %d\n",
                km->km_mp->mnt_stat.f_mntonname, error);
        }

        km->km_svc_retry = error;
        km->km_svc_pending = 1;
        timeout_add_msec(&km->km_svc_timeout, CFS_SERVICE_AGE_MSEC);
    } else if (more) {
        cfs_service_touch(km, CFS_SVC_WRITEBACK);
    }

    cfs_owner_exit(km);
}

void cfs_service_create(void) {
    cfs_service_tq = taskq_create("cfssvc", 1, IPL_NONE, 0);
}

void cfs_service_init(struct cfs_kmount *km) {
    timeout_set(&km->km_svc_timeout, service_timeout, km);
    task_set(&km->km_svc_task, service_task, km);
}

void cfs_service_touch(struct cfs_kmount *km, int work) {
    cfs_owner_assert(km);
    if (work == 0 || km->km_svc_stopping || km->km_svc_error != 0 ||
        km->km_svc_pending) {
        return;
    }

    km->km_svc_pending = 1;
    timeout_add_msec(&km->km_svc_timeout, CFS_SERVICE_TICK_MSEC);
}

void cfs_service_stop(struct cfs_kmount *km) {
    cfs_owner_enter(km);
    km->km_svc_stopping = 1;
    cfs_owner_exit(km);
    timeout_del_barrier(&km->km_svc_timeout);
    taskq_del_barrier(cfs_service_tq, &km->km_svc_task);
    km->km_svc_pending = 0;
}
