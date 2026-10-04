/* SPDX-License-Identifier: ISC
 * branch_file.c -- checked file operations and atomic inode/root publication.
 */
#include "branch_file_internal.h"
#include "branch_fs_internal.h"

#include <string.h>

static cfs_bs_child_t retained(cfs_bs_ref_t ref) {
    return (cfs_bs_child_t){.kind = CFS_BS_CHILD_RETAINED, .value.retained = ref};
}

static int prepare(cfs_bs_store_t *store, cfs_bf_id_t id,
    cfs_bs_workspace_t *store_workspace, cfs_bf_workspace_t *namespace_workspace,
    cfs_bf_file_workspace_t *workspace) {
    if (workspace == NULL || !cfs_bf_id_valid(id)) {
        return CFS_BS_EINVAL;
    }

    memset(&workspace->work, 0, sizeof(workspace->work));
    return cfs_bf_prepare(store, store_workspace, namespace_workspace);
}

static int regular(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t id, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *namespace_workspace, cfs_bf_file_workspace_t *workspace) {
    int rc = cfs_bf_get(store, view, id, &workspace->entry,
        store_workspace, namespace_workspace);
    if (rc == CFS_BS_OK && workspace->entry.inode.type != CFS_BF_REGULAR) {
        return workspace->entry.inode.type == CFS_BF_DIRECTORY ?
            CFS_BS_EISDIR : CFS_BS_EINVAL;
    }

    return rc;
}

int cfs_bf_file_read(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t id, uint64_t offset, void *bytes, uint32_t length,
    uint32_t *out_length, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *namespace_workspace, cfs_bf_file_workspace_t *workspace) {
    if (out_length == NULL || (length != 0u && bytes == NULL)) {
        return CFS_BS_EINVAL;
    }

    if (length > CFS_BF_IO_MAX) {
        return CFS_BS_EWORKSPACE;
    }

    if (offset > INT64_MAX || length > INT64_MAX - offset) {
        return CFS_BS_EOVERFLOW;
    }

    int rc = prepare(store, id, store_workspace, namespace_workspace, workspace);
    if (rc == CFS_BS_OK) {
        rc = regular(store, view, id, store_workspace, namespace_workspace, workspace);
    }

    if (rc != CFS_BS_OK) {
        return rc;
    }

    cfs_bf_file_state_t state = {.root = retained(workspace->entry.tree),
        .size = workspace->entry.inode.size};
    return cfs_bf_file_content_read(store, NULL, state, offset, bytes, length,
        out_length, store_workspace, workspace);
}

static int begin(cfs_bs_store_t *store, uint64_t branch, uint64_t expected,
    cfs_bf_id_t id, cfs_bf_time_t time, uint64_t *out_revision,
    cfs_bs_workspace_t *store_workspace, cfs_bf_workspace_t *namespace_workspace,
    cfs_bf_file_workspace_t *workspace) {
    if (out_revision == NULL || time.nanoseconds >= 1000000000u) {
        return CFS_BS_EINVAL;
    }

    int rc = prepare(store, id, store_workspace, namespace_workspace, workspace);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    cfs_bt_stage_reset(&namespace_workspace->stage);
    rc = cfs_bs_branch_get(store, branch, &workspace->branch, store_workspace);
    if (rc == CFS_BS_OK && workspace->branch.revision != expected) {
        return CFS_BS_ECONFLICT;
    }

    if (rc == CFS_BS_OK) {
        rc = cfs_bf_read(store, workspace->branch.root, workspace->branch.revision,
            &workspace->view, store_workspace, namespace_workspace);
    }

    if (rc == CFS_BS_OK) {
        rc = regular(store, &workspace->view, id,
            store_workspace, namespace_workspace, workspace);
    }

    return rc;
}

static int publish(cfs_bs_store_t *store, cfs_bs_child_t tree, uint64_t size,
    cfs_bf_time_t time, uint64_t *out_revision,
    cfs_bs_workspace_t *store_workspace, cfs_bf_workspace_t *namespace_workspace,
    cfs_bf_file_workspace_t *workspace) {
    workspace->entry.inode.size = size;
    workspace->entry.inode.mtime = time;
    workspace->entry.inode.ctime = time;
    cfs_bs_child_t inodes;
    int rc = cfs_bf_edit_inode(store, retained(workspace->view.inodes),
        workspace->entry.id, workspace->entry.inode, tree, CFS_BT_REPLACE,
        &inodes, store_workspace, namespace_workspace);
    if (rc == CFS_BS_OK) {
        rc = cfs_bf_publish(store, workspace->branch, workspace->view, inodes,
            out_revision, store_workspace, namespace_workspace);
    }

    return rc;
}

static int finish(int rc, cfs_bf_workspace_t *workspace) {
    if (rc != CFS_BS_OK && workspace != NULL) {
        cfs_bt_stage_discard(&workspace->stage);
    }

    return rc;
}

int cfs_bf_file_write(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t id, uint64_t offset,
    const void *bytes, uint32_t length, cfs_bf_time_t time,
    uint64_t *out_revision, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *namespace_workspace, cfs_bf_file_workspace_t *workspace) {
    int rc;
    if (length > CFS_BF_IO_MAX) {
        return finish(CFS_BS_EWORKSPACE, namespace_workspace);
    }

    if (offset > INT64_MAX || length > INT64_MAX - offset) {
        return finish(CFS_BS_EOVERFLOW, namespace_workspace);
    }

    if (length != 0u && bytes == NULL) {
        return finish(CFS_BS_EINVAL, namespace_workspace);
    }

    rc = begin(store, branch, expected_revision, id, time, out_revision,
        store_workspace, namespace_workspace, workspace);
    if (rc != CFS_BS_OK) {
        return finish(rc, namespace_workspace);
    }

    cfs_bf_file_state_t state = {.root = retained(workspace->entry.tree),
        .size = workspace->entry.inode.size};
    cfs_bf_file_state_t result;
    rc = cfs_bf_file_content_write(store, &namespace_workspace->stage, state,
        offset, bytes, length, &result, store_workspace, workspace);
    if (rc == CFS_BS_OK && length == 0u) {
        *out_revision = expected_revision;
    } else if (rc == CFS_BS_OK) {
        rc = publish(store, result.root, result.size, time, out_revision,
            store_workspace, namespace_workspace, workspace);
    }

    return finish(rc, namespace_workspace);
}

int cfs_bf_file_truncate(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t id, uint64_t size,
    cfs_bf_time_t time, uint64_t *out_revision,
    cfs_bs_workspace_t *store_workspace, cfs_bf_workspace_t *namespace_workspace,
    cfs_bf_file_workspace_t *workspace) {
    if (size > INT64_MAX) {
        return finish(CFS_BS_EOVERFLOW, namespace_workspace);
    }

    int rc = begin(store, branch, expected_revision, id, time, out_revision,
        store_workspace, namespace_workspace, workspace);
    if (rc != CFS_BS_OK) {
        return finish(rc, namespace_workspace);
    }

    cfs_bf_file_state_t state = {.root = retained(workspace->entry.tree),
        .size = workspace->entry.inode.size};
    cfs_bf_file_state_t result;
    rc = cfs_bf_file_content_truncate(store, &namespace_workspace->stage,
        state, size, &result, store_workspace, workspace);
    if (rc == CFS_BS_OK && size == state.size) {
        *out_revision = expected_revision;
    } else if (rc == CFS_BS_OK) {
        rc = publish(store, result.root, result.size, time, out_revision,
            store_workspace, namespace_workspace, workspace);
    }

    return finish(rc, namespace_workspace);
}
