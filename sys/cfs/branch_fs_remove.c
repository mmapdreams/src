/* SPDX-License-Identifier: ISC
 * branch_fs_remove.c -- atomic unlink and empty-directory removal.
 */
#include "branch_fs_internal.h"

static cfs_bs_child_t retained(cfs_bs_ref_t ref) {
    return (cfs_bs_child_t){.kind = CFS_BS_CHILD_RETAINED, .value.retained = ref};
}

static int validate_target(cfs_bs_store_t *store, uint16_t type,
    cfs_bs_workspace_t *storage, cfs_bf_workspace_t *workspace) {
    cfs_bf_mutation_t *mutation = &workspace->mutation;
    cfs_bf_entry_t *target = &mutation->target;
    cfs_bf_entry_t *parent = &mutation->parent;

    if (cfs_bf_id_equal(target->id, mutation->view.root_id) ||
        parent->inode.size == 0u) {
        return CFS_BS_ECORRUPT;
    }

    if ((target->inode.type == CFS_BF_DIRECTORY) !=
        (type == CFS_BF_DIRECTORY)) {
        return type == CFS_BF_DIRECTORY ? CFS_BS_ENOTDIR : CFS_BS_EISDIR;
    }

    if (type == CFS_BF_DIRECTORY) {
        int status = cfs_bf_directory(store, &mutation->view, target->id,
            target, storage, workspace);
        if (status != CFS_BS_OK) {
            return status;
        }

        if (parent->inode.links < 3u) {
            return CFS_BS_ECORRUPT;
        }

        if (target->inode.size != 0u) {
            return CFS_BS_ENOTEMPTY;
        }

        parent->inode.links--;
    }

    parent->inode.size--;

    /* A nondirectory name cannot account for a claimed directory link. */
    if (cfs_bf_inode_frame(parent->inode, parent->inode.size != 0u,
            workspace->inode_bytes) != CFS_BS_OK) {
        return CFS_BS_ECORRUPT;
    }

    return CFS_BS_OK;
}

static int remove_records(cfs_bs_store_t *store, const void *name,
    uint16_t length, int keep_open, cfs_bf_time_t time, uint64_t *out_revision,
    cfs_bs_workspace_t *storage, cfs_bf_workspace_t *workspace) {
    cfs_bf_mutation_t *mutation = &workspace->mutation;
    cfs_bf_entry_t *parent = &mutation->parent;
    cfs_bf_entry_t *target = &mutation->target;
    cfs_bt_change_t change = {.kind = CFS_BT_DELETE, .key = name,
        .key_length = length};
    cfs_bs_child_t names;
    cfs_bs_child_t inodes;
    int status = cfs_bt_edit(store, &workspace->stage, retained(parent->tree),
        change, &names, storage, &workspace->edit);

    if (status != CFS_BS_OK) {
        return status;
    }

    parent->inode.mtime = time;
    parent->inode.ctime = time;
    status = cfs_bf_edit_inode(store, retained(mutation->view.inodes),
        parent->id, parent->inode, names, CFS_BT_REPLACE, &inodes,
        storage, workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    cfs_bf_roots_t roots = {inodes, retained(mutation->view.orphans)};
    status = cfs_bf_drop_link(store, target, keep_open, time, roots, &roots,
        storage, workspace);

    if (status == CFS_BS_OK) {
        status = cfs_bf_publish_roots(store, mutation->branch, mutation->view,
            roots.inodes, roots.orphans, out_revision, storage, workspace);
    }

    return status;
}

static int remove_once(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t parent, const void *name,
    uint16_t length, uint16_t type, int keep_open, cfs_bf_time_t time,
    uint64_t *out_revision, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *workspace) {
    int status = cfs_bf_prepare(store, storage, workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    cfs_bt_stage_reset(&workspace->stage);
    if (out_revision == NULL || !cfs_bf_id_valid(parent) ||
        !cfs_bf_name_valid(name, length) || time.nanoseconds >= 1000000000u ||
        (type != CFS_BF_REGULAR && type != CFS_BF_DIRECTORY) ||
        (keep_open != 0 && keep_open != 1)) {
        status = CFS_BS_EINVAL;
    }

    cfs_bf_mutation_t *mutation = &workspace->mutation;
    if (status == CFS_BS_OK) {
        status = cfs_bs_branch_get(store, branch, &mutation->branch, storage);
    }

    if (status == CFS_BS_OK && mutation->branch.revision != expected_revision) {
        status = CFS_BS_ECONFLICT;
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_read(store, mutation->branch.root,
            mutation->branch.revision, &mutation->view, storage, workspace);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_directory(store, &mutation->view, parent,
            &mutation->parent, storage, workspace);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_lookup(store, &mutation->view, parent, name, length,
            &mutation->target, storage, workspace);
    }

    if (status == CFS_BS_OK) {
        status = validate_target(store, type, storage, workspace);
    }

    if (status == CFS_BS_OK) {
        status = remove_records(store, name, length, keep_open, time, out_revision,
            storage, workspace);
    }

    if (status != CFS_BS_OK) {
        cfs_bt_stage_discard(&workspace->stage);
    }

    return status;
}

int cfs_bf_remove(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t parent, const void *name,
    uint16_t length, uint16_t type, int keep_open, cfs_bf_time_t time,
    uint64_t *out_revision, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *workspace) {
    int status = remove_once(store, branch, expected_revision, parent, name,
        length, type, keep_open, time, out_revision, storage, workspace);

    if (status == CFS_BS_EWORKSPACE) {
        status = cfs_bf_retry_done(store, branch, remove_once(store, branch,
            expected_revision, parent, name, length, type, keep_open, time,
            out_revision, storage, workspace), storage);
    }

    return status;
}
