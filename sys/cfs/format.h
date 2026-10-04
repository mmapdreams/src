/* SPDX-License-Identifier: ISC
 *
 * format.h -- the cfs v0 on-disk format: named constants, in-memory
 * record types, and the bounded encode/decode surface.
 *
 * This header is the C witness of txt/format.md. Every constant, magic,
 * enum, mask, field offset, and structure size below is normative there;
 * the _Static_assert lines pin the relationships the spec states (a
 * table's fields tile its structure with no gap, sizes sum to totals,
 * derived arithmetic like fanout and summary coverage holds). The
 * in-memory structs are natural host-layout C structs used only in
 * memory; the on-disk byte layout is expressed solely by the offset
 * constants and the codec (src/codec.h), never by a struct's layout, so
 * this file never overlays a struct on disk bytes.
 *
 * cfs_core is kernel-destined: this header pulls in only stddef.h and
 * stdint.h and defines no allocation, stdio, or floating point.
 */
#ifndef CFS_FORMAT_H
#define CFS_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#include "scratch.h"

/* 1. Blocks, addresses, common block header (sections 1.4, 1.5): 32-byte
 * header per block; logical block size (1.4) is 4096. */
#define CFS_BLOCK_SIZE 4096

/* "No block" sentinel; never a valid address (section 1.4). */
#define CFS_BLKNO_NONE UINT64_C(0xFFFFFFFFFFFFFFFF)

/* Common block header field offsets and sizes (section 1.5 table). */
#define CFS_H_CHECKSUM_OFF 0
#define CFS_H_CHECKSUM_SIZE 8
#define CFS_H_TYPE_OFF 8
#define CFS_H_TYPE_SIZE 2
#define CFS_H_FLAGS_OFF 10
#define CFS_H_FLAGS_SIZE 2
#define CFS_H_GENERATION_OFF 12
#define CFS_H_GENERATION_SIZE 8
#define CFS_H_BLKNO_OFF 20
#define CFS_H_BLKNO_SIZE 8
#define CFS_H_FMT_VERSION_OFF 28
#define CFS_H_FMT_VERSION_SIZE 2
#define CFS_H_RESERVED_OFF 30
#define CFS_H_RESERVED_SIZE 2
#define CFS_BLOCK_HEADER_SIZE 32

_Static_assert(CFS_H_TYPE_OFF == CFS_H_CHECKSUM_OFF + CFS_H_CHECKSUM_SIZE,
    "h_type follows h_checksum");
_Static_assert(CFS_H_FLAGS_OFF == CFS_H_TYPE_OFF + CFS_H_TYPE_SIZE,
    "h_flags follows h_type");
_Static_assert(CFS_H_GENERATION_OFF == CFS_H_FLAGS_OFF + CFS_H_FLAGS_SIZE,
    "h_generation follows h_flags");
_Static_assert(CFS_H_BLKNO_OFF == CFS_H_GENERATION_OFF + CFS_H_GENERATION_SIZE,
    "h_blkno follows h_generation");
_Static_assert(
    CFS_H_FMT_VERSION_OFF == CFS_H_BLKNO_OFF + CFS_H_BLKNO_SIZE,
    "h_fmt_version follows h_blkno");
_Static_assert(
    CFS_H_RESERVED_OFF == CFS_H_FMT_VERSION_OFF + CFS_H_FMT_VERSION_SIZE,
    "h_reserved follows h_fmt_version");
_Static_assert(
    CFS_H_RESERVED_OFF + CFS_H_RESERVED_SIZE == CFS_BLOCK_HEADER_SIZE,
    "block header tiles 32 bytes");

/* h_fmt_version value a v0 writer stamps (section 1.5). */
#define CFS_FMT_VERSION 0

/* Block types (h_type); enum continues across sections 1.5.1, 6.2,
 * 10.1, 11.1. */
enum cfs_block_type {
    CFS_BLK_NONE = 0,
    CFS_BLK_ANCHOR = 1,
    CFS_BLK_CKPT = 2,
    CFS_BLK_TREE_INTERIOR = 3,
    CFS_BLK_TREE_LEAF = 4,
    CFS_BLK_INTENT = 5,
    CFS_BLK_SUMMARY = 6,
    CFS_BLK_CLEANUP_LEDGER = 7
};

/* 2. Image geometry (sections 2.1-2.3). */

/* Slots per checkpoint ring R (section 4.1). */
#define CFS_CKPT_RING_SLOTS 8

/* Allocator summary sizing (section 2.2). */
#define CFS_SUMMARY_COVERAGE 518144 /* 253 segments * 2048 blocks */
#define CFS_SUMMARY_SEGMENT 2048    /* segment size in blocks (section 11.2) */
#define CFS_SUMMARY_SEG_LOG2 11     /* log2(2048) */

/* Intent-log sizing (section 2.2). */
#define CFS_INTENT_MIN 16    /* blocks (64 KiB) */
#define CFS_INTENT_MAX 65536 /* blocks (256 MiB) */
#define CFS_INTENT_SCALE_DIVISOR 1024 /* L default = ceil(N / 1024) */

/* Metadata-hint fraction 1/16 of allocatable (section 2.1). */
#define CFS_META_HINT_DIVISOR 16

/* Minimum image (section 2.3). */
#define CFS_MIN_ALLOCATABLE_BLOCKS 32 /* min allocatable term of the floor */
#define CFS_MIN_IMAGE_BLOCKS 256      /* mkfs floor (1 MiB) */

/* Structural floor (section 2.3): 1 (anchor) + R (primary) + 2 (min A) +
 * 32 (min allocatable) + CFS_INTENT_MIN (16) + R (secondary) = 67. */
_Static_assert(1 + CFS_CKPT_RING_SLOTS + 2 + CFS_MIN_ALLOCATABLE_BLOCKS +
                       CFS_INTENT_MIN + CFS_CKPT_RING_SLOTS ==
                   67,
    "section 2.3 structural floor is 67 blocks");
_Static_assert(CFS_SUMMARY_SEG_LOG2 == 11 &&
                   (1u << CFS_SUMMARY_SEG_LOG2) == CFS_SUMMARY_SEGMENT,
    "u_seg_log2 = 11 encodes the 2048-block segment");

/* Region-sizing formulas (sections 2.1, 2.2): summary copy blocks S =
 * max(1, ceil(N / coverage)); mounter validates composed regions (4.5). */
static inline uint64_t cfs_summary_copy_blocks(uint64_t n) {
    uint64_t s = (n + CFS_SUMMARY_COVERAGE - 1) / CFS_SUMMARY_COVERAGE;
    return s == 0 ? 1 : s;
}

/* Summary region length A = 2 * S (double-buffered, section 11.3). */
static inline uint64_t cfs_summary_len(uint64_t n) {
    return 2 * cfs_summary_copy_blocks(n);
}

/* Intent-log length L = clamp(ceil(N/1024), MIN, MAX). */
static inline uint64_t cfs_intent_log_len(uint64_t n) {
    uint64_t l = (n + CFS_INTENT_SCALE_DIVISOR - 1) / CFS_INTENT_SCALE_DIVISOR;
    if (l < CFS_INTENT_MIN) {
        l = CFS_INTENT_MIN;
    }

    if (l > CFS_INTENT_MAX) {
        l = CFS_INTENT_MAX;
    }

    return l;
}

/* Allocatable span = N - 2R - L - A - 1 (section 2.1). */
static inline uint64_t cfs_allocatable_blocks(uint64_t n) {
    return n - 2 * (uint64_t)CFS_CKPT_RING_SLOTS - cfs_intent_log_len(n) -
           cfs_summary_len(n) - 1;
}

/* Metadata hint span M = ceil(allocatable / 16). */
static inline uint64_t cfs_meta_hint_blocks(uint64_t allocatable) {
    return (allocatable + CFS_META_HINT_DIVISOR - 1) / CFS_META_HINT_DIVISOR;
}

/* Data hint span D = allocatable - M. */
static inline uint64_t cfs_data_hint_blocks(uint64_t allocatable) {
    return allocatable - cfs_meta_hint_blocks(allocatable);
}

/* Region first-block formulas (section 2.1). */
static inline uint64_t cfs_primary_ring_start(void) {
    return 1;
}

static inline uint64_t cfs_secondary_ring_start(uint64_t n) {
    return n - (uint64_t)CFS_CKPT_RING_SLOTS;
}

static inline uint64_t cfs_summary_start(void) {
    return 1 + (uint64_t)CFS_CKPT_RING_SLOTS;
}

static inline uint64_t cfs_meta_start(uint64_t n) {
    return cfs_summary_start() + cfs_summary_len(n);
}

static inline uint64_t cfs_data_start_hint(uint64_t n) {
    return cfs_meta_start(n) + cfs_meta_hint_blocks(cfs_allocatable_blocks(n));
}

static inline uint64_t cfs_intent_log_start(uint64_t n) {
    return n - (uint64_t)CFS_CKPT_RING_SLOTS - cfs_intent_log_len(n);
}

/* Byte offset of block b (section 1.4). */
static inline uint64_t cfs_block_byte_offset(uint64_t b) {
    return b * (uint64_t)CFS_BLOCK_SIZE;
}

/* ENOSPC reserve (section 12.1): reserve = max(MIN, ceil(alloc/32)). */
#define CFS_RESERVE_MIN 64      /* blocks (256 KiB) */
#define CFS_RESERVE_DIVISOR 32  /* CFS_RESERVE_FRACTION = 1/32 */
static inline uint64_t cfs_reserve_blocks(uint64_t allocatable) {
    uint64_t r = (allocatable + CFS_RESERVE_DIVISOR - 1) / CFS_RESERVE_DIVISOR;
    return r < CFS_RESERVE_MIN ? CFS_RESERVE_MIN : r;
}

/* 3. Anchor block (section 3.2), whole 4096-byte block. Magic bytes
 * 63 66 73 41 4e 43 48 52 ("cfsANCHR"). */
#define CFS_ANCHOR_MAGIC UINT64_C(0x5248434E41736663)

#define CFS_A_MAGIC_OFF 32
#define CFS_A_MAGIC_SIZE 8
#define CFS_A_FS_UUID_OFF 40
#define CFS_A_FS_UUID_SIZE 16
#define CFS_A_BLOCK_SIZE_OFF 56
#define CFS_A_BLOCK_SIZE_SIZE 4
#define CFS_A_CKPT_SLOT_SIZE_OFF 60
#define CFS_A_CKPT_SLOT_SIZE_SIZE 4
#define CFS_A_RING_SLOTS_OFF 64
#define CFS_A_RING_SLOTS_SIZE 4
#define CFS_A_RESERVED0_OFF 68
#define CFS_A_RESERVED0_SIZE 4
#define CFS_A_TOTAL_BLOCKS_OFF 72
#define CFS_A_TOTAL_BLOCKS_SIZE 8
#define CFS_A_PRIMARY_RING_START_OFF 80
#define CFS_A_SECONDARY_RING_START_OFF 88
#define CFS_A_SUMMARY_START_OFF 96
#define CFS_A_SUMMARY_LEN_OFF 104
#define CFS_A_META_START_OFF 112
#define CFS_A_DATA_START_HINT_OFF 120
#define CFS_A_INTENT_LOG_START_OFF 128
#define CFS_A_INTENT_LOG_LEN_OFF 136
#define CFS_A_FEATURE_COMPAT_OFF 144
#define CFS_A_FEATURE_RO_COMPAT_OFF 152
#define CFS_A_FEATURE_INCOMPAT_OFF 160
#define CFS_A_MKFS_GENERATION_OFF 168
#define CFS_A_LABEL_OFF 176
#define CFS_A_LABEL_SIZE 64
#define CFS_A_RESERVED_OFF 240
#define CFS_A_RESERVED_SIZE 3856

_Static_assert(CFS_A_MAGIC_OFF == CFS_BLOCK_HEADER_SIZE,
    "anchor body starts after the common header");
_Static_assert(CFS_A_LABEL_OFF + CFS_A_LABEL_SIZE == CFS_A_RESERVED_OFF,
    "a_reserved follows a_label");
_Static_assert(CFS_A_RESERVED_OFF + CFS_A_RESERVED_SIZE == CFS_BLOCK_SIZE,
    "anchor tiles one 4096-byte block");

/* 4. Checkpoint slot, per-tree root records (sections 4.2, 4.3). Magic
 * bytes 63 66 73 43 48 4b 50 54 ("cfsCHKPT"). */
#define CFS_CKPT_MAGIC UINT64_C(0x54504B4843736663)

/* Slot-size invariant: v0 slot is one block (section 4.1). */
#define CFS_CKPT_SLOT_BLOCKS 1

/* Root-record array in a slot (section 4.3). */
#define CFS_CKPT_ROOT_COUNT 16

/* Per-tree root record (section 4.3), 32 bytes; local +0 offsets. */
#define CFS_R_TREE_ID_OFF 0
#define CFS_R_TREE_ID_SIZE 2
#define CFS_R_FLAGS_OFF 2
#define CFS_R_FLAGS_SIZE 2
#define CFS_R_HEIGHT_OFF 4
#define CFS_R_HEIGHT_SIZE 4
#define CFS_R_ROOT_ADDR_OFF 8
#define CFS_R_ITEM_COUNT_OFF 16
#define CFS_R_ROOT_XSUM_OFF 24
#define CFS_ROOT_RECORD_SIZE 32

_Static_assert(CFS_R_HEIGHT_OFF == CFS_R_FLAGS_OFF + CFS_R_FLAGS_SIZE,
    "r_height follows r_flags");
_Static_assert(CFS_R_ROOT_ADDR_OFF == CFS_R_HEIGHT_OFF + CFS_R_HEIGHT_SIZE,
    "r_root_addr follows r_height");
_Static_assert(CFS_R_ROOT_XSUM_OFF + 8 == CFS_ROOT_RECORD_SIZE,
    "root record tiles 32 bytes");

/* Tree ids (section 4.3.1). */
enum cfs_tree_id {
    CFS_TREE_NONE = 0,
    CFS_TREE_NAMESPACE = 1,
    CFS_TREE_EXTENT = 2,
    CFS_TREE_REFCOUNT = 3,
    CFS_TREE_FREESPACE = 4,
    CFS_TREE_SNAPSHOT = 5
};

/* Checkpoint slot field offsets (section 4.2). Whole 4096-byte block. */
#define CFS_S_MAGIC_OFF 32
#define CFS_S_MAGIC_SIZE 8
#define CFS_S_BLOCK_SIZE_OFF 40
#define CFS_S_BLOCK_SIZE_SIZE 4
#define CFS_S_SLOT_SIZE_OFF 44
#define CFS_S_SLOT_SIZE_SIZE 4
#define CFS_S_RING_SLOTS_OFF 48
#define CFS_S_RING_SLOTS_SIZE 4
#define CFS_S_RESERVED0_OFF 52
#define CFS_S_RESERVED0_SIZE 4
#define CFS_S_TOTAL_BLOCKS_OFF 56
#define CFS_S_PRIMARY_RING_START_OFF 64
#define CFS_S_SECONDARY_RING_START_OFF 72
#define CFS_S_SUMMARY_START_OFF 80
#define CFS_S_SUMMARY_LEN_OFF 88
#define CFS_S_META_START_OFF 96
#define CFS_S_DATA_START_HINT_OFF 104
#define CFS_S_INTENT_LOG_START_OFF 112
#define CFS_S_INTENT_LOG_LEN_OFF 120
#define CFS_S_SUMMARY_ADDR_OFF 128
#define CFS_S_INTENT_LOG_HEAD_OFF 136
#define CFS_S_INTENT_LOG_TAIL_OFF 144
#define CFS_S_FEATURE_COMPAT_OFF 152
#define CFS_S_FEATURE_RO_COMPAT_OFF 160
#define CFS_S_FEATURE_INCOMPAT_OFF 168
#define CFS_S_ROOTS_OFF 176
#define CFS_S_ROOTS_SIZE 512
#define CFS_S_NEXT_INO_OFF 688
#define CFS_S_NEXT_SNAPID_OFF 696
#define CFS_S_REVERT_SNAPID_OFF 704
#define CFS_S_RETIRE_CURSOR_OFF 712
#define CFS_S_RETIRE_CURSOR_SIZE 8
#define CFS_S_CLEANUP_OFF 720
#define CFS_S_CLEANUP_SIZE 392
#define CFS_S_RESERVED_OFF 1112
#define CFS_S_RESERVED_SIZE 2984

_Static_assert(CFS_S_MAGIC_OFF == CFS_BLOCK_HEADER_SIZE,
    "slot body starts after the common header");
_Static_assert(CFS_S_ROOTS_SIZE == CFS_CKPT_ROOT_COUNT * CFS_ROOT_RECORD_SIZE,
    "s_roots holds 16 root records");
_Static_assert(CFS_S_NEXT_INO_OFF == CFS_S_ROOTS_OFF + CFS_S_ROOTS_SIZE,
    "s_next_ino follows the root array");
_Static_assert(CFS_S_CLEANUP_OFF ==
        CFS_S_RETIRE_CURSOR_OFF + CFS_S_RETIRE_CURSOR_SIZE,
    "s_cleanup follows the retirement cursor");
_Static_assert(CFS_S_RESERVED_OFF ==
        CFS_S_CLEANUP_OFF + CFS_S_CLEANUP_SIZE,
    "s_reserved follows the cleanup capability");
_Static_assert(CFS_S_RESERVED_OFF + CFS_S_RESERVED_SIZE == CFS_BLOCK_SIZE,
    "slot tiles one 4096-byte block");

/* 5. Feature flags (section 5.3). */
#define CFS_INCOMPAT_RETIRE_QUEUE UINT64_C(0x1)
#define CFS_INCOMPAT_CLEANUP_LEDGER UINT64_C(0x2)

#define CFS_COMPAT_SUPPORTED UINT64_C(0)
#define CFS_RO_COMPAT_SUPPORTED UINT64_C(0)
/* Public writers remain bit-0-only; bit 1 is read-only. */
#define CFS_INCOMPAT_SUPPORTED CFS_INCOMPAT_RETIRE_QUEUE
#define CFS_INCOMPAT_READ_SUPPORTED \
    (CFS_INCOMPAT_RETIRE_QUEUE | CFS_INCOMPAT_CLEANUP_LEDGER)

/* 5.1 Profile 1 cleanup capability and fixed ledger. */

#define CFS_CLEANUP_MAGIC UINT64_C(0x5041434c43736663)
#define CFS_CLEANUP_VERSION 1u
#define CFS_CLEANUP_PROFILE_1 1u

#define CFS_CLEANUP_PMAX UINT64_C(262144)
#define CFS_CLEANUP_METADATA_MAX UINT64_C(262144)
#define CFS_CLEANUP_RECORD_CAP UINT64_C(262144)
#define CFS_CLEANUP_RUNWAY_MAX UINT64_C(144701)
#define CFS_CLEANUP_INTENT_DEBIT_MAX UINT64_C(144701)
#define CFS_CLEANUP_CAPITAL_FLOOR UINT64_C(92224)
#define CFS_CLEANUP_ESCAPE_FRESH_MAX UINT64_C(9792)
#define CFS_CLEANUP_RAW_RANGE_MAX UINT64_C(248760)
#define CFS_CLEANUP_SEMANTIC_RANGE_MAX UINT64_C(1)
#define CFS_CLEANUP_INTENT_FRESH_MAX UINT64_C(4096)
#define CFS_CLEANUP_INTENT_RAW_MAX UINT64_C(4096)
#define CFS_CLEANUP_SHARD_SPAN UINT64_C(64)
#define CFS_CLEANUP_RADIX_FANOUT 64u
#define CFS_CLEANUP_RADIX_DEPTH 2u
#define CFS_CLEANUP_LOGICAL_LEAVES UINT64_C(4096)
#define CFS_CLEANUP_LOGICAL_NODES UINT64_C(4161)
#define CFS_CLEANUP_LANES 17u
#define CFS_CLEANUP_LEDGER_BLOCKS UINT64_C(70737)
#define CFS_CLEANUP_INTENT_LOG_MAX UINT64_C(65536)
#define CFS_CLEANUP_IMAGE_BLOCKS_MAX UINT64_C(398436)
#define CFS_CLEANUP_KEY_MAX UINT64_C(67108863)
#define CFS_CLEANUP_FOREGROUND_STREAK_MAX UINT64_C(8)
#define CFS_CLEANUP_GEN_NEED_MAX UINT64_C(3803858920465793)
#define CFS_CLEANUP_EPOCH_NEED_MAX UINT64_C(5100974812344628413)

_Static_assert(CFS_CLEANUP_LOGICAL_NODES * CFS_CLEANUP_LANES ==
        CFS_CLEANUP_LEDGER_BLOCKS,
    "Profile 1 fixed-ledger geometry");

/* Absolute offsets in the checkpoint block. */
#define CFS_SC_MAGIC_OFF 720
#define CFS_SC_VERSION_OFF 728
#define CFS_SC_PROFILE_OFF 730
#define CFS_SC_OWNER_KIND_OFF 732
#define CFS_SC_STATE_OFF 734
#define CFS_SC_EPOCH_OFF 736
#define CFS_SC_LEDGER_START_OFF 744
#define CFS_SC_LEDGER_NODES_OFF 752
#define CFS_SC_RECORD_CAP_OFF 760
#define CFS_SC_LEDGER_SERIAL_OFF 768
#define CFS_SC_LEDGER_ROOT_OFF 776
#define CFS_SC_LEDGER_ROOT_XSUM_OFF 784
#define CFS_SC_LEDGER_RECORDS_OFF 792
#define CFS_SC_FREE_BLOCKS_OFF 800
#define CFS_SC_ESCROW_BLOCKS_OFF 808
#define CFS_SC_SURPLUS_BLOCKS_OFF 816
#define CFS_SC_METADATA_NODES_OFF 824
#define CFS_SC_CLAIM_BLOCKS_OFF 832
#define CFS_SC_QUEUE_CLAIM_OFF 840
#define CFS_SC_CAPITAL_FLOOR_OFF 848
#define CFS_SC_ESCAPE_FRESH_MAX_OFF 856
#define CFS_SC_RAW_RANGE_MAX_OFF 864
#define CFS_SC_SEMANTIC_RANGE_MAX_OFF 872
#define CFS_SC_METADATA_MAX_OFF 880
#define CFS_SC_RUNWAY_MAX_OFF 888
#define CFS_SC_INTENT_FRESH_MAX_OFF 896
#define CFS_SC_INTENT_RAW_MAX_OFF 904
#define CFS_SC_INTENT_DEBIT_MAX_OFF 912
#define CFS_SC_OWNER_ID_OFF 920
#define CFS_SC_OWNER_EPOCH_OFF 928
#define CFS_SC_OWNER_REMAINING_DEBIT_OFF 936
#define CFS_SC_OWNER_RANK_OFF 944
#define CFS_SC_OWNER_FINGERPRINT_OFF 952
#define CFS_SC_COHORT_EPOCH_OFF 960
#define CFS_SC_SCAN_STATE_OFF 968
#define CFS_SC_SCAN_SOURCE_OFF 976
#define CFS_SC_RETIRE_ROWS_OFF 984
#define CFS_SC_RETIRE_BLOCKS_OFF 992
#define CFS_SC_SNAPSHOT_SCAN_OFF 1000
#define CFS_SC_ORPHAN_SCAN_OFF 1008
#define CFS_SC_INTENT_TERMINAL_OFF 1016
#define CFS_SC_TAIL_PHASE_OFF 1024
#define CFS_SC_TAIL_CURSOR_OFF 1032
#define CFS_SC_TAIL_SOURCE_OFF 1040
#define CFS_SC_TAIL_DIGEST_OFF 1048
#define CFS_SC_TAIL_METADATA_NODES_OFF 1056
#define CFS_SC_TAIL_ESCROW_BLOCKS_OFF 1064
#define CFS_SC_INCARNATION_EPOCH_OFF 1072
#define CFS_SC_RUNWAY_BLOCKS_OFF 1080
#define CFS_SC_SERVICE_CLASS_OFF 1088
#define CFS_SC_FOREGROUND_STREAK_OFF 1096
#define CFS_SC_RESERVED_OFF 1104

_Static_assert(CFS_SC_MAGIC_OFF == CFS_S_CLEANUP_OFF,
    "cleanup capability starts at byte 720");
_Static_assert(CFS_SC_RESERVED_OFF + 8 == CFS_S_RESERVED_OFF,
    "cleanup capability tiles bytes 720 through 1111");

enum cfs_cleanup_owner_kind {
    CFS_CLEANUP_OWNER_NONE = 0,
    CFS_CLEANUP_OWNER_SNAPSHOT = 1,
    CFS_CLEANUP_OWNER_ORPHAN = 2,
    CFS_CLEANUP_OWNER_RETIREMENT = 3,
    CFS_CLEANUP_OWNER_REVERT = 4
};

enum cfs_cleanup_state {
    CFS_CLEANUP_STATE_IDLE = 0,
    CFS_CLEANUP_STATE_OWNER_ACTIVE = 1,
    CFS_CLEANUP_STATE_OWNER_WAIT_PINNED = 2,
    CFS_CLEANUP_STATE_TAIL_AUDIT = 3,
    CFS_CLEANUP_STATE_TAIL_WAIT_PINNED = 4,
    CFS_CLEANUP_STATE_TAIL_STABLE = 5,
    CFS_CLEANUP_STATE_DISCOVER_SNAPSHOT = 6,
    CFS_CLEANUP_STATE_DISCOVER_ORPHAN = 7,
    CFS_CLEANUP_STATE_DISCOVER_RETIREMENT = 8
};

enum cfs_cleanup_tail_phase {
    CFS_CLEANUP_TAIL_NONE = 0,
    CFS_CLEANUP_TAIL_RETIRE_QUEUE = 1,
    CFS_CLEANUP_TAIL_FREE_TREE = 2,
    CFS_CLEANUP_TAIL_LEDGER = 3,
    CFS_CLEANUP_TAIL_SNAPSHOT = 4,
    CFS_CLEANUP_TAIL_ORPHAN = 5,
    CFS_CLEANUP_TAIL_INTENT = 6,
    CFS_CLEANUP_TAIL_FINAL = 7,
    CFS_CLEANUP_TAIL_DONE = 8
};

enum cfs_cleanup_service_class {
    CFS_CLEANUP_SERVICE_REVERT = 0,
    CFS_CLEANUP_SERVICE_INTENT = 1,
    CFS_CLEANUP_SERVICE_SNAPSHOT = 2,
    CFS_CLEANUP_SERVICE_ORPHAN = 3,
    CFS_CLEANUP_SERVICE_RETIREMENT = 4
};

/* Cleanup-ledger block header, absolute block offsets. */
#define CFS_CL_MAGIC UINT64_C(0x5247444c43736663)
#define CFS_CL_MAGIC_OFF 32
#define CFS_CL_SERIAL_OFF 40
#define CFS_CL_LOGICAL_ID_OFF 48
#define CFS_CL_LEVEL_OFF 56
#define CFS_CL_LANE_OFF 58
#define CFS_CL_FLAGS_OFF 60
#define CFS_CL_ENTRY_COUNT_OFF 64
#define CFS_CL_RESERVED_OFF 66
#define CFS_CL_RESERVED_SIZE 6
#define CFS_CL_SUBTREE_RECORDS_OFF 72
#define CFS_CL_SUBTREE_BLOCKS_OFF 80
#define CFS_CL_SUBTREE_BASE_OFF 88
#define CFS_CL_SUBTREE_RUNWAY_OFF 96
#define CFS_CL_SUBTREE_SURPLUS_RAW_OFF 104
#define CFS_CL_SUBTREE_SURPLUS_SEMANTIC_OFF 112
#define CFS_CL_MAX_INCARNATION_OFF 120
#define CFS_CL_SEMANTIC_DIGEST_OFF 128
#define CFS_CL_HEADER_RESERVED_OFF 136
#define CFS_CL_PAYLOAD_OFF 144
#define CFS_CL_ENTRY_SIZE 48
#define CFS_CL_ENTRY_MAX 64

_Static_assert(CFS_CL_PAYLOAD_OFF +
        CFS_CL_ENTRY_MAX * CFS_CL_ENTRY_SIZE <= CFS_BLOCK_SIZE,
    "cleanup-ledger payload fits one block");

/* Cleanup-ledger record offsets, relative to one 48-byte record. */
#define CFS_CLR_START_OFF 0
#define CFS_CLR_BLOCKS_OFF 8
#define CFS_CLR_DEATH_GENERATION_OFF 16
#define CFS_CLR_CLAIM_RESERVED_OFF 24
#define CFS_CLR_INCARNATION_GENERATION_OFF 32
#define CFS_CLR_CLASS_OFF 40
#define CFS_CLR_FLAGS_OFF 42
#define CFS_CLR_RESERVED_OFF 44
#define CFS_CLEANUP_LEDGER_RECORD_SIZE 48

enum cfs_cleanup_ledger_class {
    CFS_CLEANUP_CLASS_BASE = 1,
    CFS_CLEANUP_CLASS_RUNWAY = 2,
    CFS_CLEANUP_CLASS_SURPLUS_RAW = 3,
    CFS_CLEANUP_CLASS_SURPLUS_SEMANTIC = 4
};

/* Internal child reference offsets, relative to one 48-byte entry. */
#define CFS_CLC_LOGICAL_ID_OFF 0
#define CFS_CLC_PHYSICAL_ADDR_OFF 8
#define CFS_CLC_SERIAL_OFF 16
#define CFS_CLC_WHOLE_BLOCK_XSUM_OFF 24
#define CFS_CLC_SUBTREE_RECORDS_OFF 32
#define CFS_CLC_SUBTREE_BLOCKS_OFF 40
#define CFS_CLEANUP_LEDGER_CHILD_SIZE 48

/* 6. Tree node format (sections 6.3-6.5): node header is 16 bytes after
 * the common header; routing entries or slot directory begin at offset 48. */
#define CFS_N_LEVEL_OFF 32
#define CFS_N_LEVEL_SIZE 2
#define CFS_N_NITEMS_OFF 34
#define CFS_N_NITEMS_SIZE 2
#define CFS_N_TREE_ID_OFF 36
#define CFS_N_TREE_ID_SIZE 2
#define CFS_N_LOWER_OFF 38
#define CFS_N_LOWER_SIZE 2
#define CFS_N_UPPER_OFF 40
#define CFS_N_UPPER_SIZE 2
#define CFS_N_FLAGS_OFF 42
#define CFS_N_FLAGS_SIZE 2
#define CFS_N_RESERVED_OFF 44
#define CFS_N_RESERVED_SIZE 4
#define CFS_NODE_HEADER_SIZE 16 /* type-specific header, offsets 32..47 */
#define CFS_NODE_BODY_OFF 48    /* first routing entry / slot */

_Static_assert(CFS_N_LEVEL_OFF == CFS_BLOCK_HEADER_SIZE,
    "node header starts after the common header");
_Static_assert(
    CFS_N_RESERVED_OFF + CFS_N_RESERVED_SIZE == CFS_NODE_BODY_OFF,
    "node header tiles offsets 32..47");
_Static_assert(
    CFS_NODE_BODY_OFF == CFS_BLOCK_HEADER_SIZE + CFS_NODE_HEADER_SIZE,
    "node body begins at 48");

/* Interior routing entry (section 6.4), 41 bytes; local +0 offsets. */
#define CFS_E_KEY_OFF 0
#define CFS_E_CHILD_ADDR_OFF 17
#define CFS_E_CHILD_XSUM_OFF 25
#define CFS_E_CHILD_GEN_OFF 33
#define CFS_INTERIOR_ENTRY_SIZE 41

/* Leaf slot-directory entry (section 6.5), 4 bytes; local +0 offsets. */
#define CFS_LS_OFF_OFF 0
#define CFS_LS_OFF_SIZE 2
#define CFS_LS_LEN_OFF 2
#define CFS_LS_LEN_SIZE 2
#define CFS_LEAF_SLOT_SIZE 4

/* Fanout arithmetic (section 6.4) and leaf bounds (section 6.5). */
#define CFS_INTERIOR_MAX_ENTRIES 98
#define CFS_LEAF_USABLE (CFS_BLOCK_SIZE - CFS_NODE_BODY_OFF) /* 4048 */
#define CFS_LEAF_MAX_ITEM_SIZE (CFS_BLOCK_SIZE - CFS_NODE_BODY_OFF - CFS_LEAF_SLOT_SIZE)

_Static_assert(CFS_E_CHILD_ADDR_OFF == CFS_E_KEY_OFF + 17,
    "e_child_addr follows the 17-byte separator key");
_Static_assert(CFS_E_CHILD_GEN_OFF + 8 == CFS_INTERIOR_ENTRY_SIZE,
    "routing entry tiles 41 bytes");
_Static_assert(CFS_LS_LEN_OFF + CFS_LS_LEN_SIZE == CFS_LEAF_SLOT_SIZE,
    "leaf slot tiles 4 bytes");
_Static_assert(CFS_LEAF_USABLE == 4048, "leaf usable span is 4048 bytes");
_Static_assert(
    (CFS_BLOCK_SIZE - CFS_BLOCK_HEADER_SIZE - CFS_NODE_HEADER_SIZE) /
            CFS_INTERIOR_ENTRY_SIZE ==
        CFS_INTERIOR_MAX_ENTRIES,
    "interior fanout floor(4048/41) = 98");
_Static_assert(CFS_LEAF_MAX_ITEM_SIZE == 4044,
    "single-item leaf holds at most 4044 bytes");

/* 7. Key encoding (section 7): the one big-endian region. */
#define CFS_K_OBJID_OFF 0
#define CFS_K_OBJID_SIZE 8
#define CFS_K_TYPE_OFF 8
#define CFS_K_TYPE_SIZE 1
#define CFS_K_OFFSET_OFF 9
#define CFS_K_OFFSET_SIZE 8
#define CFS_ITEM_KEY_SIZE 17

_Static_assert(CFS_K_TYPE_OFF == CFS_K_OBJID_OFF + CFS_K_OBJID_SIZE,
    "k_type follows k_objid");
_Static_assert(CFS_K_OFFSET_OFF == CFS_K_TYPE_OFF + CFS_K_TYPE_SIZE,
    "k_offset follows k_type");
_Static_assert(CFS_K_OFFSET_OFF + CFS_K_OFFSET_SIZE == CFS_ITEM_KEY_SIZE,
    "key tiles 17 bytes");

/* Reserved inode numbers (section 7.3). */
#define CFS_INO_NONE UINT64_C(0)
#define CFS_INO_ROOT UINT64_C(1)
#define CFS_INO_ORPHAN UINT64_C(2)
#define CFS_INO_FIRST_USER UINT64_C(16)

/* Snapshot id sentinel (section 9.1). */
#define CFS_SNAP_NONE UINT64_C(0)

/* Refcount-tree-only internal objid for the retirement queue (8.8.2). */
#define CFS_CKPT_RETIRE_OBJID UINT64_MAX

/* Name-hash split for dirents/xattrs/snap names (section 8.3): the top
 * 56 bits are the bucket, the low 8 bits a disambiguator. */
#define CFS_NAME_HASH_DISAMBIG_MASK UINT64_C(0xFF)
#define CFS_NAME_HASH_BUCKET_MASK UINT64_C(0xFFFFFFFFFFFFFF00)
#define CFS_NAME_DISAMBIG_MAX 255
#define CFS_NAME_MAX 255 /* dirent/snap-name length ceiling */

/* 8. Item types and value layouts (sections 8, 9.2, 9.3). */
enum cfs_item_type {
    CFS_ITEM_NONE = 0,
    CFS_ITEM_INODE = 1,
    CFS_ITEM_DIRENT = 2,
    CFS_ITEM_INLINE_DATA = 3,
    CFS_ITEM_SYMLINK = 4,
    CFS_ITEM_XATTR = 5,
    CFS_ITEM_FILE_EXTENT = 6,
    CFS_ITEM_EXTENT_REFCOUNT = 7,
    CFS_ITEM_FREE_EXTENT = 8,
    CFS_ITEM_SNAP_DESC = 9,
    CFS_ITEM_SNAP_NAME = 10,
    CFS_ITEM_CKPT_RETIRE = 11
};

/* Inode value (section 8.2), 160 bytes; local +0 offsets. */
#define CFS_I_MODE_OFF 0
#define CFS_I_NLINK_OFF 4
#define CFS_I_UID_OFF 8
#define CFS_I_GID_OFF 12
#define CFS_I_SIZE_OFF 16
#define CFS_I_ALLOC_BYTES_OFF 24
#define CFS_I_ATIME_OFF 32
#define CFS_I_MTIME_OFF 40
#define CFS_I_CTIME_OFF 48
#define CFS_I_BTIME_OFF 56
#define CFS_I_GEN_OFF 64
#define CFS_I_FLAGS_OFF 72
#define CFS_I_PARENT_OFF 80
#define CFS_I_RDEV_OFF 88
#define CFS_I_POLICY_LABEL_OFF 96
#define CFS_I_POLICY_LABEL_SIZE 16
#define CFS_I_CRYPTO_OFF 112
#define CFS_I_CRYPTO_SIZE 32
#define CFS_I_CLEANUP_EPOCH_OFF 144
#define CFS_I_CLEANUP_REMAINING_DEBIT_OFF 152
#define CFS_I_RESERVED_OFF 144
#define CFS_I_RESERVED_SIZE 16
#define CFS_INODE_VALUE_SIZE 160

_Static_assert(CFS_I_RDEV_OFF + 8 == CFS_I_POLICY_LABEL_OFF,
    "i_policy_label follows i_rdev");
_Static_assert(CFS_I_CLEANUP_REMAINING_DEBIT_OFF + 8 ==
                   CFS_INODE_VALUE_SIZE,
    "Profile 1 inode cleanup tail tiles bytes 144..159");
_Static_assert(CFS_I_RESERVED_OFF + CFS_I_RESERVED_SIZE == CFS_INODE_VALUE_SIZE,
    "inode value tiles 160 bytes");

/* Inode flags (section 8.2). */
#define CFS_IFLAG_INLINE_DATA UINT64_C(0x1)

/* Format-owned inode mode type bits. Core consumers use these constants
 * instead of importing a host's sys/stat.h vocabulary. */
#define CFS_MODE_TYPE_MASK UINT32_C(0170000)
#define CFS_MODE_FIFO UINT32_C(0010000)
#define CFS_MODE_CHARACTER UINT32_C(0020000)
#define CFS_MODE_DIRECTORY UINT32_C(0040000)
#define CFS_MODE_BLOCK UINT32_C(0060000)
#define CFS_MODE_REGULAR UINT32_C(0100000)
#define CFS_MODE_SYMLINK UINT32_C(0120000)
#define CFS_MODE_SOCKET UINT32_C(0140000)

/* Dirent value (section 8.3), 12 + d_name_len bytes; local +0 offsets. */
#define CFS_D_CHILD_INO_OFF 0
#define CFS_D_TYPE_OFF 8
#define CFS_D_NAME_LEN_OFF 9
#define CFS_D_RESERVED_OFF 10
#define CFS_D_RESERVED_SIZE 2
#define CFS_D_NAME_OFF 12
#define CFS_DIRENT_VALUE_HEADER_SIZE 12

_Static_assert(CFS_D_NAME_OFF == CFS_D_RESERVED_OFF + CFS_D_RESERVED_SIZE,
    "dirent name follows the 12-byte header");

/* DT_* file-type hints stored in d_type (section 8.3). */
enum cfs_dt_type {
    CFS_DT_UNKNOWN = 0,
    CFS_DT_FIFO = 1,
    CFS_DT_CHR = 2,
    CFS_DT_DIR = 4,
    CFS_DT_BLK = 6,
    CFS_DT_REG = 8,
    CFS_DT_LNK = 10,
    CFS_DT_SOCK = 12
};

/* Inline-data and symlink caps (sections 8.4, 8.5). */
#define CFS_INLINE_DATA_MAX 2048
#define CFS_SYMLINK_MAX 1023

/* Xattr value (section 8.6, reserved shape); local +0 offsets. */
#define CFS_X_NAME_LEN_OFF 0
#define CFS_X_VALUE_LEN_OFF 2
#define CFS_X_FLAGS_OFF 4
#define CFS_X_FLAGS_SIZE 4
#define CFS_X_NAME_OFF 8
#define CFS_XATTR_VALUE_HEADER_SIZE 8
#define CFS_XATTR_VALUE_MAX 1024 /* in-memory cap; v0 emits no xattrs */

_Static_assert(CFS_X_NAME_OFF == CFS_X_FLAGS_OFF + CFS_X_FLAGS_SIZE,
    "xattr name follows the 8-byte header");

/* File-extent value (section 8.7), 24 bytes; local +0 offsets. */
#define CFS_FE_PHYS_START_OFF 0
#define CFS_FE_BLOCKS_OFF 8
#define CFS_FE_FLAGS_OFF 16
#define CFS_FE_FLAGS_SIZE 4
#define CFS_FE_RESERVED_OFF 20
#define CFS_FE_RESERVED_SIZE 4
#define CFS_FILE_EXTENT_VALUE_SIZE 24

_Static_assert(CFS_FE_RESERVED_OFF + CFS_FE_RESERVED_SIZE ==
                   CFS_FILE_EXTENT_VALUE_SIZE,
    "file-extent value tiles 24 bytes");

/* File-extent flags (section 8.7). */
#define CFS_FE_PREALLOC UINT32_C(0x1)

/* Extent-refcount value (section 8.8), 16 bytes; local +0 offsets. */
#define CFS_RC_BLOCKS_OFF 0
#define CFS_RC_COUNT_OFF 8
#define CFS_RC_COUNT_SIZE 4
#define CFS_RC_FLAGS_OFF 12
#define CFS_RC_FLAGS_SIZE 4
#define CFS_REFCOUNT_VALUE_SIZE 16

_Static_assert(CFS_RC_FLAGS_OFF + CFS_RC_FLAGS_SIZE == CFS_REFCOUNT_VALUE_SIZE,
    "refcount value tiles 16 bytes");

/* Checkpoint-retirement value (section 8.8.2), 24 bytes; local +0 offsets. */
#define CFS_QR_DEATH_GENERATION_OFF 0
#define CFS_QR_BLOCKS_OFF 8
#define CFS_QR_FLAGS_OFF 16
#define CFS_QR_FLAGS_SIZE 4
#define CFS_QR_RESERVED_OFF 20
#define CFS_QR_RESERVED_SIZE 4
#define CFS_QR_COHORT_EPOCH_OFF 16
#define CFS_CKPT_RETIRE_VALUE_SIZE 24

_Static_assert(CFS_QR_RESERVED_OFF + CFS_QR_RESERVED_SIZE ==
                   CFS_CKPT_RETIRE_VALUE_SIZE,
    "checkpoint-retirement value tiles 24 bytes");
_Static_assert(CFS_QR_COHORT_EPOCH_OFF + 8 ==
                   CFS_CKPT_RETIRE_VALUE_SIZE,
    "Profile 1 retirement epoch tiles bytes 16..23");

/* Free-extent value (section 8.9), 8 bytes; local +0 offsets. */
#define CFS_FF_BLOCKS_OFF 0
#define CFS_FREE_EXTENT_VALUE_SIZE 8

/* 9. Snapshot records (sections 9.2, 9.3): descriptor value is 256
 * bytes, local +0 offsets. */
#define CFS_SD_CREATE_GEN_OFF 0
#define CFS_SD_CREATE_TIME_OFF 8
#define CFS_SD_FLAGS_OFF 16
#define CFS_SD_NAME_HASH_OFF 24
#define CFS_SD_REAP_PHASE_OFF 32
#define CFS_SD_REAP_PHASE_SIZE 2
#define CFS_SD_REAP_LEVEL_OFF 34
#define CFS_SD_REAP_LEVEL_SIZE 2
#define CFS_SD_RESERVED0_OFF 36
#define CFS_SD_RESERVED0_SIZE 4
#define CFS_SD_REAP_KEY_OFF 40
#define CFS_SD_REAP_KEY_SIZE 17
#define CFS_SD_RESERVED1_OFF 57
#define CFS_SD_RESERVED1_SIZE 7
#define CFS_SD_ROOTS_OFF 64
#define CFS_SD_ROOTS_SIZE 128
#define CFS_SD_CLEANUP_EPOCH_OFF 192
#define CFS_SD_CLEANUP_REMAINING_DEBIT_OFF 200
#define CFS_SD_CLEANUP_ORIGINAL_DEBIT_OFF 208
#define CFS_SD_CLEANUP_UNITS_REMAINING_OFF 216
#define CFS_SD_CLEANUP_PROFILE_OFF 224
#define CFS_SD_CLEANUP_FLAGS_OFF 226
#define CFS_SD_CLEANUP_RESERVED_OFF 228
#define CFS_SD_CLEANUP_RESERVED_SIZE 4
#define CFS_SD_CLEANUP_RESERVED2_OFF 232
#define CFS_SD_CLEANUP_RESERVED2_SIZE 16
#define CFS_SD_CLEANUP_FINGERPRINT_OFF 248
#define CFS_SD_RESERVED2_OFF 192
#define CFS_SD_RESERVED2_SIZE 64
#define CFS_SNAP_DESC_VALUE_SIZE 256
#define CFS_SNAP_ROOT_COUNT 4

_Static_assert(CFS_SD_REAP_KEY_OFF == CFS_SD_RESERVED0_OFF + CFS_SD_RESERVED0_SIZE,
    "sd_reap_key follows sd_reserved0");
_Static_assert(CFS_SD_ROOTS_OFF == CFS_SD_RESERVED1_OFF + CFS_SD_RESERVED1_SIZE,
    "sd_roots follows sd_reserved1");
_Static_assert(CFS_SD_ROOTS_SIZE == CFS_SNAP_ROOT_COUNT * CFS_ROOT_RECORD_SIZE,
    "sd_roots holds 4 root records");
_Static_assert(CFS_SD_CLEANUP_FINGERPRINT_OFF + 8 ==
                   CFS_SNAP_DESC_VALUE_SIZE,
    "Profile 1 snapshot cleanup tail tiles bytes 192..255");
_Static_assert(CFS_SD_RESERVED2_OFF + CFS_SD_RESERVED2_SIZE ==
                   CFS_SNAP_DESC_VALUE_SIZE,
    "snapshot descriptor tiles 256 bytes");

/* Snapshot flags (section 9.2). */
#define CFS_SNAP_DELETING UINT64_C(0x1)
#define CFS_SD_CLEANUP_REVERT_TARGET UINT16_C(0x1)

/* Reaper phases (section 9.2). */
enum cfs_reap_phase {
    CFS_REAP_NONE = 0,
    CFS_REAP_NAMESPACE = 1,
    CFS_REAP_EXTENT = 2,
    CFS_REAP_FINALIZE = 3
};

/* Snapshot name value (section 9.3), 12 + sn_name_len bytes; local +0. */
#define CFS_SN_SNAP_ID_OFF 0
#define CFS_SN_NAME_LEN_OFF 8
#define CFS_SN_RESERVED_OFF 9
#define CFS_SN_RESERVED_SIZE 3
#define CFS_SN_NAME_OFF 12
#define CFS_SNAP_NAME_VALUE_HEADER_SIZE 12

_Static_assert(CFS_SN_NAME_OFF == CFS_SN_RESERVED_OFF + CFS_SN_RESERVED_SIZE,
    "snapshot name follows the 12-byte header");

/* The intent-log and allocator-summary byte layouts. */
#include "format_journal.h"

/* The in-memory record types and the encode/decode surface that reads and
 * writes the byte offsets above. */
#include "format_records.h"

#endif /* CFS_FORMAT_H */
