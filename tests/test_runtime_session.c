#include <meiso/runtime/runtime.h>
#include <meiso/profiles/wifi_telemetry.h>

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
    uint8_t transfer[8192];
    MeisoSession *host_session;
    MeisoSession *edge_session;
    MeisoRuntime *host_runtime;
    MeisoRuntime *edge_runtime;
} RuntimePair;

typedef struct {
    TestArena desired_publisher_arena;
    TestArena desired_replica_arena;
    TestArena actual_publisher_arena;
    TestArena actual_replica_arena;
    TestArena stream_arena;
    MeisoStatePublisher *desired_publisher;
    MeisoStateReplica *desired_replica;
    MeisoStatePublisher *actual_publisher;
    MeisoStateReplica *actual_replica;
    MeisoStream *stream;
} Resources;

static const MeisoEndpointId ENDPOINT = {0x101U, 0x102U};
static const uint64_t ENDPOINT_INCARNATION = 7U;
static const MeisoResourceId DESIRED_ID = {0x201U, 0x202U};
static const MeisoResourceId ACTUAL_ID = {0x211U, 0x212U};
static const MeisoResourceId DATA_ID = {0x221U, 0x222U};
static const MeisoSchemaRef CAPABILITY_SCHEMA = {{0x301U, 0x302U}, 1U};
static const MeisoSchemaRef DESIRED_SCHEMA = {{0x311U, 0x312U}, 1U};
static const MeisoSchemaRef ACTUAL_SCHEMA = {{0x321U, 0x322U}, 1U};
static const MeisoSchemaRef DATA_SCHEMA = {{0x331U, 0x332U}, 1U};
static const MeisoOwnerScope HOST_OWNER = {MEISO_OWNER_ROLE_HOST, 31U};
static const MeisoOwnerScope EDGE_OWNER = {MEISO_OWNER_ROLE_EDGE, 41U};

static MeisoResourceDescriptor descriptor(
    MeisoResourceId id,
    MeisoResourceKind kind,
    MeisoSchemaRef schema,
    MeisoOwnerRole owner,
    size_t maximum
)
{
    const MeisoResourceDescriptor value = {id, kind, schema, owner, maximum};
    return value;
}

static MeisoSessionConfig session_config(
    MeisoSessionRole role,
    uint64_t nonce_salt
)
{
    MeisoSessionConfig config;
    const MeisoPeerId host = {0x11U, 0x12U};
    const MeisoPeerId edge = {0x21U, 0x22U};

    memset(&config, 0, sizeof(config));
    config.local_role = role;
    config.local_peer_id = role == MEISO_SESSION_ROLE_HOST ? host : edge;
    config.expected_peer_id = role == MEISO_SESSION_ROLE_HOST ? edge : host;
    config.local_incarnation = role == MEISO_SESSION_ROLE_HOST ? 31U : 41U;
    config.local_nonce = role == MEISO_SESSION_ROLE_HOST ?
        (MeisoSessionNonce){0x61U, nonce_salt + 1U} :
        (MeisoSessionNonce){0x71U, nonce_salt + 2U};
    config.protocol = (MeisoProtocolVersion){1U, 0U};
    config.profile.id = (MeisoProfileId){0x81U, 0x82U};
    config.profile.version = 1U;
    config.limits = (MeisoSessionLimits){8192U, 4096U, 32U};
    return config;
}

static bool init_session(
    TestArena *arena,
    const MeisoSessionConfig *config,
    MeisoSession **out_session
)
{
    const size_t size = meiso_session_memory_required(config);

    CHECK(size != 0U && size <= sizeof(arena->bytes));
    CHECK(meiso_session_init(
        arena->bytes, size, config, out_session
    ) == MEISO_SESSION_INIT_OK);
    return true;
}

static bool handshake(MeisoSession *sender, MeisoSession *receiver)
{
    uint8_t frame[8192];
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

static bool init_runtime(
    TestArena *arena,
    MeisoSession *session,
    MeisoRuntime **out_runtime
)
{
    MeisoRuntimeConfig config;
    size_t size;

    memset(&config, 0, sizeof(config));
    config.session = session;
    config.max_endpoints = 4U;
    config.max_endpoint_history = 8U;
    config.max_routes = 12U;
    config.max_capability_bytes = 128U;
    config.max_desired_profile_bytes_per_endpoint = 128U;
    config.max_actual_profile_bytes_per_endpoint = 128U;
    size = meiso_runtime_memory_required(&config);
    CHECK(size != 0U && size <= sizeof(arena->bytes));
    CHECK(meiso_runtime_init(
        arena->bytes, size, &config, out_runtime
    ) == MEISO_RUNTIME_INIT_OK);
    return true;
}

static bool init_pair(RuntimePair *pair, uint64_t nonce_salt)
{
    MeisoSessionConfig host = session_config(
        MEISO_SESSION_ROLE_HOST, nonce_salt
    );
    MeisoSessionConfig edge = session_config(
        MEISO_SESSION_ROLE_EDGE, nonce_salt
    );

    memset(pair, 0, sizeof(*pair));
    CHECK(init_session(
        &pair->host_session_arena, &host, &pair->host_session
    ));
    CHECK(init_session(
        &pair->edge_session_arena, &edge, &pair->edge_session
    ));
    CHECK(meiso_session_start(pair->edge_session) ==
          MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(pair->host_session) ==
          MEISO_SESSION_START_STARTED);
    CHECK(handshake(pair->host_session, pair->edge_session));
    CHECK(handshake(pair->edge_session, pair->host_session));
    CHECK(handshake(pair->host_session, pair->edge_session));
    CHECK(handshake(pair->edge_session, pair->host_session));
    CHECK(init_runtime(
        &pair->host_runtime_arena,
        pair->host_session,
        &pair->host_runtime
    ));
    CHECK(init_runtime(
        &pair->edge_runtime_arena,
        pair->edge_session,
        &pair->edge_runtime
    ));
    return true;
}

static bool transfer(
    RuntimePair *pair,
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoRuntime *receiver_runtime,
    MeisoRuntimeEvent *event,
    MeisoRuntimeStatus expected
)
{
    MeisoByteView outbound;
    MeisoSessionRuntimeMessage message = {0};

    CHECK(meiso_session_next_outbound(sender, &outbound));
    CHECK(outbound.size <= sizeof(pair->transfer));
    memmove(pair->transfer, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(sender) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        receiver,
        (MeisoByteView){pair->transfer, outbound.size},
        &message
    ) == MEISO_SESSION_RECEIVE_RUNTIME);
    CHECK(meiso_runtime_receive(receiver_runtime, &message, event) == expected);
    return true;
}

static void catalog(
    MeisoRuntimeEndpointDescriptor *endpoint,
    MeisoRuntimeRouteDescriptor routes[3]
)
{
    static const uint8_t capability[] = {0xa1U, 0xa2U, 0xa3U};

    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->id = ENDPOINT;
    endpoint->incarnation = ENDPOINT_INCARNATION;
    endpoint->capability_schema = CAPABILITY_SCHEMA;
    endpoint->capability = (MeisoByteView){capability, sizeof(capability)};
    memset(routes, 0, 3U * sizeof(*routes));
    routes[0] = (MeisoRuntimeRouteDescriptor){
        ENDPOINT, ENDPOINT_INCARNATION,
        {DESIRED_ID, MEISO_RESOURCE_KIND_STATE, DESIRED_SCHEMA,
         MEISO_OWNER_ROLE_HOST, 64U},
        MEISO_RUNTIME_FACET_DESIRED
    };
    routes[1] = (MeisoRuntimeRouteDescriptor){
        ENDPOINT, ENDPOINT_INCARNATION,
        {ACTUAL_ID, MEISO_RESOURCE_KIND_STATE, ACTUAL_SCHEMA,
         MEISO_OWNER_ROLE_EDGE, 128U},
        MEISO_RUNTIME_FACET_ACTUAL
    };
    routes[2] = (MeisoRuntimeRouteDescriptor){
        ENDPOINT, ENDPOINT_INCARNATION,
        {DATA_ID, MEISO_RESOURCE_KIND_STREAM, DATA_SCHEMA,
         MEISO_OWNER_ROLE_EDGE, 64U},
        MEISO_RUNTIME_FACET_DATA
    };
}

static bool activate(RuntimePair *pair)
{
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor routes[3];
    MeisoRuntimeEvent event;

    catalog(&endpoint, routes);
    CHECK(meiso_runtime_offer_catalog(
        pair->edge_runtime, 1U, &endpoint, 1U, routes, 3U
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer(
        pair, pair->edge_session, pair->host_session, pair->host_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(meiso_runtime_accept_catalog(pair->host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(transfer(
        pair, pair->host_session, pair->edge_session, pair->edge_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    return true;
}

static MeisoRuntimeRouteKey route_key(
    MeisoResourceId resource,
    MeisoRuntimeFacet facet
)
{
    const MeisoRuntimeRouteKey value = {
        1U, ENDPOINT, ENDPOINT_INCARNATION, resource, facet
    };
    return value;
}

static bool init_resources(Resources *resources)
{
    MeisoResourceDescriptor desired = descriptor(
        DESIRED_ID, MEISO_RESOURCE_KIND_STATE, DESIRED_SCHEMA,
        MEISO_OWNER_ROLE_HOST, 64U
    );
    MeisoResourceDescriptor actual = descriptor(
        ACTUAL_ID, MEISO_RESOURCE_KIND_STATE, ACTUAL_SCHEMA,
        MEISO_OWNER_ROLE_EDGE, 128U
    );
    MeisoResourceDescriptor data = descriptor(
        DATA_ID, MEISO_RESOURCE_KIND_STREAM, DATA_SCHEMA,
        MEISO_OWNER_ROLE_EDGE, 64U
    );
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const MeisoStreamConfig stream_config = {
        MEISO_STREAM_ORDER_STRICT_MONOTONIC,
        MEISO_STREAM_OVERFLOW_DROP_OLDEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        8U,
        4U,
        16U
    };
    size_t size;

    memset(resources, 0, sizeof(*resources));
    size = meiso_state_publisher_memory_required(&desired, &state_config);
    CHECK(size != 0U && size <= sizeof(resources->desired_publisher_arena.bytes));
    CHECK(meiso_state_publisher_init(
        resources->desired_publisher_arena.bytes, size, &desired,
        &state_config, &resources->desired_publisher
    ) == MEISO_STATE_INIT_OK);
    size = meiso_state_replica_memory_required(&desired);
    CHECK(size != 0U && size <= sizeof(resources->desired_replica_arena.bytes));
    CHECK(meiso_state_replica_init(
        resources->desired_replica_arena.bytes, size, &desired,
        &resources->desired_replica
    ) == MEISO_STATE_INIT_OK);
    size = meiso_state_publisher_memory_required(&actual, &state_config);
    CHECK(size != 0U && size <= sizeof(resources->actual_publisher_arena.bytes));
    CHECK(meiso_state_publisher_init(
        resources->actual_publisher_arena.bytes, size, &actual,
        &state_config, &resources->actual_publisher
    ) == MEISO_STATE_INIT_OK);
    size = meiso_state_replica_memory_required(&actual);
    CHECK(size != 0U && size <= sizeof(resources->actual_replica_arena.bytes));
    CHECK(meiso_state_replica_init(
        resources->actual_replica_arena.bytes, size, &actual,
        &resources->actual_replica
    ) == MEISO_STATE_INIT_OK);
    size = meiso_stream_memory_required(&data, &stream_config);
    CHECK(size != 0U && size <= sizeof(resources->stream_arena.bytes));
    CHECK(meiso_stream_init(
        resources->stream_arena.bytes, size, &data, &stream_config,
        &resources->stream
    ) == MEISO_STREAM_INIT_OK);
    CHECK(meiso_state_publisher_begin_delivery(
        resources->desired_publisher, (MeisoDeliveryGeneration){101U}
    ) == MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_publisher_begin_delivery(
        resources->actual_publisher, (MeisoDeliveryGeneration){202U}
    ) == MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_replica_begin_owner(
        resources->desired_replica, HOST_OWNER
    ) == MEISO_STATE_OWNER_STARTED);
    CHECK(meiso_state_replica_begin_owner(
        resources->actual_replica, EDGE_OWNER
    ) == MEISO_STATE_OWNER_STARTED);
    return true;
}

static bool deliver_state(
    RuntimePair *pair,
    MeisoStatePublisher *publisher,
    MeisoStateReplica *replica,
    MeisoRuntime *sender_runtime,
    MeisoSession *sender_session,
    MeisoRuntime *receiver_runtime,
    MeisoSession *receiver_session,
    MeisoRuntimeRouteKey key,
    bool actual
)
{
    MeisoStateOffer offer;
    MeisoRuntimeEvent event;
    MeisoStateReceiveStatus receive_status;

    CHECK(meiso_state_publisher_next_offer(publisher, &offer));
    CHECK(meiso_runtime_send_state_offer(
        sender_runtime, key, &offer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer(
        pair, sender_session, receiver_session, receiver_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    receive_status = meiso_state_replica_receive(
        replica,
        event.state_offer.state.owner,
        event.state_offer.state.version,
        event.state_offer.state.payload
    );
    CHECK(receive_status == MEISO_STATE_RECEIVE_NEW);
    (void)actual;
    CHECK(meiso_runtime_commit_state_receive(
        receiver_runtime, event.route, &event.state_offer, receive_status
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_state_accept(
        receiver_runtime,
        event.route,
        event.state_offer.generation,
        event.state_offer.state.owner,
        event.state_offer.state.version
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer(
        pair, receiver_session, sender_session, sender_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT);
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        publisher,
        event.state_accept_generation,
        event.state_accept_owner,
        event.state_accept_version
    ) == MEISO_STATE_ACCEPT_CONFIRMED);
    return true;
}

static bool publish_desired(
    Resources *resources,
    uint64_t version,
    bool enabled,
    const uint8_t *profile,
    size_t profile_size,
    uint8_t output[64]
)
{
    MeisoRuntimeDesiredState value = {
        enabled, {profile, profile_size}
    };
    MeisoByteView payload;

    CHECK(meiso_runtime_desired_state_encode(
        &value, (MeisoMutableBuffer){output, 64U}, &payload
    ));
    CHECK(meiso_state_publisher_publish(
        resources->desired_publisher, HOST_OWNER, version, payload
    ) == MEISO_STATE_PUBLISH_NEW);
    return true;
}

static MeisoRuntimeDesiredReference desired_ref(uint64_t version)
{
    const MeisoRuntimeDesiredReference value = {true, HOST_OWNER, version};
    return value;
}

static bool publish_actual_with_profile(
    Resources *resources,
    uint64_t version,
    MeisoRuntimeDesiredReference observed,
    MeisoRuntimeDesiredReference applied,
    MeisoRuntimeApplyResult result,
    MeisoRuntimeAvailability availability,
    bool enabled,
    MeisoRuntimeProductionStatus production_status,
    MeisoRuntimeProductionReason reason,
    uint64_t epoch,
    const uint8_t *profile,
    size_t profile_size,
    uint8_t output[128]
)
{
    MeisoRuntimeActualState value;
    MeisoByteView payload;

    memset(&value, 0, sizeof(value));
    value.observed_desired = observed;
    value.applied_desired = applied;
    value.apply_result = result;
    value.availability = availability;
    value.effective_enabled = enabled;
    value.production_status = production_status;
    value.reason = reason;
    value.production_epoch = epoch;
    value.profile = (MeisoByteView){profile, profile_size};
    CHECK(meiso_runtime_actual_state_encode(
        &value, (MeisoMutableBuffer){output, 128U}, &payload
    ));
    CHECK(meiso_state_publisher_publish(
        resources->actual_publisher, EDGE_OWNER, version, payload
    ) == MEISO_STATE_PUBLISH_NEW);
    return true;
}

static bool publish_actual(
    Resources *resources,
    uint64_t version,
    MeisoRuntimeDesiredReference observed,
    MeisoRuntimeDesiredReference applied,
    MeisoRuntimeApplyResult result,
    MeisoRuntimeAvailability availability,
    bool enabled,
    MeisoRuntimeProductionStatus production_status,
    MeisoRuntimeProductionReason reason,
    uint64_t epoch,
    uint8_t output[128]
)
{
    return publish_actual_with_profile(
        resources, version, observed, applied, result, availability,
        enabled, production_status, reason, epoch, NULL, 0U, output
    );
}

static bool encode_desired_parameters(
    MeisoWifiTelemetryDesiredParameters parameters,
    uint8_t output[MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE]
)
{
    size_t size = 0U;

    CHECK(meiso_wifi_telemetry_desired_parameters_encode(
        &parameters,
        (MeisoMutableBuffer){
            output, MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE
        },
        &size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(size == MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE);
    return true;
}

static bool encode_effective_parameters(
    MeisoWifiTelemetryEffectiveParameters parameters,
    uint8_t output[MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE]
)
{
    size_t size = 0U;

    CHECK(meiso_wifi_telemetry_effective_parameters_encode(
        &parameters,
        (MeisoMutableBuffer){
            output, MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE
        },
        &size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(size == MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE);
    return true;
}

static bool sessions_ready(const RuntimePair *pair)
{
    MeisoSessionSnapshot host;
    MeisoSessionSnapshot edge;

    CHECK(meiso_session_snapshot(pair->host_session, &host));
    CHECK(meiso_session_snapshot(pair->edge_session, &edge));
    CHECK(host.state == MEISO_SESSION_STATE_READY);
    CHECK(edge.state == MEISO_SESSION_STATE_READY);
    return true;
}

static bool normal_close(RuntimePair *pair)
{
    MeisoByteView outbound;
    MeisoSessionRuntimeMessage ignored = {0};
    MeisoSessionSnapshot host;
    MeisoSessionSnapshot edge;

    CHECK(meiso_session_close(
        pair->host_session, MEISO_SESSION_CLOSE_NORMAL
    ) == MEISO_SESSION_CLOSE_QUEUED);
    CHECK(meiso_session_next_outbound(pair->host_session, &outbound));
    CHECK(outbound.size <= sizeof(pair->transfer));
    memmove(pair->transfer, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(pair->host_session) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        pair->edge_session,
        (MeisoByteView){pair->transfer, outbound.size},
        &ignored
    ) == MEISO_SESSION_RECEIVE_PEER_CLOSED);
    CHECK(meiso_session_snapshot(pair->host_session, &host));
    CHECK(meiso_session_snapshot(pair->edge_session, &edge));
    CHECK(host.state == MEISO_SESSION_STATE_CLOSED);
    CHECK(edge.state == MEISO_SESSION_STATE_CLOSED);
    CHECK(host.local_close_reason_known);
    CHECK(host.local_close_reason == MEISO_SESSION_CLOSE_NORMAL);
    CHECK(edge.peer_close_reason_known);
    CHECK(edge.peer_close_reason == MEISO_SESSION_CLOSE_NORMAL);
    return true;
}

static MeisoStreamSample stream_sample(
    uint64_t stream_epoch,
    uint64_t sequence,
    const uint8_t *dependency,
    size_t dependency_size,
    const uint8_t *payload,
    size_t payload_size
)
{
    MeisoStreamSample sample;

    memset(&sample, 0, sizeof(sample));
    sample.key.producer = EDGE_OWNER;
    sample.key.epoch = stream_epoch;
    sample.key.sequence = sequence;
    sample.source_time = (MeisoTimePoint){9U, sequence * 100U};
    sample.dependency = (MeisoByteView){dependency, dependency_size};
    sample.payload = (MeisoByteView){payload, payload_size};
    return sample;
}

static bool send_stream(
    RuntimePair *pair,
    Resources *resources,
    uint64_t production_epoch,
    const MeisoStreamSample *sample,
    bool reset_producer
)
{
    MeisoRuntimeEvent event;
    MeisoStreamReceiveResult result;

    CHECK(meiso_runtime_send_stream_sample(
        pair->edge_runtime,
        route_key(DATA_ID, MEISO_RUNTIME_FACET_DATA),
        production_epoch,
        sample
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer(
        pair, pair->edge_session, pair->host_session, pair->host_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE);
    if (reset_producer) {
        const MeisoStreamProducerStatus producer = meiso_stream_begin_producer(
            resources->stream,
            event.stream_sample.key.producer,
            event.stream_sample.key.epoch
        );
        CHECK(producer == MEISO_STREAM_PRODUCER_STARTED ||
              producer == MEISO_STREAM_PRODUCER_SWITCHED);
    }
    result = meiso_stream_receive(resources->stream, &event.stream_sample, NULL);
    CHECK(result.code == MEISO_STREAM_RECEIVE_ACCEPTED);
    return true;
}

static bool test_state_stream_integration(void)
{
    RuntimePair pair;
    Resources resources;
    uint8_t desired_bytes[3][64];
    uint8_t actual_bytes[5][128];
    const uint8_t rate_low[] = {10U};
    const uint8_t rate_high[] = {50U, 2U};
    const uint8_t dependency[] = {0xd1U, 0xd2U};
    const uint8_t payload[] = {0x41U, 0x42U, 0x43U};
    MeisoStreamSample sample;
    MeisoStreamSample observed;
    MeisoRuntimeProductionSnapshot production;

    CHECK(init_pair(&pair, 0U));
    CHECK(activate(&pair));
    CHECK(init_resources(&resources));

    CHECK(publish_desired(
        &resources, 1U, true, rate_low, sizeof(rate_low), desired_bytes[0]
    ));
    CHECK(deliver_state(
        &pair, resources.desired_publisher, resources.desired_replica,
        pair.host_runtime, pair.host_session,
        pair.edge_runtime, pair.edge_session,
        route_key(DESIRED_ID, MEISO_RUNTIME_FACET_DESIRED), false
    ));
    CHECK(publish_actual(
        &resources, 1U, desired_ref(1U), desired_ref(1U),
        MEISO_RUNTIME_APPLY_APPLIED,
        MEISO_RUNTIME_AVAILABILITY_PRESENT,
        true, MEISO_RUNTIME_PRODUCTION_RUNNING,
        MEISO_RUNTIME_PRODUCTION_REASON_NONE, 10U, actual_bytes[0]
    ));
    CHECK(deliver_state(
        &pair, resources.actual_publisher, resources.actual_replica,
        pair.edge_runtime, pair.edge_session,
        pair.host_runtime, pair.host_session,
        route_key(ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL), true
    ));
    CHECK(meiso_runtime_production_snapshot(
        pair.host_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(production.data_active && production.production_epoch == 10U);

    sample = stream_sample(
        1U, 1U, dependency, sizeof(dependency), payload, sizeof(payload)
    );
    CHECK(send_stream(&pair, &resources, 10U, &sample, true));
    CHECK(meiso_stream_peek(resources.stream, &observed));
    CHECK(observed.key.sequence == 1U);
    CHECK(observed.source_time.monotonic_ns == 100U);
    CHECK(observed.dependency.size == sizeof(dependency));
    CHECK(memcmp(
        observed.payload.data, payload, sizeof(payload)
    ) == 0);

    CHECK(publish_desired(
        &resources, 2U, false, NULL, 0U, desired_bytes[1]
    ));
    CHECK(deliver_state(
        &pair, resources.desired_publisher, resources.desired_replica,
        pair.host_runtime, pair.host_session,
        pair.edge_runtime, pair.edge_session,
        route_key(DESIRED_ID, MEISO_RUNTIME_FACET_DESIRED), false
    ));
    CHECK(meiso_runtime_production_snapshot(
        pair.edge_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(production.data_active);
    sample = stream_sample(1U, 2U, NULL, 0U, payload, sizeof(payload));
    CHECK(send_stream(&pair, &resources, 10U, &sample, false));

    CHECK(publish_actual(
        &resources, 2U, desired_ref(2U), desired_ref(1U),
        MEISO_RUNTIME_APPLY_PENDING,
        MEISO_RUNTIME_AVAILABILITY_PRESENT,
        true, MEISO_RUNTIME_PRODUCTION_STOPPING,
        MEISO_RUNTIME_PRODUCTION_REASON_APPLYING, 10U, actual_bytes[1]
    ));
    CHECK(deliver_state(
        &pair, resources.actual_publisher, resources.actual_replica,
        pair.edge_runtime, pair.edge_session,
        pair.host_runtime, pair.host_session,
        route_key(ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL), true
    ));
    sample = stream_sample(1U, 3U, NULL, 0U, payload, sizeof(payload));
    CHECK(send_stream(&pair, &resources, 10U, &sample, false));

    CHECK(publish_actual(
        &resources, 3U, desired_ref(2U), desired_ref(2U),
        MEISO_RUNTIME_APPLY_APPLIED,
        MEISO_RUNTIME_AVAILABILITY_PRESENT,
        false, MEISO_RUNTIME_PRODUCTION_STOPPED,
        MEISO_RUNTIME_PRODUCTION_REASON_DISABLED, 0U, actual_bytes[2]
    ));
    CHECK(deliver_state(
        &pair, resources.actual_publisher, resources.actual_replica,
        pair.edge_runtime, pair.edge_session,
        pair.host_runtime, pair.host_session,
        route_key(ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL), true
    ));
    CHECK(!meiso_runtime_production_snapshot(
        pair.host_runtime, ENDPOINT, &production
    ) || !production.data_active);
    CHECK(meiso_runtime_send_stream_sample(
        pair.edge_runtime,
        route_key(DATA_ID, MEISO_RUNTIME_FACET_DATA),
        10U,
        &sample
    ) == MEISO_RUNTIME_ROUTE_NOT_ACTIVE);

    CHECK(publish_desired(
        &resources, 3U, true, rate_high, sizeof(rate_high), desired_bytes[2]
    ));
    CHECK(deliver_state(
        &pair, resources.desired_publisher, resources.desired_replica,
        pair.host_runtime, pair.host_session,
        pair.edge_runtime, pair.edge_session,
        route_key(DESIRED_ID, MEISO_RUNTIME_FACET_DESIRED), false
    ));
    CHECK(publish_actual(
        &resources, 4U, desired_ref(3U), desired_ref(2U),
        MEISO_RUNTIME_APPLY_PENDING,
        MEISO_RUNTIME_AVAILABILITY_PRESENT,
        false, MEISO_RUNTIME_PRODUCTION_STARTING,
        MEISO_RUNTIME_PRODUCTION_REASON_APPLYING, 0U, actual_bytes[3]
    ));
    CHECK(deliver_state(
        &pair, resources.actual_publisher, resources.actual_replica,
        pair.edge_runtime, pair.edge_session,
        pair.host_runtime, pair.host_session,
        route_key(ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL), true
    ));
    CHECK(publish_actual(
        &resources, 5U, desired_ref(3U), desired_ref(3U),
        MEISO_RUNTIME_APPLY_APPLIED,
        MEISO_RUNTIME_AVAILABILITY_PRESENT,
        true, MEISO_RUNTIME_PRODUCTION_RUNNING,
        MEISO_RUNTIME_PRODUCTION_REASON_NONE, 11U, actual_bytes[4]
    ));
    CHECK(deliver_state(
        &pair, resources.actual_publisher, resources.actual_replica,
        pair.edge_runtime, pair.edge_session,
        pair.host_runtime, pair.host_session,
        route_key(ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL), true
    ));
    sample = stream_sample(2U, 1U, NULL, 0U, payload, sizeof(payload));
    CHECK(meiso_runtime_send_stream_sample(
        pair.edge_runtime,
        route_key(DATA_ID, MEISO_RUNTIME_FACET_DATA),
        10U,
        &sample
    ) == MEISO_RUNTIME_PRODUCTION_EPOCH_MISMATCH);
    CHECK(send_stream(&pair, &resources, 11U, &sample, true));
    return true;
}

static bool test_continuous_session_reconfigure(void)
{
    RuntimePair pair;
    Resources resources;
    uint8_t desired_state_bytes[4][64];
    uint8_t actual_state_bytes[7][128];
    uint8_t desired_low[
        MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE
    ];
    uint8_t desired_high[
        MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE
    ];
    uint8_t effective_low[
        MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE
    ];
    uint8_t effective_high[
        MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE
    ];
    uint8_t effective_stopped[
        MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE
    ];
    const MeisoWifiTelemetryFieldMask low_fields =
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
        MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM;
    const MeisoWifiTelemetryFieldMask high_fields =
        low_fields |
        MEISO_WIFI_TELEMETRY_FIELD_RX_BYTES |
        MEISO_WIFI_TELEMETRY_FIELD_TX_BYTES;
    const uint8_t payload[] = {0x41U, 0x42U, 0x43U};
    MeisoRuntimeProductionSnapshot production;
    MeisoStreamSample sample;
    MeisoStreamSample materialized_before_reconfigure;
    MeisoStreamSample materialized_before_stop;

    CHECK(init_pair(&pair, 200U));
    CHECK(activate(&pair));
    CHECK(init_resources(&resources));
    CHECK(encode_desired_parameters(
        (MeisoWifiTelemetryDesiredParameters){low_fields, 1000U},
        desired_low
    ));
    CHECK(encode_desired_parameters(
        (MeisoWifiTelemetryDesiredParameters){high_fields, 200U},
        desired_high
    ));
    CHECK(encode_effective_parameters(
        (MeisoWifiTelemetryEffectiveParameters){low_fields, 1000U},
        effective_low
    ));
    CHECK(encode_effective_parameters(
        (MeisoWifiTelemetryEffectiveParameters){high_fields, 200U},
        effective_high
    ));
    CHECK(encode_effective_parameters(
        (MeisoWifiTelemetryEffectiveParameters){0U, 0U},
        effective_stopped
    ));

    CHECK(publish_desired(
        &resources, 1U, true,
        desired_low, sizeof(desired_low), desired_state_bytes[0]
    ));
    CHECK(deliver_state(
        &pair, resources.desired_publisher, resources.desired_replica,
        pair.host_runtime, pair.host_session,
        pair.edge_runtime, pair.edge_session,
        route_key(DESIRED_ID, MEISO_RUNTIME_FACET_DESIRED), false
    ));
    CHECK(publish_actual_with_profile(
        &resources, 1U, desired_ref(1U), desired_ref(1U),
        MEISO_RUNTIME_APPLY_APPLIED,
        MEISO_RUNTIME_AVAILABILITY_PRESENT,
        true, MEISO_RUNTIME_PRODUCTION_RUNNING,
        MEISO_RUNTIME_PRODUCTION_REASON_NONE, 10U,
        effective_low, sizeof(effective_low), actual_state_bytes[0]
    ));
    CHECK(deliver_state(
        &pair, resources.actual_publisher, resources.actual_replica,
        pair.edge_runtime, pair.edge_session,
        pair.host_runtime, pair.host_session,
        route_key(ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL), true
    ));
    CHECK(meiso_runtime_production_snapshot(
        pair.host_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(production.data_active && production.production_epoch == 10U);
    CHECK(production.production_epoch_high_water == 10U);
    CHECK(production.effective_profile.size == sizeof(effective_low));
    CHECK(memcmp(
        production.effective_profile.data,
        effective_low,
        sizeof(effective_low)
    ) == 0);
    sample = stream_sample(1U, 1U, NULL, 0U, payload, sizeof(payload));
    CHECK(send_stream(&pair, &resources, 10U, &sample, true));

    /* Desired does not retract a sample already materialized under the last
     * committed actual. The old gate remains authoritative during apply. */
    materialized_before_reconfigure = stream_sample(
        1U, 2U, NULL, 0U, payload, sizeof(payload)
    );
    CHECK(publish_desired(
        &resources, 2U, true,
        desired_high, sizeof(desired_high), desired_state_bytes[1]
    ));
    CHECK(deliver_state(
        &pair, resources.desired_publisher, resources.desired_replica,
        pair.host_runtime, pair.host_session,
        pair.edge_runtime, pair.edge_session,
        route_key(DESIRED_ID, MEISO_RUNTIME_FACET_DESIRED), false
    ));
    CHECK(meiso_runtime_production_snapshot(
        pair.edge_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(production.data_active && production.production_epoch == 10U);
    CHECK(send_stream(
        &pair, &resources, 10U, &materialized_before_reconfigure, false
    ));
    CHECK(publish_actual_with_profile(
        &resources, 2U, desired_ref(2U), desired_ref(1U),
        MEISO_RUNTIME_APPLY_PENDING,
        MEISO_RUNTIME_AVAILABILITY_PRESENT,
        true, MEISO_RUNTIME_PRODUCTION_RUNNING,
        MEISO_RUNTIME_PRODUCTION_REASON_APPLYING, 10U,
        effective_low, sizeof(effective_low), actual_state_bytes[1]
    ));
    CHECK(deliver_state(
        &pair, resources.actual_publisher, resources.actual_replica,
        pair.edge_runtime, pair.edge_session,
        pair.host_runtime, pair.host_session,
        route_key(ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL), true
    ));
    sample = stream_sample(1U, 3U, NULL, 0U, payload, sizeof(payload));
    CHECK(send_stream(&pair, &resources, 10U, &sample, false));

    CHECK(publish_actual_with_profile(
        &resources, 3U, desired_ref(2U), desired_ref(2U),
        MEISO_RUNTIME_APPLY_APPLIED,
        MEISO_RUNTIME_AVAILABILITY_PRESENT,
        true, MEISO_RUNTIME_PRODUCTION_RUNNING,
        MEISO_RUNTIME_PRODUCTION_REASON_NONE, 11U,
        effective_high, sizeof(effective_high), actual_state_bytes[2]
    ));
    CHECK(deliver_state(
        &pair, resources.actual_publisher, resources.actual_replica,
        pair.edge_runtime, pair.edge_session,
        pair.host_runtime, pair.host_session,
        route_key(ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL), true
    ));
    CHECK(meiso_runtime_production_snapshot(
        pair.host_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(production.data_active && production.production_epoch == 11U);
    CHECK(production.production_epoch_high_water == 11U);
    CHECK(production.effective_profile.size == sizeof(effective_high));
    CHECK(memcmp(
        production.effective_profile.data,
        effective_high,
        sizeof(effective_high)
    ) == 0);
    sample = stream_sample(2U, 1U, NULL, 0U, payload, sizeof(payload));
    CHECK(meiso_runtime_send_stream_sample(
        pair.edge_runtime,
        route_key(DATA_ID, MEISO_RUNTIME_FACET_DATA),
        10U,
        &sample
    ) == MEISO_RUNTIME_PRODUCTION_EPOCH_MISMATCH);
    CHECK(send_stream(&pair, &resources, 11U, &sample, true));

    /* STOP is desired=false, not a subscription. The committed old actual
     * still admits already-materialized data until STOPPED is committed. */
    materialized_before_stop = stream_sample(
        2U, 2U, NULL, 0U, payload, sizeof(payload)
    );
    CHECK(publish_desired(
        &resources, 3U, false,
        desired_high, sizeof(desired_high), desired_state_bytes[2]
    ));
    CHECK(deliver_state(
        &pair, resources.desired_publisher, resources.desired_replica,
        pair.host_runtime, pair.host_session,
        pair.edge_runtime, pair.edge_session,
        route_key(DESIRED_ID, MEISO_RUNTIME_FACET_DESIRED), false
    ));
    CHECK(send_stream(
        &pair, &resources, 11U, &materialized_before_stop, false
    ));
    CHECK(publish_actual_with_profile(
        &resources, 4U, desired_ref(3U), desired_ref(2U),
        MEISO_RUNTIME_APPLY_PENDING,
        MEISO_RUNTIME_AVAILABILITY_PRESENT,
        true, MEISO_RUNTIME_PRODUCTION_STOPPING,
        MEISO_RUNTIME_PRODUCTION_REASON_APPLYING, 11U,
        effective_high, sizeof(effective_high), actual_state_bytes[3]
    ));
    CHECK(deliver_state(
        &pair, resources.actual_publisher, resources.actual_replica,
        pair.edge_runtime, pair.edge_session,
        pair.host_runtime, pair.host_session,
        route_key(ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL), true
    ));
    sample = stream_sample(2U, 3U, NULL, 0U, payload, sizeof(payload));
    CHECK(send_stream(&pair, &resources, 11U, &sample, false));
    CHECK(publish_actual_with_profile(
        &resources, 5U, desired_ref(3U), desired_ref(3U),
        MEISO_RUNTIME_APPLY_APPLIED,
        MEISO_RUNTIME_AVAILABILITY_PRESENT,
        false, MEISO_RUNTIME_PRODUCTION_STOPPED,
        MEISO_RUNTIME_PRODUCTION_REASON_DISABLED, 0U,
        effective_stopped, sizeof(effective_stopped), actual_state_bytes[4]
    ));
    CHECK(deliver_state(
        &pair, resources.actual_publisher, resources.actual_replica,
        pair.edge_runtime, pair.edge_session,
        pair.host_runtime, pair.host_session,
        route_key(ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL), true
    ));
    CHECK(meiso_runtime_production_snapshot(
        pair.host_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(!production.data_active);
    CHECK(production.production_epoch == 0U);
    CHECK(production.production_epoch_high_water == 11U);
    CHECK(meiso_runtime_send_stream_sample(
        pair.edge_runtime,
        route_key(DATA_ID, MEISO_RUNTIME_FACET_DATA),
        11U,
        &sample
    ) == MEISO_RUNTIME_ROUTE_NOT_ACTIVE);
    CHECK(sessions_ready(&pair));

    CHECK(publish_desired(
        &resources, 4U, true,
        desired_low, sizeof(desired_low), desired_state_bytes[3]
    ));
    CHECK(deliver_state(
        &pair, resources.desired_publisher, resources.desired_replica,
        pair.host_runtime, pair.host_session,
        pair.edge_runtime, pair.edge_session,
        route_key(DESIRED_ID, MEISO_RUNTIME_FACET_DESIRED), false
    ));
    CHECK(publish_actual_with_profile(
        &resources, 6U, desired_ref(4U), desired_ref(3U),
        MEISO_RUNTIME_APPLY_PENDING,
        MEISO_RUNTIME_AVAILABILITY_PRESENT,
        false, MEISO_RUNTIME_PRODUCTION_STARTING,
        MEISO_RUNTIME_PRODUCTION_REASON_APPLYING, 0U,
        effective_stopped, sizeof(effective_stopped), actual_state_bytes[5]
    ));
    CHECK(deliver_state(
        &pair, resources.actual_publisher, resources.actual_replica,
        pair.edge_runtime, pair.edge_session,
        pair.host_runtime, pair.host_session,
        route_key(ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL), true
    ));
    CHECK(publish_actual_with_profile(
        &resources, 7U, desired_ref(4U), desired_ref(4U),
        MEISO_RUNTIME_APPLY_APPLIED,
        MEISO_RUNTIME_AVAILABILITY_PRESENT,
        true, MEISO_RUNTIME_PRODUCTION_RUNNING,
        MEISO_RUNTIME_PRODUCTION_REASON_NONE, 12U,
        effective_low, sizeof(effective_low), actual_state_bytes[6]
    ));
    CHECK(deliver_state(
        &pair, resources.actual_publisher, resources.actual_replica,
        pair.edge_runtime, pair.edge_session,
        pair.host_runtime, pair.host_session,
        route_key(ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL), true
    ));
    sample = stream_sample(3U, 1U, NULL, 0U, payload, sizeof(payload));
    CHECK(meiso_runtime_send_stream_sample(
        pair.edge_runtime,
        route_key(DATA_ID, MEISO_RUNTIME_FACET_DATA),
        11U,
        &sample
    ) == MEISO_RUNTIME_PRODUCTION_EPOCH_MISMATCH);
    CHECK(send_stream(&pair, &resources, 12U, &sample, true));
    CHECK(meiso_runtime_production_snapshot(
        pair.host_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(production.data_active && production.production_epoch == 12U);
    CHECK(production.production_epoch_high_water == 12U);
    CHECK(sessions_ready(&pair));
    CHECK(normal_close(&pair));
    return true;
}

static bool test_new_session_requires_catalog(void)
{
    RuntimePair old_pair;
    RuntimePair new_pair;
    MeisoRuntimeProductionSnapshot production;

    CHECK(init_pair(&old_pair, 0U));
    CHECK(activate(&old_pair));
    CHECK(meiso_session_close(
        old_pair.host_session, MEISO_SESSION_CLOSE_NORMAL
    ) == MEISO_SESSION_CLOSE_QUEUED);
    CHECK(meiso_runtime_production_snapshot(
        old_pair.host_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_CORE_NOT_READY);

    CHECK(init_pair(&new_pair, 100U));
    CHECK(meiso_runtime_production_snapshot(
        new_pair.host_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_NO_CATALOG);
    return true;
}

static void put_u64(uint8_t *output, uint64_t value)
{
    unsigned index;

    for (index = 0U; index < 8U; ++index) {
        output[index] = (uint8_t)(value >> (56U - 8U * index));
    }
}

/* Offsets in a Runtime State offer frame (12 B header, 96 B offer prefix) of an Actual's observed and
 * applied Desired owner incarnation and version (runtime.md wire). */
enum {
    ACTUAL_OBSERVED_INCARNATION = 12 + 96 + 8,
    ACTUAL_OBSERVED_VERSION = 12 + 96 + 16,
    ACTUAL_APPLIED_INCARNATION = 12 + 96 + 24,
    ACTUAL_APPLIED_VERSION = 12 + 96 + 32
};

/* The Host takes an altered copy of a received Actual frame: offered, then refused at commit. */
static bool host_refuses_altered_actual(
    RuntimePair *pair,
    const MeisoSessionRuntimeMessage *message,
    size_t offset,
    uint64_t value
)
{
    uint8_t altered[512];
    MeisoSessionRuntimeMessage copy = *message;
    MeisoRuntimeEvent event;

    CHECK(message->payload.size <= sizeof(altered));
    memmove(altered, message->payload.data, message->payload.size);
    put_u64(altered + offset, value);
    put_u64(altered + offset + 16U, value); /* applied as observed: an APPLIED Actual stays well formed */
    copy.payload = (MeisoByteView){altered, message->payload.size};
    CHECK(meiso_runtime_receive(pair->host_runtime, &copy, &event) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    CHECK(meiso_runtime_commit_state_receive(
        pair->host_runtime, event.route, &event.state_offer, MEISO_STATE_RECEIVE_NEW
    ) == MEISO_RUNTIME_STATE_INVALID);
    return true;
}

/* The Edge's next Actual, sent: its frame is taken off the Edge Session into message (in pair->transfer). */
static bool edge_sends_actual(
    RuntimePair *pair,
    Resources *resources,
    uint64_t version,
    uint64_t observed,
    uint64_t epoch,
    uint8_t output[128],
    MeisoSessionRuntimeMessage *message
)
{
    static const uint8_t profile[] = {0xe1U};
    MeisoStateOffer offer;
    MeisoByteView outbound;

    CHECK(publish_actual_with_profile(
        resources, version, desired_ref(observed), desired_ref(observed),
        MEISO_RUNTIME_APPLY_APPLIED, MEISO_RUNTIME_AVAILABILITY_PRESENT,
        true, MEISO_RUNTIME_PRODUCTION_RUNNING,
        MEISO_RUNTIME_PRODUCTION_REASON_NONE, epoch, profile, sizeof(profile), output
    ));
    CHECK(meiso_state_publisher_next_offer(resources->actual_publisher, &offer));
    CHECK(meiso_runtime_send_state_offer(
        pair->edge_runtime, route_key(ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL), &offer
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_session_next_outbound(pair->edge_session, &outbound));
    CHECK(outbound.size <= sizeof(pair->transfer));
    memmove(pair->transfer, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(pair->edge_session) == MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        pair->host_session, (MeisoByteView){pair->transfer, outbound.size}, message
    ) == MEISO_SESSION_RECEIVE_RUNTIME);
    return true;
}

/* The Host commits a received Actual frame (its accept goes out once the Session can send it). */
static bool host_takes_actual(
    RuntimePair *pair,
    Resources *resources,
    const MeisoSessionRuntimeMessage *message,
    MeisoRuntimeEvent *event
)
{
    MeisoStateReceiveStatus received;

    CHECK(meiso_runtime_receive(pair->host_runtime, message, event) == MEISO_RUNTIME_OK);
    CHECK(event->type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    received = meiso_state_replica_receive(
        resources->actual_replica, event->state_offer.state.owner,
        event->state_offer.state.version, event->state_offer.state.payload
    );
    CHECK(received == MEISO_STATE_RECEIVE_NEW);
    CHECK(meiso_runtime_commit_state_receive(
        pair->host_runtime, event->route, &event->state_offer, received
    ) == MEISO_RUNTIME_OK);
    return true;
}

/* The Host's Desired in flight reaches the Edge, which commits and accepts it; then the Host's accept of
 * the Actual it took (event) and the Edge's accept of the Desired are delivered. */
static bool settle_crossing(RuntimePair *pair, Resources *resources, const MeisoRuntimeEvent *actual)
{
    MeisoRuntimeEvent event;
    MeisoStateReceiveStatus received;

    CHECK(transfer(pair, pair->host_session, pair->edge_session, pair->edge_runtime, &event, MEISO_RUNTIME_OK));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    received = meiso_state_replica_receive(
        resources->desired_replica, event.state_offer.state.owner,
        event.state_offer.state.version, event.state_offer.state.payload
    );
    CHECK(received == MEISO_STATE_RECEIVE_NEW);
    CHECK(meiso_runtime_commit_state_receive(
        pair->edge_runtime, event.route, &event.state_offer, received
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_state_accept(
        pair->edge_runtime, event.route, event.state_offer.generation,
        event.state_offer.state.owner, event.state_offer.state.version
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_state_accept(
        pair->host_runtime, actual->route, actual->state_offer.generation,
        actual->state_offer.state.owner, actual->state_offer.state.version
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer(pair, pair->host_session, pair->edge_session, pair->edge_runtime, &event, MEISO_RUNTIME_OK));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT);
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        resources->actual_publisher, event.state_accept_generation,
        event.state_accept_owner, event.state_accept_version
    ) == MEISO_STATE_ACCEPT_CONFIRMED);
    CHECK(transfer(pair, pair->edge_session, pair->host_session, pair->host_runtime, &event, MEISO_RUNTIME_OK));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT);
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        resources->desired_publisher, event.state_accept_generation,
        event.state_accept_owner, event.state_accept_version
    ) == MEISO_STATE_ACCEPT_CONFIRMED);
    return true;
}

/* The sender's rule is unchanged: an Edge names the last Desired it received, never an earlier one. */
static bool edge_cannot_name(RuntimePair *pair, uint64_t observed)
{
    uint8_t bytes[128];
    MeisoByteView payload;
    MeisoRuntimeActualState value;
    MeisoStateOffer offer;

    memset(&value, 0, sizeof(value));
    value.observed_desired = desired_ref(observed);
    value.applied_desired = desired_ref(observed);
    value.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    value.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    value.effective_enabled = true;
    value.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING;
    value.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    value.production_epoch = 50U;
    CHECK(meiso_runtime_actual_state_encode(&value, (MeisoMutableBuffer){bytes, sizeof(bytes)}, &payload));
    memset(&offer, 0, sizeof(offer));
    offer.generation.value = 1U;
    offer.state.owner = EDGE_OWNER;
    offer.state.version = 50U;
    offer.state.payload = payload;
    CHECK(meiso_runtime_send_state_offer(
        pair->edge_runtime, route_key(ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL), &offer
    ) == MEISO_RUNTIME_STATE_INVALID);
    return true;
}

/* runtime.md, Desired -> actual: the Edge publishes an Actual by itself (a source arrived) while the
 * Host's next Desired is on its way (meiso_core #36). The Host takes it as the Edge's state under the
 * Desired it names, the Session goes on and the Actual for the newer Desired follows. An Actual naming
 * a Desired the Host never sent, another owner's, or one before an observation already taken is
 * still invalid. */
static bool test_actual_crossing_a_newer_desired(void)
{
    RuntimePair pair;
    Resources resources;
    uint8_t desired_bytes[3][64];
    uint8_t actual_bytes[4][128];
    const uint8_t rate[] = {10U};
    const uint8_t faster[] = {20U};
    const uint8_t fastest[] = {30U};
    MeisoStateOffer offer;
    MeisoRuntimeEvent event;
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeProductionSnapshot production;

    CHECK(init_pair(&pair, 200U));
    CHECK(activate(&pair));
    CHECK(init_resources(&resources));
    CHECK(publish_desired(&resources, 1U, true, rate, sizeof(rate), desired_bytes[0]));
    CHECK(deliver_state(
        &pair, resources.desired_publisher, resources.desired_replica,
        pair.host_runtime, pair.host_session, pair.edge_runtime, pair.edge_session,
        route_key(DESIRED_ID, MEISO_RUNTIME_FACET_DESIRED), false
    ));
    CHECK(publish_actual(
        &resources, 1U, desired_ref(1U), desired_ref(1U), MEISO_RUNTIME_APPLY_APPLIED,
        MEISO_RUNTIME_AVAILABILITY_PRESENT, true, MEISO_RUNTIME_PRODUCTION_RUNNING,
        MEISO_RUNTIME_PRODUCTION_REASON_NONE, 10U, actual_bytes[0]
    ));
    CHECK(deliver_state(
        &pair, resources.actual_publisher, resources.actual_replica,
        pair.edge_runtime, pair.edge_session, pair.host_runtime, pair.host_session,
        route_key(ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL), true
    ));

    /* Desired 2 leaves the Host; the Edge, still on 1, publishes Actual 2 (epoch 11). */
    CHECK(publish_desired(&resources, 2U, true, faster, sizeof(faster), desired_bytes[1]));
    CHECK(meiso_state_publisher_next_offer(resources.desired_publisher, &offer));
    CHECK(meiso_runtime_send_state_offer(
        pair.host_runtime, route_key(DESIRED_ID, MEISO_RUNTIME_FACET_DESIRED), &offer
    ) == MEISO_RUNTIME_OK);
    CHECK(edge_sends_actual(&pair, &resources, 2U, 1U, 11U, actual_bytes[1], &message));
    CHECK(host_refuses_altered_actual(&pair, &message, ACTUAL_OBSERVED_VERSION, 3U));
    CHECK(host_refuses_altered_actual(&pair, &message, ACTUAL_OBSERVED_INCARNATION, EDGE_OWNER.incarnation));
    CHECK(host_takes_actual(&pair, &resources, &message, &event));
    CHECK(meiso_runtime_production_snapshot(pair.host_runtime, ENDPOINT, &production) == MEISO_RUNTIME_OK);
    CHECK(production.data_active && production.production_epoch == 11U);
    CHECK(production.observed_desired.version == 1U);
    CHECK(settle_crossing(&pair, &resources, &event));
    CHECK(edge_cannot_name(&pair, 1U));

    /* Desired 3 leaves the Host; the Edge's Actual 3 for Desired 2 (epoch 12) crosses it and is taken. */
    CHECK(publish_desired(&resources, 3U, true, fastest, sizeof(fastest), desired_bytes[2]));
    CHECK(meiso_state_publisher_next_offer(resources.desired_publisher, &offer));
    CHECK(meiso_runtime_send_state_offer(
        pair.host_runtime, route_key(DESIRED_ID, MEISO_RUNTIME_FACET_DESIRED), &offer
    ) == MEISO_RUNTIME_OK);
    CHECK(edge_sends_actual(&pair, &resources, 3U, 2U, 12U, actual_bytes[2], &message));
    CHECK(host_refuses_altered_actual(&pair, &message, ACTUAL_OBSERVED_VERSION, 4U));
    CHECK(host_takes_actual(&pair, &resources, &message, &event));
    CHECK(meiso_runtime_production_snapshot(pair.host_runtime, ENDPOINT, &production) == MEISO_RUNTIME_OK);
    CHECK(production.production_epoch == 12U && production.observed_desired.version == 2U);
    CHECK(settle_crossing(&pair, &resources, &event));

    /* The Edge's Actual 4 for Desired 3: an altered copy going back to Desired 1 is refused. */
    CHECK(edge_sends_actual(&pair, &resources, 4U, 3U, 13U, actual_bytes[3], &message));
    CHECK(host_refuses_altered_actual(&pair, &message, ACTUAL_OBSERVED_VERSION, 1U));
    CHECK(host_takes_actual(&pair, &resources, &message, &event));
    CHECK(meiso_runtime_production_snapshot(pair.host_runtime, ENDPOINT, &production) == MEISO_RUNTIME_OK);
    CHECK(production.production_epoch == 13U && production.observed_desired.version == 3U);
    return true;
}

int main(void)
{
    if (!test_state_stream_integration() ||
        !test_continuous_session_reconfigure() ||
        !test_new_session_requires_catalog() ||
        !test_actual_crossing_a_newer_desired()) {
        return 1;
    }
    (void)printf("runtime session integration tests passed\n");
    return 0;
}
