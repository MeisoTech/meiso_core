#include "meiso_runtime.h"
#include "meiso_stream.h"

#include "meiso_test_support.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

typedef union {
    max_align_t alignment;
    uint8_t bytes[65536];
} TestArena;

typedef struct {
    TestArena host_session_arena;
    TestArena edge_session_arena;
    TestArena host_runtime_arena;
    TestArena edge_runtime_arena;
    MeisoSession *host_session;
    MeisoSession *edge_session;
    MeisoRuntime *host_runtime;
    MeisoRuntime *edge_runtime;
} RuntimePair;

static const MeisoEndpointId SENSOR_ENDPOINT = {0x101U, 0x102U};
static const MeisoResourceId SENSOR_STREAM = {0x201U, 0x202U};
static const MeisoSchemaRef CAPABILITY_SCHEMA = {{0x301U, 0x302U}, 1U};
static const MeisoSchemaRef STREAM_SCHEMA = {{0x401U, 0x402U}, 1U};
static const MeisoSchemaRef SELECTION_SCHEMA = {{0x501U, 0x502U}, 1U};

static void write_u64_be(uint8_t *target, uint64_t value)
{
    target[0] = (uint8_t)(value >> 56U);
    target[1] = (uint8_t)(value >> 48U);
    target[2] = (uint8_t)(value >> 40U);
    target[3] = (uint8_t)(value >> 32U);
    target[4] = (uint8_t)(value >> 24U);
    target[5] = (uint8_t)(value >> 16U);
    target[6] = (uint8_t)(value >> 8U);
    target[7] = (uint8_t)value;
}

static uint64_t read_u64_be(const uint8_t *source)
{
    return (uint64_t)source[0] << 56U |
        (uint64_t)source[1] << 48U |
        (uint64_t)source[2] << 40U |
        (uint64_t)source[3] << 32U |
        (uint64_t)source[4] << 24U |
        (uint64_t)source[5] << 16U |
        (uint64_t)source[6] << 8U |
        (uint64_t)source[7];
}

static MeisoSessionConfig session_config(
    MeisoSessionRole role,
    uint64_t nonce_salt
)
{
    MeisoSessionConfig config;
    const MeisoPeerId host_id = {0x11U, 0x12U};
    const MeisoPeerId edge_id = {0x21U, 0x22U};

    memset(&config, 0, sizeof(config));
    config.local_role = role;
    config.local_peer_id = role == MEISO_SESSION_ROLE_HOST ? host_id : edge_id;
    config.expected_peer_id =
        role == MEISO_SESSION_ROLE_HOST ? edge_id : host_id;
    config.local_incarnation =
        role == MEISO_SESSION_ROLE_HOST ? UINT64_C(31) : UINT64_C(41);
    config.local_nonce = role == MEISO_SESSION_ROLE_HOST ?
        (MeisoSessionNonce){UINT64_C(0x61), nonce_salt + 1U} :
        (MeisoSessionNonce){UINT64_C(0x71), nonce_salt + 2U};
    config.protocol = (MeisoProtocolVersion){1U, 0U};
    config.profile.id = (MeisoProfileId){0x81U, 0x82U};
    config.profile.version = 1U;
    config.limits = (MeisoSessionLimits){4096U, 2048U, 8U};
    return config;
}

static bool initialize_session(
    TestArena *arena,
    const MeisoSessionConfig *config,
    MeisoSession **out_session
)
{
    const size_t required = meiso_session_memory_required(config);

    CHECK(required > 0U && required <= sizeof(arena->bytes));
    CHECK(meiso_session_init(
        arena->bytes, required, config, out_session
    ) == MEISO_SESSION_INIT_OK);
    return true;
}

static bool deliver_handshake(MeisoSession *sender, MeisoSession *receiver)
{
    uint8_t frame[4096];
    MeisoByteView outbound;
    MeisoSessionRuntimeMessage ignored = {0};

    CHECK(meiso_session_next_outbound(sender, &outbound));
    CHECK(outbound.size <= sizeof(frame));
    memmove(frame, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(sender) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        receiver, (MeisoByteView){frame, outbound.size}, &ignored
    ) == MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS);
    return true;
}

static bool initialize_pair(RuntimePair *pair, uint64_t nonce_salt)
{
    MeisoSessionConfig host = session_config(
        MEISO_SESSION_ROLE_HOST, nonce_salt
    );
    MeisoSessionConfig edge = session_config(
        MEISO_SESSION_ROLE_EDGE, nonce_salt
    );
    MeisoRuntimeConfig runtime_config;
    size_t required;

    memset(pair, 0, sizeof(*pair));
    CHECK(initialize_session(
        &pair->host_session_arena, &host, &pair->host_session
    ));
    CHECK(initialize_session(
        &pair->edge_session_arena, &edge, &pair->edge_session
    ));
    CHECK(meiso_session_start(pair->edge_session) ==
          MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(pair->host_session) ==
          MEISO_SESSION_START_STARTED);
    CHECK(deliver_handshake(pair->host_session, pair->edge_session));
    CHECK(deliver_handshake(pair->edge_session, pair->host_session));
    CHECK(deliver_handshake(pair->host_session, pair->edge_session));
    CHECK(deliver_handshake(pair->edge_session, pair->host_session));

    memset(&runtime_config, 0, sizeof(runtime_config));
    runtime_config.max_endpoints = 2U;
    runtime_config.max_routes = 2U;
    runtime_config.max_capability_bytes = 32U;
    runtime_config.max_parameter_bytes_per_route = 16U;

    runtime_config.session = pair->host_session;
    required = meiso_runtime_memory_required(&runtime_config);
    CHECK(required > 0U && required <= sizeof(pair->host_runtime_arena.bytes));
    CHECK(meiso_runtime_init(
        pair->host_runtime_arena.bytes, required, &runtime_config,
        &pair->host_runtime
    ) == MEISO_RUNTIME_INIT_OK);
    runtime_config.session = pair->edge_session;
    required = meiso_runtime_memory_required(&runtime_config);
    CHECK(required > 0U && required <= sizeof(pair->edge_runtime_arena.bytes));
    CHECK(meiso_runtime_init(
        pair->edge_runtime_arena.bytes, required, &runtime_config,
        &pair->edge_runtime
    ) == MEISO_RUNTIME_INIT_OK);
    return true;
}

static bool receive_session_runtime(
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoSessionRuntimeMessage *out_message
)
{
    uint8_t frame[4096];
    MeisoByteView outbound;

    CHECK(meiso_session_next_outbound(sender, &outbound));
    CHECK(outbound.size <= sizeof(frame));
    memmove(frame, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(sender) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        receiver, (MeisoByteView){frame, outbound.size}, out_message
    ) == MEISO_SESSION_RECEIVE_RUNTIME);
    return true;
}

static bool transfer_runtime(
    MeisoSession *sender_session,
    MeisoSession *receiver_session,
    MeisoRuntime *receiver_runtime,
    MeisoRuntimeEvent *out_event
)
{
    MeisoSessionRuntimeMessage message = {0};

    CHECK(receive_session_runtime(
        sender_session, receiver_session, &message
    ));
    CHECK(meiso_runtime_receive(
        receiver_runtime, &message, out_event
    ) == MEISO_RUNTIME_OK);
    return true;
}

static void sensor_catalog(
    MeisoRuntimeEndpointDescriptor *endpoint,
    MeisoRuntimeRouteDescriptor *route
)
{
    static const uint8_t capability[] = {0xa1U, 0xa2U};

    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->id = SENSOR_ENDPOINT;
    endpoint->incarnation = 11U;
    endpoint->capability_schema = CAPABILITY_SCHEMA;
    endpoint->capability = (MeisoByteView){capability, sizeof(capability)};

    memset(route, 0, sizeof(*route));
    route->endpoint_id = SENSOR_ENDPOINT;
    route->endpoint_incarnation = 11U;
    route->resource.id = SENSOR_STREAM;
    route->resource.kind = MEISO_RESOURCE_KIND_STREAM;
    route->resource.schema = STREAM_SCHEMA;
    route->resource.owner = MEISO_OWNER_ROLE_EDGE;
    route->resource.max_payload_size = 64U;
    route->selection_schema = SELECTION_SCHEMA;
    route->max_selection_bytes = 8U;
    route->delivery = MEISO_RUNTIME_DELIVERY_HOST_SUBSCRIPTION;
}

static bool activate_catalog(RuntimePair *pair)
{
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor route;
    MeisoRuntimeEvent event;

    sensor_catalog(&endpoint, &route);
    CHECK(meiso_runtime_offer_catalog(
        pair->edge_runtime, 1U, &endpoint, 1U, &route, 1U
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair->edge_session, pair->host_session, pair->host_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED);
    CHECK(meiso_runtime_accept_catalog(pair->host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair->host_session, pair->edge_session, pair->edge_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED);
    return true;
}

static bool activate_route(RuntimePair *pair)
{
    static const uint8_t requested_rate[] = {60U};
    MeisoRuntimeSubscriptionDesired desired;
    MeisoRuntimeSubscriptionActual actual;
    MeisoRuntimeEvent event;

    memset(&desired, 0, sizeof(desired));
    desired.request_id = 101U;
    desired.endpoint_id = SENSOR_ENDPOINT;
    desired.endpoint_incarnation = 11U;
    desired.resource_id = SENSOR_STREAM;
    desired.enabled = true;
    desired.parameters =
        (MeisoByteView){requested_rate, sizeof(requested_rate)};
    CHECK(meiso_runtime_request_subscription(
        pair->host_runtime, &desired
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair->host_session, pair->edge_session, pair->edge_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_SUBSCRIPTION_DESIRED);

    memset(&actual, 0, sizeof(actual));
    actual.request_id = desired.request_id;
    actual.endpoint_id = SENSOR_ENDPOINT;
    actual.endpoint_incarnation = 11U;
    actual.resource_id = SENSOR_STREAM;
    actual.status = MEISO_RUNTIME_ACTUAL_ACTIVE;
    actual.route_epoch = 303U;
    actual.effective_parameters = desired.parameters;
    CHECK(meiso_runtime_report_subscription(
        pair->edge_runtime, &actual
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair->edge_session, pair->host_session, pair->host_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_SUBSCRIPTION_ACTUAL);
    CHECK(event.actual.status == MEISO_RUNTIME_ACTUAL_ACTIVE);
    return true;
}

static bool test_session_runtime_stream_chain(void)
{
    MEISO_TEST_ARENA_STORAGE RuntimePair pair;
    MEISO_TEST_ARENA_STORAGE TestArena stream_arena;
    const uint8_t sensor_payload[] = {0xd1U, 0xd2U, 0xd3U};
    uint8_t stream_wire[48U + sizeof(sensor_payload)];
    MeisoOwnerScope producer;
    MeisoRuntimeRouteDescriptor route;
    MeisoRuntimeEndpointDescriptor ignored_endpoint;
    MeisoRuntimeResourceMessage resource;
    MeisoRuntimeEvent event;
    MeisoSessionRuntimeMessage runtime_message = {0};
    MeisoStreamConfig stream_config;
    MeisoStream *stream = NULL;
    MeisoStreamSample sample;
    MeisoStreamSample observed;
    MeisoStreamReceiveResult result;
    uint8_t stale_generation_payload[2048];
    size_t required;

    CHECK(initialize_pair(&pair, 100U));
    CHECK(activate_catalog(&pair));
    CHECK(activate_route(&pair));
    sensor_catalog(&ignored_endpoint, &route);

    memset(&stream_config, 0, sizeof(stream_config));
    stream_config.ordering = MEISO_STREAM_ORDER_STRICT_MONOTONIC;
    stream_config.overflow = MEISO_STREAM_OVERFLOW_DROP_OLDEST;
    stream_config.deadline_policy = MEISO_STREAM_DEADLINE_UNSUPPORTED;
    stream_config.queue_slots = 2U;
    stream_config.history_slots = 2U;
    required = meiso_stream_memory_required(&route.resource, &stream_config);
    CHECK(required > 0U && required <= sizeof(stream_arena.bytes));
    CHECK(meiso_stream_init(
        stream_arena.bytes, required, &route.resource, &stream_config, &stream
    ) == MEISO_STREAM_INIT_OK);
    memset(stream_wire, 0, sizeof(stream_wire));
    stream_wire[0] = MEISO_OWNER_ROLE_EDGE;
    write_u64_be(stream_wire + 8U, 41U);
    write_u64_be(stream_wire + 16U, 701U);
    write_u64_be(stream_wire + 24U, 5U);
    write_u64_be(stream_wire + 32U, 1U);
    write_u64_be(stream_wire + 40U, 1000U);
    memmove(stream_wire + 48U, sensor_payload, sizeof(sensor_payload));

    memset(&resource, 0, sizeof(resource));
    resource.endpoint_id = SENSOR_ENDPOINT;
    resource.endpoint_incarnation = 11U;
    resource.resource_id = SENSOR_STREAM;
    resource.kind = MEISO_RESOURCE_KIND_STREAM;
    resource.schema = STREAM_SCHEMA;
    resource.request_id = 101U;
    resource.route_epoch = 303U;
    resource.payload = (MeisoByteView){stream_wire, sizeof(stream_wire)};
    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &resource
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_session_runtime(
        pair.edge_session, pair.host_session, &runtime_message
    ));
    CHECK(runtime_message.payload.size >= MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U);
    CHECK(runtime_message.payload.size <= sizeof(stale_generation_payload));
    memmove(
        stale_generation_payload, runtime_message.payload.data,
        runtime_message.payload.size
    );
    write_u64_be(
        stale_generation_payload + MEISO_RUNTIME_WIRE_HEADER_SIZE, 2U
    );
    runtime_message.payload = (MeisoByteView){
        stale_generation_payload, runtime_message.payload.size
    };
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &runtime_message, &event
    ) == MEISO_RUNTIME_STALE_CATALOG);

    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &resource
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair.edge_session, pair.host_session, pair.host_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_RESOURCE);
    CHECK(event.resource.request_id == 101U);
    CHECK(event.resource.route_epoch == 303U);
    CHECK(event.resource.payload.size == sizeof(stream_wire));
    CHECK(event.resource.payload.data[0] == MEISO_OWNER_ROLE_EDGE);
    CHECK(event.resource.payload.data[1] == 0U);

    producer.role = event.resource.payload.data[0];
    producer.incarnation = read_u64_be(event.resource.payload.data + 8U);
    CHECK(meiso_stream_begin_producer(
        stream, producer, read_u64_be(event.resource.payload.data + 16U)
    ) == MEISO_STREAM_PRODUCER_STARTED);

    memset(&sample, 0, sizeof(sample));
    sample.key.producer = producer;
    sample.key.epoch = read_u64_be(event.resource.payload.data + 16U);
    sample.key.sequence = read_u64_be(event.resource.payload.data + 24U);
    sample.source_time.clock_epoch =
        read_u64_be(event.resource.payload.data + 32U);
    sample.source_time.monotonic_ns =
        read_u64_be(event.resource.payload.data + 40U);
    sample.payload.data = event.resource.payload.data + 48U;
    sample.payload.size = event.resource.payload.size - 48U;
    result = meiso_stream_receive(stream, &sample, NULL);
    CHECK(result.code == MEISO_STREAM_RECEIVE_ACCEPTED);
    CHECK((result.flags & MEISO_STREAM_RECEIVE_FLAG_GAP) != 0U);
    CHECK(result.missing_before == 4U);
    CHECK(meiso_stream_peek(stream, &observed));
    CHECK(observed.key.sequence == 5U);
    CHECK(observed.payload.size == sizeof(sensor_payload));
    CHECK(memcmp(
        observed.payload.data, sensor_payload, sizeof(sensor_payload)
    ) == 0);
    return true;
}

static bool test_new_session_rejects_old_runtime_message(void)
{
    MEISO_TEST_ARENA_STORAGE RuntimePair first;
    MEISO_TEST_ARENA_STORAGE RuntimePair second;
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor route;
    MeisoSessionRuntimeMessage first_message = {0};
    MeisoSessionRuntimeMessage copied_message;
    MeisoRuntimeEvent event;
    MeisoRuntimeSnapshot snapshot;
    uint8_t copied_payload[2048];

    CHECK(initialize_pair(&first, 200U));
    sensor_catalog(&endpoint, &route);
    CHECK(meiso_runtime_offer_catalog(
        first.edge_runtime, 1U, &endpoint, 1U, &route, 1U
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_session_runtime(
        first.edge_session, first.host_session, &first_message
    ));
    CHECK(first_message.payload.size <= sizeof(copied_payload));
    memmove(copied_payload, first_message.payload.data, first_message.payload.size);
    copied_message = first_message;
    copied_message.payload =
        (MeisoByteView){copied_payload, first_message.payload.size};

    CHECK(initialize_pair(&second, 300U));
    CHECK(meiso_runtime_receive(
        second.host_runtime, &copied_message, &event
    ) == MEISO_RUNTIME_STALE_SESSION);
    CHECK(meiso_runtime_snapshot(second.host_runtime, &snapshot));
    CHECK(snapshot.core_ready);
    CHECK(!snapshot.active_catalog_present);
    CHECK(!snapshot.staged_catalog_present);

    CHECK(activate_catalog(&second));
    CHECK(meiso_runtime_snapshot(second.host_runtime, &snapshot));
    CHECK(snapshot.active_catalog_present);
    CHECK(snapshot.active_catalog_generation == 1U);
    return true;
}

int main(void)
{
    if (!test_session_runtime_stream_chain() ||
        !test_new_session_rejects_old_runtime_message()) {
        return 1;
    }
    (void)printf("runtime session integration tests passed\n");
    return 0;
}
