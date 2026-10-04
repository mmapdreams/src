/* SPDX-License-Identifier: ISC
 *
 * branch_store_internal.h -- private byte layout and bounded redo interface
 * for the persistent branch store.
 */
#ifndef CFS_BRANCH_STORE_INTERNAL_H
#define CFS_BRANCH_STORE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "branch_store.h"

#define CFS_BS_MAGIC UINT64_C(0x3530305242534643)
#define CFS_BS_VERSION 5u
#define CFS_BS_HEADER_SIZE 64u
#define CFS_BS_HEADER_HASH_OFF 0u
#define CFS_BS_HEADER_MAGIC_OFF 8u
#define CFS_BS_HEADER_KIND_OFF 16u
#define CFS_BS_HEADER_VERSION_OFF 20u
#define CFS_BS_HEADER_SELF_OFF 24u
#define CFS_BS_HEADER_GENERATION_OFF 32u
#define CFS_BS_HEADER_UUID_OFF 40u
#define CFS_BS_HEADER_RESERVED_OFF 56u

enum {
    CFS_BS_KIND_SUPER = 1,
    CFS_BS_KIND_CLEAN = 2,
    CFS_BS_KIND_COMMIT = 3,
    CFS_BS_KIND_DESC = 4,
    CFS_BS_KIND_STATE = 5,
    CFS_BS_KIND_CATALOG = 6,
    CFS_BS_KIND_ROWS = 7,
    CFS_BS_KIND_OBJECT = 8,
    CFS_BS_KIND_CONTINUATION = 9
};

#define CFS_BS_SUPER0_BLOCK 0u
#define CFS_BS_SUPER1_BLOCK 1u
#define CFS_BS_CLEAN_BLOCK 2u
#define CFS_BS_COMMIT_BLOCK 3u
#define CFS_BS_CONTINUATION_BLOCK 4u
#define CFS_BS_DESC0_BLOCK 5u
#define CFS_BS_DESC1_BLOCK 6u
#define CFS_BS_PAYLOAD_START 7u
#define CFS_BS_PAYLOAD_COUNT 136u
#define CFS_BS_REDO_BLOCKS (4u + CFS_BS_PAYLOAD_COUNT)
#define CFS_BS_STATE_BLOCK 143u
#define CFS_BS_BRANCH_BLOCK 144u
#define CFS_BS_PIN_BLOCK 145u
#define CFS_BS_ROW_START 146u

#define CFS_BS_ROWS_PER_BLOCK 126u
#define CFS_BS_ROW_SIZE 32u
#define CFS_BS_DESC_PER_BLOCK 126u
#define CFS_BS_DESC_SIZE 32u
#define CFS_BS_CATALOG_ENTRY_SIZE 128u
#define CFS_BS_BRANCH_ENTRY_SIZE CFS_BS_CATALOG_ENTRY_SIZE
#define CFS_BS_PIN_ENTRY_SIZE CFS_BS_CATALOG_ENTRY_SIZE
#define CFS_BS_CATALOG_BODY_OFF 64u
#define CFS_BS_CATALOG_ID_OFF 0u
#define CFS_BS_CATALOG_REVISION_OFF 8u
#define CFS_BS_CATALOG_ORIGIN_ID_OFF 16u
#define CFS_BS_CATALOG_ORIGIN_REVISION_OFF 24u
#define CFS_BS_CATALOG_ROOT_OFF 32u
#define CFS_BS_CATALOG_NAME_LENGTH_OFF 56u
#define CFS_BS_CATALOG_KIND_OFF 60u
#define CFS_BS_CATALOG_NAME_OFF 64u
#define CFS_BS_CATALOG_RESERVED_OFF 96u

#define CFS_BS_STATE_FREE_HEAD_OFF 64u
#define CFS_BS_STATE_FREE_COUNT_OFF 72u
#define CFS_BS_STATE_PENDING_HEAD_OFF 80u
#define CFS_BS_STATE_PENDING_TAIL_OFF 88u
#define CFS_BS_STATE_PENDING_COUNT_OFF 96u
#define CFS_BS_STATE_NEXT_SERIAL_OFF 104u
#define CFS_BS_STATE_NEXT_REVISION_OFF 112u
#define CFS_BS_STATE_RESERVE_OFF 120u

#define CFS_BS_ROW_STATE_OFF 0u
#define CFS_BS_ROW_RESERVED_OFF 4u
/* A PENDING row's reserved field: its edges were consumed at retirement. */
#define CFS_BS_ROW_RELEASED 1u

/* Donor holder payload (r0125 7.3): this magic, then one when child zero is
 * the previous holder, then zero. */
#define CFS_BS_DONOR_MAGIC UINT64_C(0x524f4e4f44534643)
#define CFS_BS_DONOR_PAYLOAD 16u
#define CFS_BS_ROW_REFCOUNT_OFF 8u
#define CFS_BS_ROW_INCARNATION_OFF 16u
#define CFS_BS_ROW_NEXT_OFF 24u

#define CFS_BS_DESC_DESTINATION_OFF 0u
#define CFS_BS_DESC_BEFORE_GENERATION_OFF 8u
#define CFS_BS_DESC_BEFORE_HASH_OFF 16u
#define CFS_BS_DESC_AFTER_HASH_OFF 24u

#define CFS_BS_DECISION_PHASE_OFF 64u
#define CFS_BS_DECISION_DESC0_HASH_OFF 72u
#define CFS_BS_DECISION_DESC1_HASH_OFF 80u
#define CFS_BS_DECISION_COUNT_OFF 88u
#define CFS_BS_DECISION_PREPARED_COUNT_OFF 92u
#define CFS_BS_DECISION_STATE_HASH_OFF 96u
#define CFS_BS_DECISION_BRANCH_HASH_OFF 104u
#define CFS_BS_DECISION_PIN_HASH_OFF 112u
#define CFS_BS_DECISION_CONTINUATION_HASH_OFF 128u
#define CFS_BS_DECISION_PREPARED_REF_OFF 136u
#define CFS_BS_DECISION_INLINE_REFS 165u
#define CFS_BS_DECISION_PREDECESSOR_OFF 120u

#define CFS_BS_SUPER_TOTAL_BLOCKS_OFF 64u
#define CFS_BS_SUPER_ROW_BLOCKS_OFF 72u
#define CFS_BS_SUPER_SEMANTIC_START_OFF 80u
#define CFS_BS_SUPER_RESERVE_OFF 88u
#define CFS_BS_OBJECT_INCARNATION_OFF 64u
#define CFS_BS_OBJECT_CHILD_COUNT_OFF 72u
#define CFS_BS_OBJECT_PAYLOAD_LENGTH_OFF 76u
#define CFS_BS_OBJECT_RESERVED_OFF 80u
#define CFS_BS_OBJECT_REFS_OFF 96u
#define CFS_BS_REF_SIZE 24u

enum {
    CFS_BS_DECISION_NONE = 0,
    CFS_BS_DECISION_CLEAN = 1,
    CFS_BS_DECISION_COMMIT = 2
};

typedef struct cfs_bs_decision {
    uint32_t kind;
    uint64_t phase;
    uint64_t desc_hash[2];
    uint64_t continuation_hash;
    uint32_t count;
    uint32_t prepared_count;
    uint64_t state_hash;
    uint64_t branch_hash;
    uint64_t pin_hash;
    uint64_t predecessor_generation;
    uint64_t generation;
} cfs_bs_decision_t;

typedef struct cfs_bs_descriptor {
    uint64_t destination;
    uint64_t before_generation;
    uint64_t before_hash;
    uint64_t after_hash;
} cfs_bs_descriptor_t;

typedef struct cfs_bs_catalog_entry {
    uint64_t id;
    uint64_t revision;
    uint64_t origin_id;
    uint64_t origin_revision;
    cfs_bs_ref_t root;
    uint32_t name_length;
    uint32_t kind;
    uint8_t name[CFS_BS_NAME_BYTES];
} cfs_bs_catalog_entry_t;

typedef struct cfs_bs_tx {
    cfs_bs_store_t *store;
    cfs_bs_workspace_t *workspace;
    uint32_t page_count;
    uint32_t active;
} cfs_bs_tx_t;

/* Everything a fresh format writes is a function of these fields. */
typedef struct cfs_bs_format_plan {
    uint8_t uuid[CFS_BS_UUID_SIZE];
    uint64_t total_blocks;
    uint64_t row_blocks;
    uint64_t semantic_start;
    uint64_t reserve;
    uint64_t state_hash;
    uint64_t branch_hash;
    uint64_t pin_hash;
} cfs_bs_format_plan_t;

int cfs_bs_format_plan(const uint8_t uuid[CFS_BS_UUID_SIZE],
                       uint64_t total_blocks, uint64_t reserve,
                       uint8_t *scratch, cfs_bs_format_plan_t *out);
void cfs_bs_format_frame_block(const cfs_bs_format_plan_t *plan,
                               uint64_t block, uint8_t *page);
int cfs_bs_format_write_block(const cfs_block_backend *backend, uint64_t block,
                              const uint8_t *data);

uint64_t cfs_bs_row_block_count(uint64_t total_blocks);
uint64_t cfs_bs_semantic_start(uint64_t total_blocks);
uint64_t cfs_bs_redo_base(const cfs_bs_store_t *store, uint64_t generation);
uint64_t cfs_bs_row_page(uint64_t physical_block);
uint32_t cfs_bs_row_index(uint64_t physical_block);
uint64_t cfs_bs_block_hash(const uint8_t block[CFS_BLOCK_SIZE]);
int cfs_bs_ref_is_null(cfs_bs_ref_t ref);
int cfs_bs_incarnation_valid(uint64_t incarnation);
int cfs_bs_ref_equal(cfs_bs_ref_t a, cfs_bs_ref_t b);
void cfs_bs_ref_decode(const uint8_t *src, cfs_bs_ref_t *out);
void cfs_bs_ref_encode(uint8_t *dst, cfs_bs_ref_t ref);
void cfs_bs_descriptor_decode(const uint8_t *src,
                              cfs_bs_descriptor_t *out);
void cfs_bs_descriptor_encode(uint8_t *dst,
                              const cfs_bs_descriptor_t *descriptor);
void cfs_bs_row_decode(const uint8_t *src, cfs_bs_row_t *out);
void cfs_bs_row_encode(uint8_t *dst, const cfs_bs_row_t *row);
void cfs_bs_catalog_decode(const uint8_t *src,
                           cfs_bs_catalog_entry_t *out);
void cfs_bs_catalog_encode(uint8_t *dst,
                           const cfs_bs_catalog_entry_t *entry);
void cfs_bs_state_decode(const uint8_t *src, cfs_bs_state_t *out);
void cfs_bs_state_encode(uint8_t *dst, const cfs_bs_state_t *state);
int cfs_bs_name_validate(const void *name, uint32_t length);

void cfs_bs_header_frame(uint8_t block[CFS_BLOCK_SIZE], uint32_t kind,
                         uint64_t self, uint64_t generation,
                         const uint8_t uuid[CFS_BS_UUID_SIZE]);
void cfs_bs_header_rehash(uint8_t block[CFS_BLOCK_SIZE]);
int cfs_bs_header_validate(const uint8_t block[CFS_BLOCK_SIZE],
                           uint32_t expected_kind, uint64_t expected_self,
                           const uint8_t uuid[CFS_BS_UUID_SIZE],
                           uint64_t *out_generation);
int cfs_bs_decision_decode(const cfs_bs_store_t *store, uint64_t self,
                           uint32_t kind, const uint8_t *page,
                           cfs_bs_decision_t *out);
void cfs_bs_clean_frame(const cfs_bs_store_t *store, uint64_t generation,
                        uint64_t state_hash, uint64_t branch_hash,
                        uint64_t pin_hash, uint8_t *page);
void cfs_bs_commit_frame(const cfs_bs_store_t *store, uint64_t generation,
                         uint32_t count, const cfs_bs_ref_t *prepared,
                         uint32_t prepared_count,
                         uint64_t state_hash, uint64_t branch_hash,
                         uint64_t pin_hash, const uint8_t *first_desc,
                         const uint8_t *second_desc, uint8_t *continuation,
                         uint8_t *page);
int cfs_bs_decisions_consistent(const cfs_bs_decision_t *clean,
                                const cfs_bs_decision_t *commit);
int cfs_bs_genesis_matches(const cfs_bs_store_t *store,
                            const cfs_bs_decision_t *clean, uint8_t *page);
void cfs_bs_tx_descriptors_frame(cfs_bs_tx_t *tx, uint64_t generation,
                                 uint8_t *first, uint8_t *second);
uint32_t cfs_bs_home_kind(const cfs_bs_store_t *store, uint64_t block);
int cfs_bs_io_read(cfs_bs_store_t *store, uint64_t block, uint8_t *out);

/* A cache slot's validated field holds the home kind it passed, or the kind
 * with this bit once the whole page validated too. */
#define CFS_BS_VALIDATED_DEEP 0x100u

/* As cfs_bs_io_read; *out_slot names the cache entry now holding the block,
 * valid until the next store I/O, or NULL without a cache. */
int cfs_bs_io_read_slot(cfs_bs_store_t *store, uint64_t block, uint8_t *out,
    cfs_bs_cache_slot_t **out_slot);
void cfs_bs_cache_invalidate(cfs_bs_store_t *store);
int cfs_bs_io_write(cfs_bs_store_t *store, uint64_t block,
                    const uint8_t *data);
int cfs_bs_io_write_blocks(cfs_bs_store_t *store, uint64_t block,
    uint32_t count, const void *data);
int cfs_bs_io_sync(cfs_bs_store_t *store);
int cfs_bs_read_home(cfs_bs_store_t *store, uint64_t block, uint8_t *out);

int cfs_bs_object_frame(const cfs_bs_store_t *store, uint64_t block,
                        uint64_t incarnation, const cfs_bs_ref_t *children,
                        uint32_t child_count, const void *payload,
                        uint32_t payload_length, uint8_t *out,
                        cfs_bs_ref_t *out_ref);
int cfs_bs_object_load(cfs_bs_store_t *store, cfs_bs_ref_t ref, uint8_t *out,
                       uint32_t *out_child_count,
                       uint32_t *out_payload_length);
int cfs_bs_object_page_validate(const cfs_bs_store_t *store,
                                cfs_bs_ref_t ref, const uint8_t *page,
                                uint32_t *out_child_count,
                                uint32_t *out_payload_length);
int cfs_bs_state_page_decode(const cfs_bs_store_t *store,
                             const uint8_t *page, cfs_bs_state_t *out);
int cfs_bs_row_entry_decode(const cfs_bs_store_t *store, uint64_t block,
                            const uint8_t *page, cfs_bs_row_t *out);
int cfs_bs_row_page_validate(const cfs_bs_store_t *store,
                             uint64_t page_block, const uint8_t *page);
int cfs_bs_catalog_entry_decode_checked(const cfs_bs_store_t *store,
                                        const uint8_t *page, uint32_t slot,
                                        int pin_catalog,
                                        cfs_bs_catalog_entry_t *out);
int cfs_bs_catalog_page_validate(const cfs_bs_store_t *store,
                                 const uint8_t *page, int pin_catalog);
int cfs_bs_catalog_pair_validate(const cfs_bs_store_t *store,
                                 const uint8_t *state,
                                 const uint8_t *branches,
                                 const uint8_t *pins);

int cfs_bs_tx_row(cfs_bs_tx_t *tx, uint64_t block, int dirty,
                  cfs_bs_row_t *out, uint8_t **out_entry);
int cfs_bs_tx_state(cfs_bs_tx_t *tx, int dirty, cfs_bs_state_t *out,
                    uint8_t **out_page);
int cfs_bs_tx_enqueue(cfs_bs_tx_t *tx, cfs_bs_state_t *state,
                      uint64_t block, cfs_bs_row_t *row, uint8_t *entry);

/* Queue a zero-count row whose edges this transaction already consumed. */
int cfs_bs_tx_enqueue_released(cfs_bs_tx_t *tx, cfs_bs_state_t *state,
    uint64_t block, cfs_bs_row_t *row, uint8_t *entry);

/* One or zero for a holder with or without a previous holder; else -1. */
int cfs_bs_donor_chained(const uint8_t *object, uint32_t child_count,
    uint32_t payload_length);

/* Replace a staging root with one object over existing children. */
int cfs_bs_staging_store(cfs_bs_store_t *store, uint64_t id,
    uint64_t expected_revision, const cfs_bs_ref_t *children,
    uint32_t child_count, const void *payload, uint32_t payload_length,
    cfs_bs_ref_t *out_root, uint64_t *out_revision,
    cfs_bs_workspace_t *workspace);

int cfs_bs_tx_begin(cfs_bs_store_t *store, cfs_bs_workspace_t *workspace,
                    cfs_bs_tx_t *out);
int cfs_bs_tx_begin_leased(cfs_bs_store_t *store,
                           cfs_bs_workspace_t *workspace,
                           const void *lease_owner, cfs_bs_tx_t *out);
int cfs_bs_tx_begin_replacement(cfs_bs_store_t *store,
                                cfs_bs_workspace_t *workspace,
                                const void *lease_owner, cfs_bs_tx_t *out);
int cfs_bs_tx_get(cfs_bs_tx_t *tx, uint64_t block, int dirty,
                  uint8_t **out);
int cfs_bs_tx_prepare(cfs_bs_tx_t *tx, uint32_t prepared_count);
int cfs_bs_tx_publish(cfs_bs_tx_t *tx, uint32_t prepared_count);
int cfs_bs_tx_commit(cfs_bs_tx_t *tx, uint32_t prepared_count);
void cfs_bs_tx_abort(cfs_bs_tx_t *tx);

int cfs_bs_tx_load_branch(cfs_bs_tx_t *tx, uint64_t branch_id,
                          uint64_t expected_revision, uint8_t **out_catalog,
                          cfs_bs_catalog_entry_t *out_entry);
int cfs_bs_tx_load_head(cfs_bs_tx_t *tx, uint64_t id,
    uint64_t expected_revision, uint32_t kind, uint8_t **out_catalog,
    cfs_bs_catalog_entry_t *out_entry);
int cfs_bs_tx_live_ref(cfs_bs_tx_t *tx, cfs_bs_ref_t ref, uint8_t *object,
    cfs_bs_row_t *out_row, uint8_t **out_entry);
int cfs_bs_tx_check_catalog_ref(cfs_bs_tx_t *tx, cfs_bs_ref_t ref);
int cfs_bs_validate_root(cfs_bs_store_t *store, cfs_bs_ref_t root,
    cfs_bs_workspace_t *workspace);
int cfs_bs_tx_ref_add(cfs_bs_tx_t *tx, cfs_bs_ref_t ref, uint8_t *object);
int cfs_bs_tx_ref_drop(cfs_bs_tx_t *tx, cfs_bs_state_t *state,
                       cfs_bs_ref_t ref, uint8_t *object);

/* Stage FIFO prefix up to limit <= CFS_BS_REAP_MAX_OBJECTS; no publication.
 * Reserve control/catalogs; stop at empty/cache limit, abort on error. */
int cfs_bs_tx_reap_prefix(cfs_bs_tx_t *tx, cfs_bs_state_t *state,
    uint32_t limit, uint32_t *out_reaped);
int cfs_bs_validate_home_page(const cfs_bs_store_t *store, uint64_t self,
                              const uint8_t *page, uint64_t *out_generation);
int cfs_bs_effective_home(cfs_bs_store_t *store,
                          cfs_bs_workspace_t *workspace, uint32_t count,
                          uint64_t block, uint64_t generation,
                          uint8_t *scratch, const uint8_t **out);
int cfs_bs_validate_catalog_roots(cfs_bs_store_t *store,
                                 cfs_bs_workspace_t *workspace,
                                 uint32_t count, const uint8_t *branches,
                                 const uint8_t *pins, uint64_t generation);
int cfs_bs_validate_prepared(cfs_bs_store_t *store,
                            cfs_bs_workspace_t *workspace,
                            uint32_t count, uint32_t prepared_count,
                            uint64_t generation, int staged);

#endif /* CFS_BRANCH_STORE_INTERNAL_H */
