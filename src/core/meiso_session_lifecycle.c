#include "meiso_session_private.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Drives public Session send, close, transport, and snapshot lifecycle. */

MeisoSessionStartStatus meiso_session_start(MeisoSession *session)
{
    if (!meiso_session_private_session_valid(session)) {
        return MEISO_SESSION_START_INVALID;
    }
    if (session->state != MEISO_SESSION_STATE_NEW || session->phase != MEISO_SESSION_PRIVATE_PHASE_NEW) {
        return MEISO_SESSION_START_ALREADY_STARTED;
    }
    session->state = MEISO_SESSION_STATE_HANDSHAKING;
    if (session->config.local_role == MEISO_SESSION_ROLE_HOST) {
        meiso_session_private_queue_open(session);
        session->phase = MEISO_SESSION_PRIVATE_PHASE_HOST_OPEN_PENDING;
    } else {
        session->phase = MEISO_SESSION_PRIVATE_PHASE_EDGE_WAIT_OPEN;
    }
    return MEISO_SESSION_START_STARTED;
}

bool meiso_session_next_outbound(
    const MeisoSession *session,
    MeisoByteView *out_frame
)
{
    if (!meiso_session_private_session_valid(session) || out_frame == NULL || meiso_session_private_ranges_overlap(
        session, session->arena_size, out_frame, sizeof(*out_frame)
    )) {
        return false;
    }
    out_frame->data = NULL;
    out_frame->size = 0U;
    if (!session->tx_pending) {
        return false;
    }
    out_frame->data = meiso_session_private_tx_bytes_const(session);
    out_frame->size = session->tx_size;
    return true;
}

MeisoSessionOutboundStatus meiso_session_mark_outbound_sent(
    MeisoSession *session
)
{
    uint8_t sent_type;

    if (!meiso_session_private_session_valid(session)) {
        return MEISO_SESSION_OUTBOUND_INVALID;
    }
    if (!session->tx_pending) {
        return MEISO_SESSION_OUTBOUND_NONE;
    }
    sent_type = session->tx_type;
    meiso_session_private_clear_outbound(session);

    if (sent_type == MEISO_SESSION_PRIVATE_WIRE_TYPE_OPEN && session->phase == MEISO_SESSION_PRIVATE_PHASE_HOST_OPEN_PENDING) {
        session->phase = MEISO_SESSION_PRIVATE_PHASE_HOST_WAIT_ACCEPT;
    } else if (sent_type == MEISO_SESSION_PRIVATE_WIRE_TYPE_ACCEPT &&
               session->phase == MEISO_SESSION_PRIVATE_PHASE_EDGE_ACCEPT_PENDING) {
        session->phase = MEISO_SESSION_PRIVATE_PHASE_EDGE_WAIT_CONFIRM;
    } else if (sent_type == MEISO_SESSION_PRIVATE_WIRE_TYPE_CONFIRM &&
               session->phase == MEISO_SESSION_PRIVATE_PHASE_HOST_CONFIRM_PENDING) {
        session->phase = MEISO_SESSION_PRIVATE_PHASE_HOST_WAIT_READY;
    } else if (sent_type == MEISO_SESSION_PRIVATE_WIRE_TYPE_READY &&
               session->phase == MEISO_SESSION_PRIVATE_PHASE_EDGE_READY_PENDING) {
        session->state = MEISO_SESSION_STATE_READY;
        session->phase = MEISO_SESSION_PRIVATE_PHASE_READY;
    } else if (sent_type == MEISO_SESSION_PRIVATE_WIRE_TYPE_CLOSE &&
               session->phase == MEISO_SESSION_PRIVATE_PHASE_CLOSE_PENDING) {
        session->state = MEISO_SESSION_STATE_CLOSED;
        session->failure = MEISO_SESSION_FAILURE_LOCAL_CLOSED;
        session->phase = MEISO_SESSION_PRIVATE_PHASE_TERMINAL;
    } else if (sent_type != MEISO_SESSION_PRIVATE_WIRE_TYPE_RUNTIME && sent_type != MEISO_SESSION_PRIVATE_WIRE_TYPE_REJECT) {
        meiso_session_private_fail(session, MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE, false);
        return MEISO_SESSION_OUTBOUND_INVALID;
    }
    return MEISO_SESSION_OUTBOUND_SENT;
}

MeisoSessionSubmitStatus meiso_session_submit_runtime(
    MeisoSession *session,
    MeisoByteView payload
)
{
    uint8_t *frame;
    size_t frame_size;

    if (!meiso_session_private_session_valid(session) || !meiso_byte_view_valid(payload)) {
        return MEISO_SESSION_SUBMIT_INVALID_ARGUMENT;
    }
    if (session->state != MEISO_SESSION_STATE_READY || session->phase != MEISO_SESSION_PRIVATE_PHASE_READY) {
        return MEISO_SESSION_SUBMIT_NOT_READY;
    }
    if (session->tx_pending) {
        return MEISO_SESSION_SUBMIT_TX_BUSY;
    }
    if (payload.size > session->negotiated_limits.max_runtime_payload_bytes ||
        payload.size > UINT32_MAX -
            (MEISO_SESSION_WIRE_HEADER_SIZE + MEISO_SESSION_PRIVATE_RUNTIME_PREFIX_SIZE)) {
        return MEISO_SESSION_SUBMIT_CAPACITY;
    }
    frame_size = MEISO_SESSION_WIRE_HEADER_SIZE + MEISO_SESSION_PRIVATE_RUNTIME_PREFIX_SIZE + payload.size;
    if (frame_size > session->negotiated_limits.max_frame_bytes) {
        return MEISO_SESSION_SUBMIT_CAPACITY;
    }

    frame = meiso_session_private_tx_bytes(session);
    if (payload.size != 0U) {
        memmove(frame + MEISO_SESSION_WIRE_HEADER_SIZE + MEISO_SESSION_PRIVATE_RUNTIME_PREFIX_SIZE,
                payload.data, payload.size);
    }
    meiso_session_private_put_session_id(frame + MEISO_SESSION_WIRE_HEADER_SIZE, session->session_id);
    meiso_session_private_encode_header(frame, MEISO_SESSION_PRIVATE_WIRE_TYPE_RUNTIME, (uint32_t)frame_size);
    meiso_session_private_set_queued(session, MEISO_SESSION_PRIVATE_WIRE_TYPE_RUNTIME, frame_size);
    return MEISO_SESSION_SUBMIT_QUEUED;
}

MeisoSessionCloseStatus meiso_session_close(
    MeisoSession *session,
    MeisoSessionCloseReason reason
)
{
    uint8_t *frame;

    if (!meiso_session_private_session_valid(session) || !meiso_session_private_close_reason_valid(reason)) {
        return MEISO_SESSION_CLOSE_INVALID;
    }
    if (session->state == MEISO_SESSION_STATE_CLOSED ||
        session->state == MEISO_SESSION_STATE_FAILED ||
        session->state == MEISO_SESSION_STATE_CLOSING) {
        return MEISO_SESSION_CLOSE_ALREADY_TERMINAL;
    }
    if (session->state != MEISO_SESSION_STATE_READY || !session->session_id_known) {
        meiso_session_private_clear_outbound(session);
        session->local_close_reason = reason;
        session->local_close_reason_known = true;
        session->state = MEISO_SESSION_STATE_CLOSED;
        session->failure = MEISO_SESSION_FAILURE_LOCAL_CLOSED;
        session->phase = MEISO_SESSION_PRIVATE_PHASE_TERMINAL;
        return MEISO_SESSION_CLOSE_LOCAL_ONLY;
    }
    if (session->tx_pending) {
        return MEISO_SESSION_CLOSE_TX_BUSY;
    }

    frame = meiso_session_private_tx_bytes(session);
    meiso_session_private_put_session_id(frame + MEISO_SESSION_WIRE_HEADER_SIZE, session->session_id);
    frame[MEISO_SESSION_WIRE_HEADER_SIZE + MEISO_SESSION_PRIVATE_SESSION_PAYLOAD_SIZE] = reason;
    meiso_session_private_encode_header(frame, MEISO_SESSION_PRIVATE_WIRE_TYPE_CLOSE,
                  MEISO_SESSION_WIRE_HEADER_SIZE + MEISO_SESSION_PRIVATE_CLOSE_PAYLOAD_SIZE);
    meiso_session_private_set_queued(session, MEISO_SESSION_PRIVATE_WIRE_TYPE_CLOSE,
               MEISO_SESSION_WIRE_HEADER_SIZE + MEISO_SESSION_PRIVATE_CLOSE_PAYLOAD_SIZE);
    session->local_close_reason = reason;
    session->local_close_reason_known = true;
    session->session_bound_close_started = true;
    session->state = MEISO_SESSION_STATE_CLOSING;
    session->phase = MEISO_SESSION_PRIVATE_PHASE_CLOSE_PENDING;
    return MEISO_SESSION_CLOSE_QUEUED;
}

void meiso_session_transport_lost(MeisoSession *session)
{
    if (meiso_session_private_session_valid(session)) {
        meiso_session_private_clear_outbound(session);
        if (session->state == MEISO_SESSION_STATE_CLOSED ||
            session->state == MEISO_SESSION_STATE_FAILED) {
            return;
        }
        session->state = MEISO_SESSION_STATE_FAILED;
        session->failure = MEISO_SESSION_FAILURE_TRANSPORT_LOST;
        session->phase = MEISO_SESSION_PRIVATE_PHASE_TERMINAL;
    }
}

bool meiso_session_snapshot(
    const MeisoSession *session,
    MeisoSessionSnapshot *out_snapshot
)
{
    if (!meiso_session_private_session_valid(session) || out_snapshot == NULL || meiso_session_private_ranges_overlap(
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
