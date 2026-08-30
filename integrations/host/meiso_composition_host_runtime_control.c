/* Control-source scheduling, completions and terminal-state ownership. */
#include "meiso_composition_host_runtime_private.h"

#include <errno.h>
#include <string.h>

void meiso_composition_host_runtime_private_clear_desired_inflight(MeisoCompositionHostRuntimePrivateDesiredChannel *channel)
{
    memset(channel->inflight_payload, 0, sizeof(channel->inflight_payload));
    channel->inflight_payload_size = 0U;
    channel->inflight_command_id = 0U;
    memset(&channel->inflight_owner, 0, sizeof(channel->inflight_owner));
    channel->inflight_version = 0U;
    memset(&channel->inflight_generation, 0,
           sizeof(channel->inflight_generation));
    channel->inflight_present = false;
    channel->inflight_source_completed = false;
    channel->inflight_offer_accepted = false;
    channel->inflight_actual_observed = false;
    channel->offer_queued = false;
    channel->offer_sent = false;
    channel->snapshot.inflight_present = false;
    channel->snapshot.inflight_command_id = 0U;
    memset(&channel->snapshot.inflight_owner, 0,
           sizeof(channel->snapshot.inflight_owner));
    channel->snapshot.inflight_state_version = 0U;
    memset(&channel->snapshot.inflight_delivery_generation, 0,
           sizeof(channel->snapshot.inflight_delivery_generation));
    channel->snapshot.inflight_source_completed = false;
    channel->snapshot.inflight_offer_accepted = false;
    channel->snapshot.inflight_actual_observed = false;
}

void meiso_composition_host_runtime_private_expose_desired_inflight(MeisoCompositionHostRuntimePrivateDesiredChannel *channel)
{
    channel->snapshot.inflight_present = channel->inflight_present;
    channel->snapshot.inflight_command_id = channel->inflight_command_id;
    channel->snapshot.inflight_owner = channel->inflight_owner;
    channel->snapshot.inflight_state_version = channel->inflight_version;
    channel->snapshot.inflight_delivery_generation =
        channel->inflight_generation;
    channel->snapshot.inflight_source_completed =
        channel->inflight_source_completed;
    channel->snapshot.inflight_offer_accepted =
        channel->inflight_offer_accepted;
    channel->snapshot.inflight_actual_observed =
        channel->inflight_actual_observed;
}

static void clear_terminal_views(MeisoCompositionHostRuntime *runtime)
{
    const uint64_t transfer_id_high_water =
        runtime->mesh.transfer_id_high_water;
    size_t index;
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT;
         ++index) {
        memset(&runtime->desired[index].snapshot, 0,
               sizeof(runtime->desired[index].snapshot));
        meiso_composition_host_runtime_private_clear_desired_inflight(&runtime->desired[index]);
        runtime->desired[index].plan_present = false;
        runtime->desired[index].plan_echoed = false;
        memset(&runtime->desired[index].plan, 0,
               sizeof(runtime->desired[index].plan));
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT;
         ++index) {
        memset(&runtime->actual[index].snapshot, 0,
               sizeof(runtime->actual[index].snapshot));
        runtime->actual[index].accept_pending = false;
        runtime->actual[index].accept_work_queued = false;
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_INLINE_STREAM_COUNT;
         ++index) {
        memset(&runtime->inline_streams[index].snapshot, 0,
               sizeof(runtime->inline_streams[index].snapshot));
        runtime->inline_streams[index].producer_present = false;
    }
    memset(&runtime->mesh, 0, sizeof(runtime->mesh));
    runtime->mesh.transfer_id_high_water = transfer_id_high_water;
    memset(runtime->blob_work_queued, 0, sizeof(runtime->blob_work_queued));
    runtime->blob_chunk_queued_bytes = 0U;
    memset(&runtime->blob_transfer, 0, sizeof(runtime->blob_transfer));
    runtime->mesh_size = 0U;
    memset(runtime->mesh_bytes, 0, runtime->mesh_capacity);
    runtime->wifi_effective_present = false;
    memset(&runtime->wifi_effective, 0, sizeof(runtime->wifi_effective));
    runtime->wifi_sample_present = false;
    memset(&runtime->wifi_sample, 0, sizeof(runtime->wifi_sample));
    runtime->presentation_status_present = false;
    memset(&runtime->presentation_status, 0, sizeof(runtime->presentation_status));
    runtime->scene_status_present = false;
    memset(&runtime->scene_status, 0, sizeof(runtime->scene_status));
    runtime->render_result_present = false;
    memset(&runtime->render_result, 0, sizeof(runtime->render_result));
    runtime->device_effective_present = false;
    memset(&runtime->device_effective, 0, sizeof(runtime->device_effective));
    runtime->device_sample_present = false;
    memset(&runtime->device_sample, 0, sizeof(runtime->device_sample));
    runtime->render_media_actual_present = false;
    memset(&runtime->render_media_actual, 0,
           sizeof(runtime->render_media_actual));
    runtime->camera_media_actual_present = false;
    memset(&runtime->camera_media_actual, 0,
           sizeof(runtime->camera_media_actual));
    memset(runtime->desired_scratch, 0, sizeof(runtime->desired_scratch));
    memset(runtime->profile_scratch, 0, sizeof(runtime->profile_scratch));
}

void meiso_composition_host_runtime_private_enter_terminal(
    MeisoCompositionHostRuntime *runtime,
    MeisoCompositionHostRuntimeTerminalReason reason,
    MeisoCompositionMediaTerminalReason media_reason,
    int error_number
)
{
    meiso_composition_host_runtime_private_terminate_active_media(runtime, media_reason, error_number);
    meiso_composition_control_session_abort(runtime->control_session, error_number);
    clear_terminal_views(runtime);
    runtime->phase = MEISO_COMPOSITION_HOST_RUNTIME_PHASE_TERMINAL;
    runtime->terminal_reason = reason;
}

bool meiso_composition_host_runtime_private_route_key_equal(MeisoRuntimeRouteKey lhs, MeisoRuntimeRouteKey rhs)
{
    return lhs.catalog_generation == rhs.catalog_generation &&
        lhs.endpoint_id.high == rhs.endpoint_id.high &&
        lhs.endpoint_id.low == rhs.endpoint_id.low &&
        lhs.endpoint_incarnation == rhs.endpoint_incarnation &&
        meiso_resource_id_equal(lhs.resource_id, rhs.resource_id) &&
        lhs.facet == rhs.facet;
}

bool meiso_composition_host_runtime_private_route_channel(
    MeisoCompositionHostRuntime *runtime,
    MeisoRuntimeRouteKey route,
    const uint8_t *routes,
    size_t count,
    size_t *out_index
)
{
    size_t index;
    MeisoRuntimeRouteKey expected;
    for (index = 0U; index < count; ++index) {
        if (meiso_composition_control_session_route_key(
                runtime->control_session, routes[index], &expected
            ) && meiso_composition_host_runtime_private_route_key_equal(route, expected)) {
            *out_index = index;
            return true;
        }
    }
    return false;
}

bool meiso_composition_host_runtime_private_owner_equal(MeisoOwnerScope lhs, MeisoOwnerScope rhs)
{
    return lhs.role == rhs.role && lhs.incarnation == rhs.incarnation;
}

bool meiso_composition_host_runtime_private_desired_inflight_matches(
    const MeisoCompositionHostRuntimePrivateDesiredChannel *channel,
    MeisoDeliveryGeneration generation,
    MeisoOwnerScope owner,
    uint64_t version
)
{
    return channel->inflight_present &&
        channel->inflight_generation.value == generation.value &&
        meiso_composition_host_runtime_private_owner_equal(channel->inflight_owner, owner) &&
        channel->inflight_version == version;
}

void meiso_composition_host_runtime_private_retire_completed_desired_inflight(MeisoCompositionHostRuntimePrivateDesiredChannel *channel)
{
    if (channel->inflight_present && channel->inflight_source_completed &&
        channel->inflight_offer_accepted &&
        channel->inflight_actual_observed) {
        meiso_composition_host_runtime_private_clear_desired_inflight(channel);
    }
}

static bool start_desired_deliveries(MeisoCompositionHostRuntime *runtime)
{
    size_t index;
    if (runtime->deliveries_started) {
        return true;
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT;
         ++index) {
        MeisoStateDeliveryBeginStatus status =
            meiso_state_publisher_begin_delivery(
                runtime->desired[index].publisher,
                runtime->desired[index].snapshot.delivery_generation
            );
        if (status != MEISO_STATE_DELIVERY_STARTED) {
            return false;
        }
        runtime->desired[index].snapshot.delivery_active = true;
    }
    runtime->deliveries_started = true;
    return true;
}

static bool queue_desired_offer(
    MeisoCompositionHostRuntime *runtime,
    size_t index,
    MeisoCompositionHostRuntimePumpResult *out_result
)
{
    MeisoCompositionHostRuntimePrivateDesiredChannel *channel = &runtime->desired[index];
    MeisoStateOffer offer;
    MeisoRuntimeRouteKey route;
    MeisoRuntimeSchedulerWork work;
    MeisoCompositionControlSessionStatus status;
    if (!channel->snapshot.requested || channel->inflight_present ||
        !meiso_state_publisher_next_offer(
            channel->publisher, &offer
        )) {
        return false;
    }
    if (offer.state.payload.size > sizeof(channel->inflight_payload) ||
        (offer.state.payload.size != 0U &&
         offer.state.payload.data == NULL)) {
        return false;
    }
    if (!meiso_composition_control_session_route_key(
            runtime->control_session, meiso_composition_host_runtime_private_desired_route_index[index], &route
        )) {
        return false;
    }
    if (offer.state.payload.size != 0U) {
        memcpy(channel->inflight_payload, offer.state.payload.data,
               offer.state.payload.size);
    }
    channel->inflight_payload_size = offer.state.payload.size;
    offer.state.payload.data = channel->inflight_payload;
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
    work.value.state_offer.route = route;
    work.value.state_offer.offer = offer;
    status = meiso_composition_control_session_queue_work(
        runtime->control_session, meiso_composition_host_runtime_private_desired_source_index[index], &work
    );
    if (status != MEISO_COMPOSITION_CONTROL_SESSION_OK) {
        memset(channel->inflight_payload, 0,
               sizeof(channel->inflight_payload));
        channel->inflight_payload_size = 0U;
        return false;
    }
    channel->inflight_present = true;
    channel->inflight_source_completed = false;
    channel->inflight_offer_accepted = false;
    channel->inflight_actual_observed = false;
    channel->inflight_command_id = channel->snapshot.command_id;
    channel->inflight_owner = offer.state.owner;
    channel->inflight_version = offer.state.version;
    channel->inflight_generation = offer.generation;
    channel->offer_queued = true;
    channel->offer_sent = false;
    meiso_composition_host_runtime_private_expose_desired_inflight(channel);
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_DESIRED_DELIVERY;
    out_result->command_present = true;
    out_result->command_id = channel->inflight_command_id;
    out_result->desired_channel_present = true;
    out_result->desired_channel = (uint8_t)index;
    return true;
}

static bool queue_actual_accept(
    MeisoCompositionHostRuntime *runtime,
    size_t index,
    MeisoCompositionHostRuntimePumpResult *out_result
)
{
    MeisoCompositionHostRuntimePrivateActualChannel *channel = &runtime->actual[index];
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeRouteKey route;
    if (!channel->accept_pending || channel->accept_work_queued ||
        !meiso_composition_control_session_route_key(
            runtime->control_session, meiso_composition_host_runtime_private_actual_route_index[index], &route
        )) {
        return false;
    }
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
    work.value.state_accept.route = route;
    work.value.state_accept.generation = channel->accept_generation;
    work.value.state_accept.owner = channel->accept_owner;
    work.value.state_accept.version = channel->accept_version;
    if (meiso_composition_control_session_queue_work(
            runtime->control_session, meiso_composition_host_runtime_private_actual_source_index[index], &work
        ) != MEISO_COMPOSITION_CONTROL_SESSION_OK) {
        return false;
    }
    channel->accept_work_queued = true;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_ACTUAL_UPDATED;
    out_result->actual_channel_present = true;
    out_result->actual_channel = (uint8_t)index;
    return true;
}

static bool queue_blob_work(
    MeisoCompositionHostRuntime *runtime,
    MeisoCompositionHostRuntimePumpResult *out_result
)
{
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeRouteKey route;
    uint8_t source;
    uint8_t operation_index;
    size_t remaining;
    size_t chunk;
    if (!runtime->mesh.queued || !meiso_composition_control_session_route_key(
            runtime->control_session, MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_MESH_BLOB, &route
        )) {
        return false;
    }
    memset(&work, 0, sizeof(work));
    if (!runtime->mesh.transfer_active) {
        operation_index = 0U;
        source = MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_BLOB_BEGIN;
        work.kind = MEISO_RUNTIME_SCHEDULER_WORK_BLOB_BEGIN;
        work.value.blob_begin.route = route;
        work.value.blob_begin.content_id = runtime->mesh.content_id;
        work.value.blob_begin.total_size = runtime->mesh.total_bytes;
    } else if (!runtime->mesh.begin_ready || runtime->mesh.waiting_result) {
        return false;
    } else if (runtime->mesh.next_offset < runtime->mesh.total_bytes) {
        operation_index = 1U;
        source = MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_BLOB_CHUNK;
        remaining = (size_t)(runtime->mesh.total_bytes - runtime->mesh.next_offset);
        chunk = remaining < MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE ?
            remaining : MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE;
        work.kind = MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK;
        work.value.blob_chunk.transfer = runtime->blob_transfer;
        work.value.blob_chunk.offset = runtime->mesh.next_offset;
        work.value.blob_chunk.payload = (MeisoByteView){
            runtime->mesh_bytes + (size_t)runtime->mesh.next_offset, chunk
        };
        runtime->blob_chunk_queued_bytes = (uint32_t)chunk;
    } else {
        operation_index = 2U;
        source = MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_BLOB_COMMIT;
        work.kind = MEISO_RUNTIME_SCHEDULER_WORK_BLOB_COMMIT;
        work.value.blob_commit.transfer = runtime->blob_transfer;
    }
    if (runtime->blob_work_queued[operation_index] ||
        meiso_composition_control_session_queue_work(
            runtime->control_session, source, &work
        ) != MEISO_COMPOSITION_CONTROL_SESSION_OK) {
        return false;
    }
    runtime->blob_work_queued[operation_index] = true;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_MESH_PROGRESS;
    out_result->command_present = true;
    out_result->command_id = runtime->mesh.command_id;
    return true;
}

static bool handle_source_completion(
    MeisoCompositionHostRuntime *runtime,
    MeisoCompositionHostRuntimePumpResult *out_result
)
{
    MeisoRuntimeSchedulerCompletion completion;
    size_t index;
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT;
         ++index) {
        if (meiso_composition_control_session_take_completion(
                runtime->control_session, meiso_composition_host_runtime_private_desired_source_index[index], &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            MeisoCompositionHostRuntimePrivateDesiredChannel *channel = &runtime->desired[index];
            uint64_t completed_command_id;
            if (!channel->inflight_present || !channel->offer_queued) {
                meiso_composition_host_runtime_private_enter_terminal(
                    runtime,
                    MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_INTERNAL_FAILURE,
                    MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
                );
                return true;
            }
            completed_command_id = channel->inflight_command_id;
            channel->offer_queued = false;
            if (completion.code == MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT) {
                channel->offer_sent = true;
                channel->inflight_source_completed = true;
                meiso_composition_host_runtime_private_expose_desired_inflight(channel);
                meiso_composition_host_runtime_private_retire_completed_desired_inflight(channel);
            } else {
                (void)meiso_state_publisher_mark_delivery_failed(
                    channel->publisher, channel->inflight_generation,
                    channel->inflight_owner, channel->inflight_version
                );
                if (channel->snapshot.command_id ==
                        channel->inflight_command_id &&
                    channel->snapshot.state_version ==
                        channel->inflight_version) {
                    channel->snapshot.delivery_failed = true;
                    channel->snapshot.delivery_pending = false;
                }
                meiso_composition_host_runtime_private_clear_desired_inflight(channel);
            }
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event =
                MEISO_COMPOSITION_HOST_RUNTIME_PUMP_DESIRED_DELIVERY;
            out_result->command_present = true;
            out_result->command_id = completed_command_id;
            out_result->desired_channel_present = true;
            out_result->desired_channel = (uint8_t)index;
            return true;
        }
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT;
         ++index) {
        if (meiso_composition_control_session_take_completion(
                runtime->control_session, meiso_composition_host_runtime_private_actual_source_index[index], &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            runtime->actual[index].accept_work_queued = false;
            if (completion.code == MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT) {
                runtime->actual[index].accept_pending = false;
            } else {
                meiso_composition_host_runtime_private_enter_terminal(
                    runtime,
                    MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_INTERNAL_FAILURE,
                    MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
                );
            }
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_ACTUAL_UPDATED;
            out_result->actual_channel_present = true;
            out_result->actual_channel = (uint8_t)index;
            return true;
        }
    }
    for (index = 0U; index < 3U; ++index) {
        const uint8_t source = index == 0U ? MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_BLOB_BEGIN :
            index == 1U ? MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_BLOB_CHUNK : MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_SOURCE_BLOB_COMMIT;
        if (meiso_composition_control_session_take_completion(
                runtime->control_session, source, &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            runtime->blob_work_queued[index] = false;
            if (completion.code != MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT) {
                runtime->mesh.queued = false;
                runtime->mesh.transfer_active = false;
                runtime->mesh.begin_ready = false;
                runtime->mesh.waiting_result = false;
                runtime->mesh.terminal_result_present = true;
                runtime->mesh.terminal_result =
                    MEISO_RUNTIME_BLOB_RESULT_RECEIVER_FAILURE;
            } else if (index == 0U &&
                       completion.generated_blob_transfer_present) {
                runtime->blob_transfer = completion.generated_blob_transfer;
                runtime->mesh.transfer_active = true;
                if (completion.generated_blob_transfer.transfer_id >
                    runtime->mesh.transfer_id_high_water) {
                    runtime->mesh.transfer_id_high_water =
                        completion.generated_blob_transfer.transfer_id;
                }
            } else if (index == 0U) {
                meiso_composition_host_runtime_private_enter_terminal(
                    runtime,
                    MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_INTERNAL_FAILURE,
                    MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
                );
            } else if (index == 1U) {
                runtime->mesh.next_offset += runtime->blob_chunk_queued_bytes;
                runtime->blob_chunk_queued_bytes = 0U;
            } else if (index == 2U) {
                runtime->mesh.waiting_result = true;
            }
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_MESH_PROGRESS;
            out_result->command_present = true;
            out_result->command_id = runtime->mesh.command_id;
            return true;
        }
    }
    return false;
}

static bool maybe_echo_presentation_token(MeisoCompositionHostRuntime *runtime)
{
    MeisoCompositionHostRuntimePrivateDesiredChannel *desired =
        &runtime->desired[MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_PRESENTATION];
    MeisoCompositionScenePresentationPlan plan;
    MeisoRuntimeDesiredState wrapper;
    MeisoByteView payload;
    size_t profile_size = 0U;
    uint64_t version;
    if (!desired->plan_present || desired->plan_echoed ||
        desired->inflight_present ||
        !runtime->presentation_status_present ||
        runtime->presentation_status.candidate_state !=
            MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED ||
        !runtime->presentation_status.prepared_token_present ||
        !runtime->actual[MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_PRESENTATION]
             .snapshot.observed_desired.present ||
        !meiso_composition_host_runtime_private_owner_equal(
            runtime->actual[MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_PRESENTATION]
                .snapshot.observed_desired.owner,
            desired->snapshot.owner
        ) || runtime->actual[MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_PRESENTATION]
                    .snapshot.observed_desired.version !=
                desired->snapshot.state_version ||
        runtime->presentation_status.candidate_plan_generation !=
            desired->plan.plan_generation ||
        memcmp(runtime->presentation_status.candidate_digest,
               desired->plan.candidate_digest,
               MEISO_SHA256_DIGEST_SIZE) != 0) {
        return true;
    }
    if (desired->snapshot.state_version == UINT64_MAX) {
        return false;
    }
    plan = desired->plan;
    plan.prepared_token_echo_present = true;
    memcpy(plan.prepared_token_echo,
           runtime->presentation_status.prepared_token,
           MEISO_SHA256_DIGEST_SIZE);
    if (meiso_composition_scene_presentation_plan_encode(
            &plan,
            (MeisoMutableBuffer){runtime->profile_scratch,
                                 sizeof(runtime->profile_scratch)},
            &profile_size
        ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return false;
    }
    wrapper.enabled = true;
    wrapper.profile = (MeisoByteView){runtime->profile_scratch, profile_size};
    if (!meiso_runtime_desired_state_encode(
            &wrapper,
            (MeisoMutableBuffer){runtime->desired_scratch,
                                 sizeof(runtime->desired_scratch)},
            &payload
        )) {
        return false;
    }
    version = desired->snapshot.state_version + 1U;
    if (meiso_state_publisher_publish(
            desired->publisher, desired->snapshot.owner, version, payload
        ) != MEISO_STATE_PUBLISH_NEW) {
        return false;
    }
    desired->plan = plan;
    desired->plan_echoed = true;
    desired->snapshot.state_version = version;
    desired->snapshot.delivery_pending = true;
    desired->snapshot.delivery_accepted = false;
    desired->snapshot.delivery_failed = false;
    return true;
}

MeisoCompositionHostRuntimePumpStatus meiso_composition_host_runtime_private_pump_local(
    MeisoCompositionHostRuntime *runtime,
    MeisoCompositionHostRuntimePumpResult *out_result
)
{
    size_t index;
    if (runtime->phase != MEISO_COMPOSITION_HOST_RUNTIME_PHASE_READY) {
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_IDLE;
    }
    if (!runtime->deliveries_started) {
        if (!start_desired_deliveries(runtime)) {
            meiso_composition_host_runtime_private_enter_terminal(
                runtime,
                MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_INTERNAL_FAILURE,
                MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
            );
            return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INTERNAL_FAILURE;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_BOUNDARY;
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS;
    }
    if (handle_source_completion(runtime, out_result)) {
        return runtime->phase == MEISO_COMPOSITION_HOST_RUNTIME_PHASE_TERMINAL ?
            MEISO_COMPOSITION_HOST_RUNTIME_PUMP_TERMINAL :
            MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS;
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_COUNT;
         ++index) {
        if (queue_actual_accept(runtime, index, out_result)) {
            return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS;
        }
    }
    if (!maybe_echo_presentation_token(runtime)) {
        meiso_composition_host_runtime_private_enter_terminal(
            runtime, MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_INTERNAL_FAILURE,
            MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
        );
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INTERNAL_FAILURE;
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT;
         ++index) {
        if (queue_desired_offer(runtime, index, out_result)) {
            return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS;
        }
    }
    if (queue_blob_work(runtime, out_result)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS;
    }
    return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_IDLE;
}

MeisoCompositionHostRuntimePumpStatus meiso_composition_host_runtime_private_pump_control(
    MeisoCompositionHostRuntime *runtime,
    const MeisoCompositionHostRuntimeIoReady *ready,
    MeisoCompositionHostRuntimePumpResult *out_result
)
{
    MeisoCompositionControlSessionPumpResult peer_result;
    MeisoCompositionControlSessionPumpStatus peer_status;
    MeisoCompositionHostRuntimePumpResult result;
    runtime->mutable_active = true;
    peer_status = meiso_composition_control_session_pump(
        runtime->control_session, ready->control_readable, ready->control_writable,
        &peer_result
    );
    runtime->mutable_active = false;
    meiso_composition_host_runtime_private_refresh_phase(runtime);
    if (runtime->phase == MEISO_COMPOSITION_HOST_RUNTIME_PHASE_TERMINAL ||
        peer_status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_TERMINAL) {
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_TERMINAL;
    }
    if (peer_status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_WOULD_BLOCK ||
        peer_status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_TX_BUSY) {
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_WOULD_BLOCK;
    }
    if (peer_status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_IDLE) {
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_IDLE;
    }
    if (peer_status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_EVENT) {
        if (!peer_result.event_present || !meiso_composition_host_runtime_private_handle_runtime_event(
                runtime, &peer_result.event, &result
            )) {
            meiso_composition_host_runtime_private_enter_terminal(
                runtime,
                MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_INTERNAL_FAILURE,
                MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
            );
            return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INTERNAL_FAILURE;
        }
        meiso_composition_host_runtime_private_refresh_phase(runtime);
        *out_result = result;
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS;
    }
    if (peer_status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_RX_OBLIGATION) {
        meiso_composition_host_runtime_private_enter_terminal(
            runtime, MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_INTERNAL_FAILURE,
            MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
        );
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INTERNAL_FAILURE;
    }
    if (peer_status > MEISO_COMPOSITION_CONTROL_SESSION_PUMP_SOURCE_INVALID) {
        meiso_composition_host_runtime_private_enter_terminal(
            runtime, MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_INTERNAL_FAILURE,
            MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION, EPROTO
        );
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INTERNAL_FAILURE;
    }
    memset(&result, 0, sizeof(result));
    result.event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_CONTROL_PROGRESS;
    result.event_present = true;
    *out_result = result;
    return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS;
}
