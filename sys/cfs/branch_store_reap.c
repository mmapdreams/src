/* SPDX-License-Identifier: ISC
 *
 * branch_store_reap.c -- bounded atomic FIFO reclamation batches for the
 * persistent branch store.
 *
 * Each call admits a prefix into one redo transaction. Candidate pages and
 * objects are validated before that candidate changes cached ownership state,
 * so only cache admission may shorten a positive prefix.
 */
#include "branch_store_internal.h"

#include <string.h>

#include "codec.h"

static int
cfs_bs_reap_pending_object(cfs_bs_store_t *store, cfs_bs_row_t row,
    uint64_t block, uint8_t *page, uint32_t *out_child_count) {
    cfs_bs_ref_t object;
    uint32_t payload_length;
    int status;

    if (row.state != CFS_BS_ROW_PENDING || row.refcount != 0u ||
        row.incarnation == 0u) {
        return CFS_BS_ECORRUPT;
    }

    status = cfs_bs_io_read(store, block, page);
    if (status != CFS_BS_OK) {
        return status;
    }

    object.block = block;
    object.incarnation = row.incarnation;
    object.hash = cfs_get64(page + CFS_BS_HEADER_HASH_OFF);
    return cfs_bs_object_page_validate(store, object, page, out_child_count,
        &payload_length);
}

static int
cfs_bs_reap_child_preflight(cfs_bs_tx_t *tx, cfs_bs_ref_t child,
    uint8_t *scratch) {
    cfs_bs_row_t row;
    uint8_t *entry;
    uint32_t child_count;
    uint32_t payload_length;
    int status;

    if (cfs_bs_ref_is_null(child) ||
        child.block < tx->store->semantic_start ||
        child.block >= tx->store->total_blocks || child.incarnation == 0u) {
        return CFS_BS_ECORRUPT;
    }

    status = cfs_bs_tx_row(tx, child.block, 0, &row, &entry);
    if (status != CFS_BS_OK) {
        return status == CFS_BS_EINVAL ? CFS_BS_ECORRUPT : status;
    }

    if (row.state != CFS_BS_ROW_LIVE || row.refcount == 0u ||
        row.incarnation != child.incarnation) {
        return CFS_BS_ECORRUPT;
    }

    status = cfs_bs_object_load(tx->store, child, scratch, &child_count,
        &payload_length);
    return status == CFS_BS_EINVAL ? CFS_BS_ECORRUPT : status;
}

static int
cfs_bs_reap_candidate_preflight(cfs_bs_tx_t *tx, cfs_bs_state_t *state,
    uint8_t *object, cfs_bs_row_t *out_row, uint8_t **out_entry,
    uint32_t *out_child_count) {
    cfs_bs_row_t tail;
    uint8_t *tail_entry;
    uint32_t i;
    int status;

    status = cfs_bs_tx_row(tx, state->pending_head, 0, out_row, out_entry);
    if (status == CFS_BS_OK && out_row->reserved == CFS_BS_ROW_RELEASED) {
        /* Its retiring publication already consumed every edge. */
        *out_child_count = 0u;
        if (out_row->refcount != 0u || out_row->incarnation == 0u) {
            status = CFS_BS_ECORRUPT;
        }
    } else if (status == CFS_BS_OK) {
        status = cfs_bs_reap_pending_object(tx->store, *out_row,
            state->pending_head, object, out_child_count);
    }

    if (status != CFS_BS_OK) {
        return status;
    }

    if ((state->pending_count == 1u &&
            (state->pending_head != state->pending_tail ||
                out_row->next != CFS_BS_BLOCK_NONE)) ||
        (state->pending_count > 1u &&
            (state->pending_head == state->pending_tail ||
                out_row->next == CFS_BS_BLOCK_NONE ||
                out_row->next == state->pending_head))) {
        return CFS_BS_ECORRUPT;
    }

    status = cfs_bs_tx_row(tx, state->pending_tail, 0, &tail,
        &tail_entry);
    if (status != CFS_BS_OK) {
        return status;
    }

    if (tail.state != CFS_BS_ROW_PENDING || tail.refcount != 0u ||
        tail.next != CFS_BS_BLOCK_NONE) {
        return CFS_BS_ECORRUPT;
    }

    for (i = 0; i < *out_child_count; ++i) {
        cfs_bs_ref_t child;

        cfs_bs_ref_decode(object + CFS_BS_OBJECT_REFS_OFF +
            (size_t)i * CFS_BS_REF_SIZE, &child);
        status = cfs_bs_reap_child_preflight(tx, child,
            tx->workspace->scratch[1].bytes);
        if (status != CFS_BS_OK) {
            return status;
        }
    }

    if (state->free_count == UINT64_MAX) {
        return CFS_BS_EOVERFLOW;
    }

    return cfs_bs_tx_row(tx, state->pending_head, 1, out_row, out_entry);
}

static int
cfs_bs_reap_drop_cached(cfs_bs_tx_t *tx, cfs_bs_state_t *state,
    cfs_bs_ref_t child) {
    cfs_bs_row_t row;
    uint8_t *entry;
    int status;

    status = cfs_bs_tx_row(tx, child.block, 1, &row, &entry);
    if (status != CFS_BS_OK) {
        return status;
    }

    if (row.state != CFS_BS_ROW_LIVE || row.refcount == 0u ||
        row.incarnation != child.incarnation) {
        return CFS_BS_ECORRUPT;
    }

    --row.refcount;
    if (row.refcount == 0u) {
        return cfs_bs_tx_enqueue(tx, state, child.block, &row, entry);
    }

    cfs_bs_row_encode(entry, &row);
    return CFS_BS_OK;
}

static int
cfs_bs_reap_candidate_apply(cfs_bs_tx_t *tx, cfs_bs_state_t *state,
    uint8_t *object, cfs_bs_row_t *row, uint8_t *entry,
    uint32_t child_count) {
    uint64_t block = state->pending_head;
    uint32_t i;
    int status;

    state->pending_head = row->next;
    --state->pending_count;
    if (state->pending_count == 0u) {
        state->pending_tail = CFS_BS_BLOCK_NONE;
    }

    for (i = 0; i < child_count; ++i) {
        cfs_bs_ref_t child;

        cfs_bs_ref_decode(object + CFS_BS_OBJECT_REFS_OFF +
            (size_t)i * CFS_BS_REF_SIZE, &child);
        status = cfs_bs_reap_drop_cached(tx, state, child);
        if (status != CFS_BS_OK) {
            return status;
        }
    }

    row->state = CFS_BS_ROW_FREE;
    row->reserved = 0u;
    row->refcount = 0u;
    row->next = state->free_head;
    cfs_bs_row_encode(entry, row);
    state->free_head = block;
    ++state->free_count;
    return CFS_BS_OK;
}

static int
cfs_bs_reap_begin(cfs_bs_store_t *store, cfs_bs_workspace_t *workspace,
    cfs_bs_tx_t *tx, cfs_bs_state_t *state, uint8_t **out_state_page) {
    uint8_t *ignored;
    int status;

    status = cfs_bs_tx_begin(store, workspace, tx);
    if (status == CFS_BS_OK) {
        status = cfs_bs_tx_state(tx, 1, state, out_state_page);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bs_tx_get(tx, CFS_BS_BRANCH_BLOCK, 0, &ignored);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bs_tx_get(tx, CFS_BS_PIN_BLOCK, 0, &ignored);
    }

    return status;
}

int cfs_bs_tx_reap_prefix(cfs_bs_tx_t *tx, cfs_bs_state_t *state,
    uint32_t limit, uint32_t *out_reaped) {
    uint32_t reaped = 0u;
    while (reaped < limit && state->pending_count != 0u) {
        cfs_bs_row_t row;
        uint8_t *entry;
        uint32_t child_count;
        uint8_t *object = tx->workspace->scratch[0].bytes;
        int status = cfs_bs_reap_candidate_preflight(tx, state, object,
            &row, &entry, &child_count);
        if (status == CFS_BS_EWORKSPACE && reaped != 0u) {
            break;
        }

        if (status != CFS_BS_OK) {
            return status;
        }

        status = cfs_bs_reap_candidate_apply(tx, state, object, &row, entry,
            child_count);
        if (status != CFS_BS_OK) {
            return status;
        }

        reaped++;
    }

    *out_reaped = reaped;
    return CFS_BS_OK;
}

int
cfs_bs_reap(cfs_bs_store_t *store, uint32_t limit, uint32_t *out_reaped,
    cfs_bs_workspace_t *workspace) {
    cfs_bs_state_t state;
    cfs_bs_tx_t tx;
    uint8_t *state_page;
    uint32_t goal;
    uint32_t reaped = 0u;
    int status;

    if (out_reaped) {
        *out_reaped = 0u;
    }

    if (limit == 0u || out_reaped == NULL) {
        return CFS_BS_EINVAL;
    }

    memset(&tx, 0, sizeof(tx));
    goal = limit < CFS_BS_REAP_MAX_OBJECTS ?
        limit : CFS_BS_REAP_MAX_OBJECTS;
    status = cfs_bs_reap_begin(store, workspace, &tx, &state, &state_page);
    if (status != CFS_BS_OK) {
        cfs_bs_tx_abort(&tx);
        return status;
    }

    if (state.pending_count == 0u) {
        cfs_bs_tx_abort(&tx);
        return CFS_BS_OK;
    }

    status = cfs_bs_tx_reap_prefix(&tx, &state, goal, &reaped);
    if (status != CFS_BS_OK) {
        cfs_bs_tx_abort(&tx);
        return status;
    }

    cfs_bs_state_encode(state_page, &state);
    status = cfs_bs_tx_commit(&tx, 0);
    if (status != CFS_BS_OK) {
        return status;
    }

    *out_reaped = reaped;
    return CFS_BS_OK;
}
