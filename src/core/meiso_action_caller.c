#include "meiso_action_private.h"

#include "meiso_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Owns caller delivery, submission, result, query, and statistics state. */

static uint8_t *caller_scratch(const MeisoActionCaller *caller)
{
    return (uint8_t *)(void *)caller + caller->scratch_offset;
}
static MeisoActionPrivateSlot *caller_slot(
    const MeisoActionCaller *caller,
    uint32_t index
)
{
    return meiso_action_private_slot_at(caller, caller->storage_offset, caller->slot_stride, index);
}

static MeisoActionPrivateSlot *find_caller_slot(
    const MeisoActionCaller *caller,
    MeisoActionId id
)
{
    uint32_t index;

    for (index = 0U; index < caller->count; ++index) {
        MeisoActionPrivateSlot *slot = caller_slot(caller, index);
        if (meiso_action_id_equal(slot->id, id)) {
            return slot;
        }
    }
    return NULL;
}

static bool caller_valid(const MeisoActionCaller *caller)
{
    return caller != NULL && meiso_action_private_descriptor_valid(&caller->descriptor) &&
        meiso_action_private_config_valid(&caller->descriptor, &caller->config) &&
        caller->count <= caller->config.slot_count &&
        caller->slot_stride >= sizeof(MeisoActionPrivateSlot);
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
        MeisoActionPrivateSlot *slot = caller_slot(caller, index);
        if (slot->delivery.value == delivery.value &&
            !meiso_action_private_terminal_state(slot->state)) {
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
    MeisoActionPrivateSlot *slot;
    uint8_t *scratch;

    if (!caller_valid(caller) || !meiso_action_private_request_valid(request)) {
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
        if (meiso_action_private_request_equal(&caller->config, slot, request)) {
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
    meiso_action_private_fill_slot_from_staged_request(
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
        MeisoActionPrivateSlot *slot = caller_slot(caller, (uint32_t)cursor);
        cursor += 1U;
        if (slot->delivery.value == delivery.value && !slot->delivery_accepted &&
            slot->state == MEISO_ACTION_STATE_PENDING) {
            *in_out_cursor = cursor;
            out_offer->generation = slot->delivery;
            meiso_action_private_request_view(&caller->config, slot, &out_offer->request);
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
    MeisoActionPrivateSlot *slot;

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
    MeisoActionPrivateSlot *slot;
    uint8_t *scratch;

    if (!caller_valid(caller) || !meiso_action_private_result_valid(result)) {
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
    if (meiso_action_private_terminal_state(slot->state)) {
        if (slot->state == result->state &&
            meiso_action_private_bytes_equal(
                meiso_action_private_slot_result(&caller->config, slot), slot->result_size,
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
            meiso_action_private_slot_result(&caller->config, slot), scratch, result_copy.payload.size
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
    MeisoActionPrivateSlot *slot;

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
    if (meiso_action_private_terminal_state(slot->state)) {
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
    MeisoActionPrivateSlot *slot;

    if (!caller_valid(caller) || !meiso_action_id_valid(id) ||
        out_view == NULL) {
        return false;
    }
    slot = find_caller_slot(caller, id);
    if (slot == NULL) {
        return false;
    }
    meiso_action_private_entry_view(&caller->config, slot, out_view);
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
