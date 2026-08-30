#include "meiso_composition_remote_edge_private.h"

/* Remote snapshot/media projection owns Host borrows but never its transport. */

#include <string.h>

static bool meiso_composition_remote_edge_private_binding_equal(
    const MeisoCompositionMediaBinding *lhs,
    const MeisoCompositionMediaBinding *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        meiso_session_id_equal(lhs->session_id, rhs->session_id) &&
        lhs->catalog_generation == rhs->catalog_generation &&
        meiso_endpoint_id_equal(lhs->endpoint_id, rhs->endpoint_id) &&
        lhs->endpoint_incarnation == rhs->endpoint_incarnation &&
        meiso_resource_id_equal(lhs->resource_id, rhs->resource_id) &&
        lhs->source == rhs->source &&
        lhs->production_epoch == rhs->production_epoch &&
        lhs->media_stream_epoch == rhs->media_stream_epoch &&
        lhs->channel_nonce.high == rhs->channel_nonce.high &&
        lhs->channel_nonce.low == rhs->channel_nonce.low;
}

void meiso_composition_remote_edge_private_refresh_media_borrows(
    MeisoEdgeHandle *handle,
    const MeisoCompositionHostRuntimeSnapshot *host,
    const MeisoEdgeSnapshot *edge
)
{
    size_t index;

    if (handle == NULL || host == NULL || edge == NULL) {
        return;
    }
    if (!edge->resource_graph_live) {
        memset(handle->media_borrows, 0, sizeof(handle->media_borrows));
        return;
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT;
         ++index) {
        MeisoCompositionEdgeMediaBorrow *borrow =
            &handle->media_borrows[index];
        const MeisoCompositionHostRuntimeMediaRouteSnapshot *route =
            &host->media[index];
        if (borrow->active &&
            (!route->latest_present ||
             route->latest_frame_sequence != borrow->ref.frame_sequence ||
             !meiso_composition_remote_edge_private_binding_equal(
                 &route->binding, &borrow->ref.binding
             ))) {
            memset(borrow, 0, sizeof(*borrow));
        }
    }
}

static void meiso_composition_remote_edge_private_copy_desired(
    const MeisoCompositionHostRuntimeDesiredSnapshot *source,
    MeisoCompositionEdgeDesiredView *destination
)
{
    *destination = (MeisoCompositionEdgeDesiredView){
        source->requested,
        source->enabled,
        source->command_id,
        source->owner,
        source->state_version,
        source->delivery_active,
        source->delivery_pending,
        source->delivery_accepted,
        source->delivery_failed,
        source->replaced_pending_count,
        source->inflight_present,
        source->inflight_command_id,
        source->inflight_owner,
        source->inflight_state_version,
        source->inflight_source_completed,
        source->inflight_offer_accepted,
        source->inflight_actual_observed
    };
}

static void meiso_composition_remote_edge_private_copy_actual(
    const MeisoCompositionHostRuntimeActualSnapshot *source,
    MeisoCompositionEdgeActualView *destination
)
{
    *destination = (MeisoCompositionEdgeActualView){
        source->present,
        source->state_owner,
        source->state_version,
        source->observed_desired,
        source->applied_desired,
        source->apply_result,
        source->availability,
        source->effective_enabled,
        source->production_status,
        source->reason,
        source->production_epoch,
        source->profile_decoded
    };
}

static void meiso_composition_remote_edge_private_copy_inline(
    const MeisoCompositionHostRuntimeInlineSnapshot *source,
    MeisoCompositionEdgeInlineView *destination
)
{
    *destination = (MeisoCompositionEdgeInlineView){
        source->present,
        source->key,
        source->source_time,
        source->receive_flags,
        source->missing_before,
        source->received_count,
        source->replaced_count
    };
}

static void meiso_composition_remote_edge_private_copy_media_actual(
    const MeisoCompositionMediaActual *source,
    MeisoCompositionEdgeMediaActual *destination
)
{
    *destination = (MeisoCompositionEdgeMediaActual){
        source->source,
        source->effective,
        source->media_stream_epoch,
        source->producer_dropped_frames,
        source->queue_depth,
        source->recovery_generation
    };
}

MeisoEdgeStatus meiso_composition_remote_edge_private_snapshot(
    const MeisoEdgeHandle *handle,
    MeisoCompositionEdgeSnapshot *out_snapshot
)
{
    MeisoCompositionHostRuntimeSnapshot host;
    MeisoCompositionEdgeSnapshot candidate;
    size_t index;

    memset(&candidate, 0, sizeof(candidate));
    if (!meiso_composition_host_runtime_snapshot(
            meiso_composition_remote_edge_private_const_runtime(handle),
            &host
        ) ||
        !meiso_composition_remote_edge_private_base_snapshot(
            handle, &host, &candidate.edge
        )) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    if (candidate.edge.phase != MEISO_EDGE_PHASE_TERMINAL) {
        for (index = 0U;
             index < MEISO_COMPOSITION_EDGE_DESIRED_COUNT;
             ++index) {
            meiso_composition_remote_edge_private_copy_desired(
                &host.desired[index], &candidate.desired[index]
            );
        }
        candidate.mesh = (MeisoCompositionEdgeMeshView){
            host.mesh.queued,
            host.mesh.transfer_active,
            host.mesh.begin_ready,
            host.mesh.waiting_result,
            host.mesh.terminal_result_present,
            host.mesh.command_id,
            host.mesh.content_id,
            host.mesh.total_bytes,
            host.mesh.next_offset,
            host.mesh.terminal_result,
            host.mesh.edge_core_published
        };
    }
    if (candidate.edge.resource_graph_live) {
        for (index = 0U;
             index < MEISO_COMPOSITION_EDGE_ACTUAL_COUNT;
             ++index) {
            meiso_composition_remote_edge_private_copy_actual(
                &host.actual[index], &candidate.actual[index]
            );
        }
        for (index = 0U;
             index < MEISO_COMPOSITION_EDGE_INLINE_STREAM_COUNT;
             ++index) {
            meiso_composition_remote_edge_private_copy_inline(
                &host.inline_streams[index],
                &candidate.inline_streams[index]
            );
        }
        for (index = 0U;
             index < MEISO_COMPOSITION_EDGE_MEDIA_ROUTE_COUNT;
             ++index) {
            const MeisoCompositionHostRuntimeMediaRouteSnapshot *route =
                &host.media[index];
            candidate.media[index] = (MeisoCompositionEdgeMediaRouteView){
                route->actual_binding_ready,
                route->latest_present,
                route->source,
                route->latest_frame_sequence,
                route->latest_source_time,
                route->latest_access_unit_bytes,
                route->published_sequence_high_water,
                route->sequence_gap_frames,
                route->producer_drop_high_water,
                route->reassembly_frames_dropped,
                route->unread_latest_frames_dropped
            };
        }
        candidate.wifi_effective_present = host.wifi_effective_present;
        candidate.wifi_effective = host.wifi_effective;
        candidate.wifi_sample_present = host.wifi_sample_present;
        candidate.wifi_sample = host.wifi_sample;
        candidate.presentation_status_present =
            host.presentation_status_present;
        candidate.presentation_status =
            (MeisoCompositionEdgePresentationStatus){
                host.presentation_status.capability_generation,
                host.presentation_status.candidate_state,
                host.presentation_status.candidate_plan_generation,
                {0},
                host.presentation_status.active_state,
                host.presentation_status.active_plan_generation,
                {0},
                host.presentation_status.reason,
                host.presentation_status.target_width,
                host.presentation_status.target_height,
                host.presentation_status.color_format,
                host.presentation_status.depth_format
            };
        memmove(
            candidate.presentation_status.candidate_digest,
            host.presentation_status.candidate_digest,
            MEISO_SHA256_DIGEST_SIZE
        );
        memmove(
            candidate.presentation_status.active_digest,
            host.presentation_status.active_digest,
            MEISO_SHA256_DIGEST_SIZE
        );
        candidate.scene_status_present = host.scene_status_present;
        candidate.scene_status = host.scene_status;
        candidate.render_result_present = host.render_result_present;
        candidate.render_result = host.render_result;
        candidate.device_effective_present = host.device_effective_present;
        candidate.device_effective = host.device_effective;
        candidate.device_sample_present = host.device_sample_present;
        candidate.device_sample = host.device_sample;
        candidate.render_media_actual_present =
            host.render_media_actual_present;
        if (host.render_media_actual_present) {
            meiso_composition_remote_edge_private_copy_media_actual(
                &host.render_media_actual,
                &candidate.render_media_actual
            );
        }
        candidate.camera_media_actual_present =
            host.camera_media_actual_present;
        if (host.camera_media_actual_present) {
            meiso_composition_remote_edge_private_copy_media_actual(
                &host.camera_media_actual,
                &candidate.camera_media_actual
            );
        }
    }
    memmove(out_snapshot, &candidate, sizeof(candidate));
    return MEISO_EDGE_OK;
}

MeisoEdgeStatus meiso_composition_remote_edge_private_media_latest(
    MeisoEdgeHandle *handle,
    MeisoCompositionMediaSource source,
    MeisoEdgeMediaFrameView *out_frame
)
{
    MeisoCompositionHostRuntimeSnapshot host;
    MeisoEdgeSnapshot edge;
    MeisoCompositionMediaLatestView latest;
    MeisoEdgeMediaFrameView candidate;
    MeisoCompositionHostRuntimeStatus host_status;
    const int index = meiso_composition_edge_private_media_index(source);

    if (!meiso_composition_remote_edge_private_read_snapshot(
            handle, &host, &edge
        )) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    meiso_composition_remote_edge_private_refresh_media_borrows(
        handle, &host, &edge
    );
    if (edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (!edge.resource_graph_live) {
        return MEISO_EDGE_NOT_READY;
    }
    host_status = meiso_composition_host_runtime_media_latest(
        meiso_composition_remote_edge_private_runtime(handle),
        source,
        &latest
    );
    if (host_status == MEISO_COMPOSITION_HOST_RUNTIME_INVALID_STATE) {
        return MEISO_EDGE_NOT_READY;
    }
    if (host_status != MEISO_COMPOSITION_HOST_RUNTIME_OK) {
        return meiso_composition_remote_edge_private_status(host_status);
    }
    candidate = (MeisoEdgeMediaFrameView){
        {
            handle->continuity_serial,
            source,
            latest.unit.binding.media_stream_epoch,
            latest.ref.frame_sequence
        },
        latest.unit.source_time,
        latest.unit.applied_scene_owner,
        latest.unit.applied_scene_state_version,
        latest.unit.active_plan_generation,
        latest.unit.world_generation,
        latest.unit.dependency_sequence,
        latest.unit.producer_dropped_frames,
        latest.unit.pixel_format,
        latest.unit.codec,
        latest.unit.width,
        latest.unit.height,
        latest.unit.flags,
        latest.unit.access_unit
    };
    handle->media_borrows[(size_t)index].active = true;
    handle->media_borrows[(size_t)index].ref = latest.ref;
    memmove(out_frame, &candidate, sizeof(candidate));
    return MEISO_EDGE_OK;
}

MeisoEdgeStatus meiso_composition_remote_edge_private_media_release(
    MeisoEdgeHandle *handle,
    const MeisoEdgeMediaFrameKey *expected_frame
)
{
    MeisoCompositionHostRuntimeSnapshot host;
    MeisoEdgeSnapshot edge;
    MeisoCompositionHostRuntimeStatus host_status;
    MeisoCompositionEdgeMediaBorrow *borrow;
    const int index = meiso_composition_edge_private_media_index(
        expected_frame->source
    );

    if (!meiso_composition_remote_edge_private_read_snapshot(
            handle, &host, &edge
        )) {
        return MEISO_EDGE_INTERNAL_FAILURE;
    }
    meiso_composition_remote_edge_private_refresh_media_borrows(
        handle, &host, &edge
    );
    if (edge.phase == MEISO_EDGE_PHASE_TERMINAL) {
        return MEISO_EDGE_TERMINAL;
    }
    if (!edge.resource_graph_live) {
        return MEISO_EDGE_NOT_READY;
    }
    borrow = &handle->media_borrows[(size_t)index];
    if (!borrow->active ||
        expected_frame->continuity_serial != handle->continuity_serial ||
        expected_frame->frame_sequence != borrow->ref.frame_sequence ||
        expected_frame->media_stream_epoch !=
            borrow->ref.binding.media_stream_epoch) {
        return MEISO_EDGE_STALE;
    }
    host_status = meiso_composition_host_runtime_media_release(
        meiso_composition_remote_edge_private_runtime(handle),
        &borrow->ref
    );
    memset(borrow, 0, sizeof(*borrow));
    return meiso_composition_remote_edge_private_status(host_status);
}
