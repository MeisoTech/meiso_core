#include "meiso_runtime.h"

#include <stdbool.h>
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

static const MeisoEndpointId ENDPOINT = {
    UINT64_C(0x0102030405060708), UINT64_C(0x1112131415161718)
};
static const uint64_t ENDPOINT_INCARNATION =
    UINT64_C(0x2122232425262728);
static const MeisoSchemaRef CAPABILITY_SCHEMA = {{
    UINT64_C(0x3132333435363738), UINT64_C(0x4142434445464748)
}, UINT32_C(0x51525354)};
static const MeisoResourceId DESIRED_ID = {
    UINT64_C(0x6162636465666768), UINT64_C(0x7172737475767778)
};
static const MeisoResourceId ACTUAL_ID = {
    UINT64_C(0x1111111111111111), UINT64_C(0x2222222222222222)
};
static const MeisoResourceId DATA_ID = {
    UINT64_C(0x6263646566676869), UINT64_C(0x7273747576777879)
};
static const MeisoSchemaRef DESIRED_SCHEMA = {{
    UINT64_C(0x8182838485868788), UINT64_C(0x9192939495969798)
}, UINT32_C(0xa1a2a3a4)};
static const MeisoSchemaRef ACTUAL_SCHEMA = {{0x31U, 0x32U}, 1U};
static const MeisoSchemaRef DATA_SCHEMA = {{0x41U, 0x42U}, 1U};

/* Independent literal Runtime wire v2 contract. */
static const uint8_t GOLDEN_CATALOG[] = {
    0x4dU, 0x52U, 0x54U, 0x30U, 0x02U, 0x01U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x97U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x01U, 0x02U, 0x03U, 0x04U,
    0x05U, 0x06U, 0x07U, 0x08U, 0x11U, 0x12U, 0x13U, 0x14U,
    0x15U, 0x16U, 0x17U, 0x18U, 0x21U, 0x22U, 0x23U, 0x24U,
    0x25U, 0x26U, 0x27U, 0x28U, 0x31U, 0x32U, 0x33U, 0x34U,
    0x35U, 0x36U, 0x37U, 0x38U, 0x41U, 0x42U, 0x43U, 0x44U,
    0x45U, 0x46U, 0x47U, 0x48U, 0x51U, 0x52U, 0x53U, 0x54U,
    0x00U, 0x00U, 0x00U, 0x03U, 0xa1U, 0xb2U, 0xc3U, 0x01U,
    0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x11U,
    0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U, 0x21U,
    0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U, 0x61U,
    0x62U, 0x63U, 0x64U, 0x65U, 0x66U, 0x67U, 0x68U, 0x71U,
    0x72U, 0x73U, 0x74U, 0x75U, 0x76U, 0x77U, 0x78U, 0x01U,
    0x01U, 0x02U, 0x00U, 0x81U, 0x82U, 0x83U, 0x84U, 0x85U,
    0x86U, 0x87U, 0x88U, 0x91U, 0x92U, 0x93U, 0x94U, 0x95U,
    0x96U, 0x97U, 0x98U, 0xa1U, 0xa2U, 0xa3U, 0xa4U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x40U,
};

static const uint8_t GOLDEN_ACCEPT[] = {
    0x4dU, 0x52U, 0x54U, 0x30U, 0x02U, 0x02U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x14U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x01U,
};

static const uint8_t GOLDEN_REJECT[] = {
    0x4dU, 0x52U, 0x54U, 0x30U, 0x02U, 0x03U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x18U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x02U, 0x02U, 0x00U, 0x00U, 0x00U,
};

static const uint8_t GOLDEN_STATE_OFFER[] = {
    0x4dU, 0x52U, 0x54U, 0x30U, 0x02U, 0x04U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x76U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x01U, 0x02U, 0x03U, 0x04U,
    0x05U, 0x06U, 0x07U, 0x08U, 0x11U, 0x12U, 0x13U, 0x14U,
    0x15U, 0x16U, 0x17U, 0x18U, 0x21U, 0x22U, 0x23U, 0x24U,
    0x25U, 0x26U, 0x27U, 0x28U, 0x61U, 0x62U, 0x63U, 0x64U,
    0x65U, 0x66U, 0x67U, 0x68U, 0x71U, 0x72U, 0x73U, 0x74U,
    0x75U, 0x76U, 0x77U, 0x78U, 0x02U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0xd1U, 0xd2U, 0xd3U, 0xd4U,
    0xd5U, 0xd6U, 0xd7U, 0xd8U, 0x01U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0xe1U, 0xe2U, 0xe3U, 0xe4U,
    0xe5U, 0xe6U, 0xe7U, 0xe8U, 0xf1U, 0xf2U, 0xf3U, 0xf4U,
    0xf5U, 0xf6U, 0xf7U, 0xf8U, 0x00U, 0x00U, 0x00U, 0x0aU,
    0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x21U, 0x22U,
};

static const uint8_t GOLDEN_STATE_ACCEPT[] = {
    0x4dU, 0x52U, 0x54U, 0x30U, 0x02U, 0x05U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x64U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x01U, 0x02U, 0x03U, 0x04U,
    0x05U, 0x06U, 0x07U, 0x08U, 0x11U, 0x12U, 0x13U, 0x14U,
    0x15U, 0x16U, 0x17U, 0x18U, 0x21U, 0x22U, 0x23U, 0x24U,
    0x25U, 0x26U, 0x27U, 0x28U, 0x61U, 0x62U, 0x63U, 0x64U,
    0x65U, 0x66U, 0x67U, 0x68U, 0x71U, 0x72U, 0x73U, 0x74U,
    0x75U, 0x76U, 0x77U, 0x78U, 0x02U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0xd1U, 0xd2U, 0xd3U, 0xd4U,
    0xd5U, 0xd6U, 0xd7U, 0xd8U, 0x01U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0xe1U, 0xe2U, 0xe3U, 0xe4U,
    0xe5U, 0xe6U, 0xe7U, 0xe8U, 0xf1U, 0xf2U, 0xf3U, 0xf4U,
    0xf5U, 0xf6U, 0xf7U, 0xf8U,
};

static const uint8_t GOLDEN_STREAM_SAMPLE[] = {
    0x4dU, 0x52U, 0x54U, 0x30U, 0x02U, 0x06U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0xa1U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x03U, 0x01U, 0x02U, 0x03U, 0x04U,
    0x05U, 0x06U, 0x07U, 0x08U, 0x11U, 0x12U, 0x13U, 0x14U,
    0x15U, 0x16U, 0x17U, 0x18U, 0x21U, 0x22U, 0x23U, 0x24U,
    0x25U, 0x26U, 0x27U, 0x28U, 0x62U, 0x63U, 0x64U, 0x65U,
    0x66U, 0x67U, 0x68U, 0x69U, 0x72U, 0x73U, 0x74U, 0x75U,
    0x76U, 0x77U, 0x78U, 0x79U, 0x04U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x01U, 0x01U, 0x01U,
    0x01U, 0x01U, 0x01U, 0x01U, 0x02U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x41U, 0x42U, 0x43U, 0x44U,
    0x45U, 0x46U, 0x47U, 0x48U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x05U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x06U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x08U, 0x01U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x09U, 0x00U, 0x00U, 0x00U, 0x02U,
    0x00U, 0x00U, 0x00U, 0x03U, 0xd1U, 0xd2U, 0x41U, 0x42U,
    0x43U,
};

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

static bool init_pair(RuntimePair *pair)
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
    CHECK(handshake(pair->host_session, pair->edge_session));
    CHECK(handshake(pair->edge_session, pair->host_session));
    CHECK(handshake(pair->host_session, pair->edge_session));
    CHECK(handshake(pair->edge_session, pair->host_session));
    CHECK(init_runtime(
        &pair->host_runtime_arena, pair->host_session, &pair->host_runtime
    ));
    CHECK(init_runtime(
        &pair->edge_runtime_arena, pair->edge_session, &pair->edge_runtime
    ));
    return true;
}

static bool receive_outbound(
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
        receiver,
        (MeisoByteView){pair->transfer, outbound.size},
        out_message
    ) == MEISO_SESSION_RECEIVE_RUNTIME);
    return true;
}

static bool compare_and_receive(
    RuntimePair *pair,
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoRuntime *runtime,
    const uint8_t *golden,
    size_t golden_size,
    MeisoRuntimeEvent *event
)
{
    MeisoSessionRuntimeMessage message = {0};

    CHECK(receive_outbound(pair, sender, receiver, &message));
    CHECK(message.payload.size == golden_size);
    CHECK(memcmp(message.payload.data, golden, golden_size) == 0);
    CHECK(meiso_runtime_receive(runtime, &message, event) == MEISO_RUNTIME_OK);
    return true;
}

static void single_catalog(
    MeisoRuntimeEndpointDescriptor *endpoint,
    MeisoRuntimeRouteDescriptor *route
)
{
    static const uint8_t capability[] = {0xa1U, 0xb2U, 0xc3U};

    *endpoint = (MeisoRuntimeEndpointDescriptor){
        ENDPOINT, ENDPOINT_INCARNATION, CAPABILITY_SCHEMA,
        {capability, sizeof(capability)}
    };
    *route = (MeisoRuntimeRouteDescriptor){
        ENDPOINT, ENDPOINT_INCARNATION,
        {DESIRED_ID, MEISO_RESOURCE_KIND_STATE, DESIRED_SCHEMA,
         MEISO_OWNER_ROLE_HOST, 64U},
        MEISO_RUNTIME_FACET_DESIRED
    };
}

static void full_catalog(
    MeisoRuntimeEndpointDescriptor *endpoint,
    MeisoRuntimeRouteDescriptor routes[3]
)
{
    single_catalog(endpoint, &routes[0]);
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

static MeisoRuntimeRouteKey route_key(
    uint64_t generation,
    MeisoResourceId resource,
    MeisoRuntimeFacet facet
)
{
    const MeisoRuntimeRouteKey key = {
        generation, ENDPOINT, ENDPOINT_INCARNATION, resource, facet
    };
    return key;
}

static bool exercise_golden_encoding(RuntimePair *pair)
{
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor route;
    MeisoRuntimeRouteDescriptor routes[3];
    MeisoRuntimeEvent event;
    uint8_t desired_bytes[16];
    uint8_t actual_bytes[128];
    MeisoByteView payload;
    MeisoRuntimeDesiredState desired = {
        true, {(const uint8_t *)"\x21\x22", 2U}
    };
    MeisoStateOffer offer;
    MeisoRuntimeActualState actual;
    MeisoStreamSample sample;
    const uint8_t dependency[] = {0xd1U, 0xd2U};
    const uint8_t sample_payload[] = {0x41U, 0x42U, 0x43U};

    single_catalog(&endpoint, &route);
    CHECK(meiso_runtime_offer_catalog(
        pair->edge_runtime, 1U, &endpoint, 1U, &route, 1U
    ) == MEISO_RUNTIME_OK);
    CHECK(compare_and_receive(
        pair, pair->edge_session, pair->host_session, pair->host_runtime,
        GOLDEN_CATALOG, sizeof(GOLDEN_CATALOG), &event
    ));
    CHECK(meiso_runtime_accept_catalog(pair->host_runtime) == MEISO_RUNTIME_OK);
    CHECK(compare_and_receive(
        pair, pair->host_session, pair->edge_session, pair->edge_runtime,
        GOLDEN_ACCEPT, sizeof(GOLDEN_ACCEPT), &event
    ));

    memset(&actual, 0, sizeof(actual));
    CHECK(meiso_runtime_actual_state_encoded_size(&actual) == 0U);
    CHECK(meiso_runtime_desired_state_encode(
        &desired,
        (MeisoMutableBuffer){desired_bytes, sizeof(desired_bytes)},
        &payload
    ));
    offer.generation.value = UINT64_C(0xd1d2d3d4d5d6d7d8);
    offer.state.owner = (MeisoOwnerScope){
        MEISO_OWNER_ROLE_HOST, UINT64_C(0xe1e2e3e4e5e6e7e8)
    };
    offer.state.version = UINT64_C(0xf1f2f3f4f5f6f7f8);
    offer.state.payload = payload;
    CHECK(meiso_runtime_send_state_offer(
        pair->host_runtime,
        route_key(1U, DESIRED_ID, MEISO_RUNTIME_FACET_DESIRED),
        &offer
    ) == MEISO_RUNTIME_OK);
    CHECK(compare_and_receive(
        pair, pair->host_session, pair->edge_session, pair->edge_runtime,
        GOLDEN_STATE_OFFER, sizeof(GOLDEN_STATE_OFFER), &event
    ));
    CHECK(meiso_runtime_send_state_accept(
        pair->edge_runtime,
        event.route,
        event.state_offer.generation,
        event.state_offer.state.owner,
        event.state_offer.state.version
    ) == MEISO_RUNTIME_OK);
    CHECK(compare_and_receive(
        pair, pair->edge_session, pair->host_session, pair->host_runtime,
        GOLDEN_STATE_ACCEPT, sizeof(GOLDEN_STATE_ACCEPT), &event
    ));

    single_catalog(&endpoint, &route);
    CHECK(meiso_runtime_offer_catalog(
        pair->edge_runtime, 2U, &endpoint, 1U, &route, 1U
    ) == MEISO_RUNTIME_OK);
    {
        MeisoSessionRuntimeMessage message = {0};
        CHECK(receive_outbound(
            pair, pair->edge_session, pair->host_session, &message
        ));
        CHECK(meiso_runtime_receive(
            pair->host_runtime, &message, &event
        ) == MEISO_RUNTIME_OK);
    }
    CHECK(meiso_runtime_reject_catalog(
        pair->host_runtime, MEISO_RUNTIME_CATALOG_REJECT_POLICY
    ) == MEISO_RUNTIME_OK);
    CHECK(compare_and_receive(
        pair, pair->host_session, pair->edge_session, pair->edge_runtime,
        GOLDEN_REJECT, sizeof(GOLDEN_REJECT), &event
    ));

    full_catalog(&endpoint, routes);
    CHECK(meiso_runtime_offer_catalog(
        pair->edge_runtime, 3U, &endpoint, 1U, routes, 3U
    ) == MEISO_RUNTIME_OK);
    {
        MeisoSessionRuntimeMessage message = {0};
        CHECK(receive_outbound(
            pair, pair->edge_session, pair->host_session, &message
        ));
        CHECK(meiso_runtime_receive(
            pair->host_runtime, &message, &event
        ) == MEISO_RUNTIME_OK);
    }
    CHECK(meiso_runtime_accept_catalog(pair->host_runtime) == MEISO_RUNTIME_OK);
    {
        MeisoSessionRuntimeMessage message = {0};
        CHECK(receive_outbound(
            pair, pair->host_session, pair->edge_session, &message
        ));
        CHECK(meiso_runtime_receive(
            pair->edge_runtime, &message, &event
        ) == MEISO_RUNTIME_OK);
    }

    offer.generation.value = 8U;
    offer.state.owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 31U};
    offer.state.version = 1U;
    offer.state.payload = (MeisoByteView){desired_bytes, 10U};
    CHECK(meiso_runtime_send_state_offer(
        pair->host_runtime,
        route_key(3U, DESIRED_ID, MEISO_RUNTIME_FACET_DESIRED),
        &offer
    ) == MEISO_RUNTIME_OK);
    {
        MeisoSessionRuntimeMessage message = {0};
        CHECK(receive_outbound(
            pair, pair->host_session, pair->edge_session, &message
        ));
        CHECK(meiso_runtime_receive(
            pair->edge_runtime, &message, &event
        ) == MEISO_RUNTIME_OK);
        CHECK(meiso_runtime_commit_state_receive(
            pair->edge_runtime, event.route, &event.state_offer,
            MEISO_STATE_RECEIVE_NEW
        ) == MEISO_RUNTIME_OK);
    }

    memset(&actual, 0, sizeof(actual));
    actual.observed_desired = (MeisoRuntimeDesiredReference){
        true, {MEISO_OWNER_ROLE_HOST, 31U}, 1U
    };
    actual.applied_desired = actual.observed_desired;
    actual.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    actual.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    actual.effective_enabled = true;
    actual.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING;
    actual.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    actual.production_epoch = UINT64_C(0x0101010101010101);
    CHECK(meiso_runtime_actual_state_encode(
        &actual,
        (MeisoMutableBuffer){actual_bytes, sizeof(actual_bytes)},
        &payload
    ));
    offer.generation.value = 9U;
    offer.state.owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 41U};
    offer.state.version = 1U;
    offer.state.payload = payload;
    CHECK(meiso_runtime_send_state_offer(
        pair->edge_runtime,
        route_key(3U, ACTUAL_ID, MEISO_RUNTIME_FACET_ACTUAL),
        &offer
    ) == MEISO_RUNTIME_OK);
    {
        MeisoSessionRuntimeMessage message = {0};
        CHECK(receive_outbound(
            pair, pair->edge_session, pair->host_session, &message
        ));
        CHECK(meiso_runtime_receive(
            pair->host_runtime, &message, &event
        ) == MEISO_RUNTIME_OK);
        CHECK(meiso_runtime_commit_state_receive(
            pair->host_runtime, event.route, &event.state_offer,
            MEISO_STATE_RECEIVE_NEW
        ) == MEISO_RUNTIME_OK);
    }
    memset(&sample, 0, sizeof(sample));
    sample.key.producer = (MeisoOwnerScope){
        MEISO_OWNER_ROLE_EDGE, UINT64_C(0x4142434445464748)
    };
    sample.key.epoch = 5U;
    sample.key.sequence = 6U;
    sample.source_time = (MeisoTimePoint){7U, 8U};
    sample.sender_deadline.present = true;
    sample.sender_deadline.value = (MeisoTimePoint){7U, 9U};
    sample.dependency = (MeisoByteView){dependency, sizeof(dependency)};
    sample.payload = (MeisoByteView){sample_payload, sizeof(sample_payload)};
    CHECK(meiso_runtime_send_stream_sample(
        pair->edge_runtime,
        route_key(3U, DATA_ID, MEISO_RUNTIME_FACET_DATA),
        UINT64_C(0x0101010101010101),
        &sample
    ) == MEISO_RUNTIME_OK);
    CHECK(compare_and_receive(
        pair, pair->edge_session, pair->host_session, pair->host_runtime,
        GOLDEN_STREAM_SAMPLE, sizeof(GOLDEN_STREAM_SAMPLE), &event
    ));
    return true;
}

static bool snapshot_equal(
    const MeisoRuntimeSnapshot *lhs,
    const MeisoRuntimeSnapshot *rhs
)
{
    return memcmp(lhs, rhs, sizeof(*lhs)) == 0;
}

static bool exercise_malformed_preserves_state(RuntimePair *pair)
{
    const struct {
        const uint8_t *bytes;
        size_t size;
    } vectors[] = {
        {GOLDEN_CATALOG, sizeof(GOLDEN_CATALOG)},
        {GOLDEN_ACCEPT, sizeof(GOLDEN_ACCEPT)},
        {GOLDEN_REJECT, sizeof(GOLDEN_REJECT)},
        {GOLDEN_STATE_OFFER, sizeof(GOLDEN_STATE_OFFER)},
        {GOLDEN_STATE_ACCEPT, sizeof(GOLDEN_STATE_ACCEPT)},
        {GOLDEN_STREAM_SAMPLE, sizeof(GOLDEN_STREAM_SAMPLE)}
    };
    MeisoSessionSnapshot session;
    MeisoRuntimeSnapshot before;
    MeisoRuntimeSnapshot after;
    MeisoSessionRuntimeMessage message;
    MeisoRuntimeEvent event;
    uint8_t mutated[256];
    size_t vector_index;
    size_t cut;

    CHECK(meiso_session_snapshot(pair->host_session, &session));
    CHECK(meiso_runtime_snapshot(pair->host_runtime, &before));
    message.session_id = session.session_id;
    for (vector_index = 0U;
         vector_index < sizeof(vectors) / sizeof(vectors[0]);
         ++vector_index) {
        for (cut = 0U; cut < vectors[vector_index].size; ++cut) {
            message.payload = (MeisoByteView){vectors[vector_index].bytes, cut};
            CHECK(meiso_runtime_receive(
                pair->host_runtime, &message, &event
            ) == MEISO_RUNTIME_MALFORMED);
            CHECK(meiso_runtime_snapshot(pair->host_runtime, &after));
            CHECK(snapshot_equal(&before, &after));
        }
        CHECK(vectors[vector_index].size <= sizeof(mutated));
        memmove(mutated, vectors[vector_index].bytes, vectors[vector_index].size);
        mutated[6] = 1U;
        message.payload = (MeisoByteView){
            mutated, vectors[vector_index].size
        };
        CHECK(meiso_runtime_receive(
            pair->host_runtime, &message, &event
        ) == MEISO_RUNTIME_MALFORMED);
        CHECK(meiso_runtime_snapshot(pair->host_runtime, &after));
        CHECK(snapshot_equal(&before, &after));
    }

    memmove(mutated, GOLDEN_STATE_OFFER, sizeof(GOLDEN_STATE_OFFER));
    mutated[12U + 49U] = 1U;
    message.payload = (MeisoByteView){mutated, sizeof(GOLDEN_STATE_OFFER)};
    CHECK(meiso_runtime_receive(
        pair->host_runtime, &message, &event
    ) == MEISO_RUNTIME_MALFORMED);
    memmove(mutated, GOLDEN_STATE_ACCEPT, sizeof(GOLDEN_STATE_ACCEPT));
    mutated[12U + 65U] = 1U;
    message.payload = (MeisoByteView){mutated, sizeof(GOLDEN_STATE_ACCEPT)};
    CHECK(meiso_runtime_receive(
        pair->host_runtime, &message, &event
    ) == MEISO_RUNTIME_MALFORMED);
    memmove(mutated, GOLDEN_STREAM_SAMPLE, sizeof(GOLDEN_STREAM_SAMPLE));
    mutated[12U + 113U] = 1U;
    message.payload = (MeisoByteView){mutated, sizeof(GOLDEN_STREAM_SAMPLE)};
    CHECK(meiso_runtime_receive(
        pair->host_runtime, &message, &event
    ) == MEISO_RUNTIME_MALFORMED);
    memmove(mutated, GOLDEN_REJECT, sizeof(GOLDEN_REJECT));
    mutated[21U] = 1U;
    message.payload = (MeisoByteView){mutated, sizeof(GOLDEN_REJECT)};
    CHECK(meiso_runtime_receive(
        pair->host_runtime, &message, &event
    ) == MEISO_RUNTIME_MALFORMED);
    return true;
}

int main(void)
{
    RuntimePair pair;

    if (!init_pair(&pair) || !exercise_golden_encoding(&pair) ||
        !exercise_malformed_preserves_state(&pair)) {
        return 1;
    }
    (void)printf("runtime wire v2 tests passed\n");
    return 0;
}
