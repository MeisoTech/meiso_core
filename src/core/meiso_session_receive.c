#include "meiso_session_private.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Decodes inbound frames and applies handshake/runtime receive transitions. */

static bool failure_wire_valid(uint8_t value)
{
    return value >= MEISO_SESSION_FAILURE_MALFORMED_FRAME &&
        value <= MEISO_SESSION_FAILURE_EARLY_RUNTIME;
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

static MeisoPeerId get_peer_id(const uint8_t *source)
{
    const MeisoPeerId value = {get_u64(source), get_u64(source + 8U)};
    return value;
}

static MeisoSessionNonce get_nonce(const uint8_t *source)
{
    const MeisoSessionNonce value = {get_u64(source), get_u64(source + 8U)};
    return value;
}

static MeisoSessionId get_session_id(const uint8_t *source)
{
    MeisoSessionId value;
    value.host_nonce = get_nonce(source);
    value.edge_nonce = get_nonce(source + 16U);
    return value;
}

static MeisoProtocolVersion get_protocol(const uint8_t *source)
{
    const MeisoProtocolVersion value = {
        get_u16(source), get_u16(source + 2U)
    };
    return value;
}

static MeisoProfileRef get_profile(const uint8_t *source)
{
    MeisoProfileRef value;
    value.id.high = get_u64(source);
    value.id.low = get_u64(source + 8U);
    value.version = get_u32(source + 16U);
    return value;
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
    return *out_type >= MEISO_SESSION_PRIVATE_WIRE_TYPE_OPEN && *out_type <= MEISO_SESSION_PRIVATE_WIRE_TYPE_RUNTIME;
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
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_ROLE_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (session->phase != MEISO_SESSION_PRIVATE_PHASE_EDGE_WAIT_OPEN ||
        payload_size != MEISO_SESSION_PRIVATE_OPEN_PAYLOAD_SIZE) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE, true);
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
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_PEER_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_session_nonce_valid(remote_nonce)) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_INVALID_NONCE, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    session->handshake_host_nonce = remote_nonce;
    session->handshake_host_nonce_known = true;
    if (remote_incarnation == 0U) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_INVALID_INCARNATION, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_protocol_version_equal(protocol, session->config.protocol)) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_PROTOCOL_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_profile_ref_equal(profile, session->config.profile)) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_PROFILE_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_session_limits_valid(limits)) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_LIMITS_INCOMPATIBLE, true);
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
    meiso_session_private_queue_accept(session, remote_nonce);
    session->phase = MEISO_SESSION_PRIVATE_PHASE_EDGE_ACCEPT_PENDING;
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
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_ROLE_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (session->phase != MEISO_SESSION_PRIVATE_PHASE_HOST_WAIT_ACCEPT ||
        payload_size != MEISO_SESSION_PRIVATE_ACCEPT_PAYLOAD_SIZE) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE, true);
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
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_PEER_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (remote_incarnation == 0U) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_INVALID_INCARNATION, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_session_nonce_valid(remote_nonce)) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_INVALID_NONCE, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_session_private_nonce_equal(echoed_nonce, session->config.local_nonce)) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_SESSION_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_protocol_version_equal(protocol, session->config.protocol)) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_PROTOCOL_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_profile_ref_equal(profile, session->config.profile)) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_PROFILE_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (!meiso_session_limits_valid(limits)) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_LIMITS_INCOMPATIBLE, true);
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
    meiso_session_private_queue_session_only(
        session, MEISO_SESSION_PRIVATE_WIRE_TYPE_CONFIRM
    );
    session->phase = MEISO_SESSION_PRIVATE_PHASE_HOST_CONFIRM_PENDING;
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
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_ROLE_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (session->phase != MEISO_SESSION_PRIVATE_PHASE_EDGE_WAIT_CONFIRM ||
        payload_size != MEISO_SESSION_PRIVATE_SESSION_PAYLOAD_SIZE) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    received = get_session_id(payload);
    if (!meiso_session_id_equal(received, session->session_id)) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_SESSION_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    meiso_session_private_queue_session_only(
        session, MEISO_SESSION_PRIVATE_WIRE_TYPE_READY
    );
    session->phase = MEISO_SESSION_PRIVATE_PHASE_EDGE_READY_PENDING;
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
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_ROLE_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (session->phase != MEISO_SESSION_PRIVATE_PHASE_HOST_WAIT_READY ||
        payload_size != MEISO_SESSION_PRIVATE_SESSION_PAYLOAD_SIZE) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    received = get_session_id(payload);
    if (!meiso_session_id_equal(received, session->session_id)) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_SESSION_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    session->state = MEISO_SESSION_STATE_READY;
    session->phase = MEISO_SESSION_PRIVATE_PHASE_READY;
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
         (session->phase == MEISO_SESSION_PRIVATE_PHASE_HOST_WAIT_ACCEPT ||
          session->phase == MEISO_SESSION_PRIVATE_PHASE_HOST_WAIT_READY)) ||
        (session->config.local_role == MEISO_SESSION_ROLE_EDGE &&
         session->phase == MEISO_SESSION_PRIVATE_PHASE_EDGE_WAIT_CONFIRM);

    if (session->state != MEISO_SESSION_STATE_HANDSHAKING || !phase_valid ||
        !session->handshake_host_nonce_known) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE, false);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (payload_size != MEISO_SESSION_PRIVATE_REJECT_PAYLOAD_SIZE || !failure_wire_valid(payload[16])) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_MALFORMED_FRAME, false);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    received_nonce = get_nonce(payload);
    if (!meiso_session_private_nonce_equal(received_nonce, session->handshake_host_nonce)) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_SESSION_MISMATCH, false);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    meiso_session_private_fail(session, payload[16], false);
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
        payload_size != MEISO_SESSION_PRIVATE_CLOSE_PAYLOAD_SIZE ||
        !meiso_session_private_close_reason_valid(payload[MEISO_SESSION_PRIVATE_SESSION_PAYLOAD_SIZE])) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    received = get_session_id(payload);
    if (!meiso_session_id_equal(received, session->session_id)) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_SESSION_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    meiso_session_private_clear_outbound(session);
    session->session_bound_close_started = false;
    session->peer_close_reason = payload[MEISO_SESSION_PRIVATE_SESSION_PAYLOAD_SIZE];
    session->peer_close_reason_known = true;
    session->state = MEISO_SESSION_STATE_CLOSED;
    session->failure = locally_closing ? MEISO_SESSION_FAILURE_LOCAL_CLOSED :
        MEISO_SESSION_FAILURE_PEER_CLOSED;
    session->phase = MEISO_SESSION_PRIVATE_PHASE_TERMINAL;
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

    if (session->state != MEISO_SESSION_STATE_READY || session->phase != MEISO_SESSION_PRIVATE_PHASE_READY) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_EARLY_RUNTIME, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (payload_size < MEISO_SESSION_PRIVATE_RUNTIME_PREFIX_SIZE) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_MALFORMED_FRAME, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    received = get_session_id(payload);
    if (!meiso_session_id_equal(received, session->session_id)) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_SESSION_MISMATCH, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    runtime_size = payload_size - MEISO_SESSION_PRIVATE_RUNTIME_PREFIX_SIZE;
    if (runtime_size > session->negotiated_limits.max_runtime_payload_bytes) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_LIMITS_INCOMPATIBLE, true);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (runtime_size != 0U) {
        memmove(meiso_session_private_rx_bytes(session), payload + MEISO_SESSION_PRIVATE_RUNTIME_PREFIX_SIZE, runtime_size);
    }
    session->rx_size = runtime_size;
    out_runtime->payload.data = runtime_size == 0U ? NULL : meiso_session_private_rx_bytes(session);
    out_runtime->payload.size = runtime_size;
    out_runtime->session_id = received;
    return MEISO_SESSION_RECEIVE_RUNTIME;
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

    if (!meiso_session_private_session_valid(session) || out_runtime == NULL ||
        !meiso_byte_view_valid(frame) ||
        meiso_session_private_ranges_overlap(session, session->arena_size,
                       out_runtime, sizeof(*out_runtime)) ||
        (frame.data != NULL && meiso_session_private_ranges_overlap(
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
            meiso_session_private_fail(session, MEISO_SESSION_FAILURE_MALFORMED_FRAME, false);
        }
        return MEISO_SESSION_RECEIVE_FAILED;
    }
    if (session->state == MEISO_SESSION_STATE_CLOSED &&
        !(type == MEISO_SESSION_PRIVATE_WIRE_TYPE_CLOSE &&
          session->failure == MEISO_SESSION_FAILURE_LOCAL_CLOSED &&
          session->session_bound_close_started)) {
        return MEISO_SESSION_RECEIVE_FAILED;
    }

    payload = frame.data + MEISO_SESSION_WIRE_HEADER_SIZE;
    payload_size = frame.size - MEISO_SESSION_WIRE_HEADER_SIZE;
    switch (type) {
    case MEISO_SESSION_PRIVATE_WIRE_TYPE_OPEN:
        return receive_open(session, payload, payload_size);
    case MEISO_SESSION_PRIVATE_WIRE_TYPE_ACCEPT:
        return receive_accept(session, payload, payload_size);
    case MEISO_SESSION_PRIVATE_WIRE_TYPE_CONFIRM:
        return receive_confirm(session, payload, payload_size);
    case MEISO_SESSION_PRIVATE_WIRE_TYPE_READY:
        return receive_ready(session, payload, payload_size);
    case MEISO_SESSION_PRIVATE_WIRE_TYPE_REJECT:
        return receive_reject(session, payload, payload_size);
    case MEISO_SESSION_PRIVATE_WIRE_TYPE_CLOSE:
        return receive_close(session, payload, payload_size);
    case MEISO_SESSION_PRIVATE_WIRE_TYPE_RUNTIME:
        return receive_runtime(session, payload, payload_size, out_runtime);
    default:
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_MALFORMED_FRAME, false);
        return MEISO_SESSION_RECEIVE_FAILED;
    }
}
