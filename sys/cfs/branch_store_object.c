/* SPDX-License-Identifier: ISC
 *
 * branch_store_object.c -- immutable branch-store object framing and reads.
 *
 * This file owns the one semantic object encoding and its bounded physical
 * owning-edge enumerator.  It never changes ownership rows or publication
 * state. Construction may reserve private ownership rows first, but the
 * complete object must authenticate before any ownership change is published.
 */
#include "branch_store_internal.h"

#include <string.h>

#include "codec.h"

static int
cfs_bs_bytes_zero(const uint8_t *bytes, size_t length) {
    size_t i;

    for (i = 0; i < length; ++i) {
        if (bytes[i] != 0u) {
            return 0;
        }
    }

    return 1;
}

static int
cfs_bs_object_shape(uint32_t child_count, uint32_t payload_length,
    size_t *out_payload_off, size_t *out_end) {
    size_t payload_off;

    if (out_payload_off == NULL || out_end == NULL ||
        child_count > CFS_BS_MAX_CHILDREN) {
        return CFS_BS_EINVAL;
    }

    payload_off = CFS_BS_OBJECT_REFS_OFF +
        (size_t)child_count * CFS_BS_REF_SIZE;
    if (payload_off > CFS_BLOCK_SIZE ||
        payload_length > CFS_BLOCK_SIZE - payload_off) {
        return CFS_BS_EINVAL;
    }

    *out_payload_off = payload_off;
    *out_end = payload_off + payload_length;
    return CFS_BS_OK;
}

static int
cfs_bs_child_ref_shape(const cfs_bs_store_t *store, cfs_bs_ref_t ref,
    uint64_t parent_incarnation) {
    if (cfs_bs_ref_is_null(ref) || ref.block < store->semantic_start ||
        ref.block >= store->total_blocks || !cfs_bs_incarnation_valid(ref.incarnation) ||
        ref.incarnation >= parent_incarnation) {
        return CFS_BS_EINVAL;
    }

    return CFS_BS_OK;
}

int
cfs_bs_object_frame(const cfs_bs_store_t *store, uint64_t block,
    uint64_t incarnation, const cfs_bs_ref_t *children,
    uint32_t child_count, const void *payload, uint32_t payload_length,
    uint8_t *out, cfs_bs_ref_t *out_ref) {
    size_t end;
    size_t payload_off;
    uint32_t i;
    int status;

    if (store == NULL || out == NULL || out_ref == NULL ||
        (child_count != 0u && children == NULL) ||
        (payload_length != 0u && payload == NULL) ||
        block < store->semantic_start || block >= store->total_blocks ||
        !cfs_bs_incarnation_valid(incarnation) ||
        store->generation >= CFS_BS_GENERATION_MAX ||
        (incarnation >> CFS_BS_INCARNATION_INDEX_BITS) != store->generation + 1u) {
        return CFS_BS_EINVAL;
    }

    status = cfs_bs_object_shape(child_count, payload_length, &payload_off,
        &end);
    if (status != CFS_BS_OK) {
        return status;
    }

    for (i = 0; i < child_count; ++i) {
        status = cfs_bs_child_ref_shape(store, children[i], incarnation);
        if (status != CFS_BS_OK) {
            return status;
        }
    }

    memset(out, 0, CFS_BLOCK_SIZE);
    cfs_bs_header_frame(out, CFS_BS_KIND_OBJECT, block, incarnation >> CFS_BS_INCARNATION_INDEX_BITS,
        store->uuid);
    cfs_put64(out + CFS_BS_OBJECT_INCARNATION_OFF, incarnation);
    cfs_put32(out + CFS_BS_OBJECT_CHILD_COUNT_OFF, child_count);
    cfs_put32(out + CFS_BS_OBJECT_PAYLOAD_LENGTH_OFF, payload_length);
    for (i = 0; i < child_count; ++i) {
        cfs_bs_ref_encode(out + CFS_BS_OBJECT_REFS_OFF +
            (size_t)i * CFS_BS_REF_SIZE, children[i]);
    }

    if (payload_length != 0u) {
        memcpy(out + payload_off, payload, payload_length);
    }

    (void)end;
    cfs_bs_header_rehash(out);

    out_ref->block = block;
    out_ref->incarnation = incarnation;
    out_ref->hash = cfs_get64(out + CFS_BS_HEADER_HASH_OFF);
    return CFS_BS_OK;
}

int
cfs_bs_object_page_validate(const cfs_bs_store_t *store, cfs_bs_ref_t ref,
    const uint8_t *page, uint32_t *out_child_count,
    uint32_t *out_payload_length) {
    uint64_t generation;
    uint32_t child_count;
    uint32_t payload_length;
    size_t end;
    size_t payload_off;
    uint32_t i;
    int status;

    if (store == NULL || page == NULL) {
        return CFS_BS_EINVAL;
    }

    if (cfs_bs_ref_is_null(ref) || ref.block < store->semantic_start ||
        ref.block >= store->total_blocks || !cfs_bs_incarnation_valid(ref.incarnation)) {
        return CFS_BS_EINVAL;
    }

    status = cfs_bs_header_validate(page, CFS_BS_KIND_OBJECT, ref.block,
        store->uuid, &generation);
    if (status != CFS_BS_OK) {
        return status;
    }

    if (generation != (ref.incarnation >> CFS_BS_INCARNATION_INDEX_BITS) ||
        cfs_get64(page + CFS_BS_OBJECT_INCARNATION_OFF) != ref.incarnation ||
        cfs_get64(page + CFS_BS_HEADER_HASH_OFF) != ref.hash ||
        !cfs_bs_bytes_zero(page + CFS_BS_OBJECT_RESERVED_OFF,
            CFS_BS_OBJECT_REFS_OFF - CFS_BS_OBJECT_RESERVED_OFF)) {
        return CFS_BS_ECORRUPT;
    }

    child_count = cfs_get32(page + CFS_BS_OBJECT_CHILD_COUNT_OFF);
    payload_length = cfs_get32(page + CFS_BS_OBJECT_PAYLOAD_LENGTH_OFF);
    status = cfs_bs_object_shape(child_count, payload_length, &payload_off,
        &end);
    if (status != CFS_BS_OK) {
        return CFS_BS_ECORRUPT;
    }

    for (i = 0; i < child_count; ++i) {
        cfs_bs_ref_t child;

        cfs_bs_ref_decode(page + CFS_BS_OBJECT_REFS_OFF +
            (size_t)i * CFS_BS_REF_SIZE, &child);
        if (cfs_bs_child_ref_shape(store, child, ref.incarnation) !=
            CFS_BS_OK) {
            return CFS_BS_ECORRUPT;
        }
    }

    if (!cfs_bs_bytes_zero(page + end, CFS_BLOCK_SIZE - end)) {
        return CFS_BS_ECORRUPT;
    }

    if (out_child_count) {
        *out_child_count = child_count;
    }

    if (out_payload_length) {
        *out_payload_length = payload_length;
    }

    return CFS_BS_OK;
}

int
cfs_bs_object_load(cfs_bs_store_t *store, cfs_bs_ref_t ref, uint8_t *out,
    uint32_t *out_child_count, uint32_t *out_payload_length) {
    int status;

    if (store == NULL || out == NULL || out_child_count == NULL ||
        out_payload_length == NULL) {
        return CFS_BS_EINVAL;
    }

    if (store->poisoned != 0u) {
        return CFS_BS_EPOISON;
    }

    if (cfs_bs_ref_is_null(ref) || ref.block < store->semantic_start ||
        ref.block >= store->total_blocks || ref.incarnation == 0u) {
        return CFS_BS_EINVAL;
    }

    cfs_bs_cache_slot_t *slot;
    status = cfs_bs_io_read_slot(store, ref.block, out, &slot);
    if (status != CFS_BS_OK) {
        return status;
    }

    /* These exact bytes already validated as this reference. */
    if (slot != NULL && slot->validated == CFS_BS_KIND_OBJECT &&
        slot->incarnation == ref.incarnation && slot->hash == ref.hash) {
        *out_child_count = slot->child_count;
        *out_payload_length = slot->payload_length;
        return CFS_BS_OK;
    }

    status = cfs_bs_object_page_validate(store, ref, out, out_child_count,
        out_payload_length);
    if (status == CFS_BS_OK && slot != NULL) {
        slot->validated = CFS_BS_KIND_OBJECT;
        slot->incarnation = ref.incarnation;
        slot->hash = ref.hash;
        slot->child_count = *out_child_count;
        slot->payload_length = *out_payload_length;
    }

    return status;
}

int
cfs_bs_object_read(cfs_bs_store_t *store, cfs_bs_ref_t object,
    cfs_bs_ref_t *children, uint32_t child_capacity,
    uint32_t *out_child_count, void *payload, uint32_t payload_capacity,
    uint32_t *out_payload_length, cfs_bs_workspace_t *workspace) {
    cfs_bs_row_t row;
    uint32_t child_count;
    uint32_t payload_length;
    size_t payload_off;
    uint32_t i;
    int status;

    if (out_child_count == NULL || out_payload_length == NULL ||
        workspace == NULL || (child_capacity != 0u && children == NULL) ||
        (payload_capacity != 0u && payload == NULL)) {
        return CFS_BS_EINVAL;
    }

    status = cfs_bs_inspect_row(store, object.block, &row, workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    if (row.state != CFS_BS_ROW_LIVE ||
        row.incarnation != object.incarnation) {
        return CFS_BS_ENOENT;
    }

    status = cfs_bs_object_load(store, object, workspace->scratch[0].bytes,
        &child_count, &payload_length);
    if (status != CFS_BS_OK) {
        return status;
    }

    *out_child_count = child_count;
    *out_payload_length = payload_length;
    if (child_capacity < child_count || payload_capacity < payload_length) {
        return CFS_BS_EWORKSPACE;
    }

    for (i = 0; i < child_count; ++i) {
        cfs_bs_ref_decode(workspace->scratch[0].bytes +
            CFS_BS_OBJECT_REFS_OFF + (size_t)i * CFS_BS_REF_SIZE,
            &children[i]);
    }

    payload_off = CFS_BS_OBJECT_REFS_OFF +
        (size_t)child_count * CFS_BS_REF_SIZE;
    if (payload_length != 0u) {
        memcpy(payload, workspace->scratch[0].bytes + payload_off,
            payload_length);
    }

    return CFS_BS_OK;
}
