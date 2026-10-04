/* SPDX-License-Identifier: ISC
 * branch_file_tree.c -- iterative range copying and whole-subtree tail pruning.
 */
#include "branch_file_internal.h"

#include <string.h>

static void empty_frame(cfs_bf_file_frame_t *frame, uint8_t level, uint64_t base) {
    frame->original = cfs_bf_file_empty();
    frame->level = level;
    frame->base = base;
    frame->slot = 0u;
    frame->dirty = 0u;
    for (uint32_t i = 0u; i < CFS_BF_RADIX_SLOTS; i++) {
        frame->children[i] = cfs_bf_file_empty();
    }
}

static int prepare_root(cfs_bs_store_t *store,
    const cfs_bt_stage_t *source_stage, cfs_bt_stage_t *destination_stage,
    cfs_bf_file_request_t request, cfs_bs_workspace_t *store_workspace,
    cfs_bf_file_workspace_t *workspace) {
    cfs_bf_file_frame_t *frame = &workspace->path[0];
    empty_frame(frame, 0u, 0u);
    if (cfs_bf_file_present(request.root)) {
        int rc = cfs_bf_file_load(store, source_stage, request.root, -1, 0u,
            request.old_size, 1, frame, store_workspace, workspace);
        if (rc != CFS_BS_OK) {
            return rc;
        }
    }

    if (request.kind != CFS_BF_FILE_WRITE) {
        return CFS_BS_OK;
    }

    uint64_t last = (request.offset + request.length - 1u) / CFS_BF_CHUNK_BYTES;
    while (last >= (UINT64_C(1) << (7u * (frame->level + 1u)))) {
        cfs_bs_child_t child = frame->original;
        uint8_t level = frame->level;
        empty_frame(frame, level + 1u, 0u);
        frame->children[0] = child;
        if (cfs_bf_file_present(child)) {
            int rc = cfs_bf_file_emit(destination_stage, frame,
                &frame->original, workspace);
            if (rc != CFS_BS_OK) {
                return rc;
            }
        }
    }

    return CFS_BS_OK;
}

static int chunk_operation(cfs_bs_store_t *store,
    const cfs_bt_stage_t *source_stage, cfs_bt_stage_t *destination_stage,
    cfs_bf_file_request_t request, cfs_bf_file_frame_t *frame,
    cfs_bs_workspace_t *store_workspace, cfs_bf_file_workspace_t *workspace) {
    uint64_t number = frame->base + frame->slot;
    uint64_t start = number * CFS_BF_CHUNK_BYTES;
    uint64_t first = request.offset > start ? request.offset - start : 0u;
    uint64_t end = request.offset + request.length - start;
    if (end > CFS_BF_CHUNK_BYTES) {
        end = CFS_BF_CHUNK_BYTES;
    }

    int rc = CFS_BS_OK;
    if (request.kind == CFS_BF_FILE_WRITE && first == 0u &&
        end == CFS_BF_CHUNK_BYTES) {
        memset(workspace->chunk, 0, CFS_BF_CHUNK_BYTES);
    } else {
        rc = cfs_bf_file_chunk(store, source_stage,
            frame->children[frame->slot], number, request.old_size,
            store_workspace, workspace);
    }

    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (request.kind == CFS_BF_FILE_READ) {
        memcpy(workspace->output + (start + first - request.offset),
            workspace->chunk + first, (size_t)(end - first));
        return CFS_BS_OK;
    }

    if (request.kind == CFS_BF_FILE_WRITE) {
        memcpy(workspace->chunk + first,
            request.bytes + (start + first - request.offset), (size_t)(end - first));
    } else {
        uint32_t keep = (uint32_t)(request.new_size % CFS_BF_CHUNK_BYTES);
        memset(workspace->chunk + keep, 0, CFS_BF_CHUNK_BYTES - keep);
    }

    int nonzero = 0;
    for (uint32_t i = 0u; i < CFS_BF_CHUNK_BYTES; i++) {
        nonzero |= workspace->chunk[i];
    }

    cfs_bs_child_t child = cfs_bf_file_empty();
    if (nonzero != 0) {
        cfs_bs_object_plan_t plan = {.payload = workspace->chunk,
            .payload_length = CFS_BF_CHUNK_BYTES};
        uint32_t index;
        rc = cfs_bt_stage_add(destination_stage, &plan, &index);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        child.kind = CFS_BS_CHILD_PREPARED;
        child.value.prepared_index = index;
        workspace->work.data_emitted++;
    }

    frame->children[frame->slot] = child;
    frame->dirty = 1u;
    return CFS_BS_OK;
}

static int collapse(cfs_bs_store_t *store,
    const cfs_bt_stage_t *source_stage,
    cfs_bs_child_t child, uint64_t size, cfs_bs_child_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bf_file_workspace_t *workspace) {
    cfs_bf_file_frame_t *frame = &workspace->path[0];
    while (cfs_bf_file_present(child)) {
        int rc = cfs_bf_file_load(store, source_stage, child, -1, 0u, size, 0,
            frame, store_workspace, workspace);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        if (frame->level == 0u || !cfs_bf_file_present(frame->children[0])) {
            break;
        }

        int other = 0;
        for (uint32_t i = 1u; i < CFS_BF_RADIX_SLOTS; i++) {
            other |= cfs_bf_file_present(frame->children[i]);
        }

        if (other) {
            break;
        }

        child = frame->children[0];
    }

    *out = child;
    return CFS_BS_OK;
}

static int visit_slot(cfs_bs_store_t *store,
    const cfs_bt_stage_t *source_stage, cfs_bt_stage_t *destination_stage,
    cfs_bf_file_request_t request, uint32_t *depth,
    cfs_bs_workspace_t *store_workspace, cfs_bf_file_workspace_t *workspace) {
    cfs_bf_file_frame_t *frame = &workspace->path[*depth];
    uint64_t span = UINT64_C(1) << (7u * frame->level);
    uint64_t start = frame->base + frame->slot * span;
    uint64_t first = request.offset / CFS_BF_CHUNK_BYTES;
    uint64_t last = request.kind == CFS_BF_FILE_TRUNCATE ? UINT64_MAX :
        (request.offset + request.length - 1u) / CFS_BF_CHUNK_BYTES;
    workspace->work.slots_examined++;
    if (first >= start + span || last < start) {
        frame->slot++;
        return CFS_BS_OK;
    }

    cfs_bs_child_t child = frame->children[frame->slot];
    if (request.kind == CFS_BF_FILE_TRUNCATE &&
        start >= cfs_bf_file_chunks(request.new_size)) {
        if (cfs_bf_file_present(child)) {
            frame->children[frame->slot] = cfs_bf_file_empty();
            frame->dirty = 1u;
        }

        frame->slot++;
        return CFS_BS_OK;
    }

    if (!cfs_bf_file_present(child) && request.kind != CFS_BF_FILE_WRITE) {
        frame->slot++;
        return CFS_BS_OK;
    }

    if (frame->level == 0u) {
        int rc = chunk_operation(store, source_stage, destination_stage,
            request, frame, store_workspace, workspace);
        frame->slot++;
        return rc;
    }

    cfs_bf_file_frame_t *next = &workspace->path[*depth + 1u];
    if (!cfs_bf_file_present(child)) {
        empty_frame(next, frame->level - 1u, start);
    } else {
        int rc = cfs_bf_file_load(store, source_stage, child,
            frame->level - 1u, start, request.old_size, 0, next,
            store_workspace, workspace);
        if (rc != CFS_BS_OK) {
            return rc;
        }
    }

    (*depth)++;
    return CFS_BS_OK;
}

int cfs_bf_file_walk(cfs_bs_store_t *store,
    const cfs_bt_stage_t *source_stage, cfs_bt_stage_t *destination_stage,
    cfs_bf_file_request_t request, cfs_bs_child_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bf_file_workspace_t *workspace) {
    if (request.kind < CFS_BF_FILE_READ ||
        request.kind > CFS_BF_FILE_TRUNCATE ||
        (request.kind == CFS_BF_FILE_READ && destination_stage != NULL) ||
        (request.kind != CFS_BF_FILE_READ &&
            (destination_stage == NULL || source_stage != destination_stage))) {
        return CFS_BS_EINVAL;
    }

    int rc = prepare_root(store, source_stage, destination_stage, request,
        store_workspace, workspace);
    uint32_t depth = 0u;
    while (rc == CFS_BS_OK) {
        cfs_bf_file_frame_t *frame = &workspace->path[depth];
        if (frame->slot < CFS_BF_RADIX_SLOTS) {
            rc = visit_slot(store, source_stage, destination_stage, request,
                &depth, store_workspace, workspace);
            continue;
        }

        cfs_bs_child_t result = frame->original;
        int dirty = frame->dirty;
        if (request.kind != CFS_BF_FILE_READ && dirty) {
            rc = cfs_bf_file_emit(destination_stage, frame, &result, workspace);
            if (rc != CFS_BS_OK) {
                break;
            }
        }

        if (depth == 0u) {
            if (request.kind == CFS_BF_FILE_READ) {
                *out = result;
                return CFS_BS_OK;
            }

            return collapse(store, source_stage, result, request.new_size, out,
                store_workspace, workspace);
        }

        frame = &workspace->path[--depth];
        frame->children[frame->slot++] = result;
        frame->dirty |= (uint8_t)dirty;
    }

    return rc;
}
