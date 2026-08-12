#include "meiso_action.h"

#include "meiso_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    MeisoActionId id;
    MeisoOwnerScope expected_executor;
    MeisoResourceId target;
    MeisoSchemaRef operation;
    MeisoDeadline sender_deadline;
    MeisoTimePoint mapped_local_deadline;
    MeisoDeliveryGeneration delivery;
    size_t argument_size;
    size_t result_size;
    MeisoActionRetryPolicy retry_policy;
    MeisoActionState state;
    bool delivery_accepted;
    bool has_mapped_deadline;
} MeisoActionSlot;

struct MeisoActionCaller {
    MeisoResourceDescriptor descriptor;
    MeisoActionConfig config;
    MeisoDeliveryGeneration delivery;
    size_t storage_offset;
    size_t slot_stride;
    size_t scratch_offset;
    uint32_t count;
    MeisoActionCallerStats stats;
    bool delivery_active;
};

struct MeisoActionExecutor {
    MeisoResourceDescriptor descriptor;
    MeisoActionConfig config;
    MeisoOwnerScope executor;
    size_t storage_offset;
    size_t slot_stride;
    size_t scratch_offset;
    uint32_t count;
    MeisoActionExecutorStats stats;
};

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

static bool output_overlaps_arena(
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

static bool descriptor_valid(const MeisoResourceDescriptor *descriptor)
{
    return meiso_resource_descriptor_validate(descriptor) == MEISO_DESCRIPTOR_VALID &&
        descriptor->kind == MEISO_RESOURCE_KIND_ACTION;
}

static bool config_valid(
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

static bool terminal_state(MeisoActionState state)
{
    return state == MEISO_ACTION_STATE_SUCCEEDED ||
        state == MEISO_ACTION_STATE_FAILED ||
        state == MEISO_ACTION_STATE_REJECTED ||
        state == MEISO_ACTION_STATE_EXPIRED ||
        state == MEISO_ACTION_STATE_INDETERMINATE;
}

static bool request_valid(const MeisoActionRequest *request)
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

static bool result_valid(const MeisoActionResult *result)
{
    return result != NULL && meiso_action_id_valid(result->id) &&
        meiso_owner_scope_valid(result->executor) &&
        (result->state == MEISO_ACTION_STATE_ACCEPTED ||
         terminal_state(result->state)) &&
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

static bool bytes_equal(
    const uint8_t *stored,
    size_t stored_size,
    MeisoByteView candidate
)
{
    return stored_size == candidate.size &&
        (stored_size == 0U || memcmp(stored, candidate.data, stored_size) == 0);
}

static bool layout_required(
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
            sizeof(MeisoActionSlot), config->max_argument_size, &slot_bytes
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

static bool arena_aligned(const void *arena)
{
    return arena != NULL &&
        (uintptr_t)arena % meiso_action_alignment_required() == 0U;
}

static MeisoActionSlot *slot_at(
    const void *handle,
    size_t storage_offset,
    size_t slot_stride,
    uint32_t index
)
{
    return (MeisoActionSlot *)(void *)((uint8_t *)(void *)handle +
        storage_offset + (size_t)index * slot_stride);
}

static uint8_t *slot_arguments(MeisoActionSlot *slot)
{
    return (uint8_t *)(void *)(slot + 1);
}

static uint8_t *slot_result(
    const MeisoActionConfig *config,
    MeisoActionSlot *slot
)
{
    return slot_arguments(slot) + config->max_argument_size;
}

static uint8_t *caller_scratch(const MeisoActionCaller *caller)
{
    return (uint8_t *)(void *)caller + caller->scratch_offset;
}

static uint8_t *executor_scratch(const MeisoActionExecutor *executor)
{
    return (uint8_t *)(void *)executor + executor->scratch_offset;
}

static MeisoActionSlot *caller_slot(
    const MeisoActionCaller *caller,
    uint32_t index
)
{
    return slot_at(caller, caller->storage_offset, caller->slot_stride, index);
}

static MeisoActionSlot *executor_slot(
    const MeisoActionExecutor *executor,
    uint32_t index
)
{
    return slot_at(executor, executor->storage_offset, executor->slot_stride, index);
}

static MeisoActionSlot *find_caller_slot(
    const MeisoActionCaller *caller,
    MeisoActionId id
)
{
    uint32_t index;

    for (index = 0U; index < caller->count; ++index) {
        MeisoActionSlot *slot = caller_slot(caller, index);
        if (meiso_action_id_equal(slot->id, id)) {
            return slot;
        }
    }
    return NULL;
}

static MeisoActionSlot *find_executor_slot(
    const MeisoActionExecutor *executor,
    MeisoActionId id
)
{
    uint32_t index;

    for (index = 0U; index < executor->count; ++index) {
        MeisoActionSlot *slot = executor_slot(executor, index);
        if (meiso_action_id_equal(slot->id, id)) {
            return slot;
        }
    }
    return NULL;
}

static bool request_equal(
    const MeisoActionConfig *config,
    MeisoActionSlot *slot,
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
        bytes_equal(
            slot_arguments(slot), slot->argument_size, request->arguments
        );
}

static void fill_slot_from_staged_request(
    const MeisoActionConfig *config,
    size_t slot_stride,
    MeisoActionSlot *slot,
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
        memmove(slot_arguments(slot), staged, request->arguments.size);
    }
    (void)config;
}

static void request_view(
    const MeisoActionConfig *config,
    MeisoActionSlot *slot,
    MeisoActionRequest *out_request
)
{
    (void)config;
    out_request->id = slot->id;
    out_request->expected_executor = slot->expected_executor;
    out_request->target = slot->target;
    out_request->operation = slot->operation;
    out_request->arguments.data = slot_arguments(slot);
    out_request->arguments.size = slot->argument_size;
    out_request->sender_deadline = slot->sender_deadline;
    out_request->retry_policy = slot->retry_policy;
}

static void entry_view(
    const MeisoActionConfig *config,
    MeisoActionSlot *slot,
    MeisoActionEntryView *out_view
)
{
    request_view(config, slot, &out_view->request);
    out_view->delivery = slot->delivery;
    out_view->state = slot->state;
    out_view->delivery_accepted = slot->delivery_accepted ? 1U : 0U;
    out_view->result.data = slot_result(config, slot);
    out_view->result.size = slot->result_size;
}

static bool caller_valid(const MeisoActionCaller *caller)
{
    return caller != NULL && descriptor_valid(&caller->descriptor) &&
        config_valid(&caller->descriptor, &caller->config) &&
        caller->count <= caller->config.slot_count &&
        caller->slot_stride >= sizeof(MeisoActionSlot);
}

static bool executor_valid(const MeisoActionExecutor *executor)
{
    return executor != NULL && descriptor_valid(&executor->descriptor) &&
        config_valid(&executor->descriptor, &executor->config) &&
        meiso_owner_scope_valid(executor->executor) &&
        executor->executor.role == executor->descriptor.owner &&
        executor->count <= executor->config.slot_count &&
        executor->slot_stride >= sizeof(MeisoActionSlot);
}

size_t meiso_action_alignment_required(void)
{
    return _Alignof(max_align_t);
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

    if (!descriptor_valid(descriptor) || !config_valid(descriptor, config) ||
        !layout_required(
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
    if (arena != NULL && output_overlaps_arena(
        arena, arena_size, out_caller, sizeof(*out_caller)
    )) {
        return MEISO_ACTION_INIT_INVALID_ARGUMENT;
    }
    *out_caller = NULL;
    if (!descriptor_valid(descriptor)) {
        return MEISO_ACTION_INIT_INVALID_DESCRIPTOR;
    }
    if (!config_valid(descriptor, config)) {
        return MEISO_ACTION_INIT_INVALID_CONFIG;
    }
    if (!arena_aligned(arena)) {
        return MEISO_ACTION_INIT_MISALIGNED_ARENA;
    }
    if (!layout_required(
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

MeisoActionDeliveryBeginStatus meiso_action_caller_begin_delivery(
    MeisoActionCaller *caller,
    MeisoDeliveryGeneration delivery
)
{
    if (!caller_valid(caller) || !meiso_delivery_generation_valid(delivery)) {
        return MEISO_ACTION_DELIVERY_BEGIN_INVALID;
    }
    if (caller->delivery_active) {
        return MEISO_ACTION_DELIVERY_ALREADY_ACTIVE;
    }
    if (caller->delivery.value != 0U && delivery.value <= caller->delivery.value) {
        if (caller->delivery.value == UINT64_MAX) {
            return MEISO_ACTION_DELIVERY_GENERATION_WRAP;
        }
        return MEISO_ACTION_DELIVERY_STALE_GENERATION;
    }
    caller->delivery = delivery;
    caller->delivery_active = true;
    meiso_increment_saturated(&caller->stats.delivery_starts);
    return MEISO_ACTION_DELIVERY_STARTED;
}

MeisoActionDeliveryEndStatus meiso_action_caller_end_delivery(
    MeisoActionCaller *caller,
    MeisoDeliveryGeneration delivery
)
{
    uint32_t index;
    bool unresolved = false;

    if (!caller_valid(caller) || !meiso_delivery_generation_valid(delivery)) {
        return MEISO_ACTION_DELIVERY_END_INVALID;
    }
    if (!caller->delivery_active) {
        return MEISO_ACTION_DELIVERY_END_NOT_ACTIVE;
    }
    if (caller->delivery.value != delivery.value) {
        return MEISO_ACTION_DELIVERY_END_MISMATCH;
    }
    for (index = 0U; index < caller->count; ++index) {
        MeisoActionSlot *slot = caller_slot(caller, index);
        if (slot->delivery.value == delivery.value &&
            !terminal_state(slot->state)) {
            unresolved = true;
            break;
        }
    }
    caller->delivery_active = false;
    meiso_increment_saturated(&caller->stats.delivery_ends);
    return unresolved ? MEISO_ACTION_DELIVERY_ENDED_WITH_UNRESOLVED :
        MEISO_ACTION_DELIVERY_ENDED;
}

MeisoActionSubmitStatus meiso_action_caller_submit(
    MeisoActionCaller *caller,
    const MeisoActionRequest *request
)
{
    MeisoActionRequest request_copy;
    MeisoActionSlot *slot;
    uint8_t *scratch;

    if (!caller_valid(caller) || !request_valid(request)) {
        return MEISO_ACTION_SUBMIT_INVALID;
    }
    if (!caller->delivery_active) {
        return MEISO_ACTION_SUBMIT_NO_ACTIVE_DELIVERY;
    }
    if (request->expected_executor.role != caller->descriptor.owner) {
        meiso_increment_saturated(&caller->stats.scope_mismatches);
        return MEISO_ACTION_SUBMIT_SCOPE_MISMATCH;
    }
    slot = find_caller_slot(caller, request->id);
    if (slot != NULL) {
        if (request_equal(&caller->config, slot, request)) {
            meiso_increment_saturated(&caller->stats.duplicates);
            return MEISO_ACTION_SUBMIT_DUPLICATE;
        }
        meiso_increment_saturated(&caller->stats.conflicts);
        return MEISO_ACTION_SUBMIT_CONFLICT;
    }
    if (request->arguments.size > caller->config.max_argument_size) {
        meiso_increment_saturated(&caller->stats.argument_capacity_failures);
        return MEISO_ACTION_SUBMIT_ARGUMENT_CAPACITY;
    }
    if (caller->count == caller->config.slot_count) {
        meiso_increment_saturated(&caller->stats.table_full_failures);
        return MEISO_ACTION_SUBMIT_TABLE_FULL;
    }
    request_copy = *request;
    scratch = caller_scratch(caller);
    if (request_copy.arguments.size != 0U) {
        memmove(
            scratch, request_copy.arguments.data, request_copy.arguments.size
        );
    }
    slot = caller_slot(caller, caller->count);
    fill_slot_from_staged_request(
        &caller->config, caller->slot_stride, slot, &request_copy, scratch,
        caller->delivery, MEISO_ACTION_STATE_PENDING
    );
    caller->count += 1U;
    meiso_increment_saturated(&caller->stats.submissions);
    return MEISO_ACTION_SUBMIT_NEW;
}

bool meiso_action_caller_next_offer(
    const MeisoActionCaller *caller,
    MeisoDeliveryGeneration delivery,
    size_t *in_out_cursor,
    MeisoActionOffer *out_offer
)
{
    size_t cursor;

    if (!caller_valid(caller) || !meiso_delivery_generation_valid(delivery) ||
        in_out_cursor == NULL || out_offer == NULL || !caller->delivery_active ||
        caller->delivery.value != delivery.value ||
        *in_out_cursor > (size_t)caller->count) {
        return false;
    }
    cursor = *in_out_cursor;
    while (cursor < (size_t)caller->count) {
        MeisoActionSlot *slot = caller_slot(caller, (uint32_t)cursor);
        cursor += 1U;
        if (slot->delivery.value == delivery.value && !slot->delivery_accepted &&
            slot->state == MEISO_ACTION_STATE_PENDING) {
            *in_out_cursor = cursor;
            out_offer->generation = slot->delivery;
            request_view(&caller->config, slot, &out_offer->request);
            return true;
        }
    }
    *in_out_cursor = cursor;
    return false;
}

MeisoActionDeliveryAcceptStatus meiso_action_caller_mark_delivery_accepted(
    MeisoActionCaller *caller,
    MeisoDeliveryGeneration delivery,
    MeisoActionId id,
    MeisoOwnerScope executor
)
{
    MeisoActionSlot *slot;

    if (!caller_valid(caller) || !meiso_delivery_generation_valid(delivery) ||
        !meiso_action_id_valid(id) || !meiso_owner_scope_valid(executor)) {
        return MEISO_ACTION_DELIVERY_ACCEPT_INVALID;
    }
    slot = find_caller_slot(caller, id);
    if (slot == NULL) {
        return MEISO_ACTION_DELIVERY_ACCEPT_UNKNOWN;
    }
    if (!meiso_owner_scope_equal(slot->expected_executor, executor)) {
        meiso_increment_saturated(&caller->stats.scope_mismatches);
        return MEISO_ACTION_DELIVERY_ACCEPT_SCOPE_MISMATCH;
    }
    if (slot->delivery.value != delivery.value) {
        return MEISO_ACTION_DELIVERY_ACCEPT_DELIVERY_MISMATCH;
    }
    if (slot->delivery_accepted) {
        return MEISO_ACTION_DELIVERY_ACCEPT_DUPLICATE;
    }
    slot->delivery_accepted = true;
    meiso_increment_saturated(&caller->stats.delivery_accepts);
    return MEISO_ACTION_DELIVERY_ACCEPT_CONFIRMED;
}

MeisoActionObserveResultStatus meiso_action_caller_observe_result(
    MeisoActionCaller *caller,
    const MeisoActionResult *result
)
{
    MeisoActionResult result_copy;
    MeisoActionSlot *slot;
    uint8_t *scratch;

    if (!caller_valid(caller) || !result_valid(result)) {
        return MEISO_ACTION_RESULT_INVALID;
    }
    slot = find_caller_slot(caller, result->id);
    if (slot == NULL) {
        return MEISO_ACTION_RESULT_UNKNOWN;
    }
    if (!meiso_owner_scope_equal(slot->expected_executor, result->executor)) {
        meiso_increment_saturated(&caller->stats.scope_mismatches);
        return MEISO_ACTION_RESULT_SCOPE_MISMATCH;
    }
    if (terminal_state(slot->state)) {
        if (slot->state == result->state &&
            bytes_equal(
                slot_result(&caller->config, slot), slot->result_size,
                result->payload
            )) {
            meiso_increment_saturated(&caller->stats.result_duplicates);
            return MEISO_ACTION_RESULT_DUPLICATE;
        }
        meiso_increment_saturated(&caller->stats.terminal_rejections);
        return MEISO_ACTION_RESULT_TERMINAL_ALREADY_SET;
    }
    if (result->state == MEISO_ACTION_STATE_ACCEPTED) {
        if (slot->state == MEISO_ACTION_STATE_ACCEPTED) {
            meiso_increment_saturated(&caller->stats.result_duplicates);
            return MEISO_ACTION_RESULT_DUPLICATE;
        }
        slot->state = MEISO_ACTION_STATE_ACCEPTED;
        meiso_increment_saturated(&caller->stats.results_recorded);
        return MEISO_ACTION_RESULT_RECORDED;
    }
    if (slot->state == MEISO_ACTION_STATE_ACCEPTED &&
        (result->state == MEISO_ACTION_STATE_REJECTED ||
         result->state == MEISO_ACTION_STATE_EXPIRED)) {
        return MEISO_ACTION_RESULT_WRONG_STATE;
    }
    if (result->payload.size > caller->config.max_result_size) {
        meiso_increment_saturated(&caller->stats.result_capacity_failures);
        return MEISO_ACTION_RESULT_CAPACITY;
    }
    result_copy = *result;
    scratch = caller_scratch(caller);
    if (result_copy.payload.size != 0U) {
        memmove(
            scratch, result_copy.payload.data, result_copy.payload.size
        );
        memmove(
            slot_result(&caller->config, slot), scratch, result_copy.payload.size
        );
    }
    slot->result_size = result_copy.payload.size;
    slot->state = result_copy.state;
    meiso_increment_saturated(&caller->stats.results_recorded);
    return MEISO_ACTION_RESULT_RECORDED;
}

MeisoActionIndeterminateStatus meiso_action_caller_mark_indeterminate(
    MeisoActionCaller *caller,
    MeisoActionId id,
    MeisoOwnerScope executor
)
{
    MeisoActionSlot *slot;

    if (!caller_valid(caller) || !meiso_action_id_valid(id) ||
        !meiso_owner_scope_valid(executor)) {
        return MEISO_ACTION_INDETERMINATE_INVALID;
    }
    slot = find_caller_slot(caller, id);
    if (slot == NULL) {
        return MEISO_ACTION_INDETERMINATE_UNKNOWN;
    }
    if (!meiso_owner_scope_equal(slot->expected_executor, executor)) {
        meiso_increment_saturated(&caller->stats.scope_mismatches);
        return MEISO_ACTION_INDETERMINATE_SCOPE_MISMATCH;
    }
    if (slot->state == MEISO_ACTION_STATE_INDETERMINATE) {
        return MEISO_ACTION_INDETERMINATE_DUPLICATE;
    }
    if (terminal_state(slot->state)) {
        meiso_increment_saturated(&caller->stats.terminal_rejections);
        return MEISO_ACTION_INDETERMINATE_TERMINAL_ALREADY_SET;
    }
    slot->state = MEISO_ACTION_STATE_INDETERMINATE;
    slot->result_size = 0U;
    meiso_increment_saturated(&caller->stats.indeterminate);
    return MEISO_ACTION_INDETERMINATE_MARKED;
}

bool meiso_action_caller_query(
    const MeisoActionCaller *caller,
    MeisoActionId id,
    MeisoActionEntryView *out_view
)
{
    MeisoActionSlot *slot;

    if (!caller_valid(caller) || !meiso_action_id_valid(id) ||
        out_view == NULL) {
        return false;
    }
    slot = find_caller_slot(caller, id);
    if (slot == NULL) {
        return false;
    }
    entry_view(&caller->config, slot, out_view);
    return true;
}

bool meiso_action_caller_stats(
    const MeisoActionCaller *caller,
    MeisoActionCallerStats *out_stats
)
{
    if (!caller_valid(caller) || out_stats == NULL) {
        return false;
    }
    *out_stats = caller->stats;
    return true;
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

    if (!descriptor_valid(descriptor) || !config_valid(descriptor, config) ||
        !layout_required(
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
    if (arena != NULL && output_overlaps_arena(
        arena, arena_size, out_executor, sizeof(*out_executor)
    )) {
        return MEISO_ACTION_INIT_INVALID_ARGUMENT;
    }
    *out_executor = NULL;
    if (!descriptor_valid(descriptor)) {
        return MEISO_ACTION_INIT_INVALID_DESCRIPTOR;
    }
    if (!config_valid(descriptor, config)) {
        return MEISO_ACTION_INIT_INVALID_CONFIG;
    }
    if (!meiso_owner_scope_valid(executor_scope) ||
        executor_scope.role != descriptor->owner) {
        return MEISO_ACTION_INIT_INVALID_EXECUTOR;
    }
    if (!arena_aligned(arena)) {
        return MEISO_ACTION_INIT_MISALIGNED_ARENA;
    }
    if (!layout_required(
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

static bool admission_comparable(const MeisoActionAdmission *admission)
{
    return admission != NULL && admission->comparable == 1U &&
        admission->uncertainty_acceptable == 1U &&
        meiso_time_point_valid(admission->local_deadline) &&
        meiso_time_point_valid(admission->local_now) &&
        admission->local_deadline.clock_epoch == admission->local_now.clock_epoch;
}

MeisoActionIngestStatus meiso_action_executor_ingest(
    MeisoActionExecutor *executor,
    const MeisoActionRequest *request,
    const MeisoActionAdmission *admission
)
{
    MeisoActionRequest request_copy;
    MeisoActionSlot *slot;
    MeisoActionState initial_state = MEISO_ACTION_STATE_PENDING;
    MeisoTimePoint mapped_local_deadline = {0U, 0U};
    bool has_mapped_deadline = false;
    uint8_t *scratch;

    if (!executor_valid(executor) || !request_valid(request)) {
        return MEISO_ACTION_INGEST_INVALID;
    }
    if (!meiso_owner_scope_equal(
            request->expected_executor, executor->executor
        )) {
        meiso_increment_saturated(&executor->stats.scope_mismatches);
        return MEISO_ACTION_INGEST_SCOPE_MISMATCH;
    }
    slot = find_executor_slot(executor, request->id);
    if (slot != NULL) {
        if (request_equal(&executor->config, slot, request)) {
            meiso_increment_saturated(&executor->stats.replays);
            return MEISO_ACTION_INGEST_REPLAY;
        }
        meiso_increment_saturated(&executor->stats.conflicts);
        return MEISO_ACTION_INGEST_CONFLICT;
    }
    if (admission != NULL &&
        (admission->comparable > 1U ||
         admission->uncertainty_acceptable > 1U)) {
        return MEISO_ACTION_INGEST_INVALID;
    }
    if (request->arguments.size > executor->config.max_argument_size) {
        meiso_increment_saturated(&executor->stats.argument_capacity_failures);
        return MEISO_ACTION_INGEST_ARGUMENT_CAPACITY;
    }
    if (executor->count == executor->config.slot_count) {
        meiso_increment_saturated(&executor->stats.table_full_failures);
        return MEISO_ACTION_INGEST_TABLE_FULL;
    }
    if (request->sender_deadline.present) {
        if (!admission_comparable(admission)) {
            meiso_increment_saturated(&executor->stats.clock_scope_mismatches);
            return MEISO_ACTION_INGEST_CLOCK_SCOPE_MISMATCH;
        }
        if (admission->local_now.monotonic_ns >
            admission->local_deadline.monotonic_ns) {
            initial_state = MEISO_ACTION_STATE_EXPIRED;
        }
        mapped_local_deadline = admission->local_deadline;
        has_mapped_deadline = true;
    }
    request_copy = *request;
    scratch = executor_scratch(executor);
    if (request_copy.arguments.size != 0U) {
        memmove(
            scratch, request_copy.arguments.data, request_copy.arguments.size
        );
    }
    slot = executor_slot(executor, executor->count);
    fill_slot_from_staged_request(
        &executor->config, executor->slot_stride, slot, &request_copy, scratch,
        (MeisoDeliveryGeneration){0U}, initial_state
    );
    slot->mapped_local_deadline = mapped_local_deadline;
    slot->has_mapped_deadline = has_mapped_deadline;
    executor->count += 1U;
    meiso_increment_saturated(&executor->stats.ingests);
    if (initial_state == MEISO_ACTION_STATE_EXPIRED) {
        meiso_increment_saturated(&executor->stats.expired);
        return MEISO_ACTION_INGEST_EXPIRED;
    }
    return MEISO_ACTION_INGEST_NEW;
}

bool meiso_action_executor_next_application_request(
    const MeisoActionExecutor *executor,
    size_t *in_out_cursor,
    MeisoActionRequest *out_request
)
{
    size_t cursor;

    if (!executor_valid(executor) || in_out_cursor == NULL ||
        out_request == NULL || *in_out_cursor > (size_t)executor->count) {
        return false;
    }
    cursor = *in_out_cursor;
    while (cursor < (size_t)executor->count) {
        MeisoActionSlot *slot = executor_slot(executor, (uint32_t)cursor);
        cursor += 1U;
        if (slot->state == MEISO_ACTION_STATE_PENDING) {
            *in_out_cursor = cursor;
            request_view(&executor->config, slot, out_request);
            return true;
        }
    }
    *in_out_cursor = cursor;
    return false;
}

MeisoActionExecutorAcceptStatus meiso_action_executor_accept(
    MeisoActionExecutor *executor,
    MeisoOwnerScope executor_scope,
    MeisoActionId id,
    MeisoTimePoint comparable_now
)
{
    MeisoActionSlot *slot;

    if (!executor_valid(executor) || !meiso_action_id_valid(id) ||
        !meiso_owner_scope_valid(executor_scope)) {
        return MEISO_ACTION_EXECUTOR_ACCEPT_INVALID;
    }
    if (!meiso_owner_scope_equal(executor->executor, executor_scope)) {
        meiso_increment_saturated(&executor->stats.scope_mismatches);
        return MEISO_ACTION_EXECUTOR_ACCEPT_SCOPE_MISMATCH;
    }
    slot = find_executor_slot(executor, id);
    if (slot == NULL) {
        return MEISO_ACTION_EXECUTOR_ACCEPT_UNKNOWN;
    }
    if (slot->state == MEISO_ACTION_STATE_ACCEPTED) {
        return MEISO_ACTION_EXECUTOR_ACCEPT_DUPLICATE;
    }
    if (terminal_state(slot->state)) {
        return MEISO_ACTION_EXECUTOR_ACCEPT_TERMINAL_ALREADY_SET;
    }
    if (slot->has_mapped_deadline) {
        if (!meiso_time_point_valid(comparable_now) ||
            comparable_now.clock_epoch != slot->mapped_local_deadline.clock_epoch) {
            meiso_increment_saturated(&executor->stats.clock_scope_mismatches);
            return MEISO_ACTION_EXECUTOR_ACCEPT_CLOCK_SCOPE_MISMATCH;
        }
        if (comparable_now.monotonic_ns >
            slot->mapped_local_deadline.monotonic_ns) {
            slot->state = MEISO_ACTION_STATE_EXPIRED;
            meiso_increment_saturated(&executor->stats.expired);
            return MEISO_ACTION_EXECUTOR_ACCEPT_EXPIRED;
        }
    }
    slot->state = MEISO_ACTION_STATE_ACCEPTED;
    meiso_increment_saturated(&executor->stats.accepts);
    return MEISO_ACTION_EXECUTOR_ACCEPTED;
}

MeisoActionRecordResultStatus meiso_action_executor_record_result(
    MeisoActionExecutor *executor,
    const MeisoActionResult *result
)
{
    MeisoActionResult result_copy;
    MeisoActionSlot *slot;
    uint8_t *scratch;

    if (!executor_valid(executor) || !result_valid(result)) {
        return MEISO_ACTION_RECORD_INVALID;
    }
    if (!meiso_owner_scope_equal(result->executor, executor->executor)) {
        meiso_increment_saturated(&executor->stats.scope_mismatches);
        return MEISO_ACTION_RECORD_SCOPE_MISMATCH;
    }
    slot = find_executor_slot(executor, result->id);
    if (slot == NULL) {
        return MEISO_ACTION_RECORD_UNKNOWN;
    }
    if (terminal_state(slot->state)) {
        if (slot->state == result->state &&
            bytes_equal(
                slot_result(&executor->config, slot), slot->result_size,
                result->payload
            )) {
            meiso_increment_saturated(&executor->stats.result_duplicates);
            return MEISO_ACTION_RECORD_DUPLICATE;
        }
        meiso_increment_saturated(&executor->stats.terminal_rejections);
        return MEISO_ACTION_RECORD_TERMINAL_ALREADY_SET;
    }
    if ((result->state == MEISO_ACTION_STATE_SUCCEEDED ||
         result->state == MEISO_ACTION_STATE_FAILED) &&
        slot->state != MEISO_ACTION_STATE_ACCEPTED) {
        return MEISO_ACTION_RECORD_NOT_ACCEPTED;
    }
    if (result->state == MEISO_ACTION_STATE_REJECTED &&
        slot->state != MEISO_ACTION_STATE_PENDING) {
        return MEISO_ACTION_RECORD_WRONG_STATE;
    }
    if (result->state != MEISO_ACTION_STATE_SUCCEEDED &&
        result->state != MEISO_ACTION_STATE_FAILED &&
        result->state != MEISO_ACTION_STATE_REJECTED) {
        return MEISO_ACTION_RECORD_WRONG_STATE;
    }
    if (result->payload.size > executor->config.max_result_size) {
        meiso_increment_saturated(&executor->stats.result_capacity_failures);
        return MEISO_ACTION_RECORD_CAPACITY;
    }
    result_copy = *result;
    scratch = executor_scratch(executor);
    if (result_copy.payload.size != 0U) {
        memmove(
            scratch, result_copy.payload.data, result_copy.payload.size
        );
        memmove(
            slot_result(&executor->config, slot), scratch,
            result_copy.payload.size
        );
    }
    slot->result_size = result_copy.payload.size;
    slot->state = result_copy.state;
    if (result_copy.state == MEISO_ACTION_STATE_SUCCEEDED) {
        meiso_increment_saturated(&executor->stats.succeeded);
    } else if (result_copy.state == MEISO_ACTION_STATE_FAILED) {
        meiso_increment_saturated(&executor->stats.failed);
    } else {
        meiso_increment_saturated(&executor->stats.rejected);
    }
    return MEISO_ACTION_RECORD_RECORDED;
}

MeisoActionIndeterminateStatus meiso_action_executor_mark_indeterminate(
    MeisoActionExecutor *executor,
    MeisoOwnerScope executor_scope,
    MeisoActionId id
)
{
    MeisoActionSlot *slot;

    if (!executor_valid(executor) || !meiso_action_id_valid(id) ||
        !meiso_owner_scope_valid(executor_scope)) {
        return MEISO_ACTION_INDETERMINATE_INVALID;
    }
    if (!meiso_owner_scope_equal(executor->executor, executor_scope)) {
        meiso_increment_saturated(&executor->stats.scope_mismatches);
        return MEISO_ACTION_INDETERMINATE_SCOPE_MISMATCH;
    }
    slot = find_executor_slot(executor, id);
    if (slot == NULL) {
        return MEISO_ACTION_INDETERMINATE_UNKNOWN;
    }
    if (slot->state == MEISO_ACTION_STATE_INDETERMINATE) {
        return MEISO_ACTION_INDETERMINATE_DUPLICATE;
    }
    if (terminal_state(slot->state)) {
        meiso_increment_saturated(&executor->stats.terminal_rejections);
        return MEISO_ACTION_INDETERMINATE_TERMINAL_ALREADY_SET;
    }
    if (slot->state != MEISO_ACTION_STATE_ACCEPTED) {
        return MEISO_ACTION_INDETERMINATE_NOT_ACCEPTED;
    }
    slot->state = MEISO_ACTION_STATE_INDETERMINATE;
    slot->result_size = 0U;
    meiso_increment_saturated(&executor->stats.indeterminate);
    return MEISO_ACTION_INDETERMINATE_MARKED;
}

bool meiso_action_executor_query(
    const MeisoActionExecutor *executor,
    MeisoActionId id,
    MeisoActionEntryView *out_view
)
{
    MeisoActionSlot *slot;

    if (!executor_valid(executor) || !meiso_action_id_valid(id) ||
        out_view == NULL) {
        return false;
    }
    slot = find_executor_slot(executor, id);
    if (slot == NULL) {
        return false;
    }
    entry_view(&executor->config, slot, out_view);
    return true;
}

bool meiso_action_executor_stats(
    const MeisoActionExecutor *executor,
    MeisoActionExecutorStats *out_stats
)
{
    if (!executor_valid(executor) || out_stats == NULL) {
        return false;
    }
    *out_stats = executor->stats;
    return true;
}
