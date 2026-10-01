#ifndef MEISO_ADAPTERS_WINDOWS_DISPLAY_INGRESS_MEDIA_UDP_H
#define MEISO_ADAPTERS_WINDOWS_DISPLAY_INGRESS_MEDIA_UDP_H

#include <meiso/profiles/display_ingress/media_transport.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_MAX_DATAGRAM_BYTES 1200U
#define MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_MAX_FRAGMENT_BYTES             \
    (MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_MAX_DATAGRAM_BYTES -             \
     MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE)

typedef struct MeisoDisplayIngressMediaWindowsUdp
    MeisoDisplayIngressMediaWindowsUdp;

typedef struct {
    uintptr_t connected_socket; /* Winsock SOCKET, never a CRT fd. */
    MeisoDisplayIngressMediaTransport *transport;
    MeisoDisplayIngressMediaBinding binding;
} MeisoDisplayIngressMediaWindowsUdpConfig;

/* Network errors lose one datagram, not the Session. These classes are local
 * diagnostics, not wire values. All other I/O errors retain their existing
 * handling (including READ_TRUNCATED for receive WSAEMSGSIZE). */
typedef uint8_t MeisoDisplayIngressMediaWindowsUdpDropClass;
enum {
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_DROP_HOST_UNREACHABLE = 0, /* WSAEHOSTUNREACH */
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_DROP_NETWORK_UNREACHABLE = 1, /* WSAENETUNREACH */
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_DROP_NETWORK_DOWN = 2, /* WSAENETDOWN */
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_DROP_REFUSED = 3, /* WSAECONNRESET: UDP ICMP port unreachable */
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_DROP_NO_BUFFERS = 4, /* WSAENOBUFS */
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_DROP_CLASS_COUNT = 5
};

typedef uint8_t MeisoDisplayIngressMediaWindowsUdpInitStatus;
enum {
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_INIT_OK = 0,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_INIT_INVALID_ARGUMENT = 1,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_INIT_INVALID_CONFIG = 2,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_INIT_SOCKET_ERROR = 3,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_INIT_MISALIGNED_ARENA = 5,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_INIT_ARENA_TOO_SMALL = 6,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_INIT_BINDING_MISMATCH = 7
};

typedef uint8_t MeisoDisplayIngressMediaWindowsUdpPrepareStatus;
enum {
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_PREPARE_READY = 0,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_PREPARE_IDLE = 1,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_PREPARE_TERMINAL = 2,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_PREPARE_TRANSPORT_INVALID = 3,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_PREPARE_INVALID_ARGUMENT = 4,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_PREPARE_INVALID_STATE = 5
};

typedef uint8_t MeisoDisplayIngressMediaWindowsUdpReadStatus;
enum {
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_READ_PACKET_ACCEPTED = 0,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_READ_FRAGMENT_ACCEPTED = 1,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_READ_FRAME_PUBLISHED = 2,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_READ_REASSEMBLY_REPLACED = 3,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_READ_PACKET_REJECTED = 4,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_READ_TRUNCATED = 5,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_READ_WOULD_BLOCK = 6,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_READ_INTERRUPTED = 7,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_READ_TRANSPORT_LOST = 8,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_READ_INVALID_ARGUMENT = 9,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_READ_INVALID_STATE = 10,
    /* A network error instead of a datagram: counted, read again. */
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_READ_DROPPED = 11
};

typedef uint8_t MeisoDisplayIngressMediaWindowsUdpWriteStatus;
enum {
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_WRITE_PACKET_SENT = 0,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_WRITE_WOULD_BLOCK = 1,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_WRITE_INTERRUPTED = 2,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_WRITE_IDLE = 3,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_WRITE_TRANSPORT_LOST = 4,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_WRITE_TRANSPORT_INVALID = 5,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_WRITE_INVALID_ARGUMENT = 6,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_WRITE_INVALID_STATE = 7,
    /* Lost and consumed: prepare the next packet, do not retry this one. */
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_WRITE_DROPPED = 8
};

typedef uint8_t MeisoDisplayIngressMediaWindowsUdpTerminateStatus;
enum {
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_TERMINATE_OK = 0,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_TERMINATE_ALREADY = 1,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_TERMINATE_TRANSPORT_INVALID = 2,
    MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_TERMINATE_INVALID_ARGUMENT = 3
};

typedef struct {
    MeisoSessionRole local_role;
    MeisoDisplayIngressMediaBinding binding;
    bool terminal;
    MeisoDisplayIngressMediaTerminalReason terminal_reason;
    int last_error;
    bool tx_packet_borrowed;
    MeisoDisplayIngressMediaPacketRef tx_packet;
    bool receive_status_present;
    MeisoDisplayIngressMediaTransportStatus last_receive_status;
    uint64_t datagrams_sent;
    uint64_t datagrams_received;
    uint64_t datagrams_rejected;
    uint64_t datagrams_truncated;
    size_t arena_bytes;
    uint64_t datagrams_dropped;
    uint64_t receive_errors_dropped;
    uint64_t drops_by_class
        [MEISO_DISPLAY_INGRESS_MEDIA_WINDOWS_UDP_DROP_CLASS_COUNT];
    int last_drop_error;
} MeisoDisplayIngressMediaWindowsUdpSnapshot;

/*
 * received counts every datagram removed from the socket, including rejected
 * and truncated datagrams. rejected excludes truncated datagrams. sent counts
 * every complete datagram accepted by the local kernel, including the
 * unrecoverable case where subsequent local packet completion detects an
 * internal contract failure. datagrams_dropped counts network send errors,
 * excluded from sent. receive_errors_dropped counts network receive errors,
 * not received datagrams. drops_by_class counts both (each adapter has one
 * direction); last_drop_error is their latest WSA code, initially zero.
 * Drop counters saturate. last_error remains terminal-only.
 */

/* False leaves a non-NULL out_class unchanged; NULL is valid for classification. */
bool meiso_display_ingress_media_windows_udp_drop_class(
    int error_number,
    MeisoDisplayIngressMediaWindowsUdpDropClass *out_class
);
/* WSA symbolic name for known diagnostics, NULL otherwise. Log with the number. */
const char *meiso_display_ingress_media_windows_udp_error_name(int error_number);

size_t meiso_display_ingress_media_windows_udp_alignment_required(void);
size_t meiso_display_ingress_media_windows_udp_memory_required(
    const MeisoDisplayIngressMediaWindowsUdpConfig *config
);

/*
 * Profile-local controlled bring-up adapter for one caller-created,
 * already-connected AF_INET/AF_INET6 SOCK_DGRAM socket.
 * The caller owns WSAStartup/WSACleanup. After all other validation, init
 * sets FIONBIO=1; the caller must not restore blocking mode.
 * One complete DIFR packet is one UDP datagram. The adapter creates no socket,
 * thread, heap object, OPEN/BIND exchange, acknowledgement or retransmission.
 * Host transports are send-only and Edge transports are receive-only.
 *
 * socket and transport are borrowed and must outlive the adapter. The binding must
 * equal transport.snapshot.binding. The adapter accepts only a fresh ACTIVE
 * transport with no pending packet, frame, reassembly, sequence or drop state.
 * Its arena, config, output and full transport arena must be disjoint. One
 * owner serializes all adapter/transport calls; while bound, only the adapter
 * calls next_outbound, complete_outbound, receive_packet or terminate.
 *
 * The caller must use a fresh connected UDP 4-tuple (local IP/port plus peer
 * IP/port) for every fresh Session/media object. Tuple reuse and resume are
 * unsupported. This raw UDP adapter does not authenticate or protect bytes.
 * Only init sets nonblocking mode. No API shuts down or closes the socket.
 */
MeisoDisplayIngressMediaWindowsUdpInitStatus
meiso_display_ingress_media_windows_udp_init(
    void *arena,
    size_t arena_size,
    const MeisoDisplayIngressMediaWindowsUdpConfig *config,
    MeisoDisplayIngressMediaWindowsUdp **out_adapter
);

/* Level-triggered interests for a caller-owned event loop. */
bool meiso_display_ingress_media_windows_udp_wants_read(
    const MeisoDisplayIngressMediaWindowsUdp *adapter
);
bool meiso_display_ingress_media_windows_udp_wants_write(
    const MeisoDisplayIngressMediaWindowsUdp *adapter
);

/*
 * Host-only. READY borrows one exact idempotent DIFR packet but sends nothing.
 * Repeated prepare is idempotent until send completion or WRITE_DROPPED.
 */
MeisoDisplayIngressMediaWindowsUdpPrepareStatus
meiso_display_ingress_media_windows_udp_prepare_outbound(
    MeisoDisplayIngressMediaWindowsUdp *adapter
);

/*
 * Edge-only. Performs at most one recv and never passes a truncated
 * datagram to the media receiver. admission is the caller's Core-style local
 * deadline mapping assertion for this receive attempt. WSAEWOULDBLOCK and
 * WSAEINTR leave both adapter and transport byte-exact unchanged.
 * Drop-class network errors return READ_DROPPED and update diagnostics only;
 * they do not discard in-progress reassembly or count a received datagram.
 */
MeisoDisplayIngressMediaWindowsUdpReadStatus
meiso_display_ingress_media_windows_udp_on_readable(
    MeisoDisplayIngressMediaWindowsUdp *adapter,
    const MeisoStreamAdmission *admission
);

/*
 * Host-only. Performs at most one atomic datagram send. A full send or a
 * drop-class network error completes the transport packet and clears its
 * borrow; WRITE_DROPPED does not count as sent. WSAEWOULDBLOCK and WSAEINTR
 * preserve the exact borrow and all transport cursors. A short send or other
 * I/O error makes adapter and media transport terminal.
 */
MeisoDisplayIngressMediaWindowsUdpWriteStatus
meiso_display_ingress_media_windows_udp_on_writable(
    MeisoDisplayIngressMediaWindowsUdp *adapter
);

/*
 * Terminates the exact bound media object, clears the packet borrow and RX
 * buffer, but never closes the caller-owned socket.
 */
MeisoDisplayIngressMediaWindowsUdpTerminateStatus
meiso_display_ingress_media_windows_udp_terminate(
    MeisoDisplayIngressMediaWindowsUdp *adapter,
    MeisoDisplayIngressMediaTerminalReason reason,
    int error_number
);

/*
 * error_number is an Winsock-style nonnegative diagnostic. last_error succeeds
 * only after terminal. Pure-query failure leaves disjoint output byte-exact
 * unchanged.
 */
bool meiso_display_ingress_media_windows_udp_last_error(
    const MeisoDisplayIngressMediaWindowsUdp *adapter,
    int *out_error_number
);
bool meiso_display_ingress_media_windows_udp_snapshot(
    const MeisoDisplayIngressMediaWindowsUdp *adapter,
    MeisoDisplayIngressMediaWindowsUdpSnapshot *out_snapshot
);

#ifdef __cplusplus
}
#endif

#endif
