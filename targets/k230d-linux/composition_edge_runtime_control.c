#define _POSIX_C_SOURCE 200809L

#include "composition_edge_runtime_private.h"

/* Control routing, Actual publication, and channel coordination. */

bool meiso_k230d_composition_edge_runtime_private_route_channel(
    const MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoRuntimeRouteKey route,
    const uint8_t *route_indices,
    size_t count,
    size_t *out_index
)
{
    size_t index;
    MeisoRuntimeRouteKey expected;
    if (!meiso_k230d_composition_edge_runtime_private_edge_runtime_valid(runtime) || route_indices == NULL || out_index == NULL) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        if (meiso_composition_control_session_route_key(
                runtime->control_session, route_indices[index], &expected
            ) && meiso_k230d_composition_edge_runtime_private_route_equal(route, expected)) {
            *out_index = index;
            return true;
        }
    }
    return false;
}
bool meiso_k230d_composition_edge_runtime_private_ensure_coordinator(MeisoK230dCompositionEdgeRuntime *runtime)
{
    MeisoCompositionControlSessionSnapshot control_session;
    MeisoCompositionCoordinatorConfig config;
    if (runtime->coordinator_initialized) {
        return true;
    }
    if (!meiso_composition_control_session_snapshot(runtime->control_session, &control_session) ||
        control_session.phase != MEISO_COMPOSITION_CONTROL_SESSION_PHASE_READY ||
        !control_session.session.session_id_known) {
        return false;
    }
    memset(&config, 0, sizeof(config));
    config.scene_store = runtime->scene_store;
    config.session_id = control_session.session.session_id;
    config.edge_incarnation = runtime->config.local_incarnation;
    config.capability_generation = runtime->config.catalog_generation;
    config.edge_clock_epoch = runtime->config.edge_clock_epoch;
    if (meiso_composition_coordinator_init(
            runtime->coordinator_arena, runtime->coordinator_bytes, &config,
            &runtime->coordinator
        ) != MEISO_COMPOSITION_COORDINATOR_INIT_OK) {
        return false;
    }
    runtime->coordinator_initialized = true;
    return true;
}

bool meiso_k230d_composition_edge_runtime_private_ensure_desired_owner(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoOwnerScope owner
)
{
    MeisoCompositionSceneControlScope scope;
    MeisoCompositionCoordinatorOwnerStatus coordinator_status;
    MeisoCompositionCpuRasterScopeStatus renderer_status;
    if (owner.role != MEISO_OWNER_ROLE_HOST || owner.incarnation == 0U ||
        !meiso_k230d_composition_edge_runtime_private_ensure_coordinator(runtime)) {
        return false;
    }
    if (runtime->desired_owner_present) {
        return meiso_k230d_composition_edge_runtime_private_owner_equal(runtime->desired_owner, owner);
    }
    coordinator_status = meiso_composition_coordinator_begin_owner(
        runtime->coordinator, owner.incarnation
    );
    if (coordinator_status != MEISO_COMPOSITION_COORDINATOR_OWNER_STARTED ||
        !meiso_composition_scene_store_control_scope(
            runtime->scene_store, &scope
        )) {
        return false;
    }
    renderer_status = meiso_composition_cpu_raster_renderer_begin_control_scope(
        runtime->renderer, &scope
    );
    if (renderer_status != MEISO_COMPOSITION_CPU_RASTER_SCOPE_STARTED &&
        renderer_status != MEISO_COMPOSITION_CPU_RASTER_SCOPE_UNCHANGED) {
        return false;
    }
    runtime->desired_owner = owner;
    runtime->desired_owner_present = true;
    return true;
}

static bool encode_actual_profile(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    MeisoByteView *out_profile
)
{
    size_t size = 0U;
    MeisoCompositionCpuRasterSnapshot renderer;
    MeisoCompositionSceneResourceUsage usage;
    if (runtime == NULL || out_profile == NULL) {
        return false;
    }
    out_profile->data = NULL;
    out_profile->size = 0U;
    if (!runtime->actual[index].snapshot.effective_enabled &&
        index != MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_PRESENTATION &&
        index != MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE) {
        return true;
    }
    switch (index) {
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_WIFI:
        if (!runtime->wifi_effective_present ||
            meiso_wifi_telemetry_effective_parameters_encode(
                &runtime->wifi_effective,
                (MeisoMutableBuffer){
                    runtime->profile_scratch, sizeof(runtime->profile_scratch)
                }, &size
            ) != MEISO_WIFI_TELEMETRY_CODEC_OK) {
            return false;
        }
        break;
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_PRESENTATION:
        if (!runtime->coordinator_initialized ||
            !meiso_composition_coordinator_status(
                runtime->coordinator, &runtime->presentation_status
            ) || meiso_composition_scene_presentation_status_encode(
                &runtime->presentation_status,
                (MeisoMutableBuffer){
                    runtime->profile_scratch, sizeof(runtime->profile_scratch)
                }, &size
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
            return false;
        }
        runtime->presentation_status_present = true;
        break;
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE:
        memset(&usage, 0, sizeof(usage));
        if (!meiso_composition_cpu_raster_renderer_snapshot(
                runtime->renderer, &renderer
            )) {
            return false;
        }
        usage.published_blob_count = (uint32_t)meiso_blob_store_count(
            runtime->blob_store
        );
        usage.published_blob_bytes = runtime->blob_published_bytes;
        usage.target_slots_used = renderer.target_slots_allocated;
        usage.target_bytes = renderer.target_bytes;
        usage.render_jobs_in_flight = renderer.queued_jobs;
        usage.result_queue_depth = (uint32_t)meiso_stream_count(
            runtime->inline_streams[
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_RENDER_RESULT
            ].stream
        );
        if (!meiso_composition_scene_store_status(
                runtime->scene_store, &usage, &runtime->scene_status
            ) || meiso_composition_scene_status_encode(
                &runtime->scene_status,
                (MeisoMutableBuffer){
                    runtime->profile_scratch, sizeof(runtime->profile_scratch)
                }, &size
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
            return false;
        }
        runtime->scene_status_present = true;
        break;
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_DEVICE:
        if (!runtime->device_effective_present ||
            meiso_device_telemetry_parameters_encode(
                &runtime->device_effective,
                (MeisoMutableBuffer){
                    runtime->profile_scratch, sizeof(runtime->profile_scratch)
                }, &size
            ) != MEISO_DEVICE_TELEMETRY_CODEC_OK) {
            return false;
        }
        break;
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_RENDER_MEDIA:
        if (!runtime->render_media_actual_present ||
            meiso_composition_media_actual_encode(
                &runtime->render_media_actual,
                (MeisoMutableBuffer){
                    runtime->profile_scratch, sizeof(runtime->profile_scratch)
                }, &size
            ) != MEISO_COMPOSITION_PROFILE_OK) {
            return false;
        }
        break;
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA:
        if (!runtime->camera_media_actual_present ||
            meiso_composition_media_actual_encode(
                &runtime->camera_media_actual,
                (MeisoMutableBuffer){
                    runtime->profile_scratch, sizeof(runtime->profile_scratch)
                }, &size
            ) != MEISO_COMPOSITION_PROFILE_OK) {
            return false;
        }
        break;
    default:
        return false;
    }
    out_profile->data = runtime->profile_scratch;
    out_profile->size = size;
    return true;
}

bool meiso_k230d_composition_edge_runtime_private_publish_actual(MeisoK230dCompositionEdgeRuntime *runtime, size_t index)
{
    EdgeActualChannel *channel;
    MeisoRuntimeActualState actual;
    MeisoByteView profile;
    MeisoByteView payload;
    MeisoStatePublishStatus status;
    uint64_t version;
    if (!meiso_k230d_composition_edge_runtime_private_edge_runtime_valid(runtime) || index >= MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT) {
        return false;
    }
    channel = &runtime->actual[index];
    if (channel->snapshot.actual_state_version == UINT64_MAX ||
        !encode_actual_profile(runtime, index, &profile)) {
        return false;
    }
    memset(&actual, 0, sizeof(actual));
    actual.observed_desired = channel->snapshot.observed_desired;
    actual.applied_desired = channel->snapshot.applied_desired;
    actual.apply_result = channel->snapshot.apply_result;
    actual.availability = channel->snapshot.availability;
    actual.effective_enabled = channel->snapshot.effective_enabled;
    actual.production_status = channel->snapshot.production_status;
    actual.reason = channel->snapshot.reason;
    actual.production_epoch = channel->snapshot.production_epoch;
    actual.profile = profile;
    if (!meiso_runtime_actual_state_encode(
            &actual,
            (MeisoMutableBuffer){
                runtime->actual_scratch, sizeof(runtime->actual_scratch)
            }, &payload
        )) {
        return false;
    }
    version = channel->snapshot.actual_state_version + 1U;
    status = meiso_state_publisher_publish(
        channel->publisher, meiso_k230d_composition_edge_runtime_private_edge_owner(&runtime->config), version, payload
    );
    if (status != MEISO_STATE_PUBLISH_NEW &&
        status != MEISO_STATE_PUBLISH_REPLACED_PENDING &&
        status != MEISO_STATE_PUBLISH_DUPLICATE) {
        return false;
    }
    channel->snapshot.present = true;
    channel->snapshot.actual_state_version = version;
    channel->snapshot.delivery_pending = status != MEISO_STATE_PUBLISH_DUPLICATE;
    channel->snapshot.delivery_accepted = false;
    channel->snapshot.delivery_failed = false;
    channel->offer_sent = false;
    return true;
}

void meiso_k230d_composition_edge_runtime_private_set_actual_rejected(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    MeisoRuntimeDesiredReference observed,
    MeisoRuntimeProductionReason reason
)
{
    EdgeActualChannel *channel = &runtime->actual[index];
    channel->snapshot.observed_desired = observed;
    channel->snapshot.apply_result = MEISO_RUNTIME_APPLY_REJECTED;
    channel->snapshot.reason = reason;
    if (!channel->snapshot.present ||
        !channel->snapshot.applied_desired.present) {
        memset(&channel->snapshot.applied_desired, 0,
               sizeof(channel->snapshot.applied_desired));
        channel->snapshot.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
        channel->snapshot.effective_enabled = false;
        channel->snapshot.production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
        channel->snapshot.production_epoch = 0U;
    }
}

bool meiso_k230d_composition_edge_runtime_private_set_actual_applied(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    MeisoRuntimeDesiredReference desired,
    bool enabled,
    bool running,
    bool start_new_epoch
)
{
    EdgeActualChannel *channel = &runtime->actual[index];
    channel->snapshot.observed_desired = desired;
    channel->snapshot.applied_desired = desired;
    channel->snapshot.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    channel->snapshot.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    channel->snapshot.effective_enabled = enabled && running;
    channel->snapshot.production_status = enabled ?
        (running ? MEISO_RUNTIME_PRODUCTION_RUNNING :
                   MEISO_RUNTIME_PRODUCTION_STARTING) :
        MEISO_RUNTIME_PRODUCTION_STOPPED;
    channel->snapshot.reason = enabled && !running ?
        MEISO_RUNTIME_PRODUCTION_REASON_APPLYING :
        (enabled ? MEISO_RUNTIME_PRODUCTION_REASON_NONE :
                   MEISO_RUNTIME_PRODUCTION_REASON_DISABLED);
    if (enabled && running) {
        if (!start_new_epoch &&
            channel->snapshot.production_epoch != 0U) {
            /* Same producer/configuration; only desired identity advanced. */
        } else {
            if (channel->production_epoch_high_water == UINT64_MAX) {
                return false;
            }
            ++channel->production_epoch_high_water;
            channel->snapshot.production_epoch =
                channel->production_epoch_high_water;
        }
    } else {
        channel->snapshot.production_epoch = 0U;
    }
    return true;
}

static bool camera_resources_clean(
    const MeisoK230dCompositionEdgeRuntime *runtime,
    bool require_stopped
)
{
    MeisoCameraV4l2Snapshot snapshot;
    return runtime != NULL &&
        meiso_camera_v4l2_snapshot(runtime->camera, &snapshot) &&
        (!require_stopped ||
         snapshot.phase == MEISO_CAMERA_V4L2_PHASE_STOPPED) &&
        !snapshot.stream_on && !snapshot.frame_borrowed &&
        snapshot.driver_buffer_count == 0U &&
        snapshot.mapped_buffer_count == 0U &&
        snapshot.queued_buffer_count == 0U && snapshot.mapped_bytes == 0U;
}

/* The adapter retains cleanup debt after a failed stop so one retry is legal. */
bool meiso_k230d_composition_edge_runtime_private_stop_camera_bounded(
    MeisoK230dCompositionEdgeRuntime *runtime,
    bool require_stopped
)
{
    unsigned attempt;
    if (camera_resources_clean(runtime, require_stopped)) {
        return true;
    }
    for (attempt = 0U; attempt < 2U; ++attempt) {
        (void)meiso_camera_v4l2_stop(runtime->camera);
        if (camera_resources_clean(runtime, require_stopped)) {
            return true;
        }
    }
    return false;
}

bool meiso_k230d_composition_edge_runtime_private_fill_media_actual(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    const MeisoCompositionMediaDesired *effective
)
{
    const bool render =
        index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_RENDER_MEDIA;
    MeisoCompositionMediaActual *actual = render ?
        &runtime->render_media_actual : &runtime->camera_media_actual;
    memset(actual, 0, sizeof(*actual));
    actual->source = render ?
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW :
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
    actual->effective = *effective;
    actual->media_stream_epoch = runtime->actual[index].snapshot.production_epoch;
    actual->channel_nonce = (MeisoSessionNonce){
        runtime->config.local_nonce.high ^ runtime->actual[index].snapshot.production_epoch,
        runtime->config.local_nonce.low ^ (render ? UINT64_C(0x52554e44) :
                                               UINT64_C(0x43414d45))
    };
    if (!meiso_session_nonce_valid(actual->channel_nonce)) {
        actual->channel_nonce.low = 1U;
    }
    actual->queue_depth = 1U;
    if (render) {
        runtime->render_media_actual_present = true;
    } else {
        runtime->camera_media_actual_present = true;
    }
    return true;
}

bool meiso_k230d_composition_edge_runtime_private_media_desired_equal(
    const MeisoCompositionMediaDesired *lhs,
    const MeisoCompositionMediaDesired *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        lhs->pixel_format == rhs->pixel_format && lhs->codec == rhs->codec &&
        lhs->width == rhs->width && lhs->height == rhs->height &&
        lhs->fps_millihz == rhs->fps_millihz &&
        lhs->bitrate_bps == rhs->bitrate_bps &&
        lhs->gop_frames == rhs->gop_frames &&
        lhs->recovery_flags == rhs->recovery_flags;
}

bool meiso_k230d_composition_edge_runtime_private_start_inline_producer(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t inline_index,
    uint64_t production_epoch
)
{
    EdgeInlineChannel *channel;
    MeisoStreamProducerStatus status;
    if (inline_index >= MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT ||
        production_epoch == 0U) {
        return false;
    }
    channel = &runtime->inline_streams[inline_index];
    status = meiso_stream_begin_producer(
        channel->stream, meiso_k230d_composition_edge_runtime_private_edge_owner(&runtime->config), production_epoch
    );
    if (status != MEISO_STREAM_PRODUCER_STARTED &&
        status != MEISO_STREAM_PRODUCER_SWITCHED &&
        status != MEISO_STREAM_PRODUCER_UNCHANGED) {
        return false;
    }
    channel->snapshot.enabled = true;
    channel->snapshot.stream_epoch = production_epoch;
    if (status == MEISO_STREAM_PRODUCER_STARTED ||
        status == MEISO_STREAM_PRODUCER_SWITCHED) {
        channel->snapshot.next_sequence = 1U;
        channel->snapshot.queue_depth = 0U;
        channel->sample_queued = false;
        channel->sample_sent = false;
        channel->pending_sequence = 0U;
    }
    return true;
}
