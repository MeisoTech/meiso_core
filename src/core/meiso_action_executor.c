#include "meiso_action_private.h"

#include "meiso_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Owns executor admission, application, result, query, and statistics state. */

static uint8_t *executor_scratch(const MeisoActionExecutor *executor)
{
    return (uint8_t *)(void *)executor + executor->scratch_offset;
}

static MeisoActionPrivateSlot *executor_slot(
    const MeisoActionExecutor *executor,
    uint32_t index
)
{
    return meiso_action_private_slot_at(executor, executor->storage_offset, executor->slot_stride, index);
}

static MeisoActionPrivateSlot *find_executor_slot(
    const MeisoActionExecutor *executor,
    MeisoActionId id
)
{
    uint32_t index;

    for (index = 0U; index < executor->count; ++index) {
        MeisoActionPrivateSlot *slot = executor_slot(executor, index);
        if (meiso_action_id_equal(slot->id, id)) {
            return slot;
        }
    }
    return NULL;
}

static bool executor_valid(const MeisoActionExecutor *executor)
{
    return executor != NULL && meiso_action_private_descriptor_valid(&executor->descriptor) &&
        meiso_action_private_config_valid(&executor->descriptor, &executor->config) &&
        meiso_owner_scope_valid(executor->executor) &&
        executor->executor.role == executor->descriptor.owner &&
        executor->count <= executor->config.slot_count &&
        executor->slot_stride >= sizeof(MeisoActionPrivateSlot);
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
    MeisoActionPrivateSlot *slot;
    MeisoActionState initial_state = MEISO_ACTION_STATE_PENDING;
    MeisoTimePoint mapped_local_deadline = {0U, 0U};
    bool has_mapped_deadline = false;
    uint8_t *scratch;

    if (!executor_valid(executor) || !meiso_action_private_request_valid(request)) {
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
        if (meiso_action_private_request_equal(&executor->config, slot, request)) {
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
    meiso_action_private_fill_slot_from_staged_request(
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
        MeisoActionPrivateSlot *slot = executor_slot(executor, (uint32_t)cursor);
        cursor += 1U;
        if (slot->state == MEISO_ACTION_STATE_PENDING) {
            *in_out_cursor = cursor;
            meiso_action_private_request_view(&executor->config, slot, out_request);
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
    MeisoActionPrivateSlot *slot;

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
    if (meiso_action_private_terminal_state(slot->state)) {
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
    MeisoActionPrivateSlot *slot;
    uint8_t *scratch;

    if (!executor_valid(executor) || !meiso_action_private_result_valid(result)) {
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
    if (meiso_action_private_terminal_state(slot->state)) {
        if (slot->state == result->state &&
            meiso_action_private_bytes_equal(
                meiso_action_private_slot_result(&executor->config, slot), slot->result_size,
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
            meiso_action_private_slot_result(&executor->config, slot), scratch,
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
    MeisoActionPrivateSlot *slot;

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
    if (meiso_action_private_terminal_state(slot->state)) {
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
    MeisoActionPrivateSlot *slot;

    if (!executor_valid(executor) || !meiso_action_id_valid(id) ||
        out_view == NULL) {
        return false;
    }
    slot = find_executor_slot(executor, id);
    if (slot == NULL) {
        return false;
    }
    meiso_action_private_entry_view(&executor->config, slot, out_view);
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
