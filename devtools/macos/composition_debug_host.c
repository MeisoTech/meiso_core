#define _POSIX_C_SOURCE 200809L

#include "composition_host_app.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
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
 * Native Composition debug service. This is an application adapter, not a
 * new protocol surface: only CompositionHostApp owns Core/Session/Runtime
 * truth. stdin/stdout NDJSON and CMFB/v1 are bounded machine-local bridges.
 */

enum {
    HOST_CONFIG_LINE_BYTES = 512,
    HOST_CONTROL_LINE_BYTES = 16384,
    HOST_CONNECT_TIMEOUT_MS = 10000,
    HOST_IO_STEP_BYTES = 4096,
    HOST_CONTROL_ADAPTER_BYTES = 65536,
    HOST_CONTROL_RUNTIME_BYTES = 400000,
    HOST_CONTROL_SCHEDULER_BYTES = 65536,
    HOST_MEDIA_CONNECTION_SLOTS = 4,
    HOST_EDITOR_OBJECTS = MEISO_COMPOSITION_SCENE_MAX_OBJECTS,
    HOST_CUBE_BYTES = 512,
    HOST_CMFB_HEADER_BYTES = 80,
    HOST_FRAME_BYTES =
        MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES >
                MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES ?
            MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES :
            MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES,
    HOST_FRAME_RECORD_BYTES = HOST_CMFB_HEADER_BYTES + HOST_FRAME_BYTES,
    HOST_POLL_TIMEOUT_MS = 1000
};

enum {
    CONFIG_CONTROL_ADDRESS = UINT64_C(1) << 0U,
    CONFIG_CONTROL_PORT = UINT64_C(1) << 1U,
    CONFIG_MEDIA_ADDRESS = UINT64_C(1) << 2U,
    CONFIG_MEDIA_PORT = UINT64_C(1) << 3U,
    CONFIG_LOCAL_PEER_HIGH = UINT64_C(1) << 4U,
    CONFIG_LOCAL_PEER_LOW = UINT64_C(1) << 5U,
    CONFIG_EDGE_PEER_HIGH = UINT64_C(1) << 6U,
    CONFIG_EDGE_PEER_LOW = UINT64_C(1) << 7U,
    CONFIG_LOCAL_INCARNATION = UINT64_C(1) << 8U,
    CONFIG_NONCE_HIGH = UINT64_C(1) << 9U,
    CONFIG_NONCE_LOW = UINT64_C(1) << 10U,
    CONFIG_TARGET_MANIFEST = UINT64_C(1) << 11U,
    CONFIG_WIFI_FIELDS = UINT64_C(1) << 12U,
    CONFIG_DEVICE_FIELDS = UINT64_C(1) << 13U,
    CONFIG_CATALOG_GENERATION = UINT64_C(1) << 14U,
    CONFIG_WIFI_INCARNATION = UINT64_C(1) << 15U,
    CONFIG_PRESENTATION_INCARNATION = UINT64_C(1) << 16U,
    CONFIG_SCENE_INCARNATION = UINT64_C(1) << 17U,
    CONFIG_DEVICE_INCARNATION = UINT64_C(1) << 18U,
    CONFIG_RENDER_INCARNATION = UINT64_C(1) << 19U,
    CONFIG_CAMERA_INCARNATION = UINT64_C(1) << 20U,
    CONFIG_REQUIRED = (UINT64_C(1) << 21U) - 1U
};

typedef struct {
    struct sockaddr_storage control_address;
    socklen_t control_address_size;
    uint16_t control_port;
    struct sockaddr_storage media_address;
    socklen_t media_address_size;
    uint16_t media_port;
    MeisoPeerId local_peer_id;
    MeisoPeerId edge_peer_id;
    uint64_t local_incarnation;
    MeisoSessionNonce nonce;
    uint64_t catalog_generation;
    MeisoCompositionSessionV3CatalogConfig catalog;
} ServiceConfig;

/*
 * The closed config requires numeric control_ip/control_port and
 * media_ip/media_port, both peer IDs, local_incarnation, nonce_high/low,
 * target_manifest=k230d_zero_gc2093_v1, and the exact probe receipt masks
 * wifi_supported_fields/device_supported_fields, catalog_generation and all
 * six endpoint incarnations. Every key is single-assignment. This private
 * manifest is trusted target configuration,
 * never a browser command; its capability values mirror EdgeApp's fixed
 * K230D/GC2093 catalog while the two probe masks carry boot-specific truth.
 */

typedef struct {
    bool used;
    uint64_t editor_id;
    MeisoCompositionSceneObjectId object_id;
} EditorObjectIdentity;

typedef struct {
    const char *text;
    size_t size;
    size_t offset;
} JsonCursor;

typedef struct {
    uint64_t request_id;
    bool enabled;
    MeisoCompositionSceneDesired value;
    uint64_t editor_ids[HOST_EDITOR_OBJECTS];
} SceneCommand;

/*
 * CMFB/v1 is a local, big-endian 80-byte header followed by one copied access
 * unit: magic0, version4, header-size6, source8, format10, codec12, flags14,
 * width16, height18, payload-bytes20, sequence24, source clock/time32/40,
 * active plan48, world56 and applied Scene State version64; bytes72..79 zero.
 * It is not a Meiso wire frame and never crosses Runtime/Session.
 */
typedef struct {
    uint8_t bytes[HOST_FRAME_RECORD_BYTES];
    size_t size;
    size_t offset;
    bool pending;
    MeisoCompositionMediaFrameRef ref;
} FrameOutput;

typedef struct {
    ServiceConfig config;
    int control_fd;
    int media_fd;
    bool media_connecting;
    MeisoCompositionHostAppMediaRouteMask media_connect_mask;
    struct timespec media_connect_deadline;
    struct timespec media_retry_after;
    int frame_fd;
    void *app_arena;
    size_t app_arena_size;
    MeisoCompositionHostApp *app;
    char input[HOST_CONTROL_LINE_BYTES];
    size_t input_size;
    /* True only after read(STDIN_FILENO) returns zero, never from POLLHUP. */
    bool stdin_eof;
    uint8_t cube_bytes[HOST_CUBE_BYTES];
    size_t cube_size;
    bool cube_loaded;
    MeisoContentId cube_content_id;
    EditorObjectIdentity objects[HOST_EDITOR_OBJECTS];
    uint64_t next_object_serial;
    uint64_t scene_space_epoch;
    uint64_t host_clock_epoch;
    FrameOutput frame_output;
    uint8_t frame_drain_cursor;
    uint8_t loop_cursor;
    bool close_requested;
} Service;

static bool emit_typed_truth(Service *service);

static void close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static bool checked_add_size(size_t lhs, size_t rhs, size_t *out)
{
    if (out == NULL || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

static uint64_t mix_nonzero_epoch(uint64_t value)
{
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31U;
    return value == 0U ? 1U : value;
}

static bool parse_u64_text(
    const char *text,
    uint64_t maximum,
    uint64_t *out_value
)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || out_value == NULL || text[0] == '\0' ||
        text[0] == '-' || text[0] == '+') {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' ||
        (uint64_t)value > maximum) {
        return false;
    }
    *out_value = (uint64_t)value;
    return true;
}

static char *trim(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t' || *text == '\r' ||
           *text == '\n') {
        ++text;
    }
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\r' || end[-1] == '\n')) {
        --end;
    }
    *end = '\0';
    return text;
}

static MeisoCompositionSessionV3CatalogConfig default_catalog(void)
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
        0U, MEISO_WIFI_TELEMETRY_MIN_PERIOD_MS,
        MEISO_WIFI_TELEMETRY_MAX_PERIOD_MS,
        MEISO_WIFI_TELEMETRY_PERIOD_STEP_MS
    };
    value.device_telemetry_capability = (MeisoDeviceTelemetryCapability){
        0U, MEISO_DEVICE_TELEMETRY_MIN_PERIOD_MS,
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

static bool parse_numeric_address(
    const char *text,
    uint16_t port,
    struct sockaddr_storage *out_address,
    socklen_t *out_size
)
{
    struct sockaddr_in address4;
    struct sockaddr_in6 address6;

    if (text == NULL || out_address == NULL || out_size == NULL || port == 0U) {
        return false;
    }
    memset(out_address, 0, sizeof(*out_address));
    memset(&address4, 0, sizeof(address4));
    address4.sin_family = AF_INET;
    address4.sin_port = htons(port);
    if (inet_pton(AF_INET, text, &address4.sin_addr) == 1) {
        memcpy(out_address, &address4, sizeof(address4));
        *out_size = (socklen_t)sizeof(address4);
        return true;
    }
    memset(&address6, 0, sizeof(address6));
    address6.sin6_family = AF_INET6;
    address6.sin6_port = htons(port);
    if (inet_pton(AF_INET6, text, &address6.sin6_addr) == 1) {
        memcpy(out_address, &address6, sizeof(address6));
        *out_size = (socklen_t)sizeof(address6);
        return true;
    }
    return false;
}

static bool set_address_port(
    struct sockaddr_storage *address,
    uint16_t port
)
{
    if (address == NULL || port == 0U) {
        return false;
    }
    if (address->ss_family == AF_INET) {
        ((struct sockaddr_in *)(void *)address)->sin_port = htons(port);
        return true;
    }
    if (address->ss_family == AF_INET6) {
        ((struct sockaddr_in6 *)(void *)address)->sin6_port = htons(port);
        return true;
    }
    return false;
}

static bool parse_catalog_u64(
    ServiceConfig *config,
    const char *key,
    const char *value,
    bool *out_matched,
    uint64_t *out_bit
)
{
    uint64_t number;
    uint64_t *destination = NULL;

    if (strcmp(key, "wifi_incarnation") == 0) {
        destination = &config->catalog.wifi_incarnation;
        *out_bit = CONFIG_WIFI_INCARNATION;
    } else if (strcmp(key, "presentation_incarnation") == 0) {
        destination = &config->catalog.presentation_incarnation;
        *out_bit = CONFIG_PRESENTATION_INCARNATION;
    } else if (strcmp(key, "scene_incarnation") == 0) {
        destination = &config->catalog.scene_incarnation;
        *out_bit = CONFIG_SCENE_INCARNATION;
    } else if (strcmp(key, "device_incarnation") == 0) {
        destination = &config->catalog.device_telemetry_incarnation;
        *out_bit = CONFIG_DEVICE_INCARNATION;
    } else if (strcmp(key, "render_incarnation") == 0) {
        destination = &config->catalog.render_media_incarnation;
        *out_bit = CONFIG_RENDER_INCARNATION;
    } else if (strcmp(key, "camera_incarnation") == 0) {
        destination = &config->catalog.camera_media_incarnation;
        *out_bit = CONFIG_CAMERA_INCARNATION;
    } else {
        *out_matched = false;
        return true;
    }
    *out_matched = true;
    if (!parse_u64_text(value, UINT64_MAX, &number) || number == 0U) {
        return false;
    }
    *destination = number;
    return true;
}

static bool parse_config_line(
    ServiceConfig *config,
    uint64_t *seen,
    char *line
)
{
    char *comment;
    char *separator;
    char *key;
    char *value;
    uint64_t number;
    uint64_t bit = 0U;
    bool catalog_matched;

    comment = strchr(line, '#');
    if (comment != NULL) {
        *comment = '\0';
    }
    key = trim(line);
    if (*key == '\0') {
        return true;
    }
    separator = strchr(key, '=');
    if (separator == NULL || strchr(separator + 1, '=') != NULL) {
        return false;
    }
    *separator = '\0';
    value = trim(separator + 1);
    key = trim(key);

    if (strcmp(key, "control_ip") == 0) {
        bit = CONFIG_CONTROL_ADDRESS;
        if (strlen(value) >= INET6_ADDRSTRLEN) {
            return false;
        }
        if (!parse_numeric_address(value, 1U, &config->control_address,
                                   &config->control_address_size)) {
            return false;
        }
    } else if (strcmp(key, "control_port") == 0) {
        bit = CONFIG_CONTROL_PORT;
        if (!parse_u64_text(value, UINT16_MAX, &number) || number == 0U) {
            return false;
        }
        config->control_port = (uint16_t)number;
    } else if (strcmp(key, "media_ip") == 0) {
        bit = CONFIG_MEDIA_ADDRESS;
        if (strlen(value) >= INET6_ADDRSTRLEN) {
            return false;
        }
        if (!parse_numeric_address(value, 1U, &config->media_address,
                                   &config->media_address_size)) {
            return false;
        }
    } else if (strcmp(key, "media_port") == 0) {
        bit = CONFIG_MEDIA_PORT;
        if (!parse_u64_text(value, UINT16_MAX, &number) || number == 0U) {
            return false;
        }
        config->media_port = (uint16_t)number;
    } else if (strcmp(key, "local_host_peer_id_high") == 0) {
        bit = CONFIG_LOCAL_PEER_HIGH;
        if (!parse_u64_text(value, UINT64_MAX, &config->local_peer_id.high)) {
            return false;
        }
    } else if (strcmp(key, "local_host_peer_id_low") == 0) {
        bit = CONFIG_LOCAL_PEER_LOW;
        if (!parse_u64_text(value, UINT64_MAX, &config->local_peer_id.low)) {
            return false;
        }
    } else if (strcmp(key, "expected_edge_peer_id_high") == 0) {
        bit = CONFIG_EDGE_PEER_HIGH;
        if (!parse_u64_text(value, UINT64_MAX, &config->edge_peer_id.high)) {
            return false;
        }
    } else if (strcmp(key, "expected_edge_peer_id_low") == 0) {
        bit = CONFIG_EDGE_PEER_LOW;
        if (!parse_u64_text(value, UINT64_MAX, &config->edge_peer_id.low)) {
            return false;
        }
    } else if (strcmp(key, "local_incarnation") == 0) {
        bit = CONFIG_LOCAL_INCARNATION;
        if (!parse_u64_text(value, UINT64_MAX, &config->local_incarnation) ||
            config->local_incarnation == 0U) {
            return false;
        }
    } else if (strcmp(key, "nonce_high") == 0) {
        bit = CONFIG_NONCE_HIGH;
        if (!parse_u64_text(value, UINT64_MAX, &config->nonce.high)) {
            return false;
        }
    } else if (strcmp(key, "nonce_low") == 0) {
        bit = CONFIG_NONCE_LOW;
        if (!parse_u64_text(value, UINT64_MAX, &config->nonce.low)) {
            return false;
        }
    } else if (strcmp(key, "target_manifest") == 0) {
        bit = CONFIG_TARGET_MANIFEST;
        if (strcmp(value, "k230d_zero_gc2093_v1") != 0) {
            return false;
        }
    } else if (strcmp(key, "wifi_supported_fields") == 0) {
        bit = CONFIG_WIFI_FIELDS;
        if (!parse_u64_text(value, MEISO_WIFI_TELEMETRY_FIELD_ALL, &number) ||
            number == 0U) {
            return false;
        }
        config->catalog.wifi_capability.supported_fields = (uint32_t)number;
    } else if (strcmp(key, "device_supported_fields") == 0) {
        bit = CONFIG_DEVICE_FIELDS;
        if (!parse_u64_text(value, MEISO_DEVICE_TELEMETRY_FIELD_ALL, &number) ||
            number == 0U) {
            return false;
        }
        config->catalog.device_telemetry_capability.supported_fields =
            (uint32_t)number;
    } else if (strcmp(key, "catalog_generation") == 0) {
        bit = CONFIG_CATALOG_GENERATION;
        if (!parse_u64_text(value, UINT64_MAX, &config->catalog_generation) ||
            config->catalog_generation == 0U) {
            return false;
        }
    } else if (!parse_catalog_u64(
                   config, key, value, &catalog_matched, &bit
               ) || !catalog_matched) {
        return false;
    }

    if ((*seen & bit) != 0U) {
        return false;
    }
    *seen |= bit;
    return true;
}

static bool load_config(const char *path, ServiceConfig *out_config)
{
    FILE *file;
    ServiceConfig config;
    char line[HOST_CONFIG_LINE_BYTES];
    uint64_t seen = 0U;
    bool ok = true;

    if (path == NULL || out_config == NULL) {
        return false;
    }
    memset(&config, 0, sizeof(config));
    config.catalog_generation = 1U;
    config.catalog = default_catalog();
    file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strchr(line, '\n') == NULL && !feof(file)) {
            ok = false;
            break;
        }
        if (!parse_config_line(&config, &seen, line)) {
            ok = false;
            break;
        }
    }
    if (ferror(file) != 0) {
        ok = false;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok || (seen & CONFIG_REQUIRED) != CONFIG_REQUIRED ||
        !set_address_port(&config.control_address, config.control_port) ||
        !set_address_port(&config.media_address, config.media_port) ||
        (config.nonce.high == 0U && config.nonce.low == 0U) ||
        (config.local_peer_id.high == 0U && config.local_peer_id.low == 0U) ||
        (config.edge_peer_id.high == 0U && config.edge_peer_id.low == 0U)) {
        return false;
    }
    if ((config.catalog.device_telemetry_capability.supported_fields &
         MEISO_DEVICE_TELEMETRY_FIELD_TEMPERATURE_MILLIC) == 0U) {
        config.catalog.device_telemetry_capability.min_temperature_millic = 0;
        config.catalog.device_telemetry_capability.max_temperature_millic = 0;
        config.catalog.device_telemetry_capability
            .max_temperature_uncertainty_millic = 0U;
    }
    *out_config = config;
    return true;
}

static bool set_nonblocking(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool deadline_after_ms(int milliseconds, struct timespec *out_deadline)
{
    struct timespec now;
    int64_t nanoseconds;

    if (out_deadline == NULL || milliseconds < 0 ||
        clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return false;
    }
    nanoseconds = (int64_t)now.tv_nsec +
        (int64_t)milliseconds * INT64_C(1000000);
    out_deadline->tv_sec = now.tv_sec + (time_t)(nanoseconds / INT64_C(1000000000));
    out_deadline->tv_nsec = (long)(nanoseconds % INT64_C(1000000000));
    return true;
}

static int deadline_remaining_ms(const struct timespec *deadline)
{
    struct timespec now;
    int64_t milliseconds;

    if (deadline == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    milliseconds = ((int64_t)deadline->tv_sec - (int64_t)now.tv_sec) *
        INT64_C(1000) +
        ((int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec) / INT64_C(1000000);
    if (milliseconds <= 0) {
        return 0;
    }
    return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

static int connect_numeric(
    const struct sockaddr_storage *address,
    socklen_t address_size
)
{
    struct timespec deadline;
    struct pollfd descriptor;
    int fd = -1;
    int error_number;
    socklen_t error_size;
    int result;

    if (address == NULL ||
        (address->ss_family != AF_INET && address->ss_family != AF_INET6) ||
        !deadline_after_ms(HOST_CONNECT_TIMEOUT_MS, &deadline)) {
        return -1;
    }
    fd = socket(address->ss_family, SOCK_STREAM, 0);
    if (fd < 0 || !set_nonblocking(fd)) {
        close_fd(&fd);
        return -1;
    }
    result = connect(fd, (const struct sockaddr *)(const void *)address,
                     address_size);
    if (result == 0) {
        return fd;
    }
    if (errno != EINPROGRESS) {
        close_fd(&fd);
        return -1;
    }
    descriptor.fd = fd;
    descriptor.events = POLLOUT;
    descriptor.revents = 0;
    for (;;) {
        int remaining = deadline_remaining_ms(&deadline);
        if (remaining <= 0) {
            close_fd(&fd);
            return -1;
        }
        result = poll(&descriptor, 1U, remaining);
        if (result > 0) {
            break;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        close_fd(&fd);
        return -1;
    }
    error_number = 0;
    error_size = (socklen_t)sizeof(error_number);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error_number, &error_size) != 0 ||
        error_number != 0) {
        close_fd(&fd);
        return -1;
    }
    return fd;
}

static int start_numeric_connect(
    const struct sockaddr_storage *address,
    socklen_t address_size,
    bool *out_connected
)
{
    int fd;
    int result;

    if (address == NULL || out_connected == NULL ||
        (address->ss_family != AF_INET && address->ss_family != AF_INET6)) {
        return -1;
    }
    fd = socket(address->ss_family, SOCK_STREAM, 0);
    if (fd < 0 || !set_nonblocking(fd)) {
        close_fd(&fd);
        return -1;
    }
    result = connect(fd, (const struct sockaddr *)(const void *)address,
                     address_size);
    if (result == 0) {
        *out_connected = true;
        return fd;
    }
    if (errno != EINPROGRESS) {
        close_fd(&fd);
        return -1;
    }
    *out_connected = false;
    return fd;
}

static bool time_reached(const struct timespec *when)
{
    struct timespec now;

    if (when->tv_sec == 0 && when->tv_nsec == 0) {
        return true;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return false;
    }
    return now.tv_sec > when->tv_sec ||
        (now.tv_sec == when->tv_sec && now.tv_nsec >= when->tv_nsec);
}

static void media_retry_later(Service *service)
{
    if (!deadline_after_ms(1000, &service->media_retry_after)) {
        memset(&service->media_retry_after, 0,
               sizeof(service->media_retry_after));
    }
}

static void json_skip_space(JsonCursor *cursor)
{
    while (cursor->offset < cursor->size &&
           (cursor->text[cursor->offset] == ' ' ||
            cursor->text[cursor->offset] == '\t' ||
            cursor->text[cursor->offset] == '\r' ||
            cursor->text[cursor->offset] == '\n')) {
        ++cursor->offset;
    }
}

static bool json_literal(JsonCursor *cursor, const char *literal)
{
    size_t size;

    if (cursor == NULL || literal == NULL) {
        return false;
    }
    json_skip_space(cursor);
    size = strlen(literal);
    if (size > cursor->size - cursor->offset ||
        memcmp(cursor->text + cursor->offset, literal, size) != 0) {
        return false;
    }
    cursor->offset += size;
    return true;
}

static bool json_end(JsonCursor *cursor)
{
    json_skip_space(cursor);
    return cursor->offset == cursor->size;
}

static bool json_u64(JsonCursor *cursor, uint64_t maximum, uint64_t *out_value)
{
    size_t start;
    char buffer[32];
    size_t size;

    if (cursor == NULL || out_value == NULL) {
        return false;
    }
    json_skip_space(cursor);
    start = cursor->offset;
    while (cursor->offset < cursor->size &&
           cursor->text[cursor->offset] >= '0' &&
           cursor->text[cursor->offset] <= '9') {
        ++cursor->offset;
    }
    size = cursor->offset - start;
    if (size == 0U || size >= sizeof(buffer)) {
        cursor->offset = start;
        return false;
    }
    memcpy(buffer, cursor->text + start, size);
    buffer[size] = '\0';
    if (!parse_u64_text(buffer, maximum, out_value)) {
        cursor->offset = start;
        return false;
    }
    return true;
}

static bool json_u32(JsonCursor *cursor, uint32_t *out_value)
{
    uint64_t value;

    if (!json_u64(cursor, UINT32_MAX, &value)) {
        return false;
    }
    *out_value = (uint32_t)value;
    return true;
}

static bool json_u16(JsonCursor *cursor, uint16_t *out_value)
{
    uint64_t value;

    if (!json_u64(cursor, UINT16_MAX, &value)) {
        return false;
    }
    *out_value = (uint16_t)value;
    return true;
}

static bool json_bool(JsonCursor *cursor, bool *out_value)
{
    if (cursor == NULL || out_value == NULL) {
        return false;
    }
    if (json_literal(cursor, "true")) {
        *out_value = true;
        return true;
    }
    if (json_literal(cursor, "false")) {
        *out_value = false;
        return true;
    }
    return false;
}

static bool json_float(JsonCursor *cursor, float *out_value)
{
    size_t start;
    size_t size;
    char buffer[64];
    char *end = NULL;
    float value;

    if (cursor == NULL || out_value == NULL) {
        return false;
    }
    json_skip_space(cursor);
    start = cursor->offset;
    while (cursor->offset < cursor->size &&
           strchr("-+0123456789.eE", cursor->text[cursor->offset]) != NULL) {
        ++cursor->offset;
    }
    size = cursor->offset - start;
    if (size == 0U || size >= sizeof(buffer)) {
        cursor->offset = start;
        return false;
    }
    memcpy(buffer, cursor->text + start, size);
    buffer[size] = '\0';
    errno = 0;
    value = strtof(buffer, &end);
    if (errno != 0 || end == buffer || *end != '\0' || !isfinite(value)) {
        cursor->offset = start;
        return false;
    }
    *out_value = value;
    return true;
}

static bool json_float_array(
    JsonCursor *cursor,
    float *values,
    size_t count
)
{
    size_t index;

    if (!json_literal(cursor, "[")) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        if ((index != 0U && !json_literal(cursor, ",")) ||
            !json_float(cursor, &values[index])) {
            return false;
        }
    }
    return json_literal(cursor, "]");
}

static bool json_color_format(
    JsonCursor *cursor,
    MeisoCompositionSceneColorFormat *out_format
)
{
    if (json_literal(cursor, "\"rgba8888\"")) {
        *out_format = MEISO_COMPOSITION_SCENE_COLOR_RGBA8888;
        return true;
    }
    if (json_literal(cursor, "\"rgb565\"")) {
        *out_format = MEISO_COMPOSITION_SCENE_COLOR_RGB565;
        return true;
    }
    return false;
}

static bool json_pixel_format(
    JsonCursor *cursor,
    MeisoCompositionMediaPixelFormat *out_format
)
{
    if (json_literal(cursor, "\"rgba8888\"")) {
        *out_format = MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888;
        return true;
    }
    if (json_literal(cursor, "\"rgb565\"")) {
        *out_format = MEISO_COMPOSITION_MEDIA_PIXEL_RGB565;
        return true;
    }
    if (json_literal(cursor, "\"nv12\"")) {
        *out_format = MEISO_COMPOSITION_MEDIA_PIXEL_NV12;
        return true;
    }
    if (json_literal(cursor, "\"yuyv\"")) {
        *out_format = MEISO_COMPOSITION_MEDIA_PIXEL_YUYV;
        return true;
    }
    return false;
}

static bool parse_scene_camera(
    JsonCursor *cursor,
    MeisoCompositionSceneCamera *camera
)
{
    memset(camera, 0, sizeof(*camera));
    camera->space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    camera->projection = MEISO_COMPOSITION_SCENE_PROJECTION_PERSPECTIVE;
    if (!json_literal(cursor, "{\"position\":") ||
        !json_float_array(cursor, camera->position, 3U) ||
        !json_literal(cursor, ",\"orientation_xyzw\":") ||
        !json_float_array(cursor, camera->orientation_xyzw, 4U) ||
        !json_literal(cursor, ",\"vertical_fov_degrees\":") ||
        !json_float(cursor, &camera->vertical_fov_degrees) ||
        !json_literal(cursor, ",\"near_z\":") ||
        !json_float(cursor, &camera->near_z) ||
        !json_literal(cursor, ",\"far_z\":") ||
        !json_float(cursor, &camera->far_z) || !json_literal(cursor, "}")) {
        return false;
    }
    return true;
}

static bool parse_scene_object(
    JsonCursor *cursor,
    uint64_t *out_editor_id,
    MeisoCompositionSceneObject *object
)
{
    bool visible;

    memset(object, 0, sizeof(*object));
    object->space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    if (!json_literal(cursor, "{\"editor_id\":") ||
        !json_u64(cursor, UINT64_MAX, out_editor_id) ||
        !json_literal(cursor, ",\"asset_handle\":\"cube\"") ||
        !json_literal(cursor, ",\"position\":") ||
        !json_float_array(cursor, object->transform.position, 3U) ||
        !json_literal(cursor, ",\"orientation_xyzw\":") ||
        !json_float_array(cursor, object->transform.orientation_xyzw, 4U) ||
        !json_literal(cursor, ",\"scale\":") ||
        !json_float_array(cursor, object->transform.scale, 3U) ||
        !json_literal(cursor, ",\"visible\":") ||
        !json_bool(cursor, &visible) || !json_literal(cursor, "}")) {
        return false;
    }
    object->flags = visible ? MEISO_COMPOSITION_SCENE_OBJECT_VISIBLE : 0U;
    return true;
}

static bool parse_scene_command(JsonCursor *cursor, SceneCommand *out_command)
{
    uint32_t count = 0U;

    memset(out_command, 0, sizeof(*out_command));
    if (!json_literal(cursor, "{\"command\":\"set_scene\",\"request_id\":") ||
        !json_u64(cursor, UINT64_MAX, &out_command->request_id) ||
        !json_literal(cursor, ",\"enabled\":") ||
        !json_bool(cursor, &out_command->enabled)) {
        return false;
    }
    if (!out_command->enabled) {
        return json_literal(cursor, "}") && json_end(cursor);
    }
    if (!json_literal(cursor, ",\"camera\":") ||
        !parse_scene_camera(cursor, &out_command->value.camera) ||
        !json_literal(cursor, ",\"objects\":[")) {
        return false;
    }
    while (!json_literal(cursor, "]")) {
        if (count >= HOST_EDITOR_OBJECTS ||
            (count != 0U && !json_literal(cursor, ",")) ||
            !parse_scene_object(cursor, &out_command->editor_ids[count],
                                &out_command->value.objects[count])) {
            return false;
        }
        ++count;
    }
    out_command->value.flags = MEISO_COMPOSITION_SCENE_FLAGS_NONE;
    out_command->value.object_count = count;
    return json_literal(cursor, "}") && json_end(cursor);
}

static const char *command_kind_name(MeisoCompositionHostAppCommandKind value)
{
    switch (value) {
        case MEISO_COMPOSITION_HOST_APP_COMMAND_PUT_MESH: return "put_mesh";
        case MEISO_COMPOSITION_HOST_APP_COMMAND_SET_SCENE: return "set_scene";
        case MEISO_COMPOSITION_HOST_APP_COMMAND_SET_PRESENTATION:
            return "set_presentation";
        case MEISO_COMPOSITION_HOST_APP_COMMAND_SET_WIFI: return "set_wifi";
        case MEISO_COMPOSITION_HOST_APP_COMMAND_SET_DEVICE: return "set_device";
        case MEISO_COMPOSITION_HOST_APP_COMMAND_SET_RENDER_MEDIA:
            return "set_render_media";
        case MEISO_COMPOSITION_HOST_APP_COMMAND_SET_CAMERA_MEDIA:
            return "set_camera_media";
        default: return "unknown";
    }
}

static const char *local_result_name(MeisoCompositionHostAppLocalQueueResult value)
{
    switch (value) {
        case MEISO_COMPOSITION_HOST_APP_LOCAL_QUEUED: return "queued";
        case MEISO_COMPOSITION_HOST_APP_LOCAL_REPLACED_PENDING:
            return "replaced_pending";
        default: return "unknown";
    }
}

static const char *phase_name(MeisoCompositionHostAppPhase value)
{
    switch (value) {
        case MEISO_COMPOSITION_HOST_APP_PHASE_HANDSHAKE: return "handshake";
        case MEISO_COMPOSITION_HOST_APP_PHASE_CATALOG: return "catalog";
        case MEISO_COMPOSITION_HOST_APP_PHASE_REBIND: return "rebind";
        case MEISO_COMPOSITION_HOST_APP_PHASE_READY: return "ready";
        case MEISO_COMPOSITION_HOST_APP_PHASE_TERMINAL: return "terminal";
        default: return "unknown";
    }
}

static const char *terminal_reason_name(
    MeisoCompositionHostAppTerminalReason value
)
{
    switch (value) {
        case MEISO_COMPOSITION_HOST_APP_TERMINAL_NONE: return "none";
        case MEISO_COMPOSITION_HOST_APP_TERMINAL_CONTROL_TRANSPORT:
            return "control_transport";
        case MEISO_COMPOSITION_HOST_APP_TERMINAL_CONTROL_SESSION:
            return "control_session";
        case MEISO_COMPOSITION_HOST_APP_TERMINAL_CATALOG_CHANGED:
            return "catalog_changed";
        case MEISO_COMPOSITION_HOST_APP_TERMINAL_LOCAL_ABORT:
            return "local_abort";
        case MEISO_COMPOSITION_HOST_APP_TERMINAL_INTERNAL_FAILURE:
            return "internal_failure";
        default: return "unknown";
    }
}

static const char *apply_result_name(MeisoRuntimeApplyResult value)
{
    switch (value) {
        case MEISO_RUNTIME_APPLY_NONE: return "none";
        case MEISO_RUNTIME_APPLY_PENDING: return "pending";
        case MEISO_RUNTIME_APPLY_APPLIED: return "applied";
        case MEISO_RUNTIME_APPLY_REJECTED: return "rejected";
        case MEISO_RUNTIME_APPLY_FAILED: return "failed";
        default: return "unknown";
    }
}

static const char *availability_name(MeisoRuntimeAvailability value)
{
    switch (value) {
        case MEISO_RUNTIME_AVAILABILITY_PRESENT: return "present";
        case MEISO_RUNTIME_AVAILABILITY_ABSENT: return "absent";
        case MEISO_RUNTIME_AVAILABILITY_UNAVAILABLE: return "unavailable";
        default: return "unknown";
    }
}

static const char *production_status_name(MeisoRuntimeProductionStatus value)
{
    switch (value) {
        case MEISO_RUNTIME_PRODUCTION_STOPPED: return "stopped";
        case MEISO_RUNTIME_PRODUCTION_STARTING: return "starting";
        case MEISO_RUNTIME_PRODUCTION_RUNNING: return "running";
        case MEISO_RUNTIME_PRODUCTION_DEGRADED: return "degraded";
        case MEISO_RUNTIME_PRODUCTION_STOPPING: return "stopping";
        case MEISO_RUNTIME_PRODUCTION_FAILED: return "failed";
        default: return "unknown";
    }
}

static const char *production_reason_name(MeisoRuntimeProductionReason value)
{
    switch (value) {
        case MEISO_RUNTIME_PRODUCTION_REASON_NONE: return "none";
        case MEISO_RUNTIME_PRODUCTION_REASON_DISABLED: return "disabled";
        case MEISO_RUNTIME_PRODUCTION_REASON_APPLYING: return "applying";
        case MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER:
            return "unsupported_parameter";
        case MEISO_RUNTIME_PRODUCTION_REASON_ADMISSION_DENIED:
            return "admission_denied";
        case MEISO_RUNTIME_PRODUCTION_REASON_UNAVAILABLE: return "unavailable";
        case MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED: return "apply_failed";
        default: return "unknown";
    }
}

static const char *scene_reason_name(MeisoCompositionSceneStableReason value)
{
    switch (value) {
        case MEISO_COMPOSITION_SCENE_REASON_NONE: return "none";
        case MEISO_COMPOSITION_SCENE_REASON_READY: return "ready";
        case MEISO_COMPOSITION_SCENE_REASON_WAITING_BLOB: return "waiting_blob";
        case MEISO_COMPOSITION_SCENE_REASON_OBJECT_CAPACITY:
            return "object_capacity";
        case MEISO_COMPOSITION_SCENE_REASON_BLOB_CAPACITY:
            return "blob_capacity";
        case MEISO_COMPOSITION_SCENE_REASON_MESH_BYTES_CAPACITY:
            return "mesh_bytes_capacity";
        case MEISO_COMPOSITION_SCENE_REASON_BUFFER_CAPACITY:
            return "buffer_capacity";
        case MEISO_COMPOSITION_SCENE_REASON_INVALID_SCENE:
            return "invalid_scene";
        case MEISO_COMPOSITION_SCENE_REASON_RENDERER_FAILED:
            return "renderer_failed";
        default: return "unknown";
    }
}

static const char *candidate_state_name(MeisoCompositionSceneCandidateState value)
{
    switch (value) {
        case MEISO_COMPOSITION_SCENE_CANDIDATE_NONE: return "none";
        case MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARING: return "preparing";
        case MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED: return "prepared";
        case MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING:
            return "activation_pending";
        case MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED: return "rejected";
        case MEISO_COMPOSITION_SCENE_CANDIDATE_FAILED: return "failed";
        default: return "unknown";
    }
}

static const char *active_state_name(MeisoCompositionSceneActiveState value)
{
    switch (value) {
        case MEISO_COMPOSITION_SCENE_ACTIVE_NONE: return "none";
        case MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE: return "active";
        case MEISO_COMPOSITION_SCENE_ACTIVE_RETIRING: return "retiring";
        case MEISO_COMPOSITION_SCENE_ACTIVE_SAFETY: return "safety";
        default: return "unknown";
    }
}

static const char *presentation_reason_name(
    MeisoCompositionScenePresentationReason value
)
{
    switch (value) {
        case MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE: return "none";
        case MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_WAITING_SCENE:
            return "waiting_scene";
        case MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_CAPACITY:
            return "capacity";
        case MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN:
            return "invalid_plan";
        case MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_RENDERER_FAILED:
            return "renderer_failed";
        default: return "unknown";
    }
}

static const char *scene_color_format_name(
    MeisoCompositionSceneColorFormat value
)
{
    switch (value) {
        case MEISO_COMPOSITION_SCENE_COLOR_RGBA8888: return "rgba8888";
        case MEISO_COMPOSITION_SCENE_COLOR_RGB565: return "rgb565";
        default: return "unknown";
    }
}

static const char *scene_depth_format_name(
    MeisoCompositionSceneDepthFormat value
)
{
    return value == MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32 ?
        "implementation32" : "unknown";
}

static void sha256_hex(const uint8_t digest[MEISO_SHA256_DIGEST_SIZE], char out[65])
{
    static const char digits[] = "0123456789abcdef";
    size_t index;

    for (index = 0U; index < MEISO_SHA256_DIGEST_SIZE; ++index) {
        out[index * 2U] = digits[digest[index] >> 4U];
        out[index * 2U + 1U] = digits[digest[index] & 0x0fU];
    }
    out[64] = '\0';
}

static const char *actual_channel_name(MeisoCompositionHostAppActualChannel value)
{
    static const char *const names[] = {
        "wifi", "presentation", "scene", "device", "render_media",
        "camera_media"
    };
    return value < MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT ? names[value] :
        "unknown";
}

static const char *desired_channel_name(MeisoCompositionHostAppDesiredChannel value)
{
    return actual_channel_name((MeisoCompositionHostAppActualChannel)value);
}

static const char *inline_channel_name(MeisoCompositionHostAppInlineChannel value)
{
    static const char *const names[] = {"wifi", "render_result", "device"};
    return value < MEISO_COMPOSITION_HOST_APP_INLINE_STREAM_COUNT ? names[value] :
        "unknown";
}

static const char *media_source_name(MeisoCompositionMediaSource value)
{
    switch (value) {
        case MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW: return "render";
        case MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA: return "camera";
        default: return "unknown";
    }
}

static const char *pixel_format_name(MeisoCompositionMediaPixelFormat value)
{
    switch (value) {
        case MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888: return "rgba8888";
        case MEISO_COMPOSITION_MEDIA_PIXEL_RGB565: return "rgb565";
        case MEISO_COMPOSITION_MEDIA_PIXEL_NV12: return "nv12";
        case MEISO_COMPOSITION_MEDIA_PIXEL_YUYV: return "yuyv";
        default: return "unknown";
    }
}

static const char *codec_name(MeisoCompositionMediaCodec value)
{
    return value == MEISO_COMPOSITION_MEDIA_CODEC_RAW ? "raw" : "unknown";
}

static const char *media_mask_name(MeisoCompositionHostAppMediaRouteMask value)
{
    switch (value) {
        case 0U: return "none";
        case MEISO_COMPOSITION_HOST_APP_MEDIA_RENDER: return "render";
        case MEISO_COMPOSITION_HOST_APP_MEDIA_CAMERA: return "camera";
        case MEISO_COMPOSITION_HOST_APP_MEDIA_ALL: return "both";
        default: return "unknown";
    }
}

static const char *pump_event_name(MeisoCompositionHostAppPumpEvent value)
{
    switch (value) {
        case MEISO_COMPOSITION_HOST_APP_PUMP_CONTROL_PROGRESS:
            return "control_progress";
        case MEISO_COMPOSITION_HOST_APP_PUMP_DESIRED_DELIVERY:
            return "desired_delivery";
        case MEISO_COMPOSITION_HOST_APP_PUMP_ACTUAL_UPDATED:
            return "actual_updated";
        case MEISO_COMPOSITION_HOST_APP_PUMP_INLINE_UPDATED:
            return "inline_updated";
        case MEISO_COMPOSITION_HOST_APP_PUMP_MESH_PROGRESS:
            return "mesh_progress";
        case MEISO_COMPOSITION_HOST_APP_PUMP_MEDIA_PROGRESS:
            return "media_progress";
        case MEISO_COMPOSITION_HOST_APP_PUMP_MEDIA_FRAME:
            return "media_frame";
        case MEISO_COMPOSITION_HOST_APP_PUMP_BOUNDARY: return "boundary";
        case MEISO_COMPOSITION_HOST_APP_PUMP_EVENT_TERMINAL: return "terminal";
        default: return "unknown";
    }
}

static bool emit_line(const char *line)
{
    return line != NULL && fputs(line, stdout) >= 0 && fputc('\n', stdout) != EOF &&
        fflush(stdout) == 0;
}

static bool emit_error(uint64_t request_id, const char *code)
{
    char line[256];
    int written;

    written = snprintf(
        line, sizeof(line),
        "{\"type\":\"error\",\"request_id\":%" PRIu64
        ",\"code\":\"%s\"}", request_id, code
    );
    return written > 0 && (size_t)written < sizeof(line) && emit_line(line);
}

static bool emit_receipt(
    uint64_t request_id,
    const MeisoCompositionHostAppCommandReceipt *receipt
)
{
    char line[640];
    int written;

    written = snprintf(
        line, sizeof(line),
        "{\"type\":\"command.receipt\",\"request_id\":%" PRIu64
        ",\"command_id\":%" PRIu64 ",\"kind\":\"%s\","
        "\"local_result\":\"%s\",\"replaced\":%s,"
        "\"replaced_command_id\":%" PRIu64 ","
        "\"state_version\":%" PRIu64 ","
        "\"plan_generation\":%" PRIu64 "}",
        request_id, receipt->command_id, command_kind_name(receipt->kind),
        local_result_name(receipt->local_result),
        receipt->replaced_command_present ? "true" : "false",
        receipt->replaced_command_id,
        receipt->desired_state_present ? receipt->desired_state_version : 0U,
        receipt->plan_generation_present ? receipt->plan_generation : 0U
    );
    return written > 0 && (size_t)written < sizeof(line) && emit_line(line);
}

static const char *app_status_code(MeisoCompositionHostAppStatus status)
{
    switch (status) {
        case MEISO_COMPOSITION_HOST_APP_INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case MEISO_COMPOSITION_HOST_APP_INVALID_STATE:
            return "INVALID_STATE";
        case MEISO_COMPOSITION_HOST_APP_CAPACITY:
            return "CAPACITY";
        case MEISO_COMPOSITION_HOST_APP_BUSY:
            return "BUSY";
        case MEISO_COMPOSITION_HOST_APP_CONFLICT:
            return "CONFLICT";
        case MEISO_COMPOSITION_HOST_APP_NOT_READY:
            return "NOT_READY";
        case MEISO_COMPOSITION_HOST_APP_NO_FRAME:
            return "NO_FRAME";
        case MEISO_COMPOSITION_HOST_APP_FRAME_MISMATCH:
            return "FRAME_MISMATCH";
        case MEISO_COMPOSITION_HOST_APP_COUNTER_EXHAUSTED:
            return "COUNTER_EXHAUSTED";
        case MEISO_COMPOSITION_HOST_APP_TERMINAL:
            return "TERMINAL";
        default:
            return "INTERNAL_FAILURE";
    }
}

static bool allocate_scene_ids(Service *service, SceneCommand *command)
{
    EditorObjectIdentity candidate[HOST_EDITOR_OBJECTS];
    uint64_t next_serial;
    uint32_t object_index;
    size_t identity_index;

    memset(candidate, 0, sizeof(candidate));
    next_serial = service->next_object_serial;
    for (object_index = 0U; object_index < command->value.object_count;
         ++object_index) {
        EditorObjectIdentity *found = NULL;
        uint32_t previous;

        for (previous = 0U; previous < object_index; ++previous) {
            if (command->editor_ids[previous] ==
                command->editor_ids[object_index]) {
                return false;
            }
        }
        for (identity_index = 0U; identity_index < HOST_EDITOR_OBJECTS;
             ++identity_index) {
            if (service->objects[identity_index].used &&
                service->objects[identity_index].editor_id ==
                    command->editor_ids[object_index]) {
                size_t candidate_index;
                for (candidate_index = 0U;
                     candidate_index < HOST_EDITOR_OBJECTS;
                     ++candidate_index) {
                    if (!candidate[candidate_index].used) {
                        candidate[candidate_index] =
                            service->objects[identity_index];
                        found = &candidate[candidate_index];
                        break;
                    }
                }
                break;
            }
        }
        if (found == NULL) {
            for (identity_index = 0U; identity_index < HOST_EDITOR_OBJECTS;
                 ++identity_index) {
                if (!candidate[identity_index].used) {
                    found = &candidate[identity_index];
                    break;
                }
            }
            if (found == NULL || next_serial == UINT64_MAX) {
                return false;
            }
            ++next_serial;
            found->used = true;
            found->editor_id = command->editor_ids[object_index];
            found->object_id.high = service->config.local_incarnation;
            found->object_id.low = next_serial;
        }
        command->value.objects[object_index].id = found->object_id;
        command->value.objects[object_index].mesh_content_id =
            service->cube_content_id;
    }
    memcpy(service->objects, candidate, sizeof(candidate));
    service->next_object_serial = next_serial;
    return true;
}

static bool stamp_scene_truth(Service *service, SceneCommand *command)
{
    struct timespec now;
    uint64_t seconds;
    uint64_t monotonic_ns;
    uint32_t index;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > UINT64_MAX / UINT64_C(1000000000)) {
        return false;
    }
    seconds = (uint64_t)now.tv_sec;
    monotonic_ns = seconds * UINT64_C(1000000000);
    if ((uint64_t)now.tv_nsec > UINT64_MAX - monotonic_ns) {
        return false;
    }
    monotonic_ns += (uint64_t)now.tv_nsec;
    command->value.camera.space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    command->value.camera.space_epoch = service->scene_space_epoch;
    command->value.camera.source_time =
        (MeisoTimePoint){service->host_clock_epoch, monotonic_ns};
    command->value.camera.projection =
        MEISO_COMPOSITION_SCENE_PROJECTION_PERSPECTIVE;
    command->value.camera.viewport_x = 0U;
    command->value.camera.viewport_y = 0U;
    command->value.camera.viewport_width =
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    command->value.camera.viewport_height =
        MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    for (index = 0U; index < command->value.object_count; ++index) {
        command->value.objects[index].space =
            MEISO_COMPOSITION_SCENE_SPACE_WORLD;
        command->value.objects[index].space_epoch = service->scene_space_epoch;
    }
    return true;
}

static bool handle_asset_command(Service *service, const char *line, size_t size)
{
    JsonCursor cursor = {line, size, 0U};
    uint64_t request_id;
    MeisoCompositionHostAppCommandReceipt receipt;
    MeisoCompositionHostAppStatus status;

    if (!json_literal(
            &cursor,
            "{\"command\":\"asset.load_builtin\",\"request_id\":"
        ) || !json_u64(&cursor, UINT64_MAX, &request_id) ||
        !json_literal(&cursor, ",\"asset_handle\":\"cube\"}") ||
        !json_end(&cursor)) {
        return emit_error(0U, "INVALID_COMMAND");
    }
    if (service->cube_loaded) {
        MeisoCompositionHostAppSnapshot snapshot;
        if (!meiso_composition_host_app_snapshot(service->app, &snapshot)) {
            return emit_error(request_id, "SNAPSHOT_FAILED");
        }
        if (snapshot.mesh.edge_core_published) {
            char output[256];
            int written = snprintf(
                output, sizeof(output),
                "{\"type\":\"asset.loaded\",\"request_id\":%" PRIu64
                ",\"asset_handle\":\"cube\",\"already_loaded\":true}",
                request_id
            );
            return written > 0 && (size_t)written < sizeof(output) &&
                emit_line(output);
        }
        if (snapshot.mesh.queued || snapshot.mesh.transfer_active ||
            snapshot.mesh.waiting_result) {
            return emit_error(request_id, "BUSY");
        }
    } else {
        service->cube_size = sizeof(service->cube_bytes);
        if (meiso_composition_scene_mesh_encode(
                meiso_composition_scene_canonical_cube(),
                (MeisoMutableBuffer){
                    service->cube_bytes, sizeof(service->cube_bytes)
                },
                &service->cube_size
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
            return emit_error(request_id, "CUBE_ENCODE_FAILED");
        }
    }
    status = meiso_composition_host_app_put_mesh(
        service->app,
        (MeisoByteView){service->cube_bytes, service->cube_size}, &receipt
    );
    if (status != MEISO_COMPOSITION_HOST_APP_OK) {
        return emit_error(request_id, app_status_code(status));
    }
    service->cube_content_id = receipt.mesh_content_id;
    service->cube_loaded = true;
    return emit_receipt(request_id, &receipt);
}

static bool handle_scene_command(Service *service, const char *line, size_t size)
{
    JsonCursor cursor = {line, size, 0U};
    SceneCommand command;
    MeisoCompositionHostAppCommandReceipt receipt;
    MeisoCompositionHostAppStatus status;
    EditorObjectIdentity old_objects[HOST_EDITOR_OBJECTS];
    uint64_t old_serial;

    if (!parse_scene_command(&cursor, &command)) {
        return emit_error(0U, "INVALID_COMMAND");
    }
    if (command.enabled && !service->cube_loaded) {
        return emit_error(command.request_id, "ASSET_NOT_LOADED");
    }
    memcpy(old_objects, service->objects, sizeof(old_objects));
    old_serial = service->next_object_serial;
    if (command.enabled && !stamp_scene_truth(service, &command)) {
        return emit_error(command.request_id, "CLOCK_FAILURE");
    }
    if (command.enabled && !allocate_scene_ids(service, &command)) {
        return emit_error(command.request_id, "OBJECT_ID_CAPACITY");
    }
    status = meiso_composition_host_app_set_scene(
        service->app, command.enabled,
        command.enabled ? &command.value : NULL, &receipt
    );
    if (status != MEISO_COMPOSITION_HOST_APP_OK) {
        memcpy(service->objects, old_objects, sizeof(old_objects));
        service->next_object_serial = old_serial;
        return emit_error(command.request_id, app_status_code(status));
    }
    return emit_receipt(command.request_id, &receipt);
}

static bool parse_enabled_prefix(
    JsonCursor *cursor,
    const char *command,
    uint64_t *request_id,
    bool *enabled
)
{
    char prefix[96];
    int written;

    written = snprintf(prefix, sizeof(prefix),
                       "{\"command\":\"%s\",\"request_id\":", command);
    return written > 0 && (size_t)written < sizeof(prefix) &&
        json_literal(cursor, prefix) &&
        json_u64(cursor, UINT64_MAX, request_id) &&
        json_literal(cursor, ",\"enabled\":") && json_bool(cursor, enabled);
}

static bool handle_presentation_command(
    Service *service,
    const char *line,
    size_t size
)
{
    JsonCursor cursor = {line, size, 0U};
    MeisoCompositionHostAppPresentationSettings settings;
    MeisoCompositionHostAppCommandReceipt receipt;
    MeisoCompositionHostAppStatus status;
    uint64_t request_id;
    bool enabled;
    bool safety_frame;

    memset(&settings, 0, sizeof(settings));
    if (!parse_enabled_prefix(&cursor, "set_presentation", &request_id,
                              &enabled)) {
        return emit_error(0U, "INVALID_COMMAND");
    }
    if (enabled) {
        if (!json_literal(&cursor, ",\"use_safety_frame\":") ||
            !json_bool(&cursor, &safety_frame) ||
            !json_literal(&cursor, ",\"target_width\":") ||
            !json_u16(&cursor, &settings.target_width) ||
            !json_literal(&cursor, ",\"target_height\":") ||
            !json_u16(&cursor, &settings.target_height) ||
            !json_literal(&cursor, ",\"color_format\":") ||
            !json_color_format(&cursor, &settings.color_format) ||
            !json_literal(&cursor, ",\"depth_format\":\"implementation32\"") ||
            !json_literal(&cursor, ",\"max_result_hz\":") ||
            !json_u16(&cursor, &settings.max_result_hz) ||
            !json_literal(&cursor, ",\"offline_lease_ms\":") ||
            !json_u32(&cursor, &settings.offline_lease_ms)) {
            return emit_error(request_id, "INVALID_COMMAND");
        }
        settings.flags = safety_frame ?
            MEISO_COMPOSITION_SCENE_PLAN_USE_SAFETY_FRAME : 0U;
        settings.depth_format =
            MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32;
    }
    if (!json_literal(&cursor, "}") || !json_end(&cursor)) {
        return emit_error(request_id, "INVALID_COMMAND");
    }
    status = meiso_composition_host_app_set_presentation(
        service->app, enabled, enabled ? &settings : NULL, &receipt
    );
    return status == MEISO_COMPOSITION_HOST_APP_OK ?
        emit_receipt(request_id, &receipt) :
        emit_error(request_id, app_status_code(status));
}

static bool handle_periodic_command(
    Service *service,
    const char *line,
    size_t size,
    bool device
)
{
    JsonCursor cursor = {line, size, 0U};
    uint64_t request_id;
    bool enabled;
    uint32_t selected_fields = 0U;
    uint32_t sample_period_ms = 0U;
    MeisoCompositionHostAppCommandReceipt receipt;
    MeisoCompositionHostAppStatus status;

    if (!parse_enabled_prefix(
            &cursor, device ? "set_device" : "set_wifi", &request_id,
            &enabled
        )) {
        return emit_error(0U, "INVALID_COMMAND");
    }
    if (enabled &&
        (!json_literal(&cursor, ",\"selected_fields\":") ||
         !json_u32(&cursor, &selected_fields) ||
         !json_literal(&cursor, ",\"sample_period_ms\":") ||
         !json_u32(&cursor, &sample_period_ms))) {
        return emit_error(request_id, "INVALID_COMMAND");
    }
    if (!json_literal(&cursor, "}") || !json_end(&cursor)) {
        return emit_error(request_id, "INVALID_COMMAND");
    }
    if (device) {
        MeisoDeviceTelemetryParameters parameters = {
            selected_fields, sample_period_ms
        };
        status = meiso_composition_host_app_set_device(
            service->app, enabled, enabled ? &parameters : NULL, &receipt
        );
    } else {
        MeisoWifiTelemetryDesiredParameters parameters = {
            selected_fields, sample_period_ms
        };
        status = meiso_composition_host_app_set_wifi(
            service->app, enabled, enabled ? &parameters : NULL, &receipt
        );
    }
    return status == MEISO_COMPOSITION_HOST_APP_OK ?
        emit_receipt(request_id, &receipt) :
        emit_error(request_id, app_status_code(status));
}

static bool handle_media_desired_command(
    Service *service,
    const char *line,
    size_t size,
    bool camera
)
{
    JsonCursor cursor = {line, size, 0U};
    MeisoCompositionMediaDesired desired;
    MeisoCompositionHostAppCommandReceipt receipt;
    MeisoCompositionHostAppStatus status;
    uint64_t request_id;
    bool enabled;
    bool request_key;

    memset(&desired, 0, sizeof(desired));
    if (!parse_enabled_prefix(
            &cursor, camera ? "set_camera_media" : "set_render_media",
            &request_id, &enabled
        )) {
        return emit_error(0U, "INVALID_COMMAND");
    }
    if (enabled &&
        (!json_literal(&cursor, ",\"pixel_format\":") ||
         !json_pixel_format(&cursor, &desired.pixel_format) ||
         !json_literal(&cursor, ",\"codec\":\"raw\"") ||
         !json_literal(&cursor, ",\"width\":") ||
         !json_u16(&cursor, &desired.width) ||
         !json_literal(&cursor, ",\"height\":") ||
         !json_u16(&cursor, &desired.height) ||
         !json_literal(&cursor, ",\"fps_millihz\":") ||
         !json_u32(&cursor, &desired.fps_millihz) ||
         !json_literal(&cursor, ",\"bitrate_bps\":") ||
         !json_u32(&cursor, &desired.bitrate_bps) ||
         !json_literal(&cursor, ",\"gop_frames\":") ||
         !json_u32(&cursor, &desired.gop_frames) ||
         !json_literal(&cursor, ",\"request_key\":") ||
         !json_bool(&cursor, &request_key))) {
        return emit_error(request_id, "INVALID_COMMAND");
    }
    if (enabled) {
        desired.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
        desired.recovery_flags = request_key ?
            MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY : 0U;
    }
    if (!json_literal(&cursor, "}") || !json_end(&cursor)) {
        return emit_error(request_id, "INVALID_COMMAND");
    }
    status = camera ? meiso_composition_host_app_set_camera_media(
                          service->app, enabled, enabled ? &desired : NULL,
                          &receipt
                      ) :
                      meiso_composition_host_app_set_render_media(
                          service->app, enabled, enabled ? &desired : NULL,
                          &receipt
                      );
    return status == MEISO_COMPOSITION_HOST_APP_OK ?
        emit_receipt(request_id, &receipt) :
        emit_error(request_id, app_status_code(status));
}

static bool emit_snapshot(Service *service, uint64_t request_id)
{
    MeisoCompositionHostAppSnapshot snapshot;
    char line[2048];
    int written;

    if (!meiso_composition_host_app_snapshot(service->app, &snapshot)) {
        return emit_error(request_id, "SNAPSHOT_FAILED");
    }
    written = snprintf(
        line, sizeof(line),
        "{\"type\":\"snapshot\",\"request_id\":%" PRIu64
        ",\"phase\":\"%s\",\"terminal_reason\":\"%s\","
        "\"control_ready\":%s,"
        "\"catalog_generation\":%" PRIu64 ",\"media_bound\":%s,"
        "\"media_mask\":\"%s\",\"media_slots_consumed\":%u,"
        "\"mesh_active\":%s,\"mesh_next_offset\":%" PRIu64 ","
        "\"wifi_actual\":%s,\"presentation_actual\":%s,"
        "\"scene_actual\":%s,\"device_actual\":%s,"
        "\"render_actual\":%s,\"camera_actual\":%s,"
        "\"wifi_sample\":%s,\"render_result\":%s,"
        "\"device_sample\":%s,\"render_frame\":%s,"
        "\"camera_frame\":%s}",
        request_id, phase_name(snapshot.phase),
        terminal_reason_name(snapshot.terminal_reason),
        snapshot.control_ready ? "true" : "false",
        snapshot.active_catalog_generation,
        snapshot.media_bound ? "true" : "false",
        media_mask_name(snapshot.media_active_route_mask),
        (unsigned)snapshot.media_slots_consumed,
        snapshot.mesh.transfer_active ? "true" : "false",
        snapshot.mesh.next_offset,
        snapshot.actual[MEISO_COMPOSITION_HOST_APP_ACTUAL_WIFI].present ?
            "true" : "false",
        snapshot.actual[MEISO_COMPOSITION_HOST_APP_ACTUAL_PRESENTATION].present ?
            "true" : "false",
        snapshot.actual[MEISO_COMPOSITION_HOST_APP_ACTUAL_SCENE].present ?
            "true" : "false",
        snapshot.actual[MEISO_COMPOSITION_HOST_APP_ACTUAL_DEVICE].present ?
            "true" : "false",
        snapshot.actual[MEISO_COMPOSITION_HOST_APP_ACTUAL_RENDER_MEDIA].present ?
            "true" : "false",
        snapshot.actual[MEISO_COMPOSITION_HOST_APP_ACTUAL_CAMERA_MEDIA].present ?
            "true" : "false",
        snapshot.wifi_sample_present ? "true" : "false",
        snapshot.render_result_present ? "true" : "false",
        snapshot.device_sample_present ? "true" : "false",
        snapshot.media[0].latest_present ? "true" : "false",
        snapshot.media[1].latest_present ? "true" : "false"
    );
    return written > 0 && (size_t)written < sizeof(line) && emit_line(line);
}

static bool parse_request_only(
    const char *line,
    size_t size,
    const char *command,
    uint64_t *out_request_id
)
{
    JsonCursor cursor = {line, size, 0U};
    char prefix[96];
    int written;

    written = snprintf(prefix, sizeof(prefix),
                       "{\"command\":\"%s\",\"request_id\":", command);
    return written > 0 && (size_t)written < sizeof(prefix) &&
        json_literal(&cursor, prefix) &&
        json_u64(&cursor, UINT64_MAX, out_request_id) &&
        json_literal(&cursor, "}") && json_end(&cursor);
}

static bool line_contains_literal(
    const char *line,
    size_t line_size,
    const char *literal
)
{
    size_t literal_size;
    size_t offset;

    if (line == NULL || literal == NULL) {
        return false;
    }
    literal_size = strlen(literal);
    if (literal_size > line_size) {
        return false;
    }
    for (offset = 0U; offset <= line_size - literal_size; ++offset) {
        if (memcmp(line + offset, literal, literal_size) == 0) {
            return true;
        }
    }
    return false;
}

static bool handle_command(Service *service, const char *line, size_t size)
{
    uint64_t request_id;

    #define LINE_HAS(literal) line_contains_literal(line, size, literal)

    if (size >= HOST_CONTROL_LINE_BYTES || memchr(line, '\0', size) != NULL) {
        return emit_error(0U, "INVALID_COMMAND");
    }
    if (LINE_HAS("\"command\":\"asset.load_builtin\"")) {
        return handle_asset_command(service, line, size);
    }
    if (LINE_HAS("\"command\":\"set_scene\"")) {
        return handle_scene_command(service, line, size);
    }
    if (LINE_HAS("\"command\":\"set_presentation\"")) {
        return handle_presentation_command(service, line, size);
    }
    if (LINE_HAS("\"command\":\"set_wifi\"")) {
        return handle_periodic_command(service, line, size, false);
    }
    if (LINE_HAS("\"command\":\"set_device\"")) {
        return handle_periodic_command(service, line, size, true);
    }
    if (LINE_HAS("\"command\":\"set_render_media\"")) {
        return handle_media_desired_command(service, line, size, false);
    }
    if (LINE_HAS("\"command\":\"set_camera_media\"")) {
        return handle_media_desired_command(service, line, size, true);
    }
    if (parse_request_only(line, size, "snapshot", &request_id)) {
        return emit_snapshot(service, request_id) && emit_typed_truth(service);
    }
    if (parse_request_only(line, size, "close", &request_id)) {
        char output[160];
        int written;
        service->close_requested = true;
        written = snprintf(
            output, sizeof(output),
            "{\"type\":\"closing\",\"request_id\":%" PRIu64 "}",
            request_id
        );
        return written > 0 && (size_t)written < sizeof(output) &&
            emit_line(output);
    }
    #undef LINE_HAS
    return emit_error(0U, "INVALID_COMMAND");
}

static void write_be_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void write_be_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void write_be_u64(uint8_t *output, uint64_t value)
{
    write_be_u32(output, (uint32_t)(value >> 32U));
    write_be_u32(output + 4U, (uint32_t)value);
}

static bool queue_frame_output(
    Service *service,
    MeisoCompositionMediaSource source,
    bool *out_emitted
)
{
    MeisoCompositionMediaLatestView latest;
    MeisoCompositionHostAppStatus status;
    size_t record_size;
    char metadata[512];
    int written;

    *out_emitted = false;
    if (service->frame_output.pending) {
        return true;
    }
    status = meiso_composition_host_app_media_latest(
        service->app, source, &latest
    );
    if (status == MEISO_COMPOSITION_HOST_APP_NO_FRAME) {
        return true;
    }
    if (status != MEISO_COMPOSITION_HOST_APP_OK ||
        (source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW &&
         latest.unit.access_unit.size > service->config.catalog
             .render_media_capability.max_access_unit_bytes) ||
        (source == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA &&
         latest.unit.access_unit.size > service->config.catalog
             .camera_media_capability.max_access_unit_bytes) ||
        latest.unit.access_unit.size > HOST_FRAME_BYTES ||
        !checked_add_size(HOST_CMFB_HEADER_BYTES,
                          latest.unit.access_unit.size, &record_size)) {
        return false;
    }
    memset(service->frame_output.bytes, 0, HOST_CMFB_HEADER_BYTES);
    memcpy(service->frame_output.bytes, "CMFB", 4U);
    write_be_u16(service->frame_output.bytes + 4U, 1U);
    write_be_u16(service->frame_output.bytes + 6U, HOST_CMFB_HEADER_BYTES);
    service->frame_output.bytes[8] = source;
    write_be_u16(service->frame_output.bytes + 10U, latest.unit.pixel_format);
    write_be_u16(service->frame_output.bytes + 12U, latest.unit.codec);
    write_be_u16(service->frame_output.bytes + 14U, latest.unit.flags);
    write_be_u16(service->frame_output.bytes + 16U, latest.unit.width);
    write_be_u16(service->frame_output.bytes + 18U, latest.unit.height);
    write_be_u32(service->frame_output.bytes + 20U,
                 (uint32_t)latest.unit.access_unit.size);
    write_be_u64(service->frame_output.bytes + 24U,
                 latest.unit.frame_sequence);
    write_be_u64(service->frame_output.bytes + 32U,
                 latest.unit.source_time.clock_epoch);
    write_be_u64(service->frame_output.bytes + 40U,
                 latest.unit.source_time.monotonic_ns);
    write_be_u64(service->frame_output.bytes + 48U,
                 latest.unit.active_plan_generation);
    write_be_u64(service->frame_output.bytes + 56U,
                 latest.unit.world_generation);
    write_be_u64(service->frame_output.bytes + 64U,
                 latest.unit.applied_scene_state_version);
    memcpy(service->frame_output.bytes + HOST_CMFB_HEADER_BYTES,
           latest.unit.access_unit.data, latest.unit.access_unit.size);
    service->frame_output.size = record_size;
    service->frame_output.offset = 0U;
    service->frame_output.ref = latest.ref;
    service->frame_output.pending = service->frame_fd >= 0;
    status = meiso_composition_host_app_media_release(
        service->app, &latest.ref
    );
    if (status != MEISO_COMPOSITION_HOST_APP_OK) {
        memset(&service->frame_output, 0, sizeof(service->frame_output));
        return false;
    }
    written = snprintf(
        metadata, sizeof(metadata),
        "{\"type\":\"media.frame\",\"source\":\"%s\","
        "\"sequence\":%" PRIu64 ",\"clock_epoch\":%" PRIu64
        ",\"monotonic_ns\":%" PRIu64 ",\"pixel_format\":\"%s\","
        "\"codec\":\"%s\",\"width\":%u,\"height\":%u,"
        "\"access_unit_bytes\":%zu,\"frame_fd_queued\":%s}",
        media_source_name(source), latest.unit.frame_sequence,
        latest.unit.source_time.clock_epoch,
        latest.unit.source_time.monotonic_ns,
        pixel_format_name(latest.unit.pixel_format),
        codec_name(latest.unit.codec),
        (unsigned)latest.unit.width, (unsigned)latest.unit.height,
        latest.unit.access_unit.size,
        service->frame_fd >= 0 ? "true" : "false"
    );
    if (written <= 0 || (size_t)written >= sizeof(metadata) ||
        !emit_line(metadata)) {
        return false;
    }
    *out_emitted = true;
    return true;
}

static bool write_frame_output(Service *service, bool writable)
{
    ssize_t result;

    if (!service->frame_output.pending || !writable) {
        return true;
    }
    result = write(
        service->frame_fd,
        service->frame_output.bytes + service->frame_output.offset,
        service->frame_output.size - service->frame_output.offset
    );
    if (result > 0) {
        service->frame_output.offset += (size_t)result;
        if (service->frame_output.offset == service->frame_output.size) {
            service->frame_drain_cursor =
                service->frame_output.bytes[8] ==
                    MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW ? 1U : 0U;
            memset(&service->frame_output, 0, sizeof(service->frame_output));
        }
        return true;
    }
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                       errno == EINTR)) {
        return true;
    }
    return false;
}

static MeisoCompositionHostAppMediaRouteMask current_media_mask(
    const MeisoCompositionHostAppSnapshot *snapshot
)
{
    MeisoCompositionHostAppMediaRouteMask mask = 0U;

    if (snapshot->media[0].actual_binding_ready) {
        mask |= MEISO_COMPOSITION_HOST_APP_MEDIA_RENDER;
    }
    if (snapshot->media[1].actual_binding_ready) {
        mask |= MEISO_COMPOSITION_HOST_APP_MEDIA_CAMERA;
    }
    return mask;
}

static bool reconcile_media(Service *service)
{
    MeisoCompositionHostAppSnapshot snapshot;
    MeisoCompositionHostAppMediaRouteMask mask;
    MeisoCompositionHostAppStatus status;
    int fd;
    bool connected;

    if (!meiso_composition_host_app_snapshot(service->app, &snapshot)) {
        return false;
    }
    if (service->media_fd >= 0 && !service->media_connecting &&
        !snapshot.media_bound) {
        close_fd(&service->media_fd);
    }
    mask = current_media_mask(&snapshot);
    if (service->media_connecting) {
        if (snapshot.phase != MEISO_COMPOSITION_HOST_APP_PHASE_READY ||
            mask == 0U || mask != service->media_connect_mask ||
            time_reached(&service->media_connect_deadline)) {
            close_fd(&service->media_fd);
            service->media_connecting = false;
            service->media_connect_mask = 0U;
            media_retry_later(service);
        }
        return true;
    }
    if (service->media_fd >= 0 || snapshot.media_bound ||
        snapshot.phase != MEISO_COMPOSITION_HOST_APP_PHASE_READY) {
        return true;
    }
    if (mask == 0U ||
        snapshot.media_slots_consumed >= snapshot.media_slots_capacity ||
        !time_reached(&service->media_retry_after)) {
        return true;
    }
    fd = start_numeric_connect(&service->config.media_address,
                               service->config.media_address_size,
                               &connected);
    if (fd < 0) {
        media_retry_later(service);
        return true;
    }
    if (!connected) {
        service->media_fd = fd;
        service->media_connecting = true;
        service->media_connect_mask = mask;
        if (!deadline_after_ms(HOST_CONNECT_TIMEOUT_MS,
                               &service->media_connect_deadline)) {
            close_fd(&service->media_fd);
            service->media_connecting = false;
            service->media_connect_mask = 0U;
            media_retry_later(service);
        }
        return true;
    }
    status = meiso_composition_host_app_bind_media_fd(service->app, fd,
                                                       mask);
    if (status != MEISO_COMPOSITION_HOST_APP_OK) {
        close_fd(&fd);
        media_retry_later(service);
        return status == MEISO_COMPOSITION_HOST_APP_NOT_READY ||
            status == MEISO_COMPOSITION_HOST_APP_INVALID_STATE ||
            status == MEISO_COMPOSITION_HOST_APP_BUSY;
    }
    service->media_fd = fd;
    return emit_line(
        "{\"type\":\"media.connected\",\"transport\":\"tcp\"}"
    );
}

static bool finish_media_connect(Service *service)
{
    MeisoCompositionHostAppStatus status;
    int error_number = 0;
    socklen_t error_size = (socklen_t)sizeof(error_number);

    if (!service->media_connecting || service->media_fd < 0) {
        return false;
    }
    if (getsockopt(service->media_fd, SOL_SOCKET, SO_ERROR, &error_number,
                   &error_size) != 0 || error_number != 0) {
        close_fd(&service->media_fd);
        service->media_connecting = false;
        service->media_connect_mask = 0U;
        media_retry_later(service);
        return true;
    }
    status = meiso_composition_host_app_bind_media_fd(
        service->app, service->media_fd, service->media_connect_mask
    );
    if (status != MEISO_COMPOSITION_HOST_APP_OK) {
        close_fd(&service->media_fd);
        service->media_connecting = false;
        service->media_connect_mask = 0U;
        media_retry_later(service);
        return status == MEISO_COMPOSITION_HOST_APP_NOT_READY ||
            status == MEISO_COMPOSITION_HOST_APP_INVALID_STATE ||
            status == MEISO_COMPOSITION_HOST_APP_BUSY;
    }
    service->media_connecting = false;
    service->media_connect_mask = 0U;
    memset(&service->media_connect_deadline, 0,
           sizeof(service->media_connect_deadline));
    return emit_line(
        "{\"type\":\"media.connected\",\"transport\":\"tcp\"}"
    );
}

static bool emit_pump_result(
    const MeisoCompositionHostAppPumpResult *result
)
{
    char line[384];
    int written;

    written = snprintf(
        line, sizeof(line),
        "{\"type\":\"pump.event\",\"event\":\"%s\","
        "\"command_id\":%" PRIu64 ",\"desired_channel\":\"%s\","
        "\"actual_channel\":\"%s\",\"inline_channel\":\"%s\","
        "\"media_source\":\"%s\"}",
        pump_event_name(result->event),
        result->command_present ? result->command_id : 0U,
        result->desired_channel_present ?
            desired_channel_name(result->desired_channel) : "none",
        result->actual_channel_present ?
            actual_channel_name(result->actual_channel) : "none",
        result->inline_channel_present ?
            inline_channel_name(result->inline_channel) : "none",
        result->media_source_present ?
            media_source_name(result->media_source) : "none"
    );
    return written > 0 && (size_t)written < sizeof(line) && emit_line(line);
}

static bool emit_typed_truth(Service *service)
{
    MeisoCompositionHostAppSnapshot snapshot;
    char line[1024];
    size_t index;
    int written;

    if (!meiso_composition_host_app_snapshot(service->app, &snapshot)) {
        return false;
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_ACTUAL_COUNT;
         ++index) {
        const MeisoCompositionHostAppActualSnapshot *actual =
            &snapshot.actual[index];
        if (!actual->present) {
            continue;
        }
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"actual\",\"channel\":\"%s\","
            "\"state_version\":%" PRIu64 ",\"apply_result\":\"%s\","
            "\"availability\":\"%s\",\"profile_decoded\":%s,"
            "\"enabled\":%s,"
            "\"production_status\":\"%s\",\"reason\":\"%s\","
            "\"production_epoch\":%" PRIu64 "}",
            actual_channel_name((MeisoCompositionHostAppActualChannel)index),
            actual->state_version, apply_result_name(actual->apply_result),
            availability_name(actual->availability),
            actual->profile_decoded ? "true" : "false",
            actual->effective_enabled ? "true" : "false",
            production_status_name(actual->production_status),
            production_reason_name(actual->reason),
            actual->production_epoch
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !emit_line(line)) {
            return false;
        }
    }
    if (snapshot.wifi_effective_present) {
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"telemetry.wifi_effective\","
            "\"selected_fields\":%u,\"sample_period_ms\":%u}",
            snapshot.wifi_effective.selected_fields,
            snapshot.wifi_effective.sample_period_ms
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !emit_line(line)) {
            return false;
        }
    }
    if (snapshot.device_effective_present) {
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"telemetry.device_effective\","
            "\"selected_fields\":%u,\"sample_period_ms\":%u}",
            snapshot.device_effective.selected_fields,
            snapshot.device_effective.sample_period_ms
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !emit_line(line)) {
            return false;
        }
    }
    if (snapshot.presentation_status_present) {
        const MeisoCompositionScenePresentationStatus *presentation =
            &snapshot.presentation_status;
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"presentation.status\","
            "\"candidate_state\":\"%s\","
            "\"candidate_plan_generation\":%" PRIu64 ","
            "\"active_state\":\"%s\","
            "\"active_plan_generation\":%" PRIu64 ","
            "\"reason\":\"%s\",\"target_width\":%u,"
            "\"target_height\":%u,\"color_format\":\"%s\","
            "\"depth_format\":\"%s\"}",
            candidate_state_name(presentation->candidate_state),
            presentation->candidate_plan_generation,
            active_state_name(presentation->active_state),
            presentation->active_plan_generation,
            presentation_reason_name(presentation->reason),
            (unsigned)presentation->target_width,
            (unsigned)presentation->target_height,
            scene_color_format_name(presentation->color_format),
            scene_depth_format_name(presentation->depth_format)
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !emit_line(line)) {
            return false;
        }
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_APP_MEDIA_ROUTE_COUNT;
         ++index) {
        const MeisoCompositionMediaActual *media = index == 0U ?
            &snapshot.render_media_actual : &snapshot.camera_media_actual;
        bool present = index == 0U ? snapshot.render_media_actual_present :
            snapshot.camera_media_actual_present;
        if (!present) {
            continue;
        }
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"media.actual\",\"source\":\"%s\","
            "\"pixel_format\":\"%s\",\"codec\":\"%s\","
            "\"width\":%u,\"height\":%u,\"fps_millihz\":%u,"
            "\"bitrate_bps\":%u,\"gop_frames\":%u,"
            "\"request_key\":%s,\"media_stream_epoch\":%" PRIu64
            ",\"producer_dropped_frames\":%" PRIu64 ","
            "\"queue_depth\":%u,\"recovery_generation\":%u}",
            media_source_name(media->source),
            pixel_format_name(media->effective.pixel_format),
            codec_name(media->effective.codec),
            (unsigned)media->effective.width,
            (unsigned)media->effective.height,
            media->effective.fps_millihz, media->effective.bitrate_bps,
            media->effective.gop_frames,
            (media->effective.recovery_flags &
             MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY) != 0U ?
                "true" : "false",
            media->media_stream_epoch, media->producer_dropped_frames,
            media->queue_depth, media->recovery_generation
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !emit_line(line)) {
            return false;
        }
    }
    if (snapshot.wifi_sample_present) {
        const MeisoWifiTelemetrySample *sample = &snapshot.wifi_sample;
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"telemetry.wifi\",\"selected_fields\":%u,"
            "\"present_fields\":%u,\"link_state\":%s,"
            "\"signal_mbm\":%" PRId32 ",\"rx_bytes\":%" PRIu64
            ",\"tx_bytes\":%" PRIu64 ",\"rx_packets\":%" PRIu64
            ",\"tx_packets\":%" PRIu64 ",\"rx_dropped\":%" PRIu64
            ",\"tx_dropped\":%" PRIu64 ",\"rx_errors\":%" PRIu64
            ",\"tx_errors\":%" PRIu64 "}",
            sample->selected_fields, sample->present_fields,
            sample->link_state ? "true" : "false", sample->signal_mbm,
            sample->rx_bytes, sample->tx_bytes, sample->rx_packets,
            sample->tx_packets, sample->rx_dropped, sample->tx_dropped,
            sample->rx_errors, sample->tx_errors
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !emit_line(line)) {
            return false;
        }
    }
    if (snapshot.device_sample_present) {
        const MeisoDeviceTelemetrySample *sample = &snapshot.device_sample;
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"telemetry.device\","
            "\"selected_fields\":%u,\"present_fields\":%u,"
            "\"temperature_millic\":%" PRId32 ","
            "\"temperature_uncertainty_millic\":%u,"
            "\"cpu_load_ppm\":%u,\"memory_total_bytes\":%" PRIu64
            ",\"memory_available_bytes\":%" PRIu64 "}",
            sample->selected_fields, sample->present_fields,
            sample->temperature_millic,
            sample->temperature_uncertainty_millic,
            sample->cpu_load_ppm, sample->memory_total_bytes,
            sample->memory_available_bytes
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !emit_line(line)) {
            return false;
        }
    }
    if (snapshot.scene_status_present) {
        const MeisoCompositionSceneStatus *scene = &snapshot.scene_status;
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"scene.status\",\"reason\":\"%s\","
            "\"active_plan_generation\":%" PRIu64 ","
            "\"world_generation\":%" PRIu64 ","
            "\"object_count\":%u,\"unique_mesh_count\":%u,"
            "\"resident_mesh_bytes\":%" PRIu64 ","
            "\"render_jobs_in_flight\":%u,\"result_queue_depth\":%u}",
            scene_reason_name(scene->reason), scene->active_plan_generation,
            scene->world_generation, scene->active_object_count,
            scene->unique_mesh_count, scene->resident_mesh_bytes,
            scene->render_jobs_in_flight, scene->result_queue_depth
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !emit_line(line)) {
            return false;
        }
    }
    if (snapshot.render_result_present) {
        const MeisoCompositionSceneRenderResult *render =
            &snapshot.render_result;
        char digest[65];
        sha256_hex(render->full_target_sha256, digest);
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"render.result\","
            "\"result\":\"complete\","
            "\"active_plan_generation\":%" PRIu64 ","
            "\"world_generation\":%" PRIu64 ","
            "\"object_count\":%u,\"unique_mesh_count\":%u,"
            "\"target_width\":%u,\"target_height\":%u,"
            "\"color_format\":\"%s\",\"depth_format\":\"%s\","
            "\"source_clock_epoch\":%" PRIu64 ","
            "\"render_start_monotonic_ns\":%" PRIu64 ","
            "\"render_end_monotonic_ns\":%" PRIu64 ","
            "\"offscreen_complete\":%s,\"thumbnail_included\":%s,"
            "\"fence_complete\":%s,\"display_commit_accepted\":%s,"
            "\"vblank_observed\":%s,\"physical_verified\":%s,"
            "\"full_target_sha256\":\"%s\"}",
            render->active_plan_generation, render->world_generation,
            render->object_count, render->unique_mesh_count,
            (unsigned)render->target_width, (unsigned)render->target_height,
            scene_color_format_name(render->color_format),
            scene_depth_format_name(render->depth_format),
            render->source_clock_epoch, render->render_start_monotonic_ns,
            render->render_end_monotonic_ns,
            (render->evidence_flags &
             MEISO_COMPOSITION_SCENE_EVIDENCE_OFFSCREEN_COMPLETE) != 0U ?
                "true" : "false",
            (render->evidence_flags &
             MEISO_COMPOSITION_SCENE_EVIDENCE_THUMBNAIL_INCLUDED) != 0U ?
                "true" : "false",
            (render->evidence_flags &
             MEISO_COMPOSITION_SCENE_EVIDENCE_FENCE_COMPLETE) != 0U ?
                "true" : "false",
            (render->evidence_flags &
             MEISO_COMPOSITION_SCENE_EVIDENCE_DISPLAY_COMMIT_ACCEPTED) != 0U ?
                "true" : "false",
            (render->evidence_flags &
             MEISO_COMPOSITION_SCENE_EVIDENCE_VBLANK_OBSERVED) != 0U ?
                "true" : "false",
            (render->evidence_flags &
             MEISO_COMPOSITION_SCENE_EVIDENCE_PHYSICAL_VERIFIED) != 0U ?
                "true" : "false",
            digest
        );
        if (written <= 0 || (size_t)written >= sizeof(line) ||
            !emit_line(line)) {
            return false;
        }
    }
    return true;
}

static bool pump_host(
    Service *service,
    const MeisoCompositionHostAppIoReady *ready,
    bool *out_progress
)
{
    MeisoCompositionHostAppPumpResult result;
    MeisoCompositionHostAppPumpStatus status;

    *out_progress = false;
    status = meiso_composition_host_app_pump(service->app, ready, &result);
    if (status == MEISO_COMPOSITION_HOST_APP_PUMP_IDLE ||
        status == MEISO_COMPOSITION_HOST_APP_PUMP_WOULD_BLOCK) {
        return true;
    }
    if (status == MEISO_COMPOSITION_HOST_APP_PUMP_TERMINAL) {
        service->close_requested = true;
        return emit_line(
            "{\"type\":\"terminal\",\"code\":\"HOST_APP_TERMINAL\"}"
        );
    }
    if (status != MEISO_COMPOSITION_HOST_APP_PUMP_PROGRESS) {
        return false;
    }
    *out_progress = true;
    if (result.event_present && !emit_pump_result(&result)) {
        return false;
    }
    if (result.event_present &&
        (result.event == MEISO_COMPOSITION_HOST_APP_PUMP_ACTUAL_UPDATED ||
         result.event == MEISO_COMPOSITION_HOST_APP_PUMP_INLINE_UPDATED ||
         result.event == MEISO_COMPOSITION_HOST_APP_PUMP_BOUNDARY)) {
        return emit_typed_truth(service);
    }
    return true;
}

static bool process_buffered_line(Service *service, bool *out_processed)
{
    char *newline;
    size_t line_size;
    size_t remaining;

    *out_processed = false;
    newline = memchr(service->input, '\n', service->input_size);
    if (newline == NULL) {
        return true;
    }
    line_size = (size_t)(newline - service->input);
    if (line_size > 0U && service->input[line_size - 1U] == '\r') {
        --line_size;
    }
    if (!handle_command(service, service->input, line_size)) {
        return false;
    }
    remaining = service->input_size - ((size_t)(newline - service->input) + 1U);
    memmove(service->input, newline + 1, remaining);
    service->input_size = remaining;
    *out_processed = true;
    return true;
}

static bool read_control_input(Service *service, bool readable)
{
    ssize_t result;

    if (!readable) {
        return true;
    }
    if (service->input_size == sizeof(service->input)) {
        return false;
    }
    result = read(STDIN_FILENO, service->input + service->input_size,
                  sizeof(service->input) - service->input_size);
    if (result > 0) {
        service->input_size += (size_t)result;
        return true;
    }
    if (result == 0) {
        service->stdin_eof = true;
        return true;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static bool frame_fd_valid(int fd)
{
    int flags;
    int access_mode;

    if (fd < 0) {
        return true;
    }
    flags = fcntl(fd, F_GETFL, 0);
    access_mode = flags < 0 ? O_RDONLY : flags & O_ACCMODE;
    return flags >= 0 && (flags & O_NONBLOCK) != 0 && fd != STDIN_FILENO &&
        fd != STDOUT_FILENO && fd != STDERR_FILENO &&
        access_mode != O_RDONLY;
}

static bool initialize_service(
    Service *service,
    const ServiceConfig *config,
    int frame_fd
)
{
    MeisoCompositionHostAppConfig app_config;
    MeisoCompositionHostAppInitStatus init_status;
    size_t alignment;
    void *arena = NULL;

    if (service == NULL || config == NULL || !frame_fd_valid(frame_fd)) {
        return false;
    }
    memset(service, 0, sizeof(*service));
    service->control_fd = -1;
    service->media_fd = -1;
    service->frame_fd = frame_fd;
    service->config = *config;
    service->next_object_serial = 0U;
    service->host_clock_epoch = mix_nonzero_epoch(
        config->nonce.high ^ config->nonce.low ^ config->local_incarnation
    );
    service->scene_space_epoch = mix_nonzero_epoch(
        service->host_clock_epoch ^ UINT64_C(0x5343454e45535043)
    );
    service->control_fd = connect_numeric(
        &config->control_address, config->control_address_size
    );
    if (service->control_fd < 0 || service->control_fd == frame_fd) {
        close_fd(&service->control_fd);
        return false;
    }
    memset(&app_config, 0, sizeof(app_config));
    app_config.control_connected_fd = service->control_fd;
    app_config.local_peer_id = config->local_peer_id;
    app_config.local_incarnation = config->local_incarnation;
    app_config.local_nonce = config->nonce;
    app_config.expected_edge_peer_id = config->edge_peer_id;
    app_config.protocol = (MeisoProtocolVersion){1U, 0U};
    app_config.catalog_generation = config->catalog_generation;
    app_config.catalog = config->catalog;
    app_config.max_control_io_bytes_per_step = HOST_IO_STEP_BYTES;
    app_config.control_adapter_reserve_bytes = HOST_CONTROL_ADAPTER_BYTES;
    app_config.control_runtime_reserve_bytes = HOST_CONTROL_RUNTIME_BYTES;
    app_config.control_scheduler_reserve_bytes = HOST_CONTROL_SCHEDULER_BYTES;
    app_config.initial_desired_delivery_generation =
        (MeisoDeliveryGeneration){101U};
    app_config.media_connection_slots = HOST_MEDIA_CONNECTION_SLOTS;
    app_config.max_media_io_bytes_per_step = HOST_IO_STEP_BYTES;
    service->app_arena_size =
        meiso_composition_host_app_memory_required(&app_config);
    alignment = meiso_composition_host_app_alignment_required();
    if (service->app_arena_size == 0U || alignment == 0U ||
        posix_memalign(&arena, alignment, service->app_arena_size) != 0) {
        close_fd(&service->control_fd);
        return false;
    }
    memset(arena, 0, service->app_arena_size);
    init_status = meiso_composition_host_app_init(
        arena, service->app_arena_size, &app_config, &service->app
    );
    if (init_status != MEISO_COMPOSITION_HOST_APP_INIT_OK) {
        free(arena);
        service->app = NULL;
        close_fd(&service->control_fd);
        return false;
    }
    service->app_arena = arena;
    return true;
}

static void destroy_service(Service *service)
{
    if (service == NULL) {
        return;
    }
    if (service->app != NULL) {
        (void)meiso_composition_host_app_abort(
            service->app, MEISO_COMPOSITION_HOST_APP_TERMINAL_LOCAL_ABORT, 0
        );
    }
    close_fd(&service->media_fd);
    close_fd(&service->control_fd);
    free(service->app_arena);
    service->app_arena = NULL;
    service->app = NULL;
}

static bool drain_latest_frames(Service *service)
{
    MeisoCompositionHostAppSnapshot snapshot;
    uint8_t scan;

    if (service->frame_output.pending) {
        return true;
    }
    if (!meiso_composition_host_app_snapshot(service->app, &snapshot)) {
        return false;
    }
    if (!snapshot.media_bound || snapshot.media_active_route_mask == 0U) {
        return true;
    }
    if ((snapshot.media_active_route_mask &
         (uint8_t)(UINT8_C(1) << service->frame_drain_cursor)) == 0U) {
        service->frame_drain_cursor =
            (snapshot.media_active_route_mask &
             MEISO_COMPOSITION_HOST_APP_MEDIA_RENDER) != 0U ? 0U : 1U;
    }
    for (scan = 0U; scan < MEISO_COMPOSITION_HOST_APP_MEDIA_ROUTE_COUNT;
         ++scan) {
        uint8_t route = (uint8_t)((service->frame_drain_cursor + scan) %
            MEISO_COMPOSITION_HOST_APP_MEDIA_ROUTE_COUNT);
        MeisoCompositionHostAppMediaRouteMask route_mask =
            (MeisoCompositionHostAppMediaRouteMask)(UINT8_C(1) << route);
        MeisoCompositionMediaSource source = route == 0U ?
            MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW :
            MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
        bool emitted;

        if ((snapshot.media_active_route_mask & route_mask) == 0U) {
            continue;
        }
        if (!queue_frame_output(service, source, &emitted)) {
            return false;
        }
        if (emitted) {
            if (!service->frame_output.pending) {
                service->frame_drain_cursor = (uint8_t)(route ^ UINT8_C(1));
            }
            return true;
        }
    }
    return true;
}

static bool run_service(Service *service)
{
    bool progress;

    if (!emit_line(
            "{\"type\":\"service.ready\",\"profile\":\"composition-v3\","
            "\"wire_owner\":\"native_service\",\"asset_import\":\"builtin_cube_only\","
            "\"frame_projection\":\"CMFB/v1\"}"
        )) {
        return false;
    }
    while (!service->close_requested) {
        MeisoCompositionHostAppIoInterest interest;
        MeisoCompositionHostAppIoReady ready = {false, false, false, false};
        struct pollfd descriptors[4];
        nfds_t descriptor_count = 0U;
        int stdin_index = -1;
        int control_index = -1;
        int media_index = -1;
        int frame_index = -1;
        int poll_result;
        bool processed;
        uint8_t scan;
        bool virtual_action = false;

        for (scan = 0U; scan < 3U; ++scan) {
            uint8_t domain = (uint8_t)((service->loop_cursor + scan) % 3U);
            if (domain == 0U) {
                if (!process_buffered_line(service, &processed)) {
                    return false;
                }
                if (processed) {
                    service->loop_cursor = 1U;
                    virtual_action = true;
                    break;
                }
            } else if (domain == 1U) {
                if (!pump_host(
                        service,
                        &(MeisoCompositionHostAppIoReady){
                            false, false, false, false
                        },
                        &progress
                    )) {
                    return false;
                }
                if (progress) {
                    service->loop_cursor = 2U;
                    virtual_action = true;
                    break;
                }
            }
        }
        if (service->close_requested) {
            break;
        }
        if (virtual_action) {
            continue;
        }
        if (service->stdin_eof) {
            if (service->input_size != 0U) {
                (void)emit_error(0U, "INVALID_COMMAND");
                return false;
            }
            service->close_requested = true;
            break;
        }
        if (!reconcile_media(service) || !drain_latest_frames(service) ||
            !meiso_composition_host_app_readiness(service->app, &interest)) {
            return false;
        }

        if (!service->stdin_eof) {
            stdin_index = (int)descriptor_count;
            descriptors[descriptor_count++] =
                (struct pollfd){STDIN_FILENO, POLLIN, 0};
        }
        if (interest.control_read || interest.control_write) {
            control_index = (int)descriptor_count;
            descriptors[descriptor_count++] = (struct pollfd){
                service->control_fd,
                (short)((interest.control_read ? POLLIN : 0) |
                        (interest.control_write ? POLLOUT : 0)), 0
            };
        }
        if (service->media_fd >= 0 &&
            (service->media_connecting || interest.media_read ||
             interest.media_write)) {
            media_index = (int)descriptor_count;
            descriptors[descriptor_count++] = (struct pollfd){
                service->media_fd,
                service->media_connecting ? POLLOUT :
                    (short)((interest.media_read ? POLLIN : 0) |
                            (interest.media_write ? POLLOUT : 0)), 0
            };
        }
        if (service->frame_fd >= 0 && service->frame_output.pending) {
            frame_index = (int)descriptor_count;
            descriptors[descriptor_count++] =
                (struct pollfd){service->frame_fd, POLLOUT, 0};
        }
        poll_result = poll(descriptors, descriptor_count, HOST_POLL_TIMEOUT_MS);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (poll_result == 0) {
            continue;
        }
        if (stdin_index >= 0) {
            short events = descriptors[stdin_index].revents;
            if ((events & (POLLERR | POLLNVAL)) != 0) {
                return false;
            }
        }
        if (control_index >= 0) {
            short events = descriptors[control_index].revents;
            if ((events & POLLNVAL) != 0) {
                return false;
            }
            ready.control_readable =
                (events & (POLLIN | POLLERR | POLLHUP)) != 0;
            ready.control_writable = (events & POLLOUT) != 0;
        }
        if (media_index >= 0) {
            short events = descriptors[media_index].revents;
            if (service->media_connecting &&
                (events & (POLLOUT | POLLERR | POLLHUP)) != 0) {
                if (!finish_media_connect(service)) {
                    return false;
                }
            } else if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                (void)meiso_composition_host_app_unbind_media_fd(
                    service->app, ECONNRESET
                );
                close_fd(&service->media_fd);
            } else {
                ready.media_readable = (events & POLLIN) != 0;
                ready.media_writable = (events & POLLOUT) != 0;
            }
        }
        for (scan = 0U; scan < 3U; ++scan) {
            uint8_t domain = (uint8_t)((service->loop_cursor + scan) % 3U);
            if (domain == 0U && stdin_index >= 0 &&
                (descriptors[stdin_index].revents &
                 (POLLIN | POLLHUP)) != 0) {
                if (!read_control_input(service, true)) {
                    return false;
                }
                service->loop_cursor = 1U;
                break;
            }
            if (domain == 1U &&
                (ready.control_readable || ready.control_writable ||
                 ready.media_readable || ready.media_writable)) {
                if (!pump_host(service, &ready, &progress)) {
                    return false;
                }
                service->loop_cursor = 2U;
                break;
            }
            if (domain == 2U && frame_index >= 0 &&
                (descriptors[frame_index].revents & POLLOUT) != 0) {
                if (!write_frame_output(service, true)) {
                    return false;
                }
                service->loop_cursor = 0U;
                break;
            }
        }
        if (frame_index >= 0 &&
            (descriptors[frame_index].revents &
             (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return false;
        }
    }
    return true;
}

static bool parse_frame_fd(const char *text, int *out_fd)
{
    uint64_t value;

    if (!parse_u64_text(text, INT_MAX, &value)) {
        return false;
    }
    *out_fd = (int)value;
    return true;
}

/* Testable process seam. It restores the caller's SIGPIPE disposition. */
int meiso_composition_debug_host_run(int argc, char **argv)
{
    const char *config_path = NULL;
    int frame_fd = -1;
    int index;
    bool interactive = false;
    ServiceConfig config;
    Service *service = NULL;
    struct sigaction action;
    struct sigaction old_action;
    bool signal_changed = false;
    bool ok;
    int exit_code = 1;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--config") == 0 && index + 1 < argc &&
            config_path == NULL) {
            config_path = argv[++index];
        } else if (strcmp(argv[index], "--interactive") == 0) {
            if (interactive) {
                fputs("E_ARGUMENT\n", stderr);
                return 2;
            }
            interactive = true;
        } else if (strcmp(argv[index], "--frame-fd") == 0 &&
                   index + 1 < argc && frame_fd < 0) {
            if (!parse_frame_fd(argv[++index], &frame_fd)) {
                fputs("E_ARGUMENT\n", stderr);
                return 2;
            }
        } else {
            fputs("E_ARGUMENT\n", stderr);
            return 2;
        }
    }
    if (config_path == NULL || !interactive ||
        !load_config(config_path, &config)) {
        fputs("E_CONFIG\n", stderr);
        return 2;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    (void)sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, &old_action) != 0) {
        fputs("E_SIGNAL\n", stderr);
        return 1;
    }
    signal_changed = true;
    service = calloc(1U, sizeof(*service));
    if (service == NULL) {
        fputs("E_MEMORY\n", stderr);
        goto done;
    }
    if (!initialize_service(service, &config, frame_fd)) {
        fputs("E_INIT\n", stderr);
        goto done;
    }
    ok = run_service(service);
    destroy_service(service);
    service->app = NULL;
    if (!ok) {
        fputs("E_RUNTIME\n", stderr);
        goto done;
    }
    exit_code = 0;

done:
    if (service != NULL) {
        if (service->app != NULL) {
            destroy_service(service);
        }
        free(service);
    }
    if (signal_changed && sigaction(SIGPIPE, &old_action, NULL) != 0 &&
        exit_code == 0) {
        fputs("E_SIGNAL\n", stderr);
        exit_code = 1;
    }
    return exit_code;
}

#ifndef MEISO_COMPOSITION_DEBUG_HOST_NO_MAIN
int main(int argc, char **argv)
{
    return meiso_composition_debug_host_run(argc, argv);
}
#endif
