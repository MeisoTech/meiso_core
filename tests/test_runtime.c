#include "meiso_runtime.h"

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

static const MeisoEndpointId ENDPOINT_1 = {UINT64_C(0x101), UINT64_C(0x102)};
static const MeisoResourceId ANNOUNCED_STATE = {
    UINT64_C(0x201), UINT64_C(0x202)
};
static const MeisoResourceId SUBSCRIBED_STREAM = {
    UINT64_C(0x301), UINT64_C(0x302)
};
static const MeisoSchemaRef CAPABILITY_SCHEMA = {
    {UINT64_C(0x401), UINT64_C(0x402)}, 1U
};
static const MeisoSchemaRef STATE_SCHEMA = {
    {UINT64_C(0x501), UINT64_C(0x502)}, 1U
};
static const MeisoSchemaRef STREAM_SCHEMA = {
    {UINT64_C(0x601), UINT64_C(0x602)}, 1U
};
static const MeisoSchemaRef SELECTION_SCHEMA = {
    {UINT64_C(0x701), UINT64_C(0x702)}, 1U
};

static MeisoSessionConfig session_config(MeisoSessionRole role)
{
    MeisoSessionConfig config;
    const MeisoPeerId host_id = {
        UINT64_C(0x1111111111111111), UINT64_C(0x1111111111111112)
    };
    const MeisoPeerId edge_id = {
        UINT64_C(0x2222222222222221), UINT64_C(0x2222222222222222)
    };

    memset(&config, 0, sizeof(config));
    config.local_role = role;
    config.local_peer_id = role == MEISO_SESSION_ROLE_HOST ? host_id : edge_id;
    config.expected_peer_id =
        role == MEISO_SESSION_ROLE_HOST ? edge_id : host_id;
    config.local_incarnation =
        role == MEISO_SESSION_ROLE_HOST ? UINT64_C(11) : UINT64_C(22);
    config.local_nonce = role == MEISO_SESSION_ROLE_HOST ?
        (MeisoSessionNonce){UINT64_C(0x31), UINT64_C(0x32)} :
        (MeisoSessionNonce){UINT64_C(0x41), UINT64_C(0x42)};
    config.protocol = (MeisoProtocolVersion){1U, 0U};
    config.profile.id = (MeisoProfileId){UINT64_C(0x51), UINT64_C(0x52)};
    config.profile.version = 1U;
    config.limits = (MeisoSessionLimits){8192U, 4096U, 16U};
    return config;
}

static MeisoRuntimeConfig runtime_config(MeisoSession *session)
{
    MeisoRuntimeConfig config;

    memset(&config, 0, sizeof(config));
    config.session = session;
    config.max_endpoints = 4U;
    config.max_routes = 8U;
    config.max_capability_bytes = 128U;
    config.max_parameter_bytes_per_route = 32U;
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

static bool copy_and_deliver_handshake(
    MeisoSession *sender,
    MeisoSession *receiver
)
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

static bool make_sessions_ready_with_limits(
    RuntimePair *pair,
    MeisoSessionLimits limits
)
{
    MeisoSessionConfig host = session_config(MEISO_SESSION_ROLE_HOST);
    MeisoSessionConfig edge = session_config(MEISO_SESSION_ROLE_EDGE);

    host.limits = limits;
    edge.limits = limits;

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
    CHECK(copy_and_deliver_handshake(pair->host_session, pair->edge_session));
    CHECK(copy_and_deliver_handshake(pair->edge_session, pair->host_session));
    CHECK(copy_and_deliver_handshake(pair->host_session, pair->edge_session));
    CHECK(copy_and_deliver_handshake(pair->edge_session, pair->host_session));
    return true;
}

static bool make_sessions_ready(RuntimePair *pair)
{
    const MeisoSessionConfig config =
        session_config(MEISO_SESSION_ROLE_HOST);

    return make_sessions_ready_with_limits(pair, config.limits);
}

static bool initialize_runtime(
    TestArena *arena,
    MeisoSession *session,
    MeisoRuntime **out_runtime
)
{
    const MeisoRuntimeConfig config = runtime_config(session);
    const size_t required = meiso_runtime_memory_required(&config);

    CHECK(required > 0U && required <= sizeof(arena->bytes));
    CHECK(meiso_runtime_init(
        arena->bytes, required, &config, out_runtime
    ) == MEISO_RUNTIME_INIT_OK);
    return true;
}

static bool initialize_pair(RuntimePair *pair)
{
    memset(pair, 0, sizeof(*pair));
    CHECK(make_sessions_ready(pair));
    CHECK(initialize_runtime(
        &pair->host_runtime_arena, pair->host_session, &pair->host_runtime
    ));
    CHECK(initialize_runtime(
        &pair->edge_runtime_arena, pair->edge_session, &pair->edge_runtime
    ));
    return true;
}

static bool transfer_runtime_status(
    MeisoSession *sender_session,
    MeisoSession *receiver_session,
    MeisoRuntime *receiver_runtime,
    MeisoRuntimeEvent *out_event,
    MeisoRuntimeStatus *out_status
)
{
    uint8_t frame[8192];
    MeisoByteView outbound;
    MeisoSessionRuntimeMessage message = {0};

    CHECK(meiso_session_next_outbound(sender_session, &outbound));
    CHECK(outbound.size <= sizeof(frame));
    memmove(frame, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(sender_session) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        receiver_session, (MeisoByteView){frame, outbound.size}, &message
    ) == MEISO_SESSION_RECEIVE_RUNTIME);
    CHECK(out_status != NULL);
    *out_status = meiso_runtime_receive(receiver_runtime, &message, out_event);
    return true;
}

static bool transfer_runtime(
    MeisoSession *sender_session,
    MeisoSession *receiver_session,
    MeisoRuntime *receiver_runtime,
    MeisoRuntimeEvent *out_event
)
{
    MeisoRuntimeStatus status;

    CHECK(transfer_runtime_status(
        sender_session, receiver_session, receiver_runtime,
        out_event, &status
    ));
    CHECK(status == MEISO_RUNTIME_OK);
    return true;
}

static void make_catalog(
    uint64_t endpoint_incarnation,
    MeisoRuntimeEndpointDescriptor *endpoint,
    MeisoRuntimeRouteDescriptor routes[2]
)
{
    static const uint8_t capability[] = {0x10U, 0x20U, 0x30U};

    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->id = ENDPOINT_1;
    endpoint->incarnation = endpoint_incarnation;
    endpoint->capability_schema = CAPABILITY_SCHEMA;
    endpoint->capability =
        (MeisoByteView){capability, sizeof(capability)};

    memset(routes, 0, 2U * sizeof(*routes));
    routes[0].endpoint_id = ENDPOINT_1;
    routes[0].endpoint_incarnation = endpoint_incarnation;
    routes[0].resource.id = ANNOUNCED_STATE;
    routes[0].resource.kind = MEISO_RESOURCE_KIND_STATE;
    routes[0].resource.schema = STATE_SCHEMA;
    routes[0].resource.owner = MEISO_OWNER_ROLE_EDGE;
    routes[0].resource.max_payload_size = 16U;
    routes[0].delivery = MEISO_RUNTIME_DELIVERY_ANNOUNCED;

    routes[1].endpoint_id = ENDPOINT_1;
    routes[1].endpoint_incarnation = endpoint_incarnation;
    routes[1].resource.id = SUBSCRIBED_STREAM;
    routes[1].resource.kind = MEISO_RESOURCE_KIND_STREAM;
    routes[1].resource.schema = STREAM_SCHEMA;
    routes[1].resource.owner = MEISO_OWNER_ROLE_EDGE;
    routes[1].resource.max_payload_size = 16U;
    routes[1].selection_schema = SELECTION_SCHEMA;
    routes[1].max_selection_bytes = 8U;
    routes[1].delivery = MEISO_RUNTIME_DELIVERY_HOST_SUBSCRIPTION;
}

static bool activate_catalog(
    RuntimePair *pair,
    uint64_t generation,
    uint64_t endpoint_incarnation
)
{
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor routes[2];
    MeisoRuntimeEvent event;

    make_catalog(endpoint_incarnation, &endpoint, routes);
    CHECK(meiso_runtime_offer_catalog(
        pair->edge_runtime, generation, &endpoint, 1U, routes, 2U
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair->edge_session, pair->host_session, pair->host_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED);
    CHECK(event.catalog_generation == generation);
    CHECK(meiso_runtime_accept_catalog(pair->host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair->host_session, pair->edge_session, pair->edge_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED);
    CHECK(event.catalog_generation == generation);
    return true;
}

static bool activate_subscription(
    RuntimePair *pair,
    uint64_t request_id,
    uint64_t endpoint_incarnation,
    uint64_t route_epoch
)
{
    static const uint8_t desired_bytes[] = {0x55U, 0x66U};
    static const uint8_t effective_bytes[] = {0x55U};
    MeisoRuntimeSubscriptionDesired desired;
    MeisoRuntimeSubscriptionActual actual;
    MeisoRuntimeEvent event;

    memset(&desired, 0, sizeof(desired));
    desired.request_id = request_id;
    desired.endpoint_id = ENDPOINT_1;
    desired.endpoint_incarnation = endpoint_incarnation;
    desired.resource_id = SUBSCRIBED_STREAM;
    desired.enabled = true;
    desired.parameters =
        (MeisoByteView){desired_bytes, sizeof(desired_bytes)};
    CHECK(meiso_runtime_request_subscription(
        pair->host_runtime, &desired
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair->host_session, pair->edge_session, pair->edge_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_SUBSCRIPTION_DESIRED);
    CHECK(event.desired.request_id == request_id);

    memset(&actual, 0, sizeof(actual));
    actual.request_id = request_id;
    actual.endpoint_id = ENDPOINT_1;
    actual.endpoint_incarnation = endpoint_incarnation;
    actual.resource_id = SUBSCRIBED_STREAM;
    actual.status = MEISO_RUNTIME_ACTUAL_ACTIVE;
    actual.route_epoch = route_epoch;
    actual.effective_parameters =
        (MeisoByteView){effective_bytes, sizeof(effective_bytes)};
    CHECK(meiso_runtime_report_subscription(
        pair->edge_runtime, &actual
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair->edge_session, pair->host_session, pair->host_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_SUBSCRIPTION_ACTUAL);
    CHECK(event.actual.status == MEISO_RUNTIME_ACTUAL_ACTIVE);
    CHECK(event.actual.route_epoch == route_epoch);
    return true;
}

static bool test_init_ready_and_arena_contract(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena session_arena;
    MEISO_TEST_ARENA_STORAGE TestArena runtime_arena;
    const MeisoSessionConfig session_cfg =
        session_config(MEISO_SESSION_ROLE_HOST);
    MeisoRuntimeConfig config;
    MeisoSession *session = NULL;
    MeisoRuntime *runtime = (MeisoRuntime *)(uintptr_t)1U;
    size_t required;

    CHECK(initialize_session(&session_arena, &session_cfg, &session));
    config = runtime_config(session);
    CHECK(meiso_runtime_memory_required(&config) == 0U);
    CHECK(meiso_runtime_init(
        runtime_arena.bytes, sizeof(runtime_arena.bytes), &config, &runtime
    ) == MEISO_RUNTIME_INIT_CORE_NOT_READY);
    CHECK(runtime == NULL);

    {
        MEISO_TEST_ARENA_STORAGE RuntimePair pair;
        memset(&pair, 0, sizeof(pair));
        CHECK(make_sessions_ready(&pair));
        config = runtime_config(pair.host_session);
        required = meiso_runtime_memory_required(&config);
        CHECK(required > 0U && required <= sizeof(runtime_arena.bytes));
        CHECK(meiso_runtime_alignment_required() == _Alignof(max_align_t));
        CHECK(meiso_runtime_init(
            runtime_arena.bytes + 1U, sizeof(runtime_arena.bytes) - 1U,
            &config, &runtime
        ) == MEISO_RUNTIME_INIT_MISALIGNED_ARENA);
        CHECK(runtime == NULL);
        CHECK(meiso_runtime_init(
            runtime_arena.bytes, required - 1U, &config, &runtime
        ) == MEISO_RUNTIME_INIT_ARENA_TOO_SMALL);
        CHECK(runtime == NULL);

        config.max_routes = 16U;
        CHECK(meiso_runtime_memory_required(&config) == 0U);
        CHECK(meiso_runtime_init(
            runtime_arena.bytes, sizeof(runtime_arena.bytes), &config, &runtime
        ) == MEISO_RUNTIME_INIT_INVALID_CONFIG);
        CHECK(runtime == NULL);
        config = runtime_config(pair.host_session);
        config.max_capability_bytes = 4097U;
        CHECK(meiso_runtime_memory_required(&config) == 0U);
    }
    {
        MEISO_TEST_ARENA_STORAGE RuntimePair pair;
        const MeisoSessionLimits limits = {8192U, 27U, 16U};

        memset(&pair, 0, sizeof(pair));
        CHECK(make_sessions_ready_with_limits(&pair, limits));
        config = runtime_config(pair.host_session);
        config.max_capability_bytes = 0U;
        config.max_parameter_bytes_per_route = 0U;
        CHECK(meiso_runtime_memory_required(&config) == 0U);
        runtime = (MeisoRuntime *)(uintptr_t)1U;
        CHECK(meiso_runtime_init(
            runtime_arena.bytes, sizeof(runtime_arena.bytes), &config, &runtime
        ) == MEISO_RUNTIME_INIT_INVALID_CONFIG);
        CHECK(runtime == NULL);
    }
    {
        MEISO_TEST_ARENA_STORAGE RuntimePair pair;
        const MeisoSessionLimits limits = {8192U, 28U, 16U};

        memset(&pair, 0, sizeof(pair));
        CHECK(make_sessions_ready_with_limits(&pair, limits));
        config = runtime_config(pair.host_session);
        config.max_capability_bytes = 0U;
        config.max_parameter_bytes_per_route = 0U;
        required = meiso_runtime_memory_required(&config);
        CHECK(required > 0U && required <= sizeof(runtime_arena.bytes));
        CHECK(meiso_runtime_init(
            runtime_arena.bytes, sizeof(runtime_arena.bytes), &config, &runtime
        ) == MEISO_RUNTIME_INIT_OK);
        CHECK(runtime != NULL);
    }
    return true;
}

static bool test_catalog_atomic_accept_reject_and_lookup(void)
{
    MEISO_TEST_ARENA_STORAGE RuntimePair pair;
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeEndpointDescriptor too_many_endpoints[5] = {0};
    MeisoRuntimeEndpointDescriptor found_endpoint;
    MeisoRuntimeRouteDescriptor routes[2];
    MeisoRuntimeRouteDescriptor found_route;
    MeisoRuntimeSnapshot snapshot;
    MeisoRuntimeEvent event;

    CHECK(initialize_pair(&pair));
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 1U, too_many_endpoints, 5U, NULL, 0U
    ) == MEISO_RUNTIME_CAPACITY);
    CHECK(meiso_runtime_snapshot(pair.edge_runtime, &snapshot));
    CHECK(snapshot.highest_catalog_generation == 0U);
    CHECK(!snapshot.staged_catalog_present);
    make_catalog(11U, &endpoint, routes);
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 1U, &endpoint, 1U, routes, 2U
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_snapshot(pair.edge_runtime, &snapshot));
    CHECK(!snapshot.active_catalog_present);
    CHECK(snapshot.staged_catalog_present);
    CHECK(snapshot.staged_catalog_generation == 1U);
    CHECK(snapshot.highest_catalog_generation == 1U);
    CHECK(snapshot.staged_endpoint_count == 1U);
    CHECK(snapshot.staged_route_count == 2U);
    CHECK(meiso_runtime_staged_endpoint_at(
        pair.edge_runtime, 0U, &found_endpoint
    ) == MEISO_RUNTIME_OK);
    CHECK(found_endpoint.incarnation == 11U);
    CHECK(transfer_runtime(
        pair.edge_session, pair.host_session, pair.host_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED);
    CHECK(meiso_runtime_snapshot(pair.host_runtime, &snapshot));
    CHECK(snapshot.staged_endpoint_count == 1U);
    CHECK(snapshot.staged_route_count == 2U);
    CHECK(meiso_runtime_staged_route_at(
        pair.host_runtime, 1U, &found_route
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_resource_id_equal(
        found_route.resource.id, SUBSCRIBED_STREAM
    ));
    CHECK(meiso_runtime_find_endpoint(
        pair.host_runtime, ENDPOINT_1, &found_endpoint
    ) == MEISO_RUNTIME_NO_CATALOG);
    CHECK(meiso_runtime_accept_catalog(pair.host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_snapshot(pair.host_runtime, &snapshot));
    CHECK(snapshot.active_catalog_present);
    CHECK(snapshot.active_catalog_generation == 1U);
    CHECK(snapshot.active_endpoint_count == 1U);
    CHECK(snapshot.active_route_count == 2U);
    CHECK(!snapshot.staged_catalog_present);
    CHECK(meiso_runtime_find_endpoint(
        pair.host_runtime, ENDPOINT_1, &found_endpoint
    ) == MEISO_RUNTIME_OK);
    CHECK(found_endpoint.incarnation == 11U);
    CHECK(found_endpoint.capability.size == endpoint.capability.size);
    CHECK(memcmp(
        found_endpoint.capability.data, endpoint.capability.data,
        endpoint.capability.size
    ) == 0);
    CHECK(meiso_runtime_find_route(
        pair.host_runtime, SUBSCRIBED_STREAM, &found_route
    ) == MEISO_RUNTIME_OK);
    CHECK(found_route.delivery == MEISO_RUNTIME_DELIVERY_HOST_SUBSCRIPTION);
    CHECK(transfer_runtime(
        pair.host_session, pair.edge_session, pair.edge_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED);

    make_catalog(12U, &endpoint, routes);
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 2U, &endpoint, 1U, routes, 2U
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair.edge_session, pair.host_session, pair.host_runtime, &event
    ));
    CHECK(meiso_runtime_reject_catalog(
        pair.host_runtime, MEISO_RUNTIME_CATALOG_REJECT_POLICY
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_snapshot(pair.host_runtime, &snapshot));
    CHECK(snapshot.active_catalog_generation == 1U);
    CHECK(!snapshot.staged_catalog_present);
    CHECK(transfer_runtime(
        pair.host_session, pair.edge_session, pair.edge_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_REJECTED);
    CHECK(event.catalog_generation == 2U);
    CHECK(event.reject_reason == MEISO_RUNTIME_CATALOG_REJECT_POLICY);
    CHECK(meiso_runtime_snapshot(pair.edge_runtime, &snapshot));
    CHECK(snapshot.active_catalog_generation == 1U);
    CHECK(snapshot.highest_catalog_generation == 2U);
    CHECK(!snapshot.staged_catalog_present);
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 2U, &endpoint, 1U, routes, 2U
    ) == MEISO_RUNTIME_STALE_CATALOG);
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 3U, &endpoint, 1U, routes, 2U
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair.edge_session, pair.host_session, pair.host_runtime, &event
    ));
    CHECK(event.catalog_generation == 3U);
    CHECK(meiso_runtime_reject_catalog(
        pair.host_runtime, MEISO_RUNTIME_CATALOG_REJECT_POLICY
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair.host_session, pair.edge_session, pair.edge_runtime, &event
    ));
    CHECK(event.catalog_generation == 3U);
    return true;
}

static bool test_desired_actual_and_catalog_invalidation(void)
{
    MEISO_TEST_ARENA_STORAGE RuntimePair pair;
    static const uint8_t parameters[] = {0x81U, 0x82U};
    MeisoRuntimeSubscriptionDesired desired;
    MeisoRuntimeSubscriptionActual actual;
    MeisoRuntimeSubscriptionSnapshot subscription;
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor routes[2];
    MeisoRuntimeEvent event;

    CHECK(initialize_pair(&pair));
    CHECK(activate_catalog(&pair, 1U, 11U));

    memset(&desired, 0, sizeof(desired));
    desired.request_id = 1001U;
    desired.endpoint_id = ENDPOINT_1;
    desired.endpoint_incarnation = 11U;
    desired.resource_id = SUBSCRIBED_STREAM;
    desired.enabled = true;
    desired.parameters = (MeisoByteView){parameters, sizeof(parameters)};
    CHECK(meiso_runtime_request_subscription(
        pair.host_runtime, &desired
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_subscription_snapshot(
        pair.host_runtime, SUBSCRIBED_STREAM, &subscription
    ) == MEISO_RUNTIME_OK);
    CHECK(subscription.desired_present);
    CHECK(subscription.desired_enabled);
    CHECK(subscription.request_id == 1001U);
    CHECK(subscription.actual_status == MEISO_RUNTIME_ACTUAL_NONE);
    CHECK(!subscription.route_active);
    CHECK(transfer_runtime(
        pair.host_session, pair.edge_session, pair.edge_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_SUBSCRIPTION_DESIRED);

    memset(&actual, 0, sizeof(actual));
    actual.request_id = 1001U;
    actual.endpoint_id = ENDPOINT_1;
    actual.endpoint_incarnation = 11U;
    actual.resource_id = SUBSCRIBED_STREAM;
    actual.status = MEISO_RUNTIME_ACTUAL_APPLYING;
    CHECK(meiso_runtime_report_subscription(
        pair.edge_runtime, &actual
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_subscription_snapshot(
        pair.edge_runtime, SUBSCRIBED_STREAM, &subscription
    ) == MEISO_RUNTIME_OK);
    CHECK(subscription.actual_status == MEISO_RUNTIME_ACTUAL_APPLYING);
    CHECK(!subscription.route_active);
    CHECK(transfer_runtime(
        pair.edge_session, pair.host_session, pair.host_runtime, &event
    ));
    CHECK(event.actual.status == MEISO_RUNTIME_ACTUAL_APPLYING);

    actual.status = MEISO_RUNTIME_ACTUAL_ACTIVE;
    actual.route_epoch = 7001U;
    actual.effective_parameters =
        (MeisoByteView){parameters, sizeof(parameters)};
    CHECK(meiso_runtime_report_subscription(
        pair.edge_runtime, &actual
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair.edge_session, pair.host_session, pair.host_runtime, &event
    ));
    CHECK(meiso_runtime_subscription_snapshot(
        pair.host_runtime, SUBSCRIBED_STREAM, &subscription
    ) == MEISO_RUNTIME_OK);
    CHECK(subscription.actual_status == MEISO_RUNTIME_ACTUAL_ACTIVE);
    CHECK(subscription.route_active);
    CHECK(subscription.route_epoch == 7001U);

    actual.status = MEISO_RUNTIME_ACTUAL_REJECTED;
    actual.reason = MEISO_RUNTIME_ACTUAL_REASON_ADMISSION_DENIED;
    actual.route_epoch = 0U;
    actual.effective_parameters = (MeisoByteView){NULL, 0U};
    CHECK(meiso_runtime_report_subscription(
        pair.edge_runtime, &actual
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair.edge_session, pair.host_session, pair.host_runtime, &event
    ));
    CHECK(event.actual.status == MEISO_RUNTIME_ACTUAL_REJECTED);
    CHECK(event.actual.reason == MEISO_RUNTIME_ACTUAL_REASON_ADMISSION_DENIED);
    CHECK(meiso_runtime_subscription_snapshot(
        pair.host_runtime, SUBSCRIBED_STREAM, &subscription
    ) == MEISO_RUNTIME_OK);
    CHECK(!subscription.route_active);

    desired.request_id = 1002U;
    desired.enabled = false;
    CHECK(meiso_runtime_request_subscription(
        pair.host_runtime, &desired
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair.host_session, pair.edge_session, pair.edge_runtime, &event
    ));
    actual.status = MEISO_RUNTIME_ACTUAL_ACTIVE;
    actual.reason = MEISO_RUNTIME_ACTUAL_REASON_NONE;
    actual.route_epoch = 8001U;
    CHECK(meiso_runtime_report_subscription(
        pair.edge_runtime, &actual
    ) == MEISO_RUNTIME_REQUEST_MISMATCH);
    CHECK(meiso_runtime_subscription_snapshot(
        pair.edge_runtime, SUBSCRIBED_STREAM, &subscription
    ) == MEISO_RUNTIME_OK);
    CHECK(subscription.request_id == 1002U);
    CHECK(!subscription.desired_enabled);
    CHECK(!subscription.route_active);
    actual.request_id = 1002U;
    CHECK(meiso_runtime_report_subscription(
        pair.edge_runtime, &actual
    ) == MEISO_RUNTIME_CONFLICT);
    actual.status = MEISO_RUNTIME_ACTUAL_DISABLED;
    actual.route_epoch = 0U;
    CHECK(meiso_runtime_report_subscription(
        pair.edge_runtime, &actual
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair.edge_session, pair.host_session, pair.host_runtime, &event
    ));
    CHECK(event.actual.status == MEISO_RUNTIME_ACTUAL_DISABLED);

    desired.request_id = 1003U;
    desired.enabled = true;
    CHECK(meiso_runtime_request_subscription(
        pair.host_runtime, &desired
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair.host_session, pair.edge_session, pair.edge_runtime, &event
    ));
    actual.request_id = 1003U;
    actual.status = MEISO_RUNTIME_ACTUAL_ACTIVE;
    actual.route_epoch = 9001U;
    CHECK(meiso_runtime_report_subscription(
        pair.edge_runtime, &actual
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair.edge_session, pair.host_session, pair.host_runtime, &event
    ));

    make_catalog(12U, &endpoint, routes);
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 2U, &endpoint, 1U, routes, 2U
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair.edge_session, pair.host_session, pair.host_runtime, &event
    ));
    CHECK(meiso_runtime_accept_catalog(pair.host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_subscription_snapshot(
        pair.host_runtime, SUBSCRIBED_STREAM, &subscription
    ) == MEISO_RUNTIME_OK);
    CHECK(!subscription.desired_present);
    CHECK(subscription.actual_status == MEISO_RUNTIME_ACTUAL_NONE);
    CHECK(!subscription.route_active);
    CHECK(transfer_runtime(
        pair.host_session, pair.edge_session, pair.edge_runtime, &event
    ));
    CHECK(meiso_runtime_subscription_snapshot(
        pair.edge_runtime, SUBSCRIBED_STREAM, &subscription
    ) == MEISO_RUNTIME_OK);
    CHECK(!subscription.desired_present);
    CHECK(!subscription.route_active);
    return true;
}

static bool test_catalog_capacity_reject_does_not_deadlock(void)
{
    MEISO_TEST_ARENA_STORAGE RuntimePair pair;
    MeisoRuntimeConfig host_config;
    MeisoRuntimeConfig edge_config;
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor routes[2];
    MeisoRuntimeSnapshot snapshot;
    MeisoRuntimeEvent event;
    MeisoRuntimeStatus receive_status;
    size_t required;

    memset(&pair, 0, sizeof(pair));
    CHECK(make_sessions_ready(&pair));
    host_config = runtime_config(pair.host_session);
    host_config.max_routes = 1U;
    edge_config = runtime_config(pair.edge_session);
    required = meiso_runtime_memory_required(&host_config);
    CHECK(required > 0U && required <= sizeof(pair.host_runtime_arena.bytes));
    CHECK(meiso_runtime_init(
        pair.host_runtime_arena.bytes, required, &host_config,
        &pair.host_runtime
    ) == MEISO_RUNTIME_INIT_OK);
    required = meiso_runtime_memory_required(&edge_config);
    CHECK(required > 0U && required <= sizeof(pair.edge_runtime_arena.bytes));
    CHECK(meiso_runtime_init(
        pair.edge_runtime_arena.bytes, required, &edge_config,
        &pair.edge_runtime
    ) == MEISO_RUNTIME_INIT_OK);

    make_catalog(11U, &endpoint, routes);
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 1U, &endpoint, 1U, routes, 2U
    ) == MEISO_RUNTIME_OK);
    memset(&event, 0, sizeof(event));
    CHECK(transfer_runtime_status(
        pair.edge_session, pair.host_session, pair.host_runtime,
        &event, &receive_status
    ));
    CHECK(receive_status == MEISO_RUNTIME_CAPACITY);
    CHECK(meiso_runtime_snapshot(pair.host_runtime, &snapshot));
    CHECK(snapshot.catalog_decision_pending);
    CHECK(snapshot.pending_catalog_generation == 1U);
    CHECK(snapshot.highest_catalog_generation == 1U);
    CHECK(!snapshot.staged_catalog_present);
    CHECK(meiso_runtime_accept_catalog(pair.host_runtime) ==
          MEISO_RUNTIME_NO_CATALOG);
    CHECK(meiso_runtime_reject_catalog(
        pair.host_runtime, MEISO_RUNTIME_CATALOG_REJECT_CAPACITY
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair.host_session, pair.edge_session, pair.edge_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_REJECTED);
    CHECK(event.catalog_generation == 1U);
    CHECK(event.reject_reason == MEISO_RUNTIME_CATALOG_REJECT_CAPACITY);
    CHECK(meiso_runtime_snapshot(pair.edge_runtime, &snapshot));
    CHECK(!snapshot.catalog_decision_pending);
    CHECK(snapshot.highest_catalog_generation == 1U);
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 1U, &endpoint, 1U, routes, 2U
    ) == MEISO_RUNTIME_STALE_CATALOG);
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 2U, &endpoint, 1U, routes, 2U
    ) == MEISO_RUNTIME_OK);
    return true;
}

static bool test_catalog_tx_busy_is_retriable(void)
{
    MEISO_TEST_ARENA_STORAGE RuntimePair pair;
    static const uint8_t occupied_payload[] = {0xA5U};
    uint8_t frame[8192];
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor routes[2];
    MeisoRuntimeSnapshot snapshot;
    MeisoSessionRuntimeMessage ignored = {0};
    MeisoByteView outbound;

    CHECK(initialize_pair(&pair));
    make_catalog(11U, &endpoint, routes);
    CHECK(meiso_session_submit_runtime(
        pair.edge_session,
        (MeisoByteView){occupied_payload, sizeof(occupied_payload)}
    ) == MEISO_SESSION_SUBMIT_QUEUED);
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 1U, &endpoint, 1U, routes, 2U
    ) == MEISO_RUNTIME_TX_BUSY);
    CHECK(meiso_runtime_snapshot(pair.edge_runtime, &snapshot));
    CHECK(snapshot.highest_catalog_generation == 0U);
    CHECK(!snapshot.catalog_decision_pending);

    CHECK(meiso_session_next_outbound(pair.edge_session, &outbound));
    CHECK(outbound.size <= sizeof(frame));
    memmove(frame, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(pair.edge_session) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        pair.host_session, (MeisoByteView){frame, outbound.size}, &ignored
    ) == MEISO_SESSION_RECEIVE_RUNTIME);
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 1U, &endpoint, 1U, routes, 2U
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_snapshot(pair.edge_runtime, &snapshot));
    CHECK(snapshot.highest_catalog_generation == 1U);
    CHECK(snapshot.catalog_decision_pending);
    return true;
}

static bool test_resource_route_gate(void)
{
    MEISO_TEST_ARENA_STORAGE RuntimePair pair;
    const uint8_t payload[] = {1U, 2U, 3U};
    uint8_t oversized[17] = {0};
    MeisoRuntimeResourceMessage message;
    MeisoRuntimeEvent event;
    MeisoSessionSnapshot host_session_snapshot;
    MeisoSessionRuntimeMessage wrong_session_message;

    CHECK(initialize_pair(&pair));
    CHECK(activate_catalog(&pair, 1U, 11U));
    memset(&message, 0, sizeof(message));
    message.endpoint_id = ENDPOINT_1;
    message.endpoint_incarnation = 11U;
    message.resource_id = ANNOUNCED_STATE;
    message.kind = MEISO_RESOURCE_KIND_STATE;
    message.schema = STATE_SCHEMA;
    message.payload = (MeisoByteView){payload, sizeof(payload)};
    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &message
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair.edge_session, pair.host_session, pair.host_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_RESOURCE);
    CHECK(event.resource.route_epoch == 0U);
    CHECK(event.resource.payload.size == sizeof(payload));
    CHECK(memcmp(event.resource.payload.data, payload, sizeof(payload)) == 0);
    message.request_id = 1U;
    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &message
    ) == MEISO_RUNTIME_ROUTE_NOT_ACTIVE);
    message.request_id = 0U;

    message.resource_id = SUBSCRIBED_STREAM;
    message.kind = MEISO_RESOURCE_KIND_STREAM;
    message.schema = STREAM_SCHEMA;
    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &message
    ) == MEISO_RUNTIME_ROUTE_NOT_ACTIVE);
    CHECK(activate_subscription(&pair, 41U, 11U, 51U));
    message.request_id = 41U;
    message.route_epoch = 51U;

    message.endpoint_id = (MeisoEndpointId){0x999U, 0x998U};
    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &message
    ) == MEISO_RUNTIME_UNKNOWN_ENDPOINT);
    message.endpoint_id = ENDPOINT_1;
    message.endpoint_incarnation = 12U;
    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &message
    ) == MEISO_RUNTIME_STALE_ENDPOINT);
    message.endpoint_incarnation = 11U;
    message.resource_id = (MeisoResourceId){0x888U, 0x887U};
    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &message
    ) == MEISO_RUNTIME_UNKNOWN_RESOURCE);
    message.resource_id = SUBSCRIBED_STREAM;
    message.kind = MEISO_RESOURCE_KIND_STATE;
    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &message
    ) == MEISO_RUNTIME_UNSUPPORTED_KIND);
    message.kind = MEISO_RESOURCE_KIND_STREAM;
    message.schema.version = 2U;
    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &message
    ) == MEISO_RUNTIME_SCHEMA_MISMATCH);
    message.schema = STREAM_SCHEMA;
    message.request_id = 42U;
    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &message
    ) == MEISO_RUNTIME_ROUTE_NOT_ACTIVE);
    message.request_id = 41U;
    message.route_epoch = 52U;
    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &message
    ) == MEISO_RUNTIME_ROUTE_NOT_ACTIVE);
    message.route_epoch = 51U;
    message.payload = (MeisoByteView){oversized, sizeof(oversized)};
    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &message
    ) == MEISO_RUNTIME_PAYLOAD_CAPACITY);
    message.payload = (MeisoByteView){payload, sizeof(payload)};
    CHECK(meiso_runtime_send_resource(
        pair.host_runtime, &message
    ) == MEISO_RUNTIME_OWNER_MISMATCH);
    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &message
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair.edge_session, pair.host_session, pair.host_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_RESOURCE);
    CHECK(event.resource.route_epoch == 51U);

    CHECK(meiso_session_snapshot(pair.host_session, &host_session_snapshot));
    memset(&wrong_session_message, 0, sizeof(wrong_session_message));
    wrong_session_message.session_id = host_session_snapshot.session_id;
    wrong_session_message.session_id.edge_nonce.low ^= 1U;
    wrong_session_message.payload = (MeisoByteView){payload, sizeof(payload)};
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &wrong_session_message, &event
    ) == MEISO_RUNTIME_STALE_SESSION);

    CHECK(meiso_session_close(
        pair.edge_session, MEISO_SESSION_CLOSE_NORMAL
    ) == MEISO_SESSION_CLOSE_QUEUED);
    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &message
    ) == MEISO_RUNTIME_CORE_NOT_READY);
    {
        MeisoRuntimeSnapshot closed_snapshot;
        CHECK(meiso_runtime_snapshot(pair.edge_runtime, &closed_snapshot));
        CHECK(!closed_snapshot.core_ready);
        CHECK(!closed_snapshot.active_catalog_present);
        CHECK(!closed_snapshot.staged_catalog_present);
    }
    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &message
    ) == MEISO_RUNTIME_STALE_SESSION);
    return true;
}

int main(void)
{
    if (!test_init_ready_and_arena_contract() ||
        !test_catalog_atomic_accept_reject_and_lookup() ||
        !test_desired_actual_and_catalog_invalidation() ||
        !test_catalog_capacity_reject_does_not_deadlock() ||
        !test_catalog_tx_busy_is_retriable() ||
        !test_resource_route_gate()) {
        return 1;
    }
    (void)printf("runtime tests passed\n");
    return 0;
}
