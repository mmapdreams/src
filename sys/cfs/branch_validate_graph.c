/* SPDX-License-Identifier: ISC
 * branch_validate_graph.c -- link census and linear directory reachability.
 */
#include "branch_validate_internal.h"

static int parents(cfs_bf_validate_workspace_t *w, size_t start) {
    size_t index = start;
    while (w->inodes[index].color == 0u) {
        if (cfs_bf_validate_step(w) != CFS_BS_OK) {
            return w->failure;
        }

        w->inodes[index].color = 1u;
        w->counts.parent_steps++;
        index = w->inodes[index].parent_index;
    }

    if (w->inodes[index].color == 1u) {
        return CFS_BS_ECORRUPT;
    }

    index = start;
    while (w->inodes[index].color == 1u) {
        if (cfs_bf_validate_step(w) != CFS_BS_OK) {
            return w->failure;
        }

        w->inodes[index].color = 2u;
        w->counts.parent_steps++;
        index = w->inodes[index].parent_index;
    }

    return CFS_BS_OK;
}

int cfs_bf_validate_graph(cfs_bf_validate_workspace_t *w) {
    for (size_t i = 0u; i < w->count; i++) {
        if (cfs_bf_validate_checkpoint(w) != CFS_BS_OK) {
            return w->failure;
        }

        cfs_bf_validate_inode_t *inode = &w->inodes[i];
        if ((inode->links == 0u) != (inode->orphan != 0u)) {
            return CFS_BS_ECORRUPT;
        }

        if (inode->type != CFS_BF_DIRECTORY || inode->links == 0u) {
            if (inode->links != inode->incoming) {
                return CFS_BS_ECORRUPT;
            }
        } else if (inode->links != inode->subdirectories + 2u ||
            inode->incoming != (i == w->root_index ? 0u : 1u) ||
            (i != w->root_index && inode->parent_index >= w->count)) {
            return CFS_BS_ECORRUPT;
        }
    }

    w->inodes[w->root_index].color = 2u;
    for (size_t i = 0u; i < w->count; i++) {
        if (cfs_bf_validate_checkpoint(w) != CFS_BS_OK) {
            return w->failure;
        }

        if (w->inodes[i].type == CFS_BF_DIRECTORY &&
            w->inodes[i].links != 0u) {
            int status = parents(w, i);
            if (status != CFS_BS_OK) {
                return status;
            }
        }
    }

    return CFS_BS_OK;
}
