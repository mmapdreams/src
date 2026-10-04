/* SPDX-License-Identifier: ISC
 *
 * cfs_kern_internal.h -- the OpenBSD kernel adapter's internal contract:
 * mount and node state, the owner lock, status mapping and every cross-file
 * prototype. txt/openbsd_kernel.md owns the rules this header encodes.
 */
#ifndef CFS_OPENBSD_KERN_INTERNAL_H
#define CFS_OPENBSD_KERN_INTERNAL_H

#ifndef _KERNEL
#error "cfs_kern_internal.h requires the OpenBSD kernel compile environment"
#endif

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/atomic.h>
#include <sys/errno.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/pool.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/task.h>
#include <sys/time.h>
#include <sys/timeout.h>
#include <sys/vnode.h>

#include <cfs/cfs_mount.h>

#include "branch_consumers.h"
#include "branch_symlink.h"
#include "branch_working.h"
#include "cfs_kimage.h"

#define CFS_KNODE_HASH_BUCKETS 1024u
#define CFS_SERVICE_TICK_MSEC 250u
#define CFS_SERVICE_AGE_MSEC 1000u

/* Service work kinds for cfs_service_touch. */
#define CFS_SVC_WRITEBACK 0x1
#define CFS_SVC_REAP 0x2

/* cnd_flags: a name was removed while the vnode lived; node is in the table. */
#define CFS_CND_UNLINKED 0x1
#define CFS_CND_HASHED 0x2

struct cfs_knode;
LIST_HEAD(cfs_knode_list, cfs_knode);

/* km_image (cfs_kimage.h) owns the device, store, working owner, consumer
 * table and branch head; the adapter touches it only under the owner lock. */
struct cfs_kmount {
    struct mount *km_mp;
    struct rwlock km_lock;
    u_int km_owners; /* Holding or waiting for km_lock; unmount drains it. */
    cfs_kimage_t km_image;
    cfs_bf_id_t km_root;
    cfs_bf_symlink_workspace_t *km_symlink;
    int km_read_only;
    int km_unmounting; /* Owner lock; admits no new consumer hold. */

    struct mutex km_node_mtx;
    struct cfs_knode_list *km_node_hash;
    u_long km_node_mask;
    u_int km_node_busy; /* Node mutex; cfs_node_get_unlocked in flight. */

    struct timeout km_svc_timeout;
    struct task km_svc_task;
    uint64_t km_svc_seen;
    uint64_t km_svc_first_dirty;
    int km_svc_pending;
    int km_svc_stopping;
    int km_svc_error;
    int km_svc_retry;
};

struct cfs_knode {
    struct rrwlock cnd_lock;
    struct vnode *cnd_vnode;
    struct cfs_kmount *cnd_km;
    LIST_ENTRY(cfs_knode) cnd_hash;
    cfs_bf_id_t cnd_id;
    uint64_t cnd_hold;
    ino_t cnd_ino;
    struct lockf_state *cnd_lockf;
    uint16_t cnd_type;
    uint32_t cnd_flags;
};

#define VFSTOCFS(mp) ((struct cfs_kmount *)(mp)->mnt_data)
#define VTOCFS(vp) ((struct cfs_knode *)(vp)->v_data)
#define VTOCFSMNT(vp) VFSTOCFS((vp)->v_mount)

extern const struct vfsops cfs_vfsops;
extern const struct vops cfs_vops;
extern struct pool cfs_knode_pool;

/* Owner lock: sleepable, exclusive, inner to every vnode lock. Unlocked
 * vnode ops (fstat) may wait on it across a forced unmount's vflush. */
static inline void cfs_owner_enter(struct cfs_kmount *km) {
    atomic_inc_int(&km->km_owners);
    rw_enter_write(&km->km_lock);
}

static inline int cfs_owner_try(struct cfs_kmount *km) {
    atomic_inc_int(&km->km_owners);
    if (rw_enter(&km->km_lock, RW_WRITE | RW_NOSLEEP) != 0) {
        atomic_dec_int(&km->km_owners);
        return 0;
    }

    return 1;
}

static inline void cfs_owner_exit(struct cfs_kmount *km) {
    rw_exit_write(&km->km_lock);
    atomic_dec_int(&km->km_owners);
}

static inline void cfs_owner_assert(struct cfs_kmount *km) {
    rw_assert_wrlock(&km->km_lock);
}

/* Positive errno for a CFS_BS_* status; src/error_catalog/branch_store.def. */
static inline int cfs_errno(int status) {
    return cfs_kimage_errno(status);
}

static inline cfs_bf_time_t cfs_now(void) {
    struct timespec now;
    getnanotime(&now);
    return (cfs_bf_time_t){(int64_t)now.tv_sec, (uint32_t)now.tv_nsec};
}

/* Root is 1; any other inode is its birth. Ids that cannot map refuse. */
static inline int cfs_ino(const struct cfs_kmount *km, cfs_bf_id_t id,
    ino_t *out) {
    if (id.ordinal != 0u || id.birth == 0u || id.birth > INT64_MAX) {
        return EOVERFLOW;
    }

    if (cfs_bf_id_equal(id, km->km_root)) {
        *out = 1;
    } else if (id.birth == 1u) {
        return EOVERFLOW;
    } else {
        *out = (ino_t)id.birth;
    }

    return 0;
}

static inline int cfs_ino_id(const struct cfs_kmount *km, ino_t ino,
    cfs_bf_id_t *out) {
    if (ino == 0 || (uint64_t)ino > INT64_MAX) {
        return ESTALE;
    }

    *out = ino == 1 ? km->km_root : (cfs_bf_id_t){(uint64_t)ino, 0u};
    return ino != 1 && cfs_bf_id_equal(*out, km->km_root) ? ESTALE : 0;
}

/* cfs_node.c: node table and vnode lifetime. */
void cfs_node_table_init(struct cfs_kmount *km);
void cfs_node_table_destroy(struct cfs_kmount *km);
int cfs_node_get(struct cfs_kmount *km, cfs_bf_id_t id, struct vnode **vpp);
int cfs_node_get_unlocked(struct vnode *dvp, cfs_bf_id_t id,
    struct vnode **vpp);
int cfs_inactive(void *v);
int cfs_reclaim(void *v);
int cfs_lock(void *v);
int cfs_unlock(void *v);
int cfs_islocked(void *v);
int cfs_print(void *v);

/* cfs_node.c, owner lock held, errno results. The view is km_image.view and
 * the scratch is valid until the next owner call; cfs_ns_end takes raw. */
int cfs_view_locked(struct cfs_kmount *km, cfs_bw_scratch_t *scratch);
int cfs_ns_begin(struct cfs_kmount *km, cfs_bw_scratch_t *scratch);
int cfs_ns_end(struct cfs_kmount *km, int status);

/* cfs_node.c: scheduled writeback and reclamation on a private taskq. */
void cfs_service_create(void);
void cfs_service_init(struct cfs_kmount *km);
void cfs_service_touch(struct cfs_kmount *km, int work);
void cfs_service_stop(struct cfs_kmount *km);

/* cfs_vnops_namespace.c */
int cfs_lookup(void *v);
int cfs_create(void *v);
int cfs_mknod(void *v);
int cfs_mkdir(void *v);
int cfs_remove(void *v);
int cfs_rmdir(void *v);
int cfs_link(void *v);
int cfs_symlink(void *v);
int cfs_readlink(void *v);
int cfs_rename(void *v);
int cfs_readdir(void *v);

/* cfs_vnops_file.c */
int cfs_open(void *v);
int cfs_close(void *v);
int cfs_access(void *v);
int cfs_getattr(void *v);
int cfs_setattr(void *v);
int cfs_read(void *v);
int cfs_write(void *v);
int cfs_fsync(void *v);

/* cfs_vnops.c */
int cfs_pathconf(void *v);
int cfs_ioctl(void *v);
int cfs_kqfilter(void *v);
int cfs_bmap(void *v);
int cfs_strategy(void *v);
int cfs_advlock(void *v);
int cfs_abortop(void *v);

/* cfs_vfsops.c: open the image on devvp into mp; consumes devvp on failure. */
int cfs_mount_device(struct mount *mp, struct vnode *devvp, uint64_t branch,
    struct proc *p);

/* cfs_root.c */
int cfs_mountroot(void);

#endif /* CFS_OPENBSD_KERN_INTERNAL_H */
