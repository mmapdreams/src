/* SPDX-License-Identifier: ISC
 * branch_tree_node.c -- pure ordered-node codec and parent-context checks.
 */
#include "branch_tree_node.h"

#include <string.h>

#include "codec.h"

#define NODE_MAGIC UINT64_C(0x3130305442534643)
#define OBJECT_REF_BYTES 24u

int cfs_bt_key_compare(const void *left, uint16_t left_length,
    const void *right, uint16_t right_length) {
    const uint8_t *a = left;
    const uint8_t *b = right;
    uint16_t length = left_length < right_length ? left_length : right_length;
    for (uint16_t i = 0; i < length; i++) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }

    return left_length == right_length ? 0 : left_length < right_length ? -1 : 1;
}

static int occupancy_valid(uint16_t level, uint16_t count,
    uint32_t entry_bytes, int root) {
    if (root) {
        return level == 0u || count >= 2u;
    }

    return count >= CFS_BT_MIN_RECORDS || entry_bytes >= CFS_BT_MIN_ENTRY_BYTES;
}

int cfs_bt_node_record(const cfs_bt_node_view_t *view, uint16_t index,
    cfs_bt_record_t *out) {
    cfs_bt_record_t record;
    const uint8_t *slot;
    if (view == NULL || out == NULL || index >= view->count) {
        return CFS_BS_EINVAL;
    }

    slot = view->payload + CFS_BT_HEADER_SIZE + index * CFS_BT_SLOT_SIZE;
    record.key_length = cfs_get16(slot);
    record.value_length = cfs_get16(slot + 2u);
    record.child_index = cfs_get16(slot + 4u);
    record.key = view->payload + view->data_offset[index];
    record.value = record.key + record.key_length;
    *out = record;
    return CFS_BS_OK;
}

int cfs_bt_node_decode(const void *payload, uint32_t payload_length,
    uint32_t child_count, int root, cfs_bt_node_view_t *out) {
    const uint8_t *bytes = payload;
    cfs_bt_node_view_t view;
    uint32_t offset;
    uint16_t owned = 0u;
    if (payload == NULL || out == NULL || (root != 0 && root != 1)) {
        return CFS_BS_EINVAL;
    }

    if (payload_length < CFS_BT_HEADER_SIZE ||
        payload_length > CFS_BT_PAYLOAD_MAX ||
        child_count > CFS_BT_MAX_RECORDS ||
        payload_length + child_count * OBJECT_REF_BYTES > CFS_BT_PAYLOAD_MAX ||
        cfs_get64(bytes) != NODE_MAGIC || cfs_get32(bytes + 12u) != 0u ||
        cfs_get64(bytes + 24u) != 0u) {
        return CFS_BS_ECORRUPT;
    }

    memset(&view, 0, sizeof(view));
    view.payload = bytes;
    view.payload_length = payload_length;
    view.child_count = (uint16_t)child_count;
    view.level = cfs_get16(bytes + 8u);
    view.count = cfs_get16(bytes + 10u);
    view.entry_bytes = payload_length - CFS_BT_HEADER_SIZE +
        child_count * OBJECT_REF_BYTES;
    offset = CFS_BT_HEADER_SIZE + view.count * CFS_BT_SLOT_SIZE;
    if (view.level > CFS_BT_MAX_LEVEL || view.count == 0u ||
        view.count > CFS_BT_MAX_RECORDS || offset > payload_length ||
        !occupancy_valid(view.level, view.count, view.entry_bytes, root)) {
        return CFS_BS_ECORRUPT;
    }

    for (uint16_t i = 0; i < view.count; i++) {
        const uint8_t *slot = bytes + CFS_BT_HEADER_SIZE + i * CFS_BT_SLOT_SIZE;
        uint16_t key_length = cfs_get16(slot);
        uint16_t value_length = cfs_get16(slot + 2u);
        uint16_t child_index = cfs_get16(slot + 4u);
        uint64_t items = 1u;
        if (key_length == 0u || key_length > CFS_BT_MAX_KEY ||
            value_length > CFS_BT_MAX_VALUE || cfs_get16(slot + 6u) != 0u ||
            (view.level != 0u && value_length != 8u) ||
            offset + key_length + value_length > payload_length) {
            return CFS_BS_ECORRUPT;
        }

        if (child_index == CFS_BT_NO_CHILD) {
            if (view.level != 0u) {
                return CFS_BS_ECORRUPT;
            }
        } else if (child_index != owned || owned >= child_count) {
            return CFS_BS_ECORRUPT;
        } else {
            owned++;
        }

        view.data_offset[i] = (uint16_t)offset;
        if (i != 0u) {
            cfs_bt_record_t previous;
            (void)cfs_bt_node_record(&view, i - 1u, &previous);
            if (cfs_bt_key_compare(previous.key, previous.key_length,
                    bytes + offset, key_length) >= 0) {
                return CFS_BS_ECORRUPT;
            }
        }

        if (view.level != 0u) {
            items = cfs_get64(bytes + offset + key_length);
        }

        if (items == 0u || view.item_count > UINT64_MAX - items) {
            return CFS_BS_ECORRUPT;
        }

        view.item_count += items;
        offset += key_length + value_length;
    }

    if (offset != payload_length || owned != child_count ||
        view.item_count != cfs_get64(bytes + 16u)) {
        return CFS_BS_ECORRUPT;
    }

    *out = view;
    return CFS_BS_OK;
}

static int frame_shape(uint16_t level, const cfs_bt_record_t *records,
    uint16_t count, int root, uint32_t *out_length, uint32_t *out_children,
    uint64_t *out_items) {
    uint32_t length = CFS_BT_HEADER_SIZE + count * CFS_BT_SLOT_SIZE;
    uint32_t owned = 0u;
    uint64_t items = 0u;
    if (level > CFS_BT_MAX_LEVEL || records == NULL || count == 0u ||
        count > CFS_BT_MAX_RECORDS || (root != 0 && root != 1)) {
        return CFS_BS_EINVAL;
    }

    for (uint16_t i = 0; i < count; i++) {
        const cfs_bt_record_t *record = &records[i];
        uint64_t increment = 1u;
        if (record->key == NULL || record->key_length == 0u ||
            record->key_length > CFS_BT_MAX_KEY ||
            record->value_length > CFS_BT_MAX_VALUE ||
            (record->value_length != 0u && record->value == NULL) ||
            (level != 0u && record->value_length != 8u)) {
            return CFS_BS_EINVAL;
        }

        if (i != 0u && cfs_bt_key_compare(records[i - 1u].key,
                records[i - 1u].key_length, record->key,
                record->key_length) >= 0) {
            return CFS_BS_EINVAL;
        }

        if (record->child_index == CFS_BT_NO_CHILD) {
            if (level != 0u) {
                return CFS_BS_EINVAL;
            }
        } else if (record->child_index != owned) {
            return CFS_BS_EINVAL;
        } else {
            owned++;
        }

        length += record->key_length + record->value_length;
        if (level != 0u) {
            increment = cfs_get64(record->value);
        }

        if (increment == 0u || items > UINT64_MAX - increment) {
            return CFS_BS_EINVAL;
        }

        items += increment;
    }

    if (length + owned * OBJECT_REF_BYTES > CFS_BT_PAYLOAD_MAX ||
        !occupancy_valid(level, count,
            length - CFS_BT_HEADER_SIZE + owned * OBJECT_REF_BYTES, root)) {
        return CFS_BS_EINVAL;
    }

    *out_length = length;
    *out_children = owned;
    *out_items = items;
    return CFS_BS_OK;
}

int cfs_bt_node_frame(uint16_t level, const cfs_bt_record_t *records,
    uint16_t count, int root, void *payload, uint32_t capacity,
    uint32_t *out_length, uint32_t *out_child_count) {
    uint8_t *bytes = payload;
    uint32_t length;
    uint32_t children;
    uint64_t items;
    uint32_t offset = CFS_BT_HEADER_SIZE + count * CFS_BT_SLOT_SIZE;
    int rc;
    if (payload == NULL || out_length == NULL || out_child_count == NULL) {
        return CFS_BS_EINVAL;
    }

    rc = frame_shape(level, records, count, root, &length, &children, &items);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (capacity < length) {
        return CFS_BS_EWORKSPACE;
    }

    memset(bytes, 0, length);
    cfs_put64(bytes, NODE_MAGIC);
    cfs_put16(bytes + 8u, level);
    cfs_put16(bytes + 10u, count);
    cfs_put64(bytes + 16u, items);
    for (uint16_t i = 0; i < count; i++) {
        const cfs_bt_record_t *record = &records[i];
        uint8_t *slot = bytes + CFS_BT_HEADER_SIZE + i * CFS_BT_SLOT_SIZE;
        cfs_put16(slot, record->key_length);
        cfs_put16(slot + 2u, record->value_length);
        cfs_put16(slot + 4u, record->child_index);
        memcpy(bytes + offset, record->key, record->key_length);
        offset += record->key_length;
        if (record->value_length != 0u) {
            memcpy(bytes + offset, record->value, record->value_length);
        }

        offset += record->value_length;
    }

    *out_length = length;
    *out_child_count = children;
    return CFS_BS_OK;
}

int cfs_bt_node_check_range(const cfs_bt_node_view_t *view,
    const void *lower, uint16_t lower_length,
    const void *upper, uint16_t upper_length) {
    cfs_bt_record_t first;
    cfs_bt_record_t last;
    if (view == NULL || lower_length > CFS_BT_MAX_KEY ||
        upper_length > CFS_BT_MAX_KEY ||
        (lower_length != 0u && lower == NULL) ||
        (upper_length != 0u && upper == NULL) ||
        (lower_length != 0u && upper_length != 0u &&
            cfs_bt_key_compare(lower, lower_length, upper, upper_length) >= 0)) {
        return CFS_BS_EINVAL;
    }

    (void)cfs_bt_node_record(view, 0u, &first);
    (void)cfs_bt_node_record(view, view->count - 1u, &last);
    if ((lower_length != 0u && cfs_bt_key_compare(first.key,
            first.key_length, lower, lower_length) < 0) ||
        (upper_length != 0u && cfs_bt_key_compare(last.key,
            last.key_length, upper, upper_length) >= 0)) {
        return CFS_BS_ECORRUPT;
    }

    return CFS_BS_OK;
}

int cfs_bt_node_check_child(const cfs_bt_node_view_t *parent, uint16_t index,
    const cfs_bt_node_view_t *child) {
    cfs_bt_record_t edge;
    cfs_bt_record_t first;
    cfs_bt_record_t next;
    const void *upper = NULL;
    uint16_t upper_length = 0u;
    if (parent == NULL || child == NULL || parent->level == 0u ||
        index >= parent->count) {
        return CFS_BS_EINVAL;
    }

    (void)cfs_bt_node_record(parent, index, &edge);
    (void)cfs_bt_node_record(child, 0u, &first);
    if (child->level + 1u != parent->level ||
        !occupancy_valid(child->level, child->count, child->entry_bytes, 0) ||
        cfs_bt_key_compare(edge.key, edge.key_length,
            first.key, first.key_length) != 0 ||
        child->item_count != cfs_get64(edge.value)) {
        return CFS_BS_ECORRUPT;
    }

    if (index + 1u < parent->count) {
        (void)cfs_bt_node_record(parent, index + 1u, &next);
        upper = next.key;
        upper_length = next.key_length;
    }

    return cfs_bt_node_check_range(child, edge.key, edge.key_length,
        upper, upper_length);
}
