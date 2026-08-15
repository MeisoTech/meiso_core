#define _POSIX_C_SOURCE 200809L

#include "meiso_posix_tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    LOOPBACK_MAX_FRAME_BYTES = 256,
    LOOPBACK_SESSION_ARENA_BYTES = 2048,
    LOOPBACK_ADAPTER_ARENA_BYTES = 4096,
    LOOPBACK_IO_CHUNK_BYTES = 7,
    LOOPBACK_POLL_TIMEOUT_MS = 20,
    LOOPBACK_PHASE_TIMEOUT_MS = 5000
};

typedef union {
    max_align_t alignment;
    uint8_t bytes[LOOPBACK_SESSION_ARENA_BYTES];
} SessionArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[LOOPBACK_ADAPTER_ARENA_BYTES];
} AdapterArena;

typedef uint8_t LoopbackGoal;
enum {
    LOOPBACK_GOAL_READY = 1,
    LOOPBACK_GOAL_RUNTIME = 2,
    LOOPBACK_GOAL_CLOSED = 3
};

static const uint8_t HOST_PAYLOAD[] = {
    0x68U, 0x6fU, 0x73U, 0x74U
};
static const uint8_t EDGE_PAYLOAD[] = {
    0x65U, 0x64U, 0x67U, 0x65U
};

typedef struct {
    int host_fd;
    int edge_fd;
    SessionArena host_session_arena;
    SessionArena edge_session_arena;
    AdapterArena host_adapter_arena;
    AdapterArena edge_adapter_arena;
    MeisoSessionConfig host_config;
    MeisoSessionConfig edge_config;
    MeisoSession *host_session;
    MeisoSession *edge_session;
    MeisoPosixTcp *host_adapter;
    MeisoPosixTcp *edge_adapter;
    bool host_received_runtime;
    bool edge_received_runtime;
} Loopback;

static MeisoSessionConfig host_config(void)
{
    MeisoSessionConfig config;
    memset(&config, 0, sizeof(config));
    config.local_role = MEISO_SESSION_ROLE_HOST;
    config.local_peer_id = (MeisoPeerId){0x1111U, 0x1112U};
    config.local_incarnation = 0x1113U;
    config.local_nonce = (MeisoSessionNonce){0x1114U, 0x1115U};
    config.expected_peer_id = (MeisoPeerId){0x2221U, 0x2222U};
    config.protocol = (MeisoProtocolVersion){1U, 0U};
    config.profile.id = (MeisoProfileId){0x3331U, 0x3332U};
    config.profile.version = 1U;
    config.limits = (MeisoSessionLimits){
        LOOPBACK_MAX_FRAME_BYTES, 64U, 8U
    };
    return config;
}

static MeisoSessionConfig edge_config(void)
{
    MeisoSessionConfig config = host_config();
    config.local_role = MEISO_SESSION_ROLE_EDGE;
    config.local_peer_id = (MeisoPeerId){0x2221U, 0x2222U};
    config.local_incarnation = 0x2223U;
    config.local_nonce = (MeisoSessionNonce){0x2224U, 0x2225U};
    config.expected_peer_id = (MeisoPeerId){0x1111U, 0x1112U};
    return config;
}

static bool set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static bool make_tcp_pair(int *out_client, int *out_server)
{
    struct sockaddr_in address;
    socklen_t address_size = (socklen_t)sizeof(address);
    int listener = -1;
    int client = -1;
    int server = -1;
    int enabled = 1;

    *out_client = -1;
    *out_server = -1;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0 || setsockopt(
            listener, SOL_SOCKET, SO_REUSEADDR,
            &enabled, (socklen_t)sizeof(enabled)
        ) != 0) {
        goto fail;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(UINT32_C(0x7f000001));
    address.sin_port = htons(0U);
    if (bind(
            listener, (const struct sockaddr *)(const void *)&address,
            (socklen_t)sizeof(address)
        ) != 0 || listen(listener, 1) != 0 || getsockname(
            listener, (struct sockaddr *)(void *)&address, &address_size
        ) != 0) {
        goto fail;
    }
    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0 || connect(
            client, (const struct sockaddr *)(const void *)&address,
            address_size
        ) != 0) {
        goto fail;
    }
    server = accept(listener, NULL, NULL);
    if (server < 0 || !set_nonblocking(client) || !set_nonblocking(server)) {
        goto fail;
    }
    (void)close(listener);
    *out_client = client;
    *out_server = server;
    return true;

fail:
    close_fd(&server);
    close_fd(&client);
    close_fd(&listener);
    return false;
}

static bool initialize_session(
    SessionArena *arena,
    const MeisoSessionConfig *config,
    MeisoSession **out_session
)
{
    const size_t required = meiso_session_memory_required(config);
    return required != 0U && required <= sizeof(arena->bytes) &&
        meiso_session_init(
            arena->bytes, sizeof(arena->bytes), config, out_session
        ) == MEISO_SESSION_INIT_OK;
}

static bool initialize_adapter(
    AdapterArena *arena,
    int fd,
    MeisoSession *session,
    MeisoPosixTcp **out_adapter
)
{
    const MeisoPosixTcpConfig config = {
        fd,
        session,
        LOOPBACK_MAX_FRAME_BYTES,
        LOOPBACK_IO_CHUNK_BYTES
    };
    const size_t required = meiso_posix_tcp_memory_required(&config);
    return required != 0U && required <= sizeof(arena->bytes) &&
        meiso_posix_tcp_init(
            arena->bytes, sizeof(arena->bytes), &config, out_adapter
        ) == MEISO_POSIX_TCP_INIT_OK;
}

static bool initialize_loopback(Loopback *loopback)
{
    memset(loopback, 0, sizeof(*loopback));
    loopback->host_fd = -1;
    loopback->edge_fd = -1;
    loopback->host_config = host_config();
    loopback->edge_config = edge_config();
    return make_tcp_pair(&loopback->host_fd, &loopback->edge_fd) &&
        initialize_session(
            &loopback->host_session_arena,
            &loopback->host_config,
            &loopback->host_session
        ) && initialize_session(
            &loopback->edge_session_arena,
            &loopback->edge_config,
            &loopback->edge_session
        ) && initialize_adapter(
            &loopback->host_adapter_arena,
            loopback->host_fd,
            loopback->host_session,
            &loopback->host_adapter
        ) && initialize_adapter(
            &loopback->edge_adapter_arena,
            loopback->edge_fd,
            loopback->edge_session,
            &loopback->edge_adapter
        );
}

static void destroy_loopback(Loopback *loopback)
{
    close_fd(&loopback->host_fd);
    close_fd(&loopback->edge_fd);
}

static bool make_deadline(struct timespec *out_deadline)
{
    if (clock_gettime(CLOCK_MONOTONIC, out_deadline) != 0) {
        return false;
    }
    out_deadline->tv_sec += (time_t)(LOOPBACK_PHASE_TIMEOUT_MS / 1000);
    out_deadline->tv_nsec += (long)(
        (LOOPBACK_PHASE_TIMEOUT_MS % 1000) * 1000000
    );
    if (out_deadline->tv_nsec >= 1000000000L) {
        out_deadline->tv_nsec -= 1000000000L;
        out_deadline->tv_sec += (time_t)1;
    }
    return true;
}

static bool deadline_expired(const struct timespec *deadline)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return true;
    }
    return now.tv_sec > deadline->tv_sec ||
        (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec);
}

static bool session_state(
    MeisoSession *session,
    MeisoSessionState state,
    MeisoSessionFailure failure
)
{
    MeisoSessionSnapshot snapshot;
    return meiso_session_snapshot(session, &snapshot) &&
        snapshot.state == state && snapshot.failure == failure;
}

static bool goal_reached(const Loopback *loopback, LoopbackGoal goal)
{
    if (goal == LOOPBACK_GOAL_READY) {
        return session_state(
                loopback->host_session,
                MEISO_SESSION_STATE_READY,
                MEISO_SESSION_FAILURE_NONE
            ) && session_state(
                loopback->edge_session,
                MEISO_SESSION_STATE_READY,
                MEISO_SESSION_FAILURE_NONE
            );
    }
    if (goal == LOOPBACK_GOAL_RUNTIME) {
        return loopback->host_received_runtime &&
            loopback->edge_received_runtime;
    }
    return session_state(
            loopback->host_session,
            MEISO_SESSION_STATE_CLOSED,
            MEISO_SESSION_FAILURE_LOCAL_CLOSED
        ) && session_state(
            loopback->edge_session,
            MEISO_SESSION_STATE_CLOSED,
            MEISO_SESSION_FAILURE_PEER_CLOSED
        );
}

static bool write_status_valid(MeisoPosixTcpWriteStatus status)
{
    return status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
        status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK ||
        status == MEISO_POSIX_TCP_WRITE_INTERRUPTED ||
        status == MEISO_POSIX_TCP_WRITE_FRAME_SENT ||
        status == MEISO_POSIX_TCP_WRITE_IDLE;
}

static bool consume_runtime(
    Loopback *loopback,
    size_t endpoint,
    const MeisoSessionRuntimeMessage *runtime
)
{
    const uint8_t *expected = endpoint == 0U ? EDGE_PAYLOAD : HOST_PAYLOAD;
    const size_t expected_size = endpoint == 0U ?
        sizeof(EDGE_PAYLOAD) : sizeof(HOST_PAYLOAD);

    if (runtime->payload.size != expected_size || memcmp(
            runtime->payload.data, expected, expected_size
        ) != 0) {
        return false;
    }
    if (endpoint == 0U) {
        loopback->host_received_runtime = true;
    } else {
        loopback->edge_received_runtime = true;
    }
    return true;
}

static bool read_one(Loopback *loopback, size_t endpoint)
{
    MeisoPosixTcp *adapter = endpoint == 0U ?
        loopback->host_adapter : loopback->edge_adapter;
    MeisoSessionRuntimeMessage runtime;
    const MeisoPosixTcpReadStatus status = meiso_posix_tcp_on_readable(
        adapter, &runtime
    );

    if (status == MEISO_POSIX_TCP_READ_RUNTIME) {
        return consume_runtime(loopback, endpoint, &runtime);
    }
    return status == MEISO_POSIX_TCP_READ_PROGRESS ||
        status == MEISO_POSIX_TCP_READ_WOULD_BLOCK ||
        status == MEISO_POSIX_TCP_READ_INTERRUPTED ||
        status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS ||
        status == MEISO_POSIX_TCP_READ_PEER_CLOSED;
}

static bool drive_until(Loopback *loopback, LoopbackGoal goal)
{
    struct timespec deadline;
    struct pollfd poll_fds[2];
    MeisoPosixTcp *adapters[2] = {
        loopback->host_adapter, loopback->edge_adapter
    };
    size_t endpoint;

    if (!make_deadline(&deadline)) {
        return false;
    }
    while (!goal_reached(loopback, goal)) {
        int poll_result;
        if (deadline_expired(&deadline)) {
            return false;
        }
        for (endpoint = 0U; endpoint < 2U; ++endpoint) {
            short events = 0;
            if (meiso_posix_tcp_wants_read(adapters[endpoint])) {
                events = (short)(events | POLLIN);
            }
            if (meiso_posix_tcp_wants_write(adapters[endpoint])) {
                events = (short)(events | POLLOUT);
            }
            poll_fds[endpoint].fd = endpoint == 0U ?
                loopback->host_fd : loopback->edge_fd;
            poll_fds[endpoint].events = events;
            poll_fds[endpoint].revents = 0;
        }
        poll_result = poll(
            poll_fds, (nfds_t)2U, LOOPBACK_POLL_TIMEOUT_MS
        );
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        for (endpoint = 0U; endpoint < 2U; ++endpoint) {
            const short revents = poll_fds[endpoint].revents;
            if ((revents & POLLOUT) != 0 && !write_status_valid(
                    meiso_posix_tcp_on_writable(adapters[endpoint])
                )) {
                return false;
            }
            if ((revents & POLLIN) != 0 &&
                !read_one(loopback, endpoint)) {
                return false;
            }
            if ((revents & POLLHUP) != 0 && (revents & POLLIN) == 0 &&
                meiso_posix_tcp_wants_read(adapters[endpoint]) &&
                !read_one(loopback, endpoint)) {
                return false;
            }
            if ((revents & (POLLERR | POLLNVAL)) != 0) {
                meiso_posix_tcp_abort(adapters[endpoint], EIO);
                return false;
            }
        }
    }
    return true;
}

int main(void)
{
    Loopback loopback;
    bool passed = false;

    if (!initialize_loopback(&loopback)) {
        fputs("loopback bootstrap failed\n", stderr);
        destroy_loopback(&loopback);
        return 1;
    }
    if (meiso_session_start(loopback.edge_session) !=
            MEISO_SESSION_START_STARTED ||
        meiso_session_start(loopback.host_session) !=
            MEISO_SESSION_START_STARTED ||
        !drive_until(&loopback, LOOPBACK_GOAL_READY)) {
        fputs("loopback handshake failed\n", stderr);
        goto done;
    }
    if (meiso_session_submit_runtime(
            loopback.host_session,
            (MeisoByteView){HOST_PAYLOAD, sizeof(HOST_PAYLOAD)}
        ) != MEISO_SESSION_SUBMIT_QUEUED ||
        meiso_session_submit_runtime(
            loopback.edge_session,
            (MeisoByteView){EDGE_PAYLOAD, sizeof(EDGE_PAYLOAD)}
        ) != MEISO_SESSION_SUBMIT_QUEUED ||
        !drive_until(&loopback, LOOPBACK_GOAL_RUNTIME)) {
        fputs("loopback runtime payload failed\n", stderr);
        goto done;
    }
    if (meiso_session_close(
            loopback.host_session, MEISO_SESSION_CLOSE_NORMAL
        ) != MEISO_SESSION_CLOSE_QUEUED ||
        !drive_until(&loopback, LOOPBACK_GOAL_CLOSED)) {
        fputs("loopback close failed\n", stderr);
        goto done;
    }
    passed = true;

done:
    destroy_loopback(&loopback);
    if (!passed) {
        return 1;
    }
    puts("macOS POSIX TCP Session loopback passed");
    return 0;
}
