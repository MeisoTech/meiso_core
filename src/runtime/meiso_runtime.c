#include "meiso_runtime_private.h"

#include <string.h>

/* Arena layout, storage access, Session binding and handle lifecycle. */

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
    MeisoRuntimeArenaLayout *out
)
{
    MeisoRuntimeArenaLayout value;
    size_t cursor = sizeof(MeisoRuntime);
    size_t actual_profile_total;
    size_t pending_desired_capacity;
    unsigned int bank;

    if (out == NULL || !meiso_runtime_private_checked_multiply(
            max_endpoints,
            max_actual_profile_bytes_per_endpoint,
            &actual_profile_total
        ) || !meiso_runtime_private_checked_add(
            MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE,
            max_desired_profile_bytes_per_endpoint,
            &pending_desired_capacity
        )) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    for (bank = 0U; bank < 2U; ++bank) {
        if (!meiso_runtime_private_add_aligned_array(
                &cursor, _Alignof(MeisoRuntimeEndpointSlot), max_endpoints,
                sizeof(MeisoRuntimeEndpointSlot), &value.endpoint_offset[bank]
            ) ||
            !meiso_runtime_private_add_aligned_array(
                &cursor, _Alignof(MeisoRuntimeRouteSlot), max_routes,
                sizeof(MeisoRuntimeRouteSlot), &value.route_offset[bank]
            ) ||
            !meiso_runtime_private_add_aligned_array(
                &cursor, 1U, max_capability_bytes, 1U,
                &value.capability_offset[bank]
            ) ||
            !meiso_runtime_private_add_aligned_array(
                &cursor, 1U, actual_profile_total, 1U,
                &value.actual_profile_offset[bank]
            )) {
            return false;
        }
    }
    if (!meiso_runtime_private_add_aligned_array(
            &cursor, _Alignof(MeisoRuntimeIdentitySlot), max_endpoint_history,
            sizeof(MeisoRuntimeIdentitySlot), &value.identity_offset
        ) ||
        !meiso_runtime_private_add_aligned_array(
            &cursor, 1U, pending_desired_capacity, 1U,
            &value.pending_desired_payload_offset
        ) ||
        !meiso_runtime_private_add_aligned_array(
            &cursor, 1U, max_actual_profile_bytes_per_endpoint, 1U,
            &value.pending_actual_profile_offset
        ) ||
        !meiso_runtime_private_add_aligned_array(
            &cursor, 1U, max_runtime_payload_bytes, 1U, &value.tx_offset
        ) || !meiso_runtime_private_add_aligned_array(
            &cursor,
            1U,
            (wire_version == MEISO_RUNTIME_WIRE_VERSION_V3 ||
             wire_version == MEISO_RUNTIME_WIRE_VERSION_V4) &&
                    local_role == MEISO_SESSION_ROLE_EDGE ?
                max_blob_transfer_bytes : 0U,
            1U,
            &value.blob_scratch_offset
        )) {
        return false;
    }
    if (wire_version == MEISO_RUNTIME_WIRE_VERSION_V4) {
        for (bank = 0U; bank < 2U; ++bank) {
            if (!meiso_runtime_private_add_aligned_array(
                    &cursor, 1U, max_routes, 1U,
                    &value.delivery_offset[bank]
                )) {
                return false;
            }
        }
    }
    if (
        !meiso_runtime_private_align_up(cursor, _Alignof(max_align_t), &value.required)) {
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
    MeisoRuntimeArenaLayout *out_layout
)
{
    uint64_t entry_capacity;

    if (config == NULL || out_snapshot == NULL || out_layout == NULL ||
        !snapshot_ready(config->session, out_snapshot) ||
        config->max_endpoints == 0U ||
        config->max_endpoint_history < config->max_endpoints ||
        config->max_routes == 0U ||
        (wire_version != MEISO_RUNTIME_WIRE_VERSION_V2 &&
         wire_version != MEISO_RUNTIME_WIRE_VERSION_V3 &&
         wire_version != MEISO_RUNTIME_WIRE_VERSION_V4) ||
        (wire_version == MEISO_RUNTIME_WIRE_VERSION_V2 &&
         max_blob_transfer_bytes != 0U) ||
        ((wire_version == MEISO_RUNTIME_WIRE_VERSION_V3 ||
          wire_version == MEISO_RUNTIME_WIRE_VERSION_V4) &&
         (max_blob_transfer_bytes == 0U ||
          out_snapshot->negotiated_limits.max_runtime_payload_bytes <
              MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE +
                  MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE))) {
        return false;
    }
    entry_capacity = (uint64_t)config->max_endpoints + config->max_routes;
    return out_snapshot->negotiated_limits.max_runtime_payload_bytes >=
            MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_CATALOG_PREFIX_SIZE &&
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
    MeisoRuntimeArenaLayout *out_layout
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

MeisoRuntimeEndpointSlot *meiso_runtime_private_endpoint_slots(MeisoRuntime *runtime, uint8_t bank)
{
    return (MeisoRuntimeEndpointSlot *)(void *)((uint8_t *)(void *)runtime +
                                    runtime->endpoint_offset[bank]);
}

const MeisoRuntimeEndpointSlot *meiso_runtime_private_endpoint_slots_const(
    const MeisoRuntime *runtime,
    uint8_t bank
)
{
    return (const MeisoRuntimeEndpointSlot *)(const void *)(
        (const uint8_t *)(const void *)runtime + runtime->endpoint_offset[bank]
    );
}

MeisoRuntimeRouteSlot *meiso_runtime_private_route_slots(MeisoRuntime *runtime, uint8_t bank)
{
    return (MeisoRuntimeRouteSlot *)(void *)((uint8_t *)(void *)runtime +
                                 runtime->route_offset[bank]);
}

const MeisoRuntimeRouteSlot *meiso_runtime_private_route_slots_const(
    const MeisoRuntime *runtime,
    uint8_t bank
)
{
    return (const MeisoRuntimeRouteSlot *)(const void *)(
        (const uint8_t *)(const void *)runtime + runtime->route_offset[bank]
    );
}

uint8_t *meiso_runtime_private_delivery_classes(MeisoRuntime *runtime, uint8_t bank)
{
    MeisoRuntimeArenaLayout layout;

    if (runtime == NULL || bank > 1U ||
        runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V4 ||
        !calculate_layout(
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
        )) {
        return NULL;
    }
    return (uint8_t *)(void *)runtime + layout.delivery_offset[bank];
}

const uint8_t *meiso_runtime_private_delivery_classes_const(
    const MeisoRuntime *runtime,
    uint8_t bank
)
{
    return meiso_runtime_private_delivery_classes((MeisoRuntime *)(uintptr_t)runtime, bank);
}

static bool delivery_banks_valid(const MeisoRuntime *runtime)
{
    unsigned int bank;

    if (runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V4) {
        return true;
    }
    for (bank = 0U; bank < 2U; ++bank) {
        const uint8_t *values = meiso_runtime_private_delivery_classes_const(
            runtime, (uint8_t)bank
        );
        uint32_t index;

        if (values == NULL) {
            return false;
        }
        for (index = 0U; index < runtime->route_count[bank]; ++index) {
            if (values[index] != MEISO_RUNTIME_DELIVERY_INLINE &&
                values[index] != MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA) {
                return false;
            }
        }
    }
    return true;
}

uint8_t *meiso_runtime_private_capability_bytes(MeisoRuntime *runtime, uint8_t bank)
{
    return (uint8_t *)(void *)runtime + runtime->capability_offset[bank];
}

const uint8_t *meiso_runtime_private_capability_bytes_const(
    const MeisoRuntime *runtime,
    uint8_t bank
)
{
    return (const uint8_t *)(const void *)runtime +
        runtime->capability_offset[bank];
}

uint8_t *meiso_runtime_private_actual_profile_bytes(
    MeisoRuntime *runtime,
    uint8_t bank,
    size_t endpoint_index
)
{
    return (uint8_t *)(void *)runtime + runtime->actual_profile_offset[bank] +
        endpoint_index * runtime->max_actual_profile_bytes_per_endpoint;
}

const uint8_t *meiso_runtime_private_actual_profile_bytes_const(
    const MeisoRuntime *runtime,
    uint8_t bank,
    size_t endpoint_index
)
{
    return (const uint8_t *)(const void *)runtime +
        runtime->actual_profile_offset[bank] +
        endpoint_index * runtime->max_actual_profile_bytes_per_endpoint;
}

uint8_t *meiso_runtime_private_pending_actual_profile_bytes(MeisoRuntime *runtime)
{
    return (uint8_t *)(void *)runtime +
        runtime->pending_actual_profile_offset;
}

MeisoRuntimeIdentitySlot *meiso_runtime_private_identity_slots(MeisoRuntime *runtime)
{
    return (MeisoRuntimeIdentitySlot *)(void *)((uint8_t *)(void *)runtime +
                                    runtime->identity_offset);
}

const MeisoRuntimeIdentitySlot *meiso_runtime_private_identity_slots_const(const MeisoRuntime *runtime)
{
    return (const MeisoRuntimeIdentitySlot *)(const void *)(
        (const uint8_t *)(const void *)runtime + runtime->identity_offset
    );
}

uint8_t *meiso_runtime_private_pending_desired_payload_bytes(MeisoRuntime *runtime)
{
    return (uint8_t *)(void *)runtime +
        runtime->pending_desired_payload_offset;
}

uint8_t *meiso_runtime_private_tx_bytes(MeisoRuntime *runtime)
{
    return (uint8_t *)(void *)runtime + runtime->tx_offset;
}

uint8_t *meiso_runtime_private_blob_scratch(MeisoRuntime *runtime)
{
    return (uint8_t *)(void *)runtime + runtime->blob_scratch_offset;
}

void meiso_runtime_private_clear_blob_transfer(MeisoRuntime *runtime)
{
    if ((runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V3 ||
         runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4) &&
        runtime->local_role == MEISO_SESSION_ROLE_EDGE &&
        runtime->max_blob_transfer_bytes != 0U) {
        memset(
            meiso_runtime_private_blob_scratch(runtime),
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

uint8_t meiso_runtime_private_staging_bank(const MeisoRuntime *runtime)
{
    return (uint8_t)(runtime->active_bank ^ 1U);
}

bool meiso_runtime_private_layout_valid(const MeisoRuntime *runtime)
{
    MeisoRuntimeArenaLayout layout;

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
        delivery_banks_valid(runtime) &&
        runtime->max_endpoint_history >= runtime->max_endpoints &&
        runtime->identity_count <= runtime->max_endpoint_history &&
        runtime->pending_desired_payload_size <=
            MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE +
                runtime->max_desired_profile_bytes_per_endpoint &&
        runtime->pending_actual_profile_size <=
            runtime->max_actual_profile_bytes_per_endpoint &&
        ((runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V2 &&
          runtime->max_blob_transfer_bytes == 0U) ||
         ((runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V3 ||
           runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4) &&
          runtime->max_blob_transfer_bytes != 0U)) &&
        (runtime->local_role == MEISO_SESSION_ROLE_HOST ||
         runtime->local_role == MEISO_SESSION_ROLE_EDGE);
}

void meiso_runtime_private_clear_catalog_bank(MeisoRuntime *runtime, uint8_t bank)
{
    const size_t actual_profile_total =
        (size_t)runtime->max_endpoints *
        runtime->max_actual_profile_bytes_per_endpoint;

    memset(
        meiso_runtime_private_endpoint_slots(runtime, bank),
        0,
        (size_t)runtime->max_endpoints * sizeof(MeisoRuntimeEndpointSlot)
    );
    memset(
        meiso_runtime_private_route_slots(runtime, bank),
        0,
        (size_t)runtime->max_routes * sizeof(MeisoRuntimeRouteSlot)
    );
    if (runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4) {
        memset(meiso_runtime_private_delivery_classes(runtime, bank), 0, runtime->max_routes);
    }
    if (runtime->max_capability_bytes != 0U) {
        memset(
            meiso_runtime_private_capability_bytes(runtime, bank),
            0,
            runtime->max_capability_bytes
        );
    }
    if (actual_profile_total != 0U) {
        memset(
            meiso_runtime_private_actual_profile_bytes(runtime, bank, 0U),
            0,
            actual_profile_total
        );
    }
    runtime->generation[bank] = 0U;
    runtime->endpoint_count[bank] = 0U;
    runtime->route_count[bank] = 0U;
    runtime->capability_used[bank] = 0U;
}

void meiso_runtime_private_clear_catalog_decision(MeisoRuntime *runtime)
{
    runtime->pending_catalog_generation = 0U;
    runtime->catalog_decision_pending = false;
}

void meiso_runtime_private_clear_pending_state_receive(MeisoRuntime *runtime)
{
    runtime->pending_desired_receive = false;
    runtime->pending_actual_receive = false;
    runtime->pending_desired_payload_size = 0U;
    runtime->pending_actual_profile_size = 0U;
}

static void invalidate_runtime(MeisoRuntime *runtime)
{
    meiso_runtime_private_clear_blob_transfer(runtime);
    runtime->invalidated = true;
    runtime->active_present = false;
    runtime->staged_present = false;
    meiso_runtime_private_clear_pending_state_receive(runtime);
    meiso_runtime_private_clear_catalog_decision(runtime);
    meiso_runtime_private_clear_catalog_bank(runtime, 0U);
    meiso_runtime_private_clear_catalog_bank(runtime, 1U);
}

MeisoRuntimeStatus meiso_runtime_private_check_bound_mutable_internal(
    MeisoRuntime *runtime,
    bool preserve_blob_publish
)
{
    MeisoSessionSnapshot snapshot;

    if (!meiso_runtime_private_layout_valid(runtime)) {
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
        meiso_runtime_private_clear_blob_transfer(runtime);
    }
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_private_check_bound_mutable(MeisoRuntime *runtime)
{
    return meiso_runtime_private_check_bound_mutable_internal(runtime, false);
}

MeisoRuntimeStatus meiso_runtime_private_check_bound_const(const MeisoRuntime *runtime)
{
    MeisoSessionSnapshot snapshot;

    if (!meiso_runtime_private_layout_valid(runtime)) {
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

bool meiso_runtime_private_output_overlaps_runtime_or_session(
    const MeisoRuntime *runtime,
    const void *output,
    size_t output_size
)
{
    return meiso_runtime_private_ranges_overlap(runtime, runtime->arena_size, output, output_size) ||
        meiso_runtime_private_ranges_overlap(runtime->session, 1U, output, output_size);
}
MeisoRuntimeStatus meiso_runtime_private_submit_bytes(MeisoRuntime *runtime, size_t size)
{
    MeisoSessionSubmitStatus status;
    const MeisoByteView payload = {meiso_runtime_private_tx_bytes(runtime), size};

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

MeisoRuntimeStatus meiso_runtime_private_blob_tx_preflight(const MeisoRuntime *runtime)
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
    MeisoRuntimeArenaLayout layout;

    return config_layout(config, &snapshot, &layout) ? layout.required : 0U;
}

size_t meiso_runtime_v3_memory_required(const MeisoRuntimeV3Config *config)
{
    MeisoSessionSnapshot snapshot;
    MeisoRuntimeArenaLayout layout;

    return config != NULL && config_layout_versioned(
        &config->base,
        MEISO_RUNTIME_WIRE_VERSION_V3,
        config->max_blob_transfer_bytes,
        &snapshot,
        &layout
    ) ? layout.required : 0U;
}

size_t meiso_runtime_v4_memory_required(const MeisoRuntimeV4Config *config)
{
    MeisoSessionSnapshot snapshot;
    MeisoRuntimeArenaLayout layout;

    return config != NULL && config_layout_versioned(
        &config->base,
        MEISO_RUNTIME_WIRE_VERSION_V4,
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
    MeisoRuntimeArenaLayout layout;
    MeisoRuntime *runtime;

    if (out_runtime == NULL) {
        return MEISO_RUNTIME_INIT_INVALID_ARGUMENT;
    }
    if ((arena != NULL && meiso_runtime_private_ranges_overlap(
            arena, arena_size, out_runtime, sizeof(*out_runtime)
        )) ||
        (config_object != NULL && meiso_runtime_private_ranges_overlap(
            config_object,
            config_object_size,
            out_runtime,
            sizeof(*out_runtime)
        )) ||
        (config != NULL && config->session != NULL && meiso_runtime_private_ranges_overlap(
            config->session, 1U, out_runtime, sizeof(*out_runtime)
        )) ||
        (arena != NULL && config != NULL && config->session != NULL &&
         meiso_runtime_private_ranges_overlap(arena, arena_size, config->session, 1U))) {
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

MeisoRuntimeInitStatus meiso_runtime_v4_init(
    void *arena,
    size_t arena_size,
    const MeisoRuntimeV4Config *config,
    MeisoRuntime **out_runtime
)
{
    return runtime_init_versioned(
        arena,
        arena_size,
        config != NULL ? &config->base : NULL,
        config,
        config != NULL ? sizeof(*config) : 0U,
        MEISO_RUNTIME_WIRE_VERSION_V4,
        config != NULL ? config->max_blob_transfer_bytes : 0U,
        out_runtime
    );
}

MeisoRuntimeStatus meiso_runtime_refresh_bound_session(
    MeisoRuntime *runtime
)
{
    return meiso_runtime_private_check_bound_mutable_internal(runtime, true);
}
