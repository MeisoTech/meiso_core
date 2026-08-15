#include "meiso_session.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    WIRE_TYPE_OPEN = 1,
    WIRE_TYPE_ACCEPT = 2,
    WIRE_TYPE_CONFIRM = 3,
    WIRE_TYPE_READY = 4,
    WIRE_TYPE_REJECT = 5,
    WIRE_TYPE_CLOSE = 6,
    WIRE_TYPE_RUNTIME = 7
};

enum {
    PHASE_NEW = 1,
    PHASE_HOST_OPEN_PENDING = 2,
    PHASE_HOST_WAIT_ACCEPT = 3,
    PHASE_HOST_CONFIRM_PENDING = 4,
    PHASE_HOST_WAIT_READY = 5,
    PHASE_EDGE_WAIT_OPEN = 6,
    PHASE_EDGE_ACCEPT_PENDING = 7,
    PHASE_EDGE_WAIT_CONFIRM = 8,
    PHASE_EDGE_READY_PENDING = 9,
    PHASE_READY = 10,
    PHASE_CLOSE_PENDING = 11,
    PHASE_TERMINAL = 12
};

enum {
    OPEN_PAYLOAD_SIZE = 92,
    ACCEPT_PAYLOAD_SIZE = 92,
    SESSION_PAYLOAD_SIZE = 32,
    REJECT_PAYLOAD_SIZE = 17,
    CLOSE_PAYLOAD_SIZE = 33,
    RUNTIME_PREFIX_SIZE = 32
};

struct MeisoSession {
    MeisoSessionConfig config;
    MeisoPeerId remote_peer_id;
    uint64_t remote_incarnation;
    MeisoSessionNonce remote_nonce;
    MeisoSessionNonce handshake_host_nonce;
    MeisoSessionId session_id;
    MeisoSessionLimits negotiated_limits;
    size_t arena_size;
    size_t tx_offset;
    size_t rx_offset;
    size_t tx_size;
    size_t rx_size;
    MeisoSessionState state;
    MeisoSessionFailure failure;
    MeisoSessionCloseReason local_close_reason;
    MeisoSessionCloseReason peer_close_reason;
    uint8_t phase;
    uint8_t tx_type;
    bool remote_identity_known;
    bool handshake_host_nonce_known;
    bool session_id_known;
    bool tx_pending;
    bool local_close_reason_known;
    bool peer_close_reason_known;
    bool session_bound_close_started;
    uint8_t storage[];
};

static bool role_valid(MeisoSessionRole role)
{
    return role == MEISO_SESSION_ROLE_HOST || role == MEISO_SESSION_ROLE_EDGE;
}

static bool close_reason_valid(MeisoSessionCloseReason reason)
{
    return reason == MEISO_SESSION_CLOSE_NORMAL ||
        reason == MEISO_SESSION_CLOSE_SHUTDOWN ||
        reason == MEISO_SESSION_CLOSE_PROTOCOL_ERROR;
}

static bool failure_wire_valid(uint8_t value)
{
    return value >= MEISO_SESSION_FAILURE_MALFORMED_FRAME &&
        value <= MEISO_SESSION_FAILURE_EARLY_RUNTIME;
}

static bool add_size(size_t lhs, size_t rhs, size_t *out)
{
    if (out == NULL || rhs > SIZE_MAX - lhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

static bool ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
{
    const uintptr_t lhs_start = (uintptr_t)lhs;
    const uintptr_t rhs_start = (uintptr_t)rhs;
    uintptr_t lhs_end;
    uintptr_t rhs_end;

    if (lhs == NULL || rhs == NULL || lhs_size == 0U || rhs_size == 0U) {
        return false;
    }
    if (lhs_size > UINTPTR_MAX - lhs_start ||
        rhs_size > UINTPTR_MAX - rhs_start) {
        return true;
    }
    lhs_end = lhs_start + lhs_size;
    rhs_end = rhs_start + rhs_size;
    return lhs_start < rhs_end && rhs_start < lhs_end;
}

static bool config_valid(const MeisoSessionConfig *config)
{
    return config != NULL && role_valid(config->local_role) &&
        meiso_peer_id_valid(config->local_peer_id) &&
        config->local_incarnation != 0U &&
        meiso_session_nonce_valid(config->local_nonce) &&
        meiso_peer_id_valid(config->expected_peer_id) &&
        !meiso_peer_id_equal(config->local_peer_id, config->expected_peer_id) &&
        meiso_protocol_version_valid(config->protocol) &&
        meiso_profile_ref_valid(config->profile) &&
        meiso_session_limits_valid(config->limits);
}

static bool session_valid(const MeisoSession *session)
{
    size_t tx_end;
    size_t rx_end;

    if (session == NULL || !config_valid(&session->config) ||
        session->arena_size < sizeof(*session) ||
        !add_size(session->tx_offset, session->config.limits.max_frame_bytes,
                  &tx_end) ||
        !add_size(session->rx_offset,
                  session->config.limits.max_runtime_payload_bytes, &rx_end) ||
        tx_end > session->arena_size || rx_end > session->arena_size ||
        session->tx_size > session->config.limits.max_frame_bytes ||
        session->rx_size > session->config.limits.max_runtime_payload_bytes) {
        return false;
    }
    return session->state >= MEISO_SESSION_STATE_NEW &&
        session->state <= MEISO_SESSION_STATE_FAILED;
}

static uint8_t *tx_bytes(MeisoSession *session)
{
    return (uint8_t *)session + session->tx_offset;
}

static const uint8_t *tx_bytes_const(const MeisoSession *session)
{
    return (const uint8_t *)session + session->tx_offset;
}

static uint8_t *rx_bytes(MeisoSession *session)
{
    return (uint8_t *)session + session->rx_offset;
}

static void put_u16(uint8_t *target, uint16_t value)
{
    target[0] = (uint8_t)(value >> 8U);
    target[1] = (uint8_t)value;
}

static void put_u32(uint8_t *target, uint32_t value)
{
    target[0] = (uint8_t)(value >> 24U);
    target[1] = (uint8_t)(value >> 16U);
    target[2] = (uint8_t)(value >> 8U);
    target[3] = (uint8_t)value;
}

static void put_u64(uint8_t *target, uint64_t value)
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

static uint16_t get_u16(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] << 8U) |
        (uint16_t)source[1];
}

static uint32_t get_u32(const uint8_t *source)
{
    return (uint32_t)source[0] << 24U |
        (uint32_t)source[1] << 16U |
        (uint32_t)source[2] << 8U |
        (uint32_t)source[3];
}

static uint64_t get_u64(const uint8_t *source)
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

static void put_id128(uint8_t *target, uint64_t high, uint64_t low)
{
    put_u64(target, high);
    put_u64(target + 8U, low);
}

static void put_peer_id(uint8_t *target, MeisoPeerId value)
{
    put_id128(target, value.high, value.low);
}

static MeisoPeerId get_peer_id(const uint8_t *source)
{
    const MeisoPeerId value = {get_u64(source), get_u64(source + 8U)};
    return value;
}

static void put_nonce(uint8_t *target, MeisoSessionNonce value)
{
    put_id128(target, value.high, value.low);
}

static MeisoSessionNonce get_nonce(const uint8_t *source)
{
    const MeisoSessionNonce value = {get_u64(source), get_u64(source + 8U)};
    return value;
}

static bool nonce_equal(MeisoSessionNonce lhs, MeisoSessionNonce rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

static void put_session_id(uint8_t *target, MeisoSessionId value)
{
    put_nonce(target, value.host_nonce);
    put_nonce(target + 16U, value.edge_nonce);
}

static MeisoSessionId get_session_id(const uint8_t *source)
{
    MeisoSessionId value;
    value.host_nonce = get_nonce(source);
    value.edge_nonce = get_nonce(source + 16U);
    return value;
}

static void put_protocol(uint8_t *target, MeisoProtocolVersion value)
{
    put_u16(target, value.major);
    put_u16(target + 2U, value.minor);
}

static MeisoProtocolVersion get_protocol(const uint8_t *source)
{
    const MeisoProtocolVersion value = {
        get_u16(source), get_u16(source + 2U)
    };
    return value;
}

static void put_profile(uint8_t *target, MeisoProfileRef value)
{
    put_id128(target, value.id.high, value.id.low);
    put_u32(target + 16U, value.version);
}

static MeisoProfileRef get_profile(const uint8_t *source)
{
    MeisoProfileRef value;
    value.id.high = get_u64(source);
    value.id.low = get_u64(source + 8U);
    value.version = get_u32(source + 16U);
    return value;
}

static void put_limits(uint8_t *target, MeisoSessionLimits value)
{
    put_u32(target, value.max_frame_bytes);
    put_u32(target + 4U, value.max_runtime_payload_bytes);
    put_u32(target + 8U, value.max_registry_entries);
}

static MeisoSessionLimits get_limits(const uint8_t *source)
{
    const MeisoSessionLimits value = {
        get_u32(source), get_u32(source + 4U), get_u32(source + 8U)
    };
    return value;
}

static uint32_t minimum_u32(uint32_t lhs, uint32_t rhs)
{
    return lhs < rhs ? lhs : rhs;
}

static MeisoSessionLimits negotiate_limits(
    MeisoSessionLimits local,
    MeisoSessionLimits remote
)
{
    const MeisoSessionLimits value = {
        minimum_u32(local.max_frame_bytes, remote.max_frame_bytes),
        minimum_u32(local.max_runtime_payload_bytes,
                    remote.max_runtime_payload_bytes),
        minimum_u32(local.max_registry_entries, remote.max_registry_entries)
    };
    return value;
}

static void encode_header(uint8_t *target, uint8_t type, uint32_t size)
{
    target[0] = (uint8_t)'M';
    target[1] = (uint8_t)'S';
    target[2] = (uint8_t)'L';
    target[3] = (uint8_t)'0';
    target[4] = (uint8_t)MEISO_SESSION_WIRE_VERSION;
    target[5] = type;
    target[6] = 0U;
    target[7] = 0U;
    put_u32(target + 8U, size);
}

static void set_queued(MeisoSession *session, uint8_t type, size_t size)
{
    session->tx_type = type;
    session->tx_size = size;
    session->tx_pending = true;
}

static void queue_open(MeisoSession *session)
{
    uint8_t *frame = tx_bytes(session);
    uint8_t *payload = frame + MEISO_SESSION_WIRE_HEADER_SIZE;

    put_peer_id(payload, session->config.local_peer_id);
    put_u64(payload + 16U, session->config.local_incarnation);
    put_nonce(payload + 24U, session->config.local_nonce);
    put_peer_id(payload + 40U, session->config.expected_peer_id);
    put_protocol(payload + 56U, session->config.protocol);
    put_profile(payload + 60U, session->config.profile);
    put_limits(payload + 80U, session->config.limits);
    encode_header(frame, WIRE_TYPE_OPEN,
                  MEISO_SESSION_WIRE_HEADER_SIZE + OPEN_PAYLOAD_SIZE);
    set_queued(session, WIRE_TYPE_OPEN,
               MEISO_SESSION_WIRE_HEADER_SIZE + OPEN_PAYLOAD_SIZE);
}

static void queue_accept(MeisoSession *session, MeisoSessionNonce host_nonce)
{
    uint8_t *frame = tx_bytes(session);
    uint8_t *payload = frame + MEISO_SESSION_WIRE_HEADER_SIZE;

    put_peer_id(payload, session->config.local_peer_id);
    put_u64(payload + 16U, session->config.local_incarnation);
    put_nonce(payload + 24U, session->config.local_nonce);
    put_nonce(payload + 40U, host_nonce);
    put_protocol(payload + 56U, session->config.protocol);
    put_profile(payload + 60U, session->config.profile);
    put_limits(payload + 80U, session->config.limits);
    encode_header(frame, WIRE_TYPE_ACCEPT,
                  MEISO_SESSION_WIRE_HEADER_SIZE + ACCEPT_PAYLOAD_SIZE);
    set_queued(session, WIRE_TYPE_ACCEPT,
               MEISO_SESSION_WIRE_HEADER_SIZE + ACCEPT_PAYLOAD_SIZE);
}

static void queue_session_only(MeisoSession *session, uint8_t type)
{
    uint8_t *frame = tx_bytes(session);
    put_session_id(frame + MEISO_SESSION_WIRE_HEADER_SIZE, session->session_id);
    encode_header(frame, type,
                  MEISO_SESSION_WIRE_HEADER_SIZE + SESSION_PAYLOAD_SIZE);
    set_queued(session, type,
               MEISO_SESSION_WIRE_HEADER_SIZE + SESSION_PAYLOAD_SIZE);
}

static void queue_reject(MeisoSession *session, MeisoSessionFailure failure)
{
    uint8_t *frame = tx_bytes(session);
    uint8_t *payload = frame + MEISO_SESSION_WIRE_HEADER_SIZE;

    put_nonce(payload, session->handshake_host_nonce);
    payload[16] = failure;
    encode_header(frame, WIRE_TYPE_REJECT,
                  MEISO_SESSION_WIRE_HEADER_SIZE + REJECT_PAYLOAD_SIZE);
    set_queued(session, WIRE_TYPE_REJECT,
               MEISO_SESSION_WIRE_HEADER_SIZE + REJECT_PAYLOAD_SIZE);
}

static void clear_outbound(MeisoSession *session)
{
    session->tx_pending = false;
    session->tx_type = 0U;
    session->tx_size = 0U;
}

static void fail_session(
    MeisoSession *session,
    MeisoSessionFailure failure,
    bool send_reject
)
{
    const bool can_reject = send_reject &&
        session->state == MEISO_SESSION_STATE_HANDSHAKING &&
        session->handshake_host_nonce_known;

    if (session->state == MEISO_SESSION_STATE_CLOSED ||
        session->state == MEISO_SESSION_STATE_FAILED) {
        return;
    }
    clear_outbound(session);
    session->state = MEISO_SESSION_STATE_FAILED;
    session->failure = failure;
    session->phase = PHASE_TERMINAL;
    if (can_reject) {
        queue_reject(session, failure);
    }
}

static bool frame_header_valid(
    const MeisoSession *session,
    MeisoByteView frame,
    uint8_t *out_type
)
{
    uint32_t claimed_size;

    if (!meiso_byte_view_valid(frame) || frame.data == NULL ||
        frame.size < MEISO_SESSION_WIRE_HEADER_SIZE ||
        frame.size > session->config.limits.max_frame_bytes ||
        frame.data[0] != (uint8_t)'M' || frame.data[1] != (uint8_t)'S' ||
        frame.data[2] != (uint8_t)'L' || frame.data[3] != (uint8_t)'0' ||
        frame.data[4] != (uint8_t)MEISO_SESSION_WIRE_VERSION ||
        frame.data[6] != 0U || frame.data[7] != 0U) {
        return false;
    }
    claimed_size = get_u32(frame.data + 8U);
    if ((size_t)claimed_size != frame.size) {
        return false;
    }
    *out_type = frame.data[5];
    return *out_type >= WIRE_TYPE_OPEN && *out_type <= WIRE_TYPE_RUNTIME;
}

static MeisoSessionReceiveStatus receive_open(
    MeisoSession *session,
    const uint8_t *payload,
    size_t payload_size
)
{
    MeisoPeerId remote_peer;
    uint64_t remote_incarnation;
    MeisoSessionNonce remote_nonce;
    MeisoPeerId expected_edge;
    MeisoProtocolVersion protocol;
    MeisoProfileRef profile;
    MeisoSessionLimits limits;

    if (session->config.local_role != MEISO_SESSION_ROLE_EDGE) {
        fail_session(session, MEISO_SESSION_FAILURE_ROLE_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (session->phase != PHASE_EDGE_WAIT_OPEN ||
        payload_size != OPEN_PAYLOAD_SIZE) {
        fail_session(session, MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    remote_peer = get_peer_id(payload);
    remote_incarnation = get_u64(payload + 16U);
    remote_nonce = get_nonce(payload + 24U);
    expected_edge = get_peer_id(payload + 40U);
    protocol = get_protocol(payload + 56U);
    profile = get_profile(payload + 60U);
    limits = get_limits(payload + 80U);

    if (!meiso_peer_id_equal(remote_peer, session->config.expected_peer_id) ||
        !meiso_peer_id_equal(expected_edge, session->config.local_peer_id)) {
        fail_session(session, MEISO_SESSION_FAILURE_PEER_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_session_nonce_valid(remote_nonce)) {
        fail_session(session, MEISO_SESSION_FAILURE_INVALID_NONCE, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    session->handshake_host_nonce = remote_nonce;
    session->handshake_host_nonce_known = true;
    if (remote_incarnation == 0U) {
        fail_session(session, MEISO_SESSION_FAILURE_INVALID_INCARNATION, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_protocol_version_equal(protocol, session->config.protocol)) {
        fail_session(session, MEISO_SESSION_FAILURE_PROTOCOL_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_profile_ref_equal(profile, session->config.profile)) {
        fail_session(session, MEISO_SESSION_FAILURE_PROFILE_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_session_limits_valid(limits)) {
        fail_session(session, MEISO_SESSION_FAILURE_LIMITS_INCOMPATIBLE, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }

    session->remote_peer_id = remote_peer;
    session->remote_incarnation = remote_incarnation;
    session->remote_nonce = remote_nonce;
    session->remote_identity_known = true;
    session->session_id.host_nonce = remote_nonce;
    session->session_id.edge_nonce = session->config.local_nonce;
    session->session_id_known = true;
    session->negotiated_limits = negotiate_limits(session->config.limits, limits);
    queue_accept(session, remote_nonce);
    session->phase = PHASE_EDGE_ACCEPT_PENDING;
    return MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS;
}

static MeisoSessionReceiveStatus receive_accept(
    MeisoSession *session,
    const uint8_t *payload,
    size_t payload_size
)
{
    MeisoPeerId remote_peer;
    uint64_t remote_incarnation;
    MeisoSessionNonce remote_nonce;
    MeisoSessionNonce echoed_nonce;
    MeisoProtocolVersion protocol;
    MeisoProfileRef profile;
    MeisoSessionLimits limits;

    if (session->config.local_role != MEISO_SESSION_ROLE_HOST) {
        fail_session(session, MEISO_SESSION_FAILURE_ROLE_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (session->phase != PHASE_HOST_WAIT_ACCEPT ||
        payload_size != ACCEPT_PAYLOAD_SIZE) {
        fail_session(session, MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    remote_peer = get_peer_id(payload);
    remote_incarnation = get_u64(payload + 16U);
    remote_nonce = get_nonce(payload + 24U);
    echoed_nonce = get_nonce(payload + 40U);
    protocol = get_protocol(payload + 56U);
    profile = get_profile(payload + 60U);
    limits = get_limits(payload + 80U);

    if (!meiso_peer_id_equal(remote_peer, session->config.expected_peer_id)) {
        fail_session(session, MEISO_SESSION_FAILURE_PEER_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (remote_incarnation == 0U) {
        fail_session(session, MEISO_SESSION_FAILURE_INVALID_INCARNATION, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_session_nonce_valid(remote_nonce)) {
        fail_session(session, MEISO_SESSION_FAILURE_INVALID_NONCE, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!nonce_equal(echoed_nonce, session->config.local_nonce)) {
        fail_session(session, MEISO_SESSION_FAILURE_SESSION_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_protocol_version_equal(protocol, session->config.protocol)) {
        fail_session(session, MEISO_SESSION_FAILURE_PROTOCOL_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_profile_ref_equal(profile, session->config.profile)) {
        fail_session(session, MEISO_SESSION_FAILURE_PROFILE_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_session_limits_valid(limits)) {
        fail_session(session, MEISO_SESSION_FAILURE_LIMITS_INCOMPATIBLE, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }

    session->remote_peer_id = remote_peer;
    session->remote_incarnation = remote_incarnation;
    session->remote_nonce = remote_nonce;
    session->remote_identity_known = true;
    session->session_id.host_nonce = session->config.local_nonce;
    session->session_id.edge_nonce = remote_nonce;
    session->session_id_known = true;
    session->negotiated_limits = negotiate_limits(session->config.limits, limits);
    queue_session_only(session, WIRE_TYPE_CONFIRM);
    session->phase = PHASE_HOST_CONFIRM_PENDING;
    return MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS;
}

static MeisoSessionReceiveStatus receive_confirm(
    MeisoSession *session,
    const uint8_t *payload,
    size_t payload_size
)
{
    MeisoSessionId received;

    if (session->config.local_role != MEISO_SESSION_ROLE_EDGE) {
        fail_session(session, MEISO_SESSION_FAILURE_ROLE_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (session->phase != PHASE_EDGE_WAIT_CONFIRM ||
        payload_size != SESSION_PAYLOAD_SIZE) {
        fail_session(session, MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    received = get_session_id(payload);
    if (!meiso_session_id_equal(received, session->session_id)) {
        fail_session(session, MEISO_SESSION_FAILURE_SESSION_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    queue_session_only(session, WIRE_TYPE_READY);
    session->phase = PHASE_EDGE_READY_PENDING;
    return MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS;
}

static MeisoSessionReceiveStatus receive_ready(
    MeisoSession *session,
    const uint8_t *payload,
    size_t payload_size
)
{
    MeisoSessionId received;

    if (session->config.local_role != MEISO_SESSION_ROLE_HOST) {
        fail_session(session, MEISO_SESSION_FAILURE_ROLE_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (session->phase != PHASE_HOST_WAIT_READY ||
        payload_size != SESSION_PAYLOAD_SIZE) {
        fail_session(session, MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    received = get_session_id(payload);
    if (!meiso_session_id_equal(received, session->session_id)) {
        fail_session(session, MEISO_SESSION_FAILURE_SESSION_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    session->state = MEISO_SESSION_STATE_READY;
    session->phase = PHASE_READY;
    return MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS;
}

static MeisoSessionReceiveStatus receive_reject(
    MeisoSession *session,
    const uint8_t *payload,
    size_t payload_size
)
{
    MeisoSessionNonce received_nonce;
    const bool phase_valid =
        (session->config.local_role == MEISO_SESSION_ROLE_HOST &&
         (session->phase == PHASE_HOST_WAIT_ACCEPT ||
          session->phase == PHASE_HOST_WAIT_READY)) ||
        (session->config.local_role == MEISO_SESSION_ROLE_EDGE &&
         session->phase == PHASE_EDGE_WAIT_CONFIRM);

    if (session->state != MEISO_SESSION_STATE_HANDSHAKING || !phase_valid ||
        !session->handshake_host_nonce_known) {
        fail_session(session, MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE, false);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (payload_size != REJECT_PAYLOAD_SIZE || !failure_wire_valid(payload[16])) {
        fail_session(session, MEISO_SESSION_FAILURE_MALFORMED_FRAME, false);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    received_nonce = get_nonce(payload);
    if (!nonce_equal(received_nonce, session->handshake_host_nonce)) {
        fail_session(session, MEISO_SESSION_FAILURE_SESSION_MISMATCH, false);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    fail_session(session, payload[16], false);
    return MEISO_SESSION_RECEIVE_FAILED;
}

static MeisoSessionReceiveStatus receive_close(
    MeisoSession *session,
    const uint8_t *payload,
    size_t payload_size
)
{
    MeisoSessionId received;
    const bool locally_closing = session->session_bound_close_started &&
        (session->state == MEISO_SESSION_STATE_CLOSING ||
        (session->state == MEISO_SESSION_STATE_CLOSED &&
         session->failure == MEISO_SESSION_FAILURE_LOCAL_CLOSED));

    if ((session->state != MEISO_SESSION_STATE_READY && !locally_closing) ||
        payload_size != CLOSE_PAYLOAD_SIZE ||
        !close_reason_valid(payload[SESSION_PAYLOAD_SIZE])) {
        fail_session(session, MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    received = get_session_id(payload);
    if (!meiso_session_id_equal(received, session->session_id)) {
        fail_session(session, MEISO_SESSION_FAILURE_SESSION_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    clear_outbound(session);
    session->session_bound_close_started = false;
    session->peer_close_reason = payload[SESSION_PAYLOAD_SIZE];
    session->peer_close_reason_known = true;
    session->state = MEISO_SESSION_STATE_CLOSED;
    session->failure = locally_closing ? MEISO_SESSION_FAILURE_LOCAL_CLOSED :
        MEISO_SESSION_FAILURE_PEER_CLOSED;
    session->phase = PHASE_TERMINAL;
    return MEISO_SESSION_RECEIVE_PEER_CLOSED;
}

static MeisoSessionReceiveStatus receive_runtime(
    MeisoSession *session,
    const uint8_t *payload,
    size_t payload_size,
    MeisoSessionRuntimeMessage *out_runtime
)
{
    MeisoSessionId received;
    size_t runtime_size;

    if (session->state != MEISO_SESSION_STATE_READY || session->phase != PHASE_READY) {
        fail_session(session, MEISO_SESSION_FAILURE_EARLY_RUNTIME, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (payload_size < RUNTIME_PREFIX_SIZE) {
        fail_session(session, MEISO_SESSION_FAILURE_MALFORMED_FRAME, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    received = get_session_id(payload);
    if (!meiso_session_id_equal(received, session->session_id)) {
        fail_session(session, MEISO_SESSION_FAILURE_SESSION_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    runtime_size = payload_size - RUNTIME_PREFIX_SIZE;
    if (runtime_size > session->negotiated_limits.max_runtime_payload_bytes) {
        fail_session(session, MEISO_SESSION_FAILURE_LIMITS_INCOMPATIBLE, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (runtime_size != 0U) {
        memmove(rx_bytes(session), payload + RUNTIME_PREFIX_SIZE, runtime_size);
    }
    session->rx_size = runtime_size;
    out_runtime->payload.data = runtime_size == 0U ? NULL : rx_bytes(session);
    out_runtime->payload.size = runtime_size;
    out_runtime->session_id = received;
    return MEISO_SESSION_RECEIVE_RUNTIME;
}

bool meiso_peer_id_valid(MeisoPeerId value)
{
    return value.high != 0U || value.low != 0U;
}

bool meiso_peer_id_equal(MeisoPeerId lhs, MeisoPeerId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

bool meiso_session_nonce_valid(MeisoSessionNonce value)
{
    return value.high != 0U || value.low != 0U;
}

bool meiso_session_id_equal(MeisoSessionId lhs, MeisoSessionId rhs)
{
    return nonce_equal(lhs.host_nonce, rhs.host_nonce) &&
        nonce_equal(lhs.edge_nonce, rhs.edge_nonce);
}

bool meiso_profile_ref_valid(MeisoProfileRef value)
{
    return (value.id.high != 0U || value.id.low != 0U) && value.version != 0U;
}

bool meiso_profile_ref_equal(MeisoProfileRef lhs, MeisoProfileRef rhs)
{
    return lhs.id.high == rhs.id.high && lhs.id.low == rhs.id.low &&
        lhs.version == rhs.version;
}

bool meiso_protocol_version_valid(MeisoProtocolVersion value)
{
    return value.major != 0U;
}

bool meiso_protocol_version_equal(
    MeisoProtocolVersion lhs,
    MeisoProtocolVersion rhs
)
{
    return lhs.major == rhs.major && lhs.minor == rhs.minor;
}

bool meiso_session_limits_valid(MeisoSessionLimits value)
{
    return value.max_frame_bytes >= MEISO_SESSION_MIN_MAX_FRAME_BYTES &&
        value.max_runtime_payload_bytes != 0U &&
        value.max_runtime_payload_bytes <=
            value.max_frame_bytes -
                (MEISO_SESSION_WIRE_HEADER_SIZE + MEISO_SESSION_ID_WIRE_SIZE) &&
        value.max_registry_entries != 0U;
}

size_t meiso_session_alignment_required(void)
{
    return _Alignof(max_align_t);
}

size_t meiso_session_memory_required(const MeisoSessionConfig *config)
{
    size_t required;

    if (!config_valid(config) ||
        !add_size(sizeof(MeisoSession), config->limits.max_frame_bytes,
                  &required) ||
        !add_size(required, config->limits.max_runtime_payload_bytes,
                  &required)) {
        return 0U;
    }
    return required;
}

MeisoSessionInitStatus meiso_session_init(
    void *arena,
    size_t arena_size,
    const MeisoSessionConfig *config,
    MeisoSession **out_session
)
{
    MeisoSessionConfig config_copy;
    MeisoSession *session;
    size_t required;

    if (out_session == NULL) {
        return MEISO_SESSION_INIT_INVALID_ARGUMENT;
    }
    if ((arena != NULL && ranges_overlap(
            arena, arena_size, out_session, sizeof(*out_session)
        )) ||
        (config != NULL && ranges_overlap(
            config, sizeof(*config), out_session, sizeof(*out_session)
        ))) {
        return MEISO_SESSION_INIT_INVALID_ARGUMENT;
    }
    *out_session = NULL;
    if (!config_valid(config)) {
        return MEISO_SESSION_INIT_INVALID_CONFIG;
    }
    if (arena == NULL ||
        (uintptr_t)arena % meiso_session_alignment_required() != 0U) {
        return MEISO_SESSION_INIT_MISALIGNED_ARENA;
    }
    required = meiso_session_memory_required(config);
    if (required == 0U || arena_size < required) {
        return MEISO_SESSION_INIT_ARENA_TOO_SMALL;
    }

    config_copy = *config;
    memset(arena, 0, required);
    session = arena;
    session->config = config_copy;
    session->arena_size = required;
    session->tx_offset = sizeof(*session);
    session->rx_offset = sizeof(*session) + config_copy.limits.max_frame_bytes;
    session->state = MEISO_SESSION_STATE_NEW;
    session->failure = MEISO_SESSION_FAILURE_NONE;
    session->phase = PHASE_NEW;
    if (config_copy.local_role == MEISO_SESSION_ROLE_HOST) {
        session->handshake_host_nonce = config_copy.local_nonce;
        session->handshake_host_nonce_known = true;
    }
    *out_session = session;
    return MEISO_SESSION_INIT_OK;
}

MeisoSessionStartStatus meiso_session_start(MeisoSession *session)
{
    if (!session_valid(session)) {
        return MEISO_SESSION_START_INVALID;
    }
    if (session->state != MEISO_SESSION_STATE_NEW || session->phase != PHASE_NEW) {
        return MEISO_SESSION_START_ALREADY_STARTED;
    }
    session->state = MEISO_SESSION_STATE_HANDSHAKING;
    if (session->config.local_role == MEISO_SESSION_ROLE_HOST) {
        queue_open(session);
        session->phase = PHASE_HOST_OPEN_PENDING;
    } else {
        session->phase = PHASE_EDGE_WAIT_OPEN;
    }
    return MEISO_SESSION_START_STARTED;
}

bool meiso_session_next_outbound(
    const MeisoSession *session,
    MeisoByteView *out_frame
)
{
    if (!session_valid(session) || out_frame == NULL || ranges_overlap(
        session, session->arena_size, out_frame, sizeof(*out_frame)
    )) {
        return false;
    }
    out_frame->data = NULL;
    out_frame->size = 0U;
    if (!session->tx_pending) {
        return false;
    }
    out_frame->data = tx_bytes_const(session);
    out_frame->size = session->tx_size;
    return true;
}

MeisoSessionOutboundStatus meiso_session_mark_outbound_sent(
    MeisoSession *session
)
{
    uint8_t sent_type;

    if (!session_valid(session)) {
        return MEISO_SESSION_OUTBOUND_INVALID;
    }
    if (!session->tx_pending) {
        return MEISO_SESSION_OUTBOUND_NONE;
    }
    sent_type = session->tx_type;
    clear_outbound(session);

    if (sent_type == WIRE_TYPE_OPEN && session->phase == PHASE_HOST_OPEN_PENDING) {
        session->phase = PHASE_HOST_WAIT_ACCEPT;
    } else if (sent_type == WIRE_TYPE_ACCEPT &&
               session->phase == PHASE_EDGE_ACCEPT_PENDING) {
        session->phase = PHASE_EDGE_WAIT_CONFIRM;
    } else if (sent_type == WIRE_TYPE_CONFIRM &&
               session->phase == PHASE_HOST_CONFIRM_PENDING) {
        session->phase = PHASE_HOST_WAIT_READY;
    } else if (sent_type == WIRE_TYPE_READY &&
               session->phase == PHASE_EDGE_READY_PENDING) {
        session->state = MEISO_SESSION_STATE_READY;
        session->phase = PHASE_READY;
    } else if (sent_type == WIRE_TYPE_CLOSE &&
               session->phase == PHASE_CLOSE_PENDING) {
        session->state = MEISO_SESSION_STATE_CLOSED;
        session->failure = MEISO_SESSION_FAILURE_LOCAL_CLOSED;
        session->phase = PHASE_TERMINAL;
    } else if (sent_type != WIRE_TYPE_RUNTIME && sent_type != WIRE_TYPE_REJECT) {
        fail_session(session, MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE, false);
        return MEISO_SESSION_OUTBOUND_INVALID;
    }
    return MEISO_SESSION_OUTBOUND_SENT;
}

MeisoSessionReceiveStatus meiso_session_receive(
    MeisoSession *session,
    MeisoByteView frame,
    MeisoSessionRuntimeMessage *out_runtime
)
{
    uint8_t type;
    const uint8_t *payload;
    size_t payload_size;

    if (!session_valid(session) || out_runtime == NULL ||
        !meiso_byte_view_valid(frame) ||
        ranges_overlap(session, session->arena_size,
                       out_runtime, sizeof(*out_runtime)) ||
        (frame.data != NULL && ranges_overlap(
            frame.data, frame.size, out_runtime, sizeof(*out_runtime)
        ))) {
        return MEISO_SESSION_RECEIVE_INVALID_ARGUMENT;
    }
    out_runtime->payload.data = NULL;
    out_runtime->payload.size = 0U;
    memset(&out_runtime->session_id, 0, sizeof(out_runtime->session_id));
    if (session->state == MEISO_SESSION_STATE_FAILED ||
        !frame_header_valid(session, frame, &type)) {
        if (session->state != MEISO_SESSION_STATE_CLOSED &&
            session->state != MEISO_SESSION_STATE_FAILED) {
            fail_session(session, MEISO_SESSION_FAILURE_MALFORMED_FRAME, false);
        }
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (session->state == MEISO_SESSION_STATE_CLOSED &&
        !(type == WIRE_TYPE_CLOSE &&
          session->failure == MEISO_SESSION_FAILURE_LOCAL_CLOSED &&
          session->session_bound_close_started)) {
        return MEISO_SESSION_RECEIVE_FAILED;
    }

    payload = frame.data + MEISO_SESSION_WIRE_HEADER_SIZE;
    payload_size = frame.size - MEISO_SESSION_WIRE_HEADER_SIZE;
    switch (type) {
    case WIRE_TYPE_OPEN:
        return receive_open(session, payload, payload_size);
    case WIRE_TYPE_ACCEPT:
        return receive_accept(session, payload, payload_size);
    case WIRE_TYPE_CONFIRM:
        return receive_confirm(session, payload, payload_size);
    case WIRE_TYPE_READY:
        return receive_ready(session, payload, payload_size);
    case WIRE_TYPE_REJECT:
        return receive_reject(session, payload, payload_size);
    case WIRE_TYPE_CLOSE:
        return receive_close(session, payload, payload_size);
    case WIRE_TYPE_RUNTIME:
        return receive_runtime(session, payload, payload_size, out_runtime);
    default:
        fail_session(session, MEISO_SESSION_FAILURE_MALFORMED_FRAME, false);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
}

MeisoSessionSubmitStatus meiso_session_submit_runtime(
    MeisoSession *session,
    MeisoByteView payload
)
{
    uint8_t *frame;
    size_t frame_size;

    if (!session_valid(session) || !meiso_byte_view_valid(payload)) {
        return MEISO_SESSION_SUBMIT_INVALID_ARGUMENT;
    }
    if (session->state != MEISO_SESSION_STATE_READY || session->phase != PHASE_READY) {
        return MEISO_SESSION_SUBMIT_NOT_READY;
    }
    if (session->tx_pending) {
        return MEISO_SESSION_SUBMIT_TX_BUSY;
    }
    if (payload.size > session->negotiated_limits.max_runtime_payload_bytes ||
        payload.size > UINT32_MAX -
            (MEISO_SESSION_WIRE_HEADER_SIZE + RUNTIME_PREFIX_SIZE)) {
        return MEISO_SESSION_SUBMIT_CAPACITY;
    }
    frame_size = MEISO_SESSION_WIRE_HEADER_SIZE + RUNTIME_PREFIX_SIZE + payload.size;
    if (frame_size > session->negotiated_limits.max_frame_bytes) {
        return MEISO_SESSION_SUBMIT_CAPACITY;
    }

    frame = tx_bytes(session);
    if (payload.size != 0U) {
        memmove(frame + MEISO_SESSION_WIRE_HEADER_SIZE + RUNTIME_PREFIX_SIZE,
                payload.data, payload.size);
    }
    put_session_id(frame + MEISO_SESSION_WIRE_HEADER_SIZE, session->session_id);
    encode_header(frame, WIRE_TYPE_RUNTIME, (uint32_t)frame_size);
    set_queued(session, WIRE_TYPE_RUNTIME, frame_size);
    return MEISO_SESSION_SUBMIT_QUEUED;
}

MeisoSessionCloseStatus meiso_session_close(
    MeisoSession *session,
    MeisoSessionCloseReason reason
)
{
    uint8_t *frame;

    if (!session_valid(session) || !close_reason_valid(reason)) {
        return MEISO_SESSION_CLOSE_INVALID;
    }
    if (session->state == MEISO_SESSION_STATE_CLOSED ||
        session->state == MEISO_SESSION_STATE_FAILED ||
        session->state == MEISO_SESSION_STATE_CLOSING) {
        return MEISO_SESSION_CLOSE_ALREADY_TERMINAL;
    }
    if (session->state != MEISO_SESSION_STATE_READY || !session->session_id_known) {
        clear_outbound(session);
        session->local_close_reason = reason;
        session->local_close_reason_known = true;
        session->state = MEISO_SESSION_STATE_CLOSED;
        session->failure = MEISO_SESSION_FAILURE_LOCAL_CLOSED;
        session->phase = PHASE_TERMINAL;
        return MEISO_SESSION_CLOSE_LOCAL_ONLY;
    }
    if (session->tx_pending) {
        return MEISO_SESSION_CLOSE_TX_BUSY;
    }

    frame = tx_bytes(session);
    put_session_id(frame + MEISO_SESSION_WIRE_HEADER_SIZE, session->session_id);
    frame[MEISO_SESSION_WIRE_HEADER_SIZE + SESSION_PAYLOAD_SIZE] = reason;
    encode_header(frame, WIRE_TYPE_CLOSE,
                  MEISO_SESSION_WIRE_HEADER_SIZE + CLOSE_PAYLOAD_SIZE);
    set_queued(session, WIRE_TYPE_CLOSE,
               MEISO_SESSION_WIRE_HEADER_SIZE + CLOSE_PAYLOAD_SIZE);
    session->local_close_reason = reason;
    session->local_close_reason_known = true;
    session->session_bound_close_started = true;
    session->state = MEISO_SESSION_STATE_CLOSING;
    session->phase = PHASE_CLOSE_PENDING;
    return MEISO_SESSION_CLOSE_QUEUED;
}

void meiso_session_transport_lost(MeisoSession *session)
{
    if (session_valid(session)) {
        clear_outbound(session);
        if (session->state == MEISO_SESSION_STATE_CLOSED ||
            session->state == MEISO_SESSION_STATE_FAILED) {
            return;
        }
        session->state = MEISO_SESSION_STATE_FAILED;
        session->failure = MEISO_SESSION_FAILURE_TRANSPORT_LOST;
        session->phase = PHASE_TERMINAL;
    }
}

bool meiso_session_snapshot(
    const MeisoSession *session,
    MeisoSessionSnapshot *out_snapshot
)
{
    if (!session_valid(session) || out_snapshot == NULL || ranges_overlap(
        session, session->arena_size, out_snapshot, sizeof(*out_snapshot)
    )) {
        return false;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->state = session->state;
    out_snapshot->failure = session->failure;
    out_snapshot->local_role = session->config.local_role;
    out_snapshot->local_peer_id = session->config.local_peer_id;
    out_snapshot->local_incarnation = session->config.local_incarnation;
    out_snapshot->remote_peer_id = session->remote_peer_id;
    out_snapshot->remote_incarnation = session->remote_incarnation;
    out_snapshot->session_id = session->session_id;
    out_snapshot->negotiated_limits = session->negotiated_limits;
    out_snapshot->local_close_reason = session->local_close_reason;
    out_snapshot->peer_close_reason = session->peer_close_reason;
    out_snapshot->remote_identity_known = session->remote_identity_known;
    out_snapshot->session_id_known = session->session_id_known;
    out_snapshot->outbound_pending = session->tx_pending;
    out_snapshot->local_close_reason_known = session->local_close_reason_known;
    out_snapshot->peer_close_reason_known = session->peer_close_reason_known;
    return true;
}
