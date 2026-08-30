#include "meiso_composition_host_bridge_private.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Closed target configuration and numeric TCP connection support. */

/*
 * The closed config requires numeric control_ip/control_port and
 * media_ip/media_port, both peer IDs, local_incarnation, nonce_high/low,
 * target_manifest=k230d_zero_gc2093_v1, and the exact probe receipt masks
 * wifi_supported_fields/device_supported_fields, catalog_generation and all
 * six endpoint incarnations. Every key is single-assignment. This private
 * manifest is trusted target configuration,
 * never a browser command; its capability values mirror the K230D Edge
 * Runtime's fixed
 * K230D/GC2093 catalog while the two probe masks carry boot-specific truth.
 */

/* ---------- bounded process-local support ---------- */

void meiso_composition_host_bridge_private_close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

bool meiso_composition_host_bridge_private_parse_u64_text(
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

static MeisoCompositionProfileCatalogConfig default_catalog(void)
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

MeisoCompositionEdgeSimulatorConfig meiso_composition_host_bridge_private_offline_simulator_config(void)
{
    MeisoCompositionEdgeSimulatorConfig value;

    memset(&value, 0, sizeof(value));
    value.catalog_generation = 1U;
    value.catalog = default_catalog();
    value.catalog.wifi_capability.supported_fields =
        MEISO_WIFI_TELEMETRY_FIELD_ALL;
    value.catalog.device_telemetry_capability.supported_fields =
        MEISO_DEVICE_TELEMETRY_FIELD_ALL;
    value.continuity_serial = 1U;
    value.desired_owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 1U};
    value.wifi_production_epoch = 1001U;
    value.render_media_stream_epoch = 2001U;
    value.camera_media_stream_epoch = 3001U;
    value.wifi_sample.selected_fields = MEISO_WIFI_TELEMETRY_FIELD_ALL;
    value.wifi_sample.present_fields = MEISO_WIFI_TELEMETRY_FIELD_ALL;
    value.wifi_sample.link_state = true;
    value.wifi_sample.signal_mbm = -4200;
    value.wifi_sample.rx_bytes = 1000000U;
    value.wifi_sample.tx_bytes = 500000U;
    value.wifi_sample.rx_packets = 1000U;
    value.wifi_sample.tx_packets = 500U;
    value.device_telemetry_enabled = true;
    value.device_production_epoch = 4001U;
    value.device_sample.selected_fields = MEISO_DEVICE_TELEMETRY_FIELD_ALL;
    value.device_sample.present_fields = MEISO_DEVICE_TELEMETRY_FIELD_ALL;
    value.device_sample.temperature_millic = 25000;
    value.device_sample.temperature_uncertainty_millic = 1000U;
    value.device_sample.cpu_load_ppm = 250000U;
    value.device_sample.memory_total_bytes = UINT64_C(536870912);
    value.device_sample.memory_available_bytes = UINT64_C(402653184);
    value.composition_enabled = true;
    value.composition_blob_slot_count = 4U;
    value.composition_clock_epoch = 5001U;
    value.presentation_production_epoch = 6001U;
    value.scene_production_epoch = 7001U;
    value.cpu_raster_preview_enabled = true;
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
    MeisoCompositionHostBridgePrivateConfig *config,
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
        *out_bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_WIFI_INCARNATION;
    } else if (strcmp(key, "presentation_incarnation") == 0) {
        destination = &config->catalog.presentation_incarnation;
        *out_bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_PRESENTATION_INCARNATION;
    } else if (strcmp(key, "scene_incarnation") == 0) {
        destination = &config->catalog.scene_incarnation;
        *out_bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_SCENE_INCARNATION;
    } else if (strcmp(key, "device_incarnation") == 0) {
        destination = &config->catalog.device_telemetry_incarnation;
        *out_bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_DEVICE_INCARNATION;
    } else if (strcmp(key, "render_incarnation") == 0) {
        destination = &config->catalog.render_media_incarnation;
        *out_bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_RENDER_INCARNATION;
    } else if (strcmp(key, "camera_incarnation") == 0) {
        destination = &config->catalog.camera_media_incarnation;
        *out_bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_CAMERA_INCARNATION;
    } else {
        *out_matched = false;
        return true;
    }
    *out_matched = true;
    if (!meiso_composition_host_bridge_private_parse_u64_text(value, UINT64_MAX, &number) || number == 0U) {
        return false;
    }
    *destination = number;
    return true;
}

static bool parse_config_line(
    MeisoCompositionHostBridgePrivateConfig *config,
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
        bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_CONTROL_ADDRESS;
        if (strlen(value) >= INET6_ADDRSTRLEN) {
            return false;
        }
        if (!parse_numeric_address(value, 1U, &config->control_address,
                                   &config->control_address_size)) {
            return false;
        }
    } else if (strcmp(key, "control_port") == 0) {
        bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_CONTROL_PORT;
        if (!meiso_composition_host_bridge_private_parse_u64_text(value, UINT16_MAX, &number) || number == 0U) {
            return false;
        }
        config->control_port = (uint16_t)number;
    } else if (strcmp(key, "media_ip") == 0) {
        bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_MEDIA_ADDRESS;
        if (strlen(value) >= INET6_ADDRSTRLEN) {
            return false;
        }
        if (!parse_numeric_address(value, 1U, &config->media_address,
                                   &config->media_address_size)) {
            return false;
        }
    } else if (strcmp(key, "media_port") == 0) {
        bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_MEDIA_PORT;
        if (!meiso_composition_host_bridge_private_parse_u64_text(value, UINT16_MAX, &number) || number == 0U) {
            return false;
        }
        config->media_port = (uint16_t)number;
    } else if (strcmp(key, "local_host_peer_id_high") == 0) {
        bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_LOCAL_PEER_HIGH;
        if (!meiso_composition_host_bridge_private_parse_u64_text(value, UINT64_MAX, &config->local_peer_id.high)) {
            return false;
        }
    } else if (strcmp(key, "local_host_peer_id_low") == 0) {
        bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_LOCAL_PEER_LOW;
        if (!meiso_composition_host_bridge_private_parse_u64_text(value, UINT64_MAX, &config->local_peer_id.low)) {
            return false;
        }
    } else if (strcmp(key, "expected_edge_peer_id_high") == 0) {
        bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_EDGE_PEER_HIGH;
        if (!meiso_composition_host_bridge_private_parse_u64_text(value, UINT64_MAX, &config->edge_peer_id.high)) {
            return false;
        }
    } else if (strcmp(key, "expected_edge_peer_id_low") == 0) {
        bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_EDGE_PEER_LOW;
        if (!meiso_composition_host_bridge_private_parse_u64_text(value, UINT64_MAX, &config->edge_peer_id.low)) {
            return false;
        }
    } else if (strcmp(key, "local_incarnation") == 0) {
        bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_LOCAL_INCARNATION;
        if (!meiso_composition_host_bridge_private_parse_u64_text(value, UINT64_MAX, &config->local_incarnation) ||
            config->local_incarnation == 0U) {
            return false;
        }
    } else if (strcmp(key, "nonce_high") == 0) {
        bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_NONCE_HIGH;
        if (!meiso_composition_host_bridge_private_parse_u64_text(value, UINT64_MAX, &config->nonce.high)) {
            return false;
        }
    } else if (strcmp(key, "nonce_low") == 0) {
        bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_NONCE_LOW;
        if (!meiso_composition_host_bridge_private_parse_u64_text(value, UINT64_MAX, &config->nonce.low)) {
            return false;
        }
    } else if (strcmp(key, "target_manifest") == 0) {
        bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_TARGET_MANIFEST;
        if (strcmp(value, "k230d_zero_gc2093_v1") != 0) {
            return false;
        }
    } else if (strcmp(key, "wifi_supported_fields") == 0) {
        bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_WIFI_FIELDS;
        if (!meiso_composition_host_bridge_private_parse_u64_text(value, MEISO_WIFI_TELEMETRY_FIELD_ALL, &number) ||
            number == 0U) {
            return false;
        }
        config->catalog.wifi_capability.supported_fields = (uint32_t)number;
    } else if (strcmp(key, "device_supported_fields") == 0) {
        bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_DEVICE_FIELDS;
        if (!meiso_composition_host_bridge_private_parse_u64_text(value, MEISO_DEVICE_TELEMETRY_FIELD_ALL, &number) ||
            number == 0U) {
            return false;
        }
        config->catalog.device_telemetry_capability.supported_fields =
            (uint32_t)number;
    } else if (strcmp(key, "catalog_generation") == 0) {
        bit = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_CATALOG_GENERATION;
        if (!meiso_composition_host_bridge_private_parse_u64_text(value, UINT64_MAX, &config->catalog_generation) ||
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

bool meiso_composition_host_bridge_private_load_config(const char *path, MeisoCompositionHostBridgePrivateConfig *out_config)
{
    FILE *file;
    MeisoCompositionHostBridgePrivateConfig config;
    char line[MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_LINE_BYTES];
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
    if (!ok || (seen & MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_REQUIRED) != MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_REQUIRED ||
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

bool meiso_composition_host_bridge_private_deadline_after_ms(int milliseconds, struct timespec *out_deadline)
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

int meiso_composition_host_bridge_private_connect_numeric(
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
        !meiso_composition_host_bridge_private_deadline_after_ms(MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONNECT_TIMEOUT_MS, &deadline)) {
        return -1;
    }
    fd = socket(address->ss_family, SOCK_STREAM, 0);
    if (fd < 0 || !set_nonblocking(fd)) {
        meiso_composition_host_bridge_private_close_fd(&fd);
        return -1;
    }
    result = connect(fd, (const struct sockaddr *)(const void *)address,
                     address_size);
    if (result == 0) {
        return fd;
    }
    if (errno != EINPROGRESS) {
        meiso_composition_host_bridge_private_close_fd(&fd);
        return -1;
    }
    descriptor.fd = fd;
    descriptor.events = POLLOUT;
    descriptor.revents = 0;
    for (;;) {
        int remaining = deadline_remaining_ms(&deadline);
        if (remaining <= 0) {
            meiso_composition_host_bridge_private_close_fd(&fd);
            return -1;
        }
        result = poll(&descriptor, 1U, remaining);
        if (result > 0) {
            break;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        meiso_composition_host_bridge_private_close_fd(&fd);
        return -1;
    }
    error_number = 0;
    error_size = (socklen_t)sizeof(error_number);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error_number, &error_size) != 0 ||
        error_number != 0) {
        meiso_composition_host_bridge_private_close_fd(&fd);
        return -1;
    }
    return fd;
}

int meiso_composition_host_bridge_private_start_numeric_connect(
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
        meiso_composition_host_bridge_private_close_fd(&fd);
        return -1;
    }
    result = connect(fd, (const struct sockaddr *)(const void *)address,
                     address_size);
    if (result == 0) {
        *out_connected = true;
        return fd;
    }
    if (errno != EINPROGRESS) {
        meiso_composition_host_bridge_private_close_fd(&fd);
        return -1;
    }
    *out_connected = false;
    return fd;
}

bool meiso_composition_host_bridge_private_time_reached(const struct timespec *when)
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
