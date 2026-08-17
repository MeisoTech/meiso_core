#define _POSIX_C_SOURCE 200809L

#include "meiso_posix_tcp.h"
#include "meiso_runtime.h"
#include "meiso_session.h"
#include "meiso_state.h"
#include "wifi_linux_adapter.h"
#include "wifi_telemetry.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
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

enum {
    EDGE_MAX_FRAME_BYTES = 4096,
    EDGE_MAX_RUNTIME_PAYLOAD_BYTES = 2048,
    EDGE_MAX_REGISTRY_ENTRIES = 16,
    EDGE_IO_BYTES_PER_STEP = 1024,
    EDGE_SESSION_ARENA_BYTES = 8192,
    EDGE_TRANSPORT_ARENA_BYTES = 16384,
    EDGE_RUNTIME_ARENA_BYTES = 32768,
    EDGE_STATE_ARENA_BYTES = 4096,
    EDGE_CONFIG_LINE_BYTES = 256,
    EDGE_POLL_SLICE_MS = 100,
    EDGE_PHASE_TIMEOUT_MS = 10000,
    EDGE_CLOSE_TIMEOUT_MS = 2000,
    EDGE_CATALOG_GENERATION = 1,
    EDGE_STATE_DELIVERY_GENERATION = 1
};

typedef union {
    max_align_t alignment;
    uint8_t bytes[EDGE_SESSION_ARENA_BYTES];
} SessionArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[EDGE_TRANSPORT_ARENA_BYTES];
} TransportArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[EDGE_RUNTIME_ARENA_BYTES];
} RuntimeArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[EDGE_STATE_ARENA_BYTES];
} StateArena;

typedef struct {
    SessionArena session;
    TransportArena transport;
    RuntimeArena runtime;
    StateArena desired_replica;
    StateArena actual_publisher;
} EdgeStorage;

typedef struct {
    char listen_ipv4[INET_ADDRSTRLEN];
    char interface_name[MEISO_WIFI_LINUX_INTERFACE_NAME_CAPACITY];
    MeisoPeerId local_edge_peer_id;
    MeisoPeerId expected_host_peer_id;
    uint16_t listen_port;
    MeisoWifiTelemetryFieldMask allowed_fields;
    uint32_t min_period_ms;
    uint32_t max_period_ms;
    uint32_t period_step_ms;
} EdgePrivateConfig;

typedef struct {
    bool present;
    MeisoRuntimeRouteKey route;
    MeisoDeliveryGeneration generation;
    MeisoOwnerScope owner;
    uint64_t version;
} PendingStateAccept;

typedef enum {
    ACTUAL_FOLLOW_UP_NONE = 0,
    ACTUAL_FOLLOW_UP_STOPPED,
    ACTUAL_FOLLOW_UP_RUNNING
} ActualFollowUp;

typedef struct {
    bool present;
    bool published;
    MeisoRuntimeDesiredReference observed;
    MeisoRuntimeDesiredReference applied;
    MeisoRuntimeApplyResult apply_result;
    MeisoRuntimeAvailability availability;
    bool effective_enabled;
    MeisoRuntimeProductionStatus production_status;
    MeisoRuntimeProductionReason reason;
    uint64_t production_epoch;
    MeisoWifiTelemetryEffectiveParameters effective;
    ActualFollowUp follow_up;
    MeisoWifiTelemetryDesiredParameters follow_up_parameters;
} ActualTransition;

typedef struct {
    bool present;
    uint64_t production_epoch;
    uint8_t payload[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    MeisoStreamSample sample;
} PendingSample;

typedef struct {
    MeisoWifiTelemetryFieldMask seen_fields;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
    uint64_t rx_errors;
    uint64_t tx_errors;
} CounterContinuity;

typedef struct {
    EdgePrivateConfig private_config;
    MeisoWifiLinuxAdapter wifi;
    MeisoWifiTelemetryCapability capability;
    uint8_t capability_payload[MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE];
    size_t capability_payload_size;

    int listener_fd;
    int connection_fd;
    MeisoSessionConfig session_config;
    MeisoSession *session;
    MeisoPosixTcp *transport;
    MeisoRuntime *runtime;
    MeisoStateReplica *desired_replica;
    MeisoStatePublisher *actual_publisher;
    MeisoOwnerScope edge_owner;
    MeisoOwnerScope host_owner;

    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor routes[MEISO_WIFI_TELEMETRY_RESOURCE_COUNT];
    MeisoRuntimeRouteKey desired_route;
    MeisoRuntimeRouteKey actual_route;
    MeisoRuntimeRouteKey data_route;

    PendingStateAccept pending_accept;
    ActualTransition actual_outbound;
    ActualTransition actual_in_flight_value;
    ActualTransition actual_next;
    PendingSample pending_sample;
    MeisoWifiTelemetryDesiredParameters current_parameters;
    MeisoRuntimeDesiredReference current_desired;
    MeisoRuntimeDesiredReference production_applied_desired;
    MeisoWifiTelemetryEffectiveParameters production_effective;
    CounterContinuity counter_continuity;

    uint64_t actual_version;
    uint64_t production_epoch_high_water;
    uint64_t active_production_epoch;
    uint64_t stream_sequence;
    uint64_t next_sample_ns;
    uint64_t next_probe_ns;
    uint64_t phase_deadline_ns;
    uint64_t samples_queued;
    uint64_t clock_epoch;

    bool runtime_initialized;
    bool catalog_offered;
    bool catalog_active;
    bool actual_in_flight;
    bool desired_profile_valid;
    bool desired_enabled;
    bool production_active;
    bool production_ever_started;
    bool source_continuity_lost;
    bool peer_closed_normally;
} Edge;

static EdgeStorage g_storage;
static volatile sig_atomic_t g_stop_requested = 0;

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    g_stop_requested = 1;
}

static bool install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    if (sigemptyset(&action.sa_mask) != 0) {
        return false;
    }
    return sigaction(SIGINT, &action, NULL) == 0 &&
        sigaction(SIGTERM, &action, NULL) == 0;
}

static void close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static bool monotonic_now_ns(uint64_t *out_value)
{
    struct timespec value;
    uint64_t seconds;

    if (out_value == NULL || clock_gettime(CLOCK_MONOTONIC, &value) != 0 ||
        value.tv_sec < 0 || value.tv_nsec < 0 ||
        value.tv_nsec >= 1000000000L) {
        return false;
    }
    seconds = (uint64_t)value.tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t)value.tv_nsec) /
            UINT64_C(1000000000)) {
        return false;
    }
    *out_value = seconds * UINT64_C(1000000000) +
        (uint64_t)value.tv_nsec;
    return true;
}

static uint64_t deadline_after_ms(uint64_t now_ns, uint32_t milliseconds)
{
    const uint64_t delta = (uint64_t)milliseconds * UINT64_C(1000000);
    return delta > UINT64_MAX - now_ns ? UINT64_MAX : now_ns + delta;
}

static bool read_entropy(void *output, size_t size)
{
    uint8_t *bytes = output;
    size_t offset = 0U;
    int fd;

    if (output == NULL || size == 0U) {
        return false;
    }
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    while (offset < size) {
        const ssize_t count = read(fd, bytes + offset, size - offset);
        if (count > 0) {
            offset += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            close_fd(&fd);
            return false;
        }
    }
    close_fd(&fd);
    return true;
}

static bool random_nonzero_u64(uint64_t *out_value)
{
    uint64_t value;

    if (out_value == NULL || !read_entropy(&value, sizeof(value))) {
        return false;
    }
    value &= UINT64_C(0x7fffffffffffffff);
    *out_value = value == 0U ? 1U : value;
    return true;
}

static bool random_nonce(MeisoSessionNonce *out_nonce)
{
    if (out_nonce == NULL || !read_entropy(out_nonce, sizeof(*out_nonce))) {
        return false;
    }
    if (!meiso_session_nonce_valid(*out_nonce)) {
        out_nonce->low = 1U;
    }
    return true;
}

static char *trim_text(char *value)
{
    char *start = value;
    char *end;

    while (*start != '\0' && isspace((unsigned char)*start)) {
        start += 1;
    }
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end -= 1;
    }
    *end = '\0';
    return start;
}

static bool copy_config_text(char *output, size_t capacity, const char *value)
{
    const size_t length = value != NULL ? strlen(value) : 0U;
    if (output == NULL || capacity == 0U || length == 0U ||
        length >= capacity) {
        return false;
    }
    memmove(output, value, length + 1U);
    return true;
}

static bool parse_u32(const char *text, int base, uint32_t *out_value)
{
    char *end = NULL;
    uintmax_t value;

    if (text == NULL || out_value == NULL || text[0] == '\0' ||
        text[0] == '-' || text[0] == '+') {
        return false;
    }
    errno = 0;
    value = strtoumax(text, &end, base);
    if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) {
        return false;
    }
    *out_value = (uint32_t)value;
    return true;
}

static bool parse_u64_config(
    const char *text,
    int base,
    uint64_t *out_value
)
{
    char *end = NULL;
    uintmax_t value;

    if (text == NULL || out_value == NULL || text[0] == '\0' ||
        text[0] == '-' || text[0] == '+') {
        return false;
    }
    errno = 0;
    value = strtoumax(text, &end, base);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
#if UINTMAX_MAX > UINT64_MAX
    if (value > UINT64_MAX) {
        return false;
    }
#endif
    *out_value = (uint64_t)value;
    return true;
}

static bool parse_private_config(
    const char *path,
    EdgePrivateConfig *out_config
)
{
    enum {
        HAVE_LISTEN_IPV4 = 1U << 0U,
        HAVE_LISTEN_PORT = 1U << 1U,
        HAVE_INTERFACE = 1U << 2U,
        HAVE_FIELDS = 1U << 3U,
        HAVE_MIN_PERIOD = 1U << 4U,
        HAVE_MAX_PERIOD = 1U << 5U,
        HAVE_PERIOD_STEP = 1U << 6U,
        HAVE_LOCAL_EDGE_HIGH = 1U << 7U,
        HAVE_LOCAL_EDGE_LOW = 1U << 8U,
        HAVE_EXPECTED_HOST_HIGH = 1U << 9U,
        HAVE_EXPECTED_HOST_LOW = 1U << 10U,
        HAVE_ALL = (1U << 11U) - 1U
    };
    EdgePrivateConfig value;
    FILE *file;
    char line[EDGE_CONFIG_LINE_BYTES];
    unsigned int present = 0U;
    bool valid = true;

    if (path == NULL || out_config == NULL) {
        return false;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    while (valid && fgets(line, (int)sizeof(line), file) != NULL) {
        char *entry;
        char *equals;
        char *key;
        char *field_value;
        uint32_t number;

        if (strchr(line, '\n') == NULL && !feof(file)) {
            valid = false;
            break;
        }
        entry = trim_text(line);
        if (*entry == '\0' || *entry == '#') {
            continue;
        }
        equals = strchr(entry, '=');
        if (equals == NULL || strchr(equals + 1, '=') != NULL) {
            valid = false;
            break;
        }
        *equals = '\0';
        key = trim_text(entry);
        field_value = trim_text(equals + 1);
        if (strcmp(key, "listen_ipv4") == 0 &&
            (present & HAVE_LISTEN_IPV4) == 0U &&
            copy_config_text(
                value.listen_ipv4, sizeof(value.listen_ipv4), field_value
            )) {
            present |= HAVE_LISTEN_IPV4;
        } else if (strcmp(key, "listen_port") == 0 &&
                   (present & HAVE_LISTEN_PORT) == 0U &&
                   parse_u32(field_value, 10, &number) && number != 0U &&
                   number <= UINT16_MAX) {
            value.listen_port = (uint16_t)number;
            present |= HAVE_LISTEN_PORT;
        } else if (strcmp(key, "interface") == 0 &&
                   (present & HAVE_INTERFACE) == 0U &&
                   copy_config_text(
                       value.interface_name,
                       sizeof(value.interface_name),
                       field_value
                   )) {
            present |= HAVE_INTERFACE;
        } else if (strcmp(key, "allowed_fields") == 0 &&
                   (present & HAVE_FIELDS) == 0U &&
                   parse_u32(field_value, 0, &number) && number != 0U &&
                   (number & ~MEISO_WIFI_TELEMETRY_FIELD_ALL) == 0U) {
            value.allowed_fields = number;
            present |= HAVE_FIELDS;
        } else if (strcmp(key, "min_period_ms") == 0 &&
                   (present & HAVE_MIN_PERIOD) == 0U &&
                   parse_u32(field_value, 10, &value.min_period_ms)) {
            present |= HAVE_MIN_PERIOD;
        } else if (strcmp(key, "max_period_ms") == 0 &&
                   (present & HAVE_MAX_PERIOD) == 0U &&
                   parse_u32(field_value, 10, &value.max_period_ms)) {
            present |= HAVE_MAX_PERIOD;
        } else if (strcmp(key, "period_step_ms") == 0 &&
                   (present & HAVE_PERIOD_STEP) == 0U &&
                   parse_u32(field_value, 10, &value.period_step_ms)) {
            present |= HAVE_PERIOD_STEP;
        } else if (strcmp(key, "local_edge_peer_id_high") == 0 &&
                   (present & HAVE_LOCAL_EDGE_HIGH) == 0U &&
                   parse_u64_config(
                       field_value, 0, &value.local_edge_peer_id.high
                   )) {
            present |= HAVE_LOCAL_EDGE_HIGH;
        } else if (strcmp(key, "local_edge_peer_id_low") == 0 &&
                   (present & HAVE_LOCAL_EDGE_LOW) == 0U &&
                   parse_u64_config(
                       field_value, 0, &value.local_edge_peer_id.low
                   )) {
            present |= HAVE_LOCAL_EDGE_LOW;
        } else if (strcmp(key, "expected_host_peer_id_high") == 0 &&
                   (present & HAVE_EXPECTED_HOST_HIGH) == 0U &&
                   parse_u64_config(
                       field_value, 0, &value.expected_host_peer_id.high
                   )) {
            present |= HAVE_EXPECTED_HOST_HIGH;
        } else if (strcmp(key, "expected_host_peer_id_low") == 0 &&
                   (present & HAVE_EXPECTED_HOST_LOW) == 0U &&
                   parse_u64_config(
                       field_value, 0, &value.expected_host_peer_id.low
                   )) {
            present |= HAVE_EXPECTED_HOST_LOW;
        } else {
            valid = false;
        }
    }
    if (ferror(file)) {
        (void)fclose(file);
        return false;
    }
    if (fclose(file) != 0) {
        return false;
    }
    if (!valid || present != HAVE_ALL ||
        !meiso_peer_id_valid(value.local_edge_peer_id) ||
        !meiso_peer_id_valid(value.expected_host_peer_id) ||
        meiso_peer_id_equal(
            value.local_edge_peer_id, value.expected_host_peer_id
        ) ||
        inet_pton(AF_INET, value.listen_ipv4, &(struct in_addr){0}) != 1) {
        return false;
    }
    *out_config = value;
    return true;
}

static bool set_cloexec(int fd)
{
    const int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static bool set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int make_listener(const EdgePrivateConfig *config)
{
    struct sockaddr_in address;
    int fd = -1;
    int enabled = 1;

    if (config == NULL) {
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(config->listen_port);
    if (inet_pton(AF_INET, config->listen_ipv4, &address.sin_addr) != 1) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || !set_cloexec(fd) || setsockopt(
            fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &enabled,
            (socklen_t)sizeof(enabled)
        ) != 0 || bind(
            fd,
            (const struct sockaddr *)(const void *)&address,
            (socklen_t)sizeof(address)
        ) != 0 || listen(fd, 1) != 0) {
        close_fd(&fd);
        return -1;
    }
    return fd;
}

static int accept_one(int listener_fd)
{
    int fd;

    for (;;) {
        fd = accept(listener_fd, NULL, NULL);
        if (fd >= 0) {
            if (set_cloexec(fd) && set_nonblocking(fd)) {
                return fd;
            }
            close_fd(&fd);
            return -1;
        }
        if (errno == EINTR && !g_stop_requested) {
            continue;
        }
        return -1;
    }
}

static MeisoRuntimeRouteKey route_key(
    const Edge *edge,
    const MeisoResourceDescriptor *resource,
    MeisoRuntimeFacet facet
)
{
    const MeisoRuntimeRouteKey key = {
        EDGE_CATALOG_GENERATION,
        *meiso_wifi_telemetry_endpoint(),
        edge->session_config.local_incarnation,
        resource->id,
        facet
    };
    return key;
}

static bool initialize_profile(Edge *edge)
{
    MeisoWifiLinuxAdapterConfig adapter_config;
    MeisoWifiTelemetryFieldMask probed_fields = 0U;
    size_t encoded_size = 0U;

    memset(&adapter_config, 0, sizeof(adapter_config));
    memmove(
        adapter_config.interface_name,
        edge->private_config.interface_name,
        strlen(edge->private_config.interface_name) + 1U
    );
    adapter_config.allowed_fields = edge->private_config.allowed_fields;
    if (meiso_wifi_linux_adapter_init(&edge->wifi, &adapter_config) !=
            MEISO_WIFI_LINUX_ADAPTER_OK ||
        meiso_wifi_linux_adapter_probe(&edge->wifi, &probed_fields) !=
            MEISO_WIFI_LINUX_ADAPTER_OK) {
        return false;
    }
    edge->capability.supported_fields =
        edge->private_config.allowed_fields & probed_fields;
    edge->capability.min_period_ms = edge->private_config.min_period_ms;
    edge->capability.max_period_ms = edge->private_config.max_period_ms;
    edge->capability.period_step_ms = edge->private_config.period_step_ms;
    if (edge->capability.supported_fields == 0U ||
        meiso_wifi_telemetry_capability_encode(
            &edge->capability,
            (MeisoMutableBuffer){
                edge->capability_payload,
                sizeof(edge->capability_payload)
            },
            &encoded_size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK) {
        return false;
    }
    edge->capability_payload_size = encoded_size;
    return true;
}

static bool initialize_resources(Edge *edge)
{
    const MeisoResourceDescriptor *desired =
        meiso_wifi_telemetry_resource_for_facet(
            MEISO_RUNTIME_FACET_DESIRED
        );
    const MeisoResourceDescriptor *actual =
        meiso_wifi_telemetry_resource_for_facet(
            MEISO_RUNTIME_FACET_ACTUAL
        );
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    size_t required;

    if (desired == NULL || actual == NULL) {
        return false;
    }
    required = meiso_state_replica_memory_required(desired);
    if (required == 0U || required > sizeof(g_storage.desired_replica.bytes) ||
        meiso_state_replica_init(
            g_storage.desired_replica.bytes,
            required,
            desired,
            &edge->desired_replica
        ) != MEISO_STATE_INIT_OK) {
        return false;
    }
    required = meiso_state_publisher_memory_required(actual, &state_config);
    if (required == 0U ||
        required > sizeof(g_storage.actual_publisher.bytes) ||
        meiso_state_publisher_init(
            g_storage.actual_publisher.bytes,
            required,
            actual,
            &state_config,
            &edge->actual_publisher
        ) != MEISO_STATE_INIT_OK ||
        meiso_state_publisher_begin_delivery(
            edge->actual_publisher,
            (MeisoDeliveryGeneration){EDGE_STATE_DELIVERY_GENERATION}
        ) != MEISO_STATE_DELIVERY_STARTED) {
        return false;
    }
    return true;
}

static bool initialize_session_and_transport(Edge *edge)
{
    MeisoPosixTcpConfig transport_config;
    size_t required;

    memset(&edge->session_config, 0, sizeof(edge->session_config));
    edge->session_config.local_role = MEISO_SESSION_ROLE_EDGE;
    edge->session_config.local_peer_id =
        edge->private_config.local_edge_peer_id;
    edge->session_config.expected_peer_id =
        edge->private_config.expected_host_peer_id;
    edge->session_config.protocol = (MeisoProtocolVersion){1U, 0U};
    edge->session_config.profile = *meiso_wifi_telemetry_profile();
    edge->session_config.limits = (MeisoSessionLimits){
        EDGE_MAX_FRAME_BYTES,
        EDGE_MAX_RUNTIME_PAYLOAD_BYTES,
        EDGE_MAX_REGISTRY_ENTRIES
    };
    if (!random_nonzero_u64(&edge->session_config.local_incarnation) ||
        !random_nonce(&edge->session_config.local_nonce)) {
        return false;
    }
    edge->edge_owner = (MeisoOwnerScope){
        MEISO_OWNER_ROLE_EDGE,
        edge->session_config.local_incarnation
    };
    edge->clock_epoch = edge->session_config.local_incarnation;
    required = meiso_session_memory_required(&edge->session_config);
    if (required == 0U || required > sizeof(g_storage.session.bytes) ||
        meiso_session_init(
            g_storage.session.bytes,
            required,
            &edge->session_config,
            &edge->session
        ) != MEISO_SESSION_INIT_OK) {
        return false;
    }
    transport_config = (MeisoPosixTcpConfig){
        edge->connection_fd,
        edge->session,
        EDGE_MAX_FRAME_BYTES,
        EDGE_IO_BYTES_PER_STEP
    };
    required = meiso_posix_tcp_memory_required(&transport_config);
    if (required == 0U || required > sizeof(g_storage.transport.bytes) ||
        meiso_posix_tcp_init(
            g_storage.transport.bytes,
            required,
            &transport_config,
            &edge->transport
        ) != MEISO_POSIX_TCP_INIT_OK ||
        meiso_session_start(edge->session) != MEISO_SESSION_START_STARTED) {
        return false;
    }
    return true;
}

static bool initialize_runtime(Edge *edge, uint64_t now_ns)
{
    MeisoSessionSnapshot session_snapshot;
    MeisoRuntimeConfig runtime_config;
    const MeisoResourceDescriptor *desired;
    const MeisoResourceDescriptor *actual;
    const MeisoResourceDescriptor *data;
    size_t required;

    if (!meiso_session_snapshot(edge->session, &session_snapshot) ||
        session_snapshot.state != MEISO_SESSION_STATE_READY ||
        !session_snapshot.remote_identity_known ||
        session_snapshot.remote_incarnation == 0U) {
        return false;
    }
    edge->host_owner = (MeisoOwnerScope){
        MEISO_OWNER_ROLE_HOST,
        session_snapshot.remote_incarnation
    };
    if (meiso_state_replica_begin_owner(
            edge->desired_replica, edge->host_owner
        ) != MEISO_STATE_OWNER_STARTED) {
        return false;
    }
    memset(&runtime_config, 0, sizeof(runtime_config));
    runtime_config.session = edge->session;
    runtime_config.max_endpoints = 1U;
    runtime_config.max_endpoint_history = 2U;
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
            g_storage.runtime.bytes,
            required,
            &runtime_config,
            &edge->runtime
        ) != MEISO_RUNTIME_INIT_OK) {
        return false;
    }

    desired = meiso_wifi_telemetry_resource_for_facet(
        MEISO_RUNTIME_FACET_DESIRED
    );
    actual = meiso_wifi_telemetry_resource_for_facet(
        MEISO_RUNTIME_FACET_ACTUAL
    );
    data = meiso_wifi_telemetry_resource_for_facet(
        MEISO_RUNTIME_FACET_DATA
    );
    if (desired == NULL || actual == NULL || data == NULL) {
        return false;
    }
    edge->endpoint = (MeisoRuntimeEndpointDescriptor){
        *meiso_wifi_telemetry_endpoint(),
        edge->session_config.local_incarnation,
        *meiso_wifi_telemetry_capability_schema(),
        {
            edge->capability_payload,
            edge->capability_payload_size
        }
    };
    edge->routes[0] = (MeisoRuntimeRouteDescriptor){
        edge->endpoint.id,
        edge->endpoint.incarnation,
        *desired,
        MEISO_RUNTIME_FACET_DESIRED
    };
    edge->routes[1] = (MeisoRuntimeRouteDescriptor){
        edge->endpoint.id,
        edge->endpoint.incarnation,
        *actual,
        MEISO_RUNTIME_FACET_ACTUAL
    };
    edge->routes[2] = (MeisoRuntimeRouteDescriptor){
        edge->endpoint.id,
        edge->endpoint.incarnation,
        *data,
        MEISO_RUNTIME_FACET_DATA
    };
    edge->desired_route = route_key(
        edge, desired, MEISO_RUNTIME_FACET_DESIRED
    );
    edge->actual_route = route_key(
        edge, actual, MEISO_RUNTIME_FACET_ACTUAL
    );
    edge->data_route = route_key(edge, data, MEISO_RUNTIME_FACET_DATA);
    edge->runtime_initialized = true;
    edge->phase_deadline_ns = deadline_after_ms(
        now_ns, EDGE_PHASE_TIMEOUT_MS
    );
    (void)printf(
        "{\"type\":\"session\",\"phase\":\"handshake\","
        "\"status\":\"READY\"}\n"
    );
    return true;
}

static bool session_tx_idle(const Edge *edge)
{
    MeisoByteView frame;
    return edge->session != NULL &&
        !meiso_session_next_outbound(edge->session, &frame);
}

static bool try_offer_catalog(Edge *edge)
{
    MeisoRuntimeStatus status;

    if (!edge->runtime_initialized || edge->catalog_offered ||
        !session_tx_idle(edge)) {
        return true;
    }
    status = meiso_runtime_offer_catalog(
        edge->runtime,
        EDGE_CATALOG_GENERATION,
        &edge->endpoint,
        1U,
        edge->routes,
        MEISO_WIFI_TELEMETRY_RESOURCE_COUNT
    );
    if (status == MEISO_RUNTIME_TX_BUSY) {
        return true;
    }
    if (status != MEISO_RUNTIME_OK) {
        return false;
    }
    edge->catalog_offered = true;
    (void)printf(
        "{\"type\":\"runtime\",\"phase\":\"catalog\","
        "\"status\":\"OFFERED\",\"generation\":%u}\n",
        EDGE_CATALOG_GENERATION
    );
    return true;
}

static MeisoRuntimeDesiredReference desired_reference(MeisoStateView view)
{
    const MeisoRuntimeDesiredReference reference = {
        true,
        view.owner,
        view.version
    };
    return reference;
}

static ActualTransition stopped_transition(
    MeisoRuntimeDesiredReference desired,
    MeisoRuntimeAvailability availability
)
{
    ActualTransition value;
    memset(&value, 0, sizeof(value));
    value.present = true;
    value.observed = desired;
    value.applied = desired;
    value.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    value.availability = availability;
    value.production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
    value.reason = MEISO_RUNTIME_PRODUCTION_REASON_DISABLED;
    return value;
}

static ActualTransition starting_transition(
    const Edge *edge,
    MeisoRuntimeDesiredReference desired,
    MeisoRuntimeAvailability availability,
    MeisoWifiTelemetryDesiredParameters parameters
)
{
    ActualTransition value;

    memset(&value, 0, sizeof(value));
    value.present = true;
    value.observed = desired;
    value.applied = edge->production_applied_desired;
    value.apply_result = MEISO_RUNTIME_APPLY_PENDING;
    value.availability = availability;
    value.production_status = MEISO_RUNTIME_PRODUCTION_STARTING;
    value.reason = MEISO_RUNTIME_PRODUCTION_REASON_APPLYING;
    value.follow_up = ACTUAL_FOLLOW_UP_RUNNING;
    value.follow_up_parameters = parameters;
    return value;
}

static bool live_pending_transition(
    const Edge *edge,
    MeisoRuntimeDesiredReference desired,
    MeisoRuntimeProductionStatus status,
    ActualFollowUp follow_up,
    MeisoWifiTelemetryDesiredParameters follow_up_parameters,
    ActualTransition *out_transition
)
{
    ActualTransition value;

    if (edge == NULL || out_transition == NULL ||
        !edge->production_applied_desired.present ||
        edge->active_production_epoch == 0U ||
        edge->production_effective.selected_fields == 0U ||
        edge->production_effective.sample_period_ms == 0U ||
        (status != MEISO_RUNTIME_PRODUCTION_RUNNING &&
         status != MEISO_RUNTIME_PRODUCTION_STOPPING)) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.present = true;
    value.observed = desired;
    value.applied = edge->production_applied_desired;
    value.apply_result = MEISO_RUNTIME_APPLY_PENDING;
    value.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    value.effective_enabled = true;
    value.production_status = status;
    value.reason = MEISO_RUNTIME_PRODUCTION_REASON_APPLYING;
    value.production_epoch = edge->active_production_epoch;
    value.effective = edge->production_effective;
    value.follow_up = follow_up;
    value.follow_up_parameters = follow_up_parameters;
    *out_transition = value;
    return true;
}

static ActualTransition rejected_transition(
    MeisoRuntimeDesiredReference desired,
    MeisoRuntimeAvailability availability
)
{
    ActualTransition value;
    memset(&value, 0, sizeof(value));
    value.present = true;
    value.observed = desired;
    value.apply_result = MEISO_RUNTIME_APPLY_REJECTED;
    value.availability = availability;
    value.production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
    value.reason = MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER;
    return value;
}

static ActualTransition unavailable_transition(
    MeisoRuntimeDesiredReference desired
)
{
    ActualTransition value;
    memset(&value, 0, sizeof(value));
    value.present = true;
    value.observed = desired;
    value.apply_result = MEISO_RUNTIME_APPLY_FAILED;
    value.availability = MEISO_RUNTIME_AVAILABILITY_UNAVAILABLE;
    value.production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
    value.reason = MEISO_RUNTIME_PRODUCTION_REASON_UNAVAILABLE;
    return value;
}

static bool running_transition(
    Edge *edge,
    MeisoRuntimeDesiredReference desired,
    MeisoWifiTelemetryDesiredParameters parameters,
    ActualTransition *out_transition
)
{
    ActualTransition value;

    if (out_transition == NULL ||
        edge->production_epoch_high_water == UINT64_MAX) {
        return false;
    }
    edge->production_epoch_high_water += 1U;
    memset(&value, 0, sizeof(value));
    value.present = true;
    value.observed = desired;
    value.applied = desired;
    value.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    value.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    value.effective_enabled = true;
    value.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING;
    value.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    value.production_epoch = edge->production_epoch_high_water;
    value.effective.selected_fields = parameters.selected_fields;
    value.effective.sample_period_ms = parameters.sample_period_ms;
    *out_transition = value;
    return true;
}

static bool request_actual(Edge *edge, ActualTransition transition)
{
    if (edge == NULL || !transition.present) {
        return false;
    }
    if (edge->actual_in_flight || edge->actual_outbound.published) {
        if (edge->actual_next.present) {
            return false;
        }
        edge->actual_next = transition;
    } else if (edge->actual_outbound.present) {
        if (edge->actual_next.present) {
            return false;
        }
        edge->actual_next = transition;
    } else {
        edge->actual_outbound = transition;
    }
    return true;
}

static void mark_source_continuity_lost(Edge *edge, const char *cause)
{
    if (!edge->source_continuity_lost) {
        edge->source_continuity_lost = true;
        edge->production_active = false;
        (void)printf(
            "{\"type\":\"source\",\"phase\":\"continuity\","
            "\"status\":\"LOST\",\"cause\":\"%s\","
            "\"action\":\"SESSION_SHUTDOWN\"}\n",
            cause
        );
    }
}

static bool observe_current_availability(
    Edge *edge,
    MeisoRuntimeAvailability *out_availability
)
{
    MeisoWifiTelemetryFieldMask fields = 0U;
    const MeisoWifiLinuxAdapterStatus status =
        meiso_wifi_linux_adapter_probe(&edge->wifi, &fields);

    if (out_availability == NULL) {
        return false;
    }
    if (status == MEISO_WIFI_LINUX_ADAPTER_OK) {
        *out_availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
        return true;
    }
    if (status != MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE) {
        return false;
    }
    *out_availability = MEISO_RUNTIME_AVAILABILITY_UNAVAILABLE;
    if (edge->production_ever_started) {
        mark_source_continuity_lost(edge, "SOURCE_UNAVAILABLE");
    }
    return true;
}

static bool apply_current_desired(Edge *edge, uint64_t now_ns)
{
    MeisoStateView view;
    MeisoRuntimeDesiredState desired;
    MeisoWifiTelemetryDesiredParameters parameters;
    MeisoRuntimeDesiredReference reference;
    MeisoRuntimeAvailability availability;
    ActualTransition transition;
    MeisoWifiTelemetryCodecStatus decoded;

    if (!meiso_state_replica_current(edge->desired_replica, &view) ||
        !meiso_runtime_desired_state_decode(view.payload, &desired)) {
        return false;
    }
    decoded = meiso_wifi_telemetry_desired_parameters_decode(
        desired.profile, &parameters
    );
    reference = desired_reference(view);
    edge->current_desired = reference;
    edge->desired_enabled = desired.enabled;
    edge->desired_profile_valid =
        decoded == MEISO_WIFI_TELEMETRY_CODEC_OK &&
        meiso_wifi_telemetry_parameters_supported(
            &edge->capability, &parameters
        );
    if (!observe_current_availability(edge, &availability)) {
        return false;
    }
    if (edge->source_continuity_lost) {
        return true;
    }
    if (!edge->desired_profile_valid) {
        return request_actual(
            edge,
            rejected_transition(reference, availability)
        );
    }
    edge->current_parameters = parameters;
    if (!desired.enabled) {
        if (edge->active_production_epoch != 0U) {
            memset(&transition, 0, sizeof(transition));
            if (!live_pending_transition(
                    edge,
                    reference,
                    MEISO_RUNTIME_PRODUCTION_STOPPING,
                    ACTUAL_FOLLOW_UP_STOPPED,
                    parameters,
                    &transition
                )) {
                return false;
            }
            /* The public source is cut here. Any sample already materialized
             * remains pending and is sent before the STOPPING actual. */
            edge->production_active = false;
            return request_actual(edge, transition);
        }
        return request_actual(
            edge, stopped_transition(reference, availability)
        );
    }
    if (availability != MEISO_RUNTIME_AVAILABILITY_PRESENT) {
        if (!request_actual(edge, unavailable_transition(reference))) {
            return false;
        }
        edge->next_probe_ns = deadline_after_ms(
            now_ns, parameters.sample_period_ms
        );
        return true;
    }
    if (edge->active_production_epoch != 0U) {
        memset(&transition, 0, sizeof(transition));
        if (!live_pending_transition(
                edge,
                reference,
                MEISO_RUNTIME_PRODUCTION_RUNNING,
                ACTUAL_FOLLOW_UP_RUNNING,
                parameters,
                &transition
            )) {
            return false;
        }
    } else {
        transition = starting_transition(
            edge, reference, availability, parameters
        );
    }
    return request_actual(edge, transition);
}

static bool queue_state_accept(
    Edge *edge,
    MeisoRuntimeRouteKey route,
    MeisoDeliveryGeneration generation,
    MeisoOwnerScope owner,
    uint64_t version
)
{
    if (edge->pending_accept.present) {
        return edge->pending_accept.generation.value == generation.value &&
            meiso_owner_scope_equal(edge->pending_accept.owner, owner) &&
            edge->pending_accept.version == version;
    }
    edge->pending_accept.present = true;
    edge->pending_accept.route = route;
    edge->pending_accept.generation = generation;
    edge->pending_accept.owner = owner;
    edge->pending_accept.version = version;
    return true;
}

static bool handle_desired_offer(
    Edge *edge,
    const MeisoRuntimeEvent *event,
    uint64_t now_ns
)
{
    MeisoStateReceiveStatus receive_status;
    MeisoRuntimeRouteKey route;
    MeisoDeliveryGeneration generation;
    MeisoOwnerScope owner;
    uint64_t version;

    if (event == NULL || event->type != MEISO_RUNTIME_EVENT_STATE_OFFER ||
        event->route.facet != MEISO_RUNTIME_FACET_DESIRED) {
        return false;
    }
    route = event->route;
    generation = event->state_offer.generation;
    owner = event->state_offer.state.owner;
    version = event->state_offer.state.version;
    receive_status = meiso_state_replica_receive(
        edge->desired_replica,
        owner,
        version,
        event->state_offer.state.payload
    );
    if (meiso_runtime_commit_state_receive(
            edge->runtime,
            route,
            &event->state_offer,
            receive_status
        ) != MEISO_RUNTIME_OK) {
        return false;
    }
    if (receive_status != MEISO_STATE_RECEIVE_NEW &&
        receive_status != MEISO_STATE_RECEIVE_DUPLICATE) {
        return false;
    }
    if (!queue_state_accept(edge, route, generation, owner, version)) {
        return false;
    }
    return receive_status != MEISO_STATE_RECEIVE_NEW ||
        apply_current_desired(edge, now_ns);
}

static bool handle_actual_accept(
    Edge *edge,
    const MeisoRuntimeEvent *event
)
{
    ActualTransition accepted;
    ActualTransition follow_up;
    MeisoStateAcceptStatus status;

    if (!edge->actual_in_flight || event == NULL ||
        event->type != MEISO_RUNTIME_EVENT_STATE_ACCEPT ||
        event->route.facet != MEISO_RUNTIME_FACET_ACTUAL) {
        return false;
    }
    status = meiso_state_publisher_mark_delivery_accepted(
        edge->actual_publisher,
        event->state_accept_generation,
        event->state_accept_owner,
        event->state_accept_version
    );
    if (status != MEISO_STATE_ACCEPT_CONFIRMED) {
        return false;
    }
    accepted = edge->actual_in_flight_value;
    memset(
        &edge->actual_in_flight_value,
        0,
        sizeof(edge->actual_in_flight_value)
    );
    edge->actual_in_flight = false;
    if (accepted.follow_up != ACTUAL_FOLLOW_UP_NONE) {
        if (edge->actual_next.present) {
            return false;
        }
        memset(&follow_up, 0, sizeof(follow_up));
        if (accepted.follow_up == ACTUAL_FOLLOW_UP_STOPPED) {
            follow_up = stopped_transition(
                accepted.observed, accepted.availability
            );
        } else if (accepted.follow_up == ACTUAL_FOLLOW_UP_RUNNING) {
            /* No new sample may materialize across the production cut. An
             * already-materialized old-epoch sample remains drainable. */
            edge->production_active = false;
            if (!running_transition(
                    edge,
                    accepted.observed,
                    accepted.follow_up_parameters,
                    &follow_up
                )) {
                return false;
            }
        } else {
            return false;
        }
        return request_actual(edge, follow_up);
    }
    if (edge->actual_next.present) {
        edge->actual_outbound = edge->actual_next;
        memset(&edge->actual_next, 0, sizeof(edge->actual_next));
    }
    return true;
}

static bool handle_runtime_message(
    Edge *edge,
    const MeisoSessionRuntimeMessage *message,
    uint64_t now_ns
)
{
    MeisoRuntimeEvent event;
    const MeisoRuntimeStatus status = meiso_runtime_receive(
        edge->runtime, message, &event
    );

    if (status != MEISO_RUNTIME_OK) {
        return false;
    }
    switch (event.type) {
    case MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED:
        if (!edge->catalog_offered || edge->catalog_active ||
            event.catalog_generation != EDGE_CATALOG_GENERATION) {
            return false;
        }
        edge->catalog_active = true;
        (void)printf(
            "{\"type\":\"runtime\",\"phase\":\"catalog\","
            "\"status\":\"ACCEPTED\",\"generation\":%u}\n",
            EDGE_CATALOG_GENERATION
        );
        return true;
    case MEISO_RUNTIME_EVENT_CATALOG_REJECTED:
        return false;
    case MEISO_RUNTIME_EVENT_STATE_OFFER:
        return edge->catalog_active &&
            handle_desired_offer(edge, &event, now_ns);
    case MEISO_RUNTIME_EVENT_STATE_ACCEPT:
        return edge->catalog_active && handle_actual_accept(edge, &event);
    default:
        return false;
    }
}

static bool try_send_pending_sample(Edge *edge)
{
    MeisoRuntimeStatus status;

    if (!edge->pending_sample.present || !session_tx_idle(edge)) {
        return true;
    }
    status = meiso_runtime_send_stream_sample(
        edge->runtime,
        edge->data_route,
        edge->pending_sample.production_epoch,
        &edge->pending_sample.sample
    );
    if (status == MEISO_RUNTIME_TX_BUSY) {
        return true;
    }
    if (status != MEISO_RUNTIME_OK) {
        return false;
    }
    edge->samples_queued += 1U;
    edge->pending_sample.present = false;
    (void)printf(
        "{\"type\":\"stream\",\"phase\":\"sample\","
        "\"status\":\"QUEUED\",\"count\":%" PRIu64 "}\n",
        edge->samples_queued
    );
    return true;
}

static bool try_send_state_accept(Edge *edge)
{
    MeisoRuntimeStatus status;

    if (!edge->pending_accept.present || !session_tx_idle(edge)) {
        return true;
    }
    status = meiso_runtime_send_state_accept(
        edge->runtime,
        edge->pending_accept.route,
        edge->pending_accept.generation,
        edge->pending_accept.owner,
        edge->pending_accept.version
    );
    if (status == MEISO_RUNTIME_TX_BUSY) {
        return true;
    }
    if (status != MEISO_RUNTIME_OK) {
        return false;
    }
    edge->pending_accept.present = false;
    return true;
}

static const char *actual_status_name(const ActualTransition *transition)
{
    if (transition->apply_result == MEISO_RUNTIME_APPLY_REJECTED) {
        return "REJECTED";
    }
    if (transition->availability == MEISO_RUNTIME_AVAILABILITY_UNAVAILABLE) {
        return "UNAVAILABLE";
    }
    switch (transition->production_status) {
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
        return "STOPPED";
    }
}

static bool publish_actual_if_needed(Edge *edge)
{
    uint8_t effective_payload[
        MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE
    ];
    uint8_t actual_payload[
        MEISO_WIFI_TELEMETRY_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    ];
    size_t effective_size = 0U;
    MeisoByteView encoded_actual;
    MeisoRuntimeActualState actual;
    MeisoStateOffer offer;
    MeisoStatePublishStatus published;
    MeisoRuntimeStatus sent;
    ActualTransition queued;
    uint64_t now_ns;

    if (!edge->actual_outbound.present || edge->actual_in_flight ||
        edge->pending_sample.present || edge->pending_accept.present ||
        !session_tx_idle(edge)) {
        return true;
    }
    if (!edge->actual_outbound.published) {
        if (edge->actual_version == UINT64_MAX ||
            meiso_wifi_telemetry_effective_parameters_encode(
                &edge->actual_outbound.effective,
                (MeisoMutableBuffer){
                    effective_payload, sizeof(effective_payload)
                },
                &effective_size
            ) != MEISO_WIFI_TELEMETRY_CODEC_OK) {
            return false;
        }
        memset(&actual, 0, sizeof(actual));
        actual.observed_desired = edge->actual_outbound.observed;
        actual.applied_desired = edge->actual_outbound.applied;
        actual.apply_result = edge->actual_outbound.apply_result;
        actual.availability = edge->actual_outbound.availability;
        actual.effective_enabled = edge->actual_outbound.effective_enabled;
        actual.production_status = edge->actual_outbound.production_status;
        actual.reason = edge->actual_outbound.reason;
        actual.production_epoch = edge->actual_outbound.production_epoch;
        actual.profile = (MeisoByteView){effective_payload, effective_size};
        if (!meiso_runtime_actual_state_encode(
                &actual,
                (MeisoMutableBuffer){
                    actual_payload, sizeof(actual_payload)
                },
                &encoded_actual
            )) {
            return false;
        }
        edge->actual_version += 1U;
        published = meiso_state_publisher_publish(
            edge->actual_publisher,
            edge->edge_owner,
            edge->actual_version,
            encoded_actual
        );
        if (published != MEISO_STATE_PUBLISH_NEW) {
            return false;
        }
        edge->actual_outbound.published = true;
    }
    if (!meiso_state_publisher_next_offer(edge->actual_publisher, &offer)) {
        return false;
    }
    sent = meiso_runtime_send_state_offer(
        edge->runtime, edge->actual_route, &offer
    );
    if (sent == MEISO_RUNTIME_TX_BUSY) {
        return true;
    }
    if (sent != MEISO_RUNTIME_OK) {
        return false;
    }
    queued = edge->actual_outbound;
    memset(&edge->actual_outbound, 0, sizeof(edge->actual_outbound));
    edge->actual_in_flight = true;
    edge->actual_in_flight_value = queued;
    if (queued.apply_result == MEISO_RUNTIME_APPLY_APPLIED &&
        queued.production_status == MEISO_RUNTIME_PRODUCTION_RUNNING &&
        queued.effective_enabled) {
        if (!monotonic_now_ns(&now_ns)) {
            return false;
        }
        edge->production_applied_desired = queued.applied;
        edge->production_effective = queued.effective;
        edge->active_production_epoch = queued.production_epoch;
        edge->production_active = true;
        edge->production_ever_started = true;
        edge->stream_sequence = 0U;
        edge->next_sample_ns = now_ns;
    } else if (queued.apply_result == MEISO_RUNTIME_APPLY_PENDING &&
        queued.production_status == MEISO_RUNTIME_PRODUCTION_RUNNING &&
        queued.effective_enabled && queued.production_epoch != 0U) {
        if (queued.production_epoch != edge->active_production_epoch ||
            queued.effective.selected_fields !=
                edge->production_effective.selected_fields ||
            queued.effective.sample_period_ms !=
                edge->production_effective.sample_period_ms) {
            return false;
        }
        edge->production_active = true;
    } else if (queued.production_status ==
        MEISO_RUNTIME_PRODUCTION_STOPPED) {
        edge->production_applied_desired = queued.applied;
        memset(
            &edge->production_effective,
            0,
            sizeof(edge->production_effective)
        );
        edge->active_production_epoch = 0U;
        edge->production_active = false;
    } else {
        edge->production_active = false;
    }
    (void)printf(
        "{\"type\":\"state\",\"phase\":\"actual\","
        "\"status\":\"%s\",\"version\":%" PRIu64 ","
        "\"desired_version\":%" PRIu64 ","
        "\"production_epoch\":%" PRIu64 "}\n",
        actual_status_name(&queued),
        edge->actual_version,
        queued.observed.version,
        queued.production_epoch
    );
    return true;
}

static bool observe_counter(
    CounterContinuity *continuity,
    MeisoWifiTelemetryFieldMask present_fields,
    MeisoWifiTelemetryFieldMask field,
    uint64_t value,
    uint64_t *high_water
)
{
    if ((present_fields & field) == 0U) {
        return true;
    }
    if ((continuity->seen_fields & field) != 0U && value < *high_water) {
        return false;
    }
    if ((continuity->seen_fields & field) == 0U || value > *high_water) {
        *high_water = value;
    }
    continuity->seen_fields |= field;
    return true;
}

static bool counter_continuity_observe(
    CounterContinuity *continuity,
    const MeisoWifiTelemetrySample *sample
)
{
    return continuity != NULL && sample != NULL &&
        observe_counter(
            continuity,
            sample->present_fields,
            MEISO_WIFI_TELEMETRY_FIELD_RX_BYTES,
            sample->rx_bytes,
            &continuity->rx_bytes
        ) && observe_counter(
            continuity,
            sample->present_fields,
            MEISO_WIFI_TELEMETRY_FIELD_TX_BYTES,
            sample->tx_bytes,
            &continuity->tx_bytes
        ) && observe_counter(
            continuity,
            sample->present_fields,
            MEISO_WIFI_TELEMETRY_FIELD_RX_PACKETS,
            sample->rx_packets,
            &continuity->rx_packets
        ) && observe_counter(
            continuity,
            sample->present_fields,
            MEISO_WIFI_TELEMETRY_FIELD_TX_PACKETS,
            sample->tx_packets,
            &continuity->tx_packets
        ) && observe_counter(
            continuity,
            sample->present_fields,
            MEISO_WIFI_TELEMETRY_FIELD_RX_DROPPED,
            sample->rx_dropped,
            &continuity->rx_dropped
        ) && observe_counter(
            continuity,
            sample->present_fields,
            MEISO_WIFI_TELEMETRY_FIELD_TX_DROPPED,
            sample->tx_dropped,
            &continuity->tx_dropped
        ) && observe_counter(
            continuity,
            sample->present_fields,
            MEISO_WIFI_TELEMETRY_FIELD_RX_ERRORS,
            sample->rx_errors,
            &continuity->rx_errors
        ) && observe_counter(
            continuity,
            sample->present_fields,
            MEISO_WIFI_TELEMETRY_FIELD_TX_ERRORS,
            sample->tx_errors,
            &continuity->tx_errors
        );
}

static bool counter_continuity_self_test(void)
{
    const MeisoWifiTelemetryFieldMask counter_fields =
        MEISO_WIFI_TELEMETRY_FIELD_RX_BYTES |
        MEISO_WIFI_TELEMETRY_FIELD_TX_BYTES |
        MEISO_WIFI_TELEMETRY_FIELD_RX_PACKETS |
        MEISO_WIFI_TELEMETRY_FIELD_TX_PACKETS |
        MEISO_WIFI_TELEMETRY_FIELD_RX_DROPPED |
        MEISO_WIFI_TELEMETRY_FIELD_TX_DROPPED |
        MEISO_WIFI_TELEMETRY_FIELD_RX_ERRORS |
        MEISO_WIFI_TELEMETRY_FIELD_TX_ERRORS;
    CounterContinuity continuity;
    MeisoWifiTelemetrySample sample;

    memset(&continuity, 0, sizeof(continuity));
    memset(&sample, 0, sizeof(sample));
    sample.selected_fields = counter_fields;
    sample.present_fields = counter_fields;
    sample.rx_bytes = 100U;
    sample.tx_bytes = 101U;
    sample.rx_packets = 102U;
    sample.tx_packets = 103U;
    sample.rx_dropped = 104U;
    sample.tx_dropped = 105U;
    sample.rx_errors = 106U;
    sample.tx_errors = 107U;
    if (!counter_continuity_observe(&continuity, &sample)) {
        return false;
    }
    sample.present_fields = 0U;
    if (!counter_continuity_observe(&continuity, &sample) ||
        continuity.rx_bytes != 100U ||
        continuity.tx_bytes != 101U ||
        continuity.rx_packets != 102U ||
        continuity.tx_packets != 103U ||
        continuity.rx_dropped != 104U ||
        continuity.tx_dropped != 105U ||
        continuity.rx_errors != 106U ||
        continuity.tx_errors != 107U ||
        (continuity.seen_fields & counter_fields) != counter_fields) {
        return false;
    }
    sample.present_fields = MEISO_WIFI_TELEMETRY_FIELD_TX_ERRORS;
    sample.tx_errors = 5U;
    return !counter_continuity_observe(&continuity, &sample);
}

static bool request_running_for_current_desired(Edge *edge)
{
    ActualTransition transition;
    if (!running_transition(
            edge,
            edge->current_desired,
            edge->current_parameters,
            &transition
        )) {
        return false;
    }
    return request_actual(edge, transition);
}

static bool maybe_materialize_sample(Edge *edge, uint64_t now_ns)
{
    MeisoWifiTelemetrySample value;
    MeisoWifiLinuxAdapterStatus adapter_status;
    size_t encoded_size = 0U;

    if (!edge->production_active || edge->pending_sample.present ||
        now_ns < edge->next_sample_ns) {
        return true;
    }
    if (edge->active_production_epoch == 0U ||
        edge->production_effective.selected_fields == 0U ||
        edge->production_effective.sample_period_ms == 0U) {
        return false;
    }
    adapter_status = meiso_wifi_linux_adapter_sample(
        &edge->wifi,
        edge->production_effective.selected_fields,
        &value
    );
    if (adapter_status == MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE) {
        mark_source_continuity_lost(edge, "SOURCE_UNAVAILABLE");
        return true;
    }
    if (adapter_status != MEISO_WIFI_LINUX_ADAPTER_OK) {
        return false;
    }
    if (!counter_continuity_observe(&edge->counter_continuity, &value)) {
        mark_source_continuity_lost(edge, "COUNTER_RESET");
        return true;
    }
    if (edge->stream_sequence == UINT64_MAX ||
        meiso_wifi_telemetry_sample_encode(
            &value,
            (MeisoMutableBuffer){
                edge->pending_sample.payload,
                sizeof(edge->pending_sample.payload)
            },
            &encoded_size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        encoded_size != sizeof(edge->pending_sample.payload)) {
        return false;
    }
    edge->stream_sequence += 1U;
    memset(&edge->pending_sample.sample, 0, sizeof(edge->pending_sample.sample));
    edge->pending_sample.sample.key = (MeisoStreamKey){
        edge->edge_owner,
        edge->active_production_epoch,
        edge->stream_sequence
    };
    edge->pending_sample.sample.source_time = (MeisoTimePoint){
        edge->clock_epoch,
        now_ns
    };
    edge->pending_sample.sample.payload = (MeisoByteView){
        edge->pending_sample.payload,
        encoded_size
    };
    edge->pending_sample.production_epoch =
        edge->active_production_epoch;
    edge->pending_sample.present = true;
    edge->next_sample_ns = deadline_after_ms(
        now_ns, edge->production_effective.sample_period_ms
    );
    return true;
}

static bool maybe_probe_recovery(Edge *edge, uint64_t now_ns)
{
    MeisoWifiTelemetryFieldMask fields = 0U;
    MeisoWifiLinuxAdapterStatus status;

    if (!edge->catalog_active || !edge->desired_profile_valid ||
        !edge->desired_enabled || edge->production_active ||
        edge->production_ever_started || edge->source_continuity_lost ||
        edge->actual_in_flight || edge->actual_outbound.present ||
        edge->actual_next.present || now_ns < edge->next_probe_ns) {
        return true;
    }
    status = meiso_wifi_linux_adapter_probe(&edge->wifi, &fields);
    if (status == MEISO_WIFI_LINUX_ADAPTER_OK) {
        return request_running_for_current_desired(edge);
    }
    if (status != MEISO_WIFI_LINUX_ADAPTER_UNAVAILABLE) {
        return false;
    }
    edge->next_probe_ns = deadline_after_ms(
        now_ns, edge->current_parameters.sample_period_ms
    );
    return true;
}

static bool drive_application(Edge *edge, uint64_t now_ns)
{
    if ((!edge->source_continuity_lost && !try_offer_catalog(edge)) ||
        !try_send_pending_sample(edge) || !try_send_state_accept(edge)) {
        return false;
    }
    if (edge->source_continuity_lost) {
        return true;
    }
    if (!publish_actual_if_needed(edge) ||
        !maybe_materialize_sample(edge, now_ns) ||
        !try_send_pending_sample(edge)) {
        return false;
    }
    if (edge->source_continuity_lost) {
        return true;
    }
    if (!maybe_probe_recovery(edge, now_ns) ||
        !publish_actual_if_needed(edge)) {
        return false;
    }
    return true;
}

static bool source_loss_drained(const Edge *edge)
{
    return edge->source_continuity_lost &&
        !edge->pending_sample.present && !edge->pending_accept.present &&
        session_tx_idle(edge);
}

static int poll_timeout_ms(const Edge *edge, uint64_t now_ns)
{
    uint64_t target = deadline_after_ms(now_ns, EDGE_POLL_SLICE_MS);
    uint64_t remaining;

    if (meiso_posix_tcp_wants_write(edge->transport)) {
        return 0;
    }
    if (edge->production_active && !edge->pending_sample.present &&
        edge->next_sample_ns < target) {
        target = edge->next_sample_ns;
    }
    if (edge->desired_profile_valid && edge->desired_enabled &&
        !edge->production_active && edge->next_probe_ns < target) {
        target = edge->next_probe_ns;
    }
    if (target <= now_ns) {
        return 0;
    }
    remaining = target - now_ns;
    remaining = (remaining + UINT64_C(999999)) / UINT64_C(1000000);
    return remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
}

static bool write_status_ok(MeisoPosixTcpWriteStatus status)
{
    return status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
        status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK ||
        status == MEISO_POSIX_TCP_WRITE_INTERRUPTED ||
        status == MEISO_POSIX_TCP_WRITE_FRAME_SENT ||
        status == MEISO_POSIX_TCP_WRITE_IDLE;
}

static bool peer_close_is_normal_and_stopped(Edge *edge)
{
    MeisoSessionSnapshot snapshot;
    return !edge->source_continuity_lost &&
        meiso_session_snapshot(edge->session, &snapshot) &&
        snapshot.state == MEISO_SESSION_STATE_CLOSED &&
        snapshot.failure == MEISO_SESSION_FAILURE_PEER_CLOSED &&
        snapshot.peer_close_reason_known &&
        snapshot.peer_close_reason == MEISO_SESSION_CLOSE_NORMAL &&
        !edge->production_active && !edge->pending_sample.present &&
        edge->active_production_epoch == 0U &&
        !edge->pending_accept.present && !edge->actual_in_flight &&
        !edge->actual_outbound.present && !edge->actual_next.present &&
        edge->current_desired.present && !edge->desired_enabled &&
        edge->production_applied_desired.present &&
        meiso_owner_scope_equal(
            edge->production_applied_desired.owner,
            edge->current_desired.owner
        ) && edge->production_applied_desired.version ==
            edge->current_desired.version;
}

static bool process_readable(Edge *edge, uint64_t now_ns)
{
    MeisoSessionRuntimeMessage message;
    const MeisoPosixTcpReadStatus status = meiso_posix_tcp_on_readable(
        edge->transport, &message
    );

    switch (status) {
    case MEISO_POSIX_TCP_READ_PROGRESS:
    case MEISO_POSIX_TCP_READ_WOULD_BLOCK:
    case MEISO_POSIX_TCP_READ_INTERRUPTED:
    case MEISO_POSIX_TCP_READ_SESSION_PROGRESS:
        return true;
    case MEISO_POSIX_TCP_READ_RUNTIME:
        return edge->runtime_initialized &&
            handle_runtime_message(edge, &message, now_ns);
    case MEISO_POSIX_TCP_READ_PEER_CLOSED:
        edge->peer_closed_normally = peer_close_is_normal_and_stopped(edge);
        return edge->peer_closed_normally;
    default:
        return false;
    }
}

static bool phase_timed_out(const Edge *edge, uint64_t now_ns)
{
    if (!edge->runtime_initialized) {
        return now_ns >= edge->phase_deadline_ns;
    }
    return !edge->catalog_active && now_ns >= edge->phase_deadline_ns;
}

static bool run_connection(Edge *edge)
{
    uint64_t now_ns;

    if (!monotonic_now_ns(&now_ns)) {
        return false;
    }
    edge->phase_deadline_ns = deadline_after_ms(
        now_ns, EDGE_PHASE_TIMEOUT_MS
    );
    while (!g_stop_requested && !edge->peer_closed_normally) {
        struct pollfd descriptor;
        MeisoSessionSnapshot snapshot;
        int poll_result;

        if (!monotonic_now_ns(&now_ns) || phase_timed_out(edge, now_ns) ||
            !meiso_session_snapshot(edge->session, &snapshot)) {
            return false;
        }
        if (!edge->runtime_initialized &&
            snapshot.state == MEISO_SESSION_STATE_READY) {
            if (!initialize_runtime(edge, now_ns)) {
                return false;
            }
        }
        if (!drive_application(edge, now_ns)) {
            return false;
        }
        if (source_loss_drained(edge)) {
            return false;
        }
        descriptor.fd = edge->connection_fd;
        descriptor.events = 0;
        descriptor.revents = 0;
        if (!edge->source_continuity_lost &&
            meiso_posix_tcp_wants_read(edge->transport)) {
            descriptor.events = (short)(descriptor.events | POLLIN);
        }
        if (meiso_posix_tcp_wants_write(edge->transport)) {
            descriptor.events = (short)(descriptor.events | POLLOUT);
        }
        poll_result = poll(
            &descriptor,
            (nfds_t)1U,
            poll_timeout_ms(edge, now_ns)
        );
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if ((descriptor.revents & POLLOUT) != 0 && !write_status_ok(
                meiso_posix_tcp_on_writable(edge->transport)
            )) {
            return false;
        }
        if (!edge->source_continuity_lost &&
            (descriptor.revents & POLLIN) != 0 &&
            !process_readable(edge, now_ns)) {
            return false;
        }
        if (!edge->source_continuity_lost &&
            (descriptor.revents & POLLHUP) != 0 &&
            (descriptor.revents & POLLIN) == 0 &&
            meiso_posix_tcp_wants_read(edge->transport) &&
            !process_readable(edge, now_ns)) {
            return false;
        }
        if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            meiso_posix_tcp_abort(edge->transport, EIO);
            return false;
        }
    }
    return edge->peer_closed_normally;
}

static void flush_local_close(Edge *edge, MeisoSessionCloseReason reason)
{
    uint64_t now_ns;
    uint64_t deadline;
    MeisoSessionCloseStatus close_status;

    if (edge->session == NULL || edge->transport == NULL ||
        !monotonic_now_ns(&now_ns)) {
        return;
    }
    close_status = meiso_session_close(edge->session, reason);
    if (close_status != MEISO_SESSION_CLOSE_QUEUED) {
        return;
    }
    deadline = deadline_after_ms(now_ns, EDGE_CLOSE_TIMEOUT_MS);
    while (meiso_posix_tcp_wants_write(edge->transport) &&
           monotonic_now_ns(&now_ns) && now_ns < deadline) {
        struct pollfd descriptor = {
            edge->connection_fd,
            POLLOUT,
            0
        };
        const int wait_ms = (int)((deadline - now_ns) /
            UINT64_C(1000000));
        const int result = poll(
            &descriptor,
            (nfds_t)1U,
            wait_ms > EDGE_POLL_SLICE_MS ? EDGE_POLL_SLICE_MS : wait_ms
        );
        if (result > 0 && (descriptor.revents & POLLOUT) != 0) {
            if (!write_status_ok(
                    meiso_posix_tcp_on_writable(edge->transport)
                )) {
                break;
            }
        } else if (result < 0 && errno != EINTR) {
            break;
        }
    }
}

static void usage(const char *program)
{
    (void)fprintf(
        stderr,
        "usage: %s --config PATH\n",
        program != NULL ? program : "wifi_runtime_edge"
    );
}

int main(int argc, char **argv)
{
    Edge edge;
    bool passed;

    memset(&edge, 0, sizeof(edge));
    edge.listener_fd = -1;
    edge.connection_fd = -1;
    if (!counter_continuity_self_test()) {
        fputs("wifi Runtime Edge continuity self-test failed\n", stderr);
        return 1;
    }
    if (argc != 3 || strcmp(argv[1], "--config") != 0) {
        usage(argv[0]);
        return 2;
    }
    if (!install_signal_handlers() ||
        !parse_private_config(argv[2], &edge.private_config) ||
        !initialize_profile(&edge) || !initialize_resources(&edge)) {
        fputs("wifi Runtime Edge bootstrap failed\n", stderr);
        return 1;
    }
    edge.listener_fd = make_listener(&edge.private_config);
    if (edge.listener_fd < 0) {
        fputs("wifi Runtime Edge listen failed\n", stderr);
        return 1;
    }
    (void)printf(
        "{\"type\":\"edge\",\"phase\":\"listen\","
        "\"status\":\"READY\"}\n"
    );
    (void)fflush(stdout);
    edge.connection_fd = accept_one(edge.listener_fd);
    close_fd(&edge.listener_fd);
    if (edge.connection_fd < 0) {
        if (!g_stop_requested) {
            fputs("wifi Runtime Edge accept failed\n", stderr);
        }
        return g_stop_requested ? 0 : 1;
    }
    if (!initialize_session_and_transport(&edge)) {
        fputs("wifi Runtime Edge Session bootstrap failed\n", stderr);
        close_fd(&edge.connection_fd);
        return 1;
    }
    passed = run_connection(&edge);
    if (!passed && !edge.peer_closed_normally) {
        flush_local_close(
            &edge,
            (g_stop_requested || edge.source_continuity_lost) ?
                MEISO_SESSION_CLOSE_SHUTDOWN :
                MEISO_SESSION_CLOSE_PROTOCOL_ERROR
        );
    }
    close_fd(&edge.connection_fd);
    if (edge.peer_closed_normally) {
        (void)printf(
            "{\"type\":\"session\",\"phase\":\"close\","
            "\"status\":\"NORMAL\",\"samples_queued\":%" PRIu64 "}\n",
            edge.samples_queued
        );
        return 0;
    }
    if (g_stop_requested) {
        (void)printf(
            "{\"type\":\"session\",\"phase\":\"close\","
            "\"status\":\"SHUTDOWN\"}\n"
        );
        return 0;
    }
    if (edge.source_continuity_lost) {
        (void)printf(
            "{\"type\":\"session\",\"phase\":\"close\","
            "\"status\":\"SHUTDOWN\","
            "\"cause\":\"CONTINUITY_LOST\"}\n"
        );
        return 1;
    }
    fputs("wifi Runtime Edge connection failed\n", stderr);
    return 1;
}
