#include "meiso_four_kind_lab_private.h"

/* Host transcript owns Core receiver primitives and ordered lab consumption. */

static bool initialize_host_primitives(MeisoFourKindLabConnection *connection,
                                       MeisoFourKindLabHostPrimitives *host)
{
    const MeisoResourceDescriptor *descriptors;
    const MeisoResourceDescriptor *state_descriptor;
    const MeisoResourceDescriptor *stream_descriptor;
    const MeisoResourceDescriptor *action_descriptor;
    const MeisoResourceDescriptor *blob_descriptor;
    const MeisoStreamConfig stream_config = {
        MEISO_STREAM_ORDER_STRICT_MONOTONIC,
        MEISO_STREAM_OVERFLOW_REJECT_NEWEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        1U,
        0U,
        0U};
    const MeisoActionConfig action_config = {1U, 0U, 8U};
    const MeisoBlobConfig blob_config = {1U};
    size_t descriptor_count = 0U;
    size_t required;
    size_t index;

    if (connection == NULL || host == NULL) {
        return false;
    }
    memset(host, 0, sizeof(*host));
    descriptors = meiso_lab_descriptors(&descriptor_count);
    state_descriptor = meiso_lab_descriptor(MEISO_RESOURCE_KIND_STATE);
    stream_descriptor = meiso_lab_descriptor(MEISO_RESOURCE_KIND_STREAM);
    action_descriptor = meiso_lab_descriptor(MEISO_RESOURCE_KIND_ACTION);
    blob_descriptor = meiso_lab_descriptor(MEISO_RESOURCE_KIND_BLOB);
    if (descriptors == NULL || descriptor_count != MEISO_LAB_DESCRIPTOR_COUNT ||
        state_descriptor == NULL || stream_descriptor == NULL ||
        action_descriptor == NULL || blob_descriptor == NULL) {
        return false;
    }

    required = meiso_descriptor_set_memory_required(descriptor_count);
    if (required == 0U ||
        required > sizeof(connection->storage->descriptors.bytes) ||
        meiso_descriptor_set_init(
            connection->storage->descriptors.bytes,
            sizeof(connection->storage->descriptors.bytes), descriptor_count,
            &host->descriptors) != MEISO_DESCRIPTOR_SET_INIT_OK) {
        return false;
    }
    for (index = 0U; index < descriptor_count; ++index) {
        if (meiso_descriptor_set_register(host->descriptors,
                                          &descriptors[index]) !=
            MEISO_DESCRIPTOR_SET_REGISTERED) {
            return false;
        }
    }
    if (meiso_descriptor_set_seal(host->descriptors) !=
        MEISO_DESCRIPTOR_SET_SEALED) {
        return false;
    }

    required = meiso_state_replica_memory_required(state_descriptor);
    if (required == 0U || required > sizeof(connection->storage->state.bytes) ||
        meiso_state_replica_init(connection->storage->state.bytes,
                                 sizeof(connection->storage->state.bytes),
                                 state_descriptor,
                                 &host->state) != MEISO_STATE_INIT_OK) {
        return false;
    }
    required = meiso_stream_memory_required(stream_descriptor, &stream_config);
    if (required == 0U ||
        required > sizeof(connection->storage->stream.bytes) ||
        meiso_stream_init(connection->storage->stream.bytes,
                          sizeof(connection->storage->stream.bytes),
                          stream_descriptor, &stream_config,
                          &host->stream) != MEISO_STREAM_INIT_OK) {
        return false;
    }
    required =
        meiso_action_caller_memory_required(action_descriptor, &action_config);
    if (required == 0U ||
        required > sizeof(connection->storage->action.bytes) ||
        meiso_action_caller_init(connection->storage->action.bytes,
                                 sizeof(connection->storage->action.bytes),
                                 action_descriptor, &action_config,
                                 &host->action) != MEISO_ACTION_INIT_OK) {
        return false;
    }
    required = meiso_blob_store_memory_required(blob_descriptor, &blob_config);
    if (required == 0U || required > sizeof(connection->storage->blob.bytes) ||
        meiso_blob_store_init(connection->storage->blob.bytes,
                              sizeof(connection->storage->blob.bytes),
                              blob_descriptor, &blob_config,
                              &host->blob) != MEISO_BLOB_INIT_OK) {
        return false;
    }
    host->action_delivery.value = 1U;
    return meiso_four_kind_lab_private_random_action_id(&host->action_id) &&
           meiso_action_caller_begin_delivery(host->action,
                                              host->action_delivery) ==
               MEISO_ACTION_DELIVERY_STARTED;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_initialize_edge_primitives(
    MeisoFourKindLabConnection *connection,
    MeisoFourKindLabEdgePrimitives *edge)
{
    const MeisoResourceDescriptor *state_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_STATE);
    const MeisoStateConfig state_config = {MEISO_STATE_DELIVERY_CONVERGENT};
    size_t required;

    if (connection == NULL || edge == NULL || state_descriptor == NULL) {
        return false;
    }
    memset(edge, 0, sizeof(*edge));
    edge->state_delivery.value = 1U;
    required =
        meiso_state_publisher_memory_required(state_descriptor, &state_config);
    if (required == 0U || required > sizeof(connection->storage->state.bytes) ||
        meiso_state_publisher_init(connection->storage->state.bytes,
                                   sizeof(connection->storage->state.bytes),
                                   state_descriptor, &state_config,
                                   &edge->state) != MEISO_STATE_INIT_OK) {
        return false;
    }
    return true;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_initialize_edge_process_with_thermal_mode(
    MeisoFourKindLabEdgeProcess *process,
    MeisoFourKindLabThermalChildMode thermal_mode)
{
    const MeisoResourceDescriptor *action_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_ACTION);
    const MeisoActionConfig action_config = {MEISO_FOUR_KIND_LAB_MAX_SESSIONS,
                                             0U, 8U};
    size_t required;

    if (process == NULL || action_descriptor == NULL) {
        return false;
    }
    memset(process, 0, sizeof(*process));
    process->thermal_child.pid = (pid_t)-1;
    process->scope.role = MEISO_OWNER_ROLE_EDGE;
    if (!meiso_four_kind_lab_private_random_nonzero_u64(
            &process->scope.incarnation) ||
        !meiso_four_kind_lab_private_random_nonzero_u64(
            &process->stream_epoch) ||
        !meiso_four_kind_lab_private_random_nonzero_u64(
            &process->clock_epoch) ||
        !meiso_four_kind_lab_private_initialize_telemetry(
            &process->telemetry, &process->thermal_child, thermal_mode)) {
        meiso_four_kind_lab_private_thermal_child_terminate_nonblocking(
            &process->thermal_child);
        return false;
    }
    required = meiso_action_executor_memory_required(action_descriptor,
                                                     &action_config);
    if (required == 0U || required > sizeof(process->action_arena.bytes) ||
        meiso_action_executor_init(
            process->action_arena.bytes, sizeof(process->action_arena.bytes),
            action_descriptor, &action_config, process->scope,
            &process->action) != MEISO_ACTION_INIT_OK) {
        meiso_four_kind_lab_private_thermal_child_terminate_nonblocking(
            &process->thermal_child);
        return false;
    }
    return true;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_initialize_edge_process(
    MeisoFourKindLabEdgeProcess *process)
{
    return meiso_four_kind_lab_private_initialize_edge_process_with_thermal_mode(
        process, MEISO_FOUR_KIND_THERMAL_CHILD_ACTUAL);
}

#ifdef MEISO_LAB_INTERNAL_TEST
static bool
self_test_observe_ready(const MeisoFourKindLabConnection *connection)
{
    MeisoSessionSnapshot snapshot;
    size_t index;

    if (!meiso_four_kind_lab_private_self_test.enabled) {
        return true;
    }
    if (connection == NULL || connection->session == NULL ||
        meiso_four_kind_lab_private_self_test.session_count >=
            MEISO_FOUR_KIND_LAB_SELF_TEST_SESSIONS ||
        !meiso_session_snapshot(connection->session, &snapshot) ||
        snapshot.state != MEISO_SESSION_STATE_READY ||
        !snapshot.session_id_known) {
        return false;
    }
    index = meiso_four_kind_lab_private_self_test.session_count;
    meiso_four_kind_lab_private_self_test.session_ids[index] =
        snapshot.session_id;
    meiso_four_kind_lab_private_self_test.session_count += 1U;
    return true;
}

static bool self_test_observation_index(size_t *out_index)
{
    if (!meiso_four_kind_lab_private_self_test.enabled) {
        return true;
    }
    if (out_index == NULL ||
        meiso_four_kind_lab_private_self_test.session_count == 0U ||
        meiso_four_kind_lab_private_self_test.session_count >
            MEISO_FOUR_KIND_LAB_SELF_TEST_SESSIONS) {
        return false;
    }
    *out_index = meiso_four_kind_lab_private_self_test.session_count - 1U;
    return true;
}
#endif

static bool host_receive_registry(MeisoFourKindLabConnection *connection,
                                  MeisoFourKindLabHostPrimitives *host)
{
    MeisoLabMessage received;
    MeisoLabMessage response;

    if (!meiso_four_kind_lab_private_receive_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &received) ||
        received.type != MEISO_LAB_MESSAGE_REGISTRY ||
        received.body.registry.descriptor_count != MEISO_LAB_DESCRIPTOR_COUNT ||
        !initialize_host_primitives(connection, host)) {
        return false;
    }
    meiso_four_kind_lab_private_emit_registry();
    memset(&response, 0, sizeof(response));
    response.type = MEISO_LAB_MESSAGE_REGISTRY_ACCEPT;
    response.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    response.body.registry_accept.descriptor_count = MEISO_LAB_DESCRIPTOR_COUNT;
    return meiso_four_kind_lab_private_send_lab_message(
        connection, MEISO_LAB_SENDER_HOST, &response);
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_host_runtimely_state_actual(
    MeisoFourKindLabConnection *connection,
    MeisoFourKindLabHostPrimitives *host, const MeisoLabMessage *received,
    MeisoStateView *out_current)
{
    uint8_t payload[MEISO_LAB_EDGE_STATE_WIRE_SIZE];
    size_t payload_size = 0U;

    if (connection == NULL || host == NULL || received == NULL ||
        out_current == NULL ||
        !meiso_owner_scope_equal(received->body.state_actual.owner,
                                 connection->edge_scope) ||
        meiso_lab_edge_state_encode(
            &received->body.state_actual.value,
            (MeisoMutableBuffer){payload, sizeof(payload)},
            &payload_size) != MEISO_LAB_CODEC_OK ||
        meiso_state_replica_begin_owner(host->state,
                                        received->body.state_actual.owner) !=
            MEISO_STATE_OWNER_STARTED ||
        meiso_state_replica_receive(host->state,
                                    received->body.state_actual.owner,
                                    received->body.state_actual.version,
                                    (MeisoByteView){payload, payload_size}) !=
            MEISO_STATE_RECEIVE_NEW ||
        !meiso_state_replica_current(host->state, out_current) ||
        out_current->version != received->body.state_actual.version ||
        !meiso_four_kind_lab_private_bytes_equal(
            out_current->payload, (MeisoByteView){payload, payload_size})) {
        return false;
    }
    host->edge_state = received->body.state_actual.value;
    host->edge_owner = received->body.state_actual.owner;
    host->edge_state_known = true;
#ifdef MEISO_LAB_INTERNAL_TEST
    if (meiso_four_kind_lab_private_self_test.enabled) {
        size_t index;
        if (!self_test_observation_index(&index)) {
            return false;
        }
        meiso_four_kind_lab_private_self_test.owners[index] = host->edge_owner;
        meiso_four_kind_lab_private_self_test.state_versions[index] =
            received->body.state_actual.version;
    }
#endif
    return true;
}

static bool host_receive_state(MeisoFourKindLabConnection *connection,
                               MeisoFourKindLabHostPrimitives *host)
{
    MeisoStateView current;
    MeisoLabMessage received;
    MeisoLabMessage response;

    if (!meiso_four_kind_lab_private_receive_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &received) ||
        received.type != MEISO_LAB_MESSAGE_STATE_ACTUAL ||
        !meiso_four_kind_lab_private_host_runtimely_state_actual(
            connection, host, &received, &current)) {
        return false;
    }
    meiso_four_kind_lab_private_emit_state_event(current.version,
                                                 &host->edge_state);
    memset(&response, 0, sizeof(response));
    response.type = MEISO_LAB_MESSAGE_STATE_ACCEPT;
    response.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    response.body.state_accept.owner = received.body.state_actual.owner;
    response.body.state_accept.version = received.body.state_actual.version;
    return meiso_four_kind_lab_private_send_lab_message(
        connection, MEISO_LAB_SENDER_HOST, &response);
}

static bool host_receive_stream(MeisoFourKindLabConnection *connection,
                                MeisoFourKindLabHostPrimitives *host,
                                MeisoLabStreamMetrics *out_metrics)
{
    uint8_t payload[MEISO_LAB_TELEMETRY_SAMPLE_WIRE_SIZE];
    size_t payload_size = 0U;
    MeisoStreamSample sample;
    MeisoStreamSample queued;
    MeisoStreamReceiveResult result;
    MeisoLabMessage received;
    uint64_t receive_time_ns;

    if (out_metrics == NULL || !host->edge_state_known ||
        !meiso_four_kind_lab_private_receive_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &received) ||
        received.type != MEISO_LAB_MESSAGE_STREAM_METRICS ||
        !meiso_four_kind_lab_private_monotonic_now_ns(&receive_time_ns)) {
        return false;
    }
    if (meiso_lab_telemetry_sample_encode(
            &received.body.stream_metrics.value,
            (MeisoMutableBuffer){payload, sizeof(payload)},
            &payload_size) != MEISO_LAB_CODEC_OK ||
        !meiso_owner_scope_equal(received.body.stream_metrics.producer,
                                 host->edge_owner) ||
        meiso_stream_begin_producer(host->stream,
                                    received.body.stream_metrics.producer,
                                    received.body.stream_metrics.epoch) !=
            MEISO_STREAM_PRODUCER_STARTED) {
        return false;
    }
    memset(&sample, 0, sizeof(sample));
    sample.key = (MeisoStreamKey){received.body.stream_metrics.producer,
                                  received.body.stream_metrics.epoch,
                                  received.body.stream_metrics.sequence};
    sample.source_time = received.body.stream_metrics.source_time;
    sample.payload = (MeisoByteView){payload, payload_size};
    result = meiso_stream_receive(host->stream, &sample, NULL);
    if (result.code != MEISO_STREAM_RECEIVE_ACCEPTED ||
        received.body.stream_metrics.sequence == 0U ||
        result.missing_before != received.body.stream_metrics.sequence - 1U ||
        result.missing_before >= MEISO_FOUR_KIND_LAB_MAX_SESSIONS ||
        meiso_stream_count(host->stream) != 1U ||
        !meiso_stream_peek(host->stream, &queued) ||
        !meiso_four_kind_lab_private_bytes_equal(queued.payload,
                                                 sample.payload)) {
        return false;
    }
    *out_metrics = received.body.stream_metrics;
#ifdef MEISO_LAB_INTERNAL_TEST
    if (meiso_four_kind_lab_private_self_test.enabled) {
        size_t index;
        if (!self_test_observation_index(&index)) {
            return false;
        }
        meiso_four_kind_lab_private_self_test.stream_epochs[index] =
            out_metrics->epoch;
        meiso_four_kind_lab_private_self_test.stream_sequences[index] =
            out_metrics->sequence;
        meiso_four_kind_lab_private_self_test.clock_epochs[index] =
            out_metrics->source_time.clock_epoch;
    }
#endif
    meiso_four_kind_lab_private_emit_stream_event(out_metrics, receive_time_ns);
    return true;
}

static bool host_send_action(MeisoFourKindLabConnection *connection,
                             MeisoFourKindLabHostPrimitives *host)
{
    const MeisoResourceDescriptor *stream_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_STREAM);
    const MeisoResourceDescriptor *action_descriptor =
        meiso_lab_descriptor(MEISO_RESOURCE_KIND_ACTION);
    MeisoActionRequest request;
    MeisoActionOffer offer;
    MeisoLabMessage message;
    size_t cursor = 0U;

    if (stream_descriptor == NULL || action_descriptor == NULL) {
        return false;
    }
    memset(&request, 0, sizeof(request));
    request.id = host->action_id;
    request.expected_executor = connection->edge_scope;
    request.target = stream_descriptor->id;
    request.operation = action_descriptor->schema;
    request.retry_policy = MEISO_ACTION_RETRY_NONE;
    if (meiso_action_caller_submit(host->action, &request) !=
            MEISO_ACTION_SUBMIT_NEW ||
        !meiso_action_caller_next_offer(host->action, host->action_delivery,
                                        &cursor, &offer) ||
        !meiso_action_id_equal(offer.request.id, request.id)) {
        return false;
    }
    memset(&message, 0, sizeof(message));
    message.type = MEISO_LAB_MESSAGE_ACTION_RESET;
    message.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    message.body.action_reset.id = offer.request.id;
    message.body.action_reset.executor = offer.request.expected_executor;
    return meiso_four_kind_lab_private_send_lab_message(
        connection, MEISO_LAB_SENDER_HOST, &message);
}

static bool host_receive_action(MeisoFourKindLabConnection *connection,
                                MeisoFourKindLabHostPrimitives *host,
                                uint64_t *out_window_id)
{
    MeisoLabMessage received;
    MeisoActionResult result;
    MeisoActionEntryView view;
    uint8_t result_bytes[8];

    if (out_window_id == NULL ||
        !meiso_four_kind_lab_private_receive_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &received) ||
        received.type != MEISO_LAB_MESSAGE_ACTION_DELIVERY_ACCEPT ||
        meiso_action_caller_mark_delivery_accepted(
            host->action, host->action_delivery,
            received.body.action_delivery_accept.id,
            received.body.action_delivery_accept.executor) !=
            MEISO_ACTION_DELIVERY_ACCEPT_CONFIRMED) {
        return false;
    }
    host->action_delivery_accepted = true;
    meiso_four_kind_lab_private_emit_action_delivery_accepted();
    if (!meiso_four_kind_lab_private_receive_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &received) ||
        received.type != MEISO_LAB_MESSAGE_ACTION_RESULT ||
        received.body.action_result.state != MEISO_ACTION_STATE_ACCEPTED) {
        return false;
    }
    result.id = received.body.action_result.id;
    result.executor = received.body.action_result.executor;
    result.state = received.body.action_result.state;
    result.payload = (MeisoByteView){NULL, 0U};
    if (meiso_action_caller_observe_result(host->action, &result) !=
        MEISO_ACTION_RESULT_RECORDED) {
        return false;
    }
    meiso_four_kind_lab_private_emit_action_executor_accepted();
    if (!meiso_four_kind_lab_private_receive_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &received) ||
        received.type != MEISO_LAB_MESSAGE_ACTION_RESULT ||
        received.body.action_result.state != MEISO_ACTION_STATE_SUCCEEDED ||
        received.body.action_result.new_window_id == 0U) {
        return false;
    }
    meiso_four_kind_lab_private_write_u64_be(
        result_bytes, received.body.action_result.new_window_id);
    result.id = received.body.action_result.id;
    result.executor = received.body.action_result.executor;
    result.state = received.body.action_result.state;
    result.payload = (MeisoByteView){result_bytes, sizeof(result_bytes)};
    if (meiso_action_caller_observe_result(host->action, &result) !=
            MEISO_ACTION_RESULT_RECORDED ||
        !meiso_action_caller_query(host->action, host->action_id, &view) ||
        view.state != MEISO_ACTION_STATE_SUCCEEDED ||
        !meiso_four_kind_lab_private_bytes_equal(view.result, result.payload) ||
        meiso_action_caller_end_delivery(host->action, host->action_delivery) !=
            MEISO_ACTION_DELIVERY_ENDED) {
        return false;
    }
    host->action_terminal = true;
    *out_window_id = received.body.action_result.new_window_id;
#ifdef MEISO_LAB_INTERNAL_TEST
    if (meiso_four_kind_lab_private_self_test.enabled) {
        size_t index;
        if (!self_test_observation_index(&index)) {
            return false;
        }
        meiso_four_kind_lab_private_self_test.action_windows[index] =
            *out_window_id;
    }
#endif
    meiso_four_kind_lab_private_emit_action_event(*out_window_id);
    return true;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_host_mark_accepted_action_indeterminate(
    MeisoFourKindLabHostPrimitives *host)
{
    MeisoActionEntryView view;

    if (host == NULL || !host->action_delivery_accepted ||
        host->action_terminal) {
        return true;
    }
    if (meiso_action_caller_mark_indeterminate(host->action, host->action_id,
                                               host->edge_owner) !=
        MEISO_ACTION_INDETERMINATE_MARKED) {
        return false;
    }
    if (!meiso_action_caller_query(host->action, host->action_id, &view) ||
        view.state != MEISO_ACTION_STATE_INDETERMINATE ||
        meiso_action_caller_end_delivery(host->action, host->action_delivery) !=
            MEISO_ACTION_DELIVERY_ENDED) {
        return false;
    }
    host->action_terminal = true;
    return true;
}

static bool host_receive_blob(MeisoFourKindLabConnection *connection,
                              MeisoFourKindLabHostPrimitives *host)
{
    MeisoLabMessage received;
    MeisoLabMessage response;
    MeisoMutableBuffer staging;
    MeisoBlobView published;
    MeisoFourKindLabBlobVerifyContext verify;
    const MeisoContentId *fixed_content_id;

    if (!meiso_four_kind_lab_private_receive_lab_message(
            connection, MEISO_LAB_SENDER_EDGE, &received) ||
        received.type != MEISO_LAB_MESSAGE_BLOB_MANIFEST ||
        meiso_blob_store_stage_begin(
            host->blob, &received.body.blob_manifest.content_id,
            received.body.blob_manifest.content.size) !=
            MEISO_BLOB_STAGE_STARTED ||
        !meiso_blob_store_staging_buffer(host->blob, &staging) ||
        received.body.blob_manifest.content.size > staging.capacity) {
        return false;
    }
    meiso_four_kind_lab_private_emit_blob_staged(
        received.body.blob_manifest.content.size);
    memmove(staging.data, received.body.blob_manifest.content.data,
            received.body.blob_manifest.content.size);
    fixed_content_id = meiso_lab_capability_manifest_content_id();
    if (fixed_content_id == NULL) {
        return false;
    }
    verify.expected_id = *fixed_content_id;
    {
        size_t manifest_size = 0U;
        const uint8_t *manifest = meiso_lab_capability_manifest(&manifest_size);
        if (manifest == NULL) {
            return false;
        }
        verify.expected_content = (MeisoByteView){manifest, manifest_size};
    }
    verify.calls = 0U;
    if (meiso_blob_store_commit_verified(
            host->blob, received.body.blob_manifest.content.size,
            meiso_four_kind_lab_private_verify_fixed_blob,
            &verify) != MEISO_BLOB_COMMIT_PUBLISHED ||
        verify.calls != 1U || meiso_blob_store_count(host->blob) != 1U ||
        !meiso_blob_store_lookup(
            host->blob, &received.body.blob_manifest.content_id, &published) ||
        !meiso_four_kind_lab_private_bytes_equal(published.content,
                                                 verify.expected_content)) {
        return false;
    }
    meiso_four_kind_lab_private_emit_blob_event(published.content.size);
    memset(&response, 0, sizeof(response));
    response.type = MEISO_LAB_MESSAGE_BLOB_ACCEPT;
    response.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    response.body.blob_accept.content_id = published.content_id;
    return meiso_four_kind_lab_private_send_lab_message(
        connection, MEISO_LAB_SENDER_HOST, &response);
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_run_host_transcript(int fd,
                                                MeisoFourKindLabTest test)
{
    MeisoFourKindLabConnection connection;
    MeisoFourKindLabHostPrimitives host;
    MeisoLabStreamMetrics metrics;
    MeisoLabMessage close_message;
    uint64_t new_window_id;
    bool passed = false;

    memset(&connection, 0, sizeof(connection));
    connection.fd = -1;
    memset(&host, 0, sizeof(host));
    memset(&metrics, 0, sizeof(metrics));
    if (!meiso_four_kind_lab_private_initialize_connection(
            &connection, fd, MEISO_SESSION_ROLE_HOST, NULL)) {
        meiso_four_kind_lab_private_emit_connection_failed(&connection);
        meiso_four_kind_lab_private_emit_verdicts(test, false);
        return false;
    }
    if (!meiso_four_kind_lab_private_wait_session_ready(&connection)) {
        meiso_four_kind_lab_private_emit_connection_failed(&connection);
        meiso_four_kind_lab_private_emit_verdicts(test, false);
        return false;
    }
    meiso_four_kind_lab_private_emit_session_ready(&connection);
#ifdef MEISO_LAB_INTERNAL_TEST
    if (!self_test_observe_ready(&connection)) {
        goto finish;
    }
#endif
    if (!host_receive_registry(&connection, &host) ||
        !host_receive_state(&connection, &host) ||
        !host_receive_stream(&connection, &host, &metrics) ||
        !host_send_action(&connection, &host) ||
        !host_receive_action(&connection, &host, &new_window_id) ||
        meiso_stream_count(host.stream) != 1U ||
        !meiso_stream_pop(host.stream) ||
        meiso_stream_count(host.stream) != 0U) {
        goto finish;
    }
    meiso_four_kind_lab_private_emit_stream_consumed(&metrics);
    if (!host_receive_blob(&connection, &host)) {
        goto finish;
    }
    memset(&close_message, 0, sizeof(close_message));
    close_message.type = MEISO_LAB_MESSAGE_CLOSE;
    close_message.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    close_message.body.close.status = MEISO_LAB_CLOSE_PASS;
    if (!meiso_four_kind_lab_private_send_lab_message(
            &connection, MEISO_LAB_SENDER_HOST, &close_message) ||
        !meiso_four_kind_lab_private_normal_host_close(&connection)) {
        goto finish;
    }
    passed = true;

finish:
    if (!passed) {
        if (!meiso_four_kind_lab_private_host_mark_accepted_action_indeterminate(
                &host) ||
            connection.local_failure ==
                MEISO_FOUR_KIND_LAB_LOCAL_FAILURE_NONE) {
            meiso_four_kind_lab_private_record_protocol_failure(&connection);
        }
        meiso_four_kind_lab_private_emit_transcript_failed(&connection);
        meiso_four_kind_lab_private_best_effort_protocol_close(&connection);
    } else {
        meiso_four_kind_lab_private_emit_session_closed(&connection);
    }
    meiso_four_kind_lab_private_emit_verdicts(test, passed);
    return passed;
}
