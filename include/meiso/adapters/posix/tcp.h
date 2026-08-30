#ifndef MEISO_ADAPTERS_POSIX_TCP_H
#define MEISO_ADAPTERS_POSIX_TCP_H

#include <meiso/core/session.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MeisoPosixTcp MeisoPosixTcp;

typedef struct {
    int connected_fd;
    MeisoSession *session;
    uint32_t max_frame_bytes;
    size_t max_io_bytes_per_step;
} MeisoPosixTcpConfig;

typedef uint8_t MeisoPosixTcpInitStatus;
enum {
    MEISO_POSIX_TCP_INIT_OK = 0,
    MEISO_POSIX_TCP_INIT_INVALID_ARGUMENT = 1,
    MEISO_POSIX_TCP_INIT_INVALID_CONFIG = 2,
    MEISO_POSIX_TCP_INIT_FD_ERROR = 3,
    MEISO_POSIX_TCP_INIT_NONBLOCKING_REQUIRED = 4,
    MEISO_POSIX_TCP_INIT_MISALIGNED_ARENA = 5,
    MEISO_POSIX_TCP_INIT_ARENA_TOO_SMALL = 6
};

typedef uint8_t MeisoPosixTcpReadStatus;
enum {
    MEISO_POSIX_TCP_READ_PROGRESS = 0,
    MEISO_POSIX_TCP_READ_WOULD_BLOCK = 1,
    MEISO_POSIX_TCP_READ_INTERRUPTED = 2,
    MEISO_POSIX_TCP_READ_SESSION_PROGRESS = 3,
    MEISO_POSIX_TCP_READ_RUNTIME = 4,
    MEISO_POSIX_TCP_READ_PEER_CLOSED = 5,
    MEISO_POSIX_TCP_READ_SESSION_FAILED = 6,
    MEISO_POSIX_TCP_READ_TRANSPORT_LOST = 7,
    MEISO_POSIX_TCP_READ_INVALID_ARGUMENT = 8
};

typedef uint8_t MeisoPosixTcpWriteStatus;
enum {
    MEISO_POSIX_TCP_WRITE_PROGRESS = 0,
    MEISO_POSIX_TCP_WRITE_WOULD_BLOCK = 1,
    MEISO_POSIX_TCP_WRITE_INTERRUPTED = 2,
    MEISO_POSIX_TCP_WRITE_FRAME_SENT = 3,
    MEISO_POSIX_TCP_WRITE_IDLE = 4,
    MEISO_POSIX_TCP_WRITE_TRANSPORT_LOST = 5,
    MEISO_POSIX_TCP_WRITE_SESSION_INVALID = 6,
    MEISO_POSIX_TCP_WRITE_INVALID_ARGUMENT = 7
};

size_t meiso_posix_tcp_alignment_required(void);
size_t meiso_posix_tcp_memory_required(const MeisoPosixTcpConfig *config);

/*
 * The descriptor is borrowed: the owner retains the fd and Session lifetime,
 * and both must outlive the adapter.  The adapter arena/storage must not
 * overlap the Session arena/storage.  init rejects the directly detectable
 * case where the Session handle itself lies in the adapter arena; other
 * overlapping subranges, including output storage placed elsewhere inside
 * Session storage, remain a caller precondition because Session is opaque.
 * The same owner serializes all calls involving this adapter and Session.
 * While bound, receive, mark_outbound_sent, and transport_lost are driven by
 * the adapter; the owner must not call those Session operations independently.
 *
 * connected_fd must already be a connected, nonblocking SOCK_STREAM socket.
 * init fails closed on a platform that provides neither SO_NOSIGPIPE nor
 * MSG_NOSIGNAL; no unprotected send is attempted.
 * max_frame_bytes must be at least the Session's configured local frame limit.
 * The current Session snapshot does not expose that local limit, so this is a
 * caller precondition rather than an init-time verified property.
 * A violated precondition still fails boundedly if a frame exceeds capacity.
 */
MeisoPosixTcpInitStatus meiso_posix_tcp_init(
    void *arena,
    size_t arena_size,
    const MeisoPosixTcpConfig *config,
    MeisoPosixTcp **out_adapter
);

/* These are level-triggered interests for an owner-provided event loop. */
bool meiso_posix_tcp_wants_read(const MeisoPosixTcp *adapter);
bool meiso_posix_tcp_wants_write(const MeisoPosixTcp *adapter);

/*
 * Each step performs at most one recv/send syscall and moves no more than the
 * lesser of max_io_bytes_per_step, SSIZE_MAX, and the current frame remainder.
 * A RUNTIME payload borrow ends before the next non-const call on the same
 * Session/adapter.
 */
MeisoPosixTcpReadStatus meiso_posix_tcp_on_readable(
    MeisoPosixTcp *adapter,
    MeisoSessionRuntimeMessage *out_runtime
);

MeisoPosixTcpWriteStatus meiso_posix_tcp_on_writable(
    MeisoPosixTcp *adapter
);

/* Clears staged bytes and records transport loss without closing the fd. */
void meiso_posix_tcp_abort(MeisoPosixTcp *adapter, int error_number);

/* Zero means EOF or an explicit abort without an errno value. */
bool meiso_posix_tcp_last_error(
    const MeisoPosixTcp *adapter,
    int *out_error_number
);

#ifdef __cplusplus
}
#endif

#endif
