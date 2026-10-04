/* SPDX-License-Identifier: ISC
 * branch_store_recovery.c -- bounded layered redo authentication and recovery.
 */
#include "branch_store_internal.h"

#include <string.h>

#include "codec.h"

static int bytes_zero(const uint8_t *bytes, size_t length) {
    size_t i;

    for (i = 0; i < length; i++) {
        if (bytes[i] != 0) {
            return 0;
        }
    }

    return 1;
}

static int checksum_valid(const uint8_t *page) {
    return cfs_get64(page + CFS_BS_HEADER_HASH_OFF) ==
           cfs_bs_block_hash(page);
}

static int read_decision(cfs_bs_store_t *store, uint64_t self,
    uint32_t kind, uint8_t *page, cfs_bs_decision_t *out, int *out_valid) {
    int rc;

    *out_valid = 0;
    rc = cfs_bs_io_read(store, self, page);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (kind == CFS_BS_KIND_COMMIT && bytes_zero(page, CFS_BLOCK_SIZE)) {
        return CFS_BS_OK;
    }

    if (!checksum_valid(page)) {
        return CFS_BS_OK;
    }

    rc = cfs_bs_decision_decode(store, self, kind, page, out);
    if (rc != CFS_BS_OK) {
        return CFS_BS_ECORRUPT;
    }

    *out_valid = 1;
    return CFS_BS_OK;
}

static int validate_bound_home(cfs_bs_store_t *store, uint64_t block,
    uint64_t expected_hash, uint64_t maximum_generation, int exact_generation,
    uint8_t *page) {
    uint64_t generation;
    int rc;

    rc = cfs_bs_read_home(store, block, page);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = cfs_bs_validate_home_page(store, block, page, &generation);
    if (rc != CFS_BS_OK ||
        cfs_get64(page + CFS_BS_HEADER_HASH_OFF) != expected_hash ||
        (exact_generation && generation != maximum_generation) ||
        (!exact_generation && generation > maximum_generation)) {
        return CFS_BS_ECORRUPT;
    }

    return CFS_BS_OK;
}

static int recover_clean(cfs_bs_store_t *store,
    const cfs_bs_decision_t *decision, cfs_bs_workspace_t *workspace) {
    int rc;

    rc = validate_bound_home(store, CFS_BS_STATE_BLOCK,
        decision->state_hash, decision->generation, 1,
        workspace->scratch[0].bytes);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = validate_bound_home(store, CFS_BS_BRANCH_BLOCK,
        decision->branch_hash, decision->generation, 0,
        workspace->scratch[1].bytes);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = validate_bound_home(store, CFS_BS_PIN_BLOCK,
        decision->pin_hash, decision->generation, 0,
        workspace->scratch[2].bytes);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = cfs_bs_catalog_pair_validate(store,
        workspace->scratch[0].bytes, workspace->scratch[1].bytes,
        workspace->scratch[2].bytes);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    return cfs_bs_validate_catalog_roots(store, workspace, 0,
        workspace->scratch[1].bytes, workspace->scratch[2].bytes,
        decision->generation);
}

static uint8_t *descriptor_raw(uint8_t *first, uint8_t *second,
    uint32_t index) {
    if (index < CFS_BS_DESC_PER_BLOCK) {
        return first + CFS_BS_HEADER_SIZE +
               (uint64_t)index * CFS_BS_DESC_SIZE;
    }

    return second + CFS_BS_HEADER_SIZE +
           (uint64_t)(index - CFS_BS_DESC_PER_BLOCK) * CFS_BS_DESC_SIZE;
}

static int descriptor_index(const cfs_bs_workspace_t *workspace,
    uint32_t count, uint64_t destination, uint32_t *out) {
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (workspace->destination[i] == destination) {
            if (out) {
                *out = i;
            }

            return 1;
        }
    }

    return 0;
}

static void prepared_ref(const uint8_t *decision, const uint8_t *continuation,
    uint32_t index, cfs_bs_ref_t *out) {
    const uint8_t *slot = index < CFS_BS_DECISION_INLINE_REFS ?
        decision + CFS_BS_DECISION_PREPARED_REF_OFF +
            (size_t)index * CFS_BS_REF_SIZE :
        continuation + CFS_BS_HEADER_SIZE +
            (size_t)(index - CFS_BS_DECISION_INLINE_REFS) * CFS_BS_REF_SIZE;
    cfs_bs_ref_decode(slot, out);
}

/* Bound bytes are checked before their addresses drive provider reads.
 * The decision's inline references were already checked by its decoder. */
static int validate_continuation(cfs_bs_store_t *store,
    const cfs_bs_decision_t *decision, const uint8_t *decision_page,
    const uint8_t *continuation, int semantic) {
    uint64_t generation = decision->generation;
    size_t used = CFS_BS_HEADER_SIZE +
        (size_t)(decision->prepared_count - CFS_BS_DECISION_INLINE_REFS) *
            CFS_BS_REF_SIZE;
    if (semantic && (cfs_bs_header_validate(continuation, CFS_BS_KIND_CONTINUATION,
            cfs_bs_redo_base(store, decision->generation) + 1u,
            store->uuid, &generation) != CFS_BS_OK ||
        generation != decision->generation ||
        !bytes_zero(continuation + used, CFS_BLOCK_SIZE - used))) {
        return CFS_BS_ECORRUPT;
    }

    for (uint32_t i = CFS_BS_DECISION_INLINE_REFS;
         i < decision->prepared_count; i++) {
        cfs_bs_ref_t ref;
        prepared_ref(decision_page, continuation, i, &ref);
        if (ref.block < store->semantic_start ||
            ref.block >= store->total_blocks ||
            ref.incarnation !=
                ((generation << CFS_BS_INCARNATION_INDEX_BITS) | i)) {
            return CFS_BS_ECORRUPT;
        }

        for (uint32_t prior = 0; prior < i; prior++) {
            cfs_bs_ref_t earlier;
            prepared_ref(decision_page, continuation, prior, &earlier);
            if (earlier.block == ref.block) {
                return CFS_BS_ECORRUPT;
            }
        }
    }

    return CFS_BS_OK;
}

/* A decision can persist ahead of its data. Select the highest complete
 * byte envelope before semantic validation; see r0125 section 7.5. */
static int recovery_envelope(cfs_bs_store_t *store,
    const cfs_bs_decision_t *decision, const uint8_t *decision_page,
    cfs_bs_workspace_t *workspace, uint32_t offset, int *complete) {
    uint64_t base = cfs_bs_redo_base(store, decision->generation);
    uint8_t *continuation =
        workspace->objects[2u + decision->generation % 2u].bytes;
    int rc;

    *complete = 0;
    rc = cfs_bs_io_read(store, base + 1u, continuation);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (decision->prepared_count <= CFS_BS_DECISION_INLINE_REFS) {
        if (!bytes_zero(continuation, CFS_BLOCK_SIZE)) {
            return CFS_BS_OK;
        }
    } else {
        if (!checksum_valid(continuation) ||
            cfs_get64(continuation) != decision->continuation_hash) {
            return CFS_BS_OK;
        }

        rc = validate_continuation(store, decision, decision_page, continuation, 0);
        if (rc != CFS_BS_OK) {
            return rc;
        }
    }

    for (uint32_t i = 0; i < 2; i++) {
        uint8_t *page = workspace->scratch[i].bytes;

        rc = cfs_bs_io_read(store, base + 2 + i, page);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        if (!checksum_valid(page) || cfs_get64(page) != decision->desc_hash[i]) {
            return CFS_BS_OK;
        }
    }

    for (uint32_t i = 0; i < decision->count; i++) {
        cfs_bs_descriptor_t descriptor;
        uint8_t *page = workspace->pages[offset + i].bytes;

        cfs_bs_descriptor_decode(descriptor_raw(workspace->scratch[0].bytes,
            workspace->scratch[1].bytes, i), &descriptor);
        rc = cfs_bs_io_read(store, base + 4 + i, page);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        if (!checksum_valid(page) || cfs_get64(page) != descriptor.after_hash) {
            return CFS_BS_OK;
        }
    }

    for (uint32_t i = 0; i < decision->prepared_count; i++) {
        uint8_t *page = workspace->scratch[2].bytes;
        cfs_bs_ref_t prepared;

        prepared_ref(decision_page, continuation, i, &prepared);
        rc = cfs_bs_io_read(store, prepared.block, page);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        if (!checksum_valid(page) || cfs_get64(page) != prepared.hash) {
            return CFS_BS_OK;
        }
    }

    *complete = 1;
    return CFS_BS_OK;
}

static int validate_recovery_descriptors(cfs_bs_store_t *store,
    const cfs_bs_decision_t *decision, cfs_bs_workspace_t *workspace,
    uint32_t offset) {
    uint8_t *first = workspace->scratch[0].bytes;
    uint8_t *second = workspace->scratch[1].bytes;
    uint32_t first_used;
    uint32_t second_used;
    uint32_t i;
    uint64_t base = cfs_bs_redo_base(store, decision->generation);
    int has_state = 0;
    int rc;

    if (cfs_bs_header_validate(first, CFS_BS_KIND_DESC,
            base + 2, store->uuid, NULL) != CFS_BS_OK ||
        cfs_bs_header_validate(second, CFS_BS_KIND_DESC,
            base + 3, store->uuid, NULL) != CFS_BS_OK ||
        cfs_get64(first + CFS_BS_HEADER_GENERATION_OFF) !=
            decision->generation ||
        cfs_get64(second + CFS_BS_HEADER_GENERATION_OFF) !=
            decision->generation ||
        cfs_get64(first + CFS_BS_HEADER_HASH_OFF) !=
            decision->desc_hash[0] ||
        cfs_get64(second + CFS_BS_HEADER_HASH_OFF) !=
            decision->desc_hash[1]) {
        return CFS_BS_ECORRUPT;
    }

    first_used = decision->count < CFS_BS_DESC_PER_BLOCK ?
                     decision->count : CFS_BS_DESC_PER_BLOCK;
    second_used = decision->count > CFS_BS_DESC_PER_BLOCK ?
                      decision->count - CFS_BS_DESC_PER_BLOCK : 0;
    if (!bytes_zero(first + CFS_BS_HEADER_SIZE +
                        (uint64_t)first_used * CFS_BS_DESC_SIZE,
                    (CFS_BS_DESC_PER_BLOCK - first_used) *
                        CFS_BS_DESC_SIZE) ||
        !bytes_zero(second + CFS_BS_HEADER_SIZE +
                        (uint64_t)second_used * CFS_BS_DESC_SIZE,
                    (CFS_BS_DESC_PER_BLOCK - second_used) *
                        CFS_BS_DESC_SIZE)) {
        return CFS_BS_ECORRUPT;
    }

    for (i = 0; i < decision->count; i++) {
        cfs_bs_descriptor_t descriptor;
        uint64_t generation;
        uint32_t j;

        cfs_bs_descriptor_decode(descriptor_raw(first, second, i),
                                 &descriptor);
        if (cfs_bs_home_kind(store, descriptor.destination) == 0 ||
            descriptor.before_generation == 0 ||
            descriptor.before_generation >
                decision->predecessor_generation) {
            return CFS_BS_ECORRUPT;
        }

        for (j = 0; j < i; j++) {
            if (workspace->destination[offset + j] == descriptor.destination) {
                return CFS_BS_ECORRUPT;
            }
        }

        workspace->destination[offset + i] = descriptor.destination;
        workspace->before_generation[offset + i] = descriptor.before_generation;
        workspace->before_hash[offset + i] = descriptor.before_hash;
        rc = cfs_bs_validate_home_page(store, descriptor.destination,
                                workspace->pages[offset + i].bytes, &generation);
        if (rc != CFS_BS_OK || generation != decision->generation ||
            cfs_get64(workspace->pages[offset + i].bytes +
                          CFS_BS_HEADER_HASH_OFF) != descriptor.after_hash) {
            return CFS_BS_ECORRUPT;
        }

        workspace->loaded[offset + i] = 1;
        has_state |= descriptor.destination == CFS_BS_STATE_BLOCK;
    }

    if (!has_state) {
        return CFS_BS_ECORRUPT;
    }

    return CFS_BS_OK;
}

static int validate_successor_roots(cfs_bs_store_t *store,
    const cfs_bs_decision_t *decision, cfs_bs_workspace_t *workspace,
    uint32_t count) {
    const uint8_t *pages[3];
    uint64_t expected[3];
    uint64_t blocks[3];
    uint32_t i;
    int rc;

    blocks[0] = CFS_BS_STATE_BLOCK;
    blocks[1] = CFS_BS_BRANCH_BLOCK;
    blocks[2] = CFS_BS_PIN_BLOCK;
    expected[0] = decision->state_hash;
    expected[1] = decision->branch_hash;
    expected[2] = decision->pin_hash;
    for (i = 0; i < 3; i++) {
        rc = cfs_bs_effective_home(store, workspace, count, blocks[i],
                            decision->generation, workspace->scratch[i].bytes, &pages[i]);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        if (cfs_get64(pages[i] + CFS_BS_HEADER_HASH_OFF) != expected[i]) {
            return CFS_BS_ECORRUPT;
        }
    }

    rc = cfs_bs_catalog_pair_validate(store, pages[0], pages[1], pages[2]);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    return cfs_bs_validate_catalog_roots(store, workspace, count,
        pages[1], pages[2], decision->generation);
}

static int validate_current_homes(cfs_bs_store_t *store,
    uint32_t count, cfs_bs_workspace_t *workspace) {
    uint32_t i;
    int rc;

    for (i = 0; i < count; i++) {
        uint8_t *current = workspace->scratch[0].bytes;
        uint64_t current_hash;
        uint64_t current_generation;
        uint64_t after_hash = cfs_get64(workspace->pages[i].bytes +
                                        CFS_BS_HEADER_HASH_OFF);
        uint64_t after_generation = cfs_get64(workspace->pages[i].bytes +
            CFS_BS_HEADER_GENERATION_OFF);

        rc = cfs_bs_io_read(store, workspace->destination[i], current);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        if (!checksum_valid(current)) {
            workspace->dirty[i] = 1;
            continue;
        }

        rc = cfs_bs_validate_home_page(store, workspace->destination[i], current,
                                &current_generation);
        if (rc != CFS_BS_OK) {
            return CFS_BS_ECORRUPT;
        }

        current_hash = cfs_get64(current + CFS_BS_HEADER_HASH_OFF);
        if (current_hash == after_hash &&
            current_generation == after_generation) {
            workspace->dirty[i] = 0;
        } else if (current_hash == workspace->before_hash[i] &&
                   current_generation == workspace->before_generation[i]) {
            workspace->dirty[i] = 1;
        } else if (workspace->middle_generation[i] != 0 &&
                   current_hash == workspace->middle_hash[i] &&
                   current_generation == workspace->middle_generation[i]) {
            workspace->dirty[i] = 1;
        } else {
            return CFS_BS_ECORRUPT;
        }
    }

    return CFS_BS_OK;
}

static int recover_commit(cfs_bs_store_t *store,
    const cfs_bs_decision_t *decision, cfs_bs_workspace_t *workspace,
    uint32_t count, int clean_exact) {
    uint32_t i;
    int changed = !clean_exact;
    int rc;

    rc = validate_successor_roots(store, decision, workspace, count);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = cfs_bs_validate_prepared(store, workspace, count,
                                   decision->prepared_count,
                                   decision->generation, 0);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = validate_current_homes(store, count, workspace);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    for (i = 0; i < count; i++) {
        if (workspace->dirty[i] == 0) {
            continue;
        }

        changed = 1;
        rc = cfs_bs_io_write(store, workspace->destination[i],
                             workspace->pages[i].bytes);
        if (rc != CFS_BS_OK) {
            return rc;
        }
    }

    if (!changed) {
        return CFS_BS_OK;
    }

    cfs_bs_clean_frame(store, decision->generation, decision->state_hash,
                       decision->branch_hash, decision->pin_hash,
                       workspace->scratch[0].bytes);
    rc = cfs_bs_io_write(store, CFS_BS_CLEAN_BLOCK,
                         workspace->scratch[0].bytes);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    return cfs_bs_io_sync(store);
}

static int validate_previous_roots(const cfs_bs_decision_t *selected,
    const cfs_bs_decision_t *previous, const cfs_bs_workspace_t *workspace) {
    const uint64_t selected_hash[3] = {selected->state_hash,
        selected->branch_hash, selected->pin_hash};
    const uint64_t previous_hash[3] = {previous->state_hash,
        previous->branch_hash, previous->pin_hash};

    for (uint32_t root = 0; root < 3u; root++) {
        uint64_t block = CFS_BS_STATE_BLOCK + root;
        uint64_t hash = selected_hash[root];
        uint32_t index;
        if (descriptor_index(workspace, selected->count, block, &index)) {
            hash = workspace->before_hash[index];
        }

        for (uint32_t i = 0; i < previous->count; i++) {
            index = CFS_BS_TX_MAX_PAGES + i;
            if (workspace->destination[index] == block) {
                hash = cfs_get64(workspace->pages[index].bytes);
                break;
            }
        }

        if (hash != previous_hash[root]) {
            return CFS_BS_ECORRUPT;
        }
    }

    return CFS_BS_OK;
}

/* Keep the selected journal first; append predecessor-only homes and retain
 * the exact three-point chain for overlapping destinations. */
static int recovery_union(cfs_bs_store_t *store,
    const cfs_bs_decision_t *selected, const cfs_bs_decision_t *previous,
    const uint8_t *previous_page, cfs_bs_workspace_t *workspace,
    uint32_t *out_count, int *out_previous) {
    uint32_t count = selected->count;
    int complete;
    int rc = validate_recovery_descriptors(store, selected, workspace, 0);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (selected->prepared_count > CFS_BS_DECISION_INLINE_REFS) {
        uint32_t index = (uint32_t)(selected->generation % 2u);
        rc = validate_continuation(store, selected,
            workspace->objects[index].bytes,
            workspace->objects[2u + index].bytes, 1);
        if (rc != CFS_BS_OK) {
            return rc;
        }
    }

    *out_count = count;
    *out_previous = 0;
    if (!previous) {
        return CFS_BS_OK;
    }

    rc = recovery_envelope(store, previous, previous_page, workspace,
        CFS_BS_TX_MAX_PAGES, &complete);
    if (rc != CFS_BS_OK || !complete) {
        return rc;
    }

    rc = validate_recovery_descriptors(store, previous, workspace,
        CFS_BS_TX_MAX_PAGES);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (previous->prepared_count > CFS_BS_DECISION_INLINE_REFS) {
        uint32_t index = (uint32_t)(previous->generation % 2u);
        rc = validate_continuation(store, previous, previous_page,
            workspace->objects[2u + index].bytes, 1);
        if (rc != CFS_BS_OK) {
            return rc;
        }
    }

    rc = validate_previous_roots(selected, previous, workspace);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    for (uint32_t i = 0; i < previous->count; i++) {
        uint32_t source = CFS_BS_TX_MAX_PAGES + i;
        uint32_t destination;
        if (descriptor_index(workspace, selected->count,
                workspace->destination[source], &destination)) {
            uint64_t hash = cfs_get64(workspace->pages[source].bytes);
            if (workspace->before_generation[destination] !=
                    previous->generation ||
                workspace->before_hash[destination] != hash) {
                return CFS_BS_ECORRUPT;
            }

            workspace->middle_generation[destination] = previous->generation;
            workspace->middle_hash[destination] = hash;
        } else {
            destination = count++;
            if (destination != source) {
                memcpy(workspace->pages[destination].bytes,
                    workspace->pages[source].bytes, CFS_BLOCK_SIZE);
            }

            workspace->destination[destination] = workspace->destination[source];
            workspace->loaded[destination] = 1;
        }

        workspace->before_generation[destination] =
            workspace->before_generation[source];
        workspace->before_hash[destination] = workspace->before_hash[source];
    }

    *out_count = count;
    *out_previous = 1;
    return CFS_BS_OK;
}

int cfs_bs_recover(cfs_bs_store_t *store, cfs_bs_workspace_t *workspace) {
    cfs_bs_decision_t clean;
    cfs_bs_decision_t commit[2];
    const cfs_bs_decision_t *selected = NULL;
    int clean_valid;
    int commit_valid[2];
    uint32_t first = 0;
    int rc;

    if (store == NULL || workspace == NULL || store->total_blocks <
            CFS_BS_MIN_BLOCKS || store->semantic_start !=
            cfs_bs_semantic_start(store->total_blocks)) {
        return CFS_BS_EINVAL;
    }

    /* Media may have changed beneath an attached cache. */
    cfs_bs_cache_invalidate(store);

    if (store->lease_owner) {
        return CFS_BS_EBUSY;
    }

    if (store->poisoned != 0) {
        return CFS_BS_EPOISON;
    }

    store->generation = 0;
    memset(workspace->loaded, 0, sizeof(workspace->loaded));
    memset(workspace->dirty, 0, sizeof(workspace->dirty));
    memset(workspace->middle_generation, 0, sizeof(workspace->middle_generation));
    rc = read_decision(store, CFS_BS_CLEAN_BLOCK, CFS_BS_KIND_CLEAN,
                       workspace->scratch[0].bytes, &clean, &clean_valid);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    /* Keep both decision bytes outside descriptor scratch until selection.
     * Graph validation may reuse these object slots only after selection. */
    for (uint32_t i = 0; i < 2; i++) {
        rc = read_decision(store, cfs_bs_redo_base(store, i),
            CFS_BS_KIND_COMMIT, workspace->objects[i].bytes,
            &commit[i], &commit_valid[i]);
        if (rc != CFS_BS_OK) {
            return rc;
        }
    }

    if (commit_valid[1] && (!commit_valid[0] ||
            commit[1].generation > commit[0].generation)) {
        first = 1;
    }

    if (commit_valid[0] && commit_valid[1] &&
        commit[first].generation != commit[1 - first].generation + 1) {
        return CFS_BS_ECORRUPT;
    }

    for (uint32_t attempt = 0; attempt < 2; attempt++) {
        uint32_t i = attempt == 0 ? first : 1 - first;
        int complete;

        if (!commit_valid[i]) {
            continue;
        }

        rc = recovery_envelope(store, &commit[i],
            workspace->objects[i].bytes, workspace, 0, &complete);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        if (complete) {
            selected = &commit[i];
            break;
        }
    }

    if (!selected) {
        if (!clean_valid || !cfs_bs_genesis_matches(store, &clean,
                workspace->scratch[0].bytes)) {
            return CFS_BS_ECORRUPT;
        }

        selected = &clean;
        rc = recover_clean(store, &clean, workspace);
    } else {
        uint32_t index = selected == &commit[0] ? 0u : 1u;
        uint32_t count;
        int previous_complete;
        const cfs_bs_decision_t *previous = commit_valid[1u - index] &&
            commit[1u - index].generation + 1u == selected->generation ?
            &commit[1u - index] : NULL;
        rc = recovery_union(store, selected, previous,
            workspace->objects[1u - index].bytes, workspace, &count,
            &previous_complete);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        if (clean_valid &&
            !cfs_bs_decisions_consistent(&clean, selected) &&
            !(previous_complete && clean.generation + 2u == selected->generation)) {
            return CFS_BS_ECORRUPT;
        }

        if (clean_valid && previous_complete &&
            clean.generation == previous->generation &&
            !cfs_bs_decisions_consistent(&clean, previous)) {
            return CFS_BS_ECORRUPT;
        }

        for (uint32_t i = 0; i < selected->prepared_count; i++) {
            prepared_ref(workspace->objects[index].bytes,
                workspace->objects[2u + index].bytes, i, &workspace->prepared[i]);
        }

        rc = recover_commit(store, selected, workspace, count,
            clean_valid && clean.generation == selected->generation);
    }

    if (rc != CFS_BS_OK) {
        return rc;
    }

    /* A retry can reuse this generation and reproduce only the missing
     * payload of a different old proposal. Retire its decision first. */
    int abandoned = 0;
    for (uint32_t i = 0; i < 2; i++) {
        if (!commit_valid[i] ||
            commit[i].generation <= selected->generation) {
            continue;
        }

        memset(workspace->scratch[0].bytes, 0, CFS_BLOCK_SIZE);
        rc = cfs_bs_io_write(store, cfs_bs_redo_base(store,
            commit[i].generation), workspace->scratch[0].bytes);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        abandoned = 1;
    }

    if (abandoned) {
        rc = cfs_bs_io_sync(store);
        if (rc != CFS_BS_OK) {
            return rc;
        }
    }

    store->generation = selected->generation;
    return CFS_BS_OK;
}
