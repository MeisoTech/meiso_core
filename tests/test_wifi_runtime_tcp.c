#define _POSIX_C_SOURCE 200809L

#include "meiso_posix_tcp.h"
#include "meiso_runtime.h"
#include "wifi_telemetry.h"

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
    TEST_MAX_FRAME_BYTES = 1024,
    TEST_MAX_RUNTIME_BYTES = 768,
    TEST_SESSION_ARENA_BYTES = 4096,
    TEST_ADAPTER_ARENA_BYTES = 4096,
    TEST_RUNTIME_ARENA_BYTES = 8192,
    TEST_RESOURCE_ARENA_BYTES = 4096,
    TEST_IO_BYTES_PER_STEP = 17,
    TEST_POLL_SLICE_MS = 20,
    TEST_DEADLINE_MS = 3000
};

static const uint64_t ENDPOINT_INCARNATION = 501U;
static const uint64_t HOST_INCARNATION = 101U;
static const uint64_t EDGE_INCARNATION = 201U;
static const uint64_t PRODUCTION_EPOCH = 77U;
static const MeisoOwnerScope HOST_OWNER = {MEISO_OWNER_ROLE_HOST, 101U};
static const MeisoOwnerScope EDGE_OWNER = {MEISO_OWNER_ROLE_EDGE, 201U};

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
    uint8_t bytes[TEST_RESOURCE_ARENA_BYTES];
} ResourceArena;

typedef struct {
    int fd;
    SessionArena session_arena;
    AdapterArena adapter_arena;
    RuntimeArena runtime_arena;
    MeisoSession *session;
    MeisoPosixTcp *adapter;
    MeisoRuntime *runtime;
} Peer;

typedef struct {
    Peer host;
    Peer edge;
} Pair;

typedef struct {
    ResourceArena desired_publisher_arena;
    ResourceArena desired_replica_arena;
    ResourceArena actual_publisher_arena;
    ResourceArena actual_replica_arena;
    ResourceArena stream_arena;
    MeisoStatePublisher *desired_publisher;
    MeisoStateReplica *desired_replica;
    MeisoStatePublisher *actual_publisher;
    MeisoStateReplica *actual_replica;
    MeisoStream *stream;
} Resources;

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
        out_deadline->tv_sec += 1;
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
        seconds -= 1;
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
            listener, (const struct sockaddr *)(const void *)&address,
            (socklen_t)sizeof(address)
        ) != 0 || listen(listener, 1) != 0 || getsockname(
            listener, (struct sockaddr *)(void *)&address, &address_size
        ) != 0 || address.sin_family != AF_INET ||
        address.sin_addr.s_addr != htonl(UINT32_C(0x7f000001)) ||
        !set_nonblocking(listener)) {
        goto fail;
    }
    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0 || !set_nonblocking(client)) {
        goto fail;
    }
    if (connect(
            client, (const struct sockaddr *)(const void *)&address,
            address_size
        ) != 0) {
        if (errno != EINPROGRESS || !wait_fd(client, POLLOUT, &deadline) ||
            getsockopt(
                client, SOL_SOCKET, SO_ERROR,
                &connect_error, &connect_error_size
            ) != 0 || connect_error != 0) {
            goto fail;
        }
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
    if (server < 0 || !set_nonblocking(server)) {
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
    const MeisoPeerId host_id = {0x1011U, 0x1012U};
    const MeisoPeerId edge_id = {0x2011U, 0x2012U};
    MeisoSessionConfig config;

    memset(&config, 0, sizeof(config));
    config.local_role = role;
    config.local_peer_id = role == MEISO_SESSION_ROLE_HOST ?
        host_id : edge_id;
    config.expected_peer_id = role == MEISO_SESSION_ROLE_HOST ?
        edge_id : host_id;
    config.local_incarnation = role == MEISO_SESSION_ROLE_HOST ?
        HOST_INCARNATION : EDGE_INCARNATION;
    config.local_nonce = role == MEISO_SESSION_ROLE_HOST ?
        (MeisoSessionNonce){0x3011U + nonce_salt, 0x3012U + nonce_salt} :
        (MeisoSessionNonce){0x4011U + nonce_salt, 0x4012U + nonce_salt};
    config.protocol = (MeisoProtocolVersion){1U, 0U};
    config.profile = *meiso_wifi_telemetry_profile();
    config.limits = (MeisoSessionLimits){
        TEST_MAX_FRAME_BYTES, TEST_MAX_RUNTIME_BYTES, 8U
    };
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
        TEST_MAX_FRAME_BYTES,
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
        snapshot.session_id_known;
}

static bool wait_pair(Pair *pair, const struct timespec *deadline)
{
    struct pollfd items[2];
    Peer *peers[2] = {&pair->host, &pair->edge};
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
        for (index = 0U; index < 2U; ++index) {
            items[index].revents = 0;
        }
    }
    return false;
}

static bool drive_ready(Pair *pair)
{
    struct timespec deadline;
    Peer *peers[2] = {&pair->host, &pair->edge};

    CHECK(make_deadline(&deadline));
    while (remaining_poll_ms(&deadline) > 0) {
        bool advanced = false;
        size_t index;

        for (index = 0U; index < 2U; ++index) {
            MeisoSessionRuntimeMessage runtime_message;

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
                        peers[index]->adapter, &runtime_message
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
        if (session_ready(pair->host.session) &&
            session_ready(pair->edge.session)) {
            return true;
        }
        if (!advanced) {
            CHECK(wait_pair(pair, &deadline));
        }
    }
    return false;
}

static bool initialize_runtime(Peer *peer)
{
    const MeisoRuntimeConfig config = {
        peer->session,
        1U,
        2U,
        MEISO_WIFI_TELEMETRY_RESOURCE_COUNT,
        MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE,
        MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE,
        MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE
    };
    const size_t required = meiso_runtime_memory_required(&config);

    CHECK(required != 0U && required <= sizeof(peer->runtime_arena.bytes));
    CHECK(meiso_runtime_init(
        peer->runtime_arena.bytes, required, &config, &peer->runtime
    ) == MEISO_RUNTIME_INIT_OK);
    return true;
}

static bool initialize_pair(Pair *pair, uint64_t nonce_salt)
{
    MeisoSessionConfig host_config = session_config(
        MEISO_SESSION_ROLE_HOST, nonce_salt
    );
    MeisoSessionConfig edge_config = session_config(
        MEISO_SESSION_ROLE_EDGE, nonce_salt
    );

    memset(pair, 0, sizeof(*pair));
    pair->host.fd = -1;
    pair->edge.fd = -1;
    CHECK(make_tcp_pair(&pair->host.fd, &pair->edge.fd));
    CHECK(initialize_session(
        &pair->host.session_arena, &host_config, &pair->host.session
    ));
    CHECK(initialize_session(
        &pair->edge.session_arena, &edge_config, &pair->edge.session
    ));
    CHECK(initialize_adapter(&pair->host));
    CHECK(initialize_adapter(&pair->edge));
    CHECK(meiso_session_start(pair->edge.session) ==
          MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(pair->host.session) ==
          MEISO_SESSION_START_STARTED);
    CHECK(drive_ready(pair));
    CHECK(initialize_runtime(&pair->host));
    CHECK(initialize_runtime(&pair->edge));
    return true;
}

static void destroy_pair(Pair *pair)
{
    close_fd(&pair->host.fd);
    close_fd(&pair->edge.fd);
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
        const MeisoPosixTcpReadStatus status = meiso_posix_tcp_on_readable(
            peer->adapter, out_message
        );

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

/* On success event byte views remain borrowed from receiver until its next step. */
static bool exchange(Peer *sender, Peer *receiver, MeisoRuntimeEvent *event)
{
    MeisoSessionRuntimeMessage message;

    CHECK(write_frame(sender));
    CHECK(read_runtime(receiver, &message));
    CHECK(meiso_runtime_receive(
        receiver->runtime, &message, event
    ) == MEISO_RUNTIME_OK);
    return true;
}

static bool route_equal(
    MeisoRuntimeRouteKey lhs,
    MeisoRuntimeRouteKey rhs
)
{
    return lhs.catalog_generation == rhs.catalog_generation &&
        meiso_endpoint_id_equal(lhs.endpoint_id, rhs.endpoint_id) &&
        lhs.endpoint_incarnation == rhs.endpoint_incarnation &&
        meiso_resource_id_equal(lhs.resource_id, rhs.resource_id) &&
        lhs.facet == rhs.facet;
}

static MeisoRuntimeRouteKey route_key(MeisoRuntimeFacet facet)
{
    const MeisoResourceDescriptor *resource =
        meiso_wifi_telemetry_resource_for_facet(facet);
    MeisoRuntimeRouteKey key;

    memset(&key, 0, sizeof(key));
    key.catalog_generation = 1U;
    key.endpoint_id = *meiso_wifi_telemetry_endpoint();
    key.endpoint_incarnation = ENDPOINT_INCARNATION;
    if (resource != NULL) {
        key.resource_id = resource->id;
    }
    key.facet = facet;
    return key;
}

static bool activate_catalog(
    Pair *pair,
    const MeisoWifiTelemetryCapability *capability
)
{
    const MeisoRuntimeFacet facets[MEISO_WIFI_TELEMETRY_RESOURCE_COUNT] = {
        MEISO_RUNTIME_FACET_DESIRED,
        MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA
    };
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor routes[MEISO_WIFI_TELEMETRY_RESOURCE_COUNT];
    MeisoRuntimeEndpointDescriptor staged;
    MeisoRuntimeSnapshot host_snapshot;
    MeisoRuntimeSnapshot edge_snapshot;
    MeisoRuntimeEvent event;
    MeisoWifiTelemetryCapability decoded;
    uint8_t capability_wire[MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE];
    size_t capability_size = 0U;
    size_t index;

    CHECK(meiso_wifi_telemetry_capability_encode(
        capability,
        (MeisoMutableBuffer){capability_wire, sizeof(capability_wire)},
        &capability_size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(capability_size == sizeof(capability_wire));
    endpoint = (MeisoRuntimeEndpointDescriptor){
        *meiso_wifi_telemetry_endpoint(),
        ENDPOINT_INCARNATION,
        *meiso_wifi_telemetry_capability_schema(),
        {capability_wire, capability_size}
    };
    memset(routes, 0, sizeof(routes));
    for (index = 0U; index < MEISO_WIFI_TELEMETRY_RESOURCE_COUNT; ++index) {
        routes[index].endpoint_id = endpoint.id;
        routes[index].endpoint_incarnation = endpoint.incarnation;
        routes[index].resource =
            *meiso_wifi_telemetry_resource_for_facet(facets[index]);
        routes[index].facet = facets[index];
    }
    CHECK(meiso_runtime_offer_catalog(
        pair->edge.runtime, 1U, &endpoint, 1U,
        routes, MEISO_WIFI_TELEMETRY_RESOURCE_COUNT
    ) == MEISO_RUNTIME_OK);
    CHECK(exchange(&pair->edge, &pair->host, &event));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED &&
          event.catalog_generation == 1U);
    CHECK(meiso_runtime_staged_endpoint_at(
        pair->host.runtime, 0U, &staged
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_wifi_telemetry_capability_decode(
        staged.capability, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(decoded.supported_fields == capability->supported_fields &&
          decoded.min_period_ms == capability->min_period_ms &&
          decoded.max_period_ms == capability->max_period_ms &&
          decoded.period_step_ms == capability->period_step_ms);
    CHECK(meiso_runtime_accept_catalog(pair->host.runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(exchange(&pair->host, &pair->edge, &event));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED &&
          event.catalog_generation == 1U);
    CHECK(meiso_runtime_snapshot(pair->host.runtime, &host_snapshot));
    CHECK(meiso_runtime_snapshot(pair->edge.runtime, &edge_snapshot));
    CHECK(host_snapshot.active_catalog_present &&
          edge_snapshot.active_catalog_present &&
          host_snapshot.active_catalog_generation == 1U &&
          edge_snapshot.active_catalog_generation == 1U &&
          host_snapshot.active_endpoint_count == 1U &&
          edge_snapshot.active_route_count ==
              MEISO_WIFI_TELEMETRY_RESOURCE_COUNT);
    return true;
}

static bool initialize_resources(Resources *resources)
{
    const MeisoResourceDescriptor *desired =
        meiso_wifi_telemetry_resource_for_facet(
            MEISO_RUNTIME_FACET_DESIRED
        );
    const MeisoResourceDescriptor *actual =
        meiso_wifi_telemetry_resource_for_facet(
            MEISO_RUNTIME_FACET_ACTUAL
        );
    const MeisoResourceDescriptor *data =
        meiso_wifi_telemetry_resource_for_facet(MEISO_RUNTIME_FACET_DATA);
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const MeisoStreamConfig stream_config = {
        MEISO_STREAM_ORDER_STRICT_MONOTONIC,
        MEISO_STREAM_OVERFLOW_REJECT_NEWEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        4U,
        4U,
        0U
    };
    size_t required;

    memset(resources, 0, sizeof(*resources));
    required = meiso_state_publisher_memory_required(desired, &state_config);
    CHECK(required != 0U &&
          required <= sizeof(resources->desired_publisher_arena.bytes));
    CHECK(meiso_state_publisher_init(
        resources->desired_publisher_arena.bytes,
        required,
        desired,
        &state_config,
        &resources->desired_publisher
    ) == MEISO_STATE_INIT_OK);
    required = meiso_state_replica_memory_required(desired);
    CHECK(required != 0U &&
          required <= sizeof(resources->desired_replica_arena.bytes));
    CHECK(meiso_state_replica_init(
        resources->desired_replica_arena.bytes,
        required,
        desired,
        &resources->desired_replica
    ) == MEISO_STATE_INIT_OK);
    required = meiso_state_publisher_memory_required(actual, &state_config);
    CHECK(required != 0U &&
          required <= sizeof(resources->actual_publisher_arena.bytes));
    CHECK(meiso_state_publisher_init(
        resources->actual_publisher_arena.bytes,
        required,
        actual,
        &state_config,
        &resources->actual_publisher
    ) == MEISO_STATE_INIT_OK);
    required = meiso_state_replica_memory_required(actual);
    CHECK(required != 0U &&
          required <= sizeof(resources->actual_replica_arena.bytes));
    CHECK(meiso_state_replica_init(
        resources->actual_replica_arena.bytes,
        required,
        actual,
        &resources->actual_replica
    ) == MEISO_STATE_INIT_OK);
    required = meiso_stream_memory_required(data, &stream_config);
    CHECK(required != 0U &&
          required <= sizeof(resources->stream_arena.bytes));
    CHECK(meiso_stream_init(
        resources->stream_arena.bytes,
        required,
        data,
        &stream_config,
        &resources->stream
    ) == MEISO_STREAM_INIT_OK);
    CHECK(meiso_state_publisher_begin_delivery(
        resources->desired_publisher, (MeisoDeliveryGeneration){11U}
    ) == MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_publisher_begin_delivery(
        resources->actual_publisher, (MeisoDeliveryGeneration){22U}
    ) == MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_replica_begin_owner(
        resources->desired_replica, HOST_OWNER
    ) == MEISO_STATE_OWNER_STARTED);
    CHECK(meiso_state_replica_begin_owner(
        resources->actual_replica, EDGE_OWNER
    ) == MEISO_STATE_OWNER_STARTED);
    return true;
}

static bool publish_desired(
    Resources *resources,
    uint64_t version,
    bool enabled,
    MeisoWifiTelemetryDesiredParameters parameters
)
{
    uint8_t parameter_wire[
        MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE
    ];
    uint8_t state_wire[MEISO_WIFI_TELEMETRY_DESIRED_STATE_MAX_PAYLOAD_SIZE];
    MeisoRuntimeDesiredState state;
    MeisoByteView payload;
    size_t parameter_size = 0U;

    CHECK(meiso_wifi_telemetry_desired_parameters_encode(
        &parameters,
        (MeisoMutableBuffer){parameter_wire, sizeof(parameter_wire)},
        &parameter_size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    state.enabled = enabled;
    state.profile = (MeisoByteView){parameter_wire, parameter_size};
    CHECK(meiso_runtime_desired_state_encode(
        &state,
        (MeisoMutableBuffer){state_wire, sizeof(state_wire)},
        &payload
    ));
    CHECK(meiso_state_publisher_publish(
        resources->desired_publisher, HOST_OWNER, version, payload
    ) == MEISO_STATE_PUBLISH_NEW);
    return true;
}

static MeisoRuntimeDesiredReference desired_reference(uint64_t version)
{
    const MeisoRuntimeDesiredReference reference = {
        true, HOST_OWNER, version
    };

    return reference;
}

static bool publish_actual(
    Resources *resources,
    uint64_t version,
    MeisoRuntimeDesiredReference observed,
    MeisoRuntimeDesiredReference applied,
    MeisoRuntimeApplyResult apply_result,
    bool effective_enabled,
    MeisoRuntimeProductionStatus production_status,
    MeisoRuntimeProductionReason reason,
    uint64_t production_epoch,
    MeisoWifiTelemetryEffectiveParameters parameters
)
{
    uint8_t parameter_wire[
        MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE
    ];
    uint8_t state_wire[MEISO_WIFI_TELEMETRY_ACTUAL_STATE_MAX_PAYLOAD_SIZE];
    MeisoRuntimeActualState state;
    MeisoByteView payload;
    size_t parameter_size = 0U;

    CHECK(meiso_wifi_telemetry_effective_parameters_encode(
        &parameters,
        (MeisoMutableBuffer){parameter_wire, sizeof(parameter_wire)},
        &parameter_size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    memset(&state, 0, sizeof(state));
    state.observed_desired = observed;
    state.applied_desired = applied;
    state.apply_result = apply_result;
    state.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    state.effective_enabled = effective_enabled;
    state.production_status = production_status;
    state.reason = reason;
    state.production_epoch = production_epoch;
    state.profile = (MeisoByteView){parameter_wire, parameter_size};
    CHECK(meiso_runtime_actual_state_encode(
        &state,
        (MeisoMutableBuffer){state_wire, sizeof(state_wire)},
        &payload
    ));
    CHECK(meiso_state_publisher_publish(
        resources->actual_publisher, EDGE_OWNER, version, payload
    ) == MEISO_STATE_PUBLISH_NEW);
    return true;
}

static bool deliver_state(Pair *pair, Resources *resources, bool desired)
{
    Peer *sender = desired ? &pair->host : &pair->edge;
    Peer *receiver = desired ? &pair->edge : &pair->host;
    MeisoStatePublisher *publisher = desired ?
        resources->desired_publisher : resources->actual_publisher;
    MeisoStateReplica *replica = desired ?
        resources->desired_replica : resources->actual_replica;
    const MeisoRuntimeRouteKey key = route_key(
        desired ? MEISO_RUNTIME_FACET_DESIRED : MEISO_RUNTIME_FACET_ACTUAL
    );
    MeisoStateOffer offer;
    MeisoStateOffer received_offer;
    MeisoRuntimeRouteKey received_route;
    MeisoRuntimeEvent event;
    MeisoStateReceiveStatus receive_status;

    CHECK(meiso_state_publisher_next_offer(publisher, &offer));
    CHECK(meiso_runtime_send_state_offer(
        sender->runtime, key, &offer
    ) == MEISO_RUNTIME_OK);
    CHECK(exchange(sender, receiver, &event));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER &&
          route_equal(event.route, key));
    received_offer = event.state_offer;
    received_route = event.route;
    receive_status = meiso_state_replica_receive(
        replica,
        received_offer.state.owner,
        received_offer.state.version,
        received_offer.state.payload
    );
    CHECK(receive_status == MEISO_STATE_RECEIVE_NEW);
    CHECK(meiso_runtime_commit_state_receive(
        receiver->runtime,
        received_route,
        &received_offer,
        receive_status
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_send_state_accept(
        receiver->runtime,
        received_route,
        received_offer.generation,
        received_offer.state.owner,
        received_offer.state.version
    ) == MEISO_RUNTIME_OK);
    CHECK(exchange(receiver, sender, &event));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT &&
          route_equal(event.route, key));
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        publisher,
        event.state_accept_generation,
        event.state_accept_owner,
        event.state_accept_version
    ) == MEISO_STATE_ACCEPT_CONFIRMED);
    return true;
}

static bool desired_replica_is(
    Resources *resources,
    uint64_t version,
    bool enabled,
    MeisoWifiTelemetryDesiredParameters expected
)
{
    MeisoStateView view;
    MeisoRuntimeDesiredState state;
    MeisoWifiTelemetryDesiredParameters parameters;

    CHECK(meiso_state_replica_current(resources->desired_replica, &view));
    CHECK(view.version == version &&
          meiso_owner_scope_equal(view.owner, HOST_OWNER));
    CHECK(meiso_runtime_desired_state_decode(view.payload, &state));
    CHECK(state.enabled == enabled);
    CHECK(meiso_wifi_telemetry_desired_parameters_decode(
        state.profile, &parameters
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(parameters.selected_fields == expected.selected_fields &&
          parameters.sample_period_ms == expected.sample_period_ms);
    return true;
}

static bool actual_replica_is(
    Resources *resources,
    uint64_t version,
    MeisoRuntimeDesiredReference observed,
    MeisoRuntimeDesiredReference applied,
    MeisoRuntimeApplyResult apply_result,
    bool enabled,
    MeisoRuntimeProductionStatus production_status,
    MeisoRuntimeProductionReason reason,
    uint64_t production_epoch,
    MeisoWifiTelemetryEffectiveParameters expected
)
{
    MeisoStateView view;
    MeisoRuntimeActualState state;
    MeisoWifiTelemetryEffectiveParameters parameters;

    CHECK(meiso_state_replica_current(resources->actual_replica, &view));
    CHECK(view.version == version &&
          meiso_owner_scope_equal(view.owner, EDGE_OWNER));
    CHECK(meiso_runtime_actual_state_decode(view.payload, &state));
    CHECK(state.observed_desired.present == observed.present &&
          state.applied_desired.present == applied.present &&
          meiso_owner_scope_equal(
              state.observed_desired.owner, observed.owner
          ) && state.observed_desired.version == observed.version &&
          meiso_owner_scope_equal(
              state.applied_desired.owner, applied.owner
          ) && state.applied_desired.version == applied.version &&
          state.apply_result == apply_result &&
          state.availability == MEISO_RUNTIME_AVAILABILITY_PRESENT &&
          state.effective_enabled == enabled &&
          state.production_status == production_status &&
          state.reason == reason &&
          state.production_epoch == production_epoch);
    CHECK(meiso_wifi_telemetry_effective_parameters_decode(
        state.profile, &parameters
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(parameters.selected_fields == expected.selected_fields &&
          parameters.sample_period_ms == expected.sample_period_ms);
    return true;
}

static bool send_sample(
    Pair *pair,
    Resources *resources,
    MeisoWifiTelemetryFieldMask selected_fields,
    uint64_t sequence,
    uint64_t rx_bytes
)
{
    MeisoWifiTelemetrySample wifi_sample;
    MeisoWifiTelemetrySample decoded;
    MeisoStreamSample sample;
    MeisoStreamSample queued;
    MeisoRuntimeEvent event;
    MeisoStreamReceiveResult receive_result;
    uint8_t payload[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    size_t payload_size = 0U;

    memset(&wifi_sample, 0, sizeof(wifi_sample));
    wifi_sample.selected_fields = selected_fields;
    wifi_sample.present_fields = selected_fields;
    wifi_sample.link_state = true;
    wifi_sample.signal_mbm = -4800;
    wifi_sample.rx_bytes = rx_bytes;
    wifi_sample.tx_bytes = rx_bytes / 2U;
    CHECK(meiso_wifi_telemetry_sample_encode(
        &wifi_sample,
        (MeisoMutableBuffer){payload, sizeof(payload)},
        &payload_size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(payload_size == MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE);
    memset(&sample, 0, sizeof(sample));
    sample.key.producer = EDGE_OWNER;
    sample.key.epoch = PRODUCTION_EPOCH;
    sample.key.sequence = sequence;
    sample.source_time = (MeisoTimePoint){1U, sequence * 1000U};
    sample.payload = (MeisoByteView){payload, payload_size};
    CHECK(meiso_runtime_send_stream_sample(
        pair->edge.runtime,
        route_key(MEISO_RUNTIME_FACET_DATA),
        PRODUCTION_EPOCH,
        &sample
    ) == MEISO_RUNTIME_OK);
    CHECK(exchange(&pair->edge, &pair->host, &event));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
          route_equal(event.route, route_key(MEISO_RUNTIME_FACET_DATA)) &&
          event.production_epoch == PRODUCTION_EPOCH &&
          event.stream_sample.payload.size ==
              MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE);
    CHECK(meiso_wifi_telemetry_sample_decode(
        event.stream_sample.payload, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    CHECK(decoded.selected_fields == selected_fields &&
          decoded.present_fields == selected_fields &&
          decoded.rx_bytes == rx_bytes);
    if (meiso_stream_count(resources->stream) == 0U) {
        CHECK(meiso_stream_begin_producer(
            resources->stream,
            event.stream_sample.key.producer,
            event.stream_sample.key.epoch
        ) == MEISO_STREAM_PRODUCER_STARTED);
    }
    receive_result = meiso_stream_receive(
        resources->stream, &event.stream_sample, NULL
    );
    CHECK(receive_result.code == MEISO_STREAM_RECEIVE_ACCEPTED);
    CHECK(meiso_stream_peek(resources->stream, &queued));
    CHECK(queued.payload.size == MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE);
    CHECK(meiso_wifi_telemetry_sample_decode(
        queued.payload, &decoded
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    return true;
}

static bool test_wifi_runtime_over_tcp(void)
{
    const MeisoWifiTelemetryFieldMask selected_fields =
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
        MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM |
        MEISO_WIFI_TELEMETRY_FIELD_RX_BYTES |
        MEISO_WIFI_TELEMETRY_FIELD_TX_BYTES;
    const MeisoWifiTelemetryCapability capability = {
        selected_fields, 200U, 1000U, 200U
    };
    const MeisoWifiTelemetryDesiredParameters supported = {
        selected_fields, 600U
    };
    const MeisoWifiTelemetryDesiredParameters unsupported = {
        selected_fields, 700U
    };
    const MeisoWifiTelemetryEffectiveParameters effective = {
        selected_fields, 600U
    };
    const MeisoWifiTelemetryEffectiveParameters stopped = {0U, 0U};
    Pair pair;
    Resources resources;
    MeisoRuntimeProductionSnapshot production;
    MeisoRuntimeSnapshot fresh_runtime;
    MeisoSessionSnapshot first_session;
    MeisoSessionSnapshot second_session;
    MeisoSessionSnapshot edge_after_reject;
    MeisoStreamSample rejected_sample;
    uint8_t rejected_payload[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE] = {0};

    CHECK(initialize_pair(&pair, 0U));
    CHECK(session_ready(pair.host.session) && session_ready(pair.edge.session));
    CHECK(meiso_session_snapshot(pair.host.session, &first_session));
    CHECK(activate_catalog(&pair, &capability));
    CHECK(initialize_resources(&resources));

    CHECK(publish_desired(&resources, 1U, true, supported));
    CHECK(deliver_state(&pair, &resources, true));
    CHECK(desired_replica_is(&resources, 1U, true, supported));
    CHECK(meiso_wifi_telemetry_parameters_supported(
        &capability, &supported
    ));
    CHECK(publish_actual(
        &resources,
        1U,
        desired_reference(1U),
        desired_reference(1U),
        MEISO_RUNTIME_APPLY_APPLIED,
        true,
        MEISO_RUNTIME_PRODUCTION_RUNNING,
        MEISO_RUNTIME_PRODUCTION_REASON_NONE,
        PRODUCTION_EPOCH,
        effective
    ));
    CHECK(deliver_state(&pair, &resources, false));
    CHECK(actual_replica_is(
        &resources,
        1U,
        desired_reference(1U),
        desired_reference(1U),
        MEISO_RUNTIME_APPLY_APPLIED,
        true,
        MEISO_RUNTIME_PRODUCTION_RUNNING,
        MEISO_RUNTIME_PRODUCTION_REASON_NONE,
        PRODUCTION_EPOCH,
        effective
    ));
    CHECK(meiso_runtime_production_snapshot(
        pair.host.runtime, *meiso_wifi_telemetry_endpoint(), &production
    ) == MEISO_RUNTIME_OK);
    CHECK(production.data_active &&
          production.production_epoch == PRODUCTION_EPOCH &&
          production.observed_desired.version == 1U &&
          production.applied_desired.version == 1U &&
          production.effective_profile.size ==
              MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE);
    CHECK(send_sample(&pair, &resources, selected_fields, 1U, 1000U));
    CHECK(meiso_stream_count(resources.stream) == 1U);

    CHECK(publish_desired(&resources, 2U, false, supported));
    CHECK(deliver_state(&pair, &resources, true));
    CHECK(desired_replica_is(&resources, 2U, false, supported));
    CHECK(meiso_runtime_production_snapshot(
        pair.edge.runtime, *meiso_wifi_telemetry_endpoint(), &production
    ) == MEISO_RUNTIME_OK);
    CHECK(production.data_active &&
          production.production_epoch == PRODUCTION_EPOCH &&
          production.applied_desired.version == 1U);
    CHECK(send_sample(&pair, &resources, selected_fields, 2U, 2000U));
    CHECK(meiso_stream_count(resources.stream) == 2U);

    CHECK(publish_actual(
        &resources,
        2U,
        desired_reference(2U),
        desired_reference(2U),
        MEISO_RUNTIME_APPLY_APPLIED,
        false,
        MEISO_RUNTIME_PRODUCTION_STOPPED,
        MEISO_RUNTIME_PRODUCTION_REASON_DISABLED,
        0U,
        stopped
    ));
    CHECK(deliver_state(&pair, &resources, false));
    CHECK(actual_replica_is(
        &resources,
        2U,
        desired_reference(2U),
        desired_reference(2U),
        MEISO_RUNTIME_APPLY_APPLIED,
        false,
        MEISO_RUNTIME_PRODUCTION_STOPPED,
        MEISO_RUNTIME_PRODUCTION_REASON_DISABLED,
        0U,
        stopped
    ));
    CHECK(meiso_runtime_production_snapshot(
        pair.host.runtime, *meiso_wifi_telemetry_endpoint(), &production
    ) == MEISO_RUNTIME_OK);
    CHECK(!production.data_active &&
          production.production_status == MEISO_RUNTIME_PRODUCTION_STOPPED &&
          production.production_epoch_high_water == PRODUCTION_EPOCH);
    memset(&rejected_sample, 0, sizeof(rejected_sample));
    rejected_sample.key = (MeisoStreamKey){EDGE_OWNER, PRODUCTION_EPOCH, 3U};
    rejected_sample.source_time = (MeisoTimePoint){1U, 3000U};
    rejected_sample.payload = (MeisoByteView){
        rejected_payload, sizeof(rejected_payload)
    };
    CHECK(meiso_runtime_send_stream_sample(
        pair.edge.runtime,
        route_key(MEISO_RUNTIME_FACET_DATA),
        PRODUCTION_EPOCH,
        &rejected_sample
    ) == MEISO_RUNTIME_ROUTE_NOT_ACTIVE);
    CHECK(meiso_stream_count(resources.stream) == 2U);

    CHECK(!meiso_wifi_telemetry_parameters_supported(
        &capability, &unsupported
    ));
    CHECK(publish_desired(&resources, 3U, true, unsupported));
    CHECK(deliver_state(&pair, &resources, true));
    CHECK(desired_replica_is(&resources, 3U, true, unsupported));
    CHECK(publish_actual(
        &resources,
        3U,
        desired_reference(3U),
        desired_reference(2U),
        MEISO_RUNTIME_APPLY_REJECTED,
        false,
        MEISO_RUNTIME_PRODUCTION_STOPPED,
        MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER,
        0U,
        stopped
    ));
    CHECK(deliver_state(&pair, &resources, false));
    CHECK(actual_replica_is(
        &resources,
        3U,
        desired_reference(3U),
        desired_reference(2U),
        MEISO_RUNTIME_APPLY_REJECTED,
        false,
        MEISO_RUNTIME_PRODUCTION_STOPPED,
        MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER,
        0U,
        stopped
    ));
    CHECK(meiso_runtime_production_snapshot(
        pair.host.runtime, *meiso_wifi_telemetry_endpoint(), &production
    ) == MEISO_RUNTIME_OK);
    CHECK(!production.data_active &&
          production.apply_result == MEISO_RUNTIME_APPLY_REJECTED &&
          production.reason ==
              MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER &&
          production.observed_desired.version == 3U &&
          production.applied_desired.version == 2U);

    destroy_pair(&pair);
    CHECK(initialize_pair(&pair, 1000U));
    CHECK(meiso_session_snapshot(pair.host.session, &second_session));
    CHECK(!meiso_session_id_equal(
        first_session.session_id, second_session.session_id
    ));
    CHECK(meiso_runtime_snapshot(pair.host.runtime, &fresh_runtime));
    CHECK(!fresh_runtime.active_catalog_present &&
          fresh_runtime.active_catalog_generation == 0U);
    CHECK(meiso_runtime_production_snapshot(
        pair.host.runtime, *meiso_wifi_telemetry_endpoint(), &production
    ) == MEISO_RUNTIME_NO_CATALOG);
    CHECK(activate_catalog(&pair, &capability));
    CHECK(meiso_runtime_production_snapshot(
        pair.host.runtime, *meiso_wifi_telemetry_endpoint(), &production
    ) == MEISO_RUNTIME_OK);
    CHECK(!production.actual_present && !production.data_active);
    CHECK(meiso_runtime_production_snapshot(
        pair.edge.runtime, *meiso_wifi_telemetry_endpoint(), &production
    ) == MEISO_RUNTIME_OK);
    CHECK(!production.actual_present && !production.data_active);
    CHECK(meiso_runtime_send_stream_sample(
        pair.edge.runtime,
        route_key(MEISO_RUNTIME_FACET_DATA),
        PRODUCTION_EPOCH,
        &rejected_sample
    ) == MEISO_RUNTIME_ROUTE_NOT_ACTIVE);
    CHECK(meiso_session_snapshot(pair.edge.session, &edge_after_reject));
    CHECK(!edge_after_reject.outbound_pending);
    destroy_pair(&pair);
    return true;
}

int main(void)
{
    if (!test_wifi_runtime_over_tcp()) {
        return 1;
    }
    (void)printf("wifi runtime TCP integration test passed\n");
    return 0;
}
