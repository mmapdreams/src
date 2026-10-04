/* SPDX-License-Identifier: ISC
 *
 * branch_store.h -- persistent bounded writable-branch object store.
 *
 * The store is a fresh-image, incompatible development format.  It exposes
 * immutable graph objects and durable branch roots; it is not the namespace
 * or file-data interface of a qualified filesystem.  The core borrows the
 * block backend and caller-owned workspace and performs no allocation.
 */
#ifndef CFS_BRANCH_STORE_H
#define CFS_BRANCH_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "format.h"

enum {
#define CFS_ERROR_STATUS_OK(symbol, value) symbol = value,
#define CFS_ERROR_STATUS_NEGATIVE(symbol, value, posix, description) \
    symbol = value,
#include "error_catalog/branch_store.def"
#undef CFS_ERROR_STATUS_NEGATIVE
#undef CFS_ERROR_STATUS_OK
};

#define CFS_BS_UUID_SIZE 16u
#define CFS_BS_MAX_BRANCHES 16u
#define CFS_BS_MAX_PINS 16u
#define CFS_BS_MAX_CHILDREN 128u
#define CFS_BS_CONSTRUCT_MAX_OBJECTS 218u
/* An object incarnation packs the image generation above its index in the
 * transaction's prepared list, so this width is what bounds that list. */
#define CFS_BS_INCARNATION_INDEX_BITS 8u
#define CFS_BS_INCARNATION_INDEX_MAX (UINT64_C(1) << CFS_BS_INCARNATION_INDEX_BITS)
#define CFS_BS_INCARNATION_INDEX_MASK (CFS_BS_INCARNATION_INDEX_MAX - 1u)
#define CFS_BS_GENERATION_MAX (UINT64_MAX >> CFS_BS_INCARNATION_INDEX_BITS)
#define CFS_BS_NAME_MAX 31u
#define CFS_BS_NAME_BYTES 32u
#define CFS_BS_TX_MAX_PAGES 136u
#define CFS_BS_RECOVERY_MAX_PAGES (2u * CFS_BS_TX_MAX_PAGES)
#define CFS_BS_REAP_MAX_OBJECTS 256u
#define CFS_BS_MIN_BLOCKS 512u
#define CFS_BS_MAX_BLOCKS UINT64_C(188073993831)
#define CFS_BS_MAIN_ID UINT64_C(16)
#define CFS_BS_BLOCK_NONE UINT64_MAX

typedef struct cfs_bs_ref {
    uint64_t block;
    uint64_t incarnation;
    uint64_t hash;
} cfs_bs_ref_t;

enum {
    CFS_BS_CHILD_RETAINED = 1,
    CFS_BS_CHILD_PREPARED = 2
};

typedef struct cfs_bs_child {
    uint32_t kind;
    union {
        cfs_bs_ref_t retained;
        uint32_t prepared_index;
    } value;
} cfs_bs_child_t;

typedef struct cfs_bs_object_plan {
    const cfs_bs_child_t *children;
    uint32_t child_count;
    const void *payload;
    uint32_t payload_length;
} cfs_bs_object_plan_t;

typedef union cfs_bs_page {
    uint64_t align;
    uint8_t bytes[CFS_BLOCK_SIZE];
} cfs_bs_page_t;

/* Recovery holds two journals; mutation still admits at most 136 homes.
 * Call storage is disjoint; returned values never borrow workspace bytes. */
typedef struct cfs_bs_workspace {
    cfs_bs_page_t pages[CFS_BS_RECOVERY_MAX_PAGES];
    uint64_t destination[CFS_BS_RECOVERY_MAX_PAGES];
    uint64_t before_generation[CFS_BS_RECOVERY_MAX_PAGES];
    uint64_t before_hash[CFS_BS_RECOVERY_MAX_PAGES];
    uint64_t middle_generation[CFS_BS_RECOVERY_MAX_PAGES];
    uint64_t middle_hash[CFS_BS_RECOVERY_MAX_PAGES];
    uint8_t loaded[CFS_BS_RECOVERY_MAX_PAGES];
    uint8_t dirty[CFS_BS_RECOVERY_MAX_PAGES];
    cfs_bs_page_t scratch[4];
    cfs_bs_page_t objects[CFS_BS_CONSTRUCT_MAX_OBJECTS];
    cfs_bs_ref_t prepared[CFS_BS_CONSTRUCT_MAX_OBJECTS];
    uint64_t incoming[CFS_BS_CONSTRUCT_MAX_OBJECTS];
    cfs_bs_ref_t children[CFS_BS_MAX_CHILDREN];

    /* Edge transfer at head replacement (r0125 7.2): inherited retained
     * edges, plans paired with an original, and originals to process. */
    uint8_t transferred[CFS_BS_CONSTRUCT_MAX_OBJECTS][CFS_BS_MAX_CHILDREN / 8u];
    uint8_t paired[CFS_BS_CONSTRUCT_MAX_OBJECTS];
    cfs_bs_ref_t releasing[CFS_BS_CONSTRUCT_MAX_OBJECTS];
    uint16_t releasing_plan[CFS_BS_CONSTRUCT_MAX_OBJECTS];
} cfs_bs_workspace_t;

/* One cached block: its address, recency, and the validation it passed;
 * an object entry also records the reference and shape it validated. */
typedef struct cfs_bs_cache_slot {
    uint64_t block;
    uint64_t stamp;
    uint64_t incarnation;
    uint64_t hash;
    uint32_t validated;
    uint32_t child_count;
    uint32_t payload_length;
    uint32_t reserved;
} cfs_bs_cache_slot_t;

/* Optional bounded block cache, coherent with this store's own writes. The
 * caller owns all memory; verify rereads media on hits and counts drift. */
typedef struct cfs_bs_cache {
    cfs_bs_cache_slot_t *slots;
    uint8_t *bytes;
    uint32_t slot_count;
    uint32_t verify;
    uint64_t clock;
    uint64_t hits;
    uint64_t misses;
    uint64_t mismatches;
    uint64_t validations;
} cfs_bs_cache_t;

/* The store borrows its backend. Any provider failure poisons this handle;
 * discard it and open a fresh handle to recover. */
typedef struct cfs_bs_store {
    cfs_block_backend backend;
    cfs_bs_cache_t *cache;
    uint8_t uuid[CFS_BS_UUID_SIZE];
    uint64_t generation;
    uint64_t total_blocks;
    uint64_t semantic_start;
    uint64_t reserve;
    const void *lease_owner;
    cfs_bs_workspace_t *lease_workspace;
    uint64_t lease_generation;
    uint32_t poisoned;
} cfs_bs_store_t;

enum {
    CFS_BS_CATALOG_BRANCH = 1,
    CFS_BS_CATALOG_SNAPSHOT_PIN = 2,
    CFS_BS_CATALOG_RETAINED_ROOT_PIN = 3,
    CFS_BS_CATALOG_STAGING = 4,
    CFS_BS_ROW_FREE = 1,
    CFS_BS_ROW_LIVE = 2,
    CFS_BS_ROW_PENDING = 3
};

typedef struct cfs_bs_branch {
    uint64_t id;
    uint64_t revision;
    uint64_t origin_id;
    uint64_t origin_revision;
    cfs_bs_ref_t root;
    uint32_t name_length;
    uint32_t kind;
    uint8_t name[CFS_BS_NAME_BYTES];
} cfs_bs_branch_t;

typedef struct cfs_bs_pin {
    uint64_t id;
    uint64_t revision;
    uint64_t origin_id;
    uint64_t origin_revision;
    cfs_bs_ref_t root;
    uint32_t name_length;
    uint32_t kind;
    uint8_t name[CFS_BS_NAME_BYTES];
} cfs_bs_pin_t;

typedef struct cfs_bs_row {
    uint32_t state;
    uint32_t reserved;
    uint64_t refcount;
    uint64_t incarnation;
    uint64_t next;
} cfs_bs_row_t;

typedef struct cfs_bs_state {
    uint64_t free_head;
    uint64_t free_count;
    uint64_t pending_head;
    uint64_t pending_tail;
    uint64_t pending_count;
    uint64_t next_serial;
    uint64_t next_revision;
    uint64_t reserve;
} cfs_bs_state_t;

typedef struct cfs_bs_stats {
    uint64_t generation;
    uint64_t total_blocks;
    uint64_t semantic_start;
    uint64_t free_count;
    uint64_t pending_count;
    uint64_t reserve;
    uint32_t branch_count;
    uint32_t pin_count;
    uint32_t staging_count;
} cfs_bs_stats_t;

typedef struct cfs_bs_object_view {
    cfs_bs_ref_t ref;
    uint32_t child_count;
    uint32_t payload_length;
} cfs_bs_object_view_t;

enum {
    CFS_BS_SIGNATURE_NONE = 0,
    CFS_BS_SIGNATURE_BRANCH = 1,
    CFS_BS_SIGNATURE_ANCHOR = 2
};

/* What the two superblock addresses carry: a CFSBR profile header of any
 * version, the older checkpoint-ring anchor, or nothing recognizable. */
typedef struct cfs_bs_signature {
    uint32_t kind;
    uint32_t copies;
    uint32_t consistent;
    uint8_t uuid[CFS_BS_UUID_SIZE];
} cfs_bs_signature_t;

/* The geometry a format derives and the blocks a read-back compared. */
typedef struct cfs_bs_format_report {
    uint64_t total_blocks;
    uint64_t row_blocks;
    uint64_t semantic_start;
    uint64_t reserve;
    uint64_t metadata_blocks;
    uint64_t metadata_bytes;
    uint64_t verified_blocks;
    uint64_t verified_bytes;
    uint64_t mismatched_block;
} cfs_bs_format_report_t;

/* Format and sync a fresh image. uuid is nonzero; reserve counts semantic
 * blocks left FREE by each completed construction. */
int cfs_bs_format(const cfs_block_backend *backend,
                  const uint8_t uuid[CFS_BS_UUID_SIZE], uint64_t reserve,
                  cfs_bs_workspace_t *workspace);

/* Read blocks 0 and 1 and report any signature they carry; both known
 * layouts keep the image identity at byte 40. No write, no validation. */
int cfs_bs_signature_probe(const cfs_block_backend *backend,
                           cfs_bs_workspace_t *workspace,
                           cfs_bs_signature_t *out);

/* Read every block a format of this uuid and reserve writes and compare it
 * with the framed bytes; ECORRUPT names the first differing block. */
int cfs_bs_format_verify(const cfs_block_backend *backend,
                         const uint8_t uuid[CFS_BS_UUID_SIZE],
                         uint64_t reserve, cfs_bs_workspace_t *workspace,
                         cfs_bs_format_report_t *out);

/* One bounded step of that read-back, per r0125 7.1. Prefix ownership and
 * the refusals are specified in txt/branch_store.md. */
int cfs_bs_format_verify_range(const cfs_block_backend *backend,
                               const uint8_t uuid[CFS_BS_UUID_SIZE],
                               uint64_t reserve, uint64_t first_block,
                               uint64_t max_blocks,
                               cfs_bs_workspace_t *workspace,
                               cfs_bs_format_report_t *out);

/* Bounded format per RFC r0125 section 7.1: begin invalidates and syncs, each
 * step writes at most max_pages metadata blocks, finish publishes and syncs. */
#define CFS_BS_FORMAT_STEP_MIN_PAGES 1u

typedef struct cfs_bs_format_cursor {
    uint8_t uuid[CFS_BS_UUID_SIZE];
    uint64_t total_blocks;
    uint64_t reserve;
    uint64_t semantic_start;
    uint64_t next_block;
    uint64_t written_blocks;
    uint32_t invalidated;
    uint32_t sealed;
    uint32_t synced;
} cfs_bs_format_cursor_t;

int cfs_bs_format_begin(const cfs_block_backend *backend,
                        const uint8_t uuid[CFS_BS_UUID_SIZE],
                        uint64_t reserve, cfs_bs_workspace_t *workspace,
                        cfs_bs_format_cursor_t *out);
int cfs_bs_format_step(const cfs_block_backend *backend,
                       cfs_bs_format_cursor_t *cursor, uint32_t max_pages,
                       cfs_bs_workspace_t *workspace, uint32_t *out_written);
int cfs_bs_format_finish(const cfs_block_backend *backend,
                         cfs_bs_format_cursor_t *cursor,
                         cfs_bs_workspace_t *workspace);

/* Open validates geometry and recovers. Success names a CLEAN generation. */
int cfs_bs_open(cfs_bs_store_t *out, const cfs_block_backend *backend,
                cfs_bs_workspace_t *workspace);
int cfs_bs_recover(cfs_bs_store_t *store, cfs_bs_workspace_t *workspace);

/* Attach an empty cache of slot_count slots (a power of two, at least 4)
 * over slot_count blocks of bytes. Open detaches it; attach again after. */
int cfs_bs_cache_attach(cfs_bs_store_t *store, cfs_bs_cache_t *cache,
    cfs_bs_cache_slot_t *slots, uint8_t *bytes, uint32_t slot_count);

int cfs_bs_branch_get(cfs_bs_store_t *store, uint64_t branch_id,
                      cfs_bs_branch_t *out, cfs_bs_workspace_t *workspace);
int cfs_bs_branch_fork(cfs_bs_store_t *store, uint64_t source_branch_id,
                       uint64_t expected_revision, const void *name,
                       uint16_t name_length, cfs_bs_branch_t *out,
                       cfs_bs_workspace_t *workspace);

/* Checked whole-root replacement of a quiescent destination, not a merge.
 * See txt/branch_store.md. The source is retained; out changes on success. */
int cfs_bs_branch_reset_from_branch(cfs_bs_store_t *store,
                                    uint64_t destination_branch_id,
                                    uint64_t expected_destination_revision,
                                    uint64_t source_branch_id,
                                    uint64_t expected_source_revision,
                                    cfs_bs_branch_t *out,
                                    cfs_bs_workspace_t *workspace);
int cfs_bs_branch_store(cfs_bs_store_t *store, uint64_t branch_id,
                        uint64_t expected_revision,
                        const cfs_bs_ref_t *children, uint32_t child_count,
                        const void *payload, uint32_t payload_length,
                        cfs_bs_ref_t *out_root, uint64_t *out_revision,
                        cfs_bs_workspace_t *workspace);

/* Publish a connected plan; inputs are immutable and disjoint through return.
 * Only predecessor FREE space is allocated; see txt/branch_store.md. */
int cfs_bs_branch_construct(cfs_bs_store_t *store, uint64_t branch_id,
                            uint64_t expected_revision,
                            const cfs_bs_object_plan_t *plans,
                            uint32_t plan_count, cfs_bs_ref_t *out_root,
                            uint64_t *out_revision,
                            cfs_bs_workspace_t *workspace);
int cfs_bs_branch_discard(cfs_bs_store_t *store, uint64_t branch_id,
                          uint64_t expected_revision,
                          cfs_bs_workspace_t *workspace);

/* Authenticate an object. Capacity failure returns required lengths without
 * publishing partial content. */
int cfs_bs_object_read(cfs_bs_store_t *store, cfs_bs_ref_t object,
                       cfs_bs_ref_t *children, uint32_t child_capacity,
                       uint32_t *out_child_count, void *payload,
                       uint32_t payload_capacity, uint32_t *out_payload_length,
                       cfs_bs_workspace_t *workspace);

int cfs_bs_pin_create(cfs_bs_store_t *store, uint32_t kind,
                      uint64_t branch_id, uint64_t expected_revision,
                      const void *name, uint16_t name_length,
                      cfs_bs_pin_t *out, cfs_bs_workspace_t *workspace);
int cfs_bs_pin_drop(cfs_bs_store_t *store, uint64_t pin_id,
                    cfs_bs_workspace_t *workspace);

/* Reap one FIFO batch, at most min(limit, CFS_BS_REAP_MAX_OBJECTS).
 * Failure reports zero; provider publication failure may be uncertain. */
int cfs_bs_reap(cfs_bs_store_t *store, uint32_t limit,
                uint32_t *out_reaped, cfs_bs_workspace_t *workspace);

/* Bounded inspection reads raw catalogs, including unnamed STAGING entries.
 * An unused slot returns CFS_BS_ENOENT; branch_get admits ordinary branches. */
int cfs_bs_inspect_state(cfs_bs_store_t *store, cfs_bs_state_t *out,
                         cfs_bs_workspace_t *workspace);
int cfs_bs_inspect_branch(cfs_bs_store_t *store, uint32_t slot,
                          cfs_bs_branch_t *out,
                          cfs_bs_workspace_t *workspace);
int cfs_bs_inspect_pin(cfs_bs_store_t *store, uint32_t slot,
                       cfs_bs_pin_t *out, cfs_bs_workspace_t *workspace);
int cfs_bs_inspect_row(cfs_bs_store_t *store, uint64_t block,
                       cfs_bs_row_t *out, cfs_bs_workspace_t *workspace);
int cfs_bs_inspect_stats(cfs_bs_store_t *store, cfs_bs_stats_t *out,
                         cfs_bs_workspace_t *workspace);

#endif /* CFS_BRANCH_STORE_H */
