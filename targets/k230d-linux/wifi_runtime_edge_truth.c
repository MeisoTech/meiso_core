#define _POSIX_C_SOURCE 200809L

#include "meiso_k230d_wifi_runtime_edge_private.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/*
 * Owns catalog activation and Desired-to-Actual truth transitions. Pending
 * accepts, Actual publications, and production cuts remain serialized through
 * the single Session outbound slot.
 */

bool meiso_k230d_wifi_runtime_edge_private_session_tx_idle(const K230dWifiRuntimeEdge *edge)
{
    MeisoByteView frame;
    return edge->session != NULL &&
        !meiso_session_next_outbound(edge->session, &frame);
}

bool meiso_k230d_wifi_runtime_edge_private_try_offer_catalog(K230dWifiRuntimeEdge *edge)
{
    MeisoRuntimeStatus status;

    if (!edge->runtime_initialized || edge->catalog_offered ||
        !meiso_k230d_wifi_runtime_edge_private_session_tx_idle(edge)) {
        return true;
    }
    status = meiso_runtime_offer_catalog(
        edge->runtime,
        K230D_WIFI_RUNTIME_EDGE_CATALOG_GENERATION,
        &edge->endpoint,
        1U,
        edge->routes,
        MEISO_WIFI_TELEMETRY_RESOURCE_COUNT
    );
    if (status == MEISO_RUNTIME_TX_BUSY) {
        return true;
    }
    if (status != MEISO_RUNTIME_OK) {
        return false;
    }
    edge->catalog_offered = true;
    (void)printf(
        "{\"type\":\"runtime\",\"phase\":\"catalog\","
        "\"status\":\"OFFERED\",\"generation\":%u}\n",
        K230D_WIFI_RUNTIME_EDGE_CATALOG_GENERATION
    );
    return true;
}

static MeisoRuntimeDesiredReference desired_reference(MeisoStateView view)
{
    const MeisoRuntimeDesiredReference reference = {
        true,
        view.owner,
        view.version
    };
    return reference;
}

static K230dWifiRuntimeEdgeActualTransition stopped_transition(
    MeisoRuntimeDesiredReference desired,
    MeisoRuntimeAvailability availability
)
{
    K230dWifiRuntimeEdgeActualTransition value;
    memset(&value, 0, sizeof(value));
    value.present = true;
    value.observed = desired;
    value.applied = desired;
    value.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    value.availability = availability;
    value.production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
    value.reason = MEISO_RUNTIME_PRODUCTION_REASON_DISABLED;
    return value;
}

static K230dWifiRuntimeEdgeActualTransition starting_transition(
    const K230dWifiRuntimeEdge *edge,
    MeisoRuntimeDesiredReference desired,
    MeisoRuntimeAvailability availability,
    MeisoWifiTelemetryDesiredParameters parameters
)
{
    K230dWifiRuntimeEdgeActualTransition value;

    memset(&value, 0, sizeof(value));
    value.present = true;
    value.observed = desired;
    value.applied = edge->production_applied_desired;
    value.apply_result = MEISO_RUNTIME_APPLY_PENDING;
    value.availability = availability;
    value.production_status = MEISO_RUNTIME_PRODUCTION_STARTING;
    value.reason = MEISO_RUNTIME_PRODUCTION_REASON_APPLYING;
    value.follow_up = K230D_WIFI_RUNTIME_EDGE_ACTUAL_FOLLOW_UP_RUNNING;
    value.follow_up_parameters = parameters;
    return value;
}

static bool live_pending_transition(
    const K230dWifiRuntimeEdge *edge,
    MeisoRuntimeDesiredReference desired,
    MeisoRuntimeProductionStatus status,
    K230dWifiRuntimeEdgeActualFollowUp follow_up,
    MeisoWifiTelemetryDesiredParameters follow_up_parameters,
    K230dWifiRuntimeEdgeActualTransition *out_transition
)
{
    K230dWifiRuntimeEdgeActualTransition value;

    if (edge == NULL || out_transition == NULL ||
        !edge->production_applied_desired.present ||
        edge->active_production_epoch == 0U ||
        edge->production_effective.selected_fields == 0U ||
        edge->production_effective.sample_period_ms == 0U ||
        (status != MEISO_RUNTIME_PRODUCTION_RUNNING &&
         status != MEISO_RUNTIME_PRODUCTION_STOPPING)) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.present = true;
    value.observed = desired;
    value.applied = edge->production_applied_desired;
    value.apply_result = MEISO_RUNTIME_APPLY_PENDING;
    value.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    value.effective_enabled = true;
    value.production_status = status;
    value.reason = MEISO_RUNTIME_PRODUCTION_REASON_APPLYING;
    value.production_epoch = edge->active_production_epoch;
    value.effective = edge->production_effective;
    value.follow_up = follow_up;
    value.follow_up_parameters = follow_up_parameters;
    *out_transition = value;
    return true;
}

static K230dWifiRuntimeEdgeActualTransition rejected_transition(
    MeisoRuntimeDesiredReference desired,
    MeisoRuntimeAvailability availability
)
{
    K230dWifiRuntimeEdgeActualTransition value;
    memset(&value, 0, sizeof(value));
    value.present = true;
    value.observed = desired;
    value.apply_result = MEISO_RUNTIME_APPLY_REJECTED;
    value.availability = availability;
    value.production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
    value.reason = MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER;
    return value;
}

static K230dWifiRuntimeEdgeActualTransition unavailable_transition(
    MeisoRuntimeDesiredReference desired
)
{
    K230dWifiRuntimeEdgeActualTransition value;
    memset(&value, 0, sizeof(value));
    value.present = true;
    value.observed = desired;
    value.apply_result = MEISO_RUNTIME_APPLY_FAILED;
    value.availability = MEISO_RUNTIME_AVAILABILITY_UNAVAILABLE;
    value.production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
    value.reason = MEISO_RUNTIME_PRODUCTION_REASON_UNAVAILABLE;
    return value;
}

static bool running_transition(
    K230dWifiRuntimeEdge *edge,
    MeisoRuntimeDesiredReference desired,
    MeisoWifiTelemetryDesiredParameters parameters,
    K230dWifiRuntimeEdgeActualTransition *out_transition
)
{
    K230dWifiRuntimeEdgeActualTransition value;

    if (out_transition == NULL ||
        edge->production_epoch_high_water == UINT64_MAX) {
        return false;
    }
    edge->production_epoch_high_water += 1U;
    memset(&value, 0, sizeof(value));
    value.present = true;
    value.observed = desired;
    value.applied = desired;
    value.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    value.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    value.effective_enabled = true;
    value.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING;
    value.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    value.production_epoch = edge->production_epoch_high_water;
    value.effective.selected_fields = parameters.selected_fields;
    value.effective.sample_period_ms = parameters.sample_period_ms;
    *out_transition = value;
    return true;
}

static bool request_actual(
    K230dWifiRuntimeEdge *edge,
    K230dWifiRuntimeEdgeActualTransition transition
)
{
    if (edge == NULL || !transition.present) {
        return false;
    }
    if (edge->actual_in_flight || edge->actual_outbound.published) {
        if (edge->actual_next.present) {
            return false;
        }
        edge->actual_next = transition;
    } else if (edge->actual_outbound.present) {
        if (edge->actual_next.present) {
            return false;
        }
        edge->actual_next = transition;
    } else {
        edge->actual_outbound = transition;
    }
    return true;
}

void meiso_k230d_wifi_runtime_edge_private_mark_source_continuity_lost(
    K230dWifiRuntimeEdge *edge,
    const char *cause
)
{
    if (!edge->source_continuity_lost) {
        edge->source_continuity_lost = true;
        edge->production_active = false;
        (void)printf(
            "{\"type\":\"source\",\"phase\":\"continuity\","
            "\"status\":\"LOST\",\"cause\":\"%s\","
            "\"action\":\"SESSION_SHUTDOWN\"}\n",
            cause
        );
    }
}

static bool observe_current_availability(
    K230dWifiRuntimeEdge *edge,
    MeisoRuntimeAvailability *out_availability
)
{
    MeisoWifiTelemetryFieldMask fields = 0U;
    const MeisoWifiLinuxAdapterStatus status =
        meiso_wifi_linux_adapter_probe(&edge->wifi, &fields);

    if (out_availability == NULL) {
        return false;
    }
    if (status == MEISO_WIFI_LINUX_ADAPTER_OK) {
        *out_availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
        return true;
    }
    if (status != MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE) {
        return false;
    }
    *out_availability = MEISO_RUNTIME_AVAILABILITY_UNAVAILABLE;
    if (edge->production_ever_started) {
        meiso_k230d_wifi_runtime_edge_private_mark_source_continuity_lost(
            edge, "SOURCE_UNAVAILABLE"
        );
    }
    return true;
}

static bool apply_current_desired(K230dWifiRuntimeEdge *edge, uint64_t now_ns)
{
    MeisoStateView view;
    MeisoRuntimeDesiredState desired;
    MeisoWifiTelemetryDesiredParameters parameters;
    MeisoRuntimeDesiredReference reference;
    MeisoRuntimeAvailability availability;
    K230dWifiRuntimeEdgeActualTransition transition;
    MeisoWifiTelemetryCodecStatus decoded;

    if (!meiso_state_replica_current(edge->desired_replica, &view) ||
        !meiso_runtime_desired_state_decode(view.payload, &desired)) {
        return false;
    }
    decoded = meiso_wifi_telemetry_desired_parameters_decode(
        desired.profile, &parameters
    );
    reference = desired_reference(view);
    edge->current_desired = reference;
    edge->desired_enabled = desired.enabled;
    edge->desired_profile_valid =
        decoded == MEISO_WIFI_TELEMETRY_CODEC_OK &&
        meiso_wifi_telemetry_parameters_supported(
            &edge->capability, &parameters
        );
    if (!observe_current_availability(edge, &availability)) {
        return false;
    }
    if (edge->source_continuity_lost) {
        return true;
    }
    if (!edge->desired_profile_valid) {
        return request_actual(
            edge,
            rejected_transition(reference, availability)
        );
    }
    edge->current_parameters = parameters;
    if (!desired.enabled) {
        if (edge->active_production_epoch != 0U) {
            memset(&transition, 0, sizeof(transition));
            if (!live_pending_transition(
                    edge,
                    reference,
                    MEISO_RUNTIME_PRODUCTION_STOPPING,
                    K230D_WIFI_RUNTIME_EDGE_ACTUAL_FOLLOW_UP_STOPPED,
                    parameters,
                    &transition
                )) {
                return false;
            }
            /* The public source is cut here. Any sample already materialized
             * remains pending and is sent before the STOPPING actual. */
            edge->production_active = false;
            return request_actual(edge, transition);
        }
        return request_actual(
            edge, stopped_transition(reference, availability)
        );
    }
    if (availability != MEISO_RUNTIME_AVAILABILITY_PRESENT) {
        if (!request_actual(edge, unavailable_transition(reference))) {
            return false;
        }
        edge->next_probe_ns = meiso_k230d_wifi_runtime_edge_private_deadline_after_ms(
            now_ns, parameters.sample_period_ms
        );
        return true;
    }
    if (edge->active_production_epoch != 0U) {
        memset(&transition, 0, sizeof(transition));
        if (!live_pending_transition(
                edge,
                reference,
                MEISO_RUNTIME_PRODUCTION_RUNNING,
                K230D_WIFI_RUNTIME_EDGE_ACTUAL_FOLLOW_UP_RUNNING,
                parameters,
                &transition
            )) {
            return false;
        }
    } else {
        transition = starting_transition(
            edge, reference, availability, parameters
        );
    }
    return request_actual(edge, transition);
}

static bool queue_state_accept(
    K230dWifiRuntimeEdge *edge,
    MeisoRuntimeRouteKey route,
    MeisoDeliveryGeneration generation,
    MeisoOwnerScope owner,
    uint64_t version
)
{
    if (edge->pending_accept.present) {
        return edge->pending_accept.generation.value == generation.value &&
            meiso_owner_scope_equal(edge->pending_accept.owner, owner) &&
            edge->pending_accept.version == version;
    }
    edge->pending_accept.present = true;
    edge->pending_accept.route = route;
    edge->pending_accept.generation = generation;
    edge->pending_accept.owner = owner;
    edge->pending_accept.version = version;
    return true;
}

static bool handle_desired_offer(
    K230dWifiRuntimeEdge *edge,
    const MeisoRuntimeEvent *event,
    uint64_t now_ns
)
{
    MeisoStateReceiveStatus receive_status;
    MeisoRuntimeRouteKey route;
    MeisoDeliveryGeneration generation;
    MeisoOwnerScope owner;
    uint64_t version;

    if (event == NULL || event->type != MEISO_RUNTIME_EVENT_STATE_OFFER ||
        event->route.facet != MEISO_RUNTIME_FACET_DESIRED) {
        return false;
    }
    route = event->route;
    generation = event->state_offer.generation;
    owner = event->state_offer.state.owner;
    version = event->state_offer.state.version;
    receive_status = meiso_state_replica_receive(
        edge->desired_replica,
        owner,
        version,
        event->state_offer.state.payload
    );
    if (meiso_runtime_commit_state_receive(
            edge->runtime,
            route,
            &event->state_offer,
            receive_status
        ) != MEISO_RUNTIME_OK) {
        return false;
    }
    if (receive_status != MEISO_STATE_RECEIVE_NEW &&
        receive_status != MEISO_STATE_RECEIVE_DUPLICATE) {
        return false;
    }
    if (!queue_state_accept(edge, route, generation, owner, version)) {
        return false;
    }
    return receive_status != MEISO_STATE_RECEIVE_NEW ||
        apply_current_desired(edge, now_ns);
}

static bool handle_actual_accept(
    K230dWifiRuntimeEdge *edge,
    const MeisoRuntimeEvent *event
)
{
    K230dWifiRuntimeEdgeActualTransition accepted;
    K230dWifiRuntimeEdgeActualTransition follow_up;
    MeisoStateAcceptStatus status;

    if (!edge->actual_in_flight || event == NULL ||
        event->type != MEISO_RUNTIME_EVENT_STATE_ACCEPT ||
        event->route.facet != MEISO_RUNTIME_FACET_ACTUAL) {
        return false;
    }
    status = meiso_state_publisher_mark_delivery_accepted(
        edge->actual_publisher,
        event->state_accept_generation,
        event->state_accept_owner,
        event->state_accept_version
    );
    if (status != MEISO_STATE_ACCEPT_CONFIRMED) {
        return false;
    }
    accepted = edge->actual_in_flight_value;
    memset(
        &edge->actual_in_flight_value,
        0,
        sizeof(edge->actual_in_flight_value)
    );
    edge->actual_in_flight = false;
    if (accepted.follow_up != K230D_WIFI_RUNTIME_EDGE_ACTUAL_FOLLOW_UP_NONE) {
        if (edge->actual_next.present) {
            return false;
        }
        memset(&follow_up, 0, sizeof(follow_up));
        if (accepted.follow_up == K230D_WIFI_RUNTIME_EDGE_ACTUAL_FOLLOW_UP_STOPPED) {
            follow_up = stopped_transition(
                accepted.observed, accepted.availability
            );
        } else if (accepted.follow_up == K230D_WIFI_RUNTIME_EDGE_ACTUAL_FOLLOW_UP_RUNNING) {
            /* No new sample may materialize across the production cut. An
             * already-materialized old-epoch sample remains drainable. */
            edge->production_active = false;
            if (!running_transition(
                    edge,
                    accepted.observed,
                    accepted.follow_up_parameters,
                    &follow_up
                )) {
                return false;
            }
        } else {
            return false;
        }
        return request_actual(edge, follow_up);
    }
    if (edge->actual_next.present) {
        edge->actual_outbound = edge->actual_next;
        memset(&edge->actual_next, 0, sizeof(edge->actual_next));
    }
    return true;
}

bool meiso_k230d_wifi_runtime_edge_private_handle_runtime_message(
    K230dWifiRuntimeEdge *edge,
    const MeisoSessionRuntimeMessage *message,
    uint64_t now_ns
)
{
    MeisoRuntimeEvent event;
    const MeisoRuntimeStatus status = meiso_runtime_receive(
        edge->runtime, message, &event
    );

    if (status != MEISO_RUNTIME_OK) {
        return false;
    }
    switch (event.type) {
    case MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED:
        if (!edge->catalog_offered || edge->catalog_active ||
            event.catalog_generation != K230D_WIFI_RUNTIME_EDGE_CATALOG_GENERATION) {
            return false;
        }
        edge->catalog_active = true;
        (void)printf(
            "{\"type\":\"runtime\",\"phase\":\"catalog\","
            "\"status\":\"ACCEPTED\",\"generation\":%u}\n",
            K230D_WIFI_RUNTIME_EDGE_CATALOG_GENERATION
        );
        return true;
    case MEISO_RUNTIME_EVENT_CATALOG_REJECTED:
        return false;
    case MEISO_RUNTIME_EVENT_STATE_OFFER:
        return edge->catalog_active &&
            handle_desired_offer(edge, &event, now_ns);
    case MEISO_RUNTIME_EVENT_STATE_ACCEPT:
        return edge->catalog_active && handle_actual_accept(edge, &event);
    default:
        return false;
    }
}

bool meiso_k230d_wifi_runtime_edge_private_try_send_pending_sample(K230dWifiRuntimeEdge *edge)
{
    MeisoRuntimeStatus status;

    if (!edge->pending_sample.present ||
        !meiso_k230d_wifi_runtime_edge_private_session_tx_idle(edge)) {
        return true;
    }
    status = meiso_runtime_send_stream_sample(
        edge->runtime,
        edge->data_route,
        edge->pending_sample.production_epoch,
        &edge->pending_sample.sample
    );
    if (status == MEISO_RUNTIME_TX_BUSY) {
        return true;
    }
    if (status != MEISO_RUNTIME_OK) {
        return false;
    }
    edge->samples_queued += 1U;
    edge->pending_sample.present = false;
    (void)printf(
        "{\"type\":\"stream\",\"phase\":\"sample\","
        "\"status\":\"QUEUED\",\"count\":%" PRIu64 "}\n",
        edge->samples_queued
    );
    return true;
}

bool meiso_k230d_wifi_runtime_edge_private_try_send_state_accept(K230dWifiRuntimeEdge *edge)
{
    MeisoRuntimeStatus status;

    if (!edge->pending_accept.present ||
        !meiso_k230d_wifi_runtime_edge_private_session_tx_idle(edge)) {
        return true;
    }
    status = meiso_runtime_send_state_accept(
        edge->runtime,
        edge->pending_accept.route,
        edge->pending_accept.generation,
        edge->pending_accept.owner,
        edge->pending_accept.version
    );
    if (status == MEISO_RUNTIME_TX_BUSY) {
        return true;
    }
    if (status != MEISO_RUNTIME_OK) {
        return false;
    }
    edge->pending_accept.present = false;
    return true;
}

static const char *actual_status_name(const K230dWifiRuntimeEdgeActualTransition *transition)
{
    if (transition->apply_result == MEISO_RUNTIME_APPLY_REJECTED) {
        return "REJECTED";
    }
    if (transition->availability == MEISO_RUNTIME_AVAILABILITY_UNAVAILABLE) {
        return "UNAVAILABLE";
    }
    switch (transition->production_status) {
    case MEISO_RUNTIME_PRODUCTION_STARTING:
        return "STARTING";
    case MEISO_RUNTIME_PRODUCTION_RUNNING:
        return "RUNNING";
    case MEISO_RUNTIME_PRODUCTION_DEGRADED:
        return "DEGRADED";
    case MEISO_RUNTIME_PRODUCTION_STOPPING:
        return "STOPPING";
    case MEISO_RUNTIME_PRODUCTION_FAILED:
        return "FAILED";
    default:
        return "STOPPED";
    }
}

bool meiso_k230d_wifi_runtime_edge_private_publish_actual_if_needed(K230dWifiRuntimeEdge *edge)
{
    uint8_t effective_payload[
        MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE
    ];
    uint8_t actual_payload[
        MEISO_WIFI_TELEMETRY_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    ];
    size_t effective_size = 0U;
    MeisoByteView encoded_actual;
    MeisoRuntimeActualState actual;
    MeisoStateOffer offer;
    MeisoStatePublishStatus published;
    MeisoRuntimeStatus sent;
    K230dWifiRuntimeEdgeActualTransition queued;
    uint64_t now_ns;

    if (!edge->actual_outbound.present || edge->actual_in_flight ||
        edge->pending_sample.present || edge->pending_accept.present ||
        !meiso_k230d_wifi_runtime_edge_private_session_tx_idle(edge)) {
        return true;
    }
    if (!edge->actual_outbound.published) {
        if (edge->actual_version == UINT64_MAX ||
            meiso_wifi_telemetry_effective_parameters_encode(
                &edge->actual_outbound.effective,
                (MeisoMutableBuffer){
                    effective_payload, sizeof(effective_payload)
                },
                &effective_size
            ) != MEISO_WIFI_TELEMETRY_CODEC_OK) {
            return false;
        }
        memset(&actual, 0, sizeof(actual));
        actual.observed_desired = edge->actual_outbound.observed;
        actual.applied_desired = edge->actual_outbound.applied;
        actual.apply_result = edge->actual_outbound.apply_result;
        actual.availability = edge->actual_outbound.availability;
        actual.effective_enabled = edge->actual_outbound.effective_enabled;
        actual.production_status = edge->actual_outbound.production_status;
        actual.reason = edge->actual_outbound.reason;
        actual.production_epoch = edge->actual_outbound.production_epoch;
        actual.profile = (MeisoByteView){effective_payload, effective_size};
        if (!meiso_runtime_actual_state_encode(
                &actual,
                (MeisoMutableBuffer){
                    actual_payload, sizeof(actual_payload)
                },
                &encoded_actual
            )) {
            return false;
        }
        edge->actual_version += 1U;
        published = meiso_state_publisher_publish(
            edge->actual_publisher,
            edge->edge_owner,
            edge->actual_version,
            encoded_actual
        );
        if (published != MEISO_STATE_PUBLISH_NEW) {
            return false;
        }
        edge->actual_outbound.published = true;
    }
    if (!meiso_state_publisher_next_offer(edge->actual_publisher, &offer)) {
        return false;
    }
    sent = meiso_runtime_send_state_offer(
        edge->runtime, edge->actual_route, &offer
    );
    if (sent == MEISO_RUNTIME_TX_BUSY) {
        return true;
    }
    if (sent != MEISO_RUNTIME_OK) {
        return false;
    }
    queued = edge->actual_outbound;
    memset(&edge->actual_outbound, 0, sizeof(edge->actual_outbound));
    edge->actual_in_flight = true;
    edge->actual_in_flight_value = queued;
    if (queued.apply_result == MEISO_RUNTIME_APPLY_APPLIED &&
        queued.production_status == MEISO_RUNTIME_PRODUCTION_RUNNING &&
        queued.effective_enabled) {
        if (!meiso_k230d_wifi_runtime_edge_private_monotonic_now_ns(&now_ns)) {
            return false;
        }
        edge->production_applied_desired = queued.applied;
        edge->production_effective = queued.effective;
        edge->active_production_epoch = queued.production_epoch;
        edge->production_active = true;
        edge->production_ever_started = true;
        edge->stream_sequence = 0U;
        edge->next_sample_ns = now_ns;
    } else if (queued.apply_result == MEISO_RUNTIME_APPLY_PENDING &&
        queued.production_status == MEISO_RUNTIME_PRODUCTION_RUNNING &&
        queued.effective_enabled && queued.production_epoch != 0U) {
        if (queued.production_epoch != edge->active_production_epoch ||
            queued.effective.selected_fields !=
                edge->production_effective.selected_fields ||
            queued.effective.sample_period_ms !=
                edge->production_effective.sample_period_ms) {
            return false;
        }
        edge->production_active = true;
    } else if (queued.production_status ==
        MEISO_RUNTIME_PRODUCTION_STOPPED) {
        edge->production_applied_desired = queued.applied;
        memset(
            &edge->production_effective,
            0,
            sizeof(edge->production_effective)
        );
        edge->active_production_epoch = 0U;
        edge->production_active = false;
    } else {
        edge->production_active = false;
    }
    (void)printf(
        "{\"type\":\"state\",\"phase\":\"actual\","
        "\"status\":\"%s\",\"version\":%" PRIu64 ","
        "\"desired_version\":%" PRIu64 ","
        "\"production_epoch\":%" PRIu64 "}\n",
        actual_status_name(&queued),
        edge->actual_version,
        queued.observed.version,
        queued.production_epoch
    );
    return true;
}

bool
meiso_k230d_wifi_runtime_edge_private_request_running_for_current_desired(
    K230dWifiRuntimeEdge *edge
)
{
    K230dWifiRuntimeEdgeActualTransition transition;
    if (!running_transition(
            edge,
            edge->current_desired,
            edge->current_parameters,
            &transition
        )) {
        return false;
    }
    return request_actual(edge, transition);
}
