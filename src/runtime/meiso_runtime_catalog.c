#include "meiso_runtime_private.h"

#include <string.h>

/* Catalog admission, staging, wire construction and atomic activation. */

int meiso_runtime_private_endpoint_index_in_bank(
    const MeisoRuntime *runtime,
    uint8_t bank,
    MeisoEndpointId endpoint_id
)
{
    const MeisoRuntimeEndpointSlot *slots = meiso_runtime_private_endpoint_slots_const(runtime, bank);
    uint32_t index;

    for (index = 0U; index < runtime->endpoint_count[bank]; ++index) {
        if (meiso_endpoint_id_equal(slots[index].id, endpoint_id)) {
            return (int)index;
        }
    }
    return -1;
}

int meiso_runtime_private_identity_pair_index(
    const MeisoRuntime *runtime,
    MeisoEndpointId endpoint_id,
    uint64_t incarnation
)
{
    const MeisoRuntimeIdentitySlot *slots = meiso_runtime_private_identity_slots_const(runtime);
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
    const MeisoRuntimeIdentitySlot *slots = meiso_runtime_private_identity_slots_const(runtime);
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
    const MeisoRuntimeEndpointSlot *endpoints = meiso_runtime_private_endpoint_slots_const(runtime, bank);
    uint32_t new_identities = 0U;
    uint32_t index;

    for (index = 0U; index < runtime->endpoint_count[bank]; ++index) {
        const int ledger_index = meiso_runtime_private_identity_pair_index(
            runtime, endpoints[index].id, endpoints[index].incarnation
        );

        if (ledger_index < 0) {
            new_identities += 1U;
        } else if (!meiso_runtime_private_identity_slots_const(runtime)[(size_t)ledger_index]
                        .current) {
            return MEISO_RUNTIME_STALE_ENDPOINT;
        }
    }
    return new_identities >
            runtime->max_endpoint_history - runtime->identity_count ?
        MEISO_RUNTIME_CAPACITY : MEISO_RUNTIME_OK;
}

int meiso_runtime_private_route_index_in_bank(
    const MeisoRuntime *runtime,
    uint8_t bank,
    MeisoResourceId resource_id
)
{
    const MeisoRuntimeRouteSlot *slots = meiso_runtime_private_route_slots_const(runtime, bank);
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
        meiso_runtime_private_schema_valid(endpoint->capability_schema) &&
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
            MEISO_RUNTIME_STATE_OFFER_PREFIX_SIZE;
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
            MEISO_RUNTIME_STATE_OFFER_PREFIX_SIZE;
        break;
    case MEISO_RUNTIME_FACET_DATA:
        if (route->resource.kind != MEISO_RESOURCE_KIND_STREAM) {
            return MEISO_RUNTIME_UNSUPPORTED_KIND;
        }
        if (route->resource.owner != MEISO_OWNER_ROLE_EDGE) {
            return MEISO_RUNTIME_OWNER_MISMATCH;
        }
        minimum_wire = MEISO_RUNTIME_WIRE_HEADER_SIZE +
            MEISO_RUNTIME_STREAM_SAMPLE_PREFIX_SIZE;
        break;
    case MEISO_RUNTIME_FACET_IMMUTABLE:
        if (!meiso_runtime_private_blob_wire_supported(runtime)) {
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

static MeisoRuntimeStatus route_input_validate_delivery(
    const MeisoRuntime *runtime,
    const MeisoRuntimeRouteDescriptor *route,
    MeisoRuntimeDeliveryClass delivery
)
{
    if (delivery == MEISO_RUNTIME_DELIVERY_INLINE) {
        return route_input_validate(runtime, route);
    }
    if (delivery != MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA ||
        runtime == NULL ||
        runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V4 ||
        route == NULL || !meiso_endpoint_id_valid(route->endpoint_id) ||
        route->endpoint_incarnation == 0U ||
        meiso_resource_descriptor_validate(&route->resource) !=
            MEISO_DESCRIPTOR_VALID ||
        route->facet != MEISO_RUNTIME_FACET_DATA) {
        return MEISO_RUNTIME_DELIVERY_MISMATCH;
    }
    if (route->resource.kind != MEISO_RESOURCE_KIND_STREAM) {
        return MEISO_RUNTIME_UNSUPPORTED_KIND;
    }
    if (route->resource.owner != MEISO_OWNER_ROLE_EDGE) {
        return MEISO_RUNTIME_OWNER_MISMATCH;
    }
    return route->resource.max_payload_size == 0U ?
        MEISO_RUNTIME_PAYLOAD_CAPACITY : MEISO_RUNTIME_OK;
}

static bool catalog_wire_size(
    const MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_count,
    size_t route_count,
    size_t route_record_size,
    size_t *out_size
)
{
    size_t size = MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_CATALOG_PREFIX_SIZE;
    size_t records;
    size_t index;

    if (out_size == NULL ||
        !meiso_runtime_private_checked_multiply(endpoint_count, MEISO_RUNTIME_ENDPOINT_RECORD_SIZE, &records) ||
        !meiso_runtime_private_checked_add(size, records, &size)) {
        return false;
    }
    for (index = 0U; index < endpoint_count; ++index) {
        if (!meiso_runtime_private_checked_add(size, endpoints[index].capability.size, &size)) {
            return false;
        }
    }
    if (!meiso_runtime_private_checked_multiply(route_count, route_record_size, &records) ||
        !meiso_runtime_private_checked_add(size, records, &size)) {
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
    const MeisoRuntimeRouteSlot *routes = meiso_runtime_private_route_slots_const(runtime, bank);
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
                    meiso_runtime_private_endpoint_slots_const(runtime, bank)[endpoint_index].id
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
    const MeisoRuntimeV4RouteDescriptor *routes_v4,
    size_t route_count
)
{
    const uint8_t bank = meiso_runtime_private_staging_bank(runtime);
    MeisoRuntimeEndpointSlot *endpoint_output;
    MeisoRuntimeRouteSlot *route_output;
    uint8_t *capability_output;
    size_t capability_used = 0U;
    size_t wire_size;
    size_t index;
    MeisoRuntimeStatus status;

    if (generation == 0U || endpoint_count == 0U || route_count == 0U ||
        (endpoint_count != 0U && endpoints == NULL) ||
        (route_count != 0U &&
         ((routes == NULL) == (routes_v4 == NULL))) ||
        endpoint_count > runtime->max_endpoints ||
        route_count > runtime->max_routes || endpoint_count > UINT32_MAX ||
        route_count > UINT32_MAX ||
        meiso_runtime_private_ranges_overlap(
            runtime, runtime->arena_size, endpoints,
            endpoint_count * sizeof(*endpoints)
        ) ||
        meiso_runtime_private_ranges_overlap(
            runtime, runtime->arena_size,
            routes != NULL ? (const void *)routes : (const void *)routes_v4,
            route_count * (routes != NULL ? sizeof(*routes) :
                sizeof(*routes_v4))
        )) {
        return endpoint_count > runtime->max_endpoints ||
            route_count > runtime->max_routes ?
            MEISO_RUNTIME_CAPACITY : MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    for (index = 0U; index < endpoint_count; ++index) {
        if (!endpoint_input_valid(&endpoints[index]) ||
            meiso_runtime_private_ranges_overlap(
                runtime,
                runtime->arena_size,
                endpoints[index].capability.data,
                endpoints[index].capability.size
            ) ||
            !meiso_runtime_private_checked_add(
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
            endpoints, endpoint_count, route_count,
            runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4 ?
                MEISO_RUNTIME_ROUTE_V4_RECORD_SIZE : MEISO_RUNTIME_ROUTE_RECORD_SIZE,
            &wire_size
        ) || wire_size > runtime->limits.max_runtime_payload_bytes) {
        return MEISO_RUNTIME_CAPACITY;
    }

    meiso_runtime_private_clear_catalog_bank(runtime, bank);
    endpoint_output = meiso_runtime_private_endpoint_slots(runtime, bank);
    route_output = meiso_runtime_private_route_slots(runtime, bank);
    capability_output = meiso_runtime_private_capability_bytes(runtime, bank);
    capability_used = 0U;
    for (index = 0U; index < endpoint_count; ++index) {
        size_t earlier;

        for (earlier = 0U; earlier < index; ++earlier) {
            if (meiso_endpoint_id_equal(
                    endpoints[earlier].id,
                    endpoints[index].id
                )) {
                meiso_runtime_private_clear_catalog_bank(runtime, bank);
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
        const MeisoRuntimeRouteDescriptor *route = routes != NULL ?
            &routes[index] : &routes_v4[index].route;
        const MeisoRuntimeDeliveryClass delivery = routes != NULL ?
            MEISO_RUNTIME_DELIVERY_INLINE : routes_v4[index].delivery;
        const int endpoint_index = meiso_runtime_private_endpoint_index_in_bank(
            runtime, bank, route->endpoint_id
        );
        size_t earlier;

        status = route_input_validate_delivery(runtime, route, delivery);
        if (status != MEISO_RUNTIME_OK) {
            meiso_runtime_private_clear_catalog_bank(runtime, bank);
            return status;
        }
        if (endpoint_index < 0) {
            meiso_runtime_private_clear_catalog_bank(runtime, bank);
            return MEISO_RUNTIME_UNKNOWN_ENDPOINT;
        }
        if (meiso_runtime_private_endpoint_slots(runtime, bank)[(size_t)endpoint_index].incarnation !=
            route->endpoint_incarnation) {
            meiso_runtime_private_clear_catalog_bank(runtime, bank);
            return MEISO_RUNTIME_STALE_ENDPOINT;
        }
        for (earlier = 0U; earlier < index; ++earlier) {
            if (meiso_resource_id_equal(
                    (routes != NULL ? routes[earlier].resource.id :
                        routes_v4[earlier].route.resource.id),
                    route->resource.id
                )) {
                meiso_runtime_private_clear_catalog_bank(runtime, bank);
                return MEISO_RUNTIME_CONFLICT;
            }
        }
        route_output[index].value = *route;
        if (runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4) {
            meiso_runtime_private_delivery_classes(runtime, bank)[index] = delivery;
        }
        runtime->route_count[bank] += 1U;
    }
    status = validate_catalog_semantics(runtime, bank);
    if (status == MEISO_RUNTIME_OK) {
        status = validate_identity_ledger_capacity(runtime, bank);
    }
    if (status != MEISO_RUNTIME_OK) {
        meiso_runtime_private_clear_catalog_bank(runtime, bank);
        return status;
    }
    runtime->generation[bank] = generation;
    runtime->staged_present = true;
    return MEISO_RUNTIME_OK;
}

static size_t encode_catalog(MeisoRuntime *runtime, uint8_t bank)
{
    const MeisoRuntimeEndpointSlot *endpoints = meiso_runtime_private_endpoint_slots_const(runtime, bank);
    const MeisoRuntimeRouteSlot *routes = meiso_runtime_private_route_slots_const(runtime, bank);
    const uint8_t *capabilities = meiso_runtime_private_capability_bytes_const(runtime, bank);
    const uint8_t *deliveries = meiso_runtime_private_delivery_classes_const(runtime, bank);
    uint8_t *output = meiso_runtime_private_tx_bytes(runtime);
    size_t cursor = MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_CATALOG_PREFIX_SIZE;
    uint32_t index;

    meiso_runtime_private_put_u64(output + MEISO_RUNTIME_WIRE_HEADER_SIZE, runtime->generation[bank]);
    meiso_runtime_private_put_u32(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U,
        runtime->endpoint_count[bank]
    );
    meiso_runtime_private_put_u32(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 12U,
        runtime->route_count[bank]
    );
    for (index = 0U; index < runtime->endpoint_count[bank]; ++index) {
        meiso_runtime_private_put_endpoint_id(output + cursor, endpoints[index].id);
        meiso_runtime_private_put_u64(output + cursor + 16U, endpoints[index].incarnation);
        meiso_runtime_private_put_schema(output + cursor + 24U, endpoints[index].capability_schema);
        meiso_runtime_private_put_u32(output + cursor + 44U, endpoints[index].capability_size);
        cursor += MEISO_RUNTIME_ENDPOINT_RECORD_SIZE;
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

        meiso_runtime_private_put_endpoint_id(output + cursor, route->endpoint_id);
        meiso_runtime_private_put_u64(output + cursor + 16U, route->endpoint_incarnation);
        meiso_runtime_private_put_resource_id(output + cursor + 24U, route->resource.id);
        output[cursor + 40U] = route->resource.kind;
        output[cursor + 41U] = route->resource.owner;
        output[cursor + 42U] = route->facet;
        output[cursor + 43U] = 0U;
        meiso_runtime_private_put_schema(output + cursor + 44U, route->resource.schema);
        meiso_runtime_private_put_u64(
            output + cursor + 64U,
            (uint64_t)route->resource.max_payload_size
        );
        if (runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4) {
            output[cursor + 72U] = deliveries[index];
            memset(output + cursor + 73U, 0, 7U);
            cursor += MEISO_RUNTIME_ROUTE_V4_RECORD_SIZE;
        } else {
            cursor += MEISO_RUNTIME_ROUTE_RECORD_SIZE;
        }
    }
    meiso_runtime_private_encode_header(runtime, output, MEISO_RUNTIME_WIRE_CATALOG, (uint32_t)cursor);
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
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_mutable(runtime);
    const uint8_t bank = runtime != NULL ? meiso_runtime_private_staging_bank(runtime) : 0U;

    if (status != MEISO_RUNTIME_OK || runtime == NULL) {
        return status != MEISO_RUNTIME_OK ? status :
            MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    meiso_runtime_private_clear_pending_state_receive(runtime);
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
        runtime, generation, endpoints, endpoint_count, routes, NULL,
        route_count
    );
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    status = meiso_runtime_private_submit_bytes(runtime, encode_catalog(runtime, bank));
    if (status != MEISO_RUNTIME_OK) {
        runtime->staged_present = false;
        meiso_runtime_private_clear_catalog_bank(runtime, bank);
        return status;
    }
    runtime->highest_catalog_generation = generation;
    runtime->pending_catalog_generation = generation;
    runtime->catalog_decision_pending = true;
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_v4_offer_catalog(
    MeisoRuntime *runtime,
    uint64_t generation,
    const MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_count,
    const MeisoRuntimeV4RouteDescriptor *routes,
    size_t route_count
)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_mutable(runtime);
    const uint8_t bank = runtime != NULL ? meiso_runtime_private_staging_bank(runtime) : 0U;

    if (status != MEISO_RUNTIME_OK || runtime == NULL) {
        return status != MEISO_RUNTIME_OK ? status :
            MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (runtime->wire_version != MEISO_RUNTIME_WIRE_VERSION_V4) {
        return MEISO_RUNTIME_WIRE_VERSION_MISMATCH;
    }
    meiso_runtime_private_clear_pending_state_receive(runtime);
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
        runtime, generation, endpoints, endpoint_count, NULL, routes,
        route_count
    );
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    status = meiso_runtime_private_submit_bytes(runtime, encode_catalog(runtime, bank));
    if (status != MEISO_RUNTIME_OK) {
        runtime->staged_present = false;
        meiso_runtime_private_clear_catalog_bank(runtime, bank);
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
        status == MEISO_RUNTIME_DELIVERY_MISMATCH ||
        status == MEISO_RUNTIME_OWNER_MISMATCH ||
        status == MEISO_RUNTIME_PAYLOAD_CAPACITY;
}

MeisoRuntimeStatus meiso_runtime_private_decode_catalog_to_staging(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    uint64_t *out_generation
)
{
    const uint8_t *bytes = frame.data;
    const uint8_t bank = meiso_runtime_private_staging_bank(runtime);
    MeisoRuntimeEndpointSlot *endpoint_output;
    MeisoRuntimeRouteSlot *route_output;
    uint8_t *capability_output;
    uint64_t generation;
    uint32_t endpoint_count;
    uint32_t route_count;
    size_t cursor;
    const size_t route_record_size =
        runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4 ?
            MEISO_RUNTIME_ROUTE_V4_RECORD_SIZE : MEISO_RUNTIME_ROUTE_RECORD_SIZE;
    size_t capability_total = 0U;
    size_t route_bytes;
    uint32_t index;
    bool local_capacity;
    MeisoRuntimeStatus status = MEISO_RUNTIME_OK;

    if (out_generation == NULL ||
        frame.size < MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_CATALOG_PREFIX_SIZE) {
        return MEISO_RUNTIME_MALFORMED;
    }
    generation = meiso_runtime_private_get_u64(bytes + MEISO_RUNTIME_WIRE_HEADER_SIZE);
    endpoint_count = meiso_runtime_private_get_u32(bytes + MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U);
    route_count = meiso_runtime_private_get_u32(bytes + MEISO_RUNTIME_WIRE_HEADER_SIZE + 12U);
    if (generation == 0U || endpoint_count == 0U || route_count == 0U) {
        return MEISO_RUNTIME_MALFORMED;
    }
    cursor = MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_CATALOG_PREFIX_SIZE;
    for (index = 0U; index < endpoint_count; ++index) {
        uint32_t capability_size;

        if (cursor > frame.size ||
            frame.size - cursor < MEISO_RUNTIME_ENDPOINT_RECORD_SIZE) {
            return MEISO_RUNTIME_MALFORMED;
        }
        capability_size = meiso_runtime_private_get_u32(bytes + cursor + 44U);
        cursor += MEISO_RUNTIME_ENDPOINT_RECORD_SIZE;
        if (capability_size > frame.size - cursor ||
            !meiso_runtime_private_checked_add(capability_total, capability_size, &capability_total)) {
            return MEISO_RUNTIME_MALFORMED;
        }
        cursor += capability_size;
    }
    if (!meiso_runtime_private_checked_multiply(route_count, route_record_size, &route_bytes) ||
        cursor > frame.size || route_bytes != frame.size - cursor) {
        return MEISO_RUNTIME_MALFORMED;
    }
    for (index = 0U; index < route_count; ++index) {
        const uint8_t *record = bytes + cursor +
            (size_t)index * route_record_size;
        if (record[43U] != 0U ||
            (runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4 &&
             (record[72U] != MEISO_RUNTIME_DELIVERY_INLINE &&
              record[72U] != MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA)) ||
            (runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4 &&
             !meiso_runtime_private_bytes_zero(record + 73U, 7U))) {
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
        meiso_runtime_private_clear_catalog_bank(runtime, bank);
        return MEISO_RUNTIME_CAPACITY;
    }

    meiso_runtime_private_clear_catalog_bank(runtime, bank);
    endpoint_output = meiso_runtime_private_endpoint_slots(runtime, bank);
    route_output = meiso_runtime_private_route_slots(runtime, bank);
    capability_output = meiso_runtime_private_capability_bytes(runtime, bank);
    cursor = MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_CATALOG_PREFIX_SIZE;
    capability_total = 0U;
    for (index = 0U; index < endpoint_count; ++index) {
        const uint8_t *record = bytes + cursor;
        const uint32_t capability_size = meiso_runtime_private_get_u32(record + 44U);
        uint32_t earlier;

        endpoint_output[index].id = meiso_runtime_private_get_endpoint_id(record);
        endpoint_output[index].incarnation = meiso_runtime_private_get_u64(record + 16U);
        endpoint_output[index].capability_schema = meiso_runtime_private_get_schema(record + 24U);
        endpoint_output[index].capability_offset = (uint32_t)capability_total;
        endpoint_output[index].capability_size = capability_size;
        if (!meiso_endpoint_id_valid(endpoint_output[index].id) ||
            endpoint_output[index].incarnation == 0U ||
            !meiso_runtime_private_schema_valid(endpoint_output[index].capability_schema)) {
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
        cursor += MEISO_RUNTIME_ENDPOINT_RECORD_SIZE;
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
            const MeisoRuntimeDeliveryClass delivery =
                runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4 ?
                    record[72U] : MEISO_RUNTIME_DELIVERY_INLINE;
            const uint64_t maximum = meiso_runtime_private_get_u64(record + 64U);
            const int endpoint_index = meiso_runtime_private_endpoint_index_in_bank(
                runtime, bank, meiso_runtime_private_get_endpoint_id(record)
            );
            uint32_t earlier;

            memset(&route, 0, sizeof(route));
            route.endpoint_id = meiso_runtime_private_get_endpoint_id(record);
            route.endpoint_incarnation = meiso_runtime_private_get_u64(record + 16U);
            route.resource.id = meiso_runtime_private_get_resource_id(record + 24U);
            route.resource.kind = record[40U];
            route.resource.owner = record[41U];
            route.facet = record[42U];
            route.resource.schema = meiso_runtime_private_get_schema(record + 44U);
            if (sizeof(size_t) < sizeof(uint64_t) &&
                maximum > (uint64_t)SIZE_MAX) {
                status = MEISO_RUNTIME_PAYLOAD_CAPACITY;
                break;
            }
            route.resource.max_payload_size = (size_t)maximum;
            status = route_input_validate_delivery(runtime, &route, delivery);
            if (status != MEISO_RUNTIME_OK) {
                break;
            }
            if (endpoint_index < 0) {
                status = MEISO_RUNTIME_UNKNOWN_ENDPOINT;
                break;
            }
            if (meiso_runtime_private_endpoint_slots(runtime, bank)[(size_t)endpoint_index].incarnation !=
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
            if (runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4) {
                meiso_runtime_private_delivery_classes(runtime, bank)[index] = delivery;
            }
            runtime->route_count[bank] += 1U;
            cursor += route_record_size;
        }
    }
    if (status == MEISO_RUNTIME_OK) {
        status = validate_catalog_semantics(runtime, bank);
    }
    if (status == MEISO_RUNTIME_OK) {
        status = validate_identity_ledger_capacity(runtime, bank);
    }
    if (status != MEISO_RUNTIME_OK) {
        meiso_runtime_private_clear_catalog_bank(runtime, bank);
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

void meiso_runtime_private_commit_staged(MeisoRuntime *runtime)
{
    const uint8_t new_bank = meiso_runtime_private_staging_bank(runtime);
    const uint8_t retired_bank = runtime->active_bank;
    MeisoRuntimeEndpointSlot *next = meiso_runtime_private_endpoint_slots(runtime, new_bank);
    MeisoRuntimeIdentitySlot *ledger = meiso_runtime_private_identity_slots(runtime);
    uint32_t next_index;

    for (next_index = 0U;
         next_index < runtime->endpoint_count[new_bank];
         ++next_index) {
        int ledger_index = meiso_runtime_private_identity_pair_index(
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
    meiso_runtime_private_clear_blob_transfer(runtime);
    runtime->active_bank = new_bank;
    runtime->active_present = true;
    runtime->staged_present = false;
    if (runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4) {
        meiso_runtime_private_clear_catalog_bank(runtime, retired_bank);
    }
    meiso_runtime_private_clear_pending_state_receive(runtime);
    meiso_runtime_private_clear_catalog_decision(runtime);
}

static MeisoRuntimeStatus submit_catalog_decision(
    MeisoRuntime *runtime,
    uint8_t type,
    MeisoRuntimeCatalogRejectReason reason
)
{
    uint8_t *output = meiso_runtime_private_tx_bytes(runtime);
    size_t size;

    meiso_runtime_private_put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE,
        runtime->pending_catalog_generation
    );
    if (type == MEISO_RUNTIME_WIRE_CATALOG_ACCEPT) {
        size = MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_CATALOG_ACCEPT_SIZE;
    } else {
        output[MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U] = reason;
        output[MEISO_RUNTIME_WIRE_HEADER_SIZE + 9U] = 0U;
        output[MEISO_RUNTIME_WIRE_HEADER_SIZE + 10U] = 0U;
        output[MEISO_RUNTIME_WIRE_HEADER_SIZE + 11U] = 0U;
        size = MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_CATALOG_REJECT_SIZE;
    }
    meiso_runtime_private_encode_header(runtime, output, type, (uint32_t)size);
    return meiso_runtime_private_submit_bytes(runtime, size);
}

MeisoRuntimeStatus meiso_runtime_accept_catalog(MeisoRuntime *runtime)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_mutable(runtime);

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    meiso_runtime_private_clear_pending_state_receive(runtime);
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (!runtime->catalog_decision_pending || !runtime->staged_present) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    status = submit_catalog_decision(runtime, MEISO_RUNTIME_WIRE_CATALOG_ACCEPT, 0U);
    if (status == MEISO_RUNTIME_OK) {
        meiso_runtime_private_commit_staged(runtime);
    }
    return status;
}

MeisoRuntimeStatus meiso_runtime_reject_catalog(
    MeisoRuntime *runtime,
    MeisoRuntimeCatalogRejectReason reason
)
{
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_mutable(runtime);
    const uint8_t bank = runtime != NULL ? meiso_runtime_private_staging_bank(runtime) : 0U;

    if (status != MEISO_RUNTIME_OK || runtime == NULL) {
        return status != MEISO_RUNTIME_OK ? status :
            MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    meiso_runtime_private_clear_pending_state_receive(runtime);
    if (runtime->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (!meiso_runtime_private_reject_reason_valid(reason)) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->catalog_decision_pending) {
        return MEISO_RUNTIME_NO_CATALOG;
    }
    status = submit_catalog_decision(runtime, MEISO_RUNTIME_WIRE_CATALOG_REJECT, reason);
    if (status == MEISO_RUNTIME_OK) {
        runtime->staged_present = false;
        meiso_runtime_private_clear_catalog_bank(runtime, bank);
        meiso_runtime_private_clear_catalog_decision(runtime);
    }
    return status;
}
