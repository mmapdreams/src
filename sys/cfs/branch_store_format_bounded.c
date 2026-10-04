/* SPDX-License-Identifier: ISC
 *
 * branch_store_format_bounded.c -- stepwise formatting of an existing target.
 * RFC r0125 section 7.1 owns the contract; txt/format_publication.md describes
 * this implementation.
 */

#include "branch_store_internal.h"

#include <string.h>

#include "block.h"
#include "codec.h"

/* The caller owns the loop; the clean decision is deferred to finish so an
 * unfinished medium carries no signature. */
int cfs_bs_format_begin(const cfs_block_backend *backend,
    const uint8_t uuid[CFS_BS_UUID_SIZE], uint64_t reserve,
    cfs_bs_workspace_t *workspace, cfs_bs_format_cursor_t *out) {
    cfs_bs_format_plan_t plan;
    uint8_t *page;
    int rc;

    if (backend == NULL || uuid == NULL || workspace == NULL || out == NULL ||
        backend->dev.write == NULL || backend->dev.sync == NULL) {
        return CFS_BS_EINVAL;
    }

    page = workspace->scratch[0].bytes;
    rc = cfs_bs_format_plan(uuid, backend->block_count, reserve, page, &plan);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    /* Invalidate every record recovery consults, then sync. The odd-parity
     * commit sits at the metadata tail, so a low range would miss it. */
    memset(page, 0, CFS_BLOCK_SIZE);
    {
        const uint64_t gating[] = {
            CFS_BS_SUPER0_BLOCK,
            CFS_BS_SUPER1_BLOCK,
            CFS_BS_CLEAN_BLOCK,
            CFS_BS_COMMIT_BLOCK,
            plan.semantic_start - CFS_BS_REDO_BLOCKS,
        };
        size_t i;

        for (i = 0; i < sizeof(gating) / sizeof(gating[0]); i++) {
            rc = cfs_bs_format_write_block(backend, gating[i], page);
            if (rc != CFS_BS_OK) {
                return rc;
            }
        }
    }

    if (cfs_block_sync(backend) != CFS_BLK_OK) {
        return CFS_BS_EIO;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->uuid, uuid, CFS_BS_UUID_SIZE);
    out->total_blocks = plan.total_blocks;
    out->reserve = plan.reserve;
    out->semantic_start = plan.semantic_start;
    out->next_block = 0;
    out->invalidated = 1;
    return CFS_BS_OK;
}

/* Recover the plan from the cursor; the cursor carries every field the plan
 * is a pure function of, so a step never trusts stored geometry. */
static int cursor_plan(const cfs_block_backend *backend,
    const cfs_bs_format_cursor_t *cursor, cfs_bs_workspace_t *workspace,
    cfs_bs_format_plan_t *out) {
    if (backend == NULL || cursor == NULL || workspace == NULL ||
        cursor->invalidated == 0 ||
        cursor->total_blocks != backend->block_count ||
        cursor->semantic_start != cfs_bs_semantic_start(cursor->total_blocks) ||
        cursor->next_block > cursor->semantic_start ||
        cursor->written_blocks > cursor->semantic_start) {
        return CFS_BS_EINVAL;
    }

    return cfs_bs_format_plan(cursor->uuid, cursor->total_blocks,
        cursor->reserve, workspace->scratch[0].bytes, out);
}

int cfs_bs_format_step(const cfs_block_backend *backend,
    cfs_bs_format_cursor_t *cursor, uint32_t max_pages,
    cfs_bs_workspace_t *workspace, uint32_t *out_written) {
    cfs_bs_format_plan_t plan;
    uint64_t remaining;
    uint64_t block;
    uint32_t count;
    uint32_t index;
    int rc;

    if (!out_written) {
        return CFS_BS_EINVAL;
    }

    *out_written = 0;
    if (max_pages < CFS_BS_FORMAT_STEP_MIN_PAGES ||
        max_pages > CFS_BS_TX_MAX_PAGES) {
        return CFS_BS_EINVAL;
    }

    rc = cursor_plan(backend, cursor, workspace, &plan);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (cursor->sealed != 0 || cursor->synced != 0) {
        return CFS_BS_EINVAL;
    }

    if (cursor->next_block >= plan.semantic_start) {
        return CFS_BS_OK;
    }

    block = cursor->next_block;
    remaining = plan.semantic_start - block;
    count = remaining > max_pages ? max_pages : (uint32_t)remaining;
    for (index = 0; index < count; index++) {
        uint64_t target = block + index;

        if (target == CFS_BS_CLEAN_BLOCK) {
            /* Deferred to finish: zero it now so a partial medium carries
             * no decision, and rewrite the framed bytes when sealing. */
            memset(workspace->pages[index].bytes, 0, CFS_BLOCK_SIZE);
            continue;
        }

        cfs_bs_format_frame_block(&plan, target,
            workspace->pages[index].bytes);
    }

    if (cfs_block_write(backend, block, count, workspace->pages) !=
        CFS_BLK_OK) {
        return CFS_BS_EIO;
    }

    cursor->next_block = block + count;
    cursor->written_blocks += count;
    *out_written = count;
    return CFS_BS_OK;
}

int cfs_bs_format_finish(const cfs_block_backend *backend,
    cfs_bs_format_cursor_t *cursor, cfs_bs_workspace_t *workspace) {
    cfs_bs_format_plan_t plan;
    int rc;

    rc = cursor_plan(backend, cursor, workspace, &plan);
    if (rc != CFS_BS_OK) {
        return rc;
    }

    if (cursor->synced != 0 ||
        cursor->next_block != plan.semantic_start ||
        cursor->written_blocks != plan.semantic_start) {
        return CFS_BS_EINVAL;
    }

    if (cursor->sealed == 0) {
        /* The clean decision binds three home pages but no row page, so the
         * metadata must be durable before publication is issued. */
        if (cfs_block_sync(backend) != CFS_BLK_OK) {
            return CFS_BS_EIO;
        }

        cfs_bs_format_frame_block(&plan, CFS_BS_CLEAN_BLOCK,
            workspace->scratch[0].bytes);
        rc = cfs_bs_format_write_block(backend, CFS_BS_CLEAN_BLOCK,
            workspace->scratch[0].bytes);
        if (rc != CFS_BS_OK) {
            return rc;
        }

        cursor->sealed = 1;
    }

    if (cfs_block_sync(backend) != CFS_BLK_OK) {
        return CFS_BS_EIO;
    }

    cursor->synced = 1;
    return CFS_BS_OK;
}
