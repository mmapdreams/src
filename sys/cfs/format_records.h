/* SPDX-License-Identifier: ISC
 *
 * format_records.h -- the in-memory record types and the bounded
 * encode/decode surface for the cfs v0 on-disk format.
 *
 * Included from format.h, which owns the named byte offsets these encoders
 * and decoders write and read. The structs here are natural host-layout C
 * used only in memory: no struct is ever overlaid on disk bytes, so the
 * on-disk order and endianness live entirely in format.h's offset
 * constants and in the codec.
 */
#ifndef CFS_FORMAT_RECORDS_H
#define CFS_FORMAT_RECORDS_H

#include <stddef.h>
#include <stdint.h>

#include "scratch.h"

/* In-memory record types (natural host layout). Reserved fields: encoder
 * zero-fills, decoder ignores (section 5.4 rule 1). */

typedef struct {
    uint64_t h_checksum;
    uint16_t h_type;
    uint16_t h_flags;
    uint64_t h_generation;
    uint64_t h_blkno;
    uint16_t h_fmt_version;
} cfs_block_header_t;

typedef struct {
    uint16_t r_tree_id;
    uint16_t r_flags;
    uint32_t r_height;
    uint64_t r_root_addr;
    uint64_t r_item_count;
    uint64_t r_root_xsum;
} cfs_root_record_t;

typedef struct {
    cfs_block_header_t hdr;
    uint64_t a_magic;
    uint8_t a_fs_uuid[16];
    uint32_t a_block_size;
    uint32_t a_ckpt_slot_size;
    uint32_t a_ring_slots;
    uint64_t a_total_blocks;
    uint64_t a_primary_ring_start;
    uint64_t a_secondary_ring_start;
    uint64_t a_summary_start;
    uint64_t a_summary_len;
    uint64_t a_meta_start;
    uint64_t a_data_start_hint;
    uint64_t a_intent_log_start;
    uint64_t a_intent_log_len;
    uint64_t a_feature_compat;
    uint64_t a_feature_ro_compat;
    uint64_t a_feature_incompat;
    uint64_t a_mkfs_generation;
    uint8_t a_label[64];
} cfs_anchor_t;

typedef struct {
    uint64_t magic;
    uint16_t version;
    uint16_t profile;
    uint16_t owner_kind;
    uint16_t state;
    uint64_t epoch;
    uint64_t ledger_start;
    uint64_t ledger_nodes;
    uint64_t record_cap;
    uint64_t ledger_serial;
    uint64_t ledger_root;
    uint64_t ledger_root_xsum;
    uint64_t ledger_records;
    uint64_t free_blocks;
    uint64_t escrow_blocks;
    uint64_t surplus_blocks;
    uint64_t metadata_nodes;
    uint64_t claim_blocks;
    uint64_t queue_claim;
    uint64_t capital_floor;
    uint64_t escape_fresh_max;
    uint64_t raw_range_max;
    uint64_t semantic_range_max;
    uint64_t metadata_max;
    uint64_t runway_max;
    uint64_t intent_fresh_max;
    uint64_t intent_raw_max;
    uint64_t intent_debit_max;
    uint64_t owner_id;
    uint64_t owner_epoch;
    uint64_t owner_remaining_debit;
    uint64_t owner_rank;
    uint64_t owner_fingerprint;
    uint64_t cohort_epoch;
    uint64_t scan_state;
    uint64_t scan_source;
    uint64_t retire_rows;
    uint64_t retire_blocks;
    uint64_t snapshot_scan;
    uint64_t orphan_scan;
    uint64_t intent_terminal;
    uint64_t tail_phase;
    uint64_t tail_cursor;
    uint64_t tail_source;
    uint64_t tail_digest;
    uint64_t tail_metadata_nodes;
    uint64_t tail_escrow_blocks;
    uint64_t incarnation_epoch;
    uint64_t runway_blocks;
    uint64_t service_class;
    uint64_t foreground_streak;
    uint64_t reserved;
} cfs_cleanup_capability_t;

typedef struct {
    cfs_block_header_t hdr;
    uint64_t s_magic;
    uint32_t s_block_size;
    uint32_t s_slot_size;
    uint32_t s_ring_slots;
    uint64_t s_total_blocks;
    uint64_t s_primary_ring_start;
    uint64_t s_secondary_ring_start;
    uint64_t s_summary_start;
    uint64_t s_summary_len;
    uint64_t s_meta_start;
    uint64_t s_data_start_hint;
    uint64_t s_intent_log_start;
    uint64_t s_intent_log_len;
    uint64_t s_summary_addr;
    uint64_t s_intent_log_head;
    uint64_t s_intent_log_tail;
    uint64_t s_feature_compat;
    uint64_t s_feature_ro_compat;
    uint64_t s_feature_incompat;
    cfs_root_record_t s_roots[CFS_CKPT_ROOT_COUNT];
    uint64_t s_next_ino;
    uint64_t s_next_snapid;
    uint64_t s_revert_snapid;
    uint64_t s_retire_cursor;
    cfs_cleanup_capability_t s_cleanup;

    /* Decoder evidence for the feature-assigned trailing zero range. */
    uint8_t s_cleanup_tail_zero;
} cfs_ckpt_slot_t;

typedef struct {
    cfs_block_header_t hdr;
    uint64_t cl_magic;
    uint64_t cl_serial;
    uint64_t cl_logical_id;
    uint16_t cl_level;
    uint16_t cl_lane;
    uint32_t cl_flags;
    uint16_t cl_entry_count;
    uint64_t cl_subtree_records;
    uint64_t cl_subtree_blocks;
    uint64_t cl_subtree_base;
    uint64_t cl_subtree_runway;
    uint64_t cl_subtree_surplus_raw;
    uint64_t cl_subtree_surplus_semantic;
    uint64_t cl_max_incarnation;
    uint64_t cl_semantic_digest;
} cfs_cleanup_ledger_header_t;

typedef struct {
    uint64_t cl_start;
    uint64_t cl_blocks;
    uint64_t cl_death_generation;
    uint64_t cl_claim_reserved;
    uint64_t cl_incarnation_generation;
    uint16_t cl_class;
    uint16_t cl_flags;
} cfs_cleanup_ledger_record_t;

typedef struct {
    uint64_t child_logical_id;
    uint64_t child_physical_addr;
    uint64_t child_serial;
    uint64_t child_whole_block_xsum;
    uint64_t child_subtree_records;
    uint64_t child_subtree_blocks;
} cfs_cleanup_ledger_child_t;

typedef struct {
    cfs_block_header_t hdr;
    uint16_t n_level;
    uint16_t n_nitems;
    uint16_t n_tree_id;
    uint16_t n_lower;
    uint16_t n_upper;
    uint16_t n_flags;
} cfs_node_header_t;

typedef struct {
    uint64_t k_objid; /* big-endian on disk */
    uint8_t k_type;
    uint64_t k_offset; /* big-endian on disk */
} cfs_key_t;

typedef struct {
    cfs_key_t e_key;
    uint64_t e_child_addr;
    uint64_t e_child_xsum;
    uint64_t e_child_gen;
} cfs_routing_entry_t;

typedef struct {
    uint16_t s_off;
    uint16_t s_len;
} cfs_leaf_slot_t;

typedef struct {
    uint32_t i_mode;
    uint32_t i_nlink;
    uint32_t i_uid;
    uint32_t i_gid;
    uint64_t i_size;
    uint64_t i_alloc_bytes;
    int64_t i_atime;
    int64_t i_mtime;
    int64_t i_ctime;
    int64_t i_btime;
    uint64_t i_gen;
    uint64_t i_flags;
    uint64_t i_parent;
    uint64_t i_rdev;
    uint64_t i_cleanup_epoch;
    uint64_t i_cleanup_remaining_debit;
} cfs_inode_t;

typedef struct {
    uint64_t d_child_ino;
    uint8_t d_type;
    uint8_t d_name_len;
    uint8_t d_name[CFS_NAME_MAX];
} cfs_dirent_t;

typedef struct {
    uint16_t len; /* value length == i_size */
    uint8_t data[CFS_INLINE_DATA_MAX];
} cfs_inline_data_t;

typedef struct {
    uint16_t len; /* target length == i_size */
    uint8_t target[CFS_SYMLINK_MAX];
} cfs_symlink_t;

typedef struct {
    uint16_t x_name_len;
    uint16_t x_value_len;
    uint8_t x_name[CFS_NAME_MAX];
    uint8_t x_value[CFS_XATTR_VALUE_MAX];
} cfs_xattr_t;

typedef struct {
    uint64_t fe_phys_start;
    uint64_t fe_blocks;
    uint32_t fe_flags;
} cfs_file_extent_t;

typedef struct {
    uint64_t rc_blocks;
    uint32_t rc_count;
} cfs_refcount_t;

typedef struct {
    uint64_t qr_death_generation;
    uint64_t qr_blocks;
    uint64_t qr_cohort_epoch;
} cfs_ckpt_retire_t;

typedef struct {
    uint64_t ff_blocks;
} cfs_free_extent_t;

typedef struct {
    uint64_t sd_create_gen;
    int64_t sd_create_time;
    uint64_t sd_flags;
    uint64_t sd_name_hash;
    uint16_t sd_reap_phase;
    uint16_t sd_reap_level;
    cfs_key_t sd_reap_key;
    cfs_root_record_t sd_roots[CFS_SNAP_ROOT_COUNT];
    uint64_t sd_cleanup_epoch;
    uint64_t sd_cleanup_remaining_debit;
    uint64_t sd_cleanup_original_debit;
    uint64_t sd_cleanup_units_remaining;
    uint16_t sd_cleanup_profile;
    uint16_t sd_cleanup_flags;
    uint64_t sd_cleanup_fingerprint;
} cfs_snap_desc_t;

typedef struct {
    uint64_t sn_snap_id;
    uint8_t sn_name_len;
    uint8_t sn_name[CFS_NAME_MAX];
} cfs_snap_name_t;

typedef struct {
    cfs_block_header_t hdr;
    uint64_t l_seq;
    uint16_t l_nrecs;
    uint16_t l_bytes;
} cfs_intent_block_header_t;

typedef struct {
    uint16_t rf_type;
    uint16_t rf_len;
    uint64_t rf_xsum;
} cfs_intent_rec_frame_t;

typedef struct {
    uint64_t ino;
    cfs_inode_t inode;
} cfs_intent_inode_t;

typedef struct {
    uint64_t ino;
    uint64_t file_offset;
    uint64_t fe_phys_start;
    uint64_t fe_blocks;
    uint32_t fe_flags;
} cfs_intent_extent_t;

typedef struct {
    uint64_t dir_ino;
    uint64_t child_ino;
    uint8_t d_type;
    uint8_t op;
    uint8_t name_len;
    uint8_t name[CFS_NAME_MAX];
} cfs_intent_dirent_t;

typedef struct {
    uint64_t base_gen;
    uint16_t rec_count;
} cfs_intent_commit_t;

typedef struct {
    uint64_t cleanup_epoch;
    uint32_t cleanup_profile;
    uint32_t flags;
    uint64_t fresh_claim;
    uint64_t raw_record_claim;
    uint64_t debit_claim;
    uint64_t payload_claim;
} cfs_intent_cleanup_credit_t;

typedef struct {
    cfs_block_header_t hdr;
    uint64_t u_first_block;
    uint16_t u_nrecs;
    uint16_t u_seg_log2;
} cfs_summary_header_t;

typedef struct {
    uint32_t sm_free_blocks;
    uint32_t sm_largest_run;
    uint16_t sm_class;
} cfs_summary_rec_t;

/* Encode/decode: return value is bytes written/consumed. Block records
 * use an absolute block-start buffer, sub-block records a record-start. */

size_t cfs_fmt_block_header_encode(uint8_t *buf, const cfs_block_header_t *h);
size_t cfs_fmt_block_header_decode(cfs_block_header_t *h, const uint8_t *buf);

size_t cfs_fmt_root_record_encode(uint8_t *buf, const cfs_root_record_t *r);
size_t cfs_fmt_root_record_decode(cfs_root_record_t *r, const uint8_t *buf);

size_t cfs_fmt_anchor_encode(uint8_t *buf, const cfs_anchor_t *a);
size_t cfs_fmt_anchor_decode(cfs_anchor_t *a, const uint8_t *buf);

size_t cfs_fmt_cleanup_capability_encode(uint8_t *buf,
    const cfs_cleanup_capability_t *capability);
size_t cfs_fmt_cleanup_capability_decode(cfs_cleanup_capability_t *capability,
    const uint8_t *buf);

size_t cfs_fmt_ckpt_slot_encode(uint8_t *buf, const cfs_ckpt_slot_t *s);
size_t cfs_fmt_ckpt_slot_decode(cfs_ckpt_slot_t *s, const uint8_t *buf);

size_t cfs_fmt_cleanup_ledger_header_encode(uint8_t *buf,
    const cfs_cleanup_ledger_header_t *header);
size_t cfs_fmt_cleanup_ledger_header_decode(
    cfs_cleanup_ledger_header_t *header, const uint8_t *buf);
size_t cfs_fmt_cleanup_ledger_record_encode(uint8_t *buf,
    const cfs_cleanup_ledger_record_t *record);
size_t cfs_fmt_cleanup_ledger_record_decode(
    cfs_cleanup_ledger_record_t *record, const uint8_t *buf);
size_t cfs_fmt_cleanup_ledger_child_encode(uint8_t *buf,
    const cfs_cleanup_ledger_child_t *child);
size_t cfs_fmt_cleanup_ledger_child_decode(cfs_cleanup_ledger_child_t *child,
    const uint8_t *buf);

size_t cfs_fmt_node_header_encode(uint8_t *buf, const cfs_node_header_t *n);
size_t cfs_fmt_node_header_decode(cfs_node_header_t *n, const uint8_t *buf);

size_t cfs_fmt_key_encode(uint8_t *buf, const cfs_key_t *k);
size_t cfs_fmt_key_decode(cfs_key_t *k, const uint8_t *buf);

size_t cfs_fmt_routing_entry_encode(uint8_t *buf, const cfs_routing_entry_t *e);
size_t cfs_fmt_routing_entry_decode(cfs_routing_entry_t *e, const uint8_t *buf);

size_t cfs_fmt_leaf_slot_encode(uint8_t *buf, const cfs_leaf_slot_t *s);
size_t cfs_fmt_leaf_slot_decode(cfs_leaf_slot_t *s, const uint8_t *buf);

size_t cfs_fmt_inode_encode(uint8_t *buf, const cfs_inode_t *i);
size_t cfs_fmt_inode_decode(cfs_inode_t *i, const uint8_t *buf);

size_t cfs_fmt_dirent_encode(uint8_t *buf, const cfs_dirent_t *d);
size_t cfs_fmt_dirent_decode(cfs_dirent_t *d, const uint8_t *buf);

/* Validates a raw inode leaf value: exactly CFS_INODE_VALUE_SIZE bytes,
 * key id 1/2/>=16, type CFS_ITEM_INODE, offset 0. Infers no linkage. */
int cfs_fmt_inode_validate(const cfs_key_t *key, const uint8_t *value,
    size_t value_len, cfs_inode_t *out);

/* Validates raw dirent leaf value: header + d_name_len (1..255), d_type,
 * child inode; dir-inode key, CFS_ITEM_DIRENT. Key hash not recomputed. */
int cfs_fmt_dirent_validate(const cfs_key_t *key, const uint8_t *value,
    size_t value_len, cfs_dirent_t *out);

/* Inline-data and symlink values are raw bytes whose length lives in the
 * owning slot (s_len - 17), not in the value, so decode takes it. */
size_t cfs_fmt_inline_data_encode(uint8_t *buf, const cfs_inline_data_t *v);
size_t cfs_fmt_inline_data_decode(cfs_inline_data_t *v, const uint8_t *buf,
    size_t value_len);

/* Validates raw inline-data item. Zero-length value is the canonical
 * empty-file representation and may have a NULL value pointer. */
int cfs_fmt_inline_data_validate(const cfs_key_t *key,
    const uint8_t *value, size_t value_len, cfs_inline_data_t *out);

size_t cfs_fmt_symlink_encode(uint8_t *buf, const cfs_symlink_t *v);
size_t cfs_fmt_symlink_decode(cfs_symlink_t *v, const uint8_t *buf,
    size_t value_len);

/* Validates raw symlink-target item: zero target length is valid (may
 * be NULL); key needs assigned inode id, CFS_ITEM_SYMLINK, offset 0. */
int cfs_fmt_symlink_validate(const cfs_key_t *key,
    const uint8_t *value, size_t value_len, cfs_symlink_t *out);

size_t cfs_fmt_xattr_encode(uint8_t *buf, const cfs_xattr_t *x);
size_t cfs_fmt_xattr_decode(cfs_xattr_t *x, const uint8_t *buf);

size_t cfs_fmt_file_extent_encode(uint8_t *buf, const cfs_file_extent_t *fe);
size_t cfs_fmt_file_extent_decode(cfs_file_extent_t *fe, const uint8_t *buf);

/* Validates raw file-extent item with an assigned inode id, against the
 * selected slot's complete allocatable span. Reserved bytes ignored. */
int cfs_fmt_file_extent_validate(const cfs_key_t *key,
    const uint8_t *value, size_t value_len, uint64_t alloc_lo,
    uint64_t alloc_hi, cfs_file_extent_t *out);

size_t cfs_fmt_refcount_encode(uint8_t *buf, const cfs_refcount_t *rc);
size_t cfs_fmt_refcount_decode(cfs_refcount_t *rc, const uint8_t *buf);

/* Validates raw extent-refcount value (CFS_REFCOUNT_VALUE_SIZE bytes):
 * type-7, zero flags, nonempty range, nonzero count within slot span. */
int cfs_fmt_refcount_validate(uint16_t tree_id, const cfs_key_t *key,
    const uint8_t *value, size_t value_len, uint64_t meta_start,
    uint64_t intent_log_start, cfs_refcount_t *out);

size_t cfs_fmt_ckpt_retire_encode(uint8_t *buf, const cfs_ckpt_retire_t *qr);
size_t cfs_fmt_ckpt_retire_decode(cfs_ckpt_retire_t *qr, const uint8_t *buf);

/* Validates raw checkpoint-retirement value (CFS_CKPT_RETIRE_VALUE_SIZE
 * bytes): queue namespace, zero fields, range in span, death gen <= gen. */
int cfs_fmt_ckpt_retire_validate(uint16_t tree_id, const cfs_key_t *key,
    const uint8_t *value, size_t value_len, uint64_t meta_start,
    uint64_t intent_log_start, uint64_t selected_generation,
    cfs_ckpt_retire_t *out);

/* Agreement between incompat features and a type-11 retirement item:
 * has_retire_item requires the bit; bit alone is a valid empty queue. */
static inline int cfs_fmt_ckpt_retire_feature_agrees(
    uint64_t feature_incompat, int has_retire_item) {
    return has_retire_item == 0 ||
           (feature_incompat & CFS_INCOMPAT_RETIRE_QUEUE) != 0;
}

size_t cfs_fmt_free_extent_encode(uint8_t *buf, const cfs_free_extent_t *ff);
size_t cfs_fmt_free_extent_decode(cfs_free_extent_t *ff, const uint8_t *buf);

/* Validates raw free-space-tree extent: exactly one nonempty type-8
 * range contained in the selected allocatable span. */
int cfs_fmt_free_extent_validate(uint16_t tree_id, const cfs_key_t *key,
    const uint8_t *value, size_t value_len, uint64_t meta_start,
    uint64_t intent_log_start, cfs_free_extent_t *out);

size_t cfs_fmt_snap_desc_encode(uint8_t *buf, const cfs_snap_desc_t *sd);
size_t cfs_fmt_snap_desc_decode(cfs_snap_desc_t *sd, const uint8_t *buf);

size_t cfs_fmt_snap_name_encode(uint8_t *buf, const cfs_snap_name_t *sn);
size_t cfs_fmt_snap_name_decode(cfs_snap_name_t *sn, const uint8_t *buf);

size_t cfs_fmt_intent_block_header_encode(uint8_t *buf,
    const cfs_intent_block_header_t *l);
size_t cfs_fmt_intent_block_header_decode(cfs_intent_block_header_t *l,
    const uint8_t *buf);

size_t cfs_fmt_intent_rec_frame_encode(uint8_t *buf,
    const cfs_intent_rec_frame_t *f);
size_t cfs_fmt_intent_rec_frame_decode(cfs_intent_rec_frame_t *f,
    const uint8_t *buf);

size_t cfs_fmt_intent_inode_encode(uint8_t *buf, const cfs_intent_inode_t *r);
size_t cfs_fmt_intent_inode_decode(cfs_intent_inode_t *r, const uint8_t *buf);

size_t cfs_fmt_intent_extent_encode(uint8_t *buf, const cfs_intent_extent_t *r);
size_t cfs_fmt_intent_extent_decode(cfs_intent_extent_t *r, const uint8_t *buf);

size_t cfs_fmt_intent_dirent_encode(uint8_t *buf, const cfs_intent_dirent_t *r);
size_t cfs_fmt_intent_dirent_decode(cfs_intent_dirent_t *r, const uint8_t *buf);

size_t cfs_fmt_intent_commit_encode(uint8_t *buf, const cfs_intent_commit_t *r);
size_t cfs_fmt_intent_commit_decode(cfs_intent_commit_t *r, const uint8_t *buf);

size_t cfs_fmt_intent_cleanup_credit_encode(uint8_t *buf,
    const cfs_intent_cleanup_credit_t *r);
size_t cfs_fmt_intent_cleanup_credit_decode(cfs_intent_cleanup_credit_t *r,
    const uint8_t *buf);

size_t cfs_fmt_summary_header_encode(uint8_t *buf,
    const cfs_summary_header_t *u);
size_t cfs_fmt_summary_header_decode(cfs_summary_header_t *u,
    const uint8_t *buf);

size_t cfs_fmt_summary_rec_encode(uint8_t *buf, const cfs_summary_rec_t *sm);
size_t cfs_fmt_summary_rec_decode(cfs_summary_rec_t *sm, const uint8_t *buf);

/* Whole-block checksum (section 1.3): cfs_hash64 over CFS_BLOCK_SIZE
 * bytes with h_checksum zeroed; scratch needs one CFS_BLOCK_SIZE temp. */
int cfs_fmt_block_checksum(const uint8_t *buf, cfs_scratch_t *scratch,
    uint64_t *out);

/* Per-record intent checksum (section 10.3): cfs_hash64 over rf_type,
 * rf_len (LE), body; body is NULL iff rf_len 0, max CFS_INTENT_MAX_FRAMING. */
int cfs_fmt_intent_rec_xsum(uint16_t rf_type, uint16_t rf_len,
    const uint8_t *body, cfs_scratch_t *scratch, uint64_t *out);

#endif /* CFS_FORMAT_RECORDS_H */
