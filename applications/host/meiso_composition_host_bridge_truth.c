#include "meiso_composition_host_bridge_private.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* NDJSON receipts, snapshots, pump events, and typed truth projection. */

static const char *command_kind_name(MeisoEdgeCommandKind value)
{
    switch (value) {
        case MEISO_EDGE_COMMAND_PUT_MESH: return "put_mesh";
        case MEISO_EDGE_COMMAND_SET_SCENE: return "set_scene";
        case MEISO_EDGE_COMMAND_SET_PRESENTATION:
            return "set_presentation";
        case MEISO_EDGE_COMMAND_SET_WIFI: return "set_wifi";
        case MEISO_EDGE_COMMAND_SET_DEVICE: return "set_device";
        case MEISO_EDGE_COMMAND_SET_RENDER_MEDIA:
            return "set_render_media";
        case MEISO_EDGE_COMMAND_SET_CAMERA_MEDIA:
            return "set_camera_media";
        default: return "unknown";
    }
}

static const char *local_result_name(MeisoEdgeLocalQueueResult value)
{
    switch (value) {
        case MEISO_EDGE_LOCAL_QUEUED: return "queued";
        case MEISO_EDGE_LOCAL_REPLACED_PENDING:
            return "replaced_pending";
        default: return "unknown";
    }
}

static const char *phase_name(MeisoCompositionHostRuntimePhase value)
{
    switch (value) {
        case MEISO_COMPOSITION_HOST_RUNTIME_PHASE_HANDSHAKE: return "handshake";
        case MEISO_COMPOSITION_HOST_RUNTIME_PHASE_CATALOG: return "catalog";
        case MEISO_COMPOSITION_HOST_RUNTIME_PHASE_REBIND: return "rebind";
        case MEISO_COMPOSITION_HOST_RUNTIME_PHASE_READY: return "ready";
        case MEISO_COMPOSITION_HOST_RUNTIME_PHASE_TERMINAL: return "terminal";
        default: return "unknown";
    }
}

static const char *terminal_reason_name(
    MeisoCompositionHostRuntimeTerminalReason value
)
{
    switch (value) {
        case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_NONE: return "none";
        case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_CONTROL_TRANSPORT:
            return "control_transport";
        case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_CONTROL_SESSION:
            return "control_session";
        case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_CATALOG_CHANGED:
            return "catalog_changed";
        case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_LOCAL_ABORT:
            return "local_abort";
        case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_INTERNAL_FAILURE:
            return "internal_failure";
        default: return "unknown";
    }
}

static const char *edge_terminal_reason_name(MeisoEdgeTerminalReason value)
{
    switch (value) {
        case MEISO_EDGE_TERMINAL_NONE: return "none";
        case MEISO_EDGE_TERMINAL_TRANSPORT: return "control_transport";
        case MEISO_EDGE_TERMINAL_SESSION: return "control_session";
        case MEISO_EDGE_TERMINAL_CATALOG_DISCONTINUITY:
            return "catalog_changed";
        case MEISO_EDGE_TERMINAL_LOCAL_ABORT: return "local_abort";
        case MEISO_EDGE_TERMINAL_INTERNAL_FAILURE:
            return "internal_failure";
        default: return "unknown";
    }
}

const char *meiso_composition_host_bridge_private_host_terminal_code(const MeisoCompositionHostBridgePrivateProcess *service)
{
    MeisoCompositionHostRuntimeSnapshot snapshot;

    if (service == NULL || service->host_runtime == NULL ||
        !meiso_composition_host_runtime_snapshot(service->host_runtime, &snapshot)) {
        return "HOST_RUNTIME_INTERNAL_TERMINAL";
    }
    switch (snapshot.terminal_reason) {
        case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_CONTROL_TRANSPORT:
            return "HOST_RUNTIME_TRANSPORT_TERMINAL";
        case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_CONTROL_SESSION:
            return "HOST_RUNTIME_SESSION_TERMINAL";
        case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_CATALOG_CHANGED:
            return "HOST_RUNTIME_CATALOG_TERMINAL";
        case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_LOCAL_ABORT:
            return "HOST_RUNTIME_LOCAL_TERMINAL";
        case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_INTERNAL_FAILURE:
        case MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_NONE:
        default:
            return "HOST_RUNTIME_INTERNAL_TERMINAL";
    }
}

static const char *apply_result_name(MeisoRuntimeApplyResult value)
{
    switch (value) {
        case MEISO_RUNTIME_APPLY_NONE: return "none";
        case MEISO_RUNTIME_APPLY_PENDING: return "pending";
        case MEISO_RUNTIME_APPLY_APPLIED: return "applied";
        case MEISO_RUNTIME_APPLY_REJECTED: return "rejected";
        case MEISO_RUNTIME_APPLY_FAILED: return "failed";
        default: return "unknown";
    }
}

static const char *availability_name(MeisoRuntimeAvailability value)
{
    switch (value) {
        case MEISO_RUNTIME_AVAILABILITY_PRESENT: return "present";
        case MEISO_RUNTIME_AVAILABILITY_ABSENT: return "absent";
        case MEISO_RUNTIME_AVAILABILITY_UNAVAILABLE: return "unavailable";
        default: return "unknown";
    }
}

static const char *production_status_name(MeisoRuntimeProductionStatus value)
{
    switch (value) {
        case MEISO_RUNTIME_PRODUCTION_STOPPED: return "stopped";
        case MEISO_RUNTIME_PRODUCTION_STARTING: return "starting";
        case MEISO_RUNTIME_PRODUCTION_RUNNING: return "running";
        case MEISO_RUNTIME_PRODUCTION_DEGRADED: return "degraded";
        case MEISO_RUNTIME_PRODUCTION_STOPPING: return "stopping";
        case MEISO_RUNTIME_PRODUCTION_FAILED: return "failed";
        default: return "unknown";
    }
}

static const char *production_reason_name(MeisoRuntimeProductionReason value)
{
    switch (value) {
        case MEISO_RUNTIME_PRODUCTION_REASON_NONE: return "none";
        case MEISO_RUNTIME_PRODUCTION_REASON_DISABLED: return "disabled";
        case MEISO_RUNTIME_PRODUCTION_REASON_APPLYING: return "applying";
        case MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER:
            return "unsupported_parameter";
        case MEISO_RUNTIME_PRODUCTION_REASON_ADMISSION_DENIED:
            return "admission_denied";
        case MEISO_RUNTIME_PRODUCTION_REASON_UNAVAILABLE: return "unavailable";
        case MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED: return "apply_failed";
        default: return "unknown";
    }
}

static const char *scene_reason_name(MeisoCompositionSceneStableReason value)
{
    switch (value) {
        case MEISO_COMPOSITION_SCENE_REASON_NONE: return "none";
        case MEISO_COMPOSITION_SCENE_REASON_READY: return "ready";
        case MEISO_COMPOSITION_SCENE_REASON_WAITING_BLOB: return "waiting_blob";
        case MEISO_COMPOSITION_SCENE_REASON_OBJECT_CAPACITY:
            return "object_capacity";
        case MEISO_COMPOSITION_SCENE_REASON_BLOB_CAPACITY:
            return "blob_capacity";
        case MEISO_COMPOSITION_SCENE_REASON_MESH_BYTES_CAPACITY:
            return "mesh_bytes_capacity";
        case MEISO_COMPOSITION_SCENE_REASON_BUFFER_CAPACITY:
            return "buffer_capacity";
        case MEISO_COMPOSITION_SCENE_REASON_INVALID_SCENE:
            return "invalid_scene";
        case MEISO_COMPOSITION_SCENE_REASON_RENDERER_FAILED:
            return "renderer_failed";
        default: return "unknown";
    }
}

static const char *candidate_state_name(MeisoCompositionSceneCandidateState value)
{
    switch (value) {
        case MEISO_COMPOSITION_SCENE_CANDIDATE_NONE: return "none";
        case MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARING: return "preparing";
        case MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED: return "prepared";
        case MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING:
            return "activation_pending";
        case MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED: return "rejected";
        case MEISO_COMPOSITION_SCENE_CANDIDATE_FAILED: return "failed";
        default: return "unknown";
    }
}

static const char *active_state_name(MeisoCompositionSceneActiveState value)
{
    switch (value) {
        case MEISO_COMPOSITION_SCENE_ACTIVE_NONE: return "none";
        case MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE: return "active";
        case MEISO_COMPOSITION_SCENE_ACTIVE_RETIRING: return "retiring";
        case MEISO_COMPOSITION_SCENE_ACTIVE_SAFETY: return "safety";
        default: return "unknown";
    }
}

static const char *presentation_reason_name(
    MeisoCompositionScenePresentationReason value
)
{
    switch (value) {
        case MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE: return "none";
        case MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_WAITING_SCENE:
            return "waiting_scene";
        case MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_CAPACITY:
            return "capacity";
        case MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN:
            return "invalid_plan";
        case MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_RENDERER_FAILED:
            return "renderer_failed";
        default: return "unknown";
    }
}

static const char *scene_color_format_name(
    MeisoCompositionSceneColorFormat value
)
{
    switch (value) {
        case MEISO_COMPOSITION_SCENE_COLOR_RGBA8888: return "rgba8888";
        case MEISO_COMPOSITION_SCENE_COLOR_RGB565: return "rgb565";
        default: return "unknown";
    }
}

static const char *scene_depth_format_name(
    MeisoCompositionSceneDepthFormat value
)
{
    return value == MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32 ?
        "implementation32" : "unknown";
}

static void sha256_hex(const uint8_t digest[MEISO_SHA256_DIGEST_SIZE], char out[65])
{
    static const char digits[] = "0123456789abcdef";
    size_t index;

    for (index = 0U; index < MEISO_SHA256_DIGEST_SIZE; ++index) {
        out[index * 2U] = digits[digest[index] >> 4U];
        out[index * 2U + 1U] = digits[digest[index] & 0x0fU];
    }
    out[64] = '\0';
}

static const char *actual_channel_name(MeisoCompositionHostRuntimeActualChannel value)
{
    static const char *const names[] = {
        "wifi", "presentation", "scene", "device", "render_media",
        "camera_media"
    };
    return value < MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT ? names[value] :
        "unknown";
}

static const char *desired_channel_name(MeisoCompositionHostRuntimeDesiredChannel value)
{
    return actual_channel_name((MeisoCompositionHostRuntimeActualChannel)value);
}

static const char *inline_channel_name(MeisoCompositionHostRuntimeInlineChannel value)
{
    static const char *const names[] = {"wifi", "render_result", "device"};
    return value < MEISO_COMPOSITION_HOST_RUNTIME_INLINE_STREAM_COUNT ? names[value] :
        "unknown";
}

const char *meiso_composition_host_bridge_private_media_source_name(MeisoCompositionMediaSource value)
{
    switch (value) {
        case MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW: return "render";
        case MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA: return "camera";
        default: return "unknown";
    }
}

const char *meiso_composition_host_bridge_private_pixel_format_name(MeisoCompositionMediaPixelFormat value)
{
    switch (value) {
        case MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888: return "rgba8888";
        case MEISO_COMPOSITION_MEDIA_PIXEL_RGB565: return "rgb565";
        case MEISO_COMPOSITION_MEDIA_PIXEL_NV12: return "nv12";
        case MEISO_COMPOSITION_MEDIA_PIXEL_YUYV: return "yuyv";
        default: return "unknown";
    }
}

const char *meiso_composition_host_bridge_private_codec_name(MeisoCompositionMediaCodec value)
{
    return value == MEISO_COMPOSITION_MEDIA_CODEC_RAW ? "raw" : "unknown";
}

static const char *media_mask_name(MeisoCompositionHostRuntimeMediaRouteMask value)
{
    switch (value) {
        case 0U: return "none";
        case MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_RENDER: return "render";
        case MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_CAMERA: return "camera";
        case MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ALL: return "both";
        default: return "unknown";
    }
}

static const char *pump_event_name(MeisoCompositionHostRuntimePumpEvent value)
{
    switch (value) {
        case MEISO_COMPOSITION_HOST_RUNTIME_PUMP_CONTROL_PROGRESS:
            return "control_progress";
        case MEISO_COMPOSITION_HOST_RUNTIME_PUMP_DESIRED_DELIVERY:
            return "desired_delivery";
        case MEISO_COMPOSITION_HOST_RUNTIME_PUMP_ACTUAL_UPDATED:
            return "actual_updated";
        case MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INLINE_UPDATED:
            return "inline_updated";
        case MEISO_COMPOSITION_HOST_RUNTIME_PUMP_MESH_PROGRESS:
            return "mesh_progress";
        case MEISO_COMPOSITION_HOST_RUNTIME_PUMP_MEDIA_PROGRESS:
            return "media_progress";
        case MEISO_COMPOSITION_HOST_RUNTIME_PUMP_MEDIA_FRAME:
            return "media_frame";
        case MEISO_COMPOSITION_HOST_RUNTIME_PUMP_BOUNDARY: return "boundary";
        case MEISO_COMPOSITION_HOST_RUNTIME_PUMP_EVENT_TERMINAL: return "terminal";
        default: return "unknown";
    }
}

bool meiso_composition_host_bridge_private_emit_line(const char *line)
{
    return line != NULL && fputs(line, stdout) >= 0 && fputc('\n', stdout) != EOF &&
        fflush(stdout) == 0;
}

bool meiso_composition_host_bridge_private_emit_error(uint64_t request_id, const char *code)
{
    char line[256];
    int written;

    written = snprintf(
        line, sizeof(line),
        "{\"type\":\"error\",\"request_id\":%" PRIu64
        ",\"code\":\"%s\"}", request_id, code
    );
    return written > 0 && (size_t)written < sizeof(line) && meiso_composition_host_bridge_private_emit_line(line);
}

bool meiso_composition_host_bridge_private_emit_receipt(
    uint64_t request_id,
    const MeisoEdgeCommandReceipt *receipt
)
{
    char line[640];
    int written;

    written = snprintf(
        line, sizeof(line),
        "{\"type\":\"command.receipt\",\"request_id\":%" PRIu64
        ",\"command_id\":%" PRIu64 ",\"kind\":\"%s\","
        "\"local_result\":\"%s\",\"replaced\":%s,"
        "\"replaced_command_id\":%" PRIu64 ","
        "\"state_version\":%" PRIu64 ","
        "\"plan_generation\":%" PRIu64 "}",
        request_id, receipt->command_id, command_kind_name(receipt->kind),
        local_result_name(receipt->local_result),
        receipt->replaced_command_present ? "true" : "false",
        receipt->replaced_command_id,
        receipt->desired_state_present ? receipt->desired_state_version : 0U,
        receipt->plan_generation_present ? receipt->plan_generation : 0U
    );
    return written > 0 && (size_t)written < sizeof(line) && meiso_composition_host_bridge_private_emit_line(line);
}

const char *meiso_composition_host_bridge_private_edge_status_code(MeisoEdgeStatus status)
{
    switch (status) {
        case MEISO_EDGE_INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case MEISO_EDGE_STALE:
            return "INVALID_STATE";
        case MEISO_EDGE_CAPACITY:
            return "CAPACITY";
        case MEISO_EDGE_BUSY:
            return "BUSY";
        case MEISO_EDGE_CONFLICT:
            return "CONFLICT";
        case MEISO_EDGE_NOT_READY:
            return "NOT_READY";
        case MEISO_EDGE_NO_DATA:
            return "NO_FRAME";
        case MEISO_EDGE_COUNTER_EXHAUSTED:
            return "COUNTER_EXHAUSTED";
        case MEISO_EDGE_TERMINAL:
            return "TERMINAL";
        case MEISO_EDGE_UNSUPPORTED:
            return "UNSUPPORTED";
        default:
            return "INTERNAL_FAILURE";
    }
}

bool meiso_composition_host_bridge_private_emit_snapshot(MeisoCompositionHostBridgePrivateProcess *service, uint64_t request_id)
{
    MeisoCompositionHostRuntimeSnapshot integration;
    MeisoCompositionEdgeSnapshot snapshot;
    const char *phase;
    const char *terminal_reason;
    bool control_ready;
    bool media_bound;
    MeisoCompositionHostRuntimeMediaRouteMask media_mask = 0U;
    uint32_t media_slots_consumed = 0U;
    char line[2048];
    int written;

    if (meiso_composition_edge_snapshot(service->edge, &snapshot) !=
        MEISO_EDGE_OK) {
        return meiso_composition_host_bridge_private_emit_error(request_id, "SNAPSHOT_FAILED");
    }
    if (service->backend == MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_BACKEND_SIMULATOR) {
        phase = snapshot.edge.phase == MEISO_EDGE_PHASE_READY ? "ready" :
            snapshot.edge.phase == MEISO_EDGE_PHASE_TERMINAL ? "terminal" :
            "handshake";
        terminal_reason = edge_terminal_reason_name(
            snapshot.edge.terminal_reason
        );
        control_ready = false;
        media_bound = false;
    } else {
        if (!meiso_composition_host_runtime_snapshot(service->host_runtime,
                                                  &integration)) {
            return meiso_composition_host_bridge_private_emit_error(request_id, "SNAPSHOT_FAILED");
        }
        phase = phase_name(integration.phase);
        terminal_reason = terminal_reason_name(integration.terminal_reason);
        control_ready = integration.control_ready;
        media_bound = integration.media_bound;
        media_mask = integration.media_active_route_mask;
        media_slots_consumed = integration.media_slots_consumed;
    }
    written = snprintf(
        line, sizeof(line),
        "{\"type\":\"snapshot\",\"request_id\":%" PRIu64
        ",\"phase\":\"%s\",\"terminal_reason\":\"%s\","
        "\"control_ready\":%s,"
        "\"catalog_generation\":%" PRIu64 ",\"media_bound\":%s,"
        "\"media_mask\":\"%s\",\"media_slots_consumed\":%u,"
        "\"mesh_active\":%s,\"mesh_next_offset\":%" PRIu64 ","
        "\"wifi_actual\":%s,\"presentation_actual\":%s,"
        "\"scene_actual\":%s,\"device_actual\":%s,"
        "\"render_actual\":%s,\"camera_actual\":%s,"
        "\"wifi_sample\":%s,\"render_result\":%s,"
        "\"device_sample\":%s,\"render_frame\":%s,"
        "\"camera_frame\":%s}",
        request_id, phase, terminal_reason,
        control_ready ? "true" : "false",
        snapshot.edge.catalog_generation,
        media_bound ? "true" : "false",
        media_mask_name(media_mask),
        (unsigned)media_slots_consumed,
        snapshot.mesh.transfer_active ? "true" : "false",
        snapshot.mesh.next_offset,
        snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI].present ?
            "true" : "false",
        snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_PRESENTATION].present ?
            "true" : "false",
        snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_SCENE].present ?
            "true" : "false",
        snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_DEVICE].present ?
            "true" : "false",
        snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_RENDER_MEDIA].present ?
            "true" : "false",
        snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_CAMERA_MEDIA].present ?
            "true" : "false",
        snapshot.wifi_sample_present ? "true" : "false",
        snapshot.render_result_present ? "true" : "false",
        snapshot.device_sample_present ? "true" : "false",
        snapshot.media[0].latest_present ? "true" : "false",
        snapshot.media[1].latest_present ? "true" : "false"
    );
    return written > 0 && (size_t)written < sizeof(line) && meiso_composition_host_bridge_private_emit_line(line);
}

bool meiso_composition_host_bridge_private_emit_pump_result(
    const MeisoCompositionHostRuntimePumpResult *result
)
{
    char line[384];
    int written;

    written = snprintf(
        line, sizeof(line),
        "{\"type\":\"pump.event\",\"event\":\"%s\","
        "\"command_id\":%" PRIu64 ",\"desired_channel\":\"%s\","
        "\"actual_channel\":\"%s\",\"inline_channel\":\"%s\","
        "\"media_source\":\"%s\"}",
        pump_event_name(result->event),
        result->command_present ? result->command_id : 0U,
        result->desired_channel_present ?
            desired_channel_name(result->desired_channel) : "none",
        result->actual_channel_present ?
            actual_channel_name(result->actual_channel) : "none",
        result->inline_channel_present ?
            inline_channel_name(result->inline_channel) : "none",
        result->media_source_present ?
            meiso_composition_host_bridge_private_media_source_name(result->media_source) : "none"
    );
    return written > 0 && (size_t)written < sizeof(line) && meiso_composition_host_bridge_private_emit_line(line);
}

bool meiso_composition_host_bridge_private_emit_typed_truth(MeisoCompositionHostBridgePrivateProcess *service)
{
    MeisoCompositionEdgeSnapshot snapshot;
    char line[1024];
    size_t index;
    int written;

    if (meiso_composition_edge_snapshot(service->edge, &snapshot) !=
        MEISO_EDGE_OK) {
        return false;
    }
    for (index = 0U; index < MEISO_COMPOSITION_EDGE_ACTUAL_COUNT;
         ++index) {
        const MeisoCompositionEdgeActualView *actual =
            &snapshot.actual[index];
        if (!actual->present) {
            continue;
        }
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"actual\",\"channel\":\"%s\","
            "\"state_version\":%" PRIu64 ",\"apply_result\":\"%s\","
            "\"availability\":\"%s\",\"profile_decoded\":%s,"
            "\"enabled\":%s,"
            "\"production_status\":\"%s\",\"reason\":\"%s\","
            "\"production_epoch\":%" PRIu64 "}",
            actual_channel_name((MeisoCompositionHostRuntimeActualChannel)index),
            actual->state_version, apply_result_name(actual->apply_result),
            availability_name(actual->availability),
            actual->profile_decoded ? "true" : "false",
            actual->effective_enabled ? "true" : "false",
            production_status_name(actual->production_status),
            production_reason_name(actual->reason),
            actual->production_epoch
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !meiso_composition_host_bridge_private_emit_line(line)) {
            return false;
        }
    }
    if (snapshot.wifi_effective_present) {
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"telemetry.wifi_effective\","
            "\"selected_fields\":%u,\"sample_period_ms\":%u}",
            snapshot.wifi_effective.selected_fields,
            snapshot.wifi_effective.sample_period_ms
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !meiso_composition_host_bridge_private_emit_line(line)) {
            return false;
        }
    }
    if (snapshot.device_effective_present) {
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"telemetry.device_effective\","
            "\"selected_fields\":%u,\"sample_period_ms\":%u}",
            snapshot.device_effective.selected_fields,
            snapshot.device_effective.sample_period_ms
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !meiso_composition_host_bridge_private_emit_line(line)) {
            return false;
        }
    }
    if (snapshot.presentation_status_present) {
        const MeisoCompositionEdgePresentationStatus *presentation =
            &snapshot.presentation_status;
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"presentation.status\","
            "\"candidate_state\":\"%s\","
            "\"candidate_plan_generation\":%" PRIu64 ","
            "\"active_state\":\"%s\","
            "\"active_plan_generation\":%" PRIu64 ","
            "\"reason\":\"%s\",\"target_width\":%u,"
            "\"target_height\":%u,\"color_format\":\"%s\","
            "\"depth_format\":\"%s\"}",
            candidate_state_name(presentation->candidate_state),
            presentation->candidate_plan_generation,
            active_state_name(presentation->active_state),
            presentation->active_plan_generation,
            presentation_reason_name(presentation->reason),
            (unsigned)presentation->target_width,
            (unsigned)presentation->target_height,
            scene_color_format_name(presentation->color_format),
            scene_depth_format_name(presentation->depth_format)
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !meiso_composition_host_bridge_private_emit_line(line)) {
            return false;
        }
    }
    for (index = 0U; index < MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT;
         ++index) {
        const MeisoCompositionEdgeMediaActual *media = index == 0U ?
            &snapshot.render_media_actual : &snapshot.camera_media_actual;
        bool present = index == 0U ? snapshot.render_media_actual_present :
            snapshot.camera_media_actual_present;
        if (!present) {
            continue;
        }
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"media.actual\",\"source\":\"%s\","
            "\"pixel_format\":\"%s\",\"codec\":\"%s\","
            "\"width\":%u,\"height\":%u,\"fps_millihz\":%u,"
            "\"bitrate_bps\":%u,\"gop_frames\":%u,"
            "\"request_key\":%s,\"media_stream_epoch\":%" PRIu64
            ",\"producer_dropped_frames\":%" PRIu64 ","
            "\"queue_depth\":%u,\"recovery_generation\":%u}",
            meiso_composition_host_bridge_private_media_source_name(media->source),
            meiso_composition_host_bridge_private_pixel_format_name(media->effective.pixel_format),
            meiso_composition_host_bridge_private_codec_name(media->effective.codec),
            (unsigned)media->effective.width,
            (unsigned)media->effective.height,
            media->effective.fps_millihz, media->effective.bitrate_bps,
            media->effective.gop_frames,
            (media->effective.recovery_flags &
             MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY) != 0U ?
                "true" : "false",
            media->media_stream_epoch, media->producer_dropped_frames,
            media->queue_depth, media->recovery_generation
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !meiso_composition_host_bridge_private_emit_line(line)) {
            return false;
        }
    }
    if (snapshot.wifi_sample_present) {
        const MeisoWifiTelemetrySample *sample = &snapshot.wifi_sample;
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"telemetry.wifi\",\"selected_fields\":%u,"
            "\"present_fields\":%u,\"link_state\":%s,"
            "\"signal_mbm\":%" PRId32 ",\"rx_bytes\":%" PRIu64
            ",\"tx_bytes\":%" PRIu64 ",\"rx_packets\":%" PRIu64
            ",\"tx_packets\":%" PRIu64 ",\"rx_dropped\":%" PRIu64
            ",\"tx_dropped\":%" PRIu64 ",\"rx_errors\":%" PRIu64
            ",\"tx_errors\":%" PRIu64 "}",
            sample->selected_fields, sample->present_fields,
            sample->link_state ? "true" : "false", sample->signal_mbm,
            sample->rx_bytes, sample->tx_bytes, sample->rx_packets,
            sample->tx_packets, sample->rx_dropped, sample->tx_dropped,
            sample->rx_errors, sample->tx_errors
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !meiso_composition_host_bridge_private_emit_line(line)) {
            return false;
        }
    }
    if (snapshot.device_sample_present) {
        const MeisoDeviceTelemetrySample *sample = &snapshot.device_sample;
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"telemetry.device\","
            "\"selected_fields\":%u,\"present_fields\":%u,"
            "\"temperature_millic\":%" PRId32 ","
            "\"temperature_uncertainty_millic\":%u,"
            "\"cpu_load_ppm\":%u,\"memory_total_bytes\":%" PRIu64
            ",\"memory_available_bytes\":%" PRIu64 "}",
            sample->selected_fields, sample->present_fields,
            sample->temperature_millic,
            sample->temperature_uncertainty_millic,
            sample->cpu_load_ppm, sample->memory_total_bytes,
            sample->memory_available_bytes
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !meiso_composition_host_bridge_private_emit_line(line)) {
            return false;
        }
    }
    if (snapshot.scene_status_present) {
        const MeisoCompositionSceneStatus *scene = &snapshot.scene_status;
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"scene.status\",\"reason\":\"%s\","
            "\"active_plan_generation\":%" PRIu64 ","
            "\"world_generation\":%" PRIu64 ","
            "\"object_count\":%u,\"unique_mesh_count\":%u,"
            "\"resident_mesh_bytes\":%" PRIu64 ","
            "\"render_jobs_in_flight\":%u,\"result_queue_depth\":%u}",
            scene_reason_name(scene->reason), scene->active_plan_generation,
            scene->world_generation, scene->active_object_count,
            scene->unique_mesh_count, scene->resident_mesh_bytes,
            scene->render_jobs_in_flight, scene->result_queue_depth
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !meiso_composition_host_bridge_private_emit_line(line)) {
            return false;
        }
    }
    if (snapshot.render_result_present) {
        const MeisoCompositionSceneRenderResult *render =
            &snapshot.render_result;
        char digest[65];
        sha256_hex(render->full_target_sha256, digest);
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"render.result\","
            "\"result\":\"complete\","
            "\"active_plan_generation\":%" PRIu64 ","
            "\"world_generation\":%" PRIu64 ","
            "\"object_count\":%u,\"unique_mesh_count\":%u,"
            "\"target_width\":%u,\"target_height\":%u,"
            "\"color_format\":\"%s\",\"depth_format\":\"%s\","
            "\"source_clock_epoch\":%" PRIu64 ","
            "\"render_start_monotonic_ns\":%" PRIu64 ","
            "\"render_end_monotonic_ns\":%" PRIu64 ","
            "\"offscreen_complete\":%s,\"thumbnail_included\":%s,"
            "\"fence_complete\":%s,\"display_commit_accepted\":%s,"
            "\"vblank_observed\":%s,\"physical_verified\":%s,"
            "\"full_target_sha256\":\"%s\"}",
            render->active_plan_generation, render->world_generation,
            render->object_count, render->unique_mesh_count,
            (unsigned)render->target_width, (unsigned)render->target_height,
            scene_color_format_name(render->color_format),
            scene_depth_format_name(render->depth_format),
            render->source_clock_epoch, render->render_start_monotonic_ns,
            render->render_end_monotonic_ns,
            (render->evidence_flags &
             MEISO_COMPOSITION_SCENE_EVIDENCE_OFFSCREEN_COMPLETE) != 0U ?
                "true" : "false",
            (render->evidence_flags &
             MEISO_COMPOSITION_SCENE_EVIDENCE_THUMBNAIL_INCLUDED) != 0U ?
                "true" : "false",
            (render->evidence_flags &
             MEISO_COMPOSITION_SCENE_EVIDENCE_FENCE_COMPLETE) != 0U ?
                "true" : "false",
            (render->evidence_flags &
             MEISO_COMPOSITION_SCENE_EVIDENCE_DISPLAY_COMMIT_ACCEPTED) != 0U ?
                "true" : "false",
            (render->evidence_flags &
             MEISO_COMPOSITION_SCENE_EVIDENCE_VBLANK_OBSERVED) != 0U ?
                "true" : "false",
            (render->evidence_flags &
             MEISO_COMPOSITION_SCENE_EVIDENCE_PHYSICAL_VERIFIED) != 0U ?
                "true" : "false",
            digest
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !meiso_composition_host_bridge_private_emit_line(line)) {
            return false;
        }
    }
    return true;
}
