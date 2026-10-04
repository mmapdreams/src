/* SPDX-License-Identifier: ISC
 * branch_tree_stage.c -- bounded object staging and reachable-graph sealing.
 */
#include "branch_tree_stage.h"

#include <string.h>

#include "branch_store_internal.h"

_Static_assert(CFS_BT_PAYLOAD_MAX == CFS_BLOCK_SIZE - CFS_BS_OBJECT_REFS_OFF,
    "staging payload storage matches outer object capacity");

void cfs_bt_stage_reset(cfs_bt_stage_t *stage) {
    if (stage) {
        stage->count = 0u;
        stage->state = CFS_BT_STAGE_EDITABLE;
        stage->sealed_count = 0u;
        memset(&stage->work, 0, sizeof(stage->work));
    }
}

void cfs_bt_stage_discard(cfs_bt_stage_t *stage) {
    if (stage) {
        stage->state = CFS_BT_STAGE_INVALID;
        stage->sealed_count = 0u;
    }
}

static int refuse(cfs_bt_stage_t *stage, int status) {
    cfs_bt_stage_discard(stage);
    return status;
}

static int editable(const cfs_bt_stage_t *stage) {
    return stage != NULL && stage->state == CFS_BT_STAGE_EDITABLE;
}

int cfs_bt_stage_clone(const cfs_bt_stage_t *source,
    cfs_bt_stage_t *destination) {
    if (!editable(source) || destination == NULL || source == destination ||
        source->count > CFS_BS_CONSTRUCT_MAX_OBJECTS) {
        return CFS_BS_EINVAL;
    }

    cfs_bt_stage_reset(destination);
    for (uint32_t i = 0u; i < source->count; i++) {
        destination->objects[i] = source->objects[i];
        destination->work.objects_cloned++;
    }

    destination->count = source->count;
    return CFS_BS_OK;
}

static int plan_valid(const cfs_bt_stage_t *stage,
    const cfs_bs_object_plan_t *plan) {
    if (plan == NULL || plan->child_count > CFS_BS_MAX_CHILDREN ||
        (plan->child_count != 0u && plan->children == NULL) ||
        (plan->payload_length != 0u && plan->payload == NULL) ||
        plan->payload_length > CFS_BT_PAYLOAD_MAX -
            plan->child_count * CFS_BS_REF_SIZE) {
        return 0;
    }

    for (uint32_t i = 0u; i < plan->child_count; i++) {
        const cfs_bs_child_t *child = &plan->children[i];
        if (child->kind == CFS_BS_CHILD_PREPARED) {
            if (child->value.prepared_index >= stage->count) {
                return 0;
            }
        } else if (child->kind == CFS_BS_CHILD_RETAINED) {
            if (child->value.retained.block == CFS_BS_BLOCK_NONE ||
                child->value.retained.incarnation == 0u) {
                return 0;
            }
        } else {
            return 0;
        }
    }

    return 1;
}

static void copy_plan(cfs_bt_staged_object_t *object,
    const cfs_bs_object_plan_t *plan) {
    object->child_count = plan->child_count;
    object->payload_length = plan->payload_length;
    if (plan->child_count != 0u) {
        memcpy(object->children, plan->children,
            plan->child_count * sizeof(*object->children));
    }

    if (plan->payload_length != 0u) {
        memcpy(object->payload, plan->payload, plan->payload_length);
    }
}

int cfs_bt_stage_add(cfs_bt_stage_t *stage, const cfs_bs_object_plan_t *plan,
    uint32_t *out_index) {
    if (!editable(stage) || out_index == NULL || !plan_valid(stage, plan)) {
        return refuse(stage, CFS_BS_EINVAL);
    }

    if (stage->count == CFS_BS_CONSTRUCT_MAX_OBJECTS) {
        return refuse(stage, CFS_BS_EWORKSPACE);
    }

    copy_plan(&stage->objects[stage->count], plan);
    *out_index = stage->count++;
    return CFS_BS_OK;
}

int cfs_bt_stage_replace(cfs_bt_stage_t *stage, uint32_t index,
    const cfs_bs_object_plan_t *plan) {
    if (!editable(stage) || index >= stage->count || !plan_valid(stage, plan)) {
        return refuse(stage, CFS_BS_EINVAL);
    }

    copy_plan(&stage->objects[index], plan);
    return CFS_BS_OK;
}

static cfs_bs_object_plan_t object_view(const cfs_bt_staged_object_t *object) {
    cfs_bs_object_plan_t plan = {
        .children = object->children,
        .child_count = object->child_count,
        .payload = object->payload,
        .payload_length = object->payload_length
    };
    return plan;
}

int cfs_bt_stage_view(const cfs_bt_stage_t *stage, uint32_t index,
    cfs_bs_object_plan_t *out) {
    if (!editable(stage) || index >= stage->count || out == NULL) {
        return CFS_BS_EINVAL;
    }

    *out = object_view(&stage->objects[index]);
    return CFS_BS_OK;
}

static void enter(cfs_bt_stage_t *stage, uint32_t depth, uint32_t index) {
    stage->path[depth].object = (uint16_t)index;
    stage->path[depth].next_child = 0u;
    stage->color[index] = 1u;
    stage->work.objects_visited++;
}

static int order_graph(cfs_bt_stage_t *stage, uint32_t root, uint32_t *count) {
    uint32_t depth = 0u;
    *count = 0u;
    memset(stage->color, 0, sizeof(stage->color));
    enter(stage, depth, root);
    for (;;) {
        cfs_bt_stage_frame_t *frame = &stage->path[depth];
        cfs_bt_staged_object_t *object = &stage->objects[frame->object];
        if (frame->next_child == object->child_count) {
            stage->color[frame->object] = 2u;
            stage->remap[frame->object] = (uint16_t)*count;
            stage->order[(*count)++] = frame->object;
            if (depth == 0u) {
                return CFS_BS_OK;
            }

            depth--;
            continue;
        }

        const cfs_bs_child_t *child = &object->children[frame->next_child++];
        stage->work.edges_visited++;
        if (child->kind == CFS_BS_CHILD_RETAINED) {
            continue;
        }

        uint32_t index = child->value.prepared_index;

        /* Edits admitted every index. Revisiting an active ancestor is a
         * user-supplied graph cycle, not an internal traversal failure. */
        if (stage->color[index] == 1u) {
            return CFS_BS_EINVAL;
        }

        if (stage->color[index] == 0u) {
            enter(stage, ++depth, index);
        }
    }
}

int cfs_bt_stage_compile(cfs_bt_stage_t *stage, uint32_t root,
    cfs_bt_stage_batch_t *out) {
    if (!editable(stage) || root >= stage->count || out == NULL) {
        return refuse(stage, CFS_BS_EINVAL);
    }

    uint32_t count;
    int rc = order_graph(stage, root, &count);
    if (rc != CFS_BS_OK) {
        return refuse(stage, rc);
    }

    for (uint32_t i = 0u; i < count; i++) {
        cfs_bt_staged_object_t *object = &stage->objects[stage->order[i]];
        for (uint32_t j = 0u; j < object->child_count; j++) {
            cfs_bs_child_t *child = &object->children[j];
            if (child->kind == CFS_BS_CHILD_PREPARED) {
                child->value.prepared_index =
                    stage->remap[child->value.prepared_index];
                stage->work.references_remapped++;
            }
        }

        stage->plans[i] = object_view(object);
    }

    stage->sealed_count = count;
    stage->state = CFS_BT_STAGE_SEALED;
    cfs_bt_stage_batch_t batch = { .plans = stage->plans, .count = count };
    *out = batch;
    return CFS_BS_OK;
}

int cfs_bt_stage_reopen(cfs_bt_stage_t *stage) {
    if (stage == NULL || stage->state != CFS_BT_STAGE_SEALED ||
        stage->sealed_count == 0u || stage->sealed_count > stage->count ||
        stage->count > CFS_BS_CONSTRUCT_MAX_OBJECTS) {
        return CFS_BS_EINVAL;
    }

    /* Only selected objects were remapped. Unreachable slots still carry
     * original local indices, including edges into the selected graph. */
    for (uint32_t i = 0u; i < stage->sealed_count; i++) {
        cfs_bt_staged_object_t *object = &stage->objects[stage->order[i]];
        stage->work.objects_visited++;
        for (uint32_t j = 0u; j < object->child_count; j++) {
            cfs_bs_child_t *child = &object->children[j];
            stage->work.edges_visited++;
            if (child->kind == CFS_BS_CHILD_PREPARED) {
                child->value.prepared_index =
                    stage->order[child->value.prepared_index];
                stage->work.references_restored++;
            }
        }
    }

    stage->sealed_count = 0u;
    stage->state = CFS_BT_STAGE_EDITABLE;
    return CFS_BS_OK;
}
