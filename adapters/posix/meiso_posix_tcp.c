#define _POSIX_C_SOURCE 200809L

#include "meiso_posix_tcp.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    MEISO_POSIX_TCP_MAGIC = 0x4d505443U
};

struct MeisoPosixTcp {
    MeisoSession *session;
    size_t arena_size;
    size_t rx_offset;
    size_t tx_offset;
    size_t rx_size;
    size_t rx_expected;
    size_t tx_size;
    size_t tx_sent;
    size_t max_io_bytes_per_step;
    uint32_t max_frame_bytes;
    uint32_t magic;
    int connected_fd;
    int last_error;
    bool tx_staged;
    bool transport_terminal;
    uint8_t storage[];
};

typedef uint8_t TxReconcileStatus;
enum {
    TX_RECONCILE_READY = 0,
    TX_RECONCILE_IDLE = 1,
    TX_RECONCILE_TERMINAL = 2
};

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

static bool config_value_valid(const MeisoPosixTcpConfig *config)
{
    MeisoSessionSnapshot snapshot;

    return config != NULL && config->connected_fd >= 0 &&
        config->session != NULL &&
        config->max_frame_bytes >= MEISO_SESSION_MIN_MAX_FRAME_BYTES &&
        config->max_io_bytes_per_step != 0U &&
        meiso_session_snapshot(config->session, &snapshot);
}

static bool adapter_valid(const MeisoPosixTcp *adapter)
{
    MeisoSessionSnapshot snapshot;
    size_t rx_end;
    size_t tx_end;

    if (adapter == NULL || adapter->magic != MEISO_POSIX_TCP_MAGIC ||
        adapter->session == NULL || adapter->connected_fd < 0 ||
        adapter->max_frame_bytes < MEISO_SESSION_MIN_MAX_FRAME_BYTES ||
        adapter->max_io_bytes_per_step == 0U ||
        adapter->arena_size < sizeof(*adapter) ||
        !add_size(adapter->rx_offset, adapter->max_frame_bytes, &rx_end) ||
        !add_size(adapter->tx_offset, adapter->max_frame_bytes, &tx_end) ||
        rx_end > adapter->arena_size || tx_end > adapter->arena_size ||
        adapter->rx_size > adapter->max_frame_bytes ||
        adapter->rx_expected > adapter->max_frame_bytes ||
        adapter->tx_size > adapter->max_frame_bytes ||
        adapter->tx_sent > adapter->tx_size ||
        !meiso_session_snapshot(adapter->session, &snapshot)) {
        return false;
    }
    return true;
}

static uint8_t *rx_bytes(MeisoPosixTcp *adapter)
{
    return (uint8_t *)adapter + adapter->rx_offset;
}

static uint8_t *tx_bytes(MeisoPosixTcp *adapter)
{
    return (uint8_t *)adapter + adapter->tx_offset;
}

static uint32_t get_u32(const uint8_t *source)
{
    return (uint32_t)source[0] << 24U |
        (uint32_t)source[1] << 16U |
        (uint32_t)source[2] << 8U |
        (uint32_t)source[3];
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

static void clear_rx(MeisoPosixTcp *adapter)
{
    adapter->rx_size = 0U;
    adapter->rx_expected = 0U;
}

static void clear_tx(MeisoPosixTcp *adapter)
{
    adapter->tx_size = 0U;
    adapter->tx_sent = 0U;
    adapter->tx_staged = false;
}

static void record_transport_lost(
    MeisoPosixTcp *adapter,
    int error_number
)
{
    if (!adapter->transport_terminal) {
        clear_rx(adapter);
        clear_tx(adapter);
        adapter->last_error = error_number;
        adapter->transport_terminal = true;
        meiso_session_transport_lost(adapter->session);
    }
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

static bool outbound_frame_valid(
    const MeisoPosixTcp *adapter,
    MeisoByteView frame
)
{
    return meiso_byte_view_valid(frame) && frame.size != 0U &&
        frame.size <= adapter->max_frame_bytes;
}

static void stage_frame(MeisoPosixTcp *adapter, MeisoByteView frame)
{
    memmove(tx_bytes(adapter), frame.data, frame.size);
    adapter->tx_size = frame.size;
    adapter->tx_sent = 0U;
    adapter->tx_staged = true;
}

static TxReconcileStatus reconcile_outbound(MeisoPosixTcp *adapter)
{
    MeisoByteView frame;
    const bool pending = meiso_session_next_outbound(
        adapter->session, &frame
    );

    if (!pending) {
        if (adapter->tx_staged && adapter->tx_sent != 0U) {
            record_transport_lost(adapter, EPROTO);
            return TX_RECONCILE_TERMINAL;
        }
        clear_tx(adapter);
        return TX_RECONCILE_IDLE;
    }
    if (!outbound_frame_valid(adapter, frame)) {
        record_transport_lost(adapter, EMSGSIZE);
        return TX_RECONCILE_TERMINAL;
    }
    if (!adapter->tx_staged) {
        stage_frame(adapter, frame);
        return TX_RECONCILE_READY;
    }
    if (adapter->tx_size == frame.size && memcmp(
            tx_bytes(adapter), frame.data, frame.size
        ) == 0) {
        return TX_RECONCILE_READY;
    }
    if (adapter->tx_sent != 0U) {
        record_transport_lost(adapter, EPROTO);
        return TX_RECONCILE_TERMINAL;
    }
    stage_frame(adapter, frame);
    return TX_RECONCILE_READY;
}

static MeisoPosixTcpReadStatus deliver_frame(
    MeisoPosixTcp *adapter,
    MeisoSessionRuntimeMessage *out_runtime
)
{
    const MeisoByteView frame = {rx_bytes(adapter), adapter->rx_size};
    const MeisoSessionReceiveStatus received = meiso_session_receive(
        adapter->session, frame, out_runtime
    );
    const bool old_tx_prefix_sent = adapter->tx_staged &&
        adapter->tx_sent != 0U;

    clear_rx(adapter);
    if (received == MEISO_SESSION_RECEIVE_FAILED ||
        received == MEISO_SESSION_RECEIVE_PEER_CLOSED) {
        if (old_tx_prefix_sent) {
            record_transport_lost(adapter, EPROTO);
        } else {
            clear_tx(adapter);
        }
    }

    switch (received) {
    case MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS:
        return MEISO_POSIX_TCP_READ_SESSION_PROGRESS;
    case MEISO_SESSION_RECEIVE_RUNTIME:
        return MEISO_POSIX_TCP_READ_RUNTIME;
    case MEISO_SESSION_RECEIVE_PEER_CLOSED:
        return MEISO_POSIX_TCP_READ_PEER_CLOSED;
    case MEISO_SESSION_RECEIVE_FAILED:
        return MEISO_POSIX_TCP_READ_SESSION_FAILED;
    case MEISO_SESSION_RECEIVE_INVALID_ARGUMENT:
    default:
        record_transport_lost(adapter, EINVAL);
        return MEISO_POSIX_TCP_READ_INVALID_ARGUMENT;
    }
}

size_t meiso_posix_tcp_alignment_required(void)
{
    return _Alignof(max_align_t);
}

size_t meiso_posix_tcp_memory_required(const MeisoPosixTcpConfig *config)
{
    size_t required;

    if (!config_value_valid(config) ||
        !add_size(sizeof(MeisoPosixTcp), config->max_frame_bytes, &required) ||
        !add_size(required, config->max_frame_bytes, &required)) {
        return 0U;
    }
    return required;
}

MeisoPosixTcpInitStatus meiso_posix_tcp_init(
    void *arena,
    size_t arena_size,
    const MeisoPosixTcpConfig *config,
    MeisoPosixTcp **out_adapter
)
{
    MeisoPosixTcpConfig config_copy;
    MeisoPosixTcp *adapter;
    size_t required;
    bool fd_error;

    if (out_adapter == NULL) {
        return MEISO_POSIX_TCP_INIT_INVALID_ARGUMENT;
    }
    if ((arena != NULL && ranges_overlap(
            arena, arena_size, out_adapter, sizeof(*out_adapter)
        )) || (config != NULL && ranges_overlap(
            config, sizeof(*config), out_adapter, sizeof(*out_adapter)
        )) || (config != NULL && config->session != NULL && ranges_overlap(
            config->session, 1U, out_adapter, sizeof(*out_adapter)
        )) || (arena != NULL && config != NULL && ranges_overlap(
            arena, arena_size, config->session, 1U
        ))) {
        return MEISO_POSIX_TCP_INIT_INVALID_ARGUMENT;
    }
    *out_adapter = NULL;
    if (!config_value_valid(config)) {
        return MEISO_POSIX_TCP_INIT_INVALID_CONFIG;
    }
    if (arena == NULL ||
        (uintptr_t)arena % meiso_posix_tcp_alignment_required() != 0U) {
        return MEISO_POSIX_TCP_INIT_MISALIGNED_ARENA;
    }
    required = meiso_posix_tcp_memory_required(config);
    if (required == 0U || arena_size < required) {
        return MEISO_POSIX_TCP_INIT_ARENA_TOO_SMALL;
    }
    if (!socket_is_nonblocking_stream(config->connected_fd, &fd_error)) {
        return fd_error ? MEISO_POSIX_TCP_INIT_FD_ERROR :
            MEISO_POSIX_TCP_INIT_NONBLOCKING_REQUIRED;
    }
    if (!install_no_sigpipe(config->connected_fd)) {
        return MEISO_POSIX_TCP_INIT_FD_ERROR;
    }

    config_copy = *config;
    memset(arena, 0, required);
    adapter = arena;
    adapter->session = config_copy.session;
    adapter->arena_size = required;
    adapter->rx_offset = sizeof(*adapter);
    adapter->tx_offset = sizeof(*adapter) + config_copy.max_frame_bytes;
    adapter->max_io_bytes_per_step = config_copy.max_io_bytes_per_step;
    adapter->max_frame_bytes = config_copy.max_frame_bytes;
    adapter->connected_fd = config_copy.connected_fd;
    adapter->magic = MEISO_POSIX_TCP_MAGIC;
    *out_adapter = adapter;
    return MEISO_POSIX_TCP_INIT_OK;
}

bool meiso_posix_tcp_wants_read(const MeisoPosixTcp *adapter)
{
    MeisoSessionSnapshot snapshot;

    if (!adapter_valid(adapter) || adapter->transport_terminal ||
        !meiso_session_snapshot(adapter->session, &snapshot)) {
        return false;
    }
    if (snapshot.state == MEISO_SESSION_STATE_FAILED ||
        (snapshot.state == MEISO_SESSION_STATE_CLOSED &&
         snapshot.failure == MEISO_SESSION_FAILURE_PEER_CLOSED)) {
        return false;
    }
    return true;
}

bool meiso_posix_tcp_wants_write(const MeisoPosixTcp *adapter)
{
    MeisoByteView frame;

    if (!adapter_valid(adapter) || adapter->transport_terminal) {
        return false;
    }
    return adapter->tx_staged ||
        meiso_session_next_outbound(adapter->session, &frame);
}

MeisoPosixTcpReadStatus meiso_posix_tcp_on_readable(
    MeisoPosixTcp *adapter,
    MeisoSessionRuntimeMessage *out_runtime
)
{
    size_t target;
    size_t remaining;
    size_t request_size;
    ssize_t received;

    if (!adapter_valid(adapter) || out_runtime == NULL ||
        ranges_overlap(
            adapter, adapter->arena_size, out_runtime, sizeof(*out_runtime)
        ) || ranges_overlap(
            adapter->session, 1U, out_runtime, sizeof(*out_runtime)
        )) {
        return MEISO_POSIX_TCP_READ_INVALID_ARGUMENT;
    }
    if (adapter->transport_terminal) {
        return MEISO_POSIX_TCP_READ_TRANSPORT_LOST;
    }

    target = adapter->rx_expected == 0U ?
        MEISO_SESSION_WIRE_HEADER_SIZE : adapter->rx_expected;
    remaining = target - adapter->rx_size;
    request_size = syscall_request_size(
        remaining, adapter->max_io_bytes_per_step
    );
    received = recv(
        adapter->connected_fd,
        rx_bytes(adapter) + adapter->rx_size,
        request_size,
        0
    );
    if (received > 0) {
        adapter->rx_size += (size_t)received;
        if (adapter->rx_expected == 0U &&
            adapter->rx_size == MEISO_SESSION_WIRE_HEADER_SIZE) {
            const uint32_t claimed_size = get_u32(
                rx_bytes(adapter) + 8U
            );
            if (claimed_size < MEISO_SESSION_WIRE_HEADER_SIZE ||
                claimed_size > adapter->max_frame_bytes) {
                return deliver_frame(adapter, out_runtime);
            }
            adapter->rx_expected = claimed_size;
        }
        if (adapter->rx_expected != 0U &&
            adapter->rx_size == adapter->rx_expected) {
            return deliver_frame(adapter, out_runtime);
        }
        return MEISO_POSIX_TCP_READ_PROGRESS;
    }
    if (received == 0) {
        record_transport_lost(adapter, 0);
        return MEISO_POSIX_TCP_READ_TRANSPORT_LOST;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return MEISO_POSIX_TCP_READ_WOULD_BLOCK;
    }
    if (errno == EINTR) {
        return MEISO_POSIX_TCP_READ_INTERRUPTED;
    }
    record_transport_lost(adapter, errno);
    return MEISO_POSIX_TCP_READ_TRANSPORT_LOST;
}

MeisoPosixTcpWriteStatus meiso_posix_tcp_on_writable(
    MeisoPosixTcp *adapter
)
{
    TxReconcileStatus reconciled;
    size_t remaining;
    size_t request_size;
    ssize_t sent;

    if (!adapter_valid(adapter)) {
        return MEISO_POSIX_TCP_WRITE_INVALID_ARGUMENT;
    }
    if (adapter->transport_terminal) {
        return MEISO_POSIX_TCP_WRITE_TRANSPORT_LOST;
    }
    reconciled = reconcile_outbound(adapter);
    if (reconciled != TX_RECONCILE_READY) {
        return reconciled == TX_RECONCILE_TERMINAL ?
            MEISO_POSIX_TCP_WRITE_TRANSPORT_LOST :
            MEISO_POSIX_TCP_WRITE_IDLE;
    }

    remaining = adapter->tx_size - adapter->tx_sent;
    request_size = syscall_request_size(
        remaining, adapter->max_io_bytes_per_step
    );
    sent = send(
        adapter->connected_fd,
        tx_bytes(adapter) + adapter->tx_sent,
        request_size,
        send_flags()
    );
    if (sent > 0) {
        adapter->tx_sent += (size_t)sent;
        if (adapter->tx_sent == adapter->tx_size) {
            clear_tx(adapter);
            if (meiso_session_mark_outbound_sent(adapter->session) !=
                MEISO_SESSION_OUTBOUND_SENT) {
                record_transport_lost(adapter, EPROTO);
                return MEISO_POSIX_TCP_WRITE_SESSION_INVALID;
            }
            return MEISO_POSIX_TCP_WRITE_FRAME_SENT;
        }
        return MEISO_POSIX_TCP_WRITE_PROGRESS;
    }
    if (sent == 0) {
        record_transport_lost(adapter, EPIPE);
        return MEISO_POSIX_TCP_WRITE_TRANSPORT_LOST;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return MEISO_POSIX_TCP_WRITE_WOULD_BLOCK;
    }
    if (errno == EINTR) {
        return MEISO_POSIX_TCP_WRITE_INTERRUPTED;
    }
    record_transport_lost(adapter, errno);
    return MEISO_POSIX_TCP_WRITE_TRANSPORT_LOST;
}

void meiso_posix_tcp_abort(MeisoPosixTcp *adapter, int error_number)
{
    if (adapter_valid(adapter)) {
        record_transport_lost(adapter, error_number);
    }
}

bool meiso_posix_tcp_last_error(
    const MeisoPosixTcp *adapter,
    int *out_error_number
)
{
    if (!adapter_valid(adapter) || out_error_number == NULL ||
        ranges_overlap(
            adapter, adapter->arena_size,
            out_error_number, sizeof(*out_error_number)
        ) || ranges_overlap(
            adapter->session, 1U,
            out_error_number, sizeof(*out_error_number)
        )) {
        return false;
    }
    *out_error_number = adapter->last_error;
    return adapter->transport_terminal;
}
