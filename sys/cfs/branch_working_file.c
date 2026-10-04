/* SPDX-License-Identifier: ISC
 * branch_working_file.c -- coherent regular-file observations and edits.
 */
#include "branch_working_internal.h"

#include "branch_fs_internal.h"

static cfs_bs_child_t retained(cfs_bs_ref_t ref) {
    return (cfs_bs_child_t){.kind = CFS_BS_CHILD_RETAINED,
        .value.retained = ref};
}

static int regular(const cfs_bw_slot_t *slot) {
    if (slot->entry.inode.type == CFS_BF_REGULAR) {
        return CFS_BS_OK;
    }

    return slot->entry.inode.type == CFS_BF_DIRECTORY ?
        CFS_BS_EISDIR : CFS_BS_EINVAL;
}

int cfs_bw_stat(cfs_bw_t *owner, uint64_t branch, cfs_bf_id_t id,
    cfs_bw_stat_t *out) {
    if (!out) {
        return CFS_BS_EINVAL;
    }

    cfs_bw_slot_t *slot;
    int working;
    int status = cfs_bw_observe(owner, branch, id, &slot, &working);
    if (status == CFS_BS_OK) {
        (void)working;
        *out = (cfs_bw_stat_t){.id = slot->entry.id,
            .inode = slot->entry.inode, .revision = slot->branch.revision,
            .sequence = owner->accepted_sequence};
    }

    return status;
}

int cfs_bw_read(cfs_bw_t *owner, uint64_t branch, cfs_bf_id_t id,
    uint64_t offset, void *bytes, uint32_t length, uint32_t *out_length) {
    if (out_length == NULL || (length != 0u && bytes == NULL)) {
        return CFS_BS_EINVAL;
    }

    if (length > CFS_BF_IO_MAX) {
        return CFS_BS_EWORKSPACE;
    }

    if (offset > INT64_MAX || length > INT64_MAX - offset) {
        return CFS_BS_EOVERFLOW;
    }

    cfs_bw_slot_t *slot;
    int working;
    int status = cfs_bw_observe(owner, branch, id, &slot, &working);
    if (status == CFS_BS_OK) {
        status = regular(slot);
    }

    if (status == CFS_BS_OK) {
        cfs_bw_slot_t *scratch = working ?
            &owner->workspace->slots[1u - owner->active] : slot;
        const cfs_bt_stage_t *stage = working ? &slot->ns.stage : NULL;
        status = cfs_bf_file_content_read(owner->store, stage,
            slot->content, offset, bytes, length, out_length,
            &scratch->storage, &owner->workspace->file);
    }

    return cfs_bw_latch(owner, status);
}

static int binding_equal(const cfs_bw_t *owner, uint64_t branch,
    cfs_bf_id_t id) {
    const cfs_bw_slot_t *slot = &owner->workspace->slots[owner->active];
    return owner->dirty != 0u && slot->branch.id == branch &&
        cfs_bf_id_equal(slot->entry.id, id);
}

static int pressure_before(const cfs_bw_t *owner, uint64_t branch,
    cfs_bf_id_t id, uint64_t bytes) {
    if (owner->dirty == 0u) {
        return 0;
    }

    const cfs_bw_slot_t *slot = &owner->workspace->slots[owner->active];
    if (!binding_equal(owner, branch, id) ||
        owner->batch_operations >= CFS_BW_BATCH_OPERATIONS ||
        owner->batch_bytes >= CFS_BW_BATCH_BYTES ||
        slot->ns.stage.count >= CFS_BW_BATCH_OBJECTS) {
        return 1;
    }

    if (bytes > CFS_BW_BATCH_BYTES) {
        return 1;
    }

    return bytes != 0u &&
        owner->batch_bytes > CFS_BW_BATCH_BYTES - bytes;
}

static int candidate_begin(cfs_bw_t *owner, cfs_bw_slot_t *source,
    int working, uint32_t *out_index, cfs_bw_slot_t **out) {
    uint32_t index = working ? 1u - owner->active :
        (uint32_t)(source - owner->workspace->slots);
    cfs_bw_slot_t *slot = &owner->workspace->slots[index];
    *out = slot;
    int status;
    if (working) {
        cfs_bt_stage_discard(&slot->ns.stage);
        status = cfs_bt_stage_clone(&source->ns.stage, &slot->ns.stage);
        if (status == CFS_BS_OK) {
            owner->work.objects_cloned += source->ns.stage.count;
            slot->branch = source->branch;
            slot->view = source->view;
            slot->entry = source->entry;
            slot->content = source->content;
        }
    } else {
        cfs_bt_stage_reset(&slot->ns.stage);
        status = CFS_BS_OK;
    }

    if (status == CFS_BS_OK) {
        *out_index = index;
    }

    return status;
}

static int complete(cfs_bw_t *owner, cfs_bw_slot_t *slot,
    uint32_t *out_root) {
    cfs_bs_child_t inodes;
    int status = cfs_bf_edit_inode(owner->store, retained(slot->view.inodes),
        slot->entry.id, slot->entry.inode, slot->content.root,
        CFS_BT_REPLACE, &inodes, &slot->storage, &slot->ns);
    if (status == CFS_BS_OK) {
        status = cfs_bf_stage_root(owner->store, slot->view, inodes,
            retained(slot->view.orphans), out_root, &slot->ns);
    }

    return status;
}

enum { CFS_BW_WRITE = 1, CFS_BW_TRUNCATE = 2 };

typedef struct cfs_bw_change {
    const void *bytes;
    uint64_t offset;
    uint64_t size;
    cfs_bf_time_t time;
    uint32_t length;
    uint32_t kind;
} cfs_bw_change_t;

static int apply_change(cfs_bw_t *owner, cfs_bw_slot_t *source,
    int working, cfs_bw_change_t change, uint64_t *out_sequence) {
    uint32_t index;
    cfs_bw_slot_t *slot;
    int status = candidate_begin(owner, source, working, &index, &slot);
    cfs_bf_file_state_t result;
    if (status == CFS_BS_OK && change.kind == CFS_BW_WRITE) {
        status = cfs_bf_file_content_write(owner->store, &slot->ns.stage,
            slot->content, change.offset, change.bytes, change.length,
            &result, &slot->storage, &owner->workspace->file);
    } else if (status == CFS_BS_OK) {
        status = cfs_bf_file_content_truncate(owner->store, &slot->ns.stage,
            slot->content, change.size, &result, &slot->storage,
            &owner->workspace->file);
    }

    uint32_t root;
    if (status == CFS_BS_OK) {
        slot->content = result;
        slot->entry.inode.size = result.size;
        slot->entry.inode.mtime = change.time;
        slot->entry.inode.ctime = change.time;
        status = complete(owner, slot, &root);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bw_admit(owner, index, root,
            change.kind == CFS_BW_WRITE ? change.length : 0u,
            out_sequence);
    }

    if (status != CFS_BS_OK) {
        cfs_bt_stage_discard(&slot->ns.stage);
    }

    return cfs_bw_latch(owner, status);
}

static int reload(cfs_bw_t *owner, uint64_t branch, cfs_bf_id_t id,
    cfs_bw_slot_t **slot, int *working) {
    return cfs_bw_observe(owner, branch, id, slot, working);
}

static int changed_operation(cfs_bw_t *owner, uint64_t branch,
    cfs_bf_id_t id, cfs_bw_slot_t *source, int working,
    cfs_bw_change_t change, uint64_t *out_sequence) {
    uint64_t bytes = change.kind == CFS_BW_WRITE ? change.length : 0u;
    if (owner->accepted_sequence == UINT64_MAX) {
        return CFS_BS_EOVERFLOW;
    }

    if (pressure_before(owner, branch, id, bytes)) {
        int status = cfs_bw_checkpoint_internal(owner,
            owner->accepted_sequence, 1);
        if (status != CFS_BS_OK) {
            return status;
        }

        status = reload(owner, branch, id, &source, &working);
        if (status != CFS_BS_OK) {
            return status;
        }
    }

    int had_batch = owner->dirty != 0u;
    int status = apply_change(owner, source, working, change, out_sequence);
    if (had_batch && (status == CFS_BS_ENOSPC ||
            status == CFS_BS_EWORKSPACE)) {
        status = cfs_bw_checkpoint_internal(owner,
            owner->accepted_sequence, 1);
        if (status == CFS_BS_OK) {
            status = reload(owner, branch, id, &source, &working);
        }

        if (status == CFS_BS_OK) {
            status = apply_change(owner, source, working, change,
                out_sequence);
        }
    }

    return status;
}

int cfs_bw_write(cfs_bw_t *owner, uint64_t branch, cfs_bf_id_t id,
    uint64_t offset, const void *bytes, uint32_t length, int append,
    cfs_bf_time_t time, uint64_t *out_sequence) {
    int status = cfs_bw_ready(owner);
    if (status != CFS_BS_OK) {
        return status;
    }

    if (out_sequence == NULL || (length != 0u && bytes == NULL) ||
        (append != 0 && append != 1) ||
        time.nanoseconds >= 1000000000u) {
        return CFS_BS_EINVAL;
    }

    if (length > CFS_BF_IO_MAX) {
        return CFS_BS_EWORKSPACE;
    }

    if (!append && (offset > INT64_MAX || length > INT64_MAX - offset)) {
        return CFS_BS_EOVERFLOW;
    }

    cfs_bw_slot_t *source;
    int working;
    status = cfs_bw_observe(owner, branch, id, &source, &working);
    if (status == CFS_BS_OK) {
        status = regular(source);
    }

    if (status != CFS_BS_OK) {
        return status;
    }

    if (append) {
        offset = source->content.size;
        if (length > INT64_MAX - offset) {
            return CFS_BS_EOVERFLOW;
        }
    }

    if (length == 0u) {
        *out_sequence = owner->accepted_sequence;
        return CFS_BS_OK;
    }

    cfs_bw_change_t change = {.bytes = bytes, .offset = offset,
        .time = time, .length = length, .kind = CFS_BW_WRITE};
    return changed_operation(owner, branch, id, source, working, change,
        out_sequence);
}

int cfs_bw_truncate(cfs_bw_t *owner, uint64_t branch, cfs_bf_id_t id,
    uint64_t size, cfs_bf_time_t time, uint64_t *out_sequence) {
    int status = cfs_bw_ready(owner);
    if (status != CFS_BS_OK) {
        return status;
    }

    if (out_sequence == NULL || time.nanoseconds >= 1000000000u) {
        return CFS_BS_EINVAL;
    }

    if (size > INT64_MAX) {
        return CFS_BS_EOVERFLOW;
    }

    cfs_bw_slot_t *source;
    int working;
    status = cfs_bw_observe(owner, branch, id, &source, &working);
    if (status == CFS_BS_OK) {
        status = regular(source);
    }

    if (status != CFS_BS_OK) {
        return status;
    }

    if (size == source->content.size) {
        *out_sequence = owner->accepted_sequence;
        return CFS_BS_OK;
    }

    cfs_bw_change_t change = {.size = size, .time = time,
        .kind = CFS_BW_TRUNCATE};
    return changed_operation(owner, branch, id, source, working, change,
        out_sequence);
}
