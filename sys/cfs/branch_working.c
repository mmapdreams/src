/* SPDX-License-Identifier: ISC
 * branch_working.c -- image-wide working admission and durable frontiers.
 */
#include "branch_working_internal.h"
#include "branch_store_staging.h"

#include <string.h>

int cfs_bw_owner_valid(const cfs_bw_t *owner) {
    return owner != NULL && owner->identity == owner && owner->store != NULL &&
        owner->workspace != NULL && owner->active < 2u && owner->dirty <= 1u &&
        owner->external <= 1u && owner->durable_sequence <= owner->accepted_sequence;
}

int cfs_bw_latch(cfs_bw_t *owner, int status) {
    if (status != CFS_BS_OK && cfs_bw_owner_valid(owner) &&
        owner->store->poisoned != 0u &&
        owner->delayed_error == CFS_BS_OK) {
        owner->delayed_error = status;
    }

    return status;
}

int cfs_bw_ready(cfs_bw_t *owner) {
    if (!cfs_bw_owner_valid(owner)) {
        return CFS_BS_EINVAL;
    }

    if (owner->external != 0u) {
        return CFS_BS_EBUSY;
    }

    if (owner->delayed_error != CFS_BS_OK) {
        return owner->delayed_error;
    }

    if (owner->store->poisoned != 0u) {
        return cfs_bw_latch(owner, CFS_BS_EIO);
    }

    return CFS_BS_OK;
}

static uint32_t scratch_slot(const cfs_bw_t *owner) {
    return owner->dirty != 0u ? 1u - owner->active : owner->active;
}

static int same_working(const cfs_bw_t *owner, uint64_t branch,
    cfs_bf_id_t id) {
    const cfs_bw_slot_t *slot = &owner->workspace->slots[owner->active];
    return owner->dirty != 0u && slot->branch.id == branch &&
        cfs_bf_id_equal(slot->entry.id, id);
}

int cfs_bw_observe(cfs_bw_t *owner, uint64_t branch, cfs_bf_id_t id,
    cfs_bw_slot_t **out, int *working) {
    int status = cfs_bw_ready(owner);
    if (status != CFS_BS_OK) {
        return status;
    }

    if (out == NULL || working == NULL || branch == 0u ||
        !cfs_bf_id_valid(id)) {
        return CFS_BS_EINVAL;
    }

    if (same_working(owner, branch, id)) {
        *out = &owner->workspace->slots[owner->active];
        *working = 1;
        return CFS_BS_OK;
    }

    cfs_bw_slot_t *slot = &owner->workspace->slots[scratch_slot(owner)];
    status = cfs_bs_branch_get(owner->store, branch, &slot->branch,
        &slot->storage);
    if (status == CFS_BS_OK) {
        status = cfs_bf_read(owner->store, slot->branch.root,
            slot->branch.revision, &slot->view, &slot->storage, &slot->ns);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_get(owner->store, &slot->view, id, &slot->entry,
            &slot->storage, &slot->ns);
    }

    if (status == CFS_BS_OK) {
        slot->content = (cfs_bf_file_state_t){
            .root = {.kind = CFS_BS_CHILD_RETAINED,
                .value.retained = slot->entry.tree},
            .size = slot->entry.inode.size};
        *out = slot;
        *working = 0;
    }

    return cfs_bw_latch(owner, status);
}

int cfs_bw_init(cfs_bw_t *owner, cfs_bs_store_t *store,
    cfs_bw_workspace_t *workspace) {
    if (owner == NULL || store == NULL || workspace == NULL) {
        return CFS_BS_EINVAL;
    }

    if (store->poisoned != 0u) {
        return CFS_BS_EPOISON;
    }

    if (store->generation == 0u) {
        return CFS_BS_ECORRUPT;
    }

    if (store->lease_owner) {
        return CFS_BS_EBUSY;
    }

    memset(workspace, 0, sizeof(*workspace));
    cfs_bt_stage_reset(&workspace->slots[0].ns.stage);
    cfs_bt_stage_reset(&workspace->slots[1].ns.stage);
    memset(owner, 0, sizeof(*owner));
    owner->identity = owner;
    owner->store = store;
    owner->workspace = workspace;
    return CFS_BS_OK;
}

static int checkpoint_needed(const cfs_bw_t *owner) {
    if (owner->dirty == 0u) {
        return 0;
    }

    const cfs_bw_slot_t *slot = &owner->workspace->slots[owner->active];
    return owner->batch_operations >= CFS_BW_BATCH_OPERATIONS ||
        owner->batch_bytes >= CFS_BW_BATCH_BYTES ||
        slot->ns.stage.count >= CFS_BW_BATCH_OBJECTS;
}

int cfs_bw_status(const cfs_bw_t *owner, cfs_bw_status_t *out) {
    if (!cfs_bw_owner_valid(owner) || out == NULL) {
        return CFS_BS_EINVAL;
    }

    cfs_bw_status_t status = {.accepted_sequence = owner->accepted_sequence,
        .durable_sequence = owner->durable_sequence,
        .batch_bytes = owner->batch_bytes,
        .batch_operations = owner->batch_operations,
        .dirty = owner->dirty, .external = owner->external,
        .needs_checkpoint = (uint32_t)checkpoint_needed(owner),
        .delayed_error = owner->delayed_error != CFS_BS_OK ?
            owner->delayed_error : owner->store->poisoned != 0u ?
                CFS_BS_EIO : CFS_BS_OK,
        .work = owner->work};
    if (owner->dirty != 0u) {
        const cfs_bw_slot_t *slot = &owner->workspace->slots[owner->active];
        status.branch = slot->branch.id;
        status.inode = slot->entry.id;
        status.stage_objects = slot->ns.stage.count;
    }

    *out = status;
    return CFS_BS_OK;
}

int cfs_bw_borrow(cfs_bw_t *owner, cfs_bw_scratch_t *out) {
    if (!cfs_bw_owner_valid(owner) || out == NULL) {
        return CFS_BS_EINVAL;
    }

    if (owner->delayed_error != CFS_BS_OK) {
        return owner->delayed_error;
    }

    if (owner->store->poisoned != 0u) {
        return cfs_bw_latch(owner, CFS_BS_EIO);
    }

    cfs_bw_slot_t *slot = &owner->workspace->slots[scratch_slot(owner)];
    *out = (cfs_bw_scratch_t){.storage = &slot->storage,
        .ns = &slot->ns, .file = &owner->workspace->file};
    return CFS_BS_OK;
}

int cfs_bw_admit(cfs_bw_t *owner, uint32_t candidate, uint32_t root,
    uint64_t bytes, uint64_t *out_sequence) {
    if (!cfs_bw_owner_valid(owner) || candidate >= 2u ||
        out_sequence == NULL) {
        return CFS_BS_EINVAL;
    }

    if (owner->accepted_sequence == UINT64_MAX) {
        return CFS_BS_EOVERFLOW;
    }

    cfs_bw_slot_t *slot = &owner->workspace->slots[candidate];
    cfs_bt_stage_batch_t batch;
    int status = cfs_bt_stage_compile(&slot->ns.stage, root, &batch);
    if (status != CFS_BS_OK) {
        return status;
    }

    owner->work.admission_attempts++;
    owner->work.admission_objects += batch.count;
    if (owner->dirty != 0u) {
        cfs_bw_slot_t *current = &owner->workspace->slots[owner->active];
        status = cfs_bs_branch_construct_replace_prepare(owner->store,
            &current->token, batch.plans, batch.count, &slot->token,
            &slot->storage);
    } else {
        status = cfs_bs_branch_construct_prepare(owner->store,
            slot->branch.id, slot->branch.revision, batch.plans, batch.count,
            &slot->token, &slot->storage);

        /* Shared children on scattered rows: acquire them durably in bounded
         * donor transactions, then admit the same proposal. */
        if (status == CFS_BS_EWORKSPACE &&
            cfs_bs_branch_unshare(owner->store, slot->branch.id,
                slot->branch.revision, batch.plans, batch.count,
                &slot->storage) == CFS_BS_OK) {
            status = cfs_bs_branch_construct_prepare(owner->store,
                slot->branch.id, slot->branch.revision, batch.plans,
                batch.count, &slot->token, &slot->storage);
            if (status != CFS_BS_OK) {
                (void)cfs_bs_branch_unshare_cancel(owner->store,
                    slot->branch.id, &slot->storage);
            }
        }
    }

    if (status != CFS_BS_OK) {
        cfs_bt_stage_discard(&slot->ns.stage);
        return cfs_bw_latch(owner, status);
    }

    if (cfs_bt_stage_reopen(&slot->ns.stage) != CFS_BS_OK) {
        __builtin_trap();
    }

    owner->active = candidate;
    owner->dirty = 1u;
    owner->accepted_sequence++;
    owner->batch_operations++;
    owner->batch_bytes += bytes;
    owner->work.accepted_operations++;
    *out_sequence = owner->accepted_sequence;
    return CFS_BS_OK;
}

int cfs_bw_checkpoint_internal(cfs_bw_t *owner, uint64_t through,
    int pressure) {
    int status = cfs_bw_ready(owner);
    if (status != CFS_BS_OK) {
        return status;
    }

    if (through > owner->accepted_sequence) {
        return CFS_BS_EINVAL;
    }

    if (through <= owner->durable_sequence) {
        return CFS_BS_OK;
    }

    if (owner->dirty == 0u) {
        __builtin_trap();
    }

    cfs_bw_slot_t *slot = &owner->workspace->slots[owner->active];
    cfs_bs_ref_t root;
    uint64_t revision;
    status = cfs_bs_branch_construct_publish(owner->store, &slot->token,
        &root, &revision);
    if (status != CFS_BS_OK) {
        return cfs_bw_latch(owner, status);
    }

    slot->branch.root = root;
    slot->branch.revision = revision;
    owner->durable_sequence = owner->accepted_sequence;
    owner->dirty = 0u;
    owner->batch_operations = 0u;
    owner->batch_bytes = 0u;
    owner->work.checkpoints++;
    owner->work.pressure_checkpoints += pressure != 0;
    cfs_bt_stage_reset(&owner->workspace->slots[0].ns.stage);
    cfs_bt_stage_reset(&owner->workspace->slots[1].ns.stage);
    return CFS_BS_OK;
}

int cfs_bw_checkpoint(cfs_bw_t *owner, uint64_t through_sequence) {
    return cfs_bw_checkpoint_internal(owner, through_sequence, 0);
}

int cfs_bw_flush(cfs_bw_t *owner) {
    if (!cfs_bw_owner_valid(owner)) {
        return CFS_BS_EINVAL;
    }

    return cfs_bw_checkpoint_internal(owner, owner->accepted_sequence, 0);
}

int cfs_bw_external_begin(cfs_bw_t *owner, uint64_t *out_sequence) {
    int status = cfs_bw_ready(owner);
    if (status != CFS_BS_OK) {
        return status;
    }

    if (!out_sequence) {
        return CFS_BS_EINVAL;
    }

    if (owner->accepted_sequence == UINT64_MAX) {
        return CFS_BS_EOVERFLOW;
    }

    status = cfs_bw_checkpoint_internal(owner, owner->accepted_sequence, 0);
    if (status == CFS_BS_OK) {
        owner->external_generation = owner->store->generation;
        owner->external = 1u;
        *out_sequence = owner->accepted_sequence + 1u;
    }

    return status;
}

int cfs_bw_external_end(cfs_bw_t *owner, int status) {
    if (!cfs_bw_owner_valid(owner) || owner->external == 0u ||
        status > CFS_BS_OK || status < CFS_BS_EBUSY) {
        return CFS_BS_EINVAL;
    }

    uint64_t generation = owner->external_generation;
    owner->external = 0u;
    owner->external_generation = 0u;
    if (status == CFS_BS_OK && owner->store->poisoned != 0u) {
        status = CFS_BS_EIO;
    }

    if (status == CFS_BS_OK && owner->store->generation != generation) {
        /* Donor transactions may precede the bracket's one publication. */
        if (owner->store->generation < generation) {
            __builtin_trap();
        }

        owner->accepted_sequence++;
        owner->durable_sequence = owner->accepted_sequence;
        owner->work.accepted_operations++;
    }

    return cfs_bw_latch(owner, status);
}

static void invalidate(cfs_bw_t *owner) {
    cfs_bt_stage_discard(&owner->workspace->slots[0].ns.stage);
    cfs_bt_stage_discard(&owner->workspace->slots[1].ns.stage);
    owner->identity = NULL;
    owner->store = NULL;
    owner->workspace = NULL;
    owner->dirty = 0u;
    owner->external = 0u;
}

int cfs_bw_shutdown(cfs_bw_t *owner) {
    int status = cfs_bw_flush(owner);
    if (status == CFS_BS_OK) {
        invalidate(owner);
    }

    return status;
}

int cfs_bw_abort(cfs_bw_t *owner) {
    if (!cfs_bw_owner_valid(owner) || owner->external != 0u) {
        return CFS_BS_EINVAL;
    }

    int status = CFS_BS_OK;
    if (owner->dirty != 0u) {
        cfs_bw_slot_t *slot = &owner->workspace->slots[owner->active];
        if (slot->token.state != 0u) {
            status = cfs_bs_branch_construct_cancel(owner->store,
                &slot->token);
        }
    }

    if (status == CFS_BS_OK) {
        invalidate(owner);
    }

    return status;
}
