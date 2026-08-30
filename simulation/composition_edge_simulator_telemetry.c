#include "meiso_composition_edge_simulator_private.h"

/* Simulator telemetry owner applies Wi-Fi, device and media Actual truth. */

#include <string.h>

void meiso_composition_edge_simulator_private_apply_wifi(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
)
{
    MeisoCompositionEdgeSnapshot *snapshot = &handle->simulator_snapshot;
    MeisoCompositionEdgeInlineView *stream =
        &snapshot->inline_streams[MEISO_COMPOSITION_EDGE_INLINE_WIFI];

    meiso_composition_edge_simulator_private_apply_actual(handle, MEISO_COMPOSITION_EDGE_DESIRED_WIFI,
                           handle->simulator_wifi_epoch);
    handle->simulator_wifi_pending = false;
    snapshot->wifi_effective_present = handle->simulator_wifi_enabled;
    snapshot->wifi_effective = handle->simulator_wifi_enabled ?
        (MeisoWifiTelemetryEffectiveParameters){
            handle->simulator_wifi_desired.selected_fields,
            handle->simulator_wifi_desired.sample_period_ms
        } : (MeisoWifiTelemetryEffectiveParameters){0U, 0U};
    snapshot->wifi_sample_present = handle->simulator_wifi_enabled;
    if (!handle->simulator_wifi_enabled) {
        memset(&snapshot->wifi_sample, 0, sizeof(snapshot->wifi_sample));
        memset(stream, 0, sizeof(*stream));
        return;
    }
    snapshot->wifi_sample = handle->simulator_wifi_sample;
    snapshot->wifi_sample.selected_fields =
        handle->simulator_wifi_desired.selected_fields;
    snapshot->wifi_sample.present_fields &=
        handle->simulator_wifi_desired.selected_fields;
    if ((snapshot->wifi_sample.present_fields &
         MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE) == 0U) {
        snapshot->wifi_sample.link_state = false;
    }
    if ((snapshot->wifi_sample.present_fields &
         MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM) == 0U) {
        snapshot->wifi_sample.signal_mbm = 0;
    }
#define CLEAR_WIFI_COUNTER(field, mask)                                        \
    do {                                                                       \
        if ((snapshot->wifi_sample.present_fields & (mask)) == 0U) {           \
            snapshot->wifi_sample.field = 0U;                                  \
        }                                                                      \
    } while (0)
    CLEAR_WIFI_COUNTER(rx_bytes, MEISO_WIFI_TELEMETRY_FIELD_RX_BYTES);
    CLEAR_WIFI_COUNTER(tx_bytes, MEISO_WIFI_TELEMETRY_FIELD_TX_BYTES);
    CLEAR_WIFI_COUNTER(rx_packets, MEISO_WIFI_TELEMETRY_FIELD_RX_PACKETS);
    CLEAR_WIFI_COUNTER(tx_packets, MEISO_WIFI_TELEMETRY_FIELD_TX_PACKETS);
    CLEAR_WIFI_COUNTER(rx_dropped, MEISO_WIFI_TELEMETRY_FIELD_RX_DROPPED);
    CLEAR_WIFI_COUNTER(tx_dropped, MEISO_WIFI_TELEMETRY_FIELD_TX_DROPPED);
    CLEAR_WIFI_COUNTER(rx_errors, MEISO_WIFI_TELEMETRY_FIELD_RX_ERRORS);
    CLEAR_WIFI_COUNTER(tx_errors, MEISO_WIFI_TELEMETRY_FIELD_TX_ERRORS);
#undef CLEAR_WIFI_COUNTER
    *stream = (MeisoCompositionEdgeInlineView){
        true,
        {handle->simulator_desired_owner, handle->simulator_wifi_epoch, 1U},
        now,
        MEISO_STREAM_RECEIVE_FLAG_NONE,
        0U,
        stream->received_count + 1U,
        0U
    };
    if (handle->simulator_wifi_epoch == UINT64_MAX) {
        handle->simulator_wifi_epoch_available = false;
    } else {
        ++handle->simulator_wifi_epoch;
    }
}

void meiso_composition_edge_simulator_private_apply_device(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
)
{
    MeisoCompositionEdgeSnapshot *snapshot = &handle->simulator_snapshot;
    MeisoCompositionEdgeInlineView *stream =
        &snapshot->inline_streams[MEISO_COMPOSITION_EDGE_INLINE_DEVICE];

    meiso_composition_edge_simulator_private_apply_actual(handle, MEISO_COMPOSITION_EDGE_DESIRED_DEVICE,
                           handle->simulator_device_epoch);
    handle->simulator_device_pending = false;
    snapshot->device_effective_present = handle->simulator_device_enabled;
    snapshot->device_effective = handle->simulator_device_enabled ?
        handle->simulator_device_desired :
        (MeisoDeviceTelemetryParameters){0U, 0U};
    snapshot->device_sample_present = handle->simulator_device_enabled;
    if (!handle->simulator_device_enabled) {
        memset(&snapshot->device_sample, 0, sizeof(snapshot->device_sample));
        memset(stream, 0, sizeof(*stream));
        return;
    }
    snapshot->device_sample = handle->simulator_device_sample;
    snapshot->device_sample.selected_fields =
        handle->simulator_device_desired.selected_fields;
    snapshot->device_sample.present_fields &=
        handle->simulator_device_desired.selected_fields;
    if ((snapshot->device_sample.present_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC) == 0U) {
        snapshot->device_sample.temperature_millic = 0;
        snapshot->device_sample.temperature_uncertainty_millic = 0U;
    }
    if ((snapshot->device_sample.present_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM) == 0U) {
        snapshot->device_sample.cpu_load_ppm = 0U;
    }
    if ((snapshot->device_sample.present_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_TOTAL_BYTES) == 0U) {
        snapshot->device_sample.memory_total_bytes = 0U;
    }
    if ((snapshot->device_sample.present_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_MEMORY_AVAILABLE_BYTES) == 0U) {
        snapshot->device_sample.memory_available_bytes = 0U;
    }
    *stream = (MeisoCompositionEdgeInlineView){
        true,
        {handle->simulator_desired_owner, handle->simulator_device_epoch, 1U},
        now,
        MEISO_STREAM_RECEIVE_FLAG_NONE,
        0U,
        stream->received_count + 1U,
        0U
    };
    if (handle->simulator_device_epoch == UINT64_MAX) {
        handle->simulator_device_epoch_available = false;
    } else {
        ++handle->simulator_device_epoch;
    }
}

void meiso_composition_edge_simulator_private_apply_media(MeisoEdgeHandle *handle, size_t index)
{
    MeisoCompositionEdgeSimulatorMediaPending *pending =
        &handle->simulator_media_pending[index];
    MeisoCompositionEdgeSnapshot *snapshot = &handle->simulator_snapshot;
    const MeisoCompositionEdgeDesiredChannel channel = index == 0U ?
        MEISO_COMPOSITION_EDGE_DESIRED_RENDER_MEDIA :
        MEISO_COMPOSITION_EDGE_DESIRED_CAMERA_MEDIA;
    const MeisoCompositionMediaSource source = index == 0U ?
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW :
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
    const uint64_t epoch = handle->simulator_media_epoch[index];
    MeisoCompositionEdgeMediaActual *actual = index == 0U ?
        &snapshot->render_media_actual : &snapshot->camera_media_actual;

    meiso_composition_edge_simulator_private_apply_actual(handle, channel, epoch);
    pending->pending = false;
    memset(&handle->simulator_media[index].frame, 0,
           sizeof(handle->simulator_media[index].frame));
    handle->simulator_media[index].present = false;
    handle->simulator_media[index].borrowed = false;
    snapshot->media[index] = (MeisoCompositionEdgeMediaRouteView){
        pending->enabled, false, source, 0U, {0U, 0U}, 0U, 0U, 0U, 0U, 0U,
        0U
    };
    if (index == 0U) {
        snapshot->render_media_actual_present = pending->enabled;
        handle->simulator_raster_preview_pending =
            handle->simulator_cpu_raster_enabled && pending->enabled &&
            snapshot->render_result_present;
    } else {
        snapshot->camera_media_actual_present = pending->enabled;
    }
    if (pending->enabled) {
        *actual = (MeisoCompositionEdgeMediaActual){
            source, pending->desired, epoch, 0U, 0U, 0U
        };
        if (handle->simulator_media_epoch[index] == UINT64_MAX) {
            handle->simulator_media_epoch_available[index] = false;
        } else {
            ++handle->simulator_media_epoch[index];
        }
    } else {
        memset(actual, 0, sizeof(*actual));
    }
}

bool meiso_composition_edge_simulator_private_take_epoch(
    uint64_t *next_epoch,
    bool *available,
    uint64_t *out_epoch
)
{
    if (next_epoch == NULL || available == NULL || out_epoch == NULL ||
        !*available || *next_epoch == 0U) {
        return false;
    }
    *out_epoch = *next_epoch;
    if (*next_epoch == UINT64_MAX) {
        *available = false;
    } else {
        ++*next_epoch;
    }
    return true;
}

bool meiso_composition_edge_simulator_private_candidate_nonce(
    const MeisoCompositionCoordinatorPlanRef *ref,
    uint8_t nonce[MEISO_SHA256_DIGEST_SIZE]
)
{
    size_t index;

    if (ref == NULL || nonce == NULL || ref->plan_generation == 0U) {
        return false;
    }
    for (index = 0U; index < MEISO_SHA256_DIGEST_SIZE; ++index) {
        nonce[index] = (uint8_t)(
            ref->candidate_digest[index] ^
            (uint8_t)(ref->plan_generation >> ((index % 8U) * 8U)) ^
            (uint8_t)(index + 1U)
        );
    }
    nonce[0] |= 1U;
    return true;
}

bool meiso_composition_edge_simulator_private_refresh_presentation_status(MeisoEdgeHandle *handle)
{
    MeisoCompositionScenePresentationStatus status;
    MeisoCompositionEdgePresentationStatus public_status;

    if (!handle->simulator_composition_initialized ||
        !meiso_composition_coordinator_status(
            handle->simulator_coordinator, &status
        )) {
        return false;
    }
    memset(&public_status, 0, sizeof(public_status));
    public_status.capability_generation = status.capability_generation;
    public_status.candidate_state = status.candidate_state;
    public_status.candidate_plan_generation =
        status.candidate_plan_generation;
    memmove(public_status.candidate_digest, status.candidate_digest,
            sizeof(public_status.candidate_digest));
    public_status.active_state = status.active_state;
    public_status.active_plan_generation = status.active_plan_generation;
    memmove(public_status.active_digest, status.active_digest,
            sizeof(public_status.active_digest));
    public_status.reason = status.reason;
    public_status.target_width = status.target_width;
    public_status.target_height = status.target_height;
    public_status.color_format = status.color_format;
    public_status.depth_format = status.depth_format;
    handle->simulator_snapshot.presentation_status_present = true;
    handle->simulator_snapshot.presentation_status = public_status;
    return true;
}

bool meiso_composition_edge_simulator_private_refresh_scene_status(MeisoEdgeHandle *handle)
{
    MeisoCompositionSceneResourceUsage usage;
    MeisoCompositionSceneStatus status;
    MeisoCompositionCpuRasterSnapshot renderer_snapshot;
    size_t blob_count;

    if (!handle->simulator_composition_initialized) {
        return false;
    }
    blob_count = meiso_blob_store_count(handle->backend_runtime.simulator_blob_store);
    if (blob_count > UINT32_MAX) {
        return false;
    }
    memset(&usage, 0, sizeof(usage));
    usage.published_blob_count = (uint32_t)blob_count;
    usage.published_blob_bytes = handle->simulator_published_blob_bytes;
    if (handle->simulator_cpu_raster_enabled) {
        if (!meiso_composition_cpu_raster_renderer_snapshot(
                handle->simulator_renderer, &renderer_snapshot
            )) {
            return false;
        }
        usage.target_slots_used = renderer_snapshot.target_slots_allocated;
        usage.target_bytes = renderer_snapshot.target_bytes;
        usage.render_jobs_in_flight = renderer_snapshot.queued_jobs;
    }
    usage.result_queue_depth =
        handle->simulator_snapshot.inline_streams[
            MEISO_COMPOSITION_EDGE_INLINE_RENDER_RESULT
        ].present ? 1U : 0U;
    if (!meiso_composition_scene_store_status(
            handle->simulator_scene_store, &usage, &status
        )) {
        return false;
    }
    handle->simulator_snapshot.scene_status_present = true;
    handle->simulator_snapshot.scene_status = status;
    return true;
}

bool meiso_composition_edge_simulator_private_refresh_composition_status(MeisoEdgeHandle *handle)
{
    return meiso_composition_edge_simulator_private_refresh_presentation_status(handle) &&
        meiso_composition_edge_simulator_private_refresh_scene_status(handle);
}
