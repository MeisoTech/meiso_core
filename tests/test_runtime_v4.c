#include <meiso/profiles/composition/profile.h>
#include <meiso/runtime/runtime.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(                                                      \
                stderr, "CHECK failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition                                  \
            );                                                                  \
            return false;                                                       \
        }                                                                       \
    } while (0)

enum {
    CATALOG_ROUTE_OFFSET =
        MEISO_RUNTIME_WIRE_HEADER_SIZE + 16U +
        MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT * 48U +
        MEISO_COMPOSITION_PROFILE_CAPABILITY_BYTES,
    RENDER_DESIRED_ROUTE = 12U,
    RENDER_ACTUAL_ROUTE = 13U,
    RENDER_DATA_ROUTE = 14U,
    CAMERA_DATA_ROUTE = 17U,
    SCENE_BLOB_ROUTE = 7U,
    WIFI_DESIRED_ROUTE = 0U,
    WIFI_ACTUAL_ROUTE = 1U,
    WIFI_DATA_ROUTE = 2U
};

typedef union {
    max_align_t alignment;
    uint8_t bytes[65536];
} SessionArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[300000];
} RuntimeArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[1024];
} StateArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[1100000];
} BlobArena;

typedef struct {
    SessionArena host_session_arena;
    SessionArena edge_session_arena;
    RuntimeArena host_runtime_arena;
    RuntimeArena edge_runtime_arena;
    uint8_t transfer[MEISO_COMPOSITION_PROFILE_MAX_FRAME_BYTES];
    MeisoSession *host_session;
    MeisoSession *edge_session;
    MeisoRuntime *host_runtime;
    MeisoRuntime *edge_runtime;
    size_t host_session_size;
    size_t edge_session_size;
    size_t host_runtime_size;
    size_t edge_runtime_size;
} RuntimePair;

typedef struct {
    uint8_t capability[MEISO_COMPOSITION_PROFILE_DYNAMIC_CAPABILITY_BYTES];
    MeisoRuntimeEndpointDescriptor endpoints[
        MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT
    ];
    MeisoCompositionProfileRouteDescriptor routes[
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ];
} Catalog;

typedef struct {
    StateArena render_desired_arena;
    StateArena render_actual_arena;
    StateArena wifi_desired_arena;
    StateArena wifi_actual_arena;
    MeisoStateReplica *render_desired;
    MeisoStateReplica *render_actual;
    MeisoStateReplica *wifi_desired;
    MeisoStateReplica *wifi_actual;
} Replicas;

static const MeisoOwnerScope HOST_OWNER = {MEISO_OWNER_ROLE_HOST, 501U};
static const MeisoOwnerScope EDGE_OWNER = {MEISO_OWNER_ROLE_EDGE, 601U};
static const uint8_t BLOB_BYTES[] = {0xe1U, 0xe2U, 0xe3U, 0xe4U};
static const uint8_t BLOB_SHA256[MEISO_SHA256_DIGEST_SIZE] = {
    0xb0U, 0x06U, 0x7bU, 0x7aU, 0xebU, 0x07U, 0x9cU, 0x61U,
    0x78U, 0x80U, 0x6bU, 0x87U, 0x8dU, 0x06U, 0x0cU, 0x75U,
    0x23U, 0x6cU, 0xf2U, 0xa1U, 0xcfU, 0x65U, 0x8cU, 0x14U,
    0x93U, 0x23U, 0x50U, 0x4aU, 0x31U, 0x35U, 0xf1U, 0x5dU
};

static bool bytes_are(const uint8_t *bytes, uint8_t value, size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != value) {
            return false;
        }
    }
    return true;
}

static uint64_t fnv1a64(const uint8_t *bytes, size_t size)
{
    uint64_t value = UINT64_C(0xcbf29ce484222325);
    size_t index;

    for (index = 0U; index < size; ++index) {
        value ^= bytes[index];
        value *= UINT64_C(0x100000001b3);
    }
    return value;
}

static bool owner_equal(MeisoOwnerScope lhs, MeisoOwnerScope rhs)
{
    return lhs.role == rhs.role && lhs.incarnation == rhs.incarnation;
}

static bool endpoint_equal(MeisoEndpointId lhs, MeisoEndpointId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

static bool resource_equal(MeisoResourceId lhs, MeisoResourceId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

static bool session_id_equal(MeisoSessionId lhs, MeisoSessionId rhs)
{
    return lhs.host_nonce.high == rhs.host_nonce.high &&
        lhs.host_nonce.low == rhs.host_nonce.low &&
        lhs.edge_nonce.high == rhs.edge_nonce.high &&
        lhs.edge_nonce.low == rhs.edge_nonce.low;
}

static bool route_key_equal(
    MeisoRuntimeRouteKey lhs,
    MeisoRuntimeRouteKey rhs
)
{
    return lhs.catalog_generation == rhs.catalog_generation &&
        endpoint_equal(lhs.endpoint_id, rhs.endpoint_id) &&
        lhs.endpoint_incarnation == rhs.endpoint_incarnation &&
        resource_equal(lhs.resource_id, rhs.resource_id) &&
        lhs.facet == rhs.facet;
}

static MeisoCompositionProfileCatalogConfig catalog_config(void)
{
    MeisoCompositionProfileCatalogConfig value;

    memset(&value, 0, sizeof(value));
    value.wifi_incarnation = 11U;
    value.presentation_incarnation = 12U;
    value.scene_incarnation = 13U;
    value.device_telemetry_incarnation = 14U;
    value.render_media_incarnation = 15U;
    value.camera_media_incarnation = 16U;
    value.wifi_capability = (MeisoWifiTelemetryCapability){
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
            MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM,
        100U, 1000U, 100U
    };
    value.device_telemetry_capability = (MeisoDeviceTelemetryCapability){
        MEISO_DEVICE_TELEMETRY_FIELD_ALL,
        100U, 1000U, 100U, -40000, 125000, 1000U
    };
    value.render_media_capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888 |
            MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGB565,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        64U, 48U, 320U, 240U, 10000U,
        MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES,
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE, 0U
    };
    value.camera_media_capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        320U, 240U, 640U, 480U, 30000U,
        MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES,
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE, 0U
    };
    return value;
}

static bool build_catalog(Catalog *catalog)
{
    const MeisoCompositionProfileCatalogConfig config = catalog_config();

    memset(catalog, 0, sizeof(*catalog));
    CHECK(meiso_composition_profile_build_catalog(
        &config,
        (MeisoMutableBuffer){catalog->capability, sizeof(catalog->capability)},
        catalog->endpoints, MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT,
        catalog->routes, MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ) == MEISO_COMPOSITION_PROFILE_OK);
    return true;
}

static MeisoSessionConfig session_config(
    MeisoSessionRole role,
    uint64_t nonce_salt
)
{
    MeisoSessionConfig value;
    const MeisoPeerId host = {0x11U, 0x12U};
    const MeisoPeerId edge = {0x21U, 0x22U};

    memset(&value, 0, sizeof(value));
    value.local_role = role;
    value.local_peer_id = role == MEISO_SESSION_ROLE_HOST ? host : edge;
    value.expected_peer_id = role == MEISO_SESSION_ROLE_HOST ? edge : host;
    value.local_incarnation = role == MEISO_SESSION_ROLE_HOST ? 31U : 41U;
    value.local_nonce = role == MEISO_SESSION_ROLE_HOST ?
        (MeisoSessionNonce){0x61U, nonce_salt + 1U} :
        (MeisoSessionNonce){0x71U, nonce_salt + 2U};
    value.protocol = (MeisoProtocolVersion){1U, 0U};
    value.profile = *meiso_composition_profile_ref();
    value.limits = (MeisoSessionLimits){
        MEISO_COMPOSITION_PROFILE_MAX_FRAME_BYTES,
        MEISO_COMPOSITION_PROFILE_MAX_RUNTIME_PAYLOAD_BYTES,
        MEISO_COMPOSITION_PROFILE_MAX_REGISTRY_ENTRIES
    };
    return value;
}

static bool init_session(
    SessionArena *arena,
    const MeisoSessionConfig *config,
    MeisoSession **out_session,
    size_t *out_size
)
{
    const size_t required = meiso_session_memory_required(config);

    CHECK(required != 0U && required <= sizeof(arena->bytes));
    CHECK(meiso_session_init(
        arena->bytes, required, config, out_session
    ) == MEISO_SESSION_INIT_OK);
    *out_size = required;
    return true;
}

static bool handshake(MeisoSession *sender, MeisoSession *receiver)
{
    uint8_t frame[MEISO_COMPOSITION_PROFILE_MAX_FRAME_BYTES];
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

static MeisoRuntimeV4Config runtime_config(MeisoSession *session)
{
    MeisoRuntimeV4Config value;

    memset(&value, 0, sizeof(value));
    value.base.session = session;
    value.base.max_endpoints = MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT;
    value.base.max_endpoint_history =
        MEISO_COMPOSITION_PROFILE_MAX_ENDPOINT_HISTORY;
    value.base.max_routes = MEISO_COMPOSITION_PROFILE_ROUTE_COUNT;
    value.base.max_capability_bytes =
        MEISO_COMPOSITION_PROFILE_CAPABILITY_BYTES;
    value.base.max_desired_profile_bytes_per_endpoint =
        MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE;
    value.base.max_actual_profile_bytes_per_endpoint =
        MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE;
    value.max_blob_transfer_bytes = 262144U;
    return value;
}

static bool init_runtime(
    RuntimeArena *arena,
    MeisoSession *session,
    MeisoRuntime **out_runtime,
    size_t *out_size
)
{
    const MeisoRuntimeV4Config config = runtime_config(session);
    const size_t required = meiso_runtime_v4_memory_required(&config);

    CHECK(required != 0U && required <= sizeof(arena->bytes));
    memset(arena->bytes, 0xa5, sizeof(arena->bytes));
    CHECK(meiso_runtime_v4_init(
        arena->bytes, required, &config, out_runtime
    ) == MEISO_RUNTIME_INIT_OK);
    CHECK(bytes_are(
        arena->bytes + required, 0xa5, sizeof(arena->bytes) - required
    ));
    *out_size = required;
    return true;
}

static bool init_pair(RuntimePair *pair, uint64_t nonce_salt)
{
    const MeisoSessionConfig host = session_config(
        MEISO_SESSION_ROLE_HOST, nonce_salt
    );
    const MeisoSessionConfig edge = session_config(
        MEISO_SESSION_ROLE_EDGE, nonce_salt
    );

    memset(pair, 0, sizeof(*pair));
    CHECK(init_session(
        &pair->host_session_arena, &host, &pair->host_session,
        &pair->host_session_size
    ));
    CHECK(init_session(
        &pair->edge_session_arena, &edge, &pair->edge_session,
        &pair->edge_session_size
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
        &pair->host_runtime_arena, pair->host_session, &pair->host_runtime,
        &pair->host_runtime_size
    ));
    CHECK(init_runtime(
        &pair->edge_runtime_arena, pair->edge_session, &pair->edge_runtime,
        &pair->edge_runtime_size
    ));
    return true;
}

static bool pull_message(
    RuntimePair *pair,
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoSessionRuntimeMessage *out_message
)
{
    MeisoByteView outbound;

    CHECK(meiso_session_next_outbound(sender, &outbound));
    CHECK(outbound.size <= sizeof(pair->transfer));
    memmove(pair->transfer, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(sender) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        receiver, (MeisoByteView){pair->transfer, outbound.size}, out_message
    ) == MEISO_SESSION_RECEIVE_RUNTIME);
    return true;
}

static bool transfer_runtime(
    RuntimePair *pair,
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoRuntime *receiver_runtime,
    MeisoRuntimeEvent *out_event,
    MeisoRuntimeStatus expected
)
{
    MeisoSessionRuntimeMessage message = {0};

    CHECK(pull_message(pair, sender, receiver, &message));
    CHECK(meiso_runtime_receive(receiver_runtime, &message, out_event) ==
          expected);
    return true;
}

static MeisoRuntimeRouteKey route_key(
    const Catalog *catalog,
    size_t route_index,
    uint64_t generation
)
{
    const MeisoRuntimeRouteDescriptor *route =
        &catalog->routes[route_index].route;
    const MeisoRuntimeRouteKey value = {
        generation,
        route->endpoint_id,
        route->endpoint_incarnation,
        route->resource.id,
        route->facet
    };
    return value;
}

static bool init_replica(
    StateArena *arena,
    const MeisoResourceDescriptor *descriptor,
    MeisoOwnerScope owner,
    MeisoStateReplica **out_replica
)
{
    const size_t required = meiso_state_replica_memory_required(descriptor);

    CHECK(required != 0U && required <= sizeof(arena->bytes));
    CHECK(meiso_state_replica_init(
        arena->bytes, required, descriptor, out_replica
    ) == MEISO_STATE_INIT_OK);
    CHECK(meiso_state_replica_begin_owner(*out_replica, owner) ==
          MEISO_STATE_OWNER_STARTED);
    return true;
}

static bool init_replicas(Replicas *replicas, const Catalog *catalog)
{
    memset(replicas, 0, sizeof(*replicas));
    CHECK(init_replica(
        &replicas->render_desired_arena,
        &catalog->routes[RENDER_DESIRED_ROUTE].route.resource,
        HOST_OWNER, &replicas->render_desired
    ));
    CHECK(init_replica(
        &replicas->render_actual_arena,
        &catalog->routes[RENDER_ACTUAL_ROUTE].route.resource,
        EDGE_OWNER, &replicas->render_actual
    ));
    CHECK(init_replica(
        &replicas->wifi_desired_arena,
        &catalog->routes[WIFI_DESIRED_ROUTE].route.resource,
        HOST_OWNER, &replicas->wifi_desired
    ));
    CHECK(init_replica(
        &replicas->wifi_actual_arena,
        &catalog->routes[WIFI_ACTUAL_ROUTE].route.resource,
        EDGE_OWNER, &replicas->wifi_actual
    ));
    return true;
}

static bool encode_desired(
    bool enabled,
    uint8_t output[MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE],
    MeisoByteView *out_payload
)
{
    const MeisoRuntimeDesiredState desired = {enabled, {NULL, 0U}};

    CHECK(meiso_runtime_desired_state_encode(
        &desired,
        (MeisoMutableBuffer){
            output, MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE
        },
        out_payload
    ));
    return true;
}

static bool encode_running_actual(
    uint64_t desired_version,
    uint64_t production_epoch,
    uint8_t output[MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE],
    MeisoByteView *out_payload
)
{
    MeisoRuntimeActualState actual;

    memset(&actual, 0, sizeof(actual));
    actual.observed_desired = (MeisoRuntimeDesiredReference){
        true, HOST_OWNER, desired_version
    };
    actual.applied_desired = actual.observed_desired;
    actual.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    actual.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    actual.effective_enabled = true;
    actual.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING;
    actual.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    actual.production_epoch = production_epoch;
    CHECK(meiso_runtime_actual_state_encode(
        &actual,
        (MeisoMutableBuffer){
            output, MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE
        },
        out_payload
    ));
    return true;
}

static bool deliver_offer(
    RuntimePair *pair,
    MeisoRuntime *sender_runtime,
    MeisoSession *sender_session,
    MeisoRuntime *receiver_runtime,
    MeisoSession *receiver_session,
    MeisoStateReplica *replica,
    MeisoRuntimeRouteKey key,
    const MeisoStateOffer *offer
)
{
    MeisoRuntimeEvent event = {0};
    MeisoStateReceiveStatus receive_status;

    CHECK(meiso_runtime_send_state_offer(
        sender_runtime, key, offer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, sender_session, receiver_session, receiver_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER &&
          route_key_equal(event.route, key));
    receive_status = meiso_state_replica_receive(
        replica, event.state_offer.state.owner,
        event.state_offer.state.version, event.state_offer.state.payload
    );
    CHECK(receive_status == MEISO_STATE_RECEIVE_NEW);
    CHECK(meiso_runtime_commit_state_receive(
        receiver_runtime, event.route, &event.state_offer, receive_status
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_state_accept(
        receiver_runtime, event.route, event.state_offer.generation,
        event.state_offer.state.owner, event.state_offer.state.version
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, receiver_session, sender_session, sender_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT &&
          event.state_accept_generation.value == offer->generation.value &&
          owner_equal(event.state_accept_owner, offer->state.owner) &&
          event.state_accept_version == offer->state.version);
    return true;
}

static bool enable_endpoint(
    RuntimePair *pair,
    const Catalog *catalog,
    MeisoStateReplica *desired_replica,
    MeisoStateReplica *actual_replica,
    size_t desired_route,
    size_t actual_route,
    uint64_t catalog_generation,
    uint64_t desired_version,
    uint64_t actual_version,
    uint64_t production_epoch
)
{
    uint8_t desired_wire[MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE];
    uint8_t actual_wire[MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE];
    MeisoByteView desired_payload;
    MeisoByteView actual_payload;
    MeisoStateOffer offer;

    CHECK(encode_desired(true, desired_wire, &desired_payload));
    offer = (MeisoStateOffer){
        {desired_version}, {HOST_OWNER, desired_version, desired_payload}
    };
    CHECK(deliver_offer(
        pair, pair->host_runtime, pair->host_session,
        pair->edge_runtime, pair->edge_session, desired_replica,
        route_key(catalog, desired_route, catalog_generation), &offer
    ));

    CHECK(encode_running_actual(
        desired_version, production_epoch, actual_wire, &actual_payload
    ));
    offer = (MeisoStateOffer){
        {actual_version}, {EDGE_OWNER, actual_version, actual_payload}
    };
    CHECK(deliver_offer(
        pair, pair->edge_runtime, pair->edge_session,
        pair->host_runtime, pair->host_session, actual_replica,
        route_key(catalog, actual_route, catalog_generation), &offer
    ));
    return true;
}

static MeisoStreamSample sample(
    uint64_t stream_epoch,
    uint64_t sequence,
    const uint8_t *payload,
    size_t payload_size
)
{
    MeisoStreamSample value;

    memset(&value, 0, sizeof(value));
    value.key.producer = EDGE_OWNER;
    value.key.epoch = stream_epoch;
    value.key.sequence = sequence;
    value.source_time = (MeisoTimePoint){9U, sequence * 100U};
    value.payload = (MeisoByteView){payload, payload_size};
    return value;
}

static void put_u64(uint8_t *target, uint64_t value)
{
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        target[index] = (uint8_t)(value >> (56U - index * 8U));
    }
}

static void patch_route_key(
    uint8_t *wire,
    const MeisoRuntimeRouteKey *key
)
{
    uint8_t *body = wire + MEISO_RUNTIME_WIRE_HEADER_SIZE;

    put_u64(body, key->catalog_generation);
    put_u64(body + 8U, key->endpoint_id.high);
    put_u64(body + 16U, key->endpoint_id.low);
    put_u64(body + 24U, key->endpoint_incarnation);
    put_u64(body + 32U, key->resource_id.high);
    put_u64(body + 40U, key->resource_id.low);
    body[48U] = key->facet;
    memset(body + 49U, 0, 7U);
}

static bool test_host_owned_data_direction(
    RuntimePair *pair,
    Catalog *catalog,
    Replicas *replicas
)
{
    const uint8_t payload[] = {0x71U, 0x72U, 0x73U};
    MeisoRuntimeEvent event = {0};
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeV4ExternalMediaBinding host_binding;
    MeisoRuntimeV4ExternalMediaBinding edge_binding;
    MeisoStreamSample stream = sample(41U, 1U, payload, sizeof(payload));
    const MeisoRuntimeRouteKey wifi_key = route_key(
        catalog, WIFI_DATA_ROUTE, 1U
    );
    const MeisoRuntimeRouteKey render_key = route_key(
        catalog, RENDER_DATA_ROUTE, 1U
    );
    uint8_t hostile[512];
    uint8_t runtime_before[300000];
    uint8_t session_before[65536];

    /*
     * This deliberately tests generic Runtime v4, not Composition Profile
     * conformance. The Composition builder supplies stable descriptors and
     * the fixture changes only DATA ownership to exercise Runtime's accepted
     * set without inventing a second catalog builder in this unit test.
     */
    catalog->routes[WIFI_DATA_ROUTE].route.resource.owner =
        MEISO_OWNER_ROLE_HOST;
    catalog->routes[RENDER_DATA_ROUTE].route.resource.owner =
        MEISO_OWNER_ROLE_HOST;
    CHECK(meiso_runtime_v4_offer_catalog(
        pair->edge_runtime, 1U, catalog->endpoints,
        MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT, catalog->routes,
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, pair->edge_session, pair->host_session, pair->host_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED &&
          event.catalog_generation == 1U);
    CHECK(meiso_runtime_accept_catalog(pair->host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, pair->host_session, pair->edge_session, pair->edge_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED &&
          event.catalog_generation == 1U);

    memset(&host_binding, 0xa5, sizeof(host_binding));
    CHECK(meiso_runtime_v4_external_media_binding(
        pair->host_runtime,
        catalog->routes[RENDER_DATA_ROUTE].route.resource.id,
        &host_binding
    ) == MEISO_RUNTIME_ROUTE_NOT_ACTIVE);
    CHECK(bytes_are(
        (const uint8_t *)&host_binding, 0xa5, sizeof(host_binding)
    ));
    memset(&edge_binding, 0x5a, sizeof(edge_binding));
    CHECK(meiso_runtime_v4_external_media_binding(
        pair->edge_runtime,
        catalog->routes[RENDER_DATA_ROUTE].route.resource.id,
        &edge_binding
    ) == MEISO_RUNTIME_ROUTE_NOT_ACTIVE);
    CHECK(bytes_are(
        (const uint8_t *)&edge_binding, 0x5a, sizeof(edge_binding)
    ));
    CHECK(init_replicas(replicas, catalog));
    CHECK(enable_endpoint(
        pair, catalog, replicas->render_desired, replicas->render_actual,
        RENDER_DESIRED_ROUTE, RENDER_ACTUAL_ROUTE, 1U, 1U, 1U, 91U
    ));
    CHECK(enable_endpoint(
        pair, catalog, replicas->wifi_desired, replicas->wifi_actual,
        WIFI_DESIRED_ROUTE, WIFI_ACTUAL_ROUTE, 1U, 1U, 1U, 92U
    ));

    CHECK(meiso_runtime_v4_external_media_binding(
        pair->host_runtime,
        catalog->routes[RENDER_DATA_ROUTE].route.resource.id,
        &host_binding
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_v4_external_media_binding(
        pair->edge_runtime,
        catalog->routes[RENDER_DATA_ROUTE].route.resource.id,
        &edge_binding
    ) == MEISO_RUNTIME_OK);
    CHECK(host_binding.resource.owner == MEISO_OWNER_ROLE_HOST &&
          edge_binding.resource.owner == MEISO_OWNER_ROLE_HOST &&
          route_key_equal(host_binding.route, render_key) &&
          route_key_equal(edge_binding.route, render_key) &&
          host_binding.production_epoch == 91U &&
          edge_binding.production_epoch == 91U);

    stream.key.producer = HOST_OWNER;
    CHECK(meiso_runtime_send_stream_sample(
        pair->host_runtime, wifi_key, 92U, &stream
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, pair->host_session, pair->edge_session, pair->edge_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          route_key_equal(event.route, wifi_key) &&
          event.production_epoch == 92U &&
          owner_equal(event.stream_sample.key.producer, HOST_OWNER) &&
          event.stream_sample.payload.size == sizeof(payload) &&
          memcmp(event.stream_sample.payload.data, payload,
                 sizeof(payload)) == 0);

    stream.key.sequence = 2U;
    CHECK(meiso_runtime_send_stream_sample(
        pair->host_runtime, wifi_key, 92U, &stream
    ) == MEISO_RUNTIME_OK);
    CHECK(pull_message(
        pair, pair->host_session, pair->edge_session, &message
    ));
    CHECK(message.payload.size <= sizeof(hostile));
    memmove(hostile, message.payload.data, message.payload.size);
    CHECK(meiso_session_submit_runtime(
        pair->edge_session, (MeisoByteView){hostile, message.payload.size}
    ) == MEISO_SESSION_SUBMIT_QUEUED);
    CHECK(pull_message(
        pair, pair->edge_session, pair->host_session, &message
    ));
    memmove(runtime_before, pair->host_runtime_arena.bytes,
            pair->host_runtime_size);
    memset(&event, 0xa5, sizeof(event));
    CHECK(meiso_runtime_receive(
        pair->host_runtime, &message, &event
    ) == MEISO_RUNTIME_WRONG_ROLE);
    CHECK(memcmp(runtime_before, pair->host_runtime_arena.bytes,
                 pair->host_runtime_size) == 0 &&
          bytes_are((const uint8_t *)&event, 0xa5, sizeof(event)));
    stream.key.sequence = 3U;
    CHECK(meiso_runtime_send_stream_sample(
        pair->host_runtime, wifi_key, 92U, &stream
    ) == MEISO_RUNTIME_OK);
    CHECK(pull_message(
        pair, pair->host_session, pair->edge_session, &message
    ));
    CHECK(message.payload.size <= sizeof(hostile));
    memmove(hostile, message.payload.data, message.payload.size);
    hostile[MEISO_RUNTIME_WIRE_HEADER_SIZE + 64U] =
        MEISO_OWNER_ROLE_EDGE;
    message.payload = (MeisoByteView){hostile, message.payload.size};
    memmove(runtime_before, pair->edge_runtime_arena.bytes,
            pair->edge_runtime_size);
    memset(&event, 0x5a, sizeof(event));
    CHECK(meiso_runtime_receive(
        pair->edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OWNER_MISMATCH);
    CHECK(memcmp(runtime_before, pair->edge_runtime_arena.bytes,
                 pair->edge_runtime_size) == 0 &&
          bytes_are((const uint8_t *)&event, 0x5a, sizeof(event)));

    stream.key.producer = HOST_OWNER;
    stream.key.sequence = 4U;
    memmove(runtime_before, pair->edge_runtime_arena.bytes,
            pair->edge_runtime_size);
    memmove(session_before, pair->edge_session_arena.bytes,
            pair->edge_session_size);
    CHECK(meiso_runtime_send_stream_sample(
        pair->edge_runtime, wifi_key, 92U, &stream
    ) == MEISO_RUNTIME_WRONG_ROLE);
    CHECK(memcmp(runtime_before, pair->edge_runtime_arena.bytes,
                 pair->edge_runtime_size) == 0 &&
          memcmp(session_before, pair->edge_session_arena.bytes,
                 pair->edge_session_size) == 0);

    stream.key.producer = EDGE_OWNER;
    memmove(runtime_before, pair->host_runtime_arena.bytes,
            pair->host_runtime_size);
    memmove(session_before, pair->host_session_arena.bytes,
            pair->host_session_size);
    CHECK(meiso_runtime_send_stream_sample(
        pair->host_runtime, wifi_key, 92U, &stream
    ) == MEISO_RUNTIME_OWNER_MISMATCH);
    CHECK(memcmp(runtime_before, pair->host_runtime_arena.bytes,
                 pair->host_runtime_size) == 0 &&
          memcmp(session_before, pair->host_session_arena.bytes,
                 pair->host_session_size) == 0);
    stream.key.producer = HOST_OWNER;
    memmove(runtime_before, pair->host_runtime_arena.bytes,
            pair->host_runtime_size);
    memmove(session_before, pair->host_session_arena.bytes,
            pair->host_session_size);
    CHECK(meiso_runtime_send_stream_sample(
        pair->host_runtime, render_key, 91U, &stream
    ) == MEISO_RUNTIME_DELIVERY_MISMATCH);
    CHECK(memcmp(runtime_before, pair->host_runtime_arena.bytes,
                 pair->host_runtime_size) == 0 &&
          memcmp(session_before, pair->host_session_arena.bytes,
                 pair->host_session_size) == 0);

    CHECK(meiso_runtime_send_stream_sample(
        pair->host_runtime, wifi_key, 92U, &stream
    ) == MEISO_RUNTIME_OK);
    CHECK(pull_message(
        pair, pair->host_session, pair->edge_session, &message
    ));
    CHECK(message.payload.size <= sizeof(hostile));
    memmove(hostile, message.payload.data, message.payload.size);
    patch_route_key(hostile, &render_key);
    message.payload = (MeisoByteView){hostile, message.payload.size};
    memmove(runtime_before, pair->edge_runtime_arena.bytes,
            pair->edge_runtime_size);
    memset(&event, 0x5a, sizeof(event));
    CHECK(meiso_runtime_receive(
        pair->edge_runtime, &message, &event
    ) == MEISO_RUNTIME_DELIVERY_MISMATCH);
    CHECK(memcmp(runtime_before, pair->edge_runtime_arena.bytes,
                 pair->edge_runtime_size) == 0 &&
          bytes_are((const uint8_t *)&event, 0x5a, sizeof(event)));
    return true;
}

static bool test_v2_v3_layout_regression(
    RuntimePair *pair,
    const Catalog *catalog
)
{
    RuntimeArena v2_arena;
    RuntimeArena v3_arena;
    RuntimeArena short_arena;
    MeisoRuntimeConfig v2;
    MeisoRuntimeConfig edge_v2;
    MeisoRuntimeV3Config v3;
    MeisoRuntimeV3Config edge_v3;
    MeisoRuntimeV4Config v4;
    MeisoRuntime *runtime = NULL;
    MeisoRuntime *sentinel = (MeisoRuntime *)(uintptr_t)1U;
    MeisoRuntimeSnapshot snapshot;
    MeisoRuntimeV4RouteDescriptor v4_route;
    MeisoRuntimeV4ExternalMediaBinding binding;
    MeisoRuntimeBlobSnapshot blob_snapshot;
    MeisoRuntimeRouteDescriptor legacy_routes[3];
    uint8_t session_before[65536];
    size_t v2_required;
    size_t v3_required;
    size_t v4_required;
    size_t edge_v2_required;
    size_t edge_v3_required;

    v4 = runtime_config(pair->host_session);
    v2 = v4.base;
    memset(&v3, 0, sizeof(v3));
    v3.base = v2;
    v3.max_blob_transfer_bytes = v4.max_blob_transfer_bytes;
    v2_required = meiso_runtime_memory_required(&v2);
    v3_required = meiso_runtime_v3_memory_required(&v3);
    v4_required = meiso_runtime_v4_memory_required(&v4);
    CHECK(v2_required != 0U && v3_required != 0U && v4_required != 0U &&
          v2_required <= sizeof(v2_arena.bytes) &&
          v3_required <= sizeof(v3_arena.bytes) &&
          v4_required == pair->host_runtime_size &&
          v2_required == v3_required && v4_required > v3_required);

    memset(v2_arena.bytes, 0xa5, sizeof(v2_arena.bytes));
    CHECK(meiso_runtime_init(
        v2_arena.bytes, v2_required, &v2, &runtime
    ) == MEISO_RUNTIME_INIT_OK);
    CHECK(meiso_runtime_snapshot(runtime, &snapshot) &&
          meiso_runtime_blob_snapshot(runtime, &blob_snapshot) &&
          blob_snapshot.wire_version == MEISO_RUNTIME_WIRE_VERSION_V2);
    CHECK(bytes_are(
        v2_arena.bytes + v2_required, 0xa5,
        sizeof(v2_arena.bytes) - v2_required
    ));
    memset(v3_arena.bytes, 0xa5, sizeof(v3_arena.bytes));
    CHECK(meiso_runtime_v3_init(
        v3_arena.bytes, v3_required, &v3, &runtime
    ) == MEISO_RUNTIME_INIT_OK);
    CHECK(meiso_runtime_snapshot(runtime, &snapshot) &&
          meiso_runtime_blob_snapshot(runtime, &blob_snapshot) &&
          blob_snapshot.wire_version == MEISO_RUNTIME_WIRE_VERSION_V3);
    CHECK(bytes_are(
        v3_arena.bytes + v3_required, 0xa5,
        sizeof(v3_arena.bytes) - v3_required
    ));
    memset(&v4_route, 0xa5, sizeof(v4_route));
    CHECK(meiso_runtime_v4_route_at(runtime, 0U, &v4_route) ==
          MEISO_RUNTIME_WIRE_VERSION_MISMATCH);
    CHECK(bytes_are((const uint8_t *)&v4_route, 0xa5, sizeof(v4_route)));
    memset(&binding, 0x5a, sizeof(binding));
    CHECK(meiso_runtime_v4_external_media_binding(
        runtime, (MeisoResourceId){1U, 1U}, &binding
    ) == MEISO_RUNTIME_WIRE_VERSION_MISMATCH);
    CHECK(bytes_are((const uint8_t *)&binding, 0x5a, sizeof(binding)));

    legacy_routes[0] = catalog->routes[WIFI_DESIRED_ROUTE].route;
    legacy_routes[1] = catalog->routes[WIFI_ACTUAL_ROUTE].route;
    legacy_routes[2] = catalog->routes[WIFI_DATA_ROUTE].route;
    legacy_routes[2].resource.owner = MEISO_OWNER_ROLE_HOST;
    edge_v2 = v2;
    edge_v2.session = pair->edge_session;
    edge_v3 = v3;
    edge_v3.base.session = pair->edge_session;
    edge_v2_required = meiso_runtime_memory_required(&edge_v2);
    edge_v3_required = meiso_runtime_v3_memory_required(&edge_v3);
    CHECK(edge_v2_required != 0U && edge_v3_required != 0U &&
          edge_v2_required <= sizeof(v2_arena.bytes) &&
          edge_v3_required <= sizeof(v3_arena.bytes));

    memset(v2_arena.bytes, 0xa5, sizeof(v2_arena.bytes));
    CHECK(meiso_runtime_init(
        v2_arena.bytes, edge_v2_required, &edge_v2, &runtime
    ) == MEISO_RUNTIME_INIT_OK);
    memmove(short_arena.bytes, v2_arena.bytes, edge_v2_required);
    memmove(session_before, pair->edge_session_arena.bytes,
            pair->edge_session_size);
    CHECK(meiso_runtime_offer_catalog(
        runtime, 1U, catalog->endpoints, 1U, legacy_routes, 3U
    ) == MEISO_RUNTIME_OWNER_MISMATCH);
    CHECK(memcmp(short_arena.bytes, v2_arena.bytes, edge_v2_required) == 0 &&
          memcmp(session_before, pair->edge_session_arena.bytes,
                 pair->edge_session_size) == 0);

    memset(v3_arena.bytes, 0xa5, sizeof(v3_arena.bytes));
    CHECK(meiso_runtime_v3_init(
        v3_arena.bytes, edge_v3_required, &edge_v3, &runtime
    ) == MEISO_RUNTIME_INIT_OK);
    memmove(short_arena.bytes, v3_arena.bytes, edge_v3_required);
    memmove(session_before, pair->edge_session_arena.bytes,
            pair->edge_session_size);
    CHECK(meiso_runtime_offer_catalog(
        runtime, 1U, catalog->endpoints, 1U, legacy_routes, 3U
    ) == MEISO_RUNTIME_OWNER_MISMATCH);
    CHECK(memcmp(short_arena.bytes, v3_arena.bytes, edge_v3_required) == 0 &&
          memcmp(session_before, pair->edge_session_arena.bytes,
                 pair->edge_session_size) == 0);

    memset(short_arena.bytes, 0x3c, sizeof(short_arena.bytes));
    runtime = sentinel;
    CHECK(meiso_runtime_v4_init(
        short_arena.bytes, v4_required - 1U, &v4, &runtime
    ) == MEISO_RUNTIME_INIT_ARENA_TOO_SMALL);
    CHECK(runtime == NULL && bytes_are(
        short_arena.bytes, 0x3c, sizeof(short_arena.bytes)
    ));
    CHECK(MEISO_RUNTIME_WIRE_VERSION_V2 == 2U &&
          MEISO_RUNTIME_WIRE_VERSION_V3 == 3U &&
          MEISO_RUNTIME_WIRE_VERSION_V4 == 4U &&
          MEISO_RUNTIME_V3_BLOB_BEGIN_WIRE_SIZE == 120U &&
          MEISO_RUNTIME_V4_ROUTE_RECORD_WIRE_SIZE == 80U);
    return true;
}

static bool rejected_offer_is_atomic(
    RuntimePair *pair,
    const Catalog *catalog,
    const MeisoCompositionProfileRouteDescriptor routes[18],
    MeisoRuntimeStatus expected
)
{
    MeisoRuntimeSnapshot before;
    MeisoRuntimeSnapshot after;
    uint8_t runtime_before[300000];
    uint8_t session_before[65536];

    CHECK(meiso_runtime_snapshot(pair->edge_runtime, &before));
    memmove(runtime_before, pair->edge_runtime_arena.bytes,
            pair->edge_runtime_size);
    memmove(session_before, pair->edge_session_arena.bytes,
            pair->edge_session_size);
    CHECK(meiso_runtime_v4_offer_catalog(
        pair->edge_runtime, 1U, catalog->endpoints,
        MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT, routes,
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ) == expected);
    CHECK(meiso_runtime_snapshot(pair->edge_runtime, &after));
    CHECK(memcmp(&before, &after, sizeof(before)) == 0 &&
          memcmp(runtime_before, pair->edge_runtime_arena.bytes,
                 pair->edge_runtime_size) == 0 &&
          memcmp(session_before, pair->edge_session_arena.bytes,
                 pair->edge_session_size) == 0);
    return true;
}

static bool test_offer_delivery_constraints(
    RuntimePair *pair,
    const Catalog *catalog
)
{
    MeisoCompositionProfileRouteDescriptor routes[18];
    MeisoRuntimeRouteDescriptor base_routes[18];
    MeisoRuntimeSnapshot before;
    MeisoRuntimeSnapshot after;
    uint8_t runtime_before[300000];
    uint8_t session_before[65536];
    size_t index;

    for (index = 0U; index < 18U; ++index) {
        base_routes[index] = catalog->routes[index].route;
    }
    CHECK(meiso_runtime_snapshot(pair->edge_runtime, &before));
    memmove(runtime_before, pair->edge_runtime_arena.bytes,
            pair->edge_runtime_size);
    memmove(session_before, pair->edge_session_arena.bytes,
            pair->edge_session_size);
    CHECK(meiso_runtime_offer_catalog(
        pair->edge_runtime, 1U, catalog->endpoints, 6U, base_routes, 18U
    ) == MEISO_RUNTIME_WIRE_VERSION_MISMATCH);
    CHECK(meiso_runtime_snapshot(pair->edge_runtime, &after));
    CHECK(memcmp(&before, &after, sizeof(before)) == 0 &&
          memcmp(runtime_before, pair->edge_runtime_arena.bytes,
                 pair->edge_runtime_size) == 0 &&
          memcmp(session_before, pair->edge_session_arena.bytes,
                 pair->edge_session_size) == 0);

    memmove(routes, catalog->routes, sizeof(routes));
    routes[WIFI_DESIRED_ROUTE].delivery =
        MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA;
    CHECK(rejected_offer_is_atomic(
        pair, catalog, routes, MEISO_RUNTIME_DELIVERY_MISMATCH
    ));

    memmove(routes, catalog->routes, sizeof(routes));
    routes[WIFI_DATA_ROUTE].delivery =
        MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA;
    routes[WIFI_DATA_ROUTE].route.resource.kind = MEISO_RESOURCE_KIND_BLOB;
    CHECK(rejected_offer_is_atomic(
        pair, catalog, routes, MEISO_RUNTIME_UNSUPPORTED_KIND
    ));

    memmove(routes, catalog->routes, sizeof(routes));
    routes[WIFI_DATA_ROUTE].delivery =
        MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA;
    routes[WIFI_DATA_ROUTE].route.resource.max_payload_size = 0U;
    CHECK(rejected_offer_is_atomic(
        pair, catalog, routes, MEISO_RUNTIME_PAYLOAD_CAPACITY
    ));

    memmove(routes, catalog->routes, sizeof(routes));
    routes[RENDER_DATA_ROUTE].delivery = MEISO_RUNTIME_DELIVERY_INLINE;
    CHECK(rejected_offer_is_atomic(
        pair, catalog, routes, MEISO_RUNTIME_PAYLOAD_CAPACITY
    ));
    return true;
}

static bool test_catalog_wire_and_views(
    RuntimePair *pair,
    const Catalog *catalog,
    uint8_t catalog_wire[MEISO_COMPOSITION_PROFILE_CATALOG_WIRE_SIZE]
)
{
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event;
    MeisoRuntimeSnapshot snapshot;
    MeisoRuntimeRouteDescriptor base_route;
    MeisoRuntimeV4RouteDescriptor route;
    MeisoRuntimeV4ExternalMediaBinding binding;
    uint8_t malformed[MEISO_COMPOSITION_PROFILE_CATALOG_WIRE_SIZE];
    uint8_t runtime_before[300000];
    size_t index;
    MeisoRuntimeStatus offer_status;
    const size_t delivery_offset = CATALOG_ROUTE_OFFSET +
        RENDER_DATA_ROUTE * MEISO_RUNTIME_V4_ROUTE_RECORD_WIRE_SIZE + 72U;

    offer_status = meiso_runtime_v4_offer_catalog(
        pair->edge_runtime, 1U, catalog->endpoints,
        MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT, catalog->routes,
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    );
    CHECK(offer_status == MEISO_RUNTIME_OK);
    CHECK(pull_message(
        pair, pair->edge_session, pair->host_session, &message
    ));
    CHECK(message.payload.size == sizeof(malformed));
    memmove(catalog_wire, message.payload.data, message.payload.size);
    CHECK(catalog_wire[4] == MEISO_RUNTIME_WIRE_VERSION_V4 &&
          catalog_wire[5] == 1U &&
          fnv1a64(catalog_wire, sizeof(malformed)) ==
              UINT64_C(0xf8cdf61c22fd83ed));

    memmove(malformed, catalog_wire, sizeof(malformed));
    malformed[delivery_offset] = 0U;
    memmove(runtime_before, pair->host_runtime_arena.bytes,
            pair->host_runtime_size);
    memset(&event, 0xa5, sizeof(event));
    message.payload = (MeisoByteView){malformed, sizeof(malformed)};
    CHECK(meiso_runtime_receive(pair->host_runtime, &message, &event) ==
          MEISO_RUNTIME_MALFORMED);
    CHECK(memcmp(runtime_before, pair->host_runtime_arena.bytes,
                 pair->host_runtime_size) == 0);
    CHECK(bytes_are((const uint8_t *)&event, 0U, sizeof(event)));

    memmove(malformed, catalog_wire, sizeof(malformed));
    malformed[delivery_offset + 1U] = 1U;
    memmove(runtime_before, pair->host_runtime_arena.bytes,
            pair->host_runtime_size);
    memset(&event, 0, sizeof(event));
    message.payload = (MeisoByteView){malformed, sizeof(malformed)};
    CHECK(meiso_runtime_receive(pair->host_runtime, &message, &event) ==
          MEISO_RUNTIME_MALFORMED);
    CHECK(memcmp(runtime_before, pair->host_runtime_arena.bytes,
                 pair->host_runtime_size) == 0 &&
          bytes_are((const uint8_t *)&event, 0U, sizeof(event)));

    message.payload = (MeisoByteView){catalog_wire, sizeof(malformed)};
    CHECK(meiso_runtime_receive(pair->host_runtime, &message, &event) ==
          MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED &&
          event.catalog_generation == 1U);
    CHECK(meiso_runtime_snapshot(pair->host_runtime, &snapshot) &&
          !snapshot.active_catalog_present &&
          snapshot.staged_catalog_present &&
          snapshot.staged_endpoint_count == 6U &&
          snapshot.staged_route_count == 18U);

    memset(&base_route, 0xa5, sizeof(base_route));
    CHECK(meiso_runtime_staged_route_at(
        pair->host_runtime, 0U, &base_route
    ) == MEISO_RUNTIME_WIRE_VERSION_MISMATCH);
    CHECK(bytes_are((const uint8_t *)&base_route, 0xa5, sizeof(base_route)));
    for (index = 0U; index < 18U; ++index) {
        CHECK(meiso_runtime_v4_staged_route_at(
            pair->host_runtime, index, &route
        ) == MEISO_RUNTIME_OK);
        CHECK(meiso_resource_descriptor_equal(
            &route.route.resource, &catalog->routes[index].route.resource
        ));
        CHECK(route.delivery == (index == RENDER_DATA_ROUTE ||
                                  index == CAMERA_DATA_ROUTE ?
            MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA :
            MEISO_RUNTIME_DELIVERY_INLINE));
    }

    CHECK(meiso_runtime_accept_catalog(pair->host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, pair->host_session, pair->edge_session, pair->edge_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED &&
          event.catalog_generation == 1U);
    for (index = 0U; index < 18U; ++index) {
        CHECK(meiso_runtime_v4_route_at(
            pair->host_runtime, index, &route
        ) == MEISO_RUNTIME_OK);
        CHECK(meiso_runtime_v4_find_route(
            pair->edge_runtime, catalog->routes[index].route.resource.id,
            &route
        ) == MEISO_RUNTIME_OK);
        CHECK(route.delivery == catalog->routes[index].delivery);
    }
    memset(&base_route, 0x5a, sizeof(base_route));
    CHECK(meiso_runtime_route_at(pair->host_runtime, 0U, &base_route) ==
          MEISO_RUNTIME_WIRE_VERSION_MISMATCH);
    CHECK(bytes_are((const uint8_t *)&base_route, 0x5a, sizeof(base_route)));
    memset(&base_route, 0x6b, sizeof(base_route));
    CHECK(meiso_runtime_find_route(
        pair->edge_runtime, catalog->routes[0].route.resource.id, &base_route
    ) == MEISO_RUNTIME_WIRE_VERSION_MISMATCH);
    CHECK(bytes_are((const uint8_t *)&base_route, 0x6b, sizeof(base_route)));

    memset(&binding, 0xa5, sizeof(binding));
    CHECK(meiso_runtime_v4_external_media_binding(
        pair->host_runtime,
        catalog->routes[RENDER_DATA_ROUTE].route.resource.id,
        &binding
    ) == MEISO_RUNTIME_ROUTE_NOT_ACTIVE);
    CHECK(bytes_are((const uint8_t *)&binding, 0xa5, sizeof(binding)));
    return true;
}

static bool test_delivery_gates_and_hostile_inline(
    RuntimePair *pair,
    const Catalog *catalog,
    Replicas *replicas
)
{
    const uint8_t payload[] = {0x41U, 0x42U, 0x43U};
    MeisoRuntimeV4ExternalMediaBinding binding;
    MeisoRuntimeV4ExternalMediaBinding edge_binding;
    MeisoSessionSnapshot host_session_snapshot;
    MeisoRuntimeProductionSnapshot production;
    MeisoRuntimeEvent event;
    MeisoSessionRuntimeMessage message = {0};
    MeisoStreamSample stream = sample(10U, 1U, payload, sizeof(payload));
    MeisoRuntimeRouteKey render_key = route_key(
        catalog, RENDER_DATA_ROUTE, 1U
    );
    MeisoRuntimeRouteKey wifi_key = route_key(catalog, WIFI_DATA_ROUTE, 1U);
    uint8_t runtime_before[300000];
    uint8_t session_before[65536];
    uint8_t event_before[sizeof(event)];
    uint8_t sample_before[sizeof(stream)];
    uint8_t hostile[512];
    uint8_t actual_wire[MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE];
    MeisoByteView actual_payload;
    MeisoStateOffer pending_offer;
    MeisoStateReceiveStatus receive_status;

    CHECK(enable_endpoint(
        pair, catalog, replicas->render_desired, replicas->render_actual,
        RENDER_DESIRED_ROUTE, RENDER_ACTUAL_ROUTE, 1U, 1U, 1U, 77U
    ));
    CHECK(enable_endpoint(
        pair, catalog, replicas->wifi_desired, replicas->wifi_actual,
        WIFI_DESIRED_ROUTE, WIFI_ACTUAL_ROUTE, 1U, 1U, 1U, 77U
    ));
    CHECK(meiso_session_snapshot(pair->host_session, &host_session_snapshot));
    CHECK(meiso_runtime_v4_external_media_binding(
        pair->host_runtime,
        catalog->routes[RENDER_DATA_ROUTE].route.resource.id,
        &binding
    ) == MEISO_RUNTIME_OK);
    CHECK(session_id_equal(binding.session_id, host_session_snapshot.session_id));
    CHECK(route_key_equal(binding.route, render_key));
    CHECK(meiso_resource_descriptor_equal(
        &binding.resource, &catalog->routes[RENDER_DATA_ROUTE].route.resource
    ));
    CHECK(binding.production_epoch == 77U);
    CHECK(meiso_runtime_v4_external_media_binding(
        pair->edge_runtime,
        catalog->routes[RENDER_DATA_ROUTE].route.resource.id,
        &edge_binding
    ) == MEISO_RUNTIME_OK);
    CHECK(session_id_equal(edge_binding.session_id, binding.session_id) &&
          route_key_equal(edge_binding.route, binding.route) &&
          meiso_resource_descriptor_equal(
              &edge_binding.resource, &binding.resource
          ) && edge_binding.production_epoch == binding.production_epoch);
    memset(&edge_binding, 0x5a, sizeof(edge_binding));
    CHECK(meiso_runtime_v4_external_media_binding(
        pair->host_runtime,
        catalog->routes[WIFI_DATA_ROUTE].route.resource.id,
        &edge_binding
    ) == MEISO_RUNTIME_DELIVERY_MISMATCH);
    CHECK(bytes_are(
        (const uint8_t *)&edge_binding, 0x5a, sizeof(edge_binding)
    ));
    CHECK(meiso_runtime_production_snapshot(
        pair->host_runtime,
        catalog->routes[RENDER_DATA_ROUTE].route.endpoint_id, &production
    ) == MEISO_RUNTIME_OK);
    CHECK(production.actual_present && production.data_active &&
          production.production_epoch == 77U);

    memmove(runtime_before, pair->edge_runtime_arena.bytes,
            pair->edge_runtime_size);
    memmove(session_before, pair->edge_session_arena.bytes,
            pair->edge_session_size);
    memcpy(sample_before, &stream, sizeof(stream));
    CHECK(meiso_runtime_send_stream_sample(
        pair->edge_runtime, render_key, 77U, &stream
    ) == MEISO_RUNTIME_DELIVERY_MISMATCH);
    CHECK(memcmp(runtime_before, pair->edge_runtime_arena.bytes,
                 pair->edge_runtime_size) == 0 &&
          memcmp(session_before, pair->edge_session_arena.bytes,
                 pair->edge_session_size) == 0 &&
          memcmp(sample_before, &stream, sizeof(stream)) == 0);

    CHECK(meiso_runtime_send_stream_sample(
        pair->edge_runtime, wifi_key, 77U, &stream
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, pair->edge_session, pair->host_session, pair->host_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          route_key_equal(event.route, wifi_key) &&
          event.production_epoch == 77U &&
          event.stream_sample.payload.size == sizeof(payload) &&
          memcmp(event.stream_sample.payload.data, payload,
                 sizeof(payload)) == 0);

    CHECK(encode_running_actual(1U, 77U, actual_wire, &actual_payload));
    pending_offer = (MeisoStateOffer){
        {2U}, {EDGE_OWNER, 2U, actual_payload}
    };
    CHECK(meiso_runtime_send_state_offer(
        pair->edge_runtime,
        route_key(catalog, WIFI_ACTUAL_ROUTE, 1U), &pending_offer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, pair->edge_session, pair->host_session, pair->host_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    receive_status = meiso_state_replica_receive(
        replicas->wifi_actual, event.state_offer.state.owner,
        event.state_offer.state.version, event.state_offer.state.payload
    );
    CHECK(receive_status == MEISO_STATE_RECEIVE_NEW);

    stream.key.sequence = 2U;
    CHECK(meiso_runtime_send_stream_sample(
        pair->edge_runtime, wifi_key, 77U, &stream
    ) == MEISO_RUNTIME_OK);
    CHECK(pull_message(
        pair, pair->edge_session, pair->host_session, &message
    ));
    CHECK(message.payload.size <= sizeof(hostile));
    memmove(hostile, message.payload.data, message.payload.size);
    patch_route_key(hostile, &render_key);
    message.payload = (MeisoByteView){hostile, message.payload.size};
    memmove(runtime_before, pair->host_runtime_arena.bytes,
            pair->host_runtime_size);
    memmove(session_before, pair->host_session_arena.bytes,
            pair->host_session_size);
    memset(&event, 0xa5, sizeof(event));
    memcpy(event_before, &event, sizeof(event));
    CHECK(meiso_runtime_receive(pair->host_runtime, &message, &event) ==
          MEISO_RUNTIME_DELIVERY_MISMATCH);
    CHECK(memcmp(runtime_before, pair->host_runtime_arena.bytes,
                 pair->host_runtime_size) == 0 &&
          memcmp(session_before, pair->host_session_arena.bytes,
                 pair->host_session_size) == 0 &&
          memcmp(&event, event_before, sizeof(event)) == 0);

    CHECK(meiso_runtime_commit_state_receive(
        pair->host_runtime,
        route_key(catalog, WIFI_ACTUAL_ROUTE, 1U),
        &pending_offer, receive_status
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_state_accept(
        pair->host_runtime,
        route_key(catalog, WIFI_ACTUAL_ROUTE, 1U),
        pending_offer.generation, pending_offer.state.owner,
        pending_offer.state.version
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, pair->host_session, pair->edge_session, pair->edge_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT &&
          event.state_accept_version == 2U);
    return true;
}

static MeisoContentId blob_content_id(void)
{
    MeisoContentId value;

    memset(&value, 0, sizeof(value));
    value.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    value.digest_size = MEISO_SHA256_DIGEST_SIZE;
    memmove(value.digest, BLOB_SHA256, sizeof(BLOB_SHA256));
    return value;
}

static bool test_v4_blob_carriage(
    RuntimePair *pair,
    const Catalog *catalog
)
{
    BlobArena blob_arena;
    const MeisoBlobConfig blob_config = {2U};
    const MeisoRuntimeRouteKey key = route_key(
        catalog, SCENE_BLOB_ROUTE, 1U
    );
    const MeisoContentId content_id = blob_content_id();
    MeisoRuntimeBlobTransferRef transfer = {0};
    MeisoRuntimeBlobTransferRef committed_transfer = {0};
    MeisoRuntimeBlobPublishReport report = {0};
    MeisoRuntimeBlobSnapshot host_snapshot;
    MeisoRuntimeBlobSnapshot edge_snapshot;
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};
    MeisoBlobStore *store = NULL;
    MeisoBlobView view = {0};
    size_t required;

    CHECK(catalog->routes[SCENE_BLOB_ROUTE].delivery ==
          MEISO_RUNTIME_DELIVERY_INLINE &&
          catalog->routes[SCENE_BLOB_ROUTE].route.resource.kind ==
              MEISO_RESOURCE_KIND_BLOB &&
          catalog->routes[SCENE_BLOB_ROUTE].route.facet ==
              MEISO_RUNTIME_FACET_IMMUTABLE);
    CHECK(meiso_runtime_send_blob_begin(
        pair->host_runtime, key, &content_id, sizeof(BLOB_BYTES), &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer.transfer_id == 1U && transfer.total_size ==
          sizeof(BLOB_BYTES));
    CHECK(pull_message(
        pair, pair->host_session, pair->edge_session, &message
    ));
    CHECK(message.payload.size == MEISO_RUNTIME_V3_BLOB_BEGIN_WIRE_SIZE &&
          message.payload.data[4] == MEISO_RUNTIME_WIRE_VERSION_V4 &&
          message.payload.data[5] == 7U);
    CHECK(meiso_runtime_receive(
        pair->edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN &&
          event.blob_transfer.transfer_id == transfer.transfer_id);
    CHECK(meiso_runtime_send_blob_result(
        pair->edge_runtime, &event.blob_transfer, MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, pair->edge_session, pair->host_session, pair->host_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_RESULT &&
          event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN &&
          event.blob_result == MEISO_RUNTIME_BLOB_RESULT_READY &&
          event.blob_next_offset == 0U);

    CHECK(meiso_runtime_send_blob_chunk(
        pair->host_runtime, &transfer, 0U,
        (MeisoByteView){BLOB_BYTES, sizeof(BLOB_BYTES)}
    ) == MEISO_RUNTIME_OK);
    CHECK(pull_message(
        pair, pair->host_session, pair->edge_session, &message
    ));
    CHECK(message.payload.size ==
              MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE +
                  sizeof(BLOB_BYTES) &&
          message.payload.data[4] == MEISO_RUNTIME_WIRE_VERSION_V4 &&
          message.payload.data[5] == 8U);
    CHECK(meiso_runtime_receive(
        pair->edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          event.blob_operation == MEISO_RUNTIME_BLOB_CHUNK &&
          event.blob_offset == 0U &&
          event.blob_payload.size == sizeof(BLOB_BYTES) &&
          memcmp(event.blob_payload.data, BLOB_BYTES, sizeof(BLOB_BYTES)) == 0);

    CHECK(meiso_runtime_send_blob_commit(
        pair->host_runtime, &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(pull_message(
        pair, pair->host_session, pair->edge_session, &message
    ));
    CHECK(message.payload.size == MEISO_RUNTIME_V3_BLOB_COMMIT_WIRE_SIZE &&
          message.payload.data[4] == MEISO_RUNTIME_WIRE_VERSION_V4 &&
          message.payload.data[5] == 9U);
    CHECK(meiso_runtime_receive(
        pair->edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          event.blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          event.blob_payload.size == sizeof(BLOB_BYTES) &&
          memcmp(event.blob_payload.data, BLOB_BYTES, sizeof(BLOB_BYTES)) == 0);
    committed_transfer = event.blob_transfer;

    required = meiso_blob_store_memory_required(
        &catalog->routes[SCENE_BLOB_ROUTE].route.resource, &blob_config
    );
    CHECK(required != 0U && required <= sizeof(blob_arena.bytes));
    CHECK(meiso_blob_store_init(
        blob_arena.bytes, required,
        &catalog->routes[SCENE_BLOB_ROUTE].route.resource,
        &blob_config, &store
    ) == MEISO_BLOB_INIT_OK);
    CHECK(meiso_runtime_publish_blob_receive(
        pair->edge_runtime, &committed_transfer, store,
        meiso_runtime_blob_sha256_verify, NULL, &report
    ) == MEISO_RUNTIME_OK);
    CHECK(report.stage_attempted && report.commit_attempted &&
          report.stage_status == MEISO_BLOB_STAGE_STARTED &&
          report.commit_status == MEISO_BLOB_COMMIT_PUBLISHED &&
          report.result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
          meiso_blob_store_count(store) == 1U);
    CHECK(meiso_blob_store_lookup(store, &content_id, &view) &&
          view.content.size == sizeof(BLOB_BYTES) &&
          memcmp(view.content.data, BLOB_BYTES, sizeof(BLOB_BYTES)) == 0);
    CHECK(meiso_runtime_send_blob_result(
        pair->edge_runtime, &committed_transfer, MEISO_RUNTIME_BLOB_COMMIT
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, pair->edge_session, pair->host_session, pair->host_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_RESULT &&
          event.blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          event.blob_result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
          event.blob_next_offset == sizeof(BLOB_BYTES));
    CHECK(meiso_runtime_blob_snapshot(pair->host_runtime, &host_snapshot) &&
          meiso_runtime_blob_snapshot(pair->edge_runtime, &edge_snapshot));
    CHECK(host_snapshot.wire_version == MEISO_RUNTIME_WIRE_VERSION_V4 &&
          edge_snapshot.wire_version == MEISO_RUNTIME_WIRE_VERSION_V4 &&
          !host_snapshot.transfer_active && !edge_snapshot.transfer_active &&
          !host_snapshot.publish_pending && !edge_snapshot.publish_pending &&
          host_snapshot.transfer_id_high_water == 1U &&
          edge_snapshot.transfer_id_high_water == 1U);
    return true;
}

static bool test_replacement_and_terminal(
    RuntimePair *pair,
    const Catalog *catalog,
    Replicas *replicas
)
{
    MeisoRuntimeEvent event = {0};
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeSnapshot host_before;
    MeisoRuntimeSnapshot edge_before;
    MeisoRuntimeSnapshot snapshot;
    MeisoRuntimeV4RouteDescriptor staged;
    MeisoRuntimeV4ExternalMediaBinding old_binding;
    MeisoRuntimeV4ExternalMediaBinding output;
    MeisoRuntimeRouteKey old_route = route_key(catalog, RENDER_DATA_ROUTE, 1U);
    MeisoRuntimeRouteKey new_route = route_key(catalog, RENDER_DATA_ROUTE, 2U);
    MeisoStreamSample stream = sample(10U, 9U, NULL, 0U);
    MeisoByteView close_frame;
    MeisoSessionRuntimeMessage ignored = {0};

    CHECK(meiso_runtime_snapshot(pair->host_runtime, &host_before));
    CHECK(meiso_runtime_snapshot(pair->edge_runtime, &edge_before));
    CHECK(meiso_runtime_v4_external_media_binding(
        pair->host_runtime,
        catalog->routes[RENDER_DATA_ROUTE].route.resource.id,
        &old_binding
    ) == MEISO_RUNTIME_OK);
    CHECK(old_binding.route.catalog_generation == 1U &&
          old_binding.production_epoch == 77U);

    CHECK(meiso_runtime_v4_offer_catalog(
        pair->edge_runtime, 2U, catalog->endpoints,
        MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT, catalog->routes,
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ) == MEISO_RUNTIME_OK);
    CHECK(pull_message(
        pair, pair->edge_session, pair->host_session, &message
    ));
    CHECK(meiso_runtime_receive(pair->host_runtime, &message, &event) ==
          MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED &&
          event.catalog_generation == 2U);
    CHECK(meiso_runtime_v4_staged_route_at(
        pair->host_runtime, RENDER_DATA_ROUTE, &staged
    ) == MEISO_RUNTIME_OK);
    CHECK(staged.delivery == MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA);
    CHECK(meiso_runtime_v4_external_media_binding(
        pair->host_runtime,
        catalog->routes[RENDER_DATA_ROUTE].route.resource.id,
        &output
    ) == MEISO_RUNTIME_OK);
    CHECK(memcmp(&output, &old_binding, sizeof(output)) == 0);
    CHECK(meiso_runtime_snapshot(pair->edge_runtime, &snapshot) &&
          snapshot.active_catalog_generation ==
              edge_before.active_catalog_generation &&
          snapshot.active_route_count == edge_before.active_route_count &&
          snapshot.staged_catalog_present &&
          snapshot.staged_catalog_generation == 2U);

    CHECK(meiso_runtime_accept_catalog(pair->host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair, pair->host_session, pair->edge_session, pair->edge_runtime,
        &event, MEISO_RUNTIME_OK
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED &&
          event.catalog_generation == 2U);
    CHECK(meiso_runtime_snapshot(pair->host_runtime, &snapshot) &&
          snapshot.active_catalog_generation == 2U &&
          !snapshot.staged_catalog_present &&
          snapshot.active_endpoint_count == host_before.active_endpoint_count &&
          snapshot.active_route_count == host_before.active_route_count);
    memset(&output, 0xa5, sizeof(output));
    CHECK(meiso_runtime_v4_external_media_binding(
        pair->host_runtime,
        catalog->routes[RENDER_DATA_ROUTE].route.resource.id,
        &output
    ) == MEISO_RUNTIME_ROUTE_NOT_ACTIVE);
    CHECK(bytes_are((const uint8_t *)&output, 0xa5, sizeof(output)));
    CHECK(meiso_runtime_send_stream_sample(
        pair->edge_runtime, old_route, 77U, &stream
    ) == MEISO_RUNTIME_STALE_CATALOG);
    CHECK(meiso_runtime_send_stream_sample(
        pair->edge_runtime, new_route, 77U, &stream
    ) == MEISO_RUNTIME_DELIVERY_MISMATCH);

    CHECK(enable_endpoint(
        pair, catalog, replicas->render_desired, replicas->render_actual,
        RENDER_DESIRED_ROUTE, RENDER_ACTUAL_ROUTE, 2U, 2U, 2U, 78U
    ));
    CHECK(enable_endpoint(
        pair, catalog, replicas->wifi_desired, replicas->wifi_actual,
        WIFI_DESIRED_ROUTE, WIFI_ACTUAL_ROUTE, 2U, 2U, 3U, 78U
    ));
    CHECK(meiso_runtime_v4_external_media_binding(
        pair->host_runtime,
        catalog->routes[RENDER_DATA_ROUTE].route.resource.id,
        &output
    ) == MEISO_RUNTIME_OK);
    CHECK(output.route.catalog_generation == 2U &&
          output.production_epoch == 78U &&
          !session_id_equal((MeisoSessionId){0}, output.session_id));

    CHECK(meiso_session_close(
        pair->host_session, MEISO_SESSION_CLOSE_NORMAL
    ) == MEISO_SESSION_CLOSE_QUEUED);
    CHECK(meiso_session_next_outbound(pair->host_session, &close_frame));
    CHECK(close_frame.size <= sizeof(pair->transfer));
    memmove(pair->transfer, close_frame.data, close_frame.size);
    CHECK(meiso_session_mark_outbound_sent(pair->host_session) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        pair->edge_session,
        (MeisoByteView){pair->transfer, close_frame.size}, &ignored
    ) == MEISO_SESSION_RECEIVE_PEER_CLOSED);
    CHECK(meiso_runtime_refresh_bound_session(pair->host_runtime) ==
          MEISO_RUNTIME_CORE_NOT_READY);
    CHECK(meiso_runtime_refresh_bound_session(pair->edge_runtime) ==
          MEISO_RUNTIME_CORE_NOT_READY);
    CHECK(meiso_runtime_snapshot(pair->host_runtime, &snapshot) &&
          !snapshot.core_ready && !snapshot.active_catalog_present &&
          !snapshot.staged_catalog_present &&
          snapshot.active_endpoint_count == 0U &&
          snapshot.active_route_count == 0U);
    memset(&output, 0x5a, sizeof(output));
    CHECK(meiso_runtime_v4_external_media_binding(
        pair->host_runtime,
        catalog->routes[RENDER_DATA_ROUTE].route.resource.id,
        &output
    ) == MEISO_RUNTIME_STALE_SESSION);
    CHECK(bytes_are((const uint8_t *)&output, 0x5a, sizeof(output)));
    CHECK(meiso_runtime_send_stream_sample(
        pair->edge_runtime, new_route, 78U, &stream
    ) == MEISO_RUNTIME_STALE_SESSION);
    return true;
}

int main(void)
{
    RuntimePair pair;
    Catalog catalog;
    Replicas replicas;
    uint8_t catalog_wire[MEISO_COMPOSITION_PROFILE_CATALOG_WIRE_SIZE];

    if (!init_pair(&pair, 90U) || !build_catalog(&catalog) ||
        !test_host_owned_data_direction(&pair, &catalog, &replicas) ||
        !init_pair(&pair, 100U) || !build_catalog(&catalog) ||
        !test_v2_v3_layout_regression(&pair, &catalog) ||
        !test_offer_delivery_constraints(&pair, &catalog) ||
        !test_catalog_wire_and_views(&pair, &catalog, catalog_wire) ||
        !init_replicas(&replicas, &catalog) ||
        !test_delivery_gates_and_hostile_inline(
            &pair, &catalog, &replicas
        ) || !test_v4_blob_carriage(&pair, &catalog) ||
        !test_replacement_and_terminal(&pair, &catalog, &replicas)) {
        return 1;
    }
    puts("runtime v4 tests passed");
    return 0;
}
