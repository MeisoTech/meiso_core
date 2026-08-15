#ifndef MEISO_SESSION_H
#define MEISO_SESSION_H

#include "meiso_resource.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_SESSION_WIRE_VERSION 1U
#define MEISO_SESSION_WIRE_HEADER_SIZE 12U
#define MEISO_SESSION_ID_WIRE_SIZE 32U
#define MEISO_SESSION_MIN_MAX_FRAME_BYTES 104U

typedef struct MeisoSession MeisoSession;

/* Calls on one handle must be serialized by its owner; handles are independent. */

typedef struct {
    uint64_t high;
    uint64_t low;
} MeisoPeerId;

typedef struct {
    uint64_t high;
    uint64_t low;
} MeisoSessionNonce;

typedef struct {
    MeisoSessionNonce host_nonce;
    MeisoSessionNonce edge_nonce;
} MeisoSessionId;

typedef struct {
    uint64_t high;
    uint64_t low;
} MeisoProfileId;

typedef struct {
    uint16_t major;
    uint16_t minor;
} MeisoProtocolVersion;

typedef struct {
    MeisoProfileId id;
    uint32_t version;
} MeisoProfileRef;

typedef struct {
    uint32_t max_frame_bytes;
    uint32_t max_runtime_payload_bytes;
    uint32_t max_registry_entries;
} MeisoSessionLimits;

typedef uint8_t MeisoSessionRole;
enum {
    MEISO_SESSION_ROLE_HOST = 1,
    MEISO_SESSION_ROLE_EDGE = 2
};

typedef struct {
    MeisoSessionRole local_role;
    MeisoPeerId local_peer_id;
    uint64_t local_incarnation;
    MeisoSessionNonce local_nonce;
    MeisoPeerId expected_peer_id;
    MeisoProtocolVersion protocol;
    MeisoProfileRef profile;
    MeisoSessionLimits limits;
} MeisoSessionConfig;

typedef uint8_t MeisoSessionState;
enum {
    MEISO_SESSION_STATE_NEW = 1,
    MEISO_SESSION_STATE_HANDSHAKING = 2,
    MEISO_SESSION_STATE_READY = 3,
    MEISO_SESSION_STATE_CLOSING = 4,
    MEISO_SESSION_STATE_CLOSED = 5,
    MEISO_SESSION_STATE_FAILED = 6
};

typedef uint8_t MeisoSessionFailure;
enum {
    MEISO_SESSION_FAILURE_NONE = 0,
    MEISO_SESSION_FAILURE_MALFORMED_FRAME = 1,
    MEISO_SESSION_FAILURE_UNEXPECTED_MESSAGE = 2,
    MEISO_SESSION_FAILURE_ROLE_MISMATCH = 3,
    MEISO_SESSION_FAILURE_PEER_MISMATCH = 4,
    MEISO_SESSION_FAILURE_INVALID_INCARNATION = 5,
    MEISO_SESSION_FAILURE_INVALID_NONCE = 6,
    MEISO_SESSION_FAILURE_PROTOCOL_MISMATCH = 7,
    MEISO_SESSION_FAILURE_PROFILE_MISMATCH = 8,
    MEISO_SESSION_FAILURE_LIMITS_INCOMPATIBLE = 9,
    MEISO_SESSION_FAILURE_SESSION_MISMATCH = 10,
    MEISO_SESSION_FAILURE_EARLY_RUNTIME = 11,
    MEISO_SESSION_FAILURE_TRANSPORT_LOST = 12,
    MEISO_SESSION_FAILURE_LOCAL_CLOSED = 13,
    MEISO_SESSION_FAILURE_PEER_CLOSED = 14
};

typedef uint8_t MeisoSessionInitStatus;
enum {
    MEISO_SESSION_INIT_OK = 0,
    MEISO_SESSION_INIT_INVALID_ARGUMENT = 1,
    MEISO_SESSION_INIT_INVALID_CONFIG = 2,
    MEISO_SESSION_INIT_MISALIGNED_ARENA = 3,
    MEISO_SESSION_INIT_ARENA_TOO_SMALL = 4
};

typedef uint8_t MeisoSessionStartStatus;
enum {
    MEISO_SESSION_START_STARTED = 0,
    MEISO_SESSION_START_ALREADY_STARTED = 1,
    MEISO_SESSION_START_INVALID = 2
};

typedef uint8_t MeisoSessionOutboundStatus;
enum {
    MEISO_SESSION_OUTBOUND_SENT = 0,
    MEISO_SESSION_OUTBOUND_NONE = 1,
    MEISO_SESSION_OUTBOUND_INVALID = 2
};

typedef uint8_t MeisoSessionReceiveStatus;
enum {
    MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS = 0,
    MEISO_SESSION_RECEIVE_RUNTIME = 1,
    MEISO_SESSION_RECEIVE_PEER_CLOSED = 2,
    MEISO_SESSION_RECEIVE_FAILED = 3,
    MEISO_SESSION_RECEIVE_INVALID_ARGUMENT = 4
};

typedef uint8_t MeisoSessionSubmitStatus;
enum {
    MEISO_SESSION_SUBMIT_QUEUED = 0,
    MEISO_SESSION_SUBMIT_NOT_READY = 1,
    MEISO_SESSION_SUBMIT_TX_BUSY = 2,
    MEISO_SESSION_SUBMIT_CAPACITY = 3,
    MEISO_SESSION_SUBMIT_INVALID_ARGUMENT = 4
};

typedef uint8_t MeisoSessionCloseStatus;
enum {
    MEISO_SESSION_CLOSE_QUEUED = 0,
    MEISO_SESSION_CLOSE_LOCAL_ONLY = 1,
    MEISO_SESSION_CLOSE_TX_BUSY = 2,
    MEISO_SESSION_CLOSE_ALREADY_TERMINAL = 3,
    MEISO_SESSION_CLOSE_INVALID = 4
};

typedef uint8_t MeisoSessionCloseReason;
enum {
    MEISO_SESSION_CLOSE_NORMAL = 1,
    MEISO_SESSION_CLOSE_SHUTDOWN = 2,
    MEISO_SESSION_CLOSE_PROTOCOL_ERROR = 3
};

typedef struct {
    MeisoSessionId session_id;
    MeisoByteView payload;
} MeisoSessionRuntimeMessage;

typedef struct {
    MeisoSessionState state;
    MeisoSessionFailure failure;
    MeisoSessionRole local_role;
    MeisoPeerId local_peer_id;
    uint64_t local_incarnation;
    MeisoPeerId remote_peer_id;
    uint64_t remote_incarnation;
    MeisoSessionId session_id;
    MeisoSessionLimits negotiated_limits;
    MeisoSessionCloseReason local_close_reason;
    MeisoSessionCloseReason peer_close_reason;
    bool remote_identity_known;
    bool session_id_known;
    bool outbound_pending;
    bool local_close_reason_known;
    bool peer_close_reason_known;
} MeisoSessionSnapshot;

bool meiso_peer_id_valid(MeisoPeerId value);
bool meiso_peer_id_equal(MeisoPeerId lhs, MeisoPeerId rhs);
bool meiso_session_nonce_valid(MeisoSessionNonce value);
bool meiso_session_id_equal(MeisoSessionId lhs, MeisoSessionId rhs);
bool meiso_profile_ref_valid(MeisoProfileRef value);
bool meiso_profile_ref_equal(MeisoProfileRef lhs, MeisoProfileRef rhs);
bool meiso_protocol_version_valid(MeisoProtocolVersion value);
bool meiso_protocol_version_equal(
    MeisoProtocolVersion lhs,
    MeisoProtocolVersion rhs
);
bool meiso_session_limits_valid(MeisoSessionLimits value);

size_t meiso_session_alignment_required(void);
size_t meiso_session_memory_required(const MeisoSessionConfig *config);

MeisoSessionInitStatus meiso_session_init(
    void *arena,
    size_t arena_size,
    const MeisoSessionConfig *config,
    MeisoSession **out_session
);

MeisoSessionStartStatus meiso_session_start(MeisoSession *session);

/* The frame borrow ends before the next non-const call on this handle. */
bool meiso_session_next_outbound(
    const MeisoSession *session,
    MeisoByteView *out_frame
);

MeisoSessionOutboundStatus meiso_session_mark_outbound_sent(
    MeisoSession *session
);

/* The payload borrow ends before the next non-const call on this handle. */
MeisoSessionReceiveStatus meiso_session_receive(
    MeisoSession *session,
    MeisoByteView frame,
    MeisoSessionRuntimeMessage *out_runtime
);

MeisoSessionSubmitStatus meiso_session_submit_runtime(
    MeisoSession *session,
    MeisoByteView payload
);

MeisoSessionCloseStatus meiso_session_close(
    MeisoSession *session,
    MeisoSessionCloseReason reason
);

void meiso_session_transport_lost(MeisoSession *session);

bool meiso_session_snapshot(
    const MeisoSession *session,
    MeisoSessionSnapshot *out_snapshot
);

#ifdef __cplusplus
}
#endif

#endif
