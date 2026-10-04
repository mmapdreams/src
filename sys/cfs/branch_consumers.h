/* SPDX-License-Identifier: ISC
 * branch_consumers.h -- caller-owned branch-local inode and token lifetimes.
 */
#ifndef CFS_BRANCH_CONSUMERS_H
#define CFS_BRANCH_CONSUMERS_H

#include "branch_fs.h"

#define CFS_BCONS_MIN_HANDLES 32u
#define CFS_BCONS_MAX_HANDLES 32768u
#define CFS_BCONS_MAX_PROBES 64u
#define CFS_BCONS_SEQUENCE_MAX (UINT64_MAX >> 16)
#define CFS_BCONS_NONE UINT32_MAX


typedef struct cfs_bcons_entry {
    cfs_bf_id_t id;
    uint32_t references;
} cfs_bcons_entry_t;

typedef struct cfs_bcons_slot {
    uint64_t sequence;
    uint32_t next;
    uint32_t consumer;
    int tag; /* Uninterpreted adapter data; never a core permission policy. */
} cfs_bcons_slot_t;

typedef struct cfs_bcons {
    cfs_bcons_entry_t *entries;
    cfs_bcons_slot_t *slots;
    uint64_t probes;
    uint32_t limit;
    uint32_t free_slot;
    uint32_t handles;
} cfs_bcons_t;

/* Disjoint caller storage outlives all calls: 32..32768 power-of-two slots,
 * twice as many entries. Invalid shapes preserve state; see module note. */
int cfs_bcons_init(cfs_bcons_t *table, cfs_bcons_entry_t *entries,
    uint32_t entry_count, cfs_bcons_slot_t *slots, uint32_t slot_count);

/* Later calls require a successfully initialized table and serialization.
 * Never reset the shared sequence during an image-owner lifetime. */

uint32_t cfs_bcons_find(cfs_bcons_t *table, cfs_bf_id_t id, int vacant);

/* The n-th entry index the find for id probes, n < CFS_BCONS_MAX_PROBES. */
uint32_t cfs_bcons_probe(const cfs_bcons_t *table, cfs_bf_id_t id, uint32_t n);

/* A valid namespace identity is required. Failure preserves the output and
 * admissions; a bounded lookup can still advance the probe counter. */
int cfs_bcons_admit(cfs_bcons_t *table, cfs_bf_id_t id, uint64_t sequence,
    uint32_t *consumer);

/* Use admitted table/identity/consumer/sequence with no intervening mutation.
 * sequence is non-null, image-wide, and nonreused; keep image serialization. */
uint64_t cfs_bcons_attach(cfs_bcons_t *table, uint64_t *sequence,
    uint32_t consumer, cfs_bf_id_t id, int tag);

/* Borrow until the next table mutation or unlock. Do not modify slot fields. */
cfs_bcons_slot_t *cfs_bcons_handle(cfs_bcons_t *table, uint64_t handle);

/* Consume one token without I/O; invalid/stale tokens return CFS_BS_EBADF.
 * Durable orphan cleanup belongs to image service after final release. */
int cfs_bcons_release(cfs_bcons_t *table, uint64_t handle);

#endif /* CFS_BRANCH_CONSUMERS_H */
