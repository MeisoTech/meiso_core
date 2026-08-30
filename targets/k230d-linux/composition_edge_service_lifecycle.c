#define _POSIX_C_SOURCE 200809L

#include "meiso_k230d_composition_edge_service_private.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/*
 * Owns signals, descriptors, listeners, runtime arena initialization, the
 * bounded thermal worker, and shutdown-proof evaluation. Protocol work stays
 * in Edge Runtime; this layer only manages process-owned resources.
 */

volatile sig_atomic_t
    meiso_k230d_composition_edge_service_private_g_stop_requested = 0;

static void handle_stop_signal(int signal_number)
{
    meiso_k230d_composition_edge_service_private_g_stop_requested =
        (sig_atomic_t)signal_number;
}

bool meiso_k230d_composition_edge_service_private_install_signal_handlers(
    void
)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    return sigemptyset(&action.sa_mask) == 0 &&
        sigaction(SIGINT, &action, NULL) == 0 &&
        sigaction(SIGTERM, &action, NULL) == 0;
}

void meiso_k230d_composition_edge_service_private_close_fd(int *fd)
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

bool meiso_k230d_composition_edge_service_private_set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool meiso_k230d_composition_edge_service_private_monotonic_now_ns(
    uint64_t *out_value
)
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
            meiso_k230d_composition_edge_service_private_close_fd(&fd);
            return false;
        }
    }
    meiso_k230d_composition_edge_service_private_close_fd(&fd);
    return true;
}

int meiso_k230d_composition_edge_service_private_make_listener(
    const char *ipv4,
    uint16_t port,
    bool nonblocking
)
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
        (nonblocking &&
         !meiso_k230d_composition_edge_service_private_set_nonblocking(fd)) ||
        setsockopt(
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
        meiso_k230d_composition_edge_service_private_close_fd(&fd);
        return -1;
    }
    return fd;
}

int meiso_k230d_composition_edge_service_private_accept_connected(
    int listener,
    bool wait_for_one
)
{
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd >= 0) {
            if (set_cloexec(fd) &&
                meiso_k230d_composition_edge_service_private_set_nonblocking(
                    fd
                )) {
                return fd;
            }
            meiso_k230d_composition_edge_service_private_close_fd(&fd);
            return -1;
        }
        if (errno == EINTR &&
            !meiso_k230d_composition_edge_service_private_g_stop_requested) {
            continue;
        }
        if (!wait_for_one &&
            (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return -2;
        }
        return -1;
    }
}

static void *thermal_worker_main(void *context)
{
    K230dCompositionEdgeThermalWorker *worker = context;
    worker->status = meiso_k230d_composition_edge_runtime_run_thermal_request(
        &worker->request, &worker->result
    );
    atomic_store_explicit(&worker->done, true, memory_order_release);
    return NULL;
}

bool meiso_k230d_composition_edge_service_private_start_thermal_worker(
    K230dCompositionEdgeService *service
)
{
    K230dCompositionEdgeThermalWorker *worker = &service->thermal;
    if (worker->active) {
        return true;
    }
    memset(&worker->request, 0, sizeof(worker->request));
    if (meiso_k230d_composition_edge_runtime_take_thermal_request(
            service->edge_runtime, &worker->request
        ) != MEISO_K230D_COMPOSITION_EDGE_RUNTIME_OK) {
        return false;
    }
    memset(&worker->result, 0, sizeof(worker->result));
    worker->status =
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_THERMAL_READ_INVALID_ARGUMENT;
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

bool meiso_k230d_composition_edge_service_private_collect_thermal_worker(
    K230dCompositionEdgeService *service,
    uint64_t now_ns
)
{
    K230dCompositionEdgeThermalWorker *worker = &service->thermal;
    MeisoK230dCompositionEdgeRuntimeStatus status;
    if (!worker->active || !atomic_load_explicit(
            &worker->done, memory_order_acquire
        )) {
        return true;
    }
    if (pthread_join(worker->thread, NULL) != 0) {
        return false;
    }
    worker->active = false;
    if (worker->status !=
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_THERMAL_READ_READY &&
        worker->status !=
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_THERMAL_READ_UNAVAILABLE) {
        return false;
    }
    status = meiso_k230d_composition_edge_runtime_submit_thermal_result(
        service->edge_runtime,
        &worker->result,
        (MeisoTimePoint){
            worker->request.deadline.clock_epoch, now_ns
        }
    );
    memset(&worker->request, 0, sizeof(worker->request));
    memset(&worker->result, 0, sizeof(worker->result));
    return status == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_OK ||
        status == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_STALE;
}

bool meiso_k230d_composition_edge_service_private_join_thermal_worker(
    K230dCompositionEdgeService *service
)
{
    if (service->thermal.active) {
        if (pthread_join(service->thermal.thread, NULL) != 0) {
            return false;
        }
        service->thermal.active = false;
    }
    return true;
}

const char *meiso_k230d_composition_edge_service_private_terminal_reason_name(
    MeisoK230dCompositionEdgeRuntimeTerminalReason reason
)
{
    switch (reason) {
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_NONE:
        return "NONE";
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CONTROL_TRANSPORT:
        return "CONTROL_TRANSPORT";
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CONTROL_SESSION:
        return "CONTROL_SESSION";
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CATALOG_CHANGED:
        return "CATALOG_CHANGED";
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_MEDIA_TRANSPORT:
        return "MEDIA_TRANSPORT";
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CAMERA_CLEANUP:
        return "CAMERA_CLEANUP";
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_LOCAL_ABORT:
        return "LOCAL_ABORT";
    case MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_INTERNAL_FAILURE:
        return "INTERNAL_FAILURE";
    default:
        return "INVALID";
    }
}

static bool camera_ledger_clean(
    const MeisoK230dCompositionEdgeRuntimeSnapshot *snapshot
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

bool meiso_k230d_composition_edge_service_private_final_shutdown_clean(
    int stop_signal,
    bool loop_stopped,
    const K230dCompositionEdgeShutdownLedger *ledger
)
{
    if (stop_signal != SIGTERM || ledger == NULL ||
        !ledger->thermal_joined) {
        return false;
    }
    if (!ledger->runtime_initialized) {
        return true;
    }
    return loop_stopped &&
        ledger->abort_status == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_OK &&
        ledger->snapshot_present &&
        ledger->snapshot.phase ==
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL &&
        ledger->snapshot.terminal_reason ==
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_LOCAL_ABORT &&
        camera_ledger_clean(&ledger->snapshot);
}

bool meiso_k230d_composition_edge_service_private_initialize_edge_runtime(
    K230dCompositionEdgeService *service
)
{
    MeisoK230dCompositionEdgeRuntimeConfig config;
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
    if (!meiso_k230d_composition_edge_service_private_copy_text(
            config.wifi.interface_name,
            sizeof(config.wifi.interface_name),
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
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MAX_MEDIA_CONNECTIONS;
    config.max_media_io_bytes_per_step = MEDIA_IO_BYTES_PER_STEP;
    config.thermal_worker_budget_ms = THERMAL_WORKER_BUDGET_MS;
    service->edge_runtime_arena_bytes =
        meiso_k230d_composition_edge_runtime_memory_required(&config);
    alignment = meiso_k230d_composition_edge_runtime_alignment_required();
    if (service->edge_runtime_arena_bytes == 0U || alignment == 0U) {
        return false;
    }
    allocation_status = posix_memalign(
        &service->edge_runtime_arena,
        alignment,
        service->edge_runtime_arena_bytes
    );
    if (allocation_status != 0 || service->edge_runtime_arena == NULL) {
        service->edge_runtime_arena = NULL;
        return false;
    }
    memset(
        service->edge_runtime_arena,
        0,
        service->edge_runtime_arena_bytes
    );
    return meiso_k230d_composition_edge_runtime_init(
        service->edge_runtime_arena,
        service->edge_runtime_arena_bytes,
        &config,
        &service->edge_runtime
    ) == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INIT_OK;
}
