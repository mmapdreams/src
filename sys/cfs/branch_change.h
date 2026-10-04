/* SPDX-License-Identifier: ISC
 * branch_change.h -- bounded incremental logical change-stream decoding.
 */
#ifndef CFS_BRANCH_CHANGE_H
#define CFS_BRANCH_CHANGE_H

#include "branch_diff.h"

#define CFS_BF_CHANGE_LINE_MAX 9000u

typedef struct cfs_bf_change_header {
    uint8_t uuid[CFS_BS_UUID_SIZE];
    uint64_t base;
    uint64_t result;
    cfs_bf_id_t root_before;
    cfs_bf_id_t root_after;
} cfs_bf_change_header_t;

typedef struct cfs_bf_change_receipt {
    cfs_bf_change_header_t header;
    uint64_t records;
    uint64_t hash;
} cfs_bf_change_receipt_t;

/* A null change delivers the header, and borrowed data expires at return. Zero
 * continues; any other result aborts with EINVAL. Never reenter the decoder. */
typedef int (*cfs_bf_change_visit_fn)(void *context,
    const cfs_bf_change_header_t *header, const cfs_bf_change_t *change);

typedef struct cfs_bf_change_group {
    cfs_bf_id_t id;
    uint64_t size[2];
    uint64_t added;
    uint64_t removed;
    uint64_t data_end;
    uint32_t kind;
    uint16_t type;
    uint16_t name_length;
    uint8_t inode_seen;
    uint8_t present[2];
    uint8_t symlink_seen;
    uint8_t name[CFS_BF_NAME_MAX];
} cfs_bf_change_group_t;

/* Caller-owned scratch, disjoint from feed inputs and finish output, and
 * initialized by start. No heap, storage provider, or recursion is used. */
typedef struct cfs_bf_change_decoder {
    cfs_bf_change_receipt_t receipt;
    cfs_bf_change_group_t group;
    cfs_bf_change_visit_fn visit;
    void *context;
    uint64_t bytes_consumed;
    uint64_t lines_decoded;
    uint64_t hash_bytes;
    uint32_t state;
    uint32_t used;
    uint32_t active;
    int status;
    cfs_bf_change_t change;
    cfs_bf_inode_t inode[2];
    uint8_t metadata[2][CFS_BF_INODE_BYTES];
    uint8_t bytes[CFS_BF_SYMLINK_MAX];
    uint8_t line[8u + CFS_BF_CHANGE_LINE_MAX];
} cfs_bf_change_decoder_t;

int cfs_bf_change_start(cfs_bf_change_decoder_t *decoder,
    cfs_bf_change_visit_fn visit, void *context);

/* Arbitrary chunks, including empty ones, are allowed before finish. Failures
 * are sticky, and delivered events never authorize visible publication. */
int cfs_bf_change_feed(cfs_bf_change_decoder_t *decoder,
    const void *bytes, uint32_t length);

/* Call only after observed EOF. Output changes only on success. This proves
 * framing and local consistency, not base equivalence or a valid filesystem. */
int cfs_bf_change_finish(cfs_bf_change_decoder_t *decoder,
    cfs_bf_change_receipt_t *out);

#endif /* CFS_BRANCH_CHANGE_H */
