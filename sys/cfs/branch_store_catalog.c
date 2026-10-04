/* SPDX-License-Identifier: ISC
 *
 * branch_store_catalog.c -- checked catalog, control, and ownership-row
 * inspection for the persistent branch store.
 *
 * These readers validate fixed-page bodies without changing publication
 * state.  Transactional branch operations use the same checked decoders.
 */
#include "branch_store_internal.h"

#include <string.h>

#include "codec.h"

static int
cfs_bs_bytes_zero(const uint8_t *bytes, size_t length) {
    size_t i;

    for (i = 0; i < length; ++i) {
        if (bytes[i] != 0u) {
            return 0;
        }
    }

    return 1;
}

static int
cfs_bs_block_in_arena(const cfs_bs_store_t *store, uint64_t block) {
    return block >= store->semantic_start && block < store->total_blocks;
}

static int
cfs_bs_link_valid(const cfs_bs_store_t *store, uint64_t block) {
    return block == CFS_BS_BLOCK_NONE || cfs_bs_block_in_arena(store, block);
}

static int
cfs_bs_ref_shape(const cfs_bs_store_t *store, cfs_bs_ref_t ref) {
    if (cfs_bs_ref_is_null(ref)) {
        return CFS_BS_OK;
    }

    if (!cfs_bs_block_in_arena(store, ref.block) ||
        !cfs_bs_incarnation_valid(ref.incarnation)) {
        return CFS_BS_ECORRUPT;
    }

    return CFS_BS_OK;
}

void
cfs_bs_row_decode(const uint8_t *src, cfs_bs_row_t *out) {
    if (src == NULL || out == NULL) {
        return;
    }

    out->state = cfs_get32(src + CFS_BS_ROW_STATE_OFF);
    out->reserved = cfs_get32(src + CFS_BS_ROW_RESERVED_OFF);
    out->refcount = cfs_get64(src + CFS_BS_ROW_REFCOUNT_OFF);
    out->incarnation = cfs_get64(src + CFS_BS_ROW_INCARNATION_OFF);
    out->next = cfs_get64(src + CFS_BS_ROW_NEXT_OFF);
}

void
cfs_bs_row_encode(uint8_t *dst, const cfs_bs_row_t *row) {
    if (dst == NULL || row == NULL) {
        return;
    }

    cfs_put32(dst + CFS_BS_ROW_STATE_OFF, row->state);
    cfs_put32(dst + CFS_BS_ROW_RESERVED_OFF, row->reserved);
    cfs_put64(dst + CFS_BS_ROW_REFCOUNT_OFF, row->refcount);
    cfs_put64(dst + CFS_BS_ROW_INCARNATION_OFF, row->incarnation);
    cfs_put64(dst + CFS_BS_ROW_NEXT_OFF, row->next);
}

void
cfs_bs_catalog_decode(const uint8_t *src, cfs_bs_catalog_entry_t *out) {
    if (src == NULL || out == NULL) {
        return;
    }

    out->id = cfs_get64(src + CFS_BS_CATALOG_ID_OFF);
    out->revision = cfs_get64(src + CFS_BS_CATALOG_REVISION_OFF);
    out->origin_id = cfs_get64(src + CFS_BS_CATALOG_ORIGIN_ID_OFF);
    out->origin_revision =
        cfs_get64(src + CFS_BS_CATALOG_ORIGIN_REVISION_OFF);
    cfs_bs_ref_decode(src + CFS_BS_CATALOG_ROOT_OFF, &out->root);
    out->name_length = cfs_get32(src + CFS_BS_CATALOG_NAME_LENGTH_OFF);
    out->kind = cfs_get32(src + CFS_BS_CATALOG_KIND_OFF);
    memcpy(out->name, src + CFS_BS_CATALOG_NAME_OFF, CFS_BS_NAME_BYTES);
}

void
cfs_bs_catalog_encode(uint8_t *dst,
    const cfs_bs_catalog_entry_t *entry) {
    if (dst == NULL || entry == NULL) {
        return;
    }

    memset(dst, 0, CFS_BS_CATALOG_ENTRY_SIZE);
    cfs_put64(dst + CFS_BS_CATALOG_ID_OFF, entry->id);
    cfs_put64(dst + CFS_BS_CATALOG_REVISION_OFF, entry->revision);
    cfs_put64(dst + CFS_BS_CATALOG_ORIGIN_ID_OFF, entry->origin_id);
    cfs_put64(dst + CFS_BS_CATALOG_ORIGIN_REVISION_OFF,
        entry->origin_revision);
    cfs_bs_ref_encode(dst + CFS_BS_CATALOG_ROOT_OFF, entry->root);
    cfs_put32(dst + CFS_BS_CATALOG_NAME_LENGTH_OFF, entry->name_length);
    cfs_put32(dst + CFS_BS_CATALOG_KIND_OFF, entry->kind);
    memcpy(dst + CFS_BS_CATALOG_NAME_OFF, entry->name, CFS_BS_NAME_BYTES);
}

void
cfs_bs_state_decode(const uint8_t *src, cfs_bs_state_t *out) {
    if (src == NULL || out == NULL) {
        return;
    }

    out->free_head = cfs_get64(src + CFS_BS_STATE_FREE_HEAD_OFF);
    out->free_count = cfs_get64(src + CFS_BS_STATE_FREE_COUNT_OFF);
    out->pending_head = cfs_get64(src + CFS_BS_STATE_PENDING_HEAD_OFF);
    out->pending_tail = cfs_get64(src + CFS_BS_STATE_PENDING_TAIL_OFF);
    out->pending_count = cfs_get64(src + CFS_BS_STATE_PENDING_COUNT_OFF);
    out->next_serial = cfs_get64(src + CFS_BS_STATE_NEXT_SERIAL_OFF);
    out->next_revision = cfs_get64(src + CFS_BS_STATE_NEXT_REVISION_OFF);
    out->reserve = cfs_get64(src + CFS_BS_STATE_RESERVE_OFF);
}

void
cfs_bs_state_encode(uint8_t *dst, const cfs_bs_state_t *state) {
    if (dst == NULL || state == NULL) {
        return;
    }

    memset(dst + CFS_BS_HEADER_SIZE, 0,
        CFS_BLOCK_SIZE - CFS_BS_HEADER_SIZE);
    cfs_put64(dst + CFS_BS_STATE_FREE_HEAD_OFF, state->free_head);
    cfs_put64(dst + CFS_BS_STATE_FREE_COUNT_OFF, state->free_count);
    cfs_put64(dst + CFS_BS_STATE_PENDING_HEAD_OFF, state->pending_head);
    cfs_put64(dst + CFS_BS_STATE_PENDING_TAIL_OFF, state->pending_tail);
    cfs_put64(dst + CFS_BS_STATE_PENDING_COUNT_OFF, state->pending_count);
    cfs_put64(dst + CFS_BS_STATE_NEXT_SERIAL_OFF, state->next_serial);
    cfs_put64(dst + CFS_BS_STATE_NEXT_REVISION_OFF, state->next_revision);
    cfs_put64(dst + CFS_BS_STATE_RESERVE_OFF, state->reserve);
}

int
cfs_bs_name_validate(const void *name, uint32_t length) {
    const uint8_t *bytes = name;
    uint32_t i;

    if (name == NULL || length == 0u || length > CFS_BS_NAME_MAX) {
        return CFS_BS_EINVAL;
    }

    for (i = 0; i < length; ++i) {
        uint8_t c = bytes[i];

        if (!((c >= (uint8_t)'A' && c <= (uint8_t)'Z') ||
                (c >= (uint8_t)'a' && c <= (uint8_t)'z') ||
                (c >= (uint8_t)'0' && c <= (uint8_t)'9') ||
                c == (uint8_t)'_' || c == (uint8_t)'-' ||
                c == (uint8_t)'.')) {
            return CFS_BS_EINVAL;
        }
    }

    return CFS_BS_OK;
}

int
cfs_bs_state_page_decode(const cfs_bs_store_t *store, const uint8_t *page,
    cfs_bs_state_t *out) {
    uint64_t arena_count;

    if (store == NULL || page == NULL || out == NULL) {
        return CFS_BS_EINVAL;
    }

    cfs_bs_state_decode(page, out);
    arena_count = store->total_blocks - store->semantic_start;
    if (out->reserve != store->reserve || out->reserve < 5u ||
        out->reserve >= arena_count || out->free_count > arena_count ||
        out->pending_count > arena_count ||
        out->free_count > arena_count - out->pending_count ||
        out->next_serial < 2u || out->next_revision < 2u ||
        !cfs_bs_link_valid(store, out->free_head) ||
        !cfs_bs_link_valid(store, out->pending_head) ||
        !cfs_bs_link_valid(store, out->pending_tail) ||
        (out->free_count == 0u) !=
            (out->free_head == CFS_BS_BLOCK_NONE) ||
        (out->pending_count == 0u) !=
            (out->pending_head == CFS_BS_BLOCK_NONE) ||
        (out->pending_count == 0u) !=
            (out->pending_tail == CFS_BS_BLOCK_NONE) ||
        (out->pending_count == 1u &&
            out->pending_head != out->pending_tail) ||
        (out->pending_count > 1u &&
            out->pending_head == out->pending_tail) ||
        !cfs_bs_bytes_zero(page + CFS_BS_STATE_RESERVE_OFF + 8u,
            CFS_BLOCK_SIZE - CFS_BS_STATE_RESERVE_OFF - 8u)) {
        return CFS_BS_ECORRUPT;
    }

    return CFS_BS_OK;
}

int
cfs_bs_catalog_entry_decode_checked(const cfs_bs_store_t *store,
    const uint8_t *page, uint32_t slot, int pin_catalog,
    cfs_bs_catalog_entry_t *out) {
    const uint8_t *entry;
    uint32_t i;

    if (store == NULL || page == NULL || out == NULL ||
        slot >= CFS_BS_MAX_BRANCHES) {
        return CFS_BS_EINVAL;
    }

    entry = page + CFS_BS_CATALOG_BODY_OFF +
        (size_t)slot * CFS_BS_CATALOG_ENTRY_SIZE;
    if (cfs_bs_bytes_zero(entry, CFS_BS_CATALOG_ENTRY_SIZE)) {
        return CFS_BS_ENOENT;
    }

    cfs_bs_catalog_decode(entry, out);
    if ((out->id >> 4) == 0u || (out->id & 15u) != slot ||
        out->revision == 0u ||
        out->name_length > CFS_BS_NAME_MAX ||
        (out->kind == CFS_BS_CATALOG_STAGING ?
            out->name_length != 0u :
            cfs_bs_name_validate(out->name, out->name_length) != CFS_BS_OK) ||
        cfs_bs_ref_shape(store, out->root) != CFS_BS_OK ||
        !cfs_bs_bytes_zero(entry + CFS_BS_CATALOG_RESERVED_OFF,
            CFS_BS_CATALOG_ENTRY_SIZE - CFS_BS_CATALOG_RESERVED_OFF)) {
        return CFS_BS_ECORRUPT;
    }

    for (i = out->name_length; i < CFS_BS_NAME_BYTES; ++i) {
        if (out->name[i] != 0u) {
            return CFS_BS_ECORRUPT;
        }
    }

    if ((!pin_catalog && out->kind != CFS_BS_CATALOG_BRANCH &&
            out->kind != CFS_BS_CATALOG_STAGING) ||
        (pin_catalog &&
            out->kind != CFS_BS_CATALOG_SNAPSHOT_PIN &&
            out->kind != CFS_BS_CATALOG_RETAINED_ROOT_PIN) ||
        ((out->origin_id == 0u) != (out->origin_revision == 0u)) ||
        (out->origin_id != 0u && (out->origin_id >> 4) == 0u) ||
        (pin_catalog && (out->origin_id == 0u ||
            out->origin_revision != out->revision)) ||
        (!pin_catalog && out->id != CFS_BS_MAIN_ID &&
            out->origin_id == 0u) ||
        (out->kind == CFS_BS_CATALOG_STAGING &&
            (out->id == CFS_BS_MAIN_ID || out->origin_id == out->id))) {
        return CFS_BS_ECORRUPT;
    }

    return CFS_BS_OK;
}

int
cfs_bs_catalog_page_validate(const cfs_bs_store_t *store,
    const uint8_t *page, int pin_catalog) {
    cfs_bs_catalog_entry_t entry;
    cfs_bs_catalog_entry_t prior;
    uint32_t earlier;
    uint32_t slot;
    int status;

    if (store == NULL || page == NULL) {
        return CFS_BS_EINVAL;
    }

    for (slot = 0; slot < CFS_BS_MAX_BRANCHES; ++slot) {
        status = cfs_bs_catalog_entry_decode_checked(store, page, slot,
            pin_catalog, &entry);
        if (status != CFS_BS_OK && status != CFS_BS_ENOENT) {
            return status;
        }

        if (status == CFS_BS_ENOENT) {
            continue;
        }

        for (earlier = 0; earlier < slot; ++earlier) {
            status = cfs_bs_catalog_entry_decode_checked(store, page,
                earlier, pin_catalog, &prior);
            if (status == CFS_BS_ENOENT) {
                continue;
            }

            if (status != CFS_BS_OK) {
                return status;
            }

            if (entry.name_length != 0u &&
                entry.name_length == prior.name_length &&
                memcmp(entry.name, prior.name, entry.name_length) == 0) {
                return CFS_BS_ECORRUPT;
            }
        }
    }

    if (!cfs_bs_bytes_zero(page + CFS_BS_CATALOG_BODY_OFF +
            CFS_BS_MAX_BRANCHES * CFS_BS_CATALOG_ENTRY_SIZE,
            CFS_BLOCK_SIZE - CFS_BS_CATALOG_BODY_OFF -
                CFS_BS_MAX_BRANCHES * CFS_BS_CATALOG_ENTRY_SIZE)) {
        return CFS_BS_ECORRUPT;
    }

    return CFS_BS_OK;
}

static int
cfs_bs_catalog_index_decode(const cfs_bs_store_t *store,
    const uint8_t *branches, const uint8_t *pins, uint32_t index,
    cfs_bs_catalog_entry_t *out) {
    int pin_catalog = index >= CFS_BS_MAX_BRANCHES;
    const uint8_t *page = pin_catalog ? pins : branches;
    uint32_t slot = index & 15u;

    return cfs_bs_catalog_entry_decode_checked(store, page, slot,
        pin_catalog, out);
}

int
cfs_bs_catalog_pair_validate(const cfs_bs_store_t *store,
    const uint8_t *state_page, const uint8_t *branches, const uint8_t *pins) {
    cfs_bs_catalog_entry_t entry;
    cfs_bs_catalog_entry_t prior;
    cfs_bs_state_t state;
    uint32_t index;
    uint32_t earlier;
    int status;

    if (store == NULL || state_page == NULL || branches == NULL ||
        pins == NULL) {
        return CFS_BS_EINVAL;
    }

    status = cfs_bs_state_page_decode(store, state_page, &state);
    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bs_catalog_page_validate(store, branches, 0);
    if (status == CFS_BS_OK) {
        status = cfs_bs_catalog_page_validate(store, pins, 1);
    }

    if (status != CFS_BS_OK) {
        return status;
    }

    for (index = 0; index < CFS_BS_MAX_BRANCHES + CFS_BS_MAX_PINS;
        ++index) {
        status = cfs_bs_catalog_index_decode(store, branches, pins, index,
            &entry);
        if (status == CFS_BS_ENOENT) {
            continue;
        }

        if (status != CFS_BS_OK) {
            return status;
        }

        if ((entry.id >> 4) >= state.next_serial ||
            entry.revision >= state.next_revision ||
            (entry.origin_id != 0u &&
                ((entry.origin_id >> 4) >= state.next_serial ||
                    entry.origin_revision >= state.next_revision))) {
            return CFS_BS_ECORRUPT;
        }

        for (earlier = 0; earlier < index; ++earlier) {
            status = cfs_bs_catalog_index_decode(store, branches, pins,
                earlier, &prior);
            if (status == CFS_BS_ENOENT) {
                continue;
            }

            if (status != CFS_BS_OK) {
                return status;
            }

            if (entry.id == prior.id ||
                (entry.id >> 4) == (prior.id >> 4)) {
                return CFS_BS_ECORRUPT;
            }
        }
    }

    return CFS_BS_OK;
}

int
cfs_bs_row_entry_decode(const cfs_bs_store_t *store, uint64_t block,
    const uint8_t *entry, cfs_bs_row_t *out) {
    if (store == NULL || entry == NULL || out == NULL ||
        block >= store->total_blocks) {
        return CFS_BS_EINVAL;
    }

    cfs_bs_row_decode(entry, out);
    if (!cfs_bs_block_in_arena(store, block)) {
        return cfs_bs_bytes_zero(entry, CFS_BS_ROW_SIZE) ?
            CFS_BS_OK : CFS_BS_ECORRUPT;
    }

    if ((out->reserved != 0u && (out->reserved != CFS_BS_ROW_RELEASED ||
            out->state != CFS_BS_ROW_PENDING)) ||
        !cfs_bs_link_valid(store, out->next) ||
        (out->incarnation != 0u && !cfs_bs_incarnation_valid(out->incarnation)) ||
        (out->state != CFS_BS_ROW_FREE &&
            out->state != CFS_BS_ROW_LIVE &&
            out->state != CFS_BS_ROW_PENDING) ||
        (out->state == CFS_BS_ROW_LIVE &&
            (out->refcount == 0u || out->incarnation == 0u ||
                out->next != CFS_BS_BLOCK_NONE)) ||
        (out->state != CFS_BS_ROW_LIVE && out->refcount != 0u) ||
        (out->state == CFS_BS_ROW_PENDING && out->incarnation == 0u)) {
        return CFS_BS_ECORRUPT;
    }

    return CFS_BS_OK;
}

int
cfs_bs_row_page_validate(const cfs_bs_store_t *store, uint64_t page_block,
    const uint8_t *page) {
    cfs_bs_row_t row;
    uint64_t base;
    uint32_t index;
    int status;

    if (store == NULL || page == NULL || page_block < CFS_BS_ROW_START ||
        page_block >= store->semantic_start - CFS_BS_REDO_BLOCKS) {
        return CFS_BS_EINVAL;
    }

    base = (page_block - CFS_BS_ROW_START) * CFS_BS_ROWS_PER_BLOCK;
    for (index = 0; index < CFS_BS_ROWS_PER_BLOCK; ++index) {
        uint64_t block = base + index;
        const uint8_t *entry = page + CFS_BS_HEADER_SIZE +
            (size_t)index * CFS_BS_ROW_SIZE;

        if (block >= store->total_blocks) {
            if (!cfs_bs_bytes_zero(entry, CFS_BS_ROW_SIZE)) {
                return CFS_BS_ECORRUPT;
            }

            continue;
        }

        status = cfs_bs_row_entry_decode(store, block, entry, &row);
        if (status != CFS_BS_OK) {
            return status;
        }
    }

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
cfs_bs_inspect_state(cfs_bs_store_t *store, cfs_bs_state_t *out,
    cfs_bs_workspace_t *workspace) {
    int status;

    if (store == NULL || out == NULL || workspace == NULL) {
        return CFS_BS_EINVAL;
    }

    if (store->poisoned != 0u) {
        return CFS_BS_EPOISON;
    }

    status = cfs_bs_read_home(store, CFS_BS_STATE_BLOCK,
        workspace->scratch[0].bytes);
    if (status != CFS_BS_OK) {
        return status;
    }

    return cfs_bs_state_page_decode(store, workspace->scratch[0].bytes, out);
}

int
cfs_bs_inspect_branch(cfs_bs_store_t *store, uint32_t slot,
    cfs_bs_branch_t *out, cfs_bs_workspace_t *workspace) {
    cfs_bs_catalog_entry_t entry;
    int status;

    if (store == NULL || out == NULL || workspace == NULL ||
        slot >= CFS_BS_MAX_BRANCHES) {
        return CFS_BS_EINVAL;
    }

    if (store->poisoned != 0u) {
        return CFS_BS_EPOISON;
    }

    status = cfs_bs_read_home(store, CFS_BS_BRANCH_BLOCK,
        workspace->scratch[0].bytes);
    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bs_catalog_page_validate(store,
        workspace->scratch[0].bytes, 0);
    if (status == CFS_BS_OK) {
        status = cfs_bs_catalog_entry_decode_checked(store,
            workspace->scratch[0].bytes, slot, 0, &entry);
    }

    if (status != CFS_BS_OK) {
        return status;
    }

    cfs_bs_branch_from_entry(&entry, out);
    return CFS_BS_OK;
}

int
cfs_bs_inspect_pin(cfs_bs_store_t *store, uint32_t slot,
    cfs_bs_pin_t *out, cfs_bs_workspace_t *workspace) {
    cfs_bs_catalog_entry_t entry;
    int status;

    if (store == NULL || out == NULL || workspace == NULL ||
        slot >= CFS_BS_MAX_PINS) {
        return CFS_BS_EINVAL;
    }

    if (store->poisoned != 0u) {
        return CFS_BS_EPOISON;
    }

    status = cfs_bs_read_home(store, CFS_BS_PIN_BLOCK,
        workspace->scratch[0].bytes);
    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bs_catalog_page_validate(store,
        workspace->scratch[0].bytes, 1);
    if (status == CFS_BS_OK) {
        status = cfs_bs_catalog_entry_decode_checked(store,
            workspace->scratch[0].bytes, slot, 1, &entry);
    }

    if (status != CFS_BS_OK) {
        return status;
    }

    cfs_bs_pin_from_entry(&entry, out);
    return CFS_BS_OK;
}

int
cfs_bs_inspect_row(cfs_bs_store_t *store, uint64_t block,
    cfs_bs_row_t *out, cfs_bs_workspace_t *workspace) {
    const uint8_t *entry;
    int status;

    if (store == NULL || out == NULL || workspace == NULL ||
        block >= store->total_blocks) {
        return CFS_BS_EINVAL;
    }

    if (store->poisoned != 0u) {
        return CFS_BS_EPOISON;
    }

    status = cfs_bs_read_home(store, cfs_bs_row_page(block),
        workspace->scratch[0].bytes);
    if (status != CFS_BS_OK) {
        return status;
    }

    entry = workspace->scratch[0].bytes + CFS_BS_HEADER_SIZE +
        (size_t)cfs_bs_row_index(block) * CFS_BS_ROW_SIZE;
    return cfs_bs_row_entry_decode(store, block, entry, out);
}

int
cfs_bs_validate_root(cfs_bs_store_t *store, cfs_bs_ref_t root,
    cfs_bs_workspace_t *workspace) {
    cfs_bs_row_t row;
    uint32_t child_count;
    uint32_t payload_length;
    int status;

    if (cfs_bs_ref_is_null(root)) {
        return CFS_BS_OK;
    }

    status = cfs_bs_inspect_row(store, root.block, &row, workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    if (row.state != CFS_BS_ROW_LIVE ||
        row.incarnation != root.incarnation) {
        return CFS_BS_ECORRUPT;
    }

    status = cfs_bs_object_load(store, root, workspace->scratch[1].bytes,
        &child_count, &payload_length);
    return status == CFS_BS_EINVAL ? CFS_BS_ECORRUPT : status;
}

int
cfs_bs_branch_get(cfs_bs_store_t *store, uint64_t branch_id,
    cfs_bs_branch_t *out, cfs_bs_workspace_t *workspace) {
    cfs_bs_branch_t branch;
    int status;

    if (branch_id == 0u || out == NULL) {
        return CFS_BS_EINVAL;
    }

    status = cfs_bs_inspect_branch(store, (uint32_t)(branch_id & 15u),
        &branch, workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    if (branch.id != branch_id || branch.kind != CFS_BS_CATALOG_BRANCH) {
        return CFS_BS_ENOENT;
    }

    status = cfs_bs_validate_root(store, branch.root, workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    *out = branch;
    return CFS_BS_OK;
}

int
cfs_bs_inspect_stats(cfs_bs_store_t *store, cfs_bs_stats_t *out,
    cfs_bs_workspace_t *workspace) {
    cfs_bs_branch_t branch;
    cfs_bs_pin_t pin;
    cfs_bs_state_t state;
    uint32_t slot;
    int status;

    if (store == NULL || out == NULL || workspace == NULL) {
        return CFS_BS_EINVAL;
    }

    status = cfs_bs_inspect_state(store, &state, workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    memset(out, 0, sizeof(*out));
    out->generation = store->generation;
    out->total_blocks = store->total_blocks;
    out->semantic_start = store->semantic_start;
    out->free_count = state.free_count;
    out->pending_count = state.pending_count;
    out->reserve = state.reserve;
    for (slot = 0; slot < CFS_BS_MAX_BRANCHES; ++slot) {
        status = cfs_bs_inspect_branch(store, slot, &branch, workspace);
        if (status == CFS_BS_OK) {
            if (branch.kind == CFS_BS_CATALOG_STAGING) {
                out->staging_count++;
            } else {
                out->branch_count++;
            }
        } else if (status != CFS_BS_ENOENT) {
            return status;
        }

        status = cfs_bs_inspect_pin(store, slot, &pin, workspace);
        if (status == CFS_BS_OK) {
            out->pin_count++;
        } else if (status != CFS_BS_ENOENT) {
            return status;
        }
    }

    return CFS_BS_OK;
}
