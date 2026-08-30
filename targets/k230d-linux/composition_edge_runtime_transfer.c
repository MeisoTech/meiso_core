#define _POSIX_C_SOURCE 200809L

#include "composition_edge_runtime_private.h"

/* Control events, Blob transfer, completions, and source queues. */

static bool blob_route_equal(
    MeisoRuntimeRouteKey lhs,
    MeisoRuntimeRouteKey rhs
)
{
    return lhs.catalog_generation == rhs.catalog_generation &&
        meiso_endpoint_id_equal(lhs.endpoint_id, rhs.endpoint_id) &&
        lhs.endpoint_incarnation == rhs.endpoint_incarnation &&
        meiso_resource_id_equal(lhs.resource_id, rhs.resource_id) &&
        lhs.facet == rhs.facet;
}
static bool blob_transfer_equal(
    const MeisoRuntimeBlobTransferRef *lhs,
    const MeisoRuntimeBlobTransferRef *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        blob_route_equal(lhs->route, rhs->route) &&
        lhs->transfer_id == rhs->transfer_id &&
        meiso_content_id_equal(&lhs->content_id, &rhs->content_id) &&
        lhs->total_size == rhs->total_size;
}

static bool queue_blob_result(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoRuntimeBlobTransferRef *transfer,
    MeisoRuntimeBlobOperation operation,
    bool marks_ready,
    bool clears_active
)
{
    MeisoRuntimeSchedulerWork work;
    if (runtime == NULL || transfer == NULL || runtime->blob_result_pending ||
        runtime->blob_result_queued) {
        return false;
    }
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT;
    work.value.blob_result.transfer = *transfer;
    work.value.blob_result.operation = operation;
    if (meiso_composition_control_session_queue_work(
            runtime->control_session, SOURCE_BLOB_RESULT, &work
        ) != MEISO_COMPOSITION_CONTROL_SESSION_OK) {
        return false;
    }
    runtime->blob_result_transfer = *transfer;
    runtime->blob_result_operation = operation;
    runtime->blob_result_pending = true;
    runtime->blob_result_queued = true;
    runtime->blob_result_marks_ready = marks_ready;
    runtime->blob_result_clears_active = clears_active;
    return true;
}

static bool handle_blob_request(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoRuntimeEvent *event,
    MeisoTimePoint now,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
)
{
    MeisoRuntimeBlobPublishReport report;
    uint64_t previous_high_water;
    uint64_t chunk_end;
    bool prepare_attempted;
    bool active_plan;
    bool begin_accepted = false;
    bool result_required = false;
    bool result_clears_active = false;
    bool core_published = false;
    MeisoCompositionScenePrepareStatus prepare;
    previous_high_water = runtime->blob_transfer_id_high_water;
    if (event->blob_transfer.transfer_id >
        runtime->blob_transfer_id_high_water) {
        runtime->blob_transfer_id_high_water = event->blob_transfer.transfer_id;
    }
    switch (event->blob_operation) {
    case MEISO_RUNTIME_BLOB_BEGIN:
        begin_accepted = event->blob_transfer.transfer_id >
                previous_high_water &&
            meiso_content_id_valid(&event->blob_transfer.content_id) &&
            !runtime->blob_receive_active &&
            event->blob_transfer.total_size <=
                MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE;
        if (begin_accepted) {
            runtime->blob_receive_transfer = event->blob_transfer;
            runtime->blob_receive_next_offset = 0U;
            runtime->blob_receive_active = true;
            runtime->blob_receive_begin_ready = false;
        }
        result_required = true;
        break;
    case MEISO_RUNTIME_BLOB_CHUNK:
        if (!runtime->blob_receive_active ||
            !blob_transfer_equal(
                &event->blob_transfer, &runtime->blob_receive_transfer
            ) || !runtime->blob_receive_begin_ready) {
            return false;
        }
        if (event->blob_offset != runtime->blob_receive_next_offset ||
            event->blob_payload.size > UINT64_MAX - event->blob_offset) {
            result_required = true;
            result_clears_active = true;
            break;
        }
        chunk_end = event->blob_offset + event->blob_payload.size;
        if (chunk_end > runtime->blob_receive_transfer.total_size ||
            (event->blob_payload.size <
                    MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE &&
             chunk_end != runtime->blob_receive_transfer.total_size)) {
            result_required = true;
            result_clears_active = true;
        } else {
            runtime->blob_receive_next_offset = chunk_end;
        }
        break;
    case MEISO_RUNTIME_BLOB_COMMIT:
        if (!runtime->blob_receive_active ||
            !blob_transfer_equal(
                &event->blob_transfer, &runtime->blob_receive_transfer
            ) || !runtime->blob_receive_begin_ready) {
            return false;
        }
        result_required = true;
        result_clears_active = true;
        if (runtime->blob_receive_next_offset ==
                event->blob_transfer.total_size &&
            meiso_runtime_blob_sha256_verify(
                &event->blob_transfer.content_id,
                event->blob_payload, NULL
            )) {
            memset(&report, 0, sizeof(report));
            if (meiso_composition_control_session_publish_blob_receive(
                    runtime->control_session, &event->blob_transfer, runtime->blob_store,
                    meiso_composition_scene_mesh_blob_verify, NULL, &report
                ) != MEISO_COMPOSITION_CONTROL_SESSION_OK) {
                return false;
            }
            if (report.result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED) {
                if (runtime->blob_published_bytes >
                    UINT64_MAX - event->blob_transfer.total_size) {
                    return false;
                }
                runtime->blob_published_bytes +=
                    event->blob_transfer.total_size;
            }
            core_published =
                report.result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED ||
                report.result == MEISO_RUNTIME_BLOB_RESULT_DUPLICATE;
            if (core_published &&
                (!meiso_k230d_composition_edge_runtime_private_remember_visible_blob(runtime, &event->blob_transfer) ||
                 !meiso_k230d_composition_edge_runtime_private_apply_visible_blobs(runtime) ||
                 !meiso_k230d_composition_edge_runtime_private_prepare_stored_scene(
                     runtime, &prepare_attempted, &active_plan, &prepare
                 ) || !meiso_k230d_composition_edge_runtime_private_try_prepare_presentation(runtime, now) ||
                 !meiso_k230d_composition_edge_runtime_private_refresh_composition_actuals(runtime))) {
                return false;
            }
        }
        break;
    case MEISO_RUNTIME_BLOB_ABORT:
        if (!runtime->blob_receive_active ||
            !blob_transfer_equal(
                &event->blob_transfer, &runtime->blob_receive_transfer
            )) {
            return false;
        }
        result_required = true;
        result_clears_active = true;
        break;
    default:
        return false;
    }
    if (result_required && !queue_blob_result(
            runtime, &event->blob_transfer, event->blob_operation,
            begin_accepted, result_clears_active
        )) {
        return false;
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = core_published ?
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_BLOB_PUBLISHED :
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_CONTROL_PROGRESS;
    return true;
}

bool meiso_k230d_composition_edge_runtime_private_handle_runtime_event(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoRuntimeEvent *event,
    MeisoTimePoint now,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
)
{
    switch (event->type) {
    case MEISO_RUNTIME_EVENT_STATE_OFFER:
        return meiso_k230d_composition_edge_runtime_private_handle_desired_offer(runtime, event, now, out_result);
    case MEISO_RUNTIME_EVENT_STATE_ACCEPT:
        return meiso_k230d_composition_edge_runtime_private_handle_actual_accept(runtime, event, out_result);
    case MEISO_RUNTIME_EVENT_BLOB_REQUEST:
        return handle_blob_request(runtime, event, now, out_result);
    case MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED:
        if (event->catalog_generation != runtime->config.catalog_generation) {
            return false;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_CONTROL_PROGRESS;
        return true;
    case MEISO_RUNTIME_EVENT_CATALOG_OFFERED:
    case MEISO_RUNTIME_EVENT_CATALOG_REJECTED:
    case MEISO_RUNTIME_EVENT_STREAM_SAMPLE:
    case MEISO_RUNTIME_EVENT_BLOB_RESULT:
    default:
        return false;
    }
}

bool meiso_k230d_composition_edge_runtime_private_handle_source_completion(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
)
{
    MeisoRuntimeSchedulerCompletion completion;
    size_t index;
    if (meiso_composition_control_session_take_completion(
            runtime->control_session, SOURCE_BLOB_RESULT, &completion
        ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
        runtime->blob_result_queued = false;
        if (!runtime->blob_result_pending || completion.code !=
                MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT) {
            meiso_k230d_composition_edge_runtime_private_enter_terminal(
                runtime,
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_INTERNAL_FAILURE,
                (int)completion.runtime_status
            );
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event =
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_EVENT_TERMINAL;
            return true;
        }
        if (runtime->blob_result_marks_ready && runtime->blob_receive_active &&
            blob_transfer_equal(
                &runtime->blob_result_transfer, &runtime->blob_receive_transfer
            )) {
            runtime->blob_receive_begin_ready = true;
        }
        if (runtime->blob_result_clears_active && runtime->blob_receive_active &&
            blob_transfer_equal(
                &runtime->blob_result_transfer, &runtime->blob_receive_transfer
            )) {
            memset(
                &runtime->blob_receive_transfer, 0,
                sizeof(runtime->blob_receive_transfer)
            );
            runtime->blob_receive_next_offset = 0U;
            runtime->blob_receive_active = false;
            runtime->blob_receive_begin_ready = false;
        }
        memset(
            &runtime->blob_result_transfer, 0,
            sizeof(runtime->blob_result_transfer)
        );
        runtime->blob_result_operation = 0U;
        runtime->blob_result_pending = false;
        runtime->blob_result_marks_ready = false;
        runtime->blob_result_clears_active = false;
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event =
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_CONTROL_PROGRESS;
        return true;
    }
    for (index = 0U; index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_COUNT;
         ++index) {
        if (meiso_composition_control_session_take_completion(
                runtime->control_session, meiso_k230d_composition_edge_runtime_private_desired_source_index[index], &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            runtime->desired[index].accept_queued = false;
            if (completion.code !=
                MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT) {
                meiso_k230d_composition_edge_runtime_private_enter_terminal(
                    runtime,
                    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_INTERNAL_FAILURE,
                    (int)completion.runtime_status
                );
                memset(out_result, 0, sizeof(*out_result));
                out_result->event_present = true;
                out_result->event =
                    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_EVENT_TERMINAL;
                return true;
            }
            runtime->desired[index].accept_pending = false;
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event =
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_CONTROL_PROGRESS;
            out_result->desired_channel_present = true;
            out_result->desired_channel = (uint8_t)index;
            return true;
        }
    }
    for (index = 0U; index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT;
         ++index) {
        if (meiso_composition_control_session_take_completion(
                runtime->control_session, meiso_k230d_composition_edge_runtime_private_actual_source_index[index], &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            runtime->actual[index].offer_queued = false;
            if (completion.code !=
                MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT) {
                runtime->actual[index].snapshot.delivery_failed = true;
                meiso_k230d_composition_edge_runtime_private_enter_terminal(
                    runtime,
                    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_INTERNAL_FAILURE,
                    (int)completion.runtime_status
                );
                memset(out_result, 0, sizeof(*out_result));
                out_result->event_present = true;
                out_result->event =
                    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_EVENT_TERMINAL;
                return true;
            }
            runtime->actual[index].offer_sent = true;
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event =
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_ACTUAL_DELIVERY;
            out_result->actual_channel_present = true;
            out_result->actual_channel = (uint8_t)index;
            return true;
        }
    }
    for (index = 0U;
         index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT; ++index) {
        if (meiso_composition_control_session_take_completion(
                runtime->control_session, meiso_k230d_composition_edge_runtime_private_inline_source_index[index], &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            runtime->inline_streams[index].sample_queued = false;
            runtime->inline_streams[index].sample_sent = false;
            if (!meiso_stream_pop(runtime->inline_streams[index].stream)) {
                return false;
            }
            runtime->inline_streams[index].snapshot.queue_depth =
                (uint32_t)meiso_stream_count(
                    runtime->inline_streams[index].stream
                );
            if (completion.code ==
                MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT) {
                ++runtime->inline_streams[index].snapshot.samples_accepted;
                if (index ==
                        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_RENDER_RESULT &&
                    runtime->render_complete_present &&
                    runtime->inline_streams[index].pending_sequence != 0U) {
                    if (!meiso_composition_scene_store_record_result_published(
                            runtime->scene_store,
                            &runtime->render_complete.job.world,
                            runtime->inline_streams[index].snapshot.stream_epoch,
                            runtime->inline_streams[index].pending_sequence
                        )) {
                        return false;
                    }
                    runtime->render_inline_accepted = true;
                }
            } else {
                ++runtime->inline_streams[index].snapshot.samples_rejected;
                if (index ==
                    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_RENDER_RESULT) {
                    runtime->render_inline_accepted = true;
                }
            }
            runtime->inline_streams[index].pending_sequence = 0U;
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INLINE_SAMPLE;
            out_result->inline_channel_present = true;
            out_result->inline_channel = (uint8_t)index;
            return true;
        }
    }
    return false;
}

bool meiso_k230d_composition_edge_runtime_private_edge_runtime_source_work_outstanding(
    const MeisoK230dCompositionEdgeRuntime *runtime
)
{
    size_t index;
    if (runtime == NULL) {
        return false;
    }
    if (runtime->blob_result_pending || runtime->blob_result_queued) {
        return true;
    }
    for (index = 0U; index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_COUNT;
         ++index) {
        if (runtime->desired[index].accept_queued ||
            runtime->actual[index].offer_queued) {
            return true;
        }
    }
    for (index = 0U;
         index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT; ++index) {
        if (runtime->inline_streams[index].sample_queued) {
            return true;
        }
    }
    return false;
}

bool meiso_k230d_composition_edge_runtime_private_queue_desired_accept(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
)
{
    EdgeDesiredChannel *channel = &runtime->desired[index];
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeRouteKey route;
    if (!channel->accept_pending || channel->accept_queued ||
        !meiso_composition_control_session_route_key(
            runtime->control_session, meiso_k230d_composition_edge_runtime_private_desired_route_index[index], &route
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
            runtime->control_session, meiso_k230d_composition_edge_runtime_private_desired_source_index[index], &work
        ) != MEISO_COMPOSITION_CONTROL_SESSION_OK) {
        return false;
    }
    channel->accept_queued = true;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_CONTROL_PROGRESS;
    out_result->desired_channel_present = true;
    out_result->desired_channel = (uint8_t)index;
    return true;
}

bool meiso_k230d_composition_edge_runtime_private_queue_actual_offer(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
)
{
    EdgeActualChannel *channel = &runtime->actual[index];
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeRouteKey route;
    MeisoStateOffer offer;
    if (channel->offer_queued || channel->offer_sent ||
        !meiso_state_publisher_next_offer(channel->publisher, &offer) ||
        !meiso_composition_control_session_route_key(
            runtime->control_session, meiso_k230d_composition_edge_runtime_private_actual_route_index[index], &route
        )) {
        return false;
    }
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
    work.value.state_offer.route = route;
    work.value.state_offer.offer = offer;
    if (meiso_composition_control_session_queue_work(
            runtime->control_session, meiso_k230d_composition_edge_runtime_private_actual_source_index[index], &work
        ) != MEISO_COMPOSITION_CONTROL_SESSION_OK) {
        return false;
    }
    channel->offer_queued = true;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_ACTUAL_DELIVERY;
    out_result->actual_channel_present = true;
    out_result->actual_channel = (uint8_t)index;
    return true;
}

bool meiso_k230d_composition_edge_runtime_private_queue_inline_sample(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
)
{
    EdgeInlineChannel *channel = &runtime->inline_streams[index];
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeRouteKey route;
    MeisoStreamSample sample;
    if (channel->sample_queued || channel->sample_sent ||
        !meiso_stream_peek(channel->stream, &sample) ||
        !meiso_composition_control_session_route_key(
            runtime->control_session, meiso_k230d_composition_edge_runtime_private_inline_route_index[index], &route
        )) {
        return false;
    }
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE;
    work.value.stream_sample.route = route;
    work.value.stream_sample.production_epoch = sample.key.epoch;
    work.value.stream_sample.sample = sample;
    if (meiso_composition_control_session_queue_work(
            runtime->control_session, meiso_k230d_composition_edge_runtime_private_inline_source_index[index], &work
        ) != MEISO_COMPOSITION_CONTROL_SESSION_OK) {
        return false;
    }
    channel->sample_queued = true;
    channel->sample_sent = true;
    memset(out_result, 0, sizeof(*out_result));
    out_result->event_present = true;
    out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INLINE_SAMPLE;
    out_result->inline_channel_present = true;
    out_result->inline_channel = (uint8_t)index;
    return true;
}

bool meiso_k230d_composition_edge_runtime_private_enqueue_inline_payload(
    MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index,
    MeisoTimePoint source_time,
    MeisoByteView payload
)
{
    EdgeInlineChannel *channel;
    MeisoStreamSample sample;
    MeisoStreamReceiveResult receive;
    uint64_t sequence;
    if (index >= MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_STREAM_COUNT ||
        !meiso_time_point_valid(source_time) ||
        source_time.clock_epoch != runtime->config.edge_clock_epoch ||
        payload.data == NULL || payload.size == 0U) {
        return false;
    }
    channel = &runtime->inline_streams[index];
    if (!channel->snapshot.enabled ||
        channel->snapshot.next_sequence == 0U ||
        channel->snapshot.next_sequence == UINT64_MAX) {
        return false;
    }
    sequence = channel->snapshot.next_sequence;
    memset(&sample, 0, sizeof(sample));
    sample.key.producer = meiso_k230d_composition_edge_runtime_private_edge_owner(&runtime->config);
    sample.key.epoch = channel->snapshot.stream_epoch;
    sample.key.sequence = sequence;
    sample.source_time = source_time;
    sample.payload = payload;
    receive = meiso_stream_receive(channel->stream, &sample, NULL);
    if (receive.code != MEISO_STREAM_RECEIVE_ACCEPTED) {
        return false;
    }
    channel->pending_sequence = sequence;
    channel->snapshot.next_sequence = sequence + 1U;
    ++channel->snapshot.samples_materialized;
    channel->snapshot.queue_depth = (uint32_t)meiso_stream_count(
        channel->stream
    );
    return true;
}

bool meiso_k230d_composition_edge_runtime_private_actual_delivery_current(
    const MeisoK230dCompositionEdgeRuntime *runtime,
    size_t index
)
{
    return runtime != NULL && index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_COUNT &&
        runtime->actual[index].snapshot.delivery_accepted &&
        !runtime->actual[index].snapshot.delivery_pending &&
        !runtime->actual[index].snapshot.delivery_failed;
}
