/* SPDX-License-Identifier: ISC
 * branch_fs_rename.c -- atomic rename, replacement, and ancestry.
 */
#include "branch_fs_internal.h"

#include <string.h>

static cfs_bs_child_t retained(cfs_bs_ref_t ref) {
    return (cfs_bs_child_t){.kind = CFS_BS_CHILD_RETAINED, .value.retained = ref};
}

static cfs_bf_entry_t *destination_parent(cfs_bf_workspace_t *w) {
    return w->rename.same_parent ? &w->mutation.parent : &w->rename.parent;
}

static int binding_counts(const cfs_bf_entry_t *parent, uint16_t type,
    uint64_t observed) {
    if (parent->inode.size < observed) {
        return CFS_BS_ECORRUPT;
    }

    /* Each observed directory requires one child link. Each observed regular
     * name excludes one from the maximum possible child-directory count. */
    int valid = type == CFS_BF_DIRECTORY ? parent->inode.links >= 2u + observed :
        parent->inode.links <= parent->inode.size + 2u - observed;
    return valid ? CFS_BS_OK : CFS_BS_ECORRUPT;
}

static int ancestry(cfs_bs_store_t *store, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *w) {
    cfs_bf_id_t cursor = w->rename.parent.id;
    cfs_bf_id_t anchor = cursor;
    uint64_t power = 1u;
    uint64_t distance = 0u;

    /* Brent cycle detection: one ancestor read per step, constant scratch, and
     * a doubling counter that supported geometry keeps far below overflow. */
    for (;;) {
        if (cfs_bf_id_equal(cursor, w->mutation.target.id)) {
            return CFS_BS_EINVAL;
        }

        int status = cfs_bf_directory(store, &w->mutation.view, cursor,
            &w->rename.ancestor, storage, w);
        if (status == CFS_BS_ENOENT || status == CFS_BS_ENOTDIR) {
            return CFS_BS_ECORRUPT;
        }

        if (status != CFS_BS_OK) {
            return status;
        }

        if (cfs_bf_id_equal(cursor, w->mutation.view.root_id)) {
            return cfs_bf_id_equal(w->rename.ancestor.inode.parent, cursor) ?
                CFS_BS_OK : CFS_BS_ECORRUPT;
        }

        cursor = w->rename.ancestor.inode.parent;
        distance++;
        if (cfs_bf_id_equal(cursor, anchor)) {
            return CFS_BS_ECORRUPT;
        }

        if (distance == power) {
            anchor = cursor;
            distance = 0u;
            power *= 2u;
        }
    }
}

static int validate(cfs_bs_store_t *store, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *w) {
    cfs_bf_mutation_t *m = &w->mutation;
    cfs_bf_rename_state_t *r = &w->rename;
    cfs_bf_entry_t *parent = destination_parent(w);

    if (m->parent.inode.size == 0u ||
        cfs_bf_id_equal(m->target.id, m->view.root_id) ||
        cfs_bf_id_equal(m->target.id, m->parent.id) ||
        (m->target.inode.type == CFS_BF_DIRECTORY && m->parent.inode.links < 3u)) {
        return CFS_BS_ECORRUPT;
    }

    if (r->replaces &&
        (parent->inode.size == 0u ||
         cfs_bf_id_equal(r->replaced.id, m->view.root_id) ||
         cfs_bf_id_equal(r->replaced.id, parent->id))) {
        return CFS_BS_ECORRUPT;
    }

    if (r->replaces && (m->target.inode.type == CFS_BF_DIRECTORY) !=
        (r->replaced.inode.type == CFS_BF_DIRECTORY)) {
        return m->target.inode.type == CFS_BF_DIRECTORY ?
            CFS_BS_ENOTDIR : CFS_BS_EISDIR;
    }

    if (r->replaces && r->replaced.inode.type == CFS_BF_DIRECTORY) {
        int status = cfs_bf_directory(store, &m->view, r->replaced.id,
            &r->replaced, storage, w);
        if (status != CFS_BS_OK) {
            return status;
        }

        if (parent->inode.links < 3u) {
            return CFS_BS_ECORRUPT;
        }

        if (r->replaced.inode.size != 0u) {
            return CFS_BS_ENOTEMPTY;
        }
    }

    if (m->target.inode.type == CFS_BF_DIRECTORY && !r->same_parent) {
        return ancestry(store, storage, w);
    }

    return CFS_BS_OK;
}

static int metadata(cfs_bf_time_t time, cfs_bf_workspace_t *w) {
    cfs_bf_mutation_t *m = &w->mutation;
    cfs_bf_rename_state_t *r = &w->rename;
    cfs_bf_entry_t *parent = destination_parent(w);

    m->parent.inode.size--;
    if (m->target.inode.type == CFS_BF_DIRECTORY) {
        m->parent.inode.links--;
        if (r->replaces) {
            parent->inode.links--;
        }

        parent->inode.links++;
        m->target.inode.parent = parent->id;
    }

    if (!r->replaces) {
        if (parent->inode.size == INT64_MAX) {
            return CFS_BS_EOVERFLOW;
        }

        parent->inode.size++;
    }

    m->parent.inode.mtime = time;
    m->parent.inode.ctime = time;
    parent->inode.mtime = time;
    parent->inode.ctime = time;
    m->target.inode.ctime = time;
    if (cfs_bf_inode_frame(m->parent.inode, m->parent.inode.size != 0u,
            w->inode_bytes) != CFS_BS_OK ||
        cfs_bf_inode_frame(parent->inode, parent->inode.size != 0u,
            w->inode_bytes) != CFS_BS_OK) {
        return CFS_BS_ECORRUPT;
    }

    return CFS_BS_OK;
}

static int stage(cfs_bs_store_t *store, const void *source_name,
    uint16_t source_length, const void *destination_name,
    uint16_t destination_length, int keep_open, cfs_bf_time_t time,
    uint64_t *out_revision,
    cfs_bs_workspace_t *storage, cfs_bf_workspace_t *w) {
    cfs_bf_mutation_t *m = &w->mutation;
    cfs_bf_entry_t *parent = destination_parent(w);
    cfs_bt_change_t change = {.kind = CFS_BT_DELETE, .key = source_name,
        .key_length = source_length};
    cfs_bs_child_t source_names;
    cfs_bs_child_t destination_names;
    cfs_bs_child_t inodes = {0};
    int status = cfs_bt_edit(store, &w->stage, retained(m->parent.tree),
        change, &source_names, storage, &w->edit);
    if (status != CFS_BS_OK) {
        return status;
    }

    (void)cfs_bf_dirent_frame(m->target.id, w->dirent);
    change = (cfs_bt_change_t){.kind = w->rename.replaces ? CFS_BT_REPLACE : CFS_BT_INSERT,
        .key = destination_name, .key_length = destination_length,
        .value = w->dirent, .value_length = CFS_BF_ID_BYTES};
    status = cfs_bt_edit(store, &w->stage, w->rename.same_parent ?
        source_names : retained(parent->tree), change, &destination_names,
        storage, &w->edit);
    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bf_edit_inode(store, retained(m->view.inodes), m->parent.id,
        m->parent.inode, w->rename.same_parent ? destination_names : source_names,
        CFS_BT_REPLACE, &inodes, storage, w);
    if (status == CFS_BS_OK && !w->rename.same_parent) {
        status = cfs_bf_edit_inode(store, inodes, parent->id, parent->inode,
            destination_names, CFS_BT_REPLACE, &inodes, storage, w);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_edit_inode(store, inodes, m->target.id, m->target.inode,
            retained(m->target.tree), CFS_BT_REPLACE, &inodes, storage, w);
    }

    cfs_bf_roots_t roots = {inodes, retained(m->view.orphans)};
    if (status == CFS_BS_OK && w->rename.replaces) {
        status = cfs_bf_drop_link(store, &w->rename.replaced, keep_open, time,
            roots, &roots, storage, w);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_publish_roots(store, m->branch, m->view,
            roots.inodes, roots.orphans, out_revision, storage, w);
    }

    return status;
}

static int select_entries(cfs_bs_store_t *store, cfs_bf_id_t source_parent,
    const void *source_name, uint16_t source_length,
    cfs_bf_id_t destination, const void *destination_name,
    uint16_t destination_length, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *w) {
    cfs_bf_mutation_t *m = &w->mutation;
    cfs_bf_rename_state_t *r = &w->rename;
    r->same_parent = cfs_bf_id_equal(source_parent, destination);
    int status = cfs_bf_directory(store, &m->view, source_parent, &m->parent,
        storage, w);
    if (status == CFS_BS_OK) {
        status = cfs_bf_directory(store, &m->view, destination, &r->parent,
            storage, w);
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_lookup(store, &m->view, source_parent, source_name,
            source_length, &m->target, storage, w);
    }

    if (status != CFS_BS_OK) {
        return status;
    }

    status = cfs_bf_lookup(store, &m->view, destination, destination_name,
        destination_length, &r->replaced, storage, w);
    r->replaces = status == CFS_BS_OK;
    if (status != CFS_BS_OK && status != CFS_BS_ENOENT) {
        return status;
    }

    int same_binding = r->same_parent && source_length == destination_length &&
        memcmp(source_name, destination_name, source_length) == 0;
    uint64_t observed = r->same_parent && r->replaces && !same_binding &&
        (m->target.inode.type == CFS_BF_DIRECTORY) ==
        (r->replaced.inode.type == CFS_BF_DIRECTORY) ? 2u : 1u;
    status = binding_counts(&m->parent, m->target.inode.type, observed);
    if (status == CFS_BS_OK && r->replaces) {
        status = binding_counts(&r->parent, r->replaced.inode.type, observed);
    }

    return status;
}

static int rename_once(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t source_parent,
    const void *source_name, uint16_t source_length,
    cfs_bf_id_t destination, const void *destination_name,
    uint16_t destination_length, int keep_open, cfs_bf_time_t time,
    uint64_t *out_revision,
    cfs_bs_workspace_t *storage, cfs_bf_workspace_t *w) {
    int status = cfs_bf_prepare(store, storage, w);
    if (status != CFS_BS_OK) {
        return status;
    }

    cfs_bt_stage_reset(&w->stage);
    if (out_revision == NULL || !cfs_bf_id_valid(source_parent) ||
        !cfs_bf_id_valid(destination) || !cfs_bf_name_valid(source_name, source_length) ||
        !cfs_bf_name_valid(destination_name, destination_length) ||
        time.nanoseconds >= 1000000000u || (keep_open != 0 && keep_open != 1)) {
        return CFS_BS_EINVAL;
    }

    cfs_bf_mutation_t *m = &w->mutation;
    status = cfs_bs_branch_get(store, branch, &m->branch, storage);
    if (status == CFS_BS_OK && m->branch.revision != expected_revision) {
        status = CFS_BS_ECONFLICT;
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_read(store, m->branch.root, m->branch.revision, &m->view,
            storage, w);
    }

    if (status == CFS_BS_OK) {
        status = select_entries(store, source_parent, source_name, source_length,
            destination, destination_name, destination_length, storage, w);
    }

    if (status == CFS_BS_OK && w->rename.replaces &&
        cfs_bf_id_equal(m->target.id, w->rename.replaced.id)) {
        int same_name = w->rename.same_parent && source_length == destination_length &&
            memcmp(source_name, destination_name, source_length) == 0;
        if (cfs_bf_id_equal(m->target.id, m->view.root_id) ||
            m->parent.inode.size == 0u ||
            cfs_bf_id_equal(m->target.id, m->parent.id) ||
            (m->target.inode.type == CFS_BF_DIRECTORY && m->parent.inode.links < 3u) ||
            (!same_name && (m->target.inode.type == CFS_BF_DIRECTORY ||
                m->target.inode.links < 2u))) {
            return CFS_BS_ECORRUPT;
        }

        *out_revision = expected_revision;
        return CFS_BS_OK;
    }

    if (status == CFS_BS_OK) {
        status = validate(store, storage, w);
    }

    if (status == CFS_BS_OK) {
        status = metadata(time, w);
    }

    if (status == CFS_BS_OK) {
        status = stage(store, source_name, source_length, destination_name,
            destination_length, keep_open, time, out_revision, storage, w);
    }

    if (status != CFS_BS_OK) {
        cfs_bt_stage_discard(&w->stage);
    }

    return status;
}

int cfs_bf_rename(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t source_parent,
    const void *source_name, uint16_t source_length,
    cfs_bf_id_t destination, const void *destination_name,
    uint16_t destination_length, int keep_open, cfs_bf_time_t time,
    uint64_t *out_revision,
    cfs_bs_workspace_t *storage, cfs_bf_workspace_t *w) {
    int status = rename_once(store, branch, expected_revision, source_parent,
        source_name, source_length, destination, destination_name,
        destination_length, keep_open, time, out_revision, storage, w);

    if (status == CFS_BS_EWORKSPACE) {
        status = cfs_bf_retry_done(store, branch, rename_once(store, branch,
            expected_revision, source_parent, source_name, source_length,
            destination, destination_name, destination_length, keep_open,
            time, out_revision, storage, w), storage);
    }

    return status;
}
