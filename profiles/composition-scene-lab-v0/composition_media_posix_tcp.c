#define _POSIX_C_SOURCE 200809L

#include "composition_media_posix_tcp.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdalign.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    MEDIA_POSIX_TCP_MAGIC = 0x4d50584dU, /* MPXM */
    MEDIA_OPEN_MAGIC = 0x4d4f504eU,      /* MOPN */
    MEDIA_BIND_MAGIC = 0x4d424e44U,      /* MBND */
    MEDIA_FRAME_MAGIC = 0x4d46524dU      /* MFRM */
};

struct MeisoCompositionMediaPosixTcp {
    MeisoCompositionMediaTransport *transport;
    MeisoCompositionMediaOpen scope;
    size_t arena_size;
    size_t rx_offset;
    size_t rx_size;
    size_t rx_expected;
    size_t max_io_bytes_per_step;
    MeisoCompositionMediaPacketView tx_packet;
    size_t tx_sent;
    MeisoCompositionMediaTransportStatus last_receive_status;
    uint32_t magic;
    int connected_fd;
    int last_error;
    MeisoSessionRole local_role;
    MeisoCompositionMediaTerminalReason terminal_reason;
    bool tx_borrowed;
    bool receive_status_present;
    bool terminal;
    uint8_t storage[];
};

static bool add_size(size_t lhs, size_t rhs, size_t *out_value)
{
    if (out_value == NULL || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out_value = lhs + rhs;
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
    if (lhs == NULL || rhs == NULL || lhs_size == 0U || rhs_size == 0U) {
        return false;
    }
    if (lhs_start > UINTPTR_MAX - lhs_size ||
        rhs_start > UINTPTR_MAX - rhs_size) {
        return true;
    }
    return lhs_start < rhs_start + rhs_size &&
        rhs_start < lhs_start + lhs_size;
}

static bool open_equal(
    const MeisoCompositionMediaOpen *lhs,
    const MeisoCompositionMediaOpen *rhs
)
{
    return meiso_session_id_equal(lhs->session_id, rhs->session_id) &&
        lhs->catalog_generation == rhs->catalog_generation;
}

static bool packet_ref_equal(
    const MeisoCompositionMediaPacketRef *lhs,
    const MeisoCompositionMediaPacketRef *rhs
)
{
    return meiso_session_id_equal(lhs->session_id, rhs->session_id) &&
        lhs->catalog_generation == rhs->catalog_generation &&
        lhs->serial == rhs->serial && lhs->kind == rhs->kind &&
        lhs->source == rhs->source &&
        lhs->frame_sequence == rhs->frame_sequence &&
        lhs->fragment_offset == rhs->fragment_offset &&
        lhs->packet_bytes == rhs->packet_bytes;
}

static uint32_t get_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
        ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
}

static size_t minimum_size(size_t lhs, size_t rhs)
{
    return lhs < rhs ? lhs : rhs;
}

static size_t syscall_request_size(size_t remaining, size_t configured_limit)
{
    return minimum_size(
        minimum_size(remaining, configured_limit), (size_t)SSIZE_MAX
    );
}

static bool terminal_reason_valid(MeisoCompositionMediaTerminalReason reason)
{
    return reason >= MEISO_COMPOSITION_MEDIA_TERMINAL_LOCAL_CLOSE &&
        reason <= MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED;
}

static bool route_snapshot_fresh(
    const MeisoCompositionMediaRouteSnapshot *route
)
{
    return !route->send_in_flight && !route->send_latest &&
        route->send_frame_sequence == 0U &&
        route->send_fragment_offset == 0U &&
        route->submitted_sequence_high_water == 0U &&
        route->transport_whole_frames_dropped == 0U &&
        !route->receive_reassembly && !route->receive_latest &&
        route->receive_frame_sequence == 0U &&
        route->receive_next_offset == 0U &&
        route->published_sequence_high_water == 0U &&
        route->sequence_gap_frames == 0U &&
        route->producer_drop_high_water == 0U &&
        route->reassembly_frames_dropped == 0U &&
        route->unread_latest_frames_dropped == 0U;
}

static bool transport_snapshot_fresh(
    const MeisoCompositionMediaPosixTcpConfig *config,
    const MeisoCompositionMediaTransportSnapshot *snapshot
)
{
    const MeisoCompositionMediaTransportPhase expected_phase =
        snapshot->local_role == MEISO_SESSION_ROLE_HOST ?
        MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_OPEN_PENDING :
        MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_WAIT_OPEN;
    return snapshot->local_role == MEISO_SESSION_ROLE_HOST ||
            snapshot->local_role == MEISO_SESSION_ROLE_EDGE ?
        open_equal(&config->scope, &snapshot->scope) &&
            snapshot->phase == expected_phase &&
            snapshot->terminal_reason == 0U &&
            snapshot->bound_route_mask == 0U &&
            snapshot->fragment_rr_cursor == 0U &&
            !snapshot->outbound_packet_pending &&
            snapshot->outbound_packet_kind == 0U &&
            snapshot->outbound_packet_serial_high_water == 0U &&
            route_snapshot_fresh(&snapshot->routes[0]) &&
            route_snapshot_fresh(&snapshot->routes[1]) : false;
}

static bool config_value_valid(
    const MeisoCompositionMediaPosixTcpConfig *config,
    MeisoCompositionMediaTransportSnapshot *out_snapshot
)
{
    MeisoCompositionMediaTransportSnapshot snapshot;
    if (config == NULL || config->connected_fd < 0 ||
        config->transport == NULL || config->max_io_bytes_per_step == 0U ||
        meiso_composition_media_transport_snapshot(
            config->transport, &snapshot
        ) != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK ||
        ranges_overlap(
            config, sizeof(*config), config->transport, snapshot.arena_bytes
        ) || !transport_snapshot_fresh(config, &snapshot)) {
        return false;
    }
    if (out_snapshot != NULL) {
        *out_snapshot = snapshot;
    }
    return true;
}

static bool socket_is_nonblocking_stream(int fd, bool *out_fd_error)
{
    struct sockaddr_storage peer_address;
    socklen_t peer_address_size = (socklen_t)sizeof(peer_address);
    int flags;
    int socket_type;
    socklen_t socket_type_size = (socklen_t)sizeof(socket_type);

    *out_fd_error = false;
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || getsockopt(
            fd, SOL_SOCKET, SO_TYPE, &socket_type, &socket_type_size
        ) != 0 || getpeername(
            fd, (struct sockaddr *)(void *)&peer_address,
            &peer_address_size
        ) != 0 || socket_type != SOCK_STREAM ||
        (peer_address.ss_family != AF_INET &&
         peer_address.ss_family != AF_INET6)) {
        *out_fd_error = true;
        return false;
    }
    return (flags & O_NONBLOCK) != 0;
}

static bool install_no_sigpipe(int fd)
{
#if defined(SO_NOSIGPIPE)
    const int enabled = 1;
    return setsockopt(
        fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, (socklen_t)sizeof(enabled)
    ) == 0;
#elif defined(MSG_NOSIGNAL)
    (void)fd;
    return true;
#else
    (void)fd;
    return false;
#endif
}

static int send_flags(void)
{
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

static uint8_t *rx_bytes(MeisoCompositionMediaPosixTcp *adapter)
{
    return (uint8_t *)adapter + adapter->rx_offset;
}

static bool adapter_valid(
    const MeisoCompositionMediaPosixTcp *adapter
)
{
    MeisoCompositionMediaTransportSnapshot transport_snapshot;
    size_t rx_end;
    if (adapter == NULL || adapter->magic != MEDIA_POSIX_TCP_MAGIC ||
        adapter->transport == NULL || adapter->connected_fd < 0 ||
        adapter->max_io_bytes_per_step == 0U ||
        adapter->arena_size < sizeof(*adapter) ||
        !add_size(
            adapter->rx_offset,
            MEISO_COMPOSITION_MEDIA_TRANSPORT_MAX_PACKET_BYTES, &rx_end
        ) ||
        rx_end > adapter->arena_size ||
        adapter->rx_size >
            MEISO_COMPOSITION_MEDIA_TRANSPORT_MAX_PACKET_BYTES ||
        adapter->rx_expected >
            MEISO_COMPOSITION_MEDIA_TRANSPORT_MAX_PACKET_BYTES ||
        adapter->tx_sent > adapter->tx_packet.bytes.size ||
        (adapter->tx_borrowed &&
         (adapter->tx_packet.bytes.data == NULL ||
          adapter->tx_packet.bytes.size == 0U ||
          adapter->tx_packet.bytes.size >
              MEISO_COMPOSITION_MEDIA_TRANSPORT_MAX_PACKET_BYTES ||
          adapter->tx_packet.ref.packet_bytes !=
              adapter->tx_packet.bytes.size)) ||
        meiso_composition_media_transport_snapshot(
            adapter->transport, &transport_snapshot
        ) != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK ||
        transport_snapshot.local_role != adapter->local_role ||
        ranges_overlap(
            adapter, adapter->arena_size, adapter->transport,
            transport_snapshot.arena_bytes
        ) ||
        !open_equal(&transport_snapshot.scope, &adapter->scope)) {
        return false;
    }
    return true;
}

static void clear_rx(MeisoCompositionMediaPosixTcp *adapter)
{
    adapter->rx_size = 0U;
    adapter->rx_expected = 0U;
}

static void clear_tx(MeisoCompositionMediaPosixTcp *adapter)
{
    memset(&adapter->tx_packet, 0, sizeof(adapter->tx_packet));
    adapter->tx_sent = 0U;
    adapter->tx_borrowed = false;
}

static bool mark_terminal(
    MeisoCompositionMediaPosixTcp *adapter,
    MeisoCompositionMediaTerminalReason reason,
    int error_number
)
{
    MeisoCompositionMediaTransportSnapshot snapshot;
    const MeisoCompositionMediaTransportStatus status =
        meiso_composition_media_transport_terminate(
            adapter->transport, &adapter->scope, reason
        );
    if (status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK &&
        status != MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL) {
        return false;
    }
    if (meiso_composition_media_transport_snapshot(
            adapter->transport, &snapshot
        ) != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK ||
        snapshot.phase !=
            MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return false;
    }
    memset(
        rx_bytes(adapter), 0,
        MEISO_COMPOSITION_MEDIA_TRANSPORT_MAX_PACKET_BYTES
    );
    clear_rx(adapter);
    clear_tx(adapter);
    adapter->terminal = true;
    adapter->terminal_reason = snapshot.terminal_reason;
    adapter->last_error = error_number;
    return true;
}

size_t meiso_composition_media_posix_tcp_alignment_required(void)
{
    return alignof(max_align_t);
}

size_t meiso_composition_media_posix_tcp_memory_required(
    const MeisoCompositionMediaPosixTcpConfig *config
)
{
    size_t required;
    if (!config_value_valid(config, NULL) ||
        !add_size(
            sizeof(MeisoCompositionMediaPosixTcp),
            MEISO_COMPOSITION_MEDIA_TRANSPORT_MAX_PACKET_BYTES, &required
        )) {
        return 0U;
    }
    return required;
}

MeisoCompositionMediaPosixTcpInitStatus
meiso_composition_media_posix_tcp_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionMediaPosixTcpConfig *config,
    MeisoCompositionMediaPosixTcp **out_adapter
)
{
    MeisoCompositionMediaPosixTcpConfig config_copy;
    MeisoCompositionMediaTransportSnapshot snapshot;
    MeisoCompositionMediaPosixTcp value;
    MeisoCompositionMediaPosixTcp *adapter;
    size_t required;
    bool fd_error;

    if (arena == NULL || config == NULL || out_adapter == NULL ||
        ranges_overlap(arena, arena_size, config, sizeof(*config)) ||
        ranges_overlap(
            arena, arena_size, out_adapter, sizeof(*out_adapter)
        ) ||
        ranges_overlap(
            config, sizeof(*config), out_adapter, sizeof(*out_adapter)
        ) ||
        config->transport == NULL) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_INVALID_ARGUMENT;
    }
    if (meiso_composition_media_transport_snapshot(
            config->transport, &snapshot
        ) != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK ||
        config->connected_fd < 0 || config->max_io_bytes_per_step == 0U) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_INVALID_CONFIG;
    }
    if (!open_equal(&config->scope, &snapshot.scope)) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_SCOPE_MISMATCH;
    }
    if (ranges_overlap(
            arena, arena_size, config->transport, snapshot.arena_bytes
        ) ||
        ranges_overlap(
            config, sizeof(*config), config->transport, snapshot.arena_bytes
        ) ||
        ranges_overlap(
            out_adapter, sizeof(*out_adapter), config->transport,
            snapshot.arena_bytes
        )) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_INVALID_ARGUMENT;
    }
    if (!transport_snapshot_fresh(config, &snapshot)) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_INVALID_CONFIG;
    }
    if ((uintptr_t)arena %
        meiso_composition_media_posix_tcp_alignment_required() != 0U) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_MISALIGNED_ARENA;
    }
    required = meiso_composition_media_posix_tcp_memory_required(config);
    if (required == 0U || arena_size < required) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_ARENA_TOO_SMALL;
    }
    if (!socket_is_nonblocking_stream(config->connected_fd, &fd_error)) {
        return fd_error ?
            MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_FD_ERROR :
            MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_NONBLOCKING_REQUIRED;
    }
    if (!install_no_sigpipe(config->connected_fd)) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_FD_ERROR;
    }

    config_copy = *config;
    memset(&value, 0, sizeof(value));
    value.transport = config_copy.transport;
    value.scope = config_copy.scope;
    value.arena_size = required;
    value.rx_offset = sizeof(MeisoCompositionMediaPosixTcp);
    value.max_io_bytes_per_step = config_copy.max_io_bytes_per_step;
    value.magic = MEDIA_POSIX_TCP_MAGIC;
    value.connected_fd = config_copy.connected_fd;
    value.local_role = snapshot.local_role;

    memset(arena, 0, required);
    adapter = arena;
    *adapter = value;
    *out_adapter = adapter;
    return MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_OK;
}

bool meiso_composition_media_posix_tcp_wants_read(
    const MeisoCompositionMediaPosixTcp *adapter
)
{
    return adapter_valid(adapter) && !adapter->terminal;
}

bool meiso_composition_media_posix_tcp_wants_write(
    const MeisoCompositionMediaPosixTcp *adapter
)
{
    return adapter_valid(adapter) && !adapter->terminal &&
        adapter->tx_borrowed;
}

MeisoCompositionMediaPosixTcpPrepareStatus
meiso_composition_media_posix_tcp_prepare_outbound(
    MeisoCompositionMediaPosixTcp *adapter
)
{
    MeisoCompositionMediaPacketView packet;
    MeisoCompositionMediaTransportStatus status;
    if (!adapter_valid(adapter)) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_INVALID_ARGUMENT;
    }
    if (adapter->terminal) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_TERMINAL;
    }
    if (adapter->tx_borrowed) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_READY;
    }
    status = meiso_composition_media_transport_next_outbound(
        adapter->transport, &packet
    );
    if (status == MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_PACKET) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_IDLE;
    }
    if (status == MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL) {
        (void)mark_terminal(
            adapter, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            0
        );
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_TERMINAL;
    }
    if (status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK ||
        packet.bytes.data == NULL || packet.bytes.size == 0U ||
        packet.bytes.size >
            MEISO_COMPOSITION_MEDIA_TRANSPORT_MAX_PACKET_BYTES ||
        packet.ref.packet_bytes != packet.bytes.size ||
        ranges_overlap(
            adapter, adapter->arena_size, packet.bytes.data,
            packet.bytes.size
        )) {
        (void)mark_terminal(
            adapter, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            EPROTO
        );
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_TRANSPORT_INVALID;
    }
    adapter->tx_packet = packet;
    adapter->tx_sent = 0U;
    adapter->tx_borrowed = true;
    return MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_READY;
}

static MeisoCompositionMediaPosixTcpReadStatus dispatch_rx_packet(
    MeisoCompositionMediaPosixTcp *adapter
)
{
    const uint32_t magic = get_u32(rx_bytes(adapter));
    const MeisoCompositionMediaTransportStatus status =
        meiso_composition_media_transport_receive_packet(
            adapter->transport,
            (MeisoByteView){rx_bytes(adapter), adapter->rx_size}
        );
    adapter->last_receive_status = status;
    adapter->receive_status_present = true;
    clear_rx(adapter);
    switch (status) {
    case MEISO_COMPOSITION_MEDIA_TRANSPORT_OK:
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PACKET_ACCEPTED;
    case MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED:
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_FRAGMENT_ACCEPTED;
    case MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAME_PUBLISHED:
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_FRAME_PUBLISHED;
    case MEISO_COMPOSITION_MEDIA_TRANSPORT_REASSEMBLY_REPLACED:
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_REASSEMBLY_REPLACED;
    case MEISO_COMPOSITION_MEDIA_TRANSPORT_DISCONTINUITY:
    case MEISO_COMPOSITION_MEDIA_TRANSPORT_SEQUENCE_REPLAY:
    case MEISO_COMPOSITION_MEDIA_TRANSPORT_CONFLICT:
        if (magic == MEDIA_FRAME_MAGIC) {
            return MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PACKET_REJECTED;
        }
        break;
    default:
        break;
    }
    (void)mark_terminal(
        adapter, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
        EPROTO
    );
    return MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST;
}

static bool advance_rx_target(MeisoCompositionMediaPosixTcp *adapter)
{
    const uint32_t magic = get_u32(rx_bytes(adapter));
    if (adapter->rx_expected == 0U && adapter->rx_size == 4U) {
        if (magic == MEDIA_OPEN_MAGIC) {
            adapter->rx_expected = MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE;
            return true;
        }
        if (magic == MEDIA_BIND_MAGIC) {
            adapter->rx_expected = MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE;
            return true;
        }
        if (magic == MEDIA_FRAME_MAGIC) {
            adapter->rx_expected =
                MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE;
            return true;
        }
        return false;
    }
    if (magic == MEDIA_FRAME_MAGIC &&
        adapter->rx_expected ==
            MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE &&
        adapter->rx_size ==
            MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE) {
        const uint32_t fragment_bytes = get_u32(rx_bytes(adapter) + 200U);
        if (fragment_bytes == 0U || fragment_bytes >
            MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE) {
            return false;
        }
        adapter->rx_expected =
            MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE + fragment_bytes;
    }
    return true;
}

MeisoCompositionMediaPosixTcpReadStatus
meiso_composition_media_posix_tcp_on_readable(
    MeisoCompositionMediaPosixTcp *adapter
)
{
    size_t target;
    size_t remaining;
    size_t request_size;
    ssize_t received;
    if (!adapter_valid(adapter)) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_INVALID_ARGUMENT;
    }
    if (adapter->terminal) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST;
    }
    target = adapter->rx_expected == 0U ? 4U : adapter->rx_expected;
    if (target <= adapter->rx_size) {
        (void)mark_terminal(
            adapter, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            EPROTO
        );
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST;
    }
    remaining = target - adapter->rx_size;
    request_size = syscall_request_size(
        remaining, adapter->max_io_bytes_per_step
    );
    received = recv(
        adapter->connected_fd, rx_bytes(adapter) + adapter->rx_size,
        request_size, 0
    );
    if (received > 0) {
        adapter->rx_size += (size_t)received;
        if (adapter->rx_size != target) {
            return MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PROGRESS;
        }
        if (!advance_rx_target(adapter)) {
            (void)mark_terminal(
                adapter,
                MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
                EPROTO
            );
            return MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST;
        }
        if (adapter->rx_expected != 0U &&
            adapter->rx_size == adapter->rx_expected) {
            return dispatch_rx_packet(adapter);
        }
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PROGRESS;
    }
    if (received == 0) {
        (void)mark_terminal(
            adapter, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            0
        );
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_WOULD_BLOCK;
    }
    if (errno == EINTR) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_INTERRUPTED;
    }
    {
        const int error_number = errno;
        (void)mark_terminal(
            adapter, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            error_number
        );
    }
    return MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST;
}

MeisoCompositionMediaPosixTcpWriteStatus
meiso_composition_media_posix_tcp_on_writable(
    MeisoCompositionMediaPosixTcp *adapter
)
{
    MeisoCompositionMediaPacketView current;
    MeisoCompositionMediaTransportStatus status;
    size_t remaining;
    size_t request_size;
    ssize_t sent;
    if (!adapter_valid(adapter)) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_INVALID_ARGUMENT;
    }
    if (adapter->terminal) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_LOST;
    }
    if (!adapter->tx_borrowed) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_IDLE;
    }
    status = meiso_composition_media_transport_next_outbound(
        adapter->transport, &current
    );
    if (status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK ||
        !packet_ref_equal(&current.ref, &adapter->tx_packet.ref) ||
        current.bytes.data != adapter->tx_packet.bytes.data ||
        current.bytes.size != adapter->tx_packet.bytes.size) {
        (void)mark_terminal(
            adapter, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            EPROTO
        );
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_INVALID;
    }
    remaining = adapter->tx_packet.bytes.size - adapter->tx_sent;
    request_size = syscall_request_size(
        remaining, adapter->max_io_bytes_per_step
    );
    sent = send(
        adapter->connected_fd,
        adapter->tx_packet.bytes.data + adapter->tx_sent,
        request_size, send_flags()
    );
    if (sent > 0) {
        adapter->tx_sent += (size_t)sent;
        if (adapter->tx_sent == adapter->tx_packet.bytes.size) {
            status = meiso_composition_media_transport_complete_outbound(
                adapter->transport, &adapter->tx_packet.ref
            );
            if (status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
                (void)mark_terminal(
                    adapter,
                    MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
                    EPROTO
                );
                return
                    MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_INVALID;
            }
            clear_tx(adapter);
            return MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PACKET_SENT;
        }
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PROGRESS;
    }
    if (sent == 0) {
        (void)mark_terminal(
            adapter, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            EPIPE
        );
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_LOST;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_WOULD_BLOCK;
    }
    if (errno == EINTR) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_INTERRUPTED;
    }
    {
        const int error_number = errno;
        (void)mark_terminal(
            adapter, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            error_number
        );
    }
    return MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_LOST;
}

MeisoCompositionMediaPosixTcpTerminateStatus
meiso_composition_media_posix_tcp_terminate(
    MeisoCompositionMediaPosixTcp *adapter,
    MeisoCompositionMediaTerminalReason reason,
    int error_number
)
{
    if (!adapter_valid(adapter) || !terminal_reason_valid(reason) ||
        error_number < 0) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_TERMINATE_INVALID_ARGUMENT;
    }
    if (adapter->terminal) {
        return MEISO_COMPOSITION_MEDIA_POSIX_TCP_TERMINATE_ALREADY;
    }
    if (!mark_terminal(adapter, reason, error_number)) {
        MeisoCompositionMediaTransportSnapshot snapshot;
        if (meiso_composition_media_transport_snapshot(
                adapter->transport, &snapshot
            ) != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
            return
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_TERMINATE_TRANSPORT_INVALID;
        }
        return open_equal(&snapshot.scope, &adapter->scope) ?
            MEISO_COMPOSITION_MEDIA_POSIX_TCP_TERMINATE_TRANSPORT_INVALID :
            MEISO_COMPOSITION_MEDIA_POSIX_TCP_TERMINATE_SCOPE_MISMATCH;
    }
    return MEISO_COMPOSITION_MEDIA_POSIX_TCP_TERMINATE_OK;
}

bool meiso_composition_media_posix_tcp_last_error(
    const MeisoCompositionMediaPosixTcp *adapter,
    int *out_error_number
)
{
    MeisoCompositionMediaTransportSnapshot transport_snapshot;
    if (!adapter_valid(adapter) || out_error_number == NULL ||
        ranges_overlap(
            adapter, adapter->arena_size, out_error_number,
            sizeof(*out_error_number)
        ) ||
        meiso_composition_media_transport_snapshot(
            adapter->transport, &transport_snapshot
        ) != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK ||
        ranges_overlap(
            adapter->transport, transport_snapshot.arena_bytes,
            out_error_number, sizeof(*out_error_number)
        ) || !adapter->terminal) {
        return false;
    }
    *out_error_number = adapter->last_error;
    return true;
}

bool meiso_composition_media_posix_tcp_snapshot(
    const MeisoCompositionMediaPosixTcp *adapter,
    MeisoCompositionMediaPosixTcpSnapshot *out_snapshot
)
{
    MeisoCompositionMediaPosixTcpSnapshot snapshot;
    MeisoCompositionMediaTransportSnapshot transport_snapshot;
    if (!adapter_valid(adapter) || out_snapshot == NULL ||
        ranges_overlap(
            adapter, adapter->arena_size, out_snapshot,
            sizeof(*out_snapshot)
        ) ||
        meiso_composition_media_transport_snapshot(
            adapter->transport, &transport_snapshot
        ) != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK ||
        ranges_overlap(
            adapter->transport, transport_snapshot.arena_bytes,
            out_snapshot, sizeof(*out_snapshot)
        )) {
        return false;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.local_role = adapter->local_role;
    snapshot.scope = adapter->scope;
    snapshot.terminal = adapter->terminal;
    snapshot.terminal_reason = adapter->terminal_reason;
    snapshot.last_error = adapter->last_error;
    snapshot.rx_bytes = adapter->rx_size;
    snapshot.rx_expected_bytes = adapter->rx_expected;
    snapshot.tx_packet_borrowed = adapter->tx_borrowed;
    if (adapter->tx_borrowed) {
        snapshot.tx_packet = adapter->tx_packet.ref;
        snapshot.tx_bytes_sent = adapter->tx_sent;
        snapshot.tx_packet_bytes = adapter->tx_packet.bytes.size;
    }
    snapshot.receive_status_present = adapter->receive_status_present;
    if (adapter->receive_status_present) {
        snapshot.last_receive_status = adapter->last_receive_status;
    }
    snapshot.arena_bytes = adapter->arena_size;
    *out_snapshot = snapshot;
    return true;
}
