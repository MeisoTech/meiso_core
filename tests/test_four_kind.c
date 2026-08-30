#include <meiso/core/action.h>
#include <meiso/core/blob.h>
#include <meiso/core/descriptor_set.h>
#include <meiso/core/state.h>
#include <meiso/core/stream.h>

#include "meiso_test_support.h"

#include <stdio.h>
#include <string.h>

typedef union {
    max_align_t alignment;
    uint8_t bytes[8192];
} FixedArena;

typedef struct {
    FixedArena descriptors;
    FixedArena state_publisher;
    FixedArena state_replica;
    FixedArena stream;
    FixedArena action_caller;
    FixedArena action_executor;
    FixedArena blob;
} FixtureStorage;

typedef struct {
    FixtureStorage storage;
    MeisoResourceDescriptor state_descriptor;
    MeisoResourceDescriptor stream_descriptor;
    MeisoResourceDescriptor action_descriptor;
    MeisoResourceDescriptor blob_descriptor;
    MeisoDescriptorSet *descriptors;
    MeisoStatePublisher *state_publisher;
    MeisoStateReplica *state_replica;
    MeisoStream *stream;
    MeisoActionCaller *action_caller;
    MeisoActionExecutor *action_executor;
    MeisoBlobStore *blob;
} FourKindFixture;

typedef struct {
    MeisoContentId id;
    const uint8_t *content;
    size_t size;
    size_t calls;
} VerifyContext;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(                                                      \
                stderr, "CHECK failed at %s:%d: %s\n",                       \
                __FILE__, __LINE__, #condition                                  \
            );                                                                  \
            return false;                                                       \
        }                                                                       \
    } while (0)

static MeisoResourceDescriptor descriptor(
    uint64_t number,
    MeisoResourceKind kind,
    MeisoOwnerRole owner,
    size_t max_payload_size
)
{
    MeisoResourceDescriptor value;

    memset(&value, 0, sizeof(value));
    value.id = (MeisoResourceId){0x46495854555245U, number};
    value.kind = kind;
    value.schema.id = (MeisoSchemaId){0x534348454d41U, number};
    value.schema.version = 1U;
    value.owner = owner;
    value.max_payload_size = max_payload_size;
    return value;
}

static MeisoContentId content_id(uint8_t seed)
{
    MeisoContentId value;
    size_t index;

    memset(&value, 0, sizeof(value));
    value.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    value.digest_size = MEISO_SHA256_DIGEST_SIZE;
    for (index = 0U; index < MEISO_SHA256_DIGEST_SIZE; ++index) {
        value.digest[index] = (uint8_t)(seed + (uint8_t)index);
    }
    return value;
}

static bool verify_content(
    const MeisoContentId *expected_content_id,
    MeisoByteView staged_content,
    void *context
)
{
    VerifyContext *expected = context;

    expected->calls += 1U;
    return meiso_content_id_equal(expected_content_id, &expected->id) &&
        staged_content.size == expected->size &&
        (staged_content.size == 0U ||
         memcmp(staged_content.data, expected->content, staged_content.size) == 0);
}

static bool bytes_equal(MeisoByteView view, const uint8_t *bytes, size_t size)
{
    return view.size == size &&
        (size == 0U || memcmp(view.data, bytes, size) == 0);
}

static bool register_and_seal_descriptors(FourKindFixture *fixture)
{
    MeisoResourceDescriptor declared[4];
    MeisoResourceDescriptor conflict;
    MeisoResourceDescriptor *resolved[4] = {
        &fixture->state_descriptor,
        &fixture->stream_descriptor,
        &fixture->action_descriptor,
        &fixture->blob_descriptor
    };
    const size_t required = meiso_descriptor_set_memory_required(4U);
    size_t index;

    declared[0] = descriptor(
        1U, MEISO_RESOURCE_KIND_STATE, MEISO_OWNER_ROLE_EDGE, 4U
    );
    declared[1] = descriptor(
        2U, MEISO_RESOURCE_KIND_STREAM, MEISO_OWNER_ROLE_EDGE, 4U
    );
    declared[2] = descriptor(
        3U, MEISO_RESOURCE_KIND_ACTION, MEISO_OWNER_ROLE_EDGE, 4U
    );
    declared[3] = descriptor(
        4U, MEISO_RESOURCE_KIND_BLOB, MEISO_OWNER_ROLE_HOST, 4U
    );

    CHECK(required != 0U && required <= sizeof(fixture->storage.descriptors.bytes));
    CHECK(meiso_descriptor_set_init(
        fixture->storage.descriptors.bytes,
        required,
        4U,
        &fixture->descriptors
    ) == MEISO_DESCRIPTOR_SET_INIT_OK);
    for (index = 0U; index < 4U; ++index) {
        CHECK(meiso_descriptor_set_register(fixture->descriptors, &declared[index]) ==
            MEISO_DESCRIPTOR_SET_REGISTERED);
    }

    conflict = declared[0];
    conflict.kind = MEISO_RESOURCE_KIND_STREAM;
    CHECK(meiso_descriptor_set_register(fixture->descriptors, &conflict) ==
        MEISO_DESCRIPTOR_SET_REGISTER_CONFLICT);
    CHECK(meiso_descriptor_set_count(fixture->descriptors) == 4U);
    CHECK(meiso_descriptor_set_seal(fixture->descriptors) ==
        MEISO_DESCRIPTOR_SET_SEALED);

    for (index = 0U; index < 4U; ++index) {
        CHECK(meiso_descriptor_set_lookup(
            fixture->descriptors, declared[index].id, resolved[index]
        ) == MEISO_DESCRIPTOR_SET_LOOKUP_FOUND);
        CHECK(meiso_resource_descriptor_equal(resolved[index], &declared[index]));
    }
    return true;
}

static bool initialize_handles(
    FourKindFixture *fixture,
    MeisoOwnerScope executor_scope
)
{
    const MeisoStateConfig state_config = {MEISO_STATE_DELIVERY_CONVERGENT};
    const MeisoStreamConfig stream_config = {
        MEISO_STREAM_ORDER_STRICT_MONOTONIC,
        MEISO_STREAM_OVERFLOW_REJECT_NEWEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        1U,
        1U,
        0U
    };
    const MeisoActionConfig action_config = {1U, 4U, 4U};
    const MeisoBlobConfig blob_config = {1U};
    size_t required;

    required = meiso_state_publisher_memory_required(
        &fixture->state_descriptor, &state_config
    );
    CHECK(required != 0U &&
        required <= sizeof(fixture->storage.state_publisher.bytes));
    CHECK(meiso_state_publisher_init(
        fixture->storage.state_publisher.bytes,
        required,
        &fixture->state_descriptor,
        &state_config,
        &fixture->state_publisher
    ) == MEISO_STATE_INIT_OK);

    required = meiso_state_replica_memory_required(&fixture->state_descriptor);
    CHECK(required != 0U &&
        required <= sizeof(fixture->storage.state_replica.bytes));
    CHECK(meiso_state_replica_init(
        fixture->storage.state_replica.bytes,
        required,
        &fixture->state_descriptor,
        &fixture->state_replica
    ) == MEISO_STATE_INIT_OK);

    required = meiso_stream_memory_required(
        &fixture->stream_descriptor, &stream_config
    );
    CHECK(required != 0U && required <= sizeof(fixture->storage.stream.bytes));
    CHECK(meiso_stream_init(
        fixture->storage.stream.bytes,
        required,
        &fixture->stream_descriptor,
        &stream_config,
        &fixture->stream
    ) == MEISO_STREAM_INIT_OK);

    required = meiso_action_caller_memory_required(
        &fixture->action_descriptor, &action_config
    );
    CHECK(required != 0U &&
        required <= sizeof(fixture->storage.action_caller.bytes));
    CHECK(meiso_action_caller_init(
        fixture->storage.action_caller.bytes,
        required,
        &fixture->action_descriptor,
        &action_config,
        &fixture->action_caller
    ) == MEISO_ACTION_INIT_OK);

    required = meiso_action_executor_memory_required(
        &fixture->action_descriptor, &action_config
    );
    CHECK(required != 0U &&
        required <= sizeof(fixture->storage.action_executor.bytes));
    CHECK(meiso_action_executor_init(
        fixture->storage.action_executor.bytes,
        required,
        &fixture->action_descriptor,
        &action_config,
        executor_scope,
        &fixture->action_executor
    ) == MEISO_ACTION_INIT_OK);

    required = meiso_blob_store_memory_required(
        &fixture->blob_descriptor, &blob_config
    );
    CHECK(required != 0U && required <= sizeof(fixture->storage.blob.bytes));
    CHECK(meiso_blob_store_init(
        fixture->storage.blob.bytes,
        required,
        &fixture->blob_descriptor,
        &blob_config,
        &fixture->blob
    ) == MEISO_BLOB_INIT_OK);
    return true;
}

static bool assert_committed_baseline(
    FourKindFixture *fixture,
    MeisoOwnerScope state_owner,
    MeisoOwnerScope executor_scope,
    MeisoActionId action_id,
    const MeisoContentId *blob_id
)
{
    const uint8_t desired[] = {'D', '1'};
    const uint8_t stream_payload[] = {'S', '3'};
    const uint8_t action_result[] = {'O', 'K'};
    const uint8_t blob_content[] = {'B', 'L', 'B'};
    MeisoStateView state;
    MeisoStreamSample sample;
    MeisoActionEntryView caller_action;
    MeisoActionEntryView executor_action;
    MeisoBlobView blob;

    CHECK(meiso_state_publisher_current(fixture->state_publisher, &state));
    CHECK(meiso_owner_scope_equal(state.owner, state_owner));
    CHECK(state.version == 1U);
    CHECK(bytes_equal(state.payload, desired, sizeof(desired)));
    CHECK(meiso_state_replica_current(fixture->state_replica, &state));
    CHECK(state.version == 1U);
    CHECK(bytes_equal(state.payload, desired, sizeof(desired)));

    CHECK(meiso_stream_count(fixture->stream) == 1U);
    CHECK(meiso_stream_peek(fixture->stream, &sample));
    CHECK(sample.key.sequence == 3U);
    CHECK(bytes_equal(sample.payload, stream_payload, sizeof(stream_payload)));

    CHECK(meiso_action_caller_query(
        fixture->action_caller, action_id, &caller_action
    ));
    CHECK(caller_action.delivery_accepted == 1U);
    CHECK(caller_action.state == MEISO_ACTION_STATE_SUCCEEDED);
    CHECK(bytes_equal(
        caller_action.result, action_result, sizeof(action_result)
    ));
    CHECK(meiso_action_executor_query(
        fixture->action_executor, action_id, &executor_action
    ));
    CHECK(meiso_owner_scope_equal(
        executor_action.request.expected_executor, executor_scope
    ));
    CHECK(executor_action.state == MEISO_ACTION_STATE_SUCCEEDED);
    CHECK(bytes_equal(
        executor_action.result, action_result, sizeof(action_result)
    ));

    CHECK(meiso_blob_store_count(fixture->blob) == 1U);
    CHECK(meiso_blob_store_lookup(fixture->blob, blob_id, &blob));
    CHECK(bytes_equal(blob.content, blob_content, sizeof(blob_content)));
    return true;
}

static bool exercise_orthogonal_semantics(FourKindFixture *fixture)
{
    const MeisoOwnerScope state_owner = {MEISO_OWNER_ROLE_EDGE, 11U};
    const MeisoOwnerScope stream_owner = {MEISO_OWNER_ROLE_EDGE, 12U};
    const MeisoOwnerScope executor_scope = {MEISO_OWNER_ROLE_EDGE, 13U};
    const MeisoDeliveryGeneration state_delivery = {1U};
    const MeisoDeliveryGeneration action_delivery = {1U};
    const uint8_t desired[] = {'D', '1'};
    const uint8_t first_stream_payload[] = {'S', '1'};
    const uint8_t third_stream_payload[] = {'S', '3'};
    const uint8_t arguments[] = {'G', 'O'};
    const uint8_t result_payload[] = {'O', 'K'};
    const uint8_t blob_content[] = {'B', 'L', 'B'};
    const MeisoContentId first_blob_id = content_id(1U);
    MeisoStateOffer state_offer;
    MeisoStateView actual;
    MeisoStreamSample stream_sample;
    MeisoStreamSample stream_view;
    MeisoStreamReceiveResult stream_result;
    MeisoActionRequest request;
    MeisoActionOffer action_offer;
    MeisoActionEntryView action_view;
    MeisoActionResult action_result;
    MeisoMutableBuffer staging;
    MeisoBlobView blob_view;
    VerifyContext verify = {
        first_blob_id, blob_content, sizeof(blob_content), 0U
    };
    uint8_t transported_state[sizeof(desired)];
    size_t cursor = 0U;
    size_t execution_count = 0U;

    CHECK(meiso_state_publisher_publish(
        fixture->state_publisher,
        state_owner,
        1U,
        (MeisoByteView){desired, sizeof(desired)}
    ) == MEISO_STATE_PUBLISH_NEW);
    CHECK(meiso_state_publisher_begin_delivery(
        fixture->state_publisher, state_delivery
    ) == MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_publisher_next_offer(
        fixture->state_publisher, &state_offer
    ));
    CHECK(state_offer.state.payload.size == sizeof(transported_state));
    memmove(
        transported_state,
        state_offer.state.payload.data,
        state_offer.state.payload.size
    );
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        fixture->state_publisher,
        state_delivery,
        state_offer.state.owner,
        state_offer.state.version
    ) == MEISO_STATE_ACCEPT_CONFIRMED);
    CHECK(!meiso_state_replica_current(fixture->state_replica, &actual));
    CHECK(meiso_state_replica_begin_owner(fixture->state_replica, state_owner) ==
        MEISO_STATE_OWNER_STARTED);
    CHECK(meiso_state_replica_receive(
        fixture->state_replica,
        state_owner,
        1U,
        (MeisoByteView){transported_state, sizeof(transported_state)}
    ) == MEISO_STATE_RECEIVE_NEW);
    CHECK(meiso_state_replica_current(fixture->state_replica, &actual));
    CHECK(bytes_equal(actual.payload, desired, sizeof(desired)));

    /* Core v0 has Stream, not a MediaStream contract; this stays byte-stream only. */
    CHECK(meiso_stream_begin_producer(
        fixture->stream, stream_owner, 7U
    ) == MEISO_STREAM_PRODUCER_STARTED);
    memset(&stream_sample, 0, sizeof(stream_sample));
    stream_sample.key = (MeisoStreamKey){stream_owner, 7U, 1U};
    stream_sample.source_time = (MeisoTimePoint){21U, 100U};
    stream_sample.payload = (MeisoByteView){
        first_stream_payload, sizeof(first_stream_payload)
    };
    stream_result = meiso_stream_receive(fixture->stream, &stream_sample, NULL);
    CHECK(stream_result.code == MEISO_STREAM_RECEIVE_ACCEPTED);
    CHECK(meiso_stream_count(fixture->stream) == 1U);
    CHECK(meiso_stream_peek(fixture->stream, &stream_view));
    CHECK(stream_view.key.sequence == 1U);
    CHECK(meiso_stream_pop(fixture->stream));
    CHECK(meiso_stream_count(fixture->stream) == 0U);

    stream_sample.key.sequence = 3U;
    stream_sample.source_time.monotonic_ns = 300U;
    stream_sample.payload = (MeisoByteView){
        third_stream_payload, sizeof(third_stream_payload)
    };
    stream_result = meiso_stream_receive(fixture->stream, &stream_sample, NULL);
    CHECK(stream_result.code == MEISO_STREAM_RECEIVE_ACCEPTED);
    CHECK(stream_result.flags == MEISO_STREAM_RECEIVE_FLAG_GAP);
    CHECK(stream_result.missing_before == 1U);

    memset(&request, 0, sizeof(request));
    request.id = (MeisoActionId){0x414354494f4eU, 1U};
    request.expected_executor = executor_scope;
    request.target = fixture->state_descriptor.id;
    request.operation = fixture->action_descriptor.schema;
    request.arguments = (MeisoByteView){arguments, sizeof(arguments)};
    request.retry_policy = MEISO_ACTION_RETRY_NONE;
    CHECK(meiso_action_caller_begin_delivery(
        fixture->action_caller, action_delivery
    ) == MEISO_ACTION_DELIVERY_STARTED);
    CHECK(meiso_action_caller_submit(fixture->action_caller, &request) ==
        MEISO_ACTION_SUBMIT_NEW);
    CHECK(meiso_action_caller_next_offer(
        fixture->action_caller, action_delivery, &cursor, &action_offer
    ));
    CHECK(meiso_action_executor_ingest(
        fixture->action_executor, &action_offer.request, NULL
    ) == MEISO_ACTION_INGEST_NEW);

    CHECK(meiso_action_caller_mark_delivery_accepted(
        fixture->action_caller,
        action_delivery,
        request.id,
        executor_scope
    ) == MEISO_ACTION_DELIVERY_ACCEPT_CONFIRMED);
    CHECK(meiso_action_caller_query(
        fixture->action_caller, request.id, &action_view
    ));
    CHECK(action_view.delivery_accepted == 1U);
    CHECK(action_view.state == MEISO_ACTION_STATE_PENDING);
    CHECK(meiso_action_executor_query(
        fixture->action_executor, request.id, &action_view
    ));
    CHECK(action_view.state == MEISO_ACTION_STATE_PENDING);

    CHECK(meiso_action_executor_accept(
        fixture->action_executor,
        executor_scope,
        request.id,
        (MeisoTimePoint){0U, 0U}
    ) == MEISO_ACTION_EXECUTOR_ACCEPTED);
    execution_count += 1U;
    CHECK(meiso_action_executor_accept(
        fixture->action_executor,
        executor_scope,
        request.id,
        (MeisoTimePoint){0U, 0U}
    ) == MEISO_ACTION_EXECUTOR_ACCEPT_DUPLICATE);
    CHECK(execution_count == 1U);
    CHECK(meiso_action_caller_query(
        fixture->action_caller, request.id, &action_view
    ));
    CHECK(action_view.state == MEISO_ACTION_STATE_PENDING);

    action_result = (MeisoActionResult){
        request.id,
        executor_scope,
        MEISO_ACTION_STATE_SUCCEEDED,
        {result_payload, sizeof(result_payload)}
    };
    CHECK(meiso_action_executor_record_result(
        fixture->action_executor, &action_result
    ) == MEISO_ACTION_RECORD_RECORDED);
    CHECK(meiso_action_caller_query(
        fixture->action_caller, request.id, &action_view
    ));
    CHECK(action_view.state == MEISO_ACTION_STATE_PENDING);
    CHECK(meiso_action_caller_observe_result(
        fixture->action_caller, &action_result
    ) == MEISO_ACTION_RESULT_RECORDED);
    CHECK(meiso_action_executor_ingest(
        fixture->action_executor, &request, NULL
    ) == MEISO_ACTION_INGEST_REPLAY);
    cursor = 0U;
    CHECK(!meiso_action_executor_next_application_request(
        fixture->action_executor, &cursor, &request
    ));
    CHECK(execution_count == 1U);

    CHECK(meiso_blob_store_stage_begin(
        fixture->blob, &first_blob_id, sizeof(blob_content)
    ) == MEISO_BLOB_STAGE_STARTED);
    CHECK(!meiso_blob_store_lookup(fixture->blob, &first_blob_id, &blob_view));
    CHECK(meiso_blob_store_count(fixture->blob) == 0U);
    CHECK(meiso_blob_store_staging_buffer(fixture->blob, &staging));
    memmove(staging.data, blob_content, sizeof(blob_content));
    CHECK(meiso_blob_store_commit_verified(
        fixture->blob,
        sizeof(blob_content),
        verify_content,
        &verify
    ) == MEISO_BLOB_COMMIT_PUBLISHED);
    CHECK(verify.calls == 1U);
    CHECK(meiso_blob_store_lookup(fixture->blob, &first_blob_id, &blob_view));
    CHECK(bytes_equal(blob_view.content, blob_content, sizeof(blob_content)));

    CHECK(assert_committed_baseline(
        fixture, state_owner, executor_scope, action_result.id, &first_blob_id
    ));
    return true;
}

static bool exercise_isolated_capacity_failures(FourKindFixture *fixture)
{
    const MeisoOwnerScope state_owner = {MEISO_OWNER_ROLE_EDGE, 11U};
    const MeisoOwnerScope stream_owner = {MEISO_OWNER_ROLE_EDGE, 12U};
    const MeisoOwnerScope executor_scope = {MEISO_OWNER_ROLE_EDGE, 13U};
    const MeisoActionId first_action_id = {0x414354494f4eU, 1U};
    const uint8_t oversized_state[] = {1U, 2U, 3U, 4U, 5U};
    const uint8_t overflow_payload[] = {'S', '4'};
    const uint8_t arguments[] = {'N', 'O'};
    const uint8_t second_blob_content[] = {'N', 'E', 'W'};
    const MeisoContentId first_blob_id = content_id(1U);
    const MeisoContentId second_blob_id = content_id(2U);
    MeisoStreamSample overflow;
    MeisoStreamReceiveResult stream_result;
    MeisoActionRequest second_action;
    MeisoBlobView view;
    MeisoMutableBuffer staging;
    VerifyContext verify = {
        second_blob_id,
        second_blob_content,
        sizeof(second_blob_content),
        0U
    };

    CHECK(meiso_state_publisher_publish(
        fixture->state_publisher,
        state_owner,
        2U,
        (MeisoByteView){oversized_state, sizeof(oversized_state)}
    ) == MEISO_STATE_PUBLISH_CAPACITY);
    CHECK(assert_committed_baseline(
        fixture, state_owner, executor_scope, first_action_id, &first_blob_id
    ));

    memset(&overflow, 0, sizeof(overflow));
    overflow.key = (MeisoStreamKey){stream_owner, 7U, 4U};
    overflow.source_time = (MeisoTimePoint){21U, 400U};
    overflow.payload = (MeisoByteView){
        overflow_payload, sizeof(overflow_payload)
    };
    stream_result = meiso_stream_receive(fixture->stream, &overflow, NULL);
    CHECK(stream_result.code == MEISO_STREAM_RECEIVE_OVERFLOW_REJECTED);
    CHECK(stream_result.flags == MEISO_STREAM_RECEIVE_FLAG_NONE);
    CHECK(stream_result.missing_before == 0U);
    CHECK(assert_committed_baseline(
        fixture, state_owner, executor_scope, first_action_id, &first_blob_id
    ));

    memset(&second_action, 0, sizeof(second_action));
    second_action.id = (MeisoActionId){0x414354494f4eU, 2U};
    second_action.expected_executor = executor_scope;
    second_action.target = fixture->state_descriptor.id;
    second_action.operation = fixture->action_descriptor.schema;
    second_action.arguments = (MeisoByteView){arguments, sizeof(arguments)};
    second_action.retry_policy = MEISO_ACTION_RETRY_NONE;
    CHECK(meiso_action_caller_submit(fixture->action_caller, &second_action) ==
        MEISO_ACTION_SUBMIT_TABLE_FULL);
    CHECK(meiso_action_executor_ingest(
        fixture->action_executor, &second_action, NULL
    ) == MEISO_ACTION_INGEST_TABLE_FULL);
    CHECK(assert_committed_baseline(
        fixture, state_owner, executor_scope, first_action_id, &first_blob_id
    ));

    CHECK(meiso_blob_store_stage_begin(
        fixture->blob, &second_blob_id, sizeof(second_blob_content)
    ) == MEISO_BLOB_STAGE_STARTED);
    CHECK(meiso_blob_store_staging_buffer(fixture->blob, &staging));
    memmove(staging.data, second_blob_content, sizeof(second_blob_content));
    CHECK(!meiso_blob_store_lookup(fixture->blob, &second_blob_id, &view));
    CHECK(meiso_blob_store_commit_verified(
        fixture->blob,
        sizeof(second_blob_content),
        verify_content,
        &verify
    ) == MEISO_BLOB_COMMIT_CAPACITY);
    CHECK(verify.calls == 1U);
    CHECK(!meiso_blob_store_lookup(fixture->blob, &second_blob_id, &view));
    CHECK(meiso_blob_store_staging_buffer(fixture->blob, &staging));
    CHECK(assert_committed_baseline(
        fixture, state_owner, executor_scope, first_action_id, &first_blob_id
    ));
    CHECK(meiso_blob_store_abort_staging(
        fixture->blob, MEISO_BLOB_ABORT_CALLER_REQUEST
    ) == MEISO_BLOB_ABORTED);
    return true;
}

static bool test_four_kind_fixture(void)
{
    MEISO_TEST_ARENA_STORAGE FourKindFixture fixture;
    const MeisoOwnerScope executor_scope = {MEISO_OWNER_ROLE_EDGE, 13U};

    memset(&fixture, 0, sizeof(fixture));
    CHECK(register_and_seal_descriptors(&fixture));
    CHECK(initialize_handles(&fixture, executor_scope));
    CHECK(exercise_orthogonal_semantics(&fixture));
    CHECK(exercise_isolated_capacity_failures(&fixture));
    return true;
}

int main(void)
{
    if (!test_four_kind_fixture()) {
        return 1;
    }
    (void)puts("four-kind conformance fixture passed");
    return 0;
}
