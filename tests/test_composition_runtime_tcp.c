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
    TEST_SCENE_STORE_SNAPSHOT_BYTES = 40000,
    TEST_IO_BYTES_PER_STEP = 17,
    TEST_POLL_SLICE_MS = 20,
    TEST_DEADLINE_MS = 5000,
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
    HOST_BLOB_COMMIT = 8,
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
    MeisoRuntimeSchedulerBoundary last_boundary;
    uint64_t last_boundary_generation;
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
    if (source != NULL) {
        source->boundaries++;
        source->last_boundary = boundary;
        source->last_boundary_generation = generation;
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

static MeisoRuntimeRouteKey route_key_at_generation(
    const TcpFixture *fixture,
    size_t index,
    uint64_t catalog_generation
)
{
    const MeisoRuntimeRouteDescriptor *route = &fixture->routes[index];
    const MeisoRuntimeRouteKey key = {
        catalog_generation,
        route->endpoint_id,
        route->endpoint_incarnation,
        route->resource.id,
        route->facet
    };

    return key;
}

static MeisoRuntimeRouteKey route_key(
    const TcpFixture *fixture,
    size_t index
)
{
    return route_key_at_generation(
        fixture, index, fixture->catalog_generation
    );
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

static bool set_source_work_route(
    TxSource *source,
    MeisoRuntimeRouteKey route
)
{
    if (source == NULL) {
        return false;
    }
    switch (source->work.kind) {
    case MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER:
        source->work.value.state_offer.route = route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT:
        source->work.value.state_accept.route = route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE:
        source->work.value.stream_sample.route = route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_BEGIN:
        source->work.value.blob_begin.route = route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK:
        source->work.value.blob_chunk.transfer.route = route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_COMMIT:
        source->work.value.blob_commit.transfer.route = route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT:
        source->work.value.blob_result.transfer.route = route;
        return true;
    default:
        return false;
    }
}

static bool register_peer_sources(
    TcpFixture *fixture,
    Peer *peer,
    bool host
)
{
    const uint16_t max_control = host ? 9U : 8U;
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
        } else if (work->kind == MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT) {
            route = work->value.blob_result.transfer.route;
        } else {
            continue;
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

static bool same_ready_session_identity(
    const MeisoSessionSnapshot *before,
    const MeisoSessionSnapshot *after
)
{
    return before != NULL && after != NULL &&
        before->state == MEISO_SESSION_STATE_READY &&
        after->state == MEISO_SESSION_STATE_READY &&
        before->failure == MEISO_SESSION_FAILURE_NONE &&
        after->failure == MEISO_SESSION_FAILURE_NONE &&
        before->local_role == after->local_role &&
        meiso_peer_id_equal(before->local_peer_id, after->local_peer_id) &&
        before->local_incarnation == after->local_incarnation &&
        meiso_peer_id_equal(before->remote_peer_id, after->remote_peer_id) &&
        before->remote_incarnation == after->remote_incarnation &&
        meiso_session_id_equal(before->session_id, after->session_id) &&
        before->session_id_known && after->session_id_known &&
        before->remote_identity_known && after->remote_identity_known &&
        before->negotiated_limits.max_frame_bytes ==
            after->negotiated_limits.max_frame_bytes &&
        before->negotiated_limits.max_runtime_payload_bytes ==
            after->negotiated_limits.max_runtime_payload_bytes &&
        before->negotiated_limits.max_registry_entries ==
            after->negotiated_limits.max_registry_entries &&
        !before->outbound_pending && !after->outbound_pending &&
        !before->local_close_reason_known &&
        !after->local_close_reason_known &&
        !before->peer_close_reason_known &&
        !after->peer_close_reason_known;
}

static bool rebind_peer_after_catalog(
    TcpFixture *fixture,
    Peer *peer,
    bool host,
    uint64_t old_generation,
    uint64_t new_generation
)
{
    MeisoRuntimeSchedulerStepResult step;
    MeisoRuntimeSchedulerSnapshot snapshot;
    uint64_t boundaries_before[TEST_HOST_SOURCE_COUNT];
    size_t index;

    CHECK(peer->source_count <= TEST_HOST_SOURCE_COUNT);
    for (index = 0U; index < peer->source_count; ++index) {
        boundaries_before[index] = peer->sources[index].boundaries;
    }
    memset(&step, 0xa5, sizeof(step));
    CHECK(meiso_runtime_scheduler_step(peer->scheduler, &step) ==
          MEISO_RUNTIME_SCHEDULER_STEP_REBIND_REQUIRED);
    for (index = 0U; index < peer->source_count; ++index) {
        CHECK(peer->sources[index].boundaries ==
                  boundaries_before[index] + 1U &&
              peer->sources[index].last_boundary ==
                  MEISO_RUNTIME_SCHEDULER_BOUNDARY_CATALOG_CHANGED &&
              peer->sources[index].last_boundary_generation ==
                  new_generation);
    }
    memset(&step, 0xa5, sizeof(step));
    CHECK(meiso_runtime_scheduler_step(peer->scheduler, &step) ==
          MEISO_RUNTIME_SCHEDULER_STEP_REBIND_REQUIRED);
    for (index = 0U; index < peer->source_count; ++index) {
        CHECK(peer->sources[index].boundaries ==
              boundaries_before[index] + 1U);
    }
    CHECK(meiso_runtime_scheduler_finish_rebind(peer->scheduler) ==
          MEISO_RUNTIME_SCHEDULER_REBIND_REQUIRED);
    for (index = 0U; index < peer->source_count; ++index) {
        size_t route_index;
        MeisoRuntimeRouteKey old_route;
        MeisoRuntimeRouteKey new_route;

        if (!source_route_index(host, index, &route_index)) {
            continue;
        }
        old_route = route_key_at_generation(
            fixture, route_index, old_generation
        );
        new_route = route_key_at_generation(
            fixture, route_index, new_generation
        );
        CHECK(meiso_runtime_scheduler_bind_source_route(
            peer->scheduler, peer->sources[index].id, old_route
        ) == MEISO_RUNTIME_SCHEDULER_INVALID_STATE);
        CHECK(set_source_work_route(&peer->sources[index], new_route));
        CHECK(meiso_runtime_scheduler_bind_source_route(
            peer->scheduler, peer->sources[index].id, new_route
        ) == MEISO_RUNTIME_SCHEDULER_OK);
    }
    CHECK(meiso_runtime_scheduler_finish_rebind(peer->scheduler) ==
          MEISO_RUNTIME_SCHEDULER_OK);
    CHECK(meiso_runtime_scheduler_snapshot(peer->scheduler, &snapshot) &&
          snapshot.phase == MEISO_RUNTIME_SCHEDULER_PHASE_READY &&
          snapshot.catalog_generation == new_generation);
    return true;
}

static bool reject_stale_queued_route(
    TcpFixture *fixture,
    Peer *peer,
    TxSource *source,
    size_t route_index,
    uint64_t old_generation
)
{
    MeisoRuntimeSchedulerStepResult step;
    MeisoSessionSnapshot before;
    MeisoSessionSnapshot after;
    const uint64_t completions_before = source->completions;
    const MeisoRuntimeRouteKey active_route = route_key(fixture, route_index);

    CHECK(meiso_session_snapshot(peer->session, &before) &&
          !before.outbound_pending);
    CHECK(set_source_work_route(
        source,
        route_key_at_generation(fixture, route_index, old_generation)
    ));
    source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    memset(&step, 0xa5, sizeof(step));
    CHECK(meiso_runtime_scheduler_step(peer->scheduler, &step) ==
          MEISO_RUNTIME_SCHEDULER_STEP_SOURCE_INVALID);
    CHECK(source->completions == completions_before + 1U &&
          source->poll_status == MEISO_RUNTIME_SCHEDULER_POLL_EMPTY &&
          source->last_completion.source_id == source->id &&
          source->last_completion.kind == source->work.kind &&
          source->last_completion.code ==
              MEISO_RUNTIME_SCHEDULER_COMPLETION_REJECTED &&
          source->last_completion.runtime_status ==
              MEISO_RUNTIME_INVALID_ARGUMENT &&
          meiso_session_snapshot(peer->session, &after) &&
          !after.outbound_pending);
    CHECK(set_source_work_route(source, active_route));
    return true;
}

static bool replace_catalog_generation(
    TcpFixture *fixture,
    uint64_t new_generation
)
{
    TxSource *offer = &fixture->edge_sources[EDGE_CATALOG_OFFER];
    TxSource *accept = &fixture->host_sources[HOST_CATALOG_ACCEPT];
    MeisoRuntimeEvent event;
    MeisoRuntimeSnapshot host_runtime;
    MeisoRuntimeSnapshot edge_runtime;
    MeisoRuntimeSchedulerSnapshot host_scheduler;
    MeisoRuntimeSchedulerSnapshot edge_scheduler;
    MeisoSessionSnapshot host_before;
    MeisoSessionSnapshot edge_before;
    MeisoSessionSnapshot host_after;
    MeisoSessionSnapshot edge_after;
    MeisoSession *const host_session = fixture->host.session;
    MeisoSession *const edge_session = fixture->edge.session;
    const int host_fd = fixture->host.fd;
    const int edge_fd = fixture->edge.fd;
    const uint64_t old_generation = fixture->catalog_generation;
    uint64_t host_boundaries_before[TEST_HOST_SOURCE_COUNT] = {0U};
    uint64_t edge_boundaries_before[TEST_EDGE_SOURCE_COUNT] = {0U};
    size_t index;

    CHECK(new_generation > old_generation &&
          meiso_session_snapshot(fixture->host.session, &host_before) &&
          meiso_session_snapshot(fixture->edge.session, &edge_before));
    for (index = 0U; index < fixture->host.source_count; ++index) {
        host_boundaries_before[index] =
            fixture->host.sources[index].boundaries;
    }
    for (index = 0U; index < fixture->edge.source_count; ++index) {
        edge_boundaries_before[index] =
            fixture->edge.sources[index].boundaries;
    }
    offer->work.value.catalog_offer.generation = new_generation;
    offer->work.value.catalog_offer.endpoints = fixture->endpoints;
    offer->work.value.catalog_offer.endpoint_count =
        MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT;
    offer->work.value.catalog_offer.routes = fixture->routes;
    offer->work.value.catalog_offer.route_count =
        MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT;
    CHECK(dispatch_source(
        &fixture->edge, &fixture->host, offer, NULL, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED &&
          event.catalog_generation == new_generation);
    CHECK(meiso_runtime_snapshot(fixture->host.runtime, &host_runtime) &&
          meiso_runtime_snapshot(fixture->edge.runtime, &edge_runtime) &&
          host_runtime.active_catalog_generation == old_generation &&
          edge_runtime.active_catalog_generation == old_generation &&
          host_runtime.active_endpoint_count == 3U &&
          edge_runtime.active_endpoint_count == 3U &&
          host_runtime.active_route_count == 9U &&
          edge_runtime.active_route_count == 9U &&
          host_runtime.staged_catalog_present &&
          edge_runtime.staged_catalog_present &&
          host_runtime.staged_catalog_generation == new_generation &&
          edge_runtime.staged_catalog_generation == new_generation &&
          host_runtime.staged_endpoint_count == 3U &&
          edge_runtime.staged_endpoint_count == 3U &&
          host_runtime.staged_route_count == 9U &&
          edge_runtime.staged_route_count == 9U &&
          host_runtime.catalog_decision_pending &&
          edge_runtime.catalog_decision_pending &&
          host_runtime.pending_catalog_generation == new_generation &&
          edge_runtime.pending_catalog_generation == new_generation &&
          meiso_runtime_scheduler_snapshot(
              fixture->host.scheduler, &host_scheduler
          ) && meiso_runtime_scheduler_snapshot(
              fixture->edge.scheduler, &edge_scheduler
          ) && host_scheduler.phase ==
              MEISO_RUNTIME_SCHEDULER_PHASE_READY &&
          edge_scheduler.phase == MEISO_RUNTIME_SCHEDULER_PHASE_READY &&
          host_scheduler.catalog_generation == old_generation &&
          edge_scheduler.catalog_generation == old_generation);
    for (index = 0U; index < fixture->host.source_count; ++index) {
        CHECK(fixture->host.sources[index].boundaries ==
              host_boundaries_before[index]);
    }
    for (index = 0U; index < fixture->edge.source_count; ++index) {
        CHECK(fixture->edge.sources[index].boundaries ==
              edge_boundaries_before[index]);
    }
    CHECK(dispatch_source(
        &fixture->host, &fixture->edge, accept, NULL, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED &&
          event.catalog_generation == new_generation);
    fixture->catalog_generation = new_generation;
    CHECK(meiso_runtime_snapshot(fixture->host.runtime, &host_runtime) &&
          meiso_runtime_snapshot(fixture->edge.runtime, &edge_runtime) &&
          host_runtime.active_catalog_present &&
          edge_runtime.active_catalog_present &&
          host_runtime.active_catalog_generation == new_generation &&
          edge_runtime.active_catalog_generation == new_generation &&
          host_runtime.active_endpoint_count == 3U &&
          edge_runtime.active_endpoint_count == 3U &&
          host_runtime.active_route_count == 9U &&
          edge_runtime.active_route_count == 9U);
    CHECK(rebind_peer_after_catalog(
        fixture, &fixture->host, true, old_generation, new_generation
    ));
    CHECK(rebind_peer_after_catalog(
        fixture, &fixture->edge, false, old_generation, new_generation
    ));
    CHECK(reject_stale_queued_route(
        fixture,
        &fixture->host,
        &fixture->host_sources[HOST_WIFI_DESIRED],
        ROUTE_WIFI_DESIRED,
        old_generation
    ));
    CHECK(reject_stale_queued_route(
        fixture,
        &fixture->edge,
        &fixture->edge_sources[EDGE_WIFI_ACTUAL],
        ROUTE_WIFI_ACTUAL,
        old_generation
    ));
    CHECK(meiso_session_snapshot(fixture->host.session, &host_after) &&
          meiso_session_snapshot(fixture->edge.session, &edge_after) &&
          same_ready_session_identity(&host_before, &host_after) &&
          same_ready_session_identity(&edge_before, &edge_after) &&
          fixture->host.session == host_session &&
          fixture->edge.session == edge_session &&
          fixture->host.fd == host_fd && fixture->edge.fd == edge_fd);
    return true;
}

static bool initialize_tcp_fixture_with_nonce_salt(
    TcpFixture *fixture,
    uint64_t nonce_salt
)
{
    MeisoSessionConfig host_config = session_config(
        MEISO_SESSION_ROLE_HOST, nonce_salt
    );
    MeisoSessionConfig edge_config = session_config(
        MEISO_SESSION_ROLE_EDGE, nonce_salt
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

static bool initialize_tcp_fixture(TcpFixture *fixture)
{
    return initialize_tcp_fixture_with_nonce_salt(fixture, 0U);
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

static bool restart_state_delivery(StateChannel *channel, uint64_t increment)
{
    MeisoDeliveryGeneration next;

    CHECK(channel != NULL && increment != 0U &&
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

static bool restart_all_state_deliveries(SemanticFixture *fixture)
{
    CHECK(restart_state_delivery(&fixture->wifi_desired, 100U));
    CHECK(restart_state_delivery(&fixture->wifi_actual, 100U));
    CHECK(restart_state_delivery(&fixture->scene_desired, 100U));
    CHECK(restart_state_delivery(&fixture->plan_desired, 100U));
    CHECK(restart_state_delivery(&fixture->presentation_actual, 100U));
    CHECK(restart_state_delivery(&fixture->scene_actual, 100U));
    return true;
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

static bool republish_cube_duplicate_over_scheduler(
    SemanticFixture *fixture,
    MeisoByteView cube,
    const MeisoContentId *content_id,
    uint64_t expected_transfer_id
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
    MeisoBlobStats before;
    MeisoBlobStats after;

    CHECK(content_id != NULL && cube.data != NULL && cube.size != 0U &&
          meiso_blob_store_stats(fixture->blob_store, &before) &&
          meiso_blob_store_count(fixture->blob_store) == 1U);
    begin->work.value.blob_begin.content_id = *content_id;
    begin->work.value.blob_begin.total_size = cube.size;
    CHECK(dispatch_source(
        &fixture->tcp.host, &fixture->tcp.edge, begin,
        &begin_step, &edge_event
    ));
    CHECK(begin_step.completion.generated_blob_transfer_present &&
          edge_event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          edge_event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN &&
          route_key_equal(
              edge_event.route,
              route_key(&fixture->tcp, ROUTE_MESH_BLOB)
          ));
    transfer = begin_step.completion.generated_blob_transfer;
    CHECK(transfer.transfer_id == expected_transfer_id &&
          edge_event.blob_transfer.transfer_id == transfer.transfer_id &&
          edge_event.blob_transfer.total_size == cube.size);

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
    chunk->work.value.blob_chunk.payload = cube;
    CHECK(dispatch_source(
        &fixture->tcp.host, &fixture->tcp.edge, chunk, NULL, &edge_event
    ));
    CHECK(edge_event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          edge_event.blob_operation == MEISO_RUNTIME_BLOB_CHUNK &&
          edge_event.blob_offset == 0U &&
          edge_event.blob_payload.size == cube.size &&
          memcmp(edge_event.blob_payload.data, cube.data, cube.size) == 0);

    commit->work.value.blob_commit.transfer = transfer;
    CHECK(dispatch_source(
        &fixture->tcp.host, &fixture->tcp.edge, commit, NULL, &edge_event
    ));
    CHECK(edge_event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          edge_event.blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          edge_event.blob_payload.size == cube.size &&
          memcmp(edge_event.blob_payload.data, cube.data, cube.size) == 0);
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
          report.commit_status == MEISO_BLOB_COMMIT_DUPLICATE &&
          report.result == MEISO_RUNTIME_BLOB_RESULT_DUPLICATE &&
          meiso_blob_store_count(fixture->blob_store) == 1U &&
          meiso_blob_store_stats(fixture->blob_store, &after) &&
          after.publications == before.publications &&
          after.duplicates == before.duplicates + 1U);
    CHECK(meiso_composition_scene_store_mark_blob_visible(
        fixture->scene_store,
        HOST_OWNER.incarnation,
        content_id,
        cube.size
    ) == MEISO_COMPOSITION_SCENE_BLOB_VISIBLE);

    result->work.value.blob_result.transfer = transfer;
    result->work.value.blob_result.operation = MEISO_RUNTIME_BLOB_COMMIT;
    CHECK(dispatch_source(
        &fixture->tcp.edge, &fixture->tcp.host, result, NULL, &host_event
    ));
    CHECK(host_event.type == MEISO_RUNTIME_EVENT_BLOB_RESULT &&
          host_event.blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          host_event.blob_result == MEISO_RUNTIME_BLOB_RESULT_DUPLICATE &&
          host_event.blob_next_offset == cube.size);
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

static bool publish_desired_state_expect(
    StateChannel *channel,
    uint64_t version,
    MeisoByteView profile,
    MeisoStatePublishStatus expected_status
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
    ) == expected_status);
    return true;
}

static bool publish_desired_state(
    StateChannel *channel,
    uint64_t version,
    MeisoByteView profile
)
{
    return publish_desired_state_expect(
        channel, version, profile, MEISO_STATE_PUBLISH_NEW
    );
}

static bool publish_actual_state_expect(
    StateChannel *channel,
    uint64_t version,
    uint64_t desired_version,
    uint64_t production_epoch,
    MeisoByteView profile,
    MeisoStatePublishStatus expected_status
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
    ) == expected_status);
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
    return publish_actual_state_expect(
        channel,
        version,
        desired_version,
        production_epoch,
        profile,
        MEISO_STATE_PUBLISH_NEW
    );
}

static bool publish_stopped_actual_state(
    StateChannel *channel,
    uint64_t version,
    uint64_t desired_version,
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
    actual.effective_enabled = false;
    actual.production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
    actual.reason = MEISO_RUNTIME_PRODUCTION_REASON_DISABLED;
    actual.production_epoch = 0U;
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

static MeisoCompositionScenePresentationPlan plan_value_at_generation(
    uint64_t plan_generation,
    uint64_t capability_generation
)
{
    MeisoCompositionScenePresentationPlan plan;
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE];

    memset(&plan, 0, sizeof(plan));
    plan.plan_generation = plan_generation;
    plan.capability_generation = capability_generation;
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

static MeisoCompositionScenePresentationPlan plan_value(void)
{
    return plan_value_at_generation(1U, 1U);
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

static bool deliver_wifi_desired_version(
    SemanticFixture *fixture,
    uint64_t version,
    MeisoStatePublishStatus expected_publish
)
{
    const MeisoWifiTelemetryDesiredParameters parameters = {
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
            MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM,
        500U
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
    CHECK(publish_desired_state_expect(
        &fixture->wifi_desired, version,
        (MeisoByteView){suffix, suffix_size},
        expected_publish
    ));
    CHECK(deliver_state_over_scheduler(
        &fixture->tcp,
        &fixture->wifi_desired,
        true,
        HOST_WIFI_DESIRED,
        EDGE_WIFI_DESIRED_ACCEPT,
        &current
    ));
    CHECK(current.version == version &&
          meiso_owner_scope_equal(current.owner, HOST_OWNER) &&
          meiso_runtime_desired_state_decode(
              current.payload, &decoded_runtime
          ) && decoded_runtime.enabled &&
          meiso_wifi_telemetry_desired_parameters_decode(
              decoded_runtime.profile, &decoded_parameters
          ) == MEISO_WIFI_TELEMETRY_CODEC_OK &&
          decoded_parameters.selected_fields == parameters.selected_fields &&
          decoded_parameters.sample_period_ms == parameters.sample_period_ms);
    return true;
}

static bool deliver_scene_state_without_prepare(
    SemanticFixture *fixture,
    uint64_t version,
    const MeisoCompositionSceneDesired *scene,
    MeisoCompositionSceneDesired *out_decoded,
    MeisoStatePublishStatus expected_publish
)
{
    uint8_t suffix[MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE];
    uint8_t canonical_suffix[MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE];
    MeisoStateView current;
    MeisoRuntimeDesiredState decoded_runtime;
    size_t suffix_size = 0U;
    size_t canonical_size = 0U;

    CHECK(out_decoded != NULL);
    CHECK(meiso_composition_scene_desired_encode(
        scene,
        (MeisoMutableBuffer){suffix, sizeof(suffix)},
        &suffix_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_desired_state_expect(
        &fixture->scene_desired, version,
        (MeisoByteView){suffix, suffix_size},
        expected_publish
    ));
    CHECK(deliver_state_over_scheduler(
        &fixture->tcp,
        &fixture->scene_desired,
        true,
        HOST_SCENE_DESIRED,
        EDGE_SCENE_DESIRED_ACCEPT,
        &current
    ));
    CHECK(current.version == version &&
          meiso_owner_scope_equal(current.owner, HOST_OWNER) &&
          meiso_runtime_desired_state_decode(
              current.payload, &decoded_runtime
          ) && decoded_runtime.enabled);
    CHECK(meiso_composition_scene_desired_decode(
        decoded_runtime.profile, out_decoded
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(meiso_composition_scene_desired_encode(
        out_decoded,
        (MeisoMutableBuffer){canonical_suffix, sizeof(canonical_suffix)},
        &canonical_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(canonical_size == suffix_size &&
          memcmp(canonical_suffix, suffix, suffix_size) == 0);
    return true;
}

static bool deliver_scene_snapshot(
    SemanticFixture *fixture,
    uint64_t version,
    const MeisoCompositionSceneDesired *scene
)
{
    uint8_t suffix[MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE];
    uint8_t canonical_suffix[MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE];
    MeisoStateView current;
    MeisoRuntimeDesiredState decoded_runtime;
    MeisoCompositionSceneDesired decoded_scene;
    size_t suffix_size = 0U;
    size_t canonical_size = 0U;

    CHECK(meiso_composition_scene_desired_encode(
        scene,
        (MeisoMutableBuffer){suffix, sizeof(suffix)},
        &suffix_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_desired_state(
        &fixture->scene_desired, version,
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
    CHECK(current.version == version &&
          meiso_runtime_desired_state_decode(
              current.payload, &decoded_runtime
          ) && decoded_runtime.enabled);
    CHECK(meiso_composition_scene_desired_decode(
        decoded_runtime.profile, &decoded_scene
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(meiso_composition_scene_desired_encode(
        &decoded_scene,
        (MeisoMutableBuffer){canonical_suffix, sizeof(canonical_suffix)},
        &canonical_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(canonical_size == suffix_size &&
          memcmp(canonical_suffix, suffix, suffix_size) == 0);
    CHECK(decoded_scene.object_count == scene->object_count &&
          decoded_scene.camera.position[0] == scene->camera.position[0]);
    if (scene->object_count != 0U) {
        CHECK(memcmp(
            &decoded_scene.objects[0].mesh_content_id,
            &scene->objects[0].mesh_content_id,
            sizeof(scene->objects[0].mesh_content_id)
        ) == 0);
    }
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
    MeisoCompositionSceneWorldRef *out_world,
    uint8_t out_token[MEISO_SHA256_DIGEST_SIZE]
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
    if (out_token != NULL) {
        memcpy(out_token, token, sizeof(token));
    }
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

static bool deliver_scene_actual_snapshot(
    SemanticFixture *fixture,
    uint64_t actual_version,
    uint64_t desired_version,
    uint64_t production_epoch,
    size_t cube_size,
    const MeisoCompositionSceneWorldRef *world,
    uint32_t expected_object_count
)
{
    const MeisoCompositionSceneResourceUsage usage = {
        1U,
        cube_size,
        0U,
        0U,
        0U,
        (uint32_t)meiso_stream_count(fixture->render_stream)
    };
    uint8_t suffix[MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE];
    MeisoCompositionSceneStatus status;
    MeisoCompositionSceneStatus decoded_status;
    MeisoRuntimeActualState decoded_runtime;
    MeisoRuntimeProductionSnapshot host_gate;
    MeisoRuntimeProductionSnapshot edge_gate;
    MeisoStateView current;
    size_t suffix_size = 0U;

    CHECK(meiso_composition_scene_store_status(
        fixture->scene_store, &usage, &status
    ));
    CHECK(status.reason == MEISO_COMPOSITION_SCENE_REASON_READY &&
          status.active_plan_generation == 1U &&
          status.world_generation == world->world_generation &&
          status.active_object_count == expected_object_count &&
          status.unique_mesh_count == 1U &&
          status.published_blob_count == 1U &&
          status.published_blob_bytes == cube_size);
    CHECK(meiso_composition_scene_status_encode(
        &status,
        (MeisoMutableBuffer){suffix, sizeof(suffix)},
        &suffix_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_actual_state(
        &fixture->scene_actual,
        actual_version,
        desired_version,
        production_epoch,
        (MeisoByteView){suffix, suffix_size}
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
    ) && decoded_runtime.production_epoch == production_epoch &&
          decoded_runtime.applied_desired.version == desired_version &&
          decoded_runtime.production_status ==
              MEISO_RUNTIME_PRODUCTION_RUNNING);
    CHECK(meiso_composition_scene_status_decode(
        decoded_runtime.profile, &decoded_status
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(decoded_status.world_generation == world->world_generation &&
          decoded_status.active_object_count == expected_object_count &&
          decoded_status.unique_mesh_count == 1U);
    CHECK(meiso_runtime_production_snapshot(
        fixture->tcp.host.runtime,
        *meiso_composition_scene_scene_endpoint(),
        &host_gate
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_production_snapshot(
        fixture->tcp.edge.runtime,
        *meiso_composition_scene_scene_endpoint(),
        &edge_gate
    ) == MEISO_RUNTIME_OK);
    CHECK(host_gate.data_active && edge_gate.data_active &&
          host_gate.production_epoch == production_epoch &&
          edge_gate.production_epoch == production_epoch &&
          host_gate.applied_desired.version == desired_version &&
          edge_gate.applied_desired.version == desired_version);
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
    const MeisoCompositionSceneWorldRef *world,
    uint64_t production_epoch,
    uint64_t sequence,
    uint64_t render_start,
    uint64_t render_end,
    MeisoCompositionSceneRenderResult *out_result
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
        fixture->scene_store, 77U, render_start, render_end, &result
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
    sample.key = (MeisoStreamKey){EDGE_OWNER, 417U, sequence};
    sample.source_time = composition_time(render_end);
    sample.payload = (MeisoByteView){payload, payload_size};
    source->work.value.stream_sample.production_epoch = production_epoch;
    source->work.value.stream_sample.sample = sample;
    CHECK(dispatch_source(
        &fixture->tcp.edge, &fixture->tcp.host, source, NULL, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          route_key_equal(
              event.route, route_key(&fixture->tcp, ROUTE_RENDER_RESULT)
          ) &&
          event.production_epoch == production_epoch &&
          event.stream_sample.key.epoch == 417U &&
          event.stream_sample.key.sequence == sequence);
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
          receive.flags == MEISO_STREAM_RECEIVE_FLAG_NONE &&
          receive.missing_before == 0U &&
          meiso_stream_count(fixture->render_stream) == 1U &&
          meiso_stream_peek(fixture->render_stream, &queued) &&
          queued.key.epoch == 417U && queued.key.sequence == sequence &&
          queued.payload.size == payload_size &&
          memcmp(queued.payload.data, payload, payload_size) == 0);
    CHECK(meiso_composition_scene_store_record_result_published(
        fixture->scene_store, world, 417U, sequence
    ));
    CHECK(meiso_stream_pop(fixture->render_stream) &&
          meiso_stream_count(fixture->render_stream) == 0U);
    CHECK(meiso_stream_count(fixture->wifi_stream) == 1U &&
          meiso_stream_peek(fixture->wifi_stream, &wifi_queued) &&
          wifi_queued.key.epoch == 177U &&
          wifi_queued.key.sequence == 1U &&
          wifi_queued.payload.size == fixture->expected_wifi_sample_size &&
          memcmp(
              wifi_queued.payload.data,
              fixture->expected_wifi_sample,
              fixture->expected_wifi_sample_size
          ) == 0 && event.stream_sample.key.epoch != wifi_queued.key.epoch);
    CHECK(meiso_wifi_telemetry_sample_decode(
        wifi_queued.payload, &wifi_decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(wifi_decoded.selected_fields ==
              (MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
               MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM) &&
          wifi_decoded.present_fields == wifi_decoded.selected_fields &&
          wifi_decoded.link_state && wifi_decoded.signal_mbm == -4800);
    if (out_result != NULL) {
        *out_result = decoded;
    }
    return true;
}

static bool reject_render_epoch_without_consuming_sequence(
    SemanticFixture *fixture,
    const MeisoCompositionSceneRenderResult *result,
    uint64_t rejected_production_epoch,
    uint64_t sequence,
    MeisoRuntimeStatus expected_status
)
{
    TxSource *source = &fixture->tcp.edge_sources[EDGE_RENDER_STREAM];
    MeisoRuntimeSchedulerStepResult step;
    MeisoStreamSample sample;
    MeisoStreamStats stats_before;
    MeisoStreamStats stats_after;
    uint8_t payload[MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE];
    size_t payload_size = 0U;

    CHECK(meiso_composition_scene_render_result_encode(
        result,
        (MeisoMutableBuffer){payload, sizeof(payload)},
        &payload_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(meiso_stream_count(fixture->render_stream) == 0U &&
          meiso_stream_stats(fixture->render_stream, &stats_before));
    memset(&sample, 0, sizeof(sample));
    sample.key = (MeisoStreamKey){EDGE_OWNER, 417U, sequence};
    sample.source_time = composition_time(310U);
    sample.payload = (MeisoByteView){payload, payload_size};
    source->work.value.stream_sample.production_epoch =
        rejected_production_epoch;
    source->work.value.stream_sample.sample = sample;
    source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    CHECK(meiso_runtime_scheduler_step(
        fixture->tcp.edge.scheduler, &step
    ) == MEISO_RUNTIME_SCHEDULER_STEP_REJECTED);
    CHECK(step.completion.source_id == source->id &&
          step.completion.kind == MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE &&
          step.completion.code ==
              MEISO_RUNTIME_SCHEDULER_COMPLETION_REJECTED &&
          step.completion.runtime_status == expected_status &&
          source->poll_status == MEISO_RUNTIME_SCHEDULER_POLL_EMPTY &&
          meiso_stream_count(fixture->render_stream) == 0U &&
          meiso_stream_stats(fixture->render_stream, &stats_after) &&
          memcmp(&stats_before, &stats_after, sizeof(stats_before)) == 0);
    return true;
}

static bool advance_scene_snapshot(
    SemanticFixture *fixture,
    uint64_t desired_version,
    const MeisoCompositionSceneDesired *desired,
    uint64_t boundary_time,
    uint64_t production_epoch,
    uint64_t render_sequence,
    bool publish_old_result,
    size_t cube_size,
    uint32_t expected_object_count,
    const MeisoCompositionSceneWorldRef *old_world,
    const MeisoCompositionSceneRenderResult *old_fixed_result,
    MeisoCompositionSceneWorldRef *out_world,
    MeisoCompositionSceneRenderResult *out_result
)
{
    MeisoCompositionCoordinatorPlanView active_plan;
    MeisoCompositionSceneWorldView active_scene;
    MeisoCompositionSceneRenderResult pending_result;
    MeisoCompositionSceneRenderResult pending_published;
    MeisoCompositionSceneRenderResult post_swap_result;
    MeisoRuntimeProductionSnapshot pre_swap_gate;

    CHECK(deliver_scene_snapshot(fixture, desired_version, desired));
    CHECK(meiso_composition_coordinator_active(
        fixture->coordinator, &active_plan
    ) && active_plan.active_state == MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE &&
          active_plan.ref.desired_owner_incarnation ==
              HOST_OWNER.incarnation &&
          active_plan.ref.desired_state_version == 2U &&
          active_plan.ref.plan_generation == 1U);
    CHECK(meiso_composition_scene_store_active(
        fixture->scene_store, &active_scene
    ));
    CHECK(active_scene.ref.desired_state_version ==
              old_world->desired_state_version &&
          active_scene.ref.world_generation == old_world->world_generation &&
          active_scene.ref.plan_generation == 1U);
    CHECK(meiso_composition_scene_store_fake_render(
        fixture->scene_store, 77U, 300U, 310U, &pending_result
    ) == MEISO_COMPOSITION_SCENE_RENDER_COMPLETE);
    CHECK(pending_result.applied_scene_state_version ==
              old_world->desired_state_version &&
          pending_result.world_generation == old_world->world_generation &&
          pending_result.active_plan_generation == 1U &&
          memcmp(
              pending_result.full_target_sha256,
              old_fixed_result->full_target_sha256,
              sizeof(pending_result.full_target_sha256)
          ) == 0 &&
          memcmp(
              pending_result.thumbnail_rgb565,
              old_fixed_result->thumbnail_rgb565,
              sizeof(pending_result.thumbnail_rgb565)
          ) == 0);
    CHECK(meiso_runtime_production_snapshot(
        fixture->tcp.edge.runtime,
        *meiso_composition_scene_scene_endpoint(),
        &pre_swap_gate
    ) == MEISO_RUNTIME_OK);
    CHECK(pre_swap_gate.data_active &&
          pre_swap_gate.production_status ==
              MEISO_RUNTIME_PRODUCTION_RUNNING &&
          pre_swap_gate.production_epoch == production_epoch - 1U);
    if (publish_old_result) {
        CHECK(send_render_result_over_scheduler(
            fixture,
            old_world,
            production_epoch - 1U,
            render_sequence - 1U,
            300U,
            310U,
            &pending_published
        ));
        CHECK(memcmp(
            &pending_published,
            old_fixed_result,
            sizeof(pending_published)
        ) == 0);
    }
    CHECK(reject_render_epoch_without_consuming_sequence(
        fixture,
        &pending_result,
        production_epoch,
        render_sequence,
        MEISO_RUNTIME_PRODUCTION_EPOCH_MISMATCH
    ));
    CHECK(meiso_composition_coordinator_swap_scene_frame_boundary(
        fixture->coordinator,
        HOST_OWNER.incarnation,
        desired_version,
        composition_time(boundary_time),
        out_world
    ) == MEISO_COMPOSITION_COORDINATOR_SWAP_ACTIVATED);
    CHECK(out_world->desired_state_version == desired_version &&
          out_world->world_generation == old_world->world_generation + 1U &&
          out_world->plan_generation == 1U);
    CHECK(meiso_composition_coordinator_active(
        fixture->coordinator, &active_plan
    ) && active_plan.ref.desired_owner_incarnation ==
              HOST_OWNER.incarnation &&
          active_plan.ref.desired_state_version == 2U &&
          active_plan.ref.plan_generation == 1U &&
          active_plan.active_state == MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE);
    CHECK(meiso_composition_scene_store_active(
        fixture->scene_store, &active_scene
    ) && memcmp(
              &active_scene.ref, out_world, sizeof(active_scene.ref)
          ) == 0);
    CHECK(deliver_scene_actual_snapshot(
        fixture,
        desired_version,
        desired_version,
        production_epoch,
        cube_size,
        out_world,
        expected_object_count
    ));
    CHECK(meiso_composition_scene_store_fake_render(
        fixture->scene_store, 77U, 300U, 310U, &post_swap_result
    ) == MEISO_COMPOSITION_SCENE_RENDER_COMPLETE);
    CHECK(post_swap_result.applied_scene_state_version == desired_version &&
          post_swap_result.world_generation == out_world->world_generation);
    CHECK(reject_render_epoch_without_consuming_sequence(
        fixture,
        &post_swap_result,
        production_epoch - 1U,
        render_sequence,
        MEISO_RUNTIME_PRODUCTION_EPOCH_MISMATCH
    ));
    CHECK(send_render_result_over_scheduler(
        fixture,
        out_world,
        production_epoch,
        render_sequence,
        300U,
        310U,
        out_result
    ));
    CHECK(out_result->applied_scene_state_version == desired_version &&
          out_result->world_generation == out_world->world_generation &&
          out_result->active_plan_generation == 1U &&
          out_result->object_count == expected_object_count);
    return true;
}

static bool send_second_wifi_sample(SemanticFixture *fixture)
{
    const MeisoWifiTelemetryFieldMask fields =
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
        MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM;
    TxSource *source = &fixture->tcp.edge_sources[EDGE_WIFI_STREAM];
    MeisoWifiTelemetrySample value;
    MeisoWifiTelemetrySample decoded;
    MeisoStreamSample sample;
    MeisoStreamReceiveResult receive;
    MeisoRuntimeEvent event;
    MeisoRuntimeProductionSnapshot host_gate;
    MeisoRuntimeProductionSnapshot edge_gate;
    MeisoStreamStats stats;
    uint8_t payload[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    size_t payload_size = 0U;

    memset(&value, 0, sizeof(value));
    value.selected_fields = fields;
    value.present_fields = fields;
    value.link_state = true;
    value.signal_mbm = -4700;
    CHECK(meiso_wifi_telemetry_sample_encode(
        &value,
        (MeisoMutableBuffer){payload, sizeof(payload)},
        &payload_size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    memset(&sample, 0, sizeof(sample));
    sample.key = (MeisoStreamKey){EDGE_OWNER, 177U, 2U};
    sample.source_time = (MeisoTimePoint){1U, 2000U};
    sample.payload = (MeisoByteView){payload, payload_size};
    source->work.value.stream_sample.production_epoch = 77U;
    source->work.value.stream_sample.sample = sample;
    CHECK(dispatch_source(
        &fixture->tcp.edge, &fixture->tcp.host, source, NULL, &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          route_key_equal(
              event.route, route_key(&fixture->tcp, ROUTE_WIFI_DATA)
          ) && event.production_epoch == 77U &&
          event.stream_sample.key.epoch == 177U &&
          event.stream_sample.key.sequence == 2U);
    CHECK(meiso_wifi_telemetry_sample_decode(
        event.stream_sample.payload, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(decoded.link_state && decoded.signal_mbm == -4700);
    receive = meiso_stream_receive(
        fixture->wifi_stream, &event.stream_sample, NULL
    );
    CHECK(receive.code == MEISO_STREAM_RECEIVE_ACCEPTED &&
          meiso_stream_count(fixture->wifi_stream) == 2U &&
          meiso_stream_stats(fixture->wifi_stream, &stats) &&
          stats.accepted == 2U);
    CHECK(meiso_runtime_production_snapshot(
        fixture->tcp.host.runtime,
        *meiso_wifi_telemetry_endpoint(),
        &host_gate
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_production_snapshot(
        fixture->tcp.edge.runtime,
        *meiso_wifi_telemetry_endpoint(),
        &edge_gate
    ) == MEISO_RUNTIME_OK);
    CHECK(host_gate.data_active && edge_gate.data_active &&
          host_gate.production_epoch == 77U &&
          edge_gate.production_epoch == 77U &&
          host_gate.production_status == MEISO_RUNTIME_PRODUCTION_RUNNING &&
          edge_gate.production_status == MEISO_RUNTIME_PRODUCTION_RUNNING);
    return true;
}

static bool disable_scene_lifecycle(
    SemanticFixture *fixture,
    size_t cube_size,
    const MeisoCompositionSceneWorldRef *last_world,
    const MeisoCompositionSceneRenderResult *last_fixed_result
)
{
    const MeisoCompositionSceneResourceUsage usage = {
        1U,
        cube_size,
        0U,
        0U,
        0U,
        (uint32_t)meiso_stream_count(fixture->render_stream)
    };
    const MeisoRuntimeDesiredState disabled = {false, {NULL, 0U}};
    uint8_t desired_storage[MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE];
    uint8_t presentation_suffix[
        MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE
    ];
    uint8_t scene_suffix[MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE];
    uint8_t coordinator_raw_before[4096];
    uint8_t scene_store_raw_before[40000];
    MeisoByteView desired_payload;
    MeisoStateView current;
    MeisoRuntimeDesiredState decoded_desired;
    MeisoRuntimeActualState decoded_actual;
    MeisoCompositionCoordinatorPlanView active_before;
    MeisoCompositionCoordinatorPlanView active_after;
    MeisoCompositionCoordinatorObservation observation_before;
    MeisoCompositionCoordinatorObservation observation_after;
    MeisoCompositionCoordinatorStats stats_before;
    MeisoCompositionCoordinatorStats stats_after;
    MeisoCompositionScenePresentationStatus status_before;
    MeisoCompositionScenePresentationStatus status_after;
    MeisoCompositionSceneWorldView world_before;
    MeisoCompositionSceneWorldView world_after;
    MeisoCompositionSceneRenderResult render_before;
    MeisoCompositionSceneRenderResult render_after;
    MeisoCompositionSceneStatus scene_status;
    MeisoCompositionSceneStatus decoded_scene_status;
    MeisoRuntimeProductionSnapshot host_scene_gate;
    MeisoRuntimeProductionSnapshot edge_scene_gate;
    MeisoRuntimeProductionSnapshot host_wifi_gate;
    size_t presentation_size = 0U;
    size_t scene_size = 0U;
    const size_t coordinator_bytes =
        meiso_composition_coordinator_memory_required();
    const size_t scene_store_bytes =
        meiso_composition_scene_store_memory_required();

    CHECK(meiso_runtime_desired_state_encode(
        &disabled,
        (MeisoMutableBuffer){desired_storage, sizeof(desired_storage)},
        &desired_payload
    ) && desired_payload.size == MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE);
    CHECK(meiso_state_publisher_publish(
        fixture->plan_desired.publisher,
        HOST_OWNER,
        3U,
        desired_payload
    ) == MEISO_STATE_PUBLISH_NEW);
    CHECK(deliver_state_over_scheduler(
        &fixture->tcp,
        &fixture->plan_desired,
        true,
        HOST_PLAN_DESIRED,
        EDGE_PLAN_DESIRED_ACCEPT,
        &current
    ));
    CHECK(meiso_runtime_desired_state_decode(
        current.payload, &decoded_desired
    ) && !decoded_desired.enabled && decoded_desired.profile.size == 0U);
    CHECK(meiso_composition_coordinator_observe_desired(
        fixture->coordinator,
        current.owner.incarnation,
        current.version,
        &decoded_desired,
        composition_time(180U)
    ) == MEISO_COMPOSITION_COORDINATOR_OBSERVE_DISABLE_PENDING);

    memset(&active_before, 0, sizeof(active_before));
    memset(&active_after, 0, sizeof(active_after));
    memset(&observation_before, 0, sizeof(observation_before));
    memset(&observation_after, 0, sizeof(observation_after));
    memset(&stats_before, 0, sizeof(stats_before));
    memset(&stats_after, 0, sizeof(stats_after));
    memset(&status_before, 0, sizeof(status_before));
    memset(&status_after, 0, sizeof(status_after));
    memset(&world_before, 0, sizeof(world_before));
    memset(&world_after, 0, sizeof(world_after));
    CHECK(meiso_composition_coordinator_active(
        fixture->coordinator, &active_before
    ));
    CHECK(meiso_composition_coordinator_observation(
        fixture->coordinator, &observation_before
    ));
    CHECK(meiso_composition_coordinator_stats(
        fixture->coordinator, &stats_before
    ));
    CHECK(meiso_composition_coordinator_status(
        fixture->coordinator, &status_before
    ));
    CHECK(meiso_composition_scene_store_active(
        fixture->scene_store, &world_before
    ));
    CHECK(meiso_composition_scene_store_fake_render(
        fixture->scene_store, 77U, 300U, 310U, &render_before
    ) == MEISO_COMPOSITION_SCENE_RENDER_COMPLETE);
    CHECK(render_before.world_generation == last_world->world_generation &&
          memcmp(render_before.full_target_sha256,
                 last_fixed_result->full_target_sha256,
                 sizeof(render_before.full_target_sha256)) == 0);
    CHECK(coordinator_bytes <= sizeof(coordinator_raw_before) &&
          scene_store_bytes <= sizeof(scene_store_raw_before));
    memcpy(
        coordinator_raw_before,
        fixture->coordinator_arena,
        coordinator_bytes
    );
    memcpy(
        scene_store_raw_before,
        fixture->scene_store_arena,
        scene_store_bytes
    );
    CHECK(meiso_composition_coordinator_disable_frame_boundary(
        fixture->coordinator, false, composition_time(190U)
    ) == MEISO_COMPOSITION_COORDINATOR_DISABLE_WAITING_DRAIN);
    CHECK(memcmp(coordinator_raw_before, fixture->coordinator_arena,
                 coordinator_bytes) == 0 &&
          memcmp(scene_store_raw_before, fixture->scene_store_arena,
                 scene_store_bytes) == 0);
    CHECK(meiso_composition_coordinator_active(
        fixture->coordinator, &active_after
    ));
    CHECK(meiso_composition_coordinator_observation(
        fixture->coordinator, &observation_after
    ));
    CHECK(meiso_composition_coordinator_stats(
        fixture->coordinator, &stats_after
    ));
    CHECK(meiso_composition_coordinator_status(
        fixture->coordinator, &status_after
    ));
    CHECK(meiso_composition_scene_store_active(
        fixture->scene_store, &world_after
    ));
    CHECK(meiso_composition_scene_store_fake_render(
        fixture->scene_store, 77U, 300U, 310U, &render_after
    ) == MEISO_COMPOSITION_SCENE_RENDER_COMPLETE);
    CHECK(memcmp(&active_before, &active_after, sizeof(active_before)) == 0 &&
          memcmp(&observation_before, &observation_after,
                 sizeof(observation_before)) == 0 &&
          memcmp(&stats_before, &stats_after, sizeof(stats_before)) == 0 &&
          memcmp(&status_before, &status_after, sizeof(status_before)) == 0 &&
          memcmp(&world_before, &world_after, sizeof(world_before)) == 0 &&
          memcmp(&render_before, &render_after, sizeof(render_before)) == 0);
    CHECK(meiso_composition_coordinator_disable_frame_boundary(
        fixture->coordinator, true, composition_time(200U)
    ) == MEISO_COMPOSITION_COORDINATOR_DISABLE_DISABLED);
    CHECK(!meiso_composition_coordinator_active(
        fixture->coordinator, &active_after
    ) && !meiso_composition_scene_store_active(
        fixture->scene_store, &world_after
    ));

    CHECK(meiso_composition_coordinator_status(
        fixture->coordinator, &status_after
    ));
    CHECK(status_after.active_state == MEISO_COMPOSITION_SCENE_ACTIVE_NONE);
    CHECK(meiso_composition_scene_presentation_status_encode(
        &status_after,
        (MeisoMutableBuffer){
            presentation_suffix, sizeof(presentation_suffix)
        },
        &presentation_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_stopped_actual_state(
        &fixture->presentation_actual,
        2U,
        3U,
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
        current.payload, &decoded_actual
    ) && decoded_actual.production_status ==
              MEISO_RUNTIME_PRODUCTION_STOPPED &&
          decoded_actual.production_epoch == 0U &&
          decoded_actual.observed_desired.present &&
          decoded_actual.applied_desired.present &&
          meiso_owner_scope_equal(
              decoded_actual.observed_desired.owner, HOST_OWNER
          ) && meiso_owner_scope_equal(
              decoded_actual.applied_desired.owner, HOST_OWNER
          ) && decoded_actual.observed_desired.version == 3U &&
          decoded_actual.applied_desired.version == 3U &&
          decoded_actual.apply_result == MEISO_RUNTIME_APPLY_APPLIED &&
          decoded_actual.availability == MEISO_RUNTIME_AVAILABILITY_PRESENT &&
          !decoded_actual.effective_enabled &&
          decoded_actual.reason == MEISO_RUNTIME_PRODUCTION_REASON_DISABLED);

    CHECK(meiso_composition_scene_store_status(
        fixture->scene_store, &usage, &scene_status
    ));
    CHECK(scene_status.reason == MEISO_COMPOSITION_SCENE_REASON_NONE &&
          scene_status.active_plan_generation == 0U &&
          scene_status.world_generation == 0U &&
          scene_status.active_object_count == 0U);
    CHECK(meiso_composition_scene_status_encode(
        &scene_status,
        (MeisoMutableBuffer){scene_suffix, sizeof(scene_suffix)},
        &scene_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_stopped_actual_state(
        &fixture->scene_actual,
        6U,
        5U,
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
        current.payload, &decoded_actual
    ) && decoded_actual.production_status ==
              MEISO_RUNTIME_PRODUCTION_STOPPED &&
          decoded_actual.production_epoch == 0U &&
          decoded_actual.observed_desired.present &&
          decoded_actual.applied_desired.present &&
          meiso_owner_scope_equal(
              decoded_actual.observed_desired.owner, HOST_OWNER
          ) && meiso_owner_scope_equal(
              decoded_actual.applied_desired.owner, HOST_OWNER
          ) && decoded_actual.observed_desired.version == 5U &&
          decoded_actual.applied_desired.version == 5U &&
          decoded_actual.apply_result == MEISO_RUNTIME_APPLY_APPLIED &&
          decoded_actual.availability == MEISO_RUNTIME_AVAILABILITY_PRESENT &&
          !decoded_actual.effective_enabled &&
          decoded_actual.reason == MEISO_RUNTIME_PRODUCTION_REASON_DISABLED);
    CHECK(meiso_composition_scene_status_decode(
        decoded_actual.profile, &decoded_scene_status
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(decoded_scene_status.reason ==
              MEISO_COMPOSITION_SCENE_REASON_NONE &&
          decoded_scene_status.active_plan_generation == 0U &&
          decoded_scene_status.world_generation == 0U &&
          decoded_scene_status.active_object_count == 0U);
    CHECK(meiso_runtime_production_snapshot(
        fixture->tcp.host.runtime,
        *meiso_composition_scene_scene_endpoint(),
        &host_scene_gate
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_production_snapshot(
        fixture->tcp.edge.runtime,
        *meiso_composition_scene_scene_endpoint(),
        &edge_scene_gate
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_production_snapshot(
        fixture->tcp.host.runtime,
        *meiso_wifi_telemetry_endpoint(),
        &host_wifi_gate
    ) == MEISO_RUNTIME_OK);
    CHECK(!host_scene_gate.data_active && !edge_scene_gate.data_active &&
          host_scene_gate.production_status ==
              MEISO_RUNTIME_PRODUCTION_STOPPED &&
          edge_scene_gate.production_status ==
              MEISO_RUNTIME_PRODUCTION_STOPPED &&
          host_scene_gate.production_epoch == 0U &&
          edge_scene_gate.production_epoch == 0U &&
          host_scene_gate.production_epoch_high_water == 46U &&
          edge_scene_gate.production_epoch_high_water == 46U &&
          host_wifi_gate.data_active &&
          host_wifi_gate.production_status ==
              MEISO_RUNTIME_PRODUCTION_RUNNING &&
          host_wifi_gate.production_epoch == 77U);
    CHECK(reject_render_epoch_without_consuming_sequence(
        fixture,
        last_fixed_result,
        0U,
        7U,
        MEISO_RUNTIME_ROUTE_NOT_ACTIVE
    ));
    CHECK(send_second_wifi_sample(fixture));
    return true;
}

static bool assert_catalog_replacement_closed_gates(
    SemanticFixture *fixture
)
{
    MeisoRuntimeProductionSnapshot host_wifi;
    MeisoRuntimeProductionSnapshot edge_wifi;
    MeisoRuntimeProductionSnapshot host_presentation;
    MeisoRuntimeProductionSnapshot edge_presentation;
    MeisoRuntimeProductionSnapshot host_scene;
    MeisoRuntimeProductionSnapshot edge_scene;

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
        *meiso_composition_scene_presentation_endpoint(),
        &host_presentation
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_production_snapshot(
        fixture->tcp.edge.runtime,
        *meiso_composition_scene_presentation_endpoint(),
        &edge_presentation
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
    CHECK(!host_wifi.actual_present && !edge_wifi.actual_present &&
          !host_wifi.data_active && !edge_wifi.data_active &&
          !host_presentation.actual_present &&
          !edge_presentation.actual_present &&
          !host_presentation.data_active &&
          !edge_presentation.data_active &&
          !host_scene.actual_present && !edge_scene.actual_present &&
          !host_scene.data_active && !edge_scene.data_active &&
          host_wifi.production_epoch_high_water == 77U &&
          edge_wifi.production_epoch_high_water == 77U &&
          host_presentation.production_epoch_high_water == 41U &&
          edge_presentation.production_epoch_high_water == 41U &&
          host_scene.production_epoch_high_water == 46U &&
          edge_scene.production_epoch_high_water == 46U);
    return true;
}

static bool start_catalog_generation_two_coordinator(
    SemanticFixture *fixture,
    const MeisoContentId *cube_id,
    const MeisoCompositionSceneWorldRef *old_world,
    const uint8_t old_token[MEISO_SHA256_DIGEST_SIZE]
)
{
    MeisoCompositionCoordinator *old_coordinator = fixture->coordinator;
    void *old_arena = fixture->coordinator_arena;
    void *new_arena = NULL;
    MeisoCompositionCoordinator *new_coordinator = NULL;
    MeisoCompositionCoordinatorConfig config;
    MeisoCompositionSceneControlScope scope_before;
    MeisoCompositionSceneControlScope scope_after;
    MeisoCompositionCoordinatorObservation observation;
    MeisoCompositionCoordinatorPlanView plan_view;
    MeisoCompositionCoordinatorStats stats;
    MeisoCompositionScenePresentationStatus status;
    MeisoCompositionSceneWorldView world_view;
    MeisoCompositionSceneWorldRef output_world;
    MeisoRuntimeDesiredState disabled = {false, {NULL, 0U}};
    MeisoBlobView cached;
    MeisoSessionSnapshot session;
    const size_t required = meiso_composition_coordinator_memory_required();

    CHECK(cube_id != NULL && old_world != NULL && old_token != NULL &&
          meiso_composition_scene_store_control_scope(
              fixture->scene_store, &scope_before
          ) && scope_before.desired_owner_incarnation ==
              HOST_OWNER.incarnation &&
          meiso_blob_store_count(fixture->blob_store) == 1U &&
          meiso_blob_store_lookup(
              fixture->blob_store, cube_id, &cached
          ) && cached.content.size != 0U &&
          meiso_session_snapshot(fixture->tcp.edge.session, &session) &&
          session.state == MEISO_SESSION_STATE_READY &&
          session.session_id_known);
    memset(&config, 0, sizeof(config));
    config.scene_store = fixture->scene_store;
    config.session_id = session.session_id;
    config.edge_incarnation = EDGE_OWNER.incarnation;
    config.capability_generation = 2U;
    config.edge_clock_epoch = 77U;
    CHECK(allocate_arena(&new_arena, required));
    CHECK(meiso_composition_coordinator_init(
        new_arena, required, &config, &new_coordinator
    ) == MEISO_COMPOSITION_COORDINATOR_INIT_OK);
    CHECK(meiso_composition_coordinator_begin_owner(
        new_coordinator, HOST_OWNER.incarnation
    ) == MEISO_COMPOSITION_COORDINATOR_OWNER_STARTED);
    CHECK(meiso_composition_scene_store_control_scope(
        fixture->scene_store, &scope_after
    ) && scope_after.desired_owner_incarnation ==
              scope_before.desired_owner_incarnation &&
          scope_after.control_scope_generation ==
              scope_before.control_scope_generation + 1U);
    CHECK(!meiso_composition_coordinator_observation(
              old_coordinator, &observation
          ) &&
          !meiso_composition_coordinator_candidate(
              old_coordinator, &plan_view
          ) &&
          !meiso_composition_coordinator_active(
              old_coordinator, &plan_view
          ) &&
          !meiso_composition_coordinator_status(old_coordinator, &status) &&
          !meiso_composition_coordinator_stats(old_coordinator, &stats));
    output_world = *old_world;
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
        old_coordinator,
        old_token,
        composition_time(300U),
        &output_world
    ) == MEISO_COMPOSITION_COORDINATOR_ACTIVATE_INVALID_ARGUMENT);
    CHECK(memcmp(&output_world, old_world, sizeof(output_world)) == 0);
    CHECK(meiso_composition_coordinator_observe_desired(
        old_coordinator,
        HOST_OWNER.incarnation,
        7U,
        &disabled,
        composition_time(300U)
    ) == MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_ARGUMENT);
    CHECK(!meiso_composition_scene_store_active(
              fixture->scene_store, &world_view
          ) &&
          !meiso_composition_scene_store_candidate(
              fixture->scene_store, &world_view
          ) &&
          meiso_blob_store_count(fixture->blob_store) == 1U &&
          meiso_blob_store_lookup(
              fixture->blob_store, cube_id, &cached
          ));

    fixture->coordinator_arena = new_arena;
    fixture->coordinator = new_coordinator;
    free(old_arena);
    return true;
}

static bool activate_catalog_generation_two_plan(
    SemanticFixture *fixture,
    const MeisoCompositionSceneWorldRef *old_world,
    const uint8_t old_token[MEISO_SHA256_DIGEST_SIZE],
    MeisoCompositionSceneWorldRef *out_world,
    uint8_t out_token[MEISO_SHA256_DIGEST_SIZE]
)
{
    MeisoCompositionScenePresentationPlan plan =
        plan_value_at_generation(2U, 2U);
    MeisoCompositionScenePresentationPlan candidate_plan_before;
    MeisoCompositionCoordinatorPlanView candidate_before;
    MeisoCompositionCoordinatorPlanView candidate_after;
    MeisoCompositionCoordinatorPlanView active;
    MeisoCompositionCoordinatorPlanRef expected;
    MeisoRuntimeDesiredState decoded;
    MeisoStateView current;
    MeisoCompositionSceneWorldRef untouched;
    uint8_t suffix[MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE];
    uint8_t nonce[MEISO_SHA256_DIGEST_SIZE];
    uint8_t store_before[TEST_SCENE_STORE_SNAPSHOT_BYTES];
    const size_t store_bytes =
        meiso_composition_scene_store_memory_required();
    size_t suffix_size = 0U;

    CHECK(old_world != NULL && old_token != NULL && out_world != NULL &&
          out_token != NULL && plan.plan_generation == 2U &&
          plan.capability_generation == 2U);
    CHECK(meiso_composition_scene_presentation_plan_encode(
        &plan,
        (MeisoMutableBuffer){suffix, sizeof(suffix)},
        &suffix_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_desired_state(
        &fixture->plan_desired, 4U,
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
    CHECK(current.version == 4U &&
          meiso_runtime_desired_state_decode(current.payload, &decoded) &&
          meiso_composition_coordinator_observe_desired(
              fixture->coordinator,
              current.owner.incarnation,
              current.version,
              &decoded,
              composition_time(310U)
          ) == MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING &&
          meiso_composition_coordinator_candidate(
              fixture->coordinator, &candidate_before
          ));
    expected = candidate_before.ref;
    fill_nonce(nonce, 0x71U);
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
        fixture->coordinator,
        &expected,
        MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
        composition_time(320U),
        nonce
    ) == MEISO_COMPOSITION_COORDINATOR_REPORT_PREPARED);
    CHECK(meiso_composition_coordinator_candidate(
        fixture->coordinator, &candidate_before
    ) && candidate_before.prepared_token_present &&
          candidate_before.candidate_state ==
              MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED &&
          candidate_before.plan != NULL);
    candidate_plan_before = *candidate_before.plan;
    memcpy(
        out_token,
        candidate_before.prepared_token,
        MEISO_SHA256_DIGEST_SIZE
    );
    CHECK(memcmp(
        out_token, old_token, MEISO_SHA256_DIGEST_SIZE
    ) != 0);

    plan.prepared_token_echo_present = true;
    memcpy(
        plan.prepared_token_echo, old_token, MEISO_SHA256_DIGEST_SIZE
    );
    CHECK(meiso_composition_scene_presentation_plan_encode(
        &plan,
        (MeisoMutableBuffer){suffix, sizeof(suffix)},
        &suffix_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_desired_state(
        &fixture->plan_desired, 5U,
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
    CHECK(meiso_runtime_desired_state_decode(current.payload, &decoded) &&
          meiso_composition_coordinator_observe_desired(
              fixture->coordinator,
              current.owner.incarnation,
              current.version,
              &decoded,
              composition_time(330U)
          ) == MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH &&
          meiso_composition_coordinator_candidate(
              fixture->coordinator, &candidate_after
          ));
    CHECK(memcmp(
              &candidate_before.ref,
              &candidate_after.ref,
              sizeof(candidate_before.ref)
          ) == 0 &&
          candidate_before.candidate_state == candidate_after.candidate_state &&
          candidate_before.active_state == candidate_after.active_state &&
          candidate_before.prepared_token_present ==
              candidate_after.prepared_token_present &&
          memcmp(
              candidate_before.prepared_token,
              candidate_after.prepared_token,
              MEISO_SHA256_DIGEST_SIZE
          ) == 0 &&
          memcmp(
              &candidate_before.first_observed_at,
              &candidate_after.first_observed_at,
              sizeof(candidate_before.first_observed_at)
          ) == 0 &&
          memcmp(
              &candidate_before.prepared_at,
              &candidate_after.prepared_at,
              sizeof(candidate_before.prepared_at)
          ) == 0 &&
          candidate_after.plan != NULL &&
          memcmp(
              &candidate_plan_before,
              candidate_after.plan,
              sizeof(candidate_plan_before)
          ) == 0);

    memcpy(
        plan.prepared_token_echo, out_token, MEISO_SHA256_DIGEST_SIZE
    );
    CHECK(meiso_composition_scene_presentation_plan_encode(
        &plan,
        (MeisoMutableBuffer){suffix, sizeof(suffix)},
        &suffix_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_desired_state(
        &fixture->plan_desired, 6U,
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
    CHECK(meiso_runtime_desired_state_decode(current.payload, &decoded) &&
          meiso_composition_coordinator_observe_desired(
              fixture->coordinator,
              current.owner.incarnation,
              current.version,
              &decoded,
              composition_time(340U)
          ) == MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING);
    untouched = *old_world;
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
        fixture->coordinator,
        old_token,
        composition_time(350U),
        &untouched
    ) == MEISO_COMPOSITION_COORDINATOR_ACTIVATE_TOKEN_MISMATCH &&
          memcmp(&untouched, old_world, sizeof(untouched)) == 0);
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
        fixture->coordinator,
        out_token,
        composition_time(360U),
        out_world
    ) == MEISO_COMPOSITION_COORDINATOR_ACTIVATE_ACTIVATED);
    CHECK(out_world->desired_owner_incarnation == HOST_OWNER.incarnation &&
          out_world->desired_state_version == 6U &&
          out_world->plan_generation == 2U &&
          out_world->world_generation == old_world->world_generation + 1U &&
          meiso_composition_coordinator_active(
              fixture->coordinator, &active
          ) && active.ref.desired_state_version == 6U &&
          active.ref.plan_generation == 2U &&
          active.active_state == MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE);

    CHECK(store_bytes <= sizeof(store_before));
    memcpy(store_before, fixture->scene_store_arena, store_bytes);
    CHECK(!meiso_composition_scene_store_record_result_published(
        fixture->scene_store, old_world, 417U, 7U
    ));
    CHECK(memcmp(
        store_before, fixture->scene_store_arena, store_bytes
    ) == 0);
    return true;
}

static bool deliver_catalog_generation_two_actuals(
    SemanticFixture *fixture,
    size_t cube_size,
    const MeisoCompositionSceneWorldRef *world
)
{
    const MeisoWifiTelemetryEffectiveParameters wifi_parameters = {
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
            MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM,
        500U
    };
    const MeisoCompositionSceneResourceUsage usage = {
        1U, cube_size, 0U, 0U, 0U, 0U
    };
    uint8_t wifi_suffix[
        MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE
    ];
    uint8_t presentation_suffix[
        MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE
    ];
    uint8_t scene_suffix[MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE];
    MeisoCompositionScenePresentationStatus presentation_status;
    MeisoCompositionSceneStatus scene_status;
    MeisoRuntimeActualState decoded;
    MeisoStateView current;
    MeisoRuntimeProductionSnapshot host_wifi;
    MeisoRuntimeProductionSnapshot edge_wifi;
    MeisoRuntimeProductionSnapshot host_scene;
    MeisoRuntimeProductionSnapshot edge_scene;
    MeisoRuntimeProductionSnapshot host_presentation;
    MeisoRuntimeProductionSnapshot edge_presentation;
    size_t wifi_size = 0U;
    size_t presentation_size = 0U;
    size_t scene_size = 0U;

    CHECK(world != NULL);
    CHECK(meiso_wifi_telemetry_effective_parameters_encode(
        &wifi_parameters,
        (MeisoMutableBuffer){wifi_suffix, sizeof(wifi_suffix)},
        &wifi_size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(publish_actual_state(
        &fixture->wifi_actual,
        2U,
        2U,
        88U,
        (MeisoByteView){wifi_suffix, wifi_size}
    ));
    CHECK(deliver_state_over_scheduler(
        &fixture->tcp,
        &fixture->wifi_actual,
        false,
        EDGE_WIFI_ACTUAL,
        HOST_WIFI_ACTUAL_ACCEPT,
        &current
    ));
    CHECK(current.version == 2U &&
          meiso_runtime_actual_state_decode(current.payload, &decoded) &&
          decoded.applied_desired.version == 2U &&
          decoded.production_epoch == 88U);

    CHECK(meiso_composition_coordinator_status(
        fixture->coordinator, &presentation_status
    ) && presentation_status.capability_generation == 2U &&
          presentation_status.active_state ==
              MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE &&
          presentation_status.active_plan_generation == 2U);
    CHECK(meiso_composition_scene_presentation_status_encode(
        &presentation_status,
        (MeisoMutableBuffer){
            presentation_suffix, sizeof(presentation_suffix)
        },
        &presentation_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_actual_state(
        &fixture->presentation_actual,
        3U,
        6U,
        51U,
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
    CHECK(current.version == 3U &&
          meiso_runtime_actual_state_decode(current.payload, &decoded) &&
          decoded.applied_desired.version == 6U &&
          decoded.production_epoch == 51U);

    CHECK(meiso_composition_scene_store_status(
        fixture->scene_store, &usage, &scene_status
    ) && scene_status.reason == MEISO_COMPOSITION_SCENE_REASON_READY &&
          scene_status.active_plan_generation == 2U &&
          scene_status.world_generation == world->world_generation &&
          scene_status.active_object_count == 1U &&
          scene_status.unique_mesh_count == 1U &&
          scene_status.published_blob_count == 1U &&
          scene_status.published_blob_bytes == cube_size);
    CHECK(meiso_composition_scene_status_encode(
        &scene_status,
        (MeisoMutableBuffer){scene_suffix, sizeof(scene_suffix)},
        &scene_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_actual_state(
        &fixture->scene_actual,
        7U,
        6U,
        52U,
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
    CHECK(current.version == 7U &&
          meiso_runtime_actual_state_decode(current.payload, &decoded) &&
          decoded.applied_desired.version == 6U &&
          decoded.production_epoch == 52U);

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
        *meiso_composition_scene_presentation_endpoint(),
        &host_presentation
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_production_snapshot(
        fixture->tcp.edge.runtime,
        *meiso_composition_scene_presentation_endpoint(),
        &edge_presentation
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
    CHECK(host_wifi.data_active && edge_wifi.data_active &&
          host_wifi.production_epoch == 88U &&
          edge_wifi.production_epoch == 88U &&
          host_presentation.data_active && edge_presentation.data_active &&
          host_presentation.production_epoch == 51U &&
          edge_presentation.production_epoch == 51U &&
          host_scene.data_active && edge_scene.data_active &&
          host_scene.production_epoch == 52U &&
          edge_scene.production_epoch == 52U &&
          host_wifi.production_epoch_high_water == 88U &&
          edge_wifi.production_epoch_high_water == 88U &&
          host_presentation.production_epoch_high_water == 51U &&
          edge_presentation.production_epoch_high_water == 51U &&
          host_scene.production_epoch_high_water == 52U &&
          edge_scene.production_epoch_high_water == 52U);
    return true;
}

static bool reject_old_production_epoch(
    SemanticFixture *fixture,
    TxSource *source,
    uint64_t production_epoch,
    const MeisoStreamSample *sample
)
{
    MeisoRuntimeSchedulerStepResult step;
    MeisoSessionSnapshot before;
    MeisoSessionSnapshot after;
    const uint64_t completions_before = source->completions;

    CHECK(sample != NULL &&
          meiso_session_snapshot(fixture->tcp.edge.session, &before) &&
          !before.outbound_pending);
    source->work.value.stream_sample.production_epoch = production_epoch;
    source->work.value.stream_sample.sample = *sample;
    source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    memset(&step, 0xa5, sizeof(step));
    CHECK(meiso_runtime_scheduler_step(
        fixture->tcp.edge.scheduler, &step
    ) == MEISO_RUNTIME_SCHEDULER_STEP_REJECTED);
    CHECK(step.completion.source_id == source->id &&
          step.completion.kind == MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE &&
          step.completion.code ==
              MEISO_RUNTIME_SCHEDULER_COMPLETION_REJECTED &&
          step.completion.runtime_status ==
              MEISO_RUNTIME_PRODUCTION_EPOCH_MISMATCH &&
          source->completions == completions_before + 1U &&
          source->poll_status == MEISO_RUNTIME_SCHEDULER_POLL_EMPTY &&
          meiso_session_snapshot(fixture->tcp.edge.session, &after) &&
          !after.outbound_pending);
    return true;
}

static bool restore_generation_two_streams(
    SemanticFixture *fixture,
    const MeisoCompositionSceneWorldRef *world
)
{
    const MeisoWifiTelemetryFieldMask fields =
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
        MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM;
    TxSource *wifi_source = &fixture->tcp.edge_sources[EDGE_WIFI_STREAM];
    TxSource *render_source = &fixture->tcp.edge_sources[EDGE_RENDER_STREAM];
    MeisoWifiTelemetrySample wifi_value;
    MeisoWifiTelemetrySample decoded_wifi;
    MeisoCompositionSceneRenderResult render_value;
    MeisoCompositionSceneRenderResult decoded_render;
    MeisoStreamSample wifi_sample;
    MeisoStreamSample render_sample;
    MeisoStreamSample queued;
    MeisoRuntimeEvent event;
    MeisoStreamReceiveResult receive;
    uint8_t wifi_payload[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    uint8_t render_payload[
        MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE
    ];
    size_t wifi_size = 0U;
    size_t render_size = 0U;

    CHECK(world != NULL &&
          meiso_composition_scene_store_fake_render(
              fixture->scene_store, 77U, 400U, 410U, &render_value
          ) == MEISO_COMPOSITION_SCENE_RENDER_COMPLETE &&
          render_value.applied_scene_owner_incarnation ==
              world->desired_owner_incarnation &&
          render_value.applied_scene_state_version ==
              world->desired_state_version &&
          render_value.active_plan_generation == world->plan_generation &&
          render_value.world_generation == world->world_generation);
    memset(&wifi_value, 0, sizeof(wifi_value));
    wifi_value.selected_fields = fields;
    wifi_value.present_fields = fields;
    wifi_value.link_state = true;
    wifi_value.signal_mbm = -4600;
    CHECK(meiso_wifi_telemetry_sample_encode(
        &wifi_value,
        (MeisoMutableBuffer){wifi_payload, sizeof(wifi_payload)},
        &wifi_size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(meiso_composition_scene_render_result_encode(
        &render_value,
        (MeisoMutableBuffer){render_payload, sizeof(render_payload)},
        &render_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    memset(&wifi_sample, 0, sizeof(wifi_sample));
    wifi_sample.key = (MeisoStreamKey){EDGE_OWNER, 177U, 3U};
    wifi_sample.source_time = composition_time(400U);
    wifi_sample.payload = (MeisoByteView){wifi_payload, wifi_size};
    memset(&render_sample, 0, sizeof(render_sample));
    render_sample.key = (MeisoStreamKey){EDGE_OWNER, 417U, 7U};
    render_sample.source_time = composition_time(410U);
    render_sample.payload = (MeisoByteView){render_payload, render_size};

    CHECK(reject_old_production_epoch(
        fixture, wifi_source, 77U, &wifi_sample
    ));
    CHECK(reject_old_production_epoch(
        fixture, render_source, 46U, &render_sample
    ));
    CHECK(meiso_stream_begin_producer(
        fixture->wifi_stream, EDGE_OWNER, 178U
    ) == MEISO_STREAM_PRODUCER_SWITCHED);
    CHECK(meiso_stream_begin_producer(
        fixture->render_stream, EDGE_OWNER, 418U
    ) == MEISO_STREAM_PRODUCER_SWITCHED);
    CHECK(meiso_stream_count(fixture->wifi_stream) == 0U &&
          meiso_stream_count(fixture->render_stream) == 0U);

    wifi_source->work.value.stream_sample.production_epoch = 88U;
    wifi_source->work.value.stream_sample.sample = wifi_sample;
    CHECK(dispatch_source(
        &fixture->tcp.edge,
        &fixture->tcp.host,
        wifi_source,
        NULL,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          route_key_equal(
              event.route, route_key(&fixture->tcp, ROUTE_WIFI_DATA)
          ) && event.production_epoch == 88U &&
          event.stream_sample.key.epoch == 177U &&
          event.stream_sample.key.sequence == 3U);
    receive = meiso_stream_receive(
        fixture->wifi_stream, &event.stream_sample, NULL
    );
    CHECK(receive.code == MEISO_STREAM_RECEIVE_EPOCH_MISMATCH &&
          meiso_stream_count(fixture->wifi_stream) == 0U);

    render_source->work.value.stream_sample.production_epoch = 52U;
    render_source->work.value.stream_sample.sample = render_sample;
    CHECK(dispatch_source(
        &fixture->tcp.edge,
        &fixture->tcp.host,
        render_source,
        NULL,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          route_key_equal(
              event.route, route_key(&fixture->tcp, ROUTE_RENDER_RESULT)
          ) && event.production_epoch == 52U &&
          event.stream_sample.key.epoch == 417U &&
          event.stream_sample.key.sequence == 7U);
    receive = meiso_stream_receive(
        fixture->render_stream, &event.stream_sample, NULL
    );
    CHECK(receive.code == MEISO_STREAM_RECEIVE_EPOCH_MISMATCH &&
          meiso_stream_count(fixture->render_stream) == 0U);

    wifi_sample.key.epoch = 178U;
    wifi_sample.key.sequence = 1U;
    wifi_sample.source_time = composition_time(420U);
    wifi_source->work.value.stream_sample.production_epoch = 88U;
    wifi_source->work.value.stream_sample.sample = wifi_sample;
    CHECK(dispatch_source(
        &fixture->tcp.edge,
        &fixture->tcp.host,
        wifi_source,
        NULL,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          route_key_equal(
              event.route, route_key(&fixture->tcp, ROUTE_WIFI_DATA)
          ) && event.production_epoch == 88U &&
          event.stream_sample.key.epoch == 178U &&
          event.stream_sample.key.sequence == 1U &&
          meiso_wifi_telemetry_sample_decode(
              event.stream_sample.payload, &decoded_wifi
          ) == MEISO_WIFI_TELEMETRY_CODEC_OK &&
          decoded_wifi.link_state && decoded_wifi.signal_mbm == -4600);
    receive = meiso_stream_receive(
        fixture->wifi_stream, &event.stream_sample, NULL
    );
    CHECK(receive.code == MEISO_STREAM_RECEIVE_ACCEPTED &&
          receive.flags == MEISO_STREAM_RECEIVE_FLAG_NONE &&
          receive.missing_before == 0U &&
          meiso_stream_count(fixture->wifi_stream) == 1U &&
          meiso_stream_peek(fixture->wifi_stream, &queued) &&
          queued.key.epoch == 178U && queued.key.sequence == 1U &&
          meiso_stream_pop(fixture->wifi_stream));

    render_sample.key.epoch = 418U;
    render_sample.key.sequence = 1U;
    render_sample.source_time = composition_time(430U);
    render_source->work.value.stream_sample.production_epoch = 52U;
    render_source->work.value.stream_sample.sample = render_sample;
    CHECK(dispatch_source(
        &fixture->tcp.edge,
        &fixture->tcp.host,
        render_source,
        NULL,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          route_key_equal(
              event.route, route_key(&fixture->tcp, ROUTE_RENDER_RESULT)
          ) && event.production_epoch == 52U &&
          event.stream_sample.key.epoch == 418U &&
          event.stream_sample.key.sequence == 1U &&
          meiso_composition_scene_render_result_decode(
              event.stream_sample.payload, &decoded_render
          ) == MEISO_COMPOSITION_SCENE_CODEC_OK &&
          decoded_render.applied_scene_state_version == 6U &&
          decoded_render.active_plan_generation == 2U &&
          decoded_render.world_generation == world->world_generation);
    receive = meiso_stream_receive(
        fixture->render_stream, &event.stream_sample, NULL
    );
    CHECK(receive.code == MEISO_STREAM_RECEIVE_ACCEPTED &&
          receive.flags == MEISO_STREAM_RECEIVE_FLAG_NONE &&
          receive.missing_before == 0U &&
          meiso_stream_count(fixture->render_stream) == 1U &&
          meiso_stream_peek(fixture->render_stream, &queued) &&
          queued.key.epoch == 418U && queued.key.sequence == 1U &&
          queued.payload.size == render_size &&
          memcmp(queued.payload.data, render_payload, render_size) == 0 &&
          meiso_composition_scene_store_record_result_published(
              fixture->scene_store, world, 418U, 1U
          ) &&
          meiso_stream_pop(fixture->render_stream));
    return true;
}

static bool catalog_generation_two_lifecycle(
    SemanticFixture *fixture,
    MeisoByteView cube,
    const MeisoContentId *cube_id,
    const MeisoCompositionSceneDesired *scene,
    const MeisoCompositionSceneWorldRef *old_world,
    const uint8_t old_token[MEISO_SHA256_DIGEST_SIZE],
    MeisoCompositionSceneWorldRef *out_world,
    uint8_t out_token[MEISO_SHA256_DIGEST_SIZE]
)
{
    MeisoCompositionSceneDesired decoded_scene;
    MeisoCompositionSceneWorldRef new_world;
    MeisoCompositionCoordinatorPlanView active;
    MeisoRuntimeBlobSnapshot host_blob;
    MeisoRuntimeBlobSnapshot edge_blob;
    MeisoRuntimeSnapshot host_runtime;
    MeisoRuntimeSnapshot edge_runtime;
    MeisoSessionSnapshot host_session;
    MeisoSessionSnapshot edge_session;
    MeisoStateView current;
    MeisoStateOffer offer;
    MeisoBlobView cached;
    uint8_t new_token[MEISO_SHA256_DIGEST_SIZE];

    CHECK(fixture != NULL && cube_id != NULL && scene != NULL &&
          old_world != NULL && old_token != NULL && out_world != NULL &&
          out_token != NULL &&
          replace_catalog_generation(&fixture->tcp, 2U) &&
          assert_catalog_replacement_closed_gates(fixture) &&
          start_catalog_generation_two_coordinator(
              fixture, cube_id, old_world, old_token
          ) &&
          meiso_blob_store_count(fixture->blob_store) == 1U &&
          meiso_blob_store_lookup(
              fixture->blob_store, cube_id, &cached
          ) && cached.content.size == cube.size &&
          deliver_wifi_desired_version(
              fixture, 2U, MEISO_STATE_PUBLISH_NEW
          ) &&
          deliver_scene_state_without_prepare(
              fixture,
              6U,
              scene,
              &decoded_scene,
              MEISO_STATE_PUBLISH_NEW
          ));
    CHECK(meiso_composition_scene_store_prepare(
        fixture->scene_store,
        HOST_OWNER.incarnation,
        6U,
        2U,
        &decoded_scene
    ) == MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB);
    CHECK(republish_cube_duplicate_over_scheduler(
        fixture, cube, cube_id, 2U
    ));
    CHECK(meiso_composition_scene_store_prepare(
        fixture->scene_store,
        HOST_OWNER.incarnation,
        6U,
        2U,
        &decoded_scene
    ) == MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(activate_catalog_generation_two_plan(
        fixture, old_world, old_token, &new_world, new_token
    ));
    CHECK(meiso_composition_coordinator_active(
        fixture->coordinator, &active
    ) && active.ref.desired_owner_incarnation == HOST_OWNER.incarnation &&
          active.ref.desired_state_version == 6U &&
          active.ref.plan_generation == 2U &&
          active.plan != NULL && active.plan->capability_generation == 2U &&
          !active.prepared_token_present);
    CHECK(deliver_catalog_generation_two_actuals(
        fixture, cube.size, &new_world
    ));
    CHECK(restore_generation_two_streams(fixture, &new_world));
    *out_world = new_world;
    memcpy(out_token, new_token, MEISO_SHA256_DIGEST_SIZE);

    CHECK(meiso_runtime_blob_snapshot(
        fixture->tcp.host.runtime, &host_blob
    ) && meiso_runtime_blob_snapshot(
        fixture->tcp.edge.runtime, &edge_blob
    ) && host_blob.transfer_id_high_water == 2U &&
          edge_blob.transfer_id_high_water == 2U &&
          !host_blob.transfer_active && !host_blob.begin_ready &&
          !host_blob.publish_pending && !host_blob.result_pending &&
          !edge_blob.transfer_active && !edge_blob.begin_ready &&
          !edge_blob.publish_pending && !edge_blob.result_pending);
    CHECK(meiso_state_publisher_current(
        fixture->wifi_desired.publisher, &current
    ) && current.version == 2U &&
          meiso_state_publisher_current(
              fixture->scene_desired.publisher, &current
          ) && current.version == 6U &&
          meiso_state_publisher_current(
              fixture->plan_desired.publisher, &current
          ) && current.version == 6U &&
          meiso_state_publisher_current(
              fixture->wifi_actual.publisher, &current
          ) && current.version == 2U &&
          meiso_state_publisher_current(
              fixture->presentation_actual.publisher, &current
          ) && current.version == 3U &&
          meiso_state_publisher_current(
              fixture->scene_actual.publisher, &current
          ) && current.version == 7U);
    CHECK(!meiso_state_publisher_next_offer(
              fixture->wifi_desired.publisher, &offer
          ) &&
          !meiso_state_publisher_next_offer(
              fixture->scene_desired.publisher, &offer
          ) &&
          !meiso_state_publisher_next_offer(
              fixture->plan_desired.publisher, &offer
          ) &&
          !meiso_state_publisher_next_offer(
              fixture->wifi_actual.publisher, &offer
          ) &&
          !meiso_state_publisher_next_offer(
              fixture->presentation_actual.publisher, &offer
          ) &&
          !meiso_state_publisher_next_offer(
              fixture->scene_actual.publisher, &offer
          ));
    CHECK(meiso_runtime_snapshot(
        fixture->tcp.host.runtime, &host_runtime
    ) && meiso_runtime_snapshot(
        fixture->tcp.edge.runtime, &edge_runtime
    ) && host_runtime.active_catalog_generation == 2U &&
          edge_runtime.active_catalog_generation == 2U &&
          meiso_session_snapshot(
              fixture->tcp.host.session, &host_session
          ) && meiso_session_snapshot(
              fixture->tcp.edge.session, &edge_session
          ) && host_session.state == MEISO_SESSION_STATE_READY &&
          edge_session.state == MEISO_SESSION_STATE_READY &&
          host_session.failure == MEISO_SESSION_FAILURE_NONE &&
          edge_session.failure == MEISO_SESSION_FAILURE_NONE &&
          host_session.session_id_known && edge_session.session_id_known &&
          meiso_session_id_equal(
              host_session.session_id, edge_session.session_id
          ) && !host_session.outbound_pending &&
          !edge_session.outbound_pending &&
          !host_session.local_close_reason_known &&
          !edge_session.local_close_reason_known &&
          !host_session.peer_close_reason_known &&
          !edge_session.peer_close_reason_known);
    return true;
}

static bool make_distinct_mesh(
    uint8_t storage[512],
    MeisoByteView *out_mesh,
    MeisoContentId *out_content_id,
    const MeisoContentId *original_content_id
)
{
    const MeisoCompositionSceneMesh *cube =
        meiso_composition_scene_canonical_cube();
    MeisoCompositionSceneMeshVertex vertices[8];
    uint32_t indices[36];
    MeisoCompositionSceneMesh changed;
    size_t size = 0U;

    CHECK(storage != NULL && out_mesh != NULL && out_content_id != NULL &&
          original_content_id != NULL && cube != NULL &&
          cube->vertex_count == 8U && cube->index_count == 36U);
    memcpy(vertices, cube->vertices, sizeof(vertices));
    memcpy(indices, cube->indices, sizeof(indices));
    vertices[0].position[0] = -0.75F;
    changed = *cube;
    changed.vertices = vertices;
    changed.indices = indices;
    CHECK(meiso_composition_scene_mesh_encode(
        &changed, (MeisoMutableBuffer){storage, 512U}, &size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    *out_mesh = (MeisoByteView){storage, size};
    CHECK(meiso_composition_scene_mesh_content_id(
        *out_mesh, out_content_id
    ) && meiso_composition_scene_mesh_blob_verify(
        out_content_id, *out_mesh, NULL
    ) && memcmp(
        out_content_id, original_content_id, sizeof(*out_content_id)
    ) != 0);
    return true;
}

static bool begin_partial_mesh_transfer(
    SemanticFixture *fixture,
    MeisoByteView mesh,
    const MeisoContentId *content_id,
    MeisoRuntimeBlobTransferRef *out_transfer
)
{
    TxSource *begin = &fixture->tcp.host_sources[HOST_BLOB_BEGIN];
    TxSource *chunk = &fixture->tcp.host_sources[HOST_BLOB_CHUNK];
    TxSource *result = &fixture->tcp.edge_sources[EDGE_BLOB_RESULT];
    MeisoRuntimeSchedulerStepResult begin_step;
    MeisoRuntimeEvent edge_event;
    MeisoRuntimeEvent host_event;
    MeisoRuntimeBlobSnapshot host_blob;
    MeisoRuntimeBlobSnapshot edge_blob;
    MeisoBlobStats before;
    MeisoBlobStats after;

    CHECK(content_id != NULL && out_transfer != NULL && mesh.data != NULL &&
          mesh.size != 0U && meiso_blob_store_stats(
              fixture->blob_store, &before
          ) && meiso_blob_store_count(fixture->blob_store) == 1U &&
          !meiso_blob_store_lookup(
              fixture->blob_store, content_id, &(MeisoBlobView){0}
          ));
    begin->work.value.blob_begin.content_id = *content_id;
    begin->work.value.blob_begin.total_size = mesh.size;
    CHECK(dispatch_source(
        &fixture->tcp.host,
        &fixture->tcp.edge,
        begin,
        &begin_step,
        &edge_event
    ));
    CHECK(begin_step.completion.generated_blob_transfer_present &&
          edge_event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          edge_event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN &&
          route_key_equal(
              edge_event.route, route_key(&fixture->tcp, ROUTE_MESH_BLOB)
          ));
    *out_transfer = begin_step.completion.generated_blob_transfer;
    CHECK(out_transfer->transfer_id == 3U &&
          edge_event.blob_transfer.transfer_id == out_transfer->transfer_id &&
          edge_event.blob_transfer.total_size == mesh.size &&
          memcmp(
              &edge_event.blob_transfer.content_id,
              content_id,
              sizeof(*content_id)
          ) == 0);
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

    chunk->work.value.blob_chunk.transfer = *out_transfer;
    chunk->work.value.blob_chunk.offset = 0U;
    chunk->work.value.blob_chunk.payload = mesh;
    CHECK(dispatch_source(
        &fixture->tcp.host,
        &fixture->tcp.edge,
        chunk,
        NULL,
        &edge_event
    ));
    CHECK(edge_event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          edge_event.blob_operation == MEISO_RUNTIME_BLOB_CHUNK &&
          edge_event.blob_offset == 0U &&
          edge_event.blob_payload.size == mesh.size &&
          memcmp(edge_event.blob_payload.data, mesh.data, mesh.size) == 0);
    CHECK(meiso_runtime_blob_snapshot(
        fixture->tcp.host.runtime, &host_blob
    ) && meiso_runtime_blob_snapshot(
        fixture->tcp.edge.runtime, &edge_blob
    ) && host_blob.transfer_active && host_blob.begin_ready &&
          !host_blob.publish_pending && !host_blob.result_pending &&
          host_blob.transfer.transfer_id == out_transfer->transfer_id &&
          host_blob.next_offset == mesh.size &&
          host_blob.transfer_id_high_water == 3U &&
          edge_blob.transfer_active && edge_blob.begin_ready &&
          !edge_blob.publish_pending && !edge_blob.result_pending &&
          edge_blob.transfer.transfer_id == out_transfer->transfer_id &&
          edge_blob.next_offset == mesh.size &&
          edge_blob.transfer_id_high_water == 3U);
    CHECK(meiso_blob_store_stats(fixture->blob_store, &after) &&
          memcmp(&before, &after, sizeof(before)) == 0 &&
          meiso_blob_store_count(fixture->blob_store) == 1U &&
          !meiso_blob_store_lookup(
              fixture->blob_store, content_id, &(MeisoBlobView){0}
          ));
    return true;
}

static bool read_transport_reset(Peer *peer)
{
    struct timespec deadline;
    MeisoSessionRuntimeMessage message;

    CHECK(peer != NULL && make_deadline(&deadline));
    while (remaining_poll_ms(&deadline) > 0) {
        const MeisoPosixTcpReadStatus status =
            meiso_posix_tcp_on_readable(peer->adapter, &message);

        if (status == MEISO_POSIX_TCP_READ_TRANSPORT_LOST) {
            return true;
        }
        CHECK(status == MEISO_POSIX_TCP_READ_PROGRESS ||
              status == MEISO_POSIX_TCP_READ_WOULD_BLOCK ||
              status == MEISO_POSIX_TCP_READ_INTERRUPTED);
        if (status != MEISO_POSIX_TCP_READ_PROGRESS) {
            CHECK(wait_fd(peer->fd, POLLIN, &deadline));
        }
    }
    return false;
}

static bool reset_partial_transfer_connection(
    SemanticFixture *fixture,
    const MeisoRuntimeBlobTransferRef *partial_transfer,
    const MeisoContentId *partial_content_id,
    const MeisoCompositionSceneWorldRef *active_world
)
{
    const struct linger reset = {1, 0};
    MeisoSessionSnapshot host_session;
    MeisoSessionSnapshot edge_session;
    MeisoRuntimeBlobSnapshot host_blob;
    MeisoRuntimeBlobSnapshot edge_blob;
    MeisoRuntimeSchedulerStepResult step;
    MeisoCompositionSceneWorldView world;
    uint64_t host_boundaries[TEST_HOST_SOURCE_COUNT] = {0U};
    uint64_t edge_boundaries[TEST_EDGE_SOURCE_COUNT] = {0U};
    int host_error = 0;
    int edge_error = 0;
    size_t index;

    CHECK(partial_transfer != NULL && partial_content_id != NULL &&
          active_world != NULL);
    for (index = 0U; index < fixture->tcp.host.source_count; ++index) {
        host_boundaries[index] =
            fixture->tcp.host.sources[index].boundaries;
    }
    for (index = 0U; index < fixture->tcp.edge.source_count; ++index) {
        edge_boundaries[index] =
            fixture->tcp.edge.sources[index].boundaries;
    }
    CHECK(setsockopt(
        fixture->tcp.edge.fd,
        SOL_SOCKET,
        SO_LINGER,
        &reset,
        (socklen_t)sizeof(reset)
    ) == 0);
    meiso_posix_tcp_abort(fixture->tcp.edge.adapter, ECONNRESET);
    close_fd(&fixture->tcp.edge.fd);
    CHECK(read_transport_reset(&fixture->tcp.host));
    CHECK(meiso_posix_tcp_last_error(
        fixture->tcp.host.adapter, &host_error
    ) && host_error == ECONNRESET);
    CHECK(meiso_posix_tcp_last_error(
        fixture->tcp.edge.adapter, &edge_error
    ) && edge_error == ECONNRESET);
    CHECK(meiso_session_snapshot(
        fixture->tcp.host.session, &host_session
    ) && meiso_session_snapshot(
        fixture->tcp.edge.session, &edge_session
    ) && host_session.state == MEISO_SESSION_STATE_FAILED &&
          edge_session.state == MEISO_SESSION_STATE_FAILED &&
          host_session.failure == MEISO_SESSION_FAILURE_TRANSPORT_LOST &&
          edge_session.failure == MEISO_SESSION_FAILURE_TRANSPORT_LOST &&
          !host_session.outbound_pending && !edge_session.outbound_pending);
    memset(&step, 0xa5, sizeof(step));
    CHECK(meiso_runtime_scheduler_step(
        fixture->tcp.host.scheduler, &step
    ) == MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL);
    memset(&step, 0xa5, sizeof(step));
    CHECK(meiso_runtime_scheduler_step(
        fixture->tcp.edge.scheduler, &step
    ) == MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL);
    CHECK(meiso_runtime_blob_snapshot(
        fixture->tcp.host.runtime, &host_blob
    ) && meiso_runtime_blob_snapshot(
        fixture->tcp.edge.runtime, &edge_blob
    ) && !host_blob.core_ready && !edge_blob.core_ready &&
          !host_blob.transfer_active && !host_blob.begin_ready &&
          !host_blob.publish_pending && !host_blob.result_pending &&
          !edge_blob.transfer_active && !edge_blob.begin_ready &&
          !edge_blob.publish_pending && !edge_blob.result_pending &&
          host_blob.transfer_id_high_water == 3U &&
          edge_blob.transfer_id_high_water == 3U);
    CHECK(meiso_runtime_send_blob_commit(
        fixture->tcp.host.runtime, partial_transfer
    ) == MEISO_RUNTIME_STALE_SESSION);
    for (index = 0U; index < fixture->tcp.host.source_count; ++index) {
        CHECK(fixture->tcp.host.sources[index].boundaries ==
                  host_boundaries[index] + 1U &&
              fixture->tcp.host.sources[index].last_boundary ==
                  MEISO_RUNTIME_SCHEDULER_BOUNDARY_SESSION_TERMINAL &&
              fixture->tcp.host.sources[index].last_boundary_generation ==
                  0U);
    }
    for (index = 0U; index < fixture->tcp.edge.source_count; ++index) {
        CHECK(fixture->tcp.edge.sources[index].boundaries ==
                  edge_boundaries[index] + 1U &&
              fixture->tcp.edge.sources[index].last_boundary ==
                  MEISO_RUNTIME_SCHEDULER_BOUNDARY_SESSION_TERMINAL &&
              fixture->tcp.edge.sources[index].last_boundary_generation ==
                  0U);
    }
    CHECK(meiso_runtime_scheduler_step(
        fixture->tcp.host.scheduler, &step
    ) == MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL);
    CHECK(meiso_runtime_scheduler_step(
        fixture->tcp.edge.scheduler, &step
    ) == MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL);
    for (index = 0U; index < fixture->tcp.host.source_count; ++index) {
        CHECK(fixture->tcp.host.sources[index].boundaries ==
              host_boundaries[index] + 1U);
    }
    for (index = 0U; index < fixture->tcp.edge.source_count; ++index) {
        CHECK(fixture->tcp.edge.sources[index].boundaries ==
              edge_boundaries[index] + 1U);
    }
    CHECK(meiso_blob_store_count(fixture->blob_store) == 1U &&
          !meiso_blob_store_lookup(
              fixture->blob_store,
              partial_content_id,
              &(MeisoBlobView){0}
          ) && meiso_composition_scene_store_active(
              fixture->scene_store, &world
          ) && memcmp(
              &world.ref, active_world, sizeof(world.ref)
          ) == 0);
    return true;
}

static bool initialize_reconnected_semantic_view(
    SemanticFixture *fixture,
    const SemanticFixture *old_fixture,
    MeisoSessionId old_session_id,
    const MeisoContentId *cube_id,
    const MeisoContentId *partial_content_id,
    const MeisoCompositionSceneWorldRef *old_world,
    const uint8_t old_token[MEISO_SHA256_DIGEST_SIZE]
)
{
    MeisoCompositionCoordinatorConfig config;
    MeisoCompositionSceneControlScope scope_before;
    MeisoCompositionSceneControlScope scope_after;
    MeisoCompositionCoordinatorPlanView view;
    MeisoCompositionCoordinatorObservation observation;
    MeisoCompositionCoordinatorStats stats;
    MeisoCompositionScenePresentationStatus status;
    MeisoCompositionSceneWorldView world;
    MeisoCompositionSceneWorldRef output_world;
    MeisoRuntimeProductionSnapshot host_wifi;
    MeisoRuntimeProductionSnapshot edge_wifi;
    MeisoRuntimeProductionSnapshot host_scene;
    MeisoRuntimeProductionSnapshot edge_scene;
    MeisoRuntimeBlobSnapshot old_host_blob;
    MeisoRuntimeBlobSnapshot old_edge_blob;
    MeisoRuntimeSchedulerStepResult old_step;
    MeisoSessionSnapshot host_session;
    MeisoSessionSnapshot edge_session;
    MeisoSessionId new_session_id;
    MeisoBlobView cached;
    const size_t required = meiso_composition_coordinator_memory_required();

    CHECK(fixture != NULL && old_fixture != NULL && cube_id != NULL &&
          partial_content_id != NULL &&
          old_world != NULL && old_token != NULL &&
          meiso_composition_scene_store_control_scope(
              old_fixture->scene_store, &scope_before
          ) && meiso_composition_scene_store_active(
              old_fixture->scene_store, &world
          ) && memcmp(&world.ref, old_world, sizeof(world.ref)) == 0 &&
          meiso_blob_store_count(old_fixture->blob_store) == 1U &&
          meiso_blob_store_lookup(
              old_fixture->blob_store, cube_id, &cached
          ) && !meiso_blob_store_lookup(
              old_fixture->blob_store,
              partial_content_id,
              &(MeisoBlobView){0}
          ));
    memset(fixture, 0, sizeof(*fixture));
    CHECK(initialize_tcp_fixture_with_nonce_salt(&fixture->tcp, 100U));
    fixture->blob_store = old_fixture->blob_store;
    fixture->scene_store = old_fixture->scene_store;
    fixture->scene_store_arena = old_fixture->scene_store_arena;
    fixture->wifi_desired = old_fixture->wifi_desired;
    fixture->wifi_actual = old_fixture->wifi_actual;
    fixture->scene_desired = old_fixture->scene_desired;
    fixture->plan_desired = old_fixture->plan_desired;
    fixture->presentation_actual = old_fixture->presentation_actual;
    fixture->scene_actual = old_fixture->scene_actual;
    fixture->wifi_stream = old_fixture->wifi_stream;
    fixture->render_stream = old_fixture->render_stream;
    CHECK(fixture->tcp.host.session != old_fixture->tcp.host.session &&
          fixture->tcp.edge.session != old_fixture->tcp.edge.session &&
          fixture->tcp.host.runtime != old_fixture->tcp.host.runtime &&
          fixture->tcp.edge.runtime != old_fixture->tcp.edge.runtime &&
          fixture->tcp.host.scheduler != old_fixture->tcp.host.scheduler &&
          fixture->tcp.edge.scheduler != old_fixture->tcp.edge.scheduler &&
          fixture->tcp.host.adapter != old_fixture->tcp.host.adapter &&
          fixture->tcp.edge.adapter != old_fixture->tcp.edge.adapter &&
          fixture->tcp.host.fd != old_fixture->tcp.host.fd &&
          fixture->tcp.edge.fd != old_fixture->tcp.host.fd);
    CHECK(meiso_session_snapshot(
        fixture->tcp.host.session, &host_session
    ) && meiso_session_snapshot(
        fixture->tcp.edge.session, &edge_session
    ) && host_session.state == MEISO_SESSION_STATE_READY &&
          edge_session.state == MEISO_SESSION_STATE_READY &&
          host_session.session_id_known && edge_session.session_id_known &&
          meiso_session_id_equal(
              host_session.session_id, edge_session.session_id
          ) && !meiso_session_id_equal(
              host_session.session_id, old_session_id
          ) && fixture->tcp.catalog_generation == 1U);
    new_session_id = edge_session.session_id;
    CHECK(meiso_session_snapshot(
        old_fixture->tcp.host.session, &host_session
    ) && meiso_session_snapshot(
        old_fixture->tcp.edge.session, &edge_session
    ) && host_session.state == MEISO_SESSION_STATE_FAILED &&
          edge_session.state == MEISO_SESSION_STATE_FAILED &&
          host_session.failure == MEISO_SESSION_FAILURE_TRANSPORT_LOST &&
          edge_session.failure == MEISO_SESSION_FAILURE_TRANSPORT_LOST &&
          meiso_runtime_blob_snapshot(
              old_fixture->tcp.host.runtime, &old_host_blob
          ) && meiso_runtime_blob_snapshot(
              old_fixture->tcp.edge.runtime, &old_edge_blob
          ) && !old_host_blob.core_ready && !old_edge_blob.core_ready);
    memset(&old_step, 0xa5, sizeof(old_step));
    CHECK(meiso_runtime_scheduler_step(
        old_fixture->tcp.host.scheduler, &old_step
    ) == MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL);
    memset(&old_step, 0xa5, sizeof(old_step));
    CHECK(meiso_runtime_scheduler_step(
        old_fixture->tcp.edge.scheduler, &old_step
    ) == MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL);
    CHECK(reject_stale_queued_route(
        &fixture->tcp,
        &fixture->tcp.host,
        &fixture->tcp.host_sources[HOST_WIFI_DESIRED],
        ROUTE_WIFI_DESIRED,
        2U
    ));
    CHECK(reject_stale_queued_route(
        &fixture->tcp,
        &fixture->tcp.edge,
        &fixture->tcp.edge_sources[EDGE_WIFI_ACTUAL],
        ROUTE_WIFI_ACTUAL,
        2U
    ));
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
    CHECK(!host_wifi.actual_present && !edge_wifi.actual_present &&
          !host_wifi.data_active && !edge_wifi.data_active &&
          !host_scene.actual_present && !edge_scene.actual_present &&
          !host_scene.data_active && !edge_scene.data_active &&
          host_wifi.production_epoch_high_water == 0U &&
          edge_wifi.production_epoch_high_water == 0U &&
          host_scene.production_epoch_high_water == 0U &&
          edge_scene.production_epoch_high_water == 0U);
    CHECK(meiso_composition_scene_store_active(
        fixture->scene_store, &world
    ) && memcmp(&world.ref, old_world, sizeof(world.ref)) == 0);

    memset(&config, 0, sizeof(config));
    config.scene_store = fixture->scene_store;
    config.session_id = new_session_id;
    config.edge_incarnation = EDGE_OWNER.incarnation;
    config.capability_generation = 1U;
    config.edge_clock_epoch = 77U;
    CHECK(allocate_arena(&fixture->coordinator_arena, required));
    CHECK(meiso_composition_coordinator_init(
        fixture->coordinator_arena,
        required,
        &config,
        &fixture->coordinator
    ) == MEISO_COMPOSITION_COORDINATOR_INIT_OK);
    CHECK(fixture->coordinator != old_fixture->coordinator);
    CHECK(meiso_composition_coordinator_begin_owner(
        fixture->coordinator, HOST_OWNER.incarnation
    ) == MEISO_COMPOSITION_COORDINATOR_OWNER_STARTED);
    CHECK(meiso_composition_scene_store_control_scope(
        fixture->scene_store, &scope_after
    ) && scope_after.desired_owner_incarnation ==
              scope_before.desired_owner_incarnation &&
          scope_after.control_scope_generation ==
              scope_before.control_scope_generation + 1U &&
          !meiso_composition_scene_store_active(
              fixture->scene_store, &world
          ) && !meiso_composition_scene_store_candidate(
              fixture->scene_store, &world
          ));
    CHECK(!meiso_composition_coordinator_observation(
              old_fixture->coordinator, &observation
          ) && !meiso_composition_coordinator_candidate(
              old_fixture->coordinator, &view
          ) && !meiso_composition_coordinator_active(
              old_fixture->coordinator, &view
          ) && !meiso_composition_coordinator_status(
              old_fixture->coordinator, &status
          ) && !meiso_composition_coordinator_stats(
              old_fixture->coordinator, &stats
          ));
    output_world = *old_world;
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
        old_fixture->coordinator,
        old_token,
        composition_time(500U),
        &output_world
    ) == MEISO_COMPOSITION_COORDINATOR_ACTIVATE_INVALID_ARGUMENT &&
          memcmp(&output_world, old_world, sizeof(output_world)) == 0);
    CHECK(meiso_blob_store_count(fixture->blob_store) == 1U &&
          meiso_blob_store_lookup(
              fixture->blob_store, cube_id, &cached
          ) && !meiso_blob_store_lookup(
              fixture->blob_store,
              partial_content_id,
              &(MeisoBlobView){0}
          ));
    return true;
}

static void destroy_reconnected_semantic_view(SemanticFixture *fixture)
{
    free(fixture->coordinator_arena);
    destroy_tcp_fixture(&fixture->tcp);
    memset(fixture, 0, sizeof(*fixture));
}

static bool activate_reconnected_plan(
    SemanticFixture *fixture,
    const MeisoCompositionSceneWorldRef *old_world,
    const uint8_t old_token[MEISO_SHA256_DIGEST_SIZE],
    MeisoCompositionSceneWorldRef *out_world,
    uint8_t out_token[MEISO_SHA256_DIGEST_SIZE]
)
{
    MeisoCompositionScenePresentationPlan plan =
        plan_value_at_generation(3U, 1U);
    MeisoCompositionCoordinatorPlanView candidate;
    MeisoCompositionCoordinatorPlanView active;
    MeisoCompositionCoordinatorPlanRef expected;
    MeisoRuntimeDesiredState decoded;
    MeisoStateView current;
    MeisoCompositionSceneWorldRef untouched;
    uint8_t suffix[MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE];
    uint8_t nonce[MEISO_SHA256_DIGEST_SIZE];
    uint8_t store_before[TEST_SCENE_STORE_SNAPSHOT_BYTES];
    const size_t store_bytes =
        meiso_composition_scene_store_memory_required();
    size_t suffix_size = 0U;

    CHECK(fixture != NULL && old_world != NULL && old_token != NULL &&
          out_world != NULL && out_token != NULL &&
          plan.plan_generation == 3U && plan.capability_generation == 1U);
    CHECK(meiso_composition_scene_presentation_plan_encode(
        &plan,
        (MeisoMutableBuffer){suffix, sizeof(suffix)},
        &suffix_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_desired_state_expect(
        &fixture->plan_desired,
        7U,
        (MeisoByteView){suffix, suffix_size},
        MEISO_STATE_PUBLISH_REPLACED_PENDING
    ));
    CHECK(deliver_state_over_scheduler(
        &fixture->tcp,
        &fixture->plan_desired,
        true,
        HOST_PLAN_DESIRED,
        EDGE_PLAN_DESIRED_ACCEPT,
        &current
    ));
    CHECK(current.version == 7U &&
          meiso_runtime_desired_state_decode(current.payload, &decoded) &&
          meiso_composition_coordinator_observe_desired(
              fixture->coordinator,
              current.owner.incarnation,
              current.version,
              &decoded,
              composition_time(510U)
          ) == MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING &&
          meiso_composition_coordinator_candidate(
              fixture->coordinator, &candidate
          ));
    expected = candidate.ref;
    fill_nonce(nonce, 0x91U);
    CHECK(meiso_composition_coordinator_report_prepare_outcome(
        fixture->coordinator,
        &expected,
        MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
        composition_time(520U),
        nonce
    ) == MEISO_COMPOSITION_COORDINATOR_REPORT_PREPARED);
    CHECK(meiso_composition_coordinator_candidate(
        fixture->coordinator, &candidate
    ) && candidate.candidate_state ==
              MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED &&
          candidate.prepared_token_present);
    memcpy(out_token, candidate.prepared_token, MEISO_SHA256_DIGEST_SIZE);
    CHECK(memcmp(out_token, old_token, MEISO_SHA256_DIGEST_SIZE) != 0);
    plan.prepared_token_echo_present = true;
    memcpy(
        plan.prepared_token_echo, out_token, MEISO_SHA256_DIGEST_SIZE
    );
    CHECK(meiso_composition_scene_presentation_plan_encode(
        &plan,
        (MeisoMutableBuffer){suffix, sizeof(suffix)},
        &suffix_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_desired_state(
        &fixture->plan_desired,
        8U,
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
    CHECK(current.version == 8U &&
          meiso_runtime_desired_state_decode(current.payload, &decoded) &&
          meiso_composition_coordinator_observe_desired(
              fixture->coordinator,
              current.owner.incarnation,
              current.version,
              &decoded,
              composition_time(530U)
          ) == MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING);
    untouched = *old_world;
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
        fixture->coordinator,
        old_token,
        composition_time(540U),
        &untouched
    ) == MEISO_COMPOSITION_COORDINATOR_ACTIVATE_TOKEN_MISMATCH &&
          memcmp(&untouched, old_world, sizeof(untouched)) == 0);
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
        fixture->coordinator,
        out_token,
        composition_time(550U),
        out_world
    ) == MEISO_COMPOSITION_COORDINATOR_ACTIVATE_ACTIVATED);
    CHECK(out_world->desired_owner_incarnation == HOST_OWNER.incarnation &&
          out_world->desired_state_version == 7U &&
          out_world->plan_generation == 3U &&
          out_world->world_generation == old_world->world_generation + 1U &&
          meiso_composition_coordinator_active(
              fixture->coordinator, &active
          ) && active.ref.desired_state_version == 8U &&
          active.ref.plan_generation == 3U &&
          active.plan != NULL && active.plan->capability_generation == 1U);
    CHECK(store_bytes <= sizeof(store_before));
    memcpy(store_before, fixture->scene_store_arena, store_bytes);
    CHECK(!meiso_composition_scene_store_record_result_published(
        fixture->scene_store, old_world, 418U, 1U
    ));
    CHECK(memcmp(
        store_before, fixture->scene_store_arena, store_bytes
    ) == 0);
    return true;
}

static bool deliver_reconnected_actuals(
    SemanticFixture *fixture,
    size_t cube_size,
    const MeisoCompositionSceneWorldRef *world
)
{
    const MeisoWifiTelemetryEffectiveParameters wifi_parameters = {
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
            MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM,
        500U
    };
    const MeisoCompositionSceneResourceUsage usage = {
        1U, cube_size, 0U, 0U, 0U, 0U
    };
    uint8_t wifi_suffix[
        MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE
    ];
    uint8_t presentation_suffix[
        MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE
    ];
    uint8_t scene_suffix[MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE];
    MeisoCompositionScenePresentationStatus presentation_status;
    MeisoCompositionSceneStatus scene_status;
    MeisoRuntimeActualState decoded;
    MeisoStateView current;
    MeisoRuntimeProductionSnapshot host_wifi;
    MeisoRuntimeProductionSnapshot edge_wifi;
    MeisoRuntimeProductionSnapshot host_scene;
    MeisoRuntimeProductionSnapshot edge_scene;
    size_t wifi_size = 0U;
    size_t presentation_size = 0U;
    size_t scene_size = 0U;

    CHECK(world != NULL);
    CHECK(meiso_wifi_telemetry_effective_parameters_encode(
        &wifi_parameters,
        (MeisoMutableBuffer){wifi_suffix, sizeof(wifi_suffix)},
        &wifi_size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(publish_actual_state_expect(
        &fixture->wifi_actual,
        3U,
        3U,
        99U,
        (MeisoByteView){wifi_suffix, wifi_size},
        MEISO_STATE_PUBLISH_REPLACED_PENDING
    ));
    CHECK(deliver_state_over_scheduler(
        &fixture->tcp,
        &fixture->wifi_actual,
        false,
        EDGE_WIFI_ACTUAL,
        HOST_WIFI_ACTUAL_ACCEPT,
        &current
    ));
    CHECK(current.version == 3U &&
          meiso_runtime_actual_state_decode(current.payload, &decoded) &&
          decoded.applied_desired.version == 3U &&
          decoded.production_epoch == 99U);

    CHECK(meiso_composition_coordinator_status(
        fixture->coordinator, &presentation_status
    ) && presentation_status.capability_generation == 1U &&
          presentation_status.active_state ==
              MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE &&
          presentation_status.active_plan_generation == 3U);
    CHECK(meiso_composition_scene_presentation_status_encode(
        &presentation_status,
        (MeisoMutableBuffer){
            presentation_suffix, sizeof(presentation_suffix)
        },
        &presentation_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_actual_state_expect(
        &fixture->presentation_actual,
        4U,
        8U,
        61U,
        (MeisoByteView){presentation_suffix, presentation_size},
        MEISO_STATE_PUBLISH_REPLACED_PENDING
    ));
    CHECK(deliver_state_over_scheduler(
        &fixture->tcp,
        &fixture->presentation_actual,
        false,
        EDGE_PRESENTATION_ACTUAL,
        HOST_PRESENTATION_ACTUAL_ACCEPT,
        &current
    ));
    CHECK(current.version == 4U &&
          meiso_runtime_actual_state_decode(current.payload, &decoded) &&
          decoded.applied_desired.version == 8U &&
          decoded.production_epoch == 61U);

    CHECK(meiso_composition_scene_store_status(
        fixture->scene_store, &usage, &scene_status
    ) && scene_status.reason == MEISO_COMPOSITION_SCENE_REASON_READY &&
          scene_status.active_plan_generation == 3U &&
          scene_status.world_generation == world->world_generation &&
          scene_status.active_object_count == 1U &&
          scene_status.unique_mesh_count == 1U &&
          scene_status.published_blob_count == 1U &&
          scene_status.published_blob_bytes == cube_size);
    CHECK(meiso_composition_scene_status_encode(
        &scene_status,
        (MeisoMutableBuffer){scene_suffix, sizeof(scene_suffix)},
        &scene_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    CHECK(publish_actual_state_expect(
        &fixture->scene_actual,
        8U,
        7U,
        62U,
        (MeisoByteView){scene_suffix, scene_size},
        MEISO_STATE_PUBLISH_REPLACED_PENDING
    ));
    CHECK(deliver_state_over_scheduler(
        &fixture->tcp,
        &fixture->scene_actual,
        false,
        EDGE_SCENE_ACTUAL,
        HOST_SCENE_ACTUAL_ACCEPT,
        &current
    ));
    CHECK(current.version == 8U &&
          meiso_runtime_actual_state_decode(current.payload, &decoded) &&
          decoded.applied_desired.version == 7U &&
          decoded.production_epoch == 62U);
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
    CHECK(host_wifi.data_active && edge_wifi.data_active &&
          host_wifi.production_epoch == 99U &&
          edge_wifi.production_epoch == 99U &&
          host_scene.data_active && edge_scene.data_active &&
          host_scene.production_epoch == 62U &&
          edge_scene.production_epoch == 62U);
    return true;
}

static bool restore_reconnected_streams(
    SemanticFixture *fixture,
    const MeisoCompositionSceneWorldRef *world
)
{
    const MeisoWifiTelemetryFieldMask fields =
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
        MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM;
    TxSource *wifi_source = &fixture->tcp.edge_sources[EDGE_WIFI_STREAM];
    TxSource *render_source =
        &fixture->tcp.edge_sources[EDGE_RENDER_STREAM];
    MeisoWifiTelemetrySample wifi_value;
    MeisoWifiTelemetrySample decoded_wifi;
    MeisoCompositionSceneRenderResult render_value;
    MeisoCompositionSceneRenderResult decoded_render;
    MeisoStreamSample wifi_sample;
    MeisoStreamSample render_sample;
    MeisoStreamSample queued;
    MeisoStreamReceiveResult receive;
    MeisoRuntimeEvent event;
    uint8_t wifi_payload[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    uint8_t render_payload[
        MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE
    ];
    size_t wifi_size = 0U;
    size_t render_size = 0U;

    CHECK(world != NULL && world->plan_generation == 3U &&
          meiso_composition_scene_store_fake_render(
              fixture->scene_store, 77U, 600U, 610U, &render_value
          ) == MEISO_COMPOSITION_SCENE_RENDER_COMPLETE &&
          render_value.applied_scene_owner_incarnation ==
              world->desired_owner_incarnation &&
          render_value.applied_scene_state_version ==
              world->desired_state_version &&
          render_value.active_plan_generation == world->plan_generation &&
          render_value.world_generation == world->world_generation);
    memset(&wifi_value, 0, sizeof(wifi_value));
    wifi_value.selected_fields = fields;
    wifi_value.present_fields = fields;
    wifi_value.link_state = true;
    wifi_value.signal_mbm = -4500;
    CHECK(meiso_wifi_telemetry_sample_encode(
        &wifi_value,
        (MeisoMutableBuffer){wifi_payload, sizeof(wifi_payload)},
        &wifi_size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(meiso_composition_scene_render_result_encode(
        &render_value,
        (MeisoMutableBuffer){render_payload, sizeof(render_payload)},
        &render_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    memset(&wifi_sample, 0, sizeof(wifi_sample));
    wifi_sample.key = (MeisoStreamKey){EDGE_OWNER, 178U, 1U};
    wifi_sample.source_time = composition_time(600U);
    wifi_sample.payload = (MeisoByteView){wifi_payload, wifi_size};
    memset(&render_sample, 0, sizeof(render_sample));
    render_sample.key = (MeisoStreamKey){EDGE_OWNER, 418U, 1U};
    render_sample.source_time = composition_time(610U);
    render_sample.payload = (MeisoByteView){render_payload, render_size};

    CHECK(reject_old_production_epoch(
        fixture, wifi_source, 88U, &wifi_sample
    ));
    CHECK(reject_old_production_epoch(
        fixture, render_source, 52U, &render_sample
    ));
    CHECK(meiso_stream_begin_producer(
        fixture->wifi_stream, EDGE_OWNER, 179U
    ) == MEISO_STREAM_PRODUCER_SWITCHED);
    CHECK(meiso_stream_begin_producer(
        fixture->render_stream, EDGE_OWNER, 419U
    ) == MEISO_STREAM_PRODUCER_SWITCHED);
    CHECK(meiso_stream_count(fixture->wifi_stream) == 0U &&
          meiso_stream_count(fixture->render_stream) == 0U);

    wifi_source->work.value.stream_sample.production_epoch = 99U;
    wifi_source->work.value.stream_sample.sample = wifi_sample;
    CHECK(dispatch_source(
        &fixture->tcp.edge,
        &fixture->tcp.host,
        wifi_source,
        NULL,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          route_key_equal(
              event.route, route_key(&fixture->tcp, ROUTE_WIFI_DATA)
          ) && event.production_epoch == 99U &&
          event.stream_sample.key.epoch == 178U &&
          event.stream_sample.key.sequence == 1U);
    receive = meiso_stream_receive(
        fixture->wifi_stream, &event.stream_sample, NULL
    );
    CHECK(receive.code == MEISO_STREAM_RECEIVE_EPOCH_MISMATCH &&
          meiso_stream_count(fixture->wifi_stream) == 0U);

    render_source->work.value.stream_sample.production_epoch = 62U;
    render_source->work.value.stream_sample.sample = render_sample;
    CHECK(dispatch_source(
        &fixture->tcp.edge,
        &fixture->tcp.host,
        render_source,
        NULL,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          route_key_equal(
              event.route, route_key(&fixture->tcp, ROUTE_RENDER_RESULT)
          ) && event.production_epoch == 62U &&
          event.stream_sample.key.epoch == 418U &&
          event.stream_sample.key.sequence == 1U);
    receive = meiso_stream_receive(
        fixture->render_stream, &event.stream_sample, NULL
    );
    CHECK(receive.code == MEISO_STREAM_RECEIVE_EPOCH_MISMATCH &&
          meiso_stream_count(fixture->render_stream) == 0U);

    wifi_sample.key.epoch = 179U;
    wifi_source->work.value.stream_sample.sample = wifi_sample;
    CHECK(dispatch_source(
        &fixture->tcp.edge,
        &fixture->tcp.host,
        wifi_source,
        NULL,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          route_key_equal(
              event.route, route_key(&fixture->tcp, ROUTE_WIFI_DATA)
          ) && event.production_epoch == 99U &&
          event.stream_sample.key.epoch == 179U &&
          event.stream_sample.key.sequence == 1U &&
          meiso_wifi_telemetry_sample_decode(
              event.stream_sample.payload, &decoded_wifi
          ) == MEISO_WIFI_TELEMETRY_CODEC_OK &&
          decoded_wifi.link_state && decoded_wifi.signal_mbm == -4500);
    receive = meiso_stream_receive(
        fixture->wifi_stream, &event.stream_sample, NULL
    );
    CHECK(receive.code == MEISO_STREAM_RECEIVE_ACCEPTED &&
          receive.flags == MEISO_STREAM_RECEIVE_FLAG_NONE &&
          receive.missing_before == 0U &&
          meiso_stream_count(fixture->wifi_stream) == 1U &&
          meiso_stream_peek(fixture->wifi_stream, &queued) &&
          queued.key.epoch == 179U && queued.key.sequence == 1U &&
          queued.payload.size == wifi_size &&
          memcmp(queued.payload.data, wifi_payload, wifi_size) == 0 &&
          meiso_stream_pop(fixture->wifi_stream) &&
          meiso_stream_count(fixture->wifi_stream) == 0U);

    render_sample.key.epoch = 419U;
    render_source->work.value.stream_sample.sample = render_sample;
    CHECK(dispatch_source(
        &fixture->tcp.edge,
        &fixture->tcp.host,
        render_source,
        NULL,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          route_key_equal(
              event.route, route_key(&fixture->tcp, ROUTE_RENDER_RESULT)
          ) && event.production_epoch == 62U &&
          event.stream_sample.key.epoch == 419U &&
          event.stream_sample.key.sequence == 1U &&
          meiso_composition_scene_render_result_decode(
              event.stream_sample.payload, &decoded_render
          ) == MEISO_COMPOSITION_SCENE_CODEC_OK &&
          decoded_render.applied_scene_owner_incarnation ==
              world->desired_owner_incarnation &&
          decoded_render.applied_scene_state_version ==
              world->desired_state_version &&
          decoded_render.active_plan_generation == world->plan_generation &&
          decoded_render.world_generation == world->world_generation);
    receive = meiso_stream_receive(
        fixture->render_stream, &event.stream_sample, NULL
    );
    CHECK(receive.code == MEISO_STREAM_RECEIVE_ACCEPTED &&
          receive.flags == MEISO_STREAM_RECEIVE_FLAG_NONE &&
          receive.missing_before == 0U &&
          meiso_stream_count(fixture->render_stream) == 1U &&
          meiso_stream_peek(fixture->render_stream, &queued) &&
          queued.key.epoch == 419U && queued.key.sequence == 1U &&
          queued.payload.size == render_size &&
          memcmp(queued.payload.data, render_payload, render_size) == 0 &&
          meiso_composition_scene_store_record_result_published(
              fixture->scene_store, world, 419U, 1U
          ) && meiso_stream_pop(fixture->render_stream) &&
          meiso_stream_count(fixture->render_stream) == 0U);
    return true;
}

static bool reset_and_reconnect_lifecycle(
    SemanticFixture *old_fixture,
    MeisoByteView cube,
    const MeisoContentId *cube_id,
    const MeisoCompositionSceneDesired *scene,
    const MeisoCompositionSceneWorldRef *old_world,
    const uint8_t old_token[MEISO_SHA256_DIGEST_SIZE]
)
{
    SemanticFixture reconnected;
    uint8_t partial_storage[512];
    MeisoByteView partial_mesh;
    MeisoContentId partial_id;
    MeisoRuntimeBlobTransferRef partial_transfer;
    MeisoCompositionSceneDesired decoded_scene;
    MeisoCompositionSceneWorldRef new_world;
    MeisoCompositionCoordinatorPlanView active;
    MeisoCompositionCoordinatorObservation old_observation;
    MeisoCompositionSceneWorldRef untouched_world;
    MeisoRuntimeBlobSnapshot host_blob;
    MeisoRuntimeBlobSnapshot edge_blob;
    MeisoRuntimeSchedulerStepResult old_step;
    MeisoSessionSnapshot old_host_session;
    MeisoSessionSnapshot old_edge_session;
    MeisoSessionSnapshot new_host_session;
    MeisoSessionSnapshot new_edge_session;
    MeisoRuntimeSchedulerSnapshot host_scheduler;
    MeisoRuntimeSchedulerSnapshot edge_scheduler;
    MeisoBlobStats stats_before;
    MeisoBlobStats stats_after;
    MeisoStateView current;
    MeisoStateOffer offer;
    MeisoBlobView cached;
    uint8_t new_token[MEISO_SHA256_DIGEST_SIZE];
    uint64_t host_boundaries[TEST_HOST_SOURCE_COUNT] = {0U};
    uint64_t edge_boundaries[TEST_EDGE_SOURCE_COUNT] = {0U};
    size_t index;

    CHECK(old_fixture != NULL && cube_id != NULL && scene != NULL &&
          old_world != NULL && old_token != NULL &&
          meiso_session_snapshot(
              old_fixture->tcp.host.session, &old_host_session
          ) && meiso_session_snapshot(
              old_fixture->tcp.edge.session, &old_edge_session
          ) && old_host_session.state == MEISO_SESSION_STATE_READY &&
          old_edge_session.state == MEISO_SESSION_STATE_READY &&
          old_host_session.session_id_known &&
          old_edge_session.session_id_known &&
          meiso_session_id_equal(
              old_host_session.session_id, old_edge_session.session_id
          ) && meiso_blob_store_stats(
              old_fixture->blob_store, &stats_before
          ));
    CHECK(make_distinct_mesh(
        partial_storage, &partial_mesh, &partial_id, cube_id
    ));
    CHECK(begin_partial_mesh_transfer(
        old_fixture, partial_mesh, &partial_id, &partial_transfer
    ));
    CHECK(reset_partial_transfer_connection(
        old_fixture, &partial_transfer, &partial_id, old_world
    ));
    CHECK(initialize_reconnected_semantic_view(
        &reconnected,
        old_fixture,
        old_host_session.session_id,
        cube_id,
        &partial_id,
        old_world,
        old_token
    ));
    CHECK(restart_all_state_deliveries(&reconnected));
    CHECK(deliver_wifi_desired_version(
        &reconnected, 3U, MEISO_STATE_PUBLISH_REPLACED_PENDING
    ));
    CHECK(deliver_scene_state_without_prepare(
        &reconnected,
        7U,
        scene,
        &decoded_scene,
        MEISO_STATE_PUBLISH_REPLACED_PENDING
    ));
    CHECK(meiso_composition_scene_store_prepare(
        reconnected.scene_store,
        HOST_OWNER.incarnation,
        7U,
        3U,
        &decoded_scene
    ) == MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB);
    CHECK(meiso_blob_store_count(reconnected.blob_store) == 1U &&
          meiso_blob_store_lookup(
              reconnected.blob_store, cube_id, &cached
          ) && !meiso_blob_store_lookup(
              reconnected.blob_store, &partial_id, &(MeisoBlobView){0}
          ));
    CHECK(republish_cube_duplicate_over_scheduler(
        &reconnected, cube, cube_id, 1U
    ));
    CHECK(meiso_composition_scene_store_prepare(
        reconnected.scene_store,
        HOST_OWNER.incarnation,
        7U,
        3U,
        &decoded_scene
    ) == MEISO_COMPOSITION_SCENE_PREPARE_PREPARED);
    CHECK(activate_reconnected_plan(
        &reconnected,
        old_world,
        old_token,
        &new_world,
        new_token
    ));
    CHECK(deliver_reconnected_actuals(
        &reconnected, cube.size, &new_world
    ));
    CHECK(restore_reconnected_streams(&reconnected, &new_world));

    CHECK(meiso_runtime_blob_snapshot(
        reconnected.tcp.host.runtime, &host_blob
    ) && meiso_runtime_blob_snapshot(
        reconnected.tcp.edge.runtime, &edge_blob
    ) && host_blob.transfer_id_high_water == 1U &&
          edge_blob.transfer_id_high_water == 1U &&
          !host_blob.transfer_active && !host_blob.begin_ready &&
          !host_blob.publish_pending && !host_blob.result_pending &&
          !edge_blob.transfer_active && !edge_blob.begin_ready &&
          !edge_blob.publish_pending && !edge_blob.result_pending);
    CHECK(meiso_blob_store_stats(reconnected.blob_store, &stats_after) &&
          stats_after.publications == stats_before.publications &&
          stats_after.duplicates == stats_before.duplicates + 1U &&
          meiso_blob_store_count(reconnected.blob_store) == 1U &&
          !meiso_blob_store_lookup(
              reconnected.blob_store, &partial_id, &(MeisoBlobView){0}
          ));
    CHECK(meiso_composition_coordinator_active(
        reconnected.coordinator, &active
    ) && active.ref.desired_owner_incarnation == HOST_OWNER.incarnation &&
          active.ref.desired_state_version == 8U &&
          active.ref.plan_generation == 3U &&
          active.plan != NULL && active.plan->capability_generation == 1U &&
          memcmp(new_token, old_token, sizeof(new_token)) != 0);
    CHECK(meiso_state_publisher_current(
        reconnected.wifi_desired.publisher, &current
    ) && current.version == 3U &&
          meiso_state_publisher_current(
              reconnected.scene_desired.publisher, &current
          ) && current.version == 7U &&
          meiso_state_publisher_current(
              reconnected.plan_desired.publisher, &current
          ) && current.version == 8U &&
          meiso_state_publisher_current(
              reconnected.wifi_actual.publisher, &current
          ) && current.version == 3U &&
          meiso_state_publisher_current(
              reconnected.presentation_actual.publisher, &current
          ) && current.version == 4U &&
          meiso_state_publisher_current(
              reconnected.scene_actual.publisher, &current
          ) && current.version == 8U);
    CHECK(!meiso_state_publisher_next_offer(
              reconnected.wifi_desired.publisher, &offer
          ) && !meiso_state_publisher_next_offer(
              reconnected.scene_desired.publisher, &offer
          ) && !meiso_state_publisher_next_offer(
              reconnected.plan_desired.publisher, &offer
          ) && !meiso_state_publisher_next_offer(
              reconnected.wifi_actual.publisher, &offer
          ) && !meiso_state_publisher_next_offer(
              reconnected.presentation_actual.publisher, &offer
          ) && !meiso_state_publisher_next_offer(
              reconnected.scene_actual.publisher, &offer
          ));
    CHECK(meiso_session_snapshot(
        reconnected.tcp.host.session, &new_host_session
    ) && meiso_session_snapshot(
        reconnected.tcp.edge.session, &new_edge_session
    ) && new_host_session.state == MEISO_SESSION_STATE_READY &&
          new_edge_session.state == MEISO_SESSION_STATE_READY &&
          new_host_session.session_id_known &&
          new_edge_session.session_id_known &&
          meiso_session_id_equal(
              new_host_session.session_id, new_edge_session.session_id
          ) && !meiso_session_id_equal(
              new_host_session.session_id, old_host_session.session_id
          ) && !new_host_session.outbound_pending &&
          !new_edge_session.outbound_pending &&
          meiso_runtime_scheduler_snapshot(
              reconnected.tcp.host.scheduler, &host_scheduler
          ) && meiso_runtime_scheduler_snapshot(
              reconnected.tcp.edge.scheduler, &edge_scheduler
          ) && host_scheduler.phase == MEISO_RUNTIME_SCHEDULER_PHASE_READY &&
          edge_scheduler.phase == MEISO_RUNTIME_SCHEDULER_PHASE_READY &&
          host_scheduler.catalog_generation == 1U &&
          edge_scheduler.catalog_generation == 1U);
    CHECK(meiso_session_snapshot(
        old_fixture->tcp.host.session, &old_host_session
    ) && meiso_session_snapshot(
        old_fixture->tcp.edge.session, &old_edge_session
    ) && old_host_session.state == MEISO_SESSION_STATE_FAILED &&
          old_edge_session.state == MEISO_SESSION_STATE_FAILED &&
          old_host_session.failure == MEISO_SESSION_FAILURE_TRANSPORT_LOST &&
          old_edge_session.failure == MEISO_SESSION_FAILURE_TRANSPORT_LOST);
    CHECK(!meiso_composition_coordinator_observation(
              old_fixture->coordinator, &old_observation
          ));
    untouched_world = *old_world;
    CHECK(meiso_composition_coordinator_activate_frame_boundary(
        old_fixture->coordinator,
        old_token,
        composition_time(700U),
        &untouched_world
    ) == MEISO_COMPOSITION_COORDINATOR_ACTIVATE_INVALID_ARGUMENT);
    CHECK(memcmp(
        &untouched_world, old_world, sizeof(untouched_world)
    ) == 0);
    CHECK(meiso_runtime_send_blob_commit(
        old_fixture->tcp.host.runtime, &partial_transfer
    ) == MEISO_RUNTIME_STALE_SESSION);
    for (index = 0U; index < old_fixture->tcp.host.source_count; ++index) {
        host_boundaries[index] =
            old_fixture->tcp.host.sources[index].boundaries;
    }
    for (index = 0U; index < old_fixture->tcp.edge.source_count; ++index) {
        edge_boundaries[index] =
            old_fixture->tcp.edge.sources[index].boundaries;
    }
    memset(&old_step, 0xa5, sizeof(old_step));
    CHECK(meiso_runtime_scheduler_step(
        old_fixture->tcp.host.scheduler, &old_step
    ) == MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL);
    memset(&old_step, 0xa5, sizeof(old_step));
    CHECK(meiso_runtime_scheduler_step(
        old_fixture->tcp.edge.scheduler, &old_step
    ) == MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL);
    for (index = 0U; index < old_fixture->tcp.host.source_count; ++index) {
        CHECK(old_fixture->tcp.host.sources[index].boundaries ==
              host_boundaries[index]);
    }
    for (index = 0U; index < old_fixture->tcp.edge.source_count; ++index) {
        CHECK(old_fixture->tcp.edge.sources[index].boundaries ==
              edge_boundaries[index]);
    }
    destroy_reconnected_semantic_view(&reconnected);
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
          host_scheduler.control_source_count == 9U &&
          host_scheduler.data_source_count == 1U &&
          edge_scheduler.control_source_count == 8U &&
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
    MeisoCompositionSceneDesired scene;
    MeisoCompositionSceneDesired trs_scene;
    MeisoCompositionSceneDesired camera_scene;
    MeisoCompositionSceneDesired shared_scene;
    MeisoCompositionSceneDesired deleted_scene;
    MeisoCompositionSceneWorldRef world;
    MeisoCompositionSceneWorldRef next_world;
    MeisoCompositionSceneWorldRef generation_two_world;
    MeisoCompositionSceneRenderResult baseline_one;
    MeisoCompositionSceneRenderResult trs_result;
    MeisoCompositionSceneRenderResult camera_result;
    MeisoCompositionSceneRenderResult shared_result;
    MeisoCompositionSceneRenderResult deleted_result;
    MeisoRuntimeBlobSnapshot host_blob_before;
    MeisoRuntimeBlobSnapshot host_blob_after;
    MeisoRuntimeBlobSnapshot edge_blob_before;
    MeisoRuntimeBlobSnapshot edge_blob_after;
    MeisoBlobStats blob_stats_before;
    MeisoBlobStats blob_stats_after;
    MeisoStreamStats render_stats;
    MeisoBlobView blob_before;
    MeisoBlobView blob_after;
    MeisoCompositionCoordinatorPlanView initial_plan;
    MeisoCompositionCoordinatorPlanView final_plan;
    uint8_t old_token[MEISO_SHA256_DIGEST_SIZE];
    uint8_t generation_two_token[MEISO_SHA256_DIGEST_SIZE];
    MeisoRuntimeSchedulerSnapshot host_scheduler;
    MeisoRuntimeSchedulerSnapshot edge_scheduler;

    CHECK(initialize_semantic_fixture(&fixture));
    CHECK(deliver_wifi_desired(&fixture));
    CHECK(publish_cube_over_scheduler(
        &fixture, cube_storage, &cube, &cube_id
    ));
    scene = scene_value(cube_id);
    CHECK(deliver_scene_snapshot(&fixture, 1U, &scene));
    CHECK(activate_plan_over_state(&fixture, &world, old_token));
    CHECK(meiso_composition_coordinator_active(
        fixture.coordinator, &initial_plan
    ) && initial_plan.ref.desired_owner_incarnation ==
              HOST_OWNER.incarnation &&
          initial_plan.ref.desired_state_version == 2U &&
          initial_plan.ref.plan_generation == 1U);
    CHECK(deliver_wifi_actual(&fixture));
    CHECK(deliver_composition_actuals(&fixture, cube.size));
    CHECK(assert_independent_production_gates(&fixture));
    CHECK(send_wifi_sample_over_scheduler(&fixture));
    CHECK(send_render_result_over_scheduler(
        &fixture, &world, 42U, 1U, 300U, 310U, &baseline_one
    ));
    CHECK(meiso_runtime_blob_snapshot(
        fixture.tcp.host.runtime, &host_blob_before
    ));
    CHECK(meiso_runtime_blob_snapshot(
        fixture.tcp.edge.runtime, &edge_blob_before
    ));
    CHECK(meiso_blob_store_stats(fixture.blob_store, &blob_stats_before) &&
          meiso_blob_store_lookup(
              fixture.blob_store, &cube_id, &blob_before
          ) && blob_before.content.size == cube.size &&
          meiso_blob_store_count(fixture.blob_store) == 1U &&
          host_blob_before.transfer_id_high_water == 1U &&
          edge_blob_before.transfer_id_high_water == 1U &&
          !host_blob_before.transfer_active &&
          !host_blob_before.begin_ready &&
          !host_blob_before.publish_pending &&
          !host_blob_before.result_pending &&
          !edge_blob_before.transfer_active &&
          !edge_blob_before.begin_ready &&
          !edge_blob_before.publish_pending &&
          !edge_blob_before.result_pending);

    trs_scene = scene;
    trs_scene.objects[0].transform.position[0] = 1.0F;
    CHECK(advance_scene_snapshot(
        &fixture, 2U, &trs_scene, 140U, 43U, 3U, true, cube.size, 1U,
        &world, &baseline_one, &next_world, &trs_result
    ));
    CHECK(memcmp(trs_result.full_target_sha256,
                 baseline_one.full_target_sha256,
                 sizeof(trs_result.full_target_sha256)) != 0 &&
          memcmp(trs_result.thumbnail_rgb565,
                 baseline_one.thumbnail_rgb565,
                 sizeof(trs_result.thumbnail_rgb565)) != 0);
    world = next_world;

    camera_scene = trs_scene;
    camera_scene.camera.position[0] = 0.5F;
    CHECK(advance_scene_snapshot(
        &fixture, 3U, &camera_scene, 150U, 44U, 4U, false, cube.size, 1U,
        &world, &trs_result, &next_world, &camera_result
    ));
    CHECK(memcmp(camera_result.full_target_sha256,
                 trs_result.full_target_sha256,
                 sizeof(camera_result.full_target_sha256)) != 0 &&
          memcmp(camera_result.thumbnail_rgb565,
                 trs_result.thumbnail_rgb565,
                 sizeof(camera_result.thumbnail_rgb565)) != 0);
    world = next_world;

    shared_scene = camera_scene;
    shared_scene.object_count = 2U;
    shared_scene.objects[1] = shared_scene.objects[0];
    shared_scene.objects[1].id =
        (MeisoCompositionSceneObjectId){1U, 2U};
    shared_scene.objects[1].transform.position[0] = -1.0F;
    CHECK(advance_scene_snapshot(
        &fixture, 4U, &shared_scene, 160U, 45U, 5U, false, cube.size, 2U,
        &world, &camera_result, &next_world, &shared_result
    ));
    CHECK(shared_result.unique_mesh_count == 1U &&
          shared_result.object_count == 2U &&
          memcmp(shared_result.full_target_sha256,
                 camera_result.full_target_sha256,
                 sizeof(shared_result.full_target_sha256)) != 0 &&
          memcmp(shared_result.thumbnail_rgb565,
                 camera_result.thumbnail_rgb565,
                 sizeof(shared_result.thumbnail_rgb565)) != 0);
    world = next_world;

    deleted_scene = shared_scene;
    deleted_scene.objects[0] = deleted_scene.objects[1];
    memset(&deleted_scene.objects[1], 0, sizeof(deleted_scene.objects[1]));
    deleted_scene.object_count = 1U;
    CHECK(advance_scene_snapshot(
        &fixture, 5U, &deleted_scene, 170U, 46U, 6U, false, cube.size, 1U,
        &world, &shared_result, &next_world, &deleted_result
    ));
    CHECK(deleted_result.object_count == 1U &&
          deleted_result.unique_mesh_count == 1U &&
          memcmp(deleted_result.full_target_sha256,
                 shared_result.full_target_sha256,
                 sizeof(deleted_result.full_target_sha256)) != 0 &&
          memcmp(deleted_result.thumbnail_rgb565,
                 shared_result.thumbnail_rgb565,
                 sizeof(deleted_result.thumbnail_rgb565)) != 0);
    world = next_world;

    CHECK(meiso_runtime_blob_snapshot(
        fixture.tcp.host.runtime, &host_blob_after
    ));
    CHECK(meiso_runtime_blob_snapshot(
        fixture.tcp.edge.runtime, &edge_blob_after
    ));
    CHECK(meiso_blob_store_stats(fixture.blob_store, &blob_stats_after) &&
          meiso_blob_store_lookup(
              fixture.blob_store, &cube_id, &blob_after
          ) && meiso_blob_store_count(fixture.blob_store) == 1U &&
          blob_after.content.size == cube.size &&
          blob_before.content.size == blob_after.content.size &&
          blob_stats_after.publications == blob_stats_before.publications &&
          blob_stats_after.duplicates == blob_stats_before.duplicates &&
          blob_stats_after.publications == 1U &&
          host_blob_after.transfer_id_high_water ==
              host_blob_before.transfer_id_high_water &&
          edge_blob_after.transfer_id_high_water ==
              edge_blob_before.transfer_id_high_water &&
          !host_blob_after.transfer_active &&
          !host_blob_after.begin_ready &&
          !host_blob_after.publish_pending &&
          !host_blob_after.result_pending &&
          !edge_blob_after.transfer_active &&
          !edge_blob_after.begin_ready &&
          !edge_blob_after.publish_pending &&
          !edge_blob_after.result_pending);
    CHECK(meiso_composition_coordinator_active(
        fixture.coordinator, &final_plan
    ) && memcmp(
              &initial_plan.ref, &final_plan.ref, sizeof(initial_plan.ref)
          ) == 0 && final_plan.active_state ==
              MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE);
    CHECK(meiso_stream_count(fixture.render_stream) == 0U &&
          meiso_stream_stats(fixture.render_stream, &render_stats) &&
          render_stats.accepted == 6U && render_stats.gaps == 0U &&
          render_stats.missing_samples == 0U &&
          render_stats.overflow_drops == 0U &&
          render_stats.overflow_rejections == 0U &&
          render_stats.pops == 6U);
    CHECK(disable_scene_lifecycle(
        &fixture, cube.size, &world, &deleted_result
    ));
    CHECK(meiso_runtime_scheduler_snapshot(
        fixture.tcp.host.scheduler, &host_scheduler
    ));
    CHECK(meiso_runtime_scheduler_snapshot(
        fixture.tcp.edge.scheduler, &edge_scheduler
    ));
    CHECK(host_scheduler.successful_control_slots > 0U &&
          host_scheduler.successful_data_slots == 1U &&
          edge_scheduler.successful_control_slots > 0U &&
          edge_scheduler.successful_data_slots == 8U &&
          host_scheduler.catalog_generation ==
              edge_scheduler.catalog_generation &&
          host_scheduler.catalog_generation == 1U &&
          fixture.tcp.host.runtime_frames_with_multi_write_progress > 0U &&
          fixture.tcp.edge.runtime_frames_with_multi_write_progress > 0U &&
          fixture.tcp.host.runtime_frames_with_multi_read_progress > 0U &&
          fixture.tcp.edge.runtime_frames_with_multi_read_progress > 0U);
    CHECK(catalog_generation_two_lifecycle(
        &fixture,
        cube,
        &cube_id,
        &deleted_scene,
        &world,
        old_token,
        &generation_two_world,
        generation_two_token
    ));
    CHECK(meiso_runtime_scheduler_snapshot(
        fixture.tcp.host.scheduler, &host_scheduler
    ) && meiso_runtime_scheduler_snapshot(
        fixture.tcp.edge.scheduler, &edge_scheduler
    ) && host_scheduler.phase == MEISO_RUNTIME_SCHEDULER_PHASE_READY &&
          edge_scheduler.phase == MEISO_RUNTIME_SCHEDULER_PHASE_READY &&
          host_scheduler.catalog_generation == 2U &&
          edge_scheduler.catalog_generation == 2U &&
          host_scheduler.successful_data_slots == 2U &&
          edge_scheduler.successful_data_slots == 12U);
    CHECK(reset_and_reconnect_lifecycle(
        &fixture,
        cube,
        &cube_id,
        &deleted_scene,
        &generation_two_world,
        generation_two_token
    ));
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
