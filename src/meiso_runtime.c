#include "meiso_runtime.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MEISO_RUNTIME_MAGIC UINT64_C(0x4D4549534F52544D)

enum {
    WIRE_CATALOG = 1,
    WIRE_CATALOG_ACCEPT = 2,
    WIRE_CATALOG_REJECT = 3,
    WIRE_STATE_OFFER = 4,
    WIRE_STATE_ACCEPT = 5,
    WIRE_STREAM_SAMPLE = 6,
    WIRE_BLOB_BEGIN = 7,
    WIRE_BLOB_CHUNK = 8,
    WIRE_BLOB_COMMIT = 9,
    WIRE_BLOB_ABORT = 10,
    WIRE_BLOB_RESULT = 11
};

enum {
    CATALOG_PREFIX_SIZE = 16,
    ENDPOINT_RECORD_SIZE = 48,
    ROUTE_RECORD_SIZE = 72,
    CATALOG_ACCEPT_SIZE = 8,
    CATALOG_REJECT_SIZE = 12,
    ROUTE_KEY_SIZE = 56,
    STATE_OFFER_PREFIX_SIZE = 96,
    STATE_ACCEPT_SIZE = 88,
    STREAM_SAMPLE_PREFIX_SIZE = 144,
    CONTENT_ID_WIRE_SIZE = 36,
    BLOB_BEGIN_SIZE = 108,
    BLOB_CHUNK_PREFIX_SIZE = 72,
    BLOB_COMMIT_SIZE = 108,
    BLOB_ABORT_SIZE = 72,
    BLOB_RESULT_SIZE = 84
};

_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + BLOB_BEGIN_SIZE ==
        MEISO_RUNTIME_V3_BLOB_BEGIN_WIRE_SIZE,
    "Blob BEGIN wire size"
);
_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + BLOB_CHUNK_PREFIX_SIZE ==
        MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE,
    "Blob CHUNK prefix wire size"
);
_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + BLOB_COMMIT_SIZE ==
        MEISO_RUNTIME_V3_BLOB_COMMIT_WIRE_SIZE,
    "Blob COMMIT wire size"
);
_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + BLOB_ABORT_SIZE ==
        MEISO_RUNTIME_V3_BLOB_ABORT_WIRE_SIZE,
    "Blob ABORT wire size"
);
_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + BLOB_RESULT_SIZE ==
        MEISO_RUNTIME_V3_BLOB_RESULT_WIRE_SIZE,
    "Blob RESULT wire size"
);

typedef struct {
    MeisoEndpointId id;
    uint64_t incarnation;
    MeisoSchemaRef capability_schema;
    uint32_t capability_offset;
    uint32_t capability_size;
    uint32_t actual_profile_size;
    MeisoRuntimeDesiredReference desired;
    MeisoRuntimeProductionSnapshot production;
} EndpointSlot;

typedef struct {
    MeisoRuntimeRouteDescriptor value;
} RouteSlot;

typedef struct {
    MeisoEndpointId id;
    uint64_t incarnation;
    uint64_t production_epoch_high_water;
    bool occupied;
    bool current;
} IdentitySlot;

struct MeisoRuntime {
    uint64_t magic;
    MeisoSession *session;
    MeisoSessionId session_id;
    MeisoSessionLimits limits;
    size_t arena_size;
    size_t endpoint_offset[2];
    size_t route_offset[2];
    size_t capability_offset[2];
    size_t actual_profile_offset[2];
    size_t identity_offset;
    size_t pending_desired_payload_offset;
    size_t pending_actual_profile_offset;
    size_t tx_offset;
    size_t blob_scratch_offset;
    uint64_t highest_catalog_generation;
    uint64_t pending_catalog_generation;
    uint64_t generation[2];
    uint32_t endpoint_count[2];
    uint32_t route_count[2];
    uint32_t capability_used[2];
    uint32_t max_endpoints;
    uint32_t max_endpoint_history;
    uint32_t max_routes;
    uint32_t max_capability_bytes;
    uint32_t max_desired_profile_bytes_per_endpoint;
    uint32_t max_actual_profile_bytes_per_endpoint;
    uint32_t max_blob_transfer_bytes;
    uint32_t identity_count;
    MeisoSessionRole local_role;
    uint8_t wire_version;
    uint8_t active_bank;
    MeisoRuntimeRouteKey pending_actual_route;
    MeisoDeliveryGeneration pending_actual_delivery;
    MeisoOwnerScope pending_actual_owner;
    uint64_t pending_actual_version;
    uint32_t pending_desired_payload_size;
    uint32_t pending_actual_profile_size;
    MeisoRuntimeProductionSnapshot pending_actual_value;
    MeisoRuntimeBlobTransferRef blob_transfer;
    MeisoRuntimeBlobTransferRef blob_result_transfer;
    uint64_t blob_next_offset;
    uint64_t blob_result_next_offset;
    uint64_t blob_transfer_id_high_water;
    MeisoRuntimeBlobOperation blob_pending_operation;
    MeisoRuntimeBlobResult blob_pending_result;
    bool active_present;
    bool staged_present;
    bool catalog_decision_pending;
    bool pending_desired_receive;
    bool pending_actual_receive;
    bool blob_transfer_active;
    bool blob_begin_ready;
    bool blob_publish_pending;
    bool blob_result_pending;
    bool blob_outbound_terminal_wait;
    bool blob_bridge_active;
    bool invalidated;
    uint8_t storage[];
};

typedef struct {
    size_t endpoint_offset[2];
    size_t route_offset[2];
    size_t capability_offset[2];
    size_t actual_profile_offset[2];
    size_t identity_offset;
    size_t pending_desired_payload_offset;
    size_t pending_actual_profile_offset;
    size_t tx_offset;
    size_t blob_scratch_offset;
    size_t required;
} RuntimeLayout;

static bool checked_add(size_t lhs, size_t rhs, size_t *out)
{
    if (out == NULL || rhs > SIZE_MAX - lhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

static bool checked_multiply(size_t lhs, size_t rhs, size_t *out)
{
    if (out == NULL || (lhs != 0U && rhs > SIZE_MAX / lhs)) {
        return false;
    }
    *out = lhs * rhs;
    return true;
}

static bool align_up(size_t value, size_t alignment, size_t *out)
{
    const size_t remainder = alignment == 0U ? 0U : value % alignment;

    if (alignment == 0U || out == NULL) {
        return false;
    }
    return remainder == 0U ? (*out = value, true) :
        checked_add(value, alignment - remainder, out);
}

static bool add_aligned_array(
    size_t *cursor,
    size_t alignment,
    size_t count,
    size_t item_size,
    size_t *out_offset
)
{
    size_t bytes;
    size_t aligned;

    if (cursor == NULL || out_offset == NULL ||
        !align_up(*cursor, alignment, &aligned) ||
        !checked_multiply(count, item_size, &bytes) ||
        !checked_add(aligned, bytes, cursor)) {
        return false;
    }
    *out_offset = aligned;
    return true;
}

static bool ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
{
    const uintptr_t lhs_start = (uintptr_t)lhs;
    const uintptr_t rhs_start = (uintptr_t)rhs;
    uintptr_t lhs_end;
    uintptr_t rhs_end;

    if (lhs == NULL || rhs == NULL || lhs_size == 0U || rhs_size == 0U) {
        return false;
    }
    if (lhs_size > UINTPTR_MAX - lhs_start ||
        rhs_size > UINTPTR_MAX - rhs_start) {
        return true;
    }
    lhs_end = lhs_start + lhs_size;
    rhs_end = rhs_start + rhs_size;
    return lhs_start < rhs_end && rhs_start < lhs_end;
}

static void put_u16(uint8_t *target, uint16_t value)
{
    target[0] = (uint8_t)(value >> 8U);
    target[1] = (uint8_t)value;
}

static void put_u32(uint8_t *target, uint32_t value)
{
    target[0] = (uint8_t)(value >> 24U);
    target[1] = (uint8_t)(value >> 16U);
    target[2] = (uint8_t)(value >> 8U);
    target[3] = (uint8_t)value;
}

static void put_u64(uint8_t *target, uint64_t value)
{
    target[0] = (uint8_t)(value >> 56U);
    target[1] = (uint8_t)(value >> 48U);
    target[2] = (uint8_t)(value >> 40U);
    target[3] = (uint8_t)(value >> 32U);
    target[4] = (uint8_t)(value >> 24U);
    target[5] = (uint8_t)(value >> 16U);
    target[6] = (uint8_t)(value >> 8U);
    target[7] = (uint8_t)value;
}

static uint16_t get_u16(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] << 8U | (uint16_t)source[1]);
}

static uint32_t get_u32(const uint8_t *source)
{
    return (uint32_t)source[0] << 24U |
        (uint32_t)source[1] << 16U |
        (uint32_t)source[2] << 8U |
        (uint32_t)source[3];
}

static uint64_t get_u64(const uint8_t *source)
{
    return (uint64_t)source[0] << 56U |
        (uint64_t)source[1] << 48U |
        (uint64_t)source[2] << 40U |
        (uint64_t)source[3] << 32U |
        (uint64_t)source[4] << 24U |
        (uint64_t)source[5] << 16U |
        (uint64_t)source[6] << 8U |
        (uint64_t)source[7];
}

static void put_id128(uint8_t *target, uint64_t high, uint64_t low)
{
    put_u64(target, high);
    put_u64(target + 8U, low);
}

static void put_endpoint_id(uint8_t *target, MeisoEndpointId value)
{
    put_id128(target, value.high, value.low);
}

static MeisoEndpointId get_endpoint_id(const uint8_t *source)
{
    const MeisoEndpointId value = {get_u64(source), get_u64(source + 8U)};
    return value;
}

static void put_resource_id(uint8_t *target, MeisoResourceId value)
{
    put_id128(target, value.high, value.low);
}

static MeisoResourceId get_resource_id(const uint8_t *source)
{
    const MeisoResourceId value = {get_u64(source), get_u64(source + 8U)};
    return value;
}

static void put_schema(uint8_t *target, MeisoSchemaRef value)
{
    put_id128(target, value.id.high, value.id.low);
    put_u32(target + 16U, value.version);
}

static MeisoSchemaRef get_schema(const uint8_t *source)
{
    MeisoSchemaRef value;

    value.id.high = get_u64(source);
    value.id.low = get_u64(source + 8U);
    value.version = get_u32(source + 16U);
    return value;
}

static bool bytes_zero(const uint8_t *bytes, size_t size)
{
    size_t index;

    if (bytes == NULL) {
        return false;
    }
    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool schema_valid(MeisoSchemaRef value)
{
    return meiso_schema_id_valid(value.id) && value.version != 0U;
}

static bool reject_reason_valid(MeisoRuntimeCatalogRejectReason value)
{
    return value >= MEISO_RUNTIME_CATALOG_REJECT_UNSUPPORTED &&
        value <= MEISO_RUNTIME_CATALOG_REJECT_CAPACITY;
}

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

static bool desired_reference_equal(
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
         !desired_reference_equal(
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

static bool actual_state_for_route_valid(const MeisoRuntimeActualState *state)
{
    return actual_state_valid(state) &&
        (!state->observed_desired.present ||
         state->observed_desired.owner.role == MEISO_OWNER_ROLE_HOST) &&
        (!state->applied_desired.present ||
         state->applied_desired.owner.role == MEISO_OWNER_ROLE_HOST);
}

static bool actual_data_active(const MeisoRuntimeActualState *state)
{
    return actual_state_for_route_valid(state) && state->effective_enabled &&
        state->availability == MEISO_RUNTIME_AVAILABILITY_PRESENT &&
        state->production_epoch != 0U &&
        (state->production_status == MEISO_RUNTIME_PRODUCTION_RUNNING ||
         state->production_status == MEISO_RUNTIME_PRODUCTION_DEGRADED ||
         state->production_status == MEISO_RUNTIME_PRODUCTION_STOPPING);
}

bool meiso_endpoint_id_valid(MeisoEndpointId value)
{
    return value.high != 0U || value.low != 0U;
}

bool meiso_endpoint_id_equal(MeisoEndpointId lhs, MeisoEndpointId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

bool meiso_runtime_facet_valid(MeisoRuntimeFacet value)
{
    return value >= MEISO_RUNTIME_FACET_CAPABILITY &&
        value <= MEISO_RUNTIME_FACET_IMMUTABLE;
}

size_t meiso_runtime_desired_state_encoded_size(
    const MeisoRuntimeDesiredState *state
)
{
    size_t size;

    return state != NULL && meiso_byte_view_valid(state->profile) &&
        checked_add(
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
        size == 0U || size > output.capacity || ranges_overlap(
            value.profile.data, value.profile.size, output.data,
            MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE
        ) ||
        ranges_overlap(out_payload, sizeof(*out_payload), output.data, size)) {
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
        payload.data[0] > 1U || !bytes_zero(payload.data + 1U, 7U) ||
        ranges_overlap(out_state, sizeof(*out_state), payload.data, payload.size)) {
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

    return actual_state_valid(state) && checked_add(
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
        size == 0U || size > output.capacity || ranges_overlap(
            value.profile.data, value.profile.size, output.data,
            MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE
        ) ||
        ranges_overlap(out_payload, sizeof(*out_payload), output.data, size)) {
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
    put_u64(
        output.data + 8U,
        value.observed_desired.present ?
            value.observed_desired.owner.incarnation : 0U
    );
    put_u64(
        output.data + 16U,
        value.observed_desired.present ? value.observed_desired.version : 0U
    );
    put_u64(
        output.data + 24U,
        value.applied_desired.present ?
            value.applied_desired.owner.incarnation : 0U
    );
    put_u64(
        output.data + 32U,
        value.applied_desired.present ? value.applied_desired.version : 0U
    );
    put_u64(output.data + 40U, value.production_epoch);
    put_u16(output.data + 48U, value.reason);
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
        payload.data[7] > 1U || !bytes_zero(payload.data + 50U, 6U) ||
        ranges_overlap(out_state, sizeof(*out_state), payload.data, payload.size)) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.observed_desired.present = payload.data[0] != 0U;
    value.observed_desired.owner.role = payload.data[1];
    value.observed_desired.owner.incarnation = get_u64(payload.data + 8U);
    value.observed_desired.version = get_u64(payload.data + 16U);
    value.applied_desired.present = payload.data[2] != 0U;
    value.applied_desired.owner.role = payload.data[3];
    value.applied_desired.owner.incarnation = get_u64(payload.data + 24U);
    value.applied_desired.version = get_u64(payload.data + 32U);
    value.apply_result = payload.data[4];
    value.availability = payload.data[5];
    value.production_status = payload.data[6];
    value.effective_enabled = payload.data[7] != 0U;
    value.production_epoch = get_u64(payload.data + 40U);
    value.reason = get_u16(payload.data + 48U);
    value.profile.data = payload.data + MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE;
    value.profile.size = payload.size - MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE;
    if (!actual_state_valid(&value)) {
        return false;
    }
    *out_state = value;
    return true;
}

static bool calculate_layout(
    uint32_t max_endpoints,
    uint32_t max_endpoint_history,
    uint32_t max_routes,
    uint32_t max_capability_bytes,
    uint32_t max_desired_profile_bytes_per_endpoint,
    uint32_t max_actual_profile_bytes_per_endpoint,
    uint32_t max_runtime_payload_bytes,
    uint8_t wire_version,
    MeisoSessionRole local_role,
    uint32_t max_blob_transfer_bytes,
    RuntimeLayout *out
)
{
    RuntimeLayout value;
    size_t cursor = sizeof(MeisoRuntime);
    size_t actual_profile_total;
    size_t pending_desired_capacity;
    unsigned int bank;

    if (out == NULL || !checked_multiply(
            max_endpoints,
            max_actual_profile_bytes_per_endpoint,
            &actual_profile_total
        ) || !checked_add(
            MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE,
            max_desired_profile_bytes_per_endpoint,
            &pending_desired_capacity
        )) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    for (bank = 0U; bank < 2U; ++bank) {
        if (!add_aligned_array(
                &cursor, _Alignof(EndpointSlot), max_endpoints,
                sizeof(EndpointSlot), &value.endpoint_offset[bank]
            ) ||
            !add_aligned_array(
                &cursor, _Alignof(RouteSlot), max_routes,
                sizeof(RouteSlot), &value.route_offset[bank]
            ) ||
            !add_aligned_array(
                &cursor, 1U, max_capability_bytes, 1U,
                &value.capability_offset[bank]
            ) ||
            !add_aligned_array(
                &cursor, 1U, actual_profile_total, 1U,
                &value.actual_profile_offset[bank]
            )) {
            return false;
        }
    }
    if (!add_aligned_array(
            &cursor, _Alignof(IdentitySlot), max_endpoint_history,
            sizeof(IdentitySlot), &value.identity_offset
        ) ||
        !add_aligned_array(
            &cursor, 1U, pending_desired_capacity, 1U,
            &value.pending_desired_payload_offset
        ) ||
        !add_aligned_array(
            &cursor, 1U, max_actual_profile_bytes_per_endpoint, 1U,
            &value.pending_actual_profile_offset
        ) ||
        !add_aligned_array(
            &cursor, 1U, max_runtime_payload_bytes, 1U, &value.tx_offset
        ) || !add_aligned_array(
            &cursor,
            1U,
            wire_version == MEISO_RUNTIME_WIRE_VERSION_V3 &&
                    local_role == MEISO_SESSION_ROLE_EDGE ?
                max_blob_transfer_bytes : 0U,
            1U,
            &value.blob_scratch_offset
        ) ||
        !align_up(cursor, _Alignof(max_align_t), &value.required)) {
        return false;
    }
    *out = value;
    return true;
}

static bool snapshot_ready(
    MeisoSession *session,
    MeisoSessionSnapshot *out_snapshot
)
{
    return session != NULL && out_snapshot != NULL &&
        meiso_session_snapshot(session, out_snapshot) &&
        out_snapshot->state == MEISO_SESSION_STATE_READY &&
        out_snapshot->session_id_known &&
        (out_snapshot->local_role == MEISO_SESSION_ROLE_HOST ||
         out_snapshot->local_role == MEISO_SESSION_ROLE_EDGE);
}

static bool config_layout_versioned(
    const MeisoRuntimeConfig *config,
    uint8_t wire_version,
    uint32_t max_blob_transfer_bytes,
    MeisoSessionSnapshot *out_snapshot,
    RuntimeLayout *out_layout
)
{
    uint64_t entry_capacity;

    if (config == NULL || out_snapshot == NULL || out_layout == NULL ||
        !snapshot_ready(config->session, out_snapshot) ||
        config->max_endpoints == 0U ||
        config->max_endpoint_history < config->max_endpoints ||
        config->max_routes == 0U ||
        (wire_version != MEISO_RUNTIME_WIRE_VERSION_V2 &&
         wire_version != MEISO_RUNTIME_WIRE_VERSION_V3) ||
        (wire_version == MEISO_RUNTIME_WIRE_VERSION_V2 &&
         max_blob_transfer_bytes != 0U) ||
        (wire_version == MEISO_RUNTIME_WIRE_VERSION_V3 &&
         (max_blob_transfer_bytes == 0U ||
          out_snapshot->negotiated_limits.max_runtime_payload_bytes <
              MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE +
                  MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE))) {
        return false;
    }
    entry_capacity = (uint64_t)config->max_endpoints + config->max_routes;
    return out_snapshot->negotiated_limits.max_runtime_payload_bytes >=
            MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_PREFIX_SIZE &&
        entry_capacity <= out_snapshot->negotiated_limits.max_registry_entries &&
        config->max_capability_bytes <=
            out_snapshot->negotiated_limits.max_runtime_payload_bytes &&
        config->max_desired_profile_bytes_per_endpoint <=
            out_snapshot->negotiated_limits.max_runtime_payload_bytes &&
        config->max_actual_profile_bytes_per_endpoint <=
            out_snapshot->negotiated_limits.max_runtime_payload_bytes &&
        calculate_layout(
            config->max_endpoints,
            config->max_endpoint_history,
            config->max_routes,
            config->max_capability_bytes,
            config->max_desired_profile_bytes_per_endpoint,
            config->max_actual_profile_bytes_per_endpoint,
            out_snapshot->negotiated_limits.max_runtime_payload_bytes,
            wire_version,
            out_snapshot->local_role,
            max_blob_transfer_bytes,
            out_layout
        );
}

static bool config_layout(
    const MeisoRuntimeConfig *config,
    MeisoSessionSnapshot *out_snapshot,
    RuntimeLayout *out_layout
)
{
    return config_layout_versioned(
        config,
        MEISO_RUNTIME_WIRE_VERSION_V2,
        0U,
        out_snapshot,
        out_layout
    );
}

static EndpointSlot *endpoint_slots(MeisoRuntime *runtime, uint8_t bank)
{
    return (EndpointSlot *)(void *)((uint8_t *)(void *)runtime +
                                    runtime->endpoint_offset[bank]);
}

static const EndpointSlot *endpoint_slots_const(
    const MeisoRuntime *runtime,
    uint8_t bank
)
{
    return (const EndpointSlot *)(const void *)(
        (const uint8_t *)(const void *)runtime + runtime->endpoint_offset[bank]
    );
}

static RouteSlot *route_slots(MeisoRuntime *runtime, uint8_t bank)
{
    return (RouteSlot *)(void *)((uint8_t *)(void *)runtime +
                                 runtime->route_offset[bank]);
}

static const RouteSlot *route_slots_const(
    const MeisoRuntime *runtime,
    uint8_t bank
)
{
    return (const RouteSlot *)(const void *)(
        (const uint8_t *)(const void *)runtime + runtime->route_offset[bank]
    );
}

static uint8_t *capability_bytes(MeisoRuntime *runtime, uint8_t bank)
{
    return (uint8_t *)(void *)runtime + runtime->capability_offset[bank];
}

static const uint8_t *capability_bytes_const(
    const MeisoRuntime *runtime,
    uint8_t bank
)
{
    return (const uint8_t *)(const void *)runtime +
        runtime->capability_offset[bank];
}

static uint8_t *actual_profile_bytes(
    MeisoRuntime *runtime,
    uint8_t bank,
    size_t endpoint_index
)
{
    return (uint8_t *)(void *)runtime + runtime->actual_profile_offset[bank] +
        endpoint_index * runtime->max_actual_profile_bytes_per_endpoint;
}

static const uint8_t *actual_profile_bytes_const(
    const MeisoRuntime *runtime,
    uint8_t bank,
    size_t endpoint_index
)
{
    return (const uint8_t *)(const void *)runtime +
        runtime->actual_profile_offset[bank] +
        endpoint_index * runtime->max_actual_profile_bytes_per_endpoint;
}

static uint8_t *pending_actual_profile_bytes(MeisoRuntime *runtime)
{
    return (uint8_t *)(void *)runtime +
        runtime->pending_actual_profile_offset;
}

static IdentitySlot *identity_slots(MeisoRuntime *runtime)
{
    return (IdentitySlot *)(void *)((uint8_t *)(void *)runtime +
                                    runtime->identity_offset);
}

static const IdentitySlot *identity_slots_const(const MeisoRuntime *runtime)
{
    return (const IdentitySlot *)(const void *)(
        (const uint8_t *)(const void *)runtime + runtime->identity_offset
    );
}

static uint8_t *pending_desired_payload_bytes(MeisoRuntime *runtime)
{
    return (uint8_t *)(void *)runtime +
        runtime->pending_desired_payload_offset;
}

static uint8_t *tx_bytes(MeisoRuntime *runtime)
{
    return (uint8_t *)(void *)runtime + runtime->tx_offset;
}

static uint8_t *blob_scratch(MeisoRuntime *runtime)
{
    return (uint8_t *)(void *)runtime + runtime->blob_scratch_offset;
}

static void clear_blob_transfer(MeisoRuntime *runtime)
{
    if (runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V3 &&
        runtime->local_role == MEISO_SESSION_ROLE_EDGE &&
        runtime->max_blob_transfer_bytes != 0U) {
        memset(
            blob_scratch(runtime),
            0,
            runtime->max_blob_transfer_bytes
        );
    }
    memset(&runtime->blob_transfer, 0, sizeof(runtime->blob_transfer));
    memset(
        &runtime->blob_result_transfer,
        0,
        sizeof(runtime->blob_result_transfer)
    );
    runtime->blob_next_offset = 0U;
    runtime->blob_result_next_offset = 0U;
    runtime->blob_pending_operation = 0U;
    runtime->blob_pending_result = 0U;
    runtime->blob_transfer_active = false;
    runtime->blob_begin_ready = false;
    runtime->blob_publish_pending = false;
    runtime->blob_result_pending = false;
    runtime->blob_outbound_terminal_wait = false;
}

static uint8_t staging_bank(const MeisoRuntime *runtime)
{
    return (uint8_t)(runtime->active_bank ^ 1U);
}

static bool runtime_layout_valid(const MeisoRuntime *runtime)
{
    RuntimeLayout layout;

    return runtime != NULL && runtime->magic == MEISO_RUNTIME_MAGIC &&
        runtime->active_bank <= 1U &&
        calculate_layout(
            runtime->max_endpoints,
            runtime->max_endpoint_history,
            runtime->max_routes,
            runtime->max_capability_bytes,
            runtime->max_desired_profile_bytes_per_endpoint,
            runtime->max_actual_profile_bytes_per_endpoint,
            runtime->limits.max_runtime_payload_bytes,
            runtime->wire_version,
            runtime->local_role,
            runtime->max_blob_transfer_bytes,
            &layout
        ) && runtime->arena_size == layout.required &&
        runtime->endpoint_offset[0] == layout.endpoint_offset[0] &&
        runtime->endpoint_offset[1] == layout.endpoint_offset[1] &&
        runtime->route_offset[0] == layout.route_offset[0] &&
        runtime->route_offset[1] == layout.route_offset[1] &&
        runtime->capability_offset[0] == layout.capability_offset[0] &&
        runtime->capability_offset[1] == layout.capability_offset[1] &&
        runtime->actual_profile_offset[0] == layout.actual_profile_offset[0] &&
        runtime->actual_profile_offset[1] == layout.actual_profile_offset[1] &&
        runtime->identity_offset == layout.identity_offset &&
        runtime->pending_desired_payload_offset ==
            layout.pending_desired_payload_offset &&
        runtime->pending_actual_profile_offset ==
            layout.pending_actual_profile_offset &&
        runtime->tx_offset == layout.tx_offset &&
        runtime->blob_scratch_offset == layout.blob_scratch_offset &&
        runtime->endpoint_count[0] <= runtime->max_endpoints &&
        runtime->endpoint_count[1] <= runtime->max_endpoints &&
        runtime->route_count[0] <= runtime->max_routes &&
        runtime->route_count[1] <= runtime->max_routes &&
        runtime->capability_used[0] <= runtime->max_capability_bytes &&
        runtime->capability_used[1] <= runtime->max_capability_bytes &&
        runtime->max_endpoint_history >= runtime->max_endpoints &&
        runtime->identity_count <= runtime->max_endpoint_history &&
        runtime->pending_desired_payload_size <=
            MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE +
                runtime->max_desired_profile_bytes_per_endpoint &&
        runtime->pending_actual_profile_size <=
            runtime->max_actual_profile_bytes_per_endpoint &&
        ((runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V2 &&
          runtime->max_blob_transfer_bytes == 0U) ||
         (runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V3 &&
          runtime->max_blob_transfer_bytes != 0U)) &&
        (runtime->local_role == MEISO_SESSION_ROLE_HOST ||
         runtime->local_role == MEISO_SESSION_ROLE_EDGE);
}

static void clear_catalog_bank(MeisoRuntime *runtime, uint8_t bank)
{
    const size_t actual_profile_total =
        (size_t)runtime->max_endpoints *
        runtime->max_actual_profile_bytes_per_endpoint;

    memset(
        endpoint_slots(runtime, bank),
        0,
        (size_t)runtime->max_endpoints * sizeof(EndpointSlot)
    );
    memset(
        route_slots(runtime, bank),
        0,
        (size_t)runtime->max_routes * sizeof(RouteSlot)
    );
    if (runtime->max_capability_bytes != 0U) {
        memset(
            capability_bytes(runtime, bank),
            0,
            runtime->max_capability_bytes
        );
    }
    if (actual_profile_total != 0U) {
        memset(
            actual_profile_bytes(runtime, bank, 0U),
            0,
            actual_profile_total
        );
    }
    runtime->generation[bank] = 0U;
    runtime->endpoint_count[bank] = 0U;
    runtime->route_count[bank] = 0U;
    runtime->capability_used[bank] = 0U;
}

static void clear_catalog_decision(MeisoRuntime *runtime)
{
    runtime->pending_catalog_generation = 0U;
    runtime->catalog_decision_pending = false;
}

static void clear_pending_state_receive(MeisoRuntime *runtime)
{
    runtime->pending_desired_receive = false;
    runtime->pending_actual_receive = false;
    runtime->pending_desired_payload_size = 0U;
    runtime->pending_actual_profile_size = 0U;
}

static void invalidate_runtime(MeisoRuntime *runtime)
{
    clear_blob_transfer(runtime);
    runtime->invalidated = true;
    runtime->active_present = false;
    runtime->staged_present = false;
    clear_pending_state_receive(runtime);
    clear_catalog_decision(runtime);
    clear_catalog_bank(runtime, 0U);
    clear_catalog_bank(runtime, 1U);
}

static MeisoRuntimeStatus check_bound_mutable_internal(
    MeisoRuntime *runtime,
    bool preserve_blob_publish
)
{
    MeisoSessionSnapshot snapshot;

    if (!runtime_layout_valid(runtime)) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (runtime->invalidated) {
        return MEISO_RUNTIME_STALE_SESSION;
    }
    if (runtime->blob_bridge_active) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (!meiso_session_snapshot(runtime->session, &snapshot) ||
        snapshot.state != MEISO_SESSION_STATE_READY ||
        !snapshot.session_id_known) {
        invalidate_runtime(runtime);
        return MEISO_RUNTIME_CORE_NOT_READY;
    }
    if (snapshot.local_role != runtime->local_role ||
        !meiso_session_id_equal(snapshot.session_id, runtime->session_id) ||
        memcmp(
            &snapshot.negotiated_limits,
            &runtime->limits,
            sizeof(runtime->limits)
        ) != 0) {
        invalidate_runtime(runtime);
        return MEISO_RUNTIME_STALE_SESSION;
    }
    if (runtime->blob_publish_pending && !preserve_blob_publish) {
        clear_blob_transfer(runtime);
    }
    return MEISO_RUNTIME_OK;
}

static MeisoRuntimeStatus check_bound_mutable(MeisoRuntime *runtime)
{
    return check_bound_mutable_internal(runtime, false);
}

static MeisoRuntimeStatus check_bound_const(const MeisoRuntime *runtime)
{
    MeisoSessionSnapshot snapshot;

    if (!runtime_layout_valid(runtime)) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (runtime->invalidated) {
        return MEISO_RUNTIME_STALE_SESSION;
    }
    if (runtime->blob_bridge_active) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (!meiso_session_snapshot(runtime->session, &snapshot) ||
        snapshot.state != MEISO_SESSION_STATE_READY ||
        !snapshot.session_id_known) {
        return MEISO_RUNTIME_CORE_NOT_READY;
    }
    return snapshot.local_role == runtime->local_role &&
        meiso_session_id_equal(snapshot.session_id, runtime->session_id) &&
        memcmp(
            &snapshot.negotiated_limits,
            &runtime->limits,
            sizeof(runtime->limits)
        ) == 0 ? MEISO_RUNTIME_OK : MEISO_RUNTIME_STALE_SESSION;
}

static bool output_overlaps_runtime_or_session(
    const MeisoRuntime *runtime,
    const void *output,
    size_t output_size
)
{
    return ranges_overlap(runtime, runtime->arena_size, output, output_size) ||
        ranges_overlap(runtime->session, 1U, output, output_size);
}

static void encode_header(
    const MeisoRuntime *runtime,
    uint8_t *target,
    uint8_t type,
    uint32_t size
)
{
    target[0] = (uint8_t)'M';
    target[1] = (uint8_t)'R';
    target[2] = (uint8_t)'T';
    target[3] = (uint8_t)'0';
    target[4] = runtime->wire_version;
    target[5] = type;
    target[6] = 0U;
    target[7] = 0U;
    put_u32(target + 8U, size);
}

static bool decode_header(
    const MeisoRuntime *runtime,
    MeisoByteView frame,
    uint8_t *out_type
)
{
    if (runtime == NULL || out_type == NULL || frame.data == NULL ||
        frame.size < MEISO_RUNTIME_WIRE_HEADER_SIZE ||
        frame.size > UINT32_MAX || frame.data[0] != (uint8_t)'M' ||
        frame.data[1] != (uint8_t)'R' || frame.data[2] != (uint8_t)'T' ||
        frame.data[3] != (uint8_t)'0' ||
        frame.data[4] != runtime->wire_version ||
        frame.data[6] != 0U || frame.data[7] != 0U ||
        get_u32(frame.data + 8U) != frame.size) {
        return false;
    }
    *out_type = frame.data[5];
    return true;
}

static MeisoRuntimeStatus submit_bytes(MeisoRuntime *runtime, size_t size)
{
    MeisoSessionSubmitStatus status;
    const MeisoByteView payload = {tx_bytes(runtime), size};

    if (size > runtime->limits.max_runtime_payload_bytes) {
        return MEISO_RUNTIME_CAPACITY;
    }
    status = meiso_session_submit_runtime(runtime->session, payload);
    switch (status) {
    case MEISO_SESSION_SUBMIT_QUEUED:
        return MEISO_RUNTIME_OK;
    case MEISO_SESSION_SUBMIT_NOT_READY:
        invalidate_runtime(runtime);
        return MEISO_RUNTIME_CORE_NOT_READY;
    case MEISO_SESSION_SUBMIT_TX_BUSY:
        return MEISO_RUNTIME_TX_BUSY;
    case MEISO_SESSION_SUBMIT_CAPACITY:
        return MEISO_RUNTIME_CAPACITY;
    default:
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
}

static MeisoRuntimeStatus blob_tx_preflight(const MeisoRuntime *runtime)
{
    MeisoSessionSnapshot snapshot;

    if (!meiso_session_snapshot(runtime->session, &snapshot) ||
        snapshot.state != MEISO_SESSION_STATE_READY ||
        !snapshot.session_id_known) {
        return MEISO_RUNTIME_CORE_NOT_READY;
    }
    return snapshot.outbound_pending ?
        MEISO_RUNTIME_TX_BUSY : MEISO_RUNTIME_OK;
}

size_t meiso_runtime_alignment_required(void)
{
    return _Alignof(max_align_t);
}

size_t meiso_runtime_memory_required(const MeisoRuntimeConfig *config)
{
    MeisoSessionSnapshot snapshot;
    RuntimeLayout layout;

    return config_layout(config, &snapshot, &layout) ? layout.required : 0U;
}

size_t meiso_runtime_v3_memory_required(const MeisoRuntimeV3Config *config)
{
    MeisoSessionSnapshot snapshot;
    RuntimeLayout layout;

    return config != NULL && config_layout_versioned(
        &config->base,
        MEISO_RUNTIME_WIRE_VERSION_V3,
        config->max_blob_transfer_bytes,
        &snapshot,
        &layout
    ) ? layout.required : 0U;
}

static MeisoRuntimeInitStatus runtime_init_versioned(
    void *arena,
    size_t arena_size,
    const MeisoRuntimeConfig *config,
    const void *config_object,
    size_t config_object_size,
    uint8_t wire_version,
    uint32_t max_blob_transfer_bytes,
    MeisoRuntime **out_runtime
)
{
    MeisoRuntimeConfig config_copy;
    MeisoSessionSnapshot snapshot;
    RuntimeLayout layout;
    MeisoRuntime *runtime;

    if (out_runtime == NULL) {
        return MEISO_RUNTIME_INIT_INVALID_ARGUMENT;
    }
    if ((arena != NULL && ranges_overlap(
            arena, arena_size, out_runtime, sizeof(*out_runtime)
        )) ||
        (config_object != NULL && ranges_overlap(
            config_object,
            config_object_size,
            out_runtime,
            sizeof(*out_runtime)
        )) ||
        (config != NULL && config->session != NULL && ranges_overlap(
            config->session, 1U, out_runtime, sizeof(*out_runtime)
        )) ||
        (arena != NULL && config != NULL && config->session != NULL &&
         ranges_overlap(arena, arena_size, config->session, 1U))) {
        return MEISO_RUNTIME_INIT_INVALID_ARGUMENT;
    }
    *out_runtime = NULL;
    if (config == NULL || config->session == NULL) {
        return MEISO_RUNTIME_INIT_INVALID_CONFIG;
    }
    if (!snapshot_ready(config->session, &snapshot)) {
        return MEISO_RUNTIME_INIT_CORE_NOT_READY;
    }
    if (!config_layout_versioned(
            config,
            wire_version,
            max_blob_transfer_bytes,
            &snapshot,
            &layout
        )) {
        return MEISO_RUNTIME_INIT_INVALID_CONFIG;
    }
    if (arena == NULL ||
        (uintptr_t)arena % meiso_runtime_alignment_required() != 0U) {
        return MEISO_RUNTIME_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < layout.required) {
        return MEISO_RUNTIME_INIT_ARENA_TOO_SMALL;
    }

    config_copy = *config;
    memset(arena, 0, layout.required);
    runtime = arena;
    runtime->magic = MEISO_RUNTIME_MAGIC;
    runtime->session = config_copy.session;
    runtime->session_id = snapshot.session_id;
    runtime->limits = snapshot.negotiated_limits;
    runtime->arena_size = layout.required;
    runtime->endpoint_offset[0] = layout.endpoint_offset[0];
    runtime->endpoint_offset[1] = layout.endpoint_offset[1];
    runtime->route_offset[0] = layout.route_offset[0];
    runtime->route_offset[1] = layout.route_offset[1];
    runtime->capability_offset[0] = layout.capability_offset[0];
    runtime->capability_offset[1] = layout.capability_offset[1];
    runtime->actual_profile_offset[0] = layout.actual_profile_offset[0];
    runtime->actual_profile_offset[1] = layout.actual_profile_offset[1];
    runtime->identity_offset = layout.identity_offset;
    runtime->pending_desired_payload_offset =
        layout.pending_desired_payload_offset;
    runtime->pending_actual_profile_offset =
        layout.pending_actual_profile_offset;
    runtime->tx_offset = layout.tx_offset;
    runtime->blob_scratch_offset = layout.blob_scratch_offset;
    runtime->max_endpoints = config_copy.max_endpoints;
    runtime->max_endpoint_history = config_copy.max_endpoint_history;
    runtime->max_routes = config_copy.max_routes;
    runtime->max_capability_bytes = config_copy.max_capability_bytes;
    runtime->max_desired_profile_bytes_per_endpoint =
        config_copy.max_desired_profile_bytes_per_endpoint;
    runtime->max_actual_profile_bytes_per_endpoint =
        config_copy.max_actual_profile_bytes_per_endpoint;
    runtime->max_blob_transfer_bytes = max_blob_transfer_bytes;
    runtime->local_role = snapshot.local_role;
    runtime->wire_version = wire_version;
    *out_runtime = runtime;
    return MEISO_RUNTIME_INIT_OK;
}

MeisoRuntimeInitStatus meiso_runtime_init(
    void *arena,
    size_t arena_size,
    const MeisoRuntimeConfig *config,
    MeisoRuntime **out_runtime
)
{
    return runtime_init_versioned(
        arena,
        arena_size,
        config,
        config,
        config != NULL ? sizeof(*config) : 0U,
        MEISO_RUNTIME_WIRE_VERSION_V2,
        0U,
        out_runtime
    );
}

MeisoRuntimeInitStatus meiso_runtime_v3_init(
    void *arena,
    size_t arena_size,
    const MeisoRuntimeV3Config *config,
    MeisoRuntime **out_runtime
)
{
    return runtime_init_versioned(
        arena,
        arena_size,
        config != NULL ? &config->base : NULL,
        config,
        config != NULL ? sizeof(*config) : 0U,
        MEISO_RUNTIME_WIRE_VERSION_V3,
        config != NULL ? config->max_blob_transfer_bytes : 0U,
        out_runtime
    );
}

static int endpoint_index_in_bank(
    const MeisoRuntime *runtime,
    uint8_t bank,
    MeisoEndpointId endpoint_id
)
{
    const EndpointSlot *slots = endpoint_slots_const(runtime, bank);
    uint32_t index;

    for (index = 0U; index < runtime->endpoint_count[bank]; ++index) {
        if (meiso_endpoint_id_equal(slots[index].id, endpoint_id)) {
            return (int)index;
        }
    }
    return -1;
}

static int identity_pair_index(
    const MeisoRuntime *runtime,
    MeisoEndpointId endpoint_id,
    uint64_t incarnation
)
{
    const IdentitySlot *slots = identity_slots_const(runtime);
    uint32_t index;

    for (index = 0U; index < runtime->identity_count; ++index) {
        if (slots[index].occupied &&
            meiso_endpoint_id_equal(slots[index].id, endpoint_id) &&
            slots[index].incarnation == incarnation) {
            return (int)index;
        }
    }
    return -1;
}

static int current_identity_index(
    const MeisoRuntime *runtime,
    MeisoEndpointId endpoint_id
)
{
    const IdentitySlot *slots = identity_slots_const(runtime);
    uint32_t index;

    for (index = 0U; index < runtime->identity_count; ++index) {
        if (slots[index].occupied && slots[index].current &&
            meiso_endpoint_id_equal(slots[index].id, endpoint_id)) {
            return (int)index;
        }
    }
    return -1;
}

static MeisoRuntimeStatus validate_identity_ledger_capacity(
    const MeisoRuntime *runtime,
    uint8_t bank
)
{
    const EndpointSlot *endpoints = endpoint_slots_const(runtime, bank);
    uint32_t new_identities = 0U;
    uint32_t index;

    for (index = 0U; index < runtime->endpoint_count[bank]; ++index) {
        const int ledger_index = identity_pair_index(
            runtime, endpoints[index].id, endpoints[index].incarnation
        );

        if (ledger_index < 0) {
            new_identities += 1U;
        } else if (!identity_slots_const(runtime)[(size_t)ledger_index]
                        .current) {
            return MEISO_RUNTIME_STALE_ENDPOINT;
        }
    }
    return new_identities >
            runtime->max_endpoint_history - runtime->identity_count ?
        MEISO_RUNTIME_CAPACITY : MEISO_RUNTIME_OK;
}

static int route_index_in_bank(
    const MeisoRuntime *runtime,
    uint8_t bank,
    MeisoResourceId resource_id
)
{
    const RouteSlot *slots = route_slots_const(runtime, bank);
    uint32_t index;

    for (index = 0U; index < runtime->route_count[bank]; ++index) {
        if (meiso_resource_id_equal(
                slots[index].value.resource.id,
                resource_id
            )) {
            return (int)index;
        }
    }
    return -1;
}

static bool endpoint_input_valid(
    const MeisoRuntimeEndpointDescriptor *endpoint
)
{
    return endpoint != NULL && meiso_endpoint_id_valid(endpoint->id) &&
        endpoint->incarnation != 0U &&
        schema_valid(endpoint->capability_schema) &&
        meiso_byte_view_valid(endpoint->capability) &&
        endpoint->capability.size <= UINT32_MAX;
}

static MeisoRuntimeStatus route_input_validate(
    const MeisoRuntime *runtime,
    const MeisoRuntimeRouteDescriptor *route
)
{
    size_t minimum_wire;

    if (route == NULL || !meiso_endpoint_id_valid(route->endpoint_id) ||
        route->endpoint_incarnation == 0U ||
        meiso_resource_descriptor_validate(&route->resource) !=
            MEISO_DESCRIPTOR_VALID ||
        !meiso_runtime_facet_valid(route->facet)) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    switch (route->facet) {
    case MEISO_RUNTIME_FACET_DESIRED:
        if (route->resource.kind != MEISO_RESOURCE_KIND_STATE) {
            return MEISO_RUNTIME_UNSUPPORTED_KIND;
        }
        if (route->resource.owner != MEISO_OWNER_ROLE_HOST) {
            return MEISO_RUNTIME_OWNER_MISMATCH;
        }
        if (route->resource.max_payload_size <
            MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE) {
            return MEISO_RUNTIME_PAYLOAD_CAPACITY;
        }
        if (route->resource.max_payload_size -
                MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE >
            runtime->max_desired_profile_bytes_per_endpoint) {
            return MEISO_RUNTIME_PAYLOAD_CAPACITY;
        }
        minimum_wire = MEISO_RUNTIME_WIRE_HEADER_SIZE +
            STATE_OFFER_PREFIX_SIZE;
        break;
    case MEISO_RUNTIME_FACET_ACTUAL:
        if (route->resource.kind != MEISO_RESOURCE_KIND_STATE) {
            return MEISO_RUNTIME_UNSUPPORTED_KIND;
        }
        if (route->resource.owner != MEISO_OWNER_ROLE_EDGE) {
            return MEISO_RUNTIME_OWNER_MISMATCH;
        }
        if (route->resource.max_payload_size <
            MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE) {
            return MEISO_RUNTIME_PAYLOAD_CAPACITY;
        }
        if (route->resource.max_payload_size -
                MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE >
            runtime->max_actual_profile_bytes_per_endpoint) {
            return MEISO_RUNTIME_PAYLOAD_CAPACITY;
        }
        minimum_wire = MEISO_RUNTIME_WIRE_HEADER_SIZE +
            STATE_OFFER_PREFIX_SIZE;
        break;
    case MEISO_RUNTIME_FACET_DATA:
        if (route->resource.kind != MEISO_RESOURCE_KIND_STREAM) {
            return MEISO_RUNTIME_UNSUPPORTED_KIND;
        }
        if (route->resource.owner != MEISO_OWNER_ROLE_EDGE) {
            return MEISO_RUNTIME_OWNER_MISMATCH;
        }
        minimum_wire = MEISO_RUNTIME_WIRE_HEADER_SIZE +
            STREAM_SAMPLE_PREFIX_SIZE;
        break;
    case MEISO_RUNTIME_FACET_IMMUTABLE:
        if (runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V3) {
            return MEISO_RUNTIME_UNSUPPORTED_KIND;
        }
        if (route->resource.kind != MEISO_RESOURCE_KIND_BLOB) {
            return MEISO_RUNTIME_UNSUPPORTED_KIND;
        }
        if (route->resource.owner != MEISO_OWNER_ROLE_HOST) {
            return MEISO_RUNTIME_OWNER_MISMATCH;
        }
        if (route->resource.max_payload_size >
            runtime->max_blob_transfer_bytes) {
            return MEISO_RUNTIME_PAYLOAD_CAPACITY;
        }
        minimum_wire = MEISO_RUNTIME_V3_BLOB_BEGIN_WIRE_SIZE;
        break;
    default:
        /* Capability is inline and Action carriage is not implemented. */
        return MEISO_RUNTIME_UNSUPPORTED_KIND;
    }
    if (minimum_wire > runtime->limits.max_runtime_payload_bytes ||
        (route->facet != MEISO_RUNTIME_FACET_IMMUTABLE &&
         route->resource.max_payload_size >
            runtime->limits.max_runtime_payload_bytes - minimum_wire)) {
        return MEISO_RUNTIME_PAYLOAD_CAPACITY;
    }
    return MEISO_RUNTIME_OK;
}

static bool catalog_wire_size(
    const MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_count,
    size_t route_count,
    size_t *out_size
)
{
    size_t size = MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_PREFIX_SIZE;
    size_t records;
    size_t index;

    if (out_size == NULL ||
        !checked_multiply(endpoint_count, ENDPOINT_RECORD_SIZE, &records) ||
        !checked_add(size, records, &size)) {
        return false;
    }
    for (index = 0U; index < endpoint_count; ++index) {
        if (!checked_add(size, endpoints[index].capability.size, &size)) {
            return false;
        }
    }
    if (!checked_multiply(route_count, ROUTE_RECORD_SIZE, &records) ||
        !checked_add(size, records, &size)) {
        return false;
    }
    *out_size = size;
    return true;
}

static MeisoRuntimeStatus validate_catalog_semantics(
    const MeisoRuntime *runtime,
    uint8_t bank
)
{
    const RouteSlot *routes = route_slots_const(runtime, bank);
    uint32_t endpoint_index;
    uint32_t route_index;

    for (endpoint_index = 0U;
         endpoint_index < runtime->endpoint_count[bank];
         ++endpoint_index) {
        uint32_t desired_count = 0U;
        uint32_t actual_count = 0U;
        uint32_t data_count = 0U;

        for (route_index = 0U;
             route_index < runtime->route_count[bank];
             ++route_index) {
            if (!meiso_endpoint_id_equal(
                    routes[route_index].value.endpoint_id,
                    endpoint_slots_const(runtime, bank)[endpoint_index].id
                )) {
                continue;
            }
            if (routes[route_index].value.facet ==
                MEISO_RUNTIME_FACET_DESIRED) {
                desired_count += 1U;
            } else if (routes[route_index].value.facet ==
                       MEISO_RUNTIME_FACET_ACTUAL) {
                actual_count += 1U;
            } else if (routes[route_index].value.facet ==
                       MEISO_RUNTIME_FACET_DATA) {
                data_count += 1U;
            }
        }
        if (desired_count > 1U || actual_count > 1U) {
            return MEISO_RUNTIME_CONFLICT;
        }
        if (data_count != 0U && (desired_count != 1U || actual_count != 1U)) {
            return MEISO_RUNTIME_CONFLICT;
        }
    }
    return MEISO_RUNTIME_OK;
}

static MeisoRuntimeStatus stage_catalog_input(
    MeisoRuntime *runtime,
    uint64_t generation,
    const MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_count,
    const MeisoRuntimeRouteDescriptor *routes,
    size_t route_count
)
{
    const uint8_t bank = staging_bank(runtime);
    EndpointSlot *endpoint_output;
    RouteSlot *route_output;
    uint8_t *capability_output;
    size_t capability_used = 0U;
    size_t wire_size;
    size_t index;
    MeisoRuntimeStatus status;

    if (generation == 0U || endpoint_count == 0U || route_count == 0U ||
        (endpoint_count != 0U && endpoints == NULL) ||
        (route_count != 0U && routes == NULL) ||
        endpoint_count > runtime->max_endpoints ||
        route_count > runtime->max_routes || endpoint_count > UINT32_MAX ||
        route_count > UINT32_MAX ||
        ranges_overlap(
            runtime, runtime->arena_size, endpoints,
            endpoint_count * sizeof(*endpoints)
        ) ||
        ranges_overlap(
            runtime, runtime->arena_size, routes,
            route_count * sizeof(*routes)
        )) {
        return endpoint_count > runtime->max_endpoints ||
            route_count > runtime->max_routes ?
            MEISO_RUNTIME_CAPACITY : MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    for (index = 0U; index < endpoint_count; ++index) {
        if (!endpoint_input_valid(&endpoints[index]) ||
            ranges_overlap(
                runtime,
                runtime->arena_size,
                endpoints[index].capability.data,
                endpoints[index].capability.size
            ) ||
            !checked_add(
                capability_used,
                endpoints[index].capability.size,
                &capability_used
            )) {
            return MEISO_RUNTIME_INVALID_ARGUMENT;
        }
    }
    if (capability_used > runtime->max_capability_bytes) {
        return MEISO_RUNTIME_CAPACITY;
    }
    if (!catalog_wire_size(
            endpoints, endpoint_count, route_count, &wire_size
        ) || wire_size > runtime->limits.max_runtime_payload_bytes) {
        return MEISO_RUNTIME_CAPACITY;
    }

    clear_catalog_bank(runtime, bank);
    endpoint_output = endpoint_slots(runtime, bank);
    route_output = route_slots(runtime, bank);
    capability_output = capability_bytes(runtime, bank);
    capability_used = 0U;
    for (index = 0U; index < endpoint_count; ++index) {
        size_t earlier;

        for (earlier = 0U; earlier < index; ++earlier) {
            if (meiso_endpoint_id_equal(
                    endpoints[earlier].id,
                    endpoints[index].id
                )) {
                clear_catalog_bank(runtime, bank);
                return MEISO_RUNTIME_CONFLICT;
            }
        }
        endpoint_output[index].id = endpoints[index].id;
        endpoint_output[index].incarnation = endpoints[index].incarnation;
        endpoint_output[index].capability_schema =
            endpoints[index].capability_schema;
        endpoint_output[index].capability_offset = (uint32_t)capability_used;
        endpoint_output[index].capability_size =
            (uint32_t)endpoints[index].capability.size;
        if (endpoints[index].capability.size != 0U) {
            memmove(
                capability_output + capability_used,
                endpoints[index].capability.data,
                endpoints[index].capability.size
            );
        }
        capability_used += endpoints[index].capability.size;
    }
    runtime->endpoint_count[bank] = (uint32_t)endpoint_count;
    runtime->capability_used[bank] = (uint32_t)capability_used;

    for (index = 0U; index < route_count; ++index) {
        const int endpoint_index = endpoint_index_in_bank(
            runtime, bank, routes[index].endpoint_id
        );
        size_t earlier;

        status = route_input_validate(runtime, &routes[index]);
        if (status != MEISO_RUNTIME_OK) {
            clear_catalog_bank(runtime, bank);
            return status;
        }
        if (endpoint_index < 0) {
            clear_catalog_bank(runtime, bank);
            return MEISO_RUNTIME_UNKNOWN_ENDPOINT;
        }
        if (endpoint_slots(runtime, bank)[(size_t)endpoint_index].incarnation !=
            routes[index].endpoint_incarnation) {
            clear_catalog_bank(runtime, bank);
            return MEISO_RUNTIME_STALE_ENDPOINT;
        }
        for (earlier = 0U; earlier < index; ++earlier) {
            if (meiso_resource_id_equal(
                    routes[earlier].resource.id,
                    routes[index].resource.id
                )) {
                clear_catalog_bank(runtime, bank);
                return MEISO_RUNTIME_CONFLICT;
            }
        }
        route_output[index].value = routes[index];
        runtime->route_count[bank] += 1U;
    }
    status = validate_catalog_semantics(runtime, bank);
    if (status == MEISO_RUNTIME_OK) {
        status = validate_identity_ledger_capacity(runtime, bank);
    }
    if (status != MEISO_RUNTIME_OK) {
        clear_catalog_bank(runtime, bank);
        return status;
    }
    runtime->generation[bank] = generation;
    runtime->staged_present = true;
    return MEISO_RUNTIME_OK;
}

static size_t encode_catalog(MeisoRuntime *runtime, uint8_t bank)
{
    const EndpointSlot *endpoints = endpoint_slots_const(runtime, bank);
    const RouteSlot *routes = route_slots_const(runtime, bank);
    const uint8_t *capabilities = capability_bytes_const(runtime, bank);
    uint8_t *output = tx_bytes(runtime);
    size_t cursor = MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_PREFIX_SIZE;
    uint32_t index;

    put_u64(output + MEISO_RUNTIME_WIRE_HEADER_SIZE, runtime->generation[bank]);
    put_u32(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U,
        runtime->endpoint_count[bank]
    );
    put_u32(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 12U,
        runtime->route_count[bank]
    );
    for (index = 0U; index < runtime->endpoint_count[bank]; ++index) {
        put_endpoint_id(output + cursor, endpoints[index].id);
        put_u64(output + cursor + 16U, endpoints[index].incarnation);
        put_schema(output + cursor + 24U, endpoints[index].capability_schema);
        put_u32(output + cursor + 44U, endpoints[index].capability_size);
        cursor += ENDPOINT_RECORD_SIZE;
        if (endpoints[index].capability_size != 0U) {
            memmove(
                output + cursor,
                capabilities + endpoints[index].capability_offset,
                endpoints[index].capability_size
            );
            cursor += endpoints[index].capability_size;
        }
    }
    for (index = 0U; index < runtime->route_count[bank]; ++index) {
        const MeisoRuntimeRouteDescriptor *route = &routes[index].value;

        put_endpoint_id(output + cursor, route->endpoint_id);
        put_u64(output + cursor + 16U, route->endpoint_incarnation);
        put_resource_id(output + cursor + 24U, route->resource.id);
        output[cursor + 40U] = route->resource.kind;
        output[cursor + 41U] = route->resource.owner;
        output[cursor + 42U] = route->facet;
        output[cursor + 43U] = 0U;
        put_schema(output + cursor + 44U, route->resource.schema);
        put_u64(
            output + cursor + 64U,
            (uint64_t)route->resource.max_payload_size
        );
        cursor += ROUTE_RECORD_SIZE;
    }
    encode_header(runtime, output, WIRE_CATALOG, (uint32_t)cursor);
    return cursor;
}

MeisoRuntimeStatus meiso_runtime_offer_catalog(
    MeisoRuntime *runtime,
    uint64_t generation,
    const MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_count,
    const MeisoRuntimeRouteDescriptor *routes,
    size_t route_count
)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);
    const uint8_t bank = runtime != NULL ? staging_bank(runtime) : 0U;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    clear_pending_state_receive(runtime);
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (runtime->catalog_decision_pending) {
        return MEISO_RUNTIME_CATALOG_PENDING;
    }
    if (generation == 0U || generation <= runtime->highest_catalog_generation) {
        return MEISO_RUNTIME_STALE_CATALOG;
    }
    status = stage_catalog_input(
        runtime, generation, endpoints, endpoint_count, routes, route_count
    );
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    status = submit_bytes(runtime, encode_catalog(runtime, bank));
    if (status != MEISO_RUNTIME_OK) {
        runtime->staged_present = false;
        clear_catalog_bank(runtime, bank);
        return status;
    }
    runtime->highest_catalog_generation = generation;
    runtime->pending_catalog_generation = generation;
    runtime->catalog_decision_pending = true;
    return MEISO_RUNTIME_OK;
}

static bool catalog_status_rejectable(MeisoRuntimeStatus status)
{
    return status == MEISO_RUNTIME_CAPACITY ||
        status == MEISO_RUNTIME_CONFLICT ||
        status == MEISO_RUNTIME_UNKNOWN_ENDPOINT ||
        status == MEISO_RUNTIME_STALE_ENDPOINT ||
        status == MEISO_RUNTIME_UNSUPPORTED_KIND ||
        status == MEISO_RUNTIME_OWNER_MISMATCH ||
        status == MEISO_RUNTIME_PAYLOAD_CAPACITY;
}

static MeisoRuntimeStatus decode_catalog_to_staging(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    uint64_t *out_generation
)
{
    const uint8_t *bytes = frame.data;
    const uint8_t bank = staging_bank(runtime);
    EndpointSlot *endpoint_output;
    RouteSlot *route_output;
    uint8_t *capability_output;
    uint64_t generation;
    uint32_t endpoint_count;
    uint32_t route_count;
    size_t cursor;
    size_t capability_total = 0U;
    size_t route_bytes;
    uint32_t index;
    bool local_capacity;
    MeisoRuntimeStatus status = MEISO_RUNTIME_OK;

    if (out_generation == NULL ||
        frame.size < MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_PREFIX_SIZE) {
        return MEISO_RUNTIME_MALFORMED;
    }
    generation = get_u64(bytes + MEISO_RUNTIME_WIRE_HEADER_SIZE);
    endpoint_count = get_u32(bytes + MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U);
    route_count = get_u32(bytes + MEISO_RUNTIME_WIRE_HEADER_SIZE + 12U);
    if (generation == 0U || endpoint_count == 0U || route_count == 0U) {
        return MEISO_RUNTIME_MALFORMED;
    }
    cursor = MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_PREFIX_SIZE;
    for (index = 0U; index < endpoint_count; ++index) {
        uint32_t capability_size;

        if (cursor > frame.size ||
            frame.size - cursor < ENDPOINT_RECORD_SIZE) {
            return MEISO_RUNTIME_MALFORMED;
        }
        capability_size = get_u32(bytes + cursor + 44U);
        cursor += ENDPOINT_RECORD_SIZE;
        if (capability_size > frame.size - cursor ||
            !checked_add(capability_total, capability_size, &capability_total)) {
            return MEISO_RUNTIME_MALFORMED;
        }
        cursor += capability_size;
    }
    if (!checked_multiply(route_count, ROUTE_RECORD_SIZE, &route_bytes) ||
        cursor > frame.size || route_bytes != frame.size - cursor) {
        return MEISO_RUNTIME_MALFORMED;
    }
    for (index = 0U; index < route_count; ++index) {
        if (bytes[cursor + (size_t)index * ROUTE_RECORD_SIZE + 43U] != 0U) {
            return MEISO_RUNTIME_MALFORMED;
        }
    }
    if (generation <= runtime->highest_catalog_generation) {
        return MEISO_RUNTIME_STALE_CATALOG;
    }
    *out_generation = generation;
    local_capacity = endpoint_count > runtime->max_endpoints ||
        route_count > runtime->max_routes ||
        capability_total > runtime->max_capability_bytes;
    if (local_capacity) {
        runtime->highest_catalog_generation = generation;
        runtime->pending_catalog_generation = generation;
        runtime->catalog_decision_pending = true;
        runtime->staged_present = false;
        clear_catalog_bank(runtime, bank);
        return MEISO_RUNTIME_CAPACITY;
    }

    clear_catalog_bank(runtime, bank);
    endpoint_output = endpoint_slots(runtime, bank);
    route_output = route_slots(runtime, bank);
    capability_output = capability_bytes(runtime, bank);
    cursor = MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_PREFIX_SIZE;
    capability_total = 0U;
    for (index = 0U; index < endpoint_count; ++index) {
        const uint8_t *record = bytes + cursor;
        const uint32_t capability_size = get_u32(record + 44U);
        uint32_t earlier;

        endpoint_output[index].id = get_endpoint_id(record);
        endpoint_output[index].incarnation = get_u64(record + 16U);
        endpoint_output[index].capability_schema = get_schema(record + 24U);
        endpoint_output[index].capability_offset = (uint32_t)capability_total;
        endpoint_output[index].capability_size = capability_size;
        if (!meiso_endpoint_id_valid(endpoint_output[index].id) ||
            endpoint_output[index].incarnation == 0U ||
            !schema_valid(endpoint_output[index].capability_schema)) {
            status = MEISO_RUNTIME_MALFORMED;
            break;
        }
        for (earlier = 0U; earlier < index; ++earlier) {
            if (meiso_endpoint_id_equal(
                    endpoint_output[earlier].id,
                    endpoint_output[index].id
                )) {
                status = MEISO_RUNTIME_CONFLICT;
                break;
            }
        }
        if (status != MEISO_RUNTIME_OK) {
            break;
        }
        cursor += ENDPOINT_RECORD_SIZE;
        if (capability_size != 0U) {
            memmove(
                capability_output + capability_total,
                bytes + cursor,
                capability_size
            );
        }
        cursor += capability_size;
        capability_total += capability_size;
        runtime->endpoint_count[bank] += 1U;
    }
    runtime->capability_used[bank] = (uint32_t)capability_total;
    if (status == MEISO_RUNTIME_OK) {
        for (index = 0U; index < route_count; ++index) {
            const uint8_t *record = bytes + cursor;
            MeisoRuntimeRouteDescriptor route;
            const uint64_t maximum = get_u64(record + 64U);
            const int endpoint_index = endpoint_index_in_bank(
                runtime, bank, get_endpoint_id(record)
            );
            uint32_t earlier;

            memset(&route, 0, sizeof(route));
            route.endpoint_id = get_endpoint_id(record);
            route.endpoint_incarnation = get_u64(record + 16U);
            route.resource.id = get_resource_id(record + 24U);
            route.resource.kind = record[40U];
            route.resource.owner = record[41U];
            route.facet = record[42U];
            route.resource.schema = get_schema(record + 44U);
            if (sizeof(size_t) < sizeof(uint64_t) &&
                maximum > (uint64_t)SIZE_MAX) {
                status = MEISO_RUNTIME_PAYLOAD_CAPACITY;
                break;
            }
            route.resource.max_payload_size = (size_t)maximum;
            status = route_input_validate(runtime, &route);
            if (status != MEISO_RUNTIME_OK) {
                break;
            }
            if (endpoint_index < 0) {
                status = MEISO_RUNTIME_UNKNOWN_ENDPOINT;
                break;
            }
            if (endpoint_slots(runtime, bank)[(size_t)endpoint_index].incarnation !=
                route.endpoint_incarnation) {
                status = MEISO_RUNTIME_STALE_ENDPOINT;
                break;
            }
            for (earlier = 0U; earlier < index; ++earlier) {
                if (meiso_resource_id_equal(
                        route_output[earlier].value.resource.id,
                        route.resource.id
                    )) {
                    status = MEISO_RUNTIME_CONFLICT;
                    break;
                }
            }
            if (status != MEISO_RUNTIME_OK) {
                break;
            }
            route_output[index].value = route;
            runtime->route_count[bank] += 1U;
            cursor += ROUTE_RECORD_SIZE;
        }
    }
    if (status == MEISO_RUNTIME_OK) {
        status = validate_catalog_semantics(runtime, bank);
    }
    if (status == MEISO_RUNTIME_OK) {
        status = validate_identity_ledger_capacity(runtime, bank);
    }
    if (status != MEISO_RUNTIME_OK) {
        clear_catalog_bank(runtime, bank);
        runtime->staged_present = false;
        if (catalog_status_rejectable(status)) {
            runtime->highest_catalog_generation = generation;
            runtime->pending_catalog_generation = generation;
            runtime->catalog_decision_pending = true;
        }
        return status;
    }
    runtime->generation[bank] = generation;
    runtime->highest_catalog_generation = generation;
    runtime->pending_catalog_generation = generation;
    runtime->catalog_decision_pending = true;
    runtime->staged_present = true;
    return MEISO_RUNTIME_OK;
}

static void commit_staged(MeisoRuntime *runtime)
{
    const uint8_t new_bank = staging_bank(runtime);
    EndpointSlot *next = endpoint_slots(runtime, new_bank);
    IdentitySlot *ledger = identity_slots(runtime);
    uint32_t next_index;

    for (next_index = 0U;
         next_index < runtime->endpoint_count[new_bank];
         ++next_index) {
        int ledger_index = identity_pair_index(
            runtime,
            next[next_index].id,
            next[next_index].incarnation
        );

        if (ledger_index < 0) {
            const int previous_current = current_identity_index(
                runtime, next[next_index].id
            );

            if (previous_current >= 0) {
                ledger[(size_t)previous_current].current = false;
            }
            ledger_index = (int)runtime->identity_count;
            runtime->identity_count += 1U;
            ledger[(size_t)ledger_index].id = next[next_index].id;
            ledger[(size_t)ledger_index].incarnation =
                next[next_index].incarnation;
            ledger[(size_t)ledger_index].production_epoch_high_water = 0U;
            ledger[(size_t)ledger_index].occupied = true;
            ledger[(size_t)ledger_index].current = true;
        }
        next[next_index].production.production_epoch_high_water =
            ledger[(size_t)ledger_index].production_epoch_high_water;
    }
    clear_blob_transfer(runtime);
    runtime->active_bank = new_bank;
    runtime->active_present = true;
    runtime->staged_present = false;
    clear_pending_state_receive(runtime);
    clear_catalog_decision(runtime);
}

static MeisoRuntimeStatus submit_catalog_decision(
    MeisoRuntime *runtime,
    uint8_t type,
    MeisoRuntimeCatalogRejectReason reason
)
{
    uint8_t *output = tx_bytes(runtime);
    size_t size;

    put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE,
        runtime->pending_catalog_generation
    );
    if (type == WIRE_CATALOG_ACCEPT) {
        size = MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_ACCEPT_SIZE;
    } else {
        output[MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U] = reason;
        output[MEISO_RUNTIME_WIRE_HEADER_SIZE + 9U] = 0U;
        output[MEISO_RUNTIME_WIRE_HEADER_SIZE + 10U] = 0U;
        output[MEISO_RUNTIME_WIRE_HEADER_SIZE + 11U] = 0U;
        size = MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_REJECT_SIZE;
    }
    encode_header(runtime, output, type, (uint32_t)size);
    return submit_bytes(runtime, size);
}

MeisoRuntimeStatus meiso_runtime_accept_catalog(MeisoRuntime *runtime)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    clear_pending_state_receive(runtime);
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (!runtime->catalog_decision_pending || !runtime->staged_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    status = submit_catalog_decision(runtime, WIRE_CATALOG_ACCEPT, 0U);
    if (status == MEISO_RUNTIME_OK) {
        commit_staged(runtime);
    }
    return status;
}

MeisoRuntimeStatus meiso_runtime_reject_catalog(
    MeisoRuntime *runtime,
    MeisoRuntimeCatalogRejectReason reason
)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);
    const uint8_t bank = runtime != NULL ? staging_bank(runtime) : 0U;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    clear_pending_state_receive(runtime);
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (!reject_reason_valid(reason)) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->catalog_decision_pending) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    status = submit_catalog_decision(runtime, WIRE_CATALOG_REJECT, reason);
    if (status == MEISO_RUNTIME_OK) {
        runtime->staged_present = false;
        clear_catalog_bank(runtime, bank);
        clear_catalog_decision(runtime);
    }
    return status;
}

static bool route_key_equal(MeisoRuntimeRouteKey lhs, MeisoRuntimeRouteKey rhs)
{
    return lhs.catalog_generation == rhs.catalog_generation &&
        meiso_endpoint_id_equal(lhs.endpoint_id, rhs.endpoint_id) &&
        lhs.endpoint_incarnation == rhs.endpoint_incarnation &&
        meiso_resource_id_equal(lhs.resource_id, rhs.resource_id) &&
        lhs.facet == rhs.facet;
}

static MeisoRuntimeStatus resolve_active_route(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey key,
    RouteSlot **out_route,
    EndpointSlot **out_endpoint
)
{
    int route_index;
    int endpoint_index;
    RouteSlot *route;

    if (out_route == NULL || out_endpoint == NULL ||
        !meiso_endpoint_id_valid(key.endpoint_id) ||
        key.endpoint_incarnation == 0U ||
        !meiso_resource_id_valid(key.resource_id) ||
        !meiso_runtime_facet_valid(key.facet)) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    if (key.catalog_generation != runtime->generation[runtime->active_bank]) {
        return MEISO_RUNTIME_STALE_CATALOG;
    }
    endpoint_index = endpoint_index_in_bank(
        runtime, runtime->active_bank, key.endpoint_id
    );
    if (endpoint_index < 0) {
        return MEISO_RUNTIME_UNKNOWN_ENDPOINT;
    }
    if (endpoint_slots(runtime, runtime->active_bank)
            [(size_t)endpoint_index].incarnation != key.endpoint_incarnation) {
        return MEISO_RUNTIME_STALE_ENDPOINT;
    }
    route_index = route_index_in_bank(
        runtime, runtime->active_bank, key.resource_id
    );
    if (route_index < 0) {
        return MEISO_RUNTIME_UNKNOWN_RESOURCE;
    }
    route = &route_slots(runtime, runtime->active_bank)[(size_t)route_index];
    if (!meiso_endpoint_id_equal(route->value.endpoint_id, key.endpoint_id)) {
        return MEISO_RUNTIME_UNKNOWN_RESOURCE;
    }
    if (route->value.endpoint_incarnation != key.endpoint_incarnation) {
        return MEISO_RUNTIME_STALE_ENDPOINT;
    }
    if (route->value.facet != key.facet) {
        return MEISO_RUNTIME_FACET_MISMATCH;
    }
    *out_route = route;
    *out_endpoint = &endpoint_slots(runtime, runtime->active_bank)
        [(size_t)endpoint_index];
    return MEISO_RUNTIME_OK;
}

static void encode_route_key(uint8_t *output, MeisoRuntimeRouteKey key)
{
    put_u64(output, key.catalog_generation);
    put_endpoint_id(output + 8U, key.endpoint_id);
    put_u64(output + 24U, key.endpoint_incarnation);
    put_resource_id(output + 32U, key.resource_id);
    output[48U] = key.facet;
    memset(output + 49U, 0, 7U);
}

static bool decode_route_key(
    const uint8_t *input,
    MeisoRuntimeRouteKey *out_key
)
{
    if (input == NULL || out_key == NULL || !bytes_zero(input + 49U, 7U)) {
        return false;
    }
    out_key->catalog_generation = get_u64(input);
    out_key->endpoint_id = get_endpoint_id(input + 8U);
    out_key->endpoint_incarnation = get_u64(input + 24U);
    out_key->resource_id = get_resource_id(input + 32U);
    out_key->facet = input[48U];
    return out_key->catalog_generation != 0U &&
        meiso_endpoint_id_valid(out_key->endpoint_id) &&
        out_key->endpoint_incarnation != 0U &&
        meiso_resource_id_valid(out_key->resource_id) &&
        meiso_runtime_facet_valid(out_key->facet);
}

static void canonicalize_content_id(
    const MeisoContentId *source,
    MeisoContentId *target
)
{
    memset(target, 0, sizeof(*target));
    target->algorithm = source->algorithm;
    target->digest_size = source->digest_size;
    memmove(target->digest, source->digest, source->digest_size);
}

static void encode_content_id(uint8_t *output, const MeisoContentId *value)
{
    put_u16(output, value->algorithm);
    output[2] = value->digest_size;
    output[3] = 0U;
    memmove(output + 4U, value->digest, MEISO_SHA256_DIGEST_SIZE);
}

static bool decode_content_id(
    const uint8_t input[CONTENT_ID_WIRE_SIZE],
    MeisoContentId *out_value
)
{
    MeisoContentId value;

    if (input == NULL || out_value == NULL || input[3] != 0U) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.algorithm = get_u16(input);
    value.digest_size = input[2];
    memmove(value.digest, input + 4U, MEISO_SHA256_DIGEST_SIZE);
    if (!meiso_content_id_valid(&value)) {
        return false;
    }
    *out_value = value;
    return true;
}

static bool blob_transfer_equal(
    const MeisoRuntimeBlobTransferRef *lhs,
    const MeisoRuntimeBlobTransferRef *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        route_key_equal(lhs->route, rhs->route) &&
        lhs->transfer_id == rhs->transfer_id &&
        meiso_content_id_equal(&lhs->content_id, &rhs->content_id) &&
        lhs->total_size == rhs->total_size;
}

static bool blob_operation_valid(MeisoRuntimeBlobOperation value)
{
    return value >= MEISO_RUNTIME_BLOB_BEGIN &&
        value <= MEISO_RUNTIME_BLOB_ABORT;
}

static bool blob_abort_reason_valid(MeisoRuntimeBlobAbortReason value)
{
    return value == MEISO_RUNTIME_BLOB_ABORT_CALLER_REQUEST;
}

static MeisoOwnerRole owner_role_for_session(MeisoSessionRole role)
{
    return role == MEISO_SESSION_ROLE_HOST ?
        MEISO_OWNER_ROLE_HOST : MEISO_OWNER_ROLE_EDGE;
}

static void production_snapshot_from_actual(
    MeisoRuntimeProductionSnapshot *value,
    const MeisoRuntimeActualState *actual,
    uint64_t high_water
)
{
    memset(value, 0, sizeof(*value));
    value->actual_present = true;
    value->observed_desired = actual->observed_desired;
    value->applied_desired = actual->applied_desired;
    value->apply_result = actual->apply_result;
    value->availability = actual->availability;
    value->effective_enabled = actual->effective_enabled;
    value->production_status = actual->production_status;
    value->reason = actual->reason;
    value->production_epoch = actual->production_epoch;
    value->production_epoch_high_water = high_water;
    value->data_active = actual_data_active(actual);
}

static bool production_transition_valid(
    const MeisoRuntime *runtime,
    uint8_t bank,
    size_t endpoint_index,
    const MeisoRuntimeActualState *actual
)
{
    const EndpointSlot *endpoint =
        &endpoint_slots_const(runtime, bank)[endpoint_index];
    const bool next_active = actual_data_active(actual);
    const bool applied_bound = !actual->applied_desired.present ||
        desired_reference_equal(
            actual->applied_desired, endpoint->desired
        ) || desired_reference_equal(
            actual->applied_desired,
            endpoint->production.applied_desired
        );

    if (actual->profile.size >
            runtime->max_actual_profile_bytes_per_endpoint ||
        !desired_reference_equal(
            actual->observed_desired, endpoint->desired
        ) || !applied_bound) {
        return false;
    }
    if (!next_active) {
        return true;
    }
    if (endpoint->production.data_active &&
        actual->production_epoch == endpoint->production.production_epoch) {
        return desired_reference_equal(
                actual->applied_desired,
                endpoint->production.applied_desired
            ) && actual->profile.size == endpoint->actual_profile_size &&
            (actual->profile.size == 0U || memcmp(
                actual->profile.data,
                actual_profile_bytes_const(runtime, bank, endpoint_index),
                actual->profile.size
            ) == 0);
    }
    return actual->production_epoch >
        endpoint->production.production_epoch_high_water;
}

static void apply_production(
    MeisoRuntime *runtime,
    uint8_t bank,
    size_t endpoint_index,
    const MeisoRuntimeActualState *actual
)
{
    EndpointSlot *endpoint = &endpoint_slots(runtime, bank)[endpoint_index];
    const int ledger_index = identity_pair_index(
        runtime, endpoint->id, endpoint->incarnation
    );
    uint64_t high_water = endpoint->production.production_epoch_high_water;

    if (actual_data_active(actual) && actual->production_epoch > high_water) {
        high_water = actual->production_epoch;
    }
    production_snapshot_from_actual(&endpoint->production, actual, high_water);
    if (ledger_index >= 0 &&
        identity_slots(runtime)[(size_t)ledger_index].current) {
        identity_slots(runtime)[(size_t)ledger_index]
            .production_epoch_high_water = high_water;
    }
    endpoint->actual_profile_size = (uint32_t)actual->profile.size;
    if (actual->profile.size != 0U) {
        memmove(
            actual_profile_bytes(runtime, bank, endpoint_index),
            actual->profile.data,
            actual->profile.size
        );
    }
}

static bool state_offer_valid(
    const MeisoStateOffer *offer,
    const MeisoResourceDescriptor *resource
)
{
    return offer != NULL && resource != NULL &&
        meiso_delivery_generation_valid(offer->generation) &&
        meiso_owner_scope_valid(offer->state.owner) &&
        offer->state.version != 0U &&
        meiso_byte_view_valid(offer->state.payload) &&
        offer->state.payload.size <= resource->max_payload_size &&
        offer->state.payload.size <= UINT32_MAX;
}

MeisoRuntimeStatus meiso_runtime_send_state_offer(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey route_key,
    const MeisoStateOffer *offer
)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);
    RouteSlot *route;
    EndpointSlot *endpoint;
    MeisoRuntimeActualState actual;
    MeisoRuntimeActualState committed_actual;
    MeisoRuntimeDesiredState desired;
    MeisoOwnerScope committed_owner;
    uint8_t *output;
    size_t size;
    size_t endpoint_index = 0U;
    uint64_t committed_version;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    clear_pending_state_receive(runtime);
    status = resolve_active_route(runtime, route_key, &route, &endpoint);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    endpoint_index = (size_t)(
        endpoint - endpoint_slots(runtime, runtime->active_bank)
    );
    if (route->value.resource.kind != MEISO_RESOURCE_KIND_STATE ||
        (route->value.facet != MEISO_RUNTIME_FACET_DESIRED &&
         route->value.facet != MEISO_RUNTIME_FACET_ACTUAL)) {
        return MEISO_RUNTIME_UNSUPPORTED_KIND;
    }
    if (!state_offer_valid(offer, &route->value.resource)) {
        return MEISO_RUNTIME_STATE_INVALID;
    }
    if (ranges_overlap(
            runtime,
            runtime->arena_size,
            offer->state.payload.data,
            offer->state.payload.size
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (offer->state.owner.role != route->value.resource.owner) {
        return MEISO_RUNTIME_OWNER_MISMATCH;
    }
    if (owner_role_for_session(runtime->local_role) !=
        route->value.resource.owner) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    committed_owner = offer->state.owner;
    committed_version = offer->state.version;
    if (route->value.facet == MEISO_RUNTIME_FACET_DESIRED) {
        if (!meiso_runtime_desired_state_decode(
                offer->state.payload, &desired
            )) {
            return MEISO_RUNTIME_STATE_INVALID;
        }
    } else {
        if (!meiso_runtime_actual_state_decode(
                offer->state.payload, &actual
            ) || !actual_state_for_route_valid(&actual)) {
            return MEISO_RUNTIME_STATE_INVALID;
        }
        if (!production_transition_valid(
                runtime, runtime->active_bank, endpoint_index, &actual
            )) {
            return MEISO_RUNTIME_STATE_INVALID;
        }
    }
    if (!checked_add(
            MEISO_RUNTIME_WIRE_HEADER_SIZE + STATE_OFFER_PREFIX_SIZE,
            offer->state.payload.size,
            &size
        ) || size > runtime->limits.max_runtime_payload_bytes ||
        size > UINT32_MAX) {
        return MEISO_RUNTIME_PAYLOAD_CAPACITY;
    }
    output = tx_bytes(runtime);
    encode_route_key(output + MEISO_RUNTIME_WIRE_HEADER_SIZE, route_key);
    put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 56U,
        offer->generation.value
    );
    output[MEISO_RUNTIME_WIRE_HEADER_SIZE + 64U] = offer->state.owner.role;
    memset(output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 65U, 0, 7U);
    put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 72U,
        offer->state.owner.incarnation
    );
    put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 80U,
        offer->state.version
    );
    put_u32(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 88U,
        (uint32_t)offer->state.payload.size
    );
    memset(output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 92U, 0, 4U);
    if (offer->state.payload.size != 0U) {
        memmove(
            output + MEISO_RUNTIME_WIRE_HEADER_SIZE + STATE_OFFER_PREFIX_SIZE,
            offer->state.payload.data,
            offer->state.payload.size
        );
    }
    if (route->value.facet == MEISO_RUNTIME_FACET_ACTUAL &&
        !meiso_runtime_actual_state_decode(
            (MeisoByteView){
                output + MEISO_RUNTIME_WIRE_HEADER_SIZE +
                    STATE_OFFER_PREFIX_SIZE,
                offer->state.payload.size
            },
            &committed_actual
        )) {
        return MEISO_RUNTIME_STATE_INVALID;
    }
    encode_header(runtime, output, WIRE_STATE_OFFER, (uint32_t)size);
    status = submit_bytes(runtime, size);
    if (status == MEISO_RUNTIME_OK) {
        if (route->value.facet == MEISO_RUNTIME_FACET_ACTUAL) {
            apply_production(
                runtime,
                runtime->active_bank,
                endpoint_index,
                &committed_actual
            );
        } else {
            endpoint->desired.present = true;
            endpoint->desired.owner = committed_owner;
            endpoint->desired.version = committed_version;
        }
    }
    return status;
}

MeisoRuntimeStatus meiso_runtime_send_state_accept(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey route_key,
    MeisoDeliveryGeneration generation,
    MeisoOwnerScope owner,
    uint64_t version
)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);
    RouteSlot *route;
    EndpointSlot *endpoint;
    uint8_t *output;
    const size_t size = MEISO_RUNTIME_WIRE_HEADER_SIZE + STATE_ACCEPT_SIZE;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    clear_pending_state_receive(runtime);
    status = resolve_active_route(runtime, route_key, &route, &endpoint);
    (void)endpoint;
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (route->value.resource.kind != MEISO_RESOURCE_KIND_STATE ||
        (route->value.facet != MEISO_RUNTIME_FACET_DESIRED &&
         route->value.facet != MEISO_RUNTIME_FACET_ACTUAL)) {
        return MEISO_RUNTIME_UNSUPPORTED_KIND;
    }
    if (!meiso_delivery_generation_valid(generation) ||
        !meiso_owner_scope_valid(owner) || version == 0U) {
        return MEISO_RUNTIME_STATE_INVALID;
    }
    if (owner.role != route->value.resource.owner) {
        return MEISO_RUNTIME_OWNER_MISMATCH;
    }
    if (owner_role_for_session(runtime->local_role) ==
        route->value.resource.owner) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    output = tx_bytes(runtime);
    encode_route_key(output + MEISO_RUNTIME_WIRE_HEADER_SIZE, route_key);
    put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 56U,
        generation.value
    );
    output[MEISO_RUNTIME_WIRE_HEADER_SIZE + 64U] = owner.role;
    memset(output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 65U, 0, 7U);
    put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 72U,
        owner.incarnation
    );
    put_u64(output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 80U, version);
    encode_header(runtime, output, WIRE_STATE_ACCEPT, (uint32_t)size);
    return submit_bytes(runtime, size);
}

static bool stream_sample_valid(
    const MeisoStreamSample *sample,
    const MeisoResourceDescriptor *resource
)
{
    return sample != NULL && resource != NULL &&
        meiso_owner_scope_valid(sample->key.producer) &&
        sample->key.epoch != 0U && sample->key.sequence != 0U &&
        meiso_time_point_valid(sample->source_time) &&
        meiso_deadline_valid(sample->sender_deadline) &&
        (!sample->sender_deadline.present ||
         sample->sender_deadline.value.clock_epoch ==
            sample->source_time.clock_epoch) &&
        meiso_byte_view_valid(sample->dependency) &&
        meiso_byte_view_valid(sample->payload) &&
        sample->dependency.size <= UINT32_MAX &&
        sample->payload.size <= resource->max_payload_size &&
        sample->payload.size <= UINT32_MAX;
}

MeisoRuntimeStatus meiso_runtime_send_stream_sample(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey route_key,
    uint64_t production_epoch,
    const MeisoStreamSample *sample
)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);
    RouteSlot *route;
    EndpointSlot *endpoint;
    uint8_t *output;
    size_t size;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    clear_pending_state_receive(runtime);
    status = resolve_active_route(runtime, route_key, &route, &endpoint);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (route->value.facet != MEISO_RUNTIME_FACET_DATA ||
        route->value.resource.kind != MEISO_RESOURCE_KIND_STREAM) {
        return MEISO_RUNTIME_UNSUPPORTED_KIND;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (!stream_sample_valid(sample, &route->value.resource)) {
        return MEISO_RUNTIME_STREAM_INVALID;
    }
    if (ranges_overlap(
            runtime,
            runtime->arena_size,
            sample->dependency.data,
            sample->dependency.size
        ) || ranges_overlap(
            runtime,
            runtime->arena_size,
            sample->payload.data,
            sample->payload.size
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (sample->key.producer.role != MEISO_OWNER_ROLE_EDGE) {
        return MEISO_RUNTIME_OWNER_MISMATCH;
    }
    if (!endpoint->production.data_active) {
        return MEISO_RUNTIME_ROUTE_NOT_ACTIVE;
    }
    if (production_epoch == 0U ||
        production_epoch != endpoint->production.production_epoch) {
        return MEISO_RUNTIME_PRODUCTION_EPOCH_MISMATCH;
    }
    if (!checked_add(
            MEISO_RUNTIME_WIRE_HEADER_SIZE + STREAM_SAMPLE_PREFIX_SIZE,
            sample->dependency.size,
            &size
        ) || !checked_add(size, sample->payload.size, &size) ||
        size > runtime->limits.max_runtime_payload_bytes || size > UINT32_MAX) {
        return MEISO_RUNTIME_PAYLOAD_CAPACITY;
    }
    output = tx_bytes(runtime);
    encode_route_key(output + MEISO_RUNTIME_WIRE_HEADER_SIZE, route_key);
    put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 56U,
        production_epoch
    );
    output[MEISO_RUNTIME_WIRE_HEADER_SIZE + 64U] =
        sample->key.producer.role;
    memset(output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 65U, 0, 7U);
    put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 72U,
        sample->key.producer.incarnation
    );
    put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 80U,
        sample->key.epoch
    );
    put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 88U,
        sample->key.sequence
    );
    put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 96U,
        sample->source_time.clock_epoch
    );
    put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 104U,
        sample->source_time.monotonic_ns
    );
    output[MEISO_RUNTIME_WIRE_HEADER_SIZE + 112U] =
        sample->sender_deadline.present ? 1U : 0U;
    memset(output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 113U, 0, 7U);
    put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 120U,
        sample->sender_deadline.present ?
            sample->sender_deadline.value.clock_epoch : 0U
    );
    put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 128U,
        sample->sender_deadline.present ?
            sample->sender_deadline.value.monotonic_ns : 0U
    );
    put_u32(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 136U,
        (uint32_t)sample->dependency.size
    );
    put_u32(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 140U,
        (uint32_t)sample->payload.size
    );
    if (sample->dependency.size != 0U) {
        memmove(
            output + MEISO_RUNTIME_WIRE_HEADER_SIZE +
                STREAM_SAMPLE_PREFIX_SIZE,
            sample->dependency.data,
            sample->dependency.size
        );
    }
    if (sample->payload.size != 0U) {
        memmove(
            output + MEISO_RUNTIME_WIRE_HEADER_SIZE +
                STREAM_SAMPLE_PREFIX_SIZE + sample->dependency.size,
            sample->payload.data,
            sample->payload.size
        );
    }
    encode_header(runtime, output, WIRE_STREAM_SAMPLE, (uint32_t)size);
    return submit_bytes(runtime, size);
}

static MeisoRuntimeStatus resolve_blob_route(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey key,
    RouteSlot **out_route
)
{
    EndpointSlot *endpoint;
    MeisoRuntimeStatus status;

    if (runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V3) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    status = resolve_active_route(runtime, key, out_route, &endpoint);
    (void)endpoint;
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if ((*out_route)->value.facet != MEISO_RUNTIME_FACET_IMMUTABLE) {
        return MEISO_RUNTIME_FACET_MISMATCH;
    }
    if ((*out_route)->value.resource.kind != MEISO_RESOURCE_KIND_BLOB) {
        return MEISO_RUNTIME_UNSUPPORTED_KIND;
    }
    if ((*out_route)->value.resource.owner != MEISO_OWNER_ROLE_HOST) {
        return MEISO_RUNTIME_OWNER_MISMATCH;
    }
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_send_blob_begin(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey route_key,
    const MeisoContentId *content_id,
    uint64_t total_size,
    MeisoRuntimeBlobTransferRef *out_transfer
)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);
    MeisoRuntimeBlobTransferRef transfer;
    RouteSlot *route;
    uint8_t *body;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->pending_desired_receive || runtime->pending_actual_receive) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (out_transfer == NULL || !meiso_content_id_valid(content_id) ||
        output_overlaps_runtime_or_session(
            runtime, out_transfer, sizeof(*out_transfer)
        ) || ranges_overlap(
            runtime, runtime->arena_size, content_id, sizeof(*content_id)
        )) {
        return MEISO_RUNTIME_BLOB_INVALID;
    }
    if (runtime->blob_transfer_active || runtime->blob_result_pending ||
        runtime->blob_publish_pending) {
        return MEISO_RUNTIME_BLOB_BUSY;
    }
    status = resolve_blob_route(runtime, route_key, &route);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (total_size > runtime->max_blob_transfer_bytes ||
        total_size > route->value.resource.max_payload_size) {
        return MEISO_RUNTIME_PAYLOAD_CAPACITY;
    }
    if (runtime->blob_transfer_id_high_water == UINT64_MAX) {
        return MEISO_RUNTIME_CAPACITY;
    }
    status = blob_tx_preflight(runtime);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    memset(&transfer, 0, sizeof(transfer));
    transfer.route = route_key;
    transfer.transfer_id = runtime->blob_transfer_id_high_water + 1U;
    canonicalize_content_id(content_id, &transfer.content_id);
    transfer.total_size = total_size;
    body = tx_bytes(runtime) + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    encode_route_key(body, transfer.route);
    put_u64(body + 56U, transfer.transfer_id);
    encode_content_id(body + 64U, &transfer.content_id);
    put_u64(body + 100U, transfer.total_size);
    encode_header(
        runtime,
        tx_bytes(runtime),
        WIRE_BLOB_BEGIN,
        MEISO_RUNTIME_V3_BLOB_BEGIN_WIRE_SIZE
    );
    status = submit_bytes(
        runtime, MEISO_RUNTIME_V3_BLOB_BEGIN_WIRE_SIZE
    );
    if (status == MEISO_RUNTIME_OK) {
        runtime->blob_transfer = transfer;
        runtime->blob_transfer_id_high_water = transfer.transfer_id;
        runtime->blob_transfer_active = true;
        runtime->blob_begin_ready = false;
        runtime->blob_next_offset = 0U;
        *out_transfer = transfer;
    }
    return status;
}

MeisoRuntimeStatus meiso_runtime_send_blob_chunk(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *transfer,
    uint64_t offset,
    MeisoByteView payload
)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);
    uint8_t *body;
    uint64_t end;
    size_t size;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->pending_desired_receive || runtime->pending_actual_receive) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (transfer == NULL || !meiso_byte_view_valid(payload) ||
        payload.size == 0U ||
        payload.size > MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE ||
        ranges_overlap(
            runtime, runtime->arena_size, transfer, sizeof(*transfer)
        ) || ranges_overlap(
            runtime, runtime->arena_size, payload.data, payload.size
        )) {
        return MEISO_RUNTIME_BLOB_INVALID;
    }
    if (!runtime->blob_transfer_active) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    if (!blob_transfer_equal(transfer, &runtime->blob_transfer)) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (!runtime->blob_begin_ready ||
        runtime->blob_outbound_terminal_wait) {
        return MEISO_RUNTIME_BLOB_BUSY;
    }
    if (offset != runtime->blob_next_offset ||
        payload.size > UINT64_MAX - offset) {
        return MEISO_RUNTIME_BLOB_INVALID;
    }
    end = offset + payload.size;
    if (end > transfer->total_size ||
        (payload.size < MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE &&
         end != transfer->total_size)) {
        return MEISO_RUNTIME_BLOB_INVALID;
    }
    status = blob_tx_preflight(runtime);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    size = MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE + payload.size;
    body = tx_bytes(runtime) + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    encode_route_key(body, transfer->route);
    put_u64(body + 56U, transfer->transfer_id);
    put_u64(body + 64U, offset);
    memmove(body + BLOB_CHUNK_PREFIX_SIZE, payload.data, payload.size);
    encode_header(runtime, tx_bytes(runtime), WIRE_BLOB_CHUNK, (uint32_t)size);
    status = submit_bytes(runtime, size);
    if (status == MEISO_RUNTIME_OK) {
        runtime->blob_next_offset = end;
    }
    return status;
}

MeisoRuntimeStatus meiso_runtime_send_blob_commit(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *transfer
)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);
    uint8_t *body;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->pending_desired_receive || runtime->pending_actual_receive) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (transfer == NULL || ranges_overlap(
            runtime, runtime->arena_size, transfer, sizeof(*transfer)
        )) {
        return MEISO_RUNTIME_BLOB_INVALID;
    }
    if (!runtime->blob_transfer_active) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    if (!blob_transfer_equal(transfer, &runtime->blob_transfer)) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (!runtime->blob_begin_ready ||
        runtime->blob_outbound_terminal_wait) {
        return MEISO_RUNTIME_BLOB_BUSY;
    }
    if (runtime->blob_next_offset != transfer->total_size) {
        return MEISO_RUNTIME_BLOB_INVALID;
    }
    status = blob_tx_preflight(runtime);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    body = tx_bytes(runtime) + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    encode_route_key(body, transfer->route);
    put_u64(body + 56U, transfer->transfer_id);
    encode_content_id(body + 64U, &transfer->content_id);
    put_u64(body + 100U, transfer->total_size);
    encode_header(
        runtime,
        tx_bytes(runtime),
        WIRE_BLOB_COMMIT,
        MEISO_RUNTIME_V3_BLOB_COMMIT_WIRE_SIZE
    );
    status = submit_bytes(
        runtime, MEISO_RUNTIME_V3_BLOB_COMMIT_WIRE_SIZE
    );
    if (status == MEISO_RUNTIME_OK) {
        runtime->blob_outbound_terminal_wait = true;
        runtime->blob_pending_operation = MEISO_RUNTIME_BLOB_COMMIT;
    }
    return status;
}

MeisoRuntimeStatus meiso_runtime_send_blob_abort(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *transfer,
    MeisoRuntimeBlobAbortReason reason
)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);
    uint8_t *body;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->pending_desired_receive || runtime->pending_actual_receive) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (transfer == NULL || !blob_abort_reason_valid(reason) ||
        ranges_overlap(
            runtime, runtime->arena_size, transfer, sizeof(*transfer)
        )) {
        return MEISO_RUNTIME_BLOB_INVALID;
    }
    if (!runtime->blob_transfer_active) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    if (!blob_transfer_equal(transfer, &runtime->blob_transfer)) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->blob_outbound_terminal_wait) {
        return MEISO_RUNTIME_BLOB_BUSY;
    }
    status = blob_tx_preflight(runtime);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    body = tx_bytes(runtime) + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    encode_route_key(body, transfer->route);
    put_u64(body + 56U, transfer->transfer_id);
    body[64U] = reason;
    memset(body + 65U, 0, 7U);
    encode_header(
        runtime,
        tx_bytes(runtime),
        WIRE_BLOB_ABORT,
        MEISO_RUNTIME_V3_BLOB_ABORT_WIRE_SIZE
    );
    status = submit_bytes(runtime, MEISO_RUNTIME_V3_BLOB_ABORT_WIRE_SIZE);
    if (status == MEISO_RUNTIME_OK) {
        runtime->blob_outbound_terminal_wait = true;
        runtime->blob_pending_operation = MEISO_RUNTIME_BLOB_ABORT;
    }
    return status;
}

static MeisoRuntimeStatus receive_catalog(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    uint64_t generation = 0U;
    MeisoRuntimeStatus status;

    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (runtime->catalog_decision_pending) {
        return MEISO_RUNTIME_CATALOG_PENDING;
    }
    status = decode_catalog_to_staging(runtime, frame, &generation);
    if (status == MEISO_RUNTIME_OK) {
        event->type = MEISO_RUNTIME_EVENT_CATALOG_OFFERED;
        event->catalog_generation = generation;
    }
    return status;
}

static MeisoRuntimeStatus receive_catalog_accept(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    uint64_t generation;

    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (frame.size !=
        MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_ACCEPT_SIZE) {
        return MEISO_RUNTIME_MALFORMED;
    }
    generation = get_u64(frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE);
    if (!runtime->catalog_decision_pending || !runtime->staged_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    if (generation == 0U ||
        generation != runtime->pending_catalog_generation ||
        generation != runtime->generation[staging_bank(runtime)]) {
        return MEISO_RUNTIME_STALE_CATALOG;
    }
    commit_staged(runtime);
    event->type = MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED;
    event->catalog_generation = generation;
    return MEISO_RUNTIME_OK;
}

static MeisoRuntimeStatus receive_catalog_reject(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    const uint8_t bank = staging_bank(runtime);
    uint64_t generation;
    MeisoRuntimeCatalogRejectReason reason;

    if (frame.size != MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_REJECT_SIZE ||
        !bytes_zero(
            frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 9U,
            3U
        )) {
        return MEISO_RUNTIME_MALFORMED;
    }
    generation = get_u64(frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE);
    reason = frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U];
    if (!reject_reason_valid(reason)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (!runtime->catalog_decision_pending) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    if (generation == 0U || generation != runtime->pending_catalog_generation) {
        return MEISO_RUNTIME_STALE_CATALOG;
    }
    runtime->staged_present = false;
    clear_catalog_bank(runtime, bank);
    clear_catalog_decision(runtime);
    event->type = MEISO_RUNTIME_EVENT_CATALOG_REJECTED;
    event->catalog_generation = generation;
    event->reject_reason = reason;
    return MEISO_RUNTIME_OK;
}

static MeisoRuntimeStatus receive_state_offer(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    const uint8_t *body = frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    MeisoRuntimeRouteKey key;
    MeisoStateOffer offer;
    MeisoRuntimeActualState actual;
    MeisoRuntimeDesiredState desired;
    RouteSlot *route;
    EndpointSlot *endpoint;
    uint32_t payload_size;
    MeisoRuntimeStatus status;

    if (frame.size <
            MEISO_RUNTIME_WIRE_HEADER_SIZE + STATE_OFFER_PREFIX_SIZE ||
        !decode_route_key(body, &key) ||
        !bytes_zero(body + 65U, 7U) ||
        !bytes_zero(body + 92U, 4U)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    payload_size = get_u32(body + 88U);
    if (payload_size != frame.size -
            (MEISO_RUNTIME_WIRE_HEADER_SIZE + STATE_OFFER_PREFIX_SIZE)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    offer.generation.value = get_u64(body + 56U);
    offer.state.owner.role = body[64U];
    offer.state.owner.incarnation = get_u64(body + 72U);
    offer.state.version = get_u64(body + 80U);
    offer.state.payload.data = body + STATE_OFFER_PREFIX_SIZE;
    offer.state.payload.size = payload_size;

    status = resolve_active_route(runtime, key, &route, &endpoint);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (route->value.resource.kind != MEISO_RESOURCE_KIND_STATE ||
        (route->value.facet != MEISO_RUNTIME_FACET_DESIRED &&
         route->value.facet != MEISO_RUNTIME_FACET_ACTUAL)) {
        return MEISO_RUNTIME_UNSUPPORTED_KIND;
    }
    if (!state_offer_valid(&offer, &route->value.resource)) {
        return MEISO_RUNTIME_STATE_INVALID;
    }
    if (offer.state.owner.role != route->value.resource.owner) {
        return MEISO_RUNTIME_OWNER_MISMATCH;
    }
    if (owner_role_for_session(runtime->local_role) ==
        route->value.resource.owner) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    runtime->pending_actual_route = key;
    runtime->pending_actual_delivery = offer.generation;
    runtime->pending_actual_owner = offer.state.owner;
    runtime->pending_actual_version = offer.state.version;
    if (route->value.facet == MEISO_RUNTIME_FACET_DESIRED) {
        if (!meiso_runtime_desired_state_decode(
                offer.state.payload, &desired
            )) {
            return MEISO_RUNTIME_STATE_INVALID;
        }
        if (offer.state.payload.size >
            MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE +
                runtime->max_desired_profile_bytes_per_endpoint) {
            return MEISO_RUNTIME_STATE_INVALID;
        }
        runtime->pending_desired_payload_size =
            (uint32_t)offer.state.payload.size;
        if (offer.state.payload.size != 0U) {
            memmove(
                pending_desired_payload_bytes(runtime),
                offer.state.payload.data,
                offer.state.payload.size
            );
        }
        runtime->pending_desired_receive = true;
    } else {
        if (!meiso_runtime_actual_state_decode(
                offer.state.payload, &actual
            ) || !actual_state_for_route_valid(&actual)) {
            return MEISO_RUNTIME_STATE_INVALID;
        }
        production_snapshot_from_actual(
            &runtime->pending_actual_value,
            &actual,
            endpoint->production.production_epoch_high_water
        );
        runtime->pending_actual_profile_size =
            (uint32_t)actual.profile.size;
        if (actual.profile.size != 0U) {
            memmove(
                pending_actual_profile_bytes(runtime),
                actual.profile.data,
                actual.profile.size
            );
        }
        runtime->pending_actual_receive = true;
    }
    event->type = MEISO_RUNTIME_EVENT_STATE_OFFER;
    event->catalog_generation = key.catalog_generation;
    event->route = key;
    event->state_offer = offer;
    (void)endpoint;
    return MEISO_RUNTIME_OK;
}

static MeisoRuntimeStatus receive_state_accept(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    const uint8_t *body = frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    MeisoRuntimeRouteKey key;
    MeisoDeliveryGeneration generation;
    MeisoOwnerScope owner;
    uint64_t version;
    RouteSlot *route;
    EndpointSlot *endpoint;
    MeisoRuntimeStatus status;

    if (frame.size != MEISO_RUNTIME_WIRE_HEADER_SIZE + STATE_ACCEPT_SIZE ||
        !decode_route_key(body, &key) || !bytes_zero(body + 65U, 7U)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    generation.value = get_u64(body + 56U);
    owner.role = body[64U];
    owner.incarnation = get_u64(body + 72U);
    version = get_u64(body + 80U);
    if (!meiso_delivery_generation_valid(generation) ||
        !meiso_owner_scope_valid(owner) || version == 0U) {
        return MEISO_RUNTIME_STATE_INVALID;
    }
    status = resolve_active_route(runtime, key, &route, &endpoint);
    (void)endpoint;
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (route->value.resource.kind != MEISO_RESOURCE_KIND_STATE ||
        (route->value.facet != MEISO_RUNTIME_FACET_DESIRED &&
         route->value.facet != MEISO_RUNTIME_FACET_ACTUAL)) {
        return MEISO_RUNTIME_UNSUPPORTED_KIND;
    }
    if (owner.role != route->value.resource.owner) {
        return MEISO_RUNTIME_OWNER_MISMATCH;
    }
    if (owner_role_for_session(runtime->local_role) !=
        route->value.resource.owner) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    event->type = MEISO_RUNTIME_EVENT_STATE_ACCEPT;
    event->catalog_generation = key.catalog_generation;
    event->route = key;
    event->state_accept_generation = generation;
    event->state_accept_owner = owner;
    event->state_accept_version = version;
    return MEISO_RUNTIME_OK;
}

static MeisoRuntimeStatus receive_stream_sample(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    const uint8_t *body = frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    MeisoRuntimeRouteKey key;
    MeisoStreamSample sample;
    uint64_t production_epoch;
    uint32_t dependency_size;
    uint32_t payload_size;
    size_t variable_size;
    RouteSlot *route;
    EndpointSlot *endpoint;
    MeisoRuntimeStatus status;

    if (frame.size <
            MEISO_RUNTIME_WIRE_HEADER_SIZE + STREAM_SAMPLE_PREFIX_SIZE ||
        !decode_route_key(body, &key) ||
        !bytes_zero(body + 65U, 7U) ||
        body[112U] > 1U || !bytes_zero(body + 113U, 7U)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    dependency_size = get_u32(body + 136U);
    payload_size = get_u32(body + 140U);
    if (!checked_add(dependency_size, payload_size, &variable_size) ||
        variable_size != frame.size -
            (MEISO_RUNTIME_WIRE_HEADER_SIZE + STREAM_SAMPLE_PREFIX_SIZE)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    production_epoch = get_u64(body + 56U);
    memset(&sample, 0, sizeof(sample));
    sample.key.producer.role = body[64U];
    sample.key.producer.incarnation = get_u64(body + 72U);
    sample.key.epoch = get_u64(body + 80U);
    sample.key.sequence = get_u64(body + 88U);
    sample.source_time.clock_epoch = get_u64(body + 96U);
    sample.source_time.monotonic_ns = get_u64(body + 104U);
    sample.sender_deadline.present = body[112U] != 0U;
    sample.sender_deadline.value.clock_epoch = get_u64(body + 120U);
    sample.sender_deadline.value.monotonic_ns = get_u64(body + 128U);
    if (!sample.sender_deadline.present &&
        (sample.sender_deadline.value.clock_epoch != 0U ||
         sample.sender_deadline.value.monotonic_ns != 0U)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    sample.dependency.data = body + STREAM_SAMPLE_PREFIX_SIZE;
    sample.dependency.size = dependency_size;
    sample.payload.data = body + STREAM_SAMPLE_PREFIX_SIZE + dependency_size;
    sample.payload.size = payload_size;

    status = resolve_active_route(runtime, key, &route, &endpoint);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (route->value.facet != MEISO_RUNTIME_FACET_DATA ||
        route->value.resource.kind != MEISO_RESOURCE_KIND_STREAM) {
        return MEISO_RUNTIME_UNSUPPORTED_KIND;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (!stream_sample_valid(&sample, &route->value.resource)) {
        return MEISO_RUNTIME_STREAM_INVALID;
    }
    if (sample.key.producer.role != MEISO_OWNER_ROLE_EDGE) {
        return MEISO_RUNTIME_OWNER_MISMATCH;
    }
    if (!endpoint->production.data_active) {
        return MEISO_RUNTIME_ROUTE_NOT_ACTIVE;
    }
    if (production_epoch == 0U ||
        production_epoch != endpoint->production.production_epoch) {
        return MEISO_RUNTIME_PRODUCTION_EPOCH_MISMATCH;
    }
    event->type = MEISO_RUNTIME_EVENT_STREAM_SAMPLE;
    event->catalog_generation = key.catalog_generation;
    event->route = key;
    event->production_epoch = production_epoch;
    event->stream_sample = sample;
    return MEISO_RUNTIME_OK;
}

static void set_blob_result(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *transfer,
    MeisoRuntimeBlobOperation operation,
    MeisoRuntimeBlobResult result,
    uint64_t next_offset
)
{
    runtime->blob_result_transfer = *transfer;
    runtime->blob_pending_operation = operation;
    runtime->blob_pending_result = result;
    runtime->blob_result_next_offset = next_offset;
    runtime->blob_result_pending = true;
}

static void fill_blob_request_event(
    MeisoRuntimeEvent *event,
    MeisoRuntimeBlobOperation operation,
    const MeisoRuntimeBlobTransferRef *transfer,
    uint64_t offset,
    MeisoByteView payload
)
{
    event->type = MEISO_RUNTIME_EVENT_BLOB_REQUEST;
    event->catalog_generation = transfer->route.catalog_generation;
    event->route = transfer->route;
    event->blob_operation = operation;
    event->blob_transfer = *transfer;
    event->blob_offset = offset;
    event->blob_payload = payload;
}

static bool blob_result_is_terminal(MeisoRuntimeBlobResult result)
{
    return result >= MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
        result <= MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE;
}

static bool blob_result_reference_equal(
    const MeisoRuntimeBlobTransferRef *lhs,
    const MeisoRuntimeBlobTransferRef *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        route_key_equal(lhs->route, rhs->route) &&
        lhs->transfer_id == rhs->transfer_id &&
        lhs->total_size == rhs->total_size;
}

static bool blob_result_combination_valid(
    MeisoRuntimeBlobOperation operation,
    MeisoRuntimeBlobResult result
)
{
    if (!blob_operation_valid(operation)) {
        return false;
    }
    if (operation == MEISO_RUNTIME_BLOB_BEGIN) {
        return result == MEISO_RUNTIME_BLOB_RESULT_READY ||
            (result >= MEISO_RUNTIME_BLOB_RESULT_BUSY &&
             result <= MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE);
    }
    if (operation == MEISO_RUNTIME_BLOB_CHUNK) {
        return result == MEISO_RUNTIME_BLOB_RESULT_DISCONTINUITY ||
            result == MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE;
    }
    if (operation == MEISO_RUNTIME_BLOB_COMMIT) {
        return result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED ||
            result == MEISO_RUNTIME_BLOB_RESULT_DUPLICATE ||
            (result >= MEISO_RUNTIME_BLOB_RESULT_BUSY &&
             result <= MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE);
    }
    return result == MEISO_RUNTIME_BLOB_RESULT_ABORTED;
}

static MeisoRuntimeStatus receive_blob_begin(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    const uint8_t *body = frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    MeisoRuntimeBlobTransferRef transfer;
    RouteSlot *route;
    MeisoRuntimeStatus status;
    bool content_valid;

    if (runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V3) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (frame.size != MEISO_RUNTIME_V3_BLOB_BEGIN_WIRE_SIZE ||
        !decode_route_key(body, &transfer.route) || body[67U] != 0U) {
        return MEISO_RUNTIME_MALFORMED;
    }
    memset(&transfer.content_id, 0, sizeof(transfer.content_id));
    transfer.transfer_id = get_u64(body + 56U);
    content_valid = decode_content_id(body + 64U, &transfer.content_id);
    transfer.total_size = get_u64(body + 100U);
    if (transfer.transfer_id == 0U) {
        return MEISO_RUNTIME_MALFORMED;
    }
    status = resolve_blob_route(runtime, transfer.route, &route);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->blob_result_pending || runtime->blob_publish_pending) {
        return MEISO_RUNTIME_BLOB_BUSY;
    }
    if (runtime->blob_transfer_active &&
        transfer.transfer_id == runtime->blob_transfer.transfer_id) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (transfer.transfer_id <= runtime->blob_transfer_id_high_water) {
        set_blob_result(
            runtime,
            &transfer,
            MEISO_RUNTIME_BLOB_BEGIN,
            MEISO_RUNTIME_BLOB_RESULT_CONFLICT,
            0U
        );
        fill_blob_request_event(
            event,
            MEISO_RUNTIME_BLOB_BEGIN,
            &transfer,
            0U,
            (MeisoByteView){NULL, 0U}
        );
        return MEISO_RUNTIME_OK;
    }
    runtime->blob_transfer_id_high_water = transfer.transfer_id;
    if (!content_valid) {
        set_blob_result(
            runtime,
            &transfer,
            MEISO_RUNTIME_BLOB_BEGIN,
            MEISO_RUNTIME_BLOB_RESULT_INVALID_CONTENT_ID,
            0U
        );
    } else if (runtime->blob_transfer_active) {
        set_blob_result(
            runtime,
            &transfer,
            MEISO_RUNTIME_BLOB_BEGIN,
            MEISO_RUNTIME_BLOB_RESULT_BUSY,
            0U
        );
    } else if (transfer.total_size > runtime->max_blob_transfer_bytes ||
               transfer.total_size > route->value.resource.max_payload_size ||
               (sizeof(size_t) < sizeof(uint64_t) &&
                transfer.total_size > (uint64_t)SIZE_MAX)) {
        set_blob_result(
            runtime,
            &transfer,
            MEISO_RUNTIME_BLOB_BEGIN,
            MEISO_RUNTIME_BLOB_RESULT_CAPACITY,
            0U
        );
    } else {
        if (runtime->max_blob_transfer_bytes != 0U) {
            memset(
                blob_scratch(runtime),
                0,
                runtime->max_blob_transfer_bytes
            );
        }
        runtime->blob_transfer = transfer;
        runtime->blob_next_offset = 0U;
        runtime->blob_transfer_active = true;
        runtime->blob_begin_ready = false;
        set_blob_result(
            runtime,
            &transfer,
            MEISO_RUNTIME_BLOB_BEGIN,
            MEISO_RUNTIME_BLOB_RESULT_READY,
            0U
        );
    }
    fill_blob_request_event(
        event,
        MEISO_RUNTIME_BLOB_BEGIN,
        &transfer,
        0U,
        (MeisoByteView){NULL, 0U}
    );
    return MEISO_RUNTIME_OK;
}

static MeisoRuntimeStatus receive_blob_chunk(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    const uint8_t *body = frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    MeisoRuntimeRouteKey route;
    MeisoByteView payload;
    uint64_t transfer_id;
    uint64_t offset;
    uint64_t end;
    RouteSlot *route_slot;
    MeisoRuntimeStatus status;

    if (runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V3) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (frame.size <= MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE ||
        frame.size > MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE +
                MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE ||
        !decode_route_key(body, &route)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    transfer_id = get_u64(body + 56U);
    offset = get_u64(body + 64U);
    payload.data = body + BLOB_CHUNK_PREFIX_SIZE;
    payload.size = frame.size - MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE;
    status = resolve_blob_route(runtime, route, &route_slot);
    (void)route_slot;
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (!runtime->blob_transfer_active) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    if (!route_key_equal(route, runtime->blob_transfer.route) ||
        transfer_id != runtime->blob_transfer.transfer_id) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->blob_result_pending || runtime->blob_publish_pending ||
        !runtime->blob_begin_ready) {
        return MEISO_RUNTIME_BLOB_BUSY;
    }
    if (offset != runtime->blob_next_offset ||
        payload.size > UINT64_MAX - offset) {
        set_blob_result(
            runtime,
            &runtime->blob_transfer,
            MEISO_RUNTIME_BLOB_CHUNK,
            MEISO_RUNTIME_BLOB_RESULT_DISCONTINUITY,
            runtime->blob_next_offset
        );
    } else {
        end = offset + payload.size;
        if (end > runtime->blob_transfer.total_size ||
            (payload.size < MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE &&
             end != runtime->blob_transfer.total_size)) {
            set_blob_result(
                runtime,
                &runtime->blob_transfer,
                MEISO_RUNTIME_BLOB_CHUNK,
                MEISO_RUNTIME_BLOB_RESULT_DISCONTINUITY,
                runtime->blob_next_offset
            );
        } else {
            memmove(blob_scratch(runtime) + (size_t)offset,
                    payload.data, payload.size);
            runtime->blob_next_offset = end;
        }
    }
    fill_blob_request_event(
        event,
        MEISO_RUNTIME_BLOB_CHUNK,
        &runtime->blob_transfer,
        offset,
        payload
    );
    return MEISO_RUNTIME_OK;
}

static MeisoRuntimeStatus receive_blob_commit(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    const uint8_t *body = frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    MeisoRuntimeBlobTransferRef transfer;
    RouteSlot *route;
    MeisoRuntimeStatus status;
    MeisoByteView content;

    if (runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V3) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (frame.size != MEISO_RUNTIME_V3_BLOB_COMMIT_WIRE_SIZE ||
        !decode_route_key(body, &transfer.route) ||
        !decode_content_id(body + 64U, &transfer.content_id)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    transfer.transfer_id = get_u64(body + 56U);
    transfer.total_size = get_u64(body + 100U);
    status = resolve_blob_route(runtime, transfer.route, &route);
    (void)route;
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (!runtime->blob_transfer_active) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    if (!blob_transfer_equal(&transfer, &runtime->blob_transfer)) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->blob_result_pending || runtime->blob_publish_pending ||
        !runtime->blob_begin_ready) {
        return MEISO_RUNTIME_BLOB_BUSY;
    }
    content.data = blob_scratch(runtime);
    content.size = (size_t)transfer.total_size;
    if (runtime->blob_next_offset != transfer.total_size) {
        set_blob_result(
            runtime,
            &transfer,
            MEISO_RUNTIME_BLOB_COMMIT,
            MEISO_RUNTIME_BLOB_RESULT_LENGTH_MISMATCH,
            runtime->blob_next_offset
        );
    } else if (!meiso_runtime_blob_sha256_verify(
                   &transfer.content_id, content, NULL
               )) {
        set_blob_result(
            runtime,
            &transfer,
            MEISO_RUNTIME_BLOB_COMMIT,
            MEISO_RUNTIME_BLOB_RESULT_INTEGRITY_FAILED,
            runtime->blob_next_offset
        );
    } else {
        runtime->blob_publish_pending = true;
        runtime->blob_pending_operation = MEISO_RUNTIME_BLOB_COMMIT;
    }
    fill_blob_request_event(
        event,
        MEISO_RUNTIME_BLOB_COMMIT,
        &transfer,
        0U,
        content
    );
    return MEISO_RUNTIME_OK;
}

static MeisoRuntimeStatus receive_blob_abort(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    const uint8_t *body = frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    MeisoRuntimeRouteKey route;
    uint64_t transfer_id;
    RouteSlot *route_slot;
    MeisoRuntimeStatus status;

    if (runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V3) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (frame.size != MEISO_RUNTIME_V3_BLOB_ABORT_WIRE_SIZE ||
        !decode_route_key(body, &route) ||
        !blob_abort_reason_valid(body[64U]) ||
        !bytes_zero(body + 65U, 7U)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    transfer_id = get_u64(body + 56U);
    status = resolve_blob_route(runtime, route, &route_slot);
    (void)route_slot;
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (!runtime->blob_transfer_active) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    if (!route_key_equal(route, runtime->blob_transfer.route) ||
        transfer_id != runtime->blob_transfer.transfer_id) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->blob_publish_pending ||
        (runtime->blob_result_pending &&
         runtime->blob_pending_result != MEISO_RUNTIME_BLOB_RESULT_READY)) {
        return MEISO_RUNTIME_BLOB_BUSY;
    }
    memset(blob_scratch(runtime), 0, runtime->max_blob_transfer_bytes);
    set_blob_result(
        runtime,
        &runtime->blob_transfer,
        MEISO_RUNTIME_BLOB_ABORT,
        MEISO_RUNTIME_BLOB_RESULT_ABORTED,
        runtime->blob_next_offset
    );
    fill_blob_request_event(
        event,
        MEISO_RUNTIME_BLOB_ABORT,
        &runtime->blob_transfer,
        runtime->blob_next_offset,
        (MeisoByteView){NULL, 0U}
    );
    return MEISO_RUNTIME_OK;
}

static MeisoRuntimeStatus receive_blob_result(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    const uint8_t *body = frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    MeisoRuntimeRouteKey route;
    MeisoRuntimeBlobOperation operation;
    MeisoRuntimeBlobResult result;
    uint64_t transfer_id;
    uint64_t next_offset;
    uint64_t total_size;
    MeisoRuntimeBlobTransferRef transfer;
    bool terminal;
    bool canonical_prefix;

    if (runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V3) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (frame.size != MEISO_RUNTIME_V3_BLOB_RESULT_WIRE_SIZE ||
        !decode_route_key(body, &route) || !bytes_zero(body + 66U, 2U)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    transfer_id = get_u64(body + 56U);
    operation = body[64U];
    result = body[65U];
    next_offset = get_u64(body + 68U);
    total_size = get_u64(body + 76U);
    if (!blob_result_combination_valid(operation, result)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    if (!runtime->blob_transfer_active) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    transfer = runtime->blob_transfer;
    if (!route_key_equal(route, transfer.route) ||
        transfer_id != transfer.transfer_id ||
        total_size != transfer.total_size || next_offset > total_size) {
        return MEISO_RUNTIME_CONFLICT;
    }
    terminal = blob_result_is_terminal(result);
    canonical_prefix = next_offset <= runtime->blob_next_offset &&
        (next_offset == total_size ||
         next_offset % MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE == 0U);
    if (operation == MEISO_RUNTIME_BLOB_BEGIN) {
        if (next_offset != 0U) {
            return MEISO_RUNTIME_MALFORMED;
        }
        if (result == MEISO_RUNTIME_BLOB_RESULT_READY) {
            if (runtime->blob_begin_ready) {
                return MEISO_RUNTIME_CONFLICT;
            }
        } else if (runtime->blob_begin_ready) {
            return MEISO_RUNTIME_CONFLICT;
        }
    } else if (operation == MEISO_RUNTIME_BLOB_CHUNK) {
        if (!runtime->blob_begin_ready || runtime->blob_next_offset == 0U ||
            !canonical_prefix) {
            return MEISO_RUNTIME_CONFLICT;
        }
    } else if (operation == MEISO_RUNTIME_BLOB_COMMIT) {
        if (!runtime->blob_outbound_terminal_wait ||
            runtime->blob_pending_operation != MEISO_RUNTIME_BLOB_COMMIT ||
            ((result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED ||
              result == MEISO_RUNTIME_BLOB_RESULT_DUPLICATE) ?
                next_offset != total_size : !canonical_prefix)) {
            return MEISO_RUNTIME_CONFLICT;
        }
    } else if (!runtime->blob_outbound_terminal_wait ||
               runtime->blob_pending_operation != MEISO_RUNTIME_BLOB_ABORT ||
               !canonical_prefix) {
        return MEISO_RUNTIME_CONFLICT;
    }
    event->type = MEISO_RUNTIME_EVENT_BLOB_RESULT;
    event->catalog_generation = route.catalog_generation;
    event->route = route;
    event->blob_operation = operation;
    event->blob_transfer = transfer;
    event->blob_result = result;
    event->blob_next_offset = next_offset;
    if (terminal) {
        clear_blob_transfer(runtime);
    } else if (!runtime->blob_outbound_terminal_wait) {
        runtime->blob_begin_ready = true;
    }
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_receive(
    MeisoRuntime *runtime,
    const MeisoSessionRuntimeMessage *message,
    MeisoRuntimeEvent *out_event
)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);
    uint8_t type;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    clear_pending_state_receive(runtime);
    if (message == NULL || out_event == NULL ||
        !meiso_byte_view_valid(message->payload) ||
        !meiso_session_id_equal(message->session_id, runtime->session_id) ||
        output_overlaps_runtime_or_session(
            runtime, out_event, sizeof(*out_event)
        ) || ranges_overlap(
            out_event,
            sizeof(*out_event),
            message->payload.data,
            message->payload.size
        )) {
        return message != NULL &&
            !meiso_session_id_equal(message->session_id, runtime->session_id) ?
            MEISO_RUNTIME_STALE_SESSION : MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (message->payload.size >= MEISO_RUNTIME_WIRE_HEADER_SIZE &&
        message->payload.size <= UINT32_MAX &&
        message->payload.data[0] == (uint8_t)'M' &&
        message->payload.data[1] == (uint8_t)'R' &&
        message->payload.data[2] == (uint8_t)'T' &&
        message->payload.data[3] == (uint8_t)'0' &&
        message->payload.data[6] == 0U &&
        message->payload.data[7] == 0U &&
        get_u32(message->payload.data + 8U) == message->payload.size &&
        message->payload.data[4] != runtime->wire_version) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (!decode_header(runtime, message->payload, &type)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    memset(out_event, 0, sizeof(*out_event));
    switch (type) {
    case WIRE_CATALOG:
        return receive_catalog(runtime, message->payload, out_event);
    case WIRE_CATALOG_ACCEPT:
        return receive_catalog_accept(runtime, message->payload, out_event);
    case WIRE_CATALOG_REJECT:
        return receive_catalog_reject(runtime, message->payload, out_event);
    case WIRE_STATE_OFFER:
        return receive_state_offer(runtime, message->payload, out_event);
    case WIRE_STATE_ACCEPT:
        return receive_state_accept(runtime, message->payload, out_event);
    case WIRE_STREAM_SAMPLE:
        return receive_stream_sample(runtime, message->payload, out_event);
    case WIRE_BLOB_BEGIN:
        return receive_blob_begin(runtime, message->payload, out_event);
    case WIRE_BLOB_CHUNK:
        return receive_blob_chunk(runtime, message->payload, out_event);
    case WIRE_BLOB_COMMIT:
        return receive_blob_commit(runtime, message->payload, out_event);
    case WIRE_BLOB_ABORT:
        return receive_blob_abort(runtime, message->payload, out_event);
    case WIRE_BLOB_RESULT:
        return receive_blob_result(runtime, message->payload, out_event);
    default:
        return MEISO_RUNTIME_MALFORMED;
    }
}

typedef struct {
    uint32_t state[8];
    uint8_t block[64];
    uint64_t total_bytes;
    size_t block_size;
} RuntimeSha256;

static uint32_t runtime_rotate_right(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32U - count));
}

static void runtime_sha256_transform(
    RuntimeSha256 *sha,
    const uint8_t block[64]
)
{
    static const uint32_t constants[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        words[index] = get_u32(block + index * 4U);
    }
    for (; index < 64U; ++index) {
        const uint32_t s0 =
            runtime_rotate_right(words[index - 15U], 7U) ^
            runtime_rotate_right(words[index - 15U], 18U) ^
            (words[index - 15U] >> 3U);
        const uint32_t s1 =
            runtime_rotate_right(words[index - 2U], 17U) ^
            runtime_rotate_right(words[index - 2U], 19U) ^
            (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + s0 +
            words[index - 7U] + s1;
    }
    a = sha->state[0];
    b = sha->state[1];
    c = sha->state[2];
    d = sha->state[3];
    e = sha->state[4];
    f = sha->state[5];
    g = sha->state[6];
    h = sha->state[7];
    for (index = 0U; index < 64U; ++index) {
        const uint32_t s1 = runtime_rotate_right(e, 6U) ^
            runtime_rotate_right(e, 11U) ^ runtime_rotate_right(e, 25U);
        const uint32_t choice = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + choice + constants[index] + words[index];
        const uint32_t s0 = runtime_rotate_right(a, 2U) ^
            runtime_rotate_right(a, 13U) ^ runtime_rotate_right(a, 22U);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    sha->state[0] += a;
    sha->state[1] += b;
    sha->state[2] += c;
    sha->state[3] += d;
    sha->state[4] += e;
    sha->state[5] += f;
    sha->state[6] += g;
    sha->state[7] += h;
}

static void runtime_sha256_init(RuntimeSha256 *sha)
{
    static const uint32_t initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    memset(sha, 0, sizeof(*sha));
    memmove(sha->state, initial, sizeof(initial));
}

static void runtime_sha256_update(
    RuntimeSha256 *sha,
    const uint8_t *data,
    size_t size
)
{
    while (size != 0U) {
        const size_t available = sizeof(sha->block) - sha->block_size;
        const size_t take = size < available ? size : available;

        memmove(sha->block + sha->block_size, data, take);
        sha->block_size += take;
        sha->total_bytes += take;
        data += take;
        size -= take;
        if (sha->block_size == sizeof(sha->block)) {
            runtime_sha256_transform(sha, sha->block);
            sha->block_size = 0U;
        }
    }
}

static void runtime_sha256_final(
    RuntimeSha256 *sha,
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    const uint64_t total_bits = sha->total_bytes * 8U;
    size_t index;

    sha->block[sha->block_size++] = 0x80U;
    if (sha->block_size > 56U) {
        memset(sha->block + sha->block_size, 0,
               sizeof(sha->block) - sha->block_size);
        runtime_sha256_transform(sha, sha->block);
        sha->block_size = 0U;
    }
    memset(sha->block + sha->block_size, 0, 56U - sha->block_size);
    for (index = 0U; index < 8U; ++index) {
        sha->block[56U + index] =
            (uint8_t)(total_bits >> (56U - index * 8U));
    }
    runtime_sha256_transform(sha, sha->block);
    for (index = 0U; index < 8U; ++index) {
        put_u32(digest + index * 4U, sha->state[index]);
    }
}

bool meiso_runtime_blob_sha256_verify(
    const MeisoContentId *expected_content_id,
    MeisoByteView content,
    void *context
)
{
    RuntimeSha256 sha;
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE];

    (void)context;
    if (!meiso_content_id_valid(expected_content_id) ||
        !meiso_byte_view_valid(content)) {
        return false;
    }
    runtime_sha256_init(&sha);
    runtime_sha256_update(&sha, content.data, content.size);
    runtime_sha256_final(&sha, digest);
    return memcmp(
        digest,
        expected_content_id->digest,
        MEISO_SHA256_DIGEST_SIZE
    ) == 0;
}

static MeisoRuntimeBlobResult blob_stage_result(MeisoBlobStageStatus status)
{
    switch (status) {
    case MEISO_BLOB_STAGE_UNCHANGED:
    case MEISO_BLOB_STAGE_BUSY:
        return MEISO_RUNTIME_BLOB_RESULT_BUSY;
    case MEISO_BLOB_STAGE_CONFLICT:
        return MEISO_RUNTIME_BLOB_RESULT_CONFLICT;
    case MEISO_BLOB_STAGE_INVALID_CONTENT_ID:
        return MEISO_RUNTIME_BLOB_RESULT_INVALID_CONTENT_ID;
    case MEISO_BLOB_STAGE_CAPACITY:
        return MEISO_RUNTIME_BLOB_RESULT_CAPACITY;
    default:
        return MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE;
    }
}

static MeisoRuntimeBlobResult blob_commit_result(MeisoBlobCommitStatus status)
{
    switch (status) {
    case MEISO_BLOB_COMMIT_PUBLISHED:
        return MEISO_RUNTIME_BLOB_RESULT_PUBLISHED;
    case MEISO_BLOB_COMMIT_DUPLICATE:
        return MEISO_RUNTIME_BLOB_RESULT_DUPLICATE;
    case MEISO_BLOB_COMMIT_CONFLICT:
        return MEISO_RUNTIME_BLOB_RESULT_CONFLICT;
    case MEISO_BLOB_COMMIT_LENGTH_MISMATCH:
        return MEISO_RUNTIME_BLOB_RESULT_LENGTH_MISMATCH;
    case MEISO_BLOB_COMMIT_INTEGRITY_FAILED:
        return MEISO_RUNTIME_BLOB_RESULT_INTEGRITY_FAILED;
    case MEISO_BLOB_COMMIT_CAPACITY:
        return MEISO_RUNTIME_BLOB_RESULT_CAPACITY;
    default:
        return MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE;
    }
}

typedef struct {
    MeisoBlobVerifier verifier;
    void *context;
} RuntimeBlobVerifierContext;

static bool runtime_blob_combined_verifier(
    const MeisoContentId *expected_content_id,
    MeisoByteView content,
    void *context
)
{
    RuntimeBlobVerifierContext *combined = context;

    return combined != NULL && combined->verifier != NULL &&
        meiso_runtime_blob_sha256_verify(
            expected_content_id, content, NULL
        ) && combined->verifier(
            expected_content_id, content, combined->context
        );
}

MeisoRuntimeStatus meiso_runtime_publish_blob_receive(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *expected_transfer,
    MeisoBlobStore *store,
    MeisoBlobVerifier verifier,
    void *verifier_context,
    MeisoRuntimeBlobPublishReport *out_report
)
{
    MeisoRuntimeStatus status = check_bound_mutable_internal(runtime, true);
    MeisoRuntimeBlobPublishReport report;
    RuntimeBlobVerifierContext combined;
    MeisoMutableBuffer staging;
    MeisoBlobStageStatus stage_status;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->pending_desired_receive || runtime->pending_actual_receive) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V3) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (expected_transfer == NULL || store == NULL || verifier == NULL ||
        out_report == NULL || ranges_overlap(
            runtime,
            runtime->arena_size,
            expected_transfer,
            sizeof(*expected_transfer)
        ) || output_overlaps_runtime_or_session(
            runtime, out_report, sizeof(*out_report)
        ) || ranges_overlap(
            expected_transfer,
            sizeof(*expected_transfer),
            out_report,
            sizeof(*out_report)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->blob_transfer_active || !runtime->blob_publish_pending) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    if (runtime->blob_result_pending ||
        !blob_transfer_equal(expected_transfer, &runtime->blob_transfer)) {
        return MEISO_RUNTIME_CONFLICT;
    }
    memset(&report, 0, sizeof(report));
    runtime->blob_bridge_active = true;
    report.stage_attempted = true;
    stage_status = meiso_blob_store_stage_begin(
        store,
        &runtime->blob_transfer.content_id,
        (size_t)runtime->blob_transfer.total_size
    );
    report.stage_status = stage_status;
    if (stage_status != MEISO_BLOB_STAGE_STARTED) {
        report.result = blob_stage_result(stage_status);
    } else {
        if (!meiso_blob_store_staging_buffer(store, &staging) ||
            staging.capacity != (size_t)runtime->blob_transfer.total_size) {
            report.commit_status = MEISO_BLOB_COMMIT_INVALID;
            report.result = MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE;
        } else {
            if (staging.capacity != 0U) {
                memmove(
                    staging.data,
                    blob_scratch(runtime),
                    staging.capacity
                );
            }
            report.commit_attempted = true;
            combined.verifier = verifier;
            combined.context = verifier_context;
            report.commit_status = meiso_blob_store_commit_verified(
                store,
                staging.capacity,
                runtime_blob_combined_verifier,
                &combined
            );
            report.result = blob_commit_result(report.commit_status);
        }
        if (report.result != MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
            report.result != MEISO_RUNTIME_BLOB_RESULT_DUPLICATE) {
            report.forced_abort_attempted = true;
            report.forced_abort_status = meiso_blob_store_abort_staging(
                store, MEISO_BLOB_ABORT_TRANSFER_DISCONTINUITY
            );
            if (report.forced_abort_status != MEISO_BLOB_ABORTED &&
                report.forced_abort_status != MEISO_BLOB_ABORT_NO_STAGING) {
                report.result =
                    MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE;
            }
        }
    }
    runtime->blob_bridge_active = false;
    runtime->blob_publish_pending = false;
    set_blob_result(
        runtime,
        &runtime->blob_transfer,
        MEISO_RUNTIME_BLOB_COMMIT,
        report.result,
        runtime->blob_next_offset
    );
    *out_report = report;
    return MEISO_RUNTIME_OK;
}

static void clear_pending_blob_result(MeisoRuntime *runtime)
{
    memset(
        &runtime->blob_result_transfer,
        0,
        sizeof(runtime->blob_result_transfer)
    );
    runtime->blob_result_next_offset = 0U;
    runtime->blob_pending_operation = 0U;
    runtime->blob_pending_result = 0U;
    runtime->blob_result_pending = false;
}

MeisoRuntimeStatus meiso_runtime_send_blob_result(
    MeisoRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *expected_transfer,
    MeisoRuntimeBlobOperation expected_operation
)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);
    uint8_t *body;
    bool result_for_active;
    MeisoRuntimeBlobResult result;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->pending_desired_receive || runtime->pending_actual_receive) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V3) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (expected_transfer == NULL ||
        !blob_operation_valid(expected_operation) || ranges_overlap(
            runtime,
            runtime->arena_size,
            expected_transfer,
            sizeof(*expected_transfer)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->blob_result_pending) {
        return MEISO_RUNTIME_BLOB_NOT_ACTIVE;
    }
    if (expected_operation != runtime->blob_pending_operation ||
        !blob_result_reference_equal(
            expected_transfer, &runtime->blob_result_transfer
        )) {
        return MEISO_RUNTIME_CONFLICT;
    }
    status = blob_tx_preflight(runtime);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    body = tx_bytes(runtime) + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    encode_route_key(body, runtime->blob_result_transfer.route);
    put_u64(body + 56U, runtime->blob_result_transfer.transfer_id);
    body[64U] = runtime->blob_pending_operation;
    body[65U] = runtime->blob_pending_result;
    memset(body + 66U, 0, 2U);
    put_u64(body + 68U, runtime->blob_result_next_offset);
    put_u64(body + 76U, runtime->blob_result_transfer.total_size);
    encode_header(
        runtime,
        tx_bytes(runtime),
        WIRE_BLOB_RESULT,
        MEISO_RUNTIME_V3_BLOB_RESULT_WIRE_SIZE
    );
    status = submit_bytes(runtime, MEISO_RUNTIME_V3_BLOB_RESULT_WIRE_SIZE);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    result = runtime->blob_pending_result;
    result_for_active = runtime->blob_transfer_active &&
        blob_transfer_equal(
            &runtime->blob_result_transfer, &runtime->blob_transfer
        );
    if (result == MEISO_RUNTIME_BLOB_RESULT_READY) {
        if (result_for_active) {
            runtime->blob_begin_ready = true;
        }
        clear_pending_blob_result(runtime);
    } else if (result_for_active) {
        clear_blob_transfer(runtime);
    } else {
        clear_pending_blob_result(runtime);
    }
    return MEISO_RUNTIME_OK;
}

static bool production_snapshot_equal(
    const MeisoRuntimeProductionSnapshot *lhs,
    const MeisoRuntimeProductionSnapshot *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        lhs->actual_present == rhs->actual_present &&
        desired_reference_equal(
            lhs->observed_desired, rhs->observed_desired
        ) &&
        desired_reference_equal(lhs->applied_desired, rhs->applied_desired) &&
        lhs->apply_result == rhs->apply_result &&
        lhs->availability == rhs->availability &&
        lhs->effective_enabled == rhs->effective_enabled &&
        lhs->production_status == rhs->production_status &&
        lhs->reason == rhs->reason &&
        lhs->production_epoch == rhs->production_epoch &&
        lhs->production_epoch_high_water ==
            rhs->production_epoch_high_water &&
        lhs->data_active == rhs->data_active;
}

MeisoRuntimeStatus meiso_runtime_commit_state_receive(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey route_key,
    const MeisoStateOffer *offer,
    MeisoStateReceiveStatus receive_status
)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);
    RouteSlot *route;
    EndpointSlot *endpoint;
    MeisoRuntimeActualState actual;
    MeisoRuntimeDesiredState desired;
    MeisoRuntimeProductionSnapshot candidate;
    size_t endpoint_index;
    bool pending_desired;
    bool pending_actual;
    bool matches;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    pending_desired = runtime->pending_desired_receive;
    pending_actual = runtime->pending_actual_receive;
    if ((!pending_desired && !pending_actual) || offer == NULL ||
        receive_status > MEISO_STATE_RECEIVE_INVALID) {
        clear_pending_state_receive(runtime);
        return MEISO_RUNTIME_CONFLICT;
    }
    status = resolve_active_route(runtime, route_key, &route, &endpoint);
    if (status != MEISO_RUNTIME_OK) {
        clear_pending_state_receive(runtime);
        return status;
    }
    matches = route_key_equal(
            route_key, runtime->pending_actual_route
        ) && offer->generation.value ==
            runtime->pending_actual_delivery.value &&
        meiso_owner_scope_equal(
            offer->state.owner, runtime->pending_actual_owner
        ) && offer->state.version == runtime->pending_actual_version;
    if (!matches || route->value.resource.kind != MEISO_RESOURCE_KIND_STATE ||
        !state_offer_valid(offer, &route->value.resource)) {
        clear_pending_state_receive(runtime);
        return !matches ? MEISO_RUNTIME_CONFLICT :
            MEISO_RUNTIME_STATE_INVALID;
    }
    endpoint_index = (size_t)(
        endpoint - endpoint_slots(runtime, runtime->active_bank)
    );
    if (pending_desired) {
        matches = offer->state.payload.size ==
                runtime->pending_desired_payload_size &&
            (offer->state.payload.size == 0U || memcmp(
                offer->state.payload.data,
                pending_desired_payload_bytes(runtime),
                offer->state.payload.size
            ) == 0);
        if (!matches) {
            clear_pending_state_receive(runtime);
            return MEISO_RUNTIME_CONFLICT;
        }
        if (pending_actual ||
            route->value.facet != MEISO_RUNTIME_FACET_DESIRED ||
            !meiso_runtime_desired_state_decode(
                offer->state.payload, &desired
            )) {
            clear_pending_state_receive(runtime);
            return MEISO_RUNTIME_STATE_INVALID;
        }
        clear_pending_state_receive(runtime);
        if (receive_status == MEISO_STATE_RECEIVE_NEW) {
            endpoint->desired.present = true;
            endpoint->desired.owner = offer->state.owner;
            endpoint->desired.version = offer->state.version;
        }
        return MEISO_RUNTIME_OK;
    }
    if (route->value.facet != MEISO_RUNTIME_FACET_ACTUAL ||
        !meiso_runtime_actual_state_decode(offer->state.payload, &actual) ||
        !actual_state_for_route_valid(&actual)) {
        clear_pending_state_receive(runtime);
        return MEISO_RUNTIME_STATE_INVALID;
    }
    production_snapshot_from_actual(
        &candidate,
        &actual,
        endpoint->production.production_epoch_high_water
    );
    matches = production_snapshot_equal(
            &candidate,
            &runtime->pending_actual_value
        ) && actual.profile.size == runtime->pending_actual_profile_size &&
        (actual.profile.size == 0U || memcmp(
            actual.profile.data,
            pending_actual_profile_bytes(runtime),
            actual.profile.size
        ) == 0);
    clear_pending_state_receive(runtime);
    if (!matches) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (receive_status == MEISO_STATE_RECEIVE_NEW) {
        if (!production_transition_valid(
                runtime, runtime->active_bank, endpoint_index, &actual
            )) {
            return MEISO_RUNTIME_STATE_INVALID;
        }
        apply_production(
            runtime, runtime->active_bank, endpoint_index, &actual
        );
    }
    return MEISO_RUNTIME_OK;
}

bool meiso_runtime_snapshot(
    const MeisoRuntime *runtime,
    MeisoRuntimeSnapshot *out_snapshot
)
{
    MeisoRuntimeStatus status;
    const uint8_t staged = runtime != NULL && runtime_layout_valid(runtime) ?
        staging_bank(runtime) : 0U;

    if (runtime == NULL || out_snapshot == NULL ||
        !runtime_layout_valid(runtime) ||
        output_overlaps_runtime_or_session(
            runtime, out_snapshot, sizeof(*out_snapshot)
        )) {
        return false;
    }
    status = check_bound_const(runtime);
    if (status == MEISO_RUNTIME_CONFLICT) {
        return false;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->session_id = runtime->session_id;
    out_snapshot->local_role = runtime->local_role;
    out_snapshot->negotiated_limits = runtime->limits;
    out_snapshot->highest_catalog_generation =
        runtime->highest_catalog_generation;
    out_snapshot->pending_catalog_generation =
        runtime->pending_catalog_generation;
    out_snapshot->active_catalog_present = runtime->active_present;
    out_snapshot->staged_catalog_present = runtime->staged_present;
    out_snapshot->catalog_decision_pending =
        runtime->catalog_decision_pending;
    out_snapshot->core_ready = status == MEISO_RUNTIME_OK;
    if (runtime->active_present) {
        out_snapshot->active_catalog_generation =
            runtime->generation[runtime->active_bank];
        out_snapshot->active_endpoint_count =
            runtime->endpoint_count[runtime->active_bank];
        out_snapshot->active_route_count =
            runtime->route_count[runtime->active_bank];
    }
    if (runtime->staged_present) {
        out_snapshot->staged_catalog_generation = runtime->generation[staged];
        out_snapshot->staged_endpoint_count = runtime->endpoint_count[staged];
        out_snapshot->staged_route_count = runtime->route_count[staged];
    }
    return true;
}

bool meiso_runtime_blob_snapshot(
    const MeisoRuntime *runtime,
    MeisoRuntimeBlobSnapshot *out_snapshot
)
{
    MeisoRuntimeStatus status;

    if (runtime == NULL || out_snapshot == NULL ||
        !runtime_layout_valid(runtime) || output_overlaps_runtime_or_session(
            runtime, out_snapshot, sizeof(*out_snapshot)
        )) {
        return false;
    }
    status = check_bound_const(runtime);
    if (status != MEISO_RUNTIME_OK &&
        status != MEISO_RUNTIME_CORE_NOT_READY &&
        status != MEISO_RUNTIME_STALE_SESSION) {
        return false;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->wire_version = runtime->wire_version;
    out_snapshot->max_transfer_bytes = runtime->max_blob_transfer_bytes;
    out_snapshot->transfer_id_high_water =
        runtime->blob_transfer_id_high_water;
    out_snapshot->transfer_active = runtime->blob_transfer_active;
    out_snapshot->begin_ready = runtime->blob_begin_ready;
    out_snapshot->publish_pending = runtime->blob_publish_pending;
    out_snapshot->result_pending = runtime->blob_result_pending;
    out_snapshot->core_ready = status == MEISO_RUNTIME_OK;
    out_snapshot->next_offset = runtime->blob_result_pending ?
        runtime->blob_result_next_offset : runtime->blob_next_offset;
    out_snapshot->pending_operation = runtime->blob_pending_operation;
    out_snapshot->pending_result = runtime->blob_pending_result;
    if (runtime->blob_result_pending) {
        out_snapshot->transfer = runtime->blob_result_transfer;
    } else if (runtime->blob_transfer_active) {
        out_snapshot->transfer = runtime->blob_transfer;
    }
    return true;
}

static void copy_endpoint_out(
    const MeisoRuntime *runtime,
    uint8_t bank,
    size_t index,
    MeisoRuntimeEndpointDescriptor *out_endpoint
)
{
    const EndpointSlot *slot = &endpoint_slots_const(runtime, bank)[index];

    out_endpoint->id = slot->id;
    out_endpoint->incarnation = slot->incarnation;
    out_endpoint->capability_schema = slot->capability_schema;
    out_endpoint->capability.data = capability_bytes_const(runtime, bank) +
        slot->capability_offset;
    out_endpoint->capability.size = slot->capability_size;
}

MeisoRuntimeStatus meiso_runtime_endpoint_at(
    const MeisoRuntime *runtime,
    size_t index,
    MeisoRuntimeEndpointDescriptor *out_endpoint
)
{
    MeisoRuntimeStatus status = check_bound_const(runtime);

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (out_endpoint == NULL || output_overlaps_runtime_or_session(
            runtime, out_endpoint, sizeof(*out_endpoint)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    if (index >= runtime->endpoint_count[runtime->active_bank]) {
        return MEISO_RUNTIME_UNKNOWN_ENDPOINT;
    }
    copy_endpoint_out(runtime, runtime->active_bank, index, out_endpoint);
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_route_at(
    const MeisoRuntime *runtime,
    size_t index,
    MeisoRuntimeRouteDescriptor *out_route
)
{
    MeisoRuntimeStatus status = check_bound_const(runtime);

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (out_route == NULL || output_overlaps_runtime_or_session(
            runtime, out_route, sizeof(*out_route)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    if (index >= runtime->route_count[runtime->active_bank]) {
        return MEISO_RUNTIME_UNKNOWN_RESOURCE;
    }
    *out_route = route_slots_const(runtime, runtime->active_bank)[index].value;
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_staged_endpoint_at(
    const MeisoRuntime *runtime,
    size_t index,
    MeisoRuntimeEndpointDescriptor *out_endpoint
)
{
    MeisoRuntimeStatus status = check_bound_const(runtime);
    uint8_t bank;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (out_endpoint == NULL || output_overlaps_runtime_or_session(
            runtime, out_endpoint, sizeof(*out_endpoint)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->staged_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    bank = staging_bank(runtime);
    if (index >= runtime->endpoint_count[bank]) {
        return MEISO_RUNTIME_UNKNOWN_ENDPOINT;
    }
    copy_endpoint_out(runtime, bank, index, out_endpoint);
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_staged_route_at(
    const MeisoRuntime *runtime,
    size_t index,
    MeisoRuntimeRouteDescriptor *out_route
)
{
    MeisoRuntimeStatus status = check_bound_const(runtime);
    uint8_t bank;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (out_route == NULL || output_overlaps_runtime_or_session(
            runtime, out_route, sizeof(*out_route)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->staged_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    bank = staging_bank(runtime);
    if (index >= runtime->route_count[bank]) {
        return MEISO_RUNTIME_UNKNOWN_RESOURCE;
    }
    *out_route = route_slots_const(runtime, bank)[index].value;
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_find_endpoint(
    const MeisoRuntime *runtime,
    MeisoEndpointId endpoint_id,
    MeisoRuntimeEndpointDescriptor *out_endpoint
)
{
    MeisoRuntimeStatus status = check_bound_const(runtime);
    int index;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (out_endpoint == NULL || !meiso_endpoint_id_valid(endpoint_id) ||
        output_overlaps_runtime_or_session(
            runtime, out_endpoint, sizeof(*out_endpoint)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    index = endpoint_index_in_bank(
        runtime, runtime->active_bank, endpoint_id
    );
    if (index < 0) {
        return MEISO_RUNTIME_UNKNOWN_ENDPOINT;
    }
    copy_endpoint_out(
        runtime, runtime->active_bank, (size_t)index, out_endpoint
    );
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_find_route(
    const MeisoRuntime *runtime,
    MeisoResourceId resource_id,
    MeisoRuntimeRouteDescriptor *out_route
)
{
    MeisoRuntimeStatus status = check_bound_const(runtime);
    int index;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (out_route == NULL || !meiso_resource_id_valid(resource_id) ||
        output_overlaps_runtime_or_session(
            runtime, out_route, sizeof(*out_route)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    index = route_index_in_bank(
        runtime, runtime->active_bank, resource_id
    );
    if (index < 0) {
        return MEISO_RUNTIME_UNKNOWN_RESOURCE;
    }
    *out_route = route_slots_const(runtime, runtime->active_bank)
        [(size_t)index].value;
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_production_snapshot(
    const MeisoRuntime *runtime,
    MeisoEndpointId endpoint_id,
    MeisoRuntimeProductionSnapshot *out_snapshot
)
{
    MeisoRuntimeStatus status = check_bound_const(runtime);
    int index;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (out_snapshot == NULL || !meiso_endpoint_id_valid(endpoint_id) ||
        output_overlaps_runtime_or_session(
            runtime, out_snapshot, sizeof(*out_snapshot)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    index = endpoint_index_in_bank(
        runtime, runtime->active_bank, endpoint_id
    );
    if (index < 0) {
        return MEISO_RUNTIME_UNKNOWN_ENDPOINT;
    }
    *out_snapshot = endpoint_slots_const(runtime, runtime->active_bank)
        [(size_t)index].production;
    out_snapshot->effective_profile.data = actual_profile_bytes_const(
        runtime, runtime->active_bank, (size_t)index
    );
    out_snapshot->effective_profile.size =
        endpoint_slots_const(runtime, runtime->active_bank)
            [(size_t)index].actual_profile_size;
    return MEISO_RUNTIME_OK;
}
