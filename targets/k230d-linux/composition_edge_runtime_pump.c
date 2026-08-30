#define _POSIX_C_SOURCE 200809L

#include "composition_edge_runtime_private.h"

/* Fair scheduling, pump entry points, readiness, and snapshots. */

static MeisoK230dCompositionEdgeRuntimePumpStatus pump_local(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoK230dCompositionEdgeRuntimeIoReady *ready,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
)
{
    size_t index;
    int local_status;
    if (runtime->phase != MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_READY) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_IDLE;
    }
    if (meiso_k230d_composition_edge_runtime_private_handle_source_completion(runtime, out_result)) {
        return runtime->phase == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL ?
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_TERMINAL :
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS;
    }
    if (meiso_k230d_composition_edge_runtime_private_edge_runtime_source_work_outstanding(runtime)) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_IDLE;
    }
    for (index = 0U; index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_COUNT;
         ++index) {
        if (meiso_k230d_composition_edge_runtime_private_queue_desired_accept(runtime, index, out_result)) {
            return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS;
        }
    }
    for (index = 0U; index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT;
         ++index) {
        if (meiso_k230d_composition_edge_runtime_private_queue_actual_offer(runtime, index, out_result)) {
            return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS;
        }
    }
    for (index = 0U;
         index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT; ++index) {
        if (meiso_k230d_composition_edge_runtime_private_queue_inline_sample(runtime, index, out_result)) {
            return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS;
        }
    }
    local_status = meiso_k230d_composition_edge_runtime_private_pump_renderer(runtime, ready->now, out_result);
    if (local_status != 0) {
        return local_status > 0 ? MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS :
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INTERNAL_FAILURE;
    }
    local_status = meiso_k230d_composition_edge_runtime_private_pump_camera(runtime, ready, out_result);
    if (local_status != 0) {
        if (local_status > 0) {
            return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS;
        }
        if (local_status == -2) {
            return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_WOULD_BLOCK;
        }
        if (local_status == -3) {
            meiso_k230d_composition_edge_runtime_private_enter_terminal(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CAMERA_CLEANUP, 0
            );
        }
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INTERNAL_FAILURE;
    }
    local_status = meiso_k230d_composition_edge_runtime_private_pump_wifi_sample(runtime, ready->now, out_result);
    if (local_status != 0) {
        return local_status > 0 ? MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS :
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INTERNAL_FAILURE;
    }
    local_status = meiso_k230d_composition_edge_runtime_private_pump_device_sample(runtime, ready->now, out_result);
    if (local_status != 0) {
        return local_status > 0 ? MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS :
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INTERNAL_FAILURE;
    }
    return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_IDLE;
}

static MeisoK230dCompositionEdgeRuntimePumpStatus pump_control(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoK230dCompositionEdgeRuntimeIoReady *ready,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
)
{
    MeisoCompositionControlSessionPumpResult peer_result;
    MeisoCompositionControlSessionPumpStatus status;
    MeisoK230dCompositionEdgeRuntimePumpResult result;
    runtime->mutable_active = true;
    status = meiso_composition_control_session_pump(
        runtime->control_session,
        ready->control_readable && !meiso_k230d_composition_edge_runtime_private_edge_runtime_source_work_outstanding(runtime),
        ready->control_writable,
        &peer_result
    );
    runtime->mutable_active = false;
    if (!meiso_k230d_composition_edge_runtime_private_refresh_phase(runtime)) {
        meiso_k230d_composition_edge_runtime_private_enter_terminal(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_INTERNAL_FAILURE, 0
        );
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INTERNAL_FAILURE;
    }
    if (runtime->phase == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL ||
        status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_TERMINAL) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_TERMINAL;
    }
    if (status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_WOULD_BLOCK ||
        status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_TX_BUSY) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_WOULD_BLOCK;
    }
    if (status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_IDLE) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_IDLE;
    }
    if (status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_EVENT) {
        if (!peer_result.event_present || !meiso_k230d_composition_edge_runtime_private_handle_runtime_event(
                runtime, &peer_result.event, ready->now, &result
            )) {
            meiso_k230d_composition_edge_runtime_private_enter_terminal(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_INTERNAL_FAILURE, 0
            );
            return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INTERNAL_FAILURE;
        }
        *out_result = result;
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS;
    }
    if (status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_RX_OBLIGATION ||
        status > MEISO_COMPOSITION_CONTROL_SESSION_PUMP_SOURCE_INVALID) {
        meiso_k230d_composition_edge_runtime_private_enter_terminal(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_INTERNAL_FAILURE, 0
        );
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INTERNAL_FAILURE;
    }
    memset(&result, 0, sizeof(result));
    result.event_present = true;
    result.event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_CONTROL_PROGRESS;
    *out_result = result;
    return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS;
}

MeisoK230dCompositionEdgeRuntimePumpStatus meiso_k230d_composition_edge_runtime_pump(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoK230dCompositionEdgeRuntimeIoReady *ready,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
)
{
    MeisoK230dCompositionEdgeRuntimePumpResult result;
    MeisoK230dCompositionEdgeRuntimePumpStatus status;
    uint8_t scan;
    if (!meiso_k230d_composition_edge_runtime_private_edge_runtime_valid(runtime) || ready == NULL || out_result == NULL ||
        meiso_k230d_composition_edge_runtime_private_edge_runtime_range_overlap(runtime, ready, sizeof(*ready)) ||
        meiso_k230d_composition_edge_runtime_private_edge_runtime_range_overlap(runtime, out_result, sizeof(*out_result)) ||
        meiso_k230d_composition_edge_runtime_private_ranges_overlap(ready, sizeof(*ready), out_result,
                       sizeof(*out_result))) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INVALID_ARGUMENT;
    }
    if (runtime->mutable_active || !meiso_time_point_valid(ready->now) ||
        ready->now.clock_epoch != runtime->config.edge_clock_epoch) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INVALID_ARGUMENT;
    }
    if (runtime->phase == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_TERMINAL;
    }
    if (!meiso_k230d_composition_edge_runtime_private_refresh_phase(runtime)) {
        meiso_k230d_composition_edge_runtime_private_enter_terminal(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_INTERNAL_FAILURE, 0
        );
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INTERNAL_FAILURE;
    }
    for (scan = 0U; scan < 3U; ++scan) {
        const uint8_t domain = (uint8_t)((runtime->fairness_cursor + scan) % 3U);
        status = domain == 0U ? pump_local(runtime, ready, &result) :
            domain == 1U ? meiso_k230d_composition_edge_runtime_private_pump_media(runtime, ready, &result) :
                           pump_control(runtime, ready, &result);
        if (status == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS) {
            runtime->fairness_cursor = (uint8_t)((domain + 1U) % 3U);
            *out_result = result;
            return status;
        }
        if (status == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_TERMINAL ||
            status == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INTERNAL_FAILURE) {
            if (status == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INTERNAL_FAILURE &&
                runtime->phase != MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL) {
                meiso_k230d_composition_edge_runtime_private_enter_terminal(
                    runtime,
                    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_INTERNAL_FAILURE,
                    0
                );
            }
            return status;
        }
        if (status == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_WOULD_BLOCK) {
            runtime->fairness_cursor = (uint8_t)((domain + 1U) % 3U);
            return status;
        }
    }
    return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_IDLE;
}

bool meiso_k230d_composition_edge_runtime_readiness(
    const MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoK230dCompositionEdgeRuntimeIoInterest *out_interest
)
{
    MeisoK230dCompositionEdgeRuntimeIoInterest value;
    if (!meiso_k230d_composition_edge_runtime_private_edge_runtime_valid(runtime) || out_interest == NULL ||
        meiso_k230d_composition_edge_runtime_private_edge_runtime_range_overlap(runtime, out_interest, sizeof(*out_interest))) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    if (runtime->phase != MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL) {
        value.control_read = !meiso_k230d_composition_edge_runtime_private_edge_runtime_source_work_outstanding(runtime) &&
            meiso_composition_control_session_wants_read(runtime->control_session);
        value.control_write =
            meiso_composition_control_session_wants_write(runtime->control_session);
    }
    if (runtime->active_media != NULL) {
        value.media_read = meiso_composition_media_posix_tcp_wants_read(
            runtime->active_media->adapter
        );
        value.media_write = meiso_composition_media_posix_tcp_wants_write(
            runtime->active_media->adapter
        );
    }
    value.camera_poll_useful =
        runtime->camera_media_actual_present && runtime->active_media != NULL &&
        (runtime->active_media->active_route_mask &
         MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MEDIA_CAMERA) != 0U;
    value.thermal_request_ready = runtime->thermal_request_pending &&
        !runtime->thermal_request_dispatched;
    *out_interest = value;
    return true;
}

bool meiso_k230d_composition_edge_runtime_snapshot(
    const MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoK230dCompositionEdgeRuntimeSnapshot *out_snapshot
)
{
    MeisoK230dCompositionEdgeRuntimeSnapshot value;
    MeisoCompositionControlSessionSnapshot control_session;
    MeisoBlobStats blob_stats;
    size_t index;
    if (!meiso_k230d_composition_edge_runtime_private_edge_runtime_valid(runtime) || out_snapshot == NULL ||
        meiso_k230d_composition_edge_runtime_private_edge_runtime_range_overlap(runtime, out_snapshot, sizeof(*out_snapshot)) ||
        !meiso_composition_control_session_snapshot(runtime->control_session, &control_session) ||
        !meiso_blob_store_stats(runtime->blob_store, &blob_stats)) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.phase = runtime->phase;
    value.terminal_reason = runtime->terminal_reason;
    value.control_session_state = control_session.session.state;
    value.control_session_id = control_session.session.session_id;
    value.active_catalog_generation = control_session.runtime.active_catalog_generation;
    value.control_ready =
        control_session.phase == MEISO_COMPOSITION_CONTROL_SESSION_PHASE_READY;
    for (index = 0U; index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT;
         ++index) {
        value.actual[index] = runtime->actual[index].snapshot;
    }
    for (index = 0U;
         index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT; ++index) {
        value.inline_streams[index] = runtime->inline_streams[index].snapshot;
    }
    value.wifi_effective_present = runtime->wifi_effective_present;
    value.wifi_effective = runtime->wifi_effective;
    value.wifi_sample_present = runtime->wifi_sample_present;
    value.wifi_sample = runtime->wifi_sample;
    value.device_effective_present = runtime->device_effective_present;
    value.device_effective = runtime->device_effective;
    value.device_sample_present = runtime->device_sample_present;
    value.device_sample = runtime->device_sample;
    value.render_media_actual_present = runtime->render_media_actual_present;
    value.render_media_actual = runtime->render_media_actual;
    value.camera_media_actual_present = runtime->camera_media_actual_present;
    value.camera_media_actual = runtime->camera_media_actual;
    value.presentation_status_present = runtime->presentation_status_present;
    value.presentation_status = runtime->presentation_status;
    value.scene_status_present = runtime->scene_status_present;
    value.scene_status = runtime->scene_status;
    (void)meiso_composition_scene_store_control_scope(
        runtime->scene_store, &value.scene_control_scope
    );
    value.active_world_present = meiso_composition_scene_store_active(
        runtime->scene_store, &(MeisoCompositionSceneWorldView){0}
    );
    if (value.active_world_present) {
        MeisoCompositionSceneWorldView world;
        if (meiso_composition_scene_store_active(runtime->scene_store, &world)) {
            value.active_world = world.ref;
        }
    }
    (void)meiso_composition_cpu_raster_renderer_snapshot(
        runtime->renderer, &value.renderer
    );
    (void)meiso_camera_v4l2_snapshot(runtime->camera, &value.camera);
    if (runtime->active_media != NULL) {
        MeisoCompositionMediaTransportSnapshot transport;
        value.media.bound = true;
        value.media.active_route_mask = runtime->active_media->active_route_mask;
        value.media.slot_index = (uint8_t)(runtime->active_media - runtime->media_slots);
        if (meiso_composition_media_transport_snapshot(
                runtime->active_media->transport, &transport
            ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
            value.media.transport_phase = transport.phase;
            value.media.render_submitted_sequence =
                transport.routes[0].submitted_sequence_high_water;
            value.media.camera_submitted_sequence =
                transport.routes[1].submitted_sequence_high_water;
            value.media.render_whole_frames_dropped =
                transport.routes[0].transport_whole_frames_dropped;
            value.media.camera_whole_frames_dropped =
                transport.routes[1].transport_whole_frames_dropped;
        }
    }
    value.blob_published_count = (uint32_t)meiso_blob_store_count(
        runtime->blob_store
    );
    value.blob_published_bytes = runtime->blob_published_bytes;
    value.blob_transfer_id_high_water = runtime->blob_transfer_id_high_water;
    value.blob_transfer = runtime->blob_result_pending ?
        runtime->blob_result_transfer : runtime->blob_receive_transfer;
    value.blob_next_offset = runtime->blob_receive_next_offset;
    value.blob_result_operation = runtime->blob_result_operation;
    value.blob_transfer_active = runtime->blob_receive_active;
    value.blob_begin_ready = runtime->blob_receive_begin_ready;
    value.blob_result_pending = runtime->blob_result_pending;
    value.blob_result_queued = runtime->blob_result_queued;
    value.next_thermal_request_id = runtime->next_thermal_request_id;
    value.thermal_request_pending = runtime->thermal_request_pending;
    value.media_slots_consumed = runtime->media_slots_consumed;
    value.media_slots_capacity = runtime->config.media_connection_slots;
    value.arena_bytes = runtime->arena_bytes;
    value.control_session_bytes = runtime->control_session_bytes;
    value.blob_store_bytes = runtime->blob_store_bytes;
    value.scene_store_bytes = runtime->scene_store_bytes;
    value.coordinator_bytes = runtime->coordinator_bytes;
    value.renderer_bytes = runtime->renderer_bytes;
    value.camera_bytes = runtime->camera_bytes;
    value.core_state_bytes = runtime->core_state_bytes;
    value.core_stream_bytes = runtime->core_stream_bytes;
    value.media_slot_bytes = runtime->media_slot_bytes;
    *out_snapshot = value;
    return true;
}

MeisoK230dCompositionEdgeRuntimeStatus meiso_k230d_composition_edge_runtime_abort(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoK230dCompositionEdgeRuntimeTerminalReason reason,
    int error_number
)
{
    if (!meiso_k230d_composition_edge_runtime_private_edge_runtime_valid(runtime) || reason == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_NONE ||
        reason > MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_INTERNAL_FAILURE) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INVALID_ARGUMENT;
    }
    if (runtime->phase == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL;
    }
    meiso_k230d_composition_edge_runtime_private_enter_terminal(runtime, reason, error_number);
    return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_OK;
}
