/* SPDX-License-Identifier: ISC
 *
 * branch_store_device.c -- poisoned branch-store provider access and fixed
 * home-address classification.
 */

#include "branch_store_internal.h"

#include "block.h"

#include <string.h>

#define CACHE_WAYS 4u

int cfs_bs_cache_attach(cfs_bs_store_t *store, cfs_bs_cache_t *cache,
    cfs_bs_cache_slot_t *slots, uint8_t *bytes, uint32_t slot_count) {
    if (store == NULL || cache == NULL || slots == NULL || bytes == NULL ||
        slot_count < CACHE_WAYS || (slot_count & (slot_count - 1u)) != 0u) {
        return CFS_BS_EINVAL;
    }

    cache->slots = slots;
    cache->bytes = bytes;
    cache->slot_count = slot_count;
    store->cache = cache;
    cfs_bs_cache_invalidate(store);
    return CFS_BS_OK;
}

void cfs_bs_cache_invalidate(cfs_bs_store_t *store) {
    cfs_bs_cache_t *cache = store != NULL ? store->cache : NULL;

    if (!cache) {
        return;
    }

    for (uint32_t i = 0; i < cache->slot_count; i++) {
        cache->slots[i].block = CFS_BS_BLOCK_NONE;
        cache->slots[i].stamp = 0u;
        cache->slots[i].validated = 0u;
    }

    cache->clock = 0u;
}

static uint8_t *cache_bytes(const cfs_bs_cache_t *cache, uint32_t index) {
    return cache->bytes + (size_t)index * CFS_BLOCK_SIZE;
}

static uint32_t cache_set(const cfs_bs_cache_t *cache, uint64_t block) {
    uint64_t mixed = block * UINT64_C(0x9e3779b97f4a7c15);

    return ((uint32_t)(mixed >> 32) & (cache->slot_count / CACHE_WAYS - 1u)) *
        CACHE_WAYS;
}

/* The way holding block, or the set's empty or least recently used way. */
static uint32_t cache_way(const cfs_bs_cache_t *cache, uint64_t block,
    int *out_found) {
    uint32_t base = cache_set(cache, block);
    uint32_t chosen = base;

    for (uint32_t way = base; way < base + CACHE_WAYS; way++) {
        if (cache->slots[way].block == block) {
            *out_found = 1;
            return way;
        }

        if (cache->slots[chosen].block != CFS_BS_BLOCK_NONE &&
            (cache->slots[way].block == CFS_BS_BLOCK_NONE ||
                cache->slots[way].stamp < cache->slots[chosen].stamp)) {
            chosen = way;
        }
    }

    *out_found = 0;
    return chosen;
}

int cfs_bs_io_read_slot(cfs_bs_store_t *store, uint64_t block, uint8_t *out,
    cfs_bs_cache_slot_t **out_slot) {
    cfs_bs_cache_t *cache;
    uint32_t way = 0u;
    int found = 0;

    if (out_slot) {
        *out_slot = NULL;
    }

    if (store == NULL || out == NULL || block >= store->total_blocks) {
        return CFS_BS_EINVAL;
    }

    if (store->poisoned != 0) {
        return CFS_BS_EPOISON;
    }

    cache = store->cache;
    if (cache) {
        way = cache_way(cache, block, &found);
    }

    if (found && cache->verify == 0u) {
        memcpy(out, cache_bytes(cache, way), CFS_BLOCK_SIZE);
    } else if (cfs_block_read(&store->backend, block, 1, out) != CFS_BLK_OK) {
        store->poisoned = 1;
        return CFS_BS_EIO;
    }

    if (!cache) {
        return CFS_BS_OK;
    }

    cfs_bs_cache_slot_t *slot = &cache->slots[way];
    if (!found || (cache->verify != 0u &&
            memcmp(out, cache_bytes(cache, way), CFS_BLOCK_SIZE) != 0)) {
        cache->mismatches += found;
        memcpy(cache_bytes(cache, way), out, CFS_BLOCK_SIZE);
        slot->block = block;
        slot->validated = 0u;
    }

    cache->hits += found;
    cache->misses += !found;
    slot->stamp = ++cache->clock;
    if (out_slot) {
        *out_slot = slot;
    }

    return CFS_BS_OK;
}

int cfs_bs_io_read(cfs_bs_store_t *store, uint64_t block, uint8_t *out) {
    return cfs_bs_io_read_slot(store, block, out, NULL);
}

/* Cached copies follow every write; a failed write leaves media unknown. */
static void cache_written(cfs_bs_store_t *store, uint64_t block,
    uint32_t count, const uint8_t *data, int written) {
    cfs_bs_cache_t *cache = store->cache;

    for (uint32_t i = 0; cache != NULL && i < count; i++) {
        int found;
        uint32_t way = cache_way(cache, block + i, &found);

        if (!found) {
            continue;
        }

        if (written) {
            memcpy(cache_bytes(cache, way), data + (size_t)i * CFS_BLOCK_SIZE,
                CFS_BLOCK_SIZE);
        } else {
            cache->slots[way].block = CFS_BS_BLOCK_NONE;
        }

        cache->slots[way].validated = 0u;
    }
}

int cfs_bs_io_write(cfs_bs_store_t *store, uint64_t block,
    const uint8_t *data) {
    return cfs_bs_io_write_blocks(store, block, 1u, data);
}

int cfs_bs_io_write_blocks(cfs_bs_store_t *store, uint64_t block,
    uint32_t count, const void *data) {
    if (store == NULL || data == NULL || count == 0u ||
        block >= store->total_blocks || count > store->total_blocks - block) {
        return CFS_BS_EINVAL;
    }

    if (store->poisoned != 0) {
        return CFS_BS_EPOISON;
    }

    if (cfs_block_write(&store->backend, block, count, data) != CFS_BLK_OK) {
        cache_written(store, block, count, data, 0);
        store->poisoned = 1;
        return CFS_BS_EIO;
    }

    cache_written(store, block, count, data, 1);
    return CFS_BS_OK;
}

int cfs_bs_io_sync(cfs_bs_store_t *store) {
    if (!store) {
        return CFS_BS_EINVAL;
    }

    if (store->poisoned != 0) {
        return CFS_BS_EPOISON;
    }

    if (cfs_block_sync(&store->backend) != CFS_BLK_OK) {
        store->poisoned = 1;
        return CFS_BS_EIO;
    }

    return CFS_BS_OK;
}

uint32_t cfs_bs_home_kind(const cfs_bs_store_t *store, uint64_t block) {
    if (!store) {
        return 0;
    }

    if (block == CFS_BS_STATE_BLOCK) {
        return CFS_BS_KIND_STATE;
    }

    if (block == CFS_BS_BRANCH_BLOCK || block == CFS_BS_PIN_BLOCK) {
        return CFS_BS_KIND_CATALOG;
    }

    if (block >= CFS_BS_ROW_START &&
        block < store->semantic_start - CFS_BS_REDO_BLOCKS) {
        return CFS_BS_KIND_ROWS;
    }

    return 0;
}

int cfs_bs_read_home(cfs_bs_store_t *store, uint64_t block, uint8_t *out) {
    cfs_bs_cache_slot_t *slot;
    uint32_t kind;
    int rc;

    kind = cfs_bs_home_kind(store, block);
    if (kind == 0) {
        return CFS_BS_EINVAL;
    }

    rc = cfs_bs_io_read_slot(store, block, out, &slot);
    if (rc != CFS_BS_OK || (slot && (slot->validated & 0xffu) == kind)) {
        return rc;
    }

    rc = cfs_bs_header_validate(out, kind, block, store->uuid, NULL);
    if (rc == CFS_BS_OK && slot != NULL) {
        slot->validated = kind;
    }

    return rc;
}
