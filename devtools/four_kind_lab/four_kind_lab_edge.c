#include "meiso_four_kind_lab_private.h"

/* Edge transcript owns publication, action execution and blob delivery. */

static bool edge_send_registry(MeisoFourKindLabConnection *connection)
{
    MeisoLabMessage message;

    memset(&message, 0, sizeof(message));
    message.type = MEISO_LAB_MESSAGE_REGISTRY;
    message.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    message.body.registry.descriptor_count = MEISO_LAB_DESCRIPTOR_COUNT;
    return meiso_four_kind_lab_private_send_lab_message(
        connection, MEISO_LAB_SENDER_EDGE, &message);
}

static bool edge_receive_registry_accept(MeisoFourKindLabConnection *connection)
{
    MeisoLabMessage received;
    return meiso_four_kind_lab_private_receive_lab_message(
               connection, MEISO_LAB_SENDER_HOST, &received) &&
           received.type == MEISO_LAB_MESSAGE_REGISTRY_ACCEPT &&
           received.body.registry_accept.descriptor_count ==
               MEISO_LAB_DESCRIPTOR_COUNT;
}

static bool edge_send_state(MeisoFourKindLabConnection *connection,
                            MeisoFourKindLabEdgePrimitives *edge,
                            const MeisoLabEdgeState *state, uint64_t version)
{
    uint8_t payload[MEISO_LAB_EDGE_STATE_WIRE_SIZE];
    size_t payload_size = 0U;
    MeisoStateOffer offer;
    MeisoLabMessage message;

    if (meiso_lab_edge_state_encode(
            state, (MeisoMutableBuffer){payload, sizeof(payload)},
            &payload_size) != MEISO_LAB_CODEC_OK ||
        meiso_state_publisher_publish(edge->state, connection->edge_scope,
                                      version,
                                      (MeisoByteView){payload, payload_size}) !=
            MEISO_STATE_PUBLISH_NEW ||
        meiso_state_publisher_begin_delivery(edge->state,
                                             edge->state_delivery) !=
            MEISO_STATE_DELIVERY_STARTED ||
        !meiso_state_publisher_next_offer(edge->state, &offer)) {
        return false;
    }
    edge->state_owner = offer.state.owner;
    edge->state_version = offer.state.version;
    edge->state_delivery_active = true;
    memset(&message, 0, sizeof(message));
    message.type = MEISO_LAB_MESSAGE_STATE_ACTUAL;
    message.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    message.body.state_actual.owner = offer.state.owner;
    message.body.state_actual.version = offer.state.version;
    if (meiso_lab_edge_state_decode(offer.state.payload,
                                    &message.body.state_actual.value) !=
        MEISO_LAB_CODEC_OK) {
        return false;
    }
    return meiso_four_kind_lab_private_send_lab_message(
        connection, MEISO_LAB_SENDER_EDGE, &message);
}

static bool edge_receive_state_accept(MeisoFourKindLabConnection *connection,
                                      MeisoFourKindLabEdgePrimitives *edge)
{
    MeisoLabMessage received;

    if (!meiso_four_kind_lab_private_receive_lab_message(
            connection, MEISO_LAB_SENDER_HOST, &received) ||
        received.type != MEISO_LAB_MESSAGE_STATE_ACCEPT ||
        meiso_state_publisher_mark_delivery_accepted(
            edge->state, edge->state_delivery, received.body.state_accept.owner,
            received.body.state_accept.version) !=
            MEISO_STATE_ACCEPT_CONFIRMED ||
        meiso_state_publisher_end_delivery(edge->state, edge->state_delivery) !=
            MEISO_STATE_DELIVERY_ENDED) {
        return false;
    }
    edge->state_delivery_active = false;
    return true;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_edge_fail_active_state_delivery(
    MeisoFourKindLabEdgePrimitives *edge)
{
    if (edge == NULL || !edge->state_delivery_active) {
        return true;
    }
    if (meiso_state_publisher_mark_delivery_failed(
            edge->state, edge->state_delivery, edge->state_owner,
            edge->state_version) != MEISO_STATE_FAILURE_RETAINED ||
        meiso_state_publisher_end_delivery(edge->state, edge->state_delivery) !=
            MEISO_STATE_DELIVERY_CONVERGENCE_FAILED) {
        return false;
    }
    edge->state_delivery_active = false;
    return true;
}

static bool edge_send_stream(MeisoFourKindLabConnection *connection,
                             const MeisoLabTelemetrySample *sample,
                             uint64_t sequence)
{
    MeisoLabMessage message;
    uint64_t source_time;

    if (!meiso_four_kind_lab_private_monotonic_now_ns(&source_time)) {
        return false;
    }
    memset(&message, 0, sizeof(message));
    message.type = MEISO_LAB_MESSAGE_STREAM_METRICS;
    message.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    message.body.stream_metrics.producer = connection->edge_scope;
    message.body.stream_metrics.epoch = connection->stream_epoch;
    message.body.stream_metrics.sequence = sequence;
    message.body.stream_metrics.source_time =
        (MeisoTimePoint){connection->clock_epoch, source_time};
    message.body.stream_metrics.value = *sample;
    return meiso_four_kind_lab_private_send_lab_message(
        connection, MEISO_LAB_SENDER_EDGE, &message);
}

static bool edge_execute_action(MeisoFourKindLabConnection *connection,
                                MeisoFourKindLabEdgeProcess *process)
{
    const MeisoResourceDescriptor *stream_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_STREAM);
    const MeisoResourceDescriptor *action_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_ACTION);
    MeisoLabMessage received;
    MeisoLabMessage response;
    MeisoActionRequest request;
    MeisoActionRequest application_request;
    MeisoActionResult result;
    MeisoActionEntryView cached;
    MeisoActionIngestStatus ingest_status;
    uint8_t result_bytes[8];
    size_t cursor = 0U;
    uint64_t result_window_id;
    bool execute_side_effect = false;

    if (stream_descriptor == NULL || action_descriptor == NULL ||
        !meiso_four_kind_lab_private_receive_lab_message(
            connection, MEISO_LAB_SENDER_HOST, &received) ||
        received.type != MEISO_LAB_MESSAGE_ACTION_RESET) {
        return false;
    }
    memset(&request, 0, sizeof(request));
    request.id = received.body.action_reset.id;
    request.expected_executor = received.body.action_reset.executor;
    request.target = stream_descriptor->id;
    request.operation = action_descriptor->schema;
    request.retry_policy = MEISO_ACTION_RETRY_NONE;
    ingest_status =
        meiso_action_executor_ingest(process->action, &request, NULL);
    if (ingest_status == MEISO_ACTION_INGEST_NEW) {
        if (!meiso_action_executor_next_application_request(
                process->action, &cursor, &application_request) ||
            !meiso_action_id_equal(application_request.id, request.id) ||
            meiso_action_executor_accept(
                process->action, connection->edge_scope, request.id,
                (MeisoTimePoint){0U, 0U}) != MEISO_ACTION_EXECUTOR_ACCEPTED) {
            return false;
        }
        process->active_action_id = request.id;
        process->active_action_nonterminal = true;
        execute_side_effect = true;
    } else if (ingest_status == MEISO_ACTION_INGEST_REPLAY) {
        if (!meiso_action_executor_query(process->action, request.id,
                                         &cached) ||
            cached.state != MEISO_ACTION_STATE_SUCCEEDED ||
            cached.result.size != sizeof(result_bytes)) {
            return false;
        }
        result_window_id =
            meiso_four_kind_lab_private_read_u64_be(cached.result.data);
        if (result_window_id == 0U) {
            return false;
        }
    } else {
        return false;
    }
    memset(&response, 0, sizeof(response));
    response.type = MEISO_LAB_MESSAGE_ACTION_DELIVERY_ACCEPT;
    response.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    response.body.action_delivery_accept.id = request.id;
    response.body.action_delivery_accept.executor = connection->edge_scope;
    if (!meiso_four_kind_lab_private_send_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &response)) {
        return false;
    }
    memset(&response, 0, sizeof(response));
    response.type = MEISO_LAB_MESSAGE_ACTION_RESULT;
    response.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    response.body.action_result.id = request.id;
    response.body.action_result.executor = connection->edge_scope;
    response.body.action_result.state = MEISO_ACTION_STATE_ACCEPTED;
    if (!meiso_four_kind_lab_private_send_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &response)) {
        return false;
    }
    if (execute_side_effect) {
        if (!meiso_four_kind_lab_private_reset_telemetry_window(
                &process->telemetry)) {
            return false;
        }
        result_window_id = process->telemetry.window_id;
        meiso_four_kind_lab_private_write_u64_be(result_bytes,
                                                 result_window_id);
        result.id = request.id;
        result.executor = connection->edge_scope;
        result.state = MEISO_ACTION_STATE_SUCCEEDED;
        result.payload = (MeisoByteView){result_bytes, sizeof(result_bytes)};
        if (meiso_action_executor_record_result(process->action, &result) !=
            MEISO_ACTION_RECORD_RECORDED) {
            return false;
        }
        process->active_action_nonterminal = false;
    }
    memset(&response, 0, sizeof(response));
    response.type = MEISO_LAB_MESSAGE_ACTION_RESULT;
    response.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    response.body.action_result.id = request.id;
    response.body.action_result.executor = connection->edge_scope;
    response.body.action_result.state = MEISO_ACTION_STATE_SUCCEEDED;
    response.body.action_result.new_window_id = result_window_id;
    return meiso_four_kind_lab_private_send_lab_message(
        connection, MEISO_LAB_SENDER_EDGE, &response);
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_edge_mark_active_action_indeterminate(
    MeisoFourKindLabEdgeProcess *process)
{
    MeisoActionEntryView view;

    if (process == NULL || !process->active_action_nonterminal) {
        return true;
    }
    if (meiso_action_executor_mark_indeterminate(
            process->action, process->scope, process->active_action_id) !=
            MEISO_ACTION_INDETERMINATE_MARKED ||
        !meiso_action_executor_query(process->action, process->active_action_id,
                                     &view) ||
        view.state != MEISO_ACTION_STATE_INDETERMINATE) {
        return false;
    }
    process->active_action_nonterminal = false;
    return true;
}

static bool edge_send_blob(MeisoFourKindLabConnection *connection)
{
    MeisoLabMessage message;
    MeisoLabMessage received;
    size_t manifest_size = 0U;
    const uint8_t *manifest = meiso_lab_capability_manifest(&manifest_size);
    const MeisoContentId *content_id =
        meiso_lab_capability_manifest_content_id();

    if (manifest == NULL || content_id == NULL) {
        return false;
    }
    memset(&message, 0, sizeof(message));
    message.type = MEISO_LAB_MESSAGE_BLOB_MANIFEST;
    message.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    message.body.blob_manifest.content_id = *content_id;
    message.body.blob_manifest.content =
        (MeisoByteView){manifest, manifest_size};
    return meiso_four_kind_lab_private_send_lab_message(
               connection, MEISO_LAB_SENDER_EDGE, &message) &&
           meiso_four_kind_lab_private_receive_lab_message(
               connection, MEISO_LAB_SENDER_HOST, &received) &&
           received.type == MEISO_LAB_MESSAGE_BLOB_ACCEPT &&
           meiso_content_id_equal(&received.body.blob_accept.content_id,
                                  content_id);
}

static bool edge_receive_lab_close(MeisoFourKindLabConnection *connection)
{
    MeisoLabMessage received;
    return meiso_four_kind_lab_private_receive_lab_message(
               connection, MEISO_LAB_SENDER_HOST, &received) &&
           received.type == MEISO_LAB_MESSAGE_CLOSE &&
           received.body.close.status == MEISO_LAB_CLOSE_PASS &&
           meiso_four_kind_lab_private_edge_wait_peer_close(connection);
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_run_edge_transcript(
    int fd, MeisoFourKindLabEdgeProcess *process)
{
    MeisoFourKindLabConnection connection;
    MeisoFourKindLabEdgePrimitives edge;
    MeisoLabEdgeState state;
    MeisoLabTelemetrySample sample;
    bool passed = false;

    memset(&connection, 0, sizeof(connection));
    connection.fd = -1;
    memset(&edge, 0, sizeof(edge));
    if (!meiso_four_kind_lab_private_initialize_connection(
            &connection, fd, MEISO_SESSION_ROLE_EDGE, process)) {
        meiso_four_kind_lab_private_emit_connection_failed(&connection);
        return false;
    }
    if (!meiso_four_kind_lab_private_initialize_edge_primitives(&connection,
                                                                &edge)) {
        meiso_four_kind_lab_private_emit_connection_failed(&connection);
        return false;
    }
    if (!meiso_four_kind_lab_private_wait_session_ready(&connection)) {
        meiso_four_kind_lab_private_emit_connection_failed(&connection);
        return false;
    }
    meiso_four_kind_lab_private_emit_session_ready(&connection);
    if (!edge_send_registry(&connection) ||
        !edge_receive_registry_accept(&connection) ||
        process->state_version == UINT64_MAX) {
        goto finish;
    }
    process->state_version += 1U;
    if (!meiso_four_kind_lab_private_capture_telemetry(&process->telemetry,
                                                       &state, &sample) ||
        !edge_send_state(&connection, &edge, &state, process->state_version) ||
        !edge_receive_state_accept(&connection, &edge) ||
        process->stream_sequence == UINT64_MAX) {
        goto finish;
    }
    process->stream_sequence += 1U;
    if (!edge_send_stream(&connection, &sample, process->stream_sequence) ||
        !edge_execute_action(&connection, process) ||
        !edge_send_blob(&connection) || !edge_receive_lab_close(&connection)) {
        goto finish;
    }
    passed = true;

finish:
    if (!passed) {
        if (!meiso_four_kind_lab_private_edge_fail_active_state_delivery(
                &edge) ||
            !meiso_four_kind_lab_private_edge_mark_active_action_indeterminate(
                process) ||
            connection.local_failure ==
                MEISO_FOUR_KIND_LAB_LOCAL_FAILURE_NONE) {
            meiso_four_kind_lab_private_record_protocol_failure(&connection);
        }
        meiso_four_kind_lab_private_emit_transcript_failed(&connection);
        meiso_four_kind_lab_private_best_effort_protocol_close(&connection);
    } else {
        meiso_four_kind_lab_private_emit_session_closed(&connection);
    }
    return passed;
}
