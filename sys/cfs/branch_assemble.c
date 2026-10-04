/* SPDX-License-Identifier: ISC
 * branch_assemble.c -- durable hidden ownership and candidate-root completion.
 */
#include "branch_assemble_internal.h"
#include "branch_fs_internal.h"
#include "codec.h"

#include <string.h>

static cfs_bs_child_t retained(cfs_bs_ref_t ref) {
    return (cfs_bs_child_t){.kind = CFS_BS_CHILD_RETAINED, .value.retained = ref};
}

static int present(cfs_bs_child_t child) {
    return child.kind == CFS_BS_CHILD_PREPARED ||
        child.value.retained.block != CFS_BS_BLOCK_NONE;
}

static int same_ref(cfs_bs_ref_t left, cfs_bs_ref_t right) {
    return left.block == right.block && left.incarnation == right.incarnation &&
        left.hash == right.hash;
}

static int mutation_valid(cfs_bf_mutation_observer_t mutations) {
    return (mutations.begin == NULL) == (mutations.end == NULL);
}

static int mutation_begin(cfs_bf_mutation_observer_t mutations) {
    return mutations.begin == NULL ? CFS_BS_OK :
        mutations.begin(mutations.context);
}

static void mutation_end(cfs_bf_mutation_observer_t mutations, int status) {
    if (mutations.end) {
        mutations.end(mutations.context, status);
    }
}

int cfs_bf_assemble_fail(cfs_bf_assemble_t *assembly, int status) {
    if (assembly != NULL && status != CFS_BS_OK && assembly->failure == 0) {
        assembly->failure = status;
    }

    return status;
}

int cfs_bf_assemble_prepare(cfs_bs_store_t *store, cfs_bf_assemble_t *assembly,
    cfs_bs_workspace_t *storage, cfs_bf_assemble_workspace_t *w) {
    if (assembly == NULL || w == NULL) {
        return CFS_BS_EINVAL;
    }

    if (assembly->failure != 0) {
        return assembly->failure;
    }

    if (assembly->state != 1u) {
        return CFS_BS_EINVAL;
    }

    int status = cfs_bf_prepare(store, storage, &w->namespace);
    cfs_bs_staging_t stage;
    if (status == CFS_BS_OK) {
        status = cfs_bs_staging_get(store, assembly->staging.id, &stage, storage);
    }

    if (status == CFS_BS_OK && (stage.revision != assembly->staging.revision ||
            !same_ref(stage.root, assembly->staging.root))) {
        status = CFS_BS_ECONFLICT;
    }

    if (status == CFS_BS_OK) {
        cfs_bt_stage_reset(&w->namespace.stage);
        memset(&w->file.work, 0, sizeof(w->file.work));
    }

    return status;
}

static int construct(cfs_bs_store_t *store, cfs_bf_assemble_t *assembly,
    const cfs_bs_child_t *children, uint32_t count, uint32_t length,
    cfs_bs_workspace_t *storage, cfs_bf_assemble_workspace_t *w) {
    cfs_bs_object_plan_t plan = {.children = children, .child_count = count,
        .payload = w->payload, .payload_length = length};
    uint32_t index;
    int status = cfs_bt_stage_add(&w->namespace.stage, &plan, &index);
    cfs_bt_stage_batch_t batch;
    if (status == CFS_BS_OK) {
        status = cfs_bt_stage_compile(&w->namespace.stage, index, &batch);
    }

    cfs_bs_ref_t root;
    uint64_t revision;
    if (status == CFS_BS_OK) {
        status = mutation_begin(assembly->mutations);
    }

    if (status == CFS_BS_OK) {
        int raw = cfs_bs_staging_construct(store, assembly->staging.id,
            assembly->staging.revision, batch.plans, batch.count, &root,
            &revision, storage);

        if (raw == CFS_BS_OK) {
            assembly->staging.root = root;
            assembly->staging.revision = revision;
        }

        mutation_end(assembly->mutations, raw);
        status = raw;
    }

    return status;
}

int cfs_bf_assemble_save(cfs_bs_store_t *store, cfs_bf_assemble_t *assembly,
    cfs_bs_child_t inodes, cfs_bs_child_t orphans, cfs_bs_child_t content,
    cfs_bs_workspace_t *storage, cfs_bf_assemble_workspace_t *w) {
    cfs_bs_child_t optional[3] = {inodes, orphans, content};
    cfs_bs_child_t children[4] = {retained(assembly->base_root)};
    uint64_t flags = 0u;
    uint32_t count = 1u;
    for (uint32_t i = 0u; i < 3u; i++) {
        if (present(optional[i])) {
            children[count++] = optional[i];
            flags |= UINT64_C(1) << i;
        }
    }

    memcpy(w->payload, "CFSAS001", 8u);
    cfs_put64(w->payload + 8u, flags);
    int status = construct(store, assembly, children, count, 16u, storage, w);
    uint32_t observed;
    uint32_t length;
    if (status == CFS_BS_OK) {
        status = cfs_bs_object_read(store, assembly->staging.root, w->refs, 4u,
            &observed, w->payload, sizeof(w->payload), &length, storage);
    }

    if (status != CFS_BS_OK) {
        return status;
    }

    if (length != 16u || observed != count ||
        memcmp(w->payload, "CFSAS001", 8u) != 0 ||
        cfs_get64(w->payload + 8u) != flags ||
        !same_ref(w->refs[0], assembly->base_root)) {
        return CFS_BS_ECORRUPT;
    }

    cfs_bs_ref_t *roots[3] = {&assembly->inodes, &assembly->orphans,
        &assembly->content};
    uint32_t next = 1u;
    for (uint32_t i = 0u; i < 3u; i++) {
        *roots[i] = (flags & (UINT64_C(1) << i)) != 0u ? w->refs[next++] :
            (cfs_bs_ref_t){CFS_BS_BLOCK_NONE, 0u, 0u};
    }

    return CFS_BS_OK;
}

int cfs_bf_assemble_begin(cfs_bs_store_t *store, uint64_t stage_id,
    uint64_t expected, const cfs_bf_change_header_t *header,
    cfs_bf_assemble_t *out, cfs_bs_workspace_t *storage,
    cfs_bf_assemble_workspace_t *w, cfs_bf_mutation_observer_t mutations) {
    if (header == NULL || out == NULL || w == NULL ||
        !mutation_valid(mutations) ||
        !cfs_bf_id_valid(header->root_before) ||
        !cfs_bf_id_valid(header->root_after) || header->base == 0u ||
        header->result == 0u || header->root_before.birth > header->base ||
        header->root_after.birth > header->result) {
        return CFS_BS_EINVAL;
    }

    int status = cfs_bf_prepare(store, storage, &w->namespace);
    if (status != CFS_BS_OK) {
        return status;
    }

    cfs_bf_assemble_t *a = &w->next;
    memset(a, 0, sizeof(*a));
    a->content = (cfs_bs_ref_t){CFS_BS_BLOCK_NONE, 0u, 0u};
    a->header = *header;
    a->mutations = mutations;
    cfs_bs_state_t state;
    status = cfs_bs_inspect_state(store, &state, storage);
    if (status == CFS_BS_OK && (memcmp(header->uuid, store->uuid,
            CFS_BS_UUID_SIZE) != 0 || header->base >= state.next_revision ||
            header->result >= state.next_revision ||
            store->reserve < cfs_bf_reserve_blocks(store->total_blocks))) {
        status = CFS_BS_EINVAL;
    }

    if (status == CFS_BS_OK) {
        status = cfs_bs_staging_get(store, stage_id, &a->staging, storage);
    }

    if (status == CFS_BS_OK && (a->staging.revision != expected ||
            a->staging.revision != a->staging.expected_destination)) {
        status = CFS_BS_ECONFLICT;
    }

    if (status == CFS_BS_OK) {
        a->base_root = a->staging.root;
        status = cfs_bf_read(store, a->base_root, a->staging.revision,
            &a->base, storage, &w->namespace);
    }

    if (status == CFS_BS_OK &&
        !cfs_bf_id_equal(a->base.root_id, header->root_before)) {
        status = CFS_BS_ECONFLICT;
    }

    if (status == CFS_BS_OK) {
        a->inodes = a->base.inodes;
        a->orphans = a->base.orphans;
        cfs_bt_stage_reset(&w->namespace.stage);
        status = cfs_bf_assemble_save(store, a, retained(a->inodes),
            retained(a->orphans), retained(a->content), storage, w);
    }

    if (status == CFS_BS_OK) {
        a->state = 1u;
        *out = *a;
    }

    return status;
}

int cfs_bf_assemble_finish(cfs_bs_store_t *store, cfs_bf_assemble_t *a,
    cfs_bs_staging_t *out, cfs_bs_workspace_t *storage,
    cfs_bf_assemble_workspace_t *w) {
    int status = out == NULL ? CFS_BS_EINVAL :
        cfs_bf_assemble_prepare(store, a, storage, w);
    if (status == CFS_BS_OK) {
        status = cfs_bf_assemble_flush(store, a, storage, w);
    }

    cfs_bf_entry_t directory;
    cfs_bf_view_t view;
    if (status == CFS_BS_OK) {
        view = (cfs_bf_view_t){.revision = a->staging.revision,
            .root_id = a->header.root_after, .inodes = a->inodes,
            .orphans = a->orphans};
        status = cfs_bf_directory(store, &view, view.root_id, &directory,
            storage, &w->namespace);
        if (status == CFS_BS_OK &&
            !cfs_bf_id_equal(directory.inode.parent, view.root_id)) {
            status = CFS_BS_ECORRUPT;
        }
    }

    if (status == CFS_BS_OK) {
        cfs_bt_stage_reset(&w->namespace.stage);
        cfs_bs_child_t children[2] = {retained(a->inodes), retained(a->orphans)};
        int has_orphans = present(children[1]);
        status = cfs_bf_root_frame(view.root_id, has_orphans, w->payload);
        if (status == CFS_BS_OK) {
            status = construct(store, a, children, has_orphans ? 2u : 1u,
                CFS_BF_ROOT_BYTES, storage, w);
        }
    }

    if (status == CFS_BS_OK) {
        a->state = 2u;
        *out = a->staging;
    }

    return cfs_bf_assemble_fail(a, status);
}
