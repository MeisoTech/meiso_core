#ifndef MEISO_ADAPTERS_POSIX_COMPOSITION_MEDIA_TCP_H
#define MEISO_ADAPTERS_POSIX_COMPOSITION_MEDIA_TCP_H

#include <meiso/profiles/composition/media_transport.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MeisoCompositionMediaPosixTcp
    MeisoCompositionMediaPosixTcp;

typedef struct {
    int connected_fd;
    MeisoCompositionMediaTransport *transport;
    MeisoCompositionMediaOpen scope;
    size_t max_io_bytes_per_step;
} MeisoCompositionMediaPosixTcpConfig;

typedef uint8_t MeisoCompositionMediaPosixTcpInitStatus;
enum {
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_OK = 0,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_INVALID_CONFIG = 2,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_FD_ERROR = 3,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_NONBLOCKING_REQUIRED = 4,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_MISALIGNED_ARENA = 5,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_ARENA_TOO_SMALL = 6,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_SCOPE_MISMATCH = 7
};

typedef uint8_t MeisoCompositionMediaPosixTcpPrepareStatus;
enum {
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_READY = 0,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_IDLE = 1,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_TERMINAL = 2,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_TRANSPORT_INVALID = 3,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_INVALID_ARGUMENT = 4
};

typedef uint8_t MeisoCompositionMediaPosixTcpReadStatus;
enum {
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PROGRESS = 0,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_WOULD_BLOCK = 1,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_INTERRUPTED = 2,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PACKET_ACCEPTED = 3,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_FRAGMENT_ACCEPTED = 4,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_FRAME_PUBLISHED = 5,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_REASSEMBLY_REPLACED = 6,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PACKET_REJECTED = 7,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST = 8,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_INVALID_ARGUMENT = 9
};

typedef uint8_t MeisoCompositionMediaPosixTcpWriteStatus;
enum {
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PROGRESS = 0,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_WOULD_BLOCK = 1,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_INTERRUPTED = 2,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PACKET_SENT = 3,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_IDLE = 4,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_LOST = 5,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_INVALID = 6,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_INVALID_ARGUMENT = 7
};

typedef uint8_t MeisoCompositionMediaPosixTcpTerminateStatus;
enum {
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_TERMINATE_OK = 0,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_TERMINATE_ALREADY = 1,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_TERMINATE_SCOPE_MISMATCH = 2,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_TERMINATE_TRANSPORT_INVALID = 3,
    MEISO_COMPOSITION_MEDIA_POSIX_TCP_TERMINATE_INVALID_ARGUMENT = 4
};

typedef struct {
    MeisoSessionRole local_role;
    MeisoCompositionMediaOpen scope;
    bool terminal;
    MeisoCompositionMediaTerminalReason terminal_reason;
    int last_error;
    size_t rx_bytes;
    size_t rx_expected_bytes;
    bool tx_packet_borrowed;
    MeisoCompositionMediaPacketRef tx_packet;
    size_t tx_bytes_sent;
    size_t tx_packet_bytes;
    bool receive_status_present;
    MeisoCompositionMediaTransportStatus last_receive_status;
    size_t arena_bytes;
} MeisoCompositionMediaPosixTcpSnapshot;

size_t meiso_composition_media_posix_tcp_alignment_required(void);
size_t meiso_composition_media_posix_tcp_memory_required(
    const MeisoCompositionMediaPosixTcpConfig *config
);

/*
 * Profile-local adapter for one caller-created, already-connected,
 * nonblocking AF_INET/AF_INET6 SOCK_STREAM fd. The caller resolves the numeric
 * direct-IP locator and owns connect/listen/accept/close; the adapter performs
 * none of them and never owns DNS, authentication or a second Core Session.
 * It adds no wire: packet boundaries are derived from exact MOPN/MBND/MFRM
 * codecs (64, 128, or 240+fragment bytes).
 *
 * fd and transport are borrowed and must outlive the adapter. scope must equal
 * transport.snapshot.scope. The adapter arena and transport arena/storage are
 * disjoint; init uses transport.snapshot.arena_bytes to reject any overlap of
 * the full transport arena with config, adapter arena or output storage.
 * One owner serializes all adapter and transport calls. While bound, only the
 * adapter may call transport next_outbound, complete_outbound, receive_packet
 * or terminate. The owner may still serialize submit/latest/release/snapshot.
 * init accepts only a fresh transport: Host OPEN_PENDING or Edge WAIT_OPEN,
 * with zero pending packet/bind mask, slots, sequence/drop/gap counters and
 * packet serial. A new fd cannot resume an ACTIVE or partial old transport;
 * reconnect uses a fresh transport arena and repeats OPEN/BIND.
 *
 * The adapter arena contains metadata plus one 4336-byte RX packet buffer. TX
 * holds one transport packet borrow and allocates/copies no payload. init has
 * no heap allocation. Any failure leaves disjoint arena/out_adapter storage
 * byte-exact unchanged. Send is protected by MSG_NOSIGNAL or SO_NOSIGPIPE;
 * init fails where neither exists. No API closes the fd or changes fd flags.
 */
MeisoCompositionMediaPosixTcpInitStatus
meiso_composition_media_posix_tcp_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionMediaPosixTcpConfig *config,
    MeisoCompositionMediaPosixTcp **out_adapter
);

/* Level-triggered interests for a caller-owned event loop. */
bool meiso_composition_media_posix_tcp_wants_read(
    const MeisoCompositionMediaPosixTcp *adapter
);
bool meiso_composition_media_posix_tcp_wants_write(
    const MeisoCompositionMediaPosixTcp *adapter
);

/*
 * Explicitly acquires at most one idempotent transport packet borrow before
 * POLLOUT is armed. READY may change transport pending-packet metadata but
 * sends/completes nothing. While borrowed, repeated prepare is idempotent.
 * IDLE and INVALID_ARGUMENT make no adapter/transport change.
 * TRANSPORT_INVALID enters sticky terminal; TERMINAL synchronizes the adapter
 * to an already-terminal transport.
 */
MeisoCompositionMediaPosixTcpPrepareStatus
meiso_composition_media_posix_tcp_prepare_outbound(
    MeisoCompositionMediaPosixTcp *adapter
);

/*
 * Each readable/writable step performs at most one recv/send syscall and
 * moves at most min(max_io_bytes_per_step, SSIZE_MAX, packet remainder).
 * on_readable never calls transport.receive_packet until one exact complete
 * codec packet is buffered, so a partial packet is never published.
 *
 * on_writable requires a prior READY prepare. Only a complete kernel send of
 * the packet calls transport.complete_outbound. WOULD_BLOCK and INTERRUPTED
 * leave adapter arena, packet borrow, transport state/cursor/offset/sequence,
 * output truth and counters byte-exact unchanged. Partial positive I/O is
 * observable PROGRESS, not WOULD_BLOCK. If exact completion fails after the
 * final positive send, those wire bytes cannot be rolled back: adapter and
 * transport enter sticky terminal and WRITE_TRANSPORT_INVALID is returned.
 *
 * Only an MFRM DISCONTINUITY, SEQUENCE_REPLAY or immutable-frame CONFLICT is
 * nonterminal PACKET_REJECTED after the transport applies its documented
 * whole-frame semantics. A BIND/OPEN CONFLICT and all malformed/framing/
 * identity/epoch/nonce/state/counter failures terminate the entire media
 * connection and clear all media slots.
 */
MeisoCompositionMediaPosixTcpReadStatus
meiso_composition_media_posix_tcp_on_readable(
    MeisoCompositionMediaPosixTcp *adapter
);
MeisoCompositionMediaPosixTcpWriteStatus
meiso_composition_media_posix_tcp_on_writable(
    MeisoCompositionMediaPosixTcp *adapter
);

/*
 * Explicit control/media boundary. First success calls transport.terminate
 * with the exact initialized scope, zeroes staged RX metadata/buffer and drops
 * the pending TX borrow. If a packet prefix was written, the caller must close
 * the fd; the adapter never attempts to complete or reuse that byte stream.
 * error_number is diagnostic only (zero is permitted).
 */
MeisoCompositionMediaPosixTcpTerminateStatus
meiso_composition_media_posix_tcp_terminate(
    MeisoCompositionMediaPosixTcp *adapter,
    MeisoCompositionMediaTerminalReason reason,
    int error_number
);

/*
 * Pure queries; failure leaves output byte-exact unchanged. Query output may
 * overlap neither the adapter arena nor the full bound transport arena.
 */
bool meiso_composition_media_posix_tcp_last_error(
    const MeisoCompositionMediaPosixTcp *adapter,
    int *out_error_number
);
bool meiso_composition_media_posix_tcp_snapshot(
    const MeisoCompositionMediaPosixTcp *adapter,
    MeisoCompositionMediaPosixTcpSnapshot *out_snapshot
);

#ifdef __cplusplus
}
#endif

#endif
