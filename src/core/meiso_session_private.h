#ifndef MEISO_SESSION_PRIVATE_H
#define MEISO_SESSION_PRIVATE_H

#include <meiso/core/session.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__clang__) || defined(__GNUC__)
#define MEISO_SESSION_PRIVATE_API __attribute__((visibility("hidden")))
#else
#define MEISO_SESSION_PRIVATE_API
#endif

enum {
    MEISO_SESSION_PRIVATE_WIRE_TYPE_OPEN = 1,
    MEISO_SESSION_PRIVATE_WIRE_TYPE_ACCEPT = 2,
    MEISO_SESSION_PRIVATE_WIRE_TYPE_CONFIRM = 3,
    MEISO_SESSION_PRIVATE_WIRE_TYPE_READY = 4,
    MEISO_SESSION_PRIVATE_WIRE_TYPE_REJECT = 5,
    MEISO_SESSION_PRIVATE_WIRE_TYPE_CLOSE = 6,
    MEISO_SESSION_PRIVATE_WIRE_TYPE_RUNTIME = 7
};

enum {
    MEISO_SESSION_PRIVATE_PHASE_NEW = 1,
    MEISO_SESSION_PRIVATE_PHASE_HOST_OPEN_PENDING = 2,
    MEISO_SESSION_PRIVATE_PHASE_HOST_WAIT_ACCEPT = 3,
    MEISO_SESSION_PRIVATE_PHASE_HOST_CONFIRM_PENDING = 4,
    MEISO_SESSION_PRIVATE_PHASE_HOST_WAIT_READY = 5,
    MEISO_SESSION_PRIVATE_PHASE_EDGE_WAIT_OPEN = 6,
    MEISO_SESSION_PRIVATE_PHASE_EDGE_ACCEPT_PENDING = 7,
    MEISO_SESSION_PRIVATE_PHASE_EDGE_WAIT_CONFIRM = 8,
    MEISO_SESSION_PRIVATE_PHASE_EDGE_READY_PENDING = 9,
    MEISO_SESSION_PRIVATE_PHASE_READY = 10,
    MEISO_SESSION_PRIVATE_PHASE_CLOSE_PENDING = 11,
    MEISO_SESSION_PRIVATE_PHASE_TERMINAL = 12
};

enum {
    MEISO_SESSION_PRIVATE_OPEN_PAYLOAD_SIZE = 92,
    MEISO_SESSION_PRIVATE_ACCEPT_PAYLOAD_SIZE = 92,
    MEISO_SESSION_PRIVATE_SESSION_PAYLOAD_SIZE = 32,
    MEISO_SESSION_PRIVATE_REJECT_PAYLOAD_SIZE = 17,
    MEISO_SESSION_PRIVATE_CLOSE_PAYLOAD_SIZE = 33,
    MEISO_SESSION_PRIVATE_RUNTIME_PREFIX_SIZE = 32
};

/*
 * Build-only Session representation. The handle and its TX/RX storage live
 * in one caller-owned arena; config is copied during init. Returned outbound
 * and runtime payload views borrow that arena and are invalidated by the next
 * mutating Session operation. Field order is the arena ABI and must not move.
 */
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

MEISO_SESSION_PRIVATE_API bool
meiso_session_private_close_reason_valid(MeisoSessionCloseReason reason);
MEISO_SESSION_PRIVATE_API bool meiso_session_private_ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
);
MEISO_SESSION_PRIVATE_API bool meiso_session_private_config_valid(
    const MeisoSessionConfig *config
);
MEISO_SESSION_PRIVATE_API bool meiso_session_private_session_valid(
    const MeisoSession *session
);
MEISO_SESSION_PRIVATE_API uint8_t *meiso_session_private_tx_bytes(
    MeisoSession *session
);
MEISO_SESSION_PRIVATE_API const uint8_t *meiso_session_private_tx_bytes_const(
    const MeisoSession *session
);
MEISO_SESSION_PRIVATE_API uint8_t *meiso_session_private_rx_bytes(
    MeisoSession *session
);
MEISO_SESSION_PRIVATE_API bool meiso_session_private_nonce_equal(
    MeisoSessionNonce lhs,
    MeisoSessionNonce rhs
);

MEISO_SESSION_PRIVATE_API void meiso_session_private_queue_open(
    MeisoSession *session
);
MEISO_SESSION_PRIVATE_API void meiso_session_private_queue_accept(
    MeisoSession *session,
    MeisoSessionNonce host_nonce
);
MEISO_SESSION_PRIVATE_API void meiso_session_private_queue_session_only(
    MeisoSession *session,
    uint8_t type
);
MEISO_SESSION_PRIVATE_API void meiso_session_private_clear_outbound(
    MeisoSession *session
);
MEISO_SESSION_PRIVATE_API void meiso_session_private_fail(
    MeisoSession *session,
    MeisoSessionFailure failure,
    bool send_reject
);
MEISO_SESSION_PRIVATE_API void meiso_session_private_put_session_id(
    uint8_t *target,
    MeisoSessionId value
);
MEISO_SESSION_PRIVATE_API void meiso_session_private_encode_header(
    uint8_t *target,
    uint8_t type,
    uint32_t size
);
MEISO_SESSION_PRIVATE_API void meiso_session_private_set_queued(
    MeisoSession *session,
    uint8_t type,
    size_t size
);

#endif
