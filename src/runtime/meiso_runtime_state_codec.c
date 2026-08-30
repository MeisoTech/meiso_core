#include "meiso_runtime_private.h"

#include <string.h>

/* Public Desired/Actual State payload codecs and their semantic validation. */

static bool production_reason_valid(MeisoRuntimeProductionReason value)
{
    return value <= MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED;
}

static bool production_status_valid(MeisoRuntimeProductionStatus value)
{
    return value >= MEISO_RUNTIME_PRODUCTION_STOPPED &&
        value <= MEISO_RUNTIME_PRODUCTION_FAILED;
}

static bool apply_result_valid(MeisoRuntimeApplyResult value)
{
    return value >= MEISO_RUNTIME_APPLY_NONE &&
        value <= MEISO_RUNTIME_APPLY_FAILED;
}

static bool availability_valid(MeisoRuntimeAvailability value)
{
    return value >= MEISO_RUNTIME_AVAILABILITY_PRESENT &&
        value <= MEISO_RUNTIME_AVAILABILITY_UNAVAILABLE;
}

static bool desired_reference_valid(MeisoRuntimeDesiredReference value)
{
    if (!value.present) {
        return value.owner.role == 0U && value.owner.incarnation == 0U &&
            value.version == 0U;
    }
    return meiso_owner_scope_valid(value.owner) && value.version != 0U;
}

bool meiso_runtime_private_desired_reference_equal(
    MeisoRuntimeDesiredReference lhs,
    MeisoRuntimeDesiredReference rhs
)
{
    if (lhs.present != rhs.present) {
        return false;
    }
    return !lhs.present ||
        (meiso_owner_scope_equal(lhs.owner, rhs.owner) &&
         lhs.version == rhs.version);
}

static bool actual_state_valid(const MeisoRuntimeActualState *state)
{
    const bool live = state != NULL &&
        (state->production_status == MEISO_RUNTIME_PRODUCTION_RUNNING ||
         state->production_status == MEISO_RUNTIME_PRODUCTION_DEGRADED ||
         state->production_status == MEISO_RUNTIME_PRODUCTION_STOPPING);

    if (state == NULL ||
        !desired_reference_valid(state->observed_desired) ||
        !desired_reference_valid(state->applied_desired) ||
        !apply_result_valid(state->apply_result) ||
        !availability_valid(state->availability) ||
        !production_status_valid(state->production_status) ||
        !production_reason_valid(state->reason) ||
        !meiso_byte_view_valid(state->profile)) {
        return false;
    }
    if (state->apply_result == MEISO_RUNTIME_APPLY_NONE &&
        (state->observed_desired.present || state->applied_desired.present)) {
        return false;
    }
    if ((state->apply_result == MEISO_RUNTIME_APPLY_PENDING ||
         state->apply_result == MEISO_RUNTIME_APPLY_REJECTED ||
         state->apply_result == MEISO_RUNTIME_APPLY_FAILED) &&
        !state->observed_desired.present) {
        return false;
    }
    if (state->apply_result == MEISO_RUNTIME_APPLY_APPLIED &&
        (!state->observed_desired.present ||
         !meiso_runtime_private_desired_reference_equal(
             state->observed_desired, state->applied_desired
         ))) {
        return false;
    }
    if (live && (!state->applied_desired.present ||
                 !state->effective_enabled || state->production_epoch == 0U ||
                 state->availability != MEISO_RUNTIME_AVAILABILITY_PRESENT)) {
        return false;
    }
    if ((state->production_status == MEISO_RUNTIME_PRODUCTION_STOPPED ||
         state->production_status == MEISO_RUNTIME_PRODUCTION_FAILED) &&
        (state->effective_enabled || state->production_epoch != 0U)) {
        return false;
    }
    if (state->production_status == MEISO_RUNTIME_PRODUCTION_STARTING &&
        (state->effective_enabled || state->production_epoch != 0U)) {
        return false;
    }
    if (state->availability != MEISO_RUNTIME_AVAILABILITY_PRESENT &&
        (state->effective_enabled || state->production_epoch != 0U ||
         (state->production_status != MEISO_RUNTIME_PRODUCTION_STOPPED &&
          state->production_status != MEISO_RUNTIME_PRODUCTION_FAILED))) {
        return false;
    }
    return true;
}

bool meiso_runtime_private_actual_state_for_route_valid(const MeisoRuntimeActualState *state)
{
    return actual_state_valid(state) &&
        (!state->observed_desired.present ||
         state->observed_desired.owner.role == MEISO_OWNER_ROLE_HOST) &&
        (!state->applied_desired.present ||
         state->applied_desired.owner.role == MEISO_OWNER_ROLE_HOST);
}

bool meiso_runtime_private_actual_data_active(const MeisoRuntimeActualState *state)
{
    return meiso_runtime_private_actual_state_for_route_valid(state) && state->effective_enabled &&
        state->availability == MEISO_RUNTIME_AVAILABILITY_PRESENT &&
        state->production_epoch != 0U &&
        (state->production_status == MEISO_RUNTIME_PRODUCTION_RUNNING ||
         state->production_status == MEISO_RUNTIME_PRODUCTION_DEGRADED ||
         state->production_status == MEISO_RUNTIME_PRODUCTION_STOPPING);
}
size_t meiso_runtime_desired_state_encoded_size(
    const MeisoRuntimeDesiredState *state
)
{
    size_t size;

    return state != NULL && meiso_byte_view_valid(state->profile) &&
        meiso_runtime_private_checked_add(
            MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE,
            state->profile.size,
            &size
        ) ? size : 0U;
}

bool meiso_runtime_desired_state_encode(
    const MeisoRuntimeDesiredState *state,
    MeisoMutableBuffer output,
    MeisoByteView *out_payload
)
{
    MeisoRuntimeDesiredState value;
    size_t size;

    if (state == NULL) {
        return false;
    }
    value = *state;
    size = meiso_runtime_desired_state_encoded_size(&value);
    if (out_payload == NULL || !meiso_mutable_buffer_valid(output) ||
        size == 0U || size > output.capacity || meiso_runtime_private_ranges_overlap(
            value.profile.data, value.profile.size, output.data,
            MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE
        ) ||
        meiso_runtime_private_ranges_overlap(out_payload, sizeof(*out_payload), output.data, size)) {
        return false;
    }
    memset(output.data, 0, MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE);
    output.data[0] = value.enabled ? 1U : 0U;
    if (value.profile.size != 0U) {
        memmove(
            output.data + MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE,
            value.profile.data,
            value.profile.size
        );
    }
    out_payload->data = output.data;
    out_payload->size = size;
    return true;
}

bool meiso_runtime_desired_state_decode(
    MeisoByteView payload,
    MeisoRuntimeDesiredState *out_state
)
{
    if (out_state == NULL || !meiso_byte_view_valid(payload) ||
        payload.size < MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE ||
        payload.data[0] > 1U || !meiso_runtime_private_bytes_zero(payload.data + 1U, 7U) ||
        meiso_runtime_private_ranges_overlap(out_state, sizeof(*out_state), payload.data, payload.size)) {
        return false;
    }
    memset(out_state, 0, sizeof(*out_state));
    out_state->enabled = payload.data[0] != 0U;
    out_state->profile.data =
        payload.data + MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE;
    out_state->profile.size =
        payload.size - MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE;
    return true;
}

size_t meiso_runtime_actual_state_encoded_size(
    const MeisoRuntimeActualState *state
)
{
    size_t size;

    return actual_state_valid(state) && meiso_runtime_private_checked_add(
        MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE, state->profile.size, &size
    ) ? size : 0U;
}

bool meiso_runtime_actual_state_encode(
    const MeisoRuntimeActualState *state,
    MeisoMutableBuffer output,
    MeisoByteView *out_payload
)
{
    MeisoRuntimeActualState value;
    size_t size;

    if (state == NULL) {
        return false;
    }
    value = *state;
    size = meiso_runtime_actual_state_encoded_size(&value);
    if (out_payload == NULL || !meiso_mutable_buffer_valid(output) ||
        size == 0U || size > output.capacity || meiso_runtime_private_ranges_overlap(
            value.profile.data, value.profile.size, output.data,
            MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE
        ) ||
        meiso_runtime_private_ranges_overlap(out_payload, sizeof(*out_payload), output.data, size)) {
        return false;
    }
    memset(output.data, 0, MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE);
    output.data[0] = value.observed_desired.present ? 1U : 0U;
    output.data[1] = value.observed_desired.present ?
        value.observed_desired.owner.role : 0U;
    output.data[2] = value.applied_desired.present ? 1U : 0U;
    output.data[3] = value.applied_desired.present ?
        value.applied_desired.owner.role : 0U;
    output.data[4] = value.apply_result;
    output.data[5] = value.availability;
    output.data[6] = value.production_status;
    output.data[7] = value.effective_enabled ? 1U : 0U;
    meiso_runtime_private_put_u64(
        output.data + 8U,
        value.observed_desired.present ?
            value.observed_desired.owner.incarnation : 0U
    );
    meiso_runtime_private_put_u64(
        output.data + 16U,
        value.observed_desired.present ? value.observed_desired.version : 0U
    );
    meiso_runtime_private_put_u64(
        output.data + 24U,
        value.applied_desired.present ?
            value.applied_desired.owner.incarnation : 0U
    );
    meiso_runtime_private_put_u64(
        output.data + 32U,
        value.applied_desired.present ? value.applied_desired.version : 0U
    );
    meiso_runtime_private_put_u64(output.data + 40U, value.production_epoch);
    meiso_runtime_private_put_u16(output.data + 48U, value.reason);
    if (value.profile.size != 0U) {
        memmove(
            output.data + MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE,
            value.profile.data,
            value.profile.size
        );
    }
    out_payload->data = output.data;
    out_payload->size = size;
    return true;
}

bool meiso_runtime_actual_state_decode(
    MeisoByteView payload,
    MeisoRuntimeActualState *out_state
)
{
    MeisoRuntimeActualState value;

    if (out_state == NULL || !meiso_byte_view_valid(payload) ||
        payload.size < MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE ||
        payload.data[0] > 1U || payload.data[2] > 1U ||
        payload.data[7] > 1U || !meiso_runtime_private_bytes_zero(payload.data + 50U, 6U) ||
        meiso_runtime_private_ranges_overlap(out_state, sizeof(*out_state), payload.data, payload.size)) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.observed_desired.present = payload.data[0] != 0U;
    value.observed_desired.owner.role = payload.data[1];
    value.observed_desired.owner.incarnation = meiso_runtime_private_get_u64(payload.data + 8U);
    value.observed_desired.version = meiso_runtime_private_get_u64(payload.data + 16U);
    value.applied_desired.present = payload.data[2] != 0U;
    value.applied_desired.owner.role = payload.data[3];
    value.applied_desired.owner.incarnation = meiso_runtime_private_get_u64(payload.data + 24U);
    value.applied_desired.version = meiso_runtime_private_get_u64(payload.data + 32U);
    value.apply_result = payload.data[4];
    value.availability = payload.data[5];
    value.production_status = payload.data[6];
    value.effective_enabled = payload.data[7] != 0U;
    value.production_epoch = meiso_runtime_private_get_u64(payload.data + 40U);
    value.reason = meiso_runtime_private_get_u16(payload.data + 48U);
    value.profile.data = payload.data + MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE;
    value.profile.size = payload.size - MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE;
    if (!actual_state_valid(&value)) {
        return false;
    }
    *out_state = value;
    return true;
}
