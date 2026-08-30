#include "meiso_runtime_private.h"

#include <string.h>

/* Active route resolution and transfer-reference identity predicates. */

bool meiso_runtime_private_route_key_equal(MeisoRuntimeRouteKey lhs, MeisoRuntimeRouteKey rhs)
{
    return lhs.catalog_generation == rhs.catalog_generation &&
        meiso_endpoint_id_equal(lhs.endpoint_id, rhs.endpoint_id) &&
        lhs.endpoint_incarnation == rhs.endpoint_incarnation &&
        meiso_resource_id_equal(lhs.resource_id, rhs.resource_id) &&
        lhs.facet == rhs.facet;
}

MeisoRuntimeStatus meiso_runtime_private_resolve_active_route(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey key,
    MeisoRuntimeRouteSlot **out_route,
    MeisoRuntimeEndpointSlot **out_endpoint
)
{
    int route_index;
    int endpoint_index;
    MeisoRuntimeRouteSlot *route;

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
    endpoint_index = meiso_runtime_private_endpoint_index_in_bank(
        runtime, runtime->active_bank, key.endpoint_id
    );
    if (endpoint_index < 0) {
        return MEISO_RUNTIME_UNKNOWN_ENDPOINT;
    }
    if (meiso_runtime_private_endpoint_slots(runtime, runtime->active_bank)
            [(size_t)endpoint_index].incarnation != key.endpoint_incarnation) {
        return MEISO_RUNTIME_STALE_ENDPOINT;
    }
    route_index = meiso_runtime_private_route_index_in_bank(
        runtime, runtime->active_bank, key.resource_id
    );
    if (route_index < 0) {
        return MEISO_RUNTIME_UNKNOWN_RESOURCE;
    }
    route = &meiso_runtime_private_route_slots(runtime, runtime->active_bank)[(size_t)route_index];
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
    *out_endpoint = &meiso_runtime_private_endpoint_slots(runtime, runtime->active_bank)
        [(size_t)endpoint_index];
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_private_resolve_active_route_const(
    const MeisoRuntime *runtime,
    MeisoRuntimeRouteKey key,
    const MeisoRuntimeRouteSlot **out_route,
    const MeisoRuntimeEndpointSlot **out_endpoint,
    size_t *out_route_index
)
{
    int route_index;
    int endpoint_index;
    const MeisoRuntimeRouteSlot *route;

    if (out_route == NULL || out_endpoint == NULL || out_route_index == NULL ||
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
    endpoint_index = meiso_runtime_private_endpoint_index_in_bank(
        runtime, runtime->active_bank, key.endpoint_id
    );
    if (endpoint_index < 0) {
        return MEISO_RUNTIME_UNKNOWN_ENDPOINT;
    }
    if (meiso_runtime_private_endpoint_slots_const(runtime, runtime->active_bank)
            [(size_t)endpoint_index].incarnation != key.endpoint_incarnation) {
        return MEISO_RUNTIME_STALE_ENDPOINT;
    }
    route_index = meiso_runtime_private_route_index_in_bank(
        runtime, runtime->active_bank, key.resource_id
    );
    if (route_index < 0) {
        return MEISO_RUNTIME_UNKNOWN_RESOURCE;
    }
    route = &meiso_runtime_private_route_slots_const(runtime, runtime->active_bank)
        [(size_t)route_index];
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
    *out_endpoint = &meiso_runtime_private_endpoint_slots_const(runtime, runtime->active_bank)
        [(size_t)endpoint_index];
    *out_route_index = (size_t)route_index;
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeDeliveryClass meiso_runtime_private_route_delivery_at(
    const MeisoRuntime *runtime,
    uint8_t bank,
    size_t index
)
{
    return runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4 ?
        meiso_runtime_private_delivery_classes_const(runtime, bank)[index] :
        MEISO_RUNTIME_DELIVERY_INLINE;
}
bool meiso_runtime_private_blob_transfer_equal(
    const MeisoRuntimeBlobTransferRef *lhs,
    const MeisoRuntimeBlobTransferRef *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        meiso_runtime_private_route_key_equal(lhs->route, rhs->route) &&
        lhs->transfer_id == rhs->transfer_id &&
        meiso_content_id_equal(&lhs->content_id, &rhs->content_id) &&
        lhs->total_size == rhs->total_size;
}

bool meiso_runtime_private_blob_operation_valid(MeisoRuntimeBlobOperation value)
{
    return value >= MEISO_RUNTIME_BLOB_BEGIN &&
        value <= MEISO_RUNTIME_BLOB_ABORT;
}

bool meiso_runtime_private_blob_abort_reason_valid(MeisoRuntimeBlobAbortReason value)
{
    return value == MEISO_RUNTIME_BLOB_ABORT_CALLER_REQUEST;
}
