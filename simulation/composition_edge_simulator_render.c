#include "meiso_composition_edge_simulator_private.h"

/* Simulator render/media owner commits deterministic results and frame borrows. */

#include <string.h>

bool meiso_composition_edge_simulator_private_publish_render_result(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
)
{
    MeisoCompositionSceneWorldView world;
    MeisoCompositionSceneRenderResult result;
    MeisoCompositionCpuRasterJobRef job;
    MeisoCompositionCpuRasterCompleteView complete;
    MeisoCompositionSceneControlScope scope;
    MeisoCompositionEdgeInlineView stream;
    MeisoCompositionEdgeInlineView *current =
        &handle->simulator_snapshot.inline_streams[
            MEISO_COMPOSITION_EDGE_INLINE_RENDER_RESULT
        ];
    const uint64_t epoch = handle->simulator_snapshot.actual[
        MEISO_COMPOSITION_EDGE_ACTUAL_SCENE
    ].production_epoch;
    uint64_t sequence;

    if (now.monotonic_ns == UINT64_MAX || epoch == 0U ||
        handle->simulator_render_result_sequence == UINT64_MAX ||
        current->received_count == UINT64_MAX ||
        (current->present && current->replaced_count == UINT64_MAX) ||
        !meiso_composition_scene_store_active(
            handle->simulator_scene_store, &world
        )) {
        return false;
    }
    sequence = handle->simulator_render_result_sequence + 1U;
    if (handle->simulator_cpu_raster_enabled) {
        if (!meiso_composition_scene_store_control_scope(
                handle->simulator_scene_store, &scope
            ) ||
            meiso_composition_cpu_raster_renderer_submit(
                handle->simulator_renderer, &scope, &world,
                handle->simulator_composition_clock_epoch, &job
            ) != MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED ||
            meiso_composition_cpu_raster_renderer_render_next(
                handle->simulator_renderer, &job, now.monotonic_ns,
                now.monotonic_ns + 1U, &complete
            ) != MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE) {
            return false;
        }
        result = complete.result;
    } else if (meiso_composition_scene_store_fake_render(
                   handle->simulator_scene_store,
                   handle->simulator_composition_clock_epoch,
                   now.monotonic_ns, now.monotonic_ns + 1U, &result
               ) != MEISO_COMPOSITION_SCENE_RENDER_COMPLETE) {
        return false;
    }
    if (!meiso_composition_scene_store_record_result_published(
            handle->simulator_scene_store, &world.ref, epoch, sequence
        )) {
        return false;
    }
    stream = (MeisoCompositionEdgeInlineView){
        true,
        {handle->simulator_desired_owner, epoch, sequence},
        now,
        MEISO_STREAM_RECEIVE_FLAG_NONE,
        0U,
        current->received_count + 1U,
        current->replaced_count + (current->present ? 1U : 0U)
    };
    handle->simulator_render_result_sequence = sequence;
    handle->simulator_snapshot.render_result_present = true;
    handle->simulator_snapshot.render_result = result;
    *current = stream;
    handle->simulator_render_result_pending = false;
    handle->simulator_raster_preview_pending =
        handle->simulator_cpu_raster_enabled;
    return meiso_composition_edge_simulator_private_refresh_scene_status(handle);
}

static bool meiso_composition_edge_simulator_private_media_size_exact(
    const MeisoCompositionEdgeSimulatorFrame *frame
)
{
    uint64_t pixels = (uint64_t)frame->width * frame->height;
    uint64_t bytes;

    switch (frame->pixel_format) {
    case MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888:
        bytes = pixels * 4U;
        break;
    case MEISO_COMPOSITION_MEDIA_PIXEL_RGB565:
    case MEISO_COMPOSITION_MEDIA_PIXEL_YUYV:
        bytes = pixels * 2U;
        break;
    case MEISO_COMPOSITION_MEDIA_PIXEL_NV12:
        if ((frame->width & 1U) != 0U || (frame->height & 1U) != 0U) {
            return false;
        }
        bytes = pixels + pixels / 2U;
        break;
    default:
        return false;
    }
    return bytes == frame->access_unit.size;
}

static bool meiso_composition_edge_simulator_private_frame_scene_fields_valid(
    const MeisoEdgeHandle *handle,
    const MeisoCompositionEdgeSimulatorFrame *frame
)
{
    if (frame->dependency_sequence != 0U) {
        return false;
    }
    if (frame->source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) {
        const bool fields_valid =
            meiso_owner_scope_valid(frame->applied_scene_owner) &&
            frame->applied_scene_owner.role == MEISO_OWNER_ROLE_HOST &&
            frame->applied_scene_state_version != 0U &&
            frame->active_plan_generation != 0U &&
            frame->world_generation != 0U;
        if (!fields_valid || handle == NULL ||
            !handle->simulator_composition_supported) {
            return fields_valid;
        }
        return handle->simulator_snapshot.actual[
                   MEISO_COMPOSITION_EDGE_ACTUAL_SCENE
               ].production_status == MEISO_RUNTIME_PRODUCTION_RUNNING &&
            meiso_owner_scope_equal(
                frame->applied_scene_owner,
                handle->simulator_desired_owner
            ) &&
            frame->applied_scene_state_version ==
                handle->simulator_snapshot.actual[
                    MEISO_COMPOSITION_EDGE_ACTUAL_SCENE
                ].applied_desired.version &&
            handle->simulator_snapshot.presentation_status_present &&
            frame->active_plan_generation ==
                handle->simulator_snapshot.presentation_status
                    .active_plan_generation &&
            handle->simulator_snapshot.scene_status_present &&
            frame->world_generation ==
                handle->simulator_snapshot.scene_status.world_generation;
    }
    return frame->applied_scene_owner.role == 0U &&
        frame->applied_scene_owner.incarnation == 0U &&
        frame->applied_scene_state_version == 0U &&
        frame->active_plan_generation == 0U &&
        frame->world_generation == 0U;
}

MeisoEdgeStatus meiso_composition_edge_simulator_private_commit_frame(
    MeisoEdgeHandle *handle,
    const MeisoCompositionEdgeSimulatorFrame *frame
)
{
    MeisoCompositionEdgeSimulatorMediaRoute *route;
    MeisoCompositionEdgeMediaRouteView *snapshot_route;
    const MeisoCompositionEdgeMediaActual *actual;
    uint64_t gap = 0U;
    const int index = meiso_composition_edge_private_media_index(frame->source);

    if (index < 0) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    route = &handle->simulator_media[(size_t)index];
    snapshot_route = &handle->simulator_snapshot.media[(size_t)index];
    actual = index == 0 ? &handle->simulator_snapshot.render_media_actual :
        &handle->simulator_snapshot.camera_media_actual;
    if (!handle->simulator_snapshot.edge.resource_graph_live ||
        !snapshot_route->actual_binding_ready ||
        frame->media_stream_epoch != actual->media_stream_epoch) {
        return MEISO_EDGE_NOT_READY;
    }
    if (route->borrowed) {
        return MEISO_EDGE_BUSY;
    }
    if (frame->frame_sequence == 0U ||
        frame->frame_sequence <=
            snapshot_route->published_sequence_high_water ||
        frame->producer_dropped_frames <
            snapshot_route->producer_drop_high_water) {
        return MEISO_EDGE_STALE;
    }
    if (frame->pixel_format != actual->effective.pixel_format ||
        frame->codec != actual->effective.codec ||
        frame->width != actual->effective.width ||
        frame->height != actual->effective.height ||
        (frame->flags & MEISO_COMPOSITION_MEDIA_FRAME_KEY) == 0U ||
        (frame->flags &
         (MeisoCompositionMediaFrameFlags)~
             (MEISO_COMPOSITION_MEDIA_FRAME_KEY |
              MEISO_COMPOSITION_MEDIA_FRAME_RECOVERY)) != 0U ||
        !meiso_composition_edge_simulator_private_frame_scene_fields_valid(handle, frame) ||
        !meiso_composition_edge_simulator_private_media_size_exact(frame)) {
        return MEISO_EDGE_CONFLICT;
    }
    if (frame->access_unit.size > route->capacity) {
        return MEISO_EDGE_CAPACITY;
    }
    if (snapshot_route->published_sequence_high_water != 0U) {
        gap = frame->frame_sequence -
            snapshot_route->published_sequence_high_water - 1U;
    }
    if (gap > UINT64_MAX - snapshot_route->sequence_gap_frames ||
        (route->present &&
         snapshot_route->unread_latest_frames_dropped == UINT64_MAX)) {
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    }
    if (route->present) {
        ++snapshot_route->unread_latest_frames_dropped;
    }
    memmove(route->buffer, frame->access_unit.data, frame->access_unit.size);
    route->frame = (MeisoEdgeMediaFrameView){
        {handle->continuity_serial, frame->source,
         frame->media_stream_epoch, frame->frame_sequence},
        frame->source_time,
        frame->applied_scene_owner,
        frame->applied_scene_state_version,
        frame->active_plan_generation,
        frame->world_generation,
        frame->dependency_sequence,
        frame->producer_dropped_frames,
        frame->pixel_format,
        frame->codec,
        frame->width,
        frame->height,
        frame->flags,
        {route->buffer, frame->access_unit.size}
    };
    route->present = true;
    snapshot_route->latest_present = true;
    snapshot_route->latest_frame_sequence = frame->frame_sequence;
    snapshot_route->latest_source_time = frame->source_time;
    snapshot_route->latest_access_unit_bytes =
        (uint32_t)frame->access_unit.size;
    snapshot_route->published_sequence_high_water = frame->frame_sequence;
    snapshot_route->sequence_gap_frames += gap;
    snapshot_route->producer_drop_high_water =
        frame->producer_dropped_frames;
    return MEISO_EDGE_OK;
}

MeisoEdgeStatus meiso_composition_edge_simulator_private_publish_raster_preview(
    MeisoEdgeHandle *handle
)
{
    MeisoCompositionCpuRasterCompleteView complete;
    const MeisoCompositionEdgeMediaActual *actual =
        &handle->simulator_snapshot.render_media_actual;
    MeisoCompositionEdgeMediaRouteView *route =
        &handle->simulator_snapshot.media[0];
    MeisoCompositionEdgeSimulatorFrame frame;
    MeisoEdgeStatus status;

    if (!handle->simulator_cpu_raster_enabled ||
        !handle->simulator_raster_preview_pending ||
        !route->actual_binding_ready ||
        !handle->simulator_snapshot.render_media_actual_present) {
        return MEISO_EDGE_NOT_READY;
    }
    if (route->published_sequence_high_water == UINT64_MAX) {
        return MEISO_EDGE_COUNTER_EXHAUSTED;
    }
    if (!meiso_composition_cpu_raster_renderer_latest_complete(
            handle->simulator_renderer, &complete
        )) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    memset(&frame, 0, sizeof(frame));
    frame.source = MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW;
    frame.media_stream_epoch = actual->media_stream_epoch;
    frame.frame_sequence = route->published_sequence_high_water + 1U;
    frame.source_time = (MeisoTimePoint){
        complete.result.source_clock_epoch,
        complete.result.render_end_monotonic_ns
    };
    frame.applied_scene_owner = handle->simulator_desired_owner;
    frame.applied_scene_state_version =
        complete.job.world.desired_state_version;
    frame.active_plan_generation = complete.job.world.plan_generation;
    frame.world_generation = complete.job.world.world_generation;
    frame.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888;
    frame.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
    frame.width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    frame.height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    frame.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    frame.access_unit = complete.color_rgba8888;
    status = meiso_composition_edge_simulator_private_commit_frame(handle, &frame);
    if (status == MEISO_EDGE_OK) {
        handle->simulator_raster_preview_pending = false;
    }
    return status;
}

MeisoEdgeStatus meiso_composition_edge_simulator_publish_frame(
    MeisoEdgeHandle *handle,
    const MeisoCompositionEdgeSimulatorFrame *frame
)
{
    if (!meiso_composition_edge_private_handle_valid(handle) ||
        handle->backend_kind != MEISO_COMPOSITION_EDGE_BACKEND_SIMULATOR || frame == NULL ||
        !meiso_composition_edge_private_optional_input_disjoint(handle, frame, sizeof(*frame)) ||
        !meiso_byte_view_valid(frame->access_unit) ||
        !meiso_composition_edge_private_optional_input_disjoint(handle, frame->access_unit.data,
                                 frame->access_unit.size) ||
        !meiso_composition_edge_private_disjoint_pair(frame, sizeof(*frame), frame->access_unit.data,
                       frame->access_unit.size) ||
        !meiso_time_point_valid(frame->source_time) ||
        frame->access_unit.size == 0U ||
        (frame->flags &
         ~(MEISO_COMPOSITION_MEDIA_FRAME_BEGIN |
           MEISO_COMPOSITION_MEDIA_FRAME_END |
           MEISO_COMPOSITION_MEDIA_FRAME_KEY |
           MEISO_COMPOSITION_MEDIA_FRAME_RECOVERY)) != 0U) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (handle->simulator_snapshot.edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (handle->simulator_cpu_raster_enabled &&
        frame->source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) {
        return MEISO_EDGE_CONFLICT;
    }
    return meiso_composition_edge_simulator_private_commit_frame(handle, frame);
}

MeisoEdgeStatus meiso_composition_edge_simulator_terminate(
    MeisoEdgeHandle *handle,
    MeisoEdgeTerminalReason reason
)
{
    MeisoEdgeSnapshot edge;

    if (!meiso_composition_edge_private_handle_valid(handle) ||
        handle->backend_kind != MEISO_COMPOSITION_EDGE_BACKEND_SIMULATOR ||
        reason == MEISO_EDGE_TERMINAL_NONE ||
        reason > MEISO_EDGE_TERMINAL_INTERNAL_FAILURE) {
        return MEISO_EDGE_INVALID_ARGUMENT;
    }
    if (handle->simulator_snapshot.edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    edge = (MeisoEdgeSnapshot){
        MEISO_HOST_EDGE_API_VERSION,
        handle->profile,
        MEISO_EDGE_PHASE_TERMINAL,
        reason,
        false,
        false,
        handle->continuity_serial,
        0U,
        0U,
        0U,
        0U
    };
    memset(&handle->simulator_snapshot, 0,
           sizeof(handle->simulator_snapshot));
    handle->simulator_snapshot.edge = edge;
    handle->simulator_wifi_pending = false;
    handle->simulator_device_pending = false;
    handle->simulator_mesh_pending = false;
    handle->simulator_mesh_size = 0U;
    memset(&handle->simulator_mesh_content_id, 0,
           sizeof(handle->simulator_mesh_content_id));
    memset(&handle->simulator_scene_pending, 0,
           sizeof(handle->simulator_scene_pending));
    handle->simulator_scene_retry_pending = false;
    handle->simulator_scene_retry_ready = false;
    handle->simulator_scene_value_present = false;
    handle->simulator_scene_enabled = false;
    memset(&handle->simulator_scene_value, 0,
           sizeof(handle->simulator_scene_value));
    memset(&handle->simulator_presentation_pending, 0,
           sizeof(handle->simulator_presentation_pending));
    handle->simulator_presentation_plan_present = false;
    handle->simulator_presentation_activation_pending = false;
    memset(&handle->simulator_presentation_plan, 0,
           sizeof(handle->simulator_presentation_plan));
    handle->simulator_render_result_pending = false;
    handle->simulator_raster_preview_pending = false;
    memset(handle->simulator_media_pending, 0,
           sizeof(handle->simulator_media_pending));
    memset(handle->simulator_media, 0, sizeof(handle->simulator_media));
    return MEISO_EDGE_OK;
}
