/* SPDX-License-Identifier: ISC
 *
 * branch_store_io.c -- home validation and bounded transaction publication.
 */

#include "branch_store_internal.h"

#include <string.h>

#include "codec.h"

_Static_assert(sizeof(cfs_bs_page_t) == CFS_BLOCK_SIZE,
    "page arrays must be byte-contiguous for block transfers");


int cfs_bs_validate_home_page(const cfs_bs_store_t *store, uint64_t self,
    const uint8_t *page, uint64_t *out_generation) {
    uint32_t kind;
    int rc;

    kind = cfs_bs_home_kind(store, self);
    if (kind == 0) {
        return CFS_BS_ECORRUPT;
    }

    rc = cfs_bs_header_validate(page, kind, self, store->uuid,
                                out_generation);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (self == CFS_BS_STATE_BLOCK) {
        cfs_bs_state_t state;

        rc = cfs_bs_state_page_decode(store, page, &state);
    } else if (self == CFS_BS_BRANCH_BLOCK || self == CFS_BS_PIN_BLOCK) {
        rc = cfs_bs_catalog_page_validate(store, page,
                                          self == CFS_BS_PIN_BLOCK);
    } else if (kind == CFS_BS_KIND_ROWS) {
        rc = cfs_bs_row_page_validate(store, self, page);
    } else {
        return CFS_BS_ECORRUPT;
    }

    return rc;
}


static int tx_find(const cfs_bs_tx_t *tx, uint64_t block,
    uint32_t *out_index) {
    uint32_t i;

    for (i = 0; i < tx->page_count; i++) {
        if (tx->workspace->loaded[i] != 0 &&
            tx->workspace->destination[i] == block) {
            *out_index = i;
            return 1;
        }
    }

    return 0;
}

static int tx_begin(cfs_bs_store_t *store, cfs_bs_workspace_t *workspace,
    const void *lease_owner, int replacement, cfs_bs_tx_t *out) {
    if (store == NULL || workspace == NULL || out == NULL) {
        return CFS_BS_EINVAL;
    }

    if (store->lease_owner != NULL && store->lease_owner != lease_owner) {
        return CFS_BS_EBUSY;
    }

    if (lease_owner) {
        if (store->lease_owner != lease_owner ||
            store->lease_generation != store->generation) {
            return CFS_BS_EINVAL;
        }

        if ((replacement == 0 && store->lease_workspace != workspace) ||
            (replacement != 0 && store->lease_workspace == workspace)) {
            return CFS_BS_EINVAL;
        }
    }

    if (store->poisoned != 0) {
        return CFS_BS_EPOISON;
    }

    if (store->generation == 0) {
        return CFS_BS_ECORRUPT;
    }

    if (store->generation >= CFS_BS_GENERATION_MAX) {
        return CFS_BS_EOVERFLOW;
    }

    memset(workspace->loaded, 0, sizeof(workspace->loaded));
    memset(workspace->dirty, 0, sizeof(workspace->dirty));
    memset(out, 0, sizeof(*out));
    out->store = store;
    out->workspace = workspace;
    out->active = 1;
    return CFS_BS_OK;
}

int cfs_bs_tx_begin(cfs_bs_store_t *store, cfs_bs_workspace_t *workspace,
    cfs_bs_tx_t *out) {
    return tx_begin(store, workspace, NULL, 0, out);
}

int cfs_bs_tx_begin_leased(cfs_bs_store_t *store,
    cfs_bs_workspace_t *workspace, const void *lease_owner,
    cfs_bs_tx_t *out) {
    if (!lease_owner) {
        return CFS_BS_EINVAL;
    }

    return tx_begin(store, workspace, lease_owner, 0, out);
}

int cfs_bs_tx_begin_replacement(cfs_bs_store_t *store,
    cfs_bs_workspace_t *workspace, const void *lease_owner,
    cfs_bs_tx_t *out) {
    if (!lease_owner) {
        return CFS_BS_EINVAL;
    }

    return tx_begin(store, workspace, lease_owner, 1, out);
}

int cfs_bs_tx_get(cfs_bs_tx_t *tx, uint64_t block, int dirty,
    uint8_t **out) {
    uint32_t index;
    int rc;

    if (tx == NULL || out == NULL || tx->active == 0 ||
        (dirty != 0 && dirty != 1) ||
        cfs_bs_home_kind(tx->store, block) == 0) {
        return CFS_BS_EINVAL;
    }

    if (tx->store->poisoned != 0) {
        return CFS_BS_EPOISON;
    }

    if (tx_find(tx, block, &index)) {
        if (dirty != 0) {
            tx->workspace->dirty[index] = 1;
        }

        *out = tx->workspace->pages[index].bytes;
        return CFS_BS_OK;
    }

    if (tx->page_count == CFS_BS_TX_MAX_PAGES) {
        return CFS_BS_EWORKSPACE;
    }

    index = tx->page_count;
    rc = cfs_bs_read_home(tx->store, block,
                          tx->workspace->pages[index].bytes);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = cfs_bs_validate_home_page(tx->store, block,
                            tx->workspace->pages[index].bytes,
                            &tx->workspace->before_generation[index]);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (tx->workspace->before_generation[index] >
        tx->store->generation) {
        return CFS_BS_ECORRUPT;
    }

    tx->workspace->destination[index] = block;
    tx->workspace->before_hash[index] =
        cfs_get64(tx->workspace->pages[index].bytes +
                  CFS_BS_HEADER_HASH_OFF);
    tx->workspace->loaded[index] = 1;
    tx->workspace->dirty[index] = (uint8_t)dirty;
    tx->page_count++;
    *out = tx->workspace->pages[index].bytes;
    return CFS_BS_OK;
}

static int tx_require_roots(cfs_bs_tx_t *tx, uint32_t *state_index,
    uint32_t *branch_index, uint32_t *pin_index) {
    uint8_t *ignored;
    int rc;

    rc = cfs_bs_tx_get(tx, CFS_BS_STATE_BLOCK, 0, &ignored);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = cfs_bs_tx_get(tx, CFS_BS_BRANCH_BLOCK, 0, &ignored);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = cfs_bs_tx_get(tx, CFS_BS_PIN_BLOCK, 0, &ignored);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    (void)tx_find(tx, CFS_BS_STATE_BLOCK, state_index);
    (void)tx_find(tx, CFS_BS_BRANCH_BLOCK, branch_index);
    (void)tx_find(tx, CFS_BS_PIN_BLOCK, pin_index);
    if (tx->workspace->dirty[*state_index] == 0) {
        return CFS_BS_EINVAL;
    }

    return CFS_BS_OK;
}

static uint32_t tx_dirty_count(const cfs_bs_tx_t *tx) {
    uint32_t count = 0;
    uint32_t i;

    for (i = 0; i < tx->page_count; i++) {
        if (tx->workspace->dirty[i] != 0) {
            count++;
        }
    }

    return count;
}

static void tx_pack_dirty(cfs_bs_tx_t *tx) {
    cfs_bs_workspace_t *w = tx->workspace;
    uint32_t packed = 0u;
    for (uint32_t i = 0u; i < tx->page_count; i++) {
        if (tx->workspace->dirty[i] == 0) {
            continue;
        }

        if (i != packed) {
            w->pages[packed] = w->pages[i];
            w->destination[packed] = w->destination[i];
            w->before_generation[packed] = w->before_generation[i];
            w->before_hash[packed] = w->before_hash[i];
            w->loaded[packed] = 1u;
            w->dirty[packed] = 1u;
        }

        packed++;
    }

    for (uint32_t i = packed; i < tx->page_count; i++) {
        w->loaded[i] = 0u;
        w->dirty[i] = 0u;
    }

    tx->page_count = packed;
}

static int tx_install_homes(cfs_bs_tx_t *tx) {
    uint32_t i;
    int rc;

    for (i = 0; i < tx->page_count; i++) {
        if (tx->workspace->dirty[i] == 0) {
            continue;
        }

        rc = cfs_bs_io_write(tx->store, tx->workspace->destination[i],
                             tx->workspace->pages[i].bytes);
        if (rc != CFS_BS_OK) {
            return rc;
        }
    }

    return CFS_BS_OK;
}

int cfs_bs_tx_prepare(cfs_bs_tx_t *tx, uint32_t prepared_count) {
    uint8_t *first_desc;
    uint8_t *second_desc;
    uint8_t *decision_page;
    uint64_t generation;
    uint64_t state_hash;
    uint64_t branch_hash;
    uint64_t pin_hash;
    uint32_t state_index;
    uint32_t branch_index;
    uint32_t pin_index;
    uint32_t count;
    uint32_t i;
    int rc;

    if (tx == NULL || tx->active == 0) {
        return CFS_BS_EINVAL;
    }

    if (tx->store->poisoned != 0) {
        tx->active = 0;
        return CFS_BS_EPOISON;
    }

    generation = tx->store->generation + 1;
    rc = tx_require_roots(tx, &state_index, &branch_index, &pin_index);
    if (rc != CFS_BS_OK) {
        cfs_bs_tx_abort(tx);
        return rc;
    }

    count = tx_dirty_count(tx);
    if (count == 0 || count > CFS_BS_TX_MAX_PAGES ||
        prepared_count > CFS_BS_CONSTRUCT_MAX_OBJECTS) {
        cfs_bs_tx_abort(tx);
        return CFS_BS_EINVAL;
    }

    for (i = 0; i < tx->page_count; i++) {
        if (tx->workspace->dirty[i] == 0) {
            continue;
        }

        cfs_put64(tx->workspace->pages[i].bytes +
                      CFS_BS_HEADER_GENERATION_OFF,
                  generation);
        cfs_bs_header_rehash(tx->workspace->pages[i].bytes);
        rc = cfs_bs_validate_home_page(tx->store,
                                tx->workspace->destination[i],
                                tx->workspace->pages[i].bytes, NULL);
        if (rc != CFS_BS_OK) {
            cfs_bs_tx_abort(tx);
            return rc;
        }
    }

    state_hash = cfs_get64(tx->workspace->pages[state_index].bytes +
                           CFS_BS_HEADER_HASH_OFF);
    branch_hash = cfs_get64(tx->workspace->pages[branch_index].bytes +
                            CFS_BS_HEADER_HASH_OFF);
    pin_hash = cfs_get64(tx->workspace->pages[pin_index].bytes +
                         CFS_BS_HEADER_HASH_OFF);
    rc = cfs_bs_catalog_pair_validate(
        tx->store, tx->workspace->pages[state_index].bytes,
        tx->workspace->pages[branch_index].bytes,
        tx->workspace->pages[pin_index].bytes);
    if (rc != CFS_BS_OK) {
        cfs_bs_tx_abort(tx);
        return rc;
    }

    rc = cfs_bs_validate_prepared(tx->store, tx->workspace, tx->page_count,
                                   prepared_count, generation, 1);
    if (rc != CFS_BS_OK) {
        cfs_bs_tx_abort(tx);
        return rc;
    }

    /* All provider reads and validation finish before descriptor framing.
     * Keep dirty pages in descriptor order without a second payload arena. */
    tx_pack_dirty(tx);
    decision_page = tx->workspace->scratch[0].bytes;
    first_desc = tx->workspace->scratch[2].bytes;
    second_desc = tx->workspace->scratch[3].bytes;
    cfs_bs_tx_descriptors_frame(tx, generation, first_desc, second_desc);
    cfs_bs_commit_frame(tx->store, generation, count, tx->workspace->prepared,
                        prepared_count, state_hash,
                        branch_hash, pin_hash, first_desc, second_desc,
                        tx->workspace->scratch[1].bytes, decision_page);
    return CFS_BS_OK;
}

int cfs_bs_tx_publish(cfs_bs_tx_t *tx, uint32_t prepared_count) {
    uint8_t *decision_page;
    uint64_t generation;
    uint64_t base;
    uint64_t state_hash;
    uint64_t branch_hash;
    uint64_t pin_hash;
    uint32_t count;
    uint32_t i;
    int rc;

    if (tx == NULL || tx->active == 0 ||
        prepared_count > CFS_BS_CONSTRUCT_MAX_OBJECTS ||
        tx->page_count == 0 || tx->page_count > CFS_BS_TX_MAX_PAGES) {
        return CFS_BS_EINVAL;
    }

    if (tx->store->poisoned != 0) {
        tx->active = 0;
        return CFS_BS_EPOISON;
    }

    generation = tx->store->generation + 1u;
    base = cfs_bs_redo_base(tx->store, generation);
    count = tx->page_count;
    decision_page = tx->workspace->scratch[0].bytes;
    state_hash = cfs_get64(decision_page + CFS_BS_DECISION_STATE_HASH_OFF);
    branch_hash = cfs_get64(decision_page + CFS_BS_DECISION_BRANCH_HASH_OFF);
    pin_hash = cfs_get64(decision_page + CFS_BS_DECISION_PIN_HASH_OFF);

    for (i = 0; i < prepared_count;) {
        uint32_t end = i + 1u;
        while (end < prepared_count && tx->workspace->prepared[end].block ==
            tx->workspace->prepared[end - 1u].block + 1u) {
            end++;
        }

        rc = cfs_bs_io_write_blocks(tx->store,
            tx->workspace->prepared[i].block, end - i,
            tx->workspace->objects + i);
        if (rc != CFS_BS_OK) {
            tx->active = 0;
            return rc;
        }

        i = end;
    }

    rc = cfs_bs_io_write_blocks(tx->store, base + 4u, count,
        tx->workspace->pages);
    if (rc == CFS_BS_OK) {
        rc = cfs_bs_io_write_blocks(tx->store, base, 4u, tx->workspace->scratch);
    }

    if (rc == CFS_BS_OK) {
        rc = cfs_bs_io_sync(tx->store);
    }

    if (rc == CFS_BS_OK) {
        rc = tx_install_homes(tx);
    }

    if (rc != CFS_BS_OK) {
        tx->active = 0;
        return rc;
    }

    cfs_bs_clean_frame(tx->store, generation, state_hash, branch_hash,
                       pin_hash, decision_page);
    rc = cfs_bs_io_write(tx->store, CFS_BS_CLEAN_BLOCK, decision_page);
    tx->active = 0;
    if (rc != CFS_BS_OK) {
        return rc;
    }

    tx->store->generation = generation;
    return CFS_BS_OK;
}

int cfs_bs_tx_commit(cfs_bs_tx_t *tx, uint32_t prepared_count) {
    int rc = cfs_bs_tx_prepare(tx, prepared_count);

    if (rc != CFS_BS_OK) {
        return rc;
    }

    return cfs_bs_tx_publish(tx, prepared_count);
}

void cfs_bs_tx_abort(cfs_bs_tx_t *tx) {
    if (!tx) {
        return;
    }

    if (tx->workspace) {
        memset(tx->workspace->loaded, 0,
               sizeof(tx->workspace->loaded));
        memset(tx->workspace->dirty, 0,
               sizeof(tx->workspace->dirty));
    }

    tx->page_count = 0;
    tx->active = 0;
}
