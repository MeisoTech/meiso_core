/* Public fairness pump, readiness, snapshots and abort lifecycle. */
#include "meiso_composition_host_runtime_private.h"

#include <string.h>

MeisoCompositionHostRuntimePumpStatus meiso_composition_host_runtime_pump(
    MeisoCompositionHostRuntime *runtime,
    const MeisoCompositionHostRuntimeIoReady *ready,
    MeisoCompositionHostRuntimePumpResult *out_result
)
{
    MeisoCompositionHostRuntimePumpResult result;
    MeisoCompositionHostRuntimePumpStatus status;
    uint8_t scan;
    if (!meiso_composition_host_runtime_private_valid(runtime) || ready == NULL || out_result == NULL ||
        meiso_composition_host_runtime_private_range_overlap(runtime, ready, sizeof(*ready)) ||
        meiso_composition_host_runtime_private_range_overlap(runtime, out_result, sizeof(*out_result)) ||
        meiso_composition_host_runtime_private_ranges_overlap(ready, sizeof(*ready), out_result,
                       sizeof(*out_result))) {
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INVALID_ARGUMENT;
    }
    if (runtime->mutable_active) {
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INVALID_ARGUMENT;
    }
    if (runtime->phase == MEISO_COMPOSITION_HOST_RUNTIME_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_TERMINAL;
    }
    meiso_composition_host_runtime_private_refresh_phase(runtime);
    if (runtime->phase == MEISO_COMPOSITION_HOST_RUNTIME_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_TERMINAL;
    }
    for (scan = 0U; scan < 3U; ++scan) {
        const uint8_t domain = (uint8_t)((runtime->fairness_cursor + scan) % 3U);
        if (domain == 0U) {
            status = meiso_composition_host_runtime_private_pump_local(runtime, &result);
        } else if (domain == 1U) {
            status = meiso_composition_host_runtime_private_pump_media(runtime, ready, &result);
        } else {
            status = meiso_composition_host_runtime_private_pump_control(runtime, ready, &result);
        }
        if (status == MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS) {
            runtime->fairness_cursor = (uint8_t)((domain + 1U) % 3U);
            *out_result = result;
            return status;
        }
        if (status == MEISO_COMPOSITION_HOST_RUNTIME_PUMP_TERMINAL ||
            status == MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INTERNAL_FAILURE) {
            return status;
        }
        if (status == MEISO_COMPOSITION_HOST_RUNTIME_PUMP_WOULD_BLOCK) {
            runtime->fairness_cursor = (uint8_t)((domain + 1U) % 3U);
            return status;
        }
    }
    return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_IDLE;
}

bool meiso_composition_host_runtime_readiness(
    const MeisoCompositionHostRuntime *runtime,
    MeisoCompositionHostRuntimeIoInterest *out_interest
)
{
    MeisoCompositionHostRuntimeIoInterest result;
    if (!meiso_composition_host_runtime_private_valid(runtime) || out_interest == NULL ||
        meiso_composition_host_runtime_private_range_overlap(runtime, out_interest, sizeof(*out_interest)) ||
        runtime->phase == MEISO_COMPOSITION_HOST_RUNTIME_PHASE_TERMINAL) {
        return false;
    }
    memset(&result, 0, sizeof(result));
    result.control_read = meiso_composition_control_session_wants_read(runtime->control_session);
    result.control_write = meiso_composition_control_session_wants_write(runtime->control_session);
    if (runtime->active_media != NULL) {
        result.media_read = meiso_composition_media_posix_tcp_wants_read(
            runtime->active_media->adapter
        );
        result.media_write = meiso_composition_media_posix_tcp_wants_write(
            runtime->active_media->adapter
        );
    }
    *out_interest = result;
    return true;
}

bool meiso_composition_host_runtime_snapshot(
    const MeisoCompositionHostRuntime *runtime,
    MeisoCompositionHostRuntimeSnapshot *out_snapshot
)
{
    MeisoCompositionHostRuntimeSnapshot snapshot;
    MeisoCompositionControlSessionSnapshot control_session;
    MeisoCompositionMediaTransportSnapshot media;
    MeisoCompositionMediaLatestView latest;
    MeisoCompositionMediaRouteConfig current_media_route;
    size_t index;
    if (!meiso_composition_host_runtime_private_valid(runtime) || out_snapshot == NULL ||
        meiso_composition_host_runtime_private_range_overlap(runtime, out_snapshot, sizeof(*out_snapshot)) ||
        !meiso_composition_control_session_snapshot(runtime->control_session, &control_session)) {
        return false;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.phase = runtime->phase;
    snapshot.terminal_reason = runtime->terminal_reason;
    snapshot.control_session_state = control_session.session.state;
    snapshot.control_session_id = control_session.session.session_id;
    snapshot.active_catalog_generation = control_session.runtime.active_catalog_generation;
    snapshot.control_ready = control_session.phase == MEISO_COMPOSITION_CONTROL_SESSION_PHASE_READY;
    snapshot.media_bound = runtime->active_media != NULL;
    snapshot.media_slots_consumed = runtime->media_slots_consumed;
    snapshot.media_slots_capacity = runtime->config.media_connection_slots;
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT; ++index)
        snapshot.desired[index] = runtime->desired[index].snapshot;
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT; ++index)
        snapshot.actual[index] = runtime->actual[index].snapshot;
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_INLINE_STREAM_COUNT;
         ++index)
        snapshot.inline_streams[index] = runtime->inline_streams[index].snapshot;
    snapshot.mesh = runtime->mesh;
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ROUTE_COUNT;
         ++index) {
        snapshot.media[index].source = index == 0U ?
            MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW :
            MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
        if (meiso_composition_host_runtime_private_media_route_config(runtime, &control_session, index, &current_media_route)) {
            snapshot.media[index].actual_binding_ready = true;
            snapshot.media[index].binding = current_media_route.binding;
        }
    }
    if (runtime->active_media != NULL &&
        meiso_composition_media_transport_snapshot(
            runtime->active_media->transport, &media
        ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        snapshot.media_active_route_mask = media.configured_active_route_mask;
        for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ROUTE_COUNT;
             ++index) {
            if ((media.configured_active_route_mask &
                 (uint8_t)(UINT8_C(1) << index)) != 0U) {
                snapshot.media[index].source = media.routes[index].source;
                snapshot.media[index].actual_binding_ready = true;
                snapshot.media[index].binding =
                    runtime->active_media->transport_config.routes[index].binding;
                snapshot.media[index].latest_present =
                    media.routes[index].receive_latest;
                snapshot.media[index].latest_frame_sequence =
                    media.routes[index].receive_frame_sequence;
                snapshot.media[index].published_sequence_high_water =
                    media.routes[index].published_sequence_high_water;
                snapshot.media[index].sequence_gap_frames =
                    media.routes[index].sequence_gap_frames;
                snapshot.media[index].producer_drop_high_water =
                    media.routes[index].producer_drop_high_water;
                snapshot.media[index].reassembly_frames_dropped =
                    media.routes[index].reassembly_frames_dropped;
                snapshot.media[index].unread_latest_frames_dropped =
                    media.routes[index].unread_latest_frames_dropped;
            }
            if ((media.configured_active_route_mask &
                 (uint8_t)(UINT8_C(1) << index)) != 0U &&
                media.routes[index].receive_latest &&
                meiso_composition_media_transport_latest(
                    runtime->active_media->transport,
                    media.routes[index].source, &latest
                ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
                snapshot.media[index].latest_frame_sequence =
                    latest.ref.frame_sequence;
                snapshot.media[index].latest_source_time =
                    latest.unit.source_time;
                snapshot.media[index].latest_access_unit_bytes =
                    (uint32_t)latest.unit.access_unit.size;
            }
        }
    }
    snapshot.wifi_effective_present = runtime->wifi_effective_present;
    snapshot.wifi_effective = runtime->wifi_effective;
    snapshot.wifi_sample_present = runtime->wifi_sample_present;
    snapshot.wifi_sample = runtime->wifi_sample;
    snapshot.presentation_status_present = runtime->presentation_status_present;
    snapshot.presentation_status = runtime->presentation_status;
    snapshot.scene_status_present = runtime->scene_status_present;
    snapshot.scene_status = runtime->scene_status;
    snapshot.render_result_present = runtime->render_result_present;
    snapshot.render_result = runtime->render_result;
    snapshot.device_effective_present = runtime->device_effective_present;
    snapshot.device_effective = runtime->device_effective;
    snapshot.device_sample_present = runtime->device_sample_present;
    snapshot.device_sample = runtime->device_sample;
    snapshot.render_media_actual_present = runtime->render_media_actual_present;
    snapshot.render_media_actual = runtime->render_media_actual;
    snapshot.camera_media_actual_present = runtime->camera_media_actual_present;
    snapshot.camera_media_actual = runtime->camera_media_actual;
    snapshot.next_command_id = runtime->next_command_id;
    snapshot.plan_generation_high_water = runtime->plan_generation_high_water;
    snapshot.arena_bytes = runtime->arena_bytes;
    snapshot.control_session_bytes = runtime->control_session_bytes;
    snapshot.core_state_bytes = runtime->core_state_bytes;
    snapshot.core_stream_bytes = runtime->core_stream_bytes;
    snapshot.mesh_staging_bytes = runtime->mesh_staging_bytes;
    snapshot.media_slot_bytes = runtime->media_slot_bytes;
    *out_snapshot = snapshot;
    return true;
}

MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_abort(
    MeisoCompositionHostRuntime *runtime,
    MeisoCompositionHostRuntimeTerminalReason reason,
    int error_number
)
{
    MeisoCompositionMediaTerminalReason media_reason;
    if (!meiso_composition_host_runtime_private_valid(runtime) || reason == MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_NONE ||
        reason > MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_INTERNAL_FAILURE) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    if (runtime->mutable_active) {
        return MEISO_COMPOSITION_HOST_RUNTIME_BUSY;
    }
    if (runtime->phase == MEISO_COMPOSITION_HOST_RUNTIME_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL;
    }
    if (reason == MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_CATALOG_CHANGED) {
        media_reason = MEISO_COMPOSITION_MEDIA_TERMINAL_CATALOG_CHANGED;
    } else if (reason == MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_LOCAL_ABORT) {
        media_reason = MEISO_COMPOSITION_MEDIA_TERMINAL_LOCAL_CLOSE;
    } else {
        media_reason = MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION;
    }
    runtime->mutable_active = true;
    meiso_composition_host_runtime_private_enter_terminal(runtime, reason, media_reason, error_number);
    runtime->mutable_active = false;
    return MEISO_COMPOSITION_HOST_RUNTIME_OK;
}
