/* SPDX-License-Identifier: ISC
 *
 * cfs_kimage.c -- open, service and close of one OpenBSD kernel image owner.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/atomic.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/vnode.h>

#include <uvm/uvm_extern.h>

#include "cfs_kimage.h"

#define CFS_ERROR_POSIX(name) name

int cfs_kimage_errno(int status) {
    switch (status) {
#define CFS_ERROR_STATUS_OK(symbol, value) case symbol: return 0;
#define CFS_ERROR_STATUS_NEGATIVE(symbol, value, posix, description) \
    case symbol: return posix;
#include "error_catalog/branch_store.def"
#undef CFS_ERROR_STATUS_NEGATIVE
#undef CFS_ERROR_STATUS_OK
    default:
        return EIO;
    }
}

/* Large arrays come from kernel_map, not the bounded malloc arena. */
static void *big_alloc(size_t size, int wait) {
    return km_alloc(round_page(size), &kv_any, &kp_zero,
        wait ? &kd_waitok : &kd_nowait);
}

static void big_free(void *ptr, size_t size) {
    if (ptr) {
        km_free(ptr, round_page(size), &kv_any, &kp_zero);
    }
}

static size_t cache_slot_bytes(uint32_t blocks) {
    return (size_t)blocks * sizeof(cfs_bs_cache_slot_t);
}

static size_t cache_block_bytes(uint32_t blocks) {
    return (size_t)blocks * CFS_BLOCK_SIZE;
}

static size_t entry_bytes(void) {
    return 2u * CFS_KIMAGE_CONSUMERS * sizeof(cfs_bcons_entry_t);
}

static size_t slot_bytes(void) {
    return CFS_KIMAGE_CONSUMERS * sizeof(cfs_bcons_slot_t);
}

static int cache_shape(uint32_t blocks) {
    return blocks == 0u || (blocks >= 4u &&
        blocks <= CFS_KIMAGE_CACHE_MAX_BLOCKS &&
        (blocks & (blocks - 1u)) == 0u);
}

static void release(cfs_kimage_t *image) {
    big_free(image->cache_slots, cache_slot_bytes(image->cache_blocks));
    big_free(image->cache_bytes, cache_block_bytes(image->cache_blocks));
    big_free(image->consumers.entries, entry_bytes());
    big_free(image->consumers.slots, slot_bytes());
    big_free(image->work, sizeof(*image->work));
}

/* A failed cache allocation leaves the image uncached, as the hosted owner. */
static int attach_cache(cfs_kimage_t *image, uint32_t blocks) {
    if (blocks == 0u) {
        return CFS_BS_OK;
    }

    image->cache_slots = big_alloc(cache_slot_bytes(blocks), 0);
    image->cache_bytes = big_alloc(cache_block_bytes(blocks), 0);
    if (image->cache_slots == NULL || image->cache_bytes == NULL) {
        big_free(image->cache_slots, cache_slot_bytes(blocks));
        big_free(image->cache_bytes, cache_block_bytes(blocks));
        image->cache_slots = NULL;
        image->cache_bytes = NULL;
        printf("cfs: block cache of %u blocks unavailable\n", blocks);
        return CFS_BS_OK;
    }

    image->cache_blocks = blocks;
    return cfs_bs_cache_attach(&image->store, &image->cache,
        image->cache_slots, image->cache_bytes, blocks);
}

int cfs_kimage_head(cfs_kimage_t *image) {
    if (image->store.poisoned) {
        return EIO;
    }

    if (image->head_generation == image->store.generation &&
        image->view.revision != 0u &&
        image->view.revision == image->branch.revision) {
        return 0;
    }

    int status = cfs_bs_branch_get(&image->store, image->branch_id,
        &image->branch, &image->work->storage);
    if (status == CFS_BS_OK) {
        status = cfs_bf_read(&image->store, image->branch.root,
            image->branch.revision, &image->view, &image->work->storage,
            &image->work->ns);
    }

    if (status == CFS_BS_OK) {
        image->head_generation = image->store.generation;
    }

    return cfs_kimage_errno(status);
}

int cfs_kimage_open(cfs_kimage_t *image, struct vnode *devvp,
    struct ucred *cred, struct proc *proc, uint64_t branch,
    uint32_t cache_blocks, int read_only) {
    if (image == NULL || !cache_shape(cache_blocks)) {
        return EINVAL;
    }

    memset(image, 0, sizeof(*image));
    int error = cfs_kprov_backend_open(&image->backend, devvp, cred, proc,
        read_only);
    if (error != 0) {
        return error;
    }

    image->work = big_alloc(sizeof(*image->work), 1);
    image->consumers.entries = big_alloc(entry_bytes(), 1);
    image->consumers.slots = big_alloc(slot_bytes(), 1);
    int status = image->work == NULL || image->consumers.entries == NULL ||
        image->consumers.slots == NULL ? CFS_BS_EWORKSPACE :
        cfs_bs_open(&image->store, &image->backend.block,
            &image->work->storage);
    if (status == CFS_BS_OK) {
        status = attach_cache(image, cache_blocks);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bw_init(&image->working, &image->store,
            &image->work->working);
    }

    if (status == CFS_BS_OK) {
        cfs_bcons_entry_t *entries = image->consumers.entries;
        cfs_bcons_slot_t *slots = image->consumers.slots;
        status = cfs_bcons_init(&image->consumers, entries,
            2u * CFS_KIMAGE_CONSUMERS, slots, CFS_KIMAGE_CONSUMERS);
    }

    error = image->backend.write_refused ? EROFS : cfs_kimage_errno(status);
    if (error == 0) {
        image->branch_id = branch;
        error = cfs_kimage_head(image);
    }

    if (error != 0) {
        release(image);
        (void)cfs_kprov_backend_abort(&image->backend, proc);
        memset(image, 0, sizeof(*image));
        return error;
    }

    image->ready = image->view.orphans.block == CFS_BS_BLOCK_NONE;
    image->open = 1;
    return 0;
}

int cfs_kimage_checkpoint(cfs_kimage_t *image, uint64_t through_sequence) {
    return cfs_kimage_errno(cfs_bw_checkpoint(&image->working,
        through_sequence));
}

int cfs_kimage_flush(cfs_kimage_t *image) {
    return cfs_kimage_errno(cfs_bw_flush(&image->working));
}

int cfs_kimage_mutation_begin(cfs_kimage_t *image) {
    uint64_t sequence;

    return cfs_bw_external_begin(&image->working, &sequence);
}

int cfs_kimage_mutation_end(cfs_kimage_t *image, int raw_status) {
    int status = cfs_bw_external_end(&image->working, raw_status);
    if (status == CFS_BS_EINVAL && raw_status != CFS_BS_EINVAL) {
        panic("cfs: unbalanced external mutation");
    }

    return status;
}

/* Release up to one batch of orphans no consumer holds, halving on a
 * workspace refusal; the cursor never passes an unreleased candidate. */
static int orphan_batch(cfs_kimage_t *image, int *out_more) {
    cfs_bf_id_t ids[CFS_BF_ORPHAN_BATCH_MAX];
    cfs_bf_id_t cursor = image->orphan_cursor;
    uint32_t count = 0u;
    int exhausted = 0;

    for (uint32_t n = 0u; n < CFS_BF_ORPHAN_BATCH_MAX; n++) {
        cfs_bf_id_t id;
        int status = cfs_bf_orphan_next(&image->store, &image->view, cursor,
            &id, &image->work->storage, &image->work->ns);
        if (status == CFS_BS_ENOENT) {
            exhausted = 1;
            break;
        }

        if (status != CFS_BS_OK) {
            return cfs_kimage_errno(status);
        }

        cursor = id;
        if (cfs_bcons_find(&image->consumers, id, 0) == CFS_BCONS_NONE) {
            ids[count++] = id;
        }
    }

    uint32_t selected = count;
    while (selected != 0u) {
        int raw = cfs_kimage_mutation_begin(image);
        if (raw == CFS_BS_OK) {
            raw = cfs_bf_orphan_reap_batch(&image->store, image->branch_id,
                image->branch.revision, ids, selected,
                &image->branch.revision, &image->work->storage,
                &image->work->ns);
            raw = cfs_kimage_mutation_end(image, raw);
        }

        if ((raw == CFS_BS_ENOSPC || raw == CFS_BS_EWORKSPACE) &&
            selected > 1u) {
            selected /= 2u;
        } else if (raw != CFS_BS_OK) {
            return cfs_kimage_errno(raw);
        } else {
            break;
        }
    }

    image->orphan_cursor = selected < count ? ids[selected - 1u] :
        exhausted ? (cfs_bf_id_t){0} : cursor;
    *out_more = !exhausted || selected < count;
    return 0;
}

#define CFS_KIMAGE_RECLAIM_STEPS 64u

int cfs_kimage_reclaim(cfs_kimage_t *image) {
    for (uint32_t step = 0u; step < CFS_KIMAGE_RECLAIM_STEPS; step++) {
        int more = 0;
        int error = cfs_kimage_reap_step(image, CFS_BS_REAP_MAX_OBJECTS,
            &more);
        if (error != 0) {
            return error;
        }

        if (!more) {
            return 0;
        }
    }

    return 0;
}

int cfs_kimage_space_errno(int status) {
    return status == CFS_BS_EWORKSPACE ? ENOSPC : cfs_kimage_errno(status);
}

int cfs_kimage_reap_step(cfs_kimage_t *image, uint32_t limit,
    int *out_more) {
    int orphans = 0;
    uint32_t reaped = 0u;

    if (image == NULL || out_more == NULL) {
        return EINVAL;
    }

    if (limit > CFS_BS_REAP_MAX_OBJECTS) {
        limit = CFS_BS_REAP_MAX_OBJECTS;
    }

    int error = cfs_kimage_flush(image);
    if (error == 0) {
        error = cfs_kimage_head(image);
    }

    if (error == 0 && image->view.orphans.block != CFS_BS_BLOCK_NONE) {
        error = orphan_batch(image, &orphans);
    }

    if ((error == 0 || error == ENOSPC || error == ENOMEM) && limit != 0u) {
        int raw = cfs_kimage_mutation_begin(image);
        if (raw == CFS_BS_OK) {
            raw = cfs_bs_reap(&image->store, limit, &reaped,
                &image->work->storage);
            raw = cfs_kimage_mutation_end(image, raw);
        }

        if (raw != CFS_BS_OK) {
            error = cfs_kimage_errno(raw);
        }
    }

    if (error == 0) {
        error = cfs_kimage_head(image);
    }

    if (error == 0 && image->view.orphans.block == CFS_BS_BLOCK_NONE) {
        image->ready = 1;
        image->orphan_cursor = (cfs_bf_id_t){0};
        orphans = 0;
    }

    *out_more = cfs_kimage_reap_more(error, orphans, limit, reaped);
    return error;
}

int cfs_kimage_stats(cfs_kimage_t *image, cfs_kimage_stats_t *out) {
    cfs_bs_stats_t space;

    if (image == NULL || out == NULL) {
        return EINVAL;
    }

    if (image->store.poisoned) {
        return EIO;
    }

    int error = cfs_kimage_errno(cfs_bs_inspect_stats(&image->store, &space,
        &image->work->storage));
    if (error == 0) {
        *out = (cfs_kimage_stats_t){
            .blocks = space.total_blocks - space.semantic_start,
            .free = space.free_count,
            .available = space.free_count > space.reserve ?
                space.free_count - space.reserve : 0u,
        };
    }

    return error;
}

static int delayed_error(cfs_kimage_t *image) {
    cfs_bw_status_t status;

    if (cfs_bw_status(&image->working, &status) != CFS_BS_OK) {
        return EIO;
    }

    return cfs_kimage_errno(status.delayed_error);
}

/* Drain orphans no consumer holds; a held one stays for the next open. */
static int drain(cfs_kimage_t *image) {
    int more = 1;
    int error = 0;

    for (uint32_t step = 0u; more && error == 0 && step < 65536u; step++) {
        error = cfs_kimage_reap_step(image, 0u, &more);
    }

    return error == ENOSPC || error == ENOMEM ? 0 : error;
}

int cfs_kimage_close(cfs_kimage_t *image, int force) {
    if (image == NULL || !image->open) {
        return EINVAL;
    }

    if (!force && image->consumers.handles != 0u) {
        return EBUSY;
    }

    int error = delayed_error(image);
    int raw = CFS_BS_EPOISON;
    if (!force && !image->backend.read_only && !image->store.poisoned) {
        int flushed = cfs_kimage_flush(image);
        if (flushed == 0) {
            flushed = drain(image);
        }

        raw = cfs_bw_shutdown(&image->working);
        if (error == 0) {
            error = flushed != 0 ? flushed : cfs_kimage_errno(raw);
        }
    }

    if (raw != CFS_BS_OK && cfs_bw_abort(&image->working) != CFS_BS_OK) {
        printf("cfs: working owner abort failed\n");
    }

    if (error == 0 && image->store.poisoned) {
        error = EIO;
    }

    release(image);
    int closed = cfs_kprov_backend_close(&image->backend, curproc);
    memset(image, 0, sizeof(*image));
    return error != 0 ? error : closed;
}

void *cfs_kimage_bounce_get(void) {
    return malloc(CFS_BF_IO_MAX, M_MISCFSMNT, M_WAITOK);
}

void cfs_kimage_bounce_put(void *bounce) {
    free(bounce, M_MISCFSMNT, CFS_BF_IO_MAX);
}

void cfs_kimage_sizes(void) {
    static volatile unsigned int printed;

    if (atomic_swap_uint(&printed, 1u) != 0u) {
        return;
    }

    size_t cache = cache_slot_bytes(CFS_KIMAGE_CACHE_BLOCKS) +
        cache_block_bytes(CFS_KIMAGE_CACHE_BLOCKS);
    size_t work = round_page(sizeof(cfs_kimage_work_t));
    size_t consumers = round_page(entry_bytes()) + round_page(slot_bytes());
    printf("cfs: image %zu work %zu (bw %zu bs %zu bf %zu file %zu "
        "path %zu)\n", sizeof(cfs_kimage_t), sizeof(cfs_kimage_work_t),
        sizeof(cfs_bw_workspace_t), sizeof(cfs_bs_workspace_t),
        sizeof(cfs_bf_workspace_t), sizeof(cfs_bf_file_workspace_t),
        sizeof(cfs_bf_path_workspace_t));
    printf("cfs: cache %u blocks %zu consumers %u %zu bounce %u "
        "total %zu\n", CFS_KIMAGE_CACHE_BLOCKS, cache, CFS_KIMAGE_CONSUMERS,
        consumers, CFS_BF_IO_MAX, work + cache + consumers);
}
