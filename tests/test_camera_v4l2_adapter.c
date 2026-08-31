#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <meiso/edge/linux/camera_v4l2.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(                                                      \
                stderr, "CHECK failed at %s:%d: %s\n",                       \
                __FILE__, __LINE__, #condition                                  \
            );                                                                  \
            return false;                                                       \
        }                                                                       \
    } while (0)

enum { FAKE_FD = 41 };
#define FAKE_NO_INDEX UINT32_MAX
#define TEST_ARENA_SNAPSHOT_CAPACITY 4096U

enum {
    CLEANUP_STREAM_OFF = 1U,
    CLEANUP_REQUEST_ZERO = 2U,
    CLEANUP_CLOSE = 3U,
    CLEANUP_UNMAP_BASE = 10U
};

typedef struct {
    uint8_t *storage;
    size_t storage_size;
    bool capture;
    bool streaming;
    MeisoCameraV4l2FormatIoctl format;
    uint32_t returned_buffer_count;
    int open_error;
    MeisoCameraV4l2IoctlRequest ioctl_error_request;
    uint32_t ioctl_error_index;
    int ioctl_error;
    uint32_t map_error_index;
    int map_error;
    uint32_t mapping_override_index;
    void *mapping_override;
    uint32_t unmap_error_index;
    int unmap_error;
    bool unmap_error_once;
    int close_error;
    int release_buffers_error;
    bool duplicate_mapping;
    int dequeue_error;
    MeisoCameraV4l2BufferIoctl dequeue;
    unsigned open_calls;
    unsigned close_calls;
    unsigned ioctl_calls[10];
    unsigned map_calls;
    unsigned unmap_calls[MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT];
    uint32_t unmap_order[8];
    unsigned unmap_order_count;
    uint32_t cleanup_order[16];
    unsigned cleanup_order_count;
    unsigned qbuf_calls;
    unsigned stream_on_calls;
    unsigned stream_off_calls;
    unsigned request_four_calls;
    unsigned request_zero_calls;
    int opened_flags;
    char opened_path[MEISO_CAMERA_V4L2_DEVICE_PATH_CAPACITY];
    MeisoCameraV4l2FormatIoctl set_format;
    MeisoCameraV4l2Adapter *reentry_adapter;
    bool reenter_on_open;
    MeisoCameraV4l2StartStatus reentrant_start_status;
    bool reentrant_snapshot_failed;
    uint64_t monotonic_now_ns;
    uint64_t monotonic_step_ns;
    int monotonic_error;
    unsigned monotonic_calls;
    bool reenter_on_monotonic;
    bool monotonic_reentrant_snapshot_failed;
} FakeKernel;

typedef struct {
    void *arena;
    size_t arena_size;
    MeisoCameraV4l2Adapter *adapter;
    FakeKernel kernel;
    MeisoCameraV4l2Syscalls syscalls;
    MeisoCameraV4l2Config config;
    char path[32];
} Fixture;

static void fake_record_cleanup(FakeKernel *kernel, uint32_t operation)
{
    if (kernel->cleanup_order_count <
        sizeof(kernel->cleanup_order) / sizeof(kernel->cleanup_order[0])) {
        kernel->cleanup_order[kernel->cleanup_order_count++] = operation;
    }
}

static void fake_kernel_defaults(FakeKernel *kernel)
{
    memset(kernel, 0, sizeof(*kernel));
    kernel->capture = true;
    kernel->streaming = true;
    kernel->format.width = MEISO_CAMERA_V4L2_WIDTH;
    kernel->format.height = MEISO_CAMERA_V4L2_HEIGHT;
    kernel->format.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_NV12;
    kernel->format.progressive = true;
    kernel->format.bytes_per_line = MEISO_CAMERA_V4L2_BYTES_PER_LINE;
    kernel->format.size_image = MEISO_CAMERA_V4L2_SIZE_IMAGE;
    kernel->returned_buffer_count = MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT;
    kernel->ioctl_error_index = FAKE_NO_INDEX;
    kernel->map_error_index = FAKE_NO_INDEX;
    kernel->mapping_override_index = FAKE_NO_INDEX;
    kernel->unmap_error_index = FAKE_NO_INDEX;
    kernel->dequeue.index = 0U;
    kernel->dequeue.length = MEISO_CAMERA_V4L2_SIZE_IMAGE;
    kernel->dequeue.bytes_used = MEISO_CAMERA_V4L2_SIZE_IMAGE;
    kernel->dequeue.driver_sequence = 1U;
    kernel->dequeue.driver_sequence_present = true;
    kernel->dequeue.timestamp_seconds = 10;
    kernel->dequeue.timestamp_microseconds = 20;
    kernel->dequeue.timestamp_monotonic = true;
    kernel->dequeue.raw_field = MEISO_CAMERA_V4L2_RAW_FIELD_NONE;
    kernel->dequeue.field_provenance =
        MEISO_CAMERA_V4L2_FIELD_EXPLICIT_PROGRESSIVE;
    kernel->monotonic_now_ns = UINT64_C(30000000000);
    kernel->monotonic_step_ns = 1U;
}

static void fake_clear_calls(FakeKernel *kernel)
{
    kernel->open_calls = 0U;
    kernel->close_calls = 0U;
    memset(kernel->ioctl_calls, 0, sizeof(kernel->ioctl_calls));
    kernel->map_calls = 0U;
    memset(kernel->unmap_calls, 0, sizeof(kernel->unmap_calls));
    memset(kernel->unmap_order, 0, sizeof(kernel->unmap_order));
    kernel->unmap_order_count = 0U;
    memset(kernel->cleanup_order, 0, sizeof(kernel->cleanup_order));
    kernel->cleanup_order_count = 0U;
    kernel->qbuf_calls = 0U;
    kernel->stream_on_calls = 0U;
    kernel->stream_off_calls = 0U;
    kernel->request_four_calls = 0U;
    kernel->request_zero_calls = 0U;
    kernel->monotonic_calls = 0U;
    kernel->opened_flags = 0;
    memset(kernel->opened_path, 0, sizeof(kernel->opened_path));
    memset(&kernel->set_format, 0, sizeof(kernel->set_format));
}

static int fake_open(
    void *context,
    const char *path,
    int flags,
    int *out_fd
)
{
    FakeKernel *kernel = (FakeKernel *)context;

    ++kernel->open_calls;
    kernel->opened_flags = flags;
    (void)snprintf(kernel->opened_path, sizeof(kernel->opened_path), "%s", path);
    if (kernel->reenter_on_open && kernel->reentry_adapter != NULL) {
        MeisoCameraV4l2Snapshot snapshot;
        kernel->reentrant_start_status =
            meiso_camera_v4l2_start(kernel->reentry_adapter);
        kernel->reentrant_snapshot_failed = !meiso_camera_v4l2_snapshot(
            kernel->reentry_adapter, &snapshot
        );
    }
    if (kernel->open_error != 0) {
        return kernel->open_error;
    }
    *out_fd = FAKE_FD;
    return 0;
}

static bool fake_ioctl_should_fail(
    const FakeKernel *kernel,
    MeisoCameraV4l2IoctlRequest request,
    const MeisoCameraV4l2IoctlArgument *argument
)
{
    if (kernel->ioctl_error == 0 ||
        kernel->ioctl_error_request != request) {
        return false;
    }
    if (kernel->ioctl_error_index == FAKE_NO_INDEX) {
        return true;
    }
    return (request == MEISO_CAMERA_V4L2_IOCTL_QUERY_BUFFER ||
            request == MEISO_CAMERA_V4L2_IOCTL_QUEUE_BUFFER) &&
        argument->buffer.index == kernel->ioctl_error_index;
}

static int fake_ioctl(
    void *context,
    int fd,
    MeisoCameraV4l2IoctlRequest request,
    MeisoCameraV4l2IoctlArgument *argument
)
{
    FakeKernel *kernel = (FakeKernel *)context;

    if (fd != FAKE_FD || request == 0U || request >= 10U) {
        return EINVAL;
    }
    ++kernel->ioctl_calls[request];
    if (request == MEISO_CAMERA_V4L2_IOCTL_STREAM_OFF) {
        fake_record_cleanup(kernel, CLEANUP_STREAM_OFF);
    }
    if (request == MEISO_CAMERA_V4L2_IOCTL_DEQUEUE_BUFFER &&
        kernel->dequeue_error != 0) {
        return kernel->dequeue_error;
    }
    if (fake_ioctl_should_fail(kernel, request, argument)) {
        return kernel->ioctl_error;
    }
    switch (request) {
        case MEISO_CAMERA_V4L2_IOCTL_QUERY_CAPABILITY:
            argument->capability.video_capture = kernel->capture;
            argument->capability.streaming = kernel->streaming;
            break;
        case MEISO_CAMERA_V4L2_IOCTL_SET_FORMAT:
            kernel->set_format = argument->format;
            break;
        case MEISO_CAMERA_V4L2_IOCTL_GET_FORMAT:
            argument->format = kernel->format;
            break;
        case MEISO_CAMERA_V4L2_IOCTL_REQUEST_BUFFERS:
            if (argument->request_buffers.count == 0U) {
                ++kernel->request_zero_calls;
                fake_record_cleanup(kernel, CLEANUP_REQUEST_ZERO);
                if (kernel->release_buffers_error != 0) {
                    return kernel->release_buffers_error;
                }
            } else {
                ++kernel->request_four_calls;
                argument->request_buffers.count =
                    kernel->returned_buffer_count;
            }
            break;
        case MEISO_CAMERA_V4L2_IOCTL_QUERY_BUFFER:
            argument->buffer.length = MEISO_CAMERA_V4L2_SIZE_IMAGE;
            argument->buffer.memory_offset =
                (uint64_t)argument->buffer.index *
                MEISO_CAMERA_V4L2_SIZE_IMAGE;
            break;
        case MEISO_CAMERA_V4L2_IOCTL_QUEUE_BUFFER:
            ++kernel->qbuf_calls;
            break;
        case MEISO_CAMERA_V4L2_IOCTL_STREAM_ON:
            ++kernel->stream_on_calls;
            break;
        case MEISO_CAMERA_V4L2_IOCTL_DEQUEUE_BUFFER:
            argument->buffer = kernel->dequeue;
            break;
        case MEISO_CAMERA_V4L2_IOCTL_STREAM_OFF:
            ++kernel->stream_off_calls;
            break;
        default:
            return EINVAL;
    }
    return 0;
}

static int fake_map(
    void *context,
    int fd,
    size_t length,
    uint64_t offset,
    void **out_mapping
)
{
    FakeKernel *kernel = (FakeKernel *)context;
    const uint32_t index = (uint32_t)(offset / MEISO_CAMERA_V4L2_SIZE_IMAGE);

    if (fd != FAKE_FD || length != MEISO_CAMERA_V4L2_SIZE_IMAGE ||
        index >= MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT ||
        offset != (uint64_t)index * MEISO_CAMERA_V4L2_SIZE_IMAGE) {
        return EINVAL;
    }
    ++kernel->map_calls;
    if (kernel->map_error != 0 && index == kernel->map_error_index) {
        return kernel->map_error;
    }
    if (index == kernel->mapping_override_index) {
        *out_mapping = kernel->mapping_override;
    } else {
        *out_mapping = kernel->storage +
            (kernel->duplicate_mapping && index == 1U ? 0U : (size_t)index) *
                MEISO_CAMERA_V4L2_SIZE_IMAGE;
    }
    return 0;
}

static int fake_unmap(void *context, void *mapping, size_t length)
{
    FakeKernel *kernel = (FakeKernel *)context;
    const uintptr_t mapping_value = (uintptr_t)mapping;
    const uintptr_t storage_value = (uintptr_t)kernel->storage;
    uint32_t index;

    if (length != MEISO_CAMERA_V4L2_SIZE_IMAGE) {
        return EINVAL;
    }
    if (mapping == kernel->mapping_override &&
        kernel->mapping_override_index != FAKE_NO_INDEX) {
        index = kernel->mapping_override_index;
    } else if (mapping_value >= storage_value &&
               mapping_value - storage_value < kernel->storage_size &&
               (mapping_value - storage_value) %
                   MEISO_CAMERA_V4L2_SIZE_IMAGE == 0U) {
        index = (uint32_t)((mapping_value - storage_value) /
            MEISO_CAMERA_V4L2_SIZE_IMAGE);
    } else {
        return EINVAL;
    }
    if (index >= MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT) {
        return EINVAL;
    }
    ++kernel->unmap_calls[index];
    if (kernel->unmap_order_count <
        sizeof(kernel->unmap_order) / sizeof(kernel->unmap_order[0])) {
        kernel->unmap_order[kernel->unmap_order_count++] = index;
    }
    fake_record_cleanup(kernel, CLEANUP_UNMAP_BASE + index);
    if (kernel->unmap_error != 0 && index == kernel->unmap_error_index) {
        const int result = kernel->unmap_error;
        if (kernel->unmap_error_once) {
            kernel->unmap_error = 0;
        }
        return result;
    }
    return 0;
}

static int fake_close(void *context, int fd)
{
    FakeKernel *kernel = (FakeKernel *)context;

    if (fd != FAKE_FD) {
        return EINVAL;
    }
    ++kernel->close_calls;
    fake_record_cleanup(kernel, CLEANUP_CLOSE);
    return kernel->close_error;
}

static int fake_monotonic_now(void *context, uint64_t *out_monotonic_ns)
{
    FakeKernel *kernel = (FakeKernel *)context;

    ++kernel->monotonic_calls;
    if (kernel->reenter_on_monotonic && kernel->reentry_adapter != NULL) {
        MeisoCameraV4l2Snapshot snapshot;
        kernel->monotonic_reentrant_snapshot_failed =
            !meiso_camera_v4l2_snapshot(
                kernel->reentry_adapter, &snapshot
            );
    }
    if (kernel->monotonic_error != 0) {
        return kernel->monotonic_error;
    }
    if (out_monotonic_ns == NULL) {
        return EINVAL;
    }
    *out_monotonic_ns = kernel->monotonic_now_ns;
    if (kernel->monotonic_step_ns <=
        UINT64_MAX - kernel->monotonic_now_ns) {
        kernel->monotonic_now_ns += kernel->monotonic_step_ns;
    }
    return 0;
}

static void fixture_destroy(Fixture *fixture)
{
    free(fixture->kernel.storage);
    free(fixture->arena);
    memset(fixture, 0, sizeof(*fixture));
}

static bool fixture_init(Fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fake_kernel_defaults(&fixture->kernel);
    fixture->kernel.storage_size =
        (size_t)MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT *
        MEISO_CAMERA_V4L2_SIZE_IMAGE;
    fixture->kernel.storage =
        (uint8_t *)calloc(1U, fixture->kernel.storage_size);
    CHECK(fixture->kernel.storage != NULL);
    fixture->syscalls = (MeisoCameraV4l2Syscalls){
        fake_open, fake_ioctl, fake_map, fake_unmap, fake_close,
        fake_monotonic_now
    };
    (void)snprintf(fixture->path, sizeof(fixture->path), "%s", "/dev/fake0");
    fixture->config.device_path = fixture->path;
    fixture->config.source_clock_epoch = 77U;
    fixture->config.syscalls = &fixture->syscalls;
    fixture->config.syscall_context = &fixture->kernel;
    fixture->arena_size = meiso_camera_v4l2_memory_required();
    CHECK(fixture->arena_size != 0U);
    fixture->arena = calloc(1U, fixture->arena_size);
    CHECK(fixture->arena != NULL);
    CHECK(meiso_camera_v4l2_init(
        fixture->arena, fixture->arena_size, &fixture->config,
        &fixture->adapter
    ) == MEISO_CAMERA_V4L2_INIT_OK);
    fixture->kernel.reentry_adapter = fixture->adapter;
    return true;
}

static bool start_and_check(Fixture *fixture)
{
    MeisoCameraV4l2Snapshot snapshot;
    const int expected_open_flags = O_RDWR | O_NONBLOCK | O_CLOEXEC;

    CHECK(meiso_camera_v4l2_start(fixture->adapter) ==
          MEISO_CAMERA_V4L2_START_STARTED);
    CHECK(fixture->kernel.open_calls == 1U &&
          strcmp(fixture->kernel.opened_path, fixture->path) == 0 &&
          fixture->kernel.opened_flags == expected_open_flags &&
          fixture->kernel.set_format.width == MEISO_CAMERA_V4L2_WIDTH &&
          fixture->kernel.set_format.height == MEISO_CAMERA_V4L2_HEIGHT &&
          fixture->kernel.set_format.pixel_format ==
              MEISO_COMPOSITION_MEDIA_PIXEL_NV12 &&
          fixture->kernel.set_format.progressive &&
          fixture->kernel.set_format.bytes_per_line ==
              MEISO_CAMERA_V4L2_BYTES_PER_LINE &&
          fixture->kernel.set_format.size_image ==
              MEISO_CAMERA_V4L2_SIZE_IMAGE &&
          fixture->kernel.request_four_calls == 1U &&
          fixture->kernel.map_calls == 4U &&
          fixture->kernel.qbuf_calls == 4U &&
          fixture->kernel.stream_on_calls == 1U);
    CHECK(meiso_camera_v4l2_snapshot(fixture->adapter, &snapshot) &&
          snapshot.phase == MEISO_CAMERA_V4L2_PHASE_RUNNING &&
          snapshot.run_generation == 1U &&
          snapshot.source_clock_epoch == 77U &&
          snapshot.driver_buffer_count == 4U &&
          snapshot.mapped_buffer_count == 4U &&
          snapshot.queued_buffer_count == 4U && snapshot.stream_on &&
          !snapshot.frame_borrowed && snapshot.mapped_bytes ==
              UINT64_C(4) * MEISO_CAMERA_V4L2_SIZE_IMAGE &&
          snapshot.primary_failure_stage == MEISO_CAMERA_V4L2_FAILURE_NONE &&
          snapshot.primary_error == 0 &&
          snapshot.cleanup_failure_stage == MEISO_CAMERA_V4L2_FAILURE_NONE &&
          snapshot.cleanup_error == 0 &&
          snapshot.arena_bytes == fixture->arena_size);
    return true;
}

static bool test_init_start_stop_and_rollback(void)
{
    Fixture fixture;
    MeisoCameraV4l2Snapshot snapshot;
    unsigned calls;

    CHECK(fixture_init(&fixture));
    CHECK(start_and_check(&fixture));
    calls = fixture.kernel.open_calls;
    CHECK(meiso_camera_v4l2_start(fixture.adapter) ==
              MEISO_CAMERA_V4L2_START_UNCHANGED &&
          fixture.kernel.open_calls == calls);
    CHECK(meiso_camera_v4l2_stop(fixture.adapter) ==
          MEISO_CAMERA_V4L2_STOP_STOPPED);
    CHECK(fixture.kernel.stream_off_calls == 1U &&
          fixture.kernel.request_zero_calls == 1U &&
          fixture.kernel.close_calls == 1U &&
          fixture.kernel.unmap_calls[0] == 1U &&
          fixture.kernel.unmap_calls[1] == 1U &&
          fixture.kernel.unmap_calls[2] == 1U &&
          fixture.kernel.unmap_calls[3] == 1U &&
          fixture.kernel.cleanup_order_count == 7U &&
          fixture.kernel.cleanup_order[0] == CLEANUP_STREAM_OFF &&
          fixture.kernel.cleanup_order[1] == CLEANUP_UNMAP_BASE + 3U &&
          fixture.kernel.cleanup_order[2] == CLEANUP_UNMAP_BASE + 2U &&
          fixture.kernel.cleanup_order[3] == CLEANUP_UNMAP_BASE + 1U &&
          fixture.kernel.cleanup_order[4] == CLEANUP_UNMAP_BASE + 0U &&
          fixture.kernel.cleanup_order[5] == CLEANUP_REQUEST_ZERO &&
          fixture.kernel.cleanup_order[6] == CLEANUP_CLOSE);
    CHECK(meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.phase == MEISO_CAMERA_V4L2_PHASE_STOPPED &&
          snapshot.run_generation == 1U && !snapshot.stream_on &&
          snapshot.mapped_buffer_count == 0U &&
          snapshot.queued_buffer_count == 0U &&
          snapshot.mapped_bytes == 0U);
    CHECK(meiso_camera_v4l2_stop(fixture.adapter) ==
              MEISO_CAMERA_V4L2_STOP_UNCHANGED &&
          fixture.kernel.close_calls == 1U);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    fixture.kernel.returned_buffer_count = 3U;
    CHECK(meiso_camera_v4l2_start(fixture.adapter) ==
          MEISO_CAMERA_V4L2_START_INSUFFICIENT_BUFFERS);
    CHECK(meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.phase == MEISO_CAMERA_V4L2_PHASE_STOPPED &&
          snapshot.primary_failure_stage ==
              MEISO_CAMERA_V4L2_FAILURE_REQUEST_BUFFERS &&
          snapshot.primary_error == ENOBUFS &&
          snapshot.cleanup_failure_stage == MEISO_CAMERA_V4L2_FAILURE_NONE &&
          snapshot.cleanup_error == 0 &&
          fixture.kernel.request_zero_calls == 1U &&
          fixture.kernel.close_calls == 1U);
    fixture.kernel.returned_buffer_count = 4U;
    fake_clear_calls(&fixture.kernel);
    CHECK(start_and_check(&fixture));
    CHECK(meiso_camera_v4l2_stop(fixture.adapter) ==
          MEISO_CAMERA_V4L2_STOP_STOPPED);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    fixture.kernel.map_error_index = 2U;
    fixture.kernel.map_error = ENOMEM;
    CHECK(meiso_camera_v4l2_start(fixture.adapter) ==
          MEISO_CAMERA_V4L2_START_BUFFER_SETUP_FAILED);
    CHECK(fixture.kernel.unmap_calls[0] == 1U &&
          fixture.kernel.unmap_calls[1] == 1U &&
          fixture.kernel.unmap_order_count == 2U &&
          fixture.kernel.unmap_order[0] == 1U &&
          fixture.kernel.unmap_order[1] == 0U &&
          fixture.kernel.request_zero_calls == 1U &&
          fixture.kernel.close_calls == 1U &&
          meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.phase == MEISO_CAMERA_V4L2_PHASE_STOPPED &&
          snapshot.primary_failure_stage ==
              MEISO_CAMERA_V4L2_FAILURE_MAP_BUFFER &&
          snapshot.primary_error == ENOMEM &&
          snapshot.cleanup_failure_stage == MEISO_CAMERA_V4L2_FAILURE_NONE &&
          snapshot.cleanup_error == 0);
    fixture_destroy(&fixture);
    return true;
}

static bool run_start_failure_case(uint32_t variant)
{
    static const MeisoCameraV4l2StartStatus expected_status[8] = {
        MEISO_CAMERA_V4L2_START_OPEN_FAILED,
        MEISO_CAMERA_V4L2_START_DEVICE_UNSUPPORTED,
        MEISO_CAMERA_V4L2_START_FORMAT_UNSUPPORTED,
        MEISO_CAMERA_V4L2_START_FORMAT_UNSUPPORTED,
        MEISO_CAMERA_V4L2_START_BUFFER_SETUP_FAILED,
        MEISO_CAMERA_V4L2_START_BUFFER_SETUP_FAILED,
        MEISO_CAMERA_V4L2_START_BUFFER_SETUP_FAILED,
        MEISO_CAMERA_V4L2_START_STREAM_FAILED
    };
    static const MeisoCameraV4l2FailureStage expected_stage[8] = {
        MEISO_CAMERA_V4L2_FAILURE_OPEN,
        MEISO_CAMERA_V4L2_FAILURE_QUERY_CAPABILITY,
        MEISO_CAMERA_V4L2_FAILURE_SET_FORMAT,
        MEISO_CAMERA_V4L2_FAILURE_GET_FORMAT,
        MEISO_CAMERA_V4L2_FAILURE_REQUEST_BUFFERS,
        MEISO_CAMERA_V4L2_FAILURE_QUERY_BUFFER,
        MEISO_CAMERA_V4L2_FAILURE_QUEUE_BUFFER,
        MEISO_CAMERA_V4L2_FAILURE_STREAM_ON
    };
    static const int expected_error[8] = {
        EACCES, ENOTSUP, EIO, ENOTSUP, ENOMEM, EIO, EIO, EIO
    };
    Fixture fixture;
    MeisoCameraV4l2Snapshot snapshot;

    CHECK(variant < 8U && fixture_init(&fixture));
    switch (variant) {
        case 0U:
            fixture.kernel.open_error = EACCES;
            break;
        case 1U:
            fixture.kernel.streaming = false;
            break;
        case 2U:
            fixture.kernel.ioctl_error_request =
                MEISO_CAMERA_V4L2_IOCTL_SET_FORMAT;
            fixture.kernel.ioctl_error = EIO;
            break;
        case 3U:
            --fixture.kernel.format.width;
            break;
        case 4U:
            fixture.kernel.ioctl_error_request =
                MEISO_CAMERA_V4L2_IOCTL_REQUEST_BUFFERS;
            fixture.kernel.ioctl_error = ENOMEM;
            break;
        case 5U:
            fixture.kernel.ioctl_error_request =
                MEISO_CAMERA_V4L2_IOCTL_QUERY_BUFFER;
            fixture.kernel.ioctl_error_index = 2U;
            fixture.kernel.ioctl_error = EIO;
            break;
        case 6U:
            fixture.kernel.ioctl_error_request =
                MEISO_CAMERA_V4L2_IOCTL_QUEUE_BUFFER;
            fixture.kernel.ioctl_error_index = 2U;
            fixture.kernel.ioctl_error = EIO;
            break;
        default:
            fixture.kernel.ioctl_error_request =
                MEISO_CAMERA_V4L2_IOCTL_STREAM_ON;
            fixture.kernel.ioctl_error = EIO;
            break;
    }
    CHECK(meiso_camera_v4l2_start(fixture.adapter) ==
          expected_status[variant]);
    CHECK(meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.phase == MEISO_CAMERA_V4L2_PHASE_STOPPED &&
          snapshot.run_generation == 0U &&
          snapshot.primary_failure_stage == expected_stage[variant] &&
          snapshot.primary_error == expected_error[variant] &&
          snapshot.cleanup_failure_stage == MEISO_CAMERA_V4L2_FAILURE_NONE &&
          snapshot.cleanup_error == 0 &&
          !snapshot.stream_on && snapshot.mapped_buffer_count == 0U &&
          snapshot.queued_buffer_count == 0U &&
          fixture.kernel.close_calls == (variant == 0U ? 0U : 1U));
    fixture.kernel.open_error = 0;
    fixture.kernel.capture = true;
    fixture.kernel.streaming = true;
    fixture.kernel.format.width = MEISO_CAMERA_V4L2_WIDTH;
    fixture.kernel.ioctl_error = 0;
    fixture.kernel.ioctl_error_request = 0U;
    fixture.kernel.ioctl_error_index = FAKE_NO_INDEX;
    fake_clear_calls(&fixture.kernel);
    CHECK(start_and_check(&fixture));
    CHECK(meiso_camera_v4l2_stop(fixture.adapter) ==
          MEISO_CAMERA_V4L2_STOP_STOPPED);
    fixture_destroy(&fixture);
    return true;
}

static bool test_start_failure_matrix(void)
{
    uint32_t variant;

    for (variant = 0U; variant < 8U; ++variant) {
        CHECK(run_start_failure_case(variant));
    }
    return true;
}

static bool test_callback_reentry_is_fail_closed(void)
{
    Fixture fixture;

    CHECK(fixture_init(&fixture));
    fixture.kernel.reenter_on_open = true;
    CHECK(start_and_check(&fixture));
    CHECK(fixture.kernel.reentrant_start_status ==
              MEISO_CAMERA_V4L2_START_INVALID_ARGUMENT &&
          fixture.kernel.reentrant_snapshot_failed &&
          meiso_camera_v4l2_stop(fixture.adapter) ==
              MEISO_CAMERA_V4L2_STOP_STOPPED);
    fixture_destroy(&fixture);
    return true;
}

static bool test_poll_borrow_release_sequence_and_transient(void)
{
    Fixture fixture;
    MeisoCameraV4l2FrameView frame;
    MeisoCameraV4l2FrameView output;
    MeisoCameraV4l2FrameRef wrong;
    MeisoCameraV4l2Snapshot snapshot;
    uint8_t arena_before[TEST_ARENA_SNAPSHOT_CAPACITY];
    unsigned dq_calls;

    CHECK(fixture_init(&fixture) && start_and_check(&fixture));
    memset(fixture.kernel.storage, 0x5a, MEISO_CAMERA_V4L2_SIZE_IMAGE);
    fixture.kernel.dequeue.driver_sequence = UINT32_MAX;
    CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &frame) ==
          MEISO_CAMERA_V4L2_POLL_FRAME);
    CHECK(frame.ref.run_generation == 1U && frame.ref.borrow_serial == 1U &&
          frame.ref.buffer_index == 0U &&
          frame.ref.driver_sequence == UINT32_MAX &&
          frame.ref.sequence_provenance ==
              MEISO_CAMERA_V4L2_SEQUENCE_DRIVER &&
          frame.source_time.clock_epoch == 77U &&
          frame.source_time.monotonic_ns == UINT64_C(10000020000) &&
          frame.producer_dropped_frames == 0U &&
          frame.field_provenance ==
              MEISO_CAMERA_V4L2_FIELD_EXPLICIT_PROGRESSIVE &&
          frame.sequence_provenance ==
              MEISO_CAMERA_V4L2_SEQUENCE_DRIVER &&
          frame.timestamp_provenance ==
              MEISO_CAMERA_V4L2_TIMESTAMP_DRIVER_MONOTONIC &&
          frame.pixel_format == MEISO_COMPOSITION_MEDIA_PIXEL_NV12 &&
          frame.width == MEISO_CAMERA_V4L2_WIDTH &&
          frame.height == MEISO_CAMERA_V4L2_HEIGHT &&
          frame.bytes_per_line == MEISO_CAMERA_V4L2_BYTES_PER_LINE &&
          frame.size_image == MEISO_CAMERA_V4L2_SIZE_IMAGE &&
          frame.bytes.data == fixture.kernel.storage &&
          frame.bytes.size == MEISO_CAMERA_V4L2_SIZE_IMAGE &&
          frame.bytes.data[0] == UINT8_C(0x5a));
    dq_calls = fixture.kernel.ioctl_calls[
        MEISO_CAMERA_V4L2_IOCTL_DEQUEUE_BUFFER
    ];
    memset(&output, 0xa5, sizeof(output));
    {
        const MeisoCameraV4l2FrameView before = output;
        CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &output) ==
                  MEISO_CAMERA_V4L2_POLL_BORROW_ACTIVE &&
              memcmp(&before, &output, sizeof(before)) == 0 &&
              fixture.kernel.ioctl_calls[
                  MEISO_CAMERA_V4L2_IOCTL_DEQUEUE_BUFFER
              ] == dq_calls);
    }
    wrong = frame.ref;
    ++wrong.driver_sequence;
    CHECK(meiso_camera_v4l2_release_frame(fixture.adapter, &wrong) ==
          MEISO_CAMERA_V4L2_RELEASE_MISMATCH);
    CHECK(meiso_camera_v4l2_release_frame(fixture.adapter, &frame.ref) ==
          MEISO_CAMERA_V4L2_RELEASE_RELEASED);

    fixture.kernel.dequeue.index = 1U;
    fixture.kernel.dequeue.driver_sequence = 1U;
    fixture.kernel.dequeue.timestamp_seconds = 11;
    CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &frame) ==
              MEISO_CAMERA_V4L2_POLL_FRAME &&
          frame.producer_dropped_frames == 1U);
    CHECK(meiso_camera_v4l2_release_frame(fixture.adapter, &frame.ref) ==
          MEISO_CAMERA_V4L2_RELEASE_RELEASED);
    fixture.kernel.dequeue.index = 2U;
    fixture.kernel.dequeue.driver_sequence = 4U;
    fixture.kernel.dequeue.timestamp_seconds = 12;
    CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &frame) ==
              MEISO_CAMERA_V4L2_POLL_FRAME &&
          frame.producer_dropped_frames == 3U);
    CHECK(meiso_camera_v4l2_release_frame(fixture.adapter, &frame.ref) ==
          MEISO_CAMERA_V4L2_RELEASE_RELEASED);

    CHECK(fixture.arena_size <= sizeof(arena_before));
    memcpy(arena_before, fixture.arena, fixture.arena_size);
    fixture.kernel.dequeue_error = EAGAIN;
    memset(&output, 0xa5, sizeof(output));
    {
        const MeisoCameraV4l2FrameView before = output;
        CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &output) ==
                  MEISO_CAMERA_V4L2_POLL_WOULD_BLOCK &&
              memcmp(&before, &output, sizeof(before)) == 0 &&
              memcmp(arena_before, fixture.arena, fixture.arena_size) == 0);
    }
    fixture.kernel.dequeue_error = EINTR;
    memset(&output, 0xa5, sizeof(output));
    {
        const MeisoCameraV4l2FrameView before = output;
        CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &output) ==
                  MEISO_CAMERA_V4L2_POLL_INTERRUPTED &&
              memcmp(&before, &output, sizeof(before)) == 0 &&
              memcmp(arena_before, fixture.arena, fixture.arena_size) == 0);
    }
    CHECK(meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.producer_dropped_frames == 3U &&
          snapshot.last_driver_sequence == 4U &&
          snapshot.last_sequence == 4U &&
          snapshot.last_sequence_provenance ==
              MEISO_CAMERA_V4L2_SEQUENCE_DRIVER &&
          snapshot.last_timestamp_provenance ==
              MEISO_CAMERA_V4L2_TIMESTAMP_DRIVER_MONOTONIC &&
          snapshot.last_dequeue.timestamp_provenance ==
              MEISO_CAMERA_V4L2_TIMESTAMP_DRIVER_MONOTONIC &&
          snapshot.last_dequeue.timestamp_candidate_present &&
          !snapshot.frame_borrowed);
    CHECK(meiso_camera_v4l2_stop(fixture.adapter) ==
          MEISO_CAMERA_V4L2_STOP_STOPPED);
    fixture_destroy(&fixture);
    return true;
}

static bool cleanup_is_complete(const MeisoCameraV4l2Snapshot *snapshot)
{
    return snapshot != NULL && !snapshot->stream_on &&
        !snapshot->frame_borrowed && snapshot->driver_buffer_count == 0U &&
        snapshot->mapped_buffer_count == 0U &&
        snapshot->queued_buffer_count == 0U && snapshot->mapped_bytes == 0U;
}

static bool test_timestamp_provenance_and_zero_fallback(void)
{
    Fixture fixture;
    MeisoCameraV4l2FrameView frame;
    MeisoCameraV4l2Snapshot snapshot;

    CHECK(fixture_init(&fixture) && start_and_check(&fixture));
    fixture.kernel.reenter_on_monotonic = true;
    fixture.kernel.dequeue.timestamp_seconds = 0;
    fixture.kernel.dequeue.timestamp_microseconds = 0;
    fixture.kernel.dequeue.driver_sequence = 1U;
    CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &frame) ==
              MEISO_CAMERA_V4L2_POLL_FRAME &&
          frame.source_time.monotonic_ns == UINT64_C(30000000000) &&
          frame.timestamp_provenance ==
              MEISO_CAMERA_V4L2_TIMESTAMP_ADAPTER_DEQUEUE_MONOTONIC &&
          fixture.kernel.monotonic_calls == 1U &&
          fixture.kernel.monotonic_reentrant_snapshot_failed &&
          meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.last_timestamp_monotonic_ns ==
              UINT64_C(30000000000) &&
          snapshot.last_timestamp_provenance ==
              MEISO_CAMERA_V4L2_TIMESTAMP_ADAPTER_DEQUEUE_MONOTONIC &&
          snapshot.last_dequeue.timestamp_seconds == 0 &&
          snapshot.last_dequeue.timestamp_microseconds == 0 &&
          snapshot.last_dequeue.timestamp_monotonic &&
          snapshot.last_dequeue.timestamp_candidate_present &&
          snapshot.last_dequeue.timestamp_candidate_monotonic_ns ==
              UINT64_C(30000000000) &&
          snapshot.last_dequeue.timestamp_provenance ==
              MEISO_CAMERA_V4L2_TIMESTAMP_ADAPTER_DEQUEUE_MONOTONIC &&
          meiso_camera_v4l2_release_frame(fixture.adapter, &frame.ref) ==
              MEISO_CAMERA_V4L2_RELEASE_RELEASED);
    fixture.kernel.dequeue.driver_sequence = 2U;
    CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &frame) ==
              MEISO_CAMERA_V4L2_POLL_FRAME &&
          frame.source_time.monotonic_ns == UINT64_C(30000000001) &&
          frame.timestamp_provenance ==
              MEISO_CAMERA_V4L2_TIMESTAMP_ADAPTER_DEQUEUE_MONOTONIC &&
          fixture.kernel.monotonic_calls == 2U &&
          meiso_camera_v4l2_release_frame(fixture.adapter, &frame.ref) ==
              MEISO_CAMERA_V4L2_RELEASE_RELEASED);
    fixture.kernel.dequeue.driver_sequence = 3U;
    fixture.kernel.dequeue.timestamp_seconds = 31;
    fixture.kernel.dequeue.timestamp_microseconds = 0;
    CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &frame) ==
              MEISO_CAMERA_V4L2_POLL_FRAME &&
          frame.source_time.monotonic_ns == UINT64_C(31000000000) &&
          frame.timestamp_provenance ==
              MEISO_CAMERA_V4L2_TIMESTAMP_DRIVER_MONOTONIC &&
          fixture.kernel.monotonic_calls == 2U &&
          meiso_camera_v4l2_release_frame(fixture.adapter, &frame.ref) ==
              MEISO_CAMERA_V4L2_RELEASE_RELEASED);
    fixture.kernel.dequeue.driver_sequence = 4U;
    fixture.kernel.dequeue.timestamp_seconds = 0;
    fixture.kernel.monotonic_now_ns = UINT64_C(32000000000);
    CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &frame) ==
              MEISO_CAMERA_V4L2_POLL_FRAME &&
          frame.source_time.monotonic_ns == UINT64_C(32000000000) &&
          frame.timestamp_provenance ==
              MEISO_CAMERA_V4L2_TIMESTAMP_ADAPTER_DEQUEUE_MONOTONIC &&
          fixture.kernel.monotonic_calls == 3U &&
          meiso_camera_v4l2_release_frame(fixture.adapter, &frame.ref) ==
              MEISO_CAMERA_V4L2_RELEASE_RELEASED &&
          meiso_camera_v4l2_stop(fixture.adapter) ==
              MEISO_CAMERA_V4L2_STOP_STOPPED &&
          meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          cleanup_is_complete(&snapshot));
    fixture_destroy(&fixture);
    return true;
}

static bool run_nonzero_timestamp_violation(bool regression)
{
    Fixture fixture;
    MeisoCameraV4l2FrameView frame;
    MeisoCameraV4l2FrameView output;
    MeisoCameraV4l2Snapshot snapshot;

    CHECK(fixture_init(&fixture) && start_and_check(&fixture));
    CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &frame) ==
              MEISO_CAMERA_V4L2_POLL_FRAME &&
          frame.timestamp_provenance ==
              MEISO_CAMERA_V4L2_TIMESTAMP_DRIVER_MONOTONIC &&
          meiso_camera_v4l2_release_frame(fixture.adapter, &frame.ref) ==
              MEISO_CAMERA_V4L2_RELEASE_RELEASED);
    fixture.kernel.dequeue.driver_sequence = 2U;
    fixture.kernel.dequeue.timestamp_microseconds = regression ? 19 : 20;
    memset(&output, 0xa5, sizeof(output));
    {
        const MeisoCameraV4l2FrameView before = output;
        CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &output) ==
                  MEISO_CAMERA_V4L2_POLL_TERMINAL &&
              memcmp(&before, &output, sizeof(before)) == 0);
    }
    CHECK(fixture.kernel.monotonic_calls == 0U &&
          meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.phase == MEISO_CAMERA_V4L2_PHASE_TERMINAL &&
          snapshot.primary_failure_stage ==
              MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT &&
          snapshot.primary_error == EPROTO &&
          snapshot.last_dequeue.reject_reason ==
              MEISO_CAMERA_V4L2_FRAME_REJECT_EFFECTIVE_TIMESTAMP &&
          snapshot.last_dequeue.timestamp_candidate_present &&
          snapshot.last_dequeue.timestamp_provenance ==
              MEISO_CAMERA_V4L2_TIMESTAMP_DRIVER_MONOTONIC &&
          snapshot.last_timestamp_monotonic_ns == UINT64_C(10000020000) &&
          snapshot.last_timestamp_provenance ==
              MEISO_CAMERA_V4L2_TIMESTAMP_DRIVER_MONOTONIC &&
          cleanup_is_complete(&snapshot));
    fixture_destroy(&fixture);
    return true;
}

static bool run_fallback_failure(
    int clock_error,
    uint64_t clock_value,
    MeisoCameraV4l2FailureStage expected_stage,
    int expected_error,
    MeisoCameraV4l2FrameRejectReason expected_reject
)
{
    Fixture fixture;
    MeisoCameraV4l2FrameView frame;
    MeisoCameraV4l2FrameView output;
    MeisoCameraV4l2Snapshot snapshot;

    CHECK(fixture_init(&fixture) && start_and_check(&fixture));
    CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &frame) ==
              MEISO_CAMERA_V4L2_POLL_FRAME &&
          meiso_camera_v4l2_release_frame(fixture.adapter, &frame.ref) ==
              MEISO_CAMERA_V4L2_RELEASE_RELEASED);
    fixture.kernel.dequeue.driver_sequence = 2U;
    fixture.kernel.dequeue.timestamp_seconds = 0;
    fixture.kernel.dequeue.timestamp_microseconds = 0;
    fixture.kernel.monotonic_error = clock_error;
    fixture.kernel.monotonic_now_ns = clock_value;
    memset(&output, 0xa5, sizeof(output));
    {
        const MeisoCameraV4l2FrameView before = output;
        CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &output) ==
                  MEISO_CAMERA_V4L2_POLL_TERMINAL &&
              memcmp(&before, &output, sizeof(before)) == 0);
    }
    CHECK(fixture.kernel.monotonic_calls == 1U &&
          meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.phase == MEISO_CAMERA_V4L2_PHASE_TERMINAL &&
          snapshot.primary_failure_stage == expected_stage &&
          snapshot.primary_error == expected_error &&
          snapshot.last_dequeue.reject_reason == expected_reject &&
          snapshot.last_dequeue.timestamp_provenance ==
              MEISO_CAMERA_V4L2_TIMESTAMP_ADAPTER_DEQUEUE_MONOTONIC &&
          snapshot.last_dequeue.timestamp_candidate_present ==
              (clock_error == 0) &&
          snapshot.last_dequeue.timestamp_candidate_monotonic_ns ==
              (clock_error == 0 ? clock_value : 0U) &&
          snapshot.last_timestamp_monotonic_ns == UINT64_C(10000020000) &&
          snapshot.last_timestamp_provenance ==
              MEISO_CAMERA_V4L2_TIMESTAMP_DRIVER_MONOTONIC &&
          cleanup_is_complete(&snapshot));
    fixture_destroy(&fixture);
    return true;
}

static bool test_timestamp_terminal_matrix(void)
{
    Fixture fixture;
    MeisoCameraV4l2FrameView frame;
    MeisoCameraV4l2FrameView output;
    MeisoCameraV4l2Snapshot snapshot;
    uint32_t variant;

    CHECK(run_nonzero_timestamp_violation(false));
    CHECK(run_nonzero_timestamp_violation(true));
    CHECK(run_fallback_failure(
        EIO, UINT64_C(40000000000),
        MEISO_CAMERA_V4L2_FAILURE_TIMESTAMP_CLOCK, EIO,
        MEISO_CAMERA_V4L2_FRAME_REJECT_TIMESTAMP_CLOCK
    ));
    CHECK(run_fallback_failure(
        EOVERFLOW, UINT64_MAX,
        MEISO_CAMERA_V4L2_FAILURE_TIMESTAMP_CLOCK, EOVERFLOW,
        MEISO_CAMERA_V4L2_FRAME_REJECT_TIMESTAMP_CLOCK
    ));
    CHECK(run_fallback_failure(
        0, 0U, MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT, EPROTO,
        MEISO_CAMERA_V4L2_FRAME_REJECT_EFFECTIVE_TIMESTAMP
    ));
    CHECK(run_fallback_failure(
        0, UINT64_C(10000020000),
        MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT, EPROTO,
        MEISO_CAMERA_V4L2_FRAME_REJECT_EFFECTIVE_TIMESTAMP
    ));
    CHECK(run_fallback_failure(
        0, UINT64_C(10000019999),
        MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT, EPROTO,
        MEISO_CAMERA_V4L2_FRAME_REJECT_EFFECTIVE_TIMESTAMP
    ));
    for (variant = 0U; variant < 3U; ++variant) {
        CHECK(fixture_init(&fixture) && start_and_check(&fixture));
        fixture.kernel.dequeue.timestamp_seconds = 0;
        fixture.kernel.dequeue.timestamp_microseconds = 0;
        CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &frame) ==
                  MEISO_CAMERA_V4L2_POLL_FRAME &&
              frame.timestamp_provenance ==
                  MEISO_CAMERA_V4L2_TIMESTAMP_ADAPTER_DEQUEUE_MONOTONIC &&
              meiso_camera_v4l2_release_frame(fixture.adapter, &frame.ref) ==
                  MEISO_CAMERA_V4L2_RELEASE_RELEASED);
        fixture.kernel.dequeue.driver_sequence = 2U;
        if (variant == 0U) {
            fixture.kernel.dequeue.timestamp_seconds = 30;
            fixture.kernel.monotonic_now_ns = UINT64_C(90000000000);
        } else {
            fixture.kernel.monotonic_now_ns = variant == 1U ?
                UINT64_C(30000000000) : UINT64_C(29999999999);
        }
        memset(&output, 0xa5, sizeof(output));
        {
            const MeisoCameraV4l2FrameView before = output;
            CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &output) ==
                      MEISO_CAMERA_V4L2_POLL_TERMINAL &&
                  memcmp(&before, &output, sizeof(before)) == 0);
        }
        CHECK(fixture.kernel.monotonic_calls == (variant == 0U ? 1U : 2U) &&
              meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
              snapshot.primary_failure_stage ==
                  MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT &&
              snapshot.last_dequeue.reject_reason ==
                  MEISO_CAMERA_V4L2_FRAME_REJECT_EFFECTIVE_TIMESTAMP &&
              snapshot.last_dequeue.timestamp_candidate_present &&
              snapshot.last_dequeue.timestamp_provenance ==
                  (variant == 0U ?
                       MEISO_CAMERA_V4L2_TIMESTAMP_DRIVER_MONOTONIC :
                       MEISO_CAMERA_V4L2_TIMESTAMP_ADAPTER_DEQUEUE_MONOTONIC) &&
              snapshot.last_timestamp_monotonic_ns ==
                  UINT64_C(30000000000) &&
              snapshot.last_timestamp_provenance ==
                  MEISO_CAMERA_V4L2_TIMESTAMP_ADAPTER_DEQUEUE_MONOTONIC &&
              cleanup_is_complete(&snapshot));
        fixture_destroy(&fixture);
    }
    return true;
}

static bool test_contract_failure_terminal_and_cleanup_retry(void)
{
    Fixture fixture;
    MeisoCameraV4l2FrameView output;
    MeisoCameraV4l2Snapshot snapshot;

    CHECK(fixture_init(&fixture) && start_and_check(&fixture));
    fixture.kernel.dequeue.bytes_used = MEISO_CAMERA_V4L2_SIZE_IMAGE - 1U;
    memset(&output, 0xa5, sizeof(output));
    {
        const MeisoCameraV4l2FrameView before = output;
        CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &output) ==
                  MEISO_CAMERA_V4L2_POLL_TERMINAL &&
              memcmp(&before, &output, sizeof(before)) == 0);
    }
    CHECK(meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.phase == MEISO_CAMERA_V4L2_PHASE_TERMINAL &&
          snapshot.primary_failure_stage ==
              MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT &&
          snapshot.primary_error == EPROTO &&
          snapshot.cleanup_failure_stage == MEISO_CAMERA_V4L2_FAILURE_NONE &&
          snapshot.cleanup_error == 0 &&
          snapshot.last_dequeue.present &&
          snapshot.last_dequeue.bytes_used ==
              MEISO_CAMERA_V4L2_SIZE_IMAGE - 1U &&
          snapshot.last_dequeue.reject_reason ==
              MEISO_CAMERA_V4L2_FRAME_REJECT_BYTES_USED &&
          !snapshot.stream_on && !snapshot.frame_borrowed &&
          fixture.kernel.stream_off_calls == 1U &&
          fixture.kernel.request_zero_calls == 1U &&
          fixture.kernel.close_calls == 1U);
    CHECK(meiso_camera_v4l2_start(fixture.adapter) ==
              MEISO_CAMERA_V4L2_START_TERMINAL &&
          meiso_camera_v4l2_poll_frame(fixture.adapter, &output) ==
              MEISO_CAMERA_V4L2_POLL_TERMINAL &&
          meiso_camera_v4l2_release_frame(
              fixture.adapter, &snapshot.borrowed_frame
          ) == MEISO_CAMERA_V4L2_RELEASE_TERMINAL &&
          meiso_camera_v4l2_stop(fixture.adapter) ==
              MEISO_CAMERA_V4L2_STOP_TERMINAL &&
          fixture.kernel.close_calls == 1U);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture) && start_and_check(&fixture));
    fixture.kernel.dequeue.bytes_used =
        MEISO_CAMERA_V4L2_SIZE_IMAGE - 1U;
    fixture.kernel.ioctl_error_request =
        MEISO_CAMERA_V4L2_IOCTL_STREAM_OFF;
    fixture.kernel.ioctl_error = EBUSY;
    CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &output) ==
          MEISO_CAMERA_V4L2_POLL_TERMINAL);
    CHECK(meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.primary_failure_stage ==
              MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT &&
          snapshot.primary_error == EPROTO &&
          snapshot.cleanup_failure_stage ==
              MEISO_CAMERA_V4L2_FAILURE_STREAM_OFF &&
          snapshot.cleanup_error == EBUSY &&
          snapshot.last_dequeue.reject_reason ==
              MEISO_CAMERA_V4L2_FRAME_REJECT_BYTES_USED &&
          !snapshot.stream_on && snapshot.mapped_buffer_count == 0U);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture) && start_and_check(&fixture));
    fixture.kernel.unmap_error_index = 2U;
    fixture.kernel.unmap_error = EIO;
    fixture.kernel.unmap_error_once = true;
    CHECK(meiso_camera_v4l2_stop(fixture.adapter) ==
          MEISO_CAMERA_V4L2_STOP_TERMINAL);
    CHECK(fixture.kernel.unmap_calls[2] == 1U &&
          fixture.kernel.close_calls == 1U &&
          meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.phase == MEISO_CAMERA_V4L2_PHASE_TERMINAL &&
          snapshot.mapped_buffer_count == 1U &&
          snapshot.primary_failure_stage == MEISO_CAMERA_V4L2_FAILURE_NONE &&
          snapshot.primary_error == 0 &&
          snapshot.cleanup_failure_stage ==
              MEISO_CAMERA_V4L2_FAILURE_UNMAP_BUFFER &&
          snapshot.cleanup_error == EIO);
    CHECK(meiso_camera_v4l2_stop(fixture.adapter) ==
              MEISO_CAMERA_V4L2_STOP_TERMINAL &&
          fixture.kernel.unmap_calls[2] == 2U &&
          fixture.kernel.close_calls == 1U &&
          meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.mapped_buffer_count == 0U);
    fixture_destroy(&fixture);
    return true;
}

static bool test_cleanup_failure_stages_and_close_once(void)
{
    Fixture fixture;
    MeisoCameraV4l2Snapshot snapshot;
    uint32_t variant;

    for (variant = 0U; variant < 3U; ++variant) {
        CHECK(fixture_init(&fixture) && start_and_check(&fixture));
        if (variant == 0U) {
            fixture.kernel.ioctl_error_request =
                MEISO_CAMERA_V4L2_IOCTL_STREAM_OFF;
            fixture.kernel.ioctl_error = EIO;
        } else if (variant == 1U) {
            fixture.kernel.release_buffers_error = ENOMEM;
        } else {
            fixture.kernel.close_error = EIO;
        }
        CHECK(meiso_camera_v4l2_stop(fixture.adapter) ==
              MEISO_CAMERA_V4L2_STOP_TERMINAL);
        CHECK(meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
              snapshot.phase == MEISO_CAMERA_V4L2_PHASE_TERMINAL &&
              snapshot.primary_failure_stage ==
                  MEISO_CAMERA_V4L2_FAILURE_NONE &&
              snapshot.primary_error == 0 &&
              snapshot.cleanup_failure_stage == (variant == 0U ?
                  MEISO_CAMERA_V4L2_FAILURE_STREAM_OFF :
                  (variant == 1U ?
                      MEISO_CAMERA_V4L2_FAILURE_RELEASE_BUFFERS :
                      MEISO_CAMERA_V4L2_FAILURE_CLOSE)) &&
              snapshot.cleanup_error == (variant == 1U ? ENOMEM : EIO) &&
              fixture.kernel.close_calls == 1U);
        if (variant == 0U) {
            CHECK(fixture.kernel.stream_off_calls == 0U &&
                  fixture.kernel.request_zero_calls == 0U &&
                  fixture.kernel.cleanup_order_count == 6U &&
                  fixture.kernel.cleanup_order[0] == CLEANUP_STREAM_OFF &&
                  fixture.kernel.cleanup_order[1] == CLEANUP_CLOSE &&
                  fixture.kernel.cleanup_order[2] ==
                      CLEANUP_UNMAP_BASE + 3U &&
                  fixture.kernel.cleanup_order[3] ==
                      CLEANUP_UNMAP_BASE + 2U &&
                  fixture.kernel.cleanup_order[4] ==
                      CLEANUP_UNMAP_BASE + 1U &&
                  fixture.kernel.cleanup_order[5] ==
                      CLEANUP_UNMAP_BASE + 0U);
        }
        CHECK(meiso_camera_v4l2_stop(fixture.adapter) ==
                  MEISO_CAMERA_V4L2_STOP_TERMINAL &&
                  fixture.kernel.close_calls == 1U &&
              fixture.kernel.request_zero_calls ==
                  (variant == 0U ? 0U : 1U));
        fixture_destroy(&fixture);
    }
    return true;
}

static bool test_duplicate_mapping_and_dequeue_error(void)
{
    Fixture fixture;
    MeisoCameraV4l2Snapshot snapshot;
    MeisoCameraV4l2FrameView output;

    CHECK(fixture_init(&fixture));
    fixture.kernel.duplicate_mapping = true;
    CHECK(meiso_camera_v4l2_start(fixture.adapter) ==
          MEISO_CAMERA_V4L2_START_BUFFER_SETUP_FAILED);
    CHECK(meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.phase == MEISO_CAMERA_V4L2_PHASE_STOPPED &&
          snapshot.primary_failure_stage ==
              MEISO_CAMERA_V4L2_FAILURE_MAP_BUFFER &&
          snapshot.primary_error == EINVAL &&
          snapshot.cleanup_failure_stage == MEISO_CAMERA_V4L2_FAILURE_NONE &&
          snapshot.cleanup_error == 0 &&
          snapshot.mapped_buffer_count == 0U &&
          fixture.kernel.unmap_order_count == 2U &&
          fixture.kernel.unmap_order[0] == 0U &&
          fixture.kernel.unmap_order[1] == 0U &&
          fixture.kernel.unmap_calls[0] == 2U &&
          fixture.kernel.unmap_calls[1] == 0U &&
          fixture.kernel.close_calls == 1U);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture));
    fixture.kernel.mapping_override_index = 1U;
    fixture.kernel.mapping_override = fixture.arena;
    CHECK(meiso_camera_v4l2_start(fixture.adapter) ==
          MEISO_CAMERA_V4L2_START_BUFFER_SETUP_FAILED);
    CHECK(meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.phase == MEISO_CAMERA_V4L2_PHASE_STOPPED &&
          snapshot.primary_failure_stage ==
              MEISO_CAMERA_V4L2_FAILURE_MAP_BUFFER &&
          snapshot.primary_error == EINVAL &&
          snapshot.cleanup_failure_stage == MEISO_CAMERA_V4L2_FAILURE_NONE &&
          snapshot.cleanup_error == 0 &&
          snapshot.mapped_buffer_count == 0U &&
          fixture.kernel.unmap_order_count == 2U &&
          fixture.kernel.unmap_order[0] == 1U &&
          fixture.kernel.unmap_order[1] == 0U &&
          fixture.kernel.unmap_calls[1] == 1U &&
          fixture.kernel.unmap_calls[0] == 1U &&
          fixture.kernel.close_calls == 1U);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture) && start_and_check(&fixture));
    fixture.kernel.dequeue_error = EIO;
    memset(&output, 0xa5, sizeof(output));
    {
        const MeisoCameraV4l2FrameView before = output;
        CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &output) ==
                  MEISO_CAMERA_V4L2_POLL_TERMINAL &&
              memcmp(&before, &output, sizeof(before)) == 0);
    }
    CHECK(meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.phase == MEISO_CAMERA_V4L2_PHASE_TERMINAL &&
          snapshot.primary_failure_stage ==
              MEISO_CAMERA_V4L2_FAILURE_DEQUEUE_BUFFER &&
          snapshot.primary_error == EIO &&
          snapshot.cleanup_failure_stage == MEISO_CAMERA_V4L2_FAILURE_NONE &&
          snapshot.cleanup_error == 0 && fixture.kernel.close_calls == 1U);
    fixture_destroy(&fixture);
    return true;
}

static bool run_bad_dequeue_case(uint32_t variant)
{
    Fixture fixture;
    MeisoCameraV4l2FrameView frame;
    MeisoCameraV4l2FrameView output;
    MeisoCameraV4l2Snapshot snapshot;

    CHECK(fixture_init(&fixture) && start_and_check(&fixture));
    if (variant >= 6U) {
        fixture.kernel.dequeue.driver_sequence = 20U;
        CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &frame) ==
                  MEISO_CAMERA_V4L2_POLL_FRAME &&
              meiso_camera_v4l2_release_frame(
                  fixture.adapter, &frame.ref
              ) == MEISO_CAMERA_V4L2_RELEASE_RELEASED);
        fixture.kernel.dequeue.timestamp_seconds = 11;
    }
    switch (variant) {
        case 0U:
            fixture.kernel.dequeue.index =
                MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT;
            break;
        case 1U:
            fixture.kernel.dequeue.bytes_used =
                MEISO_CAMERA_V4L2_SIZE_IMAGE - 1U;
            break;
        case 2U:
            fixture.kernel.dequeue.timestamp_monotonic = false;
            break;
        case 3U:
            fixture.kernel.dequeue.timestamp_microseconds = -1;
            break;
        case 4U:
            fixture.kernel.dequeue.timestamp_microseconds = 1000000;
            break;
        case 5U:
            fixture.kernel.dequeue.frame_error = true;
            break;
        case 6U:
            fixture.kernel.dequeue.driver_sequence = 20U;
            break;
        case 7U:
            fixture.kernel.dequeue.driver_sequence = 19U;
            break;
        case 8U:
            fixture.kernel.dequeue.driver_sequence = 21U;
            fixture.kernel.dequeue.timestamp_seconds = 10;
            fixture.kernel.dequeue.timestamp_microseconds = 19;
            break;
        default:
            fixture.kernel.dequeue.driver_sequence = 21U;
            fixture.kernel.dequeue.timestamp_seconds = INT64_MAX;
            fixture.kernel.dequeue.timestamp_microseconds = 999999;
            break;
    }
    memset(&output, 0xa5, sizeof(output));
    {
        const MeisoCameraV4l2FrameView before = output;
        CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &output) ==
                  MEISO_CAMERA_V4L2_POLL_TERMINAL &&
              memcmp(&before, &output, sizeof(before)) == 0);
    }
    CHECK(meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.phase == MEISO_CAMERA_V4L2_PHASE_TERMINAL &&
          snapshot.primary_failure_stage ==
              MEISO_CAMERA_V4L2_FAILURE_FRAME_CONTRACT &&
          snapshot.primary_error == EPROTO &&
          snapshot.cleanup_failure_stage == MEISO_CAMERA_V4L2_FAILURE_NONE &&
          snapshot.cleanup_error == 0 &&
          !snapshot.frame_borrowed && !snapshot.stream_on &&
          fixture.kernel.close_calls == 1U);
    fixture_destroy(&fixture);
    return true;
}

static bool test_poll_contract_matrix(void)
{
    uint32_t variant;

    for (variant = 0U; variant < 10U; ++variant) {
        CHECK(run_bad_dequeue_case(variant));
    }
    return true;
}

static bool test_mapping_alias_borrow_stop_and_release_failure(void)
{
    Fixture fixture;
    MeisoCameraV4l2FrameView frame;
    MeisoCameraV4l2Snapshot snapshot;
    MeisoCameraV4l2FrameRef retained_ref;
    uint8_t mapping_before[sizeof(MeisoCameraV4l2FrameView)];
    uint8_t arena_before[TEST_ARENA_SNAPSHOT_CAPACITY];
    unsigned dequeue_calls;

    CHECK(fixture_init(&fixture) && start_and_check(&fixture));
    memset(fixture.kernel.storage, 0x3c, fixture.kernel.storage_size);
    memcpy(mapping_before, fixture.kernel.storage, sizeof(mapping_before));
    CHECK(fixture.arena_size <= sizeof(arena_before));
    memcpy(arena_before, fixture.arena, fixture.arena_size);
    dequeue_calls = fixture.kernel.ioctl_calls[
        MEISO_CAMERA_V4L2_IOCTL_DEQUEUE_BUFFER
    ];
    CHECK(meiso_camera_v4l2_poll_frame(
        fixture.adapter,
        (MeisoCameraV4l2FrameView *)(void *)fixture.kernel.storage
    ) == MEISO_CAMERA_V4L2_POLL_INVALID_ARGUMENT &&
          !meiso_camera_v4l2_snapshot(
              fixture.adapter,
              (MeisoCameraV4l2Snapshot *)(void *)fixture.kernel.storage
          ) &&
          memcmp(mapping_before, fixture.kernel.storage,
                 sizeof(mapping_before)) == 0 &&
          memcmp(arena_before, fixture.arena, fixture.arena_size) == 0 &&
          fixture.kernel.ioctl_calls[
              MEISO_CAMERA_V4L2_IOCTL_DEQUEUE_BUFFER
          ] == dequeue_calls);
    CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &frame) ==
          MEISO_CAMERA_V4L2_POLL_FRAME);
    retained_ref = frame.ref;
    memcpy(arena_before, fixture.arena, fixture.arena_size);
    CHECK(meiso_camera_v4l2_release_frame(
        fixture.adapter,
        (const MeisoCameraV4l2FrameRef *)(const void *)
            fixture.kernel.storage
    ) == MEISO_CAMERA_V4L2_RELEASE_INVALID_ARGUMENT &&
          memcmp(arena_before, fixture.arena, fixture.arena_size) == 0);
    CHECK(meiso_camera_v4l2_stop(fixture.adapter) ==
              MEISO_CAMERA_V4L2_STOP_STOPPED &&
          meiso_camera_v4l2_release_frame(
              fixture.adapter, &retained_ref
          ) == MEISO_CAMERA_V4L2_RELEASE_NO_FRAME &&
          meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.phase == MEISO_CAMERA_V4L2_PHASE_STOPPED &&
          !snapshot.frame_borrowed);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture) && start_and_check(&fixture));
    CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &frame) ==
          MEISO_CAMERA_V4L2_POLL_FRAME);
    fixture.kernel.ioctl_error_request =
        MEISO_CAMERA_V4L2_IOCTL_QUEUE_BUFFER;
    fixture.kernel.ioctl_error_index = frame.ref.buffer_index;
    fixture.kernel.ioctl_error = EIO;
    CHECK(meiso_camera_v4l2_release_frame(fixture.adapter, &frame.ref) ==
          MEISO_CAMERA_V4L2_RELEASE_TERMINAL);
    CHECK(meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.phase == MEISO_CAMERA_V4L2_PHASE_TERMINAL &&
          snapshot.primary_failure_stage ==
              MEISO_CAMERA_V4L2_FAILURE_QUEUE_BUFFER &&
          snapshot.primary_error == EIO &&
          snapshot.cleanup_failure_stage == MEISO_CAMERA_V4L2_FAILURE_NONE &&
          snapshot.cleanup_error == 0 &&
          !snapshot.frame_borrowed && fixture.kernel.close_calls == 1U);
    fixture_destroy(&fixture);
    return true;
}

static bool check_init_failure(
    const MeisoCameraV4l2Config *config,
    size_t arena_size,
    size_t offset,
    MeisoCameraV4l2Adapter **out_adapter,
    MeisoCameraV4l2InitStatus expected
)
{
    uint8_t *storage = (uint8_t *)malloc(arena_size + offset);
    uint8_t *before = (uint8_t *)malloc(arena_size + offset);
    bool okay;

    if (storage == NULL || before == NULL) {
        free(before);
        free(storage);
        return false;
    }
    memset(storage, 0xa5, arena_size + offset);
    memcpy(before, storage, arena_size + offset);
    okay = meiso_camera_v4l2_init(
        storage + offset, arena_size, config, out_adapter
    ) == expected &&
        memcmp(storage, before, arena_size + offset) == 0;
    free(before);
    free(storage);
    return okay;
}

static bool run_init_alias_case(Fixture *fixture, uint32_t variant)
{
    const size_t required = meiso_camera_v4l2_memory_required();
    MeisoCameraV4l2Config config = fixture->config;
    MeisoCameraV4l2Config config_before;
    const MeisoCameraV4l2Config *config_input = &config;
    MeisoCameraV4l2Adapter *sentinel =
        (MeisoCameraV4l2Adapter *)(uintptr_t)UINTPTR_MAX;
    MeisoCameraV4l2Adapter **output = &sentinel;
    uint8_t *arena = (uint8_t *)malloc(required);
    uint8_t *before = (uint8_t *)malloc(required);
    bool okay;

    if (arena == NULL || before == NULL || variant >= 6U) {
        free(before);
        free(arena);
        return false;
    }
    memset(arena, 0x5a, required);
    if (variant == 0U) {
        memcpy(arena, &config, sizeof(config));
        config_input = (const MeisoCameraV4l2Config *)(const void *)arena;
    } else if (variant == 1U) {
        memcpy(arena, fixture->path, strlen(fixture->path) + 1U);
        config.device_path = (const char *)(const void *)arena;
    } else if (variant == 2U) {
        memcpy(arena, &fixture->syscalls, sizeof(fixture->syscalls));
        config.syscalls =
            (const MeisoCameraV4l2Syscalls *)(const void *)arena;
    } else if (variant == 3U) {
        config.syscall_context = arena + required / 2U;
    } else if (variant == 4U) {
        output = (MeisoCameraV4l2Adapter **)(void *)arena;
    } else {
        output = (MeisoCameraV4l2Adapter **)(void *)&config;
    }
    config_before = config;
    memcpy(before, arena, required);
    okay = meiso_camera_v4l2_init(
        arena, required, config_input, output
    ) == MEISO_CAMERA_V4L2_INIT_INVALID_ARGUMENT &&
        memcmp(before, arena, required) == 0 &&
        memcmp(&config_before, &config, sizeof(config)) == 0 &&
        (output != &sentinel || sentinel ==
            (MeisoCameraV4l2Adapter *)(uintptr_t)UINTPTR_MAX);
    free(before);
    free(arena);
    return okay;
}

static bool test_init_alias_bounds_and_public_aliases(void)
{
    Fixture fixture;
    MeisoCameraV4l2Config config;
    MeisoCameraV4l2Config default_config;
    MeisoCameraV4l2Adapter *sentinel =
        (MeisoCameraV4l2Adapter *)(uintptr_t)UINTPTR_MAX;
    MeisoCameraV4l2Snapshot snapshot;
    MeisoCameraV4l2FrameView frame;
    MeisoCameraV4l2Syscalls incomplete_syscalls;
    size_t required;
    size_t alignment;
    char unterminated[MEISO_CAMERA_V4L2_DEVICE_PATH_CAPACITY];
    uint8_t arena_before[TEST_ARENA_SNAPSHOT_CAPACITY];

    meiso_camera_v4l2_default_config(99U, &default_config);
    CHECK(default_config.device_path != NULL &&
          strcmp(default_config.device_path, "/dev/video1") == 0 &&
          default_config.source_clock_epoch == 99U &&
          default_config.syscalls == NULL &&
          default_config.syscall_context == NULL);
    CHECK(fixture_init(&fixture));
    required = meiso_camera_v4l2_memory_required();
    alignment = meiso_camera_v4l2_alignment_required();
    CHECK(required == fixture.arena_size && alignment != 0U &&
          check_init_failure(
              &fixture.config, required - 1U, 0U, &sentinel,
              MEISO_CAMERA_V4L2_INIT_ARENA_TOO_SMALL
          ) && sentinel ==
              (MeisoCameraV4l2Adapter *)(uintptr_t)UINTPTR_MAX &&
          check_init_failure(
              &fixture.config, required, 1U, &sentinel,
              MEISO_CAMERA_V4L2_INIT_MISALIGNED_ARENA
          ));
    memset(unterminated, 'x', sizeof(unterminated));
    config = fixture.config;
    config.device_path = unterminated;
    CHECK(check_init_failure(
        &config, required, 0U, &sentinel,
        MEISO_CAMERA_V4L2_INIT_INVALID_CONFIG
    ));
    config = fixture.config;
    config.source_clock_epoch = 0U;
    CHECK(check_init_failure(
        &config, required, 0U, &sentinel,
        MEISO_CAMERA_V4L2_INIT_INVALID_CONFIG
    ));
    incomplete_syscalls = fixture.syscalls;
    incomplete_syscalls.monotonic_now = NULL;
    config = fixture.config;
    config.syscalls = &incomplete_syscalls;
    CHECK(check_init_failure(
        &config, required, 0U, &sentinel,
        MEISO_CAMERA_V4L2_INIT_INVALID_CONFIG
    ));
    {
        uint32_t variant;
        for (variant = 0U; variant < 6U; ++variant) {
            CHECK(run_init_alias_case(&fixture, variant));
        }
    }

    CHECK(start_and_check(&fixture));
    CHECK(fixture.arena_size <= sizeof(arena_before));
    memcpy(arena_before, fixture.arena, fixture.arena_size);
    memset(&frame, 0xa5, sizeof(frame));
    CHECK(meiso_camera_v4l2_poll_frame(
        fixture.adapter, (MeisoCameraV4l2FrameView *)fixture.arena
    ) == MEISO_CAMERA_V4L2_POLL_INVALID_ARGUMENT);
    CHECK(!meiso_camera_v4l2_snapshot(
        fixture.adapter, (MeisoCameraV4l2Snapshot *)fixture.arena
    ) && memcmp(arena_before, fixture.arena, fixture.arena_size) == 0);
    memset(&snapshot, 0xa5, sizeof(snapshot));
    CHECK(meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot));
    CHECK(meiso_camera_v4l2_stop(fixture.adapter) ==
          MEISO_CAMERA_V4L2_STOP_STOPPED);
    fixture_destroy(&fixture);
    return true;
}

static bool test_vvcam_inherited_field_and_synthetic_sequence(void)
{
    Fixture fixture;
    MeisoCameraV4l2FrameView frame;
    MeisoCameraV4l2Snapshot snapshot;
    uint32_t ordinal;

    CHECK(fixture_init(&fixture) && start_and_check(&fixture));
    fixture.kernel.dequeue.raw_field = MEISO_CAMERA_V4L2_RAW_FIELD_ANY;
    fixture.kernel.dequeue.field_provenance =
        MEISO_CAMERA_V4L2_FIELD_FORMAT_INHERITED_PROGRESSIVE;
    fixture.kernel.dequeue.driver_sequence = 0U;
    fixture.kernel.dequeue.driver_sequence_present = false;
    for (ordinal = 1U; ordinal <= 5U; ++ordinal) {
        fixture.kernel.dequeue.index = (ordinal - 1U) %
            MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT;
        fixture.kernel.dequeue.timestamp_seconds = (int64_t)(20U + ordinal);
        CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &frame) ==
                  MEISO_CAMERA_V4L2_POLL_FRAME &&
              frame.ref.driver_sequence == ordinal &&
              frame.ref.sequence_provenance ==
                  MEISO_CAMERA_V4L2_SEQUENCE_ADAPTER_SYNTHETIC &&
              frame.field_provenance ==
                  MEISO_CAMERA_V4L2_FIELD_FORMAT_INHERITED_PROGRESSIVE &&
              frame.sequence_provenance ==
                  MEISO_CAMERA_V4L2_SEQUENCE_ADAPTER_SYNTHETIC &&
              frame.producer_dropped_frames == 0U &&
              meiso_camera_v4l2_release_frame(
                  fixture.adapter, &frame.ref
              ) == MEISO_CAMERA_V4L2_RELEASE_RELEASED);
    }
    CHECK(meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.last_sequence == 5U &&
          snapshot.last_sequence_provenance ==
              MEISO_CAMERA_V4L2_SEQUENCE_ADAPTER_SYNTHETIC &&
          !snapshot.driver_sequence_present &&
          snapshot.last_driver_sequence == 0U &&
          snapshot.producer_dropped_frames == 0U &&
          snapshot.last_dequeue.present &&
          snapshot.last_dequeue.raw_field ==
              MEISO_CAMERA_V4L2_RAW_FIELD_ANY &&
          snapshot.last_dequeue.field_provenance ==
              MEISO_CAMERA_V4L2_FIELD_FORMAT_INHERITED_PROGRESSIVE &&
          !snapshot.last_dequeue.driver_sequence_present &&
          snapshot.last_dequeue.reject_reason ==
              MEISO_CAMERA_V4L2_FRAME_REJECT_NONE);
    CHECK(meiso_camera_v4l2_stop(fixture.adapter) ==
          MEISO_CAMERA_V4L2_STOP_STOPPED);
    fixture_destroy(&fixture);

    CHECK(fixture_init(&fixture) && start_and_check(&fixture));
    fixture.kernel.dequeue.raw_field = 4U;
    fixture.kernel.dequeue.field_provenance =
        MEISO_CAMERA_V4L2_FIELD_UNKNOWN;
    CHECK(meiso_camera_v4l2_poll_frame(fixture.adapter, &frame) ==
          MEISO_CAMERA_V4L2_POLL_TERMINAL);
    CHECK(meiso_camera_v4l2_snapshot(fixture.adapter, &snapshot) &&
          snapshot.phase == MEISO_CAMERA_V4L2_PHASE_TERMINAL &&
          snapshot.last_dequeue.present &&
          snapshot.last_dequeue.raw_field == 4U &&
          snapshot.last_dequeue.reject_reason ==
              MEISO_CAMERA_V4L2_FRAME_REJECT_FIELD &&
          !snapshot.stream_on && !snapshot.frame_borrowed);
    fixture_destroy(&fixture);
    return true;
}

int main(void)
{
    if (!test_init_start_stop_and_rollback() ||
        !test_start_failure_matrix() ||
        !test_callback_reentry_is_fail_closed() ||
        !test_poll_borrow_release_sequence_and_transient() ||
        !test_timestamp_provenance_and_zero_fallback() ||
        !test_timestamp_terminal_matrix() ||
        !test_contract_failure_terminal_and_cleanup_retry() ||
        !test_cleanup_failure_stages_and_close_once() ||
        !test_duplicate_mapping_and_dequeue_error() ||
        !test_poll_contract_matrix() ||
        !test_mapping_alias_borrow_stop_and_release_failure() ||
        !test_init_alias_bounds_and_public_aliases() ||
        !test_vvcam_inherited_field_and_synthetic_sequence()) {
        return 1;
    }
    (void)puts("camera v4l2 adapter tests passed");
    return 0;
}
