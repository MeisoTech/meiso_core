#define _POSIX_C_SOURCE 200809L

#include <meiso/host/composition_runtime.h>
#include "composition_edge_client_fixture.h"
#include <meiso/host/remote_edge.h>
#include <meiso/composition/scene_store.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/*
 * Source-checkpoint expansion matrix:
 *
 *  1. exact arena/alignment, invalid config, every public alias/no-partial;
 *  2. pre-READY mesh plus six desired commands and local replacement truth;
 *  3. real AF_INET Composition READY with exact 6 endpoints/18 routes;
 *  4. desired offer/accept, actual receive, three inline Streams and Blob;
 *  5. exact PREPARED presentation-token echo and all stale suppressions;
 *  6. render-only, camera-only and both-route external-media connections;
 *  7. catalog, Session, transport and explicit-abort terminal boundaries;
 *  8. fresh arena/socket/Session no-resume and bounded resource counters.
 *
 * The fixture fixes the real transport and Profile identity without
 * manufacturing wire frames or application-owned counters.
 */

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            (void)fprintf(                                                     \
                stderr, "CHECK failed at %s:%d: %s\n",                      \
                __FILE__, __LINE__, #condition                                \
            );                                                                 \
            return false;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_GOTO(condition, label)                                           \
    do {                                                                       \
        if (!(condition)) {                                                    \
            (void)fprintf(                                                     \
                stderr, "CHECK failed at %s:%d: %s\n",                      \
                __FILE__, __LINE__, #condition                                \
            );                                                                 \
            goto label;                                                        \
        }                                                                      \
    } while (0)

static bool buffer_is_byte(const void *buffer, size_t size, uint8_t value)
{
    const uint8_t *bytes = (const uint8_t *)buffer;
    size_t i;

    if (buffer == NULL) {
        return false;
    }
    for (i = 0U; i < size; ++i) {
        if (bytes[i] != value) {
            return false;
        }
    }
    return true;
}

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
    ROUTE_RENDER_EXTERNAL = 14,
    ROUTE_CAMERA_DESIRED = 15,
    ROUTE_CAMERA_ACTUAL = 16,
    ROUTE_CAMERA_EXTERNAL = 17
};

typedef struct {
    uint8_t capability[MEISO_COMPOSITION_PROFILE_DYNAMIC_CAPABILITY_BYTES];
    MeisoRuntimeEndpointDescriptor
        endpoints[MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT];
    MeisoCompositionProfileRouteDescriptor
        routes[MEISO_COMPOSITION_PROFILE_ROUTE_COUNT];
} CatalogStorage;

typedef struct {
    int host_fd;
    int edge_fd;
    int host_fd_flags;
    int edge_fd_flags;
    void *app_arena;
    size_t app_bytes;
    MeisoCompositionHostRuntime *app;
    void *edge_arena;
    size_t edge_bytes;
    MeisoCompositionControlSession *edge;
    MeisoCompositionHostRuntimeConfig app_config;
    MeisoCompositionControlSessionConfig edge_config;
    CatalogStorage catalog;
} HostRuntimeFixture;

static const MeisoPeerId HOST_PEER = {
    UINT64_C(0x1111111111111111), UINT64_C(0x1212121212121212)
};
static const MeisoPeerId EDGE_PEER = {
    UINT64_C(0x2121212121212121), UINT64_C(0x2222222222222222)
};

static void close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static bool set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);

    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool make_deadline(struct timespec *out_deadline)
{
    if (out_deadline == NULL ||
        clock_gettime(CLOCK_MONOTONIC, out_deadline) != 0) {
        return false;
    }
    out_deadline->tv_sec += 5;
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
        INT64_C(1000000000) +
        (int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec;
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
        const int result = poll(&item, (nfds_t)1U, timeout);

        if (result > 0) {
            return (item.revents & POLLNVAL) == 0 &&
                (item.revents & (short)(events | POLLERR | POLLHUP)) != 0;
        }
        if (result < 0 && errno != EINTR) {
            return false;
        }
        item.revents = 0;
    }
    return false;
}

static bool fd_ready_now(int fd, short event)
{
    struct pollfd item = {fd, event, 0};
    return poll(&item, (nfds_t)1U, 0) > 0 &&
        (item.revents & (short)(event | POLLERR | POLLHUP)) != 0;
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
    int socket_error = 0;
    socklen_t socket_error_size = (socklen_t)sizeof(socket_error);

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
                       client, SOL_SOCKET, SO_ERROR, &socket_error,
                       &socket_error_size
                   ) != 0 || socket_error != 0)) {
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

static MeisoCompositionProfileCatalogConfig catalog_config(void)
{
    MeisoCompositionProfileCatalogConfig value;

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

static void configure_source(
    MeisoCompositionControlSessionConfig *config,
    uint8_t source_index,
    MeisoRuntimeSchedulerWorkKind kind,
    uint8_t route_index
)
{
    config->sources[source_index].work_kind = kind;
    config->sources[source_index].route_index = route_index;
}

static void configure_edge_sources(MeisoCompositionControlSessionConfig *config)
{
    size_t index;

    configure_source(
        config, 0U, MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER,
        MEISO_COMPOSITION_CONTROL_SESSION_NO_ROUTE
    );
    configure_source(config, 1U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                     ROUTE_WIFI_DESIRED);
    configure_source(config, 2U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                     ROUTE_WIFI_ACTUAL);
    configure_source(config, 3U, MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
                     ROUTE_WIFI_DATA);
    configure_source(config, 4U, MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT,
                     ROUTE_MESH_BLOB);
    configure_source(config, 5U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                     ROUTE_PRESENTATION_DESIRED);
    configure_source(config, 6U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                     ROUTE_PRESENTATION_ACTUAL);
    configure_source(config, 7U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                     ROUTE_SCENE_DESIRED);
    configure_source(config, 8U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                     ROUTE_SCENE_ACTUAL);
    configure_source(config, 9U, MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
                     ROUTE_RENDER_RESULT);
    configure_source(config, 10U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                     ROUTE_DEVICE_DESIRED);
    configure_source(config, 11U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                     ROUTE_DEVICE_ACTUAL);
    configure_source(config, 12U, MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
                     ROUTE_DEVICE_DATA);
    configure_source(config, 13U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                     ROUTE_RENDER_DESIRED);
    configure_source(config, 14U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                     ROUTE_RENDER_ACTUAL);
    configure_source(config, 15U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                     ROUTE_CAMERA_DESIRED);
    configure_source(config, 16U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                     ROUTE_CAMERA_ACTUAL);
    for (index = 17U; index < MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT;
         ++index) {
        configure_source(
            config, (uint8_t)index,
            MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, ROUTE_WIFI_ACTUAL
        );
    }
}

static MeisoCompositionHostRuntimeConfig app_config(int fd, uint64_t nonce_salt)
{
    MeisoCompositionHostRuntimeConfig value;

    memset(&value, 0, sizeof(value));
    value.control_connected_fd = fd;
    value.local_peer_id = HOST_PEER;
    value.local_incarnation = 31U;
    value.local_nonce = (MeisoSessionNonce){UINT64_C(0x61), nonce_salt + 1U};
    value.expected_edge_peer_id = EDGE_PEER;
    value.protocol = (MeisoProtocolVersion){1U, 0U};
    value.catalog_generation = 1U;
    value.catalog = catalog_config();
    value.max_control_io_bytes_per_step = 17U;
    value.control_adapter_reserve_bytes = 65536U;
    value.control_runtime_reserve_bytes = 400000U;
    value.control_scheduler_reserve_bytes = 65536U;
    value.initial_desired_delivery_generation =
        (MeisoDeliveryGeneration){101U};
    value.media_connection_slots =
        MEISO_COMPOSITION_HOST_RUNTIME_MAX_MEDIA_CONNECTIONS;
    value.max_media_io_bytes_per_step = 17U;
    return value;
}

static MeisoCompositionControlSessionConfig edge_config(
    int fd,
    uint64_t nonce_salt
)
{
    MeisoCompositionControlSessionConfig value;

    memset(&value, 0, sizeof(value));
    value.connected_fd = fd;
    value.local_role = MEISO_SESSION_ROLE_EDGE;
    value.local_peer_id = EDGE_PEER;
    value.local_incarnation = 41U;
    value.local_nonce = (MeisoSessionNonce){UINT64_C(0x71), nonce_salt + 2U};
    value.expected_peer_id = HOST_PEER;
    value.protocol = (MeisoProtocolVersion){1U, 0U};
    value.catalog_generation = 1U;
    value.catalog = catalog_config();
    value.max_io_bytes_per_step = 17U;
    value.adapter_reserve_bytes = 65536U;
    value.runtime_reserve_bytes = 400000U;
    value.scheduler_reserve_bytes = 65536U;
    configure_edge_sources(&value);
    return value;
}

static void destroy_fixture(HostRuntimeFixture *fixture)
{
    if (fixture != NULL) {
        close_fd(&fixture->host_fd);
        close_fd(&fixture->edge_fd);
        free(fixture->app_arena);
        free(fixture->edge_arena);
        memset(fixture, 0, sizeof(*fixture));
        fixture->host_fd = -1;
        fixture->edge_fd = -1;
    }
}

static bool initialize_fixture(HostRuntimeFixture *fixture, uint64_t nonce_salt)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->host_fd = -1;
    fixture->edge_fd = -1;
    CHECK(make_tcp_pair(&fixture->host_fd, &fixture->edge_fd));
    fixture->host_fd_flags = fcntl(fixture->host_fd, F_GETFL, 0);
    fixture->edge_fd_flags = fcntl(fixture->edge_fd, F_GETFL, 0);
    CHECK_GOTO(fixture->host_fd_flags >= 0 && fixture->edge_fd_flags >= 0,
               fail);
    fixture->app_config = app_config(fixture->host_fd, nonce_salt);
    fixture->edge_config = edge_config(fixture->edge_fd, nonce_salt);
    CHECK_GOTO(meiso_composition_profile_build_catalog(
        &fixture->app_config.catalog,
        (MeisoMutableBuffer){fixture->catalog.capability,
                             sizeof(fixture->catalog.capability)},
        fixture->catalog.endpoints,
        MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT,
        fixture->catalog.routes,
        MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
    ) == MEISO_COMPOSITION_PROFILE_OK, fail);
    fixture->app_bytes = meiso_composition_host_runtime_memory_required(
        &fixture->app_config
    );
    fixture->edge_bytes = meiso_composition_control_session_memory_required(
        &fixture->edge_config
    );
    CHECK_GOTO(fixture->app_bytes != 0U && fixture->edge_bytes != 0U, fail);
    fixture->app_arena = calloc(1U, fixture->app_bytes);
    fixture->edge_arena = calloc(1U, fixture->edge_bytes);
    CHECK_GOTO(fixture->app_arena != NULL && fixture->edge_arena != NULL,
               fail);
    CHECK_GOTO(meiso_composition_host_runtime_init(
        fixture->app_arena, fixture->app_bytes, &fixture->app_config,
        &fixture->app
    ) == MEISO_COMPOSITION_HOST_RUNTIME_INIT_OK, fail);
    CHECK_GOTO(meiso_composition_control_session_init(
        fixture->edge_arena, fixture->edge_bytes, &fixture->edge_config,
        &fixture->edge
    ) == MEISO_COMPOSITION_CONTROL_SESSION_INIT_OK, fail);
    CHECK_GOTO(fcntl(fixture->host_fd, F_GETFL, 0) ==
                   fixture->host_fd_flags &&
               fcntl(fixture->edge_fd, F_GETFL, 0) ==
                   fixture->edge_fd_flags,
               fail);
    return true;

fail:
    destroy_fixture(fixture);
    return false;
}

static MeisoCompositionRemoteEdgeConfig remote_edge_config(
    HostRuntimeFixture *fixture,
    uint64_t continuity_serial
)
{
    MeisoCompositionRemoteEdgeConfig config;

    memset(&config, 0, sizeof(config));
    config.host_runtime = fixture->app;
    config.host_runtime_arena = fixture->app_arena;
    config.host_runtime_arena_size = fixture->app_bytes;
    config.catalog_generation = fixture->app_config.catalog_generation;
    config.catalog = fixture->app_config.catalog;
    config.continuity_serial = continuity_serial;
    return config;
}

static bool initialize_remote_edge(
    HostRuntimeFixture *fixture,
    uint64_t continuity_serial,
    void **out_arena,
    MeisoEdgeHandle **out_handle
)
{
    MeisoCompositionRemoteEdgeConfig config;
    const size_t required =
        meiso_composition_remote_edge_memory_required();
    void *arena;

    CHECK(fixture != NULL && out_arena != NULL && out_handle != NULL &&
          required != 0U);
    *out_arena = NULL;
    *out_handle = NULL;
    arena = calloc(1U, required);
    if (arena == NULL) {
        return false;
    }
    if (((uintptr_t)arena %
         meiso_composition_remote_edge_alignment_required()) != 0U) {
        free(arena);
        return false;
    }
    config = remote_edge_config(fixture, continuity_serial);
    if (meiso_composition_remote_edge_init(
            arena, required, &config, out_handle
        ) != MEISO_COMPOSITION_REMOTE_EDGE_INIT_OK) {
        free(arena);
        return false;
    }
    *out_arena = arena;
    return true;
}

static bool content_id_equal(MeisoContentId lhs, MeisoContentId rhs)
{
    return meiso_content_id_equal(&lhs, &rhs);
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

static MeisoCompositionSceneDesired scene_value(MeisoContentId mesh_id)
{
    MeisoCompositionSceneDesired value;
    MeisoCompositionSceneObject *object;

    memset(&value, 0, sizeof(value));
    value.camera.space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    value.camera.space_epoch = 1U;
    value.camera.source_time = (MeisoTimePoint){1U, 100U};
    value.camera.position[2] = 3.0F;
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

static bool receipt_is_desired(
    const MeisoCompositionHostRuntimeCommandReceipt *receipt,
    uint64_t command_id,
    MeisoCompositionHostRuntimeCommandKind kind,
    MeisoCompositionHostRuntimeDesiredChannel channel,
    uint64_t version
)
{
    return receipt->command_id == command_id && receipt->kind == kind &&
        receipt->local_result == MEISO_COMPOSITION_HOST_RUNTIME_LOCAL_QUEUED &&
        !receipt->replaced_command_present && receipt->desired_state_present &&
        receipt->desired_channel == channel &&
        receipt->desired_state_version == version &&
        !receipt->mesh_content_id_present;
}

static bool queue_pre_ready_commands(HostRuntimeFixture *fixture)
{
    const MeisoWifiTelemetryDesiredParameters wifi = {
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
            MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM,
        1000U
    };
    const MeisoWifiTelemetryDesiredParameters wifi_replacement = {
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE,
        500U
    };
    const MeisoDeviceTelemetryParameters device = {
        MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC |
            MEISO_DEVICE_TELEMETRY_FIELD_CPU_LOAD_PPM,
        500U
    };
    const MeisoCompositionMediaDesired render_media = {
        MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        320U, 240U, 10000U, 0U, 1U,
        MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
    };
    const MeisoCompositionMediaDesired camera_media = {
        MEISO_COMPOSITION_MEDIA_PIXEL_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        320U, 240U, 10000U, 0U, 1U,
        MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
    };
    const MeisoCompositionHostRuntimePresentationSettings presentation = {
        MEISO_COMPOSITION_SCENE_PLAN_USE_SAFETY_FRAME,
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH,
        MEISO_COMPOSITION_SCENE_TARGET_HEIGHT,
        MEISO_COMPOSITION_SCENE_COLOR_RGBA8888,
        MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32,
        1U,
        1000U
    };
    uint8_t cube[512];
    uint8_t bad_mesh[512];
    uint8_t *arena_before = NULL;
    size_t cube_size = 0U;
    MeisoCompositionHostRuntimeCommandReceipt receipt;
    MeisoCompositionHostRuntimeCommandReceipt receipt_before;
    MeisoCompositionHostRuntimeCommandReceipt mesh_receipt;
    MeisoCompositionHostRuntimeCommandReceipt wifi_receipt;
    MeisoCompositionHostRuntimeCommandReceipt replacement_receipt;
    MeisoCompositionHostRuntimePresentationSettings invalid_presentation;
    MeisoCompositionHostRuntimeStatus replacement_status;
    MeisoCompositionSceneDesired scene;
    MeisoCompositionHostRuntimeSnapshot snapshot_value;
    bool ok = false;

    CHECK(meiso_composition_scene_mesh_encode(
              meiso_composition_scene_canonical_cube(),
              (MeisoMutableBuffer){cube, sizeof(cube)}, &cube_size
          ) == MEISO_COMPOSITION_SCENE_CODEC_OK && cube_size == 304U);
    arena_before = malloc(fixture->app_bytes);
    CHECK(arena_before != NULL);

    memcpy(arena_before, fixture->app_arena, fixture->app_bytes);
    memset(&receipt, 0xa5, sizeof(receipt));
    receipt_before = receipt;
    CHECK_GOTO(meiso_composition_host_runtime_set_wifi(
                   fixture->app, true, NULL, &receipt
               ) == MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT &&
               memcmp(fixture->app_arena, arena_before,
                      fixture->app_bytes) == 0 &&
               memcmp(&receipt, &receipt_before, sizeof(receipt)) == 0,
               cleanup);

    memcpy(arena_before, fixture->app_arena, fixture->app_bytes);
    CHECK_GOTO(meiso_composition_host_runtime_set_wifi(
                   fixture->app, true, &wifi,
                   (MeisoCompositionHostRuntimeCommandReceipt *)
                       fixture->app_arena
               ) == MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT &&
               memcmp(fixture->app_arena, arena_before,
                      fixture->app_bytes) == 0,
               cleanup);

    memcpy(bad_mesh, cube, cube_size);
    bad_mesh[0] ^= UINT8_C(0x01);
    memcpy(arena_before, fixture->app_arena, fixture->app_bytes);
    memset(&receipt, 0xa5, sizeof(receipt));
    receipt_before = receipt;
    CHECK_GOTO(meiso_composition_host_runtime_put_mesh(
                   fixture->app, (MeisoByteView){bad_mesh, cube_size},
                   &receipt
               ) == MEISO_COMPOSITION_HOST_RUNTIME_CONFLICT &&
               memcmp(fixture->app_arena, arena_before,
                      fixture->app_bytes) == 0 &&
               memcmp(&receipt, &receipt_before, sizeof(receipt)) == 0,
               cleanup);

    CHECK_GOTO(meiso_composition_host_runtime_put_mesh(
                   fixture->app, (MeisoByteView){cube, cube_size},
                   &mesh_receipt
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
               mesh_receipt.command_id == 1U &&
               mesh_receipt.kind ==
                   MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_PUT_MESH &&
               mesh_receipt.local_result ==
                   MEISO_COMPOSITION_HOST_RUNTIME_LOCAL_QUEUED &&
               !mesh_receipt.desired_state_present &&
               mesh_receipt.mesh_content_id_present,
               cleanup);
    scene = scene_value(mesh_receipt.mesh_content_id);

    CHECK_GOTO(meiso_composition_host_runtime_set_wifi(
                   fixture->app, true, &wifi, &wifi_receipt
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
               receipt_is_desired(
                   &wifi_receipt, 2U,
                   MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_WIFI,
                   MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI, 1U
               ), cleanup);
    invalid_presentation = presentation;
    invalid_presentation.max_result_hz = 0U;
    memcpy(arena_before, fixture->app_arena, fixture->app_bytes);
    memset(&receipt, 0xa5, sizeof(receipt));
    receipt_before = receipt;
    CHECK_GOTO(meiso_composition_host_runtime_set_presentation(
                   fixture->app, true, &invalid_presentation, &receipt
               ) == MEISO_COMPOSITION_HOST_RUNTIME_CONFLICT &&
               memcmp(fixture->app_arena, arena_before,
                      fixture->app_bytes) == 0 &&
               memcmp(&receipt, &receipt_before, sizeof(receipt)) == 0,
               cleanup);
    invalid_presentation.max_result_hz =
        MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ + 1U;
    memcpy(arena_before, fixture->app_arena, fixture->app_bytes);
    memset(&receipt, 0xa5, sizeof(receipt));
    receipt_before = receipt;
    CHECK_GOTO(meiso_composition_host_runtime_set_presentation(
                   fixture->app, true, &invalid_presentation, &receipt
               ) == MEISO_COMPOSITION_HOST_RUNTIME_CONFLICT &&
               memcmp(fixture->app_arena, arena_before,
                      fixture->app_bytes) == 0 &&
               memcmp(&receipt, &receipt_before, sizeof(receipt)) == 0,
               cleanup);
    CHECK_GOTO(meiso_composition_host_runtime_set_presentation(
                   fixture->app, true, &presentation, &receipt
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
               receipt_is_desired(
                   &receipt, 3U,
                   MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_PRESENTATION,
                   MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_PRESENTATION, 1U
               ) && receipt.plan_generation_present &&
               receipt.plan_generation == 1U,
               cleanup);
    CHECK_GOTO(meiso_composition_host_runtime_set_scene(
                   fixture->app, true, &scene, &receipt
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
               receipt_is_desired(
                   &receipt, 4U,
                   MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_SCENE,
                   MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_SCENE, 1U
               ), cleanup);
    CHECK_GOTO(meiso_composition_host_runtime_set_device(
                   fixture->app, true, &device, &receipt
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
               receipt_is_desired(
                   &receipt, 5U,
                   MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_DEVICE,
                   MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_DEVICE, 1U
               ), cleanup);
    CHECK_GOTO(meiso_composition_host_runtime_set_render_media(
                   fixture->app, true, &render_media, &receipt
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
               receipt_is_desired(
                   &receipt, 6U,
                   MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_RENDER_MEDIA,
                   MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_RENDER_MEDIA, 1U
               ), cleanup);
    CHECK_GOTO(meiso_composition_host_runtime_set_camera_media(
                   fixture->app, true, &camera_media, &receipt
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
               receipt_is_desired(
                   &receipt, 7U,
                   MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_CAMERA_MEDIA,
                   MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_CAMERA_MEDIA, 1U
               ), cleanup);
    memset(&replacement_receipt, 0, sizeof(replacement_receipt));
    replacement_status = meiso_composition_host_runtime_set_wifi(
        fixture->app, true, &wifi_replacement, &replacement_receipt
    );
    if (replacement_status != MEISO_COMPOSITION_HOST_RUNTIME_OK ||
        replacement_receipt.command_id != 8U ||
        replacement_receipt.local_result !=
            MEISO_COMPOSITION_HOST_RUNTIME_LOCAL_REPLACED_PENDING ||
        !replacement_receipt.replaced_command_present ||
        replacement_receipt.replaced_command_id != wifi_receipt.command_id ||
        replacement_receipt.desired_state_version != 2U) {
        (void)fprintf(
            stderr,
            "WiFi replacement failed: status=%u id=%llu result=%u "
            "replaced=%u old=%llu expected_old=%llu version=%llu\n",
            (unsigned)replacement_status,
            (unsigned long long)replacement_receipt.command_id,
            (unsigned)replacement_receipt.local_result,
            replacement_receipt.replaced_command_present ? 1U : 0U,
            (unsigned long long)replacement_receipt.replaced_command_id,
            (unsigned long long)wifi_receipt.command_id,
            (unsigned long long)replacement_receipt.desired_state_version
        );
    }
    CHECK_GOTO(replacement_status == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
               replacement_receipt.command_id == 8U &&
               replacement_receipt.kind ==
                   MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_WIFI &&
               replacement_receipt.local_result ==
                   MEISO_COMPOSITION_HOST_RUNTIME_LOCAL_REPLACED_PENDING &&
               replacement_receipt.replaced_command_present &&
               replacement_receipt.replaced_command_id ==
                   wifi_receipt.command_id &&
               replacement_receipt.desired_state_present &&
               replacement_receipt.desired_channel ==
                   MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI &&
               replacement_receipt.desired_state_version == 2U,
               cleanup);

    CHECK_GOTO(meiso_composition_host_runtime_snapshot(
                   fixture->app, &snapshot_value
               ) && snapshot_value.phase ==
                   MEISO_COMPOSITION_HOST_RUNTIME_PHASE_HANDSHAKE &&
               snapshot_value.next_command_id == 9U &&
               snapshot_value.plan_generation_high_water == 1U &&
               snapshot_value.mesh.queued &&
               snapshot_value.mesh.command_id == mesh_receipt.command_id &&
               snapshot_value.mesh.total_bytes == cube_size &&
               content_id_equal(
                   snapshot_value.mesh.content_id,
                   mesh_receipt.mesh_content_id
               ) && snapshot_value.desired[
                   MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
               ].command_id == replacement_receipt.command_id &&
               snapshot_value.desired[
                   MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
               ].state_version == 2U &&
               snapshot_value.desired[
                   MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
               ].replaced_pending_count == 1U,
               cleanup);
    for (size_t index = 0U;
         index < MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_COUNT;
         ++index) {
        const MeisoCompositionHostRuntimeDesiredSnapshot *desired =
            &snapshot_value.desired[index];
        CHECK_GOTO(desired->requested && desired->enabled &&
                   desired->delivery_generation.value == 101U &&
                   !desired->delivery_active && desired->delivery_pending &&
                   !desired->delivery_accepted && !desired->delivery_failed &&
                   desired->state_version == (index ==
                       MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI ? 2U : 1U),
                   cleanup);
    }
    ok = true;

cleanup:
    free(arena_before);
    return ok;
}

static bool host_pump_status_benign(MeisoCompositionHostRuntimePumpStatus status)
{
    return status == MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS ||
        status == MEISO_COMPOSITION_HOST_RUNTIME_PUMP_IDLE ||
        status == MEISO_COMPOSITION_HOST_RUNTIME_PUMP_WOULD_BLOCK;
}

static bool edge_pump_status_benign(MeisoCompositionControlSessionPumpStatus status)
{
    return status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_PROGRESS ||
        status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_IDLE ||
        status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_EVENT ||
        status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_WOULD_BLOCK ||
        status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_TX_BUSY ||
        status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_REBIND_PROGRESS;
}

static bool resolve_unrelated_desired_offer(
    MeisoCompositionControlSession *edge,
    const MeisoRuntimeEvent *event
)
{
    static const uint8_t desired_routes[] = {
        ROUTE_WIFI_DESIRED,
        ROUTE_PRESENTATION_DESIRED,
        ROUTE_SCENE_DESIRED,
        ROUTE_DEVICE_DESIRED,
        ROUTE_RENDER_DESIRED,
        ROUTE_CAMERA_DESIRED
    };
    static const uint8_t accept_sources[] = {1U, 5U, 7U, 10U, 13U, 15U};
    MeisoRuntimeSchedulerWork work;
    size_t index;

    if (edge == NULL || event == NULL ||
        event->type != MEISO_RUNTIME_EVENT_STATE_OFFER) {
        return false;
    }
    for (index = 0U;
         index < sizeof(desired_routes) / sizeof(desired_routes[0]);
         ++index) {
        MeisoRuntimeRouteKey route;

        if (!meiso_composition_control_session_route_key(
                edge, desired_routes[index], &route
            ) || !route_equal(event->route, route)) {
            continue;
        }
        if (meiso_composition_control_session_commit_state_receive(
                edge, event->route, &event->state_offer,
                MEISO_STATE_RECEIVE_STALE
            ) != MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            return false;
        }
        memset(&work, 0, sizeof(work));
        work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
        work.value.state_accept.route = event->route;
        work.value.state_accept.generation = event->state_offer.generation;
        work.value.state_accept.owner = event->state_offer.state.owner;
        work.value.state_accept.version = event->state_offer.state.version;
        return meiso_composition_control_session_queue_work(
                   edge, accept_sources[index], &work
               ) == MEISO_COMPOSITION_CONTROL_SESSION_OK;
    }
    return false;
}

static bool drive_control_ready(HostRuntimeFixture *fixture)
{
    struct timespec deadline;

    CHECK(make_deadline(&deadline));
    while (remaining_ms(&deadline) > 0) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true, false, false
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionHostRuntimePumpStatus host_status;
        MeisoCompositionControlSessionPumpResult edge_result;
        MeisoCompositionControlSessionPumpStatus edge_status;
        MeisoCompositionHostRuntimeSnapshot host_snapshot;
        MeisoCompositionControlSessionSnapshot edge_snapshot;

        memset(&host_result, 0, sizeof(host_result));
        host_status = meiso_composition_host_runtime_pump(
            fixture->app, &ready, &host_result
        );
        if (!host_pump_status_benign(host_status)) {
            (void)meiso_composition_host_runtime_snapshot(
                fixture->app, &host_snapshot
            );
            (void)fprintf(
                stderr,
                "HostRuntime READY pump failed: status=%u phase=%u reason=%u\n",
                (unsigned)host_status, (unsigned)host_snapshot.phase,
                (unsigned)host_snapshot.terminal_reason
            );
            return false;
        }
        memset(&edge_result, 0, sizeof(edge_result));
        edge_status = meiso_composition_control_session_pump(
            fixture->edge, true, true, &edge_result
        );
        if (!edge_pump_status_benign(edge_status)) {
            (void)fprintf(
                stderr, "Edge READY pump failed: status=%u\n",
                (unsigned)edge_status
            );
            return false;
        }
        CHECK(meiso_composition_host_runtime_snapshot(
                  fixture->app, &host_snapshot
              ) && meiso_composition_control_session_snapshot(
                  fixture->edge, &edge_snapshot
              ));
        if (host_snapshot.phase == MEISO_COMPOSITION_HOST_RUNTIME_PHASE_READY &&
            edge_snapshot.phase == MEISO_COMPOSITION_CONTROL_SESSION_PHASE_READY) {
            CHECK(host_snapshot.control_ready &&
                  host_snapshot.control_session_state ==
                      MEISO_SESSION_STATE_READY &&
                  host_snapshot.active_catalog_generation == 1U &&
                  edge_snapshot.runtime.active_catalog_generation == 1U &&
                  meiso_session_id_equal(
                      host_snapshot.control_session_id,
                      edge_snapshot.session.session_id
                  ));
            return true;
        }
    }
    (void)fprintf(stderr, "HostRuntime READY pump timed out\n");
    return false;
}

static bool drive_first_wifi_desired(HostRuntimeFixture *fixture)
{
    const MeisoOwnerScope host_owner = {
        MEISO_OWNER_ROLE_HOST, 31U
    };
    const MeisoWifiTelemetryDesiredParameters expected_parameters = {
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE,
        500U
    };
    struct timespec deadline;
    MeisoRuntimeRouteKey expected_route;
    MeisoStateReplica *replica = NULL;
    void *replica_arena = NULL;
    MeisoBlobStore *blob_store = NULL;
    void *blob_arena = NULL;
    size_t replica_bytes;
    size_t blob_bytes;
    bool offer_received = false;
    bool accept_queued = false;
    bool accept_completed = false;
    bool blob_transfer_present = false;
    bool blob_published = false;
    MeisoRuntimeBlobTransferRef blob_transfer;
    uint64_t desired_events = 0U;
    bool ok = false;

    replica_bytes = meiso_state_replica_memory_required(
        &fixture->catalog.routes[ROUTE_WIFI_DESIRED].route.resource
    );
    CHECK(replica_bytes != 0U);
    replica_arena = calloc(1U, replica_bytes);
    CHECK(replica_arena != NULL);
    blob_bytes = meiso_blob_store_memory_required(
        &fixture->catalog.routes[ROUTE_MESH_BLOB].route.resource,
        &(MeisoBlobConfig){2U}
    );
    blob_arena = calloc(1U, blob_bytes);
    CHECK_GOTO(blob_bytes != 0U && blob_arena != NULL &&
               meiso_blob_store_init(
                   blob_arena, blob_bytes,
                   &fixture->catalog.routes[ROUTE_MESH_BLOB].route.resource,
                   &(MeisoBlobConfig){2U}, &blob_store
               ) == MEISO_BLOB_INIT_OK,
               cleanup);
    CHECK_GOTO(meiso_state_replica_init(
                   replica_arena, replica_bytes,
                   &fixture->catalog.routes[ROUTE_WIFI_DESIRED].route.resource,
                   &replica
               ) == MEISO_STATE_INIT_OK &&
               meiso_state_replica_begin_owner(replica, host_owner) ==
                   MEISO_STATE_OWNER_STARTED &&
               meiso_composition_control_session_route_key(
                   fixture->edge, ROUTE_WIFI_DESIRED, &expected_route
               ) && make_deadline(&deadline),
               cleanup);

    while (remaining_ms(&deadline) > 0) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true, false, false
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionHostRuntimePumpStatus host_status;
        MeisoCompositionControlSessionPumpResult edge_result;
        MeisoCompositionControlSessionPumpStatus edge_status;
        MeisoCompositionHostRuntimeSnapshot host_snapshot;
        MeisoRuntimeSchedulerCompletion completion;

        memset(&host_result, 0, sizeof(host_result));
        host_status = meiso_composition_host_runtime_pump(
            fixture->app, &ready, &host_result
        );
        CHECK_GOTO(host_pump_status_benign(host_status), cleanup);
        if (host_status == MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS &&
            host_result.event_present && host_result.event ==
                MEISO_COMPOSITION_HOST_RUNTIME_PUMP_DESIRED_DELIVERY &&
            host_result.desired_channel_present &&
            host_result.desired_channel ==
                MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI) {
            CHECK_GOTO(host_result.command_present &&
                       host_result.command_id == 8U,
                       cleanup);
            ++desired_events;
        }

        memset(&edge_result, 0, sizeof(edge_result));
        edge_status = meiso_composition_control_session_pump(
            fixture->edge, true, true, &edge_result
        );
        CHECK_GOTO(edge_pump_status_benign(edge_status), cleanup);
        if (edge_status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_EVENT &&
            edge_result.event_present) {
            if (edge_result.event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST) {
                MeisoRuntimeSchedulerWork result_work;

                memset(&result_work, 0, sizeof(result_work));
                if (edge_result.event.blob_operation ==
                    MEISO_RUNTIME_BLOB_BEGIN) {
                    CHECK_GOTO(!blob_transfer_present, cleanup);
                    blob_transfer = edge_result.event.blob_transfer;
                    blob_transfer_present = true;
                    result_work.kind =
                        MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT;
                    result_work.value.blob_result.transfer = blob_transfer;
                    result_work.value.blob_result.operation =
                        MEISO_RUNTIME_BLOB_BEGIN;
                    CHECK_GOTO(meiso_composition_control_session_queue_work(
                                   fixture->edge, 4U, &result_work
                               ) == MEISO_COMPOSITION_CONTROL_SESSION_OK,
                               cleanup);
                } else if (edge_result.event.blob_operation ==
                           MEISO_RUNTIME_BLOB_CHUNK) {
                    CHECK_GOTO(blob_transfer_present &&
                               edge_result.event.blob_transfer.transfer_id ==
                                   blob_transfer.transfer_id,
                               cleanup);
                } else if (edge_result.event.blob_operation ==
                           MEISO_RUNTIME_BLOB_COMMIT) {
                    MeisoRuntimeBlobPublishReport report;

                    CHECK_GOTO(blob_transfer_present && !blob_published &&
                               meiso_composition_control_session_publish_blob_receive(
                                   fixture->edge,
                                   &edge_result.event.blob_transfer,
                                   blob_store,
                                   meiso_composition_scene_mesh_blob_verify,
                                   NULL, &report
                               ) == MEISO_COMPOSITION_CONTROL_SESSION_OK &&
                               report.result ==
                                   MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
                               meiso_blob_store_count(blob_store) == 1U,
                               cleanup);
                    blob_published = true;
                    result_work.kind =
                        MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT;
                    result_work.value.blob_result.transfer = blob_transfer;
                    result_work.value.blob_result.operation =
                        MEISO_RUNTIME_BLOB_COMMIT;
                    CHECK_GOTO(meiso_composition_control_session_queue_work(
                                   fixture->edge, 4U, &result_work
                               ) == MEISO_COMPOSITION_CONTROL_SESSION_OK,
                               cleanup);
                } else {
                    CHECK_GOTO(false, cleanup);
                }
                goto edge_event_handled;
            }
            if (edge_result.event.type != MEISO_RUNTIME_EVENT_STATE_OFFER ||
                !route_equal(edge_result.event.route, expected_route)) {
                CHECK_GOTO(resolve_unrelated_desired_offer(
                               fixture->edge, &edge_result.event
                           ), cleanup);
                goto edge_event_handled;
            }
            MeisoRuntimeDesiredState desired;
            MeisoWifiTelemetryDesiredParameters parameters;
            MeisoStateReceiveStatus receive_status;
            MeisoRuntimeSchedulerWork accept;

            CHECK_GOTO(edge_result.event.type ==
                           MEISO_RUNTIME_EVENT_STATE_OFFER &&
                       route_equal(edge_result.event.route, expected_route) &&
                       owner_equal(
                           edge_result.event.state_offer.state.owner,
                           host_owner
                       ) && edge_result.event.state_offer.state.version == 2U,
                       cleanup);
            CHECK_GOTO(meiso_runtime_desired_state_decode(
                           edge_result.event.state_offer.state.payload,
                           &desired
                       ) && desired.enabled &&
                       meiso_wifi_telemetry_desired_parameters_decode(
                           desired.profile, &parameters
                       ) == MEISO_WIFI_TELEMETRY_CODEC_OK &&
                       parameters.selected_fields ==
                           expected_parameters.selected_fields &&
                       parameters.sample_period_ms ==
                           expected_parameters.sample_period_ms,
                       cleanup);
            receive_status = meiso_state_replica_receive(
                replica,
                edge_result.event.state_offer.state.owner,
                edge_result.event.state_offer.state.version,
                edge_result.event.state_offer.state.payload
            );
            /* The exact offer may replay before the queued accept arrives. */
            CHECK_GOTO(receive_status == (offer_received ?
                           MEISO_STATE_RECEIVE_DUPLICATE :
                           MEISO_STATE_RECEIVE_NEW) &&
                       meiso_composition_control_session_commit_state_receive(
                           fixture->edge, edge_result.event.route,
                           &edge_result.event.state_offer, receive_status
                       ) == MEISO_COMPOSITION_CONTROL_SESSION_OK,
                       cleanup);
            if (!offer_received) {
                memset(&accept, 0, sizeof(accept));
                accept.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
                accept.value.state_accept.route = edge_result.event.route;
                accept.value.state_accept.generation =
                    edge_result.event.state_offer.generation;
                accept.value.state_accept.owner =
                    edge_result.event.state_offer.state.owner;
                accept.value.state_accept.version =
                    edge_result.event.state_offer.state.version;
                CHECK_GOTO(meiso_composition_control_session_queue_work(
                               fixture->edge, 1U, &accept
                           ) == MEISO_COMPOSITION_CONTROL_SESSION_OK,
                           cleanup);
                offer_received = true;
                accept_queued = true;
            }
        }

edge_event_handled:

        memset(&completion, 0, sizeof(completion));
        if (meiso_composition_control_session_take_completion(
                fixture->edge, 1U, &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            CHECK_GOTO(accept_queued && completion.kind ==
                           MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT &&
                       completion.code ==
                           MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT &&
                       completion.runtime_status == MEISO_RUNTIME_OK,
                       cleanup);
            accept_completed = true;
        }
        memset(&completion, 0, sizeof(completion));
        if (meiso_composition_control_session_take_completion(
                fixture->edge, 4U, &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            CHECK_GOTO(completion.kind ==
                           MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT &&
                       completion.code ==
                           MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT &&
                       completion.runtime_status == MEISO_RUNTIME_OK,
                       cleanup);
        }

        CHECK_GOTO(meiso_composition_host_runtime_snapshot(
                       fixture->app, &host_snapshot
                   ), cleanup);
        if (offer_received && accept_completed && desired_events >= 1U &&
            host_snapshot.desired[
                MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
            ].delivery_accepted) {
            MeisoStateView current;
            CHECK_GOTO(offer_received && accept_completed &&
                       desired_events <= 2U &&
                       !host_snapshot.desired[
                           MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
                       ].delivery_pending &&
                       host_snapshot.desired[
                           MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
                       ].delivery_active &&
                       host_snapshot.desired[
                           MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
                       ].state_version == 2U &&
                       meiso_state_replica_current(replica, &current) &&
                       current.version == 2U &&
                       owner_equal(current.owner, host_owner),
                       cleanup);
            ok = true;
            break;
        }
    }
    if (!ok) {
        (void)fprintf(
            stderr,
            "WiFi desired timeout: offer=%u accept_queued=%u "
            "accept_completed=%u events=%llu\n",
            offer_received ? 1U : 0U, accept_queued ? 1U : 0U,
            accept_completed ? 1U : 0U,
            (unsigned long long)desired_events
        );
    }

cleanup:
    free(blob_arena);
    free(replica_arena);
    return ok;
}

static bool queue_single_wifi_desired(HostRuntimeFixture *fixture)
{
    const MeisoWifiTelemetryDesiredParameters parameters = {
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
            MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM,
        500U
    };
    MeisoCompositionHostRuntimeCommandReceipt receipt;

    return meiso_composition_host_runtime_set_wifi(
               fixture->app, true, &parameters, &receipt
           ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
        receipt_is_desired(
            &receipt, 1U, MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_WIFI,
            MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI, 1U
        );
}

static bool drive_single_wifi_desired(HostRuntimeFixture *fixture)
{
    const MeisoOwnerScope host_owner = {MEISO_OWNER_ROLE_HOST, 31U};
    struct timespec deadline;
    MeisoRuntimeRouteKey expected_route;
    MeisoStateReplica *replica = NULL;
    void *replica_arena = NULL;
    size_t replica_bytes;
    bool offer_seen = false;
    bool accept_seen = false;
    bool source_complete = false;
    bool ok = false;

    replica_bytes = meiso_state_replica_memory_required(
        &fixture->catalog.routes[ROUTE_WIFI_DESIRED].route.resource
    );
    CHECK(replica_bytes != 0U);
    replica_arena = calloc(1U, replica_bytes);
    CHECK(replica_arena != NULL);
    CHECK_GOTO(meiso_state_replica_init(
                   replica_arena, replica_bytes,
                   &fixture->catalog.routes[ROUTE_WIFI_DESIRED].route.resource,
                   &replica
               ) == MEISO_STATE_INIT_OK &&
               meiso_state_replica_begin_owner(replica, host_owner) ==
                   MEISO_STATE_OWNER_STARTED &&
               meiso_composition_control_session_route_key(
                   fixture->edge, ROUTE_WIFI_DESIRED, &expected_route
               ) &&
               make_deadline(&deadline),
               cleanup);

    while (remaining_ms(&deadline) > 0) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true, false, false
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionControlSessionPumpResult edge_result;
        MeisoCompositionHostRuntimePumpStatus host_status;
        MeisoCompositionControlSessionPumpStatus edge_status;
        MeisoRuntimeSchedulerCompletion completion;
        MeisoCompositionHostRuntimeSnapshot snapshot;

        memset(&host_result, 0, sizeof(host_result));
        host_status = meiso_composition_host_runtime_pump(
            fixture->app, &ready, &host_result
        );
        CHECK_GOTO(host_pump_status_benign(host_status), cleanup);

        memset(&edge_result, 0, sizeof(edge_result));
        edge_status = meiso_composition_control_session_pump(
            fixture->edge, true, true, &edge_result
        );
        CHECK_GOTO(edge_pump_status_benign(edge_status), cleanup);
        if (edge_status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_EVENT &&
            edge_result.event_present) {
            MeisoRuntimeDesiredState desired;
            MeisoWifiTelemetryDesiredParameters parameters;
            MeisoStateReceiveStatus receive;
            MeisoRuntimeSchedulerWork accept;

            CHECK_GOTO(!offer_seen && edge_result.event.type ==
                           MEISO_RUNTIME_EVENT_STATE_OFFER &&
                       route_equal(
                           edge_result.event.route,
                           expected_route
                       ), cleanup);
            receive = meiso_state_replica_receive(
                replica, edge_result.event.state_offer.state.owner,
                edge_result.event.state_offer.state.version,
                edge_result.event.state_offer.state.payload
            );
            CHECK_GOTO(receive == MEISO_STATE_RECEIVE_NEW &&
                       meiso_runtime_desired_state_decode(
                           edge_result.event.state_offer.state.payload,
                           &desired
                       ) && desired.enabled &&
                       meiso_wifi_telemetry_desired_parameters_decode(
                           desired.profile, &parameters
                       ) == MEISO_WIFI_TELEMETRY_CODEC_OK &&
                       parameters.selected_fields ==
                           (MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
                            MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM) &&
                       parameters.sample_period_ms == 500U &&
                       meiso_composition_control_session_commit_state_receive(
                           fixture->edge, edge_result.event.route,
                           &edge_result.event.state_offer, receive
                       ) == MEISO_COMPOSITION_CONTROL_SESSION_OK,
                       cleanup);
            memset(&accept, 0, sizeof(accept));
            accept.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
            accept.value.state_accept.route = edge_result.event.route;
            accept.value.state_accept.generation =
                edge_result.event.state_offer.generation;
            accept.value.state_accept.owner =
                edge_result.event.state_offer.state.owner;
            accept.value.state_accept.version =
                edge_result.event.state_offer.state.version;
            CHECK_GOTO(meiso_composition_control_session_queue_work(
                           fixture->edge, 1U, &accept
                       ) == MEISO_COMPOSITION_CONTROL_SESSION_OK,
                       cleanup);
            offer_seen = true;
        }
        memset(&completion, 0, sizeof(completion));
        if (meiso_composition_control_session_take_completion(
                fixture->edge, 1U, &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            CHECK_GOTO(completion.kind ==
                           MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT &&
                       completion.code ==
                           MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT,
                       cleanup);
            source_complete = true;
        }
        CHECK_GOTO(meiso_composition_host_runtime_snapshot(
                       fixture->app, &snapshot
                   ), cleanup);
        accept_seen = snapshot.desired[
            MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
        ].delivery_accepted;
        if (offer_seen && accept_seen && source_complete) {
            ok = true;
            break;
        }
    }

cleanup:
    free(replica_arena);
    return ok;
}

static bool queue_wifi_actual(
    HostRuntimeFixture *fixture,
    MeisoStatePublisher **out_publisher,
    void **out_arena
)
{
    const MeisoOwnerScope edge_owner = {MEISO_OWNER_ROLE_EDGE, 41U};
    const MeisoOwnerScope host_owner = {MEISO_OWNER_ROLE_HOST, 31U};
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const MeisoWifiTelemetryEffectiveParameters effective = {
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
            MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM,
        500U
    };
    uint8_t effective_wire[
        MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE
    ];
    uint8_t actual_wire[MEISO_WIFI_TELEMETRY_ACTUAL_STATE_MAX_PAYLOAD_SIZE];
    MeisoRuntimeActualState actual;
    MeisoByteView payload;
    MeisoStateOffer offer;
    MeisoRuntimeRouteKey route;
    MeisoRuntimeSchedulerWork work;
    size_t effective_size = 0U;
    size_t required;

    *out_publisher = NULL;
    *out_arena = NULL;
    required = meiso_state_publisher_memory_required(
        &fixture->catalog.routes[ROUTE_WIFI_ACTUAL].route.resource,
        &state_config
    );
    CHECK(required != 0U);
    *out_arena = calloc(1U, required);
    CHECK(*out_arena != NULL);
    CHECK(meiso_state_publisher_init(
        *out_arena, required,
        &fixture->catalog.routes[ROUTE_WIFI_ACTUAL].route.resource,
        &state_config, out_publisher
    ) == MEISO_STATE_INIT_OK);
    CHECK(meiso_wifi_telemetry_effective_parameters_encode(
        &effective,
        (MeisoMutableBuffer){effective_wire, sizeof(effective_wire)},
        &effective_size
    ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    memset(&actual, 0, sizeof(actual));
    actual.observed_desired = (MeisoRuntimeDesiredReference){
        true, host_owner, 1U
    };
    actual.applied_desired = actual.observed_desired;
    actual.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    actual.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    actual.effective_enabled = true;
    actual.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING;
    actual.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    actual.production_epoch = 77U;
    actual.profile = (MeisoByteView){effective_wire, effective_size};
    CHECK(meiso_runtime_actual_state_encode(
        &actual, (MeisoMutableBuffer){actual_wire, sizeof(actual_wire)},
        &payload
    ));
    CHECK(meiso_state_publisher_publish(
              *out_publisher, edge_owner, 1U, payload
          ) == MEISO_STATE_PUBLISH_NEW &&
          meiso_state_publisher_begin_delivery(
              *out_publisher, (MeisoDeliveryGeneration){201U}
          ) == MEISO_STATE_DELIVERY_STARTED &&
          meiso_state_publisher_next_offer(*out_publisher, &offer) &&
          meiso_composition_control_session_route_key(
              fixture->edge, ROUTE_WIFI_ACTUAL, &route
          ));
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
    work.value.state_offer.route = route;
    work.value.state_offer.offer = offer;
    return meiso_composition_control_session_queue_work(
               fixture->edge, 2U, &work
           ) == MEISO_COMPOSITION_CONTROL_SESSION_OK;
}

static bool drive_wifi_actual(
    HostRuntimeFixture *fixture,
    MeisoStatePublisher *publisher
)
{
    struct timespec deadline;
    bool offer_complete = false;
    bool accept_received = false;
    uint64_t actual_events = 0U;

    CHECK(make_deadline(&deadline));
    while (remaining_ms(&deadline) > 0) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true, false, false
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionControlSessionPumpResult edge_result;
        MeisoRuntimeSchedulerCompletion completion;
        MeisoCompositionHostRuntimeSnapshot snapshot;

        memset(&host_result, 0, sizeof(host_result));
        CHECK(host_pump_status_benign(meiso_composition_host_runtime_pump(
            fixture->app, &ready, &host_result
        )));
        if (host_result.event_present && host_result.event ==
                MEISO_COMPOSITION_HOST_RUNTIME_PUMP_ACTUAL_UPDATED &&
            host_result.actual_channel_present &&
            host_result.actual_channel ==
                MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_WIFI) {
            ++actual_events;
        }
        memset(&edge_result, 0, sizeof(edge_result));
        CHECK(edge_pump_status_benign(meiso_composition_control_session_pump(
            fixture->edge, true, true, &edge_result
        )));
        if (edge_result.event_present) {
            CHECK(edge_result.event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT);
            CHECK(meiso_state_publisher_mark_delivery_accepted(
                publisher,
                edge_result.event.state_accept_generation,
                edge_result.event.state_accept_owner,
                edge_result.event.state_accept_version
            ) == MEISO_STATE_ACCEPT_CONFIRMED);
            accept_received = true;
        }
        memset(&completion, 0, sizeof(completion));
        if (meiso_composition_control_session_take_completion(
                fixture->edge, 2U, &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            CHECK(completion.kind ==
                      MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER &&
                  completion.code ==
                      MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT);
            offer_complete = true;
        }
        CHECK(meiso_composition_host_runtime_snapshot(fixture->app, &snapshot));
        if (offer_complete && accept_received &&
            snapshot.actual[MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_WIFI].present) {
            CHECK(actual_events == 3U &&
                  snapshot.actual[
                      MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_WIFI
                  ].production_status == MEISO_RUNTIME_PRODUCTION_RUNNING &&
                  snapshot.actual[
                      MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_WIFI
                  ].production_epoch == 77U &&
                  snapshot.wifi_effective_present &&
                  snapshot.wifi_effective.sample_period_ms == 500U);
            return true;
        }
    }
    return false;
}

static bool drive_inline_sample_once(
    HostRuntimeFixture *fixture,
    const MeisoRuntimeSchedulerWork *work,
    MeisoCompositionHostRuntimePumpResult *out_inline
)
{
    struct timespec deadline;
    bool completion_seen = false;
    bool inline_seen = false;

    CHECK(meiso_composition_control_session_queue_work(
              fixture->edge, 3U, work
          ) == MEISO_COMPOSITION_CONTROL_SESSION_OK &&
          make_deadline(&deadline));
    while (remaining_ms(&deadline) > 0) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true, false, false
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionControlSessionPumpResult edge_result;
        MeisoRuntimeSchedulerCompletion completion;

        memset(&host_result, 0, sizeof(host_result));
        CHECK(host_pump_status_benign(meiso_composition_host_runtime_pump(
            fixture->app, &ready, &host_result
        )));
        if (host_result.event_present && host_result.event ==
                MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INLINE_UPDATED) {
            CHECK(host_result.inline_channel_present &&
                  host_result.inline_channel ==
                      MEISO_COMPOSITION_HOST_RUNTIME_INLINE_WIFI &&
                  !host_result.command_present &&
                  !host_result.desired_channel_present &&
                  !host_result.actual_channel_present &&
                  !host_result.media_source_present);
            *out_inline = host_result;
            inline_seen = true;
        }
        memset(&edge_result, 0, sizeof(edge_result));
        CHECK(edge_pump_status_benign(meiso_composition_control_session_pump(
            fixture->edge, true, true, &edge_result
        )) && !edge_result.event_present);
        memset(&completion, 0, sizeof(completion));
        if (meiso_composition_control_session_take_completion(
                fixture->edge, 3U, &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            CHECK(completion.kind ==
                      MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE &&
                  completion.code ==
                      MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT);
            completion_seen = true;
        }
        if (completion_seen && inline_seen) {
            return true;
        }
    }
    return false;
}

static bool test_inline_duplicate_result(void)
{
    const MeisoOwnerScope edge_owner = {MEISO_OWNER_ROLE_EDGE, 41U};
    HostRuntimeFixture fixture;
    uint8_t sample_wire[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    MeisoWifiTelemetrySample sample_value;
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeRouteKey route;
    MeisoStatePublisher *publisher = NULL;
    void *publisher_arena = NULL;
    size_t sample_size = 0U;
    MeisoCompositionHostRuntimePumpResult first_result;
    MeisoCompositionHostRuntimePumpResult duplicate_result_a;
    MeisoCompositionHostRuntimePumpResult duplicate_result_b;
    MeisoCompositionHostRuntimeSnapshot first_snapshot;
    MeisoCompositionHostRuntimeSnapshot duplicate_snapshot;
    bool ok = false;

    CHECK(initialize_fixture(&fixture, UINT64_C(0x2000)));
    CHECK_GOTO(queue_single_wifi_desired(&fixture) &&
               drive_control_ready(&fixture) &&
               drive_single_wifi_desired(&fixture) &&
               queue_wifi_actual(
                   &fixture, &publisher, &publisher_arena
               ) && drive_wifi_actual(&fixture, publisher),
               cleanup);
    memset(&sample_value, 0, sizeof(sample_value));
    sample_value.selected_fields =
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
        MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM;
    sample_value.present_fields = sample_value.selected_fields;
    sample_value.link_state = true;
    sample_value.signal_mbm = -5100;
    CHECK_GOTO(meiso_wifi_telemetry_sample_encode(
                   &sample_value,
                   (MeisoMutableBuffer){sample_wire, sizeof(sample_wire)},
                   &sample_size
               ) == MEISO_WIFI_TELEMETRY_CODEC_OK &&
               meiso_composition_control_session_route_key(
                   fixture.edge, ROUTE_WIFI_DATA, &route
               ), cleanup);
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE;
    work.value.stream_sample.route = route;
    work.value.stream_sample.production_epoch = 77U;
    work.value.stream_sample.sample.key = (MeisoStreamKey){
        edge_owner, 77U, 1U
    };
    work.value.stream_sample.sample.source_time = (MeisoTimePoint){
        77U, 1000U
    };
    work.value.stream_sample.sample.payload = (MeisoByteView){
        sample_wire, sample_size
    };
    CHECK_GOTO(drive_inline_sample_once(
                   &fixture, &work, &first_result
               ) && meiso_composition_host_runtime_snapshot(
                   fixture.app, &first_snapshot
               ) && first_snapshot.inline_streams[
                   MEISO_COMPOSITION_HOST_RUNTIME_INLINE_WIFI
               ].received_count == 1U &&
               first_snapshot.wifi_sample_present,
               cleanup);
    CHECK_GOTO(drive_inline_sample_once(
                   &fixture, &work, &duplicate_result_a
               ) && meiso_composition_host_runtime_snapshot(
                   fixture.app, &duplicate_snapshot
               ) && memcmp(
                   &duplicate_snapshot, &first_snapshot,
                   sizeof(first_snapshot)
               ) == 0,
               cleanup);
    CHECK_GOTO(drive_inline_sample_once(
                   &fixture, &work, &duplicate_result_b
               ) && meiso_composition_host_runtime_snapshot(
                   fixture.app, &duplicate_snapshot
               ) && memcmp(
                   &duplicate_snapshot, &first_snapshot,
                   sizeof(first_snapshot)
               ) == 0 &&
               memcmp(
                   &duplicate_result_a, &duplicate_result_b,
                   sizeof(duplicate_result_a)
               ) == 0,
               cleanup);
    ok = true;

cleanup:
    free(publisher_arena);
    destroy_fixture(&fixture);
    return ok;
}

static bool test_blob_full_interleave(void)
{
    HostRuntimeFixture fixture;
    uint8_t cube[512];
    size_t cube_size = 0U;
    MeisoCompositionHostRuntimeCommandReceipt receipt;
    MeisoBlobStore *blob_store = NULL;
    void *blob_arena = NULL;
    size_t blob_bytes;
    struct timespec deadline;
    bool begin_seen = false;
    bool chunk_seen = false;
    bool commit_seen = false;
    bool host_terminal_seen = false;
    uint8_t result_completions = 0U;
    MeisoRuntimeBlobTransferRef transfer;
    bool ok = false;

    CHECK(initialize_fixture(&fixture, UINT64_C(0x3000)));
    CHECK_GOTO(meiso_composition_scene_mesh_encode(
                   meiso_composition_scene_canonical_cube(),
                   (MeisoMutableBuffer){cube, sizeof(cube)}, &cube_size
               ) == MEISO_COMPOSITION_SCENE_CODEC_OK &&
               cube_size == 304U &&
               meiso_composition_host_runtime_put_mesh(
                   fixture.app, (MeisoByteView){cube, cube_size}, &receipt
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
               receipt.command_id == 1U &&
               drive_control_ready(&fixture),
               cleanup);
    blob_bytes = meiso_blob_store_memory_required(
        &fixture.catalog.routes[ROUTE_MESH_BLOB].route.resource,
        &(MeisoBlobConfig){2U}
    );
    blob_arena = calloc(1U, blob_bytes);
    CHECK_GOTO(blob_bytes != 0U && blob_arena != NULL &&
               meiso_blob_store_init(
                   blob_arena, blob_bytes,
                   &fixture.catalog.routes[
                       ROUTE_MESH_BLOB
                   ].route.resource,
                   &(MeisoBlobConfig){2U}, &blob_store
               ) == MEISO_BLOB_INIT_OK &&
               make_deadline(&deadline),
               cleanup);

    while (remaining_ms(&deadline) > 0) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true, false, false
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionControlSessionPumpResult edge_result;
        MeisoRuntimeSchedulerCompletion completion;
        MeisoCompositionHostRuntimeSnapshot snapshot;

        memset(&host_result, 0, sizeof(host_result));
        CHECK_GOTO(host_pump_status_benign(
                       meiso_composition_host_runtime_pump(
                           fixture.app, &ready, &host_result
                       )
                   ), cleanup);
        memset(&edge_result, 0, sizeof(edge_result));
        CHECK_GOTO(edge_pump_status_benign(
                       meiso_composition_control_session_pump(
                           fixture.edge, true, true, &edge_result
                       )
                   ), cleanup);
        if (edge_result.event_present) {
            MeisoRuntimeSchedulerWork result_work;

            CHECK_GOTO(edge_result.event.type ==
                           MEISO_RUNTIME_EVENT_BLOB_REQUEST,
                       cleanup);
            memset(&result_work, 0, sizeof(result_work));
            if (edge_result.event.blob_operation ==
                MEISO_RUNTIME_BLOB_BEGIN) {
                CHECK_GOTO(!begin_seen && !chunk_seen && !commit_seen &&
                           edge_result.event.blob_transfer.total_size ==
                               cube_size,
                           cleanup);
                transfer = edge_result.event.blob_transfer;
                begin_seen = true;
                result_work.kind =
                    MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT;
                result_work.value.blob_result.transfer = transfer;
                result_work.value.blob_result.operation =
                    MEISO_RUNTIME_BLOB_BEGIN;
                CHECK_GOTO(meiso_composition_control_session_queue_work(
                               fixture.edge, 4U, &result_work
                           ) == MEISO_COMPOSITION_CONTROL_SESSION_OK,
                           cleanup);
            } else if (edge_result.event.blob_operation ==
                       MEISO_RUNTIME_BLOB_CHUNK) {
                CHECK_GOTO(begin_seen && !chunk_seen && !commit_seen &&
                           edge_result.event.blob_transfer.transfer_id ==
                               transfer.transfer_id &&
                           edge_result.event.blob_offset == 0U &&
                           edge_result.event.blob_payload.size == cube_size &&
                           memcmp(
                               edge_result.event.blob_payload.data,
                               cube, cube_size
                           ) == 0,
                           cleanup);
                chunk_seen = true;
            } else if (edge_result.event.blob_operation ==
                       MEISO_RUNTIME_BLOB_COMMIT) {
                MeisoRuntimeBlobPublishReport report;
                MeisoBlobView published;

                CHECK_GOTO(begin_seen && chunk_seen && !commit_seen &&
                           meiso_composition_control_session_publish_blob_receive(
                               fixture.edge,
                               &edge_result.event.blob_transfer,
                               blob_store,
                               meiso_composition_scene_mesh_blob_verify,
                               NULL, &report
                           ) == MEISO_COMPOSITION_CONTROL_SESSION_OK &&
                           report.result ==
                               MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
                           meiso_blob_store_count(blob_store) == 1U &&
                           meiso_blob_store_lookup(
                               blob_store, &receipt.mesh_content_id,
                               &published
                           ) && published.content.size == cube_size &&
                           memcmp(
                               published.content.data, cube, cube_size
                           ) == 0,
                           cleanup);
                commit_seen = true;
                result_work.kind =
                    MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT;
                result_work.value.blob_result.transfer = transfer;
                result_work.value.blob_result.operation =
                    MEISO_RUNTIME_BLOB_COMMIT;
                CHECK_GOTO(meiso_composition_control_session_queue_work(
                               fixture.edge, 4U, &result_work
                           ) == MEISO_COMPOSITION_CONTROL_SESSION_OK,
                           cleanup);
            } else {
                CHECK_GOTO(false, cleanup);
            }
        }
        memset(&completion, 0, sizeof(completion));
        if (meiso_composition_control_session_take_completion(
                fixture.edge, 4U, &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            CHECK_GOTO(completion.kind ==
                           MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT &&
                       completion.code ==
                           MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT,
                       cleanup);
            CHECK_GOTO(result_completions < 2U, cleanup);
            ++result_completions;
        }
        CHECK_GOTO(meiso_composition_host_runtime_snapshot(
                       fixture.app, &snapshot
                   ), cleanup);
        if (snapshot.mesh.terminal_result_present) {
            CHECK_GOTO(begin_seen && chunk_seen && commit_seen &&
                       snapshot.mesh.terminal_result ==
                           MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
                       snapshot.mesh.edge_core_published &&
                       !snapshot.mesh.queued &&
                       !snapshot.mesh.transfer_active &&
                       snapshot.mesh.begin_ready &&
                       !snapshot.mesh.waiting_result &&
                       snapshot.mesh.next_offset == cube_size &&
                       snapshot.mesh.transfer_id_high_water ==
                           transfer.transfer_id,
                       cleanup);
            host_terminal_seen = true;
        }
        if (host_terminal_seen && result_completions == 2U) {
            ok = true;
            break;
        }
    }

cleanup:
    free(blob_arena);
    destroy_fixture(&fixture);
    return ok;
}

static MeisoCompositionMediaDesired media_desired(
    MeisoCompositionMediaSource source
)
{
    if (source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) {
        return (MeisoCompositionMediaDesired){
            MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888,
            MEISO_COMPOSITION_MEDIA_CODEC_RAW,
            64U, 48U, 10000U, 0U, 1U,
            MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
        };
    }
    return (MeisoCompositionMediaDesired){
        MEISO_COMPOSITION_MEDIA_PIXEL_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        320U, 240U, 10000U, 0U, 1U,
        MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
    };
}

static bool queue_media_desired(
    HostRuntimeFixture *fixture,
    MeisoCompositionMediaSource source,
    uint64_t command_id
)
{
    const MeisoCompositionMediaDesired desired = media_desired(source);
    MeisoCompositionHostRuntimeCommandReceipt receipt;
    MeisoCompositionHostRuntimeStatus status;
    MeisoCompositionHostRuntimeDesiredChannel channel;

    if (source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) {
        status = meiso_composition_host_runtime_set_render_media(
            fixture->app, true, &desired, &receipt
        );
        channel = MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_RENDER_MEDIA;
    } else {
        status = meiso_composition_host_runtime_set_camera_media(
            fixture->app, true, &desired, &receipt
        );
        channel = MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_CAMERA_MEDIA;
    }
    return status == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
        receipt_is_desired(
            &receipt, command_id,
            source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW ?
                MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_RENDER_MEDIA :
                MEISO_COMPOSITION_HOST_RUNTIME_COMMAND_SET_CAMERA_MEDIA,
            channel, 1U
        );
}

static bool drive_media_desired(
    HostRuntimeFixture *fixture,
    MeisoCompositionMediaSource source
)
{
    const MeisoOwnerScope host_owner = {MEISO_OWNER_ROLE_HOST, 31U};
    const bool render =
        source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW;
    const uint8_t route_index = render ?
        ROUTE_RENDER_DESIRED : ROUTE_CAMERA_DESIRED;
    const uint8_t accept_source = render ? 13U : 15U;
    const MeisoCompositionHostRuntimeDesiredChannel channel = render ?
        MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_RENDER_MEDIA :
        MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_CAMERA_MEDIA;
    const MeisoCompositionMediaDesired expected = media_desired(source);
    struct timespec deadline;
    MeisoRuntimeRouteKey expected_route;
    MeisoStateReplica *replica = NULL;
    void *replica_arena = NULL;
    size_t replica_bytes;
    bool offer_seen = false;
    bool completion_seen = false;
    bool ok = false;

    replica_bytes = meiso_state_replica_memory_required(
        &fixture->catalog.routes[route_index].route.resource
    );
    replica_arena = calloc(1U, replica_bytes);
    CHECK(replica_bytes != 0U && replica_arena != NULL);
    CHECK_GOTO(meiso_state_replica_init(
                   replica_arena, replica_bytes,
                   &fixture->catalog.routes[route_index].route.resource,
                   &replica
               ) == MEISO_STATE_INIT_OK &&
               meiso_state_replica_begin_owner(replica, host_owner) ==
                   MEISO_STATE_OWNER_STARTED &&
               meiso_composition_control_session_route_key(
                   fixture->edge, route_index, &expected_route
               ) && make_deadline(&deadline),
               cleanup);
    while (remaining_ms(&deadline) > 0) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true, false, false
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionControlSessionPumpResult edge_result;
        MeisoRuntimeSchedulerCompletion completion;
        MeisoCompositionHostRuntimeSnapshot snapshot;

        memset(&host_result, 0, sizeof(host_result));
        CHECK_GOTO(host_pump_status_benign(
                       meiso_composition_host_runtime_pump(
                           fixture->app, &ready, &host_result
                       )
                   ), cleanup);
        memset(&edge_result, 0, sizeof(edge_result));
        CHECK_GOTO(edge_pump_status_benign(
                       meiso_composition_control_session_pump(
                           fixture->edge, true, true, &edge_result
                       )
                   ), cleanup);
        if (edge_result.event_present) {
            MeisoRuntimeDesiredState desired;
            MeisoCompositionMediaDesired decoded;
            MeisoStateReceiveStatus receive;
            MeisoRuntimeSchedulerWork accept;

            if (edge_result.event.type != MEISO_RUNTIME_EVENT_STATE_OFFER ||
                !route_equal(edge_result.event.route, expected_route)) {
                CHECK_GOTO(resolve_unrelated_desired_offer(
                               fixture->edge, &edge_result.event
                           ), cleanup);
                goto media_event_handled;
            }
            receive = meiso_state_replica_receive(
                replica, edge_result.event.state_offer.state.owner,
                edge_result.event.state_offer.state.version,
                edge_result.event.state_offer.state.payload
            );
            CHECK_GOTO(receive == (offer_seen ?
                           MEISO_STATE_RECEIVE_DUPLICATE :
                           MEISO_STATE_RECEIVE_NEW) &&
                       meiso_runtime_desired_state_decode(
                           edge_result.event.state_offer.state.payload,
                           &desired
                       ) && desired.enabled &&
                       meiso_composition_media_desired_decode(
                           desired.profile, &decoded
                       ) == MEISO_COMPOSITION_PROFILE_OK &&
                       memcmp(&decoded, &expected, sizeof(decoded)) == 0 &&
                       meiso_composition_control_session_commit_state_receive(
                           fixture->edge, edge_result.event.route,
                           &edge_result.event.state_offer, receive
                       ) == MEISO_COMPOSITION_CONTROL_SESSION_OK,
                       cleanup);
            if (!offer_seen) {
                memset(&accept, 0, sizeof(accept));
                accept.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
                accept.value.state_accept.route = edge_result.event.route;
                accept.value.state_accept.generation =
                    edge_result.event.state_offer.generation;
                accept.value.state_accept.owner =
                    edge_result.event.state_offer.state.owner;
                accept.value.state_accept.version =
                    edge_result.event.state_offer.state.version;
                CHECK_GOTO(meiso_composition_control_session_queue_work(
                               fixture->edge, accept_source, &accept
                           ) == MEISO_COMPOSITION_CONTROL_SESSION_OK,
                           cleanup);
                offer_seen = true;
            }
        }
media_event_handled:
        memset(&completion, 0, sizeof(completion));
        if (meiso_composition_control_session_take_completion(
                fixture->edge, accept_source, &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            CHECK_GOTO(completion.kind ==
                           MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT &&
                       completion.code ==
                           MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT,
                       cleanup);
            completion_seen = true;
        }
        CHECK_GOTO(meiso_composition_host_runtime_snapshot(
                       fixture->app, &snapshot
                   ), cleanup);
        if (completion_seen && snapshot.desired[channel].delivery_accepted) {
            CHECK_GOTO(offer_seen && completion_seen &&
                       snapshot.desired[channel].state_version == 1U,
                       cleanup);
            ok = true;
            break;
        }
    }

cleanup:
    free(replica_arena);
    return ok;
}

static bool deliver_media_actual(
    HostRuntimeFixture *fixture,
    MeisoCompositionMediaSource source,
    uint64_t desired_version,
    uint64_t actual_state_version,
    uint64_t production_epoch,
    uint64_t media_stream_epoch,
    MeisoSessionNonce nonce
)
{
    const MeisoOwnerScope edge_owner = {MEISO_OWNER_ROLE_EDGE, 41U};
    const MeisoOwnerScope host_owner = {MEISO_OWNER_ROLE_HOST, 31U};
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const bool render =
        source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW;
    const uint8_t route_index = render ?
        ROUTE_RENDER_ACTUAL : ROUTE_CAMERA_ACTUAL;
    const uint8_t offer_source = render ? 14U : 16U;
    const MeisoCompositionHostRuntimeActualChannel channel = render ?
        MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_RENDER_MEDIA :
        MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_CAMERA_MEDIA;
    uint8_t suffix[MEISO_COMPOSITION_MEDIA_ACTUAL_WIRE_SIZE];
    uint8_t wire[MEISO_COMPOSITION_MEDIA_ACTUAL_STATE_MAX_PAYLOAD_SIZE];
    MeisoCompositionMediaActual media;
    MeisoRuntimeActualState actual;
    MeisoByteView payload;
    MeisoStatePublisher *publisher = NULL;
    void *publisher_arena = NULL;
    size_t publisher_bytes;
    size_t suffix_size = 0U;
    MeisoStateOffer offer;
    MeisoRuntimeRouteKey route;
    MeisoRuntimeSchedulerWork work;
    struct timespec deadline;
    bool offer_complete = false;
    bool accept_seen = false;
    uint8_t actual_events = 0U;
    bool ok = false;

    memset(&media, 0, sizeof(media));
    media.source = source;
    media.effective = media_desired(source);
    media.media_stream_epoch = media_stream_epoch;
    media.channel_nonce = nonce;
    media.queue_depth = 2U;
    media.recovery_generation = 1U;
    CHECK(meiso_composition_media_actual_encode(
              &media, (MeisoMutableBuffer){suffix, sizeof(suffix)},
              &suffix_size
          ) == MEISO_COMPOSITION_PROFILE_OK);
    memset(&actual, 0, sizeof(actual));
    actual.observed_desired = (MeisoRuntimeDesiredReference){
        true, host_owner, desired_version
    };
    actual.applied_desired = actual.observed_desired;
    actual.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    actual.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    actual.effective_enabled = true;
    actual.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING;
    actual.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    actual.production_epoch = production_epoch;
    actual.profile = (MeisoByteView){suffix, suffix_size};
    CHECK(meiso_runtime_actual_state_encode(
        &actual, (MeisoMutableBuffer){wire, sizeof(wire)}, &payload
    ));
    publisher_bytes = meiso_state_publisher_memory_required(
        &fixture->catalog.routes[route_index].route.resource, &state_config
    );
    publisher_arena = calloc(1U, publisher_bytes);
    CHECK_GOTO(publisher_bytes != 0U && publisher_arena != NULL &&
               meiso_state_publisher_init(
                   publisher_arena, publisher_bytes,
                   &fixture->catalog.routes[route_index].route.resource,
                   &state_config, &publisher
               ) == MEISO_STATE_INIT_OK &&
               meiso_state_publisher_publish(
                   publisher, edge_owner, actual_state_version, payload
               ) == MEISO_STATE_PUBLISH_NEW &&
               meiso_state_publisher_begin_delivery(
                   publisher, (MeisoDeliveryGeneration){301U + channel}
               ) == MEISO_STATE_DELIVERY_STARTED &&
               meiso_state_publisher_next_offer(publisher, &offer) &&
               meiso_composition_control_session_route_key(
                   fixture->edge, route_index, &route
               ) && make_deadline(&deadline),
               cleanup);
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
    work.value.state_offer.route = route;
    work.value.state_offer.offer = offer;
    CHECK_GOTO(meiso_composition_control_session_queue_work(
                   fixture->edge, offer_source, &work
               ) == MEISO_COMPOSITION_CONTROL_SESSION_OK,
               cleanup);
    while (remaining_ms(&deadline) > 0) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true, false, false
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionControlSessionPumpResult edge_result;
        MeisoRuntimeSchedulerCompletion completion;
        MeisoCompositionHostRuntimeSnapshot snapshot;

        memset(&host_result, 0, sizeof(host_result));
        CHECK_GOTO(host_pump_status_benign(
                       meiso_composition_host_runtime_pump(
                           fixture->app, &ready, &host_result
                       )
                   ), cleanup);
        if (host_result.event_present && host_result.event ==
                MEISO_COMPOSITION_HOST_RUNTIME_PUMP_ACTUAL_UPDATED &&
            host_result.actual_channel_present &&
            host_result.actual_channel == channel) {
            CHECK_GOTO(actual_events < 3U, cleanup);
            ++actual_events;
        }
        memset(&edge_result, 0, sizeof(edge_result));
        CHECK_GOTO(edge_pump_status_benign(
                       meiso_composition_control_session_pump(
                           fixture->edge, true, true, &edge_result
                       )
                   ), cleanup);
        if (edge_result.event_present) {
            CHECK_GOTO(edge_result.event.type ==
                           MEISO_RUNTIME_EVENT_STATE_ACCEPT &&
                       meiso_state_publisher_mark_delivery_accepted(
                           publisher,
                           edge_result.event.state_accept_generation,
                           edge_result.event.state_accept_owner,
                           edge_result.event.state_accept_version
                       ) == MEISO_STATE_ACCEPT_CONFIRMED,
                       cleanup);
            accept_seen = true;
        }
        memset(&completion, 0, sizeof(completion));
        if (meiso_composition_control_session_take_completion(
                fixture->edge, offer_source, &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            CHECK_GOTO(completion.kind ==
                           MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER &&
                       completion.code ==
                           MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT,
                       cleanup);
            offer_complete = true;
        }
        CHECK_GOTO(meiso_composition_host_runtime_snapshot(
                       fixture->app, &snapshot
                   ), cleanup);
        if (accept_seen && offer_complete && snapshot.actual[channel].present) {
            CHECK_GOTO(actual_events == 3U &&
                       snapshot.actual[channel].production_epoch ==
                           production_epoch &&
                       snapshot.actual[channel].profile_decoded &&
                       snapshot.media[render ? 0U : 1U].actual_binding_ready,
                       cleanup);
            ok = true;
            break;
        }
    }

cleanup:
    free(publisher_arena);
    return ok;
}

typedef struct {
    int host_fd;
    int edge_fd;
    void *transport_arena;
    size_t transport_bytes;
    MeisoCompositionMediaTransport *transport;
    void *adapter_arena;
    size_t adapter_bytes;
    MeisoCompositionMediaPosixTcp *adapter;
    MeisoCompositionMediaTransportConfig config;
} EdgeMediaFixture;

static void destroy_edge_media(EdgeMediaFixture *media)
{
    if (media != NULL) {
        close_fd(&media->host_fd);
        close_fd(&media->edge_fd);
        free(media->transport_arena);
        free(media->adapter_arena);
        memset(media, 0, sizeof(*media));
        media->host_fd = -1;
        media->edge_fd = -1;
    }
}

static bool initialize_edge_media(
    HostRuntimeFixture *fixture,
    uint8_t mask,
    EdgeMediaFixture *media
)
{
    MeisoCompositionHostRuntimeSnapshot host;
    size_t alignment = 0U;
    size_t index;

    memset(media, 0, sizeof(*media));
    media->host_fd = -1;
    media->edge_fd = -1;
    CHECK(meiso_composition_host_runtime_snapshot(fixture->app, &host));
    memset(&media->config, 0, sizeof(media->config));
    media->config.local_role = MEISO_SESSION_ROLE_EDGE;
    media->config.active_route_mask = mask;
    media->config.scope.session_id = host.control_session_id;
    media->config.scope.catalog_generation = 1U;
    for (index = 0U; index < 2U; ++index) {
        const bool render = index == 0U;
        const uint8_t route_index = render ?
            ROUTE_RENDER_EXTERNAL : ROUTE_CAMERA_EXTERNAL;
        MeisoCompositionMediaRouteConfig *route =
            &media->config.routes[index];
        route->capability = render ?
            fixture->app_config.catalog.render_media_capability :
            fixture->app_config.catalog.camera_media_capability;
        if ((mask & (uint8_t)(UINT8_C(1) << index)) == 0U) {
            continue;
        }
        route->binding.session_id = host.control_session_id;
        route->binding.catalog_generation = 1U;
        route->binding.endpoint_id =
            fixture->catalog.routes[route_index].route.endpoint_id;
        route->binding.endpoint_incarnation =
            fixture->catalog.routes[route_index].route.endpoint_incarnation;
        route->binding.resource_id =
            fixture->catalog.routes[route_index].route.resource.id;
        route->binding.source = render ?
            MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW :
            MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
        route->binding.production_epoch = render ? 501U : 502U;
        route->binding.media_stream_epoch = render ? 601U : 602U;
        route->binding.channel_nonce = render ?
            (MeisoSessionNonce){701U, 711U} :
            (MeisoSessionNonce){702U, 712U};
        route->effective = media_desired(route->binding.source);
    }
    CHECK(meiso_composition_media_transport_memory_required(
              &media->config, &media->transport_bytes, &alignment
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK &&
          media->transport_bytes != 0U && alignment == alignof(max_align_t));
    media->transport_arena = calloc(1U, media->transport_bytes);
    CHECK(media->transport_arena != NULL &&
          meiso_composition_media_transport_init(
              &media->config,
              (MeisoMutableBuffer){
                  media->transport_arena, media->transport_bytes
              }, &media->transport
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK &&
          make_tcp_pair(&media->host_fd, &media->edge_fd));
    media->adapter_bytes =
        meiso_composition_media_posix_tcp_memory_required(
            &(MeisoCompositionMediaPosixTcpConfig){
                media->edge_fd, media->transport,
                media->config.scope, 17U
            }
        );
    media->adapter_arena = calloc(1U, media->adapter_bytes);
    CHECK(media->adapter_bytes != 0U && media->adapter_arena != NULL &&
          meiso_composition_media_posix_tcp_init(
              media->adapter_arena, media->adapter_bytes,
              &(MeisoCompositionMediaPosixTcpConfig){
                  media->edge_fd, media->transport,
                  media->config.scope, 17U
              }, &media->adapter
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_OK);
    return true;
}

static bool drive_media_active(
    HostRuntimeFixture *fixture,
    EdgeMediaFixture *media,
    uint8_t mask
)
{
    struct timespec deadline;

    CHECK(meiso_composition_host_runtime_bind_media_fd(
              fixture->app, media->host_fd, mask
          ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
          make_deadline(&deadline));
    while (remaining_ms(&deadline) > 0) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true,
            fd_ready_now(media->host_fd, POLLIN),
            fd_ready_now(media->host_fd, POLLOUT)
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionMediaPosixTcpPrepareStatus prepare;
        MeisoCompositionHostRuntimeSnapshot host;
        MeisoCompositionMediaTransportSnapshot edge;

        prepare = meiso_composition_media_posix_tcp_prepare_outbound(
            media->adapter
        );
        CHECK(prepare == MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_READY ||
              prepare == MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_IDLE);
        if (meiso_composition_media_posix_tcp_wants_write(media->adapter)) {
            const MeisoCompositionMediaPosixTcpWriteStatus write_status =
                meiso_composition_media_posix_tcp_on_writable(media->adapter);
            CHECK(write_status ==
                      MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PROGRESS ||
                  write_status ==
                      MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PACKET_SENT ||
                  write_status ==
                      MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_WOULD_BLOCK);
        }
        if (meiso_composition_media_posix_tcp_wants_read(media->adapter)) {
            const MeisoCompositionMediaPosixTcpReadStatus read_status =
                meiso_composition_media_posix_tcp_on_readable(media->adapter);
            CHECK(read_status <=
                      MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_REASSEMBLY_REPLACED ||
                  read_status ==
                      MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_WOULD_BLOCK);
        }
        memset(&host_result, 0, sizeof(host_result));
        CHECK(host_pump_status_benign(meiso_composition_host_runtime_pump(
            fixture->app, &ready, &host_result
        )));
        if (!meiso_composition_host_runtime_snapshot(fixture->app, &host)) {
            (void)fprintf(stderr, "media host snapshot failed\n");
            return false;
        }
        if (meiso_composition_media_transport_snapshot(
                media->transport, &edge
            ) != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
            (void)fprintf(stderr, "media edge snapshot failed\n");
            return false;
        }
        if (host.media_bound && edge.phase ==
                MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE) {
            return host.media_active_route_mask == mask &&
                edge.configured_active_route_mask == mask &&
                edge.bound_route_mask == mask;
        }
    }
    {
        MeisoCompositionHostRuntimeSnapshot host;
        MeisoCompositionMediaTransportSnapshot edge;
        MeisoCompositionMediaPosixTcpSnapshot adapter;
        (void)meiso_composition_host_runtime_snapshot(fixture->app, &host);
        (void)meiso_composition_media_transport_snapshot(
            media->transport, &edge
        );
        (void)meiso_composition_media_posix_tcp_snapshot(
            media->adapter, &adapter
        );
        (void)fprintf(
            stderr,
            "media active timeout: host_bound=%u mask=%u edge_phase=%u "
            "edge_bound=%u edge_out=%u adapter_terminal=%u rx=%zu/%zu "
            "tx=%zu/%zu\n",
            host.media_bound ? 1U : 0U,
            (unsigned)host.media_active_route_mask,
            (unsigned)edge.phase, (unsigned)edge.bound_route_mask,
            edge.outbound_packet_pending ? 1U : 0U,
            adapter.terminal ? 1U : 0U,
            adapter.rx_bytes, adapter.rx_expected_bytes,
            adapter.tx_bytes_sent, adapter.tx_packet_bytes
        );
    }
    return false;
}

static bool drive_render_frame(
    HostRuntimeFixture *fixture,
    EdgeMediaFixture *media
)
{
    const MeisoOwnerScope host_owner = {MEISO_OWNER_ROLE_HOST, 31U};
    const MeisoCompositionMediaDesired desired = media_desired(
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW
    );
    const size_t payload_size = 64U * 48U * 4U;
    uint8_t *payload = NULL;
    void *replica_arena = NULL;
    MeisoStateReplica *replica = NULL;
    size_t replica_bytes;
    MeisoCompositionMediaUnit unit;
    MeisoRuntimeRouteKey desired_route;
    MeisoCompositionHostRuntimeCommandReceipt receipt;
    struct timespec deadline;
    uint64_t desired_version = 2U;
    uint8_t completed_desired_versions = 0U;
    bool desired_offer_seen = false;
    bool desired_completion_seen = false;
    bool frame_seen = false;
    bool ok = false;

    payload = malloc(payload_size);
    CHECK(payload != NULL);
    replica_bytes = meiso_state_replica_memory_required(
        &fixture->catalog.routes[ROUTE_RENDER_DESIRED].route.resource
    );
    replica_arena = calloc(1U, replica_bytes);
    CHECK_GOTO(replica_bytes != 0U && replica_arena != NULL &&
               meiso_state_replica_init(
                   replica_arena, replica_bytes,
                   &fixture->catalog.routes[
                       ROUTE_RENDER_DESIRED
                   ].route.resource,
                   &replica
               ) == MEISO_STATE_INIT_OK &&
               meiso_state_replica_begin_owner(replica, host_owner) ==
                   MEISO_STATE_OWNER_STARTED &&
               meiso_composition_control_session_route_key(
                   fixture->edge, ROUTE_RENDER_DESIRED, &desired_route
               ) &&
               meiso_composition_host_runtime_set_render_media(
                   fixture->app, true, &desired, &receipt
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
               receipt.desired_state_version == desired_version,
               cleanup);
    for (size_t index = 0U; index < payload_size; ++index) {
        payload[index] = (uint8_t)(index * 17U + 3U);
    }
    memset(&unit, 0, sizeof(unit));
    unit.binding = media->config.routes[0].binding;
    unit.frame_sequence = 1U;
    unit.source_time = (MeisoTimePoint){9U, 1000U};
    unit.applied_scene_owner = (MeisoOwnerScope){
        MEISO_OWNER_ROLE_HOST, 31U
    };
    unit.applied_scene_state_version = 1U;
    unit.active_plan_generation = 1U;
    unit.world_generation = 1U;
    unit.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888;
    unit.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
    unit.width = 64U;
    unit.height = 48U;
    unit.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    unit.access_unit = (MeisoByteView){payload, payload_size};
    CHECK_GOTO(meiso_composition_media_transport_submit(
                   media->transport, &unit
               ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK &&
               make_deadline(&deadline),
               cleanup);
    while (remaining_ms(&deadline) > 0) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true,
            fd_ready_now(media->host_fd, POLLIN),
            fd_ready_now(media->host_fd, POLLOUT)
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionControlSessionPumpResult edge_result;
        MeisoRuntimeSchedulerCompletion completion;
        MeisoCompositionHostRuntimeSnapshot snapshot;

        CHECK_GOTO(meiso_composition_media_posix_tcp_prepare_outbound(
                       media->adapter
                   ) <= MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_IDLE,
                   cleanup);
        if (meiso_composition_media_posix_tcp_wants_write(media->adapter)) {
            const MeisoCompositionMediaPosixTcpWriteStatus status =
                meiso_composition_media_posix_tcp_on_writable(media->adapter);
            CHECK_GOTO(status ==
                           MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PROGRESS ||
                       status ==
                           MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PACKET_SENT ||
                       status ==
                           MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_WOULD_BLOCK,
                       cleanup);
        }
        memset(&host_result, 0, sizeof(host_result));
        CHECK_GOTO(host_pump_status_benign(
                       meiso_composition_host_runtime_pump(
                           fixture->app, &ready, &host_result
                       )
                   ) && meiso_composition_host_runtime_snapshot(
                       fixture->app, &snapshot
                   ), cleanup);
        memset(&edge_result, 0, sizeof(edge_result));
        CHECK_GOTO(edge_pump_status_benign(
                       meiso_composition_control_session_pump(
                           fixture->edge, true, true, &edge_result
                       )
                   ), cleanup);
        if (edge_result.event_present) {
            MeisoRuntimeDesiredState wrapper;
            MeisoCompositionMediaDesired decoded;
            MeisoStateReceiveStatus receive;
            MeisoRuntimeSchedulerWork accept;

            CHECK_GOTO(!desired_offer_seen && edge_result.event.type ==
                           MEISO_RUNTIME_EVENT_STATE_OFFER &&
                       route_equal(edge_result.event.route, desired_route) &&
                       edge_result.event.state_offer.state.version ==
                           desired_version,
                       cleanup);
            receive = meiso_state_replica_receive(
                replica, edge_result.event.state_offer.state.owner,
                edge_result.event.state_offer.state.version,
                edge_result.event.state_offer.state.payload
            );
            CHECK_GOTO(receive == MEISO_STATE_RECEIVE_NEW &&
                       meiso_runtime_desired_state_decode(
                           edge_result.event.state_offer.state.payload,
                           &wrapper
                       ) && wrapper.enabled &&
                       meiso_composition_media_desired_decode(
                           wrapper.profile, &decoded
                       ) == MEISO_COMPOSITION_PROFILE_OK &&
                       memcmp(&decoded, &desired, sizeof(decoded)) == 0 &&
                       meiso_composition_control_session_commit_state_receive(
                           fixture->edge, edge_result.event.route,
                           &edge_result.event.state_offer, receive
                       ) == MEISO_COMPOSITION_CONTROL_SESSION_OK,
                       cleanup);
            memset(&accept, 0, sizeof(accept));
            accept.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
            accept.value.state_accept.route = edge_result.event.route;
            accept.value.state_accept.generation =
                edge_result.event.state_offer.generation;
            accept.value.state_accept.owner =
                edge_result.event.state_offer.state.owner;
            accept.value.state_accept.version = desired_version;
            CHECK_GOTO(meiso_composition_control_session_queue_work(
                           fixture->edge, 13U, &accept
                       ) == MEISO_COMPOSITION_CONTROL_SESSION_OK,
                       cleanup);
            desired_offer_seen = true;
        }
        memset(&completion, 0, sizeof(completion));
        if (meiso_composition_control_session_take_completion(
                fixture->edge, 13U, &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            CHECK_GOTO(desired_offer_seen && completion.kind ==
                           MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT &&
                       completion.code ==
                           MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT,
                       cleanup);
            desired_completion_seen = true;
        }
        frame_seen = snapshot.media[0].latest_present;
        if (desired_completion_seen && snapshot.desired[
                MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_RENDER_MEDIA
            ].delivery_accepted && snapshot.desired[
                MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_RENDER_MEDIA
            ].state_version == desired_version) {
            completed_desired_versions = 1U;
        }
        if (frame_seen && desired_completion_seen && snapshot.desired[
                MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_RENDER_MEDIA
            ].delivery_accepted) {
            MeisoCompositionMediaLatestView latest;
            MeisoCompositionHostRuntimeStatus latest_status;
            if (snapshot.media[0].latest_frame_sequence != 1U ||
                snapshot.media[0].latest_access_unit_bytes != payload_size) {
                (void)fprintf(
                    stderr,
                    "media snapshot seq=%llu bytes=%u expected=%zu\n",
                    (unsigned long long)
                        snapshot.media[0].latest_frame_sequence,
                    snapshot.media[0].latest_access_unit_bytes,
                    payload_size
                );
            }
            CHECK_GOTO(snapshot.media[0].latest_frame_sequence == 1U &&
                       snapshot.media[0].latest_access_unit_bytes ==
                           payload_size,
                       cleanup);
            latest_status = meiso_composition_host_runtime_media_latest(
                fixture->app,
                MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
                &latest
            );
            if (latest_status != MEISO_COMPOSITION_HOST_RUNTIME_OK) {
                (void)fprintf(stderr, "media latest status=%u\n",
                              (unsigned)latest_status);
            }
            CHECK_GOTO(latest_status == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
                       latest.unit.access_unit.size == payload_size &&
                       memcmp(
                           latest.unit.access_unit.data, payload, payload_size
                       ) == 0,
                       cleanup);
            CHECK_GOTO(meiso_composition_host_runtime_media_release(
                           fixture->app, &latest.ref
                       ) == MEISO_COMPOSITION_HOST_RUNTIME_OK,
                       cleanup);
            CHECK_GOTO(completed_desired_versions >= 1U, cleanup);
            ok = true;
            break;
        }
    }

cleanup:
    free(replica_arena);
    free(payload);
    return ok;
}

static bool drive_camera_frame(
    HostRuntimeFixture *fixture,
    EdgeMediaFixture *media,
    MeisoEdgeHandle *edge_handle,
    uint64_t continuity_serial
)
{
    const size_t payload_size = 320U * 240U * 3U / 2U;
    uint8_t *payload = NULL;
    MeisoCompositionMediaUnit unit;
    struct timespec deadline;
    bool ok = false;

    payload = malloc(payload_size);
    CHECK(payload != NULL);
    for (size_t index = 0U; index < payload_size; ++index) {
        payload[index] = (uint8_t)(index * 11U + 5U);
    }
    memset(&unit, 0, sizeof(unit));
    unit.binding = media->config.routes[1].binding;
    unit.frame_sequence = 1U;
    unit.source_time = (MeisoTimePoint){10U, 2000U};
    unit.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_NV12;
    unit.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
    unit.width = 320U;
    unit.height = 240U;
    unit.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
    unit.access_unit = (MeisoByteView){payload, payload_size};
    CHECK_GOTO(meiso_composition_media_transport_submit(
                   media->transport, &unit
               ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK &&
               make_deadline(&deadline),
               cleanup);
    while (remaining_ms(&deadline) > 0) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true,
            fd_ready_now(media->host_fd, POLLIN),
            fd_ready_now(media->host_fd, POLLOUT)
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionHostRuntimeSnapshot snapshot;

        CHECK_GOTO(meiso_composition_media_posix_tcp_prepare_outbound(
                       media->adapter
                   ) <= MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_IDLE,
                   cleanup);
        if (meiso_composition_media_posix_tcp_wants_write(media->adapter)) {
            const MeisoCompositionMediaPosixTcpWriteStatus status =
                meiso_composition_media_posix_tcp_on_writable(media->adapter);
            CHECK_GOTO(status ==
                           MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PROGRESS ||
                       status ==
                           MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PACKET_SENT ||
                       status ==
                           MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_WOULD_BLOCK,
                       cleanup);
        }
        memset(&host_result, 0, sizeof(host_result));
        CHECK_GOTO(host_pump_status_benign(
                       meiso_composition_host_runtime_pump(
                           fixture->app, &ready, &host_result
                       )
                   ) && meiso_composition_host_runtime_snapshot(
                       fixture->app, &snapshot
                   ), cleanup);
        if (snapshot.media[1].latest_present) {
            CHECK_GOTO(snapshot.media[1].latest_frame_sequence == 1U &&
                       snapshot.media[1].latest_access_unit_bytes ==
                           payload_size &&
                       meiso_test_composition_edge_camera_frame(
                           edge_handle, continuity_serial, payload, payload_size
                       ),
                       cleanup);
            ok = true;
            break;
        }
    }

cleanup:
    free(payload);
    return ok;
}

static bool test_render_media_smoke(void)
{
    HostRuntimeFixture fixture;
    EdgeMediaFixture media;
    uint8_t *arena_before = NULL;
    bool ok = false;

    CHECK(initialize_fixture(&fixture, UINT64_C(0x4000)));
    memset(&media, 0, sizeof(media));
    media.host_fd = -1;
    media.edge_fd = -1;
    CHECK_GOTO(queue_media_desired(
                   &fixture,
                   MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW, 1U
               ) && drive_control_ready(&fixture) &&
               drive_media_desired(
                   &fixture,
                   MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW
               ) && deliver_media_actual(
                   &fixture,
                   MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
                   1U, 1U,
                   501U, 601U, (MeisoSessionNonce){701U, 711U}
               ) && initialize_edge_media(
                   &fixture, MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_RENDER, &media
               ), cleanup);
    arena_before = malloc(fixture.app_bytes);
    CHECK_GOTO(arena_before != NULL, cleanup);
    memcpy(arena_before, fixture.app_arena, fixture.app_bytes);
    CHECK_GOTO(meiso_composition_host_runtime_bind_media_fd(
                   fixture.app, media.host_fd,
                   MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_CAMERA
               ) == MEISO_COMPOSITION_HOST_RUNTIME_INVALID_STATE,
               cleanup);
    CHECK_GOTO(memcmp(
                   arena_before, fixture.app_arena, fixture.app_bytes
               ) == 0,
               cleanup);
    CHECK_GOTO(drive_media_active(
                   &fixture, &media,
                   MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_RENDER
               ), cleanup);
    CHECK_GOTO(drive_render_frame(&fixture, &media), cleanup);
    CHECK_GOTO(meiso_composition_host_runtime_unbind_media_fd(
                   fixture.app, 0
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK,
               cleanup);
    CHECK_GOTO(deliver_media_actual(
                   &fixture,
                   MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
                   2U, 2U,
                   502U, 602U, (MeisoSessionNonce){702U, 712U}
               ), cleanup);
    ok = true;

cleanup:
    free(arena_before);
    destroy_edge_media(&media);
    destroy_fixture(&fixture);
    return ok;
}

static bool test_camera_and_both_media_masks(void)
{
    const uint8_t masks[2] = {
        MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_CAMERA,
        MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ALL
    };

    for (size_t case_index = 0U; case_index < 2U; ++case_index) {
        const uint8_t mask = masks[case_index];
        HostRuntimeFixture fixture;
        EdgeMediaFixture media;
        MeisoCompositionHostRuntimeSnapshot snapshot;
        MeisoEdgeHandle *edge_handle = NULL;
        void *edge_handle_arena = NULL;
        const uint64_t continuity_serial =
            UINT64_C(9100) + (uint64_t)case_index;
        uint64_t command_id = 1U;
        bool fixture_initialized = false;
        bool media_initialized = false;
        bool case_ok = false;

        memset(&media, 0, sizeof(media));
        media.host_fd = -1;
        media.edge_fd = -1;
        CHECK(initialize_fixture(
            &fixture, UINT64_C(0x5000) + (uint64_t)case_index
        ));
        fixture_initialized = true;
        CHECK_GOTO(initialize_remote_edge(
                       &fixture, continuity_serial, &edge_handle_arena,
                       &edge_handle
                   ), case_cleanup);
        CHECK_GOTO(drive_control_ready(&fixture), case_cleanup);
        if ((mask & MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_RENDER) != 0U) {
            CHECK_GOTO(queue_media_desired(
                           &fixture,
                           MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
                           command_id++
                       ) && drive_media_desired(
                           &fixture,
                           MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW
                       ), case_cleanup);
        }
        if ((mask & MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_CAMERA) != 0U) {
            CHECK_GOTO(queue_media_desired(
                           &fixture,
                           MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
                           command_id++
                       ) && drive_media_desired(
                           &fixture,
                           MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA
                       ), case_cleanup);
        }
        if ((mask & MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_RENDER) != 0U) {
            CHECK_GOTO(deliver_media_actual(
                           &fixture,
                           MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
                           1U, 1U,
                           501U, 601U,
                           (MeisoSessionNonce){701U, 711U}
                       ), case_cleanup);
        }
        if ((mask & MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_CAMERA) != 0U) {
            CHECK_GOTO(deliver_media_actual(
                           &fixture,
                           MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
                           1U, 1U,
                           502U, 602U,
                           (MeisoSessionNonce){702U, 712U}
                       ), case_cleanup);
        }
        CHECK_GOTO(initialize_edge_media(&fixture, mask, &media),
                   case_cleanup);
        media_initialized = true;
        CHECK_GOTO(drive_media_active(&fixture, &media, mask) &&
                   meiso_composition_host_runtime_snapshot(
                       fixture.app, &snapshot
                   ) && snapshot.media_bound &&
                   snapshot.media_active_route_mask == mask &&
                   snapshot.media[0].source ==
                       MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW &&
                   snapshot.media[1].source ==
                       MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA &&
                   snapshot.media[0].actual_binding_ready ==
                       ((mask &
                         MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_RENDER) != 0U) &&
                   snapshot.media[1].actual_binding_ready ==
                       ((mask &
                         MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_CAMERA) != 0U) &&
                   (mask != MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_CAMERA ||
                    drive_camera_frame(
                        &fixture, &media, edge_handle, continuity_serial
                    )) &&
                   meiso_composition_host_runtime_unbind_media_fd(
                       fixture.app, 0
                   ) == MEISO_COMPOSITION_HOST_RUNTIME_OK,
                   case_cleanup);
        case_ok = true;

case_cleanup:
        if (media_initialized) {
            destroy_edge_media(&media);
        }
        free(edge_handle_arena);
        if (fixture_initialized) {
            destroy_fixture(&fixture);
        }
        CHECK(case_ok);
    }
    return true;
}

typedef struct {
    void *arena;
    size_t arena_bytes;
    MeisoStatePublisher *publisher;
    bool delivery_started;
    MeisoRuntimeRouteKey route;
} WifiActualHarness;

static void destroy_wifi_actual_harness(WifiActualHarness *harness)
{
    if (harness != NULL) {
        free(harness->arena);
        memset(harness, 0, sizeof(*harness));
    }
}

static bool initialize_wifi_actual_harness(
    HostRuntimeFixture *fixture,
    WifiActualHarness *harness
)
{
    const MeisoStateConfig config = {MEISO_STATE_DELIVERY_CONVERGENT};

    CHECK(fixture != NULL && harness != NULL);
    memset(harness, 0, sizeof(*harness));
    harness->arena_bytes = meiso_state_publisher_memory_required(
        &fixture->catalog.routes[ROUTE_WIFI_ACTUAL].route.resource, &config
    );
    harness->arena = calloc(1U, harness->arena_bytes);
    return harness->arena_bytes != 0U && harness->arena != NULL &&
        meiso_state_publisher_init(
            harness->arena, harness->arena_bytes,
            &fixture->catalog.routes[ROUTE_WIFI_ACTUAL].route.resource,
            &config, &harness->publisher
        ) == MEISO_STATE_INIT_OK &&
        meiso_composition_control_session_route_key(
            fixture->edge, ROUTE_WIFI_ACTUAL, &harness->route
        );
}

static bool queue_wifi_actual_for_desired(
    HostRuntimeFixture *fixture,
    WifiActualHarness *harness,
    uint64_t desired_version,
    uint64_t actual_version,
    uint64_t production_epoch,
    const MeisoWifiTelemetryEffectiveParameters *effective
)
{
    const MeisoOwnerScope edge_owner = {MEISO_OWNER_ROLE_EDGE, 41U};
    const MeisoOwnerScope host_owner = {MEISO_OWNER_ROLE_HOST, 31U};
    uint8_t suffix[MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE];
    uint8_t wire[MEISO_WIFI_TELEMETRY_ACTUAL_STATE_MAX_PAYLOAD_SIZE];
    MeisoRuntimeActualState actual;
    MeisoByteView payload;
    MeisoStateOffer offer;
    MeisoRuntimeSchedulerWork work;
    size_t suffix_size = 0U;

    CHECK(fixture != NULL && harness != NULL && harness->publisher != NULL &&
          effective != NULL && desired_version != 0U &&
          actual_version != 0U && production_epoch != 0U);
    CHECK(meiso_wifi_telemetry_effective_parameters_encode(
              effective, (MeisoMutableBuffer){suffix, sizeof(suffix)},
              &suffix_size
          ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
    memset(&actual, 0, sizeof(actual));
    actual.observed_desired = (MeisoRuntimeDesiredReference){
        true, host_owner, desired_version
    };
    actual.applied_desired = actual.observed_desired;
    actual.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    actual.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    actual.effective_enabled = true;
    actual.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING;
    actual.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    actual.production_epoch = production_epoch;
    actual.profile = (MeisoByteView){suffix, suffix_size};
    CHECK(meiso_runtime_actual_state_encode(
              &actual, (MeisoMutableBuffer){wire, sizeof(wire)}, &payload
          ) && meiso_state_publisher_publish(
              harness->publisher, edge_owner, actual_version, payload
          ) == MEISO_STATE_PUBLISH_NEW);
    if (!harness->delivery_started) {
        CHECK(meiso_state_publisher_begin_delivery(
                  harness->publisher, (MeisoDeliveryGeneration){201U}
              ) == MEISO_STATE_DELIVERY_STARTED);
        harness->delivery_started = true;
    }
    CHECK(meiso_state_publisher_next_offer(harness->publisher, &offer));
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
    work.value.state_offer.route = harness->route;
    work.value.state_offer.offer = offer;
    return meiso_composition_control_session_queue_work(
               fixture->edge, 2U, &work
           ) == MEISO_COMPOSITION_CONTROL_SESSION_OK;
}

static bool decode_wifi_desired_exact(
    const MeisoStateOffer *offer,
    uint64_t version,
    const MeisoWifiTelemetryDesiredParameters *expected
)
{
    MeisoRuntimeDesiredState desired;
    MeisoWifiTelemetryDesiredParameters decoded;

    return offer != NULL && expected != NULL &&
        offer->state.version == version &&
        meiso_runtime_desired_state_decode(offer->state.payload, &desired) &&
        desired.enabled &&
        meiso_wifi_telemetry_desired_parameters_decode(
            desired.profile, &decoded
        ) == MEISO_WIFI_TELEMETRY_CODEC_OK &&
        decoded.selected_fields == expected->selected_fields &&
        decoded.sample_period_ms == expected->sample_period_ms;
}

static bool queue_state_accept(
    MeisoCompositionControlSession *edge,
    uint8_t source_index,
    const MeisoRuntimeEvent *event
)
{
    MeisoRuntimeSchedulerWork accept;

    CHECK(edge != NULL && event != NULL &&
          event->type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    memset(&accept, 0, sizeof(accept));
    accept.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
    accept.value.state_accept.route = event->route;
    accept.value.state_accept.generation = event->state_offer.generation;
    accept.value.state_accept.owner = event->state_offer.state.owner;
    accept.value.state_accept.version = event->state_offer.state.version;
    return meiso_composition_control_session_queue_work(
               edge, source_index, &accept
           ) == MEISO_COMPOSITION_CONTROL_SESSION_OK;
}

typedef struct {
    void *arena;
    MeisoStatePublisher *publisher;
    MeisoRuntimeRouteKey route;
    bool delivery_started;
} PresentationActualHarness;

static void destroy_presentation_actual_harness(
    PresentationActualHarness *harness
)
{
    if (harness != NULL) {
        free(harness->arena);
        memset(harness, 0, sizeof(*harness));
    }
}

static bool initialize_presentation_actual_harness(
    HostRuntimeFixture *fixture,
    PresentationActualHarness *harness
)
{
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    size_t required;

    CHECK(fixture != NULL && harness != NULL);
    memset(harness, 0, sizeof(*harness));
    required = meiso_state_publisher_memory_required(
        &fixture->catalog.routes[ROUTE_PRESENTATION_ACTUAL].route.resource,
        &state_config
    );
    harness->arena = calloc(1U, required);
    CHECK(required != 0U && harness->arena != NULL &&
          meiso_state_publisher_init(
              harness->arena, required,
              &fixture->catalog.routes[
                  ROUTE_PRESENTATION_ACTUAL
              ].route.resource,
              &state_config, &harness->publisher
          ) == MEISO_STATE_INIT_OK &&
          meiso_composition_control_session_route_key(
              fixture->edge, ROUTE_PRESENTATION_ACTUAL, &harness->route
          ));
    return true;
}

static bool decode_presentation_desired(
    const MeisoStateOffer *offer,
    uint64_t version,
    MeisoCompositionScenePresentationPlan *out_plan
)
{
    MeisoRuntimeDesiredState desired;

    return offer != NULL && out_plan != NULL &&
        offer->state.version == version &&
        meiso_runtime_desired_state_decode(offer->state.payload, &desired) &&
        desired.enabled &&
        meiso_composition_scene_presentation_plan_decode(
            desired.profile, out_plan
        ) == MEISO_COMPOSITION_SCENE_CODEC_OK;
}

static bool queue_presentation_actual(
    HostRuntimeFixture *fixture,
    PresentationActualHarness *harness,
    const MeisoCompositionScenePresentationPlan *plan,
    const uint8_t token[MEISO_SHA256_DIGEST_SIZE],
    uint64_t desired_version,
    uint64_t actual_version,
    MeisoCompositionSceneCandidateState candidate_state
)
{
    const MeisoOwnerScope edge_owner = {MEISO_OWNER_ROLE_EDGE, 41U};
    const MeisoOwnerScope host_owner = {MEISO_OWNER_ROLE_HOST, 31U};
    uint8_t suffix[MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE];
    uint8_t wire[
        MEISO_COMPOSITION_SCENE_PRESENTATION_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoCompositionScenePresentationStatus status;
    MeisoRuntimeActualState actual;
    MeisoByteView payload;
    MeisoStateOffer offer;
    MeisoRuntimeSchedulerWork work;
    size_t suffix_size = 0U;
    MeisoStatePublishStatus publish;

    CHECK(fixture != NULL && harness != NULL && plan != NULL &&
          token != NULL && harness->publisher != NULL &&
          (candidate_state == MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED ||
           candidate_state ==
               MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING));
    memset(&status, 0, sizeof(status));
    status.capability_generation = plan->capability_generation;
    status.candidate_state = candidate_state;
    status.candidate_plan_generation = plan->plan_generation;
    memcpy(status.candidate_digest, plan->candidate_digest,
           sizeof(status.candidate_digest));
    status.active_state = MEISO_COMPOSITION_SCENE_ACTIVE_NONE;
    status.reason = MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE;
    status.prepared_token_present = true;
    memcpy(status.prepared_token, token, sizeof(status.prepared_token));
    status.scene_binding = plan->scene_binding;
    status.target_width = plan->target_width;
    status.target_height = plan->target_height;
    status.color_format = plan->color_format;
    status.depth_format = plan->depth_format;
    CHECK(meiso_composition_scene_presentation_status_encode(
              &status, (MeisoMutableBuffer){suffix, sizeof(suffix)},
              &suffix_size
          ) == MEISO_COMPOSITION_SCENE_CODEC_OK);
    memset(&actual, 0, sizeof(actual));
    actual.observed_desired = (MeisoRuntimeDesiredReference){
        true, host_owner, desired_version
    };
    actual.applied_desired = actual.observed_desired;
    actual.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    actual.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    actual.effective_enabled = false;
    actual.production_status = MEISO_RUNTIME_PRODUCTION_STARTING;
    actual.reason = MEISO_RUNTIME_PRODUCTION_REASON_APPLYING;
    actual.production_epoch = 0U;
    actual.profile = (MeisoByteView){suffix, suffix_size};
    CHECK(meiso_runtime_actual_state_encode(
              &actual, (MeisoMutableBuffer){wire, sizeof(wire)}, &payload
          ));
    publish = meiso_state_publisher_publish(
        harness->publisher, edge_owner, actual_version, payload
    );
    CHECK(publish == MEISO_STATE_PUBLISH_NEW);
    if (!harness->delivery_started) {
        CHECK(meiso_state_publisher_begin_delivery(
                  harness->publisher, (MeisoDeliveryGeneration){301U}
              ) == MEISO_STATE_DELIVERY_STARTED);
        harness->delivery_started = true;
    }
    CHECK(meiso_state_publisher_next_offer(harness->publisher, &offer));
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
    work.value.state_offer.route = harness->route;
    work.value.state_offer.offer = offer;
    return meiso_composition_control_session_queue_work(
               fixture->edge, 6U, &work
           ) == MEISO_COMPOSITION_CONTROL_SESSION_OK;
}

static bool test_wifi_inflight_barrier_and_coalesce(void)
{
    const MeisoOwnerScope host_owner = {MEISO_OWNER_ROLE_HOST, 31U};
    const MeisoWifiTelemetryDesiredParameters values[4] = {
        {MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE, 500U},
        {MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
             MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM, 600U},
        {MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE, 700U},
        {MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
             MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM, 800U}
    };
    HostRuntimeFixture fixture;
    WifiActualHarness actual;
    MeisoStateReplica *desired_replica = NULL;
    void *desired_replica_arena = NULL;
    size_t desired_replica_bytes;
    MeisoCompositionHostRuntimeCommandReceipt receipts[4];
    MeisoRuntimeRouteKey desired_route;
    MeisoRuntimeEvent borrowed_v2;
    uint8_t v2_payload[
        MEISO_WIFI_TELEMETRY_DESIRED_STATE_MAX_PAYLOAD_SIZE
    ];
    size_t v2_payload_size = 0U;
    bool v2_offer_seen = false;
    bool v2_accept_source_complete = false;
    bool v2_barrier_seen = false;
    bool actual_v2_source_complete = false;
    bool actual_v2_accept_seen = false;
    bool v4_offer_seen = false;
    bool v4_accept_source_complete = false;
    bool actual_v4_queued = false;
    bool actual_v4_source_complete = false;
    bool actual_v4_accept_seen = false;
    bool ok = false;
    struct timespec deadline;

    memset(&actual, 0, sizeof(actual));
    memset(&borrowed_v2, 0, sizeof(borrowed_v2));
    CHECK(initialize_fixture(&fixture, UINT64_C(0x7000)));
    CHECK_GOTO(meiso_composition_host_runtime_set_wifi(
                   fixture.app, true, &values[0], &receipts[0]
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
               meiso_composition_host_runtime_set_wifi(
                   fixture.app, true, &values[1], &receipts[1]
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
               receipts[0].desired_state_version == 1U &&
               receipts[1].desired_state_version == 2U &&
               receipts[1].replaced_command_present &&
               receipts[1].replaced_command_id == receipts[0].command_id &&
               drive_control_ready(&fixture), cleanup);
    desired_replica_bytes = meiso_state_replica_memory_required(
        &fixture.catalog.routes[ROUTE_WIFI_DESIRED].route.resource
    );
    desired_replica_arena = calloc(1U, desired_replica_bytes);
    CHECK_GOTO(desired_replica_bytes != 0U &&
               desired_replica_arena != NULL &&
               meiso_state_replica_init(
                   desired_replica_arena, desired_replica_bytes,
                   &fixture.catalog.routes[ROUTE_WIFI_DESIRED].route.resource,
                   &desired_replica
               ) == MEISO_STATE_INIT_OK &&
               meiso_state_replica_begin_owner(
                   desired_replica, host_owner
               ) == MEISO_STATE_OWNER_STARTED &&
               meiso_composition_control_session_route_key(
                   fixture.edge, ROUTE_WIFI_DESIRED, &desired_route
               ) && initialize_wifi_actual_harness(&fixture, &actual) &&
               make_deadline(&deadline), cleanup);

    while (remaining_ms(&deadline) > 0 && !v2_offer_seen) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true, false, false
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionControlSessionPumpResult edge_result;

        memset(&host_result, 0, sizeof(host_result));
        CHECK_GOTO(host_pump_status_benign(
                       meiso_composition_host_runtime_pump(
                           fixture.app, &ready, &host_result
                       )
                   ), cleanup);
        memset(&edge_result, 0, sizeof(edge_result));
        CHECK_GOTO(edge_pump_status_benign(
                       meiso_composition_control_session_pump(
                           fixture.edge, true, true, &edge_result
                       )
                   ), cleanup);
        if (edge_result.event_present) {
            CHECK_GOTO(edge_result.event.type ==
                           MEISO_RUNTIME_EVENT_STATE_OFFER &&
                       route_equal(edge_result.event.route, desired_route) &&
                       decode_wifi_desired_exact(
                           &edge_result.event.state_offer, 2U, &values[1]
                       ) && edge_result.event.state_offer.state.payload.size <=
                           sizeof(v2_payload), cleanup);
            borrowed_v2 = edge_result.event;
            v2_payload_size =
                borrowed_v2.state_offer.state.payload.size;
            memcpy(v2_payload,
                   borrowed_v2.state_offer.state.payload.data,
                   v2_payload_size);
            v2_offer_seen = true;
        }
    }
    CHECK_GOTO(v2_offer_seen &&
               meiso_composition_host_runtime_set_wifi(
                   fixture.app, true, &values[2], &receipts[2]
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
               meiso_composition_host_runtime_set_wifi(
                   fixture.app, true, &values[3], &receipts[3]
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
               receipts[2].desired_state_version == 3U &&
               receipts[2].local_result ==
                   MEISO_COMPOSITION_HOST_RUNTIME_LOCAL_QUEUED &&
               !receipts[2].replaced_command_present &&
               receipts[3].desired_state_version == 4U &&
               receipts[3].local_result ==
                   MEISO_COMPOSITION_HOST_RUNTIME_LOCAL_REPLACED_PENDING &&
               receipts[3].replaced_command_present &&
               receipts[3].replaced_command_id == receipts[2].command_id &&
               borrowed_v2.state_offer.state.payload.size == v2_payload_size &&
               memcmp(borrowed_v2.state_offer.state.payload.data,
                      v2_payload, v2_payload_size) == 0,
               cleanup);
    {
        MeisoCompositionHostRuntimeSnapshot snapshot;
        const MeisoStateReceiveStatus receive = meiso_state_replica_receive(
            desired_replica, borrowed_v2.state_offer.state.owner,
            borrowed_v2.state_offer.state.version,
            borrowed_v2.state_offer.state.payload
        );

        CHECK_GOTO(meiso_composition_host_runtime_snapshot(
                       fixture.app, &snapshot
                   ) && snapshot.desired[
                       MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
                   ].command_id == receipts[3].command_id &&
                   snapshot.desired[
                       MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
                   ].state_version == 4U &&
                   snapshot.desired[
                       MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
                   ].inflight_present &&
                   snapshot.desired[
                       MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
                   ].inflight_command_id == receipts[1].command_id &&
                   snapshot.desired[
                       MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
                   ].inflight_state_version == 2U &&
                   !snapshot.desired[
                       MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
                   ].inflight_offer_accepted &&
                   !snapshot.desired[
                       MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
                   ].inflight_actual_observed &&
                   receive == MEISO_STATE_RECEIVE_NEW &&
                   meiso_composition_control_session_commit_state_receive(
                       fixture.edge, borrowed_v2.route,
                       &borrowed_v2.state_offer, receive
                   ) == MEISO_COMPOSITION_CONTROL_SESSION_OK &&
                   queue_state_accept(fixture.edge, 1U, &borrowed_v2),
                   cleanup);
    }

    CHECK_GOTO(make_deadline(&deadline), cleanup);
    while (remaining_ms(&deadline) > 0 && !v2_barrier_seen) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true, false, false
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionControlSessionPumpResult edge_result;
        MeisoRuntimeSchedulerCompletion completion;
        MeisoCompositionHostRuntimeSnapshot snapshot;

        memset(&host_result, 0, sizeof(host_result));
        CHECK_GOTO(host_pump_status_benign(
                       meiso_composition_host_runtime_pump(
                           fixture.app, &ready, &host_result
                       )
                   ), cleanup);
        memset(&edge_result, 0, sizeof(edge_result));
        CHECK_GOTO(edge_pump_status_benign(
                       meiso_composition_control_session_pump(
                           fixture.edge, true, true, &edge_result
                       )
                   ) && !edge_result.event_present, cleanup);
        memset(&completion, 0, sizeof(completion));
        if (meiso_composition_control_session_take_completion(
                fixture.edge, 1U, &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            CHECK_GOTO(completion.code ==
                           MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT,
                       cleanup);
            v2_accept_source_complete = true;
        }
        CHECK_GOTO(meiso_composition_host_runtime_snapshot(
                       fixture.app, &snapshot
                   ), cleanup);
        if (v2_accept_source_complete && snapshot.desired[
                MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
            ].inflight_source_completed && snapshot.desired[
                MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
            ].inflight_offer_accepted) {
            CHECK_GOTO(snapshot.desired[
                           MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
                       ].inflight_present &&
                       snapshot.desired[
                           MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
                       ].inflight_state_version == 2U &&
                       !snapshot.desired[
                           MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
                       ].inflight_actual_observed &&
                       snapshot.desired[
                           MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
                       ].state_version == 4U,
                       cleanup);
            v2_barrier_seen = true;
        }
    }
    CHECK_GOTO(v2_barrier_seen && queue_wifi_actual_for_desired(
                   &fixture, &actual, 2U, 1U, 77U,
                   &(const MeisoWifiTelemetryEffectiveParameters){
                       values[1].selected_fields,
                       values[1].sample_period_ms
                   }
               ) && make_deadline(&deadline), cleanup);

    while (remaining_ms(&deadline) > 0) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true, false, false
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionControlSessionPumpResult edge_result;
        MeisoRuntimeSchedulerCompletion completion;
        MeisoCompositionHostRuntimeSnapshot snapshot;

        memset(&host_result, 0, sizeof(host_result));
        CHECK_GOTO(host_pump_status_benign(
                       meiso_composition_host_runtime_pump(
                           fixture.app, &ready, &host_result
                       )
                   ), cleanup);
        memset(&edge_result, 0, sizeof(edge_result));
        CHECK_GOTO(edge_pump_status_benign(
                       meiso_composition_control_session_pump(
                           fixture.edge, true, true, &edge_result
                       )
                   ), cleanup);
        if (edge_result.event_present) {
            if (edge_result.event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT &&
                route_equal(edge_result.event.route, actual.route)) {
                const MeisoStateAcceptStatus accepted =
                    meiso_state_publisher_mark_delivery_accepted(
                        actual.publisher,
                        edge_result.event.state_accept_generation,
                        edge_result.event.state_accept_owner,
                        edge_result.event.state_accept_version
                    );

                CHECK_GOTO(accepted == MEISO_STATE_ACCEPT_CONFIRMED,
                           cleanup);
                if (edge_result.event.state_accept_version == 1U) {
                    actual_v2_accept_seen = true;
                } else if (edge_result.event.state_accept_version == 2U) {
                    actual_v4_accept_seen = true;
                } else {
                    CHECK_GOTO(false, cleanup);
                }
            } else if (edge_result.event.type ==
                           MEISO_RUNTIME_EVENT_STATE_OFFER &&
                       route_equal(edge_result.event.route, desired_route)) {
                const MeisoStateReceiveStatus receive =
                    meiso_state_replica_receive(
                        desired_replica,
                        edge_result.event.state_offer.state.owner,
                        edge_result.event.state_offer.state.version,
                        edge_result.event.state_offer.state.payload
                    );

                CHECK_GOTO(!v4_offer_seen &&
                       decode_wifi_desired_exact(
                           &edge_result.event.state_offer, 4U, &values[3]
                       ) && receive == MEISO_STATE_RECEIVE_NEW &&
                       meiso_composition_control_session_commit_state_receive(
                           fixture.edge, edge_result.event.route,
                           &edge_result.event.state_offer, receive
                       ) == MEISO_COMPOSITION_CONTROL_SESSION_OK &&
                       queue_state_accept(fixture.edge, 1U,
                                          &edge_result.event), cleanup);
                v4_offer_seen = true;
            } else {
                CHECK_GOTO(false, cleanup);
            }
        }
        memset(&completion, 0, sizeof(completion));
        if (meiso_composition_control_session_take_completion(
                fixture.edge, 2U, &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            CHECK_GOTO(completion.code ==
                           MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT,
                       cleanup);
            if (!actual_v2_source_complete) {
                actual_v2_source_complete = true;
            } else {
                actual_v4_source_complete = true;
            }
        }
        memset(&completion, 0, sizeof(completion));
        if (meiso_composition_control_session_take_completion(
                fixture.edge, 1U, &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            CHECK_GOTO(v4_offer_seen && completion.code ==
                           MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT,
                       cleanup);
            v4_accept_source_complete = true;
        }
        if (actual_v2_source_complete && actual_v2_accept_seen &&
            v4_offer_seen && !actual_v4_queued) {
            CHECK_GOTO(queue_wifi_actual_for_desired(
                           &fixture, &actual, 4U, 2U, 78U,
                           &(const MeisoWifiTelemetryEffectiveParameters){
                               values[3].selected_fields,
                               values[3].sample_period_ms
                           }
                       ), cleanup);
            actual_v4_queued = true;
        }
        CHECK_GOTO(meiso_composition_host_runtime_snapshot(
                       fixture.app, &snapshot
                   ), cleanup);
        if (actual_v4_source_complete && actual_v4_accept_seen &&
            v4_accept_source_complete &&
            snapshot.desired[
                MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
            ].delivery_accepted &&
            !snapshot.desired[
                MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
            ].inflight_present &&
            snapshot.actual[
                MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_WIFI
            ].present && snapshot.actual[
                MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_WIFI
            ].observed_desired.version == 4U) {
            CHECK_GOTO(snapshot.desired[
                           MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
                       ].command_id == receipts[3].command_id &&
                       snapshot.desired[
                           MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_WIFI
                       ].state_version == 4U &&
                       snapshot.actual[
                           MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_WIFI
                       ].applied_desired.version == 4U &&
                       snapshot.actual[
                           MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_WIFI
                       ].production_epoch == 78U &&
                       snapshot.wifi_effective_present &&
                       snapshot.wifi_effective.selected_fields ==
                           values[3].selected_fields &&
                       snapshot.wifi_effective.sample_period_ms == 800U,
                       cleanup);
            ok = true;
            break;
        }
    }

cleanup:
    destroy_wifi_actual_harness(&actual);
    free(desired_replica_arena);
    destroy_fixture(&fixture);
    return ok;
}

static bool test_presentation_echo_inflight_barrier(void)
{
    const MeisoOwnerScope host_owner = {MEISO_OWNER_ROLE_HOST, 31U};
    const MeisoCompositionHostRuntimePresentationSettings settings = {
        MEISO_COMPOSITION_SCENE_PLAN_USE_SAFETY_FRAME,
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH,
        MEISO_COMPOSITION_SCENE_TARGET_HEIGHT,
        MEISO_COMPOSITION_SCENE_COLOR_RGBA8888,
        MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32,
        MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ,
        1000U
    };
    HostRuntimeFixture fixture;
    PresentationActualHarness actual;
    MeisoStateReplica *desired_replica = NULL;
    void *desired_replica_arena = NULL;
    size_t desired_replica_bytes;
    MeisoCompositionHostRuntimeCommandReceipt receipt;
    MeisoRuntimeRouteKey desired_route;
    MeisoRuntimeEvent desired_v1;
    MeisoCompositionScenePresentationPlan plan_v1;
    MeisoCompositionScenePresentationPlan plan_echo;
    uint8_t token[MEISO_SHA256_DIGEST_SIZE];
    bool desired_v1_seen = false;
    bool actual_v1_source_complete = false;
    bool actual_v1_accept_seen = false;
    bool actual_observed_before_accept = false;
    uint8_t desired_accept_completions = 0U;
    bool echo_seen = false;
    bool actual_v2_queued = false;
    bool actual_v2_source_complete = false;
    bool actual_v2_accept_seen = false;
    bool ok = false;
    struct timespec deadline;
    size_t index;

    memset(&actual, 0, sizeof(actual));
    memset(&desired_v1, 0, sizeof(desired_v1));
    memset(&plan_v1, 0, sizeof(plan_v1));
    memset(&plan_echo, 0, sizeof(plan_echo));
    for (index = 0U; index < sizeof(token); ++index) {
        token[index] = (uint8_t)(UINT8_C(0x41) + (uint8_t)index);
    }
    CHECK(initialize_fixture(&fixture, UINT64_C(0x7100)));
    CHECK_GOTO(meiso_composition_host_runtime_set_presentation(
                   fixture.app, true, &settings, &receipt
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
               receipt.command_id == 1U &&
               receipt.desired_state_version == 1U &&
               receipt.plan_generation_present &&
               receipt.plan_generation == 1U &&
               drive_control_ready(&fixture), cleanup);
    desired_replica_bytes = meiso_state_replica_memory_required(
        &fixture.catalog.routes[ROUTE_PRESENTATION_DESIRED].route.resource
    );
    desired_replica_arena = calloc(1U, desired_replica_bytes);
    CHECK_GOTO(desired_replica_bytes != 0U &&
               desired_replica_arena != NULL &&
               meiso_state_replica_init(
                   desired_replica_arena, desired_replica_bytes,
                   &fixture.catalog.routes[
                       ROUTE_PRESENTATION_DESIRED
                   ].route.resource,
                   &desired_replica
               ) == MEISO_STATE_INIT_OK &&
               meiso_state_replica_begin_owner(
                   desired_replica, host_owner
               ) == MEISO_STATE_OWNER_STARTED &&
               meiso_composition_control_session_route_key(
                   fixture.edge, ROUTE_PRESENTATION_DESIRED, &desired_route
               ) && initialize_presentation_actual_harness(
                   &fixture, &actual
               ) && make_deadline(&deadline), cleanup);

    while (remaining_ms(&deadline) > 0 && !desired_v1_seen) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true, false, false
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionControlSessionPumpResult edge_result;

        memset(&host_result, 0, sizeof(host_result));
        CHECK_GOTO(host_pump_status_benign(
                       meiso_composition_host_runtime_pump(
                           fixture.app, &ready, &host_result
                       )
                   ), cleanup);
        memset(&edge_result, 0, sizeof(edge_result));
        CHECK_GOTO(edge_pump_status_benign(
                       meiso_composition_control_session_pump(
                           fixture.edge, true, true, &edge_result
                       )
                   ), cleanup);
        if (edge_result.event_present) {
            CHECK_GOTO(edge_result.event.type ==
                           MEISO_RUNTIME_EVENT_STATE_OFFER &&
                       route_equal(edge_result.event.route, desired_route) &&
                       decode_presentation_desired(
                           &edge_result.event.state_offer, 1U, &plan_v1
                       ) && plan_v1.plan_generation == 1U &&
                       !plan_v1.prepared_token_echo_present,
                       cleanup);
            desired_v1 = edge_result.event;
            desired_v1_seen = true;
        }
    }
    if (desired_v1_seen) {
        const MeisoStateReceiveStatus receive = meiso_state_replica_receive(
            desired_replica, desired_v1.state_offer.state.owner,
            desired_v1.state_offer.state.version,
            desired_v1.state_offer.state.payload
        );

        CHECK_GOTO(receive == MEISO_STATE_RECEIVE_NEW &&
                   meiso_composition_control_session_commit_state_receive(
                       fixture.edge, desired_v1.route,
                       &desired_v1.state_offer, receive
                   ) == MEISO_COMPOSITION_CONTROL_SESSION_OK &&
                   queue_presentation_actual(
                       &fixture, &actual, &plan_v1, token, 1U, 1U,
                       MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED
                   ) && make_deadline(&deadline), cleanup);
    } else {
        CHECK_GOTO(false, cleanup);
    }

    while (remaining_ms(&deadline) > 0 &&
           (!actual_observed_before_accept ||
            !actual_v1_source_complete || !actual_v1_accept_seen)) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true, false, false
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionControlSessionPumpResult edge_result;
        MeisoRuntimeSchedulerCompletion completion;
        MeisoCompositionHostRuntimeSnapshot snapshot;

        memset(&host_result, 0, sizeof(host_result));
        CHECK_GOTO(host_pump_status_benign(
                       meiso_composition_host_runtime_pump(
                           fixture.app, &ready, &host_result
                       )
                   ), cleanup);
        memset(&edge_result, 0, sizeof(edge_result));
        CHECK_GOTO(edge_pump_status_benign(
                       meiso_composition_control_session_pump(
                           fixture.edge, true, true, &edge_result
                       )
                   ), cleanup);
        if (edge_result.event_present) {
            CHECK_GOTO(edge_result.event.type ==
                           MEISO_RUNTIME_EVENT_STATE_ACCEPT &&
                       route_equal(edge_result.event.route, actual.route) &&
                       edge_result.event.state_accept_version == 1U &&
                       meiso_state_publisher_mark_delivery_accepted(
                           actual.publisher,
                           edge_result.event.state_accept_generation,
                           edge_result.event.state_accept_owner,
                           edge_result.event.state_accept_version
                       ) == MEISO_STATE_ACCEPT_CONFIRMED,
                       cleanup);
            actual_v1_accept_seen = true;
        }
        memset(&completion, 0, sizeof(completion));
        if (meiso_composition_control_session_take_completion(
                fixture.edge, 6U, &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            CHECK_GOTO(completion.code ==
                           MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT,
                       cleanup);
            actual_v1_source_complete = true;
        }
        CHECK_GOTO(meiso_composition_host_runtime_snapshot(
                       fixture.app, &snapshot
                   ), cleanup);
        if (snapshot.actual[
                MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_PRESENTATION
            ].present && snapshot.desired[
                MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_PRESENTATION
            ].inflight_actual_observed) {
            CHECK_GOTO(snapshot.presentation_status_present &&
                       snapshot.presentation_status.candidate_state ==
                           MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED &&
                       snapshot.desired[
                           MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_PRESENTATION
                       ].inflight_present &&
                       !snapshot.desired[
                           MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_PRESENTATION
                       ].inflight_offer_accepted &&
                       snapshot.desired[
                           MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_PRESENTATION
                       ].state_version == 1U &&
                       snapshot.desired[
                           MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_PRESENTATION
                       ].command_id == receipt.command_id,
                       cleanup);
            actual_observed_before_accept = true;
        }
    }
    CHECK_GOTO(actual_observed_before_accept &&
               actual_v1_source_complete && actual_v1_accept_seen &&
               queue_state_accept(fixture.edge, 5U, &desired_v1) &&
               make_deadline(&deadline), cleanup);

    while (remaining_ms(&deadline) > 0) {
        const MeisoCompositionHostRuntimeIoReady ready = {
            true, true, false, false
        };
        MeisoCompositionHostRuntimePumpResult host_result;
        MeisoCompositionControlSessionPumpResult edge_result;
        MeisoRuntimeSchedulerCompletion completion;
        MeisoCompositionHostRuntimeSnapshot snapshot;

        memset(&host_result, 0, sizeof(host_result));
        CHECK_GOTO(host_pump_status_benign(
                       meiso_composition_host_runtime_pump(
                           fixture.app, &ready, &host_result
                       )
                   ), cleanup);
        memset(&edge_result, 0, sizeof(edge_result));
        CHECK_GOTO(edge_pump_status_benign(
                       meiso_composition_control_session_pump(
                           fixture.edge, true, true, &edge_result
                       )
                   ), cleanup);
        if (edge_result.event_present) {
            if (edge_result.event.type == MEISO_RUNTIME_EVENT_STATE_OFFER &&
                route_equal(edge_result.event.route, desired_route)) {
                const MeisoStateReceiveStatus receive =
                    meiso_state_replica_receive(
                        desired_replica,
                        edge_result.event.state_offer.state.owner,
                        edge_result.event.state_offer.state.version,
                        edge_result.event.state_offer.state.payload
                    );

                CHECK_GOTO(!echo_seen &&
                       decode_presentation_desired(
                           &edge_result.event.state_offer, 2U, &plan_echo
                       ) && plan_echo.plan_generation ==
                           plan_v1.plan_generation &&
                       memcmp(plan_echo.candidate_digest,
                              plan_v1.candidate_digest,
                              sizeof(plan_echo.candidate_digest)) == 0 &&
                       plan_echo.prepared_token_echo_present &&
                       memcmp(plan_echo.prepared_token_echo, token,
                              sizeof(token)) == 0 &&
                       receive == MEISO_STATE_RECEIVE_NEW &&
                       meiso_composition_control_session_commit_state_receive(
                           fixture.edge, edge_result.event.route,
                           &edge_result.event.state_offer, receive
                       ) == MEISO_COMPOSITION_CONTROL_SESSION_OK &&
                       queue_state_accept(
                           fixture.edge, 5U, &edge_result.event
                       ), cleanup);
                echo_seen = true;
            } else if (edge_result.event.type ==
                           MEISO_RUNTIME_EVENT_STATE_ACCEPT &&
                       route_equal(edge_result.event.route, actual.route)) {
                CHECK_GOTO(edge_result.event.state_accept_version == 2U &&
                       meiso_state_publisher_mark_delivery_accepted(
                           actual.publisher,
                           edge_result.event.state_accept_generation,
                           edge_result.event.state_accept_owner,
                           edge_result.event.state_accept_version
                       ) == MEISO_STATE_ACCEPT_CONFIRMED,
                       cleanup);
                actual_v2_accept_seen = true;
            } else {
                CHECK_GOTO(false, cleanup);
            }
        }
        memset(&completion, 0, sizeof(completion));
        if (meiso_composition_control_session_take_completion(
                fixture.edge, 5U, &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            CHECK_GOTO(completion.code ==
                           MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT &&
                       desired_accept_completions < 2U,
                       cleanup);
            ++desired_accept_completions;
        }
        memset(&completion, 0, sizeof(completion));
        if (meiso_composition_control_session_take_completion(
                fixture.edge, 6U, &completion
            ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
            CHECK_GOTO(actual_v2_queued && completion.code ==
                           MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT,
                       cleanup);
            actual_v2_source_complete = true;
        }
        if (echo_seen && desired_accept_completions >= 1U &&
            !actual_v2_queued) {
            CHECK_GOTO(queue_presentation_actual(
                           &fixture, &actual, &plan_echo, token, 2U, 2U,
                           MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING
                       ), cleanup);
            actual_v2_queued = true;
        }
        CHECK_GOTO(meiso_composition_host_runtime_snapshot(
                       fixture.app, &snapshot
                   ), cleanup);
        if (echo_seen && desired_accept_completions == 2U &&
            actual_v2_source_complete && actual_v2_accept_seen &&
            snapshot.desired[
                MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_PRESENTATION
            ].delivery_accepted && !snapshot.desired[
                MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_PRESENTATION
            ].inflight_present && snapshot.actual[
                MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_PRESENTATION
            ].observed_desired.version == 2U) {
            CHECK_GOTO(snapshot.desired[
                           MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_PRESENTATION
                       ].command_id == receipt.command_id &&
                       snapshot.desired[
                           MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_PRESENTATION
                       ].state_version == 2U &&
                       snapshot.plan_generation_high_water == 1U &&
                       snapshot.presentation_status_present &&
                       snapshot.presentation_status.candidate_state ==
                           MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING &&
                       snapshot.presentation_status.prepared_token_present &&
                       memcmp(snapshot.presentation_status.prepared_token,
                              token, sizeof(token)) == 0,
                       cleanup);
            ok = true;
            break;
        }
    }

cleanup:
    destroy_presentation_actual_harness(&actual);
    free(desired_replica_arena);
    destroy_fixture(&fixture);
    return ok;
}

static bool test_remote_edge_init_atomic(void)
{
    HostRuntimeFixture fixture;
    MeisoCompositionRemoteEdgeConfig config;
    MeisoCompositionRemoteEdgeConfig invalid_config;
    const size_t required =
        meiso_composition_remote_edge_memory_required();
    const size_t alignment =
        meiso_composition_remote_edge_alignment_required();
    uint8_t *storage = NULL;
    uint8_t *before = NULL;
    uint8_t *host_before = NULL;
    MeisoEdgeHandle *out_handle;
    bool ok = false;

    CHECK(initialize_fixture(&fixture, UINT64_C(0x0700)));
    storage = malloc(required + alignment);
    before = malloc(required);
    host_before = malloc(fixture.app_bytes);
    CHECK_GOTO(required != 0U && alignment > 1U && storage != NULL &&
                   before != NULL && host_before != NULL &&
                   ((uintptr_t)storage % alignment) == 0U,
               cleanup);
    config = remote_edge_config(&fixture, UINT64_C(8999));
    out_handle = (MeisoEdgeHandle *)fixture.app;
    memset(storage, 0xa5, required + alignment);
    memcpy(before, storage, required);
    CHECK_GOTO(meiso_composition_remote_edge_init(
                   storage, required - 1U, &config, &out_handle
               ) == MEISO_COMPOSITION_REMOTE_EDGE_INIT_ARENA_TOO_SMALL &&
                   out_handle == (MeisoEdgeHandle *)fixture.app &&
                   memcmp(storage, before, required) == 0,
               cleanup);
    invalid_config = config;
    invalid_config.catalog.wifi_incarnation = 0U;
    CHECK_GOTO(meiso_composition_remote_edge_init(
                   storage, required, &invalid_config, &out_handle
               ) == MEISO_COMPOSITION_REMOTE_EDGE_INIT_INVALID_CONFIG &&
                   out_handle == (MeisoEdgeHandle *)fixture.app &&
                   memcmp(storage, before, required) == 0,
               cleanup);
    CHECK_GOTO(meiso_composition_remote_edge_init(
                   storage + 1U, required, &config, &out_handle
               ) == MEISO_COMPOSITION_REMOTE_EDGE_INIT_MISALIGNED_ARENA &&
                   out_handle == (MeisoEdgeHandle *)fixture.app &&
                   memcmp(storage, before, required) == 0,
               cleanup);
    CHECK_GOTO(meiso_composition_remote_edge_init(
                   storage, required, &config,
                   (MeisoEdgeHandle **)storage
               ) == MEISO_COMPOSITION_REMOTE_EDGE_INIT_INVALID_ARGUMENT &&
                   memcmp(storage, before, required) == 0,
               cleanup);
    memcpy(host_before, fixture.app_arena, fixture.app_bytes);
    CHECK_GOTO(meiso_composition_remote_edge_init(
                   fixture.app_arena, required, &config, &out_handle
               ) == MEISO_COMPOSITION_REMOTE_EDGE_INIT_INVALID_ARGUMENT &&
                   out_handle == (MeisoEdgeHandle *)fixture.app &&
                   memcmp(
                       fixture.app_arena, host_before, fixture.app_bytes
                   ) == 0,
               cleanup);
    CHECK_GOTO(meiso_composition_remote_edge_init(
                   storage, required + alignment, &config, &out_handle
               ) == MEISO_COMPOSITION_REMOTE_EDGE_INIT_OK &&
                   out_handle == (MeisoEdgeHandle *)storage &&
                   buffer_is_byte(
                       storage + required, alignment, UINT8_C(0xa5)
                   ),
               cleanup);
    CHECK_GOTO(meiso_composition_host_runtime_abort(
                   fixture.app,
                   MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_LOCAL_ABORT, 0
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK,
               cleanup);
    memset(storage, 0xa5, required + alignment);
    memcpy(before, storage, required);
    memcpy(host_before, fixture.app_arena, fixture.app_bytes);
    out_handle = (MeisoEdgeHandle *)fixture.app;
    CHECK_GOTO(meiso_composition_remote_edge_init(
                   storage, required + alignment, &config, &out_handle
               ) == MEISO_COMPOSITION_REMOTE_EDGE_INIT_BACKEND_TERMINAL &&
                   out_handle == (MeisoEdgeHandle *)fixture.app &&
                   memcmp(storage, before, required) == 0 &&
                   buffer_is_byte(
                       storage + required, alignment, UINT8_C(0xa5)
                   ) &&
                   memcmp(
                       fixture.app_arena, host_before, fixture.app_bytes
                   ) == 0,
               cleanup);
    ok = true;

cleanup:
    free(host_before);
    free(before);
    free(storage);
    destroy_fixture(&fixture);
    return ok;
}

static bool test_remote_edge_facade(void)
{
    const MeisoOwnerScope edge_owner = {MEISO_OWNER_ROLE_EDGE, 41U};
    HostRuntimeFixture fixture;
    MeisoEdgeHandle *edge_handle = NULL;
    void *handle_arena = NULL;
    uint8_t *handle_before = NULL;
    MeisoEdgeCommandReceipt receipt;
    MeisoEdgeCommandReceipt receipt_before;
    MeisoStatePublisher *publisher = NULL;
    void *publisher_arena = NULL;
    uint8_t sample_wire[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    MeisoWifiTelemetrySample sample;
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeRouteKey route;
    MeisoCompositionHostRuntimePumpResult inline_result;
    size_t sample_size = 0U;
    bool ok = false;

    CHECK(initialize_fixture(&fixture, UINT64_C(0x0800)));
    CHECK_GOTO(initialize_remote_edge(
                   &fixture, UINT64_C(9001), &handle_arena, &edge_handle
               ) && edge_handle != NULL,
               cleanup);
    handle_before = malloc(
        meiso_composition_remote_edge_memory_required()
    );
    CHECK_GOTO(handle_before != NULL, cleanup);
    memcpy(handle_before, handle_arena,
           meiso_composition_remote_edge_memory_required());
    memset(&receipt, 0xa5, sizeof(receipt));
    memcpy(&receipt_before, &receipt, sizeof(receipt));
    CHECK_GOTO(meiso_edge_snapshot(
                   edge_handle, (MeisoEdgeSnapshot *)edge_handle
               ) == MEISO_EDGE_INVALID_ARGUMENT &&
                   meiso_composition_edge_set_wifi(
                       edge_handle, true,
                       (const MeisoWifiTelemetryDesiredParameters *)edge_handle,
                       &receipt
                   ) == MEISO_EDGE_INVALID_ARGUMENT &&
                   memcmp(&receipt, &receipt_before, sizeof(receipt)) == 0 &&
                   memcmp(
                       handle_before, handle_arena,
                       meiso_composition_remote_edge_memory_required()
                   ) == 0 &&
                   meiso_test_composition_edge_bootstrap(edge_handle) &&
                   meiso_test_composition_edge_submit_wifi(edge_handle, &receipt) &&
                   drive_control_ready(&fixture) &&
                   drive_single_wifi_desired(&fixture) &&
                   queue_wifi_actual(
                       &fixture, &publisher, &publisher_arena
                   ) &&
                   drive_wifi_actual(&fixture, publisher) &&
                   meiso_test_composition_edge_ready(
                       edge_handle, UINT64_C(9001), false
                   ),
               cleanup);

    memset(&sample, 0, sizeof(sample));
    sample.selected_fields = MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
        MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM;
    sample.present_fields = sample.selected_fields;
    sample.link_state = true;
    sample.signal_mbm = -5100;
    CHECK_GOTO(meiso_wifi_telemetry_sample_encode(
                   &sample,
                   (MeisoMutableBuffer){sample_wire, sizeof(sample_wire)},
                   &sample_size
               ) == MEISO_WIFI_TELEMETRY_CODEC_OK &&
                   meiso_composition_control_session_route_key(
                       fixture.edge, ROUTE_WIFI_DATA, &route
                   ),
               cleanup);
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE;
    work.value.stream_sample.route = route;
    work.value.stream_sample.production_epoch = 77U;
    work.value.stream_sample.sample.key =
        (MeisoStreamKey){edge_owner, 77U, 1U};
    work.value.stream_sample.sample.source_time =
        (MeisoTimePoint){77U, 1000U};
    work.value.stream_sample.sample.payload =
        (MeisoByteView){sample_wire, sample_size};
    CHECK_GOTO(drive_inline_sample_once(
                   &fixture, &work, &inline_result
               ) && meiso_test_composition_edge_ready(
                   edge_handle, UINT64_C(9001), true
               ) && meiso_composition_host_runtime_abort(
                   fixture.app, MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_LOCAL_ABORT,
                   0
               ) == MEISO_COMPOSITION_HOST_RUNTIME_OK &&
                   meiso_test_composition_edge_terminal(edge_handle),
               cleanup);
    ok = true;

cleanup:
    free(publisher_arena);
    free(handle_before);
    free(handle_arena);
    destroy_fixture(&fixture);
    return ok;
}

static bool test_fixture_checkpoint(void)
{
    HostRuntimeFixture fixture;
    uint8_t *arena_before = NULL;
    MeisoCompositionHostRuntimeSnapshot snapshot;
    MeisoCompositionHostRuntimeIoInterest interest;
    MeisoCompositionControlSessionSnapshot edge_snapshot;
    bool ok = false;

    CHECK(meiso_composition_host_runtime_alignment_required() ==
          alignof(max_align_t));
    if (!initialize_fixture(&fixture, UINT64_C(0x1000))) {
        return false;
    }
    CHECK_GOTO(meiso_composition_host_runtime_snapshot(fixture.app, &snapshot),
               cleanup);
    CHECK_GOTO(snapshot.phase == MEISO_COMPOSITION_HOST_RUNTIME_PHASE_HANDSHAKE &&
               snapshot.active_catalog_generation == 0U &&
               !snapshot.control_ready && !snapshot.media_bound &&
               snapshot.media_active_route_mask == 0U &&
               snapshot.media_slots_consumed == 0U &&
               snapshot.media_slots_capacity ==
                   MEISO_COMPOSITION_HOST_RUNTIME_MAX_MEDIA_CONNECTIONS &&
               snapshot.next_command_id == 1U &&
               snapshot.arena_bytes == fixture.app_bytes,
               cleanup);
    CHECK_GOTO(meiso_composition_host_runtime_readiness(
                   fixture.app, &interest
               ) && interest.control_read,
               cleanup);
    CHECK_GOTO(meiso_composition_control_session_snapshot(
                   fixture.edge, &edge_snapshot
               ) && edge_snapshot.phase ==
                   MEISO_COMPOSITION_CONTROL_SESSION_PHASE_HANDSHAKE,
               cleanup);
    CHECK_GOTO(queue_pre_ready_commands(&fixture), cleanup);
    CHECK_GOTO(drive_control_ready(&fixture), cleanup);
    arena_before = malloc(fixture.app_bytes);
    CHECK_GOTO(arena_before != NULL, cleanup);
    memcpy(arena_before, fixture.app_arena, fixture.app_bytes);
    CHECK_GOTO(meiso_composition_host_runtime_bind_media_fd(
                   fixture.app, fixture.host_fd,
                   MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_RENDER
               ) == MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT &&
               memcmp(
                   arena_before, fixture.app_arena, fixture.app_bytes
               ) == 0 &&
               meiso_composition_host_runtime_snapshot(fixture.app, &snapshot) &&
               !snapshot.media_bound &&
               snapshot.media_active_route_mask == 0U &&
               !snapshot.media[0].actual_binding_ready &&
               snapshot.media[0].source ==
                   MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW &&
               !snapshot.media[1].actual_binding_ready &&
               snapshot.media[1].source ==
                   MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
               cleanup);
    CHECK_GOTO(drive_first_wifi_desired(&fixture), cleanup);
    ok = true;

cleanup:
    free(arena_before);
    destroy_fixture(&fixture);
    return ok;
}

int main(void)
{
    if (!test_remote_edge_init_atomic() || !test_remote_edge_facade() ||
        !test_fixture_checkpoint() ||
        !test_wifi_inflight_barrier_and_coalesce() ||
        !test_presentation_echo_inflight_barrier() ||
        !test_blob_full_interleave() ||
        !test_inline_duplicate_result() || !test_render_media_smoke()) {
        return 1;
    }
    if (!test_camera_and_both_media_masks()) {
        return 1;
    }
    (void)puts("composition host app fixture checkpoint: PASS");
    return 0;
}
