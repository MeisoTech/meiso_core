#include "meiso_runtime.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MEISO_RUNTIME_MAGIC UINT64_C(0x4D4549534F52544D)

enum {
    WIRE_CATALOG = 1,
    WIRE_CATALOG_ACCEPT = 2,
    WIRE_CATALOG_REJECT = 3,
    WIRE_SUBSCRIPTION_DESIRED = 4,
    WIRE_SUBSCRIPTION_ACTUAL = 5,
    WIRE_RESOURCE = 6
};

enum {
    CATALOG_PREFIX_SIZE = 16,
    ENDPOINT_RECORD_SIZE = 48,
    ROUTE_RECORD_SIZE = 96,
    CATALOG_ACCEPT_SIZE = 8,
    CATALOG_REJECT_SIZE = 12,
    DESIRED_PREFIX_SIZE = 64,
    ACTUAL_PREFIX_SIZE = 72,
    RESOURCE_PREFIX_SIZE = 92
};

typedef struct {
    MeisoEndpointId id;
    uint64_t incarnation;
    MeisoSchemaRef capability_schema;
    uint32_t capability_offset;
    uint32_t capability_size;
} EndpointSlot;

typedef struct {
    MeisoRuntimeRouteDescriptor value;
} RouteSlot;

typedef struct {
    uint64_t request_id;
    uint64_t route_epoch;
    uint32_t desired_size;
    uint32_t effective_size;
    MeisoRuntimeActualStatus actual_status;
    MeisoRuntimeActualReason actual_reason;
    bool desired_present;
    bool enabled;
} SubscriptionSlot;

struct MeisoRuntime {
    uint64_t magic;
    MeisoSession *session;
    MeisoSessionId session_id;
    MeisoSessionLimits limits;
    size_t arena_size;
    size_t endpoint_offset[2];
    size_t route_offset[2];
    size_t capability_offset[2];
    size_t subscription_offset;
    size_t desired_offset;
    size_t effective_offset;
    size_t tx_offset;
    uint64_t highest_catalog_generation;
    uint64_t pending_catalog_generation;
    uint64_t generation[2];
    uint32_t endpoint_count[2];
    uint32_t route_count[2];
    uint32_t capability_used[2];
    uint32_t max_endpoints;
    uint32_t max_routes;
    uint32_t max_capability_bytes;
    uint32_t max_parameter_bytes_per_route;
    MeisoSessionRole local_role;
    uint8_t active_bank;
    bool active_present;
    bool staged_present;
    bool catalog_decision_pending;
    bool invalidated;
    uint8_t storage[];
};

typedef struct {
    size_t endpoint_offset[2];
    size_t route_offset[2];
    size_t capability_offset[2];
    size_t subscription_offset;
    size_t desired_offset;
    size_t effective_offset;
    size_t tx_offset;
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

    if (alignment == 0U) {
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

static bool output_overlaps_runtime_or_session(
    const MeisoRuntime *runtime,
    const void *output,
    size_t output_size
)
{
    return ranges_overlap(runtime, runtime->arena_size, output, output_size) ||
        ranges_overlap(runtime->session, 1U, output, output_size);
}

static bool schema_valid(MeisoSchemaRef value)
{
    return meiso_schema_id_valid(value.id) && value.version != 0U;
}

static bool schema_zero(MeisoSchemaRef value)
{
    return value.id.high == 0U && value.id.low == 0U && value.version == 0U;
}

static bool schema_equal(MeisoSchemaRef lhs, MeisoSchemaRef rhs)
{
    return meiso_schema_id_equal(lhs.id, rhs.id) &&
        lhs.version == rhs.version;
}

static bool delivery_valid(MeisoRuntimeDelivery value)
{
    return value == MEISO_RUNTIME_DELIVERY_ANNOUNCED ||
        value == MEISO_RUNTIME_DELIVERY_HOST_SUBSCRIPTION;
}

static bool actual_status_valid(MeisoRuntimeActualStatus value)
{
    return value >= MEISO_RUNTIME_ACTUAL_APPLYING &&
        value <= MEISO_RUNTIME_ACTUAL_DISABLED;
}

static bool actual_reason_valid(MeisoRuntimeActualReason value)
{
    return value <= MEISO_RUNTIME_ACTUAL_REASON_APPLY_FAILED;
}

static bool actual_status_reason_valid(
    MeisoRuntimeActualStatus status,
    MeisoRuntimeActualReason reason
)
{
    if (!actual_status_valid(status) || !actual_reason_valid(reason)) {
        return false;
    }
    if (status == MEISO_RUNTIME_ACTUAL_REJECTED) {
        return reason == MEISO_RUNTIME_ACTUAL_REASON_UNSUPPORTED_PARAMETER ||
            reason == MEISO_RUNTIME_ACTUAL_REASON_ADMISSION_DENIED ||
            reason == MEISO_RUNTIME_ACTUAL_REASON_APPLY_FAILED;
    }
    if (status == MEISO_RUNTIME_ACTUAL_UNAVAILABLE) {
        return reason == MEISO_RUNTIME_ACTUAL_REASON_UNAVAILABLE;
    }
    return reason == MEISO_RUNTIME_ACTUAL_REASON_NONE;
}

static bool reject_reason_valid(MeisoRuntimeCatalogRejectReason value)
{
    return value >= MEISO_RUNTIME_CATALOG_REJECT_UNSUPPORTED &&
        value <= MEISO_RUNTIME_CATALOG_REJECT_CAPACITY;
}

static bool calculate_layout(
    uint32_t max_endpoints,
    uint32_t max_routes,
    uint32_t max_capability_bytes,
    uint32_t max_parameter_bytes,
    uint32_t max_runtime_payload_bytes,
    RuntimeLayout *out
)
{
    RuntimeLayout value;
    size_t cursor = sizeof(MeisoRuntime);
    size_t parameter_total;
    unsigned int bank;

    if (out == NULL ||
        !checked_multiply(max_routes, max_parameter_bytes, &parameter_total)) {
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
            )) {
            return false;
        }
    }
    if (!add_aligned_array(
            &cursor, _Alignof(SubscriptionSlot), max_routes,
            sizeof(SubscriptionSlot), &value.subscription_offset
        ) ||
        !add_aligned_array(
            &cursor, 1U, parameter_total, 1U, &value.desired_offset
        ) ||
        !add_aligned_array(
            &cursor, 1U, parameter_total, 1U, &value.effective_offset
        ) ||
        !add_aligned_array(
            &cursor, 1U, max_runtime_payload_bytes, 1U, &value.tx_offset
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

static bool config_layout(
    const MeisoRuntimeConfig *config,
    MeisoSessionSnapshot *out_snapshot,
    RuntimeLayout *out_layout
)
{
    uint64_t entry_capacity;

    if (config == NULL || out_snapshot == NULL || out_layout == NULL ||
        !snapshot_ready(config->session, out_snapshot)) {
        return false;
    }
    entry_capacity = (uint64_t)config->max_endpoints + config->max_routes;
    return out_snapshot->negotiated_limits.max_runtime_payload_bytes >=
            MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_PREFIX_SIZE &&
        entry_capacity <= out_snapshot->negotiated_limits.max_registry_entries &&
        config->max_capability_bytes <=
            out_snapshot->negotiated_limits.max_runtime_payload_bytes &&
        config->max_parameter_bytes_per_route <=
            out_snapshot->negotiated_limits.max_runtime_payload_bytes &&
        calculate_layout(
            config->max_endpoints, config->max_routes,
            config->max_capability_bytes,
            config->max_parameter_bytes_per_route,
            out_snapshot->negotiated_limits.max_runtime_payload_bytes,
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

static SubscriptionSlot *subscription_slots(MeisoRuntime *runtime)
{
    return (SubscriptionSlot *)(void *)((uint8_t *)(void *)runtime +
                                        runtime->subscription_offset);
}

static const SubscriptionSlot *subscription_slots_const(
    const MeisoRuntime *runtime
)
{
    return (const SubscriptionSlot *)(const void *)(
        (const uint8_t *)(const void *)runtime + runtime->subscription_offset
    );
}

static uint8_t *desired_bytes(MeisoRuntime *runtime, size_t route_index)
{
    return (uint8_t *)(void *)runtime + runtime->desired_offset +
        route_index * runtime->max_parameter_bytes_per_route;
}

static const uint8_t *desired_bytes_const(
    const MeisoRuntime *runtime,
    size_t route_index
)
{
    return (const uint8_t *)(const void *)runtime + runtime->desired_offset +
        route_index * runtime->max_parameter_bytes_per_route;
}

static uint8_t *effective_bytes(MeisoRuntime *runtime, size_t route_index)
{
    return (uint8_t *)(void *)runtime + runtime->effective_offset +
        route_index * runtime->max_parameter_bytes_per_route;
}

static const uint8_t *effective_bytes_const(
    const MeisoRuntime *runtime,
    size_t route_index
)
{
    return (const uint8_t *)(const void *)runtime + runtime->effective_offset +
        route_index * runtime->max_parameter_bytes_per_route;
}

static uint8_t *tx_bytes(MeisoRuntime *runtime)
{
    return (uint8_t *)(void *)runtime + runtime->tx_offset;
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
            runtime->max_endpoints, runtime->max_routes,
            runtime->max_capability_bytes,
            runtime->max_parameter_bytes_per_route,
            runtime->limits.max_runtime_payload_bytes, &layout
        ) && runtime->arena_size == layout.required &&
        runtime->endpoint_offset[0] == layout.endpoint_offset[0] &&
        runtime->endpoint_offset[1] == layout.endpoint_offset[1] &&
        runtime->route_offset[0] == layout.route_offset[0] &&
        runtime->route_offset[1] == layout.route_offset[1] &&
        runtime->capability_offset[0] == layout.capability_offset[0] &&
        runtime->capability_offset[1] == layout.capability_offset[1] &&
        runtime->subscription_offset == layout.subscription_offset &&
        runtime->desired_offset == layout.desired_offset &&
        runtime->effective_offset == layout.effective_offset &&
        runtime->tx_offset == layout.tx_offset &&
        runtime->endpoint_count[0] <= runtime->max_endpoints &&
        runtime->endpoint_count[1] <= runtime->max_endpoints &&
        runtime->route_count[0] <= runtime->max_routes &&
        runtime->route_count[1] <= runtime->max_routes &&
        runtime->capability_used[0] <= runtime->max_capability_bytes &&
        runtime->capability_used[1] <= runtime->max_capability_bytes &&
        (runtime->local_role == MEISO_SESSION_ROLE_HOST ||
         runtime->local_role == MEISO_SESSION_ROLE_EDGE);
}

static void invalidate_runtime(MeisoRuntime *runtime)
{
    runtime->invalidated = true;
    runtime->active_present = false;
    runtime->staged_present = false;
    runtime->catalog_decision_pending = false;
    runtime->pending_catalog_generation = 0U;
    memset(subscription_slots(runtime), 0,
           (size_t)runtime->max_routes * sizeof(SubscriptionSlot));
}

static MeisoRuntimeStatus check_bound_mutable(MeisoRuntime *runtime)
{
    MeisoSessionSnapshot snapshot;

    if (!runtime_layout_valid(runtime)) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (runtime->invalidated) {
        return MEISO_RUNTIME_STALE_SESSION;
    }
    if (!meiso_session_snapshot(runtime->session, &snapshot) ||
        snapshot.state != MEISO_SESSION_STATE_READY ||
        !snapshot.session_id_known) {
        invalidate_runtime(runtime);
        return MEISO_RUNTIME_CORE_NOT_READY;
    }
    if (snapshot.local_role != runtime->local_role ||
        !meiso_session_id_equal(snapshot.session_id, runtime->session_id) ||
        memcmp(&snapshot.negotiated_limits, &runtime->limits,
               sizeof(runtime->limits)) != 0) {
        invalidate_runtime(runtime);
        return MEISO_RUNTIME_STALE_SESSION;
    }
    return MEISO_RUNTIME_OK;
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
    if (!meiso_session_snapshot(runtime->session, &snapshot) ||
        snapshot.state != MEISO_SESSION_STATE_READY ||
        !snapshot.session_id_known) {
        return MEISO_RUNTIME_CORE_NOT_READY;
    }
    return snapshot.local_role == runtime->local_role &&
        meiso_session_id_equal(snapshot.session_id, runtime->session_id) &&
        memcmp(&snapshot.negotiated_limits, &runtime->limits,
               sizeof(runtime->limits)) == 0 ?
        MEISO_RUNTIME_OK : MEISO_RUNTIME_STALE_SESSION;
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

static void encode_header(uint8_t *target, uint8_t type, uint32_t size)
{
    target[0] = (uint8_t)'M';
    target[1] = (uint8_t)'R';
    target[2] = (uint8_t)'T';
    target[3] = (uint8_t)'0';
    target[4] = (uint8_t)MEISO_RUNTIME_WIRE_VERSION;
    target[5] = type;
    target[6] = 0U;
    target[7] = 0U;
    put_u32(target + 8U, size);
}

static bool decode_header(MeisoByteView frame, uint8_t *out_type)
{
    if (out_type == NULL || frame.data == NULL ||
        frame.size < MEISO_RUNTIME_WIRE_HEADER_SIZE ||
        frame.size > UINT32_MAX || frame.data[0] != (uint8_t)'M' ||
        frame.data[1] != (uint8_t)'R' || frame.data[2] != (uint8_t)'T' ||
        frame.data[3] != (uint8_t)'0' ||
        frame.data[4] != (uint8_t)MEISO_RUNTIME_WIRE_VERSION ||
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

bool meiso_endpoint_id_valid(MeisoEndpointId value)
{
    return value.high != 0U || value.low != 0U;
}

bool meiso_endpoint_id_equal(MeisoEndpointId lhs, MeisoEndpointId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
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

MeisoRuntimeInitStatus meiso_runtime_init(
    void *arena,
    size_t arena_size,
    const MeisoRuntimeConfig *config,
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
        (config != NULL && ranges_overlap(
            config, sizeof(*config), out_runtime, sizeof(*out_runtime)
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
    if (!config_layout(config, &snapshot, &layout)) {
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
    runtime->subscription_offset = layout.subscription_offset;
    runtime->desired_offset = layout.desired_offset;
    runtime->effective_offset = layout.effective_offset;
    runtime->tx_offset = layout.tx_offset;
    runtime->max_endpoints = config_copy.max_endpoints;
    runtime->max_routes = config_copy.max_routes;
    runtime->max_capability_bytes = config_copy.max_capability_bytes;
    runtime->max_parameter_bytes_per_route =
        config_copy.max_parameter_bytes_per_route;
    runtime->local_role = snapshot.local_role;
    *out_runtime = runtime;
    return MEISO_RUNTIME_INIT_OK;
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
                slots[index].value.resource.id, resource_id
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
    uint8_t bank,
    const MeisoRuntimeRouteDescriptor *route
)
{
    int endpoint_index;
    const EndpointSlot *endpoints;

    if (route == NULL || !meiso_endpoint_id_valid(route->endpoint_id) ||
        route->endpoint_incarnation == 0U ||
        meiso_resource_descriptor_validate(&route->resource) !=
            MEISO_DESCRIPTOR_VALID ||
        !delivery_valid(route->delivery)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    if (runtime->limits.max_runtime_payload_bytes <
            MEISO_RUNTIME_WIRE_HEADER_SIZE + RESOURCE_PREFIX_SIZE ||
        route->resource.max_payload_size >
            runtime->limits.max_runtime_payload_bytes -
                (MEISO_RUNTIME_WIRE_HEADER_SIZE + RESOURCE_PREFIX_SIZE)) {
        return MEISO_RUNTIME_CAPACITY;
    }
    endpoint_index = endpoint_index_in_bank(runtime, bank, route->endpoint_id);
    if (endpoint_index < 0) {
        return MEISO_RUNTIME_UNKNOWN_ENDPOINT;
    }
    endpoints = endpoint_slots_const(runtime, bank);
    if (endpoints[(size_t)endpoint_index].incarnation !=
        route->endpoint_incarnation) {
        return MEISO_RUNTIME_STALE_ENDPOINT;
    }
    if (route->delivery == MEISO_RUNTIME_DELIVERY_HOST_SUBSCRIPTION) {
        if (route->resource.kind != MEISO_RESOURCE_KIND_STREAM) {
            return MEISO_RUNTIME_UNSUPPORTED_KIND;
        }
        if (route->resource.owner != MEISO_OWNER_ROLE_EDGE) {
            return MEISO_RUNTIME_OWNER_MISMATCH;
        }
        if (!schema_valid(route->selection_schema)) {
            return MEISO_RUNTIME_SCHEMA_MISMATCH;
        }
        if (route->max_selection_bytes >
                runtime->max_parameter_bytes_per_route ||
            runtime->limits.max_runtime_payload_bytes <
                MEISO_RUNTIME_WIRE_HEADER_SIZE + ACTUAL_PREFIX_SIZE ||
            route->max_selection_bytes >
                runtime->limits.max_runtime_payload_bytes -
                    (MEISO_RUNTIME_WIRE_HEADER_SIZE + ACTUAL_PREFIX_SIZE)) {
            return MEISO_RUNTIME_CAPACITY;
        }
    } else if (!schema_zero(route->selection_schema) ||
               route->max_selection_bytes != 0U) {
        return MEISO_RUNTIME_MALFORMED;
    }
    return MEISO_RUNTIME_OK;
}

static void clear_catalog_bank(MeisoRuntime *runtime, uint8_t bank)
{
    runtime->generation[bank] = 0U;
    runtime->endpoint_count[bank] = 0U;
    runtime->route_count[bank] = 0U;
    runtime->capability_used[bank] = 0U;
}

static void clear_catalog_decision(MeisoRuntime *runtime)
{
    clear_catalog_bank(runtime, staging_bank(runtime));
    runtime->staged_present = false;
    runtime->catalog_decision_pending = false;
    runtime->pending_catalog_generation = 0U;
}

static void clear_subscriptions(MeisoRuntime *runtime)
{
    size_t parameter_total;

    memset(subscription_slots(runtime), 0,
           (size_t)runtime->max_routes * sizeof(SubscriptionSlot));
    parameter_total = (size_t)runtime->max_routes *
        runtime->max_parameter_bytes_per_route;
    if (parameter_total != 0U) {
        memset((uint8_t *)(void *)runtime + runtime->desired_offset,
               0, parameter_total);
        memset((uint8_t *)(void *)runtime + runtime->effective_offset,
               0, parameter_total);
    }
}

static void commit_staged(MeisoRuntime *runtime)
{
    const uint8_t old_active = runtime->active_bank;

    runtime->active_bank = staging_bank(runtime);
    runtime->active_present = true;
    runtime->staged_present = false;
    runtime->catalog_decision_pending = false;
    runtime->pending_catalog_generation = 0U;
    clear_catalog_bank(runtime, old_active);
    clear_subscriptions(runtime);
}

static bool catalog_counts_fit(
    const MeisoRuntime *runtime,
    uint32_t endpoint_count,
    uint32_t route_count
)
{
    return endpoint_count <= runtime->max_endpoints &&
        route_count <= runtime->max_routes &&
        (uint64_t)endpoint_count + route_count <=
            runtime->limits.max_registry_entries;
}

static MeisoRuntimeStatus parse_catalog_wire(
    MeisoRuntime *runtime,
    MeisoByteView frame
)
{
    const uint8_t bank = staging_bank(runtime);
    EndpointSlot *endpoints = endpoint_slots(runtime, bank);
    RouteSlot *routes = route_slots(runtime, bank);
    uint8_t type;
    uint64_t generation;
    uint32_t endpoint_count;
    uint32_t route_count;
    uint32_t capability_used = 0U;
    size_t cursor;
    uint32_t index;

    if (!decode_header(frame, &type) || type != WIRE_CATALOG ||
        frame.size < MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_PREFIX_SIZE) {
        return MEISO_RUNTIME_MALFORMED;
    }
    generation = get_u64(frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE);
    endpoint_count = get_u32(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U
    );
    route_count = get_u32(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 12U
    );
    if (generation == 0U) {
        return MEISO_RUNTIME_MALFORMED;
    }
    if (!catalog_counts_fit(runtime, endpoint_count, route_count)) {
        return MEISO_RUNTIME_CAPACITY;
    }

    clear_catalog_bank(runtime, bank);
    runtime->generation[bank] = generation;
    cursor = MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_PREFIX_SIZE;
    for (index = 0U; index < endpoint_count; ++index) {
        EndpointSlot value;
        uint32_t previous;

        if (cursor > frame.size ||
            ENDPOINT_RECORD_SIZE > frame.size - cursor) {
            clear_catalog_bank(runtime, bank);
            return MEISO_RUNTIME_MALFORMED;
        }
        value.id = get_endpoint_id(frame.data + cursor);
        value.incarnation = get_u64(frame.data + cursor + 16U);
        value.capability_schema = get_schema(frame.data + cursor + 24U);
        value.capability_size = get_u32(frame.data + cursor + 44U);
        value.capability_offset = capability_used;
        cursor += ENDPOINT_RECORD_SIZE;
        if (!meiso_endpoint_id_valid(value.id) || value.incarnation == 0U ||
            !schema_valid(value.capability_schema) ||
            value.capability_size > frame.size - cursor) {
            clear_catalog_bank(runtime, bank);
            return MEISO_RUNTIME_MALFORMED;
        }
        if (value.capability_size > runtime->max_capability_bytes -
                capability_used) {
            clear_catalog_bank(runtime, bank);
            return MEISO_RUNTIME_CAPACITY;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (meiso_endpoint_id_equal(endpoints[previous].id, value.id)) {
                clear_catalog_bank(runtime, bank);
                return MEISO_RUNTIME_CONFLICT;
            }
        }
        if (value.capability_size != 0U) {
            memmove(capability_bytes(runtime, bank) + capability_used,
                    frame.data + cursor, value.capability_size);
        }
        endpoints[index] = value;
        capability_used += value.capability_size;
        cursor += value.capability_size;
        runtime->endpoint_count[bank] = index + 1U;
    }
    runtime->capability_used[bank] = capability_used;

    for (index = 0U; index < route_count; ++index) {
        MeisoRuntimeRouteDescriptor value;
        MeisoRuntimeStatus status;
        uint64_t max_payload;
        uint32_t previous;

        if (cursor > frame.size || ROUTE_RECORD_SIZE > frame.size - cursor) {
            clear_catalog_bank(runtime, bank);
            return MEISO_RUNTIME_MALFORMED;
        }
        memset(&value, 0, sizeof(value));
        value.endpoint_id = get_endpoint_id(frame.data + cursor);
        value.endpoint_incarnation = get_u64(frame.data + cursor + 16U);
        value.resource.id = get_resource_id(frame.data + cursor + 24U);
        value.resource.kind = frame.data[cursor + 40U];
        value.resource.owner = frame.data[cursor + 41U];
        value.delivery = frame.data[cursor + 42U];
        if (frame.data[cursor + 43U] != 0U) {
            clear_catalog_bank(runtime, bank);
            return MEISO_RUNTIME_MALFORMED;
        }
        value.resource.schema = get_schema(frame.data + cursor + 44U);
        value.selection_schema = get_schema(frame.data + cursor + 64U);
        max_payload = get_u64(frame.data + cursor + 84U);
        value.max_selection_bytes = get_u32(frame.data + cursor + 92U);
        if (max_payload > SIZE_MAX) {
            clear_catalog_bank(runtime, bank);
            return MEISO_RUNTIME_CAPACITY;
        }
        value.resource.max_payload_size = (size_t)max_payload;
        status = route_input_validate(runtime, bank, &value);
        if (status != MEISO_RUNTIME_OK) {
            clear_catalog_bank(runtime, bank);
            return status;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (meiso_resource_id_equal(
                    routes[previous].value.resource.id, value.resource.id
                )) {
                clear_catalog_bank(runtime, bank);
                return MEISO_RUNTIME_CONFLICT;
            }
        }
        routes[index].value = value;
        runtime->route_count[bank] = index + 1U;
        cursor += ROUTE_RECORD_SIZE;
    }
    if (cursor != frame.size) {
        clear_catalog_bank(runtime, bank);
        return MEISO_RUNTIME_MALFORMED;
    }
    runtime->staged_present = true;
    return MEISO_RUNTIME_OK;
}

static bool catalog_status_rejectable(MeisoRuntimeStatus status)
{
    return status == MEISO_RUNTIME_CAPACITY ||
        status == MEISO_RUNTIME_UNSUPPORTED_KIND ||
        status == MEISO_RUNTIME_OWNER_MISMATCH ||
        status == MEISO_RUNTIME_SCHEMA_MISMATCH;
}

static MeisoRuntimeStatus stage_catalog_wire(
    MeisoRuntime *runtime,
    MeisoByteView frame
)
{
    uint8_t type;
    uint64_t generation;
    MeisoRuntimeStatus status;

    if (runtime->catalog_decision_pending) {
        return MEISO_RUNTIME_CATALOG_PENDING;
    }
    if (!decode_header(frame, &type) || type != WIRE_CATALOG ||
        frame.size < MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_PREFIX_SIZE) {
        return MEISO_RUNTIME_MALFORMED;
    }
    generation = get_u64(frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE);
    if (generation == 0U) {
        return MEISO_RUNTIME_MALFORMED;
    }
    if (generation <= runtime->highest_catalog_generation) {
        return MEISO_RUNTIME_STALE_CATALOG;
    }

    clear_catalog_bank(runtime, staging_bank(runtime));
    status = parse_catalog_wire(runtime, frame);
    if (status != MEISO_RUNTIME_OK) {
        clear_catalog_bank(runtime, staging_bank(runtime));
        runtime->staged_present = false;
    }
    if (status == MEISO_RUNTIME_OK || catalog_status_rejectable(status)) {
        runtime->highest_catalog_generation = generation;
        runtime->pending_catalog_generation = generation;
        runtime->catalog_decision_pending = true;
    }
    return status;
}

static MeisoRuntimeStatus encode_catalog(
    MeisoRuntime *runtime,
    uint64_t generation,
    const MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_count,
    const MeisoRuntimeRouteDescriptor *routes,
    size_t route_count,
    size_t *out_size
)
{
    uint8_t *frame = tx_bytes(runtime);
    uint64_t capability_total = 0U;
    uint64_t total_size;
    size_t cursor;
    size_t index;

    if (out_size == NULL || generation == 0U ||
        (endpoint_count != 0U && endpoints == NULL) ||
        (route_count != 0U && routes == NULL)) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (endpoint_count > UINT32_MAX || route_count > UINT32_MAX ||
        !catalog_counts_fit(runtime, (uint32_t)endpoint_count,
                            (uint32_t)route_count)) {
        return MEISO_RUNTIME_CAPACITY;
    }
    if (generation <= runtime->highest_catalog_generation) {
        return MEISO_RUNTIME_STALE_CATALOG;
    }
    for (index = 0U; index < endpoint_count; ++index) {
        size_t previous;

        if (!endpoint_input_valid(&endpoints[index])) {
            return MEISO_RUNTIME_MALFORMED;
        }
        capability_total += endpoints[index].capability.size;
        if (capability_total > runtime->max_capability_bytes) {
            return MEISO_RUNTIME_CAPACITY;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (meiso_endpoint_id_equal(
                    endpoints[previous].id, endpoints[index].id
                )) {
                return MEISO_RUNTIME_CONFLICT;
            }
        }
    }
    total_size = MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_PREFIX_SIZE;
    total_size += (uint64_t)endpoint_count * ENDPOINT_RECORD_SIZE;
    total_size += capability_total;
    total_size += (uint64_t)route_count * ROUTE_RECORD_SIZE;
    if (total_size > runtime->limits.max_runtime_payload_bytes ||
        total_size > UINT32_MAX) {
        return MEISO_RUNTIME_CAPACITY;
    }

    encode_header(frame, WIRE_CATALOG, (uint32_t)total_size);
    put_u64(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE, generation);
    put_u32(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U,
            (uint32_t)endpoint_count);
    put_u32(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 12U,
            (uint32_t)route_count);
    cursor = MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_PREFIX_SIZE;
    for (index = 0U; index < endpoint_count; ++index) {
        put_endpoint_id(frame + cursor, endpoints[index].id);
        put_u64(frame + cursor + 16U, endpoints[index].incarnation);
        put_schema(frame + cursor + 24U,
                   endpoints[index].capability_schema);
        put_u32(frame + cursor + 44U,
                (uint32_t)endpoints[index].capability.size);
        cursor += ENDPOINT_RECORD_SIZE;
        if (endpoints[index].capability.size != 0U) {
            memmove(frame + cursor, endpoints[index].capability.data,
                    endpoints[index].capability.size);
            cursor += endpoints[index].capability.size;
        }
    }

    /* Stage the endpoint records temporarily so route validation is exact. */
    runtime->generation[staging_bank(runtime)] = generation;
    runtime->endpoint_count[staging_bank(runtime)] = (uint32_t)endpoint_count;
    runtime->capability_used[staging_bank(runtime)] =
        (uint32_t)capability_total;
    for (index = 0U; index < endpoint_count; ++index) {
        EndpointSlot *slot = &endpoint_slots(
            runtime, staging_bank(runtime)
        )[index];
        slot->id = endpoints[index].id;
        slot->incarnation = endpoints[index].incarnation;
        slot->capability_schema = endpoints[index].capability_schema;
    }
    for (index = 0U; index < route_count; ++index) {
        MeisoRuntimeStatus status;
        size_t previous;

        status = route_input_validate(
            runtime, staging_bank(runtime), &routes[index]
        );
        if (status != MEISO_RUNTIME_OK) {
            clear_catalog_bank(runtime, staging_bank(runtime));
            return status;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (meiso_resource_id_equal(
                    routes[previous].resource.id, routes[index].resource.id
                )) {
                clear_catalog_bank(runtime, staging_bank(runtime));
                return MEISO_RUNTIME_CONFLICT;
            }
        }
        put_endpoint_id(frame + cursor, routes[index].endpoint_id);
        put_u64(frame + cursor + 16U, routes[index].endpoint_incarnation);
        put_resource_id(frame + cursor + 24U, routes[index].resource.id);
        frame[cursor + 40U] = routes[index].resource.kind;
        frame[cursor + 41U] = routes[index].resource.owner;
        frame[cursor + 42U] = routes[index].delivery;
        frame[cursor + 43U] = 0U;
        put_schema(frame + cursor + 44U, routes[index].resource.schema);
        put_schema(frame + cursor + 64U, routes[index].selection_schema);
        put_u64(frame + cursor + 84U,
                (uint64_t)routes[index].resource.max_payload_size);
        put_u32(frame + cursor + 92U, routes[index].max_selection_bytes);
        cursor += ROUTE_RECORD_SIZE;
    }
    clear_catalog_bank(runtime, staging_bank(runtime));
    *out_size = cursor;
    return MEISO_RUNTIME_OK;
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
    uint64_t previous_highest_generation;
    size_t size;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (runtime->catalog_decision_pending) {
        return MEISO_RUNTIME_CATALOG_PENDING;
    }
    previous_highest_generation = runtime->highest_catalog_generation;
    status = encode_catalog(runtime, generation, endpoints, endpoint_count,
                            routes, route_count, &size);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    status = stage_catalog_wire(
        runtime, (MeisoByteView){tx_bytes(runtime), size}
    );
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    status = submit_bytes(runtime, size);
    if (status != MEISO_RUNTIME_OK) {
        clear_catalog_decision(runtime);
        runtime->highest_catalog_generation = previous_highest_generation;
    }
    return status;
}

static MeisoRuntimeStatus submit_catalog_decision(
    MeisoRuntime *runtime,
    uint8_t type,
    MeisoRuntimeCatalogRejectReason reason
)
{
    uint8_t *frame = tx_bytes(runtime);
    size_t size;

    if (!runtime->catalog_decision_pending) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    if (type == WIRE_CATALOG_ACCEPT) {
        if (!runtime->staged_present) {
            return MEISO_RUNTIME_NO_CATALOG;
        }
        size = MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_ACCEPT_SIZE;
        encode_header(frame, type, (uint32_t)size);
        put_u64(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE,
                runtime->pending_catalog_generation);
    } else {
        size = MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_REJECT_SIZE;
        encode_header(frame, type, (uint32_t)size);
        put_u64(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE,
                runtime->pending_catalog_generation);
        frame[MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U] = reason;
        frame[MEISO_RUNTIME_WIRE_HEADER_SIZE + 9U] = 0U;
        frame[MEISO_RUNTIME_WIRE_HEADER_SIZE + 10U] = 0U;
        frame[MEISO_RUNTIME_WIRE_HEADER_SIZE + 11U] = 0U;
    }
    return submit_bytes(runtime, size);
}

MeisoRuntimeStatus meiso_runtime_accept_catalog(MeisoRuntime *runtime)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    status = submit_catalog_decision(
        runtime, WIRE_CATALOG_ACCEPT,
        MEISO_RUNTIME_CATALOG_REJECT_UNSUPPORTED
    );
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

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (!reject_reason_valid(reason)) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    status = submit_catalog_decision(runtime, WIRE_CATALOG_REJECT, reason);
    if (status == MEISO_RUNTIME_OK) {
        clear_catalog_decision(runtime);
    }
    return status;
}

static MeisoRuntimeStatus resolve_active_route(
    const MeisoRuntime *runtime,
    MeisoEndpointId endpoint_id,
    uint64_t endpoint_incarnation,
    MeisoResourceId resource_id,
    size_t *out_index
)
{
    int endpoint_index;
    int route_index;
    const EndpointSlot *endpoints;
    const MeisoRuntimeRouteDescriptor *route;

    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    endpoint_index = endpoint_index_in_bank(
        runtime, runtime->active_bank, endpoint_id
    );
    if (endpoint_index < 0) {
        return MEISO_RUNTIME_UNKNOWN_ENDPOINT;
    }
    endpoints = endpoint_slots_const(runtime, runtime->active_bank);
    if (endpoints[(size_t)endpoint_index].incarnation !=
        endpoint_incarnation) {
        return MEISO_RUNTIME_STALE_ENDPOINT;
    }
    route_index = route_index_in_bank(
        runtime, runtime->active_bank, resource_id
    );
    if (route_index < 0) {
        return MEISO_RUNTIME_UNKNOWN_RESOURCE;
    }
    route = &route_slots_const(
        runtime, runtime->active_bank
    )[(size_t)route_index].value;
    if (!meiso_endpoint_id_equal(route->endpoint_id, endpoint_id)) {
        return MEISO_RUNTIME_UNKNOWN_RESOURCE;
    }
    if (route->endpoint_incarnation != endpoint_incarnation) {
        return MEISO_RUNTIME_STALE_ENDPOINT;
    }
    if (out_index != NULL) {
        *out_index = (size_t)route_index;
    }
    return MEISO_RUNTIME_OK;
}

static bool route_is_subscription(
    const MeisoRuntimeRouteDescriptor *route
)
{
    return route->delivery == MEISO_RUNTIME_DELIVERY_HOST_SUBSCRIPTION;
}

static MeisoRuntimeStatus validate_subscription_route(
    const MeisoRuntime *runtime,
    size_t route_index
)
{
    const MeisoRuntimeRouteDescriptor *route = &route_slots_const(
        runtime, runtime->active_bank
    )[route_index].value;

    if (!route_is_subscription(route) ||
        route->resource.kind != MEISO_RESOURCE_KIND_STREAM) {
        return MEISO_RUNTIME_UNSUPPORTED_KIND;
    }
    if (route->resource.owner != MEISO_OWNER_ROLE_EDGE) {
        return MEISO_RUNTIME_OWNER_MISMATCH;
    }
    return MEISO_RUNTIME_OK;
}

static void set_desired(
    MeisoRuntime *runtime,
    size_t route_index,
    const MeisoRuntimeSubscriptionDesired *desired
)
{
    SubscriptionSlot *slot = &subscription_slots(runtime)[route_index];

    if (desired->parameters.size != 0U) {
        memmove(desired_bytes(runtime, route_index), desired->parameters.data,
                desired->parameters.size);
    }
    slot->request_id = desired->request_id;
    slot->desired_size = (uint32_t)desired->parameters.size;
    slot->effective_size = 0U;
    slot->route_epoch = 0U;
    slot->actual_status = MEISO_RUNTIME_ACTUAL_NONE;
    slot->actual_reason = MEISO_RUNTIME_ACTUAL_REASON_NONE;
    slot->desired_present = true;
    slot->enabled = desired->enabled;
}

MeisoRuntimeStatus meiso_runtime_request_subscription(
    MeisoRuntime *runtime,
    const MeisoRuntimeSubscriptionDesired *desired
)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);
    MeisoRuntimeSubscriptionDesired committed;
    const MeisoRuntimeRouteDescriptor *route;
    uint8_t *frame;
    size_t route_index;
    size_t size;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (desired == NULL || desired->request_id == 0U ||
        !meiso_endpoint_id_valid(desired->endpoint_id) ||
        desired->endpoint_incarnation == 0U ||
        !meiso_resource_id_valid(desired->resource_id) ||
        !meiso_byte_view_valid(desired->parameters)) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    committed = *desired;
    status = resolve_active_route(
        runtime, desired->endpoint_id, desired->endpoint_incarnation,
        desired->resource_id, &route_index
    );
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    status = validate_subscription_route(runtime, route_index);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    route = &route_slots_const(
        runtime, runtime->active_bank
    )[route_index].value;
    if (subscription_slots_const(runtime)[route_index].desired_present &&
        desired->request_id <=
            subscription_slots_const(runtime)[route_index].request_id) {
        return MEISO_RUNTIME_REQUEST_MISMATCH;
    }
    if (desired->parameters.size > route->max_selection_bytes ||
        desired->parameters.size > runtime->max_parameter_bytes_per_route ||
        desired->parameters.size > UINT32_MAX) {
        return MEISO_RUNTIME_CAPACITY;
    }
    size = MEISO_RUNTIME_WIRE_HEADER_SIZE + DESIRED_PREFIX_SIZE +
        desired->parameters.size;
    if (size > runtime->limits.max_runtime_payload_bytes || size > UINT32_MAX) {
        return MEISO_RUNTIME_CAPACITY;
    }
    frame = tx_bytes(runtime);
    encode_header(frame, WIRE_SUBSCRIPTION_DESIRED, (uint32_t)size);
    put_u64(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE,
            runtime->generation[runtime->active_bank]);
    put_endpoint_id(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U,
                    desired->endpoint_id);
    put_u64(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 24U,
            desired->endpoint_incarnation);
    put_resource_id(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 32U,
                    desired->resource_id);
    put_u64(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 48U,
            desired->request_id);
    frame[MEISO_RUNTIME_WIRE_HEADER_SIZE + 56U] =
        desired->enabled ? 1U : 0U;
    frame[MEISO_RUNTIME_WIRE_HEADER_SIZE + 57U] = 0U;
    frame[MEISO_RUNTIME_WIRE_HEADER_SIZE + 58U] = 0U;
    frame[MEISO_RUNTIME_WIRE_HEADER_SIZE + 59U] = 0U;
    put_u32(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 60U,
            (uint32_t)desired->parameters.size);
    if (desired->parameters.size != 0U) {
        memmove(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + DESIRED_PREFIX_SIZE,
                desired->parameters.data, desired->parameters.size);
    }
    committed.parameters.data = desired->parameters.size == 0U ? NULL :
        frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + DESIRED_PREFIX_SIZE;
    status = submit_bytes(runtime, size);
    if (status == MEISO_RUNTIME_OK) {
        set_desired(runtime, route_index, &committed);
    }
    return status;
}

static MeisoRuntimeStatus actual_value_validate(
    const MeisoRuntimeSubscriptionActual *actual,
    const SubscriptionSlot *subscription,
    const MeisoRuntimeRouteDescriptor *route,
    uint32_t max_parameter_bytes
)
{
    if (actual == NULL || actual->request_id == 0U ||
        !meiso_endpoint_id_valid(actual->endpoint_id) ||
        actual->endpoint_incarnation == 0U ||
        !meiso_resource_id_valid(actual->resource_id) ||
        !actual_status_reason_valid(actual->status, actual->reason) ||
        !meiso_byte_view_valid(actual->effective_parameters)) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!subscription->desired_present) {
        return MEISO_RUNTIME_NO_DESIRED;
    }
    if (subscription->request_id != actual->request_id) {
        return MEISO_RUNTIME_REQUEST_MISMATCH;
    }
    if (actual->effective_parameters.size > route->max_selection_bytes ||
        actual->effective_parameters.size > max_parameter_bytes ||
        actual->effective_parameters.size > UINT32_MAX) {
        return MEISO_RUNTIME_CAPACITY;
    }
    if ((actual->status == MEISO_RUNTIME_ACTUAL_ACTIVE) !=
        (actual->route_epoch != 0U)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    if (actual->status == MEISO_RUNTIME_ACTUAL_ACTIVE &&
        !subscription->enabled) {
        return MEISO_RUNTIME_CONFLICT;
    }
    return MEISO_RUNTIME_OK;
}

static void set_actual(
    MeisoRuntime *runtime,
    size_t route_index,
    const MeisoRuntimeSubscriptionActual *actual
)
{
    SubscriptionSlot *slot = &subscription_slots(runtime)[route_index];

    if (actual->effective_parameters.size != 0U) {
        memmove(effective_bytes(runtime, route_index),
                actual->effective_parameters.data,
                actual->effective_parameters.size);
    }
    slot->actual_status = actual->status;
    slot->actual_reason = actual->reason;
    slot->route_epoch = actual->route_epoch;
    slot->effective_size = (uint32_t)actual->effective_parameters.size;
}

MeisoRuntimeStatus meiso_runtime_report_subscription(
    MeisoRuntime *runtime,
    const MeisoRuntimeSubscriptionActual *actual
)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);
    MeisoRuntimeSubscriptionActual committed;
    const MeisoRuntimeRouteDescriptor *route;
    const SubscriptionSlot *subscription;
    uint8_t *frame;
    size_t route_index;
    size_t size;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (actual == NULL) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    committed = *actual;
    status = resolve_active_route(
        runtime, actual->endpoint_id, actual->endpoint_incarnation,
        actual->resource_id, &route_index
    );
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    status = validate_subscription_route(runtime, route_index);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    route = &route_slots_const(
        runtime, runtime->active_bank
    )[route_index].value;
    subscription = &subscription_slots_const(runtime)[route_index];
    status = actual_value_validate(
        actual, subscription, route,
        runtime->max_parameter_bytes_per_route
    );
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    size = MEISO_RUNTIME_WIRE_HEADER_SIZE + ACTUAL_PREFIX_SIZE +
        actual->effective_parameters.size;
    if (size > runtime->limits.max_runtime_payload_bytes || size > UINT32_MAX) {
        return MEISO_RUNTIME_CAPACITY;
    }
    frame = tx_bytes(runtime);
    encode_header(frame, WIRE_SUBSCRIPTION_ACTUAL, (uint32_t)size);
    put_u64(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE,
            runtime->generation[runtime->active_bank]);
    put_endpoint_id(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U,
                    actual->endpoint_id);
    put_u64(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 24U,
            actual->endpoint_incarnation);
    put_resource_id(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 32U,
                    actual->resource_id);
    put_u64(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 48U,
            actual->request_id);
    frame[MEISO_RUNTIME_WIRE_HEADER_SIZE + 56U] = actual->status;
    frame[MEISO_RUNTIME_WIRE_HEADER_SIZE + 57U] = actual->reason;
    frame[MEISO_RUNTIME_WIRE_HEADER_SIZE + 58U] = 0U;
    frame[MEISO_RUNTIME_WIRE_HEADER_SIZE + 59U] = 0U;
    put_u64(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 60U,
            actual->route_epoch);
    put_u32(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 68U,
            (uint32_t)actual->effective_parameters.size);
    if (actual->effective_parameters.size != 0U) {
        memmove(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + ACTUAL_PREFIX_SIZE,
                actual->effective_parameters.data,
                actual->effective_parameters.size);
    }
    committed.effective_parameters.data =
        actual->effective_parameters.size == 0U ? NULL :
        frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + ACTUAL_PREFIX_SIZE;
    status = submit_bytes(runtime, size);
    if (status == MEISO_RUNTIME_OK) {
        set_actual(runtime, route_index, &committed);
    }
    return status;
}

static bool owner_is_local(
    const MeisoRuntime *runtime,
    MeisoOwnerRole owner
)
{
    return (runtime->local_role == MEISO_SESSION_ROLE_HOST &&
            owner == MEISO_OWNER_ROLE_HOST) ||
        (runtime->local_role == MEISO_SESSION_ROLE_EDGE &&
         owner == MEISO_OWNER_ROLE_EDGE);
}

static MeisoRuntimeStatus route_delivery_validate(
    const MeisoRuntime *runtime,
    size_t route_index,
    uint64_t request_id,
    uint64_t route_epoch
)
{
    const MeisoRuntimeRouteDescriptor *route = &route_slots_const(
        runtime, runtime->active_bank
    )[route_index].value;
    const SubscriptionSlot *subscription =
        &subscription_slots_const(runtime)[route_index];

    if (route->delivery == MEISO_RUNTIME_DELIVERY_ANNOUNCED) {
        return request_id == 0U && route_epoch == 0U ? MEISO_RUNTIME_OK :
            MEISO_RUNTIME_ROUTE_NOT_ACTIVE;
    }
    return subscription->desired_present && subscription->enabled &&
        subscription->request_id == request_id && request_id != 0U &&
        subscription->actual_status == MEISO_RUNTIME_ACTUAL_ACTIVE &&
        subscription->route_epoch != 0U &&
        subscription->route_epoch == route_epoch ?
        MEISO_RUNTIME_OK : MEISO_RUNTIME_ROUTE_NOT_ACTIVE;
}

static MeisoRuntimeStatus resource_value_validate(
    const MeisoRuntime *runtime,
    const MeisoRuntimeResourceMessage *message,
    bool sending,
    size_t *out_route_index
)
{
    MeisoRuntimeStatus status;
    const MeisoRuntimeRouteDescriptor *route;
    size_t route_index;

    if (message == NULL || !meiso_endpoint_id_valid(message->endpoint_id) ||
        message->endpoint_incarnation == 0U ||
        !meiso_resource_id_valid(message->resource_id) ||
        !schema_valid(message->schema) ||
        !meiso_byte_view_valid(message->payload)) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    status = resolve_active_route(
        runtime, message->endpoint_id, message->endpoint_incarnation,
        message->resource_id, &route_index
    );
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    route = &route_slots_const(
        runtime, runtime->active_bank
    )[route_index].value;
    if (route->resource.kind != message->kind) {
        return MEISO_RUNTIME_UNSUPPORTED_KIND;
    }
    if (!schema_equal(route->resource.schema, message->schema)) {
        return MEISO_RUNTIME_SCHEMA_MISMATCH;
    }
    if (sending == !owner_is_local(runtime, route->resource.owner)) {
        return MEISO_RUNTIME_OWNER_MISMATCH;
    }
    if (message->payload.size > route->resource.max_payload_size ||
        message->payload.size > UINT32_MAX) {
        return MEISO_RUNTIME_PAYLOAD_CAPACITY;
    }
    status = route_delivery_validate(runtime, route_index,
                                     message->request_id,
                                     message->route_epoch);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (out_route_index != NULL) {
        *out_route_index = route_index;
    }
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_send_resource(
    MeisoRuntime *runtime,
    const MeisoRuntimeResourceMessage *message
)
{
    MeisoRuntimeStatus status = check_bound_mutable(runtime);
    uint8_t *frame;
    size_t size;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    status = resource_value_validate(runtime, message, true, NULL);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    size = MEISO_RUNTIME_WIRE_HEADER_SIZE + RESOURCE_PREFIX_SIZE +
        message->payload.size;
    if (size > runtime->limits.max_runtime_payload_bytes || size > UINT32_MAX) {
        return MEISO_RUNTIME_PAYLOAD_CAPACITY;
    }
    frame = tx_bytes(runtime);
    encode_header(frame, WIRE_RESOURCE, (uint32_t)size);
    put_u64(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE,
            runtime->generation[runtime->active_bank]);
    put_endpoint_id(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U,
                    message->endpoint_id);
    put_u64(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 24U,
            message->endpoint_incarnation);
    put_resource_id(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 32U,
                    message->resource_id);
    frame[MEISO_RUNTIME_WIRE_HEADER_SIZE + 48U] = message->kind;
    frame[MEISO_RUNTIME_WIRE_HEADER_SIZE + 49U] = 0U;
    frame[MEISO_RUNTIME_WIRE_HEADER_SIZE + 50U] = 0U;
    frame[MEISO_RUNTIME_WIRE_HEADER_SIZE + 51U] = 0U;
    put_schema(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 52U,
               message->schema);
    put_u64(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 72U,
            message->request_id);
    put_u64(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 80U,
            message->route_epoch);
    put_u32(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + 88U,
            (uint32_t)message->payload.size);
    if (message->payload.size != 0U) {
        memmove(frame + MEISO_RUNTIME_WIRE_HEADER_SIZE + RESOURCE_PREFIX_SIZE,
                message->payload.data, message->payload.size);
    }
    return submit_bytes(runtime, size);
}

static MeisoRuntimeStatus receive_catalog(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    MeisoRuntimeStatus status;

    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    status = stage_catalog_wire(runtime, frame);
    if (status == MEISO_RUNTIME_OK) {
        event->type = MEISO_RUNTIME_EVENT_CATALOG_OFFERED;
        event->catalog_generation =
            runtime->generation[staging_bank(runtime)];
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
    if (frame.size != MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_ACCEPT_SIZE) {
        return MEISO_RUNTIME_MALFORMED;
    }
    generation = get_u64(frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE);
    if (!runtime->catalog_decision_pending || !runtime->staged_present) {
        return runtime->active_present &&
            generation <= runtime->generation[runtime->active_bank] ?
            MEISO_RUNTIME_STALE_CATALOG : MEISO_RUNTIME_NO_CATALOG;
    }
    if (generation != runtime->pending_catalog_generation) {
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
    uint64_t generation;
    MeisoRuntimeCatalogRejectReason reason;

    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (frame.size != MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_REJECT_SIZE ||
        frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 9U] != 0U ||
        frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 10U] != 0U ||
        frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 11U] != 0U) {
        return MEISO_RUNTIME_MALFORMED;
    }
    generation = get_u64(frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE);
    reason = frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U];
    if (!reject_reason_valid(reason)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    if (!runtime->catalog_decision_pending) {
        return generation <= runtime->highest_catalog_generation ?
            MEISO_RUNTIME_STALE_CATALOG : MEISO_RUNTIME_NO_CATALOG;
    }
    if (generation != runtime->pending_catalog_generation) {
        return MEISO_RUNTIME_STALE_CATALOG;
    }
    clear_catalog_decision(runtime);
    event->type = MEISO_RUNTIME_EVENT_CATALOG_REJECTED;
    event->catalog_generation = generation;
    event->reject_reason = reason;
    return MEISO_RUNTIME_OK;
}

static MeisoRuntimeStatus receive_desired(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    MeisoRuntimeSubscriptionDesired desired;
    MeisoRuntimeStatus status;
    const MeisoRuntimeRouteDescriptor *route;
    uint64_t generation;
    uint32_t parameter_size;
    size_t route_index;

    if (runtime->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (frame.size < MEISO_RUNTIME_WIRE_HEADER_SIZE + DESIRED_PREFIX_SIZE) {
        return MEISO_RUNTIME_MALFORMED;
    }
    generation = get_u64(frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE);
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    if (generation != runtime->generation[runtime->active_bank]) {
        return MEISO_RUNTIME_STALE_CATALOG;
    }
    if (frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 56U] > 1U ||
        frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 57U] != 0U ||
        frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 58U] != 0U ||
        frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 59U] != 0U) {
        return MEISO_RUNTIME_MALFORMED;
    }
    parameter_size = get_u32(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 60U
    );
    if (parameter_size != frame.size -
            (MEISO_RUNTIME_WIRE_HEADER_SIZE + DESIRED_PREFIX_SIZE)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    memset(&desired, 0, sizeof(desired));
    desired.endpoint_id = get_endpoint_id(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U
    );
    desired.endpoint_incarnation = get_u64(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 24U
    );
    desired.resource_id = get_resource_id(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 32U
    );
    desired.request_id = get_u64(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 48U
    );
    desired.enabled =
        frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 56U] != 0U;
    desired.parameters.data = parameter_size == 0U ? NULL :
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + DESIRED_PREFIX_SIZE;
    desired.parameters.size = parameter_size;
    if (desired.request_id == 0U ||
        !meiso_endpoint_id_valid(desired.endpoint_id) ||
        desired.endpoint_incarnation == 0U ||
        !meiso_resource_id_valid(desired.resource_id)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    status = resolve_active_route(
        runtime, desired.endpoint_id, desired.endpoint_incarnation,
        desired.resource_id, &route_index
    );
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    status = validate_subscription_route(runtime, route_index);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    route = &route_slots_const(
        runtime, runtime->active_bank
    )[route_index].value;
    if (subscription_slots_const(runtime)[route_index].desired_present &&
        desired.request_id <=
            subscription_slots_const(runtime)[route_index].request_id) {
        return MEISO_RUNTIME_REQUEST_MISMATCH;
    }
    if (parameter_size > route->max_selection_bytes ||
        parameter_size > runtime->max_parameter_bytes_per_route) {
        return MEISO_RUNTIME_CAPACITY;
    }
    set_desired(runtime, route_index, &desired);
    event->type = MEISO_RUNTIME_EVENT_SUBSCRIPTION_DESIRED;
    event->catalog_generation = generation;
    event->desired = desired;
    event->desired.parameters.data = parameter_size == 0U ? NULL :
        desired_bytes(runtime, route_index);
    return MEISO_RUNTIME_OK;
}

static MeisoRuntimeStatus receive_actual(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    MeisoRuntimeSubscriptionActual actual;
    MeisoRuntimeStatus status;
    const MeisoRuntimeRouteDescriptor *route;
    const SubscriptionSlot *subscription;
    uint64_t generation;
    uint32_t effective_size;
    size_t route_index;

    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (frame.size < MEISO_RUNTIME_WIRE_HEADER_SIZE + ACTUAL_PREFIX_SIZE) {
        return MEISO_RUNTIME_MALFORMED;
    }
    generation = get_u64(frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE);
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    if (generation != runtime->generation[runtime->active_bank]) {
        return MEISO_RUNTIME_STALE_CATALOG;
    }
    if (frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 58U] != 0U ||
        frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 59U] != 0U) {
        return MEISO_RUNTIME_MALFORMED;
    }
    effective_size = get_u32(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 68U
    );
    if (effective_size != frame.size -
            (MEISO_RUNTIME_WIRE_HEADER_SIZE + ACTUAL_PREFIX_SIZE)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    memset(&actual, 0, sizeof(actual));
    actual.endpoint_id = get_endpoint_id(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U
    );
    actual.endpoint_incarnation = get_u64(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 24U
    );
    actual.resource_id = get_resource_id(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 32U
    );
    actual.request_id = get_u64(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 48U
    );
    actual.status = frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 56U];
    actual.reason = frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 57U];
    actual.route_epoch = get_u64(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 60U
    );
    actual.effective_parameters.data = effective_size == 0U ? NULL :
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + ACTUAL_PREFIX_SIZE;
    actual.effective_parameters.size = effective_size;
    status = resolve_active_route(
        runtime, actual.endpoint_id, actual.endpoint_incarnation,
        actual.resource_id, &route_index
    );
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    status = validate_subscription_route(runtime, route_index);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    route = &route_slots_const(
        runtime, runtime->active_bank
    )[route_index].value;
    subscription = &subscription_slots_const(runtime)[route_index];
    status = actual_value_validate(
        &actual, subscription, route,
        runtime->max_parameter_bytes_per_route
    );
    if (status != MEISO_RUNTIME_OK) {
        return status == MEISO_RUNTIME_INVALID_ARGUMENT ?
            MEISO_RUNTIME_MALFORMED : status;
    }
    set_actual(runtime, route_index, &actual);
    event->type = MEISO_RUNTIME_EVENT_SUBSCRIPTION_ACTUAL;
    event->catalog_generation = generation;
    event->actual = actual;
    event->actual.effective_parameters.data = effective_size == 0U ? NULL :
        effective_bytes(runtime, route_index);
    return MEISO_RUNTIME_OK;
}

static MeisoRuntimeStatus receive_resource(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    MeisoRuntimeResourceMessage message;
    MeisoRuntimeStatus status;
    uint64_t generation;
    uint32_t payload_size;

    if (frame.size < MEISO_RUNTIME_WIRE_HEADER_SIZE + RESOURCE_PREFIX_SIZE) {
        return MEISO_RUNTIME_MALFORMED;
    }
    generation = get_u64(frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE);
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    if (generation != runtime->generation[runtime->active_bank]) {
        return MEISO_RUNTIME_STALE_CATALOG;
    }
    if (frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 49U] != 0U ||
        frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 50U] != 0U ||
        frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 51U] != 0U) {
        return MEISO_RUNTIME_MALFORMED;
    }
    payload_size = get_u32(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 88U
    );
    if (payload_size != frame.size -
            (MEISO_RUNTIME_WIRE_HEADER_SIZE + RESOURCE_PREFIX_SIZE)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    memset(&message, 0, sizeof(message));
    message.endpoint_id = get_endpoint_id(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U
    );
    message.endpoint_incarnation = get_u64(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 24U
    );
    message.resource_id = get_resource_id(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 32U
    );
    message.kind = frame.data[MEISO_RUNTIME_WIRE_HEADER_SIZE + 48U];
    message.schema = get_schema(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 52U
    );
    message.request_id = get_u64(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 72U
    );
    message.route_epoch = get_u64(
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + 80U
    );
    message.payload.data = payload_size == 0U ? NULL :
        frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE + RESOURCE_PREFIX_SIZE;
    message.payload.size = payload_size;
    status = resource_value_validate(runtime, &message, false, NULL);
    if (status != MEISO_RUNTIME_OK) {
        return status == MEISO_RUNTIME_INVALID_ARGUMENT ?
            MEISO_RUNTIME_MALFORMED : status;
    }
    event->type = MEISO_RUNTIME_EVENT_RESOURCE;
    event->catalog_generation = generation;
    event->resource = message;
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
    if (message == NULL || out_event == NULL ||
        !meiso_byte_view_valid(message->payload) ||
        output_overlaps_runtime_or_session(
            runtime, out_event, sizeof(*out_event)
        ) ||
        ranges_overlap(message, sizeof(*message),
                       out_event, sizeof(*out_event)) ||
        (message->payload.data != NULL && ranges_overlap(
            message->payload.data, message->payload.size,
            out_event, sizeof(*out_event)
        ))) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    memset(out_event, 0, sizeof(*out_event));
    if (!meiso_session_id_equal(message->session_id, runtime->session_id)) {
        return MEISO_RUNTIME_STALE_SESSION;
    }
    if (!decode_header(message->payload, &type)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    switch (type) {
    case WIRE_CATALOG:
        return receive_catalog(runtime, message->payload, out_event);
    case WIRE_CATALOG_ACCEPT:
        return receive_catalog_accept(runtime, message->payload, out_event);
    case WIRE_CATALOG_REJECT:
        return receive_catalog_reject(runtime, message->payload, out_event);
    case WIRE_SUBSCRIPTION_DESIRED:
        return receive_desired(runtime, message->payload, out_event);
    case WIRE_SUBSCRIPTION_ACTUAL:
        return receive_actual(runtime, message->payload, out_event);
    case WIRE_RESOURCE:
        return receive_resource(runtime, message->payload, out_event);
    default:
        return MEISO_RUNTIME_MALFORMED;
    }
}

bool meiso_runtime_snapshot(
    const MeisoRuntime *runtime,
    MeisoRuntimeSnapshot *out_snapshot
)
{
    MeisoRuntimeStatus bound;

    if (!runtime_layout_valid(runtime) || out_snapshot == NULL ||
        output_overlaps_runtime_or_session(
            runtime, out_snapshot, sizeof(*out_snapshot)
        )) {
        return false;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    bound = check_bound_const(runtime);
    out_snapshot->session_id = runtime->session_id;
    out_snapshot->local_role = runtime->local_role;
    out_snapshot->negotiated_limits = runtime->limits;
    out_snapshot->core_ready = bound == MEISO_RUNTIME_OK;
    if (!out_snapshot->core_ready) {
        return true;
    }
    out_snapshot->highest_catalog_generation =
        runtime->highest_catalog_generation;
    out_snapshot->catalog_decision_pending =
        runtime->catalog_decision_pending;
    if (runtime->catalog_decision_pending) {
        out_snapshot->pending_catalog_generation =
            runtime->pending_catalog_generation;
    }
    out_snapshot->active_catalog_present = runtime->active_present;
    out_snapshot->staged_catalog_present = runtime->staged_present;
    if (runtime->active_present) {
        out_snapshot->active_catalog_generation =
            runtime->generation[runtime->active_bank];
        out_snapshot->active_endpoint_count =
            runtime->endpoint_count[runtime->active_bank];
        out_snapshot->active_route_count =
            runtime->route_count[runtime->active_bank];
    }
    if (runtime->staged_present) {
        out_snapshot->staged_catalog_generation =
            runtime->generation[staging_bank(runtime)];
        out_snapshot->staged_endpoint_count =
            runtime->endpoint_count[staging_bank(runtime)];
        out_snapshot->staged_route_count =
            runtime->route_count[staging_bank(runtime)];
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
    out_endpoint->capability.data = slot->capability_size == 0U ? NULL :
        capability_bytes_const(runtime, bank) + slot->capability_offset;
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
    *out_route = route_slots_const(
        runtime, runtime->active_bank
    )[index].value;
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
    if (!meiso_endpoint_id_valid(endpoint_id) || out_endpoint == NULL ||
        output_overlaps_runtime_or_session(
            runtime, out_endpoint, sizeof(*out_endpoint)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    index = endpoint_index_in_bank(runtime, runtime->active_bank, endpoint_id);
    if (index < 0) {
        return MEISO_RUNTIME_UNKNOWN_ENDPOINT;
    }
    copy_endpoint_out(runtime, runtime->active_bank,
                      (size_t)index, out_endpoint);
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
    if (!meiso_resource_id_valid(resource_id) || out_route == NULL ||
        output_overlaps_runtime_or_session(
            runtime, out_route, sizeof(*out_route)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    index = route_index_in_bank(runtime, runtime->active_bank, resource_id);
    if (index < 0) {
        return MEISO_RUNTIME_UNKNOWN_RESOURCE;
    }
    *out_route = route_slots_const(
        runtime, runtime->active_bank
    )[(size_t)index].value;
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_subscription_snapshot(
    const MeisoRuntime *runtime,
    MeisoResourceId resource_id,
    MeisoRuntimeSubscriptionSnapshot *out_snapshot
)
{
    MeisoRuntimeStatus status = check_bound_const(runtime);
    const SubscriptionSlot *subscription;
    const MeisoRuntimeRouteDescriptor *route;
    int index;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (!meiso_resource_id_valid(resource_id) || out_snapshot == NULL ||
        output_overlaps_runtime_or_session(
            runtime, out_snapshot, sizeof(*out_snapshot)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    index = route_index_in_bank(runtime, runtime->active_bank, resource_id);
    if (index < 0) {
        return MEISO_RUNTIME_UNKNOWN_RESOURCE;
    }
    route = &route_slots_const(
        runtime, runtime->active_bank
    )[(size_t)index].value;
    if (!route_is_subscription(route)) {
        return MEISO_RUNTIME_UNSUPPORTED_KIND;
    }
    subscription = &subscription_slots_const(runtime)[(size_t)index];
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->desired_present = subscription->desired_present;
    out_snapshot->desired_enabled = subscription->enabled;
    out_snapshot->request_id = subscription->request_id;
    out_snapshot->desired_parameters.data =
        subscription->desired_size == 0U ? NULL :
        desired_bytes_const(runtime, (size_t)index);
    out_snapshot->desired_parameters.size = subscription->desired_size;
    out_snapshot->actual_status = subscription->actual_status;
    out_snapshot->actual_reason = subscription->actual_reason;
    out_snapshot->route_epoch = subscription->route_epoch;
    out_snapshot->effective_parameters.data =
        subscription->effective_size == 0U ? NULL :
        effective_bytes_const(runtime, (size_t)index);
    out_snapshot->effective_parameters.size = subscription->effective_size;
    out_snapshot->route_active = subscription->desired_present &&
        subscription->enabled &&
        subscription->actual_status == MEISO_RUNTIME_ACTUAL_ACTIVE &&
        subscription->route_epoch != 0U;
    return MEISO_RUNTIME_OK;
}
