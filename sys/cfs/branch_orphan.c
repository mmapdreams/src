/* SPDX-License-Identifier: ISC
 * branch_orphan.c -- final-link retention and bounded orphan-index service.
 */
#include "branch_orphan.h"
#include "branch_fs_internal.h"
#include "codec.h"

static cfs_bs_child_t retained(cfs_bs_ref_t ref) {
    return (cfs_bs_child_t){.kind = CFS_BS_CHILD_RETAINED, .value.retained = ref};
}

int cfs_bf_drop_link(cfs_bs_store_t *store, cfs_bf_entry_t *entry,
    int keep_open, cfs_bf_time_t time, cfs_bf_roots_t roots,
    cfs_bf_roots_t *out, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *w) {
    (void)cfs_bf_id_key(entry->id, w->key);
    int status = cfs_bt_lookup(store, w->mutation.view.orphans, w->key,
        CFS_BF_ID_BYTES, &w->value, storage, &w->read);
    if (status != CFS_BS_ENOENT) {
        return status == CFS_BS_OK ? CFS_BS_ECORRUPT : status;
    }

    cfs_bt_change_t change = {.kind = CFS_BT_DELETE, .key = w->key,
        .key_length = CFS_BF_ID_BYTES};
    int survives = entry->inode.type != CFS_BF_DIRECTORY && entry->inode.links > 1u;
    if (survives || keep_open) {
        entry->inode.links = survives ? entry->inode.links - 1u : 0u;
        entry->inode.ctime = time;
        status = cfs_bf_edit_inode(store, roots.inodes, entry->id, entry->inode,
            retained(entry->tree), CFS_BT_REPLACE, &roots.inodes, storage, w);
        if (status == CFS_BS_OK && !survives) {
            change.kind = CFS_BT_INSERT;
            status = cfs_bt_edit(store, &w->stage, roots.orphans, change,
                &roots.orphans, storage, &w->edit);
        }
    } else {
        status = cfs_bt_edit(store, &w->stage, roots.inodes, change,
            &roots.inodes, storage, &w->edit);
    }

    if (status == CFS_BS_OK) {
        *out = roots;
    }

    return status;
}

static int checked_inode(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t id, cfs_bf_entry_t *out, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *w) {
    int status = cfs_bf_get(store, view, id, out, storage, w);
    if (status == CFS_BS_ENOENT || (status == CFS_BS_OK &&
            (out->inode.links != 0u || cfs_bf_id_equal(id, view->root_id)))) {
        return CFS_BS_ECORRUPT;
    }

    return status;
}

static int entry_lookup(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t id, cfs_bs_workspace_t *storage, cfs_bf_workspace_t *w) {
    (void)cfs_bf_id_key(id, w->key);
    int status = cfs_bt_lookup(store, view->orphans, w->key, CFS_BF_ID_BYTES,
        &w->value, storage, &w->read);
    if (status == CFS_BS_OK && (w->value.length != 0u ||
            w->value.child.block != CFS_BS_BLOCK_NONE || id.birth > view->revision)) {
        return CFS_BS_ECORRUPT;
    }

    return status;
}

typedef struct orphan_scan {
    cfs_bf_id_t after;
    cfs_bf_id_t selected;
    uint64_t revision;
    int status;
} orphan_scan_t;

static int visit(void *context, const cfs_bt_record_t *record,
    const cfs_bs_ref_t *child) {
    orphan_scan_t *scan = context;
    if (record->key_length != CFS_BF_ID_BYTES || record->value_length != 0u ||
        child != NULL) {
        scan->status = CFS_BS_ECORRUPT;
        return CFS_BT_SCAN_STOP;
    }

    const uint8_t *bytes = record->key;
    cfs_bf_id_t id = {cfs_get64be(bytes), cfs_get64be(bytes + 8u)};
    if (!cfs_bf_id_valid(id) || id.birth > scan->revision) {
        scan->status = CFS_BS_ECORRUPT;
        return CFS_BT_SCAN_STOP;
    }

    if (cfs_bf_id_equal(id, scan->after)) {
        return CFS_BT_SCAN_CONTINUE;
    }

    scan->selected = id;
    return CFS_BT_SCAN_STOP;
}

int cfs_bf_orphan_next(cfs_bs_store_t *store, const cfs_bf_view_t *view,
    cfs_bf_id_t after, cfs_bf_id_t *out, cfs_bs_workspace_t *storage,
    cfs_bf_workspace_t *w) {
    int status = cfs_bf_prepare(store, storage, w);
    if (status != CFS_BS_OK) {
        return status;
    }

    int first = after.birth == 0u && after.ordinal == 0u;
    if (view == NULL || out == NULL || (!first && !cfs_bf_id_valid(after))) {
        return CFS_BS_EINVAL;
    }

    if (!first) {
        (void)cfs_bf_id_key(after, w->key);
    }

    orphan_scan_t scan = {.after = after, .revision = view->revision};
    cfs_bt_scan_result_t result;
    status = cfs_bt_scan(store, view->orphans, first ? NULL : w->key,
        first ? 0u : CFS_BF_ID_BYTES, NULL, 0u, visit, &scan, &result,
        storage, &w->read);
    if (status != CFS_BS_OK || scan.status != CFS_BS_OK) {
        return status == CFS_BS_OK ? scan.status : status;
    }

    if (scan.selected.birth == 0u) {
        return CFS_BS_ENOENT;
    }

    cfs_bf_entry_t entry;
    status = checked_inode(store, view, scan.selected, &entry, storage, w);
    if (status == CFS_BS_OK) {
        *out = scan.selected;
    }

    return status;
}

int cfs_bf_orphan_reap(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, cfs_bf_id_t id, uint64_t *out_revision,
    cfs_bs_workspace_t *storage, cfs_bf_workspace_t *w) {
    return cfs_bf_orphan_reap_batch(store, branch, expected_revision, &id, 1u,
        out_revision, storage, w);
}

int cfs_bf_orphan_reap_batch(cfs_bs_store_t *store, uint64_t branch,
    uint64_t expected_revision, const cfs_bf_id_t *ids, uint32_t count,
    uint64_t *out_revision, cfs_bs_workspace_t *storage, cfs_bf_workspace_t *w) {
    int status = cfs_bf_prepare(store, storage, w);
    if (status != CFS_BS_OK) {
        return status;
    }

    cfs_bt_stage_reset(&w->stage);
    if (ids == NULL || count == 0u || count > CFS_BF_ORPHAN_BATCH_MAX ||
        out_revision == NULL) {
        return CFS_BS_EINVAL;
    }

    for (uint32_t i = 0u; i < count; i++) {
        if (!cfs_bf_id_valid(ids[i]) || (i != 0u &&
                (ids[i].birth < ids[i - 1u].birth ||
                    (ids[i].birth == ids[i - 1u].birth &&
                        ids[i].ordinal <= ids[i - 1u].ordinal)))) {
            return CFS_BS_EINVAL;
        }
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

    for (uint32_t i = 0u; status == CFS_BS_OK && i < count; i++) {
        status = entry_lookup(store, &m->view, ids[i], storage, w);
        if (status == CFS_BS_OK) {
            status = checked_inode(store, &m->view, ids[i], &m->target, storage, w);
        }
    }

    cfs_bs_child_t inodes = {0};
    cfs_bs_child_t orphans = {0};
    if (status == CFS_BS_OK) {
        inodes = retained(m->view.inodes);
        orphans = retained(m->view.orphans);
    }

    for (uint32_t i = 0u; status == CFS_BS_OK && i < count; i++) {
        (void)cfs_bf_id_key(ids[i], w->key);
        cfs_bt_change_t change = {.kind = CFS_BT_DELETE, .key = w->key,
            .key_length = CFS_BF_ID_BYTES};
        status = cfs_bt_edit(store, &w->stage, inodes, change,
            &inodes, storage, &w->edit);
        if (status == CFS_BS_OK) {
            status = cfs_bt_edit(store, &w->stage, orphans,
                change, &orphans, storage, &w->edit);
        }
    }

    if (status == CFS_BS_OK) {
        status = cfs_bf_publish_roots(store, m->branch, m->view, inodes, orphans,
            out_revision, storage, w);
    }

    if (status != CFS_BS_OK) {
        cfs_bt_stage_discard(&w->stage);
    }

    return status;
}
