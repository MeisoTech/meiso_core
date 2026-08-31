#include "meiso_camera_v4l2_adapter_private.h"

#include <stdint.h>
#include <string.h>

/*
 * Owns representation invariants, alias guards, failure provenance, and
 * immutable diagnostic snapshots. It never invokes a platform callback.
 */

bool meiso_camera_v4l2_private_ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
{
    const uintptr_t lhs_begin = (uintptr_t)lhs;
    const uintptr_t rhs_begin = (uintptr_t)rhs;

    if (lhs == NULL || rhs == NULL || lhs_size == 0U || rhs_size == 0U) {
        return false;
    }
    if (lhs_begin > UINTPTR_MAX - lhs_size ||
        rhs_begin > UINTPTR_MAX - rhs_size) {
        return true;
    }
    return lhs_begin < rhs_begin + rhs_size &&
        rhs_begin < lhs_begin + lhs_size;
}

bool meiso_camera_v4l2_private_path_length(
    const char *path,
    size_t *out_length
)
{
    const char *end;

    if (path == NULL || out_length == NULL || path[0] != '/') {
        return false;
    }
    end = (const char *)memchr(
        path, '\0', MEISO_CAMERA_V4L2_DEVICE_PATH_CAPACITY
    );
    if (end == NULL || end <= path + 1) {
        return false;
    }
    *out_length = (size_t)(end - path);
    return true;
}

bool meiso_camera_v4l2_private_syscall_table_valid(
    const MeisoCameraV4l2Syscalls *syscalls
)
{
    return syscalls != NULL && syscalls->open_device != NULL &&
        syscalls->device_ioctl != NULL && syscalls->map_buffer != NULL &&
        syscalls->unmap_buffer != NULL && syscalls->close_device != NULL &&
        syscalls->monotonic_now != NULL;
}

bool meiso_camera_v4l2_private_mapping_ranges_valid(
    const MeisoCameraV4l2Adapter *adapter
)
{
    uint32_t outer;
    uint32_t inner;

    for (outer = 0U;
         outer < MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT;
         ++outer) {
        const CameraV4l2Mapping *mapping = &adapter->mappings[outer];
        if (!mapping->owned) {
            if (mapping->data != NULL || mapping->length != 0U ||
                mapping->queued) {
                return false;
            }
            continue;
        }
        if (mapping->data == NULL ||
            mapping->data == (void *)(uintptr_t)UINTPTR_MAX ||
            mapping->length < MEISO_CAMERA_V4L2_SIZE_IMAGE ||
            (adapter->phase != MEISO_CAMERA_V4L2_PHASE_TERMINAL &&
             meiso_camera_v4l2_private_ranges_overlap(
                 adapter,
                 adapter->arena_size,
                 mapping->data,
                 mapping->length
             ))) {
            return false;
        }
        for (inner = 0U; inner < outer; ++inner) {
            const CameraV4l2Mapping *previous = &adapter->mappings[inner];
            if (adapter->phase != MEISO_CAMERA_V4L2_PHASE_TERMINAL &&
                previous->owned &&
                meiso_camera_v4l2_private_ranges_overlap(
                    mapping->data,
                    mapping->length,
                    previous->data,
                    previous->length
                )) {
                return false;
            }
        }
    }
    return true;
}

bool meiso_camera_v4l2_private_adapter_valid(
    const MeisoCameraV4l2Adapter *adapter
)
{
    size_t path_size;
    uint32_t mapped = 0U;
    uint32_t queued = 0U;
    uint64_t mapped_bytes = 0U;
    uint32_t index;

    if (adapter == NULL || adapter->magic != CAMERA_V4L2_MAGIC ||
        adapter->arena_size < sizeof(*adapter) ||
        adapter->source_clock_epoch == 0U ||
        !meiso_camera_v4l2_private_path_length(
            adapter->device_path, &path_size
        ) ||
        !meiso_camera_v4l2_private_syscall_table_valid(
            &adapter->syscalls
        ) ||
        (adapter->phase != MEISO_CAMERA_V4L2_PHASE_STOPPED &&
         adapter->phase != MEISO_CAMERA_V4L2_PHASE_RUNNING &&
         adapter->phase != MEISO_CAMERA_V4L2_PHASE_TERMINAL) ||
        adapter->last_sequence_provenance >
            MEISO_CAMERA_V4L2_SEQUENCE_ADAPTER_SYNTHETIC ||
        adapter->last_timestamp_provenance >
            MEISO_CAMERA_V4L2_TIMESTAMP_ADAPTER_DEQUEUE_MONOTONIC ||
        adapter->primary_failure_stage >
            MEISO_CAMERA_V4L2_FAILURE_TIMESTAMP_CLOCK ||
        adapter->cleanup_failure_stage >
            MEISO_CAMERA_V4L2_FAILURE_TIMESTAMP_CLOCK ||
        ((adapter->primary_failure_stage ==
              MEISO_CAMERA_V4L2_FAILURE_NONE) !=
         (adapter->primary_error == 0)) ||
        ((adapter->cleanup_failure_stage ==
              MEISO_CAMERA_V4L2_FAILURE_NONE) !=
         (adapter->cleanup_error == 0)) ||
        adapter->driver_sequence_present !=
            (adapter->last_sequence_provenance ==
             MEISO_CAMERA_V4L2_SEQUENCE_DRIVER) ||
        (adapter->last_sequence_provenance ==
             MEISO_CAMERA_V4L2_SEQUENCE_NONE &&
         adapter->last_sequence != 0U) ||
        ((adapter->last_timestamp_provenance ==
              MEISO_CAMERA_V4L2_TIMESTAMP_NONE) !=
         (adapter->last_timestamp_monotonic_ns == 0U)) ||
        ((adapter->last_sequence_provenance ==
              MEISO_CAMERA_V4L2_SEQUENCE_NONE) !=
         (adapter->last_timestamp_provenance ==
              MEISO_CAMERA_V4L2_TIMESTAMP_NONE)) ||
        adapter->last_dequeue.timestamp_provenance >
            MEISO_CAMERA_V4L2_TIMESTAMP_ADAPTER_DEQUEUE_MONOTONIC ||
        adapter->last_dequeue.reject_reason >
            MEISO_CAMERA_V4L2_FRAME_REJECT_TIMESTAMP_CLOCK ||
        (!adapter->last_dequeue.timestamp_candidate_present &&
         adapter->last_dequeue.timestamp_candidate_monotonic_ns != 0U) ||
        !meiso_camera_v4l2_private_mapping_ranges_valid(adapter)) {
        return false;
    }
    for (index = 0U;
         index < MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT;
         ++index) {
        if (adapter->mappings[index].owned) {
            if (mapped_bytes > UINT64_MAX -
                    (uint64_t)adapter->mappings[index].length) {
                return false;
            }
            mapped_bytes += (uint64_t)adapter->mappings[index].length;
            ++mapped;
        }
        if (adapter->mappings[index].queued) {
            ++queued;
        }
    }
    if (mapped != adapter->mapped_buffer_count ||
        queued != adapter->queued_buffer_count ||
        mapped_bytes != adapter->mapped_bytes ||
        adapter->queued_buffer_count > adapter->mapped_buffer_count ||
        adapter->mapped_buffer_count >
            MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT ||
        adapter->frame_borrowed ==
            (adapter->borrowed_frame.run_generation == 0U) ||
        (adapter->fd_owned && adapter->fd < 0) ||
        (!adapter->fd_owned && adapter->fd != -1) ||
        (adapter->stream_on && !adapter->fd_owned) ||
        (adapter->buffers_allocated && !adapter->fd_owned)) {
        return false;
    }
    return true;
}

bool meiso_camera_v4l2_private_adapter_overlaps(
    const MeisoCameraV4l2Adapter *adapter,
    const void *value,
    size_t value_size
)
{
    return meiso_camera_v4l2_private_ranges_overlap(
        adapter, adapter->arena_size, value, value_size
    );
}

bool meiso_camera_v4l2_private_mappings_overlap(
    const MeisoCameraV4l2Adapter *adapter,
    const void *value,
    size_t value_size
)
{
    uint32_t index;

    for (index = 0U;
         index < MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT;
         ++index) {
        if (adapter->mappings[index].owned &&
            meiso_camera_v4l2_private_ranges_overlap(
                adapter->mappings[index].data,
                adapter->mappings[index].length,
                value,
                value_size
            )) {
            return true;
        }
    }
    return false;
}

void meiso_camera_v4l2_private_clear_borrow(
    MeisoCameraV4l2Adapter *adapter
)
{
    adapter->frame_borrowed = false;
    memset(&adapter->borrowed_frame, 0, sizeof(adapter->borrowed_frame));
}

void meiso_camera_v4l2_private_set_failure(
    MeisoCameraV4l2Adapter *adapter,
    MeisoCameraV4l2FailureStage stage,
    int error
)
{
    adapter->primary_failure_stage = stage;
    adapter->primary_error =
        meiso_camera_v4l2_private_error_or_io(error);
}

void meiso_camera_v4l2_private_set_cleanup_failure(
    MeisoCameraV4l2Adapter *adapter,
    MeisoCameraV4l2FailureStage stage,
    int error
)
{
    if (adapter->cleanup_failure_stage ==
        MEISO_CAMERA_V4L2_FAILURE_NONE) {
        adapter->cleanup_failure_stage = stage;
        adapter->cleanup_error =
            meiso_camera_v4l2_private_error_or_io(error);
    }
}

bool meiso_camera_v4l2_snapshot(
    const MeisoCameraV4l2Adapter *adapter,
    MeisoCameraV4l2Snapshot *out_snapshot
)
{
    MeisoCameraV4l2Snapshot value;

    if (!meiso_camera_v4l2_private_adapter_valid(adapter) ||
        adapter->callback_active || out_snapshot == NULL ||
        meiso_camera_v4l2_private_adapter_overlaps(
            adapter, out_snapshot, sizeof(*out_snapshot)
        ) || meiso_camera_v4l2_private_mappings_overlap(
            adapter, out_snapshot, sizeof(*out_snapshot)
        )) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.phase = adapter->phase;
    value.run_generation = adapter->run_generation;
    value.source_clock_epoch = adapter->source_clock_epoch;
    value.driver_buffer_count = adapter->driver_buffer_count;
    value.mapped_buffer_count = adapter->mapped_buffer_count;
    value.queued_buffer_count = adapter->queued_buffer_count;
    value.stream_on = adapter->stream_on;
    value.frame_borrowed = adapter->frame_borrowed;
    value.borrowed_frame = adapter->borrowed_frame;
    value.producer_dropped_frames = adapter->producer_dropped_frames;
    value.driver_sequence_present = adapter->driver_sequence_present;
    value.last_driver_sequence = adapter->last_driver_sequence;
    value.last_sequence = adapter->last_sequence;
    value.last_sequence_provenance = adapter->last_sequence_provenance;
    value.last_timestamp_monotonic_ns =
        adapter->last_timestamp_monotonic_ns;
    value.last_timestamp_provenance = adapter->last_timestamp_provenance;
    value.last_dequeue = adapter->last_dequeue;
    value.primary_failure_stage = adapter->primary_failure_stage;
    value.primary_error = adapter->primary_error;
    value.cleanup_failure_stage = adapter->cleanup_failure_stage;
    value.cleanup_error = adapter->cleanup_error;
    value.arena_bytes = adapter->arena_size;
    value.mapped_bytes = adapter->mapped_bytes;
    *out_snapshot = value;
    return true;
}
