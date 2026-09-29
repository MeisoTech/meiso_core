#include "meiso_action_private.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Owns public Action sizing and caller/executor arena initialization. */

size_t meiso_action_alignment_required(void)
{
    return _Alignof(MeisoMaxAlign);
}
size_t meiso_action_caller_memory_required(
    const MeisoResourceDescriptor *descriptor,
    const MeisoActionConfig *config
)
{
    size_t storage_offset;
    size_t slot_stride;
    size_t scratch_offset;
    size_t total;

    if (!meiso_action_private_descriptor_valid(descriptor) || !meiso_action_private_config_valid(descriptor, config) ||
        !meiso_action_private_layout_required(
            sizeof(MeisoActionCaller), config, &storage_offset, &slot_stride,
            &scratch_offset, &total
        )) {
        return 0U;
    }
    return total;
}

MeisoActionInitStatus meiso_action_caller_init(
    void *arena,
    size_t arena_size,
    const MeisoResourceDescriptor *descriptor,
    const MeisoActionConfig *config,
    MeisoActionCaller **out_caller
)
{
    MeisoResourceDescriptor descriptor_copy;
    MeisoActionConfig config_copy;
    MeisoActionCaller *caller;
    size_t storage_offset;
    size_t slot_stride;
    size_t scratch_offset;
    size_t required;

    if (out_caller == NULL) {
        return MEISO_ACTION_INIT_INVALID_ARGUMENT;
    }
    if (arena != NULL && meiso_action_private_output_overlaps_arena(
        arena, arena_size, out_caller, sizeof(*out_caller)
    )) {
        return MEISO_ACTION_INIT_INVALID_ARGUMENT;
    }
    *out_caller = NULL;
    if (!meiso_action_private_descriptor_valid(descriptor)) {
        return MEISO_ACTION_INIT_INVALID_DESCRIPTOR;
    }
    if (!meiso_action_private_config_valid(descriptor, config)) {
        return MEISO_ACTION_INIT_INVALID_CONFIG;
    }
    if (!meiso_action_private_arena_aligned(arena)) {
        return MEISO_ACTION_INIT_MISALIGNED_ARENA;
    }
    if (!meiso_action_private_layout_required(
            sizeof(MeisoActionCaller), config, &storage_offset, &slot_stride,
            &scratch_offset, &required
        ) || arena_size < required) {
        return MEISO_ACTION_INIT_ARENA_TOO_SMALL;
    }
    descriptor_copy = *descriptor;
    config_copy = *config;
    memset(arena, 0, required);
    caller = arena;
    caller->descriptor = descriptor_copy;
    caller->config = config_copy;
    caller->storage_offset = storage_offset;
    caller->slot_stride = slot_stride;
    caller->scratch_offset = scratch_offset;
    *out_caller = caller;
    return MEISO_ACTION_INIT_OK;
}

size_t meiso_action_executor_memory_required(
    const MeisoResourceDescriptor *descriptor,
    const MeisoActionConfig *config
)
{
    size_t storage_offset;
    size_t slot_stride;
    size_t scratch_offset;
    size_t total;

    if (!meiso_action_private_descriptor_valid(descriptor) || !meiso_action_private_config_valid(descriptor, config) ||
        !meiso_action_private_layout_required(
            sizeof(MeisoActionExecutor), config, &storage_offset, &slot_stride,
            &scratch_offset, &total
        )) {
        return 0U;
    }
    return total;
}

MeisoActionInitStatus meiso_action_executor_init(
    void *arena,
    size_t arena_size,
    const MeisoResourceDescriptor *descriptor,
    const MeisoActionConfig *config,
    MeisoOwnerScope executor_scope,
    MeisoActionExecutor **out_executor
)
{
    MeisoResourceDescriptor descriptor_copy;
    MeisoActionConfig config_copy;
    MeisoOwnerScope executor_copy;
    MeisoActionExecutor *executor;
    size_t storage_offset;
    size_t slot_stride;
    size_t scratch_offset;
    size_t required;

    if (out_executor == NULL) {
        return MEISO_ACTION_INIT_INVALID_ARGUMENT;
    }
    if (arena != NULL && meiso_action_private_output_overlaps_arena(
        arena, arena_size, out_executor, sizeof(*out_executor)
    )) {
        return MEISO_ACTION_INIT_INVALID_ARGUMENT;
    }
    *out_executor = NULL;
    if (!meiso_action_private_descriptor_valid(descriptor)) {
        return MEISO_ACTION_INIT_INVALID_DESCRIPTOR;
    }
    if (!meiso_action_private_config_valid(descriptor, config)) {
        return MEISO_ACTION_INIT_INVALID_CONFIG;
    }
    if (!meiso_owner_scope_valid(executor_scope) ||
        executor_scope.role != descriptor->owner) {
        return MEISO_ACTION_INIT_INVALID_EXECUTOR;
    }
    if (!meiso_action_private_arena_aligned(arena)) {
        return MEISO_ACTION_INIT_MISALIGNED_ARENA;
    }
    if (!meiso_action_private_layout_required(
            sizeof(MeisoActionExecutor), config, &storage_offset, &slot_stride,
            &scratch_offset, &required
        ) || arena_size < required) {
        return MEISO_ACTION_INIT_ARENA_TOO_SMALL;
    }
    descriptor_copy = *descriptor;
    config_copy = *config;
    executor_copy = executor_scope;
    memset(arena, 0, required);
    executor = arena;
    executor->descriptor = descriptor_copy;
    executor->config = config_copy;
    executor->executor = executor_copy;
    executor->storage_offset = storage_offset;
    executor->slot_stride = slot_stride;
    executor->scratch_offset = scratch_offset;
    *out_executor = executor;
    return MEISO_ACTION_INIT_OK;
}
