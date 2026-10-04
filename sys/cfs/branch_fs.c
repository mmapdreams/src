/* SPDX-License-Identifier: ISC
 * branch_fs.c -- authenticated namespace roots, inode lookup, and enumeration.
 */
#include "branch_fs_internal.h"

#include <string.h>

int cfs_bf_prepare(cfs_bs_store_t *store, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace) {
    if (store == NULL || store_workspace == NULL || workspace == NULL) {
        return CFS_BS_EINVAL;
    }

    if (store->poisoned != 0u) {
        return CFS_BS_EPOISON;
    }

    return store->generation == 0u ? CFS_BS_ECORRUPT : CFS_BS_OK;
}

int cfs_bf_get(cfs_bs_store_t *store, const cfs_bf_view_t *view, cfs_bf_id_t id,
    cfs_bf_entry_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace) {
    int rc = cfs_bf_prepare(store, store_workspace, workspace);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (view == NULL || out == NULL || !cfs_bf_id_valid(id)) {
        return CFS_BS_EINVAL;
    }

    if (id.birth > view->revision) {
        return CFS_BS_ENOENT;
    }

    (void)cfs_bf_id_key(id, workspace->key);
    rc = cfs_bt_lookup(store, view->inodes, workspace->key, CFS_BF_ID_BYTES,
        &workspace->value, store_workspace, &workspace->read);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    cfs_bf_entry_t entry = {.id = id, .tree = workspace->value.child};
    rc = cfs_bf_inode_decode(workspace->value.bytes, workspace->value.length,
        entry.tree.block != CFS_BS_BLOCK_NONE, &entry.inode);
    if (rc != CFS_BS_OK || entry.inode.parent.birth > view->revision) {
        return CFS_BS_ECORRUPT;
    }

    *out = entry;
    return CFS_BS_OK;
}

int cfs_bf_directory(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t id, cfs_bf_entry_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace) {
    cfs_bf_entry_t directory;
    int rc = cfs_bf_get(store, view, id, &directory, store_workspace, workspace);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (directory.inode.type != CFS_BF_DIRECTORY) {
        return CFS_BS_ENOTDIR;
    }

    if (directory.inode.links == 0u) {
        return CFS_BS_ENOENT;
    }

    if (directory.tree.block != CFS_BS_BLOCK_NONE) {
        cfs_bt_read_frame_t *frame = &workspace->read.path[0];
        uint32_t children;
        uint32_t length;
        rc = cfs_bs_object_read(store, directory.tree,
            workspace->read.physical_children,
            CFS_BT_MAX_RECORDS, &children, frame->payload, sizeof(frame->payload),
            &length, store_workspace);
        if (rc == CFS_BS_EWORKSPACE || rc == CFS_BS_ENOENT) {
            return CFS_BS_ECORRUPT;
        }

        if (rc != CFS_BS_OK) {
            return rc;
        }

        rc = cfs_bt_node_decode(frame->payload, length, children, 1, &frame->node);
        if (rc != CFS_BS_OK || frame->node.item_count != directory.inode.size) {
            return CFS_BS_ECORRUPT;
        }
    }

    *out = directory;
    return CFS_BS_OK;
}

int cfs_bf_read(cfs_bs_store_t *store, cfs_bs_ref_t root, uint64_t revision,
    cfs_bf_view_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace) {
    int rc = cfs_bf_prepare(store, store_workspace, workspace);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (out == NULL || revision == 0u) {
        return CFS_BS_EINVAL;
    }

    if (root.block == CFS_BS_BLOCK_NONE) {
        return root.incarnation == 0u && root.hash == 0u ?
            CFS_BS_ENOENT : CFS_BS_EINVAL;
    }

    cfs_bs_ref_t children[2];
    uint32_t count;
    uint32_t length;
    rc = cfs_bs_object_read(store, root, children, 2u, &count,
        workspace->root_bytes, CFS_BF_ROOT_BYTES, &length, store_workspace);
    if (rc == CFS_BS_EWORKSPACE) {
        return CFS_BS_ECORRUPT;
    }

    if (rc != CFS_BS_OK) {
        return rc;
    }

    cfs_bf_view_t view = {.revision = revision,
        .orphans = {CFS_BS_BLOCK_NONE, 0u, 0u}};
    rc = cfs_bf_root_decode(workspace->root_bytes, length, count, &view.root_id);
    if (rc != CFS_BS_OK || view.root_id.birth > revision) {
        return CFS_BS_ECORRUPT;
    }

    view.inodes = children[0];
    if (count == 2u) {
        view.orphans = children[1];
    }

    cfs_bf_entry_t directory;
    rc = cfs_bf_directory(store, &view, view.root_id, &directory,
        store_workspace, workspace);
    if (rc == CFS_BS_ENOTDIR || rc == CFS_BS_ENOENT) {
        return CFS_BS_ECORRUPT;
    }

    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (!cfs_bf_id_equal(directory.inode.parent, view.root_id)) {
        return CFS_BS_ECORRUPT;
    }

    *out = view;
    return CFS_BS_OK;
}

int cfs_bf_lookup(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t directory, const void *name, uint16_t length,
    cfs_bf_entry_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace) {
    if (out == NULL || !cfs_bf_name_valid(name, length)) {
        return CFS_BS_EINVAL;
    }

    cfs_bf_entry_t parent;
    int rc = cfs_bf_directory(store, view, directory, &parent,
        store_workspace, workspace);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = cfs_bt_lookup(store, parent.tree, name, length, &workspace->value,
        store_workspace, &workspace->read);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    cfs_bf_id_t id;
    rc = cfs_bf_dirent_decode(workspace->value.bytes, workspace->value.length, &id);
    if (rc != CFS_BS_OK || workspace->value.child.block != CFS_BS_BLOCK_NONE ||
        id.birth > view->revision) {
        return CFS_BS_ECORRUPT;
    }

    cfs_bf_entry_t entry;
    rc = cfs_bf_get(store, view, id, &entry, store_workspace, workspace);
    if (rc == CFS_BS_ENOENT || (rc == CFS_BS_OK && entry.inode.links == 0u)) {
        return CFS_BS_ECORRUPT;
    }

    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (entry.inode.type == CFS_BF_DIRECTORY &&
        !cfs_bf_id_equal(entry.inode.parent, directory)) {
        return CFS_BS_ECORRUPT;
    }

    *out = entry;
    return CFS_BS_OK;
}

typedef struct directory_scan {
    cfs_bf_visit_fn visit;
    void *context;
    uint64_t revision;
    int status;
} directory_scan_t;

static int visit_name(void *context, const cfs_bt_record_t *record,
    const cfs_bs_ref_t *child) {
    directory_scan_t *scan = context;
    cfs_bf_id_t id;
    if (child != NULL || !cfs_bf_name_valid(record->key, record->key_length) ||
        cfs_bf_dirent_decode(record->value, record->value_length, &id) != CFS_BS_OK ||
        id.birth > scan->revision) {
        scan->status = CFS_BS_ECORRUPT;
        return CFS_BT_SCAN_STOP;
    }

    return scan->visit(scan->context, record->key, record->key_length, id);
}

int cfs_bf_list(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t directory, cfs_bf_visit_fn visit, void *context,
    cfs_bt_scan_result_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace) {
    if (visit == NULL || out == NULL) {
        return CFS_BS_EINVAL;
    }

    cfs_bf_entry_t parent;
    int rc = cfs_bf_directory(store, view, directory, &parent,
        store_workspace, workspace);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    directory_scan_t scan = {.visit = visit, .context = context,
        .revision = view->revision, .status = CFS_BS_OK};
    cfs_bt_scan_result_t result;
    rc = cfs_bt_scan(store, parent.tree, NULL, 0u, NULL, 0u, visit_name, &scan,
        &result, store_workspace, &workspace->read);
    if (rc != CFS_BS_OK || scan.status != CFS_BS_OK) {
        return rc == CFS_BS_OK ? scan.status : rc;
    }

    *out = result;
    return CFS_BS_OK;
}
