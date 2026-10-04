/* SPDX-License-Identifier: ISC
 * branch_validate_content.c -- full bindings and materialized content audit.
 */
#include "branch_validate_internal.h"
#include "branch_file_internal.h"
#include "branch_symlink_internal.h"

static int binding(void *opaque, const cfs_bt_record_t *record,
    const cfs_bs_ref_t *child) {
    cfs_bf_validate_workspace_t *w = opaque;
    cfs_bf_id_t id;
    int status = cfs_bf_validate_step(w);
    if (status == CFS_BS_OK && (child != NULL ||
            !cfs_bf_name_valid(record->key, record->key_length))) {
        status = CFS_BS_ECORRUPT;
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_dirent_decode(record->value, record->value_length, &id);
    }

    size_t index = status == CFS_BS_OK ? cfs_bf_validate_find(w, id) : SIZE_MAX;
    if (status == CFS_BS_OK && (index == SIZE_MAX ||
            w->inodes[index].links == 0u || index == w->root_index)) {
        status = CFS_BS_ECORRUPT;
    }

    if (status == CFS_BS_OK) {
        cfs_bf_validate_inode_t *target = &w->inodes[index];
        cfs_bf_validate_inode_t *directory = &w->inodes[w->current];
        if (target->type == CFS_BF_DIRECTORY) {
            if (target->incoming != 0u ||
                !cfs_bf_id_equal(target->parent, directory->id)) {
                status = CFS_BS_ECORRUPT;
            } else {
                target->parent_index = w->current;
                directory->subdirectories++;
            }
        }

        if (status == CFS_BS_OK) {
            target->incoming++;
            w->counts.bindings++;
        }
    }

    w->failure = status;
    return status == CFS_BS_OK ? CFS_BT_SCAN_CONTINUE : CFS_BT_SCAN_STOP;
}

static int regular(cfs_bs_store_t *store, cfs_bf_validate_inode_t *inode,
    cfs_bs_workspace_t *storage, cfs_bf_validate_workspace_t *w) {
    if (inode->content.block == CFS_BS_BLOCK_NONE) {
        return CFS_BS_OK;
    }

    cfs_bf_file_workspace_t *file = &w->file;
    cfs_bs_child_t root = {.kind = CFS_BS_CHILD_RETAINED,
        .value.retained = inode->content};
    int status = cfs_bf_validate_step(w);
    if (status == CFS_BS_OK) {
        status = cfs_bf_file_load(store, NULL, root, -1, 0u, inode->size, 1,
            &file->path[0], storage, file);
        w->counts.file_nodes++;
    }

    uint32_t depth = 0u;
    while (status == CFS_BS_OK) {
        cfs_bf_file_frame_t *frame = &file->path[depth];
        if (frame->slot == CFS_BF_RADIX_SLOTS) {
            if (depth == 0u) {
                break;
            }

            depth--;
            continue;
        }

        uint16_t slot = frame->slot++;
        cfs_bs_child_t child = frame->children[slot];
        if (!cfs_bf_file_present(child)) {
            continue;
        }

        status = cfs_bf_validate_step(w);
        if (status != CFS_BS_OK) {
            break;
        }

        uint64_t base = frame->base +
            slot * (UINT64_C(1) << (7u * frame->level));
        if (frame->level == 0u) {
            status = cfs_bf_file_chunk(store, NULL, child, base, inode->size,
                storage, file);
            w->counts.data_chunks++;
        } else {
            status = cfs_bf_file_load(store, NULL, child, frame->level - 1u,
                base, inode->size, 0, &file->path[depth + 1u], storage, file);
            w->counts.file_nodes++;
            depth++;
        }
    }

    return status;
}

int cfs_bf_validate_content(cfs_bs_store_t *store, cfs_bs_workspace_t *storage,
    cfs_bf_validate_workspace_t *w) {
    cfs_bf_validate_inode_t *inode = &w->inodes[w->current];
    if (inode->type == CFS_BF_REGULAR) {
        return regular(store, inode, storage, w);
    }

    if (inode->type == CFS_BF_SYMLINK) {
        int status = cfs_bf_validate_step(w);
        if (status == CFS_BS_OK) {
            cfs_bf_entry_t entry = {.id = inode->id, .tree = inode->content,
                .inode = {.type = inode->type, .size = inode->size}};
            status = cfs_bf_symlink_load(store, &entry, storage, &w->symlink);
            w->counts.symbolic_bytes += inode->size;
        }

        return status;
    }

    uint64_t count;
    int status = cfs_bf_validate_scan(store, inode->content, binding,
        &count, storage, w);
    if (status == CFS_BS_OK && count != inode->size) {
        status = CFS_BS_ECORRUPT;
    }

    w->counts.directories++;
    return status;
}
