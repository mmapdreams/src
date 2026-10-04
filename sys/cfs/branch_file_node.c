/* SPDX-License-Identifier: ISC
 * branch_file_node.c -- sparse index framing, authentication, and chunk reads.
 */
#include "branch_file_internal.h"

#include <string.h>

#include "codec.h"

#define FILE_MAGIC UINT64_C(0x3130304446534643)

cfs_bs_child_t cfs_bf_file_empty(void) {
    return (cfs_bs_child_t){.kind = CFS_BS_CHILD_RETAINED,
        .value.retained = {CFS_BS_BLOCK_NONE, 0u, 0u}};
}

int cfs_bf_file_present(cfs_bs_child_t child) {
    return child.kind == CFS_BS_CHILD_PREPARED ||
        (child.kind == CFS_BS_CHILD_RETAINED &&
            child.value.retained.block != CFS_BS_BLOCK_NONE);
}

uint64_t cfs_bf_file_chunks(uint64_t size) {
    return size / CFS_BF_CHUNK_BYTES + (size % CFS_BF_CHUNK_BYTES != 0u);
}

static int staged_child_valid(const cfs_bt_stage_t *stage,
    cfs_bs_child_t child) {
    if (child.kind == CFS_BS_CHILD_PREPARED) {
        return stage != NULL &&
            stage->count <= CFS_BS_CONSTRUCT_MAX_OBJECTS &&
            child.value.prepared_index < stage->count;
    }

    return child.kind == CFS_BS_CHILD_RETAINED &&
        child.value.retained.block != CFS_BS_BLOCK_NONE &&
        child.value.retained.incarnation != 0u;
}

int cfs_bf_file_node_decode(const void *bytes, uint32_t length,
    uint32_t children, uint8_t *out_level) {
    if (bytes == NULL || out_level == NULL) {
        return CFS_BS_EINVAL;
    }

    const uint8_t *data = bytes;
    if (length != CFS_BF_RADIX_BYTES || cfs_get64(data) != FILE_MAGIC ||
        cfs_get64(data + 8u) > CFS_BF_RADIX_LEVEL_MAX || children == 0u) {
        return CFS_BS_ECORRUPT;
    }

    uint32_t count = 0u;
    for (uint32_t i = 0u; i < CFS_BF_RADIX_SLOTS; i++) {
        count += (data[16u + i / 8u] >> (i % 8u)) & 1u;
    }

    if (count != children) {
        return CFS_BS_ECORRUPT;
    }

    *out_level = (uint8_t)cfs_get64(data + 8u);
    return CFS_BS_OK;
}

int cfs_bf_file_load(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    cfs_bs_child_t child, int level, uint64_t base, uint64_t size, int root,
    cfs_bf_file_frame_t *frame, cfs_bs_workspace_t *store_workspace,
    cfs_bf_file_workspace_t *workspace) {
    uint32_t count;
    uint32_t length;
    int rc;
    cfs_bs_object_plan_t plan;
    if (child.kind == CFS_BS_CHILD_PREPARED) {
        rc = cfs_bt_stage_view(stage, child.value.prepared_index, &plan);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        count = plan.child_count;
        length = plan.payload_length;
        if (length != CFS_BF_RADIX_BYTES) {
            return CFS_BS_ECORRUPT;
        }

        memcpy(workspace->node, plan.payload, length);
    } else if (child.kind == CFS_BS_CHILD_RETAINED) {
        rc = cfs_bs_object_read(store, child.value.retained, workspace->physical,
            CFS_BF_RADIX_SLOTS, &count, workspace->node, CFS_BF_RADIX_BYTES,
            &length, store_workspace);
        if (rc != CFS_BS_OK) {
            return rc == CFS_BS_EWORKSPACE || rc == CFS_BS_ENOENT ?
                CFS_BS_ECORRUPT : rc;
        }
    } else {
        return CFS_BS_EINVAL;
    }

    uint8_t decoded;
    rc = cfs_bf_file_node_decode(workspace->node, length, count, &decoded);
    if (rc != CFS_BS_OK || (level >= 0 && decoded != level) ||
        (root && decoded != 0u && count == 1u && workspace->node[16] == 1u)) {
        return CFS_BS_ECORRUPT;
    }

    frame->original = child;
    frame->level = decoded;
    frame->base = base;
    frame->slot = 0u;
    frame->dirty = 0u;
    uint32_t index = 0u;
    uint64_t span = UINT64_C(1) << (7u * decoded);
    for (uint32_t i = 0u; i < CFS_BF_RADIX_SLOTS; i++) {
        frame->children[i] = cfs_bf_file_empty();
        if (((workspace->node[16u + i / 8u] >> (i % 8u)) & 1u) == 0u) {
            continue;
        }

        if (base + i * span >= cfs_bf_file_chunks(size)) {
            return CFS_BS_ECORRUPT;
        }

        if (child.kind == CFS_BS_CHILD_PREPARED) {
            frame->children[i] = plan.children[index];
            if (!staged_child_valid(stage, frame->children[i])) {
                return CFS_BS_EINVAL;
            }
        } else {
            frame->children[i].kind = CFS_BS_CHILD_RETAINED;
            frame->children[i].value.retained = workspace->physical[index];
        }

        index++;
    }

    workspace->work.nodes_loaded++;
    return CFS_BS_OK;
}

int cfs_bf_file_emit(cfs_bt_stage_t *stage, const cfs_bf_file_frame_t *frame,
    cfs_bs_child_t *out, cfs_bf_file_workspace_t *workspace) {
    memset(workspace->node, 0, CFS_BF_RADIX_BYTES);
    cfs_put64(workspace->node, FILE_MAGIC);
    cfs_put64(workspace->node + 8u, frame->level);
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < CFS_BF_RADIX_SLOTS; i++) {
        if (cfs_bf_file_present(frame->children[i])) {
            workspace->node[16u + i / 8u] |= (uint8_t)(1u << (i % 8u));
            workspace->packed[count++] = frame->children[i];
        }
    }

    if (count == 0u) {
        *out = cfs_bf_file_empty();
        return CFS_BS_OK;
    }

    cfs_bs_object_plan_t plan = {.children = workspace->packed,
        .child_count = count, .payload = workspace->node,
        .payload_length = CFS_BF_RADIX_BYTES};
    uint32_t index;
    int rc = cfs_bt_stage_add(stage, &plan, &index);
    if (rc == CFS_BS_OK) {
        *out = (cfs_bs_child_t){.kind = CFS_BS_CHILD_PREPARED,
            .value.prepared_index = index};
        workspace->work.nodes_emitted++;
    }

    return rc;
}

int cfs_bf_file_chunk(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    cfs_bs_child_t child, uint64_t number, uint64_t size,
    cfs_bs_workspace_t *store_workspace, cfs_bf_file_workspace_t *workspace) {
    if (child.kind == CFS_BS_CHILD_RETAINED &&
        child.value.retained.block == CFS_BS_BLOCK_NONE) {
        if (child.value.retained.incarnation != 0u ||
            child.value.retained.hash != 0u) {
            return CFS_BS_EINVAL;
        }

        memset(workspace->chunk, 0, CFS_BF_CHUNK_BYTES);
        return CFS_BS_OK;
    }

    uint32_t count;
    uint32_t length;
    int rc;
    if (child.kind == CFS_BS_CHILD_PREPARED) {
        cfs_bs_object_plan_t plan;
        rc = cfs_bt_stage_view(stage, child.value.prepared_index, &plan);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        count = plan.child_count;
        length = plan.payload_length;
        if (count != 0u || length != CFS_BF_CHUNK_BYTES) {
            return CFS_BS_ECORRUPT;
        }

        memcpy(workspace->chunk, plan.payload, length);
    } else if (child.kind == CFS_BS_CHILD_RETAINED) {
        rc = cfs_bs_object_read(store, child.value.retained, NULL, 0u, &count,
            workspace->chunk, CFS_BF_CHUNK_BYTES, &length, store_workspace);
        if (rc != CFS_BS_OK) {
            return rc == CFS_BS_EWORKSPACE || rc == CFS_BS_ENOENT ?
                CFS_BS_ECORRUPT : rc;
        }
    } else {
        return CFS_BS_EINVAL;
    }

    if (count != 0u || length != CFS_BF_CHUNK_BYTES ||
        number >= cfs_bf_file_chunks(size)) {
        return CFS_BS_ECORRUPT;
    }

    if (number == size / CFS_BF_CHUNK_BYTES) {
        for (uint32_t i = (uint32_t)(size % CFS_BF_CHUNK_BYTES);
            i < CFS_BF_CHUNK_BYTES; i++) {
            if (workspace->chunk[i] != 0u) {
                return CFS_BS_ECORRUPT;
            }
        }
    }

    workspace->work.data_loaded++;
    return CFS_BS_OK;
}
