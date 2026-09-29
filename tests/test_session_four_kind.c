#include <meiso/core/action.h>
#include <meiso/core/blob.h>
#include <meiso/core/descriptor_set.h>
#include <meiso/core/session.h>
#include <meiso/core/state.h>
#include <meiso/core/stream.h>

#include "meiso_test_support.h"

#include <stdio.h>
#include <string.h>

#define TEST_ENVELOPE_HEADER_SIZE 24U

typedef union {
    MeisoMaxAlign alignment;
    uint8_t bytes[4096];
} FixedArena;

typedef struct {
    FixedArena host_session;
    FixedArena edge_session;
    FixedArena descriptors;
    FixedArena state_replica;
    FixedArena stream;
    FixedArena action_executor;
    FixedArena blob;
} FixtureStorage;

typedef struct {
    FixtureStorage storage;
    MeisoSessionConfig host_config;
    MeisoSessionConfig edge_config;
    MeisoSession *host_session;
    MeisoSession *edge_session;
    MeisoDescriptorSet *descriptors;
    MeisoResourceDescriptor state_descriptor;
    MeisoResourceDescriptor stream_descriptor;
    MeisoResourceDescriptor action_descriptor;
    MeisoResourceDescriptor blob_descriptor;
    MeisoStateReplica *state_replica;
    MeisoStream *stream;
    MeisoActionExecutor *action_executor;
    MeisoBlobStore *blob;
    MeisoOwnerScope edge_scope;
    size_t dispatch_count[5];
} SessionFourKindFixture;

typedef struct {
    MeisoContentId id;
    MeisoByteView content;
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

static void write_u32_be(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24U);
    destination[1] = (uint8_t)(value >> 16U);
    destination[2] = (uint8_t)(value >> 8U);
    destination[3] = (uint8_t)value;
}

static void write_u64_be(uint8_t *destination, uint64_t value)
{
    destination[0] = (uint8_t)(value >> 56U);
    destination[1] = (uint8_t)(value >> 48U);
    destination[2] = (uint8_t)(value >> 40U);
    destination[3] = (uint8_t)(value >> 32U);
    destination[4] = (uint8_t)(value >> 24U);
    destination[5] = (uint8_t)(value >> 16U);
    destination[6] = (uint8_t)(value >> 8U);
    destination[7] = (uint8_t)value;
}

static uint32_t read_u32_be(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24U) |
        ((uint32_t)source[1] << 16U) |
        ((uint32_t)source[2] << 8U) |
        (uint32_t)source[3];
}

static uint64_t read_u64_be(const uint8_t *source)
{
    return ((uint64_t)source[0] << 56U) |
        ((uint64_t)source[1] << 48U) |
        ((uint64_t)source[2] << 40U) |
        ((uint64_t)source[3] << 32U) |
        ((uint64_t)source[4] << 24U) |
        ((uint64_t)source[5] << 16U) |
        ((uint64_t)source[6] << 8U) |
        (uint64_t)source[7];
}

static bool bytes_equal(MeisoByteView actual, const uint8_t *expected, size_t size)
{
    return actual.size == size &&
        (size == 0U || memcmp(actual.data, expected, size) == 0);
}

static MeisoSessionConfig session_config(MeisoSessionRole role)
{
    MeisoSessionConfig config;
    const MeisoPeerId host_peer = {
        UINT64_C(0x484f53545f504545), UINT64_C(0x525f303030303031)
    };
    const MeisoPeerId edge_peer = {
        UINT64_C(0x454447455f504545), UINT64_C(0x525f303030303031)
    };

    memset(&config, 0, sizeof(config));
    config.local_role = role;
    config.local_peer_id = role == MEISO_SESSION_ROLE_HOST ?
        host_peer : edge_peer;
    config.local_incarnation = role == MEISO_SESSION_ROLE_HOST ? 17U : 23U;
    config.local_nonce = role == MEISO_SESSION_ROLE_HOST ?
        (MeisoSessionNonce){0x101U, 0x102U} :
        (MeisoSessionNonce){0x201U, 0x202U};
    config.expected_peer_id = role == MEISO_SESSION_ROLE_HOST ?
        edge_peer : host_peer;
    config.protocol = (MeisoProtocolVersion){1U, 0U};
    config.profile.id = (MeisoProfileId){0x4d4549534fU, 1U};
    config.profile.version = 1U;
    config.limits = (MeisoSessionLimits){256U, 128U, 4U};
    return config;
}

static MeisoResourceDescriptor descriptor(
    uint64_t number,
    MeisoResourceKind kind
)
{
    MeisoResourceDescriptor value;

    memset(&value, 0, sizeof(value));
    value.id = (MeisoResourceId){UINT64_C(0x53455353494f4e52), number};
    value.kind = kind;
    value.schema.id = (MeisoSchemaId){
        UINT64_C(0x53455353494f4e53), number
    };
    value.schema.version = 1U;
    value.owner = MEISO_OWNER_ROLE_EDGE;
    value.max_payload_size = 4U;
    return value;
}

static MeisoContentId content_id(void)
{
    MeisoContentId value;
    size_t index;

    memset(&value, 0, sizeof(value));
    value.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    value.digest_size = MEISO_SHA256_DIGEST_SIZE;
    for (index = 0U; index < MEISO_SHA256_DIGEST_SIZE; ++index) {
        value.digest[index] = (uint8_t)(0x40U + index);
    }
    return value;
}

static bool verify_blob(
    const MeisoContentId *expected_content_id,
    MeisoByteView staged_content,
    void *context
)
{
    VerifyContext *verify = context;

    verify->calls += 1U;
    return meiso_content_id_equal(expected_content_id, &verify->id) &&
        bytes_equal(staged_content, verify->content.data, verify->content.size);
}

static bool initialize_session(
    FixedArena *arena,
    const MeisoSessionConfig *config,
    MeisoSession **out_session
)
{
    const size_t required = meiso_session_memory_required(config);

    CHECK(required != 0U && required <= sizeof(arena->bytes));
    CHECK(meiso_session_init(
        arena->bytes, required, config, out_session
    ) == MEISO_SESSION_INIT_OK);
    return true;
}

static bool initialize_descriptors(SessionFourKindFixture *fixture)
{
    MeisoResourceDescriptor *items[4] = {
        &fixture->state_descriptor,
        &fixture->stream_descriptor,
        &fixture->action_descriptor,
        &fixture->blob_descriptor
    };
    const size_t required = meiso_descriptor_set_memory_required(4U);
    size_t index;

    fixture->state_descriptor = descriptor(1U, MEISO_RESOURCE_KIND_STATE);
    fixture->stream_descriptor = descriptor(2U, MEISO_RESOURCE_KIND_STREAM);
    fixture->action_descriptor = descriptor(3U, MEISO_RESOURCE_KIND_ACTION);
    fixture->blob_descriptor = descriptor(4U, MEISO_RESOURCE_KIND_BLOB);
    CHECK(required != 0U && required <=
        sizeof(fixture->storage.descriptors.bytes));
    CHECK(meiso_descriptor_set_init(
        fixture->storage.descriptors.bytes,
        required,
        4U,
        &fixture->descriptors
    ) == MEISO_DESCRIPTOR_SET_INIT_OK);
    for (index = 0U; index < 4U; ++index) {
        CHECK(meiso_descriptor_set_register(
            fixture->descriptors, items[index]
        ) == MEISO_DESCRIPTOR_SET_REGISTERED);
    }
    CHECK(meiso_descriptor_set_seal(fixture->descriptors) ==
        MEISO_DESCRIPTOR_SET_SEALED);
    return true;
}

static bool initialize_receivers(SessionFourKindFixture *fixture)
{
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

    required = meiso_state_replica_memory_required(&fixture->state_descriptor);
    CHECK(required != 0U && required <=
        sizeof(fixture->storage.state_replica.bytes));
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

    required = meiso_action_executor_memory_required(
        &fixture->action_descriptor, &action_config
    );
    CHECK(required != 0U && required <=
        sizeof(fixture->storage.action_executor.bytes));
    CHECK(meiso_action_executor_init(
        fixture->storage.action_executor.bytes,
        required,
        &fixture->action_descriptor,
        &action_config,
        fixture->edge_scope,
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

static bool initialize_fixture(SessionFourKindFixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->host_config = session_config(MEISO_SESSION_ROLE_HOST);
    fixture->edge_config = session_config(MEISO_SESSION_ROLE_EDGE);
    fixture->edge_scope = (MeisoOwnerScope){
        MEISO_OWNER_ROLE_EDGE, fixture->edge_config.local_incarnation
    };
    CHECK(initialize_session(
        &fixture->storage.host_session,
        &fixture->host_config,
        &fixture->host_session
    ));
    CHECK(initialize_session(
        &fixture->storage.edge_session,
        &fixture->edge_config,
        &fixture->edge_session
    ));
    CHECK(initialize_descriptors(fixture));
    CHECK(initialize_receivers(fixture));
    return true;
}

static bool deliver_handshake_frame(
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoSessionReceiveStatus expected_status
)
{
    uint8_t frame[512];
    MeisoByteView outbound;
    MeisoSessionRuntimeMessage runtime = {0};

    CHECK(meiso_session_next_outbound(sender, &outbound));
    CHECK(outbound.size <= sizeof(frame));
    memmove(frame, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(sender) ==
        MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        receiver,
        (MeisoByteView){frame, outbound.size},
        &runtime
    ) == expected_status);
    return true;
}

static bool make_ready(MeisoSession *host_session, MeisoSession *edge_session)
{
    MeisoSessionSnapshot host;
    MeisoSessionSnapshot edge;

    CHECK(meiso_session_start(edge_session) ==
        MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(host_session) ==
        MEISO_SESSION_START_STARTED);
    CHECK(deliver_handshake_frame(
        host_session,
        edge_session,
        MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(deliver_handshake_frame(
        edge_session,
        host_session,
        MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(deliver_handshake_frame(
        host_session,
        edge_session,
        MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(deliver_handshake_frame(
        edge_session,
        host_session,
        MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(meiso_session_snapshot(host_session, &host));
    CHECK(meiso_session_snapshot(edge_session, &edge));
    CHECK(host.state == MEISO_SESSION_STATE_READY);
    CHECK(edge.state == MEISO_SESSION_STATE_READY);
    CHECK(meiso_session_id_equal(host.session_id, edge.session_id));
    return true;
}

/* This is a test-only Runtime envelope, not a frozen Meiso wire layout. */
static bool build_envelope(
    uint8_t *storage,
    size_t capacity,
    MeisoResourceId resource_id,
    MeisoResourceKind kind,
    MeisoByteView payload,
    MeisoByteView *out_envelope
)
{
    const size_t total = TEST_ENVELOPE_HEADER_SIZE + payload.size;

    CHECK(meiso_resource_id_valid(resource_id));
    CHECK(kind >= MEISO_RESOURCE_KIND_STATE &&
        kind <= MEISO_RESOURCE_KIND_BLOB);
    CHECK(meiso_byte_view_valid(payload));
    CHECK(total <= capacity && payload.size <= UINT32_MAX);
    write_u64_be(storage, resource_id.high);
    write_u64_be(storage + 8U, resource_id.low);
    storage[16] = kind;
    storage[17] = 0U;
    storage[18] = 0U;
    storage[19] = 0U;
    write_u32_be(storage + 20U, (uint32_t)payload.size);
    if (payload.size != 0U) {
        memmove(storage + TEST_ENVELOPE_HEADER_SIZE, payload.data, payload.size);
    }
    *out_envelope = (MeisoByteView){storage, total};
    return true;
}

static bool route_runtime(
    SessionFourKindFixture *fixture,
    const MeisoSession *receiving_session,
    MeisoSessionRuntimeMessage runtime,
    MeisoResourceDescriptor *out_descriptor,
    MeisoByteView *out_payload
)
{
    MeisoSessionSnapshot session;
    MeisoResourceId resource_id;
    MeisoResourceKind kind;
    uint32_t declared_size;

    if (!meiso_session_snapshot(receiving_session, &session) ||
        session.state != MEISO_SESSION_STATE_READY ||
        !session.session_id_known ||
        !meiso_session_id_equal(runtime.session_id, session.session_id) ||
        runtime.payload.size < TEST_ENVELOPE_HEADER_SIZE ||
        runtime.payload.data == NULL || runtime.payload.data[17] != 0U ||
        runtime.payload.data[18] != 0U || runtime.payload.data[19] != 0U) {
        return false;
    }
    resource_id.high = read_u64_be(runtime.payload.data);
    resource_id.low = read_u64_be(runtime.payload.data + 8U);
    kind = runtime.payload.data[16];
    declared_size = read_u32_be(runtime.payload.data + 20U);
    if ((size_t)declared_size !=
        runtime.payload.size - TEST_ENVELOPE_HEADER_SIZE) {
        return false;
    }
    *out_payload = (MeisoByteView){
        runtime.payload.data + TEST_ENVELOPE_HEADER_SIZE,
        (size_t)declared_size
    };
    if (meiso_descriptor_set_lookup(
            fixture->descriptors, resource_id, out_descriptor
        ) != MEISO_DESCRIPTOR_SET_LOOKUP_FOUND ||
        out_descriptor->kind != kind) {
        return false;
    }
    return true;
}

static bool dispatch_runtime(
    SessionFourKindFixture *fixture,
    const MeisoSession *receiving_session,
    MeisoSessionRuntimeMessage runtime
)
{
    MeisoResourceDescriptor resolved;
    MeisoByteView payload;
    MeisoResourceKind kind;

    CHECK(route_runtime(
        fixture, receiving_session, runtime, &resolved, &payload
    ));
    kind = resolved.kind;

    if (kind == MEISO_RESOURCE_KIND_STATE) {
        CHECK(meiso_state_replica_begin_owner(
            fixture->state_replica, fixture->edge_scope
        ) == MEISO_STATE_OWNER_STARTED);
        CHECK(meiso_state_replica_receive(
            fixture->state_replica, fixture->edge_scope, 1U, payload
        ) == MEISO_STATE_RECEIVE_NEW);
    } else if (kind == MEISO_RESOURCE_KIND_STREAM) {
        MeisoStreamSample sample;
        MeisoStreamReceiveResult result;

        CHECK(meiso_stream_begin_producer(
            fixture->stream, fixture->edge_scope, 7U
        ) == MEISO_STREAM_PRODUCER_STARTED);
        memset(&sample, 0, sizeof(sample));
        sample.key = (MeisoStreamKey){fixture->edge_scope, 7U, 1U};
        sample.source_time = (MeisoTimePoint){9U, 100U};
        sample.payload = payload;
        result = meiso_stream_receive(fixture->stream, &sample, NULL);
        CHECK(result.code == MEISO_STREAM_RECEIVE_ACCEPTED);
    } else if (kind == MEISO_RESOURCE_KIND_ACTION) {
        MeisoActionRequest request;

        memset(&request, 0, sizeof(request));
        request.id = (MeisoActionId){UINT64_C(0x414354494f4e), 1U};
        request.expected_executor = fixture->edge_scope;
        request.target = fixture->state_descriptor.id;
        request.operation = fixture->action_descriptor.schema;
        request.arguments = payload;
        request.retry_policy = MEISO_ACTION_RETRY_NONE;
        CHECK(meiso_action_executor_ingest(
            fixture->action_executor, &request, NULL
        ) == MEISO_ACTION_INGEST_NEW);
    } else if (kind == MEISO_RESOURCE_KIND_BLOB) {
        const MeisoContentId id = content_id();
        MeisoMutableBuffer staging;
        VerifyContext verify = {id, payload, 0U};

        CHECK(meiso_blob_store_stage_begin(
            fixture->blob, &id, payload.size
        ) == MEISO_BLOB_STAGE_STARTED);
        CHECK(meiso_blob_store_staging_buffer(fixture->blob, &staging));
        CHECK(payload.size <= staging.capacity);
        if (payload.size != 0U) {
            memmove(staging.data, payload.data, payload.size);
        }
        CHECK(meiso_blob_store_commit_verified(
            fixture->blob, payload.size, verify_blob, &verify
        ) == MEISO_BLOB_COMMIT_PUBLISHED);
        CHECK(verify.calls == 1U);
    } else {
        CHECK(false);
    }
    fixture->dispatch_count[kind] += 1U;
    return true;
}

static bool receive_envelope(
    SessionFourKindFixture *fixture,
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoResourceId resource_id,
    MeisoResourceKind kind,
    MeisoByteView payload,
    MeisoSessionRuntimeMessage *out_runtime
)
{
    uint8_t envelope_storage[128];
    uint8_t session_frame[512];
    MeisoByteView envelope;
    MeisoByteView outbound;

    (void)fixture;

    CHECK(build_envelope(
        envelope_storage,
        sizeof(envelope_storage),
        resource_id,
        kind,
        payload,
        &envelope
    ));
    CHECK(meiso_session_submit_runtime(sender, envelope) ==
        MEISO_SESSION_SUBMIT_QUEUED);
    CHECK(meiso_session_next_outbound(sender, &outbound));
    CHECK(outbound.size <= sizeof(session_frame));
    memmove(session_frame, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(sender) ==
        MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        receiver,
        (MeisoByteView){session_frame, outbound.size},
        out_runtime
    ) == MEISO_SESSION_RECEIVE_RUNTIME);
    return true;
}

static bool send_envelope(
    SessionFourKindFixture *fixture,
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoResourceId resource_id,
    MeisoResourceKind kind,
    MeisoByteView payload
)
{
    MeisoSessionRuntimeMessage runtime = {0};

    CHECK(receive_envelope(
        fixture, sender, receiver, resource_id, kind, payload, &runtime
    ));
    CHECK(dispatch_runtime(fixture, receiver, runtime));
    return true;
}

static bool assert_no_delivery(const SessionFourKindFixture *fixture)
{
    MeisoStateView state;
    MeisoActionEntryView action;
    const MeisoActionId action_id = {UINT64_C(0x414354494f4e), 1U};
    const MeisoContentId id = content_id();
    MeisoBlobView blob;

    CHECK(!meiso_state_replica_current(fixture->state_replica, &state));
    CHECK(meiso_stream_count(fixture->stream) == 0U);
    CHECK(!meiso_action_executor_query(
        fixture->action_executor, action_id, &action
    ));
    CHECK(meiso_blob_store_count(fixture->blob) == 0U);
    CHECK(!meiso_blob_store_lookup(fixture->blob, &id, &blob));
    CHECK(fixture->dispatch_count[MEISO_RESOURCE_KIND_STATE] == 0U);
    CHECK(fixture->dispatch_count[MEISO_RESOURCE_KIND_STREAM] == 0U);
    CHECK(fixture->dispatch_count[MEISO_RESOURCE_KIND_ACTION] == 0U);
    CHECK(fixture->dispatch_count[MEISO_RESOURCE_KIND_BLOB] == 0U);
    return true;
}

static bool assert_delivered(const SessionFourKindFixture *fixture)
{
    static const uint8_t state_bytes[] = {'S', 'T'};
    static const uint8_t stream_bytes[] = {'S', 'M'};
    static const uint8_t action_bytes[] = {'A', 'C'};
    static const uint8_t blob_bytes[] = {'B', 'L', 'B'};
    MeisoStateView state;
    MeisoStreamSample stream;
    MeisoActionEntryView action;
    const MeisoActionId action_id = {UINT64_C(0x414354494f4e), 1U};
    const MeisoContentId id = content_id();
    MeisoBlobView blob;

    CHECK(meiso_state_replica_current(fixture->state_replica, &state));
    CHECK(state.version == 1U);
    CHECK(bytes_equal(state.payload, state_bytes, sizeof(state_bytes)));
    CHECK(meiso_stream_count(fixture->stream) == 1U);
    CHECK(meiso_stream_peek(fixture->stream, &stream));
    CHECK(stream.key.epoch == 7U && stream.key.sequence == 1U);
    CHECK(bytes_equal(stream.payload, stream_bytes, sizeof(stream_bytes)));
    CHECK(meiso_action_executor_query(
        fixture->action_executor, action_id, &action
    ));
    CHECK(action.state == MEISO_ACTION_STATE_PENDING);
    CHECK(bytes_equal(
        action.request.arguments, action_bytes, sizeof(action_bytes)
    ));
    CHECK(meiso_blob_store_count(fixture->blob) == 1U);
    CHECK(meiso_blob_store_lookup(fixture->blob, &id, &blob));
    CHECK(bytes_equal(blob.content, blob_bytes, sizeof(blob_bytes)));
    CHECK(fixture->dispatch_count[MEISO_RESOURCE_KIND_STATE] == 1U);
    CHECK(fixture->dispatch_count[MEISO_RESOURCE_KIND_STREAM] == 1U);
    CHECK(fixture->dispatch_count[MEISO_RESOURCE_KIND_ACTION] == 1U);
    CHECK(fixture->dispatch_count[MEISO_RESOURCE_KIND_BLOB] == 1U);
    return true;
}

static bool submit_before_ready_is_rejected(
    MeisoSession *session,
    MeisoResourceId resource_id,
    MeisoResourceKind kind,
    MeisoByteView payload
)
{
    uint8_t storage[128];
    MeisoByteView envelope;
    MeisoByteView outbound;

    CHECK(build_envelope(
        storage, sizeof(storage), resource_id, kind, payload, &envelope
    ));
    CHECK(meiso_session_submit_runtime(session, envelope) ==
        MEISO_SESSION_SUBMIT_NOT_READY);
    CHECK(!meiso_session_next_outbound(session, &outbound));
    return true;
}

static bool close_pair(SessionFourKindFixture *fixture)
{
    CHECK(meiso_session_close(
        fixture->host_session, MEISO_SESSION_CLOSE_NORMAL
    ) == MEISO_SESSION_CLOSE_QUEUED);
    CHECK(deliver_handshake_frame(
        fixture->host_session,
        fixture->edge_session,
        MEISO_SESSION_RECEIVE_PEER_CLOSED
    ));
    return true;
}

static bool test_session_gated_four_kind_delivery(void)
{
    MEISO_TEST_ARENA_STORAGE SessionFourKindFixture fixture;
    MEISO_TEST_ARENA_STORAGE FixedArena second_host_arena;
    MEISO_TEST_ARENA_STORAGE FixedArena second_edge_arena;
    static const uint8_t state_bytes[] = {'S', 'T'};
    static const uint8_t stream_bytes[] = {'S', 'M'};
    static const uint8_t action_bytes[] = {'A', 'C'};
    static const uint8_t blob_bytes[] = {'B', 'L', 'B'};
    MeisoSessionRuntimeMessage rejected_runtime = {0};
    MeisoSessionRuntimeMessage cached_runtime = {0};
    MeisoSessionRuntimeMessage copied_runtime = {0};
    MeisoSessionConfig second_host_config;
    MeisoSessionConfig second_edge_config;
    MeisoSession *second_host = NULL;
    MeisoSession *second_edge = NULL;
    MeisoResourceDescriptor resolved;
    MeisoByteView routed_payload;
    uint8_t copied_payload[128];
    const MeisoResourceId undeclared = {
        UINT64_C(0x53455353494f4e52), 99U
    };

    CHECK(initialize_fixture(&fixture));

    CHECK(submit_before_ready_is_rejected(
        fixture.edge_session,
        fixture.state_descriptor.id,
        MEISO_RESOURCE_KIND_STATE,
        (MeisoByteView){state_bytes, sizeof(state_bytes)}
    ));
    CHECK(submit_before_ready_is_rejected(
        fixture.edge_session,
        fixture.stream_descriptor.id,
        MEISO_RESOURCE_KIND_STREAM,
        (MeisoByteView){stream_bytes, sizeof(stream_bytes)}
    ));
    CHECK(submit_before_ready_is_rejected(
        fixture.host_session,
        fixture.action_descriptor.id,
        MEISO_RESOURCE_KIND_ACTION,
        (MeisoByteView){action_bytes, sizeof(action_bytes)}
    ));
    CHECK(submit_before_ready_is_rejected(
        fixture.edge_session,
        fixture.blob_descriptor.id,
        MEISO_RESOURCE_KIND_BLOB,
        (MeisoByteView){blob_bytes, sizeof(blob_bytes)}
    ));
    CHECK(assert_no_delivery(&fixture));

    CHECK(make_ready(fixture.host_session, fixture.edge_session));
    CHECK(receive_envelope(
        &fixture,
        fixture.edge_session,
        fixture.host_session,
        undeclared,
        MEISO_RESOURCE_KIND_STATE,
        (MeisoByteView){state_bytes, sizeof(state_bytes)},
        &rejected_runtime
    ));
    CHECK(!route_runtime(
        &fixture,
        fixture.host_session,
        rejected_runtime,
        &resolved,
        &routed_payload
    ));
    CHECK(receive_envelope(
        &fixture,
        fixture.edge_session,
        fixture.host_session,
        fixture.state_descriptor.id,
        MEISO_RESOURCE_KIND_STREAM,
        (MeisoByteView){state_bytes, sizeof(state_bytes)},
        &rejected_runtime
    ));
    CHECK(!route_runtime(
        &fixture,
        fixture.host_session,
        rejected_runtime,
        &resolved,
        &routed_payload
    ));
    CHECK(assert_no_delivery(&fixture));

    CHECK(send_envelope(
        &fixture,
        fixture.edge_session,
        fixture.host_session,
        fixture.state_descriptor.id,
        MEISO_RESOURCE_KIND_STATE,
        (MeisoByteView){state_bytes, sizeof(state_bytes)}
    ));
    CHECK(send_envelope(
        &fixture,
        fixture.edge_session,
        fixture.host_session,
        fixture.stream_descriptor.id,
        MEISO_RESOURCE_KIND_STREAM,
        (MeisoByteView){stream_bytes, sizeof(stream_bytes)}
    ));
    CHECK(send_envelope(
        &fixture,
        fixture.host_session,
        fixture.edge_session,
        fixture.action_descriptor.id,
        MEISO_RESOURCE_KIND_ACTION,
        (MeisoByteView){action_bytes, sizeof(action_bytes)}
    ));
    CHECK(send_envelope(
        &fixture,
        fixture.edge_session,
        fixture.host_session,
        fixture.blob_descriptor.id,
        MEISO_RESOURCE_KIND_BLOB,
        (MeisoByteView){blob_bytes, sizeof(blob_bytes)}
    ));
    CHECK(assert_delivered(&fixture));

    CHECK(receive_envelope(
        &fixture,
        fixture.edge_session,
        fixture.host_session,
        fixture.state_descriptor.id,
        MEISO_RESOURCE_KIND_STATE,
        (MeisoByteView){state_bytes, sizeof(state_bytes)},
        &cached_runtime
    ));
    CHECK(cached_runtime.payload.size <= sizeof(copied_payload));
    memmove(
        copied_payload, cached_runtime.payload.data, cached_runtime.payload.size
    );
    copied_runtime.payload = (MeisoByteView){
        copied_payload, cached_runtime.payload.size
    };
    copied_runtime.session_id = cached_runtime.session_id;
    CHECK(route_runtime(
        &fixture,
        fixture.host_session,
        copied_runtime,
        &resolved,
        &routed_payload
    ));
    CHECK(close_pair(&fixture));
    CHECK(!route_runtime(
        &fixture,
        fixture.host_session,
        copied_runtime,
        &resolved,
        &routed_payload
    ));

    second_host_config = fixture.host_config;
    second_edge_config = fixture.edge_config;
    second_host_config.local_nonce.low += 1U;
    second_edge_config.local_nonce.low += 1U;
    CHECK(initialize_session(
        &second_host_arena, &second_host_config, &second_host
    ));
    CHECK(initialize_session(
        &second_edge_arena, &second_edge_config, &second_edge
    ));
    CHECK(make_ready(second_host, second_edge));
    CHECK(!route_runtime(
        &fixture,
        second_host,
        copied_runtime,
        &resolved,
        &routed_payload
    ));
    CHECK(submit_before_ready_is_rejected(
        fixture.edge_session,
        fixture.state_descriptor.id,
        MEISO_RESOURCE_KIND_STATE,
        (MeisoByteView){state_bytes, sizeof(state_bytes)}
    ));
    CHECK(submit_before_ready_is_rejected(
        fixture.edge_session,
        fixture.stream_descriptor.id,
        MEISO_RESOURCE_KIND_STREAM,
        (MeisoByteView){stream_bytes, sizeof(stream_bytes)}
    ));
    CHECK(submit_before_ready_is_rejected(
        fixture.host_session,
        fixture.action_descriptor.id,
        MEISO_RESOURCE_KIND_ACTION,
        (MeisoByteView){action_bytes, sizeof(action_bytes)}
    ));
    CHECK(submit_before_ready_is_rejected(
        fixture.edge_session,
        fixture.blob_descriptor.id,
        MEISO_RESOURCE_KIND_BLOB,
        (MeisoByteView){blob_bytes, sizeof(blob_bytes)}
    ));
    CHECK(assert_delivered(&fixture));
    return true;
}

int main(void)
{
    if (!test_session_gated_four_kind_delivery()) {
        return 1;
    }
    (void)puts("session four-kind fixture passed");
    return 0;
}
