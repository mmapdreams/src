/* SPDX-License-Identifier: ISC
 * branch_change_order.c -- per-inode wire order and local change consistency.
 */
#include "branch_change_internal.h"

#include <string.h>

static int compare_id(cfs_bf_id_t left, cfs_bf_id_t right) {
    if (left.birth != right.birth) {
        return left.birth < right.birth ? -1 : 1;
    }

    return left.ordinal < right.ordinal ? -1 : left.ordinal != right.ordinal;
}

int cfs_bf_change_group_end(const cfs_bf_change_group_t *group) {
    if (group->kind == CFS_BF_CHANGE_ENTRY || group->type == CFS_BF_DIRECTORY) {
        uint64_t before = group->size[0];
        uint64_t after = group->size[1];
        if (!group->inode_seen) {
            return group->added == group->removed ?
                CFS_BS_OK : CFS_BS_ECORRUPT;
        }

        if (group->added >= group->removed) {
            uint64_t growth = group->added - group->removed;
            if (before > UINT64_MAX - growth || after != before + growth) {
                return CFS_BS_ECORRUPT;
            }
        } else if (before < group->removed - group->added ||
            after != before - (group->removed - group->added)) {
            return CFS_BS_ECORRUPT;
        }
    }

    if (group->type == CFS_BF_SYMLINK && group->present[1] &&
        (!group->present[0] || group->size[0] != group->size[1]) &&
        !group->symlink_seen) {
        return CFS_BS_ECORRUPT;
    }

    return CFS_BS_OK;
}

static int inode(cfs_bf_change_decoder_t *decoder) {
    cfs_bf_change_group_t *group = &decoder->group;
    const cfs_bf_change_t *change = &decoder->change;
    if (group->inode_seen || group->kind != 0u) {
        return CFS_BS_ECORRUPT;
    }

    const cfs_bf_inode_t *sides[2] = {change->before, change->after};
    uint64_t revisions[2] = {decoder->receipt.header.base,
        decoder->receipt.header.result};
    for (uint32_t i = 0u; i < 2u; i++) {
        if (sides[i]) {
            if (change->id.birth > revisions[i] ||
                sides[i]->parent.birth > revisions[i]) {
                return CFS_BS_ECORRUPT;
            }

            group->type = sides[i]->type;
            group->present[i] = 1u;
            group->size[i] = sides[i]->size;
        }
    }

    group->inode_seen = 1u;
    return CFS_BS_OK;
}

static int entry(cfs_bf_change_decoder_t *decoder) {
    cfs_bf_change_group_t *group = &decoder->group;
    const cfs_bf_change_t *change = &decoder->change;
    uint32_t shorter = change->length < group->name_length ?
        change->length : group->name_length;
    int order = memcmp(change->bytes, group->name, shorter);
    if (group->name_length != 0u && (order < 0 ||
            (order == 0 && change->length <= group->name_length))) {
        return CFS_BS_ECORRUPT;
    }

    uint64_t births[2] = {change->before_id.birth, change->after_id.birth};
    uint64_t revisions[2] = {decoder->receipt.header.base,
        decoder->receipt.header.result};
    for (uint32_t i = 0u; i < 2u; i++) {
        if (births[i] > revisions[i] || (births[i] != 0u &&
                (change->id.birth > revisions[i] ||
                    (group->inode_seen && !group->present[i])))) {
            return CFS_BS_ECORRUPT;
        }
    }

    if (births[0] == 0u) {
        if (group->added == UINT64_MAX) {
            return CFS_BS_EOVERFLOW;
        }

        group->added++;
    } else if (births[1] == 0u) {
        if (group->removed == UINT64_MAX) {
            return CFS_BS_EOVERFLOW;
        }

        group->removed++;
    }

    memcpy(group->name, change->bytes, change->length);
    group->name_length = (uint16_t)change->length;
    return CFS_BS_OK;
}

int cfs_bf_change_order(cfs_bf_change_decoder_t *decoder) {
    cfs_bf_change_group_t *group = &decoder->group;
    const cfs_bf_change_t *change = &decoder->change;
    int order = compare_id(change->id, group->id);
    if (order < 0 || (order > 0 &&
            cfs_bf_change_group_end(group) != CFS_BS_OK)) {
        return CFS_BS_ECORRUPT;
    }

    if (order > 0) {
        memset(group, 0, sizeof(*group));
        group->id = change->id;
    }

    if (change->kind == CFS_BF_CHANGE_INODE) {
        return inode(decoder);
    }

    uint16_t type = change->kind == CFS_BF_CHANGE_ENTRY ? CFS_BF_DIRECTORY :
        change->kind == CFS_BF_CHANGE_DATA ? CFS_BF_REGULAR : CFS_BF_SYMLINK;
    if ((group->inode_seen && group->type != type) ||
        (group->kind != 0u && group->kind != change->kind)) {
        return CFS_BS_ECORRUPT;
    }

    group->kind = change->kind;
    if (change->kind == CFS_BF_CHANGE_ENTRY) {
        return entry(decoder);
    }

    if (change->id.birth > decoder->receipt.header.result ||
        (!group->inode_seen && change->id.birth > decoder->receipt.header.base) ||
        (group->inode_seen && !group->present[1])) {
        return CFS_BS_ECORRUPT;
    }

    if (change->kind == CFS_BF_CHANGE_DATA) {
        if (change->offset < group->data_end || (group->inode_seen &&
                (change->offset > group->size[1] ||
                    change->length > group->size[1] - change->offset))) {
            return CFS_BS_ECORRUPT;
        }

        group->data_end = change->offset + change->length;
    } else {
        if (group->symlink_seen || (group->inode_seen &&
                change->length != group->size[1])) {
            return CFS_BS_ECORRUPT;
        }

        group->symlink_seen = 1u;
    }

    return CFS_BS_OK;
}
