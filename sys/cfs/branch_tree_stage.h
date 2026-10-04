/* SPDX-License-Identifier: ISC
 * branch_tree_stage.h -- caller-owned graph staging for atomic tree edits.
 */
#ifndef CFS_BRANCH_TREE_STAGE_H
#define CFS_BRANCH_TREE_STAGE_H

#include "branch_tree_node.h"

enum {
    CFS_BT_STAGE_INVALID = 0,
    CFS_BT_STAGE_EDITABLE = 1,
    CFS_BT_STAGE_SEALED = 2
};

typedef struct cfs_bt_staged_object {
    cfs_bs_child_t children[CFS_BS_MAX_CHILDREN];
    uint8_t payload[CFS_BT_PAYLOAD_MAX];
    uint32_t child_count;
    uint32_t payload_length;
} cfs_bt_staged_object_t;

typedef struct cfs_bt_stage_frame {
    uint16_t object;
    uint16_t next_child;
} cfs_bt_stage_frame_t;

typedef struct cfs_bt_stage_work {
    uint64_t objects_visited;
    uint64_t edges_visited;
    uint64_t references_remapped;
    uint64_t objects_cloned;
    uint64_t references_restored;
} cfs_bt_stage_work_t;

/* Initialize before use. Fields are private except the diagnostic work.
 * All views borrow this storage; never copy the initialized context. */
typedef struct cfs_bt_stage {
    cfs_bt_staged_object_t objects[CFS_BS_CONSTRUCT_MAX_OBJECTS];
    cfs_bs_object_plan_t plans[CFS_BS_CONSTRUCT_MAX_OBJECTS];
    cfs_bt_stage_frame_t path[CFS_BS_CONSTRUCT_MAX_OBJECTS];
    uint16_t order[CFS_BS_CONSTRUCT_MAX_OBJECTS];
    uint16_t remap[CFS_BS_CONSTRUCT_MAX_OBJECTS];
    uint8_t color[CFS_BS_CONSTRUCT_MAX_OBJECTS];
    cfs_bt_stage_work_t work;
    uint32_t count;
    uint32_t state;
    uint32_t sealed_count;
} cfs_bt_stage_t;

typedef struct cfs_bt_stage_batch {
    const cfs_bs_object_plan_t *plans;
    uint32_t count;
} cfs_bt_stage_batch_t;

/* Reset/discard invalidate every prior local reference, view, and batch.
 * These functions accept NULL; all staging functions perform no I/O. */
void cfs_bt_stage_reset(cfs_bt_stage_t *stage);
void cfs_bt_stage_discard(cfs_bt_stage_t *stage);

/* Clone EDITABLE objects into disjoint fresh/inactive caller storage. Success
 * preserves local slots without borrowing source; failure preserves both. */
int cfs_bt_stage_clone(const cfs_bt_stage_t *source,
    cfs_bt_stage_t *destination);

/* PREPARED operands name local slots. Inputs never overlap context or outputs.
 * Every failure invalidates the context and preserves out. */
int cfs_bt_stage_add(cfs_bt_stage_t *stage, const cfs_bs_object_plan_t *plan,
    uint32_t *out_index);
int cfs_bt_stage_replace(cfs_bt_stage_t *stage, uint32_t index,
    const cfs_bs_object_plan_t *plan);

/* EDITABLE-only observation; failures preserve stage and out. Views borrow
 * storage through the next edit, reset, discard, or seal. */
int cfs_bt_stage_view(const cfs_bt_stage_t *stage, uint32_t index,
    cfs_bs_object_plan_t *out);

/* Return a borrowed child-before-parent batch and seal, without I/O or publish.
 * Keep context immutable and children retained through construction. */
int cfs_bt_stage_compile(cfs_bt_stage_t *stage, uint32_t root,
    cfs_bt_stage_batch_t *out);

/* Restore SEALED operands to local slots and resume editing. All batch/view
 * borrows must have ended. Wrong-state refusal preserves the context. */
int cfs_bt_stage_reopen(cfs_bt_stage_t *stage);

#endif /* CFS_BRANCH_TREE_STAGE_H */
