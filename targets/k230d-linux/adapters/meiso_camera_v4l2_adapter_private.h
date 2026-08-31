#ifndef MEISO_TARGETS_K230D_LINUX_ADAPTERS_CAMERA_V4L2_PRIVATE_H
#define MEISO_TARGETS_K230D_LINUX_ADAPTERS_CAMERA_V4L2_PRIVATE_H

/*
 * Build-only implementation seam for the camera adapter target. This header
 * owns the opaque adapter layout and cross-TU helpers; it is never installed.
 * The caller owns the arena for the adapter lifetime. File descriptors and
 * mappings are acquired and released only by the lifecycle implementation,
 * while syscall callbacks remain non-reentrant through callback_active.
 */

#include <meiso/edge/linux/camera_v4l2.h>

#if defined(__GNUC__) || defined(__clang__)
#define MEISO_CAMERA_V4L2_PRIVATE_API \
    __attribute__((visibility("hidden")))
#else
#define MEISO_CAMERA_V4L2_PRIVATE_API
#endif

enum {
    CAMERA_V4L2_MAGIC = 0x43563241U /* CV2A */
};

typedef struct {
    void *data;
    size_t length;
    bool owned;
    bool queued;
} CameraV4l2Mapping;

struct MeisoCameraV4l2Adapter {
    size_t arena_size;
    MeisoCameraV4l2Syscalls syscalls;
    void *syscall_context;
    CameraV4l2Mapping mappings[MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT];
    MeisoCameraV4l2FrameRef borrowed_frame;
    uint64_t run_generation;
    uint64_t borrow_serial_high_water;
    uint64_t source_clock_epoch;
    uint64_t producer_dropped_frames;
    uint64_t last_timestamp_monotonic_ns;
    uint64_t mapped_bytes;
    uint32_t magic;
    uint32_t driver_buffer_count;
    uint32_t mapped_buffer_count;
    uint32_t queued_buffer_count;
    uint32_t last_driver_sequence;
    uint32_t last_sequence;
    uint32_t synthetic_sequence_high_water;
    int fd;
    int primary_error;
    int cleanup_error;
    MeisoCameraV4l2Phase phase;
    MeisoCameraV4l2FailureStage primary_failure_stage;
    MeisoCameraV4l2FailureStage cleanup_failure_stage;
    bool fd_owned;
    bool buffers_allocated;
    bool stream_on;
    bool frame_borrowed;
    bool driver_sequence_present;
    MeisoCameraV4l2SequenceProvenance last_sequence_provenance;
    MeisoCameraV4l2TimestampProvenance last_timestamp_provenance;
    bool callback_active;
    MeisoCameraV4l2DequeuedDiagnostic last_dequeue;
    char device_path[MEISO_CAMERA_V4L2_DEVICE_PATH_CAPACITY];
};

MEISO_CAMERA_V4L2_PRIVATE_API
extern const MeisoCameraV4l2Syscalls
    meiso_camera_v4l2_private_native_syscalls;

MEISO_CAMERA_V4L2_PRIVATE_API
int meiso_camera_v4l2_private_error_or_io(int value);

MEISO_CAMERA_V4L2_PRIVATE_API
bool meiso_camera_v4l2_private_ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
);

MEISO_CAMERA_V4L2_PRIVATE_API
bool meiso_camera_v4l2_private_path_length(
    const char *path,
    size_t *out_length
);

MEISO_CAMERA_V4L2_PRIVATE_API
bool meiso_camera_v4l2_private_syscall_table_valid(
    const MeisoCameraV4l2Syscalls *syscalls
);

MEISO_CAMERA_V4L2_PRIVATE_API
bool meiso_camera_v4l2_private_mapping_ranges_valid(
    const MeisoCameraV4l2Adapter *adapter
);

MEISO_CAMERA_V4L2_PRIVATE_API
bool meiso_camera_v4l2_private_adapter_valid(
    const MeisoCameraV4l2Adapter *adapter
);

MEISO_CAMERA_V4L2_PRIVATE_API
bool meiso_camera_v4l2_private_adapter_overlaps(
    const MeisoCameraV4l2Adapter *adapter,
    const void *value,
    size_t value_size
);

MEISO_CAMERA_V4L2_PRIVATE_API
bool meiso_camera_v4l2_private_mappings_overlap(
    const MeisoCameraV4l2Adapter *adapter,
    const void *value,
    size_t value_size
);

MEISO_CAMERA_V4L2_PRIVATE_API
void meiso_camera_v4l2_private_clear_borrow(
    MeisoCameraV4l2Adapter *adapter
);

MEISO_CAMERA_V4L2_PRIVATE_API
int meiso_camera_v4l2_private_call_open(
    MeisoCameraV4l2Adapter *adapter,
    int *out_fd
);

MEISO_CAMERA_V4L2_PRIVATE_API
int meiso_camera_v4l2_private_call_ioctl(
    MeisoCameraV4l2Adapter *adapter,
    MeisoCameraV4l2IoctlRequest request,
    MeisoCameraV4l2IoctlArgument *argument
);

MEISO_CAMERA_V4L2_PRIVATE_API
int meiso_camera_v4l2_private_call_map(
    MeisoCameraV4l2Adapter *adapter,
    size_t length,
    uint64_t offset,
    void **out_mapping
);

MEISO_CAMERA_V4L2_PRIVATE_API
int meiso_camera_v4l2_private_call_unmap(
    MeisoCameraV4l2Adapter *adapter,
    void *mapping,
    size_t length
);

MEISO_CAMERA_V4L2_PRIVATE_API
int meiso_camera_v4l2_private_call_close(
    MeisoCameraV4l2Adapter *adapter,
    int fd
);

MEISO_CAMERA_V4L2_PRIVATE_API
int meiso_camera_v4l2_private_call_monotonic_now(
    MeisoCameraV4l2Adapter *adapter,
    uint64_t *out_monotonic_ns
);

MEISO_CAMERA_V4L2_PRIVATE_API
void meiso_camera_v4l2_private_set_failure(
    MeisoCameraV4l2Adapter *adapter,
    MeisoCameraV4l2FailureStage stage,
    int error
);

MEISO_CAMERA_V4L2_PRIVATE_API
void meiso_camera_v4l2_private_set_cleanup_failure(
    MeisoCameraV4l2Adapter *adapter,
    MeisoCameraV4l2FailureStage stage,
    int error
);

MEISO_CAMERA_V4L2_PRIVATE_API
bool meiso_camera_v4l2_private_cleanup_resources(
    MeisoCameraV4l2Adapter *adapter
);

#endif
