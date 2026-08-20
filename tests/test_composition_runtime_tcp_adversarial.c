#define _POSIX_C_SOURCE 200809L

#include "composition_session_v2.h"
#include "meiso_blob.h"
#include "meiso_posix_tcp.h"
#include "meiso_runtime_scheduler.h"
#include "meiso_state.h"
#include "meiso_stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(                                                      \
                stderr, "CHECK failed at %s:%d: %s\n",                      \
                __FILE__, __LINE__, #condition                                  \
            );                                                                  \
            return false;                                                       \
        }                                                                       \
    } while (0)

enum {
    TEST_SESSION_ARENA_BYTES = 65536,
    TEST_ADAPTER_ARENA_BYTES = 32768,
    TEST_RUNTIME_ARENA_BYTES = 400000,
    TEST_SCHEDULER_ARENA_BYTES = 16384,
    TEST_IO_BYTES_PER_STEP = MEISO_COMPOSITION_SESSION_V2_MAX_FRAME_BYTES,
    TEST_POLL_SLICE_MS = 20,
    TEST_DEADLINE_MS = 10000,
    TEST_SOCKET_BUFFER_BYTES = 1024,
    TEST_BACKPRESSURE_FRAME_LIMIT = 4096,
    TEST_HOST_SOURCE_COUNT = 10,
    TEST_EDGE_SOURCE_COUNT = 10
};

enum {
    ROUTE_WIFI_DESIRED = 0,
    ROUTE_WIFI_ACTUAL = 1,
    ROUTE_WIFI_DATA = 2,
    ROUTE_PRESENTATION_DESIRED = 3,
    ROUTE_PRESENTATION_ACTUAL = 4,
    ROUTE_SCENE_DESIRED = 5,
    ROUTE_SCENE_ACTUAL = 6,
    ROUTE_MESH_BLOB = 7,
    ROUTE_RENDER_RESULT = 8
};

enum {
    HOST_WIFI_DESIRED = 0,
    HOST_PLAN_DESIRED = 1,
    HOST_SCENE_DESIRED = 2,
    HOST_WIFI_ACTUAL_ACCEPT = 3,
    HOST_PRESENTATION_ACTUAL_ACCEPT = 4,
    HOST_SCENE_ACTUAL_ACCEPT = 5,
    HOST_BLOB_BEGIN = 6,
    HOST_BLOB_CHUNK = 7,
    HOST_BLOB_ABORT = 8,
    HOST_CATALOG_ACCEPT = 9
};

enum {
    EDGE_WIFI_DESIRED_ACCEPT = 0,
    EDGE_PLAN_DESIRED_ACCEPT = 1,
    EDGE_SCENE_DESIRED_ACCEPT = 2,
    EDGE_WIFI_ACTUAL = 3,
    EDGE_PRESENTATION_ACTUAL = 4,
    EDGE_SCENE_ACTUAL = 5,
    EDGE_BLOB_RESULT = 6,
    EDGE_WIFI_STREAM = 7,
    EDGE_RENDER_STREAM = 8,
    EDGE_CATALOG_OFFER = 9
};

typedef union {
    max_align_t alignment;
    uint8_t bytes[TEST_SESSION_ARENA_BYTES];
} SessionArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[TEST_ADAPTER_ARENA_BYTES];
} AdapterArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[TEST_RUNTIME_ARENA_BYTES];
} RuntimeArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[TEST_SCHEDULER_ARENA_BYTES];
} SchedulerArena;

typedef struct {
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeSchedulerSourceId id;
    MeisoRuntimeSchedulerPollStatus poll_status;
    uint64_t polls;
    uint64_t completions;
    uint64_t boundaries;
    MeisoRuntimeSchedulerCompletion last_completion;
} TxSource;

typedef struct {
    int fd;
    SessionArena session_arena;
    AdapterArena adapter_arena;
    RuntimeArena runtime_arena;
    SchedulerArena scheduler_arena;
    size_t session_bytes;
    size_t adapter_bytes;
    size_t runtime_bytes;
    size_t scheduler_bytes;
    MeisoSession *session;
    MeisoPosixTcp *adapter;
    MeisoRuntime *runtime;
    MeisoRuntimeScheduler *scheduler;
    TxSource *sources;
    size_t source_count;
} Peer;

typedef struct {
    Peer host;
    Peer edge;
    TxSource host_sources[TEST_HOST_SOURCE_COUNT];
    TxSource edge_sources[TEST_EDGE_SOURCE_COUNT];
    MeisoRuntimeEndpointDescriptor endpoints[
        MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT
    ];
    MeisoRuntimeRouteDescriptor routes[
        MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT
    ];
    uint8_t wifi_capability[
        MEISO_COMPOSITION_SESSION_V2_WIFI_CAPABILITY_BYTES
    ];
    uint64_t catalog_generation;
} TcpFixture;

typedef struct {
    void *publisher_arena;
    void *replica_arena;
    MeisoStatePublisher *publisher;
    MeisoStateReplica *replica;
    MeisoDeliveryGeneration delivery;
} StateChannel;

typedef struct {
    TcpFixture tcp;
    StateChannel wifi_desired;
    StateChannel plan_desired;
    StateChannel scene_desired;
    StateChannel wifi_actual;
    StateChannel scene_actual;
    void *wifi_stream_arena;
    void *render_stream_arena;
    MeisoStream *wifi_stream;
    MeisoStream *render_stream;
    void *blob_arena;
    MeisoBlobStore *blob_store;
} SemanticFixture;

typedef struct {
    SchedulerArena scheduler;
    RuntimeArena runtime;
    SessionArena session;
    AdapterArena adapter;
    TxSource sources[TEST_EDGE_SOURCE_COUNT];
    MeisoRuntimeSchedulerStepResult output;
} BusySnapshot;

static const MeisoOwnerScope HOST_OWNER = {
    MEISO_OWNER_ROLE_HOST, UINT64_C(0x5101)
};
static const MeisoOwnerScope EDGE_OWNER = {
    MEISO_OWNER_ROLE_EDGE, UINT64_C(0x5201)
};

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

static bool make_deadline(struct timespec *out_deadline)
{
    if (clock_gettime(CLOCK_MONOTONIC, out_deadline) != 0) {
        return false;
    }
    out_deadline->tv_sec += TEST_DEADLINE_MS / 1000;
    out_deadline->tv_nsec +=
        (long)(TEST_DEADLINE_MS % 1000) * 1000000L;
    if (out_deadline->tv_nsec >= 1000000000L) {
        out_deadline->tv_sec++;
        out_deadline->tv_nsec -= 1000000000L;
    }
    return true;
}

static int remaining_poll_ms(const struct timespec *deadline)
{
    struct timespec now;
    int64_t seconds;
    int64_t nanoseconds;
    int64_t milliseconds;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    seconds = (int64_t)deadline->tv_sec - (int64_t)now.tv_sec;
    nanoseconds = (int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec;
    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += INT64_C(1000000000);
    }
    if (seconds < 0 || (seconds == 0 && nanoseconds == 0)) {
        return 0;
    }
    milliseconds = seconds * INT64_C(1000) +
        (nanoseconds + INT64_C(999999)) / INT64_C(1000000);
    return milliseconds < TEST_POLL_SLICE_MS ?
        (int)milliseconds : TEST_POLL_SLICE_MS;
}

static bool wait_fd(int fd, short events, const struct timespec *deadline)
{
    struct pollfd item = {fd, events, 0};
    int timeout;

    while ((timeout = remaining_poll_ms(deadline)) > 0) {
        const int result = poll(&item, (nfds_t)1U, timeout);

        if (result > 0) {
            return (item.revents & POLLNVAL) == 0 &&
                (item.revents & (events | POLLERR | POLLHUP)) != 0;
        }
        if (result < 0 && errno != EINTR) {
            return false;
        }
        item.revents = 0;
    }
    return false;
}

static bool configure_socket_buffers(int fd, int bytes)
{
    return bytes <= 0 ||
        (setsockopt(
             fd, SOL_SOCKET, SO_SNDBUF,
             &bytes, (socklen_t)sizeof(bytes)
         ) == 0 &&
         setsockopt(
             fd, SOL_SOCKET, SO_RCVBUF,
             &bytes, (socklen_t)sizeof(bytes)
         ) == 0);
}

static bool make_tcp_pair(int buffer_bytes, int *out_client, int *out_server)
{
    struct sockaddr_in address;
    struct timespec deadline;
    socklen_t address_size = (socklen_t)sizeof(address);
    int listener = -1;
    int client = -1;
    int server = -1;
    int enabled = 1;
    int connect_error = 0;
    socklen_t connect_error_size = (socklen_t)sizeof(connect_error);

    *out_client = -1;
    *out_server = -1;
    if (!make_deadline(&deadline)) {
        return false;
    }
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
            listener,
            (const struct sockaddr *)(const void *)&address,
            (socklen_t)sizeof(address)
        ) != 0 || listen(listener, 1) != 0 || getsockname(
            listener, (struct sockaddr *)(void *)&address, &address_size
        ) != 0 || !set_nonblocking(listener)) {
        goto fail;
    }
    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0 || !set_nonblocking(client) ||
        !configure_socket_buffers(client, buffer_bytes)) {
        goto fail;
    }
    if (connect(
            client,
            (const struct sockaddr *)(const void *)&address,
            address_size
        ) != 0 && (errno != EINPROGRESS ||
                   !wait_fd(client, POLLOUT, &deadline) ||
                   getsockopt(
                       client, SOL_SOCKET, SO_ERROR,
                       &connect_error, &connect_error_size
                   ) != 0 || connect_error != 0)) {
        goto fail;
    }
    while (server < 0 && remaining_poll_ms(&deadline) > 0) {
        server = accept(listener, NULL, NULL);
        if (server < 0 && errno != EINTR && errno != EAGAIN &&
            errno != EWOULDBLOCK) {
            goto fail;
        }
        if (server < 0 && errno != EINTR &&
            !wait_fd(listener, POLLIN, &deadline)) {
            goto fail;
        }
    }
    if (server < 0 || !set_nonblocking(server) ||
        !configure_socket_buffers(server, buffer_bytes)) {
        goto fail;
    }
    close_fd(&listener);
    *out_client = client;
    *out_server = server;
    return true;

fail:
    close_fd(&server);
    close_fd(&client);
    close_fd(&listener);
    return false;
}

static MeisoSessionConfig base_session_config(
    MeisoSessionRole role,
    uint64_t nonce_salt
)
{
    const MeisoPeerId host = {
        UINT64_C(0x1111111111111111), UINT64_C(0x1212121212121212)
    };
    const MeisoPeerId edge = {
        UINT64_C(0x2121212121212121), UINT64_C(0x2222222222222222)
    };
    MeisoSessionConfig config;

    memset(&config, 0, sizeof(config));
    config.local_role = role;
    config.local_peer_id = role == MEISO_SESSION_ROLE_HOST ? host : edge;
    config.expected_peer_id = role == MEISO_SESSION_ROLE_HOST ? edge : host;
    config.local_incarnation = role == MEISO_SESSION_ROLE_HOST ?
        HOST_OWNER.incarnation : EDGE_OWNER.incarnation;
    config.local_nonce = role == MEISO_SESSION_ROLE_HOST ?
        (MeisoSessionNonce){UINT64_C(0x3131313131313131), nonce_salt + 1U} :
        (MeisoSessionNonce){UINT64_C(0x4141414141414141), nonce_salt + 2U};
    config.protocol = (MeisoProtocolVersion){1U, 0U};
    config.profile = *meiso_composition_scene_profile_v2();
    config.limits = (MeisoSessionLimits){16384U, 8192U, 32U};
    return config;
}

static bool initialize_session(
    Peer *peer,
    const MeisoSessionConfig *config
)
{
    peer->session_bytes = meiso_session_memory_required(config);
    CHECK(peer->session_bytes != 0U &&
          peer->session_bytes <= sizeof(peer->session_arena.bytes));
    CHECK(meiso_session_init(
        peer->session_arena.bytes,
        peer->session_bytes,
        config,
        &peer->session
    ) == MEISO_SESSION_INIT_OK);
    return true;
}

static bool initialize_adapter(Peer *peer)
{
    const MeisoPosixTcpConfig config = {
        peer->fd,
        peer->session,
        MEISO_COMPOSITION_SESSION_V2_MAX_FRAME_BYTES,
        TEST_IO_BYTES_PER_STEP
    };

    peer->adapter_bytes = meiso_posix_tcp_memory_required(&config);
    CHECK(peer->adapter_bytes != 0U &&
          peer->adapter_bytes <= sizeof(peer->adapter_arena.bytes));
    CHECK(meiso_posix_tcp_init(
        peer->adapter_arena.bytes,
        peer->adapter_bytes,
        &config,
        &peer->adapter
    ) == MEISO_POSIX_TCP_INIT_OK);
    return true;
}

static bool session_ready(const MeisoSession *session)
{
    MeisoSessionSnapshot snapshot;

    return meiso_session_snapshot(session, &snapshot) &&
        snapshot.state == MEISO_SESSION_STATE_READY &&
        snapshot.failure == MEISO_SESSION_FAILURE_NONE &&
        snapshot.session_id_known &&
        snapshot.negotiated_limits.max_frame_bytes ==
            MEISO_COMPOSITION_SESSION_V2_MAX_FRAME_BYTES &&
        snapshot.negotiated_limits.max_runtime_payload_bytes ==
            MEISO_COMPOSITION_SESSION_V2_MAX_RUNTIME_PAYLOAD_BYTES &&
        snapshot.negotiated_limits.max_registry_entries ==
            MEISO_COMPOSITION_SESSION_V2_MAX_REGISTRY_ENTRIES;
}

static bool wait_pair(TcpFixture *fixture, const struct timespec *deadline)
{
    struct pollfd items[2];
    Peer *peers[2] = {&fixture->host, &fixture->edge};
    size_t index;
    int timeout;

    memset(items, 0, sizeof(items));
    for (index = 0U; index < 2U; ++index) {
        items[index].fd = peers[index]->fd;
        if (meiso_posix_tcp_wants_read(peers[index]->adapter)) {
            items[index].events = (short)(items[index].events | POLLIN);
        }
        if (meiso_posix_tcp_wants_write(peers[index]->adapter)) {
            items[index].events = (short)(items[index].events | POLLOUT);
        }
    }
    while ((timeout = remaining_poll_ms(deadline)) > 0) {
        const int result = poll(items, (nfds_t)2U, timeout);

        if (result > 0) {
            for (index = 0U; index < 2U; ++index) {
                if ((items[index].revents & POLLNVAL) != 0) {
                    return false;
                }
                if ((items[index].revents &
                     (items[index].events | POLLERR | POLLHUP)) != 0) {
                    return true;
                }
            }
        } else if (result < 0 && errno != EINTR) {
            return false;
        }
        items[0].revents = 0;
        items[1].revents = 0;
    }
    return false;
}

static bool drive_ready(TcpFixture *fixture)
{
    struct timespec deadline;
    Peer *peers[2] = {&fixture->host, &fixture->edge};

    CHECK(make_deadline(&deadline));
    while (remaining_poll_ms(&deadline) > 0) {
        bool advanced = false;
        size_t index;

        for (index = 0U; index < 2U; ++index) {
            MeisoSessionRuntimeMessage ignored;

            if (meiso_posix_tcp_wants_write(peers[index]->adapter)) {
                const MeisoPosixTcpWriteStatus status =
                    meiso_posix_tcp_on_writable(peers[index]->adapter);

                CHECK(status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
                      status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK ||
                      status == MEISO_POSIX_TCP_WRITE_INTERRUPTED ||
                      status == MEISO_POSIX_TCP_WRITE_FRAME_SENT ||
                      status == MEISO_POSIX_TCP_WRITE_IDLE);
                advanced = advanced ||
                    status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
                    status == MEISO_POSIX_TCP_WRITE_FRAME_SENT;
            }
            if (meiso_posix_tcp_wants_read(peers[index]->adapter)) {
                const MeisoPosixTcpReadStatus status =
                    meiso_posix_tcp_on_readable(
                        peers[index]->adapter, &ignored
                    );

                CHECK(status == MEISO_POSIX_TCP_READ_PROGRESS ||
                      status == MEISO_POSIX_TCP_READ_WOULD_BLOCK ||
                      status == MEISO_POSIX_TCP_READ_INTERRUPTED ||
                      status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS);
                advanced = advanced ||
                    status == MEISO_POSIX_TCP_READ_PROGRESS ||
                    status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS;
            }
        }
        if (session_ready(fixture->host.session) &&
            session_ready(fixture->edge.session)) {
            return true;
        }
        if (!advanced) {
            CHECK(wait_pair(fixture, &deadline));
        }
    }
    return false;
}

static bool write_frame(Peer *peer)
{
    struct timespec deadline;

    CHECK(make_deadline(&deadline));
    while (remaining_poll_ms(&deadline) > 0) {
        const MeisoPosixTcpWriteStatus status =
            meiso_posix_tcp_on_writable(peer->adapter);

        if (status == MEISO_POSIX_TCP_WRITE_FRAME_SENT) {
            return true;
        }
        CHECK(status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
              status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK ||
              status == MEISO_POSIX_TCP_WRITE_INTERRUPTED);
        if (status != MEISO_POSIX_TCP_WRITE_PROGRESS) {
            CHECK(wait_fd(peer->fd, POLLOUT, &deadline));
        }
    }
    return false;
}

static bool read_runtime(
    Peer *peer,
    MeisoSessionRuntimeMessage *out_message
)
{
    struct timespec deadline;

    CHECK(make_deadline(&deadline));
    while (remaining_poll_ms(&deadline) > 0) {
        const MeisoPosixTcpReadStatus status =
            meiso_posix_tcp_on_readable(peer->adapter, out_message);

        if (status == MEISO_POSIX_TCP_READ_RUNTIME) {
            return true;
        }
        CHECK(status == MEISO_POSIX_TCP_READ_PROGRESS ||
              status == MEISO_POSIX_TCP_READ_WOULD_BLOCK ||
              status == MEISO_POSIX_TCP_READ_INTERRUPTED ||
              status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS);
        if (status != MEISO_POSIX_TCP_READ_PROGRESS &&
            status != MEISO_POSIX_TCP_READ_SESSION_PROGRESS) {
            CHECK(wait_fd(peer->fd, POLLIN, &deadline));
        }
    }
    return false;
}

static MeisoRuntimeSchedulerPollStatus source_poll(
    void *context,
    MeisoRuntimeSchedulerWork *out_work
)
{
    TxSource *source = (TxSource *)context;

    if (source == NULL || out_work == NULL) {
        return MEISO_RUNTIME_SCHEDULER_POLL_INVALID;
    }
    source->polls++;
    if (source->poll_status == MEISO_RUNTIME_SCHEDULER_POLL_READY) {
        *out_work = source->work;
    }
    return source->poll_status;
}

static void source_complete(
    void *context,
    const MeisoRuntimeSchedulerCompletion *completion
)
{
    TxSource *source = (TxSource *)context;

    if (source == NULL || completion == NULL) {
        return;
    }
    source->completions++;
    source->last_completion = *completion;
    source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_EMPTY;
    source->work.ticket++;
}

static void source_boundary(
    void *context,
    MeisoRuntimeSchedulerBoundary boundary,
    uint64_t generation
)
{
    TxSource *source = (TxSource *)context;

    (void)boundary;
    (void)generation;
    if (source != NULL) {
        source->boundaries++;
    }
}

static bool initialize_runtime(Peer *peer)
{
    MeisoRuntimeV3Config config;

    CHECK(meiso_composition_session_v2_runtime_config(
        peer->session, &config
    ) == MEISO_COMPOSITION_SESSION_V2_OK);
    peer->runtime_bytes = meiso_runtime_v3_memory_required(&config);
    CHECK(peer->runtime_bytes != 0U &&
          peer->runtime_bytes <= sizeof(peer->runtime_arena.bytes));
    CHECK(meiso_runtime_v3_init(
        peer->runtime_arena.bytes,
        peer->runtime_bytes,
        &config,
        &peer->runtime
    ) == MEISO_RUNTIME_INIT_OK);
    return true;
}

static MeisoRuntimeRouteKey route_key(
    const TcpFixture *fixture,
    size_t index
)
{
    const MeisoRuntimeRouteDescriptor *route = &fixture->routes[index];
    const MeisoRuntimeRouteKey key = {
        fixture->catalog_generation,
        route->endpoint_id,
        route->endpoint_incarnation,
        route->resource.id,
        route->facet
    };

    return key;
}

static bool route_key_equal(
    MeisoRuntimeRouteKey lhs,
    MeisoRuntimeRouteKey rhs
)
{
    return lhs.catalog_generation == rhs.catalog_generation &&
        lhs.endpoint_incarnation == rhs.endpoint_incarnation &&
        lhs.facet == rhs.facet &&
        meiso_endpoint_id_equal(lhs.endpoint_id, rhs.endpoint_id) &&
        meiso_resource_id_equal(lhs.resource_id, rhs.resource_id);
}

static void initialize_source_route(
    TxSource *source,
    MeisoRuntimeSchedulerWorkKind kind,
    MeisoRuntimeRouteKey route
)
{
    memset(source, 0, sizeof(*source));
    source->work.ticket = 1U;
    source->work.kind = kind;
    switch (kind) {
    case MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER:
        source->work.value.state_offer.route = route;
        break;
    case MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT:
        source->work.value.state_accept.route = route;
        break;
    case MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE:
        source->work.value.stream_sample.route = route;
        break;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_BEGIN:
        source->work.value.blob_begin.route = route;
        break;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK:
        source->work.value.blob_chunk.transfer.route = route;
        break;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_ABORT:
        source->work.value.blob_abort.transfer.route = route;
        break;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT:
        source->work.value.blob_result.transfer.route = route;
        break;
    default:
        break;
    }
}

static bool source_route_index(
    bool host,
    size_t source_index,
    size_t *out_route_index
)
{
    static const uint8_t host_routes[9] = {
        ROUTE_WIFI_DESIRED,
        ROUTE_PRESENTATION_DESIRED,
        ROUTE_SCENE_DESIRED,
        ROUTE_WIFI_ACTUAL,
        ROUTE_PRESENTATION_ACTUAL,
        ROUTE_SCENE_ACTUAL,
        ROUTE_MESH_BLOB,
        ROUTE_MESH_BLOB,
        ROUTE_MESH_BLOB
    };
    static const uint8_t edge_routes[9] = {
        ROUTE_WIFI_DESIRED,
        ROUTE_PRESENTATION_DESIRED,
        ROUTE_SCENE_DESIRED,
        ROUTE_WIFI_ACTUAL,
        ROUTE_PRESENTATION_ACTUAL,
        ROUTE_SCENE_ACTUAL,
        ROUTE_MESH_BLOB,
        ROUTE_WIFI_DATA,
        ROUTE_RENDER_RESULT
    };

    if (out_route_index == NULL || source_index >= 9U) {
        return false;
    }
    *out_route_index = host ? host_routes[source_index] :
        edge_routes[source_index];
    return true;
}

static bool register_peer_sources(
    const TcpFixture *fixture,
    Peer *peer,
    bool host
)
{
    const MeisoRuntimeSchedulerConfig config = {
        peer->runtime,
        peer->session,
        host ? 9U : 8U,
        host ? 1U : 2U
    };
    size_t index;

    peer->scheduler_bytes = meiso_runtime_scheduler_memory_required(&config);
    CHECK(peer->scheduler_bytes != 0U &&
          peer->scheduler_bytes <= sizeof(peer->scheduler_arena.bytes));
    CHECK(meiso_runtime_scheduler_init(
        peer->scheduler_arena.bytes,
        peer->scheduler_bytes,
        &config,
        &peer->scheduler
    ) == MEISO_RUNTIME_SCHEDULER_INIT_OK);
    for (index = 0U; index < peer->source_count; ++index) {
        MeisoRuntimeSchedulerSourceConfig source_config = {
            peer->sources[index].work.kind,
            source_poll,
            source_complete,
            source_boundary,
            &peer->sources[index]
        };

        CHECK(meiso_runtime_scheduler_register_source(
            peer->scheduler,
            &source_config,
            &peer->sources[index].id
        ) == MEISO_RUNTIME_SCHEDULER_OK);
    }
    CHECK(meiso_runtime_scheduler_seal(peer->scheduler) ==
          MEISO_RUNTIME_SCHEDULER_REBIND_REQUIRED);
    for (index = 0U; index < peer->source_count; ++index) {
        size_t route_index;

        if (!source_route_index(host, index, &route_index)) {
            continue;
        }
        CHECK(meiso_runtime_scheduler_bind_source_route(
            peer->scheduler,
            peer->sources[index].id,
            route_key(fixture, route_index)
        ) == MEISO_RUNTIME_SCHEDULER_OK);
    }
    CHECK(meiso_runtime_scheduler_finish_rebind(peer->scheduler) ==
          MEISO_RUNTIME_SCHEDULER_OK);
    return true;
}

static bool initialize_sources(TcpFixture *fixture)
{
    fixture->host.sources = fixture->host_sources;
    fixture->host.source_count = TEST_HOST_SOURCE_COUNT;
    fixture->edge.sources = fixture->edge_sources;
    fixture->edge.source_count = TEST_EDGE_SOURCE_COUNT;

    initialize_source_route(
        &fixture->host_sources[HOST_WIFI_DESIRED],
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
        route_key(fixture, ROUTE_WIFI_DESIRED)
    );
    initialize_source_route(
        &fixture->host_sources[HOST_PLAN_DESIRED],
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
        route_key(fixture, ROUTE_PRESENTATION_DESIRED)
    );
    initialize_source_route(
        &fixture->host_sources[HOST_SCENE_DESIRED],
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
        route_key(fixture, ROUTE_SCENE_DESIRED)
    );
    initialize_source_route(
        &fixture->host_sources[HOST_WIFI_ACTUAL_ACCEPT],
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        route_key(fixture, ROUTE_WIFI_ACTUAL)
    );
    initialize_source_route(
        &fixture->host_sources[HOST_PRESENTATION_ACTUAL_ACCEPT],
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        route_key(fixture, ROUTE_PRESENTATION_ACTUAL)
    );
    initialize_source_route(
        &fixture->host_sources[HOST_SCENE_ACTUAL_ACCEPT],
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        route_key(fixture, ROUTE_SCENE_ACTUAL)
    );
    initialize_source_route(
        &fixture->host_sources[HOST_BLOB_BEGIN],
        MEISO_RUNTIME_SCHEDULER_WORK_BLOB_BEGIN,
        route_key(fixture, ROUTE_MESH_BLOB)
    );
    initialize_source_route(
        &fixture->host_sources[HOST_BLOB_CHUNK],
        MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK,
        route_key(fixture, ROUTE_MESH_BLOB)
    );
    initialize_source_route(
        &fixture->host_sources[HOST_BLOB_ABORT],
        MEISO_RUNTIME_SCHEDULER_WORK_BLOB_ABORT,
        route_key(fixture, ROUTE_MESH_BLOB)
    );
    initialize_source_route(
        &fixture->host_sources[HOST_CATALOG_ACCEPT],
        MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_ACCEPT,
        (MeisoRuntimeRouteKey){0}
    );

    initialize_source_route(
        &fixture->edge_sources[EDGE_WIFI_DESIRED_ACCEPT],
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        route_key(fixture, ROUTE_WIFI_DESIRED)
    );
    initialize_source_route(
        &fixture->edge_sources[EDGE_PLAN_DESIRED_ACCEPT],
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        route_key(fixture, ROUTE_PRESENTATION_DESIRED)
    );
    initialize_source_route(
        &fixture->edge_sources[EDGE_SCENE_DESIRED_ACCEPT],
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        route_key(fixture, ROUTE_SCENE_DESIRED)
    );
    initialize_source_route(
        &fixture->edge_sources[EDGE_WIFI_ACTUAL],
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
        route_key(fixture, ROUTE_WIFI_ACTUAL)
    );
    initialize_source_route(
        &fixture->edge_sources[EDGE_PRESENTATION_ACTUAL],
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
        route_key(fixture, ROUTE_PRESENTATION_ACTUAL)
    );
    initialize_source_route(
        &fixture->edge_sources[EDGE_SCENE_ACTUAL],
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
        route_key(fixture, ROUTE_SCENE_ACTUAL)
    );
    initialize_source_route(
        &fixture->edge_sources[EDGE_BLOB_RESULT],
        MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT,
        route_key(fixture, ROUTE_MESH_BLOB)
    );
    initialize_source_route(
        &fixture->edge_sources[EDGE_WIFI_STREAM],
        MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
        route_key(fixture, ROUTE_WIFI_DATA)
    );
    initialize_source_route(
        &fixture->edge_sources[EDGE_RENDER_STREAM],
        MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
        route_key(fixture, ROUTE_RENDER_RESULT)
    );
    initialize_source_route(
        &fixture->edge_sources[EDGE_CATALOG_OFFER],
        MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER,
        (MeisoRuntimeRouteKey){0}
    );
    CHECK(register_peer_sources(fixture, &fixture->host, true));
    CHECK(register_peer_sources(fixture, &fixture->edge, false));
    return true;
}

static bool build_catalog(TcpFixture *fixture)
{
    const MeisoCompositionSessionV2CatalogConfig config = {
        UINT64_C(0x5501),
        UINT64_C(0x5601),
        UINT64_C(0x5701),
        {
            MEISO_WIFI_TELEMETRY_FIELD_ALL,
            MEISO_WIFI_TELEMETRY_MIN_PERIOD_MS,
            MEISO_WIFI_TELEMETRY_MAX_PERIOD_MS,
            MEISO_WIFI_TELEMETRY_PERIOD_STEP_MS
        }
    };

    CHECK(meiso_composition_session_v2_build_catalog(
        &config,
        (MeisoMutableBuffer){
            fixture->wifi_capability, sizeof(fixture->wifi_capability)
        },
        fixture->endpoints,
        MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT,
        fixture->routes,
        MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT
    ) == MEISO_COMPOSITION_SESSION_V2_OK);
    return true;
}

static bool activate_catalog(TcpFixture *fixture)
{
    MeisoSessionRuntimeMessage message;
    MeisoRuntimeEvent event;

    CHECK(meiso_runtime_offer_catalog(
        fixture->edge.runtime,
        1U,
        fixture->endpoints,
        MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT,
        fixture->routes,
        MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT
    ) == MEISO_RUNTIME_OK);
    CHECK(write_frame(&fixture->edge));
    CHECK(read_runtime(&fixture->host, &message));
    CHECK(message.payload.size ==
          MEISO_COMPOSITION_SESSION_V2_CATALOG_WIRE_SIZE);
    CHECK(meiso_runtime_receive(
        fixture->host.runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED &&
          event.catalog_generation == 1U);
    CHECK(meiso_runtime_accept_catalog(fixture->host.runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(write_frame(&fixture->host));
    CHECK(read_runtime(&fixture->edge, &message));
    CHECK(meiso_runtime_receive(
        fixture->edge.runtime, &message, &event
    ) == MEISO_RUNTIME_OK);
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED &&
          event.catalog_generation == 1U);
    fixture->catalog_generation = 1U;
    return true;
}

static bool initialize_tcp_fixture(
    TcpFixture *fixture,
    int socket_buffer_bytes,
    uint64_t nonce_salt
)
{
    MeisoSessionConfig host_config = base_session_config(
        MEISO_SESSION_ROLE_HOST, nonce_salt
    );
    MeisoSessionConfig edge_config = base_session_config(
        MEISO_SESSION_ROLE_EDGE, nonce_salt
    );

    memset(fixture, 0, sizeof(*fixture));
    fixture->host.fd = -1;
    fixture->edge.fd = -1;
    CHECK(meiso_composition_session_v2_configure_session(&host_config) ==
          MEISO_COMPOSITION_SESSION_V2_OK);
    CHECK(meiso_composition_session_v2_configure_session(&edge_config) ==
          MEISO_COMPOSITION_SESSION_V2_OK);
    CHECK(make_tcp_pair(
        socket_buffer_bytes, &fixture->host.fd, &fixture->edge.fd
    ));
    CHECK(initialize_session(&fixture->host, &host_config));
    CHECK(initialize_session(&fixture->edge, &edge_config));
    CHECK(initialize_adapter(&fixture->host));
    CHECK(initialize_adapter(&fixture->edge));
    CHECK(meiso_session_start(fixture->edge.session) ==
          MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(fixture->host.session) ==
          MEISO_SESSION_START_STARTED);
    CHECK(drive_ready(fixture));
    CHECK(initialize_runtime(&fixture->host));
    CHECK(initialize_runtime(&fixture->edge));
    CHECK(build_catalog(fixture));
    CHECK(activate_catalog(fixture));
    CHECK(initialize_sources(fixture));
    return true;
}

static void destroy_tcp_fixture(TcpFixture *fixture)
{
    close_fd(&fixture->host.fd);
    close_fd(&fixture->edge.fd);
}

static bool dispatch_source(
    Peer *sender,
    Peer *receiver,
    TxSource *source,
    MeisoRuntimeSchedulerStepResult *out_step,
    MeisoRuntimeEvent *out_event
)
{
    MeisoRuntimeSchedulerStepResult step;
    MeisoSessionRuntimeMessage message;
    const uint64_t completions_before = source->completions;

    source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    CHECK(meiso_runtime_scheduler_step(sender->scheduler, &step) ==
          MEISO_RUNTIME_SCHEDULER_STEP_SENT);
    CHECK(step.completion.source_id == source->id &&
          step.completion.code ==
              MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT &&
          source->completions == completions_before + 1U &&
          source->poll_status == MEISO_RUNTIME_SCHEDULER_POLL_EMPTY);
    CHECK(write_frame(sender));
    CHECK(read_runtime(receiver, &message));
    CHECK(meiso_runtime_scheduler_receive(
        receiver->scheduler, &message, out_event
    ) == MEISO_RUNTIME_OK);
    if (out_step != NULL) {
        *out_step = step;
    }
    return true;
}

static bool initialize_state_channel(
    StateChannel *channel,
    const MeisoResourceDescriptor *descriptor,
    uint64_t delivery,
    MeisoOwnerScope replica_owner
)
{
    const MeisoStateConfig config = {MEISO_STATE_DELIVERY_CONVERGENT};
    size_t required;

    memset(channel, 0, sizeof(*channel));
    channel->delivery = (MeisoDeliveryGeneration){delivery};
    required = meiso_state_publisher_memory_required(descriptor, &config);
    CHECK(required != 0U);
    channel->publisher_arena = calloc(1U, required);
    CHECK(channel->publisher_arena != NULL);
    CHECK(meiso_state_publisher_init(
        channel->publisher_arena,
        required,
        descriptor,
        &config,
        &channel->publisher
    ) == MEISO_STATE_INIT_OK);
    required = meiso_state_replica_memory_required(descriptor);
    CHECK(required != 0U);
    channel->replica_arena = calloc(1U, required);
    CHECK(channel->replica_arena != NULL);
    CHECK(meiso_state_replica_init(
        channel->replica_arena,
        required,
        descriptor,
        &channel->replica
    ) == MEISO_STATE_INIT_OK);
    CHECK(meiso_state_publisher_begin_delivery(
        channel->publisher, channel->delivery
    ) == MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_replica_begin_owner(
        channel->replica, replica_owner
    ) == MEISO_STATE_OWNER_STARTED);
    return true;
}

static void destroy_state_channel(StateChannel *channel)
{
    free(channel->replica_arena);
    free(channel->publisher_arena);
    memset(channel, 0, sizeof(*channel));
}

static bool initialize_stream(
    void **out_arena,
    MeisoStream **out_stream,
    const MeisoResourceDescriptor *descriptor,
    uint64_t epoch
)
{
    const MeisoStreamConfig config = {
        MEISO_STREAM_ORDER_STRICT_MONOTONIC,
        MEISO_STREAM_OVERFLOW_REJECT_NEWEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        2U,
        0U,
        0U
    };
    const size_t required = meiso_stream_memory_required(
        descriptor, &config
    );

    CHECK(required != 0U);
    *out_arena = calloc(1U, required);
    CHECK(*out_arena != NULL);
    CHECK(meiso_stream_init(
        *out_arena, required, descriptor, &config, out_stream
    ) == MEISO_STREAM_INIT_OK);
    CHECK(meiso_stream_begin_producer(*out_stream, EDGE_OWNER, epoch) ==
          MEISO_STREAM_PRODUCER_STARTED);
    return true;
}

static bool initialize_semantic_fixture(
    SemanticFixture *fixture,
    int socket_buffer_bytes,
    uint64_t nonce_salt
)
{
    const MeisoResourceDescriptor *wifi_desired =
        meiso_wifi_telemetry_resource_for_facet(
            MEISO_RUNTIME_FACET_DESIRED
        );
    const MeisoResourceDescriptor *wifi_actual =
        meiso_wifi_telemetry_resource_for_facet(
            MEISO_RUNTIME_FACET_ACTUAL
        );
    const MeisoResourceDescriptor *wifi_data =
        meiso_wifi_telemetry_resource_for_facet(
            MEISO_RUNTIME_FACET_DATA
        );
    const MeisoResourceDescriptor *plan_desired =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_PRESENTATION_DESIRED
        );
    const MeisoResourceDescriptor *scene_desired =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_SCENE_DESIRED
        );
    const MeisoResourceDescriptor *scene_actual =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_SCENE_ACTUAL
        );
    const MeisoResourceDescriptor *render =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_RENDER_RESULT
        );
    const MeisoResourceDescriptor *mesh =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
        );
    const MeisoBlobConfig blob_config = {1U};
    size_t required;

    memset(fixture, 0, sizeof(*fixture));
    fixture->tcp.host.fd = -1;
    fixture->tcp.edge.fd = -1;
    CHECK(wifi_desired != NULL && wifi_actual != NULL &&
          wifi_data != NULL && plan_desired != NULL &&
          scene_desired != NULL && scene_actual != NULL &&
          render != NULL && mesh != NULL);
    CHECK(initialize_tcp_fixture(
        &fixture->tcp, socket_buffer_bytes, nonce_salt
    ));
    CHECK(initialize_state_channel(
        &fixture->wifi_desired, wifi_desired, 11U, HOST_OWNER
    ));
    CHECK(initialize_state_channel(
        &fixture->plan_desired, plan_desired, 12U, HOST_OWNER
    ));
    CHECK(initialize_state_channel(
        &fixture->scene_desired, scene_desired, 13U, HOST_OWNER
    ));
    CHECK(initialize_state_channel(
        &fixture->wifi_actual, wifi_actual, 14U, EDGE_OWNER
    ));
    CHECK(initialize_state_channel(
        &fixture->scene_actual, scene_actual, 15U, EDGE_OWNER
    ));
    CHECK(initialize_stream(
        &fixture->wifi_stream_arena,
        &fixture->wifi_stream,
        wifi_data,
        301U
    ));
    CHECK(initialize_stream(
        &fixture->render_stream_arena,
        &fixture->render_stream,
        render,
        302U
    ));
    required = meiso_blob_store_memory_required(mesh, &blob_config);
    CHECK(required != 0U);
    fixture->blob_arena = calloc(1U, required);
    CHECK(fixture->blob_arena != NULL);
    CHECK(meiso_blob_store_init(
        fixture->blob_arena,
        required,
        mesh,
        &blob_config,
        &fixture->blob_store
    ) == MEISO_BLOB_INIT_OK);
    return true;
}

static void destroy_semantic_fixture(SemanticFixture *fixture)
{
    free(fixture->blob_arena);
    free(fixture->render_stream_arena);
    free(fixture->wifi_stream_arena);
    destroy_state_channel(&fixture->scene_actual);
    destroy_state_channel(&fixture->wifi_actual);
    destroy_state_channel(&fixture->scene_desired);
    destroy_state_channel(&fixture->plan_desired);
    destroy_state_channel(&fixture->wifi_desired);
    destroy_tcp_fixture(&fixture->tcp);
    memset(fixture, 0, sizeof(*fixture));
}

static bool publish_desired(StateChannel *channel, uint64_t version)
{
    uint8_t storage[MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE];
    const MeisoRuntimeDesiredState desired = {
        true, {NULL, 0U}
    };
    MeisoByteView payload;

    CHECK(meiso_runtime_desired_state_encode(
        &desired,
        (MeisoMutableBuffer){storage, sizeof(storage)},
        &payload
    ));
    CHECK(meiso_state_publisher_publish(
        channel->publisher, HOST_OWNER, version, payload
    ) == MEISO_STATE_PUBLISH_NEW);
    return true;
}

static bool publish_actual(
    StateChannel *channel,
    uint64_t version,
    uint64_t desired_version,
    uint64_t production_epoch
)
{
    uint8_t storage[MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE];
    const MeisoRuntimeDesiredReference desired_ref = {
        true, HOST_OWNER, desired_version
    };
    MeisoRuntimeActualState actual;
    MeisoByteView payload;

    memset(&actual, 0, sizeof(actual));
    actual.observed_desired = desired_ref;
    actual.applied_desired = desired_ref;
    actual.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    actual.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    actual.effective_enabled = true;
    actual.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING;
    actual.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    actual.production_epoch = production_epoch;
    actual.profile = (MeisoByteView){NULL, 0U};
    CHECK(meiso_runtime_actual_state_encode(
        &actual,
        (MeisoMutableBuffer){storage, sizeof(storage)},
        &payload
    ));
    CHECK(meiso_state_publisher_publish(
        channel->publisher, EDGE_OWNER, version, payload
    ) == MEISO_STATE_PUBLISH_NEW);
    return true;
}

static bool deliver_state(
    SemanticFixture *fixture,
    StateChannel *channel,
    bool host_to_edge,
    size_t offer_index,
    size_t accept_index
)
{
    Peer *sender = host_to_edge ? &fixture->tcp.host : &fixture->tcp.edge;
    Peer *receiver = host_to_edge ? &fixture->tcp.edge : &fixture->tcp.host;
    TxSource *offer_source = &sender->sources[offer_index];
    TxSource *accept_source = &receiver->sources[accept_index];
    MeisoStateOffer offer;
    MeisoRuntimeEvent event;
    MeisoRuntimeEvent accepted;
    MeisoStateReceiveStatus receive_status;

    CHECK(meiso_state_publisher_next_offer(channel->publisher, &offer));
    offer_source->work.value.state_offer.offer = offer;
    CHECK(dispatch_source(
        sender, receiver, offer_source, NULL, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER &&
          route_key_equal(
              event.route, offer_source->work.value.state_offer.route
          ));
    receive_status = meiso_state_replica_receive(
        channel->replica,
        event.state_offer.state.owner,
        event.state_offer.state.version,
        event.state_offer.state.payload
    );
    CHECK(receive_status == MEISO_STATE_RECEIVE_NEW ||
          receive_status == MEISO_STATE_RECEIVE_DUPLICATE);
    CHECK(meiso_runtime_scheduler_commit_state_receive(
        receiver->scheduler,
        event.route,
        &event.state_offer,
        receive_status
    ) == MEISO_RUNTIME_OK);
    accept_source->work.value.state_accept.generation =
        event.state_offer.generation;
    accept_source->work.value.state_accept.owner =
        event.state_offer.state.owner;
    accept_source->work.value.state_accept.version =
        event.state_offer.state.version;
    CHECK(dispatch_source(
        receiver, sender, accept_source, NULL, &accepted
    ));
    CHECK(accepted.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT &&
          route_key_equal(accepted.route, event.route));
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        channel->publisher,
        accepted.state_accept_generation,
        accepted.state_accept_owner,
        accepted.state_accept_version
    ) == MEISO_STATE_ACCEPT_CONFIRMED);
    return true;
}

static bool establish_data_gates(SemanticFixture *fixture)
{
    CHECK(publish_desired(&fixture->wifi_desired, 1U));
    CHECK(deliver_state(
        fixture,
        &fixture->wifi_desired,
        true,
        HOST_WIFI_DESIRED,
        EDGE_WIFI_DESIRED_ACCEPT
    ));
    CHECK(publish_actual(&fixture->wifi_actual, 1U, 1U, 77U));
    CHECK(deliver_state(
        fixture,
        &fixture->wifi_actual,
        false,
        EDGE_WIFI_ACTUAL,
        HOST_WIFI_ACTUAL_ACCEPT
    ));
    CHECK(publish_desired(&fixture->scene_desired, 1U));
    CHECK(deliver_state(
        fixture,
        &fixture->scene_desired,
        true,
        HOST_SCENE_DESIRED,
        EDGE_SCENE_DESIRED_ACCEPT
    ));
    CHECK(publish_actual(&fixture->scene_actual, 1U, 1U, 91U));
    CHECK(deliver_state(
        fixture,
        &fixture->scene_actual,
        false,
        EDGE_SCENE_ACTUAL,
        HOST_SCENE_ACTUAL_ACCEPT
    ));
    CHECK(publish_desired(&fixture->plan_desired, 1U));
    CHECK(deliver_state(
        fixture,
        &fixture->plan_desired,
        true,
        HOST_PLAN_DESIRED,
        EDGE_PLAN_DESIRED_ACCEPT
    ));
    return true;
}

static void capture_busy_snapshot(
    const Peer *peer,
    BusySnapshot *out_snapshot
)
{
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    memcpy(&out_snapshot->scheduler, &peer->scheduler_arena,
           sizeof(out_snapshot->scheduler));
    memcpy(&out_snapshot->runtime, &peer->runtime_arena,
           sizeof(out_snapshot->runtime));
    memcpy(&out_snapshot->session, &peer->session_arena,
           sizeof(out_snapshot->session));
    memcpy(&out_snapshot->adapter, &peer->adapter_arena,
           sizeof(out_snapshot->adapter));
    memcpy(out_snapshot->sources, peer->sources,
           peer->source_count * sizeof(peer->sources[0]));
}

static bool busy_snapshot_unchanged(
    const Peer *peer,
    const BusySnapshot *before,
    const BusySnapshot *after
)
{
    return memcmp(&before->scheduler, &after->scheduler,
                  sizeof(before->scheduler)) == 0 &&
        memcmp(&before->runtime, &after->runtime,
               sizeof(before->runtime)) == 0 &&
        memcmp(&before->session, &after->session,
               sizeof(before->session)) == 0 &&
        memcmp(&before->adapter, &after->adapter,
               sizeof(before->adapter)) == 0 &&
        memcmp(before->sources, after->sources,
               peer->source_count * sizeof(peer->sources[0])) == 0 &&
        memcmp(&before->output, &after->output,
               sizeof(before->output)) == 0;
}

static bool assert_tx_busy_zero_diff(Peer *peer, TxSource *next_source)
{
    BusySnapshot before;
    BusySnapshot after;
    MeisoSessionSnapshot session;
    const uint64_t polls_before = next_source->polls;
    const uint64_t completions_before = next_source->completions;
    MeisoRuntimeSchedulerStepResult output;

    next_source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    capture_busy_snapshot(peer, &before);
    memset(&output, 0xa5, sizeof(output));
    before.output = output;
    CHECK(meiso_runtime_scheduler_step(
        peer->scheduler, &output
    ) == MEISO_RUNTIME_SCHEDULER_STEP_TX_BUSY);
    capture_busy_snapshot(peer, &after);
    after.output = output;
    CHECK(meiso_session_snapshot(peer->session, &session) &&
          session.state == MEISO_SESSION_STATE_READY &&
          session.outbound_pending &&
          meiso_posix_tcp_wants_write(peer->adapter));
    CHECK(next_source->polls == polls_before &&
          next_source->completions == completions_before &&
          busy_snapshot_unchanged(peer, &before, &after));
    next_source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_EMPTY;
    return true;
}

static void set_stream_work(
    TxSource *source,
    uint64_t production_epoch,
    uint64_t stream_epoch,
    uint64_t sequence,
    MeisoByteView payload
)
{
    MeisoStreamSample sample;

    memset(&sample, 0, sizeof(sample));
    sample.key = (MeisoStreamKey){EDGE_OWNER, stream_epoch, sequence};
    sample.source_time = (MeisoTimePoint){1U, sequence};
    sample.payload = payload;
    source->work.value.stream_sample.production_epoch = production_epoch;
    source->work.value.stream_sample.sample = sample;
}

static bool queue_source_without_socket(
    Peer *peer,
    TxSource *source,
    MeisoRuntimeSchedulerStepResult *out_step
)
{
    const uint64_t completions_before = source->completions;

    source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    CHECK(meiso_runtime_scheduler_step(peer->scheduler, out_step) ==
          MEISO_RUNTIME_SCHEDULER_STEP_SENT);
    CHECK(out_step->completion.source_id == source->id &&
          out_step->completion.code ==
              MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT &&
          source->completions == completions_before + 1U &&
          source->poll_status == MEISO_RUNTIME_SCHEDULER_POLL_EMPTY);
    return true;
}

static bool accept_stream_event(
    SemanticFixture *fixture,
    const MeisoRuntimeEvent *event,
    size_t route_index,
    uint64_t production_epoch,
    uint64_t stream_epoch,
    uint64_t sequence,
    MeisoByteView payload
)
{
    MeisoStream *stream = route_index == ROUTE_WIFI_DATA ?
        fixture->wifi_stream : fixture->render_stream;
    MeisoStreamSample queued;
    MeisoStreamReceiveResult receive;

    CHECK(event->type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          route_key_equal(
              event->route, route_key(&fixture->tcp, route_index)
          ) && event->production_epoch == production_epoch &&
          event->stream_sample.key.epoch == stream_epoch &&
          event->stream_sample.key.sequence == sequence &&
          event->stream_sample.payload.size == payload.size &&
          memcmp(
              event->stream_sample.payload.data,
              payload.data,
              payload.size
          ) == 0);
    if (route_index == ROUTE_RENDER_RESULT) {
        MeisoCompositionSceneRenderResult decoded;

        CHECK(meiso_composition_scene_render_result_decode(
            event->stream_sample.payload, &decoded
        ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
        CHECK(decoded.applied_scene_owner_incarnation ==
                  HOST_OWNER.incarnation &&
              decoded.applied_scene_state_version == 7U &&
              decoded.active_plan_generation == 3U &&
              decoded.world_generation == 5U &&
              decoded.result_code ==
                  MEISO_COMPOSITION_SCENE_RESULT_COMPLETE &&
              decoded.target_width ==
                  MEISO_COMPOSITION_SCENE_TARGET_WIDTH &&
              decoded.target_height ==
                  MEISO_COMPOSITION_SCENE_TARGET_HEIGHT &&
              decoded.thumbnail_byte_size ==
                  MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE);
    }
    receive = meiso_stream_receive(stream, &event->stream_sample, NULL);
    CHECK(receive.code == MEISO_STREAM_RECEIVE_ACCEPTED &&
          receive.flags == MEISO_STREAM_RECEIVE_FLAG_NONE &&
          receive.missing_before == 0U &&
          meiso_stream_count(stream) == 1U &&
          meiso_stream_peek(stream, &queued) &&
          queued.key.sequence == sequence &&
          queued.payload.size == payload.size &&
          memcmp(queued.payload.data, payload.data, payload.size) == 0 &&
          meiso_stream_pop(stream) && meiso_stream_count(stream) == 0U);
    return true;
}

static bool encode_render_payload(
    uint8_t storage[MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE],
    MeisoByteView *out_payload
)
{
    MeisoCompositionSceneRenderResult value;
    size_t encoded_size = 0U;
    size_t index;

    memset(&value, 0, sizeof(value));
    value.applied_scene_owner_incarnation = HOST_OWNER.incarnation;
    value.applied_scene_state_version = 7U;
    value.active_plan_generation = 3U;
    value.world_generation = 5U;
    value.result_code = MEISO_COMPOSITION_SCENE_RESULT_COMPLETE;
    value.evidence_flags =
        MEISO_COMPOSITION_SCENE_EVIDENCE_OFFSCREEN_COMPLETE |
        MEISO_COMPOSITION_SCENE_EVIDENCE_THUMBNAIL_INCLUDED;
    value.object_count = 1U;
    value.unique_mesh_count = 1U;
    value.source_clock_epoch = 1U;
    value.render_start_monotonic_ns = 100U;
    value.render_end_monotonic_ns = 200U;
    for (index = 0U; index < sizeof(value.full_target_sha256); ++index) {
        value.full_target_sha256[index] = (uint8_t)(index * 37U + 11U);
    }
    value.target_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    value.target_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    value.color_format = MEISO_COMPOSITION_SCENE_COLOR_RGBA8888;
    value.depth_format = MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32;
    value.logical_color_row_bytes =
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH * 4U;
    value.logical_depth_row_bytes =
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH * 4U;
    value.thumbnail_width = MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH;
    value.thumbnail_height = MEISO_COMPOSITION_SCENE_THUMBNAIL_HEIGHT;
    value.thumbnail_format = MEISO_COMPOSITION_SCENE_COLOR_RGB565;
    value.thumbnail_stride_bytes =
        MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH *
        MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTES_PER_PIXEL;
    value.thumbnail_byte_size =
        MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE;
    for (index = 0U; index < sizeof(value.thumbnail_rgb565); ++index) {
        value.thumbnail_rgb565[index] = (uint8_t)(index * 17U + 5U);
    }
    CHECK(meiso_composition_scene_render_result_encode(
        &value,
        (MeisoMutableBuffer){
            storage, MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE
        },
        &encoded_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(encoded_size == MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE);
    *out_payload = (MeisoByteView){storage, encoded_size};
    return true;
}

static bool recover_stream_frames(
    SemanticFixture *fixture,
    uint64_t frame_count,
    MeisoByteView payload
)
{
    Peer *sender = &fixture->tcp.edge;
    Peer *receiver = &fixture->tcp.host;
    struct timespec deadline;
    uint64_t received = 0U;

    CHECK(make_deadline(&deadline));
    while (received < frame_count && remaining_poll_ms(&deadline) > 0) {
        bool advanced = false;

        if (meiso_posix_tcp_wants_write(sender->adapter)) {
            const MeisoPosixTcpWriteStatus status =
                meiso_posix_tcp_on_writable(sender->adapter);

            CHECK(status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
                  status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK ||
                  status == MEISO_POSIX_TCP_WRITE_INTERRUPTED ||
                  status == MEISO_POSIX_TCP_WRITE_FRAME_SENT);
            advanced = status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
                status == MEISO_POSIX_TCP_WRITE_FRAME_SENT;
        }
        if (meiso_posix_tcp_wants_read(receiver->adapter)) {
            MeisoSessionRuntimeMessage message;
            const MeisoPosixTcpReadStatus status =
                meiso_posix_tcp_on_readable(receiver->adapter, &message);

            CHECK(status == MEISO_POSIX_TCP_READ_PROGRESS ||
                  status == MEISO_POSIX_TCP_READ_WOULD_BLOCK ||
                  status == MEISO_POSIX_TCP_READ_INTERRUPTED ||
                  status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS ||
                  status == MEISO_POSIX_TCP_READ_RUNTIME);
            if (status == MEISO_POSIX_TCP_READ_RUNTIME) {
                MeisoRuntimeEvent event;

                received++;
                CHECK(meiso_runtime_scheduler_receive(
                    receiver->scheduler, &message, &event
                ) == MEISO_RUNTIME_OK);
                CHECK(accept_stream_event(
                    fixture,
                    &event,
                    ROUTE_RENDER_RESULT,
                    91U,
                    302U,
                    received,
                    payload
                ));
            }
            advanced = advanced ||
                status == MEISO_POSIX_TCP_READ_PROGRESS ||
                status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS ||
                status == MEISO_POSIX_TCP_READ_RUNTIME;
        }
        if (!advanced) {
            CHECK(wait_pair(&fixture->tcp, &deadline));
        }
    }
    CHECK(received == frame_count &&
          !meiso_posix_tcp_wants_write(sender->adapter));
    {
        MeisoSessionRuntimeMessage extra;

        memset(&extra, 0xa5, sizeof(extra));
        CHECK(meiso_posix_tcp_on_readable(
            receiver->adapter, &extra
        ) == MEISO_POSIX_TCP_READ_WOULD_BLOCK);
        CHECK(meiso_stream_count(fixture->render_stream) == 0U);
    }
    return true;
}

static bool test_real_backpressure_edge_to_host(void)
{
    SemanticFixture fixture;
    TxSource *source;
    TxSource *next_source;
    MeisoSessionSnapshot session;
    MeisoRuntimeSchedulerStepResult step;
    MeisoPosixTcpWriteStatus write_status =
        MEISO_POSIX_TCP_WRITE_PROGRESS;
    MeisoByteView payload;
    uint8_t *storage;
    uint64_t queued = 0U;

    CHECK(initialize_semantic_fixture(
        &fixture, TEST_SOCKET_BUFFER_BYTES, 400U
    ));
    CHECK(establish_data_gates(&fixture));
    storage = (uint8_t *)malloc(
        MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE
    );
    CHECK(storage != NULL);
    CHECK(encode_render_payload(storage, &payload));
    source = &fixture.tcp.edge_sources[EDGE_RENDER_STREAM];
    while (queued < TEST_BACKPRESSURE_FRAME_LIMIT &&
           write_status != MEISO_POSIX_TCP_WRITE_WOULD_BLOCK) {
        set_stream_work(source, 91U, 302U, queued + 1U, payload);
        CHECK(queue_source_without_socket(&fixture.tcp.edge, source, &step));
        queued++;
        do {
            write_status = meiso_posix_tcp_on_writable(
                fixture.tcp.edge.adapter
            );
            CHECK(write_status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
                  write_status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK ||
                  write_status == MEISO_POSIX_TCP_WRITE_INTERRUPTED ||
                  write_status == MEISO_POSIX_TCP_WRITE_FRAME_SENT);
        } while (write_status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
                 write_status == MEISO_POSIX_TCP_WRITE_INTERRUPTED);
    }
    CHECK(queued != 0U &&
          write_status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK &&
          meiso_session_snapshot(fixture.tcp.edge.session, &session) &&
          session.state == MEISO_SESSION_STATE_READY &&
          session.failure == MEISO_SESSION_FAILURE_NONE &&
          session.outbound_pending &&
          meiso_posix_tcp_wants_write(fixture.tcp.edge.adapter));
    next_source = &fixture.tcp.edge_sources[EDGE_RENDER_STREAM];
    set_stream_work(next_source, 91U, 302U, queued + 1U, payload);
    CHECK(assert_tx_busy_zero_diff(&fixture.tcp.edge, next_source));
    CHECK(recover_stream_frames(&fixture, queued, payload));
    CHECK(meiso_session_snapshot(fixture.tcp.edge.session, &session) &&
          session.state == MEISO_SESSION_STATE_READY &&
          !session.outbound_pending);
    free(storage);
    destroy_semantic_fixture(&fixture);
    return true;
}

static MeisoContentId test_blob_content_id(void)
{
    MeisoContentId value;
    size_t index;

    memset(&value, 0, sizeof(value));
    value.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    value.digest_size = MEISO_SHA256_DIGEST_SIZE;
    for (index = 0U; index < MEISO_SHA256_DIGEST_SIZE; ++index) {
        value.digest[index] = (uint8_t)(index * 13U + 7U);
    }
    return value;
}

static bool begin_blob_transfer(
    SemanticFixture *fixture,
    MeisoRuntimeBlobTransferRef *out_transfer
)
{
    TxSource *begin = &fixture->tcp.host_sources[HOST_BLOB_BEGIN];
    TxSource *result = &fixture->tcp.edge_sources[EDGE_BLOB_RESULT];
    MeisoRuntimeSchedulerStepResult step;
    MeisoRuntimeEvent edge_event;
    MeisoRuntimeEvent host_event;

    begin->work.value.blob_begin.content_id = test_blob_content_id();
    begin->work.value.blob_begin.total_size =
        MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE;
    CHECK(dispatch_source(
        &fixture->tcp.host,
        &fixture->tcp.edge,
        begin,
        &step,
        &edge_event
    ));
    CHECK(step.completion.generated_blob_transfer_present &&
          edge_event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          edge_event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN &&
          route_key_equal(
              edge_event.route,
              route_key(&fixture->tcp, ROUTE_MESH_BLOB)
          ));
    *out_transfer = step.completion.generated_blob_transfer;
    CHECK(edge_event.blob_transfer.transfer_id ==
              out_transfer->transfer_id &&
          edge_event.blob_transfer.total_size ==
              MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE);
    result->work.value.blob_result.transfer = edge_event.blob_transfer;
    result->work.value.blob_result.operation = MEISO_RUNTIME_BLOB_BEGIN;
    CHECK(dispatch_source(
        &fixture->tcp.edge,
        &fixture->tcp.host,
        result,
        NULL,
        &host_event
    ));
    CHECK(host_event.type == MEISO_RUNTIME_EVENT_BLOB_RESULT &&
          host_event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN &&
          host_event.blob_result == MEISO_RUNTIME_BLOB_RESULT_READY &&
          host_event.blob_next_offset == 0U);
    return true;
}

static bool recover_blob_chunks(
    SemanticFixture *fixture,
    const MeisoRuntimeBlobTransferRef *transfer,
    uint64_t chunk_count,
    MeisoByteView payload
)
{
    Peer *sender = &fixture->tcp.host;
    Peer *receiver = &fixture->tcp.edge;
    struct timespec deadline;
    uint64_t received = 0U;

    CHECK(make_deadline(&deadline));
    while (received < chunk_count && remaining_poll_ms(&deadline) > 0) {
        bool advanced = false;

        if (meiso_posix_tcp_wants_write(sender->adapter)) {
            const MeisoPosixTcpWriteStatus status =
                meiso_posix_tcp_on_writable(sender->adapter);

            CHECK(status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
                  status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK ||
                  status == MEISO_POSIX_TCP_WRITE_INTERRUPTED ||
                  status == MEISO_POSIX_TCP_WRITE_FRAME_SENT);
            advanced = status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
                status == MEISO_POSIX_TCP_WRITE_FRAME_SENT;
        }
        if (meiso_posix_tcp_wants_read(receiver->adapter)) {
            MeisoSessionRuntimeMessage message;
            const MeisoPosixTcpReadStatus status =
                meiso_posix_tcp_on_readable(receiver->adapter, &message);

            CHECK(status == MEISO_POSIX_TCP_READ_PROGRESS ||
                  status == MEISO_POSIX_TCP_READ_WOULD_BLOCK ||
                  status == MEISO_POSIX_TCP_READ_INTERRUPTED ||
                  status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS ||
                  status == MEISO_POSIX_TCP_READ_RUNTIME);
            if (status == MEISO_POSIX_TCP_READ_RUNTIME) {
                MeisoRuntimeEvent event;
                MeisoRuntimeSchedulerSnapshot scheduler;

                CHECK(meiso_runtime_scheduler_receive(
                    receiver->scheduler, &message, &event
                ) == MEISO_RUNTIME_OK);
                CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
                      event.blob_operation ==
                          MEISO_RUNTIME_BLOB_CHUNK &&
                      route_key_equal(
                          event.route,
                          route_key(&fixture->tcp, ROUTE_MESH_BLOB)
                      ) && event.blob_transfer.transfer_id ==
                          transfer->transfer_id &&
                      event.blob_offset == received * payload.size &&
                      event.blob_payload.size == payload.size &&
                      memcmp(
                          event.blob_payload.data,
                          payload.data,
                          payload.size
                      ) == 0 &&
                      meiso_runtime_scheduler_snapshot(
                          receiver->scheduler, &scheduler
                      ) && scheduler.rx_obligation ==
                          MEISO_RUNTIME_SCHEDULER_RX_NONE);
                received++;
            }
            advanced = advanced ||
                status == MEISO_POSIX_TCP_READ_PROGRESS ||
                status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS ||
                status == MEISO_POSIX_TCP_READ_RUNTIME;
        }
        if (!advanced) {
            CHECK(wait_pair(&fixture->tcp, &deadline));
        }
    }
    CHECK(received == chunk_count &&
          !meiso_posix_tcp_wants_write(sender->adapter));
    {
        MeisoSessionRuntimeMessage extra;

        memset(&extra, 0xa5, sizeof(extra));
        CHECK(meiso_posix_tcp_on_readable(
            receiver->adapter, &extra
        ) == MEISO_POSIX_TCP_READ_WOULD_BLOCK);
        CHECK(meiso_blob_store_count(fixture->blob_store) == 0U);
    }
    return true;
}

static bool abort_blob_transfer(
    SemanticFixture *fixture,
    MeisoRuntimeBlobTransferRef transfer,
    uint64_t expected_next
)
{
    TxSource *abort = &fixture->tcp.host_sources[HOST_BLOB_ABORT];
    TxSource *result = &fixture->tcp.edge_sources[EDGE_BLOB_RESULT];
    MeisoRuntimeEvent edge_event;
    MeisoRuntimeEvent host_event;
    MeisoRuntimeBlobSnapshot host_snapshot;

    abort->work.value.blob_abort.transfer = transfer;
    abort->work.value.blob_abort.reason =
        MEISO_RUNTIME_BLOB_ABORT_CALLER_REQUEST;
    CHECK(dispatch_source(
        &fixture->tcp.host,
        &fixture->tcp.edge,
        abort,
        NULL,
        &edge_event
    ));
    CHECK(edge_event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          edge_event.blob_operation == MEISO_RUNTIME_BLOB_ABORT);
    result->work.value.blob_result.transfer = edge_event.blob_transfer;
    result->work.value.blob_result.operation = MEISO_RUNTIME_BLOB_ABORT;
    CHECK(dispatch_source(
        &fixture->tcp.edge,
        &fixture->tcp.host,
        result,
        NULL,
        &host_event
    ));
    CHECK(host_event.type == MEISO_RUNTIME_EVENT_BLOB_RESULT &&
          host_event.blob_operation == MEISO_RUNTIME_BLOB_ABORT &&
          host_event.blob_result == MEISO_RUNTIME_BLOB_RESULT_ABORTED &&
          host_event.blob_next_offset == expected_next &&
          meiso_runtime_blob_snapshot(
              fixture->tcp.host.runtime, &host_snapshot
          ) && !host_snapshot.transfer_active &&
          host_snapshot.transfer_id_high_water == transfer.transfer_id);
    return true;
}

static bool test_real_backpressure_host_to_edge(void)
{
    SemanticFixture fixture;
    TxSource *chunk;
    TxSource *abort;
    MeisoRuntimeBlobTransferRef transfer;
    MeisoRuntimeSchedulerStepResult step;
    MeisoSessionSnapshot session;
    MeisoBlobStats before;
    MeisoBlobStats after;
    MeisoPosixTcpWriteStatus write_status =
        MEISO_POSIX_TCP_WRITE_PROGRESS;
    MeisoByteView payload;
    uint8_t storage[MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE];
    uint64_t queued = 0U;
    size_t index;

    CHECK(initialize_semantic_fixture(
        &fixture, TEST_SOCKET_BUFFER_BYTES, 500U
    ));
    CHECK(meiso_blob_store_count(fixture.blob_store) == 0U &&
          meiso_blob_store_stats(fixture.blob_store, &before));
    CHECK(begin_blob_transfer(&fixture, &transfer));
    for (index = 0U; index < sizeof(storage); ++index) {
        storage[index] = (uint8_t)(index * 29U + 3U);
    }
    payload = (MeisoByteView){storage, sizeof(storage)};
    chunk = &fixture.tcp.host_sources[HOST_BLOB_CHUNK];
    while (queued <
               MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE / payload.size &&
           write_status != MEISO_POSIX_TCP_WRITE_WOULD_BLOCK) {
        chunk->work.value.blob_chunk.transfer = transfer;
        chunk->work.value.blob_chunk.offset = queued * payload.size;
        chunk->work.value.blob_chunk.payload = payload;
        CHECK(queue_source_without_socket(&fixture.tcp.host, chunk, &step));
        queued++;
        do {
            write_status = meiso_posix_tcp_on_writable(
                fixture.tcp.host.adapter
            );
            CHECK(write_status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
                  write_status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK ||
                  write_status == MEISO_POSIX_TCP_WRITE_INTERRUPTED ||
                  write_status == MEISO_POSIX_TCP_WRITE_FRAME_SENT);
        } while (write_status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
                 write_status == MEISO_POSIX_TCP_WRITE_INTERRUPTED);
    }
    CHECK(queued != 0U &&
          write_status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK &&
          meiso_session_snapshot(fixture.tcp.host.session, &session) &&
          session.state == MEISO_SESSION_STATE_READY &&
          session.outbound_pending &&
          meiso_posix_tcp_wants_write(fixture.tcp.host.adapter));
    abort = &fixture.tcp.host_sources[HOST_BLOB_ABORT];
    abort->work.value.blob_abort.transfer = transfer;
    abort->work.value.blob_abort.reason =
        MEISO_RUNTIME_BLOB_ABORT_CALLER_REQUEST;
    CHECK(assert_tx_busy_zero_diff(&fixture.tcp.host, abort));
    CHECK(recover_blob_chunks(&fixture, &transfer, queued, payload));
    CHECK(abort_blob_transfer(
        &fixture, transfer, queued * payload.size
    ));
    CHECK(meiso_blob_store_count(fixture.blob_store) == 0U &&
          meiso_blob_store_stats(fixture.blob_store, &after) &&
          memcmp(&before, &after, sizeof(before)) == 0);
    destroy_semantic_fixture(&fixture);
    return true;
}

static bool restart_state_delivery(StateChannel *channel, uint64_t increment)
{
    MeisoDeliveryGeneration next;

    CHECK(increment != 0U &&
          channel->delivery.value <= UINT64_MAX - increment);
    CHECK(meiso_state_publisher_end_delivery(
        channel->publisher, channel->delivery
    ) == MEISO_STATE_DELIVERY_ENDED);
    next = (MeisoDeliveryGeneration){channel->delivery.value + increment};
    CHECK(meiso_state_publisher_begin_delivery(
        channel->publisher, next
    ) == MEISO_STATE_DELIVERY_STARTED);
    channel->delivery = next;
    return true;
}

static bool prepare_state_offer(
    StateChannel *channel,
    TxSource *source
)
{
    MeisoStateOffer offer;

    CHECK(meiso_state_publisher_next_offer(channel->publisher, &offer));
    source->work.value.state_offer.offer = offer;
    source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    return true;
}

static bool send_scheduler_frame(
    Peer *sender,
    Peer *receiver,
    MeisoRuntimeSchedulerStepResult *out_step,
    MeisoRuntimeEvent *out_event
)
{
    MeisoSessionRuntimeMessage message;

    CHECK(meiso_runtime_scheduler_step(sender->scheduler, out_step) ==
          MEISO_RUNTIME_SCHEDULER_STEP_SENT);
    CHECK(out_step->completion.code ==
          MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT);
    CHECK(write_frame(sender));
    CHECK(read_runtime(receiver, &message));
    CHECK(meiso_runtime_scheduler_receive(
        receiver->scheduler, &message, out_event
    ) == MEISO_RUNTIME_OK);
    return true;
}

static bool commit_state_offer_prepare_accept(
    StateChannel *channel,
    Peer *receiver,
    TxSource *accept_source,
    const MeisoRuntimeEvent *event
)
{
    MeisoStateReceiveStatus status;

    CHECK(event->type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    status = meiso_state_replica_receive(
        channel->replica,
        event->state_offer.state.owner,
        event->state_offer.state.version,
        event->state_offer.state.payload
    );
    CHECK(status == MEISO_STATE_RECEIVE_DUPLICATE);
    CHECK(meiso_runtime_scheduler_commit_state_receive(
        receiver->scheduler,
        event->route,
        &event->state_offer,
        status
    ) == MEISO_RUNTIME_OK);
    accept_source->work.value.state_accept.generation =
        event->state_offer.generation;
    accept_source->work.value.state_accept.owner =
        event->state_offer.state.owner;
    accept_source->work.value.state_accept.version =
        event->state_offer.state.version;
    accept_source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    return true;
}

static bool accept_state_delivery(
    StateChannel *channel,
    const MeisoRuntimeEvent *event
)
{
    CHECK(event->type == MEISO_RUNTIME_EVENT_STATE_ACCEPT);
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        channel->publisher,
        event->state_accept_generation,
        event->state_accept_owner,
        event->state_accept_version
    ) == MEISO_STATE_ACCEPT_CONFIRMED);
    return true;
}

static bool encode_wifi_payload(
    uint8_t storage[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE],
    MeisoByteView *out_payload
)
{
    MeisoWifiTelemetrySample value;
    size_t encoded_size = 0U;

    memset(&value, 0, sizeof(value));
    value.selected_fields =
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
        MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM;
    value.present_fields = value.selected_fields;
    value.link_state = true;
    value.signal_mbm = -4700;
    CHECK(meiso_wifi_telemetry_sample_encode(
        &value,
        (MeisoMutableBuffer){
            storage, MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE
        },
        &encoded_size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    *out_payload = (MeisoByteView){storage, encoded_size};
    return true;
}

static bool dispatch_stream_sample(
    SemanticFixture *fixture,
    size_t source_index,
    size_t route_index,
    uint64_t production_epoch,
    uint64_t stream_epoch,
    uint64_t sequence,
    MeisoByteView payload
)
{
    TxSource *source = &fixture->tcp.edge_sources[source_index];
    MeisoRuntimeEvent event;

    set_stream_work(
        source,
        production_epoch,
        stream_epoch,
        sequence,
        payload
    );
    CHECK(dispatch_source(
        &fixture->tcp.edge,
        &fixture->tcp.host,
        source,
        NULL,
        &event
    ));
    CHECK(accept_stream_event(
        fixture,
        &event,
        route_index,
        production_epoch,
        stream_epoch,
        sequence,
        payload
    ));
    return true;
}

static bool prepare_duplicate_actual_for_host_accept(
    SemanticFixture *fixture
)
{
    TxSource *offer = &fixture->tcp.edge_sources[EDGE_WIFI_ACTUAL];
    TxSource *accept =
        &fixture->tcp.host_sources[HOST_WIFI_ACTUAL_ACCEPT];
    MeisoRuntimeEvent event;

    CHECK(restart_state_delivery(&fixture->wifi_actual, 100U));
    CHECK(prepare_state_offer(&fixture->wifi_actual, offer));
    CHECK(dispatch_source(
        &fixture->tcp.edge,
        &fixture->tcp.host,
        offer,
        NULL,
        &event
    ));
    CHECK(commit_state_offer_prepare_accept(
        &fixture->wifi_actual,
        &fixture->tcp.host,
        accept,
        &event
    ));
    return true;
}

static bool prepare_host_fairness_controls(SemanticFixture *fixture)
{
    CHECK(restart_state_delivery(&fixture->wifi_desired, 100U));
    CHECK(restart_state_delivery(&fixture->plan_desired, 100U));
    CHECK(restart_state_delivery(&fixture->scene_desired, 100U));
    CHECK(prepare_state_offer(
        &fixture->wifi_desired,
        &fixture->tcp.host_sources[HOST_WIFI_DESIRED]
    ));
    CHECK(prepare_state_offer(
        &fixture->plan_desired,
        &fixture->tcp.host_sources[HOST_PLAN_DESIRED]
    ));
    CHECK(prepare_state_offer(
        &fixture->scene_desired,
        &fixture->tcp.host_sources[HOST_SCENE_DESIRED]
    ));
    return true;
}

static bool handle_host_fairness_event(
    SemanticFixture *fixture,
    const MeisoRuntimeSchedulerStepResult *step,
    const MeisoRuntimeEvent *event,
    MeisoRuntimeBlobTransferRef transfer,
    MeisoByteView blob_payload,
    bool *out_data_sent
)
{
    if (step->completion.kind ==
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER) {
        if (route_key_equal(
                event->route,
                route_key(&fixture->tcp, ROUTE_WIFI_DESIRED))) {
            CHECK(commit_state_offer_prepare_accept(
                &fixture->wifi_desired,
                &fixture->tcp.edge,
                &fixture->tcp.edge_sources[EDGE_WIFI_DESIRED_ACCEPT],
                event
            ));
        } else if (route_key_equal(
                       event->route,
                       route_key(
                           &fixture->tcp, ROUTE_PRESENTATION_DESIRED))) {
            CHECK(commit_state_offer_prepare_accept(
                &fixture->plan_desired,
                &fixture->tcp.edge,
                &fixture->tcp.edge_sources[EDGE_PLAN_DESIRED_ACCEPT],
                event
            ));
        } else {
            CHECK(route_key_equal(
                event->route,
                route_key(&fixture->tcp, ROUTE_SCENE_DESIRED)
            ));
            CHECK(commit_state_offer_prepare_accept(
                &fixture->scene_desired,
                &fixture->tcp.edge,
                &fixture->tcp.edge_sources[EDGE_SCENE_DESIRED_ACCEPT],
                event
            ));
        }
    } else if (step->completion.kind ==
               MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT) {
        CHECK(accept_state_delivery(&fixture->wifi_actual, event));
    } else {
        CHECK(step->completion.kind ==
                  MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK &&
              event->type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
              event->blob_operation == MEISO_RUNTIME_BLOB_CHUNK &&
              event->blob_transfer.transfer_id == transfer.transfer_id &&
              event->blob_offset == blob_payload.size &&
              event->blob_payload.size == blob_payload.size &&
              memcmp(
                  event->blob_payload.data,
                  blob_payload.data,
                  blob_payload.size
              ) == 0);
        *out_data_sent = true;
    }
    return true;
}

static bool run_host_fairness(
    SemanticFixture *fixture,
    MeisoRuntimeBlobTransferRef transfer,
    MeisoByteView blob_payload
)
{
    MeisoRuntimeSchedulerSnapshot host_before;
    MeisoRuntimeSchedulerSnapshot edge_before;
    MeisoRuntimeSchedulerSnapshot host_after;
    MeisoRuntimeSchedulerSnapshot edge_after;
    TxSource *chunk = &fixture->tcp.host_sources[HOST_BLOB_CHUNK];
    bool data_sent = false;
    uint64_t local_slots = 0U;
    uint64_t data_round = 0U;

    CHECK(prepare_host_fairness_controls(fixture));
    chunk->work.value.blob_chunk.transfer = transfer;
    chunk->work.value.blob_chunk.offset = blob_payload.size;
    chunk->work.value.blob_chunk.payload = blob_payload;
    chunk->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    CHECK(meiso_runtime_scheduler_snapshot(
              fixture->tcp.host.scheduler, &host_before
          ) && meiso_runtime_scheduler_snapshot(
              fixture->tcp.edge.scheduler, &edge_before
          ) && host_before.successful_control_burst == 0U);
    while (!data_sent && local_slots < 5U) {
        MeisoRuntimeSchedulerStepResult step;
        MeisoRuntimeEvent event;

        CHECK(send_scheduler_frame(
            &fixture->tcp.host,
            &fixture->tcp.edge,
            &step,
            &event
        ));
        local_slots++;
        CHECK(step.successful_slot ==
              host_before.successful_slots + local_slots);
        CHECK(handle_host_fairness_event(
            fixture,
            &step,
            &event,
            transfer,
            blob_payload,
            &data_sent
        ));
        if (data_sent) {
            data_round = step.data_round;
        }
        CHECK(meiso_runtime_scheduler_snapshot(
                  fixture->tcp.edge.scheduler, &edge_after
              ) && edge_after.successful_slots ==
                  edge_before.successful_slots);
    }
    CHECK(data_sent && local_slots == 5U &&
          data_round != 0U &&
          meiso_runtime_scheduler_snapshot(
              fixture->tcp.host.scheduler, &host_after
          ) && host_after.successful_slots ==
              host_before.successful_slots + 5U &&
          host_after.successful_data_slots ==
              host_before.successful_data_slots + 1U &&
          host_after.data_rounds == host_before.data_rounds + 1U &&
          host_after.data_round_member_count == 0U &&
          host_after.successful_control_burst == 0U &&
          host_after.phase == MEISO_RUNTIME_SCHEDULER_PHASE_READY);
    return true;
}

static bool prepare_edge_fourth_control(SemanticFixture *fixture)
{
    CHECK(restart_state_delivery(&fixture->scene_actual, 100U));
    CHECK(prepare_state_offer(
        &fixture->scene_actual,
        &fixture->tcp.edge_sources[EDGE_SCENE_ACTUAL]
    ));
    return true;
}

static bool handle_edge_fairness_event(
    SemanticFixture *fixture,
    const MeisoRuntimeSchedulerStepResult *step,
    const MeisoRuntimeEvent *event,
    MeisoByteView wifi_payload,
    MeisoByteView render_payload,
    bool *out_wifi_sent,
    bool *out_render_sent
)
{
    if (step->completion.kind ==
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT) {
        if (route_key_equal(
                event->route,
                route_key(&fixture->tcp, ROUTE_WIFI_DESIRED))) {
            CHECK(accept_state_delivery(&fixture->wifi_desired, event));
        } else if (route_key_equal(
                       event->route,
                       route_key(
                           &fixture->tcp, ROUTE_PRESENTATION_DESIRED))) {
            CHECK(accept_state_delivery(&fixture->plan_desired, event));
        } else {
            CHECK(route_key_equal(
                event->route,
                route_key(&fixture->tcp, ROUTE_SCENE_DESIRED)
            ));
            CHECK(accept_state_delivery(&fixture->scene_desired, event));
        }
    } else if (step->completion.kind ==
               MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER) {
        CHECK(route_key_equal(
            event->route, route_key(&fixture->tcp, ROUTE_SCENE_ACTUAL)
        ));
        CHECK(commit_state_offer_prepare_accept(
            &fixture->scene_actual,
            &fixture->tcp.host,
            &fixture->tcp.host_sources[HOST_SCENE_ACTUAL_ACCEPT],
            event
        ));
    } else {
        CHECK(step->completion.kind ==
              MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE);
        if (route_key_equal(
                event->route,
                route_key(&fixture->tcp, ROUTE_WIFI_DATA))) {
            CHECK(accept_stream_event(
                fixture,
                event,
                ROUTE_WIFI_DATA,
                77U,
                301U,
                2U,
                wifi_payload
            ));
            *out_wifi_sent = true;
        } else {
            CHECK(accept_stream_event(
                fixture,
                event,
                ROUTE_RENDER_RESULT,
                91U,
                302U,
                2U,
                render_payload
            ));
            *out_render_sent = true;
        }
    }
    return true;
}

static bool run_edge_fairness(
    SemanticFixture *fixture,
    MeisoByteView wifi_payload,
    MeisoByteView render_payload
)
{
    MeisoRuntimeSchedulerSnapshot host_before;
    MeisoRuntimeSchedulerSnapshot host_after;
    MeisoRuntimeSchedulerSnapshot edge_before;
    MeisoRuntimeSchedulerSnapshot edge_after;
    bool wifi_sent = false;
    bool render_sent = false;
    uint64_t local_slots = 0U;
    uint64_t data_round = 0U;

    CHECK(prepare_edge_fourth_control(fixture));
    set_stream_work(
        &fixture->tcp.edge_sources[EDGE_WIFI_STREAM],
        77U,
        301U,
        2U,
        wifi_payload
    );
    fixture->tcp.edge_sources[EDGE_WIFI_STREAM].poll_status =
        MEISO_RUNTIME_SCHEDULER_POLL_READY;
    set_stream_work(
        &fixture->tcp.edge_sources[EDGE_RENDER_STREAM],
        91U,
        302U,
        2U,
        render_payload
    );
    fixture->tcp.edge_sources[EDGE_RENDER_STREAM].poll_status =
        MEISO_RUNTIME_SCHEDULER_POLL_READY;
    CHECK(meiso_runtime_scheduler_snapshot(
              fixture->tcp.edge.scheduler, &edge_before
          ) && meiso_runtime_scheduler_snapshot(
              fixture->tcp.host.scheduler, &host_before
          ) && edge_before.successful_control_burst == 0U);
    while ((!wifi_sent || !render_sent) && local_slots < 6U) {
        MeisoRuntimeSchedulerStepResult step;
        MeisoRuntimeEvent event;

        CHECK(send_scheduler_frame(
            &fixture->tcp.edge,
            &fixture->tcp.host,
            &step,
            &event
        ));
        local_slots++;
        CHECK(step.successful_slot ==
              edge_before.successful_slots + local_slots);
        CHECK(handle_edge_fairness_event(
            fixture,
            &step,
            &event,
            wifi_payload,
            render_payload,
            &wifi_sent,
            &render_sent
        ));
        if (step.completion.kind ==
            MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE) {
            CHECK(step.data_round != 0U);
            if (data_round == 0U) {
                data_round = step.data_round;
            } else {
                CHECK(step.data_round == data_round);
            }
        }
        CHECK(meiso_runtime_scheduler_snapshot(
                  fixture->tcp.host.scheduler, &host_after
              ) && host_after.successful_slots ==
                  host_before.successful_slots);
    }
    CHECK(wifi_sent && render_sent && local_slots == 6U &&
          data_round != 0U &&
          meiso_runtime_scheduler_snapshot(
              fixture->tcp.edge.scheduler, &edge_after
          ) && edge_after.successful_slots ==
              edge_before.successful_slots + 6U &&
          edge_after.successful_data_slots ==
              edge_before.successful_data_slots + 2U &&
          edge_after.data_rounds == edge_before.data_rounds + 1U &&
          edge_after.data_round_member_count == 0U &&
          edge_after.successful_control_burst == 0U &&
          edge_after.phase == MEISO_RUNTIME_SCHEDULER_PHASE_READY);
    return true;
}

static bool test_mixed_full_duplex_direction_local_fairness(void)
{
    SemanticFixture fixture;
    MeisoRuntimeBlobTransferRef transfer;
    MeisoRuntimeEvent baseline_chunk;
    MeisoRuntimeEvent accepted;
    MeisoByteView blob_payload;
    MeisoByteView wifi_payload;
    MeisoByteView render_payload;
    uint8_t blob_storage[MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE];
    uint8_t wifi_storage[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    uint8_t render_storage[
        MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE
    ];
    TxSource *chunk;
    TxSource *scene_accept;
    size_t index;

    CHECK(initialize_semantic_fixture(&fixture, 0, 600U));
    CHECK(establish_data_gates(&fixture));
    CHECK(encode_wifi_payload(wifi_storage, &wifi_payload));
    CHECK(encode_render_payload(render_storage, &render_payload));
    for (index = 0U; index < sizeof(blob_storage); ++index) {
        blob_storage[index] = (uint8_t)(index * 29U + 3U);
    }
    blob_payload = (MeisoByteView){blob_storage, sizeof(blob_storage)};
    CHECK(begin_blob_transfer(&fixture, &transfer));
    chunk = &fixture.tcp.host_sources[HOST_BLOB_CHUNK];
    chunk->work.value.blob_chunk.transfer = transfer;
    chunk->work.value.blob_chunk.offset = 0U;
    chunk->work.value.blob_chunk.payload = blob_payload;
    CHECK(dispatch_source(
        &fixture.tcp.host,
        &fixture.tcp.edge,
        chunk,
        NULL,
        &baseline_chunk
    ));
    CHECK(baseline_chunk.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          baseline_chunk.blob_operation ==
              MEISO_RUNTIME_BLOB_CHUNK &&
          baseline_chunk.blob_offset == 0U);
    CHECK(prepare_duplicate_actual_for_host_accept(&fixture));
    CHECK(dispatch_stream_sample(
        &fixture,
        EDGE_WIFI_STREAM,
        ROUTE_WIFI_DATA,
        77U,
        301U,
        1U,
        wifi_payload
    ));
    CHECK(dispatch_stream_sample(
        &fixture,
        EDGE_RENDER_STREAM,
        ROUTE_RENDER_RESULT,
        91U,
        302U,
        1U,
        render_payload
    ));
    CHECK(run_host_fairness(&fixture, transfer, blob_payload));
    CHECK(run_edge_fairness(&fixture, wifi_payload, render_payload));
    scene_accept = &fixture.tcp.host_sources[HOST_SCENE_ACTUAL_ACCEPT];
    CHECK(scene_accept->poll_status ==
          MEISO_RUNTIME_SCHEDULER_POLL_READY);
    CHECK(dispatch_source(
        &fixture.tcp.host,
        &fixture.tcp.edge,
        scene_accept,
        NULL,
        &accepted
    ));
    CHECK(accept_state_delivery(&fixture.scene_actual, &accepted));
    CHECK(abort_blob_transfer(
        &fixture, transfer, 2U * blob_payload.size
    ));
    CHECK(meiso_blob_store_count(fixture.blob_store) == 0U);
    destroy_semantic_fixture(&fixture);
    return true;
}

static bool initialize_profile_pair(
    TcpFixture *fixture,
    MeisoProfileRef edge_profile,
    uint64_t nonce_salt
)
{
    MeisoSessionConfig host_config = base_session_config(
        MEISO_SESSION_ROLE_HOST, nonce_salt
    );
    MeisoSessionConfig edge_config = base_session_config(
        MEISO_SESSION_ROLE_EDGE, nonce_salt
    );

    memset(fixture, 0, sizeof(*fixture));
    fixture->host.fd = -1;
    fixture->edge.fd = -1;
    CHECK(meiso_composition_session_v2_configure_session(&host_config) ==
          MEISO_COMPOSITION_SESSION_V2_OK);
    CHECK(meiso_composition_session_v2_configure_session(&edge_config) ==
          MEISO_COMPOSITION_SESSION_V2_OK);
    edge_config.profile = edge_profile;
    CHECK(make_tcp_pair(0, &fixture->host.fd, &fixture->edge.fd));
    CHECK(initialize_session(&fixture->host, &host_config));
    CHECK(initialize_session(&fixture->edge, &edge_config));
    CHECK(initialize_adapter(&fixture->host));
    CHECK(initialize_adapter(&fixture->edge));
    CHECK(meiso_session_start(fixture->edge.session) ==
          MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(fixture->host.session) ==
          MEISO_SESSION_START_STARTED);
    return true;
}

static bool read_profile_failure(Peer *peer)
{
    struct timespec deadline;

    CHECK(make_deadline(&deadline));
    while (remaining_poll_ms(&deadline) > 0) {
        MeisoSessionRuntimeMessage ignored;
        const MeisoPosixTcpReadStatus status =
            meiso_posix_tcp_on_readable(peer->adapter, &ignored);

        if (status == MEISO_POSIX_TCP_READ_SESSION_FAILED ||
            status == MEISO_POSIX_TCP_READ_PEER_CLOSED ||
            status == MEISO_POSIX_TCP_READ_TRANSPORT_LOST) {
            return true;
        }
        CHECK(status == MEISO_POSIX_TCP_READ_PROGRESS ||
              status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS ||
              status == MEISO_POSIX_TCP_READ_WOULD_BLOCK ||
              status == MEISO_POSIX_TCP_READ_INTERRUPTED);
        if (status != MEISO_POSIX_TCP_READ_PROGRESS &&
            status != MEISO_POSIX_TCP_READ_SESSION_PROGRESS) {
            CHECK(wait_fd(peer->fd, POLLIN, &deadline));
        }
    }
    return false;
}

static bool expect_wrong_profile(MeisoProfileRef edge_profile, uint64_t salt)
{
    TcpFixture fixture;
    MeisoSessionSnapshot host;
    MeisoSessionSnapshot edge;
    MeisoSessionRuntimeMessage ignored;

    CHECK(initialize_profile_pair(&fixture, edge_profile, salt));
    /* The Host is the protocol opener; Edge fails before it can reply. */
    CHECK(write_frame(&fixture.host));
    CHECK(read_profile_failure(&fixture.edge));
    CHECK(meiso_session_snapshot(fixture.host.session, &host) &&
          meiso_session_snapshot(fixture.edge.session, &edge));
    CHECK(host.state == MEISO_SESSION_STATE_HANDSHAKING &&
          edge.state == MEISO_SESSION_STATE_FAILED &&
          host.failure == MEISO_SESSION_FAILURE_NONE &&
          edge.failure == MEISO_SESSION_FAILURE_PROFILE_MISMATCH &&
          !host.session_id_known && !edge.session_id_known &&
          !host.outbound_pending && edge.outbound_pending);
    CHECK(!meiso_posix_tcp_wants_write(fixture.host.adapter) &&
          meiso_posix_tcp_wants_write(fixture.edge.adapter));
    CHECK(write_frame(&fixture.edge));
    CHECK(read_profile_failure(&fixture.host));
    CHECK(meiso_session_snapshot(fixture.host.session, &host) &&
          host.state == MEISO_SESSION_STATE_FAILED &&
          host.failure == MEISO_SESSION_FAILURE_PROFILE_MISMATCH &&
          !host.session_id_known && !host.outbound_pending &&
          !meiso_posix_tcp_wants_write(fixture.host.adapter));
    CHECK(meiso_posix_tcp_on_readable(
              fixture.host.adapter, &ignored
          ) != MEISO_POSIX_TCP_READ_RUNTIME &&
          meiso_posix_tcp_on_readable(
              fixture.edge.adapter, &ignored
          ) != MEISO_POSIX_TCP_READ_RUNTIME);
    destroy_tcp_fixture(&fixture);
    return true;
}

static bool test_wrong_profile_fails_closed_over_tcp(void)
{
    TcpFixture positive;

    CHECK(expect_wrong_profile(
        *meiso_composition_scene_profile(), 100U
    ));
    CHECK(expect_wrong_profile(*meiso_wifi_telemetry_profile(), 200U));
    CHECK(initialize_profile_pair(
        &positive, *meiso_composition_scene_profile_v2(), 300U
    ));
    CHECK(drive_ready(&positive));
    CHECK(session_ready(positive.host.session) &&
          session_ready(positive.edge.session));
    destroy_tcp_fixture(&positive);
    return true;
}

int main(void)
{
    if (!test_wrong_profile_fails_closed_over_tcp() ||
        !test_real_backpressure_edge_to_host() ||
        !test_real_backpressure_host_to_edge() ||
        !test_mixed_full_duplex_direction_local_fairness()) {
        return 1;
    }
    (void)puts("composition Runtime TCP adversarial tests passed");
    return 0;
}
