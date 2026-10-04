/* SPDX-License-Identifier: ISC
 * branch_fs_mutate.c -- expected-head namespace initialization and creation.
 */
#include "branch_fs_internal.h"
#include "branch_store_staging.h"

#include <string.h>

static cfs_bs_child_t retained(cfs_bs_ref_t ref) {
    return (cfs_bs_child_t){.kind = CFS_BS_CHILD_RETAINED, .value.retained = ref};
}

static cfs_bs_child_t empty(void) {
    return retained((cfs_bs_ref_t){CFS_BS_BLOCK_NONE, 0u, 0u});
}

static int has_child(cfs_bs_child_t child) {
    return child.kind == CFS_BS_CHILD_PREPARED ||
        child.value.retained.block != CFS_BS_BLOCK_NONE;
}

static cfs_bf_inode_t new_inode(uint16_t type, cfs_bf_id_t parent,
    cfs_bf_attributes_t attributes) {
    cfs_bf_inode_t inode = {.type = type, .mode = attributes.mode,
        .uid = attributes.uid, .gid = attributes.gid,
        .links = type == CFS_BF_DIRECTORY ? 2u : 1u,
        .atime = attributes.time, .mtime = attributes.time,
        .ctime = attributes.time, .btime = attributes.time};
    if (type == CFS_BF_DIRECTORY) {
        inode.parent = parent;
    }

    return inode;
}

static int begin(cfs_bs_store_t *store, uint64_t branch_id, uint64_t expected,
    cfs_bf_inode_t inode, cfs_bf_created_t *out, cfs_bs_branch_t *branch,
    cfs_bs_workspace_t *store_workspace, cfs_bf_workspace_t *workspace) {
    int rc = cfs_bf_prepare(store, store_workspace, workspace);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    cfs_bt_stage_reset(&workspace->stage);
    if (out == NULL || cfs_bf_inode_frame(inode, 0, workspace->inode_bytes) !=
        CFS_BS_OK) {
        return CFS_BS_EINVAL;
    }

    rc = cfs_bs_branch_get(store, branch_id, branch, store_workspace);
    if (rc == CFS_BS_OK && branch->revision != expected) {
        return CFS_BS_ECONFLICT;
    }

    return rc;
}

static int next_id(cfs_bs_store_t *store, cfs_bf_id_t *out,
    cfs_bs_workspace_t *store_workspace) {
    cfs_bs_state_t state;
    int rc = cfs_bs_inspect_state(store, &state, store_workspace);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (state.next_revision == UINT64_MAX) {
        return CFS_BS_EOVERFLOW;
    }

    *out = (cfs_bf_id_t){state.next_revision, 0u};
    return CFS_BS_OK;
}

int cfs_bf_edit_inode(cfs_bs_store_t *store, cfs_bs_child_t root,
    cfs_bf_id_t id, cfs_bf_inode_t inode, cfs_bs_child_t tree, uint32_t kind,
    cfs_bs_child_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace) {
    int rc = cfs_bf_inode_frame(inode, has_child(tree), workspace->inode_bytes);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    (void)cfs_bf_id_key(id, workspace->key);
    cfs_bt_change_t change = {.kind = kind, .key = workspace->key,
        .key_length = CFS_BF_ID_BYTES, .value = workspace->inode_bytes,
        .value_length = CFS_BF_INODE_BYTES, .child = has_child(tree) ? &tree : NULL};
    return cfs_bt_edit(store, &workspace->stage, root, change, out,
        store_workspace, &workspace->edit);
}

int cfs_bf_stage_root(cfs_bs_store_t *store, cfs_bf_view_t view,
    cfs_bs_child_t inodes, cfs_bs_child_t orphans, uint32_t *out_index,
    cfs_bf_workspace_t *workspace) {
    if (store == NULL || workspace == NULL || out_index == NULL) {
        return CFS_BS_EINVAL;
    }

    uint32_t reserve = cfs_bf_reserve_blocks(store->total_blocks);
    if (reserve == 0u || store->reserve < reserve) {
        return CFS_BS_ENOSPC;
    }

    cfs_bs_child_t children[2] = {inodes, orphans};
    int orphan = has_child(orphans);
    int rc = cfs_bf_root_frame(view.root_id, orphan, workspace->root_bytes);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    cfs_bs_object_plan_t plan = {.children = children, .child_count = orphan ? 2u : 1u,
        .payload = workspace->root_bytes, .payload_length = CFS_BF_ROOT_BYTES};
    return cfs_bt_stage_add(&workspace->stage, &plan, out_index);
}

int cfs_bf_publish_roots(cfs_bs_store_t *store, cfs_bs_branch_t branch,
    cfs_bf_view_t view, cfs_bs_child_t inodes, cfs_bs_child_t orphans,
    uint64_t *out_revision,
    cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace) {
    uint32_t index;
    int rc = cfs_bf_stage_root(store, view, inodes, orphans, &index, workspace);
    cfs_bt_stage_batch_t batch;
    if (rc == CFS_BS_OK) {
        rc = cfs_bt_stage_compile(&workspace->stage, index, &batch);
    }

    cfs_bs_ref_t root;
    uint64_t revision;
    if (rc == CFS_BS_OK) {
        rc = cfs_bs_branch_construct(store, branch.id, branch.revision,
            batch.plans, batch.count, &root, &revision, store_workspace);
    }

    /* Scattered retained rows: a donor holds them for the caller's second
     * attempt, which predicts identities from the advanced counter. */
    if (rc == CFS_BS_EWORKSPACE) {
        (void)cfs_bs_branch_unshare(store, branch.id, branch.revision,
            batch.plans, batch.count, store_workspace);
    }

    if (rc == CFS_BS_OK) {
        *out_revision = revision;
    }

    return rc;
}

int cfs_bf_retry_done(cfs_bs_store_t *store, uint64_t branch_id, int status,
    cfs_bs_workspace_t *store_workspace) {
    if (status != CFS_BS_OK) {
        (void)cfs_bs_branch_unshare_cancel(store, branch_id, store_workspace);
    }

    return status;
}

int cfs_bf_publish(cfs_bs_store_t *store, cfs_bs_branch_t branch,
    cfs_bf_view_t view, cfs_bs_child_t inodes, uint64_t *out_revision,
    cfs_bs_workspace_t *store_workspace, cfs_bf_workspace_t *workspace) {
    return cfs_bf_publish_roots(store, branch, view, inodes,
        retained(view.orphans), out_revision, store_workspace, workspace);
}

static int publish_created(cfs_bs_store_t *store, cfs_bs_branch_t branch,
    cfs_bf_view_t view, cfs_bs_child_t inodes, cfs_bf_id_t created,
    cfs_bf_created_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace) {
    uint64_t revision;
    int rc = cfs_bf_publish(store, branch, view, inodes, &revision,
        store_workspace, workspace);
    if (rc == CFS_BS_OK) {
        if (revision != created.birth) {
            /* Serialization prevents intervening identity allocation. */
            __builtin_trap();
        }

        *out = (cfs_bf_created_t){.id = created, .revision = revision};
    }

    return rc;
}

static int finish(cfs_bf_workspace_t *workspace, int status) {
    if (workspace != NULL && status != CFS_BS_OK) {
        cfs_bt_stage_discard(&workspace->stage);
    }

    return status;
}

int cfs_bf_initialize(cfs_bs_store_t *store, uint64_t branch_id,
    uint64_t expected_revision, cfs_bf_attributes_t attributes,
    cfs_bf_created_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace) {
    cfs_bf_inode_t inode = new_inode(CFS_BF_DIRECTORY, (cfs_bf_id_t){1u, 0u},
        attributes);
    cfs_bs_branch_t branch;
    int rc = begin(store, branch_id, expected_revision, inode, out, &branch,
        store_workspace, workspace);
    if (rc == CFS_BS_OK && branch.root.block != CFS_BS_BLOCK_NONE) {
        rc = CFS_BS_EEXIST;
    }

    cfs_bf_id_t id;
    if (rc == CFS_BS_OK) {
        rc = next_id(store, &id, store_workspace);
    }

    cfs_bs_child_t inodes;
    if (rc == CFS_BS_OK) {
        inode.parent = id;
        rc = cfs_bf_edit_inode(store, empty(), id, inode, empty(), CFS_BT_INSERT,
            &inodes, store_workspace, workspace);
    }

    if (rc == CFS_BS_OK) {
        cfs_bf_view_t view = {.root_id = id,
            .orphans = {CFS_BS_BLOCK_NONE, 0u, 0u}};
        rc = publish_created(store, branch, view, inodes, id, out,
            store_workspace, workspace);
    }

    return finish(workspace, rc);
}

int cfs_bf_create_records(cfs_bs_store_t *store, const void *name, uint16_t length,
    cfs_bs_child_t content, cfs_bf_created_t *out,
    cfs_bs_workspace_t *store_workspace, cfs_bf_workspace_t *workspace) {
    /* Metadata spanning the lifecycle stays off the kernel stack. */
    cfs_bf_mutation_t *mutation = &workspace->mutation;
    cfs_bf_entry_t *parent = &mutation->parent;
    cfs_bf_inode_t *inode = &mutation->inode;
    if (parent->inode.size == INT64_MAX) {
        return CFS_BS_EOVERFLOW;
    }

    cfs_bf_id_t id;
    int rc = next_id(store, &id, store_workspace);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    (void)cfs_bf_dirent_frame(id, workspace->dirent);
    cfs_bt_change_t change = {.kind = CFS_BT_INSERT, .key = name,
        .key_length = length, .value = workspace->dirent,
        .value_length = CFS_BF_ID_BYTES};
    cfs_bs_child_t names;
    rc = cfs_bt_edit(store, &workspace->stage, retained(parent->tree), change,
        &names, store_workspace, &workspace->edit);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    parent->inode.size++;
    if (inode->type == CFS_BF_DIRECTORY) {
        parent->inode.links++;
    }

    parent->inode.mtime = inode->mtime;
    parent->inode.ctime = inode->ctime;
    cfs_bs_child_t inodes;
    rc = cfs_bf_edit_inode(store, retained(mutation->view.inodes), parent->id, parent->inode,
        names, CFS_BT_REPLACE, &inodes, store_workspace, workspace);
    if (rc == CFS_BS_OK) {
        cfs_bs_child_t next;
        rc = cfs_bf_edit_inode(store, inodes, id, *inode, content, CFS_BT_INSERT,
            &next, store_workspace, workspace);
        if (rc == CFS_BS_OK) {
            inodes = next;
        }
    }

    if (rc == CFS_BS_OK) {
        rc = publish_created(store, mutation->branch, mutation->view, inodes, id, out,
            store_workspace, workspace);
    }

    return rc;
}

static int create_once(cfs_bs_store_t *store, uint64_t branch_id,
    uint64_t expected_revision, cfs_bf_id_t parent_id, const void *name,
    uint16_t length, uint16_t type, cfs_bf_attributes_t attributes,
    cfs_bf_created_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace) {
    if (!cfs_bf_id_valid(parent_id) || !cfs_bf_name_valid(name, length)) {
        return finish(workspace, CFS_BS_EINVAL);
    }

    int rc = cfs_bf_prepare(store, store_workspace, workspace);
    if (rc != CFS_BS_OK) {
        return finish(workspace, rc);
    }

    cfs_bf_mutation_t *mutation = &workspace->mutation;
    mutation->inode = new_inode(type, parent_id, attributes);
    rc = begin(store, branch_id, expected_revision, mutation->inode, out,
        &mutation->branch,
        store_workspace, workspace);
    if (rc == CFS_BS_OK) {
        rc = cfs_bf_read(store, mutation->branch.root, mutation->branch.revision,
            &mutation->view, store_workspace, workspace);
    }

    if (rc == CFS_BS_OK) {
        rc = cfs_bf_directory(store, &mutation->view, parent_id, &mutation->parent,
            store_workspace, workspace);
    }

    if (rc == CFS_BS_OK) {
        rc = cfs_bf_create_records(store, name, length, empty(),
            out, store_workspace, workspace);
    }

    return finish(workspace, rc);
}

int cfs_bf_create(cfs_bs_store_t *store, uint64_t branch_id,
    uint64_t expected_revision, cfs_bf_id_t parent_id, const void *name,
    uint16_t length, uint16_t type, cfs_bf_attributes_t attributes,
    cfs_bf_created_t *out, cfs_bs_workspace_t *store_workspace,
    cfs_bf_workspace_t *workspace) {
    int rc = create_once(store, branch_id, expected_revision, parent_id, name,
        length, type, attributes, out, store_workspace, workspace);

    if (rc == CFS_BS_EWORKSPACE) {
        rc = cfs_bf_retry_done(store, branch_id, create_once(store, branch_id,
            expected_revision, parent_id, name, length, type, attributes, out,
            store_workspace, workspace), store_workspace);
    }

    return rc;
}
