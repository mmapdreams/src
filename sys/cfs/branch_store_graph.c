/* SPDX-License-Identifier: ISC
 *
 * branch_store_graph.c -- bounded successor graph authentication shared by
 * construction preflight and redo recovery.
 */
#include "branch_store_internal.h"

#include <string.h>

#include "codec.h"

static int descriptor_index(const cfs_bs_workspace_t *workspace,
    uint32_t count, uint64_t destination, uint32_t *out) {
    for (uint32_t i = 0; i < count; i++) {
        if (workspace->destination[i] == destination) {
            *out = i;
            return 1;
        }
    }

    return 0;
}

int cfs_bs_effective_home(cfs_bs_store_t *store,
    cfs_bs_workspace_t *workspace, uint32_t count, uint64_t block,
    uint64_t generation, uint8_t *scratch, const uint8_t **out) {
    uint32_t index;
    int rc;

    if (descriptor_index(workspace, count, block, &index)) {
        if (cfs_get64(workspace->pages[index].bytes +
                          CFS_BS_HEADER_GENERATION_OFF) > generation) {
            return CFS_BS_ECORRUPT;
        }

        *out = workspace->pages[index].bytes;
        return CFS_BS_OK;
    }

    cfs_bs_cache_slot_t *slot;
    uint32_t kind = cfs_bs_home_kind(store, block);

    if (kind == 0u) {
        return CFS_BS_EINVAL;
    }

    rc = cfs_bs_io_read_slot(store, block, scratch, &slot);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    /* A cached page keeps its whole-page validation until rewritten. */
    if (!slot || slot->validated != (kind | CFS_BS_VALIDATED_DEEP)) {
        rc = cfs_bs_validate_home_page(store, block, scratch, NULL);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        if (slot) {
            slot->validated = kind | CFS_BS_VALIDATED_DEEP;
            store->cache->validations++;
        }
    }

    if (cfs_get64(scratch + CFS_BS_HEADER_GENERATION_OFF) > generation) {
        return CFS_BS_ECORRUPT;
    }

    *out = scratch;
    return CFS_BS_OK;
}

static int validate_effective_row(cfs_bs_store_t *store,
    cfs_bs_workspace_t *workspace, uint32_t count, cfs_bs_ref_t ref,
    uint64_t generation, uint8_t *scratch, cfs_bs_row_t *out_row) {
    const uint8_t *row_page;
    const uint8_t *raw;
    cfs_bs_row_t row;
    int rc;

    rc = cfs_bs_effective_home(store, workspace, count, cfs_bs_row_page(ref.block),
                        generation, scratch, &row_page);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    raw = row_page + CFS_BS_HEADER_SIZE +
          (uint64_t)cfs_bs_row_index(ref.block) * CFS_BS_ROW_SIZE;
    cfs_bs_row_decode(raw, &row);
    if (row.state != CFS_BS_ROW_LIVE || row.refcount == 0 ||
        row.incarnation != ref.incarnation) {
        return CFS_BS_ECORRUPT;
    }

    if (out_row) {
        *out_row = row;
    }

    return CFS_BS_OK;
}

int cfs_bs_validate_catalog_roots(cfs_bs_store_t *store,
    cfs_bs_workspace_t *workspace, uint32_t count,
    const uint8_t *branches, const uint8_t *pins, uint64_t generation) {
    const uint8_t *catalogs[2];
    uint32_t which;
    uint32_t slot;
    int rc;

    catalogs[0] = branches;
    catalogs[1] = pins;
    for (which = 0; which < 2; which++) {
        for (slot = 0; slot < CFS_BS_MAX_BRANCHES; slot++) {
            cfs_bs_catalog_entry_t entry;
            uint32_t child_count;
            uint32_t payload_length;

            rc = cfs_bs_catalog_entry_decode_checked(
                store, catalogs[which], slot, which != 0, &entry);
            if (rc == CFS_BS_ENOENT) {
                continue;
            }

            if (rc != CFS_BS_OK) {
                return rc;
            }

            if (cfs_bs_ref_is_null(entry.root)) {
                continue;
            }

            if ((entry.root.incarnation >> CFS_BS_INCARNATION_INDEX_BITS) > generation) {
                return CFS_BS_ECORRUPT;
            }

            rc = validate_effective_row(store, workspace, count, entry.root, generation,
                                        workspace->scratch[0].bytes, NULL);
            if (rc != CFS_BS_OK) {
                return rc;
            }

            rc = cfs_bs_io_read(store, entry.root.block,
                                workspace->scratch[0].bytes);
            if (rc != CFS_BS_OK) {
                return rc;
            }

            rc = cfs_bs_object_page_validate(
                store, entry.root, workspace->scratch[0].bytes,
                &child_count, &payload_length);
            if (rc != CFS_BS_OK) {
                return rc;
            }
        }
    }

    return CFS_BS_OK;
}


static int listed_ref(const cfs_bs_workspace_t *workspace,
    uint32_t prepared_count, uint64_t generation, cfs_bs_ref_t ref) {
    uint32_t index = (uint32_t)(ref.incarnation & CFS_BS_INCARNATION_INDEX_MASK);

    return (ref.incarnation >> CFS_BS_INCARNATION_INDEX_BITS) == generation &&
        index < prepared_count &&
        cfs_bs_ref_equal(ref, workspace->prepared[index]);
}

static int prepared_catalogs(cfs_bs_store_t *store,
    cfs_bs_workspace_t *workspace, uint32_t count,
    uint32_t prepared_count, uint64_t generation) {
    uint32_t roots = 0;

    for (uint32_t which = 0; which < 2; which++) {
        const uint8_t *page;
        int rc = cfs_bs_effective_home(store, workspace, count,
            which == 0 ? CFS_BS_BRANCH_BLOCK : CFS_BS_PIN_BLOCK,
            generation, workspace->scratch[2].bytes, &page);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        for (uint32_t slot = 0; slot < CFS_BS_MAX_BRANCHES; slot++) {
            cfs_bs_catalog_entry_t entry;

            rc = cfs_bs_catalog_entry_decode_checked(store, page, slot,
                                                      which != 0, &entry);
            if (rc == CFS_BS_ENOENT) {
                continue;
            }

            if (rc != CFS_BS_OK) {
                return rc;
            }

            if (cfs_bs_ref_is_null(entry.root)) {
                continue;
            }

            if ((entry.root.incarnation >> CFS_BS_INCARNATION_INDEX_BITS) > generation) {
                return CFS_BS_ECORRUPT;
            }

            if ((entry.root.incarnation >> CFS_BS_INCARNATION_INDEX_BITS) == generation) {
                if (!listed_ref(workspace, prepared_count, generation,
                                 entry.root) || which != 0 ||
                    (entry.root.incarnation & CFS_BS_INCARNATION_INDEX_MASK) != prepared_count - 1u) {
                    return CFS_BS_ECORRUPT;
                }

                roots++;
            }
        }
    }

    return roots == (prepared_count == 0 ? 0u : 1u) ?
        CFS_BS_OK : CFS_BS_ECORRUPT;
}

static int prepared_rows(cfs_bs_store_t *store,
    cfs_bs_workspace_t *workspace, uint32_t count,
    uint32_t prepared_count, uint64_t generation) {
    for (uint32_t i = 0; i < count; i++) {
        uint64_t page_block = workspace->destination[i];
        if (page_block < CFS_BS_ROW_START) {
            continue;
        }

        for (uint32_t slot = 0; slot < CFS_BS_ROWS_PER_BLOCK; slot++) {
            cfs_bs_row_t row;
            uint64_t block = (page_block - CFS_BS_ROW_START) *
                CFS_BS_ROWS_PER_BLOCK + slot;
            uint32_t ordinal;

            cfs_bs_row_decode(workspace->pages[i].bytes +
                CFS_BS_HEADER_SIZE + (size_t)slot * CFS_BS_ROW_SIZE, &row);
            if ((row.incarnation >> CFS_BS_INCARNATION_INDEX_BITS) > generation) {
                return CFS_BS_ECORRUPT;
            }

            if ((row.incarnation >> CFS_BS_INCARNATION_INDEX_BITS) != generation) {
                continue;
            }

            ordinal = (uint32_t)(row.incarnation & CFS_BS_INCARNATION_INDEX_MASK);
            if (block < store->semantic_start ||
                block >= store->total_blocks || ordinal >= prepared_count ||
                row.state != CFS_BS_ROW_LIVE ||
                row.next != CFS_BS_BLOCK_NONE ||
                workspace->prepared[ordinal].block != block ||
                row.refcount != workspace->incoming[ordinal]) {
                return CFS_BS_ECORRUPT;
            }
        }
    }

    return CFS_BS_OK;
}

static int prepared_children(cfs_bs_store_t *store,
    cfs_bs_workspace_t *workspace, uint32_t count,
    uint32_t index, uint64_t generation, uint32_t child_count) {
    const uint8_t *object = workspace->objects[index].bytes;

    for (uint32_t edge = 0; edge < child_count; edge++) {
        cfs_bs_ref_t child;
        int rc;

        cfs_bs_ref_decode(object + CFS_BS_OBJECT_REFS_OFF +
                              (size_t)edge * CFS_BS_REF_SIZE, &child);
        if ((child.incarnation >> CFS_BS_INCARNATION_INDEX_BITS) == generation) {
            uint32_t ordinal = (uint32_t)(child.incarnation & CFS_BS_INCARNATION_INDEX_MASK);

            if (!listed_ref(workspace, index, generation, child)) {
                return CFS_BS_ECORRUPT;
            }

            workspace->incoming[ordinal]++;
            continue;
        }

        if ((child.incarnation >> CFS_BS_INCARNATION_INDEX_BITS) >= generation) {
            return CFS_BS_ECORRUPT;
        }

        rc = validate_effective_row(store, workspace, count, child, generation,
                                     workspace->scratch[0].bytes, NULL);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        rc = cfs_bs_io_read(store, child.block, workspace->scratch[0].bytes);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        rc = cfs_bs_object_page_validate(store, child,
            workspace->scratch[0].bytes, NULL, NULL);
        if (rc != CFS_BS_OK) {
            return rc;
        }
    }

    return CFS_BS_OK;
}

int cfs_bs_validate_prepared(cfs_bs_store_t *store,
    cfs_bs_workspace_t *workspace, uint32_t count, uint32_t prepared_count,
    uint64_t generation, int staged) {
    int rc;

    if (prepared_count > CFS_BS_CONSTRUCT_MAX_OBJECTS) {
        return CFS_BS_EINVAL;
    }

    memset(workspace->incoming, 0, sizeof(workspace->incoming));
    for (uint32_t i = 0; i < prepared_count; i++) {
        cfs_bs_ref_t ref = workspace->prepared[i];
        uint32_t child_count;

        if (ref.incarnation != ((generation << CFS_BS_INCARNATION_INDEX_BITS) | i)) {
            return CFS_BS_ECORRUPT;
        }

        if (!staged) {
            rc = cfs_bs_io_read(store, ref.block, workspace->objects[i].bytes);
            if (rc != CFS_BS_OK) {
                return rc;
            }
        }

        rc = cfs_bs_object_page_validate(store, ref,
            workspace->objects[i].bytes, &child_count, NULL);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        rc = prepared_children(store, workspace, count, i, generation,
                                child_count);
        if (rc != CFS_BS_OK) {
            return rc;
        }
    }

    if (prepared_count != 0) {
        workspace->incoming[prepared_count - 1u]++;
    }

    for (uint32_t i = 0; i < prepared_count; i++) {
        cfs_bs_row_t row;
        uint32_t page_index;

        /* Every new row must be in redo, never an unbound home page. */
        if (!descriptor_index(workspace, count,
                cfs_bs_row_page(workspace->prepared[i].block), &page_index)) {
            return CFS_BS_ECORRUPT;
        }

        rc = validate_effective_row(store, workspace, count,
            workspace->prepared[i], generation,
            workspace->scratch[0].bytes, &row);
        if (rc != CFS_BS_OK || workspace->incoming[i] == 0 ||
            row.refcount != workspace->incoming[i]) {
            return CFS_BS_ECORRUPT;
        }
    }

    rc = prepared_rows(store, workspace, count, prepared_count, generation);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    return prepared_catalogs(store, workspace, count, prepared_count,
                               generation);
}
