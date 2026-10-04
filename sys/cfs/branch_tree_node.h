/* SPDX-License-Identifier: ISC
 * branch_tree_node.h -- canonical ordered-node framing for branch graphs.
 */
#ifndef CFS_BRANCH_TREE_NODE_H
#define CFS_BRANCH_TREE_NODE_H

#include "branch_store.h"

#define CFS_BT_MAX_RECORDS 96u
#define CFS_BT_MAX_KEY 272u
#define CFS_BT_MAX_VALUE 256u
#define CFS_BT_MAX_LEVEL 15u
#define CFS_BT_NO_CHILD UINT16_C(65535)
#define CFS_BT_HEADER_SIZE 32u
#define CFS_BT_SLOT_SIZE 8u
#define CFS_BT_MIN_RECORDS 48u
#define CFS_BT_MIN_ENTRY_BYTES 1600u
#define CFS_BT_PAYLOAD_MAX 4000u

typedef struct cfs_bt_record {
    const uint8_t *key;
    const uint8_t *value;
    uint16_t key_length;
    uint16_t value_length;
    uint16_t child_index;
} cfs_bt_record_t;

/* A decoded view borrows immutable payload bytes through its last use.
 * Offsets make record access O(1), not a repeated scan of packed values. */
typedef struct cfs_bt_node_view {
    const uint8_t *payload;
    uint64_t item_count;
    uint32_t payload_length;
    uint32_t entry_bytes;
    uint16_t level;
    uint16_t count;
    uint16_t child_count;
    uint16_t data_offset[CFS_BT_MAX_RECORDS];
} cfs_bt_node_view_t;

/* Keys have at most MAX_KEY unsigned bytes; zero length permits NULL. */
int cfs_bt_key_compare(const void *left, uint16_t left_length,
    const void *right, uint16_t right_length);

/* The outer OBJECT must already be authenticated by the branch store.
 * root is 0 or 1. All outputs remain unchanged on refusal. */
int cfs_bt_node_decode(const void *payload, uint32_t payload_length,
    uint32_t child_count, int root, cfs_bt_node_view_t *out);

/* Outputs are mutually disjoint and do not overlap borrowed input storage.
 * Invalid shapes return EINVAL; a short buffer is EWORKSPACE. */
int cfs_bt_node_frame(uint16_t level, const cfs_bt_record_t *records,
    uint16_t count, int root, void *payload, uint32_t capacity,
    uint32_t *out_length, uint32_t *out_child_count);

/* Accessors require a successfully decoded, still-live view. */
int cfs_bt_node_record(const cfs_bt_node_view_t *view, uint16_t index,
    cfs_bt_record_t *out);
int cfs_bt_node_check_child(const cfs_bt_node_view_t *parent, uint16_t index,
    const cfs_bt_node_view_t *child);

/* Zero bound length means unbounded. Bounds are lower-inclusive and
 * upper-exclusive; inherited bounds must accompany each tree descent. */
int cfs_bt_node_check_range(const cfs_bt_node_view_t *view,
    const void *lower, uint16_t lower_length,
    const void *upper, uint16_t upper_length);

#endif /* CFS_BRANCH_TREE_NODE_H */
