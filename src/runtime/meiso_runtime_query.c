#include "meiso_runtime_private.h"

#include <string.h>

/* Read-only snapshots, catalog views and active media bindings. */

bool meiso_runtime_snapshot(
    const MeisoRuntime *runtime,
    MeisoRuntimeSnapshot *out_snapshot
)
{
    MeisoRuntimeStatus status;
    const uint8_t staged = runtime != NULL && meiso_runtime_private_layout_valid(runtime) ?
        meiso_runtime_private_staging_bank(runtime) : 0U;

    if (runtime == NULL || out_snapshot == NULL ||
        !meiso_runtime_private_layout_valid(runtime) ||
        meiso_runtime_private_output_overlaps_runtime_or_session(
            runtime, out_snapshot, sizeof(*out_snapshot)
        )) {
        return false;
    }
    status = meiso_runtime_private_check_bound_const(runtime);
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
        !meiso_runtime_private_layout_valid(runtime) || meiso_runtime_private_output_overlaps_runtime_or_session(
            runtime, out_snapshot, sizeof(*out_snapshot)
        )) {
        return false;
    }
    status = meiso_runtime_private_check_bound_const(runtime);
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
    const MeisoRuntimeEndpointSlot *slot = &meiso_runtime_private_endpoint_slots_const(runtime, bank)[index];

    out_endpoint->id = slot->id;
    out_endpoint->incarnation = slot->incarnation;
    out_endpoint->capability_schema = slot->capability_schema;
    out_endpoint->capability.data = meiso_runtime_private_capability_bytes_const(runtime, bank) +
        slot->capability_offset;
    out_endpoint->capability.size = slot->capability_size;
}

MeisoRuntimeStatus meiso_runtime_endpoint_at(
    const MeisoRuntime *runtime,
    size_t index,
    MeisoRuntimeEndpointDescriptor *out_endpoint
)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_const(runtime);

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (out_endpoint == NULL || meiso_runtime_private_output_overlaps_runtime_or_session(
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
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_const(runtime);

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (out_route == NULL || meiso_runtime_private_output_overlaps_runtime_or_session(
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
    *out_route = meiso_runtime_private_route_slots_const(runtime, runtime->active_bank)[index].value;
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_staged_endpoint_at(
    const MeisoRuntime *runtime,
    size_t index,
    MeisoRuntimeEndpointDescriptor *out_endpoint
)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_const(runtime);
    uint8_t bank;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (out_endpoint == NULL || meiso_runtime_private_output_overlaps_runtime_or_session(
            runtime, out_endpoint, sizeof(*out_endpoint)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->staged_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    bank = meiso_runtime_private_staging_bank(runtime);
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
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_const(runtime);
    uint8_t bank;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (out_route == NULL || meiso_runtime_private_output_overlaps_runtime_or_session(
            runtime, out_route, sizeof(*out_route)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->staged_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    bank = meiso_runtime_private_staging_bank(runtime);
    if (index >= runtime->route_count[bank]) {
        return MEISO_RUNTIME_UNKNOWN_RESOURCE;
    }
    *out_route = meiso_runtime_private_route_slots_const(runtime, bank)[index].value;
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_v4_route_at(
    const MeisoRuntime *runtime,
    size_t index,
    MeisoRuntimeV4RouteDescriptor *out_route
)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_const(runtime);
    MeisoRuntimeV4RouteDescriptor value;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V4) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (out_route == NULL || meiso_runtime_private_output_overlaps_runtime_or_session(
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
    value.route = meiso_runtime_private_route_slots_const(runtime, runtime->active_bank)[index].value;
    value.delivery = meiso_runtime_private_route_delivery_at(
        runtime, runtime->active_bank, index
    );
    *out_route = value;
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_v4_staged_route_at(
    const MeisoRuntime *runtime,
    size_t index,
    MeisoRuntimeV4RouteDescriptor *out_route
)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_const(runtime);
    MeisoRuntimeV4RouteDescriptor value;
    uint8_t bank;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V4) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (out_route == NULL || meiso_runtime_private_output_overlaps_runtime_or_session(
            runtime, out_route, sizeof(*out_route)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->staged_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    bank = meiso_runtime_private_staging_bank(runtime);
    if (index >= runtime->route_count[bank]) {
        return MEISO_RUNTIME_UNKNOWN_RESOURCE;
    }
    value.route = meiso_runtime_private_route_slots_const(runtime, bank)[index].value;
    value.delivery = meiso_runtime_private_route_delivery_at(runtime, bank, index);
    *out_route = value;
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_find_endpoint(
    const MeisoRuntime *runtime,
    MeisoEndpointId endpoint_id,
    MeisoRuntimeEndpointDescriptor *out_endpoint
)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_const(runtime);
    int index;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (out_endpoint == NULL || !meiso_endpoint_id_valid(endpoint_id) ||
        meiso_runtime_private_output_overlaps_runtime_or_session(
            runtime, out_endpoint, sizeof(*out_endpoint)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    index = meiso_runtime_private_endpoint_index_in_bank(
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
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_const(runtime);
    int index;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (out_route == NULL || !meiso_resource_id_valid(resource_id) ||
        meiso_runtime_private_output_overlaps_runtime_or_session(
            runtime, out_route, sizeof(*out_route)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    index = meiso_runtime_private_route_index_in_bank(
        runtime, runtime->active_bank, resource_id
    );
    if (index < 0) {
        return MEISO_RUNTIME_UNKNOWN_RESOURCE;
    }
    *out_route = meiso_runtime_private_route_slots_const(runtime, runtime->active_bank)
        [(size_t)index].value;
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_v4_find_route(
    const MeisoRuntime *runtime,
    MeisoResourceId resource_id,
    MeisoRuntimeV4RouteDescriptor *out_route
)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_const(runtime);
    MeisoRuntimeV4RouteDescriptor value;
    int index;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V4) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (out_route == NULL || !meiso_resource_id_valid(resource_id) ||
        meiso_runtime_private_output_overlaps_runtime_or_session(
            runtime, out_route, sizeof(*out_route)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    index = meiso_runtime_private_route_index_in_bank(runtime, runtime->active_bank, resource_id);
    if (index < 0) {
        return MEISO_RUNTIME_UNKNOWN_RESOURCE;
    }
    value.route = meiso_runtime_private_route_slots_const(runtime, runtime->active_bank)
        [(size_t)index].value;
    value.delivery = meiso_runtime_private_route_delivery_at(
        runtime, runtime->active_bank, (size_t)index
    );
    *out_route = value;
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_v4_external_media_binding(
    const MeisoRuntime *runtime,
    MeisoResourceId resource_id,
    MeisoRuntimeV4ExternalMediaBinding *out_binding
)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_const(runtime);
    MeisoRuntimeV4ExternalMediaBinding value;
    const MeisoRuntimeEndpointSlot *endpoint;
    const MeisoRuntimeRouteSlot *route;
    int route_index;
    int endpoint_index;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V4) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    if (out_binding == NULL || !meiso_resource_id_valid(resource_id) ||
        meiso_runtime_private_output_overlaps_runtime_or_session(
            runtime, out_binding, sizeof(*out_binding)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    route_index = meiso_runtime_private_route_index_in_bank(
        runtime, runtime->active_bank, resource_id
    );
    if (route_index < 0) {
        return MEISO_RUNTIME_UNKNOWN_RESOURCE;
    }
    if (meiso_runtime_private_route_delivery_at(
            runtime, runtime->active_bank, (size_t)route_index
        ) != MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA) {
        return MEISO_RUNTIME_DELIVERY_MISMATCH;
    }
    route = &meiso_runtime_private_route_slots_const(runtime, runtime->active_bank)
        [(size_t)route_index];
    endpoint_index = meiso_runtime_private_endpoint_index_in_bank(
        runtime, runtime->active_bank, route->value.endpoint_id
    );
    if (endpoint_index < 0) {
        return MEISO_RUNTIME_UNKNOWN_ENDPOINT;
    }
    endpoint = &meiso_runtime_private_endpoint_slots_const(runtime, runtime->active_bank)
        [(size_t)endpoint_index];
    if (!endpoint->production.data_active) {
        return MEISO_RUNTIME_ROUTE_NOT_ACTIVE;
    }
    memset(&value, 0, sizeof(value));
    value.session_id = runtime->session_id;
    value.route.catalog_generation =
        runtime->generation[runtime->active_bank];
    value.route.endpoint_id = route->value.endpoint_id;
    value.route.endpoint_incarnation = route->value.endpoint_incarnation;
    value.route.resource_id = route->value.resource.id;
    value.route.facet = route->value.facet;
    value.resource = route->value.resource;
    value.production_epoch = endpoint->production.production_epoch;
    *out_binding = value;
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_production_snapshot(
    const MeisoRuntime *runtime,
    MeisoEndpointId endpoint_id,
    MeisoRuntimeProductionSnapshot *out_snapshot
)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_const(runtime);
    int index;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (out_snapshot == NULL || !meiso_endpoint_id_valid(endpoint_id) ||
        meiso_runtime_private_output_overlaps_runtime_or_session(
            runtime, out_snapshot, sizeof(*out_snapshot)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->active_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    index = meiso_runtime_private_endpoint_index_in_bank(
        runtime, runtime->active_bank, endpoint_id
    );
    if (index < 0) {
        return MEISO_RUNTIME_UNKNOWN_ENDPOINT;
    }
    *out_snapshot = meiso_runtime_private_endpoint_slots_const(runtime, runtime->active_bank)
        [(size_t)index].production;
    out_snapshot->effective_profile.data = meiso_runtime_private_actual_profile_bytes_const(
        runtime, runtime->active_bank, (size_t)index
    );
    out_snapshot->effective_profile.size =
        meiso_runtime_private_endpoint_slots_const(runtime, runtime->active_bank)
            [(size_t)index].actual_profile_size;
    return MEISO_RUNTIME_OK;
}
