/* SPDX-License-Identifier: ISC
 * branch_fs_link.c -- atomic nondirectory hard-link creation within a branch.
 */
#include "branch_fs_internal.h"

static cfs_bs_child_t retained(cfs_bs_ref_t ref) {
    return (cfs_bs_child_t){.kind = CFS_BS_CHILD_RETAINED, .value.retained = ref};
}

static int link_records(cfs_bs_store_t *store, const void *name,
    uint16_t length, cfs_bf_time_t time, uint64_t *out_revision,
    cfs_bs_workspace_t *storage, cfs_bf_workspace_t *workspace) {
    cfs_bf_mutation_t *mutation = &workspace->mutation;
    cfs_bf_entry_t *parent = &mutation->parent;
    cfs_bf_entry_t *target = &mutation->target;
    if (target->inode.links == 0u) {
        return CFS_BS_ENOENT;
    }

    if (target->inode.type == CFS_BF_DIRECTORY) {
        return CFS_BS_EISDIR;
    }

    if (parent->inode.size == INT64_MAX || target->inode.links == UINT64_MAX) {
        return CFS_BS_EOVERFLOW;
    }

    (void)cfs_bf_dirent_frame(target->id, workspace->dirent);
    cfs_bt_change_t change = {.kind = CFS_BT_INSERT, .key = name,
        .key_length = length, .value = workspace->dirent,
        .value_length = CFS_BF_ID_BYTES};
    cfs_bs_child_t names;
    cfs_bs_child_t inodes;
    cfs_bs_child_t next;
    int status = cfs_bt_edit(store, &workspace->stage, retained(parent->tree),
        change, &names, storage, &workspace->edit);
    if (status != CFS_BS_OK) {
        return status;
    }

    parent->inode.size++;
    parent->inode.mtime = time;
    parent->inode.ctime = time;
    target->inode.links++;
    target->inode.ctime = time;
    status = cfs_bf_edit_inode(store, retained(mutation->view.inodes),
        parent->id, parent->inode, names, CFS_BT_REPLACE, &inodes,
        storage, workspace);
    if (status == CFS_BS_OK) {
        status = cfs_bf_edit_inode(store, inodes, target->id, target->inode,
            retained(target->tree), CFS_BT_REPLACE, &next, storage, workspace);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_publish(store, mutation->branch, mutation->view, next,
            out_revision, storage, workspace);
    }

    return status;
}

static int link_once(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t target, cfs_bf_id_t parent,
    const void *name, uint16_t length, cfs_bf_time_t time,
    uint64_t *out_revision, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *workspace) {
    int status = cfs_bf_prepare(store, storage, workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    cfs_bt_stage_reset(&workspace->stage);
    if (out_revision == NULL || !cfs_bf_id_valid(target) ||
        !cfs_bf_id_valid(parent) || !cfs_bf_name_valid(name, length) ||
        time.nanoseconds >= 1000000000u) {
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
        status = cfs_bf_get(store, &mutation->view, target,
            &mutation->target, storage, workspace);
    }

    if (status == CFS_BS_OK) {
        status = link_records(store, name, length, time, out_revision,
            storage, workspace);
    }

    if (status != CFS_BS_OK) {
        cfs_bt_stage_discard(&workspace->stage);
    }

    return status;
}

int cfs_bf_link(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t target, cfs_bf_id_t parent,
    const void *name, uint16_t length, cfs_bf_time_t time,
    uint64_t *out_revision, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *workspace) {
    int status = link_once(store, branch, expected_revision, target, parent,
        name, length, time, out_revision, storage, workspace);

    if (status == CFS_BS_EWORKSPACE) {
        status = cfs_bf_retry_done(store, branch, link_once(store, branch,
            expected_revision, target, parent, name, length, time,
            out_revision, storage, workspace), storage);
    }

    return status;
}
