/* SPDX-License-Identifier: ISC
 * branch_assemble_content.c -- base-relative bindings and pending file contents.
 */
#include "branch_assemble_internal.h"
#include "branch_file_internal.h"
#include "branch_fs_internal.h"
#include "branch_symlink_internal.h"

static cfs_bs_child_t retained(cfs_bs_ref_t ref) {
    return (cfs_bs_child_t){.kind = CFS_BS_CHILD_RETAINED, .value.retained = ref};
}

static int entry(cfs_bs_store_t *store, cfs_bf_assemble_t *a,
    const cfs_bf_change_t *change, cfs_bs_child_t *out,
    cfs_bs_workspace_t *storage, cfs_bf_assemble_workspace_t *w) {
    if (change->length > CFS_BF_NAME_MAX ||
        !cfs_bf_name_valid(change->bytes, (uint16_t)change->length) ||
        (change->before_id.birth != 0u && !cfs_bf_id_valid(change->before_id)) ||
        (change->after_id.birth != 0u && !cfs_bf_id_valid(change->after_id)) ||
        cfs_bf_id_equal(change->before_id, change->after_id) ||
        change->before_id.birth > a->header.base ||
        change->after_id.birth > a->header.result ||
        (!a->result_present && change->after_id.birth != 0u)) {
        return CFS_BS_ECORRUPT;
    }

    cfs_bf_id_t before = {0u, 0u};
    int status = CFS_BS_OK;
    if (a->physical_before && a->original.inode.links != 0u) {
        cfs_bf_entry_t target;
        status = cfs_bf_lookup(store, &a->base, a->current,
            change->bytes, (uint16_t)change->length, &target,
            storage, &w->namespace);
        if (status == CFS_BS_ENOENT) {
            status = CFS_BS_OK;
        } else if (status == CFS_BS_OK) {
            before = target.id;
        }
    }

    if (status != CFS_BS_OK) {
        return status;
    }

    if (!cfs_bf_id_equal(before, change->before_id)) {
        return CFS_BS_ECONFLICT;
    }

    cfs_bt_change_t edit = {.key = change->bytes,
        .key_length = (uint16_t)change->length, .kind = CFS_BT_DELETE};
    if (change->after_id.birth != 0u) {
        (void)cfs_bf_dirent_frame(change->after_id, w->namespace.dirent);
        edit.kind = before.birth == 0u ? CFS_BT_INSERT : CFS_BT_REPLACE;
        edit.value = w->namespace.dirent;
        edit.value_length = CFS_BF_ID_BYTES;
    }

    return cfs_bt_edit(store, &w->namespace.stage, retained(a->content), edit,
        out, storage, &w->namespace.edit);
}

int cfs_bf_assemble_content(cfs_bs_store_t *store, cfs_bf_assemble_t *a,
    const cfs_bf_change_t *change, cfs_bs_workspace_t *storage,
    cfs_bf_assemble_workspace_t *w) {
    uint16_t type = a->result_present ? a->result.type : a->original.inode.type;
    uint16_t needed = change->kind == CFS_BF_CHANGE_ENTRY ? CFS_BF_DIRECTORY :
        change->kind == CFS_BF_CHANGE_DATA ? CFS_BF_REGULAR : CFS_BF_SYMLINK;
    if (type != needed || (needed != CFS_BF_DIRECTORY && !a->result_present) ||
        change->bytes == NULL || change->length == 0u) {
        return CFS_BS_ECORRUPT;
    }

    cfs_bs_child_t content;
    int status;
    if (needed == CFS_BF_DIRECTORY) {
        status = entry(store, a, change, &content, storage, w);
    } else if (needed == CFS_BF_REGULAR) {
        if (change->length > CFS_BF_IO_MAX ||
            change->offset > a->result.size ||
            change->length > a->result.size - change->offset) {
            return CFS_BS_ECORRUPT;
        }

        cfs_bf_file_request_t request = {.root = retained(a->content),
            .old_size = a->result.size, .new_size = a->result.size,
            .offset = change->offset, .bytes = change->bytes,
            .length = change->length, .kind = CFS_BF_FILE_WRITE};
        status = cfs_bf_file_walk(store, &w->namespace.stage,
            &w->namespace.stage, request, &content, storage, &w->file);
    } else {
        if (change->length != a->result.size || a->content_seen) {
            return CFS_BS_ECORRUPT;
        }

        status = cfs_bf_symlink_stage(&w->namespace.stage, change->bytes,
            change->length, &content);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_assemble_save(store, a, retained(a->inodes),
            retained(a->orphans), content, storage, w);
    }

    if (status == CFS_BS_OK) {
        a->content_seen = 1u;
    }

    return status;
}
