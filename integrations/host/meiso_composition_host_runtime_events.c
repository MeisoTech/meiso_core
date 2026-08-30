/* Runtime event decoding and accepted Actual/Stream/Blob projections. */
#include "meiso_composition_host_runtime_private.h"

#include <errno.h>
#include <string.h>

static bool decode_actual_profile(
    MeisoCompositionHostRuntime *runtime,
    size_t index,
    MeisoByteView payload
)
{
    MeisoCompositionHostRuntimePrivateActualChannel *channel = &runtime->actual[index];
    MeisoRuntimeActualState actual;
    if (!meiso_runtime_actual_state_decode(payload, &actual)) {
        return false;
    }
    channel->snapshot.observed_desired = actual.observed_desired;
    channel->snapshot.applied_desired = actual.applied_desired;
    channel->snapshot.apply_result = actual.apply_result;
    channel->snapshot.availability = actual.availability;
    channel->snapshot.effective_enabled = actual.effective_enabled;
    channel->snapshot.production_status = actual.production_status;
    channel->snapshot.reason = actual.reason;
    channel->snapshot.production_epoch = actual.production_epoch;
    channel->snapshot.profile_decoded = false;
    if (!actual.effective_enabled && actual.profile.size == 0U) {
        switch (index) {
        case MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_WIFI:
            runtime->wifi_effective_present = false;
            break;
        case MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_PRESENTATION:
            runtime->presentation_status_present = false;
            break;
        case MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_SCENE:
            runtime->scene_status_present = false;
            break;
        case MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_DEVICE:
            runtime->device_effective_present = false;
            break;
        case MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_RENDER_MEDIA:
            runtime->render_media_actual_present = false;
            break;
        case MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_CAMERA_MEDIA:
            runtime->camera_media_actual_present = false;
            break;
        default:
            return false;
        }
        channel->snapshot.profile_decoded = true;
        return true;
    }
    switch (index) {
    case MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_WIFI:
        if (meiso_wifi_telemetry_effective_parameters_decode(
                actual.profile, &runtime->wifi_effective
            ) != MEISO_WIFI_TELEMETRY_CODEC_OK) return false;
        runtime->wifi_effective_present = true;
        break;
    case MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_PRESENTATION:
        if (meiso_composition_scene_presentation_status_decode(
                actual.profile, &runtime->presentation_status
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) return false;
        runtime->presentation_status_present = true;
        break;
    case MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_SCENE:
        if (meiso_composition_scene_status_decode(
                actual.profile, &runtime->scene_status
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) return false;
        runtime->scene_status_present = true;
        break;
    case MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_DEVICE:
        if (meiso_device_telemetry_parameters_decode(
                actual.profile, &runtime->device_effective
            ) != MEISO_DEVICE_TELEMETRY_CODEC_OK) return false;
        runtime->device_effective_present = true;
        break;
    case MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_RENDER_MEDIA:
        if (meiso_composition_media_actual_decode(
                actual.profile, &runtime->render_media_actual
            ) != MEISO_COMPOSITION_PROFILE_OK ||
            runtime->render_media_actual.source !=
                MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) return false;
        runtime->render_media_actual_present = true;
        break;
    case MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_CAMERA_MEDIA:
        if (meiso_composition_media_actual_decode(
                actual.profile, &runtime->camera_media_actual
            ) != MEISO_COMPOSITION_PROFILE_OK ||
            runtime->camera_media_actual.source !=
                MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA) return false;
        runtime->camera_media_actual_present = true;
        break;
    default:
        return false;
    }
    channel->snapshot.profile_decoded = true;
    return true;
}

static void note_desired_actual_observed(
    MeisoCompositionHostRuntime *runtime,
    size_t index
)
{
    MeisoCompositionHostRuntimePrivateDesiredChannel *desired;
    const MeisoRuntimeDesiredReference *observed;
    if (index >= MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT) {
        return;
    }
    desired = &runtime->desired[index];
    observed = &runtime->actual[index].snapshot.observed_desired;
    if (!desired->inflight_present || !observed->present ||
        !meiso_composition_host_runtime_private_owner_equal(desired->inflight_owner, observed->owner) ||
        desired->inflight_version != observed->version) {
        return;
    }
    desired->inflight_actual_observed = true;
    meiso_composition_host_runtime_private_expose_desired_inflight(desired);
    meiso_composition_host_runtime_private_retire_completed_desired_inflight(desired);
}

static bool handle_actual_offer(
    MeisoCompositionHostRuntime *runtime,
    const MeisoRuntimeEvent *event,
    MeisoCompositionHostRuntimePumpResult *out_result
)
{
    size_t index;
    MeisoCompositionHostRuntimePrivateActualChannel *channel;
    MeisoStateReceiveStatus receive;
    MeisoStateView current;
    if (!meiso_composition_host_runtime_private_route_channel(runtime, event->route, meiso_composition_host_runtime_private_actual_route_index,
                       MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT, &index)) {
        return false;
    }
    channel = &runtime->actual[index];
    if (!channel->owner_started) {
        if (event->state_offer.state.owner.role != MEISO_OWNER_ROLE_EDGE ||
            meiso_state_replica_begin_owner(
                channel->replica, event->state_offer.state.owner
            ) != MEISO_STATE_OWNER_STARTED) {
            return false;
        }
        channel->owner_started = true;
        channel->snapshot.state_owner = event->state_offer.state.owner;
    } else if (!meiso_composition_host_runtime_private_owner_equal(
                   channel->snapshot.state_owner,
                   event->state_offer.state.owner
               )) {
        return false;
    }
    receive = meiso_state_replica_receive(
        channel->replica, event->state_offer.state.owner,
        event->state_offer.state.version,
        event->state_offer.state.payload
    );
    if (meiso_composition_control_session_commit_state_receive(
            runtime->control_session, event->route, &event->state_offer, receive
        ) != MEISO_COMPOSITION_CONTROL_SESSION_OK) {
        return false;
    }
    if (receive != MEISO_STATE_RECEIVE_NEW &&
        receive != MEISO_STATE_RECEIVE_DUPLICATE) {
        return false;
    }
    if (!meiso_state_replica_current(channel->replica, &current) ||
        !decode_actual_profile(runtime, index, current.payload)) {
        return false;
    }
    channel->snapshot.present = true;
    channel->snapshot.state_owner = current.owner;
    channel->snapshot.state_version = current.version;
    note_desired_actual_observed(runtime, index);
    channel->accept_pending = true;
    channel->accept_generation = event->state_offer.generation;
    channel->accept_owner = event->state_offer.state.owner;
    channel->accept_version = event->state_offer.state.version;
    if (runtime->active_media != NULL && !meiso_composition_host_runtime_private_active_media_scope_current(runtime)) {
        meiso_composition_host_runtime_private_terminate_active_media(
            runtime, MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED, 0
        );
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_ACTUAL_UPDATED;
    out_result->actual_channel_present = true;
    out_result->actual_channel = (uint8_t)index;
    return true;
}

static bool handle_desired_accept(
    MeisoCompositionHostRuntime *runtime,
    const MeisoRuntimeEvent *event,
    MeisoCompositionHostRuntimePumpResult *out_result
)
{
    size_t index;
    MeisoCompositionHostRuntimePrivateDesiredChannel *channel;
    MeisoStateAcceptStatus accept_status;
    uint64_t accepted_command_id;
    if (!meiso_composition_host_runtime_private_route_channel(runtime, event->route, meiso_composition_host_runtime_private_desired_route_index,
                       MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT, &index)) {
        return false;
    }
    channel = &runtime->desired[index];
    if (!meiso_composition_host_runtime_private_desired_inflight_matches(
            channel, event->state_accept_generation,
            event->state_accept_owner, event->state_accept_version
        )) {
        return false;
    }
    accept_status = meiso_state_publisher_mark_delivery_accepted(
        channel->publisher, event->state_accept_generation,
        event->state_accept_owner, event->state_accept_version
    );
    if (accept_status != MEISO_STATE_ACCEPT_CONFIRMED &&
        accept_status != MEISO_STATE_ACCEPT_DUPLICATE &&
        accept_status != MEISO_STATE_ACCEPT_STALE) {
        return false;
    }
    accepted_command_id = channel->inflight_command_id;
    channel->inflight_offer_accepted = true;
    if (channel->snapshot.command_id == channel->inflight_command_id &&
        channel->snapshot.state_version == channel->inflight_version) {
        channel->snapshot.delivery_pending = false;
        channel->snapshot.delivery_accepted = true;
        channel->snapshot.delivery_failed = false;
    }
    meiso_composition_host_runtime_private_expose_desired_inflight(channel);
    meiso_composition_host_runtime_private_retire_completed_desired_inflight(channel);
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_DESIRED_DELIVERY;
    out_result->command_present = true;
    out_result->command_id = accepted_command_id;
    out_result->desired_channel_present = true;
    out_result->desired_channel = (uint8_t)index;
    return true;
}

static bool decode_inline(
    MeisoCompositionHostRuntime *runtime,
    size_t index,
    MeisoByteView payload
)
{
    switch (index) {
    case MEISO_COMPOSITION_HOST_RUNTIME_INLINE_WIFI:
        if (meiso_wifi_telemetry_sample_decode(
                payload, &runtime->wifi_sample
            ) != MEISO_WIFI_TELEMETRY_CODEC_OK) return false;
        runtime->wifi_sample_present = true;
        return true;
    case MEISO_COMPOSITION_HOST_RUNTIME_INLINE_RENDER_RESULT:
        if (meiso_composition_scene_render_result_decode(
                payload, &runtime->render_result
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) return false;
        runtime->render_result_present = true;
        return true;
    case MEISO_COMPOSITION_HOST_RUNTIME_INLINE_DEVICE:
        if (meiso_device_telemetry_sample_decode(
                payload, &runtime->device_sample
            ) != MEISO_DEVICE_TELEMETRY_CODEC_OK) return false;
        runtime->device_sample_present = true;
        return true;
    default:
        return false;
    }
}

static bool handle_inline_sample(
    MeisoCompositionHostRuntime *runtime,
    const MeisoRuntimeEvent *event,
    MeisoCompositionHostRuntimePumpResult *out_result
)
{
    size_t index;
    MeisoCompositionHostRuntimePrivateInlineChannel *channel;
    MeisoStreamReceiveResult receive;
    MeisoStreamSample sample;
    if (!meiso_composition_host_runtime_private_route_channel(runtime, event->route, meiso_composition_host_runtime_private_inline_route_index,
                       MEISO_COMPOSITION_HOST_RUNTIME_INLINE_STREAM_COUNT,
                       &index)) {
        return false;
    }
    channel = &runtime->inline_streams[index];
    if (!channel->producer_present ||
        !meiso_composition_host_runtime_private_owner_equal(channel->producer, event->stream_sample.key.producer) ||
        channel->epoch != event->production_epoch) {
        MeisoStreamProducerStatus producer_status = meiso_stream_begin_producer(
            channel->stream, event->stream_sample.key.producer,
            event->production_epoch
        );
        if (producer_status != MEISO_STREAM_PRODUCER_STARTED &&
            producer_status != MEISO_STREAM_PRODUCER_SWITCHED &&
            producer_status != MEISO_STREAM_PRODUCER_UNCHANGED) {
            return false;
        }
        channel->producer_present = true;
        channel->producer = event->stream_sample.key.producer;
        channel->epoch = event->production_epoch;
    }
    receive = meiso_stream_receive(channel->stream, &event->stream_sample, NULL);
    if (receive.code == MEISO_STREAM_RECEIVE_DUPLICATE ||
        receive.code == MEISO_STREAM_RECEIVE_STALE) {
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INLINE_UPDATED;
        out_result->inline_channel_present = true;
        out_result->inline_channel = (uint8_t)index;
        return true;
    }
    if (receive.code != MEISO_STREAM_RECEIVE_ACCEPTED ||
        !meiso_stream_peek(channel->stream, &sample) ||
        !decode_inline(runtime, index, sample.payload)) {
        return false;
    }
    channel->snapshot.present = true;
    channel->snapshot.key = sample.key;
    channel->snapshot.source_time = sample.source_time;
    channel->snapshot.receive_flags = receive.flags;
    channel->snapshot.missing_before = receive.missing_before;
    ++channel->snapshot.received_count;
    if ((receive.flags & MEISO_STREAM_RECEIVE_FLAG_DROPPED_OLDEST) != 0U)
        ++channel->snapshot.replaced_count;
    if (!meiso_stream_pop(channel->stream)) {
        return false;
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INLINE_UPDATED;
    out_result->inline_channel_present = true;
    out_result->inline_channel = (uint8_t)index;
    return true;
}

static bool handle_blob_result(
    MeisoCompositionHostRuntime *runtime,
    const MeisoRuntimeEvent *event,
    MeisoCompositionHostRuntimePumpResult *out_result
)
{
    if (!runtime->mesh.transfer_active ||
        event->blob_transfer.transfer_id != runtime->blob_transfer.transfer_id ||
        !meiso_composition_host_runtime_private_route_key_equal(event->blob_transfer.route,
                         runtime->blob_transfer.route)) {
        return false;
    }
    if (event->blob_operation == MEISO_RUNTIME_BLOB_BEGIN &&
        event->blob_result == MEISO_RUNTIME_BLOB_RESULT_READY) {
        runtime->mesh.begin_ready = true;
    } else {
        runtime->mesh.terminal_result_present = true;
        runtime->mesh.terminal_result = event->blob_result;
        runtime->mesh.edge_core_published =
            event->blob_result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED ||
            event->blob_result == MEISO_RUNTIME_BLOB_RESULT_DUPLICATE;
        runtime->mesh.queued = false;
        runtime->mesh.transfer_active = false;
        runtime->mesh.waiting_result = false;
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_MESH_PROGRESS;
    out_result->command_present = true;
    out_result->command_id = runtime->mesh.command_id;
    return true;
}

bool meiso_composition_host_runtime_private_handle_runtime_event(
    MeisoCompositionHostRuntime *runtime,
    const MeisoRuntimeEvent *event,
    MeisoCompositionHostRuntimePumpResult *out_result
)
{
    switch (event->type) {
    case MEISO_RUNTIME_EVENT_STATE_OFFER:
        return handle_actual_offer(runtime, event, out_result);
    case MEISO_RUNTIME_EVENT_STATE_ACCEPT:
        return handle_desired_accept(runtime, event, out_result);
    case MEISO_RUNTIME_EVENT_STREAM_SAMPLE:
        return handle_inline_sample(runtime, event, out_result);
    case MEISO_RUNTIME_EVENT_BLOB_RESULT:
        return handle_blob_result(runtime, event, out_result);
    case MEISO_RUNTIME_EVENT_CATALOG_OFFERED:
    case MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED:
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_CONTROL_PROGRESS;
        return true;
    default:
        return false;
    }
}

void meiso_composition_host_runtime_private_refresh_phase(MeisoCompositionHostRuntime *runtime)
{
    MeisoCompositionControlSessionSnapshot control_session;
    if (!meiso_composition_control_session_snapshot(runtime->control_session, &control_session)) {
        meiso_composition_host_runtime_private_enter_terminal(
            runtime, MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_INTERNAL_FAILURE,
            MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
        );
        return;
    }
    if (control_session.runtime.active_catalog_generation != 0U &&
        control_session.runtime.active_catalog_generation != runtime->config.catalog_generation) {
        meiso_composition_host_runtime_private_enter_terminal(
            runtime, MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_CATALOG_CHANGED,
            MEISO_COMPOSITION_MEDIA_TERMINAL_CATALOG_CHANGED, EPROTO
        );
        return;
    }
    if (control_session.terminal) {
        meiso_composition_host_runtime_private_enter_terminal(
            runtime,
            control_session.session.failure == MEISO_SESSION_FAILURE_TRANSPORT_LOST ?
                MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_CONTROL_TRANSPORT :
                MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_CONTROL_SESSION,
            MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
        );
    } else if (control_session.phase == MEISO_COMPOSITION_CONTROL_SESSION_PHASE_READY) {
        runtime->phase = MEISO_COMPOSITION_HOST_RUNTIME_PHASE_READY;
    } else if (control_session.phase == MEISO_COMPOSITION_CONTROL_SESSION_PHASE_REBIND) {
        runtime->phase = MEISO_COMPOSITION_HOST_RUNTIME_PHASE_REBIND;
    } else if (control_session.phase == MEISO_COMPOSITION_CONTROL_SESSION_PHASE_CATALOG) {
        runtime->phase = MEISO_COMPOSITION_HOST_RUNTIME_PHASE_CATALOG;
    } else {
        runtime->phase = MEISO_COMPOSITION_HOST_RUNTIME_PHASE_HANDSHAKE;
    }
}
