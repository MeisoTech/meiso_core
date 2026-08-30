#define _POSIX_C_SOURCE 200809L

#include "composition_edge_runtime_private.h"

/* Desired-state application and lifecycle transitions. */

static bool apply_simple_desired(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    const MeisoRuntimeDesiredState *desired,
    MeisoRuntimeDesiredReference reference
)
{
    MeisoWifiTelemetryDesiredParameters wifi;
    MeisoDeviceTelemetryParameters device;
    MeisoCompositionMediaDesired media;
    const MeisoCompositionMediaCapability *media_capability;
    bool restart_production = true;
    if (!desired->enabled) {
        if (desired->profile.size != 0U) {
            return false;
        }
        if (index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA &&
            !meiso_k230d_composition_edge_runtime_private_stop_camera_bounded(runtime, true)) {
            return false;
        }
        if (runtime->active_media != NULL &&
            ((index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_RENDER_MEDIA &&
              (runtime->active_media->active_route_mask &
               MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MEDIA_RENDER) != 0U) ||
             (index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA &&
              (runtime->active_media->active_route_mask &
               MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MEDIA_CAMERA) != 0U))) {
            meiso_k230d_composition_edge_runtime_private_terminate_active_media(
                runtime, MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED, 0
            );
        }
        if (!meiso_k230d_composition_edge_runtime_private_set_actual_applied(
                runtime, index, reference, false, false, false
            )) {
            return false;
        }
        if (index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_WIFI) {
            runtime->wifi_effective_present = false;
            runtime->wifi_next_due_ns = 0U;
            if (!meiso_k230d_composition_edge_runtime_private_disable_inline_producer(
                    &runtime->inline_streams[
                        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_WIFI
                    ]
                )) {
                return false;
            }
        } else if (index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_DEVICE) {
            runtime->device_effective_present = false;
            runtime->device_next_due_ns = 0U;
            runtime->thermal_request_pending = false;
            runtime->thermal_request_dispatched = false;
            runtime->thermal_result_present = false;
            memset(&runtime->thermal_request, 0, sizeof(runtime->thermal_request));
            memset(&runtime->thermal_result, 0, sizeof(runtime->thermal_result));
            if (!meiso_k230d_composition_edge_runtime_private_disable_inline_producer(
                    &runtime->inline_streams[
                        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_DEVICE
                    ]
                )) {
                return false;
            }
            (void)meiso_device_telemetry_linux_adapter_reset_sampling(
                &runtime->device
            );
        } else if (index ==
                   MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_RENDER_MEDIA) {
            runtime->render_media_actual_present = false;
            runtime->render_media_sequence = 0U;
            meiso_k230d_composition_edge_runtime_private_cadence_reset(&runtime->render_media_cadence);
        } else if (index ==
                   MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA) {
            runtime->camera_media_actual_present = false;
            runtime->camera_media_sequence = 0U;
            meiso_k230d_composition_edge_runtime_private_cadence_reset(&runtime->camera_media_cadence);
            runtime->camera_frame_pending = false;
            runtime->camera_frame_submitted = false;
            memset(&runtime->camera_frame, 0, sizeof(runtime->camera_frame));
        }
        return meiso_k230d_composition_edge_runtime_private_publish_actual(runtime, index);
    }
    if (index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_WIFI) {
        if (meiso_wifi_telemetry_desired_parameters_decode(
                desired->profile, &wifi
            ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
            !meiso_wifi_telemetry_parameters_supported(
                &runtime->wifi_capability, &wifi
            )) {
            goto rejected;
        }
        runtime->wifi_effective.selected_fields = wifi.selected_fields;
        runtime->wifi_effective.sample_period_ms = wifi.sample_period_ms;
        runtime->wifi_effective_present = true;
    } else if (index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_DEVICE) {
        if (meiso_device_telemetry_parameters_decode(
                desired->profile, &device
            ) != MEISO_DEVICE_TELEMETRY_CODEC_OK ||
            !meiso_device_telemetry_parameters_supported(
                &runtime->device_capability, &device
            )) {
            goto rejected;
        }
        runtime->device_effective = device;
        runtime->device_effective_present = true;
    } else if (index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_RENDER_MEDIA ||
               index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA) {
        media_capability = index ==
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_RENDER_MEDIA ?
            &runtime->catalog.render_media_capability :
            &runtime->catalog.camera_media_capability;
        if (meiso_composition_media_desired_decode(
                desired->profile, &media
            ) != MEISO_COMPOSITION_PROFILE_OK ||
            !meiso_composition_media_desired_supported(
                media_capability, &media
            )) {
            goto rejected;
        }
    } else {
        return false;
    }
    if (restart_production && runtime->active_media != NULL &&
        ((index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_RENDER_MEDIA &&
          (runtime->active_media->active_route_mask &
           MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MEDIA_RENDER) != 0U) ||
         (index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA &&
          (runtime->active_media->active_route_mask &
           MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MEDIA_CAMERA) != 0U))) {
        meiso_k230d_composition_edge_runtime_private_terminate_active_media(
            runtime, MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED, 0
        );
    }
    if (restart_production &&
        index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA) {
        if (!meiso_k230d_composition_edge_runtime_private_stop_camera_bounded(runtime, true)) {
            return false;
        }
        runtime->camera_frame_pending = false;
        runtime->camera_frame_submitted = false;
        memset(&runtime->camera_frame, 0, sizeof(runtime->camera_frame));
    }
    if (!meiso_k230d_composition_edge_runtime_private_set_actual_applied(
            runtime, index, reference, true, true, restart_production
        )) {
        return false;
    }
    if ((index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_RENDER_MEDIA ||
         index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA) &&
        !meiso_k230d_composition_edge_runtime_private_fill_media_actual(runtime, index, &media)) {
        return false;
    }
    if (restart_production) {
        if (index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_WIFI) {
            runtime->wifi_next_due_ns = 0U;
        } else if (index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_DEVICE) {
            runtime->device_next_due_ns = 0U;
            runtime->thermal_request_pending = false;
            runtime->thermal_request_dispatched = false;
            runtime->thermal_result_present = false;
            memset(&runtime->thermal_request, 0, sizeof(runtime->thermal_request));
            memset(&runtime->thermal_result, 0, sizeof(runtime->thermal_result));
        } else if (index ==
                   MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_RENDER_MEDIA) {
            runtime->render_media_sequence = 0U;
            meiso_k230d_composition_edge_runtime_private_cadence_reset(&runtime->render_media_cadence);
        } else if (index ==
                   MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA) {
            runtime->camera_media_sequence = 0U;
            meiso_k230d_composition_edge_runtime_private_cadence_reset(&runtime->camera_media_cadence);
        }
    }
    if (index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_WIFI &&
        !meiso_k230d_composition_edge_runtime_private_start_inline_producer(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_WIFI,
            runtime->actual[index].snapshot.production_epoch
        )) {
        return false;
    }
    if (index == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_DEVICE &&
        !meiso_k230d_composition_edge_runtime_private_start_inline_producer(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_DEVICE,
            runtime->actual[index].snapshot.production_epoch
        )) {
        return false;
    }
    return meiso_k230d_composition_edge_runtime_private_publish_actual(runtime, index);

rejected:
    meiso_k230d_composition_edge_runtime_private_set_actual_rejected(
        runtime, index, reference,
        MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER
    );
    return meiso_k230d_composition_edge_runtime_private_publish_actual(runtime, index);
}
static bool apply_desired(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    MeisoStateView current,
    MeisoTimePoint now
)
{
    MeisoRuntimeDesiredState desired;
    MeisoRuntimeDesiredReference reference;
    if (!meiso_runtime_desired_state_decode(current.payload, &desired)) {
        return false;
    }
    reference.present = true;
    reference.owner = current.owner;
    reference.version = current.version;
    runtime->desired[index].decoded = desired;
    runtime->desired[index].reference = reference;
    switch (index) {
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_PRESENTATION:
        return meiso_k230d_composition_edge_runtime_private_apply_presentation(runtime, &desired, reference, now);
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_SCENE:
        return meiso_k230d_composition_edge_runtime_private_apply_scene(runtime, &desired, reference, now);
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_WIFI:
        return apply_simple_desired(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_WIFI,
            &desired, reference
        );
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_DEVICE:
        return apply_simple_desired(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_DEVICE,
            &desired, reference
        );
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_RENDER_MEDIA:
        return apply_simple_desired(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_RENDER_MEDIA,
            &desired, reference
        );
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_CAMERA_MEDIA:
        return apply_simple_desired(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA,
            &desired, reference
        );
    default:
        return false;
    }
}

void meiso_k230d_composition_edge_runtime_private_enter_terminal(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoK230dCompositionEdgeRuntimeTerminalReason reason,
    int error_number
)
{
    bool camera_clean;
    MeisoCompositionMediaTerminalReason media_reason;
    if (!meiso_k230d_composition_edge_runtime_private_edge_runtime_valid(runtime) ||
        runtime->phase == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL) {
        return;
    }
    camera_clean = meiso_k230d_composition_edge_runtime_private_stop_camera_bounded(runtime, false);
    if (runtime->active_media != NULL && runtime->active_media->adapter != NULL) {
        media_reason = reason ==
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CATALOG_CHANGED ?
            MEISO_COMPOSITION_MEDIA_TERMINAL_CATALOG_CHANGED :
            reason == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_LOCAL_ABORT ?
                MEISO_COMPOSITION_MEDIA_TERMINAL_LOCAL_CLOSE :
                MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION;
        (void)meiso_composition_media_posix_tcp_terminate(
            runtime->active_media->adapter,
            media_reason,
            error_number
        );
    }
    meiso_composition_control_session_abort(runtime->control_session, error_number);
    runtime->active_media = NULL;
    memset(&runtime->blob_receive_transfer, 0,
           sizeof(runtime->blob_receive_transfer));
    memset(&runtime->blob_result_transfer, 0,
           sizeof(runtime->blob_result_transfer));
    runtime->blob_receive_next_offset = 0U;
    runtime->blob_result_operation = 0U;
    runtime->blob_receive_active = false;
    runtime->blob_receive_begin_ready = false;
    runtime->blob_result_pending = false;
    runtime->blob_result_queued = false;
    runtime->blob_result_marks_ready = false;
    runtime->blob_result_clears_active = false;
    runtime->thermal_request_pending = false;
    runtime->thermal_request_dispatched = false;
    runtime->thermal_result_present = false;
    runtime->terminal_reason = camera_clean ? reason :
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CAMERA_CLEANUP;
    runtime->phase = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL;
}

bool meiso_k230d_composition_edge_runtime_private_refresh_phase(MeisoK230dCompositionEdgeRuntime *runtime)
{
    MeisoCompositionControlSessionSnapshot control_session;
    if (!meiso_k230d_composition_edge_runtime_private_edge_runtime_valid(runtime) ||
        !meiso_composition_control_session_snapshot(runtime->control_session, &control_session)) {
        return false;
    }
    if (control_session.terminal ||
        control_session.phase == MEISO_COMPOSITION_CONTROL_SESSION_PHASE_TERMINAL) {
        meiso_k230d_composition_edge_runtime_private_enter_terminal(
            runtime,
            control_session.session.failure == MEISO_SESSION_FAILURE_TRANSPORT_LOST ?
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CONTROL_TRANSPORT :
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CONTROL_SESSION,
            0
        );
        return true;
    }
    switch (control_session.phase) {
    case MEISO_COMPOSITION_CONTROL_SESSION_PHASE_HANDSHAKE:
        runtime->phase = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_HANDSHAKE;
        break;
    case MEISO_COMPOSITION_CONTROL_SESSION_PHASE_CATALOG:
        runtime->phase = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_CATALOG;
        break;
    case MEISO_COMPOSITION_CONTROL_SESSION_PHASE_REBIND:
        if (runtime->deliveries_started) {
            meiso_k230d_composition_edge_runtime_private_enter_terminal(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CATALOG_CHANGED, 0
            );
        } else {
            runtime->phase = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_REBIND;
        }
        break;
    case MEISO_COMPOSITION_CONTROL_SESSION_PHASE_READY:
        if (!meiso_k230d_composition_edge_runtime_private_ensure_coordinator(runtime)) {
            return false;
        }
        runtime->deliveries_started = true;
        runtime->phase = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_READY;
        break;
    case MEISO_COMPOSITION_CONTROL_SESSION_PHASE_RX_OBLIGATION:
        break;
    case MEISO_COMPOSITION_CONTROL_SESSION_PHASE_TERMINAL:
    default:
        return false;
    }
    return true;
}

bool meiso_k230d_composition_edge_runtime_private_remember_visible_blob(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *transfer
)
{
    size_t index;
    if (runtime == NULL || transfer == NULL) {
        return false;
    }
    for (index = 0U; index < runtime->visible_blob_count; ++index) {
        if (meiso_content_id_equal(
                &runtime->visible_blob_ids[index], &transfer->content_id
            )) {
            return runtime->visible_blob_sizes[index] == transfer->total_size;
        }
    }
    if (runtime->visible_blob_count >=
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MESH_BLOB_SLOTS) {
        return false;
    }
    index = runtime->visible_blob_count++;
    runtime->visible_blob_ids[index] = transfer->content_id;
    runtime->visible_blob_sizes[index] = transfer->total_size;
    return true;
}

bool meiso_k230d_composition_edge_runtime_private_apply_visible_blobs(MeisoK230dCompositionEdgeRuntime *runtime)
{
    size_t index;
    MeisoCompositionSceneBlobVisibilityStatus status;
    if (!runtime->desired_owner_present) {
        return true;
    }
    for (index = 0U; index < runtime->visible_blob_count; ++index) {
        status = meiso_composition_scene_store_mark_blob_visible(
            runtime->scene_store, runtime->desired_owner.incarnation,
            &runtime->visible_blob_ids[index],
            (size_t)runtime->visible_blob_sizes[index]
        );
        if (status != MEISO_COMPOSITION_SCENE_BLOB_VISIBLE &&
            status != MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_UNCHANGED) {
            return false;
        }
    }
    return true;
}

bool meiso_k230d_composition_edge_runtime_private_handle_desired_offer(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoRuntimeEvent *event,
    MeisoTimePoint now,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
)
{
    size_t index;
    EdgeDesiredChannel *channel;
    MeisoStateReceiveStatus receive;
    MeisoStateView current;
    MeisoStateOwnerStatus owner_status;
    if (!meiso_k230d_composition_edge_runtime_private_route_channel(
            runtime, event->route, meiso_k230d_composition_edge_runtime_private_desired_route_index,
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_COUNT, &index
        ) || !meiso_k230d_composition_edge_runtime_private_ensure_desired_owner(runtime, event->state_offer.state.owner)) {
        return false;
    }
    channel = &runtime->desired[index];
    if (!channel->owner_started) {
        owner_status = meiso_state_replica_begin_owner(
            channel->replica, event->state_offer.state.owner
        );
        if (owner_status != MEISO_STATE_OWNER_STARTED &&
            owner_status != MEISO_STATE_OWNER_UNCHANGED) {
            return false;
        }
        channel->owner_started = true;
    }
    receive = meiso_state_replica_receive(
        channel->replica, event->state_offer.state.owner,
        event->state_offer.state.version,
        event->state_offer.state.payload
    );
    if (meiso_composition_control_session_commit_state_receive(
            runtime->control_session, event->route, &event->state_offer, receive
        ) != MEISO_COMPOSITION_CONTROL_SESSION_OK ||
        (receive != MEISO_STATE_RECEIVE_NEW &&
         receive != MEISO_STATE_RECEIVE_DUPLICATE) ||
        !meiso_state_replica_current(channel->replica, &current)) {
        return false;
    }
    channel->accept_pending = true;
    channel->accept_generation = event->state_offer.generation;
    channel->accept_owner = event->state_offer.state.owner;
    channel->accept_version = event->state_offer.state.version;
    if ((receive == MEISO_STATE_RECEIVE_NEW &&
         !apply_desired(runtime, index, current, now)) ||
        !meiso_k230d_composition_edge_runtime_private_apply_visible_blobs(runtime)) {
        return false;
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event =
        runtime->actual[index].snapshot.apply_result ==
                MEISO_RUNTIME_APPLY_REJECTED ?
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_DESIRED_REJECTED :
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_DESIRED_APPLIED;
    out_result->desired_channel_present = true;
    out_result->desired_channel = (uint8_t)index;
    out_result->desired_reference_present = true;
    out_result->desired_reference = (MeisoRuntimeDesiredReference){
        true, current.owner, current.version
    };
    return true;
}

bool meiso_k230d_composition_edge_runtime_private_handle_actual_accept(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoRuntimeEvent *event,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
)
{
    size_t index;
    EdgeActualChannel *channel;
    MeisoStateOffer next;
    MeisoStateAcceptStatus status;
    if (!meiso_k230d_composition_edge_runtime_private_route_channel(
            runtime, event->route, meiso_k230d_composition_edge_runtime_private_actual_route_index,
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT, &index
        )) {
        return false;
    }
    channel = &runtime->actual[index];
    status = meiso_state_publisher_mark_delivery_accepted(
        channel->publisher, event->state_accept_generation,
        event->state_accept_owner, event->state_accept_version
    );
    if (status != MEISO_STATE_ACCEPT_CONFIRMED &&
        status != MEISO_STATE_ACCEPT_DUPLICATE &&
        status != MEISO_STATE_ACCEPT_STALE) {
        return false;
    }
    channel->offer_sent = false;
    channel->snapshot.delivery_pending =
        meiso_state_publisher_next_offer(channel->publisher, &next);
    channel->snapshot.delivery_accepted =
        !channel->snapshot.delivery_pending;
    channel->snapshot.delivery_failed = false;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_ACTUAL_DELIVERY;
    out_result->actual_channel_present = true;
    out_result->actual_channel = (uint8_t)index;
    return true;
}
