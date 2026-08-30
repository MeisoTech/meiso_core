#include "meiso_runtime_private.h"

#include <string.h>

/* State carriage, pending receive obligations and production transitions. */

MeisoOwnerRole meiso_runtime_private_owner_role_for_session(MeisoSessionRole role)
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
    value->data_active = meiso_runtime_private_actual_data_active(actual);
}

static bool production_transition_valid(
    const MeisoRuntime *runtime,
    uint8_t bank,
    size_t endpoint_index,
    const MeisoRuntimeActualState *actual
)
{
    const MeisoRuntimeEndpointSlot *endpoint =
        &meiso_runtime_private_endpoint_slots_const(runtime, bank)[endpoint_index];
    const bool next_active = meiso_runtime_private_actual_data_active(actual);
    const bool applied_bound = !actual->applied_desired.present ||
        meiso_runtime_private_desired_reference_equal(
            actual->applied_desired, endpoint->desired
        ) || meiso_runtime_private_desired_reference_equal(
            actual->applied_desired,
            endpoint->production.applied_desired
        );

    if (actual->profile.size >
            runtime->max_actual_profile_bytes_per_endpoint ||
        !meiso_runtime_private_desired_reference_equal(
            actual->observed_desired, endpoint->desired
        ) || !applied_bound) {
        return false;
    }
    if (!next_active) {
        return true;
    }
    if (endpoint->production.data_active &&
        actual->production_epoch == endpoint->production.production_epoch) {
        return meiso_runtime_private_desired_reference_equal(
                actual->applied_desired,
                endpoint->production.applied_desired
            ) && actual->profile.size == endpoint->actual_profile_size &&
            (actual->profile.size == 0U || memcmp(
                actual->profile.data,
                meiso_runtime_private_actual_profile_bytes_const(runtime, bank, endpoint_index),
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
    MeisoRuntimeEndpointSlot *endpoint = &meiso_runtime_private_endpoint_slots(runtime, bank)[endpoint_index];
    const int ledger_index = meiso_runtime_private_identity_pair_index(
        runtime, endpoint->id, endpoint->incarnation
    );
    uint64_t high_water = endpoint->production.production_epoch_high_water;

    if (meiso_runtime_private_actual_data_active(actual) && actual->production_epoch > high_water) {
        high_water = actual->production_epoch;
    }
    production_snapshot_from_actual(&endpoint->production, actual, high_water);
    if (ledger_index >= 0 &&
        meiso_runtime_private_identity_slots(runtime)[(size_t)ledger_index].current) {
        meiso_runtime_private_identity_slots(runtime)[(size_t)ledger_index]
            .production_epoch_high_water = high_water;
    }
    endpoint->actual_profile_size = (uint32_t)actual->profile.size;
    if (actual->profile.size != 0U) {
        memmove(
            meiso_runtime_private_actual_profile_bytes(runtime, bank, endpoint_index),
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
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_mutable(runtime);
    MeisoRuntimeRouteSlot *route;
    MeisoRuntimeEndpointSlot *endpoint;
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
    meiso_runtime_private_clear_pending_state_receive(runtime);
    status = meiso_runtime_private_resolve_active_route(runtime, route_key, &route, &endpoint);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    endpoint_index = (size_t)(
        endpoint - meiso_runtime_private_endpoint_slots(runtime, runtime->active_bank)
    );
    if (route->value.resource.kind != MEISO_RESOURCE_KIND_STATE ||
        (route->value.facet != MEISO_RUNTIME_FACET_DESIRED &&
         route->value.facet != MEISO_RUNTIME_FACET_ACTUAL)) {
        return MEISO_RUNTIME_UNSUPPORTED_KIND;
    }
    if (!state_offer_valid(offer, &route->value.resource)) {
        return MEISO_RUNTIME_STATE_INVALID;
    }
    if (meiso_runtime_private_ranges_overlap(
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
    if (meiso_runtime_private_owner_role_for_session(runtime->local_role) !=
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
            ) || !meiso_runtime_private_actual_state_for_route_valid(&actual)) {
            return MEISO_RUNTIME_STATE_INVALID;
        }
        if (!production_transition_valid(
                runtime, runtime->active_bank, endpoint_index, &actual
            )) {
            return MEISO_RUNTIME_STATE_INVALID;
        }
    }
    if (!meiso_runtime_private_checked_add(
            MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_STATE_OFFER_PREFIX_SIZE,
            offer->state.payload.size,
            &size
        ) || size > runtime->limits.max_runtime_payload_bytes ||
        size > UINT32_MAX) {
        return MEISO_RUNTIME_PAYLOAD_CAPACITY;
    }
    output = meiso_runtime_private_tx_bytes(runtime);
    meiso_runtime_private_encode_route_key(output + MEISO_RUNTIME_WIRE_HEADER_SIZE, route_key);
    meiso_runtime_private_put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 56U,
        offer->generation.value
    );
    output[MEISO_RUNTIME_WIRE_HEADER_SIZE + 64U] = offer->state.owner.role;
    memset(output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 65U, 0, 7U);
    meiso_runtime_private_put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 72U,
        offer->state.owner.incarnation
    );
    meiso_runtime_private_put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 80U,
        offer->state.version
    );
    meiso_runtime_private_put_u32(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 88U,
        (uint32_t)offer->state.payload.size
    );
    memset(output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 92U, 0, 4U);
    if (offer->state.payload.size != 0U) {
        memmove(
            output + MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_STATE_OFFER_PREFIX_SIZE,
            offer->state.payload.data,
            offer->state.payload.size
        );
    }
    if (route->value.facet == MEISO_RUNTIME_FACET_ACTUAL &&
        !meiso_runtime_actual_state_decode(
            (MeisoByteView){
                output + MEISO_RUNTIME_WIRE_HEADER_SIZE +
                    MEISO_RUNTIME_STATE_OFFER_PREFIX_SIZE,
                offer->state.payload.size
            },
            &committed_actual
        )) {
        return MEISO_RUNTIME_STATE_INVALID;
    }
    meiso_runtime_private_encode_header(runtime, output, MEISO_RUNTIME_WIRE_STATE_OFFER, (uint32_t)size);
    status = meiso_runtime_private_submit_bytes(runtime, size);
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
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_mutable(runtime);
    MeisoRuntimeRouteSlot *route;
    MeisoRuntimeEndpointSlot *endpoint;
    uint8_t *output;
    const size_t size = MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_STATE_ACCEPT_SIZE;

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    meiso_runtime_private_clear_pending_state_receive(runtime);
    status = meiso_runtime_private_resolve_active_route(runtime, route_key, &route, &endpoint);
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
    if (meiso_runtime_private_owner_role_for_session(runtime->local_role) ==
        route->value.resource.owner) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    output = meiso_runtime_private_tx_bytes(runtime);
    meiso_runtime_private_encode_route_key(output + MEISO_RUNTIME_WIRE_HEADER_SIZE, route_key);
    meiso_runtime_private_put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 56U,
        generation.value
    );
    output[MEISO_RUNTIME_WIRE_HEADER_SIZE + 64U] = owner.role;
    memset(output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 65U, 0, 7U);
    meiso_runtime_private_put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 72U,
        owner.incarnation
    );
    meiso_runtime_private_put_u64(output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 80U, version);
    meiso_runtime_private_encode_header(runtime, output, MEISO_RUNTIME_WIRE_STATE_ACCEPT, (uint32_t)size);
    return meiso_runtime_private_submit_bytes(runtime, size);
}
MeisoRuntimeStatus meiso_runtime_private_receive_state_offer(
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
    MeisoRuntimeRouteSlot *route;
    MeisoRuntimeEndpointSlot *endpoint;
    uint32_t payload_size;
    MeisoRuntimeStatus status;

    if (frame.size <
            MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_STATE_OFFER_PREFIX_SIZE ||
        !meiso_runtime_private_decode_route_key(body, &key) ||
        !meiso_runtime_private_bytes_zero(body + 65U, 7U) ||
        !meiso_runtime_private_bytes_zero(body + 92U, 4U)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    payload_size = meiso_runtime_private_get_u32(body + 88U);
    if (payload_size != frame.size -
            (MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_STATE_OFFER_PREFIX_SIZE)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    offer.generation.value = meiso_runtime_private_get_u64(body + 56U);
    offer.state.owner.role = body[64U];
    offer.state.owner.incarnation = meiso_runtime_private_get_u64(body + 72U);
    offer.state.version = meiso_runtime_private_get_u64(body + 80U);
    offer.state.payload.data = body + MEISO_RUNTIME_STATE_OFFER_PREFIX_SIZE;
    offer.state.payload.size = payload_size;

    status = meiso_runtime_private_resolve_active_route(runtime, key, &route, &endpoint);
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
    if (meiso_runtime_private_owner_role_for_session(runtime->local_role) ==
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
                meiso_runtime_private_pending_desired_payload_bytes(runtime),
                offer.state.payload.data,
                offer.state.payload.size
            );
        }
        runtime->pending_desired_receive = true;
    } else {
        if (!meiso_runtime_actual_state_decode(
                offer.state.payload, &actual
            ) || !meiso_runtime_private_actual_state_for_route_valid(&actual)) {
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
                meiso_runtime_private_pending_actual_profile_bytes(runtime),
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

MeisoRuntimeStatus meiso_runtime_private_receive_state_accept(
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
    MeisoRuntimeRouteSlot *route;
    MeisoRuntimeEndpointSlot *endpoint;
    MeisoRuntimeStatus status;

    if (frame.size != MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_STATE_ACCEPT_SIZE ||
        !meiso_runtime_private_decode_route_key(body, &key) || !meiso_runtime_private_bytes_zero(body + 65U, 7U)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    generation.value = meiso_runtime_private_get_u64(body + 56U);
    owner.role = body[64U];
    owner.incarnation = meiso_runtime_private_get_u64(body + 72U);
    version = meiso_runtime_private_get_u64(body + 80U);
    if (!meiso_delivery_generation_valid(generation) ||
        !meiso_owner_scope_valid(owner) || version == 0U) {
        return MEISO_RUNTIME_STATE_INVALID;
    }
    status = meiso_runtime_private_resolve_active_route(runtime, key, &route, &endpoint);
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
    if (meiso_runtime_private_owner_role_for_session(runtime->local_role) !=
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
static bool production_snapshot_equal(
    const MeisoRuntimeProductionSnapshot *lhs,
    const MeisoRuntimeProductionSnapshot *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        lhs->actual_present == rhs->actual_present &&
        meiso_runtime_private_desired_reference_equal(
            lhs->observed_desired, rhs->observed_desired
        ) &&
        meiso_runtime_private_desired_reference_equal(lhs->applied_desired, rhs->applied_desired) &&
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
    MeisoRuntimeStatus status = meiso_runtime_private_check_bound_mutable(runtime);
    MeisoRuntimeRouteSlot *route;
    MeisoRuntimeEndpointSlot *endpoint;
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
        meiso_runtime_private_clear_pending_state_receive(runtime);
        return MEISO_RUNTIME_CONFLICT;
    }
    status = meiso_runtime_private_resolve_active_route(runtime, route_key, &route, &endpoint);
    if (status != MEISO_RUNTIME_OK) {
        meiso_runtime_private_clear_pending_state_receive(runtime);
        return status;
    }
    matches = meiso_runtime_private_route_key_equal(
            route_key, runtime->pending_actual_route
        ) && offer->generation.value ==
            runtime->pending_actual_delivery.value &&
        meiso_owner_scope_equal(
            offer->state.owner, runtime->pending_actual_owner
        ) && offer->state.version == runtime->pending_actual_version;
    if (!matches || route->value.resource.kind != MEISO_RESOURCE_KIND_STATE ||
        !state_offer_valid(offer, &route->value.resource)) {
        meiso_runtime_private_clear_pending_state_receive(runtime);
        return !matches ? MEISO_RUNTIME_CONFLICT :
            MEISO_RUNTIME_STATE_INVALID;
    }
    endpoint_index = (size_t)(
        endpoint - meiso_runtime_private_endpoint_slots(runtime, runtime->active_bank)
    );
    if (pending_desired) {
        matches = offer->state.payload.size ==
                runtime->pending_desired_payload_size &&
            (offer->state.payload.size == 0U || memcmp(
                offer->state.payload.data,
                meiso_runtime_private_pending_desired_payload_bytes(runtime),
                offer->state.payload.size
            ) == 0);
        if (!matches) {
            meiso_runtime_private_clear_pending_state_receive(runtime);
            return MEISO_RUNTIME_CONFLICT;
        }
        if (pending_actual ||
            route->value.facet != MEISO_RUNTIME_FACET_DESIRED ||
            !meiso_runtime_desired_state_decode(
                offer->state.payload, &desired
            )) {
            meiso_runtime_private_clear_pending_state_receive(runtime);
            return MEISO_RUNTIME_STATE_INVALID;
        }
        meiso_runtime_private_clear_pending_state_receive(runtime);
        if (receive_status == MEISO_STATE_RECEIVE_NEW) {
            endpoint->desired.present = true;
            endpoint->desired.owner = offer->state.owner;
            endpoint->desired.version = offer->state.version;
        }
        return MEISO_RUNTIME_OK;
    }
    if (route->value.facet != MEISO_RUNTIME_FACET_ACTUAL ||
        !meiso_runtime_actual_state_decode(offer->state.payload, &actual) ||
        !meiso_runtime_private_actual_state_for_route_valid(&actual)) {
        meiso_runtime_private_clear_pending_state_receive(runtime);
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
            meiso_runtime_private_pending_actual_profile_bytes(runtime),
            actual.profile.size
        ) == 0);
    meiso_runtime_private_clear_pending_state_receive(runtime);
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
