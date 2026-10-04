/* SPDX-License-Identifier: ISC
 * branch_path.c -- branch-confined ordered component and symbolic resolution.
 */
#include "branch_path.h"
#include "branch_fs_internal.h"
#include "branch_symlink_internal.h"

#include <string.h>

static int dot(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t current, int parent, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *ns, cfs_bf_path_workspace_t *w) {
    int status = cfs_bf_directory(store, view, current, &w->entry, storage, ns);
    if (status == CFS_BS_OK && parent &&
        !cfs_bf_id_equal(current, view->root_id)) {
        status = cfs_bf_directory(store, view, w->entry.inode.parent,
            &w->entry, storage, ns);
        if (status == CFS_BS_ENOENT || status == CFS_BS_ENOTDIR) {
            return CFS_BS_ECORRUPT;
        }
    }

    return status;
}

static int resolve(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t current, uint32_t length, int follow_final,
    cfs_bs_workspace_t *storage, cfs_bf_workspace_t *ns,
    cfs_bf_path_workspace_t *w) {
    uint32_t position = 0u;
    uint32_t followed = 0u;
    int status = cfs_bf_directory(store, view, current, &w->entry, storage, ns);
    while (status == CFS_BS_OK && position < length) {
        if (w->pending[position] == '/') {
            position++;
            continue;
        }

        uint32_t end = position;
        while (end < length && w->pending[end] != '/') {
            end++;
        }

        uint32_t count = end - position;
        if (count > CFS_BF_NAME_MAX) {
            return CFS_BS_ENAMETOOLONG;
        }

        const uint8_t *name = w->pending + position;
        if ((count == 1u && name[0] == '.') ||
            (count == 2u && name[0] == '.' && name[1] == '.')) {
            status = dot(store, view, current, count == 2u, storage, ns, w);
        } else {
            status = cfs_bf_lookup(store, view, current, name, (uint16_t)count,
                &w->entry, storage, ns);
        }

        if (status != CFS_BS_OK) {
            return status;
        }

        if (w->entry.inode.type == CFS_BF_SYMLINK &&
            (follow_final || end < length)) {
            if (followed++ == CFS_BF_FOLLOW_MAX) {
                return CFS_BS_ELOOP;
            }

            status = cfs_bf_symlink_load(store, &w->entry, storage, &w->target);
            if (status != CFS_BS_OK) {
                return status;
            }

            uint32_t target_length = (uint32_t)w->entry.inode.size;
            uint32_t suffix = length - end;
            if (target_length > CFS_BF_PATH_MAX - suffix) {
                return CFS_BS_ENAMETOOLONG;
            }

            memmove(w->pending + target_length, w->pending + end, suffix);
            memcpy(w->pending, w->target.bytes, target_length);
            length = target_length + suffix;
            position = 0u;
            if (w->pending[0] == '/') {
                current = view->root_id;
            }

            status = cfs_bf_directory(store, view, current, &w->entry, storage, ns);
        } else {
            current = w->entry.id;
            position = end;
        }
    }

    if (status == CFS_BS_OK && w->pending[length - 1u] == '/' &&
        w->entry.inode.type != CFS_BF_DIRECTORY) {
        return CFS_BS_ENOTDIR;
    }

    return status;
}

int cfs_bf_resolve(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t start, const void *path, uint32_t length, int follow_final,
    cfs_bf_entry_t *out, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *namespace_workspace, cfs_bf_path_workspace_t *workspace) {
    int status = cfs_bf_prepare(store, storage, namespace_workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    if (view == NULL || path == NULL || out == NULL || workspace == NULL ||
        length == 0u || (follow_final != 0 && follow_final != 1)) {
        return CFS_BS_EINVAL;
    }

    if (length > CFS_BF_PATH_MAX) {
        return CFS_BS_ENAMETOOLONG;
    }

    const uint8_t *bytes = path;
    for (uint32_t i = 0u; i < length; i++) {
        if (bytes[i] == 0u) {
            return CFS_BS_EINVAL;
        }
    }

    memcpy(workspace->pending, path, length);
    cfs_bf_id_t current = workspace->pending[0] == '/' ? view->root_id : start;
    status = resolve(store, view, current, length, follow_final, storage,
        namespace_workspace, workspace);
    if (status == CFS_BS_OK) {
        *out = workspace->entry;
    }

    return status;
}
