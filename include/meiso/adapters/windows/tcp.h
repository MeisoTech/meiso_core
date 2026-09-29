#ifndef MEISO_ADAPTERS_WINDOWS_TCP_H
#define MEISO_ADAPTERS_WINDOWS_TCP_H

#include <meiso/core/session.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MeisoWindowsTcp MeisoWindowsTcp;

typedef struct {
    uintptr_t connected_socket; /* Winsock SOCKET, never a CRT fd. */
    MeisoSession *session;
    uint32_t max_frame_bytes;
    size_t max_io_bytes_per_step;
} MeisoWindowsTcpConfig;

typedef uint8_t MeisoWindowsTcpInitStatus;
enum {
    MEISO_WINDOWS_TCP_INIT_OK = 0,
    MEISO_WINDOWS_TCP_INIT_INVALID_ARGUMENT = 1,
    MEISO_WINDOWS_TCP_INIT_INVALID_CONFIG = 2,
    MEISO_WINDOWS_TCP_INIT_SOCKET_ERROR = 3,
    MEISO_WINDOWS_TCP_INIT_MISALIGNED_ARENA = 5,
    MEISO_WINDOWS_TCP_INIT_ARENA_TOO_SMALL = 6
};

typedef uint8_t MeisoWindowsTcpReadStatus;
enum {
    MEISO_WINDOWS_TCP_READ_PROGRESS = 0,
    MEISO_WINDOWS_TCP_READ_WOULD_BLOCK = 1,
    MEISO_WINDOWS_TCP_READ_INTERRUPTED = 2,
    MEISO_WINDOWS_TCP_READ_SESSION_PROGRESS = 3,
    MEISO_WINDOWS_TCP_READ_RUNTIME = 4,
    MEISO_WINDOWS_TCP_READ_PEER_CLOSED = 5,
    MEISO_WINDOWS_TCP_READ_SESSION_FAILED = 6,
    MEISO_WINDOWS_TCP_READ_TRANSPORT_LOST = 7,
    MEISO_WINDOWS_TCP_READ_INVALID_ARGUMENT = 8
};

typedef uint8_t MeisoWindowsTcpWriteStatus;
enum {
    MEISO_WINDOWS_TCP_WRITE_PROGRESS = 0,
    MEISO_WINDOWS_TCP_WRITE_WOULD_BLOCK = 1,
    MEISO_WINDOWS_TCP_WRITE_INTERRUPTED = 2,
    MEISO_WINDOWS_TCP_WRITE_FRAME_SENT = 3,
    MEISO_WINDOWS_TCP_WRITE_IDLE = 4,
    MEISO_WINDOWS_TCP_WRITE_TRANSPORT_LOST = 5,
    MEISO_WINDOWS_TCP_WRITE_SESSION_INVALID = 6,
    MEISO_WINDOWS_TCP_WRITE_INVALID_ARGUMENT = 7
};

size_t meiso_windows_tcp_alignment_required(void);
size_t meiso_windows_tcp_memory_required(const MeisoWindowsTcpConfig *config);

/*
 * The socket is borrowed: the owner retains the SOCKET and Session lifetime,
 * and both must outlive the adapter.  The adapter arena/storage must not
 * overlap the Session arena/storage.  init rejects the directly detectable
 * case where the Session handle itself lies in the adapter arena; other
 * overlapping subranges, including output storage placed elsewhere inside
 * Session storage, remain a caller precondition because Session is opaque.
 * The same owner serializes all calls involving this adapter and Session.
 * While bound, receive, mark_outbound_sent, and transport_lost are driven by
 * the adapter; the owner must not call those Session operations independently.
 *
 * connected_socket must be a connected AF_INET/AF_INET6 SOCK_STREAM socket.
 * The caller owns WSAStartup/WSACleanup. init sets FIONBIO=1 only after all
 * other validation succeeds; the caller must not restore blocking mode.
 * No operation shuts down or closes the borrowed socket.
 * max_frame_bytes must be at least the Session's configured local frame limit.
 * The current Session snapshot does not expose that local limit, so this is a
 * caller precondition rather than an init-time verified property.
 * A violated precondition still fails boundedly if a frame exceeds capacity.
 */
MeisoWindowsTcpInitStatus meiso_windows_tcp_init(
    void *arena,
    size_t arena_size,
    const MeisoWindowsTcpConfig *config,
    MeisoWindowsTcp **out_adapter
);

/* These are level-triggered interests for an owner-provided event loop. */
bool meiso_windows_tcp_wants_read(const MeisoWindowsTcp *adapter);
bool meiso_windows_tcp_wants_write(const MeisoWindowsTcp *adapter);

/*
 * Each step performs at most one recv/send syscall and moves no more than the
 * lesser of max_io_bytes_per_step, INT_MAX, and the current frame remainder.
 * A RUNTIME payload borrow ends before the next non-const call on the same
 * Session/adapter.
 */
MeisoWindowsTcpReadStatus meiso_windows_tcp_on_readable(
    MeisoWindowsTcp *adapter,
    MeisoSessionRuntimeMessage *out_runtime
);

MeisoWindowsTcpWriteStatus meiso_windows_tcp_on_writable(
    MeisoWindowsTcp *adapter
);

/* Clears staged bytes and records transport loss without closing the socket. */
void meiso_windows_tcp_abort(MeisoWindowsTcp *adapter, int error_number);

/* Zero means EOF or an explicit abort without an Winsock error. */
bool meiso_windows_tcp_last_error(
    const MeisoWindowsTcp *adapter,
    int *out_error_number
);

#ifdef __cplusplus
}
#endif

#endif
