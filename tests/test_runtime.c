#include <meiso/runtime/runtime.h>

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
    MeisoMaxAlign alignment;
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

static const MeisoEndpointId ENDPOINT = {0x101U, 0x102U};
static const uint64_t ENDPOINT_INCARNATION = 7U;
static const MeisoResourceId DESIRED_ID = {0x201U, 0x202U};
static const MeisoResourceId ACTUAL_ID = {0x211U, 0x212U};
static const MeisoResourceId DATA_ID = {0x221U, 0x222U};
static const MeisoSchemaRef CAPABILITY_SCHEMA = {{0x301U, 0x302U}, 1U};
static const MeisoSchemaRef DESIRED_SCHEMA = {{0x311U, 0x312U}, 1U};
static const MeisoSchemaRef ACTUAL_SCHEMA = {{0x321U, 0x322U}, 1U};
static const MeisoSchemaRef DATA_SCHEMA = {{0x331U, 0x332U}, 1U};

static MeisoSessionConfig session_config(MeisoSessionRole role)
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
        (MeisoSessionNonce){0x61U, 0x62U} :
        (MeisoSessionNonce){0x71U, 0x72U};
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

static bool handshake_step(MeisoSession *sender, MeisoSession *receiver)
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

static bool init_runtime_with_history(
    TestArena *arena,
    MeisoSession *session,
    uint32_t max_endpoints,
    uint32_t max_endpoint_history,
    MeisoRuntime **out_runtime
)
{
    MeisoRuntimeConfig config;
    size_t size;

    memset(&config, 0, sizeof(config));
    config.session = session;
    config.max_endpoints = max_endpoints;
    config.max_endpoint_history = max_endpoint_history;
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

static bool init_pair_with_history(
    RuntimePair *pair,
    uint32_t max_endpoints,
    uint32_t max_endpoint_history
)
{
    MeisoSessionConfig host = session_config(MEISO_SESSION_ROLE_HOST);
    MeisoSessionConfig edge = session_config(MEISO_SESSION_ROLE_EDGE);

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
    CHECK(handshake_step(pair->host_session, pair->edge_session));
    CHECK(handshake_step(pair->edge_session, pair->host_session));
    CHECK(handshake_step(pair->host_session, pair->edge_session));
    CHECK(handshake_step(pair->edge_session, pair->host_session));
    CHECK(init_runtime_with_history(
        &pair->host_runtime_arena,
        pair->host_session,
        max_endpoints,
        max_endpoint_history,
        &pair->host_runtime
    ));
    CHECK(init_runtime_with_history(
        &pair->edge_runtime_arena,
        pair->edge_session,
        max_endpoints,
        max_endpoint_history,
        &pair->edge_runtime
    ));
    return true;
}

static bool init_pair(RuntimePair *pair)
{
    return init_pair_with_history(pair, 4U, 8U);
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

static void make_catalog(
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

    routes[0].endpoint_id = ENDPOINT;
    routes[0].endpoint_incarnation = ENDPOINT_INCARNATION;
    routes[0].resource = (MeisoResourceDescriptor){
        DESIRED_ID, MEISO_RESOURCE_KIND_STATE, DESIRED_SCHEMA,
        MEISO_OWNER_ROLE_HOST, 64U
    };
    routes[0].facet = MEISO_RUNTIME_FACET_DESIRED;
    routes[1].endpoint_id = ENDPOINT;
    routes[1].endpoint_incarnation = ENDPOINT_INCARNATION;
    routes[1].resource = (MeisoResourceDescriptor){
        ACTUAL_ID, MEISO_RESOURCE_KIND_STATE, ACTUAL_SCHEMA,
        MEISO_OWNER_ROLE_EDGE, 128U
    };
    routes[1].facet = MEISO_RUNTIME_FACET_ACTUAL;
    routes[2].endpoint_id = ENDPOINT;
    routes[2].endpoint_incarnation = ENDPOINT_INCARNATION;
    routes[2].resource = (MeisoResourceDescriptor){
        DATA_ID, MEISO_RESOURCE_KIND_STREAM, DATA_SCHEMA,
        MEISO_OWNER_ROLE_EDGE, 64U
    };
    routes[2].facet = MEISO_RUNTIME_FACET_DATA;
}

static void make_desired_only_catalog(
    MeisoEndpointId endpoint_id,
    uint64_t incarnation,
    MeisoRuntimeEndpointDescriptor *endpoint,
    MeisoRuntimeRouteDescriptor *route
)
{
    static const uint8_t capability[] = {0xb1U};

    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->id = endpoint_id;
    endpoint->incarnation = incarnation;
    endpoint->capability_schema = CAPABILITY_SCHEMA;
    endpoint->capability = (MeisoByteView){capability, sizeof(capability)};
    memset(route, 0, sizeof(*route));
    route->endpoint_id = endpoint_id;
    route->endpoint_incarnation = incarnation;
    route->resource = (MeisoResourceDescriptor){
        DESIRED_ID, MEISO_RESOURCE_KIND_STATE, DESIRED_SCHEMA,
        MEISO_OWNER_ROLE_HOST, 64U
    };
    route->facet = MEISO_RUNTIME_FACET_DESIRED;
}

static bool activate_catalog(
    RuntimePair *pair,
    uint64_t generation,
    const MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_count,
    const MeisoRuntimeRouteDescriptor *routes,
    size_t route_count
)
{
    MeisoRuntimeEvent event;

    CHECK(meiso_runtime_offer_catalog(
        pair->edge_runtime, generation, endpoints, endpoint_count,
        routes, route_count
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer(
        pair, pair->edge_session, pair->host_session, pair->host_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED);
    CHECK(meiso_runtime_accept_catalog(pair->host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(transfer(
        pair, pair->host_session, pair->edge_session, pair->edge_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED);
    return true;
}

static bool activate(RuntimePair *pair, uint64_t generation)
{
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor routes[3];

    make_catalog(&endpoint, routes);
    return activate_catalog(pair, generation, &endpoint, 1U, routes, 3U);
}

static MeisoRuntimeRouteKey key(
    uint64_t generation,
    MeisoResourceId resource,
    MeisoRuntimeFacet facet
)
{
    MeisoRuntimeRouteKey value;

    memset(&value, 0, sizeof(value));
    value.catalog_generation = generation;
    value.endpoint_id = ENDPOINT;
    value.endpoint_incarnation = ENDPOINT_INCARNATION;
    value.resource_id = resource;
    value.facet = facet;
    return value;
}

static MeisoRuntimeActualState running_actual(
    uint64_t observed_version,
    uint64_t applied_version,
    MeisoRuntimeApplyResult result,
    uint64_t epoch
)
{
    MeisoRuntimeActualState actual;

    memset(&actual, 0, sizeof(actual));
    actual.observed_desired.present = true;
    actual.observed_desired.owner = (MeisoOwnerScope){
        MEISO_OWNER_ROLE_HOST, 31U
    };
    actual.observed_desired.version = observed_version;
    actual.applied_desired.present = true;
    actual.applied_desired.owner = (MeisoOwnerScope){
        MEISO_OWNER_ROLE_HOST, 31U
    };
    actual.applied_desired.version = applied_version;
    actual.apply_result = result;
    actual.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    actual.effective_enabled = true;
    actual.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING;
    actual.reason = result == MEISO_RUNTIME_APPLY_REJECTED ?
        MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER :
        MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    actual.production_epoch = epoch;
    return actual;
}

static bool test_payload_contract(void)
{
    const uint8_t profile[] = {0x10U, 0x20U};
    uint8_t bytes[128];
    MeisoByteView encoded;
    MeisoRuntimeDesiredState desired = {
        true, {profile, sizeof(profile)}
    };
    MeisoRuntimeDesiredState desired_view;
    MeisoRuntimeActualState actual = running_actual(
        9U, 9U, MEISO_RUNTIME_APPLY_APPLIED, 77U
    );
    MeisoRuntimeActualState actual_view;
    TestArena overlap;
    MeisoRuntimeDesiredState *overlap_desired =
        (MeisoRuntimeDesiredState *)(void *)overlap.bytes;
    MeisoRuntimeActualState *overlap_actual =
        (MeisoRuntimeActualState *)(void *)overlap.bytes;

    CHECK(meiso_runtime_desired_state_encoded_size(&desired) == 10U);
    CHECK(meiso_runtime_desired_state_encode(
        &desired, (MeisoMutableBuffer){bytes, sizeof(bytes)}, &encoded
    ));
    CHECK(encoded.size == 10U && bytes[0] == 1U);
    CHECK(memcmp(bytes + 1U, "\0\0\0\0\0\0\0", 7U) == 0);
    CHECK(meiso_runtime_desired_state_decode(encoded, &desired_view));
    CHECK(desired_view.enabled && desired_view.profile.size == sizeof(profile));
    bytes[1] = 1U;
    CHECK(!meiso_runtime_desired_state_decode(encoded, &desired_view));
    bytes[1] = 0U;
    *overlap_desired = desired;
    CHECK(meiso_runtime_desired_state_encode(
        overlap_desired,
        (MeisoMutableBuffer){overlap.bytes, sizeof(overlap.bytes)},
        &encoded
    ));
    CHECK(encoded.data[0] == 1U && encoded.size == 10U);

    actual.profile = (MeisoByteView){profile, sizeof(profile)};
    CHECK(meiso_runtime_actual_state_encoded_size(&actual) == 58U);
    CHECK(meiso_runtime_actual_state_encode(
        &actual, (MeisoMutableBuffer){bytes, sizeof(bytes)}, &encoded
    ));
    CHECK(meiso_runtime_actual_state_decode(encoded, &actual_view));
    CHECK(actual_view.observed_desired.owner.incarnation == 31U);
    CHECK(actual_view.applied_desired.version == 9U);
    CHECK(actual_view.production_epoch == 77U);
    *overlap_actual = actual;
    CHECK(meiso_runtime_actual_state_encode(
        overlap_actual,
        (MeisoMutableBuffer){overlap.bytes, sizeof(overlap.bytes)},
        &encoded
    ));
    CHECK(encoded.size == 58U);

    actual.applied_desired.present = false;
    actual.applied_desired.owner = (MeisoOwnerScope){0U, 0U};
    actual.applied_desired.version = 0U;
    CHECK(meiso_runtime_actual_state_encoded_size(&actual) == 0U);
    actual = running_actual(10U, 9U, MEISO_RUNTIME_APPLY_REJECTED, 77U);
    CHECK(meiso_runtime_actual_state_encoded_size(&actual) == 56U);
    actual.availability = MEISO_RUNTIME_AVAILABILITY_ABSENT;
    CHECK(meiso_runtime_actual_state_encoded_size(&actual) == 0U);
    actual = running_actual(9U, 9U, MEISO_RUNTIME_APPLY_APPLIED, 77U);
    actual.production_status = MEISO_RUNTIME_PRODUCTION_STARTING;
    CHECK(meiso_runtime_actual_state_encoded_size(&actual) == 0U);
    actual.production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
    CHECK(meiso_runtime_actual_state_encoded_size(&actual) == 0U);
    return true;
}

static bool test_catalog_contract(void)
{
    RuntimePair pair;
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor routes[3];
    MeisoRuntimeSnapshot snapshot;

    CHECK(init_pair(&pair));
    make_catalog(&endpoint, routes);
    routes[2].facet = MEISO_RUNTIME_FACET_OPERATION;
    routes[2].resource.kind = MEISO_RESOURCE_KIND_ACTION;
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 1U, &endpoint, 1U, routes, 3U
    ) == MEISO_RUNTIME_UNSUPPORTED_KIND);
    make_catalog(&endpoint, routes);
    routes[1].resource.id = routes[0].resource.id;
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 1U, &endpoint, 1U, routes, 3U
    ) == MEISO_RUNTIME_CONFLICT);
    make_catalog(&endpoint, routes);
    routes[0].facet = MEISO_RUNTIME_FACET_DATA;
    routes[0].resource.kind = MEISO_RESOURCE_KIND_STREAM;
    routes[0].resource.owner = MEISO_OWNER_ROLE_EDGE;
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 1U, &endpoint, 1U, routes, 3U
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(activate(&pair, 1U));
    CHECK(meiso_runtime_snapshot(pair.host_runtime, &snapshot));
    CHECK(snapshot.active_catalog_generation == 1U);
    CHECK(snapshot.active_route_count == 3U);
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 1U, &endpoint, 1U, routes, 3U
    ) == MEISO_RUNTIME_STALE_CATALOG);
    return true;
}

static bool test_identity_history_capacity(void)
{
    const MeisoEndpointId endpoint_id = {0x401U, 0x402U};
    RuntimePair pair;
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor route;
    MeisoRuntimeConfig invalid_config;

    CHECK(init_pair_with_history(&pair, 1U, 2U));
    memset(&invalid_config, 0, sizeof(invalid_config));
    invalid_config.session = pair.edge_session;
    invalid_config.max_endpoints = 2U;
    invalid_config.max_endpoint_history = 1U;
    invalid_config.max_routes = 2U;
    invalid_config.max_capability_bytes = 8U;
    invalid_config.max_desired_profile_bytes_per_endpoint = 8U;
    invalid_config.max_actual_profile_bytes_per_endpoint = 8U;
    CHECK(meiso_runtime_memory_required(&invalid_config) == 0U);

    make_desired_only_catalog(endpoint_id, 9U, &endpoint, &route);
    CHECK(activate_catalog(&pair, 1U, &endpoint, 1U, &route, 1U));
    make_desired_only_catalog(endpoint_id, 3U, &endpoint, &route);
    CHECK(activate_catalog(&pair, 2U, &endpoint, 1U, &route, 1U));
    make_desired_only_catalog(endpoint_id, 5U, &endpoint, &route);
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 3U, &endpoint, 1U, &route, 1U
    ) == MEISO_RUNTIME_CAPACITY);
    make_desired_only_catalog(endpoint_id, 3U, &endpoint, &route);
    CHECK(activate_catalog(&pair, 3U, &endpoint, 1U, &route, 1U));
    return true;
}

static bool test_gate_and_core_commit(void)
{
    RuntimePair pair;
    uint8_t actual_bytes[128];
    uint8_t forged_bytes[128];
    uint8_t desired_bytes[16];
    const uint8_t stream_payload[] = {1U, 2U, 3U};
    const uint8_t desired_profile[] = {0x41U, 0x42U};
    const uint8_t actual_profile[] = {0x51U};
    const uint8_t changed_profile[] = {0x52U};
    const MeisoEndpointId other_endpoint = {0x111U, 0x112U};
    MeisoByteView actual_payload;
    MeisoByteView desired_payload;
    MeisoRuntimeActualState actual = running_actual(
        1U, 1U, MEISO_RUNTIME_APPLY_APPLIED, 70U
    );
    MeisoRuntimeDesiredState desired = {false, {NULL, 0U}};
    MeisoStateOffer actual_offer;
    MeisoStateOffer forged_offer;
    MeisoStateOffer desired_offer;
    MeisoStreamSample sample;
    MeisoRuntimeEvent event;
    MeisoRuntimeProductionSnapshot production;
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor routes[3];

    CHECK(init_pair(&pair));
    CHECK(activate(&pair, 1U));
    actual.profile = (MeisoByteView){actual_profile, sizeof(actual_profile)};
    memset(&sample, 0, sizeof(sample));
    sample.key.producer = (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 41U};
    sample.key.epoch = 5U;
    sample.key.sequence = 1U;
    sample.source_time = (MeisoTimePoint){9U, 100U};
    sample.payload = (MeisoByteView){stream_payload, sizeof(stream_payload)};
    CHECK(meiso_runtime_send_stream_sample(
        pair.edge_runtime,
        key(1U, DATA_ID, MEISO_RUNTIME_FACET_DATA),
        70U,
        &sample
    ) == MEISO_RUNTIME_ROUTE_NOT_ACTIVE);

    desired.enabled = true;
    desired.profile = (MeisoByteView){
        desired_profile, sizeof(desired_profile)
    };
    CHECK(meiso_runtime_desired_state_encode(
        &desired,
        (MeisoMutableBuffer){desired_bytes, sizeof(desired_bytes)},
        &desired_payload
    ));
    desired_offer.generation = (MeisoDeliveryGeneration){1U};
    desired_offer.state.owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 31U};
    desired_offer.state.version = 1U;
    desired_offer.state.payload = desired_payload;
    CHECK(meiso_runtime_send_state_offer(
        pair.host_runtime,
        key(1U, DESIRED_ID, MEISO_RUNTIME_FACET_DESIRED),
        &desired_offer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer(
        &pair, pair.host_session, pair.edge_session, pair.edge_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.state_offer.state.payload.size <= sizeof(forged_bytes));
    memmove(
        forged_bytes,
        event.state_offer.state.payload.data,
        event.state_offer.state.payload.size
    );
    forged_bytes[event.state_offer.state.payload.size - 1U] ^= 1U;
    forged_offer = event.state_offer;
    forged_offer.state.payload.data = forged_bytes;
    CHECK(meiso_runtime_commit_state_receive(
        pair.edge_runtime, event.route, &forged_offer,
        MEISO_STATE_RECEIVE_NEW
    ) == MEISO_RUNTIME_CONFLICT);

    desired_offer.generation.value = 2U;
    CHECK(meiso_runtime_send_state_offer(
        pair.host_runtime,
        key(1U, DESIRED_ID, MEISO_RUNTIME_FACET_DESIRED),
        &desired_offer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer(
        &pair, pair.host_session, pair.edge_session, pair.edge_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(meiso_runtime_commit_state_receive(
        pair.edge_runtime, event.route, &event.state_offer,
        MEISO_STATE_RECEIVE_NEW
    ) == MEISO_RUNTIME_OK);

    actual_offer.generation = (MeisoDeliveryGeneration){2U};
    actual_offer.state.owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 41U};
    actual_offer.state.version = 2U;
    actual = running_actual(2U, 2U, MEISO_RUNTIME_APPLY_APPLIED, 70U);
    actual.profile = (MeisoByteView){actual_profile, sizeof(actual_profile)};
    CHECK(meiso_runtime_actual_state_encode(
        &actual,
        (MeisoMutableBuffer){actual_bytes, sizeof(actual_bytes)},
        &actual_payload
    ));
    actual_offer.state.payload = actual_payload;
    CHECK(meiso_runtime_send_state_offer(
        pair.edge_runtime,
        key(1U, ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL),
        &actual_offer
    ) == MEISO_RUNTIME_STATE_INVALID);

    actual = running_actual(1U, 1U, MEISO_RUNTIME_APPLY_APPLIED, 70U);
    actual.profile = (MeisoByteView){actual_profile, sizeof(actual_profile)};
    CHECK(meiso_runtime_actual_state_encode(
        &actual,
        (MeisoMutableBuffer){actual_bytes, sizeof(actual_bytes)},
        &actual_payload
    ));
    actual_offer.generation = (MeisoDeliveryGeneration){3U};
    actual_offer.state.version = 3U;
    actual_offer.state.payload = actual_payload;
    CHECK(meiso_runtime_send_state_offer(
        pair.edge_runtime,
        key(1U, ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL),
        &actual_offer
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_production_snapshot(
        pair.edge_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(production.data_active && production.production_epoch == 70U);
    CHECK(transfer(
        &pair, pair.edge_session, pair.host_session, pair.host_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    CHECK(meiso_runtime_commit_state_receive(
        pair.host_runtime, event.route, &event.state_offer,
        MEISO_STATE_RECEIVE_STALE
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_production_snapshot(
        pair.host_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(!production.data_active);

    actual_offer.state.version = 4U;
    actual_offer.generation.value = 4U;
    CHECK(meiso_runtime_send_state_offer(
        pair.edge_runtime,
        key(1U, ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL),
        &actual_offer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer(
        &pair, pair.edge_session, pair.host_session, pair.host_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.state_offer.state.payload.size <= sizeof(forged_bytes));
    memmove(
        forged_bytes,
        event.state_offer.state.payload.data,
        event.state_offer.state.payload.size
    );
    forged_bytes[event.state_offer.state.payload.size - 1U] ^= 1U;
    forged_offer = event.state_offer;
    forged_offer.state.payload.data = forged_bytes;
    CHECK(meiso_runtime_commit_state_receive(
        pair.host_runtime, event.route, &forged_offer,
        MEISO_STATE_RECEIVE_NEW
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(meiso_runtime_production_snapshot(
        pair.host_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(!production.data_active);

    actual_offer.state.version = 5U;
    actual_offer.generation.value = 5U;
    CHECK(meiso_runtime_send_state_offer(
        pair.edge_runtime,
        key(1U, ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL),
        &actual_offer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer(
        &pair, pair.edge_session, pair.host_session, pair.host_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(meiso_runtime_commit_state_receive(
        pair.host_runtime, event.route, &event.state_offer,
        MEISO_STATE_RECEIVE_NEW
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_production_snapshot(
        pair.host_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(production.data_active && production.production_epoch == 70U &&
          production.effective_profile.size == sizeof(actual_profile) &&
          production.effective_profile.data[0] == actual_profile[0]);

    actual = running_actual(1U, 1U, MEISO_RUNTIME_APPLY_APPLIED, 70U);
    actual.profile = (MeisoByteView){
        changed_profile, sizeof(changed_profile)
    };
    CHECK(meiso_runtime_actual_state_encode(
        &actual,
        (MeisoMutableBuffer){actual_bytes, sizeof(actual_bytes)},
        &actual_payload
    ));
    actual_offer.generation.value = 6U;
    actual_offer.state.version = 6U;
    actual_offer.state.payload = actual_payload;
    CHECK(meiso_runtime_send_state_offer(
        pair.edge_runtime,
        key(1U, ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL),
        &actual_offer
    ) == MEISO_RUNTIME_STATE_INVALID);

    desired.enabled = false;
    desired.profile = (MeisoByteView){NULL, 0U};
    CHECK(meiso_runtime_desired_state_encode(
        &desired,
        (MeisoMutableBuffer){desired_bytes, sizeof(desired_bytes)},
        &desired_payload
    ));
    desired_offer.generation = (MeisoDeliveryGeneration){5U};
    desired_offer.state.owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 31U};
    desired_offer.state.version = 5U;
    desired_offer.state.payload = desired_payload;
    CHECK(meiso_runtime_send_state_offer(
        pair.host_runtime,
        key(1U, DESIRED_ID, MEISO_RUNTIME_FACET_DESIRED),
        &desired_offer
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_production_snapshot(
        pair.host_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(production.data_active);
    CHECK(transfer(
        &pair, pair.host_session, pair.edge_session, pair.edge_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    CHECK(meiso_runtime_commit_state_receive(
        pair.edge_runtime, event.route, &event.state_offer,
        MEISO_STATE_RECEIVE_NEW
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_production_snapshot(
        pair.edge_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(production.data_active);

    CHECK(meiso_runtime_send_stream_sample(
        pair.edge_runtime,
        key(1U, DATA_ID, MEISO_RUNTIME_FACET_DATA),
        69U,
        &sample
    ) == MEISO_RUNTIME_PRODUCTION_EPOCH_MISMATCH);
    CHECK(meiso_runtime_send_stream_sample(
        pair.edge_runtime,
        key(1U, DATA_ID, MEISO_RUNTIME_FACET_DATA),
        70U,
        &sample
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer(
        &pair, pair.edge_session, pair.host_session, pair.host_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE);
    CHECK(event.production_epoch == 70U);
    CHECK(event.stream_sample.key.sequence == 1U);

    memset(&actual, 0, sizeof(actual));
    actual.observed_desired = (MeisoRuntimeDesiredReference){
        true, {MEISO_OWNER_ROLE_HOST, 31U}, 5U
    };
    actual.applied_desired = actual.observed_desired;
    actual.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    actual.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    actual.production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
    actual.reason = MEISO_RUNTIME_PRODUCTION_REASON_DISABLED;
    CHECK(meiso_runtime_actual_state_encode(
        &actual,
        (MeisoMutableBuffer){actual_bytes, sizeof(actual_bytes)},
        &actual_payload
    ));
    actual_offer.generation.value = 6U;
    actual_offer.state.version = 6U;
    actual_offer.state.payload = actual_payload;
    CHECK(meiso_runtime_send_state_offer(
        pair.edge_runtime,
        key(1U, ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL),
        &actual_offer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer(
        &pair, pair.edge_session, pair.host_session, pair.host_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(meiso_runtime_commit_state_receive(
        pair.host_runtime, event.route, &event.state_offer,
        MEISO_STATE_RECEIVE_NEW
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_production_snapshot(
        pair.edge_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(!production.data_active &&
          production.production_epoch_high_water == 70U);

    actual = running_actual(5U, 5U, MEISO_RUNTIME_APPLY_APPLIED, 70U);
    actual.profile = (MeisoByteView){actual_profile, sizeof(actual_profile)};
    CHECK(meiso_runtime_actual_state_encode(
        &actual,
        (MeisoMutableBuffer){actual_bytes, sizeof(actual_bytes)},
        &actual_payload
    ));
    actual_offer.generation.value = 7U;
    actual_offer.state.version = 7U;
    actual_offer.state.payload = actual_payload;
    CHECK(meiso_runtime_send_state_offer(
        pair.edge_runtime,
        key(1U, ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL),
        &actual_offer
    ) == MEISO_RUNTIME_STATE_INVALID);

    make_desired_only_catalog(
        other_endpoint, 1U, &endpoint, &routes[0]
    );
    CHECK(activate_catalog(&pair, 2U, &endpoint, 1U, routes, 1U));
    CHECK(meiso_runtime_production_snapshot(
        pair.host_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_UNKNOWN_ENDPOINT);
    CHECK(meiso_runtime_production_snapshot(
        pair.edge_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_UNKNOWN_ENDPOINT);

    make_catalog(&endpoint, routes);
    CHECK(activate_catalog(&pair, 3U, &endpoint, 1U, routes, 3U));
    CHECK(meiso_runtime_production_snapshot(
        pair.host_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(!production.actual_present && !production.data_active &&
          production.production_epoch_high_water == 70U);
    CHECK(meiso_runtime_production_snapshot(
        pair.edge_runtime, ENDPOINT, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(!production.actual_present && !production.data_active &&
          production.production_epoch_high_water == 70U);

    desired.enabled = true;
    CHECK(meiso_runtime_desired_state_encode(
        &desired,
        (MeisoMutableBuffer){desired_bytes, sizeof(desired_bytes)},
        &desired_payload
    ));
    desired_offer.generation.value = 8U;
    desired_offer.state.version = 8U;
    desired_offer.state.payload = desired_payload;
    CHECK(meiso_runtime_send_state_offer(
        pair.host_runtime,
        key(3U, DESIRED_ID, MEISO_RUNTIME_FACET_DESIRED),
        &desired_offer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer(
        &pair, pair.host_session, pair.edge_session, pair.edge_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(meiso_runtime_commit_state_receive(
        pair.edge_runtime, event.route, &event.state_offer,
        MEISO_STATE_RECEIVE_NEW
    ) == MEISO_RUNTIME_OK);
    actual = running_actual(8U, 8U, MEISO_RUNTIME_APPLY_APPLIED, 70U);
    actual.profile = (MeisoByteView){actual_profile, sizeof(actual_profile)};
    CHECK(meiso_runtime_actual_state_encode(
        &actual,
        (MeisoMutableBuffer){actual_bytes, sizeof(actual_bytes)},
        &actual_payload
    ));
    actual_offer.state.payload = actual_payload;
    CHECK(meiso_runtime_send_state_offer(
        pair.edge_runtime,
        key(3U, ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL),
        &actual_offer
    ) == MEISO_RUNTIME_STATE_INVALID);

    make_catalog(&endpoint, routes);
    endpoint.incarnation = ENDPOINT_INCARNATION - 4U;
    routes[0].endpoint_incarnation = endpoint.incarnation;
    routes[1].endpoint_incarnation = endpoint.incarnation;
    routes[2].endpoint_incarnation = endpoint.incarnation;
    CHECK(activate_catalog(&pair, 4U, &endpoint, 1U, routes, 3U));
    make_catalog(&endpoint, routes);
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 5U, &endpoint, 1U, routes, 3U
    ) == MEISO_RUNTIME_STALE_ENDPOINT);
    return true;
}

static bool test_core_terminal_invalidates(void)
{
    RuntimePair pair;
    MeisoRuntimeSnapshot snapshot;

    CHECK(init_pair(&pair));
    CHECK(activate(&pair, 1U));
    CHECK(meiso_session_close(
        pair.host_session, MEISO_SESSION_CLOSE_SHUTDOWN
    ) == MEISO_SESSION_CLOSE_QUEUED);
    CHECK(!meiso_runtime_snapshot(pair.host_runtime, &snapshot) ||
          !snapshot.core_ready);
    CHECK(meiso_runtime_find_route(
        pair.host_runtime, DATA_ID,
        &(MeisoRuntimeRouteDescriptor){0}
    ) == MEISO_RUNTIME_CORE_NOT_READY);
    return true;
}

int main(void)
{
    const bool ok = test_payload_contract() &&
        test_catalog_contract() &&
        test_identity_history_capacity() &&
        test_gate_and_core_commit() &&
        test_core_terminal_invalidates();

    if (!ok) {
        return 1;
    }
    (void)printf("runtime tests passed\n");
    return 0;
}
