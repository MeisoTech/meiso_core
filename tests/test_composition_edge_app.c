#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif

#include "composition_edge_app.h"

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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * Source-checkpoint expansion matrix:
 *
 *  1. config/arena exact, overflow, aliases, fd ownership and no-partial;
 *  2. real AF_INET Host DebugPeer, Profile-v3 6/18 catalog and 20 sources;
 *  3. controlled Wi-Fi/device/camera probes and capability intersection;
 *  4. six desired Replica obligations and six derived actual Publishers;
 *  5. Blob publish/duplicate -> explicit visibility -> Scene/C2 activation;
 *  6. CPU raster FIFO/targets -> RenderResult inline and external routes;
 *  7. Wi-Fi/device cadence, thermal handoff and camera borrow/release;
 *  8. render-only/camera-only/both media binds and one-use slot boundaries;
 *  9. catalog/Session/adapter/cleanup terminal and fresh-app no-resume.
 *
 * DeviceTelemetry and Wi-Fi paths plus Camera syscalls are injectable. This
 * fixture keeps all three target seams controlled and never reads host
 * telemetry or opens a host camera device.
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
    ROUTE_CAMERA_EXTERNAL = 17,
    TEST_PATH_CAPACITY = 256
};

typedef struct {
    uint8_t capability[MEISO_COMPOSITION_SESSION_V3_DYNAMIC_CAPABILITY_BYTES];
    MeisoRuntimeEndpointDescriptor
        endpoints[MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT];
    MeisoCompositionSessionV3RouteDescriptor
        routes[MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT];
} CatalogStorage;

typedef struct {
    char directory[TEST_PATH_CAPACITY];
    char stat_path[TEST_PATH_CAPACITY];
    char meminfo_path[TEST_PATH_CAPACITY];
    char thermal_type_path[TEST_PATH_CAPACITY];
    char thermal_value_path[TEST_PATH_CAPACITY];
    MeisoDeviceTelemetryLinuxAdapterConfig config;
} DeviceFiles;

typedef struct {
    char directory[TEST_PATH_CAPACITY];
    char sys_class_net_root[TEST_PATH_CAPACITY];
    char interface_root[TEST_PATH_CAPACITY];
    char wireless_directory[TEST_PATH_CAPACITY];
    char carrier_path[TEST_PATH_CAPACITY];
    char proc_wireless_path[TEST_PATH_CAPACITY];
} WifiFiles;

typedef struct {
    uint8_t *storage;
    size_t storage_size;
    bool succeed;
    int dequeue_error;
    int queue_error;
    MeisoCameraV4l2BufferIoctl dequeue;
    uint64_t open_calls;
    uint64_t ioctl_calls;
    uint64_t dequeue_calls;
    uint64_t map_calls;
    uint64_t unmap_calls;
    uint64_t close_calls;
} FakeCamera;

enum { FAKE_CAMERA_FD = 41 };

typedef struct {
    int host_fd;
    int edge_fd;
    int host_fd_flags;
    int edge_fd_flags;
    DeviceFiles device;
    WifiFiles wifi;
    FakeCamera camera;
    MeisoCameraV4l2Syscalls camera_syscalls;
    MeisoCompositionEdgeAppConfig edge_config;
    MeisoCompositionDebugPeerConfig host_config;
    CatalogStorage catalog;
    size_t edge_bytes;
    size_t host_bytes;
} EdgeFixtureShape;

static const MeisoPeerId HOST_PEER = {
    UINT64_C(0x1111111111111111), UINT64_C(0x1212121212121212)
};
static const MeisoPeerId EDGE_PEER = {
    UINT64_C(0x2121212121212121), UINT64_C(0x2222222222222222)
};
static const MeisoOwnerScope HOST_OWNER = {
    MEISO_OWNER_ROLE_HOST, 31U
};
static const MeisoOwnerScope EDGE_OWNER = {
    MEISO_OWNER_ROLE_EDGE, 41U
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
    int result;

    do {
        result = poll(&item, (nfds_t)1U, 0);
    } while (result < 0 && errno == EINTR);
    return result > 0 && (item.revents & (short)(event | POLLERR | POLLHUP)) !=
        0 && (item.revents & POLLNVAL) == 0;
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

static bool write_text(const char *path, const char *text)
{
    FILE *file;

    if (path == NULL || text == NULL) {
        return false;
    }
    file = fopen(path, "w");
    return file != NULL && fputs(text, file) >= 0 && fclose(file) == 0;
}

static bool format_path(
    char *output,
    size_t output_capacity,
    const char *parent,
    const char *leaf
)
{
    const int count = snprintf(output, output_capacity, "%s/%s", parent, leaf);

    return count > 0 && (size_t)count < output_capacity;
}

static bool wifi_files_create(WifiFiles *fixture)
{
    char directory[] = "/tmp/meiso-edge-wifi.XXXXXX";

    memset(fixture, 0, sizeof(*fixture));
    CHECK(mkdtemp(directory) != NULL &&
          strlen(directory) < sizeof(fixture->directory));
    memcpy(fixture->directory, directory, strlen(directory) + 1U);
    CHECK(format_path(
        fixture->sys_class_net_root, sizeof(fixture->sys_class_net_root),
        directory, "class-net"
    ) && mkdir(fixture->sys_class_net_root, 0700) == 0);
    CHECK(format_path(
        fixture->interface_root, sizeof(fixture->interface_root),
        fixture->sys_class_net_root, "wlan0"
    ) && mkdir(fixture->interface_root, 0700) == 0);
    CHECK(format_path(
        fixture->wireless_directory, sizeof(fixture->wireless_directory),
        fixture->interface_root, "wireless"
    ) && mkdir(fixture->wireless_directory, 0700) == 0);
    CHECK(format_path(
        fixture->carrier_path, sizeof(fixture->carrier_path),
        fixture->interface_root, "carrier"
    ) && write_text(fixture->carrier_path, "1\n"));
    CHECK(format_path(
        fixture->proc_wireless_path, sizeof(fixture->proc_wireless_path),
        directory, "wireless"
    ) && write_text(
        fixture->proc_wireless_path,
        "Inter-| sta\n face | quality\n wlan0: 0000 42. -55. 0 0 0 0 0\n"
    ));
    return true;
}

static void wifi_files_destroy(WifiFiles *fixture)
{
    if (fixture != NULL) {
        (void)unlink(fixture->carrier_path);
        (void)unlink(fixture->proc_wireless_path);
        (void)rmdir(fixture->wireless_directory);
        (void)rmdir(fixture->interface_root);
        (void)rmdir(fixture->sys_class_net_root);
        (void)rmdir(fixture->directory);
        memset(fixture, 0, sizeof(*fixture));
    }
}

static bool device_files_create(DeviceFiles *fixture)
{
    char directory[] = "/tmp/meiso-edge-device.XXXXXX";

    memset(fixture, 0, sizeof(*fixture));
    CHECK(mkdtemp(directory) != NULL &&
          strlen(directory) < sizeof(fixture->directory));
    memcpy(fixture->directory, directory, strlen(directory) + 1U);
    CHECK(snprintf(fixture->stat_path, sizeof(fixture->stat_path),
                   "%s/stat", directory) > 0 &&
          snprintf(fixture->meminfo_path, sizeof(fixture->meminfo_path),
                   "%s/meminfo", directory) > 0 &&
          snprintf(fixture->thermal_type_path,
                   sizeof(fixture->thermal_type_path),
                   "%s/type", directory) > 0 &&
          snprintf(fixture->thermal_value_path,
                   sizeof(fixture->thermal_value_path),
                   "%s/temp", directory) > 0);
    CHECK(write_text(fixture->stat_path, "cpu 100 10 20 200\n") &&
          write_text(fixture->meminfo_path,
                     "MemTotal: 1024 kB\nMemAvailable: 512 kB\n") &&
          write_text(
              fixture->thermal_type_path, "canaan_thermal_zone\n"
          ) &&
          write_text(fixture->thermal_value_path, "42000\n"));
    fixture->config = (MeisoDeviceTelemetryLinuxAdapterConfig){
        fixture->stat_path,
        fixture->meminfo_path,
        fixture->thermal_type_path,
        fixture->thermal_value_path,
        MEISO_DEVICE_TELEMETRY_FIELD_ALL
    };
    return true;
}

static void device_files_destroy(DeviceFiles *fixture)
{
    if (fixture != NULL) {
        (void)unlink(fixture->stat_path);
        (void)unlink(fixture->meminfo_path);
        (void)unlink(fixture->thermal_type_path);
        (void)unlink(fixture->thermal_value_path);
        (void)rmdir(fixture->directory);
        memset(fixture, 0, sizeof(*fixture));
    }
}

static int fake_camera_open(
    void *context,
    const char *path,
    int flags,
    int *out_fd
)
{
    FakeCamera *camera = context;
    (void)path;
    (void)flags;
    ++camera->open_calls;
    if (!camera->succeed) {
        return ENOSYS;
    }
    *out_fd = FAKE_CAMERA_FD;
    return 0;
}

static int fake_camera_ioctl(
    void *context,
    int fd,
    MeisoCameraV4l2IoctlRequest request,
    MeisoCameraV4l2IoctlArgument *argument
)
{
    FakeCamera *camera = context;
    ++camera->ioctl_calls;
    if (!camera->succeed || fd != FAKE_CAMERA_FD || argument == NULL) {
        return ENOSYS;
    }
    switch (request) {
    case MEISO_CAMERA_V4L2_IOCTL_QUERY_CAPABILITY:
        argument->capability.video_capture = true;
        argument->capability.streaming = true;
        break;
    case MEISO_CAMERA_V4L2_IOCTL_SET_FORMAT:
        break;
    case MEISO_CAMERA_V4L2_IOCTL_GET_FORMAT:
        argument->format.width = MEISO_CAMERA_V4L2_WIDTH;
        argument->format.height = MEISO_CAMERA_V4L2_HEIGHT;
        argument->format.pixel_format =
            MEISO_COMPOSITION_MEDIA_PIXEL_NV12;
        argument->format.progressive = true;
        argument->format.bytes_per_line = MEISO_CAMERA_V4L2_BYTES_PER_LINE;
        argument->format.size_image = MEISO_CAMERA_V4L2_SIZE_IMAGE;
        break;
    case MEISO_CAMERA_V4L2_IOCTL_REQUEST_BUFFERS:
        if (argument->request_buffers.count != 0U) {
            argument->request_buffers.count =
                MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT;
        }
        break;
    case MEISO_CAMERA_V4L2_IOCTL_QUERY_BUFFER:
        argument->buffer.length = MEISO_CAMERA_V4L2_SIZE_IMAGE;
        argument->buffer.memory_offset =
            (uint64_t)argument->buffer.index *
            MEISO_CAMERA_V4L2_SIZE_IMAGE;
        break;
    case MEISO_CAMERA_V4L2_IOCTL_QUEUE_BUFFER:
        if (camera->queue_error != 0) {
            return camera->queue_error;
        }
        break;
    case MEISO_CAMERA_V4L2_IOCTL_STREAM_ON:
    case MEISO_CAMERA_V4L2_IOCTL_STREAM_OFF:
        break;
    case MEISO_CAMERA_V4L2_IOCTL_DEQUEUE_BUFFER:
        ++camera->dequeue_calls;
        if (camera->dequeue_error != 0) {
            return camera->dequeue_error;
        }
        argument->buffer = camera->dequeue;
        break;
    default:
        return EINVAL;
    }
    return 0;
}

static int fake_camera_map(
    void *context,
    int fd,
    size_t length,
    uint64_t offset,
    void **out_mapping
)
{
    FakeCamera *camera = context;
    ++camera->map_calls;
    if (!camera->succeed || fd != FAKE_CAMERA_FD || out_mapping == NULL ||
        length != MEISO_CAMERA_V4L2_SIZE_IMAGE ||
        offset % MEISO_CAMERA_V4L2_SIZE_IMAGE != 0U ||
        offset / MEISO_CAMERA_V4L2_SIZE_IMAGE >=
            MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT) {
        return EINVAL;
    }
    *out_mapping = camera->storage + (size_t)offset;
    return 0;
}

static int fake_camera_unmap(void *context, void *mapping, size_t length)
{
    FakeCamera *camera = context;
    ++camera->unmap_calls;
    return camera->succeed && mapping != NULL &&
            length == MEISO_CAMERA_V4L2_SIZE_IMAGE ? 0 : EINVAL;
}

static int fake_camera_close(void *context, int fd)
{
    FakeCamera *camera = context;
    ++camera->close_calls;
    return camera->succeed && fd == FAKE_CAMERA_FD ? 0 : ENOSYS;
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
        MEISO_WIFI_TELEMETRY_MIN_PERIOD_MS,
        MEISO_WIFI_TELEMETRY_MAX_PERIOD_MS,
        MEISO_WIFI_TELEMETRY_PERIOD_STEP_MS
    };
    value.device_telemetry_capability = (MeisoDeviceTelemetryCapability){
        MEISO_DEVICE_TELEMETRY_FIELD_ALL,
        MEISO_DEVICE_TELEMETRY_MIN_PERIOD_MS,
        MEISO_DEVICE_TELEMETRY_MAX_PERIOD_MS,
        MEISO_DEVICE_TELEMETRY_PERIOD_STEP_MS,
        -40000, 125000,
        MEISO_DEVICE_TELEMETRY_LINUX_TEMPERATURE_UNCERTAINTY_MILLIC
    };
    value.render_media_capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        320U, 240U, 320U, 240U, 10000U,
        MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES,
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE, 0U
    };
    value.camera_media_capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        320U, 240U, 320U, 240U, 30000U,
        MEISO_CAMERA_V4L2_SIZE_IMAGE,
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE, 0U
    };
    return value;
}

static void configure_source(
    MeisoCompositionDebugPeerConfig *config,
    uint8_t index,
    MeisoRuntimeSchedulerWorkKind kind,
    uint8_t route
)
{
    config->sources[index].work_kind = kind;
    config->sources[index].route_index = route;
}

static void configure_host_sources(MeisoCompositionDebugPeerConfig *config)
{
    size_t index;

    configure_source(config, 0U, MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_ACCEPT,
                     MEISO_COMPOSITION_DEBUG_PEER_NO_ROUTE);
    configure_source(config, 1U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                     ROUTE_WIFI_DESIRED);
    configure_source(config, 2U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                     ROUTE_WIFI_ACTUAL);
    configure_source(config, 3U, MEISO_RUNTIME_SCHEDULER_WORK_BLOB_BEGIN,
                     ROUTE_MESH_BLOB);
    configure_source(config, 4U, MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK,
                     ROUTE_MESH_BLOB);
    configure_source(config, 5U, MEISO_RUNTIME_SCHEDULER_WORK_BLOB_COMMIT,
                     ROUTE_MESH_BLOB);
    configure_source(config, 6U, MEISO_RUNTIME_SCHEDULER_WORK_BLOB_ABORT,
                     ROUTE_MESH_BLOB);
    configure_source(config, 7U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                     ROUTE_PRESENTATION_DESIRED);
    configure_source(config, 8U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                     ROUTE_PRESENTATION_ACTUAL);
    configure_source(config, 9U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                     ROUTE_SCENE_DESIRED);
    configure_source(config, 10U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                     ROUTE_SCENE_ACTUAL);
    configure_source(config, 11U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                     ROUTE_DEVICE_DESIRED);
    configure_source(config, 12U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                     ROUTE_DEVICE_ACTUAL);
    configure_source(config, 13U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                     ROUTE_RENDER_DESIRED);
    configure_source(config, 14U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                     ROUTE_RENDER_ACTUAL);
    configure_source(config, 15U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
                     ROUTE_CAMERA_DESIRED);
    configure_source(config, 16U, MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
                     ROUTE_CAMERA_ACTUAL);
    for (index = 17U; index < MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT;
         ++index) {
        configure_source(
            config, (uint8_t)index,
            MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, ROUTE_WIFI_DESIRED
        );
    }
}

static MeisoCompositionDebugPeerConfig host_config(int fd, uint64_t salt)
{
    MeisoCompositionDebugPeerConfig value;

    memset(&value, 0, sizeof(value));
    value.connected_fd = fd;
    value.local_role = MEISO_SESSION_ROLE_HOST;
    value.local_peer_id = HOST_PEER;
    value.local_incarnation = 31U;
    value.local_nonce = (MeisoSessionNonce){UINT64_C(0x61), salt + 1U};
    value.expected_peer_id = EDGE_PEER;
    value.protocol = (MeisoProtocolVersion){1U, 0U};
    value.catalog_generation = 1U;
    value.catalog = catalog_config();
    value.max_io_bytes_per_step = 17U;
    value.adapter_reserve_bytes = 65536U;
    value.runtime_reserve_bytes = 400000U;
    value.scheduler_reserve_bytes = 65536U;
    configure_host_sources(&value);
    return value;
}

static MeisoCompositionEdgeAppConfig edge_config(
    EdgeFixtureShape *fixture,
    uint64_t salt
)
{
    MeisoCompositionEdgeAppConfig value;

    memset(&value, 0, sizeof(value));
    value.control_connected_fd = fixture->edge_fd;
    value.local_peer_id = EDGE_PEER;
    value.local_incarnation = 41U;
    value.local_nonce = (MeisoSessionNonce){UINT64_C(0x71), salt + 2U};
    value.expected_host_peer_id = HOST_PEER;
    value.protocol = (MeisoProtocolVersion){1U, 0U};
    value.catalog_generation = 1U;
    value.wifi_incarnation = 11U;
    value.presentation_incarnation = 12U;
    value.scene_incarnation = 13U;
    value.device_telemetry_incarnation = 14U;
    value.render_media_incarnation = 15U;
    value.camera_media_incarnation = 16U;
    (void)snprintf(
        value.wifi.interface_name, sizeof(value.wifi.interface_name), "wlan0"
    );
    value.wifi.allowed_fields = MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
        MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM;
    value.wifi.sys_class_net_root = fixture->wifi.sys_class_net_root;
    value.wifi.proc_net_wireless_path = fixture->wifi.proc_wireless_path;
    value.device_telemetry = fixture->device.config;
    value.camera = (MeisoCameraV4l2Config){
        "/dev/video-test", 77U, &fixture->camera_syscalls, &fixture->camera
    };
    value.edge_clock_epoch = 77U;
    value.max_control_io_bytes_per_step = 17U;
    value.control_adapter_reserve_bytes = 65536U;
    value.control_runtime_reserve_bytes = 400000U;
    value.control_scheduler_reserve_bytes = 65536U;
    value.initial_actual_delivery_generation =
        (MeisoDeliveryGeneration){101U};
    value.media_connection_slots =
        MEISO_COMPOSITION_EDGE_APP_MAX_MEDIA_CONNECTIONS;
    value.max_media_io_bytes_per_step = 17U;
    value.thermal_worker_budget_ms = 50U;
    return value;
}

static void fixture_destroy(EdgeFixtureShape *fixture)
{
    if (fixture != NULL) {
        close_fd(&fixture->host_fd);
        close_fd(&fixture->edge_fd);
        wifi_files_destroy(&fixture->wifi);
        device_files_destroy(&fixture->device);
        free(fixture->camera.storage);
        memset(fixture, 0, sizeof(*fixture));
        fixture->host_fd = -1;
        fixture->edge_fd = -1;
    }
}

static bool fixture_shape_create(EdgeFixtureShape *fixture, uint64_t salt)
{
    MeisoWifiLinuxAdapter wifi;
    MeisoWifiTelemetryFieldMask wifi_fields = 0U;
    MeisoDeviceTelemetryLinuxAdapter device;
    MeisoDeviceTelemetryCapability device_capability;

    memset(fixture, 0, sizeof(*fixture));
    fixture->host_fd = -1;
    fixture->edge_fd = -1;
    CHECK(make_tcp_pair(&fixture->host_fd, &fixture->edge_fd));
    CHECK(wifi_files_create(&fixture->wifi));
    CHECK(device_files_create(&fixture->device));
    fixture->camera_syscalls = (MeisoCameraV4l2Syscalls){
        fake_camera_open,
        fake_camera_ioctl,
        fake_camera_map,
        fake_camera_unmap,
        fake_camera_close
    };
    fixture->camera.storage_size =
        (size_t)MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT *
        MEISO_CAMERA_V4L2_SIZE_IMAGE;
    fixture->camera.storage = calloc(1U, fixture->camera.storage_size);
    fixture->camera.dequeue.index = 0U;
    fixture->camera.dequeue.length = MEISO_CAMERA_V4L2_SIZE_IMAGE;
    fixture->camera.dequeue.bytes_used = MEISO_CAMERA_V4L2_SIZE_IMAGE;
    fixture->camera.dequeue.driver_sequence = 1U;
    fixture->camera.dequeue.driver_sequence_present = true;
    fixture->camera.dequeue.timestamp_seconds = 1;
    fixture->camera.dequeue.timestamp_microseconds = 0;
    fixture->camera.dequeue.timestamp_monotonic = true;
    fixture->camera.dequeue.raw_field = MEISO_CAMERA_V4L2_RAW_FIELD_NONE;
    fixture->camera.dequeue.field_provenance =
        MEISO_CAMERA_V4L2_FIELD_EXPLICIT_PROGRESSIVE;
    CHECK(fixture->camera.storage != NULL);
    fixture->host_fd_flags = fcntl(fixture->host_fd, F_GETFL, 0);
    fixture->edge_fd_flags = fcntl(fixture->edge_fd, F_GETFL, 0);
    CHECK(fixture->host_fd_flags >= 0 && fixture->edge_fd_flags >= 0);
    fixture->edge_config = edge_config(fixture, salt);
    fixture->host_config = host_config(fixture->host_fd, salt);
    memset(&wifi, 0, sizeof(wifi));
    memset(&device, 0, sizeof(device));
    memset(&device_capability, 0, sizeof(device_capability));
    CHECK(meiso_wifi_linux_adapter_init(
              &wifi, &fixture->edge_config.wifi
          ) == MEISO_WIFI_LINUX_ADAPTER_OK &&
          meiso_wifi_linux_adapter_probe(&wifi, &wifi_fields) ==
              MEISO_WIFI_LINUX_ADAPTER_OK &&
          meiso_device_telemetry_linux_adapter_init(
              &device, &fixture->edge_config.device_telemetry
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK &&
          meiso_device_telemetry_linux_adapter_probe(
              &device, &device_capability
          ) == MEISO_DEVICE_TELEMETRY_LINUX_ADAPTER_OK);
    fixture->host_config.catalog.wifi_capability.supported_fields =
        wifi_fields;
    fixture->host_config.catalog.device_telemetry_capability =
        device_capability;
    CHECK(meiso_composition_session_v3_build_catalog(
        &fixture->host_config.catalog,
        (MeisoMutableBuffer){fixture->catalog.capability,
                             sizeof(fixture->catalog.capability)},
        fixture->catalog.endpoints,
        MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT,
        fixture->catalog.routes,
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
    ) == MEISO_COMPOSITION_SESSION_V3_OK);
    fixture->edge_bytes = meiso_composition_edge_app_memory_required(
        &fixture->edge_config
    );
    fixture->host_bytes = meiso_composition_debug_peer_memory_required(
        &fixture->host_config
    );
    CHECK(fixture->edge_bytes != 0U && fixture->host_bytes != 0U &&
          fcntl(fixture->host_fd, F_GETFL, 0) == fixture->host_fd_flags &&
          fcntl(fixture->edge_fd, F_GETFL, 0) == fixture->edge_fd_flags);
    return true;
}

static bool benign_host_status(MeisoCompositionDebugPeerPumpStatus status)
{
    return status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_PROGRESS ||
        status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_IDLE ||
        status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_EVENT ||
        status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_WOULD_BLOCK ||
        status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_TX_BUSY ||
        status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_REBIND_PROGRESS;
}

static bool benign_edge_status(MeisoCompositionEdgeAppPumpStatus status)
{
    return status == MEISO_COMPOSITION_EDGE_APP_PUMP_PROGRESS ||
        status == MEISO_COMPOSITION_EDGE_APP_PUMP_IDLE ||
        status == MEISO_COMPOSITION_EDGE_APP_PUMP_WOULD_BLOCK;
}

static bool route_key_equal(
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

static bool encode_wifi_desired(
    MeisoWifiTelemetryFieldMask selected_fields,
    uint32_t sample_period_ms,
    uint8_t output[MEISO_WIFI_TELEMETRY_DESIRED_STATE_MAX_PAYLOAD_SIZE],
    MeisoByteView *out_payload
)
{
    const MeisoWifiTelemetryDesiredParameters parameters = {
        selected_fields,
        sample_period_ms
    };
    uint8_t suffix[MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE];
    size_t suffix_size = 0U;
    MeisoRuntimeDesiredState desired;

    CHECK(meiso_wifi_telemetry_desired_parameters_encode(
              &parameters,
              (MeisoMutableBuffer){suffix, sizeof(suffix)},
              &suffix_size
          ) == MEISO_WIFI_TELEMETRY_CODEC_OK &&
          suffix_size == sizeof(suffix));
    desired.enabled = true;
    desired.profile = (MeisoByteView){suffix, suffix_size};
    CHECK(meiso_runtime_desired_state_encode(
        &desired,
        (MeisoMutableBuffer){
            output, MEISO_WIFI_TELEMETRY_DESIRED_STATE_MAX_PAYLOAD_SIZE
        },
        out_payload
    ));
    return true;
}

static MeisoCompositionMediaDesired test_media_desired(
    bool render,
    uint32_t fps_millihz
)
{
    MeisoCompositionMediaDesired desired;

    memset(&desired, 0, sizeof(desired));
    desired.pixel_format = render ?
        MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888 :
        MEISO_COMPOSITION_MEDIA_PIXEL_NV12;
    desired.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
    desired.width = 320U;
    desired.height = 240U;
    desired.fps_millihz = fps_millihz;
    desired.gop_frames = 1U;
    desired.recovery_flags = MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY;
    return desired;
}

static bool encode_media_desired(
    bool render,
    uint32_t fps_millihz,
    uint8_t output[MEISO_COMPOSITION_MEDIA_DESIRED_STATE_MAX_PAYLOAD_SIZE],
    MeisoByteView *out_payload,
    MeisoCompositionMediaDesired *out_desired
)
{
    uint8_t suffix[MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE];
    size_t suffix_size = 0U;
    MeisoRuntimeDesiredState desired;

    CHECK(output != NULL && out_payload != NULL && out_desired != NULL);
    *out_desired = test_media_desired(render, fps_millihz);
    CHECK(meiso_composition_media_desired_encode(
              out_desired, (MeisoMutableBuffer){suffix, sizeof(suffix)},
              &suffix_size
          ) == MEISO_COMPOSITION_SESSION_V3_OK &&
          suffix_size == sizeof(suffix));
    desired.enabled = true;
    desired.profile = (MeisoByteView){suffix, suffix_size};
    CHECK(meiso_runtime_desired_state_encode(
        &desired,
        (MeisoMutableBuffer){
            output, MEISO_COMPOSITION_MEDIA_DESIRED_STATE_MAX_PAYLOAD_SIZE
        },
        out_payload
    ));
    return true;
}

typedef uint8_t CompositionDesiredGoal;
enum {
    COMPOSITION_DESIRED_SCENE_OBSERVED = 1,
    COMPOSITION_DESIRED_PLAN_PREPARED = 2,
    COMPOSITION_DESIRED_PLAN_ACTIVE = 3,
    COMPOSITION_DESIRED_SCENE_ACTIVE = 4,
    COMPOSITION_DESIRED_PLAN_STOPPED = 5
};

static bool resolve_unrelated_actual_offer(
    MeisoCompositionDebugPeer *host,
    const MeisoRuntimeEvent *event
);

static bool encode_composition_desired(
    bool enabled,
    MeisoByteView suffix,
    uint8_t output[MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE],
    MeisoByteView *out_payload
)
{
    const MeisoRuntimeDesiredState desired = {enabled, suffix};

    CHECK(output != NULL && out_payload != NULL &&
          suffix.size <= MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE);
    return meiso_runtime_desired_state_encode(
        &desired,
        (MeisoMutableBuffer){
            output, MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE
        },
        out_payload
    );
}

static bool composition_goal_reached(
    const MeisoCompositionEdgeAppSnapshot *snapshot,
    CompositionDesiredGoal goal,
    uint64_t desired_version
)
{
    const bool scene_goal = goal == COMPOSITION_DESIRED_SCENE_OBSERVED ||
        goal == COMPOSITION_DESIRED_SCENE_ACTIVE;
    const size_t actual_index = scene_goal ?
        MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE :
        MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION;
    const MeisoCompositionEdgeAppActualSnapshot *actual;

    if (snapshot == NULL || desired_version == 0U) {
        return false;
    }
    actual = &snapshot->actual[actual_index];
    if (!actual->present || !actual->delivery_accepted ||
        actual->delivery_pending || !actual->applied_desired.present ||
        actual->applied_desired.owner.role != MEISO_OWNER_ROLE_HOST ||
        actual->applied_desired.version != desired_version) {
        return false;
    }
    switch (goal) {
    case COMPOSITION_DESIRED_SCENE_OBSERVED:
        return snapshot->scene_status_present;
    case COMPOSITION_DESIRED_PLAN_PREPARED:
        return snapshot->presentation_status_present &&
            snapshot->presentation_status.candidate_state ==
                MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED &&
            snapshot->presentation_status.prepared_token_present;
    case COMPOSITION_DESIRED_PLAN_ACTIVE:
        return snapshot->presentation_status_present &&
            snapshot->presentation_status.active_state ==
                MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE &&
            snapshot->active_world_present &&
            snapshot->renderer.complete_available &&
            snapshot->renderer.latest_complete_job.world
                .world_generation == snapshot->active_world.world_generation &&
            snapshot->renderer.latest_complete_job.world
                .desired_state_version ==
                    snapshot->active_world.desired_state_version;
    case COMPOSITION_DESIRED_SCENE_ACTIVE:
        return snapshot->scene_status_present &&
            snapshot->scene_status.reason ==
                MEISO_COMPOSITION_SCENE_REASON_READY &&
            snapshot->active_world_present &&
            snapshot->active_world.desired_owner_incarnation ==
                HOST_OWNER.incarnation &&
            snapshot->active_world.desired_state_version == desired_version &&
            snapshot->renderer.complete_available &&
            snapshot->renderer.latest_complete_job.world
                .world_generation == snapshot->active_world.world_generation &&
            snapshot->renderer.latest_complete_job.world
                .desired_state_version == desired_version;
    case COMPOSITION_DESIRED_PLAN_STOPPED:
        return !actual->effective_enabled &&
            actual->production_status == MEISO_RUNTIME_PRODUCTION_STOPPED &&
            actual->reason == MEISO_RUNTIME_PRODUCTION_REASON_DISABLED &&
            !snapshot->active_world_present;
    default:
        return false;
    }
}

static bool drive_composition_desired_actual(
    MeisoCompositionEdgeApp *edge,
    MeisoCompositionDebugPeer *host,
    const CatalogStorage *catalog,
    bool scene,
    bool enabled,
    uint64_t desired_version,
    MeisoByteView suffix,
    CompositionDesiredGoal goal,
    uint64_t now_base,
    MeisoCompositionEdgeAppSnapshot *out_snapshot
)
{
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const MeisoDeliveryGeneration desired_generation = {1U};
    const uint8_t desired_route_index = scene ?
        ROUTE_SCENE_DESIRED : ROUTE_PRESENTATION_DESIRED;
    const uint8_t actual_route_index = scene ?
        ROUTE_SCENE_ACTUAL : ROUTE_PRESENTATION_ACTUAL;
    const uint8_t desired_source = scene ? 9U : 7U;
    const uint8_t accept_source = scene ? 10U : 8U;
    MeisoStatePublisher *desired = NULL;
    MeisoStateReplica *actual = NULL;
    MeisoStateReplica *side_actual = NULL;
    void *desired_arena = NULL;
    void *actual_arena = NULL;
    void *side_actual_arena = NULL;
    size_t desired_bytes;
    size_t actual_bytes;
    size_t side_actual_bytes;
    uint8_t desired_wire[
        MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoByteView desired_payload;
    MeisoRuntimeRouteKey desired_route;
    MeisoRuntimeRouteKey actual_route;
    MeisoRuntimeRouteKey render_route;
    MeisoRuntimeRouteKey side_actual_route;
    MeisoRuntimeSchedulerWork work;
    bool desired_accept_seen = false;
    bool desired_completion_seen = false;
    bool edge_desired_completion_seen = false;
    uint64_t actual_offers_seen = 0U;
    uint64_t actual_accept_completions = 0U;
    bool actual_accept_pending = false;
    bool side_actual_accept_pending = false;
    bool matching_render_seen = false;
    bool edge_render_completion_seen = false;
    bool success = false;
    size_t step;

    CHECK(edge != NULL && host != NULL && catalog != NULL &&
          out_snapshot != NULL && desired_version != 0U);
    desired_bytes = meiso_state_publisher_memory_required(
        &catalog->routes[desired_route_index].route.resource, &state_config
    );
    actual_bytes = meiso_state_replica_memory_required(
        &catalog->routes[actual_route_index].route.resource
    );
    side_actual_bytes = meiso_state_replica_memory_required(
        &catalog->routes[scene ? ROUTE_PRESENTATION_ACTUAL :
                               ROUTE_SCENE_ACTUAL].route.resource
    );
    desired_arena = calloc(1U, desired_bytes);
    actual_arena = calloc(1U, actual_bytes);
    side_actual_arena = calloc(1U, side_actual_bytes);
    CHECK_GOTO(desired_bytes != 0U && actual_bytes != 0U &&
          side_actual_bytes != 0U && desired_arena != NULL &&
          actual_arena != NULL && side_actual_arena != NULL &&
          meiso_state_publisher_init(
              desired_arena, desired_bytes,
              &catalog->routes[desired_route_index].route.resource,
              &state_config, &desired
          ) == MEISO_STATE_INIT_OK &&
          meiso_state_replica_init(
              actual_arena, actual_bytes,
              &catalog->routes[actual_route_index].route.resource, &actual
          ) == MEISO_STATE_INIT_OK &&
          meiso_state_replica_init(
              side_actual_arena, side_actual_bytes,
              &catalog->routes[scene ? ROUTE_PRESENTATION_ACTUAL :
                                       ROUTE_SCENE_ACTUAL].route.resource,
              &side_actual
          ) == MEISO_STATE_INIT_OK &&
          meiso_state_publisher_begin_delivery(
              desired, desired_generation
          ) == MEISO_STATE_DELIVERY_STARTED &&
          meiso_state_replica_begin_owner(actual, EDGE_OWNER) ==
              MEISO_STATE_OWNER_STARTED &&
          meiso_state_replica_begin_owner(side_actual, EDGE_OWNER) ==
              MEISO_STATE_OWNER_STARTED &&
          encode_composition_desired(
              enabled, suffix, desired_wire, &desired_payload
          ) &&
          meiso_state_publisher_publish(
              desired, HOST_OWNER, desired_version, desired_payload
          ) == MEISO_STATE_PUBLISH_NEW &&
          meiso_composition_debug_peer_route_key(
              host, desired_route_index, &desired_route
          ) &&
          meiso_composition_debug_peer_route_key(
              host, actual_route_index, &actual_route
          ) &&
          meiso_composition_debug_peer_route_key(
              host, ROUTE_RENDER_RESULT, &render_route
          ) &&
          meiso_composition_debug_peer_route_key(
              host, scene ? ROUTE_PRESENTATION_ACTUAL : ROUTE_SCENE_ACTUAL,
              &side_actual_route
          ), cleanup);
    {
        MeisoStateOffer offer;

        CHECK_GOTO(meiso_state_publisher_next_offer(desired, &offer), cleanup);
        memset(&work, 0, sizeof(work));
        work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
        work.value.state_offer.route = desired_route;
        work.value.state_offer.offer = offer;
        CHECK_GOTO(meiso_composition_debug_peer_queue_work(
              host, desired_source, &work
            ) == MEISO_COMPOSITION_DEBUG_PEER_OK, cleanup);
    }
    for (step = 0U; step < 60000U; ++step) {
        MeisoCompositionDebugPeerPumpResult host_result;
        MeisoCompositionEdgeAppPumpResult edge_result;
        MeisoCompositionEdgeAppIoReady io = {
            true, true, false, false, false,
            {77U, now_base + (uint64_t)step * UINT64_C(1000000)}
        };

        memset(&host_result, 0, sizeof(host_result));
        {
            const MeisoCompositionDebugPeerPumpStatus host_status =
                meiso_composition_debug_peer_pump(
                    host, true, true, &host_result
                );

            if (!benign_host_status(host_status)) {
                MeisoCompositionDebugPeerSnapshot peer_snapshot;

                memset(&peer_snapshot, 0, sizeof(peer_snapshot));
                (void)meiso_composition_debug_peer_snapshot(
                    host, &peer_snapshot
                );
                (void)fprintf(
                    stderr,
                    "composition host status=%u scene=%u v=%llu step=%zu "
                    "phase=%u obligation=%u result_obligation=%u\n",
                    (unsigned)host_status, scene ? 1U : 0U,
                    (unsigned long long)desired_version, step,
                    (unsigned)peer_snapshot.phase,
                    (unsigned)peer_snapshot.scheduler.rx_obligation,
                    (unsigned)host_result.rx_obligation
                );
            }
            CHECK_GOTO(benign_host_status(host_status), cleanup);
        }
        if (host_result.completed_source_present) {
            MeisoRuntimeSchedulerCompletion completion;

            memset(&completion, 0, sizeof(completion));
            CHECK_GOTO(meiso_composition_debug_peer_take_completion(
                  host, host_result.completed_source_index, &completion
                ) == MEISO_COMPOSITION_DEBUG_PEER_OK &&
                  completion.code ==
                      MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT,
                  cleanup);
            if (host_result.completed_source_index == desired_source) {
                desired_completion_seen = true;
            } else if (host_result.completed_source_index == accept_source) {
                ++actual_accept_completions;
                actual_accept_pending = false;
            } else if (host_result.completed_source_index ==
                       (scene ? 8U : 10U)) {
                side_actual_accept_pending = false;
            }
        }
        if (host_result.event_present) {
            const MeisoRuntimeEvent *event = &host_result.event;

            if (event->type == MEISO_RUNTIME_EVENT_STATE_ACCEPT &&
                route_key_equal(event->route, desired_route)) {
                const MeisoStateAcceptStatus accepted =
                    meiso_state_publisher_mark_delivery_accepted(
                        desired, event->state_accept_generation,
                        event->state_accept_owner,
                        event->state_accept_version
                    );

                CHECK_GOTO(accepted == MEISO_STATE_ACCEPT_CONFIRMED ||
                      accepted == MEISO_STATE_ACCEPT_DUPLICATE, cleanup);
                desired_accept_seen = true;
            } else if (event->type == MEISO_RUNTIME_EVENT_STATE_OFFER &&
                       route_key_equal(event->route, actual_route)) {
                const MeisoStateReceiveStatus receive =
                    meiso_state_replica_receive(
                        actual, event->state_offer.state.owner,
                        event->state_offer.state.version,
                        event->state_offer.state.payload
                    );

                CHECK_GOTO(!actual_accept_pending &&
                      (receive == MEISO_STATE_RECEIVE_NEW ||
                       receive == MEISO_STATE_RECEIVE_DUPLICATE) &&
                      meiso_composition_debug_peer_commit_state_receive(
                          host, event->route, &event->state_offer, receive
                      ) == MEISO_COMPOSITION_DEBUG_PEER_OK,
                      cleanup);
                memset(&work, 0, sizeof(work));
                work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
                work.value.state_accept.route = event->route;
                work.value.state_accept.generation =
                    event->state_offer.generation;
                work.value.state_accept.owner =
                    event->state_offer.state.owner;
                work.value.state_accept.version =
                    event->state_offer.state.version;
                CHECK_GOTO(meiso_composition_debug_peer_queue_work(
                      host, accept_source, &work
                    ) == MEISO_COMPOSITION_DEBUG_PEER_OK, cleanup);
                actual_accept_pending = true;
                ++actual_offers_seen;
            } else if (event->type == MEISO_RUNTIME_EVENT_STATE_OFFER &&
                       route_key_equal(event->route, side_actual_route)) {
                const MeisoStateReceiveStatus receive =
                    meiso_state_replica_receive(
                        side_actual, event->state_offer.state.owner,
                        event->state_offer.state.version,
                        event->state_offer.state.payload
                    );

                CHECK_GOTO(!side_actual_accept_pending &&
                      (receive == MEISO_STATE_RECEIVE_NEW ||
                       receive == MEISO_STATE_RECEIVE_DUPLICATE) &&
                      meiso_composition_debug_peer_commit_state_receive(
                          host, event->route, &event->state_offer, receive
                      ) == MEISO_COMPOSITION_DEBUG_PEER_OK,
                      cleanup);
                memset(&work, 0, sizeof(work));
                work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
                work.value.state_accept.route = event->route;
                work.value.state_accept.generation =
                    event->state_offer.generation;
                work.value.state_accept.owner =
                    event->state_offer.state.owner;
                work.value.state_accept.version =
                    event->state_offer.state.version;
                CHECK_GOTO(meiso_composition_debug_peer_queue_work(
                      host, scene ? 8U : 10U, &work
                    ) == MEISO_COMPOSITION_DEBUG_PEER_OK, cleanup);
                side_actual_accept_pending = true;
            } else if (event->type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE &&
                       route_key_equal(event->route, render_route)) {
                MeisoCompositionSceneRenderResult render_result;
                const uint64_t expected_scene_version =
                    goal == COMPOSITION_DESIRED_PLAN_ACTIVE ?
                        1U : desired_version;

                CHECK_GOTO(meiso_composition_scene_render_result_decode(
                      event->stream_sample.payload, &render_result
                    ) == MEISO_COMPOSITION_SCENE_CODEC_OK, cleanup);
                if (render_result.applied_scene_owner_incarnation ==
                        HOST_OWNER.incarnation &&
                    render_result.applied_scene_state_version ==
                        expected_scene_version) {
                    matching_render_seen = true;
                }
            } else if (event->type == MEISO_RUNTIME_EVENT_STATE_OFFER) {
                CHECK_GOTO(resolve_unrelated_actual_offer(host, event),
                           cleanup);
            }
        }
        memset(&edge_result, 0, sizeof(edge_result));
        {
            const MeisoCompositionEdgeAppPumpStatus edge_status =
                meiso_composition_edge_app_pump(edge, &io, &edge_result);

            if (!benign_edge_status(edge_status)) {
                (void)meiso_composition_edge_app_snapshot(edge, out_snapshot);
                (void)fprintf(
                    stderr,
                    "composition edge status=%u scene=%u v=%llu step=%zu "
                    "phase=%u terminal=%u\n",
                    (unsigned)edge_status, scene ? 1U : 0U,
                    (unsigned long long)desired_version, step,
                    (unsigned)out_snapshot->phase,
                    (unsigned)out_snapshot->terminal_reason
                );
            }
            CHECK_GOTO(benign_edge_status(edge_status) &&
                  meiso_composition_edge_app_snapshot(edge, out_snapshot),
                  cleanup);
        }
        if (edge_result.event_present &&
            edge_result.event ==
                MEISO_COMPOSITION_EDGE_APP_PUMP_CONTROL_PROGRESS &&
            edge_result.desired_channel_present &&
            edge_result.desired_channel == (scene ?
                MEISO_COMPOSITION_EDGE_APP_DESIRED_SCENE :
                MEISO_COMPOSITION_EDGE_APP_DESIRED_PRESENTATION)) {
            edge_desired_completion_seen = true;
        }
        if (edge_result.event_present &&
            edge_result.event ==
                MEISO_COMPOSITION_EDGE_APP_PUMP_INLINE_SAMPLE &&
            edge_result.inline_channel_present &&
            edge_result.inline_channel ==
                MEISO_COMPOSITION_EDGE_APP_INLINE_RENDER_RESULT) {
            edge_render_completion_seen = true;
        }
        if (desired_accept_seen && desired_completion_seen &&
            edge_desired_completion_seen &&
            actual_offers_seen != 0U && actual_accept_completions != 0U &&
            !actual_accept_pending && !side_actual_accept_pending &&
            ((goal != COMPOSITION_DESIRED_PLAN_ACTIVE &&
              goal != COMPOSITION_DESIRED_SCENE_ACTIVE) ||
             (out_snapshot->actual[scene ?
                  MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION :
                  MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE]
                  .delivery_accepted &&
              !out_snapshot->actual[scene ?
                  MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION :
                  MEISO_COMPOSITION_EDGE_APP_ACTUAL_SCENE]
                  .delivery_pending)) &&
            ((goal != COMPOSITION_DESIRED_PLAN_ACTIVE &&
             goal != COMPOSITION_DESIRED_SCENE_ACTIVE) ||
             (matching_render_seen && edge_render_completion_seen &&
              out_snapshot->inline_streams[
                  MEISO_COMPOSITION_EDGE_APP_INLINE_RENDER_RESULT
              ].queue_depth == 0U)) &&
            composition_goal_reached(
                out_snapshot, goal, desired_version
            )) {
            success = true;
            break;
        }
    }
    if (!success) {
        (void)fprintf(
            stderr,
            "composition desired timeout scene=%u enabled=%u version=%llu "
            "goal=%u desired_accept=%u desired_complete=%u actual=%llu/%llu "
            "edge_desired_complete=%u pending=%u phase=%u active_world=%u\n",
            scene ? 1U : 0U, enabled ? 1U : 0U,
            (unsigned long long)desired_version, (unsigned)goal,
            desired_accept_seen ? 1U : 0U,
            desired_completion_seen ? 1U : 0U,
            (unsigned long long)actual_offers_seen,
            (unsigned long long)actual_accept_completions,
            edge_desired_completion_seen ? 1U : 0U,
            actual_accept_pending ? 1U : 0U,
            (unsigned)out_snapshot->phase,
            out_snapshot->active_world_present ? 1U : 0U
        );
    }

cleanup:
    free(side_actual_arena);
    free(actual_arena);
    free(desired_arena);
    return success;
}

typedef struct {
    MeisoCompositionMediaActual actual;
    MeisoCompositionEdgeAppActualSnapshot snapshot;
} MediaActualResult;

typedef struct {
    void *edge_arena;
    size_t edge_bytes;
    int media_edge_fd;
    bool checked;
} MediaPreacceptGate;

static bool resolve_unrelated_actual_offer(
    MeisoCompositionDebugPeer *host,
    const MeisoRuntimeEvent *event
)
{
    static const uint8_t actual_routes[] = {
        ROUTE_WIFI_ACTUAL,
        ROUTE_PRESENTATION_ACTUAL,
        ROUTE_SCENE_ACTUAL,
        ROUTE_DEVICE_ACTUAL,
        ROUTE_RENDER_ACTUAL,
        ROUTE_CAMERA_ACTUAL
    };
    static const uint8_t accept_sources[] = {2U, 8U, 10U, 12U, 14U, 16U};
    MeisoRuntimeSchedulerWork work;
    size_t index;

    if (host == NULL || event == NULL ||
        event->type != MEISO_RUNTIME_EVENT_STATE_OFFER) {
        return false;
    }
    for (index = 0U; index < sizeof(actual_routes); ++index) {
        MeisoRuntimeRouteKey route;

        if (!meiso_composition_debug_peer_route_key(
                host, actual_routes[index], &route
            ) || !route_key_equal(event->route, route)) {
            continue;
        }
        if (meiso_composition_debug_peer_commit_state_receive(
                host, event->route, &event->state_offer,
                MEISO_STATE_RECEIVE_STALE
            ) != MEISO_COMPOSITION_DEBUG_PEER_OK) {
            return false;
        }
        memset(&work, 0, sizeof(work));
        work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
        work.value.state_accept.route = event->route;
        work.value.state_accept.generation = event->state_offer.generation;
        work.value.state_accept.owner = event->state_offer.state.owner;
        work.value.state_accept.version = event->state_offer.state.version;
        return meiso_composition_debug_peer_queue_work(
            host, accept_sources[index], &work
        ) == MEISO_COMPOSITION_DEBUG_PEER_OK;
    }
    return false;
}

static bool check_media_preaccept_gate(
    MeisoCompositionEdgeApp *edge,
    MediaPreacceptGate *gate
)
{
    MeisoCompositionEdgeAppMediaBindReceipt receipt;
    uint8_t *before = NULL;
    int flags_before;
    bool ok = false;

    CHECK(edge != NULL && gate != NULL && gate->edge_arena != NULL &&
          gate->edge_bytes != 0U && gate->media_edge_fd >= 0 &&
          !gate->checked);
    before = malloc(gate->edge_bytes);
    CHECK(before != NULL);
    memcpy(before, gate->edge_arena, gate->edge_bytes);
    flags_before = fcntl(gate->media_edge_fd, F_GETFL, 0);
    memset(&receipt, 0xa5, sizeof(receipt));
    ok = flags_before >= 0 &&
        meiso_composition_edge_app_bind_media_fd(
            edge, gate->media_edge_fd, &receipt
        ) == MEISO_COMPOSITION_EDGE_APP_NOT_READY &&
        memcmp(before, gate->edge_arena, gate->edge_bytes) == 0 &&
        fcntl(gate->media_edge_fd, F_GETFL, 0) == flags_before;
    if (ok) {
        const uint8_t *bytes = (const uint8_t *)(const void *)&receipt;
        size_t index;

        for (index = 0U; index < sizeof(receipt); ++index) {
            if (bytes[index] != UINT8_C(0xa5)) {
                ok = false;
                break;
            }
        }
    }
    free(before);
    gate->checked = ok;
    return ok;
}

static bool drive_media_desired_actual(
    MeisoCompositionEdgeApp *edge,
    MeisoCompositionDebugPeer *host,
    const CatalogStorage *catalog,
    bool render,
    uint64_t desired_version,
    uint32_t fps_millihz,
    MediaPreacceptGate *preaccept,
    MediaActualResult *out_result
)
{
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const MeisoDeliveryGeneration desired_generation = {1U};
    const uint8_t desired_route_index = render ?
        ROUTE_RENDER_DESIRED : ROUTE_CAMERA_DESIRED;
    const uint8_t actual_route_index = render ?
        ROUTE_RENDER_ACTUAL : ROUTE_CAMERA_ACTUAL;
    const uint8_t desired_source = render ? 13U : 15U;
    const uint8_t accept_source = render ? 14U : 16U;
    const size_t actual_index = render ?
        MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA :
        MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA;
    MeisoStatePublisher *desired = NULL;
    MeisoStateReplica *actual = NULL;
    void *desired_arena = NULL;
    void *actual_arena = NULL;
    size_t desired_bytes;
    size_t actual_bytes;
    uint8_t desired_wire[
        MEISO_COMPOSITION_MEDIA_DESIRED_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoByteView desired_payload;
    MeisoCompositionMediaDesired desired_value;
    MeisoRuntimeRouteKey desired_route;
    MeisoRuntimeRouteKey actual_route;
    MeisoRuntimeSchedulerWork work;
    bool desired_accept_seen = false;
    bool desired_completion_seen = false;
    bool actual_offer_seen = false;
    bool actual_accept_completion_seen = false;
    uint32_t edge_actual_events = 0U;
    bool success = false;
    size_t step;

    CHECK(edge != NULL && host != NULL && catalog != NULL &&
          out_result != NULL && desired_version != 0U);
    memset(out_result, 0, sizeof(*out_result));
    desired_bytes = meiso_state_publisher_memory_required(
        &catalog->routes[desired_route_index].route.resource, &state_config
    );
    actual_bytes = meiso_state_replica_memory_required(
        &catalog->routes[actual_route_index].route.resource
    );
    desired_arena = calloc(1U, desired_bytes);
    actual_arena = calloc(1U, actual_bytes);
    CHECK_GOTO(desired_bytes != 0U && actual_bytes != 0U &&
          desired_arena != NULL && actual_arena != NULL, cleanup);
    CHECK_GOTO(meiso_state_publisher_init(
          desired_arena, desired_bytes,
          &catalog->routes[desired_route_index].route.resource,
          &state_config, &desired
        ) == MEISO_STATE_INIT_OK &&
          meiso_state_replica_init(
              actual_arena, actual_bytes,
              &catalog->routes[actual_route_index].route.resource, &actual
          ) == MEISO_STATE_INIT_OK &&
          meiso_state_publisher_begin_delivery(
              desired, desired_generation
          ) == MEISO_STATE_DELIVERY_STARTED &&
          meiso_state_replica_begin_owner(actual, EDGE_OWNER) ==
              MEISO_STATE_OWNER_STARTED &&
          encode_media_desired(
              render, fps_millihz, desired_wire, &desired_payload,
              &desired_value
          ) &&
          meiso_state_publisher_publish(
              desired, HOST_OWNER, desired_version, desired_payload
          ) == MEISO_STATE_PUBLISH_NEW &&
          meiso_composition_debug_peer_route_key(
              host, desired_route_index, &desired_route
          ) &&
          meiso_composition_debug_peer_route_key(
              host, actual_route_index, &actual_route
          ), cleanup);
    {
        MeisoStateOffer offer;

        CHECK_GOTO(meiso_state_publisher_next_offer(desired, &offer), cleanup);
        memset(&work, 0, sizeof(work));
        work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
        work.value.state_offer.route = desired_route;
        work.value.state_offer.offer = offer;
        CHECK_GOTO(meiso_composition_debug_peer_queue_work(
              host, desired_source, &work
            ) == MEISO_COMPOSITION_DEBUG_PEER_OK, cleanup);
    }
    for (step = 0U; step < 40000U; ++step) {
        MeisoCompositionDebugPeerPumpResult host_result;
        MeisoCompositionEdgeAppPumpResult edge_result;
        MeisoCompositionEdgeAppSnapshot snapshot;
        MeisoCompositionEdgeAppIoReady io = {
            true, true, false, false, false,
            {77U, UINT64_C(500000) + step}
        };

        memset(&host_result, 0, sizeof(host_result));
        {
            const MeisoCompositionDebugPeerPumpStatus host_status =
                meiso_composition_debug_peer_pump(
                    host, true, true, &host_result
                );
            if (!benign_host_status(host_status)) {
                MeisoCompositionDebugPeerSnapshot peer_snapshot;

                memset(&peer_snapshot, 0, sizeof(peer_snapshot));
                (void)meiso_composition_debug_peer_snapshot(
                    host, &peer_snapshot
                );
                (void)fprintf(
                    stderr,
                    "media desired host status=%u render=%u step=%zu "
                    "event=%u present=%u obligation=%u phase=%u\n",
                    (unsigned)host_status, render ? 1U : 0U, step,
                    (unsigned)host_result.event.type,
                    host_result.event_present ? 1U : 0U,
                    (unsigned)peer_snapshot.scheduler.rx_obligation,
                    (unsigned)peer_snapshot.phase
                );
            }
            CHECK_GOTO(benign_host_status(host_status), cleanup);
        }
        if (host_result.completed_source_present) {
            MeisoRuntimeSchedulerCompletion completion;

            memset(&completion, 0, sizeof(completion));
            CHECK_GOTO(meiso_composition_debug_peer_take_completion(
                  host, host_result.completed_source_index, &completion
                ) == MEISO_COMPOSITION_DEBUG_PEER_OK &&
                  completion.code ==
                      MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT,
                  cleanup);
            if (host_result.completed_source_index == desired_source) {
                desired_completion_seen = true;
            } else if (host_result.completed_source_index == accept_source) {
                actual_accept_completion_seen = true;
            }
        }
        if (host_result.event_present) {
            const MeisoRuntimeEvent *event = &host_result.event;

            if (event->type == MEISO_RUNTIME_EVENT_STATE_ACCEPT &&
                route_key_equal(event->route, desired_route)) {
                CHECK_GOTO(meiso_state_publisher_mark_delivery_accepted(
                      desired, event->state_accept_generation,
                      event->state_accept_owner,
                      event->state_accept_version
                    ) == MEISO_STATE_ACCEPT_CONFIRMED, cleanup);
                desired_accept_seen = true;
            } else if (event->type == MEISO_RUNTIME_EVENT_STATE_OFFER &&
                       route_key_equal(event->route, actual_route)) {
                MeisoRuntimeActualState actual_value;
                MeisoCompositionMediaActual media_actual;
                MeisoStateReceiveStatus receive;

                CHECK_GOTO(!actual_offer_seen &&
                      event->state_offer.state.owner.role ==
                          MEISO_OWNER_ROLE_EDGE &&
                      meiso_runtime_actual_state_decode(
                          event->state_offer.state.payload, &actual_value
                      ) && actual_value.observed_desired.present &&
                      actual_value.observed_desired.owner.role ==
                          MEISO_OWNER_ROLE_HOST &&
                      actual_value.observed_desired.version ==
                          desired_version &&
                      actual_value.applied_desired.present &&
                      actual_value.applied_desired.version ==
                          desired_version &&
                      actual_value.apply_result == MEISO_RUNTIME_APPLY_APPLIED &&
                      actual_value.effective_enabled &&
                      actual_value.production_status ==
                          MEISO_RUNTIME_PRODUCTION_RUNNING &&
                      actual_value.production_epoch != 0U &&
                      meiso_composition_media_actual_decode(
                          actual_value.profile, &media_actual
                      ) == MEISO_COMPOSITION_SESSION_V3_OK &&
                      media_actual.source == (render ?
                          MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW :
                          MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA) &&
                      media_actual.effective.pixel_format ==
                          desired_value.pixel_format &&
                      media_actual.effective.codec == desired_value.codec &&
                      media_actual.effective.width == desired_value.width &&
                      media_actual.effective.height == desired_value.height &&
                      media_actual.effective.fps_millihz ==
                          desired_value.fps_millihz,
                      cleanup);
                receive = meiso_state_replica_receive(
                    actual, event->state_offer.state.owner,
                    event->state_offer.state.version,
                    event->state_offer.state.payload
                );
                CHECK_GOTO(receive == MEISO_STATE_RECEIVE_NEW &&
                      meiso_composition_debug_peer_commit_state_receive(
                          host, event->route, &event->state_offer, receive
                      ) == MEISO_COMPOSITION_DEBUG_PEER_OK,
                      cleanup);
                if (preaccept != NULL && !preaccept->checked) {
                    CHECK_GOTO(check_media_preaccept_gate(edge, preaccept),
                               cleanup);
                }
                memset(&work, 0, sizeof(work));
                work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
                work.value.state_accept.route = event->route;
                work.value.state_accept.generation =
                    event->state_offer.generation;
                work.value.state_accept.owner =
                    event->state_offer.state.owner;
                work.value.state_accept.version =
                    event->state_offer.state.version;
                CHECK_GOTO(meiso_composition_debug_peer_queue_work(
                      host, accept_source, &work
                    ) == MEISO_COMPOSITION_DEBUG_PEER_OK,
                    cleanup);
                out_result->actual = media_actual;
                actual_offer_seen = true;
            } else if (event->type == MEISO_RUNTIME_EVENT_STATE_OFFER) {
                CHECK_GOTO(resolve_unrelated_actual_offer(host, event),
                           cleanup);
            }
        }
        memset(&edge_result, 0, sizeof(edge_result));
        CHECK_GOTO(benign_edge_status(meiso_composition_edge_app_pump(
              edge, &io, &edge_result
            )) && meiso_composition_edge_app_snapshot(edge, &snapshot),
            cleanup);
        if (edge_result.event_present && edge_result.event ==
                MEISO_COMPOSITION_EDGE_APP_PUMP_ACTUAL_DELIVERY &&
            edge_result.actual_channel_present &&
            edge_result.actual_channel == actual_index) {
            ++edge_actual_events;
        }
        if (desired_accept_seen && desired_completion_seen &&
            actual_offer_seen && actual_accept_completion_seen &&
            snapshot.actual[actual_index].delivery_accepted &&
            !snapshot.actual[actual_index].delivery_pending) {
            out_result->snapshot = snapshot.actual[actual_index];
            success = true;
            break;
        }
    }
    if (!success) {
        MeisoCompositionEdgeAppSnapshot final_snapshot;

        memset(&final_snapshot, 0, sizeof(final_snapshot));
        (void)meiso_composition_edge_app_snapshot(edge, &final_snapshot);
        (void)fprintf(
            stderr,
            "media desired timeout render=%u v=%llu accept=%u complete=%u "
            "offer=%u ackcomplete=%u edgeaccepted=%u pending=%u actualv=%llu "
            "edgeevents=%u\n",
            render ? 1U : 0U, (unsigned long long)desired_version,
            desired_accept_seen ? 1U : 0U,
            desired_completion_seen ? 1U : 0U,
            actual_offer_seen ? 1U : 0U,
            actual_accept_completion_seen ? 1U : 0U,
            final_snapshot.actual[actual_index].delivery_accepted ? 1U : 0U,
            final_snapshot.actual[actual_index].delivery_pending ? 1U : 0U,
            (unsigned long long)final_snapshot.actual[actual_index]
                .actual_state_version,
            edge_actual_events
        );
    }
cleanup:
    free(actual_arena);
    free(desired_arena);
    return success;
}

typedef struct {
    void *transport_arena;
    size_t transport_bytes;
    MeisoCompositionMediaTransport *transport;
    void *adapter_arena;
    size_t adapter_bytes;
    MeisoCompositionMediaPosixTcp *adapter;
    MeisoCompositionMediaTransportConfig config;
} HostMediaFixture;

static void destroy_host_media(HostMediaFixture *media)
{
    if (media != NULL) {
        free(media->adapter_arena);
        free(media->transport_arena);
        memset(media, 0, sizeof(*media));
    }
}

static bool initialize_host_media(
    const EdgeFixtureShape *fixture,
    const MeisoCompositionEdgeAppSnapshot *edge_snapshot,
    int media_host_fd,
    const MediaActualResult *render,
    const MediaActualResult *camera,
    HostMediaFixture *media
)
{
    const MediaActualResult *actuals[2] = {render, camera};
    const uint8_t route_indices[2] = {
        ROUTE_RENDER_EXTERNAL, ROUTE_CAMERA_EXTERNAL
    };
    size_t alignment = 0U;
    size_t index;

    CHECK(fixture != NULL && edge_snapshot != NULL && media_host_fd >= 0 &&
          render != NULL && camera != NULL && media != NULL);
    memset(media, 0, sizeof(*media));
    media->config.local_role = MEISO_SESSION_ROLE_HOST;
    media->config.active_route_mask =
        MEISO_COMPOSITION_EDGE_APP_MEDIA_ALL;
    media->config.scope.session_id = edge_snapshot->control_session_id;
    media->config.scope.catalog_generation =
        edge_snapshot->active_catalog_generation;
    for (index = 0U; index < 2U; ++index) {
        const MeisoCompositionSessionV3RouteDescriptor *descriptor =
            &fixture->catalog.routes[route_indices[index]];
        MeisoCompositionMediaRouteConfig *route =
            &media->config.routes[index];

        route->capability = index == 0U ?
            fixture->host_config.catalog.render_media_capability :
            fixture->host_config.catalog.camera_media_capability;
        route->binding.session_id = edge_snapshot->control_session_id;
        route->binding.catalog_generation =
            edge_snapshot->active_catalog_generation;
        route->binding.endpoint_id = descriptor->route.endpoint_id;
        route->binding.endpoint_incarnation =
            descriptor->route.endpoint_incarnation;
        route->binding.resource_id = descriptor->route.resource.id;
        route->binding.source = actuals[index]->actual.source;
        route->binding.production_epoch =
            actuals[index]->snapshot.production_epoch;
        route->binding.media_stream_epoch =
            actuals[index]->actual.media_stream_epoch;
        route->binding.channel_nonce = actuals[index]->actual.channel_nonce;
        route->effective = actuals[index]->actual.effective;
    }
    CHECK(meiso_composition_media_transport_memory_required(
              &media->config, &media->transport_bytes, &alignment
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK &&
          media->transport_bytes != 0U &&
          alignment == alignof(max_align_t));
    media->transport_arena = calloc(1U, media->transport_bytes);
    CHECK(media->transport_arena != NULL &&
          meiso_composition_media_transport_init(
              &media->config,
              (MeisoMutableBuffer){
                  media->transport_arena, media->transport_bytes
              },
              &media->transport
          ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
    media->adapter_bytes =
        meiso_composition_media_posix_tcp_memory_required(
            &(MeisoCompositionMediaPosixTcpConfig){
                media_host_fd, media->transport, media->config.scope, 17U
            }
        );
    media->adapter_arena = calloc(1U, media->adapter_bytes);
    CHECK(media->adapter_bytes != 0U && media->adapter_arena != NULL &&
          meiso_composition_media_posix_tcp_init(
              media->adapter_arena, media->adapter_bytes,
              &(MeisoCompositionMediaPosixTcpConfig){
                  media_host_fd, media->transport, media->config.scope, 17U
              },
              &media->adapter
          ) == MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_OK);
    return true;
}

static bool host_media_step(HostMediaFixture *media, int media_host_fd)
{
    MeisoCompositionMediaPosixTcpPrepareStatus prepare;

    CHECK(media != NULL && media->adapter != NULL && media_host_fd >= 0);
    prepare = meiso_composition_media_posix_tcp_prepare_outbound(
        media->adapter
    );
    CHECK(prepare == MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_READY ||
          prepare == MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_IDLE);
    if (meiso_composition_media_posix_tcp_wants_write(media->adapter) &&
        fd_ready_now(media_host_fd, POLLOUT)) {
        const MeisoCompositionMediaPosixTcpWriteStatus status =
            meiso_composition_media_posix_tcp_on_writable(media->adapter);
        CHECK(status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PROGRESS ||
              status ==
                  MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PACKET_SENT ||
              status ==
                  MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_WOULD_BLOCK ||
              status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_IDLE);
    }
    if (meiso_composition_media_posix_tcp_wants_read(media->adapter) &&
        fd_ready_now(media_host_fd, POLLIN)) {
        const MeisoCompositionMediaPosixTcpReadStatus status =
            meiso_composition_media_posix_tcp_on_readable(media->adapter);
        CHECK(status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PROGRESS ||
              status ==
                  MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PACKET_ACCEPTED ||
              status ==
                  MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_FRAGMENT_ACCEPTED ||
              status ==
                  MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_FRAME_PUBLISHED ||
              status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_WOULD_BLOCK);
    }
    return true;
}

static bool drive_both_media_active(
    MeisoCompositionEdgeApp *edge,
    void *edge_arena,
    size_t edge_bytes,
    int media_edge_fd,
    int media_host_fd,
    HostMediaFixture *host_media,
    uint8_t expected_slot,
    MeisoCompositionEdgeAppMediaBindReceipt *out_receipt
)
{
    MeisoCompositionEdgeAppMediaBindReceipt receipt;
    size_t step;

    CHECK(edge != NULL && edge_arena != NULL && edge_bytes != 0U &&
          media_edge_fd >= 0 && media_host_fd >= 0 && host_media != NULL &&
          out_receipt != NULL);
    memset(&receipt, 0, sizeof(receipt));
    CHECK(meiso_composition_edge_app_bind_media_fd(
              edge, media_edge_fd, &receipt
          ) == MEISO_COMPOSITION_EDGE_APP_OK &&
          receipt.slot_index == expected_slot &&
          receipt.active_route_mask == MEISO_COMPOSITION_EDGE_APP_MEDIA_ALL);
    for (step = 0U; step < 30000U; ++step) {
        MeisoCompositionEdgeAppIoReady ready;
        MeisoCompositionEdgeAppPumpResult result;
        MeisoCompositionEdgeAppSnapshot edge_snapshot;
        MeisoCompositionMediaTransportSnapshot host_snapshot;

        CHECK(host_media_step(host_media, media_host_fd));
        ready = (MeisoCompositionEdgeAppIoReady){
            true, true,
            fd_ready_now(media_edge_fd, POLLIN),
            fd_ready_now(media_edge_fd, POLLOUT),
            false,
            {77U, UINT64_C(900000) + step}
        };
        memset(&result, 0, sizeof(result));
        CHECK(benign_edge_status(meiso_composition_edge_app_pump(
                  edge, &ready, &result
              )) &&
              meiso_composition_edge_app_snapshot(edge, &edge_snapshot) &&
              meiso_composition_media_transport_snapshot(
                  host_media->transport, &host_snapshot
              ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
        if (edge_snapshot.media.bound &&
            edge_snapshot.media.transport_phase ==
                MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE &&
            host_snapshot.phase !=
                MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_ACTIVE) {
            size_t host_step;

            for (host_step = 0U; host_step < 100U; ++host_step) {
                CHECK(host_media_step(host_media, media_host_fd) &&
                      meiso_composition_media_transport_snapshot(
                          host_media->transport, &host_snapshot
                      ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK);
                if (host_snapshot.phase ==
                    MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_ACTIVE) {
                    break;
                }
            }
        }
        if (edge_snapshot.media.bound &&
            edge_snapshot.media.transport_phase ==
                MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE &&
            host_snapshot.phase ==
                MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_ACTIVE) {
            CHECK(edge_snapshot.media.active_route_mask ==
                      MEISO_COMPOSITION_EDGE_APP_MEDIA_ALL &&
                  edge_snapshot.media_slots_consumed ==
                      (uint8_t)(expected_slot + 1U));
            *out_receipt = receipt;
            return true;
        }
    }
    {
        MeisoCompositionEdgeAppSnapshot edge_snapshot;
        MeisoCompositionMediaTransportSnapshot host_snapshot;
        MeisoCompositionMediaPosixTcpSnapshot adapter_snapshot;

        memset(&edge_snapshot, 0, sizeof(edge_snapshot));
        memset(&host_snapshot, 0, sizeof(host_snapshot));
        memset(&adapter_snapshot, 0, sizeof(adapter_snapshot));
        (void)meiso_composition_edge_app_snapshot(edge, &edge_snapshot);
        (void)meiso_composition_media_transport_snapshot(
            host_media->transport, &host_snapshot
        );
        (void)meiso_composition_media_posix_tcp_snapshot(
            host_media->adapter, &adapter_snapshot
        );
        (void)fprintf(
            stderr,
            "edge media active timeout: bound=%u phase=%u mask=%u "
            "hostphase=%u camera=%u render_actual=%u camera_actual=%u "
            "rx=%zu/%zu tx=%zu/%zu\n",
            edge_snapshot.media.bound ? 1U : 0U,
            (unsigned)edge_snapshot.media.transport_phase,
            (unsigned)edge_snapshot.media.active_route_mask,
            (unsigned)host_snapshot.phase,
            (unsigned)edge_snapshot.camera.phase,
            edge_snapshot.actual[
                MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA
            ].production_status,
            edge_snapshot.actual[
                MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA
            ].production_status,
            adapter_snapshot.rx_bytes, adapter_snapshot.rx_expected_bytes,
            adapter_snapshot.tx_bytes_sent, adapter_snapshot.tx_packet_bytes
        );
    }
    return false;
}

static bool drive_camera_start_failure(
    MeisoCompositionEdgeApp *edge,
    MeisoCompositionDebugPeer *host,
    const CatalogStorage *catalog,
    FakeCamera *camera,
    uint8_t expected_failure_mask,
    uint64_t expected_open_calls
)
{
    const uint8_t route_indices[2] = {
        ROUTE_RENDER_ACTUAL, ROUTE_CAMERA_ACTUAL
    };
    const uint8_t accept_sources[2] = {14U, 16U};
    const size_t actual_indices[2] = {
        MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA,
        MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA
    };
    MeisoStateReplica *replicas[2] = {NULL, NULL};
    void *arenas[2] = {NULL, NULL};
    size_t arena_bytes[2];
    bool offer_seen[2] = {false, false};
    bool completion_seen[2] = {false, false};
    MeisoCompositionEdgeAppSnapshot snapshot;
    size_t step;
    size_t index;
    bool success = false;

    CHECK(edge != NULL && host != NULL && catalog != NULL && camera != NULL);
    for (index = 0U; index < 2U; ++index) {
        arena_bytes[index] = meiso_state_replica_memory_required(
            &catalog->routes[route_indices[index]].route.resource
        );
        arenas[index] = calloc(1U, arena_bytes[index]);
        CHECK_GOTO(arena_bytes[index] != 0U && arenas[index] != NULL &&
              meiso_state_replica_init(
                  arenas[index], arena_bytes[index],
                  &catalog->routes[route_indices[index]].route.resource,
                  &replicas[index]
              ) == MEISO_STATE_INIT_OK &&
              meiso_state_replica_begin_owner(replicas[index], EDGE_OWNER) ==
                  MEISO_STATE_OWNER_STARTED,
              cleanup);
    }
    for (step = 0U; step < 40000U; ++step) {
        MeisoCompositionDebugPeerPumpResult host_result;
        MeisoCompositionEdgeAppPumpResult edge_result;
        MeisoCompositionEdgeAppIoReady ready = {
            true, true, false, false, false,
            {77U, UINT64_C(1200000) + step}
        };

        memset(&host_result, 0, sizeof(host_result));
        CHECK_GOTO(benign_host_status(meiso_composition_debug_peer_pump(
              host, true, true, &host_result
            )), cleanup);
        if (host_result.completed_source_present) {
            MeisoRuntimeSchedulerCompletion completion;

            memset(&completion, 0, sizeof(completion));
            CHECK_GOTO(meiso_composition_debug_peer_take_completion(
                  host, host_result.completed_source_index, &completion
                ) == MEISO_COMPOSITION_DEBUG_PEER_OK &&
                  completion.code ==
                      MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT,
                  cleanup);
            for (index = 0U; index < 2U; ++index) {
                if (host_result.completed_source_index ==
                    accept_sources[index]) {
                    completion_seen[index] = true;
                }
            }
        }
        if (host_result.event_present && host_result.event.type ==
                MEISO_RUNTIME_EVENT_STATE_OFFER) {
            const MeisoRuntimeEvent *event = &host_result.event;
            bool matched = false;

            for (index = 0U; index < 2U; ++index) {
                MeisoRuntimeRouteKey route;

                CHECK_GOTO(meiso_composition_debug_peer_route_key(
                      host, route_indices[index], &route
                    ), cleanup);
                if (route_key_equal(event->route, route)) {
                    MeisoRuntimeActualState actual;
                    MeisoStateReceiveStatus receive;
                    MeisoRuntimeSchedulerWork work;

                    CHECK_GOTO((expected_failure_mask &
                          (uint8_t)(UINT8_C(1) << index)) != 0U &&
                          !offer_seen[index] &&
                          meiso_runtime_actual_state_decode(
                              event->state_offer.state.payload, &actual
                          ), cleanup);
                    if (!(actual.apply_result ==
                              MEISO_RUNTIME_APPLY_FAILED &&
                          actual.availability ==
                              MEISO_RUNTIME_AVAILABILITY_UNAVAILABLE &&
                          !actual.effective_enabled &&
                          actual.production_status ==
                              MEISO_RUNTIME_PRODUCTION_FAILED &&
                          actual.reason ==
                              MEISO_RUNTIME_PRODUCTION_REASON_UNAVAILABLE &&
                          actual.production_epoch == 0U &&
                          actual.profile.size == 0U)) {
                        (void)fprintf(
                            stderr,
                            "failure actual index=%zu apply=%u avail=%u "
                            "enabled=%u production=%u reason=%u epoch=%llu "
                            "profile=%zu\n",
                            index, (unsigned)actual.apply_result,
                            (unsigned)actual.availability,
                            actual.effective_enabled ? 1U : 0U,
                            (unsigned)actual.production_status,
                            (unsigned)actual.reason,
                            (unsigned long long)actual.production_epoch,
                            actual.profile.size
                        );
                    }
                    CHECK_GOTO(
                          actual.apply_result == MEISO_RUNTIME_APPLY_FAILED &&
                          actual.availability ==
                              MEISO_RUNTIME_AVAILABILITY_UNAVAILABLE &&
                          !actual.effective_enabled &&
                          actual.production_status ==
                              MEISO_RUNTIME_PRODUCTION_FAILED &&
                          actual.reason ==
                              MEISO_RUNTIME_PRODUCTION_REASON_UNAVAILABLE &&
                          actual.production_epoch == 0U &&
                          actual.profile.size == 0U,
                          cleanup);
                    receive = meiso_state_replica_receive(
                        replicas[index], event->state_offer.state.owner,
                        event->state_offer.state.version,
                        event->state_offer.state.payload
                    );
                    CHECK_GOTO(receive == MEISO_STATE_RECEIVE_NEW &&
                          meiso_composition_debug_peer_commit_state_receive(
                              host, event->route, &event->state_offer, receive
                          ) == MEISO_COMPOSITION_DEBUG_PEER_OK,
                          cleanup);
                    memset(&work, 0, sizeof(work));
                    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
                    work.value.state_accept.route = event->route;
                    work.value.state_accept.generation =
                        event->state_offer.generation;
                    work.value.state_accept.owner =
                        event->state_offer.state.owner;
                    work.value.state_accept.version =
                        event->state_offer.state.version;
                    CHECK_GOTO(meiso_composition_debug_peer_queue_work(
                          host, accept_sources[index], &work
                        ) == MEISO_COMPOSITION_DEBUG_PEER_OK,
                        cleanup);
                    offer_seen[index] = true;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                CHECK_GOTO(resolve_unrelated_actual_offer(host, event),
                           cleanup);
            }
        }
        memset(&edge_result, 0, sizeof(edge_result));
        CHECK_GOTO(benign_edge_status(meiso_composition_edge_app_pump(
              edge, &ready, &edge_result
            )) && meiso_composition_edge_app_snapshot(edge, &snapshot),
            cleanup);
        if (((expected_failure_mask &
                  MEISO_COMPOSITION_EDGE_APP_MEDIA_RENDER) == 0U ||
             (offer_seen[0] && completion_seen[0] &&
              snapshot.actual[actual_indices[0]].delivery_accepted)) &&
            ((expected_failure_mask &
                  MEISO_COMPOSITION_EDGE_APP_MEDIA_CAMERA) == 0U ||
             (offer_seen[1] && completion_seen[1] &&
              snapshot.actual[actual_indices[1]].delivery_accepted))) {
            const bool render_failed = (expected_failure_mask &
                MEISO_COMPOSITION_EDGE_APP_MEDIA_RENDER) != 0U;

            CHECK_GOTO(camera->open_calls == expected_open_calls &&
                  snapshot.phase == MEISO_COMPOSITION_EDGE_APP_PHASE_READY &&
                  snapshot.control_ready && !snapshot.media.bound &&
                  snapshot.media_slots_consumed >= 1U &&
                  snapshot.actual[actual_indices[0]].production_status ==
                      (render_failed ?
                           MEISO_RUNTIME_PRODUCTION_FAILED :
                           MEISO_RUNTIME_PRODUCTION_RUNNING) &&
                  snapshot.actual[actual_indices[1]].production_status ==
                      MEISO_RUNTIME_PRODUCTION_FAILED &&
                  snapshot.render_media_actual_present == !render_failed &&
                  !snapshot.camera_media_actual_present &&
                  snapshot.actual[MEISO_COMPOSITION_EDGE_APP_ACTUAL_WIFI]
                      .production_status == MEISO_RUNTIME_PRODUCTION_RUNNING &&
                  snapshot.wifi_effective_present,
                  cleanup);
            success = true;
            break;
        }
    }
    if (!success) {
        (void)meiso_composition_edge_app_snapshot(edge, &snapshot);
        (void)fprintf(
            stderr,
            "camera failure timeout: offers=%u/%u completions=%u/%u "
            "accepted=%u/%u pending=%u/%u open=%llu bound=%u phase=%u\n",
            offer_seen[0] ? 1U : 0U, offer_seen[1] ? 1U : 0U,
            completion_seen[0] ? 1U : 0U,
            completion_seen[1] ? 1U : 0U,
            snapshot.actual[actual_indices[0]].delivery_accepted ? 1U : 0U,
            snapshot.actual[actual_indices[1]].delivery_accepted ? 1U : 0U,
            snapshot.actual[actual_indices[0]].delivery_pending ? 1U : 0U,
            snapshot.actual[actual_indices[1]].delivery_pending ? 1U : 0U,
            (unsigned long long)camera->open_calls,
            snapshot.media.bound ? 1U : 0U, (unsigned)snapshot.phase
        );
    }

cleanup:
    free(arenas[1]);
    free(arenas[0]);
    return success;
}

static bool pump_edge_media_once(
    MeisoCompositionEdgeApp *edge,
    HostMediaFixture *host_media,
    int media_edge_fd,
    int media_host_fd,
    uint64_t now_ns,
    bool camera_poll_allowed
)
{
    MeisoCompositionEdgeAppIoReady ready;
    MeisoCompositionEdgeAppPumpResult result;

    CHECK(edge != NULL && host_media != NULL && media_edge_fd >= 0 &&
          media_host_fd >= 0 &&
          host_media_step(host_media, media_host_fd));
    ready = (MeisoCompositionEdgeAppIoReady){
        true, true,
        fd_ready_now(media_edge_fd, POLLIN),
        fd_ready_now(media_edge_fd, POLLOUT),
        camera_poll_allowed, {77U, now_ns}
    };
    memset(&result, 0, sizeof(result));
    CHECK(benign_edge_status(meiso_composition_edge_app_pump(
        edge, &ready, &result
    )));
    return true;
}

static bool drive_camera_fixed_point_cadence(
    MeisoCompositionEdgeApp *edge,
    HostMediaFixture *host_media,
    int media_edge_fd,
    int media_host_fd,
    FakeCamera *camera
)
{
    const uint64_t first_time = UINT64_C(2000000000);
    const uint64_t period_ns = UINT64_C(100000000);
    MeisoCompositionEdgeAppSnapshot snapshot;
    size_t step;

    CHECK(edge != NULL && host_media != NULL && camera != NULL &&
          camera->succeed && camera->dequeue_calls == 0U);
    for (step = 0U; step < 200U; ++step) {
        CHECK(pump_edge_media_once(
            edge, host_media, media_edge_fd, media_host_fd,
            first_time, false
        ));
        CHECK(meiso_composition_edge_app_snapshot(edge, &snapshot));
        if (snapshot.camera.phase == MEISO_CAMERA_V4L2_PHASE_RUNNING) {
            break;
        }
    }
    CHECK(step < 200U && camera->open_calls == 2U &&
          snapshot.camera.phase == MEISO_CAMERA_V4L2_PHASE_RUNNING &&
          camera->dequeue_calls == 0U);
    for (step = 0U; step < 200U && camera->dequeue_calls == 0U; ++step) {
        CHECK(pump_edge_media_once(
            edge, host_media, media_edge_fd, media_host_fd,
            first_time, true
        ));
    }
    CHECK(camera->dequeue_calls == 1U);
    for (step = 0U; step < 200U; ++step) {
        CHECK(pump_edge_media_once(
            edge, host_media, media_edge_fd, media_host_fd,
            first_time, false
        ));
        CHECK(meiso_composition_edge_app_snapshot(edge, &snapshot));
        if (snapshot.media.camera_submitted_sequence == 1U &&
            !snapshot.camera.frame_borrowed) {
            break;
        }
    }
    CHECK(step < 200U && snapshot.media.camera_submitted_sequence == 1U &&
          camera->dequeue_calls == 1U);
    for (step = 0U; step < 64U; ++step) {
        CHECK(pump_edge_media_once(
            edge, host_media, media_edge_fd, media_host_fd,
            first_time + period_ns - 1U, true
        ));
    }
    CHECK(camera->dequeue_calls == 1U);
    camera->dequeue.index = 1U;
    camera->dequeue.driver_sequence = 2U;
    camera->dequeue.timestamp_microseconds = 100000;
    for (step = 0U; step < 200U && camera->dequeue_calls == 1U; ++step) {
        CHECK(pump_edge_media_once(
            edge, host_media, media_edge_fd, media_host_fd,
            first_time + period_ns, true
        ));
    }
    CHECK(camera->dequeue_calls == 2U);
    for (step = 0U; step < 200U; ++step) {
        CHECK(pump_edge_media_once(
            edge, host_media, media_edge_fd, media_host_fd,
            first_time + period_ns, false
        ));
        CHECK(meiso_composition_edge_app_snapshot(edge, &snapshot));
        if (snapshot.media.camera_submitted_sequence == 2U &&
            !snapshot.camera.frame_borrowed) {
            break;
        }
    }
    CHECK(step < 200U && snapshot.media.camera_submitted_sequence == 2U &&
          snapshot.media.camera_whole_frames_dropped <= 1U &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA]
              .production_status == MEISO_RUNTIME_PRODUCTION_RUNNING);
    return true;
}

static bool drive_camera_next_due(
    MeisoCompositionEdgeApp *edge,
    HostMediaFixture *host_media,
    int media_edge_fd,
    int media_host_fd,
    FakeCamera *camera,
    uint64_t early_ns,
    uint64_t due_ns,
    uint64_t expected_sequence
)
{
    const uint64_t before_calls = camera->dequeue_calls;
    MeisoCompositionEdgeAppSnapshot snapshot;
    size_t step;

    CHECK(expected_sequence == before_calls + 1U);
    for (step = 0U; step < 64U; ++step) {
        CHECK(pump_edge_media_once(
            edge, host_media, media_edge_fd, media_host_fd, early_ns, true
        ));
    }
    CHECK(camera->dequeue_calls == before_calls);
    camera->dequeue.index = (uint32_t)(expected_sequence %
        MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT);
    camera->dequeue.driver_sequence = expected_sequence;
    camera->dequeue.timestamp_microseconds =
        (int64_t)((expected_sequence - 1U) * UINT64_C(100000));
    for (step = 0U; step < 200U &&
         camera->dequeue_calls == before_calls; ++step) {
        CHECK(pump_edge_media_once(
            edge, host_media, media_edge_fd, media_host_fd, due_ns, true
        ));
    }
    CHECK(camera->dequeue_calls == expected_sequence);
    for (step = 0U; step < 200U; ++step) {
        CHECK(pump_edge_media_once(
            edge, host_media, media_edge_fd, media_host_fd, due_ns, false
        ) && meiso_composition_edge_app_snapshot(edge, &snapshot));
        if (snapshot.media.camera_submitted_sequence == expected_sequence &&
            !snapshot.camera.frame_borrowed) {
            break;
        }
    }
    CHECK(step < 200U &&
          snapshot.media.camera_submitted_sequence == expected_sequence);
    return true;
}

static bool trigger_active_media_transport_loss(
    MeisoCompositionEdgeApp *edge,
    int *media_host_fd,
    int media_edge_fd
)
{
    MeisoCompositionEdgeAppSnapshot snapshot;
    size_t step;

    CHECK(edge != NULL && media_host_fd != NULL && *media_host_fd >= 0 &&
          media_edge_fd >= 0);
    close_fd(media_host_fd);
    for (step = 0U; step < 100U; ++step) {
        MeisoCompositionEdgeAppIoReady ready = {
            true, true, true, false, false,
            {77U, UINT64_C(2300000000) + step}
        };
        MeisoCompositionEdgeAppPumpResult result;

        memset(&result, 0, sizeof(result));
        CHECK(benign_edge_status(meiso_composition_edge_app_pump(
                  edge, &ready, &result
              )) && meiso_composition_edge_app_snapshot(edge, &snapshot));
        if (!snapshot.media.bound) {
            CHECK(snapshot.phase == MEISO_COMPOSITION_EDGE_APP_PHASE_READY &&
                  snapshot.control_ready);
            return true;
        }
    }
    return false;
}

static bool drive_camera_epoch_reset_first_due(
    MeisoCompositionEdgeApp *edge,
    HostMediaFixture *host_media,
    int media_edge_fd,
    int media_host_fd,
    FakeCamera *camera,
    uint64_t now_ns,
    uint64_t expected_open_calls
)
{
    const uint64_t before_dequeues = camera->dequeue_calls;
    MeisoCompositionEdgeAppSnapshot snapshot;
    size_t step;

    CHECK(edge != NULL && host_media != NULL && camera != NULL);
    for (step = 0U; step < 200U; ++step) {
        CHECK(pump_edge_media_once(
            edge, host_media, media_edge_fd, media_host_fd, now_ns, false
        ) && meiso_composition_edge_app_snapshot(edge, &snapshot));
        if (snapshot.camera.phase == MEISO_CAMERA_V4L2_PHASE_RUNNING) {
            break;
        }
    }
    CHECK(step < 200U && camera->open_calls == expected_open_calls &&
          camera->dequeue_calls == before_dequeues &&
          snapshot.media.camera_submitted_sequence == 0U);
    camera->dequeue.index = (uint32_t)(before_dequeues %
        MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT);
    camera->dequeue.driver_sequence = before_dequeues + 1U;
    camera->dequeue.timestamp_microseconds =
        (int64_t)(before_dequeues * UINT64_C(100000));
    for (step = 0U; step < 200U &&
         camera->dequeue_calls == before_dequeues; ++step) {
        CHECK(pump_edge_media_once(
            edge, host_media, media_edge_fd, media_host_fd, now_ns, true
        ));
    }
    CHECK(camera->dequeue_calls == before_dequeues + 1U);
    for (step = 0U; step < 200U; ++step) {
        CHECK(pump_edge_media_once(
            edge, host_media, media_edge_fd, media_host_fd, now_ns, false
        ) && meiso_composition_edge_app_snapshot(edge, &snapshot));
        if (snapshot.media.camera_submitted_sequence == 1U &&
            !snapshot.camera.frame_borrowed) {
            break;
        }
    }
    CHECK(step < 200U && snapshot.media.camera_submitted_sequence == 1U);
    return true;
}

static MeisoCompositionSceneDesired empty_scene_value(
    uint64_t desired_version
)
{
    MeisoCompositionSceneDesired scene;

    memset(&scene, 0, sizeof(scene));
    scene.camera.space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    scene.camera.space_epoch = 9U;
    scene.camera.source_time.clock_epoch = 77U;
    scene.camera.source_time.monotonic_ns =
        UINT64_C(4000000000) + desired_version * UINT64_C(1000000);
    scene.camera.position[0] = (float)(desired_version - 1U) * 0.01F;
    scene.camera.position[2] = 3.0F;
    scene.camera.orientation_xyzw[3] = 1.0F;
    scene.camera.projection =
        MEISO_COMPOSITION_SCENE_PROJECTION_PERSPECTIVE;
    scene.camera.vertical_fov_degrees = 60.0F;
    scene.camera.near_z = 0.1F;
    scene.camera.far_z = 100.0F;
    scene.camera.viewport_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    scene.camera.viewport_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    return scene;
}

static MeisoCompositionScenePresentationPlan test_presentation_plan(void)
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

static bool drive_empty_scene_world_swaps_and_disable(
    MeisoCompositionEdgeApp *edge,
    MeisoCompositionDebugPeer *host,
    const CatalogStorage *catalog,
    uint8_t expected_media_slot,
    uint8_t expected_media_slots_consumed
)
{
    MeisoCompositionSceneDesired scene = empty_scene_value(1U);
    MeisoCompositionScenePresentationPlan plan = test_presentation_plan();
    MeisoCompositionEdgeAppSnapshot snapshot;
    MeisoCompositionEdgeAppSnapshot active_snapshot;
    uint8_t scene_suffix[MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE];
    uint8_t plan_suffix[MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE];
    uint8_t token[MEISO_SHA256_DIGEST_SIZE];
    size_t scene_suffix_size = 0U;
    size_t plan_suffix_size = 0U;
    uint64_t prior_world_generation;
    uint64_t render_epoch;
    uint64_t camera_epoch;
    uint64_t desired_version;
    uint64_t now_base = UINT64_C(100000000000);

    CHECK(edge != NULL && host != NULL && catalog != NULL &&
          plan.plan_generation == 1U && plan.capability_generation == 1U &&
          meiso_composition_scene_desired_encode(
              &scene,
              (MeisoMutableBuffer){scene_suffix, sizeof(scene_suffix)},
              &scene_suffix_size
          ) == MEISO_COMPOSITION_SCENE_CODEC_OK &&
          drive_composition_desired_actual(
              edge, host, catalog, true, true, 1U,
              (MeisoByteView){scene_suffix, scene_suffix_size},
              COMPOSITION_DESIRED_SCENE_OBSERVED, now_base, &snapshot
          ));
    now_base += UINT64_C(100000000000);
    CHECK(meiso_composition_scene_presentation_plan_encode(
              &plan,
              (MeisoMutableBuffer){plan_suffix, sizeof(plan_suffix)},
              &plan_suffix_size
          ) == MEISO_COMPOSITION_SCENE_CODEC_OK &&
          drive_composition_desired_actual(
              edge, host, catalog, false, true, 1U,
              (MeisoByteView){plan_suffix, plan_suffix_size},
              COMPOSITION_DESIRED_PLAN_PREPARED, now_base, &snapshot
          ) && snapshot.presentation_status.prepared_token_present &&
          snapshot.presentation_status.candidate_plan_generation == 1U &&
          memcmp(snapshot.presentation_status.candidate_digest,
                 plan.candidate_digest, sizeof(plan.candidate_digest)) == 0);
    memcpy(token, snapshot.presentation_status.prepared_token, sizeof(token));
    plan.prepared_token_echo_present = true;
    memcpy(plan.prepared_token_echo, token, sizeof(token));
    now_base += UINT64_C(100000000000);
    CHECK(meiso_composition_scene_presentation_plan_encode(
              &plan,
              (MeisoMutableBuffer){plan_suffix, sizeof(plan_suffix)},
              &plan_suffix_size
          ) == MEISO_COMPOSITION_SCENE_CODEC_OK &&
          drive_composition_desired_actual(
              edge, host, catalog, false, true, 2U,
              (MeisoByteView){plan_suffix, plan_suffix_size},
              COMPOSITION_DESIRED_PLAN_ACTIVE, now_base, &active_snapshot
          ) && active_snapshot.active_world_present &&
          active_snapshot.active_world.desired_state_version == 1U &&
          active_snapshot.active_world.plan_generation == 1U &&
          active_snapshot.presentation_status.active_plan_generation == 1U &&
          active_snapshot.media.bound &&
          active_snapshot.media.slot_index == expected_media_slot &&
          active_snapshot.media_slots_consumed ==
              expected_media_slots_consumed);
    prior_world_generation = active_snapshot.active_world.world_generation;
    render_epoch = active_snapshot.actual[
        MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA
    ].production_epoch;
    camera_epoch = active_snapshot.actual[
        MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA
    ].production_epoch;
    for (desired_version = 2U; desired_version <= 11U;
         ++desired_version) {
        scene = empty_scene_value(desired_version);
        scene_suffix_size = 0U;
        now_base += UINT64_C(100000000000);
        CHECK(meiso_composition_scene_desired_encode(
                  &scene,
                  (MeisoMutableBuffer){scene_suffix, sizeof(scene_suffix)},
                  &scene_suffix_size
              ) == MEISO_COMPOSITION_SCENE_CODEC_OK &&
              drive_composition_desired_actual(
                  edge, host, catalog, true, true, desired_version,
                  (MeisoByteView){scene_suffix, scene_suffix_size},
                  COMPOSITION_DESIRED_SCENE_ACTIVE, now_base, &snapshot
              ) && snapshot.active_world_present &&
              snapshot.active_world.desired_state_version == desired_version &&
              snapshot.active_world.plan_generation == 1U &&
              snapshot.active_world.world_generation > prior_world_generation &&
              snapshot.scene_status.world_generation ==
                  snapshot.active_world.world_generation &&
              snapshot.scene_status.active_object_count == 0U &&
              snapshot.media.bound &&
              snapshot.media.slot_index == expected_media_slot &&
              snapshot.media_slots_consumed == expected_media_slots_consumed &&
              snapshot.actual[
                  MEISO_COMPOSITION_EDGE_APP_ACTUAL_RENDER_MEDIA
              ].production_epoch == render_epoch &&
              snapshot.actual[
                  MEISO_COMPOSITION_EDGE_APP_ACTUAL_CAMERA_MEDIA
              ].production_epoch == camera_epoch);
        prior_world_generation = snapshot.active_world.world_generation;
    }
    now_base += UINT64_C(100000000000);
    CHECK(drive_composition_desired_actual(
              edge, host, catalog, false, false, 3U,
              (MeisoByteView){NULL, 0U},
              COMPOSITION_DESIRED_PLAN_STOPPED, now_base, &snapshot
          ) && !snapshot.active_world_present &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_APP_ACTUAL_PRESENTATION]
              .applied_desired.version == 3U &&
          !snapshot.inline_streams[
              MEISO_COMPOSITION_EDGE_APP_INLINE_RENDER_RESULT
          ].enabled &&
          snapshot.media.bound &&
          snapshot.media.slot_index == expected_media_slot &&
          snapshot.media_slots_consumed == expected_media_slots_consumed);
    return true;
}

static bool drive_wifi_desired_actual(
    MeisoCompositionEdgeApp *edge,
    MeisoCompositionDebugPeer *host,
    const CatalogStorage *catalog
)
{
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const MeisoDeliveryGeneration desired_generation = {1U};
    MeisoStatePublisher *desired = NULL;
    MeisoStateReplica *actual = NULL;
    void *desired_arena = NULL;
    void *actual_arena = NULL;
    size_t desired_bytes;
    size_t actual_bytes;
    uint8_t desired_wire[
        MEISO_WIFI_TELEMETRY_DESIRED_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoByteView desired_payload;
    MeisoStateOffer desired_offer;
    MeisoRuntimeRouteKey desired_route;
    MeisoRuntimeRouteKey actual_route;
    MeisoRuntimeSchedulerWork work;
    MeisoStateView actual_view;
    MeisoRuntimeActualState actual_value;
    MeisoWifiTelemetryEffectiveParameters effective;
    MeisoStatePublisherStats desired_stats;
    MeisoCompositionEdgeAppSnapshot snapshot;
    bool desired_accepted = false;
    bool actual_received = false;
    bool actual_accept_sent = false;
    bool success = false;
    size_t step;

    desired_bytes = meiso_state_publisher_memory_required(
        &catalog->routes[ROUTE_WIFI_DESIRED].route.resource, &state_config
    );
    actual_bytes = meiso_state_replica_memory_required(
        &catalog->routes[ROUTE_WIFI_ACTUAL].route.resource
    );
    desired_arena = calloc(1U, desired_bytes);
    actual_arena = calloc(1U, actual_bytes);
    CHECK_GOTO(desired_bytes != 0U && actual_bytes != 0U &&
          desired_arena != NULL && actual_arena != NULL, cleanup);
    CHECK_GOTO(meiso_state_publisher_init(
          desired_arena, desired_bytes,
          &catalog->routes[ROUTE_WIFI_DESIRED].route.resource,
          &state_config, &desired
        ) == MEISO_STATE_INIT_OK &&
          meiso_state_replica_init(
              actual_arena, actual_bytes,
              &catalog->routes[ROUTE_WIFI_ACTUAL].route.resource,
              &actual
          ) == MEISO_STATE_INIT_OK &&
          meiso_state_publisher_begin_delivery(
              desired, desired_generation
          ) == MEISO_STATE_DELIVERY_STARTED &&
          meiso_state_replica_begin_owner(actual, EDGE_OWNER) ==
              MEISO_STATE_OWNER_STARTED &&
          encode_wifi_desired(
              MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
                  MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM,
              500U, desired_wire, &desired_payload
          ) &&
          meiso_state_publisher_publish(
              desired, HOST_OWNER, 1U, desired_payload
          ) == MEISO_STATE_PUBLISH_NEW &&
          meiso_state_publisher_next_offer(desired, &desired_offer) &&
          meiso_composition_debug_peer_route_key(
              host, ROUTE_WIFI_DESIRED, &desired_route
          ) &&
          meiso_composition_debug_peer_route_key(
              host, ROUTE_WIFI_ACTUAL, &actual_route
          ), cleanup);
    memset(&work, 0, sizeof(work));
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
    work.value.state_offer.route = desired_route;
    work.value.state_offer.offer = desired_offer;
    CHECK_GOTO(meiso_composition_debug_peer_queue_work(
          host, 1U, &work
        ) == MEISO_COMPOSITION_DEBUG_PEER_OK, cleanup);

    for (step = 0U; step < 30000U; ++step) {
        MeisoCompositionDebugPeerPumpResult host_result;
        MeisoCompositionDebugPeerPumpStatus host_status;
        MeisoCompositionEdgeAppPumpResult edge_result;
        MeisoCompositionEdgeAppPumpStatus edge_status;
        MeisoCompositionEdgeAppIoReady io = {
            true, true, false, false, false, {77U, 100000U + step}
        };

        memset(&host_result, 0, sizeof(host_result));
        host_status = meiso_composition_debug_peer_pump(
            host, true, true, &host_result
        );
        CHECK_GOTO(benign_host_status(host_status), cleanup);
        if (host_result.completed_source_present) {
            MeisoRuntimeSchedulerCompletion completion;

            memset(&completion, 0, sizeof(completion));
            CHECK_GOTO(meiso_composition_debug_peer_take_completion(
                  host, host_result.completed_source_index, &completion
                ) == MEISO_COMPOSITION_DEBUG_PEER_OK &&
                  completion.code ==
                      MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT,
                  cleanup);
            if (host_result.completed_source_index == 2U) {
                actual_accept_sent = true;
            }
        }
        if (host_status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_EVENT &&
            host_result.event_present) {
            const MeisoRuntimeEvent *event = &host_result.event;

            if (event->type == MEISO_RUNTIME_EVENT_STATE_ACCEPT &&
                route_key_equal(event->route, desired_route)) {
                CHECK_GOTO(meiso_state_publisher_mark_delivery_accepted(
                      desired, event->state_accept_generation,
                      event->state_accept_owner,
                      event->state_accept_version
                    ) == MEISO_STATE_ACCEPT_CONFIRMED, cleanup);
                desired_accepted = true;
            } else if (event->type == MEISO_RUNTIME_EVENT_STATE_OFFER &&
                       route_key_equal(event->route, actual_route)) {
                MeisoStateReceiveStatus receive_status;

                CHECK_GOTO(!actual_received, cleanup);
                receive_status = meiso_state_replica_receive(
                    actual, event->state_offer.state.owner,
                    event->state_offer.state.version,
                    event->state_offer.state.payload
                );
                CHECK_GOTO(receive_status == MEISO_STATE_RECEIVE_NEW &&
                      meiso_composition_debug_peer_commit_state_receive(
                          host, event->route, &event->state_offer,
                          receive_status
                      ) == MEISO_COMPOSITION_DEBUG_PEER_OK,
                      cleanup);
                memset(&work, 0, sizeof(work));
                work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
                work.value.state_accept.route = event->route;
                work.value.state_accept.generation =
                    event->state_offer.generation;
                work.value.state_accept.owner =
                    event->state_offer.state.owner;
                work.value.state_accept.version =
                    event->state_offer.state.version;
                CHECK_GOTO(meiso_composition_debug_peer_queue_work(
                      host, 2U, &work
                    ) == MEISO_COMPOSITION_DEBUG_PEER_OK, cleanup);
                actual_received = true;
            }
        }

        memset(&edge_result, 0, sizeof(edge_result));
        edge_status = meiso_composition_edge_app_pump(edge, &io, &edge_result);
        CHECK_GOTO(benign_edge_status(edge_status), cleanup);
        CHECK_GOTO(meiso_composition_edge_app_snapshot(edge, &snapshot),
                   cleanup);
        if (desired_accepted && actual_received && actual_accept_sent &&
            snapshot.actual[MEISO_COMPOSITION_EDGE_APP_ACTUAL_WIFI]
                .delivery_accepted) {
            break;
        }
    }
    CHECK_GOTO(step < 30000U && desired_accepted && actual_received &&
          actual_accept_sent && meiso_state_replica_current(
              actual, &actual_view
          ) && meiso_runtime_actual_state_decode(
              actual_view.payload, &actual_value
          ) && meiso_wifi_telemetry_effective_parameters_decode(
              actual_value.profile, &effective
          ) == MEISO_WIFI_TELEMETRY_CODEC_OK &&
          meiso_state_publisher_stats(desired, &desired_stats), cleanup);
    CHECK_GOTO(meiso_owner_scope_equal(actual_view.owner, EDGE_OWNER) &&
          actual_view.version == 1U &&
          actual_value.observed_desired.present &&
          meiso_owner_scope_equal(
              actual_value.observed_desired.owner, HOST_OWNER
          ) && actual_value.observed_desired.version == 1U &&
          meiso_owner_scope_equal(
              actual_value.applied_desired.owner, HOST_OWNER
          ) && actual_value.applied_desired.version == 1U &&
          actual_value.apply_result == MEISO_RUNTIME_APPLY_APPLIED &&
          actual_value.availability == MEISO_RUNTIME_AVAILABILITY_PRESENT &&
          actual_value.effective_enabled &&
          actual_value.production_status ==
              MEISO_RUNTIME_PRODUCTION_RUNNING &&
          actual_value.reason == MEISO_RUNTIME_PRODUCTION_REASON_NONE &&
          actual_value.production_epoch != 0U &&
          effective.selected_fields ==
              (MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
               MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM) &&
          effective.sample_period_ms == 500U &&
          desired_stats.delivery_accepts == 1U &&
          snapshot.wifi_effective_present &&
          snapshot.wifi_effective.selected_fields ==
              effective.selected_fields &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_APP_ACTUAL_WIFI]
              .actual_state_version == 1U &&
          snapshot.actual[MEISO_COMPOSITION_EDGE_APP_ACTUAL_WIFI]
              .delivery_accepted,
          cleanup);
    success = true;

cleanup:
    free(actual_arena);
    free(desired_arena);
    return success;
}

static bool drive_wifi_back_to_back_replacement(
    MeisoCompositionEdgeApp *edge,
    MeisoCompositionDebugPeer *host,
    const CatalogStorage *catalog
)
{
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const MeisoDeliveryGeneration desired_generation = {2U};
    MeisoStatePublisher *desired = NULL;
    MeisoStateReplica *actual = NULL;
    void *desired_arena = NULL;
    void *actual_arena = NULL;
    size_t desired_bytes;
    size_t actual_bytes;
    uint8_t desired_wire[2][
        MEISO_WIFI_TELEMETRY_DESIRED_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoByteView desired_payload[2];
    MeisoRuntimeRouteKey desired_route;
    MeisoRuntimeRouteKey actual_route;
    MeisoRuntimeSchedulerWork work;
    MeisoCompositionEdgeAppSnapshot snapshot;
    bool version3_queued = false;
    bool version2_accept_seen = false;
    bool version3_accept_seen = false;
    bool version2_actual_seen = false;
    bool version3_actual_seen = false;
    bool actual_accept_pending = false;
    bool saw_control_read_suppressed = false;
    uint32_t desired_offer_completions = 0U;
    uint32_t actual_accept_completions = 0U;
    bool success = false;
    size_t step;

    desired_bytes = meiso_state_publisher_memory_required(
        &catalog->routes[ROUTE_WIFI_DESIRED].route.resource, &state_config
    );
    actual_bytes = meiso_state_replica_memory_required(
        &catalog->routes[ROUTE_WIFI_ACTUAL].route.resource
    );
    desired_arena = calloc(1U, desired_bytes);
    actual_arena = calloc(1U, actual_bytes);
    CHECK_GOTO(desired_bytes != 0U && actual_bytes != 0U &&
          desired_arena != NULL && actual_arena != NULL, cleanup);
    CHECK_GOTO(meiso_state_publisher_init(
          desired_arena, desired_bytes,
          &catalog->routes[ROUTE_WIFI_DESIRED].route.resource,
          &state_config, &desired
        ) == MEISO_STATE_INIT_OK &&
          meiso_state_replica_init(
              actual_arena, actual_bytes,
              &catalog->routes[ROUTE_WIFI_ACTUAL].route.resource,
              &actual
          ) == MEISO_STATE_INIT_OK &&
          meiso_state_publisher_begin_delivery(
              desired, desired_generation
          ) == MEISO_STATE_DELIVERY_STARTED &&
          meiso_state_replica_begin_owner(actual, EDGE_OWNER) ==
              MEISO_STATE_OWNER_STARTED &&
          encode_wifi_desired(
              MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE, 600U,
              desired_wire[0], &desired_payload[0]
          ) && encode_wifi_desired(
              MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
                  MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM,
              700U, desired_wire[1], &desired_payload[1]
          ) && meiso_composition_debug_peer_route_key(
              host, ROUTE_WIFI_DESIRED, &desired_route
          ) && meiso_composition_debug_peer_route_key(
              host, ROUTE_WIFI_ACTUAL, &actual_route
          ), cleanup);
    CHECK_GOTO(meiso_state_publisher_publish(
          desired, HOST_OWNER, 2U, desired_payload[0]
        ) == MEISO_STATE_PUBLISH_NEW, cleanup);
    {
        MeisoStateOffer offer;

        CHECK_GOTO(meiso_state_publisher_next_offer(desired, &offer), cleanup);
        memset(&work, 0, sizeof(work));
        work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
        work.value.state_offer.route = desired_route;
        work.value.state_offer.offer = offer;
        CHECK_GOTO(meiso_composition_debug_peer_queue_work(
              host, 1U, &work
            ) == MEISO_COMPOSITION_DEBUG_PEER_OK, cleanup);
    }

    for (step = 0U; step < 40000U; ++step) {
        MeisoCompositionDebugPeerPumpResult host_result;
        MeisoCompositionDebugPeerPumpStatus host_status;
        MeisoCompositionEdgeAppPumpResult edge_result;
        MeisoCompositionEdgeAppPumpStatus edge_status;
        MeisoCompositionEdgeAppIoReady io = {
            true, true, false, false, false, {77U, 200000U + step}
        };
        MeisoCompositionEdgeAppIoInterest interest;

        memset(&host_result, 0, sizeof(host_result));
        host_status = meiso_composition_debug_peer_pump(
            host, !actual_accept_pending, true, &host_result
        );
        if (!benign_host_status(host_status)) {
            (void)fprintf(
                stderr,
                "rapid host status=%u step=%zu event=%u present=%u "
                "q3=%u a2=%u a3=%u v2=%u v3=%u dc=%u ac=%u ap=%u\n",
                (unsigned)host_status, step,
                (unsigned)host_result.event.type,
                host_result.event_present ? 1U : 0U,
                version3_queued ? 1U : 0U,
                version2_accept_seen ? 1U : 0U,
                version3_accept_seen ? 1U : 0U,
                version2_actual_seen ? 1U : 0U,
                version3_actual_seen ? 1U : 0U,
                desired_offer_completions,
                actual_accept_completions,
                actual_accept_pending ? 1U : 0U
            );
        }
        CHECK_GOTO(benign_host_status(host_status), cleanup);
        if (host_result.completed_source_present) {
            MeisoRuntimeSchedulerCompletion completion;

            memset(&completion, 0, sizeof(completion));
            CHECK_GOTO(meiso_composition_debug_peer_take_completion(
                  host, host_result.completed_source_index, &completion
                ) == MEISO_COMPOSITION_DEBUG_PEER_OK &&
                  completion.code ==
                      MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT,
                  cleanup);
            if (host_result.completed_source_index == 1U) {
                ++desired_offer_completions;
                if (!version3_queued) {
                    MeisoStateOffer offer;

                    CHECK_GOTO(meiso_state_publisher_publish(
                          desired, HOST_OWNER, 3U, desired_payload[1]
                        ) == MEISO_STATE_PUBLISH_REPLACED_PENDING &&
                          meiso_state_publisher_next_offer(desired, &offer),
                          cleanup);
                    memset(&work, 0, sizeof(work));
                    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
                    work.value.state_offer.route = desired_route;
                    work.value.state_offer.offer = offer;
                    CHECK_GOTO(meiso_composition_debug_peer_queue_work(
                          host, 1U, &work
                        ) == MEISO_COMPOSITION_DEBUG_PEER_OK, cleanup);
                    version3_queued = true;
                }
            } else if (host_result.completed_source_index == 2U) {
                ++actual_accept_completions;
                actual_accept_pending = false;
            }
        }
        if (host_status == MEISO_COMPOSITION_DEBUG_PEER_PUMP_EVENT &&
            host_result.event_present) {
            const MeisoRuntimeEvent *event = &host_result.event;

            if (event->type == MEISO_RUNTIME_EVENT_STATE_ACCEPT &&
                route_key_equal(event->route, desired_route)) {
                MeisoStateAcceptStatus accept_status;

                accept_status = meiso_state_publisher_mark_delivery_accepted(
                    desired, event->state_accept_generation,
                    event->state_accept_owner,
                    event->state_accept_version
                );
                if (event->state_accept_version == 2U) {
                    CHECK_GOTO(accept_status == MEISO_STATE_ACCEPT_STALE,
                               cleanup);
                    version2_accept_seen = true;
                } else if (event->state_accept_version == 3U) {
                    CHECK_GOTO(accept_status ==
                                   MEISO_STATE_ACCEPT_CONFIRMED ||
                               accept_status == MEISO_STATE_ACCEPT_DUPLICATE,
                               cleanup);
                    version3_accept_seen = true;
                } else {
                    CHECK_GOTO(false, cleanup);
                }
            } else if (event->type == MEISO_RUNTIME_EVENT_STATE_OFFER &&
                       route_key_equal(event->route, actual_route)) {
                MeisoRuntimeActualState actual_value;
                MeisoWifiTelemetryEffectiveParameters effective;
                const uint64_t desired_version =
                    event->state_offer.state.version;

                CHECK_GOTO(!actual_accept_pending &&
                      meiso_runtime_actual_state_decode(
                          event->state_offer.state.payload, &actual_value
                      ) && meiso_wifi_telemetry_effective_parameters_decode(
                          actual_value.profile, &effective
                      ) == MEISO_WIFI_TELEMETRY_CODEC_OK &&
                      actual_value.applied_desired.present &&
                      actual_value.applied_desired.version == desired_version,
                      cleanup);
                if (desired_version == 2U) {
                    CHECK_GOTO(effective.selected_fields ==
                          MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE &&
                          effective.sample_period_ms == 600U,
                          cleanup);
                    version2_actual_seen = true;
                    CHECK_GOTO(meiso_composition_debug_peer_commit_state_receive(
                          host, event->route, &event->state_offer,
                          MEISO_STATE_RECEIVE_STALE
                        ) == MEISO_COMPOSITION_DEBUG_PEER_OK,
                        cleanup);
                    memset(&work, 0, sizeof(work));
                    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
                    work.value.state_accept.route = event->route;
                    work.value.state_accept.generation =
                        event->state_offer.generation;
                    work.value.state_accept.owner =
                        event->state_offer.state.owner;
                    work.value.state_accept.version =
                        event->state_offer.state.version;
                    CHECK_GOTO(meiso_composition_debug_peer_queue_work(
                          host, 2U, &work
                        ) == MEISO_COMPOSITION_DEBUG_PEER_OK, cleanup);
                    actual_accept_pending = true;
                } else if (desired_version == 3U) {
                    MeisoStateReceiveStatus receive_status;

                    CHECK_GOTO(effective.selected_fields ==
                          (MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
                           MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM) &&
                          effective.sample_period_ms == 700U,
                          cleanup);
                    version3_actual_seen = true;
                    receive_status = meiso_state_replica_receive(
                        actual, event->state_offer.state.owner,
                        event->state_offer.state.version,
                        event->state_offer.state.payload
                    );
                    CHECK_GOTO((receive_status == MEISO_STATE_RECEIVE_NEW ||
                                receive_status ==
                                    MEISO_STATE_RECEIVE_DUPLICATE) &&
                          meiso_composition_debug_peer_commit_state_receive(
                              host, event->route, &event->state_offer,
                              receive_status
                          ) == MEISO_COMPOSITION_DEBUG_PEER_OK,
                          cleanup);
                    memset(&work, 0, sizeof(work));
                    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
                    work.value.state_accept.route = event->route;
                    work.value.state_accept.generation =
                        event->state_offer.generation;
                    work.value.state_accept.owner =
                        event->state_offer.state.owner;
                    work.value.state_accept.version =
                        event->state_offer.state.version;
                    CHECK_GOTO(meiso_composition_debug_peer_queue_work(
                          host, 2U, &work
                        ) == MEISO_COMPOSITION_DEBUG_PEER_OK, cleanup);
                    actual_accept_pending = true;
                } else {
                    CHECK_GOTO(false, cleanup);
                }
            }
        }

        memset(&edge_result, 0, sizeof(edge_result));
        edge_status = meiso_composition_edge_app_pump(edge, &io, &edge_result);
        CHECK_GOTO(benign_edge_status(edge_status), cleanup);
        memset(&interest, 0xa5, sizeof(interest));
        CHECK_GOTO(meiso_composition_edge_app_readiness(edge, &interest),
                   cleanup);
        if (!interest.control_read) {
            saw_control_read_suppressed = true;
        }
        CHECK_GOTO(meiso_composition_edge_app_snapshot(edge, &snapshot),
                   cleanup);
        if (version2_accept_seen && version3_accept_seen &&
            version2_actual_seen && version3_actual_seen &&
            desired_offer_completions == 2U &&
            actual_accept_completions >= 2U &&
            !actual_accept_pending &&
            snapshot.actual[MEISO_COMPOSITION_EDGE_APP_ACTUAL_WIFI]
                .delivery_accepted &&
            snapshot.actual[MEISO_COMPOSITION_EDGE_APP_ACTUAL_WIFI]
                .actual_state_version == 3U) {
            break;
        }
    }
    CHECK_GOTO(step < 40000U && version3_queued &&
          version2_accept_seen && version3_accept_seen &&
          version2_actual_seen && version3_actual_seen &&
          desired_offer_completions == 2U &&
          actual_accept_completions >= 2U &&
          saw_control_read_suppressed &&
          snapshot.wifi_effective_present &&
          snapshot.wifi_effective.selected_fields ==
              (MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
               MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM) &&
          snapshot.wifi_effective.sample_period_ms == 700U,
          cleanup);
    success = true;

cleanup:
    free(actual_arena);
    free(desired_arena);
    return success;
}

static bool test_init_and_control_bootstrap(void)
{
    EdgeFixtureShape fixture;
    MeisoCompositionEdgeApp *edge = NULL;
    MeisoCompositionDebugPeer *host = NULL;
    MeisoCompositionEdgeAppSnapshot edge_snapshot;
    MeisoCompositionDebugPeerSnapshot host_snapshot;
    void *edge_arena = NULL;
    void *host_arena = NULL;
    int media_host_fd = -1;
    int media_edge_fd = -1;
    MediaActualResult render_media;
    MediaActualResult camera_media;
    MediaPreacceptGate preaccept;
    HostMediaFixture host_media;
    MeisoCompositionEdgeAppMediaBindReceipt media_receipt;
    uint64_t camera_epoch = 0U;
    size_t step;
    bool ready = false;
    bool success = false;

    memset(&host_media, 0, sizeof(host_media));

    CHECK(fixture_shape_create(&fixture, UINT64_C(0x3000)));
    edge_arena = calloc(1U, fixture.edge_bytes);
    host_arena = calloc(1U, fixture.host_bytes);
    CHECK_GOTO(edge_arena != NULL && host_arena != NULL, cleanup);
    CHECK_GOTO(meiso_composition_debug_peer_init(
          host_arena, fixture.host_bytes, &fixture.host_config, &host
        ) == MEISO_COMPOSITION_DEBUG_PEER_INIT_OK, cleanup);
    CHECK_GOTO(meiso_composition_edge_app_init(
          edge_arena, fixture.edge_bytes, &fixture.edge_config, &edge
        ) == MEISO_COMPOSITION_EDGE_APP_INIT_OK, cleanup);
    CHECK_GOTO(meiso_composition_edge_app_snapshot(edge, &edge_snapshot),
               cleanup);
    CHECK_GOTO(edge_snapshot.phase == MEISO_COMPOSITION_EDGE_APP_PHASE_HANDSHAKE &&
          !edge_snapshot.control_ready &&
          edge_snapshot.active_catalog_generation == 0U &&
          edge_snapshot.arena_bytes == fixture.edge_bytes &&
          edge_snapshot.control_peer_bytes != 0U &&
          edge_snapshot.blob_store_bytes != 0U &&
          edge_snapshot.scene_store_bytes != 0U &&
          edge_snapshot.coordinator_bytes != 0U &&
          edge_snapshot.renderer_bytes != 0U &&
          edge_snapshot.camera_bytes != 0U &&
          edge_snapshot.core_state_bytes != 0U &&
          edge_snapshot.core_stream_bytes != 0U &&
          edge_snapshot.media_slot_bytes != 0U &&
          edge_snapshot.media_slots_capacity ==
              MEISO_COMPOSITION_EDGE_APP_MAX_MEDIA_CONNECTIONS &&
          edge_snapshot.media_slots_consumed == 0U &&
          fcntl(fixture.host_fd, F_GETFL, 0) == fixture.host_fd_flags &&
          fcntl(fixture.edge_fd, F_GETFL, 0) == fixture.edge_fd_flags,
          cleanup);

    for (step = 0U; step < 20000U && !ready; ++step) {
        MeisoCompositionDebugPeerPumpResult host_result;
        MeisoCompositionEdgeAppPumpResult edge_result;
        MeisoCompositionEdgeAppIoReady io = {
            true, true, false, false, false, {77U, step + 1U}
        };
        MeisoCompositionDebugPeerPumpStatus host_status;
        MeisoCompositionEdgeAppPumpStatus edge_status;

        memset(&host_result, 0, sizeof(host_result));
        host_status = meiso_composition_debug_peer_pump(
            host, true, true, &host_result
        );
        if (!benign_host_status(host_status)) {
            (void)meiso_composition_debug_peer_snapshot(
                host, &host_snapshot
            );
            (void)meiso_composition_edge_app_snapshot(
                edge, &edge_snapshot
            );
            (void)fprintf(
                stderr,
                "host bootstrap status=%u step=%zu phase=%u session=%u "
                "terminal=%u staged=%u active=%u generation=%llu "
                "runtime=%zu/%zu scheduler=%zu/%zu edge_phase=%u\n",
                (unsigned)host_status, step,
                (unsigned)host_snapshot.phase,
                (unsigned)host_snapshot.session.state,
                host_snapshot.terminal ? 1U : 0U,
                host_snapshot.runtime.staged_catalog_present ? 1U : 0U,
                host_snapshot.runtime.active_catalog_present ? 1U : 0U,
                (unsigned long long)host_snapshot.catalog_generation,
                host_snapshot.runtime_bytes,
                host_snapshot.runtime_reserve_bytes,
                host_snapshot.scheduler_bytes,
                host_snapshot.scheduler_reserve_bytes,
                (unsigned)edge_snapshot.phase
            );
        }
        CHECK_GOTO(benign_host_status(host_status), cleanup);
        memset(&edge_result, 0, sizeof(edge_result));
        edge_status = meiso_composition_edge_app_pump(edge, &io, &edge_result);
        if (!benign_edge_status(edge_status)) {
            (void)fprintf(
                stderr, "edge bootstrap status=%u step=%zu\n",
                (unsigned)edge_status, step
            );
        }
        CHECK_GOTO(benign_edge_status(edge_status), cleanup);
        CHECK_GOTO(meiso_composition_edge_app_snapshot(edge, &edge_snapshot) &&
              meiso_composition_debug_peer_snapshot(host, &host_snapshot),
              cleanup);
        ready = edge_snapshot.phase == MEISO_COMPOSITION_EDGE_APP_PHASE_READY &&
            host_snapshot.phase == MEISO_COMPOSITION_DEBUG_PEER_PHASE_READY;
    }
    CHECK_GOTO(ready && edge_snapshot.control_ready &&
          edge_snapshot.control_session_state == MEISO_SESSION_STATE_READY &&
          edge_snapshot.active_catalog_generation == 1U &&
          host_snapshot.session.state == MEISO_SESSION_STATE_READY &&
          host_snapshot.runtime.active_catalog_present &&
          host_snapshot.runtime.active_catalog_generation == 1U &&
          host_snapshot.runtime.active_endpoint_count == 6U &&
          host_snapshot.runtime.active_route_count == 18U &&
          edge_snapshot.media_slots_consumed == 0U &&
          fcntl(fixture.host_fd, F_GETFL, 0) == fixture.host_fd_flags &&
          fcntl(fixture.edge_fd, F_GETFL, 0) == fixture.edge_fd_flags,
          cleanup);
    CHECK_GOTO(drive_wifi_desired_actual(
          edge, host, &fixture.catalog
        ), cleanup);
    CHECK_GOTO(drive_wifi_back_to_back_replacement(
          edge, host, &fixture.catalog
        ), cleanup);
    CHECK_GOTO(make_tcp_pair(&media_host_fd, &media_edge_fd), cleanup);
    CHECK_GOTO(drive_media_desired_actual(
          edge, host, &fixture.catalog, true, 1U, 10000U,
          NULL, &render_media
        ), cleanup);
    memset(&preaccept, 0, sizeof(preaccept));
    preaccept.edge_arena = edge_arena;
    preaccept.edge_bytes = fixture.edge_bytes;
    preaccept.media_edge_fd = media_edge_fd;
    CHECK_GOTO(drive_media_desired_actual(
          edge, host, &fixture.catalog, false, 1U, 10000U,
          &preaccept, &camera_media
        ) && preaccept.checked &&
          render_media.snapshot.delivery_accepted &&
          camera_media.snapshot.delivery_accepted &&
          render_media.actual.source ==
              MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW &&
          camera_media.actual.source ==
              MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA &&
          render_media.snapshot.production_epoch != 0U &&
          camera_media.snapshot.production_epoch != 0U,
          cleanup);
    CHECK_GOTO(meiso_composition_edge_app_snapshot(edge, &edge_snapshot),
               cleanup);
    CHECK_GOTO(initialize_host_media(
          &fixture, &edge_snapshot, media_host_fd,
          &render_media, &camera_media, &host_media
        ), cleanup);
    CHECK_GOTO(drive_both_media_active(
          edge, edge_arena, fixture.edge_bytes,
          media_edge_fd, media_host_fd, &host_media, 0U, &media_receipt
        ), cleanup);
    CHECK_GOTO(media_receipt.active_route_mask ==
          MEISO_COMPOSITION_EDGE_APP_MEDIA_ALL, cleanup);
    CHECK_GOTO(drive_camera_start_failure(
          edge, host, &fixture.catalog, &fixture.camera,
          MEISO_COMPOSITION_EDGE_APP_MEDIA_CAMERA, 1U
        ), cleanup);
    destroy_host_media(&host_media);
    close_fd(&media_host_fd);
    close_fd(&media_edge_fd);
    fixture.camera.succeed = true;
    CHECK_GOTO(make_tcp_pair(&media_host_fd, &media_edge_fd), cleanup);
    CHECK_GOTO(drive_media_desired_actual(
          edge, host, &fixture.catalog, false, 2U, 10000U,
          NULL, &camera_media
        ), cleanup);
    CHECK_GOTO(camera_media.snapshot.production_epoch != 0U, cleanup);
    CHECK_GOTO(meiso_composition_edge_app_snapshot(edge, &edge_snapshot) &&
          initialize_host_media(
              &fixture, &edge_snapshot, media_host_fd,
              &render_media, &camera_media, &host_media
          ) &&
          drive_both_media_active(
              edge, edge_arena, fixture.edge_bytes,
              media_edge_fd, media_host_fd, &host_media, 1U, &media_receipt
          ) && media_receipt.slot_index == 1U,
          cleanup);
    CHECK_GOTO(drive_camera_fixed_point_cadence(
          edge, &host_media, media_edge_fd, media_host_fd, &fixture.camera
        ), cleanup);
    CHECK_GOTO(drive_camera_next_due(
          edge, &host_media, media_edge_fd, media_host_fd, &fixture.camera,
          UINT64_C(2199999999), UINT64_C(2200000000), 3U
        ), cleanup);
    camera_epoch = camera_media.snapshot.production_epoch;
    CHECK_GOTO(drive_media_desired_actual(
          edge, host, &fixture.catalog, false, 3U, 10000U,
          NULL, &camera_media
        ) && camera_media.snapshot.production_epoch != camera_epoch &&
          meiso_composition_edge_app_snapshot(edge, &edge_snapshot) &&
          !edge_snapshot.media.bound &&
          edge_snapshot.media_slots_consumed == 2U,
          cleanup);
    destroy_host_media(&host_media);
    close_fd(&media_host_fd);
    close_fd(&media_edge_fd);
    CHECK_GOTO(make_tcp_pair(&media_host_fd, &media_edge_fd) &&
          initialize_host_media(
              &fixture, &edge_snapshot, media_host_fd,
              &render_media, &camera_media, &host_media
          ) &&
          drive_both_media_active(
              edge, edge_arena, fixture.edge_bytes,
              media_edge_fd, media_host_fd, &host_media, 2U, &media_receipt
          ) && media_receipt.slot_index == 2U &&
          drive_camera_epoch_reset_first_due(
              edge, &host_media, media_edge_fd, media_host_fd,
              &fixture.camera, UINT64_C(2200000001), 3U
          ), cleanup);
    CHECK_GOTO(drive_empty_scene_world_swaps_and_disable(
          edge, host, &fixture.catalog, 2U, 3U
        ), cleanup);
    CHECK_GOTO(trigger_active_media_transport_loss(
          edge, &media_host_fd, media_edge_fd
        ) && drive_camera_start_failure(
          edge, host, &fixture.catalog, &fixture.camera,
          MEISO_COMPOSITION_EDGE_APP_MEDIA_ALL, 3U
        ), cleanup);
    success = true;

cleanup:
    destroy_host_media(&host_media);
    close_fd(&media_host_fd);
    close_fd(&media_edge_fd);
    if (edge != NULL) {
        (void)meiso_composition_edge_app_abort(
            edge, MEISO_COMPOSITION_EDGE_APP_TERMINAL_LOCAL_ABORT, ECANCELED
        );
    }
    if (host != NULL) {
        meiso_composition_debug_peer_abort(host, ECANCELED);
    }
    free(host_arena);
    free(edge_arena);
    fixture_destroy(&fixture);
    return success;
}

static bool test_fixture_shape_checkpoint(void)
{
    EdgeFixtureShape fixture;
    bool ok;

    CHECK(meiso_composition_edge_app_alignment_required() ==
          alignof(max_align_t));
    ok = fixture_shape_create(&fixture, UINT64_C(0x2000));
    if (ok) {
        CHECK(fixture.edge_config.edge_clock_epoch == 77U &&
              fixture.edge_config.initial_actual_delivery_generation.value ==
                  101U &&
              fixture.edge_config.media_connection_slots ==
                  MEISO_COMPOSITION_EDGE_APP_MAX_MEDIA_CONNECTIONS &&
              fixture.catalog.routes[ROUTE_RENDER_EXTERNAL].delivery ==
                  MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA &&
              fixture.catalog.routes[ROUTE_CAMERA_EXTERNAL].delivery ==
                  MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA);
    }
    fixture_destroy(&fixture);
    return ok;
}

int main(void)
{
    if (!test_fixture_shape_checkpoint() ||
        !test_init_and_control_bootstrap()) {
        return 1;
    }
    (void)puts("composition edge app init/bootstrap: PASS");
    return 0;
}
