/* SPDX-License-Identifier: ISC
 * branch_assemble_inode.c -- base-checked inode groups and index installation.
 */
#include "branch_assemble_internal.h"
#include "branch_file_internal.h"
#include "branch_fs_internal.h"
#include "branch_symlink_internal.h"

#include <string.h>

static cfs_bs_child_t retained(cfs_bs_ref_t ref) {
    return (cfs_bs_child_t){.kind = CFS_BS_CHILD_RETAINED, .value.retained = ref};
}

static cfs_bs_child_t empty(void) {
    return retained((cfs_bs_ref_t){CFS_BS_BLOCK_NONE, 0u, 0u});
}

static int metadata(cfs_bf_inode_t inode, uint8_t *bytes) {
    if (inode.links == 0u) {
        return CFS_BS_ECORRUPT;
    }

    return cfs_bf_inode_frame(inode,
        inode.type != CFS_BF_REGULAR && inode.size != 0u, bytes);
}

static int start_inode(cfs_bs_store_t *store, cfs_bf_assemble_t *a,
    const cfs_bf_change_t *change, cfs_bs_workspace_t *storage,
    cfs_bf_assemble_workspace_t *w) {
    a->current = change->id;
    a->active = 1u;
    a->content_seen = 0u;
    a->content = empty().value.retained;
    a->physical_before = 0u;
    a->result_present = 0u;
    memset(&a->original, 0, sizeof(a->original));
    int status = cfs_bf_get(store, &a->base, change->id, &a->original,
        storage, &w->namespace);
    if (status == CFS_BS_ENOENT) {
        status = CFS_BS_OK;
    } else if (status == CFS_BS_OK) {
        a->physical_before = 1u;
        a->result_present = a->original.inode.links != 0u;
        if (a->result_present) {
            a->result = a->original.inode;
            a->content = a->original.tree;
        }
    }

    if (status != CFS_BS_OK) {
        return status;
    }

    if (change->kind != CFS_BF_CHANGE_INODE) {
        return a->result_present ? CFS_BS_OK : CFS_BS_ECORRUPT;
    }

    if ((change->before != NULL) != (a->result_present != 0u) ||
        (change->before == NULL && change->after == NULL)) {
        return CFS_BS_ECONFLICT;
    }

    if (change->before) {
        status = metadata(*change->before, w->metadata);
        if (status == CFS_BS_OK) {
            status = metadata(a->original.inode, w->namespace.inode_bytes);
        }

        if (status != CFS_BS_OK || memcmp(w->metadata,
                w->namespace.inode_bytes, CFS_BF_INODE_BYTES) != 0) {
            return CFS_BS_ECONFLICT;
        }
    }

    a->result_present = change->after != NULL;
    if (a->result_present) {
        a->result = *change->after;
        status = metadata(a->result, w->metadata);
        if (status != CFS_BS_OK || (a->physical_before &&
                a->result.type != a->original.inode.type) ||
            change->id.birth > a->header.result ||
            a->result.parent.birth > a->header.result) {
            return CFS_BS_ECORRUPT;
        }
    }

    cfs_bs_child_t content = retained(a->content);
    if (a->result_present && a->result.type == CFS_BF_REGULAR) {
        uint64_t before = change->before == NULL ? 0u : a->original.inode.size;
        status = cfs_bf_file_resize(store, a->content, before, a->result.size,
            &content, storage, &w->namespace, &w->file);
    } else if (!a->result_present && a->original.inode.type != CFS_BF_DIRECTORY) {
        content = empty();
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_assemble_save(store, a, retained(a->inodes),
            retained(a->orphans), content, storage, w);
    }

    return status;
}

static int local_content(cfs_bs_store_t *store, cfs_bf_assemble_t *a,
    cfs_bs_workspace_t *storage, cfs_bf_assemble_workspace_t *w) {
    uint16_t type = a->result_present ? a->result.type : a->original.inode.type;
    if (type == CFS_BF_DIRECTORY) {
        uint64_t expected = a->result_present ? a->result.size : 0u;
        if (a->content.block == CFS_BS_BLOCK_NONE) {
            return expected == 0u ? CFS_BS_OK : CFS_BS_ECORRUPT;
        }

        cfs_bt_read_frame_t *frame = &w->namespace.read.path[0];
        uint32_t count;
        uint32_t length;
        int status = cfs_bs_object_read(store, a->content,
            w->namespace.read.physical_children,
            CFS_BT_MAX_RECORDS, &count, frame->payload, sizeof(frame->payload),
            &length, storage);
        if (status == CFS_BS_OK) {
            status = cfs_bt_node_decode(frame->payload, length, count, 1,
                &frame->node);
        }

        if (status == CFS_BS_OK && frame->node.item_count != expected) {
            status = CFS_BS_ECORRUPT;
        }

        return status;
    }

    if (type == CFS_BF_SYMLINK && a->result_present) {
        cfs_bf_entry_t entry = {.id = a->current, .inode = a->result,
            .tree = a->content};
        return cfs_bf_symlink_load(store, &entry, storage, &w->symlink);
    }

    return CFS_BS_OK;
}

int cfs_bf_assemble_flush(cfs_bs_store_t *store, cfs_bf_assemble_t *a,
    cfs_bs_workspace_t *storage, cfs_bf_assemble_workspace_t *w) {
    if (!a->active) {
        return CFS_BS_OK;
    }

    int status = local_content(store, a, storage, w);
    cfs_bs_child_t inodes;
    cfs_bs_child_t orphans = retained(a->orphans);
    cfs_bt_stage_reset(&w->namespace.stage);
    if (status == CFS_BS_OK && a->result_present) {
        status = cfs_bf_edit_inode(store, retained(a->inodes), a->current,
            a->result, retained(a->content),
            a->physical_before ? CFS_BT_REPLACE : CFS_BT_INSERT,
            &inodes, storage, &w->namespace);
    } else if (status == CFS_BS_OK) {
        (void)cfs_bf_id_key(a->current, w->namespace.key);
        cfs_bt_change_t change = {.kind = CFS_BT_DELETE,
            .key = w->namespace.key, .key_length = CFS_BF_ID_BYTES};
        status = cfs_bt_edit(store, &w->namespace.stage, retained(a->inodes),
            change, &inodes, storage, &w->namespace.edit);
    }

    if (status == CFS_BS_OK && a->physical_before &&
        a->original.inode.links == 0u && a->result_present) {
        (void)cfs_bf_id_key(a->current, w->namespace.key);
        status = cfs_bt_lookup(store, a->base.orphans, w->namespace.key,
            CFS_BF_ID_BYTES, &w->namespace.value, storage, &w->namespace.read);
        if (status == CFS_BS_OK && (w->namespace.value.length != 0u ||
                w->namespace.value.child.block != CFS_BS_BLOCK_NONE)) {
            status = CFS_BS_ECORRUPT;
        }

        if (status == CFS_BS_OK) {
            cfs_bt_change_t change = {.kind = CFS_BT_DELETE,
                .key = w->namespace.key, .key_length = CFS_BF_ID_BYTES};
            status = cfs_bt_edit(store, &w->namespace.stage, orphans, change,
                &orphans, storage, &w->namespace.edit);
        }
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_assemble_save(store, a, inodes, orphans, empty(), storage, w);
    }

    if (status == CFS_BS_OK) {
        a->active = 0u;
    }

    return status;
}

int cfs_bf_assemble_change(cfs_bs_store_t *store, cfs_bf_assemble_t *a,
    const cfs_bf_change_t *change, cfs_bs_workspace_t *storage,
    cfs_bf_assemble_workspace_t *w) {
    int status = change == NULL || !cfs_bf_id_valid(change->id) ||
        change->kind < CFS_BF_CHANGE_INODE || change->kind > CFS_BF_CHANGE_SYMLINK ?
        CFS_BS_EINVAL : cfs_bf_assemble_prepare(store, a, storage, w);
    if (status != CFS_BS_OK) {
        return cfs_bf_assemble_fail(a, status);
    }

    if (a->events == UINT64_MAX) {
        return cfs_bf_assemble_fail(a, CFS_BS_EOVERFLOW);
    }

    int same = cfs_bf_id_equal(a->current, change->id);
    if (change->id.birth < a->current.birth ||
        (change->id.birth == a->current.birth &&
            change->id.ordinal < a->current.ordinal) ||
        (same && (!a->active || change->kind == CFS_BF_CHANGE_INODE))) {
        return cfs_bf_assemble_fail(a, CFS_BS_EINVAL);
    }

    if (!same) {
        status = cfs_bf_assemble_flush(store, a, storage, w);
        if (status == CFS_BS_OK) {
            cfs_bt_stage_reset(&w->namespace.stage);
            status = start_inode(store, a, change, storage, w);
        }
    }

    if (status == CFS_BS_OK && change->kind != CFS_BF_CHANGE_INODE) {
        cfs_bt_stage_reset(&w->namespace.stage);
        status = cfs_bf_assemble_content(store, a, change, storage, w);
    }

    if (status == CFS_BS_OK) {
        a->events++;
    }

    return cfs_bf_assemble_fail(a, status);
}
