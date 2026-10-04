/* SPDX-License-Identifier: ISC
 *
 * branch_store_construct.c -- all-or-nothing admission and staging of bounded
 * immutable object graphs before a single branch-head publication.
 */
#include "branch_store_internal.h"
#include "branch_store_prepare.h"
#include "branch_store_staging.h"

#include <string.h>

static int plans_validate(const cfs_bs_object_plan_t *plans, uint32_t count,
    const cfs_bs_ref_t *single_children, cfs_bs_workspace_t *workspace) {
    memset(workspace->incoming, 0, sizeof(workspace->incoming));
    for (uint32_t i = 0; i < count; i++) {
        const cfs_bs_object_plan_t *plan = &plans[i];
        size_t payload_offset;

        if (plan->child_count > CFS_BS_MAX_CHILDREN ||
            (plan->child_count != 0 && plan->children == NULL &&
             single_children == NULL) ||
            (plan->payload_length != 0 && plan->payload == NULL)) {
            return CFS_BS_EINVAL;
        }

        payload_offset = CFS_BS_OBJECT_REFS_OFF +
            (size_t)plan->child_count * CFS_BS_REF_SIZE;
        if (plan->payload_length > CFS_BLOCK_SIZE - payload_offset) {
            return CFS_BS_EINVAL;
        }

        if (single_children) {
            continue;
        }

        for (uint32_t edge = 0; edge < plan->child_count; edge++) {
            const cfs_bs_child_t *child = &plan->children[edge];

            if (child->kind == CFS_BS_CHILD_RETAINED) {
                continue;
            }

            if (child->kind != CFS_BS_CHILD_PREPARED ||
                child->value.prepared_index >= i) {
                return CFS_BS_EINVAL;
            }

            workspace->incoming[child->value.prepared_index]++;
        }
    }

    workspace->incoming[count - 1u]++;
    for (uint32_t i = 0; i < count; i++) {
        if (workspace->incoming[i] == 0) {
            return CFS_BS_EINVAL;
        }
    }

    return CFS_BS_OK;
}

static int block_is_prepared(const cfs_bs_tx_t *tx, uint64_t block,
    uint64_t generation) {
    uint64_t page = cfs_bs_row_page(block);

    for (uint32_t i = 0; i < tx->page_count; i++) {
        if (tx->workspace->destination[i] == page) {
            cfs_bs_row_t row;

            cfs_bs_row_decode(tx->workspace->pages[i].bytes +
                CFS_BS_HEADER_SIZE +
                (size_t)cfs_bs_row_index(block) * CFS_BS_ROW_SIZE, &row);
            return (row.incarnation >> CFS_BS_INCARNATION_INDEX_BITS) == generation;
        }
    }

    return 0;
}

static int allocate_plans(cfs_bs_tx_t *tx, cfs_bs_state_t *state,
    uint32_t count) {
    uint64_t generation = tx->store->generation + 1u;
    cfs_bs_row_t row;
    uint8_t *entry;
    int rc;

    if (state->free_count < count || state->free_count < state->reserve) {
        return CFS_BS_ENOSPC;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint64_t block = state->free_head;

        rc = cfs_bs_tx_row(tx, block, 1, &row, &entry);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        if (row.state != CFS_BS_ROW_FREE || row.refcount != 0 ||
            (row.incarnation >> CFS_BS_INCARNATION_INDEX_BITS) >= generation ||
            ((row.next == CFS_BS_BLOCK_NONE) != (state->free_count == 1u)) ||
            row.next == block) {
            return CFS_BS_ECORRUPT;
        }

        state->free_head = row.next;
        state->free_count--;
        row.state = CFS_BS_ROW_LIVE;
        row.refcount = tx->workspace->incoming[i];
        row.incarnation = (generation << CFS_BS_INCARNATION_INDEX_BITS) | i;
        row.next = CFS_BS_BLOCK_NONE;
        cfs_bs_row_encode(entry, &row);
        tx->workspace->prepared[i].block = block;
        tx->workspace->prepared[i].incarnation = row.incarnation;
        tx->workspace->prepared[i].hash = 0;
    }

    if (state->free_count == 0u) {
        return CFS_BS_OK;
    }

    rc = cfs_bs_tx_row(tx, state->free_head, 0, &row, &entry);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (row.state != CFS_BS_ROW_FREE || row.refcount != 0 ||
        (row.incarnation >> CFS_BS_INCARNATION_INDEX_BITS) >= generation ||
        ((row.next == CFS_BS_BLOCK_NONE) != (state->free_count == 1u)) ||
        (row.next != CFS_BS_BLOCK_NONE &&
            (row.next < tx->store->semantic_start ||
                row.next >= tx->store->total_blocks)) ||
        row.next == state->free_head ||
        block_is_prepared(tx, row.next, generation)) {
        return CFS_BS_ECORRUPT;
    }

    return CFS_BS_OK;
}

static int reserve_retained_child(cfs_bs_tx_t *tx, cfs_bs_ref_t child) {
    cfs_bs_row_t row;
    uint8_t *entry;
    int rc;

    if (cfs_bs_ref_is_null(child) ||
        child.block < tx->store->semantic_start ||
        child.block >= tx->store->total_blocks || child.incarnation == 0 ||
        (child.incarnation >> CFS_BS_INCARNATION_INDEX_BITS) > tx->store->generation) {
        return CFS_BS_EINVAL;
    }

    rc = cfs_bs_tx_row(tx, child.block, 1, &row, &entry);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (row.state != CFS_BS_ROW_LIVE || row.incarnation != child.incarnation) {
        return CFS_BS_ENOENT;
    }

    if (row.refcount == UINT64_MAX) {
        return CFS_BS_EOVERFLOW;
    }

    /* Reserve before root release/reaping; keep rows private until commit's
     * pre-write graph validation authenticates every retained payload/hash. */
    row.refcount++;
    cfs_bs_row_encode(entry, &row);
    return CFS_BS_OK;
}

static int ref_same(cfs_bs_ref_t a, cfs_bs_ref_t b) {
    return a.block == b.block && a.incarnation == b.incarnation &&
        a.hash == b.hash;
}

static int edge_marked(const cfs_bs_workspace_t *w, uint32_t plan,
    uint32_t edge) {
    return (w->transferred[plan][edge >> 3] >> (edge & 7u)) & 1u;
}

/* Claim one not-yet-acquired retained edge of one plan naming ref. */
static int claim_in(cfs_bs_workspace_t *w, const cfs_bs_object_plan_t *plan,
    uint32_t index, cfs_bs_ref_t ref) {
    for (uint32_t edge = 0; edge < plan->child_count; edge++) {
        if (plan->children[edge].kind == CFS_BS_CHILD_RETAINED &&
            !edge_marked(w, index, edge) &&
            ref_same(plan->children[edge].value.retained, ref)) {
            w->transferred[index][edge >> 3] |= (uint8_t)(1u << (edge & 7u));
            return 1;
        }
    }

    return 0;
}

static int claim_any(cfs_bs_workspace_t *w, const cfs_bs_object_plan_t *plans,
    uint32_t count, cfs_bs_ref_t ref) {
    for (uint32_t i = 0; i < count; i++) {
        if (claim_in(w, &plans[i], i, ref)) {
            return 1;
        }
    }

    return 0;
}

static int holds_retained(const cfs_bs_object_plan_t *plan) {
    for (uint32_t edge = 0; edge < plan->child_count; edge++) {
        if (plan->children[edge].kind == CFS_BS_CHILD_RETAINED) {
            return 1;
        }
    }

    return 0;
}

/* A candidate copy is an unpaired new object holding a retained edge. */
static int copy_candidate(const cfs_bs_workspace_t *w,
    const cfs_bs_object_plan_t *plans, cfs_bs_child_t child) {
    return child.kind == CFS_BS_CHILD_PREPARED &&
        !w->paired[child.value.prepared_index] &&
        holds_retained(&plans[child.value.prepared_index]);
}

/* Removed edges a release may drop before it costs more than acquiring. */
#define RELEASE_SLACK 32u

/* Count an original's edges its copy keeps, and those neither kept nor
 * continued to a child copy, which a release must drop immediately. */
static uint32_t removed_edges(const cfs_bs_workspace_t *w,
    const cfs_bs_object_plan_t *plans, uint32_t index, const uint8_t *object,
    uint32_t child_count, uint32_t *out_kept) {
    const cfs_bs_object_plan_t *plan = &plans[index];
    uint8_t used[CFS_BS_MAX_CHILDREN / 8u];
    uint32_t kept = 0u;
    uint32_t removed = 0u;

    memcpy(used, w->transferred[index], sizeof(used));
    for (uint32_t c = 0; c < child_count; c++) {
        cfs_bs_ref_t child;
        uint32_t edge = 0;

        cfs_bs_ref_decode(object + CFS_BS_OBJECT_REFS_OFF +
            (size_t)c * CFS_BS_REF_SIZE, &child);
        while (edge < plan->child_count &&
            (plan->children[edge].kind != CFS_BS_CHILD_RETAINED ||
                ((used[edge >> 3] >> (edge & 7u)) & 1u) ||
                !ref_same(plan->children[edge].value.retained, child))) {
            edge++;
        }

        if (edge < plan->child_count) {
            used[edge >> 3] |= (uint8_t)(1u << (edge & 7u));
            kept++;
        } else if (c >= plan->child_count ||
            !copy_candidate(w, plans, plan->children[c])) {
            removed++;
        }
    }

    *out_kept = kept;
    return removed;
}

/* The copy of a replaced child is the parent copy's prepared child at the
 * same slot, else its only candidate; a wrong guess only forgoes transfer. */
static uint32_t pair_for(const cfs_bs_workspace_t *w,
    const cfs_bs_object_plan_t *plans, uint32_t parent, uint32_t edge) {
    const cfs_bs_object_plan_t *plan = &plans[parent];
    uint32_t found = UINT32_MAX;

    if (edge < plan->child_count &&
        copy_candidate(w, plans, plan->children[edge])) {
        return plan->children[edge].value.prepared_index;
    }

    for (uint32_t e = 0; e < plan->child_count; e++) {
        if (copy_candidate(w, plans, plan->children[e])) {
            if (found != UINT32_MAX) {
                return UINT32_MAX;
            }

            found = plan->children[e].value.prepared_index;
        }
    }

    return found;
}

typedef struct releasing {
    cfs_bs_tx_t *tx;
    cfs_bs_state_t *state;
    const cfs_bs_object_plan_t *plans;
    uint32_t count;
    uint32_t queued;
} releasing_t;

static int queue_release(releasing_t *r, cfs_bs_ref_t ref, uint32_t plan) {
    cfs_bs_workspace_t *w = r->tx->workspace;

    if (plan == UINT32_MAX || r->queued == CFS_BS_CONSTRUCT_MAX_OBJECTS) {
        return 0;
    }

    w->paired[plan] = 1u;
    w->releasing[r->queued] = ref;
    w->releasing_plan[r->queued] = (uint16_t)plan;
    r->queued++;
    return 1;
}

static int edge_row(cfs_bs_tx_t *tx, cfs_bs_ref_t child, cfs_bs_row_t *row,
    uint8_t **entry) {
    int rc = cfs_bs_tx_row(tx, child.block, 1, row, entry);

    if (rc == CFS_BS_OK && (row->state != CFS_BS_ROW_LIVE ||
            row->refcount == 0u || row->incarnation != child.incarnation)) {
        rc = CFS_BS_ECORRUPT;
    }

    return rc;
}

static int drop_row(cfs_bs_tx_t *tx, cfs_bs_state_t *state, uint64_t block,
    cfs_bs_row_t *row, uint8_t *entry) {
    row->refcount--;
    if (row->refcount == 0u) {
        return cfs_bs_tx_enqueue(tx, state, block, row, entry);
    }

    cfs_bs_row_encode(entry, row);
    return CFS_BS_OK;
}

/* Release one edge of a retiring original; a child retiring with a copy of
 * its own has its row updated when that copy is processed. */
static int release_edge(releasing_t *r, uint32_t parent_plan, uint32_t edge,
    cfs_bs_ref_t child) {
    cfs_bs_workspace_t *w = r->tx->workspace;
    cfs_bs_row_t row;
    uint8_t *entry;
    int rc = edge_row(r->tx, child, &row, &entry);

    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (row.refcount == 1u && (claim_any(w, r->plans, r->count, child) ||
            queue_release(r, child,
                pair_for(w, r->plans, parent_plan, edge)))) {
        return CFS_BS_OK;
    }

    return drop_row(r->tx, r->state, child.block, &row, entry);
}

/* Retire a released original: kept edges transfer and it queues RELEASED,
 * unless dropping its removed edges now costs more than acquiring. */
static int retire_original(releasing_t *r, cfs_bs_ref_t original,
    uint32_t plan) {
    cfs_bs_workspace_t *w = r->tx->workspace;
    uint8_t *object = w->scratch[2].bytes;
    uint32_t child_count;
    uint32_t payload_length;
    cfs_bs_row_t row;
    uint8_t *entry;
    int rc = cfs_bs_tx_row(r->tx, original.block, 1, &row, &entry);

    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (row.state != CFS_BS_ROW_LIVE || row.refcount != 1u ||
        row.incarnation != original.incarnation) {
        return CFS_BS_ECORRUPT;
    }

    rc = cfs_bs_object_load(r->tx->store, original, object, &child_count,
        &payload_length);
    if (rc != CFS_BS_OK) {
        return rc == CFS_BS_EINVAL ? CFS_BS_ECORRUPT : rc;
    }

    uint32_t kept;
    uint32_t removed = removed_edges(w, r->plans, plan, object, child_count,
        &kept);
    row.refcount = 0u;
    if (removed > kept && removed > RELEASE_SLACK) {
        return cfs_bs_tx_enqueue(r->tx, r->state, original.block, &row,
            entry);
    }

    rc = cfs_bs_tx_enqueue_released(r->tx, r->state, original.block, &row,
        entry);
    for (uint32_t c = 0; rc == CFS_BS_OK && c < child_count; c++) {
        cfs_bs_ref_t child;

        cfs_bs_ref_decode(object + CFS_BS_OBJECT_REFS_OFF +
            (size_t)c * CFS_BS_REF_SIZE, &child);
        if (!claim_in(w, &r->plans[plan], plan, child)) {
            rc = release_edge(r, plan, c, child);
        }
    }

    return rc;
}

/* Release the replaced head's root edge (r0125 7.2). */
static int release_root(cfs_bs_tx_t *tx, cfs_bs_state_t *state,
    const cfs_bs_object_plan_t *plans, uint32_t count, cfs_bs_ref_t root) {
    releasing_t r = {tx, state, plans, count, 0u};
    cfs_bs_row_t row;
    uint8_t *entry;
    int rc = cfs_bs_tx_live_ref(tx, root, tx->workspace->scratch[0].bytes,
        &row, &entry);

    if (rc != CFS_BS_OK || claim_any(tx->workspace, plans, count, root)) {
        return rc;
    }

    if (row.refcount > 1u) {
        row.refcount--;
        cfs_bs_row_encode(entry, &row);
        return CFS_BS_OK;
    }

    /* The queue is empty, so the old root always pairs with the new one. */
    (void)queue_release(&r, root, count - 1u);

    for (uint32_t i = 0; rc == CFS_BS_OK && i < r.queued; i++) {
        rc = retire_original(&r, tx->workspace->releasing[i],
            tx->workspace->releasing_plan[i]);
    }

    return rc;
}

/* A holder that only its donor chain owns, else chained is -1. */
static int holder_load(cfs_bs_tx_t *tx, cfs_bs_ref_t holder, uint8_t *object,
    uint32_t *out_children, int *out_chained) {
    cfs_bs_row_t row;
    uint8_t *entry;
    uint32_t length;
    int rc = cfs_bs_tx_row(tx, holder.block, 0, &row, &entry);

    if (rc == CFS_BS_OK) {
        rc = cfs_bs_object_load(tx->store, holder, object, out_children,
            &length);
    }

    if (rc == CFS_BS_OK) {
        *out_chained = row.state == CFS_BS_ROW_LIVE && row.refcount == 1u &&
            row.incarnation == holder.incarnation ?
            cfs_bs_donor_chained(object, *out_children, length) : -1;
    }

    return rc;
}

/* The catalog slot of this head's donor, if any: an unpublished staging
 * entry bound to the head and revision whose root is a donor holder. */
static int donor_find(cfs_bs_tx_t *tx, const uint8_t *catalog, uint64_t head,
    uint64_t revision, uint32_t *out_slot, cfs_bs_ref_t *out_holder) {
    uint8_t *object = tx->workspace->scratch[2].bytes;

    for (uint32_t slot = 0; slot < CFS_BS_MAX_BRANCHES; slot++) {
        cfs_bs_catalog_entry_t entry;
        uint32_t children;
        int chained;
        int rc;

        if (cfs_bs_catalog_entry_decode_checked(tx->store, catalog, slot, 0,
                &entry) != CFS_BS_OK ||
            entry.kind != CFS_BS_CATALOG_STAGING || entry.origin_id != head ||
            entry.origin_revision != revision ||
            cfs_bs_ref_is_null(entry.root)) {
            continue;
        }

        rc = holder_load(tx, entry.root, object, &children, &chained);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        if (chained >= 0) {
            *out_slot = slot;
            *out_holder = entry.root;
            return CFS_BS_OK;
        }
    }

    return CFS_BS_ENOENT;
}

/* Consume this head's donor (r0125 7.3): held edges transfer to matching
 * retained edges, the rest are released, and its entry leaves the catalog. */
static int donor_consume(cfs_bs_tx_t *tx, cfs_bs_state_t *state,
    uint8_t *catalog, uint64_t head, uint64_t revision,
    const cfs_bs_object_plan_t *plans, uint32_t count, int *out_consumed) {
    uint8_t *object = tx->workspace->scratch[2].bytes;
    cfs_bs_ref_t holder;
    uint32_t slot;
    int rc = donor_find(tx, catalog, head, revision, &slot, &holder);

    *out_consumed = 0;
    if (rc == CFS_BS_ENOENT) {
        return CFS_BS_OK;
    }

    while (rc == CFS_BS_OK && !cfs_bs_ref_is_null(holder)) {
        cfs_bs_ref_t next = {CFS_BS_BLOCK_NONE, 0u, 0u};
        uint32_t children;
        int chained;
        cfs_bs_row_t row;
        uint8_t *row_entry;

        rc = holder_load(tx, holder, object, &children, &chained);
        if (rc == CFS_BS_OK && chained < 0) {
            rc = CFS_BS_ECORRUPT;
        }

        if (rc == CFS_BS_OK) {
            rc = cfs_bs_tx_row(tx, holder.block, 1, &row, &row_entry);
        }

        if (rc == CFS_BS_OK) {
            row.refcount = 0u;
            rc = cfs_bs_tx_enqueue_released(tx, state, holder.block, &row,
                row_entry);
        }

        for (uint32_t c = 0; rc == CFS_BS_OK && c < children; c++) {
            cfs_bs_ref_t child;

            cfs_bs_ref_decode(object + CFS_BS_OBJECT_REFS_OFF +
                (size_t)c * CFS_BS_REF_SIZE, &child);
            if (c == 0u && chained) {
                next = child;
            } else if (!claim_any(tx->workspace, plans, count, child)) {
                rc = edge_row(tx, child, &row, &row_entry);
                if (rc == CFS_BS_OK) {
                    rc = drop_row(tx, state, child.block, &row, row_entry);
                }
            }
        }

        holder = next;
    }

    if (rc == CFS_BS_OK) {
        memset(catalog + CFS_BS_CATALOG_BODY_OFF +
            (size_t)slot * CFS_BS_CATALOG_ENTRY_SIZE, 0,
            CFS_BS_CATALOG_ENTRY_SIZE);
        *out_consumed = 1;
    }

    return rc;
}

static int stage_plan(cfs_bs_tx_t *tx, const cfs_bs_object_plan_t *plan,
    const cfs_bs_ref_t *single_children, uint32_t index) {
    cfs_bs_workspace_t *workspace = tx->workspace;
    int rc;

    for (uint32_t edge = 0; edge < plan->child_count; edge++) {
        cfs_bs_ref_t child;

        if (single_children == NULL &&
            plan->children[edge].kind == CFS_BS_CHILD_PREPARED) {
            workspace->children[edge] = workspace->prepared[
                plan->children[edge].value.prepared_index];
            continue;
        }

        child = single_children != NULL ? single_children[edge] :
            plan->children[edge].value.retained;
        if (single_children == NULL && edge_marked(workspace, index, edge)) {
            workspace->children[edge] = child;
            continue;
        }

        rc = reserve_retained_child(tx, child);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        workspace->children[edge] = child;
    }

    return cfs_bs_object_frame(tx->store, workspace->prepared[index].block,
        workspace->prepared[index].incarnation, workspace->children,
        plan->child_count, plan->payload, plan->payload_length,
        workspace->objects[index].bytes, &workspace->prepared[index]);
}

enum { CFS_BS_PREPARED_ACTIVE = 1 };

static void lease_release(cfs_bs_store_t *store,
    cfs_bs_prepared_construct_t *prepared) {
    store->lease_owner = NULL;
    store->lease_workspace = NULL;
    store->lease_generation = 0u;
    memset(prepared, 0, sizeof(*prepared));
}

static int lease_acquire(cfs_bs_store_t *store, cfs_bs_workspace_t *workspace,
    const cfs_bs_prepared_construct_t *owner) {
    if (store == NULL || workspace == NULL || owner == NULL) {
        return CFS_BS_EINVAL;
    }

    if (store->lease_owner) {
        return CFS_BS_EBUSY;
    }

    store->lease_owner = owner;
    store->lease_workspace = workspace;
    store->lease_generation = store->generation;
    return CFS_BS_OK;
}

static int construct_prepare(cfs_bs_store_t *store, uint64_t branch_id,
    uint64_t expected_revision, uint32_t kind,
    const cfs_bs_object_plan_t *plans,
    uint32_t count, const cfs_bs_ref_t *single_children,
    const void *lease_owner, int replacement,
    cfs_bs_prepared_construct_t *out,
    cfs_bs_workspace_t *workspace) {
    cfs_bs_prepared_construct_t prepared = {0};
    cfs_bs_tx_t tx;
    cfs_bs_state_t state;
    cfs_bs_catalog_entry_t branch;
    uint8_t *state_page;
    uint8_t *catalog;
    uint8_t *pins;
    int donated = 0;
    int rc;

    if (store == NULL || workspace == NULL || plans == NULL || out == NULL ||
        count == 0 || count > CFS_BS_CONSTRUCT_MAX_OBJECTS) {
        return CFS_BS_EINVAL;
    }

    rc = replacement != 0 ?
        cfs_bs_tx_begin_replacement(store, workspace, lease_owner, &tx) :
        cfs_bs_tx_begin_leased(store, workspace, lease_owner, &tx);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = plans_validate(plans, count, single_children, workspace);
    if (rc != CFS_BS_OK) {
        cfs_bs_tx_abort(&tx);
        return rc;
    }

    rc = cfs_bs_tx_load_head(&tx, branch_id, expected_revision, kind,
                                &catalog, &branch);
    if (rc == CFS_BS_OK) {
        rc = cfs_bs_tx_state(&tx, 1, &state, &state_page);
    }

    if (rc == CFS_BS_OK) {
        rc = cfs_bs_tx_get(&tx, CFS_BS_PIN_BLOCK, 0, &pins);
    }

    if (rc == CFS_BS_OK) {
        rc = cfs_bs_catalog_pair_validate(store, state_page, catalog, pins);
    }

    if (rc == CFS_BS_OK && state.next_revision == UINT64_MAX) {
        rc = CFS_BS_EOVERFLOW;
    }

    if (rc == CFS_BS_OK && kind == CFS_BS_CATALOG_STAGING) {
        rc = cfs_bs_tx_check_catalog_ref(&tx, branch.root);
    }

    if (rc == CFS_BS_OK) {
        rc = allocate_plans(&tx, &state, count);
    }

    /* Release the old root first so its retiring edges transfer to the
     * copies below instead of being acquired again (r0125 7.2). */
    memset(workspace->transferred, 0, sizeof(workspace->transferred));
    memset(workspace->paired, 0, sizeof(workspace->paired));
    if (rc == CFS_BS_OK && single_children == NULL) {
        rc = donor_consume(&tx, &state, catalog, branch_id, expected_revision,
            plans, count, &donated);
    }

    if (rc == CFS_BS_OK && single_children == NULL && !donated &&
        !cfs_bs_ref_is_null(branch.root)) {
        rc = release_root(&tx, &state, plans, count, branch.root);
    }

    for (uint32_t i = 0; rc == CFS_BS_OK && i < count; i++) {
        rc = stage_plan(&tx, &plans[i], single_children, i);
    }

    /* A donor already supplied every edge, and the single-object path
     * acquires first; both then release the old root edge plainly. */
    if (rc == CFS_BS_OK && (single_children != NULL || donated) &&
        !cfs_bs_ref_is_null(branch.root)) {
        rc = cfs_bs_tx_ref_drop(&tx, &state, branch.root,
            workspace->scratch[0].bytes);
    }

    /* Spend only predecessor FREE blocks, then restore the reserve. Reclaimed
     * addresses remain unwritten while the old head is recoverable. */
    if (rc == CFS_BS_OK && state.free_count < state.reserve) {
        uint32_t deficit = (uint32_t)(state.reserve - state.free_count);
        uint32_t reaped;
        rc = cfs_bs_tx_reap_prefix(&tx, &state, deficit, &reaped);
        if (rc == CFS_BS_OK && reaped != deficit) {
            rc = state.pending_count == 0u ? CFS_BS_ENOSPC : CFS_BS_EWORKSPACE;
        }
    }

    if (rc != CFS_BS_OK) {
        cfs_bs_tx_abort(&tx);
        return rc;
    }

    branch.revision = state.next_revision++;
    branch.root = workspace->prepared[count - 1u];
    cfs_bs_catalog_encode(catalog + CFS_BS_CATALOG_BODY_OFF +
        (size_t)(branch_id & 15u) * CFS_BS_CATALOG_ENTRY_SIZE, &branch);
    cfs_bs_state_encode(state_page, &state);
    rc = cfs_bs_tx_prepare(&tx, count);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    memset(&prepared, 0, sizeof(prepared));
    prepared.store = store;
    prepared.workspace = workspace;
    prepared.branch_id = branch_id;
    prepared.base_revision = expected_revision;
    prepared.generation = store->generation + 1u;
    prepared.root = branch.root;
    prepared.revision = branch.revision;
    prepared.page_count = tx.page_count;
    prepared.object_count = count;
    prepared.state = CFS_BS_PREPARED_ACTIVE;
    *out = prepared;
    return CFS_BS_OK;
}

static int prepared_matches(const cfs_bs_store_t *store,
    const cfs_bs_prepared_construct_t *prepared) {
    if (store == NULL || prepared == NULL || store->lease_owner != prepared) {
        return 0;
    }

    return prepared->store == store &&
        prepared->workspace == store->lease_workspace &&
        prepared->branch_id != 0u && prepared->base_revision != 0u &&
        prepared->state == CFS_BS_PREPARED_ACTIVE &&
        prepared->reserved == 0u && prepared->page_count != 0u &&
        prepared->page_count <= CFS_BS_TX_MAX_PAGES &&
        prepared->object_count != 0u &&
        prepared->object_count <= CFS_BS_CONSTRUCT_MAX_OBJECTS &&
        store->generation == store->lease_generation &&
        prepared->generation == store->lease_generation + 1u &&
        prepared->revision > prepared->base_revision &&
        cfs_bs_ref_equal(prepared->root,
            prepared->workspace->prepared[prepared->object_count - 1u]);
}

int cfs_bs_branch_construct_prepare(cfs_bs_store_t *store,
    uint64_t branch_id, uint64_t expected_revision,
    const cfs_bs_object_plan_t *plans, uint32_t plan_count,
    cfs_bs_prepared_construct_t *out, cfs_bs_workspace_t *workspace) {
    cfs_bs_prepared_construct_t prepared = {0};
    int rc;

    if (store == NULL || workspace == NULL || plans == NULL || out == NULL ||
        plan_count == 0u || plan_count > CFS_BS_CONSTRUCT_MAX_OBJECTS) {
        return CFS_BS_EINVAL;
    }

    rc = lease_acquire(store, workspace, out);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = construct_prepare(store, branch_id, expected_revision,
        CFS_BS_CATALOG_BRANCH, plans, plan_count, NULL, out, 0, &prepared,
        workspace);
    if (rc != CFS_BS_OK) {
        store->lease_owner = NULL;
        store->lease_workspace = NULL;
        store->lease_generation = 0u;
        return rc;
    }

    *out = prepared;
    return CFS_BS_OK;
}

int cfs_bs_branch_construct_replace_prepare(cfs_bs_store_t *store,
    cfs_bs_prepared_construct_t *current,
    const cfs_bs_object_plan_t *plans, uint32_t plan_count,
    cfs_bs_prepared_construct_t *out,
    cfs_bs_workspace_t *alternate_workspace) {
    cfs_bs_prepared_construct_t prepared = {0};
    cfs_bs_tx_t prior_tx;
    int rc;

    if (!prepared_matches(store, current) || plans == NULL || out == NULL ||
        alternate_workspace == NULL || out == current ||
        alternate_workspace == current->workspace || plan_count == 0u ||
        plan_count > CFS_BS_CONSTRUCT_MAX_OBJECTS) {
        return CFS_BS_EINVAL;
    }

    rc = construct_prepare(store, current->branch_id,
        current->base_revision, CFS_BS_CATALOG_BRANCH, plans, plan_count,
        NULL, current, 1, &prepared, alternate_workspace);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    *out = prepared;
    store->lease_owner = out;
    store->lease_workspace = alternate_workspace;
    memset(&prior_tx, 0, sizeof(prior_tx));
    prior_tx.workspace = current->workspace;
    prior_tx.page_count = current->page_count;
    prior_tx.active = 1u;
    cfs_bs_tx_abort(&prior_tx);
    memset(current, 0, sizeof(*current));
    return CFS_BS_OK;
}

int cfs_bs_branch_construct_publish(cfs_bs_store_t *store,
    cfs_bs_prepared_construct_t *prepared, cfs_bs_ref_t *out_root,
    uint64_t *out_revision) {
    cfs_bs_tx_t tx;
    cfs_bs_ref_t root;
    uint64_t revision;
    int rc;

    if (out_root == NULL || out_revision == NULL ||
        !prepared_matches(store, prepared)) {
        return CFS_BS_EINVAL;
    }

    root = prepared->root;
    revision = prepared->revision;
    memset(&tx, 0, sizeof(tx));
    tx.store = store;
    tx.workspace = prepared->workspace;
    tx.page_count = prepared->page_count;
    tx.active = 1u;
    rc = cfs_bs_tx_publish(&tx, prepared->object_count);
    lease_release(store, prepared);
    if (rc == CFS_BS_OK) {
        *out_root = root;
        *out_revision = revision;
    }

    return rc;
}

int cfs_bs_branch_construct_cancel(cfs_bs_store_t *store,
    cfs_bs_prepared_construct_t *prepared) {
    cfs_bs_tx_t tx;

    if (!prepared_matches(store, prepared)) {
        return CFS_BS_EINVAL;
    }

    memset(&tx, 0, sizeof(tx));
    tx.workspace = prepared->workspace;
    tx.page_count = prepared->page_count;
    tx.active = 1u;
    cfs_bs_tx_abort(&tx);
    lease_release(store, prepared);
    return CFS_BS_OK;
}

static int construct_sync(cfs_bs_store_t *store, uint64_t branch_id,
    uint64_t expected_revision, uint32_t kind,
    const cfs_bs_object_plan_t *plans, uint32_t count,
    const cfs_bs_ref_t *single_children, cfs_bs_ref_t *out_root,
    uint64_t *out_revision, cfs_bs_workspace_t *workspace) {
    cfs_bs_prepared_construct_t prepared = {0};
    int rc;

    if (out_root == NULL || out_revision == NULL) {
        return CFS_BS_EINVAL;
    }

    rc = lease_acquire(store, workspace, &prepared);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = construct_prepare(store, branch_id, expected_revision, kind, plans,
        count, single_children, &prepared, 0, &prepared, workspace);
    if (rc != CFS_BS_OK) {
        store->lease_owner = NULL;
        store->lease_workspace = NULL;
        store->lease_generation = 0u;
        return rc;
    }

    return cfs_bs_branch_construct_publish(store, &prepared, out_root,
        out_revision);
}

int cfs_bs_staging_store(cfs_bs_store_t *store, uint64_t id,
    uint64_t expected_revision, const cfs_bs_ref_t *children,
    uint32_t child_count, const void *payload, uint32_t payload_length,
    cfs_bs_ref_t *out_root, uint64_t *out_revision,
    cfs_bs_workspace_t *workspace) {
    cfs_bs_object_plan_t plan = {0};

    plan.child_count = child_count;
    plan.payload = payload;
    plan.payload_length = payload_length;
    return construct_sync(store, id, expected_revision,
        CFS_BS_CATALOG_STAGING, &plan, 1, children, out_root, out_revision,
        workspace);
}

int cfs_bs_branch_construct(cfs_bs_store_t *store, uint64_t branch_id,
    uint64_t expected_revision, const cfs_bs_object_plan_t *plans,
    uint32_t plan_count, cfs_bs_ref_t *out_root, uint64_t *out_revision,
    cfs_bs_workspace_t *workspace) {
    return construct_sync(store, branch_id, expected_revision,
        CFS_BS_CATALOG_BRANCH, plans, plan_count, NULL, out_root,
        out_revision, workspace);
}

int cfs_bs_staging_construct(cfs_bs_store_t *store, uint64_t id,
    uint64_t expected_revision, const cfs_bs_object_plan_t *plans,
    uint32_t plan_count, cfs_bs_ref_t *out_root, uint64_t *out_revision,
    cfs_bs_workspace_t *workspace) {
    int rc = construct_sync(store, id, expected_revision,
        CFS_BS_CATALOG_STAGING, plans, plan_count, NULL, out_root,
        out_revision, workspace);

    /* Staging plans predict no identities, so the same plans may follow a
     * donor; a second refusal cancels it. */
    if (rc == CFS_BS_EWORKSPACE && cfs_bs_branch_unshare(store, id,
            expected_revision, plans, plan_count, workspace) == CFS_BS_OK) {
        rc = construct_sync(store, id, expected_revision,
            CFS_BS_CATALOG_STAGING, plans, plan_count, NULL, out_root,
            out_revision, workspace);
        if (rc != CFS_BS_OK) {
            (void)cfs_bs_branch_unshare_cancel(store, id, workspace);
        }
    }

    return rc;
}

int cfs_bs_branch_store(cfs_bs_store_t *store, uint64_t branch_id,
    uint64_t expected_revision, const cfs_bs_ref_t *children,
    uint32_t child_count, const void *payload, uint32_t payload_length,
    cfs_bs_ref_t *out_root, uint64_t *out_revision,
    cfs_bs_workspace_t *workspace) {
    cfs_bs_object_plan_t plan;

    if (child_count != 0 && children == NULL) {
        return CFS_BS_EINVAL;
    }

    memset(&plan, 0, sizeof(plan));
    plan.child_count = child_count;
    plan.payload = payload;
    plan.payload_length = payload_length;
    return construct_sync(store, branch_id, expected_revision,
        CFS_BS_CATALOG_BRANCH, &plan, 1, children, out_root, out_revision,
        workspace);
}
