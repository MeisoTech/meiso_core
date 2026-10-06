#include <meiso/profiles/composition/scene.h>
#include <meiso/runtime/runtime.h>
#include <meiso/composition/scene_store.h>
#include "composition_profile_fixture.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(stderr, "CHECK failed at %s:%d: %s\n",             \
                          __FILE__, __LINE__, #condition);                       \
            return false;                                                       \
        }                                                                       \
    } while (0)

/* Independent literal MRT0 v3 contract. Do not generate these with Runtime. */
static const uint8_t GOLDEN_BLOB_BEGIN[] = {
    0x4dU, 0x52U, 0x54U, 0x30U, 0x03U, 0x07U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x78U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
    0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
    0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U,
    0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
    0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
    0x06U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x00U, 0x01U, 0x20U, 0x00U,
    0xb0U, 0x06U, 0x7bU, 0x7aU, 0xebU, 0x07U, 0x9cU, 0x61U,
    0x78U, 0x80U, 0x6bU, 0x87U, 0x8dU, 0x06U, 0x0cU, 0x75U,
    0x23U, 0x6cU, 0xf2U, 0xa1U, 0xcfU, 0x65U, 0x8cU, 0x14U,
    0x93U, 0x23U, 0x50U, 0x4aU, 0x31U, 0x35U, 0xf1U, 0x5dU,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U
};

static const uint8_t GOLDEN_BLOB_CHUNK[] = {
    0x4dU, 0x52U, 0x54U, 0x30U, 0x03U, 0x08U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x58U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
    0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
    0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U,
    0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
    0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
    0x06U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0xe1U, 0xe2U, 0xe3U, 0xe4U
};

static const uint8_t GOLDEN_BLOB_COMMIT[] = {
    0x4dU, 0x52U, 0x54U, 0x30U, 0x03U, 0x09U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x78U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
    0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
    0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U,
    0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
    0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
    0x06U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x00U, 0x01U, 0x20U, 0x00U,
    0xb0U, 0x06U, 0x7bU, 0x7aU, 0xebU, 0x07U, 0x9cU, 0x61U,
    0x78U, 0x80U, 0x6bU, 0x87U, 0x8dU, 0x06U, 0x0cU, 0x75U,
    0x23U, 0x6cU, 0xf2U, 0xa1U, 0xcfU, 0x65U, 0x8cU, 0x14U,
    0x93U, 0x23U, 0x50U, 0x4aU, 0x31U, 0x35U, 0xf1U, 0x5dU,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U
};

static const uint8_t GOLDEN_BLOB_ABORT[] = {
    0x4dU, 0x52U, 0x54U, 0x30U, 0x03U, 0x0aU, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x54U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
    0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
    0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U,
    0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
    0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
    0x06U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
};

static const uint8_t GOLDEN_BLOB_RESULT_READY[] = {
    0x4dU, 0x52U, 0x54U, 0x30U, 0x03U, 0x0bU, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x60U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
    0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
    0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U,
    0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
    0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
    0x06U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x01U, 0x01U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U
};

static const uint8_t GOLDEN_BLOB_RESULT_PUBLISHED[] = {
    0x4dU, 0x52U, 0x54U, 0x30U, 0x03U, 0x0bU, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x60U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
    0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
    0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U,
    0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
    0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
    0x06U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x03U, 0x02U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U
};

typedef union {
    max_align_t alignment;
    uint8_t bytes[65536];
} SessionArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[400000];
} RuntimeArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[1100000];
} BlobArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[5000000];
} LargeBlobArena;

typedef struct {
    SessionArena host_session_arena;
    SessionArena edge_session_arena;
    RuntimeArena host_runtime_arena;
    RuntimeArena edge_runtime_arena;
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
static const MeisoResourceId BLOB_ID = {
    UINT64_C(0x3132333435363738), UINT64_C(0x4142434445464748)
};
static const MeisoResourceId ACTUAL_ID = {
    UINT64_C(0x7172737475767778), UINT64_C(0x8182838485868788)
};
static const MeisoResourceId BLOB2_ID = {
    UINT64_C(0x9192939495969798), UINT64_C(0xa1a2a3a4a5a6a7a8)
};
static const MeisoResourceId DESIRED_ID = {
    UINT64_C(0xb1b2b3b4b5b6b7b8), UINT64_C(0xc1c2c3c4c5c6c7c8)
};
static const MeisoResourceId DATA_ID = {
    UINT64_C(0xd1d2d3d4d5d6d7d8), UINT64_C(0xe1e2e3e4e5e6e7e8)
};
static const MeisoSchemaRef CAPABILITY_SCHEMA = {{0x51U, 0x52U}, 1U};
static const MeisoSchemaRef BLOB_SCHEMA = {{0x61U, 0x62U}, 1U};
static const MeisoSchemaRef ACTUAL_SCHEMA = {{0x71U, 0x72U}, 1U};
static const MeisoSchemaRef DESIRED_SCHEMA = {{0x81U, 0x82U}, 1U};
static const MeisoSchemaRef DATA_SCHEMA = {{0x91U, 0x92U}, 1U};
static const uint8_t BLOB_BYTES[] = {0xe1U, 0xe2U, 0xe3U, 0xe4U};
static const uint8_t BLOB_SHA256[MEISO_SHA256_DIGEST_SIZE] = {
    0xb0U, 0x06U, 0x7bU, 0x7aU, 0xebU, 0x07U, 0x9cU, 0x61U,
    0x78U, 0x80U, 0x6bU, 0x87U, 0x8dU, 0x06U, 0x0cU, 0x75U,
    0x23U, 0x6cU, 0xf2U, 0xa1U, 0xcfU, 0x65U, 0x8cU, 0x14U,
    0x93U, 0x23U, 0x50U, 0x4aU, 0x31U, 0x35U, 0xf1U, 0x5dU
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
    config.profile = meiso_composition_internal_mrt0_v3_fixture_profile();
    config.limits = (MeisoSessionLimits){16384U, 8192U, 32U};
    return config;
}

static bool init_session(
    SessionArena *arena,
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

static MeisoRuntimeV3Config runtime_config(MeisoSession *session)
{
    MeisoRuntimeV3Config config;

    memset(&config, 0, sizeof(config));
    config.base.session = session;
    config.base.max_endpoints = 4U;
    config.base.max_endpoint_history = 8U;
    config.base.max_routes = 12U;
    config.base.max_capability_bytes = 128U;
    config.base.max_desired_profile_bytes_per_endpoint = 128U;
    config.base.max_actual_profile_bytes_per_endpoint = 128U;
    config.max_blob_transfer_bytes = 262144U;
    return config;
}

static bool init_runtime(
    RuntimeArena *arena,
    MeisoSession *session,
    MeisoRuntime **out_runtime
)
{
    const MeisoRuntimeV3Config config = runtime_config(session);
    const size_t size = meiso_runtime_v3_memory_required(&config);

    CHECK(size != 0U && size <= sizeof(arena->bytes));
    CHECK(meiso_runtime_v3_init(
        arena->bytes, size, &config, out_runtime
    ) == MEISO_RUNTIME_INIT_OK);
    return true;
}

static bool init_pair_with_nonce(
    RuntimePair *pair,
    uint64_t host_nonce_low,
    uint64_t edge_nonce_low
)
{
    MeisoSessionConfig host = session_config(MEISO_SESSION_ROLE_HOST);
    MeisoSessionConfig edge = session_config(MEISO_SESSION_ROLE_EDGE);

    host.local_nonce.low = host_nonce_low;
    edge.local_nonce.low = edge_nonce_low;

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

static bool init_pair(RuntimePair *pair)
{
    return init_pair_with_nonce(pair, 0x62U, 0x72U);
}

static bool receive_outbound(
    RuntimePair *pair,
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoSessionRuntimeMessage *out_message,
    const uint8_t *golden,
    size_t golden_size
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
    if (golden != NULL) {
        CHECK(out_message->payload.size == golden_size);
        CHECK(memcmp(out_message->payload.data, golden, golden_size) == 0);
    }
    return true;
}

static void blob_catalog(
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
        {BLOB_ID, MEISO_RESOURCE_KIND_BLOB, BLOB_SCHEMA,
         MEISO_OWNER_ROLE_HOST, 262144U},
        MEISO_RUNTIME_FACET_IMMUTABLE
    };
}

static MeisoRuntimeRouteKey blob_route_key(void)
{
    const MeisoRuntimeRouteKey key = {
        1U, ENDPOINT, ENDPOINT_INCARNATION, BLOB_ID,
        MEISO_RUNTIME_FACET_IMMUTABLE
    };
    return key;
}

static MeisoContentId blob_content_id(void)
{
    MeisoContentId content_id;

    memset(&content_id, 0, sizeof(content_id));
    content_id.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    content_id.digest_size = MEISO_SHA256_DIGEST_SIZE;
    memmove(content_id.digest, BLOB_SHA256, sizeof(BLOB_SHA256));
    return content_id;
}

static bool activate_blob_catalog(RuntimePair *pair)
{
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor route;
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};

    blob_catalog(&endpoint, &route);
    CHECK(meiso_runtime_offer_catalog(
        pair->edge_runtime, 1U, &endpoint, 1U, &route, 1U
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair, pair->edge_session, pair->host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair->host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED);
    CHECK(meiso_runtime_accept_catalog(pair->host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair, pair->host_session, pair->edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair->edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED);
    return true;
}

static bool test_frozen_v3_literals(void)
{
    CHECK(MEISO_RUNTIME_WIRE_VERSION_V2 == 2U &&
          MEISO_RUNTIME_WIRE_VERSION_V3 == 3U &&
          MEISO_RUNTIME_WIRE_VERSION_V4 == 4U);
    CHECK(meiso_composition_internal_mrt0_v2_fixture_profile().version == 1U &&
          meiso_composition_internal_mrt0_v3_fixture_profile().version == 2U);
    CHECK(sizeof(GOLDEN_BLOB_BEGIN) ==
              MEISO_RUNTIME_V3_BLOB_BEGIN_WIRE_SIZE &&
          sizeof(GOLDEN_BLOB_CHUNK) ==
              MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE + 4U &&
          sizeof(GOLDEN_BLOB_COMMIT) ==
              MEISO_RUNTIME_V3_BLOB_COMMIT_WIRE_SIZE &&
          sizeof(GOLDEN_BLOB_ABORT) ==
              MEISO_RUNTIME_V3_BLOB_ABORT_WIRE_SIZE &&
          sizeof(GOLDEN_BLOB_RESULT_READY) ==
              MEISO_RUNTIME_V3_BLOB_RESULT_WIRE_SIZE &&
          sizeof(GOLDEN_BLOB_RESULT_PUBLISHED) ==
              MEISO_RUNTIME_V3_BLOB_RESULT_WIRE_SIZE);
    CHECK(GOLDEN_BLOB_BEGIN[4] == 3U && GOLDEN_BLOB_BEGIN[5] == 7U &&
          GOLDEN_BLOB_CHUNK[4] == 3U && GOLDEN_BLOB_CHUNK[5] == 8U &&
          GOLDEN_BLOB_COMMIT[4] == 3U && GOLDEN_BLOB_COMMIT[5] == 9U &&
          GOLDEN_BLOB_ABORT[4] == 3U && GOLDEN_BLOB_ABORT[5] == 10U &&
          GOLDEN_BLOB_RESULT_READY[4] == 3U &&
          GOLDEN_BLOB_RESULT_READY[5] == 11U);
    CHECK(GOLDEN_BLOB_BEGIN[75] == 0x01U &&
          GOLDEN_BLOB_BEGIN[76] == 0x00U &&
          GOLDEN_BLOB_BEGIN[77] == 0x01U &&
          GOLDEN_BLOB_BEGIN[78] == 0x20U &&
          GOLDEN_BLOB_BEGIN[79] == 0x00U &&
          GOLDEN_BLOB_RESULT_READY[76] == MEISO_RUNTIME_BLOB_BEGIN &&
          GOLDEN_BLOB_RESULT_READY[77] ==
              MEISO_RUNTIME_BLOB_RESULT_READY);
    return true;
}

static bool test_golden_publish_flow(void)
{
    RuntimePair pair;
    BlobArena blob_arena;
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor route;
    MeisoRuntimeBlobTransferRef transfer = {0};
    MeisoRuntimeBlobPublishReport report = {0};
    MeisoRuntimeBlobSnapshot blob_snapshot = {0};
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};
    MeisoBlobStore *store = NULL;
    MeisoBlobView view = {0};
    MeisoBlobStats before = {0};
    MeisoBlobStats after = {0};
    MeisoBlobConfig blob_config = {2U};
    MeisoContentId content_id = blob_content_id();
    size_t blob_arena_size;

    CHECK(init_pair(&pair));
    CHECK(activate_blob_catalog(&pair));
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        blob_route_key(),
        &content_id,
        sizeof(BLOB_BYTES),
        &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer.transfer_id == 1U &&
          transfer.total_size == sizeof(BLOB_BYTES));
    CHECK(receive_outbound(
        &pair,
        pair.host_session,
        pair.edge_session,
        &message,
        GOLDEN_BLOB_BEGIN,
        sizeof(GOLDEN_BLOB_BEGIN)
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN &&
          event.blob_transfer.transfer_id == transfer.transfer_id &&
          event.blob_payload.data == NULL && event.blob_payload.size == 0U);
    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime, &event.blob_transfer, MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair,
        pair.edge_session,
        pair.host_session,
        &message,
        GOLDEN_BLOB_RESULT_READY,
        sizeof(GOLDEN_BLOB_RESULT_READY)
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_RESULT &&
          event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN &&
          event.blob_result == MEISO_RUNTIME_BLOB_RESULT_READY &&
          event.blob_next_offset == 0U);

    CHECK(meiso_runtime_send_blob_chunk(
        pair.host_runtime,
        &transfer,
        0U,
        (MeisoByteView){BLOB_BYTES, sizeof(BLOB_BYTES)}
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair,
        pair.host_session,
        pair.edge_session,
        &message,
        GOLDEN_BLOB_CHUNK,
        sizeof(GOLDEN_BLOB_CHUNK)
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          event.blob_operation == MEISO_RUNTIME_BLOB_CHUNK &&
          event.blob_offset == 0U &&
          event.blob_payload.size == sizeof(BLOB_BYTES) &&
          memcmp(event.blob_payload.data, BLOB_BYTES, sizeof(BLOB_BYTES)) == 0);

    CHECK(meiso_runtime_send_blob_commit(
        pair.host_runtime, &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair,
        pair.host_session,
        pair.edge_session,
        &message,
        GOLDEN_BLOB_COMMIT,
        sizeof(GOLDEN_BLOB_COMMIT)
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          event.blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          event.blob_payload.size == sizeof(BLOB_BYTES) &&
          memcmp(event.blob_payload.data, BLOB_BYTES, sizeof(BLOB_BYTES)) == 0);

    blob_catalog(&endpoint, &route);
    blob_arena_size = meiso_blob_store_memory_required(
        &route.resource, &blob_config
    );
    CHECK(blob_arena_size != 0U &&
          blob_arena_size <= sizeof(blob_arena.bytes));
    CHECK(meiso_blob_store_init(
        blob_arena.bytes,
        blob_arena_size,
        &route.resource,
        &blob_config,
        &store
    ) == MEISO_BLOB_INIT_OK);
    CHECK(meiso_blob_store_stats(store, &before));
    CHECK(meiso_blob_store_count(store) == 0U &&
          before.stages_started == 0U && before.publications == 0U);
    CHECK(meiso_runtime_publish_blob_receive(
        pair.edge_runtime,
        &event.blob_transfer,
        store,
        meiso_runtime_blob_sha256_verify,
        NULL,
        &report
    ) == MEISO_RUNTIME_OK);
    CHECK(report.stage_attempted && report.commit_attempted &&
          !report.forced_abort_attempted &&
          report.stage_status == MEISO_BLOB_STAGE_STARTED &&
          report.commit_status == MEISO_BLOB_COMMIT_PUBLISHED &&
          report.result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED);
    CHECK(meiso_blob_store_count(store) == 1U);
    CHECK(meiso_blob_store_stats(store, &after));
    CHECK(after.stages_started == before.stages_started + 1U &&
          after.publications == before.publications + 1U);
    CHECK(meiso_blob_store_lookup(store, &content_id, &view));
    CHECK(view.content.size == sizeof(BLOB_BYTES) &&
          memcmp(view.content.data, BLOB_BYTES, sizeof(BLOB_BYTES)) == 0);

    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime, &event.blob_transfer, MEISO_RUNTIME_BLOB_COMMIT
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair,
        pair.edge_session,
        pair.host_session,
        &message,
        GOLDEN_BLOB_RESULT_PUBLISHED,
        sizeof(GOLDEN_BLOB_RESULT_PUBLISHED)
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_RESULT &&
          event.blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          event.blob_result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
          event.blob_next_offset == sizeof(BLOB_BYTES));
    CHECK(meiso_runtime_blob_snapshot(
        pair.host_runtime, &blob_snapshot
    ));
    CHECK(!blob_snapshot.transfer_active &&
          blob_snapshot.transfer_id_high_water == 1U &&
          blob_snapshot.wire_version == MEISO_RUNTIME_WIRE_VERSION_V3);
    return true;
}

static bool test_profile_and_runtime_version_selection(void)
{
    RuntimePair pair;
    RuntimeArena host_runtime_arena;
    RuntimeArena edge_runtime_arena;
    SessionArena host_session_arena;
    SessionArena edge_session_arena;
    MeisoSession *host_session = NULL;
    MeisoSession *edge_session = NULL;
    MeisoRuntime *host_runtime = NULL;
    MeisoRuntime *edge_runtime = NULL;
    MeisoSessionConfig host = session_config(MEISO_SESSION_ROLE_HOST);
    MeisoSessionConfig edge = session_config(MEISO_SESSION_ROLE_EDGE);
    MeisoRuntimeConfig host_runtime_config;
    MeisoRuntimeConfig edge_runtime_config;
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor route;
    MeisoRuntimeBlobSnapshot snapshot = {0};
    size_t host_runtime_size;
    size_t edge_runtime_size;

    CHECK(meiso_profile_ref_equal(
        meiso_composition_internal_mrt0_v3_fixture_profile(), host.profile
    ));

    host.profile = meiso_composition_internal_mrt0_v2_fixture_profile();
    edge.profile = meiso_composition_internal_mrt0_v2_fixture_profile();
    CHECK(init_session(&host_session_arena, &host, &host_session));
    CHECK(init_session(&edge_session_arena, &edge, &edge_session));
    CHECK(meiso_session_start(edge_session) == MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(host_session) == MEISO_SESSION_START_STARTED);
    CHECK(handshake(host_session, edge_session));
    CHECK(handshake(edge_session, host_session));
    CHECK(handshake(host_session, edge_session));
    CHECK(handshake(edge_session, host_session));

    host_runtime_config = runtime_config(host_session).base;
    edge_runtime_config = runtime_config(edge_session).base;
    host_runtime_size = meiso_runtime_memory_required(&host_runtime_config);
    edge_runtime_size = meiso_runtime_memory_required(&edge_runtime_config);
    CHECK(host_runtime_size != 0U &&
          host_runtime_size <= sizeof(host_runtime_arena.bytes));
    CHECK(edge_runtime_size != 0U &&
          edge_runtime_size <= sizeof(edge_runtime_arena.bytes));
    CHECK(meiso_runtime_init(
        host_runtime_arena.bytes,
        host_runtime_size,
        &host_runtime_config,
        &host_runtime
    ) == MEISO_RUNTIME_INIT_OK);
    CHECK(meiso_runtime_init(
        edge_runtime_arena.bytes,
        edge_runtime_size,
        &edge_runtime_config,
        &edge_runtime
    ) == MEISO_RUNTIME_INIT_OK);
    blob_catalog(&endpoint, &route);
    CHECK(meiso_runtime_offer_catalog(
        edge_runtime, 1U, &endpoint, 1U, &route, 1U
    ) == MEISO_RUNTIME_UNSUPPORTED_KIND);
    CHECK(meiso_runtime_blob_snapshot(host_runtime, &snapshot));
    CHECK(snapshot.wire_version == MEISO_RUNTIME_WIRE_VERSION_V2 &&
          snapshot.max_transfer_bytes == 0U &&
          !snapshot.transfer_active);

    CHECK(init_pair(&pair));
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &snapshot));
    CHECK(snapshot.wire_version == MEISO_RUNTIME_WIRE_VERSION_V3 &&
          snapshot.max_transfer_bytes == 262144U);
    return true;
}

static bool test_profile_mismatch_and_wire_mismatch(void)
{
    RuntimePair pair;
    SessionArena host_arena;
    SessionArena edge_arena;
    MeisoSession *host_session = NULL;
    MeisoSession *edge_session = NULL;
    MeisoSessionConfig host = session_config(MEISO_SESSION_ROLE_HOST);
    MeisoSessionConfig edge = session_config(MEISO_SESSION_ROLE_EDGE);
    MeisoSessionRuntimeMessage message = {0};
    MeisoSessionSnapshot session_snapshot = {0};
    MeisoRuntimeEvent event = {0};
    MeisoByteView outbound;
    uint8_t changed[sizeof(GOLDEN_BLOB_BEGIN)];

    edge.profile = meiso_composition_internal_mrt0_v2_fixture_profile();
    CHECK(init_session(&host_arena, &host, &host_session));
    CHECK(init_session(&edge_arena, &edge, &edge_session));
    CHECK(meiso_session_start(edge_session) == MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(host_session) == MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_next_outbound(host_session, &outbound));
    CHECK(outbound.size <= sizeof(changed));
    memmove(changed, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(host_session) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        edge_session,
        (MeisoByteView){changed, outbound.size},
        &message
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(meiso_session_snapshot(edge_session, &session_snapshot));
    CHECK(session_snapshot.failure == MEISO_SESSION_FAILURE_PROFILE_MISMATCH);

    CHECK(init_pair(&pair));
    CHECK(meiso_session_snapshot(pair.edge_session, &session_snapshot));
    message.session_id = session_snapshot.session_id;
    message.payload = (MeisoByteView){changed, sizeof(changed)};

    memmove(changed, GOLDEN_BLOB_BEGIN, sizeof(changed));
    changed[4] = MEISO_RUNTIME_WIRE_VERSION_V2;
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_WIRE_VERSION_MISMATCH);

    memmove(changed, GOLDEN_BLOB_BEGIN, sizeof(changed));
    changed[0] ^= 1U;
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_MALFORMED);

    memmove(changed, GOLDEN_BLOB_BEGIN, sizeof(changed));
    changed[11] -= 1U;
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_MALFORMED);
    return true;
}

static bool test_v3_arena_contract(void)
{
    RuntimePair pair;
    RuntimeArena exact;
    MeisoRuntime *runtime = NULL;
    MeisoRuntimeV3Config config;
    size_t required;

    CHECK(init_pair(&pair));
    config = runtime_config(pair.host_session);
    required = meiso_runtime_v3_memory_required(&config);
    CHECK(required != 0U && required <= sizeof(exact.bytes));
    CHECK(meiso_runtime_v3_init(
        exact.bytes, required - 1U, &config, &runtime
    ) == MEISO_RUNTIME_INIT_ARENA_TOO_SMALL);
    CHECK(runtime == NULL);
    CHECK(meiso_runtime_v3_init(
        exact.bytes + 1U, required, &config, &runtime
    ) == MEISO_RUNTIME_INIT_MISALIGNED_ARENA);
    CHECK(runtime == NULL);
    CHECK(meiso_runtime_v3_init(
        exact.bytes,
        required,
        &config,
        (MeisoRuntime **)(void *)exact.bytes
    ) == MEISO_RUNTIME_INIT_INVALID_ARGUMENT);
    CHECK(meiso_runtime_v3_init(
        exact.bytes, required, &config, &runtime
    ) == MEISO_RUNTIME_INIT_OK);
    CHECK(runtime != NULL);

    config.max_blob_transfer_bytes = 0U;
    CHECK(meiso_runtime_v3_memory_required(&config) == 0U);
    return true;
}

static bool blob_snapshot_equal(
    const MeisoRuntimeBlobSnapshot *lhs,
    const MeisoRuntimeBlobSnapshot *rhs
)
{
    return memcmp(lhs, rhs, sizeof(*lhs)) == 0;
}

static bool test_malformed_blob_framing(void)
{
    RuntimePair pair;
    MeisoSessionSnapshot session_snapshot = {0};
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event;
    MeisoRuntimeBlobSnapshot before = {0};
    MeisoRuntimeBlobSnapshot after = {0};
    uint8_t changed[sizeof(GOLDEN_BLOB_BEGIN) + 1U];
    size_t prefix;

    CHECK(init_pair(&pair));
    CHECK(meiso_session_snapshot(pair.edge_session, &session_snapshot));
    message.session_id = session_snapshot.session_id;
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &before));

    for (prefix = 0U; prefix < sizeof(GOLDEN_BLOB_BEGIN); ++prefix) {
        memset(&event, 0xa5, sizeof(event));
        message.payload = (MeisoByteView){GOLDEN_BLOB_BEGIN, prefix};
        CHECK(meiso_runtime_receive(
            pair.edge_runtime, &message, &event
        ) == MEISO_RUNTIME_MALFORMED);
        CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &after));
        CHECK(blob_snapshot_equal(&before, &after));
    }

    memmove(changed, GOLDEN_BLOB_BEGIN, sizeof(GOLDEN_BLOB_BEGIN));
    changed[sizeof(GOLDEN_BLOB_BEGIN)] = 0U;
    message.payload = (MeisoByteView){changed, sizeof(changed)};
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_MALFORMED);

    memmove(changed, GOLDEN_BLOB_BEGIN, sizeof(GOLDEN_BLOB_BEGIN));
    changed[67] = 1U;
    message.payload.size = sizeof(GOLDEN_BLOB_BEGIN);
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_MALFORMED);

    memmove(changed, GOLDEN_BLOB_BEGIN, sizeof(GOLDEN_BLOB_BEGIN));
    changed[79] = 1U;
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_MALFORMED);

    memmove(changed, GOLDEN_BLOB_BEGIN, sizeof(GOLDEN_BLOB_BEGIN));
    changed[5] = 0xffU;
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_MALFORMED);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));

    {
        const struct {
            const uint8_t *bytes;
            size_t size;
            MeisoRuntime *runtime;
            size_t reserved_offset;
        } vectors[] = {
            {GOLDEN_BLOB_CHUNK, sizeof(GOLDEN_BLOB_CHUNK),
             pair.edge_runtime, 67U},
            {GOLDEN_BLOB_COMMIT, sizeof(GOLDEN_BLOB_COMMIT),
             pair.edge_runtime, 79U},
            {GOLDEN_BLOB_ABORT, sizeof(GOLDEN_BLOB_ABORT),
             pair.edge_runtime, 77U},
            {GOLDEN_BLOB_RESULT_READY, sizeof(GOLDEN_BLOB_RESULT_READY),
             pair.host_runtime, 78U}
        };
        size_t vector_index;

        for (vector_index = 0U;
             vector_index < sizeof(vectors) / sizeof(vectors[0]);
             ++vector_index) {
            CHECK(meiso_runtime_blob_snapshot(
                vectors[vector_index].runtime, &before
            ));
            for (prefix = 0U; prefix < vectors[vector_index].size;
                 ++prefix) {
                message.payload = (MeisoByteView){
                    vectors[vector_index].bytes, prefix
                };
                CHECK(meiso_runtime_receive(
                    vectors[vector_index].runtime, &message, &event
                ) == MEISO_RUNTIME_MALFORMED);
                CHECK(meiso_runtime_blob_snapshot(
                    vectors[vector_index].runtime, &after
                ));
                CHECK(blob_snapshot_equal(&before, &after));
            }
            memmove(
                changed,
                vectors[vector_index].bytes,
                vectors[vector_index].size
            );
            changed[vectors[vector_index].size] = 0U;
            message.payload = (MeisoByteView){
                changed, vectors[vector_index].size + 1U
            };
            CHECK(meiso_runtime_receive(
                vectors[vector_index].runtime, &message, &event
            ) == MEISO_RUNTIME_MALFORMED);
            changed[vectors[vector_index].reserved_offset] = 1U;
            message.payload.size = vectors[vector_index].size;
            CHECK(meiso_runtime_receive(
                vectors[vector_index].runtime, &message, &event
            ) == MEISO_RUNTIME_MALFORMED);
            CHECK(meiso_runtime_blob_snapshot(
                vectors[vector_index].runtime, &after
            ));
            CHECK(blob_snapshot_equal(&before, &after));
        }
    }
    return true;
}

static bool test_abort_before_ready_and_late_ready(void)
{
    RuntimePair pair;
    MeisoContentId content_id = blob_content_id();
    MeisoRuntimeBlobTransferRef transfer = {0};
    MeisoRuntimeBlobSnapshot snapshot = {0};
    MeisoSessionRuntimeMessage begin_message = {0};
    MeisoSessionRuntimeMessage abort_message = {0};
    MeisoSessionRuntimeMessage ready_message = {0};
    MeisoSessionRuntimeMessage aborted_message = {0};
    MeisoRuntimeEvent edge_event = {0};
    MeisoRuntimeEvent host_event = {0};

    CHECK(init_pair(&pair));
    CHECK(activate_blob_catalog(&pair));
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        blob_route_key(),
        &content_id,
        sizeof(BLOB_BYTES),
        &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair,
        pair.host_session,
        pair.edge_session,
        &begin_message,
        GOLDEN_BLOB_BEGIN,
        sizeof(GOLDEN_BLOB_BEGIN)
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &begin_message, &edge_event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime,
        &edge_event.blob_transfer,
        MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_OK);

    CHECK(meiso_runtime_send_blob_abort(
        pair.host_runtime,
        &transfer,
        MEISO_RUNTIME_BLOB_ABORT_CALLER_REQUEST
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair,
        pair.host_session,
        pair.edge_session,
        &abort_message,
        GOLDEN_BLOB_ABORT,
        sizeof(GOLDEN_BLOB_ABORT)
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &abort_message, &edge_event
    ) == MEISO_RUNTIME_OK);
    CHECK(edge_event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          edge_event.blob_operation == MEISO_RUNTIME_BLOB_ABORT);

    CHECK(receive_outbound(
        &pair,
        pair.edge_session,
        pair.host_session,
        &ready_message,
        GOLDEN_BLOB_RESULT_READY,
        sizeof(GOLDEN_BLOB_RESULT_READY)
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &ready_message, &host_event
    ) == MEISO_RUNTIME_OK);
    CHECK(host_event.type == MEISO_RUNTIME_EVENT_BLOB_RESULT &&
          host_event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN &&
          host_event.blob_result == MEISO_RUNTIME_BLOB_RESULT_READY);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &snapshot));
    CHECK(snapshot.transfer_active && !snapshot.begin_ready &&
          snapshot.pending_operation == MEISO_RUNTIME_BLOB_ABORT);

    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime,
        &edge_event.blob_transfer,
        MEISO_RUNTIME_BLOB_ABORT
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair,
        pair.edge_session,
        pair.host_session,
        &aborted_message,
        NULL,
        0U
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &aborted_message, &host_event
    ) == MEISO_RUNTIME_OK);
    CHECK(host_event.type == MEISO_RUNTIME_EVENT_BLOB_RESULT &&
          host_event.blob_operation == MEISO_RUNTIME_BLOB_ABORT &&
          host_event.blob_result == MEISO_RUNTIME_BLOB_RESULT_ABORTED);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &snapshot));
    CHECK(!snapshot.transfer_active &&
          snapshot.transfer_id_high_water == 1U);
    return true;
}

static bool occupy_session_tx(MeisoSession *session)
{
    static const uint8_t dummy = 0x5aU;

    CHECK(meiso_session_submit_runtime(
        session, (MeisoByteView){&dummy, 1U}
    ) == MEISO_SESSION_SUBMIT_QUEUED);
    return true;
}

static bool test_tx_busy_is_zero_progress(void)
{
    RuntimePair pair;
    MeisoContentId content_id = blob_content_id();
    MeisoRuntimeBlobTransferRef transfer;
    MeisoRuntimeBlobTransferRef sentinel;
    MeisoRuntimeBlobSnapshot before = {0};
    MeisoRuntimeBlobSnapshot after = {0};
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};

    CHECK(init_pair(&pair));
    CHECK(activate_blob_catalog(&pair));
    memset(&transfer, 0xa5, sizeof(transfer));
    sentinel = transfer;
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &before));
    CHECK(occupy_session_tx(pair.host_session));
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        blob_route_key(),
        &content_id,
        sizeof(BLOB_BYTES),
        &transfer
    ) == MEISO_RUNTIME_TX_BUSY);
    CHECK(memcmp(&transfer, &sentinel, sizeof(transfer)) == 0);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));
    CHECK(meiso_session_mark_outbound_sent(pair.host_session) ==
          MEISO_SESSION_OUTBOUND_SENT);

    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        blob_route_key(),
        &content_id,
        sizeof(BLOB_BYTES),
        &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair,
        pair.host_session,
        pair.edge_session,
        &message,
        GOLDEN_BLOB_BEGIN,
        sizeof(GOLDEN_BLOB_BEGIN)
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_blob_chunk(
        pair.host_runtime,
        &transfer,
        0U,
        (MeisoByteView){BLOB_BYTES, sizeof(BLOB_BYTES)}
    ) == MEISO_RUNTIME_BLOB_BUSY);

    CHECK(occupy_session_tx(pair.edge_session));
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &before));
    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime,
        &event.blob_transfer,
        MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_TX_BUSY);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));
    CHECK(meiso_session_mark_outbound_sent(pair.edge_session) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime,
        &event.blob_transfer,
        MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair,
        pair.edge_session,
        pair.host_session,
        &message,
        GOLDEN_BLOB_RESULT_READY,
        sizeof(GOLDEN_BLOB_RESULT_READY)
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &after));
    CHECK(after.transfer_active && after.begin_ready &&
          after.next_offset == 0U);
    return true;
}

static bool test_blob_send_cannot_bypass_pending_state_receive(void)
{
    RuntimePair pair;
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor routes[2];
    MeisoRuntimeBlobTransferRef transfer;
    MeisoRuntimeBlobTransferRef output;
    MeisoContentId content_id = blob_content_id();
    MeisoRuntimeActualState actual;
    MeisoStateOffer offer;
    MeisoByteView payload;
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};
    uint8_t actual_bytes[128];

    CHECK(init_pair(&pair));
    blob_catalog(&endpoint, &routes[0]);
    routes[1] = (MeisoRuntimeRouteDescriptor){
        ENDPOINT,
        ENDPOINT_INCARNATION,
        {ACTUAL_ID, MEISO_RESOURCE_KIND_STATE, ACTUAL_SCHEMA,
         MEISO_OWNER_ROLE_EDGE, 128U},
        MEISO_RUNTIME_FACET_ACTUAL
    };
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 1U, &endpoint, 1U, routes, 2U
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair,
        pair.edge_session,
        pair.host_session,
        &message,
        NULL,
        0U
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_accept_catalog(pair.host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair,
        pair.host_session,
        pair.edge_session,
        &message,
        NULL,
        0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);

    memset(&actual, 0, sizeof(actual));
    actual.apply_result = MEISO_RUNTIME_APPLY_NONE;
    actual.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    actual.effective_enabled = false;
    actual.production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
    actual.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    actual.production_epoch = 0U;
    CHECK(meiso_runtime_actual_state_encode(
        &actual,
        (MeisoMutableBuffer){actual_bytes, sizeof(actual_bytes)},
        &payload
    ));
    offer.generation.value = 1U;
    offer.state.owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 41U};
    offer.state.version = 1U;
    offer.state.payload = payload;
    CHECK(meiso_runtime_send_state_offer(
        pair.edge_runtime,
        (MeisoRuntimeRouteKey){
            1U, ENDPOINT, ENDPOINT_INCARNATION, ACTUAL_ID,
            MEISO_RUNTIME_FACET_ACTUAL
        },
        &offer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair,
        pair.edge_session,
        pair.host_session,
        &message,
        NULL,
        0U
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER);

    transfer = (MeisoRuntimeBlobTransferRef){
        blob_route_key(), 1U, content_id, sizeof(BLOB_BYTES)
    };
    memset(&output, 0xa5, sizeof(output));
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        blob_route_key(),
        &content_id,
        sizeof(BLOB_BYTES),
        &output
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(meiso_runtime_send_blob_chunk(
        pair.host_runtime,
        &transfer,
        0U,
        (MeisoByteView){BLOB_BYTES, sizeof(BLOB_BYTES)}
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(meiso_runtime_send_blob_commit(
        pair.host_runtime, &transfer
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(meiso_runtime_send_blob_abort(
        pair.host_runtime,
        &transfer,
        MEISO_RUNTIME_BLOB_ABORT_CALLER_REQUEST
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(meiso_runtime_commit_state_receive(
        pair.host_runtime,
        event.route,
        &event.state_offer,
        MEISO_STATE_RECEIVE_DUPLICATE
    ) == MEISO_RUNTIME_OK);
    return true;
}

static MeisoContentId sha256_content_id(const uint8_t digest[32])
{
    MeisoContentId value;

    memset(&value, 0, sizeof(value));
    value.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    value.digest_size = MEISO_SHA256_DIGEST_SIZE;
    memmove(value.digest, digest, MEISO_SHA256_DIGEST_SIZE);
    return value;
}

static bool exercise_reassembly_size(
    size_t total_size,
    const uint8_t digest[32]
)
{
    RuntimePair pair;
    static uint8_t content[262144];
    MeisoContentId content_id = sha256_content_id(digest);
    MeisoRuntimeBlobTransferRef transfer = {0};
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};
    size_t offset;

    CHECK(total_size <= sizeof(content));
    for (offset = 0U; offset < total_size; ++offset) {
        content[offset] = (uint8_t)offset;
    }
    CHECK(init_pair(&pair));
    CHECK(activate_blob_catalog(&pair));
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        blob_route_key(),
        &content_id,
        total_size,
        &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime, &event.blob_transfer, MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.edge_session, pair.host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);

    offset = 0U;
    while (offset < total_size) {
        const size_t remaining = total_size - offset;
        const size_t chunk_size =
            remaining < MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE ?
                remaining : MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE;

        CHECK(meiso_runtime_send_blob_chunk(
            pair.host_runtime,
            &transfer,
            offset,
            (MeisoByteView){content + offset, chunk_size}
        ) == MEISO_RUNTIME_OK);
        CHECK(receive_outbound(
            &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
        ));
        CHECK(meiso_runtime_receive(
            pair.edge_runtime, &message, &event
        ) == MEISO_RUNTIME_OK);
        CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
              event.blob_operation == MEISO_RUNTIME_BLOB_CHUNK &&
              event.blob_offset == offset &&
              event.blob_payload.size == chunk_size &&
              memcmp(event.blob_payload.data, content + offset,
                     chunk_size) == 0);
        offset += chunk_size;
    }
    CHECK(meiso_runtime_send_blob_commit(
        pair.host_runtime, &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          event.blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          event.blob_payload.size == total_size &&
          (total_size == 0U ||
           memcmp(event.blob_payload.data, content, total_size) == 0));
    return true;
}

static bool test_reassembly_boundaries(void)
{
    static const uint8_t EMPTY_SHA[32] = {
        0xe3U, 0xb0U, 0xc4U, 0x42U, 0x98U, 0xfcU, 0x1cU, 0x14U,
        0x9aU, 0xfbU, 0xf4U, 0xc8U, 0x99U, 0x6fU, 0xb9U, 0x24U,
        0x27U, 0xaeU, 0x41U, 0xe4U, 0x64U, 0x9bU, 0x93U, 0x4cU,
        0xa4U, 0x95U, 0x99U, 0x1bU, 0x78U, 0x52U, 0xb8U, 0x55U
    };
    static const uint8_t EXACT_CHUNK_SHA[32] = {
        0xc8U, 0xf5U, 0xd0U, 0x34U, 0x1dU, 0x54U, 0xd9U, 0x51U,
        0xa7U, 0x1bU, 0x13U, 0x6eU, 0x6eU, 0x2aU, 0xfcU, 0xb1U,
        0x4dU, 0x11U, 0xedU, 0x84U, 0x89U, 0xa7U, 0xaeU, 0x12U,
        0x6aU, 0x8fU, 0xeeU, 0x0dU, 0xf6U, 0xecU, 0xf1U, 0x93U
    };
    static const uint8_t SHORT_FINAL_SHA[32] = {
        0x1eU, 0x97U, 0x3dU, 0x02U, 0x9dU, 0xf2U, 0xb2U, 0xc6U,
        0x6cU, 0xb4U, 0x2aU, 0x94U, 0x2cU, 0x5eU, 0xdbU, 0x45U,
        0x96U, 0x6fU, 0x02U, 0xabU, 0xafU, 0xf2U, 0x9fU, 0xe9U,
        0x94U, 0x10U, 0xe4U, 0x4dU, 0x27U, 0x1dU, 0x0eU, 0xfcU
    };
    static const uint8_t MAX_SHA[32] = {
        0x23U, 0x12U, 0x39U, 0x4bU, 0xd9U, 0x95U, 0x45U, 0xd9U,
        0xdeU, 0x13U, 0x1cU, 0x24U, 0xefU, 0xb7U, 0x81U, 0xe7U,
        0x65U, 0xacU, 0x1aU, 0xecU, 0x24U, 0x3fU, 0x2eU, 0xd9U,
        0x34U, 0x75U, 0x97U, 0xa7U, 0x93U, 0xa4U, 0x15U, 0xe9U
    };

    CHECK(exercise_reassembly_size(0U, EMPTY_SHA));
    CHECK(exercise_reassembly_size(4096U, EXACT_CHUNK_SHA));
    CHECK(exercise_reassembly_size(4097U, SHORT_FINAL_SHA));
    CHECK(exercise_reassembly_size(262144U, MAX_SHA));
    return true;
}

static bool receive_small_blob_to_commit(
    RuntimePair *pair,
    MeisoRuntimeBlobTransferRef *out_transfer,
    MeisoRuntimeEvent *out_commit_event
)
{
    MeisoContentId content_id = blob_content_id();
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};

    CHECK(init_pair(pair));
    CHECK(activate_blob_catalog(pair));
    CHECK(meiso_runtime_send_blob_begin(
        pair->host_runtime,
        blob_route_key(),
        &content_id,
        sizeof(BLOB_BYTES),
        out_transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair,
        pair->host_session,
        pair->edge_session,
        &message,
        NULL,
        0U
    ));
    CHECK(meiso_runtime_receive(
        pair->edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_blob_result(
        pair->edge_runtime, &event.blob_transfer, MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair,
        pair->edge_session,
        pair->host_session,
        &message,
        NULL,
        0U
    ));
    CHECK(meiso_runtime_receive(
        pair->host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_blob_chunk(
        pair->host_runtime,
        out_transfer,
        0U,
        (MeisoByteView){BLOB_BYTES, sizeof(BLOB_BYTES)}
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair,
        pair->host_session,
        pair->edge_session,
        &message,
        NULL,
        0U
    ));
    CHECK(meiso_runtime_receive(
        pair->edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_blob_commit(
        pair->host_runtime, out_transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair,
        pair->host_session,
        pair->edge_session,
        &message,
        NULL,
        0U
    ));
    CHECK(meiso_runtime_receive(
        pair->edge_runtime, &message, out_commit_event
    ) == MEISO_RUNTIME_OK);
    CHECK(out_commit_event->type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          out_commit_event->blob_operation == MEISO_RUNTIME_BLOB_COMMIT);
    return true;
}

static bool init_test_blob_store(
    BlobArena *arena,
    MeisoBlobStore **out_store
)
{
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor route;
    const MeisoBlobConfig config = {2U};
    size_t required;

    blob_catalog(&endpoint, &route);
    required = meiso_blob_store_memory_required(&route.resource, &config);
    CHECK(required != 0U && required <= sizeof(arena->bytes));
    CHECK(meiso_blob_store_init(
        arena->bytes,
        required,
        &route.resource,
        &config,
        out_store
    ) == MEISO_BLOB_INIT_OK);
    return true;
}

static bool reject_blob(
    const MeisoContentId *expected_content_id,
    MeisoByteView content,
    void *context
)
{
    (void)expected_content_id;
    (void)content;
    (void)context;
    return false;
}

static bool accept_blob(
    const MeisoContentId *expected_content_id,
    MeisoByteView content,
    void *context
)
{
    (void)expected_content_id;
    (void)content;
    (void)context;
    return true;
}

static bool test_core_staging_ownership_and_forced_abort(void)
{
    size_t variant;

    for (variant = 0U; variant < 2U; ++variant) {
        RuntimePair pair;
        BlobArena blob_arena;
        MeisoBlobStore *store = NULL;
        MeisoRuntimeBlobTransferRef transfer = {0};
        MeisoRuntimeEvent commit_event = {0};
        MeisoRuntimeEvent result_event = {0};
        MeisoSessionRuntimeMessage message = {0};
        MeisoRuntimeBlobPublishReport report = {0};
        MeisoMutableBuffer staging = {0};
        MeisoMutableBuffer still_staging = {0};
        MeisoContentId staged_id = blob_content_id();

        CHECK(init_test_blob_store(&blob_arena, &store));
        if (variant != 0U) {
            staged_id.digest[0] ^= 0xffU;
        }
        CHECK(meiso_blob_store_stage_begin(
            store, &staged_id, sizeof(BLOB_BYTES)
        ) == MEISO_BLOB_STAGE_STARTED);
        CHECK(meiso_blob_store_staging_buffer(store, &staging));
        memmove(staging.data, BLOB_BYTES, sizeof(BLOB_BYTES));

        CHECK(receive_small_blob_to_commit(
            &pair, &transfer, &commit_event
        ));
        CHECK(meiso_runtime_publish_blob_receive(
            pair.edge_runtime,
            &commit_event.blob_transfer,
            store,
            meiso_runtime_blob_sha256_verify,
            NULL,
            &report
        ) == MEISO_RUNTIME_OK);
        CHECK(report.stage_attempted && !report.commit_attempted &&
              !report.forced_abort_attempted &&
              report.stage_status == (variant == 0U ?
                  MEISO_BLOB_STAGE_UNCHANGED : MEISO_BLOB_STAGE_BUSY) &&
              report.result == MEISO_RUNTIME_BLOB_RESULT_BUSY);
        CHECK(meiso_blob_store_staging_buffer(store, &still_staging));
        CHECK(still_staging.data == staging.data &&
              still_staging.capacity == staging.capacity &&
              memcmp(still_staging.data, BLOB_BYTES,
                     sizeof(BLOB_BYTES)) == 0);

        CHECK(meiso_runtime_send_blob_result(
            pair.edge_runtime,
            &commit_event.blob_transfer,
            MEISO_RUNTIME_BLOB_COMMIT
        ) == MEISO_RUNTIME_OK);
        CHECK(receive_outbound(
            &pair,
            pair.edge_session,
            pair.host_session,
            &message,
            NULL,
            0U
        ));
        CHECK(meiso_runtime_receive(
            pair.host_runtime, &message, &result_event
        ) == MEISO_RUNTIME_OK);
        CHECK(result_event.blob_result == MEISO_RUNTIME_BLOB_RESULT_BUSY);
        CHECK(meiso_blob_store_commit_verified(
            store,
            sizeof(BLOB_BYTES),
            variant == 0U ? meiso_runtime_blob_sha256_verify : reject_blob,
            NULL
        ) == (variant == 0U ?
            MEISO_BLOB_COMMIT_PUBLISHED :
            MEISO_BLOB_COMMIT_INTEGRITY_FAILED));
        CHECK(meiso_blob_store_abort_staging(
            store, MEISO_BLOB_ABORT_CALLER_REQUEST
        ) == (variant == 0U ?
            MEISO_BLOB_ABORT_NO_STAGING : MEISO_BLOB_ABORTED));
    }

    {
        RuntimePair pair;
        BlobArena blob_arena;
        MeisoBlobStore *store = NULL;
        MeisoRuntimeBlobTransferRef transfer = {0};
        MeisoRuntimeEvent commit_event = {0};
        MeisoRuntimeBlobPublishReport report = {0};
        MeisoBlobStats stats = {0};
        MeisoMutableBuffer staging = {0};

        CHECK(init_test_blob_store(&blob_arena, &store));
        CHECK(receive_small_blob_to_commit(
            &pair, &transfer, &commit_event
        ));
        CHECK(meiso_runtime_publish_blob_receive(
            pair.edge_runtime,
            &commit_event.blob_transfer,
            store,
            reject_blob,
            NULL,
            &report
        ) == MEISO_RUNTIME_OK);
        CHECK(report.stage_status == MEISO_BLOB_STAGE_STARTED &&
              report.commit_status == MEISO_BLOB_COMMIT_INTEGRITY_FAILED &&
              report.forced_abort_attempted &&
              report.forced_abort_status == MEISO_BLOB_ABORTED &&
              report.result == MEISO_RUNTIME_BLOB_RESULT_INTEGRITY_FAILED);
        CHECK(!meiso_blob_store_staging_buffer(store, &staging));
        CHECK(meiso_blob_store_count(store) == 0U);
        CHECK(meiso_blob_store_stats(store, &stats));
        CHECK(stats.integrity_failures == 1U &&
              stats.transfer_aborts == 1U);
    }
    return true;
}

/* An Edge short of memory refuses a complete image with CAPACITY (planes-v12 §7): nothing is staged or
 * published, the Host gets CAPACITY with every byte accepted, and the transfer slot is free again. */
static bool test_edge_refuses_blob_for_capacity(void)
{
    RuntimePair pair;
    BlobArena blob_arena;
    MeisoBlobStore *store = NULL;
    MeisoBlobStats stats = {0};
    MeisoRuntimeBlobTransferRef transfer = {0};
    MeisoRuntimeBlobTransferRef other = {0};
    MeisoRuntimeEvent commit_event = {0};
    MeisoRuntimeEvent event = {0};
    MeisoRuntimeBlobPublishReport report = {0};
    MeisoRuntimeBlobSnapshot snapshot;
    MeisoSessionRuntimeMessage message = {0};
    MeisoContentId content_id = blob_content_id();

    CHECK(init_test_blob_store(&blob_arena, &store));
    CHECK(receive_small_blob_to_commit(&pair, &transfer, &commit_event));
    CHECK(meiso_runtime_refuse_blob_receive(
        pair.host_runtime, &commit_event.blob_transfer
    ) == MEISO_RUNTIME_WRONG_ROLE);
    CHECK(meiso_runtime_refuse_blob_receive(pair.edge_runtime, NULL) == MEISO_RUNTIME_INVALID_ARGUMENT);
    other = commit_event.blob_transfer;
    ++other.transfer_id;
    CHECK(meiso_runtime_refuse_blob_receive(pair.edge_runtime, &other) == MEISO_RUNTIME_CONFLICT);

    CHECK(meiso_runtime_refuse_blob_receive(
        pair.edge_runtime, &commit_event.blob_transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &snapshot));
    CHECK(!snapshot.publish_pending && snapshot.result_pending &&
          snapshot.pending_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          snapshot.pending_result == MEISO_RUNTIME_BLOB_RESULT_CAPACITY);
    /* refused once: neither a second refusal nor a publication */
    CHECK(meiso_runtime_refuse_blob_receive(
        pair.edge_runtime, &commit_event.blob_transfer
    ) == MEISO_RUNTIME_BLOB_NOT_ACTIVE);
    CHECK(meiso_runtime_publish_blob_receive(
        pair.edge_runtime,
        &commit_event.blob_transfer,
        store,
        accept_blob,
        NULL,
        &report
    ) == MEISO_RUNTIME_BLOB_NOT_ACTIVE);
    CHECK(meiso_blob_store_count(store) == 0U);
    CHECK(meiso_blob_store_stats(store, &stats));
    CHECK(stats.stages_started == 0U && stats.publications == 0U);

    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime, &commit_event.blob_transfer, MEISO_RUNTIME_BLOB_COMMIT
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(&pair, pair.edge_session, pair.host_session, &message, NULL, 0U));
    CHECK(meiso_runtime_receive(pair.host_runtime, &message, &event) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_RESULT &&
          event.blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          event.blob_result == MEISO_RUNTIME_BLOB_RESULT_CAPACITY &&
          event.blob_next_offset == sizeof(BLOB_BYTES));
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &snapshot));
    CHECK(!snapshot.transfer_active && !snapshot.result_pending);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &snapshot));
    CHECK(!snapshot.transfer_active);
    /* the slot is free: the same image may be offered again */
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime, blob_route_key(), &content_id, sizeof(BLOB_BYTES), &other
    ) == MEISO_RUNTIME_OK);
    return true;
}

static bool test_route_and_identity_rejections(void)
{
    RuntimePair pair;
    MeisoRuntimeRouteKey route;
    MeisoContentId content_id = blob_content_id();
    MeisoRuntimeBlobTransferRef output;
    MeisoSessionSnapshot session_snapshot = {0};
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};

    CHECK(init_pair(&pair));
    CHECK(activate_blob_catalog(&pair));
    route = blob_route_key();
    route.catalog_generation++;
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime, route, &content_id, 4U, &output
    ) == MEISO_RUNTIME_STALE_CATALOG);
    route = blob_route_key();
    route.endpoint_id.low++;
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime, route, &content_id, 4U, &output
    ) == MEISO_RUNTIME_UNKNOWN_ENDPOINT);
    route = blob_route_key();
    route.endpoint_incarnation++;
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime, route, &content_id, 4U, &output
    ) == MEISO_RUNTIME_STALE_ENDPOINT);
    route = blob_route_key();
    route.resource_id.low++;
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime, route, &content_id, 4U, &output
    ) == MEISO_RUNTIME_UNKNOWN_RESOURCE);
    route = blob_route_key();
    route.facet = MEISO_RUNTIME_FACET_DESIRED;
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime, route, &content_id, 4U, &output
    ) == MEISO_RUNTIME_FACET_MISMATCH);
    CHECK(meiso_runtime_send_blob_begin(
        pair.edge_runtime, blob_route_key(), &content_id, 4U, &output
    ) == MEISO_RUNTIME_WRONG_ROLE);
    content_id.digest_size = MEISO_SHA256_DIGEST_SIZE - 1U;
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime, blob_route_key(), &content_id, 4U, &output
    ) == MEISO_RUNTIME_BLOB_INVALID);
    content_id = blob_content_id();
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        blob_route_key(),
        &content_id,
        UINT64_C(262145),
        &output
    ) == MEISO_RUNTIME_PAYLOAD_CAPACITY);
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        blob_route_key(),
        &content_id,
        4U,
        (MeisoRuntimeBlobTransferRef *)(void *)pair.host_runtime
    ) == MEISO_RUNTIME_BLOB_INVALID);

    CHECK(meiso_session_snapshot(pair.edge_session, &session_snapshot));
    message.session_id = session_snapshot.session_id;
    message.session_id.host_nonce.low++;
    message.payload = (MeisoByteView){
        GOLDEN_BLOB_BEGIN, sizeof(GOLDEN_BLOB_BEGIN)
    };
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_STALE_SESSION);
    return true;
}

static bool test_blob_routes_share_one_transfer_slot(void)
{
    RuntimePair pair;
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor routes[2];
    MeisoRuntimeRouteKey second_route;
    MeisoContentId content_id = blob_content_id();
    MeisoRuntimeBlobTransferRef first = {0};
    MeisoRuntimeBlobTransferRef second = {0};
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};

    CHECK(init_pair(&pair));
    blob_catalog(&endpoint, &routes[0]);
    routes[1] = routes[0];
    routes[1].resource.id = BLOB2_ID;
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 1U, &endpoint, 1U, routes, 2U
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.edge_session, pair.host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_accept_catalog(pair.host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);

    second_route = blob_route_key();
    second_route.resource_id = BLOB2_ID;
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        blob_route_key(),
        &content_id,
        4U,
        &first
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        second_route,
        &content_id,
        4U,
        &second
    ) == MEISO_RUNTIME_BLOB_BUSY);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_blob_abort(
        pair.host_runtime,
        &first,
        MEISO_RUNTIME_BLOB_ABORT_CALLER_REQUEST
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.blob_operation == MEISO_RUNTIME_BLOB_ABORT);
    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime, &event.blob_transfer, MEISO_RUNTIME_BLOB_ABORT
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.edge_session, pair.host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.blob_result == MEISO_RUNTIME_BLOB_RESULT_ABORTED);
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        second_route,
        &content_id,
        4U,
        &second
    ) == MEISO_RUNTIME_OK);
    CHECK(second.transfer_id == 2U &&
          meiso_resource_id_equal(second.route.resource_id, BLOB2_ID));
    return true;
}

static bool test_catalog_commit_clears_blob_transfer(void)
{
    RuntimePair pair;
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor route;
    MeisoContentId content_id = blob_content_id();
    MeisoRuntimeBlobTransferRef transfer = {0};
    MeisoRuntimeBlobSnapshot snapshot = {0};
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};

    CHECK(init_pair(&pair));
    CHECK(activate_blob_catalog(&pair));
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        blob_route_key(),
        &content_id,
        4U,
        &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime, &event.blob_transfer, MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.edge_session, pair.host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_blob_chunk(
        pair.host_runtime,
        &transfer,
        0U,
        (MeisoByteView){BLOB_BYTES, sizeof(BLOB_BYTES)}
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);

    blob_catalog(&endpoint, &route);
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 2U, &endpoint, 1U, &route, 1U
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.edge_session, pair.host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_accept_catalog(pair.host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &snapshot));
    CHECK(!snapshot.transfer_active && !snapshot.result_pending &&
          !snapshot.publish_pending &&
          snapshot.transfer_id_high_water == 1U);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &snapshot));
    CHECK(!snapshot.transfer_active && !snapshot.result_pending &&
          !snapshot.publish_pending);
    CHECK(meiso_runtime_send_blob_commit(
        pair.host_runtime, &transfer
    ) == MEISO_RUNTIME_BLOB_NOT_ACTIVE);
    return true;
}

static bool test_session_loss_clears_runtime_only_staging(void)
{
    RuntimePair pair;
    MeisoContentId content_id = blob_content_id();
    MeisoRuntimeBlobTransferRef transfer = {0};
    MeisoRuntimeBlobSnapshot snapshot = {0};
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};

    CHECK(init_pair(&pair));
    CHECK(activate_blob_catalog(&pair));
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        blob_route_key(),
        &content_id,
        4U,
        &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &snapshot));
    CHECK(snapshot.transfer_active && snapshot.result_pending);
    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime, &event.blob_transfer, MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.edge_session, pair.host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_blob_chunk(
        pair.host_runtime,
        &transfer,
        0U,
        (MeisoByteView){BLOB_BYTES, sizeof(BLOB_BYTES)}
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &snapshot));
    CHECK(snapshot.transfer_active && !snapshot.result_pending &&
          !snapshot.publish_pending &&
          snapshot.next_offset == sizeof(BLOB_BYTES));

    meiso_session_transport_lost(pair.host_session);
    meiso_session_transport_lost(pair.edge_session);
    CHECK(meiso_runtime_refresh_bound_session(pair.host_runtime) ==
          MEISO_RUNTIME_CORE_NOT_READY);
    CHECK(meiso_runtime_refresh_bound_session(pair.edge_runtime) ==
          MEISO_RUNTIME_CORE_NOT_READY);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &snapshot));
    CHECK(!snapshot.transfer_active && !snapshot.publish_pending &&
          !snapshot.result_pending &&
          snapshot.transfer_id_high_water == 1U);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &snapshot));
    CHECK(!snapshot.transfer_active && !snapshot.publish_pending &&
          !snapshot.result_pending &&
          snapshot.transfer_id_high_water == 1U);
    CHECK(meiso_runtime_refresh_bound_session(pair.host_runtime) ==
          MEISO_RUNTIME_STALE_SESSION);
    CHECK(meiso_runtime_refresh_bound_session(pair.edge_runtime) ==
          MEISO_RUNTIME_STALE_SESSION);
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        blob_route_key(),
        &content_id,
        4U,
        &transfer
    ) == MEISO_RUNTIME_STALE_SESSION);
    return true;
}

static bool test_ready_bound_session_identity_mismatch_is_stale(void)
{
    size_t variant;

    for (variant = 0U; variant < 3U; ++variant) {
        RuntimePair pair;
        MeisoSessionConfig bound_config = session_config(
            variant == 2U ? MEISO_SESSION_ROLE_EDGE :
                            MEISO_SESSION_ROLE_HOST
        );
        MeisoSessionConfig peer_config = session_config(
            variant == 2U ? MEISO_SESSION_ROLE_HOST :
                            MEISO_SESSION_ROLE_EDGE
        );
        MeisoSession *bound_session = NULL;
        MeisoSession *peer_session = NULL;
        MeisoSession *new_host;
        MeisoSession *new_edge;
        MeisoContentId content_id = blob_content_id();
        MeisoRuntimeBlobTransferRef transfer = {0};
        MeisoRuntimeBlobSnapshot blob = {0};
        MeisoRuntimeSnapshot runtime = {0};
        MeisoSessionSnapshot old_bound = {0};
        MeisoSessionSnapshot new_bound = {0};

        CHECK(init_pair(&pair));
        CHECK(meiso_session_snapshot(pair.host_session, &old_bound));
        CHECK(activate_blob_catalog(&pair));
        CHECK(meiso_runtime_send_blob_begin(
            pair.host_runtime,
            blob_route_key(),
            &content_id,
            sizeof(BLOB_BYTES),
            &transfer
        ) == MEISO_RUNTIME_OK);
        CHECK(transfer.transfer_id == 1U);

        if (variant == 0U) {
            bound_config.local_nonce.low += 10U;
            peer_config.local_nonce.low += 10U;
        } else if (variant == 1U) {
            bound_config.limits = (MeisoSessionLimits){12288U, 4096U, 32U};
            peer_config.limits = bound_config.limits;
        }
        CHECK(init_session(
            &pair.host_session_arena, &bound_config, &bound_session
        ));
        CHECK(init_session(
            &pair.edge_session_arena, &peer_config, &peer_session
        ));
        CHECK(bound_session == pair.host_session &&
              peer_session == pair.edge_session);
        new_host = bound_config.local_role == MEISO_SESSION_ROLE_HOST ?
            bound_session : peer_session;
        new_edge = bound_config.local_role == MEISO_SESSION_ROLE_EDGE ?
            bound_session : peer_session;
        CHECK(meiso_session_start(new_edge) == MEISO_SESSION_START_STARTED);
        CHECK(meiso_session_start(new_host) == MEISO_SESSION_START_STARTED);
        CHECK(handshake(new_host, new_edge));
        CHECK(handshake(new_edge, new_host));
        CHECK(handshake(new_host, new_edge));
        CHECK(handshake(new_edge, new_host));
        CHECK(meiso_session_snapshot(bound_session, &new_bound) &&
              new_bound.state == MEISO_SESSION_STATE_READY &&
              new_bound.session_id_known);
        if (variant == 0U) {
            CHECK(!meiso_session_id_equal(
                old_bound.session_id, new_bound.session_id
            ) && old_bound.local_role == new_bound.local_role &&
                  memcmp(
                      &old_bound.negotiated_limits,
                      &new_bound.negotiated_limits,
                      sizeof(old_bound.negotiated_limits)
                  ) == 0);
        } else if (variant == 1U) {
            CHECK(meiso_session_id_equal(
                old_bound.session_id, new_bound.session_id
            ) && old_bound.local_role == new_bound.local_role &&
                  memcmp(
                      &old_bound.negotiated_limits,
                      &new_bound.negotiated_limits,
                      sizeof(old_bound.negotiated_limits)
                  ) != 0);
        } else {
            CHECK(meiso_session_id_equal(
                old_bound.session_id, new_bound.session_id
            ) && old_bound.local_role != new_bound.local_role &&
                  memcmp(
                      &old_bound.negotiated_limits,
                      &new_bound.negotiated_limits,
                      sizeof(old_bound.negotiated_limits)
                  ) == 0);
        }

        CHECK(meiso_runtime_refresh_bound_session(pair.host_runtime) ==
              MEISO_RUNTIME_STALE_SESSION);
        CHECK(meiso_runtime_snapshot(pair.host_runtime, &runtime));
        CHECK(!runtime.core_ready && !runtime.active_catalog_present &&
              !runtime.staged_catalog_present &&
              !runtime.catalog_decision_pending);
        CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &blob));
        CHECK(!blob.transfer_active && !blob.publish_pending &&
              !blob.result_pending && blob.transfer_id_high_water == 1U);
        CHECK(meiso_runtime_refresh_bound_session(pair.host_runtime) ==
              MEISO_RUNTIME_STALE_SESSION);
    }
    return true;
}

static bool start_ready_transfer(
    RuntimePair *pair,
    MeisoRuntimeBlobTransferRef *out_transfer
)
{
    MeisoContentId content_id = blob_content_id();
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};

    CHECK(init_pair(pair));
    CHECK(activate_blob_catalog(pair));
    CHECK(meiso_runtime_send_blob_begin(
        pair->host_runtime,
        blob_route_key(),
        &content_id,
        4U,
        out_transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair, pair->host_session, pair->edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair->edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_blob_result(
        pair->edge_runtime, &event.blob_transfer, MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair, pair->edge_session, pair->host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair->host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    return true;
}

static MeisoRuntimeStatus inject_host_result(
    RuntimePair *pair,
    const uint8_t frame[MEISO_RUNTIME_V3_BLOB_RESULT_WIRE_SIZE],
    MeisoRuntimeEvent *out_event
)
{
    MeisoSessionSnapshot session = {0};
    MeisoSessionRuntimeMessage message;

    if (!meiso_session_snapshot(pair->host_session, &session)) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    message.session_id = session.session_id;
    message.payload = (MeisoByteView){
        frame, MEISO_RUNTIME_V3_BLOB_RESULT_WIRE_SIZE
    };
    return meiso_runtime_receive(pair->host_runtime, &message, out_event);
}

static bool test_result_phase_and_offset_validation(void)
{
    RuntimePair pair;
    MeisoContentId content_id = blob_content_id();
    MeisoRuntimeBlobTransferRef transfer = {0};
    MeisoRuntimeBlobSnapshot before = {0};
    MeisoRuntimeBlobSnapshot after = {0};
    MeisoRuntimeEvent event = {0};
    uint8_t changed[MEISO_RUNTIME_V3_BLOB_RESULT_WIRE_SIZE];

    CHECK(init_pair(&pair));
    CHECK(activate_blob_catalog(&pair));
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        blob_route_key(),
        &content_id,
        4U,
        &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_session_mark_outbound_sent(pair.host_session) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &before));
    memmove(changed, GOLDEN_BLOB_RESULT_READY, sizeof(changed));
    changed[77] = MEISO_RUNTIME_BLOB_RESULT_BUSY;
    changed[87] = 1U;
    CHECK(inject_host_result(
        &pair, changed, &event
    ) == MEISO_RUNTIME_MALFORMED);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));
    changed[87] = 0U;
    CHECK(inject_host_result(
        &pair, changed, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN &&
          event.blob_result == MEISO_RUNTIME_BLOB_RESULT_BUSY);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &after));
    CHECK(!after.transfer_active);

    CHECK(start_ready_transfer(&pair, &transfer));
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &before));
    CHECK(inject_host_result(
        &pair, GOLDEN_BLOB_RESULT_READY, &event
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));
    memmove(changed, GOLDEN_BLOB_RESULT_READY, sizeof(changed));
    changed[77] = MEISO_RUNTIME_BLOB_RESULT_BUSY;
    CHECK(inject_host_result(
        &pair, changed, &event
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));

    CHECK(start_ready_transfer(&pair, &transfer));
    CHECK(meiso_runtime_send_blob_chunk(
        pair.host_runtime,
        &transfer,
        0U,
        (MeisoByteView){BLOB_BYTES, sizeof(BLOB_BYTES)}
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_session_mark_outbound_sent(pair.host_session) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_runtime_send_blob_commit(
        pair.host_runtime, &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_session_mark_outbound_sent(pair.host_session) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &before));

    memmove(changed, GOLDEN_BLOB_RESULT_READY, sizeof(changed));
    changed[77] = MEISO_RUNTIME_BLOB_RESULT_BUSY;
    CHECK(inject_host_result(
        &pair, changed, &event
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));

    memmove(changed, GOLDEN_BLOB_RESULT_PUBLISHED, sizeof(changed));
    memset(changed + 80U, 0, 8U);
    CHECK(inject_host_result(
        &pair, changed, &event
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));

    memmove(changed, GOLDEN_BLOB_RESULT_PUBLISHED, sizeof(changed));
    changed[77] = MEISO_RUNTIME_BLOB_RESULT_BUSY;
    memset(changed + 80U, 0, 8U);
    changed[87] = 1U;
    CHECK(inject_host_result(
        &pair, changed, &event
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));

    changed[76] = MEISO_RUNTIME_BLOB_CHUNK;
    changed[77] = MEISO_RUNTIME_BLOB_RESULT_DISCONTINUITY;
    CHECK(inject_host_result(
        &pair, changed, &event
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));
    memset(changed + 80U, 0, 8U);
    changed[87] = 4U;
    CHECK(inject_host_result(
        &pair, changed, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.blob_operation == MEISO_RUNTIME_BLOB_CHUNK &&
          event.blob_result == MEISO_RUNTIME_BLOB_RESULT_DISCONTINUITY);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &after));
    CHECK(!after.transfer_active);

    CHECK(start_ready_transfer(&pair, &transfer));
    CHECK(meiso_runtime_send_blob_chunk(
        pair.host_runtime,
        &transfer,
        0U,
        (MeisoByteView){BLOB_BYTES, sizeof(BLOB_BYTES)}
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_session_mark_outbound_sent(pair.host_session) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_runtime_send_blob_abort(
        pair.host_runtime,
        &transfer,
        MEISO_RUNTIME_BLOB_ABORT_CALLER_REQUEST
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_session_mark_outbound_sent(pair.host_session) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &before));
    memmove(changed, GOLDEN_BLOB_RESULT_PUBLISHED, sizeof(changed));
    changed[76] = MEISO_RUNTIME_BLOB_ABORT;
    changed[77] = MEISO_RUNTIME_BLOB_RESULT_ABORTED;
    memset(changed + 80U, 0, 8U);
    changed[87] = 1U;
    CHECK(inject_host_result(
        &pair, changed, &event
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(meiso_runtime_blob_snapshot(pair.host_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));
    changed[87] = 4U;
    CHECK(inject_host_result(
        &pair, changed, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.blob_operation == MEISO_RUNTIME_BLOB_ABORT &&
          event.blob_result == MEISO_RUNTIME_BLOB_RESULT_ABORTED);
    return true;
}

static bool test_chunk_gap_duplicate_and_overlap(void)
{
    static const uint8_t SHORT_FINAL_SHA[32] = {
        0x1eU, 0x97U, 0x3dU, 0x02U, 0x9dU, 0xf2U, 0xb2U, 0xc6U,
        0x6cU, 0xb4U, 0x2aU, 0x94U, 0x2cU, 0x5eU, 0xdbU, 0x45U,
        0x96U, 0x6fU, 0x02U, 0xabU, 0xafU, 0xf2U, 0x9fU, 0xe9U,
        0x94U, 0x10U, 0xe4U, 0x4dU, 0x27U, 0x1dU, 0x0eU, 0xfcU
    };
    static uint8_t content[4097];
    size_t variant;
    size_t index;

    for (index = 0U; index < sizeof(content); ++index) {
        content[index] = (uint8_t)index;
    }
    for (variant = 0U; variant < 3U; ++variant) {
        RuntimePair pair;
        MeisoContentId content_id = sha256_content_id(SHORT_FINAL_SHA);
        MeisoRuntimeBlobTransferRef transfer = {0};
        MeisoRuntimeBlobSnapshot snapshot = {0};
        MeisoSessionRuntimeMessage message = {0};
        MeisoSessionSnapshot session = {0};
        MeisoRuntimeEvent event = {0};
        uint8_t wire[MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE +
                     MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE];

        CHECK(init_pair(&pair));
        CHECK(activate_blob_catalog(&pair));
        CHECK(meiso_runtime_send_blob_begin(
            pair.host_runtime,
            blob_route_key(),
            &content_id,
            sizeof(content),
            &transfer
        ) == MEISO_RUNTIME_OK);
        CHECK(receive_outbound(
            &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
        ));
        CHECK(meiso_runtime_receive(
            pair.edge_runtime, &message, &event
        ) == MEISO_RUNTIME_OK);
        CHECK(meiso_runtime_send_blob_result(
            pair.edge_runtime,
            &event.blob_transfer,
            MEISO_RUNTIME_BLOB_BEGIN
        ) == MEISO_RUNTIME_OK);
        CHECK(receive_outbound(
            &pair, pair.edge_session, pair.host_session, &message, NULL, 0U
        ));
        CHECK(meiso_runtime_receive(
            pair.host_runtime, &message, &event
        ) == MEISO_RUNTIME_OK);

        if (variant == 0U) {
            memmove(wire, GOLDEN_BLOB_CHUNK, sizeof(GOLDEN_BLOB_CHUNK));
            wire[83] = 1U;
            message.payload = (MeisoByteView){
                wire, sizeof(GOLDEN_BLOB_CHUNK)
            };
        } else {
            CHECK(meiso_runtime_send_blob_chunk(
                pair.host_runtime,
                &transfer,
                0U,
                (MeisoByteView){
                    content, MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE
                }
            ) == MEISO_RUNTIME_OK);
            CHECK(receive_outbound(
                &pair,
                pair.host_session,
                pair.edge_session,
                &message,
                NULL,
                0U
            ));
            CHECK(message.payload.size == sizeof(wire));
            memmove(wire, message.payload.data, sizeof(wire));
            CHECK(meiso_runtime_receive(
                pair.edge_runtime, &message, &event
            ) == MEISO_RUNTIME_OK);
            if (variant == 2U) {
                memset(wire + 76U, 0, 8U);
                wire[82] = 0x0fU;
                wire[83] = 0xffU;
            }
            message.payload = (MeisoByteView){wire, sizeof(wire)};
        }
        CHECK(meiso_session_snapshot(pair.edge_session, &session));
        message.session_id = session.session_id;
        CHECK(meiso_runtime_receive(
            pair.edge_runtime, &message, &event
        ) == MEISO_RUNTIME_OK);
        CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
              event.blob_operation == MEISO_RUNTIME_BLOB_CHUNK);
        CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &snapshot));
        CHECK(snapshot.result_pending &&
              snapshot.pending_operation == MEISO_RUNTIME_BLOB_CHUNK &&
              snapshot.pending_result ==
                  MEISO_RUNTIME_BLOB_RESULT_DISCONTINUITY &&
              snapshot.next_offset == (variant == 0U ? 0U : 4096U));
        CHECK(meiso_runtime_send_blob_result(
            pair.edge_runtime,
            &event.blob_transfer,
            MEISO_RUNTIME_BLOB_CHUNK
        ) == MEISO_RUNTIME_OK);
        CHECK(receive_outbound(
            &pair, pair.edge_session, pair.host_session, &message, NULL, 0U
        ));
        if (variant == 0U) {
            MeisoRuntimeBlobSnapshot host_before = {0};
            MeisoRuntimeBlobSnapshot host_after = {0};

            CHECK(meiso_runtime_blob_snapshot(
                pair.host_runtime, &host_before
            ));
            CHECK(meiso_runtime_receive(
                pair.host_runtime, &message, &event
            ) == MEISO_RUNTIME_CONFLICT);
            CHECK(meiso_runtime_blob_snapshot(
                pair.host_runtime, &host_after
            ));
            CHECK(blob_snapshot_equal(&host_before, &host_after));
        } else {
            CHECK(meiso_runtime_receive(
                pair.host_runtime, &message, &event
            ) == MEISO_RUNTIME_OK);
            CHECK(event.blob_result ==
                  MEISO_RUNTIME_BLOB_RESULT_DISCONTINUITY &&
                  event.blob_next_offset == 4096U);
        }
    }
    return true;
}

static MeisoRuntimeRouteKey composition_blob_route_key(void)
{
    const MeisoResourceDescriptor *resource =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
        );
    MeisoRuntimeRouteKey key = {
        1U, ENDPOINT, ENDPOINT_INCARNATION, {0U, 0U},
        MEISO_RUNTIME_FACET_IMMUTABLE
    };

    if (resource != NULL) {
        key.resource_id = resource->id;
    }
    return key;
}

static bool activate_composition_blob_catalog(RuntimePair *pair)
{
    const MeisoResourceDescriptor *resource =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
        );
    static const uint8_t capability[] = {0xa1U};
    MeisoRuntimeEndpointDescriptor endpoint = {
        ENDPOINT, ENDPOINT_INCARNATION, CAPABILITY_SCHEMA,
        {capability, sizeof(capability)}
    };
    MeisoRuntimeRouteDescriptor route;
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};

    CHECK(resource != NULL);
    route = (MeisoRuntimeRouteDescriptor){
        ENDPOINT, ENDPOINT_INCARNATION, *resource,
        MEISO_RUNTIME_FACET_IMMUTABLE
    };
    CHECK(meiso_runtime_offer_catalog(
        pair->edge_runtime, 1U, &endpoint, 1U, &route, 1U
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair, pair->edge_session, pair->host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair->host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_accept_catalog(pair->host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair, pair->host_session, pair->edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair->edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    return true;
}

static bool transport_composition_blob(
    RuntimePair *pair,
    MeisoBlobStore *store,
    const MeisoContentId *content_id,
    MeisoByteView content,
    MeisoRuntimeBlobPublishReport *out_report,
    MeisoRuntimeBlobResult expected_result
)
{
    MeisoRuntimeBlobTransferRef transfer = {0};
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};
    size_t offset = 0U;

    CHECK(activate_composition_blob_catalog(pair));
    CHECK(meiso_runtime_send_blob_begin(
        pair->host_runtime,
        composition_blob_route_key(),
        content_id,
        content.size,
        &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair, pair->host_session, pair->edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair->edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_blob_result(
        pair->edge_runtime, &event.blob_transfer, MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair, pair->edge_session, pair->host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair->host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    while (offset < content.size) {
        const size_t remaining = content.size - offset;
        const size_t chunk_size =
            remaining < MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE ?
                remaining : MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE;

        CHECK(meiso_runtime_send_blob_chunk(
            pair->host_runtime,
            &transfer,
            offset,
            (MeisoByteView){content.data + offset, chunk_size}
        ) == MEISO_RUNTIME_OK);
        CHECK(receive_outbound(
            pair,
            pair->host_session,
            pair->edge_session,
            &message,
            NULL,
            0U
        ));
        CHECK(meiso_runtime_receive(
            pair->edge_runtime, &message, &event
        ) == MEISO_RUNTIME_OK);
        offset += chunk_size;
    }
    CHECK(meiso_runtime_send_blob_commit(
        pair->host_runtime, &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair, pair->host_session, pair->edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair->edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_publish_blob_receive(
        pair->edge_runtime,
        &event.blob_transfer,
        store,
        meiso_composition_scene_mesh_blob_verify,
        NULL,
        out_report
    ) == MEISO_RUNTIME_OK);
    CHECK(out_report->result == expected_result);
    CHECK(meiso_runtime_send_blob_result(
        pair->edge_runtime, &event.blob_transfer, MEISO_RUNTIME_BLOB_COMMIT
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair, pair->edge_session, pair->host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair->host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.blob_result == expected_result);
    return true;
}

static MeisoCompositionSceneDesired scene_desired(MeisoContentId mesh_id)
{
    MeisoCompositionSceneDesired desired;
    MeisoCompositionSceneObject *object;

    memset(&desired, 0, sizeof(desired));
    desired.camera.space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    desired.camera.space_epoch = 1U;
    desired.camera.source_time = (MeisoTimePoint){1U, 1U};
    desired.camera.position[2] = -3.0F;
    desired.camera.orientation_xyzw[3] = 1.0F;
    desired.camera.projection =
        MEISO_COMPOSITION_SCENE_PROJECTION_PERSPECTIVE;
    desired.camera.vertical_fov_degrees = 60.0F;
    desired.camera.near_z = 0.1F;
    desired.camera.far_z = 100.0F;
    desired.camera.viewport_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    desired.camera.viewport_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    desired.object_count = 1U;
    object = &desired.objects[0];
    object->id = (MeisoCompositionSceneObjectId){1U, 1U};
    object->mesh_content_id = mesh_id;
    object->space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    object->space_epoch = 1U;
    object->transform.orientation_xyzw[3] = 1.0F;
    object->transform.scale[0] = 1.0F;
    object->transform.scale[1] = 1.0F;
    object->transform.scale[2] = 1.0F;
    object->flags = MEISO_COMPOSITION_SCENE_OBJECT_VISIBLE;
    return desired;
}

static bool test_result_requires_explicit_scene_visibility(void)
{
    static LargeBlobArena blob_arena;
    static RuntimeArena scene_store_arena;
    RuntimePair pair;
    const MeisoResourceDescriptor *resource =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
        );
    const MeisoBlobConfig blob_config = {
        MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS
    };
    MeisoBlobStore *store = NULL;
    MeisoCompositionSceneStore *scene_store = NULL;
    MeisoRuntimeBlobPublishReport report = {0};
    MeisoCompositionSceneDesired desired;
    MeisoContentId cube_id;
    uint8_t cube_wire[512];
    size_t cube_size = 0U;
    size_t required;

    CHECK(resource != NULL);
    required = meiso_blob_store_memory_required(resource, &blob_config);
    CHECK(required != 0U && required <= sizeof(blob_arena.bytes));
    CHECK(meiso_blob_store_init(
        blob_arena.bytes, required, resource, &blob_config, &store
    ) == MEISO_BLOB_INIT_OK);
    required = meiso_composition_scene_store_memory_required();
    CHECK(required != 0U && required <= sizeof(scene_store_arena.bytes));
    CHECK(meiso_composition_scene_store_init(
        scene_store_arena.bytes, required, store, &scene_store
    ) == MEISO_COMPOSITION_SCENE_STORE_INIT_OK);
    CHECK(meiso_composition_scene_store_begin_owner(scene_store, 31U) ==
          MEISO_COMPOSITION_SCENE_OWNER_STARTED);
    CHECK(meiso_composition_scene_mesh_encode(
        meiso_composition_scene_canonical_cube(),
        (MeisoMutableBuffer){cube_wire, sizeof(cube_wire)},
        &cube_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(meiso_composition_scene_mesh_content_id(
        (MeisoByteView){cube_wire, cube_size}, &cube_id
    ));
    desired = scene_desired(cube_id);

    CHECK(init_pair(&pair));
    CHECK(transport_composition_blob(
        &pair,
        store,
        &cube_id,
        (MeisoByteView){cube_wire, cube_size},
        &report,
        MEISO_RUNTIME_BLOB_RESULT_PUBLISHED
    ));
    CHECK(report.commit_status == MEISO_BLOB_COMMIT_PUBLISHED);
    CHECK(meiso_composition_scene_store_prepare(
        scene_store, 31U, 1U, 1U, &desired
    ) == MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB);
    CHECK(meiso_composition_scene_store_mark_blob_visible(
        scene_store, 31U, &cube_id, cube_size
    ) == MEISO_COMPOSITION_SCENE_BLOB_VISIBLE);
    CHECK(meiso_composition_scene_store_prepare(
        scene_store, 31U, 1U, 1U, &desired
    ) == MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);

    CHECK(meiso_composition_scene_store_reset_control_scope(
        scene_store, 31U
    ) == MEISO_COMPOSITION_SCENE_OWNER_SCOPE_RESET);
    CHECK(meiso_composition_scene_store_prepare(
        scene_store, 31U, 1U, 1U, &desired
    ) == MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB);
    CHECK(init_pair_with_nonce(&pair, 0x63U, 0x73U));
    CHECK(transport_composition_blob(
        &pair,
        store,
        &cube_id,
        (MeisoByteView){cube_wire, cube_size},
        &report,
        MEISO_RUNTIME_BLOB_RESULT_DUPLICATE
    ));
    CHECK(report.commit_status == MEISO_BLOB_COMMIT_DUPLICATE);
    CHECK(meiso_composition_scene_store_prepare(
        scene_store, 31U, 1U, 1U, &desired
    ) == MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB);
    CHECK(meiso_composition_scene_store_mark_blob_visible(
        scene_store, 31U, &cube_id, cube_size
    ) == MEISO_COMPOSITION_SCENE_BLOB_VISIBLE);
    CHECK(meiso_composition_scene_store_prepare(
        scene_store, 31U, 1U, 1U, &desired
    ) == MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    return true;
}

static bool calibrate_edge_blob_scratch_offset(size_t *out_offset)
{
    RuntimePair calibration;
    MeisoRuntimeBlobTransferRef transfer = {0};
    MeisoRuntimeEvent commit_event = {0};
    MeisoRuntimeV3Config config;
    size_t runtime_size;
    size_t offset;
    uintptr_t arena_start;
    uintptr_t payload_start;

    CHECK(out_offset != NULL && receive_small_blob_to_commit(
        &calibration, &transfer, &commit_event
    ));
    CHECK(commit_event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          commit_event.blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          commit_event.blob_payload.size == sizeof(BLOB_BYTES) &&
          memcmp(
              commit_event.blob_payload.data,
              BLOB_BYTES,
              sizeof(BLOB_BYTES)
          ) == 0);
    config = runtime_config(calibration.edge_session);
    runtime_size = meiso_runtime_v3_memory_required(&config);
    arena_start = (uintptr_t)(const void *)
        calibration.edge_runtime_arena.bytes;
    payload_start = (uintptr_t)(const void *)
        commit_event.blob_payload.data;
    CHECK(runtime_size != 0U &&
          runtime_size <= sizeof(calibration.edge_runtime_arena.bytes) &&
          runtime_size <= UINTPTR_MAX - arena_start &&
          payload_start >= arena_start &&
          payload_start <= arena_start + runtime_size);
    offset = (size_t)(payload_start - arena_start);
    CHECK(offset <= runtime_size &&
          config.max_blob_transfer_bytes <= runtime_size - offset);
    *out_offset = offset;
    return true;
}

static bool test_v3_state_stream_and_blob_coexist(void)
{
    RuntimePair pair;
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor routes[4];
    MeisoRuntimeActualState actual;
    MeisoRuntimeDesiredState desired = {
        true, {(const uint8_t *)"\x01\x02", 2U}
    };
    MeisoStateOffer offer;
    MeisoStreamSample sample;
    MeisoByteView payload;
    MeisoContentId content_id = blob_content_id();
    MeisoRuntimeBlobTransferRef transfer = {0};
    MeisoRuntimeBlobTransferRef edge_transfer = {0};
    MeisoRuntimeBlobSnapshot blob_snapshot = {0};
    MeisoRuntimeSnapshot runtime_snapshot = {0};
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};
    MeisoRuntimeV3Config edge_config;
    size_t edge_runtime_size;
    size_t index;
    size_t scratch_offset;
    uint8_t *scratch;
    uint8_t desired_bytes[16];
    uint8_t actual_bytes[128];
    static const uint8_t sample_bytes[] = {0x31U, 0x32U, 0x33U};

    /*
     * CHUNK exposes a Session message borrow, not the reassembly address.
     * Calibrate the opaque scratch offset from COMMIT's Runtime-scratch borrow
     * in an independent fixture with the exact same Edge role/config/ABI.
     */
    CHECK(calibrate_edge_blob_scratch_offset(&scratch_offset));
    CHECK(init_pair(&pair));
    blob_catalog(&endpoint, &routes[0]);
    routes[1] = (MeisoRuntimeRouteDescriptor){
        ENDPOINT,
        ENDPOINT_INCARNATION,
        {DESIRED_ID, MEISO_RESOURCE_KIND_STATE, DESIRED_SCHEMA,
         MEISO_OWNER_ROLE_HOST, 64U},
        MEISO_RUNTIME_FACET_DESIRED
    };
    routes[2] = (MeisoRuntimeRouteDescriptor){
        ENDPOINT,
        ENDPOINT_INCARNATION,
        {ACTUAL_ID, MEISO_RESOURCE_KIND_STATE, ACTUAL_SCHEMA,
         MEISO_OWNER_ROLE_EDGE, 128U},
        MEISO_RUNTIME_FACET_ACTUAL
    };
    routes[3] = (MeisoRuntimeRouteDescriptor){
        ENDPOINT,
        ENDPOINT_INCARNATION,
        {DATA_ID, MEISO_RESOURCE_KIND_STREAM, DATA_SCHEMA,
         MEISO_OWNER_ROLE_EDGE, 64U},
        MEISO_RUNTIME_FACET_DATA
    };
    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 1U, &endpoint, 1U, routes, 4U
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.edge_session, pair.host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_accept_catalog(pair.host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);

    CHECK(meiso_runtime_desired_state_encode(
        &desired,
        (MeisoMutableBuffer){desired_bytes, sizeof(desired_bytes)},
        &payload
    ));
    offer.generation.value = 1U;
    offer.state.owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 31U};
    offer.state.version = 1U;
    offer.state.payload = payload;
    CHECK(meiso_runtime_send_state_offer(
        pair.host_runtime,
        (MeisoRuntimeRouteKey){
            1U, ENDPOINT, ENDPOINT_INCARNATION, DESIRED_ID,
            MEISO_RUNTIME_FACET_DESIRED
        },
        &offer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_commit_state_receive(
        pair.edge_runtime,
        event.route,
        &event.state_offer,
        MEISO_STATE_RECEIVE_NEW
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_state_accept(
        pair.edge_runtime,
        event.route,
        event.state_offer.generation,
        event.state_offer.state.owner,
        event.state_offer.state.version
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.edge_session, pair.host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);

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
    actual.production_epoch = 1U;
    CHECK(meiso_runtime_actual_state_encode(
        &actual,
        (MeisoMutableBuffer){actual_bytes, sizeof(actual_bytes)},
        &payload
    ));
    offer.generation.value = 2U;
    offer.state.owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 41U};
    offer.state.version = 1U;
    offer.state.payload = payload;
    CHECK(meiso_runtime_send_state_offer(
        pair.edge_runtime,
        (MeisoRuntimeRouteKey){
            1U, ENDPOINT, ENDPOINT_INCARNATION, ACTUAL_ID,
            MEISO_RUNTIME_FACET_ACTUAL
        },
        &offer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.edge_session, pair.host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_commit_state_receive(
        pair.host_runtime,
        event.route,
        &event.state_offer,
        MEISO_STATE_RECEIVE_NEW
    ) == MEISO_RUNTIME_OK);

    memset(&sample, 0, sizeof(sample));
    sample.key.producer = (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 41U};
    sample.key.epoch = 1U;
    sample.key.sequence = 1U;
    sample.source_time = (MeisoTimePoint){1U, 1U};
    sample.payload = (MeisoByteView){sample_bytes, sizeof(sample_bytes)};
    CHECK(meiso_runtime_send_stream_sample(
        pair.edge_runtime,
        (MeisoRuntimeRouteKey){
            1U, ENDPOINT, ENDPOINT_INCARNATION, DATA_ID,
            MEISO_RUNTIME_FACET_DATA
        },
        1U,
        &sample
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.edge_session, pair.host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          event.stream_sample.payload.size == sizeof(sample_bytes) &&
          memcmp(event.stream_sample.payload.data,
                 sample_bytes, sizeof(sample_bytes)) == 0);

    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        blob_route_key(),
        &content_id,
        sizeof(BLOB_BYTES),
        &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer.transfer_id == 1U);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN);
    edge_transfer = event.blob_transfer;
    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime, &edge_transfer, MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.edge_session, pair.host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_blob_chunk(
        pair.host_runtime,
        &transfer,
        0U,
        (MeisoByteView){BLOB_BYTES, sizeof(BLOB_BYTES)}
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          event.blob_operation == MEISO_RUNTIME_BLOB_CHUNK &&
          event.blob_payload.size == sizeof(BLOB_BYTES) &&
          memcmp(
              event.blob_payload.data, BLOB_BYTES, sizeof(BLOB_BYTES)
          ) == 0);
    edge_config = runtime_config(pair.edge_session);
    edge_runtime_size = meiso_runtime_v3_memory_required(&edge_config);
    CHECK(edge_runtime_size != 0U &&
          edge_runtime_size <= sizeof(pair.edge_runtime_arena.bytes) &&
          scratch_offset <= edge_runtime_size &&
          edge_config.max_blob_transfer_bytes <=
              edge_runtime_size - scratch_offset);

    CHECK(meiso_runtime_desired_state_encode(
        &desired,
        (MeisoMutableBuffer){desired_bytes, sizeof(desired_bytes)},
        &payload
    ));
    offer.generation.value = 3U;
    offer.state.owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 31U};
    offer.state.version = 2U;
    offer.state.payload = payload;
    CHECK(meiso_runtime_send_state_offer(
        pair.host_runtime,
        (MeisoRuntimeRouteKey){
            1U, ENDPOINT, ENDPOINT_INCARNATION, DESIRED_ID,
            MEISO_RUNTIME_FACET_DESIRED
        },
        &offer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER &&
          event.state_offer.state.version == 2U);
    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime, &edge_transfer, MEISO_RUNTIME_BLOB_CHUNK
    ) == MEISO_RUNTIME_CONFLICT);

    scratch = pair.edge_runtime_arena.bytes + scratch_offset;
    CHECK(memcmp(scratch, BLOB_BYTES, sizeof(BLOB_BYTES)) == 0);
    meiso_session_transport_lost(pair.edge_session);
    CHECK(meiso_runtime_refresh_bound_session(pair.edge_runtime) ==
          MEISO_RUNTIME_CORE_NOT_READY);
    for (index = 0U; index < edge_config.max_blob_transfer_bytes; ++index) {
        CHECK(scratch[index] == 0U);
    }
    CHECK(meiso_runtime_snapshot(pair.edge_runtime, &runtime_snapshot));
    CHECK(!runtime_snapshot.core_ready &&
          !runtime_snapshot.active_catalog_present &&
          !runtime_snapshot.staged_catalog_present &&
          !runtime_snapshot.catalog_decision_pending &&
          runtime_snapshot.highest_catalog_generation == 1U);
    CHECK(meiso_runtime_blob_snapshot(
        pair.edge_runtime, &blob_snapshot
    ));
    CHECK(!blob_snapshot.transfer_active && !blob_snapshot.publish_pending &&
          !blob_snapshot.result_pending &&
          blob_snapshot.transfer_id_high_water == 1U);
    CHECK(meiso_runtime_refresh_bound_session(pair.edge_runtime) ==
          MEISO_RUNTIME_STALE_SESSION);
    return true;
}

static MeisoRuntimeStatus inject_edge_frame(
    RuntimePair *pair,
    MeisoByteView frame,
    MeisoRuntimeEvent *out_event
)
{
    MeisoSessionSnapshot session = {0};
    MeisoSessionRuntimeMessage message;

    if (!meiso_session_snapshot(pair->edge_session, &session)) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    message.session_id = session.session_id;
    message.payload = frame;
    return meiso_runtime_receive(pair->edge_runtime, &message, out_event);
}

static bool test_begin_replay_and_result_reference_isolation(void)
{
    RuntimePair pair;
    MeisoContentId content_id = blob_content_id();
    MeisoRuntimeBlobTransferRef transfer = {0};
    MeisoRuntimeBlobTransferRef wrong = {0};
    MeisoRuntimeBlobSnapshot before = {0};
    MeisoRuntimeBlobSnapshot after = {0};
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};
    uint8_t changed[sizeof(GOLDEN_BLOB_BEGIN)];

    CHECK(start_ready_transfer(&pair, &transfer));
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &before));
    CHECK(inject_edge_frame(
        &pair,
        (MeisoByteView){GOLDEN_BLOB_BEGIN, sizeof(GOLDEN_BLOB_BEGIN)},
        &event
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));

    memmove(changed, GOLDEN_BLOB_BEGIN, sizeof(changed));
    changed[119] = 5U;
    CHECK(inject_edge_frame(
        &pair, (MeisoByteView){changed, sizeof(changed)}, &event
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));
    memmove(changed, GOLDEN_BLOB_BEGIN, sizeof(changed));
    changed[80] ^= 1U;
    CHECK(inject_edge_frame(
        &pair, (MeisoByteView){changed, sizeof(changed)}, &event
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));
    CHECK(meiso_runtime_send_blob_chunk(
        pair.host_runtime,
        &transfer,
        0U,
        (MeisoByteView){BLOB_BYTES, sizeof(BLOB_BYTES)}
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);

    CHECK(init_pair(&pair));
    CHECK(activate_blob_catalog(&pair));
    CHECK(meiso_runtime_send_blob_begin(
        pair.host_runtime,
        blob_route_key(),
        &content_id,
        4U,
        &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        &pair, pair.host_session, pair.edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair.edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    wrong = event.blob_transfer;
    wrong.total_size++;
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &before));
    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime, &wrong, MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_CONFLICT);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));
    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime,
        &event.blob_transfer,
        MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_OK);
    return true;
}

static bool test_commit_transport_failures_skip_core_publish(void)
{
    size_t variant;

    for (variant = 0U; variant < 2U; ++variant) {
        RuntimePair pair;
        BlobArena blob_arena;
        MeisoBlobStore *store = NULL;
        MeisoRuntimeBlobTransferRef transfer = {0};
        MeisoRuntimeBlobPublishReport report;
        MeisoRuntimeBlobPublishReport sentinel;
        MeisoRuntimeBlobSnapshot before = {0};
        MeisoRuntimeBlobSnapshot after = {0};
        MeisoSessionRuntimeMessage message = {0};
        MeisoRuntimeEvent event = {0};
        MeisoBlobStats stats = {0};
        static const uint8_t wrong_bytes[] = {
            0x01U, 0x02U, 0x03U, 0x04U
        };

        CHECK(init_test_blob_store(&blob_arena, &store));
        CHECK(start_ready_transfer(&pair, &transfer));
        CHECK(meiso_runtime_send_blob_chunk(
            pair.host_runtime,
            &transfer,
            0U,
            (MeisoByteView){
                variant == 0U ? BLOB_BYTES : wrong_bytes,
                sizeof(BLOB_BYTES)
            }
        ) == MEISO_RUNTIME_OK);
        if (variant == 0U) {
            CHECK(meiso_session_mark_outbound_sent(pair.host_session) ==
                  MEISO_SESSION_OUTBOUND_SENT);
        } else {
            CHECK(receive_outbound(
                &pair,
                pair.host_session,
                pair.edge_session,
                &message,
                NULL,
                0U
            ));
            CHECK(meiso_runtime_receive(
                pair.edge_runtime, &message, &event
            ) == MEISO_RUNTIME_OK);
        }
        CHECK(meiso_runtime_send_blob_commit(
            pair.host_runtime, &transfer
        ) == MEISO_RUNTIME_OK);
        CHECK(receive_outbound(
            &pair,
            pair.host_session,
            pair.edge_session,
            &message,
            NULL,
            0U
        ));
        CHECK(meiso_runtime_receive(
            pair.edge_runtime, &message, &event
        ) == MEISO_RUNTIME_OK);
        CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
              event.blob_operation == MEISO_RUNTIME_BLOB_COMMIT);
        CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &before));
        CHECK(before.transfer_active && !before.publish_pending &&
              before.result_pending &&
              before.pending_operation == MEISO_RUNTIME_BLOB_COMMIT &&
              before.pending_result == (variant == 0U ?
                  MEISO_RUNTIME_BLOB_RESULT_LENGTH_MISMATCH :
                  MEISO_RUNTIME_BLOB_RESULT_INTEGRITY_FAILED));
        memset(&report, 0xa5, sizeof(report));
        sentinel = report;
        CHECK(meiso_runtime_publish_blob_receive(
            pair.edge_runtime,
            &event.blob_transfer,
            store,
            meiso_runtime_blob_sha256_verify,
            NULL,
            &report
        ) == MEISO_RUNTIME_BLOB_NOT_ACTIVE);
        CHECK(memcmp(&report, &sentinel, sizeof(report)) == 0);
        CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &after));
        CHECK(blob_snapshot_equal(&before, &after));
        CHECK(meiso_blob_store_stats(store, &stats));
        CHECK(meiso_blob_store_count(store) == 0U &&
              stats.stages_started == 0U);
        CHECK(meiso_runtime_send_blob_result(
            pair.edge_runtime,
            &event.blob_transfer,
            MEISO_RUNTIME_BLOB_COMMIT
        ) == MEISO_RUNTIME_OK);
        CHECK(receive_outbound(
            &pair,
            pair.edge_session,
            pair.host_session,
            &message,
            NULL,
            0U
        ));
        CHECK(meiso_runtime_receive(
            pair.host_runtime, &message, &event
        ) == MEISO_RUNTIME_OK);
        CHECK(event.blob_result == (variant == 0U ?
                  MEISO_RUNTIME_BLOB_RESULT_LENGTH_MISMATCH :
                  MEISO_RUNTIME_BLOB_RESULT_INTEGRITY_FAILED));
    }
    return true;
}

typedef struct {
    MeisoRuntime *runtime;
    const MeisoRuntimeBlobTransferRef *transfer;
    uint8_t *refresh_before;
    size_t runtime_size;
    MeisoRuntimeStatus reentry_status;
    MeisoRuntimeStatus refresh_status;
    bool refresh_untouched;
    bool query_status;
    bool query_untouched;
    size_t calls;
    uint8_t observed[sizeof(BLOB_BYTES)];
} RecordingVerifierContext;

static bool recording_reentrant_verifier(
    const MeisoContentId *expected_content_id,
    MeisoByteView content,
    void *context
)
{
    RecordingVerifierContext *recording = context;
    MeisoRuntimeBlobSnapshot snapshot;
    MeisoRuntimeBlobSnapshot sentinel;

    if (recording == NULL || recording->runtime == NULL ||
        recording->transfer == NULL || recording->refresh_before == NULL ||
        recording->runtime_size == 0U ||
        content.size != sizeof(BLOB_BYTES) ||
        !meiso_runtime_blob_sha256_verify(
            expected_content_id, content, NULL
        )) {
        return false;
    }
    recording->calls++;
    memmove(recording->observed, content.data, content.size);
    memmove(
        recording->refresh_before, recording->runtime,
        recording->runtime_size
    );
    recording->refresh_status = meiso_runtime_refresh_bound_session(
        recording->runtime
    );
    recording->refresh_untouched = memcmp(
        recording->refresh_before, recording->runtime,
        recording->runtime_size
    ) == 0;
    recording->reentry_status = meiso_runtime_send_blob_result(
        recording->runtime,
        recording->transfer,
        MEISO_RUNTIME_BLOB_COMMIT
    );
    memset(&snapshot, 0xa5, sizeof(snapshot));
    sentinel = snapshot;
    recording->query_status = meiso_runtime_blob_snapshot(
        recording->runtime, &snapshot
    );
    recording->query_untouched =
        memcmp(&snapshot, &sentinel, sizeof(snapshot)) == 0;
    return memcmp(content.data, BLOB_BYTES, sizeof(BLOB_BYTES)) == 0;
}

static bool test_combined_verifier_and_runtime_reentry_guard(void)
{
    RuntimePair pair;
    RuntimeArena refresh_before;
    BlobArena blob_arena;
    MeisoBlobStore *store = NULL;
    MeisoRuntimeBlobTransferRef transfer = {0};
    MeisoRuntimeEvent commit_event = {0};
    MeisoRuntimeBlobPublishReport report = {0};
    MeisoRuntimeBlobSnapshot before = {0};
    MeisoRuntimeBlobSnapshot after = {0};
    MeisoRuntimeV3Config runtime_config_value;
    RecordingVerifierContext recording;

    CHECK(init_test_blob_store(&blob_arena, &store));
    CHECK(receive_small_blob_to_commit(&pair, &transfer, &commit_event));
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &before));
    memset(&recording, 0, sizeof(recording));
    recording.runtime = pair.edge_runtime;
    recording.transfer = &commit_event.blob_transfer;
    runtime_config_value = runtime_config(pair.edge_session);
    recording.runtime_size = meiso_runtime_v3_memory_required(
        &runtime_config_value
    );
    CHECK(recording.runtime_size != 0U &&
          recording.runtime_size <= sizeof(refresh_before.bytes));
    recording.refresh_before = refresh_before.bytes;
    CHECK(meiso_runtime_publish_blob_receive(
        pair.edge_runtime,
        &commit_event.blob_transfer,
        store,
        recording_reentrant_verifier,
        &recording,
        &report
    ) == MEISO_RUNTIME_OK);
    CHECK(recording.calls == 1U &&
          recording.reentry_status == MEISO_RUNTIME_CONFLICT &&
          recording.refresh_status == MEISO_RUNTIME_CONFLICT &&
          recording.refresh_untouched &&
          !recording.query_status && recording.query_untouched &&
          memcmp(recording.observed, BLOB_BYTES, sizeof(BLOB_BYTES)) == 0);
    CHECK(report.commit_status == MEISO_BLOB_COMMIT_PUBLISHED &&
          report.result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
          !report.forced_abort_attempted);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &after));
    CHECK(after.transfer_active && !after.publish_pending &&
          after.result_pending &&
          after.pending_result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
          after.next_offset == before.next_offset);
    return true;
}

static bool test_publish_alias_and_skipped_bridge_cleanup(void)
{
    RuntimePair pair;
    RuntimeArena runtime_before;
    BlobArena blob_arena;
    MeisoBlobStore *store = NULL;
    MeisoRuntimeBlobTransferRef transfer = {0};
    MeisoRuntimeEvent commit_event = {0};
    MeisoRuntimeBlobPublishReport report = {0};
    MeisoRuntimeBlobSnapshot before = {0};
    MeisoRuntimeBlobSnapshot after = {0};
    MeisoBlobStats stats = {0};
    MeisoRuntimeV3Config config;
    size_t runtime_size;

    CHECK(init_test_blob_store(&blob_arena, &store));
    CHECK(receive_small_blob_to_commit(&pair, &transfer, &commit_event));
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &before));
    CHECK(before.transfer_active && before.publish_pending &&
          !before.result_pending);
    config = runtime_config(pair.edge_session);
    runtime_size = meiso_runtime_v3_memory_required(&config);
    CHECK(runtime_size != 0U && runtime_size <= sizeof(runtime_before.bytes));
    memmove(
        runtime_before.bytes, pair.edge_runtime_arena.bytes, runtime_size
    );
    CHECK(meiso_runtime_refresh_bound_session(pair.edge_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(memcmp(
        runtime_before.bytes, pair.edge_runtime_arena.bytes, runtime_size
    ) == 0);
    CHECK(meiso_runtime_refresh_bound_session(NULL) ==
          MEISO_RUNTIME_INVALID_ARGUMENT);
    CHECK(meiso_runtime_publish_blob_receive(
        pair.edge_runtime,
        &commit_event.blob_transfer,
        store,
        meiso_runtime_blob_sha256_verify,
        NULL,
        (MeisoRuntimeBlobPublishReport *)(void *)&commit_event.blob_transfer
    ) == MEISO_RUNTIME_INVALID_ARGUMENT);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));
    CHECK(meiso_runtime_publish_blob_receive(
        pair.edge_runtime,
        (const MeisoRuntimeBlobTransferRef *)(const void *)pair.edge_runtime,
        store,
        meiso_runtime_blob_sha256_verify,
        NULL,
        &report
    ) == MEISO_RUNTIME_INVALID_ARGUMENT);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));
    CHECK(meiso_runtime_publish_blob_receive(
        pair.edge_runtime,
        &commit_event.blob_transfer,
        store,
        meiso_runtime_blob_sha256_verify,
        NULL,
        (MeisoRuntimeBlobPublishReport *)(void *)pair.edge_runtime
    ) == MEISO_RUNTIME_INVALID_ARGUMENT);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &after));
    CHECK(blob_snapshot_equal(&before, &after));

    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime,
        &commit_event.blob_transfer,
        MEISO_RUNTIME_BLOB_COMMIT
    ) == MEISO_RUNTIME_BLOB_NOT_ACTIVE);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &after));
    CHECK(!after.transfer_active && !after.publish_pending &&
          !after.result_pending);
    CHECK(meiso_blob_store_stats(store, &stats));
    CHECK(meiso_blob_store_count(store) == 0U &&
          stats.stages_started == 0U && stats.publications == 0U);
    return true;
}

static bool test_core_capacity_and_conflict_results(void)
{
    static const uint8_t OTHER_BYTES[] = {1U, 2U, 3U, 4U};
    static const uint8_t OTHER_SHA[32] = {
        0x9fU, 0x64U, 0xa7U, 0x47U, 0xe1U, 0xb9U, 0x7fU, 0x13U,
        0x1fU, 0xabU, 0xb6U, 0xb4U, 0x47U, 0x29U, 0x6cU, 0x9bU,
        0x6fU, 0x02U, 0x01U, 0xe7U, 0x9fU, 0xb3U, 0xc5U, 0x35U,
        0x6eU, 0x6cU, 0x77U, 0xe8U, 0x9bU, 0x6aU, 0x80U, 0x6aU
    };
    size_t variant;

    for (variant = 0U; variant < 2U; ++variant) {
        RuntimePair pair;
        BlobArena blob_arena;
        MeisoRuntimeEndpointDescriptor endpoint;
        MeisoRuntimeRouteDescriptor route;
        const MeisoBlobConfig config = {1U};
        MeisoBlobStore *store = NULL;
        MeisoRuntimeBlobTransferRef transfer = {0};
        MeisoRuntimeEvent commit_event = {0};
        MeisoRuntimeBlobPublishReport report = {0};
        MeisoMutableBuffer staging = {0};
        MeisoContentId existing_id = variant == 0U ?
            sha256_content_id(OTHER_SHA) : blob_content_id();
        MeisoBlobStats stats = {0};
        size_t required;

        blob_catalog(&endpoint, &route);
        required = meiso_blob_store_memory_required(&route.resource, &config);
        CHECK(required != 0U && required <= sizeof(blob_arena.bytes));
        CHECK(meiso_blob_store_init(
            blob_arena.bytes,
            required,
            &route.resource,
            &config,
            &store
        ) == MEISO_BLOB_INIT_OK);
        CHECK(meiso_blob_store_stage_begin(
            store, &existing_id, sizeof(OTHER_BYTES)
        ) == MEISO_BLOB_STAGE_STARTED);
        CHECK(meiso_blob_store_staging_buffer(store, &staging));
        memmove(staging.data, OTHER_BYTES, sizeof(OTHER_BYTES));
        CHECK(meiso_blob_store_commit_verified(
            store,
            sizeof(OTHER_BYTES),
            variant == 0U ? meiso_runtime_blob_sha256_verify : accept_blob,
            NULL
        ) == MEISO_BLOB_COMMIT_PUBLISHED);

        CHECK(receive_small_blob_to_commit(
            &pair, &transfer, &commit_event
        ));
        CHECK(meiso_runtime_publish_blob_receive(
            pair.edge_runtime,
            &commit_event.blob_transfer,
            store,
            meiso_runtime_blob_sha256_verify,
            NULL,
            &report
        ) == MEISO_RUNTIME_OK);
        CHECK(report.stage_status == MEISO_BLOB_STAGE_STARTED &&
              report.commit_status == (variant == 0U ?
                  MEISO_BLOB_COMMIT_CAPACITY : MEISO_BLOB_COMMIT_CONFLICT) &&
              report.result == (variant == 0U ?
                  MEISO_RUNTIME_BLOB_RESULT_CAPACITY :
                  MEISO_RUNTIME_BLOB_RESULT_CONFLICT) &&
              report.forced_abort_attempted &&
              report.forced_abort_status == MEISO_BLOB_ABORTED);
        CHECK(meiso_blob_store_count(store) == 1U);
        CHECK(meiso_blob_store_stats(store, &stats));
        CHECK(stats.transfer_aborts == 1U &&
              (variant == 0U ?
                  stats.capacity_failures == 1U : stats.conflicts == 1U));
    }
    return true;
}

static bool test_snapshot_prioritizes_pending_rejected_begin(void)
{
    RuntimePair pair;
    MeisoRuntimeBlobTransferRef active = {0};
    MeisoRuntimeBlobSnapshot before = {0};
    MeisoRuntimeBlobSnapshot pending = {0};
    MeisoRuntimeBlobSnapshot after = {0};
    MeisoRuntimeEvent event = {0};
    uint8_t second_begin[sizeof(GOLDEN_BLOB_BEGIN)];

    CHECK(start_ready_transfer(&pair, &active));
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &before));
    CHECK(before.transfer_active && before.transfer.transfer_id == 1U &&
          !before.result_pending);
    memmove(second_begin, GOLDEN_BLOB_BEGIN, sizeof(second_begin));
    second_begin[75] = 2U;
    CHECK(inject_edge_frame(
        &pair,
        (MeisoByteView){second_begin, sizeof(second_begin)},
        &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN &&
          event.blob_transfer.transfer_id == 2U);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &pending));
    CHECK(pending.transfer_active && pending.result_pending &&
          pending.transfer.transfer_id == 2U &&
          pending.pending_operation == MEISO_RUNTIME_BLOB_BEGIN &&
          pending.pending_result == MEISO_RUNTIME_BLOB_RESULT_BUSY &&
          pending.next_offset == 0U &&
          pending.transfer_id_high_water == 2U);
    CHECK(meiso_runtime_send_blob_result(
        pair.edge_runtime,
        &pending.transfer,
        MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_session_mark_outbound_sent(pair.edge_session) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_runtime_blob_snapshot(pair.edge_runtime, &after));
    CHECK(after.transfer_active && !after.result_pending &&
          after.transfer.transfer_id == active.transfer_id &&
          meiso_content_id_equal(
              &after.transfer.content_id, &active.content_id
          ) &&
          after.transfer.total_size == active.total_size &&
          after.begin_ready && after.next_offset == before.next_offset &&
          after.transfer_id_high_water == 2U);
    return true;
}

static bool receive_zero_blob_to_commit(
    RuntimePair *pair,
    bool composition_route,
    const MeisoContentId *content_id,
    MeisoRuntimeEvent *out_commit_event
)
{
    MeisoRuntimeBlobTransferRef transfer = {0};
    MeisoSessionRuntimeMessage message = {0};
    MeisoRuntimeEvent event = {0};
    const MeisoRuntimeRouteKey route = composition_route ?
        composition_blob_route_key() : blob_route_key();

    CHECK(init_pair(pair));
    CHECK(composition_route ?
        activate_composition_blob_catalog(pair) :
        activate_blob_catalog(pair));
    CHECK(meiso_runtime_send_blob_begin(
        pair->host_runtime, route, content_id, 0U, &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair, pair->host_session, pair->edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair->edge_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_blob_result(
        pair->edge_runtime, &event.blob_transfer, MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair, pair->edge_session, pair->host_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair->host_runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_blob_commit(
        pair->host_runtime, &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(receive_outbound(
        pair, pair->host_session, pair->edge_session, &message, NULL, 0U
    ));
    CHECK(meiso_runtime_receive(
        pair->edge_runtime, &message, out_commit_event
    ) == MEISO_RUNTIME_OK);
    CHECK(out_commit_event->blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          out_commit_event->blob_payload.size == 0U);
    return true;
}

static bool test_zero_generic_publish_and_mesh_rejection(void)
{
    static const uint8_t EMPTY_SHA[32] = {
        0xe3U, 0xb0U, 0xc4U, 0x42U, 0x98U, 0xfcU, 0x1cU, 0x14U,
        0x9aU, 0xfbU, 0xf4U, 0xc8U, 0x99U, 0x6fU, 0xb9U, 0x24U,
        0x27U, 0xaeU, 0x41U, 0xe4U, 0x64U, 0x9bU, 0x93U, 0x4cU,
        0xa4U, 0x95U, 0x99U, 0x1bU, 0x78U, 0x52U, 0xb8U, 0x55U
    };
    static LargeBlobArena composition_blob_arena;
    RuntimePair pair;
    BlobArena generic_blob_arena;
    MeisoBlobStore *store = NULL;
    MeisoRuntimeEvent commit_event = {0};
    MeisoRuntimeBlobPublishReport report = {0};
    MeisoContentId empty_id = sha256_content_id(EMPTY_SHA);
    const MeisoResourceDescriptor *mesh_resource =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
        );
    const MeisoBlobConfig mesh_config = {
        MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS
    };
    size_t required;

    CHECK(init_test_blob_store(&generic_blob_arena, &store));
    CHECK(receive_zero_blob_to_commit(
        &pair, false, &empty_id, &commit_event
    ));
    CHECK(meiso_runtime_publish_blob_receive(
        pair.edge_runtime,
        &commit_event.blob_transfer,
        store,
        meiso_runtime_blob_sha256_verify,
        NULL,
        &report
    ) == MEISO_RUNTIME_OK);
    CHECK(report.result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
          report.commit_status == MEISO_BLOB_COMMIT_PUBLISHED &&
          meiso_blob_store_count(store) == 1U);

    CHECK(mesh_resource != NULL);
    required = meiso_blob_store_memory_required(mesh_resource, &mesh_config);
    CHECK(required != 0U &&
          required <= sizeof(composition_blob_arena.bytes));
    CHECK(meiso_blob_store_init(
        composition_blob_arena.bytes,
        required,
        mesh_resource,
        &mesh_config,
        &store
    ) == MEISO_BLOB_INIT_OK);
    CHECK(receive_zero_blob_to_commit(
        &pair, true, &empty_id, &commit_event
    ));
    CHECK(meiso_runtime_publish_blob_receive(
        pair.edge_runtime,
        &commit_event.blob_transfer,
        store,
        meiso_composition_scene_mesh_blob_verify,
        NULL,
        &report
    ) == MEISO_RUNTIME_OK);
    CHECK(report.result == MEISO_RUNTIME_BLOB_RESULT_INTEGRITY_FAILED &&
          report.commit_status == MEISO_BLOB_COMMIT_INTEGRITY_FAILED &&
          report.forced_abort_attempted &&
          report.forced_abort_status == MEISO_BLOB_ABORTED &&
          meiso_blob_store_count(store) == 0U);
    return true;
}

int main(void)
{
    if (!test_frozen_v3_literals()) {
        return 1;
    }
    if (!test_golden_publish_flow()) {
        return 1;
    }
    if (!test_profile_and_runtime_version_selection()) {
        return 1;
    }
    if (!test_profile_mismatch_and_wire_mismatch()) {
        return 1;
    }
    if (!test_v3_arena_contract()) {
        return 1;
    }
    if (!test_malformed_blob_framing()) {
        return 1;
    }
    if (!test_abort_before_ready_and_late_ready()) {
        return 1;
    }
    if (!test_tx_busy_is_zero_progress()) {
        return 1;
    }
    if (!test_reassembly_boundaries()) {
        return 1;
    }
    if (!test_core_staging_ownership_and_forced_abort()) {
        return 1;
    }
    if (!test_edge_refuses_blob_for_capacity()) {
        return 1;
    }
    if (!test_route_and_identity_rejections()) {
        return 1;
    }
    if (!test_blob_routes_share_one_transfer_slot()) {
        return 1;
    }
    if (!test_catalog_commit_clears_blob_transfer()) {
        return 1;
    }
    if (!test_session_loss_clears_runtime_only_staging()) {
        return 1;
    }
    if (!test_ready_bound_session_identity_mismatch_is_stale()) {
        return 1;
    }
    if (!test_result_phase_and_offset_validation()) {
        return 1;
    }
    if (!test_chunk_gap_duplicate_and_overlap()) {
        return 1;
    }
    if (!test_result_requires_explicit_scene_visibility()) {
        return 1;
    }
    if (!test_v3_state_stream_and_blob_coexist()) {
        return 1;
    }
    if (!test_begin_replay_and_result_reference_isolation()) {
        return 1;
    }
    if (!test_commit_transport_failures_skip_core_publish()) {
        return 1;
    }
    if (!test_combined_verifier_and_runtime_reentry_guard()) {
        return 1;
    }
    if (!test_publish_alias_and_skipped_bridge_cleanup()) {
        return 1;
    }
    if (!test_core_capacity_and_conflict_results()) {
        return 1;
    }
    if (!test_snapshot_prioritizes_pending_rejected_begin()) {
        return 1;
    }
    if (!test_zero_generic_publish_and_mesh_rejection()) {
        return 1;
    }
    if (!test_blob_send_cannot_bypass_pending_state_receive()) {
        return 1;
    }
    (void)puts("runtime blob wire tests passed");
    return 0;
}
