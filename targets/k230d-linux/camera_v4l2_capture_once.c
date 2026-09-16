#define _POSIX_C_SOURCE 200809L

#include <meiso/edge/linux/camera_v4l2.h>
#include "../../src/core/meiso_sha256_private.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    CAPTURE_SHA256_BYTES = 32U,
    CAPTURE_FRAME_COUNT = 3U,
    CAPTURE_LINE_BYTES = 4096U
};

#define CAPTURE_DEADLINE_NS UINT64_C(8000000000)
#define CAPTURE_RETRY_NS UINT64_C(1000000)

#ifdef MEISO_K230_LINUX_TARGET
#define CAPTURE_BUILD_TARGET "k230d_linux"
#else
#define CAPTURE_BUILD_TARGET "native_host"
#endif

typedef struct {
    uint64_t source_clock_epoch;
    uint64_t effective_monotonic_ns;
    uint64_t producer_dropped_frames;
    uint64_t fnv1a64;
    uint32_t effective_sequence;
    uint32_t raw_driver_sequence;
    uint32_t raw_field;
    uint32_t bytes_used;
    MeisoCameraV4l2FieldProvenance field_provenance;
    MeisoCameraV4l2SequenceProvenance sequence_provenance;
    MeisoCameraV4l2TimestampProvenance timestamp_provenance;
    int64_t raw_timestamp_seconds;
    int32_t raw_timestamp_microseconds;
    bool driver_sequence_present;
    bool raw_timestamp_monotonic;
    uint8_t sha256[CAPTURE_SHA256_BYTES];
} CaptureFrameReceipt;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t bytes_per_line;
    uint32_t size_image;
    uint32_t driver_buffer_count;
    uint32_t mapped_buffer_count;
    uint32_t queued_buffers_running;
    uint64_t mapped_bytes;
    MeisoCameraV4l2Snapshot final_snapshot;
    CaptureFrameReceipt frames[CAPTURE_FRAME_COUNT];
} CaptureReceipt;

typedef struct {
    char *bytes;
    size_t capacity;
    size_t size;
    bool valid;
} CaptureLine;

int meiso_camera_v4l2_capture_once_run(
    FILE *output,
    FILE *error_output,
    const MeisoCameraV4l2Config *config
);

/*
 * The 8-second bound below covers only nonblocking frame polling after a
 * successful STREAMON. It cannot preempt a blocking driver ioctl inside
 * start/stop. E_CLEANUP with detail 2 means the bounded second cleanup attempt
 * still left an owned resource in the public ledger; the CLI then exits and a
 * NO_MAIN caller must treat the process as terminal, not safely invoke another
 * capture in the same process.
 */

static uint64_t fnv1a64(MeisoByteView bytes)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t index;

    for (index = 0U; index < bytes.size; ++index) {
        hash ^= bytes.data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void digest_hex(const uint8_t digest[32], char output[65])
{
    static const char alphabet[] = "0123456789abcdef";
    size_t index;

    for (index = 0U; index < 32U; ++index) {
        output[index * 2U] = alphabet[digest[index] >> 4U];
        output[index * 2U + 1U] = alphabet[digest[index] & 0x0fU];
    }
    output[64] = '\0';
}

static bool monotonic_now(uint64_t *out_ns)
{
    struct timespec now;

    if (out_ns == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000L ||
        (uint64_t)now.tv_sec > UINT64_MAX / UINT64_C(1000000000)) {
        return false;
    }
    *out_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec;
    return true;
}

static bool retry_pause(uint64_t remaining_ns)
{
    struct timespec delay;
    const uint64_t selected = remaining_ns < CAPTURE_RETRY_NS ?
        remaining_ns : CAPTURE_RETRY_NS;

    delay.tv_sec = 0;
    delay.tv_nsec = (long)selected;
    return nanosleep(&delay, NULL) == 0 || errno == EINTR;
}

static void fixed_error(FILE *error_output, const char *code, unsigned detail)
{
    if (error_output != NULL && code != NULL) {
        (void)fprintf(
            error_output,
            "{\"type\":\"camera_capture_error\",\"code\":\"%s\","
            "\"detail\":%u}\n",
            code,
            detail
        );
        (void)fflush(error_output);
    }
}

static void poll_error(
    FILE *error_output,
    MeisoCameraV4l2PollStatus status,
    const MeisoCameraV4l2Adapter *adapter
)
{
    MeisoCameraV4l2Snapshot snapshot;

    if (error_output == NULL || adapter == NULL ||
        !meiso_camera_v4l2_snapshot(adapter, &snapshot) ||
        !snapshot.last_dequeue.present) {
        fixed_error(error_output, "E_POLL", status);
        return;
    }
    (void)fprintf(
        error_output,
        "{\"type\":\"camera_capture_error\",\"code\":\"E_POLL\","
        "\"detail\":%u,\"primary_failure_stage\":%u,"
        "\"primary_error\":%d,\"cleanup_failure_stage\":%u,"
        "\"cleanup_error\":%d,\"reject_reason\":%u,"
        "\"dq_index\":%u,\"length\":%u,\"bytes_used\":%u,"
        "\"raw_field\":%u,\"field_provenance\":%u,"
        "\"raw_driver_sequence\":%u,"
        "\"driver_sequence_present\":%s,"
        "\"raw_timestamp_seconds\":%" PRId64 ","
        "\"raw_timestamp_microseconds\":%" PRId32 ","
        "\"raw_timestamp_monotonic\":%s,"
        "\"timestamp_candidate_present\":%s,"
        "\"timestamp_candidate_monotonic_ns\":%" PRIu64 ","
        "\"timestamp_provenance\":%u}\n",
        (unsigned)status,
        (unsigned)snapshot.primary_failure_stage,
        snapshot.primary_error,
        (unsigned)snapshot.cleanup_failure_stage,
        snapshot.cleanup_error,
        (unsigned)snapshot.last_dequeue.reject_reason,
        (unsigned)snapshot.last_dequeue.index,
        (unsigned)snapshot.last_dequeue.length,
        (unsigned)snapshot.last_dequeue.bytes_used,
        (unsigned)snapshot.last_dequeue.raw_field,
        (unsigned)snapshot.last_dequeue.field_provenance,
        (unsigned)snapshot.last_dequeue.raw_driver_sequence,
        snapshot.last_dequeue.driver_sequence_present ? "true" : "false",
        snapshot.last_dequeue.timestamp_seconds,
        snapshot.last_dequeue.timestamp_microseconds,
        snapshot.last_dequeue.timestamp_monotonic ? "true" : "false",
        snapshot.last_dequeue.timestamp_candidate_present ? "true" : "false",
        snapshot.last_dequeue.timestamp_candidate_monotonic_ns,
        (unsigned)snapshot.last_dequeue.timestamp_provenance
    );
    (void)fflush(error_output);
}

static bool capture_line_append(CaptureLine *line, const char *format, ...)
{
    va_list arguments;
    int written;

    if (line == NULL || !line->valid || format == NULL ||
        line->size >= line->capacity) {
        return false;
    }
    va_start(arguments, format);
    written = vsnprintf(
        line->bytes + line->size,
        line->capacity - line->size,
        format,
        arguments
    );
    va_end(arguments);
    if (written < 0 || (size_t)written >= line->capacity - line->size) {
        line->valid = false;
        return false;
    }
    line->size += (size_t)written;
    return true;
}

static bool write_receipt(
    FILE *output,
    const CaptureReceipt *receipt
)
{
    char digest_text[CAPTURE_FRAME_COUNT][65];
    char bytes[CAPTURE_LINE_BYTES];
    CaptureLine line = {bytes, sizeof(bytes), 0U, true};
    const MeisoCameraV4l2Snapshot *final;
    uint32_t index;

    if (output == NULL || receipt == NULL) {
        return false;
    }
    for (index = 0U; index < CAPTURE_FRAME_COUNT; ++index) {
        digest_hex(receipt->frames[index].sha256, digest_text[index]);
    }
    final = &receipt->final_snapshot;
    if (!capture_line_append(
            &line,
        "{\"type\":\"camera_capture_probe\","
        "\"schema\":\"meiso.camera_v4l2_capture_probe.v2\","
        "\"verification\":\"THREE_FRAME_CAPTURE_RECEIPT\","
        "\"build_target\":\"%s\",\"pixel_format\":\"NV12\","
        "\"width\":%u,\"height\":%u,\"bytes_per_line\":%u,"
        "\"size_image\":%u,\"driver_buffer_count\":%u,"
        "\"mapped_buffer_count\":%u,\"mapped_bytes\":%" PRIu64
        ",\"queued_buffers_running\":%u,"
        "\"frame_count\":3,\"frames\":[",
        CAPTURE_BUILD_TARGET,
        (unsigned)receipt->width,
        (unsigned)receipt->height,
        (unsigned)receipt->bytes_per_line,
        (unsigned)receipt->size_image,
        (unsigned)receipt->driver_buffer_count,
        (unsigned)receipt->mapped_buffer_count,
        receipt->mapped_bytes,
        (unsigned)receipt->queued_buffers_running
        )) {
        return false;
    }
    for (index = 0U; index < CAPTURE_FRAME_COUNT; ++index) {
        const CaptureFrameReceipt *frame = &receipt->frames[index];
        if (!capture_line_append(
                &line,
                "%s{\"ordinal\":%u,\"source_clock_epoch\":%" PRIu64
                ",\"raw_timestamp_seconds\":%" PRId64
                ",\"raw_timestamp_microseconds\":%" PRId32
                ",\"raw_timestamp_monotonic\":%s,"
                "\"effective_monotonic_ns\":%" PRIu64 ","
                "\"timestamp_provenance\":%u,\"raw_field\":%u,"
                "\"field_provenance\":%u,\"raw_driver_sequence\":%u,"
                "\"driver_sequence_present\":%s,\"effective_sequence\":%u,"
                "\"sequence_provenance\":%u,"
                "\"producer_dropped_frames\":%" PRIu64 ","
                "\"bytes_used\":%u,\"fnv1a64\":\"%016" PRIx64
                "\",\"sha256\":\"%s\"}",
                index == 0U ? "" : ",",
                (unsigned)(index + 1U),
                frame->source_clock_epoch,
                frame->raw_timestamp_seconds,
                frame->raw_timestamp_microseconds,
                frame->raw_timestamp_monotonic ? "true" : "false",
                frame->effective_monotonic_ns,
                (unsigned)frame->timestamp_provenance,
                (unsigned)frame->raw_field,
                (unsigned)frame->field_provenance,
                (unsigned)frame->raw_driver_sequence,
                frame->driver_sequence_present ? "true" : "false",
                (unsigned)frame->effective_sequence,
                (unsigned)frame->sequence_provenance,
                frame->producer_dropped_frames,
                (unsigned)frame->bytes_used,
                frame->fnv1a64,
                digest_text[index]
            )) {
            return false;
        }
    }
    if (!capture_line_append(
            &line,
            "],\"final_cleanup\":{\"phase\":%u,"
            "\"run_generation\":%" PRIu64 ","
            "\"source_clock_epoch\":%" PRIu64 ","
            "\"driver_buffer_count\":%u,\"mapped_buffer_count\":%u,"
            "\"queued_buffer_count\":%u,\"stream_on\":%s,"
            "\"frame_borrowed\":%s,\"producer_dropped_frames\":%" PRIu64
            ",\"driver_sequence_present\":%s,"
            "\"last_driver_sequence\":%u,\"last_sequence\":%u,"
            "\"last_sequence_provenance\":%u,"
            "\"last_effective_monotonic_ns\":%" PRIu64 ","
            "\"last_timestamp_provenance\":%u,"
            "\"primary_failure_stage\":%u,\"primary_error\":%d,"
            "\"cleanup_failure_stage\":%u,\"cleanup_error\":%d,"
            "\"arena_bytes\":%zu,\"mapped_bytes\":%" PRIu64 "}}",
            (unsigned)final->phase,
            final->run_generation,
            final->source_clock_epoch,
            (unsigned)final->driver_buffer_count,
            (unsigned)final->mapped_buffer_count,
            (unsigned)final->queued_buffer_count,
            final->stream_on ? "true" : "false",
            final->frame_borrowed ? "true" : "false",
            final->producer_dropped_frames,
            final->driver_sequence_present ? "true" : "false",
            (unsigned)final->last_driver_sequence,
            (unsigned)final->last_sequence,
            (unsigned)final->last_sequence_provenance,
            final->last_timestamp_monotonic_ns,
            (unsigned)final->last_timestamp_provenance,
            (unsigned)final->primary_failure_stage,
            final->primary_error,
            (unsigned)final->cleanup_failure_stage,
            final->cleanup_error,
            final->arena_bytes,
            final->mapped_bytes
        )) {
        return false;
    }
    return line.valid && fputs(line.bytes, output) >= 0 &&
        fputc('\n', output) != EOF &&
        fflush(output) == 0;
}

static bool cleanup_ledger_clear(
    MeisoCameraV4l2Adapter *adapter,
    bool require_stopped
)
{
    MeisoCameraV4l2Snapshot snapshot;

    return adapter != NULL &&
        meiso_camera_v4l2_snapshot(adapter, &snapshot) &&
        (!require_stopped ||
         snapshot.phase == MEISO_CAMERA_V4L2_PHASE_STOPPED) &&
        !snapshot.stream_on && !snapshot.frame_borrowed &&
        snapshot.mapped_buffer_count == 0U &&
        snapshot.queued_buffer_count == 0U &&
        snapshot.mapped_bytes == 0U;
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

int meiso_camera_v4l2_capture_once_run(
    FILE *output,
    FILE *error_output,
    const MeisoCameraV4l2Config *config
)
{
    MeisoCameraV4l2Adapter *adapter = NULL;
    MeisoCameraV4l2FrameView frame;
    MeisoCameraV4l2Snapshot snapshot;
    CaptureReceipt receipt;
    MeisoSha256 sha;
    void *arena = NULL;
    uint64_t deadline = 0U;
    bool adapter_initialized = false;
    bool stopped = false;
    bool frame_present = false;
    bool cleanup_error = false;
    unsigned stop_attempts = 0U;
    uint32_t frame_index;
    int result = 3;

    if (output == NULL || error_output == NULL || output == error_output ||
        config == NULL) {
        fixed_error(error_output, "E_ARGUMENT", 0U);
        return 2;
    }
    memset(&receipt, 0, sizeof(receipt));
    arena = malloc(meiso_camera_v4l2_memory_required());
    if (arena == NULL) {
        fixed_error(error_output, "E_ALLOC", 0U);
        return 3;
    }
    {
        const MeisoCameraV4l2InitStatus init_status =
            meiso_camera_v4l2_init(
                arena, meiso_camera_v4l2_memory_required(), config, &adapter
            );
        if (init_status != MEISO_CAMERA_V4L2_INIT_OK) {
            fixed_error(error_output, "E_INIT", init_status);
            goto cleanup;
        }
    }
    adapter_initialized = true;
    {
        const MeisoCameraV4l2StartStatus start_status =
            meiso_camera_v4l2_start(adapter);
        if (start_status != MEISO_CAMERA_V4L2_START_STARTED) {
            fixed_error(error_output, "E_START", start_status);
            goto cleanup;
        }
    }
    if (!meiso_camera_v4l2_snapshot(adapter, &snapshot) ||
        snapshot.phase != MEISO_CAMERA_V4L2_PHASE_RUNNING ||
        snapshot.driver_buffer_count <
            MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT ||
        snapshot.mapped_buffer_count !=
            MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT ||
        snapshot.queued_buffer_count !=
            MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT ||
        !snapshot.stream_on || snapshot.frame_borrowed ||
        snapshot.mapped_bytes <
            (uint64_t)MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT *
                MEISO_CAMERA_V4L2_SIZE_IMAGE) {
        fixed_error(error_output, "E_SNAPSHOT", 1U);
        goto cleanup;
    }
    receipt.driver_buffer_count = snapshot.driver_buffer_count;
    receipt.mapped_buffer_count = snapshot.mapped_buffer_count;
    receipt.mapped_bytes = snapshot.mapped_bytes;
    receipt.queued_buffers_running = snapshot.queued_buffer_count;
    if (!monotonic_now(&deadline)) {
        fixed_error(error_output, "E_CLOCK", 0U);
        goto cleanup;
    }
    deadline = deadline > UINT64_MAX - CAPTURE_DEADLINE_NS ?
        UINT64_MAX : deadline + CAPTURE_DEADLINE_NS;
    for (frame_index = 0U;
         frame_index < CAPTURE_FRAME_COUNT;
         ++frame_index) {
        CaptureFrameReceipt *frame_receipt = &receipt.frames[frame_index];

        for (;;) {
            uint64_t now;
            MeisoCameraV4l2PollStatus poll_status;

            if (!monotonic_now(&now)) {
                fixed_error(error_output, "E_CLOCK", 0U);
                goto cleanup;
            }
            if (now >= deadline) {
                fixed_error(error_output, "E_TIMEOUT", 0U);
                goto cleanup;
            }
            poll_status = meiso_camera_v4l2_poll_frame(adapter, &frame);
            if (poll_status == MEISO_CAMERA_V4L2_POLL_FRAME) {
                frame_present = true;
                break;
            }
            if (poll_status != MEISO_CAMERA_V4L2_POLL_WOULD_BLOCK &&
                poll_status != MEISO_CAMERA_V4L2_POLL_INTERRUPTED) {
                poll_error(error_output, poll_status, adapter);
                goto cleanup;
            }
            if (!monotonic_now(&now)) {
                fixed_error(error_output, "E_CLOCK", 0U);
                goto cleanup;
            }
            if (now >= deadline) {
                fixed_error(error_output, "E_TIMEOUT", 0U);
                goto cleanup;
            }
            if (!retry_pause(deadline - now)) {
                fixed_error(error_output, "E_WAIT", (unsigned)errno);
                goto cleanup;
            }
        }
        if (!meiso_camera_v4l2_snapshot(adapter, &snapshot) ||
            snapshot.phase != MEISO_CAMERA_V4L2_PHASE_RUNNING ||
            !snapshot.stream_on || !snapshot.frame_borrowed ||
            !frame_ref_equal(snapshot.borrowed_frame, frame.ref) ||
            snapshot.mapped_buffer_count !=
                MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT ||
            snapshot.queued_buffer_count + 1U !=
                MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT ||
            !snapshot.last_dequeue.present ||
            snapshot.last_dequeue.reject_reason !=
                MEISO_CAMERA_V4L2_FRAME_REJECT_NONE ||
            !snapshot.last_dequeue.timestamp_candidate_present ||
            snapshot.last_dequeue.timestamp_candidate_monotonic_ns !=
                frame.source_time.monotonic_ns ||
            snapshot.last_dequeue.timestamp_provenance !=
                frame.timestamp_provenance ||
            snapshot.last_timestamp_monotonic_ns !=
                frame.source_time.monotonic_ns ||
            snapshot.last_timestamp_provenance !=
                frame.timestamp_provenance) {
            fixed_error(error_output, "E_SNAPSHOT", 2U);
            goto cleanup;
        }
        if (frame_index == 0U) {
            receipt.width = frame.width;
            receipt.height = frame.height;
            receipt.bytes_per_line = frame.bytes_per_line;
            receipt.size_image = frame.size_image;
        } else if (receipt.width != frame.width ||
                   receipt.height != frame.height ||
                   receipt.bytes_per_line != frame.bytes_per_line ||
                   receipt.size_image != frame.size_image) {
            fixed_error(error_output, "E_FRAME", 1U);
            goto cleanup;
        }
        frame_receipt->source_clock_epoch = frame.source_time.clock_epoch;
        frame_receipt->effective_monotonic_ns =
            frame.source_time.monotonic_ns;
        frame_receipt->producer_dropped_frames =
            frame.producer_dropped_frames;
        frame_receipt->effective_sequence = frame.ref.driver_sequence;
        frame_receipt->sequence_provenance =
            frame.ref.sequence_provenance;
        frame_receipt->timestamp_provenance =
            frame.timestamp_provenance;
        frame_receipt->raw_driver_sequence =
            snapshot.last_dequeue.raw_driver_sequence;
        frame_receipt->driver_sequence_present =
            snapshot.last_dequeue.driver_sequence_present;
        frame_receipt->raw_field = snapshot.last_dequeue.raw_field;
        frame_receipt->field_provenance =
            snapshot.last_dequeue.field_provenance;
        frame_receipt->raw_timestamp_seconds =
            snapshot.last_dequeue.timestamp_seconds;
        frame_receipt->raw_timestamp_microseconds =
            snapshot.last_dequeue.timestamp_microseconds;
        frame_receipt->raw_timestamp_monotonic =
            snapshot.last_dequeue.timestamp_monotonic;
        frame_receipt->bytes_used = snapshot.last_dequeue.bytes_used;
        frame_receipt->fnv1a64 = fnv1a64(frame.bytes);
        meiso_sha256_init(&sha);
        meiso_sha256_update(&sha, frame.bytes.data, frame.bytes.size);
        meiso_sha256_final(&sha, frame_receipt->sha256);
        {
            const MeisoCameraV4l2ReleaseStatus release_status =
                meiso_camera_v4l2_release_frame(adapter, &frame.ref);
            if (release_status != MEISO_CAMERA_V4L2_RELEASE_RELEASED) {
                fixed_error(error_output, "E_RELEASE", release_status);
                goto cleanup;
            }
            frame_present = false;
        }
        if (!meiso_camera_v4l2_snapshot(adapter, &snapshot) ||
            snapshot.phase != MEISO_CAMERA_V4L2_PHASE_RUNNING ||
            !snapshot.stream_on || snapshot.frame_borrowed ||
            snapshot.mapped_buffer_count !=
                MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT ||
            snapshot.queued_buffer_count !=
                MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT) {
            fixed_error(error_output, "E_SNAPSHOT", 3U);
            goto cleanup;
        }
    }
    {
        const MeisoCameraV4l2StopStatus stop_status =
            meiso_camera_v4l2_stop(adapter);
        ++stop_attempts;
        if (stop_status != MEISO_CAMERA_V4L2_STOP_STOPPED) {
            cleanup_error = true;
            result = 5;
            goto cleanup;
        }
        stopped = true;
    }
    if (!cleanup_ledger_clear(adapter, true) ||
        !meiso_camera_v4l2_snapshot(adapter, &snapshot)) {
        cleanup_error = true;
        stopped = false;
        result = 5;
        goto cleanup;
    }
    receipt.final_snapshot = snapshot;
    if (!write_receipt(output, &receipt)) {
        fixed_error(error_output, "E_WRITE", 0U);
        result = 4;
        goto cleanup;
    }
    result = 0;

cleanup:
    if (frame_present) {
        const MeisoCameraV4l2ReleaseStatus release_status =
            meiso_camera_v4l2_release_frame(adapter, &frame.ref);
        if (release_status != MEISO_CAMERA_V4L2_RELEASE_RELEASED &&
            release_status != MEISO_CAMERA_V4L2_RELEASE_NO_FRAME) {
            cleanup_error = true;
        }
    }
    if (adapter_initialized && !stopped) {
        MeisoCameraV4l2StopStatus stop_status =
            MEISO_CAMERA_V4L2_STOP_INVALID_ARGUMENT;

        while (stop_attempts < 2U) {
            stop_status = meiso_camera_v4l2_stop(adapter);
            ++stop_attempts;
            if (stop_status == MEISO_CAMERA_V4L2_STOP_STOPPED ||
                stop_status == MEISO_CAMERA_V4L2_STOP_UNCHANGED) {
                stopped = true;
                break;
            }
        }
        if (!cleanup_ledger_clear(adapter, stopped)) {
            fixed_error(error_output, "E_CLEANUP", 2U);
            result = 5;
        } else if (cleanup_error || !stopped) {
            fixed_error(error_output, "E_CLEANUP", 1U);
            result = 5;
        }
    }
    free(arena);
    return result;
}

#ifndef MEISO_CAMERA_V4L2_CAPTURE_ONCE_NO_MAIN
int main(void)
{
    MeisoCameraV4l2Config config;

    meiso_camera_v4l2_default_config(UINT64_C(1), &config);
    return meiso_camera_v4l2_capture_once_run(stdout, stderr, &config);
}
#else
int meiso_camera_v4l2_capture_once_test_write_max_receipt(FILE *output)
{
    CaptureReceipt receipt;
    uint32_t index;

    if (output == NULL) {
        return 2;
    }
    memset(&receipt, 0, sizeof(receipt));
    receipt.width = UINT16_MAX;
    receipt.height = UINT16_MAX;
    receipt.bytes_per_line = UINT32_MAX;
    receipt.size_image = UINT32_MAX;
    receipt.driver_buffer_count = UINT32_MAX;
    receipt.mapped_buffer_count = UINT32_MAX;
    receipt.queued_buffers_running = UINT32_MAX;
    receipt.mapped_bytes = UINT64_MAX;
    for (index = 0U; index < CAPTURE_FRAME_COUNT; ++index) {
        receipt.frames[index].source_clock_epoch = UINT64_MAX;
        receipt.frames[index].effective_monotonic_ns = UINT64_MAX;
        receipt.frames[index].producer_dropped_frames = UINT64_MAX;
        receipt.frames[index].fnv1a64 = UINT64_MAX;
        receipt.frames[index].effective_sequence = UINT32_MAX;
        receipt.frames[index].raw_driver_sequence = UINT32_MAX;
        receipt.frames[index].raw_field = UINT32_MAX;
        receipt.frames[index].bytes_used = UINT32_MAX;
        receipt.frames[index].field_provenance = UINT8_MAX;
        receipt.frames[index].sequence_provenance = UINT8_MAX;
        receipt.frames[index].timestamp_provenance = UINT8_MAX;
        receipt.frames[index].raw_timestamp_seconds = INT64_MAX;
        receipt.frames[index].raw_timestamp_microseconds = INT32_MAX;
        receipt.frames[index].driver_sequence_present = true;
        receipt.frames[index].raw_timestamp_monotonic = true;
        memset(
            receipt.frames[index].sha256,
            UINT8_MAX,
            sizeof(receipt.frames[index].sha256)
        );
    }
    receipt.final_snapshot.phase = UINT8_MAX;
    receipt.final_snapshot.run_generation = UINT64_MAX;
    receipt.final_snapshot.source_clock_epoch = UINT64_MAX;
    receipt.final_snapshot.driver_buffer_count = UINT32_MAX;
    receipt.final_snapshot.mapped_buffer_count = UINT32_MAX;
    receipt.final_snapshot.queued_buffer_count = UINT32_MAX;
    receipt.final_snapshot.stream_on = true;
    receipt.final_snapshot.frame_borrowed = true;
    receipt.final_snapshot.producer_dropped_frames = UINT64_MAX;
    receipt.final_snapshot.driver_sequence_present = true;
    receipt.final_snapshot.last_driver_sequence = UINT32_MAX;
    receipt.final_snapshot.last_sequence = UINT32_MAX;
    receipt.final_snapshot.last_sequence_provenance = UINT8_MAX;
    receipt.final_snapshot.last_timestamp_monotonic_ns = UINT64_MAX;
    receipt.final_snapshot.last_timestamp_provenance = UINT8_MAX;
    receipt.final_snapshot.primary_failure_stage = UINT8_MAX;
    receipt.final_snapshot.primary_error = INT_MAX;
    receipt.final_snapshot.cleanup_failure_stage = UINT8_MAX;
    receipt.final_snapshot.cleanup_error = INT_MAX;
    receipt.final_snapshot.arena_bytes = SIZE_MAX;
    receipt.final_snapshot.mapped_bytes = UINT64_MAX;
    return write_receipt(output, &receipt) ? 0 : 1;
}
#endif
