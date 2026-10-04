/* SPDX-License-Identifier: ISC
 *
 * format_journal.h -- the intent log and allocator summary byte layouts
 * (spec sections 10.2, 10.3, and 11.2).
 *
 * Included from format.h, which owns the block header and geometry these
 * records sit inside. Split out so no single format header exceeds the
 * repository's source-length limit; the constants remain normative in
 * txt/format.md.
 */
#ifndef CFS_FORMAT_JOURNAL_H
#define CFS_FORMAT_JOURNAL_H

#include <stddef.h>
#include <stdint.h>

/* 10. Intent log (sections 10.2, 10.3): block header is 16 bytes after
 * the common header; framed records begin at offset 48. */
#define CFS_L_SEQ_OFF 32
#define CFS_L_SEQ_SIZE 8
#define CFS_L_NRECS_OFF 40
#define CFS_L_NRECS_SIZE 2
#define CFS_L_BYTES_OFF 42
#define CFS_L_BYTES_SIZE 2
#define CFS_L_RESERVED_OFF 44
#define CFS_L_RESERVED_SIZE 4
#define CFS_INTENT_BODY_OFF 48
#define CFS_INTENT_MAX_FRAMING (CFS_BLOCK_SIZE - CFS_INTENT_BODY_OFF) /* 4048 */

_Static_assert(CFS_L_SEQ_OFF == CFS_BLOCK_HEADER_SIZE,
    "intent block header follows the common header");
_Static_assert(CFS_L_RESERVED_OFF + CFS_L_RESERVED_SIZE == CFS_INTENT_BODY_OFF,
    "intent block header tiles offsets 32..47");

/* Record framing (section 10.3), 12-byte header; local +0 offsets. */
#define CFS_RF_TYPE_OFF 0
#define CFS_RF_TYPE_SIZE 2
#define CFS_RF_LEN_OFF 2
#define CFS_RF_LEN_SIZE 2
#define CFS_RF_XSUM_OFF 4
#define CFS_RF_XSUM_SIZE 8
#define CFS_RF_BODY_OFF 12
#define CFS_INTENT_REC_FRAME_SIZE 12

_Static_assert(CFS_RF_BODY_OFF == CFS_RF_XSUM_OFF + CFS_RF_XSUM_SIZE,
    "record body follows the 12-byte framing header");

/* Intent record types (section 10.3). */
enum cfs_intent_rec_type {
    CFS_INTENT_INODE = 1,
    CFS_INTENT_EXTENT = 2,
    CFS_INTENT_DIRENT = 3,
    CFS_INTENT_COMMIT = 4,
    CFS_INTENT_CLEANUP_CREDIT = 5
};

/* CFS_INTENT_INODE body (section 10.3): 8-byte inode number + the
 * 160-byte inode value. */
#define CFS_II_INO_OFF 0
#define CFS_II_INODE_OFF 8
#define CFS_INTENT_INODE_BODY_SIZE (8 + CFS_INODE_VALUE_SIZE) /* 168 */

/* CFS_INTENT_EXTENT body (section 10.3), 40 bytes; local +0 offsets. */
#define CFS_IE_INO_OFF 0
#define CFS_IE_FILE_OFFSET_OFF 8
#define CFS_IE_PHYS_START_OFF 16
#define CFS_IE_BLOCKS_OFF 24
#define CFS_IE_FLAGS_OFF 32
#define CFS_IE_FLAGS_SIZE 4
#define CFS_IE_RESERVED_OFF 36
#define CFS_IE_RESERVED_SIZE 4
#define CFS_INTENT_EXTENT_BODY_SIZE 40

_Static_assert(CFS_IE_RESERVED_OFF + CFS_IE_RESERVED_SIZE ==
                   CFS_INTENT_EXTENT_BODY_SIZE,
    "intent-extent body tiles 40 bytes");

/* CFS_INTENT_DIRENT body (section 10.3), 20 + name_len bytes. */
#define CFS_ID_DIR_INO_OFF 0
#define CFS_ID_CHILD_INO_OFF 8
#define CFS_ID_DTYPE_OFF 16
#define CFS_ID_OP_OFF 17
#define CFS_ID_NAME_LEN_OFF 18
#define CFS_ID_RESERVED_OFF 19
#define CFS_ID_RESERVED_SIZE 1
#define CFS_ID_NAME_OFF 20
#define CFS_INTENT_DIRENT_BODY_HEADER_SIZE 20

_Static_assert(CFS_ID_NAME_OFF == CFS_ID_RESERVED_OFF + CFS_ID_RESERVED_SIZE,
    "intent-dirent name follows the 20-byte header");

/* Dirent op codes carried in the intent-dirent body (section 10.3). */
#define CFS_DIRENT_OP_ADD 0
#define CFS_DIRENT_OP_REMOVE 1

/* CFS_INTENT_COMMIT body (section 10.3), 16 bytes; local +0 offsets. */
#define CFS_IC_BASE_GEN_OFF 0
#define CFS_IC_REC_COUNT_OFF 8
#define CFS_IC_REC_COUNT_SIZE 2
#define CFS_IC_RESERVED_OFF 10
#define CFS_IC_RESERVED_SIZE 6
#define CFS_INTENT_COMMIT_BODY_SIZE 16

_Static_assert(CFS_IC_RESERVED_OFF + CFS_IC_RESERVED_SIZE ==
                   CFS_INTENT_COMMIT_BODY_SIZE,
    "intent-commit body tiles 16 bytes");

/* CFS_INTENT_CLEANUP_CREDIT body (section 10.3), 48 bytes. */
#define CFS_ICC_CLEANUP_EPOCH_OFF 0
#define CFS_ICC_CLEANUP_PROFILE_OFF 8
#define CFS_ICC_FLAGS_OFF 12
#define CFS_ICC_FRESH_CLAIM_OFF 16
#define CFS_ICC_RAW_RECORD_CLAIM_OFF 24
#define CFS_ICC_DEBIT_CLAIM_OFF 32
#define CFS_ICC_PAYLOAD_CLAIM_OFF 40
#define CFS_INTENT_CLEANUP_CREDIT_BODY_SIZE 48

_Static_assert(CFS_ICC_PAYLOAD_CLAIM_OFF + 8 ==
                   CFS_INTENT_CLEANUP_CREDIT_BODY_SIZE,
    "intent cleanup-credit body tiles 48 bytes");

/* 11. Allocator summary (section 11.2): block header is 16 bytes after
 * the common header; segment records begin at offset 48. */
#define CFS_U_FIRST_BLOCK_OFF 32
#define CFS_U_FIRST_BLOCK_SIZE 8
#define CFS_U_NRECS_OFF 40
#define CFS_U_NRECS_SIZE 2
#define CFS_U_SEG_LOG2_OFF 42
#define CFS_U_SEG_LOG2_SIZE 2
#define CFS_U_RESERVED_OFF 44
#define CFS_U_RESERVED_SIZE 4
#define CFS_SUMMARY_BODY_OFF 48

_Static_assert(CFS_U_FIRST_BLOCK_OFF == CFS_BLOCK_HEADER_SIZE,
    "summary block header follows the common header");
_Static_assert(CFS_U_RESERVED_OFF + CFS_U_RESERVED_SIZE == CFS_SUMMARY_BODY_OFF,
    "summary block header tiles offsets 32..47");

/* Segment record (section 11.2), 16 bytes; local +0 offsets. */
#define CFS_SM_FREE_BLOCKS_OFF 0
#define CFS_SM_LARGEST_RUN_OFF 4
#define CFS_SM_CLASS_OFF 8
#define CFS_SM_CLASS_SIZE 2
#define CFS_SM_FLAGS_OFF 10
#define CFS_SM_FLAGS_SIZE 2
#define CFS_SM_RESERVED_OFF 12
#define CFS_SM_RESERVED_SIZE 4
#define CFS_SUMMARY_REC_SIZE 16
#define CFS_SUMMARY_MAX_RECS 253

_Static_assert(CFS_SM_RESERVED_OFF + CFS_SM_RESERVED_SIZE ==
                   CFS_SUMMARY_REC_SIZE,
    "segment record tiles 16 bytes");
_Static_assert(
    (CFS_BLOCK_SIZE - CFS_SUMMARY_BODY_OFF) / CFS_SUMMARY_REC_SIZE ==
        CFS_SUMMARY_MAX_RECS,
    "up to 253 segment records per summary block");
_Static_assert(CFS_SUMMARY_MAX_RECS * CFS_SUMMARY_SEGMENT ==
                   CFS_SUMMARY_COVERAGE,
    "one summary block covers 518144 image blocks");

/* Segment clustering-class hints (section 11.2). */
enum cfs_summary_class {
    CFS_SM_CLASS_UNSET = 0,
    CFS_SM_CLASS_META = 1,
    CFS_SM_CLASS_DATA = 2
};

#endif /* CFS_FORMAT_JOURNAL_H */
