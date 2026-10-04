/* SPDX-License-Identifier: ISC
 * branch_symlink.c -- atomic immutable link text and complete target validation.
 */
#include "branch_symlink_internal.h"
#include "branch_fs_internal.h"
#include "codec.h"

#include <string.h>

#define TARGET_CHUNK 4000u

static int target_valid(const void *target, uint32_t length) {
    if (target == NULL || length == 0u || length > CFS_BF_SYMLINK_MAX) {
        return 0;
    }

    const uint8_t *bytes = target;
    for (uint32_t i = 0u; i < length; i++) {
        if (bytes[i] == 0u) {
            return 0;
        }
    }

    return 1;
}

int cfs_bf_symlink_stage(cfs_bt_stage_t *stage, const void *target,
    uint32_t length, cfs_bs_child_t *out) {
    if (out == NULL || !target_valid(target, length)) {
        cfs_bt_stage_discard(stage);
        return CFS_BS_EINVAL;
    }

    cfs_bs_child_t children[2];
    uint32_t count = (length + TARGET_CHUNK - 1u) / TARGET_CHUNK;
    uint32_t index;
    for (uint32_t i = 0u; i < count; i++) {
        uint32_t remaining = length - i * TARGET_CHUNK;
        cfs_bs_object_plan_t plan = {
            .payload = (const uint8_t *)target + i * TARGET_CHUNK,
            .payload_length = remaining < TARGET_CHUNK ? remaining : TARGET_CHUNK};
        int status = cfs_bt_stage_add(stage, &plan, &index);
        if (status != CFS_BS_OK) {
            return status;
        }

        children[i] = (cfs_bs_child_t){.kind = CFS_BS_CHILD_PREPARED,
            .value.prepared_index = index};
    }

    uint8_t header[16];
    memcpy(header, "CFSLK001", 8u);
    cfs_put64(header + 8u, length);
    cfs_bs_object_plan_t plan = {.children = children, .child_count = count,
        .payload = header, .payload_length = sizeof(header)};
    int status = cfs_bt_stage_add(stage, &plan, &index);
    if (status == CFS_BS_OK) {
        *out = (cfs_bs_child_t){.kind = CFS_BS_CHILD_PREPARED,
            .value.prepared_index = index};
    }

    return status;
}

int cfs_bf_symlink(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t parent, const void *name,
    uint16_t name_length, const void *target, uint32_t target_length,
    cfs_bf_attributes_t attributes, cfs_bf_created_t *out,
    cfs_bs_workspace_t *storage, cfs_bf_workspace_t *workspace) {
    int status = cfs_bf_prepare(store, storage, workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    cfs_bt_stage_reset(&workspace->stage);
    cfs_bf_mutation_t *m = &workspace->mutation;
    m->inode = (cfs_bf_inode_t){.type = CFS_BF_SYMLINK,
        .mode = attributes.mode, .uid = attributes.uid, .gid = attributes.gid,
        .size = target_length, .links = 1u, .atime = attributes.time,
        .mtime = attributes.time, .ctime = attributes.time,
        .btime = attributes.time};
    if (out == NULL || !cfs_bf_id_valid(parent) ||
        !cfs_bf_name_valid(name, name_length) ||
        !target_valid(target, target_length) ||
        cfs_bf_inode_frame(m->inode, 1, workspace->inode_bytes) != CFS_BS_OK) {
        status = CFS_BS_EINVAL;
    } else {
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
        status = cfs_bf_directory(store, &m->view, parent, &m->parent,
            storage, workspace);
    }

    cfs_bs_child_t content;
    if (status == CFS_BS_OK) {
        status = cfs_bf_symlink_stage(&workspace->stage, target, target_length,
            &content);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_create_records(store, name, name_length, content, out,
            storage, workspace);
    }

    if (status != CFS_BS_OK) {
        cfs_bt_stage_discard(&workspace->stage);
    }

    return status;
}

static int content_status(int status) {
    return status == CFS_BS_EWORKSPACE || status == CFS_BS_ENOENT ?
        CFS_BS_ECORRUPT : status;
}

int cfs_bf_symlink_load(cfs_bs_store_t *store, const cfs_bf_entry_t *entry,
    cfs_bs_workspace_t *storage, cfs_bf_symlink_workspace_t *workspace) {
    cfs_bs_ref_t children[2];
    uint32_t count;
    uint32_t length;
    int status = cfs_bs_object_read(store, entry->tree, children, 2u, &count,
        workspace->header, sizeof(workspace->header), &length, storage);
    if (status != CFS_BS_OK) {
        return content_status(status);
    }

    uint32_t total = (uint32_t)entry->inode.size;
    if (length != sizeof(workspace->header) ||
        memcmp(workspace->header, "CFSLK001", 8u) != 0 ||
        cfs_get64(workspace->header + 8u) != total ||
        count != (total + TARGET_CHUNK - 1u) / TARGET_CHUNK) {
        return CFS_BS_ECORRUPT;
    }

    for (uint32_t i = 0u; i < count; i++) {
        uint32_t remaining = total - i * TARGET_CHUNK;
        uint32_t expected = remaining < TARGET_CHUNK ? remaining : TARGET_CHUNK;
        uint32_t child_count;
        status = cfs_bs_object_read(store, children[i], NULL, 0u, &child_count,
            workspace->bytes + i * TARGET_CHUNK, expected, &length, storage);
        if (status != CFS_BS_OK) {
            return content_status(status);
        }

        if (child_count != 0u || length != expected) {
            return CFS_BS_ECORRUPT;
        }
    }

    return target_valid(workspace->bytes, total) ? CFS_BS_OK : CFS_BS_ECORRUPT;
}

int cfs_bf_readlink(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t id, void *bytes, uint32_t capacity, uint32_t *out_length,
    cfs_bs_workspace_t *storage, cfs_bf_workspace_t *namespace_workspace,
    cfs_bf_symlink_workspace_t *workspace) {
    if (workspace == NULL || bytes == NULL || out_length == NULL) {
        return CFS_BS_EINVAL;
    }

    cfs_bf_entry_t entry;
    int status = cfs_bf_get(store, view, id, &entry, storage, namespace_workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    if (entry.inode.type != CFS_BF_SYMLINK) {
        return CFS_BS_EINVAL;
    }

    if (capacity < entry.inode.size) {
        return CFS_BS_EWORKSPACE;
    }

    status = cfs_bf_symlink_load(store, &entry, storage, workspace);
    if (status == CFS_BS_OK) {
        memcpy(bytes, workspace->bytes, (size_t)entry.inode.size);
        *out_length = (uint32_t)entry.inode.size;
    }

    return status;
}
