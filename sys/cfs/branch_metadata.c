/* SPDX-License-Identifier: ISC
 * branch_metadata.c -- selected inode fields and size in one atomic update.
 */
#include "branch_metadata.h"
#include "branch_file_internal.h"
#include "branch_fs_internal.h"

#include <string.h>

static cfs_bs_child_t retained(cfs_bs_ref_t ref) {
    return (cfs_bs_child_t){.kind = CFS_BS_CHILD_RETAINED, .value.retained = ref};
}

static int valid(cfs_bf_update_t update, cfs_bf_time_t time,
    cfs_bf_file_workspace_t *file_workspace) {
    if (update.fields == 0u || (update.fields & ~CFS_BF_SET_ALL) != 0u ||
        time.nanoseconds >= 1000000000u ||
        ((update.fields & CFS_BF_SET_MODE) && update.mode > 07777u) ||
        ((update.fields & CFS_BF_SET_ATIME) && update.atime.nanoseconds >= 1000000000u) ||
        ((update.fields & CFS_BF_SET_MTIME) && update.mtime.nanoseconds >= 1000000000u) ||
        ((update.fields & CFS_BF_SET_SIZE) && file_workspace == NULL)) {
        return CFS_BS_EINVAL;
    }

    return (update.fields & CFS_BF_SET_SIZE) && update.size > INT64_MAX ?
        CFS_BS_EOVERFLOW : CFS_BS_OK;
}

static int time_equal(cfs_bf_time_t a, cfs_bf_time_t b) {
    return a.seconds == b.seconds && a.nanoseconds == b.nanoseconds;
}

static int unchanged(const cfs_bf_inode_t *before, const cfs_bf_inode_t *after) {
    return before->mode == after->mode && before->uid == after->uid &&
        before->gid == after->gid && before->size == after->size &&
        time_equal(before->atime, after->atime) &&
        time_equal(before->mtime, after->mtime) &&
        time_equal(before->ctime, after->ctime);
}

static int select_fields(cfs_bf_mutation_t *m, cfs_bf_update_t update,
    cfs_bf_time_t time) {
    m->inode = m->target.inode;
    if (update.fields & CFS_BF_SET_SIZE) {
        if (m->inode.type != CFS_BF_REGULAR) {
            return m->inode.type == CFS_BF_DIRECTORY ? CFS_BS_EISDIR : CFS_BS_EINVAL;
        }

        if (m->inode.size != update.size) {
            m->inode.mtime = time;
        }

        m->inode.size = update.size;
    }

    if (update.fields & CFS_BF_SET_MODE) {
        m->inode.mode = update.mode;
    }

    if (update.fields & CFS_BF_SET_UID) {
        m->inode.uid = update.uid;
    }

    if (update.fields & CFS_BF_SET_GID) {
        m->inode.gid = update.gid;
    }

    if (update.fields & CFS_BF_SET_ATIME) {
        m->inode.atime = update.atime;
    }

    if (update.fields & CFS_BF_SET_MTIME) {
        m->inode.mtime = update.mtime;
    }

    m->inode.ctime = time;
    return CFS_BS_OK;
}

static int publish(cfs_bs_store_t *store, cfs_bf_update_t update,
    uint64_t *out_revision, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *workspace, cfs_bf_file_workspace_t *file_workspace) {
    cfs_bf_mutation_t *m = &workspace->mutation;
    cfs_bs_child_t tree = retained(m->target.tree);
    int status = CFS_BS_OK;
    if (update.fields & CFS_BF_SET_SIZE) {
        status = cfs_bf_file_resize(store, m->target.tree, m->target.inode.size,
            m->inode.size, &tree, storage, workspace, file_workspace);
    }

    cfs_bs_child_t inodes;
    if (status == CFS_BS_OK) {
        status = cfs_bf_edit_inode(store, retained(m->view.inodes), m->target.id,
            m->inode, tree, CFS_BT_REPLACE, &inodes, storage, workspace);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_publish(store, m->branch, m->view, inodes,
            out_revision, storage, workspace);
    }

    return status;
}

int cfs_bf_setattr(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t id, cfs_bf_update_t update,
    cfs_bf_time_t time, uint64_t *out_revision, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *workspace, cfs_bf_file_workspace_t *file_workspace) {
    int status = cfs_bf_prepare(store, storage, workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    cfs_bt_stage_reset(&workspace->stage);
    status = valid(update, time, file_workspace);
    if (out_revision == NULL || !cfs_bf_id_valid(id)) {
        status = CFS_BS_EINVAL;
    }

    cfs_bf_mutation_t *m = &workspace->mutation;
    if (status == CFS_BS_OK) {
        if (update.fields & CFS_BF_SET_SIZE) {
            memset(&file_workspace->work, 0, sizeof(file_workspace->work));
        }

        status = cfs_bs_branch_get(store, branch, &m->branch, storage);
    }

    if (status == CFS_BS_OK && m->branch.revision != expected_revision) {
        status = CFS_BS_ECONFLICT;
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_read(store, m->branch.root, m->branch.revision,
            &m->view, storage, workspace);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_get(store, &m->view, id, &m->target, storage, workspace);
    }

    if (status == CFS_BS_OK) {
        status = select_fields(m, update, time);
    }

    if (status == CFS_BS_OK && unchanged(&m->target.inode, &m->inode)) {
        *out_revision = expected_revision;
        return CFS_BS_OK;
    }

    if (status == CFS_BS_OK) {
        status = publish(store, update, out_revision, storage, workspace, file_workspace);
    }

    if (status != CFS_BS_OK) {
        cfs_bt_stage_discard(&workspace->stage);
    }

    return status;
}
