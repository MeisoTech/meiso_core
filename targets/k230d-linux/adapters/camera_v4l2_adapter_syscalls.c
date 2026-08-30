#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "meiso_camera_v4l2_adapter_private.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#if defined(__linux__)
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/*
 * Owns the native V4L2 translation and the callback reentrancy boundary.
 * It does not decide adapter lifecycle or accepted-frame policy.
 */

int meiso_camera_v4l2_private_error_or_io(int value)
{
    return value > 0 ? value : EIO;
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
        return meiso_camera_v4l2_private_error_or_io(saved);
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
            return meiso_camera_v4l2_private_error_or_io(saved);
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
            return meiso_camera_v4l2_private_error_or_io(saved);
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
            return meiso_camera_v4l2_private_error_or_io(saved);
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
            return meiso_camera_v4l2_private_error_or_io(saved);
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
            return meiso_camera_v4l2_private_error_or_io(saved);
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
        return meiso_camera_v4l2_private_error_or_io(saved);
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
        return meiso_camera_v4l2_private_error_or_io(errno);
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
        return meiso_camera_v4l2_private_error_or_io(errno);
    }
    return 0;
#else
    (void)fd;
    return ENOTSUP;
#endif
}

const MeisoCameraV4l2Syscalls
    meiso_camera_v4l2_private_native_syscalls = {
        native_open_device,
        native_device_ioctl,
        native_map_buffer,
        native_unmap_buffer,
        native_close_device
    };

int meiso_camera_v4l2_private_call_open(
    MeisoCameraV4l2Adapter *adapter,
    int *out_fd
)
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
    return result == 0 ? 0 :
        meiso_camera_v4l2_private_error_or_io(result);
}

int meiso_camera_v4l2_private_call_ioctl(
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
    return result == 0 ? 0 :
        meiso_camera_v4l2_private_error_or_io(result);
}

int meiso_camera_v4l2_private_call_map(
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
    return result == 0 ? 0 :
        meiso_camera_v4l2_private_error_or_io(result);
}

int meiso_camera_v4l2_private_call_unmap(
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
    return result == 0 ? 0 :
        meiso_camera_v4l2_private_error_or_io(result);
}

int meiso_camera_v4l2_private_call_close(
    MeisoCameraV4l2Adapter *adapter,
    int fd
)
{
    int result;

    adapter->callback_active = true;
    result = adapter->syscalls.close_device(
        adapter->syscall_context, fd
    );
    adapter->callback_active = false;
    return result == 0 ? 0 :
        meiso_camera_v4l2_private_error_or_io(result);
}
