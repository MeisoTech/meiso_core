#include <meiso/core/session.h>

#include "meiso_test_support.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef union {
    max_align_t alignment;
    uint8_t bytes[4096];
} SessionArena;

typedef struct {
    SessionArena host_arena;
    SessionArena edge_arena;
    MeisoSessionConfig host_config;
    MeisoSessionConfig edge_config;
    MeisoSession *host;
    MeisoSession *edge;
} SessionPair;

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
    TEST_MESSAGE_OPEN = 1,
    TEST_MESSAGE_ACCEPT = 2,
    TEST_MESSAGE_CONFIRM = 3,
    TEST_MESSAGE_READY = 4,
    TEST_MESSAGE_REJECT = 5,
    TEST_MESSAGE_CLOSE = 6,
    TEST_MESSAGE_RUNTIME = 7
};

_Static_assert(sizeof(MeisoSessionRole) == sizeof(uint8_t),
    "session role must have fixed width");
_Static_assert(sizeof(MeisoSessionState) == sizeof(uint8_t),
    "session state must have fixed width");
_Static_assert(sizeof(MeisoSessionFailure) == sizeof(uint8_t),
    "session failure must have fixed width");
_Static_assert(sizeof(MeisoSessionInitStatus) == sizeof(uint8_t),
    "session init status must have fixed width");
_Static_assert(sizeof(MeisoSessionReceiveStatus) == sizeof(uint8_t),
    "session receive status must have fixed width");

static bool capture_valid_open(
    uint8_t *frame,
    size_t capacity,
    size_t *out_size
);

static bool expect_edge_frame_failure(
    const uint8_t *frame,
    size_t frame_size,
    MeisoSessionFailure expected_failure
);

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

static MeisoSessionConfig host_config(void)
{
    MeisoSessionConfig config;

    memset(&config, 0, sizeof(config));
    config.local_role = MEISO_SESSION_ROLE_HOST;
    config.local_peer_id = (MeisoPeerId){
        UINT64_C(0x0102030405060708),
        UINT64_C(0x1112131415161718)
    };
    config.local_incarnation = UINT64_C(0x2122232425262728);
    config.local_nonce = (MeisoSessionNonce){
        UINT64_C(0x3132333435363738),
        UINT64_C(0x4142434445464748)
    };
    config.expected_peer_id = (MeisoPeerId){
        UINT64_C(0x5152535455565758),
        UINT64_C(0x6162636465666768)
    };
    config.protocol = (MeisoProtocolVersion){UINT16_C(0x0102), UINT16_C(0x0304)};
    config.profile.id = (MeisoProfileId){
        UINT64_C(0x7172737475767778),
        UINT64_C(0x8182838485868788)
    };
    config.profile.version = UINT32_C(0x91929394);
    config.limits = (MeisoSessionLimits){256U, 64U, 4U};
    return config;
}

static MeisoSessionConfig edge_config(void)
{
    MeisoSessionConfig config = host_config();
    MeisoPeerId host_peer = config.local_peer_id;

    config.local_role = MEISO_SESSION_ROLE_EDGE;
    config.local_peer_id = config.expected_peer_id;
    config.local_incarnation = UINT64_C(0xA1A2A3A4A5A6A7A8);
    config.local_nonce = (MeisoSessionNonce){
        UINT64_C(0xB1B2B3B4B5B6B7B8),
        UINT64_C(0xC1C2C3C4C5C6C7C8)
    };
    config.expected_peer_id = host_peer;
    return config;
}

static bool initialize_session(
    SessionArena *arena,
    const MeisoSessionConfig *config,
    MeisoSession **out_session
)
{
    const size_t required = meiso_session_memory_required(config);

    CHECK(required != 0U);
    CHECK(required <= sizeof(arena->bytes));
    CHECK(meiso_session_init(
        arena->bytes, required, config, out_session
    ) == MEISO_SESSION_INIT_OK);
    CHECK(*out_session != NULL);
    return true;
}

static bool initialize_pair(SessionPair *pair)
{
    memset(pair, 0, sizeof(*pair));
    pair->host_config = host_config();
    pair->edge_config = edge_config();
    CHECK(initialize_session(
        &pair->host_arena, &pair->host_config, &pair->host
    ));
    CHECK(initialize_session(
        &pair->edge_arena, &pair->edge_config, &pair->edge
    ));
    return true;
}

static bool snapshot_is(
    const MeisoSession *session,
    MeisoSessionState state,
    MeisoSessionFailure failure
)
{
    MeisoSessionSnapshot snapshot;

    return meiso_session_snapshot(session, &snapshot) &&
        snapshot.state == state && snapshot.failure == failure;
}

static bool copy_next_outbound(
    const MeisoSession *session,
    uint8_t *storage,
    size_t capacity,
    size_t *out_size
)
{
    MeisoByteView frame;

    CHECK(meiso_session_next_outbound(session, &frame));
    CHECK(frame.size <= capacity);
    CHECK(frame.size == 0U || frame.data != NULL);
    if (frame.size != 0U) {
        memmove(storage, frame.data, frame.size);
    }
    *out_size = frame.size;
    return true;
}

static bool next_outbound_equals(
    const MeisoSession *session,
    const uint8_t *expected,
    size_t expected_size
)
{
    MeisoByteView first;
    MeisoByteView second;

    CHECK(meiso_session_next_outbound(session, &first));
    CHECK(meiso_session_next_outbound(session, &second));
    CHECK(first.data == second.data && first.size == second.size);
    CHECK(first.size == expected_size);
    CHECK(expected_size == 0U ||
        memcmp(first.data, expected, expected_size) == 0);
    return true;
}

static bool deliver_next(
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoSessionReceiveStatus expected_status
)
{
    uint8_t frame[512];
    size_t frame_size;
    MeisoSessionRuntimeMessage runtime = {0};

    CHECK(copy_next_outbound(
        sender, frame, sizeof(frame), &frame_size
    ));
    CHECK(meiso_session_mark_outbound_sent(sender) ==
        MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        receiver,
        (MeisoByteView){frame, frame_size},
        &runtime
    ) == expected_status);
    return true;
}

static bool make_ready(SessionPair *pair)
{
    MeisoByteView frame;

    CHECK(meiso_session_start(pair->edge) == MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(pair->host) == MEISO_SESSION_START_STARTED);
    CHECK(!meiso_session_next_outbound(pair->edge, &frame));
    CHECK(deliver_next(
        pair->host, pair->edge, MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(deliver_next(
        pair->edge, pair->host, MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(deliver_next(
        pair->host, pair->edge, MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(deliver_next(
        pair->edge, pair->host, MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(snapshot_is(
        pair->host, MEISO_SESSION_STATE_READY, MEISO_SESSION_FAILURE_NONE
    ));
    CHECK(snapshot_is(
        pair->edge, MEISO_SESSION_STATE_READY, MEISO_SESSION_FAILURE_NONE
    ));
    return true;
}

static size_t build_runtime_frame(
    uint8_t *frame,
    size_t capacity,
    MeisoSessionId session_id,
    const uint8_t *payload,
    size_t payload_size
)
{
    const size_t total = MEISO_SESSION_WIRE_HEADER_SIZE +
        MEISO_SESSION_ID_WIRE_SIZE + payload_size;

    if (total > capacity || total > UINT32_MAX) {
        return 0U;
    }
    frame[0] = (uint8_t)'M';
    frame[1] = (uint8_t)'S';
    frame[2] = (uint8_t)'L';
    frame[3] = (uint8_t)'0';
    frame[4] = MEISO_SESSION_WIRE_VERSION;
    frame[5] = TEST_MESSAGE_RUNTIME;
    frame[6] = 0U;
    frame[7] = 0U;
    write_u32_be(frame + 8U, (uint32_t)total);
    write_u64_be(frame + 12U, session_id.host_nonce.high);
    write_u64_be(frame + 20U, session_id.host_nonce.low);
    write_u64_be(frame + 28U, session_id.edge_nonce.high);
    write_u64_be(frame + 36U, session_id.edge_nonce.low);
    if (payload_size != 0U) {
        memmove(frame + 44U, payload, payload_size);
    }
    return total;
}

static size_t build_session_only_frame(
    uint8_t *frame,
    size_t capacity,
    uint8_t type,
    MeisoSessionId session_id
)
{
    const size_t total = MEISO_SESSION_WIRE_HEADER_SIZE +
        MEISO_SESSION_ID_WIRE_SIZE;

    if (total > capacity) {
        return 0U;
    }
    frame[0] = (uint8_t)'M';
    frame[1] = (uint8_t)'S';
    frame[2] = (uint8_t)'L';
    frame[3] = (uint8_t)'0';
    frame[4] = MEISO_SESSION_WIRE_VERSION;
    frame[5] = type;
    frame[6] = 0U;
    frame[7] = 0U;
    write_u32_be(frame + 8U, (uint32_t)total);
    write_u64_be(frame + 12U, session_id.host_nonce.high);
    write_u64_be(frame + 20U, session_id.host_nonce.low);
    write_u64_be(frame + 28U, session_id.edge_nonce.high);
    write_u64_be(frame + 36U, session_id.edge_nonce.low);
    return total;
}

static size_t build_close_frame(
    uint8_t *frame,
    size_t capacity,
    MeisoSessionId session_id,
    MeisoSessionCloseReason reason
)
{
    const size_t total = MEISO_SESSION_WIRE_HEADER_SIZE +
        MEISO_SESSION_ID_WIRE_SIZE + 1U;

    if (total > capacity) {
        return 0U;
    }
    frame[0] = (uint8_t)'M';
    frame[1] = (uint8_t)'S';
    frame[2] = (uint8_t)'L';
    frame[3] = (uint8_t)'0';
    frame[4] = MEISO_SESSION_WIRE_VERSION;
    frame[5] = TEST_MESSAGE_CLOSE;
    frame[6] = 0U;
    frame[7] = 0U;
    write_u32_be(frame + 8U, (uint32_t)total);
    write_u64_be(frame + 12U, session_id.host_nonce.high);
    write_u64_be(frame + 20U, session_id.host_nonce.low);
    write_u64_be(frame + 28U, session_id.edge_nonce.high);
    write_u64_be(frame + 36U, session_id.edge_nonce.low);
    frame[44] = reason;
    return total;
}

static bool deliver_runtime_and_compare(
    MeisoSession *sender,
    MeisoSession *receiver,
    const uint8_t *expected,
    size_t expected_size
)
{
    uint8_t frame[512];
    size_t frame_size;
    MeisoSessionRuntimeMessage runtime = {0};
    MeisoSessionSnapshot receiver_snapshot;

    CHECK(copy_next_outbound(
        sender, frame, sizeof(frame), &frame_size
    ));
    CHECK(meiso_session_mark_outbound_sent(sender) ==
        MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        receiver,
        (MeisoByteView){frame, frame_size},
        &runtime
    ) == MEISO_SESSION_RECEIVE_RUNTIME);
    CHECK(meiso_session_snapshot(receiver, &receiver_snapshot));
    CHECK(receiver_snapshot.session_id_known);
    CHECK(meiso_session_id_equal(
        runtime.session_id, receiver_snapshot.session_id
    ));
    CHECK(runtime.payload.size == expected_size);
    CHECK(expected_size == 0U || runtime.payload.data != NULL);
    CHECK(expected_size == 0U ||
        memcmp(runtime.payload.data, expected, expected_size) == 0);
    return true;
}

static bool test_value_config_and_arena_contract(void)
{
    MEISO_TEST_ARENA_STORAGE SessionArena arena;
    MEISO_TEST_ARENA_STORAGE SessionArena before;
    MeisoSessionConfig config = host_config();
    MeisoSessionConfig invalid;
    MeisoSessionConfig invalid_before;
    MeisoSession *session = (MeisoSession *)(uintptr_t)1U;
    MeisoSessionSnapshot snapshot;
    const size_t required = meiso_session_memory_required(&config);

    CHECK(MEISO_SESSION_WIRE_VERSION == 1U);
    CHECK(MEISO_SESSION_WIRE_HEADER_SIZE == 12U);
    CHECK(MEISO_SESSION_ID_WIRE_SIZE == 32U);
    CHECK(MEISO_SESSION_MIN_MAX_FRAME_BYTES == 104U);
    CHECK(meiso_session_alignment_required() == _Alignof(max_align_t));

    CHECK(meiso_peer_id_valid(config.local_peer_id));
    CHECK(!meiso_peer_id_valid((MeisoPeerId){0U, 0U}));
    CHECK(meiso_peer_id_equal(config.local_peer_id, config.local_peer_id));
    CHECK(!meiso_peer_id_equal(
        config.local_peer_id, config.expected_peer_id
    ));
    CHECK(meiso_session_nonce_valid(config.local_nonce));
    CHECK(!meiso_session_nonce_valid((MeisoSessionNonce){0U, 0U}));
    CHECK(meiso_protocol_version_valid(config.protocol));
    CHECK(meiso_protocol_version_equal(config.protocol, config.protocol));
    CHECK(meiso_profile_ref_valid(config.profile));
    CHECK(meiso_profile_ref_equal(config.profile, config.profile));
    CHECK(meiso_session_limits_valid(config.limits));
    CHECK(meiso_session_id_equal(
        (MeisoSessionId){config.local_nonce, edge_config().local_nonce},
        (MeisoSessionId){config.local_nonce, edge_config().local_nonce}
    ));

    CHECK(required != 0U && required <= sizeof(arena.bytes));
    CHECK(meiso_session_memory_required(NULL) == 0U);

    invalid = config;
    invalid.local_role = (MeisoSessionRole)0U;
    CHECK(meiso_session_memory_required(&invalid) == 0U);
    invalid = config;
    invalid.local_peer_id = (MeisoPeerId){0U, 0U};
    CHECK(meiso_session_memory_required(&invalid) == 0U);
    invalid = config;
    invalid.local_incarnation = 0U;
    CHECK(meiso_session_memory_required(&invalid) == 0U);
    invalid = config;
    invalid.local_nonce = (MeisoSessionNonce){0U, 0U};
    CHECK(meiso_session_memory_required(&invalid) == 0U);
    invalid = config;
    invalid.expected_peer_id = (MeisoPeerId){0U, 0U};
    CHECK(meiso_session_memory_required(&invalid) == 0U);
    invalid = config;
    invalid.expected_peer_id = invalid.local_peer_id;
    CHECK(meiso_session_memory_required(&invalid) == 0U);
    invalid = config;
    invalid.protocol.major = 0U;
    CHECK(meiso_session_memory_required(&invalid) == 0U);
    invalid = config;
    invalid.profile.id = (MeisoProfileId){0U, 0U};
    CHECK(meiso_session_memory_required(&invalid) == 0U);
    invalid = config;
    invalid.profile.version = 0U;
    CHECK(meiso_session_memory_required(&invalid) == 0U);
    invalid = config;
    invalid.limits.max_frame_bytes = MEISO_SESSION_MIN_MAX_FRAME_BYTES - 1U;
    CHECK(meiso_session_memory_required(&invalid) == 0U);
    invalid = config;
    invalid.limits.max_runtime_payload_bytes = 0U;
    CHECK(meiso_session_memory_required(&invalid) == 0U);
    invalid = config;
    invalid.limits.max_registry_entries = 0U;
    CHECK(meiso_session_memory_required(&invalid) == 0U);
    invalid = config;
    invalid.limits.max_runtime_payload_bytes =
        invalid.limits.max_frame_bytes - MEISO_SESSION_WIRE_HEADER_SIZE -
        MEISO_SESSION_ID_WIRE_SIZE + 1U;
    CHECK(meiso_session_memory_required(&invalid) == 0U);

    CHECK(meiso_session_init(
        arena.bytes, required, &config, NULL
    ) == MEISO_SESSION_INIT_INVALID_ARGUMENT);
    CHECK(meiso_session_init(
        NULL, required, &config, &session
    ) == MEISO_SESSION_INIT_MISALIGNED_ARENA);
    CHECK(session == NULL);
    CHECK(meiso_session_init(
        arena.bytes + 1U, required, &config, &session
    ) == MEISO_SESSION_INIT_MISALIGNED_ARENA);
    CHECK(session == NULL);
    CHECK(meiso_session_init(
        arena.bytes, required - 1U, &config, &session
    ) == MEISO_SESSION_INIT_ARENA_TOO_SMALL);
    CHECK(session == NULL);

    memset(arena.bytes, 0xA5, sizeof(arena.bytes));
    before = arena;
    CHECK(meiso_session_init(
        arena.bytes,
        required,
        &config,
        (MeisoSession **)(void *)arena.bytes
    ) == MEISO_SESSION_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(&arena, &before, sizeof(arena)) == 0);

    invalid = config;
    invalid_before = invalid;
    CHECK(meiso_session_init(
        arena.bytes,
        required,
        &invalid,
        (MeisoSession **)(void *)&invalid
    ) == MEISO_SESSION_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(&invalid, &invalid_before, sizeof(invalid)) == 0);

    CHECK(meiso_session_init(
        arena.bytes, required, &config, &session
    ) == MEISO_SESSION_INIT_OK);
    CHECK(session != NULL);
    CHECK(meiso_session_snapshot(session, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_NEW);
    CHECK(snapshot.failure == MEISO_SESSION_FAILURE_NONE);
    CHECK(snapshot.local_role == config.local_role);
    CHECK(meiso_peer_id_equal(snapshot.local_peer_id, config.local_peer_id));
    CHECK(snapshot.local_incarnation == config.local_incarnation);
    CHECK(!snapshot.remote_identity_known);
    CHECK(!snapshot.session_id_known);
    CHECK(!snapshot.outbound_pending);
    CHECK(!meiso_session_snapshot(NULL, &snapshot));
    CHECK(!meiso_session_snapshot(session, NULL));

    session = NULL;
    CHECK(meiso_session_init(
        arena.bytes, sizeof(arena.bytes), &config, &session
    ) == MEISO_SESSION_INIT_OK);
    CHECK(session != NULL);
    return true;
}

static bool test_golden_open_frame(void)
{
    MEISO_TEST_ARENA_STORAGE SessionArena arena;
    MeisoSessionConfig config = host_config();
    MeisoSession *session;
    MeisoByteView frame;
    MeisoSessionSnapshot snapshot;
    static const uint8_t expected[] = {
        0x4dU, 0x53U, 0x4cU, 0x30U,
        0x01U, 0x01U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x68U,
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
        0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
        0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U, 0x28U,
        0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
        0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
        0x51U, 0x52U, 0x53U, 0x54U, 0x55U, 0x56U, 0x57U, 0x58U,
        0x61U, 0x62U, 0x63U, 0x64U, 0x65U, 0x66U, 0x67U, 0x68U,
        0x01U, 0x02U, 0x03U, 0x04U,
        0x71U, 0x72U, 0x73U, 0x74U, 0x75U, 0x76U, 0x77U, 0x78U,
        0x81U, 0x82U, 0x83U, 0x84U, 0x85U, 0x86U, 0x87U, 0x88U,
        0x91U, 0x92U, 0x93U, 0x94U,
        0x00U, 0x00U, 0x01U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x40U,
        0x00U, 0x00U, 0x00U, 0x04U
    };

    _Static_assert(sizeof(expected) == MEISO_SESSION_MIN_MAX_FRAME_BYTES,
        "golden OPEN size must match the v0 contract");
    CHECK(initialize_session(&arena, &config, &session));
    CHECK(meiso_session_start(session) == MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_snapshot(session, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_HANDSHAKING);
    CHECK(snapshot.outbound_pending);
    CHECK(meiso_session_next_outbound(session, &frame));
    CHECK(frame.size == sizeof(expected));
    CHECK(memcmp(frame.data, expected, sizeof(expected)) == 0);
    CHECK(meiso_session_mark_outbound_sent(session) ==
        MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_mark_outbound_sent(session) ==
        MEISO_SESSION_OUTBOUND_NONE);
    CHECK(!meiso_session_next_outbound(session, &frame));
    return true;
}

static bool test_golden_remaining_frames(void)
{
    MEISO_TEST_ARENA_STORAGE SessionPair pair;
    MEISO_TEST_ARENA_STORAGE SessionArena reject_edge_arena;
    MeisoSessionConfig reject_edge_config = edge_config();
    MeisoSession *reject_edge;
    MeisoSessionRuntimeMessage runtime = {0};
    uint8_t open[MEISO_SESSION_MIN_MAX_FRAME_BYTES] = {0};
    size_t open_size;
    size_t frame_index;
    size_t truncated_size;
    const uint8_t runtime_payload[] = {0xd1U, 0xd2U};
    static const uint8_t accept[] = {
        0x4dU, 0x53U, 0x4cU, 0x30U, 0x01U, TEST_MESSAGE_ACCEPT,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x68U,
        0x51U, 0x52U, 0x53U, 0x54U, 0x55U, 0x56U, 0x57U, 0x58U,
        0x61U, 0x62U, 0x63U, 0x64U, 0x65U, 0x66U, 0x67U, 0x68U,
        0xa1U, 0xa2U, 0xa3U, 0xa4U, 0xa5U, 0xa6U, 0xa7U, 0xa8U,
        0xb1U, 0xb2U, 0xb3U, 0xb4U, 0xb5U, 0xb6U, 0xb7U, 0xb8U,
        0xc1U, 0xc2U, 0xc3U, 0xc4U, 0xc5U, 0xc6U, 0xc7U, 0xc8U,
        0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
        0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
        0x01U, 0x02U, 0x03U, 0x04U,
        0x71U, 0x72U, 0x73U, 0x74U, 0x75U, 0x76U, 0x77U, 0x78U,
        0x81U, 0x82U, 0x83U, 0x84U, 0x85U, 0x86U, 0x87U, 0x88U,
        0x91U, 0x92U, 0x93U, 0x94U,
        0x00U, 0x00U, 0x01U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x40U,
        0x00U, 0x00U, 0x00U, 0x04U
    };
    static const uint8_t confirm[] = {
        0x4dU, 0x53U, 0x4cU, 0x30U, 0x01U, TEST_MESSAGE_CONFIRM,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x2cU,
        0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
        0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
        0xb1U, 0xb2U, 0xb3U, 0xb4U, 0xb5U, 0xb6U, 0xb7U, 0xb8U,
        0xc1U, 0xc2U, 0xc3U, 0xc4U, 0xc5U, 0xc6U, 0xc7U, 0xc8U
    };
    static const uint8_t ready[] = {
        0x4dU, 0x53U, 0x4cU, 0x30U, 0x01U, TEST_MESSAGE_READY,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x2cU,
        0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
        0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
        0xb1U, 0xb2U, 0xb3U, 0xb4U, 0xb5U, 0xb6U, 0xb7U, 0xb8U,
        0xc1U, 0xc2U, 0xc3U, 0xc4U, 0xc5U, 0xc6U, 0xc7U, 0xc8U
    };
    static const uint8_t runtime_frame[] = {
        0x4dU, 0x53U, 0x4cU, 0x30U, 0x01U, TEST_MESSAGE_RUNTIME,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x2eU,
        0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
        0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
        0xb1U, 0xb2U, 0xb3U, 0xb4U, 0xb5U, 0xb6U, 0xb7U, 0xb8U,
        0xc1U, 0xc2U, 0xc3U, 0xc4U, 0xc5U, 0xc6U, 0xc7U, 0xc8U,
        0xd1U, 0xd2U
    };
    static const uint8_t close_frame[] = {
        0x4dU, 0x53U, 0x4cU, 0x30U, 0x01U, TEST_MESSAGE_CLOSE,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x2dU,
        0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
        0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
        0xb1U, 0xb2U, 0xb3U, 0xb4U, 0xb5U, 0xb6U, 0xb7U, 0xb8U,
        0xc1U, 0xc2U, 0xc3U, 0xc4U, 0xc5U, 0xc6U, 0xc7U, 0xc8U,
        0x01U
    };
    static const uint8_t reject[] = {
        0x4dU, 0x53U, 0x4cU, 0x30U, 0x01U, TEST_MESSAGE_REJECT,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x1dU,
        0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U, 0x38U,
        0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U, 0x48U,
        0x07U
    };
    const uint8_t *const truncation_frames[] = {
        accept, confirm, ready, runtime_frame, close_frame, reject
    };
    const size_t truncation_sizes[] = {
        sizeof(accept), sizeof(confirm), sizeof(ready),
        sizeof(runtime_frame), sizeof(close_frame), sizeof(reject)
    };

    _Static_assert(sizeof(accept) == MEISO_SESSION_MIN_MAX_FRAME_BYTES,
        "golden ACCEPT size");
    _Static_assert(sizeof(confirm) == 44U, "golden CONFIRM size");
    _Static_assert(sizeof(ready) == 44U, "golden READY size");
    _Static_assert(sizeof(runtime_frame) == 46U, "golden RUNTIME size");
    _Static_assert(sizeof(close_frame) == 45U, "golden CLOSE size");
    _Static_assert(sizeof(reject) == 29U, "golden REJECT size");
    _Static_assert(MEISO_SESSION_CLOSE_NORMAL == 0x01U,
        "CLOSE reason wire value");
    _Static_assert(MEISO_SESSION_FAILURE_PROTOCOL_MISMATCH == 0x07U,
        "REJECT reason wire value");

    CHECK(initialize_pair(&pair));
    CHECK(meiso_session_start(pair.edge) == MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(pair.host) == MEISO_SESSION_START_STARTED);
    CHECK(deliver_next(
        pair.host, pair.edge, MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(next_outbound_equals(pair.edge, accept, sizeof(accept)));
    CHECK(deliver_next(
        pair.edge, pair.host, MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(next_outbound_equals(pair.host, confirm, sizeof(confirm)));
    CHECK(deliver_next(
        pair.host, pair.edge, MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(next_outbound_equals(pair.edge, ready, sizeof(ready)));
    CHECK(deliver_next(
        pair.edge, pair.host, MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(meiso_session_submit_runtime(
        pair.host, (MeisoByteView){runtime_payload, sizeof(runtime_payload)}
    ) == MEISO_SESSION_SUBMIT_QUEUED);
    CHECK(next_outbound_equals(
        pair.host, runtime_frame, sizeof(runtime_frame)
    ));
    CHECK(deliver_next(
        pair.host, pair.edge, MEISO_SESSION_RECEIVE_RUNTIME
    ));
    CHECK(meiso_session_close(pair.host, MEISO_SESSION_CLOSE_NORMAL) ==
        MEISO_SESSION_CLOSE_QUEUED);
    CHECK(next_outbound_equals(pair.host, close_frame, sizeof(close_frame)));

    CHECK(capture_valid_open(open, sizeof(open), &open_size));
    open[68] ^= 1U;
    CHECK(initialize_session(
        &reject_edge_arena, &reject_edge_config, &reject_edge
    ));
    CHECK(meiso_session_start(reject_edge) == MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_receive(
        reject_edge, (MeisoByteView){open, open_size}, &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(next_outbound_equals(reject_edge, reject, sizeof(reject)));
    for (frame_index = 0U;
         frame_index < sizeof(truncation_frames) / sizeof(truncation_frames[0]);
         ++frame_index) {
        for (truncated_size = 0U;
             truncated_size < truncation_sizes[frame_index];
             ++truncated_size) {
            CHECK(expect_edge_frame_failure(
                truncation_frames[frame_index],
                truncated_size,
                MEISO_SESSION_FAILURE_MALFORMED_FRAME
            ));
        }
    }
    return true;
}

static bool test_complete_four_step_loopback(void)
{
    MEISO_TEST_ARENA_STORAGE SessionPair pair;
    MeisoSessionSnapshot host_snapshot;
    MeisoSessionSnapshot edge_snapshot;

    CHECK(initialize_pair(&pair));
    CHECK(make_ready(&pair));
    CHECK(meiso_session_snapshot(pair.host, &host_snapshot));
    CHECK(meiso_session_snapshot(pair.edge, &edge_snapshot));
    CHECK(host_snapshot.remote_identity_known);
    CHECK(edge_snapshot.remote_identity_known);
    CHECK(host_snapshot.session_id_known);
    CHECK(edge_snapshot.session_id_known);
    CHECK(meiso_peer_id_equal(
        host_snapshot.remote_peer_id, pair.edge_config.local_peer_id
    ));
    CHECK(meiso_peer_id_equal(
        edge_snapshot.remote_peer_id, pair.host_config.local_peer_id
    ));
    CHECK(host_snapshot.remote_incarnation ==
        pair.edge_config.local_incarnation);
    CHECK(edge_snapshot.remote_incarnation ==
        pair.host_config.local_incarnation);
    CHECK(meiso_session_id_equal(
        host_snapshot.session_id, edge_snapshot.session_id
    ));
    CHECK(meiso_session_nonce_valid(host_snapshot.session_id.host_nonce));
    CHECK(meiso_session_nonce_valid(host_snapshot.session_id.edge_nonce));
    CHECK(host_snapshot.negotiated_limits.max_frame_bytes == 256U);
    CHECK(host_snapshot.negotiated_limits.max_runtime_payload_bytes == 64U);
    CHECK(host_snapshot.negotiated_limits.max_registry_entries == 4U);
    CHECK(memcmp(
        &host_snapshot.negotiated_limits,
        &edge_snapshot.negotiated_limits,
        sizeof(host_snapshot.negotiated_limits)
    ) == 0);
    CHECK(!host_snapshot.outbound_pending);
    CHECK(!edge_snapshot.outbound_pending);
    return true;
}

static bool test_asymmetric_limit_negotiation(void)
{
    MEISO_TEST_ARENA_STORAGE SessionPair pair;
    MeisoSessionSnapshot host_snapshot;
    MeisoSessionSnapshot edge_snapshot;

    memset(&pair, 0, sizeof(pair));
    pair.host_config = host_config();
    pair.edge_config = edge_config();
    pair.host_config.limits = (MeisoSessionLimits){512U, 128U, 8U};
    pair.edge_config.limits = (MeisoSessionLimits){256U, 32U, 3U};
    CHECK(initialize_session(
        &pair.host_arena, &pair.host_config, &pair.host
    ));
    CHECK(initialize_session(
        &pair.edge_arena, &pair.edge_config, &pair.edge
    ));
    CHECK(make_ready(&pair));
    CHECK(meiso_session_snapshot(pair.host, &host_snapshot));
    CHECK(meiso_session_snapshot(pair.edge, &edge_snapshot));
    CHECK(host_snapshot.negotiated_limits.max_frame_bytes == 256U);
    CHECK(host_snapshot.negotiated_limits.max_runtime_payload_bytes == 32U);
    CHECK(host_snapshot.negotiated_limits.max_registry_entries == 3U);
    CHECK(memcmp(
        &host_snapshot.negotiated_limits,
        &edge_snapshot.negotiated_limits,
        sizeof(host_snapshot.negotiated_limits)
    ) == 0);
    return true;
}

static bool expect_open_failure(
    const MeisoSessionConfig *edge_side_config,
    MeisoSessionFailure expected_failure
)
{
    MEISO_TEST_ARENA_STORAGE SessionArena host_arena;
    MEISO_TEST_ARENA_STORAGE SessionArena edge_arena;
    MeisoSessionConfig host_side_config = host_config();
    MeisoSession *host;
    MeisoSession *edge;

    CHECK(initialize_session(&host_arena, &host_side_config, &host));
    CHECK(initialize_session(&edge_arena, edge_side_config, &edge));
    CHECK(meiso_session_start(edge) == MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(host) == MEISO_SESSION_START_STARTED);
    CHECK(deliver_next(host, edge, MEISO_SESSION_RECEIVE_FAILED));
    CHECK(snapshot_is(edge, MEISO_SESSION_STATE_FAILED, expected_failure));
    return true;
}

static bool test_negotiation_failures(void)
{
    MeisoSessionConfig config;
    MEISO_TEST_ARENA_STORAGE SessionArena host_arena;
    MEISO_TEST_ARENA_STORAGE SessionArena edge_arena;
    MeisoSessionConfig host_side_config = host_config();
    MeisoSessionConfig edge_side_config = edge_config();
    MeisoSession *host;
    MeisoSession *edge;
    MeisoSessionSnapshot snapshot;
    uint8_t frame[512];
    size_t frame_size;

    config = edge_config();
    config.expected_peer_id.low += 1U;
    CHECK(expect_open_failure(&config, MEISO_SESSION_FAILURE_PEER_MISMATCH));

    config = edge_config();
    config.protocol.major += 1U;
    CHECK(expect_open_failure(
        &config, MEISO_SESSION_FAILURE_PROTOCOL_MISMATCH
    ));

    config = edge_config();
    config.profile.version += 1U;
    CHECK(expect_open_failure(
        &config, MEISO_SESSION_FAILURE_PROFILE_MISMATCH
    ));

    CHECK(initialize_session(&host_arena, &host_side_config, &host));
    CHECK(initialize_session(&edge_arena, &edge_side_config, &edge));
    CHECK(meiso_session_start(edge) == MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(host) == MEISO_SESSION_START_STARTED);
    CHECK(copy_next_outbound(host, frame, sizeof(frame), &frame_size));
    CHECK(frame_size == MEISO_SESSION_MIN_MAX_FRAME_BYTES);
    write_u32_be(frame + 92U, MEISO_SESSION_MIN_MAX_FRAME_BYTES - 1U);
    CHECK(meiso_session_mark_outbound_sent(host) ==
        MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        edge,
        (MeisoByteView){frame, frame_size},
        &(MeisoSessionRuntimeMessage){0}
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(meiso_session_snapshot(edge, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_FAILED);
    CHECK(snapshot.failure == MEISO_SESSION_FAILURE_LIMITS_INCOMPATIBLE);
    return true;
}

static bool capture_valid_open(uint8_t *frame, size_t capacity, size_t *out_size)
{
    MEISO_TEST_ARENA_STORAGE SessionArena arena;
    MeisoSessionConfig config = host_config();
    MeisoSession *host;

    CHECK(initialize_session(&arena, &config, &host));
    CHECK(meiso_session_start(host) == MEISO_SESSION_START_STARTED);
    CHECK(copy_next_outbound(host, frame, capacity, out_size));
    return true;
}

static bool capture_valid_accept(
    uint8_t *frame,
    size_t capacity,
    size_t *out_size
)
{
    MEISO_TEST_ARENA_STORAGE SessionPair pair;
    uint8_t open[512];
    size_t open_size;
    MeisoSessionRuntimeMessage runtime = {0};

    CHECK(initialize_pair(&pair));
    CHECK(meiso_session_start(pair.edge) == MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(pair.host) == MEISO_SESSION_START_STARTED);
    CHECK(copy_next_outbound(pair.host, open, sizeof(open), &open_size));
    CHECK(meiso_session_mark_outbound_sent(pair.host) ==
        MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        pair.edge, (MeisoByteView){open, open_size}, &runtime
    ) == MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS);
    CHECK(copy_next_outbound(pair.edge, frame, capacity, out_size));
    return true;
}

static bool expect_host_accept_failure(
    const uint8_t *frame,
    size_t frame_size,
    MeisoSessionFailure expected_failure
)
{
    MEISO_TEST_ARENA_STORAGE SessionArena arena;
    MeisoSessionConfig config = host_config();
    MeisoSession *host;
    uint8_t open[512];
    size_t open_size;
    MeisoSessionSnapshot snapshot;
    MeisoSessionRuntimeMessage runtime = {0};

    CHECK(initialize_session(&arena, &config, &host));
    CHECK(meiso_session_start(host) == MEISO_SESSION_START_STARTED);
    CHECK(copy_next_outbound(host, open, sizeof(open), &open_size));
    CHECK(meiso_session_mark_outbound_sent(host) ==
        MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        host, (MeisoByteView){frame, frame_size}, &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(meiso_session_snapshot(host, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_FAILED);
    CHECK(snapshot.failure == expected_failure);
    return true;
}

static bool test_accept_validation_axes(void)
{
    uint8_t valid[512];
    uint8_t changed[512];
    size_t valid_size;

    CHECK(capture_valid_accept(valid, sizeof(valid), &valid_size));
    CHECK(valid_size == MEISO_SESSION_MIN_MAX_FRAME_BYTES);

    memmove(changed, valid, valid_size);
    changed[27] ^= 1U;
    CHECK(expect_host_accept_failure(
        changed, valid_size, MEISO_SESSION_FAILURE_PEER_MISMATCH
    ));

    memmove(changed, valid, valid_size);
    memset(changed + 28U, 0, 8U);
    CHECK(expect_host_accept_failure(
        changed, valid_size, MEISO_SESSION_FAILURE_INVALID_INCARNATION
    ));

    memmove(changed, valid, valid_size);
    memset(changed + 36U, 0, 16U);
    CHECK(expect_host_accept_failure(
        changed, valid_size, MEISO_SESSION_FAILURE_INVALID_NONCE
    ));

    memmove(changed, valid, valid_size);
    changed[52] ^= 1U;
    CHECK(expect_host_accept_failure(
        changed, valid_size, MEISO_SESSION_FAILURE_SESSION_MISMATCH
    ));

    memmove(changed, valid, valid_size);
    changed[68] ^= 1U;
    CHECK(expect_host_accept_failure(
        changed, valid_size, MEISO_SESSION_FAILURE_PROTOCOL_MISMATCH
    ));

    memmove(changed, valid, valid_size);
    changed[91] ^= 1U;
    CHECK(expect_host_accept_failure(
        changed, valid_size, MEISO_SESSION_FAILURE_PROFILE_MISMATCH
    ));

    memmove(changed, valid, valid_size);
    write_u32_be(changed + 92U, MEISO_SESSION_MIN_MAX_FRAME_BYTES - 1U);
    CHECK(expect_host_accept_failure(
        changed, valid_size, MEISO_SESSION_FAILURE_LIMITS_INCOMPATIBLE
    ));
    return true;
}

static bool test_session_id_and_order_validation(void)
{
    MEISO_TEST_ARENA_STORAGE SessionPair confirm_pair;
    MEISO_TEST_ARENA_STORAGE SessionPair ready_pair;
    MEISO_TEST_ARENA_STORAGE SessionPair duplicate_pair;
    uint8_t frame[512];
    uint8_t duplicate_open[512];
    size_t frame_size;
    size_t duplicate_open_size;
    MeisoSessionRuntimeMessage runtime = {0};

    CHECK(initialize_pair(&confirm_pair));
    CHECK(meiso_session_start(confirm_pair.edge) ==
        MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(confirm_pair.host) ==
        MEISO_SESSION_START_STARTED);
    CHECK(deliver_next(
        confirm_pair.host,
        confirm_pair.edge,
        MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(deliver_next(
        confirm_pair.edge,
        confirm_pair.host,
        MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(copy_next_outbound(
        confirm_pair.host, frame, sizeof(frame), &frame_size
    ));
    CHECK(meiso_session_mark_outbound_sent(confirm_pair.host) ==
        MEISO_SESSION_OUTBOUND_SENT);
    frame[12] ^= 1U;
    CHECK(meiso_session_receive(
        confirm_pair.edge,
        (MeisoByteView){frame, frame_size},
        &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(snapshot_is(
        confirm_pair.edge,
        MEISO_SESSION_STATE_FAILED,
        MEISO_SESSION_FAILURE_SESSION_MISMATCH
    ));

    CHECK(initialize_pair(&ready_pair));
    CHECK(meiso_session_start(ready_pair.edge) ==
        MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(ready_pair.host) ==
        MEISO_SESSION_START_STARTED);
    CHECK(deliver_next(
        ready_pair.host, ready_pair.edge,
        MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(deliver_next(
        ready_pair.edge, ready_pair.host,
        MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(deliver_next(
        ready_pair.host, ready_pair.edge,
        MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(copy_next_outbound(
        ready_pair.edge, frame, sizeof(frame), &frame_size
    ));
    CHECK(meiso_session_mark_outbound_sent(ready_pair.edge) ==
        MEISO_SESSION_OUTBOUND_SENT);
    frame[43] ^= 1U;
    CHECK(meiso_session_receive(
        ready_pair.host,
        (MeisoByteView){frame, frame_size},
        &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(snapshot_is(
        ready_pair.host,
        MEISO_SESSION_STATE_FAILED,
        MEISO_SESSION_FAILURE_SESSION_MISMATCH
    ));

    CHECK(initialize_pair(&duplicate_pair));
    CHECK(meiso_session_start(duplicate_pair.edge) ==
        MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(duplicate_pair.host) ==
        MEISO_SESSION_START_STARTED);
    CHECK(copy_next_outbound(
        duplicate_pair.host,
        duplicate_open,
        sizeof(duplicate_open),
        &duplicate_open_size
    ));
    CHECK(meiso_session_mark_outbound_sent(duplicate_pair.host) ==
        MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        duplicate_pair.edge,
        (MeisoByteView){duplicate_open, duplicate_open_size},
        &runtime
    ) == MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS);
    CHECK(meiso_session_receive(
        duplicate_pair.edge,
        (MeisoByteView){duplicate_open, duplicate_open_size},
        &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(snapshot_is(
        duplicate_pair.edge,
        MEISO_SESSION_STATE_FAILED,
        MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE
    ));
    return true;
}

static bool test_handshake_skip_order(void)
{
    MEISO_TEST_ARENA_STORAGE SessionArena host_arena;
    MEISO_TEST_ARENA_STORAGE SessionArena edge_arena;
    MeisoSessionConfig host_side_config = host_config();
    MeisoSessionConfig edge_side_config = edge_config();
    MeisoSession *host;
    MeisoSession *edge;
    MeisoSessionRuntimeMessage runtime = {0};
    uint8_t frame[64];
    size_t frame_size;
    const MeisoSessionId unknown = {{1U, 1U}, {2U, 2U}};

    CHECK(initialize_session(&host_arena, &host_side_config, &host));
    CHECK(meiso_session_start(host) == MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_mark_outbound_sent(host) ==
        MEISO_SESSION_OUTBOUND_SENT);
    frame_size = build_session_only_frame(
        frame, sizeof(frame), TEST_MESSAGE_READY, unknown
    );
    CHECK(frame_size != 0U);
    CHECK(meiso_session_receive(
        host, (MeisoByteView){frame, frame_size}, &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(snapshot_is(
        host,
        MEISO_SESSION_STATE_FAILED,
        MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE
    ));

    CHECK(initialize_session(&edge_arena, &edge_side_config, &edge));
    CHECK(meiso_session_start(edge) == MEISO_SESSION_START_STARTED);
    frame_size = build_session_only_frame(
        frame, sizeof(frame), TEST_MESSAGE_CONFIRM, unknown
    );
    CHECK(frame_size != 0U);
    CHECK(meiso_session_receive(
        edge, (MeisoByteView){frame, frame_size}, &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(snapshot_is(
        edge,
        MEISO_SESSION_STATE_FAILED,
        MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE
    ));
    return true;
}

static bool expect_edge_frame_failure(
    const uint8_t *frame,
    size_t frame_size,
    MeisoSessionFailure expected_failure
)
{
    MEISO_TEST_ARENA_STORAGE SessionArena arena;
    MeisoSessionConfig config = edge_config();
    MeisoSession *edge;
    MeisoSessionSnapshot snapshot;
    MeisoSessionRuntimeMessage runtime = {0};

    CHECK(initialize_session(&arena, &config, &edge));
    CHECK(meiso_session_start(edge) == MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_receive(
        edge, (MeisoByteView){frame, frame_size}, &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(meiso_session_snapshot(edge, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_FAILED);
    CHECK(snapshot.failure == expected_failure);
    return true;
}

static bool test_malformed_and_unexpected_frames(void)
{
    MEISO_TEST_ARENA_STORAGE SessionArena edge_arena;
    MEISO_TEST_ARENA_STORAGE SessionArena overlap_frame;
    MEISO_TEST_ARENA_STORAGE SessionArena overlap_before;
    MeisoSessionConfig edge_side_config = edge_config();
    MeisoSession *edge;
    MeisoSessionSnapshot snapshot;
    MeisoSessionRuntimeMessage invalid_runtime;
    MeisoSessionRuntimeMessage invalid_runtime_before;
    uint8_t valid[512];
    uint8_t changed[512];
    size_t valid_size;
    size_t size;

    CHECK(capture_valid_open(valid, sizeof(valid), &valid_size));
    CHECK(valid_size == MEISO_SESSION_MIN_MAX_FRAME_BYTES);

    CHECK(initialize_session(&edge_arena, &edge_side_config, &edge));
    CHECK(meiso_session_start(edge) == MEISO_SESSION_START_STARTED);
    memset(&overlap_frame, 0, sizeof(overlap_frame));
    memmove(overlap_frame.bytes, valid, valid_size);
    overlap_before = overlap_frame;
    CHECK(meiso_session_receive(
        edge,
        (MeisoByteView){overlap_frame.bytes, valid_size},
        (MeisoSessionRuntimeMessage *)(void *)overlap_frame.bytes
    ) == MEISO_SESSION_RECEIVE_INVALID_ARGUMENT);
    CHECK(memcmp(&overlap_frame, &overlap_before, sizeof(overlap_frame)) == 0);
    CHECK(meiso_session_snapshot(edge, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_HANDSHAKING);

    memset(&invalid_runtime, 0xa5, sizeof(invalid_runtime));
    invalid_runtime_before = invalid_runtime;
    CHECK(meiso_session_receive(
        edge,
        (MeisoByteView){NULL, 1U},
        &invalid_runtime
    ) == MEISO_SESSION_RECEIVE_INVALID_ARGUMENT);
    CHECK(memcmp(
        &invalid_runtime, &invalid_runtime_before, sizeof(invalid_runtime)
    ) == 0);
    CHECK(meiso_session_snapshot(edge, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_HANDSHAKING);

    for (size = 0U; size < valid_size; ++size) {
        CHECK(expect_edge_frame_failure(
            valid, size, MEISO_SESSION_FAILURE_MALFORMED_FRAME
        ));
    }

    memmove(changed, valid, valid_size);
    changed[0] = (uint8_t)'X';
    CHECK(expect_edge_frame_failure(
        changed, valid_size, MEISO_SESSION_FAILURE_MALFORMED_FRAME
    ));

    memmove(changed, valid, valid_size);
    changed[4] = (uint8_t)(MEISO_SESSION_WIRE_VERSION + 1U);
    CHECK(expect_edge_frame_failure(
        changed, valid_size, MEISO_SESSION_FAILURE_MALFORMED_FRAME
    ));

    memmove(changed, valid, valid_size);
    changed[6] = 1U;
    CHECK(expect_edge_frame_failure(
        changed, valid_size, MEISO_SESSION_FAILURE_MALFORMED_FRAME
    ));
    memmove(changed, valid, valid_size);
    changed[7] = 1U;
    CHECK(expect_edge_frame_failure(
        changed, valid_size, MEISO_SESSION_FAILURE_MALFORMED_FRAME
    ));

    memmove(changed, valid, valid_size);
    write_u32_be(changed + 8U, (uint32_t)(valid_size + 1U));
    CHECK(expect_edge_frame_failure(
        changed, valid_size, MEISO_SESSION_FAILURE_MALFORMED_FRAME
    ));

    memmove(changed, valid, valid_size);
    changed[5] = 0x7fU;
    CHECK(expect_edge_frame_failure(
        changed, valid_size, MEISO_SESSION_FAILURE_MALFORMED_FRAME
    ));

    memmove(changed, valid, valid_size);
    changed[5] = TEST_MESSAGE_ACCEPT;
    CHECK(expect_edge_frame_failure(
        changed, valid_size, MEISO_SESSION_FAILURE_ROLE_MISMATCH
    ));
    return true;
}

static bool test_ready_gate_and_early_runtime(void)
{
    MEISO_TEST_ARENA_STORAGE SessionPair pair;
    MeisoSessionSnapshot snapshot;
    MeisoByteView outbound;
    uint8_t frame[128];
    const uint8_t payload[] = {0x5aU};
    size_t frame_size;
    MeisoSessionRuntimeMessage runtime = {0};

    CHECK(initialize_pair(&pair));
    CHECK(meiso_session_submit_runtime(
        pair.host, (MeisoByteView){payload, sizeof(payload)}
    ) == MEISO_SESSION_SUBMIT_NOT_READY);
    CHECK(meiso_session_submit_runtime(
        pair.edge, (MeisoByteView){NULL, 0U}
    ) == MEISO_SESSION_SUBMIT_NOT_READY);
    CHECK(!meiso_session_next_outbound(pair.host, &outbound));
    CHECK(!meiso_session_next_outbound(pair.edge, &outbound));

    CHECK(meiso_session_start(pair.edge) == MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(pair.host) == MEISO_SESSION_START_STARTED);
    CHECK(deliver_next(
        pair.host, pair.edge, MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(deliver_next(
        pair.edge, pair.host, MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(deliver_next(
        pair.host, pair.edge, MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS
    ));
    CHECK(meiso_session_snapshot(pair.host, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_HANDSHAKING);
    CHECK(snapshot.session_id_known);
    frame_size = build_runtime_frame(
        frame, sizeof(frame), snapshot.session_id, payload, sizeof(payload)
    );
    CHECK(frame_size != 0U);
    CHECK(meiso_session_receive(
        pair.host,
        (MeisoByteView){frame, frame_size},
        &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(snapshot_is(
        pair.host,
        MEISO_SESSION_STATE_FAILED,
        MEISO_SESSION_FAILURE_EARLY_RUNTIME
    ));
    CHECK(runtime.payload.size == 0U);
    return true;
}

static bool test_runtime_payload_bounds(void)
{
    MEISO_TEST_ARENA_STORAGE SessionPair pair;
    uint8_t exact[64];
    uint8_t expected[64];
    uint8_t oversized[65];
    MeisoByteView frame;
    size_t index;

    CHECK(initialize_pair(&pair));
    CHECK(make_ready(&pair));

    CHECK(meiso_session_submit_runtime(
        pair.host, (MeisoByteView){NULL, 0U}
    ) == MEISO_SESSION_SUBMIT_QUEUED);
    CHECK(deliver_runtime_and_compare(pair.host, pair.edge, NULL, 0U));

    for (index = 0U; index < sizeof(exact); ++index) {
        exact[index] = (uint8_t)(index ^ 0xA5U);
        expected[index] = exact[index];
    }
    CHECK(meiso_session_submit_runtime(
        pair.edge, (MeisoByteView){exact, sizeof(exact)}
    ) == MEISO_SESSION_SUBMIT_QUEUED);
    memset(exact, 0U, sizeof(exact));
    CHECK(deliver_runtime_and_compare(
        pair.edge, pair.host, expected, sizeof(expected)
    ));

    memset(oversized, 0x5a, sizeof(oversized));
    CHECK(meiso_session_submit_runtime(
        pair.host, (MeisoByteView){oversized, sizeof(oversized)}
    ) == MEISO_SESSION_SUBMIT_CAPACITY);
    CHECK(!meiso_session_next_outbound(pair.host, &frame));
    CHECK(meiso_session_submit_runtime(
        pair.host, (MeisoByteView){NULL, 1U}
    ) == MEISO_SESSION_SUBMIT_INVALID_ARGUMENT);
    CHECK(!meiso_session_next_outbound(pair.host, &frame));
    return true;
}

static bool test_old_session_rejected_and_new_nonce_changes_id(void)
{
    MEISO_TEST_ARENA_STORAGE SessionPair first;
    MEISO_TEST_ARENA_STORAGE SessionPair second;
    MeisoSessionSnapshot first_host;
    MeisoSessionSnapshot second_host;
    MeisoSessionSnapshot second_edge;
    const uint8_t payload[] = {0x31U, 0x32U};
    uint8_t old_frame[512];
    size_t old_frame_size;
    MeisoSessionRuntimeMessage runtime = {0};

    CHECK(initialize_pair(&first));
    CHECK(make_ready(&first));
    CHECK(meiso_session_snapshot(first.host, &first_host));
    CHECK(meiso_session_submit_runtime(
        first.host, (MeisoByteView){payload, sizeof(payload)}
    ) == MEISO_SESSION_SUBMIT_QUEUED);
    CHECK(copy_next_outbound(
        first.host, old_frame, sizeof(old_frame), &old_frame_size
    ));
    CHECK(meiso_session_mark_outbound_sent(first.host) ==
        MEISO_SESSION_OUTBOUND_SENT);

    CHECK(initialize_pair(&second));
    second.host_config.local_nonce.low += 1U;
    second.edge_config.local_nonce.low += 1U;
    CHECK(initialize_session(
        &second.host_arena, &second.host_config, &second.host
    ));
    CHECK(initialize_session(
        &second.edge_arena, &second.edge_config, &second.edge
    ));
    CHECK(make_ready(&second));
    CHECK(meiso_session_snapshot(second.host, &second_host));
    CHECK(!meiso_session_id_equal(
        first_host.session_id, second_host.session_id
    ));
    CHECK(meiso_peer_id_equal(
        first_host.local_peer_id, second_host.local_peer_id
    ));
    CHECK(first_host.local_incarnation == second_host.local_incarnation);

    CHECK(meiso_session_receive(
        second.edge,
        (MeisoByteView){old_frame, old_frame_size},
        &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(meiso_session_snapshot(second.edge, &second_edge));
    CHECK(second_edge.state == MEISO_SESSION_STATE_FAILED);
    CHECK(second_edge.failure == MEISO_SESSION_FAILURE_SESSION_MISMATCH);
    CHECK(runtime.payload.size == 0U);
    return true;
}

static bool generate_protocol_reject(
    SessionPair *pair,
    uint8_t *out_frame,
    size_t capacity,
    size_t *out_size
)
{
    uint8_t open[512];
    size_t open_size;
    MeisoSessionRuntimeMessage runtime = {0};

    CHECK(initialize_pair(pair));
    CHECK(meiso_session_start(pair->edge) == MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(pair->host) == MEISO_SESSION_START_STARTED);
    CHECK(copy_next_outbound(
        pair->host, open, sizeof(open), &open_size
    ));
    CHECK(meiso_session_mark_outbound_sent(pair->host) ==
        MEISO_SESSION_OUTBOUND_SENT);
    open[68] ^= 1U;
    CHECK(meiso_session_receive(
        pair->edge, (MeisoByteView){open, open_size}, &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(copy_next_outbound(pair->edge, out_frame, capacity, out_size));
    return true;
}

static bool test_reject_binding_and_order(void)
{
    MEISO_TEST_ARENA_STORAGE SessionPair accepted;
    MEISO_TEST_ARENA_STORAGE SessionPair stale;
    MEISO_TEST_ARENA_STORAGE SessionPair invalid_reason;
    MEISO_TEST_ARENA_STORAGE SessionPair ready;
    MEISO_TEST_ARENA_STORAGE SessionPair lost;
    uint8_t reject[128];
    uint8_t stale_reject[128];
    size_t reject_size;
    size_t stale_size;
    MeisoSessionRuntimeMessage runtime = {0};

    CHECK(generate_protocol_reject(
        &accepted, reject, sizeof(reject), &reject_size
    ));
    CHECK(meiso_session_mark_outbound_sent(accepted.edge) ==
        MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        accepted.host, (MeisoByteView){reject, reject_size}, &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(snapshot_is(
        accepted.host,
        MEISO_SESSION_STATE_FAILED,
        MEISO_SESSION_FAILURE_PROTOCOL_MISMATCH
    ));

    CHECK(generate_protocol_reject(
        &stale, stale_reject, sizeof(stale_reject), &stale_size
    ));
    stale_reject[12] ^= 1U;
    CHECK(meiso_session_receive(
        stale.host, (MeisoByteView){stale_reject, stale_size}, &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(snapshot_is(
        stale.host,
        MEISO_SESSION_STATE_FAILED,
        MEISO_SESSION_FAILURE_SESSION_MISMATCH
    ));

    CHECK(generate_protocol_reject(
        &invalid_reason, stale_reject, sizeof(stale_reject), &stale_size
    ));
    stale_reject[28] = MEISO_SESSION_FAILURE_TRANSPORT_LOST;
    CHECK(meiso_session_receive(
        invalid_reason.host,
        (MeisoByteView){stale_reject, stale_size},
        &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(snapshot_is(
        invalid_reason.host,
        MEISO_SESSION_STATE_FAILED,
        MEISO_SESSION_FAILURE_MALFORMED_FRAME
    ));

    CHECK(initialize_pair(&ready));
    CHECK(make_ready(&ready));
    CHECK(meiso_session_receive(
        ready.host, (MeisoByteView){reject, reject_size}, &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(snapshot_is(
        ready.host,
        MEISO_SESSION_STATE_FAILED,
        MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE
    ));

    CHECK(generate_protocol_reject(
        &lost, stale_reject, sizeof(stale_reject), &stale_size
    ));
    meiso_session_transport_lost(lost.edge);
    CHECK(!meiso_session_next_outbound(
        lost.edge, &(MeisoByteView){NULL, 0U}
    ));
    CHECK(snapshot_is(
        lost.edge,
        MEISO_SESSION_STATE_FAILED,
        MEISO_SESSION_FAILURE_PROTOCOL_MISMATCH
    ));
    return true;
}

static bool test_simultaneous_close(void)
{
    MEISO_TEST_ARENA_STORAGE SessionPair pair;
    uint8_t host_close[128];
    uint8_t edge_close[128];
    size_t host_close_size;
    size_t edge_close_size;
    MeisoSessionRuntimeMessage runtime = {0};
    MeisoSessionSnapshot host_snapshot;
    MeisoSessionSnapshot edge_snapshot;

    CHECK(initialize_pair(&pair));
    CHECK(make_ready(&pair));
    CHECK(meiso_session_close(pair.host, MEISO_SESSION_CLOSE_NORMAL) ==
        MEISO_SESSION_CLOSE_QUEUED);
    CHECK(meiso_session_close(pair.edge, MEISO_SESSION_CLOSE_SHUTDOWN) ==
        MEISO_SESSION_CLOSE_QUEUED);
    CHECK(copy_next_outbound(
        pair.host, host_close, sizeof(host_close), &host_close_size
    ));
    CHECK(copy_next_outbound(
        pair.edge, edge_close, sizeof(edge_close), &edge_close_size
    ));
    CHECK(meiso_session_mark_outbound_sent(pair.host) ==
        MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_mark_outbound_sent(pair.edge) ==
        MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        pair.host, (MeisoByteView){edge_close, edge_close_size}, &runtime
    ) == MEISO_SESSION_RECEIVE_PEER_CLOSED);
    CHECK(meiso_session_receive(
        pair.edge, (MeisoByteView){host_close, host_close_size}, &runtime
    ) == MEISO_SESSION_RECEIVE_PEER_CLOSED);
    CHECK(meiso_session_snapshot(pair.host, &host_snapshot));
    CHECK(meiso_session_snapshot(pair.edge, &edge_snapshot));
    CHECK(host_snapshot.state == MEISO_SESSION_STATE_CLOSED);
    CHECK(edge_snapshot.state == MEISO_SESSION_STATE_CLOSED);
    CHECK(host_snapshot.failure == MEISO_SESSION_FAILURE_LOCAL_CLOSED);
    CHECK(edge_snapshot.failure == MEISO_SESSION_FAILURE_LOCAL_CLOSED);
    CHECK(host_snapshot.local_close_reason_known);
    CHECK(host_snapshot.peer_close_reason_known);
    CHECK(host_snapshot.local_close_reason == MEISO_SESSION_CLOSE_NORMAL);
    CHECK(host_snapshot.peer_close_reason == MEISO_SESSION_CLOSE_SHUTDOWN);
    CHECK(edge_snapshot.local_close_reason == MEISO_SESSION_CLOSE_SHUTDOWN);
    CHECK(edge_snapshot.peer_close_reason == MEISO_SESSION_CLOSE_NORMAL);
    edge_close[44] = MEISO_SESSION_CLOSE_PROTOCOL_ERROR;
    CHECK(meiso_session_receive(
        pair.host, (MeisoByteView){edge_close, edge_close_size}, &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(meiso_session_snapshot(pair.host, &host_snapshot));
    CHECK(host_snapshot.state == MEISO_SESSION_STATE_CLOSED);
    CHECK(host_snapshot.failure == MEISO_SESSION_FAILURE_LOCAL_CLOSED);
    CHECK(host_snapshot.peer_close_reason == MEISO_SESSION_CLOSE_SHUTDOWN);
    return true;
}

static bool test_close_and_transport_loss(void)
{
    MEISO_TEST_ARENA_STORAGE SessionPair pair;
    MEISO_TEST_ARENA_STORAGE SessionPair lost_pair;
    MEISO_TEST_ARENA_STORAGE SessionArena local_only_arena;
    MeisoSessionConfig local_only_config = host_config();
    MeisoSession *local_only;
    const uint8_t payload[] = {0x01U};
    uint8_t unsolicited_close[64];
    size_t unsolicited_close_size;
    MeisoByteView frame;
    MeisoSessionRuntimeMessage runtime = {0};
    MeisoSessionSnapshot snapshot;

    CHECK(initialize_pair(&pair));
    CHECK(make_ready(&pair));
    CHECK(meiso_session_close(pair.host, MEISO_SESSION_CLOSE_NORMAL) ==
        MEISO_SESSION_CLOSE_QUEUED);
    CHECK(snapshot_is(
        pair.host, MEISO_SESSION_STATE_CLOSING, MEISO_SESSION_FAILURE_NONE
    ));
    CHECK(deliver_next(
        pair.host, pair.edge, MEISO_SESSION_RECEIVE_PEER_CLOSED
    ));
    CHECK(snapshot_is(
        pair.host,
        MEISO_SESSION_STATE_CLOSED,
        MEISO_SESSION_FAILURE_LOCAL_CLOSED
    ));
    CHECK(snapshot_is(
        pair.edge,
        MEISO_SESSION_STATE_CLOSED,
        MEISO_SESSION_FAILURE_PEER_CLOSED
    ));
    CHECK(meiso_session_close(pair.host, MEISO_SESSION_CLOSE_NORMAL) ==
        MEISO_SESSION_CLOSE_ALREADY_TERMINAL);
    CHECK(meiso_session_submit_runtime(
        pair.host, (MeisoByteView){payload, sizeof(payload)}
    ) == MEISO_SESSION_SUBMIT_NOT_READY);
    CHECK(meiso_session_submit_runtime(
        pair.edge, (MeisoByteView){payload, sizeof(payload)}
    ) == MEISO_SESSION_SUBMIT_NOT_READY);
    CHECK(!meiso_session_next_outbound(pair.host, &frame));
    CHECK(!meiso_session_next_outbound(pair.edge, &frame));

    CHECK(initialize_pair(&lost_pair));
    CHECK(make_ready(&lost_pair));
    meiso_session_transport_lost(lost_pair.edge);
    CHECK(snapshot_is(
        lost_pair.edge,
        MEISO_SESSION_STATE_FAILED,
        MEISO_SESSION_FAILURE_TRANSPORT_LOST
    ));
    CHECK(meiso_session_submit_runtime(
        lost_pair.edge, (MeisoByteView){payload, sizeof(payload)}
    ) == MEISO_SESSION_SUBMIT_NOT_READY);

    CHECK(initialize_session(
        &local_only_arena, &local_only_config, &local_only
    ));
    CHECK(meiso_session_close(
        local_only, MEISO_SESSION_CLOSE_SHUTDOWN
    ) == MEISO_SESSION_CLOSE_LOCAL_ONLY);
    unsolicited_close_size = build_close_frame(
        unsolicited_close,
        sizeof(unsolicited_close),
        (MeisoSessionId){{0U, 0U}, {0U, 0U}},
        MEISO_SESSION_CLOSE_NORMAL
    );
    CHECK(unsolicited_close_size != 0U);
    CHECK(meiso_session_receive(
        local_only,
        (MeisoByteView){unsolicited_close, unsolicited_close_size},
        &runtime
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(meiso_session_snapshot(local_only, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_CLOSED);
    CHECK(snapshot.failure == MEISO_SESSION_FAILURE_LOCAL_CLOSED);
    CHECK(snapshot.local_close_reason_known);
    CHECK(snapshot.local_close_reason == MEISO_SESSION_CLOSE_SHUTDOWN);
    CHECK(!snapshot.peer_close_reason_known);
    meiso_session_transport_lost(NULL);
    return true;
}

int main(void)
{
    if (!test_value_config_and_arena_contract() ||
        !test_golden_open_frame() ||
        !test_golden_remaining_frames() ||
        !test_complete_four_step_loopback() ||
        !test_asymmetric_limit_negotiation() ||
        !test_negotiation_failures() ||
        !test_accept_validation_axes() ||
        !test_session_id_and_order_validation() ||
        !test_handshake_skip_order() ||
        !test_malformed_and_unexpected_frames() ||
        !test_ready_gate_and_early_runtime() ||
        !test_runtime_payload_bounds() ||
        !test_old_session_rejected_and_new_nonce_changes_id() ||
        !test_reject_binding_and_order() ||
        !test_simultaneous_close() ||
        !test_close_and_transport_loss()) {
        return 1;
    }
    (void)puts("session tests passed");
    return 0;
}
