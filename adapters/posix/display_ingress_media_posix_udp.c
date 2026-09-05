#define _POSIX_C_SOURCE 200809L

#include <meiso/adapters/posix/display_ingress_media_udp.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdalign.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

enum {
    DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAGIC = 0x44555031U /* DUP1 */
};

struct MeisoDisplayIngressMediaPosixUdp {
    MeisoDisplayIngressMediaTransport *transport;
    MeisoDisplayIngressMediaBinding binding;
    MeisoDisplayIngressMediaPacketView tx_packet;
    size_t arena_size;
    size_t rx_offset;
    uint64_t datagrams_sent;
    uint64_t datagrams_received;
    uint64_t datagrams_rejected;
    uint64_t datagrams_truncated;
    uint32_t magic;
    int connected_fd;
    int last_error;
    MeisoSessionRole local_role;
    MeisoDisplayIngressMediaTerminalReason terminal_reason;
    MeisoDisplayIngressMediaTransportStatus last_receive_status;
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
    const uintptr_t lhs_begin = (uintptr_t)lhs;
    const uintptr_t rhs_begin = (uintptr_t)rhs;

    if (lhs == NULL || rhs == NULL || lhs_size == 0U || rhs_size == 0U) {
        return false;
    }
    if (lhs_begin > UINTPTR_MAX - lhs_size ||
        rhs_begin > UINTPTR_MAX - rhs_size) {
        return true;
    }
    return lhs_begin < rhs_begin + rhs_size &&
        rhs_begin < lhs_begin + lhs_size;
}

static bool nonce_equal(MeisoSessionNonce lhs, MeisoSessionNonce rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

static bool route_equal(
    const MeisoRuntimeRouteKey *lhs,
    const MeisoRuntimeRouteKey *rhs
)
{
    return lhs->catalog_generation == rhs->catalog_generation &&
        meiso_endpoint_id_equal(lhs->endpoint_id, rhs->endpoint_id) &&
        lhs->endpoint_incarnation == rhs->endpoint_incarnation &&
        meiso_resource_id_equal(lhs->resource_id, rhs->resource_id) &&
        lhs->facet == rhs->facet;
}

static bool binding_equal(
    const MeisoDisplayIngressMediaBinding *lhs,
    const MeisoDisplayIngressMediaBinding *rhs
)
{
    return meiso_session_id_equal(
               lhs->runtime.session_id, rhs->runtime.session_id
           ) &&
        route_equal(&lhs->runtime.route, &rhs->runtime.route) &&
        meiso_resource_descriptor_equal(
            &lhs->runtime.resource, &rhs->runtime.resource
        ) &&
        lhs->runtime.production_epoch == rhs->runtime.production_epoch &&
        lhs->media_stream_epoch == rhs->media_stream_epoch &&
        nonce_equal(lhs->channel_nonce, rhs->channel_nonce) &&
        meiso_owner_scope_equal(lhs->producer, rhs->producer);
}

static bool terminal_reason_valid(
    MeisoDisplayIngressMediaTerminalReason reason
)
{
    return reason >= MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_LOCAL_CLOSE &&
        reason <= MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_PROTOCOL_BINDING;
}

static bool transport_snapshot_fresh(
    const MeisoDisplayIngressMediaPosixUdpConfig *config,
    const MeisoDisplayIngressMediaTransportSnapshot *snapshot
)
{
    return snapshot->phase == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_ACTIVE &&
        snapshot->terminal_reason == 0U &&
        binding_equal(&config->binding, &snapshot->binding) &&
        snapshot->max_fragment_payload_bytes > 0U &&
        snapshot->max_fragment_payload_bytes <=
            MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_FRAGMENT_BYTES &&
        !snapshot->outbound_packet_pending &&
        !snapshot->outbound_preemption_pending &&
        snapshot->outbound_packet_serial_high_water == 0U &&
        !snapshot->send_in_flight && !snapshot->send_latest &&
        snapshot->send_frame_sequence == 0U &&
        snapshot->send_fragment_offset == 0U &&
        snapshot->submitted_sequence_high_water == 0U &&
        snapshot->submitted_producer_drop_high_water == 0U &&
        snapshot->transport_whole_frames_dropped == 0U &&
        !snapshot->receive_reassembly && !snapshot->receive_latest &&
        snapshot->receive_frame_sequence == 0U &&
        snapshot->receive_next_offset == 0U &&
        snapshot->observed_sequence_high_water == 0U &&
        snapshot->published_sequence_high_water == 0U &&
        snapshot->sequence_gap_frames == 0U &&
        snapshot->producer_drop_high_water == 0U &&
        snapshot->reassembly_frames_dropped == 0U &&
        snapshot->unread_latest_frames_dropped == 0U &&
        snapshot->expired_frames_dropped == 0U;
}

static bool config_value_valid(
    const MeisoDisplayIngressMediaPosixUdpConfig *config,
    MeisoDisplayIngressMediaTransportSnapshot *out_snapshot
)
{
    MeisoDisplayIngressMediaTransportSnapshot snapshot;

    if (config == NULL || config->connected_fd < 0 ||
        config->transport == NULL ||
        meiso_display_ingress_media_transport_snapshot(
            config->transport, &snapshot
        ) != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK ||
        (snapshot.local_role != MEISO_SESSION_ROLE_HOST &&
         snapshot.local_role != MEISO_SESSION_ROLE_EDGE) ||
        !transport_snapshot_fresh(config, &snapshot) ||
        ranges_overlap(
            config, sizeof(*config), config->transport, snapshot.arena_bytes
        )) {
        return false;
    }
    if (out_snapshot != NULL) {
        *out_snapshot = snapshot;
    }
    return true;
}

static bool socket_is_connected_nonblocking_datagram(
    int fd,
    bool *out_nonblocking,
    bool *out_fd_error
)
{
    struct sockaddr_storage peer;
    socklen_t peer_size = (socklen_t)sizeof(peer);
    socklen_t type_size = (socklen_t)sizeof(int);
    int flags;
    int type;

    if (out_nonblocking == NULL || out_fd_error == NULL) {
        return false;
    }
    *out_nonblocking = false;
    *out_fd_error = false;
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 ||
        getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_size) != 0 ||
        getpeername(fd, (struct sockaddr *)(void *)&peer, &peer_size) != 0 ||
        type != SOCK_DGRAM ||
        (peer.ss_family != AF_INET && peer.ss_family != AF_INET6)) {
        *out_fd_error = true;
        return false;
    }
    *out_nonblocking = (flags & O_NONBLOCK) != 0;
    return *out_nonblocking;
}

static uint8_t *rx_bytes(MeisoDisplayIngressMediaPosixUdp *adapter)
{
    return (uint8_t *)adapter + adapter->rx_offset;
}

static void clear_tx(MeisoDisplayIngressMediaPosixUdp *adapter)
{
    memset(&adapter->tx_packet, 0, sizeof(adapter->tx_packet));
    adapter->tx_borrowed = false;
}

static bool adapter_valid(
    const MeisoDisplayIngressMediaPosixUdp *adapter,
    MeisoDisplayIngressMediaTransportSnapshot *out_transport_snapshot
)
{
    MeisoDisplayIngressMediaTransportSnapshot snapshot;
    size_t rx_end;

    if (adapter == NULL ||
        adapter->magic != DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAGIC ||
        adapter->transport == NULL || adapter->connected_fd < 0 ||
        adapter->arena_size < sizeof(*adapter) ||
        adapter->rx_offset != sizeof(*adapter) ||
        !add_size(
            adapter->rx_offset,
            MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_DATAGRAM_BYTES,
            &rx_end
        ) ||
        rx_end != adapter->arena_size ||
        (adapter->local_role != MEISO_SESSION_ROLE_HOST &&
         adapter->local_role != MEISO_SESSION_ROLE_EDGE) ||
        (adapter->terminal &&
         !terminal_reason_valid(adapter->terminal_reason)) ||
        (!adapter->terminal && adapter->terminal_reason != 0U) ||
        meiso_display_ingress_media_transport_snapshot(
            adapter->transport, &snapshot
        ) != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK ||
        snapshot.local_role != adapter->local_role ||
        !binding_equal(&snapshot.binding, &adapter->binding) ||
        ranges_overlap(
            adapter, adapter->arena_size, adapter->transport,
            snapshot.arena_bytes
        ) ||
        (adapter->terminal &&
         snapshot.phase !=
             MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL) ||
        (adapter->tx_borrowed &&
         (adapter->local_role != MEISO_SESSION_ROLE_HOST ||
          adapter->tx_packet.bytes.data == NULL ||
          adapter->tx_packet.bytes.size == 0U ||
          adapter->tx_packet.bytes.size >
              MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_DATAGRAM_BYTES ||
          adapter->tx_packet.ref.packet_bytes !=
              adapter->tx_packet.bytes.size ||
          !binding_equal(
              &adapter->tx_packet.ref.binding, &adapter->binding
          )))) {
        return false;
    }
    if (out_transport_snapshot != NULL) {
        *out_transport_snapshot = snapshot;
    }
    return true;
}

static void zero_rx(MeisoDisplayIngressMediaPosixUdp *adapter)
{
    memset(
        rx_bytes(adapter), 0,
        MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_DATAGRAM_BYTES
    );
}

static bool synchronize_terminal(
    MeisoDisplayIngressMediaPosixUdp *adapter,
    int error_number
)
{
    MeisoDisplayIngressMediaTransportSnapshot snapshot;

    if (!adapter_valid(adapter, &snapshot)) {
        return false;
    }
    if (snapshot.phase !=
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return true;
    }
    zero_rx(adapter);
    clear_tx(adapter);
    if (!adapter->terminal) {
        adapter->terminal = true;
        adapter->terminal_reason = snapshot.terminal_reason;
        adapter->last_error = error_number;
    }
    return true;
}

static bool mark_terminal(
    MeisoDisplayIngressMediaPosixUdp *adapter,
    MeisoDisplayIngressMediaTerminalReason reason,
    int error_number
)
{
    MeisoDisplayIngressMediaTransportSnapshot snapshot;
    MeisoDisplayIngressMediaTransportStatus status;

    if (!adapter_valid(adapter, &snapshot)) {
        return false;
    }
    if (snapshot.phase ==
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        return synchronize_terminal(adapter, error_number);
    }
    status = meiso_display_ingress_media_transport_terminate(
        adapter->transport, &adapter->binding, reason
    );
    if (status != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK) {
        return false;
    }
    return synchronize_terminal(adapter, error_number);
}

size_t meiso_display_ingress_media_posix_udp_alignment_required(void)
{
    return alignof(max_align_t);
}

size_t meiso_display_ingress_media_posix_udp_memory_required(
    const MeisoDisplayIngressMediaPosixUdpConfig *config
)
{
    size_t required;

    if (!config_value_valid(config, NULL) ||
        !add_size(
            sizeof(MeisoDisplayIngressMediaPosixUdp),
            MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_DATAGRAM_BYTES,
            &required
        )) {
        return 0U;
    }
    return required;
}

MeisoDisplayIngressMediaPosixUdpInitStatus
meiso_display_ingress_media_posix_udp_init(
    void *arena,
    size_t arena_size,
    const MeisoDisplayIngressMediaPosixUdpConfig *config,
    MeisoDisplayIngressMediaPosixUdp **out_adapter
)
{
    MeisoDisplayIngressMediaTransportSnapshot snapshot;
    MeisoDisplayIngressMediaPosixUdp value;
    MeisoDisplayIngressMediaPosixUdp *adapter;
    size_t required;
    bool nonblocking;
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
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_INVALID_ARGUMENT;
    }
    if (meiso_display_ingress_media_transport_snapshot(
            config->transport, &snapshot
        ) != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_INVALID_CONFIG;
    }
    if (!binding_equal(&config->binding, &snapshot.binding)) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_BINDING_MISMATCH;
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
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_INVALID_ARGUMENT;
    }
    if (!config_value_valid(config, &snapshot)) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_INVALID_CONFIG;
    }
    if ((uintptr_t)arena %
        meiso_display_ingress_media_posix_udp_alignment_required() != 0U) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_MISALIGNED_ARENA;
    }
    required = meiso_display_ingress_media_posix_udp_memory_required(config);
    if (required == 0U || arena_size < required) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_ARENA_TOO_SMALL;
    }
    if (!socket_is_connected_nonblocking_datagram(
            config->connected_fd, &nonblocking, &fd_error
        )) {
        return fd_error
            ? MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_FD_ERROR
            : MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_NONBLOCKING_REQUIRED;
    }
    (void)nonblocking;

    memset(&value, 0, sizeof(value));
    value.transport = config->transport;
    value.binding = config->binding;
    value.arena_size = required;
    value.rx_offset = sizeof(MeisoDisplayIngressMediaPosixUdp);
    value.magic = DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAGIC;
    value.connected_fd = config->connected_fd;
    value.local_role = snapshot.local_role;

    memset(arena, 0, required);
    adapter = (MeisoDisplayIngressMediaPosixUdp *)arena;
    *adapter = value;
    *out_adapter = adapter;
    return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_OK;
}

bool meiso_display_ingress_media_posix_udp_wants_read(
    const MeisoDisplayIngressMediaPosixUdp *adapter
)
{
    MeisoDisplayIngressMediaTransportSnapshot snapshot;

    return adapter_valid(adapter, &snapshot) && !adapter->terminal &&
        snapshot.phase == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_ACTIVE &&
        adapter->local_role == MEISO_SESSION_ROLE_EDGE;
}

bool meiso_display_ingress_media_posix_udp_wants_write(
    const MeisoDisplayIngressMediaPosixUdp *adapter
)
{
    MeisoDisplayIngressMediaTransportSnapshot snapshot;

    return adapter_valid(adapter, &snapshot) && !adapter->terminal &&
        snapshot.phase == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_ACTIVE &&
        adapter->local_role == MEISO_SESSION_ROLE_HOST &&
        adapter->tx_borrowed;
}

MeisoDisplayIngressMediaPosixUdpPrepareStatus
meiso_display_ingress_media_posix_udp_prepare_outbound(
    MeisoDisplayIngressMediaPosixUdp *adapter
)
{
    MeisoDisplayIngressMediaTransportSnapshot snapshot;
    MeisoDisplayIngressMediaPacketView packet;
    MeisoDisplayIngressMediaTransportStatus status;

    if (!adapter_valid(adapter, &snapshot)) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_INVALID_ARGUMENT;
    }
    if (snapshot.phase ==
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        (void)synchronize_terminal(adapter, 0);
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_TERMINAL;
    }
    if (adapter->terminal) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_TERMINAL;
    }
    if (adapter->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_INVALID_STATE;
    }
    if (adapter->tx_borrowed) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_READY;
    }

    status = meiso_display_ingress_media_transport_next_outbound(
        adapter->transport, &packet
    );
    if (status == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_NO_PACKET) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_IDLE;
    }
    if (status == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TERMINAL) {
        (void)synchronize_terminal(adapter, 0);
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_TERMINAL;
    }
    if (status != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK ||
        packet.bytes.data == NULL || packet.bytes.size == 0U ||
        packet.bytes.size >
            MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_DATAGRAM_BYTES ||
        packet.ref.packet_bytes != packet.bytes.size ||
        !binding_equal(&packet.ref.binding, &adapter->binding) ||
        ranges_overlap(
            adapter, adapter->arena_size, packet.bytes.data,
            packet.bytes.size
        )) {
        (void)mark_terminal(
            adapter,
            MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            EPROTO
        );
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_TRANSPORT_INVALID;
    }
    adapter->tx_packet = packet;
    adapter->tx_borrowed = true;
    return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_READY;
}

static MeisoDisplayIngressMediaPosixUdpReadStatus dispatch_received(
    MeisoDisplayIngressMediaPosixUdp *adapter,
    size_t packet_size,
    const MeisoStreamAdmission *admission
)
{
    MeisoDisplayIngressMediaTransportSnapshot snapshot;
    MeisoDisplayIngressMediaTransportStatus status;

    status = meiso_display_ingress_media_transport_receive_packet(
        adapter->transport,
        (MeisoByteView){rx_bytes(adapter), packet_size},
        admission
    );
    adapter->last_receive_status = status;
    adapter->receive_status_present = true;
    if (status != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK &&
        status != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED &&
        status != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAME_PUBLISHED &&
        status !=
            MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_REASSEMBLY_REPLACED) {
        ++adapter->datagrams_rejected;
    }
    zero_rx(adapter);

    if (meiso_display_ingress_media_transport_snapshot(
            adapter->transport, &snapshot
        ) != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK) {
        (void)mark_terminal(
            adapter,
            MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            EPROTO
        );
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRANSPORT_LOST;
    }
    if (snapshot.phase ==
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        (void)synchronize_terminal(adapter, 0);
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRANSPORT_LOST;
    }

    switch (status) {
    case MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK:
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_PACKET_ACCEPTED;
    case MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED:
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAGMENT_ACCEPTED;
    case MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAME_PUBLISHED:
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAME_PUBLISHED;
    case MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_REASSEMBLY_REPLACED:
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_REASSEMBLY_REPLACED;
    case MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT:
    case MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_STATE:
    case MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_COUNTER_EXHAUSTED:
    case MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TERMINAL:
        (void)mark_terminal(
            adapter,
            MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            status == MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_COUNTER_EXHAUSTED
                ? EOVERFLOW
                : EPROTO
        );
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRANSPORT_LOST;
    default:
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_PACKET_REJECTED;
    }
}

MeisoDisplayIngressMediaPosixUdpReadStatus
meiso_display_ingress_media_posix_udp_on_readable(
    MeisoDisplayIngressMediaPosixUdp *adapter,
    const MeisoStreamAdmission *admission
)
{
    MeisoDisplayIngressMediaTransportSnapshot snapshot;
    struct iovec vector;
    struct msghdr message;
    ssize_t received;
    int error_number;

    if (!adapter_valid(adapter, &snapshot) ||
        (admission != NULL &&
         (ranges_overlap(
              adapter, adapter->arena_size, admission, sizeof(*admission)
          ) ||
          ranges_overlap(
              adapter->transport, snapshot.arena_bytes, admission,
              sizeof(*admission)
          )))) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_INVALID_ARGUMENT;
    }
    if (snapshot.phase ==
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        (void)synchronize_terminal(adapter, 0);
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRANSPORT_LOST;
    }
    if (adapter->terminal) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRANSPORT_LOST;
    }
    if (adapter->local_role != MEISO_SESSION_ROLE_EDGE) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_INVALID_STATE;
    }
    if (adapter->datagrams_received == UINT64_MAX ||
        adapter->datagrams_rejected == UINT64_MAX ||
        adapter->datagrams_truncated == UINT64_MAX) {
        (void)mark_terminal(
            adapter,
            MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            EOVERFLOW
        );
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRANSPORT_LOST;
    }

    memset(&message, 0, sizeof(message));
    vector.iov_base = rx_bytes(adapter);
    vector.iov_len =
        MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_DATAGRAM_BYTES;
    message.msg_iov = &vector;
    message.msg_iovlen = 1U;
    received = recvmsg(adapter->connected_fd, &message, 0);
    if (received < 0) {
        error_number = errno;
        if (error_number == EAGAIN || error_number == EWOULDBLOCK) {
            return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_WOULD_BLOCK;
        }
        if (error_number == EINTR) {
            return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_INTERRUPTED;
        }
        (void)mark_terminal(
            adapter,
            MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            error_number
        );
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRANSPORT_LOST;
    }

    ++adapter->datagrams_received;
    if ((message.msg_flags & MSG_TRUNC) != 0 ||
        (uintmax_t)received >
            MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_DATAGRAM_BYTES) {
        ++adapter->datagrams_truncated;
        zero_rx(adapter);
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRUNCATED;
    }
    return dispatch_received(adapter, (size_t)received, admission);
}

MeisoDisplayIngressMediaPosixUdpWriteStatus
meiso_display_ingress_media_posix_udp_on_writable(
    MeisoDisplayIngressMediaPosixUdp *adapter
)
{
    MeisoDisplayIngressMediaTransportSnapshot snapshot;
    MeisoDisplayIngressMediaTransportStatus status;
    ssize_t sent;
    int error_number;

    if (!adapter_valid(adapter, &snapshot)) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_INVALID_ARGUMENT;
    }
    if (snapshot.phase ==
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        (void)synchronize_terminal(adapter, 0);
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_TRANSPORT_LOST;
    }
    if (adapter->terminal) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_TRANSPORT_LOST;
    }
    if (adapter->local_role != MEISO_SESSION_ROLE_HOST) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_INVALID_STATE;
    }
    if (!adapter->tx_borrowed) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_IDLE;
    }
    if (adapter->datagrams_sent == UINT64_MAX) {
        (void)mark_terminal(
            adapter,
            MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            EOVERFLOW
        );
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_TRANSPORT_LOST;
    }

    sent = send(
        adapter->connected_fd, adapter->tx_packet.bytes.data,
        adapter->tx_packet.bytes.size, 0
    );
    if (sent < 0) {
        error_number = errno;
        if (error_number == EAGAIN || error_number == EWOULDBLOCK) {
            return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_WOULD_BLOCK;
        }
        if (error_number == EINTR) {
            return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_INTERRUPTED;
        }
        (void)mark_terminal(
            adapter,
            MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            error_number
        );
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_TRANSPORT_LOST;
    }
    if ((size_t)sent != adapter->tx_packet.bytes.size) {
        (void)mark_terminal(
            adapter,
            MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            EIO
        );
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_TRANSPORT_LOST;
    }

    ++adapter->datagrams_sent;
    status = meiso_display_ingress_media_transport_complete_outbound(
        adapter->transport, &adapter->tx_packet.ref
    );
    if (status != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK) {
        (void)mark_terminal(
            adapter,
            MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            EPROTO
        );
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_TRANSPORT_INVALID;
    }
    clear_tx(adapter);
    return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_PACKET_SENT;
}

MeisoDisplayIngressMediaPosixUdpTerminateStatus
meiso_display_ingress_media_posix_udp_terminate(
    MeisoDisplayIngressMediaPosixUdp *adapter,
    MeisoDisplayIngressMediaTerminalReason reason,
    int error_number
)
{
    MeisoDisplayIngressMediaTransportSnapshot snapshot;

    if (!adapter_valid(adapter, &snapshot) ||
        !terminal_reason_valid(reason) || error_number < 0) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_TERMINATE_INVALID_ARGUMENT;
    }
    if (adapter->terminal || snapshot.phase ==
        MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL) {
        (void)synchronize_terminal(adapter, error_number);
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_TERMINATE_ALREADY;
    }
    if (!mark_terminal(adapter, reason, error_number)) {
        return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_TERMINATE_TRANSPORT_INVALID;
    }
    return MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_TERMINATE_OK;
}

bool meiso_display_ingress_media_posix_udp_last_error(
    const MeisoDisplayIngressMediaPosixUdp *adapter,
    int *out_error_number
)
{
    MeisoDisplayIngressMediaTransportSnapshot snapshot;

    if (!adapter_valid(adapter, &snapshot) || out_error_number == NULL ||
        ranges_overlap(
            adapter, adapter->arena_size, out_error_number,
            sizeof(*out_error_number)
        ) ||
        ranges_overlap(
            adapter->transport, snapshot.arena_bytes, out_error_number,
            sizeof(*out_error_number)
        ) || !adapter->terminal) {
        return false;
    }
    *out_error_number = adapter->last_error;
    return true;
}

bool meiso_display_ingress_media_posix_udp_snapshot(
    const MeisoDisplayIngressMediaPosixUdp *adapter,
    MeisoDisplayIngressMediaPosixUdpSnapshot *out_snapshot
)
{
    MeisoDisplayIngressMediaTransportSnapshot transport_snapshot;
    MeisoDisplayIngressMediaPosixUdpSnapshot snapshot;

    if (!adapter_valid(adapter, &transport_snapshot) ||
        out_snapshot == NULL ||
        ranges_overlap(
            adapter, adapter->arena_size, out_snapshot,
            sizeof(*out_snapshot)
        ) ||
        ranges_overlap(
            adapter->transport, transport_snapshot.arena_bytes,
            out_snapshot, sizeof(*out_snapshot)
        )) {
        return false;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.local_role = adapter->local_role;
    snapshot.binding = adapter->binding;
    snapshot.terminal = adapter->terminal ||
        transport_snapshot.phase ==
            MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL;
    snapshot.terminal_reason = snapshot.terminal
        ? transport_snapshot.terminal_reason
        : 0U;
    snapshot.last_error = adapter->last_error;
    snapshot.tx_packet_borrowed = adapter->tx_borrowed;
    if (adapter->tx_borrowed) {
        snapshot.tx_packet = adapter->tx_packet.ref;
    }
    snapshot.receive_status_present = adapter->receive_status_present;
    snapshot.last_receive_status = adapter->last_receive_status;
    snapshot.datagrams_sent = adapter->datagrams_sent;
    snapshot.datagrams_received = adapter->datagrams_received;
    snapshot.datagrams_rejected = adapter->datagrams_rejected;
    snapshot.datagrams_truncated = adapter->datagrams_truncated;
    snapshot.arena_bytes = adapter->arena_size;
    *out_snapshot = snapshot;
    return true;
}
