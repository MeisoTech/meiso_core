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

enum {
    TEST_FRAME_CAPACITY = 4096,
    TEST_RUNTIME_CAPACITY = 256,
    TEST_VALUE_CAPACITY = 16
};

typedef union {
    max_align_t alignment;
    uint8_t bytes[8192];
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

typedef struct {
    MeisoRuntimeSnapshot snapshot;
    MeisoRuntimeEndpointDescriptor active_endpoint;
    MeisoRuntimeEndpointDescriptor staged_endpoint;
    MeisoRuntimeRouteDescriptor active_route;
    MeisoRuntimeRouteDescriptor staged_route;
    MeisoRuntimeSubscriptionSnapshot subscription;
    MeisoRuntimeStatus active_endpoint_status;
    MeisoRuntimeStatus staged_endpoint_status;
    MeisoRuntimeStatus active_route_status;
    MeisoRuntimeStatus staged_route_status;
    MeisoRuntimeStatus subscription_status;
    uint8_t active_capability[TEST_VALUE_CAPACITY];
    uint8_t staged_capability[TEST_VALUE_CAPACITY];
    uint8_t desired[TEST_VALUE_CAPACITY];
    uint8_t effective[TEST_VALUE_CAPACITY];
} RuntimeDigest;

static const MeisoEndpointId ENDPOINT_ID = {
    UINT64_C(0x0102030405060708), UINT64_C(0x1112131415161718)
};
static const uint64_t ENDPOINT_INCARNATION =
    UINT64_C(0x2122232425262728);
static const MeisoSchemaRef CAPABILITY_SCHEMA = {{
    UINT64_C(0x3132333435363738), UINT64_C(0x4142434445464748)
}, UINT32_C(0x51525354)};
static const MeisoResourceId RESOURCE_ID = {
    UINT64_C(0x6162636465666768), UINT64_C(0x7172737475767778)
};
static const MeisoSchemaRef RESOURCE_SCHEMA = {{
    UINT64_C(0x8182838485868788), UINT64_C(0x9192939495969798)
}, UINT32_C(0xa1a2a3a4)};
static const MeisoSchemaRef SELECTION_SCHEMA = {{
    UINT64_C(0xb1b2b3b4b5b6b7b8), UINT64_C(0xc1c2c3c4c5c6c7c8)
}, UINT32_C(0xd1d2d3d4)};
static const uint64_t REQUEST_ID = UINT64_C(0xe1e2e3e4e5e6e7e8);
static const uint64_t ROUTE_EPOCH = UINT64_C(0xf1f2f3f4f5f6f7f8);

/* These literals are the independent Runtime v1 wire contract. */
static const uint8_t GOLDEN_CATALOG[] = {
    0x4d, 0x52, 0x54, 0x30, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xaf,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x51, 0x52, 0x53, 0x54, 0x00, 0x00, 0x00, 0x03,
    0xa1, 0xb2, 0xc3,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x02, 0x02, 0x02, 0x00,
    0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0xa1, 0xa2, 0xa3, 0xa4,
    0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8,
    0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8,
    0xd1, 0xd2, 0xd3, 0xd4,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20,
    0x00, 0x00, 0x00, 0x10
};

static const uint8_t GOLDEN_ACCEPT[] = {
    0x4d, 0x52, 0x54, 0x30, 0x01, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
};

static const uint8_t GOLDEN_REJECT[] = {
    0x4d, 0x52, 0x54, 0x30, 0x01, 0x03, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x18,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x02, 0x00, 0x00, 0x00
};

static const uint8_t GOLDEN_DESIRED[] = {
    0x4d, 0x52, 0x54, 0x30, 0x01, 0x04, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x4f,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
    0x21, 0x22, 0x23
};

static const uint8_t GOLDEN_ACTUAL[] = {
    0x4d, 0x52, 0x54, 0x30, 0x01, 0x05, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x56,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
    0x02, 0x00, 0x00, 0x00,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0x00, 0x00, 0x00, 0x02, 0x31, 0x32
};

static const uint8_t GOLDEN_RESOURCE[] = {
    0x4d, 0x52, 0x54, 0x30, 0x01, 0x06, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x6c,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x02, 0x00, 0x00, 0x00,
    0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0xa1, 0xa2, 0xa3, 0xa4,
    0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0x00, 0x00, 0x00, 0x04, 0x41, 0x42, 0x43, 0x44
};

static MeisoSessionConfig session_config(MeisoSessionRole role)
{
    MeisoSessionConfig config;
    const MeisoPeerId host_id = {UINT64_C(0x11), UINT64_C(0x12)};
    const MeisoPeerId edge_id = {UINT64_C(0x21), UINT64_C(0x22)};

    memset(&config, 0, sizeof(config));
    config.local_role = role;
    config.local_peer_id = role == MEISO_SESSION_ROLE_HOST ? host_id : edge_id;
    config.expected_peer_id =
        role == MEISO_SESSION_ROLE_HOST ? edge_id : host_id;
    config.local_incarnation = role == MEISO_SESSION_ROLE_HOST ? 31U : 41U;
    config.local_nonce = role == MEISO_SESSION_ROLE_HOST ?
        (MeisoSessionNonce){UINT64_C(0x61), UINT64_C(0x62)} :
        (MeisoSessionNonce){UINT64_C(0x71), UINT64_C(0x72)};
    config.protocol = (MeisoProtocolVersion){1U, 0U};
    config.profile.id = (MeisoProfileId){UINT64_C(0x81), UINT64_C(0x82)};
    config.profile.version = 1U;
    config.limits = (MeisoSessionLimits){4096U, 2048U, 2U};
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
    uint8_t frame[TEST_FRAME_CAPACITY];
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

static bool initialize_pair(RuntimePair *pair)
{
    const MeisoSessionConfig host_config =
        session_config(MEISO_SESSION_ROLE_HOST);
    const MeisoSessionConfig edge_config =
        session_config(MEISO_SESSION_ROLE_EDGE);
    MeisoRuntimeConfig runtime_config;
    size_t required;

    memset(pair, 0, sizeof(*pair));
    CHECK(initialize_session(
        &pair->host_session_arena, &host_config, &pair->host_session
    ));
    CHECK(initialize_session(
        &pair->edge_session_arena, &edge_config, &pair->edge_session
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
    runtime_config.max_endpoints = 1U;
    runtime_config.max_routes = 1U;
    runtime_config.max_capability_bytes = 8U;
    runtime_config.max_parameter_bytes_per_route = TEST_VALUE_CAPACITY;
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

static bool receive_queued_session_runtime(
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoSessionRuntimeMessage *out_message
)
{
    uint8_t frame[TEST_FRAME_CAPACITY];
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

static bool capture_queued_payload(
    MeisoSession *sender,
    MeisoSession *receiver,
    uint8_t output[TEST_RUNTIME_CAPACITY],
    size_t *out_size
)
{
    MeisoSessionRuntimeMessage message = {0};

    CHECK(out_size != NULL);
    CHECK(receive_queued_session_runtime(sender, receiver, &message));
    CHECK(message.payload.size <= TEST_RUNTIME_CAPACITY);
    if (message.payload.size != 0U) {
        memmove(output, message.payload.data, message.payload.size);
    }
    *out_size = message.payload.size;
    return true;
}

static bool inject_runtime(
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoRuntime *runtime,
    const uint8_t *bytes,
    size_t size,
    MeisoRuntimeEvent *out_event,
    MeisoRuntimeStatus *out_status
)
{
    const MeisoByteView payload = {size == 0U ? NULL : bytes, size};
    MeisoSessionRuntimeMessage message = {0};

    CHECK(out_event != NULL && out_status != NULL);
    CHECK(meiso_session_submit_runtime(sender, payload) ==
          MEISO_SESSION_SUBMIT_QUEUED);
    CHECK(receive_queued_session_runtime(sender, receiver, &message));
    *out_status = meiso_runtime_receive(runtime, &message, out_event);
    return true;
}

static bool deliver_queued_runtime(
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoRuntime *runtime,
    MeisoRuntimeEvent *out_event
)
{
    MeisoSessionRuntimeMessage message = {0};

    CHECK(receive_queued_session_runtime(sender, receiver, &message));
    CHECK(meiso_runtime_receive(runtime, &message, out_event) ==
          MEISO_RUNTIME_OK);
    return true;
}

static bool capture_runtime_digest(
    const MeisoRuntime *runtime,
    RuntimeDigest *out
)
{
    memset(out, 0, sizeof(*out));
    CHECK(meiso_runtime_snapshot(runtime, &out->snapshot));
    out->active_endpoint_status = meiso_runtime_endpoint_at(
        runtime, 0U, &out->active_endpoint
    );
    out->staged_endpoint_status = meiso_runtime_staged_endpoint_at(
        runtime, 0U, &out->staged_endpoint
    );
    out->active_route_status = meiso_runtime_route_at(
        runtime, 0U, &out->active_route
    );
    out->staged_route_status = meiso_runtime_staged_route_at(
        runtime, 0U, &out->staged_route
    );
    out->subscription_status = meiso_runtime_subscription_snapshot(
        runtime, RESOURCE_ID, &out->subscription
    );
    if (out->active_endpoint_status == MEISO_RUNTIME_OK) {
        CHECK(out->active_endpoint.capability.size <=
              sizeof(out->active_capability));
        if (out->active_endpoint.capability.size != 0U) {
            memmove(out->active_capability,
                    out->active_endpoint.capability.data,
                    out->active_endpoint.capability.size);
        }
        out->active_endpoint.capability.data = NULL;
    }
    if (out->staged_endpoint_status == MEISO_RUNTIME_OK) {
        CHECK(out->staged_endpoint.capability.size <=
              sizeof(out->staged_capability));
        if (out->staged_endpoint.capability.size != 0U) {
            memmove(out->staged_capability,
                    out->staged_endpoint.capability.data,
                    out->staged_endpoint.capability.size);
        }
        out->staged_endpoint.capability.data = NULL;
    }
    if (out->subscription_status == MEISO_RUNTIME_OK) {
        CHECK(out->subscription.desired_parameters.size <=
              sizeof(out->desired));
        CHECK(out->subscription.effective_parameters.size <=
              sizeof(out->effective));
        if (out->subscription.desired_parameters.size != 0U) {
            memmove(out->desired,
                    out->subscription.desired_parameters.data,
                    out->subscription.desired_parameters.size);
        }
        if (out->subscription.effective_parameters.size != 0U) {
            memmove(out->effective,
                    out->subscription.effective_parameters.data,
                    out->subscription.effective_parameters.size);
        }
        out->subscription.desired_parameters.data = NULL;
        out->subscription.effective_parameters.data = NULL;
    }
    return true;
}

static bool event_is_zero(const MeisoRuntimeEvent *event)
{
    MeisoRuntimeEvent zero;

    memset(&zero, 0, sizeof(zero));
    return memcmp(event, &zero, sizeof(zero)) == 0;
}

static void set_wire_size(uint8_t *bytes, size_t size)
{
    bytes[8] = (uint8_t)(size >> 24U);
    bytes[9] = (uint8_t)(size >> 16U);
    bytes[10] = (uint8_t)(size >> 8U);
    bytes[11] = (uint8_t)size;
}

static bool rejected_frame_preserves(
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoRuntime *runtime,
    const uint8_t *golden,
    size_t golden_size,
    const size_t *reserved_offsets,
    size_t reserved_count
)
{
    uint8_t candidate[TEST_RUNTIME_CAPACITY];
    RuntimeDigest before;
    RuntimeDigest after;
    MeisoRuntimeEvent event;
    MeisoRuntimeStatus status;
    size_t cut;
    size_t index;

    CHECK(golden_size + 1U <= sizeof(candidate));
    for (cut = 0U; cut < golden_size; ++cut) {
        memset(candidate, 0, sizeof(candidate));
        if (cut != 0U) {
            memmove(candidate, golden, cut);
        }
        if (cut >= MEISO_RUNTIME_WIRE_HEADER_SIZE) {
            set_wire_size(candidate, cut);
        }
        CHECK(capture_runtime_digest(runtime, &before));
        memset(&event, 0xa5, sizeof(event));
        CHECK(inject_runtime(
            sender, receiver, runtime, candidate, cut, &event, &status
        ));
        if (status != MEISO_RUNTIME_MALFORMED || !event_is_zero(&event)) {
            (void)fprintf(stderr, "truncation %zu/%zu was not cleanly rejected\n",
                          cut, golden_size);
            return false;
        }
        CHECK(capture_runtime_digest(runtime, &after));
        CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    }

    memmove(candidate, golden, golden_size);
    candidate[golden_size] = 0x5aU;
    set_wire_size(candidate, golden_size + 1U);
    CHECK(capture_runtime_digest(runtime, &before));
    memset(&event, 0xa5, sizeof(event));
    CHECK(inject_runtime(
        sender, receiver, runtime, candidate, golden_size + 1U,
        &event, &status
    ));
    CHECK(status == MEISO_RUNTIME_MALFORMED);
    CHECK(event_is_zero(&event));
    CHECK(capture_runtime_digest(runtime, &after));
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);

    for (index = 0U; index < reserved_count; ++index) {
        CHECK(reserved_offsets[index] < golden_size);
        memmove(candidate, golden, golden_size);
        candidate[reserved_offsets[index]] = 0x01U;
        CHECK(capture_runtime_digest(runtime, &before));
        memset(&event, 0xa5, sizeof(event));
        CHECK(inject_runtime(
            sender, receiver, runtime, candidate, golden_size,
            &event, &status
        ));
        CHECK(status == MEISO_RUNTIME_MALFORMED);
        CHECK(event_is_zero(&event));
        CHECK(capture_runtime_digest(runtime, &after));
        CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    }
    return true;
}

static bool check_golden(
    const char *name,
    const uint8_t *actual,
    size_t actual_size,
    const uint8_t *expected,
    size_t expected_size
)
{
    size_t index;

    if (actual_size != expected_size) {
        (void)fprintf(stderr, "%s size: got %zu, expected %zu\n",
                      name, actual_size, expected_size);
        return false;
    }
    for (index = 0U; index < expected_size; ++index) {
        if (actual[index] != expected[index]) {
            (void)fprintf(stderr,
                          "%s byte %zu: got %02x, expected %02x\n",
                          name, index, actual[index], expected[index]);
            return false;
        }
    }
    return true;
}

static void make_catalog(
    MeisoRuntimeEndpointDescriptor *endpoint,
    MeisoRuntimeRouteDescriptor *route
)
{
    static const uint8_t capability[] = {0xa1U, 0xb2U, 0xc3U};

    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->id = ENDPOINT_ID;
    endpoint->incarnation = ENDPOINT_INCARNATION;
    endpoint->capability_schema = CAPABILITY_SCHEMA;
    endpoint->capability = (MeisoByteView){capability, sizeof(capability)};

    memset(route, 0, sizeof(*route));
    route->endpoint_id = ENDPOINT_ID;
    route->endpoint_incarnation = ENDPOINT_INCARNATION;
    route->resource.id = RESOURCE_ID;
    route->resource.kind = MEISO_RESOURCE_KIND_STREAM;
    route->resource.schema = RESOURCE_SCHEMA;
    route->resource.owner = MEISO_OWNER_ROLE_EDGE;
    route->resource.max_payload_size = 32U;
    route->selection_schema = SELECTION_SCHEMA;
    route->max_selection_bytes = 16U;
    route->delivery = MEISO_RUNTIME_DELIVERY_HOST_SUBSCRIPTION;
}

static bool check_catalog_contents(const MeisoRuntime *runtime, bool staged)
{
    static const uint8_t capability[] = {0xa1U, 0xb2U, 0xc3U};
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor route;
    MeisoRuntimeStatus status;

    memset(&endpoint, 0, sizeof(endpoint));
    memset(&route, 0, sizeof(route));
    status = staged ? meiso_runtime_staged_endpoint_at(
        runtime, 0U, &endpoint
    ) : meiso_runtime_endpoint_at(runtime, 0U, &endpoint);
    CHECK(status == MEISO_RUNTIME_OK);
    CHECK(meiso_endpoint_id_equal(endpoint.id, ENDPOINT_ID));
    CHECK(endpoint.incarnation == ENDPOINT_INCARNATION);
    CHECK(meiso_schema_ref_equal(
        endpoint.capability_schema, CAPABILITY_SCHEMA
    ));
    CHECK(endpoint.capability.size == sizeof(capability));
    CHECK(memcmp(endpoint.capability.data, capability, sizeof(capability)) == 0);

    status = staged ? meiso_runtime_staged_route_at(
        runtime, 0U, &route
    ) : meiso_runtime_route_at(runtime, 0U, &route);
    CHECK(status == MEISO_RUNTIME_OK);
    CHECK(meiso_endpoint_id_equal(route.endpoint_id, ENDPOINT_ID));
    CHECK(route.endpoint_incarnation == ENDPOINT_INCARNATION);
    CHECK(meiso_resource_id_equal(route.resource.id, RESOURCE_ID));
    CHECK(route.resource.kind == MEISO_RESOURCE_KIND_STREAM);
    CHECK(route.resource.owner == MEISO_OWNER_ROLE_EDGE);
    CHECK(route.resource.max_payload_size == 32U);
    CHECK(meiso_schema_ref_equal(route.resource.schema, RESOURCE_SCHEMA));
    CHECK(meiso_schema_ref_equal(route.selection_schema, SELECTION_SCHEMA));
    CHECK(route.max_selection_bytes == 16U);
    CHECK(route.delivery == MEISO_RUNTIME_DELIVERY_HOST_SUBSCRIPTION);
    return true;
}

static bool test_runtime_wire_v1(void)
{
    static const size_t catalog_reserved[] = {6U, 7U, 122U};
    static const size_t header_reserved[] = {6U, 7U};
    static const size_t reject_reserved[] = {6U, 7U, 21U, 22U, 23U};
    static const size_t desired_reserved[] = {6U, 7U, 69U, 70U, 71U};
    static const size_t actual_reserved[] = {6U, 7U, 70U, 71U};
    static const size_t resource_reserved[] = {6U, 7U, 61U, 62U, 63U};
    static const uint8_t desired_parameters[] = {0x21U, 0x22U, 0x23U};
    static const uint8_t effective_parameters[] = {0x31U, 0x32U};
    static const uint8_t resource_payload[] = {0x41U, 0x42U, 0x43U, 0x44U};
    RuntimePair pair;
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor route;
    MeisoRuntimeSubscriptionDesired desired;
    MeisoRuntimeSubscriptionActual actual;
    MeisoRuntimeResourceMessage resource;
    MeisoRuntimeEvent event;
    MeisoRuntimeStatus status;
    uint8_t wire[TEST_RUNTIME_CAPACITY];
    size_t wire_size;

    CHECK(initialize_pair(&pair));
    make_catalog(&endpoint, &route);

    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 1U, &endpoint, 1U, &route, 1U
    ) == MEISO_RUNTIME_OK);
    CHECK(capture_queued_payload(
        pair.edge_session, pair.host_session, wire, &wire_size
    ));
    CHECK(check_golden(
        "CATALOG", wire, wire_size, GOLDEN_CATALOG, sizeof(GOLDEN_CATALOG)
    ));
    CHECK(rejected_frame_preserves(
        pair.edge_session, pair.host_session, pair.host_runtime,
        GOLDEN_CATALOG, sizeof(GOLDEN_CATALOG),
        catalog_reserved, sizeof(catalog_reserved) / sizeof(*catalog_reserved)
    ));
    CHECK(inject_runtime(
        pair.edge_session, pair.host_session, pair.host_runtime,
        GOLDEN_CATALOG, sizeof(GOLDEN_CATALOG), &event, &status
    ));
    CHECK(status == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED);
    CHECK(event.catalog_generation == 1U);
    CHECK(check_catalog_contents(pair.host_runtime, true));

    CHECK(meiso_runtime_accept_catalog(pair.host_runtime) == MEISO_RUNTIME_OK);
    CHECK(capture_queued_payload(
        pair.host_session, pair.edge_session, wire, &wire_size
    ));
    CHECK(check_golden(
        "ACCEPT", wire, wire_size, GOLDEN_ACCEPT, sizeof(GOLDEN_ACCEPT)
    ));
    CHECK(rejected_frame_preserves(
        pair.host_session, pair.edge_session, pair.edge_runtime,
        GOLDEN_ACCEPT, sizeof(GOLDEN_ACCEPT),
        header_reserved, sizeof(header_reserved) / sizeof(*header_reserved)
    ));
    CHECK(inject_runtime(
        pair.host_session, pair.edge_session, pair.edge_runtime,
        GOLDEN_ACCEPT, sizeof(GOLDEN_ACCEPT), &event, &status
    ));
    CHECK(status == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED);
    CHECK(event.catalog_generation == 1U);
    CHECK(check_catalog_contents(pair.host_runtime, false));
    CHECK(check_catalog_contents(pair.edge_runtime, false));

    CHECK(meiso_runtime_offer_catalog(
        pair.edge_runtime, 2U, &endpoint, 1U, &route, 1U
    ) == MEISO_RUNTIME_OK);
    CHECK(deliver_queued_runtime(
        pair.edge_session, pair.host_session, pair.host_runtime, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED);
    CHECK(event.catalog_generation == 2U);
    CHECK(meiso_runtime_reject_catalog(
        pair.host_runtime, MEISO_RUNTIME_CATALOG_REJECT_POLICY
    ) == MEISO_RUNTIME_OK);
    CHECK(capture_queued_payload(
        pair.host_session, pair.edge_session, wire, &wire_size
    ));
    CHECK(check_golden(
        "REJECT", wire, wire_size, GOLDEN_REJECT, sizeof(GOLDEN_REJECT)
    ));
    CHECK(rejected_frame_preserves(
        pair.host_session, pair.edge_session, pair.edge_runtime,
        GOLDEN_REJECT, sizeof(GOLDEN_REJECT),
        reject_reserved, sizeof(reject_reserved) / sizeof(*reject_reserved)
    ));
    CHECK(inject_runtime(
        pair.host_session, pair.edge_session, pair.edge_runtime,
        GOLDEN_REJECT, sizeof(GOLDEN_REJECT), &event, &status
    ));
    CHECK(status == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_REJECTED);
    CHECK(event.catalog_generation == 2U);
    CHECK(event.reject_reason == MEISO_RUNTIME_CATALOG_REJECT_POLICY);

    memset(&desired, 0, sizeof(desired));
    desired.request_id = REQUEST_ID;
    desired.endpoint_id = ENDPOINT_ID;
    desired.endpoint_incarnation = ENDPOINT_INCARNATION;
    desired.resource_id = RESOURCE_ID;
    desired.enabled = true;
    desired.parameters = (MeisoByteView){
        desired_parameters, sizeof(desired_parameters)
    };
    CHECK(meiso_runtime_request_subscription(
        pair.host_runtime, &desired
    ) == MEISO_RUNTIME_OK);
    CHECK(capture_queued_payload(
        pair.host_session, pair.edge_session, wire, &wire_size
    ));
    CHECK(check_golden(
        "DESIRED", wire, wire_size, GOLDEN_DESIRED, sizeof(GOLDEN_DESIRED)
    ));
    CHECK(rejected_frame_preserves(
        pair.host_session, pair.edge_session, pair.edge_runtime,
        GOLDEN_DESIRED, sizeof(GOLDEN_DESIRED),
        desired_reserved, sizeof(desired_reserved) / sizeof(*desired_reserved)
    ));
    CHECK(inject_runtime(
        pair.host_session, pair.edge_session, pair.edge_runtime,
        GOLDEN_DESIRED, sizeof(GOLDEN_DESIRED), &event, &status
    ));
    CHECK(status == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_SUBSCRIPTION_DESIRED);
    CHECK(event.catalog_generation == 1U);
    CHECK(event.desired.request_id == REQUEST_ID);
    CHECK(event.desired.enabled);
    CHECK(event.desired.parameters.size == sizeof(desired_parameters));
    CHECK(memcmp(event.desired.parameters.data, desired_parameters,
                 sizeof(desired_parameters)) == 0);

    memset(&actual, 0, sizeof(actual));
    actual.request_id = REQUEST_ID;
    actual.endpoint_id = ENDPOINT_ID;
    actual.endpoint_incarnation = ENDPOINT_INCARNATION;
    actual.resource_id = RESOURCE_ID;
    actual.status = MEISO_RUNTIME_ACTUAL_ACTIVE;
    actual.route_epoch = ROUTE_EPOCH;
    actual.effective_parameters = (MeisoByteView){
        effective_parameters, sizeof(effective_parameters)
    };
    CHECK(meiso_runtime_report_subscription(
        pair.edge_runtime, &actual
    ) == MEISO_RUNTIME_OK);
    CHECK(capture_queued_payload(
        pair.edge_session, pair.host_session, wire, &wire_size
    ));
    CHECK(check_golden(
        "ACTUAL", wire, wire_size, GOLDEN_ACTUAL, sizeof(GOLDEN_ACTUAL)
    ));
    CHECK(rejected_frame_preserves(
        pair.edge_session, pair.host_session, pair.host_runtime,
        GOLDEN_ACTUAL, sizeof(GOLDEN_ACTUAL),
        actual_reserved, sizeof(actual_reserved) / sizeof(*actual_reserved)
    ));
    CHECK(inject_runtime(
        pair.edge_session, pair.host_session, pair.host_runtime,
        GOLDEN_ACTUAL, sizeof(GOLDEN_ACTUAL), &event, &status
    ));
    CHECK(status == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_SUBSCRIPTION_ACTUAL);
    CHECK(event.catalog_generation == 1U);
    CHECK(event.actual.request_id == REQUEST_ID);
    CHECK(event.actual.status == MEISO_RUNTIME_ACTUAL_ACTIVE);
    CHECK(event.actual.route_epoch == ROUTE_EPOCH);
    CHECK(event.actual.effective_parameters.size ==
          sizeof(effective_parameters));
    CHECK(memcmp(event.actual.effective_parameters.data, effective_parameters,
                 sizeof(effective_parameters)) == 0);

    memset(&resource, 0, sizeof(resource));
    resource.endpoint_id = ENDPOINT_ID;
    resource.endpoint_incarnation = ENDPOINT_INCARNATION;
    resource.resource_id = RESOURCE_ID;
    resource.kind = MEISO_RESOURCE_KIND_STREAM;
    resource.schema = RESOURCE_SCHEMA;
    resource.request_id = REQUEST_ID;
    resource.route_epoch = ROUTE_EPOCH;
    resource.payload = (MeisoByteView){
        resource_payload, sizeof(resource_payload)
    };
    CHECK(meiso_runtime_send_resource(
        pair.edge_runtime, &resource
    ) == MEISO_RUNTIME_OK);
    CHECK(capture_queued_payload(
        pair.edge_session, pair.host_session, wire, &wire_size
    ));
    CHECK(check_golden(
        "RESOURCE", wire, wire_size, GOLDEN_RESOURCE, sizeof(GOLDEN_RESOURCE)
    ));
    CHECK(rejected_frame_preserves(
        pair.edge_session, pair.host_session, pair.host_runtime,
        GOLDEN_RESOURCE, sizeof(GOLDEN_RESOURCE),
        resource_reserved, sizeof(resource_reserved) / sizeof(*resource_reserved)
    ));
    CHECK(inject_runtime(
        pair.edge_session, pair.host_session, pair.host_runtime,
        GOLDEN_RESOURCE, sizeof(GOLDEN_RESOURCE), &event, &status
    ));
    CHECK(status == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_RESOURCE);
    CHECK(event.catalog_generation == 1U);
    CHECK(event.resource.request_id == REQUEST_ID);
    CHECK(event.resource.route_epoch == ROUTE_EPOCH);
    CHECK(event.resource.payload.size == sizeof(resource_payload));
    CHECK(memcmp(event.resource.payload.data, resource_payload,
                 sizeof(resource_payload)) == 0);
    return true;
}

int main(void)
{
    if (!test_runtime_wire_v1()) {
        return 1;
    }
    (void)puts("runtime wire tests passed");
    return 0;
}
