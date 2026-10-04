/* SPDX-License-Identifier: ISC
 * branch_store_staging.c -- durable hidden-root admission and ownership transfer.
 */
#include "branch_store_staging.h"
#include "branch_store_internal.h"

#include <string.h>

#include "codec.h"

static uint8_t *slot_bytes(uint8_t *catalog, uint64_t id) {
    return catalog + CFS_BS_CATALOG_BODY_OFF +
        (size_t)(id & 15u) * CFS_BS_CATALOG_ENTRY_SIZE;
}

static void staging_result(const cfs_bs_catalog_entry_t *entry,
    cfs_bs_staging_t *out) {
    memset(out, 0, sizeof(*out));
    out->id = entry->id;
    out->revision = entry->revision;
    out->destination = entry->origin_id;
    out->expected_destination = entry->origin_revision;
    out->root = entry->root;
}

/* A stage shares its branch destination's root. A donor starts empty and may
 * also serve a staging head. */
static int begin(cfs_bs_store_t *store, uint64_t destination,
    uint64_t expected_destination, int donor, cfs_bs_staging_t *out,
    cfs_bs_workspace_t *workspace) {
    cfs_bs_catalog_entry_t source;
    cfs_bs_catalog_entry_t stage = {0};
    cfs_bs_state_t state;
    cfs_bs_tx_t tx;
    uint8_t *catalog;
    uint8_t *state_page;
    uint32_t slot;
    int rc;

    if (!out) {
        return CFS_BS_EINVAL;
    }

    rc = cfs_bs_tx_begin(store, workspace, &tx);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = cfs_bs_tx_load_branch(&tx, destination, expected_destination,
        &catalog, &source);
    if (rc == CFS_BS_ENOENT && donor) {
        rc = cfs_bs_tx_load_head(&tx, destination, expected_destination,
            CFS_BS_CATALOG_STAGING, &catalog, &source);
    }

    for (slot = 0; rc == CFS_BS_OK && slot < CFS_BS_MAX_BRANCHES; slot++) {
        rc = cfs_bs_catalog_entry_decode_checked(store, catalog, slot,
            0, &stage);
        if (rc == CFS_BS_ENOENT) {
            rc = CFS_BS_OK;
            break;
        }
    }

    if (rc == CFS_BS_OK && slot == CFS_BS_MAX_BRANCHES) {
        rc = CFS_BS_ENOSPC;
    }

    if (rc == CFS_BS_OK) {
        rc = cfs_bs_tx_state(&tx, 1, &state, &state_page);
    }

    if (rc == CFS_BS_OK && state.next_serial > (UINT64_MAX >> 4)) {
        rc = CFS_BS_EOVERFLOW;
    }

    if (rc == CFS_BS_OK && !donor && !cfs_bs_ref_is_null(source.root)) {
        rc = cfs_bs_tx_check_catalog_ref(&tx, source.root);
        if (rc == CFS_BS_OK) {
            rc = cfs_bs_tx_ref_add(&tx, source.root, workspace->scratch[0].bytes);
        }
    }

    if (rc != CFS_BS_OK) {
        cfs_bs_tx_abort(&tx);
        return rc;
    }

    memset(&stage, 0, sizeof(stage));
    stage.id = (state.next_serial++ << 4) | slot;
    stage.revision = source.revision;
    stage.origin_id = source.id;
    stage.origin_revision = source.revision;
    stage.root = donor ? (cfs_bs_ref_t){CFS_BS_BLOCK_NONE, 0u, 0u} :
        source.root;
    stage.kind = CFS_BS_CATALOG_STAGING;
    cfs_bs_catalog_encode(slot_bytes(catalog, stage.id), &stage);
    cfs_bs_state_encode(state_page, &state);
    rc = cfs_bs_tx_commit(&tx, 0);
    if (rc == CFS_BS_OK) {
        staging_result(&stage, out);
    }

    return rc;
}

int cfs_bs_staging_begin(cfs_bs_store_t *store, uint64_t destination,
    uint64_t expected_destination, cfs_bs_staging_t *out,
    cfs_bs_workspace_t *workspace) {
    return begin(store, destination, expected_destination, 0, out, workspace);
}

int cfs_bs_staging_get(cfs_bs_store_t *store, uint64_t id,
    cfs_bs_staging_t *out, cfs_bs_workspace_t *workspace) {
    cfs_bs_branch_t branch;
    cfs_bs_staging_t result = {0};
    int rc;

    if (id == 0 || out == NULL) {
        return CFS_BS_EINVAL;
    }

    rc = cfs_bs_inspect_branch(store, (uint32_t)(id & 15u), &branch,
        workspace);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (branch.id != id || branch.kind != CFS_BS_CATALOG_STAGING) {
        return CFS_BS_ENOENT;
    }

    rc = cfs_bs_validate_root(store, branch.root, workspace);
    if (rc == CFS_BS_OK) {
        result.id = branch.id;
        result.revision = branch.revision;
        result.destination = branch.origin_id;
        result.expected_destination = branch.origin_revision;
        result.root = branch.root;
        *out = result;
    }

    return rc;
}

int cfs_bs_staging_publish(cfs_bs_store_t *store, uint64_t id,
    uint64_t expected_revision, cfs_bs_branch_t *out,
    cfs_bs_workspace_t *workspace) {
    cfs_bs_catalog_entry_t stage;
    cfs_bs_catalog_entry_t destination;
    cfs_bs_branch_t result = {0};
    cfs_bs_state_t state;
    cfs_bs_tx_t tx;
    uint8_t *catalog;
    uint8_t *state_page;
    int rc;

    if (!out) {
        return CFS_BS_EINVAL;
    }

    rc = cfs_bs_tx_begin(store, workspace, &tx);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = cfs_bs_tx_load_head(&tx, id, expected_revision,
        CFS_BS_CATALOG_STAGING, &catalog, &stage);
    if (rc == CFS_BS_OK) {
        rc = cfs_bs_tx_load_branch(&tx, stage.origin_id,
            stage.origin_revision, &catalog, &destination);
    }

    if (rc == CFS_BS_OK) {
        rc = cfs_bs_tx_state(&tx, 1, &state, &state_page);
    }

    if (rc == CFS_BS_OK && state.next_revision == UINT64_MAX) {
        rc = CFS_BS_EOVERFLOW;
    }

    if (rc == CFS_BS_OK) {
        rc = cfs_bs_tx_check_catalog_ref(&tx, stage.root);
    }

    if (rc == CFS_BS_OK) {
        rc = cfs_bs_tx_check_catalog_ref(&tx, destination.root);
    }

    /* Move the already-owned staging edge; do not add and drop a transient
     * extra edge, which could overflow even when the final count is valid. */
    if (rc == CFS_BS_OK && !cfs_bs_ref_is_null(destination.root)) {
        rc = cfs_bs_tx_ref_drop(&tx, &state, destination.root,
            workspace->scratch[0].bytes);
    }

    if (rc != CFS_BS_OK) {
        cfs_bs_tx_abort(&tx);
        return rc;
    }

    destination.root = stage.root;
    destination.revision = state.next_revision++;
    cfs_bs_catalog_encode(slot_bytes(catalog, destination.id), &destination);
    memset(slot_bytes(catalog, stage.id), 0, CFS_BS_CATALOG_ENTRY_SIZE);
    cfs_bs_state_encode(state_page, &state);
    rc = cfs_bs_tx_commit(&tx, 0);
    if (rc == CFS_BS_OK) {
        result.id = destination.id;
        result.revision = destination.revision;
        result.origin_id = destination.origin_id;
        result.origin_revision = destination.origin_revision;
        result.root = destination.root;
        result.name_length = destination.name_length;
        result.kind = destination.kind;
        memcpy(result.name, destination.name, sizeof(result.name));
        *out = result;
    }

    return rc;
}

int cfs_bs_staging_cancel(cfs_bs_store_t *store, uint64_t id,
    uint64_t expected_revision, cfs_bs_workspace_t *workspace) {
    cfs_bs_catalog_entry_t stage;
    cfs_bs_state_t state;
    cfs_bs_tx_t tx;
    uint8_t *catalog;
    uint8_t *state_page;
    int rc = cfs_bs_tx_begin(store, workspace, &tx);

    if (rc != CFS_BS_OK) {
        return rc;
    }

    rc = cfs_bs_tx_load_head(&tx, id, expected_revision,
        CFS_BS_CATALOG_STAGING, &catalog, &stage);
    if (rc == CFS_BS_OK) {
        rc = cfs_bs_tx_state(&tx, 1, &state, &state_page);
    }

    if (rc == CFS_BS_OK && !cfs_bs_ref_is_null(stage.root)) {
        rc = cfs_bs_tx_check_catalog_ref(&tx, stage.root);
        if (rc == CFS_BS_OK) {
            rc = cfs_bs_tx_ref_drop(&tx, &state, stage.root,
                workspace->scratch[0].bytes);
        }
    }

    if (rc != CFS_BS_OK) {
        cfs_bs_tx_abort(&tx);
        return rc;
    }

    memset(slot_bytes(catalog, stage.id), 0, CFS_BS_CATALOG_ENTRY_SIZE);
    cfs_bs_state_encode(state_page, &state);
    return cfs_bs_tx_commit(&tx, 0);
}

int cfs_bs_donor_chained(const uint8_t *object, uint32_t child_count,
    uint32_t payload_length) {
    const uint8_t *payload = object + CFS_BS_OBJECT_REFS_OFF +
        (size_t)child_count * CFS_BS_REF_SIZE;

    if (payload_length != CFS_BS_DONOR_PAYLOAD ||
        cfs_get64(payload) != CFS_BS_DONOR_MAGIC ||
        cfs_get32(payload + 8) > 1u || cfs_get32(payload + 12) != 0u ||
        cfs_get32(payload + 8) > child_count) {
        return -1;
    }

    return (int)cfs_get32(payload + 8);
}

/* Whether a catalog entry still has identity id at revision. */
static int head_current(cfs_bs_store_t *store, uint64_t id, uint64_t revision,
    cfs_bs_workspace_t *workspace) {
    for (uint32_t slot = 0; slot < CFS_BS_MAX_BRANCHES; slot++) {
        cfs_bs_branch_t entry;

        if (cfs_bs_inspect_branch(store, slot, &entry, workspace) ==
                CFS_BS_OK && entry.id == id) {
            return entry.revision == revision;
        }
    }

    return 0;
}

/* Release head's donor and every donor whose head is gone or moved on: no
 * construction can consume those, so they go to ordinary reaping. */
int cfs_bs_branch_unshare_cancel(cfs_bs_store_t *store, uint64_t head,
    cfs_bs_workspace_t *workspace) {
    int rc = CFS_BS_OK;

    if (store == NULL || workspace == NULL) {
        return CFS_BS_EINVAL;
    }

    for (uint32_t slot = 0; rc == CFS_BS_OK && slot < CFS_BS_MAX_BRANCHES;
         slot++) {
        cfs_bs_branch_t stage;
        uint32_t children;
        uint32_t length;

        if (cfs_bs_inspect_branch(store, slot, &stage, workspace) ==
                CFS_BS_OK && stage.kind == CFS_BS_CATALOG_STAGING &&
            !cfs_bs_ref_is_null(stage.root) &&
            cfs_bs_object_load(store, stage.root, workspace->scratch[2].bytes,
                &children, &length) == CFS_BS_OK &&
            cfs_bs_donor_chained(workspace->scratch[2].bytes, children,
                length) >= 0 &&
            (stage.origin_id == head || !head_current(store, stage.origin_id,
                stage.origin_revision, workspace))) {
            rc = cfs_bs_staging_cancel(store, stage.id, stage.revision,
                workspace);
        }
    }

    return rc;
}

static int holder_store(cfs_bs_store_t *store, cfs_bs_staging_t *donor,
    uint32_t count, uint32_t chained, cfs_bs_workspace_t *workspace) {
    uint8_t payload[CFS_BS_DONOR_PAYLOAD] = {0};

    cfs_put64(payload, CFS_BS_DONOR_MAGIC);
    cfs_put32(payload + 8, chained);
    return cfs_bs_staging_store(store, donor->id, donor->revision,
        workspace->releasing, count, payload, sizeof(payload), &donor->root,
        &donor->revision, workspace);
}

int cfs_bs_branch_unshare(cfs_bs_store_t *store, uint64_t head,
    uint64_t expected_revision, const cfs_bs_object_plan_t *plans,
    uint32_t plan_count, cfs_bs_workspace_t *workspace) {
    cfs_bs_staging_t donor = {0};
    uint32_t retained = 0u;
    uint32_t chained = 0u;
    uint32_t held = 0u;
    int rc;

    if (store == NULL || workspace == NULL || plans == NULL ||
        plan_count == 0u || plan_count > CFS_BS_CONSTRUCT_MAX_OBJECTS) {
        return CFS_BS_EINVAL;
    }

    for (uint32_t i = 0; i < plan_count; i++) {
        for (uint32_t edge = 0; edge < plans[i].child_count; edge++) {
            retained += plans[i].children[edge].kind ==
                CFS_BS_CHILD_RETAINED;
        }
    }

    /* With nothing retained, a donor cannot shrink the construction. */
    if (retained == 0u) {
        return CFS_BS_EWORKSPACE;
    }

    rc = cfs_bs_branch_unshare_cancel(store, head, workspace);
    if (rc == CFS_BS_OK) {
        rc = begin(store, head, expected_revision, 1, &donor, workspace);
    }

    /* Each holder is one object over at most 128 children, so its
     * transaction fits the one-object construction bound. */
    for (uint32_t i = 0; rc == CFS_BS_OK && i < plan_count; i++) {
        for (uint32_t edge = 0; rc == CFS_BS_OK && edge < plans[i].child_count;
             edge++) {
            if (plans[i].children[edge].kind != CFS_BS_CHILD_RETAINED) {
                continue;
            }

            if (held == 0u && chained) {
                workspace->releasing[held++] = donor.root;
            }

            workspace->releasing[held++] =
                plans[i].children[edge].value.retained;
            if (held == CFS_BS_MAX_CHILDREN) {
                rc = holder_store(store, &donor, held, chained, workspace);
                chained = 1u;
                held = 0u;
            }
        }
    }

    if (rc == CFS_BS_OK && held != 0u) {
        rc = holder_store(store, &donor, held, chained, workspace);
    }

    /* A partial donor holds nothing a construction should rely on. */
    if (rc != CFS_BS_OK && donor.id != 0u) {
        (void)cfs_bs_staging_cancel(store, donor.id, donor.revision,
            workspace);
    }

    return rc;
}
