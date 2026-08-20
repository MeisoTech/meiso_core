#define _POSIX_C_SOURCE 200809L

#include "composition_coordinator.h"
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
                stderr, "CHECK failed at %s:%d: %s\n",                       \
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
    TEST_IO_BYTES_PER_STEP = 17,
    TEST_POLL_SLICE_MS = 20,
    TEST_DEADLINE_MS = 5000,
    TEST_HOST_SOURCE_COUNT = 9,
    TEST_EDGE_SOURCE_COUNT = 9
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
    HOST_BLOB_COMMIT = 8
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
    EDGE_RENDER_STREAM = 8
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
} TxSource;

typedef struct {
    int fd;
    SessionArena session_arena;
    AdapterArena adapter_arena;
    RuntimeArena runtime_arena;
    SchedulerArena scheduler_arena;
    MeisoSession *session;
    MeisoPosixTcp *adapter;
    MeisoRuntime *runtime;
    MeisoRuntimeScheduler *scheduler;
    TxSource *sources;
    size_t source_count;
    uint64_t runtime_frames_with_multi_write_progress;
    uint64_t runtime_frames_with_multi_read_progress;
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
    void *blob_arena;
    void *scene_store_arena;
    void *coordinator_arena;
    void *wifi_stream_arena;
    void *render_stream_arena;
    MeisoBlobStore *blob_store;
    MeisoCompositionSceneStore *scene_store;
    MeisoCompositionCoordinator *coordinator;
    StateChannel wifi_desired;
    StateChannel wifi_actual;
    StateChannel scene_desired;
    StateChannel plan_desired;
    StateChannel presentation_actual;
    StateChannel scene_actual;
    MeisoStream *wifi_stream;
    MeisoStream *render_stream;
    uint8_t expected_wifi_sample[
        MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE
    ];
    size_t expected_wifi_sample_size;
} SemanticFixture;

static const MeisoOwnerScope HOST_OWNER = {
    MEISO_OWNER_ROLE_HOST, UINT64_C(0x101)
};
static const MeisoOwnerScope EDGE_OWNER = {
    MEISO_OWNER_ROLE_EDGE, UINT64_C(0x201)
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

static bool wait_fd(
    int fd,
    short events,
    const struct timespec *deadline
)
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

static bool make_tcp_pair(int *out_client, int *out_server)
{
    struct sockaddr_in address;
    struct sockaddr_in client_address;
    struct sockaddr_in server_address;
    struct timespec deadline;
    socklen_t address_size = (socklen_t)sizeof(address);
    socklen_t client_size = (socklen_t)sizeof(client_address);
    socklen_t server_size = (socklen_t)sizeof(server_address);
    int listener = -1;
    int client = -1;
    int server = -1;
    int enabled = 1;
    int connect_error = 0;
    int socket_type = 0;
    socklen_t connect_error_size = (socklen_t)sizeof(connect_error);
    socklen_t socket_type_size = (socklen_t)sizeof(socket_type);

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
    if (client < 0 || !set_nonblocking(client)) {
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
    if (server < 0 || !set_nonblocking(server) || getsockname(
            client,
            (struct sockaddr *)(void *)&client_address,
            &client_size
        ) != 0 || getpeername(
            client,
            (struct sockaddr *)(void *)&server_address,
            &server_size
        ) != 0 || client_address.sin_family != AF_INET ||
        server_address.sin_family != AF_INET ||
        server_address.sin_addr.s_addr != htonl(UINT32_C(0x7f000001)) ||
        getsockopt(
            client, SOL_SOCKET, SO_TYPE,
            &socket_type, &socket_type_size
        ) != 0 || socket_type != SOCK_STREAM) {
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

static MeisoSessionConfig session_config(
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
    SessionArena *arena,
    const MeisoSessionConfig *config,
    MeisoSession **out_session
)
{
    const size_t required = meiso_session_memory_required(config);

    CHECK(required != 0U && required <= sizeof(arena->bytes));
    CHECK(meiso_session_init(
        arena->bytes, required, config, out_session
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
    const size_t required = meiso_posix_tcp_memory_required(&config);

    CHECK(required != 0U && required <= sizeof(peer->adapter_arena.bytes));
    CHECK(meiso_posix_tcp_init(
        peer->adapter_arena.bytes,
        required,
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
    uint64_t progress_count = 0U;

    CHECK(make_deadline(&deadline));
    while (remaining_poll_ms(&deadline) > 0) {
        const MeisoPosixTcpWriteStatus status =
            meiso_posix_tcp_on_writable(peer->adapter);

        if (status == MEISO_POSIX_TCP_WRITE_FRAME_SENT) {
            if (progress_count > 1U) {
                peer->runtime_frames_with_multi_write_progress++;
            }
            return true;
        }
        CHECK(status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
              status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK ||
              status == MEISO_POSIX_TCP_WRITE_INTERRUPTED);
        if (status == MEISO_POSIX_TCP_WRITE_PROGRESS) {
            progress_count++;
        }
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
    uint64_t progress_count = 0U;

    CHECK(make_deadline(&deadline));
    while (remaining_poll_ms(&deadline) > 0) {
        const MeisoPosixTcpReadStatus status =
            meiso_posix_tcp_on_readable(peer->adapter, out_message);

        if (status == MEISO_POSIX_TCP_READ_RUNTIME) {
            if (progress_count > 1U) {
                peer->runtime_frames_with_multi_read_progress++;
            }
            return true;
        }
        CHECK(status == MEISO_POSIX_TCP_READ_PROGRESS ||
              status == MEISO_POSIX_TCP_READ_WOULD_BLOCK ||
              status == MEISO_POSIX_TCP_READ_INTERRUPTED ||
              status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS);
        if (status == MEISO_POSIX_TCP_READ_PROGRESS) {
            progress_count++;
        }
        if (status != MEISO_POSIX_TCP_READ_PROGRESS &&
            status != MEISO_POSIX_TCP_READ_SESSION_PROGRESS) {
            CHECK(wait_fd(peer->fd, POLLIN, &deadline));
        }
    }
    return false;
}

static bool dispatch_source(
    Peer *sender,
    Peer *receiver,
    TxSource *source,
    MeisoRuntimeSchedulerStepResult *out_step,
    MeisoRuntimeEvent *out_event
)
{
    MeisoSessionRuntimeMessage message;
    MeisoRuntimeSchedulerStepResult step;
    MeisoSessionSnapshot session_snapshot;
    const uint64_t completions_before = source->completions;

    source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    CHECK(meiso_runtime_scheduler_step(sender->scheduler, &step) ==
          MEISO_RUNTIME_SCHEDULER_STEP_SENT);
    CHECK(step.completion.source_id == source->id &&
          step.completion.kind == source->work.kind &&
          step.completion.code == MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT &&
          source->completions == completions_before + 1U &&
          source->poll_status == MEISO_RUNTIME_SCHEDULER_POLL_EMPTY);
    CHECK(write_frame(sender));
    CHECK(read_runtime(receiver, &message));
    CHECK(meiso_session_snapshot(receiver->session, &session_snapshot) &&
          session_snapshot.session_id_known && meiso_session_id_equal(
              message.session_id, session_snapshot.session_id
          ));
    CHECK(meiso_runtime_scheduler_receive(
        receiver->scheduler, &message, out_event
    ) == MEISO_RUNTIME_OK);
    if (out_step != NULL) {
        *out_step = step;
    }
    return true;
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
    size_t required;

    CHECK(meiso_composition_session_v2_runtime_config(
        peer->session, &config
    ) == MEISO_COMPOSITION_SESSION_V2_OK);
    required = meiso_runtime_v3_memory_required(&config);
    CHECK(required != 0U && required <= sizeof(peer->runtime_arena.bytes));
    CHECK(meiso_runtime_v3_init(
        peer->runtime_arena.bytes, required, &config, &peer->runtime
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
        memcmp(&lhs.endpoint_id, &rhs.endpoint_id,
               sizeof(lhs.endpoint_id)) == 0 &&
        memcmp(&lhs.resource_id, &rhs.resource_id,
               sizeof(lhs.resource_id)) == 0;
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
    source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_EMPTY;
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
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_COMMIT:
        source->work.value.blob_commit.transfer.route = route;
        break;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT:
        source->work.value.blob_result.transfer.route = route;
        break;
    default:
        break;
    }
}

static bool register_peer_sources(
    TcpFixture *fixture,
    Peer *peer,
    bool host
)
{
    const uint16_t max_control = host ? 8U : 7U;
    const uint16_t max_data = host ? 1U : 2U;
    const MeisoRuntimeSchedulerConfig scheduler_config = {
        peer->runtime, peer->session, max_control, max_data
    };
    size_t required = meiso_runtime_scheduler_memory_required(
        &scheduler_config
    );
    size_t index;

    CHECK(required != 0U && required <= sizeof(peer->scheduler_arena.bytes));
    CHECK(meiso_runtime_scheduler_init(
        peer->scheduler_arena.bytes,
        required,
        &scheduler_config,
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
        MeisoRuntimeRouteKey route;
        const MeisoRuntimeSchedulerWork *work = &peer->sources[index].work;

        if (work->kind == MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER) {
            route = work->value.state_offer.route;
        } else if (work->kind ==
                   MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT) {
            route = work->value.state_accept.route;
        } else if (work->kind ==
                   MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE) {
            route = work->value.stream_sample.route;
        } else if (work->kind == MEISO_RUNTIME_SCHEDULER_WORK_BLOB_BEGIN) {
            route = work->value.blob_begin.route;
        } else if (work->kind == MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK) {
            route = work->value.blob_chunk.transfer.route;
        } else if (work->kind == MEISO_RUNTIME_SCHEDULER_WORK_BLOB_COMMIT) {
            route = work->value.blob_commit.transfer.route;
        } else {
            route = work->value.blob_result.transfer.route;
        }
        CHECK(meiso_runtime_scheduler_bind_source_route(
            peer->scheduler, peer->sources[index].id, route
        ) == MEISO_RUNTIME_SCHEDULER_OK);
    }
    CHECK(meiso_runtime_scheduler_finish_rebind(peer->scheduler) ==
          MEISO_RUNTIME_SCHEDULER_OK);
    (void)fixture;
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
        &fixture->host_sources[HOST_BLOB_COMMIT],
        MEISO_RUNTIME_SCHEDULER_WORK_BLOB_COMMIT,
        route_key(fixture, ROUTE_MESH_BLOB)
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
    CHECK(register_peer_sources(fixture, &fixture->host, true));
    CHECK(register_peer_sources(fixture, &fixture->edge, false));
    return true;
}

static bool build_catalog(TcpFixture *fixture)
{
    const MeisoCompositionSessionV2CatalogConfig config = {
        UINT64_C(0x501), UINT64_C(0x601), UINT64_C(0x701),
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
    MeisoRuntimeSnapshot host_snapshot;
    MeisoRuntimeSnapshot edge_snapshot;

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
    CHECK(MEISO_COMPOSITION_SESSION_V2_CATALOG_WIRE_SIZE == 920U &&
          message.payload.size == 920U);
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
    CHECK(meiso_runtime_snapshot(fixture->host.runtime, &host_snapshot));
    CHECK(meiso_runtime_snapshot(fixture->edge.runtime, &edge_snapshot));
    CHECK(host_snapshot.active_endpoint_count == 3U &&
          host_snapshot.active_route_count == 9U &&
          edge_snapshot.active_endpoint_count == 3U &&
          edge_snapshot.active_route_count == 9U);
    return true;
}

static bool initialize_tcp_fixture(TcpFixture *fixture)
{
    MeisoSessionConfig host_config = session_config(
        MEISO_SESSION_ROLE_HOST, 0U
    );
    MeisoSessionConfig edge_config = session_config(
        MEISO_SESSION_ROLE_EDGE, 0U
    );

    memset(fixture, 0, sizeof(*fixture));
    fixture->host.fd = -1;
    fixture->edge.fd = -1;
    CHECK(meiso_composition_session_v2_configure_session(&host_config) ==
          MEISO_COMPOSITION_SESSION_V2_OK);
    CHECK(meiso_composition_session_v2_configure_session(&edge_config) ==
          MEISO_COMPOSITION_SESSION_V2_OK);
    CHECK(meiso_profile_ref_equal(
              host_config.profile, *meiso_composition_scene_profile_v2()) &&
          meiso_profile_ref_equal(
              edge_config.profile, *meiso_composition_scene_profile_v2()));
    CHECK(make_tcp_pair(&fixture->host.fd, &fixture->edge.fd));
    CHECK(initialize_session(
        &fixture->host.session_arena,
        &host_config,
        &fixture->host.session
    ));
    CHECK(initialize_session(
        &fixture->edge.session_arena,
        &edge_config,
        &fixture->edge.session
    ));
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

static bool allocate_arena(void **out_arena, size_t required)
{
    if (out_arena == NULL || required == 0U) {
        return false;
    }
    *out_arena = calloc(1U, required);
    return *out_arena != NULL;
}

static bool initialize_state_channel(
    StateChannel *channel,
    const MeisoResourceDescriptor *descriptor,
    uint64_t delivery_value,
    MeisoOwnerScope replica_owner
)
{
    const MeisoStateConfig config = {MEISO_STATE_DELIVERY_CONVERGENT};
    size_t required;

    memset(channel, 0, sizeof(*channel));
    channel->delivery = (MeisoDeliveryGeneration){delivery_value};
    required = meiso_state_publisher_memory_required(descriptor, &config);
    CHECK(allocate_arena(&channel->publisher_arena, required));
    CHECK(meiso_state_publisher_init(
        channel->publisher_arena, required, descriptor, &config,
        &channel->publisher
    ) == MEISO_STATE_INIT_OK);
    required = meiso_state_replica_memory_required(descriptor);
    CHECK(allocate_arena(&channel->replica_arena, required));
    CHECK(meiso_state_replica_init(
        channel->replica_arena, required, descriptor, &channel->replica
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
    uint32_t queue_slots,
    uint64_t stream_epoch
)
{
    const MeisoStreamConfig config = {
        MEISO_STREAM_ORDER_STRICT_MONOTONIC,
        MEISO_STREAM_OVERFLOW_DROP_OLDEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        queue_slots,
        0U,
        0U
    };
    size_t required = meiso_stream_memory_required(descriptor, &config);

    CHECK(allocate_arena(out_arena, required));
    CHECK(meiso_stream_init(
        *out_arena, required, descriptor, &config, out_stream
    ) == MEISO_STREAM_INIT_OK);
    CHECK(meiso_stream_begin_producer(
        *out_stream, EDGE_OWNER, stream_epoch
    ) == MEISO_STREAM_PRODUCER_STARTED);
    return true;
}

static void destroy_semantic_fixture(SemanticFixture *fixture)
{
    free(fixture->render_stream_arena);
    free(fixture->wifi_stream_arena);
    destroy_state_channel(&fixture->scene_actual);
    destroy_state_channel(&fixture->presentation_actual);
    destroy_state_channel(&fixture->plan_desired);
    destroy_state_channel(&fixture->scene_desired);
    destroy_state_channel(&fixture->wifi_actual);
    destroy_state_channel(&fixture->wifi_desired);
    free(fixture->coordinator_arena);
    free(fixture->scene_store_arena);
    free(fixture->blob_arena);
    destroy_tcp_fixture(&fixture->tcp);
    memset(fixture, 0, sizeof(*fixture));
}

static bool initialize_semantic_fixture(SemanticFixture *fixture)
{
    const MeisoResourceDescriptor *wifi_desired =
        meiso_wifi_telemetry_resource_for_facet(
            MEISO_RUNTIME_FACET_DESIRED
        );
    const MeisoResourceDescriptor *wifi_actual =
        meiso_wifi_telemetry_resource_for_facet(MEISO_RUNTIME_FACET_ACTUAL);
    const MeisoResourceDescriptor *wifi_data =
        meiso_wifi_telemetry_resource_for_facet(MEISO_RUNTIME_FACET_DATA);
    const MeisoResourceDescriptor *scene_desired =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_SCENE_DESIRED
        );
    const MeisoResourceDescriptor *plan_desired =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_PRESENTATION_DESIRED
        );
    const MeisoResourceDescriptor *presentation_actual =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_PRESENTATION_ACTUAL
        );
    const MeisoResourceDescriptor *scene_actual =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_SCENE_ACTUAL
        );
    const MeisoResourceDescriptor *mesh = meiso_composition_scene_resource(
        MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
    );
    const MeisoResourceDescriptor *render = meiso_composition_scene_resource(
        MEISO_COMPOSITION_SCENE_RESOURCE_RENDER_RESULT
    );
    const MeisoBlobConfig blob_config = {2U};
    MeisoCompositionCoordinatorConfig coordinator_config;
    MeisoSessionSnapshot session_snapshot;
    size_t required;

    memset(fixture, 0, sizeof(*fixture));
    fixture->tcp.host.fd = -1;
    fixture->tcp.edge.fd = -1;
    CHECK(wifi_desired != NULL && wifi_actual != NULL && wifi_data != NULL &&
          scene_desired != NULL && plan_desired != NULL &&
          presentation_actual != NULL && scene_actual != NULL &&
          mesh != NULL && render != NULL);
    CHECK(initialize_tcp_fixture(&fixture->tcp));
    CHECK(meiso_session_snapshot(
        fixture->tcp.edge.session, &session_snapshot
    ) && session_snapshot.session_id_known);

    required = meiso_blob_store_memory_required(mesh, &blob_config);
    CHECK(allocate_arena(&fixture->blob_arena, required));
    CHECK(meiso_blob_store_init(
        fixture->blob_arena, required, mesh, &blob_config,
        &fixture->blob_store
    ) == MEISO_BLOB_INIT_OK);
    required = meiso_composition_scene_store_memory_required();
    CHECK(allocate_arena(&fixture->scene_store_arena, required));
    CHECK(meiso_composition_scene_store_init(
        fixture->scene_store_arena, required, fixture->blob_store,
        &fixture->scene_store
    ) == MEISO_COMPOSITION_SCENE_STORE_INIT_OK);
    memset(&coordinator_config, 0, sizeof(coordinator_config));
    coordinator_config.scene_store = fixture->scene_store;
    coordinator_config.session_id = session_snapshot.session_id;
    coordinator_config.edge_incarnation = EDGE_OWNER.incarnation;
    coordinator_config.capability_generation = 1U;
    coordinator_config.edge_clock_epoch = 77U;
    required = meiso_composition_coordinator_memory_required();
    CHECK(allocate_arena(&fixture->coordinator_arena, required));
    CHECK(meiso_composition_coordinator_init(
        fixture->coordinator_arena, required, &coordinator_config,
        &fixture->coordinator
    ) == MEISO_COMPOSITION_COORDINATOR_INIT_OK);
    CHECK(meiso_composition_coordinator_begin_owner(
        fixture->coordinator, HOST_OWNER.incarnation
    ) == MEISO_COMPOSITION_COORDINATOR_OWNER_STARTED);

    CHECK(initialize_state_channel(
        &fixture->wifi_desired, wifi_desired, 11U, HOST_OWNER
    ));
    CHECK(initialize_state_channel(
        &fixture->wifi_actual, wifi_actual, 12U, EDGE_OWNER
    ));
    CHECK(initialize_state_channel(
        &fixture->scene_desired, scene_desired, 13U, HOST_OWNER
    ));
    CHECK(initialize_state_channel(
        &fixture->plan_desired, plan_desired, 14U, HOST_OWNER
    ));
    CHECK(initialize_state_channel(
        &fixture->presentation_actual, presentation_actual, 15U, EDGE_OWNER
    ));
    CHECK(initialize_state_channel(
        &fixture->scene_actual, scene_actual, 16U, EDGE_OWNER
    ));
    CHECK(initialize_stream(
        &fixture->wifi_stream_arena, &fixture->wifi_stream,
        wifi_data, 4U, 177U
    ));
    CHECK(initialize_stream(
        &fixture->render_stream_arena, &fixture->render_stream,
        render, 4U, 417U
    ));
    return true;
}

static bool deliver_state_over_scheduler(
    TcpFixture *tcp,
    StateChannel *channel,
    bool host_to_edge,
    size_t offer_source_index,
    size_t accept_source_index,
    MeisoStateView *out_current
)
{
    Peer *sender = host_to_edge ? &tcp->host : &tcp->edge;
    Peer *receiver = host_to_edge ? &tcp->edge : &tcp->host;
    TxSource *offer_source = &sender->sources[offer_source_index];
    TxSource *accept_source = &receiver->sources[accept_source_index];
    MeisoStateOffer offer;
    MeisoRuntimeEvent event;
    MeisoRuntimeEvent accepted;
    MeisoStateReceiveStatus receive_status;
    const MeisoRuntimeRouteKey route =
        offer_source->work.value.state_offer.route;

    CHECK(meiso_state_publisher_next_offer(channel->publisher, &offer));
    offer_source->work.value.state_offer.offer = offer;
    CHECK(dispatch_source(
        sender, receiver, offer_source, NULL, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER &&
          route_key_equal(event.route, route));
    receive_status = meiso_state_replica_receive(
        channel->replica,
        event.state_offer.state.owner,
        event.state_offer.state.version,
        event.state_offer.state.payload
    );
    CHECK(receive_status == MEISO_STATE_RECEIVE_NEW);
    CHECK(meiso_runtime_scheduler_commit_state_receive(
        receiver->scheduler, event.route, &event.state_offer, receive_status
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
          route_key_equal(accepted.route, route));
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        channel->publisher,
        accepted.state_accept_generation,
        accepted.state_accept_owner,
        accepted.state_accept_version
    ) == MEISO_STATE_ACCEPT_CONFIRMED);
    if (out_current != NULL) {
        CHECK(meiso_state_replica_current(channel->replica, out_current));
    }
    return true;
}

static bool publish_cube_over_scheduler(
    SemanticFixture *fixture,
    uint8_t cube_storage[512],
    MeisoByteView *out_cube,
    MeisoContentId *out_content_id
)
{
    TxSource *begin = &fixture->tcp.host_sources[HOST_BLOB_BEGIN];
    TxSource *chunk = &fixture->tcp.host_sources[HOST_BLOB_CHUNK];
    TxSource *commit = &fixture->tcp.host_sources[HOST_BLOB_COMMIT];
    TxSource *result = &fixture->tcp.edge_sources[EDGE_BLOB_RESULT];
    MeisoRuntimeSchedulerStepResult begin_step;
    MeisoRuntimeEvent edge_event;
    MeisoRuntimeEvent host_event;
    MeisoRuntimeBlobTransferRef transfer;
    MeisoRuntimeBlobPublishReport report;
    size_t cube_size = 0U;

    CHECK(meiso_composition_scene_mesh_encode(
        meiso_composition_scene_canonical_cube(),
        (MeisoMutableBuffer){cube_storage, 512U}, &cube_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    *out_cube = (MeisoByteView){cube_storage, cube_size};
    CHECK(meiso_composition_scene_mesh_content_id(
        *out_cube, out_content_id
    ));
    begin->work.value.blob_begin.content_id = *out_content_id;
    begin->work.value.blob_begin.total_size = cube_size;
    CHECK(dispatch_source(
        &fixture->tcp.host, &fixture->tcp.edge, begin,
        &begin_step, &edge_event
    ));
    CHECK(begin_step.completion.generated_blob_transfer_present &&
          edge_event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          edge_event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN);
    transfer = begin_step.completion.generated_blob_transfer;
    CHECK(edge_event.blob_transfer.transfer_id == transfer.transfer_id &&
          edge_event.blob_transfer.total_size == cube_size);

    result->work.value.blob_result.transfer = edge_event.blob_transfer;
    result->work.value.blob_result.operation = MEISO_RUNTIME_BLOB_BEGIN;
    CHECK(dispatch_source(
        &fixture->tcp.edge, &fixture->tcp.host, result, NULL, &host_event
    ));
    CHECK(host_event.type == MEISO_RUNTIME_EVENT_BLOB_RESULT &&
          host_event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN &&
          host_event.blob_result == MEISO_RUNTIME_BLOB_RESULT_READY &&
          host_event.blob_next_offset == 0U);

    chunk->work.value.blob_chunk.transfer = transfer;
    chunk->work.value.blob_chunk.offset = 0U;
    chunk->work.value.blob_chunk.payload = *out_cube;
    CHECK(dispatch_source(
        &fixture->tcp.host, &fixture->tcp.edge, chunk, NULL, &edge_event
    ));
    CHECK(edge_event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          edge_event.blob_operation == MEISO_RUNTIME_BLOB_CHUNK &&
          edge_event.blob_offset == 0U &&
          edge_event.blob_payload.size == cube_size &&
          memcmp(edge_event.blob_payload.data, cube_storage, cube_size) == 0);

    commit->work.value.blob_commit.transfer = transfer;
    CHECK(dispatch_source(
        &fixture->tcp.host, &fixture->tcp.edge, commit, NULL, &edge_event
    ));
    CHECK(edge_event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          edge_event.blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          edge_event.blob_payload.size == cube_size &&
          memcmp(edge_event.blob_payload.data, cube_storage, cube_size) == 0);
    transfer = edge_event.blob_transfer;
    CHECK(meiso_runtime_scheduler_publish_blob_receive(
        fixture->tcp.edge.scheduler,
        &transfer,
        fixture->blob_store,
        meiso_composition_scene_mesh_blob_verify,
        NULL,
        &report
    ) == MEISO_RUNTIME_OK);
    CHECK(report.stage_attempted && report.commit_attempted &&
          !report.forced_abort_attempted &&
          report.stage_status == MEISO_BLOB_STAGE_STARTED &&
          report.commit_status == MEISO_BLOB_COMMIT_PUBLISHED &&
          report.result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
          meiso_blob_store_count(fixture->blob_store) == 1U);
    CHECK(meiso_composition_scene_store_mark_blob_visible(
        fixture->scene_store,
        HOST_OWNER.incarnation,
        out_content_id,
        cube_size
    ) == MEISO_COMPOSITION_SCENE_BLOB_VISIBLE);

    result->work.value.blob_result.transfer = transfer;
    result->work.value.blob_result.operation = MEISO_RUNTIME_BLOB_COMMIT;
    CHECK(dispatch_source(
        &fixture->tcp.edge, &fixture->tcp.host, result, NULL, &host_event
    ));
    CHECK(host_event.type == MEISO_RUNTIME_EVENT_BLOB_RESULT &&
          host_event.blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          host_event.blob_result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
          host_event.blob_next_offset == cube_size);
    return true;
}

static MeisoTimePoint composition_time(uint64_t monotonic_ns)
{
    return (MeisoTimePoint){77U, monotonic_ns};
}

static void fill_nonce(
    uint8_t output[MEISO_SHA256_DIGEST_SIZE],
    uint8_t seed
)
{
    size_t index;

    for (index = 0U; index < MEISO_SHA256_DIGEST_SIZE; ++index) {
        output[index] = (uint8_t)(seed + (uint8_t)index);
    }
}

static bool publish_desired_state(
    StateChannel *channel,
    uint64_t version,
    MeisoByteView profile
)
{
    uint8_t storage[MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE];
    MeisoRuntimeDesiredState desired = {true, profile};
    MeisoByteView payload;

    CHECK(meiso_runtime_desired_state_encode(
        &desired, (MeisoMutableBuffer){storage, sizeof(storage)}, &payload
    ));
    CHECK(meiso_state_publisher_publish(
        channel->publisher, HOST_OWNER, version, payload
    ) == MEISO_STATE_PUBLISH_NEW);
    return true;
}

static bool publish_actual_state(
    StateChannel *channel,
    uint64_t version,
    uint64_t desired_version,
    uint64_t production_epoch,
    MeisoByteView profile
)
{
    uint8_t storage[
        MEISO_COMPOSITION_SCENE_PRESENTATION_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    ];
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
    actual.profile = profile;
    CHECK(meiso_runtime_actual_state_encode(
        &actual, (MeisoMutableBuffer){storage, sizeof(storage)}, &payload
    ));
    CHECK(meiso_state_publisher_publish(
        channel->publisher, EDGE_OWNER, version, payload
    ) == MEISO_STATE_PUBLISH_NEW);
    return true;
}

static MeisoCompositionSceneDesired scene_value(MeisoContentId mesh_id)
{
    MeisoCompositionSceneDesired value;
    MeisoCompositionSceneObject *object;

    memset(&value, 0, sizeof(value));
    value.camera.space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    value.camera.space_epoch = 9U;
    value.camera.source_time = composition_time(10U);
    value.camera.position[2] = -3.0F;
    value.camera.orientation_xyzw[3] = 1.0F;
    value.camera.projection = MEISO_COMPOSITION_SCENE_PROJECTION_PERSPECTIVE;
    value.camera.vertical_fov_degrees = 60.0F;
    value.camera.near_z = 0.1F;
    value.camera.far_z = 100.0F;
    value.camera.viewport_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    value.camera.viewport_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    value.object_count = 1U;
    object = &value.objects[0];
    object->id = (MeisoCompositionSceneObjectId){1U, 1U};
    object->mesh_content_id = mesh_id;
    object->space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    object->space_epoch = value.camera.space_epoch;
    object->transform.orientation_xyzw[3] = 1.0F;
    object->transform.scale[0] = 1.0F;
    object->transform.scale[1] = 1.0F;
    object->transform.scale[2] = 1.0F;
    object->flags = MEISO_COMPOSITION_SCENE_OBJECT_VISIBLE;
    return value;
}

static MeisoCompositionScenePresentationPlan plan_value(void)
{
    MeisoCompositionScenePresentationPlan plan;
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE];

    memset(&plan, 0, sizeof(plan));
    plan.plan_generation = 1U;
    plan.capability_generation = 1U;
    plan.scene_binding = *meiso_composition_scene_scene_endpoint();
    plan.source = MEISO_COMPOSITION_SCENE_LAYER_SOURCE_EDGE_NATIVE;
    plan.geometry = MEISO_COMPOSITION_SCENE_GEOMETRY_PROJECTION;
    plan.target_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    plan.target_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    plan.color_format = MEISO_COMPOSITION_SCENE_COLOR_RGBA8888;
    plan.depth_format = MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32;
    plan.max_result_hz = MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ;
    plan.offline_lease_ms = 1000U;
    if (!meiso_composition_coordinator_plan_digest(&plan, digest)) {
        memset(&plan, 0, sizeof(plan));
    } else {
        memcpy(plan.candidate_digest, digest, sizeof(digest));
    }
    return plan;
}

static bool deliver_wifi_desired(SemanticFixture *fixture)
{
    const MeisoWifiTelemetryDesiredParameters parameters = {
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
            MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM,
        1000U
    };
    uint8_t suffix[MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE];
    MeisoStateView current;
    MeisoRuntimeDesiredState decoded_runtime;
    MeisoWifiTelemetryDesiredParameters decoded_parameters;
    size_t suffix_size = 0U;

    CHECK(meiso_wifi_telemetry_desired_parameters_encode(
        &parameters,
        (MeisoMutableBuffer){suffix, sizeof(suffix)},
        &suffix_size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(publish_desired_state(
        &fixture->wifi_desired, 1U,
        (MeisoByteView){suffix, suffix_size}
    ));
    CHECK(deliver_state_over_scheduler(
        &fixture->tcp,
        &fixture->wifi_desired,
        true,
        HOST_WIFI_DESIRED,
        EDGE_WIFI_DESIRED_ACCEPT,
        &current
    ));
    CHECK(current.version == 1U &&
          meiso_owner_scope_equal(current.owner, HOST_OWNER) &&
          meiso_runtime_desired_state_decode(
              current.payload, &decoded_runtime
          ) && decoded_runtime.enabled);
    CHECK(meiso_wifi_telemetry_desired_parameters_decode(
        decoded_runtime.profile, &decoded_parameters
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(decoded_parameters.selected_fields == parameters.selected_fields &&
          decoded_parameters.sample_period_ms == parameters.sample_period_ms);
    return true;
}

static bool deliver_scene_desired(
    SemanticFixture *fixture,
    MeisoContentId cube_id
)
{
    const MeisoCompositionSceneDesired scene = scene_value(cube_id);
    uint8_t suffix[MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE];
    MeisoStateView current;
    MeisoRuntimeDesiredState decoded_runtime;
    MeisoCompositionSceneDesired decoded_scene;
    size_t suffix_size = 0U;

    CHECK(meiso_composition_scene_desired_encode(
        &scene,
        (MeisoMutableBuffer){suffix, sizeof(suffix)},
        &suffix_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_desired_state(
        &fixture->scene_desired, 1U,
        (MeisoByteView){suffix, suffix_size}
    ));
    CHECK(deliver_state_over_scheduler(
        &fixture->tcp,
        &fixture->scene_desired,
        true,
        HOST_SCENE_DESIRED,
        EDGE_SCENE_DESIRED_ACCEPT,
        &current
    ));
    CHECK(current.version == 1U &&
          meiso_runtime_desired_state_decode(
              current.payload, &decoded_runtime
          ) && decoded_runtime.enabled);
    CHECK(meiso_composition_scene_desired_decode(
        decoded_runtime.profile, &decoded_scene
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(decoded_scene.object_count == 1U &&
          memcmp(&decoded_scene.objects[0].mesh_content_id,
                 &cube_id, sizeof(cube_id)) == 0);
    CHECK(meiso_composition_scene_store_prepare(
        fixture->scene_store,
        current.owner.incarnation,
        current.version,
        1U,
        &decoded_scene
    ) == MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    return true;
}

static bool activate_plan_over_state(
    SemanticFixture *fixture,
    MeisoCompositionSceneWorldRef *out_world
)
{
    MeisoCompositionScenePresentationPlan plan = plan_value();
    uint8_t suffix[MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE];
    uint8_t nonce[MEISO_SHA256_DIGEST_SIZE];
    uint8_t token[MEISO_SHA256_DIGEST_SIZE];
    MeisoStateView current;
    MeisoRuntimeDesiredState decoded_runtime;
    MeisoCompositionCoordinatorPlanView candidate;
    MeisoCompositionCoordinatorPlanView active;
    MeisoCompositionCoordinatorPlanRef expected;
    size_t suffix_size = 0U;

    CHECK(plan.plan_generation == 1U);
    CHECK(meiso_composition_scene_presentation_plan_encode(
        &plan,
        (MeisoMutableBuffer){suffix, sizeof(suffix)},
        &suffix_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_desired_state(
        &fixture->plan_desired, 1U,
        (MeisoByteView){suffix, suffix_size}
    ));
    CHECK(deliver_state_over_scheduler(
        &fixture->tcp,
        &fixture->plan_desired,
        true,
        HOST_PLAN_DESIRED,
        EDGE_PLAN_DESIRED_ACCEPT,
        &current
    ));
    CHECK(meiso_runtime_desired_state_decode(
        current.payload, &decoded_runtime
    ));
    CHECK(meiso_composition_coordinator_observe_desired(
        fixture->coordinator,
        current.owner.incarnation,
        current.version,
        &decoded_runtime,
        composition_time(100U)
    ) == MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING);
    CHECK(meiso_composition_coordinator_candidate(
        fixture->coordinator, &candidate
    ));
    expected = candidate.ref;
    fill_nonce(nonce, 0x31U);
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
        fixture->coordinator,
        &expected,
        MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
        composition_time(110U),
        nonce
    ) == MEISO_COMPOSITION_COORDINATOR_REPORT_PREPARED);
    CHECK(meiso_composition_coordinator_candidate(
        fixture->coordinator, &candidate
    ) && candidate.prepared_token_present &&
          candidate.candidate_state ==
              MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED);
    memcpy(token, candidate.prepared_token, sizeof(token));
    plan.prepared_token_echo_present = true;
    memcpy(plan.prepared_token_echo, token, sizeof(token));
    CHECK(meiso_composition_scene_presentation_plan_encode(
        &plan,
        (MeisoMutableBuffer){suffix, sizeof(suffix)},
        &suffix_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_desired_state(
        &fixture->plan_desired, 2U,
        (MeisoByteView){suffix, suffix_size}
    ));
    CHECK(deliver_state_over_scheduler(
        &fixture->tcp,
        &fixture->plan_desired,
        true,
        HOST_PLAN_DESIRED,
        EDGE_PLAN_DESIRED_ACCEPT,
        &current
    ));
    CHECK(meiso_runtime_desired_state_decode(
        current.payload, &decoded_runtime
    ));
    CHECK(meiso_composition_coordinator_observe_desired(
        fixture->coordinator,
        current.owner.incarnation,
        current.version,
        &decoded_runtime,
        composition_time(120U)
    ) == MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING);
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
        fixture->coordinator,
        token,
        composition_time(130U),
        out_world
    ) == MEISO_COMPOSITION_COORDINATOR_ACTIVATE_ACTIVATED);
    CHECK(meiso_composition_coordinator_active(
        fixture->coordinator, &active
    ) && active.active_state == MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE &&
          active.ref.plan_generation == 1U &&
          out_world->desired_owner_incarnation == HOST_OWNER.incarnation &&
          out_world->desired_state_version == 1U &&
          out_world->plan_generation == 1U &&
          out_world->world_generation == 1U);
    return true;
}

static bool deliver_wifi_actual(SemanticFixture *fixture)
{
    const MeisoWifiTelemetryEffectiveParameters parameters = {
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
            MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM,
        1000U
    };
    uint8_t suffix[MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE];
    MeisoStateView current;
    MeisoRuntimeActualState decoded_runtime;
    MeisoWifiTelemetryEffectiveParameters decoded_parameters;
    size_t suffix_size = 0U;

    CHECK(meiso_wifi_telemetry_effective_parameters_encode(
        &parameters,
        (MeisoMutableBuffer){suffix, sizeof(suffix)},
        &suffix_size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(publish_actual_state(
        &fixture->wifi_actual, 1U, 1U, 77U,
        (MeisoByteView){suffix, suffix_size}
    ));
    CHECK(deliver_state_over_scheduler(
        &fixture->tcp,
        &fixture->wifi_actual,
        false,
        EDGE_WIFI_ACTUAL,
        HOST_WIFI_ACTUAL_ACCEPT,
        &current
    ));
    CHECK(current.version == 1U &&
          meiso_owner_scope_equal(current.owner, EDGE_OWNER) &&
          meiso_runtime_actual_state_decode(
              current.payload, &decoded_runtime
          ));
    CHECK(decoded_runtime.production_epoch == 77U &&
          decoded_runtime.production_status ==
              MEISO_RUNTIME_PRODUCTION_RUNNING &&
          decoded_runtime.applied_desired.present &&
          decoded_runtime.applied_desired.version == 1U);
    CHECK(meiso_wifi_telemetry_effective_parameters_decode(
        decoded_runtime.profile, &decoded_parameters
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(decoded_parameters.selected_fields == parameters.selected_fields &&
          decoded_parameters.sample_period_ms == parameters.sample_period_ms);
    return true;
}

static bool deliver_composition_actuals(
    SemanticFixture *fixture,
    size_t cube_size
)
{
    const MeisoCompositionSceneResourceUsage usage = {
        1U, cube_size, 0U, 0U, 0U, 0U
    };
    uint8_t presentation_suffix[
        MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE
    ];
    uint8_t scene_suffix[MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE];
    MeisoCompositionScenePresentationStatus presentation_status;
    MeisoCompositionScenePresentationStatus decoded_presentation;
    MeisoCompositionSceneStatus scene_status;
    MeisoCompositionSceneStatus decoded_scene;
    MeisoStateView current;
    MeisoRuntimeActualState decoded_runtime;
    size_t presentation_size = 0U;
    size_t scene_size = 0U;

    CHECK(meiso_composition_coordinator_status(
        fixture->coordinator, &presentation_status
    ));
    CHECK(presentation_status.active_state ==
              MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE &&
          presentation_status.active_plan_generation == 1U);
    CHECK(meiso_composition_scene_presentation_status_encode(
        &presentation_status,
        (MeisoMutableBuffer){
            presentation_suffix, sizeof(presentation_suffix)
        },
        &presentation_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_actual_state(
        &fixture->presentation_actual, 1U, 2U, 41U,
        (MeisoByteView){presentation_suffix, presentation_size}
    ));
    CHECK(deliver_state_over_scheduler(
        &fixture->tcp,
        &fixture->presentation_actual,
        false,
        EDGE_PRESENTATION_ACTUAL,
        HOST_PRESENTATION_ACTUAL_ACCEPT,
        &current
    ));
    CHECK(meiso_runtime_actual_state_decode(
        current.payload, &decoded_runtime
    ) && decoded_runtime.production_epoch == 41U &&
          decoded_runtime.applied_desired.version == 2U);
    CHECK(meiso_composition_scene_presentation_status_decode(
        decoded_runtime.profile, &decoded_presentation
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(decoded_presentation.active_state ==
              MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE &&
          decoded_presentation.active_plan_generation == 1U);

    CHECK(meiso_composition_scene_store_status(
        fixture->scene_store, &usage, &scene_status
    ));
    CHECK(scene_status.reason == MEISO_COMPOSITION_SCENE_REASON_READY &&
          scene_status.active_plan_generation == 1U &&
          scene_status.world_generation == 1U &&
          scene_status.active_object_count == 1U &&
          scene_status.published_blob_count == 1U &&
          scene_status.published_blob_bytes == cube_size);
    CHECK(meiso_composition_scene_status_encode(
        &scene_status,
        (MeisoMutableBuffer){scene_suffix, sizeof(scene_suffix)},
        &scene_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_actual_state(
        &fixture->scene_actual, 1U, 1U, 42U,
        (MeisoByteView){scene_suffix, scene_size}
    ));
    CHECK(deliver_state_over_scheduler(
        &fixture->tcp,
        &fixture->scene_actual,
        false,
        EDGE_SCENE_ACTUAL,
        HOST_SCENE_ACTUAL_ACCEPT,
        &current
    ));
    CHECK(meiso_runtime_actual_state_decode(
        current.payload, &decoded_runtime
    ) && decoded_runtime.production_epoch == 42U &&
          decoded_runtime.applied_desired.version == 1U);
    CHECK(meiso_composition_scene_status_decode(
        decoded_runtime.profile, &decoded_scene
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(decoded_scene.world_generation == 1U &&
          decoded_scene.active_object_count == 1U &&
          decoded_scene.published_blob_count == 1U);
    return true;
}

static bool assert_independent_production_gates(SemanticFixture *fixture)
{
    MeisoRuntimeProductionSnapshot host_wifi;
    MeisoRuntimeProductionSnapshot edge_wifi;
    MeisoRuntimeProductionSnapshot host_scene;
    MeisoRuntimeProductionSnapshot edge_scene;
    MeisoRuntimeProductionSnapshot host_presentation;

    CHECK(meiso_runtime_production_snapshot(
        fixture->tcp.host.runtime,
        *meiso_wifi_telemetry_endpoint(),
        &host_wifi
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_production_snapshot(
        fixture->tcp.edge.runtime,
        *meiso_wifi_telemetry_endpoint(),
        &edge_wifi
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_production_snapshot(
        fixture->tcp.host.runtime,
        *meiso_composition_scene_scene_endpoint(),
        &host_scene
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_production_snapshot(
        fixture->tcp.edge.runtime,
        *meiso_composition_scene_scene_endpoint(),
        &edge_scene
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_production_snapshot(
        fixture->tcp.host.runtime,
        *meiso_composition_scene_presentation_endpoint(),
        &host_presentation
    ) == MEISO_RUNTIME_OK);
    CHECK(host_wifi.data_active && edge_wifi.data_active &&
          host_wifi.production_epoch == 77U &&
          edge_wifi.production_epoch == 77U &&
          host_scene.data_active && edge_scene.data_active &&
          host_scene.production_epoch == 42U &&
          edge_scene.production_epoch == 42U &&
          host_presentation.production_epoch == 41U &&
          host_wifi.production_epoch != host_scene.production_epoch &&
          host_scene.applied_desired.version == 1U &&
          host_presentation.applied_desired.version == 2U);
    return true;
}

static bool send_wifi_sample_over_scheduler(SemanticFixture *fixture)
{
    const MeisoWifiTelemetryFieldMask fields =
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
        MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM;
    TxSource *source = &fixture->tcp.edge_sources[EDGE_WIFI_STREAM];
    MeisoWifiTelemetrySample value;
    MeisoWifiTelemetrySample decoded;
    MeisoStreamSample sample;
    MeisoStreamSample queued;
    MeisoStreamReceiveResult receive;
    MeisoRuntimeEvent event;
    uint8_t payload[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    size_t payload_size = 0U;

    memset(&value, 0, sizeof(value));
    value.selected_fields = fields;
    value.present_fields = fields;
    value.link_state = true;
    value.signal_mbm = -4800;
    CHECK(meiso_wifi_telemetry_sample_encode(
        &value,
        (MeisoMutableBuffer){payload, sizeof(payload)},
        &payload_size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(payload_size == sizeof(fixture->expected_wifi_sample));
    memcpy(fixture->expected_wifi_sample, payload, payload_size);
    fixture->expected_wifi_sample_size = payload_size;
    memset(&sample, 0, sizeof(sample));
    sample.key = (MeisoStreamKey){EDGE_OWNER, 177U, 1U};
    sample.source_time = (MeisoTimePoint){1U, 1000U};
    sample.payload = (MeisoByteView){payload, payload_size};
    source->work.value.stream_sample.production_epoch = 77U;
    source->work.value.stream_sample.sample = sample;
    CHECK(dispatch_source(
        &fixture->tcp.edge, &fixture->tcp.host, source, NULL, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          route_key_equal(
              event.route, route_key(&fixture->tcp, ROUTE_WIFI_DATA)
          ) &&
          event.production_epoch == 77U &&
          event.stream_sample.key.epoch == 177U &&
          event.stream_sample.key.sequence == 1U);
    CHECK(meiso_wifi_telemetry_sample_decode(
        event.stream_sample.payload, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(decoded.link_state && decoded.signal_mbm == -4800);
    receive = meiso_stream_receive(
        fixture->wifi_stream, &event.stream_sample, NULL
    );
    CHECK(receive.code == MEISO_STREAM_RECEIVE_ACCEPTED &&
          meiso_stream_count(fixture->wifi_stream) == 1U &&
          meiso_stream_peek(fixture->wifi_stream, &queued) &&
          queued.key.epoch == 177U && queued.key.sequence == 1U);
    return true;
}

static bool send_render_result_over_scheduler(
    SemanticFixture *fixture,
    const MeisoCompositionSceneWorldRef *world
)
{
    TxSource *source = &fixture->tcp.edge_sources[EDGE_RENDER_STREAM];
    MeisoCompositionSceneRenderResult result;
    MeisoCompositionSceneRenderResult decoded;
    MeisoStreamSample sample;
    MeisoStreamSample queued;
    MeisoStreamSample wifi_queued;
    MeisoWifiTelemetrySample wifi_decoded;
    MeisoStreamReceiveResult receive;
    MeisoRuntimeEvent event;
    uint8_t payload[MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE];
    size_t payload_size = 0U;

    CHECK(meiso_composition_scene_store_fake_render(
        fixture->scene_store, 77U, 200U, 210U, &result
    ) == MEISO_COMPOSITION_SCENE_RENDER_COMPLETE);
    CHECK(result.applied_scene_owner_incarnation ==
              world->desired_owner_incarnation &&
          result.applied_scene_state_version ==
              world->desired_state_version &&
          result.active_plan_generation == world->plan_generation &&
          result.world_generation == world->world_generation);
    CHECK(meiso_composition_scene_render_result_encode(
        &result,
        (MeisoMutableBuffer){payload, sizeof(payload)},
        &payload_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    memset(&sample, 0, sizeof(sample));
    sample.key = (MeisoStreamKey){EDGE_OWNER, 417U, 1U};
    sample.source_time = composition_time(210U);
    sample.payload = (MeisoByteView){payload, payload_size};
    source->work.value.stream_sample.production_epoch = 42U;
    source->work.value.stream_sample.sample = sample;
    CHECK(dispatch_source(
        &fixture->tcp.edge, &fixture->tcp.host, source, NULL, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          route_key_equal(
              event.route, route_key(&fixture->tcp, ROUTE_RENDER_RESULT)
          ) &&
          event.production_epoch == 42U &&
          event.stream_sample.key.epoch == 417U &&
          event.stream_sample.key.sequence == 1U);
    CHECK(meiso_composition_scene_render_result_decode(
        event.stream_sample.payload, &decoded
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(decoded.applied_scene_owner_incarnation ==
              world->desired_owner_incarnation &&
          decoded.applied_scene_state_version ==
              world->desired_state_version &&
          decoded.active_plan_generation == world->plan_generation &&
          decoded.world_generation == world->world_generation);
    receive = meiso_stream_receive(
        fixture->render_stream, &event.stream_sample, NULL
    );
    CHECK(receive.code == MEISO_STREAM_RECEIVE_ACCEPTED &&
          meiso_stream_count(fixture->render_stream) == 1U &&
          meiso_stream_peek(fixture->render_stream, &queued) &&
          queued.key.epoch == 417U && queued.key.sequence == 1U);
    CHECK(meiso_composition_scene_store_record_result_published(
        fixture->scene_store, world, 417U, 1U
    ));
    CHECK(meiso_stream_count(fixture->wifi_stream) == 1U &&
          meiso_stream_peek(fixture->wifi_stream, &wifi_queued) &&
          wifi_queued.key.epoch == 177U &&
          wifi_queued.key.sequence == 1U &&
          wifi_queued.payload.size == fixture->expected_wifi_sample_size &&
          memcmp(
              wifi_queued.payload.data,
              fixture->expected_wifi_sample,
              fixture->expected_wifi_sample_size
          ) == 0 && queued.key.epoch != wifi_queued.key.epoch);
    CHECK(meiso_wifi_telemetry_sample_decode(
        wifi_queued.payload, &wifi_decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(wifi_decoded.selected_fields ==
              (MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
               MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM) &&
          wifi_decoded.present_fields == wifi_decoded.selected_fields &&
          wifi_decoded.link_state && wifi_decoded.signal_mbm == -4800);
    return true;
}

static bool test_r2b0_loopback_catalog_scheduler(void)
{
    TcpFixture fixture;
    MeisoRuntimeSchedulerSnapshot host_scheduler;
    MeisoRuntimeSchedulerSnapshot edge_scheduler;

    CHECK(initialize_tcp_fixture(&fixture));
    CHECK(meiso_runtime_scheduler_snapshot(
        fixture.host.scheduler, &host_scheduler
    ));
    CHECK(meiso_runtime_scheduler_snapshot(
        fixture.edge.scheduler, &edge_scheduler
    ));
    CHECK(host_scheduler.phase == MEISO_RUNTIME_SCHEDULER_PHASE_READY &&
          edge_scheduler.phase == MEISO_RUNTIME_SCHEDULER_PHASE_READY &&
          host_scheduler.catalog_generation == 1U &&
          edge_scheduler.catalog_generation == 1U &&
          host_scheduler.control_source_count == 8U &&
          host_scheduler.data_source_count == 1U &&
          edge_scheduler.control_source_count == 7U &&
          edge_scheduler.data_source_count == 2U);
    destroy_tcp_fixture(&fixture);
    return true;
}

static bool test_composite_semantic_loop(void)
{
    SemanticFixture fixture;
    uint8_t cube_storage[512];
    MeisoByteView cube;
    MeisoContentId cube_id;
    MeisoCompositionSceneWorldRef world;
    MeisoRuntimeSchedulerSnapshot host_scheduler;
    MeisoRuntimeSchedulerSnapshot edge_scheduler;

    CHECK(initialize_semantic_fixture(&fixture));
    CHECK(deliver_wifi_desired(&fixture));
    CHECK(publish_cube_over_scheduler(
        &fixture, cube_storage, &cube, &cube_id
    ));
    CHECK(deliver_scene_desired(&fixture, cube_id));
    CHECK(activate_plan_over_state(&fixture, &world));
    CHECK(deliver_wifi_actual(&fixture));
    CHECK(deliver_composition_actuals(&fixture, cube.size));
    CHECK(assert_independent_production_gates(&fixture));
    CHECK(send_wifi_sample_over_scheduler(&fixture));
    CHECK(send_render_result_over_scheduler(&fixture, &world));
    CHECK(meiso_runtime_scheduler_snapshot(
        fixture.tcp.host.scheduler, &host_scheduler
    ));
    CHECK(meiso_runtime_scheduler_snapshot(
        fixture.tcp.edge.scheduler, &edge_scheduler
    ));
    CHECK(host_scheduler.successful_control_slots > 0U &&
          host_scheduler.successful_data_slots == 1U &&
          edge_scheduler.successful_control_slots > 0U &&
          edge_scheduler.successful_data_slots == 2U &&
          host_scheduler.catalog_generation ==
              edge_scheduler.catalog_generation &&
          host_scheduler.catalog_generation == 1U &&
          fixture.tcp.host.runtime_frames_with_multi_write_progress > 0U &&
          fixture.tcp.edge.runtime_frames_with_multi_write_progress > 0U &&
          fixture.tcp.host.runtime_frames_with_multi_read_progress > 0U &&
          fixture.tcp.edge.runtime_frames_with_multi_read_progress > 0U);
    destroy_semantic_fixture(&fixture);
    return true;
}

int main(void)
{
    if (!test_r2b0_loopback_catalog_scheduler()) {
        return 1;
    }
    if (!test_composite_semantic_loop()) {
        return 1;
    }
    (void)printf("composition Runtime TCP tests passed\n");
    return 0;
}
