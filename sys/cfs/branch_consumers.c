/* SPDX-License-Identifier: ISC
 * branch_consumers.c -- bounded inode holds and nonreused handle tokens.
 */
#include "branch_consumers.h"
#include "xxh3_64_mini.h"

#include <string.h>

int cfs_bcons_init(cfs_bcons_t *table, cfs_bcons_entry_t *entries,
    uint32_t entry_count, cfs_bcons_slot_t *slots, uint32_t slot_count) {
    if (table == NULL || entries == NULL || slots == NULL ||
        slot_count < CFS_BCONS_MIN_HANDLES ||
        slot_count > CFS_BCONS_MAX_HANDLES ||
        (slot_count & (slot_count - 1u)) != 0u ||
        entry_count != 2u * slot_count) {
        return CFS_BS_EINVAL;
    }

    memset(entries, 0, (size_t)entry_count * sizeof(*entries));
    memset(slots, 0, (size_t)slot_count * sizeof(*slots));
    for (uint32_t i = 0u; i < slot_count; i++) {
        slots[i].next = i + 1u;
    }

    slots[slot_count - 1u].next = CFS_BCONS_NONE;
    *table = (cfs_bcons_t){.entries = entries, .slots = slots,
        .limit = slot_count};
    return CFS_BS_OK;
}

uint32_t cfs_bcons_probe(const cfs_bcons_t *table, cfs_bf_id_t id, uint32_t n) {
    if (table == NULL || !cfs_bf_id_valid(id) || n >= CFS_BCONS_MAX_PROBES) {
        return CFS_BCONS_NONE;
    }

    uint64_t key[2] = {id.birth, id.ordinal};
    uint32_t mask = 2u * table->limit - 1u;

    return ((uint32_t)bfs_xxh3_64(key, sizeof(key)) + n) & mask;
}

uint32_t cfs_bcons_find(cfs_bcons_t *table, cfs_bf_id_t id, int vacant) {
    if (table == NULL || !cfs_bf_id_valid(id)) {
        return CFS_BCONS_NONE;
    }

    uint64_t key[2] = {id.birth, id.ordinal};
    uint32_t mask = 2u * table->limit - 1u;
    uint32_t first = (uint32_t)bfs_xxh3_64(key, sizeof(key)) & mask;
    uint32_t free_slot = CFS_BCONS_NONE;
    for (uint32_t n = 0u; n < CFS_BCONS_MAX_PROBES; n++) {
        uint32_t index = (first + n) & mask;
        cfs_bcons_entry_t *entry = &table->entries[index];
        table->probes++;
        if (entry->references != 0u && cfs_bf_id_equal(entry->id, id)) {
            return index;
        }

        if (entry->references == 0u && free_slot == CFS_BCONS_NONE) {
            free_slot = index;
        }

        if (entry->id.birth == 0u) {
            break;
        }
    }

    return vacant ? free_slot : CFS_BCONS_NONE;
}

int cfs_bcons_admit(cfs_bcons_t *table, cfs_bf_id_t id, uint64_t sequence,
    uint32_t *consumer) {
    if (table == NULL || consumer == NULL || !cfs_bf_id_valid(id)) {
        return CFS_BS_EINVAL;
    }

    if (table->free_slot == CFS_BCONS_NONE) {
        return CFS_BS_EMFILE;
    }

    if (sequence >= CFS_BCONS_SEQUENCE_MAX) {
        return CFS_BS_EOVERFLOW;
    }

    uint32_t found = cfs_bcons_find(table, id, 1);
    if (found == CFS_BCONS_NONE) {
        return CFS_BS_ENFILE;
    }

    *consumer = found;
    return CFS_BS_OK;
}

uint64_t cfs_bcons_attach(cfs_bcons_t *table, uint64_t *sequence,
    uint32_t consumer, cfs_bf_id_t id, int tag) {
    uint32_t index = table->free_slot;
    cfs_bcons_slot_t *slot = &table->slots[index];
    table->free_slot = slot->next;
    slot->sequence = ++*sequence;
    slot->consumer = consumer;
    slot->tag = tag;
    table->entries[consumer].id = id;
    table->entries[consumer].references++;
    table->handles++;
    return (slot->sequence << 16) | index;
}

cfs_bcons_slot_t *cfs_bcons_handle(cfs_bcons_t *table, uint64_t handle) {
    if (!table) {
        return NULL;
    }

    uint32_t index = (uint32_t)(handle & UINT64_C(65535));
    return index < table->limit && handle >> 16 != 0u &&
        table->slots[index].sequence == handle >> 16 ?
        &table->slots[index] : NULL;
}

int cfs_bcons_release(cfs_bcons_t *table, uint64_t handle) {
    cfs_bcons_slot_t *slot = cfs_bcons_handle(table, handle);
    if (!slot) {
        return CFS_BS_EBADF;
    }

    cfs_bcons_entry_t *consumer = &table->entries[slot->consumer];
    slot->sequence = 0u;
    slot->next = table->free_slot;
    table->free_slot = (uint32_t)(handle & UINT64_C(65535));
    table->handles--;
    consumer->references--;
    return CFS_BS_OK;
}
