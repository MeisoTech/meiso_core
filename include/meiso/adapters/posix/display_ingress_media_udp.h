#ifndef MEISO_ADAPTERS_POSIX_DISPLAY_INGRESS_MEDIA_UDP_H
#define MEISO_ADAPTERS_POSIX_DISPLAY_INGRESS_MEDIA_UDP_H

#include <meiso/profiles/display_ingress/media_transport.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_DATAGRAM_BYTES 1200U
#define MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_FRAGMENT_BYTES             \
    (MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_MAX_DATAGRAM_BYTES -             \
     MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE)

typedef struct MeisoDisplayIngressMediaPosixUdp
    MeisoDisplayIngressMediaPosixUdp;

typedef struct {
    int connected_fd;
    MeisoDisplayIngressMediaTransport *transport;
    MeisoDisplayIngressMediaBinding binding;
} MeisoDisplayIngressMediaPosixUdpConfig;

/*
 * Media is lossy (planes-v12 §8): a connected UDP socket reports the
 * network's trouble as errors, and each of these loses at most the one
 * datagram and never ends the transport. Whether the Session lives is the
 * control connection's to say. They are counted per class. Every other send or
 * receive error (EMSGSIZE, EBADF, EINVAL, ENOTCONN, EACCES, ...) is a
 * configuration or program error that every datagram would hit, and stays
 * terminal with its errno.
 */
typedef uint8_t MeisoDisplayIngressMediaPosixUdpDropClass;
enum {
    /* EHOSTUNREACH: no route, or the neighbour did not answer */
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_DROP_HOST_UNREACHABLE = 0,
    /* ENETUNREACH */
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_DROP_NETWORK_UNREACHABLE = 1,
    /* EHOSTDOWN */
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_DROP_HOST_DOWN = 2,
    /* ENETDOWN: the local interface went down (Wi-Fi left) */
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_DROP_NETWORK_DOWN = 3,
    /* ECONNREFUSED: an ICMP port unreachable came back */
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_DROP_REFUSED = 4,
    /* ENOBUFS */
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_DROP_NO_BUFFERS = 5,
    /* EPERM: a local firewall drop, e.g. nft in OUTPUT */
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_DROP_NOT_PERMITTED = 6,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_DROP_CLASS_COUNT = 7
};

typedef uint8_t MeisoDisplayIngressMediaPosixUdpInitStatus;
enum {
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_OK = 0,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_INVALID_ARGUMENT = 1,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_INVALID_CONFIG = 2,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_FD_ERROR = 3,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_NONBLOCKING_REQUIRED = 4,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_MISALIGNED_ARENA = 5,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_ARENA_TOO_SMALL = 6,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_INIT_BINDING_MISMATCH = 7
};

typedef uint8_t MeisoDisplayIngressMediaPosixUdpPrepareStatus;
enum {
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_READY = 0,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_IDLE = 1,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_TERMINAL = 2,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_TRANSPORT_INVALID = 3,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_INVALID_ARGUMENT = 4,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_INVALID_STATE = 5
};

typedef uint8_t MeisoDisplayIngressMediaPosixUdpReadStatus;
enum {
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_PACKET_ACCEPTED = 0,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAGMENT_ACCEPTED = 1,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAME_PUBLISHED = 2,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_REASSEMBLY_REPLACED = 3,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_PACKET_REJECTED = 4,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRUNCATED = 5,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_WOULD_BLOCK = 6,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_INTERRUPTED = 7,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_TRANSPORT_LOST = 8,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_INVALID_ARGUMENT = 9,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_INVALID_STATE = 10,
    /* A drop-class error came instead of a datagram: counted, read again. */
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_DROPPED = 11
};

typedef uint8_t MeisoDisplayIngressMediaPosixUdpWriteStatus;
enum {
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_PACKET_SENT = 0,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_WOULD_BLOCK = 1,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_INTERRUPTED = 2,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_IDLE = 3,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_TRANSPORT_LOST = 4,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_TRANSPORT_INVALID = 5,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_INVALID_ARGUMENT = 6,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_INVALID_STATE = 7,
    /* The datagram was lost to a drop-class error: counted, the packet is
     * done and the next one follows (not WOULD_BLOCK, which sends it again). */
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_DROPPED = 8
};

typedef uint8_t MeisoDisplayIngressMediaPosixUdpTerminateStatus;
enum {
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_TERMINATE_OK = 0,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_TERMINATE_ALREADY = 1,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_TERMINATE_TRANSPORT_INVALID = 2,
    MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_TERMINATE_INVALID_ARGUMENT = 3
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
        [MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_DROP_CLASS_COUNT];
    int last_drop_error;
} MeisoDisplayIngressMediaPosixUdpSnapshot;

/*
 * received counts every datagram removed from the socket, including rejected
 * and truncated datagrams. rejected excludes truncated datagrams. sent counts
 * every complete datagram accepted by the local kernel, including the
 * unrecoverable case where subsequent local packet completion detects an
 * internal contract failure. dropped counts datagrams lost to a drop-class
 * send error (not in sent); receive_errors_dropped counts drop-class receive
 * errors (no datagram removed, not in received). drops_by_class counts both by
 * class (an adapter only sends or only receives); last_drop_error is the
 * latest one's errno, 0 before any. Drop counters saturate.
 */

/* The class of a drop-class errno; false for any other value. */
bool meiso_display_ingress_media_posix_udp_drop_class(
    int error_number,
    MeisoDisplayIngressMediaPosixUdpDropClass *out_class
);
/* The errno of a drop class; 0 for an invalid class, or one the platform's
 * errno.h does not have (EHOSTDOWN is not POSIX). */
int meiso_display_ingress_media_posix_udp_drop_class_error(
    MeisoDisplayIngressMediaPosixUdpDropClass drop_class
);
/* "EHOSTUNREACH" and the like for the errnos this adapter reports (the drop
 * classes, the syscall retries and the usual terminal ones); NULL for others.
 * Diagnostics print it with the number. */
const char *meiso_display_ingress_media_posix_udp_error_name(int error_number);

size_t meiso_display_ingress_media_posix_udp_alignment_required(void);
size_t meiso_display_ingress_media_posix_udp_memory_required(
    const MeisoDisplayIngressMediaPosixUdpConfig *config
);

/*
 * Profile-local controlled bring-up adapter for one caller-created,
 * already-connected, nonblocking AF_INET/AF_INET6 SOCK_DGRAM descriptor.
 * One complete DIFR packet is one UDP datagram. The adapter creates no socket,
 * thread, heap object, OPEN/BIND exchange, acknowledgement or retransmission.
 * Host transports are send-only and Edge transports are receive-only.
 *
 * fd and transport are borrowed and must outlive the adapter. The binding must
 * equal transport.snapshot.binding. The adapter accepts only a fresh ACTIVE
 * transport with no pending packet, frame, reassembly, sequence or drop state.
 * Its arena, config, output and full transport arena must be disjoint. One
 * owner serializes all adapter/transport calls; while bound, only the adapter
 * calls next_outbound, complete_outbound, receive_packet or terminate.
 *
 * The caller must use a fresh connected UDP 4-tuple (local IP/port plus peer
 * IP/port) for every fresh Session/media object. Tuple reuse and resume are
 * unsupported. This raw UDP adapter does not authenticate or protect bytes.
 * No API changes fd flags, shuts down, or closes the descriptor.
 */
MeisoDisplayIngressMediaPosixUdpInitStatus
meiso_display_ingress_media_posix_udp_init(
    void *arena,
    size_t arena_size,
    const MeisoDisplayIngressMediaPosixUdpConfig *config,
    MeisoDisplayIngressMediaPosixUdp **out_adapter
);

/* Level-triggered interests for a caller-owned event loop. */
bool meiso_display_ingress_media_posix_udp_wants_read(
    const MeisoDisplayIngressMediaPosixUdp *adapter
);
bool meiso_display_ingress_media_posix_udp_wants_write(
    const MeisoDisplayIngressMediaPosixUdp *adapter
);

/*
 * Host-only. READY borrows one exact idempotent DIFR packet but sends nothing.
 * Repeated prepare is idempotent until successful send completion.
 */
MeisoDisplayIngressMediaPosixUdpPrepareStatus
meiso_display_ingress_media_posix_udp_prepare_outbound(
    MeisoDisplayIngressMediaPosixUdp *adapter
);

/*
 * Edge-only. Performs at most one recvmsg and never passes a truncated
 * datagram to the media receiver. admission is the caller's Core-style local
 * deadline mapping assertion for this receive attempt. EAGAIN/EWOULDBLOCK and
 * EINTR leave both adapter and transport byte-exact unchanged.
 */
MeisoDisplayIngressMediaPosixUdpReadStatus
meiso_display_ingress_media_posix_udp_on_readable(
    MeisoDisplayIngressMediaPosixUdp *adapter,
    const MeisoStreamAdmission *admission
);

/*
 * Host-only. Performs at most one atomic datagram send. Only a full successful
 * send completes the transport packet. EAGAIN/EWOULDBLOCK and EINTR preserve
 * the exact borrow and all transport cursors. A short send or other I/O error
 * makes adapter and media transport terminal.
 */
MeisoDisplayIngressMediaPosixUdpWriteStatus
meiso_display_ingress_media_posix_udp_on_writable(
    MeisoDisplayIngressMediaPosixUdp *adapter
);

/*
 * Terminates the exact bound media object, clears the packet borrow and RX
 * buffer, but never closes the caller-owned descriptor.
 */
MeisoDisplayIngressMediaPosixUdpTerminateStatus
meiso_display_ingress_media_posix_udp_terminate(
    MeisoDisplayIngressMediaPosixUdp *adapter,
    MeisoDisplayIngressMediaTerminalReason reason,
    int error_number
);

/*
 * error_number is an errno-style nonnegative diagnostic. last_error succeeds
 * only after terminal. Pure-query failure leaves disjoint output byte-exact
 * unchanged.
 */
bool meiso_display_ingress_media_posix_udp_last_error(
    const MeisoDisplayIngressMediaPosixUdp *adapter,
    int *out_error_number
);
bool meiso_display_ingress_media_posix_udp_snapshot(
    const MeisoDisplayIngressMediaPosixUdp *adapter,
    MeisoDisplayIngressMediaPosixUdpSnapshot *out_snapshot
);

#ifdef __cplusplus
}
#endif

#endif
