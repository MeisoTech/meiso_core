#define _POSIX_C_SOURCE 200809L

#include "meiso_wifi_runtime_host_private.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Reconciles desired/actual truth and validates emitted telemetry samples. */

static bool route_key_equal(
    MeisoRuntimeRouteKey lhs,
    MeisoRuntimeRouteKey rhs
)
{
    return lhs.catalog_generation == rhs.catalog_generation &&
        meiso_endpoint_id_equal(lhs.endpoint_id, rhs.endpoint_id) &&
        lhs.endpoint_incarnation == rhs.endpoint_incarnation &&
        meiso_resource_id_equal(lhs.resource_id, rhs.resource_id) &&
        lhs.facet == rhs.facet;
}
bool meiso_wifi_runtime_host_private_publish_desired(MeisoWifiRuntimeHostPrivate *host, uint64_t version, bool enabled)
{
    uint8_t profile_bytes[
        MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE
    ];
    uint8_t payload_bytes[
        MEISO_WIFI_TELEMETRY_DESIRED_STATE_MAX_PAYLOAD_SIZE
    ];
    size_t profile_size = 0U;
    MeisoByteView payload;
    MeisoRuntimeDesiredState desired;
    MeisoStateOffer offer;
    MeisoStatePublishStatus publish_status;

    if (host == NULL || !host->catalog_active || version == 0U ||
        meiso_wifi_telemetry_desired_parameters_encode(
            &host->bootstrap.desired,
            (MeisoMutableBuffer){profile_bytes, sizeof(profile_bytes)},
            &profile_size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK) {
        return false;
    }
    desired.enabled = enabled;
    desired.profile = (MeisoByteView){profile_bytes, profile_size};
    if (!meiso_runtime_desired_state_encode(
            &desired,
            (MeisoMutableBuffer){payload_bytes, sizeof(payload_bytes)},
            &payload
        )) {
        return false;
    }
    publish_status = meiso_state_publisher_publish(
        host->desired_publisher, host->host_owner, version, payload
    );
    if ((publish_status != MEISO_STATE_PUBLISH_NEW &&
         publish_status != MEISO_STATE_PUBLISH_REPLACED_PENDING) ||
        !meiso_state_publisher_next_offer(
            host->desired_publisher, &offer
        ) ||
        meiso_runtime_send_state_offer(
            host->runtime, host->desired_route, &offer
        ) != MEISO_RUNTIME_OK || !meiso_wifi_runtime_host_private_flush_outbound(host)) {
        return false;
    }
    (void)printf(
        "{\"type\":\"desired\",\"status\":\"PUBLISHED\","
        "\"version\":%" PRIu64 ",\"enabled\":%s,"
        "\"selected_fields\":%" PRIu32 ","
        "\"sample_period_ms\":%" PRIu32 "}\n",
        version,
        enabled ? "true" : "false",
        host->bootstrap.desired.selected_fields,
        host->bootstrap.desired.sample_period_ms
    );
    (void)fflush(stdout);
    return true;
}

static bool handle_desired_accept(
    MeisoWifiRuntimeHostPrivate *host,
    const MeisoRuntimeEvent *event,
    uint64_t expected_version
)
{
    MeisoStateAcceptStatus status;

    if (host == NULL || event == NULL ||
        event->type != MEISO_RUNTIME_EVENT_STATE_ACCEPT ||
        !route_key_equal(event->route, host->desired_route) ||
        event->state_accept_generation.value != 1U ||
        !meiso_owner_scope_equal(
            event->state_accept_owner, host->host_owner
        ) || event->state_accept_version != expected_version) {
        return false;
    }
    status = meiso_state_publisher_mark_delivery_accepted(
        host->desired_publisher,
        event->state_accept_generation,
        event->state_accept_owner,
        event->state_accept_version
    );
    if (status != MEISO_STATE_ACCEPT_CONFIRMED &&
        status != MEISO_STATE_ACCEPT_DUPLICATE) {
        return false;
    }
    (void)printf(
        "{\"type\":\"desired_delivery\","
        "\"status\":\"ACCEPTED\",\"version\":%" PRIu64 "}\n",
        expected_version
    );
    (void)fflush(stdout);
    return true;
}

static const char *production_status_name(MeisoRuntimeProductionStatus status)
{
    switch (status) {
    case MEISO_RUNTIME_PRODUCTION_STOPPED:
        return "STOPPED";
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
        return "INVALID";
    }
}

static const char *apply_result_name(MeisoRuntimeApplyResult result)
{
    switch (result) {
    case MEISO_RUNTIME_APPLY_NONE:
        return "NONE";
    case MEISO_RUNTIME_APPLY_PENDING:
        return "PENDING";
    case MEISO_RUNTIME_APPLY_APPLIED:
        return "APPLIED";
    case MEISO_RUNTIME_APPLY_REJECTED:
        return "REJECTED";
    case MEISO_RUNTIME_APPLY_FAILED:
        return "FAILED";
    default:
        return "INVALID";
    }
}

static bool desired_reference_matches(
    MeisoRuntimeDesiredReference reference,
    MeisoOwnerScope owner,
    uint64_t version
)
{
    return reference.present &&
        meiso_owner_scope_equal(reference.owner, owner) &&
        reference.version == version;
}

static bool effective_parameters_supported(
    const MeisoWifiRuntimeHostPrivate *host,
    MeisoWifiTelemetryEffectiveParameters effective
)
{
    const MeisoWifiTelemetryDesiredParameters parameters = {
        effective.selected_fields, effective.sample_period_ms
    };

    if (effective.selected_fields == 0U || effective.sample_period_ms == 0U) {
        return effective.selected_fields == 0U &&
            effective.sample_period_ms == 0U;
    }
    return meiso_wifi_telemetry_parameters_supported(
        &host->capability, &parameters
    );
}

static bool handle_actual_offer(
    MeisoWifiRuntimeHostPrivate *host,
    const MeisoRuntimeEvent *event,
    uint64_t desired_version,
    MeisoRuntimeProductionStatus target_status,
    bool *out_target_reached
)
{
    MeisoRuntimeActualState actual;
    MeisoWifiTelemetryEffectiveParameters effective;
    MeisoStateReceiveStatus receive_status;
    MeisoRuntimeProductionSnapshot production;
    MeisoRuntimeRouteKey received_route;
    MeisoDeliveryGeneration delivery;
    MeisoOwnerScope owner;
    uint64_t state_version;
    bool target_reached = false;

    if (host == NULL || event == NULL || out_target_reached == NULL ||
        event->type != MEISO_RUNTIME_EVENT_STATE_OFFER ||
        !route_key_equal(event->route, host->actual_route) ||
        !meiso_owner_scope_equal(
            event->state_offer.state.owner, host->edge_owner
        ) || !meiso_runtime_actual_state_decode(
            event->state_offer.state.payload, &actual
        ) || !desired_reference_matches(
            actual.observed_desired, host->host_owner, desired_version
        ) ||
        meiso_wifi_telemetry_effective_parameters_decode(
            actual.profile, &effective
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        !effective_parameters_supported(host, effective)) {
        return false;
    }

    received_route = event->route;
    delivery = event->state_offer.generation;
    owner = event->state_offer.state.owner;
    state_version = event->state_offer.state.version;
    receive_status = meiso_state_replica_receive(
        host->actual_replica,
        owner,
        state_version,
        event->state_offer.state.payload
    );
    if (meiso_runtime_commit_state_receive(
            host->runtime,
            received_route,
            &event->state_offer,
            receive_status
        ) != MEISO_RUNTIME_OK ||
        (receive_status != MEISO_STATE_RECEIVE_NEW &&
         receive_status != MEISO_STATE_RECEIVE_DUPLICATE) ||
        meiso_runtime_production_snapshot(
            host->runtime,
            received_route.endpoint_id,
            &production
        ) != MEISO_RUNTIME_OK ||
        meiso_runtime_send_state_accept(
            host->runtime,
            received_route,
            delivery,
            owner,
            state_version
        ) != MEISO_RUNTIME_OK || !meiso_wifi_runtime_host_private_flush_outbound(host)) {
        return false;
    }

    host->production_active = production.data_active;
    if (production.data_active) {
        if (!host->active_effective_present ||
            host->running_production_epoch != production.production_epoch) {
            host->active_effective = effective;
            host->active_effective_present = true;
            host->stream_producer_active = false;
        } else if (host->active_effective.selected_fields !=
                effective.selected_fields ||
            host->active_effective.sample_period_ms !=
                effective.sample_period_ms) {
            return false;
        }
        host->running_production_epoch = production.production_epoch;
    } else {
        memset(&host->active_effective, 0, sizeof(host->active_effective));
        host->active_effective_present = false;
        host->stream_producer_active = false;
    }
    if (target_status == MEISO_RUNTIME_PRODUCTION_RUNNING) {
        target_reached =
            actual.production_status == MEISO_RUNTIME_PRODUCTION_RUNNING &&
            actual.apply_result == MEISO_RUNTIME_APPLY_APPLIED &&
            actual.availability == MEISO_RUNTIME_AVAILABILITY_PRESENT &&
            actual.effective_enabled &&
            actual.reason == MEISO_RUNTIME_PRODUCTION_REASON_NONE &&
            desired_reference_matches(
                actual.applied_desired, host->host_owner, desired_version
            ) &&
            effective.selected_fields ==
                host->bootstrap.desired.selected_fields &&
            effective.sample_period_ms ==
                host->bootstrap.desired.sample_period_ms &&
            production.data_active && production.production_epoch != 0U;
    } else if (target_status == MEISO_RUNTIME_PRODUCTION_STOPPED) {
        target_reached =
            actual.production_status == MEISO_RUNTIME_PRODUCTION_STOPPED &&
            actual.apply_result == MEISO_RUNTIME_APPLY_APPLIED &&
            !actual.effective_enabled &&
            actual.reason == MEISO_RUNTIME_PRODUCTION_REASON_DISABLED &&
            desired_reference_matches(
                actual.applied_desired, host->host_owner, desired_version
            ) && effective.selected_fields == 0U &&
            effective.sample_period_ms == 0U &&
            !production.data_active && production.production_epoch == 0U;
    }
    if ((actual.apply_result == MEISO_RUNTIME_APPLY_REJECTED ||
         actual.apply_result == MEISO_RUNTIME_APPLY_FAILED ||
         actual.production_status == MEISO_RUNTIME_PRODUCTION_FAILED) &&
        !target_reached) {
        return false;
    }
    (void)printf(
        "{\"type\":\"actual\",\"status\":\"%s\","
        "\"state_version\":%" PRIu64 ","
        "\"observed_desired_version\":%" PRIu64 ","
        "\"apply_result\":\"%s\","
        "\"effective_enabled\":%s,\"data_active\":%s,"
        "\"selected_fields\":%" PRIu32 ","
        "\"sample_period_ms\":%" PRIu32 ","
        "\"production_epoch_known\":%s}\n",
        production_status_name(actual.production_status),
        state_version,
        desired_version,
        apply_result_name(actual.apply_result),
        actual.effective_enabled ? "true" : "false",
        production.data_active ? "true" : "false",
        effective.selected_fields,
        effective.sample_period_ms,
        production.production_epoch != 0U ? "true" : "false"
    );
    (void)fflush(stdout);
    *out_target_reached = target_reached;
    return true;
}

static bool field_present(
    const MeisoWifiTelemetrySample *sample,
    MeisoWifiTelemetryFieldMask field
)
{
    return (sample->present_fields & field) != 0U;
}

static void emit_nullable_u64(bool present, uint64_t value)
{
    if (present) {
        (void)printf("%" PRIu64, value);
    } else {
        fputs("null", stdout);
    }
}

static void emit_sample(
    uint64_t ordinal,
    const MeisoStreamSample *stream_sample,
    const MeisoWifiTelemetrySample *sample,
    const MeisoStreamReceiveResult *receive_result
)
{
    const bool link_present = field_present(
        sample, MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE
    );
    const bool signal_present = field_present(
        sample, MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM
    );

    (void)printf(
        "{\"type\":\"sample\",\"ordinal\":%" PRIu64 ","
        "\"sequence\":%" PRIu64 ","
        "\"source_time_ns\":%" PRIu64 ","
        "\"selected_fields\":%" PRIu32 ","
        "\"present_fields\":%" PRIu32 ","
        "\"gap\":%s,\"missing_before\":%" PRIu64 ","
        "\"link_state\":",
        ordinal,
        stream_sample->key.sequence,
        stream_sample->source_time.monotonic_ns,
        sample->selected_fields,
        sample->present_fields,
        (receive_result->flags & MEISO_STREAM_RECEIVE_FLAG_GAP) != 0U ?
            "true" : "false",
        receive_result->missing_before
    );
    if (link_present) {
        fputs(sample->link_state ? "true" : "false", stdout);
    } else {
        fputs("null", stdout);
    }
    fputs(",\"signal_mbm\":", stdout);
    if (signal_present) {
        (void)printf("%" PRId32, sample->signal_mbm);
    } else {
        fputs("null", stdout);
    }
    fputs(",\"rx_bytes\":", stdout);
    emit_nullable_u64(
        field_present(sample, MEISO_WIFI_TELEMETRY_FIELD_RX_BYTES),
        sample->rx_bytes
    );
    fputs(",\"tx_bytes\":", stdout);
    emit_nullable_u64(
        field_present(sample, MEISO_WIFI_TELEMETRY_FIELD_TX_BYTES),
        sample->tx_bytes
    );
    fputs(",\"rx_packets\":", stdout);
    emit_nullable_u64(
        field_present(sample, MEISO_WIFI_TELEMETRY_FIELD_RX_PACKETS),
        sample->rx_packets
    );
    fputs(",\"tx_packets\":", stdout);
    emit_nullable_u64(
        field_present(sample, MEISO_WIFI_TELEMETRY_FIELD_TX_PACKETS),
        sample->tx_packets
    );
    fputs(",\"rx_dropped\":", stdout);
    emit_nullable_u64(
        field_present(sample, MEISO_WIFI_TELEMETRY_FIELD_RX_DROPPED),
        sample->rx_dropped
    );
    fputs(",\"tx_dropped\":", stdout);
    emit_nullable_u64(
        field_present(sample, MEISO_WIFI_TELEMETRY_FIELD_TX_DROPPED),
        sample->tx_dropped
    );
    fputs(",\"rx_errors\":", stdout);
    emit_nullable_u64(
        field_present(sample, MEISO_WIFI_TELEMETRY_FIELD_RX_ERRORS),
        sample->rx_errors
    );
    fputs(",\"tx_errors\":", stdout);
    emit_nullable_u64(
        field_present(sample, MEISO_WIFI_TELEMETRY_FIELD_TX_ERRORS),
        sample->tx_errors
    );
    fputs("}\n", stdout);
    (void)fflush(stdout);
}

static bool handle_stream_sample(
    MeisoWifiRuntimeHostPrivate *host,
    const MeisoRuntimeEvent *event
)
{
    MeisoStreamProducerStatus producer_status;
    MeisoStreamReceiveResult receive_result;
    MeisoStreamSample stored;
    MeisoWifiTelemetrySample decoded;

    if (host == NULL || event == NULL ||
        event->type != MEISO_RUNTIME_EVENT_STREAM_SAMPLE ||
        !route_key_equal(event->route, host->data_route) ||
        !host->production_active || host->running_production_epoch == 0U ||
        !host->active_effective_present ||
        event->production_epoch != host->running_production_epoch ||
        !meiso_owner_scope_equal(
            event->stream_sample.key.producer, host->edge_owner
        ) || event->stream_sample.sender_deadline.present ||
        event->stream_sample.dependency.size != 0U ||
        host->samples_received == UINT64_MAX ||
        (!host->interactive && host->samples_received >=
            (uint64_t)(MEISO_WIFI_RUNTIME_HOST_PRIVATE_MAX_SAMPLES + MEISO_WIFI_RUNTIME_HOST_PRIVATE_MAX_STOPPING_SAMPLES))) {
        return false;
    }
    if (!host->stream_producer_active) {
        producer_status = meiso_stream_begin_producer(
            host->stream,
            event->stream_sample.key.producer,
            event->stream_sample.key.epoch
        );
        if (producer_status != MEISO_STREAM_PRODUCER_STARTED &&
            producer_status != MEISO_STREAM_PRODUCER_SWITCHED) {
            return false;
        }
        host->stream_producer = event->stream_sample.key.producer;
        host->stream_epoch = event->stream_sample.key.epoch;
        host->stream_producer_active = true;
    } else if (!meiso_owner_scope_equal(
            event->stream_sample.key.producer, host->stream_producer
        ) || event->stream_sample.key.epoch != host->stream_epoch) {
        return false;
    }
    receive_result = meiso_stream_receive(
        host->stream, &event->stream_sample, NULL
    );
    if (receive_result.code != MEISO_STREAM_RECEIVE_ACCEPTED ||
        !meiso_stream_peek(host->stream, &stored) ||
        meiso_wifi_telemetry_sample_decode(
            stored.payload, &decoded
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        decoded.selected_fields != host->active_effective.selected_fields) {
        return false;
    }
    host->samples_received += 1U;
    emit_sample(
        host->samples_received, &stored, &decoded, &receive_result
    );
    return meiso_stream_pop(host->stream);
}

bool meiso_wifi_runtime_host_private_wait_desired_and_actual(
    MeisoWifiRuntimeHostPrivate *host,
    uint64_t desired_version,
    MeisoRuntimeProductionStatus target_status
)
{
    struct timespec deadline;
    bool desired_accepted = false;
    bool target_reached = false;

    if (!meiso_wifi_runtime_host_private_deadline_after_ms(&deadline, MEISO_WIFI_RUNTIME_HOST_PRIVATE_PHASE_TIMEOUT_MS)) {
        return false;
    }
    while (!desired_accepted || !target_reached) {
        MeisoRuntimeEvent event;

        if (!meiso_wifi_runtime_host_private_wait_runtime_event(host, &deadline, &event)) {
            return false;
        }
        if (event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT) {
            if (!handle_desired_accept(
                    host, &event, desired_version
                )) {
                return false;
            }
            desired_accepted = true;
        } else if (event.type == MEISO_RUNTIME_EVENT_STATE_OFFER) {
            bool reached = false;
            if (!handle_actual_offer(
                    host,
                    &event,
                    desired_version,
                    target_status,
                    &reached
                )) {
                return false;
            }
            target_reached = reached;
        } else if (event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE) {
            if (!handle_stream_sample(host, &event) ||
                (!host->interactive &&
                 target_status == MEISO_RUNTIME_PRODUCTION_STOPPED &&
                 host->samples_received >
                    (uint64_t)host->bootstrap.sample_count +
                        MEISO_WIFI_RUNTIME_HOST_PRIVATE_MAX_STOPPING_SAMPLES)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

bool meiso_wifi_runtime_host_private_receive_requested_samples(MeisoWifiRuntimeHostPrivate *host)
{
    struct timespec deadline;
    const uint64_t wait_ms =
        (uint64_t)host->bootstrap.desired.sample_period_ms *
            (uint64_t)host->bootstrap.sample_count +
        MEISO_WIFI_RUNTIME_HOST_PRIVATE_PHASE_TIMEOUT_MS;

    if (!meiso_wifi_runtime_host_private_deadline_after_ms(&deadline, wait_ms)) {
        return false;
    }
    while (host->samples_received < host->bootstrap.sample_count) {
        MeisoRuntimeEvent event;

        if (!meiso_wifi_runtime_host_private_wait_runtime_event(host, &deadline, &event)) {
            return false;
        }
        if (event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE) {
            if (!handle_stream_sample(host, &event)) {
                return false;
            }
        } else if (event.type == MEISO_RUNTIME_EVENT_STATE_OFFER) {
            bool running = false;
            if (!handle_actual_offer(
                    host,
                    &event,
                    1U,
                    MEISO_RUNTIME_PRODUCTION_RUNNING,
                    &running
                ) || !host->production_active) {
                return false;
            }
        } else if (event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT) {
            if (!handle_desired_accept(host, &event, 1U)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

bool meiso_wifi_runtime_host_private_handle_interactive_runtime_event(
    MeisoWifiRuntimeHostPrivate *host,
    const MeisoRuntimeEvent *event,
    uint64_t desired_version,
    bool desired_enabled
)
{
    if (host == NULL || event == NULL || desired_version == 0U) {
        return false;
    }
    if (event->type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE) {
        return handle_stream_sample(host, event);
    }
    if (event->type == MEISO_RUNTIME_EVENT_STATE_ACCEPT) {
        return handle_desired_accept(host, event, desired_version);
    }
    if (event->type == MEISO_RUNTIME_EVENT_STATE_OFFER) {
        bool target_reached = false;
        return handle_actual_offer(
            host,
            event,
            desired_version,
            desired_enabled ? MEISO_RUNTIME_PRODUCTION_RUNNING :
                MEISO_RUNTIME_PRODUCTION_STOPPED,
            &target_reached
        );
    }
    return false;
}
