#include "meiso_action_private.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Owns common Action arena layout, validation, and record/view helpers. */

static bool checked_add(size_t lhs, size_t rhs, size_t *out)
{
    if (rhs > SIZE_MAX - lhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

static bool checked_multiply(size_t lhs, size_t rhs, size_t *out)
{
    if (lhs != 0U && rhs > SIZE_MAX / lhs) {
        return false;
    }
    *out = lhs * rhs;
    return true;
}

static bool align_up(size_t value, size_t alignment, size_t *out)
{
    const size_t remainder = value % alignment;

    if (remainder == 0U) {
        *out = value;
        return true;
    }
    return checked_add(value, alignment - remainder, out);
}

bool meiso_action_private_output_overlaps_arena(
    const void *arena,
    size_t arena_size,
    const void *output,
    size_t output_size
)
{
    const uintptr_t arena_start = (uintptr_t)arena;
    const uintptr_t output_start = (uintptr_t)output;
    uintptr_t arena_end;
    uintptr_t output_end;

    if (arena_size > UINTPTR_MAX - arena_start ||
        output_size > UINTPTR_MAX - output_start) {
        return true;
    }
    arena_end = arena_start + arena_size;
    output_end = output_start + output_size;
    return arena_start < output_end && output_start < arena_end;
}

bool meiso_action_private_descriptor_valid(const MeisoResourceDescriptor *descriptor)
{
    return meiso_resource_descriptor_validate(descriptor) == MEISO_DESCRIPTOR_VALID &&
        descriptor->kind == MEISO_RESOURCE_KIND_ACTION;
}

bool meiso_action_private_config_valid(
    const MeisoResourceDescriptor *descriptor,
    const MeisoActionConfig *config
)
{
    return config != NULL && config->slot_count > 0U &&
        config->max_argument_size <= descriptor->max_payload_size &&
        config->max_result_size <= descriptor->max_payload_size;
}

static bool retry_policy_valid(MeisoActionRetryPolicy policy)
{
    return policy == MEISO_ACTION_RETRY_NONE ||
        policy == MEISO_ACTION_RETRY_IDEMPOTENT;
}

bool meiso_action_private_terminal_state(MeisoActionState state)
{
    return state == MEISO_ACTION_STATE_SUCCEEDED ||
        state == MEISO_ACTION_STATE_FAILED ||
        state == MEISO_ACTION_STATE_REJECTED ||
        state == MEISO_ACTION_STATE_EXPIRED ||
        state == MEISO_ACTION_STATE_INDETERMINATE;
}

bool meiso_action_private_request_valid(const MeisoActionRequest *request)
{
    return request != NULL && meiso_action_id_valid(request->id) &&
        meiso_owner_scope_valid(request->expected_executor) &&
        meiso_resource_id_valid(request->target) &&
        meiso_schema_id_valid(request->operation.id) &&
        request->operation.version != 0U &&
        meiso_byte_view_valid(request->arguments) &&
        meiso_deadline_valid(request->sender_deadline) &&
        retry_policy_valid(request->retry_policy);
}

bool meiso_action_private_result_valid(const MeisoActionResult *result)
{
    return result != NULL && meiso_action_id_valid(result->id) &&
        meiso_owner_scope_valid(result->executor) &&
        (result->state == MEISO_ACTION_STATE_ACCEPTED ||
         meiso_action_private_terminal_state(result->state)) &&
        meiso_byte_view_valid(result->payload) &&
        (result->state != MEISO_ACTION_STATE_ACCEPTED ||
         result->payload.size == 0U);
}

static bool deadline_equal(MeisoDeadline lhs, MeisoDeadline rhs)
{
    if (lhs.present != rhs.present) {
        return false;
    }
    return !lhs.present ||
        (lhs.value.clock_epoch == rhs.value.clock_epoch &&
         lhs.value.monotonic_ns == rhs.value.monotonic_ns);
}

bool meiso_action_private_bytes_equal(
    const uint8_t *stored,
    size_t stored_size,
    MeisoByteView candidate
)
{
    return stored_size == candidate.size &&
        (stored_size == 0U || memcmp(stored, candidate.data, stored_size) == 0);
}

bool meiso_action_private_layout_required(
    size_t handle_size,
    const MeisoActionConfig *config,
    size_t *out_storage_offset,
    size_t *out_slot_stride,
    size_t *out_scratch_offset,
    size_t *out_total
)
{
    const size_t alignment = meiso_action_alignment_required();
    const size_t scratch_size = config->max_argument_size > config->max_result_size
        ? config->max_argument_size : config->max_result_size;
    size_t slot_bytes;
    size_t all_slots;
    size_t slot_stride;
    size_t storage_offset;
    size_t scratch_offset;
    size_t total;

    if (!checked_add(
            sizeof(MeisoActionPrivateSlot), config->max_argument_size, &slot_bytes
        ) ||
        !checked_add(slot_bytes, config->max_result_size, &slot_bytes) ||
        !align_up(slot_bytes, alignment, &slot_stride) ||
        !align_up(handle_size, alignment, &storage_offset) ||
        !checked_multiply((size_t)config->slot_count, slot_stride, &all_slots) ||
        !checked_add(storage_offset, all_slots, &scratch_offset) ||
        !checked_add(scratch_offset, scratch_size, &total)) {
        return false;
    }
    *out_storage_offset = storage_offset;
    *out_slot_stride = slot_stride;
    *out_scratch_offset = scratch_offset;
    *out_total = total;
    return true;
}

MeisoActionPrivateSlot *meiso_action_private_slot_at(
    const void *handle,
    size_t storage_offset,
    size_t slot_stride,
    uint32_t index
)
{
    return (MeisoActionPrivateSlot *)(void *)((uint8_t *)(void *)handle +
        storage_offset + (size_t)index * slot_stride);
}

uint8_t *meiso_action_private_slot_arguments(MeisoActionPrivateSlot *slot)
{
    return (uint8_t *)(void *)(slot + 1);
}

uint8_t *meiso_action_private_slot_result(
    const MeisoActionConfig *config,
    MeisoActionPrivateSlot *slot
)
{
    return meiso_action_private_slot_arguments(slot) + config->max_argument_size;
}

bool meiso_action_private_request_equal(
    const MeisoActionConfig *config,
    MeisoActionPrivateSlot *slot,
    const MeisoActionRequest *request
)
{
    (void)config;
    return meiso_action_id_equal(slot->id, request->id) &&
        meiso_owner_scope_equal(
            slot->expected_executor, request->expected_executor
        ) &&
        meiso_resource_id_equal(slot->target, request->target) &&
        meiso_schema_ref_equal(slot->operation, request->operation) &&
        deadline_equal(slot->sender_deadline, request->sender_deadline) &&
        slot->retry_policy == request->retry_policy &&
        meiso_action_private_bytes_equal(
            meiso_action_private_slot_arguments(slot), slot->argument_size, request->arguments
        );
}

void meiso_action_private_fill_slot_from_staged_request(
    const MeisoActionConfig *config,
    size_t slot_stride,
    MeisoActionPrivateSlot *slot,
    const MeisoActionRequest *request,
    const uint8_t *staged,
    MeisoDeliveryGeneration delivery,
    MeisoActionState state
)
{
    memset(slot, 0, slot_stride);
    slot->id = request->id;
    slot->expected_executor = request->expected_executor;
    slot->target = request->target;
    slot->operation = request->operation;
    slot->sender_deadline = request->sender_deadline;
    slot->delivery = delivery;
    slot->argument_size = request->arguments.size;
    slot->retry_policy = request->retry_policy;
    slot->state = state;
    if (request->arguments.size != 0U) {
        memmove(meiso_action_private_slot_arguments(slot), staged, request->arguments.size);
    }
    (void)config;
}

void meiso_action_private_request_view(
    const MeisoActionConfig *config,
    MeisoActionPrivateSlot *slot,
    MeisoActionRequest *out_request
)
{
    (void)config;
    out_request->id = slot->id;
    out_request->expected_executor = slot->expected_executor;
    out_request->target = slot->target;
    out_request->operation = slot->operation;
    out_request->arguments.data = meiso_action_private_slot_arguments(slot);
    out_request->arguments.size = slot->argument_size;
    out_request->sender_deadline = slot->sender_deadline;
    out_request->retry_policy = slot->retry_policy;
}

void meiso_action_private_entry_view(
    const MeisoActionConfig *config,
    MeisoActionPrivateSlot *slot,
    MeisoActionEntryView *out_view
)
{
    meiso_action_private_request_view(config, slot, &out_view->request);
    out_view->delivery = slot->delivery;
    out_view->state = slot->state;
    out_view->delivery_accepted = slot->delivery_accepted ? 1U : 0U;
    out_view->result.data = meiso_action_private_slot_result(config, slot);
    out_view->result.size = slot->result_size;
}
