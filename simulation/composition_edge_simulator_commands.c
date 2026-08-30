#include "meiso_composition_edge_simulator_private.h"

/* Simulator command backend owns bounded desired queues and Client dispatch. */

#include <limits.h>
#include <string.h>

static MeisoEdgeStatus meiso_composition_edge_simulator_private_command_preflight(
    const MeisoEdgeHandle *handle
)
{
    if (handle->simulator_snapshot.edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (handle->simulator_next_command_id == UINT64_MAX) {
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    }
    return MEISO_EDGE_OK;
}

static MeisoEdgeStatus meiso_composition_edge_simulator_private_queue_desired(
    MeisoEdgeHandle *handle,
    MeisoCompositionEdgeDesiredChannel channel,
    MeisoEdgeCommandKind kind,
    bool enabled,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoCompositionEdgeDesiredView *desired;
    MeisoEdgeCommandReceipt receipt;
    uint64_t next_version;
    MeisoEdgeStatus status = meiso_composition_edge_simulator_private_command_preflight(handle);

    if (status != MEISO_EDGE_OK) {
        return status;
    }
    desired = &handle->simulator_snapshot.desired[channel];
    if (desired->state_version == UINT64_MAX ||
        (desired->delivery_pending &&
         desired->replaced_pending_count == UINT64_MAX)) {
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    }
    next_version = desired->state_version + 1U;
    memset(&receipt, 0, sizeof(receipt));
    receipt.command_id = ++handle->simulator_next_command_id;
    receipt.kind = kind;
    receipt.local_result = desired->delivery_pending ?
        MEISO_EDGE_LOCAL_REPLACED_PENDING : MEISO_EDGE_LOCAL_QUEUED;
    receipt.replaced_command_present = desired->delivery_pending;
    receipt.replaced_command_id = desired->delivery_pending ?
        desired->command_id : 0U;
    receipt.desired_state_present = true;
    receipt.desired_channel = channel;
    receipt.desired_state_version = next_version;
    *desired = (MeisoCompositionEdgeDesiredView){
        true,
        enabled,
        receipt.command_id,
        handle->simulator_desired_owner,
        next_version,
        false,
        true,
        false,
        false,
        desired->replaced_pending_count +
            (receipt.replaced_command_present ? 1U : 0U),
        false,
        0U,
        {0},
        0U,
        false,
        false,
        false
    };
    memmove(out_receipt, &receipt, sizeof(receipt));
    return MEISO_EDGE_OK;
}

void meiso_composition_edge_simulator_private_apply_actual_state(
    MeisoEdgeHandle *handle,
    MeisoCompositionEdgeDesiredChannel channel,
    bool enabled,
    bool running,
    uint64_t production_epoch
)
{
    MeisoCompositionEdgeDesiredView *desired =
        &handle->simulator_snapshot.desired[channel];
    MeisoCompositionEdgeActualView *actual =
        &handle->simulator_snapshot.actual[channel];
    MeisoRuntimeDesiredReference reference = {
        true, desired->owner, desired->state_version
    };

    desired->delivery_pending = false;
    desired->delivery_accepted = true;
    *actual = (MeisoCompositionEdgeActualView){
        true,
        desired->owner,
        desired->state_version,
        reference,
        reference,
        MEISO_RUNTIME_APPLY_APPLIED,
        MEISO_RUNTIME_AVAILABILITY_PRESENT,
        enabled && running,
        enabled ? (running ? MEISO_RUNTIME_PRODUCTION_RUNNING :
                         MEISO_RUNTIME_PRODUCTION_STARTING) :
            MEISO_RUNTIME_PRODUCTION_STOPPED,
        enabled ? (running ? MEISO_RUNTIME_PRODUCTION_REASON_NONE :
                         MEISO_RUNTIME_PRODUCTION_REASON_APPLYING) :
            MEISO_RUNTIME_PRODUCTION_REASON_DISABLED,
        enabled && running ? production_epoch : 0U,
        true
    };
}

void meiso_composition_edge_simulator_private_apply_actual(
    MeisoEdgeHandle *handle,
    MeisoCompositionEdgeDesiredChannel channel,
    uint64_t production_epoch
)
{
    const bool enabled =
        handle->simulator_snapshot.desired[channel].enabled;

    meiso_composition_edge_simulator_private_apply_actual_state(
        handle, channel, enabled, enabled, production_epoch
    );
}

void meiso_composition_edge_simulator_private_reject_actual(
    MeisoEdgeHandle *handle,
    MeisoCompositionEdgeDesiredChannel channel,
    MeisoRuntimeProductionReason reason
)
{
    MeisoCompositionEdgeDesiredView *desired =
        &handle->simulator_snapshot.desired[channel];
    MeisoCompositionEdgeActualView *actual =
        &handle->simulator_snapshot.actual[channel];

    desired->delivery_pending = false;
    desired->delivery_accepted = true;
    if (!actual->present) {
        memset(actual, 0, sizeof(*actual));
        actual->present = true;
        actual->availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
        actual->production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
    }
    actual->state_owner = desired->owner;
    actual->state_version = desired->state_version;
    actual->observed_desired = (MeisoRuntimeDesiredReference){
        true, desired->owner, desired->state_version
    };
    actual->apply_result = MEISO_RUNTIME_APPLY_REJECTED;
    actual->reason = reason;
    actual->profile_decoded = true;
}

MeisoEdgeStatus meiso_composition_edge_simulator_private_put_mesh(
    MeisoEdgeHandle *handle,
    MeisoByteView canonical_mesh,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoContentId content_id;
    MeisoEdgeCommandReceipt receipt;
    MeisoEdgeStatus status = meiso_composition_edge_simulator_private_command_preflight(handle);

    if (status != MEISO_EDGE_OK) {
        return status;
    }
    if (!handle->simulator_composition_supported) {
        return MEISO_EDGE_UNSUPPORTED;
    }
    if (handle->simulator_mesh_pending) {
        return MEISO_EDGE_BUSY;
    }
    if (canonical_mesh.size == 0U ||
        canonical_mesh.size > handle->simulator_mesh_buffer_capacity) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (!meiso_composition_scene_mesh_content_id(
            canonical_mesh, &content_id
        )) {
        return MEISO_EDGE_CONFLICT;
    }
    memmove(handle->simulator_mesh_buffer, canonical_mesh.data,
            canonical_mesh.size);
    handle->simulator_mesh_pending = true;
    handle->simulator_mesh_size = canonical_mesh.size;
    handle->simulator_mesh_content_id = content_id;
    memset(&handle->simulator_snapshot.mesh, 0,
           sizeof(handle->simulator_snapshot.mesh));
    handle->simulator_snapshot.mesh.queued = true;
    handle->simulator_snapshot.mesh.command_id =
        ++handle->simulator_next_command_id;
    handle->simulator_snapshot.mesh.content_id = content_id;
    handle->simulator_snapshot.mesh.total_bytes = canonical_mesh.size;
    memset(&receipt, 0, sizeof(receipt));
    receipt.command_id = handle->simulator_snapshot.mesh.command_id;
    receipt.kind = MEISO_EDGE_COMMAND_PUT_MESH;
    receipt.local_result = MEISO_EDGE_LOCAL_QUEUED;
    receipt.mesh_content_id_present = true;
    receipt.mesh_content_id = content_id;
    memmove(out_receipt, &receipt, sizeof(receipt));
    return MEISO_EDGE_OK;
}

static bool meiso_composition_edge_simulator_private_scene_value_valid(
    const MeisoCompositionSceneDesired *value
)
{
    uint8_t wire[MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE];
    size_t wire_size = 0U;

    return value != NULL &&
        meiso_composition_scene_desired_encode(
            value, (MeisoMutableBuffer){wire, sizeof(wire)}, &wire_size
        ) == MEISO_COMPOSITION_SCENE_CODEC_OK && wire_size != 0U;
}

MeisoEdgeStatus meiso_composition_edge_simulator_private_set_scene(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionSceneDesired *value,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoEdgeStatus status = meiso_composition_edge_simulator_private_command_preflight(handle);

    if (status != MEISO_EDGE_OK) {
        return status;
    }
    if (!handle->simulator_composition_supported) {
        return MEISO_EDGE_UNSUPPORTED;
    }
    if (enabled != (value != NULL)) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (enabled && !meiso_composition_edge_simulator_private_scene_value_valid(value)) {
        return MEISO_EDGE_CONFLICT;
    }
    if (enabled && !handle->simulator_scene_epoch_available) {
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    }
    status = meiso_composition_edge_simulator_private_queue_desired(
        handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE,
        MEISO_EDGE_COMMAND_SET_SCENE, enabled, out_receipt
    );
    if (status == MEISO_EDGE_OK) {
        handle->simulator_scene_pending.pending = true;
        handle->simulator_scene_pending.enabled = enabled;
        handle->simulator_scene_pending.value = enabled ? *value :
            (MeisoCompositionSceneDesired){0};
    }
    return status;
}

static bool meiso_composition_edge_simulator_private_presentation_plan(
    const MeisoEdgeHandle *handle,
    const MeisoCompositionEdgePresentationSettings *settings,
    uint64_t generation,
    MeisoCompositionScenePresentationPlan *out_plan
)
{
    MeisoCompositionScenePresentationPlan plan;
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE];

    if (handle == NULL || settings == NULL || out_plan == NULL ||
        generation == 0U) {
        return false;
    }
    memset(&plan, 0, sizeof(plan));
    plan.flags = settings->flags;
    plan.plan_generation = generation;
    plan.capability_generation = handle->catalog_generation;
    plan.scene_binding = *meiso_composition_scene_scene_endpoint();
    plan.source = MEISO_COMPOSITION_SCENE_LAYER_SOURCE_EDGE_NATIVE;
    plan.geometry = MEISO_COMPOSITION_SCENE_GEOMETRY_PROJECTION;
    plan.target_width = settings->target_width;
    plan.target_height = settings->target_height;
    plan.color_format = settings->color_format;
    plan.depth_format = settings->depth_format;
    plan.max_result_hz = settings->max_result_hz;
    plan.offline_lease_ms = settings->offline_lease_ms;
    if (!meiso_composition_scene_plan_digest(&plan, digest)) {
        return false;
    }
    memmove(plan.candidate_digest, digest, sizeof(digest));
    *out_plan = plan;
    return true;
}

MeisoEdgeStatus meiso_composition_edge_simulator_private_set_presentation(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionEdgePresentationSettings *settings,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoCompositionScenePresentationPlan plan;
    uint64_t generation = 0U;
    MeisoEdgeStatus status = meiso_composition_edge_simulator_private_command_preflight(handle);

    if (status != MEISO_EDGE_OK) {
        return status;
    }
    if (!handle->simulator_composition_supported) {
        return MEISO_EDGE_UNSUPPORTED;
    }
    if ((enabled && settings == NULL) || (!enabled && settings != NULL)) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    memset(&plan, 0, sizeof(plan));
    if (enabled) {
        if (!handle->simulator_presentation_epoch_available ||
            !handle->simulator_scene_epoch_available ||
            !handle->simulator_plan_generation_available ||
            handle->simulator_snapshot.desired[
                MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION
            ].state_version >= UINT64_MAX - 1U) {
            return MEISO_EDGE_COUNTER_EXHAUSTED;
        }
        generation = handle->simulator_next_plan_generation;
        if (!meiso_composition_edge_simulator_private_presentation_plan(
                handle, settings, generation, &plan
            )) {
            return MEISO_EDGE_CONFLICT;
        }
    }
    status = meiso_composition_edge_simulator_private_queue_desired(
        handle, MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION,
        MEISO_EDGE_COMMAND_SET_PRESENTATION, enabled, out_receipt
    );
    if (status != MEISO_EDGE_OK) {
        return status;
    }
    if (enabled) {
        out_receipt->plan_generation_present = true;
        out_receipt->plan_generation = generation;
        if (generation == UINT64_MAX) {
            handle->simulator_plan_generation_available = false;
        } else {
            ++handle->simulator_next_plan_generation;
        }
    }
    handle->simulator_presentation_pending.pending = true;
    handle->simulator_presentation_pending.enabled = enabled;
    handle->simulator_presentation_pending.settings = enabled ? *settings :
        (MeisoCompositionEdgePresentationSettings){0};
    handle->simulator_presentation_pending.plan = plan;
    handle->simulator_presentation_activation_pending = false;
    return MEISO_EDGE_OK;
}
MeisoEdgeStatus meiso_composition_edge_simulator_private_set_wifi(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoWifiTelemetryDesiredParameters *value,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoEdgeStatus status;

    if ((enabled && (value == NULL ||
                     !meiso_wifi_telemetry_parameters_supported(
                         &handle->simulator_catalog.wifi_capability, value))) ||
        (!enabled && value != NULL)) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (enabled && !handle->simulator_wifi_epoch_available) {
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    }
    status = meiso_composition_edge_simulator_private_queue_desired(
        handle, MEISO_COMPOSITION_EDGE_DESIRED_WIFI,
        MEISO_EDGE_COMMAND_SET_WIFI, enabled, out_receipt
    );
    if (status == MEISO_EDGE_OK) {
        handle->simulator_wifi_pending = true;
        handle->simulator_wifi_enabled = enabled;
        handle->simulator_wifi_desired = enabled ? *value :
            (MeisoWifiTelemetryDesiredParameters){0U, 0U};
    }
    return status;
}

static MeisoEdgeStatus meiso_composition_edge_simulator_private_set_media(
    MeisoEdgeHandle *handle,
    size_t index,
    bool enabled,
    const MeisoCompositionMediaDesired *value,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    const MeisoCompositionMediaCapability *capability = index == 0U ?
        &handle->simulator_catalog.render_media_capability :
        &handle->simulator_catalog.camera_media_capability;
    MeisoCompositionEdgeDesiredChannel channel = index == 0U ?
        MEISO_COMPOSITION_EDGE_DESIRED_RENDER_MEDIA :
        MEISO_COMPOSITION_EDGE_DESIRED_CAMERA_MEDIA;
    MeisoEdgeCommandKind kind = index == 0U ?
        MEISO_EDGE_COMMAND_SET_RENDER_MEDIA :
        MEISO_EDGE_COMMAND_SET_CAMERA_MEDIA;
    MeisoEdgeStatus status;

    if ((enabled && (value == NULL ||
                     !meiso_composition_media_desired_supported(
                         capability, value))) || (!enabled && value != NULL)) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (enabled && !handle->simulator_media_epoch_available[index]) {
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    }
    status = meiso_composition_edge_simulator_private_queue_desired(
        handle, channel, kind, enabled, out_receipt
    );
    if (status == MEISO_EDGE_OK) {
        handle->simulator_media_pending[index].pending = true;
        handle->simulator_media_pending[index].enabled = enabled;
        handle->simulator_media_pending[index].desired = enabled ? *value :
            (MeisoCompositionMediaDesired){0};
    }
    return status;
}

MeisoEdgeStatus meiso_composition_edge_simulator_private_set_device(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoDeviceTelemetryParameters *value,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    MeisoEdgeStatus status;

    status = meiso_composition_edge_simulator_private_command_preflight(handle);
    if (status != MEISO_EDGE_OK) {
        return status;
    }
    if (!handle->simulator_device_supported) {
        return MEISO_EDGE_UNSUPPORTED;
    }
    if ((enabled &&
         (value == NULL ||
          !meiso_device_telemetry_parameters_supported(
              &handle->simulator_catalog.device_telemetry_capability, value
          ))) ||
        (!enabled && value != NULL)) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (enabled && !handle->simulator_device_epoch_available) {
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    }
    status = meiso_composition_edge_simulator_private_queue_desired(
        handle, MEISO_COMPOSITION_EDGE_DESIRED_DEVICE,
        MEISO_EDGE_COMMAND_SET_DEVICE, enabled, out_receipt
    );
    if (status == MEISO_EDGE_OK) {
        handle->simulator_device_pending = true;
        handle->simulator_device_enabled = enabled;
        handle->simulator_device_desired = enabled ? *value :
            (MeisoDeviceTelemetryParameters){0U, 0U};
    }
    return status;
}

MeisoEdgeStatus meiso_composition_edge_simulator_private_set_render_media(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionMediaDesired *value,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    return meiso_composition_edge_simulator_private_set_media(
        handle, 0U, enabled, value, out_receipt
    );
}

MeisoEdgeStatus meiso_composition_edge_simulator_private_set_camera_media(
    MeisoEdgeHandle *handle,
    bool enabled,
    const MeisoCompositionMediaDesired *value,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    return meiso_composition_edge_simulator_private_set_media(
        handle, 1U, enabled, value, out_receipt
    );
}

MeisoEdgeStatus meiso_composition_edge_simulator_private_snapshot(
    const MeisoEdgeHandle *handle,
    MeisoCompositionEdgeSnapshot *out_snapshot
)
{
    memmove(
        out_snapshot, &handle->simulator_snapshot, sizeof(*out_snapshot)
    );
    return MEISO_EDGE_OK;
}

MeisoEdgeStatus meiso_composition_edge_simulator_private_media_latest(
    MeisoEdgeHandle *handle,
    MeisoCompositionMediaSource source,
    MeisoEdgeMediaFrameView *out_frame
)
{
    const int index = meiso_composition_edge_private_media_index(source);
    MeisoCompositionEdgeSimulatorMediaRoute *route =
        &handle->simulator_media[(size_t)index];
    const MeisoEdgeSnapshot *edge = &handle->simulator_snapshot.edge;

    if (edge->phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (!edge->resource_graph_live ||
        !handle->simulator_snapshot.media[(size_t)index]
             .actual_binding_ready) {
        return MEISO_EDGE_NOT_READY;
    }
    if (!route->present) {
        return MEISO_EDGE_NO_DATA;
    }
    if (route->borrowed) {
        return MEISO_EDGE_BUSY;
    }
    route->borrowed = true;
    memmove(out_frame, &route->frame, sizeof(*out_frame));
    return MEISO_EDGE_OK;
}

MeisoEdgeStatus meiso_composition_edge_simulator_private_media_release(
    MeisoEdgeHandle *handle,
    const MeisoEdgeMediaFrameKey *expected_frame
)
{
    const int index = meiso_composition_edge_private_media_index(
        expected_frame->source
    );
    MeisoCompositionEdgeSimulatorMediaRoute *route =
        &handle->simulator_media[(size_t)index];
    const MeisoEdgeSnapshot *edge = &handle->simulator_snapshot.edge;

    if (edge->phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (!edge->resource_graph_live) {
        return MEISO_EDGE_NOT_READY;
    }
    if (!route->borrowed ||
        expected_frame->continuity_serial != handle->continuity_serial ||
        expected_frame->source != route->frame.key.source ||
        expected_frame->media_stream_epoch !=
            route->frame.key.media_stream_epoch ||
        expected_frame->frame_sequence != route->frame.key.frame_sequence) {
        return MEISO_EDGE_STALE;
    }
    route->borrowed = false;
    route->present = false;
    memset(&route->frame, 0, sizeof(route->frame));
    handle->simulator_snapshot.media[(size_t)index].latest_present = false;
    handle->simulator_snapshot.media[(size_t)index]
        .latest_frame_sequence = 0U;
    handle->simulator_snapshot.media[(size_t)index]
        .latest_source_time = (MeisoTimePoint){0U, 0U};
    handle->simulator_snapshot.media[(size_t)index]
        .latest_access_unit_bytes = 0U;
    return MEISO_EDGE_OK;
}

const MeisoCompositionEdgeBackendOps
    meiso_composition_edge_simulator_private_backend_ops = {
        meiso_composition_edge_simulator_private_snapshot,
        meiso_composition_edge_simulator_private_put_mesh,
        meiso_composition_edge_simulator_private_set_scene,
        meiso_composition_edge_simulator_private_set_presentation,
        meiso_composition_edge_simulator_private_set_wifi,
        meiso_composition_edge_simulator_private_set_device,
        meiso_composition_edge_simulator_private_set_render_media,
        meiso_composition_edge_simulator_private_set_camera_media,
        meiso_composition_edge_simulator_private_media_latest,
        meiso_composition_edge_simulator_private_media_release
    };
