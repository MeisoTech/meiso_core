#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "camera_v4l2_adapter.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__linux__)
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
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
    int last_error;
    int primary_error;
    int cleanup_error;
    MeisoCameraV4l2Phase phase;
    MeisoCameraV4l2FailureStage last_failure_stage;
    MeisoCameraV4l2FailureStage primary_failure_stage;
    MeisoCameraV4l2FailureStage cleanup_failure_stage;
    bool fd_owned;
    bool buffers_allocated;
    bool stream_on;
    bool frame_borrowed;
    bool driver_sequence_present;
    MeisoCameraV4l2SequenceProvenance last_sequence_provenance;
    bool callback_active;
    MeisoCameraV4l2DequeuedDiagnostic last_dequeue;
    char device_path[MEISO_CAMERA_V4L2_DEVICE_PATH_CAPACITY];
};

static int error_or_io(int value)
{
    return value > 0 ? value : EIO;
}

static bool ranges_overlap(
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

static bool path_length(const char *path, size_t *out_length)
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

static bool syscall_table_valid(const MeisoCameraV4l2Syscalls *syscalls)
{
    return syscalls != NULL && syscalls->open_device != NULL &&
        syscalls->device_ioctl != NULL && syscalls->map_buffer != NULL &&
        syscalls->unmap_buffer != NULL && syscalls->close_device != NULL;
}

static bool mapping_ranges_valid(const MeisoCameraV4l2Adapter *adapter)
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
             ranges_overlap(
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
                previous->owned && ranges_overlap(
                    mapping->data, mapping->length,
                    previous->data, previous->length
                )) {
                return false;
            }
        }
    }
    return true;
}

static bool adapter_valid(const MeisoCameraV4l2Adapter *adapter)
{
    size_t path_size;
    uint32_t mapped = 0U;
    uint32_t queued = 0U;
    uint64_t mapped_bytes = 0U;
    uint32_t index;

    if (adapter == NULL || adapter->magic != CAMERA_V4L2_MAGIC ||
        adapter->arena_size < sizeof(*adapter) ||
        adapter->source_clock_epoch == 0U ||
        !path_length(adapter->device_path, &path_size) ||
        !syscall_table_valid(&adapter->syscalls) ||
        (adapter->phase != MEISO_CAMERA_V4L2_PHASE_STOPPED &&
         adapter->phase != MEISO_CAMERA_V4L2_PHASE_RUNNING &&
         adapter->phase != MEISO_CAMERA_V4L2_PHASE_TERMINAL) ||
        adapter->last_sequence_provenance >
            MEISO_CAMERA_V4L2_SEQUENCE_ADAPTER_SYNTHETIC ||
        adapter->primary_failure_stage >
            MEISO_CAMERA_V4L2_FAILURE_CLOSE ||
        adapter->cleanup_failure_stage >
            MEISO_CAMERA_V4L2_FAILURE_CLOSE ||
        ((adapter->primary_failure_stage ==
              MEISO_CAMERA_V4L2_FAILURE_NONE) !=
         (adapter->primary_error == 0)) ||
        ((adapter->cleanup_failure_stage ==
              MEISO_CAMERA_V4L2_FAILURE_NONE) !=
         (adapter->cleanup_error == 0)) ||
        (adapter->primary_failure_stage !=
             MEISO_CAMERA_V4L2_FAILURE_NONE &&
         (adapter->last_failure_stage !=
              adapter->primary_failure_stage ||
          adapter->last_error != adapter->primary_error)) ||
        (adapter->primary_failure_stage ==
             MEISO_CAMERA_V4L2_FAILURE_NONE &&
         (adapter->last_failure_stage !=
              adapter->cleanup_failure_stage ||
          adapter->last_error != adapter->cleanup_error)) ||
        adapter->driver_sequence_present !=
            (adapter->last_sequence_provenance ==
             MEISO_CAMERA_V4L2_SEQUENCE_DRIVER) ||
        (adapter->last_sequence_provenance ==
             MEISO_CAMERA_V4L2_SEQUENCE_NONE &&
         adapter->last_sequence != 0U) ||
        !mapping_ranges_valid(adapter)) {
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

static bool adapter_overlaps(
    const MeisoCameraV4l2Adapter *adapter,
    const void *value,
    size_t value_size
)
{
    return ranges_overlap(
        adapter, adapter->arena_size, value, value_size
    );
}

static bool mappings_overlap(
    const MeisoCameraV4l2Adapter *adapter,
    const void *value,
    size_t value_size
)
{
    uint32_t index;

    for (index = 0U;
         index < MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT;
         ++index) {
        if (adapter->mappings[index].owned && ranges_overlap(
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

static void clear_borrow(MeisoCameraV4l2Adapter *adapter)
{
    adapter->frame_borrowed = false;
    memset(&adapter->borrowed_frame, 0, sizeof(adapter->borrowed_frame));
}

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

#if defined(__linux__)
static bool native_pixel_format(
    MeisoCompositionMediaPixelFormat format,
    uint32_t *out_format
)
{
    if (format != MEISO_COMPOSITION_MEDIA_PIXEL_NV12 ||
        out_format == NULL) {
        return false;
    }
    *out_format = V4L2_PIX_FMT_NV12;
    return true;
}

static bool portable_pixel_format(
    uint32_t format,
    MeisoCompositionMediaPixelFormat *out_format
)
{
    if (format != V4L2_PIX_FMT_NV12 || out_format == NULL) {
        return false;
    }
    *out_format = MEISO_COMPOSITION_MEDIA_PIXEL_NV12;
    return true;
}
#endif

static int native_open_device(
    void *context,
    const char *path,
    int flags,
    int *out_fd
)
{
    (void)context;
#if defined(__linux__)
    int candidate;
    int saved;
    if (path == NULL || out_fd == NULL) {
        return EINVAL;
    }
    candidate = open(path, flags);
    if (candidate < 0) {
        saved = errno;
        return error_or_io(saved);
    }
    *out_fd = candidate;
    return 0;
#else
    (void)path;
    (void)flags;
    (void)out_fd;
    return ENOTSUP;
#endif
}

static int native_device_ioctl(
    void *context,
    int fd,
    MeisoCameraV4l2IoctlRequest request,
    MeisoCameraV4l2IoctlArgument *ioctl_arg
)
{
    (void)context;
#if defined(__linux__)
    MeisoCameraV4l2IoctlArgument candidate;
    int result;
    int saved;

    if (fd < 0 || ioctl_arg == NULL) {
        return EINVAL;
    }
    candidate = *ioctl_arg;
    if (request == MEISO_CAMERA_V4L2_IOCTL_QUERY_CAPABILITY) {
        struct v4l2_capability capability;
        uint32_t flags;
        memset(&capability, 0, sizeof(capability));
        result = ioctl(fd, VIDIOC_QUERYCAP, &capability);
        if (result != 0) {
            saved = errno;
            return error_or_io(saved);
        }
        flags = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U ?
            capability.device_caps : capability.capabilities;
        candidate.capability.video_capture =
            (flags & V4L2_CAP_VIDEO_CAPTURE) != 0U;
        candidate.capability.streaming =
            (flags & V4L2_CAP_STREAMING) != 0U;
    } else if (request == MEISO_CAMERA_V4L2_IOCTL_SET_FORMAT ||
               request == MEISO_CAMERA_V4L2_IOCTL_GET_FORMAT) {
        struct v4l2_format format;
        uint32_t pixel_format;
        memset(&format, 0, sizeof(format));
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (request == MEISO_CAMERA_V4L2_IOCTL_SET_FORMAT) {
            if (!native_pixel_format(
                    candidate.format.pixel_format, &pixel_format
                ) || !candidate.format.progressive) {
                return EINVAL;
            }
            format.fmt.pix.width = candidate.format.width;
            format.fmt.pix.height = candidate.format.height;
            format.fmt.pix.pixelformat = pixel_format;
            format.fmt.pix.field = V4L2_FIELD_NONE;
            format.fmt.pix.bytesperline = candidate.format.bytes_per_line;
            format.fmt.pix.sizeimage = candidate.format.size_image;
            result = ioctl(fd, VIDIOC_S_FMT, &format);
        } else {
            result = ioctl(fd, VIDIOC_G_FMT, &format);
        }
        if (result != 0) {
            saved = errno;
            return error_or_io(saved);
        }
        if (format.fmt.pix.width > UINT16_MAX ||
            format.fmt.pix.height > UINT16_MAX) {
            return ENOTSUP;
        }
        if (!portable_pixel_format(
                format.fmt.pix.pixelformat,
                &candidate.format.pixel_format
            )) {
            return ENOTSUP;
        }
        candidate.format.width = (uint16_t)format.fmt.pix.width;
        candidate.format.height = (uint16_t)format.fmt.pix.height;
        candidate.format.progressive = format.fmt.pix.field == V4L2_FIELD_NONE;
        candidate.format.bytes_per_line = format.fmt.pix.bytesperline;
        candidate.format.size_image = format.fmt.pix.sizeimage;
    } else if (request == MEISO_CAMERA_V4L2_IOCTL_REQUEST_BUFFERS) {
        struct v4l2_requestbuffers buffers;
        memset(&buffers, 0, sizeof(buffers));
        buffers.count = candidate.request_buffers.count;
        buffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffers.memory = V4L2_MEMORY_MMAP;
        result = ioctl(fd, VIDIOC_REQBUFS, &buffers);
        if (result != 0) {
            saved = errno;
            return error_or_io(saved);
        }
        candidate.request_buffers.count = buffers.count;
    } else if (request == MEISO_CAMERA_V4L2_IOCTL_QUERY_BUFFER ||
               request == MEISO_CAMERA_V4L2_IOCTL_QUEUE_BUFFER ||
               request == MEISO_CAMERA_V4L2_IOCTL_DEQUEUE_BUFFER) {
        struct v4l2_buffer buffer;
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (request != MEISO_CAMERA_V4L2_IOCTL_DEQUEUE_BUFFER) {
            buffer.index = candidate.buffer.index;
        }
        if (request == MEISO_CAMERA_V4L2_IOCTL_QUERY_BUFFER) {
            result = ioctl(fd, VIDIOC_QUERYBUF, &buffer);
        } else if (request == MEISO_CAMERA_V4L2_IOCTL_QUEUE_BUFFER) {
            result = ioctl(fd, VIDIOC_QBUF, &buffer);
        } else {
            result = ioctl(fd, VIDIOC_DQBUF, &buffer);
        }
        if (result != 0) {
            saved = errno;
            return error_or_io(saved);
        }
        if (request == MEISO_CAMERA_V4L2_IOCTL_QUERY_BUFFER) {
            candidate.buffer.index = buffer.index;
            candidate.buffer.length = buffer.length;
            candidate.buffer.memory_offset = buffer.m.offset;
        } else if (request == MEISO_CAMERA_V4L2_IOCTL_DEQUEUE_BUFFER) {
            candidate.buffer.index = buffer.index;
            candidate.buffer.length = buffer.length;
            candidate.buffer.bytes_used = buffer.bytesused;
            candidate.buffer.driver_sequence = buffer.sequence;
            /*
             * The pinned K230 vvcam driver uses a private buffer-index field
             * for ISP matching but never publishes a frame sequence through
             * vb2_v4l2_buffer.sequence. Treat public zero as unavailable;
             * the adapter supplies its own checked accepted-frame sequence.
             */
            candidate.buffer.driver_sequence_present = false;
            candidate.buffer.timestamp_seconds = buffer.timestamp.tv_sec;
            candidate.buffer.timestamp_microseconds =
                (int32_t)buffer.timestamp.tv_usec;
            candidate.buffer.timestamp_monotonic =
                (buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
                V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
            candidate.buffer.raw_field = buffer.field;
            if (buffer.field == V4L2_FIELD_NONE) {
                candidate.buffer.field_provenance =
                    MEISO_CAMERA_V4L2_FIELD_EXPLICIT_PROGRESSIVE;
            } else if (buffer.field == V4L2_FIELD_ANY) {
                candidate.buffer.field_provenance =
                    MEISO_CAMERA_V4L2_FIELD_FORMAT_INHERITED_PROGRESSIVE;
            } else {
                candidate.buffer.field_provenance =
                    MEISO_CAMERA_V4L2_FIELD_UNKNOWN;
            }
            candidate.buffer.frame_error =
                (buffer.flags & V4L2_BUF_FLAG_ERROR) != 0U;
        }
    } else if (request == MEISO_CAMERA_V4L2_IOCTL_STREAM_ON ||
               request == MEISO_CAMERA_V4L2_IOCTL_STREAM_OFF) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        result = ioctl(
            fd,
            request == MEISO_CAMERA_V4L2_IOCTL_STREAM_ON ?
                VIDIOC_STREAMON : VIDIOC_STREAMOFF,
            &type
        );
        if (result != 0) {
            saved = errno;
            return error_or_io(saved);
        }
    } else {
        return EINVAL;
    }
    *ioctl_arg = candidate;
    return 0;
#else
    (void)fd;
    (void)request;
    (void)ioctl_arg;
    return ENOTSUP;
#endif
}

static int native_map_buffer(
    void *context,
    int fd,
    size_t length,
    uint64_t offset,
    void **out_mapping
)
{
    (void)context;
#if defined(__linux__)
    void *candidate;
    int saved;
    if (fd < 0 || length == 0U || out_mapping == NULL ||
        offset > (uint64_t)INT64_MAX) {
        return EINVAL;
    }
    candidate = mmap(
        NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)offset
    );
    if (candidate == MAP_FAILED) {
        saved = errno;
        return error_or_io(saved);
    }
    *out_mapping = candidate;
    return 0;
#else
    (void)fd;
    (void)length;
    (void)offset;
    (void)out_mapping;
    return ENOTSUP;
#endif
}

static int native_unmap_buffer(
    void *context,
    void *mapping,
    size_t length
)
{
    (void)context;
#if defined(__linux__)
    if (mapping == NULL || length == 0U) {
        return EINVAL;
    }
    if (munmap(mapping, length) != 0) {
        return error_or_io(errno);
    }
    return 0;
#else
    (void)mapping;
    (void)length;
    return ENOTSUP;
#endif
}

static int native_close_device(void *context, int fd)
{
    (void)context;
#if defined(__linux__)
    if (fd < 0) {
        return EINVAL;
    }
    if (close(fd) != 0) {
        return error_or_io(errno);
    }
    return 0;
#else
    (void)fd;
    return ENOTSUP;
#endif
}

static const MeisoCameraV4l2Syscalls NATIVE_SYSCALLS = {
    native_open_device,
    native_device_ioctl,
    native_map_buffer,
    native_unmap_buffer,
    native_close_device
};

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
    if (!path_length(config->device_path, &path_size) ||
        config->source_clock_epoch == 0U ||
        (config->syscalls == NULL && config->syscall_context != NULL) ||
        (config->syscalls != NULL &&
         !syscall_table_valid(config->syscalls))) {
        return MEISO_CAMERA_V4L2_INIT_INVALID_CONFIG;
    }
    selected = config->syscalls == NULL ? &NATIVE_SYSCALLS : config->syscalls;
    if ((uintptr_t)arena % meiso_camera_v4l2_alignment_required() != 0U) {
        return MEISO_CAMERA_V4L2_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < meiso_camera_v4l2_memory_required()) {
        return MEISO_CAMERA_V4L2_INIT_ARENA_TOO_SMALL;
    }
    if (ranges_overlap(arena, arena_size, config, sizeof(*config)) ||
        ranges_overlap(
            arena, arena_size, config->device_path, path_size + 1U
        ) || ranges_overlap(
            arena, arena_size, config->syscalls,
            config->syscalls == NULL ? 0U : sizeof(*config->syscalls)
        ) || ranges_overlap(
            arena, arena_size, config->syscall_context,
            config->syscall_context == NULL ? 0U : 1U
        ) || ranges_overlap(
            arena, arena_size, out_adapter, sizeof(*out_adapter)
        ) || ranges_overlap(
            config, sizeof(*config), config->device_path, path_size + 1U
        ) || ranges_overlap(
            config, sizeof(*config), config->syscalls,
            config->syscalls == NULL ? 0U : sizeof(*config->syscalls)
        ) || ranges_overlap(
            config->device_path, path_size + 1U, config->syscalls,
            config->syscalls == NULL ? 0U : sizeof(*config->syscalls)
        ) || ranges_overlap(
            config, sizeof(*config), out_adapter, sizeof(*out_adapter)
        ) || ranges_overlap(
            config->device_path, path_size + 1U,
            out_adapter, sizeof(*out_adapter)
        ) || ranges_overlap(
            config->syscalls,
            config->syscalls == NULL ? 0U : sizeof(*config->syscalls),
            out_adapter, sizeof(*out_adapter)
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

static int call_open(MeisoCameraV4l2Adapter *adapter, int *out_fd)
{
    int result;

    adapter->callback_active = true;
    result = adapter->syscalls.open_device(
        adapter->syscall_context,
        adapter->device_path,
        O_RDWR | O_NONBLOCK | O_CLOEXEC,
        out_fd
    );
    adapter->callback_active = false;
    return result == 0 ? 0 : error_or_io(result);
}

static int call_ioctl(
    MeisoCameraV4l2Adapter *adapter,
    MeisoCameraV4l2IoctlRequest request,
    MeisoCameraV4l2IoctlArgument *argument
)
{
    int result;

    adapter->callback_active = true;
    result = adapter->syscalls.device_ioctl(
        adapter->syscall_context, adapter->fd, request, argument
    );
    adapter->callback_active = false;
    return result == 0 ? 0 : error_or_io(result);
}

static int call_map(
    MeisoCameraV4l2Adapter *adapter,
    size_t length,
    uint64_t offset,
    void **out_mapping
)
{
    int result;

    adapter->callback_active = true;
    result = adapter->syscalls.map_buffer(
        adapter->syscall_context,
        adapter->fd,
        length,
        offset,
        out_mapping
    );
    adapter->callback_active = false;
    return result == 0 ? 0 : error_or_io(result);
}

static int call_unmap(
    MeisoCameraV4l2Adapter *adapter,
    void *mapping,
    size_t length
)
{
    int result;

    adapter->callback_active = true;
    result = adapter->syscalls.unmap_buffer(
        adapter->syscall_context, mapping, length
    );
    adapter->callback_active = false;
    return result == 0 ? 0 : error_or_io(result);
}

static int call_close(MeisoCameraV4l2Adapter *adapter, int fd)
{
    int result;

    adapter->callback_active = true;
    result = adapter->syscalls.close_device(
        adapter->syscall_context, fd
    );
    adapter->callback_active = false;
    return result == 0 ? 0 : error_or_io(result);
}

static void set_failure(
    MeisoCameraV4l2Adapter *adapter,
    MeisoCameraV4l2FailureStage stage,
    int error
)
{
    adapter->primary_failure_stage = stage;
    adapter->primary_error = error_or_io(error);
    adapter->last_failure_stage = stage;
    adapter->last_error = adapter->primary_error;
}

static void set_cleanup_failure(
    MeisoCameraV4l2Adapter *adapter,
    MeisoCameraV4l2FailureStage stage,
    int error
)
{
    if (adapter->cleanup_failure_stage ==
        MEISO_CAMERA_V4L2_FAILURE_NONE) {
        adapter->cleanup_failure_stage = stage;
        adapter->cleanup_error = error_or_io(error);
    }
    if (adapter->primary_failure_stage ==
        MEISO_CAMERA_V4L2_FAILURE_NONE) {
        adapter->last_failure_stage = stage;
        adapter->last_error = error_or_io(error);
    }
}

static bool cleanup_resources(MeisoCameraV4l2Adapter *adapter)
{
    bool okay = true;
    bool failure_recorded = false;
    bool stream_off_failed = false;
    uint32_t index;
    int result;

    clear_borrow(adapter);
    if (adapter->stream_on && adapter->fd_owned) {
        MeisoCameraV4l2IoctlArgument argument;
        memset(&argument, 0, sizeof(argument));
        result = call_ioctl(
            adapter, MEISO_CAMERA_V4L2_IOCTL_STREAM_OFF, &argument
        );
        if (result != 0) {
            set_cleanup_failure(
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
            result = call_close(adapter, fd);
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
        result = call_unmap(adapter, mapping->data, mapping->length);
        if (result == 0) {
            clear_mapping(adapter, mapping_index);
        } else {
            if (!failure_recorded) {
                set_cleanup_failure(
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
        result = call_ioctl(
            adapter, MEISO_CAMERA_V4L2_IOCTL_REQUEST_BUFFERS, &argument
        );
        if (result != 0) {
            if (!failure_recorded) {
                set_cleanup_failure(
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
        result = call_close(adapter, fd);
        if (result != 0) {
            if (!failure_recorded) {
                set_cleanup_failure(
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
    set_failure(adapter, stage, error);
    if (!cleanup_resources(adapter)) {
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

    if (!adapter_valid(adapter) || adapter->callback_active) {
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

    result = call_open(adapter, &candidate_fd);
    if (result != 0 || candidate_fd < 0) {
        set_failure(
            adapter,
            MEISO_CAMERA_V4L2_FAILURE_OPEN,
            result != 0 ? result : EIO
        );
        return MEISO_CAMERA_V4L2_START_OPEN_FAILED;
    }
    adapter->fd = candidate_fd;
    adapter->fd_owned = true;

    memset(&argument, 0, sizeof(argument));
    result = call_ioctl(
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
    result = call_ioctl(
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
    result = call_ioctl(
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
    result = call_ioctl(
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
        result = call_ioctl(
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
        result = call_map(
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
        if (!mapping_ranges_valid(adapter)) {
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
        result = call_ioctl(
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
    result = call_ioctl(
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
    memset(&adapter->last_dequeue, 0, sizeof(adapter->last_dequeue));
    adapter->last_failure_stage = MEISO_CAMERA_V4L2_FAILURE_NONE;
    adapter->last_error = 0;
    adapter->primary_failure_stage = MEISO_CAMERA_V4L2_FAILURE_NONE;
    adapter->primary_error = 0;
    adapter->cleanup_failure_stage = MEISO_CAMERA_V4L2_FAILURE_NONE;
    adapter->cleanup_error = 0;
    adapter->phase = MEISO_CAMERA_V4L2_PHASE_RUNNING;
    return MEISO_CAMERA_V4L2_START_STARTED;
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
    set_failure(adapter, stage, error);
    (void)cleanup_resources(adapter);
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

    if (!adapter_valid(adapter) || adapter->callback_active ||
        out_frame == NULL || adapter_overlaps(
            adapter, out_frame, sizeof(*out_frame)
        ) || mappings_overlap(adapter, out_frame, sizeof(*out_frame))) {
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
    result = call_ioctl(
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
                adapter, MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT, EOVERFLOW
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

    if (!adapter_valid(adapter) || adapter->callback_active ||
        expected_frame == NULL || adapter_overlaps(
            adapter, expected_frame, sizeof(*expected_frame)
        ) || mappings_overlap(
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
    result = call_ioctl(
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
    clear_borrow(adapter);
    return MEISO_CAMERA_V4L2_RELEASE_RELEASED;
}

MeisoCameraV4l2StopStatus meiso_camera_v4l2_stop(
    MeisoCameraV4l2Adapter *adapter
)
{
    bool terminal;

    if (!adapter_valid(adapter) || adapter->callback_active) {
        return MEISO_CAMERA_V4L2_STOP_INVALID_ARGUMENT;
    }
    terminal = adapter->phase == MEISO_CAMERA_V4L2_PHASE_TERMINAL;
    if (adapter->phase == MEISO_CAMERA_V4L2_PHASE_STOPPED) {
        return MEISO_CAMERA_V4L2_STOP_UNCHANGED;
    }
    if (!cleanup_resources(adapter)) {
        adapter->phase = MEISO_CAMERA_V4L2_PHASE_TERMINAL;
        return MEISO_CAMERA_V4L2_STOP_TERMINAL;
    }
    if (terminal) {
        adapter->phase = MEISO_CAMERA_V4L2_PHASE_TERMINAL;
        return MEISO_CAMERA_V4L2_STOP_TERMINAL;
    }
    adapter->phase = MEISO_CAMERA_V4L2_PHASE_STOPPED;
    adapter->last_failure_stage = MEISO_CAMERA_V4L2_FAILURE_NONE;
    adapter->last_error = 0;
    adapter->primary_failure_stage = MEISO_CAMERA_V4L2_FAILURE_NONE;
    adapter->primary_error = 0;
    adapter->cleanup_failure_stage = MEISO_CAMERA_V4L2_FAILURE_NONE;
    adapter->cleanup_error = 0;
    return MEISO_CAMERA_V4L2_STOP_STOPPED;
}

bool meiso_camera_v4l2_snapshot(
    const MeisoCameraV4l2Adapter *adapter,
    MeisoCameraV4l2Snapshot *out_snapshot
)
{
    MeisoCameraV4l2Snapshot value;

    if (!adapter_valid(adapter) || adapter->callback_active ||
        out_snapshot == NULL || adapter_overlaps(
            adapter, out_snapshot, sizeof(*out_snapshot)
        ) || mappings_overlap(
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
    value.last_dequeue = adapter->last_dequeue;
    value.primary_failure_stage = adapter->primary_failure_stage;
    value.primary_error = adapter->primary_error;
    value.cleanup_failure_stage = adapter->cleanup_failure_stage;
    value.cleanup_error = adapter->cleanup_error;
    value.last_failure_stage = adapter->last_failure_stage;
    value.last_error = adapter->last_error;
    value.arena_bytes = adapter->arena_size;
    value.mapped_bytes = adapter->mapped_bytes;
    *out_snapshot = value;
    return true;
}
