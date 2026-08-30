#define _POSIX_C_SOURCE 200809L

#include "meiso_k230d_wifi_runtime_edge_private.h"

#include <string.h>

/*
 * Owns counter continuity, sample materialization, source recovery probing,
 * and the deterministic application drive order. A materialized sample keeps
 * its production epoch until it is queued or the Session terminates.
 */

static bool observe_counter(
    K230dWifiRuntimeEdgeCounterContinuity *continuity,
    MeisoWifiTelemetryFieldMask present_fields,
    MeisoWifiTelemetryFieldMask field,
    uint64_t value,
    uint64_t *high_water
)
{
    if ((present_fields & field) == 0U) {
        return true;
    }
    if ((continuity->seen_fields & field) != 0U && value < *high_water) {
        return false;
    }
    if ((continuity->seen_fields & field) == 0U || value > *high_water) {
        *high_water = value;
    }
    continuity->seen_fields |= field;
    return true;
}

static bool counter_continuity_observe(
    K230dWifiRuntimeEdgeCounterContinuity *continuity,
    const MeisoWifiTelemetrySample *sample
)
{
    return continuity != NULL && sample != NULL &&
        observe_counter(
            continuity,
            sample->present_fields,
            MEISO_WIFI_TELEMETRY_FIELD_RX_BYTES,
            sample->rx_bytes,
            &continuity->rx_bytes
        ) && observe_counter(
            continuity,
            sample->present_fields,
            MEISO_WIFI_TELEMETRY_FIELD_TX_BYTES,
            sample->tx_bytes,
            &continuity->tx_bytes
        ) && observe_counter(
            continuity,
            sample->present_fields,
            MEISO_WIFI_TELEMETRY_FIELD_RX_PACKETS,
            sample->rx_packets,
            &continuity->rx_packets
        ) && observe_counter(
            continuity,
            sample->present_fields,
            MEISO_WIFI_TELEMETRY_FIELD_TX_PACKETS,
            sample->tx_packets,
            &continuity->tx_packets
        ) && observe_counter(
            continuity,
            sample->present_fields,
            MEISO_WIFI_TELEMETRY_FIELD_RX_DROPPED,
            sample->rx_dropped,
            &continuity->rx_dropped
        ) && observe_counter(
            continuity,
            sample->present_fields,
            MEISO_WIFI_TELEMETRY_FIELD_TX_DROPPED,
            sample->tx_dropped,
            &continuity->tx_dropped
        ) && observe_counter(
            continuity,
            sample->present_fields,
            MEISO_WIFI_TELEMETRY_FIELD_RX_ERRORS,
            sample->rx_errors,
            &continuity->rx_errors
        ) && observe_counter(
            continuity,
            sample->present_fields,
            MEISO_WIFI_TELEMETRY_FIELD_TX_ERRORS,
            sample->tx_errors,
            &continuity->tx_errors
        );
}

bool meiso_k230d_wifi_runtime_edge_private_counter_continuity_self_test(void)
{
    const MeisoWifiTelemetryFieldMask counter_fields =
        MEISO_WIFI_TELEMETRY_FIELD_RX_BYTES |
        MEISO_WIFI_TELEMETRY_FIELD_TX_BYTES |
        MEISO_WIFI_TELEMETRY_FIELD_RX_PACKETS |
        MEISO_WIFI_TELEMETRY_FIELD_TX_PACKETS |
        MEISO_WIFI_TELEMETRY_FIELD_RX_DROPPED |
        MEISO_WIFI_TELEMETRY_FIELD_TX_DROPPED |
        MEISO_WIFI_TELEMETRY_FIELD_RX_ERRORS |
        MEISO_WIFI_TELEMETRY_FIELD_TX_ERRORS;
    K230dWifiRuntimeEdgeCounterContinuity continuity;
    MeisoWifiTelemetrySample sample;

    memset(&continuity, 0, sizeof(continuity));
    memset(&sample, 0, sizeof(sample));
    sample.selected_fields = counter_fields;
    sample.present_fields = counter_fields;
    sample.rx_bytes = 100U;
    sample.tx_bytes = 101U;
    sample.rx_packets = 102U;
    sample.tx_packets = 103U;
    sample.rx_dropped = 104U;
    sample.tx_dropped = 105U;
    sample.rx_errors = 106U;
    sample.tx_errors = 107U;
    if (!counter_continuity_observe(&continuity, &sample)) {
        return false;
    }
    sample.present_fields = 0U;
    if (!counter_continuity_observe(&continuity, &sample) ||
        continuity.rx_bytes != 100U ||
        continuity.tx_bytes != 101U ||
        continuity.rx_packets != 102U ||
        continuity.tx_packets != 103U ||
        continuity.rx_dropped != 104U ||
        continuity.tx_dropped != 105U ||
        continuity.rx_errors != 106U ||
        continuity.tx_errors != 107U ||
        (continuity.seen_fields & counter_fields) != counter_fields) {
        return false;
    }
    sample.present_fields = MEISO_WIFI_TELEMETRY_FIELD_TX_ERRORS;
    sample.tx_errors = 5U;
    return !counter_continuity_observe(&continuity, &sample);
}

static bool maybe_materialize_sample(K230dWifiRuntimeEdge *edge, uint64_t now_ns)
{
    MeisoWifiTelemetrySample value;
    MeisoWifiLinuxAdapterStatus adapter_status;
    size_t encoded_size = 0U;

    if (!edge->production_active || edge->pending_sample.present ||
        now_ns < edge->next_sample_ns) {
        return true;
    }
    if (edge->active_production_epoch == 0U ||
        edge->production_effective.selected_fields == 0U ||
        edge->production_effective.sample_period_ms == 0U) {
        return false;
    }
    adapter_status = meiso_wifi_linux_adapter_sample(
        &edge->wifi,
        edge->production_effective.selected_fields,
        &value
    );
    if (adapter_status == MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE) {
        meiso_k230d_wifi_runtime_edge_private_mark_source_continuity_lost(
            edge, "SOURCE_UNAVAILABLE"
        );
        return true;
    }
    if (adapter_status != MEISO_WIFI_LINUX_ADAPTER_OK) {
        return false;
    }
    if (!counter_continuity_observe(&edge->counter_continuity, &value)) {
        meiso_k230d_wifi_runtime_edge_private_mark_source_continuity_lost(edge, "COUNTER_RESET");
        return true;
    }
    if (edge->stream_sequence == UINT64_MAX ||
        meiso_wifi_telemetry_sample_encode(
            &value,
            (MeisoMutableBuffer){
                edge->pending_sample.payload,
                sizeof(edge->pending_sample.payload)
            },
            &encoded_size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        encoded_size != sizeof(edge->pending_sample.payload)) {
        return false;
    }
    edge->stream_sequence += 1U;
    memset(&edge->pending_sample.sample, 0, sizeof(edge->pending_sample.sample));
    edge->pending_sample.sample.key = (MeisoStreamKey){
        edge->edge_owner,
        edge->active_production_epoch,
        edge->stream_sequence
    };
    edge->pending_sample.sample.source_time = (MeisoTimePoint){
        edge->clock_epoch,
        now_ns
    };
    edge->pending_sample.sample.payload = (MeisoByteView){
        edge->pending_sample.payload,
        encoded_size
    };
    edge->pending_sample.production_epoch =
        edge->active_production_epoch;
    edge->pending_sample.present = true;
    edge->next_sample_ns = meiso_k230d_wifi_runtime_edge_private_deadline_after_ms(
        now_ns, edge->production_effective.sample_period_ms
    );
    return true;
}

static bool maybe_probe_recovery(K230dWifiRuntimeEdge *edge, uint64_t now_ns)
{
    MeisoWifiTelemetryFieldMask fields = 0U;
    MeisoWifiLinuxAdapterStatus status;

    if (!edge->catalog_active || !edge->desired_profile_valid ||
        !edge->desired_enabled || edge->production_active ||
        edge->production_ever_started || edge->source_continuity_lost ||
        edge->actual_in_flight || edge->actual_outbound.present ||
        edge->actual_next.present || now_ns < edge->next_probe_ns) {
        return true;
    }
    status = meiso_wifi_linux_adapter_probe(&edge->wifi, &fields);
    if (status == MEISO_WIFI_LINUX_ADAPTER_OK) {
        return meiso_k230d_wifi_runtime_edge_private_request_running_for_current_desired(edge);
    }
    if (status != MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE) {
        return false;
    }
    edge->next_probe_ns = meiso_k230d_wifi_runtime_edge_private_deadline_after_ms(
        now_ns, edge->current_parameters.sample_period_ms
    );
    return true;
}

bool meiso_k230d_wifi_runtime_edge_private_drive_application(
    K230dWifiRuntimeEdge *edge,
    uint64_t now_ns
)
{
    if ((!edge->source_continuity_lost &&
         !meiso_k230d_wifi_runtime_edge_private_try_offer_catalog(edge)) ||
        !meiso_k230d_wifi_runtime_edge_private_try_send_pending_sample(edge) ||
        !meiso_k230d_wifi_runtime_edge_private_try_send_state_accept(edge)) {
        return false;
    }
    if (edge->source_continuity_lost) {
        return true;
    }
    if (!meiso_k230d_wifi_runtime_edge_private_publish_actual_if_needed(
            edge
        ) ||
        !maybe_materialize_sample(edge, now_ns) ||
        !meiso_k230d_wifi_runtime_edge_private_try_send_pending_sample(
            edge
        )) {
        return false;
    }
    if (edge->source_continuity_lost) {
        return true;
    }
    if (!maybe_probe_recovery(edge, now_ns) ||
        !meiso_k230d_wifi_runtime_edge_private_publish_actual_if_needed(
            edge
        )) {
        return false;
    }
    return true;
}
