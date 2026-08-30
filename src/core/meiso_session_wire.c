#include "meiso_session_private.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Encodes Session frames and owns the outbound queue state transitions. */

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

static void put_id128(uint8_t *target, uint64_t high, uint64_t low)
{
    put_u64(target, high);
    put_u64(target + 8U, low);
}

static void put_peer_id(uint8_t *target, MeisoPeerId value)
{
    put_id128(target, value.high, value.low);
}

static void put_nonce(uint8_t *target, MeisoSessionNonce value)
{
    put_id128(target, value.high, value.low);
}

void meiso_session_private_put_session_id(uint8_t *target, MeisoSessionId value)
{
    put_nonce(target, value.host_nonce);
    put_nonce(target + 16U, value.edge_nonce);
}

static void put_protocol(uint8_t *target, MeisoProtocolVersion value)
{
    put_u16(target, value.major);
    put_u16(target + 2U, value.minor);
}

static void put_profile(uint8_t *target, MeisoProfileRef value)
{
    put_id128(target, value.id.high, value.id.low);
    put_u32(target + 16U, value.version);
}

static void put_limits(uint8_t *target, MeisoSessionLimits value)
{
    put_u32(target, value.max_frame_bytes);
    put_u32(target + 4U, value.max_runtime_payload_bytes);
    put_u32(target + 8U, value.max_registry_entries);
}

void meiso_session_private_encode_header(uint8_t *target, uint8_t type, uint32_t size)
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

void meiso_session_private_set_queued(MeisoSession *session, uint8_t type, size_t size)
{
    session->tx_type = type;
    session->tx_size = size;
    session->tx_pending = true;
}

void meiso_session_private_queue_open(MeisoSession *session)
{
    uint8_t *frame = meiso_session_private_tx_bytes(session);
    uint8_t *payload = frame + MEISO_SESSION_WIRE_HEADER_SIZE;

    put_peer_id(payload, session->config.local_peer_id);
    put_u64(payload + 16U, session->config.local_incarnation);
    put_nonce(payload + 24U, session->config.local_nonce);
    put_peer_id(payload + 40U, session->config.expected_peer_id);
    put_protocol(payload + 56U, session->config.protocol);
    put_profile(payload + 60U, session->config.profile);
    put_limits(payload + 80U, session->config.limits);
    meiso_session_private_encode_header(frame, MEISO_SESSION_PRIVATE_WIRE_TYPE_OPEN,
                  MEISO_SESSION_WIRE_HEADER_SIZE + MEISO_SESSION_PRIVATE_OPEN_PAYLOAD_SIZE);
    meiso_session_private_set_queued(session, MEISO_SESSION_PRIVATE_WIRE_TYPE_OPEN,
               MEISO_SESSION_WIRE_HEADER_SIZE + MEISO_SESSION_PRIVATE_OPEN_PAYLOAD_SIZE);
}

void meiso_session_private_queue_accept(
    MeisoSession *session,
    MeisoSessionNonce host_nonce
)
{
    uint8_t *frame = meiso_session_private_tx_bytes(session);
    uint8_t *payload = frame + MEISO_SESSION_WIRE_HEADER_SIZE;

    put_peer_id(payload, session->config.local_peer_id);
    put_u64(payload + 16U, session->config.local_incarnation);
    put_nonce(payload + 24U, session->config.local_nonce);
    put_nonce(payload + 40U, host_nonce);
    put_protocol(payload + 56U, session->config.protocol);
    put_profile(payload + 60U, session->config.profile);
    put_limits(payload + 80U, session->config.limits);
    meiso_session_private_encode_header(frame, MEISO_SESSION_PRIVATE_WIRE_TYPE_ACCEPT,
                  MEISO_SESSION_WIRE_HEADER_SIZE + MEISO_SESSION_PRIVATE_ACCEPT_PAYLOAD_SIZE);
    meiso_session_private_set_queued(session, MEISO_SESSION_PRIVATE_WIRE_TYPE_ACCEPT,
               MEISO_SESSION_WIRE_HEADER_SIZE + MEISO_SESSION_PRIVATE_ACCEPT_PAYLOAD_SIZE);
}

void meiso_session_private_queue_session_only(
    MeisoSession *session,
    uint8_t type
)
{
    uint8_t *frame = meiso_session_private_tx_bytes(session);
    meiso_session_private_put_session_id(frame + MEISO_SESSION_WIRE_HEADER_SIZE, session->session_id);
    meiso_session_private_encode_header(frame, type,
                  MEISO_SESSION_WIRE_HEADER_SIZE + MEISO_SESSION_PRIVATE_SESSION_PAYLOAD_SIZE);
    meiso_session_private_set_queued(session, type,
               MEISO_SESSION_WIRE_HEADER_SIZE + MEISO_SESSION_PRIVATE_SESSION_PAYLOAD_SIZE);
}

static void queue_reject(MeisoSession *session, MeisoSessionFailure failure)
{
    uint8_t *frame = meiso_session_private_tx_bytes(session);
    uint8_t *payload = frame + MEISO_SESSION_WIRE_HEADER_SIZE;

    put_nonce(payload, session->handshake_host_nonce);
    payload[16] = failure;
    meiso_session_private_encode_header(frame, MEISO_SESSION_PRIVATE_WIRE_TYPE_REJECT,
                  MEISO_SESSION_WIRE_HEADER_SIZE + MEISO_SESSION_PRIVATE_REJECT_PAYLOAD_SIZE);
    meiso_session_private_set_queued(session, MEISO_SESSION_PRIVATE_WIRE_TYPE_REJECT,
               MEISO_SESSION_WIRE_HEADER_SIZE + MEISO_SESSION_PRIVATE_REJECT_PAYLOAD_SIZE);
}

void meiso_session_private_clear_outbound(MeisoSession *session)
{
    session->tx_pending = false;
    session->tx_type = 0U;
    session->tx_size = 0U;
}

void meiso_session_private_fail(
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
    meiso_session_private_clear_outbound(session);
    session->state = MEISO_SESSION_STATE_FAILED;
    session->failure = failure;
    session->phase = MEISO_SESSION_PRIVATE_PHASE_TERMINAL;
    if (can_reject) {
        queue_reject(session, failure);
    }
}
