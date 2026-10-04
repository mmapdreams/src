/* SPDX-License-Identifier: ISC
 *
 * branch_store_format.c -- byte framing, fresh-image formatting, and
 * immutable geometry opening for the bounded persistent branch store.
 */

#include "branch_store_internal.h"

#include <string.h>

#include "block.h"
#include "codec.h"

_Static_assert(sizeof(cfs_bs_ref_t) == CFS_BS_REF_SIZE,
    "branch-store reference size");
_Static_assert(sizeof(cfs_bs_row_t) == CFS_BS_ROW_SIZE,
    "branch-store row size");
_Static_assert(CFS_BS_HEADER_SIZE +
        CFS_BS_ROWS_PER_BLOCK * CFS_BS_ROW_SIZE == CFS_BLOCK_SIZE,
    "branch-store ownership page geometry");
_Static_assert(CFS_BS_HEADER_SIZE +
        CFS_BS_DESC_PER_BLOCK * CFS_BS_DESC_SIZE == CFS_BLOCK_SIZE,
    "branch-store descriptor page geometry");

_Static_assert(CFS_BS_DECISION_PREPARED_REF_OFF +
        CFS_BS_DECISION_INLINE_REFS * CFS_BS_REF_SIZE == CFS_BLOCK_SIZE,
    "branch-store inline reference geometry");
_Static_assert(CFS_BS_HEADER_SIZE +
        (CFS_BS_CONSTRUCT_MAX_OBJECTS - CFS_BS_DECISION_INLINE_REFS) *
            CFS_BS_REF_SIZE <= CFS_BLOCK_SIZE,
    "branch-store continuation holds the remaining prepared references");

/* A list index rides in the low bits of an incarnation, so a ceiling past
 * that field would alias two objects onto one ordinal. */
_Static_assert(CFS_BS_CONSTRUCT_MAX_OBJECTS <= CFS_BS_INCARNATION_INDEX_MAX,
    "branch-store prepared index addresses every constructed object");

typedef struct bs_super_fields {
    uint8_t uuid[CFS_BS_UUID_SIZE];
    uint64_t total_blocks;
    uint64_t row_blocks;
    uint64_t semantic_start;
    uint64_t reserve;
} bs_super_fields_t;

static int bytes_zero(const uint8_t *bytes, size_t length) {
    size_t i;

    for (i = 0; i < length; i++) {
        if (bytes[i] != 0) {
            return 0;
        }
    }

    return 1;
}

static int uuid_equal(const uint8_t *a, const uint8_t *b) {
    size_t i;

    for (i = 0; i < CFS_BS_UUID_SIZE; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }

    return 1;
}

uint64_t cfs_bs_row_block_count(uint64_t total_blocks) {
    return total_blocks / CFS_BS_ROWS_PER_BLOCK +
           (total_blocks % CFS_BS_ROWS_PER_BLOCK != 0 ? 1u : 0u);
}

uint64_t cfs_bs_semantic_start(uint64_t total_blocks) {
    return CFS_BS_ROW_START + cfs_bs_row_block_count(total_blocks) +
        CFS_BS_REDO_BLOCKS;
}

uint64_t cfs_bs_redo_base(const cfs_bs_store_t *store, uint64_t generation) {
    return generation % 2 == 0 ? CFS_BS_COMMIT_BLOCK :
        store->semantic_start - CFS_BS_REDO_BLOCKS;
}

uint64_t cfs_bs_row_page(uint64_t physical_block) {
    return CFS_BS_ROW_START + physical_block / CFS_BS_ROWS_PER_BLOCK;
}

uint32_t cfs_bs_row_index(uint64_t physical_block) {
    return (uint32_t)(physical_block % CFS_BS_ROWS_PER_BLOCK);
}

uint64_t cfs_bs_block_hash(const uint8_t block[CFS_BLOCK_SIZE]) {
    if (!block) {
        return 0;
    }

    return cfs_hash64(block + sizeof(uint64_t),
                      CFS_BLOCK_SIZE - sizeof(uint64_t));
}

int cfs_bs_ref_is_null(cfs_bs_ref_t ref) {
    return ref.block == CFS_BS_BLOCK_NONE && ref.incarnation == 0 &&
           ref.hash == 0;
}

int cfs_bs_incarnation_valid(uint64_t incarnation) {
    return (incarnation >> CFS_BS_INCARNATION_INDEX_BITS) != 0 &&
        (incarnation & CFS_BS_INCARNATION_INDEX_MASK) <
            CFS_BS_CONSTRUCT_MAX_OBJECTS;
}

int cfs_bs_ref_equal(cfs_bs_ref_t a, cfs_bs_ref_t b) {
    return a.block == b.block && a.incarnation == b.incarnation &&
           a.hash == b.hash;
}

void cfs_bs_ref_decode(const uint8_t *src, cfs_bs_ref_t *out) {
    if (src == NULL || out == NULL) {
        return;
    }

    out->block = cfs_get64(src);
    out->incarnation = cfs_get64(src + 8);
    out->hash = cfs_get64(src + 16);
}

void cfs_bs_ref_encode(uint8_t *dst, cfs_bs_ref_t ref) {
    if (!dst) {
        return;
    }

    cfs_put64(dst, ref.block);
    cfs_put64(dst + 8, ref.incarnation);
    cfs_put64(dst + 16, ref.hash);
}

void cfs_bs_descriptor_decode(const uint8_t *src,
    cfs_bs_descriptor_t *out) {
    if (src == NULL || out == NULL) {
        return;
    }

    out->destination = cfs_get64(src + CFS_BS_DESC_DESTINATION_OFF);
    out->before_generation =
        cfs_get64(src + CFS_BS_DESC_BEFORE_GENERATION_OFF);
    out->before_hash = cfs_get64(src + CFS_BS_DESC_BEFORE_HASH_OFF);
    out->after_hash = cfs_get64(src + CFS_BS_DESC_AFTER_HASH_OFF);
}

void cfs_bs_descriptor_encode(uint8_t *dst,
    const cfs_bs_descriptor_t *descriptor) {
    if (dst == NULL || descriptor == NULL) {
        return;
    }

    cfs_put64(dst + CFS_BS_DESC_DESTINATION_OFF, descriptor->destination);
    cfs_put64(dst + CFS_BS_DESC_BEFORE_GENERATION_OFF,
              descriptor->before_generation);
    cfs_put64(dst + CFS_BS_DESC_BEFORE_HASH_OFF,
              descriptor->before_hash);
    cfs_put64(dst + CFS_BS_DESC_AFTER_HASH_OFF, descriptor->after_hash);
}

void cfs_bs_header_rehash(uint8_t block[CFS_BLOCK_SIZE]) {
    if (!block) {
        return;
    }

    cfs_put64(block + CFS_BS_HEADER_HASH_OFF, cfs_bs_block_hash(block));
}

void cfs_bs_header_frame(uint8_t block[CFS_BLOCK_SIZE], uint32_t kind,
    uint64_t self, uint64_t generation,
    const uint8_t uuid[CFS_BS_UUID_SIZE]) {
    if (block == NULL || uuid == NULL) {
        return;
    }

    cfs_put64(block + CFS_BS_HEADER_HASH_OFF, 0);
    cfs_put64(block + CFS_BS_HEADER_MAGIC_OFF, CFS_BS_MAGIC);
    cfs_put32(block + CFS_BS_HEADER_KIND_OFF, kind);
    cfs_put32(block + CFS_BS_HEADER_VERSION_OFF, CFS_BS_VERSION);
    cfs_put64(block + CFS_BS_HEADER_SELF_OFF, self);
    cfs_put64(block + CFS_BS_HEADER_GENERATION_OFF, generation);
    memcpy(block + CFS_BS_HEADER_UUID_OFF, uuid, CFS_BS_UUID_SIZE);
    memset(block + CFS_BS_HEADER_RESERVED_OFF, 0, 8);
    cfs_bs_header_rehash(block);
}

int cfs_bs_header_validate(const uint8_t block[CFS_BLOCK_SIZE],
    uint32_t expected_kind, uint64_t expected_self,
    const uint8_t uuid[CFS_BS_UUID_SIZE], uint64_t *out_generation) {
    uint64_t generation;

    if (block == NULL || uuid == NULL) {
        return CFS_BS_EINVAL;
    }

    if (cfs_get64(block + CFS_BS_HEADER_HASH_OFF) !=
            cfs_bs_block_hash(block) ||
        cfs_get64(block + CFS_BS_HEADER_MAGIC_OFF) != CFS_BS_MAGIC ||
        cfs_get32(block + CFS_BS_HEADER_KIND_OFF) != expected_kind ||
        cfs_get32(block + CFS_BS_HEADER_VERSION_OFF) != CFS_BS_VERSION ||
        cfs_get64(block + CFS_BS_HEADER_SELF_OFF) != expected_self ||
        !uuid_equal(block + CFS_BS_HEADER_UUID_OFF, uuid) ||
        !bytes_zero(block + CFS_BS_HEADER_RESERVED_OFF, 8)) {
        return CFS_BS_ECORRUPT;
    }

    generation = cfs_get64(block + CFS_BS_HEADER_GENERATION_OFF);
    if (generation == 0 || generation > CFS_BS_GENERATION_MAX) {
        return CFS_BS_ECORRUPT;
    }

    if (out_generation) {
        *out_generation = generation;
    }

    return CFS_BS_OK;
}

int cfs_bs_decision_decode(const cfs_bs_store_t *store, uint64_t self,
    uint32_t kind, const uint8_t *page, cfs_bs_decision_t *out) {
    uint64_t generation;
    uint32_t i;
    uint32_t inline_count;
    uint64_t expected_phase;
    int rc;

    rc = cfs_bs_header_validate(page, kind, self, store->uuid, &generation);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    memset(out, 0, sizeof(*out));
    out->kind = kind == CFS_BS_KIND_CLEAN ? CFS_BS_DECISION_CLEAN :
                                            CFS_BS_DECISION_COMMIT;
    out->generation = generation;
    out->phase = cfs_get64(page + CFS_BS_DECISION_PHASE_OFF);
    out->desc_hash[0] =
        cfs_get64(page + CFS_BS_DECISION_DESC0_HASH_OFF);
    out->desc_hash[1] =
        cfs_get64(page + CFS_BS_DECISION_DESC1_HASH_OFF);
    out->count = cfs_get32(page + CFS_BS_DECISION_COUNT_OFF);
    out->prepared_count =
        cfs_get32(page + CFS_BS_DECISION_PREPARED_COUNT_OFF);
    out->state_hash = cfs_get64(page + CFS_BS_DECISION_STATE_HASH_OFF);
    out->branch_hash = cfs_get64(page + CFS_BS_DECISION_BRANCH_HASH_OFF);
    out->pin_hash = cfs_get64(page + CFS_BS_DECISION_PIN_HASH_OFF);
    out->predecessor_generation =
        cfs_get64(page + CFS_BS_DECISION_PREDECESSOR_OFF);
    out->continuation_hash =
        cfs_get64(page + CFS_BS_DECISION_CONTINUATION_HASH_OFF);
    expected_phase = kind == CFS_BS_KIND_CLEAN ? generation * 2 :
                                                generation * 2 - 1;
    if (out->prepared_count > CFS_BS_CONSTRUCT_MAX_OBJECTS) {
        return CFS_BS_ECORRUPT;
    }

    inline_count = out->prepared_count < CFS_BS_DECISION_INLINE_REFS ?
        out->prepared_count : CFS_BS_DECISION_INLINE_REFS;
    if ((out->prepared_count <= CFS_BS_DECISION_INLINE_REFS &&
            out->continuation_hash != 0) ||
        !bytes_zero(page + CFS_BS_DECISION_PREPARED_REF_OFF +
                        (size_t)inline_count * CFS_BS_REF_SIZE,
                    CFS_BLOCK_SIZE - CFS_BS_DECISION_PREPARED_REF_OFF -
                        (size_t)inline_count * CFS_BS_REF_SIZE)) {
        return CFS_BS_ECORRUPT;
    }

    for (i = 0; i < inline_count; i++) {
        cfs_bs_ref_t ref;

        cfs_bs_ref_decode(page + CFS_BS_DECISION_PREPARED_REF_OFF +
                              (size_t)i * CFS_BS_REF_SIZE, &ref);
        if (ref.block < store->semantic_start ||
            ref.block >= store->total_blocks ||
            ref.incarnation != ((generation << CFS_BS_INCARNATION_INDEX_BITS) | i)) {
            return CFS_BS_ECORRUPT;
        }

        for (uint32_t prior = 0; prior < i; prior++) {
            if (cfs_get64(page + CFS_BS_DECISION_PREPARED_REF_OFF +
                    (size_t)prior * CFS_BS_REF_SIZE) == ref.block) {
                return CFS_BS_ECORRUPT;
            }
        }
    }

    if (out->phase != expected_phase) {
        return CFS_BS_ECORRUPT;
    }

    if (kind == CFS_BS_KIND_CLEAN) {
        if (out->desc_hash[0] != 0 || out->desc_hash[1] != 0 ||
            out->count != 0 || out->prepared_count != 0 ||
            out->predecessor_generation != generation) {
            return CFS_BS_ECORRUPT;
        }
    } else if (generation < 2 || self != cfs_bs_redo_base(store, generation) ||
               out->count == 0 ||
               out->count > CFS_BS_TX_MAX_PAGES ||
               out->predecessor_generation != generation - 1) {
        return CFS_BS_ECORRUPT;
    }

    return CFS_BS_OK;
}

void cfs_bs_clean_frame(const cfs_bs_store_t *store, uint64_t generation,
    uint64_t state_hash, uint64_t branch_hash, uint64_t pin_hash,
    uint8_t *page) {
    memset(page, 0, CFS_BLOCK_SIZE);
    cfs_put64(page + CFS_BS_DECISION_PHASE_OFF, generation * 2);
    cfs_put64(page + CFS_BS_DECISION_STATE_HASH_OFF, state_hash);
    cfs_put64(page + CFS_BS_DECISION_BRANCH_HASH_OFF, branch_hash);
    cfs_put64(page + CFS_BS_DECISION_PIN_HASH_OFF, pin_hash);
    cfs_put64(page + CFS_BS_DECISION_PREDECESSOR_OFF, generation);
    cfs_bs_header_frame(page, CFS_BS_KIND_CLEAN, CFS_BS_CLEAN_BLOCK,
                        generation, store->uuid);
}

void cfs_bs_commit_frame(const cfs_bs_store_t *store, uint64_t generation,
    uint32_t count, const cfs_bs_ref_t *prepared, uint32_t prepared_count,
    uint64_t state_hash,
    uint64_t branch_hash, uint64_t pin_hash, const uint8_t *first_desc,
    const uint8_t *second_desc, uint8_t *continuation, uint8_t *page) {
    memset(page, 0, CFS_BLOCK_SIZE);
    memset(continuation, 0, CFS_BLOCK_SIZE);
    cfs_put64(page + CFS_BS_DECISION_PHASE_OFF, generation * 2 - 1);
    cfs_put64(page + CFS_BS_DECISION_DESC0_HASH_OFF,
              cfs_get64(first_desc + CFS_BS_HEADER_HASH_OFF));
    cfs_put64(page + CFS_BS_DECISION_DESC1_HASH_OFF,
              cfs_get64(second_desc + CFS_BS_HEADER_HASH_OFF));
    cfs_put32(page + CFS_BS_DECISION_COUNT_OFF, count);
    cfs_put32(page + CFS_BS_DECISION_PREPARED_COUNT_OFF,
              prepared_count);
    cfs_put64(page + CFS_BS_DECISION_STATE_HASH_OFF, state_hash);
    cfs_put64(page + CFS_BS_DECISION_BRANCH_HASH_OFF, branch_hash);
    cfs_put64(page + CFS_BS_DECISION_PIN_HASH_OFF, pin_hash);
    for (uint32_t i = 0; i < prepared_count; i++) {
        uint8_t *slot = i < CFS_BS_DECISION_INLINE_REFS ?
            page + CFS_BS_DECISION_PREPARED_REF_OFF +
                (size_t)i * CFS_BS_REF_SIZE :
            continuation + CFS_BS_HEADER_SIZE +
                (size_t)(i - CFS_BS_DECISION_INLINE_REFS) * CFS_BS_REF_SIZE;
        cfs_bs_ref_encode(slot, prepared[i]);
    }

    if (prepared_count > CFS_BS_DECISION_INLINE_REFS) {
        cfs_bs_header_frame(continuation, CFS_BS_KIND_CONTINUATION,
            cfs_bs_redo_base(store, generation) + 1u, generation, store->uuid);
        cfs_put64(page + CFS_BS_DECISION_CONTINUATION_HASH_OFF,
            cfs_get64(continuation));
    }

    cfs_put64(page + CFS_BS_DECISION_PREDECESSOR_OFF, generation - 1);
    cfs_bs_header_frame(page, CFS_BS_KIND_COMMIT,
                        cfs_bs_redo_base(store, generation),
                        generation, store->uuid);
}

int cfs_bs_decisions_consistent(const cfs_bs_decision_t *clean,
    const cfs_bs_decision_t *commit) {
    if (clean->phase == commit->phase + 1) {
        return clean->generation == commit->generation &&
               commit->predecessor_generation + 1 == clean->generation &&
               clean->state_hash == commit->state_hash &&
               clean->branch_hash == commit->branch_hash &&
               clean->pin_hash == commit->pin_hash;
    }

    if (commit->phase == clean->phase + 1) {
        return commit->generation == clean->generation + 1 &&
               commit->predecessor_generation == clean->generation;
    }

    return 0;
}

static uint8_t *tx_descriptor_raw(uint8_t *first, uint8_t *second,
    uint32_t index) {
    if (index < CFS_BS_DESC_PER_BLOCK) {
        return first + CFS_BS_HEADER_SIZE +
               (uint64_t)index * CFS_BS_DESC_SIZE;
    }

    return second + CFS_BS_HEADER_SIZE +
           (uint64_t)(index - CFS_BS_DESC_PER_BLOCK) * CFS_BS_DESC_SIZE;
}

void cfs_bs_tx_descriptors_frame(cfs_bs_tx_t *tx, uint64_t generation,
    uint8_t *first, uint8_t *second) {
    uint32_t descriptor_index_value = 0;
    uint32_t i;

    memset(first, 0, CFS_BLOCK_SIZE);
    memset(second, 0, CFS_BLOCK_SIZE);
    for (i = 0; i < tx->page_count; i++) {
        cfs_bs_descriptor_t descriptor;

        if (tx->workspace->dirty[i] == 0) {
            continue;
        }

        descriptor.destination = tx->workspace->destination[i];
        descriptor.before_generation =
            tx->workspace->before_generation[i];
        descriptor.before_hash = tx->workspace->before_hash[i];
        descriptor.after_hash =
            cfs_get64(tx->workspace->pages[i].bytes +
                      CFS_BS_HEADER_HASH_OFF);
        cfs_bs_descriptor_encode(tx_descriptor_raw(first, second,
                                                   descriptor_index_value),
                                 &descriptor);
        descriptor_index_value++;
    }

    cfs_bs_header_frame(first, CFS_BS_KIND_DESC,
                        cfs_bs_redo_base(tx->store, generation) + 2,
                        generation, tx->store->uuid);
    cfs_bs_header_frame(second, CFS_BS_KIND_DESC,
                        cfs_bs_redo_base(tx->store, generation) + 3,
                        generation, tx->store->uuid);
}

static void frame_super(uint8_t *block, uint64_t self,
    const uint8_t uuid[CFS_BS_UUID_SIZE], uint64_t total_blocks,
    uint64_t row_blocks, uint64_t semantic_start, uint64_t reserve) {
    memset(block, 0, CFS_BLOCK_SIZE);
    cfs_put64(block + CFS_BS_SUPER_TOTAL_BLOCKS_OFF, total_blocks);
    cfs_put64(block + CFS_BS_SUPER_ROW_BLOCKS_OFF, row_blocks);
    cfs_put64(block + CFS_BS_SUPER_SEMANTIC_START_OFF, semantic_start);
    cfs_put64(block + CFS_BS_SUPER_RESERVE_OFF, reserve);
    cfs_bs_header_frame(block, CFS_BS_KIND_SUPER, self, 1, uuid);
}

static void frame_state(uint8_t *block,
    const uint8_t uuid[CFS_BS_UUID_SIZE], uint64_t semantic_start,
    uint64_t total_blocks, uint64_t reserve) {
    memset(block, 0, CFS_BLOCK_SIZE);
    cfs_put64(block + CFS_BS_STATE_FREE_HEAD_OFF, semantic_start);
    cfs_put64(block + CFS_BS_STATE_FREE_COUNT_OFF,
              total_blocks - semantic_start);
    cfs_put64(block + CFS_BS_STATE_PENDING_HEAD_OFF, CFS_BS_BLOCK_NONE);
    cfs_put64(block + CFS_BS_STATE_PENDING_TAIL_OFF, CFS_BS_BLOCK_NONE);
    cfs_put64(block + CFS_BS_STATE_PENDING_COUNT_OFF, 0);
    cfs_put64(block + CFS_BS_STATE_NEXT_SERIAL_OFF, 2);
    cfs_put64(block + CFS_BS_STATE_NEXT_REVISION_OFF, 2);
    cfs_put64(block + CFS_BS_STATE_RESERVE_OFF, reserve);
    cfs_bs_header_frame(block, CFS_BS_KIND_STATE, CFS_BS_STATE_BLOCK, 1,
                        uuid);
}

static void frame_branch_catalog(uint8_t *block,
    const uint8_t uuid[CFS_BS_UUID_SIZE]) {
    uint8_t *entry;
    cfs_bs_ref_t empty;

    memset(block, 0, CFS_BLOCK_SIZE);
    entry = block + CFS_BS_CATALOG_BODY_OFF;
    empty.block = CFS_BS_BLOCK_NONE;
    empty.incarnation = 0;
    empty.hash = 0;
    cfs_put64(entry + CFS_BS_CATALOG_ID_OFF, CFS_BS_MAIN_ID);
    cfs_put64(entry + CFS_BS_CATALOG_REVISION_OFF, 1);
    cfs_bs_ref_encode(entry + CFS_BS_CATALOG_ROOT_OFF, empty);
    cfs_put32(entry + CFS_BS_CATALOG_NAME_LENGTH_OFF, 4);
    cfs_put32(entry + CFS_BS_CATALOG_KIND_OFF, CFS_BS_CATALOG_BRANCH);
    memcpy(entry + CFS_BS_CATALOG_NAME_OFF, "main", 4);
    cfs_bs_header_frame(block, CFS_BS_KIND_CATALOG, CFS_BS_BRANCH_BLOCK, 1,
                        uuid);
}

static void frame_empty_page(uint8_t *block, uint32_t kind, uint64_t self,
    const uint8_t uuid[CFS_BS_UUID_SIZE]) {
    memset(block, 0, CFS_BLOCK_SIZE);
    cfs_bs_header_frame(block, kind, self, 1, uuid);
}

static void frame_row_page(uint8_t *block, uint64_t page_index,
    const uint8_t uuid[CFS_BS_UUID_SIZE], uint64_t semantic_start,
    uint64_t total_blocks) {
    uint64_t base;
    uint32_t i;

    memset(block, 0, CFS_BLOCK_SIZE);
    base = page_index * CFS_BS_ROWS_PER_BLOCK;
    for (i = 0; i < CFS_BS_ROWS_PER_BLOCK; i++) {
        uint64_t physical = base + i;
        uint8_t *row = block + CFS_BS_HEADER_SIZE +
                       (uint64_t)i * CFS_BS_ROW_SIZE;

        if (physical < semantic_start || physical >= total_blocks) {
            continue;
        }

        cfs_put32(row + CFS_BS_ROW_STATE_OFF, CFS_BS_ROW_FREE);
        cfs_put64(row + CFS_BS_ROW_NEXT_OFF,
                  physical + 1 < total_blocks ? physical + 1 :
                                                CFS_BS_BLOCK_NONE);
    }

    cfs_bs_header_frame(block, CFS_BS_KIND_ROWS,
                        CFS_BS_ROW_START + page_index, 1, uuid);
}

static void frame_clean(uint8_t *block,
    const uint8_t uuid[CFS_BS_UUID_SIZE], uint64_t state_hash,
    uint64_t branch_hash, uint64_t pin_hash) {
    memset(block, 0, CFS_BLOCK_SIZE);
    cfs_put64(block + CFS_BS_DECISION_PHASE_OFF, 2);
    cfs_put64(block + CFS_BS_DECISION_STATE_HASH_OFF, state_hash);
    cfs_put64(block + CFS_BS_DECISION_BRANCH_HASH_OFF, branch_hash);
    cfs_put64(block + CFS_BS_DECISION_PIN_HASH_OFF, pin_hash);
    cfs_put64(block + CFS_BS_DECISION_PREDECESSOR_OFF, 1);
    cfs_bs_header_frame(block, CFS_BS_KIND_CLEAN, CFS_BS_CLEAN_BLOCK, 1,
                        uuid);
}

int cfs_bs_format_write_block(const cfs_block_backend *backend, uint64_t block,
    const uint8_t *data) {
    if (cfs_block_write(backend, block, 1, data) != CFS_BLK_OK) {
        return CFS_BS_EIO;
    }

    return CFS_BS_OK;
}

int cfs_bs_genesis_matches(const cfs_bs_store_t *store,
    const cfs_bs_decision_t *clean, uint8_t *page) {
    if (clean->generation != 1) {
        return 0;
    }

    frame_state(page, store->uuid, store->semantic_start,
        store->total_blocks, store->reserve);
    if (cfs_get64(page) != clean->state_hash) {
        return 0;
    }

    frame_branch_catalog(page, store->uuid);
    if (cfs_get64(page) != clean->branch_hash) {
        return 0;
    }

    frame_empty_page(page, CFS_BS_KIND_CATALOG, CFS_BS_PIN_BLOCK,
        store->uuid);
    return cfs_get64(page) == clean->pin_hash;
}

int cfs_bs_format(const cfs_block_backend *backend,
    const uint8_t uuid[CFS_BS_UUID_SIZE], uint64_t reserve,
    cfs_bs_workspace_t *workspace) {
    uint64_t row_blocks;
    uint64_t semantic_start;
    uint64_t state_hash;
    uint64_t branch_hash;
    uint64_t pin_hash;
    uint64_t i;
    uint8_t *page;
    int rc;

    if (backend == NULL || uuid == NULL || workspace == NULL ||
        backend->dev.write == NULL || backend->dev.sync == NULL ||
        bytes_zero(uuid, CFS_BS_UUID_SIZE) ||
        backend->block_count < CFS_BS_MIN_BLOCKS ||
        backend->block_count > CFS_BS_MAX_BLOCKS) {
        return CFS_BS_EINVAL;
    }

    row_blocks = cfs_bs_row_block_count(backend->block_count);
    semantic_start = cfs_bs_semantic_start(backend->block_count);
    if (semantic_start >= backend->block_count || reserve < 5 ||
        reserve >= backend->block_count - semantic_start) {
        return CFS_BS_EINVAL;
    }

    memset(workspace, 0, sizeof(*workspace));
    page = workspace->scratch[0].bytes;
    frame_super(page, CFS_BS_SUPER0_BLOCK, uuid, backend->block_count,
                row_blocks, semantic_start, reserve);
    rc = cfs_bs_format_write_block(backend, CFS_BS_SUPER0_BLOCK, page);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    frame_super(page, CFS_BS_SUPER1_BLOCK, uuid, backend->block_count,
                row_blocks, semantic_start, reserve);
    rc = cfs_bs_format_write_block(backend, CFS_BS_SUPER1_BLOCK, page);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    memset(page, 0, CFS_BLOCK_SIZE);
    rc = cfs_bs_format_write_block(backend, CFS_BS_COMMIT_BLOCK, page);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = cfs_bs_format_write_block(backend, CFS_BS_CONTINUATION_BLOCK, page);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    for (i = CFS_BS_PAYLOAD_START;
         i < CFS_BS_PAYLOAD_START + CFS_BS_PAYLOAD_COUNT; i++) {
        rc = cfs_bs_format_write_block(backend, i, page);
        if (rc != CFS_BS_OK) {
            return rc;
        }
    }

    frame_empty_page(page, CFS_BS_KIND_DESC, CFS_BS_DESC0_BLOCK, uuid);
    rc = cfs_bs_format_write_block(backend, CFS_BS_DESC0_BLOCK, page);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    frame_empty_page(page, CFS_BS_KIND_DESC, CFS_BS_DESC1_BLOCK, uuid);
    rc = cfs_bs_format_write_block(backend, CFS_BS_DESC1_BLOCK, page);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    memset(page, 0, CFS_BLOCK_SIZE);
    for (i = semantic_start - CFS_BS_REDO_BLOCKS; i < semantic_start; i++) {
        rc = cfs_bs_format_write_block(backend, i, page);
        if (rc != CFS_BS_OK) {
            return rc;
        }
    }

    frame_state(page, uuid, semantic_start, backend->block_count, reserve);
    state_hash = cfs_get64(page + CFS_BS_HEADER_HASH_OFF);
    rc = cfs_bs_format_write_block(backend, CFS_BS_STATE_BLOCK, page);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    frame_branch_catalog(page, uuid);
    branch_hash = cfs_get64(page + CFS_BS_HEADER_HASH_OFF);
    rc = cfs_bs_format_write_block(backend, CFS_BS_BRANCH_BLOCK, page);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    frame_empty_page(page, CFS_BS_KIND_CATALOG, CFS_BS_PIN_BLOCK, uuid);
    pin_hash = cfs_get64(page + CFS_BS_HEADER_HASH_OFF);
    rc = cfs_bs_format_write_block(backend, CFS_BS_PIN_BLOCK, page);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    for (i = 0; i < row_blocks;) {
        uint32_t count = row_blocks - i > CFS_BS_TX_MAX_PAGES ?
            CFS_BS_TX_MAX_PAGES : (uint32_t)(row_blocks - i);
        for (uint32_t index = 0; index < count; index++) {
            frame_row_page(workspace->pages[index].bytes, i + index, uuid,
                semantic_start, backend->block_count);
        }

        if (cfs_block_write(backend, CFS_BS_ROW_START + i, count,
                workspace->pages) != CFS_BLK_OK) {
            return CFS_BS_EIO;
        }

        i += count;
    }

    frame_clean(page, uuid, state_hash, branch_hash, pin_hash);
    rc = cfs_bs_format_write_block(backend, CFS_BS_CLEAN_BLOCK, page);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (cfs_block_sync(backend) != CFS_BLK_OK) {
        return CFS_BS_EIO;
    }

    return CFS_BS_OK;
}

int cfs_bs_format_plan(const uint8_t uuid[CFS_BS_UUID_SIZE],
    uint64_t total_blocks, uint64_t reserve, uint8_t *scratch,
    cfs_bs_format_plan_t *out) {
    if (uuid == NULL || scratch == NULL || out == NULL ||
        bytes_zero(uuid, CFS_BS_UUID_SIZE) ||
        total_blocks < CFS_BS_MIN_BLOCKS ||
        total_blocks > CFS_BS_MAX_BLOCKS) {
        return CFS_BS_EINVAL;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->uuid, uuid, CFS_BS_UUID_SIZE);
    out->total_blocks = total_blocks;
    out->row_blocks = cfs_bs_row_block_count(total_blocks);
    out->semantic_start = cfs_bs_semantic_start(total_blocks);
    out->reserve = reserve;
    if (out->semantic_start >= total_blocks || reserve < 5 ||
        reserve >= total_blocks - out->semantic_start) {
        return CFS_BS_EINVAL;
    }

    frame_state(scratch, uuid, out->semantic_start, total_blocks, reserve);
    out->state_hash = cfs_get64(scratch + CFS_BS_HEADER_HASH_OFF);
    frame_branch_catalog(scratch, uuid);
    out->branch_hash = cfs_get64(scratch + CFS_BS_HEADER_HASH_OFF);
    frame_empty_page(scratch, CFS_BS_KIND_CATALOG, CFS_BS_PIN_BLOCK, uuid);
    out->pin_hash = cfs_get64(scratch + CFS_BS_HEADER_HASH_OFF);
    return CFS_BS_OK;
}

/* Frame the bytes cfs_bs_format writes at one block below the semantic
 * start; every redo-area block, including both COMMIT blocks, is zero. */
void cfs_bs_format_frame_block(const cfs_bs_format_plan_t *plan,
    uint64_t block, uint8_t *page) {
    if (plan == NULL || page == NULL) {
        return;
    }

    if (block == CFS_BS_SUPER0_BLOCK || block == CFS_BS_SUPER1_BLOCK) {
        frame_super(page, block, plan->uuid, plan->total_blocks,
                    plan->row_blocks, plan->semantic_start, plan->reserve);
    } else if (block == CFS_BS_CLEAN_BLOCK) {
        frame_clean(page, plan->uuid, plan->state_hash, plan->branch_hash,
                    plan->pin_hash);
    } else if (block == CFS_BS_DESC0_BLOCK || block == CFS_BS_DESC1_BLOCK) {
        frame_empty_page(page, CFS_BS_KIND_DESC, block, plan->uuid);
    } else if (block == CFS_BS_STATE_BLOCK) {
        frame_state(page, plan->uuid, plan->semantic_start,
                    plan->total_blocks, plan->reserve);
    } else if (block == CFS_BS_BRANCH_BLOCK) {
        frame_branch_catalog(page, plan->uuid);
    } else if (block == CFS_BS_PIN_BLOCK) {
        frame_empty_page(page, CFS_BS_KIND_CATALOG, CFS_BS_PIN_BLOCK,
                         plan->uuid);
    } else if (block >= CFS_BS_ROW_START &&
               block < CFS_BS_ROW_START + plan->row_blocks) {
        frame_row_page(page, block - CFS_BS_ROW_START, plan->uuid,
                       plan->semantic_start, plan->total_blocks);
    } else {
        memset(page, 0, CFS_BLOCK_SIZE);
    }
}

static int decode_super(const uint8_t *block, uint64_t self,
    uint64_t backend_blocks, bs_super_fields_t *out) {
    uint64_t generation;
    uint64_t total_blocks;
    uint64_t row_blocks;
    uint64_t semantic_start;
    uint64_t reserve;

    if (cfs_bs_header_validate(block, CFS_BS_KIND_SUPER, self,
            block + CFS_BS_HEADER_UUID_OFF, &generation) != CFS_BS_OK ||
        generation != 1 ||
        bytes_zero(block + CFS_BS_HEADER_UUID_OFF, CFS_BS_UUID_SIZE) ||
        !bytes_zero(block + CFS_BS_SUPER_RESERVE_OFF + 8,
                    CFS_BLOCK_SIZE - (CFS_BS_SUPER_RESERVE_OFF + 8))) {
        return CFS_BS_ECORRUPT;
    }

    total_blocks = cfs_get64(block + CFS_BS_SUPER_TOTAL_BLOCKS_OFF);
    row_blocks = cfs_get64(block + CFS_BS_SUPER_ROW_BLOCKS_OFF);
    semantic_start = cfs_get64(block + CFS_BS_SUPER_SEMANTIC_START_OFF);
    reserve = cfs_get64(block + CFS_BS_SUPER_RESERVE_OFF);
    if (total_blocks != backend_blocks ||
        total_blocks < CFS_BS_MIN_BLOCKS ||
        total_blocks > CFS_BS_MAX_BLOCKS ||
        row_blocks != cfs_bs_row_block_count(total_blocks) ||
        semantic_start != cfs_bs_semantic_start(total_blocks) ||
        semantic_start >= total_blocks || reserve < 5 ||
        reserve >= total_blocks - semantic_start) {
        return CFS_BS_ECORRUPT;
    }

    memcpy(out->uuid, block + CFS_BS_HEADER_UUID_OFF, CFS_BS_UUID_SIZE);
    out->total_blocks = total_blocks;
    out->row_blocks = row_blocks;
    out->semantic_start = semantic_start;
    out->reserve = reserve;
    return CFS_BS_OK;
}

static int super_equal(const bs_super_fields_t *a,
    const bs_super_fields_t *b) {
    return uuid_equal(a->uuid, b->uuid) &&
           a->total_blocks == b->total_blocks &&
           a->row_blocks == b->row_blocks &&
           a->semantic_start == b->semantic_start &&
           a->reserve == b->reserve;
}

static int classify_super(const uint8_t *page, uint64_t self,
    uint64_t backend_blocks, bs_super_fields_t *out, int *out_valid) {
    int rc;

    *out_valid = 0;
    if (cfs_get64(page + CFS_BS_HEADER_HASH_OFF) !=
        cfs_bs_block_hash(page)) {
        return CFS_BS_OK;
    }

    rc = decode_super(page, self, backend_blocks, out);
    if (rc != CFS_BS_OK) {
        return CFS_BS_ECORRUPT;
    }

    *out_valid = 1;
    return CFS_BS_OK;
}

int cfs_bs_open(cfs_bs_store_t *out, const cfs_block_backend *backend,
    cfs_bs_workspace_t *workspace) {
    cfs_block_backend copied;
    bs_super_fields_t first;
    bs_super_fields_t second;
    int first_ok;
    int second_ok;
    int rc;

    if (out == NULL || backend == NULL || workspace == NULL ||
        backend->dev.read == NULL || backend->dev.write == NULL ||
        backend->dev.sync == NULL) {
        return CFS_BS_EINVAL;
    }

    copied = *backend;
    memset(out, 0, sizeof(*out));
    out->backend = copied;
    if (cfs_block_read(&copied, CFS_BS_SUPER0_BLOCK, 1,
            workspace->scratch[0].bytes) != CFS_BLK_OK ||
        cfs_block_read(&copied, CFS_BS_SUPER1_BLOCK, 1,
            workspace->scratch[1].bytes) != CFS_BLK_OK) {
        out->poisoned = 1;
        return CFS_BS_EIO;
    }

    rc = classify_super(workspace->scratch[0].bytes, CFS_BS_SUPER0_BLOCK,
                        copied.block_count, &first, &first_ok);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = classify_super(workspace->scratch[1].bytes, CFS_BS_SUPER1_BLOCK,
                        copied.block_count, &second, &second_ok);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (!first_ok && !second_ok) {
        return CFS_BS_ECORRUPT;
    }

    if (first_ok && second_ok && !super_equal(&first, &second)) {
        return CFS_BS_ECORRUPT;
    }

    if (!first_ok) {
        first = second;
    }

    memcpy(out->uuid, first.uuid, CFS_BS_UUID_SIZE);
    out->total_blocks = first.total_blocks;
    out->semantic_start = first.semantic_start;
    out->reserve = first.reserve;
    out->generation = 0;
    out->poisoned = 0;
    return cfs_bs_recover(out, workspace);
}
