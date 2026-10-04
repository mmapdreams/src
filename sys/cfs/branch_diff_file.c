/* SPDX-License-Identifier: ISC
 * branch_diff_file.c -- paired sparse radix paths and logical byte comparison.
 */
#include "branch_diff_internal.h"
#include "branch_file_internal.h"
#include "branch_store_internal.h"

#include <string.h>

enum { FILE_DONE = 0, FILE_NODE = 1, FILE_DATA = 2 };

static cfs_bf_file_frame_t *frame(cfs_bf_diff_file_cursor_t *cursor) {
    return &cursor->read.path[cursor->depth];
}

static uint64_t position(cfs_bf_diff_file_cursor_t *cursor) {
    cfs_bf_file_frame_t *current = frame(cursor);
    return current->base + (cursor->state == FILE_DATA ? current->slot : 0u);
}

static cfs_bs_child_t reference(cfs_bf_diff_file_cursor_t *cursor) {
    cfs_bf_file_frame_t *current = frame(cursor);
    return cursor->state == FILE_DATA ? current->children[current->slot] :
        current->original;
}

static int load(cfs_bf_diff_context_t *context,
    cfs_bf_diff_file_cursor_t *cursor, cfs_bs_child_t child, int level,
    uint64_t base, uint32_t depth) {
    int status = cfs_bf_file_load(context->store, NULL, child, level, base,
        cursor->size, depth == 0u, &cursor->read.path[depth], context->storage,
        &cursor->read);
    if (status == CFS_BS_OK) {
        cursor->depth = depth;
        cursor->state = FILE_NODE;
    }

    return status;
}

static int seek(cfs_bf_diff_context_t *context,
    cfs_bf_diff_file_cursor_t *cursor, uint64_t limit) {
    for (;;) {
        cfs_bf_file_frame_t *current = frame(cursor);
        while (current->slot < CFS_BF_RADIX_SLOTS) {
            context->workspace->work.slots_examined++;
            cfs_bs_child_t child = current->children[current->slot];
            uint64_t base = current->base + (uint64_t)current->slot *
                (UINT64_C(1) << (7u * current->level));
            if (base >= limit) {
                cursor->state = FILE_DONE;
                return CFS_BS_OK;
            }

            if (cfs_bf_file_present(child)) {
                if (current->level == 0u) {
                    cursor->state = FILE_DATA;
                    return CFS_BS_OK;
                }

                return load(context, cursor, child, current->level - 1,
                    base, cursor->depth + 1u);
            }

            current->slot++;
        }

        if (cursor->depth == 0u) {
            cursor->state = FILE_DONE;
            return CFS_BS_OK;
        }

        cursor->depth--;
        frame(cursor)->slot++;
    }
}

static int advance(cfs_bf_diff_context_t *context,
    cfs_bf_diff_file_cursor_t *cursor, uint64_t limit) {
    if (cursor->state == FILE_NODE) {
        if (cursor->depth == 0u) {
            cursor->state = FILE_DONE;
            return CFS_BS_OK;
        }

        cursor->depth--;
    }

    frame(cursor)->slot++;
    return seek(context, cursor, limit);
}

static int compare_data(cfs_bf_diff_context_t *context, int order,
    uint64_t result_size) {
    cfs_bf_diff_file_cursor_t *cursors = context->workspace->file;
    uint64_t number = position(&cursors[order <= 0 ? 0 : 1]);
    uint64_t offset = number * CFS_BF_CHUNK_BYTES;
    uint32_t length = result_size - offset < CFS_BF_CHUNK_BYTES ?
        (uint32_t)(result_size - offset) : CFS_BF_CHUNK_BYTES;
    for (uint32_t i = 0u; i < 2u; i++) {
        cfs_bs_child_t child = (i == 0u ? order <= 0 : order >= 0) ?
            reference(&cursors[i]) : cfs_bf_file_empty();
        int status = cfs_bf_file_chunk(context->store, NULL, child, number,
            cursors[i].size, context->storage, &cursors[i].read);
        if (status != CFS_BS_OK) {
            return status;
        }
    }

    if (memcmp(cursors[0].read.chunk, cursors[1].read.chunk, length) != 0) {
        cfs_bf_change_t change = {.kind = CFS_BF_CHANGE_DATA, .id = context->id,
            .offset = offset, .length = length, .bytes = cursors[1].read.chunk};
        return cfs_bf_diff_deliver(context, &change);
    }

    return CFS_BS_OK;
}

static int walk(cfs_bf_diff_context_t *context, uint64_t result_size) {
    cfs_bf_diff_file_cursor_t *left = &context->workspace->file[0];
    cfs_bf_diff_file_cursor_t *right = &context->workspace->file[1];
    uint64_t limit = cfs_bf_file_chunks(result_size);
    while (!context->stopped &&
        (left->state != FILE_DONE || right->state != FILE_DONE)) {
        if (context->yield != NULL &&
            cfs_bf_diff_checkpoint(context) == CFS_BT_SCAN_STOP) {
            return context->status;
        }

        int order = left->state == FILE_DONE ? 1 : -1;
        if (left->state != FILE_DONE && right->state != FILE_DONE) {
            uint64_t l = position(left);
            uint64_t r = position(right);
            order = l == r ? 0 : l < r ? -1 : 1;
        }

        int status;
        if (order == 0 && left->state == right->state &&
            cfs_bs_ref_equal(reference(left).value.retained,
                reference(right).value.retained)) {
            context->workspace->work.subtrees_skipped++;
            status = advance(context, left, limit);
            if (status == CFS_BS_OK) {
                status = advance(context, right, limit);
            }
        } else if (left->state == FILE_NODE &&
            (order < 0 || (order == 0 && (right->state != FILE_NODE ||
                frame(left)->level >= frame(right)->level)))) {
            status = seek(context, left, limit);
        } else if (right->state == FILE_NODE && order >= 0) {
            status = seek(context, right, limit);
        } else {
            status = compare_data(context, order, result_size);
            if (status == CFS_BS_OK && !context->stopped && order <= 0) {
                status = advance(context, left, limit);
            }

            if (status == CFS_BS_OK && !context->stopped && order >= 0) {
                status = advance(context, right, limit);
            }
        }

        if (status != CFS_BS_OK) {
            return status;
        }
    }

    return CFS_BS_OK;
}

int cfs_bf_diff_file(cfs_bf_diff_context_t *context,
    const cfs_bf_entry_t *before, const cfs_bf_entry_t *after) {
    if (after == NULL || after->inode.size == 0u) {
        return CFS_BS_OK;
    }

    const cfs_bf_entry_t *entries[2] = {before, after};
    int status = CFS_BS_OK;
    for (uint32_t i = 0u; i < 2u; i++) {
        cfs_bf_diff_file_cursor_t *cursor = &context->workspace->file[i];
        memset(&cursor->read.work, 0, sizeof(cursor->read.work));
        cursor->state = FILE_DONE;
        cursor->depth = 0u;
        cursor->size = entries[i] == NULL ? 0u : entries[i]->inode.size;
    }

    for (uint32_t i = 0u; i < 2u && status == CFS_BS_OK; i++) {
        if (entries[i] != NULL && entries[i]->tree.block != CFS_BS_BLOCK_NONE) {
            cfs_bs_child_t child = {.kind = CFS_BS_CHILD_RETAINED,
                .value.retained = entries[i]->tree};
            status = load(context, &context->workspace->file[i], child, -1,
                0u, 0u);
        }
    }

    if (status == CFS_BS_OK) {
        status = walk(context, after->inode.size);
    }

    for (uint32_t i = 0u; i < 2u; i++) {
        cfs_bf_file_work_t *work = &context->workspace->file[i].read.work;
        context->workspace->work.radix_nodes_loaded += work->nodes_loaded;
        context->workspace->work.data_loaded += work->data_loaded;
    }

    return status;
}
