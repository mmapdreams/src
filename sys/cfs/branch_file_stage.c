/* SPDX-License-Identifier: ISC
 * branch_file_stage.c -- transaction-local sparse content operations.
 */
#include "branch_file_internal.h"

#include <string.h>

static cfs_bs_child_t retained(cfs_bs_ref_t ref) {
    return (cfs_bs_child_t){.kind = CFS_BS_CHILD_RETAINED,
        .value.retained = ref};
}

static int prepare(cfs_bs_store_t *store, const cfs_bt_stage_t *stage,
    int require_stage, cfs_bs_workspace_t *store_workspace,
    cfs_bf_file_workspace_t *workspace) {
    if (store == NULL || store_workspace == NULL || workspace == NULL ||
        (require_stage && stage == NULL) ||
        (stage != NULL && (stage->state != CFS_BT_STAGE_EDITABLE ||
            stage->count > CFS_BS_CONSTRUCT_MAX_OBJECTS))) {
        return CFS_BS_EINVAL;
    }

    memset(&workspace->work, 0, sizeof(workspace->work));
    if (store->poisoned != 0u) {
        return CFS_BS_EPOISON;
    }

    return store->generation == 0u ? CFS_BS_ECORRUPT : CFS_BS_OK;
}

static int state_valid(const cfs_bs_store_t *store,
    const cfs_bt_stage_t *stage, cfs_bf_file_state_t state) {
    if (state.size > INT64_MAX) {
        return 0;
    }

    if (state.root.kind == CFS_BS_CHILD_PREPARED) {
        return state.size != 0u && stage != NULL &&
            stage->count <= CFS_BS_CONSTRUCT_MAX_OBJECTS &&
            state.root.value.prepared_index < stage->count;
    }

    if (state.root.kind != CFS_BS_CHILD_RETAINED) {
        return 0;
    }

    cfs_bs_ref_t ref = state.root.value.retained;
    if (ref.block == CFS_BS_BLOCK_NONE) {
        return ref.incarnation == 0u && ref.hash == 0u;
    }

    return state.size != 0u && ref.block >= store->semantic_start &&
        ref.block < store->total_blocks && ref.incarnation != 0u &&
        (ref.incarnation >> CFS_BS_INCARNATION_INDEX_BITS) <= store->generation;
}

static int finish(cfs_bt_stage_t *stage, int status) {
    if (status != CFS_BS_OK) {
        cfs_bt_stage_discard(stage);
    }

    return status;
}

int cfs_bf_file_content_read(cfs_bs_store_t *store,
    const cfs_bt_stage_t *stage, cfs_bf_file_state_t state, uint64_t offset,
    void *bytes, uint32_t length, uint32_t *out_length,
    cfs_bs_workspace_t *store_workspace, cfs_bf_file_workspace_t *workspace) {
    if (out_length == NULL || (length != 0u && bytes == NULL)) {
        return CFS_BS_EINVAL;
    }

    if (length > CFS_BF_IO_MAX) {
        return CFS_BS_EWORKSPACE;
    }

    if (offset > INT64_MAX || length > INT64_MAX - offset) {
        return CFS_BS_EOVERFLOW;
    }

    int status = prepare(store, stage, 0, store_workspace, workspace);
    if (status == CFS_BS_OK && !state_valid(store, stage, state)) {
        status = CFS_BS_EINVAL;
    }

    if (status != CFS_BS_OK) {
        return status;
    }

    if (offset >= state.size) {
        length = 0u;
    } else if (length > state.size - offset) {
        length = (uint32_t)(state.size - offset);
    }

    memset(workspace->output, 0, length);
    if (length != 0u && cfs_bf_file_present(state.root)) {
        cfs_bf_file_request_t request = {.root = state.root,
            .old_size = state.size, .offset = offset, .length = length,
            .kind = CFS_BF_FILE_READ};
        cfs_bs_child_t unused;
        status = cfs_bf_file_walk(store, stage, NULL, request, &unused,
            store_workspace, workspace);
    }

    if (status == CFS_BS_OK) {
        if (length != 0u) {
            memcpy(bytes, workspace->output, length);
        }

        *out_length = length;
    }

    return status;
}

int cfs_bf_file_content_write(cfs_bs_store_t *store, cfs_bt_stage_t *stage,
    cfs_bf_file_state_t state, uint64_t offset, const void *bytes,
    uint32_t length, cfs_bf_file_state_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bf_file_workspace_t *workspace) {
    int status = CFS_BS_OK;
    if (out == NULL || (length != 0u && bytes == NULL)) {
        status = CFS_BS_EINVAL;
    } else if (length > CFS_BF_IO_MAX) {
        status = CFS_BS_EWORKSPACE;
    } else if (offset > INT64_MAX || length > INT64_MAX - offset) {
        status = CFS_BS_EOVERFLOW;
    }

    if (status == CFS_BS_OK) {
        status = prepare(store, stage, 1, store_workspace, workspace);
    }

    if (status == CFS_BS_OK && !state_valid(store, stage, state)) {
        status = CFS_BS_EINVAL;
    }

    if (status == CFS_BS_OK && length == 0u) {
        *out = state;
        return CFS_BS_OK;
    }

    cfs_bf_file_state_t result;
    if (status == CFS_BS_OK) {
        result.size = state.size > offset + length ? state.size : offset + length;
        cfs_bf_file_request_t request = {.root = state.root,
            .old_size = state.size, .new_size = result.size, .offset = offset,
            .bytes = bytes, .length = length, .kind = CFS_BF_FILE_WRITE};
        status = cfs_bf_file_walk(store, stage, stage, request, &result.root,
            store_workspace, workspace);
    }

    if (status == CFS_BS_OK) {
        *out = result;
    }

    return finish(stage, status);
}

int cfs_bf_file_content_truncate(cfs_bs_store_t *store, cfs_bt_stage_t *stage,
    cfs_bf_file_state_t state, uint64_t size, cfs_bf_file_state_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bf_file_workspace_t *workspace) {
    int status = out == NULL ? CFS_BS_EINVAL : CFS_BS_OK;
    if (status == CFS_BS_OK && size > INT64_MAX) {
        status = CFS_BS_EOVERFLOW;
    }

    if (status == CFS_BS_OK) {
        status = prepare(store, stage, 1, store_workspace, workspace);
    }

    if (status == CFS_BS_OK && !state_valid(store, stage, state)) {
        status = CFS_BS_EINVAL;
    }

    cfs_bf_file_state_t result = {.root = state.root, .size = size};
    if (status == CFS_BS_OK && size == state.size) {
        *out = result;
        return CFS_BS_OK;
    }

    if (status == CFS_BS_OK && size == 0u) {
        result.root = cfs_bf_file_empty();
    } else if (status == CFS_BS_OK && size < state.size &&
        cfs_bf_file_present(state.root)) {
        cfs_bf_file_request_t request = {.root = state.root,
            .old_size = state.size, .new_size = size, .offset = size,
            .kind = CFS_BF_FILE_TRUNCATE};
        status = cfs_bf_file_walk(store, stage, stage, request, &result.root,
            store_workspace, workspace);
    }

    if (status == CFS_BS_OK) {
        *out = result;
    }

    return finish(stage, status);
}

int cfs_bf_file_resize(cfs_bs_store_t *store, cfs_bs_ref_t root,
    uint64_t old_size, uint64_t size, cfs_bs_child_t *out,
    cfs_bs_workspace_t *storage, cfs_bf_workspace_t *namespace_workspace,
    cfs_bf_file_workspace_t *workspace) {
    if (!namespace_workspace) {
        return CFS_BS_EINVAL;
    }

    if (!out) {
        return finish(&namespace_workspace->stage, CFS_BS_EINVAL);
    }

    cfs_bf_file_state_t state = {.root = retained(root), .size = old_size};
    cfs_bf_file_state_t result;
    int status = cfs_bf_file_content_truncate(store, &namespace_workspace->stage,
        state, size, &result, storage, workspace);
    if (status == CFS_BS_OK) {
        *out = result.root;
    }

    return status;
}
