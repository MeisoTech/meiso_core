#define _POSIX_C_SOURCE 200809L

#include "composition_debug_peer.h"
#include "scene_store.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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
                stderr, "CHECK failed at %s:%d: %s\n",                        \
                __FILE__, __LINE__, #condition                                  \
            );                                                                  \
            return false;                                                       \
        }                                                                       \
    } while (0)

#define CHECK_GOTO(condition, label)                                            \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(                                                      \
                stderr, "CHECK failed at %s:%d: %s\n",                        \
                __FILE__, __LINE__, #condition                                  \
            );                                                                  \
            goto label;                                                         \
        }                                                                       \
    } while (0)

enum {
    ROUTE_WIFI_DESIRED = 0,
    ROUTE_WIFI_ACTUAL = 1,
    ROUTE_WIFI_DATA = 2,
    ROUTE_PRESENTATION_DESIRED = 3,
    ROUTE_PRESENTATION_ACTUAL = 4,
    ROUTE_SCENE_DESIRED = 5,
    ROUTE_SCENE_ACTUAL = 6,
    ROUTE_MESH_BLOB = 7,
    ROUTE_RENDER_RESULT = 8,
    ROUTE_DEVICE_DESIRED = 9,
    ROUTE_DEVICE_ACTUAL = 10,
    ROUTE_DEVICE_DATA = 11,
    ROUTE_RENDER_DESIRED = 12,
    ROUTE_RENDER_ACTUAL = 13,
    ROUTE_CAMERA_DESIRED = 15,
    ROUTE_CAMERA_ACTUAL = 16
};

enum {
    HOST_CATALOG = 0,
    HOST_WIFI_DESIRED = 1,
    HOST_WIFI_ACTUAL_ACCEPT = 2,
    HOST_BLOB_BEGIN = 3,
    HOST_BLOB_CHUNK = 4,
    HOST_BLOB_COMMIT = 5,
    HOST_BLOB_ABORT = 6
};

enum {
    EDGE_CATALOG = 0,
    EDGE_WIFI_DESIRED_ACCEPT = 1,
    EDGE_WIFI_ACTUAL = 2,
    EDGE_WIFI_STREAM = 3,
    EDGE_BLOB_RESULT = 4
};

typedef struct {
    uint8_t capability[MEISO_COMPOSITION_SESSION_V3_DYNAMIC_CAPABILITY_BYTES];
    MeisoRuntimeEndpointDescriptor endpoints[
        MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT
    ];
    MeisoCompositionSessionV3RouteDescriptor routes[
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
    ];
} Catalog;

typedef struct {
    int host_fd;
    int edge_fd;
    void *host_arena;
    void *edge_arena;
    size_t host_bytes;
    size_t edge_bytes;
    MeisoCompositionDebugPeer *host;
    MeisoCompositionDebugPeer *edge;
    MeisoCompositionDebugPeerConfig host_config;
    MeisoCompositionDebugPeerConfig edge_config;
    Catalog catalog;
    uint64_t host_read_attempts;
    uint64_t host_write_attempts;
    uint64_t host_internal_steps;
    uint64_t edge_read_attempts;
    uint64_t edge_write_attempts;
    uint64_t edge_internal_steps;
} PeerFixture;

typedef struct {
    void *publisher_arena;
    void *replica_arena;
    MeisoStatePublisher *publisher;
    MeisoStateReplica *replica;
    MeisoDeliveryGeneration delivery;
} StateChannel;

typedef struct {
    MeisoCompositionDebugPeer *peer;
    MeisoRuntimeSchedulerWork reentry_work;
    uint8_t reentry_source;
    bool called;
    bool snapshot_failed;
    bool snapshot_unchanged;
    bool pump_rejected;
    bool pump_unchanged;
    bool queue_rejected;
} ReentryVerifier;

static const MeisoOwnerScope HOST_OWNER = {
    MEISO_OWNER_ROLE_HOST, UINT64_C(0x101)
};
static const MeisoOwnerScope EDGE_OWNER = {
    MEISO_OWNER_ROLE_EDGE, UINT64_C(0x202)
};

static bool bytes_are(const void *memory, uint8_t value, size_t size)
{
    const uint8_t *bytes = memory;
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != value) {
            return false;
        }
    }
    return true;
}

static bool owner_equal(MeisoOwnerScope lhs, MeisoOwnerScope rhs)
{
    return lhs.role == rhs.role && lhs.incarnation == rhs.incarnation;
}

static bool route_equal(MeisoRuntimeRouteKey lhs, MeisoRuntimeRouteKey rhs)
{
    return lhs.catalog_generation == rhs.catalog_generation &&
        meiso_endpoint_id_equal(lhs.endpoint_id, rhs.endpoint_id) &&
        lhs.endpoint_incarnation == rhs.endpoint_incarnation &&
        meiso_resource_id_equal(lhs.resource_id, rhs.resource_id) &&
        lhs.facet == rhs.facet;
}

static bool session_id_equal(MeisoSessionId lhs, MeisoSessionId rhs)
{
    return meiso_session_id_equal(lhs, rhs);
}

static void close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static bool set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool make_deadline(struct timespec *out)
{
    if (out == NULL || clock_gettime(CLOCK_MONOTONIC, out) != 0) {
        return false;
    }
    out->tv_sec += 5;
    return true;
}

static int remaining_ms(const struct timespec *deadline)
{
    struct timespec now;
    int64_t nanoseconds;

    if (deadline == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    nanoseconds = (int64_t)(deadline->tv_sec - now.tv_sec) *
        INT64_C(1000000000) + (int64_t)deadline->tv_nsec - now.tv_nsec;
    if (nanoseconds <= 0) {
        return 0;
    }
    if (nanoseconds >= INT64_C(5000000000)) {
        return 5000;
    }
    return (int)((nanoseconds + INT64_C(999999)) / INT64_C(1000000));
}

static bool wait_fd(int fd, short events, const struct timespec *deadline)
{
    struct pollfd item = {fd, events, 0};
    int timeout;

    while ((timeout = remaining_ms(deadline)) > 0) {
        int result = poll(&item, (nfds_t)1U, timeout);

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

static bool make_tcp_pair(int *out_host, int *out_edge)
{
    struct sockaddr_in address;
    struct sockaddr_in peer_address;
    struct timespec deadline;
    socklen_t address_size = (socklen_t)sizeof(address);
    socklen_t peer_size = (socklen_t)sizeof(peer_address);
    int listener = -1;
    int client = -1;
    int server = -1;
    int enabled = 1;
    int error_number = 0;
    socklen_t error_size = (socklen_t)sizeof(error_number);

    if (out_host == NULL || out_edge == NULL || !make_deadline(&deadline)) {
        return false;
    }
    *out_host = -1;
    *out_edge = -1;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0 || setsockopt(
            listener, SOL_SOCKET, SO_REUSEADDR, &enabled,
            (socklen_t)sizeof(enabled)
        ) != 0 || !set_nonblocking(listener)) {
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
    if (client < 0 || !set_nonblocking(client)) {
        goto fail;
    }
    if (connect(
            client, (const struct sockaddr *)(const void *)&address,
            address_size
        ) != 0 && (errno != EINPROGRESS ||
                   !wait_fd(client, POLLOUT, &deadline) ||
                   getsockopt(
                       client, SOL_SOCKET, SO_ERROR, &error_number, &error_size
                   ) != 0 || error_number != 0)) {
        goto fail;
    }
    while (server < 0 && remaining_ms(&deadline) > 0) {
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
    if (server < 0 || !set_nonblocking(server) || getpeername(
            client, (struct sockaddr *)(void *)&peer_address, &peer_size
        ) != 0 || peer_address.sin_family != AF_INET ||
        peer_address.sin_addr.s_addr != htonl(UINT32_C(0x7f000001))) {
        goto fail;
    }
    close_fd(&listener);
    *out_host = client;
    *out_edge = server;
    return true;

fail:
    close_fd(&server);
    close_fd(&client);
    close_fd(&listener);
    return false;
}

static MeisoCompositionSessionV3CatalogConfig catalog_config(void)
{
    MeisoCompositionSessionV3CatalogConfig value;

    memset(&value, 0, sizeof(value));
    value.wifi_incarnation = 11U;
    value.presentation_incarnation = 12U;
    value.scene_incarnation = 13U;
    value.device_telemetry_incarnation = 14U;
    value.render_media_incarnation = 15U;
    value.camera_media_incarnation = 16U;
    value.wifi_capability = (MeisoWifiTelemetryCapability){
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
            MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM,
        100U, 1000U, 100U
    };
    value.device_telemetry_capability = (MeisoDeviceTelemetryCapability){
        MEISO_DEVICE_TELEMETRY_FIELD_ALL,
        100U, 1000U, 100U, -40000, 125000, 1000U
    };
    value.render_media_capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888 |
            MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGB565,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        64U, 48U, 320U, 240U, 10000U,
        MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES,
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE, 0U
    };
    value.camera_media_capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        320U, 240U, 640U, 480U, 30000U,
        MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES,
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE, 0U
    };
    return value;
}

static void source(
    MeisoCompositionDebugPeerConfig *config,
    uint8_t index,
    MeisoRuntimeSchedulerWorkKind kind,
    uint8_t route
)
{
    config->sources[index].work_kind = kind;
    config->sources[index].route_index = route;
}

static void configure_sources(MeisoCompositionDebugPeerConfig *config)
{
    size_t index;

    if (config->local_role == MEISO_SESSION_ROLE_HOST) {
        source(config, 0U, MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_ACCEPT,
               MEISO_COMPOSITION_DEBUG_PEER_NO_ROUTE);
        source(config, 1U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, 0U);
        source(config, 2U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT, 1U);
        source(config, 3U, MEISO_RUNTIME_SCHEDULER_WORK_BLOB_BEGIN, 7U);
        source(config, 4U, MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK, 7U);
        source(config, 5U, MEISO_RUNTIME_SCHEDULER_WORK_BLOB_COMMIT, 7U);
        source(config, 6U, MEISO_RUNTIME_SCHEDULER_WORK_BLOB_ABORT, 7U);
        source(config, 7U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, 3U);
        source(config, 8U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT, 4U);
        source(config, 9U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, 5U);
        source(config, 10U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT, 6U);
        source(config, 11U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, 9U);
        source(config, 12U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT, 10U);
        source(config, 13U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, 12U);
        source(config, 14U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT, 13U);
        source(config, 15U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, 15U);
        source(config, 16U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT, 16U);
        for (index = 17U; index < 20U; ++index) {
            source(config, (uint8_t)index,
                   MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, 0U);
        }
    } else {
        source(config, 0U, MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER,
               MEISO_COMPOSITION_DEBUG_PEER_NO_ROUTE);
        source(config, 1U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT, 0U);
        source(config, 2U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, 1U);
        source(config, 3U, MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE, 2U);
        source(config, 4U, MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT, 7U);
        source(config, 5U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT, 3U);
        source(config, 6U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, 4U);
        source(config, 7U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT, 5U);
        source(config, 8U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, 6U);
        source(config, 9U, MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE, 8U);
        source(config, 10U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT, 9U);
        source(config, 11U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, 10U);
        source(config, 12U, MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE, 11U);
        source(config, 13U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT, 12U);
        source(config, 14U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, 13U);
        source(config, 15U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT, 15U);
        source(config, 16U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, 16U);
        for (index = 17U; index < 20U; ++index) {
            source(config, (uint8_t)index,
                   MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, 1U);
        }
    }
}

static MeisoCompositionDebugPeerConfig peer_config(
    int fd,
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
    MeisoCompositionDebugPeerConfig value;

    memset(&value, 0, sizeof(value));
    value.connected_fd = fd;
    value.local_role = role;
    value.local_peer_id = role == MEISO_SESSION_ROLE_HOST ? host : edge;
    value.local_incarnation = role == MEISO_SESSION_ROLE_HOST ? 31U : 41U;
    value.local_nonce = role == MEISO_SESSION_ROLE_HOST ?
        (MeisoSessionNonce){UINT64_C(0x61), nonce_salt + 1U} :
        (MeisoSessionNonce){UINT64_C(0x71), nonce_salt + 2U};
    value.expected_peer_id = role == MEISO_SESSION_ROLE_HOST ? edge : host;
    value.protocol = (MeisoProtocolVersion){1U, 0U};
    value.catalog_generation = 1U;
    value.catalog = catalog_config();
    value.max_io_bytes_per_step = 17U;
    value.adapter_reserve_bytes = 65536U;
    value.runtime_reserve_bytes = 400000U;
    value.scheduler_reserve_bytes = 65536U;
    configure_sources(&value);
    return value;
}

static void destroy_fixture(PeerFixture *fixture)
{
    if (fixture != NULL) {
        close_fd(&fixture->host_fd);
        close_fd(&fixture->edge_fd);
        free(fixture->host_arena);
        free(fixture->edge_arena);
        memset(fixture, 0, sizeof(*fixture));
        fixture->host_fd = -1;
        fixture->edge_fd = -1;
    }
}

static bool build_catalog(PeerFixture *fixture)
{
    CHECK(meiso_composition_session_v3_build_catalog(
        &fixture->host_config.catalog,
        (MeisoMutableBuffer){
            fixture->catalog.capability,
            sizeof(fixture->catalog.capability)
        },
        fixture->catalog.endpoints,
        MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT,
        fixture->catalog.routes,
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
    ) == MEISO_COMPOSITION_SESSION_V3_OK);
    return true;
}

static void record_pump(
    PeerFixture *fixture,
    bool host,
    MeisoCompositionDebugPeerPumpStatus status,
    const MeisoCompositionDebugPeerPumpResult *result
)
{
    if (result->read_attempted) {
        if (host) {
            ++fixture->host_read_attempts;
        } else {
            ++fixture->edge_read_attempts;
        }
    }
    if (result->write_attempted) {
        if (host) {
            ++fixture->host_write_attempts;
        } else {
            ++fixture->edge_write_attempts;
        }
    }
    if (result->scheduler_step_present ||
        status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_REBIND_PROGRESS ||
        (status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_PROGRESS &&
         !result->read_attempted && !result->write_attempted)) {
        if (host) {
            ++fixture->host_internal_steps;
        } else {
            ++fixture->edge_internal_steps;
        }
    }
}

static bool benign_pump_status(MeisoCompositionDebugPeerPumpStatus status)
{
    return status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_PROGRESS ||
        status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_IDLE ||
        status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_EVENT ||
        status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_WOULD_BLOCK ||
        status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_TX_BUSY ||
        status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_REBIND_PROGRESS;
}

static bool drive_ready(PeerFixture *fixture)
{
    struct timespec deadline;
    bool host_saw_staged = false;

    CHECK(make_deadline(&deadline));
    while (remaining_ms(&deadline) > 0) {
        MeisoCompositionDebugPeerPumpResult host_result;
        MeisoCompositionDebugPeerPumpResult edge_result;
        MeisoCompositionDebugPeerPumpStatus host_status;
        MeisoCompositionDebugPeerPumpStatus edge_status;
        MeisoCompositionDebugPeerSnapshot host_snapshot;
        MeisoCompositionDebugPeerSnapshot edge_snapshot;

        memset(&host_result, 0, sizeof(host_result));
        host_status = meiso_composition_debug_peer_pump(
            fixture->host, true, true, &host_result
        );
        if (!benign_pump_status(host_status)) {
            (void)fprintf(stderr, "host ready pump status=%u\n",
                          (unsigned)host_status);
        }
        CHECK(benign_pump_status(host_status));
        record_pump(fixture, true, host_status, &host_result);
        if (host_status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_EVENT &&
            host_result.event_present && host_result.event.type ==
                MEISO_RUNTIME_EVENT_CATALOG_OFFERED) {
            CHECK(meiso_composition_debug_peer_snapshot(
                fixture->host, &host_snapshot
            ));
            CHECK(host_snapshot.runtime.staged_catalog_present &&
                  host_snapshot.runtime.catalog_decision_pending &&
                  !host_snapshot.runtime.active_catalog_present &&
                  host_snapshot.runtime.staged_catalog_generation == 1U &&
                  host_snapshot.runtime.staged_endpoint_count == 6U &&
                  host_snapshot.runtime.staged_route_count == 18U &&
                  host_snapshot.boundary_serial == 0U);
            host_saw_staged = true;
        }

        memset(&edge_result, 0, sizeof(edge_result));
        edge_status = meiso_composition_debug_peer_pump(
            fixture->edge, true, true, &edge_result
        );
        if (!benign_pump_status(edge_status)) {
            (void)fprintf(stderr, "edge ready pump status=%u\n",
                          (unsigned)edge_status);
        }
        CHECK(benign_pump_status(edge_status));
        record_pump(fixture, false, edge_status, &edge_result);

        CHECK(meiso_composition_debug_peer_snapshot(
                  fixture->host, &host_snapshot
              ) && meiso_composition_debug_peer_snapshot(
                  fixture->edge, &edge_snapshot
              ));
        if (host_snapshot.phase == MEISO_COMPOSITION_DEBUG_PEER_PHASE_READY &&
            edge_snapshot.phase == MEISO_COMPOSITION_DEBUG_PEER_PHASE_READY) {
            CHECK(host_saw_staged);
            CHECK(host_snapshot.session.state == MEISO_SESSION_STATE_READY &&
                  edge_snapshot.session.state == MEISO_SESSION_STATE_READY &&
                  host_snapshot.session.session_id_known &&
                  edge_snapshot.session.session_id_known &&
                  session_id_equal(
                      host_snapshot.session.session_id,
                      edge_snapshot.session.session_id
                  ));
            CHECK(host_snapshot.runtime.active_catalog_present &&
                  edge_snapshot.runtime.active_catalog_present &&
                  host_snapshot.runtime.active_catalog_generation == 1U &&
                  edge_snapshot.runtime.active_catalog_generation == 1U &&
                  host_snapshot.runtime.active_endpoint_count == 6U &&
                  edge_snapshot.runtime.active_endpoint_count == 6U &&
                  host_snapshot.runtime.active_route_count == 18U &&
                  edge_snapshot.runtime.active_route_count == 18U &&
                  !host_snapshot.runtime.staged_catalog_present &&
                  !edge_snapshot.runtime.staged_catalog_present);
            CHECK(host_snapshot.scheduler.phase ==
                      MEISO_RUNTIME_SCHEDULER_PHASE_READY &&
                  edge_snapshot.scheduler.phase ==
                      MEISO_RUNTIME_SCHEDULER_PHASE_READY &&
                  host_snapshot.boundary_serial == 1U &&
                  edge_snapshot.boundary_serial == 1U &&
                  host_snapshot.last_boundary ==
                      MEISO_RUNTIME_SCHEDULER_BOUNDARY_CATALOG_CHANGED &&
                  edge_snapshot.last_boundary ==
                      MEISO_RUNTIME_SCHEDULER_BOUNDARY_CATALOG_CHANGED &&
                  host_snapshot.last_boundary_generation == 1U &&
                  edge_snapshot.last_boundary_generation == 1U);
            CHECK(host_snapshot.pending_source_count == 0U &&
                  edge_snapshot.pending_source_count == 0U &&
                  host_snapshot.runtime_present &&
                  edge_snapshot.runtime_present &&
                  host_snapshot.scheduler_present &&
                  edge_snapshot.scheduler_present &&
                  host_snapshot.adapter_bytes <=
                      host_snapshot.adapter_reserve_bytes &&
                  edge_snapshot.adapter_bytes <=
                      edge_snapshot.adapter_reserve_bytes &&
                  host_snapshot.runtime_bytes <=
                      host_snapshot.runtime_reserve_bytes &&
                  edge_snapshot.runtime_bytes <=
                      edge_snapshot.runtime_reserve_bytes &&
                  host_snapshot.scheduler_bytes <=
                      host_snapshot.scheduler_reserve_bytes &&
                  edge_snapshot.scheduler_bytes <=
                      edge_snapshot.scheduler_reserve_bytes);
            CHECK(fixture->host_read_attempts != 0U &&
                  fixture->host_write_attempts != 0U &&
                  fixture->host_internal_steps != 0U &&
                  fixture->edge_read_attempts != 0U &&
                  fixture->edge_write_attempts != 0U &&
                  fixture->edge_internal_steps != 0U);
            return true;
        }
    }
    return false;
}

static bool initialize_fixture(PeerFixture *fixture, uint64_t nonce_salt)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->host_fd = -1;
    fixture->edge_fd = -1;
    CHECK(make_tcp_pair(&fixture->host_fd, &fixture->edge_fd));
    fixture->host_config = peer_config(
        fixture->host_fd, MEISO_SESSION_ROLE_HOST, nonce_salt
    );
    fixture->edge_config = peer_config(
        fixture->edge_fd, MEISO_SESSION_ROLE_EDGE, nonce_salt
    );
    CHECK(build_catalog(fixture));
    fixture->host_bytes = meiso_composition_debug_peer_memory_required(
        &fixture->host_config
    );
    fixture->edge_bytes = meiso_composition_debug_peer_memory_required(
        &fixture->edge_config
    );
    CHECK(fixture->host_bytes != 0U && fixture->edge_bytes != 0U);
    fixture->host_arena = calloc(1U, fixture->host_bytes);
    fixture->edge_arena = calloc(1U, fixture->edge_bytes);
    if (fixture->host_arena == NULL || fixture->edge_arena == NULL) {
        destroy_fixture(fixture);
        return false;
    }
    if (meiso_composition_debug_peer_init(
            fixture->host_arena, fixture->host_bytes, &fixture->host_config,
            &fixture->host
        ) != MEISO_COMPOSITION_DEBUG_PEER_INIT_OK ||
        meiso_composition_debug_peer_init(
            fixture->edge_arena, fixture->edge_bytes, &fixture->edge_config,
            &fixture->edge
        ) != MEISO_COMPOSITION_DEBUG_PEER_INIT_OK || !drive_ready(fixture)) {
        destroy_fixture(fixture);
        return false;
    }
    return true;
}

static bool test_routes(PeerFixture *fixture)
{
    size_t index;

    CHECK(MEISO_COMPOSITION_SESSION_V3_CATALOG_WIRE_SIZE == 2032U &&
          MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT == 6U &&
          MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT == 18U &&
          MEISO_COMPOSITION_SESSION_V3_RUNTIME_WIRE_VERSION == 4U);
    CHECK(meiso_profile_ref_equal(
        *meiso_composition_scene_profile_v3(),
        (MeisoProfileRef){
            {UINT64_C(0x4b3233305f53434e),
             UINT64_C(0x5f4c41425f563030)},
            MEISO_COMPOSITION_SESSION_V3_PROFILE_VERSION
        }
    ));
    for (index = 0U; index < MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT;
         ++index) {
        MeisoRuntimeRouteKey host_route;
        MeisoRuntimeRouteKey edge_route;
        const MeisoRuntimeRouteDescriptor *expected =
            &fixture->catalog.routes[index].route;

        CHECK(meiso_composition_debug_peer_route_key(
                  fixture->host, (uint8_t)index, &host_route
              ) && meiso_composition_debug_peer_route_key(
                  fixture->edge, (uint8_t)index, &edge_route
              ));
        CHECK(route_equal(host_route, edge_route) &&
              host_route.catalog_generation == 1U &&
              meiso_endpoint_id_equal(
                  host_route.endpoint_id, expected->endpoint_id
              ) && host_route.endpoint_incarnation ==
                  expected->endpoint_incarnation &&
              meiso_resource_id_equal(
                  host_route.resource_id, expected->resource.id
              ) && host_route.facet == expected->facet);
    }
    return true;
}

static bool initialize_state_channel(
    StateChannel *channel,
    const MeisoResourceDescriptor *descriptor,
    MeisoOwnerScope owner,
    uint64_t delivery
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
        channel->publisher_arena, required, descriptor, &config,
        &channel->publisher
    ) == MEISO_STATE_INIT_OK);
    required = meiso_state_replica_memory_required(descriptor);
    CHECK(required != 0U);
    channel->replica_arena = calloc(1U, required);
    CHECK(channel->replica_arena != NULL);
    CHECK(meiso_state_replica_init(
        channel->replica_arena, required, descriptor, &channel->replica
    ) == MEISO_STATE_INIT_OK);
    CHECK(meiso_state_publisher_begin_delivery(
        channel->publisher, channel->delivery
    ) == MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_replica_begin_owner(channel->replica, owner) ==
          MEISO_STATE_OWNER_STARTED);
    return true;
}

static void destroy_state_channel(StateChannel *channel)
{
    free(channel->replica_arena);
    free(channel->publisher_arena);
    memset(channel, 0, sizeof(*channel));
}

static bool pump_until_event(
    PeerFixture *fixture,
    bool sender_is_host,
    MeisoRuntimeEventType expected_type,
    MeisoRuntimeEvent *out_event
)
{
    MeisoCompositionDebugPeer *sender =
        sender_is_host ? fixture->host : fixture->edge;
    MeisoCompositionDebugPeer *receiver =
        sender_is_host ? fixture->edge : fixture->host;
    struct timespec deadline;

    CHECK(make_deadline(&deadline));
    while (remaining_ms(&deadline) > 0) {
        MeisoCompositionDebugPeerPumpResult result;
        MeisoCompositionDebugPeerPumpStatus status;

        memset(&result, 0, sizeof(result));
        status = meiso_composition_debug_peer_pump(
            sender, true, true, &result
        );
        CHECK(benign_pump_status(status));
        record_pump(fixture, sender_is_host, status, &result);

        memset(&result, 0, sizeof(result));
        status = meiso_composition_debug_peer_pump(
            receiver, true, true, &result
        );
        CHECK(benign_pump_status(status));
        record_pump(fixture, !sender_is_host, status, &result);
        if (status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_EVENT &&
            result.event_present) {
            CHECK(result.event.type == expected_type);
            *out_event = result.event;
            return true;
        }
    }
    return false;
}

static bool dispatch_work(
    PeerFixture *fixture,
    bool sender_is_host,
    uint8_t source_index,
    const MeisoRuntimeSchedulerWork *work,
    MeisoRuntimeEventType event_type,
    MeisoRuntimeEvent *out_event,
    MeisoRuntimeSchedulerCompletion *out_completion
)
{
    MeisoCompositionDebugPeer *sender =
        sender_is_host ? fixture->host : fixture->edge;
    MeisoRuntimeSchedulerCompletion completion;

    CHECK(meiso_composition_debug_peer_queue_work(
        sender, source_index, work
    ) == MEISO_COMPOSITION_DEBUG_PEER_OK);
    CHECK(pump_until_event(
        fixture, sender_is_host, event_type, out_event
    ));
    memset(&completion, 0, sizeof(completion));
    CHECK(meiso_composition_debug_peer_take_completion(
        sender, source_index, &completion
    ) == MEISO_COMPOSITION_DEBUG_PEER_OK);
    CHECK(completion.ticket != 0U && completion.kind == work->kind &&
          completion.code == MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT &&
          completion.runtime_status == MEISO_RUNTIME_OK);
    if (out_completion != NULL) {
        *out_completion = completion;
    }
    return true;
}

static bool encode_desired(uint8_t output[8], MeisoByteView *out_payload)
{
    const MeisoRuntimeDesiredState desired = {true, {NULL, 0U}};

    CHECK(meiso_runtime_desired_state_encode(
        &desired, (MeisoMutableBuffer){output, 8U}, out_payload
    ));
    return true;
}

static bool encode_actual(
    uint64_t epoch,
    uint8_t output[56],
    MeisoByteView *out_payload
)
{
    MeisoRuntimeActualState actual;

    memset(&actual, 0, sizeof(actual));
    actual.observed_desired = (MeisoRuntimeDesiredReference){
        true, HOST_OWNER, 1U
    };
    actual.applied_desired = actual.observed_desired;
    actual.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    actual.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    actual.effective_enabled = true;
    actual.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING;
    actual.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    actual.production_epoch = epoch;
    CHECK(meiso_runtime_actual_state_encode(
        &actual, (MeisoMutableBuffer){output, 56U}, out_payload
    ));
    return true;
}

static bool deliver_state(
    PeerFixture *fixture,
    StateChannel *channel,
    bool host_to_edge,
    uint8_t offer_source,
    uint8_t accept_source,
    uint8_t route_index,
    MeisoStateView *out_current
)
{
    MeisoCompositionDebugPeer *receiver =
        host_to_edge ? fixture->edge : fixture->host;
    MeisoCompositionDebugPeer *accept_sender = receiver;
    MeisoRuntimeRouteKey route;
    MeisoRuntimeSchedulerWork offer_work;
    MeisoRuntimeSchedulerWork accept_work;
    MeisoRuntimeEvent offer_event;
    MeisoRuntimeEvent accept_event;
    MeisoStateOffer offer;
    MeisoStateReceiveStatus receive_status;

    CHECK(meiso_composition_debug_peer_route_key(
        host_to_edge ? fixture->host : fixture->edge,
        route_index, &route
    ));
    CHECK(meiso_state_publisher_next_offer(channel->publisher, &offer));
    memset(&offer_work, 0, sizeof(offer_work));
    offer_work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
    offer_work.value.state_offer.route = route;
    offer_work.value.state_offer.offer = offer;
    CHECK(dispatch_work(
        fixture, host_to_edge, offer_source, &offer_work,
        MEISO_RUNTIME_EVENT_STATE_OFFER, &offer_event, NULL
    ));
    CHECK(route_equal(offer_event.route, route) &&
          owner_equal(offer_event.state_offer.state.owner, offer.state.owner) &&
          offer_event.state_offer.state.version == offer.state.version &&
          offer_event.state_offer.state.payload.size == offer.state.payload.size &&
          memcmp(
              offer_event.state_offer.state.payload.data,
              offer.state.payload.data, offer.state.payload.size
          ) == 0);
    receive_status = meiso_state_replica_receive(
        channel->replica,
        offer_event.state_offer.state.owner,
        offer_event.state_offer.state.version,
        offer_event.state_offer.state.payload
    );
    CHECK(receive_status == MEISO_STATE_RECEIVE_NEW);
    CHECK(meiso_composition_debug_peer_queue_work(
        receiver, accept_source, &offer_work
    ) == MEISO_COMPOSITION_DEBUG_PEER_RX_OBLIGATION);
    CHECK(meiso_composition_debug_peer_commit_state_receive(
        receiver, offer_event.route, &offer_event.state_offer, receive_status
    ) == MEISO_COMPOSITION_DEBUG_PEER_OK);

    memset(&accept_work, 0, sizeof(accept_work));
    accept_work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
    accept_work.value.state_accept.route = route;
    accept_work.value.state_accept.generation =
        offer_event.state_offer.generation;
    accept_work.value.state_accept.owner =
        offer_event.state_offer.state.owner;
    accept_work.value.state_accept.version =
        offer_event.state_offer.state.version;
    CHECK(dispatch_work(
        fixture, !host_to_edge, accept_source, &accept_work,
        MEISO_RUNTIME_EVENT_STATE_ACCEPT, &accept_event, NULL
    ));
    CHECK(route_equal(accept_event.route, route) &&
          accept_event.state_accept_generation.value ==
              channel->delivery.value &&
          owner_equal(
              accept_event.state_accept_owner, offer.state.owner
          ) && accept_event.state_accept_version == offer.state.version);
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        channel->publisher,
        accept_event.state_accept_generation,
        accept_event.state_accept_owner,
        accept_event.state_accept_version
    ) == MEISO_STATE_ACCEPT_CONFIRMED);
    (void)accept_sender;
    if (out_current != NULL) {
        CHECK(meiso_state_replica_current(channel->replica, out_current));
    }
    return true;
}

static bool test_state_and_stream(PeerFixture *fixture)
{
    const MeisoResourceDescriptor *wifi_desired =
        &fixture->catalog.routes[ROUTE_WIFI_DESIRED].route.resource;
    const MeisoResourceDescriptor *wifi_actual =
        &fixture->catalog.routes[ROUTE_WIFI_ACTUAL].route.resource;
    const MeisoResourceDescriptor *wifi_data =
        &fixture->catalog.routes[ROUTE_WIFI_DATA].route.resource;
    StateChannel desired = {0};
    StateChannel actual = {0};
    uint8_t desired_wire[8];
    uint8_t actual_wire[56];
    uint8_t sample_wire[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    MeisoByteView desired_payload;
    MeisoByteView actual_payload;
    MeisoStateView current;
    MeisoWifiTelemetrySample sample_value;
    MeisoStreamConfig stream_config;
    MeisoStream *source_stream;
    MeisoStream *destination_stream;
    void *source_arena = NULL;
    void *destination_arena = NULL;
    size_t stream_bytes;
    size_t sample_size = 0U;
    MeisoStreamSample sample;
    MeisoStreamSample received;
    MeisoStreamReceiveResult receive_result;
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeEvent event;
    MeisoRuntimeRouteKey route;
    bool success = false;

    CHECK(initialize_state_channel(&desired, wifi_desired, HOST_OWNER, 11U));
    CHECK(initialize_state_channel(&actual, wifi_actual, EDGE_OWNER, 12U));
    CHECK(encode_desired(desired_wire, &desired_payload));
    CHECK(meiso_state_publisher_publish(
        desired.publisher, HOST_OWNER, 1U, desired_payload
    ) == MEISO_STATE_PUBLISH_NEW);
    CHECK(deliver_state(
        fixture, &desired, true, HOST_WIFI_DESIRED,
        EDGE_WIFI_DESIRED_ACCEPT, ROUTE_WIFI_DESIRED, &current
    ));
    CHECK(current.version == 1U && owner_equal(current.owner, HOST_OWNER));

    CHECK(encode_actual(77U, actual_wire, &actual_payload));
    CHECK(meiso_state_publisher_publish(
        actual.publisher, EDGE_OWNER, 1U, actual_payload
    ) == MEISO_STATE_PUBLISH_NEW);
    CHECK(deliver_state(
        fixture, &actual, false, EDGE_WIFI_ACTUAL,
        HOST_WIFI_ACTUAL_ACCEPT, ROUTE_WIFI_ACTUAL, &current
    ));
    CHECK(current.version == 1U && owner_equal(current.owner, EDGE_OWNER));

    memset(&stream_config, 0, sizeof(stream_config));
    stream_config.ordering = MEISO_STREAM_ORDER_STRICT_MONOTONIC;
    stream_config.overflow = MEISO_STREAM_OVERFLOW_DROP_OLDEST;
    stream_config.deadline_policy = MEISO_STREAM_DEADLINE_UNSUPPORTED;
    stream_config.queue_slots = 4U;
    stream_bytes = meiso_stream_memory_required(wifi_data, &stream_config);
    CHECK(stream_bytes != 0U);
    source_arena = calloc(1U, stream_bytes);
    destination_arena = calloc(1U, stream_bytes);
    if (source_arena == NULL || destination_arena == NULL) {
        free(destination_arena);
        free(source_arena);
        destroy_state_channel(&actual);
        destroy_state_channel(&desired);
        return false;
    }
    CHECK_GOTO(meiso_stream_init(
              source_arena, stream_bytes, wifi_data, &stream_config,
              &source_stream
          ) == MEISO_STREAM_INIT_OK && meiso_stream_init(
              destination_arena, stream_bytes, wifi_data, &stream_config,
              &destination_stream
          ) == MEISO_STREAM_INIT_OK, cleanup);
    CHECK_GOTO(meiso_stream_begin_producer(source_stream, EDGE_OWNER, 77U) ==
              MEISO_STREAM_PRODUCER_STARTED &&
          meiso_stream_begin_producer(destination_stream, EDGE_OWNER, 77U) ==
              MEISO_STREAM_PRODUCER_STARTED, cleanup);
    memset(&sample_value, 0, sizeof(sample_value));
    sample_value.selected_fields = MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
        MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM;
    sample_value.present_fields = sample_value.selected_fields;
    sample_value.link_state = true;
    sample_value.signal_mbm = -5100;
    CHECK_GOTO(meiso_wifi_telemetry_sample_encode(
        &sample_value,
        (MeisoMutableBuffer){sample_wire, sizeof(sample_wire)}, &sample_size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK, cleanup);
    memset(&sample, 0, sizeof(sample));
    sample.key = (MeisoStreamKey){EDGE_OWNER, 77U, 1U};
    sample.source_time = (MeisoTimePoint){77U, 1000U};
    sample.payload = (MeisoByteView){sample_wire, sample_size};
    receive_result = meiso_stream_receive(source_stream, &sample, NULL);
    CHECK_GOTO(receive_result.code == MEISO_STREAM_RECEIVE_ACCEPTED &&
          receive_result.flags == MEISO_STREAM_RECEIVE_FLAG_NONE &&
          receive_result.missing_before == 0U &&
          meiso_stream_peek(source_stream, &sample), cleanup);
    CHECK_GOTO(meiso_composition_debug_peer_route_key(
        fixture->edge, ROUTE_WIFI_DATA, &route
    ), cleanup);
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE;
    work.value.stream_sample.route = route;
    work.value.stream_sample.production_epoch = 77U;
    work.value.stream_sample.sample = sample;
    CHECK_GOTO(dispatch_work(
        fixture, false, EDGE_WIFI_STREAM, &work,
        MEISO_RUNTIME_EVENT_STREAM_SAMPLE, &event, NULL
    ), cleanup);
    CHECK_GOTO(route_equal(event.route, route) &&
          event.production_epoch == 77U &&
          event.stream_sample.key.sequence == 1U &&
          event.stream_sample.payload.size == sample_size &&
          memcmp(event.stream_sample.payload.data, sample_wire, sample_size) ==
              0, cleanup);
    receive_result = meiso_stream_receive(
        destination_stream, &event.stream_sample, NULL
    );
    CHECK_GOTO(receive_result.code == MEISO_STREAM_RECEIVE_ACCEPTED &&
          receive_result.flags == MEISO_STREAM_RECEIVE_FLAG_NONE &&
          meiso_stream_peek(destination_stream, &received) &&
          received.key.epoch == 77U && received.key.sequence == 1U &&
          received.payload.size == sample_size &&
          memcmp(received.payload.data, sample_wire, sample_size) == 0,
          cleanup);
    CHECK_GOTO(meiso_stream_pop(source_stream) &&
          meiso_stream_pop(destination_stream) &&
          meiso_stream_count(source_stream) == 0U &&
          meiso_stream_count(destination_stream) == 0U, cleanup);
    success = true;

cleanup:
    free(destination_arena);
    free(source_arena);
    destroy_state_channel(&actual);
    destroy_state_channel(&desired);
    return success;
}

static bool reentry_verifier(
    const MeisoContentId *expected,
    MeisoByteView bytes,
    void *context
)
{
    ReentryVerifier *value = context;
    MeisoCompositionDebugPeerSnapshot snapshot;
    MeisoCompositionDebugPeerPumpResult pump;

    value->called = true;
    memset(&snapshot, 0x5a, sizeof(snapshot));
    value->snapshot_failed = !meiso_composition_debug_peer_snapshot(
        value->peer, &snapshot
    );
    value->snapshot_unchanged = bytes_are(&snapshot, 0x5a, sizeof(snapshot));
    memset(&pump, 0x6b, sizeof(pump));
    value->pump_rejected = meiso_composition_debug_peer_pump(
        value->peer, true, true, &pump
    ) == MEISO_COMPOSITION_DEBUG_PEER_PUMP_INVALID_ARGUMENT;
    value->pump_unchanged = bytes_are(&pump, 0x6b, sizeof(pump));
    value->queue_rejected = meiso_composition_debug_peer_queue_work(
        value->peer, value->reentry_source, &value->reentry_work
    ) == MEISO_COMPOSITION_DEBUG_PEER_INVALID_STATE;
    return meiso_composition_scene_mesh_blob_verify(expected, bytes, NULL);
}

static bool test_blob(PeerFixture *fixture)
{
    uint8_t cube_storage[512];
    size_t cube_size = 0U;
    MeisoByteView cube;
    MeisoContentId content_id;
    MeisoRuntimeRouteKey route;
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeSchedulerCompletion completion;
    MeisoRuntimeEvent event;
    MeisoRuntimeBlobTransferRef transfer;
    MeisoBlobConfig blob_config = {2U};
    MeisoBlobStore *store;
    void *store_arena;
    size_t store_bytes;
    MeisoRuntimeBlobPublishReport report;
    MeisoBlobView view;
    ReentryVerifier verifier;

    CHECK(meiso_composition_scene_mesh_encode(
        meiso_composition_scene_canonical_cube(),
        (MeisoMutableBuffer){cube_storage, sizeof(cube_storage)}, &cube_size
    ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    cube = (MeisoByteView){cube_storage, cube_size};
    CHECK(meiso_composition_scene_mesh_content_id(cube, &content_id));
    CHECK(meiso_composition_debug_peer_route_key(
        fixture->host, ROUTE_MESH_BLOB, &route
    ));
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_BLOB_BEGIN;
    work.value.blob_begin.route = route;
    work.value.blob_begin.content_id = content_id;
    work.value.blob_begin.total_size = cube_size;
    CHECK(dispatch_work(
        fixture, true, HOST_BLOB_BEGIN, &work,
        MEISO_RUNTIME_EVENT_BLOB_REQUEST, &event, &completion
    ));
    CHECK(event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN &&
          completion.generated_blob_transfer_present &&
          event.blob_transfer.transfer_id ==
              completion.generated_blob_transfer.transfer_id &&
          route_equal(event.blob_transfer.route, route));
    transfer = event.blob_transfer;

    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT;
    work.value.blob_result.transfer = transfer;
    work.value.blob_result.operation = MEISO_RUNTIME_BLOB_BEGIN;
    CHECK(dispatch_work(
        fixture, false, EDGE_BLOB_RESULT, &work,
        MEISO_RUNTIME_EVENT_BLOB_RESULT, &event, NULL
    ));
    CHECK(event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN &&
          event.blob_result == MEISO_RUNTIME_BLOB_RESULT_READY &&
          event.blob_next_offset == 0U);

    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK;
    work.value.blob_chunk.transfer = transfer;
    work.value.blob_chunk.offset = 0U;
    work.value.blob_chunk.payload = cube;
    CHECK(dispatch_work(
        fixture, true, HOST_BLOB_CHUNK, &work,
        MEISO_RUNTIME_EVENT_BLOB_REQUEST, &event, NULL
    ));
    CHECK(event.blob_operation == MEISO_RUNTIME_BLOB_CHUNK &&
          event.blob_offset == 0U && event.blob_payload.size == cube_size &&
          memcmp(event.blob_payload.data, cube.data, cube_size) == 0);

    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_BLOB_COMMIT;
    work.value.blob_commit.transfer = transfer;
    CHECK(dispatch_work(
        fixture, true, HOST_BLOB_COMMIT, &work,
        MEISO_RUNTIME_EVENT_BLOB_REQUEST, &event, NULL
    ));
    CHECK(event.blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          event.blob_payload.size == cube_size &&
          memcmp(event.blob_payload.data, cube.data, cube_size) == 0);

    store_bytes = meiso_blob_store_memory_required(
        &fixture->catalog.routes[ROUTE_MESH_BLOB].route.resource, &blob_config
    );
    CHECK(store_bytes != 0U);
    store_arena = calloc(1U, store_bytes);
    CHECK(store_arena != NULL);
    CHECK(meiso_blob_store_init(
        store_arena, store_bytes,
        &fixture->catalog.routes[ROUTE_MESH_BLOB].route.resource,
        &blob_config, &store
    ) == MEISO_BLOB_INIT_OK);
    memset(&verifier, 0, sizeof(verifier));
    verifier.peer = fixture->edge;
    verifier.reentry_source = EDGE_WIFI_ACTUAL;
    verifier.reentry_work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
    CHECK(meiso_composition_debug_peer_route_key(
        fixture->edge, ROUTE_WIFI_ACTUAL,
        &verifier.reentry_work.value.state_offer.route
    ));
    CHECK(meiso_composition_debug_peer_publish_blob_receive(
        fixture->edge, &event.blob_transfer, store,
        reentry_verifier, &verifier, &report
    ) == MEISO_COMPOSITION_DEBUG_PEER_OK);
    CHECK(verifier.called && verifier.snapshot_failed &&
          verifier.snapshot_unchanged && verifier.pump_rejected &&
          verifier.pump_unchanged && verifier.queue_rejected);
    CHECK(report.stage_attempted && report.commit_attempted &&
          report.stage_status == MEISO_BLOB_STAGE_STARTED &&
          report.commit_status == MEISO_BLOB_COMMIT_PUBLISHED &&
          report.result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
          meiso_blob_store_count(store) == 1U &&
          meiso_blob_store_lookup(store, &content_id, &view) &&
          view.content.size == cube_size &&
          memcmp(view.content.data, cube.data, cube_size) == 0);

    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT;
    work.value.blob_result.transfer = transfer;
    work.value.blob_result.operation = MEISO_RUNTIME_BLOB_COMMIT;
    CHECK(dispatch_work(
        fixture, false, EDGE_BLOB_RESULT, &work,
        MEISO_RUNTIME_EVENT_BLOB_RESULT, &event, NULL
    ));
    CHECK(event.blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          event.blob_result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
          event.blob_next_offset == cube_size);
    free(store_arena);
    return true;
}

static bool test_invalid_config_and_alias(void)
{
    int host_fd = -1;
    int edge_fd = -1;
    MeisoCompositionDebugPeerConfig config;
    MeisoCompositionDebugPeer *peer = (MeisoCompositionDebugPeer *)(uintptr_t)1U;
    size_t required;
    uint8_t *arena;

    CHECK(make_tcp_pair(&host_fd, &edge_fd));
    config = peer_config(host_fd, MEISO_SESSION_ROLE_HOST, 500U);
    required = meiso_composition_debug_peer_memory_required(&config);
    CHECK(required != 0U);
    arena = malloc(required + meiso_composition_debug_peer_alignment_required());
    CHECK(arena != NULL);
    memset(arena, 0xa5, required +
           meiso_composition_debug_peer_alignment_required());
    CHECK(meiso_composition_debug_peer_init(
        arena, required - 1U, &config, &peer
    ) == MEISO_COMPOSITION_DEBUG_PEER_INIT_ARENA_TOO_SMALL);
    CHECK(peer == NULL && bytes_are(arena, 0xa5, required));
    peer = (MeisoCompositionDebugPeer *)(uintptr_t)1U;
    CHECK(meiso_composition_debug_peer_init(
        arena + 1U, required, &config, &peer
    ) == MEISO_COMPOSITION_DEBUG_PEER_INIT_MISALIGNED_ARENA);
    CHECK(peer == NULL);
    memcpy(arena, &config, sizeof(config));
    peer = (MeisoCompositionDebugPeer *)(uintptr_t)1U;
    CHECK(meiso_composition_debug_peer_init(
        arena, required, (const MeisoCompositionDebugPeerConfig *)arena,
        &peer
    ) == MEISO_COMPOSITION_DEBUG_PEER_INIT_INVALID_ARGUMENT);
    CHECK(peer == (MeisoCompositionDebugPeer *)(uintptr_t)1U);
    config.sources[HOST_WIFI_DESIRED].route_index = ROUTE_WIFI_ACTUAL;
    CHECK(meiso_composition_debug_peer_memory_required(&config) == 0U);
    free(arena);
    close_fd(&edge_fd);
    close_fd(&host_fd);
    return true;
}

static bool test_ready_failures_are_atomic(PeerFixture *fixture)
{
    uint8_t *before = malloc(fixture->host_bytes);
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeSchedulerCompletion completion;
    MeisoRuntimeRouteKey route;
    MeisoCompositionDebugPeerPumpResult pump;
    bool success = false;

    if (before == NULL) {
        return false;
    }
    CHECK_GOTO(meiso_composition_debug_peer_route_key(
        fixture->host, ROUTE_WIFI_DESIRED, &route
    ), cleanup);
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
    work.ticket = 1U;
    work.value.state_offer.route = route;
    memcpy(before, fixture->host_arena, fixture->host_bytes);
    CHECK_GOTO(meiso_composition_debug_peer_queue_work(
              fixture->host, HOST_WIFI_DESIRED, &work
          ) == MEISO_COMPOSITION_DEBUG_PEER_SOURCE_KIND_MISMATCH &&
          memcmp(before, fixture->host_arena, fixture->host_bytes) == 0,
          cleanup);
    work.ticket = 0U;
    work.value.state_offer.route.catalog_generation = 2U;
    CHECK_GOTO(meiso_composition_debug_peer_queue_work(
              fixture->host, HOST_WIFI_DESIRED, &work
          ) == MEISO_COMPOSITION_DEBUG_PEER_ROUTE_MISMATCH &&
          memcmp(before, fixture->host_arena, fixture->host_bytes) == 0,
          cleanup);
    work.value.state_offer.route = route;
    work.value.state_offer.offer.state.payload =
        (MeisoByteView){fixture->host_arena, 1U};
    CHECK_GOTO(meiso_composition_debug_peer_queue_work(
              fixture->host, HOST_WIFI_DESIRED, &work
          ) == MEISO_COMPOSITION_DEBUG_PEER_INVALID_ARGUMENT &&
          memcmp(before, fixture->host_arena, fixture->host_bytes) == 0,
          cleanup);
    memset(&completion, 0x5a, sizeof(completion));
    CHECK_GOTO(meiso_composition_debug_peer_take_completion(
              fixture->host, HOST_WIFI_DESIRED, &completion
          ) == MEISO_COMPOSITION_DEBUG_PEER_INVALID_STATE &&
          bytes_are(&completion, 0x5a, sizeof(completion)) &&
          memcmp(before, fixture->host_arena, fixture->host_bytes) == 0,
          cleanup);
    memset(&pump, 0x6b, sizeof(pump));
    CHECK_GOTO(meiso_composition_debug_peer_pump(
              fixture->host, true, true,
              (MeisoCompositionDebugPeerPumpResult *)fixture->host_arena
          ) == MEISO_COMPOSITION_DEBUG_PEER_PUMP_INVALID_ARGUMENT &&
          memcmp(before, fixture->host_arena, fixture->host_bytes) == 0,
          cleanup);
    success = true;
cleanup:
    free(before);
    return success;
}

static bool test_terminal_and_new_session(PeerFixture *fixture)
{
    PeerFixture fresh = {.host_fd = -1, .edge_fd = -1};
    MeisoCompositionDebugPeerSnapshot before;
    MeisoCompositionDebugPeerSnapshot terminal;
    MeisoCompositionDebugPeerSnapshot fresh_host;
    MeisoCompositionDebugPeerSnapshot fresh_edge;
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeRouteKey route;
    MeisoCompositionDebugPeerPumpResult pump;
    uint8_t *host_raw = NULL;
    uint8_t *edge_raw = NULL;
    bool success = false;

    CHECK(meiso_composition_debug_peer_snapshot(fixture->host, &before));
    meiso_composition_debug_peer_abort(fixture->edge, ECONNRESET);
    CHECK(meiso_composition_debug_peer_snapshot(fixture->edge, &terminal));
    CHECK(terminal.phase == MEISO_COMPOSITION_DEBUG_PEER_PHASE_TERMINAL &&
          terminal.terminal && terminal.pending_source_count == 0U &&
          terminal.session.state == MEISO_SESSION_STATE_FAILED &&
          terminal.session.failure == MEISO_SESSION_FAILURE_TRANSPORT_LOST &&
          terminal.boundary_serial == 2U &&
          terminal.last_boundary ==
              MEISO_RUNTIME_SCHEDULER_BOUNDARY_SESSION_TERMINAL &&
          terminal.last_boundary_generation == 0U &&
          !terminal.runtime.core_ready &&
          terminal.scheduler.phase ==
              MEISO_RUNTIME_SCHEDULER_PHASE_SESSION_TERMINAL);

    host_raw = malloc(fixture->host_bytes);
    edge_raw = malloc(fixture->edge_bytes);
    if (host_raw == NULL || edge_raw == NULL) {
        free(edge_raw);
        free(host_raw);
        return false;
    }
    memcpy(edge_raw, fixture->edge_arena, fixture->edge_bytes);
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
    CHECK_GOTO(meiso_composition_debug_peer_queue_work(
              fixture->edge, EDGE_WIFI_ACTUAL, &work
          ) == MEISO_COMPOSITION_DEBUG_PEER_TERMINAL, cleanup);
    memset(&pump, 0x5a, sizeof(pump));
    CHECK_GOTO(meiso_composition_debug_peer_pump(
              fixture->edge, true, true, &pump
          ) == MEISO_COMPOSITION_DEBUG_PEER_PUMP_TERMINAL &&
          bytes_are(&pump, 0x5a, sizeof(pump)) &&
          !meiso_composition_debug_peer_wants_read(fixture->edge) &&
          !meiso_composition_debug_peer_wants_write(fixture->edge), cleanup);
    memset(&route, 0x6b, sizeof(route));
    CHECK_GOTO(!meiso_composition_debug_peer_route_key(
              fixture->edge, ROUTE_WIFI_ACTUAL, &route
          ) && bytes_are(&route, 0x6b, sizeof(route)), cleanup);
    meiso_composition_debug_peer_abort(fixture->edge, EIO);
    CHECK_GOTO(memcmp(
        edge_raw, fixture->edge_arena, fixture->edge_bytes
    ) == 0, cleanup);

    close_fd(&fixture->edge_fd);
    {
        struct timespec deadline;
        bool host_terminal = false;
        CHECK_GOTO(make_deadline(&deadline), cleanup);
        while (remaining_ms(&deadline) > 0 && !host_terminal) {
            memset(&pump, 0, sizeof(pump));
            host_terminal = meiso_composition_debug_peer_pump(
                fixture->host, true, true, &pump
            ) == MEISO_COMPOSITION_DEBUG_PEER_PUMP_TERMINAL;
        }
        CHECK_GOTO(host_terminal, cleanup);
    }
    CHECK_GOTO(meiso_composition_debug_peer_snapshot(
          fixture->host, &terminal
      ) &&
          terminal.terminal && terminal.session.state ==
              MEISO_SESSION_STATE_FAILED, cleanup);
    memcpy(host_raw, fixture->host_arena, fixture->host_bytes);
    memset(&pump, 0x7c, sizeof(pump));
    CHECK_GOTO(meiso_composition_debug_peer_pump(
              fixture->host, true, true, &pump
          ) == MEISO_COMPOSITION_DEBUG_PEER_PUMP_TERMINAL &&
          bytes_are(&pump, 0x7c, sizeof(pump)) &&
          memcmp(host_raw, fixture->host_arena, fixture->host_bytes) == 0,
          cleanup);

    if (!initialize_fixture(&fresh, UINT64_C(0x1000))) {
        goto cleanup;
    }
    CHECK_GOTO(meiso_composition_debug_peer_snapshot(
              fresh.host, &fresh_host
          ) && meiso_composition_debug_peer_snapshot(
              fresh.edge, &fresh_edge
          ), cleanup);
    CHECK_GOTO(!session_id_equal(
              before.session.session_id, fresh_host.session.session_id
          ) && session_id_equal(
              fresh_host.session.session_id, fresh_edge.session.session_id
          ) && fresh_host.phase == MEISO_COMPOSITION_DEBUG_PEER_PHASE_READY &&
          fresh_edge.phase == MEISO_COMPOSITION_DEBUG_PEER_PHASE_READY &&
          fresh_host.pending_source_count == 0U &&
          fresh_edge.pending_source_count == 0U &&
          fresh_host.next_ticket == 1U && fresh_edge.next_ticket == 1U &&
          fresh_host.boundary_serial == 1U &&
          fresh_edge.boundary_serial == 1U, cleanup);
    CHECK_GOTO(memcmp(
                   host_raw, fixture->host_arena, fixture->host_bytes
               ) == 0 && memcmp(
                   edge_raw, fixture->edge_arena, fixture->edge_bytes
               ) == 0, cleanup);
    success = true;
cleanup:
    destroy_fixture(&fresh);
    free(edge_raw);
    free(host_raw);
    return success;
}

int main(void)
{
    PeerFixture fixture = {.host_fd = -1, .edge_fd = -1};

    if (!test_invalid_config_and_alias() ||
        !initialize_fixture(&fixture, 0U) ||
        !test_routes(&fixture) ||
        !test_ready_failures_are_atomic(&fixture) ||
        !test_state_and_stream(&fixture) ||
        !test_blob(&fixture) ||
        !test_terminal_and_new_session(&fixture)) {
        destroy_fixture(&fixture);
        return 1;
    }
    destroy_fixture(&fixture);
    puts("composition debug peer tests passed");
    return 0;
}
