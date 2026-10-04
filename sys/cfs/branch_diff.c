/* SPDX-License-Identifier: ISC
 * branch_diff.c -- logical inode, binding, and content comparison events.
 */
#include "branch_diff_internal.h"
#include "branch_fs_internal.h"
#include "branch_symlink_internal.h"
#include "codec.h"

#include <string.h>

int cfs_bf_diff_deliver(cfs_bf_diff_context_t *context,
    const cfs_bf_change_t *change) {
    if (context->workspace->work.events_delivered == UINT64_MAX) {
        return CFS_BS_EOVERFLOW;
    }

    context->workspace->work.events_delivered++;
    int answer = context->visit(context->context, change);
    if (context->store != NULL && context->store->poisoned) {
        return CFS_BS_EPOISON;
    }

    if (answer == CFS_BT_SCAN_STOP) {
        context->stopped = 1;
        return CFS_BS_OK;
    }

    return answer == CFS_BT_SCAN_CONTINUE ? CFS_BS_OK : CFS_BS_EINVAL;
}

int cfs_bf_diff_checkpoint(void *opaque) {
    cfs_bf_diff_context_t *context = opaque;
    int answer = context->yield(context->context);
    if (context->store->poisoned) {
        context->status = CFS_BS_EPOISON;
    } else if (answer != CFS_BT_SCAN_CONTINUE && answer != CFS_BT_SCAN_STOP) {
        context->status = CFS_BS_EINVAL;
    }

    if (answer != CFS_BT_SCAN_CONTINUE || context->status != CFS_BS_OK) {
        context->stopped = 1;
    }

    return context->stopped ? CFS_BT_SCAN_STOP : CFS_BT_SCAN_CONTINUE;
}

static int finish(cfs_bf_diff_context_t *context, int status) {
    context->status = status;
    return status != CFS_BS_OK || context->stopped ?
        CFS_BT_SCAN_STOP : CFS_BT_SCAN_CONTINUE;
}

static int decode_inode(const cfs_bt_record_t *record,
    const cfs_bs_ref_t *child, const cfs_bf_view_t *view,
    cfs_bf_entry_t *out) {
    if (!record) {
        return CFS_BS_OK;
    }

    if (record->key_length != CFS_BF_ID_BYTES) {
        return CFS_BS_ECORRUPT;
    }

    const uint8_t *key = record->key;
    out->id = (cfs_bf_id_t){cfs_get64be(key), cfs_get64be(key + 8u)};
    out->tree = child == NULL ?
        (cfs_bs_ref_t){CFS_BS_BLOCK_NONE, 0u, 0u} : *child;
    if (!cfs_bf_id_valid(out->id) || out->id.birth > view->revision ||
        cfs_bf_inode_decode(record->value, record->value_length, child != NULL,
            &out->inode) != CFS_BS_OK ||
        out->inode.parent.birth > view->revision) {
        return CFS_BS_ECORRUPT;
    }

    return CFS_BS_OK;
}

static int binding(cfs_bf_diff_context_t *context, uint32_t side,
    const cfs_bt_record_t *record, const cfs_bs_ref_t *child,
    cfs_bf_id_t *out) {
    if (!record) {
        *out = (cfs_bf_id_t){0u, 0u};
        return CFS_BS_OK;
    }

    if (child != NULL || !cfs_bf_name_valid(record->key, record->key_length) ||
        cfs_bf_dirent_decode(record->value, record->value_length, out) != CFS_BS_OK ||
        out->birth > context->view[side]->revision) {
        return CFS_BS_ECORRUPT;
    }

    cfs_bf_entry_t target;
    int status = cfs_bf_get(context->store, context->view[side], *out, &target,
        context->storage, context->namespace_workspace);
    if (status == CFS_BS_ENOENT || (status == CFS_BS_OK &&
            (target.inode.links == 0u || (target.inode.type == CFS_BF_DIRECTORY &&
                !cfs_bf_id_equal(target.inode.parent, context->id))))) {
        return CFS_BS_ECORRUPT;
    }

    return status;
}

static int visit_binding(void *opaque, const cfs_bt_record_t *before,
    const cfs_bs_ref_t *before_child, const cfs_bt_record_t *after,
    const cfs_bs_ref_t *after_child) {
    cfs_bf_diff_context_t *context = opaque;
    const cfs_bt_record_t *name = after == NULL ? before : after;
    cfs_bf_change_t change = {.kind = CFS_BF_CHANGE_ENTRY, .id = context->id,
        .bytes = name->key, .length = name->key_length};
    int status = binding(context, 0u, before, before_child, &change.before_id);
    if (status == CFS_BS_OK) {
        status = binding(context, 1u, after, after_child, &change.after_id);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_diff_deliver(context, &change);
    }

    return finish(context, status);
}

static int directories(cfs_bf_diff_context_t *context,
    const cfs_bf_entry_t *before, const cfs_bf_entry_t *after) {
    const cfs_bf_entry_t *entries[2] = {before, after};
    cfs_bs_ref_t roots[2] = {{CFS_BS_BLOCK_NONE, 0u, 0u},
        {CFS_BS_BLOCK_NONE, 0u, 0u}};
    for (uint32_t i = 0u; i < 2u; i++) {
        if (entries[i]) {
            cfs_bf_entry_t checked;
            int status = cfs_bf_directory(context->store, context->view[i],
                context->id, &checked, context->storage,
                context->namespace_workspace);
            if (status != CFS_BS_OK) {
                return status;
            }

            roots[i] = checked.tree;
        }
    }

    cfs_bt_scan_result_t result;
    int status = cfs_bt_diff(context->store, roots[0], roots[1], visit_binding,
        context, context->yield == NULL ? NULL : cfs_bf_diff_checkpoint,
        &result, context->storage, &context->workspace->directories);
    context->workspace->work.tree_nodes_loaded +=
        context->workspace->directories.work.nodes_loaded;
    context->workspace->work.subtrees_skipped +=
        context->workspace->directories.work.subtrees_skipped;

    /* A nested read owns its original error even when it poisons the owner. */
    return context->status != CFS_BS_OK ? context->status : status;
}

static int symlinks(cfs_bf_diff_context_t *context,
    const cfs_bf_entry_t *before, const cfs_bf_entry_t *after) {
    if (!after) {
        return CFS_BS_OK;
    }

    const cfs_bf_entry_t *entries[2] = {before, after};
    for (uint32_t i = 0u; i < 2u; i++) {
        if (entries[i]) {
            int status = cfs_bf_symlink_load(context->store, entries[i],
                context->storage, &context->workspace->symlink[i]);
            if (status != CFS_BS_OK) {
                return status;
            }
        }
    }

    if (before == NULL || before->inode.size != after->inode.size ||
        memcmp(context->workspace->symlink[0].bytes,
            context->workspace->symlink[1].bytes, (size_t)after->inode.size) != 0) {
        cfs_bf_change_t change = {.kind = CFS_BF_CHANGE_SYMLINK, .id = context->id,
            .bytes = context->workspace->symlink[1].bytes,
            .length = (uint32_t)after->inode.size};
        return cfs_bf_diff_deliver(context, &change);
    }

    return CFS_BS_OK;
}

static int visit_inode(void *opaque, const cfs_bt_record_t *before,
    const cfs_bs_ref_t *before_child, const cfs_bt_record_t *after,
    const cfs_bs_ref_t *after_child) {
    cfs_bf_diff_context_t *context = opaque;
    cfs_bf_entry_t decoded[2];
    context->workspace->work.inode_candidates++;
    int status = decode_inode(before, before_child, context->view[0], &decoded[0]);
    if (status == CFS_BS_OK) {
        status = decode_inode(after, after_child, context->view[1], &decoded[1]);
    }

    if (status != CFS_BS_OK) {
        return finish(context, status);
    }

    const cfs_bf_entry_t *left = before != NULL && decoded[0].inode.links != 0u ?
        &decoded[0] : NULL;
    const cfs_bf_entry_t *right = after != NULL && decoded[1].inode.links != 0u ?
        &decoded[1] : NULL;
    if (left == NULL && right == NULL) {
        return CFS_BT_SCAN_CONTINUE;
    }

    if (left != NULL && right != NULL && left->inode.type != right->inode.type) {
        return finish(context, CFS_BS_ECORRUPT);
    }

    context->id = right == NULL ? left->id : right->id;
    if (left == NULL || right == NULL ||
        memcmp(before->value, after->value, CFS_BF_INODE_BYTES) != 0) {
        cfs_bf_change_t change = {.kind = CFS_BF_CHANGE_INODE, .id = context->id,
            .before = left == NULL ? NULL : &left->inode,
            .after = right == NULL ? NULL : &right->inode};
        status = cfs_bf_diff_deliver(context, &change);
        if (status != CFS_BS_OK || context->stopped) {
            return finish(context, status);
        }
    }

    uint16_t type = right == NULL ? left->inode.type : right->inode.type;
    if (type == CFS_BF_DIRECTORY) {
        status = directories(context, left, right);
    } else if (type == CFS_BF_REGULAR) {
        status = cfs_bf_diff_file(context, left, right);
    } else {
        status = symlinks(context, left, right);
    }

    return finish(context, status);
}

int cfs_bf_diff(cfs_bs_store_t *store, const cfs_bf_view_t *before,
    const cfs_bf_view_t *after, cfs_bf_diff_visit_fn visit, void *context,
    cfs_bt_diff_yield_fn yield,
    cfs_bt_scan_result_t *out, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *namespace_workspace, cfs_bf_diff_workspace_t *workspace) {
    if (before == NULL || after == NULL || visit == NULL || out == NULL ||
        workspace == NULL || !cfs_bf_id_valid(before->root_id) ||
        !cfs_bf_id_valid(after->root_id) || before->revision == 0u ||
        after->revision == 0u) {
        return CFS_BS_EINVAL;
    }

    int status = cfs_bf_prepare(store, storage, namespace_workspace);
    if (status != CFS_BS_OK) {
        return status;
    }

    memset(&workspace->work, 0, sizeof(workspace->work));
    cfs_bf_diff_context_t scan = {.store = store, .view = {before, after},
        .storage = storage, .namespace_workspace = namespace_workspace,
        .workspace = workspace, .visit = visit, .context = context, .yield = yield};
    cfs_bt_scan_result_t result;
    status = cfs_bt_diff(store, before->inodes, after->inodes, visit_inode,
        &scan, yield == NULL ? NULL : cfs_bf_diff_checkpoint, &result,
        storage, &workspace->inodes);
    workspace->work.tree_nodes_loaded += workspace->inodes.work.nodes_loaded;
    workspace->work.subtrees_skipped += workspace->inodes.work.subtrees_skipped;
    if (status != CFS_BS_OK || scan.status != CFS_BS_OK) {
        return scan.status != CFS_BS_OK ? scan.status : status;
    }

    *out = (cfs_bt_scan_result_t){.count = workspace->work.events_delivered,
        .complete = !scan.stopped};
    return CFS_BS_OK;
}
