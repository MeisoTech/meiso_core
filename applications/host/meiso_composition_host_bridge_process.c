#include "meiso_composition_host_bridge_private.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Backend lifecycle, bounded pumping, polling, and shutdown. */

static uint64_t mix_nonzero_epoch(uint64_t value)
{
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31U;
    return value == 0U ? 1U : value;
}

static bool pump_host(
    MeisoCompositionHostBridgePrivateProcess *service,
    const MeisoCompositionHostRuntimeIoReady *ready,
    bool *out_progress
)
{
    MeisoCompositionHostRuntimePumpResult result;
    MeisoCompositionHostRuntimePumpStatus status;

    *out_progress = false;
    if (service->backend == MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_BACKEND_SIMULATOR) {
        MeisoCompositionEdgeSimulatorTransition transition;
        MeisoEdgeStatus edge_status;
        uint64_t next_time;

        (void)ready;
        if (service->simulator_monotonic_ns == UINT64_MAX) {
            return false;
        }
        next_time = service->simulator_monotonic_ns + 1U;
        edge_status = meiso_composition_edge_simulator_step(
            service->edge,
            (MeisoTimePoint){service->host_clock_epoch, next_time},
            &transition
        );
        if (edge_status == MEISO_EDGE_TERMINAL) {
            service->close_requested = true;
            return meiso_composition_host_bridge_private_emit_line(
                "{\"type\":\"terminal\",\"code\":\"SIMULATOR_TERMINAL\"}"
            );
        }
        if (edge_status != MEISO_EDGE_OK) {
            return false;
        }
        if (transition == MEISO_COMPOSITION_EDGE_SIMULATOR_IDLE) {
            return true;
        }
        service->simulator_monotonic_ns = next_time;
        *out_progress = true;
        return meiso_composition_host_bridge_private_emit_typed_truth(service);
    }
    status = meiso_composition_host_runtime_pump(service->host_runtime, ready, &result);
    if (status == MEISO_COMPOSITION_HOST_RUNTIME_PUMP_IDLE ||
        status == MEISO_COMPOSITION_HOST_RUNTIME_PUMP_WOULD_BLOCK) {
        return true;
    }
    if (status == MEISO_COMPOSITION_HOST_RUNTIME_PUMP_TERMINAL) {
        char line[96];
        int written;

        service->close_requested = true;
        written = snprintf(
            line, sizeof(line),
            "{\"type\":\"terminal\",\"code\":\"%s\"}",
            meiso_composition_host_bridge_private_host_terminal_code(service)
        );
        return written > 0 && (size_t)written < sizeof(line) &&
            meiso_composition_host_bridge_private_emit_line(line);
    }
    if (status != MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS) {
        return false;
    }
    *out_progress = true;
    if (result.event_present && !meiso_composition_host_bridge_private_emit_pump_result(&result)) {
        return false;
    }
    if (result.event_present &&
        (result.event == MEISO_COMPOSITION_HOST_RUNTIME_PUMP_ACTUAL_UPDATED ||
         result.event == MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INLINE_UPDATED ||
         result.event == MEISO_COMPOSITION_HOST_RUNTIME_PUMP_BOUNDARY)) {
        return meiso_composition_host_bridge_private_emit_typed_truth(service);
    }
    return true;
}

static bool process_buffered_line(MeisoCompositionHostBridgePrivateProcess *service, bool *out_processed)
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
    if (!meiso_composition_host_bridge_private_handle_command(service, service->input, line_size)) {
        return false;
    }
    remaining = service->input_size - ((size_t)(newline - service->input) + 1U);
    memmove(service->input, newline + 1, remaining);
    service->input_size = remaining;
    *out_processed = true;
    return true;
}

static bool read_control_input(MeisoCompositionHostBridgePrivateProcess *service, bool readable)
{
    ssize_t result;

    if (!readable) {
        return true;
    }
    if (service->input_size >= sizeof(service->input)) {
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

bool meiso_composition_host_bridge_private_initialize_remote_process(
    MeisoCompositionHostBridgePrivateProcess *service,
    const MeisoCompositionHostBridgePrivateConfig *config,
    int frame_fd
)
{
    MeisoCompositionHostRuntimeConfig host_runtime_config;
    MeisoCompositionHostRuntimeInitStatus init_status;
    MeisoCompositionRemoteEdgeConfig edge_config;
    MeisoCompositionRemoteEdgeInitStatus edge_init_status;
    size_t alignment;
    size_t edge_alignment;
    void *arena = NULL;
    void *edge_arena = NULL;

    if (service == NULL || config == NULL || !frame_fd_valid(frame_fd)) {
        return false;
    }
    memset(service, 0, sizeof(*service));
    service->backend = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_BACKEND_REMOTE;
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
    service->control_fd = meiso_composition_host_bridge_private_connect_numeric(
        &config->control_address, config->control_address_size
    );
    if (service->control_fd < 0 || service->control_fd == frame_fd) {
        meiso_composition_host_bridge_private_close_fd(&service->control_fd);
        return false;
    }
    memset(&host_runtime_config, 0, sizeof(host_runtime_config));
    host_runtime_config.control_connected_fd = service->control_fd;
    host_runtime_config.local_peer_id = config->local_peer_id;
    host_runtime_config.local_incarnation = config->local_incarnation;
    host_runtime_config.local_nonce = config->nonce;
    host_runtime_config.expected_edge_peer_id = config->edge_peer_id;
    host_runtime_config.protocol = (MeisoProtocolVersion){1U, 0U};
    host_runtime_config.catalog_generation = config->catalog_generation;
    host_runtime_config.catalog = config->catalog;
    host_runtime_config.max_control_io_bytes_per_step = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_IO_STEP_BYTES;
    host_runtime_config.control_adapter_reserve_bytes = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONTROL_ADAPTER_BYTES;
    host_runtime_config.control_runtime_reserve_bytes = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONTROL_RUNTIME_BYTES;
    host_runtime_config.control_scheduler_reserve_bytes = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONTROL_SCHEDULER_BYTES;
    host_runtime_config.initial_desired_delivery_generation =
        (MeisoDeliveryGeneration){101U};
    host_runtime_config.media_connection_slots = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_MEDIA_CONNECTION_SLOTS;
    host_runtime_config.max_media_io_bytes_per_step = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_IO_STEP_BYTES;
    service->host_runtime_arena_size =
        meiso_composition_host_runtime_memory_required(&host_runtime_config);
    alignment = meiso_composition_host_runtime_alignment_required();
    if (service->host_runtime_arena_size == 0U || alignment == 0U ||
        posix_memalign(&arena, alignment, service->host_runtime_arena_size) != 0) {
        meiso_composition_host_bridge_private_close_fd(&service->control_fd);
        return false;
    }
    memset(arena, 0, service->host_runtime_arena_size);
    init_status = meiso_composition_host_runtime_init(
        arena, service->host_runtime_arena_size, &host_runtime_config, &service->host_runtime
    );
    if (init_status != MEISO_COMPOSITION_HOST_RUNTIME_INIT_OK) {
        free(arena);
        service->host_runtime = NULL;
        meiso_composition_host_bridge_private_close_fd(&service->control_fd);
        return false;
    }
    service->host_runtime_arena = arena;
    service->edge_arena_size =
        meiso_composition_remote_edge_memory_required();
    edge_alignment = meiso_composition_remote_edge_alignment_required();
    if (service->edge_arena_size == 0U || edge_alignment == 0U ||
        posix_memalign(&edge_arena, edge_alignment,
                       service->edge_arena_size) != 0) {
        (void)meiso_composition_host_runtime_abort(
            service->host_runtime,
            MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_LOCAL_ABORT, 0
        );
        free(service->host_runtime_arena);
        service->host_runtime_arena = NULL;
        service->host_runtime = NULL;
        meiso_composition_host_bridge_private_close_fd(&service->control_fd);
        return false;
    }
    memset(edge_arena, 0, service->edge_arena_size);
    memset(&edge_config, 0, sizeof(edge_config));
    edge_config.host_runtime = service->host_runtime;
    edge_config.host_runtime_arena = service->host_runtime_arena;
    edge_config.host_runtime_arena_size = service->host_runtime_arena_size;
    edge_config.catalog_generation = config->catalog_generation;
    edge_config.catalog = config->catalog;
    edge_config.continuity_serial = mix_nonzero_epoch(
        service->host_clock_epoch ^ UINT64_C(0x4544474548414e44)
    );
    edge_init_status = meiso_composition_remote_edge_init(
        edge_arena, service->edge_arena_size, &edge_config, &service->edge
    );
    if (edge_init_status != MEISO_COMPOSITION_REMOTE_EDGE_INIT_OK) {
        (void)meiso_composition_host_runtime_abort(
            service->host_runtime,
            MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_LOCAL_ABORT, 0
        );
        free(edge_arena);
        free(service->host_runtime_arena);
        service->edge = NULL;
        service->host_runtime_arena = NULL;
        service->host_runtime = NULL;
        meiso_composition_host_bridge_private_close_fd(&service->control_fd);
        return false;
    }
    service->edge_arena = edge_arena;
    return true;
}

bool meiso_composition_host_bridge_private_initialize_simulator_process(MeisoCompositionHostBridgePrivateProcess *service, int frame_fd)
{
    MeisoCompositionEdgeSimulatorConfig config = meiso_composition_host_bridge_private_offline_simulator_config();
    MeisoCompositionEdgeSimulatorInitStatus init_status;
    size_t alignment;
    void *arena = NULL;

    if (service == NULL || !frame_fd_valid(frame_fd)) {
        return false;
    }
    memset(service, 0, sizeof(*service));
    service->backend = MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_BACKEND_SIMULATOR;
    service->control_fd = -1;
    service->media_fd = -1;
    service->frame_fd = frame_fd;
    service->config.catalog_generation = config.catalog_generation;
    service->config.catalog = config.catalog;
    service->config.local_incarnation = config.desired_owner.incarnation;
    service->host_clock_epoch = config.composition_clock_epoch;
    service->scene_space_epoch = UINT64_C(5002);
    service->simulator_monotonic_ns = 0U;
    service->edge_arena_size =
        meiso_composition_edge_simulator_memory_required(&config);
    alignment = meiso_composition_edge_simulator_alignment_required();
    if (service->edge_arena_size == 0U || alignment == 0U ||
        posix_memalign(&arena, alignment, service->edge_arena_size) != 0) {
        return false;
    }
    memset(arena, 0, service->edge_arena_size);
    init_status = meiso_composition_edge_simulator_init(
        arena, service->edge_arena_size, &config, &service->edge
    );
    if (init_status != MEISO_COMPOSITION_EDGE_SIMULATOR_INIT_OK ||
        meiso_composition_edge_simulator_activate(service->edge) !=
            MEISO_EDGE_OK) {
        free(arena);
        service->edge = NULL;
        return false;
    }
    service->edge_arena = arena;
    return true;
}

void meiso_composition_host_bridge_private_destroy_process(MeisoCompositionHostBridgePrivateProcess *service)
{
    if (service == NULL) {
        return;
    }
    if (service->backend == MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_BACKEND_SIMULATOR &&
        service->edge != NULL) {
        (void)meiso_composition_edge_simulator_terminate(
            service->edge, MEISO_EDGE_TERMINAL_LOCAL_ABORT
        );
    } else if (service->host_runtime != NULL) {
        (void)meiso_composition_host_runtime_abort(
            service->host_runtime, MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL_LOCAL_ABORT, 0
        );
    }
    meiso_composition_host_bridge_private_close_fd(&service->media_fd);
    meiso_composition_host_bridge_private_close_fd(&service->control_fd);
    free(service->edge_arena);
    free(service->host_runtime_arena);
    service->edge_arena = NULL;
    service->host_runtime_arena = NULL;
    service->edge = NULL;
    service->host_runtime = NULL;
}

bool meiso_composition_host_bridge_private_run_loop(MeisoCompositionHostBridgePrivateProcess *service)
{
    bool progress;

    if (!meiso_composition_host_bridge_private_emit_line(service->backend == MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_BACKEND_SIMULATOR ?
            "{\"type\":\"service.ready\",\"profile\":\"composition\","
            "\"wire_owner\":\"simulator_service\",\"asset_import\":\"builtin_cube_only\","
            "\"frame_projection\":\"CMFB/v1\"}" :
            "{\"type\":\"service.ready\",\"profile\":\"composition\","
            "\"wire_owner\":\"native_service\",\"asset_import\":\"builtin_cube_only\","
            "\"frame_projection\":\"CMFB/v1\"}")) {
        return false;
    }
    while (!service->close_requested) {
        MeisoCompositionHostRuntimeIoInterest interest;
        MeisoCompositionHostRuntimeIoReady ready = {false, false, false, false};
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

        memset(&interest, 0, sizeof(interest));

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
                        &(MeisoCompositionHostRuntimeIoReady){
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
                (void)meiso_composition_host_bridge_private_emit_error(0U, "INVALID_COMMAND");
                return false;
            }
            service->close_requested = true;
            break;
        }
        if ((service->backend == MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_BACKEND_REMOTE &&
             (!meiso_composition_host_bridge_private_reconcile_media(service) ||
              !meiso_composition_host_runtime_readiness(service->host_runtime,
                                                     &interest))) ||
            !meiso_composition_host_bridge_private_drain_latest_frames(service)) {
            return false;
        }

        if (!service->stdin_eof) {
            stdin_index = (int)descriptor_count;
            descriptors[descriptor_count++] =
                (struct pollfd){STDIN_FILENO, POLLIN, 0};
        }
        if (service->backend == MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_BACKEND_REMOTE &&
            (interest.control_read || interest.control_write)) {
            control_index = (int)descriptor_count;
            descriptors[descriptor_count++] = (struct pollfd){
                service->control_fd,
                (short)((interest.control_read ? POLLIN : 0) |
                        (interest.control_write ? POLLOUT : 0)), 0
            };
        }
        if (service->backend == MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_BACKEND_REMOTE &&
            service->media_fd >= 0 &&
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
        poll_result = poll(descriptors, descriptor_count, MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_POLL_TIMEOUT_MS);
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
                if (!meiso_composition_host_bridge_private_finish_media_connect(service)) {
                    return false;
                }
            } else if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                (void)meiso_composition_host_runtime_unbind_media_fd(
                    service->host_runtime, ECONNRESET
                );
                meiso_composition_host_bridge_private_close_fd(&service->media_fd);
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
                if (!meiso_composition_host_bridge_private_write_frame_output(service, true)) {
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
