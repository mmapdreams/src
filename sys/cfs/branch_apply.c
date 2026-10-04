/* SPDX-License-Identifier: ISC
 * branch_apply.c -- bounded receive, hidden assembly, validation, and cleanup.
 */
#include "branch_apply.h"

#include <string.h>

static int same_ref(cfs_bs_ref_t a, cfs_bs_ref_t b) {
    return a.block == b.block && a.incarnation == b.incarnation &&
        a.hash == b.hash;
}

static int mutation_valid(cfs_bf_mutation_observer_t mutations) {
    return (mutations.begin == NULL) == (mutations.end == NULL);
}

static int mutation_begin(cfs_ba_apply_t *a) {
    return a->mutations.begin == NULL ? CFS_BS_OK :
        a->mutations.begin(a->mutations.context);
}

static void mutation_end(cfs_ba_apply_t *a, int status) {
    if (a->mutations.end) {
        a->mutations.end(a->mutations.context, status);
    }
}

static int reap(cfs_ba_apply_t *a, uint32_t *out) {
    *out = 0u;
    cfs_bs_state_t state;
    int status = cfs_bs_inspect_state(a->store, &state, &a->storage);
    if (status != CFS_BS_OK || state.pending_count == 0u) {
        return status;
    }

    status = mutation_begin(a);
    if (status == CFS_BS_OK) {
        status = cfs_bs_reap(a->store, CFS_BS_REAP_MAX_OBJECTS, out,
            &a->storage);
        mutation_end(a, status);
    }

    return status;
}

static int checkpoint(cfs_ba_apply_t *a) {
    if (a->store->poisoned) {
        return CFS_BS_EPOISON;
    }

    int stopped = a->progress != NULL ? a->progress(a->context) : 0;
    return a->store->poisoned ? CFS_BS_EPOISON :
        stopped != 0 ? CFS_BS_EINVAL : CFS_BS_OK;
}

static int assemble(cfs_ba_apply_t *a, const cfs_bf_change_t *change) {
    int status = checkpoint(a);
    if (status == CFS_BS_OK) {
        status = cfs_bf_assemble_change(a->store, &a->assembly, change,
            &a->storage, &a->work.assembly);
    }

    if (status == CFS_BS_OK && ++a->since_cleanup == 7u) {
        status = checkpoint(a);
        if (status == CFS_BS_OK) {
            uint32_t reaped;
            status = reap(a, &reaped);
        }

        a->since_cleanup = 0u;
    }

    return status;
}

static int flush_data(cfs_ba_apply_t *a) {
    if (a->pending.length == 0u) {
        return CFS_BS_OK;
    }

    int status = assemble(a, &a->pending);
    a->pending.length = 0u;
    return status;
}

static int consume(cfs_ba_apply_t *a, const cfs_bf_change_t *change) {
    int adjacent = change->kind == CFS_BF_CHANGE_DATA &&
        cfs_bf_id_equal(change->id, a->pending.id) &&
        change->offset >= a->pending.offset &&
        change->offset - a->pending.offset == a->pending.length;
    if (a->pending.length != 0u && (!adjacent ||
            change->length > CFS_BF_IO_MAX - a->pending.length)) {
        int status = flush_data(a);
        if (status != CFS_BS_OK) {
            return status;
        }
    }

    if (change->kind != CFS_BF_CHANGE_DATA) {
        return assemble(a, change);
    }

    if (a->pending.length == 0u) {
        a->pending = (cfs_bf_change_t){.kind = CFS_BF_CHANGE_DATA,
            .id = change->id, .offset = change->offset, .bytes = a->data};
    }

    /* Decoder data is borrowed and at most one 4000-byte wire record. */
    memcpy(a->data + a->pending.length, change->bytes, change->length);
    a->pending.length += change->length;
    return a->pending.length == CFS_BF_IO_MAX ? flush_data(a) : CFS_BS_OK;
}

static int visit(void *context, const cfs_bf_change_header_t *header,
    const cfs_bf_change_t *change) {
    cfs_ba_apply_t *a = context;
    a->status = checkpoint(a);
    if (a->status != CFS_BS_OK) {
        return a->status;
    }

    if (!change) {
        cfs_bs_state_t state;
        a->status = cfs_bs_inspect_state(a->store, &state, &a->storage);
        if (a->status == CFS_BS_OK && (header->base != a->base.revision ||
                memcmp(header->uuid, a->store->uuid, CFS_BS_UUID_SIZE) != 0 ||
                header->result >= state.next_revision ||
                !cfs_bf_id_equal(header->root_before, a->view.root_id))) {
            a->status = CFS_BS_ECONFLICT;
        }

        if (a->status == CFS_BS_OK) {
            a->status = mutation_begin(a);
        }

        if (a->status == CFS_BS_OK) {
            a->status = cfs_bs_staging_begin(a->store, a->base.id,
                a->base.revision, &a->stage, &a->storage);
            mutation_end(a, a->status);
        }

        if (a->status == CFS_BS_OK) {
            a->status = checkpoint(a);
        }

        if (a->status == CFS_BS_OK) {
            a->status = cfs_bf_assemble_begin(a->store, a->stage.id,
                a->stage.revision, header, &a->assembly, &a->storage,
                &a->work.assembly, a->mutations);
        }
    } else {
        /* Image write authority admits stored metadata. No event-data pointer
         * survives this call, and failed feeds never retry. */
        a->status = consume(a, change);
    }

    return a->status;
}

static void initialize(cfs_ba_apply_t *a, cfs_bs_store_t *store,
    uint64_t dest, uint64_t expected, uint64_t input_limit,
    cfs_bf_validate_yield_fn progress, void *context,
    cfs_bf_mutation_observer_t mutations) {
    memset(a, 0, sizeof(*a));
    a->store = store;
    a->dest = dest;
    a->expected = expected;
    a->input_limit = input_limit;
    a->progress = progress;
    a->context = context;
    a->mutations = mutations;
}

int cfs_ba_begin(cfs_ba_apply_t *a, cfs_bs_store_t *store,
    uint64_t dest, uint64_t expected, uint64_t input_limit,
    cfs_bf_validate_yield_fn progress, void *context,
    cfs_bf_mutation_observer_t mutations) {
    if (a == NULL || store == NULL || dest == 0u || expected == 0u ||
        input_limit == 0u || !mutation_valid(mutations)) {
        return CFS_BS_EINVAL;
    }

    initialize(a, store, dest, expected, input_limit, progress, context,
        mutations);
    a->status = cfs_bs_branch_get(store, dest, &a->base, &a->storage);
    if (a->status == CFS_BS_OK && a->base.revision != expected) {
        a->status = CFS_BS_ECONFLICT;
    }

    if (a->status == CFS_BS_OK) {
        a->status = cfs_bf_read(store, a->base.root, a->base.revision,
            &a->view, &a->storage, &a->work.assembly.namespace);
    }

    if (a->status == CFS_BS_OK) {
        a->status = cfs_bf_change_start(&a->decoder, visit, a);
    }

    return a->status;
}

int cfs_ba_begin_admitted(cfs_ba_apply_t *a, cfs_bs_store_t *store,
    const cfs_bs_branch_t *base, const cfs_bf_view_t *view, uint64_t expected,
    uint64_t input_limit, cfs_bf_validate_yield_fn progress, void *context,
    cfs_bf_mutation_observer_t mutations) {
    if (a == NULL || store == NULL || base == NULL || view == NULL ||
        base->id == 0u || expected == 0u || input_limit == 0u ||
        !mutation_valid(mutations)) {
        return CFS_BS_EINVAL;
    }

    initialize(a, store, base->id, expected, input_limit, progress, context,
        mutations);
    a->base = *base;
    a->view = *view;
    a->status = store->poisoned ? CFS_BS_EPOISON :
        base->revision != expected || view->revision != base->revision ?
            CFS_BS_ECONFLICT : cfs_bf_change_start(&a->decoder, visit, a);
    return a->status;
}

int cfs_ba_feed(cfs_ba_apply_t *a, const void *bytes, uint32_t count) {
    if (a == NULL || a->store == NULL) {
        return CFS_BS_EINVAL;
    }

    if (a->status != CFS_BS_OK) {
        return a->status;
    }

    if (a->finished || a->published || (bytes == NULL && count != 0u)) {
        a->status = CFS_BS_EINVAL;
    } else if (count > a->input_limit - a->input_bytes) {
        a->status = CFS_BS_EWORKSPACE;
    } else {
        a->input_bytes += count;
        int status = cfs_bf_change_feed(&a->decoder, bytes, count);
        if (a->status == CFS_BS_OK) {
            a->status = status;
        }
    }

    return a->status;
}

int cfs_ba_finish(cfs_ba_apply_t *a, cfs_bf_validate_inode_t *inodes,
    size_t capacity, uint64_t validation_budget) {
    if (a == NULL || a->store == NULL) {
        return CFS_BS_EINVAL;
    }

    if (a->status != CFS_BS_OK) {
        return a->status;
    }

    if (a->finished || a->published) {
        return a->status = CFS_BS_EINVAL;
    }

    a->status = cfs_bf_change_finish(&a->decoder, &a->stream);
    if (a->status == CFS_BS_OK) {
        a->status = flush_data(a);
    }

    if (a->status == CFS_BS_OK) {
        a->status = checkpoint(a);
    }

    if (a->status == CFS_BS_OK) {
        a->status = cfs_bf_assemble_finish(a->store, &a->assembly, &a->stage,
            &a->storage, &a->work.assembly);
    }

    if (a->status == CFS_BS_OK) {
        /* Assembly is finished; only its durable candidate is needed now. */
        a->status = cfs_bf_validate(a->store, a->stage.root, a->stage.revision,
            inodes, capacity, validation_budget, a->progress, a->context,
            &a->validated, &a->storage, &a->work.validation);
    }

    cfs_bs_staging_t current;
    if (a->status == CFS_BS_OK) {
        a->status = cfs_bs_staging_get(a->store, a->stage.id, &current,
            &a->storage);
    }

    if (a->status == CFS_BS_OK && (current.revision != a->validated.revision ||
            current.destination != a->dest ||
            current.expected_destination != a->expected ||
            !same_ref(current.root, a->validated.root) ||
            memcmp(a->validated.uuid, a->stream.header.uuid,
                CFS_BS_UUID_SIZE) != 0 ||
            !cfs_bf_id_equal(a->validated.root_id,
                a->stream.header.root_after))) {
        a->status = CFS_BS_ECONFLICT;
    }

    if (a->status == CFS_BS_OK) {
        a->stage = current;
        a->finished = 1;
    }

    return a->status;
}

int cfs_ba_publish(cfs_ba_apply_t *a, cfs_bs_branch_t *out) {
    if (a == NULL || a->store == NULL || out == NULL) {
        return CFS_BS_EINVAL;
    }

    if (a->status != CFS_BS_OK) {
        return a->status;
    }

    if (!a->finished || a->published) {
        return a->status = CFS_BS_EINVAL;
    }

    a->status = mutation_begin(a);
    if (a->status == CFS_BS_OK) {
        a->publication_attempted = 1;
        a->status = cfs_bs_staging_publish(a->store, a->stage.id,
            a->stage.revision, out, &a->storage);
        if (a->status == CFS_BS_OK) {
            a->published = 1;
        }

        mutation_end(a, a->status);
    }

    return a->status;
}

void cfs_ba_cleanup(cfs_ba_apply_t *a, cfs_ba_cleanup_t *out) {
    *out = (cfs_ba_cleanup_t){.disposition = CFS_BA_CLEANUP_NONE};
    if (a->published) {
        return;
    }

    if (a->store->poisoned) {
        out->disposition = CFS_BA_CLEANUP_UNKNOWN;
    } else if (a->stage.id != 0u) {
        cfs_bs_staging_t stage;
        out->disposition = CFS_BA_CLEANUP_RETAINED;
        out->status = cfs_bs_staging_get(a->store, a->stage.id, &stage,
            &a->storage);
        if (out->status == CFS_BS_OK) {
            out->status = mutation_begin(a);
        }

        if (out->status == CFS_BS_OK) {
            out->status = cfs_bs_staging_cancel(a->store, stage.id,
                stage.revision, &a->storage);
            mutation_end(a, out->status);
        }

        if (out->status == CFS_BS_OK) {
            out->disposition = CFS_BA_CLEANUP_RELEASED;
            out->status = reap(a, &out->reaped);
        } else if (a->store->poisoned) {
            out->disposition = CFS_BA_CLEANUP_UNKNOWN;
        }
    }
}
