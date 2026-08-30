#define _POSIX_C_SOURCE 200809L

#include <meiso/adapters/posix/tcp.h>

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

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

enum {
    TEST_MAX_FRAME_BYTES = 256,
    TEST_SESSION_ARENA_BYTES = 2048,
    TEST_ADAPTER_ARENA_BYTES = 4096,
    TEST_POLL_TIMEOUT_MS = 20,
    TEST_DEADLINE_SECONDS = 5
};

typedef union {
    max_align_t alignment;
    uint8_t bytes[TEST_SESSION_ARENA_BYTES];
} SessionArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[TEST_ADAPTER_ARENA_BYTES];
} AdapterArena;

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
} TcpFixture;

static MeisoSessionConfig host_config(void)
{
    MeisoSessionConfig config;
    memset(&config, 0, sizeof(config));
    config.local_role = MEISO_SESSION_ROLE_HOST;
    config.local_peer_id = (MeisoPeerId){0x1001U, 0x1002U};
    config.local_incarnation = 0x1003U;
    config.local_nonce = (MeisoSessionNonce){0x1004U, 0x1005U};
    config.expected_peer_id = (MeisoPeerId){0x2001U, 0x2002U};
    config.protocol = (MeisoProtocolVersion){1U, 0U};
    config.profile.id = (MeisoProfileId){0x3001U, 0x3002U};
    config.profile.version = 1U;
    config.limits = (MeisoSessionLimits){
        TEST_MAX_FRAME_BYTES, 64U, 8U
    };
    return config;
}

static MeisoSessionConfig edge_config(void)
{
    MeisoSessionConfig config = host_config();
    config.local_role = MEISO_SESSION_ROLE_EDGE;
    config.local_peer_id = (MeisoPeerId){0x2001U, 0x2002U};
    config.local_incarnation = 0x2003U;
    config.local_nonce = (MeisoSessionNonce){0x2004U, 0x2005U};
    config.expected_peer_id = (MeisoPeerId){0x1001U, 0x1002U};
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

static bool make_tcp_pair_configured(
    int *out_client,
    int *out_server,
    int client_send_buffer,
    int server_receive_buffer
)
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
        ) != 0 || (server_receive_buffer > 0 && setsockopt(
            listener, SOL_SOCKET, SO_RCVBUF,
            &server_receive_buffer,
            (socklen_t)sizeof(server_receive_buffer)
        ) != 0)) {
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
    if (client < 0 || (client_send_buffer > 0 && setsockopt(
            client, SOL_SOCKET, SO_SNDBUF,
            &client_send_buffer, (socklen_t)sizeof(client_send_buffer)
        ) != 0) || connect(
            client, (const struct sockaddr *)(const void *)&address,
            address_size
        ) != 0) {
        goto fail;
    }
    server = accept(listener, NULL, NULL);
    if (server < 0 || (server_receive_buffer > 0 && setsockopt(
            server, SOL_SOCKET, SO_RCVBUF,
            &server_receive_buffer,
            (socklen_t)sizeof(server_receive_buffer)
        ) != 0) || !set_nonblocking(client) || !set_nonblocking(server)) {
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

static bool make_tcp_pair(int *out_client, int *out_server)
{
    return make_tcp_pair_configured(out_client, out_server, 0, 0);
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
    size_t io_chunk,
    MeisoPosixTcp **out_adapter
)
{
    const MeisoPosixTcpConfig config = {
        fd, session, TEST_MAX_FRAME_BYTES, io_chunk
    };
    const size_t required = meiso_posix_tcp_memory_required(&config);
    return required != 0U && required <= sizeof(arena->bytes) &&
        meiso_posix_tcp_init(
            arena->bytes, sizeof(arena->bytes), &config, out_adapter
        ) == MEISO_POSIX_TCP_INIT_OK;
}

static bool initialize_adapter_with_max(
    AdapterArena *arena,
    int fd,
    MeisoSession *session,
    uint32_t max_frame_bytes,
    size_t io_chunk,
    MeisoPosixTcp **out_adapter
)
{
    const MeisoPosixTcpConfig config = {
        fd, session, max_frame_bytes, io_chunk
    };
    const size_t required = meiso_posix_tcp_memory_required(&config);
    return required != 0U && required <= sizeof(arena->bytes) &&
        meiso_posix_tcp_init(
            arena->bytes, sizeof(arena->bytes), &config, out_adapter
        ) == MEISO_POSIX_TCP_INIT_OK;
}

static bool initialize_fixture_with_host_max(
    TcpFixture *fixture,
    size_t io_chunk,
    uint32_t host_max_frame_bytes
)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->host_fd = -1;
    fixture->edge_fd = -1;
    fixture->host_config = host_config();
    fixture->edge_config = edge_config();
    if (!make_tcp_pair(&fixture->host_fd, &fixture->edge_fd) ||
        !initialize_session(
            &fixture->host_session_arena,
            &fixture->host_config,
            &fixture->host_session
        ) || !initialize_session(
            &fixture->edge_session_arena,
            &fixture->edge_config,
            &fixture->edge_session
        ) || !initialize_adapter_with_max(
            &fixture->host_adapter_arena,
            fixture->host_fd,
            fixture->host_session,
            host_max_frame_bytes,
            io_chunk,
            &fixture->host_adapter
        ) || !initialize_adapter(
            &fixture->edge_adapter_arena,
            fixture->edge_fd,
            fixture->edge_session,
            io_chunk,
            &fixture->edge_adapter
        )) {
        close_fd(&fixture->host_fd);
        close_fd(&fixture->edge_fd);
        return false;
    }
    return true;
}

static bool initialize_fixture(TcpFixture *fixture, size_t io_chunk)
{
    return initialize_fixture_with_host_max(
        fixture, io_chunk, TEST_MAX_FRAME_BYTES
    );
}

static void destroy_fixture(TcpFixture *fixture)
{
    close_fd(&fixture->host_fd);
    close_fd(&fixture->edge_fd);
}

static bool snapshot_is_ready(MeisoSession *session)
{
    MeisoSessionSnapshot snapshot;
    return meiso_session_snapshot(session, &snapshot) &&
        snapshot.state == MEISO_SESSION_STATE_READY &&
        snapshot.failure == MEISO_SESSION_FAILURE_NONE;
}

static bool make_deadline(struct timespec *out_deadline)
{
    if (clock_gettime(CLOCK_MONOTONIC, out_deadline) != 0) {
        return false;
    }
    out_deadline->tv_sec += (time_t)TEST_DEADLINE_SECONDS;
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

static bool wait_for_fd(
    int fd,
    short events,
    const struct timespec *deadline
)
{
    struct pollfd poll_fd = {fd, events, 0};

    while (!deadline_expired(deadline)) {
        const int result = poll(
            &poll_fd, (nfds_t)1U, TEST_POLL_TIMEOUT_MS
        );
        if (result > 0) {
            if ((poll_fd.revents & POLLNVAL) != 0) {
                return false;
            }
            if ((poll_fd.revents & (events | POLLHUP | POLLERR)) != 0) {
                return true;
            }
            poll_fd.revents = 0;
        } else if (result < 0 && errno != EINTR) {
            return false;
        }
    }
    return false;
}

static bool wait_for_fixture(
    const TcpFixture *fixture,
    const struct timespec *deadline
)
{
    struct pollfd poll_fds[2];
    MeisoPosixTcp *adapters[2] = {
        fixture->host_adapter, fixture->edge_adapter
    };
    size_t endpoint;

    for (endpoint = 0U; endpoint < 2U; ++endpoint) {
        short events = 0;
        if (meiso_posix_tcp_wants_read(adapters[endpoint])) {
            events = (short)(events | POLLIN);
        }
        if (meiso_posix_tcp_wants_write(adapters[endpoint])) {
            events = (short)(events | POLLOUT);
        }
        poll_fds[endpoint].fd = endpoint == 0U ?
            fixture->host_fd : fixture->edge_fd;
        poll_fds[endpoint].events = events;
        poll_fds[endpoint].revents = 0;
    }

    while (!deadline_expired(deadline)) {
        const int result = poll(
            poll_fds, (nfds_t)2U, TEST_POLL_TIMEOUT_MS
        );
        if (result > 0) {
            for (endpoint = 0U; endpoint < 2U; ++endpoint) {
                if ((poll_fds[endpoint].revents & POLLNVAL) != 0) {
                    return false;
                }
                if ((poll_fds[endpoint].revents &
                     (poll_fds[endpoint].events | POLLHUP | POLLERR)) != 0) {
                    return true;
                }
                poll_fds[endpoint].revents = 0;
            }
        } else if (result < 0 && errno != EINTR) {
            return false;
        }
    }
    return false;
}

static bool write_until_frame_sent(MeisoPosixTcp *adapter, int fd)
{
    struct timespec deadline;

    if (!make_deadline(&deadline)) {
        return false;
    }
    while (!deadline_expired(&deadline)) {
        const MeisoPosixTcpWriteStatus status =
            meiso_posix_tcp_on_writable(adapter);
        if (status == MEISO_POSIX_TCP_WRITE_FRAME_SENT) {
            return true;
        }
        if (status != MEISO_POSIX_TCP_WRITE_PROGRESS &&
            status != MEISO_POSIX_TCP_WRITE_WOULD_BLOCK &&
            status != MEISO_POSIX_TCP_WRITE_INTERRUPTED) {
            return false;
        }
        if (status != MEISO_POSIX_TCP_WRITE_PROGRESS &&
            !wait_for_fd(fd, POLLOUT, &deadline)) {
            return false;
        }
    }
    return false;
}

static bool drive_started_ready(TcpFixture *fixture)
{
    struct timespec deadline;
    MeisoSessionRuntimeMessage runtime;

    if (!make_deadline(&deadline)) {
        return false;
    }
    while (!deadline_expired(&deadline)) {
        MeisoPosixTcpWriteStatus write_status;
        MeisoPosixTcpReadStatus read_status;

        if (meiso_posix_tcp_wants_write(fixture->host_adapter)) {
            write_status = meiso_posix_tcp_on_writable(
                fixture->host_adapter
            );
            CHECK(write_status <= MEISO_POSIX_TCP_WRITE_FRAME_SENT ||
                write_status == MEISO_POSIX_TCP_WRITE_IDLE);
        }
        if (meiso_posix_tcp_wants_read(fixture->host_adapter)) {
            read_status = meiso_posix_tcp_on_readable(
                fixture->host_adapter, &runtime
            );
            CHECK(read_status == MEISO_POSIX_TCP_READ_PROGRESS ||
                read_status == MEISO_POSIX_TCP_READ_WOULD_BLOCK ||
                read_status == MEISO_POSIX_TCP_READ_INTERRUPTED ||
                read_status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS);
        }
        if (meiso_posix_tcp_wants_write(fixture->edge_adapter)) {
            write_status = meiso_posix_tcp_on_writable(
                fixture->edge_adapter
            );
            CHECK(write_status <= MEISO_POSIX_TCP_WRITE_FRAME_SENT ||
                write_status == MEISO_POSIX_TCP_WRITE_IDLE);
        }
        if (meiso_posix_tcp_wants_read(fixture->edge_adapter)) {
            read_status = meiso_posix_tcp_on_readable(
                fixture->edge_adapter, &runtime
            );
            CHECK(read_status == MEISO_POSIX_TCP_READ_PROGRESS ||
                read_status == MEISO_POSIX_TCP_READ_WOULD_BLOCK ||
                read_status == MEISO_POSIX_TCP_READ_INTERRUPTED ||
                read_status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS);
        }
        if (snapshot_is_ready(fixture->host_session) &&
            snapshot_is_ready(fixture->edge_session)) {
            return true;
        }
        if (!wait_for_fixture(fixture, &deadline)) {
            return false;
        }
    }
    return false;
}

static bool drive_ready(TcpFixture *fixture)
{
    CHECK(meiso_session_start(fixture->edge_session) ==
        MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(fixture->host_session) ==
        MEISO_SESSION_START_STARTED);
    return drive_started_ready(fixture);
}

static bool read_until_runtime(
    MeisoPosixTcp *adapter,
    int fd,
    const uint8_t *expected,
    size_t expected_size
)
{
    struct timespec deadline;
    MeisoSessionRuntimeMessage runtime;

    if (!make_deadline(&deadline)) {
        return false;
    }
    while (!deadline_expired(&deadline)) {
        const MeisoPosixTcpReadStatus status = meiso_posix_tcp_on_readable(
            adapter, &runtime
        );
        if (status == MEISO_POSIX_TCP_READ_RUNTIME) {
            return runtime.payload.size == expected_size &&
                (expected_size == 0U || memcmp(
                    runtime.payload.data, expected, expected_size
                ) == 0);
        }
        if (status != MEISO_POSIX_TCP_READ_PROGRESS &&
            status != MEISO_POSIX_TCP_READ_WOULD_BLOCK &&
            status != MEISO_POSIX_TCP_READ_INTERRUPTED) {
            return false;
        }
        if (status != MEISO_POSIX_TCP_READ_PROGRESS &&
            !wait_for_fd(fd, POLLIN, &deadline)) {
            return false;
        }
    }
    return false;
}

static bool read_until_status(
    MeisoPosixTcp *adapter,
    int fd,
    MeisoPosixTcpReadStatus expected
)
{
    struct timespec deadline;
    MeisoSessionRuntimeMessage runtime;

    if (!make_deadline(&deadline)) {
        return false;
    }
    while (!deadline_expired(&deadline)) {
        const MeisoPosixTcpReadStatus status = meiso_posix_tcp_on_readable(
            adapter, &runtime
        );
        if (status == expected) {
            return true;
        }
        if (status != MEISO_POSIX_TCP_READ_PROGRESS &&
            status != MEISO_POSIX_TCP_READ_WOULD_BLOCK &&
            status != MEISO_POSIX_TCP_READ_INTERRUPTED) {
            return false;
        }
        if (status != MEISO_POSIX_TCP_READ_PROGRESS &&
            !wait_for_fd(fd, POLLIN, &deadline)) {
            return false;
        }
    }
    return false;
}

static bool write_until_status(
    MeisoPosixTcp *adapter,
    int fd,
    MeisoPosixTcpWriteStatus expected
)
{
    struct timespec deadline;

    if (!make_deadline(&deadline)) {
        return false;
    }
    while (!deadline_expired(&deadline)) {
        const MeisoPosixTcpWriteStatus status =
            meiso_posix_tcp_on_writable(adapter);
        if (status == expected) {
            return true;
        }
        if (status != MEISO_POSIX_TCP_WRITE_PROGRESS &&
            status != MEISO_POSIX_TCP_WRITE_WOULD_BLOCK &&
            status != MEISO_POSIX_TCP_WRITE_INTERRUPTED) {
            return false;
        }
        if (status != MEISO_POSIX_TCP_WRITE_PROGRESS &&
            !wait_for_fd(fd, POLLOUT, &deadline)) {
            return false;
        }
    }
    return false;
}

static bool test_init_and_read_would_block(void)
{
    SessionArena session_arena;
    AdapterArena adapter_arena;
    MeisoSessionConfig session_config = host_config();
    MeisoSession *session;
    MeisoPosixTcp *adapter = NULL;
    MeisoSessionRuntimeMessage runtime;
    MeisoSessionSnapshot before;
    MeisoSessionSnapshot after;
    MeisoPosixTcpConfig adapter_config;
    AdapterArena overlap_arena;
    AdapterArena overlap_before;
    MeisoPosixTcpConfig overlap_config;
    MeisoPosixTcpConfig overlap_config_before;
    int client = -1;
    int server = -1;
    int unconnected = -1;
    int flags;

    CHECK(make_tcp_pair(&client, &server));
    CHECK(initialize_session(&session_arena, &session_config, &session));
    adapter_config = (MeisoPosixTcpConfig){
        client, session, TEST_MAX_FRAME_BYTES, SIZE_MAX
    };
    CHECK(meiso_posix_tcp_memory_required(&adapter_config) <=
        sizeof(adapter_arena.bytes));
    flags = fcntl(client, F_GETFL);
    CHECK(flags >= 0);
    CHECK(fcntl(client, F_SETFL, flags & ~O_NONBLOCK) == 0);
    CHECK(meiso_posix_tcp_init(
        adapter_arena.bytes,
        sizeof(adapter_arena.bytes),
        &adapter_config,
        &adapter
    ) == MEISO_POSIX_TCP_INIT_NONBLOCKING_REQUIRED);
    CHECK(adapter == NULL);
    CHECK(set_nonblocking(client));
    unconnected = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(unconnected >= 0 && set_nonblocking(unconnected));
    adapter_config.connected_fd = unconnected;
    CHECK(meiso_posix_tcp_init(
        adapter_arena.bytes,
        sizeof(adapter_arena.bytes),
        &adapter_config,
        &adapter
    ) == MEISO_POSIX_TCP_INIT_FD_ERROR);
    CHECK(adapter == NULL);
    adapter_config.connected_fd = client;
    CHECK(meiso_posix_tcp_init(
        adapter_arena.bytes,
        sizeof(adapter_arena.bytes),
        &adapter_config,
        &adapter
    ) == MEISO_POSIX_TCP_INIT_OK);
    CHECK(meiso_session_snapshot(session, &before));
    CHECK(meiso_posix_tcp_on_readable(adapter, &runtime) ==
        MEISO_POSIX_TCP_READ_WOULD_BLOCK);
    CHECK(meiso_session_snapshot(session, &after));
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);

    CHECK(meiso_posix_tcp_init(
        overlap_arena.bytes,
        sizeof(overlap_arena.bytes),
        &adapter_config,
        (MeisoPosixTcp **)(void *)session
    ) == MEISO_POSIX_TCP_INIT_INVALID_ARGUMENT);
    CHECK(meiso_session_snapshot(session, &after));
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);

    memset(&overlap_arena, 0xa5, sizeof(overlap_arena));
    overlap_before = overlap_arena;
    CHECK(meiso_posix_tcp_init(
        overlap_arena.bytes,
        sizeof(overlap_arena.bytes),
        &adapter_config,
        (MeisoPosixTcp **)(void *)overlap_arena.bytes
    ) == MEISO_POSIX_TCP_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(
        &overlap_arena, &overlap_before, sizeof(overlap_arena)
    ) == 0);

    overlap_config = adapter_config;
    overlap_config_before = overlap_config;
    CHECK(meiso_posix_tcp_init(
        overlap_arena.bytes,
        sizeof(overlap_arena.bytes),
        &overlap_config,
        (MeisoPosixTcp **)(void *)&overlap_config
    ) == MEISO_POSIX_TCP_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(
        &overlap_config, &overlap_config_before, sizeof(overlap_config)
    ) == 0);

    overlap_arena = (AdapterArena){0};
    overlap_before = overlap_arena;
    overlap_config = adapter_config;
    overlap_config.session = (MeisoSession *)(void *)overlap_arena.bytes;
    CHECK(meiso_posix_tcp_init(
        overlap_arena.bytes,
        sizeof(overlap_arena.bytes),
        &overlap_config,
        &adapter
    ) == MEISO_POSIX_TCP_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(
        &overlap_arena, &overlap_before, sizeof(overlap_arena)
    ) == 0);

    adapter_config.max_io_bytes_per_step = 0U;
    CHECK(meiso_posix_tcp_memory_required(&adapter_config) == 0U);
    adapter_config.max_io_bytes_per_step = 1U;
    adapter_config.max_frame_bytes =
        MEISO_SESSION_MIN_MAX_FRAME_BYTES - 1U;
    CHECK(meiso_posix_tcp_memory_required(&adapter_config) == 0U);
    close_fd(&unconnected);
    close_fd(&client);
    close_fd(&server);
    return true;
}

static bool test_one_byte_handshake_and_full_duplex(void)
{
    TcpFixture fixture;
    MeisoSessionSnapshot snapshot;
    const uint8_t host_payload[] = {0x10U, 0x11U, 0x12U, 0x13U};
    const uint8_t edge_payload[] = {0x20U, 0x21U, 0x22U};
    struct timespec deadline;
    size_t progress_count = 0U;
    bool open_sent = false;

    CHECK(initialize_fixture(&fixture, 1U));
    CHECK(meiso_session_start(fixture.edge_session) ==
        MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(fixture.host_session) ==
        MEISO_SESSION_START_STARTED);
    CHECK(make_deadline(&deadline));
    while (!deadline_expired(&deadline)) {
        const MeisoPosixTcpWriteStatus status =
            meiso_posix_tcp_on_writable(fixture.host_adapter);
        if (status == MEISO_POSIX_TCP_WRITE_FRAME_SENT) {
            open_sent = true;
            break;
        }
        CHECK(status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
            status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK ||
            status == MEISO_POSIX_TCP_WRITE_INTERRUPTED);
        CHECK(meiso_session_snapshot(fixture.host_session, &snapshot));
        CHECK(snapshot.outbound_pending);
        if (status == MEISO_POSIX_TCP_WRITE_PROGRESS) {
            ++progress_count;
        } else {
            CHECK(wait_for_fd(fixture.host_fd, POLLOUT, &deadline));
        }
    }
    CHECK(open_sent);
    CHECK(progress_count == MEISO_SESSION_MIN_MAX_FRAME_BYTES - 1U);
    CHECK(meiso_session_snapshot(fixture.host_session, &snapshot));
    CHECK(!snapshot.outbound_pending);

    CHECK(drive_started_ready(&fixture));

    CHECK(meiso_session_submit_runtime(
        fixture.host_session,
        (MeisoByteView){host_payload, sizeof(host_payload)}
    ) == MEISO_SESSION_SUBMIT_QUEUED);
    CHECK(meiso_session_submit_runtime(
        fixture.edge_session,
        (MeisoByteView){edge_payload, sizeof(edge_payload)}
    ) == MEISO_SESSION_SUBMIT_QUEUED);

    CHECK(meiso_posix_tcp_on_writable(fixture.host_adapter) ==
        MEISO_POSIX_TCP_WRITE_PROGRESS);
    CHECK(write_until_frame_sent(fixture.edge_adapter, fixture.edge_fd));
    CHECK(read_until_runtime(
        fixture.host_adapter,
        fixture.host_fd,
        edge_payload,
        sizeof(edge_payload)
    ));
    CHECK(write_until_frame_sent(fixture.host_adapter, fixture.host_fd));
    CHECK(read_until_runtime(
        fixture.edge_adapter,
        fixture.edge_fd,
        host_payload,
        sizeof(host_payload)
    ));
    CHECK(snapshot_is_ready(fixture.host_session));
    CHECK(snapshot_is_ready(fixture.edge_session));
    destroy_fixture(&fixture);
    return true;
}

static bool test_coalesced_runtime_frames(void)
{
    TcpFixture fixture;
    const uint8_t first[] = {0x31U};
    const uint8_t second[] = {0x41U, 0x42U};

    CHECK(initialize_fixture(&fixture, TEST_MAX_FRAME_BYTES));
    CHECK(drive_ready(&fixture));
    CHECK(meiso_session_submit_runtime(
        fixture.host_session, (MeisoByteView){first, sizeof(first)}
    ) == MEISO_SESSION_SUBMIT_QUEUED);
    CHECK(write_until_frame_sent(fixture.host_adapter, fixture.host_fd));
    CHECK(meiso_session_submit_runtime(
        fixture.host_session, (MeisoByteView){second, sizeof(second)}
    ) == MEISO_SESSION_SUBMIT_QUEUED);
    CHECK(write_until_frame_sent(fixture.host_adapter, fixture.host_fd));
    CHECK(read_until_runtime(
        fixture.edge_adapter, fixture.edge_fd, first, sizeof(first)
    ));
    CHECK(read_until_runtime(
        fixture.edge_adapter, fixture.edge_fd, second, sizeof(second)
    ));
    destroy_fixture(&fixture);
    return true;
}

static bool fill_send_buffer(int fd)
{
    struct timespec deadline;
    uint8_t bytes[4096];

    memset(bytes, 0xa5, sizeof(bytes));
    if (!make_deadline(&deadline)) {
        return false;
    }
    while (!deadline_expired(&deadline)) {
        const ssize_t sent = send(fd, bytes, sizeof(bytes), 0);
        if (sent > 0 || (sent < 0 && errno == EINTR)) {
            continue;
        }
        return sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
    return false;
}

static bool test_backpressure_does_not_mark_early(void)
{
    SessionArena session_arena;
    AdapterArena adapter_arena;
    MeisoSessionConfig config = host_config();
    MeisoSession *session;
    MeisoPosixTcp *adapter;
    MeisoSessionSnapshot snapshot;
    MeisoPosixTcpWriteStatus write_status;
    int client = -1;
    int server = -1;
    int send_buffer = 1024;
    int receive_buffer = 1024;

    CHECK(make_tcp_pair_configured(
        &client, &server, send_buffer, receive_buffer
    ));
    CHECK(initialize_session(&session_arena, &config, &session));
    CHECK(initialize_adapter(
        &adapter_arena, client, session, 1U, &adapter
    ));
    CHECK(meiso_session_start(session) == MEISO_SESSION_START_STARTED);
    CHECK(fill_send_buffer(client));
    write_status = meiso_posix_tcp_on_writable(adapter);
    CHECK(write_status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK ||
        write_status == MEISO_POSIX_TCP_WRITE_PROGRESS);
    CHECK(meiso_session_snapshot(session, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_HANDSHAKING);
    CHECK(snapshot.outbound_pending);
    if (write_status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK) {
        CHECK(meiso_session_close(session, MEISO_SESSION_CLOSE_NORMAL) ==
            MEISO_SESSION_CLOSE_LOCAL_ONLY);
        CHECK(meiso_posix_tcp_on_writable(adapter) ==
            MEISO_POSIX_TCP_WRITE_IDLE);
        CHECK(!meiso_posix_tcp_wants_write(adapter));
        CHECK(meiso_session_snapshot(session, &snapshot));
        CHECK(snapshot.state == MEISO_SESSION_STATE_CLOSED);
        CHECK(snapshot.failure == MEISO_SESSION_FAILURE_LOCAL_CLOSED);
    }
    close_fd(&client);
    close_fd(&server);
    return true;
}

static bool test_partial_tx_reconciles_external_terminal(void)
{
    SessionArena session_arena;
    AdapterArena adapter_arena;
    MeisoSessionConfig config = host_config();
    MeisoSession *session;
    MeisoPosixTcp *adapter;
    MeisoSessionSnapshot snapshot;
    struct timespec deadline;
    uint8_t received[8];
    int adapter_error = 0;
    int client = -1;
    int server = -1;
    ssize_t received_size;

    CHECK(make_tcp_pair(&client, &server));
    CHECK(initialize_session(&session_arena, &config, &session));
    CHECK(initialize_adapter(&adapter_arena, client, session, 1U, &adapter));
    CHECK(meiso_session_start(session) == MEISO_SESSION_START_STARTED);
    CHECK(meiso_posix_tcp_on_writable(adapter) ==
        MEISO_POSIX_TCP_WRITE_PROGRESS);
    meiso_session_transport_lost(session);
    CHECK(meiso_posix_tcp_on_writable(adapter) ==
        MEISO_POSIX_TCP_WRITE_TRANSPORT_LOST);
    CHECK(meiso_posix_tcp_last_error(adapter, &adapter_error));
    CHECK(adapter_error == EPROTO);
    close_fd(&client);
    CHECK(make_deadline(&deadline));
    CHECK(wait_for_fd(server, POLLIN, &deadline));
    received_size = recv(server, received, sizeof(received), 0);
    CHECK(received_size == 1);
    CHECK(received[0] == (uint8_t)'M');
    CHECK(wait_for_fd(server, POLLIN, &deadline));
    received_size = recv(server, received, sizeof(received), 0);
    CHECK(received_size == 0);
    CHECK(meiso_session_snapshot(session, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_FAILED);
    CHECK(snapshot.failure == MEISO_SESSION_FAILURE_TRANSPORT_LOST);
    close_fd(&server);
    return true;
}

static void write_u32_be(uint8_t *target, uint32_t value)
{
    target[0] = (uint8_t)(value >> 24U);
    target[1] = (uint8_t)(value >> 16U);
    target[2] = (uint8_t)(value >> 8U);
    target[3] = (uint8_t)value;
}

static bool send_all_raw(int fd, const uint8_t *data, size_t size)
{
    struct timespec deadline;
    size_t sent_size = 0U;

    if (!make_deadline(&deadline)) {
        return false;
    }
    while (sent_size < size && !deadline_expired(&deadline)) {
        const ssize_t sent = send(fd, data + sent_size, size - sent_size, 0);
        if (sent > 0) {
            sent_size += (size_t)sent;
        } else {
            if (sent == 0 || (errno != EINTR && errno != EAGAIN &&
                             errno != EWOULDBLOCK)) {
                return false;
            }
            if (!wait_for_fd(fd, POLLOUT, &deadline)) {
                return false;
            }
        }
    }
    return sent_size == size;
}

static bool test_direct_session_output_aliases(void)
{
    TcpFixture fixture;
    MeisoSessionSnapshot before;
    MeisoSessionSnapshot after;
    uint8_t header[MEISO_SESSION_WIRE_HEADER_SIZE] = {
        (uint8_t)'M', (uint8_t)'S', (uint8_t)'L', (uint8_t)'0',
        (uint8_t)MEISO_SESSION_WIRE_VERSION, 1U, 0U, 0U,
        0U, 0U, 0U, 0U
    };

    CHECK(initialize_fixture(&fixture, TEST_MAX_FRAME_BYTES));
    CHECK(meiso_session_start(fixture.edge_session) ==
        MEISO_SESSION_START_STARTED);
    write_u32_be(header + 8U, TEST_MAX_FRAME_BYTES + 1U);
    CHECK(send_all_raw(fixture.host_fd, header, sizeof(header)));
    CHECK(meiso_session_snapshot(fixture.edge_session, &before));
    CHECK(meiso_posix_tcp_on_readable(
        fixture.edge_adapter,
        (MeisoSessionRuntimeMessage *)(void *)fixture.edge_session
    ) == MEISO_POSIX_TCP_READ_INVALID_ARGUMENT);
    CHECK(meiso_session_snapshot(fixture.edge_session, &after));
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    CHECK(read_until_status(
        fixture.edge_adapter,
        fixture.edge_fd,
        MEISO_POSIX_TCP_READ_SESSION_FAILED
    ));
    CHECK(meiso_session_snapshot(fixture.edge_session, &before));
    CHECK(before.state == MEISO_SESSION_STATE_FAILED);
    CHECK(before.failure == MEISO_SESSION_FAILURE_MALFORMED_FRAME);
    CHECK(!meiso_posix_tcp_last_error(
        fixture.edge_adapter, (int *)(void *)fixture.edge_session
    ));
    CHECK(meiso_session_snapshot(fixture.edge_session, &after));
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    destroy_fixture(&fixture);
    return true;
}

static bool test_invalid_length_and_eof(void)
{
    TcpFixture invalid;
    TcpFixture eof_mid;
    MeisoSessionSnapshot snapshot;
    uint8_t header[MEISO_SESSION_WIRE_HEADER_SIZE] = {
        (uint8_t)'M', (uint8_t)'S', (uint8_t)'L', (uint8_t)'0',
        (uint8_t)MEISO_SESSION_WIRE_VERSION, 1U, 0U, 0U,
        0U, 0U, 0U, 0U
    };

    CHECK(initialize_fixture(&invalid, TEST_MAX_FRAME_BYTES));
    CHECK(meiso_session_start(invalid.edge_session) ==
        MEISO_SESSION_START_STARTED);
    write_u32_be(header + 8U, TEST_MAX_FRAME_BYTES + 1U);
    CHECK(send_all_raw(invalid.host_fd, header, sizeof(header)));
    CHECK(read_until_status(
        invalid.edge_adapter,
        invalid.edge_fd,
        MEISO_POSIX_TCP_READ_SESSION_FAILED
    ));
    CHECK(meiso_session_snapshot(invalid.edge_session, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_FAILED);
    CHECK(snapshot.failure == MEISO_SESSION_FAILURE_MALFORMED_FRAME);
    destroy_fixture(&invalid);

    CHECK(initialize_fixture(&eof_mid, 3U));
    CHECK(meiso_session_start(eof_mid.edge_session) ==
        MEISO_SESSION_START_STARTED);
    write_u32_be(header + 8U, MEISO_SESSION_MIN_MAX_FRAME_BYTES);
    CHECK(send_all_raw(eof_mid.host_fd, header, 5U));
    close_fd(&eof_mid.host_fd);
    CHECK(read_until_status(
        eof_mid.edge_adapter,
        eof_mid.edge_fd,
        MEISO_POSIX_TCP_READ_TRANSPORT_LOST
    ));
    CHECK(meiso_session_snapshot(eof_mid.edge_session, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_FAILED);
    CHECK(snapshot.failure == MEISO_SESSION_FAILURE_TRANSPORT_LOST);
    destroy_fixture(&eof_mid);
    return true;
}

static bool test_undersized_adapter_capacity_fails_boundedly(void)
{
    TcpFixture fixture;
    MeisoSessionSnapshot snapshot;
    uint8_t payload[64];

    memset(payload, 0x5a, sizeof(payload));
    CHECK(initialize_fixture_with_host_max(
        &fixture,
        TEST_MAX_FRAME_BYTES,
        MEISO_SESSION_MIN_MAX_FRAME_BYTES
    ));
    CHECK(drive_ready(&fixture));
    CHECK(meiso_session_submit_runtime(
        fixture.host_session, (MeisoByteView){payload, sizeof(payload)}
    ) == MEISO_SESSION_SUBMIT_QUEUED);
    CHECK(meiso_posix_tcp_on_writable(fixture.host_adapter) ==
        MEISO_POSIX_TCP_WRITE_TRANSPORT_LOST);
    CHECK(meiso_session_snapshot(fixture.host_session, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_FAILED);
    CHECK(snapshot.failure == MEISO_SESSION_FAILURE_TRANSPORT_LOST);
    CHECK(!snapshot.outbound_pending);
    destroy_fixture(&fixture);
    return true;
}

static bool test_partial_prefix_terminal_drops_reject(void)
{
    TcpFixture fixture;
    MeisoSessionSnapshot snapshot;
    const uint8_t payload[] = {0x51U, 0x52U};
    uint8_t malformed[MEISO_SESSION_WIRE_HEADER_SIZE] = {
        (uint8_t)'X', (uint8_t)'S', (uint8_t)'L', (uint8_t)'0',
        (uint8_t)MEISO_SESSION_WIRE_VERSION, 7U, 0U, 0U,
        0U, 0U, 0U, (uint8_t)MEISO_SESSION_WIRE_HEADER_SIZE
    };

    CHECK(initialize_fixture(&fixture, 1U));
    CHECK(drive_ready(&fixture));
    CHECK(meiso_session_submit_runtime(
        fixture.host_session, (MeisoByteView){payload, sizeof(payload)}
    ) == MEISO_SESSION_SUBMIT_QUEUED);
    CHECK(meiso_posix_tcp_on_writable(fixture.host_adapter) ==
        MEISO_POSIX_TCP_WRITE_PROGRESS);
    CHECK(send_all_raw(fixture.edge_fd, malformed, sizeof(malformed)));
    CHECK(read_until_status(
        fixture.host_adapter,
        fixture.host_fd,
        MEISO_POSIX_TCP_READ_SESSION_FAILED
    ));
    CHECK(meiso_session_snapshot(fixture.host_session, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_FAILED);
    CHECK(snapshot.failure == MEISO_SESSION_FAILURE_MALFORMED_FRAME);
    CHECK(!snapshot.outbound_pending);
    CHECK(!meiso_posix_tcp_wants_write(fixture.host_adapter));
    destroy_fixture(&fixture);
    return true;
}

static bool test_peer_reset_does_not_raise_sigpipe(void)
{
    SessionArena session_arena;
    AdapterArena adapter_arena;
    MeisoSessionConfig config = host_config();
    MeisoSession *session;
    MeisoPosixTcp *adapter;
    MeisoSessionSnapshot snapshot;
    struct linger reset = {1, 0};
    int client = -1;
    int server = -1;

    CHECK(make_tcp_pair(&client, &server));
    CHECK(initialize_session(&session_arena, &config, &session));
    CHECK(initialize_adapter(
        &adapter_arena, client, session, 1U, &adapter
    ));
    CHECK(meiso_session_start(session) == MEISO_SESSION_START_STARTED);
    CHECK(setsockopt(
        server, SOL_SOCKET, SO_LINGER, &reset, (socklen_t)sizeof(reset)
    ) == 0);
    close_fd(&server);
    CHECK(write_until_status(
        adapter, client, MEISO_POSIX_TCP_WRITE_TRANSPORT_LOST
    ));
    CHECK(meiso_session_snapshot(session, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_FAILED);
    CHECK(snapshot.failure == MEISO_SESSION_FAILURE_TRANSPORT_LOST);
    close_fd(&client);
    return true;
}

int main(void)
{
    if (!test_init_and_read_would_block() ||
        !test_one_byte_handshake_and_full_duplex() ||
        !test_coalesced_runtime_frames() ||
        !test_backpressure_does_not_mark_early() ||
        !test_partial_tx_reconciles_external_terminal() ||
        !test_direct_session_output_aliases() ||
        !test_invalid_length_and_eof() ||
        !test_undersized_adapter_capacity_fails_boundedly() ||
        !test_partial_prefix_terminal_drops_reject() ||
        !test_peer_reset_does_not_raise_sigpipe()) {
        return 1;
    }
    puts("posix tcp adapter tests passed");
    return 0;
}
