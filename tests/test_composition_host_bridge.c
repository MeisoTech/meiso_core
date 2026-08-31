#define _POSIX_C_SOURCE 200809L

#include <meiso/composition/control_session.h>
#include <meiso/adapters/posix/composition_media_tcp.h>
#include <meiso/profiles/composition/media_transport.h>
#include <meiso/profiles/composition/profile.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

int meiso_composition_host_bridge_run(int argc, char **argv);

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            (void)fprintf(                                                     \
                stderr, "CHECK failed at %s:%d: %s\n",                     \
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
    CONFIG_BYTES = 4096,
    INPUT_BYTES = 65536,
    OUTPUT_BYTES = 131072,
    ERROR_BYTES = 4096,
    MEDIA_SCENARIO_TIMEOUT_SECONDS = 20,
    MEDIA_SCENARIO_MAX_PUMP_STEPS = 10000000,
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
    int control_listener;
    int media_listener;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool render_active;
    bool media_active;
    uint32_t media_accept_count;
    uint32_t stale_accept_count;
    bool service_done;
    bool failed;
    uint32_t failure_stage;
    uint32_t failure_status;
} MediaScenario;

typedef struct {
    const char *bytes;
    size_t size;
    int fd;
    int error_number;
    uint32_t hold_after_write_ms;
} Writer;

typedef struct {
    int listener;
    bool accepted;
    bool saw_client_byte;
    int error_number;
} TransportCloseServer;

typedef struct {
    MediaScenario *scenario;
    int fd;
    int error_number;
} MediaWriter;

typedef struct {
    int exit_code;
    char output[OUTPUT_BYTES];
    size_t output_size;
    char error[ERROR_BYTES];
    size_t error_size;
    int writer_error;
} RunResult;

static bool make_deadline(struct timespec *out_deadline, time_t seconds);

static void close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static bool append_text(
    char *buffer,
    size_t capacity,
    size_t *size,
    const char *format,
    ...
)
{
    va_list arguments;
    int written;

    if (buffer == NULL || size == NULL || format == NULL ||
        *size >= capacity) {
        return false;
    }
    va_start(arguments, format);
    written = vsnprintf(buffer + *size, capacity - *size, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= capacity - *size) {
        return false;
    }
    *size += (size_t)written;
    return true;
}

static bool write_all(int fd, const char *bytes, size_t size)
{
    size_t offset = 0U;

    while (offset < size) {
        ssize_t result = write(fd, bytes + offset, size - offset);
        if (result > 0) {
            offset += (size_t)result;
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static void *writer_main(void *context)
{
    Writer *writer = context;

    if (!write_all(writer->fd, writer->bytes, writer->size)) {
        writer->error_number = errno == 0 ? EIO : errno;
    } else if (writer->hold_after_write_ms > 0U) {
        struct timespec remaining = {
            (time_t)(writer->hold_after_write_ms / 1000U),
            (long)(writer->hold_after_write_ms % 1000U) * 1000000L
        };

        while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
        }
    }
    close_fd(&writer->fd);
    return NULL;
}

static void *media_writer_main(void *context)
{
    static const char render_command[] =
        "{\"command\":\"set_render_media\",\"request_id\":1,"
        "\"enabled\":true,\"pixel_format\":\"rgba8888\","
        "\"codec\":\"raw\",\"width\":320,\"height\":240,"
        "\"fps_millihz\":10000,\"bitrate_bps\":0,"
        "\"gop_frames\":1,\"request_key\":true}\n";
    static const char snapshot_command[] =
        "{\"command\":\"snapshot\",\"request_id\":3}\n";
    static const char camera_command[] =
        "{\"command\":\"set_camera_media\",\"request_id\":2,"
        "\"enabled\":true,\"pixel_format\":\"nv12\","
        "\"codec\":\"raw\",\"width\":320,\"height\":240,"
        "\"fps_millihz\":10000,\"bitrate_bps\":0,"
        "\"gop_frames\":1,\"request_key\":true}\n";
    static const char close_command[] =
        "{\"command\":\"close\",\"request_id\":4}\n";
    MediaWriter *writer = context;
    struct timespec deadline;
    bool active = false;

    if (!write_all(writer->fd, render_command, sizeof(render_command) - 1U) ||
        !make_deadline(&deadline, MEDIA_SCENARIO_TIMEOUT_SECONDS)) {
        writer->error_number = errno == 0 ? EIO : errno;
        close_fd(&writer->fd);
        return NULL;
    }
    (void)pthread_mutex_lock(&writer->scenario->mutex);
    while (!writer->scenario->render_active && !writer->scenario->failed) {
        if (pthread_cond_timedwait(
                &writer->scenario->condition, &writer->scenario->mutex,
                &deadline
            ) == ETIMEDOUT) {
            break;
        }
    }
    active = writer->scenario->render_active && !writer->scenario->failed;
    (void)pthread_mutex_unlock(&writer->scenario->mutex);
    if (!active ||
        !write_all(
            writer->fd, camera_command, sizeof(camera_command) - 1U
        )) {
        writer->error_number = errno == 0 ? ETIMEDOUT : errno;
        close_fd(&writer->fd);
        return NULL;
    }
    (void)pthread_mutex_lock(&writer->scenario->mutex);
    while (!writer->scenario->media_active && !writer->scenario->failed) {
        if (pthread_cond_timedwait(
                &writer->scenario->condition, &writer->scenario->mutex,
                &deadline
            ) == ETIMEDOUT) {
            break;
        }
    }
    active = writer->scenario->media_active && !writer->scenario->failed;
    (void)pthread_mutex_unlock(&writer->scenario->mutex);
    if (!active ||
        !write_all(
            writer->fd, snapshot_command, sizeof(snapshot_command) - 1U
        )) {
        writer->error_number = errno == 0 ? ETIMEDOUT : errno;
        close_fd(&writer->fd);
        return NULL;
    }
    while (poll(NULL, 0U, 100) < 0 && errno == EINTR) {
    }
    if (!write_all(writer->fd, close_command, sizeof(close_command) - 1U)) {
        writer->error_number = errno == 0 ? EIO : errno;
    }
    close_fd(&writer->fd);
    return NULL;
}

static bool read_all(
    int fd,
    char *buffer,
    size_t capacity,
    size_t *out_size
)
{
    size_t size = 0U;

    if (buffer == NULL || capacity == 0U || out_size == NULL) {
        return false;
    }
    while (size + 1U < capacity) {
        ssize_t result = read(fd, buffer + size, capacity - size - 1U);
        if (result > 0) {
            size += (size_t)result;
        } else if (result == 0) {
            buffer[size] = '\0';
            *out_size = size;
            return true;
        } else if (errno != EINTR) {
            return false;
        }
    }
    return false;
}

static bool make_control_socket(
    bool listening,
    int *out_fd,
    uint16_t *out_port
)
{
    struct sockaddr_in address;
    socklen_t address_size = (socklen_t)sizeof(address);
    int fd = -1;
    int enabled = 1;

    if (out_fd == NULL || out_port == NULL) {
        return false;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || setsockopt(
            fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)
        ) != 0) {
        close_fd(&fd);
        return false;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        close_fd(&fd);
        return false;
    }
    if (bind(fd, (const struct sockaddr *)(const void *)&address,
             sizeof(address)) != 0 ||
        (listening && listen(fd, 1) != 0) ||
        getsockname(fd, (struct sockaddr *)(void *)&address,
                    &address_size) != 0) {
        close_fd(&fd);
        return false;
    }
    *out_fd = fd;
    *out_port = ntohs(address.sin_port);
    return *out_port != 0U;
}

static bool set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);

    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool make_deadline(struct timespec *out_deadline, time_t seconds)
{
    if (out_deadline == NULL || seconds <= 0 ||
        clock_gettime(CLOCK_REALTIME, out_deadline) != 0) {
        return false;
    }
    out_deadline->tv_sec += seconds;
    return true;
}

static bool deadline_pending(const struct timespec *deadline)
{
    struct timespec now;

    return deadline != NULL && clock_gettime(CLOCK_REALTIME, &now) == 0 &&
        (now.tv_sec < deadline->tv_sec ||
         (now.tv_sec == deadline->tv_sec &&
          now.tv_nsec < deadline->tv_nsec));
}

static int accept_nonblocking(int listener)
{
    struct pollfd item = {listener, POLLIN, 0};
    struct timespec deadline;

    if (!make_deadline(&deadline, MEDIA_SCENARIO_TIMEOUT_SECONDS)) {
        return -1;
    }
    while (deadline_pending(&deadline)) {
        int fd = accept(listener, NULL, NULL);
        int poll_result;

        if (fd >= 0) {
            if (!set_nonblocking(fd)) {
                close_fd(&fd);
            }
            return fd;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        poll_result = poll(&item, (nfds_t)1U, 100);
        if (poll_result < 0 && errno == EINTR) {
            continue;
        }
        if (poll_result < 0 || (item.revents & POLLNVAL) != 0) {
            return -1;
        }
        item.revents = 0;
    }
    return -1;
}

static void *transport_close_server_main(void *context)
{
    TransportCloseServer *server = context;
    struct pollfd item;
    int fd = accept_nonblocking(server->listener);
    char byte;
    int poll_status;

    if (fd < 0) {
        server->error_number = errno == 0 ? EIO : errno;
        return NULL;
    }
    server->accepted = true;
    item = (struct pollfd){fd, POLLIN, 0};
    do {
        poll_status = poll(&item, (nfds_t)1U, 5000);
    } while (poll_status < 0 && errno == EINTR);
    if (poll_status <= 0 ||
        (item.revents & (POLLIN | POLLERR | POLLHUP)) == 0 ||
        recv(fd, &byte, 1U, 0) != 1) {
        server->error_number = errno == 0 ? EIO : errno;
    } else {
        server->saw_client_byte = true;
    }
    (void)shutdown(fd, SHUT_RDWR);
    close_fd(&fd);
    return NULL;
}

static bool fd_ready_now(int fd, short events)
{
    struct pollfd item = {fd, events, 0};

    return poll(&item, (nfds_t)1U, 0) > 0 &&
        (item.revents & (short)(events | POLLERR | POLLHUP)) != 0;
}

static MeisoCompositionProfileCatalogConfig service_catalog(void)
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
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE,
        MEISO_WIFI_TELEMETRY_MIN_PERIOD_MS,
        MEISO_WIFI_TELEMETRY_MAX_PERIOD_MS,
        MEISO_WIFI_TELEMETRY_PERIOD_STEP_MS
    };
    value.device_telemetry_capability = (MeisoDeviceTelemetryCapability){
        MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC,
        MEISO_DEVICE_TELEMETRY_MIN_PERIOD_MS,
        MEISO_DEVICE_TELEMETRY_MAX_PERIOD_MS,
        MEISO_DEVICE_TELEMETRY_PERIOD_STEP_MS,
        -40000, 125000, 3000U
    };
    value.render_media_capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH,
        MEISO_COMPOSITION_SCENE_TARGET_HEIGHT,
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH,
        MEISO_COMPOSITION_SCENE_TARGET_HEIGHT,
        MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ * 1000U,
        MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES,
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE, 0U
    };
    value.camera_media_capability = (MeisoCompositionMediaCapability){
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        MEISO_COMPOSITION_MEDIA_PIXEL_MASK_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        320U, 240U, 320U, 240U, 30000U,
        320U * 240U * 3U / 2U,
        MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE, 0U
    };
    return value;
}

static void configure_edge_source(
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
    static const MeisoRuntimeSchedulerWorkKind kinds[17] = {
        MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
        MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
        MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
        MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
        MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER
    };
    static const uint8_t routes[17] = {
        MEISO_COMPOSITION_CONTROL_SESSION_NO_ROUTE,
        0U, 1U, 2U, 7U, 3U, 4U, 5U, 6U, 8U, 9U, 10U, 11U, 12U,
        13U, 15U, 16U
    };
    size_t index;

    for (index = 0U; index < 17U; ++index) {
        configure_edge_source(
            config, (uint8_t)index, kinds[index], routes[index]
        );
    }
    for (; index < MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT; ++index) {
        configure_edge_source(
            config, (uint8_t)index,
            MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER, 1U
        );
    }
}

static bool build_config(
    char *buffer,
    size_t capacity,
    uint16_t control_port,
    uint16_t media_port,
    const char *extra,
    const char *omit_key
)
{
    static const struct {
        const char *key;
        const char *value;
    } entries[] = {
        {"control_ip", "127.0.0.1"},
        {"media_ip", "127.0.0.1"},
        {"local_host_peer_id_high", "1"},
        {"local_host_peer_id_low", "2"},
        {"expected_edge_peer_id_high", "3"},
        {"expected_edge_peer_id_low", "4"},
        {"local_incarnation", "5"},
        {"nonce_high", "6"},
        {"nonce_low", "7"},
        {"target_manifest", "k230d_zero_gc2093_v1"},
        {"wifi_supported_fields", "1"},
        {"device_supported_fields", "1"},
        {"catalog_generation", "1"},
        {"wifi_incarnation", "11"},
        {"presentation_incarnation", "12"},
        {"scene_incarnation", "13"},
        {"device_incarnation", "14"},
        {"render_incarnation", "15"},
        {"camera_incarnation", "16"}
    };
    size_t size = 0U;
    size_t index;

    if (!append_text(buffer, capacity, &size, "control_port=%u\n",
                     (unsigned)control_port) ||
        !append_text(buffer, capacity, &size, "media_port=%u\n",
                     (unsigned)media_port)) {
        return false;
    }
    for (index = 0U; index < sizeof(entries) / sizeof(entries[0]); ++index) {
        if (omit_key != NULL && strcmp(omit_key, entries[index].key) == 0) {
            continue;
        }
        if (!append_text(buffer, capacity, &size, "%s=%s\n",
                         entries[index].key, entries[index].value)) {
            return false;
        }
    }
    return extra == NULL || append_text(buffer, capacity, &size, "%s", extra);
}

static bool write_config_file(
    const char *contents,
    char *out_path,
    size_t path_capacity
)
{
    static const char path_template[] =
        "/tmp/meiso-composition-host-bridge-XXXXXX";
    int fd;
    size_t size;

    if (contents == NULL || out_path == NULL ||
        path_capacity < sizeof(path_template)) {
        return false;
    }
    memcpy(out_path, path_template, sizeof(path_template));
    fd = mkstemp(out_path);
    if (fd < 0) {
        return false;
    }
    size = strlen(contents);
    if (!write_all(fd, contents, size) || close(fd) != 0) {
        close_fd(&fd);
        (void)unlink(out_path);
        return false;
    }
    return true;
}

static bool edge_pump_benign(MeisoCompositionControlSessionPumpStatus status)
{
    return status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_PROGRESS ||
        status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_IDLE ||
        status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_EVENT ||
        status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_WOULD_BLOCK ||
        status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_TX_BUSY ||
        status == MEISO_COMPOSITION_CONTROL_SESSION_PUMP_REBIND_PROGRESS;
}

static void scenario_signal(
    MediaScenario *scenario,
    bool render_active,
    bool media_active,
    bool failed
)
{
    (void)pthread_mutex_lock(&scenario->mutex);
    scenario->render_active = scenario->render_active || render_active;
    scenario->media_active = scenario->media_active || media_active;
    scenario->failed = scenario->failed || failed;
    (void)pthread_cond_broadcast(&scenario->condition);
    (void)pthread_mutex_unlock(&scenario->mutex);
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

static MeisoCompositionMediaDesired render_desired(void)
{
    return (MeisoCompositionMediaDesired){
        MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH,
        MEISO_COMPOSITION_SCENE_TARGET_HEIGHT,
        MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ * 1000U,
        0U, 1U, MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
    };
}

static MeisoCompositionMediaDesired camera_desired(void)
{
    return (MeisoCompositionMediaDesired){
        MEISO_COMPOSITION_MEDIA_PIXEL_NV12,
        MEISO_COMPOSITION_MEDIA_CODEC_RAW,
        320U, 240U, 10000U, 0U, 1U,
        MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY
    };
}

static bool media_desired_equal(
    const MeisoCompositionMediaDesired *lhs,
    const MeisoCompositionMediaDesired *rhs
)
{
    return lhs->pixel_format == rhs->pixel_format &&
        lhs->codec == rhs->codec && lhs->width == rhs->width &&
        lhs->height == rhs->height &&
        lhs->fps_millihz == rhs->fps_millihz &&
        lhs->bitrate_bps == rhs->bitrate_bps &&
        lhs->gop_frames == rhs->gop_frames &&
        lhs->recovery_flags == rhs->recovery_flags;
}

static bool initialize_edge_media(
    MediaScenario *scenario,
    const CatalogStorage *catalog,
    MeisoSessionId session_id,
    int fd,
    uint8_t active_route_mask,
    void **out_transport_arena,
    MeisoCompositionMediaTransport **out_transport,
    void **out_adapter_arena,
    MeisoCompositionMediaPosixTcp **out_adapter
)
{
    const MeisoCompositionMediaDesired desired[2] = {
        render_desired(), camera_desired()
    };
    const uint8_t route_indices[2] = {
        ROUTE_RENDER_EXTERNAL, ROUTE_CAMERA_EXTERNAL
    };
    const MeisoCompositionMediaSource sources[2] = {
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA
    };
    MeisoCompositionMediaTransportConfig config;
    size_t transport_bytes;
    size_t adapter_bytes;
    size_t alignment = 0U;
    size_t index;

    if (scenario == NULL || catalog == NULL || fd < 0 ||
        active_route_mask == 0U ||
        (active_route_mask &
         (uint8_t)~MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_ALL_MASK) != 0U) {
        return false;
    }
    memset(&config, 0, sizeof(config));
    config.local_role = MEISO_SESSION_ROLE_EDGE;
    config.active_route_mask = active_route_mask;
    config.scope.session_id = session_id;
    config.scope.catalog_generation = 1U;
    config.routes[0].capability = service_catalog().render_media_capability;
    config.routes[1].capability = service_catalog().camera_media_capability;
    for (index = 0U; index < 2U; ++index) {
        MeisoCompositionMediaRouteConfig *route = &config.routes[index];
        if ((active_route_mask & (uint8_t)(UINT8_C(1) << index)) == 0U) {
            continue;
        }
        route->binding.session_id = session_id;
        route->binding.catalog_generation = 1U;
        route->binding.endpoint_id =
            catalog->routes[route_indices[index]].route.endpoint_id;
        route->binding.endpoint_incarnation =
            catalog->routes[route_indices[index]].route.endpoint_incarnation;
        route->binding.resource_id =
            catalog->routes[route_indices[index]].route.resource.id;
        route->binding.source = sources[index];
        route->binding.production_epoch = 501U + (uint64_t)index;
        route->binding.media_stream_epoch = 601U + (uint64_t)index;
        route->binding.channel_nonce = (MeisoSessionNonce){
            701U + (uint64_t)index, 711U + (uint64_t)index
        };
        route->effective = desired[index];
    }
    {
        MeisoCompositionMediaTransportStatus status =
            meiso_composition_media_transport_memory_required(
                &config, &transport_bytes, &alignment
            );
        if (status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK ||
        transport_bytes == 0U || alignment != alignof(max_align_t)) {
            scenario->failure_stage = 101U;
            scenario->failure_status = status;
            return false;
        }
    }
    *out_transport_arena = calloc(1U, transport_bytes);
    if (*out_transport_arena == NULL) {
        scenario->failure_stage = 102U;
        return false;
    }
    {
        MeisoCompositionMediaTransportStatus status =
            meiso_composition_media_transport_init(
                &config,
                (MeisoMutableBuffer){*out_transport_arena, transport_bytes},
                out_transport
            );
        if (status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
            scenario->failure_stage = 103U;
            scenario->failure_status = status;
            return false;
        }
    }
    adapter_bytes = meiso_composition_media_posix_tcp_memory_required(
        &(MeisoCompositionMediaPosixTcpConfig){
            fd, *out_transport, config.scope, 17U
        }
    );
    *out_adapter_arena = calloc(1U, adapter_bytes);
    if (adapter_bytes == 0U || *out_adapter_arena == NULL) {
        scenario->failure_stage = 104U;
        return false;
    }
    {
        MeisoCompositionMediaPosixTcpInitStatus status =
            meiso_composition_media_posix_tcp_init(
            *out_adapter_arena, adapter_bytes,
            &(MeisoCompositionMediaPosixTcpConfig){
                fd, *out_transport, config.scope, 17U
            }, out_adapter
        );
        if (status != MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_OK) {
            scenario->failure_stage = 105U;
            scenario->failure_status = status;
            return false;
        }
    }
    return true;
}

static void *edge_media_main(void *context)
{
    static const MeisoPeerId host_peer = {1U, 2U};
    static const MeisoPeerId edge_peer = {3U, 4U};
    static const MeisoOwnerScope host_owner = {MEISO_OWNER_ROLE_HOST, 5U};
    static const MeisoOwnerScope edge_owner = {MEISO_OWNER_ROLE_EDGE, 41U};
    const MeisoStateConfig state_config = {MEISO_STATE_DELIVERY_CONVERGENT};
    const uint8_t desired_routes[2] = {
        ROUTE_RENDER_DESIRED, ROUTE_CAMERA_DESIRED
    };
    const uint8_t actual_routes[2] = {
        ROUTE_RENDER_ACTUAL, ROUTE_CAMERA_ACTUAL
    };
    const uint8_t desired_sources[2] = {13U, 15U};
    const uint8_t actual_sources[2] = {14U, 16U};
    const MeisoCompositionMediaDesired desired_values[2] = {
        render_desired(), camera_desired()
    };
    const MeisoCompositionMediaSource media_sources[2] = {
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA
    };
    MediaScenario *scenario = context;
    MeisoCompositionControlSessionConfig peer_config;
    CatalogStorage catalog;
    int control_fd = -1;
    int media_fd = -1;
    void *peer_arena = NULL;
    void *replica_arenas[2] = {NULL, NULL};
    void *publisher_arenas[2] = {NULL, NULL};
    void *transport_arena = NULL;
    void *adapter_arena = NULL;
    MeisoCompositionControlSession *peer = NULL;
    MeisoStateReplica *replicas[2] = {NULL, NULL};
    MeisoStatePublisher *publishers[2] = {NULL, NULL};
    MeisoCompositionMediaTransport *transport = NULL;
    MeisoCompositionMediaPosixTcp *adapter = NULL;
    size_t peer_bytes;
    size_t replica_bytes[2];
    size_t publisher_bytes[2];
    MeisoRuntimeRouteKey desired_route_keys[2];
    MeisoRuntimeRouteKey actual_route_keys[2];
    bool desired_received[2] = {false, false};
    bool desired_accept_sent[2] = {false, false};
    bool actual_queued[2] = {false, false};
    bool actual_offer_sent[2] = {false, false};
    bool actual_accepted[2] = {false, false};
    bool media_initialized = false;
    uint8_t active_media_mask = 0U;
    bool camera_delay_started = false;
    uint32_t camera_delay_steps = 0U;
    bool ok = false;
    struct timespec pump_deadline;
    size_t pump_steps = 0U;
    size_t index;

    control_fd = accept_nonblocking(scenario->control_listener);
    if (control_fd < 0) {
        goto done;
    }
    memset(&peer_config, 0, sizeof(peer_config));
    peer_config.connected_fd = control_fd;
    peer_config.local_role = MEISO_SESSION_ROLE_EDGE;
    peer_config.local_peer_id = edge_peer;
    peer_config.local_incarnation = edge_owner.incarnation;
    peer_config.local_nonce = (MeisoSessionNonce){8U, 9U};
    peer_config.expected_peer_id = host_peer;
    peer_config.protocol = (MeisoProtocolVersion){1U, 0U};
    peer_config.catalog_generation = 1U;
    peer_config.catalog = service_catalog();
    peer_config.max_io_bytes_per_step = 17U;
    peer_config.adapter_reserve_bytes = 65536U;
    peer_config.runtime_reserve_bytes = 400000U;
    peer_config.scheduler_reserve_bytes = 65536U;
    configure_edge_sources(&peer_config);
    if (meiso_composition_profile_build_catalog(
            &peer_config.catalog,
            (MeisoMutableBuffer){catalog.capability,
                                 sizeof(catalog.capability)},
            catalog.endpoints, MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT,
            catalog.routes, MEISO_COMPOSITION_PROFILE_ROUTE_COUNT
        ) != MEISO_COMPOSITION_PROFILE_OK) {
        goto done;
    }
    peer_bytes = meiso_composition_control_session_memory_required(&peer_config);
    peer_arena = calloc(1U, peer_bytes);
    if (peer_bytes == 0U || peer_arena == NULL ||
        meiso_composition_control_session_init(
            peer_arena, peer_bytes, &peer_config, &peer
        ) != MEISO_COMPOSITION_CONTROL_SESSION_INIT_OK) {
        goto done;
    }
    for (index = 0U; index < 2U; ++index) {
        replica_bytes[index] = meiso_state_replica_memory_required(
            &catalog.routes[desired_routes[index]].route.resource
        );
        publisher_bytes[index] = meiso_state_publisher_memory_required(
            &catalog.routes[actual_routes[index]].route.resource, &state_config
        );
        replica_arenas[index] = calloc(1U, replica_bytes[index]);
        publisher_arenas[index] = calloc(1U, publisher_bytes[index]);
        if (replica_bytes[index] == 0U || publisher_bytes[index] == 0U ||
            replica_arenas[index] == NULL || publisher_arenas[index] == NULL ||
            meiso_state_replica_init(
                replica_arenas[index], replica_bytes[index],
                &catalog.routes[desired_routes[index]].route.resource,
                &replicas[index]
            ) != MEISO_STATE_INIT_OK ||
            meiso_state_replica_begin_owner(replicas[index], host_owner) !=
                MEISO_STATE_OWNER_STARTED ||
            meiso_state_publisher_init(
                publisher_arenas[index], publisher_bytes[index],
                &catalog.routes[actual_routes[index]].route.resource,
                &state_config, &publishers[index]
            ) != MEISO_STATE_INIT_OK) {
            goto done;
        }
    }

    if (!make_deadline(
            &pump_deadline, MEDIA_SCENARIO_TIMEOUT_SECONDS
        )) {
        goto done;
    }
    while (deadline_pending(&pump_deadline) &&
           pump_steps < MEDIA_SCENARIO_MAX_PUMP_STEPS) {
        MeisoCompositionControlSessionPumpResult result;
        MeisoCompositionControlSessionPumpStatus status;
        MeisoCompositionControlSessionSnapshot peer_snapshot;
        MeisoRuntimeSchedulerCompletion completion;

        memset(&result, 0, sizeof(result));
        status = meiso_composition_control_session_pump(
            peer, fd_ready_now(control_fd, POLLIN),
            fd_ready_now(control_fd, POLLOUT), &result
        );
        if (!edge_pump_benign(status)) {
            scenario->failure_stage = 201U;
            scenario->failure_status = status;
            goto done;
        }
        if (result.event_present) {
            if (result.event.type == MEISO_RUNTIME_EVENT_STATE_OFFER) {
                MeisoRuntimeDesiredState wrapper;
                MeisoCompositionMediaDesired decoded;
                MeisoStateReceiveStatus receive;
                MeisoRuntimeSchedulerWork accept;
                bool matched = false;

                for (index = 0U; index < 2U; ++index) {
                    if (!meiso_composition_control_session_route_key(
                            peer, desired_routes[index],
                            &desired_route_keys[index]
                        ) || !route_equal(
                            result.event.route, desired_route_keys[index]
                        )) {
                        continue;
                    }
                    if (desired_received[index] ||
                        (index == 1U && !scenario->render_active)) {
                        scenario->failure_stage = 202U;
                        goto done;
                    }
                    receive = meiso_state_replica_receive(
                        replicas[index],
                        result.event.state_offer.state.owner,
                        result.event.state_offer.state.version,
                        result.event.state_offer.state.payload
                    );
                    if (receive != MEISO_STATE_RECEIVE_NEW ||
                        result.event.state_offer.state.version != 1U ||
                        !meiso_runtime_desired_state_decode(
                            result.event.state_offer.state.payload, &wrapper
                        ) || !wrapper.enabled ||
                        meiso_composition_media_desired_decode(
                            wrapper.profile, &decoded
                        ) != MEISO_COMPOSITION_PROFILE_OK ||
                        !media_desired_equal(
                            &decoded, &desired_values[index]
                        ) ||
                        meiso_composition_control_session_commit_state_receive(
                            peer, result.event.route,
                            &result.event.state_offer, receive
                        ) != MEISO_COMPOSITION_CONTROL_SESSION_OK) {
                        scenario->failure_stage = 203U + (uint32_t)index;
                        scenario->failure_status = receive;
                        goto done;
                    }
                    memset(&accept, 0, sizeof(accept));
                    accept.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
                    accept.value.state_accept.route = result.event.route;
                    accept.value.state_accept.generation =
                        result.event.state_offer.generation;
                    accept.value.state_accept.owner =
                        result.event.state_offer.state.owner;
                    accept.value.state_accept.version = 1U;
                    if (meiso_composition_control_session_queue_work(
                            peer, desired_sources[index], &accept
                        ) != MEISO_COMPOSITION_CONTROL_SESSION_OK) {
                        scenario->failure_stage = 205U + (uint32_t)index;
                        goto done;
                    }
                    desired_received[index] = true;
                    matched = true;
                    break;
                }
                if (!matched) {
                    scenario->failure_stage = 207U;
                    goto done;
                }
            } else if (result.event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT) {
                bool matched = false;

                for (index = 0U; index < 2U; ++index) {
                    if (!meiso_composition_control_session_route_key(
                            peer, actual_routes[index],
                            &actual_route_keys[index]
                        ) || !route_equal(
                            result.event.route, actual_route_keys[index]
                        )) {
                        continue;
                    }
                    if (!actual_queued[index] || actual_accepted[index] ||
                        meiso_state_publisher_mark_delivery_accepted(
                            publishers[index],
                            result.event.state_accept_generation,
                            result.event.state_accept_owner,
                            result.event.state_accept_version
                        ) != MEISO_STATE_ACCEPT_CONFIRMED) {
                        scenario->failure_stage = 208U + (uint32_t)index;
                        goto done;
                    }
                    actual_accepted[index] = true;
                    matched = true;
                    break;
                }
                if (!matched) {
                    scenario->failure_stage = 210U;
                    goto done;
                }
            } else if (result.event.type !=
                       MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED) {
                scenario->failure_stage = 211U;
                scenario->failure_status = result.event.type;
                goto done;
            }
        }
        for (index = 0U; index < 2U; ++index) {
            memset(&completion, 0, sizeof(completion));
            if (meiso_composition_control_session_take_completion(
                    peer, desired_sources[index], &completion
                ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
                if (completion.kind !=
                        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT ||
                    completion.code !=
                        MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT) {
                    scenario->failure_stage = 212U + (uint32_t)index;
                    scenario->failure_status = completion.code;
                    goto done;
                }
                desired_accept_sent[index] = true;
            }
            memset(&completion, 0, sizeof(completion));
            if (meiso_composition_control_session_take_completion(
                    peer, actual_sources[index], &completion
                ) == MEISO_COMPOSITION_CONTROL_SESSION_OK) {
                if (completion.kind !=
                        MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER ||
                    completion.code !=
                        MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT) {
                    scenario->failure_stage = 214U + (uint32_t)index;
                    scenario->failure_status = completion.code;
                    goto done;
                }
                actual_offer_sent[index] = true;
            }
        }
        if (desired_accept_sent[1] && !actual_queued[1]) {
            struct pollfd item = {scenario->media_listener, POLLIN, 0};
            int poll_status;

            if (!camera_delay_started) {
                if (adapter != NULL) {
                    (void)meiso_composition_media_posix_tcp_terminate(
                        adapter,
                        MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED,
                        0
                    );
                }
                close_fd(&media_fd);
                free(adapter_arena);
                free(transport_arena);
                adapter_arena = NULL;
                transport_arena = NULL;
                adapter = NULL;
                transport = NULL;
                media_initialized = false;
                active_media_mask = 0U;
                camera_delay_started = true;
            }
            do {
                poll_status = poll(&item, (nfds_t)1U, 1);
            } while (poll_status < 0 && errno == EINTR);
            if (poll_status < 0 || (item.revents & POLLNVAL) != 0) {
                scenario->failure_stage = 216U;
                goto done;
            }
            if (poll_status > 0 &&
                (item.revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
                int stale_fd;

                do {
                    stale_fd = accept(scenario->media_listener, NULL, NULL);
                } while (stale_fd < 0 && errno == EINTR);
                if (stale_fd >= 0) {
                    ++scenario->media_accept_count;
                    ++scenario->stale_accept_count;
                    close_fd(&stale_fd);
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    scenario->failure_stage = 217U;
                    goto done;
                }
            }
            ++camera_delay_steps;
        }
        for (index = 0U; index < 2U; ++index) {
            const bool delay_complete = index == 0U ||
                camera_delay_steps >= 1500U;

            if (!desired_accept_sent[index] || actual_queued[index] ||
                !delay_complete) {
                continue;
            }
            MeisoCompositionMediaActual media;
            MeisoRuntimeActualState actual;
            MeisoRuntimeSchedulerWork work;
            MeisoStateOffer actual_offer;
            uint8_t actual_suffix[MEISO_COMPOSITION_MEDIA_ACTUAL_WIRE_SIZE];
            uint8_t actual_wire[
                MEISO_COMPOSITION_MEDIA_ACTUAL_STATE_MAX_PAYLOAD_SIZE
            ];
            MeisoByteView actual_payload;
            size_t suffix_size = 0U;

            memset(&media, 0, sizeof(media));
            media.source = media_sources[index];
            media.effective = desired_values[index];
            media.media_stream_epoch = 601U + (uint64_t)index;
            media.channel_nonce = (MeisoSessionNonce){
                701U + (uint64_t)index, 711U + (uint64_t)index
            };
            media.queue_depth = 2U;
            media.recovery_generation = 1U;
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
            actual.production_epoch = 501U + (uint64_t)index;
            if (meiso_composition_media_actual_encode(
                    &media,
                    (MeisoMutableBuffer){actual_suffix,
                                         sizeof(actual_suffix)},
                    &suffix_size
                ) != MEISO_COMPOSITION_PROFILE_OK) {
                scenario->failure_stage = 209U;
                goto done;
            }
            actual.profile = (MeisoByteView){actual_suffix, suffix_size};
            if (!meiso_runtime_actual_state_encode(
                    &actual,
                    (MeisoMutableBuffer){actual_wire, sizeof(actual_wire)},
                    &actual_payload
                ) ||
                meiso_state_publisher_publish(
                    publishers[index], edge_owner, 1U, actual_payload
                ) != MEISO_STATE_PUBLISH_NEW ||
                meiso_state_publisher_begin_delivery(
                    publishers[index],
                    (MeisoDeliveryGeneration){301U + (uint64_t)index}
                ) != MEISO_STATE_DELIVERY_STARTED ||
                !meiso_state_publisher_next_offer(
                    publishers[index], &actual_offer
                ) || !meiso_composition_control_session_route_key(
                    peer, actual_routes[index], &actual_route_keys[index]
                )) {
                scenario->failure_stage = 218U + (uint32_t)index;
                goto done;
            }
            memset(&work, 0, sizeof(work));
            work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER;
            work.value.state_offer.route = actual_route_keys[index];
            work.value.state_offer.offer = actual_offer;
            if (meiso_composition_control_session_queue_work(
                    peer, actual_sources[index], &work
                ) != MEISO_COMPOSITION_CONTROL_SESSION_OK) {
                scenario->failure_stage = 220U + (uint32_t)index;
                goto done;
            }
            actual_queued[index] = true;
        }
        if (actual_accepted[0] && actual_offer_sent[0] &&
            !scenario->render_active && !media_initialized) {
            if (!meiso_composition_control_session_snapshot(peer, &peer_snapshot) ||
                !peer_snapshot.session.session_id_known) {
                scenario->failure_stage = 222U;
                goto done;
            }
            media_fd = accept_nonblocking(scenario->media_listener);
            if (media_fd < 0 || !initialize_edge_media(
                    scenario, &catalog, peer_snapshot.session.session_id,
                    media_fd,
                    MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_RENDER_MASK,
                    &transport_arena, &transport,
                    &adapter_arena, &adapter
                )) {
                if (scenario->failure_stage == 0U) {
                    scenario->failure_stage = 223U;
                }
                goto done;
            }
            ++scenario->media_accept_count;
            active_media_mask =
                MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_RENDER_MASK;
            media_initialized = true;
        } else if (actual_accepted[0] && actual_offer_sent[0] &&
                   actual_accepted[1] && actual_offer_sent[1] &&
                   scenario->render_active && !media_initialized) {
            if (!meiso_composition_control_session_snapshot(peer, &peer_snapshot) ||
                !peer_snapshot.session.session_id_known) {
                scenario->failure_stage = 224U;
                goto done;
            }
            media_fd = accept_nonblocking(scenario->media_listener);
            if (media_fd < 0 || !initialize_edge_media(
                    scenario, &catalog, peer_snapshot.session.session_id,
                    media_fd,
                    MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_ALL_MASK,
                    &transport_arena, &transport,
                    &adapter_arena, &adapter
                )) {
                if (scenario->failure_stage == 0U) {
                    scenario->failure_stage = 225U;
                }
                goto done;
            }
            ++scenario->media_accept_count;
            active_media_mask =
                MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_ALL_MASK;
            media_initialized = true;
        }
        if (media_initialized) {
            MeisoCompositionMediaPosixTcpPrepareStatus prepare;
            MeisoCompositionMediaTransportSnapshot snapshot;

            prepare = meiso_composition_media_posix_tcp_prepare_outbound(
                adapter
            );
            if (prepare != MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_READY &&
                prepare != MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_IDLE) {
                scenario->failure_stage = 226U;
                scenario->failure_status = prepare;
                goto done;
            }
            if (meiso_composition_media_posix_tcp_wants_write(adapter)) {
                MeisoCompositionMediaPosixTcpWriteStatus write_status =
                    meiso_composition_media_posix_tcp_on_writable(adapter);
                if (write_status !=
                        MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PROGRESS &&
                    write_status !=
                        MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_PACKET_SENT &&
                    write_status !=
                        MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_WOULD_BLOCK) {
                    scenario->failure_stage = 227U;
                    scenario->failure_status = write_status;
                    goto done;
                }
            }
            if (meiso_composition_media_posix_tcp_wants_read(adapter) &&
                fd_ready_now(media_fd, POLLIN)) {
                MeisoCompositionMediaPosixTcpReadStatus read_status =
                    meiso_composition_media_posix_tcp_on_readable(adapter);
                if (read_status >
                        MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_REASSEMBLY_REPLACED &&
                    read_status !=
                        MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_WOULD_BLOCK) {
                    scenario->failure_stage = 228U;
                    scenario->failure_status = read_status;
                    goto done;
                }
            }
            if (meiso_composition_media_transport_snapshot(
                    transport, &snapshot
                ) != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
                scenario->failure_stage = 229U;
                goto done;
            }
            if (snapshot.phase == MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE) {
                if (active_media_mask ==
                    MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_RENDER_MASK) {
                    scenario_signal(scenario, true, false, false);
                } else if (active_media_mask ==
                           MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_ALL_MASK) {
                    scenario_signal(scenario, true, true, false);
                    ok = true;
                    break;
                }
            }
        }
        (void)sched_yield();
        ++pump_steps;
    }
    if (!ok && scenario->failure_stage == 0U) {
        scenario->failure_stage = pump_steps ==
            MEDIA_SCENARIO_MAX_PUMP_STEPS ? 106U : 107U;
        scenario->failure_status = (uint32_t)pump_steps;
    }

done:
    if (!ok) {
        scenario_signal(scenario, false, false, true);
    } else {
        struct timespec deadline;
        if (make_deadline(&deadline, MEDIA_SCENARIO_TIMEOUT_SECONDS)) {
            (void)pthread_mutex_lock(&scenario->mutex);
            while (!scenario->service_done) {
                if (pthread_cond_timedwait(
                        &scenario->condition, &scenario->mutex, &deadline
                    ) == ETIMEDOUT) {
                    break;
                }
            }
            (void)pthread_mutex_unlock(&scenario->mutex);
        }
    }
    close_fd(&media_fd);
    close_fd(&control_fd);
    free(adapter_arena);
    free(transport_arena);
    free(publisher_arenas[1]);
    free(publisher_arenas[0]);
    free(replica_arenas[1]);
    free(replica_arenas[0]);
    free(peer_arena);
    return NULL;
}

static bool run_service_mode(
    const char *config_contents,
    bool simulator,
    const char *input,
    size_t input_size,
    uint32_t input_hold_ms,
    RunResult *out_result
)
{
    char config_path[sizeof("/tmp/meiso-composition-host-bridge-XXXXXX")] =
        {0};
    char *remote_arguments[] = {
        (char *)"composition_host_bridge",
        (char *)"--config",
        config_path,
        (char *)"--interactive",
        NULL
    };
    char *simulator_arguments[] = {
        (char *)"composition_host_bridge",
        (char *)"--simulator",
        (char *)"--interactive",
        NULL
    };
    int input_pipe[2] = {-1, -1};
    int output_pipe[2] = {-1, -1};
    int error_pipe[2] = {-1, -1};
    int saved_input = -1;
    int saved_output = -1;
    int saved_error = -1;
    pthread_t writer_thread;
    bool writer_started = false;
    Writer writer;
    bool ok = false;

    if (input == NULL || out_result == NULL ||
        (!simulator &&
         (config_contents == NULL ||
          !write_config_file(config_contents, config_path,
                             sizeof(config_path))))) {
        return false;
    }
    memset(out_result, 0, sizeof(*out_result));
    memset(&writer, 0, sizeof(writer));
    writer.bytes = input;
    writer.size = input_size;
    writer.fd = -1;
    writer.hold_after_write_ms = input_hold_ms;
    if (pipe(input_pipe) != 0 || pipe(output_pipe) != 0 ||
        pipe(error_pipe) != 0) {
        goto done;
    }
    if (fflush(NULL) != 0) {
        goto done;
    }
    saved_input = dup(STDIN_FILENO);
    saved_output = dup(STDOUT_FILENO);
    saved_error = dup(STDERR_FILENO);
    if (saved_input < 0 || saved_output < 0 || saved_error < 0 ||
        dup2(input_pipe[0], STDIN_FILENO) < 0 ||
        dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
        dup2(error_pipe[1], STDERR_FILENO) < 0) {
        goto done;
    }
    close_fd(&input_pipe[0]);
    close_fd(&output_pipe[1]);
    close_fd(&error_pipe[1]);
    writer.fd = input_pipe[1];
    input_pipe[1] = -1;
    if (pthread_create(&writer_thread, NULL, writer_main, &writer) != 0) {
        goto done;
    }
    writer_started = true;
    out_result->exit_code = meiso_composition_host_bridge_run(
        simulator ? 3 : 4,
        simulator ? simulator_arguments : remote_arguments
    );
    if (fflush(NULL) != 0 || dup2(saved_input, STDIN_FILENO) < 0 ||
        dup2(saved_output, STDOUT_FILENO) < 0 ||
        dup2(saved_error, STDERR_FILENO) < 0) {
        goto done;
    }
    close_fd(&saved_input);
    close_fd(&saved_output);
    close_fd(&saved_error);
    if (pthread_join(writer_thread, NULL) != 0) {
        writer_started = false;
        goto done;
    }
    writer_started = false;
    out_result->writer_error = writer.error_number;
    if (!read_all(output_pipe[0], out_result->output,
                  sizeof(out_result->output), &out_result->output_size) ||
        !read_all(error_pipe[0], out_result->error,
                  sizeof(out_result->error), &out_result->error_size)) {
        goto done;
    }
    ok = true;

done:
    if (saved_input >= 0) {
        (void)dup2(saved_input, STDIN_FILENO);
    }
    if (saved_output >= 0) {
        (void)dup2(saved_output, STDOUT_FILENO);
    }
    if (saved_error >= 0) {
        (void)dup2(saved_error, STDERR_FILENO);
    }
    close_fd(&saved_input);
    close_fd(&saved_output);
    close_fd(&saved_error);
    if (writer_started) {
        close_fd(&writer.fd);
        (void)pthread_join(writer_thread, NULL);
    }
    close_fd(&writer.fd);
    close_fd(&input_pipe[0]);
    close_fd(&input_pipe[1]);
    close_fd(&output_pipe[0]);
    close_fd(&output_pipe[1]);
    close_fd(&error_pipe[0]);
    close_fd(&error_pipe[1]);
    if (!simulator) {
        (void)unlink(config_path);
    }
    return ok;
}

static bool run_service(
    const char *config_contents,
    const char *input,
    size_t input_size,
    RunResult *out_result
)
{
    return run_service_mode(
        config_contents, false, input, input_size, 0U, out_result
    );
}

static bool run_service_with_input_hold(
    const char *config_contents,
    const char *input,
    size_t input_size,
    uint32_t input_hold_ms,
    RunResult *out_result
)
{
    return run_service_mode(
        config_contents, false, input, input_size, input_hold_ms, out_result
    );
}

static bool run_simulator_service(
    const char *input,
    size_t input_size,
    RunResult *out_result
)
{
    return run_service_mode(NULL, true, input, input_size, 0U, out_result);
}

static bool run_media_service(
    const char *config_contents,
    MediaScenario *scenario,
    RunResult *out_result
)
{
    char config_path[sizeof("/tmp/meiso-composition-host-bridge-XXXXXX")];
    char *arguments[] = {
        (char *)"composition_host_bridge",
        (char *)"--config",
        config_path,
        (char *)"--interactive",
        NULL
    };
    int input_pipe[2] = {-1, -1};
    FILE *output_file = NULL;
    FILE *error_file = NULL;
    int saved_input = -1;
    int saved_output = -1;
    int saved_error = -1;
    pthread_t writer_thread;
    pthread_t edge_thread;
    bool writer_started = false;
    bool edge_started = false;
    MediaWriter writer;
    bool ok = false;

    if (config_contents == NULL || scenario == NULL || out_result == NULL ||
        !write_config_file(config_contents, config_path, sizeof(config_path))) {
        return false;
    }
    memset(out_result, 0, sizeof(*out_result));
    memset(&writer, 0, sizeof(writer));
    writer.scenario = scenario;
    writer.fd = -1;
    output_file = tmpfile();
    error_file = tmpfile();
    if (pipe(input_pipe) != 0 || output_file == NULL ||
        error_file == NULL || fflush(NULL) != 0) {
        goto done;
    }
    saved_input = dup(STDIN_FILENO);
    saved_output = dup(STDOUT_FILENO);
    saved_error = dup(STDERR_FILENO);
    if (saved_input < 0 || saved_output < 0 || saved_error < 0 ||
        dup2(input_pipe[0], STDIN_FILENO) < 0 ||
        dup2(fileno(output_file), STDOUT_FILENO) < 0 ||
        dup2(fileno(error_file), STDERR_FILENO) < 0) {
        goto done;
    }
    close_fd(&input_pipe[0]);
    writer.fd = input_pipe[1];
    input_pipe[1] = -1;
    if (pthread_create(&edge_thread, NULL, edge_media_main, scenario) != 0) {
        goto done;
    }
    edge_started = true;
    if (pthread_create(&writer_thread, NULL, media_writer_main, &writer) != 0) {
        goto done;
    }
    writer_started = true;
    out_result->exit_code = meiso_composition_host_bridge_run(4, arguments);
    if (fflush(NULL) != 0 || dup2(saved_input, STDIN_FILENO) < 0 ||
        dup2(saved_output, STDOUT_FILENO) < 0 ||
        dup2(saved_error, STDERR_FILENO) < 0) {
        goto done;
    }
    close_fd(&saved_input);
    close_fd(&saved_output);
    close_fd(&saved_error);
    (void)pthread_mutex_lock(&scenario->mutex);
    scenario->service_done = true;
    if (!scenario->media_active) {
        scenario->failed = true;
    }
    (void)pthread_cond_broadcast(&scenario->condition);
    (void)pthread_mutex_unlock(&scenario->mutex);
    if (pthread_join(writer_thread, NULL) != 0) {
        writer_started = false;
        goto done;
    }
    writer_started = false;
    if (pthread_join(edge_thread, NULL) != 0) {
        edge_started = false;
        goto done;
    }
    edge_started = false;
    out_result->writer_error = writer.error_number;
    if (fflush(output_file) != 0 || fflush(error_file) != 0 ||
        fseek(output_file, 0L, SEEK_SET) != 0 ||
        fseek(error_file, 0L, SEEK_SET) != 0) {
        goto done;
    }
    out_result->output_size = fread(
        out_result->output, 1U, sizeof(out_result->output) - 1U, output_file
    );
    out_result->error_size = fread(
        out_result->error, 1U, sizeof(out_result->error) - 1U, error_file
    );
    out_result->output[out_result->output_size] = '\0';
    out_result->error[out_result->error_size] = '\0';
    if (ferror(output_file) || ferror(error_file) ||
        !feof(output_file) || !feof(error_file)) {
        goto done;
    }
    ok = true;

done:
    if (saved_input >= 0) {
        (void)dup2(saved_input, STDIN_FILENO);
    }
    if (saved_output >= 0) {
        (void)dup2(saved_output, STDOUT_FILENO);
    }
    if (saved_error >= 0) {
        (void)dup2(saved_error, STDERR_FILENO);
    }
    close_fd(&saved_input);
    close_fd(&saved_output);
    close_fd(&saved_error);
    (void)pthread_mutex_lock(&scenario->mutex);
    scenario->service_done = true;
    scenario->failed = scenario->failed || !scenario->media_active;
    (void)pthread_cond_broadcast(&scenario->condition);
    (void)pthread_mutex_unlock(&scenario->mutex);
    if (writer_started) {
        close_fd(&writer.fd);
        (void)pthread_join(writer_thread, NULL);
    }
    if (edge_started) {
        (void)pthread_join(edge_thread, NULL);
    }
    close_fd(&writer.fd);
    close_fd(&input_pipe[0]);
    close_fd(&input_pipe[1]);
    if (output_file != NULL) {
        (void)fclose(output_file);
    }
    if (error_file != NULL) {
        (void)fclose(error_file);
    }
    (void)unlink(config_path);
    return ok;
}

static size_t occurrence_count(const char *text, const char *needle)
{
    size_t count = 0U;
    size_t needle_size = strlen(needle);

    while ((text = strstr(text, needle)) != NULL) {
        ++count;
        text += needle_size;
    }
    return count;
}

static bool output_is_closed_ndjson(const char *output)
{
    const char *line = output;

    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        size_t size;
        if (end == NULL) {
            return false;
        }
        size = (size_t)(end - line);
        if (size < 2U || line[0] != '{' || line[size - 1U] != '}' ||
            strstr(line, "\"unknown\"") != NULL) {
            return false;
        }
        line = end + 1;
    }
    return true;
}

static bool test_config_rejections(void)
{
    char config[CONFIG_BYTES];
    RunResult result;

    CHECK(build_config(config, sizeof(config), 9U, 9U,
                       "control_port=10\n", NULL));
    CHECK(run_service(config, "", 0U, &result));
    CHECK(result.exit_code == 2);
    CHECK(result.output_size == 0U);
    CHECK(strcmp(result.error, "E_CONFIG\n") == 0);

    CHECK(build_config(config, sizeof(config), 9U, 9U,
                       "unexpected_key=1\n", NULL));
    CHECK(run_service(config, "", 0U, &result));
    CHECK(result.exit_code == 2);
    CHECK(result.output_size == 0U);
    CHECK(strcmp(result.error, "E_CONFIG\n") == 0);

    CHECK(build_config(config, sizeof(config), 9U, 9U, NULL,
                       "camera_incarnation"));
    CHECK(run_service(config, "", 0U, &result));
    CHECK(result.exit_code == 2);
    CHECK(result.output_size == 0U);
    CHECK(strcmp(result.error, "E_CONFIG\n") == 0);
    return true;
}

static bool test_numeric_connect_failure(void)
{
    char config[CONFIG_BYTES];
    RunResult result;
    int bound_socket = -1;
    uint16_t port;

    CHECK(make_control_socket(false, &bound_socket, &port));
    CHECK(build_config(config, sizeof(config), port, 9U, NULL, NULL));
    CHECK(run_service(config, "", 0U, &result));
    CHECK(result.exit_code == 1);
    CHECK(result.output_size == 0U);
    CHECK(strcmp(result.error, "E_INIT\n") == 0);
    close_fd(&bound_socket);
    return true;
}

static bool test_control_transport_terminal_code(void)
{
    char config[CONFIG_BYTES];
    RunResult result;
    TransportCloseServer server;
    pthread_t server_thread;
    int listener = -1;
    uint16_t port;
    bool server_started = false;
    bool ok = false;

    memset(&server, 0, sizeof(server));
    server.listener = -1;
    CHECK_GOTO(make_control_socket(true, &listener, &port) &&
               set_nonblocking(listener) &&
               build_config(config, sizeof(config), port, 9U, NULL, NULL),
               cleanup);
    server.listener = listener;
    CHECK_GOTO(pthread_create(
                   &server_thread, NULL, transport_close_server_main, &server
               ) == 0, cleanup);
    server_started = true;
    CHECK_GOTO(run_service_with_input_hold(
                   config, "", 0U, 2000U, &result
               ), cleanup);
    CHECK_GOTO(pthread_join(server_thread, NULL) == 0, cleanup);
    server_started = false;
    CHECK_GOTO(server.accepted && server.saw_client_byte &&
               server.error_number == 0 && result.exit_code == 0 &&
               result.writer_error == 0 && result.error_size == 0U,
               cleanup);
    CHECK_GOTO(strstr(result.output, "\"type\":\"service.ready\"") !=
                   NULL &&
               occurrence_count(
                   result.output,
                   "{\"type\":\"terminal\","
                   "\"code\":\"HOST_RUNTIME_TRANSPORT_TERMINAL\"}\n"
               ) == 1U &&
               strstr(result.output, "HOST_RUNTIME_INTERNAL_TERMINAL") == NULL &&
               output_is_closed_ndjson(result.output), cleanup);
    ok = true;

cleanup:
    close_fd(&listener);
    if (server_started) {
        (void)pthread_join(server_thread, NULL);
    }
    return ok;
}

static bool append_scene(
    char *input,
    size_t capacity,
    size_t *size,
    uint64_t request_id,
    size_t object_count,
    bool long_numbers
)
{
    const char *zero = long_numbers ? "0.00000000000000000000" : "0";
    const char *one = long_numbers ? "1.00000000000000000000" : "1";
    size_t index;

    if (!append_text(
            input, capacity, size,
            "{\"command\":\"set_scene\",\"request_id\":%llu,"
            "\"enabled\":true,\"camera\":{\"position\":[%s,%s,%s],"
            "\"orientation_xyzw\":[%s,%s,%s,%s],"
            "\"vertical_fov_degrees\":60,\"near_z\":0.1,"
            "\"far_z\":100},\"objects\":[",
            (unsigned long long)request_id, zero, zero, one, zero, zero,
            zero, one
        )) {
        return false;
    }
    for (index = 0U; index < object_count; ++index) {
        if (!append_text(
                input, capacity, size,
                "%s{\"editor_id\":%llu,\"asset_handle\":\"cube\","
                "\"position\":[%s,%s,%s],\"orientation_xyzw\":[%s,%s,%s,%s],"
                "\"scale\":[%s,%s,%s],\"visible\":true}",
                index == 0U ? "" : ",", (unsigned long long)(index + 1U),
                zero, zero, zero, zero, zero, zero, one, one, one, one
            )) {
            return false;
        }
    }
    return append_text(input, capacity, size, "]}\n");
}

static bool run_valid_session(
    const char *input,
    size_t input_size,
    RunResult *out_result
)
{
    char config[CONFIG_BYTES];
    int listener = -1;
    uint16_t port;
    bool ok;

    if (!make_control_socket(true, &listener, &port) ||
        !build_config(config, sizeof(config), port, 9U, NULL, NULL)) {
        close_fd(&listener);
        return false;
    }
    ok = run_service(config, input, input_size, out_result);
    close_fd(&listener);
    return ok;
}

static bool test_pre_ready_commands_and_redaction(void)
{
    static const char invalid_commands[] =
        "{\"command\":\"set_scene\",\"request_id\":3,\"enabled\":true,"
        "\"space_epoch\":9,\"camera\":{\"position\":[0,0,1],"
        "\"orientation_xyzw\":[0,0,0,1],\"vertical_fov_degrees\":60,"
        "\"near_z\":0.1,\"far_z\":100},\"objects\":[]}\n"
        "{\"command\":\"set_scene\",\"request_id\":4,\"enabled\":true,"
        "\"camera\":{\"position\":[0,0,1],\"orientation_xyzw\":[0,0,0,1],"
        "\"vertical_fov_degrees\":60,\"near_z\":0.1,\"far_z\":100,"
        "\"source_clock_epoch\":9},\"objects\":[]}\n"
        "{\"command\":\"set_scene\",\"request_id\":5,\"enabled\":true,"
        "\"camera\":{\"position\":[0,0,1],\"orientation_xyzw\":[0,0,0,1],"
        "\"vertical_fov_degrees\":60,\"near_z\":0.1,\"far_z\":100,"
        "\"source_monotonic_ns\":9},\"objects\":[]}\n"
        "{\"command\":\"set_scene\",\"request_id\":6,\"enabled\":true,"
        "\"camera\":{\"position\":[0,0,1],\"orientation_xyzw\":[0,0,0,1],"
        "\"vertical_fov_degrees\":60,\"near_z\":0.1,\"far_z\":100,"
        "\"viewport_width\":320},\"objects\":[]}\n"
        "{\"command\":\"set_scene\",\"request_id\":7,\"enabled\":true,"
        "\"camera\":{\"position\":[0,0,1],\"orientation_xyzw\":[0,0,0,1],"
        "\"vertical_fov_degrees\":60,\"near_z\":0.1,\"far_z\":100},"
        "\"objects\":[{\"editor_id\":1,\"asset_handle\":\"cube\","
        "\"position\":[0,0,0],\"orientation_xyzw\":[0,0,0,1],"
        "\"scale\":[1,1,1],\"visible\":true,\"space_epoch\":9}]}\n";
    char input[INPUT_BYTES];
    size_t size = 0U;
    RunResult result;
    const char *forbidden[] = {
        "peer_id", "session_id", "nonce", "binding", "content_id",
        "object_id", "space_epoch", "source_clock_epoch",
        "source_monotonic_ns", "viewport_width"
    };
    size_t index;

    CHECK(append_text(
        input, sizeof(input), &size,
        "{\"command\":\"asset.load_builtin\",\"request_id\":1,"
        "\"asset_handle\":\"cube\"}\n"
    ));
    CHECK(append_scene(input, sizeof(input), &size, 2U, 1U, false));
    CHECK(append_text(input, sizeof(input), &size, "%s", invalid_commands));
    CHECK(append_text(
        input, sizeof(input), &size,
        "{\"command\":\"snapshot\",\"request_id\":8}\n"
    ));
    CHECK(run_valid_session(input, size, &result));
    CHECK(result.exit_code == 0);
    CHECK(result.writer_error == 0);
    CHECK(result.error_size == 0U);
    CHECK(strstr(
        result.output,
        "{\"type\":\"service.ready\",\"profile\":\"composition\","
        "\"wire_owner\":\"native_service\","
        "\"asset_import\":\"builtin_cube_only\","
        "\"frame_projection\":\"CMFB/v1\"}\n"
    ) == result.output);
    CHECK(strstr(result.output, "\"kind\":\"put_mesh\"") != NULL);
    CHECK(strstr(result.output, "\"kind\":\"set_scene\"") != NULL);
    CHECK(strstr(result.output, "\"type\":\"snapshot\",\"request_id\":8") !=
          NULL);
    CHECK(occurrence_count(
        result.output,
        "{\"type\":\"error\",\"request_id\":0,"
        "\"code\":\"INVALID_COMMAND\"}"
    ) == 5U);
    CHECK(output_is_closed_ndjson(result.output));
    for (index = 0U; index < sizeof(forbidden) / sizeof(forbidden[0]);
         ++index) {
        CHECK(strstr(result.output, forbidden[index]) == NULL);
    }
    return true;
}

static bool test_large_hup_drain(void)
{
    char input[INPUT_BYTES];
    size_t size = 0U;
    size_t first_line_end;
    RunResult result;

    CHECK(append_text(
        input, sizeof(input), &size,
        "{\"command\":\"asset.load_builtin\",\"request_id\":1,"
        "\"asset_handle\":\"cube\"}\n"
    ));
    CHECK(append_scene(input, sizeof(input), &size, 2U, 32U, true));
    first_line_end = size;
    CHECK(append_scene(input, sizeof(input), &size, 3U, 32U, true));
    CHECK(first_line_end < 16384U);
    CHECK(size - first_line_end < 16384U);
    CHECK(size > 16384U);
    CHECK(run_valid_session(input, size, &result));
    CHECK(result.exit_code == 0);
    CHECK(result.writer_error == 0);
    CHECK(result.error_size == 0U);
    CHECK(occurrence_count(result.output, "\"type\":\"command.receipt\"") ==
          3U);
    CHECK(strstr(result.output, "\"request_id\":2") != NULL);
    CHECK(strstr(result.output, "\"request_id\":3") != NULL);
    CHECK(output_is_closed_ndjson(result.output));
    return true;
}

static bool test_partial_line_eof(void)
{
    static const char partial[] =
        "{\"command\":\"asset.load_builtin\",\"request_id\":1";
    RunResult result;

    CHECK(run_valid_session(partial, sizeof(partial) - 1U, &result));
    CHECK(result.exit_code == 1);
    CHECK(result.writer_error == 0);
    CHECK(strcmp(result.error, "E_RUNTIME\n") == 0);
    CHECK(strstr(result.output, "\"type\":\"service.ready\"") != NULL);
    CHECK(strstr(
        result.output,
        "{\"type\":\"error\",\"request_id\":0,"
        "\"code\":\"INVALID_COMMAND\"}\n"
    ) != NULL);
    CHECK(output_is_closed_ndjson(result.output));
    return true;
}

static volatile sig_atomic_t signal_count;

static void test_signal_handler(int signal_number)
{
    if (signal_number == SIGPIPE) {
        ++signal_count;
    }
}

static bool test_sigpipe_restore(void)
{
    struct sigaction before;
    struct sigaction after;
    struct sigaction original;
    RunResult result;

    memset(&before, 0, sizeof(before));
    before.sa_handler = test_signal_handler;
    CHECK(sigemptyset(&before.sa_mask) == 0);
    CHECK(sigaddset(&before.sa_mask, SIGUSR1) == 0);
    before.sa_flags = SA_RESTART;
    signal_count = 0;
    CHECK(sigaction(SIGPIPE, &before, &original) == 0);
    CHECK(run_valid_session("", 0U, &result));
    CHECK(result.exit_code == 0);
    CHECK(result.error_size == 0U);
    CHECK(sigaction(SIGPIPE, NULL, &after) == 0);
    CHECK(after.sa_handler == test_signal_handler);
    CHECK((after.sa_flags & SA_RESTART) != 0);
    CHECK(sigismember(&after.sa_mask, SIGUSR1) == 1);
    CHECK(signal_count == 0);
    CHECK(sigaction(SIGPIPE, &original, NULL) == 0);
    return true;
}

static bool test_offline_simulator_editor_flow(void)
{
    char input[INPUT_BYTES];
    size_t size = 0U;
    uint64_t request_id;
    RunResult result;

    CHECK(append_text(
        input, sizeof(input), &size,
        "{\"command\":\"asset.load_builtin\",\"request_id\":1,"
        "\"asset_handle\":\"cube\"}\n"
    ));
    CHECK(append_scene(input, sizeof(input), &size, 2U, 1U, false));
    CHECK(append_text(
        input, sizeof(input), &size,
        "{\"command\":\"set_presentation\",\"request_id\":3,"
        "\"enabled\":true,\"use_safety_frame\":false,"
        "\"target_width\":320,\"target_height\":240,"
        "\"color_format\":\"rgba8888\","
        "\"depth_format\":\"implementation32\","
        "\"max_result_hz\":1,\"offline_lease_ms\":1000}\n"
        "{\"command\":\"set_render_media\",\"request_id\":4,"
        "\"enabled\":true,\"pixel_format\":\"rgba8888\","
        "\"codec\":\"raw\",\"width\":320,\"height\":240,"
        "\"fps_millihz\":1000,\"bitrate_bps\":0,"
        "\"gop_frames\":1,\"request_key\":true}\n"
        "{\"command\":\"set_camera_media\",\"request_id\":5,"
        "\"enabled\":true,\"pixel_format\":\"nv12\","
        "\"codec\":\"raw\",\"width\":320,\"height\":240,"
        "\"fps_millihz\":1000,\"bitrate_bps\":0,"
        "\"gop_frames\":1,\"request_key\":true}\n"
        "{\"command\":\"set_wifi\",\"request_id\":6,"
        "\"enabled\":true,\"selected_fields\":3,"
        "\"sample_period_ms\":1000}\n"
        "{\"command\":\"set_device\",\"request_id\":7,"
        "\"enabled\":true,\"selected_fields\":14,"
        "\"sample_period_ms\":1000}\n"
    ));
    for (request_id = 8U; request_id <= 16U; ++request_id) {
        CHECK(append_text(
            input, sizeof(input), &size,
            "{\"command\":\"snapshot\",\"request_id\":%llu}\n",
            (unsigned long long)request_id
        ));
    }
    CHECK(append_text(
        input, sizeof(input), &size,
        "{\"command\":\"close\",\"request_id\":17}\n"
    ));
    CHECK(run_simulator_service(input, size, &result));
    CHECK(result.exit_code == 0);
    CHECK(result.writer_error == 0);
    CHECK(result.error_size == 0U);
    CHECK(strstr(
        result.output,
        "{\"type\":\"service.ready\",\"profile\":\"composition\","
        "\"wire_owner\":\"simulator_service\""
    ) == result.output);
    CHECK(occurrence_count(result.output, "\"type\":\"command.receipt\"") ==
          7U);
    CHECK(strstr(result.output, "\"channel\":\"presentation\"") != NULL);
    CHECK(strstr(result.output, "\"channel\":\"scene\"") != NULL);
    CHECK(strstr(result.output, "\"channel\":\"wifi\"") != NULL);
    CHECK(strstr(result.output, "\"channel\":\"device\"") != NULL);
    CHECK(strstr(result.output, "\"channel\":\"render_media\"") != NULL);
    CHECK(strstr(result.output, "\"channel\":\"camera_media\"") != NULL);
    CHECK(strstr(result.output, "\"type\":\"render.result\"") != NULL);
    CHECK(strstr(result.output, "\"reason\":\"ready\"") != NULL);
    CHECK(strstr(result.output, "\"control_ready\":false") != NULL);
    CHECK(strstr(result.output, "\"media_bound\":false") != NULL);
    CHECK(strstr(result.output, "\"media_mask\":\"none\"") != NULL);
    CHECK(strstr(result.output, "\"render_frame\":false") != NULL);
    CHECK(strstr(result.output, "\"camera_frame\":false") != NULL);
    CHECK(strstr(result.output, "\"type\":\"media.connected\"") == NULL);
    CHECK(strstr(result.output, "\"type\":\"closing\",\"request_id\":17") !=
          NULL);
    CHECK(output_is_closed_ndjson(result.output));
    return true;
}

static bool test_presentation_rate_bounds(void)
{
    static const char input[] =
        "{\"command\":\"set_presentation\",\"request_id\":1,"
        "\"enabled\":true,\"use_safety_frame\":false,"
        "\"target_width\":320,\"target_height\":240,"
        "\"color_format\":\"rgba8888\","
        "\"depth_format\":\"implementation32\","
        "\"max_result_hz\":0,\"offline_lease_ms\":1000}\n"
        "{\"command\":\"set_presentation\",\"request_id\":2,"
        "\"enabled\":true,\"use_safety_frame\":false,"
        "\"target_width\":320,\"target_height\":240,"
        "\"color_format\":\"rgba8888\","
        "\"depth_format\":\"implementation32\","
        "\"max_result_hz\":11,\"offline_lease_ms\":1000}\n"
        "{\"command\":\"set_presentation\",\"request_id\":3,"
        "\"enabled\":true,\"use_safety_frame\":false,"
        "\"target_width\":320,\"target_height\":240,"
        "\"color_format\":\"rgba8888\","
        "\"depth_format\":\"implementation32\","
        "\"max_result_hz\":1,\"offline_lease_ms\":1000}\n"
        "{\"command\":\"snapshot\",\"request_id\":4}\n"
        "{\"command\":\"close\",\"request_id\":5}\n";
    RunResult result;

    CHECK(run_simulator_service(input, sizeof(input) - 1U, &result));
    CHECK(result.exit_code == 0 && result.writer_error == 0 &&
          result.error_size == 0U);
    CHECK(strstr(
        result.output,
        "{\"type\":\"error\",\"request_id\":1,"
        "\"code\":\"CONFLICT\"}\n"
    ) != NULL);
    CHECK(strstr(
        result.output,
        "{\"type\":\"error\",\"request_id\":2,"
        "\"code\":\"CONFLICT\"}\n"
    ) != NULL);
    CHECK(strstr(result.output, "\"request_id\":3,\"command_id\":") !=
          NULL);
    CHECK(strstr(result.output, "\"kind\":\"set_presentation\"") != NULL);
    CHECK(strstr(result.output, "\"type\":\"snapshot\",\"request_id\":4") !=
          NULL);
    CHECK(strstr(result.output, "\"type\":\"closing\",\"request_id\":5") !=
          NULL);
    CHECK(occurrence_count(result.output, "\"code\":\"CONFLICT\"") == 2U);
    CHECK(output_is_closed_ndjson(result.output));
    return true;
}

static bool test_media_reconnect_barrier_no_frame(void)
{
    char config[CONFIG_BYTES];
    RunResult result;
    MediaScenario scenario;
    uint16_t control_port;
    uint16_t media_port;
    bool mutex_initialized = false;
    bool condition_initialized = false;
    bool ok = false;

    memset(&scenario, 0, sizeof(scenario));
    scenario.control_listener = -1;
    scenario.media_listener = -1;
    CHECK_GOTO(make_control_socket(
                   true, &scenario.control_listener, &control_port
               ) && make_control_socket(
                   true, &scenario.media_listener, &media_port
               ) && set_nonblocking(scenario.control_listener) &&
               set_nonblocking(scenario.media_listener) &&
               build_config(
                   config, sizeof(config), control_port, media_port,
                   NULL, NULL
               ), cleanup);
    CHECK_GOTO(pthread_mutex_init(&scenario.mutex, NULL) == 0, cleanup);
    mutex_initialized = true;
    CHECK_GOTO(pthread_cond_init(&scenario.condition, NULL) == 0, cleanup);
    condition_initialized = true;
    CHECK_GOTO(run_media_service(config, &scenario, &result), cleanup);
    if (result.exit_code != 0 || result.writer_error != 0 ||
        result.error_size != 0U || !scenario.render_active ||
        !scenario.media_active || scenario.media_accept_count != 2U ||
        scenario.stale_accept_count != 0U || scenario.failed) {
        (void)fprintf(
            stderr,
            "media service result: exit=%d writer=%d active=%u failed=%u "
            "accepts=%u stale=%u stage=%u status=%u "
            "stderr=%s stdout=%s\n",
            result.exit_code, result.writer_error,
            scenario.media_active ? 1U : 0U, scenario.failed ? 1U : 0U,
            scenario.media_accept_count, scenario.stale_accept_count,
            scenario.failure_stage, scenario.failure_status,
            result.error, result.output
        );
    }
    CHECK_GOTO(result.exit_code == 0 && result.writer_error == 0 &&
               result.error_size == 0U && scenario.render_active &&
               scenario.media_active && scenario.media_accept_count == 2U &&
               scenario.stale_accept_count == 0U && !scenario.failed,
               cleanup);
    CHECK_GOTO(occurrence_count(
                   result.output,
                   "{\"type\":\"media.connected\","
                   "\"transport\":\"tcp\"}\n"
               ) == 2U &&
               strstr(
                   result.output,
                   "\"event\":\"media_progress\""
               ) != NULL &&
               strstr(
                   result.output,
                   "{\"type\":\"snapshot\",\"request_id\":3,"
               ) != NULL &&
               strstr(result.output, "\"media_bound\":true,") != NULL &&
               strstr(result.output, "\"media_mask\":\"both\"") != NULL &&
               strstr(result.output, "\"render_frame\":false") != NULL &&
               strstr(result.output, "\"camera_frame\":false") != NULL &&
               strstr(
                   result.output,
                   "{\"type\":\"closing\",\"request_id\":4}\n"
               ) != NULL && output_is_closed_ndjson(result.output), cleanup);
    ok = true;

cleanup:
    close_fd(&scenario.control_listener);
    close_fd(&scenario.media_listener);
    if (condition_initialized) {
        (void)pthread_cond_destroy(&scenario.condition);
    }
    if (mutex_initialized) {
        (void)pthread_mutex_destroy(&scenario.mutex);
    }
    return ok;
}

int main(void)
{
    if (!test_config_rejections() || !test_numeric_connect_failure() ||
        !test_control_transport_terminal_code() ||
        !test_pre_ready_commands_and_redaction() ||
        !test_large_hup_drain() || !test_partial_line_eof() ||
        !test_sigpipe_restore() || !test_offline_simulator_editor_flow() ||
        !test_presentation_rate_bounds() ||
        !test_media_reconnect_barrier_no_frame()) {
        return 1;
    }
    (void)puts("composition host bridge tests passed");
    return 0;
}
