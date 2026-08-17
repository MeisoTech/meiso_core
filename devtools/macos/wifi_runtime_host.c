#define _POSIX_C_SOURCE 200809L

#include "meiso_posix_tcp.h"
#include "meiso_runtime.h"
#include "wifi_telemetry.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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

enum {
    HOST_MAX_FRAME_BYTES = 4096,
    HOST_MAX_RUNTIME_BYTES = 2048,
    HOST_MAX_REGISTRY_ENTRIES = 16,
    HOST_IO_STEP_BYTES = 1024,
    HOST_CONNECT_TIMEOUT_MS = 10000,
    HOST_PHASE_TIMEOUT_MS = 20000,
    HOST_MAX_SAMPLES = 64,
    HOST_MAX_STOPPING_SAMPLES = 64,
    HOST_STREAM_QUEUE_SLOTS = 8,
    HOST_STREAM_HISTORY_SLOTS = 8,
    HOST_MAX_STREAM_DEPENDENCY_BYTES = 64,
    HOST_SESSION_ARENA_BYTES = 8192,
    HOST_ADAPTER_ARENA_BYTES = 16384,
    HOST_RUNTIME_ARENA_BYTES = 32768,
    HOST_RESOURCE_ARENA_BYTES = 16384,
    HOST_CONFIG_LINE_BYTES = 256,
    HOST_CONTROL_LINE_BYTES = 256
};

typedef union {
    max_align_t alignment;
    uint8_t bytes[HOST_SESSION_ARENA_BYTES];
} SessionArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[HOST_ADAPTER_ARENA_BYTES];
} AdapterArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[HOST_RUNTIME_ARENA_BYTES];
} RuntimeArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[HOST_RESOURCE_ARENA_BYTES];
} ResourceArena;

typedef struct {
    struct in_addr connect_address;
    uint16_t connect_port;
    MeisoPeerId local_peer_id;
    MeisoPeerId expected_edge_peer_id;
    MeisoWifiTelemetryDesiredParameters desired;
    uint32_t sample_count;
} HostBootstrapConfig;

typedef struct {
    SessionArena session;
    AdapterArena adapter;
    RuntimeArena runtime;
    ResourceArena desired_publisher;
    ResourceArena actual_replica;
    ResourceArena stream;
} HostStorage;

typedef struct {
    int fd;
    HostBootstrapConfig bootstrap;
    MeisoSessionConfig session_config;
    MeisoSession *session;
    MeisoPosixTcp *adapter;
    MeisoRuntime *runtime;
    MeisoStatePublisher *desired_publisher;
    MeisoStateReplica *actual_replica;
    MeisoStream *stream;
    MeisoOwnerScope host_owner;
    MeisoOwnerScope edge_owner;
    MeisoRuntimeRouteKey desired_route;
    MeisoRuntimeRouteKey actual_route;
    MeisoRuntimeRouteKey data_route;
    MeisoWifiTelemetryCapability capability;
    MeisoWifiTelemetryEffectiveParameters active_effective;
    MeisoOwnerScope stream_producer;
    uint64_t stream_epoch;
    uint64_t running_production_epoch;
    uint64_t samples_received;
    bool catalog_active;
    bool active_effective_present;
    bool stream_producer_active;
    bool production_active;
    bool interactive;
} Host;

typedef enum {
    HOST_CONTROL_INVALID = 0,
    HOST_CONTROL_APPLY,
    HOST_CONTROL_CLOSE
} HostControlType;

typedef struct {
    HostControlType type;
    bool enabled;
    MeisoWifiTelemetryDesiredParameters parameters;
} HostControlCommand;

typedef struct {
    char bytes[HOST_CONTROL_LINE_BYTES];
    size_t size;
} HostControlInput;

typedef enum {
    HOST_INTERACTIVE_ITEM_FAILED = 0,
    HOST_INTERACTIVE_ITEM_COMMAND,
    HOST_INTERACTIVE_ITEM_RUNTIME
} HostInteractiveItemType;

static HostStorage g_storage;

static void close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static bool parse_u64(const char *text, uint64_t maximum, uint64_t *out_value)
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

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
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

static bool parse_config_line(
    HostBootstrapConfig *config,
    uint32_t *seen,
    char *line
)
{
    char *separator;
    char *key;
    char *value;
    char *comment = strchr(line, '#');
    uint64_t number;
    uint32_t bit;

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
    if (strcmp(key, "connect_ipv4") == 0) {
        bit = UINT32_C(1) << 0U;
        if (inet_pton(AF_INET, value, &config->connect_address) != 1) {
            return false;
        }
    } else if (strcmp(key, "connect_port") == 0) {
        bit = UINT32_C(1) << 1U;
        if (!parse_u64(value, UINT16_MAX, &number) || number == 0U) {
            return false;
        }
        config->connect_port = (uint16_t)number;
    } else if (strcmp(key, "selected_fields") == 0) {
        bit = UINT32_C(1) << 2U;
        if (!parse_u64(value, UINT32_MAX, &number) || number == 0U ||
            ((uint32_t)number &
                ~(uint32_t)MEISO_WIFI_TELEMETRY_FIELD_ALL) != 0U) {
            return false;
        }
        config->desired.selected_fields = (uint32_t)number;
    } else if (strcmp(key, "sample_period_ms") == 0) {
        bit = UINT32_C(1) << 3U;
        if (!parse_u64(value, UINT32_MAX, &number)) {
            return false;
        }
        config->desired.sample_period_ms = (uint32_t)number;
    } else if (strcmp(key, "sample_count") == 0) {
        bit = UINT32_C(1) << 4U;
        if (!parse_u64(value, HOST_MAX_SAMPLES, &number) || number == 0U) {
            return false;
        }
        config->sample_count = (uint32_t)number;
    } else if (strcmp(key, "local_host_peer_id_high") == 0) {
        bit = UINT32_C(1) << 5U;
        if (!parse_u64(value, UINT64_MAX, &number)) {
            return false;
        }
        config->local_peer_id.high = number;
    } else if (strcmp(key, "local_host_peer_id_low") == 0) {
        bit = UINT32_C(1) << 6U;
        if (!parse_u64(value, UINT64_MAX, &number)) {
            return false;
        }
        config->local_peer_id.low = number;
    } else if (strcmp(key, "expected_edge_peer_id_high") == 0) {
        bit = UINT32_C(1) << 7U;
        if (!parse_u64(value, UINT64_MAX, &number)) {
            return false;
        }
        config->expected_edge_peer_id.high = number;
    } else if (strcmp(key, "expected_edge_peer_id_low") == 0) {
        bit = UINT32_C(1) << 8U;
        if (!parse_u64(value, UINT64_MAX, &number)) {
            return false;
        }
        config->expected_edge_peer_id.low = number;
    } else {
        return false;
    }
    if ((*seen & bit) != 0U) {
        return false;
    }
    *seen |= bit;
    return true;
}

static bool load_config(
    const char *path,
    bool interactive,
    HostBootstrapConfig *out_config
)
{
    FILE *file;
    char line[HOST_CONFIG_LINE_BYTES];
    uint8_t encoded[MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE];
    uint32_t seen = 0U;
    size_t encoded_size = 0U;
    bool ok = true;

    if (path == NULL || out_config == NULL) {
        return false;
    }
    memset(out_config, 0, sizeof(*out_config));
    file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strchr(line, '\n') == NULL && !feof(file)) {
            ok = false;
            break;
        }
        if (!parse_config_line(out_config, &seen, line)) {
            ok = false;
            break;
        }
    }
    if (ferror(file) != 0) {
        ok = false;
    }
    (void)fclose(file);
    const uint32_t bootstrap_fields = UINT32_C(0x1e3);
    const uint32_t one_shot_fields = UINT32_C(0x01c);
    const uint32_t desired_fields = seen & one_shot_fields;
    const bool config_shape_valid = interactive ?
        (seen == bootstrap_fields ||
         seen == (bootstrap_fields | one_shot_fields)) :
        seen == (bootstrap_fields | one_shot_fields);

    return ok && config_shape_valid &&
        meiso_peer_id_valid(out_config->local_peer_id) &&
        meiso_peer_id_valid(out_config->expected_edge_peer_id) &&
        !meiso_peer_id_equal(
            out_config->local_peer_id,
            out_config->expected_edge_peer_id
        ) && (!interactive || desired_fields == 0U ||
            desired_fields == one_shot_fields) &&
        (desired_fields == 0U ||
            meiso_wifi_telemetry_desired_parameters_encode(
                &out_config->desired,
                (MeisoMutableBuffer){encoded, sizeof(encoded)},
                &encoded_size
            ) == MEISO_WIFI_TELEMETRY_CODEC_OK);
}

static bool read_entropy(void *output, size_t size)
{
    uint8_t *bytes = output;
    size_t offset = 0U;
    int fd;

    if (output == NULL || size == 0U) {
        return false;
    }
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return false;
    }
    while (offset < size) {
        const ssize_t amount = read(fd, bytes + offset, size - offset);
        if (amount > 0) {
            offset += (size_t)amount;
        } else if (amount < 0 && errno == EINTR) {
            continue;
        } else {
            close_fd(&fd);
            return false;
        }
    }
    close_fd(&fd);
    return true;
}

static bool random_incarnation(uint64_t *out_value)
{
    if (out_value == NULL || !read_entropy(out_value, sizeof(*out_value))) {
        return false;
    }
    if (*out_value == 0U) {
        *out_value = 1U;
    }
    return true;
}

static bool random_nonce(MeisoSessionNonce *out_value)
{
    if (out_value == NULL || !read_entropy(out_value, sizeof(*out_value))) {
        return false;
    }
    if (!meiso_session_nonce_valid(*out_value)) {
        out_value->low = 1U;
    }
    return true;
}

static bool set_nonblocking_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    int descriptor_flags = fcntl(fd, F_GETFD);

    return flags >= 0 && descriptor_flags >= 0 &&
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0;
}

static bool deadline_after_ms(struct timespec *out_deadline, uint64_t ms)
{
    uint64_t nanoseconds;

    if (out_deadline == NULL ||
        clock_gettime(CLOCK_MONOTONIC, out_deadline) != 0) {
        return false;
    }
    out_deadline->tv_sec += (time_t)(ms / UINT64_C(1000));
    nanoseconds = (uint64_t)out_deadline->tv_nsec +
        (ms % UINT64_C(1000)) * UINT64_C(1000000);
    out_deadline->tv_sec += (time_t)(nanoseconds / UINT64_C(1000000000));
    out_deadline->tv_nsec = (long)(nanoseconds % UINT64_C(1000000000));
    return true;
}

static int deadline_wait_ms(const struct timespec *deadline)
{
    struct timespec now;
    int64_t seconds;
    int64_t nanoseconds;
    int64_t milliseconds;

    if (deadline == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    seconds = (int64_t)deadline->tv_sec - (int64_t)now.tv_sec;
    nanoseconds = (int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec;
    if (nanoseconds < 0) {
        nanoseconds += INT64_C(1000000000);
        --seconds;
    }
    if (seconds < 0 || (seconds == 0 && nanoseconds <= 0)) {
        return 0;
    }
    milliseconds = seconds * INT64_C(1000) +
        (nanoseconds + INT64_C(999999)) / INT64_C(1000000);
    return milliseconds > INT32_MAX ? INT32_MAX : (int)milliseconds;
}

static int connect_numeric_ipv4(const HostBootstrapConfig *config)
{
    struct sockaddr_in address;
    struct timespec deadline;
    struct pollfd descriptor;
    int fd = -1;
    int result;

    if (config == NULL || !deadline_after_ms(
            &deadline, HOST_CONNECT_TIMEOUT_MS
        )) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0 || !set_nonblocking_cloexec(fd)) {
        close_fd(&fd);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(config->connect_port);
    address.sin_addr = config->connect_address;
    result = connect(
        fd, (const struct sockaddr *)(const void *)&address,
        (socklen_t)sizeof(address)
    );
    if (result == 0) {
        return fd;
    }
    if (errno != EINPROGRESS) {
        close_fd(&fd);
        return -1;
    }
    for (;;) {
        int wait_ms = deadline_wait_ms(&deadline);
        int socket_error = 0;
        socklen_t socket_error_size = (socklen_t)sizeof(socket_error);

        if (wait_ms == 0) {
            break;
        }
        descriptor.fd = fd;
        descriptor.events = POLLOUT;
        descriptor.revents = 0;
        result = poll(&descriptor, (nfds_t)1U, wait_ms);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0 || (descriptor.revents & POLLNVAL) != 0 ||
            getsockopt(
                fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size
            ) != 0 || socket_error != 0) {
            break;
        }
        if ((descriptor.revents & POLLOUT) != 0) {
            return fd;
        }
    }
    close_fd(&fd);
    return -1;
}

static bool initialize_session_transport(Host *host)
{
    MeisoPosixTcpConfig adapter_config;
    size_t required;

    memset(&host->session_config, 0, sizeof(host->session_config));
    host->session_config.local_role = MEISO_SESSION_ROLE_HOST;
    host->session_config.local_peer_id = host->bootstrap.local_peer_id;
    host->session_config.expected_peer_id =
        host->bootstrap.expected_edge_peer_id;
    host->session_config.protocol = (MeisoProtocolVersion){1U, 0U};
    host->session_config.profile = *meiso_wifi_telemetry_profile();
    host->session_config.limits = (MeisoSessionLimits){
        HOST_MAX_FRAME_BYTES,
        HOST_MAX_RUNTIME_BYTES,
        HOST_MAX_REGISTRY_ENTRIES
    };
    if (!random_incarnation(&host->session_config.local_incarnation) ||
        !random_nonce(&host->session_config.local_nonce)) {
        return false;
    }
    required = meiso_session_memory_required(&host->session_config);
    if (required == 0U || required > sizeof(g_storage.session.bytes) ||
        meiso_session_init(
            g_storage.session.bytes, sizeof(g_storage.session.bytes),
            &host->session_config, &host->session
        ) != MEISO_SESSION_INIT_OK) {
        return false;
    }
    adapter_config = (MeisoPosixTcpConfig){
        host->fd, host->session, HOST_MAX_FRAME_BYTES, HOST_IO_STEP_BYTES
    };
    required = meiso_posix_tcp_memory_required(&adapter_config);
    if (required == 0U || required > sizeof(g_storage.adapter.bytes) ||
        meiso_posix_tcp_init(
            g_storage.adapter.bytes, sizeof(g_storage.adapter.bytes),
            &adapter_config, &host->adapter
        ) != MEISO_POSIX_TCP_INIT_OK) {
        return false;
    }
    return meiso_session_start(host->session) ==
        MEISO_SESSION_START_STARTED;
}

static bool write_status_progress(MeisoPosixTcpWriteStatus status)
{
    return status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
        status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK ||
        status == MEISO_POSIX_TCP_WRITE_INTERRUPTED ||
        status == MEISO_POSIX_TCP_WRITE_FRAME_SENT ||
        status == MEISO_POSIX_TCP_WRITE_IDLE;
}

static bool read_status_progress(MeisoPosixTcpReadStatus status)
{
    return status == MEISO_POSIX_TCP_READ_PROGRESS ||
        status == MEISO_POSIX_TCP_READ_WOULD_BLOCK ||
        status == MEISO_POSIX_TCP_READ_INTERRUPTED ||
        status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS;
}

static bool flush_outbound(Host *host)
{
    struct timespec deadline;

    if (host == NULL || host->adapter == NULL || host->session == NULL ||
        !deadline_after_ms(&deadline, HOST_PHASE_TIMEOUT_MS)) {
        return false;
    }
    for (;;) {
        MeisoSessionSnapshot snapshot;
        struct pollfd descriptor;
        int result;
        int wait_ms;

        if (!meiso_session_snapshot(host->session, &snapshot) ||
            snapshot.state == MEISO_SESSION_STATE_FAILED) {
            return false;
        }
        if (!snapshot.outbound_pending) {
            return true;
        }
        wait_ms = deadline_wait_ms(&deadline);
        if (wait_ms == 0) {
            return false;
        }
        descriptor.fd = host->fd;
        descriptor.events = POLLOUT;
        descriptor.revents = 0;
        result = poll(&descriptor, (nfds_t)1U, wait_ms);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0 || (descriptor.revents & POLLNVAL) != 0 ||
            ((descriptor.revents & (POLLERR | POLLHUP)) != 0 &&
             (descriptor.revents & POLLOUT) == 0) ||
            ((descriptor.revents & POLLOUT) != 0 && !write_status_progress(
                meiso_posix_tcp_on_writable(host->adapter)
            ))) {
            return false;
        }
    }
}

static bool wait_session_ready(Host *host, MeisoSessionSnapshot *out_snapshot)
{
    struct timespec deadline;

    if (host == NULL || out_snapshot == NULL ||
        !deadline_after_ms(&deadline, HOST_PHASE_TIMEOUT_MS)) {
        return false;
    }
    for (;;) {
        MeisoSessionSnapshot snapshot;
        struct pollfd descriptor;
        short events = 0;
        int result;
        int wait_ms;

        if (!meiso_session_snapshot(host->session, &snapshot)) {
            return false;
        }
        if (snapshot.state == MEISO_SESSION_STATE_READY &&
            snapshot.failure == MEISO_SESSION_FAILURE_NONE &&
            snapshot.remote_identity_known && snapshot.session_id_known &&
            meiso_peer_id_equal(
                snapshot.remote_peer_id,
                host->bootstrap.expected_edge_peer_id
            )) {
            *out_snapshot = snapshot;
            return true;
        }
        if (snapshot.state == MEISO_SESSION_STATE_FAILED ||
            snapshot.state == MEISO_SESSION_STATE_CLOSED) {
            return false;
        }
        if (meiso_posix_tcp_wants_read(host->adapter)) {
            events = (short)(events | POLLIN);
        }
        if (meiso_posix_tcp_wants_write(host->adapter)) {
            events = (short)(events | POLLOUT);
        }
        wait_ms = deadline_wait_ms(&deadline);
        if (wait_ms == 0 || events == 0) {
            return false;
        }
        descriptor.fd = host->fd;
        descriptor.events = events;
        descriptor.revents = 0;
        result = poll(&descriptor, (nfds_t)1U, wait_ms);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0 || (descriptor.revents & POLLNVAL) != 0) {
            return false;
        }
        if ((descriptor.revents & POLLOUT) != 0 && !write_status_progress(
                meiso_posix_tcp_on_writable(host->adapter)
            )) {
            return false;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            MeisoSessionRuntimeMessage runtime_message;
            const MeisoPosixTcpReadStatus read_status =
                meiso_posix_tcp_on_readable(
                    host->adapter, &runtime_message
                );
            if (!read_status_progress(read_status)) {
                return false;
            }
        }
    }
}

static bool wait_runtime_event(
    Host *host,
    const struct timespec *deadline,
    MeisoRuntimeEvent *out_event
)
{
    if (host == NULL || deadline == NULL || out_event == NULL) {
        return false;
    }
    for (;;) {
        MeisoSessionSnapshot snapshot;
        struct pollfd descriptor;
        short events = 0;
        int result;
        int wait_ms;

        if (!meiso_session_snapshot(host->session, &snapshot) ||
            snapshot.state == MEISO_SESSION_STATE_FAILED ||
            snapshot.state == MEISO_SESSION_STATE_CLOSED) {
            return false;
        }
        if (meiso_posix_tcp_wants_read(host->adapter)) {
            events = (short)(events | POLLIN);
        }
        if (meiso_posix_tcp_wants_write(host->adapter)) {
            events = (short)(events | POLLOUT);
        }
        wait_ms = deadline_wait_ms(deadline);
        if (wait_ms == 0 || events == 0) {
            return false;
        }
        descriptor.fd = host->fd;
        descriptor.events = events;
        descriptor.revents = 0;
        result = poll(&descriptor, (nfds_t)1U, wait_ms);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0 || (descriptor.revents & POLLNVAL) != 0) {
            return false;
        }
        if ((descriptor.revents & POLLOUT) != 0 && !write_status_progress(
                meiso_posix_tcp_on_writable(host->adapter)
            )) {
            return false;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            MeisoSessionRuntimeMessage message;
            const MeisoPosixTcpReadStatus read_status =
                meiso_posix_tcp_on_readable(host->adapter, &message);

            if (read_status == MEISO_POSIX_TCP_READ_RUNTIME) {
                return meiso_runtime_receive(
                    host->runtime, &message, out_event
                ) == MEISO_RUNTIME_OK;
            }
            if (!read_status_progress(read_status)) {
                return false;
            }
        }
    }
}

static bool initialize_runtime_resources(
    Host *host,
    const MeisoSessionSnapshot *session_snapshot
)
{
    MeisoRuntimeConfig runtime_config;
    const MeisoResourceDescriptor *resources;
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const MeisoStreamConfig stream_config = {
        MEISO_STREAM_ORDER_STRICT_MONOTONIC,
        MEISO_STREAM_OVERFLOW_REJECT_NEWEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        HOST_STREAM_QUEUE_SLOTS,
        HOST_STREAM_HISTORY_SLOTS,
        HOST_MAX_STREAM_DEPENDENCY_BYTES
    };
    size_t resource_count = 0U;
    size_t required;

    if (host == NULL || session_snapshot == NULL ||
        session_snapshot->remote_incarnation == 0U) {
        return false;
    }
    memset(&runtime_config, 0, sizeof(runtime_config));
    runtime_config.session = host->session;
    runtime_config.max_endpoints = 1U;
    runtime_config.max_endpoint_history = 4U;
    runtime_config.max_routes = MEISO_WIFI_TELEMETRY_RESOURCE_COUNT;
    runtime_config.max_capability_bytes =
        MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE;
    runtime_config.max_desired_profile_bytes_per_endpoint =
        MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE;
    runtime_config.max_actual_profile_bytes_per_endpoint =
        MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE;
    required = meiso_runtime_memory_required(&runtime_config);
    if (required == 0U || required > sizeof(g_storage.runtime.bytes) ||
        meiso_runtime_init(
            g_storage.runtime.bytes, sizeof(g_storage.runtime.bytes),
            &runtime_config, &host->runtime
        ) != MEISO_RUNTIME_INIT_OK) {
        return false;
    }

    resources = meiso_wifi_telemetry_resources(&resource_count);
    if (resources == NULL ||
        resource_count != MEISO_WIFI_TELEMETRY_RESOURCE_COUNT) {
        return false;
    }
    required = meiso_state_publisher_memory_required(
        &resources[0], &state_config
    );
    if (required == 0U ||
        required > sizeof(g_storage.desired_publisher.bytes) ||
        meiso_state_publisher_init(
            g_storage.desired_publisher.bytes,
            sizeof(g_storage.desired_publisher.bytes),
            &resources[0], &state_config, &host->desired_publisher
        ) != MEISO_STATE_INIT_OK) {
        return false;
    }
    required = meiso_state_replica_memory_required(&resources[1]);
    if (required == 0U || required > sizeof(g_storage.actual_replica.bytes) ||
        meiso_state_replica_init(
            g_storage.actual_replica.bytes,
            sizeof(g_storage.actual_replica.bytes),
            &resources[1], &host->actual_replica
        ) != MEISO_STATE_INIT_OK) {
        return false;
    }
    required = meiso_stream_memory_required(&resources[2], &stream_config);
    if (required == 0U || required > sizeof(g_storage.stream.bytes) ||
        meiso_stream_init(
            g_storage.stream.bytes, sizeof(g_storage.stream.bytes),
            &resources[2], &stream_config, &host->stream
        ) != MEISO_STREAM_INIT_OK) {
        return false;
    }
    host->host_owner = (MeisoOwnerScope){
        MEISO_OWNER_ROLE_HOST, host->session_config.local_incarnation
    };
    host->edge_owner = (MeisoOwnerScope){
        MEISO_OWNER_ROLE_EDGE, session_snapshot->remote_incarnation
    };
    return meiso_state_publisher_begin_delivery(
            host->desired_publisher, (MeisoDeliveryGeneration){1U}
        ) == MEISO_STATE_DELIVERY_STARTED &&
        meiso_state_replica_begin_owner(
            host->actual_replica, host->edge_owner
        ) == MEISO_STATE_OWNER_STARTED;
}

static void emit_session_ready(const MeisoSessionSnapshot *snapshot)
{
    (void)printf(
        "{\"type\":\"session\",\"status\":\"READY\","
        "\"remote_identity_known\":true,\"identity_match\":true,"
        "\"session_id_known\":true,\"limits\":{"
        "\"max_frame_bytes\":%" PRIu32 ","
        "\"max_runtime_payload_bytes\":%" PRIu32 ","
        "\"max_registry_entries\":%" PRIu32 "}}\n",
        snapshot->negotiated_limits.max_frame_bytes,
        snapshot->negotiated_limits.max_runtime_payload_bytes,
        snapshot->negotiated_limits.max_registry_entries
    );
    (void)fflush(stdout);
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

static MeisoRuntimeRouteKey route_key(
    uint64_t generation,
    const MeisoRuntimeRouteDescriptor *route
)
{
    MeisoRuntimeRouteKey key;

    memset(&key, 0, sizeof(key));
    key.catalog_generation = generation;
    key.endpoint_id = route->endpoint_id;
    key.endpoint_incarnation = route->endpoint_incarnation;
    key.resource_id = route->resource.id;
    key.facet = route->facet;
    return key;
}

static bool validate_staged_catalog(
    Host *host,
    uint64_t generation
)
{
    static const MeisoRuntimeFacet expected_facets[
        MEISO_WIFI_TELEMETRY_RESOURCE_COUNT
    ] = {
        MEISO_RUNTIME_FACET_DESIRED,
        MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA
    };
    MeisoRuntimeSnapshot snapshot;
    MeisoRuntimeEndpointDescriptor endpoint;
    const MeisoEndpointId *expected_endpoint =
        meiso_wifi_telemetry_endpoint();
    const MeisoSchemaRef *expected_capability_schema =
        meiso_wifi_telemetry_capability_schema();
    const MeisoResourceDescriptor *expected_resources;
    bool found[MEISO_WIFI_TELEMETRY_RESOURCE_COUNT] = {false, false, false};
    size_t expected_count = 0U;
    size_t index;

    if (host == NULL || generation == 0U || expected_endpoint == NULL ||
        expected_capability_schema == NULL ||
        !meiso_runtime_snapshot(host->runtime, &snapshot) ||
        !snapshot.catalog_decision_pending || !snapshot.staged_catalog_present ||
        snapshot.staged_catalog_generation != generation ||
        snapshot.staged_endpoint_count != 1U ||
        snapshot.staged_route_count != MEISO_WIFI_TELEMETRY_RESOURCE_COUNT ||
        meiso_runtime_staged_endpoint_at(
            host->runtime, 0U, &endpoint
        ) != MEISO_RUNTIME_OK ||
        !meiso_endpoint_id_equal(endpoint.id, *expected_endpoint) ||
        !meiso_schema_ref_equal(
            endpoint.capability_schema, *expected_capability_schema
        ) ||
        meiso_wifi_telemetry_capability_decode(
            endpoint.capability, &host->capability
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        (!host->interactive &&
         !meiso_wifi_telemetry_parameters_supported(
             &host->capability, &host->bootstrap.desired
         ))) {
        return false;
    }
    expected_resources = meiso_wifi_telemetry_resources(&expected_count);
    if (expected_resources == NULL ||
        expected_count != MEISO_WIFI_TELEMETRY_RESOURCE_COUNT) {
        return false;
    }
    for (index = 0U; index < snapshot.staged_route_count; ++index) {
        MeisoRuntimeRouteDescriptor offered;
        size_t expected_index;
        bool matched = false;

        if (meiso_runtime_staged_route_at(
                host->runtime, index, &offered
            ) != MEISO_RUNTIME_OK ||
            !meiso_endpoint_id_equal(offered.endpoint_id, endpoint.id) ||
            offered.endpoint_incarnation != endpoint.incarnation) {
            return false;
        }
        for (expected_index = 0U;
             expected_index < expected_count;
             ++expected_index) {
            if (meiso_resource_id_equal(
                    offered.resource.id,
                    expected_resources[expected_index].id
                )) {
                if (found[expected_index] ||
                    offered.facet != expected_facets[expected_index] ||
                    !meiso_resource_descriptor_equal(
                        &offered.resource,
                        &expected_resources[expected_index]
                    )) {
                    return false;
                }
                found[expected_index] = true;
                matched = true;
                if (expected_index == 0U) {
                    host->desired_route = route_key(generation, &offered);
                } else if (expected_index == 1U) {
                    host->actual_route = route_key(generation, &offered);
                } else {
                    host->data_route = route_key(generation, &offered);
                }
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }
    return found[0] && found[1] && found[2];
}

static bool receive_and_accept_catalog(Host *host)
{
    struct timespec deadline;
    MeisoRuntimeEvent event;
    MeisoRuntimeSnapshot snapshot;

    if (!deadline_after_ms(&deadline, HOST_PHASE_TIMEOUT_MS) ||
        !wait_runtime_event(host, &deadline, &event) ||
        event.type != MEISO_RUNTIME_EVENT_CATALOG_OFFERED) {
        return false;
    }
    if (!validate_staged_catalog(host, event.catalog_generation)) {
        if (meiso_runtime_reject_catalog(
                host->runtime, MEISO_RUNTIME_CATALOG_REJECT_POLICY
            ) == MEISO_RUNTIME_OK) {
            (void)flush_outbound(host);
        }
        return false;
    }
    if (meiso_runtime_accept_catalog(host->runtime) != MEISO_RUNTIME_OK ||
        !flush_outbound(host) ||
        !meiso_runtime_snapshot(host->runtime, &snapshot) ||
        !snapshot.active_catalog_present ||
        snapshot.active_catalog_generation != event.catalog_generation) {
        return false;
    }
    host->catalog_active = true;
    (void)printf(
        "{\"type\":\"catalog\",\"status\":\"ACCEPTED\","
        "\"generation\":%" PRIu64 ",\"endpoint_count\":1,"
        "\"route_count\":3,\"capability\":{"
        "\"supported_fields\":%" PRIu32 ","
        "\"min_period_ms\":%" PRIu32 ","
        "\"max_period_ms\":%" PRIu32 ","
        "\"period_step_ms\":%" PRIu32 "}}\n",
        event.catalog_generation,
        host->capability.supported_fields,
        host->capability.min_period_ms,
        host->capability.max_period_ms,
        host->capability.period_step_ms
    );
    (void)fflush(stdout);
    return true;
}

static bool publish_desired(Host *host, uint64_t version, bool enabled)
{
    uint8_t profile_bytes[
        MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE
    ];
    uint8_t payload_bytes[
        MEISO_WIFI_TELEMETRY_DESIRED_STATE_MAX_PAYLOAD_SIZE
    ];
    size_t profile_size = 0U;
    MeisoByteView payload;
    MeisoRuntimeDesiredState desired;
    MeisoStateOffer offer;
    MeisoStatePublishStatus publish_status;

    if (host == NULL || !host->catalog_active || version == 0U ||
        meiso_wifi_telemetry_desired_parameters_encode(
            &host->bootstrap.desired,
            (MeisoMutableBuffer){profile_bytes, sizeof(profile_bytes)},
            &profile_size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK) {
        return false;
    }
    desired.enabled = enabled;
    desired.profile = (MeisoByteView){profile_bytes, profile_size};
    if (!meiso_runtime_desired_state_encode(
            &desired,
            (MeisoMutableBuffer){payload_bytes, sizeof(payload_bytes)},
            &payload
        )) {
        return false;
    }
    publish_status = meiso_state_publisher_publish(
        host->desired_publisher, host->host_owner, version, payload
    );
    if ((publish_status != MEISO_STATE_PUBLISH_NEW &&
         publish_status != MEISO_STATE_PUBLISH_REPLACED_PENDING) ||
        !meiso_state_publisher_next_offer(
            host->desired_publisher, &offer
        ) ||
        meiso_runtime_send_state_offer(
            host->runtime, host->desired_route, &offer
        ) != MEISO_RUNTIME_OK || !flush_outbound(host)) {
        return false;
    }
    (void)printf(
        "{\"type\":\"desired\",\"status\":\"PUBLISHED\","
        "\"version\":%" PRIu64 ",\"enabled\":%s,"
        "\"selected_fields\":%" PRIu32 ","
        "\"sample_period_ms\":%" PRIu32 "}\n",
        version,
        enabled ? "true" : "false",
        host->bootstrap.desired.selected_fields,
        host->bootstrap.desired.sample_period_ms
    );
    (void)fflush(stdout);
    return true;
}

static bool handle_desired_accept(
    Host *host,
    const MeisoRuntimeEvent *event,
    uint64_t expected_version
)
{
    MeisoStateAcceptStatus status;

    if (host == NULL || event == NULL ||
        event->type != MEISO_RUNTIME_EVENT_STATE_ACCEPT ||
        !route_key_equal(event->route, host->desired_route) ||
        event->state_accept_generation.value != 1U ||
        !meiso_owner_scope_equal(
            event->state_accept_owner, host->host_owner
        ) || event->state_accept_version != expected_version) {
        return false;
    }
    status = meiso_state_publisher_mark_delivery_accepted(
        host->desired_publisher,
        event->state_accept_generation,
        event->state_accept_owner,
        event->state_accept_version
    );
    if (status != MEISO_STATE_ACCEPT_CONFIRMED &&
        status != MEISO_STATE_ACCEPT_DUPLICATE) {
        return false;
    }
    (void)printf(
        "{\"type\":\"desired_delivery\","
        "\"status\":\"ACCEPTED\",\"version\":%" PRIu64 "}\n",
        expected_version
    );
    (void)fflush(stdout);
    return true;
}

static const char *production_status_name(MeisoRuntimeProductionStatus status)
{
    switch (status) {
    case MEISO_RUNTIME_PRODUCTION_STOPPED:
        return "STOPPED";
    case MEISO_RUNTIME_PRODUCTION_STARTING:
        return "STARTING";
    case MEISO_RUNTIME_PRODUCTION_RUNNING:
        return "RUNNING";
    case MEISO_RUNTIME_PRODUCTION_DEGRADED:
        return "DEGRADED";
    case MEISO_RUNTIME_PRODUCTION_STOPPING:
        return "STOPPING";
    case MEISO_RUNTIME_PRODUCTION_FAILED:
        return "FAILED";
    default:
        return "INVALID";
    }
}

static const char *apply_result_name(MeisoRuntimeApplyResult result)
{
    switch (result) {
    case MEISO_RUNTIME_APPLY_NONE:
        return "NONE";
    case MEISO_RUNTIME_APPLY_PENDING:
        return "PENDING";
    case MEISO_RUNTIME_APPLY_APPLIED:
        return "APPLIED";
    case MEISO_RUNTIME_APPLY_REJECTED:
        return "REJECTED";
    case MEISO_RUNTIME_APPLY_FAILED:
        return "FAILED";
    default:
        return "INVALID";
    }
}

static bool desired_reference_matches(
    MeisoRuntimeDesiredReference reference,
    MeisoOwnerScope owner,
    uint64_t version
)
{
    return reference.present &&
        meiso_owner_scope_equal(reference.owner, owner) &&
        reference.version == version;
}

static bool effective_parameters_supported(
    const Host *host,
    MeisoWifiTelemetryEffectiveParameters effective
)
{
    const MeisoWifiTelemetryDesiredParameters parameters = {
        effective.selected_fields, effective.sample_period_ms
    };

    if (effective.selected_fields == 0U || effective.sample_period_ms == 0U) {
        return effective.selected_fields == 0U &&
            effective.sample_period_ms == 0U;
    }
    return meiso_wifi_telemetry_parameters_supported(
        &host->capability, &parameters
    );
}

static bool handle_actual_offer(
    Host *host,
    const MeisoRuntimeEvent *event,
    uint64_t desired_version,
    MeisoRuntimeProductionStatus target_status,
    bool *out_target_reached
)
{
    MeisoRuntimeActualState actual;
    MeisoWifiTelemetryEffectiveParameters effective;
    MeisoStateReceiveStatus receive_status;
    MeisoRuntimeProductionSnapshot production;
    MeisoRuntimeRouteKey received_route;
    MeisoDeliveryGeneration delivery;
    MeisoOwnerScope owner;
    uint64_t state_version;
    bool target_reached = false;

    if (host == NULL || event == NULL || out_target_reached == NULL ||
        event->type != MEISO_RUNTIME_EVENT_STATE_OFFER ||
        !route_key_equal(event->route, host->actual_route) ||
        !meiso_owner_scope_equal(
            event->state_offer.state.owner, host->edge_owner
        ) || !meiso_runtime_actual_state_decode(
            event->state_offer.state.payload, &actual
        ) || !desired_reference_matches(
            actual.observed_desired, host->host_owner, desired_version
        ) ||
        meiso_wifi_telemetry_effective_parameters_decode(
            actual.profile, &effective
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        !effective_parameters_supported(host, effective)) {
        return false;
    }

    received_route = event->route;
    delivery = event->state_offer.generation;
    owner = event->state_offer.state.owner;
    state_version = event->state_offer.state.version;
    receive_status = meiso_state_replica_receive(
        host->actual_replica,
        owner,
        state_version,
        event->state_offer.state.payload
    );
    if (meiso_runtime_commit_state_receive(
            host->runtime,
            received_route,
            &event->state_offer,
            receive_status
        ) != MEISO_RUNTIME_OK ||
        (receive_status != MEISO_STATE_RECEIVE_NEW &&
         receive_status != MEISO_STATE_RECEIVE_DUPLICATE) ||
        meiso_runtime_production_snapshot(
            host->runtime,
            received_route.endpoint_id,
            &production
        ) != MEISO_RUNTIME_OK ||
        meiso_runtime_send_state_accept(
            host->runtime,
            received_route,
            delivery,
            owner,
            state_version
        ) != MEISO_RUNTIME_OK || !flush_outbound(host)) {
        return false;
    }

    host->production_active = production.data_active;
    if (production.data_active) {
        if (!host->active_effective_present ||
            host->running_production_epoch != production.production_epoch) {
            host->active_effective = effective;
            host->active_effective_present = true;
            host->stream_producer_active = false;
        } else if (host->active_effective.selected_fields !=
                effective.selected_fields ||
            host->active_effective.sample_period_ms !=
                effective.sample_period_ms) {
            return false;
        }
        host->running_production_epoch = production.production_epoch;
    } else {
        memset(&host->active_effective, 0, sizeof(host->active_effective));
        host->active_effective_present = false;
        host->stream_producer_active = false;
    }
    if (target_status == MEISO_RUNTIME_PRODUCTION_RUNNING) {
        target_reached =
            actual.production_status == MEISO_RUNTIME_PRODUCTION_RUNNING &&
            actual.apply_result == MEISO_RUNTIME_APPLY_APPLIED &&
            actual.availability == MEISO_RUNTIME_AVAILABILITY_PRESENT &&
            actual.effective_enabled &&
            actual.reason == MEISO_RUNTIME_PRODUCTION_REASON_NONE &&
            desired_reference_matches(
                actual.applied_desired, host->host_owner, desired_version
            ) &&
            effective.selected_fields ==
                host->bootstrap.desired.selected_fields &&
            effective.sample_period_ms ==
                host->bootstrap.desired.sample_period_ms &&
            production.data_active && production.production_epoch != 0U;
    } else if (target_status == MEISO_RUNTIME_PRODUCTION_STOPPED) {
        target_reached =
            actual.production_status == MEISO_RUNTIME_PRODUCTION_STOPPED &&
            actual.apply_result == MEISO_RUNTIME_APPLY_APPLIED &&
            !actual.effective_enabled &&
            actual.reason == MEISO_RUNTIME_PRODUCTION_REASON_DISABLED &&
            desired_reference_matches(
                actual.applied_desired, host->host_owner, desired_version
            ) && effective.selected_fields == 0U &&
            effective.sample_period_ms == 0U &&
            !production.data_active && production.production_epoch == 0U;
    }
    if ((actual.apply_result == MEISO_RUNTIME_APPLY_REJECTED ||
         actual.apply_result == MEISO_RUNTIME_APPLY_FAILED ||
         actual.production_status == MEISO_RUNTIME_PRODUCTION_FAILED) &&
        !target_reached) {
        return false;
    }
    (void)printf(
        "{\"type\":\"actual\",\"status\":\"%s\","
        "\"state_version\":%" PRIu64 ","
        "\"observed_desired_version\":%" PRIu64 ","
        "\"apply_result\":\"%s\","
        "\"effective_enabled\":%s,\"data_active\":%s,"
        "\"selected_fields\":%" PRIu32 ","
        "\"sample_period_ms\":%" PRIu32 ","
        "\"production_epoch_known\":%s}\n",
        production_status_name(actual.production_status),
        state_version,
        desired_version,
        apply_result_name(actual.apply_result),
        actual.effective_enabled ? "true" : "false",
        production.data_active ? "true" : "false",
        effective.selected_fields,
        effective.sample_period_ms,
        production.production_epoch != 0U ? "true" : "false"
    );
    (void)fflush(stdout);
    *out_target_reached = target_reached;
    return true;
}

static bool field_present(
    const MeisoWifiTelemetrySample *sample,
    MeisoWifiTelemetryFieldMask field
)
{
    return (sample->present_fields & field) != 0U;
}

static void emit_nullable_u64(bool present, uint64_t value)
{
    if (present) {
        (void)printf("%" PRIu64, value);
    } else {
        fputs("null", stdout);
    }
}

static void emit_sample(
    uint64_t ordinal,
    const MeisoStreamSample *stream_sample,
    const MeisoWifiTelemetrySample *sample,
    const MeisoStreamReceiveResult *receive_result
)
{
    const bool link_present = field_present(
        sample, MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE
    );
    const bool signal_present = field_present(
        sample, MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM
    );

    (void)printf(
        "{\"type\":\"sample\",\"ordinal\":%" PRIu64 ","
        "\"sequence\":%" PRIu64 ","
        "\"source_time_ns\":%" PRIu64 ","
        "\"selected_fields\":%" PRIu32 ","
        "\"present_fields\":%" PRIu32 ","
        "\"gap\":%s,\"missing_before\":%" PRIu64 ","
        "\"link_state\":",
        ordinal,
        stream_sample->key.sequence,
        stream_sample->source_time.monotonic_ns,
        sample->selected_fields,
        sample->present_fields,
        (receive_result->flags & MEISO_STREAM_RECEIVE_FLAG_GAP) != 0U ?
            "true" : "false",
        receive_result->missing_before
    );
    if (link_present) {
        fputs(sample->link_state ? "true" : "false", stdout);
    } else {
        fputs("null", stdout);
    }
    fputs(",\"signal_mbm\":", stdout);
    if (signal_present) {
        (void)printf("%" PRId32, sample->signal_mbm);
    } else {
        fputs("null", stdout);
    }
    fputs(",\"rx_bytes\":", stdout);
    emit_nullable_u64(
        field_present(sample, MEISO_WIFI_TELEMETRY_FIELD_RX_BYTES),
        sample->rx_bytes
    );
    fputs(",\"tx_bytes\":", stdout);
    emit_nullable_u64(
        field_present(sample, MEISO_WIFI_TELEMETRY_FIELD_TX_BYTES),
        sample->tx_bytes
    );
    fputs(",\"rx_packets\":", stdout);
    emit_nullable_u64(
        field_present(sample, MEISO_WIFI_TELEMETRY_FIELD_RX_PACKETS),
        sample->rx_packets
    );
    fputs(",\"tx_packets\":", stdout);
    emit_nullable_u64(
        field_present(sample, MEISO_WIFI_TELEMETRY_FIELD_TX_PACKETS),
        sample->tx_packets
    );
    fputs(",\"rx_dropped\":", stdout);
    emit_nullable_u64(
        field_present(sample, MEISO_WIFI_TELEMETRY_FIELD_RX_DROPPED),
        sample->rx_dropped
    );
    fputs(",\"tx_dropped\":", stdout);
    emit_nullable_u64(
        field_present(sample, MEISO_WIFI_TELEMETRY_FIELD_TX_DROPPED),
        sample->tx_dropped
    );
    fputs(",\"rx_errors\":", stdout);
    emit_nullable_u64(
        field_present(sample, MEISO_WIFI_TELEMETRY_FIELD_RX_ERRORS),
        sample->rx_errors
    );
    fputs(",\"tx_errors\":", stdout);
    emit_nullable_u64(
        field_present(sample, MEISO_WIFI_TELEMETRY_FIELD_TX_ERRORS),
        sample->tx_errors
    );
    fputs("}\n", stdout);
    (void)fflush(stdout);
}

static bool handle_stream_sample(
    Host *host,
    const MeisoRuntimeEvent *event
)
{
    MeisoStreamProducerStatus producer_status;
    MeisoStreamReceiveResult receive_result;
    MeisoStreamSample stored;
    MeisoWifiTelemetrySample decoded;

    if (host == NULL || event == NULL ||
        event->type != MEISO_RUNTIME_EVENT_STREAM_SAMPLE ||
        !route_key_equal(event->route, host->data_route) ||
        !host->production_active || host->running_production_epoch == 0U ||
        !host->active_effective_present ||
        event->production_epoch != host->running_production_epoch ||
        !meiso_owner_scope_equal(
            event->stream_sample.key.producer, host->edge_owner
        ) || event->stream_sample.sender_deadline.present ||
        event->stream_sample.dependency.size != 0U ||
        host->samples_received == UINT64_MAX ||
        (!host->interactive && host->samples_received >=
            (uint64_t)(HOST_MAX_SAMPLES + HOST_MAX_STOPPING_SAMPLES))) {
        return false;
    }
    if (!host->stream_producer_active) {
        producer_status = meiso_stream_begin_producer(
            host->stream,
            event->stream_sample.key.producer,
            event->stream_sample.key.epoch
        );
        if (producer_status != MEISO_STREAM_PRODUCER_STARTED &&
            producer_status != MEISO_STREAM_PRODUCER_SWITCHED) {
            return false;
        }
        host->stream_producer = event->stream_sample.key.producer;
        host->stream_epoch = event->stream_sample.key.epoch;
        host->stream_producer_active = true;
    } else if (!meiso_owner_scope_equal(
            event->stream_sample.key.producer, host->stream_producer
        ) || event->stream_sample.key.epoch != host->stream_epoch) {
        return false;
    }
    receive_result = meiso_stream_receive(
        host->stream, &event->stream_sample, NULL
    );
    if (receive_result.code != MEISO_STREAM_RECEIVE_ACCEPTED ||
        !meiso_stream_peek(host->stream, &stored) ||
        meiso_wifi_telemetry_sample_decode(
            stored.payload, &decoded
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        decoded.selected_fields != host->active_effective.selected_fields) {
        return false;
    }
    host->samples_received += 1U;
    emit_sample(
        host->samples_received, &stored, &decoded, &receive_result
    );
    return meiso_stream_pop(host->stream);
}

static bool wait_desired_and_actual(
    Host *host,
    uint64_t desired_version,
    MeisoRuntimeProductionStatus target_status
)
{
    struct timespec deadline;
    bool desired_accepted = false;
    bool target_reached = false;

    if (!deadline_after_ms(&deadline, HOST_PHASE_TIMEOUT_MS)) {
        return false;
    }
    while (!desired_accepted || !target_reached) {
        MeisoRuntimeEvent event;

        if (!wait_runtime_event(host, &deadline, &event)) {
            return false;
        }
        if (event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT) {
            if (!handle_desired_accept(
                    host, &event, desired_version
                )) {
                return false;
            }
            desired_accepted = true;
        } else if (event.type == MEISO_RUNTIME_EVENT_STATE_OFFER) {
            bool reached = false;
            if (!handle_actual_offer(
                    host,
                    &event,
                    desired_version,
                    target_status,
                    &reached
                )) {
                return false;
            }
            target_reached = reached;
        } else if (event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE) {
            if (!handle_stream_sample(host, &event) ||
                (!host->interactive &&
                 target_status == MEISO_RUNTIME_PRODUCTION_STOPPED &&
                 host->samples_received >
                    (uint64_t)host->bootstrap.sample_count +
                        HOST_MAX_STOPPING_SAMPLES)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

static bool receive_requested_samples(Host *host)
{
    struct timespec deadline;
    const uint64_t wait_ms =
        (uint64_t)host->bootstrap.desired.sample_period_ms *
            (uint64_t)host->bootstrap.sample_count +
        HOST_PHASE_TIMEOUT_MS;

    if (!deadline_after_ms(&deadline, wait_ms)) {
        return false;
    }
    while (host->samples_received < host->bootstrap.sample_count) {
        MeisoRuntimeEvent event;

        if (!wait_runtime_event(host, &deadline, &event)) {
            return false;
        }
        if (event.type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE) {
            if (!handle_stream_sample(host, &event)) {
                return false;
            }
        } else if (event.type == MEISO_RUNTIME_EVENT_STATE_OFFER) {
            bool running = false;
            if (!handle_actual_offer(
                    host,
                    &event,
                    1U,
                    MEISO_RUNTIME_PRODUCTION_RUNNING,
                    &running
                ) || !host->production_active) {
                return false;
            }
        } else if (event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT) {
            if (!handle_desired_accept(host, &event, 1U)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

static bool control_consume_literal(
    const char **cursor,
    const char *end,
    const char *literal
)
{
    const size_t length = literal != NULL ? strlen(literal) : 0U;

    if (cursor == NULL || *cursor == NULL || end == NULL ||
        literal == NULL || *cursor > end ||
        length > (size_t)(end - *cursor) ||
        memcmp(*cursor, literal, length) != 0) {
        return false;
    }
    *cursor += length;
    return true;
}

static bool control_parse_u32(
    const char **cursor,
    const char *end,
    uint32_t *out_value
)
{
    char *parsed_end = NULL;
    unsigned long long value;
    size_t digits;

    if (cursor == NULL || *cursor == NULL || end == NULL ||
        out_value == NULL || *cursor >= end) {
        return false;
    }
    digits = strspn(*cursor, "0123456789");
    if (digits == 0U || digits > (size_t)(end - *cursor)) {
        return false;
    }
    errno = 0;
    value = strtoull(*cursor, &parsed_end, 10);
    if (errno != 0 || parsed_end != *cursor + digits ||
        value > (unsigned long long)UINT32_MAX) {
        return false;
    }
    *cursor = parsed_end;
    *out_value = (uint32_t)value;
    return true;
}

static HostControlCommand parse_control_command(const char *line)
{
    HostControlCommand command;
    const char *cursor = line;
    const char *end;

    memset(&command, 0, sizeof(command));
    if (line == NULL) {
        return command;
    }
    end = line + strlen(line);
    if (strcmp(line, "{\"command\":\"close\"}") == 0) {
        command.type = HOST_CONTROL_CLOSE;
        return command;
    }
    if (!control_consume_literal(
            &cursor, end, "{\"command\":\"apply\",\"enabled\":"
        )) {
        return command;
    }
    if (control_consume_literal(&cursor, end, "true")) {
        command.enabled = true;
    } else if (control_consume_literal(&cursor, end, "false")) {
        command.enabled = false;
    } else {
        return command;
    }
    if (!control_consume_literal(
            &cursor, end, ",\"selected_fields\":"
        ) ||
        !control_parse_u32(
            &cursor, end, &command.parameters.selected_fields
        ) || !control_consume_literal(
            &cursor, end, ",\"sample_period_ms\":"
        ) || !control_parse_u32(
            &cursor, end, &command.parameters.sample_period_ms
        ) || !control_consume_literal(&cursor, end, "}") || cursor != end) {
        memset(&command, 0, sizeof(command));
        return command;
    }
    command.type = HOST_CONTROL_APPLY;
    return command;
}

static bool extract_control_command(
    HostControlInput *input,
    HostControlCommand *out_command,
    bool *out_present
)
{
    size_t index;
    char line[HOST_CONTROL_LINE_BYTES];

    if (input == NULL || out_command == NULL || out_present == NULL) {
        return false;
    }
    *out_present = false;
    for (index = 0U; index < input->size; ++index) {
        if (input->bytes[index] != '\n') {
            continue;
        }
        if (index >= sizeof(line)) {
            return false;
        }
        memcpy(line, input->bytes, index);
        line[index] = '\0';
        memmove(
            input->bytes,
            input->bytes + index + 1U,
            input->size - index - 1U
        );
        input->size -= index + 1U;
        *out_command = parse_control_command(line);
        *out_present = true;
        return true;
    }
    return input->size < sizeof(input->bytes);
}

static HostInteractiveItemType wait_interactive_item(
    Host *host,
    HostControlInput *input,
    HostControlCommand *out_command,
    MeisoRuntimeEvent *out_event
)
{
    if (host == NULL || input == NULL || out_command == NULL ||
        out_event == NULL) {
        return HOST_INTERACTIVE_ITEM_FAILED;
    }
    for (;;) {
        MeisoSessionSnapshot snapshot;
        struct pollfd descriptors[2];
        bool command_present = false;
        int result;

        if (!extract_control_command(
                input, out_command, &command_present
            )) {
            return HOST_INTERACTIVE_ITEM_FAILED;
        }
        if (command_present) {
            return HOST_INTERACTIVE_ITEM_COMMAND;
        }
        if (!meiso_session_snapshot(host->session, &snapshot) ||
            snapshot.state == MEISO_SESSION_STATE_FAILED ||
            snapshot.state == MEISO_SESSION_STATE_CLOSED) {
            return HOST_INTERACTIVE_ITEM_FAILED;
        }
        descriptors[0].fd = host->fd;
        descriptors[0].events = 0;
        descriptors[0].revents = 0;
        if (meiso_posix_tcp_wants_read(host->adapter)) {
            descriptors[0].events = (short)(
                descriptors[0].events | POLLIN
            );
        }
        if (meiso_posix_tcp_wants_write(host->adapter)) {
            descriptors[0].events = (short)(
                descriptors[0].events | POLLOUT
            );
        }
        descriptors[1].fd = STDIN_FILENO;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, (nfds_t)2U, -1);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0 ||
            (descriptors[0].revents & POLLNVAL) != 0 ||
            (descriptors[1].revents & (POLLERR | POLLNVAL)) != 0) {
            return HOST_INTERACTIVE_ITEM_FAILED;
        }
        if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0) {
            const ssize_t count = read(
                STDIN_FILENO,
                input->bytes + input->size,
                sizeof(input->bytes) - input->size
            );
            if (count > 0) {
                input->size += (size_t)count;
                continue;
            }
            if (count < 0 &&
                (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            return HOST_INTERACTIVE_ITEM_FAILED;
        }
        if ((descriptors[0].revents & POLLOUT) != 0 &&
            !write_status_progress(
                meiso_posix_tcp_on_writable(host->adapter)
            )) {
            return HOST_INTERACTIVE_ITEM_FAILED;
        }
        if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            MeisoSessionRuntimeMessage message;
            const MeisoPosixTcpReadStatus read_status =
                meiso_posix_tcp_on_readable(host->adapter, &message);

            if (read_status == MEISO_POSIX_TCP_READ_RUNTIME) {
                if (meiso_runtime_receive(
                        host->runtime, &message, out_event
                    ) != MEISO_RUNTIME_OK) {
                    return HOST_INTERACTIVE_ITEM_FAILED;
                }
                return HOST_INTERACTIVE_ITEM_RUNTIME;
            }
            if (!read_status_progress(read_status)) {
                return HOST_INTERACTIVE_ITEM_FAILED;
            }
        }
    }
}

static bool handle_interactive_runtime_event(
    Host *host,
    const MeisoRuntimeEvent *event,
    uint64_t desired_version,
    bool desired_enabled
)
{
    if (host == NULL || event == NULL || desired_version == 0U) {
        return false;
    }
    if (event->type == MEISO_RUNTIME_EVENT_STREAM_SAMPLE) {
        return handle_stream_sample(host, event);
    }
    if (event->type == MEISO_RUNTIME_EVENT_STATE_ACCEPT) {
        return handle_desired_accept(host, event, desired_version);
    }
    if (event->type == MEISO_RUNTIME_EVENT_STATE_OFFER) {
        bool target_reached = false;
        return handle_actual_offer(
            host,
            event,
            desired_version,
            desired_enabled ? MEISO_RUNTIME_PRODUCTION_RUNNING :
                MEISO_RUNTIME_PRODUCTION_STOPPED,
            &target_reached
        );
    }
    return false;
}

static void emit_control_rejected(const char *reason)
{
    (void)printf(
        "{\"type\":\"control\",\"status\":\"REJECTED\","
        "\"reason\":\"%s\"}\n",
        reason
    );
    (void)fflush(stdout);
}

static bool emit_control_applied(uint64_t version, bool enabled)
{
    if (version == 0U) {
        return false;
    }
    (void)printf(
        "{\"type\":\"control\",\"command\":\"APPLY\","
        "\"status\":\"APPLIED\",\"desired_version\":%" PRIu64 ","
        "\"enabled\":%s}\n",
        version,
        enabled ? "true" : "false"
    );
    (void)fflush(stdout);
    return true;
}

static void select_close_parameters(Host *host)
{
    if (meiso_wifi_telemetry_parameters_supported(
            &host->capability, &host->bootstrap.desired
        )) {
        return;
    }
    host->bootstrap.desired.selected_fields =
        host->capability.supported_fields;
    host->bootstrap.desired.sample_period_ms =
        host->capability.min_period_ms;
}

static bool normal_close(Host *host);

static bool run_interactive(Host *host)
{
    HostControlInput input;
    uint64_t desired_version = 0U;
    bool desired_enabled = false;

    if (host == NULL) {
        return false;
    }
    memset(&input, 0, sizeof(input));
    (void)printf(
        "{\"type\":\"control\",\"status\":\"READY\","
        "\"mode\":\"INTERACTIVE\"}\n"
    );
    (void)fflush(stdout);
    for (;;) {
        HostControlCommand command;
        MeisoRuntimeEvent event;
        const HostInteractiveItemType item = wait_interactive_item(
            host, &input, &command, &event
        );

        if (item == HOST_INTERACTIVE_ITEM_FAILED) {
            return false;
        }
        if (item == HOST_INTERACTIVE_ITEM_RUNTIME) {
            if (!handle_interactive_runtime_event(
                    host, &event, desired_version, desired_enabled
                )) {
                return false;
            }
            continue;
        }
        if (command.type == HOST_CONTROL_INVALID) {
            emit_control_rejected("INVALID_COMMAND");
            continue;
        }
        if (command.type == HOST_CONTROL_APPLY) {
            if (!meiso_wifi_telemetry_parameters_supported(
                    &host->capability, &command.parameters
                )) {
                emit_control_rejected("UNSUPPORTED_PARAMETERS");
                continue;
            }
            if (desired_version == UINT64_MAX) {
                return false;
            }
            desired_version += 1U;
            host->bootstrap.desired = command.parameters;
            if (!publish_desired(
                    host, desired_version, command.enabled
                ) || !wait_desired_and_actual(
                    host,
                    desired_version,
                    command.enabled ? MEISO_RUNTIME_PRODUCTION_RUNNING :
                        MEISO_RUNTIME_PRODUCTION_STOPPED
                ) || !emit_control_applied(
                    desired_version, command.enabled
                )) {
                return false;
            }
            desired_enabled = command.enabled;
            continue;
        }
        if (desired_version == 0U || desired_enabled) {
            if (desired_version == UINT64_MAX) {
                return false;
            }
            select_close_parameters(host);
            desired_version += 1U;
            if (!publish_desired(host, desired_version, false) ||
                !wait_desired_and_actual(
                    host,
                    desired_version,
                    MEISO_RUNTIME_PRODUCTION_STOPPED
                )) {
                return false;
            }
        }
        if (!normal_close(host)) {
            return false;
        }
        (void)printf(
            "{\"type\":\"control\",\"command\":\"CLOSE\","
            "\"status\":\"COMPLETE\"}\n"
        );
        (void)fflush(stdout);
        return true;
    }
}

static bool normal_close(Host *host)
{
    MeisoSessionSnapshot snapshot;

    if (meiso_session_close(
            host->session, MEISO_SESSION_CLOSE_NORMAL
        ) != MEISO_SESSION_CLOSE_QUEUED || !flush_outbound(host) ||
        !meiso_session_snapshot(host->session, &snapshot) ||
        snapshot.state != MEISO_SESSION_STATE_CLOSED ||
        snapshot.failure != MEISO_SESSION_FAILURE_LOCAL_CLOSED ||
        !snapshot.local_close_reason_known ||
        snapshot.local_close_reason != MEISO_SESSION_CLOSE_NORMAL) {
        return false;
    }
    (void)printf(
        "{\"type\":\"session\",\"status\":\"CLOSED\","
        "\"reason\":\"NORMAL\"}\n"
    );
    (void)fflush(stdout);
    return true;
}

static void best_effort_close(Host *host)
{
    MeisoSessionSnapshot snapshot;

    if (host == NULL || host->session == NULL ||
        !meiso_session_snapshot(host->session, &snapshot) ||
        snapshot.state == MEISO_SESSION_STATE_FAILED ||
        snapshot.state == MEISO_SESSION_STATE_CLOSED) {
        return;
    }
    if (meiso_session_close(
            host->session, MEISO_SESSION_CLOSE_PROTOCOL_ERROR
        ) == MEISO_SESSION_CLOSE_QUEUED) {
        (void)flush_outbound(host);
    }
}

static void emit_failure(const char *stage)
{
    (void)printf(
        "{\"type\":\"failure\",\"status\":\"FAILED\","
        "\"stage\":\"%s\"}\n",
        stage
    );
    (void)fflush(stdout);
}

static void usage(const char *program)
{
    (void)fprintf(
        stderr,
        "usage: %s --config PATH [--interactive]\n"
        "always required: connect_ipv4, connect_port, "
        "local_host_peer_id_high/low, expected_edge_peer_id_high/low, "
        "one-shot also requires: selected_fields, sample_period_ms, "
        "sample_count\n",
        program
    );
}

int main(int argc, char **argv)
{
    Host host;
    MeisoSessionSnapshot session_snapshot;
    const char *failure_stage;
    bool passed = false;

    memset(&host, 0, sizeof(host));
    host.fd = -1;
    if ((argc != 3 && argc != 4) ||
        strcmp(argv[1], "--config") != 0 ||
        (argc == 4 && strcmp(argv[3], "--interactive") != 0)) {
        usage(argv[0]);
        return 2;
    }
    host.interactive = argc == 4;
    if (!load_config(argv[2], host.interactive, &host.bootstrap)) {
        fputs("wifi host: invalid config\n", stderr);
        return 2;
    }
    host.fd = connect_numeric_ipv4(&host.bootstrap);
    if (host.fd < 0 || !initialize_session_transport(&host)) {
        fputs("wifi host: bootstrap failed\n", stderr);
        close_fd(&host.fd);
        return 1;
    }
    (void)printf(
        "{\"type\":\"bootstrap\",\"status\":\"CONNECTED\","
        "\"locator_redacted\":true}\n"
    );
    (void)fflush(stdout);

    failure_stage = "session_ready";
    if (!wait_session_ready(&host, &session_snapshot)) {
        goto done;
    }
    failure_stage = "runtime_init";
    if (!initialize_runtime_resources(&host, &session_snapshot)) {
        goto done;
    }
    emit_session_ready(&session_snapshot);

    failure_stage = "catalog";
    if (!receive_and_accept_catalog(&host)) {
        goto done;
    }
    if (host.interactive) {
        failure_stage = "interactive";
        if (!run_interactive(&host)) {
            goto done;
        }
        passed = true;
        goto done;
    }
    failure_stage = "enable_desired";
    if (!publish_desired(&host, 1U, true) ||
        !wait_desired_and_actual(
            &host, 1U, MEISO_RUNTIME_PRODUCTION_RUNNING
        )) {
        goto done;
    }
    failure_stage = "samples";
    if (!receive_requested_samples(&host)) {
        goto done;
    }
    failure_stage = "disable_desired";
    if (!publish_desired(&host, 2U, false) ||
        !wait_desired_and_actual(
            &host, 2U, MEISO_RUNTIME_PRODUCTION_STOPPED
        )) {
        goto done;
    }
    failure_stage = "normal_close";
    if (!normal_close(&host)) {
        goto done;
    }
    (void)printf(
        "{\"type\":\"run\",\"status\":\"COMPLETE\","
        "\"requested_samples\":%" PRIu32 ","
        "\"received_samples\":%" PRIu64 "}\n",
        host.bootstrap.sample_count,
        host.samples_received
    );
    (void)fflush(stdout);
    passed = true;

done:
    if (!passed) {
        emit_failure(failure_stage);
        best_effort_close(&host);
    }
    close_fd(&host.fd);
    return passed ? 0 : 1;
}
