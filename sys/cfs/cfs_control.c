/* SPDX-License-Identifier: ISC
 *
 * cfs_control.c -- online control of a mounted cfs image through
 * quotactl(2): stats, catalog listing, checked fork and discard, bounded
 * reap and checkpoint, all under the owner lock; and vfs.cfs counters.
 */
#include "cfs_kern_internal.h"

#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/sysctl.h>

#include "cfs_control.h"

/* A listing copies every catalog branch into the fixed row array. */
_Static_assert(CFS_BS_MAX_BRANCHES <= CFS_CONTROL_ROWS,
    "control rows cannot hold the catalog");

/* Operations through this interface since boot; read by vfs.cfs. */
static volatile unsigned long cfs_control_forks;
static volatile unsigned long cfs_control_discards;
static volatile unsigned long cfs_control_reaped;
static volatile unsigned long cfs_control_checkpoints;

static int zero_bytes(const uint8_t *bytes, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] != 0u) {
            return 0;
        }
    }

    return 1;
}

static int args_valid(const struct cfs_control_args *a) {
    uint32_t n = a->cca_name_len;
    int no_name = n == 0u && zero_bytes(a->cca_name, sizeof(a->cca_name));
    int named = n != 0u && n <= CFS_CONTROL_NAME_MAX &&
        memchr(a->cca_name, '\0', n) == NULL &&
        zero_bytes(a->cca_name + n, sizeof(a->cca_name) - n);

    if (a->cca_version != CFS_CONTROL_ARGS_VERSION ||
        a->cca_size != CFS_CONTROL_ARGS_SIZE || a->cca_flags != 0u) {
        return 0;
    }

    switch (a->cca_op) {
    case CFS_CONTROL_STATS:
    case CFS_CONTROL_BRANCHES:
    case CFS_CONTROL_CHECKPOINT:
        return a->cca_branch == 0u && a->cca_expected == 0u &&
            a->cca_limit == 0u && no_name;
    case CFS_CONTROL_FORK:
        return a->cca_branch != 0u && a->cca_expected != 0u &&
            a->cca_limit == 0u && named;
    case CFS_CONTROL_DISCARD:
        return a->cca_branch != 0u && a->cca_expected != 0u &&
            a->cca_limit == 0u && no_name;
    case CFS_CONTROL_REAP:
        return a->cca_branch == 0u && a->cca_expected == 0u &&
            a->cca_limit != 0u && a->cca_limit <= CFS_CONTROL_REAP_MAX &&
            no_name;
    default:
        return 0;
    }
}

static int branch_get(cfs_kimage_t *image, uint64_t id,
    cfs_bs_branch_t *out) {
    return cfs_kimage_errno(cfs_bs_branch_get(&image->store, id, out,
        &image->work->storage));
}

/* The expected durable base authorizes capture of the current working
 * state: check, checkpoint, then refresh before the store mutation. */
static int checked_branch(cfs_kimage_t *image, uint64_t id,
    uint64_t expected, cfs_bs_branch_t *out) {
    int error = branch_get(image, id, out);
    if (error == 0 && out->revision != expected) {
        error = cfs_kimage_errno(CFS_BS_ECONFLICT);
    }

    if (error == 0) {
        error = cfs_kimage_flush(image);
    }

    return error == 0 ? branch_get(image, id, out) : error;
}

static void row(struct cfs_control_branch *r, const cfs_bs_branch_t *b,
    int mounted) {
    r->ccb_id = b->id;
    r->ccb_revision = b->revision;
    r->ccb_origin_id = b->origin_id;
    r->ccb_origin_revision = b->origin_revision;
    r->ccb_mounted = (uint32_t)mounted;
    r->ccb_name_len = b->name_length;
    memcpy(r->ccb_name, b->name, b->name_length);
}

static int op_fork(cfs_kimage_t *image, struct cfs_control_args *a) {
    cfs_bs_branch_t branch;
    int error = checked_branch(image, a->cca_branch, a->cca_expected,
        &branch);
    if (error != 0) {
        return error;
    }

    int raw = cfs_kimage_mutation_begin(image);
    if (raw == CFS_BS_OK) {
        raw = cfs_bs_branch_fork(&image->store, a->cca_branch,
            branch.revision, a->cca_name, (uint16_t)a->cca_name_len,
            &branch, &image->work->storage);
        raw = cfs_kimage_mutation_end(image, raw);
    }

    if (raw == CFS_BS_OK) {
        row(&a->cca_rows[0], &branch, 0);
        a->cca_count = 1u;
        atomic_inc_long(&cfs_control_forks);
    }

    return cfs_kimage_errno(raw);
}

static int op_discard(cfs_kimage_t *image, struct cfs_control_args *a) {
    cfs_bs_branch_t branch;
    if (a->cca_branch == image->branch_id) {
        return EBUSY;
    }

    int error = checked_branch(image, a->cca_branch, a->cca_expected,
        &branch);
    if (error != 0) {
        return error;
    }

    int raw = cfs_kimage_mutation_begin(image);
    if (raw == CFS_BS_OK) {
        raw = cfs_bs_branch_discard(&image->store, a->cca_branch,
            branch.revision, &image->work->storage);
        raw = cfs_kimage_mutation_end(image, raw);
    }

    return cfs_kimage_errno(raw);
}

static int op_reap(cfs_kimage_t *image, struct cfs_control_args *a) {
    uint32_t reaped = 0u;
    int raw = cfs_kimage_mutation_begin(image);
    if (raw == CFS_BS_OK) {
        raw = cfs_bs_reap(&image->store, a->cca_limit, &reaped,
            &image->work->storage);
        raw = cfs_kimage_mutation_end(image, raw);
    }

    if (raw == CFS_BS_OK) {
        a->cca_reaped = reaped;
        atomic_add_long(&cfs_control_reaped, reaped);
    }

    return cfs_kimage_errno(raw);
}

static int op_checkpoint(cfs_kimage_t *image) {
    cfs_bw_status_t state;
    int status = cfs_bw_status(&image->working, &state);
    if (status != CFS_BS_OK) {
        return cfs_kimage_errno(status);
    }

    if (state.delayed_error != CFS_BS_OK) {
        return cfs_kimage_errno(state.delayed_error);
    }

    int error = cfs_kimage_checkpoint(image, state.accepted_sequence);
    if (error == 0) {
        atomic_inc_long(&cfs_control_checkpoints);
    }

    return error;
}

static int op_branches(cfs_kimage_t *image, struct cfs_control_args *a) {
    cfs_bs_branch_t branch;

    for (uint32_t i = 0u; i < CFS_BS_MAX_BRANCHES; i++) {
        int status = cfs_bs_inspect_branch(&image->store, i, &branch,
            &image->work->storage);
        if (status == CFS_BS_ENOENT) {
            continue;
        }

        if (status != CFS_BS_OK) {
            return cfs_kimage_errno(status);
        }

        if (branch.kind == CFS_BS_CATALOG_BRANCH) {
            row(&a->cca_rows[a->cca_count++], &branch,
                branch.id == image->branch_id);
        }
    }

    return 0;
}

static int fill_stats(struct cfs_kmount *km, struct cfs_control_stats *s) {
    cfs_kimage_t *image = &km->km_image;
    cfs_kimage_stats_t space;
    cfs_bs_stats_t store;
    cfs_bw_status_t state;

    int error = cfs_kimage_stats(image, &space);
    if (error == 0) {
        error = cfs_kimage_errno(cfs_bs_inspect_stats(&image->store, &store,
            &image->work->storage));
    }

    if (error == 0) {
        error = cfs_kimage_errno(cfs_bw_status(&image->working, &state));
    }

    if (error == 0) {
        error = cfs_kimage_head(image);
    }

    if (error != 0) {
        return error;
    }

    *s = (struct cfs_control_stats){
        .ccs_blocks = space.blocks,
        .ccs_free = space.free,
        .ccs_available = space.available,
        .ccs_generation = store.generation,
        .ccs_total_blocks = store.total_blocks,
        .ccs_semantic_start = store.semantic_start,
        .ccs_pending = store.pending_count,
        .ccs_reserve = store.reserve,
        .ccs_accepted_sequence = state.accepted_sequence,
        .ccs_durable_sequence = state.durable_sequence,
        .ccs_branch_id = image->branch_id,
        .ccs_branch_revision = image->branch.revision,
        .ccs_branch_count = store.branch_count,
        .ccs_pin_count = store.pin_count,
        .ccs_stage_count = store.staging_count,
        .ccs_consumers = image->consumers.handles,
        .ccs_consumer_limit = CFS_KIMAGE_CONSUMERS,
        .ccs_dirty = state.dirty,
        .ccs_delayed_error = (uint32_t)cfs_kimage_errno(state.delayed_error),
        .ccs_service_pending = (uint32_t)km->km_svc_pending,
        .ccs_service_error = (uint32_t)km->km_svc_error,
        .ccs_service_retry = (uint32_t)km->km_svc_retry,
        .ccs_read_only = (uint32_t)km->km_read_only,
        .ccs_ready = (uint32_t)image->ready,
    };
    return 0;
}

/* Owner lock held. Every success also reports the image after the call. */
static int execute(struct cfs_kmount *km, struct cfs_control_args *a) {
    cfs_kimage_t *image = &km->km_image;
    int error = 0;

    if (km->km_unmounting) {
        return EBUSY;
    }

    if (image->store.poisoned) {
        return EIO;
    }

    if (km->km_read_only && a->cca_op != CFS_CONTROL_STATS &&
        a->cca_op != CFS_CONTROL_BRANCHES) {
        return EROFS;
    }

    switch (a->cca_op) {
    case CFS_CONTROL_BRANCHES:
        error = op_branches(image, a);
        break;
    case CFS_CONTROL_FORK:
        error = op_fork(image, a);
        break;
    case CFS_CONTROL_DISCARD:
        error = op_discard(image, a);
        if (error == 0) {
            atomic_inc_long(&cfs_control_discards);
            cfs_service_touch(km, CFS_SVC_REAP);
        }

        break;
    case CFS_CONTROL_REAP:
        error = op_reap(image, a);
        break;
    case CFS_CONTROL_CHECKPOINT:
        error = op_checkpoint(image);
        break;
    default:
        break;
    }

    if (error != 0) {
        return error;
    }

    /* A mutation has happened by now: a failed refresh leaves the stats
     * zeroed rather than reporting the change as a failure. */
    error = fill_stats(km, &a->cca_stats);
    if (error != 0 && a->cca_op != CFS_CONTROL_STATS &&
        a->cca_op != CFS_CONTROL_BRANCHES) {
        memset(&a->cca_stats, 0, sizeof(a->cca_stats));
        error = 0;
    }

    return error;
}

int cfs_control(struct mount *mp, int cmd, caddr_t arg, struct proc *p) {
    struct cfs_control_args *a;
    struct cfs_kmount *km;
    int error;

    if (cmd != CFS_CONTROL_CMD) {
        return EOPNOTSUPP;
    }

    /* sys_quotactl releases the vnode without busying the mount: busy it
     * before anything can sleep, or an unmount could free it under us. */
    if (vfs_busy(mp, VB_READ | VB_NOWAIT) != 0) {
        return EBUSY;
    }

    km = VFSTOCFS(mp);
    if (!km) {
        vfs_unbusy(mp);
        return EBUSY;
    }

    if (suser(p) != 0 && p->p_ucred->cr_uid != mp->mnt_stat.f_owner) {
        vfs_unbusy(mp);
        return EPERM;
    }

    a = malloc(sizeof(*a), M_TEMP, M_WAITOK);
    error = copyin(arg, a, sizeof(*a));
    if (error == 0 && !args_valid(a)) {
        error = EINVAL;
    }

    if (error == 0) {
        a->cca_count = 0u;
        a->cca_reaped = 0u;
        memset(&a->cca_stats, 0, sizeof(a->cca_stats));
        memset(a->cca_rows, 0, sizeof(a->cca_rows));
        cfs_owner_enter(km);
        error = execute(km, a);
        cfs_owner_exit(km);
    }

    vfs_unbusy(mp);
    if (error == 0) {
        error = copyout(a, arg, sizeof(*a));
    }

    free(a, M_TEMP, sizeof(*a));
    return error;
}

int cfs_control_sysctl(int *name, u_int namelen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
    struct vfsconf *vfsp;
    unsigned long value;

    (void)newlen;
    if (namelen != 1) {
        return ENOTDIR;
    }

    switch (name[0]) {
    case CFS_VFS_MOUNTS:
        vfsp = vfs_byname(MOUNT_CFS);
        value = vfsp != NULL ? vfsp->vfc_refcount : 0u;
        break;
    case CFS_VFS_FORKS:
        value = cfs_control_forks;
        break;
    case CFS_VFS_DISCARDS:
        value = cfs_control_discards;
        break;
    case CFS_VFS_REAPED:
        value = cfs_control_reaped;
        break;
    case CFS_VFS_CHECKPOINTS:
        value = cfs_control_checkpoints;
        break;
    default:
        return EOPNOTSUPP;
    }

    return sysctl_rdquad(oldp, oldlenp, newp, (int64_t)value);
}
