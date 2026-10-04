/* SPDX-License-Identifier: ISC
 *
 * cfs_kimage.h -- one OpenBSD kernel image owner: the partition backend,
 * the branch store and its block cache, the shared working owner, the
 * consumer table and the selected branch head.
 *
 * Serialization: every function except cfs_kimage_sizes and the bounce
 * helpers is called with the mount's owner lock held exclusively, or before
 * the mount is published. Nothing here takes a lock of its own; the device
 * sync takes the device vnode lock, so the order is owner lock -> devvp.
 * All functions sleep: process context only. Results are errnos.
 *
 * Open takes a referenced, unlocked VBLK devvp, owned by the image on
 * success and still the caller's on failure. cache_blocks is 0 (uncached)
 * or a power of two in 4..CFS_KIMAGE_CACHE_MAX_BLOCKS. A missing branch is
 * ENOENT. Failure leaves the image zeroed with nothing allocated. A
 * read_only image never writes the device; it is EROFS when recovery would.
 *
 * Close unforced is EBUSY while consumers are held; otherwise it flushes,
 * drains orphans and shuts down, aborting if shutdown fails. Forced and
 * read-only close abort without durability. Both free everything, so the
 * image is gone even when they return the first delayed error, which is
 * only for logging.
 */
#ifndef CFS_OPENBSD_KIMAGE_H
#define CFS_OPENBSD_KIMAGE_H

#include <stddef.h>
#include <stdint.h>

#include "branch_consumers.h"
#include "branch_orphan.h"
#include "branch_path.h"
#include "branch_working.h"
#include "cfs_kprov.h"

/* Same default as the hosted owner: 16384 slots, 64 MiB of block bytes. */
#define CFS_KIMAGE_CACHE_BLOCKS 16384u
#define CFS_KIMAGE_CACHE_MAX_BLOCKS 1048576u

/* Live consumer holds, as the FUSE mount; twice as many table entries. */
#define CFS_KIMAGE_CONSUMERS 32768u

/* Engine scratch too large for a mount structure; one allocation per image.
 * The working workspace must not move while the image is open. */
typedef struct cfs_kimage_work {
    cfs_bs_workspace_t storage;
    cfs_bf_workspace_t ns;
    cfs_bf_file_workspace_t file;
    cfs_bf_path_workspace_t path;
    cfs_bw_workspace_t working;
} cfs_kimage_work_t;

typedef struct cfs_kimage {
    cfs_kprov_backend_t backend;
    cfs_bs_store_t store;
    cfs_bs_cache_t cache;
    cfs_bs_cache_slot_t *cache_slots;
    uint8_t *cache_bytes;
    uint32_t cache_blocks;
    cfs_kimage_work_t *work;
    cfs_bw_t working;
    cfs_bcons_t consumers;
    uint64_t consumer_sequence;
    uint64_t branch_id;
    cfs_bs_branch_t branch;
    cfs_bf_view_t view;
    uint64_t head_generation;
    cfs_bf_id_t orphan_cursor;
    int ready; /* No orphan backlog remains from before this open. */
    int open;
} cfs_kimage_t;

int cfs_kimage_open(cfs_kimage_t *image, struct vnode *devvp,
    struct ucred *cred, struct proc *proc, uint64_t branch,
    uint32_t cache_blocks, int read_only);

int cfs_kimage_close(cfs_kimage_t *image, int force);

/* Publish accepted work through through_sequence, or everything. */
int cfs_kimage_checkpoint(cfs_kimage_t *image, uint64_t through_sequence);
int cfs_kimage_flush(cfs_kimage_t *image);

/* Refresh branch and view after any store generation change. */
int cfs_kimage_head(cfs_kimage_t *image);

/* Bracket one raw store mutation outside the working owner. Unlike the rest,
 * both use raw CFS_BS_* status; end returns the mutation's real outcome. */
int cfs_kimage_mutation_begin(cfs_kimage_t *image);
int cfs_kimage_mutation_end(cfs_kimage_t *image, int raw_status);

/* One service step: flush, release up to one batch of unheld orphans, then
 * at most limit physical objects. *out_more says another step has work. */
int cfs_kimage_reap_step(cfs_kimage_t *image, uint32_t limit, int *out_more);

/* The reaper may stop short of limit before a large object, so any progress
 * re-arms the service; an idle step (nothing reaped) ends it. */
static inline int cfs_kimage_reap_more(int error, int orphans, uint32_t limit,
    uint32_t reaped) {
    return error == 0 && (orphans || (limit != 0u && reaped != 0u));
}

/* Space in CFS_BLOCK_SIZE units for statfs; the engine counts no inodes. */
typedef struct cfs_kimage_stats {
    uint64_t blocks;
    uint64_t free;
    uint64_t available; /* Free beyond the admission reserve. */
} cfs_kimage_stats_t;

int cfs_kimage_stats(cfs_kimage_t *image, cfs_kimage_stats_t *out);

/* Branch-store status to errno. */
int cfs_kimage_errno(int status);

/* Space held by pending reclamation: drain bounded reap steps so that a
 * refused allocation can retry. Returns 0 or an errno. Owner lock held. */
int cfs_kimage_reclaim(cfs_kimage_t *image);

/* An allocation status to errno: a workspace refusal that survives
 * reclamation means no admissible space, ENOSPC, never ENOMEM. */
int cfs_kimage_space_errno(int status);

/* A CFS_BF_IO_MAX data buffer per call; never one shared under the lock. */
void *cfs_kimage_bounce_get(void);
void cfs_kimage_bounce_put(void *bounce);

/* Print every per-image allocation size to the console, once per boot. */
void cfs_kimage_sizes(void);

#endif /* CFS_OPENBSD_KIMAGE_H */
