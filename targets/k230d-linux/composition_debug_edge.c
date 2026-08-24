#define _POSIX_C_SOURCE 200809L

#include "composition_edge_app.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
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
 * Foreground K230D Composition Profile-v3 debug service.
 *
 * This target-local runner owns only numeric listeners, accepted fds, one
 * EdgeApp arena and one bounded thermal worker. EdgeApp remains the sole
 * control/media protocol owner. The runner never manufactures desired State,
 * exposes a browser wire endpoint, changes sensor selection, configures the
 * sensor/ISP, daemonizes, installs autostart state or writes persistent media.
 */

enum {
    CONFIG_LINE_BYTES = 384,
    CONFIG_MAX_ROWS = 64,
    POLL_SLICE_MS = 10,
    CONTROL_IO_BYTES_PER_STEP = 4096,
    CONTROL_ADAPTER_RESERVE_BYTES = 65536,
    CONTROL_RUNTIME_RESERVE_BYTES = 400000,
    CONTROL_SCHEDULER_RESERVE_BYTES = 65536,
    MEDIA_IO_BYTES_PER_STEP = 4096,
    THERMAL_WORKER_BUDGET_MS = 100,
    ENDPOINT_COUNT = 6
};

typedef struct {
    char listen_ipv4[INET_ADDRSTRLEN];
    char interface_name[MEISO_WIFI_LINUX_INTERFACE_NAME_CAPACITY];
    uint16_t control_port;
    uint16_t media_port;
    MeisoPeerId local_edge_peer_id;
    MeisoPeerId expected_host_peer_id;
    uint64_t local_incarnation;
    uint64_t catalog_generation;
    uint64_t endpoint_incarnation_base;
} PrivateConfig;

typedef struct {
    pthread_t thread;
    atomic_bool done;
    bool active;
    MeisoCompositionEdgeAppThermalRequest request;
    MeisoCompositionEdgeAppThermalResult result;
    MeisoCompositionEdgeAppThermalReadStatus status;
} ThermalWorker;

typedef struct {
    PrivateConfig private_config;
    MeisoCompositionEdgeApp *app;
    void *app_arena;
    size_t app_arena_bytes;
    int control_listener;
    int media_listener;
    int control_fd;
    int media_fd;
    bool media_bound;
    uint64_t edge_clock_epoch;
    ThermalWorker thermal;
} Service;

typedef struct {
    bool app_initialized;
    MeisoCompositionEdgeAppStatus abort_status;
    bool snapshot_present;
    MeisoCompositionEdgeAppSnapshot snapshot;
    bool thermal_joined;
} FinalLedger;

typedef uint8_t PendingMediaCandidateStatus;
enum {
    PENDING_MEDIA_CANDIDATE_LIVE = 1,
    PENDING_MEDIA_CANDIDATE_CLOSED = 2
};

static volatile sig_atomic_t g_stop_requested = 0;

static void handle_stop_signal(int signal_number)
{
    g_stop_requested = (sig_atomic_t)signal_number;
}

static bool install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    return sigemptyset(&action.sa_mask) == 0 &&
        sigaction(SIGINT, &action, NULL) == 0 &&
        sigaction(SIGTERM, &action, NULL) == 0;
}

static void close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
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

/*
 * A media peer may retire a speculative connection while this runner is
 * waiting for the matching control-plane Actuals to become accepted.  Probe
 * that caller-owned candidate without consuming OPEN bytes.  POLLHUP/POLLERR
 * is terminal even when unread bytes remain; a positive MSG_PEEK is otherwise
 * a live candidate, and EAGAIN/EINTR means that no terminal condition was
 * observed.  The socket is nonblocking before it reaches this function.
 */
static PendingMediaCandidateStatus pending_media_candidate_status(
    int fd,
    short revents
)
{
    uint8_t byte;
    ssize_t received;

    if (fd < 0 || (revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
        return PENDING_MEDIA_CANDIDATE_CLOSED;
    }
    received = recv(fd, &byte, sizeof(byte), MSG_PEEK);
    if (received > 0) {
        return PENDING_MEDIA_CANDIDATE_LIVE;
    }
    if (received == 0) {
        return PENDING_MEDIA_CANDIDATE_CLOSED;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ?
        PENDING_MEDIA_CANDIDATE_LIVE : PENDING_MEDIA_CANDIDATE_CLOSED;
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
    if (*out_value == 0U) {
        *out_value = 1U;
    }
    return true;
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

static char *trim_text(char *text)
{
    char *end;
    while (*text == ' ' || *text == '\t' || *text == '\r' ||
           *text == '\n') {
        ++text;
    }
    end = text + strlen(text);
    while (end != text &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
            end[-1] == '\n')) {
        --end;
    }
    *end = '\0';
    return text;
}

static bool copy_text(char *output, size_t capacity, const char *input)
{
    const size_t length = input != NULL ? strlen(input) : 0U;
    if (output == NULL || capacity == 0U || input == NULL || length == 0U ||
        length >= capacity) {
        return false;
    }
    memcpy(output, input, length + 1U);
    return true;
}

static bool parse_u64(const char *text, uint64_t *out_value)
{
    char *end = NULL;
    uintmax_t value;
    if (text == NULL || out_value == NULL || *text == '\0' ||
        *text == '-' || *text == '+') {
        return false;
    }
    errno = 0;
    value = strtoumax(text, &end, 0);
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

static bool parse_port(const char *text, uint16_t *out_port)
{
    uint64_t value;
    if (!parse_u64(text, &value) || value == 0U || value > UINT16_MAX) {
        return false;
    }
    *out_port = (uint16_t)value;
    return true;
}

static bool parse_private_config(
    const char *path,
    PrivateConfig *out_config
)
{
    enum {
        HAVE_IPV4 = UINT16_C(1) << 0U,
        HAVE_CONTROL_PORT = UINT16_C(1) << 1U,
        HAVE_MEDIA_PORT = UINT16_C(1) << 2U,
        HAVE_INTERFACE = UINT16_C(1) << 3U,
        HAVE_LOCAL_HIGH = UINT16_C(1) << 4U,
        HAVE_LOCAL_LOW = UINT16_C(1) << 5U,
        HAVE_HOST_HIGH = UINT16_C(1) << 6U,
        HAVE_HOST_LOW = UINT16_C(1) << 7U,
        HAVE_LOCAL_INCARNATION = UINT16_C(1) << 8U,
        HAVE_CATALOG = UINT16_C(1) << 9U,
        HAVE_ENDPOINT_BASE = UINT16_C(1) << 10U,
        HAVE_ALL = (UINT16_C(1) << 11U) - 1U
    };
    PrivateConfig value;
    FILE *file;
    struct stat source_status;
    char line[CONFIG_LINE_BYTES];
    uint16_t present = 0U;
    size_t rows = 0U;
    bool valid = true;

    if (path == NULL || out_config == NULL ||
        stat(path, &source_status) != 0 ||
        !S_ISREG(source_status.st_mode)) {
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
        char *field;

        if (++rows > CONFIG_MAX_ROWS ||
            (strchr(line, '\n') == NULL && !feof(file))) {
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
        field = trim_text(equals + 1);
        if (strcmp(key, "listen_ipv4") == 0 &&
            (present & HAVE_IPV4) == 0U &&
            copy_text(value.listen_ipv4, sizeof(value.listen_ipv4), field)) {
            present |= HAVE_IPV4;
        } else if (strcmp(key, "control_port") == 0 &&
                   (present & HAVE_CONTROL_PORT) == 0U &&
                   parse_port(field, &value.control_port)) {
            present |= HAVE_CONTROL_PORT;
        } else if (strcmp(key, "media_port") == 0 &&
                   (present & HAVE_MEDIA_PORT) == 0U &&
                   parse_port(field, &value.media_port)) {
            present |= HAVE_MEDIA_PORT;
        } else if (strcmp(key, "interface") == 0 &&
                   (present & HAVE_INTERFACE) == 0U && copy_text(
                       value.interface_name, sizeof(value.interface_name),
                       field
                   )) {
            present |= HAVE_INTERFACE;
        } else if (strcmp(key, "local_edge_peer_id_high") == 0 &&
                   (present & HAVE_LOCAL_HIGH) == 0U &&
                   parse_u64(field, &value.local_edge_peer_id.high)) {
            present |= HAVE_LOCAL_HIGH;
        } else if (strcmp(key, "local_edge_peer_id_low") == 0 &&
                   (present & HAVE_LOCAL_LOW) == 0U &&
                   parse_u64(field, &value.local_edge_peer_id.low)) {
            present |= HAVE_LOCAL_LOW;
        } else if (strcmp(key, "expected_host_peer_id_high") == 0 &&
                   (present & HAVE_HOST_HIGH) == 0U &&
                   parse_u64(field, &value.expected_host_peer_id.high)) {
            present |= HAVE_HOST_HIGH;
        } else if (strcmp(key, "expected_host_peer_id_low") == 0 &&
                   (present & HAVE_HOST_LOW) == 0U &&
                   parse_u64(field, &value.expected_host_peer_id.low)) {
            present |= HAVE_HOST_LOW;
        } else if (strcmp(key, "local_incarnation") == 0 &&
                   (present & HAVE_LOCAL_INCARNATION) == 0U &&
                   parse_u64(field, &value.local_incarnation)) {
            present |= HAVE_LOCAL_INCARNATION;
        } else if (strcmp(key, "catalog_generation") == 0 &&
                   (present & HAVE_CATALOG) == 0U &&
                   parse_u64(field, &value.catalog_generation)) {
            present |= HAVE_CATALOG;
        } else if (strcmp(key, "endpoint_incarnation_base") == 0 &&
                   (present & HAVE_ENDPOINT_BASE) == 0U &&
                   parse_u64(field, &value.endpoint_incarnation_base)) {
            present |= HAVE_ENDPOINT_BASE;
        } else {
            valid = false;
        }
    }
    {
        const bool read_failed = ferror(file) != 0;
        const bool close_failed = fclose(file) != 0;
        if (read_failed || close_failed) {
            return false;
        }
    }
    if (!valid || present != HAVE_ALL ||
        value.control_port == value.media_port ||
        value.local_incarnation == 0U || value.catalog_generation == 0U ||
        value.endpoint_incarnation_base == 0U ||
        value.endpoint_incarnation_base > UINT64_MAX - (ENDPOINT_COUNT - 1U) ||
        !meiso_peer_id_valid(value.local_edge_peer_id) ||
        !meiso_peer_id_valid(value.expected_host_peer_id) ||
        meiso_peer_id_equal(
            value.local_edge_peer_id, value.expected_host_peer_id
        ) || inet_pton(
            AF_INET, value.listen_ipv4, &(struct in_addr){0}
        ) != 1) {
        return false;
    }
    *out_config = value;
    return true;
}

static int make_listener(const char *ipv4, uint16_t port, bool nonblocking)
{
    struct sockaddr_in address;
    int fd = -1;
    int enabled = 1;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (ipv4 == NULL ||
        inet_pton(AF_INET, ipv4, &address.sin_addr) != 1) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || !set_cloexec(fd) ||
        (nonblocking && !set_nonblocking(fd)) || setsockopt(
            fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
            (socklen_t)sizeof(enabled)
        ) != 0 || bind(
            fd, (const struct sockaddr *)(const void *)&address,
            (socklen_t)sizeof(address)
        ) != 0 || listen(fd, 1) != 0) {
        close_fd(&fd);
        return -1;
    }
    return fd;
}

static int accept_connected(int listener, bool wait_for_one)
{
    for (;;) {
        int fd = accept(listener, NULL, NULL);
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
        if (!wait_for_one && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return -2;
        }
        return -1;
    }
}

static void *thermal_worker_main(void *context)
{
    ThermalWorker *worker = context;
    worker->status = meiso_composition_edge_app_run_thermal_request(
        &worker->request, &worker->result
    );
    atomic_store_explicit(&worker->done, true, memory_order_release);
    return NULL;
}

static bool start_thermal_worker(Service *service)
{
    ThermalWorker *worker = &service->thermal;
    if (worker->active) {
        return true;
    }
    memset(&worker->request, 0, sizeof(worker->request));
    if (meiso_composition_edge_app_take_thermal_request(
            service->app, &worker->request
        ) != MEISO_COMPOSITION_EDGE_APP_OK) {
        return false;
    }
    memset(&worker->result, 0, sizeof(worker->result));
    worker->status =
        MEISO_COMPOSITION_EDGE_APP_THERMAL_READ_INVALID_ARGUMENT;
    atomic_store_explicit(&worker->done, false, memory_order_relaxed);
    worker->active = true;
    if (pthread_create(
            &worker->thread, NULL, thermal_worker_main, worker
        ) != 0) {
        worker->active = false;
        return false;
    }
    return true;
}

static bool collect_thermal_worker(Service *service, uint64_t now_ns)
{
    ThermalWorker *worker = &service->thermal;
    MeisoCompositionEdgeAppStatus status;
    if (!worker->active || !atomic_load_explicit(
            &worker->done, memory_order_acquire
        )) {
        return true;
    }
    if (pthread_join(worker->thread, NULL) != 0) {
        return false;
    }
    worker->active = false;
    if (worker->status != MEISO_COMPOSITION_EDGE_APP_THERMAL_READ_READY &&
        worker->status !=
            MEISO_COMPOSITION_EDGE_APP_THERMAL_READ_UNAVAILABLE) {
        return false;
    }
    status = meiso_composition_edge_app_submit_thermal_result(
        service->app, &worker->result,
        (MeisoTimePoint){
            worker->request.deadline.clock_epoch, now_ns
        }
    );
    memset(&worker->request, 0, sizeof(worker->request));
    memset(&worker->result, 0, sizeof(worker->result));
    return status == MEISO_COMPOSITION_EDGE_APP_OK ||
        status == MEISO_COMPOSITION_EDGE_APP_STALE;
}

static bool join_thermal_worker(Service *service)
{
    if (service->thermal.active) {
        if (pthread_join(service->thermal.thread, NULL) != 0) {
            return false;
        }
        service->thermal.active = false;
    }
    return true;
}

static const char *terminal_reason_name(
    MeisoCompositionEdgeAppTerminalReason reason
)
{
    switch (reason) {
    case MEISO_COMPOSITION_EDGE_APP_TERMINAL_NONE:
        return "NONE";
    case MEISO_COMPOSITION_EDGE_APP_TERMINAL_CONTROL_TRANSPORT:
        return "CONTROL_TRANSPORT";
    case MEISO_COMPOSITION_EDGE_APP_TERMINAL_CONTROL_SESSION:
        return "CONTROL_SESSION";
    case MEISO_COMPOSITION_EDGE_APP_TERMINAL_CATALOG_CHANGED:
        return "CATALOG_CHANGED";
    case MEISO_COMPOSITION_EDGE_APP_TERMINAL_MEDIA_TRANSPORT:
        return "MEDIA_TRANSPORT";
    case MEISO_COMPOSITION_EDGE_APP_TERMINAL_CAMERA_CLEANUP:
        return "CAMERA_CLEANUP";
    case MEISO_COMPOSITION_EDGE_APP_TERMINAL_LOCAL_ABORT:
        return "LOCAL_ABORT";
    case MEISO_COMPOSITION_EDGE_APP_TERMINAL_INTERNAL_FAILURE:
        return "INTERNAL_FAILURE";
    default:
        return "INVALID";
    }
}

static bool camera_ledger_clean(
    const MeisoCompositionEdgeAppSnapshot *snapshot
)
{
    return snapshot != NULL &&
        snapshot->camera.phase == MEISO_CAMERA_V4L2_PHASE_STOPPED &&
        snapshot->camera.driver_buffer_count == 0U &&
        snapshot->camera.mapped_buffer_count == 0U &&
        snapshot->camera.queued_buffer_count == 0U &&
        snapshot->camera.mapped_bytes == 0U &&
        !snapshot->camera.stream_on && !snapshot->camera.frame_borrowed &&
        !snapshot->media.bound;
}

static bool final_shutdown_clean(
    int stop_signal,
    bool loop_stopped,
    const FinalLedger *ledger
)
{
    if (stop_signal != SIGTERM || ledger == NULL ||
        !ledger->thermal_joined) {
        return false;
    }
    if (!ledger->app_initialized) {
        return true;
    }
    return loop_stopped &&
        ledger->abort_status == MEISO_COMPOSITION_EDGE_APP_OK &&
        ledger->snapshot_present &&
        ledger->snapshot.phase == MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL &&
        ledger->snapshot.terminal_reason ==
            MEISO_COMPOSITION_EDGE_APP_TERMINAL_LOCAL_ABORT &&
        camera_ledger_clean(&ledger->snapshot);
}

static bool initialize_app(Service *service)
{
    MeisoCompositionEdgeAppConfig config;
    MeisoSessionNonce nonce;
    uint64_t clock_epoch;
    size_t alignment;
    int allocation_status;

    if (!read_entropy(&nonce, sizeof(nonce)) ||
        !read_entropy(&clock_epoch, sizeof(clock_epoch))) {
        return false;
    }
    if (!meiso_session_nonce_valid(nonce)) {
        nonce.low = 1U;
    }
    if (clock_epoch == 0U) {
        clock_epoch = 1U;
    }
    service->edge_clock_epoch = clock_epoch;
    memset(&config, 0, sizeof(config));
    config.control_connected_fd = service->control_fd;
    config.local_peer_id = service->private_config.local_edge_peer_id;
    config.local_incarnation = service->private_config.local_incarnation;
    config.local_nonce = nonce;
    config.expected_host_peer_id =
        service->private_config.expected_host_peer_id;
    config.protocol = (MeisoProtocolVersion){1U, 0U};
    config.catalog_generation = service->private_config.catalog_generation;
    config.wifi_incarnation =
        service->private_config.endpoint_incarnation_base;
    config.presentation_incarnation =
        service->private_config.endpoint_incarnation_base + 1U;
    config.scene_incarnation =
        service->private_config.endpoint_incarnation_base + 2U;
    config.device_telemetry_incarnation =
        service->private_config.endpoint_incarnation_base + 3U;
    config.render_media_incarnation =
        service->private_config.endpoint_incarnation_base + 4U;
    config.camera_media_incarnation =
        service->private_config.endpoint_incarnation_base + 5U;
    if (!copy_text(
            config.wifi.interface_name, sizeof(config.wifi.interface_name),
            service->private_config.interface_name
        )) {
        return false;
    }
    config.wifi.allowed_fields = MEISO_WIFI_TELEMETRY_FIELD_ALL;
    config.wifi.sys_class_net_root = NULL;
    config.wifi.proc_net_wireless_path = NULL;
    meiso_device_telemetry_linux_adapter_default_config(
        MEISO_DEVICE_TELEMETRY_FIELD_ALL, &config.device_telemetry
    );
    meiso_camera_v4l2_default_config(clock_epoch, &config.camera);
    config.edge_clock_epoch = clock_epoch;
    config.max_control_io_bytes_per_step = CONTROL_IO_BYTES_PER_STEP;
    config.control_adapter_reserve_bytes = CONTROL_ADAPTER_RESERVE_BYTES;
    config.control_runtime_reserve_bytes = CONTROL_RUNTIME_RESERVE_BYTES;
    config.control_scheduler_reserve_bytes =
        CONTROL_SCHEDULER_RESERVE_BYTES;
    config.initial_actual_delivery_generation =
        (MeisoDeliveryGeneration){1U};
    config.media_connection_slots =
        MEISO_COMPOSITION_EDGE_APP_MAX_MEDIA_CONNECTIONS;
    config.max_media_io_bytes_per_step = MEDIA_IO_BYTES_PER_STEP;
    config.thermal_worker_budget_ms = THERMAL_WORKER_BUDGET_MS;
    service->app_arena_bytes = meiso_composition_edge_app_memory_required(
        &config
    );
    alignment = meiso_composition_edge_app_alignment_required();
    if (service->app_arena_bytes == 0U || alignment == 0U) {
        return false;
    }
    allocation_status = posix_memalign(
        &service->app_arena, alignment, service->app_arena_bytes
    );
    if (allocation_status != 0 || service->app_arena == NULL) {
        service->app_arena = NULL;
        return false;
    }
    memset(service->app_arena, 0, service->app_arena_bytes);
    return meiso_composition_edge_app_init(
        service->app_arena, service->app_arena_bytes, &config, &service->app
    ) == MEISO_COMPOSITION_EDGE_APP_INIT_OK;
}

static bool media_connection_step(Service *service)
{
    MeisoCompositionEdgeAppSnapshot snapshot;
    if (!meiso_composition_edge_app_snapshot(service->app, &snapshot)) {
        return false;
    }
    if (service->media_bound && !snapshot.media.bound) {
        service->media_bound = false;
        close_fd(&service->media_fd);
        (void)printf(
            "{\"type\":\"media\",\"phase\":\"close\","
            "\"status\":\"BOUNDARY\"}\n"
        );
        (void)fflush(stdout);
    }
    return true;
}

static bool media_candidate_bind_step(Service *service)
{
    if (service->media_fd >= 0 && !service->media_bound) {
        MeisoCompositionEdgeAppMediaBindReceipt receipt;
        const MeisoCompositionEdgeAppStatus status =
            meiso_composition_edge_app_bind_media_fd(
                service->app, service->media_fd, &receipt
            );
        if (status == MEISO_COMPOSITION_EDGE_APP_OK) {
            service->media_bound = true;
            (void)printf(
                "{\"type\":\"media\",\"phase\":\"bind\","
                "\"status\":\"READY\",\"slot\":%u,\"mask\":%u}\n",
                (unsigned)receipt.slot_index,
                (unsigned)receipt.active_route_mask
            );
            (void)fflush(stdout);
        } else if (status != MEISO_COMPOSITION_EDGE_APP_NOT_READY &&
                   status != MEISO_COMPOSITION_EDGE_APP_BUSY) {
            close_fd(&service->media_fd);
            if (status == MEISO_COMPOSITION_EDGE_APP_CAPACITY) {
                close_fd(&service->media_listener);
            }
        }
    }
    return true;
}

static bool run_service(Service *service)
{
    while (!g_stop_requested) {
        MeisoCompositionEdgeAppIoInterest interest;
        MeisoCompositionEdgeAppIoReady ready;
        MeisoCompositionEdgeAppPumpResult result;
        struct pollfd descriptors[3];
        nfds_t descriptor_count = 0U;
        int control_index = -1;
        int listener_index = -1;
        int media_index = -1;
        uint64_t now_ns;
        int poll_status;
        MeisoCompositionEdgeAppPumpStatus pump_status;

        if (!monotonic_now_ns(&now_ns) ||
            !collect_thermal_worker(service, now_ns) ||
            !media_connection_step(service) ||
            !meiso_composition_edge_app_readiness(
                service->app, &interest
            )) {
            return false;
        }
        memset(descriptors, 0, sizeof(descriptors));
        control_index = (int)descriptor_count;
        descriptors[descriptor_count].fd = service->control_fd;
        descriptors[descriptor_count].events = (short)(
            (interest.control_read ? POLLIN : 0) |
            (interest.control_write ? POLLOUT : 0)
        );
        ++descriptor_count;
        if (service->media_listener >= 0 && service->media_fd < 0) {
            listener_index = (int)descriptor_count;
            descriptors[descriptor_count].fd = service->media_listener;
            descriptors[descriptor_count].events = POLLIN;
            ++descriptor_count;
        }
        if (service->media_fd >= 0) {
            media_index = (int)descriptor_count;
            descriptors[descriptor_count].fd = service->media_fd;
            descriptors[descriptor_count].events = service->media_bound ?
                (short)((interest.media_read ? POLLIN : 0) |
                        (interest.media_write ? POLLOUT : 0)) :
                POLLIN;
            ++descriptor_count;
        }
        poll_status = poll(
            descriptors, descriptor_count, POLL_SLICE_MS
        );
        if (poll_status < 0 && errno != EINTR) {
            return false;
        }
        if (listener_index >= 0 &&
            (descriptors[listener_index].revents & POLLIN) != 0) {
            const int accepted = accept_connected(
                service->media_listener, false
            );
            if (accepted >= 0) {
                service->media_fd = accepted;
                (void)printf(
                    "{\"type\":\"media\",\"phase\":\"accept\","
                    "\"status\":\"CONNECTED\"}\n"
                );
                (void)fflush(stdout);
            } else if (accepted != -2) {
                return false;
            }
        }
        if (!monotonic_now_ns(&now_ns)) {
            return false;
        }
        memset(&ready, 0, sizeof(ready));
        if ((control_index >= 0 &&
             (descriptors[control_index].revents & POLLNVAL) != 0) ||
            (listener_index >= 0 &&
             (descriptors[listener_index].revents & POLLNVAL) != 0)) {
            return false;
        }
        if (media_index >= 0 && !service->media_bound) {
            if (pending_media_candidate_status(
                    service->media_fd,
                    descriptors[media_index].revents
                ) == PENDING_MEDIA_CANDIDATE_CLOSED) {
                close_fd(&service->media_fd);
            } else if (!media_candidate_bind_step(service)) {
                return false;
            }
        } else if (media_index >= 0 &&
                   (descriptors[media_index].revents & POLLNVAL) != 0) {
            return false;
        }
        ready.control_readable = control_index >= 0 &&
            (descriptors[control_index].revents &
             (POLLIN | POLLHUP | POLLERR)) != 0;
        ready.control_writable = control_index >= 0 &&
            (descriptors[control_index].revents & POLLOUT) != 0;
        ready.media_readable = media_index >= 0 && service->media_bound &&
            (descriptors[media_index].revents &
             (POLLIN | POLLHUP | POLLERR)) != 0;
        ready.media_writable = media_index >= 0 && service->media_bound &&
            (descriptors[media_index].revents & POLLOUT) != 0;
        ready.camera_poll_allowed = interest.camera_poll_useful;
        ready.now = (MeisoTimePoint){service->edge_clock_epoch, now_ns};
        memset(&result, 0, sizeof(result));
        pump_status = meiso_composition_edge_app_pump(
            service->app, &ready, &result
        );
        if (pump_status == MEISO_COMPOSITION_EDGE_APP_PUMP_TERMINAL) {
            return false;
        }
        if (pump_status == MEISO_COMPOSITION_EDGE_APP_PUMP_INVALID_ARGUMENT ||
            pump_status ==
                MEISO_COMPOSITION_EDGE_APP_PUMP_INTERNAL_FAILURE) {
            return false;
        }
        if (!service->thermal.active) {
            MeisoCompositionEdgeAppIoInterest current_interest;
            if (!meiso_composition_edge_app_readiness(
                    service->app, &current_interest
                ) || (current_interest.thermal_request_ready &&
                       !start_thermal_worker(service))) {
                return false;
            }
        }
    }
    return true;
}

static void usage(const char *program)
{
    (void)fprintf(
        stderr, "usage: %s --config PATH\n",
        program != NULL ? program : "composition_debug_edge"
    );
}

#if defined(MEISO_COMPOSITION_DEBUG_EDGE_HOSTED_LIFECYCLE_TEST)
static bool pending_media_candidate_self_test(void)
{
    int pair[2] = {-1, -1};
    struct pollfd descriptor;
    uint8_t sent = UINT8_C(0x4d);
    uint8_t received = 0U;
    bool passed = false;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 ||
        !set_nonblocking(pair[0]) || !set_nonblocking(pair[1]) ||
        pending_media_candidate_status(pair[0], 0) !=
            PENDING_MEDIA_CANDIDATE_LIVE ||
        send(pair[1], &sent, sizeof(sent), 0) != (ssize_t)sizeof(sent)) {
        goto cleanup;
    }
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = pair[0];
    descriptor.events = POLLIN;
    if (poll(&descriptor, 1U, 100) != 1 ||
        pending_media_candidate_status(pair[0], descriptor.revents) !=
            PENDING_MEDIA_CANDIDATE_LIVE ||
        recv(pair[0], &received, sizeof(received), 0) !=
            (ssize_t)sizeof(received) ||
        received != sent) {
        goto cleanup;
    }
    close_fd(&pair[1]);
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = pair[0];
    descriptor.events = POLLIN;
    if (poll(&descriptor, 1U, 100) != 1 ||
        pending_media_candidate_status(pair[0], descriptor.revents) !=
            PENDING_MEDIA_CANDIDATE_CLOSED) {
        goto cleanup;
    }
    close_fd(&pair[0]);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 ||
        !set_nonblocking(pair[0]) || !set_nonblocking(pair[1]) ||
        send(pair[1], &sent, sizeof(sent), 0) != (ssize_t)sizeof(sent)) {
        goto cleanup;
    }
    close_fd(&pair[1]);
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = pair[0];
    descriptor.events = POLLIN;
    if (poll(&descriptor, 1U, 100) != 1 ||
        pending_media_candidate_status(pair[0], descriptor.revents) !=
            PENDING_MEDIA_CANDIDATE_CLOSED) {
        goto cleanup;
    }
    passed = true;

cleanup:
    close_fd(&pair[1]);
    close_fd(&pair[0]);
    return passed;
}

static int run_hosted_lifecycle_test(void)
{
    FinalLedger ledger;

    if (!pending_media_candidate_self_test()) {
        return 1;
    }
    memset(&ledger, 0, sizeof(ledger));
    ledger.app_initialized = true;
    ledger.abort_status = MEISO_COMPOSITION_EDGE_APP_OK;
    ledger.snapshot_present = true;
    ledger.snapshot.phase = MEISO_COMPOSITION_EDGE_APP_PHASE_TERMINAL;
    ledger.snapshot.terminal_reason =
        MEISO_COMPOSITION_EDGE_APP_TERMINAL_LOCAL_ABORT;
    ledger.snapshot.camera.phase = MEISO_CAMERA_V4L2_PHASE_STOPPED;
    ledger.thermal_joined = true;
    if (!final_shutdown_clean(SIGTERM, true, &ledger)) {
        return 1;
    }
    ledger.snapshot.terminal_reason =
        MEISO_COMPOSITION_EDGE_APP_TERMINAL_CONTROL_TRANSPORT;
    if (final_shutdown_clean(SIGTERM, true, &ledger)) {
        return 1;
    }
    ledger.snapshot.terminal_reason =
        MEISO_COMPOSITION_EDGE_APP_TERMINAL_LOCAL_ABORT;
    ledger.snapshot.camera.mapped_buffer_count = 1U;
    if (final_shutdown_clean(SIGTERM, true, &ledger)) {
        return 1;
    }
    ledger.snapshot.camera.mapped_buffer_count = 0U;
    ledger.snapshot.media.bound = true;
    if (final_shutdown_clean(SIGTERM, true, &ledger)) {
        return 1;
    }
    ledger.snapshot.media.bound = false;
    if (final_shutdown_clean(SIGINT, true, &ledger) ||
        final_shutdown_clean(SIGTERM, false, &ledger)) {
        return 1;
    }
    ledger.app_initialized = false;
    ledger.snapshot_present = false;
    if (!final_shutdown_clean(SIGTERM, false, &ledger)) {
        return 1;
    }
    puts("composition_debug_edge_lifecycle PASS");
    return 0;
}
#endif

int main(int argc, char **argv)
{
    Service service;
    FinalLedger ledger;
    bool success = false;
    bool clean_shutdown;
    MeisoCompositionEdgeAppTerminalReason receipt_reason =
        MEISO_COMPOSITION_EDGE_APP_TERMINAL_NONE;

    memset(&service, 0, sizeof(service));
    memset(&ledger, 0, sizeof(ledger));
    service.control_listener = -1;
    service.media_listener = -1;
    service.control_fd = -1;
    service.media_fd = -1;
    atomic_init(&service.thermal.done, false);
#if defined(MEISO_COMPOSITION_DEBUG_EDGE_HOSTED_LIFECYCLE_TEST)
    if (argc == 2 && strcmp(argv[1], "--lifecycle-self-test") == 0) {
        return run_hosted_lifecycle_test();
    }
#endif
    if (argc != 3 || strcmp(argv[1], "--config") != 0) {
        usage(argv[0]);
        return 2;
    }
    if (!install_signal_handlers() ||
        !parse_private_config(argv[2], &service.private_config)) {
        fputs("composition Edge config/bootstrap failed\n", stderr);
        return 1;
    }
    service.control_listener = make_listener(
        service.private_config.listen_ipv4,
        service.private_config.control_port, false
    );
    service.media_listener = make_listener(
        service.private_config.listen_ipv4,
        service.private_config.media_port, true
    );
    if (service.control_listener < 0 || service.media_listener < 0) {
        fputs("composition Edge listen failed\n", stderr);
        goto cleanup;
    }
    (void)printf(
        "{\"type\":\"edge\",\"phase\":\"listen\","
        "\"status\":\"READY\"}\n"
    );
    (void)fflush(stdout);
    service.control_fd = accept_connected(service.control_listener, true);
    close_fd(&service.control_listener);
    if (service.control_fd < 0) {
        if (!g_stop_requested) {
            fputs("composition Edge control accept failed\n", stderr);
        }
        goto cleanup;
    }
    if (!initialize_app(&service)) {
        fputs("composition Edge app init failed\n", stderr);
        goto cleanup;
    }
    (void)printf(
        "{\"type\":\"control\",\"phase\":\"accept\","
        "\"status\":\"CONNECTED\"}\n"
    );
    (void)fflush(stdout);
    success = run_service(&service);

cleanup:
    if (service.app != NULL) {
        ledger.app_initialized = true;
        ledger.abort_status = meiso_composition_edge_app_abort(
            service.app,
            g_stop_requested != 0 ?
                MEISO_COMPOSITION_EDGE_APP_TERMINAL_LOCAL_ABORT :
                MEISO_COMPOSITION_EDGE_APP_TERMINAL_INTERNAL_FAILURE,
            g_stop_requested != 0 ? 0 : EPROTO
        );
        ledger.snapshot_present = meiso_composition_edge_app_snapshot(
            service.app, &ledger.snapshot
        );
    }
    ledger.thermal_joined = join_thermal_worker(&service);
    clean_shutdown = final_shutdown_clean(
        (int)g_stop_requested, success, &ledger
    );
    if (ledger.snapshot_present) {
        receipt_reason = ledger.snapshot.terminal_reason;
    } else if (g_stop_requested != 0) {
        receipt_reason = MEISO_COMPOSITION_EDGE_APP_TERMINAL_LOCAL_ABORT;
    }
    close_fd(&service.media_fd);
    close_fd(&service.control_fd);
    close_fd(&service.media_listener);
    close_fd(&service.control_listener);
    free(service.app_arena);
    (void)printf(
        "{\"type\":\"edge\",\"phase\":\"close\","
        "\"status\":\"%s\",\"reason\":\"%s\","
        "\"snapshot\":%s,\"camera_phase\":%u,"
        "\"camera_driver_buffers\":%u,\"camera_mapped\":%u,"
        "\"camera_mapped_bytes\":%" PRIu64 ","
        "\"camera_queued\":%u,\"camera_borrowed\":%s,"
        "\"camera_stream_on\":%s,\"media_bound\":%s}\n",
        clean_shutdown ? "SHUTDOWN" : "FAILED",
        terminal_reason_name(receipt_reason),
        ledger.snapshot_present ? "true" : "false",
        ledger.snapshot_present ? (unsigned)ledger.snapshot.camera.phase : 0U,
        ledger.snapshot_present ?
            (unsigned)ledger.snapshot.camera.driver_buffer_count : 0U,
        ledger.snapshot_present ?
            (unsigned)ledger.snapshot.camera.mapped_buffer_count : 0U,
        ledger.snapshot_present ? ledger.snapshot.camera.mapped_bytes : 0U,
        ledger.snapshot_present ?
            (unsigned)ledger.snapshot.camera.queued_buffer_count : 0U,
        ledger.snapshot_present && ledger.snapshot.camera.frame_borrowed ?
            "true" : "false",
        ledger.snapshot_present && ledger.snapshot.camera.stream_on ?
            "true" : "false",
        ledger.snapshot_present && ledger.snapshot.media.bound ?
            "true" : "false"
    );
    return clean_shutdown ? 0 : 1;
}
