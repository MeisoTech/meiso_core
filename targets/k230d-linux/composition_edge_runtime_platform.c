#define _POSIX_C_SOURCE 200809L

#include "composition_edge_runtime_private.h"

/* Wi-Fi, device telemetry, camera, and thermal progression. */

int meiso_k230d_composition_edge_runtime_private_pump_wifi_sample(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoTimePoint now,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
)
{
    size_t size = 0U;
    uint64_t period_ns;
    EdgeInlineChannel *channel = &runtime->inline_streams[
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_WIFI
    ];
    if (!runtime->wifi_effective_present || !channel->snapshot.enabled ||
        !meiso_k230d_composition_edge_runtime_private_actual_delivery_current(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_WIFI
        ) ||
        meiso_stream_count(channel->stream) >=
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_QUEUE_SLOTS ||
        (runtime->wifi_next_due_ns != 0U &&
         now.monotonic_ns < runtime->wifi_next_due_ns)) {
        return 0;
    }
    if (meiso_wifi_linux_adapter_sample(
            &runtime->wifi, runtime->wifi_effective.selected_fields,
            &runtime->wifi_sample
        ) != MEISO_WIFI_LINUX_ADAPTER_OK ||
        meiso_wifi_telemetry_sample_encode(
            &runtime->wifi_sample,
            (MeisoMutableBuffer){
                runtime->stream_scratch, sizeof(runtime->stream_scratch)
            }, &size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        !meiso_k230d_composition_edge_runtime_private_enqueue_inline_payload(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_WIFI, now,
            (MeisoByteView){runtime->stream_scratch, size}
        )) {
        return -1;
    }
    period_ns = (uint64_t)runtime->wifi_effective.sample_period_ms *
        UINT64_C(1000000);
    if (now.monotonic_ns > UINT64_MAX - period_ns) {
        return -1;
    }
    runtime->wifi_next_due_ns = now.monotonic_ns + period_ns;
    runtime->wifi_sample_present = true;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INLINE_SAMPLE;
    out_result->inline_channel_present = true;
    out_result->inline_channel = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_WIFI;
    return 1;
}

static bool desired_reference_equal(
    MeisoRuntimeDesiredReference lhs,
    MeisoRuntimeDesiredReference rhs
)
{
    return lhs.present == rhs.present && (!lhs.present ||
        (meiso_k230d_composition_edge_runtime_private_owner_equal(lhs.owner, rhs.owner) && lhs.version == rhs.version));
}

int meiso_k230d_composition_edge_runtime_private_pump_device_sample(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoTimePoint now,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
)
{
    EdgeInlineChannel *channel = &runtime->inline_streams[
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_DEVICE
    ];
    const bool temperature_selected =
        (runtime->device_effective.selected_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC) != 0U;
    const MeisoDeviceTelemetryLinuxTemperature *temperature = NULL;
    uint64_t period_ns;
    uint64_t budget_ns;
    size_t size = 0U;
    if (!runtime->device_effective_present || !channel->snapshot.enabled ||
        !meiso_k230d_composition_edge_runtime_private_actual_delivery_current(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_DEVICE
        ) ||
        meiso_stream_count(channel->stream) >=
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_QUEUE_SLOTS ||
        (runtime->device_next_due_ns != 0U &&
         now.monotonic_ns < runtime->device_next_due_ns)) {
        return 0;
    }
    if (temperature_selected && !runtime->thermal_request_pending) {
        if (runtime->next_thermal_request_id == UINT64_MAX) {
            return -1;
        }
        budget_ns = (uint64_t)runtime->config.thermal_worker_budget_ms *
            UINT64_C(1000000);
        if (now.monotonic_ns > UINT64_MAX - budget_ns) {
            return -1;
        }
        memset(&runtime->thermal_request, 0, sizeof(runtime->thermal_request));
        runtime->thermal_request.request_id = runtime->next_thermal_request_id++;
        runtime->thermal_request.observed_desired = runtime->actual[
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_DEVICE
        ].snapshot.observed_desired;
        runtime->thermal_request.production_epoch = runtime->actual[
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_DEVICE
        ].snapshot.production_epoch;
        runtime->thermal_request.sample_due = now;
        runtime->thermal_request.deadline = (MeisoTimePoint){
            now.clock_epoch, now.monotonic_ns + budget_ns
        };
        runtime->thermal_request.reader = runtime->device;
        runtime->thermal_request_pending = true;
        runtime->thermal_request_dispatched = false;
        runtime->thermal_result_present = false;
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_THERMAL_REQUEST;
        return 1;
    }
    if (temperature_selected && runtime->thermal_request_pending &&
        !runtime->thermal_result_present &&
        now.monotonic_ns <= runtime->thermal_request.deadline.monotonic_ns) {
        return 0;
    }
    if (temperature_selected && runtime->thermal_result_present &&
        runtime->thermal_result.read_status ==
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_THERMAL_READ_READY &&
        runtime->thermal_result.request_id == runtime->thermal_request.request_id &&
        runtime->thermal_result.production_epoch ==
            runtime->thermal_request.production_epoch &&
        desired_reference_equal(
            runtime->thermal_result.observed_desired,
            runtime->thermal_request.observed_desired
        )) {
        temperature = &runtime->thermal_result.temperature;
    }
    if (meiso_device_telemetry_linux_adapter_sample(
            &runtime->device, runtime->device_effective.selected_fields,
            temperature, &runtime->device_sample
        ) != MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK ||
        meiso_device_telemetry_sample_encode(
            &runtime->device_sample,
            (MeisoMutableBuffer){
                runtime->stream_scratch, sizeof(runtime->stream_scratch)
            }, &size
        ) != MEISO_DEVICE_TELEMETRY_CODEC_OK ||
        !meiso_k230d_composition_edge_runtime_private_enqueue_inline_payload(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_DEVICE, now,
            (MeisoByteView){runtime->stream_scratch, size}
        )) {
        return -1;
    }
    period_ns = (uint64_t)runtime->device_effective.sample_period_ms *
        UINT64_C(1000000);
    if (now.monotonic_ns > UINT64_MAX - period_ns) {
        return -1;
    }
    runtime->device_next_due_ns = now.monotonic_ns + period_ns;
    runtime->device_sample_present = true;
    runtime->thermal_request_pending = false;
    runtime->thermal_request_dispatched = false;
    runtime->thermal_result_present = false;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INLINE_SAMPLE;
    out_result->inline_channel_present = true;
    out_result->inline_channel = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_DEVICE;
    return 1;
}

bool meiso_k230d_composition_edge_runtime_private_media_route_active(
    const MeisoK230dCompositionEdgeRuntime *runtime,
    uint8_t route_mask
)
{
    MeisoCompositionMediaTransportSnapshot snapshot;
    return runtime->active_media != NULL &&
        (runtime->active_media->active_route_mask & route_mask) != 0U &&
        meiso_composition_media_transport_snapshot(
            runtime->active_media->transport, &snapshot
        ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK &&
        snapshot.phase == MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE;
}

static bool fail_camera_production(
    MeisoK230dCompositionEdgeRuntime *runtime,
    bool unavailable
)
{
    EdgeActualChannel *channel = &runtime->actual[
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA
    ];
    if (!meiso_k230d_composition_edge_runtime_private_stop_camera_bounded(runtime, false)) {
        meiso_k230d_composition_edge_runtime_private_enter_terminal(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CAMERA_CLEANUP, 0
        );
        return false;
    }
    if (runtime->active_media != NULL) {
        meiso_k230d_composition_edge_runtime_private_terminate_active_media(
            runtime, MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED, 0
        );
    }
    channel->snapshot.apply_result = MEISO_RUNTIME_APPLY_FAILED;
    channel->snapshot.availability = unavailable ?
        MEISO_RUNTIME_AVAILABILITY_UNAVAILABLE :
        MEISO_RUNTIME_AVAILABILITY_PRESENT;
    channel->snapshot.effective_enabled = false;
    channel->snapshot.production_status = MEISO_RUNTIME_PRODUCTION_FAILED;
    channel->snapshot.reason = unavailable ?
        MEISO_RUNTIME_PRODUCTION_REASON_UNAVAILABLE :
        MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED;
    channel->snapshot.production_epoch = 0U;
    runtime->camera_media_actual_present = false;
    runtime->camera_media_sequence = 0U;
    meiso_k230d_composition_edge_runtime_private_cadence_reset(&runtime->camera_media_cadence);
    runtime->camera_frame_pending = false;
    runtime->camera_frame_submitted = false;
    memset(&runtime->camera_frame, 0, sizeof(runtime->camera_frame));
    return meiso_k230d_composition_edge_runtime_private_publish_actual(
        runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA
    );
}

int meiso_k230d_composition_edge_runtime_private_pump_camera(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoK230dCompositionEdgeRuntimeIoReady *ready,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
)
{
    MeisoCameraV4l2Snapshot snapshot;
    MeisoCameraV4l2StartStatus start;
    MeisoCameraV4l2PollStatus poll;
    MeisoCameraV4l2ReleaseStatus release;
    MeisoCompositionMediaUnit unit;
    if (!runtime->camera_media_actual_present ||
        !runtime->actual[MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA]
             .snapshot.effective_enabled ||
        !meiso_k230d_composition_edge_runtime_private_media_route_active(runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MEDIA_CAMERA)) {
        if (meiso_camera_v4l2_snapshot(runtime->camera, &snapshot) &&
            snapshot.phase != MEISO_CAMERA_V4L2_PHASE_STOPPED) {
            if (!meiso_k230d_composition_edge_runtime_private_stop_camera_bounded(runtime, true)) {
                return -3;
            }
            runtime->camera_frame_pending = false;
            runtime->camera_frame_submitted = false;
            memset(&runtime->camera_frame, 0, sizeof(runtime->camera_frame));
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_BOUNDARY;
            return 1;
        }
        return 0;
    }
    if (!meiso_camera_v4l2_snapshot(runtime->camera, &snapshot)) {
        return -1;
    }
    if (snapshot.phase == MEISO_CAMERA_V4L2_PHASE_TERMINAL) {
        if (!fail_camera_production(runtime, false)) {
            return runtime->phase == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL ?
                -3 : -1;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_BOUNDARY;
        return 1;
    }
    if (snapshot.phase == MEISO_CAMERA_V4L2_PHASE_STOPPED) {
        start = meiso_camera_v4l2_start(runtime->camera);
        if (start != MEISO_CAMERA_V4L2_START_STARTED &&
            start != MEISO_CAMERA_V4L2_START_UNCHANGED) {
            if (!fail_camera_production(runtime, true)) {
                return runtime->phase ==
                        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL ? -3 : -1;
            }
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_BOUNDARY;
            return 1;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_CAMERA_FRAME;
        return 1;
    }
    if (runtime->camera_frame_pending && !runtime->camera_frame_submitted) {
        if (runtime->camera_media_sequence == UINT64_MAX) {
            return -1;
        }
        memset(&unit, 0, sizeof(unit));
        unit.binding = runtime->active_media->transport_config.routes[1].binding;
        unit.frame_sequence = runtime->camera_media_sequence + 1U;
        unit.source_time = runtime->camera_frame.source_time;
        unit.producer_dropped_frames =
            runtime->camera_frame.producer_dropped_frames;
        unit.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_NV12;
        unit.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
        unit.width = MEISO_CAMERA_V4L2_WIDTH;
        unit.height = MEISO_CAMERA_V4L2_HEIGHT;
        unit.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
        unit.access_unit = runtime->camera_frame.bytes;
        {
            const MeisoCompositionMediaTransportStatus submit_status =
                meiso_composition_media_transport_submit(
                    runtime->active_media->transport, &unit
                );
            if (submit_status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK &&
                submit_status !=
                    MEISO_COMPOSITION_MEDIA_TRANSPORT_LATEST_REPLACED) {
                if (!fail_camera_production(runtime, false)) {
                    return runtime->phase ==
                            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL ?
                        -3 : -1;
                }
                memset(out_result, 0, sizeof(*out_result));
                out_result->event_present = true;
                out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_BOUNDARY;
                return 1;
            }
        }
        runtime->camera_media_sequence = unit.frame_sequence;
        runtime->camera_frame_submitted = true;
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_MEDIA_PROGRESS;
        out_result->media_source_present = true;
        out_result->media_source = MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
        return 1;
    }
    if (runtime->camera_frame_pending && runtime->camera_frame_submitted) {
        release = meiso_camera_v4l2_release_frame(
            runtime->camera, &runtime->camera_frame.ref
        );
        if (release != MEISO_CAMERA_V4L2_RELEASE_RELEASED) {
            if (!fail_camera_production(runtime, false)) {
                return runtime->phase ==
                        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL ? -3 : -1;
            }
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_BOUNDARY;
            return 1;
        }
        runtime->camera_frame_pending = false;
        runtime->camera_frame_submitted = false;
        memset(&runtime->camera_frame, 0, sizeof(runtime->camera_frame));
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_CAMERA_FRAME;
        return 1;
    }
    if (!ready->camera_poll_allowed) {
        return 0;
    }
    if (!meiso_k230d_composition_edge_runtime_private_cadence_ready(
            &runtime->camera_media_cadence,
            runtime->actual[
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA
            ].snapshot.production_epoch,
            runtime->camera_media_actual.effective.fps_millihz,
            ready->now.monotonic_ns
        )) {
        return 0;
    }
    poll = meiso_camera_v4l2_poll_frame(runtime->camera, &runtime->camera_frame);
    if (poll == MEISO_CAMERA_V4L2_POLL_WOULD_BLOCK ||
        poll == MEISO_CAMERA_V4L2_POLL_INTERRUPTED) {
        return -2;
    }
    if (poll != MEISO_CAMERA_V4L2_POLL_FRAME) {
        if (!fail_camera_production(runtime, false)) {
            return runtime->phase == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL ?
                -3 : -1;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_BOUNDARY;
        return 1;
    }
    if (!meiso_k230d_composition_edge_runtime_private_cadence_commit(
            &runtime->camera_media_cadence, ready->now.monotonic_ns
        )) {
        if (!fail_camera_production(runtime, false)) {
            return runtime->phase == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL ?
                -3 : -1;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_BOUNDARY;
        return 1;
    }
    runtime->camera_frame_pending = true;
    runtime->camera_frame_submitted = false;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_CAMERA_FRAME;
    out_result->media_source_present = true;
    out_result->media_source = MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
    return 1;
}

MeisoK230dCompositionEdgeRuntimeStatus
meiso_k230d_composition_edge_runtime_take_thermal_request(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoK230dCompositionEdgeRuntimeThermalRequest *out_request
)
{
    if (!meiso_k230d_composition_edge_runtime_private_edge_runtime_valid(runtime) || out_request == NULL ||
        meiso_k230d_composition_edge_runtime_private_edge_runtime_range_overlap(runtime, out_request, sizeof(*out_request))) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->thermal_request_pending || runtime->thermal_request_dispatched) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_NO_WORK;
    }
    *out_request = runtime->thermal_request;
    runtime->thermal_request_dispatched = true;
    return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_OK;
}

MeisoK230dCompositionEdgeRuntimeThermalReadStatus
meiso_k230d_composition_edge_runtime_run_thermal_request(
    const MeisoK230dCompositionEdgeRuntimeThermalRequest *request,
    MeisoK230dCompositionEdgeRuntimeThermalResult *out_result
)
{
    MeisoK230dCompositionEdgeRuntimeThermalResult value;
    MeisoDeviceTelemetryLinuxAdapterStatus status;
    if (request == NULL || out_result == NULL ||
        meiso_k230d_composition_edge_runtime_private_ranges_overlap(request, sizeof(*request), out_result,
                       sizeof(*out_result)) || request->request_id == 0U) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_THERMAL_READ_INVALID_ARGUMENT;
    }
    memset(&value, 0, sizeof(value));
    value.request_id = request->request_id;
    value.observed_desired = request->observed_desired;
    value.production_epoch = request->production_epoch;
    status = meiso_device_telemetry_linux_adapter_read_temperature_direct(
        &request->reader, &value.temperature
    );
    if (status == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK) {
        value.read_status = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_THERMAL_READ_READY;
    } else if (status == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_UNAVAILABLE) {
        value.read_status = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_THERMAL_READ_UNAVAILABLE;
    } else {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_THERMAL_READ_INVALID_ARGUMENT;
    }
    *out_result = value;
    return value.read_status;
}

MeisoK230dCompositionEdgeRuntimeStatus
meiso_k230d_composition_edge_runtime_submit_thermal_result(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoK230dCompositionEdgeRuntimeThermalResult *result,
    MeisoTimePoint completed_at
)
{
    if (!meiso_k230d_composition_edge_runtime_private_edge_runtime_valid(runtime) || result == NULL ||
        meiso_k230d_composition_edge_runtime_private_edge_runtime_range_overlap(runtime, result, sizeof(*result)) ||
        !meiso_time_point_valid(completed_at)) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INVALID_ARGUMENT;
    }
    if (!runtime->thermal_request_pending ||
        result->request_id != runtime->thermal_request.request_id ||
        !desired_reference_equal(
            result->observed_desired,
            runtime->thermal_request.observed_desired
        ) ||
        result->production_epoch != runtime->thermal_request.production_epoch ||
        (result->read_status !=
             MEISO_K230D_COMPOSITION_EDGE_RUNTIME_THERMAL_READ_READY &&
         result->read_status !=
             MEISO_K230D_COMPOSITION_EDGE_RUNTIME_THERMAL_READ_UNAVAILABLE) ||
        (result->read_status ==
             MEISO_K230D_COMPOSITION_EDGE_RUNTIME_THERMAL_READ_UNAVAILABLE &&
         (result->temperature.present ||
          result->temperature.temperature_millic != 0 ||
          result->temperature.uncertainty_millic != 0U)) ||
        completed_at.clock_epoch != runtime->thermal_request.deadline.clock_epoch ||
        completed_at.monotonic_ns >
            runtime->thermal_request.deadline.monotonic_ns) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_STALE;
    }
    runtime->thermal_result = *result;
    runtime->thermal_result_present = true;
    return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_OK;
}
