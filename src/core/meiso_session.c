#include "meiso_session_private.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Owns Session arena layout, value validation, sizing, and initialization. */

static bool role_valid(MeisoSessionRole role)
{
    return role == MEISO_SESSION_ROLE_HOST || role == MEISO_SESSION_ROLE_EDGE;
}
bool meiso_session_private_close_reason_valid(MeisoSessionCloseReason reason)
{
    return reason == MEISO_SESSION_CLOSE_NORMAL ||
        reason == MEISO_SESSION_CLOSE_SHUTDOWN ||
        reason == MEISO_SESSION_CLOSE_PROTOCOL_ERROR;
}

static bool add_size(size_t lhs, size_t rhs, size_t *out)
{
    if (out == NULL || rhs > SIZE_MAX - lhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

bool meiso_session_private_ranges_overlap(
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

bool meiso_session_private_config_valid(const MeisoSessionConfig *config)
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

bool meiso_session_private_session_valid(const MeisoSession *session)
{
    size_t tx_end;
    size_t rx_end;

    if (session == NULL || !meiso_session_private_config_valid(&session->config) ||
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

uint8_t *meiso_session_private_tx_bytes(MeisoSession *session)
{
    return (uint8_t *)session + session->tx_offset;
}

const uint8_t *meiso_session_private_tx_bytes_const(const MeisoSession *session)
{
    return (const uint8_t *)session + session->tx_offset;
}

uint8_t *meiso_session_private_rx_bytes(MeisoSession *session)
{
    return (uint8_t *)session + session->rx_offset;
}

bool meiso_session_private_nonce_equal(MeisoSessionNonce lhs, MeisoSessionNonce rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
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
    return meiso_session_private_nonce_equal(lhs.host_nonce, rhs.host_nonce) &&
        meiso_session_private_nonce_equal(lhs.edge_nonce, rhs.edge_nonce);
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
    return _Alignof(MeisoMaxAlign);
}

size_t meiso_session_memory_required(const MeisoSessionConfig *config)
{
    size_t required;

    if (!meiso_session_private_config_valid(config) ||
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
    if ((arena != NULL && meiso_session_private_ranges_overlap(
            arena, arena_size, out_session, sizeof(*out_session)
        )) ||
        (config != NULL && meiso_session_private_ranges_overlap(
            config, sizeof(*config), out_session, sizeof(*out_session)
        ))) {
        return MEISO_SESSION_INIT_INVALID_ARGUMENT;
    }
    *out_session = NULL;
    if (!meiso_session_private_config_valid(config)) {
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
    session->phase = MEISO_SESSION_PRIVATE_PHASE_NEW;
    if (config_copy.local_role == MEISO_SESSION_ROLE_HOST) {
        session->handshake_host_nonce = config_copy.local_nonce;
        session->handshake_host_nonce_known = true;
    }
    *out_session = session;
    return MEISO_SESSION_INIT_OK;
}
