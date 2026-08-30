/* Typed Host commands and candidate-first Desired/Profile publication. */
#include "meiso_composition_host_runtime_private.h"

#include <string.h>

static MeisoOwnerScope host_owner(const MeisoCompositionHostRuntime *runtime)
{
    MeisoOwnerScope owner;
    owner.role = MEISO_OWNER_ROLE_HOST;
    owner.incarnation = runtime->config.local_incarnation;
    return owner;
}

static MeisoCompositionHostRuntimeStatus publish_desired_profile(
    MeisoCompositionHostRuntime *runtime,
    uint8_t channel,
    MeisoCompositionHostRuntimeCommandKind kind,
    bool enabled,
    MeisoByteView profile,
    const MeisoCompositionScenePresentationPlan *plan,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
)
{
    MeisoRuntimeDesiredState desired;
    MeisoByteView payload;
    MeisoCompositionHostRuntimeCommandReceipt receipt;
    MeisoCompositionHostRuntimePrivateDesiredChannel *target;
    MeisoStatePublishStatus publish_status;
    uint64_t version;
    uint64_t command_id;
    bool replaced_local;

    if (meiso_composition_host_runtime_private_mutation_status(runtime) != MEISO_COMPOSITION_HOST_RUNTIME_OK ||
        out_receipt == NULL || channel >= MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT ||
        meiso_composition_host_runtime_private_range_overlap(runtime, out_receipt, sizeof(*out_receipt)) ||
        (profile.size != 0U && profile.data == NULL)) {
        return meiso_composition_host_runtime_private_mutation_status(runtime) == MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL ?
            MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL :
            MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    target = &runtime->desired[channel];
    if (runtime->next_command_id == UINT64_MAX ||
        target->snapshot.state_version == UINT64_MAX) {
        return MEISO_COMPOSITION_HOST_RUNTIME_COUNTER_EXHAUSTED;
    }
    command_id = runtime->next_command_id;
    version = target->snapshot.state_version + 1U;
    replaced_local = target->snapshot.requested &&
        target->snapshot.delivery_pending &&
        (!target->inflight_present ||
         target->snapshot.command_id != target->inflight_command_id);
    desired.enabled = enabled;
    desired.profile = profile;
    if (!meiso_runtime_desired_state_encode(
            &desired,
            (MeisoMutableBuffer){runtime->desired_scratch,
                                 sizeof(runtime->desired_scratch)},
            &payload
        )) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    runtime->mutable_active = true;
    publish_status = meiso_state_publisher_publish(
        target->publisher, host_owner(runtime), version, payload
    );
    runtime->mutable_active = false;
    if (publish_status != MEISO_STATE_PUBLISH_NEW &&
        publish_status != MEISO_STATE_PUBLISH_REPLACED_PENDING) {
        return publish_status == MEISO_STATE_PUBLISH_CAPACITY ?
            MEISO_COMPOSITION_HOST_RUNTIME_CAPACITY :
            MEISO_COMPOSITION_HOST_RUNTIME_INTERNAL_FAILURE;
    }
    memset(&receipt, 0, sizeof(receipt));
    receipt.command_id = command_id;
    receipt.kind = kind;
    receipt.local_result = !replaced_local ?
        MEISO_COMPOSITION_HOST_RUNTIME_LOCAL_QUEUED :
        MEISO_COMPOSITION_HOST_RUNTIME_LOCAL_REPLACED_PENDING;
    if (replaced_local) {
        receipt.replaced_command_present = true;
        receipt.replaced_command_id = target->snapshot.command_id;
        ++target->snapshot.replaced_pending_count;
    }
    receipt.desired_state_present = true;
    receipt.desired_channel = channel;
    receipt.desired_state_version = version;
    if (plan != NULL) {
        receipt.plan_generation_present = true;
        receipt.plan_generation = plan->plan_generation;
        target->plan_present = true;
        target->plan = *plan;
        target->plan_echoed = false;
        runtime->plan_generation_high_water = plan->plan_generation;
    } else {
        target->plan_present = false;
        target->plan_echoed = false;
        memset(&target->plan, 0, sizeof(target->plan));
    }
    target->snapshot.requested = true;
    target->snapshot.enabled = enabled;
    target->snapshot.command_id = command_id;
    target->snapshot.owner = host_owner(runtime);
    target->snapshot.state_version = version;
    target->snapshot.delivery_pending = true;
    target->snapshot.delivery_accepted = false;
    target->snapshot.delivery_failed = false;
    runtime->next_command_id = command_id + 1U;
    *out_receipt = receipt;
    return MEISO_COMPOSITION_HOST_RUNTIME_OK;
}

MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_put_mesh(
    MeisoCompositionHostRuntime *runtime,
    MeisoByteView canonical_mesh,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
)
{
    MeisoCompositionHostRuntimeCommandReceipt receipt;
    MeisoContentId content_id;
    uint64_t transfer_id_high_water;
    MeisoCompositionHostRuntimeStatus status = meiso_composition_host_runtime_private_mutation_status(runtime);
    if (status != MEISO_COMPOSITION_HOST_RUNTIME_OK) {
        return status;
    }
    if (!meiso_composition_host_runtime_private_receipt_valid(runtime, canonical_mesh.data, canonical_mesh.size,
                       out_receipt) || canonical_mesh.data == NULL ||
        canonical_mesh.size == 0U || canonical_mesh.size > runtime->mesh_capacity) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    if (runtime->mesh.queued || runtime->mesh.transfer_active ||
        runtime->mesh.waiting_result) {
        return MEISO_COMPOSITION_HOST_RUNTIME_BUSY;
    }
    if (runtime->next_command_id == UINT64_MAX) {
        return MEISO_COMPOSITION_HOST_RUNTIME_COUNTER_EXHAUSTED;
    }
    if (!meiso_composition_scene_mesh_content_id(
            canonical_mesh, &content_id
        )) {
        return MEISO_COMPOSITION_HOST_RUNTIME_CONFLICT;
    }
    transfer_id_high_water = runtime->mesh.transfer_id_high_water;
    memcpy(runtime->mesh_bytes, canonical_mesh.data, canonical_mesh.size);
    runtime->mesh_size = canonical_mesh.size;
    memset(&runtime->mesh, 0, sizeof(runtime->mesh));
    runtime->mesh.transfer_id_high_water = transfer_id_high_water;
    runtime->mesh.queued = true;
    runtime->mesh.command_id = runtime->next_command_id;
    runtime->mesh.content_id = content_id;
    runtime->mesh.total_bytes = canonical_mesh.size;
    memset(&receipt, 0, sizeof(receipt));
    receipt.command_id = runtime->next_command_id++;
    receipt.kind = MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_PUT_MESH;
    receipt.local_result = MEISO_COMPOSITION_HOST_RUNTIME_LOCAL_QUEUED;
    receipt.mesh_content_id_present = true;
    receipt.mesh_content_id = content_id;
    *out_receipt = receipt;
    return MEISO_COMPOSITION_HOST_RUNTIME_OK;
}

MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_set_scene(
    MeisoCompositionHostRuntime *runtime,
    bool enabled,
    const MeisoCompositionSceneDesired *value,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
)
{
    size_t profile_size = 0U;
    MeisoByteView profile = {NULL, 0U};
    MeisoCompositionHostRuntimeStatus status = meiso_composition_host_runtime_private_mutation_status(runtime);
    if (status != MEISO_COMPOSITION_HOST_RUNTIME_OK) {
        return status;
    }
    if (!meiso_composition_host_runtime_private_receipt_valid(runtime, value, value == NULL ? 0U : sizeof(*value),
                       out_receipt) || enabled != (value != NULL)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    if (enabled) {
        if (meiso_composition_scene_desired_encode(
                value,
                (MeisoMutableBuffer){runtime->profile_scratch,
                                     sizeof(runtime->profile_scratch)},
                &profile_size
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
            return MEISO_COMPOSITION_HOST_RUNTIME_CONFLICT;
        }
        profile.data = runtime->profile_scratch;
        profile.size = profile_size;
    }
    return publish_desired_profile(
        runtime, MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_SCENE,
        MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_SCENE, enabled, profile, NULL,
        out_receipt
    );
}

MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_set_presentation(
    MeisoCompositionHostRuntime *runtime,
    bool enabled,
    const MeisoCompositionHostRuntimePresentationSettings *settings,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
)
{
    MeisoCompositionScenePresentationPlan plan;
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE];
    size_t profile_size = 0U;
    MeisoByteView profile = {NULL, 0U};
    MeisoCompositionHostRuntimeStatus status = meiso_composition_host_runtime_private_mutation_status(runtime);
    if (status != MEISO_COMPOSITION_HOST_RUNTIME_OK) {
        return status;
    }
    if (!meiso_composition_host_runtime_private_receipt_valid(runtime, settings,
                       settings == NULL ? 0U : sizeof(*settings), out_receipt) ||
        enabled != (settings != NULL)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    memset(&plan, 0, sizeof(plan));
    if (enabled) {
        if (runtime->plan_generation_high_water == UINT64_MAX) {
            return MEISO_COMPOSITION_HOST_RUNTIME_COUNTER_EXHAUSTED;
        }
        plan.flags = settings->flags;
        plan.plan_generation = runtime->plan_generation_high_water + 1U;
        plan.capability_generation = runtime->config.catalog_generation;
        plan.scene_binding = runtime->catalog_routes[MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_SCENE_DESIRED]
            .route.endpoint_id;
        plan.source = MEISO_COMPOSITION_SCENE_LAYER_SOURCE_EDGE_NATIVE;
        plan.geometry = MEISO_COMPOSITION_SCENE_GEOMETRY_PROJECTION;
        plan.target_width = settings->target_width;
        plan.target_height = settings->target_height;
        plan.color_format = settings->color_format;
        plan.depth_format = settings->depth_format;
        plan.max_result_hz = settings->max_result_hz;
        plan.offline_lease_ms = settings->offline_lease_ms;
        if (!meiso_composition_scene_plan_digest(&plan, digest)) {
            return MEISO_COMPOSITION_HOST_RUNTIME_CONFLICT;
        }
        memcpy(plan.candidate_digest, digest, sizeof(digest));
        if (meiso_composition_scene_presentation_plan_encode(
                &plan,
                (MeisoMutableBuffer){runtime->profile_scratch,
                                     sizeof(runtime->profile_scratch)},
                &profile_size
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
            return MEISO_COMPOSITION_HOST_RUNTIME_CONFLICT;
        }
        profile.data = runtime->profile_scratch;
        profile.size = profile_size;
    }
    return publish_desired_profile(
        runtime, MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_PRESENTATION,
        MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_PRESENTATION, enabled, profile,
        enabled ? &plan : NULL, out_receipt
    );
}

MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_set_wifi(
    MeisoCompositionHostRuntime *runtime,
    bool enabled,
    const MeisoWifiTelemetryDesiredParameters *parameters,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
)
{
    size_t size = 0U;
    MeisoByteView profile = {NULL, 0U};
    MeisoCompositionHostRuntimeStatus status = meiso_composition_host_runtime_private_mutation_status(runtime);
    if (status != MEISO_COMPOSITION_HOST_RUNTIME_OK) return status;
    if (!meiso_composition_host_runtime_private_receipt_valid(runtime, parameters,
                       parameters == NULL ? 0U : sizeof(*parameters),
                       out_receipt) || enabled != (parameters != NULL)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    if (enabled && (!meiso_wifi_telemetry_parameters_supported(
            &runtime->config.catalog.wifi_capability, parameters) ||
        meiso_wifi_telemetry_desired_parameters_encode(
            parameters, (MeisoMutableBuffer){runtime->profile_scratch,
                                             sizeof(runtime->profile_scratch)},
            &size) != MEISO_WIFI_TELEMETRY_CODEC_OK)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_CONFLICT;
    }
    if (enabled) profile = (MeisoByteView){runtime->profile_scratch, size};
    return publish_desired_profile(
        runtime, MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI,
        MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_WIFI, enabled, profile, NULL,
        out_receipt
    );
}

MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_set_device(
    MeisoCompositionHostRuntime *runtime,
    bool enabled,
    const MeisoDeviceTelemetryParameters *parameters,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
)
{
    size_t size = 0U;
    MeisoByteView profile = {NULL, 0U};
    MeisoCompositionHostRuntimeStatus status = meiso_composition_host_runtime_private_mutation_status(runtime);
    if (status != MEISO_COMPOSITION_HOST_RUNTIME_OK) return status;
    if (!meiso_composition_host_runtime_private_receipt_valid(runtime, parameters,
                       parameters == NULL ? 0U : sizeof(*parameters),
                       out_receipt) || enabled != (parameters != NULL)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    if (enabled && (!meiso_device_telemetry_parameters_supported(
            &runtime->config.catalog.device_telemetry_capability, parameters) ||
        meiso_device_telemetry_parameters_encode(
            parameters, (MeisoMutableBuffer){runtime->profile_scratch,
                                             sizeof(runtime->profile_scratch)},
            &size) != MEISO_DEVICE_TELEMETRY_CODEC_OK)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_CONFLICT;
    }
    if (enabled) profile = (MeisoByteView){runtime->profile_scratch, size};
    return publish_desired_profile(
        runtime, MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_DEVICE,
        MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_DEVICE, enabled, profile, NULL,
        out_receipt
    );
}

static MeisoCompositionHostRuntimeStatus set_media_desired(
    MeisoCompositionHostRuntime *runtime,
    uint8_t channel,
    MeisoCompositionHostRuntimeCommandKind kind,
    const MeisoCompositionMediaCapability *capability,
    bool enabled,
    const MeisoCompositionMediaDesired *desired,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
)
{
    size_t size = 0U;
    MeisoByteView profile = {NULL, 0U};
    MeisoCompositionHostRuntimeStatus status = meiso_composition_host_runtime_private_mutation_status(runtime);
    if (status != MEISO_COMPOSITION_HOST_RUNTIME_OK) return status;
    if (!meiso_composition_host_runtime_private_receipt_valid(runtime, desired, desired == NULL ? 0U : sizeof(*desired),
                       out_receipt) || enabled != (desired != NULL)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    if (enabled && (!meiso_composition_media_desired_supported(
            capability, desired) || meiso_composition_media_desired_encode(
            desired, (MeisoMutableBuffer){runtime->profile_scratch,
                                         sizeof(runtime->profile_scratch)},
            &size) != MEISO_COMPOSITION_PROFILE_OK)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_CONFLICT;
    }
    if (enabled) profile = (MeisoByteView){runtime->profile_scratch, size};
    return publish_desired_profile(
        runtime, channel, kind, enabled, profile, NULL, out_receipt
    );
}

MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_set_render_media(
    MeisoCompositionHostRuntime *runtime,
    bool enabled,
    const MeisoCompositionMediaDesired *desired,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
)
{
    return set_media_desired(
        runtime, MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_RENDER_MEDIA,
        MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_RENDER_MEDIA,
        meiso_composition_host_runtime_private_valid(runtime) ? &runtime->config.catalog.render_media_capability : NULL,
        enabled, desired, out_receipt
    );
}

MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_set_camera_media(
    MeisoCompositionHostRuntime *runtime,
    bool enabled,
    const MeisoCompositionMediaDesired *desired,
    MeisoCompositionHostRuntimeCommandReceipt *out_receipt
)
{
    return set_media_desired(
        runtime, MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_CAMERA_MEDIA,
        MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_CAMERA_MEDIA,
        meiso_composition_host_runtime_private_valid(runtime) ? &runtime->config.catalog.camera_media_capability : NULL,
        enabled, desired, out_receipt
    );
}
