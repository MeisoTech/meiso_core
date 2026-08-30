#include "meiso_camera_v4l2_adapter_private.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

/*
 * Owns dequeue contract validation and the single outstanding frame borrow.
 * Resource acquisition/teardown remains in the lifecycle implementation.
 */

static bool progressive_field_valid(
    const MeisoCameraV4l2BufferIoctl *buffer
)
{
    return buffer != NULL &&
        ((buffer->raw_field == MEISO_CAMERA_V4L2_RAW_FIELD_NONE &&
          buffer->field_provenance ==
              MEISO_CAMERA_V4L2_FIELD_EXPLICIT_PROGRESSIVE) ||
         (buffer->raw_field == MEISO_CAMERA_V4L2_RAW_FIELD_ANY &&
          buffer->field_provenance ==
              MEISO_CAMERA_V4L2_FIELD_FORMAT_INHERITED_PROGRESSIVE));
}

static void record_dequeue(
    MeisoCameraV4l2Adapter *adapter,
    const MeisoCameraV4l2BufferIoctl *buffer,
    MeisoCameraV4l2FrameRejectReason reason
)
{
    MeisoCameraV4l2DequeuedDiagnostic diagnostic;

    memset(&diagnostic, 0, sizeof(diagnostic));
    diagnostic.present = true;
    diagnostic.index = buffer->index;
    diagnostic.length = buffer->length;
    diagnostic.bytes_used = buffer->bytes_used;
    diagnostic.raw_field = buffer->raw_field;
    diagnostic.field_provenance = buffer->field_provenance;
    diagnostic.raw_driver_sequence = buffer->driver_sequence;
    diagnostic.driver_sequence_present = buffer->driver_sequence_present;
    diagnostic.timestamp_seconds = buffer->timestamp_seconds;
    diagnostic.timestamp_microseconds = buffer->timestamp_microseconds;
    diagnostic.timestamp_monotonic = buffer->timestamp_monotonic;
    diagnostic.frame_error = buffer->frame_error;
    diagnostic.reject_reason = reason;
    adapter->last_dequeue = diagnostic;
}

static bool frame_ref_equal(
    MeisoCameraV4l2FrameRef lhs,
    MeisoCameraV4l2FrameRef rhs
)
{
    return lhs.run_generation == rhs.run_generation &&
        lhs.borrow_serial == rhs.borrow_serial &&
        lhs.buffer_index == rhs.buffer_index &&
        lhs.driver_sequence == rhs.driver_sequence &&
        lhs.sequence_provenance == rhs.sequence_provenance;
}

static bool timestamp_ns(
    const MeisoCameraV4l2BufferIoctl *buffer,
    uint64_t *out_timestamp
)
{
    uint64_t seconds;
    uint64_t micros;

    if (buffer == NULL || out_timestamp == NULL ||
        buffer->timestamp_seconds < 0 ||
        buffer->timestamp_microseconds < 0 ||
        buffer->timestamp_microseconds >= 1000000) {
        return false;
    }
    seconds = (uint64_t)buffer->timestamp_seconds;
    micros = (uint64_t)(uint32_t)buffer->timestamp_microseconds;
    if (seconds > (UINT64_MAX - micros * UINT64_C(1000)) /
            UINT64_C(1000000000)) {
        return false;
    }
    *out_timestamp = seconds * UINT64_C(1000000000) +
        micros * UINT64_C(1000);
    return true;
}

static void terminal_failure(
    MeisoCameraV4l2Adapter *adapter,
    MeisoCameraV4l2FailureStage stage,
    int error
)
{
    meiso_camera_v4l2_private_set_failure(adapter, stage, error);
    (void)meiso_camera_v4l2_private_cleanup_resources(adapter);
    adapter->phase = MEISO_CAMERA_V4L2_PHASE_TERMINAL;
}

MeisoCameraV4l2PollStatus meiso_camera_v4l2_poll_frame(
    MeisoCameraV4l2Adapter *adapter,
    MeisoCameraV4l2FrameView *out_frame
)
{
    MeisoCameraV4l2IoctlArgument argument;
    MeisoCameraV4l2FrameView frame;
    MeisoCameraV4l2FrameRef ref;
    CameraV4l2Mapping *mapping;
    uint64_t timestamp;
    uint64_t dropped = 0U;
    uint64_t dropped_total;
    uint32_t delta = 0U;
    uint32_t sequence;
    uint32_t synthetic_high_water;
    MeisoCameraV4l2SequenceProvenance sequence_provenance;
    int result;

    if (!meiso_camera_v4l2_private_adapter_valid(adapter) ||
        adapter->callback_active || out_frame == NULL ||
        meiso_camera_v4l2_private_adapter_overlaps(
            adapter, out_frame, sizeof(*out_frame)
        ) || meiso_camera_v4l2_private_mappings_overlap(
            adapter, out_frame, sizeof(*out_frame)
        )) {
        return MEISO_CAMERA_V4L2_POLL_INVALID_ARGUMENT;
    }
    if (adapter->phase == MEISO_CAMERA_V4L2_PHASE_TERMINAL) {
        return MEISO_CAMERA_V4L2_POLL_TERMINAL;
    }
    if (adapter->phase != MEISO_CAMERA_V4L2_PHASE_RUNNING) {
        return MEISO_CAMERA_V4L2_POLL_NOT_RUNNING;
    }
    if (adapter->frame_borrowed) {
        return MEISO_CAMERA_V4L2_POLL_BORROW_ACTIVE;
    }
    if (adapter->borrow_serial_high_water == UINT64_MAX) {
        return MEISO_CAMERA_V4L2_POLL_COUNTER_EXHAUSTED;
    }

    memset(&argument, 0, sizeof(argument));
    result = meiso_camera_v4l2_private_call_ioctl(
        adapter, MEISO_CAMERA_V4L2_IOCTL_DEQUEUE_BUFFER, &argument
    );
    if (result == EAGAIN || result == EWOULDBLOCK) {
        return MEISO_CAMERA_V4L2_POLL_WOULD_BLOCK;
    }
    if (result == EINTR) {
        return MEISO_CAMERA_V4L2_POLL_INTERRUPTED;
    }
    if (result != 0) {
        terminal_failure(
            adapter, MEISO_CAMERA_V4L2_FAILURE_DEQUEUE_BUFFER, result
        );
        return MEISO_CAMERA_V4L2_POLL_TERMINAL;
    }

    record_dequeue(
        adapter, &argument.buffer, MEISO_CAMERA_V4L2_FRAME_REJECT_NONE
    );
    if (argument.buffer.index >=
            MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT) {
        record_dequeue(
            adapter,
            &argument.buffer,
            MEISO_CAMERA_V4L2_FRAME_REJECT_INDEX
        );
        terminal_failure(
            adapter, MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT, EPROTO
        );
        return MEISO_CAMERA_V4L2_POLL_TERMINAL;
    }
    if (argument.buffer.length < MEISO_CAMERA_V4L2_SIZE_IMAGE) {
        record_dequeue(
            adapter,
            &argument.buffer,
            MEISO_CAMERA_V4L2_FRAME_REJECT_LENGTH
        );
        terminal_failure(
            adapter, MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT, EPROTO
        );
        return MEISO_CAMERA_V4L2_POLL_TERMINAL;
    }
    if (argument.buffer.bytes_used != MEISO_CAMERA_V4L2_SIZE_IMAGE) {
        record_dequeue(
            adapter,
            &argument.buffer,
            MEISO_CAMERA_V4L2_FRAME_REJECT_BYTES_USED
        );
        terminal_failure(
            adapter, MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT, EPROTO
        );
        return MEISO_CAMERA_V4L2_POLL_TERMINAL;
    }
    if (!argument.buffer.timestamp_monotonic) {
        record_dequeue(
            adapter,
            &argument.buffer,
            MEISO_CAMERA_V4L2_FRAME_REJECT_TIMESTAMP_FLAG
        );
        terminal_failure(
            adapter, MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT, EPROTO
        );
        return MEISO_CAMERA_V4L2_POLL_TERMINAL;
    }
    if (!progressive_field_valid(&argument.buffer)) {
        record_dequeue(
            adapter,
            &argument.buffer,
            MEISO_CAMERA_V4L2_FRAME_REJECT_FIELD
        );
        terminal_failure(
            adapter, MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT, EPROTO
        );
        return MEISO_CAMERA_V4L2_POLL_TERMINAL;
    }
    if (argument.buffer.frame_error) {
        record_dequeue(
            adapter,
            &argument.buffer,
            MEISO_CAMERA_V4L2_FRAME_REJECT_FRAME_ERROR
        );
        terminal_failure(
            adapter, MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT, EPROTO
        );
        return MEISO_CAMERA_V4L2_POLL_TERMINAL;
    }
    if (!timestamp_ns(&argument.buffer, &timestamp)) {
        record_dequeue(
            adapter,
            &argument.buffer,
            MEISO_CAMERA_V4L2_FRAME_REJECT_TIMESTAMP_VALUE
        );
        terminal_failure(
            adapter, MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT, EPROTO
        );
        return MEISO_CAMERA_V4L2_POLL_TERMINAL;
    }
    mapping = &adapter->mappings[argument.buffer.index];
    if (!mapping->owned || !mapping->queued ||
        mapping->length < MEISO_CAMERA_V4L2_SIZE_IMAGE) {
        record_dequeue(
            adapter,
            &argument.buffer,
            MEISO_CAMERA_V4L2_FRAME_REJECT_MAPPING
        );
        terminal_failure(
            adapter, MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT, EPROTO
        );
        return MEISO_CAMERA_V4L2_POLL_TERMINAL;
    }
    if (adapter->last_sequence_provenance !=
            MEISO_CAMERA_V4L2_SEQUENCE_NONE &&
        timestamp < adapter->last_timestamp_monotonic_ns) {
        record_dequeue(
            adapter,
            &argument.buffer,
            MEISO_CAMERA_V4L2_FRAME_REJECT_TIMESTAMP_REGRESSION
        );
        terminal_failure(
            adapter, MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT, EPROTO
        );
        return MEISO_CAMERA_V4L2_POLL_TERMINAL;
    }
    sequence_provenance = argument.buffer.driver_sequence_present ?
        MEISO_CAMERA_V4L2_SEQUENCE_DRIVER :
        MEISO_CAMERA_V4L2_SEQUENCE_ADAPTER_SYNTHETIC;
    if (adapter->last_sequence_provenance !=
            MEISO_CAMERA_V4L2_SEQUENCE_NONE &&
        adapter->last_sequence_provenance != sequence_provenance) {
        record_dequeue(
            adapter,
            &argument.buffer,
            MEISO_CAMERA_V4L2_FRAME_REJECT_SEQUENCE
        );
        terminal_failure(
            adapter, MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT, EPROTO
        );
        return MEISO_CAMERA_V4L2_POLL_TERMINAL;
    }
    synthetic_high_water = adapter->synthetic_sequence_high_water;
    if (sequence_provenance == MEISO_CAMERA_V4L2_SEQUENCE_DRIVER) {
        sequence = argument.buffer.driver_sequence;
    } else {
        if (synthetic_high_water == UINT32_MAX) {
            record_dequeue(
                adapter,
                &argument.buffer,
                MEISO_CAMERA_V4L2_FRAME_REJECT_COUNTER_EXHAUSTED
            );
            terminal_failure(
                adapter,
                MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT,
                EOVERFLOW
            );
            return MEISO_CAMERA_V4L2_POLL_COUNTER_EXHAUSTED;
        }
        sequence = synthetic_high_water + 1U;
        synthetic_high_water = sequence;
    }
    if (sequence_provenance == MEISO_CAMERA_V4L2_SEQUENCE_DRIVER &&
        adapter->driver_sequence_present) {
        delta = argument.buffer.driver_sequence -
            adapter->last_driver_sequence;
        if (delta == 0U || delta > UINT32_C(0x7fffffff)) {
            record_dequeue(
                adapter,
                &argument.buffer,
                MEISO_CAMERA_V4L2_FRAME_REJECT_SEQUENCE
            );
            terminal_failure(
                adapter, MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT, EPROTO
            );
            return MEISO_CAMERA_V4L2_POLL_TERMINAL;
        }
        dropped = (uint64_t)delta - 1U;
    }
    if (adapter->producer_dropped_frames > UINT64_MAX - dropped) {
        record_dequeue(
            adapter,
            &argument.buffer,
            MEISO_CAMERA_V4L2_FRAME_REJECT_COUNTER_EXHAUSTED
        );
        terminal_failure(
            adapter, MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT, EOVERFLOW
        );
        return MEISO_CAMERA_V4L2_POLL_COUNTER_EXHAUSTED;
    }
    dropped_total = adapter->producer_dropped_frames + dropped;
    ref.run_generation = adapter->run_generation;
    ref.borrow_serial = adapter->borrow_serial_high_water + 1U;
    ref.buffer_index = argument.buffer.index;
    ref.driver_sequence = sequence;
    ref.sequence_provenance = sequence_provenance;

    memset(&frame, 0, sizeof(frame));
    frame.ref = ref;
    frame.source_time.clock_epoch = adapter->source_clock_epoch;
    frame.source_time.monotonic_ns = timestamp;
    frame.producer_dropped_frames = dropped_total;
    frame.field_provenance = argument.buffer.field_provenance;
    frame.sequence_provenance = sequence_provenance;
    frame.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_NV12;
    frame.width = MEISO_CAMERA_V4L2_WIDTH;
    frame.height = MEISO_CAMERA_V4L2_HEIGHT;
    frame.bytes_per_line = MEISO_CAMERA_V4L2_BYTES_PER_LINE;
    frame.size_image = MEISO_CAMERA_V4L2_SIZE_IMAGE;
    frame.bytes.data = (const uint8_t *)mapping->data;
    frame.bytes.size = MEISO_CAMERA_V4L2_SIZE_IMAGE;

    mapping->queued = false;
    --adapter->queued_buffer_count;
    adapter->borrow_serial_high_water = ref.borrow_serial;
    adapter->borrowed_frame = ref;
    adapter->frame_borrowed = true;
    adapter->producer_dropped_frames = dropped_total;
    adapter->synthetic_sequence_high_water = synthetic_high_water;
    adapter->last_sequence = sequence;
    adapter->last_sequence_provenance = sequence_provenance;
    adapter->driver_sequence_present =
        sequence_provenance == MEISO_CAMERA_V4L2_SEQUENCE_DRIVER;
    adapter->last_driver_sequence = adapter->driver_sequence_present ?
        argument.buffer.driver_sequence : 0U;
    adapter->last_timestamp_monotonic_ns = timestamp;
    *out_frame = frame;
    return MEISO_CAMERA_V4L2_POLL_FRAME;
}

MeisoCameraV4l2ReleaseStatus meiso_camera_v4l2_release_frame(
    MeisoCameraV4l2Adapter *adapter,
    const MeisoCameraV4l2FrameRef *expected_frame
)
{
    MeisoCameraV4l2IoctlArgument argument;
    CameraV4l2Mapping *mapping;
    int result;

    if (!meiso_camera_v4l2_private_adapter_valid(adapter) ||
        adapter->callback_active || expected_frame == NULL ||
        meiso_camera_v4l2_private_adapter_overlaps(
            adapter, expected_frame, sizeof(*expected_frame)
        ) || meiso_camera_v4l2_private_mappings_overlap(
            adapter, expected_frame, sizeof(*expected_frame)
        )) {
        return MEISO_CAMERA_V4L2_RELEASE_INVALID_ARGUMENT;
    }
    if (adapter->phase == MEISO_CAMERA_V4L2_PHASE_TERMINAL) {
        return MEISO_CAMERA_V4L2_RELEASE_TERMINAL;
    }
    if (!adapter->frame_borrowed) {
        return MEISO_CAMERA_V4L2_RELEASE_NO_FRAME;
    }
    if (!frame_ref_equal(adapter->borrowed_frame, *expected_frame)) {
        return MEISO_CAMERA_V4L2_RELEASE_MISMATCH;
    }
    mapping = &adapter->mappings[expected_frame->buffer_index];
    memset(&argument, 0, sizeof(argument));
    argument.buffer.index = expected_frame->buffer_index;
    result = meiso_camera_v4l2_private_call_ioctl(
        adapter, MEISO_CAMERA_V4L2_IOCTL_QUEUE_BUFFER, &argument
    );
    if (result != 0) {
        terminal_failure(
            adapter, MEISO_CAMERA_V4L2_FAILURE_QUEUE_BUFFER, result
        );
        return MEISO_CAMERA_V4L2_RELEASE_TERMINAL;
    }
    mapping->queued = true;
    ++adapter->queued_buffer_count;
    meiso_camera_v4l2_private_clear_borrow(adapter);
    return MEISO_CAMERA_V4L2_RELEASE_RELEASED;
}
