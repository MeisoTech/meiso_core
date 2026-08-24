#ifndef MEISO_CAMERA_V4L2_ADAPTER_H
#define MEISO_CAMERA_V4L2_ADAPTER_H

#include "composition_session_v3.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * K230 Linux single-planar V4L2 capture adapter for the Profile v3 camera
 * MediaStream endpoint. This is a target-local source, not a new wire or
 * Profile contract. It neither configures a sensor nor starts, stops or
 * supervises isp_media_server.
 *
 * The selected MVP is exact progressive NV12 320x240: bytesperline=320 and
 * sizeimage/access-unit=115200. The adapter requests four MMAP buffers and
 * fails closed if VIDIOC_REQBUFS returns fewer than four. It owns the opened
 * fd and every successful mapping until full stop/terminal cleanup; it owns
 * no heap, thread, event loop or MediaTransport object.
 */
#define MEISO_CAMERA_V4L2_DEVICE_PATH_CAPACITY 192U
#define MEISO_CAMERA_V4L2_WIDTH 320U
#define MEISO_CAMERA_V4L2_HEIGHT 240U
#define MEISO_CAMERA_V4L2_BYTES_PER_LINE MEISO_CAMERA_V4L2_WIDTH
#define MEISO_CAMERA_V4L2_SIZE_IMAGE \
    (MEISO_CAMERA_V4L2_WIDTH * MEISO_CAMERA_V4L2_HEIGHT * 3U / 2U)
#define MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT 4U

typedef struct MeisoCameraV4l2Adapter MeisoCameraV4l2Adapter;

typedef uint8_t MeisoCameraV4l2IoctlRequest;
enum {
    MEISO_CAMERA_V4L2_IOCTL_QUERY_CAPABILITY = 1,
    MEISO_CAMERA_V4L2_IOCTL_SET_FORMAT = 2,
    MEISO_CAMERA_V4L2_IOCTL_GET_FORMAT = 3,
    MEISO_CAMERA_V4L2_IOCTL_REQUEST_BUFFERS = 4,
    MEISO_CAMERA_V4L2_IOCTL_QUERY_BUFFER = 5,
    MEISO_CAMERA_V4L2_IOCTL_QUEUE_BUFFER = 6,
    MEISO_CAMERA_V4L2_IOCTL_STREAM_ON = 7,
    MEISO_CAMERA_V4L2_IOCTL_DEQUEUE_BUFFER = 8,
    MEISO_CAMERA_V4L2_IOCTL_STREAM_OFF = 9
};

typedef struct {
    bool video_capture;
    bool streaming;
} MeisoCameraV4l2CapabilityIoctl;

typedef struct {
    uint16_t width;
    uint16_t height;
    MeisoCompositionMediaPixelFormat pixel_format;
    bool progressive;
    uint32_t bytes_per_line;
    uint32_t size_image;
} MeisoCameraV4l2FormatIoctl;

typedef struct {
    uint32_t count;
} MeisoCameraV4l2RequestBuffersIoctl;

/* Exact Linux v4l2_field values retained at the target syscall boundary. */
#define MEISO_CAMERA_V4L2_RAW_FIELD_ANY UINT32_C(0)
#define MEISO_CAMERA_V4L2_RAW_FIELD_NONE UINT32_C(1)

typedef uint8_t MeisoCameraV4l2FieldProvenance;
enum {
    MEISO_CAMERA_V4L2_FIELD_UNKNOWN = 0,
    MEISO_CAMERA_V4L2_FIELD_EXPLICIT_PROGRESSIVE = 1,
    MEISO_CAMERA_V4L2_FIELD_FORMAT_INHERITED_PROGRESSIVE = 2
};

typedef uint8_t MeisoCameraV4l2SequenceProvenance;
enum {
    MEISO_CAMERA_V4L2_SEQUENCE_NONE = 0,
    MEISO_CAMERA_V4L2_SEQUENCE_DRIVER = 1,
    MEISO_CAMERA_V4L2_SEQUENCE_ADAPTER_SYNTHETIC = 2
};

typedef struct {
    uint32_t index;
    uint32_t length;
    uint64_t memory_offset;
    uint32_t bytes_used;
    /* Raw public v4l2_buffer.sequence, even when it is not a frame counter. */
    uint32_t driver_sequence;
    bool driver_sequence_present;
    int64_t timestamp_seconds;
    int32_t timestamp_microseconds;
    bool timestamp_monotonic;
    uint32_t raw_field;
    MeisoCameraV4l2FieldProvenance field_provenance;
    bool frame_error;
} MeisoCameraV4l2BufferIoctl;

/*
 * A dequeue callback must preserve the raw public field/sequence values.
 * EXPLICIT_PROGRESSIVE pairs only with RAW_FIELD_NONE;
 * FORMAT_INHERITED_PROGRESSIVE pairs only with RAW_FIELD_ANY after the
 * adapter's exact progressive G_FMT admission. driver_sequence_present is
 * true only when the public sequence is a real per-frame counter. The pinned
 * native vvcam translation sets it false because that driver exposes zero and
 * uses a separate private buffer-index sequence internally.
 */

typedef union {
    MeisoCameraV4l2CapabilityIoctl capability;
    MeisoCameraV4l2FormatIoctl format;
    MeisoCameraV4l2RequestBuffersIoctl request_buffers;
    MeisoCameraV4l2BufferIoctl buffer;
} MeisoCameraV4l2IoctlArgument;

/*
 * Test seam and native-syscall boundary. Each operation returns zero on
 * success or a positive errno value on failure. It never returns a negative
 * value. A failed operation leaves every output byte unchanged. The public
 * request enum and typed union are target-local portable mirrors; the native
 * implementation alone translates them to exact Linux V4L2 UAPI structs.
 * Callbacks are serialized, non-reentrant and must not retain any borrowed
 * argument.
 *
 * close_device transfers fd ownership on its first call even if it reports an
 * error; the adapter never retries or reuses that integer. Failed unmap keeps
 * ownership of that mapping so a later stop may retry bounded cleanup.
 */
typedef struct {
    int (*open_device)(
        void *context,
        const char *path,
        int flags,
        int *out_fd
    );
    int (*device_ioctl)(
        void *context,
        int fd,
        MeisoCameraV4l2IoctlRequest request,
        MeisoCameraV4l2IoctlArgument *ioctl_arg
    );
    int (*map_buffer)(
        void *context,
        int fd,
        size_t length,
        uint64_t offset,
        void **out_mapping
    );
    int (*unmap_buffer)(void *context, void *mapping, size_t length);
    int (*close_device)(void *context, int fd);
} MeisoCameraV4l2Syscalls;

typedef struct {
    /* Private target locator: never serialized or logged by this adapter. */
    const char *device_path;
    /* Non-zero identity for the target CLOCK_MONOTONIC domain. */
    uint64_t source_clock_epoch;
    /* NULL selects the native Linux syscall implementation. */
    const MeisoCameraV4l2Syscalls *syscalls;
    /* Borrowed for adapter lifetime when custom syscalls are selected. */
    void *syscall_context;
} MeisoCameraV4l2Config;

typedef uint8_t MeisoCameraV4l2Phase;
enum {
    MEISO_CAMERA_V4L2_PHASE_STOPPED = 1,
    MEISO_CAMERA_V4L2_PHASE_RUNNING = 2,
    MEISO_CAMERA_V4L2_PHASE_TERMINAL = 3
};

typedef uint8_t MeisoCameraV4l2FailureStage;
enum {
    MEISO_CAMERA_V4L2_FAILURE_NONE = 0,
    MEISO_CAMERA_V4L2_FAILURE_OPEN = 1,
    MEISO_CAMERA_V4L2_FAILURE_QUERY_CAPABILITY = 2,
    MEISO_CAMERA_V4L2_FAILURE_SET_FORMAT = 3,
    MEISO_CAMERA_V4L2_FAILURE_GET_FORMAT = 4,
    MEISO_CAMERA_V4L2_FAILURE_REQUEST_BUFFERS = 5,
    MEISO_CAMERA_V4L2_FAILURE_QUERY_BUFFER = 6,
    MEISO_CAMERA_V4L2_FAILURE_MAP_BUFFER = 7,
    MEISO_CAMERA_V4L2_FAILURE_QUEUE_BUFFER = 8,
    MEISO_CAMERA_V4L2_FAILURE_STREAM_ON = 9,
    MEISO_CAMERA_V4L2_FAILURE_DEQUEUE_BUFFER = 10,
    MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT = 11,
    MEISO_CAMERA_V4L2_FAILURE_STREAM_OFF = 12,
    MEISO_CAMERA_V4L2_FAILURE_UNMAP_BUFFER = 13,
    MEISO_CAMERA_V4L2_FAILURE_RELEASE_BUFFERS = 14,
    MEISO_CAMERA_V4L2_FAILURE_CLOSE = 15
};

typedef struct {
    uint64_t run_generation;
    uint64_t borrow_serial;
    uint32_t buffer_index;
    /* Driver counter or checked adapter counter, as identified below. */
    uint32_t driver_sequence;
    MeisoCameraV4l2SequenceProvenance sequence_provenance;
} MeisoCameraV4l2FrameRef;

/*
 * bytes is exactly one contiguous NV12 access unit and borrows one driver MMAP
 * buffer. The borrow remains valid until exact successful release, or until a
 * stop/terminal boundary invalidates the complete run. The caller may copy or
 * synchronously submit it to MediaTransport but must not retain or modify it.
 */
typedef struct {
    MeisoCameraV4l2FrameRef ref;
    MeisoTimePoint source_time;
    uint64_t producer_dropped_frames;
    MeisoCameraV4l2FieldProvenance field_provenance;
    MeisoCameraV4l2SequenceProvenance sequence_provenance;
    MeisoCompositionMediaPixelFormat pixel_format;
    uint16_t width;
    uint16_t height;
    uint32_t bytes_per_line;
    uint32_t size_image;
    MeisoByteView bytes;
} MeisoCameraV4l2FrameView;

typedef uint8_t MeisoCameraV4l2FrameRejectReason;
enum {
    MEISO_CAMERA_V4L2_FRAME_REJECT_NONE = 0,
    MEISO_CAMERA_V4L2_FRAME_REJECT_INDEX = 1,
    MEISO_CAMERA_V4L2_FRAME_REJECT_LENGTH = 2,
    MEISO_CAMERA_V4L2_FRAME_REJECT_BYTES_USED = 3,
    MEISO_CAMERA_V4L2_FRAME_REJECT_TIMESTAMP_FLAG = 4,
    MEISO_CAMERA_V4L2_FRAME_REJECT_FIELD = 5,
    MEISO_CAMERA_V4L2_FRAME_REJECT_FRAME_ERROR = 6,
    MEISO_CAMERA_V4L2_FRAME_REJECT_TIMESTAMP_VALUE = 7,
    MEISO_CAMERA_V4L2_FRAME_REJECT_MAPPING = 8,
    MEISO_CAMERA_V4L2_FRAME_REJECT_TIMESTAMP_REGRESSION = 9,
    MEISO_CAMERA_V4L2_FRAME_REJECT_SEQUENCE = 10,
    MEISO_CAMERA_V4L2_FRAME_REJECT_COUNTER_EXHAUSTED = 11
};

/*
 * Bounded copy of the most recent successful DQBUF translation. It survives
 * terminal cleanup so a one-shot target runner can report why a frame was
 * rejected without retaining the MMAP borrow or a device path.
 */
typedef struct {
    bool present;
    uint32_t index;
    uint32_t length;
    uint32_t bytes_used;
    uint32_t raw_field;
    MeisoCameraV4l2FieldProvenance field_provenance;
    uint32_t raw_driver_sequence;
    bool driver_sequence_present;
    int64_t timestamp_seconds;
    int32_t timestamp_microseconds;
    bool timestamp_monotonic;
    bool frame_error;
    MeisoCameraV4l2FrameRejectReason reject_reason;
} MeisoCameraV4l2DequeuedDiagnostic;

typedef struct {
    MeisoCameraV4l2Phase phase;
    uint64_t run_generation;
    uint64_t source_clock_epoch;
    uint32_t driver_buffer_count;
    uint32_t mapped_buffer_count;
    uint32_t queued_buffer_count;
    bool stream_on;
    bool frame_borrowed;
    MeisoCameraV4l2FrameRef borrowed_frame;
    uint64_t producer_dropped_frames;
    bool driver_sequence_present;
    uint32_t last_driver_sequence;
    uint32_t last_sequence;
    MeisoCameraV4l2SequenceProvenance last_sequence_provenance;
    uint64_t last_timestamp_monotonic_ns;
    MeisoCameraV4l2DequeuedDiagnostic last_dequeue;
    /* Primary operation failure and first cleanup failure never overwrite. */
    MeisoCameraV4l2FailureStage primary_failure_stage;
    int primary_error;
    MeisoCameraV4l2FailureStage cleanup_failure_stage;
    int cleanup_error;
    /* Legacy aggregate: primary when present, otherwise first cleanup. */
    MeisoCameraV4l2FailureStage last_failure_stage;
    int last_error;
    size_t arena_bytes;
    uint64_t mapped_bytes;
} MeisoCameraV4l2Snapshot;

typedef uint8_t MeisoCameraV4l2InitStatus;
enum {
    MEISO_CAMERA_V4L2_INIT_OK = 0,
    MEISO_CAMERA_V4L2_INIT_INVALID_ARGUMENT = 1,
    MEISO_CAMERA_V4L2_INIT_INVALID_CONFIG = 2,
    MEISO_CAMERA_V4L2_INIT_MISALIGNED_ARENA = 3,
    MEISO_CAMERA_V4L2_INIT_ARENA_TOO_SMALL = 4
};

typedef uint8_t MeisoCameraV4l2StartStatus;
enum {
    MEISO_CAMERA_V4L2_START_STARTED = 0,
    MEISO_CAMERA_V4L2_START_UNCHANGED = 1,
    MEISO_CAMERA_V4L2_START_OPEN_FAILED = 2,
    MEISO_CAMERA_V4L2_START_DEVICE_UNSUPPORTED = 3,
    MEISO_CAMERA_V4L2_START_FORMAT_UNSUPPORTED = 4,
    MEISO_CAMERA_V4L2_START_INSUFFICIENT_BUFFERS = 5,
    MEISO_CAMERA_V4L2_START_BUFFER_SETUP_FAILED = 6,
    MEISO_CAMERA_V4L2_START_STREAM_FAILED = 7,
    MEISO_CAMERA_V4L2_START_COUNTER_EXHAUSTED = 8,
    MEISO_CAMERA_V4L2_START_CLEANUP_FAILED = 9,
    MEISO_CAMERA_V4L2_START_TERMINAL = 10,
    MEISO_CAMERA_V4L2_START_INVALID_ARGUMENT = 11
};

typedef uint8_t MeisoCameraV4l2PollStatus;
enum {
    MEISO_CAMERA_V4L2_POLL_FRAME = 0,
    MEISO_CAMERA_V4L2_POLL_WOULD_BLOCK = 1,
    MEISO_CAMERA_V4L2_POLL_INTERRUPTED = 2,
    MEISO_CAMERA_V4L2_POLL_BORROW_ACTIVE = 3,
    MEISO_CAMERA_V4L2_POLL_NOT_RUNNING = 4,
    MEISO_CAMERA_V4L2_POLL_COUNTER_EXHAUSTED = 5,
    MEISO_CAMERA_V4L2_POLL_TERMINAL = 6,
    MEISO_CAMERA_V4L2_POLL_INVALID_ARGUMENT = 7
};

typedef uint8_t MeisoCameraV4l2ReleaseStatus;
enum {
    MEISO_CAMERA_V4L2_RELEASE_RELEASED = 0,
    MEISO_CAMERA_V4L2_RELEASE_NO_FRAME = 1,
    MEISO_CAMERA_V4L2_RELEASE_MISMATCH = 2,
    MEISO_CAMERA_V4L2_RELEASE_TERMINAL = 3,
    MEISO_CAMERA_V4L2_RELEASE_INVALID_ARGUMENT = 4
};

typedef uint8_t MeisoCameraV4l2StopStatus;
enum {
    MEISO_CAMERA_V4L2_STOP_STOPPED = 0,
    MEISO_CAMERA_V4L2_STOP_UNCHANGED = 1,
    MEISO_CAMERA_V4L2_STOP_TERMINAL = 2,
    MEISO_CAMERA_V4L2_STOP_INVALID_ARGUMENT = 3
};

/* Writes the private `/dev/video1` locator and selects native syscalls. */
void meiso_camera_v4l2_default_config(
    uint64_t source_clock_epoch,
    MeisoCameraV4l2Config *out_config
);

size_t meiso_camera_v4l2_alignment_required(void);
size_t meiso_camera_v4l2_memory_required(void);

/*
 * config, its bounded device-path bytes, a custom syscall table, arena and
 * out_adapter storage must be mutually disjoint. Custom syscall context is an
 * opaque caller-owned borrow and must also be outside the arena. The adapter
 * copies path and syscall table; only the context pointer is retained. Init
 * performs no I/O. Failure leaves arena and out_adapter byte-exact unchanged.
 */
MeisoCameraV4l2InitStatus meiso_camera_v4l2_init(
    void *arena,
    size_t arena_size,
    const MeisoCameraV4l2Config *config,
    MeisoCameraV4l2Adapter **out_adapter
);

/*
 * Opens O_RDWR|O_NONBLOCK|O_CLOEXEC, requires VIDEO_CAPTURE+STREAMING, sets
 * and reads back exact progressive NV12 320x240/320/115200, requests at least
 * four MMAP buffers, maps and queues exactly four, then STREAMONs. A normal
 * failure performs full reverse cleanup and leaves phase STOPPED so start may
 * be retried. Cleanup failure enters sticky TERMINAL. Repeated start while
 * RUNNING is UNCHANGED; TERMINAL requires a new initialized adapter.
 */
MeisoCameraV4l2StartStatus meiso_camera_v4l2_start(
    MeisoCameraV4l2Adapter *adapter
);

/*
 * Performs at most one nonblocking VIDIOC_DQBUF. EAGAIN/EWOULDBLOCK returns
 * WOULD_BLOCK and EINTR returns INTERRUPTED with adapter and output byte-exact
 * unchanged. Only FRAME changes out_frame and creates the sole outstanding
 * borrow. A dequeued buffer must have exact index, bytesused, monotonic V4L2
 * timestamp flag/value and configured format. For a present driver sequence,
 * the first value is accepted; thereafter modulo-u32 delta 1..0x7fffffff is
 * forward, delta-1 is added to producer_dropped_frames, and zero/backward
 * deltas are fatal. The native K230 vvcam driver leaves public capture
 * sequence at zero; native
 * translation therefore marks it absent and the adapter emits a checked,
 * contiguous synthetic sequence without claiming producer drops. Custom
 * syscalls may mark a real sequence present and retain the modulo-u32 gap
 * accounting above. V4L2_FIELD_NONE is explicitly progressive. Native
 * V4L2_FIELD_ANY is accepted only as format-inherited progressive because
 * start already proved G_FMT field NONE; other fields fail closed. Any
 * post-dequeue contract or syscall failure performs full cleanup and enters
 * sticky TERMINAL without exposing partial output. The bounded last_dequeue
 * diagnostic remains queryable after that cleanup.
 */
MeisoCameraV4l2PollStatus meiso_camera_v4l2_poll_frame(
    MeisoCameraV4l2Adapter *adapter,
    MeisoCameraV4l2FrameView *out_frame
);

/*
 * Exact ref match is the only QBUF acknowledgement. Success ends the borrow;
 * mismatch/no-frame are zero-change. QBUF failure invalidates the run through
 * full cleanup and sticky TERMINAL.
 */
MeisoCameraV4l2ReleaseStatus meiso_camera_v4l2_release_frame(
    MeisoCameraV4l2Adapter *adapter,
    const MeisoCameraV4l2FrameRef *expected_frame
);

/*
 * Normal stop STREAMOFFs, invalidates an outstanding frame borrow, unmaps all
 * mappings, issues REQBUFS(count=0), and closes exactly once. Repeated STOPPED
 * is UNCHANGED. On cleanup failure the adapter is sticky TERMINAL; later stop
 * calls may retry only still-owned mappings, never close an fd twice. A failed
 * REQBUFS(count=0) is recorded before the fd is relinquished and is not retried
 * through a stale integer. Terminal cannot be restarted or cleared in place.
 */
MeisoCameraV4l2StopStatus meiso_camera_v4l2_stop(
    MeisoCameraV4l2Adapter *adapter
);

/*
 * Pure query. Failure leaves out_snapshot byte-exact unchanged. Across poll,
 * release and queries, public inputs/outputs must overlap neither adapter arena
 * nor any active MMAP range. A frame data view is the sole intentional borrow.
 */
bool meiso_camera_v4l2_snapshot(
    const MeisoCameraV4l2Adapter *adapter,
    MeisoCameraV4l2Snapshot *out_snapshot
);

#ifdef __cplusplus
}
#endif

#endif
