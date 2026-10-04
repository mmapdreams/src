/* SPDX-License-Identifier: ISC
 * branch_validate.c -- inode/orphan census and exact-root validation receipt.
 */
#include "branch_validate_internal.h"
#include "branch_fs_internal.h"
#include "codec.h"

#include <string.h>

int cfs_bf_validate_checkpoint(cfs_bf_validate_workspace_t *w) {
    if (w->yield) {
        int answer = w->yield(w->context);
        w->failure = w->store->poisoned ? CFS_BS_EPOISON :
            answer == 0 ? CFS_BS_OK : CFS_BS_EINVAL;
    }

    return w->failure;
}

int cfs_bf_validate_step(cfs_bf_validate_workspace_t *w) {
    if (cfs_bf_validate_checkpoint(w) != CFS_BS_OK) {
        return w->failure;
    }

    if (w->counts.steps == w->budget) {
        w->failure = CFS_BS_EWORKSPACE;
        return w->failure;
    }

    w->counts.steps++;
    return CFS_BS_OK;
}

static int compare(cfs_bf_id_t left, cfs_bf_id_t right) {
    if (left.birth != right.birth) {
        return left.birth < right.birth ? -1 : 1;
    }

    return left.ordinal < right.ordinal ? -1 : left.ordinal != right.ordinal;
}

size_t cfs_bf_validate_find(cfs_bf_validate_workspace_t *w, cfs_bf_id_t id) {
    size_t first = 0u;
    size_t end = w->count;
    while (first < end) {
        size_t middle = first + (end - first) / 2u;
        int order = compare(w->inodes[middle].id, id);
        w->counts.identity_comparisons++;
        if (order == 0) {
            return middle;
        }

        if (order < 0) {
            first = middle + 1u;
        } else {
            end = middle;
        }
    }

    return SIZE_MAX;
}

static int key(cfs_bf_validate_workspace_t *w, const cfs_bt_record_t *record,
    cfs_bf_id_t *out) {
    if (record->key_length != CFS_BF_ID_BYTES) {
        return CFS_BS_ECORRUPT;
    }

    const uint8_t *bytes = record->key;
    *out = (cfs_bf_id_t){cfs_get64be(bytes), cfs_get64be(bytes + 8u)};
    return cfs_bf_id_valid(*out) && out->birth <= w->view.revision ?
        CFS_BS_OK : CFS_BS_ECORRUPT;
}

static int inode(void *opaque, const cfs_bt_record_t *record,
    const cfs_bs_ref_t *child) {
    cfs_bf_validate_workspace_t *w = opaque;
    cfs_bf_id_t id;
    cfs_bf_inode_t metadata;
    int status = cfs_bf_validate_step(w);
    if (status == CFS_BS_OK) {
        status = key(w, record, &id);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_inode_decode(record->value, record->value_length,
            child != NULL, &metadata);
    }

    if (status == CFS_BS_OK && (metadata.parent.birth > w->view.revision ||
            (w->count != 0u && compare(w->inodes[w->count - 1u].id, id) >= 0))) {
        status = CFS_BS_ECORRUPT;
    }

    if (status == CFS_BS_OK && w->count == w->capacity) {
        status = CFS_BS_EWORKSPACE;
    }

    if (status == CFS_BS_OK) {
        w->inodes[w->count++] = (cfs_bf_validate_inode_t){.id = id,
            .parent = metadata.parent, .size = metadata.size,
            .links = metadata.links, .type = metadata.type,
            .content = child == NULL ?
                (cfs_bs_ref_t){CFS_BS_BLOCK_NONE, 0u, 0u} : *child,
            .parent_index = SIZE_MAX};
        w->counts.inodes++;
    }

    w->failure = status;
    return status == CFS_BS_OK ? CFS_BT_SCAN_CONTINUE : CFS_BT_SCAN_STOP;
}

static int orphan(void *opaque, const cfs_bt_record_t *record,
    const cfs_bs_ref_t *child) {
    cfs_bf_validate_workspace_t *w = opaque;
    cfs_bf_id_t id;
    int status = cfs_bf_validate_step(w);
    if (status == CFS_BS_OK) {
        status = key(w, record, &id);
    }

    size_t index = status == CFS_BS_OK ? cfs_bf_validate_find(w, id) : SIZE_MAX;
    if (status == CFS_BS_OK && (record->value_length != 0u || child != NULL ||
            index == SIZE_MAX || index == w->root_index ||
            w->inodes[index].links != 0u || w->inodes[index].orphan)) {
        status = CFS_BS_ECORRUPT;
    }

    if (status == CFS_BS_OK) {
        w->inodes[index].orphan = 1u;
        w->counts.orphans++;
    }

    w->failure = status;
    return status == CFS_BS_OK ? CFS_BT_SCAN_CONTINUE : CFS_BT_SCAN_STOP;
}

int cfs_bf_validate_scan(cfs_bs_store_t *store, cfs_bs_ref_t root,
    cfs_bt_visit_fn visit, uint64_t *count, cfs_bs_workspace_t *storage,
    cfs_bf_validate_workspace_t *w) {
    if (cfs_bf_validate_checkpoint(w) != CFS_BS_OK) {
        return w->failure;
    }

    cfs_bt_scan_result_t result;
    int status = cfs_bt_scan(store, root, NULL, 0u, NULL, 0u, visit, w,
        &result, storage, &w->namespace.read);
    w->counts.tree_nodes += w->namespace.read.work.nodes_loaded;
    if (w->failure != CFS_BS_OK) {
        status = w->failure;
    }

    if (status == CFS_BS_OK) {
        if (!result.complete) {
            return CFS_BS_ECORRUPT;
        }

        *count = result.count;
    }

    return status == CFS_BS_ENOENT ? CFS_BS_ECORRUPT : status;
}

int cfs_bf_validate(cfs_bs_store_t *store, cfs_bs_ref_t root,
    uint64_t revision, cfs_bf_validate_inode_t *inodes, size_t capacity,
    uint64_t budget, cfs_bf_validate_yield_fn yield, void *context,
    cfs_bf_validated_t *out, cfs_bs_workspace_t *storage,
    cfs_bf_validate_workspace_t *w) {
    if (w == NULL || out == NULL || (capacity != 0u && inodes == NULL)) {
        return CFS_BS_EINVAL;
    }

    if (capacity > SIZE_MAX / sizeof(*inodes)) {
        return CFS_BS_EOVERFLOW;
    }

    int status = cfs_bf_prepare(store, storage, &w->namespace);
    if (status != CFS_BS_OK) {
        return status;
    }

    memset(&w->counts, 0, sizeof(w->counts));
    memset(&w->file.work, 0, sizeof(w->file.work));
    w->inodes = inodes;
    w->capacity = capacity;
    w->budget = budget;
    w->count = 0u;
    w->failure = CFS_BS_OK;
    w->root_index = SIZE_MAX;
    w->store = store;
    w->yield = yield;
    w->context = context;
    status = cfs_bf_validate_checkpoint(w);
    if (status == CFS_BS_OK) {
        status = cfs_bf_read(store, root, revision, &w->view, storage,
            &w->namespace);
    }

    uint64_t count;
    if (status == CFS_BS_OK) {
        status = cfs_bf_validate_scan(store, w->view.inodes, inode, &count,
            storage, w);
    }

    if (status == CFS_BS_OK) {
        w->root_index = cfs_bf_validate_find(w, w->view.root_id);
        if (w->root_index == SIZE_MAX) {
            status = CFS_BS_ECORRUPT;
        }
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_validate_scan(store, w->view.orphans, orphan, &count,
            storage, w);
    }

    for (w->current = 0u; status == CFS_BS_OK && w->current < w->count;
        w->current++) {
        status = cfs_bf_validate_checkpoint(w);
        if (status == CFS_BS_OK) {
            status = cfs_bf_validate_content(store, storage, w);
        }
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_validate_graph(w);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_validate_checkpoint(w);
    }

    if (status == CFS_BS_OK) {
        cfs_bf_validated_t result = {.root = root, .revision = revision,
            .root_id = w->view.root_id, .counts = w->counts};
        memcpy(result.uuid, store->uuid, sizeof(result.uuid));
        *out = result;
    }

    return status;
}
