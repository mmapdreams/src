/* SPDX-License-Identifier: ISC
 *
 * branch_store_format_verify.c -- signature probing and read-back
 * verification for formatting a whole device in place.
 */

#include "branch_store_internal.h"

#include <string.h>

#include "block.h"
#include "codec.h"

/* Both recognized layouts keep the image identity at the same byte, so one
 * probe reports one identity whichever profile wrote the block. */
_Static_assert(CFS_BS_HEADER_UUID_OFF == CFS_A_FS_UUID_OFF,
    "branch header and anchor identities share an offset");
_Static_assert(CFS_BS_UUID_SIZE == CFS_A_FS_UUID_SIZE,
    "branch header and anchor identities share a width");

static uint32_t signature_kind(const uint8_t *page) {
    static const uint8_t profile_prefix[5] = {'C', 'F', 'S', 'B', 'R'};

    if (memcmp(page + CFS_BS_HEADER_MAGIC_OFF, profile_prefix,
            sizeof(profile_prefix)) == 0) {
        return CFS_BS_SIGNATURE_BRANCH;
    }

    if (cfs_get64(page + CFS_A_MAGIC_OFF) == CFS_ANCHOR_MAGIC) {
        return CFS_BS_SIGNATURE_ANCHOR;
    }

    return CFS_BS_SIGNATURE_NONE;
}

int cfs_bs_signature_probe(const cfs_block_backend *backend,
    cfs_bs_workspace_t *workspace, cfs_bs_signature_t *out) {
    uint64_t self;

    if (backend == NULL || workspace == NULL || out == NULL ||
        backend->dev.read == NULL || backend->block_count < 2u) {
        return CFS_BS_EINVAL;
    }

    memset(out, 0, sizeof(*out));
    for (self = CFS_BS_SUPER0_BLOCK; self <= CFS_BS_SUPER1_BLOCK; self++) {
        const uint8_t *page = workspace->scratch[self].bytes;
        uint32_t kind;

        if (cfs_block_read(backend, self, 1, workspace->scratch[self].bytes)
                != CFS_BLK_OK) {
            return CFS_BS_EIO;
        }

        kind = signature_kind(page);
        if (kind == CFS_BS_SIGNATURE_NONE) {
            continue;
        }

        if (out->copies == 0) {
            out->kind = kind;
            out->consistent = 1;
            memcpy(out->uuid, page + CFS_BS_HEADER_UUID_OFF,
                CFS_BS_UUID_SIZE);
        } else if (out->kind != kind ||
                   memcmp(out->uuid, page + CFS_BS_HEADER_UUID_OFF,
                       CFS_BS_UUID_SIZE) != 0) {
            out->consistent = 0;
        }

        out->copies++;
    }

    return CFS_BS_OK;
}

/* Fill the report's plan-derived fields. Shared so the bounded step and the
 * whole-operation call cannot disagree about the extent. */
static void verify_report_plan(const cfs_bs_format_plan_t *plan,
    cfs_bs_format_report_t *out) {
    memset(out, 0, sizeof(*out));
    out->total_blocks = plan->total_blocks;
    out->row_blocks = plan->row_blocks;
    out->semantic_start = plan->semantic_start;
    out->reserve = plan->reserve;
    out->metadata_blocks = plan->semantic_start;
    out->metadata_bytes = plan->semantic_start * CFS_BLOCK_SIZE;
    out->mismatched_block = CFS_BS_BLOCK_NONE;
}

int cfs_bs_format_verify_range(const cfs_block_backend *backend,
    const uint8_t uuid[CFS_BS_UUID_SIZE], uint64_t reserve,
    uint64_t first_block, uint64_t max_blocks,
    cfs_bs_workspace_t *workspace, cfs_bs_format_report_t *out) {
    cfs_bs_format_plan_t plan;
    uint64_t block;
    uint64_t limit;
    int rc;

    if (backend == NULL || uuid == NULL || workspace == NULL ||
        out == NULL || backend->dev.read == NULL) {
        return CFS_BS_EINVAL;
    }

    /* A zero budget asks for no progress; refuse it before any read rather
     * than returning a step that cannot advance. */
    if (max_blocks == 0) {
        return CFS_BS_EINVAL;
    }

    rc = cfs_bs_format_plan(uuid, backend->block_count, reserve,
        workspace->scratch[0].bytes, &plan);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    /* A position at or past the extent has nothing left to verify and is
     * a caller error, not a completion signal. */
    if (first_block >= plan.semantic_start) {
        return CFS_BS_EINVAL;
    }

    /* Refuse a budget whose end cannot be computed, rather than wrapping
     * into a short range that would look like progress. */
    if (max_blocks > UINT64_MAX - first_block) {
        return CFS_BS_EINVAL;
    }

    verify_report_plan(&plan, out);
    out->verified_blocks = first_block;
    out->verified_bytes = first_block * CFS_BLOCK_SIZE;

    limit = first_block + max_blocks;
    if (limit > plan.semantic_start) {
        limit = plan.semantic_start;
    }

    for (block = first_block; block < limit;) {
        uint64_t remaining = limit - block;
        uint32_t count = remaining > CFS_BS_TX_MAX_PAGES ?
            CFS_BS_TX_MAX_PAGES : (uint32_t)remaining;
        uint32_t index;

        if (cfs_block_read(backend, block, count, workspace->pages) !=
            CFS_BLK_OK) {
            /* Report the prefix this call did compare, in both units: a
             * block count without its byte count is an incoherent report. */
            out->verified_blocks = block;
            out->verified_bytes = block * CFS_BLOCK_SIZE;
            return CFS_BS_EIO;
        }

        for (index = 0; index < count; index++) {
            cfs_bs_format_frame_block(&plan, block + index,
                workspace->scratch[0].bytes);
            if (memcmp(workspace->pages[index].bytes,
                    workspace->scratch[0].bytes, CFS_BLOCK_SIZE) != 0) {
                /* The prefix stops AT the first differing block, never
                 * past it: a mismatch is not verified work. */
                out->verified_blocks = block + index;
                out->verified_bytes = out->verified_blocks * CFS_BLOCK_SIZE;
                out->mismatched_block = block + index;
                return CFS_BS_ECORRUPT;
            }
        }

        block += count;
        out->verified_blocks = block;
    }

    out->verified_bytes = out->verified_blocks * CFS_BLOCK_SIZE;
    return CFS_BS_OK;
}

int cfs_bs_format_verify(const cfs_block_backend *backend,
    const uint8_t uuid[CFS_BS_UUID_SIZE], uint64_t reserve,
    cfs_bs_workspace_t *workspace, cfs_bs_format_report_t *out) {
    cfs_bs_format_plan_t plan;
    uint64_t block;
    int rc;

    if (backend == NULL || uuid == NULL || workspace == NULL ||
        out == NULL || backend->dev.read == NULL) {
        return CFS_BS_EINVAL;
    }

    rc = cfs_bs_format_plan(uuid, backend->block_count, reserve,
        workspace->scratch[0].bytes, &plan);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    verify_report_plan(&plan, out);

    /* An empty extent is complete by inspection, and the bounded step
     * refuses a resume at the extent, so handle it here. */
    if (plan.semantic_start == 0) {
        return CFS_BS_OK;
    }

    /* Drive the bounded steps, so the comparison lives in one place. */
    for (block = 0; block < plan.semantic_start;) {
        rc = cfs_bs_format_verify_range(backend, uuid, reserve, block,
            CFS_BS_TX_MAX_PAGES, workspace, out);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        if (out->verified_blocks <= block) {
            /* A step that cannot advance would loop; report it rather
             * than spin, with the prefix it had reached. */
            out->verified_blocks = block;
            out->verified_bytes = block * CFS_BLOCK_SIZE;
            return CFS_BS_EIO;
        }

        block = out->verified_blocks;
    }

    out->verified_bytes = out->verified_blocks * CFS_BLOCK_SIZE;
    return CFS_BS_OK;
}
