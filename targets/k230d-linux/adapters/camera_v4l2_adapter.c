#include "meiso_camera_v4l2_adapter_private.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Owns arena initialization plus descriptor, buffer, mapping, and stream
 * lifecycle. Frame acceptance and native syscall translation live separately.
 */

static void clear_queued(MeisoCameraV4l2Adapter *adapter)
{
    uint32_t index;
    for (index = 0U;
         index < MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT;
         ++index) {
        adapter->mappings[index].queued = false;
    }
    adapter->queued_buffer_count = 0U;
}

static void clear_mapping(
    MeisoCameraV4l2Adapter *adapter,
    uint32_t index
)
{
    CameraV4l2Mapping *mapping = &adapter->mappings[index];
    if (mapping->owned) {
        --adapter->mapped_buffer_count;
        adapter->mapped_bytes -= (uint64_t)mapping->length;
    }
    memset(mapping, 0, sizeof(*mapping));
}

void meiso_camera_v4l2_default_config(
    uint64_t source_clock_epoch,
    MeisoCameraV4l2Config *out_config
)
{
    if (out_config == NULL) {
        return;
    }
    out_config->device_path = "/dev/video1";
    out_config->source_clock_epoch = source_clock_epoch;
    out_config->syscalls = NULL;
    out_config->syscall_context = NULL;
}

size_t meiso_camera_v4l2_alignment_required(void)
{
    return _Alignof(max_align_t);
}

size_t meiso_camera_v4l2_memory_required(void)
{
    return sizeof(MeisoCameraV4l2Adapter);
}

MeisoCameraV4l2InitStatus meiso_camera_v4l2_init(
    void *arena,
    size_t arena_size,
    const MeisoCameraV4l2Config *config,
    MeisoCameraV4l2Adapter **out_adapter
)
{
    const MeisoCameraV4l2Syscalls *selected;
    MeisoCameraV4l2Adapter candidate;
    size_t path_size;

    if (arena == NULL || config == NULL || out_adapter == NULL) {
        return MEISO_CAMERA_V4L2_INIT_INVALID_ARGUMENT;
    }
    if (!meiso_camera_v4l2_private_path_length(
            config->device_path, &path_size
        ) || config->source_clock_epoch == 0U ||
        (config->syscalls == NULL && config->syscall_context != NULL) ||
        (config->syscalls != NULL &&
         !meiso_camera_v4l2_private_syscall_table_valid(
             config->syscalls
         ))) {
        return MEISO_CAMERA_V4L2_INIT_INVALID_CONFIG;
    }
    selected = config->syscalls == NULL ?
        &meiso_camera_v4l2_private_native_syscalls : config->syscalls;
    if ((uintptr_t)arena % meiso_camera_v4l2_alignment_required() != 0U) {
        return MEISO_CAMERA_V4L2_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < meiso_camera_v4l2_memory_required()) {
        return MEISO_CAMERA_V4L2_INIT_ARENA_TOO_SMALL;
    }
    if (meiso_camera_v4l2_private_ranges_overlap(
            arena, arena_size, config, sizeof(*config)
        ) || meiso_camera_v4l2_private_ranges_overlap(
            arena, arena_size, config->device_path, path_size + 1U
        ) || meiso_camera_v4l2_private_ranges_overlap(
            arena,
            arena_size,
            config->syscalls,
            config->syscalls == NULL ? 0U : sizeof(*config->syscalls)
        ) || meiso_camera_v4l2_private_ranges_overlap(
            arena,
            arena_size,
            config->syscall_context,
            config->syscall_context == NULL ? 0U : 1U
        ) || meiso_camera_v4l2_private_ranges_overlap(
            arena, arena_size, out_adapter, sizeof(*out_adapter)
        ) || meiso_camera_v4l2_private_ranges_overlap(
            config,
            sizeof(*config),
            config->device_path,
            path_size + 1U
        ) || meiso_camera_v4l2_private_ranges_overlap(
            config,
            sizeof(*config),
            config->syscalls,
            config->syscalls == NULL ? 0U : sizeof(*config->syscalls)
        ) || meiso_camera_v4l2_private_ranges_overlap(
            config->device_path,
            path_size + 1U,
            config->syscalls,
            config->syscalls == NULL ? 0U : sizeof(*config->syscalls)
        ) || meiso_camera_v4l2_private_ranges_overlap(
            config, sizeof(*config), out_adapter, sizeof(*out_adapter)
        ) || meiso_camera_v4l2_private_ranges_overlap(
            config->device_path,
            path_size + 1U,
            out_adapter,
            sizeof(*out_adapter)
        ) || meiso_camera_v4l2_private_ranges_overlap(
            config->syscalls,
            config->syscalls == NULL ? 0U : sizeof(*config->syscalls),
            out_adapter,
            sizeof(*out_adapter)
        )) {
        return MEISO_CAMERA_V4L2_INIT_INVALID_ARGUMENT;
    }

    memset(&candidate, 0, sizeof(candidate));
    candidate.arena_size = arena_size;
    candidate.syscalls = *selected;
    candidate.syscall_context = config->syscall_context;
    candidate.source_clock_epoch = config->source_clock_epoch;
    candidate.magic = CAMERA_V4L2_MAGIC;
    candidate.fd = -1;
    candidate.phase = MEISO_CAMERA_V4L2_PHASE_STOPPED;
    memcpy(candidate.device_path, config->device_path, path_size + 1U);

    memset(arena, 0, arena_size);
    memcpy(arena, &candidate, sizeof(candidate));
    *out_adapter = (MeisoCameraV4l2Adapter *)arena;
    return MEISO_CAMERA_V4L2_INIT_OK;
}

bool meiso_camera_v4l2_private_cleanup_resources(
    MeisoCameraV4l2Adapter *adapter
)
{
    bool okay = true;
    bool failure_recorded = false;
    bool stream_off_failed = false;
    uint32_t index;
    int result;

    meiso_camera_v4l2_private_clear_borrow(adapter);
    if (adapter->stream_on && adapter->fd_owned) {
        MeisoCameraV4l2IoctlArgument argument;
        memset(&argument, 0, sizeof(argument));
        result = meiso_camera_v4l2_private_call_ioctl(
            adapter, MEISO_CAMERA_V4L2_IOCTL_STREAM_OFF, &argument
        );
        if (result != 0) {
            meiso_camera_v4l2_private_set_cleanup_failure(
                adapter, MEISO_CAMERA_V4L2_FAILURE_STREAM_OFF, result
            );
            failure_recorded = true;
            stream_off_failed = true;
            okay = false;
        }
    }
    adapter->stream_on = false;
    clear_queued(adapter);

    /*
     * A failed STREAMOFF does not prove DMA quiescence. Relinquish the fd
     * first (close owns it even on error), then release process mappings.
     * REQBUFS(0) must not run against that stale/live-stream fd.
     */
    if (stream_off_failed) {
        adapter->buffers_allocated = false;
        adapter->driver_buffer_count = 0U;
        if (adapter->fd_owned) {
            const int fd = adapter->fd;
            adapter->fd_owned = false;
            adapter->fd = -1;
            result = meiso_camera_v4l2_private_call_close(adapter, fd);
            if (result != 0) {
                okay = false;
            }
        }
    }

    for (index = MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT;
         index > 0U;
         --index) {
        const uint32_t mapping_index = index - 1U;
        CameraV4l2Mapping *mapping =
            &adapter->mappings[mapping_index];
        if (!mapping->owned) {
            continue;
        }
        result = meiso_camera_v4l2_private_call_unmap(
            adapter, mapping->data, mapping->length
        );
        if (result == 0) {
            clear_mapping(adapter, mapping_index);
        } else {
            if (!failure_recorded) {
                meiso_camera_v4l2_private_set_cleanup_failure(
                    adapter,
                    MEISO_CAMERA_V4L2_FAILURE_UNMAP_BUFFER,
                    result
                );
                failure_recorded = true;
            }
            okay = false;
        }
    }

    if (adapter->buffers_allocated && adapter->fd_owned) {
        MeisoCameraV4l2IoctlArgument argument;
        memset(&argument, 0, sizeof(argument));
        argument.request_buffers.count = 0U;
        result = meiso_camera_v4l2_private_call_ioctl(
            adapter, MEISO_CAMERA_V4L2_IOCTL_REQUEST_BUFFERS, &argument
        );
        if (result != 0) {
            if (!failure_recorded) {
                meiso_camera_v4l2_private_set_cleanup_failure(
                    adapter,
                    MEISO_CAMERA_V4L2_FAILURE_RELEASE_BUFFERS,
                    result
                );
                failure_recorded = true;
            }
            okay = false;
        }
    }
    adapter->buffers_allocated = false;
    adapter->driver_buffer_count = 0U;

    if (adapter->fd_owned) {
        const int fd = adapter->fd;
        adapter->fd_owned = false;
        adapter->fd = -1;
        result = meiso_camera_v4l2_private_call_close(adapter, fd);
        if (result != 0) {
            if (!failure_recorded) {
                meiso_camera_v4l2_private_set_cleanup_failure(
                    adapter, MEISO_CAMERA_V4L2_FAILURE_CLOSE, result
                );
            }
            okay = false;
        }
    }
    return okay;
}

static MeisoCameraV4l2StartStatus fail_start(
    MeisoCameraV4l2Adapter *adapter,
    MeisoCameraV4l2StartStatus status,
    MeisoCameraV4l2FailureStage stage,
    int error
)
{
    meiso_camera_v4l2_private_set_failure(adapter, stage, error);
    if (!meiso_camera_v4l2_private_cleanup_resources(adapter)) {
        adapter->phase = MEISO_CAMERA_V4L2_PHASE_TERMINAL;
        return MEISO_CAMERA_V4L2_START_CLEANUP_FAILED;
    }
    adapter->phase = MEISO_CAMERA_V4L2_PHASE_STOPPED;
    return status;
}

static bool format_exact(const MeisoCameraV4l2FormatIoctl *format)
{
    return format != NULL &&
        format->width == MEISO_CAMERA_V4L2_WIDTH &&
        format->height == MEISO_CAMERA_V4L2_HEIGHT &&
        format->pixel_format == MEISO_COMPOSITION_MEDIA_PIXEL_NV12 &&
        format->progressive &&
        format->bytes_per_line == MEISO_CAMERA_V4L2_BYTES_PER_LINE &&
        format->size_image == MEISO_CAMERA_V4L2_SIZE_IMAGE;
}

MeisoCameraV4l2StartStatus meiso_camera_v4l2_start(
    MeisoCameraV4l2Adapter *adapter
)
{
    MeisoCameraV4l2IoctlArgument argument;
    uint32_t index;
    int candidate_fd = -1;
    int result;

    if (!meiso_camera_v4l2_private_adapter_valid(adapter) ||
        adapter->callback_active) {
        return MEISO_CAMERA_V4L2_START_INVALID_ARGUMENT;
    }
    if (adapter->phase == MEISO_CAMERA_V4L2_PHASE_TERMINAL) {
        return MEISO_CAMERA_V4L2_START_TERMINAL;
    }
    if (adapter->phase == MEISO_CAMERA_V4L2_PHASE_RUNNING) {
        return MEISO_CAMERA_V4L2_START_UNCHANGED;
    }
    if (adapter->run_generation == UINT64_MAX) {
        return MEISO_CAMERA_V4L2_START_COUNTER_EXHAUSTED;
    }

    result = meiso_camera_v4l2_private_call_open(adapter, &candidate_fd);
    if (result != 0 || candidate_fd < 0) {
        meiso_camera_v4l2_private_set_failure(
            adapter,
            MEISO_CAMERA_V4L2_FAILURE_OPEN,
            result != 0 ? result : EIO
        );
        return MEISO_CAMERA_V4L2_START_OPEN_FAILED;
    }
    adapter->fd = candidate_fd;
    adapter->fd_owned = true;

    memset(&argument, 0, sizeof(argument));
    result = meiso_camera_v4l2_private_call_ioctl(
        adapter, MEISO_CAMERA_V4L2_IOCTL_QUERY_CAPABILITY, &argument
    );
    if (result != 0 || !argument.capability.video_capture ||
        !argument.capability.streaming) {
        return fail_start(
            adapter,
            MEISO_CAMERA_V4L2_START_DEVICE_UNSUPPORTED,
            MEISO_CAMERA_V4L2_FAILURE_QUERY_CAPABILITY,
            result != 0 ? result : ENOTSUP
        );
    }

    memset(&argument, 0, sizeof(argument));
    argument.format.width = MEISO_CAMERA_V4L2_WIDTH;
    argument.format.height = MEISO_CAMERA_V4L2_HEIGHT;
    argument.format.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_NV12;
    argument.format.progressive = true;
    argument.format.bytes_per_line = MEISO_CAMERA_V4L2_BYTES_PER_LINE;
    argument.format.size_image = MEISO_CAMERA_V4L2_SIZE_IMAGE;
    result = meiso_camera_v4l2_private_call_ioctl(
        adapter, MEISO_CAMERA_V4L2_IOCTL_SET_FORMAT, &argument
    );
    if (result != 0) {
        return fail_start(
            adapter,
            MEISO_CAMERA_V4L2_START_FORMAT_UNSUPPORTED,
            MEISO_CAMERA_V4L2_FAILURE_SET_FORMAT,
            result
        );
    }
    memset(&argument, 0, sizeof(argument));
    result = meiso_camera_v4l2_private_call_ioctl(
        adapter, MEISO_CAMERA_V4L2_IOCTL_GET_FORMAT, &argument
    );
    if (result != 0 || !format_exact(&argument.format)) {
        return fail_start(
            adapter,
            MEISO_CAMERA_V4L2_START_FORMAT_UNSUPPORTED,
            MEISO_CAMERA_V4L2_FAILURE_GET_FORMAT,
            result != 0 ? result : ENOTSUP
        );
    }

    memset(&argument, 0, sizeof(argument));
    argument.request_buffers.count =
        MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT;
    result = meiso_camera_v4l2_private_call_ioctl(
        adapter, MEISO_CAMERA_V4L2_IOCTL_REQUEST_BUFFERS, &argument
    );
    if (result != 0) {
        return fail_start(
            adapter,
            MEISO_CAMERA_V4L2_START_BUFFER_SETUP_FAILED,
            MEISO_CAMERA_V4L2_FAILURE_REQUEST_BUFFERS,
            result
        );
    }
    adapter->buffers_allocated = true;
    adapter->driver_buffer_count = argument.request_buffers.count;
    if (argument.request_buffers.count <
        MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT) {
        return fail_start(
            adapter,
            MEISO_CAMERA_V4L2_START_INSUFFICIENT_BUFFERS,
            MEISO_CAMERA_V4L2_FAILURE_REQUEST_BUFFERS,
            ENOBUFS
        );
    }

    for (index = 0U;
         index < MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT;
         ++index) {
        void *mapping = NULL;
        memset(&argument, 0, sizeof(argument));
        argument.buffer.index = index;
        result = meiso_camera_v4l2_private_call_ioctl(
            adapter, MEISO_CAMERA_V4L2_IOCTL_QUERY_BUFFER, &argument
        );
        if (result != 0 || argument.buffer.index != index ||
            argument.buffer.length < MEISO_CAMERA_V4L2_SIZE_IMAGE) {
            return fail_start(
                adapter,
                MEISO_CAMERA_V4L2_START_BUFFER_SETUP_FAILED,
                MEISO_CAMERA_V4L2_FAILURE_QUERY_BUFFER,
                result != 0 ? result : EINVAL
            );
        }
        result = meiso_camera_v4l2_private_call_map(
            adapter,
            argument.buffer.length,
            argument.buffer.memory_offset,
            &mapping
        );
        if (result != 0 || mapping == NULL ||
            mapping == (void *)(uintptr_t)UINTPTR_MAX) {
            return fail_start(
                adapter,
                MEISO_CAMERA_V4L2_START_BUFFER_SETUP_FAILED,
                MEISO_CAMERA_V4L2_FAILURE_MAP_BUFFER,
                result != 0 ? result : EINVAL
            );
        }
        adapter->mappings[index].data = mapping;
        adapter->mappings[index].length = argument.buffer.length;
        adapter->mappings[index].owned = true;
        ++adapter->mapped_buffer_count;
        adapter->mapped_bytes += (uint64_t)argument.buffer.length;
        if (!meiso_camera_v4l2_private_mapping_ranges_valid(adapter)) {
            return fail_start(
                adapter,
                MEISO_CAMERA_V4L2_START_BUFFER_SETUP_FAILED,
                MEISO_CAMERA_V4L2_FAILURE_MAP_BUFFER,
                EINVAL
            );
        }
    }

    for (index = 0U;
         index < MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT;
         ++index) {
        memset(&argument, 0, sizeof(argument));
        argument.buffer.index = index;
        result = meiso_camera_v4l2_private_call_ioctl(
            adapter, MEISO_CAMERA_V4L2_IOCTL_QUEUE_BUFFER, &argument
        );
        if (result != 0) {
            return fail_start(
                adapter,
                MEISO_CAMERA_V4L2_START_BUFFER_SETUP_FAILED,
                MEISO_CAMERA_V4L2_FAILURE_QUEUE_BUFFER,
                result
            );
        }
        adapter->mappings[index].queued = true;
        ++adapter->queued_buffer_count;
    }
    memset(&argument, 0, sizeof(argument));
    result = meiso_camera_v4l2_private_call_ioctl(
        adapter, MEISO_CAMERA_V4L2_IOCTL_STREAM_ON, &argument
    );
    if (result != 0) {
        return fail_start(
            adapter,
            MEISO_CAMERA_V4L2_START_STREAM_FAILED,
            MEISO_CAMERA_V4L2_FAILURE_STREAM_ON,
            result
        );
    }

    adapter->stream_on = true;
    ++adapter->run_generation;
    adapter->producer_dropped_frames = 0U;
    adapter->last_timestamp_monotonic_ns = 0U;
    adapter->last_driver_sequence = 0U;
    adapter->last_sequence = 0U;
    adapter->synthetic_sequence_high_water = 0U;
    adapter->driver_sequence_present = false;
    adapter->last_sequence_provenance = MEISO_CAMERA_V4L2_SEQUENCE_NONE;
    adapter->last_timestamp_provenance = MEISO_CAMERA_V4L2_TIMESTAMP_NONE;
    memset(&adapter->last_dequeue, 0, sizeof(adapter->last_dequeue));
    adapter->primary_failure_stage = MEISO_CAMERA_V4L2_FAILURE_NONE;
    adapter->primary_error = 0;
    adapter->cleanup_failure_stage = MEISO_CAMERA_V4L2_FAILURE_NONE;
    adapter->cleanup_error = 0;
    adapter->phase = MEISO_CAMERA_V4L2_PHASE_RUNNING;
    return MEISO_CAMERA_V4L2_START_STARTED;
}

MeisoCameraV4l2StopStatus meiso_camera_v4l2_stop(
    MeisoCameraV4l2Adapter *adapter
)
{
    bool terminal;

    if (!meiso_camera_v4l2_private_adapter_valid(adapter) ||
        adapter->callback_active) {
        return MEISO_CAMERA_V4L2_STOP_INVALID_ARGUMENT;
    }
    terminal = adapter->phase == MEISO_CAMERA_V4L2_PHASE_TERMINAL;
    if (adapter->phase == MEISO_CAMERA_V4L2_PHASE_STOPPED) {
        return MEISO_CAMERA_V4L2_STOP_UNCHANGED;
    }
    if (!meiso_camera_v4l2_private_cleanup_resources(adapter)) {
        adapter->phase = MEISO_CAMERA_V4L2_PHASE_TERMINAL;
        return MEISO_CAMERA_V4L2_STOP_TERMINAL;
    }
    if (terminal) {
        adapter->phase = MEISO_CAMERA_V4L2_PHASE_TERMINAL;
        return MEISO_CAMERA_V4L2_STOP_TERMINAL;
    }
    adapter->phase = MEISO_CAMERA_V4L2_PHASE_STOPPED;
    adapter->primary_failure_stage = MEISO_CAMERA_V4L2_FAILURE_NONE;
    adapter->primary_error = 0;
    adapter->cleanup_failure_stage = MEISO_CAMERA_V4L2_FAILURE_NONE;
    adapter->cleanup_error = 0;
    return MEISO_CAMERA_V4L2_STOP_STOPPED;
}
