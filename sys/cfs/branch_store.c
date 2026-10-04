/* SPDX-License-Identifier: ISC
 *
 * branch_store.c -- catalog and shared-ownership operations for the persistent
 * bounded writable-branch object store.
 *
 * Operations touch only fixed catalog, control, and address-indexed ownership
 * pages through the redo transaction API.  They never consult the prototype
 * free-space tree, reference-count tree, retirement ledger, or cleanup path.
 */
#include "branch_store_internal.h"

#include <string.h>

static int
cfs_bs_block_in_arena(const cfs_bs_store_t *store, uint64_t block) {
    return block >= store->semantic_start && block < store->total_blocks;
}

static int
cfs_bs_ref_shape(const cfs_bs_store_t *store, cfs_bs_ref_t ref,
    int allow_null) {
    if (cfs_bs_ref_is_null(ref)) {
        return allow_null ? CFS_BS_OK : CFS_BS_EINVAL;
    }

    if (!cfs_bs_block_in_arena(store, ref.block) || ref.incarnation == 0u) {
        return CFS_BS_EINVAL;
    }

    return CFS_BS_OK;
}

static uint8_t *
cfs_bs_catalog_slot(uint8_t *page, uint32_t slot) {
    return page + CFS_BS_CATALOG_BODY_OFF +
        (size_t)slot * CFS_BS_CATALOG_ENTRY_SIZE;
}

static void
cfs_bs_catalog_entry_encode(uint8_t *page, uint32_t slot,
    const cfs_bs_catalog_entry_t *entry) {
    cfs_bs_catalog_encode(cfs_bs_catalog_slot(page, slot), entry);
}

static int
cfs_bs_catalog_name_free(const cfs_bs_store_t *store, const uint8_t *page,
    int pin_catalog, const void *name, uint32_t name_length,
    uint32_t *out_free_slot) {
    cfs_bs_catalog_entry_t entry;
    uint32_t free_slot = CFS_BS_MAX_BRANCHES;
    uint32_t slot;
    int status;

    for (slot = 0; slot < CFS_BS_MAX_BRANCHES; ++slot) {
        status = cfs_bs_catalog_entry_decode_checked(store, page, slot,
            pin_catalog, &entry);
        if (status == CFS_BS_ENOENT) {
            if (free_slot == CFS_BS_MAX_BRANCHES) {
                free_slot = slot;
            }

            continue;
        }

        if (status != CFS_BS_OK) {
            return status;
        }

        if (entry.name_length == name_length &&
            memcmp(entry.name, name, name_length) == 0) {
            return CFS_BS_EEXIST;
        }
    }

    if (free_slot == CFS_BS_MAX_BRANCHES) {
        return CFS_BS_ENOSPC;
    }

    *out_free_slot = free_slot;
    return CFS_BS_OK;
}

static void
cfs_bs_branch_from_entry(const cfs_bs_catalog_entry_t *entry,
    cfs_bs_branch_t *out) {
    memset(out, 0, sizeof(*out));
    out->id = entry->id;
    out->revision = entry->revision;
    out->origin_id = entry->origin_id;
    out->origin_revision = entry->origin_revision;
    out->root = entry->root;
    out->name_length = entry->name_length;
    out->kind = entry->kind;
    memcpy(out->name, entry->name, CFS_BS_NAME_BYTES);
}

static void
cfs_bs_pin_from_entry(const cfs_bs_catalog_entry_t *entry,
    cfs_bs_pin_t *out) {
    memset(out, 0, sizeof(*out));
    out->id = entry->id;
    out->revision = entry->revision;
    out->origin_id = entry->origin_id;
    out->origin_revision = entry->origin_revision;
    out->root = entry->root;
    out->name_length = entry->name_length;
    out->kind = entry->kind;
    memcpy(out->name, entry->name, CFS_BS_NAME_BYTES);
}

int
cfs_bs_tx_row(cfs_bs_tx_t *tx, uint64_t block, int dirty,
    cfs_bs_row_t *out, uint8_t **out_entry) {
    uint64_t page_block;
    uint8_t *page;
    uint8_t *entry;
    int status;

    if (!cfs_bs_block_in_arena(tx->store, block)) {
        return CFS_BS_EINVAL;
    }

    page_block = CFS_BS_ROW_START + block / CFS_BS_ROWS_PER_BLOCK;
    status = cfs_bs_tx_get(tx, page_block, dirty, &page);
    if (status != CFS_BS_OK) {
        return status;
    }

    entry = page + CFS_BS_HEADER_SIZE +
        (size_t)(block % CFS_BS_ROWS_PER_BLOCK) * CFS_BS_ROW_SIZE;
    status = cfs_bs_row_entry_decode(tx->store, block, entry, out);
    if (status != CFS_BS_OK) {
        return status;
    }

    *out_entry = entry;
    return CFS_BS_OK;
}

int
cfs_bs_tx_state(cfs_bs_tx_t *tx, int dirty, cfs_bs_state_t *out,
    uint8_t **out_page) {
    uint8_t *page;
    int status;

    status = cfs_bs_tx_get(tx, CFS_BS_STATE_BLOCK, dirty, &page);
    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bs_state_page_decode(tx->store, page, out);
    if (status != CFS_BS_OK) {
        return status;
    }

    *out_page = page;
    return CFS_BS_OK;
}

static int
cfs_bs_tx_catalog(cfs_bs_tx_t *tx, uint64_t block, int pin_catalog,
    uint8_t **out_page) {
    uint8_t *page;
    int status;

    status = cfs_bs_tx_get(tx, block, 1, &page);
    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bs_catalog_page_validate(tx->store, page, pin_catalog);
    if (status != CFS_BS_OK) {
        return status;
    }

    *out_page = page;
    return CFS_BS_OK;
}

int
cfs_bs_tx_live_ref(cfs_bs_tx_t *tx, cfs_bs_ref_t ref, uint8_t *object,
    cfs_bs_row_t *out_row, uint8_t **out_entry) {
    uint32_t child_count;
    uint32_t payload_length;
    int status;

    if (cfs_bs_ref_shape(tx->store, ref, 0) != CFS_BS_OK) {
        return CFS_BS_EINVAL;
    }

    status = cfs_bs_tx_row(tx, ref.block, 1, out_row, out_entry);
    if (status != CFS_BS_OK) {
        return status;
    }

    if (out_row->state != CFS_BS_ROW_LIVE ||
        out_row->incarnation != ref.incarnation) {
        return CFS_BS_ENOENT;
    }

    status = cfs_bs_object_load(tx->store, ref, object, &child_count,
        &payload_length);
    if (status == CFS_BS_EINVAL) {
        return CFS_BS_ECORRUPT;
    }

    return status;
}

int
cfs_bs_tx_ref_add(cfs_bs_tx_t *tx, cfs_bs_ref_t ref, uint8_t *object) {
    cfs_bs_row_t row;
    uint8_t *entry;
    int status;

    status = cfs_bs_tx_live_ref(tx, ref, object, &row, &entry);
    if (status != CFS_BS_OK) {
        return status;
    }

    if (row.refcount == UINT64_MAX) {
        return CFS_BS_EOVERFLOW;
    }

    row.refcount++;
    cfs_bs_row_encode(entry, &row);
    return CFS_BS_OK;
}

int cfs_bs_tx_check_catalog_ref(cfs_bs_tx_t *tx, cfs_bs_ref_t ref) {
    cfs_bs_row_t row;
    uint8_t *row_entry;
    uint32_t roots = 0;
    int rc;

    if (cfs_bs_ref_is_null(ref)) {
        return CFS_BS_OK;
    }

    rc = cfs_bs_tx_live_ref(tx, ref, tx->workspace->scratch[0].bytes,
        &row, &row_entry);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    for (uint32_t which = 0; which < 2; which++) {
        uint8_t *page;
        rc = cfs_bs_tx_get(tx, which ? CFS_BS_PIN_BLOCK : CFS_BS_BRANCH_BLOCK,
            0, &page);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        rc = cfs_bs_catalog_page_validate(tx->store, page, which != 0);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        for (uint32_t slot = 0; slot < CFS_BS_MAX_BRANCHES; slot++) {
            cfs_bs_catalog_entry_t entry;
            rc = cfs_bs_catalog_entry_decode_checked(tx->store, page, slot,
                which != 0, &entry);
            if (rc == CFS_BS_ENOENT) {
                continue;
            }

            if (rc != CFS_BS_OK) {
                return rc;
            }

            if (entry.root.block == ref.block) {
                if (!cfs_bs_ref_equal(entry.root, ref)) {
                    return CFS_BS_ECORRUPT;
                }

                roots++;
            }
        }
    }

    return roots != 0 && row.refcount >= roots ? CFS_BS_OK : CFS_BS_ECORRUPT;
}

static int
enqueue(cfs_bs_tx_t *tx, cfs_bs_state_t *state, uint64_t block,
    cfs_bs_row_t *row, uint8_t *entry, uint32_t released) {
    cfs_bs_row_t tail_row;
    uint8_t *tail_entry;
    uint64_t arena_count = tx->store->total_blocks -
        tx->store->semantic_start;
    int status;

    if (state->pending_count >= arena_count || row->state != CFS_BS_ROW_LIVE ||
        row->refcount != 0u) {
        return CFS_BS_ECORRUPT;
    }

    row->state = CFS_BS_ROW_PENDING;
    row->reserved = released;
    row->next = CFS_BS_BLOCK_NONE;
    cfs_bs_row_encode(entry, row);
    if (state->pending_count == 0u) {
        state->pending_head = block;
        state->pending_tail = block;
    } else {
        if (state->pending_tail == block) {
            return CFS_BS_ECORRUPT;
        }

        status = cfs_bs_tx_row(tx, state->pending_tail, 1, &tail_row,
            &tail_entry);
        if (status != CFS_BS_OK) {
            return status;
        }

        if (tail_row.state != CFS_BS_ROW_PENDING ||
            tail_row.refcount != 0u ||
            tail_row.next != CFS_BS_BLOCK_NONE) {
            return CFS_BS_ECORRUPT;
        }

        tail_row.next = block;
        cfs_bs_row_encode(tail_entry, &tail_row);
        state->pending_tail = block;
    }

    state->pending_count++;
    return CFS_BS_OK;
}

int
cfs_bs_tx_enqueue(cfs_bs_tx_t *tx, cfs_bs_state_t *state,
    uint64_t block, cfs_bs_row_t *row, uint8_t *entry) {
    return enqueue(tx, state, block, row, entry, 0u);
}

int
cfs_bs_tx_enqueue_released(cfs_bs_tx_t *tx, cfs_bs_state_t *state,
    uint64_t block, cfs_bs_row_t *row, uint8_t *entry) {
    return enqueue(tx, state, block, row, entry, CFS_BS_ROW_RELEASED);
}

int
cfs_bs_tx_ref_drop(cfs_bs_tx_t *tx, cfs_bs_state_t *state,
    cfs_bs_ref_t ref, uint8_t *object) {
    cfs_bs_row_t row;
    uint8_t *entry;
    int status;

    status = cfs_bs_tx_live_ref(tx, ref, object, &row, &entry);
    if (status != CFS_BS_OK) {
        return status;
    }

    row.refcount--;
    if (row.refcount == 0u) {
        status = cfs_bs_tx_enqueue(tx, state, ref.block, &row, entry);
        if (status != CFS_BS_OK) {
            return status;
        }
    } else {
        cfs_bs_row_encode(entry, &row);
    }

    return CFS_BS_OK;
}

static int
cfs_bs_new_identity(cfs_bs_state_t *state, uint32_t slot,
    uint64_t *out_id) {
    if (state->next_serial > (UINT64_MAX >> 4)) {
        return CFS_BS_EOVERFLOW;
    }

    *out_id = (state->next_serial << 4) | slot;
    state->next_serial++;
    return CFS_BS_OK;
}

static int
cfs_bs_mutation_ready(cfs_bs_store_t *store, cfs_bs_workspace_t *workspace) {
    if (store == NULL || workspace == NULL) {
        return CFS_BS_EINVAL;
    }

    if (store->poisoned != 0u) {
        return CFS_BS_EPOISON;
    }

    if (store->generation == UINT64_MAX) {
        return CFS_BS_EOVERFLOW;
    }

    return CFS_BS_OK;
}

int
cfs_bs_tx_load_head(cfs_bs_tx_t *tx, uint64_t branch_id,
    uint64_t expected_revision, uint32_t kind, uint8_t **out_catalog,
    cfs_bs_catalog_entry_t *out_entry) {
    uint32_t slot;
    int status;

    if (branch_id == 0u) {
        return CFS_BS_EINVAL;
    }

    slot = (uint32_t)(branch_id & 15u);
    status = cfs_bs_tx_catalog(tx, CFS_BS_BRANCH_BLOCK, 0, out_catalog);
    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bs_catalog_entry_decode_checked(tx->store, *out_catalog,
        slot, 0, out_entry);
    if (status != CFS_BS_OK) {
        return status;
    }

    if (out_entry->id != branch_id || out_entry->kind != kind) {
        return CFS_BS_ENOENT;
    }

    if (out_entry->revision != expected_revision) {
        return CFS_BS_ECONFLICT;
    }

    return CFS_BS_OK;
}

int cfs_bs_tx_load_branch(cfs_bs_tx_t *tx, uint64_t branch_id,
    uint64_t expected_revision, uint8_t **out_catalog,
    cfs_bs_catalog_entry_t *out_entry) {
    return cfs_bs_tx_load_head(tx, branch_id, expected_revision,
        CFS_BS_CATALOG_BRANCH, out_catalog, out_entry);
}

int
cfs_bs_branch_fork(cfs_bs_store_t *store, uint64_t source_branch_id,
    uint64_t expected_revision, const void *name, uint16_t name_length,
    cfs_bs_branch_t *out, cfs_bs_workspace_t *workspace) {
    cfs_bs_catalog_entry_t child;
    cfs_bs_catalog_entry_t source;
    cfs_bs_state_t state;
    cfs_bs_tx_t tx;
    uint8_t *catalog;
    uint8_t *state_page;
    uint32_t slot;
    int status;

    if (cfs_bs_name_validate(name, name_length) != CFS_BS_OK || out == NULL) {
        return CFS_BS_EINVAL;
    }

    status = cfs_bs_mutation_ready(store, workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bs_tx_begin(store, workspace, &tx);
    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bs_tx_load_branch(&tx, source_branch_id, expected_revision,
        &catalog, &source);
    if (status == CFS_BS_OK) {
        status = cfs_bs_catalog_name_free(store, catalog, 0, name,
            name_length, &slot);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bs_tx_state(&tx, 1, &state, &state_page);
    }

    if (status == CFS_BS_OK && !cfs_bs_ref_is_null(source.root)) {
        status = cfs_bs_tx_ref_add(&tx, source.root,
            workspace->scratch[0].bytes);
    }

    if (status == CFS_BS_OK) {
        memset(&child, 0, sizeof(child));
        status = cfs_bs_new_identity(&state, slot, &child.id);
    }

    if (status != CFS_BS_OK) {
        cfs_bs_tx_abort(&tx);
        return status;
    }

    child.revision = source.revision;
    child.origin_id = source.id;
    child.origin_revision = source.revision;
    child.root = source.root;
    child.name_length = name_length;
    child.kind = CFS_BS_CATALOG_BRANCH;
    memcpy(child.name, name, name_length);
    cfs_bs_catalog_entry_encode(catalog, slot, &child);
    cfs_bs_state_encode(state_page, &state);
    status = cfs_bs_tx_commit(&tx, 0);
    if (status != CFS_BS_OK) {
        return status;
    }

    cfs_bs_branch_from_entry(&child, out);
    return CFS_BS_OK;
}

int
cfs_bs_branch_reset_from_branch(cfs_bs_store_t *store,
    uint64_t destination_branch_id,
    uint64_t expected_destination_revision, uint64_t source_branch_id,
    uint64_t expected_source_revision, cfs_bs_branch_t *out,
    cfs_bs_workspace_t *workspace) {
    cfs_bs_catalog_entry_t destination;
    cfs_bs_catalog_entry_t source;
    cfs_bs_branch_t result;
    cfs_bs_state_t state;
    cfs_bs_row_t row;
    cfs_bs_tx_t tx;
    uint8_t *catalog;
    uint8_t *entry;
    uint8_t *pin_catalog;
    uint8_t *state_page;
    int status;

    if (!out) {
        return CFS_BS_EINVAL;
    }

    status = cfs_bs_mutation_ready(store, workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bs_tx_begin(store, workspace, &tx);
    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bs_tx_load_branch(&tx, destination_branch_id,
        expected_destination_revision, &catalog, &destination);
    if (status == CFS_BS_OK) {
        status = cfs_bs_tx_load_branch(&tx, source_branch_id,
            expected_source_revision, &catalog, &source);
    }

    if (status != CFS_BS_OK) {
        cfs_bs_tx_abort(&tx);
        return status;
    }

    if (cfs_bs_ref_equal(destination.root, source.root)) {
        status = cfs_bs_tx_state(&tx, 0, &state, &state_page);
        if (status == CFS_BS_OK) {
            status = cfs_bs_tx_get(&tx, CFS_BS_PIN_BLOCK, 0,
                &pin_catalog);
        }

        if (status == CFS_BS_OK) {
            status = cfs_bs_catalog_pair_validate(store, state_page,
                catalog, pin_catalog);
        }

        if (status == CFS_BS_OK && !cfs_bs_ref_is_null(source.root)) {
            status = cfs_bs_tx_live_ref(&tx, source.root,
                workspace->scratch[0].bytes, &row, &entry);
            if (status == CFS_BS_OK &&
                row.refcount <
                    (destination_branch_id == source_branch_id ? 1u : 2u)) {
                status = CFS_BS_ECORRUPT;
            }
        }

        if (status != CFS_BS_OK) {
            cfs_bs_tx_abort(&tx);
            return status;
        }

        cfs_bs_branch_from_entry(&destination, &result);
        cfs_bs_tx_abort(&tx);
        *out = result;
        return CFS_BS_OK;
    }

    status = cfs_bs_tx_state(&tx, 1, &state, &state_page);
    if (status == CFS_BS_OK && state.next_revision == UINT64_MAX) {
        status = CFS_BS_EOVERFLOW;
    }

    if (status == CFS_BS_OK && !cfs_bs_ref_is_null(source.root)) {
        status = cfs_bs_tx_ref_add(&tx, source.root,
            workspace->scratch[0].bytes);
    }

    if (status == CFS_BS_OK && !cfs_bs_ref_is_null(destination.root)) {
        status = cfs_bs_tx_ref_drop(&tx, &state, destination.root,
            workspace->scratch[0].bytes);
    }

    if (status != CFS_BS_OK) {
        cfs_bs_tx_abort(&tx);
        return status;
    }

    destination.revision = state.next_revision++;
    destination.root = source.root;
    cfs_bs_catalog_entry_encode(catalog,
        (uint32_t)(destination_branch_id & 15u), &destination);
    cfs_bs_state_encode(state_page, &state);
    status = cfs_bs_tx_commit(&tx, 0);
    if (status != CFS_BS_OK) {
        return status;
    }

    cfs_bs_branch_from_entry(&destination, &result);
    *out = result;
    return CFS_BS_OK;
}

int
cfs_bs_branch_discard(cfs_bs_store_t *store, uint64_t branch_id,
    uint64_t expected_revision, cfs_bs_workspace_t *workspace) {
    cfs_bs_catalog_entry_t branch;
    cfs_bs_state_t state;
    cfs_bs_tx_t tx;
    uint8_t *catalog;
    uint8_t *state_page;
    int status;

    status = cfs_bs_mutation_ready(store, workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bs_tx_begin(store, workspace, &tx);
    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bs_tx_load_branch(&tx, branch_id, expected_revision,
        &catalog, &branch);
    if (status == CFS_BS_OK) {
        status = cfs_bs_tx_state(&tx, 1, &state, &state_page);
    }

    if (status == CFS_BS_OK && !cfs_bs_ref_is_null(branch.root)) {
        status = cfs_bs_tx_ref_drop(&tx, &state, branch.root,
            workspace->scratch[0].bytes);
    }

    if (status != CFS_BS_OK) {
        cfs_bs_tx_abort(&tx);
        return status;
    }

    memset(cfs_bs_catalog_slot(catalog, (uint32_t)(branch_id & 15u)), 0,
        CFS_BS_CATALOG_ENTRY_SIZE);
    cfs_bs_state_encode(state_page, &state);
    return cfs_bs_tx_commit(&tx, 0);
}

int
cfs_bs_pin_create(cfs_bs_store_t *store, uint32_t kind,
    uint64_t branch_id, uint64_t expected_revision, const void *name,
    uint16_t name_length, cfs_bs_pin_t *out,
    cfs_bs_workspace_t *workspace) {
    cfs_bs_catalog_entry_t branch;
    cfs_bs_catalog_entry_t pin;
    cfs_bs_state_t state;
    cfs_bs_tx_t tx;
    uint8_t *branch_catalog;
    uint8_t *pin_catalog;
    uint8_t *state_page;
    uint32_t slot;
    int status;

    if ((kind != CFS_BS_CATALOG_SNAPSHOT_PIN &&
            kind != CFS_BS_CATALOG_RETAINED_ROOT_PIN) ||
        cfs_bs_name_validate(name, name_length) != CFS_BS_OK || out == NULL) {
        return CFS_BS_EINVAL;
    }

    status = cfs_bs_mutation_ready(store, workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bs_tx_begin(store, workspace, &tx);
    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bs_tx_load_branch(&tx, branch_id, expected_revision,
        &branch_catalog, &branch);
    if (status == CFS_BS_OK) {
        status = cfs_bs_tx_catalog(&tx, CFS_BS_PIN_BLOCK, 1,
            &pin_catalog);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bs_catalog_name_free(store, pin_catalog, 1, name,
            name_length, &slot);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bs_tx_state(&tx, 1, &state, &state_page);
    }

    if (status == CFS_BS_OK && !cfs_bs_ref_is_null(branch.root)) {
        status = cfs_bs_tx_ref_add(&tx, branch.root,
            workspace->scratch[0].bytes);
    }

    if (status == CFS_BS_OK) {
        memset(&pin, 0, sizeof(pin));
        status = cfs_bs_new_identity(&state, slot, &pin.id);
    }

    if (status != CFS_BS_OK) {
        cfs_bs_tx_abort(&tx);
        return status;
    }

    pin.revision = branch.revision;
    pin.origin_id = branch.id;
    pin.origin_revision = branch.revision;
    pin.root = branch.root;
    pin.name_length = name_length;
    pin.kind = kind;
    memcpy(pin.name, name, name_length);
    cfs_bs_catalog_entry_encode(pin_catalog, slot, &pin);
    cfs_bs_state_encode(state_page, &state);
    status = cfs_bs_tx_commit(&tx, 0);
    if (status != CFS_BS_OK) {
        return status;
    }

    cfs_bs_pin_from_entry(&pin, out);
    return CFS_BS_OK;
}

int
cfs_bs_pin_drop(cfs_bs_store_t *store, uint64_t pin_id,
    cfs_bs_workspace_t *workspace) {
    cfs_bs_catalog_entry_t pin;
    cfs_bs_state_t state;
    cfs_bs_tx_t tx;
    uint8_t *catalog;
    uint8_t *state_page;
    uint32_t slot;
    int status;

    if (pin_id == 0u) {
        return CFS_BS_EINVAL;
    }

    status = cfs_bs_mutation_ready(store, workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bs_tx_begin(store, workspace, &tx);
    if (status != CFS_BS_OK) {
        return status;
    }

    slot = (uint32_t)(pin_id & 15u);
    status = cfs_bs_tx_catalog(&tx, CFS_BS_PIN_BLOCK, 1, &catalog);
    if (status == CFS_BS_OK) {
        status = cfs_bs_catalog_entry_decode_checked(store, catalog, slot, 1,
            &pin);
    }

    if (status == CFS_BS_OK && pin.id != pin_id) {
        status = CFS_BS_ENOENT;
    }

    if (status == CFS_BS_OK) {
        status = cfs_bs_tx_state(&tx, 1, &state, &state_page);
    }

    if (status == CFS_BS_OK && !cfs_bs_ref_is_null(pin.root)) {
        status = cfs_bs_tx_ref_drop(&tx, &state, pin.root,
            workspace->scratch[0].bytes);
    }

    if (status != CFS_BS_OK) {
        cfs_bs_tx_abort(&tx);
        return status;
    }

    memset(cfs_bs_catalog_slot(catalog, slot), 0,
        CFS_BS_CATALOG_ENTRY_SIZE);
    cfs_bs_state_encode(state_page, &state);
    return cfs_bs_tx_commit(&tx, 0);
}
