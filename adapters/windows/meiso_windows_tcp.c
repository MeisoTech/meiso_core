#include <meiso/adapters/windows/tcp.h>

#include "socket_private.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    MEISO_WINDOWS_TCP_MAGIC = 0x4d505443U
};

struct MeisoWindowsTcp {
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
    SOCKET connected_socket;
    int last_error;
    bool tx_staged;
    bool transport_terminal;
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

static bool config_value_valid(const MeisoWindowsTcpConfig *config)
{
    MeisoSessionSnapshot snapshot;

    return config != NULL && config->connected_socket != (uintptr_t)INVALID_SOCKET &&
        config->session != NULL &&
        config->max_frame_bytes >= MEISO_SESSION_MIN_MAX_FRAME_BYTES &&
        config->max_io_bytes_per_step != 0U &&
        meiso_session_snapshot(config->session, &snapshot);
}

static bool adapter_valid(const MeisoWindowsTcp *adapter)
{
    MeisoSessionSnapshot snapshot;
    size_t rx_end;
    size_t tx_end;

    if (adapter == NULL || adapter->magic != MEISO_WINDOWS_TCP_MAGIC ||
        adapter->session == NULL || adapter->connected_socket == INVALID_SOCKET ||
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

static uint8_t *rx_bytes(MeisoWindowsTcp *adapter)
{
    return (uint8_t *)adapter + adapter->rx_offset;
}

static uint8_t *tx_bytes(MeisoWindowsTcp *adapter)
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
        minimum_size(remaining, configured_limit), (size_t)INT_MAX
    );
}

static void clear_rx(MeisoWindowsTcp *adapter)
{
    adapter->rx_size = 0U;
    adapter->rx_expected = 0U;
}

static void clear_tx(MeisoWindowsTcp *adapter)
{
    adapter->tx_size = 0U;
    adapter->tx_sent = 0U;
    adapter->tx_staged = false;
}

static void record_transport_lost(
    MeisoWindowsTcp *adapter,
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

static bool outbound_frame_valid(
    const MeisoWindowsTcp *adapter,
    MeisoByteView frame
)
{
    return meiso_byte_view_valid(frame) && frame.size != 0U &&
        frame.size <= adapter->max_frame_bytes;
}

static void stage_frame(MeisoWindowsTcp *adapter, MeisoByteView frame)
{
    memmove(tx_bytes(adapter), frame.data, frame.size);
    adapter->tx_size = frame.size;
    adapter->tx_sent = 0U;
    adapter->tx_staged = true;
}

static TxReconcileStatus reconcile_outbound(MeisoWindowsTcp *adapter)
{
    MeisoByteView frame;
    const bool pending = meiso_session_next_outbound(
        adapter->session, &frame
    );

    if (!pending) {
        if (adapter->tx_staged && adapter->tx_sent != 0U) {
            record_transport_lost(adapter, WSAEINVAL);
            return TX_RECONCILE_TERMINAL;
        }
        clear_tx(adapter);
        return TX_RECONCILE_IDLE;
    }
    if (!outbound_frame_valid(adapter, frame)) {
        record_transport_lost(adapter, WSAEMSGSIZE);
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
        record_transport_lost(adapter, WSAEINVAL);
        return TX_RECONCILE_TERMINAL;
    }
    stage_frame(adapter, frame);
    return TX_RECONCILE_READY;
}

static MeisoWindowsTcpReadStatus deliver_frame(
    MeisoWindowsTcp *adapter,
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
            record_transport_lost(adapter, WSAEINVAL);
        } else {
            clear_tx(adapter);
        }
    }

    switch (received) {
    case MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS:
        return MEISO_WINDOWS_TCP_READ_SESSION_PROGRESS;
    case MEISO_SESSION_RECEIVE_RUNTIME:
        return MEISO_WINDOWS_TCP_READ_RUNTIME;
    case MEISO_SESSION_RECEIVE_PEER_CLOSED:
        return MEISO_WINDOWS_TCP_READ_PEER_CLOSED;
    case MEISO_SESSION_RECEIVE_FAILED:
        return MEISO_WINDOWS_TCP_READ_SESSION_FAILED;
    case MEISO_SESSION_RECEIVE_INVALID_ARGUMENT:
    default:
        record_transport_lost(adapter, WSAEINVAL);
        return MEISO_WINDOWS_TCP_READ_INVALID_ARGUMENT;
    }
}

size_t meiso_windows_tcp_alignment_required(void)
{
    return _Alignof(MeisoMaxAlign);
}

size_t meiso_windows_tcp_memory_required(const MeisoWindowsTcpConfig *config)
{
    size_t required;

    if (!config_value_valid(config) ||
        !add_size(sizeof(MeisoWindowsTcp), config->max_frame_bytes, &required) ||
        !add_size(required, config->max_frame_bytes, &required)) {
        return 0U;
    }
    return required;
}

MeisoWindowsTcpInitStatus meiso_windows_tcp_init(
    void *arena,
    size_t arena_size,
    const MeisoWindowsTcpConfig *config,
    MeisoWindowsTcp **out_adapter
)
{
    MeisoWindowsTcpConfig config_copy;
    MeisoWindowsTcp *adapter;
    size_t required;

    if (out_adapter == NULL) {
        return MEISO_WINDOWS_TCP_INIT_INVALID_ARGUMENT;
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
        return MEISO_WINDOWS_TCP_INIT_INVALID_ARGUMENT;
    }
    *out_adapter = NULL;
    if (!config_value_valid(config)) {
        return MEISO_WINDOWS_TCP_INIT_INVALID_CONFIG;
    }
    if (arena == NULL ||
        (uintptr_t)arena % meiso_windows_tcp_alignment_required() != 0U) {
        return MEISO_WINDOWS_TCP_INIT_MISALIGNED_ARENA;
    }
    required = meiso_windows_tcp_memory_required(config);
    if (required == 0U || arena_size < required) {
        return MEISO_WINDOWS_TCP_INIT_ARENA_TOO_SMALL;
    }
    if (!meiso_windows_socket_prepare((SOCKET)config->connected_socket, SOCK_STREAM)) {
        return MEISO_WINDOWS_TCP_INIT_SOCKET_ERROR;
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
    adapter->connected_socket = (SOCKET)config_copy.connected_socket;
    adapter->magic = MEISO_WINDOWS_TCP_MAGIC;
    *out_adapter = adapter;
    return MEISO_WINDOWS_TCP_INIT_OK;
}

bool meiso_windows_tcp_wants_read(const MeisoWindowsTcp *adapter)
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

bool meiso_windows_tcp_wants_write(const MeisoWindowsTcp *adapter)
{
    MeisoByteView frame;

    if (!adapter_valid(adapter) || adapter->transport_terminal) {
        return false;
    }
    return adapter->tx_staged ||
        meiso_session_next_outbound(adapter->session, &frame);
}

MeisoWindowsTcpReadStatus meiso_windows_tcp_on_readable(
    MeisoWindowsTcp *adapter,
    MeisoSessionRuntimeMessage *out_runtime
)
{
    size_t target;
    size_t remaining;
    size_t request_size;
    int received;
    int error_number;

    if (!adapter_valid(adapter) || out_runtime == NULL ||
        ranges_overlap(
            adapter, adapter->arena_size, out_runtime, sizeof(*out_runtime)
        ) || ranges_overlap(
            adapter->session, 1U, out_runtime, sizeof(*out_runtime)
        )) {
        return MEISO_WINDOWS_TCP_READ_INVALID_ARGUMENT;
    }
    if (adapter->transport_terminal) {
        return MEISO_WINDOWS_TCP_READ_TRANSPORT_LOST;
    }

    target = adapter->rx_expected == 0U ?
        MEISO_SESSION_WIRE_HEADER_SIZE : adapter->rx_expected;
    remaining = target - adapter->rx_size;
    request_size = syscall_request_size(
        remaining, adapter->max_io_bytes_per_step
    );
    received = recv(
        adapter->connected_socket,
        (char *)rx_bytes(adapter) + adapter->rx_size,
        (int)request_size,
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
        return MEISO_WINDOWS_TCP_READ_PROGRESS;
    }
    if (received == 0) {
        record_transport_lost(adapter, 0);
        return MEISO_WINDOWS_TCP_READ_TRANSPORT_LOST;
    }
    error_number = WSAGetLastError();
    if (error_number == WSAEWOULDBLOCK) {
        return MEISO_WINDOWS_TCP_READ_WOULD_BLOCK;
    }
    if (error_number == WSAEINTR) {
        return MEISO_WINDOWS_TCP_READ_INTERRUPTED;
    }
    record_transport_lost(adapter, error_number);
    return MEISO_WINDOWS_TCP_READ_TRANSPORT_LOST;
}

MeisoWindowsTcpWriteStatus meiso_windows_tcp_on_writable(
    MeisoWindowsTcp *adapter
)
{
    TxReconcileStatus reconciled;
    size_t remaining;
    size_t request_size;
    int sent;
    int error_number;

    if (!adapter_valid(adapter)) {
        return MEISO_WINDOWS_TCP_WRITE_INVALID_ARGUMENT;
    }
    if (adapter->transport_terminal) {
        return MEISO_WINDOWS_TCP_WRITE_TRANSPORT_LOST;
    }
    reconciled = reconcile_outbound(adapter);
    if (reconciled != TX_RECONCILE_READY) {
        return reconciled == TX_RECONCILE_TERMINAL ?
            MEISO_WINDOWS_TCP_WRITE_TRANSPORT_LOST :
            MEISO_WINDOWS_TCP_WRITE_IDLE;
    }

    remaining = adapter->tx_size - adapter->tx_sent;
    request_size = syscall_request_size(
        remaining, adapter->max_io_bytes_per_step
    );
    sent = send(
        adapter->connected_socket,
        (const char *)tx_bytes(adapter) + adapter->tx_sent,
        (int)request_size,
        0
    );
    if (sent > 0) {
        adapter->tx_sent += (size_t)sent;
        if (adapter->tx_sent == adapter->tx_size) {
            clear_tx(adapter);
            if (meiso_session_mark_outbound_sent(adapter->session) !=
                MEISO_SESSION_OUTBOUND_SENT) {
                record_transport_lost(adapter, WSAEINVAL);
                return MEISO_WINDOWS_TCP_WRITE_SESSION_INVALID;
            }
            return MEISO_WINDOWS_TCP_WRITE_FRAME_SENT;
        }
        return MEISO_WINDOWS_TCP_WRITE_PROGRESS;
    }
    if (sent == 0) {
        record_transport_lost(adapter, WSAECONNRESET);
        return MEISO_WINDOWS_TCP_WRITE_TRANSPORT_LOST;
    }
    error_number = WSAGetLastError();
    if (error_number == WSAEWOULDBLOCK) {
        return MEISO_WINDOWS_TCP_WRITE_WOULD_BLOCK;
    }
    if (error_number == WSAEINTR) {
        return MEISO_WINDOWS_TCP_WRITE_INTERRUPTED;
    }
    record_transport_lost(adapter, error_number);
    return MEISO_WINDOWS_TCP_WRITE_TRANSPORT_LOST;
}

void meiso_windows_tcp_abort(MeisoWindowsTcp *adapter, int error_number)
{
    if (adapter_valid(adapter)) {
        record_transport_lost(adapter, error_number);
    }
}

bool meiso_windows_tcp_last_error(
    const MeisoWindowsTcp *adapter,
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
